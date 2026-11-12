// SPDX-License-Identifier: Apache-2.0

// fidelity.h の実装。

#include "fidelity.h"

#include <QPainter>

#include <algorithm>
#include <cstdlib>

namespace cordmux::ui_preview {

QImage crop_client(const QImage &shot)
{
	const QRect client(kShotBorder, kShotTitle, shot.width() - 2 * kShotBorder,
			   shot.height() - kShotTitle - kShotBorder);
	return shot.copy(client).convertToFormat(QImage::Format_RGB32);
}

Comparison compare_images(const QImage &a_in, const QImage &b_in, const QRect &mask)
{
	const QImage a = a_in.convertToFormat(QImage::Format_RGB32);
	const QImage b = b_in.convertToFormat(QImage::Format_RGB32);
	Comparison out;
	out.width = std::min(a.width(), b.width());
	out.height = std::min(a.height(), b.height());
	const QRect area = mask.isNull() ? QRect(0, 0, out.width, out.height)
					 : mask.intersected(QRect(0, 0, out.width, out.height));
	double sum = 0.0;
	long long over = 0;
	long long count = 0;
	for (int y = area.top(); y <= area.bottom(); ++y) {
		const QRgb *ra = reinterpret_cast<const QRgb *>(a.constScanLine(y));
		const QRgb *rb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
		for (int x = area.left(); x <= area.right(); ++x) {
			const int dr = std::abs(qRed(ra[x]) - qRed(rb[x]));
			const int dg = std::abs(qGreen(ra[x]) - qGreen(rb[x]));
			const int db = std::abs(qBlue(ra[x]) - qBlue(rb[x]));
			sum += dr + dg + db;
			if (dr > 16 || dg > 16 || db > 16)
				++over;
			++count;
		}
	}
	if (count > 0) {
		out.mean_abs = sum / (3.0 * static_cast<double>(count));
		out.over16_ratio = static_cast<double>(over) / static_cast<double>(count);
	}
	return out;
}

QImage side_by_side(const QImage &real_in, const QImage &render_in)
{
	const QImage real = real_in.convertToFormat(QImage::Format_RGB32);
	const QImage render = render_in.convertToFormat(QImage::Format_RGB32);
	const int w = std::max(real.width(), render.width());
	const int h = std::max(real.height(), render.height());
	constexpr int kGap = 8;
	QImage out(3 * w + 2 * kGap, h, QImage::Format_RGB32);
	out.fill(QColor(255, 255, 255));
	QPainter painter(&out);
	painter.drawImage(0, 0, real);
	painter.drawImage(w + kGap, 0, render);
	painter.end();
	const int ow = std::min(real.width(), render.width());
	const int oh = std::min(real.height(), render.height());
	for (int y = 0; y < oh; ++y) {
		const QRgb *ra = reinterpret_cast<const QRgb *>(real.constScanLine(y));
		const QRgb *rb = reinterpret_cast<const QRgb *>(render.constScanLine(y));
		QRgb *ro = reinterpret_cast<QRgb *>(out.scanLine(y));
		for (int x = 0; x < ow; ++x) {
			const int d = std::max({std::abs(qRed(ra[x]) - qRed(rb[x])), std::abs(qGreen(ra[x]) - qGreen(rb[x])),
						std::abs(qBlue(ra[x]) - qBlue(rb[x]))});
			const int v = std::min(255, d * 3);
			ro[2 * (w + kGap) + x] = qRgb(v, v, v);
		}
	}
	return out;
}

} // namespace cordmux::ui_preview
