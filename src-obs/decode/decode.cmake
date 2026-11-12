# SPDX-License-Identifier: Apache-2.0

# decode/ (H.264 のデコーダー) を target に追加する関数 cordmux_add_decode(<target>) を定義する。
# src-obs/CMakeLists.txt と src-obs/tests/CMakeLists.txt の両方がこのファイルを include し、
# cordmux_add_decode を 1 回ずつ呼び出す。FFmpeg の探し方を 1 か所にまとめるためである。
# FFmpeg は obs-deps のものを使う。場所は src-obs/cmake/cordmux_obs_deps.cmake が
# src-obs/buildspec.json から決める。

# 関数の中で CMAKE_CURRENT_LIST_DIR を使うと呼び出し元のファイルの場所になるので、このファイルの場所を
# 先に変数に入れておく。
set(CORDMUX_DECODE_DIR "${CMAKE_CURRENT_LIST_DIR}")
include("${CORDMUX_DECODE_DIR}/../cmake/cordmux_obs_deps.cmake")
# record/record.cmake が、この変数を使って avformat を同じ場所から探す。
set(CORDMUX_DECODE_DEPS_DIR "${CORDMUX_OBS_DEPS_DIR}")

cordmux_obs_deps_file(_cordmux_avcodec_header "include/libavcodec/avcodec.h")
set(CORDMUX_FFMPEG_INCLUDE_DIR "${CORDMUX_OBS_DEPS_DIR}/include")
unset(_cordmux_avcodec_header)
cordmux_obs_deps_file(CORDMUX_AVCODEC_LIBRARY "lib/avcodec.lib")
cordmux_obs_deps_file(CORDMUX_AVUTIL_LIBRARY "lib/avutil.lib")
# swresample は avcodec が使うライブラリで、decode/ のコードは直接使わない。
# decode/ffmpeg_link_versions.cpp が swresample_version() を呼び出して、cordmux.dll の直接の依存にする
# (理由は decode/ffmpeg_link_versions.h のコメントにある)。
cordmux_obs_deps_file(CORDMUX_SWRESAMPLE_LIBRARY "lib/swresample.lib")
# DLL を実行ファイルの隣へコピーするのは src-obs/cmake/cordmux_ffmpeg_runtime.cmake である。

# target に decode/ のソースファイル、FFmpeg のヘッダーの場所、FFmpeg のライブラリを追加する。
# ffmpeg_link_versions.cpp は avformat_version() も呼び出すので、target は avformat もリンクする必要がある
# (record/record.cmake の cordmux_add_record_io())。
function(cordmux_add_decode target)
  target_sources(
    ${target}
    PRIVATE "${CORDMUX_DECODE_DIR}/h264_decoder.cpp" "${CORDMUX_DECODE_DIR}/ffmpeg_link_versions.cpp"
  )
  target_include_directories(${target} PRIVATE "${CORDMUX_FFMPEG_INCLUDE_DIR}")
  target_link_libraries(
    ${target}
    PRIVATE "${CORDMUX_AVCODEC_LIBRARY}" "${CORDMUX_AVUTIL_LIBRARY}" "${CORDMUX_SWRESAMPLE_LIBRARY}"
  )
endfunction()
