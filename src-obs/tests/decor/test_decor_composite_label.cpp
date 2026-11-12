// SPDX-License-Identifier: Apache-2.0

// 合成のソースの名前ラベルとグロー (sources/composite_label.h) のテスト。描画した画像を期待する画像
// (snapshots の PNG) と比べるテストと、位置、縁取り、不透明度、グローの範囲の性質を確認するテストがある。進め方と
// 期待する画像の作り直し方は test_decor_paint.cpp の先頭と同じ。名前には同梱のフォントにある文字だけを使う。
// 計画のとおりに重ねた画像の値で見え方を測るテストは test_decor_composite_render.cpp にある。

#include "doctest.h"

#include <QColor>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <QRect>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "decor/decor_test_env.h"
#include "decor/decor_test_support.h"
#include "sources/composite_label.h"
#include "sources/composite_roster.h"
#include "sources/decor_paint_common.h"

using cordmux::sources::CompositeLabel;
using cordmux::sources::CompositeLabelLook;
using cordmux::sources::DecorCorner;
using cordmux::sources::LabelContent;
using cordmux::sources::paint_composite_label;
using cordmux::sources::paint_common::grapheme_count;
using cordmux::sources::paint_common::prefix_collides;
using cordmux::sources::paint_common::visible_part;
using cordmux::sources::paint_frame_glow;
using cordmux::tests::decor_test_font_family;
using cordmux::tests::ink_bounds;
using cordmux::tests::snapshot_matches;
using cordmux::tests::test_avatar;

namespace {

constexpr int kCellW = 640;
constexpr int kCellH = 360;

// 描いた画像を期待する画像と比べるので、見た目の既定値が変わっても結果が動かないように、背景なしで、縁取りの太さを決めて始める。
CompositeLabelLook test_look()
{
	CompositeLabelLook look;
	look.font_family = decor_test_font_family().toStdString();
	look.outline_ratio = 0.12;
	look.show_plate = false;
	look.plate_argb = 0xBF000000u;
	return look;
}

LabelContent content_of(const char *name, DecorCorner corner, bool with_avatar)
{
	LabelContent content;
	content.name = name;
	content.corner = corner;
	if (with_avatar)
		content.avatar = test_avatar();
	return content;
}

// 左下に置く、アバターの画像がある人。
LabelContent bottom_left_with_avatar(const char *name)
{
	return content_of(name, DecorCorner::BottomLeft, true);
}

// セルの背景 (中間の灰色)。白い文字も黒い縁取りも読める色にする。
QImage gray_cell(int w, int h)
{
	QImage cell(w, h, QImage::Format_ARGB32_Premultiplied);
	cell.fill(QColor(112, 112, 112));
	return cell;
}

// グロー、名前ラベルの順に灰色のセルに重ねた画像。
QImage compose_on_gray(const CompositeLabel &label, int w, int h, bool with_glow)
{
	QImage cell = gray_cell(w, h);
	QPainter painter(&cell);
	if (with_glow && !label.glow.isNull())
		painter.drawImage(label.glow_x, label.glow_y, label.glow);
	if (!label.label.isNull())
		painter.drawImage(label.label_x, label.label_y, label.label);
	return cell;
}

int max_alpha(const QImage &image)
{
	int best = 0;
	for (int y = 0; y < image.height(); ++y)
		for (int x = 0; x < image.width(); ++x)
			best = std::max(best, qAlpha(image.pixel(x, y)));
	return best;
}

// 不透明な白に近いピクセル (文字の塗りの中) だけを囲む矩形。縁取りと文字の輪郭のなめらかな部分は含まない。
QRect fill_bounds(const QImage &image)
{
	int left = image.width();
	int top = image.height();
	int right = -1;
	int bottom = -1;
	for (int y = 0; y < image.height(); ++y) {
		for (int x = 0; x < image.width(); ++x) {
			const QRgb p = image.pixel(x, y);
			if (qAlpha(p) == 255 && qRed(p) >= 250 && qGreen(p) >= 250 && qBlue(p) >= 250) {
				left = std::min(left, x);
				top = std::min(top, y);
				right = std::max(right, x);
				bottom = std::max(bottom, y);
			}
		}
	}
	if (right < 0)
		return QRect();
	return QRect(left, top, right - left + 1, bottom - top + 1);
}

bool premultiplied_is_valid(const QImage &image)
{
	for (int y = 0; y < image.height(); ++y) {
		for (int x = 0; x < image.width(); ++x) {
			const QRgb p = image.pixel(x, y);
			if (qRed(p) > qAlpha(p) || qGreen(p) > qAlpha(p) || qBlue(p) > qAlpha(p))
				return false;
		}
	}
	return true;
}

} // namespace

TEST_CASE("縁取りの名前だけを左下に描画した画像は、期待する画像と一致する")
{
	CompositeLabelLook look = test_look();
	look.show_avatar = false;
	look.opacity = 1.0;
	const CompositeLabel label = paint_composite_label(content_of("Taro Yamada", DecorCorner::BottomLeft, false),
							   look, 30.0, 24.0, kCellW, kCellH);
	REQUIRE_FALSE(label.label.isNull());
	CHECK(snapshot_matches(compose_on_gray(label, kCellW, kCellH, false), "composite_outline_bottom_left"));
}

TEST_CASE("アバターと名前とグローを右上に描画した画像は、期待する画像と一致する")
{
	CompositeLabelLook look = test_look();
	const CompositeLabel label = paint_composite_label(content_of("Hanako 2", DecorCorner::TopRight, true), look,
							   30.0, 24.0, kCellW, kCellH);
	REQUIRE_FALSE(label.label.isNull());
	REQUIRE_FALSE(label.glow.isNull());
	CHECK(snapshot_matches(compose_on_gray(label, kCellW, kCellH, true), "composite_avatar_glow_top_right"));
}

TEST_CASE("背景を表示する設定の名前ラベルを描画した画像は、期待する画像と一致する")
{
	CompositeLabelLook look = test_look();
	look.show_plate = true;
	const CompositeLabel label = paint_composite_label(bottom_left_with_avatar("Taro Yamada"),
							   look, 30.0, 24.0, kCellW, kCellH);
	REQUIRE_FALSE(label.label.isNull());
	CHECK(snapshot_matches(compose_on_gray(label, kCellW, kCellH, false), "composite_plate_bottom_left"));
}

TEST_CASE("アバターの画像がないときの頭文字の円を描画した画像は、期待する画像と一致する")
{
	CompositeLabelLook look = test_look();
	const CompositeLabel label = paint_composite_label(content_of("ykari", DecorCorner::BottomLeft, false), look,
							   30.0, 24.0, kCellW, kCellH);
	REQUIRE_FALSE(label.label.isNull());
	CHECK(label.initial_drawn);
	CHECK(snapshot_matches(compose_on_gray(label, kCellW, kCellH, true), "composite_initial_circle"));
}

