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
| `clock_map.ts` | メディアのタイムスタンプを、トラックごとの定数で送信側の時計に換算する `mapMediaTime()` (見積もりの換算)。`audio/chunk.ts` も使う。映像のタイムスタンプが描く予定の時刻でないときと、音声のタイムスタンプの基準が分からないときに使う。定数は最初の到着で決め、2 秒の期間ごとに詰まりのない到着が 3 つあればすぐ下げ、直近 20 秒のどの期間でも差が上がったままなら少しずつ上げ、メディアの時計が替わったとみなしたときは決め直す |
| `video_time.ts` | 映像の `VideoFrame.timestamp` を送信側の時計に換算する `mapVideoTime()`。タイムスタンプが WebRTC の描く予定の時刻 (TimeTicks の値) かを、読んだ時刻との差 (先行) でストリームの途中も見分け続け、そうなら見積もりなしに換算し、そうでなければ `clock_map.ts` に戻る |
| `sender_clock.ts` | 送信側の時計の決め方 `SenderTimebase` (`qpc` はページの原点の TimeTicks、`page` は `performance.timeOrigin` を原点にする) と、今の値 `timebaseNowUs()`。ワイヤー形式の `timestamp_us` と hello と pong の `clock_us` の時計 |
| `ticks_origin.ts` | ページの原点の TimeTicks を、Vesktop のメインのプロセスの `process.hrtime` (`native.ts` の `hrtimeNs()`) を `performance.now()` ではさんで求める `measureTicksOrigin()`。QueryPerformanceCounter の経路でないときと、誤差の上限が 1 ms を超えるときは値を返さない |
| `frame_source.ts` | フレームを 1 つずつ受け取るインターフェイス `VideoFrameSource` |
| `track_source.ts` | `VideoFrameSource` の製品の実装 `createVideoTrackSource()` (MediaStreamTrackProcessor) |
| `webcodecs_types.ts` | WebCodecs の型のうち、使う部分だけの構造型 |
| `clock_origin_log.ts` | 映像と音声のタイムスタンプの原点をログに書くかを決める `clockOriginLogLine()` |
| `clock_lag.ts` | 画面共有の映像と音声の「受け取った時刻 - 換算した時刻」の 5、50、95 パーセンタイルと、`clock lag stream N:` の行の書式 (`clockLagLine()`) |
| `latency_span.ts` | 映像の段階ごとの所要時間の足し上げと、`latency stream N:` の行の書式 (`latencyLine()`) |
| `playout_diag.ts` | 画面共有の音声と映像の再生の遅延を、ログに書く行を作る `playoutReport()` など。行の書式と読み方は `playoutReport()` のコメントにある |
| `av_diag.ts` | 画面共有の音声と映像の、受け取る側の待ちの差、音声の出力の遅れ、読み出しの欠落を 1 秒ごとに集め、`av diag stream N:` の行にする `avDiagInterval()` と `avDiagReport()` など (下の「受け取りの行」) |

## 依存

- `track_source.ts` はブラウザーにしかない MediaStreamTrackProcessor を使うので、`tsconfig.json` の `exclude` でリポジトリのルートの tsc から外し、Vencord の `pnpm test:ts` でチェックする (`src-vencord/README.md`)。
- ほかのファイルは Vencord も DOM の型も使わない。VideoEncoder、時計、フレームの縮小は、呼び出し元 (`index.ts` と、テスト用の送信側の `test/tools/harness/sender.ts`) が `EncodeSessionDeps` で渡すので、リポジトリのルートの tsc と vitest でテストできる (`tests/encode_*.test.ts`)。
- このフォルダーのほかに読み込むのは、`core/wire.ts`、`adapter/types.ts` の型、`audio/chunk.ts` と `core/stream_check.ts` の型 (`playout_diag.ts`)、`transport/control.ts` の型 (`limits.ts`) だけである。`audio/track_source.ts` は、`MediaStreamTrackProcessor` の数を読むのに `av_diag.ts` を使う。

## 遅れの行

60 秒ごとの `playout` の行の直後に、`latency stream N:` の行がある。段ごとの平均 (`avg`) と最大 (`max`) で、`n` は `read/enc/prep/send` の数である。

- `read`: トラックのフレームのタイムスタンプを送信側の時計に換算した値と、読み出した時刻の差 (詰まりのない読み出しを 0 とした読み出しの遅れ。負の値は 0 として数える)
- `enc`: エンコーダに渡してから出力が出るまで
- `prep`: 出力を受け取ってから `sink.sendVideo` を呼ぶ直前まで
- `send`: `sink.sendVideo` の呼び出し (ヘッダーとの連結と WebSocket の send の呼び出し)

送信の待ち行列と転送の時間は入らず、受信側の `recv_` (src-obs/pipeline/README.md) に出る。

