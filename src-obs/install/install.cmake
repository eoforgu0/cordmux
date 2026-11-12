# SPDX-License-Identifier: Apache-2.0

# install/ (Vesktop の設定の書き換え、更新、アンインストール) を target に追加する関数
# cordmux_add_install(<target>) を定義する。src-obs/CMakeLists.txt と src-obs/tests/CMakeLists.txt の両方がこの
# ファイルを include し、cordmux_add_install を 1 回ずつ呼び出す (ledger/ledger.cmake と同じ形。ファイルの一覧を
# 1 か所にまとめるため)。
# install/vencord_dir.cpp は nlohmann/json.hpp を include するが、このファイルは FetchContent を宣言しない。
# どちらの呼び出し元も先に ws/ws_deps.cmake を (src-obs/CMakeLists.txt は ws/ws.cmake を通して) include し、
# cordmux_add_ws_deps() が同じ target に nlohmann/json の include のディレクトリを追加するためである
# (config/config.cmake と同じ)。

set(CORDMUX_INSTALL_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(cordmux_add_install target)
  target_sources(
    ${target}
    PRIVATE
      "${CORDMUX_INSTALL_DIR}/vencord_dir.h"
      "${CORDMUX_INSTALL_DIR}/vencord_dir.cpp"
      "${CORDMUX_INSTALL_DIR}/backup_first_write.h"
      "${CORDMUX_INSTALL_DIR}/build_mark.h"
      "${CORDMUX_INSTALL_DIR}/build_mark.cpp"
      "${CORDMUX_INSTALL_DIR}/reinstall_plan.h"
      "${CORDMUX_INSTALL_DIR}/reinstall_plan.cpp"
      "${CORDMUX_INSTALL_DIR}/removal_table.h"
      "${CORDMUX_INSTALL_DIR}/uninstall.h"
      "${CORDMUX_INSTALL_DIR}/uninstall.cpp"
  )

  # 実際の関数をまとめるファイル (make_real_uninstall_ops) は OBS の frontend API と sources/ を使うので、製品に
  # だけ追加する。製品かどうかは、OBS::obs-frontend-api の target があるかで見分ける。src-obs/CMakeLists.txt は
  # この include より前に find_package(obs-frontend-api) を行い、src-obs/tests/CMakeLists.txt は行わない。
  # src-obs/CMakeLists.txt に行を追加せず、関数の名前と呼び出し元も変えずに済ませるためである。
  if(TARGET OBS::obs-frontend-api)
    target_sources(${target} PRIVATE "${CORDMUX_INSTALL_DIR}/uninstall_real.cpp")
  endif()
endfunction()