TEST_CASE("幅に入らない長い名前を省略した名前ラベルを描画した画像は、期待する画像と一致する")
{
	CompositeLabelLook look = test_look();
	const CompositeLabel label =
		paint_composite_label(bottom_left_with_avatar("Maximilian Alexander Featherstonehaugh"),
				      look, 24.0, 20.0, 400, 200);
	REQUIRE_FALSE(label.label.isNull());
	CHECK(label.elided);
	CHECK(label.box_w <= 400 - 20 * 2);
	CHECK(snapshot_matches(compose_on_gray(label, 400, 200, false), "composite_elided_long_name"));
}

TEST_CASE("枠の内側のグローを小さい画像に描画した画像は、期待する画像と一致する")
{
	const QImage glow = paint_frame_glow(320, 180, 0xFF57F287u, 12.0, 3.0);
	REQUIRE(glow.width() == 320);
	REQUIRE(glow.height() == 180);
	QImage cell(320, 180, QImage::Format_ARGB32_Premultiplied);
	cell.fill(QColor(40, 40, 48));
	QPainter painter(&cell);
	painter.drawImage(0, 0, glow);
	painter.end();
	CHECK(snapshot_matches(cell, "composite_frame_glow_320x180"));
}

TEST_CASE("名前ラベルは、指定した角からセルの端まで余白だけ離れ、4 つの角のどれでも同じ大きさになる")
{
	const CompositeLabelLook look = test_look();
	const double margin = 30.0;
	CompositeLabel by_corner[4];
	const DecorCorner corners[4] = {DecorCorner::TopLeft, DecorCorner::TopRight, DecorCorner::BottomLeft,
					DecorCorner::BottomRight};
	for (int i = 0; i < 4; ++i)
		by_corner[i] = paint_composite_label(content_of("Taro", corners[i], true), look, 28.0, margin, kCellW,
						     kCellH);
	for (const CompositeLabel &l : by_corner) {
		REQUIRE_FALSE(l.label.isNull());
		CHECK(l.box_w == by_corner[0].box_w);
		CHECK(l.box_h == by_corner[0].box_h);
	}
	CHECK(by_corner[0].box_x == 30);
	CHECK(by_corner[0].box_y == 30);
	CHECK(by_corner[1].box_x + by_corner[1].box_w == kCellW - 30);
	CHECK(by_corner[1].box_y == 30);
	CHECK(by_corner[2].box_x == 30);
	CHECK(by_corner[2].box_y + by_corner[2].box_h == kCellH - 30);
	CHECK(by_corner[3].box_x + by_corner[3].box_w == kCellW - 30);
	CHECK(by_corner[3].box_y + by_corner[3].box_h == kCellH - 30);
}

TEST_CASE("縁取りを太くすると、矩形が縁取りの分だけ広がり、余白は見える縁から測ったまま変わらない")
{
	CompositeLabelLook plain = test_look();
	plain.show_avatar = false;
	plain.opacity = 1.0;
	plain.outline_ratio = 0.0;
	CompositeLabelLook thick = plain;
	thick.outline_ratio = 0.25;

	const LabelContent content = content_of("Taro Yamada", DecorCorner::BottomLeft, false);
	const CompositeLabel a = paint_composite_label(content, plain, 40.0, 40.0, kCellW, kCellH);
	const CompositeLabel b = paint_composite_label(content, thick, 40.0, 40.0, kCellW, kCellH);
	REQUIRE_FALSE(a.label.isNull());
	REQUIRE_FALSE(b.label.isNull());

	// 矩形は左下の隅から同じ余白のまま、左右に縁取りの見える太さ (40 x 0.25 / 2 = 5 ピクセル) ずつ広がる。
	CHECK(a.box_x == b.box_x);
	CHECK(a.box_y + a.box_h == b.box_y + b.box_h);
	CHECK(b.box_w - a.box_w == 10);

	// 文字の塗りは、縁取りの見える太さの分だけ右へ動き、上下には動かない。
	const QRect fill_a = fill_bounds(a.label).translated(a.label_x, a.label_y);
	const QRect fill_b = fill_bounds(b.label).translated(b.label_x, b.label_y);
	REQUIRE_FALSE(fill_a.isNull());
	CHECK(fill_b.left() - fill_a.left() >= 4);
	CHECK(fill_b.left() - fill_a.left() <= 6);
	CHECK(fill_a.top() == fill_b.top());
	CHECK(fill_a.bottom() == fill_b.bottom());

	// 縁取りは、塗りの外へ線の太さの半分 (5 ピクセル) だけ伸び、左の見える縁は矩形の左の端に来る (字形の左の
	// 空きの分だけずれてよい)。
	const QRect ink_b = ink_bounds(b.label).translated(b.label_x, b.label_y);
	CHECK(std::abs(ink_b.left() - b.box_x) <= 3);
	CHECK(ink_b.left() <= fill_b.left() - 4);
	CHECK(ink_b.right() >= fill_b.right() + 4);
	CHECK(ink_b.top() <= fill_b.top() - 4);
	CHECK(ink_b.bottom() >= fill_b.bottom() + 4);
	// 画像の外へは出ない。
	CHECK(ink_b.left() >= b.label_x);
	CHECK(ink_b.top() >= b.label_y);
	CHECK(ink_b.right() <= b.label_x + b.label.width() - 1);
	CHECK(ink_b.bottom() <= b.label_y + b.label.height() - 1);
}

TEST_CASE("縁取りの色のピクセルは、縁取りの色で塗られ、塗りの外側にだけ現れる")
{
	CompositeLabelLook look = test_look();
	look.show_avatar = false;
	look.opacity = 1.0;
	look.outline_argb = 0xFFFF0000u;
	look.outline_ratio = 0.2;
	const CompositeLabel label = paint_composite_label(content_of("Taro Yamada", DecorCorner::BottomLeft, false),
							   look, 40.0, 40.0, kCellW, kCellH);
	REQUIRE_FALSE(label.label.isNull());
	int red_pixels = 0;
	for (int y = 0; y < label.label.height(); ++y) {
		for (int x = 0; x < label.label.width(); ++x) {
			const QRgb p = label.label.pixel(x, y);
			if (qAlpha(p) == 255 && qRed(p) == 255 && qGreen(p) == 0 && qBlue(p) == 0)
				++red_pixels;
		}
	}
	CHECK(red_pixels > 100);
}

