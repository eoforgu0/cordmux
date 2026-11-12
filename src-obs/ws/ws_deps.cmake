# SPDX-License-Identifier: Apache-2.0

# ws/ が使う外部のライブラリ (standalone asio、websocketpp、nlohmann/json) を取得してターゲットに追加する関数
# cordmux_add_ws_deps(<target>) を定義する。src-obs/ws/ws.cmake と src-obs/tests/CMakeLists.txt の両方がこのファイルを
# include するので、cordmux.dll とテストの実行ファイルは同じバージョンのライブラリでビルドされる。

# 関数の中で CMAKE_CURRENT_LIST_DIR を読むと、呼び出した側のファイルのフォルダーになる。そのため、include された
# ときにこのファイルのフォルダーを変数に保存しておく (listener.cpp の /wd4244 の絶対パスに使う)。
set(CORDMUX_WS_DIR "${CMAKE_CURRENT_LIST_DIR}")

include(FetchContent)
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD) # FetchContent_Populate を単独で呼ぶので、CMake 3.30 以降の非推奨の警告を出さない
endif()

# どれもリポジトリにコピーせず、GitHub のコミットの zip を URL と SHA256 で指定して取得する。git clone にしないのは、
# asio だけで約 325 MB あるためである。バージョンは、obs-studio 31.1.1 の obs-websocket 5.7.4 がビルドに使う
# obs-deps 2025-07-11 のもの (github.com/obsproject/obs-deps の deps.windows/60-asio.ps1 などが指定するコミット) に
# 合わせてある。
# ダウンロードが TLS の証明書の検証で失敗するときの設定は、CONTRIBUTING.md の「ローカルの環境に合わせたプリセット」
# にある。
FetchContent_Declare(
  cordmux_asio
  URL https://github.com/chriskohlhoff/asio/archive/03ae834edbace31a96157b89bf50e5ee464e5ef9.zip # asio-1-32-0
  URL_HASH SHA256=1dd456deb999a45246c85e856027d17a753bd3a08e32d79cb79c2a55b7c65d52
)
FetchContent_Declare(
  cordmux_websocketpp
  URL https://github.com/zaphoyd/websocketpp/archive/56123c87598f8b1dd471be83ca841ceae07f95ba.zip # 0.8.2
  URL_HASH SHA256=824a2a41aa8dc4eaefa07e8021b26c57a47fa6751302a159a967cf724709f23a
)
FetchContent_Declare(
  cordmux_json
  URL https://github.com/nlohmann/json/archive/9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03.zip # v3.11.3
  URL_HASH SHA256=42013e884a8a38d2a974619844a294de614dab4bc14e0ad23711ef4dacbb24c1
)

# 3 つともヘッダーだけのライブラリなので、add_subdirectory はしない (websocketpp 0.8.2 の CMakeLists.txt は古い
# cmake_minimum_required を書いていて、新しい CMake が受け付けない)。FetchContent_Populate でソースだけを取得し、
# インクルードディレクトリとして追加する。
FetchContent_GetProperties(cordmux_asio)
if(NOT cordmux_asio_POPULATED)
  FetchContent_Populate(cordmux_asio)
endif()
FetchContent_GetProperties(cordmux_websocketpp)
if(NOT cordmux_websocketpp_POPULATED)
  FetchContent_Populate(cordmux_websocketpp)
endif()
FetchContent_GetProperties(cordmux_json)
if(NOT cordmux_json_POPULATED)
  FetchContent_Populate(cordmux_json)
endif()

# target に、3 つのライブラリのインクルードディレクトリ、コンパイルの定義、警告の指定を追加する。
# listener.cpp は、呼び出し元ごとにパスの書き方が違う (src-obs/ws/ws.cmake は "ws/listener.cpp"、
# src-obs/tests/CMakeLists.txt は "${CMAKE_CURRENT_SOURCE_DIR}/../ws/listener.cpp") ので、この関数では追加せず、
# 呼び出し元が追加する。
function(cordmux_add_ws_deps target)
  # BEFORE を付け、SYSTEM は付けない。obs-deps のインクルードディレクトリ (src-obs/decode/decode.cmake が追加する)
  # にも websocketpp、asio、nlohmann/json が入っていて、何も指定しないと先に追加されたそちらが見つかり、ここで
  # 取得したものが使われない。SYSTEM を付けると、Visual Studio のジェネレーターはそのディレクトリを
  # ExternalIncludeDirectories に入れ、BEFORE を付けても AdditionalIncludeDirectories より後に探す (確認したのは
  # Visual Studio のジェネレーターだけで、ほかのジェネレーターでは確認していない)。
  target_include_directories(
    ${target} BEFORE PRIVATE
      "${cordmux_asio_SOURCE_DIR}/asio/include"
      "${cordmux_websocketpp_SOURCE_DIR}"
      "${cordmux_json_SOURCE_DIR}/include"
  )

  # obs-websocket 5.7.4 の CMakeLists.txt と同じ定義である。asio 1.32.0 と websocketpp 0.8.2 の組は、この 3 つが
  # そろっていないとコンパイルできない。
  target_compile_definitions(
    ${target} PRIVATE
      ASIO_STANDALONE
      $<$<PLATFORM_ID:Windows>:_WEBSOCKETPP_CPP11_STL_>
      $<$<PLATFORM_ID:Windows>:_WIN32_WINNT=0x0603>
  )

  # websocketpp 0.8.2 の hybi00 の処理が、MSVC の <xutility> の行に C4244 を出す (理由は src-obs/ws/listener.cpp の
  # 先頭のコメント)。この警告は listener.cpp にだけ指定して表示しない。パスは絶対パスで書く。呼び出し元ごとに
  # listener.cpp を別の相対パスで追加しているので、相対パスでは set_source_files_properties が別のファイルとして
  # 扱う。
  if(MSVC)
    set_source_files_properties("${CORDMUX_WS_DIR}/listener.cpp" PROPERTIES COMPILE_OPTIONS "/wd4244")
  endif()
endfunction()
