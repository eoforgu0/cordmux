// SPDX-License-Identifier: Apache-2.0

// 話していない人の名前ラベルを暗くする処理 (sources/composite_draw_plan.h の dim と、CPU で重ねる
// sources/composite_draw_cpu.h) と、画面共有していない参加者の名前ラベルを重ねる処理の、値のテスト。暗さは名前ラベルの
// 色だけに掛かり、アルファと光の画像は変わらない。画像は、値が読みやすいように、1 色で塗った小さな画像を使う。

#include "doctest.h"

#include <QColor>
#include <QImage>

#include <cmath>
#include <cstddef>
#include <vector>

#include "sources/composite_draw_cpu.h"
#include "sources/composite_draw_plan.h"
#include "sources/composite_label_plan.h"

using cordmux::sources::CompositeDraw;
using cordmux::sources::CompositeDrawImage;
using cordmux::sources::CompositeDrawKind;
using cordmux::sources::CompositeDrawRequest;
using cordmux::sources::CompositeDrawRosterEntry;
using cordmux::sources::CompositeDrawTile;
using cordmux::sources::CompositeTileImages;
using cordmux::sources::draw_composite_plan;
using cordmux::sources::PixelRect;
using cordmux::sources::plan_composite_draws;

namespace {

// 乗算済みアルファで a = 200、色 (100, 80, 60) の単色の画像。
QImage solid(int w, int h, int r = 100, int g = 80, int b = 60, int a = 200)
{
	QImage image(w, h, QImage::Format_ARGB32_Premultiplied);
	image.fill(qRgba(r, g, b, a));
	return image;
}

CompositeDrawImage placed(const QImage &image, int x, int y)
{
	CompositeDrawImage out;
	out.present = !image.isNull();
	out.x = x;
	out.y = y;
	out.w = image.width();
	out.h = image.height();
	return out;
}

// 80 x 60 のキャンバスの 4 等分の左上に、名前ラベル (単色) を置くタイルが 1 つの要求。
CompositeDrawRequest one_tile_request(const QImage &label, const QImage &glow, double level, double dim)
{
	CompositeDrawRequest request;
	request.canvas_w = 80;
	request.canvas_h = 60;
	request.parts.label_glow = true;
	CompositeDrawTile tile;
	tile.cell = PixelRect{0, 0, 80, 60};
	tile.has_video = true;
	tile.level = level;
	tile.label = placed(label, 10, 10);
	tile.glow = placed(glow, 6, 6);
	tile.label_current = true;
	tile.label_owner = "a";
	tile.owner = "a";
	tile.label_dim = dim;
	request.tiles.push_back(tile);
	return request;
}

QImage transparent_canvas()
{
	QImage canvas(80, 60, QImage::Format_ARGB32_Premultiplied);
	canvas.fill(Qt::transparent);
	return canvas;
}

} // namespace

TEST_CASE("暗さを掛けた名前ラベルは、色のチャンネルだけが暗くなり、アルファは変わらない")
{
	const QImage label = solid(20, 10);
	for (const double dim : {1.0, 0.7, 0.5, 0.1, 0.0}) {
		CAPTURE(dim);
		const CompositeDrawRequest request = one_tile_request(label, QImage(), 1.0, dim);
		const std::vector<CompositeDraw> draws = plan_composite_draws(request);
		REQUIRE(draws.size() == 1);
		CHECK(draws[0].kind == CompositeDrawKind::Label);
		CHECK(draws[0].dim == doctest::Approx(dim));
		QImage canvas = transparent_canvas();
		draw_composite_plan(&canvas, draws, {CompositeTileImages{label, QImage(), QImage()}});
		const QRgb p = canvas.pixel(15, 15);
		CHECK(qAlpha(p) == 200);
		CHECK(qRed(p) == static_cast<int>(std::lround(100 * dim)));
		CHECK(qGreen(p) == static_cast<int>(std::lround(80 * dim)));
		CHECK(qBlue(p) == static_cast<int>(std::lround(60 * dim)));
		// 名前ラベルの外は何も変わらない。
		CHECK(canvas.pixel(5, 5) == qRgba(0, 0, 0, 0));
	}
}

