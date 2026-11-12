// SPDX-License-Identifier: Apache-2.0

// プレビューツールとドックを操作するテストが使う Qt のプラットフォームプラグイン cordmuxpreview。Qt 同梱の minimal は
// ウィンドウを表示しないが、OBS (Qt 6.8.3 の windows プラグイン) と次の 4 点が違う。フォントデータベースが GDI 版で、
// DirectWrite 版の OBS と QFontMetrics::maxWidth が違う (GDI 版は文字の大きさを 13 px に丸めて 23、DirectWrite 版は
// 16 前後) ので、そこから決まる QLineEdit の最小の幅がずれる。ClearType のガンマが Qt の既定の 1.7 になる (OBS は
// Windows の ClearType のコントラストから決める)。テーマ (ダイアログのアイコン、スタイル、フォント、パレット) がない。
// 画面が 240 x 320 なので、QMessageBox の幅の上限とメニューの位置が変わる。このプラグインは minimal と同じく OS の
// ウィンドウを作らず、この 4 点を windows プラグインに合わせる。元にしたのは Qt 6.8.3 の plugins/platforms/minimal と、
// plugins/platforms/windows の qwindowsintegration.cpp と qwindowstheme.cpp。

#pragma once

namespace cordmux::ui_preview {

// このプラグインの名前 (QT_QPA_PLATFORM に入れる文字列)。main.cpp が Q_IMPORT_PLUGIN で静的に登録する
// (登録するだけで、使うかどうかは QT_QPA_PLATFORM で決まる)。
inline constexpr const char *kPreviewPlatformName = "cordmuxpreview";

} // namespace cordmux::ui_preview
