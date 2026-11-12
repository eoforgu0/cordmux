// SPDX-License-Identifier: Apache-2.0

// record/record_black.h の単体テスト。欠落を黒埋め用のフレームで埋めるときの、並べる回数と端数の計算を確認する。

#include "doctest.h"

#include "record/record_black.h"

using cordmux::record::black_frames_to_ticks;
using cordmux::record::black_us_to_frames;
using cordmux::record::BlackPlan;
using cordmux::record::GapFill;
using cordmux::record::plan_black;
using cordmux::record::plan_black_frames;
using cordmux::record::to_gap_fill;

namespace {

constexpr int kTimescale = 60000; // 映像の timescale

} // namespace

TEST_CASE("60 fps でちょうど 3 秒の欠落は、黒埋め用のフレームのファイル 3 つで埋まり、端数が出ない")
{
	const BlackPlan plan = plan_black(3'000'000, 1'000'000, 60, 1, kTimescale);
	CHECK(plan.total_frames == 180);
	CHECK(plan.material_frames == 60);
	CHECK(plan.repeat_count == 3);
	CHECK(plan.remainder_ticks == 0);
	CHECK(plan.out_point_ticks == 0);
	CHECK(plan.exact);
	CHECK(plan.total_ticks == 180000); // 3 秒 x 60000
	CHECK(plan.timescale == kTimescale);
}

TEST_CASE("60 fps の端数は、フレーム単位の outpoint を持つ 1 行になる")
{
	// 2.5 秒 = 150 フレーム = 黒埋め用のフレームのファイル 2 つ + 30 フレーム。
	const BlackPlan plan = plan_black(2'500'000, 1'000'000, 60, 1, kTimescale);
	CHECK(plan.total_frames == 150);
	CHECK(plan.repeat_count == 2);
	CHECK(plan.remainder_ticks == 30 * 1000); // 30 フレーム x (60000/60)
	CHECK(plan.out_point_ticks == plan.remainder_ticks);
	CHECK(plan.exact);
}

TEST_CASE("59.94 fps の黒埋め用のフレームのファイル 1 つは、60 フレームで 1.001 秒になる")
{
	const std::int64_t material_us = 1'001'000;
	CHECK(black_us_to_frames(material_us, 60000, 1001) == 60);

	// 3.003 秒 = 180 フレーム = 黒埋め用のフレームのファイルちょうど 3 つ。
	const BlackPlan plan = plan_black(3'003'000, material_us, 60000, 1001, kTimescale);
	CHECK(plan.material_frames == 60);
	CHECK(plan.total_frames == 180);
	CHECK(plan.repeat_count == 3);
	CHECK(plan.remainder_ticks == 0);
	CHECK(plan.total_ticks == 180 * 1001);
}

TEST_CASE("59.94 fps の 1 フレームは、小数の秒ではなく timescale の 1001 単位で表す")
{
	CHECK(black_frames_to_ticks(1, 60000, 1001, kTimescale) == 1001);
	CHECK(black_frames_to_ticks(60, 60000, 1001, kTimescale) == 60060);
	CHECK(black_frames_to_ticks(1, 60, 1, kTimescale) == 1000);
	CHECK(black_frames_to_ticks(1, 30, 1, kTimescale) == 2000);
	CHECK(black_frames_to_ticks(0, 60, 1, kTimescale) == 0);
}

TEST_CASE("フレーム数は最も近い整数に丸めるので、欠落が毎回 1 フレーム短くならない")
{
	// 60 fps で 1 フレームは 16666.67 us。0.6 フレーム分の欠落は 1 フレームに丸める。
	CHECK(black_us_to_frames(10'000, 60, 1) == 1);
	// 0.4 フレーム分は 0 フレーム。
	CHECK(black_us_to_frames(6'000, 60, 1) == 0);
	// ちょうど 1 フレームは 1 フレーム。
	CHECK(black_us_to_frames(16'667, 60, 1) == 1);
}

TEST_CASE("丸めたかどうかを exact に持つ")
{
	CHECK(plan_black(1'000'000, 1'000'000, 60, 1, kTimescale).exact);
	CHECK_FALSE(plan_black(1'000'001, 1'000'000, 60, 1, kTimescale).exact);
	// 59.94 fps では、ちょうど 1 秒の欠落をフレーム数で表せない。
	CHECK_FALSE(plan_black(1'000'000, 1'001'000, 60000, 1001, kTimescale).exact);
}

TEST_CASE("黒埋め用のフレームのファイルより短い欠落は、端数の 1 行だけになる")
{
	const BlackPlan plan = plan_black(500'000, 1'000'000, 60, 1, kTimescale);
	CHECK(plan.repeat_count == 0);
	CHECK(plan.total_frames == 30);
	CHECK(plan.remainder_ticks == 30000);
}

TEST_CASE("欠落がないとき、黒埋め用のフレームがないとき、値が壊れているときも正しく扱う")
{
	const BlackPlan none = plan_black(0, 1'000'000, 60, 1, kTimescale);
	CHECK(none.repeat_count == 0);
	CHECK(none.total_frames == 0);
	CHECK(none.exact);

	// 黒埋め用のフレームが 1 つもない (Vesktop がクラッシュしたセッションなど)。長さだけは残す。
	const BlackPlan no_material = plan_black_frames(2'000'000, 0, 60, 1, kTimescale);
	CHECK(no_material.repeat_count == 0);
	CHECK(no_material.remainder_ticks == 0);
	CHECK(no_material.total_frames == 120);
	CHECK(no_material.total_ticks == 120000);

	const BlackPlan broken = plan_black(2'000'000, 1'000'000, 0, 0, kTimescale);
	CHECK(broken.total_frames == 0);
	CHECK(broken.repeat_count == 0);
}

TEST_CASE("録画ログに残す GapFill は、欠落の長さと埋め方を持つ")
{
	const BlackPlan plan = plan_black(2'500'000, 1'000'000, 60, 1, kTimescale);
	const GapFill fill = to_gap_fill(plan, 7'000'000, "e1");
	CHECK(fill.start_us == 7'000'000);
	CHECK(fill.duration_us == 2'500'000);
	CHECK(fill.repeat_count == 2);
	CHECK(fill.remainder_ticks == 30000);
	CHECK(fill.black_encoder_instance_id == "e1");

	// 黒埋め用のフレームがない欠落では ID が空になる (その人は ProRes で録画ファイルを作成する)。
	const GapFill missing = to_gap_fill(plan_black_frames(1'000'000, 0, 60, 1, kTimescale), 0, "");
	CHECK(missing.black_encoder_instance_id.empty());
	CHECK(missing.duration_us == 1'000'000);
}
