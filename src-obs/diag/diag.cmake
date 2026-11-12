# SPDX-License-Identifier: Apache-2.0

# ログの出力先 (src-obs/diag) のソースをターゲットに追加する関数 cordmux_add_diag(<target>) を定義する。
# src-obs/CMakeLists.txt、src-obs/tests/CMakeLists.txt、src-obs/tests/dock_drive/dock_drive.cmake がこのファイルを
# include して 1 回ずつ呼び出すので、ファイルの一覧はここにだけ書く。libobs にも Qt にも依存しないので、
# テストの実行ファイルにもそのまま追加できる。リンクするライブラリはない。

# 関数の中で CMAKE_CURRENT_LIST_DIR を読むと、呼び出した側のファイルのフォルダーになる。そのため、include された
# ときにこのファイルのフォルダーを変数に保存しておく。
set(CORDMUX_DIAG_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(cordmux_add_diag target)
  target_sources(
    ${target}
    PRIVATE
      "${CORDMUX_DIAG_DIR}/diag_log.h"
      "${CORDMUX_DIAG_DIR}/diag_log.cpp"
  )
endfunction()
