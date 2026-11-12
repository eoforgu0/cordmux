// SPDX-License-Identifier: Apache-2.0

// シナリオが共通で使う関数 (コントラスト比、画像の差、フォーカスリングの測定、イベントの処理)。

#pragma once

#include <QColor>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <vector>

namespace cordmux::dock {
class DockWidget;
} // namespace cordmux::dock

namespace cordmux::dock_drive {

// root の子孫のうち、型 T の最初のウィジェット (cordmux のウィジェットは Q_OBJECT を持たないので、findChild が使えない)。
template <class T> T *descendant_of(QWidget *root)
{
	for (QWidget *w : root->findChildren<QWidget *>()) {
		if (auto *t = dynamic_cast<T *>(w))
			return t;
	}
	return nullptr;
}

// ドックの上の 4 つの固定の部分の矩形 (スクリーン座標)。1 行目、メインとサブの行、タブのバー、下のバー。メッセージや外部の
// 出来事で動かしてはいけない部分である。
struct Frame {
	QRect first_line;
	QRect accounts;
	QRect tab_bar;
	QRect bottom;
	bool operator==(const Frame &o) const
	{
		return first_line == o.first_line && accounts == o.accounts && tab_bar == o.tab_bar && bottom == o.bottom;
	}
	bool operator!=(const Frame &o) const { return !(*this == o); }
};
Frame frame_of(dock::DockWidget *widget);
// 矩形の一覧を「(x,y 幅x高さ)」の形にした文字列。
QString frame_text(const Frame &f);

// WCAG 2.x の相対輝度とコントラスト比 (達成基準 1.4.11 と 2.4.13 で、フォーカスの見え方を比べる方法)。
double relative_luminance(const QColor &c);
double contrast_ratio(const QColor &a, const QColor &b);

// フォーカスリングを測る。ウィンドウを、フォーカスリングを表示したときと隠したとき (フォーカスリングのウィジェットを
// 一時的に隠す) の 2 回描画し、違うピクセルをフォーカスリングとみなす。focused を追うフォーカスリングがなければ、
// ring_widget_found か ring_visible が偽の値を返す。
struct RingMeasure {
	bool ring_widget_found = false;
	bool ring_visible = false;   // フォーカスリングが表示されていて、focused を追っている
	bool drawn = false;          // 違うピクセルがある (実際に描かれた)
	QRect bbox;                  // フォーカスリングのピクセルの外接矩形 (ウィンドウの座標)
	QRect hit;                   // focused の押せる範囲 (ウィンドウの座標。focus_hit_rect_of の値)
	double contrast_p05 = 0.0;   // リングの芯のピクセルと、その下の背景のコントラスト比の 5 パーセンタイル
	double contrast_median = 0.0;
	int core_pixels = 0;
	QRect ring_rect;             // フォーカスリングのウィジェットの矩形 (ウィンドウの座標。表示範囲で切る前)
	// 辺ごとの集計。並びは左、上、右、下。side_len は押せる範囲の辺の長さ (px)、side_cover はそのうち、辺の外側
	// (押せる範囲とフォーカスリングの矩形の間) にリングのピクセルがある位置の数。表示範囲の外で切れた辺は
	// side_cover が 0 になる。リングは ring_rect の内側に沿って描かれる。
	int side_len[4] = {0, 0, 0, 0};
	int side_cover[4] = {0, 0, 0, 0};
	QImage with_ring;            // フォーカスリングを表示した画像 (書き出し用)
};
RingMeasure measure_ring(QWidget *top, QWidget *focused);

// フォーカスリングの辺ごとの集計 (RingMeasure の side_len と side_cover を埋める)。pixels はリングのピクセル
// (ウィンドウの座標)。measure_ring と、集計のセルフテスト (keyboard のシナリオ) が使う。
void count_ring_sides(const std::vector<QPoint> &pixels, const QRect &hit, const QRect &ring_rect, int side_len[4],
		      int side_cover[4]);

// 4 辺のうち、辺の全長にリングのピクセルがある辺の数 (0 から 4)。
int ring_sides_drawn(const int side_len[4], const int side_cover[4]);

// ウィンドウを不透明な画像に描画する。ClearType の文字のにじみを OBS のウィンドウと同じにするためで、プレビューツールの
// grab_widget と同じ方法である。
QImage grab_window_image(QWidget *top);

// root の子孫のうち、クリックでフォーカスを取るチェック、ボタン、コンボボックスの名前 (objectName かクラス名)。ドックのタブの中には
// ないはずのもの (クリックでフォーカスを取ると、OBS が前面のまま Space や矢印のキーを押したときに同じ操作が働く)。
QStringList click_focus_controls(QWidget *root);

// ms ミリ秒の間、イベントを処理する。イベントループの入れ子の中 (QMenu::exec の中など) からは呼ばない。
void pump(int ms);

} // namespace cordmux::dock_drive
