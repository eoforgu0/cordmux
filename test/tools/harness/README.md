# テスト用の送信側 (harness)

cordmux の受信側 (OBS のプラグイン) に、映像と音声を送るテスト用の Electron アプリである。Vesktop 1.6.9 と同じ Electron 45.2.3 (Chromium 156) で動くので、Vesktop 1.6.9 と同じ WebCodecs のエンコーダを使う。Discord にも Vesktop にも接続しない。実機テストで、送信側の代わりに使う。

動作のモードは 4 つあり、環境変数 `HARNESS_MODE` で選ぶ。

| `HARNESS_MODE` | 中身 |
|---|---|
| 未指定 | tests。ハードウェアのエンコーダ (NVENC) を同時にいくつ作れるかと、作れないときの失敗の形を測る |
| `sustain` | 決めた数のエンコーダで、決めた時間だけエンコードを続け、毎秒の出力の数を測る |
| `send` | エンコードした映像と正弦波の音声を、WebSocket でそのまま送る。製品の送信側のコードは使わない |
| `product` | 製品の送信側のコードを動かして、受信側に映像と音声を送る。主に使うのはこのモード |

## ファイル

| ファイル | 役割 |
|---|---|
| `main.js` | Electron のメインプロセス。隠しウィンドウを開き、結果の JSON を書き、レンダラーに代わってファイルを読み書きする |
| `preload.js` | 環境変数と、メインプロセスの機能を、レンダラーに渡す |
| `index.html` | tests、sustain、send のモードで読み込むページ (`renderer.js` を読む) |
| `renderer.js` | tests、sustain、send のモードの本体 |
| `index-product.html` | product モードで読み込むページ (`dist/sender.js` を読む) |
| `sender.ts` | product モードの本体。esbuild で `dist/sender.js` にバンドルされる |
| `sender_pure.ts` | `sender.ts` から切り出した、DOM に触れない処理 (環境変数の解釈、同期用のマーカー、待ち、facts の組み立てなど) |
| `voice_tone.ts` | 声のストリームに音を乗せるかを決める処理 |
| `frame_pause_file.js` | フレームを止めるファイルと、声に音を乗せるファイルを、メインプロセスで読む処理 |
| `avatar_export_file.js` | 送ったアバターの PNG を、メインプロセスで書き出す処理 |
| `screen_image_file.js` | 画面共有のフレームに描く画像のファイルを、メインプロセスで読む処理 |
| `*.test.ts` | 上の `sender_pure.ts`、`voice_tone.ts`、`frame_pause_file.js`、`avatar_export_file.js`、`screen_image_file.js` の単体テスト |
| `fake-sink.mjs` | send モードで、受信側がないときに受信側の代わりに動かし、届いたフレームを数える |
| `run.ps1` | tests モードの起動スクリプト。nvidia-smi の出力を環境変数に入れて Electron を起動する |
| `summarize.py` | tests モードの結果の JSON を、1 つにつき 1 行にまとめる |
| `summarize-send.py` | send モードの結果の JSON を要約する |
| `tsconfig.json` | `sender.ts` の型チェックの設定 (`src-vencord` の tsconfig とは別) |
| `package.json` | 依存関係 (Electron、ws、esbuild) と `npm run build` |

## 前提

- Windows と Node.js 22。
- 実行には GPU のハードウェアのエンコーダ (NVENC) が要る。`run.ps1` は `nvidia-smi` を使う。
- ハードウェアのエンコーダが空いているとき (通話も配信もしていないとき) だけ実行する。実行する前に `nvidia-smi --query-gpu=utilization.encoder --format=csv,noheader` が 0% であることを確認する。
- `npx electron .` は、`test/tools/harness` で実行する。
- ウィンドウは `show: false` で表示されない。動作は結果の JSON と `tasklist` で確認する。
- OBS や Discord (Vesktop) を起動する処理は、ここにはない。OBS は `test/tools/obs-run` の実行スクリプトが起動する。

## ビルド

ビルドが要るのは product モードだけである。

```
cd test/tools/harness
npm install
npm run build
```

`npm run build` は、`esbuild` で `sender.ts` を `dist/sender.js` (1 つの classic script) にバンドルする。`src-vencord` の `transport/socket.ts`、`encode/session.ts`、`encode/serial.ts`、`audio/chunk.ts`、`core/wire.ts` も一緒にバンドルされる。`dist/` と `node_modules/` は、このフォルダーの `.gitignore` で git の管理外にしてある。esbuild は TypeScript の型を取り除くだけで、型は見ない。

