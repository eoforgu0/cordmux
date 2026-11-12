// SPDX-License-Identifier: Apache-2.0

// プレビューの描画。製品のドックを作り、OBS と同じテーマを適用し、ウィンドウを表示せずに画像にする。

#pragma once

#include <QImage>
#include <QPalette>
#include <QString>

#include <memory>

#include "dock/ui/dock_style.h"
#include "fixtures.h"
#include "ledger/dock_view.h"
#include "obs_theme.h"

class QApplication;
class OBSDock;

namespace cordmux::dock {
class DockWidget;
}

namespace cordmux::ui_preview {

// OBS のテーマとフォントのフォルダー。
struct ObsPaths {
	QString themes_dir; // <obs-studio のソース>/frontend/data/themes
	QString fonts_dir;  // <obs-studio のソース>/frontend/forms/fonts
};

// OBS 31.1.1 が起動するときの処理 (obs-main.cpp の run_program と、OBSApp::InitTheme と SetTheme) を同じ順に行う。
// Open Sans の 3 つのフォントを追加し、スタイル (OBSProxyStyle。基底は windowsvista)、パレット、QSS の順に適用する。
// プレビューツールは 1 回の実行でテーマを切り替えるので、何度呼び出してもよい。
// platform_defaults が true (プラットフォームプラグインが cordmuxpreview) なら、OBS とまったく同じ呼び方をする。
// 基底のスタイルは名前を指定せずに作り (プラグインのテーマが Windows 10 と同じ windowsvista を返す)、OBS の
// defaultPalette は、最初に呼び出した時点のアプリのパレットにする (プラグインのテーマが Windows と同じものを返す)。
// false (Qt 同梱の minimal) なら、プラグインが返せないこの 2 つを windowsvista と windows_light_base_palette() で補う。
// appearance は OBS の [外観] の [フォントサイズ] と [密度] (既定値は OBS の既定値)。
bool load_obs_fonts(const ObsPaths &paths, QString *error);
bool apply_obs_theme(QApplication &app, const ObsPaths &paths, const QString &theme_id, bool platform_defaults,
		     ComposedTheme *theme, QString *error, const Appearance &appearance = Appearance{});

// Windows 10 (アプリのモードがライト) で、Qt 6.8.3 の windows プラグインが作るアプリのパレット
// (qwindowstheme.cpp の QWindowsTheme::systemPalette(Light))。OBS が上書きしない役割 (Midlight、Shadow、
// BrightText など) はこの値のまま残る。cordmuxpreview のテーマがこれを返し、minimal のときは apply_obs_theme が
// OBS の defaultPalette の代わりに使う。
QPalette windows_light_base_palette();

// 1 つのドック (OBSDock の中に製品の DockWidget を入れたもの)。フローティングのウィンドウの中身として描画する
// (タイトルバーは OS が描画するので、画像には入らない)。
struct DockHost {
	std::unique_ptr<OBSDock> dock;
	dock::DockWidget *widget = nullptr;
	ledger::dockview::DockViewV2 view; // 描画した View (製品の判断を通したもの)
};

// 場面を描画したドックを作る (製品の DockPoll と同じく、decide_dock、stabilize、render の順に通す)。
DockHost build_dock(const Fixture &fixture, const dock::ui::DockStyle &style);

// ドックを width x height にした画像。height が 0 以下なら、スクロールが不要になる高さまで伸ばす (下限は min_height)。
struct DockShot {
	QImage image;
	int width = 0;
	int height = 0;
	bool size_reached = false; // 指定した大きさになったか (製品の最小の大きさより小さくはできない)
	bool still_scrolls = false; // 伸ばしてもスクロールが残った (高さの上限に達した)
};
DockShot render_dock(DockHost &host, int width, int height, int min_height);

// 開いたウィンドウかメニューを、不透明な画像に描画する。ウィジェットは、呼び出す側が先に show しておく
// (Qt::WA_DontShowOnScreen を付けるか、プレビューのプラットフォームプラグインの上なら、画面には表示されない)。
// たまっているレイアウトの要求を処理してから描画する。
QImage grab_widget(QWidget *widget);

// 画像の拡大率 (既定値は 1。--dpr 1.5 は表示の倍率 150%)。ウィジェットの大きさと配置は論理 px のまま、塗りだけを
// 細かくする (Windows の 150% で Qt が devicePixelRatio 1.5 で描画するのと同じ)。grab_widget と render_dock の
// 画像に反映される。
void set_grab_dpr(double dpr);

// たまっているレイアウトの要求と再描画を処理する。
void settle();

} // namespace cordmux::ui_preview
