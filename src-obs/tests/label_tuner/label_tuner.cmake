# SPDX-License-Identifier: Apache-2.0

# 名前ラベルの調整ツール (label_tuner.exe) のターゲットを cordmux_add_label_tuner() で追加する。合成のソースの名前ラベルと
# 光を、製品の描画 (src-obs/sources/composite_label.cpp)、置く物の計画と CPU で重ねる関数 (composite_draw_plan.h と
# composite_draw_cpu.cpp) で描く。場面を作る部分 (composite_scene.cpp) と、ウィンドウを開かないプラットフォームプラグイン
# (decor_qpa.cpp) は、名前ラベルのプレビューツール (src-obs/tests/decor_preview) のファイルをそのまま使う。
# src-obs/tests/CMakeLists.txt から、decor_paint.cmake と ws_deps.cmake の後に include する。合否を持たないので、
# add_test には追加しない。使い方は src-obs/tests/label_tuner/README.md にある。

set(CORDMUX_LABEL_TUNER_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(CORDMUX_LABEL_TUNER_DECOR_PREVIEW_DIR "${CMAKE_CURRENT_LIST_DIR}/../decor_preview")
include("${CORDMUX_LABEL_TUNER_DIR}/../../sources/decor_paint.cmake")

function(cordmux_add_label_tuner)
  find_package(Qt6 COMPONENTS Core Gui Widgets REQUIRED PATHS ${CORDMUX_DECOR_QT6_HINTS} NO_DEFAULT_PATH)

  add_executable(
    label_tuner
    "${CORDMUX_LABEL_TUNER_DIR}/main.cpp"
    "${CORDMUX_LABEL_TUNER_DIR}/preview_views.h"
    "${CORDMUX_LABEL_TUNER_DIR}/preview_views.cpp"
    "${CORDMUX_LABEL_TUNER_DIR}/speak_track.h"
    "${CORDMUX_LABEL_TUNER_DIR}/tuner_compositor.h"
    "${CORDMUX_LABEL_TUNER_DIR}/tuner_compositor.cpp"
    "${CORDMUX_LABEL_TUNER_DIR}/tuner_drive.h"
    "${CORDMUX_LABEL_TUNER_DIR}/tuner_drive.cpp"
    "${CORDMUX_LABEL_TUNER_DIR}/tuner_knobs.h"
    "${CORDMUX_LABEL_TUNER_DIR}/tuner_knobs.cpp"
    "${CORDMUX_LABEL_TUNER_DIR}/tuner_renderer.h"
    "${CORDMUX_LABEL_TUNER_DIR}/tuner_renderer.cpp"
    "${CORDMUX_LABEL_TUNER_DIR}/tuner_state.h"
    "${CORDMUX_LABEL_TUNER_DIR}/tuner_state.cpp"
    "${CORDMUX_LABEL_TUNER_DIR}/tuner_sweep.h"
    "${CORDMUX_LABEL_TUNER_DIR}/tuner_sweep.cpp"
    "${CORDMUX_LABEL_TUNER_DIR}/tuner_window.h"
    "${CORDMUX_LABEL_TUNER_DIR}/tuner_window.cpp"
    "${CORDMUX_LABEL_TUNER_DECOR_PREVIEW_DIR}/composite_scene.h"
    "${CORDMUX_LABEL_TUNER_DECOR_PREVIEW_DIR}/composite_scene.cpp"
    "${CORDMUX_LABEL_TUNER_DECOR_PREVIEW_DIR}/decor_qpa.h"
    "${CORDMUX_LABEL_TUNER_DECOR_PREVIEW_DIR}/decor_qpa.cpp"
    "${CORDMUX_LABEL_TUNER_DECOR_PREVIEW_DIR}/decor_qpa.json"
  )

  # 製品の名前ラベルの描画と Qt6::Gui を追加する (製品と decor_tests と同じ関数)。
  cordmux_add_decor_paint(label_tuner)

  # decor_qpa.cpp のプラグインは Q_OBJECT と Q_PLUGIN_METADATA を持つので、moc が必要。
  set_target_properties(label_tuner PROPERTIES AUTOMOC ON)

  # src-obs/config/settings.h が読む nlohmann/json のヘッダーの場所 (cordmux_json_SOURCE_DIR) は、
  # src-obs/ws/ws_deps.cmake が決める。
  target_include_directories(
    label_tuner
    PRIVATE "${CORDMUX_LABEL_TUNER_DIR}/../.." "${CORDMUX_LABEL_TUNER_DIR}" "${CORDMUX_LABEL_TUNER_DECOR_PREVIEW_DIR}"
            "${cordmux_json_SOURCE_DIR}/include"
  )
  # QT_NO_KEYWORDS は製品と同じ (Qt の slots などの小文字のキーワードをマクロにしない)。
  target_compile_definitions(label_tuner PRIVATE QT_NO_KEYWORDS)

  # Qt6::GuiPrivate は decor_qpa.cpp のプラットフォームプラグインが使う (QPA の型と、OBS と同じ DirectWrite 版の
  # フォントデータベース)。
  target_link_libraries(label_tuner PRIVATE Qt6::Core Qt6::Gui Qt6::GuiPrivate Qt6::Widgets)

  if(MSVC)
    target_compile_options(label_tuner PRIVATE /W4 /utf-8 /permissive-)
  else()
    target_compile_options(label_tuner PRIVATE -Wall -Wextra)
  endif()

  # 実行に必要な DLL と Qt のプラグインを、実行ファイルと同じフォルダーにコピーする (ないと起動できない)。
  #   platforms/qwindows    ウィンドウを開くときのプラットフォームプラグイン (--shot は実行ファイルの中の
  #                         cordmuxdecorpreview を使う)
  #   imageformats/qjpeg    背景に開く JPEG の画像 (PNG と BMP は Qt6Gui が読む)
  add_custom_command(
    TARGET label_tuner
    POST_BUILD
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::Core>" "$<TARGET_FILE:Qt6::Gui>"
      "$<TARGET_FILE:Qt6::Widgets>" "$<TARGET_FILE_DIR:label_tuner>"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:label_tuner>/platforms"
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::QWindowsIntegrationPlugin>"
      "$<TARGET_FILE_DIR:label_tuner>/platforms"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:label_tuner>/imageformats"
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::QJpegPlugin>"
      "$<TARGET_FILE_DIR:label_tuner>/imageformats"
  )
endfunction()
