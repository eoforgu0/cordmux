# src-obs/bundle

診断情報の zip を作る。設定ウィンドウの「バージョン情報」のページの [診断情報をエクスポート...] を押すと、zip を 1 つ書き出す (ドックのメニューの同じ項目は、設定ウィンドウをそのページで開いてから同じ処理を始める)。zip には manifest.json、plugin.log、plugin-stats.log、discord.log、discord-stats.log、settings.json の 6 つのファイルが入る。中身は docs/architecture.md の「診断情報」にある。

このフォルダーはダイアログを表示しない。保存のダイアログ、失敗を伝えるダイアログ、結果の表示は、呼び出し元の dock/dock_poll_export.cpp と dock/settings_window.cpp が受け持つ。

## 主な型と関数

- `export_bundle.h`: `prepare_bundle()` (伏せる、確認する、zip を作る)、`write_bundle()` (選ばれた場所に書く)、`default_export_directory()` (保存のダイアログで最初に開くフォルダー)。トークンが残ったファイルは、`leave_out_revealing_entries()` が除いた理由の 1 行に置き換え、ほかのファイルはそのまま書き出す。置き換えた後の全文にまだトークンが残っていれば、zip を書かない。
- `bundle_files.h`: zip に入れる 5 つのファイルを作る `build_bundle_entries()` と、トークンの 2 段階のチェックの型。1 段目の `RedactFn` (cordmux.dll では `make_redact()` で作る) がトークンと秘密の値を伏せ、2 段目の `RevealFn` が、伏せた後にトークンが残っていないかを確認する。
- `bundle_manifest.h`: manifest.json を作る `build_manifest_json()`。キーも値も英語で書く。バージョンの一覧 (`versions`) に何を入れるかは dock/dock_poll_export.cpp が決める (Windows、GPU、FFmpeg のバージョンは入れていない)。
- `zip_writer.h`: 無圧縮の zip を作る `build_zip_stored()`。CRC-32 も自分で計算するので、zlib に依存しない。時刻を引数で受け取るので、同じ入力からは同じバイト列の zip ができる。

トークンを知っているのは plugin-main.cpp だけで、伏せる関数と確認する関数は plugin-main.cpp が作ってドックに渡す。ポート番号はチェックしない (理由は `bundle_files.h` の `RevealFn` のコメントにある)。

## 依存

- `zip_writer.h`、`bundle_manifest.h`、`bundle_files.h`、`export_bundle.h` はヘッダーだけで、libobs にも Qt にも依存しない。`bundle_manifest.h` は nlohmann/json と pipeline/dummy_interval.h を使う。
- `export_bundle.cpp` は Qt の QFile、QDir、QStandardPaths を使う。ウィジェットには触らないので、どのスレッドから呼んでもよい。

テストは src-obs/tests の `test_bundle_zip_writer.cpp`、`test_bundle_files.cpp`、`test_bundle_leave_out.cpp` にある。
