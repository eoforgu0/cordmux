# SPDX-License-Identifier: Apache-2.0

# src-obs/record (録画) のファイルをターゲットに追加する 2 つの関数を定義する。
#   cordmux_add_record_core(<target>)  ヘッダーだけの部分 (コンパイル単位を増やさない)
#   cordmux_add_record_io(<target>)    I/O の部分 (.cpp 3 つと avformat のリンク)
# src-obs/CMakeLists.txt と src-obs/tests/CMakeLists.txt の両方がこのファイルを include し、2 つの関数を 1 回ずつ
# 呼ぶ (同じファイルの一覧を 2 か所に書かない)。src-obs/ws/ws_deps.cmake と src-obs/decode/decode.cmake の後に
# include する。理由はそれぞれの関数のコメントにある。

# include() は呼び出し元の CMAKE_CURRENT_SOURCE_DIR を変えないので、このファイルのフォルダーを先に変数に入れておく。
set(CORDMUX_RECORD_DIR "${CMAKE_CURRENT_LIST_DIR}")

# ヘッダーだけの部分を追加する。ほとんどは純粋関数である。target_sources に追加するのは .h だけなので、コンパイル単位は
# 増えない (IDE のツリーに表示するためだけに列挙する)。ヘッダーは nlohmann/json.hpp を読み込むが、その
# インクルードディレクトリをターゲットに追加するのは src-obs/ws/ws_deps.cmake の cordmux_add_ws_deps() なので、
# このファイルは ws_deps.cmake の後に include する。
function(cordmux_add_record_core target)
  target_sources(
    ${target}
    PRIVATE
      "${CORDMUX_RECORD_DIR}/record_types.h"
      "${CORDMUX_RECORD_DIR}/record_clock.h"
      "${CORDMUX_RECORD_DIR}/record_segment.h"
      "${CORDMUX_RECORD_DIR}/record_ledger.h"
      "${CORDMUX_RECORD_DIR}/record_avcc.h"
      "${CORDMUX_RECORD_DIR}/record_black.h"
      "${CORDMUX_RECORD_DIR}/record_audio.h"
      "${CORDMUX_RECORD_DIR}/record_args.h"
      "${CORDMUX_RECORD_DIR}/record_branch.h"
      "${CORDMUX_RECORD_DIR}/record_space.h"
      "${CORDMUX_RECORD_DIR}/record_location.h"
      "${CORDMUX_RECORD_DIR}/record_search_dirs.h"
  )
endfunction()

# ---------------------------------------------------------------------------
# I/O の部分
# ---------------------------------------------------------------------------
#
# 探すのは avformat だけである。srt と librist は、avformat-61.dll のインポートテーブルが読み込みのときに要求する
# 間接の依存で (ldd で確認した)、cordmux のコードは呼ばないのでリンクしない。ただし DLL は 3 つとも実行ファイルの
# 隣にコピーする (src-obs/cmake/cordmux_ffmpeg_runtime.cmake の CORDMUX_FFMPEG_RUNTIME_LINKED)。
# avformat の探し方は、src-obs/decode/decode.cmake が決めた CORDMUX_FFMPEG_INCLUDE_DIR と、decode.cmake が読み込んだ
# src-obs/cmake/cordmux_obs_deps.cmake の cordmux_obs_deps_file (場所は src-obs/buildspec.json から求める) を
# そのまま使い、2 か所に書かない。そのため、このファイルは decode.cmake の後に include する。
cordmux_obs_deps_file(CORDMUX_AVFORMAT_LIBRARY "lib/avformat.lib")

# I/O の部分を追加する。align_tests にも入る。実際の ffmpeg と ffprobe を使うテストが、セグメントを書く処理
# そのものを通してテストデータを作るためである。I/O の部分が使うのは avformat と RecordPlatformOps だけで、libobs
# には依存しないので、テストの実行ファイルにそのまま入る。
function(cordmux_add_record_io target)
  target_sources(
    ${target}
    PRIVATE
      "${CORDMUX_RECORD_DIR}/record_writer.h"
      "${CORDMUX_RECORD_DIR}/record_writer.cpp"
      "${CORDMUX_RECORD_DIR}/record_finalize.h"
      "${CORDMUX_RECORD_DIR}/record_finalize.cpp"
      "${CORDMUX_RECORD_DIR}/recorder.h"
      "${CORDMUX_RECORD_DIR}/recorder.cpp"
  )
  target_include_directories(${target} PRIVATE "${CORDMUX_FFMPEG_INCLUDE_DIR}")
  target_link_libraries(${target} PRIVATE "${CORDMUX_AVFORMAT_LIBRARY}")
endfunction()
