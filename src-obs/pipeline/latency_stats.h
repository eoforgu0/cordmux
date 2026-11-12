// SPDX-License-Identifier: Apache-2.0

// 受信側の遅れの統計。フレームごとに「受信側の OBS の時計 - 送信側のタイムスタンプ (OBS の時計に換算した値)」を足し上げ、
// 統計の周期ごとに平均と 95 パーセンタイルを取り出して戻す。換算は接続ごとに決めた時計の差で行う (pipeline/sender_clock.h)
// ので、この差は「その段階までにかかった時間」になる (タイムスタンプの時計は docs/wire_format.md の「timestamp_us」にある)。
// 足すときは数と合計とビンの 1 つの加算しかしない (受信とデコードのスレッドの負荷にしない)。計算は取り出す
// ときに行う。スレッドの安全は呼び出し側が持つ (pipeline.cpp は mu の中で足し、mu の中で取り出す)。
// libobs にも asio にも依存しない。

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace cordmux::pipeline {

// 取り出した 1 周期分の結果。count が 0 のとき、ほかは 0 である。
struct LatencySummary {
	std::uint64_t count = 0;
	double avg_ms = 0.0;
	// 95 パーセンタイルの近似。1 ms 刻みのビンの上端 (実際の値より最大 1 ms 大きい)。区間の最大を超えない。
	// 上限を超えた値は最大の値で数える。
	double p95_ms = 0.0;
	double max_ms = 0.0;
};

class LatencyWindow {
public:
	// ビンは 0 ms から kBinCount - 1 ms まで 1 ms 刻みで、最後のビンは「それ以上」。通話の遅れとして見たい範囲
	// (数百 ms) に十分な粗さと広さで、メモリは 1 つにつき約 8 KiB である。
	static constexpr std::size_t kBinCount = 2001;

	// us は「受信側の OBS の時計 - 送信側のタイムスタンプ (OBS の時計に換算した値)」。負の値 (時計の食い違い) は、合計には実際の値を入れ、ビンには 0 ms として
	// 数える。
	void add(std::int64_t us)
	{
		++count_;
		sum_us_ += us;
		max_us_ = std::max(max_us_, us);
		const std::int64_t clamped_us = std::max<std::int64_t>(us, 0);
		const std::int64_t bin = clamped_us / 1000;
		++bins_[static_cast<std::size_t>(std::min<std::int64_t>(bin, std::int64_t(kBinCount) - 1))];
	}

	std::uint64_t count() const { return count_; }

	// 溜めた分から平均と 95 パーセンタイルを出し、次の周期のために空に戻す。
	LatencySummary take()
	{
		LatencySummary out;
		out.count = count_;
		if (count_ > 0) {
			out.avg_ms = double(sum_us_) / double(count_) / 1000.0;
			out.max_ms = double(max_us_) / 1000.0;
			// 小さいほうから数えて、全体の 95% 以上に達する最初のビン。
			const std::uint64_t target = (count_ * 95 + 99) / 100;
			std::uint64_t acc = 0;
			std::size_t idx = 0;
			for (; idx < kBinCount; ++idx) {
				acc += bins_[idx];
				if (acc >= target)
					break;
			}
			const double upper_ms = (idx + 1 >= kBinCount) ? out.max_ms : double(idx + 1);
			out.p95_ms = std::min(upper_ms, std::max(out.max_ms, 0.0));
		}
		reset();
		return out;
	}

	void reset()
	{
		count_ = 0;
		sum_us_ = 0;
		max_us_ = 0;
		bins_.fill(0);
	}

private:
	std::uint64_t count_ = 0;
	std::int64_t sum_us_ = 0;
	std::int64_t max_us_ = 0;
	std::array<std::uint32_t, kBinCount> bins_{};
};

// OBS が表示に選んだ映像のフレームの遅れ (us)。frame_timestamp_ns は OBS に渡したフレームのタイムスタンプで、送信側の
// タイムスタンプに整列の補正と画面の音のずれの補正を足した値である。correction_us はその足した分
// (渡したタイムスタンプ - 送信側のタイムスタンプを OBS の時計に換算した値) で、引くと送信側のタイムスタンプを OBS の時計に
// 換算した値に戻る。now_us はそれと同じ OBS の時計 (os_gettime_ns / 1000) の今である。
inline std::int64_t shown_latency_us(std::int64_t now_us, std::uint64_t frame_timestamp_ns, std::int64_t correction_us)
{
	const std::int64_t sent_us = std::int64_t(frame_timestamp_ns / 1000) - correction_us;
	return now_us - sent_us;
}

// 表示するフレームの遅れの統計。OBS は 60 fps で描画しても、フレームが 30 fps なら同じフレームを 2 回表示するので、表示する
// フレームが替わった (タイムスタンプが前に数えたフレームと違う) ときだけ数える。足すときの負荷は LatencyWindow と同じ。
// スレッドの安全は呼び出し側が持つ。
class ShownFrameWindow {
public:
	// 数えたら true、前と同じフレームで数えなかったら false。
	bool note(std::uint64_t frame_timestamp_ns, std::int64_t now_us, std::int64_t correction_us)
	{
		if (has_last_ && frame_timestamp_ns == last_timestamp_ns_)
			return false;
		has_last_ = true;
		last_timestamp_ns_ = frame_timestamp_ns;
		window_.add(shown_latency_us(now_us, frame_timestamp_ns, correction_us));
		return true;
	}

	// 溜めた分を取り出して空に戻す。最後に数えたフレームは覚えたままなので、取り出しをまたいで同じフレームを数え直さない。
	LatencySummary take() { return window_.take(); }

private:
	bool has_last_ = false;
	std::uint64_t last_timestamp_ns_ = 0;
	LatencyWindow window_;
};

// 画面の映像 1 本の 3 か所。received は WebSocket で受け取った時、decoded はデコードし終えた時、handed は OBS に渡す
// 直前 (obs_source_output_video2 を呼ぶ関数の直前)。
struct VideoLatency {
	LatencyWindow received;
	LatencyWindow decoded;
	LatencyWindow handed;
};

// 音声 1 本の 2 か所。received は WebSocket で受け取った時、handed は OBS に渡す直前。
struct AudioLatency {
	LatencyWindow received;
	LatencyWindow handed;
};

} // namespace cordmux::pipeline
