// SPDX-License-Identifier: Apache-2.0

// ledger/slot_cells.h の plan_slot_cells と、配信の配置 (sources/composite_layout.h) が同じ位置を返すことの単体テスト。
// 2 つの並び順、自分の枠は左上、置ける数を超えた人、1 人だけのときの全面と、自分の枠を先頭に置かないとき (設定の
// self_slot_first が偽) の並びを確認する。

#include "doctest.h"

#include "ledger/slot_cells.h"
#include "sources/composite_input.h"
#include "sources/composite_layout.h"

#include <map>
#include <string>
#include <vector>

using cordmux::config::SlotOrder;
using cordmux::ledger::plan_slot_cells;
using cordmux::ledger::SlotCell;
using cordmux::ledger::SlotCellPlan;
using cordmux::ledger::SlotCellSelf;
using cordmux::ledger::SlotCellShare;

namespace {

SlotCellShare share(int slot, const std::string &name, std::int64_t added = 0)
{
	SlotCellShare s;
	s.slot = slot;
	s.name = name;
	s.user_id = name;
	s.added_order = added;
	return s;
}

// 表示名 -> 位置の番号。自分の枠は "self" にする。
std::map<std::string, int> cells_of(const std::vector<SlotCellShare> &shares, const SlotCellPlan &plan)
{
	std::map<std::string, int> out;
	for (const SlotCell &cell : plan.cells)
		out[cell.self ? std::string("self") : shares[static_cast<std::size_t>(cell.share)].name] = cell.cell;
	return out;
}

// 自分の枠を出し、先頭に置かない (first が偽) ときの置き方。名前は name、載せた順の印は mark。
SlotCellSelf self_in_order(const std::string &name, std::int64_t mark = 0)
{
	SlotCellSelf self;
	self.shown = true;
	self.first = false;
	self.name = name;
	self.user_id = name;
	self.added_mark = mark;
	return self;
}

} // namespace

TEST_CASE("名前順は表示名の自然順で左上から詰め、人が抜けると後ろが詰まる")
{
	std::vector<SlotCellShare> shares = {share(1, "c"), share(2, "a10"), share(3, "a2")};
	SlotCellPlan plan = plan_slot_cells(shares, false, SlotOrder::Name, true);
	CHECK(cells_of(shares, plan) == std::map<std::string, int>{{"a2", 0}, {"a10", 1}, {"c", 2}});

	shares.erase(shares.begin() + 2);
	plan = plan_slot_cells(shares, false, SlotOrder::Name, true);
	CHECK(cells_of(shares, plan) == std::map<std::string, int>{{"a10", 0}, {"c", 1}});
}

TEST_CASE("載せた順は番号の順に左上から詰め、同じ番号なら表示名の順にする")
{
	std::vector<SlotCellShare> shares = {share(1, "a", 30), share(2, "b", 10), share(3, "c", 20)};
	SlotCellPlan plan = plan_slot_cells(shares, false, SlotOrder::Added, true);
	CHECK(cells_of(shares, plan) == std::map<std::string, int>{{"b", 0}, {"c", 1}, {"a", 2}});

	// b が抜けると詰まる。
	shares.erase(shares.begin() + 1);
	plan = plan_slot_cells(shares, false, SlotOrder::Added, true);
	CHECK(cells_of(shares, plan) == std::map<std::string, int>{{"c", 0}, {"a", 1}});

	// 前のバージョンのファイルから戻した枠は 0 で並ぶ。
	std::vector<SlotCellShare> same = {share(2, "z", 0), share(1, "y", 0)};
	plan = plan_slot_cells(same, false, SlotOrder::Added, true);
	CHECK(cells_of(same, plan) == std::map<std::string, int>{{"y", 0}, {"z", 1}});
}

TEST_CASE("自分の枠はどの並び順でも左上")
{
	const std::vector<SlotCellShare> shares = {share(2, "b", 2), share(3, "a", 1)};
	for (const SlotOrder order : {SlotOrder::Name, SlotOrder::Added}) {
		const SlotCellPlan plan = plan_slot_cells(shares, true, order, true);
		CHECK(cells_of(shares, plan).at("self") == 0);
		REQUIRE_FALSE(plan.cells.empty());
		CHECK(plan.cells[0].self);
	}
	// 名前順と載せた順は右上から詰める。
	const SlotCellPlan named = plan_slot_cells(shares, true, SlotOrder::Name, true);
	CHECK(cells_of(shares, named) == std::map<std::string, int>{{"self", 0}, {"a", 1}, {"b", 2}});
}

