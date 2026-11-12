// SPDX-License-Identifier: Apache-2.0

// 一時的に 1 人を全画面で映す機能の純粋関数のテスト。全画面の人がいるときの合成の配置 (sources/composite_layout.h の
// plan_composite_layout)、全画面にできるタイル、全画面をやめる条件、ホットキーの位置から人を選ぶ規則
// (sources/composite_input.h) と、ダミー映像の表示の間のタイルの user_id (sources/dummy_feed.h) を確認する。

#include "doctest.h"

#include "config/allocation.h"
#include "sources/composite_input.h"
#include "sources/dummy_feed.h"
#include "sources/scene_names.h"

#include <string>
#include <vector>

using cordmux::sources::composite_focus_others;
using cordmux::sources::composite_focus_tiles;
using cordmux::sources::composite_tile_hears_voice;
using cordmux::sources::CompositeFocusOther;
using cordmux::sources::CompositeFocusTile;
using cordmux::sources::CompositeInput;
using cordmux::sources::CompositeLayout;
using cordmux::sources::CompositeLayoutRequest;
using cordmux::sources::CompositeShare;
using cordmux::sources::dummy_tile_user_id;
using cordmux::sources::focus_after_layout;
using cordmux::sources::focus_hotkey_target;
using cordmux::sources::FocusHotkeyTarget;
using cordmux::sources::make_layout_request;
using cordmux::sources::plan_composite_layout;
using cordmux::sources::split_composite_shares;

namespace {

CompositeShare share(int slot, const std::string &name, const std::string &user_id)
{
	CompositeShare s;
	s.slot = slot;
	s.name = name;
	s.user_id = user_id;
	return s;
}

// 自分の枠 (self、名前 Me) と 3 人 (Alice 枠 3、Bob 枠 1、Carol 枠 2) の入力。名前順なので、左上から自分、Alice、Bob、Carol。
CompositeInput four_tiles()
{
	CompositeInput input;
	input.self_shown = true;
	input.self_user_id = "self";
	input.self_name = "Me";
	input.gap_px = 8;
	input.shares = {share(1, "Bob", "u1"), share(2, "Carol", "u2"), share(3, "Alice", "u3")};
	return input;
}

CompositeLayout layout_of(const CompositeInput &input)
{
	return plan_composite_layout(make_layout_request(input, 1920, 1080));
}

bool full_canvas(const CompositeLayout &layout)
{
	return layout.tiles.size() == 1 && layout.tiles[0].rect.x == 0 && layout.tiles[0].rect.y == 0 &&
	       layout.tiles[0].rect.w == 1920 && layout.tiles[0].rect.h == 1080;
}

std::vector<std::string> ids_of(const std::vector<CompositeFocusTile> &tiles)
{
	std::vector<std::string> out;
	for (const CompositeFocusTile &tile : tiles)
		out.push_back(tile.user_id);
	return out;
}

} // namespace

// ---- 全画面の人がいるときの配置 ----

TEST_CASE("全画面: 全画面の人のタイルだけを、すき間なしでキャンバスの全面に描き、ほかのタイルは描かない")
{
	CompositeInput input = four_tiles();
	input.focus_user_id = "u2"; // Carol (右下)
	const CompositeLayout layout = layout_of(input);
	CHECK(layout.focused);
	REQUIRE(full_canvas(layout));
	CHECK_FALSE(layout.tiles[0].self);
	CHECK(layout.tiles[0].slot == 2);
	CHECK(layout.dropped.empty());
}

TEST_CASE("全画面: 自分の枠も全画面にできる")
{
	CompositeInput input = four_tiles();
	input.focus_user_id = "self";
	const CompositeLayout layout = layout_of(input);
	CHECK(layout.focused);
	REQUIRE(full_canvas(layout));
	CHECK(layout.tiles[0].self);
	CHECK(layout.tiles[0].slot == 0);
}

TEST_CASE("全画面: 1 つを全面に置かない設定でも、全画面の人は全面に描く")
{
	CompositeInput input;
	input.shares = {share(1, "Bob", "u1")};
	input.full_when_single = false;
	CHECK_FALSE(full_canvas(layout_of(input))); // 全画面でないときは 4 等分の左上
	input.focus_user_id = "u1";
	CHECK(full_canvas(layout_of(input)));
}

