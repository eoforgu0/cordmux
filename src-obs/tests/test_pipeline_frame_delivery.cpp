// SPDX-License-Identifier: Apache-2.0

// pipeline/frame_delivery_plan.h (画面共有のフレームを OBS に渡すかの判断) のテスト。渡す先は合成のソースの子だけで、
// 合成のソースがない間と、枠の外の経路では渡さない (呼び出し元が捨てて数える) ことを確認する。

#include "doctest.h"

#include "pipeline/frame_delivery_plan.h"

using cordmux::pipeline::frame_deliverable;

TEST_CASE("合成のソースがあれば、枠 1 から 4 の経路のフレームを渡す")
{
	for (int slot = 1; slot <= 4; ++slot)
		CHECK(frame_deliverable(/*composite_present=*/true, slot));
}

TEST_CASE("合成のソースがない間は、どの枠の経路のフレームも渡さない")
{
	for (int slot = 0; slot <= 4; ++slot)
		CHECK_FALSE(frame_deliverable(/*composite_present=*/false, slot));
}

TEST_CASE("枠から外した人の経路 (枠 0) のフレームは、合成のソースがあっても渡さない")
{
	CHECK_FALSE(frame_deliverable(/*composite_present=*/true, 0));
	CHECK_FALSE(frame_deliverable(/*composite_present=*/true, -1));
}
