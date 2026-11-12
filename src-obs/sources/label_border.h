// SPDX-License-Identifier: Apache-2.0

// 発言中の光を表示するタイミングを決める純粋関数。libobs にも Qt にも依存しない。合成のソース (cordmux) の
// 光と暗さが、描くたびにこの式で明るさを求める。

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cordmux::sources {

// 光を表示するタイミング (ミリ秒)。値は既定値で、settings.json の label_border_rise_ms などで変えられる。
struct BorderTiming {
	int rise_ms = 150; // 声が始まってから最大の明るさになるまで (0 なら次のフレームで最大の明るさにする)
	int hold_ms = 200; // 声が止まってから明るさを保つ時間
	int fade_ms = 150; // 保った後、消えるまでの時間
};

// 1 人の発言の始まりと終わり (os_gettime_ns の時刻。声に実際にかけている遅延を足した後の値)。
// start_ns が 0 なら話していない (一度も始まっていない)。stop_ns が 0 ならまだ話している。
struct SpeakingTimes {
	std::uint64_t start_ns = 0;
	std::uint64_t stop_ns = 0;
};

namespace detail {

inline std::uint64_t ms_to_ns(int ms)
{
	return ms > 0 ? static_cast<std::uint64_t>(ms) * 1'000'000ULL : 0ULL;
}

// 表示し始めてから elapsed_ns たったときの明るさ (0 から 1)。
inline double rise_level(std::uint64_t elapsed_ns, const BorderTiming &timing)
{
	const std::uint64_t rise = ms_to_ns(timing.rise_ms);
	if (rise == 0 || elapsed_ns >= rise)
		return 1.0;
	return static_cast<double>(elapsed_ns) / static_cast<double>(rise);
}

} // namespace detail

// 時刻 now_ns の光の明るさ (0 なら描画しない、1 なら最大)。明るさはフレームごとに変わり、光は名前ラベルの
// 画像に含めないので、描画するたびに呼び出す。始まりより前 (声の遅延を足した時刻がまだ来ていない) は 0。
// 止まった後は、止まった時点の明るさのまま hold_ms 保ち、fade_ms で直線的に 0 まで下げる。
inline double speaking_level(const SpeakingTimes &times, std::uint64_t now_ns, const BorderTiming &timing)
{
	if (times.start_ns == 0 || now_ns < times.start_ns)
		return 0.0;
	if (times.stop_ns == 0 || now_ns < times.stop_ns)
		return detail::rise_level(now_ns - times.start_ns, timing);

	const double at_stop =
		times.stop_ns > times.start_ns ? detail::rise_level(times.stop_ns - times.start_ns, timing) : 0.0;
	const std::uint64_t after = now_ns - times.stop_ns;
	const std::uint64_t hold = detail::ms_to_ns(timing.hold_ms);
	if (after < hold)
		return at_stop;
	const std::uint64_t fade = detail::ms_to_ns(timing.fade_ms);
	if (fade == 0 || after - hold >= fade)
		return 0.0;
	return at_stop * (1.0 - static_cast<double>(after - hold) / static_cast<double>(fade));
}

// 光が見える可能性のある最後の時刻 (この後は speaking_level が必ず 0 を返す)。一度も話していなければ 0、
// 話している間は上限がないので UINT64_MAX を返す。
inline std::uint64_t speaking_visible_until(const SpeakingTimes &times, const BorderTiming &timing)
{
	if (times.start_ns == 0)
		return 0;
	if (times.stop_ns == 0)
		return UINT64_MAX;
	return times.stop_ns + detail::ms_to_ns(timing.hold_ms) + detail::ms_to_ns(timing.fade_ms);
}

// 新しく届いた (start_ns, stop_ns) を前の値 prev に重ねる。
//   - 話し始め (stop_ns が 0) が、前の発言の光がまだ見えている間 (保っている間か、消えかけ) に来たら、
//     その時刻の明るさから上がり直す。始まりを、立ち上がりの途中でその明るさになる時刻 (start_ns - 明るさ x
//     rise_ms) にずらす。消えかけの線を 0 に戻してから表示し直すと点滅が増えるためで、配信の画面を落ち着かせる
//     ために、点いたり消えたりを 1 秒に 3 回より多くしない。一気に最大の明るさへ戻すと、明るさが跳ねて見える。
//     話している最中に話し始めが重ねて届いても、明るさは変わらない。
//   - 止まり (stop_ns が 0 でない) は、前の発言が続いている最中なら前の始まりを引き継ぐ。呼び出し元は
//     始まりを覚えていなくてもよく、start_ns に 0 を渡してよい。
inline SpeakingTimes merge_speaking(const SpeakingTimes &prev, std::uint64_t start_ns, std::uint64_t stop_ns,
				    const BorderTiming &timing)
{
	SpeakingTimes next;
	if (stop_ns == 0) {
		if (start_ns == 0)
			return prev; // 始まりも止まりも届いていない
		const bool still_visible = prev.start_ns != 0 && prev.start_ns <= start_ns &&
					   start_ns <= speaking_visible_until(prev, timing);
		next.start_ns = start_ns;
		next.stop_ns = 0;
		if (still_visible) {
			const double level = speaking_level(prev, start_ns, timing);
			const auto back = static_cast<std::uint64_t>(
				std::llround(level * static_cast<double>(detail::ms_to_ns(timing.rise_ms))));
			// 時刻 0 は「話していない」を表すので、1 ナノ秒より前にはしない。
			next.start_ns = back < start_ns ? start_ns - back : 1;
		}
		return next;
	}
	const bool talking = prev.start_ns != 0 && prev.stop_ns == 0;
	if (talking && (start_ns == 0 || prev.start_ns <= start_ns))
		next.start_ns = prev.start_ns;
	else
		next.start_ns = start_ns != 0 ? start_ns : stop_ns;
	next.stop_ns = std::max(stop_ns, next.start_ns);
	return next;
}

} // namespace cordmux::sources