Electron を起動せずバンドルだけをするとき (CI など) は、環境変数 `ELECTRON_SKIP_BINARY_DOWNLOAD=1` を付けて `npm ci` を実行すると、Electron の本体 (約 100 MB) をダウンロードしない。

型をチェックするときは、リポジトリのルートで実行する。

```
node_modules/.bin/tsc -p test/tools/harness/tsconfig.json --noEmit
```

単体テストは、リポジトリのルートで実行する。

```
npx vitest run test/tools/harness
```

## 結果の JSON

| モード | 出力先 | ファイル名 |
|---|---|---|
| tests | `_work/out/` | `harness-<ISO 日時>.json` |
| sustain | `_work/out/` | `harness-sustain-<ISO 日時>.json` |
| send | `_work/out/` | `harness-send-<ISO 日時>.json` |
| product | `_work/out/` | `product-send-<ISO 日時>.json` |

パスは、書いたときに標準出力へ表示される。出力先の `_work/` は、リポジトリのルートにあり、git の管理外である。

JSON は `writtenAt`、`writeReason`、`env` (Electron、Chromium、Node のバージョン、`nvidia-smi` の出力)、`data` を持つ。内部のタイムアウトの前に結果が届かなかったときも、その時点の途中経過を `data` に書く。タイムアウトは、tests モードが 120 秒、sustain、send、product のモードが「送る時間 + 30 秒」で、`HARNESS_TIMEOUT_MS` で変えられる。

## tests モード

Electron を起動する。

```
powershell -NoProfile -ExecutionPolicy Bypass -File test/tools/harness/run.ps1
```

`run.ps1` は、`nvidia-smi` の出力を `HARNESS_NVIDIA_SMI` に入れて `npx electron .` を実行し、最後に最新の結果の JSON のパスを表示する。1080p60 の H.264 について、次の 5 つを順に測る。

- テスト A: `isConfigSupported` が、prefer-hardware、prefer-software、no-preference のそれぞれで何と答えるか。
- テスト B: エンコーダを 1 個ずつ順に作る (最大 8 個、または configure か encode がエラーになるまで)。作れたエンコーダは閉じずに、次の 1 個を足す。何個目でどのエラーが出るかが、NVENC のセッションの上限になる。
- テスト C: B のエンコーダをすべて閉じてから、4 個を同時に作って、並行にエンコードする。
- テスト D: C の 4 個を保持したまま、一時的に 5 個目を作ってエンコードし、破棄する。元の 4 個が動き続けるかを見る。
- テスト E: `prefer-software` で 1 個作る。ハードウェアが使えないときの落ち先を見る。

結果は `data` の下に入る。

- `a`: 3 つのモードそれぞれの `supported` と、実際に使われる `config`。
- `b.attempts[n]`: n 個目の `ok`、`error` (`name` と `message`)、`outputCount`、`maxQueueSize`、`durationMs`。`ok: false` になった番号が、同時に作れた個数の上限である。
- `c.results[0..3]`: 4 個を同時に作ったときの、それぞれの成否と出力の数。
- `d.fifth`: 一時的な 5 個目の成否。`d.afterFour`: 5 個目を閉じた後に、元の 4 個がエンコードを続けられたか。
- `e`: `prefer-software` の `isConfigSupported` の答えと、実際のエンコードの結果。

`error` が出たときは、`name` と `message` をそのまま読む。`QuotaExceededError` の `message` に "reclaimed" などがあるときは、NVENC の上限ではなく、Chromium がバックグラウンドのエンコーダを回収した (非表示のウィンドウが原因) 可能性がある。

`python test/tools/harness/summarize.py <出力先のフォルダー>` は、これらの JSON を 1 つにつき 1 行にまとめる。

## sustain モード

`HARNESS_MODE=sustain` で、N 個のエンコーダに、指定した秒数だけフレームを送り続ける。エンコーダは、同時に作ると初期化エラーが出ることがあるので、1 個ずつ順に作る。エンコーダごとに別の canvas へ、動く図形、大きなフレーム番号、ノイズの矩形 200 個を、毎フレーム描く。`performance.now()` を基準にした高精度のスケジューラー (`setInterval` は使わない) で fps の間隔を刻む。遅れたときは待たずに続けて出して、追いつく。

```
HARNESS_MODE=sustain HARNESS_SUSTAIN_SEC=30 npx electron .
```

