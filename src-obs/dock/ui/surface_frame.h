// SPDX-License-Identifier: Apache-2.0

// メッセージのレベル 1 の 1 行目の背景の上に描くテキストの色 (paint_on_surface)。
// 背景はテーマの危険の色 (theme_read の l1_surface で読んだ色) で塗る。危険の色を持たないテーマでは本文の
// 色で塗り、色を反転したバーにする。背景とのコントラスト比が 3:1 に届かないときだけ、外周に本文の色の枠線を
// 描く。余白は変えない (背景の有無で、ドックの 1 行目の高さを変えないため)。背景を使うのはメッセージのレベル 1
// だけ。背景の上のテキストとアイコンは白か黒 (L1Surface::foreground) にし、弱いテキストは使わない。

#pragma once

#include <QColor>
#include <QWidget>

namespace cordmux::dock::ui {

// 背景の上のテキストの色を設定する (QLabel など、テキストを持つウィジェット)。色はウィジェット自身の
// スタイルシートの "color" の部分に、計算した後の色として書く (set_class.h の set_own_style)。w が nullptr のときと、
// foreground が無効な色のときは何もしない。
void paint_on_surface(QWidget *w, const QColor &foreground);
// 背景の上のテキストの色を外し、テーマのテキストの色に戻す。背景を外したときに呼ぶ。
void clear_surface_paint(QWidget *w);

} // namespace cordmux::dock::ui
