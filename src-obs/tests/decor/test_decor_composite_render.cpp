// SPDX-License-Identifier: Apache-2.0

// 合成のソースの名前ラベルと光を、製品の計画 (sources/composite_draw_plan.h) のとおりに CPU で重ねた画像
// (sources/composite_draw_cpu.h) の値で、見え方の性質を測るテスト。光の太さ (縦横と斜め、文字とアバター)、縁取りの
// 見える太さ、名前ラベル全体の不透明度の掛かり方、セルの境とキャンバスの端での切れ方、背景の周りの光、名前の
// 大きさを変えたときの比、ぼかしの薄れ方、明るさの掛かり方を確認する。
//
// 太さを測るときは、色をチャンネルごとに分ける (背景は黒、光は緑、縁取りは赤、文字は青、アバターは暗い青)。
// 外へ向かう線の上で、赤が半分を切る所を見える縁、緑が半分を切る所を光の外の縁とし、隣のピクセルの値から直線で
// 補って、ピクセルより細かい位置を求める。名前には、同梱のフォントで縦横の直線と直角の角だけからなる「I」を使う。

#include "doctest.h"

#include <QColor>
#include <QImage>
#include <QPainter>
#include <QRect>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "decor/decor_test_env.h"
#include "sources/composite_draw_cpu.h"
#include "sources/composite_draw_plan.h"
#include "sources/composite_label.h"

using cordmux::sources::CompositeDraw;
using cordmux::sources::CompositeDrawImage;
using cordmux::sources::CompositeDrawKind;
using cordmux::sources::CompositeDrawRequest;
using cordmux::sources::CompositeDrawTile;
using cordmux::sources::CompositeLabel;
using cordmux::sources::CompositeLabelLook;
using cordmux::sources::CompositeTileImages;
using cordmux::sources::DecorCorner;
using cordmux::sources::draw_composite_plan;
using cordmux::sources::LabelContent;
using cordmux::sources::paint_composite_label;
using cordmux::sources::paint_frame_glow;
using cordmux::sources::PixelRect;
using cordmux::sources::plan_composite_draws;
using cordmux::tests::decor_test_font_family;

namespace {

constexpr std::uint32_t kGlowGreen = 0xFF00FF00u;
constexpr std::uint32_t kOutlineRed = 0xFFFF0000u;
constexpr std::uint32_t kTextBlue = 0xFF0000FFu;
constexpr int kAvatarBlue = 200; // アバターの青。文字の青 (255) と分ける

// 測るための見た目。光はぼかし 0、広がりは名前の大きさの 0.1 倍。
CompositeLabelLook measure_look()
{
	CompositeLabelLook look;
	look.font_family = decor_test_font_family().toStdString();
	look.show_plate = false;
	look.text_argb = kTextBlue;
	look.outline_argb = kOutlineRed;
	look.outline_ratio = 0.1;
	look.glow_argb = kGlowGreen;
	look.glow_frame = false;
	look.glow_avatar = true;
	look.glow_name = true;
	look.glow_blur_ratio = 0.0;
	look.glow_spread_ratio = 0.1;
	look.opacity = 1.0;
	return look;
}

QImage solid_avatar()
{
	QImage avatar(64, 64, QImage::Format_ARGB32_Premultiplied);
	avatar.fill(QColor(0, 0, kAvatarBlue));
	return avatar;
}

LabelContent person(const char *name, bool with_avatar, DecorCorner corner = DecorCorner::TopLeft)
{
	LabelContent content;
	content.name = name;
	content.corner = corner;
	if (with_avatar)
		content.avatar = solid_avatar();
	return content;
}

// 1 つの場面。cells のタイルごとに people の人の名前ラベルを描き、計画を作って、background で塗ったキャンバスに
// 重ねる。名前ラベルの結果は labels に残る。
struct Scene {
	int canvas_w = 800;
	int canvas_h = 360;
	std::vector<PixelRect> cells{PixelRect{0, 0, 800, 360}};
	std::vector<LabelContent> people{person("I", true)};
	CompositeLabelLook look = measure_look();
	double name_px = 60.0;
	double margin_px = 40.0;
	double level = 1.0;
	QColor background = QColor(0, 0, 0);
	bool transparent = false; // 透明なキャンバスに重ねる (アルファを読むとき)

