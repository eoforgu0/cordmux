# src-obs/platform

OS に依存する処理。cordmux.dll の中で Windows の API を呼ぶのは `platform_win.cpp` だけで、ほかのファイルは OS の名前を書かない。ほかの OS に広げるときは、`platform.h` の関数を実装した `platform_<OS>.cpp` を追加する。仕組みは docs/architecture.md の「Windows に依存する部分」にある。

## 主な型と関数

- `platform.h`: OS に依存する関数の宣言。パスは、どれも UTF-8 で受け取り、UTF-8 で返す。
  - WinSock の起動と終了 (`startup()`、`shutdown()`)。
  - トークンファイル (`write_session_file()`、`read_session_file()`) と、別の OBS のプロセスが動いているかの確認 (`current_process_identity()`、`is_process_alive()`)。
  - Vesktop のプロセスと設定ファイルの場所 (`find_vesktop_process()`、`vesktop_state_json_path()`)、Vesktop と Discord の実行ファイルの名前を見分ける関数 (`is_discord_client_exe_name()`)、ファイルの読み書き (`read_file_bytes()`、`write_file_atomic()`)。
  - 録画で使うフォルダー、ファイル、子プロセス、空き容量、時刻 (`make_directories()`、`open_file()`、`start_child()`、`free_space_bytes()` など)。record/ の `RecordPlatformOps` に入れて使う。
  - インストールとアンインストールで使うファイルの操作 (`delete_file()`、`list_directory()`、`copy_file_replace()` など)。
  - ファイルの場所をエクスプローラーで開く (`open_file_location()`)。
- `platform_win.cpp`: `platform.h` の Windows の実装。
- `os_probe.h`: Vesktop の状態、同梱の Vencord のファイル、録画の保存先の空き容量を、1 つのスレッドで一定の間隔で調べるワーカー (`OsProbe`)。ドックは `latest()` で結果を読む。
- `bundle_check.h`: 同梱の Vencord のファイル (data/vencord) とコピー元 (data/vencord-src) を比べる `compare_bundle()` と、state.json とバックアップを読む関数。
- `sha256.h`: SHA-256 (`sha256_hex()`、`Sha256`)。

## 依存

- `platform_win.cpp` は Windows の API (WinSock、シェル、ACL など) を使う。リンクするライブラリは src-obs/CMakeLists.txt と src-obs/ws/ws.cmake にある。
- `os_probe.h`、`bundle_check.h`、`sha256.h` は OS にアクセスしない。`bundle_check.h` は nlohmann/json を使う。
- platform/ はほかのフォルダーに依存しない (`wait_child()` の戻り値の型だけは record/record_types.h のものを使う)。
- test/lint/windows_api_confinement.py が、platform/ の外に Windows の名前がないことを確認する。

テストは src-obs/tests の `test_platform_*.cpp` にある。Windows の API を実際に呼ぶテストは、Windows でだけコンパイルされる。