| 環境変数 | 既定値 | 意味 |
|---|---|---|
| `HARNESS_SUSTAIN_SEC` | `60` | 送る時間 (秒) |
| `HARNESS_SUSTAIN_N` | `4` | エンコーダの数 |
| `HARNESS_SUSTAIN_HW` | `prefer-hardware` | `prefer-hardware` か `prefer-software` |
| `HARNESS_FPS` | `60` | fps |

結果の `data` には、`perSecond` (毎秒の出力の数、その秒の `encodeQueueSize` の最大、送ったフレームの数)、`perEncoderTotals` (エンコーダごとの、送った数、出た数、落ちた数 = 送った数 - 出た数)、全体の `effectiveFps` が入る。

## send モード

`HARNESS_MODE=send` は、sustain モードと同じ描画と、順に作る方法を使う。VideoEncoder の出力 (`EncodedVideoChunk`) と、1 kHz の正弦波の音声 (48 kHz のステレオ、f32、10 ms = 480 サンプルごと) を、WebSocket でプラグインに送る。

バイナリメッセージの形式は、`u8 type` (1 = 映像、2 = 音声)、`u32 streamId` (リトルエンディアン、1 から始まる)、`u64 timestampUs` (リトルエンディアン)、`u8 flags` (bit0 がキーフレーム)、ペイロードの順である (docs/wire_format.md の「バイナリメッセージ」)。映像と音声は、起動時刻を起点とする同じ時計 (`performance.now()`) を使うので、2 つの `timestampUs` は同じ原点で比べられる。

`HARNESS_SYNC_EVERY_SEC` 秒ごとに、映像の 3 フレームを全面の白にし、同じ `timestampUs` から 60 ms だけ、音声を 2 kHz に上げるマーカーを出す。出した時刻は、結果の JSON の `syncEvents` に入る。WebSocket が 5 秒以内に繋がらないときは、`fatalError` に理由を書いて終了する。

```
node test/tools/harness/fake-sink.mjs
HARNESS_MODE=send HARNESS_SUSTAIN_SEC=30 npx electron .
```

1 行目は、受信側がないときに受信側の代わりに動かす `fake-sink.mjs` である (`ws` が要る。`npm install` で入る)。届いたフレームを、`type:streamId` ごとに 5 秒おきに数えて表示する。マーカーは検出しない。発生した数は、送信側の JSON の `syncEvents` で見る。

| 環境変数 | 既定値 | 意味 |
|---|---|---|
| `HARNESS_WS_URL` | `ws://127.0.0.1:4470` | 接続先 |
| `HARNESS_SUSTAIN_N` | `4` | ストリームの数 |
| `HARNESS_SUSTAIN_SEC` | `60` | 送る時間 (秒) |
| `HARNESS_KEYINT_SEC` | `2` | キーフレームの間隔 (秒) |
| `HARNESS_SYNC_EVERY_SEC` | `5` | マーカーの間隔 (秒) |
| `HARNESS_FPS` | `60` | fps |
| `HARNESS_BITRATE` | `12000000` | 映像のビットレート (bps) |
| `HARNESS_WARMUP` | `0` | `1` にすると、各エンコーダを `configure` した直後 (次のエンコーダを作る前) に、黒の 60 フレームをエンコードして捨ててから、WebSocket を開く。設定値は結果の `data.config.warmup` に残る |
| `HARNESS_SINK_PORT` | `4470` | `fake-sink.mjs` が待ち受けるポート |

結果の `data` には、`perEncoderTotals` (ストリームごとの送った数、出た数、落ちた数)、`perSecond` (毎秒の送ったバイト数と、WebSocket の `bufferedAmount` の最大)、`syncEvents` が入る。`python test/tools/harness/summarize-send.py <出力先のフォルダーまたは JSON>` で要約できる。

## product モード

canvas に描いた映像と、正弦波の音声を、`src-vencord` の実際の送信側のコードに流す。`src-vencord/index.ts` は Vencord の `@utils/types` を import するので、このフォルダーでは使えない。`index.ts` の処理のうち、Vencord に依存しない部分 (WebSocket と VideoEncoder の橋渡し、段階の変更に応じた facts の送信など) を、`sender.ts` に同じ形で持っている。製品のコードには、テスト用の分岐を入れない。

映像は、動く図形、フレーム番号を表すビットの帯、フレーム番号の数字を描く。下地は、真っ黒ではなく明るさ 48 の灰色 (`#303030`) にする。真っ黒だと、`blackframe` の既定のしきい値 (amount 98、threshold 32) で黒のフレームと判定され、黒埋めのフレームと区別できないためである。ビットの帯の升とマーカーの白は、`#000000` と `#ffffff` で描く。