	std::vector<CompositeLabel> labels;
	std::vector<CompositeDraw> draws;

	QImage render()
	{
		labels.clear();
		CompositeDrawRequest request;
		request.canvas_w = canvas_w;
		request.canvas_h = canvas_h;
		request.parts.frame_glow = look.glow_frame;
		request.parts.label_glow = look.glow_avatar || look.glow_name;
		std::vector<CompositeTileImages> images;
		for (std::size_t i = 0; i < cells.size(); ++i) {
			const PixelRect &cell = cells[i];
			const CompositeLabel label =
				paint_composite_label(people[i % people.size()], look, name_px, margin_px, cell.w, cell.h);
			labels.push_back(label);
			CompositeTileImages tile_images;
			tile_images.label = label.label;
			tile_images.glow = label.glow;
			if (look.glow_frame)
				tile_images.frame = paint_frame_glow(cell.w, cell.h, look.glow_argb,
								     look.glow_blur_ratio * name_px,
								     look.glow_spread_ratio * name_px);
			CompositeDrawTile tile;
			tile.cell = cell;
			tile.has_video = true;
			tile.level = level;
			const auto image_of = [](const QImage &image, int x, int y) {
				CompositeDrawImage out;
				out.present = !image.isNull();
				out.x = x;
				out.y = y;
				out.w = image.width();
				out.h = image.height();
				return out;
			};
			tile.label = image_of(label.label, label.label_x, label.label_y);
			tile.glow = image_of(label.glow, label.glow_x, label.glow_y);
			tile.frame = image_of(tile_images.frame, 0, 0);
			tile.label_current = true;
			tile.frame_current = true;
			tile.label_owner = "100";
			tile.owner = "100";
			request.tiles.push_back(tile);
			images.push_back(tile_images);
		}
		draws = plan_composite_draws(request);
		QImage canvas(canvas_w, canvas_h, QImage::Format_ARGB32_Premultiplied);
		canvas.fill(transparent ? QColor(0, 0, 0, 0) : background);
		draw_composite_plan(&canvas, draws, images);
		return canvas;
	}
};

// 線の上の値。
struct Ray {
	std::vector<double> red;
	std::vector<double> green;
	double step = 1.0; // 1 つ進むときの距離 (ピクセル)
};

// ピクセル (x, y) の中心から、向き (dx, dy) へ 1/4 ピクセルずつ進み、まわりの 4 つのピクセルから補った値を読む
// (斜めの向きでも、縦横と同じ細かさで縁の位置を求めるため)。length ピクセル進むか、画像の外に出たら止める。
Ray sample_ray(const QImage &image, int x, int y, int dx, int dy, int length)
{
	Ray ray;
	ray.step = 0.25;
	const double norm = std::sqrt(static_cast<double>(dx * dx + dy * dy));
	const double ux = dx / norm;
	const double uy = dy / norm;
	for (int i = 0; i <= length * 4; ++i) {
		// ピクセルの中心が整数の位置になる座標。
		const double fx = x + ux * i * ray.step;
		const double fy = y + uy * i * ray.step;
		const int x0 = static_cast<int>(std::floor(fx));
		const int y0 = static_cast<int>(std::floor(fy));
		if (x0 < 0 || y0 < 0 || x0 + 1 >= image.width() || y0 + 1 >= image.height())
			break;
		const double tx = fx - x0;
		const double ty = fy - y0;
		const auto mix = [&](int (*channel)(QRgb)) {
			const double top = channel(image.pixel(x0, y0)) * (1.0 - tx) + channel(image.pixel(x0 + 1, y0)) * tx;
			const double bottom =
				channel(image.pixel(x0, y0 + 1)) * (1.0 - tx) + channel(image.pixel(x0 + 1, y0 + 1)) * tx;
			return top * (1.0 - ty) + bottom * ty;
		};
		ray.red.push_back(mix(qRed));
		ray.green.push_back(mix(qGreen));
	}
	return ray;
}

// ピクセル (x, y) から (dx, dy) ずつ進んだ count 個のピクセルの緑の値。
std::vector<int> green_pixels(const QImage &image, int x, int y, int dx, int dy, int count)
{
	std::vector<int> values;
	for (int i = 0; i < count; ++i) {
		const int px = x + dx * i;
		const int py = y + dy * i;
		if (px < 0 || py < 0 || px >= image.width() || py >= image.height())
			break;
		values.push_back(qGreen(image.pixel(px, py)));
	}
	return values;
}

// ピクセル (x, y) から (dx, dy) ずつ進んだ count 個のピクセルの赤の値。
std::vector<int> red_pixels(const QImage &image, int x, int y, int dx, int dy, int count)
{
	std::vector<int> values;
	for (int i = 0; i < count; ++i) {
		const int px = x + dx * i;
		const int py = y + dy * i;
		if (px < 0 || py < 0 || px >= image.width() || py >= image.height())
			break;
		values.push_back(qRed(image.pixel(px, py)));
	}
	return values;
}

// values が from の後で、half (127.5) を上から下へ切る最初の位置 (補った値)。見つからなければ負。
template <typename T> double falling_cross(const std::vector<T> &values, double from)
{
	for (std::size_t i = static_cast<std::size_t>(std::max(0.0, std::floor(from))); i + 1 < values.size(); ++i) {
		const double a = values[i];
		const double b = values[i + 1];
		if (a >= 127.5 && b < 127.5 && static_cast<double>(i) + (a - 127.5) / (a - b) >= from)
			return static_cast<double>(i) + (a - 127.5) / (a - b);
	}
	return -1.0;
}

// values が from の後で、half を下から上へ切る最初の位置。
template <typename T> double rising_cross(const std::vector<T> &values, double from)
{
	for (std::size_t i = static_cast<std::size_t>(std::max(0.0, std::floor(from))); i + 1 < values.size(); ++i) {
		const double a = values[i];
		const double b = values[i + 1];
		if (a < 127.5 && b >= 127.5 && static_cast<double>(i) + (127.5 - a) / (b - a) >= from)
			return static_cast<double>(i) + (127.5 - a) / (b - a);
	}
	return -1.0;
}

// 中から外へ向かう線の上で測った、縁取りの見える太さと光の太さ (ピクセル)。
struct Widths {
	double outline = -1.0;
	double glow = -1.0;
};

Widths measure(const Ray &ray)
{
	Widths widths;
	const double inner = rising_cross(ray.red, 0.0);
	if (inner < 0.0)
		return widths;
	const double edge = falling_cross(ray.red, inner);
	if (edge < 0.0)
		return widths;
	const double glow_edge = falling_cross(ray.green, edge);
	widths.outline = (edge - inner) * ray.step;
	if (glow_edge >= 0.0)
		widths.glow = (glow_edge - edge) * ray.step;
	return widths;
}

// 文字の塗り (純粋な青) のピクセルを囲む矩形。
QRect text_fill(const QImage &image)
{
	int left = image.width();
	int top = image.height();
	int right = -1;
	int bottom = -1;
	for (int y = 0; y < image.height(); ++y) {
		for (int x = 0; x < image.width(); ++x) {
			const QRgb p = image.pixel(x, y);
			if (qRed(p) == 0 && qGreen(p) == 0 && qBlue(p) == 255) {
				left = std::min(left, x);
				right = std::max(right, x);
				top = std::min(top, y);
				bottom = std::max(bottom, y);
			}
		}
	}
	if (right < 0)
		return QRect();
	return QRect(left, top, right - left + 1, bottom - top + 1);
}

// 「I」の右の縦の画、上の横の画、右上の角 (斜め)、アバターの左の 4 か所で測った太さ。
struct LabelWidths {
	Widths vertical;
	Widths horizontal;
	Widths corner;
	Widths avatar;
};

LabelWidths measure_label(Scene &scene)
{
	const QImage image = scene.render();
	LabelWidths out;
	const QRect fill = text_fill(image);
	REQUIRE_FALSE(fill.isNull());
	const int reach = 60 + static_cast<int>(scene.name_px);
	out.vertical = measure(sample_ray(image, fill.right(), fill.center().y(), 1, 0, reach));
	out.horizontal = measure(sample_ray(image, fill.center().x(), fill.top(), 0, -1, reach));
	out.corner = measure(sample_ray(image, fill.right(), fill.top(), 1, -1, reach));
	const CompositeLabel &label = scene.labels.front();
	REQUIRE(label.avatar_d > 0);
	const int cx = scene.cells.front().x + label.avatar_x + label.avatar_d / 2;
	const int cy = scene.cells.front().y + label.avatar_y + label.avatar_d / 2;
	// アバターの中 (暗い青) から左へ。
	const Ray avatar = sample_ray(image, cx, cy, -1, 0, label.avatar_d / 2 + reach);
	out.avatar = measure(avatar);
	return out;
}

void report(const char *what, const LabelWidths &w)
{
	MESSAGE(std::string(what) << ": glow vertical=" << w.vertical.glow << " horizontal=" << w.horizontal.glow
		     << " corner=" << w.corner.glow << " avatar=" << w.avatar.glow << "; outline text="
		     << w.vertical.outline << "/" << w.horizontal.outline << " avatar=" << w.avatar.outline);
}

} // namespace