TEST_CASE("暗さ 30% の白は、GPU と同じく 32 ビットの小数で掛けて 178 になる (64 ビットなら 178.5 が 179 に丸まる)")
{
	// 黙っている人の倍率は 1 - 30 / 100。64 ビットでは 255 を掛けるとちょうど 178.5 になる。GPU は 32 ビットの 0.7
	// (0.7 より少し小さい) で掛けるので、OBS の中で撮った画像は 178 になる。
	const double dim = cordmux::sources::composite_label_dim_factor(0.0, 30, true);
	REQUIRE(std::lround(dim * 255.0) == 179);
	const QImage label = solid(20, 10, 255, 255, 255, 255);
	QImage canvas = transparent_canvas();
	draw_composite_plan(&canvas, plan_composite_draws(one_tile_request(label, QImage(), 1.0, dim)),
			    {CompositeTileImages{label, QImage(), QImage()}});
	CHECK(canvas.pixel(15, 15) == qRgba(178, 178, 178, 255));
}

TEST_CASE("暗さ 1 のとき、半透明の名前ラベルは、背景との混ざり方が「元 + 下 x (1 - 元のアルファ)」のまま")
{
	// 暗くしないときの値: 色 (100, 80, 60)、アルファ 200 を背景 (30, 60, 90) に重ねると、色は元の色 + 背景 x 55 / 255。
	const QImage label = solid(20, 10);
	const CompositeDrawRequest request = one_tile_request(label, QImage(), 1.0, 1.0);
	QImage canvas = transparent_canvas();
	canvas.fill(QColor(30, 60, 90));
	draw_composite_plan(&canvas, plan_composite_draws(request), {CompositeTileImages{label, QImage(), QImage()}});
	const QRgb p = canvas.pixel(15, 15);
	CHECK(qRed(p) == static_cast<int>(std::lround(100 + 30 * 55 / 255.0)));
	CHECK(qGreen(p) == static_cast<int>(std::lround(80 + 60 * 55 / 255.0)));
	CHECK(qBlue(p) == static_cast<int>(std::lround(60 + 90 * 55 / 255.0)));
	CHECK(qAlpha(p) == 255);
}

TEST_CASE("暗さは背景に重ねても、背景の色には掛からず、名前ラベルの色だけが暗くなる")
{
	// 不透明な名前ラベルを暗さ 0.5 で背景 (30, 60, 90) に重ねる: 名前ラベルの色の半分がそのまま出る。
	const QImage label = solid(20, 10, 200, 100, 50, 255);
	const CompositeDrawRequest request = one_tile_request(label, QImage(), 1.0, 0.5);
	QImage canvas = transparent_canvas();
	canvas.fill(QColor(30, 60, 90));
	draw_composite_plan(&canvas, plan_composite_draws(request), {CompositeTileImages{label, QImage(), QImage()}});
	const QRgb inside = canvas.pixel(15, 15);
	CHECK(qRed(inside) == 100);
	CHECK(qGreen(inside) == 50);
	CHECK(qBlue(inside) == 25);
	CHECK(qAlpha(inside) == 255);
	CHECK(canvas.pixel(60, 40) == qRgba(30, 60, 90, 255)); // 外は背景のまま
}

TEST_CASE("暗さは光の画像には掛からない")
{
	const QImage label = solid(20, 10);
	const QImage glow = solid(30, 20, 0, 160, 0, 160);
	const CompositeDrawRequest request = one_tile_request(label, glow, 1.0, 0.25);
	const std::vector<CompositeDraw> draws = plan_composite_draws(request);
	REQUIRE(draws.size() == 2);
	CHECK(draws[0].kind == CompositeDrawKind::LabelGlow);
	CHECK(draws[0].dim == 1.0);
	CHECK(draws[1].kind == CompositeDrawKind::Label);
	CHECK(draws[1].dim == doctest::Approx(0.25));
	QImage canvas = transparent_canvas();
	draw_composite_plan(&canvas, draws, {CompositeTileImages{label, glow, QImage()}});
	// 光だけの所 (名前ラベルの外) は、光の画像そのまま。
	const QRgb lit = canvas.pixel(8, 8);
	CHECK(qRed(lit) == 0);
	CHECK(qGreen(lit) == 160);
	CHECK(qAlpha(lit) == 160);
}