TEST_CASE("縁取りを表示しないと、縁取りの色のピクセルが現れず、太さ 0 と同じ矩形と画像になる")
{
	CompositeLabelLook look = test_look();
	look.show_avatar = false;
	look.opacity = 1.0;
	look.outline_argb = 0xFFFF0000u;
	look.outline_ratio = 0.2;
	look.show_outline = false;
	const LabelContent content = content_of("Taro Yamada", DecorCorner::BottomLeft, false);
	const CompositeLabel hidden = paint_composite_label(content, look, 40.0, 40.0, kCellW, kCellH);
	REQUIRE_FALSE(hidden.label.isNull());
	for (int y = 0; y < hidden.label.height(); ++y) {
		for (int x = 0; x < hidden.label.width(); ++x) {
			const QRgb p = hidden.label.pixel(x, y);
			REQUIRE_FALSE((qAlpha(p) == 255 && qRed(p) == 255 && qGreen(p) == 0 && qBlue(p) == 0));
		}
	}

	// 太さ 0 で表示する場合と、矩形も画像も同じ。
	look.show_outline = true;
	look.outline_ratio = 0.0;
	const CompositeLabel zero = paint_composite_label(content, look, 40.0, 40.0, kCellW, kCellH);
	CHECK(hidden.box_x == zero.box_x);
	CHECK(hidden.box_y == zero.box_y);
	CHECK(hidden.box_w == zero.box_w);
	CHECK(hidden.box_h == zero.box_h);
	CHECK(hidden.label == zero.label);

	// 表示すると、同じ太さで縁取りの色のピクセルが現れる。
	look.outline_ratio = 0.2;
	const CompositeLabel shown = paint_composite_label(content, look, 40.0, 40.0, kCellW, kCellH);
	CHECK(shown.box_w > hidden.box_w);
}

TEST_CASE("名前ラベルの幅はセルの幅から余白を 2 つ引いた幅を超えず、狭い幅ではアバターを省いて名前を残す")
{
	const CompositeLabelLook look = test_look();
	const CompositeLabel wide = paint_composite_label(
		bottom_left_with_avatar("Taro Yamada"), look, 30.0, 20.0, kCellW, kCellH);
	REQUIRE_FALSE(wide.label.isNull());
	CHECK_FALSE(wide.avatar_dropped);
	CHECK_FALSE(wide.elided);

	// 名前に取る最小の幅 (6 em = 180) が入らない幅。
	const int narrow_w = 200;
	const CompositeLabel narrow = paint_composite_label(bottom_left_with_avatar("Taro Yamada"),
							    look, 30.0, 20.0, narrow_w, kCellH);
	REQUIRE_FALSE(narrow.label.isNull());
	CHECK(narrow.avatar_dropped);
	CHECK(narrow.box_w <= narrow_w - 40);
	CHECK(narrow.elided);

	// 同じ名前の人がほかにも見えているときは、アバターを残す。
	LabelContent twin = bottom_left_with_avatar("Taro Yamada");
	twin.other_visible_names = {"Taro Yamada"};
	const CompositeLabel kept = paint_composite_label(twin, look, 30.0, 20.0, narrow_w, kCellH);
	CHECK_FALSE(kept.avatar_dropped);
}

TEST_CASE("アバターの形を変えると、四隅の近くが、丸では透明、角丸では角だけ透明、四角では覆われる")
{
	CompositeLabelLook look = test_look();
	look.show_outline = false; // 縁取りがあると、形の外側に線が出る
	const auto corner_alpha = [&](cordmux::config::AvatarShape shape, double inset) {
		look.avatar_shape = shape;
		const CompositeLabel label = paint_composite_label(bottom_left_with_avatar("Taro"), look, 40.0, 20.0, kCellW,
								   kCellH);
		REQUIRE_FALSE(label.label.isNull());
		REQUIRE(label.avatar_d > 0);
		// アバターの左上の角から、直径の inset 倍だけ内側の点。
		const int x = label.avatar_x - label.label_x + static_cast<int>(label.avatar_d * inset);
		const int y = label.avatar_y - label.label_y + static_cast<int>(label.avatar_d * inset);
		return qAlpha(label.label.pixel(x, y));
	};
	// 角のすぐ内側 (直径の 3%) は、四角だけ覆われる。角丸の角の半径は直径の 1/4 なので、12% の点は覆われる。
	CHECK(corner_alpha(cordmux::config::AvatarShape::Circle, 0.03) == 0);
	CHECK(corner_alpha(cordmux::config::AvatarShape::Rounded, 0.03) == 0);
	CHECK(corner_alpha(cordmux::config::AvatarShape::Square, 0.03) == 255);
	CHECK(corner_alpha(cordmux::config::AvatarShape::Circle, 0.12) == 0);
	CHECK(corner_alpha(cordmux::config::AvatarShape::Rounded, 0.12) == 255);
	CHECK(corner_alpha(cordmux::config::AvatarShape::Square, 0.12) == 255);

	// アバターの画像がないときの頭文字も、同じ形で描く。
	look.avatar_shape = cordmux::config::AvatarShape::Square;
	const CompositeLabel initial =
		paint_composite_label(content_of("Taro", DecorCorner::BottomLeft, false), look, 40.0, 20.0, kCellW, kCellH);
	REQUIRE(initial.initial_drawn);
	CHECK(qAlpha(initial.label.pixel(initial.avatar_x - initial.label_x + 1, initial.avatar_y - initial.label_y + 1)) ==
	      255);
}

TEST_CASE("名前の最大幅を決めると、セルが広くても名前をその幅に収め、アバターは省かない")
{
	CompositeLabelLook look = test_look();
	const LabelContent content = bottom_left_with_avatar("Taro Yamada the Very Long Name");
	const CompositeLabel free = paint_composite_label(content, look, 30.0, 20.0, kCellW, kCellH);
	REQUIRE_FALSE(free.label.isNull());
	CHECK_FALSE(free.elided);

	// 4 em (120 ピクセル) に収める。アバターを省くかはセルの幅で決めるので、アバターは残る。
	look.max_name_em = 4.0;
	const CompositeLabel capped = paint_composite_label(content, look, 30.0, 20.0, kCellW, kCellH);
	REQUIRE_FALSE(capped.label.isNull());
	CHECK(capped.elided);
	CHECK_FALSE(capped.avatar_dropped);
	CHECK(capped.name_room_px == doctest::Approx(120.0));
	CHECK(capped.box_w < free.box_w);

	// 上限より短い名前は、そのまま描く。
	const CompositeLabel short_name =
		paint_composite_label(bottom_left_with_avatar("Taro"), look, 30.0, 20.0, kCellW, kCellH);
	CHECK_FALSE(short_name.elided);
}

TEST_CASE("見える文字が少なくなる名前は描画せず、アバターだけを残す")
{
	const CompositeLabelLook look = test_look();
	// 名前に使える幅が 30 ピクセルしかなく、省略すると見える文字が 2 つ残らない幅。
	const CompositeLabel label = paint_composite_label(bottom_left_with_avatar("Taro Yamada"),
							   look, 30.0, 20.0, 70, kCellH);
	CHECK(label.name_hidden);
	CHECK(label.name_drawn.isEmpty());
	REQUIRE_FALSE(label.label.isNull());
	CHECK_FALSE(label.avatar_dropped);
	// 矩形は、縁取りを含むアバターの幅 (直径 + 縁取りの見える太さ x 2)。
	REQUIRE((look.outline_argb >> 24) != 0);
	CHECK(label.box_w ==
	      static_cast<int>(std::ceil(std::lround(look.avatar_ratio * 30.0) + look.outline_ratio * 30.0)));
}

