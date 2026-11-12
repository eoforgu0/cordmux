// SPDX-License-Identifier: Apache-2.0

// ドックと cordmux のダイアログで使う余白・幅・高さ (px) を、OBS の本文のフォントの大きさ (1 em) から計算する。
// 1 em は OBS の [外観] の [フォントサイズ] に従う。QSS の `em` (行の高さの倍数) とは別の量なので、スタイルシートには
// 書かず、ここで px にしてレイアウトの余白と間隔 (setContentsMargins、setSpacing) に使う。名前ラベルの em
// (配信の画面の高さに対する割合。sources/decor_params.h の kDefaultLabelEmRatio) とも別の量である。
// compute_dock_metrics は Qt に依存しないので、単体テストから直接呼び出せる。read_dock_metrics は Qt を使い、
// 実装は dock_metrics.cpp にある。使う側は、テーマ、[フォントサイズ]、[密度] が変わったとき (OBS の THEME_CHANGED
// と、Qt の StyleChange) に計算し直す。

#pragma once

#include "dock/ui/dock_style.h"
#include "dock/ui/dock_ui_calc.h"

class QWidget;

namespace cordmux::dock::ui {

struct ThemeRead;

// OBS の既定のドック (シーン、ソース) の外周の左の余白 (px)。
inline constexpr int kObsDockOuterLeftPx = 1;

// 既定の値は、本文のフォントが 10 pt で、論理 DPI が 96 のときの値である。
struct DockMetrics {
	double base_pt = 10.0; // OBS の本文のフォントの大きさ (pt)。倍率を掛ける元の値
	double em_px = 13.333; // 1 em の px (デバイスに依存しないピクセル)
	int small_px = 3;      // 小さい余白 (0.25 em)
	int large_px = 7;      // 大きい余白 (0.5 em)
	int group_gap_px = 10; // まとまりの間隔 (0.75 em)
	// 見出しの段と説明の文と、意味の違う部分の間の余白 (DockStyle の同じ名前の em の値。既定は 1、1、0.5、2.25、0.25、1、0.5、1 em)。
	int group_rule_above_px = 13;
	int group_heading_above_px = 13;
	int group_heading_below_px = 7;
	int sub_heading_above_px = 30;
	int sub_heading_below_px = 3;
	int status_heading_above_px = 13;
	int note_below_px = 7;
	int section_gap_px = 13;
	int icon_px = 16;      // アイコンの大きさ (OBS のテーマの --icon_base)
	int row_min_height_px = 24; // 一覧の行の高さの下限
	int name_floor_px = 53;     // 一覧の行で名前に残す最小の幅 (4 em)
	int paragraph_max_px = 533; // 段落の 1 行の幅の上限 (40 em)
	int dialog_max_px = 600;    // ダイアログの幅の上限 (45 em)
	int focus_ring_px = 2;      // フォーカスリングの太さ
	int focus_ring_gap_px = 1;  // フォーカスリングと、押せる範囲の間の空き
	// ドックのテキストの左端。OBS のシーンのドックの左端から、一覧の項目のテキストの左端までの px で、OBS の
	// ドックの外周の左 1 px と、テーマが一覧と項目に付ける余白と枠線の和である。[密度] で変わる。ドックの外周の
	// 左右の余白に使い、cordmux のテキストの端を、シーンのドックのテキストの端にそろえる。テーマから読めない
	// ときは large_px と同じ値にする。
	int edge_text_px = 7;