ビットの帯とマーカーは、判定スクリプト (`test/tools/obs-run` の `frames.py` と `sync.py`) が、録画から読む。マーカーは、`HARNESS_SYNC_EVERY_SEC` ごとに、映像のフレーム 3 つの全面の白と、60 ms の 2 kHz の音である。白は、画面共有のストリームごとに、独立に 3 フレーム出す。

映像と音声のメディアのタイムスタンプは、生成の予定時刻から作る (映像は `clock.epochUs0` + フレーム番号 / fps、音声は `clock.epochUs0` + ブロック番号 x 10 ms)。実際の取り込みの時刻 (`VideoFrame.timestamp`) は使わない。生成した時刻で刻むと、描画が詰まったことがタイムスタンプに写って、受信側の整列が飛ぶためである。生成が予定より 200 ms 以上遅れたときは、溜まった分のフレームを作らずに、現在の番号へ飛ぶ (`framesSkippedStale`)。溜まった分をまとめて出すと、OBS がそのストリームの表示の原点を遅く置くためである。音声の到着の時刻は、`pumpAudio` が送信側の時計 (起動のときのシステム時刻を原点にし、`performance.now()` で進める) で別に測って、`audio/chunk.ts` の `pushAudio` に渡す (製品の `index.ts` と同じ形)。

### 起動

`HARNESS_MODE=product` で起動すると、`index-product.html` が開く。

```
npm run build
HARNESS_MODE=product HARNESS_NSCREEN=2 HARNESS_NVOICE=1 CORDMUX_HANDSHAKE_DIR=<フォルダー> npx electron .
```

- `main.js` は、このモードのときだけ、WebSocket の接続の `Origin` ヘッダーを `https://discord.com` に書き換える (Vesktop の送信側が、discord.com のページの中で動くことに合わせる)。
- トークンファイル (`session.json`) は、メインプロセスが、環境変数 `CORDMUX_HANDSHAKE_DIR` のフォルダーから接続を試すたびに読み直す。指定がないときと、読めないとき、形式が違うときは、理由を区別せずに読めなかった扱いにする。実際の `%LOCALAPPDATA%\cordmux` は読まない。
- 接続先は、トークンファイルの `port` から決まる。`HARNESS_WS_URL` は使わない。
- 確立する前に作ったフレームと音声は、送らずに捨てて、数だけ数える (socket を作る前は `droppedBeforeSocket`、作った後は `socketStats` の `droppedBeforeEstablished`)。エンコーダは、確立を待たずに作る。`seconds` 秒が経った時点で、確立したかどうかを結果に書いて終わる。接続を諦める期限は持たない。
- 確立するたびに、画面共有のすべてのストリームでキーフレームを要求し直す。確立の前に送ったフレームは捨てられるので、受信側が最初に受け取るフレームを、キーフレームにするためである。
- hello の `version` には、製品の `src-vencord/version.ts` の `CORDMUX_BUILD_ID` を載せる。Discord 側のバージョンとして、受信側が同梱の `cordmux-build.txt` の `build_id` と比べて、違えば「バージョンの食い違い」の行を出すので、同じ値にしておく。実際の Vencord からの接続と見分けるために、確立するたびに `cordmux-harness: test sender ...` で始まる 1 行を、`log` メッセージで送る (OBS のログに出る)。
- 停止するときは、`session.stop()` の後で、最後のブロックが出てくるのを待ってから、socket を閉じる。待つ上限は 2 秒である。待たずに閉じると、録画が ON のまま終わったときの、黒埋めのフレームが届かない。

### 環境変数

