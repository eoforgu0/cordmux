// SPDX-License-Identifier: Apache-2.0

// sources/composite_input.h (合成のソースに渡す配置の入力の作り方) のテスト。見えている枠と人の情報の対応、範囲の外や
// 重複した枠の読み飛ばし、plan_composite_layout に渡す値の作り方、タイルごとの声を聞けるか (話していない人を暗くするかの
// 判断に使う)、ダミー映像の間の配置 (自分の枠を描かない) を確認する。

#include "doctest.h"

#include "sources/composite_input.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using cordmux::sources::composite_tile_hears_voice;
using cordmux::sources::CompositeInput;
using cordmux::sources::CompositeLayout;
using cordmux::sources::CompositeLayoutRequest;
using cordmux::sources::CompositeRosterPerson;
using cordmux::sources::CompositeTile;
using cordmux::sources::make_composite_input;
using cordmux::sources::make_layout_request;
using cordmux::sources::plan_composite_layout;
using cordmux::sources::SlotPerson;

namespace {

SlotPerson person(int slot, const std::string &name, const std::string &user_id)
{
	SlotPerson p;
	p.slot = slot;
	p.name = name;
	p.user_id = user_id;
	return p;
}

} // namespace

TEST_CASE("見えている枠ごとに、枠に載っている人の表示名と user_id を添える")
{
	const CompositeInput input = make_composite_input({2, 1}, {person(1, "Bob", "u1"), person(2, "Alice", "u2")}, false, true, 0);
	REQUIRE(input.shares.size() == 2);
	CHECK(input.shares[0].slot == 2);
	CHECK(input.shares[0].name == "Alice");
	CHECK(input.shares[0].user_id == "u2");
	CHECK(input.shares[1].slot == 1);
	CHECK(input.shares[1].name == "Bob");
	CHECK(input.shares[1].user_id == "u1");
}

TEST_CASE("見えていない枠の人は入れず、人の情報がない見えている枠は名前も user_id も空で入れる")
{
	const CompositeInput input =
		make_composite_input({3}, {person(1, "Bob", "u1"), person(4, "Carol", "u4")}, false, true, 0);
	REQUIRE(input.shares.size() == 1);
	CHECK(input.shares[0].slot == 3);
	CHECK(input.shares[0].name.empty());
	CHECK(input.shares[0].user_id.empty());
}

TEST_CASE("範囲の外の枠の番号と、同じ番号の 2 つ目以降は読み飛ばす")
{
	const CompositeInput input =
		make_composite_input({0, 2, -1, 2, 5, 4}, {person(2, "A", "u2"), person(4, "B", "u4")}, false, true, 0);
	REQUIRE(input.shares.size() == 2);
	CHECK(input.shares[0].slot == 2);
	CHECK(input.shares[1].slot == 4);
}

TEST_CASE("同じ枠の人の情報が複数あれば、先のものを使う")
{
	const CompositeInput input = make_composite_input({1}, {person(1, "First", "a"), person(1, "Second", "b")}, false,
							  true, 0);
	REQUIRE(input.shares.size() == 1);
	CHECK(input.shares[0].name == "First");
	CHECK(input.shares[0].user_id == "a");
}

TEST_CASE("自分の枠、全面にするか、すき間は、そのまま入力に入る")
{
	const CompositeInput input = make_composite_input({}, {}, true, false, 12);
	CHECK(input.shares.empty());
	CHECK(input.self_shown);
	CHECK_FALSE(input.full_when_single);
	CHECK(input.gap_px == 12);
}

TEST_CASE("キャンバスの大きさを足すと、plan_composite_layout に渡す値になる")
{
	const CompositeInput input = make_composite_input({1, 2}, {person(1, "B", "u1"), person(2, "A", "u2")}, true, true, 4);
	const CompositeLayoutRequest request = make_layout_request(input, 1920, 1080);
	CHECK(request.canvas_w == 1920);
	CHECK(request.canvas_h == 1080);
	CHECK(request.self_shown);
	CHECK(request.full_when_single);
	CHECK(request.gap_px == 4);
	REQUIRE(request.shares.size() == 2);
	CHECK(request.shares[0].slot == 1);
	CHECK(request.shares[1].slot == 2);
}

