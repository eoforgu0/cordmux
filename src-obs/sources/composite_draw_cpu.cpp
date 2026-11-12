// SPDX-License-Identifier: Apache-2.0

// composite_draw_cpu.h の実装。

#include "sources/composite_draw_cpu.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cordmux::sources {

namespace {

// 0 から 1 の値を 8 ビットにする (四捨五入)。
int to_byte(double value)
{
	return std::clamp(static_cast<int>(std::lround(value * 255.0)), 0, 255);
}

// 乗算済みアルファの元の色 (0 から 1 の 4 つの値) を、ピクセル p に「元 + 下 x (1 - 元のアルファ)」で重ねる。
void blend_into(QRgb &p, double r, double g, double b, double a)
{
	const double keep = 1.0 - a;
	p = qRgba(to_byte(r + qRed(p) / 255.0 * keep), to_byte(g + qGreen(p) / 255.0 * keep),
		  to_byte(b + qBlue(p) / 255.0 * keep), to_byte(a + qAlpha(p) / 255.0 * keep));
}

// 画像を置く。image の位置に等倍で、clip の中だけ、4 つのチャンネルに level を掛け、さらに 3 つの色のチャンネルだけに dim を
// 掛けて重ねる (アルファは dim で変えない)。dim が 1 のときは、dim を掛けない式と同じ値になる。
// 掛けるのは GPU の効果 (composite_decor.cpp の kFadeEffect) と同じく 32 ビットの小数で行う。64 ビットで掛けると、白 (255) に
// 暗さ 30% を掛けた値がちょうど 178.5 になり、四捨五入で 179 になる。GPU の 32 ビットの 0.7 は 0.7 より少し小さいので、
// GPU では 178 になる。
void draw_image(QImage *canvas, const QImage &source, const CompositeDraw &draw)
{
	if (source.isNull())
		return;
	const QImage image = source.format() == QImage::Format_ARGB32_Premultiplied
				     ? source
				     : source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	const int x_begin = std::max({draw.clip.x, 0, draw.image.x});
	const int y_begin = std::max({draw.clip.y, 0, draw.image.y});
	const int x_end = std::min({draw.clip.x + draw.clip.w, canvas->width(), draw.image.x + image.width()});
	const int y_end = std::min({draw.clip.y + draw.clip.h, canvas->height(), draw.image.y + image.height()});
	const float level = static_cast<float>(draw.level);
	const float color_level = level * static_cast<float>(draw.dim);
	const auto unit = [](int channel) { return static_cast<float>(channel) / 255.0f; };
	for (int y = y_begin; y < y_end; ++y) {
		auto *dst = reinterpret_cast<QRgb *>(canvas->scanLine(y));
		const auto *src = reinterpret_cast<const QRgb *>(image.constScanLine(y - draw.image.y));
		for (int x = x_begin; x < x_end; ++x) {
			const QRgb s = src[x - draw.image.x];
			if (qAlpha(s) == 0 && qRed(s) == 0 && qGreen(s) == 0 && qBlue(s) == 0)
				continue;
			blend_into(dst[x], unit(qRed(s)) * color_level, unit(qGreen(s)) * color_level,
				   unit(qBlue(s)) * color_level, unit(qAlpha(s)) * level);
		}
	}
}

} // namespace

void draw_composite_plan(QImage *canvas, const std::vector<CompositeDraw> &draws,
			 const std::vector<CompositeTileImages> &images,
			 const std::vector<CompositeTileImages> &roster_images)
{
	if (!canvas || canvas->isNull() || canvas->format() != QImage::Format_ARGB32_Premultiplied)
		return;
	for (const CompositeDraw &draw : draws) {
		const bool roster = draw.kind == CompositeDrawKind::RosterGlow || draw.kind == CompositeDrawKind::RosterLabel;
		const std::vector<CompositeTileImages> &source = roster ? roster_images : images;
		if (draw.tile >= source.size())
			continue;
		const CompositeTileImages &tile = source[draw.tile];
		switch (draw.kind) {
		case CompositeDrawKind::FrameGlow:
			draw_image(canvas, tile.frame, draw);
			break;
		case CompositeDrawKind::LabelGlow:
		case CompositeDrawKind::RosterGlow:
			draw_image(canvas, tile.glow, draw);
			break;
		case CompositeDrawKind::Label:
		case CompositeDrawKind::RosterLabel:
			draw_image(canvas, tile.label, draw);
			break;
		}
	}
}

} // namespace cordmux::sources
