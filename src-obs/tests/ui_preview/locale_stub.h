// SPDX-License-Identifier: Apache-2.0

// obs_module_text() と obs_frontend_get_locale_string() の代わり (プレビューツールとドックを操作するテストが使う)。
// 製品では libobs の OBS_MODULE_USE_DEFAULT_LOCALE("cordmux", "en-US") が、en-US.ini を読んだ上に選んだ言語の ini を
// 重ねる (obs_module_load_locale)。ここも同じ順で重ねるので、ja-JP.ini にないキーは英語で表示される (製品の画面と
// 同じ)。どちらにもないキーはキーそのものを返す (libobs の obs_module_text が `out = val` から始めるのと同じ)。
// ini の読み方は libobs/util/text-lookup.c の lookup_addfiledata と同じ規則にする (locale_stub.cpp の冒頭)。

#pragma once

#include <QString>

class QWidget;

namespace cordmux::ui_preview {

// locale_dir の en-US.ini を読み、lang が en-US でなければ lang.ini をその上に重ねる。前に読んだ分は捨てる。
// 読めなかったときと、キーが 1 つも読めなかったときは false を返し、error に理由を入れる (キーがないまま描画すると、
// 画面の文がすべてキーの名前で描画された画像になるため)。
bool load_locale(const QString &locale_dir, const QString &lang, QString *error);

// OBS 自身の ini (obs_frontend_get_locale_string が返すテキスト) を読む。obs_source_dir は OBS のソースのフォルダー
// (frontend/data/locale を持つ) か、OBS をインストールしたフォルダー (data/obs-studio/locale を持つ。--obs-install)。
// en-US.ini を読み、lang が en-US でなければ lang.ini を重ねる。
bool load_frontend_locale(const QString &obs_source_dir, const QString &lang, QString *error);

// obs_frontend_get_main_window が返すメインウィンドウを設定する (プレビューツールが OBS と同じ型の名前で作ったもの)。
// nullptr を渡すと外す。
void set_preview_main_window(QWidget *window);

// obs_frontend_is_theme_dark が返す値を設定する (適用したテーマの meta の dark。OBS の App::IsThemeDark と同じ)。
// 適用していない間は true を返す。
void set_preview_theme_dark(bool dark);

} // namespace cordmux::ui_preview
