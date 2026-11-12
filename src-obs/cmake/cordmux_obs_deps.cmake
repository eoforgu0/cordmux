# SPDX-License-Identifier: Apache-2.0

# obs-deps のフォルダー (FFmpeg のインポートライブラリと DLL、Qt6) を src-obs/buildspec.json のバージョンから決めて、変数に
# 入れる。リンクに使うものと配るものが同じファイルになるように、場所はこの 1 か所で決める。使うのは、
# src-obs/decode/decode.cmake (FFmpeg のヘッダーとインポートライブラリ)、src-obs/cmake/cordmux_ffmpeg_runtime.cmake (配る
# DLL)、src-obs/sources/decor_paint.cmake (テストのビルドで Qt6::Gui を探す場所)、src-obs/tests/ui_preview/ui_preview.cmake
# (Qt6 と OBS のソースの場所) で、どれも自分でこのファイルを include する (include_guard で 2 回目は読まない)。
# src-obs/record/record.cmake は、decode.cmake の CORDMUX_DECODE_DEPS_DIR から avformat を探す。

include_guard(GLOBAL)

set(CORDMUX_OBS_DEPS_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")

file(READ "${CORDMUX_OBS_DEPS_MODULE_DIR}/../buildspec.json" _cordmux_obs_deps_buildspec)
string(JSON _cordmux_obs_deps_obs_version GET "${_cordmux_obs_deps_buildspec}" dependencies obs-studio version)
string(JSON _cordmux_obs_deps_prebuilt_version GET "${_cordmux_obs_deps_buildspec}" dependencies prebuilt version)
string(JSON _cordmux_obs_deps_qt6_version GET "${_cordmux_obs_deps_buildspec}" dependencies qt6 version)

set(_cordmux_obs_deps_root "${CORDMUX_OBS_DEPS_MODULE_DIR}/../.deps")
set(_cordmux_obs_deps_subbuild "${_cordmux_obs_deps_root}/obs-studio-${_cordmux_obs_deps_obs_version}/.deps")

# FFmpeg などの obs-deps のフォルダー (include、lib、bin を持つ)。obs-studio のサブビルドの .deps の下を使う (トップレベル
# の .deps は、最初の取得に失敗すると空になることがある)。
set(CORDMUX_OBS_DEPS_DIR "${_cordmux_obs_deps_subbuild}/obs-deps-${_cordmux_obs_deps_prebuilt_version}-x64")
cmake_path(NORMAL_PATH CORDMUX_OBS_DEPS_DIR)
set(CORDMUX_OBS_DEPS_BIN_DIR "${CORDMUX_OBS_DEPS_DIR}/bin")

# Qt6 を探す場所 (サブビルドの .deps、トップレベルの .deps の順)。
set(_cordmux_obs_deps_qt6_sub "${_cordmux_obs_deps_subbuild}/obs-deps-qt6-${_cordmux_obs_deps_qt6_version}-x64")
set(_cordmux_obs_deps_qt6_top "${_cordmux_obs_deps_root}/obs-deps-qt6-${_cordmux_obs_deps_qt6_version}-x64")
cmake_path(NORMAL_PATH _cordmux_obs_deps_qt6_sub)
cmake_path(NORMAL_PATH _cordmux_obs_deps_qt6_top)
set(CORDMUX_OBS_DEPS_QT6_HINTS "${_cordmux_obs_deps_qt6_sub}" "${_cordmux_obs_deps_qt6_top}")

# OBS のソースのフォルダー。プレビューツール (src-obs/tests/ui_preview/ui_preview.cmake) だけが使い、OBS が読むのと
# 同じテーマのファイル (.obt と .ovt) とフォントを読む。実行するときに読むので、このフォルダーがなくてもビルドはできる。
# バージョンは、ほかのフォルダーと同じく buildspec.json から決める。
set(CORDMUX_OBS_STUDIO_SOURCE_DIR "${_cordmux_obs_deps_root}/obs-studio-${_cordmux_obs_deps_obs_version}")
cmake_path(NORMAL_PATH CORDMUX_OBS_STUDIO_SOURCE_DIR)

# buildspec.json を変えたら、configure をやり直す。
set_property(
  DIRECTORY
  APPEND
  PROPERTY CMAKE_CONFIGURE_DEPENDS "${CORDMUX_OBS_DEPS_MODULE_DIR}/../buildspec.json"
)

unset(_cordmux_obs_deps_buildspec)
unset(_cordmux_obs_deps_obs_version)
unset(_cordmux_obs_deps_prebuilt_version)
unset(_cordmux_obs_deps_qt6_version)
unset(_cordmux_obs_deps_root)
unset(_cordmux_obs_deps_subbuild)
unset(_cordmux_obs_deps_qt6_sub)
unset(_cordmux_obs_deps_qt6_top)

# obs-deps の中の 1 つのファイルの場所を、あることを確認してから out_var に入れる。なければ configure を失敗させる。
# find_library と find_path は使わない。どちらも見つけた場所をキャッシュに残し、次の configure では探し直さないので、
# buildspec.json のバージョンを更新しても、古い .deps が残っている環境では、前のバージョンのインポートライブラリにリンク
# したまま、新しいバージョンの DLL を配ってしまう。場所は buildspec.json から決まるので、探す必要もない。前の configure
# のキャッシュに同じ名前の変数が残っていても、普通の変数のほうが先に読まれる。
function(cordmux_obs_deps_file out_var relative_path)
  set(_path "${CORDMUX_OBS_DEPS_DIR}/${relative_path}")
  if(NOT EXISTS "${_path}")
    message(FATAL_ERROR "cordmux: obs-deps file not found: ${_path} (derived from src-obs/buildspec.json)")
  endif()
  set(${out_var} "${_path}" PARENT_SCOPE)
endfunction()
