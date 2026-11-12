// SPDX-License-Identifier: Apache-2.0

// composite_label.h の実装。

#include "sources/composite_label.h"

#include <algorithm>
#include <cmath>

#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointF>
#include <QRawFont>
#include <QRect>
#include <QRectF>
#include <QSizeF>
#include <QTextBoundaryFinder>

#include "sources/decor_paint_common.h"
#include "sources/glow_filter.h"

namespace cordmux::sources {

using namespace paint_common;

namespace {

// 頭文字の文字の色。背景 (initial_bg_argb) に対して読める白に決めてあり、文字の色の設定には従わせない
// (文字の色を黒にした利用者の頭文字が、暗い背景の上で読めなくなるため)。
constexpr std::uint32_t kInitialTextArgb = 0xFFFFFFFFu;

// 乗算済みアルファの画像の 4 つのチャンネルすべてに、不透明度を掛ける (opacity は 0 から 255)。名前ラベルの
// 背景と文字と縁取りを 1 枚の画像にしてから掛けるので、重なった部分が濃くならない。
void scale_premultiplied(QImage *image, int opacity)
{
	if (opacity >= 255)
		return;
	for (int y = 0; y < image->height(); ++y) {
		auto *row = reinterpret_cast<QRgb *>(image->scanLine(y));
		for (int x = 0; x < image->width(); ++x) {
			const QRgb p = row[x];
			const int a = (qAlpha(p) * opacity + 127) / 255;
			const int r = (qRed(p) * opacity + 127) / 255;
			const int g = (qGreen(p) * opacity + 127) / 255;
			const int b = (qBlue(p) * opacity + 127) / 255;
			row[x] = qRgba(r, g, b, a);
		}
	}
}

int opacity_of(double opacity)
{
	return std::clamp(static_cast<int>(std::lround(opacity * 255.0)), 0, 255);
}

// 画像のアルファを 8 ビットの面に取り出す。
AlphaPlane alpha_plane_of(const QImage &image)
{
	AlphaPlane plane = make_alpha_plane(image.width(), image.height());
	for (int y = 0; y < image.height(); ++y) {
		const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
		for (int x = 0; x < image.width(); ++x)
			plane.at(x, y) = static_cast<std::uint8_t>(qAlpha(row[x]));
	}
	return plane;
}

// アルファの面を色で塗った乗算済みアルファの画像にする。色のアルファと opacity (0 から 255) も掛ける。
QImage colorize(const AlphaPlane &plane, std::uint32_t argb, int opacity)
{
	QImage image = make_surface(plane.width, plane.height);
	const int color_alpha = static_cast<int>(argb >> 24);
	const int r = static_cast<int>((argb >> 16) & 0xFFu);
	const int g = static_cast<int>((argb >> 8) & 0xFFu);
	const int b = static_cast<int>(argb & 0xFFu);
	// 色のアルファと不透明度を 1 つにまとめる (0 から 255)。
	const int scale = (color_alpha * opacity + 127) / 255;
	for (int y = 0; y < plane.height; ++y) {
		auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
		for (int x = 0; x < plane.width; ++x) {
			const int a = (plane.at(x, y) * scale + 127) / 255;
			row[x] = qRgba((r * a + 127) / 255, (g * a + 127) / 255, (b * a + 127) / 255, a);
		}
	}
	return image;
}

// 名前の先頭の 1 文字 (書記素)。ラテン文字は大文字にする。名前が空白だけなら空。
QString initial_of(const QString &name)
{
	const QString trimmed = name.trimmed();
	if (trimmed.isEmpty())
		return QString();
	QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, trimmed);
	const int end = finder.toNextBoundary();
	if (end <= 0)
		return QString();
	return trimmed.left(end).toUpper();
}

// アバターの画像がないときの、頭文字の形 (一辺 diameter。形はアバターと同じ)。
QImage paint_initial_avatar(const QString &name, const CompositeLabelLook &look, int diameter)
{
	QImage image = make_surface(diameter, diameter);
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing, true);
	painter.setRenderHint(QPainter::TextAntialiasing, true);
	painter.setPen(Qt::NoPen);
	painter.setBrush(color_from_argb(look.initial_bg_argb));
	painter.drawPath(avatar_shape_path(QRectF(0.0, 0.0, diameter, diameter), look.avatar_shape));

