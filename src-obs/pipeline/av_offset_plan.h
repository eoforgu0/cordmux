// SPDX-License-Identifier: Apache-2.0

// 画面の音と映像のずれの補正 (settings の screen_audio_offset_ms) を、音のタイムスタンプに足す値と、音を置く時刻から
// 引く値に分ける純粋関数。符号を取り違えると補正が逆に動作するので、分け方だけをここに置いて単体テストで確認する。
// 実際に使うのは pipeline.cpp である。libobs にも FFmpeg にも依存しない。

#pragma once

#include <cstdint>

namespace cordmux::pipeline {

struct AvOffsetPlan {
	// 画面の音のタイムスタンプに足す定数 (マイクロ秒。align::AvOffsetConfig::constant_offset_us に渡す)。
	// 補正が正のときだけ値が入り、それ以外は 0。
	std::int64_t audio_constant_offset_us = 0;

	// 画面の音を置く時刻から引く定数 (マイクロ秒。composite_audio_plan.h の screen_audio_at_us の early_us)。
	// 補正が負のときだけ絶対値が入り、それ以外は 0。映像は遅れず、音がこの分だけ早く再生される。
	std::int64_t audio_early_us = 0;
};

// screen_audio_offset_ms (ミリ秒、符号付き) を 2 つの値に分ける。
// 正なら音のタイムスタンプに足して音を遅らせ、負なら音を置く時刻から絶対値を引いて音を早める。
// 0 ならどちらも 0 (補正しない)。
inline AvOffsetPlan plan_screen_audio_offset(int screen_audio_offset_ms)
{
	const std::int64_t offset_us = static_cast<std::int64_t>(screen_audio_offset_ms) * 1000;
	AvOffsetPlan plan;
	if (offset_us >= 0)
		plan.audio_constant_offset_us = offset_us;
	else
		plan.audio_early_us = -offset_us;
	return plan;
}

} // namespace cordmux::pipeline