TEST_CASE("アバターの直径は、名前の大きさに倍率を掛けた値になり、画像がなければ頭文字の円になる")
{
	CompositeLabelLook look = test_look();
	look.show_name = false;
	look.opacity = 1.0;
	look.avatar_ratio = 2.0;
	look.outline_ratio = 0.0;
	const CompositeLabel image_avatar = paint_composite_label(bottom_left_with_avatar("Taro"),
								   look, 30.0, 20.0, kCellW, kCellH);
	REQUIRE_FALSE(image_avatar.label.isNull());
	CHECK_FALSE(image_avatar.initial_drawn);
	CHECK(image_avatar.box_w == 60);
	CHECK(image_avatar.box_h == 60);
	const QRect circle = ink_bounds(image_avatar.label);
	CHECK(circle.width() == 60);
	CHECK(circle.height() == 60);

	const CompositeLabel initial = paint_composite_label(content_of("Taro", DecorCorner::BottomLeft, false), look,
							     30.0, 20.0, kCellW, kCellH);
	REQUIRE_FALSE(initial.label.isNull());
	CHECK(initial.initial_drawn);
	CHECK(initial.box_w == 60);

	// 縁取りを付けると、円の外へ線の太さの半分 (30 x 0.2 / 2 = 3 ピクセル) だけ広がり、矩形もその分だけ広がる。
	// 円の直径は変わらない。頭文字の円も同じ。
	look.outline_ratio = 0.2;
	const CompositeLabel outlined =
		paint_composite_label(bottom_left_with_avatar("Taro"), look, 30.0, 20.0, kCellW, kCellH);
	CHECK(outlined.avatar_d == 60);
	CHECK(outlined.box_w == 66);
	CHECK(ink_bounds(outlined.label).width() >= 65);
	CHECK(ink_bounds(outlined.label).width() <= 67);
	const CompositeLabel initial_outlined =
		paint_composite_label(content_of("Taro", DecorCorner::BottomLeft, false), look, 30.0, 20.0, kCellW, kCellH);
	CHECK(ink_bounds(initial_outlined.label).width() == ink_bounds(outlined.label).width());
	look.outline_ratio = 0.0;

	// 名前もアバターの画像もなければ、円を描画しない。
	const CompositeLabel none = paint_composite_label(content_of("", DecorCorner::BottomLeft, false), look, 30.0,
							  20.0, kCellW, kCellH);
	CHECK(none.label.isNull());
	CHECK(none.glow.isNull());
}

TEST_CASE("縁取りを太くしても、アバターと名前の間は詰まらず、縁取りは背景の外へ出ない")
{
	// アバターの見える縁 (縁取りの外側) と、文字の見える縁の間の、何も描いていない列の数。
	const auto gap_columns = [](const CompositeLabel &label) {
		const QImage &image = label.label;
		const int avatar_right = label.avatar_x + label.avatar_d - label.label_x;
		std::vector<bool> used(static_cast<std::size_t>(image.width()), false);
		for (int y = 0; y < image.height(); ++y)
			for (int x = 0; x < image.width(); ++x)
				if (qAlpha(image.pixel(x, y)) != 0)
					used[static_cast<std::size_t>(x)] = true;
		int x = std::max(0, avatar_right);
		while (x < image.width() && used[static_cast<std::size_t>(x)])
			++x;
		int empty = 0;
		while (x < image.width() && !used[static_cast<std::size_t>(x)]) {
			++empty;
			++x;
		}
		return empty;
	};

	CompositeLabelLook thin = test_look();
	thin.opacity = 1.0;
	thin.outline_ratio = 0.0;
	CompositeLabelLook thick = thin;
	thick.outline_ratio = 0.4;
	const CompositeLabel a =
		paint_composite_label(bottom_left_with_avatar("Taro Yamada"), thin, 30.0, 20.0, kCellW, kCellH);
	const CompositeLabel b =
		paint_composite_label(bottom_left_with_avatar("Taro Yamada"), thick, 30.0, 20.0, kCellW, kCellH);
	REQUIRE_FALSE(a.label.isNull());
	REQUIRE_FALSE(b.label.isNull());
	const int gap_a = gap_columns(a);
	const int gap_b = gap_columns(b);
	// 間は 0.38 em (約 11 ピクセル)。縁取りの見える太さ (6 ピクセル) が両側から食い込むと 0 近くまで詰まる。
	CHECK(gap_a >= 8);
	CHECK(std::abs(gap_b - gap_a) <= 2);

	// 背景を出したとき、縁取りを含むアバターと文字は背景の矩形の中に収まる (アバターの縁取りの見える太さ 6 ピクセルは
	// 背景の上下の余白 5 ピクセルより太い)。
	CompositeLabelLook plate = thick;
	plate.show_plate = true;
	const CompositeLabel p =
		paint_composite_label(bottom_left_with_avatar("Taro Yamada"), plate, 30.0, 20.0, kCellW, kCellH);
	REQUIRE_FALSE(p.label.isNull());
	const QRect ink = ink_bounds(p.label).translated(p.label_x, p.label_y);
	CHECK(QRect(p.box_x, p.box_y, p.box_w, p.box_h).contains(ink));
}

TEST_CASE("名前もアバターも描画しない設定、大きさが 0 以下、不透明度 0 では、画像が空になる")
{
	CompositeLabelLook look = test_look();
	look.show_name = false;
	look.show_avatar = false;
	CHECK(paint_composite_label(bottom_left_with_avatar("Taro"), look, 30.0, 20.0, kCellW, kCellH)
		      .label.isNull());

	look = test_look();
	CHECK(paint_composite_label(bottom_left_with_avatar("Taro"), look, 0.0, 20.0, kCellW, kCellH)
		      .label.isNull());
	CHECK(paint_composite_label(bottom_left_with_avatar("Taro"), look, 30.0, 20.0, 0, kCellH)
		      .label.isNull());

	look.opacity = 0.0;
	const CompositeLabel hidden = paint_composite_label(bottom_left_with_avatar("Taro"), look, 30.0,
							    20.0, kCellW, kCellH);
	CHECK(hidden.label.isNull());
	CHECK(hidden.glow.isNull());
}

