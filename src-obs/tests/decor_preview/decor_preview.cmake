# SPDX-License-Identifier: Apache-2.0

# 名前ラベルのプレビューツール (decor_preview.exe) のターゲットを cordmux_add_decor_preview() で追加する。製品の
# 名前ラベルの描画 (src-obs/sources/composite_label.cpp と composite_draw_cpu.cpp) と、ダミーの色のタイル
# (src-obs/sources/dummy_tiles.h) をそのまま組み込む。名前ラベルの描画は、製品と decor_tests と同じく
# src-obs/sources/decor_paint.cmake の cordmux_add_decor_paint で追加する。libobs は使わない。
# src-obs/tests/CMakeLists.txt から、decor_paint.cmake と ws_deps.cmake の後に include する。合否を持たないので、
# add_test には追加しない。使い方は src-obs/tests/decor_preview/README.md にある。

set(CORDMUX_DECOR_PREVIEW_DIR "${CMAKE_CURRENT_LIST_DIR}")
include("${CORDMUX_DECOR_PREVIEW_DIR}/../../sources/decor_paint.cmake")

function(cordmux_add_decor_preview)
  find_package(Qt6 COMPONENTS Core Gui REQUIRED PATHS ${CORDMUX_DECOR_QT6_HINTS} NO_DEFAULT_PATH)

  add_executable(
    decor_preview
    "${CORDMUX_DECOR_PREVIEW_DIR}/main.cpp"
    "${CORDMUX_DECOR_PREVIEW_DIR}/composite_check.h"
    "${CORDMUX_DECOR_PREVIEW_DIR}/composite_check.cpp"
    "${CORDMUX_DECOR_PREVIEW_DIR}/composite_sample.h"
    "${CORDMUX_DECOR_PREVIEW_DIR}/composite_sample.cpp"
    "${CORDMUX_DECOR_PREVIEW_DIR}/composite_scene.h"
    "${CORDMUX_DECOR_PREVIEW_DIR}/composite_scene.cpp"
    "${CORDMUX_DECOR_PREVIEW_DIR}/decor_qpa.h"
    "${CORDMUX_DECOR_PREVIEW_DIR}/decor_qpa.cpp"
    "${CORDMUX_DECOR_PREVIEW_DIR}/decor_qpa.json"
  )

  # 製品の名前ラベルの描画と Qt6::Gui を追加する (製品と decor_tests と同じ関数)。
  cordmux_add_decor_paint(decor_preview)

  # decor_qpa.cpp のプラグインは Q_OBJECT と Q_PLUGIN_METADATA を持つので、moc が必要。
  set_target_properties(decor_preview PROPERTIES AUTOMOC ON)

  # src-obs/config/settings.h が読む nlohmann/json のヘッダーの場所 (cordmux_json_SOURCE_DIR) は、
  # src-obs/ws/ws_deps.cmake が決める。
  target_include_directories(
    decor_preview
    PRIVATE "${CORDMUX_DECOR_PREVIEW_DIR}/../.." "${CORDMUX_DECOR_PREVIEW_DIR}" "${cordmux_json_SOURCE_DIR}/include"
  )
  # QT_NO_KEYWORDS は製品と同じ (Qt の slots などの小文字のキーワードをマクロにしない)。
  target_compile_definitions(decor_preview PRIVATE QT_NO_KEYWORDS)

  # Qt6::GuiPrivate は decor_qpa.cpp のプラットフォームプラグインが使う (QPA の型と、OBS と同じ DirectWrite 版の
  # フォントデータベース QWindowsDirectWriteFontDatabase)。
  target_link_libraries(decor_preview PRIVATE Qt6::Core Qt6::Gui Qt6::GuiPrivate)

  if(MSVC)
    target_compile_options(decor_preview PRIVATE /W4 /utf-8 /permissive-)
  else()
    target_compile_options(decor_preview PRIVATE -Wall -Wextra)
  endif()

  # 実行に必要な DLL と、比べるためのプラットフォームプラグインをコピーする。minimal は --backend minimal、windows は
  # --backend windows (OBS の中と同じ文字の描き方。test/tools/composite-check の参照が使う) のときだけ使う。
  # decor_tests と label_tuner のコピーはそれぞれをビルドしたときにしか実行されないので、このツールだけをビルドした
  # ときのためにここでもコピーする。
  add_custom_command(
    TARGET decor_preview
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::Core>" "$<TARGET_FILE:Qt6::Gui>"
            "$<TARGET_FILE_DIR:decor_preview>"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:decor_preview>/platforms"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::QMinimalIntegrationPlugin>"
            "$<TARGET_FILE:Qt6::QWindowsIntegrationPlugin>" "$<TARGET_FILE_DIR:decor_preview>/platforms"
  )
endfunction()
