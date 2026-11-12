// SPDX-License-Identifier: Apache-2.0

// プレビューツール (ui_preview) とドックを操作するテスト (dock_drive) が、libobs の obs-module.h の代わりに読む
// ヘッダー。プレビューツールに入る製品のソース (dock/dock.cmake の cordmux_add_dock_face ほか) が libobs から使って
// よいのは obs_module_text() だけなので、その宣言だけを置く。宣言の形は libobs の obs-module.h と同じ (C の
// リンケージ。MODULE_EXTERN)。実装は tests/ui_preview/locale_stub.cpp にあり、data/locale の ini を
// OBS と同じ規則で読む。

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

const char *obs_module_text(const char *lookup_string);

#ifdef __cplusplus
}
#endif