TEST_CASE("名前ラベル全体の不透明度は名前ラベルとグローの両方に掛かり、グローは名前ラベルが覆う所で抜けている")
{
	CompositeLabelLook look = test_look();
	look.opacity = 1.0;
	look.glow_blur_ratio = 0.5;
	const CompositeLabel full = paint_composite_label(bottom_left_with_avatar("Taro Yamada"), look,
							  30.0, 60.0, kCellW, kCellH);
	look.opacity = 0.5;
	const CompositeLabel half = paint_composite_label(bottom_left_with_avatar("Taro Yamada"), look,
							  30.0, 60.0, kCellW, kCellH);
	REQUIRE_FALSE(full.label.isNull());
	REQUIRE_FALSE(half.label.isNull());
	REQUIRE_FALSE(full.glow.isNull());
	REQUIRE_FALSE(half.glow.isNull());
	CHECK(max_alpha(full.label) == 255);
	CHECK(max_alpha(half.label) == 128);
	CHECK(premultiplied_is_valid(half.label));
	CHECK(premultiplied_is_valid(half.glow));
	CHECK(full.glow_x == half.glow_x);
	CHECK(full.glow_y == half.glow_y);
	// 名前ラベルが不透明に覆う所 (アバターの中心) には、グローがない。
	const int cx = full.avatar_x + full.avatar_d / 2 - full.glow_x;
	const int cy = full.avatar_y + full.avatar_d / 2 - full.glow_y;
	CHECK(qAlpha(full.glow.pixel(cx, cy)) == 0);
	CHECK(qAlpha(half.glow.pixel(cx, cy)) == 0);
	// 名前ラベルから離れた所 (アバターの左 4 ピクセル) では、グローのアルファは不透明度の分だけ下がる
	// (8 ビットの丸めの差まで)。
	const int x = full.avatar_x - 4 - full.glow_x;
	const int a_full = qAlpha(full.glow.pixel(x, cy));
	const int a_half = qAlpha(half.glow.pixel(x, cy));
	CHECK(a_full > 50);
	CHECK(std::abs(a_half * 255 - a_full * 128) <= 255);
}

TEST_CASE("グローは、光らせる対象に選んだものだけを含み、広がりとぼかしの範囲が画像に収まる")
{
	CompositeLabelLook look = test_look();
	look.opacity = 1.0;
	const LabelContent content = bottom_left_with_avatar("Taro Yamada");

	look.glow_avatar = false;
	look.glow_name = false;
	CHECK(paint_composite_label(content, look, 30.0, 100.0, kCellW, kCellH).glow.isNull());

	look.glow_avatar = true;
	const CompositeLabel avatar_only = paint_composite_label(content, look, 30.0, 100.0, kCellW, kCellH);
	look.glow_avatar = false;
	look.glow_name = true;
	const CompositeLabel name_only = paint_composite_label(content, look, 30.0, 100.0, kCellW, kCellH);
	look.glow_avatar = true;
	const CompositeLabel both = paint_composite_label(content, look, 30.0, 100.0, kCellW, kCellH);
	REQUIRE_FALSE(avatar_only.glow.isNull());
	REQUIRE_FALSE(name_only.glow.isNull());
	REQUIRE_FALSE(both.glow.isNull());

	// アバターだけのグローは、アバターの円の周りに収まる (名前の位置まで届かない)。両方のグローは 2 つの範囲を
	// 合わせた範囲になる (ぼかしの裾の 8 ビットの丸めで、1 ピクセルまで広がることがある)。画像は透明でない
	// ピクセルを囲む大きさに切り詰めてある。
	const auto rect_of = [](const CompositeLabel &l) {
		return QRect(l.glow_x, l.glow_y, l.glow.width(), l.glow.height());
	};
	const QRect avatar_ink = rect_of(avatar_only);
	const QRect name_ink = rect_of(name_only);
	const QRect both_ink = rect_of(both);
	CHECK(avatar_ink.width() < name_ink.width());
	const QRect united = avatar_ink.united(name_ink);
	CHECK(both_ink.contains(united));
	CHECK(united.adjusted(-1, -1, 1, 1).contains(both_ink));
	CHECK(ink_bounds(both.glow) == QRect(0, 0, both.glow.width(), both.glow.height()));

	// グローの画像は、名前ラベルの見える範囲から、広がりとぼかしの標準偏差の 3 倍の範囲に収まる。
	const int reach = static_cast<int>(std::ceil(look.glow_spread_ratio * 30.0)) + 1 +
			  static_cast<int>(std::ceil(look.glow_blur_ratio * 30.0 / 2.0 * 3.0));
	const QRect label_ink(both.label_x, both.label_y, both.label.width(), both.label.height());
	CHECK(label_ink.adjusted(-reach, -reach, reach, reach).contains(both_ink));
	for (const CompositeLabel *l : {&avatar_only, &name_only, &both})
		CHECK(premultiplied_is_valid(l->glow));

	// 広がりもぼかしも 0 なら、グローは見える形そのものになって見えないので、描かない。
	look.glow_spread_ratio = 0.0;
	look.glow_blur_ratio = 0.0;
	const CompositeLabel sharp = paint_composite_label(content, look, 30.0, 100.0, kCellW, kCellH);
	CHECK(sharp.glow.isNull());
	CHECK_FALSE(sharp.label.isNull());
}

TEST_CASE("グローはグローの色で塗られ、広がりを大きくするとグローの範囲が広がる")
{
	CompositeLabelLook look = test_look();
	look.opacity = 1.0;
	look.glow_name = false;
	look.glow_argb = 0xFF2040C0u;
	look.glow_blur_ratio = 0.0;
	const LabelContent content = bottom_left_with_avatar("Taro");
	look.glow_spread_ratio = 0.1;
	const CompositeLabel narrow = paint_composite_label(content, look, 30.0, 100.0, kCellW, kCellH);
	look.glow_spread_ratio = 0.5;
	const CompositeLabel wide = paint_composite_label(content, look, 30.0, 100.0, kCellW, kCellH);
	REQUIRE_FALSE(narrow.glow.isNull());
	REQUIRE_FALSE(wide.glow.isNull());
	CHECK(ink_bounds(wide.glow).width() > ink_bounds(narrow.glow).width() + 20);
	// アバターの左の縁から、縁取りの外へさらに 7 ピクセル (広がり 15 ピクセルの半分ほど) 出たピクセルは、
	// グローの色そのもの。
	const int outside = static_cast<int>(std::ceil(look.outline_ratio * 30.0 / 2.0)) + 7;
	const int x = wide.avatar_x - outside - wide.glow_x;
	const int y = wide.avatar_y + wide.avatar_d / 2 - wide.glow_y;
	const QRgb ring = wide.glow.pixel(x, y);
	CHECK(qAlpha(ring) == 255);
	CHECK(qRed(ring) == 0x20);
	CHECK(qGreen(ring) == 0x40);
	CHECK(qBlue(ring) == 0xC0);
	// アバターの中心には、グローがない (名前ラベルが覆う所は抜いてある)。
	CHECK(qAlpha(wide.glow.pixel(wide.avatar_x + wide.avatar_d / 2 - wide.glow_x, y)) == 0);
}

