# SPDX-License-Identifier: Apache-2.0

# ドックの参加者の一覧と音声タブのソースを target に追加する関数を定義する。
# dock/dock.cmake がこのファイルを OPTIONAL で include し、関数が定義されているときだけ呼び出す。
#   cordmux_add_rows_observation(target)  Qt を含まないソース (name_check.*)。align_tests にも入る
#   cordmux_add_rows(target)              Qt を含むソースも追加する。cordmux.dll、プレビューツール (ui_preview)、
#                                         ドックを操作するテスト (dock_drive) が呼び出す
# 関数の外でソースを追加しない。追加すると、align_tests に Qt を含むソースが入る。

# include() は呼び出し元の CMAKE_CURRENT_SOURCE_DIR を変えないので、このファイルのフォルダーを先に変数に入れておく。
set(CORDMUX_DOCK_ROWS_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(cordmux_add_rows_observation target)
  target_sources(
    ${target}
    PRIVATE
      "${CORDMUX_DOCK_ROWS_DIR}/name_check.h"
      "${CORDMUX_DOCK_ROWS_DIR}/name_check.cpp"
  )
endfunction()

function(cordmux_add_rows target)
  cordmux_add_rows_observation(${target})
  target_sources(
    ${target}
    PRIVATE
      "${CORDMUX_DOCK_ROWS_DIR}/person_row.h"
      "${CORDMUX_DOCK_ROWS_DIR}/person_row.cpp"
      "${CORDMUX_DOCK_ROWS_DIR}/row_menu.h"
      "${CORDMUX_DOCK_ROWS_DIR}/row_menu.cpp"
      "${CORDMUX_DOCK_ROWS_DIR}/people_list.h"
      "${CORDMUX_DOCK_ROWS_DIR}/people_list.cpp"
      "${CORDMUX_DOCK_ROWS_DIR}/audio_tab.h"
      "${CORDMUX_DOCK_ROWS_DIR}/audio_tab.cpp"
  )
endfunction()
