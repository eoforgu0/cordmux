// SPDX-License-Identifier: Apache-2.0

// 画面のストリーム 1 本の、届くまでの遅れ (表示の緩衝の待ち行列に積むときの age = 受信側の OBS の時計 - 送信側の
// タイムスタンプを OBS の時計に換算した値) の分布 (ArrivalDelayStats)。利用者が表示の緩衝 D を、間に合わないフレームの率を見て選べるようにする土台で、
// 「D を X にしたら何 % のフレームが間に合わないか」が、分布の分位点そのものとして求まる。
//
// 分布は 1 ms 刻みの度数で持つ。D の上限 (1000 ms) 以上の遅れは区別しても D の選択に反映されないので、最後の度数 1 つに
// まとめる。直近の範囲 (kArrivalWindowSeconds) は、1 秒ごとの度数を輪に持ち、合計の度数から期限が切れた秒を引く
// ことで保つ。足すときの負荷は度数 2 つの加算で、分位点の計算は取り出すときだけ行う。
//
// 設定ウィンドウの表のために、直近の範囲の遅れを届いた順の列としても持つ (window)。度数とは別に持つのは、
// 任意の X で続けて遅れたまとまりの数を正確に出すには順序が要るからで、度数を列に置き換えると、ログの分位点 (1 ms 刻みの
// 上端) の値が変わる。列は kArrivalFrameCapacity 個までの輪で、範囲を出たフレームは時刻で外す。読み出しは周期の数
// (take_period) に触れない。
//
// 間に合わなかったフレームは FramePacer と同じ約束で数える (遅れ >= D)。続けて間に合わなかったフレームは 1 つのまとまりと
// 数える。見た目のカクつきは、続けて間に合わなかった間が 1 回だからである。
// 時刻 (秒) は呼び出し元が単調な時計から渡す。スレッドの安全は呼び出し元が持つ。libobs に依存しない。

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "pipeline/arrival_window.h"

namespace cordmux::pipeline {

// 度数に分ける遅れの上限 (ms)。この値以上の遅れは最後の度数 1 つに入る。D の上限 (settings の kDisplayBufferMaxMs) と同じ。
constexpr std::int64_t kArrivalMaxDelayMs = 1000;

// 直近の範囲の長さ (秒)。緩衝の値を選ぶ材料として、直近の 1 分の届き方を見る。
constexpr int kArrivalWindowSeconds = 60;

// 列に持つフレームの数の上限。240 fps で 60 秒 (14,400) には足りないが、画面共有の上限は 60 fps (3,600) で、
// 120 fps でも足りる。あふれたときは古いフレームから外す。
constexpr std::size_t kArrivalFrameCapacity = 8192;

// 範囲の分布の要約。frames が 0 のとき、ほかは 0 である。
struct ArrivalSummary {
	int window_seconds = 0;     // 実際に溜めた長さ (起動直後は kArrivalWindowSeconds より短い)
	std::uint64_t frames = 0;   // 範囲の中のフレームの数
	double p50_ms = 0.0;        // 1 ms 刻みの上端 (実際の値より最大 1 ms 大きい)。範囲の最大を超えない
	double p95_ms = 0.0;
	double p99_ms = 0.0;
	double max_ms = 0.0;        // 上限を超えた遅れも、実際の値
};

// 統計の周期 (take_period から次の take_period まで) の数。
struct ArrivalPeriod {
	std::uint64_t frames = 0;
	std::uint64_t late = 0; // 間に合わなかったフレーム
	std::uint64_t runs = 0; // 間に合わなかったフレームのまとまり
};

class ArrivalDelayStats {
public:
	// フレームを 1 つ足す。now_s は現在の秒 (単調な時計)、age_us は届くまでの遅れ、delay_us は現在の D。
	void note(std::int64_t now_s, std::int64_t age_us, std::int64_t delay_us)
	{
		const std::int64_t age = std::max<std::int64_t>(age_us, 0);
		roll(now_s);
		Second &second = ring_[ring_index(now_s_)];
		const std::size_t bin = bin_of(age);
		++second.bins[bin];
		++second.frames;
		second.max_us = std::max(second.max_us, age);
		++total_[bin];
		++total_frames_;
		push_frame(age);

		const bool late = age >= std::max<std::int64_t>(delay_us, 0);
		++period_.frames;
		if (late) {
			++period_.late;
			if (!prev_late_)
				++period_.runs;
		}
		prev_late_ = late;
	}