TEST_CASE("余白 0 でも、名前ラベルは縁取りまでセルの中に収まって矩形が隅に接し、グローは切り取らずにセルの外へ広がる")
{
	CompositeLabelLook look = test_look();
	look.outline_ratio = 0.2;
	for (const DecorCorner corner : {DecorCorner::TopLeft, DecorCorner::BottomRight}) {
		CAPTURE(static_cast<int>(corner));
		const CompositeLabel l = paint_composite_label(content_of("Taro Yamada", corner, true), look, 30.0, 0.0,
							      kCellW, kCellH);
		REQUIRE_FALSE(l.label.isNull());
		REQUIRE_FALSE(l.glow.isNull());
		CHECK(l.label_x >= 0);
		CHECK(l.label_y >= 0);
		CHECK(l.label_x + l.label.width() <= kCellW);
		CHECK(l.label_y + l.label.height() <= kCellH);
		// 矩形は縁取りを含むので、矩形が隅に接する。見える縁は、字形の右の空きの分だけ内側になることがある。
		if (corner == DecorCorner::TopLeft) {
			CHECK(l.box_x == 0);
			CHECK(l.box_y == 0);
			CHECK(l.label_x == 0);
			CHECK(l.glow_x < 0);
			CHECK(l.glow_y < 0);
		} else {
			CHECK(l.box_x + l.box_w == kCellW);
			CHECK(l.box_y + l.box_h == kCellH);
			CHECK(l.glow_x + l.glow.width() > kCellW);
			CHECK(l.glow_y + l.glow.height() > kCellH);
		}
	}
}

TEST_CASE("セルより高い名前ラベルは、見える部分の上をセルの上の端に合わせる")
{
	const CompositeLabelLook look = test_look();
	const CompositeLabel l = paint_composite_label(content_of("Taro", DecorCorner::BottomRight, true), look, 30.0,
						      10.0, kCellW, 40);
	REQUIRE_FALSE(l.label.isNull());
	CHECK(l.label_y == 0);
	CHECK(l.label.height() > 40);
}

TEST_CASE("同じ入力からは同じピクセルが描画される")
{
	const CompositeLabelLook look = test_look();
	const LabelContent content = content_of("Hanako 2", DecorCorner::TopRight, true);
	const CompositeLabel a = paint_composite_label(content, look, 33.7, 24.0, kCellW, kCellH);
	const CompositeLabel b = paint_composite_label(content, look, 33.7, 24.0, kCellW, kCellH);
	REQUIRE_FALSE(a.label.isNull());
	CHECK(a.label == b.label);
	CHECK(a.glow == b.glow);
	CHECK(a.label_x == b.label_x);
	CHECK(a.glow_y == b.glow_y);
	CHECK(a.label.format() == QImage::Format_ARGB32_Premultiplied);
	CHECK(paint_frame_glow(160, 90, 0xFF57F287u, 10.0, 2.0) == paint_frame_glow(160, 90, 0xFF57F287u, 10.0, 2.0));
}

TEST_CASE("枠の内側のグローは、セルと同じ大きさで、端がグローの色で、中心は透明で、広がりが大きいと全体がグローになる")
{
	const QImage glow = paint_frame_glow(320, 180, 0xFF57F287u, 12.0, 3.0);
	REQUIRE(glow.width() == 320);
	REQUIRE(glow.height() == 180);
	CHECK(glow.format() == QImage::Format_ARGB32_Premultiplied);
	CHECK(premultiplied_is_valid(glow));
	// 辺の真ん中の端のピクセルは、グローの色を乗算済みアルファにしたもの。
	const QRgb edge = glow.pixel(0, 90);
	CHECK(qAlpha(edge) > 150);
	CHECK(qRed(edge) == (0x57 * qAlpha(edge) + 127) / 255);
	CHECK(qGreen(edge) == (0xF2 * qAlpha(edge) + 127) / 255);
	CHECK(qBlue(edge) == (0x87 * qAlpha(edge) + 127) / 255);
	CHECK(qAlpha(glow.pixel(160, 90)) == 0);

	const QImage all = paint_frame_glow(40, 20, 0xFF57F287u, 4.0, 12.0);
	for (int y = 0; y < all.height(); ++y)
		for (int x = 0; x < all.width(); ++x)
			CHECK(all.pixel(x, y) == qRgba(0x57, 0xF2, 0x87, 255));

	// 色のアルファも掛かる。
	const QImage faint = paint_frame_glow(40, 20, 0x80FFFFFFu, 0.0, 12.0);
	CHECK(faint.pixel(5, 5) == qRgba(128, 128, 128, 128));

	CHECK(paint_frame_glow(0, 90, 0xFF57F287u, 12.0, 3.0).isNull());
	CHECK(paint_frame_glow(320, -1, 0xFF57F287u, 12.0, 3.0).isNull());
}

TEST_CASE("グローを描画する時間の目安を記録する (1920 x 1080 の枠と、1080p の名前ラベルのグロー)")
{
	QElapsedTimer timer;
	timer.start();
	const QImage frame = paint_frame_glow(1920, 1080, 0xFF57F287u, 24.0, 4.0);
	const qint64 frame_ms = timer.elapsed();
	REQUIRE_FALSE(frame.isNull());

	// 1080p で名前の大きさ 2.5% は 27 px、最大の 8.5% は 92 px。ぼかしと広がりは、既定の値 (どちらも 0.1) と、
	// 大きい値と、範囲の上の端。
	struct Case {
		double name_px;
		double blur;
		double spread;
	};
	const Case cases[] = {{27.0, 0.1, 0.1}, {92.0, 0.1, 0.1}, {92.0, 0.75, 0.5}, {92.0, 3.0, 1.0}};
	qint64 label_ms[4] = {0, 0, 0, 0};
	for (int i = 0; i < 4; ++i) {
		CompositeLabelLook look = test_look();
		look.glow_blur_ratio = cases[i].blur;
		look.glow_spread_ratio = cases[i].spread;
		timer.restart();
		const CompositeLabel l = paint_composite_label(bottom_left_with_avatar("Taro Yamada"), look,
							      cases[i].name_px, 30.0, 960, 540);
		label_ms[i] = timer.elapsed();
		REQUIRE_FALSE(l.glow.isNull());
	}
	MESSAGE("frame glow 1920x1080: " << frame_ms << " ms; label name_px=27 blur=0.1 spread=0.1: " << label_ms[0]
					 << " ms; name_px=92 blur=0.1 spread=0.1: " << label_ms[1]
					 << " ms; name_px=92 blur=0.75 spread=0.5: " << label_ms[2]
					 << " ms; name_px=92 blur=3.0 spread=1.0: " << label_ms[3] << " ms");
	// 遅い PC でも 1 枠の描画が秒単位にならない目安。通常はこの 1 割以下で終わる。
	CHECK(frame_ms < 3000);
	CHECK(label_ms[0] < 3000);
	CHECK(label_ms[1] < 10000);
	CHECK(label_ms[2] < 10000);
	CHECK(label_ms[3] < 30000);
}

namespace {

// アバターの位置を右にした見た目。
CompositeLabelLook right_look(const CompositeLabelLook &left)
{
	CompositeLabelLook look = left;
	look.avatar_side = cordmux::config::AvatarSide::Right;
	return look;
}

// 背景ありで、グローなしの見た目。
CompositeLabelLook plate_look()
{
	CompositeLabelLook look = test_look();
	look.show_plate = true;
	look.plate_argb = 0xFF202020u;
	look.glow_blur_ratio = 0.0;
	look.glow_spread_ratio = 0.0;
	return look;
}

// 名前ラベルの画像の中の矩形 (セルの座標) を、画像の座標にして切り出す。
QImage crop_in_label(const CompositeLabel &label, int x, int y, int w, int h)
{
	return label.label.copy(x - label.label_x, y - label.label_y, w, h);
}

} // namespace

