// SPDX-License-Identifier: Apache-2.0

// pipeline/pipeline.h の純粋関数 retain_frame_clocks、collect_slot_snapshots の単体テスト。
// どれも Pipeline::snapshot()、handle_configure とドックが使う処理で、
// pipeline.cpp は libobs に依存してこのテストに入らないので、ここで確認する。取り除き忘れた行があると、その人の行に
// 「映像が届いていません」(cordmux.dock.row_not_receiving.status) が表示され続ける。並び順は、ドックが人の一覧を
// 作る順になる。

#include "doctest.h"

#include "pipeline/pipeline.h"

#include <map>
#include <set>
#include <string>
#include <vector>

using cordmux::pipeline::retain_frame_clocks;
using cordmux::pipeline::StreamFrameClock;

namespace {

StreamFrameClock clock_of(std::uint32_t stream_id, const std::string &user_id)
{
	StreamFrameClock c;
	c.stream_id = stream_id;
	c.user_id = user_id;
	c.slot = 1;
	c.last_video_frame_steady_us = 1000;
	c.last_video_frame_wall_us = 2000;
	return c;
}

std::map<std::uint32_t, StreamFrameClock> three_clocks()
{
	std::map<std::uint32_t, StreamFrameClock> clocks;
	clocks[1] = clock_of(1, "100");
	clocks[2] = clock_of(2, "200");
	clocks[7] = clock_of(7, "700");
	return clocks;
}

} // namespace

TEST_CASE("経路がなくなったストリームの行は取り除かれる")
{
	auto clocks = three_clocks();
	retain_frame_clocks(clocks, {1, 7});
	CHECK(clocks.size() == 2);
	CHECK(clocks.count(1) == 1);
	CHECK(clocks.count(7) == 1);
	CHECK(clocks.count(2) == 0);
	// 残った行の中身は変わらない (取り除くだけで、値には触らない)。
	CHECK(clocks.at(7).user_id == "700");
	CHECK(clocks.at(7).last_video_frame_steady_us == 1000);
}

TEST_CASE("生きている経路が 1 つもなければ、すべての行が取り除かれる")
{
	auto clocks = three_clocks();
	retain_frame_clocks(clocks, {});
	CHECK(clocks.empty());
}

TEST_CASE("すべての行の経路が生きていれば、1 行も取り除かれない")
{
	auto clocks = three_clocks();
	retain_frame_clocks(clocks, {1, 2, 7});
	CHECK(clocks.size() == 3);
}

TEST_CASE("まだフレームが届いていない経路があっても、ほかの行は取り除かれない")
{
	auto clocks = three_clocks();
	// 9 は経路としては生きているが、まだフレームが届いていないので行がない。
	retain_frame_clocks(clocks, {1, 2, 7, 9});
	CHECK(clocks.size() == 3);
	CHECK(clocks.count(9) == 0);
}

TEST_CASE("空の表から取り除いても、空のまま")
{
	std::map<std::uint32_t, StreamFrameClock> clocks;
	retain_frame_clocks(clocks, {1, 2});
	CHECK(clocks.empty());
}

// ---- 割り当て表のコピー (snapshot() が作る並び) ----

TEST_CASE("割り当てのある枠だけを、画面の枠、声の枠の順に、それぞれ番号の順で並べる")
{
	cordmux::config::AllocationTable table;
	// 画面の枠を 2 つ、声の枠を 1 つ埋める (番号の小さい順に埋まる。config/allocation.h)。
	REQUIRE(table.try_assign(cordmux::config::SlotKind::Screen, "100").outcome ==
		cordmux::config::AssignOutcome::Assigned);
	REQUIRE(table.try_assign(cordmux::config::SlotKind::Screen, "200").outcome ==
		cordmux::config::AssignOutcome::Assigned);
	REQUIRE(table.try_assign(cordmux::config::SlotKind::Voice, "300").outcome ==
		cordmux::config::AssignOutcome::Assigned);

	const auto rows = cordmux::pipeline::collect_slot_snapshots(table);
	REQUIRE(rows.size() == 3);
	// 画面が先、その中は番号順。
	CHECK(rows[0].kind == cordmux::config::SlotKind::Screen);
	CHECK(rows[0].slot == 1);
	CHECK(rows[0].owner_user_id == "100");
	CHECK(rows[1].kind == cordmux::config::SlotKind::Screen);
	CHECK(rows[1].slot == 2);
	CHECK(rows[1].owner_user_id == "200");
	// 声はその後。
	CHECK(rows[2].kind == cordmux::config::SlotKind::Voice);
	CHECK(rows[2].slot == 1);
	CHECK(rows[2].owner_user_id == "300");
	// 出してよいかも入る (割り当てのない枠は行そのものがない)。音声と映像の 2 つ。声の枠は映像を運ばない。
	CHECK(rows[0].output_allowed);
	CHECK(rows[0].video_allowed);
	CHECK(rows[2].output_allowed);
	CHECK_FALSE(rows[2].video_allowed);
}

TEST_CASE("使う声を置き換えた枠の行は voice_user_id が使う声に選んだ人になり、枠に載っている人は変わらない")
{
	cordmux::config::AllocationTable table;
	REQUIRE(table.try_assign(cordmux::config::SlotKind::Voice, "300").outcome ==
		cordmux::config::AssignOutcome::Assigned);
	table.set_voice_source(cordmux::config::SlotKind::Voice, 1, "400");

	const auto rows = cordmux::pipeline::collect_slot_snapshots(table);
	REQUIRE(rows.size() == 1);
	CHECK(rows[0].owner_user_id == "300");
	CHECK(rows[0].voice_user_id == "400");
}

TEST_CASE("何も割り当てていない表からは、行を 1 つも作らない")
{
	cordmux::config::AllocationTable table;
	CHECK(cordmux::pipeline::collect_slot_snapshots(table).empty());
}
