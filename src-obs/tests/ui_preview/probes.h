// SPDX-License-Identifier: Apache-2.0

// プレビューツールの描画の対象 (--face) のうち、1 枚の画像では済まないもの (focus、timeline、theme-switch、
// probe:*) と、faces.cpp と共有する書き出しの関数。どれも製品のウィジェットを製品と同じ方法で動かし (フォーカスは
// キーボードのイベントを送ってから setFocus、並びの保留はマウスのボタンを押す、テーマの切り替えは OBS の
// THEME_CHANGED と同じ DockWidget::on_theme_changed)、プレビューの側でウィジェットの中身を書き換えない。

#pragma once

#include <QImage>
#include <QPoint>
#include <QString>

#include <vector>

#include "faces.h"
#include "obs_theme.h"
#include "render.h"

class QAbstractButton;
class QApplication;
class QMenu;
class QPushButton;
class QWidget;

namespace cordmux::ui_preview {

// 1 つの出力先 (テーマ x 言語) への書き出し。
struct Writer {
	QString dir;
	QString metrics;
	QString targets;  // targets.tsv の行 (見出しは書くときに付ける)
	QString names;    // names.tsv
	QString contrast; // contrast.tsv
	QString extra_name; // 描画の対象ごとの表 (focus.tsv など) の名前
	QString extra;      // その中身 (見出しを含む)
	QString fields;     // fields.tsv の行 (1 行の入力欄のテキストが幅に入るか。見出しは書くときに付ける)
	QString wraps;      // wraps.tsv の行 (折り返す QLabel の行と高さ。見出しは kWrapsHeader。faces.cpp)
	// uninstall-entry.tsv の行 ([ツール] からアンインストールを始めたときのウィンドウ。
	// faces.cpp の uninstall_entry_row)
	QString uninstall_entry;
	QString screen_fit; // screen-fit.tsv の行 (ウィンドウが画面の作業領域に収まるか。faces.cpp の screen_fit_row)
	QString index_html;
	int failures = 0;

	void shot(const QString &name, const QImage &image, const QString &widgets);
	void write_tables() const;
};

void print_line(const QString &line);
bool write_text_file(const QString &path, const QString &text);

// root と、その子のすべてのウィジェットを widgets.tsv の行にする。origin は、画像の上での root の左上。
void dump_widgets(QWidget *root, const QPoint &origin, int *index, QString *out);
extern const char *const kWidgetsHeader;
extern const char *const kWrapsHeader;

// 開いたウィンドウを描画する (プレビューのプラットフォームプラグインの上で show し、描画してから隠す)。widgets に
// ウィジェットの一覧を書く。
QImage grab_window(QWidget *window, QString *widgets);

// ドックの上にメニュー (と、開いたサブメニュー) を重ねた画像。menus の 1 つ目を anchor の下に開き、2 つ目からは前の
// メニューの項目 (submenu_actions の同じ番目) の右に開く。widgets にドックとメニューのウィジェットの一覧を書く。
QImage grab_dock_with_menus(DockHost &host, const std::vector<QMenu *> &menus, QPushButton *anchor, QString *widgets,
			    std::vector<QPoint> *menu_origins = nullptr);

double em_px_of(QWidget *w);

// 描画の対象 (probes.cpp)。0 を返したら描画できた。
int face_focus(const FaceOptions &opt, Writer &w);
int face_timeline(const FaceOptions &opt, Writer &w);
int face_theme_switch(QApplication &app, const ObsPaths &paths, const FaceOptions &opt, const QString &theme_name,
		      Writer &w);
int face_probe_menu_check(QApplication &app, const ComposedTheme &theme, Writer &w);
// OBS の既定のレイアウトで、下に並ぶドックの大きさを obs-layout.tsv に書く (OBSBasic::on_resetDocks_triggered と同じ
// 計算を、Qt のレイアウトで行う)。
int face_probe_obs_layout(const FaceOptions &opt, Writer &w);

} // namespace cordmux::ui_preview
