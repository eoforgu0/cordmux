# SPDX-License-Identifier: Apache-2.0

# 接続の待ち受け (src-obs/ws) を cordmux.dll に追加する。src-obs/CMakeLists.txt が include する。
# 新しいターゲットは作らず、${CMAKE_PROJECT_NAME} に listener.cpp と、ws/ が使う外部のライブラリ
# (src-obs/ws/ws_deps.cmake の cordmux_add_ws_deps) を追加するので、add_library より後に include する。
# 単体テストの実行ファイルには、src-obs/tests/CMakeLists.txt が ws_deps.cmake を使って追加する。

include(ws/ws_deps.cmake)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    ws/listener.cpp
)

cordmux_add_ws_deps(${CMAKE_PROJECT_NAME})

# cordmux.dll のバージョンの文字列 CORDMUX_VERSION_STRING を、ターゲット全体のコンパイルの定義として渡す。
# 値は <version>+<短いコミット> で、version は src-obs/buildspec.json の値 (src-obs/cmake/common/bootstrap.cmake が
# PLUGIN_VERSION にする) である。CI は -DCORDMUX_BUILD_COMMIT=<短いコミット> を渡し、ローカルのビルドは
# <version>+dev になる。CORDMUX_BUILD_COMMIT の名前は、CI のワークフローも使うので変えない。
# ターゲット全体に適用されるので、ListenerConfig::server_version の既定値 (src-obs/ws/listener.h) と plugin-main.cpp の
# kPluginVersion は同じ値になる。送信側のバージョン (src-vencord/version.ts の CORDMUX_BUILD_ID) と、同梱の
# Vencord のビルドの情報 (src-obs/install/build_mark.h の build_id) も同じ形の文字列で、ドックはバージョンの
# 食い違いを文字列のまま比べて調べる。resource.rc.in などは、buildspec.json の version (数字 3 つ) をそのまま使う。
set(CORDMUX_BUILD_COMMIT "dev" CACHE STRING "Short commit id appended to the plugin version (dev for local builds)")
target_compile_definitions(
  ${CMAKE_PROJECT_NAME} PRIVATE
    CORDMUX_VERSION_STRING="${PLUGIN_VERSION}+${CORDMUX_BUILD_COMMIT}"
)

if(OS_WINDOWS)
  # advapi32、ole32、shell32 は、ws/ ではなく src-obs/platform/platform_win.cpp が使う。たとえば、session.json の
  # ACL (SetNamedSecurityInfoW、ConvertSidToStringSidW) と、%LOCALAPPDATA% などのフォルダーの場所
  # (SHGetKnownFolderPath、CoTaskMemFree) である。WinSock の ws2_32 は src-obs/CMakeLists.txt がリンクする。
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE advapi32 ole32 shell32)
endif()
