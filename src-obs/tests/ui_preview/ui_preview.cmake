# SPDX-License-Identifier: Apache-2.0

# プレビューツール (ui_preview.exe) のターゲットを cordmux_add_ui_preview() で追加する。製品のドック、設定ウィンドウ、
# 参加者の設定ウィンドウ、ダイアログを製品と同じ .cmake の関数で組み込み、状態ごとの画像を書き出す。libobs は使わず、
# 製品が呼ぶ libobs と OBS のフロントエンドの関数のうち 3 つ (obs_module_text、obs_frontend_get_locale_string、
# obs_frontend_get_main_window) を、stub/ と locale_stub.cpp が同じ意味で持つ。src-obs/tests/CMakeLists.txt から、
# decor_paint.cmake と ws_deps.cmake の後に include する (Qt6 を探す場所 CORDMUX_OBS_DEPS_QT6_HINTS と、nlohmann/json の
# 場所 cordmux_json_SOURCE_DIR を使うため)。合否を持たないので、add_test には追加しない。使い方は README.md にある。

set(CORDMUX_UI_PREVIEW_DIR "${CMAKE_CURRENT_LIST_DIR}")
include("${CORDMUX_UI_PREVIEW_DIR}/../../cmake/cordmux_obs_deps.cmake")

function(cordmux_add_ui_preview)
  find_package(Qt6 COMPONENTS Core Gui Widgets Svg REQUIRED PATHS ${CORDMUX_OBS_DEPS_QT6_HINTS} NO_DEFAULT_PATH)

  add_executable(
    ui_preview
    "${CORDMUX_UI_PREVIEW_DIR}/main.cpp"
    "${CORDMUX_UI_PREVIEW_DIR}/faces.h"
    "${CORDMUX_UI_PREVIEW_DIR}/faces.cpp"
    "${CORDMUX_UI_PREVIEW_DIR}/fixtures.h"
    "${CORDMUX_UI_PREVIEW_DIR}/fixtures.cpp"
    "${CORDMUX_UI_PREVIEW_DIR}/measure.h"
    "${CORDMUX_UI_PREVIEW_DIR}/measure.cpp"
    "${CORDMUX_UI_PREVIEW_DIR}/probes.h"
    "${CORDMUX_UI_PREVIEW_DIR}/probes.cpp"
    "${CORDMUX_UI_PREVIEW_DIR}/locale_stub.h"
    "${CORDMUX_UI_PREVIEW_DIR}/locale_stub.cpp"
    "${CORDMUX_UI_PREVIEW_DIR}/obs_theme.h"
    "${CORDMUX_UI_PREVIEW_DIR}/obs_theme.cpp"
    "${CORDMUX_UI_PREVIEW_DIR}/obs_dock.h"
    "${CORDMUX_UI_PREVIEW_DIR}/obs_names.h"
    "${CORDMUX_UI_PREVIEW_DIR}/render.h"
    "${CORDMUX_UI_PREVIEW_DIR}/render.cpp"
    "${CORDMUX_UI_PREVIEW_DIR}/fidelity.h"
    "${CORDMUX_UI_PREVIEW_DIR}/fidelity.cpp"
    "${CORDMUX_UI_PREVIEW_DIR}/preview_platform.h"
    "${CORDMUX_UI_PREVIEW_DIR}/preview_platform.cpp"
    "${CORDMUX_UI_PREVIEW_DIR}/preview_platform.json"
    "${CORDMUX_UI_PREVIEW_DIR}/stub/obs-module.h"
    "${CORDMUX_UI_PREVIEW_DIR}/stub/obs-frontend-api.h"
  )

  # OBSDock (obs_dock.h) は Q_OBJECT を持つ。OBS のテーマは `OBSDock > QWidget` のように型の名前でドックの中身を
  # 選ぶので、同じ名前の型が必要になる (moc が型の名前を QMetaObject に入れる)。
  set_target_properties(ui_preview PROPERTIES AUTOMOC ON)

  # stub/ を先に探す (obs-module.h を libobs のヘッダーと取り違えないように)。
  target_include_directories(
    ui_preview BEFORE
    PRIVATE "${CORDMUX_UI_PREVIEW_DIR}/stub"
  )
  target_include_directories(
    ui_preview
    PRIVATE "${CORDMUX_UI_PREVIEW_DIR}/../.." "${CORDMUX_UI_PREVIEW_DIR}" "${cordmux_json_SOURCE_DIR}/include"
  )

  # 製品のファイルは、製品と同じ .cmake の関数で追加する (ファイルの一覧を 2 か所に書かないため)。
  # cordmux_add_dock() は呼ばない。libobs を使う dock_bind と dock_poll を追加するためである。
  # cordmux_add_dock_observation() も呼ばない。追加する build_observation.cpp が src-obs/platform を使うためである。
  cordmux_add_ledger(ui_preview)

  # ドックとウィンドウ。src-obs/dock/dock.cmake が include する .cmake の関数を、製品と同じく呼ぶ (libobs の代わりは
  # stub/ の 3 つだけ。src-obs/dock/dock.cmake の cordmux_add_dock_face のコメント)。その .cmake がなくてもビルドできる
  # ように、関数があるときだけ呼び、呼んだものを CORDMUX_UI_PREVIEW_HAS_* で faces.cpp に伝える (ビルドにない描画の
  # 対象を指定すると、faces.cpp はそのことを出力して終了する)。Qt を使わない側の関数 (_observation) は、Qt を使う側の
  # 関数が追加しないので、両方を呼ぶ。
  if(COMMAND cordmux_add_dock_ui)
    cordmux_add_dock_ui(ui_preview)
  endif()
  cordmux_add_dock_face(ui_preview)
  if(COMMAND cordmux_add_rows)
    cordmux_add_rows_observation(ui_preview)
    cordmux_add_rows(ui_preview)
    target_compile_definitions(ui_preview PRIVATE CORDMUX_UI_PREVIEW_HAS_ROWS=1)
  endif()
  if(COMMAND cordmux_add_windows)
    cordmux_add_windows_observation(ui_preview)
    cordmux_add_windows(ui_preview)
    target_compile_definitions(ui_preview PRIVATE CORDMUX_UI_PREVIEW_HAS_WINDOWS=1)
  endif()
  if(COMMAND cordmux_add_install_ui)
    cordmux_add_install_ui_observation(ui_preview)
    cordmux_add_install_ui(ui_preview)
    target_compile_definitions(ui_preview PRIVATE CORDMUX_UI_PREVIEW_HAS_INSTALL_UI=1)
  endif()

  # QT_NO_KEYWORDS は製品のドックと同じ (Qt の slots などの小文字のキーワードをマクロにしない。
  # src-obs/dock/dock.cmake の cordmux_add_dock)。製品のファイルを、製品と違う前処理でビルドしないためである。
  target_compile_definitions(
    ui_preview
    PRIVATE QT_NO_KEYWORDS
            CORDMUX_UI_PREVIEW_LOCALE_DIR="${CORDMUX_UI_PREVIEW_DIR}/../../data/locale"
            CORDMUX_UI_PREVIEW_OBS_SOURCE_DIR="${CORDMUX_OBS_STUDIO_SOURCE_DIR}"
            # --obs 32 で読む、OBS 32.2.2 をインストールした形のフォルダー。リポジトリに入れないフォルダーで、なければ
            # 実行したときにエラーで終了する。
            CORDMUX_UI_PREVIEW_OBS32_DIR="${CORDMUX_UI_PREVIEW_DIR}/../../../_work/obs-32.2.2"
  )

  # Qt6::GuiPrivate は、プラットフォームプラグイン cordmuxpreview (preview_platform.cpp) が使う (QPA の型と、OBS と
  # 同じ DirectWrite 版のフォントデータベース QWindowsDirectWriteFontDatabase)。shell32 と ole32 は、ダイアログの
  # アイコン (SHGetStockIconInfo) を QWindowsTheme と同じ方法で読むために使う (OLE の初期化も QWindowsContext と同じ)。
  target_link_libraries(ui_preview PRIVATE Qt6::Core Qt6::Gui Qt6::GuiPrivate Qt6::Widgets Qt6::Svg shell32 ole32)

  if(MSVC)
    target_compile_options(ui_preview PRIVATE /W4 /utf-8 /permissive-)
  else()
    target_compile_options(ui_preview PRIVATE -Wall -Wextra)
  endif()

  # 実行に必要な DLL と Qt のプラグインを、実行ファイルと同じフォルダーにコピーする (decor_tests と同じく、ないと
  # 起動できない)。
  #   platforms/qminimal          ウィンドウを作らない Qt 同梱のプラットフォームプラグイン (--backend minimal のときだけ
  #                               使う。既定の cordmuxpreview は実行ファイルの中にある)
  #   styles/qmodernwindowsstyle  OBS が Windows で使うスタイル (windowsvista)
  #   imageformats/qsvg           テーマが url(theme:...svg) で読むチェックボックスと矢印の画像
  #   iconengines/qsvgicon        テーマの svg をアイコンとして読むプラグイン
  add_custom_command(
    TARGET ui_preview
    POST_BUILD
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::Core>" "$<TARGET_FILE:Qt6::Gui>"
      "$<TARGET_FILE:Qt6::Widgets>" "$<TARGET_FILE:Qt6::Svg>" "$<TARGET_FILE_DIR:ui_preview>"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ui_preview>/platforms"
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::QMinimalIntegrationPlugin>"
      "$<TARGET_FILE_DIR:ui_preview>/platforms"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ui_preview>/styles"
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::QModernWindowsStylePlugin>"
      "$<TARGET_FILE_DIR:ui_preview>/styles"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ui_preview>/imageformats"
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::QSvgPlugin>" "$<TARGET_FILE_DIR:ui_preview>/imageformats"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ui_preview>/iconengines"
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::QSvgIconPlugin>"
      "$<TARGET_FILE_DIR:ui_preview>/iconengines"
  )
endfunction()