TEST_CASE("ぼかし 0 のとき、光の太さは文字の縦の画、横の画、斜めの角、アバターのどれでも広がりと 1 ピクセル以内で同じ")
{
	Scene scene;
	const LabelWidths w = measure_label(scene);
	report("name_px=60 spread=6", w);
	const double spread = 6.0;
	CHECK(std::abs(w.vertical.glow - spread) <= 1.0);
	CHECK(std::abs(w.horizontal.glow - spread) <= 1.0);
	CHECK(std::abs(w.corner.glow - spread) <= 1.0);
	CHECK(std::abs(w.avatar.glow - spread) <= 1.0);
}

TEST_CASE("縁取りの見える太さは、文字とアバターで同じ (縁取りの線の太さの半分)")
{
	Scene scene;
	const LabelWidths w = measure_label(scene);
	const double half = 0.1 * 60.0 / 2.0;
	CHECK(std::abs(w.vertical.outline - half) <= 1.0);
	CHECK(std::abs(w.horizontal.outline - half) <= 1.0);
	CHECK(std::abs(w.avatar.outline - half) <= 1.0);
	CHECK(std::abs(w.vertical.outline - w.avatar.outline) <= 1.0);
	CHECK(std::abs(w.horizontal.outline - w.avatar.outline) <= 1.0);
}

