# src-obs

OBS の中で動く cordmux (受信側)。C++ と Qt で書いたプラグイン cordmux.dll と、その単体テストがある。仕組みは docs/architecture.md にある。

## 入口

- `plugin-main.cpp`: `obs_module_load` と `obs_module_unload`。settings.json を読み、ws/ の `Listener`、pipeline/ の `Pipeline`、ドック (dock/) を作って結び付ける。cordmux.dll のログの行は、すべてこのファイルの `guard_and_log` を通る。
- `CMakeLists.txt`: cordmux.dll のビルド。各フォルダーの `.cmake` を読み込み、そのフォルダーのファイルを追加する。

## フォルダー

| フォルダー | 中身 |
|---|---|
| align/ | 送信側の映像のタイムスタンプを、推定した周期のグリッドに合わせる |
| bundle/ | 診断情報の zip を作る |
| config/ | 設定 (settings.json) の値と、割り当て表 (どの人をどの枠に載せるか) |
| data/ | 配布用の zip の data に入れるファイル (画面の文言の ini) |
| decode/ | H.264 のデコード |
| diag/ | ログの出力先 |
| dock/ | ドック、設定ウィンドウ、参加者の設定ウィンドウ (Qt) |
| install/ | Vesktop の設定 (state.json の vencordDir) の書き換えと、cordmux の更新とアンインストール |
| ledger/ | 表示の規則 (状態からドックに表示するものを決める純粋関数) と、画面の文言のキー |
| pipeline/ | 受信した映像と音声を、デコードとタイムスタンプの調整を通して合成のソースに渡す。録画への受け渡しも受け持つ |
| platform/ | OS に依存する処理 (ファイルの読み書き、プロセス、フォルダーの場所、トークンファイルのアクセス権など) |
| record/ | 録画。セグメントファイルと録画ログを書き、録画ファイルを作成する |
| sources/ | OBS のソース (合成のソース) |
| tests/ | 単体テスト (doctest)、プレビューツール (ui_preview と decor_preview)、ドックを操作するテスト (dock_drive) |
| ws/ | 送信側との WebSocket の接続 |
| cmake/ | ビルドの設定。名前が cordmux_ で始まるファイルのほかは、obs-plugintemplate からコピーしたファイルである |
| build-aux/ | フォーマッターを実行するスクリプト (obs-plugintemplate からコピーしたファイル) |

`.clang-format` と `.gersemirc` も、obs-plugintemplate からコピーしたファイルである。外部からコピーしたファイルは test/lint/vendored.txt に並べてある。

Windows の API を呼ぶコードは platform/ にだけ置く。platform/ の外 (tests/ を除く) に Windows の API の名前がないことは、test/lint/windows_api_confinement.py がチェックする。

## 依存

- libobs、obs-frontend-api、Qt 6、FFmpeg (obs-deps のもの)。バージョンとハッシュは buildspec.json にあり、最初の configure で src-obs/.deps に取得する。更新の手順は docs/release.md の「OBS と obs-deps」にある。
- ビルドに使う OBS は、対応する OBS の下限以下のバージョンにする。新しいバージョンの libobs にだけある関数を使うと、古い OBS で読み込めなくなるためである。
- websocketpp、standalone asio、nlohmann/json。ビルドのときに取得する (ws/ws_deps.cmake)。
- 単体テストの doctest (tests/doctest.h)。

プラグインの名前とバージョンも buildspec.json にある。

## ビルドとテスト

必要なツールと、ビルドと単体テストのコマンドは、CONTRIBUTING.md の「必要なもの」「OBS のプラグインのビルド」「C++ の単体テスト」にある。

CMakePresets.json のプリセットは、windows-x64 (ローカルのビルド) と windows-ci-x64 (CI のビルド。警告をエラーにする) の 2 つである。obs-plugintemplate の macOS と Linux のプリセットは置いていない (cmake/macos と cmake/linux は、コピーしたまま残してある)。

単体テストは src-obs/tests にある別の CMake のプロジェクトなので、src-obs の CMakePresets.json を継承できず、プリセット tests-x64 を tests/CMakePresets.json に持つ。テストの中身は docs/testing.md の「C++ の単体テスト」にある。
