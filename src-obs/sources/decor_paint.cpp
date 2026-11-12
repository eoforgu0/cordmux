// SPDX-License-Identifier: Apache-2.0

// decor_paint.h の実装と、名前ラベルの描画 (composite_label.cpp) と共有する関数。

#include "sources/decor_paint.h"

#include <algorithm>
#include <cmath>

#include <QChar>
#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <QPointF>
#include <QRawFont>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QTextBoundaryFinder>

#include "sources/decor_paint_common.h"
#include "sources/dummy_tiles.h"

namespace cordmux::sources {

using namespace paint_common;

namespace paint_common {

// 描画する面の解像度。端数の em を文字の大きさに渡すため、文字はポイントで指定し、面を 96 dpi に固定する
// (1 pt = 96 / 72 ピクセル)。文字を測るときも同じ 96 dpi の面を通す (面を通さずに測ると、Qt はその PC の画面の
// dpi でポイントをピクセルに修正するので、OBS の中では画面の拡大率によって文字の大きさが変わる)。
constexpr int kDotsPerMeter96 = 3780;

// 文字の縦の位置を合わせる基準の文字 (漢字の文字列面の中心を、背景の中心に合わせる)。
constexpr char16_t kReferenceGlyph = 0x56FD; // 国

QColor color_from_argb(std::uint32_t value)
{
	return QColor::fromRgba(static_cast<QRgb>(value));
}

QImage make_surface(int width, int height)
{
	QImage image(std::max(1, width), std::max(1, height), QImage::Format_ARGB32_Premultiplied);
	image.setDotsPerMeterX(kDotsPerMeter96);
	image.setDotsPerMeterY(kDotsPerMeter96);
	image.fill(Qt::transparent);
	return image;
}

const QImage &measure_surface()
{
	static const QImage surface = make_surface(1, 1);
	return surface;
}

// フォントの設定が空のときのフォント (kDefaultLabelFontFamilies の候補)。フォントを指定するのは、指定しないと
// OBS のプロセスの Qt の既定のフォント (Windows の表示言語で決まる。英語の Windows では Segoe UI で、漢字と
// かなはほかのフォントで表示される) になり、同じ名前ラベルが利用者の OS の言語によって違う形になるためである。
// 候補の一覧 (setFamilies) で指定するので、Qt が先にあるものから順に探してその PC にある最初のものを使い、どれも
// ない PC では代わりのフォントを使う (OS の API を呼ばないので、どの OS でもビルドできて動く)。
void apply_default_family(QFont &font)
{
	QStringList families;
	for (const char *name : kDefaultLabelFontFamilies)
		families.append(QString::fromUtf8(name));
	font.setFamilies(families);
}

QFont make_label_font(const std::string &family, int weight, bool italic, double em_px)
{
	QFont font;
	if (!family.empty())
		font.setFamily(QString::fromStdString(family));
	else
		apply_default_family(font);
	font.setPointSizeF(std::max(0.5, em_px * 72.0 / 96.0));
	font.setWeight(static_cast<QFont::Weight>(std::clamp(weight, 1, 1000)));
	font.setItalic(italic);
	// ヒンティングを無効にする。有効だと、同じフォントでも描画する仕組みのバージョンによって形がずれ、同じ入力から
	// 同じピクセルを返せなくなる。
	font.setHintingPreference(QFont::PreferNoHinting);
	return font;
}

QPainterPath avatar_shape_path(const QRectF &rect, config::AvatarShape shape)
{
	QPainterPath path;
	switch (shape) {
	case config::AvatarShape::Circle:
		path.addEllipse(rect);
		break;
	case config::AvatarShape::Rounded: {
		const qreal radius = std::min(rect.width(), rect.height()) / 4.0;
		path.addRoundedRect(rect, radius, radius);
		break;
	}
	case config::AvatarShape::Square:
		path.addRect(rect);
		break;
	}
	return path;
}

// アバターを一辺 diameter の形で切り抜く。縦横比は保ったまま短い辺を合わせ、中央を切り取る
// (引き伸ばすと人の顔がゆがむため)。
QImage clip_avatar_to_shape(const QImage &source, int diameter, config::AvatarShape shape)
{
	if (source.isNull() || diameter <= 0)
		return QImage();

	QImage expanded =
		source.scaled(diameter, diameter, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
	int left = std::max(0, (expanded.width() - diameter) / 2);
	int top = std::max(0, (expanded.height() - diameter) / 2);
	QImage square = expanded.copy(left, top, diameter, diameter)
				.convertToFormat(QImage::Format_ARGB32_Premultiplied);

	QImage circle(diameter, diameter, QImage::Format_ARGB32_Premultiplied);
	circle.fill(Qt::transparent);
	{
		QPainter painter(&circle);
		painter.setRenderHint(QPainter::Antialiasing, true);
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(255, 255, 255));
		painter.drawPath(avatar_shape_path(
			QRectF(0.0, 0.0, static_cast<qreal>(diameter), static_cast<qreal>(diameter)), shape));
		// 形の中だけを残す合成。輪郭がなめらかになる (クリップ領域を指定すると、輪郭がギザギザになる)。
		painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
		painter.drawImage(0, 0, square);
	}
	return circle;
}

// 見えている文字 (省略記号を除いた部分)。Qt の省略記号は U+2026 の 1 文字か、フォントがそれを持たないときの
// ピリオド 3 つ。
QString visible_part(const QString &drawn)
{
	QString text = drawn;
	text.remove(QChar(0x2026));
	text.remove(QStringLiteral("..."));
	return text;
}

int grapheme_count(const QString &text)
{
	if (text.isEmpty())
		return 0;
	QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
	int count = 0;
	while (finder.toNextBoundary() != -1)
		++count;
	return count;
}

// 見えている文字が同じになる名前どうしも、片方の先頭がもう片方の先頭に一致するので、この 1 つの判定に含まれる。
bool prefix_collides(const QString &visible_head, const std::vector<std::string> &others)
{
	if (visible_head.isEmpty())
		return false;
	for (const std::string &other : others) {
		if (QString::fromStdString(other).startsWith(visible_head))
			return true;
	}
	return false;
}

bool same_name_visible(const std::string &name, const std::vector<std::string> &others)
{
	return !name.empty() && std::find(others.begin(), others.end(), name) != others.end();
}

// 「国」をほかのフォントで代わりに描画すると、代わりのフォントは PC ごとに違い、位置が PC ごとに変わるので、
// フォントが「国」を持たないときだけ、大文字の高さの中心を合わせる。
double baseline_for(const QFont &font, const QFontMetricsF &metrics, double center_y)
{
	const QRawFont raw = QRawFont::fromFont(font);
	if (raw.isValid() && raw.supportsCharacter(QChar(kReferenceGlyph))) {
		const QRectF face = metrics.tightBoundingRect(QString(QChar(kReferenceGlyph)));
		return center_y - (face.top() + face.bottom()) / 2.0;
	}
	return center_y + metrics.capHeight() / 2.0;
}

} // namespace paint_common

QImage paint_dummy_background(int slot, int width, int height)
{
	if (width <= 0 || height <= 0)
		return QImage();
	QImage tile = make_surface(width, height);
	tile.fill(color_from_argb(dummy_tile_color(slot)));

	QPainter painter(&tile);
	painter.setRenderHint(QPainter::Antialiasing, true);
	painter.setRenderHint(QPainter::TextAntialiasing, true);

	// 枠の番号。背景に溶け込みすぎず、名前ラベルより目立ちすぎないように、少し透けた白で描画する。番号のフォントも
	// 名前ラベルの既定と同じにする (OS の表示言語によって番号の形が変わらないように)。
	QFont font;
	apply_default_family(font);
	font.setPixelSize(std::max(1, static_cast<int>(height * 0.42)));
	font.setBold(true);
	font.setHintingPreference(QFont::PreferNoHinting);
	painter.setFont(font);
	painter.setPen(QColor(255, 255, 255, 200));
	painter.drawText(QRect(0, 0, width, height), Qt::AlignCenter, QString::number(slot));
	return tile;
}

} // namespace cordmux::sources