TEST_CASE("名前の大きさを 2 倍にすると、光の太さと縁取りの見える太さは 1 ピクセル以内で 2 倍になる")
{
	Scene small;
	const LabelWidths a = measure_label(small);
	Scene large;
	large.name_px = 120.0;
	large.canvas_w = 1400;
	large.canvas_h = 600;
	large.cells = {PixelRect{0, 0, 1400, 600}};
	const LabelWidths b = measure_label(large);
	report("name_px=120 spread=12", b);
	CHECK(std::abs(b.vertical.glow - 2.0 * a.vertical.glow) <= 1.0);
	CHECK(std::abs(b.horizontal.glow - 2.0 * a.horizontal.glow) <= 1.0);
	CHECK(std::abs(b.corner.glow - 2.0 * a.corner.glow) <= 1.0);
	CHECK(std::abs(b.avatar.glow - 2.0 * a.avatar.glow) <= 1.0);
	CHECK(std::abs(b.vertical.outline - 2.0 * a.vertical.outline) <= 1.0);
	CHECK(std::abs(b.avatar.outline - 2.0 * a.avatar.outline) <= 1.0);
	// 2 倍のときも、どの所でも広がりと 1 ピクセル以内。
	CHECK(std::abs(b.vertical.glow - 12.0) <= 1.0);
	CHECK(std::abs(b.horizontal.glow - 12.0) <= 1.0);
	CHECK(std::abs(b.corner.glow - 12.0) <= 1.0);
	CHECK(std::abs(b.avatar.glow - 12.0) <= 1.0);
}

