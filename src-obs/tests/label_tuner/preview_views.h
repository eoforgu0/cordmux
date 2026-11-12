// SPDX-License-Identifier: Apache-2.0

// 名前ラベルの調整ツールのプレビューのウィジェット。FitView はキャンバス全体をウィジェットに合わせて縮めて描き、
// PixelView はキャンバスの一部を、画像の 1 ピクセルが画面の scale x scale ピクセルになるように描く (Windows の
// 表示の拡大率 devicePixelRatio を打ち消し、拡大は最近傍)。PixelView は、見せる範囲のうちスクロールエリアに
// 見えている所だけを重ねた画像を受け取る。ほかに、フォーカスのないときのマウスのホイールをスクロールに渡す
// スライダーと数の欄とコンボボックス (つまみの並びをスクロールしたときに、値が変わらないように)。

#pragma once

#include <functional>

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QImage>
#include <QPoint>
#include <QSlider>
#include <QWidget>

namespace cordmux::label_tuner {

class FitView : public QWidget {
public:
	explicit FitView(QWidget *parent = nullptr);

	// 表示するキャンバス。縮めた画像は、次に描くときに作り直す。
	void set_canvas(const QImage &canvas);

	// クリックしたキャンバスの位置 (ピクセル) を受け取る関数。キャンバスの外のクリックでは呼ばない。
	void set_click_handler(std::function<void(const QPoint &)> handler) { click_ = std::move(handler); }

	QSize sizeHint() const override { return QSize(640, 360); }
	// 描いた回数 (--drive が、画面のスレッドで描いていることを数える)。
	int paint_count() const { return paint_count_; }
	// 高さは幅の 9 / 16 (キャンバスの縦横比) にする。上下に余白を作らない。
	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int width) const override { return width * 9 / 16; }

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;

private:
	// キャンバスを縦横比を保って収める矩形 (ウィジェットの座標)。
	QRectF target_rect() const;

	QImage canvas_;
	QImage scaled_;
	bool scaled_valid_ = false;
	int paint_count_ = 0;
	std::function<void(const QPoint &)> click_;
};

class PixelView : public QWidget {
public:
	// scale は 1 (等倍) か 2 (2 倍)。
	explicit PixelView(int scale, QWidget *parent = nullptr);

	// 見せる範囲 (キャンバスのピクセル)。ウィジェットの大きさを、その範囲を scale 倍した大きさにする。
	void set_extent(const QRect &extent);
	const QRect &extent() const { return extent_; }

	// 重ねた画像のうち、キャンバスの rect の部分 (等倍)。rect は見せる範囲の中にあること。描いていない所は背景の色。
	void set_patch(const QRect &rect, const QImage &image);

	// viewport (このウィジェットの座標) に見えている、キャンバスの範囲。端の 1 ピクセルを足してある。
	QRect visible_canvas_rect(const QRect &viewport) const;

	QSize sizeHint() const override;

	// 描いた回数。
	int paint_count() const { return paint_count_; }

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	int paint_count_ = 0;
	int scale_ = 1;
	QRect extent_;
	QRect patch_rect_;
	QImage patch_; // scale 倍にした画像
};

// フォーカスのないときのホイールを、親 (スクロールエリア) に渡す。
class WheelSafeSlider : public QSlider {
public:
	using QSlider::QSlider;

protected:
	void wheelEvent(QWheelEvent *event) override;
};

class WheelSafeSpinBox : public QDoubleSpinBox {
public:
	using QDoubleSpinBox::QDoubleSpinBox;

protected:
	void wheelEvent(QWheelEvent *event) override;
};

class WheelSafeComboBox : public QComboBox {
public:
	using QComboBox::QComboBox;

protected:
	void wheelEvent(QWheelEvent *event) override;
};

} // namespace cordmux::label_tuner
