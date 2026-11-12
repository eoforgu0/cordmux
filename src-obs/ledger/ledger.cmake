# SPDX-License-Identifier: Apache-2.0

# 表示の規則 (src-obs/ledger) のソースをターゲットに追加する関数 cordmux_add_ledger(<target>) を定義する。
# src-obs/CMakeLists.txt と src-obs/tests/CMakeLists.txt の両方がこのファイルを include して 1 回ずつ呼び出すので、
# ファイルの一覧はここにだけ書く。ヘッダーは IDE のツリーに表示するために並べる。
# 表示の規則は libobs にも Qt にも依存しないので、テストの実行ファイルにもそのまま追加できる。

# 関数の中で CMAKE_CURRENT_LIST_DIR を読むと、呼び出した側のファイルのフォルダーになる。そのため、include された
# ときにこのファイルのフォルダーを変数に保存しておく。
set(CORDMUX_LEDGER_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(cordmux_add_ledger target)
  target_sources(
    ${target}
    PRIVATE
      "${CORDMUX_LEDGER_DIR}/observation.h"
      "${CORDMUX_LEDGER_DIR}/view.h"
      "${CORDMUX_LEDGER_DIR}/text_ids.h"
      "${CORDMUX_LEDGER_DIR}/rules.h"
      "${CORDMUX_LEDGER_DIR}/rules.cpp"
      "${CORDMUX_LEDGER_DIR}/actions.h"
      "${CORDMUX_LEDGER_DIR}/actions.cpp"
      "${CORDMUX_LEDGER_DIR}/dock_view.h"
      "${CORDMUX_LEDGER_DIR}/plural.h"
      "${CORDMUX_LEDGER_DIR}/rules_dock.cpp"
      "${CORDMUX_LEDGER_DIR}/notice_tracker.h"
      "${CORDMUX_LEDGER_DIR}/notice_tracker.cpp"
      "${CORDMUX_LEDGER_DIR}/audio_tab.h"
      "${CORDMUX_LEDGER_DIR}/audio_tab.cpp"
      "${CORDMUX_LEDGER_DIR}/look_tab.h"
      "${CORDMUX_LEDGER_DIR}/look_tab.cpp"
      "${CORDMUX_LEDGER_DIR}/status_tab.h"
      "${CORDMUX_LEDGER_DIR}/status_tab.cpp"
      "${CORDMUX_LEDGER_DIR}/natural_order.h"
      "${CORDMUX_LEDGER_DIR}/slot_cells.h"
      "${CORDMUX_LEDGER_DIR}/inline_confirm.h"
      "${CORDMUX_LEDGER_DIR}/stream_record.h"
  )
endfunction()