TEST_CASE("不透明度 1 で話しているとき、名前ラベルが覆うピクセルは、文字と縁取りとアバターの色そのまま")
{
	Scene scene;
	const QImage image = scene.render();
	const CompositeLabel &label = scene.labels.front();
	int opaque = 0;
	for (int y = 0; y < label.label.height(); ++y) {
		for (int x = 0; x < label.label.width(); ++x) {
			const QRgb want = label.label.pixel(x, y);
			if (qAlpha(want) != 255)
				continue;
			++opaque;
			const QRgb got = image.pixel(label.label_x + x, label.label_y + y);
			CAPTURE(x);
			CAPTURE(y);
			CHECK(qRed(got) == qRed(want));
			CHECK(qGreen(got) == qGreen(want));
			CHECK(qBlue(got) == qBlue(want));
		}
	}
	CHECK(opaque > 500);
	// 光は名前ラベルの外にある (光がない場面と比べて、どこかのピクセルが緑になっている)。
	scene.level = 0.0;
	const QImage dark = scene.render();
	int lit = 0;
	for (int y = 0; y < image.height(); ++y)
		for (int x = 0; x < image.width(); ++x)
			if (qGreen(image.pixel(x, y)) > qGreen(dark.pixel(x, y)))
				++lit;
	CHECK(lit > 200);
}

TEST_CASE("不透明度 0.6 のとき、名前ラベルが覆うピクセルは 0.6 x 名前ラベルの色 + 0.4 x 背景で、光の色が混ざらない")
{
	Scene full;
	full.background = QColor(100, 100, 100);
	full.render();
	Scene scene = full;
	scene.look.opacity = 0.6;
	const QImage image = scene.render();
	const CompositeLabel &opaque_label = full.labels.front();
	int checked = 0;
	for (int y = 0; y < opaque_label.label.height(); ++y) {
		for (int x = 0; x < opaque_label.label.width(); ++x) {
			const QRgb color = opaque_label.label.pixel(x, y);
			if (qAlpha(color) != 255)
				continue;
			++checked;
			const QRgb got = image.pixel(opaque_label.label_x + x, opaque_label.label_y + y);
			CAPTURE(x);
			CAPTURE(y);
			CHECK(std::abs(qRed(got) - (0.6 * qRed(color) + 0.4 * 100.0)) <= 1.0);
			CHECK(std::abs(qGreen(got) - (0.6 * qGreen(color) + 0.4 * 100.0)) <= 1.0);
			CHECK(std::abs(qBlue(got) - (0.6 * qBlue(color) + 0.4 * 100.0)) <= 1.0);
		}
	}
	CHECK(checked > 500);
}

TEST_CASE("名前ラベル全体の不透明度は、光と名前ラベルを 1 枚にまとめた全体に 1 度だけ掛かり、縁のなめらかな所でも同じ")
{
	// まとめた全体: 不透明度 1 の画像を透明なキャンバスに重ねた 1 枚。それに 0.6 を 1 度だけ掛けて背景に重ねた値と、
	// 不透明度 0.6 の画像をそのまま背景に重ねた値を、全部のピクセルで比べる。明るさは途中の 0.7。
	for (const bool plate : {false, true}) {
		CAPTURE(plate);
		Scene group;
		group.look.show_plate = plate;
		group.look.glow_blur_ratio = 0.2;
		group.level = 0.7;
		group.transparent = true;
		const QImage together = group.render();
		Scene scene = group;
		scene.transparent = false;
		scene.background = QColor(30, 140, 220);
		scene.look.opacity = 0.6;
		const QImage image = scene.render();
		const double o = 153.0 / 255.0; // 0.6 を 8 ビットにした不透明度
		int worst = 0;
		for (int y = 0; y < image.height(); ++y) {
			for (int x = 0; x < image.width(); ++x) {
				const QRgb g = together.pixel(x, y);
				const double keep = 1.0 - o * qAlpha(g) / 255.0;
				const QRgb got = image.pixel(x, y);
				const int dr = std::abs(qRed(got) - static_cast<int>(std::lround(o * qRed(g) + keep * 30)));
				const int dg = std::abs(qGreen(got) - static_cast<int>(std::lround(o * qGreen(g) + keep * 140)));
				const int db = std::abs(qBlue(got) - static_cast<int>(std::lround(o * qBlue(g) + keep * 220)));
				worst = std::max({worst, dr, dg, db});
			}
		}
		MESSAGE("group opacity plate=" << plate << ": worst difference " << worst);
		CHECK(worst <= 2);
	}
}