TEST_CASE("1 人だけのときの全面は 2 つの並び順で同じ")
{
	const std::vector<SlotCellShare> shares = {share(3, "a")};
	for (const SlotOrder order : {SlotOrder::Name, SlotOrder::Added}) {
		SlotCellPlan plan = plan_slot_cells(shares, false, order, true);
		CHECK(plan.full);
		REQUIRE(plan.cells.size() == 1);
		CHECK(plan.cells[0].cell == 0);

		plan = plan_slot_cells({}, true, order, true);
		CHECK(plan.full);
		REQUIRE(plan.cells.size() == 1);
		CHECK(plan.cells[0].self);
	}
	// 全面にしないときは、左上の位置。
	CHECK_FALSE(plan_slot_cells(shares, false, SlotOrder::Name, false).full);
	CHECK(plan_slot_cells(shares, false, SlotOrder::Name, false).cells[0].cell == 0);
}

TEST_CASE("名前順と載せた順は、置ける数を超えた人を dropped に入れる")
{
	const std::vector<SlotCellShare> shares = {share(1, "a", 4), share(2, "b", 3), share(3, "c", 2), share(4, "d", 1)};
	SlotCellPlan plan = plan_slot_cells(shares, true, SlotOrder::Name, true);
	REQUIRE(plan.dropped.size() == 1);
	CHECK(shares[static_cast<std::size_t>(plan.dropped[0])].name == "d");

	plan = plan_slot_cells(shares, true, SlotOrder::Added, true);
	REQUIRE(plan.dropped.size() == 1);
	CHECK(shares[static_cast<std::size_t>(plan.dropped[0])].name == "a");
}

TEST_CASE("配信の配置は同じ位置に描き、入力の並び順と枠の番号をそのまま渡す")
{
	namespace src = cordmux::sources;
	std::vector<src::SlotPerson> people;
	for (int slot = 1; slot <= 3; ++slot) {
		src::SlotPerson person;
		person.slot = slot;
		person.name = std::string(1, static_cast<char>('d' - slot));
		person.user_id = person.name;
		person.added_order = 10 - slot;
		people.push_back(person);
	}
	src::CompositeInput input = src::make_composite_input({1, 3}, people, false, true, 0);
	input.slot_order = SlotOrder::Name;

	// 名前順: 枠 3 の人 (a) が左上、枠 1 の人 (c) が右上。枠 2 が空いていても詰める。
	src::CompositeLayout layout = src::plan_composite_layout(src::make_layout_request(input, 1920, 1080));
	REQUIRE(layout.tiles.size() == 2);
	CHECK(layout.tiles[0].slot == 3);
	CHECK(layout.tiles[0].rect.x == 0);
	CHECK(layout.tiles[0].rect.y == 0);
	CHECK(layout.tiles[1].slot == 1);
	CHECK(layout.tiles[1].rect.x == 960);
	CHECK(layout.tiles[1].rect.y == 0);

	// 載せた順: 枠 3 の人が先。
	input.slot_order = SlotOrder::Added;
	layout = src::plan_composite_layout(src::make_layout_request(input, 1920, 1080));
	REQUIRE(layout.tiles.size() == 2);
	CHECK(layout.tiles[0].slot == 3);
	CHECK(layout.tiles[1].slot == 1);
	CHECK(layout.tiles[1].rect.x == 960);

	// 描く人と描かない人の分け方も同じ関数で決める。
	src::CompositeInput four = src::make_composite_input({1, 2, 3, 4}, {}, true, true, 0);
	four.slot_order = SlotOrder::Name;
	src::CompositeShareSplit split = src::split_composite_shares(four.shares, true, four.slot_order);
	REQUIRE(split.dropped.size() == 1);
	CHECK(split.dropped[0].slot == 4);
	REQUIRE(split.placed.size() == 3);
	CHECK(split.placed[0].slot == 1);
}

TEST_CASE("メインアカウントの画面共有は、名前順と載せた順で先頭に置く")
{
	std::vector<SlotCellShare> shares = {share(1, "a", 1), share(2, "b", 2), share(3, "zed", 3)};
	shares[2].is_main = true;
	for (const SlotOrder order : {SlotOrder::Name, SlotOrder::Added}) {
		const SlotCellPlan plan = plan_slot_cells(shares, false, order, true);
		CHECK(cells_of(shares, plan) == std::map<std::string, int>{{"zed", 0}, {"a", 1}, {"b", 2}});
	}
	// 自分の枠があるときは、自分の枠の次。
	const SlotCellPlan with_self = plan_slot_cells(shares, true, SlotOrder::Name, true);
	CHECK(cells_of(shares, with_self) == std::map<std::string, int>{{"self", 0}, {"zed", 1}, {"a", 2}, {"b", 3}});
}