TEST_CASE("暗さの倍率は 0 から 1 に収め、計画に持つ")
{
	const QImage label = solid(20, 10);
	CHECK(plan_composite_draws(one_tile_request(label, QImage(), 1.0, 3.0)).front().dim == 1.0);
	CHECK(plan_composite_draws(one_tile_request(label, QImage(), 1.0, -1.0)).front().dim == 0.0);
}

TEST_CASE("画面共有していない参加者の名前ラベルは、画面共有の枠の物の後に置き、キャンバスの座標で、暗さを掛ける")
{
	const QImage tile_label = solid(20, 10, 100, 80, 60, 255);
	CompositeDrawRequest request = one_tile_request(tile_label, QImage(), 1.0, 1.0);
	const QImage roster_label = solid(12, 8, 200, 100, 50, 255);
	const QImage roster_glow = solid(20, 16, 0, 200, 0, 200);
	CompositeDrawRosterEntry entry;
	entry.label = placed(roster_label, 50, 40);
	entry.glow = placed(roster_glow, 46, 36);
	entry.level = 1.0;
	entry.dim = 0.5;
	request.roster.push_back(entry);
	// 画面共有の枠の名前ラベルを隠す設定でも、画面共有していない参加者は置く。
	request.hide = true;
	const std::vector<CompositeDraw> hidden = plan_composite_draws(request);
	REQUIRE(hidden.size() == 2);
	CHECK(hidden[0].kind == CompositeDrawKind::RosterGlow);
	CHECK(hidden[1].kind == CompositeDrawKind::RosterLabel);

	request.hide = false;
	const std::vector<CompositeDraw> draws = plan_composite_draws(request);
	REQUIRE(draws.size() == 3);
	CHECK(draws[0].kind == CompositeDrawKind::Label);
	CHECK(draws[1].kind == CompositeDrawKind::RosterGlow);
	CHECK(draws[2].kind == CompositeDrawKind::RosterLabel);
	CHECK(draws[2].dim == doctest::Approx(0.5));
	CHECK(draws[1].dim == 1.0);

	QImage canvas = transparent_canvas();
	draw_composite_plan(&canvas, draws, {CompositeTileImages{tile_label, QImage(), QImage()}},
			    {CompositeTileImages{roster_label, roster_glow, QImage()}});
	const QRgb tile = canvas.pixel(15, 15);
	CHECK(qRed(tile) == 100); // 画面共有の枠の名前ラベルは暗くしていない
	const QRgb rostered = canvas.pixel(55, 43);
	CHECK(qRed(rostered) == 100);
	CHECK(qGreen(rostered) == 50);
	CHECK(qBlue(rostered) == 25);
	CHECK(qAlpha(rostered) == 255);
	const QRgb glow_only = canvas.pixel(48, 38);
	CHECK(qGreen(glow_only) == 200);
	CHECK(qAlpha(glow_only) == 200);
}

TEST_CASE("画面共有していない参加者の画像はキャンバスの外を切り、明るさ 0 の光は置かない")
{
	CompositeDrawRequest request;
	request.canvas_w = 80;
	request.canvas_h = 60;
	request.parts.label_glow = true;
	CompositeDrawRosterEntry entry;
	entry.label = placed(solid(20, 10), 70, 55); // 右下にはみ出す
	entry.glow = placed(solid(30, 20), 62, 48);
	entry.level = 0.0;
	request.roster.push_back(entry);
	std::vector<CompositeDraw> draws = plan_composite_draws(request);
	REQUIRE(draws.size() == 1);
	CHECK(draws[0].kind == CompositeDrawKind::RosterLabel);
	CHECK(draws[0].clip.x == 70);
	CHECK(draws[0].clip.y == 55);
	CHECK(draws[0].clip.w == 10);
	CHECK(draws[0].clip.h == 5);

	// 画像がまったくキャンバスの外なら何も置かない。画像のない人も置かない。
	request.roster[0].label = placed(solid(20, 10), 100, 10);
	CHECK(plan_composite_draws(request).empty());
	request.roster[0].label = CompositeDrawImage{};
	CHECK(plan_composite_draws(request).empty());

	// 光は、名前ラベルの光を描く設定のときだけ、明るさがあるときに置く。
	request.roster[0].label = placed(solid(20, 10), 10, 10);
	request.roster[0].level = 0.6;
	CHECK(plan_composite_draws(request).size() == 2);
	request.parts.label_glow = false;
	CHECK(plan_composite_draws(request).size() == 1);
}