TEST_CASE("作った入力から配置を決めると、表示名の順に左上から並ぶ")
{
	// 枠 3 の Alice、枠 1 の Bob。自分の枠はない。表示名の順に左上、右上。
	const CompositeInput input =
		make_composite_input({1, 3}, {person(1, "Bob", "u1"), person(3, "Alice", "u3")}, false, true, 0);
	const CompositeLayout layout = plan_composite_layout(make_layout_request(input, 1920, 1080));
	REQUIRE(layout.tiles.size() == 2);
	CHECK(layout.tiles[0].slot == 3);
	CHECK(layout.tiles[0].rect.x == 0);
	CHECK(layout.tiles[0].rect.y == 0);
	CHECK(layout.tiles[1].slot == 1);
	CHECK(layout.tiles[1].rect.x == 960);
	CHECK(layout.tiles[1].rect.y == 0);
	CHECK(layout.dropped.empty());
}

TEST_CASE("声を聞けるかの既定は、画面共有の人と画面共有していない参加者が真、自分の枠の人が偽")
{
	const CompositeInput input = make_composite_input({1}, {person(1, "Bob", "u1")}, true, true, 0);
	REQUIRE(input.shares.size() == 1);
	CHECK(input.shares[0].hears_voice);
	CHECK_FALSE(input.self_hears_voice);
	CHECK(input.roster.empty());
	CHECK(CompositeRosterPerson{}.hears_voice);
	// 人の情報がない見えている枠も真にする。
	const CompositeInput unknown = make_composite_input({2}, {}, false, true, 0);
	REQUIRE(unknown.shares.size() == 1);
	CHECK(unknown.shares[0].hears_voice);
}

TEST_CASE("枠の人の声を聞けるかは、そのまま配置の入力に入る")
{
	SlotPerson unheard = person(2, "Alice", "u2");
	unheard.hears_voice = false;
	const CompositeInput input = make_composite_input({1, 2}, {person(1, "Bob", "u1"), unheard}, false, true, 0);
	REQUIRE(input.shares.size() == 2);
	CHECK(input.shares[0].hears_voice);
	CHECK_FALSE(input.shares[1].hears_voice);
}

TEST_CASE("タイルごとの声を聞けるかは、自分の枠は入力の値、画面共有の枠は同じ番号の枠の値で、見つからなければ偽")
{
	SlotPerson unheard = person(3, "Alice", "u3");
	unheard.hears_voice = false;
	CompositeInput input = make_composite_input({1, 3}, {person(1, "Bob", "u1"), unheard}, true, true, 0);
	const CompositeLayout layout = plan_composite_layout(make_layout_request(input, 1920, 1080));
	REQUIRE(layout.tiles.size() == 3); // 自分、Alice (枠 3)、Bob (枠 1)
	CHECK(composite_tile_hears_voice(input, layout.tiles) == std::vector<bool>{false, false, true});
	input.self_hears_voice = true;
	CHECK(composite_tile_hears_voice(input, layout.tiles) == std::vector<bool>{true, false, true});

	// 入力にない枠のタイル (通常は起きない) は、暗くしない側に倒す。
	CompositeTile stray;
	stray.slot = 4;
	CHECK(composite_tile_hears_voice(input, {stray}) == std::vector<bool>{false});
	CHECK(composite_tile_hears_voice(input, {}).empty());
}

