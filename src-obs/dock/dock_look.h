// SPDX-License-Identifier: Apache-2.0

// ドックのウィジェット (1 行目、メッセージのカード、下のバー) が共通で使う、テーマから読んだ見た目の値。DockWidget が
// テーマを読むたびに 1 つ作り、各ウィジェットに渡す。ウィジェットごとにテーマを読み直すと、読んだ時点の違う値で
// 1 つのドックを描くことになるためである。値をまとめるだけで、判断は持たない。

#pragma once

#include <QColor>
#include <QtGlobal>

#include "dock/ui/dock_metrics.h"
#include "dock/ui/dock_style.h"
#include "dock/ui/theme_read.h"

namespace cordmux::dock {

struct DockLook {
	ui::DockStyle style;
	ui::ThemeRead theme;
	ui::DockMetrics metrics;
	double base_pt = 10.0; // OBS の本文のフォントサイズ (pt)。ドックのフォントサイズは、これに倍率をかけて決める
	ui::L1Surface l1;      // メッセージのレベル 1 のパネル (ドックの背景の上に描く)
	qreal dpr = 1.0;       // アイコンを描くときのデバイスピクセル比
	// メッセージのカードの背景に、OBS のテーマの frame-notice の背景色を使うか (ui::panel_fill)。カードの背景の上のボタンが、
	// ドックの背景の上のボタンより見分けにくくなるテーマでは false にし、frame-notice を持たないテーマと同じく、ドックの背景を
	// 本文の色の方へ少し寄せた色にする。Yami Classic は、カードの背景とボタンの背景が同じ rgb(76,76,76) なので false になる。
	bool card_frame_ok = true;
	// 下のバーとスクロール領域の間の区切り線の色。OBS のドックの下のツールバー (`OBSDock QToolBar`) の上の枠線と
	// 同じ色をテーマから読む。テーマがツールバーに線を引かないときは無効な色にし、Qt の標準の区切り線を使う。
	QColor bar_rule;
};

} // namespace cordmux::dock
