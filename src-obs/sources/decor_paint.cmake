# SPDX-License-Identifier: Apache-2.0

# 名前ラベルの描画 (decor_paint.cpp と composite_label.cpp と composite_draw_cpu.cpp) をターゲットに追加し、
# Qt6::Gui をリンクする。Qt の追加の仕方を 2 か所に書かないように、src-obs/CMakeLists.txt (sources/sources.cmake
# から) と src-obs/tests/CMakeLists.txt (decor_tests) の両方がこのファイルを include し、
# cordmux_add_decor_paint(<target>) を 1 回だけ呼ぶ。include() は呼び出し元の CMAKE_CURRENT_SOURCE_DIR を変えない
# ので、このファイルのフォルダーを先に変数に入れておく (decode/decode.cmake と同じ形)。

set(CORDMUX_DECOR_PAINT_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Qt6 の探し先。src-obs/CMakeLists.txt は buildspec が用意した CMAKE_PREFIX_PATH で既に find_package しており、
# Qt6::Gui は Qt6::Widgets の依存として定義済みである。ここで探すのは、テストから呼ばれたときだけである。
# decode/decode.cmake と同じ理由で、obs-studio のサブビルドの .deps を先に見る (トップレベルの .deps は、
# 初回のダウンロードに失敗すると空になることがある)。.deps の中のフォルダーのバージョンは、
# cmake/cordmux_obs_deps.cmake が src-obs/buildspec.json から決める。
include("${CORDMUX_DECOR_PAINT_DIR}/../cmake/cordmux_obs_deps.cmake")
set(CORDMUX_DECOR_QT6_HINTS ${CORDMUX_OBS_DEPS_QT6_HINTS})

function(cordmux_add_decor_paint target)
  if(NOT TARGET Qt6::Gui)
    find_package(Qt6 COMPONENTS Gui REQUIRED PATHS ${CORDMUX_DECOR_QT6_HINTS} NO_DEFAULT_PATH)
  endif()

  target_sources(
    ${target}
    PRIVATE "${CORDMUX_DECOR_PAINT_DIR}/decor_paint.h" "${CORDMUX_DECOR_PAINT_DIR}/decor_paint.cpp"
            "${CORDMUX_DECOR_PAINT_DIR}/decor_paint_common.h"
            "${CORDMUX_DECOR_PAINT_DIR}/composite_label.h" "${CORDMUX_DECOR_PAINT_DIR}/composite_label.cpp"
            # 合成のソースの置く物の計画を CPU で重ねる関数 (テストとプレビューツールが使う)。
            "${CORDMUX_DECOR_PAINT_DIR}/composite_draw_cpu.h" "${CORDMUX_DECOR_PAINT_DIR}/composite_draw_cpu.cpp"
            # 描画が読む純粋関数のヘッダー (光のタイミングの式、ダミーの色、合成のソースの名前ラベルの
            # 見た目の値、グローのぼかし、置く物の計画。Qt に依存しない)。
            "${CORDMUX_DECOR_PAINT_DIR}/label_border.h"
            "${CORDMUX_DECOR_PAINT_DIR}/dummy_tiles.h" "${CORDMUX_DECOR_PAINT_DIR}/composite_label_look.h"
            "${CORDMUX_DECOR_PAINT_DIR}/glow_filter.h" "${CORDMUX_DECOR_PAINT_DIR}/composite_draw_plan.h"
  )
  target_link_libraries(${target} PRIVATE Qt6::Gui)
endfunction()

# decor_tests がフォントとスナップショットを探すフォルダー (tests/CMakeLists.txt が target_compile_definitions で
# 渡す)。フォルダーを決めるのは、このファイルの 1 か所だけにする。
set(CORDMUX_DECOR_FONT_DIR "${CORDMUX_DECOR_PAINT_DIR}/../tests/fonts")
set(CORDMUX_DECOR_SNAPSHOT_DIR "${CORDMUX_DECOR_PAINT_DIR}/../tests/snapshots")