TEST_CASE("アバターの位置を右にすると、名前ラベルの大きさは同じで、アバターが右端、名前がその左に並ぶ")
{
	const CompositeLabelLook left_look = plate_look();
	const LabelContent content = content_of("Mika Kimura", DecorCorner::TopLeft, true);
	const CompositeLabel left = paint_composite_label(content, left_look, 32.0, 20.0, kCellW, kCellH);
	const CompositeLabel right = paint_composite_label(content, right_look(left_look), 32.0, 20.0, kCellW, kCellH);
	REQUIRE_FALSE(left.label.isNull());
	REQUIRE_FALSE(right.label.isNull());

	CHECK(right.box_w == left.box_w);
	CHECK(right.box_h == left.box_h);
	CHECK(right.label.size() == left.label.size());
	CHECK(right.avatar_d == left.avatar_d);
	CHECK(right.name_drawn == left.name_drawn);
	CHECK(right.box_x == left.box_x);
	CHECK(right.box_y == left.box_y);

	// アバターのある側の余白 (背景の端からアバターまで) は、左右で同じ (小数の位置を丸める分の 1 ピクセルを許す)。
	const int left_pad = left.avatar_x - left.box_x;
	const int right_pad = (right.box_x + right.box_w) - (right.avatar_x + right.avatar_d);
	CHECK(std::abs(left_pad - right_pad) <= 1);

	// アバターの絵は左右反転せず、同じ位置に同じピクセルがある (円の周りの縁取りも同じ)。
	const int d = left.avatar_d;
	const QImage left_avatar = crop_in_label(left, left.avatar_x, left.avatar_y, d, d);
	const QImage right_avatar = crop_in_label(right, right.avatar_x, right.avatar_y, d, d);
	CHECK(left_avatar == right_avatar);

	// 文字の塗り (白) は、左のときはアバターの右に、右のときはアバターの左にある。
	const QRect left_text = fill_bounds(left.label);
	const QRect right_text = fill_bounds(right.label);
	REQUIRE_FALSE(left_text.isNull());
	REQUIRE_FALSE(right_text.isNull());
	CHECK(left_text.left() + left.label_x >= left.avatar_x + d);
	CHECK(right_text.right() + right.label_x < right.avatar_x);

	// 名前側の余白 (背景の端から文字の塗りまで) は plate_pad_right_em (0.5 em = 16 px) に近く、左のときの右の余白と
	// 同じ値になる (文字の端の字形の余白の違いを許す)。
	const int left_text_pad = (left.box_x + left.box_w) - (left_text.right() + left.label_x + 1);
	const int right_text_pad = (right_text.left() + right.label_x) - right.box_x;
	CHECK(std::abs(left_text_pad - right_text_pad) <= 4);
	CHECK(right_text_pad >= 12);
	CHECK(right_text_pad <= 22);
}

TEST_CASE("アバターの位置が右でも、名前がないアバターだけの名前ラベルは左のときと同じ画像になる")
{
	CompositeLabelLook left_look = plate_look();
	left_look.show_name = false;
	const LabelContent content = content_of("Mika Kimura", DecorCorner::TopLeft, true);
	const CompositeLabel left = paint_composite_label(content, left_look, 32.0, 20.0, kCellW, kCellH);
	const CompositeLabel right = paint_composite_label(content, right_look(left_look), 32.0, 20.0, kCellW, kCellH);
	REQUIRE_FALSE(left.label.isNull());
	CHECK(left.label == right.label);
	CHECK(left.label_x == right.label_x);
	CHECK(left.avatar_x == right.avatar_x);
}

TEST_CASE("アバターの位置が右で、アバターを表示しないときは、背景の内側の余白の両側が左のときと同じ幅になる")
{
	CompositeLabelLook left_look = plate_look();
	left_look.show_avatar = false;
	const LabelContent content = content_of("Mika Kimura", DecorCorner::TopLeft, false);
	const CompositeLabel left = paint_composite_label(content, left_look, 32.0, 20.0, kCellW, kCellH);
	const CompositeLabel right = paint_composite_label(content, right_look(left_look), 32.0, 20.0, kCellW, kCellH);
	REQUIRE_FALSE(right.label.isNull());
	CHECK(right.box_w == left.box_w);
	CHECK(right.avatar_d == 0);
	CHECK(right.name_drawn == left.name_drawn);
}

TEST_CASE("アバターの位置が右でも、名前の省略とアバターを省く判断は左のときと同じ")
{
	const LabelContent content = content_of("Alexander Featherstonehaugh-Montgomery", DecorCorner::TopLeft, true);
	for (const int cell_w : {640, 300, 200, 120}) {
		CAPTURE(cell_w);
		const CompositeLabelLook left_look = plate_look();
		const CompositeLabel left = paint_composite_label(content, left_look, 32.0, 10.0, cell_w, kCellH);
		const CompositeLabel right = paint_composite_label(content, right_look(left_look), 32.0, 10.0, cell_w, kCellH);
		CHECK(right.name_drawn == left.name_drawn);
		CHECK(right.elided == left.elided);
		CHECK(right.elided_middle == left.elided_middle);
		CHECK(right.avatar_dropped == left.avatar_dropped);
		CHECK(right.name_hidden == left.name_hidden);
		CHECK(right.box_w == left.box_w);
		CHECK(right.label.size() == left.label.size());
	}
}

TEST_CASE("アバターの位置が右のときのグローの形は、同じ配置から作るので左右入れ替えた形になる")
{
	for (const double outline : {0.12, CompositeLabelLook{}.outline_ratio})
	for (const bool glow_avatar : {true, false}) {
		CAPTURE(glow_avatar);
		CAPTURE(outline);
		CompositeLabelLook left_look = test_look(); // 背景なし
		left_look.outline_ratio = outline;
		left_look.glow_avatar = glow_avatar;
		left_look.glow_name = !glow_avatar;
		const LabelContent content = content_of("Mika Kimura", DecorCorner::TopLeft, true);
		const CompositeLabel left = paint_composite_label(content, left_look, 32.0, 40.0, kCellW, kCellH);
		const CompositeLabel right = paint_composite_label(content, right_look(left_look), 32.0, 40.0, kCellW, kCellH);
		REQUIRE_FALSE(left.glow.isNull());
		REQUIRE_FALSE(right.glow.isNull());
		const double left_center = left.glow_x + left.glow.width() / 2.0;
		const double right_center = right.glow_x + right.glow.width() / 2.0;
		const double left_avatar_center = left.avatar_x + left.avatar_d / 2.0;
		const double right_avatar_center = right.avatar_x + right.avatar_d / 2.0;
		if (glow_avatar) {
			// アバターの光は、アバターを中心にした円の形。
			CHECK(std::abs(left_center - left_avatar_center) <= 2.0);
			CHECK(std::abs(right_center - right_avatar_center) <= 2.0);
		} else {
			// 名前の光は、左のときはアバターの右に、右のときはアバターの左にある。
			CHECK(left_center > left_avatar_center);
			CHECK(right_center < right_avatar_center);
		}
		// グローの画像は、見える画素の外接矩形で切り出す (composite_label.cpp の visible_bounds)。左右で名前の文字の位置の
		// 端数が違うと、縁の薄い画素が 1 列だけ増減するので、大きさは 1 px まで違ってよい (縁取りの太さが 0.1 em のとき、
		// 名前の光の幅が 208 px と 207 px になる)。
		CAPTURE(left.glow.width());
		CAPTURE(left.glow.height());
		CAPTURE(right.glow.width());
		CAPTURE(right.glow.height());
		CHECK(std::abs(right.glow.width() - left.glow.width()) <= 1);
		CHECK(std::abs(right.glow.height() - left.glow.height()) <= 1);
	}
}