| 環境変数 | 既定値 | 意味 |
|---|---|---|
| `HARNESS_NSCREEN` | `1` | 画面共有のストリームの数。それぞれ映像と音声を持つ |
| `HARNESS_NVOICE` | `0` | 声のストリームの数。音声だけを持つ |
| `HARNESS_SECONDS` | `10` | 送る時間 (秒) |
| `HARNESS_FPS` | `60` | 画面共有の映像の fps |
| `HARNESS_HW` | `prefer-hardware` | `encode/session.ts` の `preference` (`prefer-hardware` か `prefer-software`) |
| `HARNESS_WARMUP` | 未指定 | エンコーダのウォームアップをするか。未指定ならする。`0` のときだけしない (sustain と send のモードとは、既定の向きが逆) |
| `HARNESS_KEYINT_SEC` | `2` | キーフレームの間隔 (秒) |
| `HARNESS_SYNC_EVERY_SEC` | `5` | マーカーの間隔 (秒) |
| `HARNESS_BITRATE` | `12000000` | ビットレート (bps)。`bitrateFactor = 指定値 / 12000000` に換算する。`encode/session.ts` はピクセル数と fps に比例させるので、指定どおりの bps になるのは 1920x1080、60 fps のときだけである |
| `HARNESS_FAIL_STAGE` | `0` | この段階まで `isConfigSupported` を不支持にして、下の段階へ移るようにする。0 は何もしない。移った結果の段階は、`facts.stages` で受信側に届く |
| `HARNESS_OVERSEND` | `0` | `1` にすると、送りすぎにする。送る間隔の待ちの目標時刻を 1/4 にして実時間の 4 倍の速さで刻み、映像のアクセスユニットの末尾にゼロを足して転送量を増やす。背圧の経路を実際に通すためである。待ちの目標時刻を過ぎていても、`setTimeout` を 1 回挟んでイベントループを 1 周させる。挟まないと、WebSocket の入出力が進まず、背圧が起きない |
| `HARNESS_MAX_LONG_EDGE` | 未指定 | `encode/session.ts` の `limits.maxLongEdge` に、そのまま渡す。長辺をこの値まで縮めてからエンコードし、`configure` の幅と高さも縮小後になる (結果の `streams[].encodedFormat` で確認できる)。実際の長辺は、この値と、その段階の目標の長辺の小さいほうになる |
| `HARNESS_FPS_DIVISOR` | `1` | `1` か `2`。`limits.fpsDivisor` (fps を半分にする) |
| `HARNESS_FAIL_CONTACT` | 未指定 | 接続の確立後に、この依存箇所の id (`src-vencord/ledger.ts` の `ContactId` のどれか。例は `roster`) を読めない状態として、`facts.warnings` に載せる。id が正しいかは検査しない |
| `HARNESS_SYNC_TONE` | `screen` | マーカーの音 (2 kHz) を載せるストリーム。`screen` は最初の画面共有、`voice` は最初の声、`none` はどれにも載せない。マーカーを持たないストリームは無音で、ブロックの数と大きさは変わらない。白のマーカーは、すべての画面共有に出す。知らない値は `screen` になる (`sender_pure.ts` の `parseSyncTone`)。`test/tools/obs-run` の実行計画が、段ごとに値を決めて渡すので、外から環境変数で指定しても上書きされる |
| `HARNESS_RESOLUTION_SCHEDULE` | 未指定 | 送っている途中に解像度を変える予定。`"<起動からの秒数>:<幅>x<高さ>"` をカンマで並べる (`"35:1280x720,60:640x360"`)。その時刻に、画面共有のストリームごとにセッションを止めて、新しい寸法で作り直す (製品の `stopScreenEntry`、`startScreenVideo` と同じ手順)。止めたセッションの最後の計数を、結果の `encodeBefore` に残す。読めない項目だけを捨てる (`parseResolutionSchedule`) |
| `HARNESS_SCREEN_SIZES` | 未指定 | 画面共有のストリームごとの、最初の寸法。`"<幅>x<高さ>"` をカンマで並べ、i 番目の項目が i 番目の画面共有 (stream id が i + 1) の寸法になる。`"1280x720,3840x2160"` は、1 つ目が 720p、2 つ目が 2160p である。項目がないもの、読めないもの、空のものは 1920x1080 になる。読めない項目を捨てて詰めると、後ろの項目が別のストリームに移るので、詰めない (`",3840x2160"` は 2 つ目だけを変える)。画面共有の数より多い項目は使わない。`HARNESS_RESOLUTION_SCHEDULE` と併用でき、予定の時刻からは、すべてのストリームが予定の寸法になる。使った寸法は結果の `config.screenSizes`、エンコードした寸法は `streams[].encodedFormat` に残る (下の段階へ移ると縮む) |
| `HARNESS_SCREEN_NAMES` | 未指定 | 画面共有のストリームごとの表示名。カンマで並べ、i 番目の項目が `harness-screen-<i + 1>` の profiles の `display_name` になる。アバターの文字も表示名の先頭の 1 文字になる。空の項目、制御文字を含む項目、64 文字を超える項目は、そのストリームだけ userId のまま (`parseScreenNames`) |
| `HARNESS_SCREEN_IMAGES` | 未指定 | 画面共有のストリームごとに描く画像のファイルの場所。カンマで並べ、i 番目の項目が i 番目の画面共有になる (下の「画像を描く」) |
| `HARNESS_VOICE_USERS` | 未指定 | 声のストリームごとの userId。カンマで並べ、i 番目の項目が i 番目の声のストリームの持ち主になる。画面共有している人の userId (`harness-screen-2` など) を書くと、その人の声になり、その人の枠に話している人の光が出る。項目がない、空、英数字と `_` と `-` の 64 文字まで以外、同じ userId の 2 本目、ほかの声の既定の userId は、既定の `harness-voice-<番号>` になる (`parseVoiceUsers`) |
| `HARNESS_FRAMES_PAUSE_FILE` | 未指定 | 映像のフレームを止めるファイルの場所 (下の「フレームを止める」) |
| `HARNESS_HELLO_VERSION` | `CORDMUX_BUILD_ID` | hello の `version` に載せる文字列。zip で配布するバージョン (`<version>+<短いコミット>`) を使うときは、zip の `cordmux-build.txt` の `build_id` を渡す。空、空白や制御文字を含むもの、64 文字を超えるものは、既定になる (`parseHelloVersion`) |
| `CORDMUX_HANDSHAKE_DIR` | 未指定 | `session.json` があるフォルダー。未指定ならトークンを読めない扱いになる |
| `CORDMUX_HARNESS_VOICE_TONE_FILE` | 未指定 | 声に音を乗せるファイルの場所 (下の「声に音を乗せる」) |
| `CORDMUX_HARNESS_AVATAR_DIR` | 未指定 | 送ったアバターの PNG を書き出すフォルダー (下の「アバターを書き出す」) |
| `HARNESS_TIMEOUT_MS` | 送る時間 + 30 秒 | 内部のタイムアウト (ミリ秒) |