	// 範囲の分布の要約。
	ArrivalSummary summary(std::int64_t now_s)
	{
		roll(now_s);
		ArrivalSummary out;
		if (total_frames_ == 0)
			return out;
		out.frames = total_frames_;
		out.window_seconds = static_cast<int>(std::min<std::int64_t>(now_s_ - first_s_ + 1, kArrivalWindowSeconds));
		std::int64_t max_us = 0;
		for (const Second &second : ring_) {
			if (second.second >= 0)
				max_us = std::max(max_us, second.max_us);
		}
		out.max_ms = static_cast<double>(max_us) / 1000.0;
		out.p50_ms = percentile_ms(50, out.max_ms);
		out.p95_ms = percentile_ms(95, out.max_ms);
		out.p99_ms = percentile_ms(99, out.max_ms);
		return out;
	}

	// 範囲の中で、D を delay_ms にしていたら間に合わなかったフレームの数 (遅れ >= delay_ms)。
	std::uint64_t late_frames_at(std::int64_t now_s, std::int64_t delay_ms)
	{
		roll(now_s);
		const std::size_t from = bin_of_ms(delay_ms);
		std::uint64_t sum = 0;
		for (std::size_t i = from; i < kBinCount; ++i)
			sum += total_[i];
		return sum;
	}

	// 同じ値の率 (0 から 1)。範囲にフレームがなければ 0。
	double late_fraction_at(std::int64_t now_s, std::int64_t delay_ms)
	{
		const std::uint64_t late = late_frames_at(now_s, delay_ms);
		return total_frames_ == 0 ? 0.0 : static_cast<double>(late) / static_cast<double>(total_frames_);
	}

	// 範囲の遅れの列と、平均と最大。周期の数にも度数にも触れない。フレームがなければ空の列を返す。
	ArrivalWindow window(std::int64_t now_s)
	{
		roll(now_s);
		ArrivalWindow out;
		out.delays_ms.reserve(frame_count_);
		std::int64_t sum_us = 0;
		std::int64_t max_us = 0;
		for (std::size_t i = 0; i < frame_count_; ++i) {
			const Frame &frame = frames_[(frame_head_ + i) % kArrivalFrameCapacity];
			out.delays_ms.push_back(static_cast<std::uint16_t>(
				std::min<std::int64_t>(frame.age_us / 1000, kArrivalWindowClampMs)));
			sum_us += frame.age_us;
			max_us = std::max<std::int64_t>(max_us, frame.age_us);
		}
		if (frame_count_ != 0) {
			out.mean_ms = static_cast<double>(sum_us) / static_cast<double>(frame_count_) / 1000.0;
			out.max_ms = static_cast<double>(max_us) / 1000.0;
		}
		return out;
	}

	// 周期の数を取り出して 0 に戻す。続けて間に合わなかったまとまりは、周期をまたいでも 1 つと数える
	// (前のフレームが間に合わなかった印は残す)。
	ArrivalPeriod take_period()
	{
		const ArrivalPeriod out = period_;
		period_ = ArrivalPeriod{};
		return out;
	}

private:
	// 度数は 0 ms から kArrivalMaxDelayMs - 1 ms まで 1 ms 刻みと、最後の「それ以上」。
	static constexpr std::size_t kBinCount = static_cast<std::size_t>(kArrivalMaxDelayMs) + 1;

