// SPDX-License-Identifier: Apache-2.0

// 非同期の映像のソース 1 つに、フレームを渡す、空ける、保ってから空ける予約を置く、をまとめたコンポーネント。合成のソースの子
// (composite_source.cpp) が OBS に渡すための、output_mu で守る状態と手順を 1 か所に持つ (本体は sources/frame_sink_core.h)。
// 空ける (obs_source_output_video に NULL を渡す) のは、OBS の tick のコールバック (tick) の中だけで行い、すぐ空ける要求も
// 次の tick (最大 1 フレーム後) で空ける。libobs の子の大きさ (async_active) が偽に変わるのを描画の前の tick の間に限り、
// 合成のソースとフィルタ cordmux_solo_filter が同じフレームに読んだ値で「描く」と決めた子が、描画の途中で空にならないように
// するためである (composite_source.h)。フレームが届いて真に変わるのは描画の途中でも起きるが、その子は、そのフレームの値では
// 大きさ 0 で描かないので、食い違わない。例外は方式の切り替え (BasicFrameSink::output) の NULL だけである。

#pragma once

#include <cstdint>

#include <obs-module.h>
#include <util/platform.h>

#include "sources/frame_sink_core.h"

namespace cordmux::sources {

// BasicFrameSink が呼ぶ OBS の関数。
struct ObsFramePort {
	using Source = obs_source_t;
	using Frame = obs_source_frame2;
	static void output_frame(obs_source_t *source, const obs_source_frame2 *frame)
	{
		obs_source_output_video2(source, frame);
	}
	static void output_null(obs_source_t *source) { obs_source_output_video(source, nullptr); }
	static void set_unbuffered(obs_source_t *source, bool unbuffered)
	{
		obs_source_set_async_unbuffered(source, unbuffered);
	}
	static std::uint64_t now_ns() { return os_gettime_ns(); }
};

using FrameSink = BasicFrameSink<ObsFramePort>;

} // namespace cordmux::sources
