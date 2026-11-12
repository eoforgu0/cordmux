# SPDX-License-Identifier: Apache-2.0

# OBS のプラグイン cordmux.dll をビルドする。obs-plugintemplate の CMakeLists.txt を元にしている。各フォルダーの
# ファイルは、そのフォルダーの .cmake (src-obs/config/config.cmake など) がこのターゲットに追加する。単体テストは、
# src-obs/tests/CMakeLists.txt の別のプロジェクトでビルドする。

cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

# 既定を ON にする。プリセットを使わずに configure しても、製品と同じ構成になるようにするためである
# (src-obs/CMakePresets.json の template も ON にしている)。
option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

# align/ には .cmake がないので、ここで追加する。src-obs/tests/CMakeLists.txt も、同じ align/*.cpp を単体テストに
# 追加する。
target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    plugin-main.cpp
    align/timestamp_aligner.cpp
    align/av_offset.cpp
)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_17)

if(OS_WINDOWS)
  # platform/platform_win.cpp は、OS に依存する処理の Windows の実装である。ws2_32 は WinSock のためにリンクする。
  # advapi32、ole32、shell32 のリンクは src-obs/ws/ws.cmake にある。
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE platform/platform_win.cpp)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ws2_32)
endif()

# 各フォルダーの .cmake。どれも target_sources でこのターゲットにファイルを追加するので、add_library より後に
# include する。
#
# config/: 設定と割り当て表 (ヘッダーだけ)。
include(config/config.cmake)

# diag/: ログの出力先。
include(diag/diag.cmake)
cordmux_add_diag(${CMAKE_PROJECT_NAME})

# decode/: H.264 のデコーダーと、FFmpeg (avcodec、avutil、swresample) のリンク。
include(decode/decode.cmake)
cordmux_add_decode(${CMAKE_PROJECT_NAME})

# ws/: 送信側との WebSocket の接続。websocketpp、standalone asio、nlohmann/json を取得して追加する。ほかの
# フォルダー (config/、install/、record/) も、ここで追加する nlohmann/json のヘッダーを使う。
include(ws/ws.cmake)

# sources/: OBS のソース (合成のソース)。
include(sources/sources.cmake)

# ledger/: 表示の規則 (ドックに表示するものを決める純粋関数) と、画面の文言のキー。
include(ledger/ledger.cmake)
cordmux_add_ledger(${CMAKE_PROJECT_NAME})

# dock/: ドックと設定ウィンドウ (Qt6::Widgets)。
include(dock/dock.cmake)
cordmux_add_dock(${CMAKE_PROJECT_NAME})

# bundle/: 診断情報の zip。
include(bundle/bundle.cmake)
cordmux_add_bundle(${CMAKE_PROJECT_NAME})

# install/: Vesktop の設定の書き換え、更新、アンインストール。
include(install/install.cmake)
cordmux_add_install(${CMAKE_PROJECT_NAME})

# record/: 録画。ヘッダーだけの部分 (cordmux_add_record_core) と、I/O の部分 (cordmux_add_record_io。avformat を
# リンクする) に分かれる。src-obs/ws/ws.cmake と src-obs/decode/decode.cmake の後に include する (理由は
# src-obs/record/record.cmake にある)。pipeline/ が録画の I/O の部分を呼び出し、decode/ffmpeg_link_versions.cpp も
# avformat を呼び出すので、cordmux_add_record_io を呼ばないと、cordmux.dll をリンクできない。
include(record/record.cmake)
cordmux_add_record_core(${CMAKE_PROJECT_NAME})
cordmux_add_record_io(${CMAKE_PROJECT_NAME})

# cmake --install で、同梱する FFmpeg のファイル (11 個の DLL と ffmpeg.exe) を、cordmux.dll と同じフォルダーに
# 置く。ファイルの一覧は src-obs/cmake/cordmux_ffmpeg_runtime.cmake にある。
include(cmake/cordmux_ffmpeg_runtime.cmake)
cordmux_install_ffmpeg_runtime(${CMAKE_PROJECT_NAME})

# pipeline/: 受信した映像と音声を、デコードとタイムスタンプの調整を通して枠のソースに渡す。リンクするライブラリは、
# 上のフォルダーの .cmake が追加したものを使う (src-obs/pipeline/pipeline.cmake)。
include(pipeline/pipeline.cmake)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
