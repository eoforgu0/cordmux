# SPDX-License-Identifier: Apache-2.0

# src-obs の C++ の単体テストをビルドする CMake のプロジェクト。cordmux.dll とは別のプロジェクトで、実行ファイルは 2 つある。
#   align_tests: libobs も Qt も使わない単体テスト (src-obs/tests の直下の test_*.cpp)
#   decor_tests: 名前ラベルと話している人の光の描画のテスト (Qt6::Gui を使う。src-obs/tests/decor の下)
# CORDMUX_BUILD_PREVIEW_TOOLS を ON にすると、プレビューツール (ui_preview と decor_preview)、名前ラベルの調整ツール
# (label_tuner)、ドックを操作するテスト (dock_drive) もビルドする。ビルドと実行の方法は docs/testing.md の
# 「C++ の単体テスト」にある。プレビューツールのスクリプトが既定で使うプリセット tests-x64-local の作り方は、
# docs/testing.md の「プレビューツール」にある。

cmake_minimum_required(VERSION 3.28)
project(cordmux_align_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

file(GLOB CORDMUX_ALIGN_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../align/*.cpp")
file(GLOB CORDMUX_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

add_executable(align_tests
  ${CORDMUX_ALIGN_SOURCES}
  ${CORDMUX_TEST_SOURCES}
  "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
)

# ヘッダーは、cordmux.dll と同じく src-obs からのパス ("align/xxx.h" など) でインクルードする。
target_include_directories(align_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/.."
  "${CMAKE_CURRENT_SOURCE_DIR}"
)

if(MSVC)
  # C5285 は doctest.h が std::tuple を特殊化していることへの警告で、こちらのコードでは消せない。align/*.cpp と
  # test_*.cpp は /W4 で警告を出さない。
  target_compile_options(align_tests PRIVATE /W4 /utf-8 /permissive- /wd5285)
else()
  target_compile_options(align_tests PRIVATE -Wall -Wextra)
endif()

# ---- decode/ ----
# align_tests に decode/ のソースと FFmpeg のライブラリを追加する。src-obs/CMakeLists.txt も同じ decode.cmake を使うので、
# FFmpeg の探し方は 1 か所にある。
include("${CMAKE_CURRENT_SOURCE_DIR}/../decode/decode.cmake")
cordmux_add_decode(align_tests)

# テストがテストデータ (src-obs/tests/fixtures の下) を、作業フォルダーにかかわらず見つけられるようにする (ctest は作業
# フォルダーを変えることがあるため)。
target_compile_definitions(
  align_tests PRIVATE CORDMUX_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

# cordmux.dll が読み込む FFmpeg の 8 つの DLL が align_tests.exe と同じフォルダーにないと、align_tests.exe を起動できない
# (DLL を起動のときに読み込む形でリンクしているため)。DLL の一覧と理由は docs/release.md の「配布用の zip の中身」に
# ある。コピーする一覧は、cordmux.dll の install(FILES ...) と同じく src-obs/cmake/cordmux_ffmpeg_runtime.cmake の 1 か所に
# あり、ここには並べない。下の record/ が使う avformat などの DLL も、この 1 回でコピーする。
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/cordmux_ffmpeg_runtime.cmake")
cordmux_copy_ffmpeg_runtime(align_tests)

# ---- diag/ ----
# align_tests に diag/ のソース (diag_log.cpp) を追加する。src-obs/CMakeLists.txt も同じ diag.cmake を使う。libobs にも Qt
# にも依存しないので、リンクするライブラリはない。
include("${CMAKE_CURRENT_SOURCE_DIR}/../diag/diag.cmake")
cordmux_add_diag(align_tests)

# ---- ws/ ----
# test_ws_*.cpp (上の test_*.cpp に含まれる) は 127.0.0.1 で実際に待ち受け、生の TCP と websocketpp のクライアントから
# 接続するので、asio、websocketpp、nlohmann/json が必要である。取得する宣言は src-obs/ws/ws_deps.cmake の
# cordmux_add_ws_deps() にあり、src-obs/ws/ws.cmake も同じ関数を使う。
include("${CMAKE_CURRENT_SOURCE_DIR}/../ws/ws_deps.cmake")

# listener.cpp と platform_win.cpp を align_tests に追加する。platform_win.cpp の関数の多くは、test_platform_*.cpp が
# 実際に呼んで確認する。is_process_alive が再利用された pid を見分けることと、ほかの Windows のアカウントから
# session.json を読めないことは、単体テストでは確認していない。
target_sources(align_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../ws/listener.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../platform/platform_win.cpp"
)

cordmux_add_ws_deps(align_tests)

# test_ws_listener.cpp も ws_test_helpers.h を通して websocketpp を使うので、listener.cpp と同じ C4244 が出る (理由は
# src-obs/ws/ws_deps.cmake のコメント)。cordmux_add_ws_deps は listener.cpp にだけ指定するので、ここで
# test_ws_listener.cpp にも指定する。align_tests は /WX を付けていないので必須ではないが、cordmux.dll のビルドと同じ状態で
# テストする。
if(MSVC)
  set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/test_ws_listener.cpp" PROPERTIES COMPILE_OPTIONS "/wd4244")
endif()

if(WIN32)
  target_link_libraries(align_tests PRIVATE ws2_32)
endif()

# ---- ledger/ ----
# align_tests に表示の規則 (src-obs/ledger) のソースを追加する。src-obs/CMakeLists.txt も同じ ledger.cmake を使う。表示の
# 規則は libobs にも Qt にも依存しないので、リンクするライブラリはない (表示の規則を、Qt を起動せずに確認するため)。
include("${CMAKE_CURRENT_SOURCE_DIR}/../ledger/ledger.cmake")
cordmux_add_ledger(align_tests)

# ---- install/ ----
# align_tests に install/ のソースを追加する。src-obs/CMakeLists.txt も同じ install.cmake を使う。install/ は libobs にも
# Qt にも依存せず、nlohmann/json のインクルードディレクトリは上の cordmux_add_ws_deps が追加しているので、リンクする
# ライブラリはない。
include("${CMAKE_CURRENT_SOURCE_DIR}/../install/install.cmake")
cordmux_add_install(align_tests)

# ---- record/ ----
# align_tests に、録画の純粋関数の部分 (ヘッダー) と I/O の部分 (.cpp 3 つ) を追加する。src-obs/CMakeLists.txt も同じ
# record.cmake の 2 つの関数を呼ぶ。純粋関数の部分のテスト (test_record_*.cpp) は、上の test_*.cpp に含まれる。
#
# I/O の部分もここに入れるのは、実際の ffmpeg と ffprobe を使うテストが、4 通りの録画ログのテストデータを、セグメント
# ファイルを書く実際の処理に通し、録画ファイルの作成の引数で 1 つのファイルにまとめてから、ffprobe で確認するためである。書く処理を
# 外すと、このテストは、手で作ったファイルを確認することになる。I/O の部分が使うのは avformat と RecordPlatformOps
# だけで、libobs は使わないので、この実行ファイルに入れられる。platform_win.cpp は上の ws/ で追加してある。
include("${CMAKE_CURRENT_SOURCE_DIR}/../record/record.cmake")
cordmux_add_record_core(align_tests)
cordmux_add_record_io(align_tests)

# avformat-61.dll と、それが読み込む srt.dll と librist.dll は、上の cordmux_copy_ffmpeg_runtime が 8 つの DLL と一緒に
# コピーする。

# ---- dock/ ----
# align_tests に追加するのは、ドックのうち Qt を使わない部分 (cordmux_add_dock_observation) だけである。ドックの
# ウィジェットは Qt を使うので、この実行ファイルには入れない (表示の規則と、その入力を作る処理を、Qt を起動せずに
# 確認するため)。src-obs/CMakeLists.txt は、同じ dock.cmake の cordmux_add_dock() を呼ぶ。pipeline/pipeline.h が使う
# ws:: の実装 (listener.cpp) は上の ws/ で追加してあるので、そろっている (PipelineSnapshot は値をコピーするだけの
# 構造体で、libobs に依存する Pipeline の実装は必要ない)。
include("${CMAKE_CURRENT_SOURCE_DIR}/../dock/dock.cmake")
cordmux_add_dock_observation(align_tests)

# ---- decor_tests ----
# 名前ラベルと話している人の光の描画だけをテストする 2 つ目の実行ファイル。align_tests と分けるのは、表示の規則を Qt を
# 起動せずに確認することを、構成で保証するためである (同じ実行ファイルに Qt6::Gui を入れると、Qt に依存する判断が
# 混ざっても気付けない)。
#
# テストは src-obs/tests/decor の下に置く。上の test_*.cpp は src-obs/tests の直下だけを見るので、フォルダーを分ければ
# align_tests に入らない。
include("${CMAKE_CURRENT_SOURCE_DIR}/../sources/decor_paint.cmake")

file(GLOB CORDMUX_DECOR_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/decor/*.cpp")
add_executable(decor_tests ${CORDMUX_DECOR_TEST_SOURCES})

target_include_directories(
  decor_tests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_CURRENT_SOURCE_DIR}"
)

# decor_paint.cpp と Qt6::Gui を追加する (cordmux.dll には src-obs/sources/sources.cmake が同じ関数で追加する)。
cordmux_add_decor_paint(decor_tests)

# テストに使うフォントと、期待する画像のフォルダー (どちらも src-obs/sources/decor_paint.cmake が決める)。
target_compile_definitions(
  decor_tests
  PRIVATE CORDMUX_DECOR_FONT_DIR="${CORDMUX_DECOR_FONT_DIR}"
          CORDMUX_DECOR_SNAPSHOT_DIR="${CORDMUX_DECOR_SNAPSHOT_DIR}"
)

if(MSVC)
  target_compile_options(decor_tests PRIVATE /W4 /utf-8 /permissive- /wd5285)
else()
  target_compile_options(decor_tests PRIVATE -Wall -Wextra)
endif()

# Qt6Core.dll と Qt6Gui.dll と、ウィンドウを開かない minimal のプラットフォームプラグインが decor_tests.exe と同じ
# フォルダーにないと、起動できない (align_tests の FFmpeg の DLL と同じ)。Qt はプラットフォームプラグインを、実行
# ファイルと同じフォルダーの platforms/ から読むので、そこに置く。
add_custom_command(
  TARGET decor_tests
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::Core>" "$<TARGET_FILE:Qt6::Gui>"
          "$<TARGET_FILE_DIR:decor_tests>"
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:decor_tests>/platforms"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::QMinimalIntegrationPlugin>"
          "$<TARGET_FILE_DIR:decor_tests>/platforms"
)

# ---- プレビューツール (ui_preview と decor_preview)、調整ツール (label_tuner)、ドックを操作するテスト (dock_drive) ----
# 既定ではビルドしない。どれも Qt の内部向けのモジュール (Qt6::GuiPrivate) を使い、CI の Visual Studio 2022 で
# ビルドできるかを確認していない。既定でビルドすると、ツールのビルドが失敗したときに CI の単体テストも失敗する。
# ビルドするときは、cmake --preset tests-x64 に -DCORDMUX_BUILD_PREVIEW_TOOLS=ON を付けるか、各自のプリセットで
# ON にする (書き方は CONTRIBUTING.md の「ローカルの環境に合わせたプリセット」)。プレビューツールのスクリプト
# (run_ui_preview.py) が既定で使うプリセット tests-x64-local の作り方は、docs/testing.md の
# 「プレビューツール」にある。別の CMake のプロジェクトに分けると、OBS のライブラリの探し方を 2 か所に持つことに
# なるので、このプロジェクトに置く。
option(CORDMUX_BUILD_PREVIEW_TOOLS "プレビューツール (ui_preview、decor_preview)、label_tuner、dock_drive をビルドする" OFF)
if(CORDMUX_BUILD_PREVIEW_TOOLS)
# ---- ui_preview ----
# ドック (src-obs/dock/dock_widget.cpp) を状態ごとに描画して PNG に書き出すプレビューツール。OBS 31 のテーマを OBS と
# 同じ手順で適用し、ウィンドウを開かない (OS のウィンドウを作らない、プレビュー専用の Qt のプラットフォームプラグインを
# 使う)。合否を持たないので、add_test には追加しない。使い方は src-obs/tests/ui_preview/README.md、ビルドに追加する
# ファイルは src-obs/tests/ui_preview/ui_preview.cmake にある。
include("${CMAKE_CURRENT_SOURCE_DIR}/ui_preview/ui_preview.cmake")
cordmux_add_ui_preview()

# ---- decor_preview ----
# 合成のソースの名前ラベルと光 (src-obs/sources/composite_label.cpp) を、製品と同じ配置で 1920 x 1080 の
# キャンバスに並べて、PNG に書き出すプレビューツール。ウィンドウを開かない。合否を持たないので、add_test には追加しない。
# 使い方は src-obs/tests/decor_preview/README.md、ビルドに追加するファイルは
# src-obs/tests/decor_preview/decor_preview.cmake にある。
include("${CMAKE_CURRENT_SOURCE_DIR}/decor_preview/decor_preview.cmake")
cordmux_add_decor_preview()

# ---- label_tuner ----
# 合成のソースの名前ラベルと光の見た目の値を、つまみを動かしながら決める Qt Widgets のツール。プレビューは decor_preview の
# 合成のソースの見本と同じコンポーネント (製品の描画、置く物の計画、CPU で重ねる関数) で描く。ウィンドウを開くのは利用者が起動した
# ときだけで、--shot はウィンドウを開かずに画像を書き出す。合否を持たないので、add_test には追加しない。使い方は
# src-obs/tests/label_tuner/README.md、ビルドに追加するファイルは src-obs/tests/label_tuner/label_tuner.cmake にある。
include("${CMAKE_CURRENT_SOURCE_DIR}/label_tuner/label_tuner.cmake")
cordmux_add_label_tuner()

# ---- dock_drive ----
# ドックと設定ウィンドウを、ウィンドウを開かずに作り、入力と状態の変化を与えて、押し間違い、ウィンドウの移り変わり、
# フォーカスを数えるテスト。合否を持つが、1 回に 10 分かかるシナリオを含むので、add_test には追加しない。ui_preview と
# 同じもの (プレビュー専用の Qt のプラットフォームプラグイン、テーマ、ini、場面) を使うので、同じ
# CORDMUX_BUILD_PREVIEW_TOOLS の中に置く。使い方は src-obs/tests/dock_drive/README.md にある。
include("${CMAKE_CURRENT_SOURCE_DIR}/dock_drive/dock_drive.cmake")
cordmux_add_dock_drive()
endif()

enable_testing()
add_test(NAME align_tests COMMAND align_tests)
add_test(NAME decor_tests COMMAND decor_tests)
