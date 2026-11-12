# SPDX-License-Identifier: Apache-2.0

# インストールとアンインストールのダイアログのソースを、ターゲットに追加する。dock/dock.cmake がこのファイルを
# include(... OPTIONAL) し、次の 2 つの関数を、`if(COMMAND ...)` で確認してから呼ぶ。
#   cordmux_add_install_ui_observation(target)  Qt を使わない側。ダイアログの中身の並び (install_dialog_plan.*)。
#                                               cordmux_add_dock_observation の中から呼ばれ、align_tests にも入る。
#   cordmux_add_install_ui(target)              Qt の側。並びを ui::MessageSpec にする (install_dialogs.*)。
# プレビューツール (ui_preview) も cordmux_add_install_ui を呼んでよい。OBS から使うのは obs_module_text と
# obs_frontend_get_locale_string だけで、プレビューツールの stub が持っている。install_flow.* は dock.cmake の
# cordmux_add_dock_observation が、dock/ui は cordmux_add_dock_ui が追加する。

# include() は呼び出し元の CMAKE_CURRENT_SOURCE_DIR を変えないので、このファイルのディレクトリを先に変数に入れておく
# (dock.cmake と同じ)。
set(CORDMUX_INSTALL_UI_DIR "${CMAKE_CURRENT_LIST_DIR}")

# 同梱の実行時のファイルの名前。展開したフォルダーが決まらない置き方でアンインストールが終わったときのダイアログが、「削除するファイル:」
# の箇条書きに表示する。一覧は、zip の cordmux フォルダーの直下に入れるファイルと同じ cmake/cordmux_ffmpeg_runtime.cmake の 1 か所から
# 取り、製品のソースが読めるように C++ のヘッダーに書き出す (cordmux_add_install_ui の中)。cordmux.dll の名前は一覧に
# ない (製品が実行中に obs_get_module_file_name で取得する)。一覧のファイルは変数と関数を定義するだけなので、ルートの
# CMakeLists.txt や tests の CMakeLists.txt が後でもう一度 include しても変わらない。
# 名前を C++ のソースに直接書かないのは、一覧が 2 か所になり、同梱のファイルを追加したときに、ダイアログだけが古いまま
# 残るためである。コンパイラの定義に並べないのは、名前の区切りの ; が CMake のリストの区切りと重なるためである。
include("${CORDMUX_INSTALL_UI_DIR}/../cmake/cordmux_ffmpeg_runtime.cmake")
set(
  CORDMUX_INSTALL_UI_RUNTIME_NAMES
  ${CORDMUX_FFMPEG_RUNTIME_LINKED}
  ${CORDMUX_FFMPEG_RUNTIME_EXE_ONLY}
  ${CORDMUX_FFMPEG_RUNTIME_EXE}
)

function(cordmux_add_install_ui_observation target)
  target_sources(
    ${target}
    PRIVATE
      "${CORDMUX_INSTALL_UI_DIR}/install_dialog_plan.h"
      "${CORDMUX_INSTALL_UI_DIR}/install_dialog_plan.cpp"
  )
endfunction()

function(cordmux_add_install_ui target)
  cordmux_add_install_ui_observation(${target})
  target_sources(
    ${target}
    PRIVATE
      "${CORDMUX_INSTALL_UI_DIR}/install_dialogs.h"
      "${CORDMUX_INSTALL_UI_DIR}/install_dialogs.cpp"
  )

  # 同梱の実行時のファイルの名前のヘッダー (上の CORDMUX_INSTALL_UI_RUNTIME_NAMES)。中身が変わったときだけ書き直す
  # (file(CONFIGURE))。読むのは製品の dock_poll_install.cpp と、プレビューツールの faces.cpp (手順書と違う置き方で
  # 終わったときのダイアログの描画) で、ドックを操作するテストには include のフォルダーが追加されるだけである
  # (使わない)。
  set(_entries "")
  foreach(_name IN LISTS CORDMUX_INSTALL_UI_RUNTIME_NAMES)
    string(APPEND _entries "  \"${_name}\",\n")
  endforeach()
  set(_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/cordmux_install_ui_generated")
  file(
    CONFIGURE
    OUTPUT "${_generated_dir}/dock/install_runtime_files.h"
    CONTENT
      "// 生成物 (src-obs/dock/install_ui.cmake が cmake/cordmux_ffmpeg_runtime.cmake の一覧から書く)。手で修正しない。\n#pragma once\n\nnamespace cordmux::dock::generated {\n\n// 同梱の実行時のファイルの名前 (cordmux.dll と並べる物。一覧の順)。\ninline constexpr const char *kBundledRuntimeFiles[] = {\n@_entries@};\n\n} // namespace cordmux::dock::generated\n"
    @ONLY
    NEWLINE_STYLE LF
  )
  target_include_directories(${target} PRIVATE "${_generated_dir}")
endfunction()