TEST_CASE("全画面: どの人をどの枠に載せているかと、描かない人 (dropped) は変わらない")
{
	// 自分の枠と 4 人で、名前順の最後の Dave は描かない (dropped)。全画面にしても、Dave は dropped のまま、ほかの人は
	// dropped に入らない (全画面で描かれていない枠の人は、画面共有していない参加者の並びに出る。
	// test_dock_composite_roster_people.cpp)。
	CompositeInput input = four_tiles();
	input.shares.push_back(share(4, "Dave", "u4"));
	const CompositeLayout before = layout_of(input);
	input.focus_user_id = "u3";
	const CompositeLayout after = layout_of(input);
	REQUIRE(before.dropped.size() == 1);
	REQUIRE(after.dropped.size() == 1);
	CHECK(after.dropped[0].user_id == "u4");
	CHECK(full_canvas(after));
	const auto split = split_composite_shares(input.shares, input.self_shown, input.slot_order);
	REQUIRE(split.dropped.size() == 1);
	CHECK(split.dropped[0].user_id == "u4");
	CHECK(split.placed.size() == 3);
}

TEST_CASE("全画面: 描かない人 (dropped) と、入力にいない人を全画面の人にしても、全画面でないときと同じに描く")
{
	CompositeInput input = four_tiles();
	input.shares.push_back(share(4, "Dave", "u4"));
	input.focus_user_id = "u4";
	const CompositeLayout dropped = layout_of(input);
	CHECK_FALSE(dropped.focused);
	CHECK(dropped.tiles.size() == 4);
	input.focus_user_id = "nobody";
	const CompositeLayout absent = layout_of(input);
	CHECK_FALSE(absent.focused);
	CHECK(absent.tiles.size() == 4);
}

TEST_CASE("全画面: 自分の枠の人が分からないときは、空の user_id を自分の枠に結び付けない")
{
	CompositeInput input = four_tiles();
	input.self_user_id.clear();
	input.focus_user_id = "self";
	CHECK_FALSE(layout_of(input).focused);
}

// ---- 全画面にできるタイル ----

TEST_CASE("全画面にできるタイル: 全画面でないときの配置の位置と名前で、左上から並ぶ")
{
	CompositeInput input = four_tiles();
	input.focus_user_id = "u2"; // 全画面の人がいても、位置は全画面でないときの配置で決める
	const std::vector<CompositeFocusTile> tiles = composite_focus_tiles(input);
	REQUIRE(tiles.size() == 4);
	CHECK(tiles[0].user_id == "self");
	CHECK(tiles[0].name == "Me");
	CHECK(tiles[0].cell == 0);
	CHECK(tiles[1].user_id == "u3");
	CHECK(tiles[1].name == "Alice");
	CHECK(tiles[1].cell == 1);
	CHECK(tiles[2].user_id == "u1");
	CHECK(tiles[2].cell == 2);
	CHECK(tiles[3].user_id == "u2");
	CHECK(tiles[3].cell == 3);
}

TEST_CASE("全画面にできるタイル: 描かない人と、人の情報がない枠と、自分の枠を出していないときの自分は入らない")
{
	CompositeInput input = four_tiles();
	input.shares.push_back(share(4, "Dave", "u4"));    // dropped
	input.shares[1].user_id.clear();                   // 人の情報がない枠 (Carol の位置)
	CHECK(ids_of(composite_focus_tiles(input)) == std::vector<std::string>{"self", "u3", "u1"});
	input.self_shown = false;
	const std::vector<CompositeFocusTile> tiles = composite_focus_tiles(input);
	CHECK(ids_of(tiles) == std::vector<std::string>{"u3", "u1", "u4"});
}

TEST_CASE("全画面にできるタイル: 1 つだけなら全面に置くので、位置は左上")
{
	CompositeInput input;
	input.shares = {share(3, "Alice", "u3")};
	const std::vector<CompositeFocusTile> tiles = composite_focus_tiles(input);
	REQUIRE(tiles.size() == 1);
	CHECK(tiles[0].cell == 0);
}

// ---- 全画面をやめる条件 ----

TEST_CASE("全画面をやめる: 全画面の人を描いている間と、ほかの人の出入りでは続ける")
{
	CompositeInput input = four_tiles();
	CHECK(focus_after_layout("u2", composite_focus_tiles(input), true) == "u2");
	input.shares.erase(input.shares.begin()); // Bob が画面共有をやめた
	CHECK(focus_after_layout("u2", composite_focus_tiles(input), true) == "u2");
	CHECK(focus_after_layout("", composite_focus_tiles(input), true).empty());
}

TEST_CASE("全画面をやめる: 全画面の人が画面共有をやめた、通話を抜けた、枠から外れた")
{
	// どれも、その人の画面共有が配置の入力から消える。
	CompositeInput input = four_tiles();
	input.shares.erase(input.shares.begin() + 1); // Carol (u2)
	CHECK(focus_after_layout("u2", composite_focus_tiles(input), true).empty());
}

TEST_CASE("全画面をやめる: 入れ替えで、全画面の人の枠に別の人が載った")
{
	CompositeInput input = four_tiles();
	input.shares[1] = share(2, "Erin", "u5"); // Carol の枠 2 に Erin
	CHECK(focus_after_layout("u2", composite_focus_tiles(input), true).empty());
	CHECK(focus_after_layout("u5", composite_focus_tiles(input), true) == "u5");
}

