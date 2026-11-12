// SPDX-License-Identifier: Apache-2.0

// sources/composite_roster.h (画面共有していない参加者の名前ラベルを並べる純粋関数) のテスト。並べる順、名前の幅の上限、
// 配置 8 か所と向きの組み合わせ、折り返し (行と列)、揃え、入りきらない人の数、端の場合 (間隔 0、1 人、空)、合成のソースが
// 描く人 (user_id の重複と空を除く)、配置の位置に足すずれ (8 か所と、キャンバスの解像度によらないこと) を確認する。

#include "doctest.h"

#include "sources/composite_roster.h"

#include <cstddef>
#include <string>
#include <vector>

using cordmux::config::AvatarSide;
using cordmux::config::RosterAnchor;
using cordmux::config::RosterDirection;
using cordmux::sources::composite_roster_cap_width;
using cordmux::sources::composite_roster_drawn;
using cordmux::sources::composite_roster_gap_px;
using cordmux::sources::composite_roster_order;
using cordmux::sources::CompositeRosterBox;
using cordmux::sources::CompositeRosterPerson;
using cordmux::sources::CompositeRosterPlaced;
using cordmux::sources::CompositeRosterPlan;
using cordmux::sources::CompositeRosterRequest;
using cordmux::sources::plan_composite_roster;

namespace {

// 1920 x 1080 のキャンバスに、幅 w、高さ h の名前ラベルを count 人、余白 10、間隔 gap で並べる要求。
CompositeRosterRequest request_of(int count, int w, int h, RosterAnchor anchor, RosterDirection direction,
				  int gap = 8, AvatarSide side = AvatarSide::Left)
{
	CompositeRosterRequest request;
	request.canvas_w = 1920;
	request.canvas_h = 1080;
	request.margin_px = 10;
	request.gap_px = gap;
	request.anchor = anchor;
	request.direction = direction;
	request.avatar_side = side;
	for (int i = 0; i < count; ++i)
		request.boxes.push_back(CompositeRosterBox{w, h});
	return request;
}

const CompositeRosterPlaced &at(const CompositeRosterPlan &plan, std::size_t index)
{
	for (const CompositeRosterPlaced &placed : plan.placed)
		if (placed.index == index)
			return placed;
	FAIL("置かれていない人: " << index);
	return plan.placed.front();
}

} // namespace

TEST_CASE("並べる順は、表示名の自然順で、同じ名前なら user_id、それも同じなら渡された順")
{
	// 自然順: "user2" は "user10" より前。
	const std::vector<CompositeRosterPerson> people = {{"user10", "u1"}, {"user2", "u2"}, {"Alice", "u3"},
							   {"Bob", "u9"},    {"Bob", "u4"},  {"Carol", "u5"}};
	const std::vector<std::size_t> order = composite_roster_order(people);
	REQUIRE(order.size() == 6);
	// Alice、Bob (u4)、Bob (u9)、Carol、user2、user10 の順になる (画面共有の枠と同じ比べ方)。
	const std::vector<std::size_t> want = {2, 4, 3, 5, 1, 0};
	CHECK(order == want);

	// 名前も user_id も同じなら、渡された順のまま。
	const std::vector<CompositeRosterPerson> same = {{"X", "id"}, {"X", "id"}, {"X", "id"}};
	CHECK(composite_roster_order(same) == std::vector<std::size_t>{0, 1, 2});
	CHECK(composite_roster_order({}).empty());
}

TEST_CASE("合成のソースが描く人は並べる順で、user_id が空の人と、同じ user_id の 2 人目以降を除く")
{
	const std::vector<CompositeRosterPerson> people = {{"Carol", "u3"}, {"Alice", "u1"}, {"Nobody", ""},
							   {"Alice", "u1"}, {"Bob", "u2"},   {"Zed", "u1"}};
	// 並べる順は添字 1、3、4、0、2、5 (Alice、Alice、Bob、Carol、Nobody、Zed)。u1 は並べる順で最初の 1 人 (添字 1) だけを
	// 描き、user_id が空の Nobody は描かない。
	CHECK(composite_roster_drawn(people) == std::vector<std::size_t>{1, 4, 0});
	CHECK(composite_roster_drawn({}).empty());
	// 声を聞けるかは並びに影響しない。
	std::vector<CompositeRosterPerson> heard = {{"B", "b"}, {"A", "a"}};
	heard[0].hears_voice = false;
	CHECK(composite_roster_drawn(heard) == std::vector<std::size_t>{1, 0});
}

