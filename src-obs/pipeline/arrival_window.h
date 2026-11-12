// SPDX-License-Identifier: Apache-2.0

// 画面のストリーム 1 本の、直近の範囲の届くまでの遅れの列 (ArrivalWindow) と、その列から「D を X にしたら」の
// 数を求める純粋な関数。設定ウィンドウの表が、入力欄の値 (X) を変えるたびに計算し直し、後から D を自動で決める
// 形にもそのまま使えるように、時計もスレッドも持たない関数にしてある。libobs に依存しない。
//
// 判定は FramePacer と同じ約束 (遅れ >= X) である。列の値は ms に切り捨てて持つので、整数の X に対する
// 遅れ(us) >= X * 1000 と、列の値 >= X は同じ結果になる。続けて間に合わなかったフレームは 1 つのまとまりと数える
// (見た目の引っかかりは、続けて間に合わなかった間が 1 回だから)。

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cordmux::pipeline {

// 列に持つ遅れの上限 (ms)。これ以上は同じ値にまとめる。D の上限 (1000 ms) より大きく取り、X の範囲では
// 判定が変わらない。
constexpr std::uint16_t kArrivalWindowClampMs = 65535;

// ストリーム 1 本の直近の範囲。delays_ms は届いた順の遅れ (ms、切り捨て)。平均と最大は、列に丸める前の
// 実際の値 (ms)。
struct ArrivalWindow {
	std::vector<std::uint16_t> delays_ms;
	double mean_ms = 0.0;
	double max_ms = 0.0;
};

inline bool operator==(const ArrivalWindow &a, const ArrivalWindow &b)
{
	return a.delays_ms == b.delays_ms && a.mean_ms == b.mean_ms && a.max_ms == b.max_ms;
}

inline bool operator!=(const ArrivalWindow &a, const ArrivalWindow &b)
{
	return !(a == b);
}

// 範囲のフレームの数と、X で間に合わなかったフレームの数とまとまりの数。
struct LateCount {
	std::uint64_t frames = 0;
	std::uint64_t late = 0;
	std::uint64_t runs = 0;

	// 遅れたフレームの率 (0 から 1)。フレームがなければ 0。
	double fraction() const
	{
		return frames == 0 ? 0.0 : static_cast<double>(late) / static_cast<double>(frames);
	}
};

// 1 本の列で、D を x_ms にしていたら間に合わなかった数 (遅れ >= x_ms)。x_ms が 0 以下なら全部が間に合わない。
inline LateCount late_count(const ArrivalWindow &window, std::int64_t x_ms)
{
	LateCount out;
	out.frames = window.delays_ms.size();
	bool prev_late = false;
	for (const std::uint16_t delay : window.delays_ms) {
		const bool late = static_cast<std::int64_t>(delay) >= x_ms;
		if (late) {
			++out.late;
			if (!prev_late)
				++out.runs;
		}
		prev_late = late;
	}
	return out;
}

// 1 人の複数のストリーム (1 人が画面を 2 本以上共有しているとき) の合計。まとまりはストリームをまたいで続かない。
inline LateCount late_count(const std::vector<ArrivalWindow> &windows, std::int64_t x_ms)
{
	LateCount out;
	for (const ArrivalWindow &window : windows) {
		const LateCount one = late_count(window, x_ms);
		out.frames += one.frames;
		out.late += one.late;
		out.runs += one.runs;
	}
	return out;
}

// フレームの数で重みを付けた平均の遅れ (ms)。フレームがなければ 0。
inline double mean_delay_ms(const std::vector<ArrivalWindow> &windows)
{
	double sum = 0.0;
	std::uint64_t frames = 0;
	for (const ArrivalWindow &window : windows) {
		sum += window.mean_ms * static_cast<double>(window.delays_ms.size());
		frames += window.delays_ms.size();
	}
	return frames == 0 ? 0.0 : sum / static_cast<double>(frames);
}

// 最大の遅れ (ms)。フレームがなければ 0。
inline double max_delay_ms(const std::vector<ArrivalWindow> &windows)
{
	double out = 0.0;
	for (const ArrivalWindow &window : windows) {
		if (!window.delays_ms.empty())
			out = std::max(out, window.max_ms);
	}
	return out;
}

// 範囲のフレームの数。
inline std::uint64_t frame_count(const std::vector<ArrivalWindow> &windows)
{
	std::uint64_t out = 0;
	for (const ArrivalWindow &window : windows)
		out += window.delays_ms.size();
	return out;
}

// 小さい方から数えて pct % 以上に達する最初のフレームの遅れ (ms。最も近い順位の方法)。pct は 0 から 100 に収める。
// フレームがなければ 0。「この率までのフレームを間に合わせる D」を求めるのに使う。
inline std::int64_t delay_quantile_ms(const std::vector<ArrivalWindow> &windows, int pct)
{
	std::vector<std::uint16_t> all;
	for (const ArrivalWindow &window : windows)
		all.insert(all.end(), window.delays_ms.begin(), window.delays_ms.end());
	if (all.empty())
		return 0;
	const std::uint64_t clamped = static_cast<std::uint64_t>(std::clamp(pct, 0, 100));
	const std::size_t rank =
		std::max<std::size_t>(1, static_cast<std::size_t>((all.size() * clamped + 99) / 100));
	std::nth_element(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(rank - 1), all.end());
	return all[rank - 1];
}

} // namespace cordmux::pipeline
