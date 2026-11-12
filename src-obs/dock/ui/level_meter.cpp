// SPDX-License-Identifier: Apache-2.0

// 音声タブのメーター (level_meter.h) を実装する。

#include "dock/ui/level_meter.h"

#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>

#include <algorithm>
#include <cmath>

#include "dock/ui/dock_ui_calc.h"

namespace cordmux::dock::ui {

MeterColors meter_colors(const ThemeRead &theme, const QColor &ground)
{
	MeterColors c;
	c.green = theme.success.isValid() ? theme.success : QColor::fromRgb(kMeterFallbackGreen);
	c.yellow = theme.warning.isValid() ? theme.warning : QColor::fromRgb(kMeterFallbackYellow);
	c.red = theme.danger.isValid() ? theme.danger : QColor::fromRgb(kMeterFallbackRed);
	c.gray = muted_or_text(theme, ground);
	c.trough = theme.text;
	c.trough.setAlpha(40);
	return c;
}

LevelMeter::LevelMeter(QWidget *parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("cordmuxLevelMeter"));
	setAttribute(Qt::WA_OpaquePaintEvent, false);
	setFocusPolicy(Qt::NoFocus);
	// 棒は読み上げの対象にしない (値はつまみとパーセントが伝える)。
	setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
}

void LevelMeter::set_colors(const MeterColors &colors)
{
	colors_ = colors;
	update();
}

void LevelMeter::set_bar_height(int px)
{
	px = std::max(1, px);
	if (bar_height_ == px)
		return;
	bar_height_ = px;
	updateGeometry();
	update();
}

void LevelMeter::set_reading(double position, bool gray)
{
	const double p = std::isnan(position) ? 0.0 : std::clamp(position, 0.0, 1.0);
	const int fill = calc::meter_fill_px(p, width());
	const bool same = fill == fill_px_ && gray == gray_;
	position_ = p;
	gray_ = gray;
	fill_px_ = fill;
	if (same)
		return;
	++repaints_;
	update();
}

void LevelMeter::refresh_fill()
{
	const int fill = calc::meter_fill_px(position_, width());
	if (fill == fill_px_)
		return;
	fill_px_ = fill;
	++repaints_;
	update();
}

QSize LevelMeter::sizeHint() const
{
	return QSize(0, bar_height_);
}

QSize LevelMeter::minimumSizeHint() const
{
	return QSize(0, bar_height_);
}

void LevelMeter::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	refresh_fill();
}

void LevelMeter::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	if (!isEnabled())
		p.setOpacity(0.4);
	const int w = width();
	const int h = height();
	const QRect bar(0, std::max(0, (h - bar_height_) / 2), w, std::min(h, bar_height_));
	p.fillRect(bar, colors_.trough);
	if (fill_px_ <= 0)
		return;
	if (gray_) {
		p.fillRect(QRect(bar.left(), bar.top(), fill_px_, bar.height()), colors_.gray);
		return;
	}
	// 色の境目は全幅に対して固定で、棒の長さに合わせて縮めない。
	const calc::MeterSegments seg = calc::meter_segments(position_);
	const int green_px = std::min(fill_px_, calc::meter_fill_px(seg.green_end, w));
	const int yellow_px = std::min(fill_px_, std::max(green_px, calc::meter_fill_px(seg.yellow_end, w)));
	p.fillRect(QRect(0, bar.top(), green_px, bar.height()), colors_.green);
	if (yellow_px > green_px)
		p.fillRect(QRect(green_px, bar.top(), yellow_px - green_px, bar.height()), colors_.yellow);
	if (fill_px_ > yellow_px)
		p.fillRect(QRect(yellow_px, bar.top(), fill_px_ - yellow_px, bar.height()), colors_.red);
}

} // namespace cordmux::dock::ui