TEST_CASE("名前の幅の上限は、キャンバスの幅の半分 - 余白 x 2 で、0 以下にならない")
{
	CHECK(composite_roster_cap_width(1920, 10.8) == 938); // 960 - 21.6 = 938.4
	CHECK(composite_roster_cap_width(1920, 0.0) == 960);
	CHECK(composite_roster_cap_width(1921, 0.0) == 960); // 奇数の幅は切り捨ててから半分
	CHECK(composite_roster_cap_width(100, 60.0) == 0);
	CHECK(composite_roster_cap_width(0, 0.0) == 0);
	CHECK(composite_roster_cap_width(1920, -5.0) == 960); // 負の余白は 0 として読む
}

TEST_CASE("間隔は、フォントサイズに対する倍率を四捨五入したピクセル数")
{
	CHECK(composite_roster_gap_px(0.5, 23.76) == 12);
	CHECK(composite_roster_gap_px(1.0, 23.76) == 24);
	CHECK(composite_roster_gap_px(0.0, 23.76) == 0);
	CHECK(composite_roster_gap_px(0.5, 0.0) == 0);
	CHECK(composite_roster_gap_px(-1.0, 23.76) == 0);
}

TEST_CASE("入力が空、キャンバスが 0 以下、大きさのない箱だけのとき、何も置かず、数えない")
{
	CHECK(plan_composite_roster(request_of(0, 100, 30, RosterAnchor::BottomLeft, RosterDirection::Row)).placed.empty());
	CompositeRosterRequest request = request_of(3, 100, 30, RosterAnchor::BottomLeft, RosterDirection::Row);
	request.canvas_w = 0;
	CHECK(plan_composite_roster(request).placed.empty());
	CHECK(plan_composite_roster(request).not_placed == 0);
	CompositeRosterRequest zero = request_of(3, 0, 30, RosterAnchor::BottomLeft, RosterDirection::Row);
	const CompositeRosterPlan plan = plan_composite_roster(zero);
	CHECK(plan.placed.empty());
	CHECK(plan.not_placed == 0);
	// 大きさのない人は飛ばし、残りの人の添字は元のまま。
	CompositeRosterRequest mixed = request_of(3, 100, 30, RosterAnchor::TopLeft, RosterDirection::Row);
	mixed.boxes[1] = CompositeRosterBox{0, 0};
	const CompositeRosterPlan skipped = plan_composite_roster(mixed);
	REQUIRE(skipped.placed.size() == 2);
	CHECK(skipped.placed[0].index == 0);
	CHECK(skipped.placed[1].index == 2);
	CHECK(skipped.placed[1].x == 10 + 100 + 8);
}

TEST_CASE("1 人は、配置の端から余白だけ離れた所に置く (8 か所)")
{
	struct Case {
		RosterAnchor anchor;
		RosterDirection direction;
		int x;
		int y;
	};
	// 箱は 200 x 40。キャンバスは 1920 x 1080、余白は 10。
	const Case cases[] = {
		{RosterAnchor::TopLeft, RosterDirection::Row, 10, 10},
		{RosterAnchor::TopCenter, RosterDirection::Row, (1920 - 200) / 2, 10},
		{RosterAnchor::TopRight, RosterDirection::Row, 1920 - 10 - 200, 10},
		{RosterAnchor::MiddleLeft, RosterDirection::Column, 10, (1080 - 40) / 2},
		{RosterAnchor::MiddleRight, RosterDirection::Column, 1920 - 10 - 200, (1080 - 40) / 2},
		{RosterAnchor::BottomLeft, RosterDirection::Row, 10, 1080 - 10 - 40},
		{RosterAnchor::BottomCenter, RosterDirection::Row, (1920 - 200) / 2, 1080 - 10 - 40},
		{RosterAnchor::BottomRight, RosterDirection::Row, 1920 - 10 - 200, 1080 - 10 - 40},
	};
	for (const Case &c : cases) {
		CAPTURE(static_cast<int>(c.anchor));
		const CompositeRosterPlan plan = plan_composite_roster(request_of(1, 200, 40, c.anchor, c.direction));
		REQUIRE(plan.placed.size() == 1);
		CHECK(plan.not_placed == 0);
		CHECK(plan.placed[0].index == 0);
		CHECK(plan.placed[0].x == c.x);
		CHECK(plan.placed[0].y == c.y);
	}
}