	const QString initial = initial_of(name);
	if (initial.isEmpty())
		return image;
	const QFont font = make_label_font(look.font_family, 700, false, look.initial_em_ratio * diameter);
	const QFontMetricsF metrics(font, &measure_surface());
	painter.setFont(font);
	painter.setPen(color_from_argb(kInitialTextArgb));
	painter.drawText(QPointF((diameter - metrics.horizontalAdvance(initial)) / 2.0,
				 baseline_for(font, metrics, diameter / 2.0)),
			 initial);
	return image;
}

// 縁取りの線 (文字の輪郭を中心に引く) と文字を描画する。文字は drawText で描画する (色付きの絵文字を、色のまま
// 表示するため)。線は文字の輪郭の経路で引くので、色付きの絵文字には線がつかない。
void paint_outlined_text(QPainter &painter, const QFont &font, const QString &text, const QPointF &baseline,
			 double outline_px, const QColor &outline, const QColor &fill)
{
	if (outline_px > 0.0) {
		QPainterPath path;
		path.addText(baseline, font, text);
		painter.strokePath(path, QPen(outline, outline_px, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	}
	painter.setFont(font);
	painter.setPen(fill);
	painter.drawText(baseline, text);
}

// 名前ラベルの画像の中での、描くものの位置と大きさ。名前ラベルの画像と、グローの形を作る面 (画像の外側に余地を
// 足した面) の両方を、同じ値から描く。
struct LabelGeometry {
	bool plate = false;
	QRectF plate_rect;
	double plate_radius = 0.0;
	bool avatar = false;
	QPoint avatar_at; // アバターを囲む正方形の左上 (整数にして、画像と形の面でアンチエイリアスをそろえる)
	int avatar_d = 0;
	config::AvatarShape avatar_shape = config::AvatarShape::Circle;
	bool name = false;
	QString text;
	QPointF baseline;
	// 見える縁取りの線の太さ (縁取りの色が透明なら 0)。
	double outline_px = 0.0;
};

// 名前ラベルの見える形を、不透明な黒で描く。offset は画像の左上から面の左上までのずれ。with_plate、with_avatar、
// with_name で描くものを選ぶ (geometry にないものは描かない)。アバターは画像ではなくアバターの形で描くので、透明な所の
// あるアバターでも、形の中は全部覆う。
void paint_visible_shape(QPainter &painter, const LabelGeometry &geometry, const QFont &font, int offset,
			 bool with_plate, bool with_avatar, bool with_name)
{
	const QColor black(0, 0, 0);
	if (with_plate && geometry.plate) {
		painter.setPen(Qt::NoPen);
		painter.setBrush(black);
		painter.drawRoundedRect(geometry.plate_rect.translated(offset, offset), geometry.plate_radius,
					geometry.plate_radius);
	}
	if (with_avatar && geometry.avatar) {
		const QRectF avatar_rect(geometry.avatar_at.x() + offset, geometry.avatar_at.y() + offset, geometry.avatar_d,
				    geometry.avatar_d);
		if (geometry.outline_px > 0.0)
			painter.setPen(QPen(black, geometry.outline_px));
		else
			painter.setPen(Qt::NoPen);
		painter.setBrush(black);
		painter.drawPath(avatar_shape_path(avatar_rect, geometry.avatar_shape));
	}
	if (with_name && geometry.name) {
		paint_outlined_text(painter, font, geometry.text, geometry.baseline + QPointF(offset, offset),
				    geometry.outline_px, black, black);
	}
}

// 見える形を描いた面のアルファ。
AlphaPlane shape_plane(int width, int height, const LabelGeometry &geometry, const QFont &font, int offset,
		       bool with_plate, bool with_avatar, bool with_name)
{
	QImage mask = make_surface(width, height);
	{
		QPainter painter(&mask);
		painter.setRenderHint(QPainter::Antialiasing, true);
		painter.setRenderHint(QPainter::TextAntialiasing, true);
		paint_visible_shape(painter, geometry, font, offset, with_plate, with_avatar, with_name);
	}
	return alpha_plane_of(mask);
}

// グローの画像を作る。glow は膨らませてぼかしたグローのアルファ、cover は名前ラベルの見える形 (glow と同じ面)、
// label は不透明度を掛ける前の名前ラベルの画像で、面の (offset, offset) に置かれる。倍率の式と理由は
// composite_label.h の paint_composite_label のコメント。
QImage glow_image(const AlphaPlane &glow, const AlphaPlane &cover, const QImage &label, int offset,
		  std::uint32_t argb, int opacity)
{
	QImage image = make_surface(glow.width, glow.height);
	const double o = opacity / 255.0;
	const double color_alpha = static_cast<double>(argb >> 24) / 255.0;
	const int r = static_cast<int>((argb >> 16) & 0xFFu);
	const int g = static_cast<int>((argb >> 8) & 0xFFu);
	const int b = static_cast<int>(argb & 0xFFu);
	for (int y = 0; y < glow.height; ++y) {
		auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
		const int ly = y - offset;
		const QRgb *label_row = (ly >= 0 && ly < label.height())
						? reinterpret_cast<const QRgb *>(label.constScanLine(ly))
						: nullptr;
		for (int x = 0; x < glow.width; ++x) {
			const int value = glow.at(x, y);
			if (value == 0)
				continue;
			const int lx = x - offset;
			const double a = (label_row && lx >= 0 && lx < label.width()) ? qAlpha(label_row[lx]) / 255.0 : 0.0;
			const double c = cover.at(x, y) / 255.0;
			const double rest = 1.0 - o * a;
			const double share = rest > 0.0 ? o * (1.0 - c) / rest : 0.0;
			const int alpha = std::clamp(static_cast<int>(std::lround(value * share * color_alpha)), 0, 255);
			row[x] = qRgba((r * alpha + 127) / 255, (g * alpha + 127) / 255, (b * alpha + 127) / 255, alpha);
		}
	}
	return image;
}

// 画像の透明でないピクセルを囲む矩形 (画像の座標)。何もなければ空の矩形。
QRect visible_bounds(const QImage &image)
{
	int left = image.width();
	int top = image.height();
	int right = -1;
	int bottom = -1;
	for (int y = 0; y < image.height(); ++y) {
		const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
		for (int x = 0; x < image.width(); ++x) {
			if (qAlpha(row[x]) == 0)
				continue;
			left = std::min(left, x);
			right = std::max(right, x);
			top = std::min(top, y);
			bottom = std::max(bottom, y);
		}
	}
	if (right < 0)
		return QRect();
	return QRect(left, top, right - left + 1, bottom - top + 1);
}

// 画像の位置 (セルの左上から見た画像の左上) を 1 つの軸で決める。want は余白から決めた位置。見える範囲
// [ink_first, ink_last] (画像の座標) がセルの [0, cell) に収まるよう寄せる。セルより大きくて収まらないときは、
// 見える範囲の始まり (左か上) をセルの端に合わせ、はみ出す分は置くときにセルで切り抜く。
int place_on_axis(int want, int ink_first, int ink_last, int cell)
{
	const int low = -ink_first;
	const int high = cell - 1 - ink_last;
	if (low > high)
		return low;
	return std::clamp(want, low, high);
}

} // namespace

CompositeLabel paint_composite_label(const LabelContent &content, const CompositeLabelLook &look, double name_px,
				     double margin_px, int cell_w, int cell_h)
{
	CompositeLabel result;
	if (!(name_px > 0.0) || cell_w <= 0 || cell_h <= 0)
		return result;
	const int opacity = opacity_of(look.opacity);
	if (opacity <= 0)
		return result;

	const QString name = QString::fromStdString(content.name);
	bool draw_name = look.show_name && !name.isEmpty();
	// アバターは、画像があるか、画像の代わりに頭文字を描ける名前があるときに描く。
	const bool avatar_available = look.show_avatar && (!content.avatar.isNull() || !name.trimmed().isEmpty());
	bool draw_avatar = avatar_available;
	if (!draw_name && !draw_avatar)
		return result;

	const double em = name_px;
	const QFont font = make_label_font(look.font_family, look.font_weight, look.font_italic, em);
	const QFontMetricsF metrics(font, &measure_surface());

	// 縁取り。文字とアバターに同じ太さで付ける。縁取りを表示しないときと、縁取りの色が透明なときは、見える縁は字形とアバターの形のまま。
	const double outline_px =
		look.show_outline && (look.outline_argb >> 24) != 0 ? std::max(0.0, look.outline_ratio * em) : 0.0;
	const int ext = outline_px > 0.0 ? static_cast<int>(std::ceil(outline_px / 2.0)) + 1 : 0;
	// 縁取りが字形とアバターの外へ出る幅。余白と間は見える縁から測るので、アバターと名前は、それぞれこの幅を両側に
	// 足した大きさとして並べる。
	const double edge = outline_px / 2.0;

	// 大きさ。アバターと行の高さと余白はピクセルに丸め、名前の幅だけ端数のまま足す。
	const double margin = std::max(0.0, margin_px);
	const int avatar_d = std::max(1, static_cast<int>(std::lround(look.avatar_ratio * em)));
	const double avatar_span = avatar_d + edge * 2.0; // 縁取りを含むアバターの幅
	const int line_h = std::max(1, static_cast<int>(std::lround(look.line_em * em)));
	const double gap = look.avatar_gap_em * em;
	const int pad_v = look.show_plate ? static_cast<int>(std::lround(look.plate_pad_v_em * em)) : 0;
	const double pad_r = look.show_plate ? look.plate_pad_right_em * em : 0.0;
	const double pad_l_avatar = look.show_plate ? static_cast<double>(pad_v) : 0.0;
	const double pad_l_plain = look.show_plate ? look.plate_pad_left_em * em : 0.0;
	const double fixed_avatar = pad_l_avatar + avatar_span + gap + edge * 2.0 + pad_r;
	const double fixed_plain = pad_l_plain + edge * 2.0 + pad_r;
	const double min_name_px = look.min_name_em * em;
	// 幅の上限は「セルの幅 - 余白 x 2」。名前に使える幅は、そこから名前以外の幅を引いた残り。
	const double cap_w = std::max(0.0, cell_w - margin * 2.0);
	const auto name_room = [&](double fixed) { return std::max(0.0, cap_w - fixed); };

	// 同じシーンに同じ名前の人が見えているときは、2 人を見分ける手がかりを残すために、アバターを省かない。
	const bool keep_avatar = same_name_visible(content.name, content.other_visible_names);
	if (draw_name && draw_avatar && look.drop_avatar_when_narrow && !keep_avatar &&
	    name_room(fixed_avatar) + 0.5 < min_name_px) {
		draw_avatar = false;
		result.avatar_dropped = true;
	}

	QString drawn;
	if (draw_name) {
		double room = name_room(draw_avatar ? fixed_avatar : fixed_plain);
		// 名前の幅の上限 (並びの名前ラベル)。アバターを省くかは、上限ではなくセルの幅で決める (上限は利用者が選んだ長さで、
		// 狭いセルとは違う)。
		if (look.max_name_em > 0.0)
			room = std::min(room, look.max_name_em * em);
		result.name_room_px = room;
		drawn = name;
		if (metrics.horizontalAdvance(name) > room) {
			drawn = metrics.elidedText(name, Qt::ElideRight, room);
			result.elided = true;
			// 見えている先頭がほかの人の名前の先頭にも一致するなら、この名前ラベルだけ真ん中を省略する
			// (先頭と末尾を残す)。
			if (prefix_collides(visible_part(drawn), content.other_visible_names)) {
				drawn = metrics.elidedText(name, Qt::ElideMiddle, room);
				result.elided_middle = true;
			}
			// 見える文字が少なすぎる名前は、読めないものとして扱う。アバターを省いていたら戻す。
			if (grapheme_count(visible_part(drawn)) < look.min_visible_graphemes) {
				draw_name = false;
				result.name_hidden = true;
				drawn.clear();
				if (result.avatar_dropped) {
					draw_avatar = avatar_available;
					result.avatar_dropped = false;
				}
			}
		}
		if (drawn.isEmpty())
			draw_name = false;
	}
	if (!draw_name && !draw_avatar)
		return result;
	result.name_drawn = draw_name ? drawn : QString();

	// 名前ラベルの矩形。行の高さは、文字の行と縁取りを含むアバターの高いほう。
	const double text_w = draw_name ? metrics.horizontalAdvance(drawn) : 0.0;
	const int avatar_h = static_cast<int>(std::ceil(avatar_span));
	const int row_h = draw_avatar ? std::max(avatar_h, draw_name ? line_h : 0) : line_h;
	const int box_w = static_cast<int>(
		std::ceil(draw_avatar ? (draw_name ? fixed_avatar + text_w : pad_l_avatar * 2.0 + avatar_span)
				      : fixed_plain + text_w));
	const int box_h = row_h + pad_v * 2;
	result.box_w = box_w;
	result.box_h = box_h;

	// 名前ラベルの画像の中の位置 (縁取りの余地の分だけ内側から始まる)。content_x は、縁取りを含む最も左のもの (アバターが
	// 左ならアバター、右かアバターがなければ文字) の左端。アバターが右のときは、左から名前 (名前側の余白 pad_r)、
	// 間、アバター (背景の内側の余白 pad_v) の順に並べ、左のときの並びを左右入れ替えた形にする。
	LabelGeometry geometry;
	geometry.outline_px = outline_px;
	const bool avatar_right = look.avatar_side == config::AvatarSide::Right;
	const bool text_first = avatar_right && draw_name;
	const double content_x = ext + (text_first ? pad_r : (draw_avatar ? pad_l_avatar : pad_l_plain));
	const int avatar_y = ext + pad_v + (row_h - avatar_d) / 2;
	if (look.show_plate) {
		geometry.plate = true;
		geometry.plate_rect = QRectF(ext, ext, box_w, box_h);
		geometry.plate_radius = std::max(0.0, look.plate_radius_ratio * box_h);
	}
	if (draw_avatar) {
		geometry.avatar = true;
		// アバターの縁取りを含む左端は、左なら content_x、右で名前があるときは名前の右の縁取りと間の後。
		const double avatar_left = text_first ? content_x + text_w + edge * 2.0 + gap : content_x;
		geometry.avatar_at = QPoint(static_cast<int>(std::lround(avatar_left + edge)), avatar_y);
		geometry.avatar_d = avatar_d;
		geometry.avatar_shape = look.avatar_shape;
	}
	if (draw_name) {
		geometry.name = true;
		geometry.text = drawn;
		const double name_left = text_first ? content_x : content_x + (draw_avatar ? avatar_span + gap : 0.0);
		geometry.baseline = QPointF(name_left + edge, baseline_for(font, metrics, ext + pad_v + row_h / 2.0));
	}

	QImage avatar_image;
	if (draw_avatar) {
		if (!content.avatar.isNull()) {
			avatar_image = clip_avatar_to_shape(content.avatar, avatar_d, look.avatar_shape);
		} else {
			avatar_image = paint_initial_avatar(name, look, avatar_d);
			result.initial_drawn = true;
		}
	}

	// 名前ラベルの画像 (不透明度を掛ける前)。重なりの順は、背景、アバターの縁取り、アバター、文字の縁取り、文字。
	QImage image = make_surface(box_w + ext * 2, box_h + ext * 2);
	{
		QPainter painter(&image);
		painter.setRenderHint(QPainter::Antialiasing, true);
		painter.setRenderHint(QPainter::TextAntialiasing, true);
		painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

		if (geometry.plate) {
			painter.setPen(Qt::NoPen);
			painter.setBrush(color_from_argb(look.plate_argb));
			painter.drawRoundedRect(geometry.plate_rect, geometry.plate_radius, geometry.plate_radius);
		}
		if (geometry.avatar) {
			// 縁取りはアバターの輪郭を中心に引き、内側の半分はアバターの下に隠れる (文字と同じ見える太さになる)。
			if (outline_px > 0.0) {
				painter.setPen(QPen(color_from_argb(look.outline_argb), outline_px));
				painter.setBrush(Qt::NoBrush);
				painter.drawPath(avatar_shape_path(QRectF(geometry.avatar_at, QSizeF(avatar_d, avatar_d)),
								   look.avatar_shape));
			}
			painter.drawImage(geometry.avatar_at, avatar_image);
		}
		if (geometry.name) {
			paint_outlined_text(painter, font, drawn, geometry.baseline, outline_px,
					    color_from_argb(look.outline_argb), color_from_argb(look.text_argb));
			const QRawFont raw = QRawFont::fromFont(font);
			result.font_family_used = raw.isValid() ? raw.familyName() : QString();
		}
	}

	// 位置は四隅のどれかで、セルの端から余白。見える範囲がセルからはみ出すときは、セルの中に寄せる。
	const bool at_left = content.corner == DecorCorner::TopLeft || content.corner == DecorCorner::BottomLeft;
	const bool at_top = content.corner == DecorCorner::TopLeft || content.corner == DecorCorner::TopRight;
	const double want_box_x = at_left ? margin : cell_w - margin - box_w;
	const double want_box_y = at_top ? margin : cell_h - margin - box_h;
	const QRect ink = visible_bounds(image);
	const int want_x = static_cast<int>(std::lround(want_box_x)) - ext;
	const int want_y = static_cast<int>(std::lround(want_box_y)) - ext;
	const int label_x = ink.isNull() ? want_x : place_on_axis(want_x, ink.left(), ink.right(), cell_w);
	const int label_y = ink.isNull() ? want_y : place_on_axis(want_y, ink.top(), ink.bottom(), cell_h);
	result.box_x = label_x + ext;
	result.box_y = label_y + ext;
	if (geometry.avatar) {
		result.avatar_x = label_x + geometry.avatar_at.x();
		result.avatar_y = label_y + geometry.avatar_at.y();
		result.avatar_d = avatar_d;
	}

	// グロー。面は名前ラベルの画像の左上から reach だけ外側に広げる (膨らませる分と、ぼかしの標準偏差の 3 倍)。
	const double sigma = std::max(0.0, look.glow_blur_ratio) * em / 2.0;
	const double spread = std::max(0.0, look.glow_spread_ratio) * em;
	const bool glow_avatar_on = look.glow_avatar && draw_avatar;
	const bool glow_name_on = look.glow_name && draw_name;
	if ((glow_avatar_on || glow_name_on) && (spread > 0.0 || sigma > 0.0)) {
		const int reach = static_cast<int>(std::ceil(spread)) + 1 + static_cast<int>(std::ceil(sigma * 3.0));
		const int glow_w = image.width() + reach * 2;
		const int glow_h = image.height() + reach * 2;
		const AlphaPlane cover = shape_plane(glow_w, glow_h, geometry, font, reach, true, true, true);
		// 背景を表示するときは、背景を含む見える形全体をグローの元にする (背景の下に隠れる文字のグローは描かない)。
		const AlphaPlane source = geometry.plate ? cover
							 : shape_plane(glow_w, glow_h, geometry, font, reach, false,
								       glow_avatar_on, glow_name_on);
		const AlphaPlane alpha = gaussian_blur(dilate_disk(source, spread), sigma);
		const QImage glow = glow_image(alpha, cover, image, reach, look.glow_argb, opacity);
		// 透明な周りを除いて、テクスチャと重ねる範囲を小さくする。
		const QRect glow_ink = visible_bounds(glow);
		if (!glow_ink.isNull()) {
			result.glow = glow.copy(glow_ink);
			result.glow_x = label_x - reach + glow_ink.x();
			result.glow_y = label_y - reach + glow_ink.y();
		}
	}

	scale_premultiplied(&image, opacity);
	if (ink.isNull())
		return result;
	result.label = image.copy(ink);
	result.label_x = label_x + ink.x();
	result.label_y = label_y + ink.y();
	return result;
}

QImage paint_frame_glow(int cell_w, int cell_h, std::uint32_t argb, double blur_px, double spread_px)
{
	if (cell_w <= 0 || cell_h <= 0)
		return QImage();
	return colorize(inset_glow_alpha(cell_w, cell_h, blur_px, spread_px), argb, 255);
}

} // namespace cordmux::sources
