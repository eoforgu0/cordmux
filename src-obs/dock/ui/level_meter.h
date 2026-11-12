// SPDX-License-Identifier: Apache-2.0

// 音声タブのメーター (LevelMeter)。-48 dB から 0 dB を横の棒で示し、緑、-20 dB から黄、-9 dB から赤で塗る。灰色のときは棒の
// 全部を灰色で塗る。位置と灰色かどうかを受け取り、色の境目は dock_ui_calc.h の meter_segments で決める。
// 色はテーマの text-success、text-warning、text-danger から読み、読めないテーマでは決まった値にする。灰色は補足のテキストの色。

#pragma once

#include <QColor>
#include <QWidget>

#include "dock/ui/theme_read.h"

namespace cordmux::dock::ui {

// テーマが成功、警告、危険の色を持たないときの色。
inline constexpr unsigned kMeterFallbackGreen = 0x3fbf6d;
inline constexpr unsigned kMeterFallbackYellow = 0xe8c23a;
inline constexpr unsigned kMeterFallbackRed = 0xe8503f;

// メーターを塗る色の組。
struct MeterColors {
	QColor green;
	QColor yellow;
	QColor red;
	QColor gray;
	QColor trough; // 棒の下地
};

// テーマから色を決める。ground はメーターを置く背景 (補足のテキストの色を使えるかの判断に使う)。
MeterColors meter_colors(const ThemeRead &theme, const QColor &ground);

class LevelMeter : public QWidget {
public:
	explicit LevelMeter(QWidget *parent = nullptr);

	void set_colors(const MeterColors &colors);
	// 棒の高さ (px)。
	void set_bar_height(int px);
	// 位置 (0 から 1) と灰色かどうか。棒の幅 (px) か灰色かどうかが変わったときだけ塗り直す。
	void set_reading(double position, bool gray);

	const MeterColors &colors() const { return colors_; }
	double position() const { return position_; }
	bool gray() const { return gray_; }
	// 塗り直した回数 (ドックを操作するテストが、値が変わったメーターだけが塗り直されることを調べる)。
	int repaint_count() const { return repaints_; }
	// 棒の塗りつぶす幅 (px)。
	int fill_px() const { return fill_px_; }

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	void paintEvent(QPaintEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;

private:
	void refresh_fill();

	MeterColors colors_;
	int bar_height_ = 4;
	double position_ = 0.0;
	bool gray_ = false;
	int fill_px_ = 0;
	int repaints_ = 0;
};

} // namespace cordmux::dock::ui