`latency` の行の直後に、`clock lag stream N:` の行がある。映像 (`v.`) と音声 (`a.`) の「受け取った時刻 - 換算した時刻」の 5、50、95 パーセンタイル (`p5`、`p50`、`p95`。ms で、負の値も残す) と、足した数 (`n`) である。映像が描く予定の時刻で正確に換算されている間 (`video time stream N: time=exact` の行の後) は、映像の値は負で、WebRTC がフレームを描く予定の時刻よりどれだけ前に渡したか (先行。描画の遅延の 10 ms 前後) を表す。音声の値は、取り出してから読むまでの遅れ (1 ms 未満) である。見積もりの換算 (`clock_map.ts`) では、定数が正しければ、`p50` は取り出しの遅れ (数 ms) になる。定数が低いまま残ると、その分だけ大きくなる (受信を始めた直後に映像のタイムスタンプが先に出た分が戻らないと、映像の `p50` が 150 ms 前後になる)。最初の行 (ストリームを作ってから 10 秒後) は受信を始めた直後の形を、その後の行は定数が戻った後の値を表す。映像は読み出したフレームごと (捨てるフレームと間引くフレームも含む)、音声は受け取った断片ごとに数える。

## 受け取りの行

`clock lag` の行の直後に、`av diag stream N:` の行がある (`av_diag.ts`)。音声と映像の関係を調べるための診断で、測るだけで動作は変えない。送信側は、確立している間、画面共有のストリームごとに 1 秒に 1 回、音声と映像のレシーバーの `getStats()` を読み、1 秒を 1 区間として足し、60 秒ごとにまとめる。`playout` の行も、この 1 秒ごとに読んだ最後の値を使う (この診断と `playout` の行でレシーバーの `getStats()` を読むのは、ストリームごとに 1 秒に 1 回まで。映像が届いているかの確認は、もとから別に読む)。そのため `playout` の行の値は、最大で約 1 秒前に読んだ値になる。読む処理は映像と音声の送信を待たせず、失敗しても送信を止めない。

例 (実際は 1 行): `av diag stream 3: span=60.0s n=57/60 G=+8.6ms G.med=+8.4ms G.min=+2.0ms G.max=+15.1ms G.cleanMed=+8.1ms G.clean=50 a.proc=45.1ms a.pkt=20.0ms v.proc=30.2ms v.lead=+12.3ms v.leadEst=0 skip.no-track=0 skip.stats=0 skip.reset=1 skip.audio=0 skip.video=1 skip.lead=1 skip.domain=0 L=42.0ms L.rows=60/60 a.blocks=6000 a.gaps=0 a.gapMax=+0.4ms a.gapMin=-0.3ms a.readMax=21.0ms a.discarded=+0/0 v.discarded=+3/120 v.replaced=+0/0`

- `span`、`n`: 前の行からの秒数と、「G を出せた区間の数 / 読んだ区間の数」。
- `G`: 受け取る側の待ちの差 (ms)。同じときに届いた音声と映像の、届いてからタイムスタンプの時刻までの長さの差で、正は音声のほうが長く待つ (音声のタイムスタンプがその分だけ後ろに付く)。G = [`a.proc` + (`a.pkt` - 10) / 2] - [`v.proc` + `v.lead`]。`G` は期間の合計から出した値、`G.med`、`G.min`、`G.max` は 1 秒ごとの区間の値の中央値、最小、最大。`G.cleanMed` と `G.clean` は、隠したサンプル (`concealedSamples`)、捨てたフレーム (`framesDropped`)、止まり (`freezeCount`)、一時停止 (`pauseCount`) のどれも増えなかった区間だけの中央値と数。
- `a.proc`: 音声の `totalProcessingDelay` の差 / `jitterBufferEmittedCount` の差 (元のパケットが届いてから NetEq から取り出すまで。Discord の暗号の変換の時間を含む)。
- `a.pkt`: 音声のパケットの長さ (`jitterBufferEmittedCount` の差 / (`packetsReceived` - `packetsDiscarded`) の差 / コーデックの clockRate)。NetEq は 10 ms ずつ取り出すので、デコードした音声は平均 (`a.pkt` - 10) / 2 ms だけ取り出されるのを待つ。パケットが失われるとわずかに大きく出る。
- `v.proc`: 映像の `totalProcessingDelay` の差 / `framesDecoded` の差 (最初のパケットが届いてからデコードが終わるまで)。
- `v.lead`: 区間に読んだフレームの先行 (タイムスタンプ - `MediaStreamTrackProcessor` から受け取った時刻) の平均。デコードが終わってから描く予定の時刻までの長さで、受け取るまでの時間 (数 ms) だけ小さく出るので、G はその分だけ大きく出る。`v.leadEst` は、そのうち見積もりの換算 (`video time stream N: time=estimated`) のフレームの数。
- `skip.*`: G を出せなかった区間の数。`no-track` (音声か映像のトラックがない)、`stats` (`getStats()` が返らない、値がない)、`reset` (最初の区間、トラックが替わった、累計が戻った)、`audio` (音声のサンプルかパケットが出ていない)、`video` (デコードしたフレームがない)、`lead` (読んだフレームがない)、`domain` (音声のタイムスタンプが取り出した時刻でない。`audio time stream N: domain=estimated`)。
- `L`: 音声の出力の遅れ (`media-playout` の `totalPlayoutDelay` の差 / `totalSamplesCount` の差)。WebRTC が音声を取り出してから音の装置に渡るまでの平均で、Vesktop の音声の出力全体で 1 つの値である。Vesktop は取り出してから `L` 後に音を出すが、cordmux の音声のタイムスタンプは取り出した時刻なので、Vesktop で見聞きする関係より cordmux の音声は `L` だけ前に付く。`L.rows` は行を読めた区間の数で、`L=none` のとき 0 なら行がなく、0 でなければ音を出していなかった。
- `a.blocks`、`a.gaps`、`a.gapMax`、`a.gapMin`: 期間に読んだ音声の断片の数と、前の断片の終わりとタイムスタンプの差。`a.gaps` は差が前の断片の長さの半分を超えた数 (断片が欠けた)。負の差は前の断片と重なったこと。
- `a.readMax`: 音声の断片を読む間隔の最大 (ページのメインのスレッドが止まった長さの見当)。音声の `MediaStreamTrackProcessor` の待ち行列は 10 個 (10 ms の断片で 100 ms 分) なので、これが 100 ms を超えると `a.discarded` が増え、`a.gaps` が出る。
- `a.discarded`、`v.discarded`: `MediaStreamTrackProcessor` の `discardedFrames` の「+期間/通算」(待ち行列が満ちて捨てた数)。`v.replaced` は、`track_source.ts` がエンコードに渡す前に次のフレームが来たので閉じた数。読み出し口が替わった後の行 (トラックが替わったときと、受信のやり直しで同じトラックから読み直したとき) の期間の値は通算と同じ。読めなければ none。