	bool operator==(const DockMetrics &o) const
	{
		return base_pt == o.base_pt && em_px == o.em_px && small_px == o.small_px && large_px == o.large_px &&
		       group_gap_px == o.group_gap_px && group_rule_above_px == o.group_rule_above_px &&
		       group_heading_above_px == o.group_heading_above_px &&
		       group_heading_below_px == o.group_heading_below_px && sub_heading_above_px == o.sub_heading_above_px &&
		       sub_heading_below_px == o.sub_heading_below_px &&
		       status_heading_above_px == o.status_heading_above_px && note_below_px == o.note_below_px &&
		       section_gap_px == o.section_gap_px && icon_px == o.icon_px &&
		       row_min_height_px == o.row_min_height_px && name_floor_px == o.name_floor_px &&
		       paragraph_max_px == o.paragraph_max_px && dialog_max_px == o.dialog_max_px &&
		       focus_ring_px == o.focus_ring_px && focus_ring_gap_px == o.focus_ring_gap_px &&
		       edge_text_px == o.edge_text_px;
	}
	bool operator!=(const DockMetrics &o) const { return !(*this == o); }
};

// base_pt は本文のフォントの大きさ (pt)。logical_dpi はウィジェットの論理 DPI で、Qt 6 では拡大率によらず 96 前後
// になる。icon_px はテーマのアイコンの大きさ。list_text_left_px は、テーマを適用した一覧の左端から項目のテキストの
// 左端までの px (ThemeRead::list_text_left_px)。0 以下の base_pt と logical_dpi は 10 pt と 96 に、0 以下の
// icon_px は 16 に置き換える。list_text_left_px が負のとき (読めなかったとき) は edge_text_px を large_px にする。
inline DockMetrics compute_dock_metrics(double base_pt, double logical_dpi, int icon_px, const DockStyle &style,
					int list_text_left_px = -1)
{
	DockMetrics m;
	m.base_pt = base_pt > 0.0 ? base_pt : 10.0;
	const double dpi = logical_dpi > 0.0 ? logical_dpi : 96.0;
	m.em_px = m.base_pt * dpi / 72.0;
	// 小さい余白は 1 px、大きい余白は 2 px を下限にする。フォントが極端に小さくても、余白が 0 になって
	// ウィジェットどうしが接しないようにするため。
	m.small_px = calc::px_from_em(m.em_px, style.spacing_small_em, 1);
	m.large_px = calc::px_from_em(m.em_px, style.spacing_large_em, 2);
	m.group_gap_px = calc::px_from_em(m.em_px, style.group_gap_em, m.large_px);
	// 見出しと説明の文の余白は、どれも行の間 (small_px) を下限にする。各部分は行の間を引いた残りを自分の余白にするので、行の間より
	// 小さくすると、引いた残りが負になる。
	m.group_rule_above_px = calc::px_from_em(m.em_px, style.group_rule_above_em, m.small_px);
	m.group_heading_above_px = calc::px_from_em(m.em_px, style.group_heading_above_em, m.small_px);
	m.group_heading_below_px = calc::px_from_em(m.em_px, style.group_heading_below_em, m.small_px);
	m.sub_heading_above_px = calc::px_from_em(m.em_px, style.sub_heading_above_em, m.small_px);
	m.sub_heading_below_px = calc::px_from_em(m.em_px, style.sub_heading_below_em, m.small_px);
	m.status_heading_above_px = calc::px_from_em(m.em_px, style.status_heading_above_em, m.small_px);
	m.note_below_px = calc::px_from_em(m.em_px, style.note_below_em, m.small_px);
	m.section_gap_px = calc::px_from_em(m.em_px, style.section_gap_em, m.small_px);
	m.icon_px = icon_px > 0 ? icon_px : 16;
	m.row_min_height_px = style.row_min_height_px;
	m.name_floor_px = calc::px_from_em(m.em_px, style.name_floor_em, 1);
	m.paragraph_max_px = calc::px_from_em(m.em_px, style.paragraph_max_em, 1);
	m.dialog_max_px = calc::px_from_em(m.em_px, style.dialog_max_em, 1);
	m.focus_ring_px = style.focus_ring_px;
	m.focus_ring_gap_px = style.focus_ring_gap_px;
	// OBS のシーンのドックは、外周の左に 1 px の余白を持つ (OBS 31.1.1 の frontend/forms/OBSBasic.ui で
	// scenesFrame を囲むレイアウト)。
	m.edge_text_px = list_text_left_px >= 0 ? kObsDockOuterLeftPx + list_text_left_px : m.large_px;
	return m;
}

// root から本文のフォントの大きさを読み、theme のアイコンの大きさと合わせて計算する。root には、cordmux が
// スタイルシートを設定していないウィジェット (ドックのルートのウィジェットか、ダイアログそのもの) を渡す。
// フォントの倍率を適用したウィジェットから読むと、計算し直すたびに倍率が重なる。
DockMetrics read_dock_metrics(QWidget *root, const ThemeRead &theme, const DockStyle &style);

// root の本文のフォントの大きさ (pt)。テーマが px で指定しているときは pt に換算する。root が nullptr のときと、
// 大きさを読めないときは 10 を返す。
double base_point_size(QWidget *root);

} // namespace cordmux::dock::ui
