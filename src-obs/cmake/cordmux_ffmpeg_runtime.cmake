# SPDX-License-Identifier: Apache-2.0

# cordmux.dll と一緒に置く FFmpeg のファイル (同じフォルダーに置く 11 個の DLL と ffmpeg.exe) の一覧を、この 1 か所に持つ。
# 置くファイルとその理由は、docs/release.md の「配布用の zip の中身」にある。obs-deps の bin フォルダー
# (CORDMUX_OBS_DEPS_BIN_DIR) は、リンクに使うインポートライブラリと同じく src-obs/cmake/cordmux_obs_deps.cmake が決める
# (リンクしたものと同じファイルを配るため)。このファイルを src-obs/cmake の直下に置き、名前を cordmux_ で始めるのは、
# src-obs/cmake/common などの下にある、テンプレートからコピーしたファイルと分けるためである。src-obs/tests は別の CMake
# プロジェクトで CMAKE_MODULE_PATH を持たないので、どちらもパスで include する。

set(CORDMUX_FFMPEG_RUNTIME_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")
include("${CORDMUX_FFMPEG_RUNTIME_MODULE_DIR}/cordmux_obs_deps.cmake")

# 一覧。test/tools/dist-zip/dist_common.py が、次の 2 つのマーカーの間の set(...) を読み、配布用の zip の cordmux フォルダーの直下と
# 比べる。マーカーは行の先頭で探すので、マーカーの行は変えない。
# ffmpeg-runtime-begin
# cordmux.dll が読み込む 8 つ (前の 5 つは avcodec と、それが読み込む DLL。後の 3 つは avformat と、それが読み込む DLL)。
set(
  CORDMUX_FFMPEG_RUNTIME_LINKED
  avcodec-61.dll
  avutil-59.dll
  swresample-5.dll
  libx264-164.dll
  zlib.dll
  avformat-61.dll
  srt.dll
  librist.dll
)
# ffmpeg.exe だけが読み込む 3 つ。
set(CORDMUX_FFMPEG_RUNTIME_EXE_ONLY avdevice-61.dll avfilter-10.dll swscale-8.dll)
# 録画ファイルの作成に起動する実行ファイル。
set(CORDMUX_FFMPEG_RUNTIME_EXE ffmpeg.exe)
# ffmpeg-runtime-end

# ファイルの名前の一覧を、obs-deps の bin フォルダーの中のパスの一覧にする。
function(_cordmux_ffmpeg_runtime_paths out_var)
  set(_paths "")
  foreach(_name IN LISTS ARGN)
    list(APPEND _paths "${CORDMUX_OBS_DEPS_BIN_DIR}/${_name}")
  endforeach()
  set(${out_var} "${_paths}" PARENT_SCOPE)
endfunction()

# target の実行ファイルのフォルダーに、cordmux.dll が読み込む 8 つの DLL をコピーする (src-obs/tests/CMakeLists.txt が
# 呼ぶ)。テストは ffmpeg.exe の場所を環境変数で受け取るので、ffmpeg.exe と、それだけが読み込む 3 つはコピーしない。
# ないファイルがあると copy_if_different が失敗し、ビルドが失敗する (ないファイルを飛ばしてビルドを続けない)。
function(cordmux_copy_ffmpeg_runtime target)
  _cordmux_ffmpeg_runtime_paths(_files ${CORDMUX_FFMPEG_RUNTIME_LINKED})
  add_custom_command(
    TARGET ${target}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_files} "$<TARGET_FILE_DIR:${target}>"
    VERBATIM
  )
endfunction()

# `cmake --install` で、11 個の DLL と ffmpeg.exe を <prefix>/<target> の直下に置く (src-obs/CMakeLists.txt が呼ぶ)。
# 置く場所は、src-obs/cmake/windows/helpers.cmake の set_target_properties_plugin が cordmux.dll を置く場所と同じである。
function(cordmux_install_ffmpeg_runtime target)
  _cordmux_ffmpeg_runtime_paths(
    _files
    ${CORDMUX_FFMPEG_RUNTIME_LINKED}
    ${CORDMUX_FFMPEG_RUNTIME_EXE_ONLY}
    ${CORDMUX_FFMPEG_RUNTIME_EXE}
  )
  install(FILES ${_files} DESTINATION "${target}")
endfunction()
