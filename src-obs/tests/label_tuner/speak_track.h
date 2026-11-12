// SPDX-License-Identifier: Apache-2.0

// 名前ラベルの調整ツールで、1 人が話しているかを持ち、話している人を示す明るさの元になる時刻 (sources::SpeakingTimes)
// を作る。話しているのは、[話している] の切り替え、[プッシュ・トゥ・トーク] のボタン、「2 秒話して 1 秒黙る」の繰り返しの
// どれかが真の間である。始まりと止まりは、合成のソースと同じ関数 (sources/label_border.h の merge_speaking) で重ね、
// 明るさも合成のソースと同じ関数 (sources/composite_label_plan.h の composite_speaking_level) で求める。時刻は
// 呼び出し元が渡す (画面では実際の時刻、--shot では引数で決めた時刻)。libobs にも Qt にも依存しない。

#pragma once

#include <algorithm>
#include <cstdint>

#include "sources/composite_label_plan.h"
#include "sources/label_border.h"

namespace cordmux::label_tuner {

// 「2 秒話して 1 秒黙る」の、話す時間と黙る時間。
inline constexpr std::uint64_t kCycleSpeakNs = 2'000'000'000ULL;
inline constexpr std::uint64_t kCycleSilentNs = 1'000'000'000ULL;

class SpeakTrack {
public:
	// 3 つの入力を切り替える。now_ns はその操作の時刻 (0 より大きい。0 は「話していない」を表す)。前に渡した時刻より
	// 前にしない。繰り返しは、入れた時刻から話し始める。
	void set_toggle(bool on, std::uint64_t now_ns, const sources::BorderTiming &timing)
	{
		advance(now_ns, timing);
		toggle_ = on;
		apply(now_ns, timing);
	}

	void set_hold(bool on, std::uint64_t now_ns, const sources::BorderTiming &timing)
	{
		advance(now_ns, timing);
		hold_ = on;
		apply(now_ns, timing);
	}

	void set_cycle(bool on, std::uint64_t now_ns, const sources::BorderTiming &timing)
	{
		advance(now_ns, timing);
		cycle_ = on;
		cycle_start_ = now_ns;
		apply(now_ns, timing);
	}

	// 繰り返しの切り替わりを、now_ns までの順に、切り替わった時刻で重ねる (描く間隔が空いても、始まりと止まりの
	// 時刻がずれない)。明るさを読む前に呼ぶ。
	void advance(std::uint64_t now_ns, const sources::BorderTiming &timing)
	{
		while (cycle_) {
			const std::uint64_t edge = next_cycle_edge(last_ns_);
			if (edge > now_ns)
				break;
			last_ns_ = edge;
			apply(edge, timing);
		}
		last_ns_ = std::max(last_ns_, now_ns);
	}

	// 時刻 now_ns の明るさ (0 から 1)。合成のソースと同じく、ミキサーの音量とミュートは、声が配信に出ている状態にする。
	double level(std::uint64_t now_ns, const sources::BorderTiming &timing, bool show_speaking) const
	{
		return sources::composite_speaking_level(times_, now_ns, timing, show_speaking, false, 1.0f);
	}

	// 時刻 now_ns の、名前ラベルを暗くするための明るさ (0 から 1)。光の表示のオンとオフには依らない。
	double dim_level(std::uint64_t now_ns, const sources::BorderTiming &timing) const
	{
		return sources::composite_label_dim_level(times_, now_ns, timing, false, 1.0f);
	}

	bool speaking() const { return speaking_; }
	bool toggle() const { return toggle_; }
	bool cycle() const { return cycle_; }
	const sources::SpeakingTimes &times() const { return times_; }

private:
	bool in_speak_phase(std::uint64_t at) const
	{
		return (at - cycle_start_) % (kCycleSpeakNs + kCycleSilentNs) < kCycleSpeakNs;
	}

	// after より後の、最初の繰り返しの切り替わりの時刻。
	std::uint64_t next_cycle_edge(std::uint64_t after) const
	{
		const std::uint64_t period = kCycleSpeakNs + kCycleSilentNs;
		const std::uint64_t from = std::max(after, cycle_start_);
		const std::uint64_t offset = (from - cycle_start_) % period;
		const std::uint64_t base = from - offset;
		return offset < kCycleSpeakNs ? base + kCycleSpeakNs : base + period;
	}

	void apply(std::uint64_t at, const sources::BorderTiming &timing)
	{
		const bool wanted = toggle_ || hold_ || (cycle_ && in_speak_phase(at));
		if (wanted == speaking_)
			return;
		speaking_ = wanted;
		times_ = wanted ? sources::merge_speaking(times_, at, 0, timing)
				: sources::merge_speaking(times_, 0, at, timing);
	}

	bool toggle_ = false;
	bool hold_ = false;
	bool cycle_ = false;
	bool speaking_ = false;
	std::uint64_t cycle_start_ = 0;
	std::uint64_t last_ns_ = 0;
	sources::SpeakingTimes times_;
};

} // namespace cordmux::label_tuner
