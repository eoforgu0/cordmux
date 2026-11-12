# SPDX-License-Identifier: Apache-2.0

# 設定ウィンドウと参加者の設定ウィンドウのソースを target に追加する関数を定義する。
# dock/dock.cmake がこのファイルを OPTIONAL で include し、関数が定義されているときだけ呼び出す。
#   cordmux_add_windows_observation(target)  Qt を含まないソース (windows_calc.h)。align_tests にも入る
#   cordmux_add_windows(target)              ウィンドウのソースも追加する。cordmux.dll、プレビューツール (ui_preview)、
#                                            ドックを操作するテスト (dock_drive) が呼び出す
# ウィンドウのソースが libobs から使うのは obs_module_text と obs_frontend_get_locale_string だけである。libobs を
# リンクしないプレビューツールとドックを操作するテストは、src-obs/tests/ui_preview/stub の代わりの関数を使う。
# libobs をほかにも使う dock_poll_settings.cpp と dock_poll_export.cpp は、dock.cmake の cordmux_add_dock が追加する。

# include() は呼び出し元の CMAKE_CURRENT_SOURCE_DIR を変えないので、このファイルのフォルダーを先に変数に入れておく。
set(CORDMUX_DOCK_WINDOWS_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(cordmux_add_windows_observation target)
  target_sources(
    ${target}
    PRIVATE
      "${CORDMUX_DOCK_WINDOWS_DIR}/windows_calc.h"
  )
endfunction()

function(cordmux_add_windows target)
  cordmux_add_windows_observation(${target})
  # settings_fields.* は Qt を含まず、dock.cmake の cordmux_add_dock_observation も追加する。プレビューツールと
  # ドックを操作するテストは cordmux_add_dock_observation を呼び出さないので、ここでも追加する。同じ target に 2 回
  # 追加しても 1 回だけ入る。
  target_sources(
    ${target}
    PRIVATE
      "${CORDMUX_DOCK_WINDOWS_DIR}/settings_fields.h"
      "${CORDMUX_DOCK_WINDOWS_DIR}/settings_fields.cpp"
      "${CORDMUX_DOCK_WINDOWS_DIR}/settings_window.h"
      "${CORDMUX_DOCK_WINDOWS_DIR}/settings_window.cpp"
      "${CORDMUX_DOCK_WINDOWS_DIR}/participants_window.h"
      "${CORDMUX_DOCK_WINDOWS_DIR}/participants_window.cpp"
  )
endfunction()