TEST_CASE("余白 0 で大きなぼかしのとき、光は隣の枠の側へ続いて出て、セルの境でまっすぐ切れず、キャンバスの外には出ない")
{
	Scene scene;
	scene.canvas_w = 640;
	scene.canvas_h = 360;
	scene.cells = {PixelRect{0, 0, 320, 180}, PixelRect{320, 0, 320, 180}, PixelRect{0, 180, 320, 180},
		       PixelRect{320, 180, 320, 180}};
	scene.people = {person("Taro Yamada", true, DecorCorner::BottomLeft)};
	scene.name_px = 20.0;
	scene.margin_px = 0.0;
	scene.look.glow_blur_ratio = 1.0;
	scene.look.glow_spread_ratio = 0.2;
	const QImage image = scene.render();

	// 左上のタイルの名前ラベルは、セルの下の端まで来る。光はその下の、左下のタイルの側に出る。
	const CompositeLabel &top_left = scene.labels[0];
	REQUIRE_FALSE(top_left.glow.isNull());
	CHECK(top_left.label_y + top_left.label.height() <= 180);
	CHECK(top_left.glow_y + top_left.glow.height() > 180);
	CHECK(top_left.glow_x < 0); // 光の画像はキャンバスの左の外まである
	const int x = top_left.avatar_x + top_left.avatar_d / 2;
	const int above = qGreen(image.pixel(x, 179));
	const int below = qGreen(image.pixel(x, 180));
	const int further = qGreen(image.pixel(x, 183));
	MESSAGE("glow across the cell edge: y=179 " << above << ", y=180 " << below << ", y=183 " << further);
	CHECK(below > 40);
	// 境で値が跳ねない (1 ピクセルで半分より大きく落ちない) で、外へ薄れていく。
	CHECK(below * 2 >= above);
	CHECK(further < below);
	CHECK(further > 0);

	// 置く物は、どれもキャンバスの中だけを描く。光はセルの境を越える。
	bool crosses_cell = false;
	for (const CompositeDraw &draw : scene.draws) {
		CHECK(draw.clip.x >= 0);
		CHECK(draw.clip.y >= 0);
		CHECK(draw.clip.x + draw.clip.w <= 640);
		CHECK(draw.clip.y + draw.clip.h <= 360);
		if (draw.kind == CompositeDrawKind::LabelGlow && draw.tile == 0 && draw.clip.y + draw.clip.h > 180)
			crosses_cell = true;
		if (draw.kind == CompositeDrawKind::Label) {
			const PixelRect &cell = scene.cells[draw.tile];
			CHECK(draw.clip.x >= cell.x);
			CHECK(draw.clip.y >= cell.y);
			CHECK(draw.clip.x + draw.clip.w <= cell.x + cell.w);
			CHECK(draw.clip.y + draw.clip.h <= cell.y + cell.h);
		}
	}
	CHECK(crosses_cell);
}

TEST_CASE("背景を出したとき、光は背景の外側にあり、背景の内側には光の色が出ない")
{
	Scene scene;
	scene.background = QColor(40, 40, 40);
	scene.look.show_plate = true;
	scene.look.plate_argb = 0x80000000u;
	scene.people = {person("Taro", true)};
	scene.look.glow_blur_ratio = 0.3;
	const QImage lit = scene.render();
	scene.level = 0.0;
	const QImage dark = scene.render();
	const CompositeLabel &label = scene.labels.front();
	const QRect box(label.box_x, label.box_y, label.box_w, label.box_h);
	// 背景の内側。角の丸みの分を除いた十字の形 (縦長と横長の 2 つの矩形) で見る。
	const int round = static_cast<int>(std::ceil(scene.look.plate_radius_ratio * label.box_h)) + 1;
	const QRect tall = box.adjusted(round, 2, -round, -2);
	const QRect wide = box.adjusted(2, round, -2, -round);
	int inside = 0;
	int outside_lit = 0;
	for (int y = 0; y < lit.height(); ++y) {
		for (int x = 0; x < lit.width(); ++x) {
			const bool changed = lit.pixel(x, y) != dark.pixel(x, y);
			if (tall.contains(x, y) || wide.contains(x, y)) {
				++inside;
				CAPTURE(x);
				CAPTURE(y);
				CHECK_FALSE(changed);
			} else if (!box.adjusted(-1, -1, 1, 1).contains(x, y) && changed) {
				++outside_lit;
			}
		}
	}
	CHECK(inside > 1000);
	CHECK(outside_lit > 500);
	// 背景のすぐ外 (左の辺の真ん中から 2 ピクセル左) は光っている。
	CHECK(qGreen(lit.pixel(box.left() - 2, box.center().y())) > qGreen(dark.pixel(box.left() - 2, box.center().y())));
}