TEST_CASE("全画面をやめる: 自分の枠を消した")
{
	CompositeInput input = four_tiles();
	input.self_shown = false;
	CHECK(focus_after_layout("self", composite_focus_tiles(input), true).empty());
}

TEST_CASE("全画面をやめる: Vesktop との接続が切れて枠が空いた (自分の枠は残る)")
{
	CompositeInput input = four_tiles();
	input.shares.clear();
	CHECK(focus_after_layout("u1", composite_focus_tiles(input), true).empty());
	CHECK(focus_after_layout("self", composite_focus_tiles(input), true) == "self");
}

TEST_CASE("全画面をやめる: 自分の枠が出て、全画面の人が描かれなくなった (dropped になった)")
{
	CompositeInput input = four_tiles();
	input.self_shown = false;
	input.shares.push_back(share(4, "Dave", "u4"));
	CHECK(focus_after_layout("u4", composite_focus_tiles(input), true) == "u4");
	input.self_shown = true;
	CHECK(focus_after_layout("u4", composite_focus_tiles(input), true).empty());
}

TEST_CASE("全画面をやめる: cordmux を無効にした")
{
	const CompositeInput input = four_tiles();
	CHECK(focus_after_layout("u2", composite_focus_tiles(input), false).empty());
	CHECK(focus_after_layout("self", composite_focus_tiles(input), false).empty());
}

TEST_CASE("全画面をやめる: 合成のソースがない (全画面にできるタイルがない)")
{
	CHECK(focus_after_layout("u2", {}, true).empty());
}

TEST_CASE("全画面をやめる: ダミー映像の表示の間は、ダミーのタイルで同じように決める")
{
	// ダミーの間は 4 つの枠のすべてにダミーのタイルがあり、Bob (u1) は枠 1、Carol (u2) は枠 2 に載っている。
	CompositeInput dummy;
	const std::string owners[] = {"u1", "u2", "", ""};
	for (int slot = 1; slot <= 4; ++slot)
		dummy.shares.push_back(share(slot, "Dummy " + std::to_string(slot), dummy_tile_user_id(slot, owners[slot - 1])));
	CHECK(dummy.shares[2].user_id == cordmux::sources::dummy_person_id(3));
	// ダミーに切り替えても、全画面の人 (Carol) の枠のダミーを描くので、続ける。
	const std::vector<CompositeFocusTile> tiles = composite_focus_tiles(dummy);
	CHECK(focus_after_layout("u2", tiles, true) == "u2");
	CompositeInput drawn = dummy;
	drawn.focus_user_id = "u2";
	const CompositeLayout layout = layout_of(drawn);
	CHECK(full_canvas(layout));
	CHECK(layout.tiles[0].slot == 2);
	// 誰も載っていない枠のダミーも全画面にでき、ダミーを止めると、そのダミーを描かなくなるのでやめる。
	const std::string dummy3 = cordmux::sources::dummy_person_id(3);
	CHECK(focus_after_layout(dummy3, tiles, true) == dummy3);
	CompositeInput real;
	real.shares = {share(1, "Bob", "u1"), share(2, "Carol", "u2")};
	CHECK(focus_after_layout(dummy3, composite_focus_tiles(real), true).empty());
	CHECK(focus_after_layout("u2", composite_focus_tiles(real), true) == "u2");
	// 自分の枠を出していると 4 つ目のダミーは描かない。描かないダミーの全画面は、ほかの人と同じくやめる。
	dummy.self_shown = true;
	dummy.self_user_id = "self";
	const std::string dummy4 = cordmux::sources::dummy_person_id(4);
	CHECK(focus_after_layout(dummy4, composite_focus_tiles(dummy), true).empty());
}

// ---- ホットキーの位置から人を選ぶ ----

TEST_CASE("ホットキー: その位置に描いている人を全画面にする")
{
	const std::vector<CompositeFocusTile> tiles = composite_focus_tiles(four_tiles());
	const FocusHotkeyTarget top_right = focus_hotkey_target(tiles, 1, "");
	CHECK(top_right.act);
	CHECK(top_right.user_id == "u3");
	const FocusHotkeyTarget top_left = focus_hotkey_target(tiles, 0, "u3");
	CHECK(top_left.act);
	CHECK(top_left.user_id == "self"); // 全画面の人がいても、戻さずにそのまま替える
}

TEST_CASE("ホットキー: その位置の人が既に全画面なら、元に戻す (同じキーで切り替える)")
{
	const std::vector<CompositeFocusTile> tiles = composite_focus_tiles(four_tiles());
	const FocusHotkeyTarget again = focus_hotkey_target(tiles, 3, "u2");
	CHECK(again.act);
	CHECK(again.user_id.empty());
}