`HARNESS_STREAM` は渡さない。配信の録画のストリームは、`test/tools/obs-run/run-window.mjs` が obs-websocket の StartStream と StopStream で扱う。

stream id は、画面共有と声を通した 1 つの通し番号である (画面共有が 1 から `HARNESS_NSCREEN` まで、声がその続き)。受信側が、stream id を、画面共有と声で 1 つの番号空間として扱うためである (`sender_pure.ts` の `assignStreamIds`)。userId は、`harness-screen-<n>`、`harness-voice-<n>` の合成した値になる (声は `HARNESS_VOICE_USERS` で変えられる)。

### 画像を描く

`HARNESS_SCREEN_IMAGES` で画像を渡した画面共有には、その画像だけを描く。縦横比を保ってストリームの寸法を覆う大きさに拡大し、はみ出た分を中央で切る (`coverSourceRect`)。フレーム番号の帯、動く図形、マーカーの白は描かないので、`frames.py` と `sync.py` で読む実行には使わない。実際の OBS の画面を撮るときに、枠の中に落ち着いた画像を出すためのものである。

- 画像は送り始める前に 1 回だけ読み、解像度の変更と `media` の開き直しで作り直したときも同じ画像を使う。
- ファイルを読むのはメインプロセスで、`HARNESS_SCREEN_IMAGES` に書かれた場所だけを読む (`main.js` の `read-screen-image`)。32 MB を超えるファイルは読まない。形式は Chromium の `createImageBitmap` が読めるもの (PNG と JPEG など)。
- 読めなかった画像は、そのストリームだけ動く図形に戻る。結果は結果の JSON の `screenImages` に残る。
- 場所にカンマは書けない。相対の場所はこのフォルダーから読むので、`test/tools/obs-run/run-window.mjs` は絶対の場所にして渡す。

### フレームを止める

`HARNESS_FRAMES_PAUSE_FILE` のファイルがある間、映像のフレームを作らず、エンコーダにも入れない。音声、接続、ハートビート、facts、configure は、そのまま続く。受信側から見ると、接続していて音声も届くが、映像だけが届かない状態になる。