	struct Second {
		std::int64_t second = -1; // この度数が溜めた秒。-1 は未使用
		std::uint64_t frames = 0;
		std::int64_t max_us = 0;
		std::array<std::uint32_t, kBinCount> bins{};
	};

	struct Frame {
		std::int64_t second = 0; // 足した秒 (roll が進めた後の now_s_)
		std::int32_t age_us = 0;
	};

	// 列に足す。輪がいっぱいなら最も古いフレームを外す。初めて足すときに輪を確保する (フレームの来ないストリームに持たせない)。
	void push_frame(std::int64_t age_us)
	{
		if (frames_.empty())
			frames_.resize(kArrivalFrameCapacity);
		if (frame_count_ == kArrivalFrameCapacity) {
			frame_head_ = (frame_head_ + 1) % kArrivalFrameCapacity;
			--frame_count_;
		}
		Frame &slot = frames_[(frame_head_ + frame_count_) % kArrivalFrameCapacity];
		slot.second = now_s_;
		slot.age_us = static_cast<std::int32_t>(
			std::min<std::int64_t>(age_us, std::numeric_limits<std::int32_t>::max()));
		++frame_count_;
	}

	static std::size_t bin_of(std::int64_t age_us) { return bin_of_ms(age_us / 1000); }

	static std::size_t bin_of_ms(std::int64_t ms)
	{
		if (ms <= 0)
			return 0;
		return static_cast<std::size_t>(std::min<std::int64_t>(ms, kArrivalMaxDelayMs));
	}

	static std::size_t ring_index(std::int64_t second)
	{
		return static_cast<std::size_t>(second % kArrivalWindowSeconds);
	}

	// 現在の秒まで進め、期限が切れた秒の度数を合計から引く。時計が戻っても、秒は戻さない。
	void roll(std::int64_t now_s)
	{
		if (!started_) {
			started_ = true;
			first_s_ = now_s;
			now_s_ = now_s;
			ring_[ring_index(now_s_)].second = now_s_;
			return;
		}
		if (now_s <= now_s_)
			return;
		now_s_ = now_s;
		for (Second &second : ring_) {
			if (second.second >= 0 && second.second <= now_s_ - kArrivalWindowSeconds) {
				for (std::size_t i = 0; i < kBinCount; ++i)
					total_[i] -= second.bins[i];
				total_frames_ -= second.frames;
				second = Second{};
			}
		}
		Second &current = ring_[ring_index(now_s_)];
		if (current.second != now_s_)
			current.second = now_s_;
		// 列も同じ期限で外す (足した順なので、先頭から外せばよい)。
		while (frame_count_ != 0 && frames_[frame_head_].second <= now_s_ - kArrivalWindowSeconds) {
			frame_head_ = (frame_head_ + 1) % kArrivalFrameCapacity;
			--frame_count_;
		}
	}

	// 小さいほうから数えて、全体の pct % 以上に達する最初の度数の上端。最大を超えない。上限以上の度数は最大で返す。
	double percentile_ms(std::uint64_t pct, double max_ms) const
	{
		const std::uint64_t target = (total_frames_ * pct + 99) / 100;
		std::uint64_t acc = 0;
		std::size_t idx = 0;
		for (; idx < kBinCount; ++idx) {
			acc += total_[idx];
			if (acc >= target)
				break;
		}
		const double upper_ms = (idx + 1 >= kBinCount) ? max_ms : static_cast<double>(idx + 1);
		return std::min(upper_ms, max_ms);
	}

	std::array<Second, kArrivalWindowSeconds> ring_{};
	std::array<std::uint64_t, kBinCount> total_{};
	std::uint64_t total_frames_ = 0;
	bool started_ = false;
	std::int64_t first_s_ = 0;
	std::int64_t now_s_ = 0;
	std::vector<Frame> frames_;
	std::size_t frame_head_ = 0;
	std::size_t frame_count_ = 0;
	bool prev_late_ = false;
	ArrivalPeriod period_;
};

} // namespace cordmux::pipeline
