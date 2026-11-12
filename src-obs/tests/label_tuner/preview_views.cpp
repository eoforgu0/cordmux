// SPDX-License-Identifier: Apache-2.0

// preview_views.h の実装。

#include "preview_views.h"

#include <QColor>
#include <QMouseEvent>
#include <QPainter>
#include <QTransform>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace cordmux::label_tuner {

namespace {

const QColor kViewBackground(32, 32, 36);

// 描く先の 1 論理ピクセルが、デバイスのピクセルでいくつか (Windows の表示の拡大率)。QWidget::render で画像に描く
// ときは画像の devicePixelRatio になるので、ウィジェットの値ではなく描く先の変換から読む。
double device_scale(const QPainter &painter)
{
	return painter.deviceTransform().m11();
}

} // namespace

FitView::FitView(QWidget *parent) : QWidget(parent)
{
	QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	policy.setHeightForWidth(true);
	setSizePolicy(policy);
	setMinimumSize(320, 180);
}

void FitView::set_canvas(const QImage &canvas)
{
	canvas_ = canvas;
	scaled_valid_ = false;
	update();
}

QRectF FitView::target_rect() const
{
	if (canvas_.isNull() || width() <= 0 || height() <= 0)
		return QRectF();
	const double scale = std::min(static_cast<double>(width()) / canvas_.width(),
				      static_cast<double>(height()) / canvas_.height());
	const double w = canvas_.width() * scale;
	const double h = canvas_.height() * scale;
	return QRectF((width() - w) / 2.0, (height() - h) / 2.0, w, h);
}

void FitView::paintEvent(QPaintEvent *)
{
	++paint_count_;
	QPainter painter(this);
	painter.fillRect(rect(), kViewBackground);
	const QRectF target = target_rect();
	if (target.isEmpty())
		return;
	// 縮めた画像は、デバイスのピクセルの大きさで 1 度だけ作り、描く先の拡大率で置く (Qt に描くたびに縮めさせない)。
	const double dpr = device_scale(painter);
	const QSize device_size(std::max(1, static_cast<int>(std::lround(target.width() * dpr))),
				std::max(1, static_cast<int>(std::lround(target.height() * dpr))));
	if (!scaled_valid_ || scaled_.size() != device_size) {
		scaled_ = canvas_.scaled(device_size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
		scaled_valid_ = true;
	}
	if (scaled_.devicePixelRatio() != dpr)
		scaled_.setDevicePixelRatio(dpr);
	painter.drawImage(target.topLeft(), scaled_);
}

void FitView::mousePressEvent(QMouseEvent *event)
{
	const QRectF target = target_rect();
	const QPointF at = event->position();
	if (!click_ || target.isEmpty() || !target.contains(at))
		return;
	const double scale = canvas_.width() / target.width();
	click_(QPoint(static_cast<int>((at.x() - target.left()) * scale),
		      static_cast<int>((at.y() - target.top()) * scale)));
}

PixelView::PixelView(int scale, QWidget *parent) : QWidget(parent), scale_(std::max(1, scale)) {}

void PixelView::set_extent(const QRect &extent)
{
	if (extent == extent_)
		return;
	extent_ = extent;
	resize(sizeHint());
	update();
}

void PixelView::set_patch(const QRect &rect, const QImage &image)
{
	patch_rect_ = rect;
	if (scale_ == 1 || image.isNull()) {
		patch_ = image;
	} else {
		// 最近傍の拡大を自分で行う (1 ピクセルを scale x scale の同じ色にする。補間のある拡大にしない)。
		const QImage source = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
		patch_ = QImage(source.width() * scale_, source.height() * scale_, QImage::Format_ARGB32_Premultiplied);
		for (int y = 0; y < patch_.height(); ++y) {
			const auto *src = reinterpret_cast<const QRgb *>(source.constScanLine(y / scale_));
			auto *dst = reinterpret_cast<QRgb *>(patch_.scanLine(y));
			for (int x = 0; x < patch_.width(); ++x)
				dst[x] = src[x / scale_];
		}
	}
	update();
}

QRect PixelView::visible_canvas_rect(const QRect &viewport) const
{
	const QRect shown = viewport.intersected(rect());
	if (shown.isEmpty() || extent_.isEmpty())
		return QRect();
	const double per_pixel = devicePixelRatioF() / scale_; // 論理ピクセル 1 つに入るキャンバスのピクセル
	const int x0 = static_cast<int>(std::floor(shown.left() * per_pixel)) - 1;
	const int y0 = static_cast<int>(std::floor(shown.top() * per_pixel)) - 1;
	const int x1 = static_cast<int>(std::ceil((shown.right() + 1) * per_pixel)) + 1;
	const int y1 = static_cast<int>(std::ceil((shown.bottom() + 1) * per_pixel)) + 1;
	return QRect(extent_.x() + x0, extent_.y() + y0, x1 - x0, y1 - y0).intersected(extent_);
}

QSize PixelView::sizeHint() const
{
	if (extent_.isEmpty())
		return QSize(1, 1);
	const double dpr = devicePixelRatioF();
	return QSize(static_cast<int>(std::ceil(extent_.width() * scale_ / dpr)),
		     static_cast<int>(std::ceil(extent_.height() * scale_ / dpr)));
}

void PixelView::paintEvent(QPaintEvent *)
{
	++paint_count_;
	QPainter painter(this);
	painter.fillRect(rect(), kViewBackground);
	if (patch_.isNull() || extent_.isEmpty())
		return;
	const QTransform device = painter.deviceTransform();
	const double dpr = device_scale(painter);
	if (patch_.devicePixelRatio() != dpr)
		patch_.setDevicePixelRatio(dpr);
	// 見せる範囲の左上をデバイスのピクセルの格子に合わせ、重ねた部分をそこから整数のデバイスのピクセルだけずらして
	// 置く。端数の位置に置くと、1 ピクセルがにじむ。
	const QPointF origin = device.map(QPointF(0.0, 0.0));
	const QPointF snapped(std::round(origin.x()) + (patch_rect_.x() - extent_.x()) * scale_,
			      std::round(origin.y()) + (patch_rect_.y() - extent_.y()) * scale_);
	painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
	painter.drawImage(device.inverted().map(snapped), patch_);
}

void WheelSafeSlider::wheelEvent(QWheelEvent *event)
{
	if (!hasFocus()) {
		event->ignore();
		return;
	}
	QSlider::wheelEvent(event);
}

void WheelSafeSpinBox::wheelEvent(QWheelEvent *event)
{
	if (!hasFocus()) {
		event->ignore();
		return;
	}
	QDoubleSpinBox::wheelEvent(event);
}

void WheelSafeComboBox::wheelEvent(QWheelEvent *event)
{
	if (!hasFocus()) {
		event->ignore();
		return;
	}
	QComboBox::wheelEvent(event);
}

} // namespace cordmux::label_tuner