- ファイルの中身が空なら、すべての画面共有を止める。`2` や `1,2` (カンマか空白で区切った stream id) なら、そのストリームだけを止める。画面共有でない項目は止めずに、結果の `framesPause.ignoredTargets` に出す。読める項目が 1 つもないときは、何も止めない。ファイルを書き換えると、止めるストリームを変えられる。
- ファイルを消すと、その時刻のフレームから再開する。止めていた間のフレームは、あとから作らない。再開のときにキーフレームを自分からは出さない。止めている間に受信側が要求していれば、再開した最初のフレームがキーフレームになる。
- エンコードしてから捨てる形にはしない。受信側が持っていないフレームを参照する差分を送ることになり、次のキーフレームまで受信側のデコードが崩れて、測りたい状態に送信側の欠陥が混ざるためである。
- レンダラーはファイルを読めないので、100 ミリ秒ごとにメインプロセスに尋ねる (`main.js` の `read-frame-pause`)。メインプロセスはファイルを読むだけで、作らず、消さない。起動したときにファイルがあれば、最初のフレームから止まる。
- 止めている間も、マーカーの音 (2 kHz) は鳴り、白のフレームは出ない。止めていた間に過ぎた境界の白は、再開の後にも出さない。再開したときに初めて記録された境界は、`syncEvents` に `whiteSkippedAfterPause: true` を付けて載る。A/V の判定 (`sync.py`) は、止めた区間を除いて読む。
- 受信側が「映像が止まっています」と表示し始める時間は、設定の `video_stall_ms` (既定 2000 ms) なので、これより長く止める。
- `stuck:2` のように書いたストリームは、製品の送信側で読み出しかエンコードが止まった形で止める (Discord には映像が届いている形)。受信側は、映像が届かないまま `receive_restart_wait_ms` (既定 5000 ms) たつと stream_check で確認を頼み (docs/wire_format.md の「メッセージの種類」の stream_check の扱い)、この送信側は、製品が読み出しを始め直したときと同じく止めるのを解いて `restarted` と答える。受信側の受信のやり直しで映像が戻ることを、OBS とこの送信側だけで確認するための形である。始め直したストリームは、ファイルから `stuck:2` を消すまで止めない。`2` だけで止めたストリームは、Discord が送っていない形として `not_receiving` と答え、止めたままにする。止めていないストリームには `sending` と答える。

### 声に音を乗せる

`CORDMUX_HARNESS_VOICE_TONE_FILE` のファイルがある間、ファイルに書いた番号の声のストリームに、1 kHz、振れ幅 0.3 の音を乗せる (受信側のミキサーが「聞こえる声」と数え、合成のソースが話している人の光を点ける)。ファイルがないときは、声は無音になる。中身の書き方と、画面共有の番号を無視して `voiceTone.ignoredTargets` に出すことは、フレームを止めるファイルと同じである。同期用のマーカーの音を出すストリームは、マーカーの音だけを出す。メインプロセスが読み (`read-voice-tone`)、作らず、消さない。

### アバターを書き出す

`CORDMUX_HARNESS_AVATAR_DIR` を指定すると、送った `avatar` の PNG のバイト列を、参加者ごとに 1 回、`<フォルダー>/<userId>.png` に書く。`test/tools/composite-check` が、OBS が受け取ったのと同じ画像で名前のラベルを描くためである。アバターは Chromium の `OffscreenCanvas` で描くので、`composite-check` の側では同じ画像を作れない。書くのはメインプロセス (`main.js` の `export-avatar`) である。未指定なら、フォルダーもファイルも作らない。ファイル名にできない userId (英数字と `_` と `-` の 64 文字まで、以外) は書かない。

### 結果の JSON (`data` の下)

- `config`: 実際に使った設定 (`screenSizes`、`warmup`、`bitrate` など)。
- `establishedReached`: 一度でも `transport/client.ts` の `established` になったか。
- `socketStats`: `{ droppedBeforeEstablished, droppedByBackpressure, droppedAudioByBackpressure, watchdogCloses, droppedInvalidControl }` (`transport/socket.ts` の `stats()`)。
- `droppedBeforeSocket`: `{ video, audio }`。socket をまだ作っていない間 (エンコーダを順に作っている間と、ウォームアップの間) に作ったが、送れずに捨てたフレームと音声の数。
- `streams[]`: ストリームごとの `{ kind, streamId, stageHistory, encode, encodedFormat, encodeBefore, audioChunksSent, audioReanchorCount, hasSyncTone }`。
  - `stageHistory`: `encode/ladder.ts` の `LadderStage` の遷移の記録。
  - `encode`: `encode/session.ts` の `stats()` (声は `null`)。
  - `encodedFormat`: 最後に `configure` へ渡した `{ width, height, fps }` (縮小した後の寸法)。声と、段階が決まらなかったストリームは `null`。
  - `encodeBefore`: 途中で解像度を変えて作り直したときの、前のセッションの最後の計数 (起きた順)。作り直していなければ空。`encode` は最後のセッションの分しか持たないので、これが要る。
  - `audioReanchorCount`: `encode/clock_map.ts` が音声の換算の定数を決め直した回数 (到着とタイムスタンプの差が 200 ms を超えた状態が 1 秒続いたとき)。
  - `hasSyncTone`: マーカーの音を出したか。`HARNESS_SYNC_TONE` の指定で、最大でも 1 つのストリームだけが真になる。
