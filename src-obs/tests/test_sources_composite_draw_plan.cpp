// SPDX-License-Identifier: Apache-2.0

// sources/composite_draw_plan.h (合成のソースが 1 フレームに置く物の計画) のテスト。置く順、切り抜きの矩形、
// 明るさ、光の形の違い、前の画像を置く判断を確認する。

#include "doctest.h"

#include "sources/composite_draw_plan.h"

#include <cstddef>
#include <vector>

using cordmux::sources::CompositeDraw;
using cordmux::sources::CompositeDrawImage;
using cordmux::sources::CompositeDrawKind;
using cordmux::sources::CompositeDrawRequest;
using cordmux::sources::CompositeDrawTile;
using cordmux::sources::PixelRect;
using cordmux::sources::plan_composite_draws;

namespace {

CompositeDrawImage image_at(int x, int y, int w, int h)
{
	CompositeDrawImage image;
	image.present = true;
	image.x = x;
	image.y = y;
	image.w = w;
	image.h = h;
	return image;
}

// 1280 x 720 のキャンバスを 4 等分した位置 index のタイル。名前ラベルは左下、光はその周りに 30 ピクセル広がり、
// 枠の光はセルと同じ大きさ。どれも現在の鍵で描いた画像で、話している。
CompositeDrawTile lit_tile(int index)
{
	CompositeDrawTile tile;
	tile.cell = PixelRect{(index % 2) * 640, (index / 2) * 360, 640, 360};
	tile.has_video = true;
	tile.level = 1.0;
	tile.label = image_at(10, 300, 200, 50);
	tile.glow = image_at(-20, 270, 260, 110);
	tile.label_current = true;
	tile.label_owner = "100";
	tile.owner = "100";
	tile.frame = image_at(0, 0, 640, 360);
	tile.frame_current = true;
	return tile;
}

CompositeDrawRequest glow_request(int tile_count)
{
	CompositeDrawRequest request;
	request.canvas_w = 1280;
	request.canvas_h = 720;
	request.parts.frame_glow = true;
	request.parts.label_glow = true;
	for (int i = 0; i < tile_count; ++i)
		request.tiles.push_back(lit_tile(i));
	return request;
}

std::vector<CompositeDrawKind> kinds_of(const std::vector<CompositeDraw> &draws)
{
	std::vector<CompositeDrawKind> kinds;
	for (const CompositeDraw &draw : draws)
		kinds.push_back(draw.kind);
	return kinds;
}

bool same_rect(const PixelRect &a, const PixelRect &b)
{
	return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

} // namespace

TEST_CASE("置く順は、全部のタイルの枠の光、全部の名前ラベルの光、全部の名前ラベルで、それぞれの中はタイルの順")
{
	const std::vector<CompositeDraw> draws = plan_composite_draws(glow_request(3));
	REQUIRE(draws.size() == 9);
	const std::vector<CompositeDrawKind> want = {
		CompositeDrawKind::FrameGlow, CompositeDrawKind::FrameGlow, CompositeDrawKind::FrameGlow,
		CompositeDrawKind::LabelGlow, CompositeDrawKind::LabelGlow, CompositeDrawKind::LabelGlow,
		CompositeDrawKind::Label,     CompositeDrawKind::Label,     CompositeDrawKind::Label};
	CHECK(kinds_of(draws) == want);
	for (std::size_t i = 0; i < draws.size(); ++i)
		CHECK(draws[i].tile == i % 3);
}

TEST_CASE("名前ラベルの光はセルの境では切らず、キャンバスの外だけを切る")
{
	CompositeDrawRequest request = glow_request(4);
	// 右上のタイルの光は、左の隣のタイルへ 20 ピクセル、上はキャンバスの外へ 30 ピクセル広がる。
	request.tiles[1].glow = image_at(-20, -30, 300, 120);
	const std::vector<CompositeDraw> draws = plan_composite_draws(request);
	const CompositeDraw *glow = nullptr;
	for (const CompositeDraw &draw : draws)
		if (draw.kind == CompositeDrawKind::LabelGlow && draw.tile == 1)
			glow = &draw;
	REQUIRE(glow != nullptr);
	CHECK(same_rect(glow->image, PixelRect{620, -30, 300, 120}));
	// 左の隣のセル (x が 640 より小さい所) は切らず、キャンバスの上の外 (y が負の所) だけを切る。
	CHECK(same_rect(glow->clip, PixelRect{620, 0, 300, 90}));
}

TEST_CASE("名前ラベルと枠の光は、セルの外を切る")
{
	CompositeDrawRequest request = glow_request(2);
	request.tiles[1].label = image_at(600, 340, 100, 40); // セルの右下からはみ出す
	request.tiles[1].frame = image_at(0, 0, 700, 400);
	request.tiles[1].frame_current = true;
	const std::vector<CompositeDraw> draws = plan_composite_draws(request);
	for (const CompositeDraw &draw : draws) {
		if (draw.tile != 1)
			continue;
		if (draw.kind == CompositeDrawKind::Label) {
			CHECK(same_rect(draw.image, PixelRect{1240, 340, 100, 40}));
			CHECK(same_rect(draw.clip, PixelRect{1240, 340, 40, 20}));
		}
		if (draw.kind == CompositeDrawKind::FrameGlow)
			CHECK(same_rect(draw.clip, PixelRect{640, 0, 640, 360}));
	}
}

TEST_CASE("切り抜きの矩形は、どの置く物でも画像の中とキャンバスの中にある")
{
	CompositeDrawRequest request = glow_request(4);
	request.tiles[0].glow = image_at(-50, -50, 800, 500);
	request.tiles[3].glow = image_at(-100, -100, 900, 600);
	for (const CompositeDraw &draw : plan_composite_draws(request)) {
		CHECK(draw.clip.w > 0);
		CHECK(draw.clip.h > 0);
		CHECK(draw.clip.x >= 0);
		CHECK(draw.clip.y >= 0);
		CHECK(draw.clip.x + draw.clip.w <= 1280);
		CHECK(draw.clip.y + draw.clip.h <= 720);
		CHECK(draw.clip.x >= draw.image.x);
		CHECK(draw.clip.y >= draw.image.y);
		CHECK(draw.clip.x + draw.clip.w <= draw.image.x + draw.image.w);
		CHECK(draw.clip.y + draw.clip.h <= draw.image.y + draw.image.h);
	}
}

TEST_CASE("明るさは光に入り、名前ラベルは明るさ 1 で置き、明るさ 0 なら光を置かない")
{
	CompositeDrawRequest request = glow_request(2);
	request.tiles[0].level = 0.25;
	request.tiles[1].level = 0.0;
	const std::vector<CompositeDraw> draws = plan_composite_draws(request);
	const std::vector<CompositeDrawKind> want = {CompositeDrawKind::FrameGlow, CompositeDrawKind::LabelGlow,
						     CompositeDrawKind::Label, CompositeDrawKind::Label};
	REQUIRE(kinds_of(draws) == want);
	CHECK(draws[0].level == doctest::Approx(0.25));
	CHECK(draws[1].level == doctest::Approx(0.25));
	CHECK(draws[2].level == doctest::Approx(1.0));
	CHECK(draws[3].level == doctest::Approx(1.0));
	CHECK(draws[3].tile == 1);
}

TEST_CASE("映像を描いていないタイルと、隠すときは、何も置かない")
{
	CompositeDrawRequest request = glow_request(2);
	request.tiles[0].has_video = false;
	for (const CompositeDraw &draw : plan_composite_draws(request))
		CHECK(draw.tile == 1);
	request.hide = true;
	CHECK(plan_composite_draws(request).empty());
}

TEST_CASE("前の名前ラベルとその光は、描く人が同じでセルに収まるときだけ置き、違う人のものは置かない")
{
	CompositeDrawRequest request = glow_request(1);
	request.tiles[0].label_current = false;
	CHECK(kinds_of(plan_composite_draws(request)) ==
	      std::vector<CompositeDrawKind>{CompositeDrawKind::FrameGlow, CompositeDrawKind::LabelGlow,
					     CompositeDrawKind::Label});

	request.tiles[0].label_owner = "200";
	CHECK(kinds_of(plan_composite_draws(request)) == std::vector<CompositeDrawKind>{CompositeDrawKind::FrameGlow});

	request.tiles[0].label_owner = "100";
	request.tiles[0].label = image_at(600, 300, 100, 50); // 現在のセルからはみ出す前の画像
	CHECK(kinds_of(plan_composite_draws(request)) == std::vector<CompositeDrawKind>{CompositeDrawKind::FrameGlow});
}

TEST_CASE("枠の光は、現在の鍵の画像か、セルと同じ大きさの前の画像だけを置く")
{
	CompositeDrawRequest request = glow_request(1);
	request.tiles[0].frame_current = false;
	CHECK(plan_composite_draws(request).front().kind == CompositeDrawKind::FrameGlow);
	request.tiles[0].frame = image_at(0, 0, 320, 180);
	CHECK(plan_composite_draws(request).front().kind == CompositeDrawKind::LabelGlow);
	request.tiles[0].frame.present = false;
	request.tiles[0].frame_current = true;
	CHECK(plan_composite_draws(request).front().kind == CompositeDrawKind::LabelGlow);
}