TEST_CASE("メインアカウントの画面共有は、配信の配置でも同じ位置に描く")
{
	namespace src = cordmux::sources;
	std::vector<src::SlotPerson> people;
	const char *names[] = {"a", "b", "zed"};
	for (int slot = 1; slot <= 3; ++slot) {
		src::SlotPerson person;
		person.slot = slot;
		person.name = names[slot - 1];
		person.user_id = person.name;
		person.added_order = slot;
		person.is_main = slot == 3;
		people.push_back(person);
	}
	src::CompositeInput input = src::make_composite_input({1, 2, 3}, people, false, true, 0);
	for (const SlotOrder order : {SlotOrder::Name, SlotOrder::Added}) {
		input.slot_order = order;
		const src::CompositeLayout layout = src::plan_composite_layout(src::make_layout_request(input, 1920, 1080));
		REQUIRE(layout.tiles.size() == 3);
		CHECK(layout.tiles[0].slot == 3);
		CHECK(layout.tiles[0].rect.x == 0);
		CHECK(layout.tiles[0].rect.y == 0);
		CHECK(layout.tiles[1].slot == 1);
		CHECK(layout.tiles[2].slot == 2);
	}
}

TEST_CASE("自分の枠を先頭に置く (既定) ときは、名前や載せた順の印によらず左上で、前と同じ並びになる")
{
	const std::vector<SlotCellShare> shares = {share(1, "a", 1), share(2, "b", 2)};
	SlotCellSelf self = self_in_order("zed", 5);
	self.first = true;
	for (const SlotOrder order : {SlotOrder::Name, SlotOrder::Added}) {
		const SlotCellPlan plan = plan_slot_cells(shares, self, order, true);
		CHECK(cells_of(shares, plan) == std::map<std::string, int>{{"self", 0}, {"a", 1}, {"b", 2}});
		CHECK(cells_of(shares, plan) == cells_of(shares, plan_slot_cells(shares, true, order, true)));
		CHECK_FALSE(plan.self_dropped);
	}
}

TEST_CASE("自分の枠を先頭に置かないとき、名前順では自分の枠の名前で、ほかの人と同じ並びに置く")
{
	const std::vector<SlotCellShare> shares = {share(1, "a"), share(2, "c")};
	// 名前が 2 人の間なので、2 番目 (右上)。
	SlotCellPlan plan = plan_slot_cells(shares, self_in_order("b"), SlotOrder::Name, true);
	CHECK(cells_of(shares, plan) == std::map<std::string, int>{{"a", 0}, {"self", 1}, {"c", 2}});
	// 名前がいちばん後なので、3 番目 (左下)。
	plan = plan_slot_cells(shares, self_in_order("zed"), SlotOrder::Name, true);
	CHECK(cells_of(shares, plan) == std::map<std::string, int>{{"a", 0}, {"c", 1}, {"self", 2}});
	// 名前がいちばん前なら、左上。
	plan = plan_slot_cells(shares, self_in_order("0"), SlotOrder::Name, true);
	CHECK(cells_of(shares, plan) == std::map<std::string, int>{{"self", 0}, {"a", 1}, {"c", 2}});
	// 自分の枠だけなら、先頭に置かなくても全面。
	plan = plan_slot_cells({}, self_in_order("zed"), SlotOrder::Name, true);
	REQUIRE(plan.cells.size() == 1);
	CHECK(plan.cells[0].self);
	CHECK(plan.full);
}

TEST_CASE("自分の枠を先頭に置かないとき、載せた順では、自分の枠を載せた時に載っていた人の後、後から載せた人の前に置く")
{
	const std::vector<SlotCellShare> shares = {share(1, "a", 1), share(2, "b", 2), share(3, "c", 3)};
	// 1 番と 2 番の人が載っていた時に付けた (印 2): 3 番目 (左下)。
	SlotCellPlan plan = plan_slot_cells(shares, self_in_order("zed", 2), SlotOrder::Added, true);
	CHECK(cells_of(shares, plan) == std::map<std::string, int>{{"a", 0}, {"b", 1}, {"self", 2}, {"c", 3}});
	// 誰よりも先に付けた (印 0。OBS を起動したときから付いている): 左上。
	plan = plan_slot_cells(shares, self_in_order("zed", 0), SlotOrder::Added, true);
	CHECK(cells_of(shares, plan) == std::map<std::string, int>{{"self", 0}, {"a", 1}, {"b", 2}, {"c", 3}});
	// 全員の後に付けた: 4 番目 (右下)。名前は見ない。
	plan = plan_slot_cells(shares, self_in_order("0", 3), SlotOrder::Added, true);
	CHECK(cells_of(shares, plan) == std::map<std::string, int>{{"a", 0}, {"b", 1}, {"c", 2}, {"self", 3}});
}

