# src-vencord/encode

画面共有の映像を、参加者ごとに WebCodecs で H.264 にエンコードする部分。エンコードの流れは docs/architecture.md の「送信側」に、タイムスタンプの扱いは同じ文書の「タイムスタンプの合わせ方」にある。

## 入口

| ファイル | 中身 |
|---|---|
| `session.ts` | `createEncodeSession()`。1 つのストリームのエンコードのセッション。段階の確保、ウォームアップ、キーフレーム、上限の変更、黒埋め用のフレームを扱う |
| `ladder.ts` | フォールバックの 5 つの段階 `LadderStage` と、段階ごとの設定、H.264 の Level の選び方 |
| `limits.ts` | 画質の上限 `EncodeSessionLimits` と、ワイヤー形式からの変換 `limitsFromWire()`、下げた上限だけを適用する `tightenLimits()` |
| `serial.ts` | エンコーダの確保を 1 つずつ順に実行する `SerialGate` |
| `warmup.ts` | ウォームアップの計画 |
| `annexb.ts` | キーフレームに SPS と PPS を入れる `applyParameterSetInsurance()` |
| `clock_map.ts` | メディアのタイムスタンプを、トラックごとの定数で送信側の時計に換算する `mapMediaTime()`。`audio/chunk.ts` も使う。定数は最初の到着で決め、2 秒の期間ごとに詰まりのない到着が 3 つあれば下げ、メディアの時計が替わったとみなしたときだけ上げる (決め直す) |
| `sender_clock.ts` | 送信側の時計 `senderClockUs()` (`performance.timeOrigin + performance.now()`)。ワイヤー形式の `timestamp_us` と hello と pong の `clock_us` の時計 |
| `frame_source.ts` | フレームを 1 つずつ受け取るインターフェイス `VideoFrameSource` |
| `track_source.ts` | `VideoFrameSource` の製品の実装 `createVideoTrackSource()` (MediaStreamTrackProcessor) |
| `webcodecs_types.ts` | WebCodecs の型のうち、使う部分だけの構造型 |
| `clock_origin_log.ts` | 映像と音声のタイムスタンプの原点をログに書くかを決める `clockOriginLogLine()` |
| `latency_span.ts` | 映像の段階ごとの所要時間の足し上げと、`latency stream N:` の行の書式 (`latencyLine()`) |
| `playout_diag.ts` | 画面共有の音声と映像の再生の遅延を、ログに書く行を作る `playoutReport()` など。行の書式と読み方は `playoutReport()` のコメントにある |

## 依存

- `track_source.ts` はブラウザーにしかない MediaStreamTrackProcessor を使うので、`tsconfig.json` の `exclude` でリポジトリのルートの tsc から外し、Vencord の `pnpm test:ts` でチェックする (`src-vencord/README.md`)。
- ほかのファイルは Vencord も DOM の型も使わない。VideoEncoder、時計、フレームの縮小は、呼び出し元 (`index.ts` と、テスト用の送信側の `test/tools/harness/sender.ts`) が `EncodeSessionDeps` で渡すので、リポジトリのルートの tsc と vitest でテストできる (`tests/encode_*.test.ts`)。
- このフォルダーのほかに読み込むのは、`core/wire.ts`、`adapter/types.ts` の型、`audio/chunk.ts` と `core/stream_check.ts` の型 (`playout_diag.ts`)、`transport/control.ts` の型 (`limits.ts`) だけである。

## 遅れの行

60 秒ごとの `playout` の行の直後に、`latency stream N:` の行がある。段ごとの平均 (`avg`) と最大 (`max`) で、`n` は `read/enc/prep/send` の数である。

- `read`: トラックのフレームのタイムスタンプをシステム時刻に変換した値と、読み出した時刻の差 (これまででいちばん早く読めたときを 0 とした読み出しの遅れ)
- `enc`: エンコーダに渡してから出力が出るまで
- `prep`: 出力を受け取ってから `sink.sendVideo` を呼ぶ直前まで
- `send`: `sink.sendVideo` の呼び出し (ヘッダーとの連結と WebSocket の send の呼び出し)

送信の待ち行列と転送の時間は入らず、受信側の `recv_` (src-obs/pipeline/README.md) に出る。
