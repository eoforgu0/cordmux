// SPDX-License-Identifier: Apache-2.0

// テーマから色とアイコンを読む (read_theme)。色を読みたい場所と同じコンテナーに見えないラベルを置き、OBS のテーマの
// クラス (setProperty("class", ...)) を付けて polish し、ラベルのパレットのテキストの色 (WindowText) と背景の色
// (Window) を読む。クラスを付けたラベルの色が、クラスのないラベルの色と同じなら、そのテーマはそのクラスを持たない。
// 色をコードに書かないのは、利用者が選んだテーマの色に合わせるためである。
// OBS の THEME_CHANGED (テーマ、[フォントサイズ]、[密度] を変えたときに通知される) と、ウィジェットの StyleChange
// と PaletteChange のときに、もう一度読む。読んだ結果をウィジェットに配るのは使う側である。

#pragma once

#include <QColor>
#include <QIcon>

#include "dock/ui/dock_style.h"
#include "dock/ui/dock_ui_calc.h"

class QWidget;

namespace cordmux::dock::ui {

// テーマから読んだ色とアイコン。テーマがクラスを持たないときの代わりは次のとおり。
//   クラス          持たないテーマの例      代わり
//   text-danger     利用者が追加したテーマ  メッセージのレベル 1 のパネルを本文の色で塗り、テキストとアイコンを
//                                           背景の色で描く (色を反転したバー)
//   text-muted      System                  弱いテキストを使わず、本文の色にする
//   frame-notice    System                  ドックの背景を本文の色の方へ少し寄せた色 (panel_fill)。has_notice を見て使う側が選ぶ
//   icon-dots-vert  利用者が追加したテーマ  cordmux のアイコン (status_icons.h の fallback_dots_icon)
struct ThemeRead {
	QColor text;      // 本文の色 (クラスのないラベルの WindowText)
	QColor muted;     // text-muted のテキストの色 (なければ text)
	QColor danger;    // text-danger のテキストの色 (なければ無効な色)
	QColor warning;   // text-warning のテキストの色 (なければ無効な色。使う側は本文の色で描く)
	QColor success;   // text-success のテキストの色 (なければ無効な色。使う側は本文の色で描く)
	QColor base_bg;   // コンテナーの背景 (ドックなら OBSDock の直下のウィジェット、ダイアログならダイアログ)
	QColor window_bg; // ダイアログの背景 (QDialog の Window)
	QColor notice_bg; // frame-notice の背景 (なければ base_bg)
	bool has_muted = false;
	bool has_danger = false;
	bool has_notice = false;
	QIcon desktop_cap; // OBS のメインウィンドウの desktopCapIcon (画面共有のアイコン)
	bool has_desktop_cap = false;
	QIcon dots; // btn-tool icon-dots-vert のアイコン ([...])
	bool has_dots = false;
	int icon_px = 16; // btn-tool のアイコンの大きさ (OBS のテーマの --icon_base)
	// テーマを適用した一覧 (OBS のシーンのドックの一覧と同じ QListWidget。枠なし) の左端から、1 つ目の項目の
	// テキストの左端までの px (DockMetrics::edge_text_px の元の値。[密度] で変わる)。読めなければ -1。コンテナーが
	// ドックの中身のときは、OBS のドックの一覧と同じ規則 (Yami の OBSDock QListWidget など) が適用される。
	int list_text_left_px = -1;
	bool dark = true; // OBS のテーマが暗いか (obs_frontend_is_theme_dark。OBS のアイコンの Dark と Light の選択に使う)
};

// container は色を読みたい場所のコンテナー (ドックの中身のウィジェット、設定ウィンドウ、参加者の設定ウィンドウ)。
// 見えないラベルをその子として一時的に作り、読んだら削除する。main_window は OBS のメインウィンドウ
// (使う側が obs_frontend_get_main_window() で取得して渡す。プレビューツールは、OBS と同じクラス名のウィジェットを
// 渡す)。main_window が nullptr なら画面共有のアイコンを読まない。container が nullptr なら、既定の値を返す。
ThemeRead read_theme(QWidget *container, QWidget *main_window);

calc::Rgb to_rgb(const QColor &c);
double contrast_ratio(const QColor &a, const QColor &b);
// パネルの上のテキストとアイコンの色。白と黒のうち、パネルとのコントラスト比が高い方 (calc::black_or_white_on)。
QColor black_or_white_on(const QColor &surface);

// 弱いテキストを背景の色 ground の上で使ってよいか (テーマが text-muted を持ち、コントラスト比が 4.5:1 に届く
// ときだけ true)。
bool muted_ok(const ThemeRead &theme, const QColor &ground);
// 補足のテキストの色。弱いテキストの色か、使えなければ本文の色。
QColor muted_or_text(const ThemeRead &theme, const QColor &ground);

// メッセージのレベル 1 のパネル。
struct L1Surface {
	QColor fill;       // パネルの色 (テーマの危険の色。持たなければ本文の色で、色を反転したバーにする)
	QColor foreground; // パネルの上のテキストとアイコンの色 (白か黒。反転したバーなら背景の色)
	QColor border;     // 外周の枠線の色 (本文の色)
	int border_px = 0; // 外周の枠線の太さ (0 なら描かない)
	bool inverted = false;
};
// ground はパネルを置く背景 (ドックなら base_bg、ダイアログの中なら window_bg)。
L1Surface l1_surface(const ThemeRead &theme, const QColor &ground, const DockStyle &style);

// メッセージのカードと状態タブのパネルの背景。テーマが frame-notice の背景を持ち、カードの上でボタンが見分けにくくならない
// (card_frame_ok) ときはその色、そうでなければ、ドックの背景を本文の色の方へ少しだけ寄せた色にする。
QColor panel_fill(const ThemeRead &theme, bool card_frame_ok);

// 設定ウィンドウの項目のアイコン (OBS の設定のウィンドウのアイコンと同じセット)。見つからなければ空の QIcon を
// 返す (使う側が cordmux のアイコンを描く)。
enum class SettingsIcon { General, Appearance, Stream, Output, Audio, Video, Hotkeys, Accessibility, Advanced };
QIcon settings_icon(const ThemeRead &theme, SettingsIcon which);

} // namespace cordmux::dock::ui
