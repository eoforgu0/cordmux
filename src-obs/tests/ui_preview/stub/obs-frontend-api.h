// SPDX-License-Identifier: Apache-2.0

// プレビューツール (ui_preview) とドックを操作するテスト (dock_drive) が、OBS の obs-frontend-api.h の代わりに読む
// ヘッダー。製品のドックと cordmux のウィンドウ (dock/ の Qt の部分) が OBS のフロントエンドから使う関数のうち、
// ウィンドウを表示せずに描画するのに必要な 3 つだけを宣言する。宣言の形は OBS 31.1.1 の
// frontend/api/obs-frontend-api.h と同じ (C のリンケージ) で、実装は tests/ui_preview/locale_stub.cpp にある。
// ここにない関数を dock/ の Qt の部分が使うと、プレビューツールはビルドできない (dock/dock.cmake の
// cordmux_add_dock_face)。

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// OBS 自身の ini のテキスト ([OK] [キャンセル] [適用] [閉じる]。ledger/text_ids.h の obs_keys)。OBS の
// frontend/data/locale の en-US.ini を読んだ上に、選んだ言語の ini を重ねる (OBS の App::InitLocale と同じ順)。
// 見つからなければ、渡された文字列をそのまま返す。
const char *obs_frontend_get_locale_string(const char *string);
// OBS のメインウィンドウ。プレビューツールが OBS と同じ型の名前で作ったメインウィンドウ (テーマが desktopCapIcon を
// 設定する) を返す。作っていなければ nullptr。
void *obs_frontend_get_main_window(void);
// OBS のテーマが暗いか。プレビューツールが適用したテーマの meta の dark (OBS の App::IsThemeDark と同じ値) を返す。
// 適用していなければ true。
bool obs_frontend_is_theme_dark(void);

#ifdef __cplusplus
}
#endif
