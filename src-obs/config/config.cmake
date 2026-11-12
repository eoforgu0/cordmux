# SPDX-License-Identifier: Apache-2.0

# src-obs/config のヘッダーを製品のターゲットに追加する。src-obs/CMakeLists.txt が include(config/config.cmake) で
# 読み込む。target_sources は対象のターゲットが既に存在している必要があるので、
# add_library(${CMAKE_PROJECT_NAME} MODULE) より後に置くこと。config/ はヘッダーだけで実装しているので、追加する
# のは .h だけである (コンパイルの単位は増えない。IDE のツリーに表示するために並べる)。

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/allocation.h
    ${CMAKE_CURRENT_LIST_DIR}/label_placement.h
    ${CMAKE_CURRENT_LIST_DIR}/person_audio.h
    ${CMAKE_CURRENT_LIST_DIR}/settings.h
    ${CMAKE_CURRENT_LIST_DIR}/stage_counters.h
)