TEST_CASE("ホットキー: その位置が空なら何もしない")
{
	CompositeInput input = four_tiles();
	input.shares.pop_back(); // Alice (右上) がいない。Bob が右上、Carol が左下、右下は空
	const std::vector<CompositeFocusTile> tiles = composite_focus_tiles(input);
	CHECK_FALSE(focus_hotkey_target(tiles, 3, "").act);
	CHECK_FALSE(focus_hotkey_target(tiles, 3, "u2").act);
	CHECK_FALSE(focus_hotkey_target({}, 0, "").act);
	// 1 人だけで全面に置いているときは、左上のキーがその人。
	CompositeInput single;
	single.shares = {share(4, "Dave", "u4")};
	const std::vector<CompositeFocusTile> one = composite_focus_tiles(single);
	CHECK(focus_hotkey_target(one, 0, "").user_id == "u4");
	CHECK_FALSE(focus_hotkey_target(one, 3, "").act);
}

// ---- 枠に載せていない人の全画面 ----

TEST_CASE("全画面だけで映す人の子の番号と子の数は、sources と config で同じ値")
{
	static_assert(cordmux::sources::kFocusOnlySlot == cordmux::config::kFocusOnlySlot);
	static_assert(cordmux::sources::kScreenTileCount == cordmux::config::kScreenTileCount);
	static_assert(cordmux::config::kFocusOnlySlot == cordmux::config::kScreenSlotCount + 1);
	CHECK(cordmux::sources::kScreenTileCount == 5);
}

TEST_CASE("全画面: 配置に描いていない人は、その人の映像を持つ子に映像があるときだけ全面に描き、ないうちは全画面でないときと同じ")
{
	CompositeInput input = four_tiles();
	input.focus_user_id = "eve";
	input.focus_extra_slot = cordmux::config::kFocusOnlySlot;
	CompositeLayoutRequest request = make_layout_request(input, 1920, 1080);
	CHECK_FALSE(request.focus_extra_ready); // 描く側が入れる
	const CompositeLayout waiting = plan_composite_layout(request);
	CHECK_FALSE(waiting.focused);
	CHECK(waiting.tiles.size() == 4);

	request.focus_extra_ready = true;
	const CompositeLayout shown = plan_composite_layout(request);
	CHECK(shown.focused);
	REQUIRE(full_canvas(shown));
	CHECK(shown.tiles[0].slot == cordmux::config::kFocusOnlySlot);
	CHECK_FALSE(shown.tiles[0].self);

	// 全画面の人が配置に描かれているなら、その人のタイルを使う (子の値は見ない)。
	input.focus_user_id = "u2";
	request = make_layout_request(input, 1920, 1080);
	request.focus_extra_ready = true;
	const CompositeLayout in_cells = plan_composite_layout(request);
	REQUIRE(full_canvas(in_cells));
	CHECK(in_cells.tiles[0].slot == 2);
}

TEST_CASE("全画面にできる、配置に描いていない人は、画面共有している人から、描いている人と除く人を除いた人")
{
	const std::vector<CompositeFocusTile> tiles = composite_focus_tiles(four_tiles());
	const auto others = composite_focus_others(
		{"u1", "eve", "vesktop", "", "eve", "dan"}, tiles, {"vesktop"},
		[](const std::string &user_id) { return user_id == "eve" ? std::string("Eve") : std::string(); });
	REQUIRE(others.size() == 2);
	CHECK(others[0].user_id == "eve");
	CHECK(others[0].name == "Eve");
	CHECK(others[1].user_id == "dan");
}

TEST_CASE("全画面をやめる: 配置に描いていない人は、others にいる間 (画面共有を続ける間) だけ続け、others を渡さなければやめる")
{
	const std::vector<CompositeFocusTile> tiles = composite_focus_tiles(four_tiles());
	const std::vector<CompositeFocusOther> others = {CompositeFocusOther{"eve", "Eve"}};
	CHECK(focus_after_layout("eve", tiles, true, others) == "eve");
	CHECK(focus_after_layout("eve", tiles, true, {}).empty()); // 画面共有をやめた
	CHECK(focus_after_layout("eve", tiles, true).empty());     // 枠に載っている人の全画面として扱うとき
	CHECK(focus_after_layout("eve", tiles, false, others).empty()); // cordmux を無効にした
}

TEST_CASE("全画面だけで映す人の子は、声を聞けるものとして扱う (暗くしない人にしない)")
{
	CompositeInput input = four_tiles();
	cordmux::sources::CompositeTile extra;
	extra.slot = cordmux::config::kFocusOnlySlot;
	cordmux::sources::CompositeTile unknown;
	unknown.slot = 4; // shares にない枠
	const std::vector<bool> hears = composite_tile_hears_voice(input, {extra, unknown});
	REQUIRE(hears.size() == 2);
	CHECK(hears[0]);
	CHECK_FALSE(hears[1]);
}
