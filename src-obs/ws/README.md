# src-obs/ws

送信側 (Vesktop の中で動く cordmux) と WebSocket で接続する部分。127.0.0.1 で接続を待ち受け、ワイヤー形式のバイナリメッセージ (映像と音声) と制御用の JSON を受け取って、コールバックに渡す。送信側への制御用の JSON もここから送る。形式は docs/wire_format.md に、接続の認証は docs/architecture.md の「接続の認証と制限」にある。

## 主な型と関数

- `Listener` (`listener.h`): `try_open()` で待ち受けを始め、ポート番号とトークンを session.json に書く。受け取ったメッセージは、`on_video_frame` や `on_configure` などのコールバックに io_thread から渡す。送信側には `send_recording()`、`send_media()`、`send_limits()`、`send_main_account()`、`send_receive_scope()`、`send_move_to_main_call()`、`send_leave_call()`、`request_keyframe()`、`send_stream_check()` で送る。接続の状態と数は `stats()` で読む。
- `control_messages.h`: 制御用の JSON の型 (`HelloMessage`、`ConfigureMessage` など) と、組み立てる関数 (`build_*`) と読む関数 (`parse_*`)。読む関数は例外を投げず、壊れた入力には nullopt を返す。
- `wire.h`: バイナリメッセージの 14 バイトのヘッダーを読む `decode_frame_header()` と、作る `encode_frame()`。
- `secret.h`: トークンを作る `generate_secret_hex()` と、定数時間で比べる `constant_time_equals()`。
- `log_sink.h`: ログの出力先の型 `LogSink` と、文字列にトークンかポート番号が含まれるかを調べる `text_reveals_secret_or_port()`。
- `ws.cmake`、`ws_deps.cmake`: cordmux.dll に listener.cpp と外部のライブラリを追加する。テストの実行ファイルには、`ws_deps.cmake` の `cordmux_add_ws_deps()` で外部のライブラリを追加する。

`Listener` を作って持つのは plugin-main.cpp で、pipeline/ の `Pipeline` がそのコールバックに自分の関数を登録する。

## 依存

- standalone asio 1.32.0、websocketpp 0.8.2、nlohmann/json 3.11.3。ビルドのときに GitHub から取得する (コミットと SHA256 は `ws_deps.cmake` にある)。
- platform/ (session.json の読み書きと、別の OBS のプロセスが動いているかの確認)。`Listener` はこれらの関数を `PlatformOps` で受け取るので、ws/ のコードは Windows の API を直接呼ばない。
- libobs にも Qt にも依存しない。

テストは src-obs/tests の `test_ws_*.cpp` にある。`test_ws_listener.cpp` は実際に 127.0.0.1 で待ち受け、`ws_test_helpers.h` のクライアントから接続する。テスト用の `PlatformOps` を使うので、実際の session.json は読み書きしない。