TEST_CASE("画面共有していない参加者の名前ラベルを、名前の幅の上限の幅と余白 0 のセルで描くと、4 等分の枠の名前ラベルと同じ大きさになる")
{
	// 1920 x 1080 のキャンバスの 4 等分の枠 (幅 960) の余白は 10.8 px。並びは、同じ上限 (960 - 21.6 を切り捨てた 938) を
	// セルの幅にして、余白 0 で描く。
	constexpr double kMargin = 10.8;
	const int cap = cordmux::sources::composite_roster_cap_width(1920, kMargin);
	REQUIRE(cap == 938);
	const CompositeLabelLook look = plate_look();
	for (const char *name : {"Mika", "Noa Kimura", "Alexander Featherstonehaugh-Montgomery"}) {
		CAPTURE(name);
		const LabelContent content = content_of(name, DecorCorner::TopLeft, true);
		const CompositeLabel tile = paint_composite_label(content, look, 23.76, kMargin, 960, 540);
		const CompositeLabel roster = paint_composite_label(content, look, 23.76, 0.0, cap, 1080);
		REQUIRE_FALSE(roster.label.isNull());
		CHECK(roster.box_w == tile.box_w);
		CHECK(roster.box_h == tile.box_h);
		CHECK(roster.name_drawn == tile.name_drawn);
		CHECK(roster.box_w <= cap);
		// 名前ラベルの矩形は、余白 0 のセルの左上の隅に接する。
		CHECK(roster.box_x == 0);
		CHECK(roster.box_y == 0);
	}
	// 上限を超える名前は、末尾を省略して上限の幅に収まる。
	const LabelContent too_long = content_of(
		"Alexander Featherstonehaugh-Montgomery Alexander Featherstonehaugh-Montgomery Alexander Featherstonehaugh",
		DecorCorner::TopLeft, true);
	const CompositeLabel clipped = paint_composite_label(too_long, look, 23.76, 0.0, cap, 1080);
	CHECK(clipped.elided);
	CHECK(clipped.box_w <= cap);
}

TEST_CASE("名前、アバター、背景の表示を別々に外すと、外したコンポーネントだけを描かない")
{
	const LabelContent content = bottom_left_with_avatar("Taro");
	const CompositeLabel both = paint_composite_label(content, test_look(), 30.0, 20.0, kCellW, kCellH);
	REQUIRE_FALSE(both.label.isNull());
	CHECK(both.name_drawn == QStringLiteral("Taro"));
	CHECK(both.avatar_d > 0);

	// 名前の表示だけを外すと、アバターだけが残る。
	CompositeLabelLook look = test_look();
	look.show_name = false;
	const CompositeLabel avatar_only = paint_composite_label(content, look, 30.0, 20.0, kCellW, kCellH);
	REQUIRE_FALSE(avatar_only.label.isNull());
	CHECK(avatar_only.name_drawn.isEmpty());
	CHECK(avatar_only.avatar_d > 0);
	CHECK(avatar_only.box_w < both.box_w);

	// アバターの表示だけを外すと、名前だけが残る。
	look = test_look();
	look.show_avatar = false;
	const CompositeLabel name_only = paint_composite_label(content, look, 30.0, 20.0, kCellW, kCellH);
	REQUIRE_FALSE(name_only.label.isNull());
	CHECK(name_only.name_drawn == QStringLiteral("Taro"));
	CHECK(name_only.avatar_d == 0);
	CHECK(name_only.box_w < both.box_w);

	// 背景は既定では描かず、表示を入れたときだけ、名前とアバターの周りに余白を足した矩形になる。
	look = test_look();
	look.show_plate = true;
	const CompositeLabel plate = paint_composite_label(content, look, 30.0, 20.0, kCellW, kCellH);
	REQUIRE_FALSE(plate.label.isNull());
	CHECK(plate.box_w > both.box_w);
	CHECK(plate.box_h > both.box_h);
}

TEST_CASE("書記素の数は、漢字と結合した絵文字と国旗と結合文字をそれぞれ 1 文字と数える")
{
	CHECK(grapheme_count(QString()) == 0);
	CHECK(grapheme_count(QStringLiteral("Taro")) == 4);
	CHECK(grapheme_count(QStringLiteral("山田")) == 2);
	// 家族の絵文字 (人 + 結合子 + 人 + 結合子 + 人) は 1 文字。
	CHECK(grapheme_count(QStringLiteral("\U0001F468\u200D\U0001F469\u200D\U0001F467")) == 1);
	// 国旗 (地域指示記号 2 つ) は 1 文字。
	CHECK(grapheme_count(QStringLiteral("\U0001F1EF\U0001F1F5")) == 1);
	// e + 結合アクセントは 1 文字。
	CHECK(grapheme_count(QStringLiteral("e\u0301")) == 1);
	// サロゲートペアの絵文字 2 つは 2 文字。
	CHECK(grapheme_count(QStringLiteral("\U0001F600\U0001F600")) == 2);
}

TEST_CASE("省略した名前の見える部分は省略記号を除き、ほかの名前の先頭に一致するかで見分けられるかを決める")
{
	CHECK(visible_part(QStringLiteral("Taro\u2026")) == QStringLiteral("Taro"));
	CHECK(visible_part(QStringLiteral("Taro...")) == QStringLiteral("Taro"));
	CHECK(visible_part(QStringLiteral("Ta\u2026ro")) == QStringLiteral("Taro"));
	CHECK(visible_part(QStringLiteral("Taro")) == QStringLiteral("Taro"));

	// 見えている先頭がほかの人の名前の先頭と同じなら、その名前ラベルは真ん中を省略して見分ける。
	CHECK(prefix_collides(QStringLiteral("Taro"), {"Taro Yamada"}));
	CHECK(prefix_collides(QStringLiteral("山"), {"山田太郎"}));
	CHECK_FALSE(prefix_collides(QStringLiteral("Taro"), {"Hanako"}));
	CHECK_FALSE(prefix_collides(QString(), {"Taro"}));
	CHECK_FALSE(prefix_collides(QStringLiteral("Taro"), {}));
}
