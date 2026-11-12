# SPDX-License-Identifier: Apache-2.0

# ドックを操作するテスト (dock_drive.exe) をビルドする関数 cordmux_add_dock_drive。CORDMUX_BUILD_PREVIEW_TOOLS が
# ON のときだけビルドするので、src-obs/tests/CMakeLists.txt のその節から include する。プレビューツールと同じく
# Qt6::GuiPrivate を使い、CI の Visual Studio 2022 でビルドできるかを確認していないためである。1 回 10 分の実行を
# 含むので、add_test には加えない。同じものを 2 か所に書かないように、プレビューツール (src-obs/tests/ui_preview) の
# テーマの適用、プラットフォームプラグイン、ini の読み込み、テストデータを、そのまま使う (書き換えない)。

set(CORDMUX_DOCK_DRIVE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(CORDMUX_DOCK_DRIVE_PREVIEW_DIR "${CMAKE_CURRENT_LIST_DIR}/../ui_preview")
include("${CORDMUX_DOCK_DRIVE_DIR}/../../cmake/cordmux_obs_deps.cmake")
include("${CORDMUX_DOCK_DRIVE_DIR}/../../diag/diag.cmake")

# プレビューツールに入力を流す機能を加える方法はとらない。プレビューツールは合否を持たず、画像を描くだけの実行に
# 10 分の実行が混ざると重くなるためである。
function(cordmux_add_dock_drive)
  find_package(Qt6 COMPONENTS Core Gui Widgets Svg REQUIRED PATHS ${CORDMUX_OBS_DEPS_QT6_HINTS} NO_DEFAULT_PATH)

  add_executable(
    dock_drive
    "${CORDMUX_DOCK_DRIVE_DIR}/main.cpp"
    "${CORDMUX_DOCK_DRIVE_DIR}/drive_env.h"
    "${CORDMUX_DOCK_DRIVE_DIR}/drive_env.cpp"
    "${CORDMUX_DOCK_DRIVE_DIR}/input.h"
    "${CORDMUX_DOCK_DRIVE_DIR}/input.cpp"
    "${CORDMUX_DOCK_DRIVE_DIR}/sim_poll.h"
    "${CORDMUX_DOCK_DRIVE_DIR}/sim_poll.cpp"
    "${CORDMUX_DOCK_DRIVE_DIR}/query.h"
    "${CORDMUX_DOCK_DRIVE_DIR}/query.cpp"
    "${CORDMUX_DOCK_DRIVE_DIR}/script.h"
    "${CORDMUX_DOCK_DRIVE_DIR}/probe.h"
    "${CORDMUX_DOCK_DRIVE_DIR}/probe.cpp"
    "${CORDMUX_DOCK_DRIVE_DIR}/mistap.h"
    "${CORDMUX_DOCK_DRIVE_DIR}/mistap.cpp"
    "${CORDMUX_DOCK_DRIVE_DIR}/scenarios.h"
    "${CORDMUX_DOCK_DRIVE_DIR}/scen_common.h"
    "${CORDMUX_DOCK_DRIVE_DIR}/scen_common.cpp"
    "${CORDMUX_DOCK_DRIVE_DIR}/owner_hold_probe.h"
    "${CORDMUX_DOCK_DRIVE_DIR}/owner_hold_probe.cpp"
    "${CORDMUX_DOCK_DRIVE_DIR}/scen_keyboard.cpp"
    "${CORDMUX_DOCK_DRIVE_DIR}/scen_windows.cpp"
    "${CORDMUX_DOCK_DRIVE_DIR}/scen_misc.cpp"
    "${CORDMUX_DOCK_DRIVE_DIR}/scen_look.cpp"
    "${CORDMUX_DOCK_DRIVE_DIR}/scen_status.cpp"
    "${CORDMUX_DOCK_DRIVE_DIR}/scen_positions.cpp"
    "${CORDMUX_DOCK_DRIVE_DIR}/scen_layout.cpp"
    "${CORDMUX_DOCK_DRIVE_DIR}/scen_framework.cpp"
    "${CORDMUX_DOCK_DRIVE_DIR}/scen_dialogs.cpp"
    "${CORDMUX_DOCK_DRIVE_DIR}/scen_glance.cpp"
    "${CORDMUX_DOCK_DRIVE_DIR}/scen_row_audio.cpp"
    "${CORDMUX_DOCK_DRIVE_DIR}/scen_audio.cpp"
    "${CORDMUX_DOCK_DRIVE_DIR}/scen_self_frame.cpp"
    "${CORDMUX_DOCK_DRIVE_DIR}/report.h"
    "${CORDMUX_DOCK_DRIVE_DIR}/report.cpp"
    "${CORDMUX_DOCK_DRIVE_DIR}/crash_trace.h"
    "${CORDMUX_DOCK_DRIVE_DIR}/crash_trace.cpp"
    # プレビューツールのファイル (読むだけで、書き換えない)。preview_platform.* は画面を開かない
    # プラットフォームプラグイン、obs_theme.* と render.* は OBS と同じ手順でテーマを適用する関数、locale_stub.* は
    # obs_module_text と obs_frontend_get_locale_string の代わり、fixtures.* はテストデータの Observation、obs_dock.h と
    # obs_names.h は OBS と同じ型名のウィジェット、stub/ は libobs の関数の宣言。
    "${CORDMUX_DOCK_DRIVE_PREVIEW_DIR}/fixtures.h"
    "${CORDMUX_DOCK_DRIVE_PREVIEW_DIR}/fixtures.cpp"
    "${CORDMUX_DOCK_DRIVE_PREVIEW_DIR}/locale_stub.h"
    "${CORDMUX_DOCK_DRIVE_PREVIEW_DIR}/locale_stub.cpp"
    "${CORDMUX_DOCK_DRIVE_PREVIEW_DIR}/obs_theme.h"
    "${CORDMUX_DOCK_DRIVE_PREVIEW_DIR}/obs_theme.cpp"
    "${CORDMUX_DOCK_DRIVE_PREVIEW_DIR}/obs_dock.h"
    "${CORDMUX_DOCK_DRIVE_PREVIEW_DIR}/render.h"
    "${CORDMUX_DOCK_DRIVE_PREVIEW_DIR}/render.cpp"
    "${CORDMUX_DOCK_DRIVE_PREVIEW_DIR}/preview_platform.h"
    "${CORDMUX_DOCK_DRIVE_PREVIEW_DIR}/preview_platform.cpp"
    "${CORDMUX_DOCK_DRIVE_PREVIEW_DIR}/preview_platform.json"
    "${CORDMUX_DOCK_DRIVE_PREVIEW_DIR}/stub/obs-module.h"
    "${CORDMUX_DOCK_DRIVE_PREVIEW_DIR}/stub/obs-frontend-api.h"
    "${CORDMUX_DOCK_DRIVE_PREVIEW_DIR}/obs_names.h"
  )

  # OBSDock と OBSBasic は Q_OBJECT を持つ (OBS のテーマが型名でスタイルを選ぶため。
  # src-obs/tests/ui_preview/ui_preview.cmake と同じ)。
  set_target_properties(dock_drive PROPERTIES AUTOMOC ON)

  # stub/ を先に探す (obs-module.h を libobs のものと取り違えないため)。
  target_include_directories(dock_drive BEFORE PRIVATE "${CORDMUX_DOCK_DRIVE_PREVIEW_DIR}/stub")
  target_include_directories(
    dock_drive
    PRIVATE "${CORDMUX_DOCK_DRIVE_DIR}/../.." "${CORDMUX_DOCK_DRIVE_PREVIEW_DIR}" "${CORDMUX_DOCK_DRIVE_DIR}"
            "${cordmux_json_SOURCE_DIR}/include"
  )

  cordmux_add_ledger(dock_drive)
  # 製品の diag::logf。テストが出力先を設定し、状態を変えない区間のログの行を数える。
  cordmux_add_diag(dock_drive)

  # 製品のドック、参加者の一覧、ウィンドウ、ダイアログ (src-obs/tests/ui_preview/ui_preview.cmake と同じ関数を呼ぶ)。
  # テストはすべてのウィジェットを押すので、ウィジェットごとの .cmake の関数がなければ、ビルドを失敗させる。
  cordmux_add_dock_ui(dock_drive)
  cordmux_add_dock_face(dock_drive)
  cordmux_add_rows_observation(dock_drive)
  cordmux_add_rows(dock_drive)
  cordmux_add_windows_observation(dock_drive)
  cordmux_add_windows(dock_drive)
  cordmux_add_install_ui_observation(dock_drive)
  cordmux_add_install_ui(dock_drive)
  # ドックの更新 1 回の時間を測る場面 (scen_positions.cpp の run_positions_refresh_cost) が、製品の build_observation を
  # 呼ぶ。build_observation.cpp は Qt を含まず、platform_win.cpp の時刻の変換を使う (align_tests と同じ組み合わせ)。
  cordmux_add_dock_observation(dock_drive)
  target_sources(dock_drive PRIVATE "${CORDMUX_DOCK_DRIVE_DIR}/../../platform/platform_win.cpp")

  target_compile_definitions(
    dock_drive
    PRIVATE QT_NO_KEYWORDS
            CORDMUX_DOCK_DRIVE_LOCALE_DIR="${CORDMUX_DOCK_DRIVE_DIR}/../../data/locale"
            CORDMUX_DOCK_DRIVE_OBS_SOURCE_DIR="${CORDMUX_OBS_STUDIO_SOURCE_DIR}"
  )

  target_link_libraries(dock_drive PRIVATE Qt6::Core Qt6::Gui Qt6::GuiPrivate Qt6::Widgets Qt6::WidgetsPrivate Qt6::Svg shell32 ole32 ws2_32)

  if(MSVC)
    # /Zi と /DEBUG: クラッシュしたときに crash_trace.cpp が関数の名前と行番号を書けるように、Release でも
    # PDB を作る (/OPT:REF と /OPT:ICF は、/DEBUG が無効にする既定を戻す)。
    target_compile_options(dock_drive PRIVATE /W4 /utf-8 /permissive- /Zi)
    target_link_options(dock_drive PRIVATE /DEBUG /OPT:REF /OPT:ICF)
    target_link_libraries(dock_drive PRIVATE dbghelp)
  else()
    target_compile_options(dock_drive PRIVATE -Wall -Wextra)
  endif()

  # 実行に必要な DLL と Qt のプラグイン (プレビューツールと同じ。実行ファイルの隣にないと起動できない)。
  add_custom_command(
    TARGET dock_drive
    POST_BUILD
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::Core>" "$<TARGET_FILE:Qt6::Gui>"
      "$<TARGET_FILE:Qt6::Widgets>" "$<TARGET_FILE:Qt6::Svg>" "$<TARGET_FILE_DIR:dock_drive>"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:dock_drive>/styles"
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::QModernWindowsStylePlugin>"
      "$<TARGET_FILE_DIR:dock_drive>/styles"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:dock_drive>/imageformats"
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::QSvgPlugin>" "$<TARGET_FILE_DIR:dock_drive>/imageformats"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:dock_drive>/iconengines"
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::QSvgIconPlugin>"
      "$<TARGET_FILE_DIR:dock_drive>/iconengines"
  )
endfunction()