TEST_CASE("タイルごとの描く人は、自分の枠は自分の枠の人、画面共有の枠は同じ番号の枠の人、全画面だけで映す人の子は全画面の人")
{
	using cordmux::sources::composite_tile_user_ids;
	CompositeInput input = make_composite_input({1, 3}, {person(1, "Bob", "u1"), person(3, "Alice", "u3")}, true, true, 0);
	input.self_user_id = "me";
	const CompositeLayout layout = plan_composite_layout(make_layout_request(input, 1920, 1080));
	REQUIRE(layout.tiles.size() == 3); // 自分、Alice (枠 3)、Bob (枠 1)
	CHECK(composite_tile_user_ids(input, layout.tiles) == std::vector<std::string>{"me", "u3", "u1"});

	// 枠に載っている人の全画面 (1 枚を全面に描く) は、その人の枠のタイル。
	input.focus_user_id = "u1";
	const CompositeLayout focused = plan_composite_layout(make_layout_request(input, 1920, 1080));
	REQUIRE(focused.tiles.size() == 1);
	CHECK(composite_tile_user_ids(input, focused.tiles) == std::vector<std::string>{"u1"});

	// 枠に載せていない人の全画面は、全画面だけで映す人の子 (kFocusOnlySlot) に描くので、全画面の人。
	CompositeTile only;
	only.slot = cordmux::config::kFocusOnlySlot;
	input.focus_user_id = "u9";
	CHECK(composite_tile_user_ids(input, {only}) == std::vector<std::string>{"u9"});
	// 入力にない枠のタイルは空。
	CompositeTile stray;
	stray.slot = 4;
	CHECK(composite_tile_user_ids(input, {stray}) == std::vector<std::string>{""});
}

// ---- ダミー映像の間の配置 (dummy_composite_input) ----

namespace {

// ドックがダミー映像の間に作る、4 つの枠のダミーの人 (dock/dock_poll_layout.cpp の push_composite_input と同じ形。載せた順は
// 枠の番号。user_id は、枠に人が載っていればその人、いなければダミーの人)。
std::vector<SlotPerson> dummy_people(const std::vector<std::string> &tile_user_ids)
{
	std::vector<SlotPerson> out;
	for (int slot = 1; slot <= 4; ++slot) {
		SlotPerson p = person(slot, "ダミー " + std::to_string(slot), tile_user_ids[static_cast<std::size_t>(slot - 1)]);
		p.added_order = slot;
		out.push_back(p);
	}
	return out;
}

// 自分の枠の条件をドックと同じ欄に入れる (dock/dock_poll_layout.cpp の push_composite_input が、配置の入力を作った後に
// 入れる値)。
void set_self(CompositeInput *input, bool first, cordmux::config::SlotOrder order, const std::string &name,
	      std::int64_t mark)
{
	input->self_first = first;
	input->slot_order = order;
	input->self_user_id = "main";
	input->self_name = name;
	input->self_added_mark = mark;
	input->self_hears_voice = true;
}

} // namespace

TEST_CASE("ダミー映像の間は、自分の枠の条件のどの組み合わせでも、4 つのダミーだけを、自分の枠がないときと同じ配置で描く")
{
	using cordmux::config::SlotOrder;
	using cordmux::sources::composite_focus_tiles;
	using cordmux::sources::CompositeFocusTile;
	using cordmux::sources::dummy_composite_input;
	// 枠 2 にメインアカウントが Discord の画面共有で載っている形と、誰も載っていない形。
	for (const std::vector<std::string> &owners :
	     {std::vector<std::string>{"dummy:1", "dummy:2", "dummy:3", "dummy:4"},
	      std::vector<std::string>{"u1", "main", "dummy:3", "u4"}}) {
		const std::vector<SlotPerson> people = dummy_people(owners);
		// 自分の枠がないときの配置 (比べる元)。
		const CompositeLayout without_self =
			plan_composite_layout(make_layout_request(make_composite_input({1, 2, 3, 4}, people, false, true, 8), 1920, 1080));
		REQUIRE(without_self.tiles.size() == 4);
		// 自分の枠の条件: [先頭に配置] の有無、並び順、名前の比べ方 (先頭に来る名前と後ろに来る名前)、載せた順の印。
		for (const bool first : {true, false}) {
			for (const SlotOrder order : {SlotOrder::Name, SlotOrder::Added}) {
				for (const std::string &name : {std::string("aaa"), std::string("zzz"), std::string()}) {
					for (const std::int64_t mark : {std::int64_t{0}, std::int64_t{1000}}) {
						CAPTURE(first);
						CAPTURE(static_cast<int>(order));
						CAPTURE(name);
						CAPTURE(mark);
						CompositeInput input = dummy_composite_input(people, true, 8);
						set_self(&input, first, order, name, mark);
						CHECK_FALSE(input.self_shown);
						const CompositeLayout layout = plan_composite_layout(make_layout_request(input, 1920, 1080));
						REQUIRE(layout.tiles.size() == 4);
						CHECK(layout.dropped.empty());
						CompositeInput plain = make_composite_input({1, 2, 3, 4}, people, false, true, 8);
						plain.slot_order = order;
						const CompositeLayout expected =
							plan_composite_layout(make_layout_request(plain, 1920, 1080));
						for (std::size_t i = 0; i < layout.tiles.size(); ++i) {
							CHECK_FALSE(layout.tiles[i].self);
							CHECK(layout.tiles[i].slot == expected.tiles[i].slot);
							CHECK(layout.tiles[i].rect.x == expected.tiles[i].rect.x);
							CHECK(layout.tiles[i].rect.y == expected.tiles[i].rect.y);
							CHECK(layout.tiles[i].rect.w == expected.tiles[i].rect.w);
							CHECK(layout.tiles[i].rect.h == expected.tiles[i].rect.h);
						}
						// 全画面にできるのも 4 つのダミーだけで、自分の枠はない (枠 2 のダミーは、そこに載っている
						// メインアカウントの user_id を持つが、自分の枠の名前ではない)。
						const std::vector<CompositeFocusTile> focus = composite_focus_tiles(input);
						CHECK(focus.size() == 4);
						for (const CompositeFocusTile &tile : focus)
							CHECK_FALSE((tile.user_id == "main" && tile.name == name));
					}
				}
			}
		}
	}
}

