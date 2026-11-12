# transport/

送信側から受信側 (cordmux.dll) への WebSocket の接続を扱う部分。接続の手順、再接続、背圧、制御用の JSON のエンコードとデコードを持つ。ワイヤー形式は docs/wire_format.md にある。

## ファイル

- `client.ts`: 接続の手順を表す、純粋関数の状態機械 (`createClient()` と `transition()`)。イベントを受け取り、次の状態と、呼び出し元が実行する処理 (送る、閉じる、トークンファイルを読み直す、タイマーを設定する) を返す。
- `socket.ts`: `client.ts` の状態機械を、実際の WebSocket とタイマーで動かす (`createSocket()`)。映像と音声のメッセージは `sendBinary()`、制御用の JSON は `sendControl()` で送る。受け取った `keyframe_request`、`stream_check`、`recording`、`media`、`limits` は `SocketHandlers` で呼び出し元に渡す。
- `control.ts`: 制御用の JSON のメッセージの型 `ControlMessage` と、`encodeControlMessage()`、`decodeControlMessage()`。デコードは例外を投げず、形が不正なメッセージには `undefined` を返す。
- `backoff.ts`: 再接続を試すまでの待ち時間を返す純粋関数 `reconnectDelayMs()`。
- `backpressure.ts`: 送信のキューがたまったときに、映像のフレームを捨てるかを決める純粋関数 `applyBackpressure()`。音声は捨てない。

## 依存

- どのファイルも Vencord を import しない。`tsconfig.json` の `lib` は `ES2022` で DOM の型を持たないので、`socket.ts` は WebSocket の型を使わずに、使う部分だけの `WebSocketLike` を定義する。
- `socket.ts` の `SocketDeps` で、WebSocket を作る関数、トークンファイルを読む関数、タイマーを受け取る。製品では `index.ts` が、Vesktop のレンダラーの WebSocket と、`native.ts` の `readSession()` を渡す。テスト用の送信側 (`test/tools/harness/sender.ts`) も `createSocket()` を使う。単体テストは、これらをテスト用の実装に置き換えて、Node の vitest で実行する (`tests/transport_*.test.ts`)。
- 確立したときに configure、facts、profiles を送り直す処理と、切れたときの後片付けは、`SocketHandlers` の `onEstablished` と `onClosed` を受けて、`index.ts` がする。
