// SPDX-License-Identifier: Apache-2.0

// 名前ラベルのプレビューツールだけが使う Qt のプラットフォームプラグイン cordmuxdecorpreview。製品の名前ラベル
// (sources/decor_paint.cpp) は、フォントの名前が空なら QFont() (アプリの既定のフォント) で描画する。OBS の中では、
// 既定のフォントは Qt 6.8.3 の windows プラグインが決め、フォントデータベースは DirectWrite 版になる。Qt 同梱の
// minimal は GDI 版を使うので、日本語と絵文字の代替フォントの選び方と文字の送り幅が OBS と違うことがある。そこで、
// 文字の描画に反映される 2 つ (フォントデータベースと既定のフォント) だけを windows プラグインと同じにする。ウィジェットを
// 描画しないので、テーマとパレットは合わせない (合わせるのは tests/ui_preview/preview_platform.cpp)。OS のウィンドウは
// 作らない。元にしたのは Qt 6.8.3 の plugins/platforms/minimal と、plugins/platforms/windows の qwindowsintegration.cpp
// (fontDatabase) と qwindowstheme.cpp (refreshFonts。既定のフォントはメッセージのフォント)。

#pragma once

namespace cordmux::decor_preview {

// このプラグインの名前 (QT_QPA_PLATFORM に入れる文字列)。main.cpp が Q_IMPORT_PLUGIN で静的に登録する。
inline constexpr const char *kDecorPlatformName = "cordmuxdecorpreview";

// プラグインのテーマが返す既定のフォントの名前を設定する。QGuiApplication を作る前に呼び出す。既定は Yu Gothic UI
// (日本語の Windows 10 で SPI_GETNONCLIENTMETRICS が返すメッセージのフォント)。英語の Windows での見え方を
// 確認するときは Segoe UI を渡す。
void set_system_font_family(const char *family);

} // namespace cordmux::decor_preview
