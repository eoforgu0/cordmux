# SPDX-License-Identifier: Apache-2.0

# src-obs/dock/ui: ドックと cordmux のダイアログが使うウィジェットと、その計算。
# 表示するテキストは呼び出し元から受け取り、ここのコードは ini のキーを持たない。見た目の値は dock_style.h の
# DockStyle で受け取り、色はテーマから読む。src-obs/dock/dock.cmake がこのファイルを include() し、次の関数を呼ぶ。
#   cordmux_add_dock_ui_observation(target)  Qt に依存しない計算のヘッダー。単体テスト (align_tests) にも入る
#   cordmux_add_dock_ui(target)              Qt を使うウィジェット。cordmux.dll と、テスト用のツール
#                                            (ui_preview、dock_drive) に入る
# テスト用のツールが cordmux_add_dock_ui を呼べるのは、ここの .cpp が libobs から使うのが text_resolve.cpp の
# obs_module_text だけで、src-obs/tests/ui_preview/stub/obs-module.h がそれを持つためである。

# include() は呼び出し元の CMAKE_CURRENT_SOURCE_DIR を変えないので、このファイルのフォルダーを先に変数に保存する。
# ヘッダーは "dock/ui/xxx.h" の形でインクルードする (src-obs がインクルードのルート)。
set(CORDMUX_DOCK_UI_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(cordmux_add_dock_ui_observation target)
  target_sources(
    ${target}
    PRIVATE
      "${CORDMUX_DOCK_UI_DIR}/dock_ui_calc.h"
      "${CORDMUX_DOCK_UI_DIR}/dock_style.h"
      "${CORDMUX_DOCK_UI_DIR}/dock_metrics.h"
      "${CORDMUX_DOCK_UI_DIR}/text_arg.h"
  )
endfunction()

function(cordmux_add_dock_ui target)
  cordmux_add_dock_ui_observation(${target})
  target_sources(
    ${target}
    PRIVATE
      "${CORDMUX_DOCK_UI_DIR}/dock_metrics.cpp"
      "${CORDMUX_DOCK_UI_DIR}/elide_label.h"
      "${CORDMUX_DOCK_UI_DIR}/elide_label.cpp"
      "${CORDMUX_DOCK_UI_DIR}/focus_ring.h"
      "${CORDMUX_DOCK_UI_DIR}/focus_ring.cpp"
      "${CORDMUX_DOCK_UI_DIR}/form_parts.h"
      "${CORDMUX_DOCK_UI_DIR}/form_parts.cpp"
      "${CORDMUX_DOCK_UI_DIR}/level_meter.h"
      "${CORDMUX_DOCK_UI_DIR}/level_meter.cpp"
      "${CORDMUX_DOCK_UI_DIR}/message_dialog.h"
      "${CORDMUX_DOCK_UI_DIR}/message_dialog.cpp"
      "${CORDMUX_DOCK_UI_DIR}/no_wheel.h"
      "${CORDMUX_DOCK_UI_DIR}/open_location.h"
      "${CORDMUX_DOCK_UI_DIR}/open_location.cpp"
      "${CORDMUX_DOCK_UI_DIR}/record_button_base.h"
      "${CORDMUX_DOCK_UI_DIR}/record_button_base.cpp"
      "${CORDMUX_DOCK_UI_DIR}/set_class.h"
      "${CORDMUX_DOCK_UI_DIR}/slider_spin.h"
      "${CORDMUX_DOCK_UI_DIR}/slider_spin.cpp"
      "${CORDMUX_DOCK_UI_DIR}/spin_box_ignore_wheel.h"
      "${CORDMUX_DOCK_UI_DIR}/status_icons.h"
      "${CORDMUX_DOCK_UI_DIR}/status_icons.cpp"
      "${CORDMUX_DOCK_UI_DIR}/surface_frame.h"
      "${CORDMUX_DOCK_UI_DIR}/surface_frame.cpp"
      "${CORDMUX_DOCK_UI_DIR}/text_resolve.h"
      "${CORDMUX_DOCK_UI_DIR}/text_resolve.cpp"
      "${CORDMUX_DOCK_UI_DIR}/theme_read.h"
      "${CORDMUX_DOCK_UI_DIR}/theme_read.cpp"
      "${CORDMUX_DOCK_UI_DIR}/type_scale.h"
      "${CORDMUX_DOCK_UI_DIR}/type_scale.cpp"
      "${CORDMUX_DOCK_UI_DIR}/volume_slider.h"
      "${CORDMUX_DOCK_UI_DIR}/volume_slider.cpp"
      "${CORDMUX_DOCK_UI_DIR}/wrap_label.h"
      "${CORDMUX_DOCK_UI_DIR}/wrap_label.cpp"
  )
endfunction()