TEST_CASE("自分の枠を先頭に置かないとき、メインアカウントの画面共有も、ほかの人と同じ並びに置く")
{
	std::vector<SlotCellShare> shares = {share(1, "a", 2), share(2, "b", 3), share(3, "zed", 1)};
	shares[2].is_main = true;
	SlotCellSelf off;
	off.first = false;
	// 名前順: zed は最後。
	SlotCellPlan plan = plan_slot_cells(shares, off, SlotOrder::Name, true);
	CHECK(cells_of(shares, plan) == std::map<std::string, int>{{"a", 0}, {"b", 1}, {"zed", 2}});
	// 載せた順: zed を最初に載せたので左上。
	plan = plan_slot_cells(shares, off, SlotOrder::Added, true);
	CHECK(cells_of(shares, plan) == std::map<std::string, int>{{"zed", 0}, {"a", 1}, {"b", 2}});
	// 先頭に置く (既定) なら、どちらの並び順でも左上。
	SlotCellSelf on;
	for (const SlotOrder order : {SlotOrder::Name, SlotOrder::Added}) {
		plan = plan_slot_cells(shares, on, order, true);
		CHECK(cells_of(shares, plan).at("zed") == 0);
	}
}

TEST_CASE("自分の枠を先頭に置かないとき、置ける数を超えると、後ろになった自分の枠も描かない")
{
	const std::vector<SlotCellShare> shares = {share(1, "a", 1), share(2, "b", 2), share(3, "c", 3), share(4, "d", 4)};
	// 名前が最後なので 5 番目になり、描かない。画面共有は 4 人とも描く。
	SlotCellPlan plan = plan_slot_cells(shares, self_in_order("zed"), SlotOrder::Name, true);
	CHECK(plan.self_dropped);
	CHECK(plan.dropped.empty());
	CHECK(cells_of(shares, plan) == std::map<std::string, int>{{"a", 0}, {"b", 1}, {"c", 2}, {"d", 3}});
	// 名前が 3 番目なら自分の枠を描き、最後の人 (d) を描かない。
	plan = plan_slot_cells(shares, self_in_order("b0"), SlotOrder::Name, true);
	CHECK_FALSE(plan.self_dropped);
	REQUIRE(plan.dropped.size() == 1);
	CHECK(shares[static_cast<std::size_t>(plan.dropped[0])].name == "d");
	CHECK(cells_of(shares, plan).at("self") == 2);
	// 先頭に置く (既定) なら、自分の枠は描き、最後の人を描かない。
	SlotCellSelf first = self_in_order("zed");
	first.first = true;
	plan = plan_slot_cells(shares, first, SlotOrder::Name, true);
	CHECK_FALSE(plan.self_dropped);
	REQUIRE(plan.dropped.size() == 1);
	CHECK(shares[static_cast<std::size_t>(plan.dropped[0])].name == "d");
}

TEST_CASE("自分の枠を先頭に置かないときも、配信の配置と全画面にできるタイルは、行の位置と同じ位置にする")
{
	namespace src = cordmux::sources;
	std::vector<src::SlotPerson> people;
	const char *names[] = {"a", "c"};
	for (int slot = 1; slot <= 2; ++slot) {
		src::SlotPerson person;
		person.slot = slot;
		person.name = names[slot - 1];
		person.user_id = person.name;
		person.added_order = slot;
		people.push_back(person);
	}
	src::CompositeInput input = src::make_composite_input({1, 2}, people, true, true, 0);
	input.self_user_id = "me";
	input.self_name = "b";
	input.self_first = false;
	input.slot_order = SlotOrder::Name;

	// 行の位置と同じ関数の答え: a、自分の枠 (b)、c。
	std::vector<SlotCellShare> shares = {share(1, "a", 1), share(2, "c", 2)};
	const SlotCellPlan cells = plan_slot_cells(shares, self_in_order("b"), SlotOrder::Name, true);
	REQUIRE(cells_of(shares, cells).at("self") == 1);

	const src::CompositeLayout layout = src::plan_composite_layout(src::make_layout_request(input, 1920, 1080));
	REQUIRE(layout.tiles.size() == 3);
	CHECK(layout.tiles[0].slot == 1);
	CHECK(layout.tiles[1].self);
	CHECK(layout.tiles[1].rect.x == 960);
	CHECK(layout.tiles[1].rect.y == 0);
	CHECK(layout.tiles[2].slot == 2);
	CHECK(layout.tiles[2].rect.y == 540);

	const std::vector<src::CompositeFocusTile> tiles = src::composite_focus_tiles(input);
	REQUIRE(tiles.size() == 3);
	CHECK(tiles[1].user_id == "me");
	CHECK(tiles[1].cell == 1);

	// 先頭に置く (既定) と、自分の枠は左上。
	input.self_first = true;
	const src::CompositeLayout first = src::plan_composite_layout(src::make_layout_request(input, 1920, 1080));
	REQUIRE(first.tiles.size() == 3);
	CHECK(first.tiles[0].self);
	CHECK(first.tiles[0].rect.x == 0);
}