TEST_CASE("配置に合わない並びは、配置に合う並びに修正して並べる")
{
	// 上と下の中央に縦並びを渡しても横に並ぶ。
	const CompositeRosterPlan top = plan_composite_roster(request_of(3, 100, 30, RosterAnchor::TopCenter, RosterDirection::Column));
	REQUIRE(top.placed.size() == 3);
	CHECK(at(top, 0).y == at(top, 1).y);
	CHECK(at(top, 1).y == at(top, 2).y);
	CHECK(at(top, 1).x == at(top, 0).x + 100 + 8);
	// 左と右の中央に横並びを渡しても縦に並ぶ。
	const CompositeRosterPlan mid = plan_composite_roster(request_of(3, 100, 30, RosterAnchor::MiddleLeft, RosterDirection::Row));
	REQUIRE(mid.placed.size() == 3);
	CHECK(at(mid, 0).x == at(mid, 1).x);
	CHECK(at(mid, 1).y == at(mid, 0).y + 30 + 8);
}

TEST_CASE("横並びは、左から右へ間隔を空けて並び、揃えは左の配置が左、中央が中央、右が右")
{
	// 3 人 (幅 100、間隔 8) の行の幅は 316。
	const CompositeRosterPlan left = plan_composite_roster(request_of(3, 100, 30, RosterAnchor::TopLeft, RosterDirection::Row));
	CHECK(at(left, 0).x == 10);
	CHECK(at(left, 1).x == 118);
	CHECK(at(left, 2).x == 226);
	const CompositeRosterPlan center = plan_composite_roster(request_of(3, 100, 30, RosterAnchor::TopCenter, RosterDirection::Row));
	CHECK(at(center, 0).x == (1920 - 316) / 2);
	CHECK(at(center, 1).x == (1920 - 316) / 2 + 108);
	const CompositeRosterPlan right = plan_composite_roster(request_of(3, 100, 30, RosterAnchor::TopRight, RosterDirection::Row));
	// 右揃えでも、行の中の順は左から右 (先頭の人が左)。
	CHECK(at(right, 2).x == 1920 - 10 - 100);
	CHECK(at(right, 1).x == 1920 - 10 - 100 - 108);
	CHECK(at(right, 0).x == 1920 - 10 - 316);
}

TEST_CASE("横並びは、行の幅がキャンバスの幅 - 余白 x 2 を超えると次の行へ折り返し、行は中央の側へ足す")
{
	// 使える幅は 1900。幅 300、間隔 8 なら 1 行に 6 人 (6 x 300 + 5 x 8 = 1840)。7 人目で折り返す。
	const CompositeRosterPlan top = plan_composite_roster(request_of(8, 300, 40, RosterAnchor::TopLeft, RosterDirection::Row));
	REQUIRE(top.placed.size() == 8);
	CHECK(top.not_placed == 0);
	for (std::size_t i = 0; i < 6; ++i) {
		CHECK(at(top, i).y == 10);
		CHECK(at(top, i).x == 10 + static_cast<int>(i) * 308);
	}
	// 上の配置は、2 行目が下に足される。
	CHECK(at(top, 6).y == 10 + 40 + 8);
	CHECK(at(top, 6).x == 10);
	CHECK(at(top, 7).x == 10 + 308);

	// 下の配置は、まとまりが下の端に寄り、読む順 (上から下) のまま 1 行目が上、2 行目が下になる。まとまりは上へ伸びる。
	const CompositeRosterPlan bottom = plan_composite_roster(request_of(8, 300, 40, RosterAnchor::BottomLeft, RosterDirection::Row));
	CHECK(at(bottom, 6).y == 1080 - 10 - 40);
	CHECK(at(bottom, 0).y == 1080 - 10 - 40 - 8 - 40);
	CHECK(at(bottom, 6).x == 10);

	// 行ごとに揃える: 中央揃えなら、短い 2 行目は 2 人分の幅で中央に寄る。
	const CompositeRosterPlan center = plan_composite_roster(request_of(8, 300, 40, RosterAnchor::TopCenter, RosterDirection::Row));
	CHECK(at(center, 0).x == (1920 - 1840) / 2);
	CHECK(at(center, 6).x == (1920 - 608) / 2);
	// 右揃えなら、2 行目の右端が右の余白に揃う。
	const CompositeRosterPlan right = plan_composite_roster(request_of(8, 300, 40, RosterAnchor::TopRight, RosterDirection::Row));
	CHECK(at(right, 7).x + 300 == 1920 - 10);
	CHECK(at(right, 5).x + 300 == 1920 - 10);
}

