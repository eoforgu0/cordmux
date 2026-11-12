// SPDX-License-Identifier: Apache-2.0

// pipeline/av_offset_plan.h の単体テスト。画面の音と映像のずれの補正を、符号に応じて音のタイムスタンプに足すか、
// 音を置く時刻から引くかを確認する。

#include "doctest.h"

#include "config/settings.h"
#include "pipeline/av_offset_plan.h"

using cordmux::pipeline::plan_screen_audio_offset;

TEST_CASE("補正が 0 なら音のタイムスタンプにも置く時刻にも何もしない")
{
	const auto plan = plan_screen_audio_offset(0);
	CHECK(plan.audio_constant_offset_us == 0);
	CHECK(plan.audio_early_us == 0);
}

TEST_CASE("補正が正なら音のタイムスタンプだけに足す")
{
	const auto plan = plan_screen_audio_offset(33);
	CHECK(plan.audio_constant_offset_us == 33000);
	CHECK(plan.audio_early_us == 0);
}

TEST_CASE("補正が負なら音を置く時刻からだけ絶対値を引く")
{
	const auto plan = plan_screen_audio_offset(-62);
	CHECK(plan.audio_constant_offset_us == 0);
	CHECK(plan.audio_early_us == 62000);
}

TEST_CASE("設定の範囲のどの補正でも、足す値と引く値は 0 以上で、両方には入らない")
{
	for (int ms = cordmux::config::kScreenAudioOffsetMinMs; ms <= cordmux::config::kScreenAudioOffsetMaxMs;
	     ms += 250) {
		const auto plan = plan_screen_audio_offset(ms);
		CHECK(plan.audio_constant_offset_us >= 0);
		CHECK(plan.audio_early_us >= 0);
		CHECK((plan.audio_constant_offset_us == 0 || plan.audio_early_us == 0));
		// 2 つの値の差は、いつも元の補正 (符号を含む) に等しい。
		CHECK(plan.audio_constant_offset_us - plan.audio_early_us ==
		      static_cast<std::int64_t>(ms) * 1000);
	}
}

TEST_CASE("設定の範囲の両端でもオーバーフローしない")
{
	const auto lowest = plan_screen_audio_offset(cordmux::config::kScreenAudioOffsetMinMs);
	CHECK(lowest.audio_early_us == 2000000);
	CHECK(lowest.audio_constant_offset_us == 0);

	const auto highest = plan_screen_audio_offset(cordmux::config::kScreenAudioOffsetMaxMs);
	CHECK(highest.audio_constant_offset_us == 2000000);
	CHECK(highest.audio_early_us == 0);
}
