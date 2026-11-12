// SPDX-License-Identifier: Apache-2.0

// sources/composite_layout.h (合成のソースの配置の純粋関数) のテスト。自分の枠と画面共有の組み合わせ、表示名の順、
// 奇数の大きさのキャンバス、すき間、縦横比を保つ計算、ログの 1 行の形を確認する。

#include "doctest.h"

#include "sources/composite_layout.h"

#include <cstddef>
#include <string>
#include <vector>

using cordmux::sources::composite_layout_log_line;
using cordmux::sources::CompositeLayout;
using cordmux::sources::CompositeLayoutRequest;
using cordmux::sources::CompositeShare;
using cordmux::sources::CompositeTile;
using cordmux::sources::fit_inside;
using cordmux::sources::PixelRect;
using cordmux::sources::plan_composite_layout;

namespace {

CompositeShare share(int slot, const std::string &name, const std::string &user_id = "")
{
	CompositeShare s;
	s.slot = slot;
	s.name = name;
	s.user_id = user_id.empty() ? "id" + std::to_string(slot) : user_id;
	return s;
}

CompositeLayoutRequest request(bool self_shown, std::vector<CompositeShare> shares, int w = 1920, int h = 1080)
{
	CompositeLayoutRequest r;
	r.canvas_w = w;
	r.canvas_h = h;
	r.self_shown = self_shown;
	r.shares = std::move(shares);
	return r;
}

bool rect_is(const PixelRect &r, int x, int y, int w, int h)
{
	return r.x == x && r.y == y && r.w == w && r.h == h;
}

std::vector<int> slots_of(const CompositeLayout &layout)
{
	std::vector<int> slots;
	for (const CompositeTile &tile : layout.tiles)
		slots.push_back(tile.self ? 0 : tile.slot);
	return slots;
}

// タイルがキャンバスを隙間なく、重ならずに覆っているか (ピクセルごとに数える)。
bool covers_exactly(const CompositeLayout &layout, int w, int h)
{
	std::vector<int> count(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
	for (const CompositeTile &tile : layout.tiles) {
		const PixelRect &r = tile.rect;
		if (r.x < 0 || r.y < 0 || r.x + r.w > w || r.y + r.h > h)
			return false;
		for (int y = r.y; y < r.y + r.h; ++y)
			for (int x = r.x; x < r.x + r.w; ++x)
				++count[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
					static_cast<std::size_t>(x)];
	}
	for (const int c : count)
		if (c != 1)
			return false;
	return true;
}

} // namespace

TEST_CASE("合成の配置: 自分の枠ありで画面共有が 3 人なら、自分が左上で 4 等分の全部が埋まる")
{
	const CompositeLayout layout =
		plan_composite_layout(request(true, {share(3, "c"), share(1, "a"), share(2, "b")}));
	REQUIRE(layout.tiles.size() == 4u);
	CHECK(layout.dropped.empty());
	CHECK(layout.tiles[0].self);
	CHECK(rect_is(layout.tiles[0].rect, 0, 0, 960, 540));
	CHECK(layout.tiles[1].slot == 1);
	CHECK(rect_is(layout.tiles[1].rect, 960, 0, 960, 540));
	CHECK(layout.tiles[2].slot == 2);
	CHECK(rect_is(layout.tiles[2].rect, 0, 540, 960, 540));
	CHECK(layout.tiles[3].slot == 3);
	CHECK(rect_is(layout.tiles[3].rect, 960, 540, 960, 540));
}

TEST_CASE("合成の配置: 自分の枠なしで画面共有が 4 人なら、左上から表示名の順に入る")
{
	const CompositeLayout layout = plan_composite_layout(
		request(false, {share(4, "d"), share(2, "b"), share(1, "a"), share(3, "c")}));
	REQUIRE(layout.tiles.size() == 4u);
	CHECK(slots_of(layout) == std::vector<int>{1, 2, 3, 4});
	CHECK(rect_is(layout.tiles[0].rect, 0, 0, 960, 540));
	CHECK(rect_is(layout.tiles[3].rect, 960, 540, 960, 540));
	CHECK(layout.dropped.empty());
}

TEST_CASE("合成の配置: 自分の枠ありで画面共有が 1 人なら、4 等分の左上と右上になる")
{
	const CompositeLayout layout = plan_composite_layout(request(true, {share(2, "a")}));
	REQUIRE(layout.tiles.size() == 2u);
	CHECK(layout.tiles[0].self);
	CHECK(rect_is(layout.tiles[0].rect, 0, 0, 960, 540));
	CHECK(layout.tiles[1].slot == 2);
	CHECK(rect_is(layout.tiles[1].rect, 960, 0, 960, 540));
}

TEST_CASE("合成の配置: 自分の枠なしで画面共有が 1 人なら全面。設定が偽なら 4 等分の左上")
{
	CompositeLayoutRequest r = request(false, {share(3, "a")});
	CompositeLayout layout = plan_composite_layout(r);
	REQUIRE(layout.tiles.size() == 1u);
	CHECK(layout.tiles[0].slot == 3);
	CHECK(rect_is(layout.tiles[0].rect, 0, 0, 1920, 1080));

	r.full_when_single = false;
	layout = plan_composite_layout(r);
	REQUIRE(layout.tiles.size() == 1u);
	CHECK(rect_is(layout.tiles[0].rect, 0, 0, 960, 540));
}

TEST_CASE("合成の配置: 自分の枠だけなら全面。設定が偽なら 4 等分の左上")
{
	CompositeLayoutRequest r = request(true, {});
	CompositeLayout layout = plan_composite_layout(r);
	REQUIRE(layout.tiles.size() == 1u);
	CHECK(layout.tiles[0].self);
	CHECK(rect_is(layout.tiles[0].rect, 0, 0, 1920, 1080));

	r.full_when_single = false;
	layout = plan_composite_layout(r);
	REQUIRE(layout.tiles.size() == 1u);
	CHECK(layout.tiles[0].self);
	CHECK(rect_is(layout.tiles[0].rect, 0, 0, 960, 540));
}

TEST_CASE("合成の配置: 何もなければ空。キャンバスの大きさが 0 以下でも空")
{
	CHECK(plan_composite_layout(request(false, {})).tiles.empty());
	CHECK(plan_composite_layout(request(false, {})).dropped.empty());
	CHECK(plan_composite_layout(request(true, {share(1, "a")}, 0, 1080)).tiles.empty());
	CHECK(plan_composite_layout(request(true, {share(1, "a")}, 1920, -1)).tiles.empty());
}

TEST_CASE("合成の配置: 表示名は自然順で並ぶ (user2 は user10 より前、大文字小文字は区別しない)")
{
	CompositeLayout layout = plan_composite_layout(
		request(false, {share(1, "user10"), share(2, "user2"), share(3, "User3")}));
	CHECK(slots_of(layout) == std::vector<int>{2, 3, 1});

	layout = plan_composite_layout(request(false, {share(1, "bob"), share(2, "Alice"), share(3, "carol")}));
	CHECK(slots_of(layout) == std::vector<int>{2, 1, 3});
}

TEST_CASE("合成の配置: 日本語の名前は UTF-8 のバイトの順 (ひらがな、カタカナ、漢字)")
{
	const CompositeLayout layout = plan_composite_layout(request(
		false, {share(1, "\xE5\xB1\xB1\xE7\x94\xB0"), share(2, "\xE3\x82\xAB\xE3\x82\xBF"),
			share(3, "\xE3\x81\x82\xE3\x81\x84")}));
	// あい (U+3042)、カタ (U+30AB)、山田 (U+5C71)
	CHECK(slots_of(layout) == std::vector<int>{3, 2, 1});
}

TEST_CASE("合成の配置: 同じ表示名なら user_id の順")
{
	const CompositeLayout layout = plan_composite_layout(
		request(false, {share(1, "same", "u-30"), share(2, "same", "u-10"), share(3, "same", "u-20")}));
	CHECK(slots_of(layout) == std::vector<int>{2, 3, 1});
}

TEST_CASE("合成の配置: 途中の人が抜けると後ろが詰まり、4 等分の形は変わらない")
{
	const std::vector<CompositeShare> all = {share(1, "a"), share(2, "b"), share(3, "c")};
	const CompositeLayout before = plan_composite_layout(request(true, all));
	REQUIRE(before.tiles.size() == 4u);
	CHECK(slots_of(before) == std::vector<int>{0, 1, 2, 3});

	// b が抜ける。c が左下に詰まる。
	const CompositeLayout after = plan_composite_layout(request(true, {share(1, "a"), share(3, "c")}));
	REQUIRE(after.tiles.size() == 3u);
	CHECK(slots_of(after) == std::vector<int>{0, 1, 3});
	CHECK(rect_is(after.tiles[2].rect, 0, 540, 960, 540));
	CHECK(rect_is(after.tiles[1].rect, before.tiles[1].rect.x, before.tiles[1].rect.y, before.tiles[1].rect.w,
		      before.tiles[1].rect.h));
}

TEST_CASE("合成の配置: 自分の枠ありで画面共有が 4 人なら、表示名の順で最後の 1 人が外れる")
{
	const CompositeLayout layout = plan_composite_layout(
		request(true, {share(1, "d"), share(2, "a"), share(3, "c"), share(4, "b")}));
	REQUIRE(layout.tiles.size() == 4u);
	CHECK(slots_of(layout) == std::vector<int>{0, 2, 4, 3});
	REQUIRE(layout.dropped.size() == 1u);
	CHECK(layout.dropped[0].slot == 1);
	CHECK(layout.dropped[0].name == "d");
}

TEST_CASE("描く画面共有と描かない画面共有の分け方は、配置と同じで、キャンバスの大きさに依らない")
{
	const std::vector<CompositeShare> shares = {share(1, "d"), share(2, "a"), share(3, "c"), share(4, "b"),
						    share(5, "e")};
	const cordmux::sources::CompositeShareSplit with_self = cordmux::sources::split_composite_shares(shares, true);
	REQUIRE(with_self.placed.size() == 3u);
	CHECK(with_self.placed[0].name == "a");
	CHECK(with_self.placed[1].name == "b");
	CHECK(with_self.placed[2].name == "c");
	REQUIRE(with_self.dropped.size() == 2u);
	CHECK(with_self.dropped[0].name == "d");
	CHECK(with_self.dropped[1].name == "e");

	const cordmux::sources::CompositeShareSplit without_self = cordmux::sources::split_composite_shares(shares, false);
	CHECK(without_self.placed.size() == 4u);
	REQUIRE(without_self.dropped.size() == 1u);
	CHECK(without_self.dropped[0].name == "e");

	// 配置の dropped と同じ人 (キャンバスが小さくても同じ)。
	const CompositeLayout layout = plan_composite_layout(request(true, shares, 64, 36));
	REQUIRE(layout.dropped.size() == with_self.dropped.size());
	for (std::size_t i = 0; i < layout.dropped.size(); ++i)
		CHECK(layout.dropped[i].slot == with_self.dropped[i].slot);
	CHECK(cordmux::sources::split_composite_shares({}, true).placed.empty());
}

TEST_CASE("合成の配置: 1366 x 768 でも、4 等分がキャンバスを隙間なく覆う (左と上を切り捨て)")
{
	const CompositeLayout layout = plan_composite_layout(
		request(true, {share(1, "a"), share(2, "b"), share(3, "c")}, 1366, 768));
	REQUIRE(layout.tiles.size() == 4u);
	CHECK(covers_exactly(layout, 1366, 768));
	CHECK(rect_is(layout.tiles[0].rect, 0, 0, 683, 384));

	// 奇数: 1365 x 767 は左が 682、上が 383 で、右が 683、下が 384。
	const CompositeLayout odd = plan_composite_layout(
		request(true, {share(1, "a"), share(2, "b"), share(3, "c")}, 1365, 767));
	CHECK(covers_exactly(odd, 1365, 767));
	CHECK(rect_is(odd.tiles[0].rect, 0, 0, 682, 383));
	CHECK(rect_is(odd.tiles[1].rect, 682, 0, 683, 383));
	CHECK(rect_is(odd.tiles[2].rect, 0, 383, 682, 384));
	CHECK(rect_is(odd.tiles[3].rect, 682, 383, 683, 384));
}

TEST_CASE("合成の配置: すき間は内側の辺だけに空き、外周には空かない")
{
	CompositeLayoutRequest r = request(true, {share(1, "a"), share(2, "b"), share(3, "c")});
	r.gap_px = 10;
	const CompositeLayout layout = plan_composite_layout(r);
	REQUIRE(layout.tiles.size() == 4u);
	CHECK(rect_is(layout.tiles[0].rect, 0, 0, 955, 535));
	CHECK(rect_is(layout.tiles[1].rect, 965, 0, 955, 535));
	CHECK(rect_is(layout.tiles[2].rect, 0, 545, 955, 535));
	CHECK(rect_is(layout.tiles[3].rect, 965, 545, 955, 535));
}

TEST_CASE("合成の配置: 奇数のすき間でも、隣り合うタイルの間はちょうどそのすき間になる")
{
	CompositeLayoutRequest r = request(true, {share(1, "a"), share(2, "b"), share(3, "c")});
	r.gap_px = 7;
	const CompositeLayout layout = plan_composite_layout(r);
	REQUIRE(layout.tiles.size() == 4u);
	const PixelRect &tl = layout.tiles[0].rect;
	const PixelRect &tr = layout.tiles[1].rect;
	const PixelRect &bl = layout.tiles[2].rect;
	CHECK(tr.x - (tl.x + tl.w) == 7);
	CHECK(bl.y - (tl.y + tl.h) == 7);
	CHECK(layout.tiles[3].rect.x + layout.tiles[3].rect.w == 1920);
	CHECK(layout.tiles[3].rect.y + layout.tiles[3].rect.h == 1080);
}

TEST_CASE("合成の配置: 全面のタイルにはすき間を空けない。4 等分の左上に置くときは内側の辺だけ空ける")
{
	CompositeLayoutRequest r = request(false, {share(1, "a")});
	r.gap_px = 10;
	CompositeLayout layout = plan_composite_layout(r);
	REQUIRE(layout.tiles.size() == 1u);
	CHECK(rect_is(layout.tiles[0].rect, 0, 0, 1920, 1080));

	r.full_when_single = false;
	layout = plan_composite_layout(r);
	REQUIRE(layout.tiles.size() == 1u);
	CHECK(rect_is(layout.tiles[0].rect, 0, 0, 955, 535));
}

TEST_CASE("合成の配置: 負のすき間は 0 として読み、大きすぎるすき間でもタイルの大きさは 1 以上")
{
	CompositeLayoutRequest r = request(true, {share(1, "a")});
	r.gap_px = -5;
	CHECK(rect_is(plan_composite_layout(r).tiles[0].rect, 0, 0, 960, 540));

	r.canvas_w = 4;
	r.canvas_h = 4;
	r.gap_px = 100;
	for (const CompositeTile &tile : plan_composite_layout(r).tiles) {
		CHECK(tile.rect.w >= 1);
		CHECK(tile.rect.h >= 1);
	}
}

TEST_CASE("合成の配置: 自分の枠なしで画面共有が 2 人なら左上と右上、3 人なら右下だけを空ける。どちらも 4 等分の大きさ")
{
	const CompositeLayout two = plan_composite_layout(request(false, {share(2, "b"), share(1, "a")}));
	REQUIRE(two.tiles.size() == 2u);
	CHECK(slots_of(two) == std::vector<int>{1, 2});
	CHECK(rect_is(two.tiles[0].rect, 0, 0, 960, 540));
	CHECK(rect_is(two.tiles[1].rect, 960, 0, 960, 540));

	const CompositeLayout three =
		plan_composite_layout(request(false, {share(3, "c"), share(1, "a"), share(2, "b")}));
	REQUIRE(three.tiles.size() == 3u);
	CHECK(slots_of(three) == std::vector<int>{1, 2, 3});
	CHECK(rect_is(three.tiles[0].rect, 0, 0, 960, 540));
	CHECK(rect_is(three.tiles[1].rect, 960, 0, 960, 540));
	CHECK(rect_is(three.tiles[2].rect, 0, 540, 960, 540));
}

TEST_CASE("合成の配置: 枠の間隔 8 では、隣り合うタイルの間が 8 で、キャンバスの外周は空かない")
{
	CompositeLayoutRequest r = request(true, {share(1, "a"), share(2, "b"), share(3, "c")});
	r.gap_px = 8;
	const CompositeLayout layout = plan_composite_layout(r);
	REQUIRE(layout.tiles.size() == 4u);
	CHECK(rect_is(layout.tiles[0].rect, 0, 0, 956, 536));
	CHECK(rect_is(layout.tiles[1].rect, 964, 0, 956, 536));
	CHECK(rect_is(layout.tiles[2].rect, 0, 544, 956, 536));
	CHECK(rect_is(layout.tiles[3].rect, 964, 544, 956, 536));
}

TEST_CASE("合成の配置: キャンバスの大きさを変えると、4 等分の位置がその大きさに合わせて決まり直す")
{
	const std::vector<CompositeShare> shares = {share(1, "a"), share(2, "b"), share(3, "c")};
	const CompositeLayout qhd = plan_composite_layout(request(true, shares, 2560, 1440));
	REQUIRE(qhd.tiles.size() == 4u);
	CHECK(covers_exactly(qhd, 2560, 1440));
	CHECK(rect_is(qhd.tiles[0].rect, 0, 0, 1280, 720));
	CHECK(rect_is(qhd.tiles[3].rect, 1280, 720, 1280, 720));

	const CompositeLayout full_hd = plan_composite_layout(request(true, shares));
	CHECK(rect_is(full_hd.tiles[3].rect, 960, 540, 960, 540));
}

TEST_CASE("合成の配置: 映像の解像度が変わっても枠の境界は動かず、内接する矩形だけが変わる")
{
	const std::vector<CompositeShare> shares = {share(1, "a"), share(2, "b")};
	const CompositeLayout layout = plan_composite_layout(request(true, shares));
	REQUIRE(layout.tiles.size() == 3u);
	const PixelRect tile = layout.tiles[1].rect;

	// 1280 x 720 も 928 x 522 も 16:9 で、同じ矩形になる。
	const PixelRect hd = fit_inside(tile, 1280, 720);
	const PixelRect odd = fit_inside(tile, 928, 522);
	CHECK(rect_is(hd, 960, 0, 960, 540));
	CHECK(rect_is(odd, 960, 0, 960, 540));

	// 4:3 に変わると、高さいっぱいで左右に余白ができる。枠の境界は変わらない。
	const PixelRect boxy = fit_inside(tile, 640, 480);
	CHECK(rect_is(boxy, 1080, 0, 720, 540));

	// 解像度が変わっても、配置の計算の結果 (枠の境界) は同じ。
	const CompositeLayout again = plan_composite_layout(request(true, shares));
	REQUIRE(again.tiles.size() == layout.tiles.size());
	for (std::size_t i = 0; i < layout.tiles.size(); ++i)
		CHECK(rect_is(again.tiles[i].rect, layout.tiles[i].rect.x, layout.tiles[i].rect.y,
			      layout.tiles[i].rect.w, layout.tiles[i].rect.h));

	// 縦長の映像に変わっても、枠の外へ出ない。
	const PixelRect tall = fit_inside(tile, 720, 1280);
	CHECK(tall.x >= tile.x);
	CHECK(tall.y >= tile.y);
	CHECK(tall.x + tall.w <= tile.x + tile.w);
	CHECK(tall.y + tall.h <= tile.y + tile.h);
}

TEST_CASE("縦横比を保つ計算: 横長の物は幅いっぱいで上下に余白ができる")
{
	// 16:9 の物を 960 x 540 に入れると同じ大きさ。2:1 の物は 960 x 480 で上下に 30 ずつ余る。
	CHECK(rect_is(fit_inside({0, 0, 960, 540}, 1920, 1080), 0, 0, 960, 540));
	CHECK(rect_is(fit_inside({100, 200, 960, 540}, 2000, 1000), 100, 230, 960, 480));
}

TEST_CASE("縦横比を保つ計算: 縦長の物は高さいっぱいで左右に余白ができる")
{
	CHECK(rect_is(fit_inside({100, 200, 960, 540}, 1080, 1920), 100 + (960 - 304) / 2, 200, 304, 540));
	// 1 : 1 の物は 540 x 540 で左右に 210 ずつ。
	CHECK(rect_is(fit_inside({0, 0, 960, 540}, 500, 500), 210, 0, 540, 540));
}

TEST_CASE("縦横比を保つ計算: 物がタイルより小さくても拡大して内側いっぱいに合わせる")
{
	CHECK(rect_is(fit_inside({0, 0, 960, 540}, 480, 270), 0, 0, 960, 540));
}

TEST_CASE("縦横比を保つ計算: 大きさが分からない物は大きさ 0 の矩形を返す。極端な比でも 1 以上")
{
	CHECK(rect_is(fit_inside({10, 20, 960, 540}, 0, 100), 10, 20, 0, 0));
	CHECK(rect_is(fit_inside({10, 20, 960, 540}, 100, -1), 10, 20, 0, 0));
	const PixelRect thin = fit_inside({0, 0, 960, 540}, 100000, 1);
	CHECK(thin.w == 960);
	CHECK(thin.h == 1);
}

TEST_CASE("合成の配置のログ: 1 行の形")
{
	const CompositeLayout layout =
		plan_composite_layout(request(true, {share(2, "b"), share(1, "z")}));
	CHECK(composite_layout_log_line(1920, 1080, layout.tiles) ==
	      "Composite layout canvas=1920x1080 count=3 self=0,0,960,540 slot2=960,0,960,540 slot1=0,540,960,540");
	CHECK(composite_layout_log_line(1280, 720, {}) == "Composite layout canvas=1280x720 count=0");

	const CompositeLayout single = plan_composite_layout(request(false, {share(3, "a")}, 1366, 768));
	CHECK(composite_layout_log_line(1366, 768, single.tiles) ==
	      "Composite layout canvas=1366x768 count=1 slot3=0,0,1366,768");
}

TEST_CASE("合成の配置のログ: 人の ID と名前を書かない")
{
	const CompositeLayout layout = plan_composite_layout(request(false, {share(1, "secret-name", "secret-id")}));
	const std::string line = composite_layout_log_line(1920, 1080, layout.tiles);
	CHECK(line.find("secret") == std::string::npos);
}
