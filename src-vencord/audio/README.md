# audio/

受信した音声を、エンコードせずに PCM のまま受信側に送る部分。

Discord の DAVE (エンドツーエンドの暗号化) が WebRTC のレシーバーの encoded transform を使うので、送信側はエンコードされたままの Opus を取り出せず、デコードした後の音声しか取得できない。Opus で送るにはエンコードし直すことになり、録画の音質が落ちる。PCM の帯域は 1 ストリームあたり 384 KB/s (最大 12 ストリームで約 4.6 MB/s) で、同じ PC の中の WebSocket では問題にならない。

形式は f32 のインターリーブ、48 kHz、2 チャンネルで、480 サンプル (10 ms) ごとのチャンクにして送る。形式とタイムスタンプの意味は、docs/wire_format.md の「音声のペイロード」と「timestamp_us」にある。

## ファイル

- `audio_source.ts`: 音声の断片を 1 つずつ取り出すインターフェイス `AudioSource`。
- `track_source.ts`: `AudioSource` の製品の実装 `createAudioTrackSource()`。`MediaStreamTrackProcessor` でトラックから `AudioData` を読む。
- `chunk.ts`: 断片を 480 サンプルのチャンクに分け、チャンクのタイムスタンプを決める純粋関数 (`createAudioChunker()` と `pushAudio()`)。

## 依存

- `chunk.ts` は、メディアのタイムスタンプを送信側の時計に換算するのに `encode/clock_map.ts` を使う (映像と同じ規則)。
- `track_source.ts` はブラウザーの実行時にしかない API を使うので、リポジトリのルートの tsc の対象から外している (`tsconfig.json` の `exclude`)。`tests/audio_track_source.test.ts` は、`MediaStreamTrackProcessor` を `vi.stubGlobal` でテスト用の実装に置き換えてテストする。
- どのファイルも Vencord を import しない。