TEST_CASE("縦並びは、上から下へ並び、高さが足りなければ次の列へ移り、列は中央の側へ足す")
{
	// 使える高さは 1060。高さ 40、間隔 8 なら 1 列に 22 人 (22 x 40 + 21 x 8 = 1048)。
	const CompositeRosterPlan left = plan_composite_roster(request_of(25, 100, 40, RosterAnchor::TopLeft, RosterDirection::Column));
	REQUIRE(left.placed.size() == 25);
	CHECK(at(left, 0).x == 10);
	CHECK(at(left, 0).y == 10);
	CHECK(at(left, 1).y == 10 + 48);
	CHECK(at(left, 21).y == 10 + 21 * 48);
	// 23 人目から次の列 (右へ足す)。
	CHECK(at(left, 22).x == 10 + 100 + 8);
	CHECK(at(left, 22).y == 10);

	// 右の配置は、まとまりが右の端に寄り、1 列目が左、2 列目が右になる (読む順 左から右)。
	const CompositeRosterPlan right = plan_composite_roster(request_of(25, 100, 40, RosterAnchor::TopRight, RosterDirection::Column));
	CHECK(at(right, 22).x + 100 == 1920 - 10);
	CHECK(at(right, 0).x == 1920 - 10 - 100 - 8 - 100);
}

TEST_CASE("縦並びの縦の揃えは、上の配置が上、下の配置が下、左右の中央の配置が中央")
{
	const CompositeRosterPlan top = plan_composite_roster(request_of(3, 100, 40, RosterAnchor::TopLeft, RosterDirection::Column));
	CHECK(at(top, 0).y == 10);
	CHECK(at(top, 2).y == 10 + 96);
	const CompositeRosterPlan bottom = plan_composite_roster(request_of(3, 100, 40, RosterAnchor::BottomLeft, RosterDirection::Column));
	// 列の高さは 3 x 40 + 2 x 8 = 136。最後の人が下の余白に接する。
	CHECK(at(bottom, 2).y + 40 == 1080 - 10);
	CHECK(at(bottom, 0).y == 1080 - 10 - 136);
	const CompositeRosterPlan middle = plan_composite_roster(request_of(3, 100, 40, RosterAnchor::MiddleLeft, RosterDirection::Column));
	CHECK(at(middle, 0).y == (1080 - 136) / 2);
	CHECK(at(middle, 2).y == (1080 - 136) / 2 + 96);
}

TEST_CASE("縦並びの列の中の揃えは、アバターが左なら左揃え、右なら右揃えで、列の幅はいちばん広い人の幅")
{
	CompositeRosterRequest request = request_of(3, 100, 40, RosterAnchor::TopLeft, RosterDirection::Column);
	request.boxes[0].w = 100;
	request.boxes[1].w = 160;
	request.boxes[2].w = 120;
	const CompositeRosterPlan left = plan_composite_roster(request);
	CHECK(at(left, 0).x == 10);
	CHECK(at(left, 1).x == 10);
	CHECK(at(left, 2).x == 10);

	request.avatar_side = AvatarSide::Right;
	const CompositeRosterPlan right = plan_composite_roster(request);
	// 列の幅 160 の右端 (10 + 160) に、全員の右端がそろう。
	CHECK(at(right, 0).x + 100 == 170);
	CHECK(at(right, 1).x + 160 == 170);
	CHECK(at(right, 2).x + 120 == 170);

	// 右の配置で右揃えなら、全員の右端がキャンバスの右の余白にそろう。
	request.anchor = RosterAnchor::TopRight;
	const CompositeRosterPlan far_right = plan_composite_roster(request);
	CHECK(at(far_right, 0).x + 100 == 1920 - 10);
	CHECK(at(far_right, 1).x + 160 == 1920 - 10);

	// 次の列の幅は、その列でいちばん広い人の幅 (列ごとに決める)。
	CompositeRosterRequest wrapped = request_of(24, 100, 40, RosterAnchor::TopLeft, RosterDirection::Column);
	wrapped.boxes[22].w = 150; // 2 列目 (23 人目から)
	wrapped.boxes[23].w = 120;
	wrapped.avatar_side = AvatarSide::Right;
	const CompositeRosterPlan two_columns = plan_composite_roster(wrapped);
	CHECK(at(two_columns, 22).x == 10 + 100 + 8);
	CHECK(at(two_columns, 23).x + 120 == 10 + 100 + 8 + 150);
}