TEST_CASE("ダミー映像をやめると、自分の枠を出していれば、ダミーの前と同じ配置に戻る")
{
	using cordmux::config::SlotOrder;
	using cordmux::sources::dummy_composite_input;
	// 自分の枠と 3 人。[先頭に配置] を外し、自分の枠の名前が最後に並ぶ形でも、自分の枠は描かれる。
	const std::vector<SlotPerson> people = {person(1, "Bob", "u1"), person(2, "Alice", "u2"), person(3, "Carol", "u3")};
	for (const bool first : {true, false}) {
		CAPTURE(first);
		CompositeInput before = make_composite_input({1, 2, 3}, people, true, true, 0);
		set_self(&before, first, SlotOrder::Name, "zzz", 1000);
		const CompositeLayout layout_before = plan_composite_layout(make_layout_request(before, 1920, 1080));
		REQUIRE(layout_before.tiles.size() == 4);
		CHECK(std::count_if(layout_before.tiles.begin(), layout_before.tiles.end(),
				    [](const CompositeTile &tile) { return tile.self; }) == 1);

		// ダミーの間は自分の枠を描かない。
		CompositeInput during = dummy_composite_input(dummy_people({"u1", "u2", "u3", "dummy:4"}), true, 0);
		set_self(&during, first, SlotOrder::Name, "zzz", 1000);
		const CompositeLayout layout_during = plan_composite_layout(make_layout_request(during, 1920, 1080));
		CHECK(std::none_of(layout_during.tiles.begin(), layout_during.tiles.end(),
				   [](const CompositeTile &tile) { return tile.self; }));

		// やめた後は、同じ自分の枠の条件 (ドックはダミーの間に変えない) から作るので、前と同じ配置になる。
		CompositeInput after = make_composite_input({1, 2, 3}, people, true, true, 0);
		set_self(&after, first, SlotOrder::Name, "zzz", 1000);
		const CompositeLayout layout_after = plan_composite_layout(make_layout_request(after, 1920, 1080));
		REQUIRE(layout_after.tiles.size() == layout_before.tiles.size());
		for (std::size_t i = 0; i < layout_after.tiles.size(); ++i) {
			CHECK(layout_after.tiles[i].self == layout_before.tiles[i].self);
			CHECK(layout_after.tiles[i].slot == layout_before.tiles[i].slot);
			CHECK(layout_after.tiles[i].rect.x == layout_before.tiles[i].rect.x);
			CHECK(layout_after.tiles[i].rect.y == layout_before.tiles[i].rect.y);
		}
	}
}