TEST_CASE("ぼかしが正のとき、文字の光とアバターの光は、どちらも見える縁から外へ単調に薄れる")
{
	for (const double spread : {0.1, 0.0}) {
		CAPTURE(spread);
		Scene scene;
		scene.margin_px = 100.0; // 光がキャンバスの端まで届かない余白
		scene.look.glow_blur_ratio = 0.4;
		scene.look.glow_spread_ratio = spread;
		const QImage image = scene.render();
		const QRect fill = text_fill(image);
		REQUIRE_FALSE(fill.isNull());
		const CompositeLabel &label = scene.labels.front();
		const int cx = label.avatar_x + label.avatar_d / 2;
		const int cy = label.avatar_y + label.avatar_d / 2;
		struct Line {
			int x;
			int y;
			int dx;
			int dy;
			int count;
		};
		const Line lines[] = {{fill.right(), fill.center().y(), 1, 0, 80},
				      {fill.center().x(), fill.top(), 0, -1, 80},
				      {cx, cy, -1, 0, label.avatar_d / 2 + 80}};
		for (const Line &line : lines) {
			const std::vector<int> red = red_pixels(image, line.x, line.y, line.dx, line.dy, line.count);
			const std::vector<int> green = green_pixels(image, line.x, line.y, line.dx, line.dy, line.count);
			const double inner = rising_cross(red, 0.0);
			const double edge = falling_cross(red, inner);
			REQUIRE(edge > 0.0);
			// 縁取りの外のなめらかな所を過ぎた最初のピクセルから。
			std::size_t i = static_cast<std::size_t>(std::ceil(edge)) + 1;
			REQUIRE(i < green.size());
			CHECK(green[i] > 0);
			int lit = 0;
			for (; i + 1 < green.size(); ++i) {
				CHECK(green[i + 1] <= green[i]);
				if (green[i] > 0)
					++lit;
			}
			CHECK(lit >= 6); // ぼかしの分だけ外へ続く
			CHECK(green.back() == 0);
		}
	}
}

TEST_CASE("広がりもぼかしも 0 なら、光は見えない")
{
	Scene scene;
	scene.background = QColor(80, 80, 80);
	scene.look.glow_spread_ratio = 0.0;
	scene.look.glow_blur_ratio = 0.0;
	const QImage lit = scene.render();
	CHECK(scene.labels.front().glow.isNull());
	scene.level = 0.0;
	CHECK(lit == scene.render());
}

TEST_CASE("明るさ 0.5 のとき、光のピクセルのアルファは 8 ビットの丸めの差だけでちょうど半分で、形は変わらない")
{
	Scene scene;
	scene.look.glow_blur_ratio = 0.3;
	scene.transparent = true;
	scene.look.show_name = true;
	const QImage full = scene.render();
	scene.level = 0.5;
	const QImage half = scene.render();
	const CompositeLabel &label = scene.labels.front();
	const QRect label_rect(label.label_x, label.label_y, label.label.width(), label.label.height());
	int glow_pixels = 0;
	for (int y = 0; y < full.height(); ++y) {
		for (int x = 0; x < full.width(); ++x) {
			if (label_rect.contains(x, y) && qAlpha(label.label.pixel(x - label.label_x, y - label.label_y)) != 0)
				continue; // 名前ラベルが重なる所は除く
			const int a = qAlpha(full.pixel(x, y));
			const int b = qAlpha(half.pixel(x, y));
			CAPTURE(x);
			CAPTURE(y);
			// 8 ビットの丸めの分 (半分が端数になるときの 0.5) だけを許す。
			CHECK(std::abs(2 * b - a) <= 1);
			CHECK(((a == 0) == (b == 0) || a == 1));
			if (a > 0)
				++glow_pixels;
		}
	}
	CHECK(glow_pixels > 1000);
}