TEST_CASE("入りきらない人は置かず、置かなかった人の数を返す (行と列)")
{
	// 横並び: 幅 900 の人は 1 行に 2 人 (1808)。高さ 300 で、行は 3 行 (3 x 300 + 2 x 8 = 916 <= 1060)、4 行目は 1228 で入らない。
	const CompositeRosterPlan row = plan_composite_roster(request_of(10, 900, 300, RosterAnchor::TopLeft, RosterDirection::Row));
	CHECK(row.placed.size() == 6);
	CHECK(row.not_placed == 4);
	// 置いたのは先頭の 6 人で、添字は 0 から 5。
	for (std::size_t i = 0; i < row.placed.size(); ++i)
		CHECK(row.placed[i].index == i);

	// 縦並び: 高さ 500 の人は 1 列に 2 人 (1008)。幅 700 で、列は 2 列 (1408 <= 1900)、3 列目は 2116 で入らない。
	const CompositeRosterPlan column = plan_composite_roster(request_of(7, 700, 500, RosterAnchor::TopLeft, RosterDirection::Column));
	CHECK(column.placed.size() == 4);
	CHECK(column.not_placed == 3);

	// 1 人でもキャンバスより高い人は置けず、全員が置けない。
	const CompositeRosterPlan too_tall = plan_composite_roster(request_of(3, 100, 2000, RosterAnchor::TopLeft, RosterDirection::Row));
	CHECK(too_tall.placed.empty());
	CHECK(too_tall.not_placed == 3);

	// 余白が大きすぎて使える幅がなければ、全員が置けない。
	CompositeRosterRequest no_room = request_of(3, 100, 30, RosterAnchor::TopLeft, RosterDirection::Row);
	no_room.margin_px = 1000;
	CHECK(plan_composite_roster(no_room).not_placed == 3);
}

TEST_CASE("間隔 0 のときは隙間なく並び、折り返しの行も隙間なく続く")
{
	const CompositeRosterPlan plan = plan_composite_roster(request_of(7, 300, 40, RosterAnchor::TopLeft, RosterDirection::Row, 0));
	REQUIRE(plan.placed.size() == 7);
	// 使える幅 1900: 6 人 (1800) まで。
	CHECK(at(plan, 1).x == 10 + 300);
	CHECK(at(plan, 5).x == 10 + 5 * 300);
	CHECK(at(plan, 6).x == 10);
	CHECK(at(plan, 6).y == 10 + 40);
}

TEST_CASE("1 人だけの箱が使える幅より広いときは、その人だけで 1 行にして置く")
{
	CompositeRosterRequest request = request_of(2, 100, 30, RosterAnchor::TopLeft, RosterDirection::Row);
	request.boxes[0].w = 2000;
	const CompositeRosterPlan plan = plan_composite_roster(request);
	REQUIRE(plan.placed.size() == 2);
	CHECK(at(plan, 0).x == 10);
	CHECK(at(plan, 1).x == 10);
	CHECK(at(plan, 1).y == 10 + 30 + 8);
}

TEST_CASE("人の数が増えても、置いた人の位置は箱の大きさとキャンバスだけで決まり、同じ入力は同じ結果")
{
	const CompositeRosterRequest request = request_of(8, 250, 36, RosterAnchor::BottomCenter, RosterDirection::Row);
	const CompositeRosterPlan a = plan_composite_roster(request);
	const CompositeRosterPlan b = plan_composite_roster(request);
	REQUIRE(a.placed.size() == b.placed.size());
	for (std::size_t i = 0; i < a.placed.size(); ++i) {
		CHECK(a.placed[i].index == b.placed[i].index);
		CHECK(a.placed[i].x == b.placed[i].x);
		CHECK(a.placed[i].y == b.placed[i].y);
	}
}