- `syncEvents[]`: マーカーを出した時刻 (`timestampUs`。送信側の時計) の一覧。
- `factsSent`: 最後に送った facts (`{warnings, sharing, stages?}`)。画面共有があるときは `stages` (ストリームごとの現在の段階) が入るので、`HARNESS_FAIL_STAGE` で下げた段階が届いたかを、録画を見ずに確認できる。画面共有がなく `HARNESS_FAIL_CONTACT` も未指定のとき、と、一度も確立しなかったときは `null` (facts を送らない)。
- `recordingChanges[]`: 受信側から `recording` の制御メッセージが届いて、値が変わった時刻 (`{ atMs, on }`。起動からのミリ秒)。録画の ON と OFF は、受信側から届く値をそのまま画面共有のセッションに配る (製品の `index.ts` の `handleRecording` と同じ)。
- `resolutionChanges[]`: 予定どおりに作り直したストリームと寸法 (`{ atMs, streamId, width, height, ok, deferred? }`)。`deferred` が真の行は、受信側の `media` で閉じている間に来た変更で、寸法だけを覚えておき、開き直すときに使った。
- `mediaChanges[]`: 受信側から届いた `media` の値が変わった時刻 (`{ atMs, on }`。`hello_ok` の初期値を含む)。`media` が偽の間は、画面共有をすべて止め、音声は送らずに数えるだけにする。真に戻ると、同じ寸法で、フレーム番号の続きから作り直す。閉じている間の `configure` は空にする。
- `limitsChanges[]`: 受信側から届いた `limits` (`{ atMs, limits }`。ワイヤー形式のまま)。動いているストリームには、下げたキーだけがその場で適用される。環境変数の上限とは、キーごとに厳しいほうを使う (`encode/limits.ts` の `tightenLimits`)。
- `gate`: `media` で閉じている間に起きたことの計数 (`{ encodersCreatedWhileClosed, videoFramesSentWhileClosed, materialFramesSentWhileClosed, audioBlocksSkippedWhileClosed }`)。閉じている間は、最初の 2 つが 0 になるはずである。閉じるときに終えるストリームの、黒埋めのフレームは `materialFramesSentWhileClosed` に出る。途中で失敗したときは `null`。製品の「確立している間だけ動く」動作は、この送信側にはない (確立を待たずにエンコーダを作るため) ので、その動作はここでは確認できない。
- `materialDrain`: 最後のブロックが出てくるのを待った実測 (`{ waitedStreams, waitedMs, timedOut }`)。待ったストリームがなければ `null`。
- `framesPause`: フレームを止めた記録 (`{ file, intervals, ignoredTargets, readErrors, checks }`)。`checks[]` は受信側の stream_check に答えた記録 `{ atMs, streamId, outcome }` である。`intervals[]` はストリームごとの区間 `{ streamId, startMs, startWallMs, endMs, endWallMs, framesWithheld }` で、`*Ms` は起動からのミリ秒、`*WallMs` はメインプロセスがファイルを読んだシステム時刻 (OBS のログと比べる値)、`framesWithheld` は作らなかったフレームの数である。`endMs` が `null` の区間は、止めたまま終わった。時刻は、尋ねる間隔の分 (約 100 ms) だけ粗い。`readErrors` は、読めずに前の状態を保った回数。使わなかったときは `null`。
- `voiceTone`: 声に音を乗せた記録 (`{ file, intervals, ignoredTargets, readErrors }`)。`intervals[]` の形と時刻の意味は `framesPause` と同じ (`framesWithheld` はない)。使わなかったときは `null`。
- `avatarExport`: 書き出した userId と、書き出せなかった userId。
- `screenImages`: 画像を渡した画面共有ごとの `{ index, file, state, width?, height?, message? }`。`state` は `loaded`、読めなかった理由 (`absent`、`too-large`、`error`、`rejected`)、`decode-failed` のどれか。画像を 1 枚も渡さなかったときは `null`。
- `aborted`、`fatalError`: 想定外の例外で終わったとき。