読み方:

- 録画か出力で測った音声と映像のずれ E (`sync.py` の「音の時刻 - 映像の時刻」) と、同じ時間の行を並べる。受け取る側の待ちの差を除いた、届いた時刻でそろえたときのずれは E - `G.med` と見込める。同じ受け手で送り手 (共有のしかた) を替えて、E と `G.med` の組を比べる。
- Vesktop で見聞きしたときのずれは E + `L` と見込める。同じ時間に Vesktop の窓と音を録って測ったずれと比べ、合わない分は別の元を探す。
- 録画のずれが段で変わった時刻を含む行に、`a.gaps`、`a.discarded`、`a.readMax` の増えがあれば、送信側の読み出しの欠落が元である。
- `G.med` と `G.cleanMed` の差が大きいときは、乱れた区間が G を動かしている。

## 時計の行

- `clock: base=qpc ticks_origin_us=... error_us=... brackets=48 rtt_min_us=... rtt_p50_us=... width_us=... ticks_per_ms=... clamp_us=100 coi=false attempts=N`: 受信側と接続するたびに 1 行。`attempts` は測った回数 (`too-wide` と `call-failed` では、接続を見送って次に試す前に測り直すので、5 回まで増える。測り直すたびに `... attempt=N; measuring again before the next connection` の行も書く)。送信側の時計を決めたときの値 (`ticks_origin.ts`)。`base=page reason=...` なら、ページの原点の TimeTicks を使えず、映像は見積もりの換算になる。理由は `coarse-ticks` (performance.now() の刻みが粗い)、`inconsistent` (組どうしが食い違う)、`too-wide` (誤差の上限が 1 ms を超える)、`call-failed` (メインのプロセスを呼べない、2 秒で答えがない)。
- `video time stream N: time=exact reason=render-time lead=13.9ms`: 映像の換算の方法を決めたとき (最初のフレーム) と替えたとき (`video_time.ts`)。`time=estimated` の理由は、`no-ticks-origin` (送信側の時計が page)、`not-render-time` (先行が描く予定の時刻の範囲にない。取り込みの時刻か、読むのが遅れ続けた)、`other-clock` (先行が 10 秒以上外れる。経過かほかの時計の値。正確な換算の間でも、1 フレームで戻る)。替えた最初の 3 回の後は、ストリームごとに 60 秒に 1 行までにし、`switches=N (M since the last line)` を添える。先行は、フレームを `MediaStreamTrackProcessor` から受け取った時刻 (`track_source.ts` の `takeReadAtUs`) で測る。ストリームの途中で `time=estimated` に戻った行は、異常に関わる行になる。
- `audio time stream N: domain=page lag=0.1ms`: 音声のタイムスタンプの基準を決めたとき (最初の断片。`audio/chunk.ts`)。`page` と `ticks` は見積もりなしの換算、`estimated` は見積もりの換算。