TEST_CASE("要求と並びの設定の既定は、右下の縦並びでアバターが右、間隔 0.25 で、ほかの物を避けず配置の端に置く")
{
	const CompositeRosterRequest request;
	CHECK(request.anchor == RosterAnchor::BottomRight);
	CHECK(request.direction == RosterDirection::Column);
	CHECK(request.avatar_side == AvatarSide::Right);
	const cordmux::sources::CompositeRosterParams params;
	CHECK(params.show);
	CHECK(params.anchor == RosterAnchor::BottomRight);
	CHECK(params.direction == RosterDirection::Column);
	CHECK(params.avatar_side == AvatarSide::Right);
	CHECK(params.gap_ratio == 0.25);
	// ずれの既定は 0 (ずらさない)。
	CHECK(params.offset_x_px == 0);
	CHECK(params.offset_y_px == 0);
	CHECK(request.offset_x_px == 0);
	CHECK(request.offset_y_px == 0);

	// 右中央の縦並びは、画面共有の人の名前ラベルがどこにあっても、右端の縦の中央に置く (避けて動かさない)。
	CompositeRosterRequest mr = request_of(2, 200, 40, RosterAnchor::MiddleRight, RosterDirection::Column, 8,
					       AvatarSide::Right);
	const CompositeRosterPlan plan = plan_composite_roster(mr);
	REQUIRE(plan.placed.size() == 2);
	CHECK(at(plan, 0).x == 1920 - 10 - 200);
	CHECK(at(plan, 0).y == 540 - (40 + 8 + 40) / 2);
}

TEST_CASE("ずれ (offset_x_px、offset_y_px) は、8 か所のどの配置でも、配置と余白で決まる位置にそのまま足す")
{
	// 右と下が正。負の値は左と上へずらす。2 人の縦並び (中央上と中央下は横並び) で、置く人の全員に同じずれを足す。
	const RosterAnchor anchors[] = {RosterAnchor::TopLeft,    RosterAnchor::TopCenter,   RosterAnchor::TopRight,
					RosterAnchor::MiddleLeft, RosterAnchor::MiddleRight, RosterAnchor::BottomLeft,
					RosterAnchor::BottomCenter, RosterAnchor::BottomRight};
	const int offsets[][2] = {{-200, -100}, {35, 12}, {0, -7}, {-1920, 1080}};
	for (const RosterAnchor anchor : anchors) {
		const CompositeRosterRequest base = request_of(2, 200, 40, anchor, RosterDirection::Column, 8);
		const CompositeRosterPlan before = plan_composite_roster(base);
		REQUIRE(before.placed.size() == 2);
		for (const auto &offset : offsets) {
			CAPTURE(static_cast<int>(anchor));
			CAPTURE(offset[0]);
			CAPTURE(offset[1]);
			CompositeRosterRequest shifted = base;
			shifted.offset_x_px = offset[0];
			shifted.offset_y_px = offset[1];
			const CompositeRosterPlan after = plan_composite_roster(shifted);
			// ずらしても置く人と置かない人の数は変わらず (キャンバスの外へはみ出しても戻さない)、位置だけが動く。
			REQUIRE(after.placed.size() == before.placed.size());
			CHECK(after.not_placed == before.not_placed);
			for (std::size_t i = 0; i < before.placed.size(); ++i) {
				CHECK(after.placed[i].index == before.placed[i].index);
				CHECK(after.placed[i].x == before.placed[i].x + offset[0]);
				CHECK(after.placed[i].y == before.placed[i].y + offset[1]);
			}
		}
	}
}

TEST_CASE("ずれはキャンバスのピクセルのまま使い、キャンバスの解像度を変えても同じピクセルだけずらす")
{
	// 右下の配置に x=-200、y=-100。1080p でも 2160p でも、ずらす量は 200 px と 100 px (解像度に合わせて伸ばさない)。
	for (const int scale : {1, 2}) {
		CAPTURE(scale);
		CompositeRosterRequest request = request_of(1, 200 * scale, 40 * scale, RosterAnchor::BottomRight,
							    RosterDirection::Column, 8 * scale);
		request.canvas_w = 1920 * scale;
		request.canvas_h = 1080 * scale;
		request.margin_px = 10 * scale;
		const CompositeRosterPlan before = plan_composite_roster(request);
		request.offset_x_px = -200;
		request.offset_y_px = -100;
		const CompositeRosterPlan after = plan_composite_roster(request);
		REQUIRE(before.placed.size() == 1);
		REQUIRE(after.placed.size() == 1);
		CHECK(before.placed[0].x == request.canvas_w - request.margin_px - 200 * scale);
		CHECK(after.placed[0].x - before.placed[0].x == -200);
		CHECK(after.placed[0].y - before.placed[0].y == -100);
	}
}
