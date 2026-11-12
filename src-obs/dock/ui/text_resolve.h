// SPDX-License-Identifier: Apache-2.0

// ini のテキストを読み、差し込む値を入れる (resolve)。ドック、参加者の一覧、設定ウィンドウ、インストールの
// ダイアログ、プレビューツールが使う。libobs から使うのは obs_module_text だけである
// (tests/ui_preview/stub/obs-module.h が同じ関数を持つ)。値の種類の判定と書式の計算は、Qt に依存しない
// text_arg.h にある。

#pragma once

#include <QString>

#include <cstdint>
#include <string>

#include "ledger/view.h"

namespace cordmux::dock::ui {

// ini の値を読み (obs_module_text)、%1、%2 ... に args を差し込む。左から右へ 1 回だけ走査するので、差し込んだ値の
// 中の %2 をもう一度置き換えない。範囲の外の番号と、% の後が数字でないものはそのまま残す (ini が壊れていても
// クラッシュしない)。ないキーは、キーをそのまま返す (libobs の obs_module_text と同じ)。
// 差し込む値が日時のマーカーの付いた値 (ledger/dock_view.h の time_arg、date_time_arg) なら、
// Windows の地域の設定 (QLocale::system()) の短い形にする。
//   - T は時と分だけ、D は日付と時刻 (QLocale の短い日時の形のまま。日付と時刻をここで連結しない)、Y は日付だけ。
//     今日でない日に日付を添えるかは、ledger の側が決める (D を選ぶ)。
//   - 秒は表示しない。画面の言語が日本語なら、午前と午後の表示を除いて 24 時間制にする。ほかの言語は地域の設定の
//     まま。画面の言語は QLocale() で読む (OBS は起動するときに、画面の言語を QLocale::setDefault に設定する。
//     OBS 31.1.1 の frontend/OBSApp.cpp)。
//   - Y の値はその日の協定世界時の正午なので、日付は協定世界時で読む (時差が +13 と +14 の地域でも同じ日になる)。
// マーカーが壊れていたら、マーカーと種類の文字を除いた残りを表示する。どの値からも制御文字を除く。
QString resolve(const ledger::TextRef &ref);
// label が ini のテキストなら resolve と同じく読み、Discord から来た文字列なら翻訳せずにそのまま返す (制御文字は
// 除く)。
QString resolve_label(const ledger::Label &label);
// キー 1 つの値を読む (差し込みなし)。
QString resolve_key(const char *key);

// 差し込む値 1 つを画面のテキストにする (日時のマーカーを書式に従って変え、制御文字を除く)。resolve が中で使う。
// テストのために公開している。
QString format_arg(const std::string &arg);

// 日時のマーカーの値を書式に従って変える (us は Unix 時刻のマイクロ秒)。with_date が true なら、日付も表示する
// (QLocale の短い日時の形)。
QString format_time_us(std::int64_t us, bool with_date);

} // namespace cordmux::dock::ui
