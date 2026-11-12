// SPDX-License-Identifier: Apache-2.0

// 録画の黒埋めの計算。欠落を「黒埋め用のフレームのファイルを N 回並べ、最後の 1 つを concat の outpoint で切る」形に
// 分ける。concat demuxer の duration は使わない。duration はパケットを落とさずに開始位置だけを動かすので
// タイムスタンプが重なり、muxer が 1 単位ずつずらしてしまうためである。
// libobs、Qt、avformat に依存しない。

#pragma once

#include <cstdint>
#include <string>

#include "record/record_types.h"

namespace cordmux::record {

// 欠落 1 つの埋め方。黒埋め用のフレームのファイルを repeat_count 回そのまま並べ、最後に out_point_ticks で切った
// 1 行を加える (端数が 0 なら加えない)。to_gap_fill が録画ログの GapFill を作れるように、欠落の長さも持つ。
struct BlackPlan {
	int repeat_count = 0;              // 黒埋め用のフレームをそのまま並べる回数
	std::int64_t remainder_ticks = 0;  // 端数の長さ (timescale の単位。0 なら端数はない)
	std::int64_t out_point_ticks = 0;  // concat の outpoint に書く位置 (黒埋め用のフレームの先頭から)
	bool exact = false;                // 欠落の長さを、ちょうどフレーム数で表せたか (black_us_to_frames)
	std::int64_t total_ticks = 0;      // 欠落の長さ (丸めた後。timescale の単位)
	int timescale = 0;                 // 上の 2 つの単位 (映像は 60000)
	std::int64_t total_frames = 0;     // 欠落のフレーム数 (丸めた後)
	std::int64_t material_frames = 0;  // 黒埋め用のフレームのファイル 1 つのフレーム数
};

namespace detail {

// a * b / c を最も近い整数に丸める (a >= 0、c > 0)。
inline std::int64_t mul_div_round(std::int64_t a, std::int64_t b, std::int64_t c)
{
	if (c <= 0)
		return 0;
	const std::int64_t product = a * b;
	return (product + c / 2) / c;
}

} // namespace detail

// フレーム数を timescale の単位に修正する。
inline std::int64_t black_frames_to_ticks(std::int64_t frames, int fps_num, int fps_den, int timescale)
{
	if (frames <= 0 || fps_num <= 0 || fps_den <= 0 || timescale <= 0)
		return 0;
	return detail::mul_div_round(frames * timescale, fps_den, fps_num);
}

// 長さ (us) をフレーム数に修正する。最も近い整数に丸める。切り捨てると欠落が毎回 1 フレーム短くなり、再接続の多い
// セッションで誤差が積もるためである。丸めた結果が元の長さと一致したかは、BlackPlan::exact に別に持つ。
inline std::int64_t black_us_to_frames(std::int64_t duration_us, int fps_num, int fps_den)
{
	if (duration_us <= 0 || fps_num <= 0 || fps_den <= 0)
		return 0;
	return detail::mul_div_round(duration_us, fps_num, static_cast<std::int64_t>(fps_den) * 1000000);
}

// 黒埋め用のフレームのファイルのフレーム数が分かっているときの分け方。録画ログの black の行は frame_count を持つので、
// 録画ファイルの作成はこちらを使う (長さを us に修正してから戻すと、2 度丸めることになる)。
// 黒埋め用のフレームがないとき (material_frames が 0) は、並べる行を作らずに欠落の長さだけを返す。その人は ProRes で
// 作成し、黒は lavfi で作るためである。
inline BlackPlan plan_black_frames(std::int64_t gap_us, std::int64_t material_frames, int fps_num, int fps_den,
				   int timescale)
{
	BlackPlan plan;
	plan.timescale = timescale;
	plan.material_frames = material_frames > 0 ? material_frames : 0;
	if (gap_us <= 0 || fps_num <= 0 || fps_den <= 0 || timescale <= 0) {
		plan.exact = gap_us <= 0;
		return plan;
	}

	plan.total_frames = black_us_to_frames(gap_us, fps_num, fps_den);
	plan.total_ticks = black_frames_to_ticks(plan.total_frames, fps_num, fps_den, timescale);
	// 丸めずに表せたか (us * fps が fps_den * 1 秒で割り切れるか)。
	plan.exact = (gap_us * fps_num) % (static_cast<std::int64_t>(fps_den) * 1000000) == 0;

	if (plan.material_frames <= 0)
		return plan; // 並べる行は作らない (上のコメント)

	plan.repeat_count = static_cast<int>(plan.total_frames / plan.material_frames);
	const std::int64_t remainder_frames = plan.total_frames % plan.material_frames;
	plan.remainder_ticks = black_frames_to_ticks(remainder_frames, fps_num, fps_den, timescale);
	plan.out_point_ticks = plan.remainder_ticks;
	return plan;
}

// 黒埋め用のフレームのファイルの長さ (us) から分ける。
inline BlackPlan plan_black(std::int64_t gap_us, std::int64_t material_duration_us, int fps_num, int fps_den,
			    int timescale)
{
	return plan_black_frames(gap_us, black_us_to_frames(material_duration_us, fps_num, fps_den), fps_num, fps_den,
				 timescale);
}

// 録画ログの finalize の行に残す、欠落 1 つの埋め方。
inline GapFill to_gap_fill(const BlackPlan &plan, std::int64_t start_us, const std::string &encoder_instance_id)
{
	GapFill fill;
	fill.start_us = start_us;
	fill.duration_us = plan.timescale > 0 ? detail::mul_div_round(plan.total_ticks, 1000000, plan.timescale) : 0;
	fill.repeat_count = plan.repeat_count;
	fill.remainder_ticks = plan.remainder_ticks;
	fill.black_encoder_instance_id = encoder_instance_id;
	return fill;
}

} // namespace cordmux::record
