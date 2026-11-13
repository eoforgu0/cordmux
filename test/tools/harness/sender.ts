/*
 * テスト用の送信側。製品の送信部 (src-vencord の transport/socket.ts、encode/session.ts、
 * encode/serial.ts、audio/chunk.ts、core/wire.ts) をそのまま動かし、Electron の Chromium 上で
 * OBS プラグインへ映像と音を送る。esbuild で 1 つの classic script にバンドルして使う。
 *
 * window.cordmuxSender.run(params) を公開する。映像の材料は OffscreenCanvas への描画 (動く図形・
 * フレーム番号のビット帯・マーカーの白フレーム)、音の材料は 1kHz の正弦波 (マーカー中は 2kHz) で作る。
 * src-vencord/index.ts は Vencord の `@utils/types` を import するのでここでは import できない。
 * index.ts が持つ配線のうち Vencord に依存しない部分だけをこのファイルで再実装している
 * (理由は下の一覧の、VideoEncoder / VideoFrame の生成と WebSocket のラップを複製した項)。
 *
 * 実装上の注意:
 *
 * 1. 環境変数の名前: obs-run の実行スクリプト (test/tools/obs-run/run-window.mjs) が渡す
 *    `HARNESS_NSCREEN` / `HARNESS_NVOICE` / `HARNESS_SECONDS` / `HARNESS_HW` / `HARNESS_WARMUP` /
 *    `HARNESS_OVERSEND` / `HARNESS_FAIL_STAGE` / `HARNESS_MAX_LONG_EDGE` / `HARNESS_FPS_DIVISOR` の名前に
 *    そろえた。両側で名前がずれると突き合わせの手間が出るため。sustain / send モードが使う
 *    `HARNESS_SUSTAIN_N` などとは別の名前にしている (既存の環境変数の意味を変えないため)。
 *    一覧は README.md の表にある。`HARNESS_STREAM` は運ばない。`--stream` は OBS 側 (obs-websocket の
 *    StartStream / StopStream) と受け口だけで完結し、送信側のコードは配信の有無で変わらないため。
 * 2. ストリームの内訳: 1 つの画面共有は映像 (encode/session.ts) と音 (audio/chunk.ts を直接使う。
 *    エンコードしない) の両方を持つ (index.ts の ScreenEntry と同じ構造)。声は音だけ (VoiceEntry)。
 *    stream_id は送信側全体で 1 つの通し番号にする (画面が 1..nScreen、声が続けて
 *    nScreen+1..nScreen+nVoice。sender_pure.ts の `assignStreamIds`)。
 *    画面と声を別々に 1 始まりで振ると、受け手 (src-obs/pipeline/stream_router.h) の経路表で
 *    同じ stream_id が後勝ちで上書きされ、画面 4 + 声 8 のときに画面の映像が 1 つも画面の枠へ
 *    出なくなる。
 * 3. userId: 実際の Discord の名簿がないので `harness-screen-<n>` / `harness-voice-<n>` という
 *    合成の userId を使う。
 * 4. wsUrl を持たない: transport/socket.ts は `session.port` (session.json の中身) から URL を
 *    組み立てる。sustain / send モードが使う `HARNESS_WS_URL` は fake-sink.mjs へ直接つなぐための
 *    ものであり、product モードにはない。
 * 5. warmup の既定値: product モードはエンコーダの温めを既定で行う。sustain / send モードは
 *    `HARNESS_WARMUP` を「'1' のときだけ温める」で扱うので、同じ環境変数名のまま向きだけ変えるため、
 *    preload.js は生の文字列 (`productWarmupRaw`) を別に渡し、`paramsFromEnv` が「'0' 以外は
 *    温める」と解釈する。
 * 6. failStage の実装: `EncodeSessionDeps.isConfigSupported` は呼ばれた段が何番目かを知らない。
 *    しかし encode/serial.ts の SerialGate は 1 つのセッションのラダー降格 (Native -> HalfFps ->
 *    Downscaled -> SoftwareFloor) 全体を 1 つのタスクとしてしか受け付けない
 *    (encode/session.ts の attemptFromStage が再帰で gate.run() の外に出ないため)。
 *    よって連続した呼び出し回数を数えれば、1 つのセッションの中では段の番号と一致する。
 *    4 回呼んでも成功しなければ次は Unavailable に落ちて isConfigSupported は呼ばれなくなるので、
 *    カウンターが 4 以上になったら次の呼び出しを別セッションの 1 段目とみなしてリセットする。
 *    failStage=0 は強制失敗なし (実物の isConfigSupported をそのまま呼ぶ)。
 * 7. overflow (送りすぎ) の実装: (a) 送信のペース制御 (fps / 10ms ごとの待ち) の目標時刻を 1/4 に
 *    して、実時間の 4 倍の速さで刻む。(b) エンコード済みの 1 アクセスユニットの末尾にゼロバイトを
 *    足して転送量を水増しする (Annex B の trailing_zero_8bits は復号を壊さない。ITU-T H.264
 *    附属書 B)。狙いは transport/backpressure.ts の「しきい値を超えたら映像を捨て、次の鍵フレームまで
 *    捨て続ける」経路を実際に通すこと。水増しの量は固定値 (OVERFLOW_PAD_BYTES) で、`overflow`
 *    パラメーターが真偽値だけを持つので環境変数では調整できない。音は水増ししない
 *    (音は捨てない方針のため)。待ちを丸ごと外すと、next() が即座に解決する繰り返しでイベント
 *    ループが空かず、WebSocket の接続 (I/O) が進まずに背圧が測れない。
 * 8. maxLongEdge は動作する: `EncodeSessionParams.limits.maxLongEdge` は、縮小を注入する
 *    `EncodeSessionDeps.scaleFrame` を通して動作する。この送信側も製品 (index.ts) と同じ実装
 *    (OffscreenCanvas へ drawImage して新しい VideoFrame を作り、元のフレームを close する) を
 *    渡す (ほかの生成処理と同じく index.ts の複製)。`HARNESS_MAX_LONG_EDGE` を渡すと縮んだ寸法でエンコードし、
 *    `configure` の width / height にもその寸法が出る。
 * 9. VideoEncoder / VideoFrame の生成と WebSocket の薄いラップは index.ts と同じ実装を複製した:
 *    index.ts の createEncoder、isConfigSupported、makeBlackFrame、wrapWebSocket、
 *    concatHeaderPayload は Vencord の `@utils/types` を import するファイルにあるので、
 *    このファイルから import できない (tsconfig.json の exclude)。中身は Vencord に依存しない
 *    ブラウザ標準 API の橋渡しなので、同じロジックを複製している。
 * 10. 確立後に鍵フレームを要求し直す: `established` に入る前に sendBinary された映像は
 *     transport/socket.ts が黙って捨てて数えるだけなので、確立の時点ではエンコーダの GOP が
 *     進んでいる可能性がある。受け手が最初に受け取るアクセスユニットを必ず鍵フレームにするため、
 *     `onEstablished` で全画面のセッションに `requestKeyframe()` を呼び直す。
 * 11. 接続を諦める締め切りを持たない: transport/client.ts のバックオフは無期限に再試行し続ける。
 *     このファイルもそれに合わせ、`seconds` 秒が経過したら、その時点の `establishedReached` を
 *     そのまま結果に残して終える。
 * 12. hello.version は製品と同じ文字列にする: hello の `version` には、製品の index.ts と同じ
 *     src-vencord/version.ts の `CORDMUX_BUILD_ID` を載せる (環境変数 `HARNESS_HELLO_VERSION` が
 *     表示できる ASCII の文字列ならそちらを優先する。読み方は sender_pure.ts の
 *     `parseHelloVersion`)。受け手は hello の `version` を、同梱のファイルの `build_id` と文字列で
 *     比べ、違えば Discord 側のバージョンの食い違いを表示する。専用の文字列にすると、接続のたびに
 *     この表示が出て、画面の判定に雑音が入る。ビルドした同梱物の `build_id` は `CORDMUX_BUILD_ID`
 *     (`<version>+dev`) と同じなので、既定で一致する。CI の zip (`<version>+<短いコミット>`) で
 *     実行するときは、実行スクリプトが zip の `build_id` を `HARNESS_HELLO_VERSION` で渡せば一致する。
 *     実際の Vencord からの接続と見分ける手がかりは、確立のたびに `log` で送る 1 行
 *     (`harnessIdentityLogLine`。`cordmux-harness: test sender ...`) に持たせる。hello に項目を
 *     足さないのは、ワイヤ形式をテスト用の都合で変えないため。
 * 13. bitrate (bps) から bitrateFactor への換算: `bitrateFactor = bitrate / 12_000_000`
 *     (encode/session.ts の BASELINE_BITRATE_BPS と同じ値)。encode/session.ts の computeBitrate が
 *     ピクセル数と fps に比例させるので、指定どおりの bps になるのは解像度が 1920x1080、fps が 60 のとき
 *     だけである。
 * 14. 解像度の既定は 1920x1080: 画面共有の最初の寸法は、`HARNESS_SCREEN_SIZES` で
 *     指定しなければ 1920x1080 にする。
 * 15. `--stream` はこの送信側を経由しない: 配信は OBS 側と受け口だけで完結する。
 *     このファイルは `HARNESS_STREAM` を受け取らない。
 * 16. ノイズ矩形を描かない: renderer.js はエンコードの負荷を上げるためにフレームへ 200 個の乱数の
 *     矩形を描くが、`frames.py` の判定に要るのは動く図形・フレーム番号のビット帯・
 *     マーカーの白フレームまでである。転送量を増やす目的は、送信のペース制御による水増しで別に満たせるので、
 *     ここでは描かない。
 * 17. マーカーの白フレームの判定を sender_pure.ts に切り出した: `sync.py` はストリーム (その人の枠)
 *     ごとに白の立ち上がりと 2kHz を対にして A/V を測るため、画面が何ストリームあっても
 *     ストリームごとに同じ 3 フレームの白が出る必要がある。残数を 1 つだけ持ち、全ストリームの
 *     `HarnessVideoFrameSource.next()` が早い者勝ちで消費する形では、白が全ストリーム合わせて
 *     3 フレームにしかならず、0 フレームのストリームは `sync.py` で測れない。
 *     `SyncCoordinatorPure` は「境界を跨いだ回数」だけを配り、`PerStreamWhiteFrameGate` を
 *     ストリームごとに持たせて残数を独立させる (window / OffscreenCanvas に依存しない純粋な
 *     ロジックなので vitest でテストできる。`sender_pure.test.ts`)。
 * 18. フレームと音の材料は `close()` まで作り続ける: エンコーダのラダー確保 (直列) + 温め
 *     60 フレーム x ストリーム数 + WebSocket 接続という立ち上がりには時間が掛かる。
 *     `seconds` から固定数で材料の総数を計算して絶対時刻でペースを刻むと、測る時間より先に材料が
 *     尽きる。さらに `getSocket()` が `null` (確立前) の間に作ったフレームと音を黙って捨てると、
 *     送った長さが `seconds` より短くなり、録画の末尾が静止画と無音になる。そのため
 *     (a) `HarnessVideoFrameSource` と `HarnessAudioSource` は総数を持たず、`close()` が呼ばれる
 *     まで作り続ける。(b) `getSocket()` が `null` の間に捨てた数は `droppedBeforeSocket` として
 *     結果に残す。`sleepMs(params.seconds*1000)` の起点は `socketHandle.start()` の直後である。
 * 19. overflow のときは、待ちの目標時刻を過ぎていても setTimeout を 1 回挟む: `waitUntil` は目標
 *     時刻を過ぎていると `Promise.resolve()` で即座に返す (マイクロタスクだけで次の周回に入る)。
 *     1080p60 を 4 倍の速さ (毎秒 240 フレーム) で作り続けられる見込みはないので、目標時刻を
 *     1/4 にしただけでは常に「過ぎている」側に落ち、接続が進まない状態に戻る。そのため
 *     overflow のときだけ、待ちが過ぎていても `setTimeout(0)` を 1 回挟んでイベントループを
 *     1 周させる。通常 (overflow でない) の経路は即座に返す。この点が背圧の測定の成否を分けるので、
 *     `waitUntil` は sender_pure.ts へ切り出して `sender_pure.test.ts` でテストしている。
 * 20. 音のメディアのタイムスタンプは生成の予定時刻: `clock.epochUs0 + ブロックの番号 x 10ms` を
 *     `mediaTimestampUs` として返す (実物の `AudioData.timestamp` に当たる値)。到着の時刻
 *     (`clock.nowUs()`) は `pumpAudio` が別に測って `pushAudio` へ渡す (製品の index.ts と同じ形)。
 *     映像も、生成の予定時刻 (`clock.epochUs0` + フレーム番号 / fps) をそのままメディアのタイムスタンプにする。
 * 21. マーカーの音の環境変数名は `HARNESS_SYNC_TONE`: 値は `screen` / `voice` / `none` で、既定は
 *     `screen` (`parseSyncTone`)。ステップごとに値を置くのは実行スクリプト側で、
 *     test/tools/obs-run/run-window-plan.mjs がステップごとに `syncTone` を決め、
 *     test/tools/obs-run/run-window.mjs が `if (step.syncTone)` のときだけ `HARNESS_SYNC_TONE` を
 *     子プロセスの環境に足す (実行計画に値がなければ何も置かず、ここの既定に委ねる)。外から
 *     環境変数を置いても、実行計画が値を持つステップでは上書きされる。
 * 22. 音の換算の定数の決め直しの回数を結果に出す: `encode/clock_map.ts` が数える `restarts`
 *     (`chunker.clockMap.restarts`) を `HarnessStreamResult.audioReanchorCount` として結果の JSON に出す。
 *     録画を見ずに決め直しが何回起きたかを読むため。overflow のときは作る速さだけが 4 倍で (到着は 2.5ms
 *     ごと)、メディアのタイムスタンプはブロックの番号 x 10ms のままなので、到着 - タイムスタンプ が 1 ブロック
 *     あたり 7.5ms ずつ小さくなる。換算の定数は、2 秒の期間の中で 3 つそろった小さい差まで下がり続けるので、
 *     送るタイムスタンプは到着とほぼ同じ速さで進み (到着より先に出る量は 30ms まで)、差が 200ms に届かないので
 *     決め直しは起きない (encode_clock_map.test.ts の「到着の 4 倍の速さで進む間」で確かめる)。映像も同じ形で、
 *     メディアのタイムスタンプは実時間より速いが、送るタイムスタンプは実時間で進む。背圧の判定
 *     (run-window-plan.mjs の judgeBackpressure) は確立したかと捨てた数だけを見るので、この進み方に依らない。
 * 23. facts を自分で組み立てて送る (`HARNESS_FAIL_CONTACT` と降格の段): 受け手の画面には `facts`
 *     からしか作れない状態 (接点を 1 つ潰した状態と降格) がある。この送信側はテスト用の材料しか
 *     持たないので、接点の生死そのものは模せない。代わりに `onEstablished` で sender_pure.ts の
 *     `buildHarnessFacts(contactId, stages, sharing, gate)` が組み立てた
 *     `{warnings,sharing,recording,media,stages?}` (docs/wire_format.md の「facts」) を
 *     `{type:"facts",facts:...}` で送る。
 *     - `warnings`: `HARNESS_FAIL_CONTACT` で指定された接点 id を 1 件だけ。接点 id の妥当性は
 *       検査しない。facts は ws の層では中身を見ない契約なので、検査を足す理由がない。
 *       どの接点 id を選ぶかは test/tools/obs-run/run-window-plan.mjs の
 *       `DOCK_VIEW_FAIL_CONTACT_ID` が決める。
 *     - `stages`: 画面共有のストリームごとの現在の段 (`session.stats().stage`)。`HARNESS_FAIL_STAGE`
 *       でラダーを下ろすのはこの送信側自身なので、段を知っているのもここである。これを送らないと、
 *       受け手の「画質」の欄は降格していても `-` のままになる。段が変わったら送り直す
 *       (`videoSink` の `onStage`)。綴りは製品と同じ encode/ladder.ts の `stageWireName` を通す
 *       (綴りを 2 か所で決めないため)。
 *     組み立ては sender_pure.ts に置く (マーカーの白フレームの判定や overflow の待ちの計算と同じ理由: DOM に触れない値の組み立てを
 *     sender.ts 本体に書くと vitest でテストできない)。index.ts の `sendFactsNow` と違い、
 *     1 秒に 1 回の間引きはしない (facts が変わるのは、段が下りたときと、受け手から届く
 *     recording / media が変わったときだけ)。src-vencord の製品コードにはテスト用の分岐を足さない。
 *     足すと実物の Discord 接続でもその分岐を通ることになり、製品の送信部をそのまま動かす
 *     という前提が崩れる。
 * 24. 録画の ON/OFF は受け手から届く `recording` をそのまま配る: `createSocket` のハンドラーに
 *     `onRecording` を足し、生きている画面のストリームすべてへ `session.setRecording(on)` を配る
 *     (製品の index.ts の `handleRecording` と同じ形)。環境変数で直接切り替える形にしないのは、
 *     製品の配線を通して確認するため。現在値を持つのは、走行中にエンコーダを作り直したとき
 *     (解像度の変更) に新しいセッションへ同じ値を渡すためである (製品の `startScreenVideo` が
 *     `start()` の前に `setRecording(recordingOn)` を呼ぶのと同じ)。切り替わりは
 *     `recordingChanges` として結果 JSON に残す。
 * 25. 走行中の解像度の変更はセッションを作り直す: 予定表 (`HARNESS_RESOLUTION_SCHEDULE`。綴りは
 *     `"<起動からの秒数>:<幅>x<高さ>"` の並び) の時刻になったら、そのストリームの
 *     `session.stop()` と、新しい寸法の材料 + 新しいセッションの `start()` を行う
 *     (製品の `stopScreenEntry` -> `startScreenVideo` と同じ形)。
 *     フレーム番号は続きから始める。材料のタイムスタンプは `epochUs0 + フレーム番号/fps` (音と同じく生成の予定時刻)
 *     なので、新しい材料を 0 から始めるとタイムスタンプが数十秒ぶん巻き戻り、受け手では「解像度が変わった」
 *     のではなく「タイムスタンプが大きく後戻りした」場合の処理を通る。作り直す前のセッションの `stats()` は
 *     `encodeBefore` として結果に残す (`encode` は最後のセッションのぶんしか持てないため)。
 * 26. 止めるときは、黒埋め用のフレームが出きるまで socket を閉じない: 製品の index.ts の
 *     `stop()` と同じ形で、`session.stop()` の後に `hasPendingWork()` を読み、真のストリームだけ
 *     `finished()` を待ってから `socketHandle.stop()` を呼ぶ。待たずに閉じると、録画が ON のまま
 *     終わるときに黒埋め用のフレームが行き先を失い、1 枚も届かない。待つ上限は製品と同じ 2 秒である
 *     (受け手は接続を 1 つしか受けないので、残りのないときまで開けたままにすると次の接続が
 *     失敗する)。
 * 27. 実映像の下地を真っ黒にしない (#303030): 判定ツール black_span.py は `blackframe` の既定
 *     (amount=98 / threshold=32) で黒のフレームを検出する。下地が `#000000` のままだと、動く円と
 *     フレーム番号の帯と数字を描いても明るいピクセルは全体の 2% に届かず、実映像のフレームが黒と
 *     判定される (同梱の ffmpeg での対照実験: 下地が黒だと 10 フレーム中 10 フレームが `pblack:98`
 *     で黒、下地を `#303030` にすると 0 フレーム)。黒埋め用のフレームは真っ黒 (`pblack:100`) なので、
 *     下地をしきい値 32 より上 (luma 48) にすれば、材料と実映像を確実に分けられる。帯の升と
 *     マーカーの白フレームは `#000000` / `#ffffff` で塗るので、`frames.py` と `sync.py` の読み方は
 *     変わらない。
 * 28. 受け手の `media` で汲み上げとエンコードを止める: `onMedia(false)` で画面のストリームをすべて止め
 *     (解像度の変更と同じく `encodeBefore` へ計数を残して `session.stop()`)、音のブロックは送らずに
 *     数えるだけにする。`onMedia(true)` で同じ寸法・続きのフレーム番号から作り直す (解像度の変更と同じ
 *     形)。閉じている間の `configure` は空にする (製品の index.ts がストリームを畳むと configure
 *     から消えるのと同じ)。閉じている間に起きたことを数えて結果の `gate` に残す (閉じている間に
 *     作ったエンコーダ、送った映像の実フレームと材料の枠、送らずに捨てた音のブロック)。値が変わった
 *     時刻は `mediaChanges` に残す。製品の「確立している間だけ動く」動作はこの送信側には入れない。
 *     「確立を待たずにエンコーダを確保し、繋がる前のフレームは数えて捨てる」形を、
 *     実機の実行の手順が前提にしているため。そのため、切断しても `media` の値は持ち越す
 *     (製品は切断で既定の真へ戻す。次の接続の `hello_ok` が現在値を運ぶので、同じ受け手へつなぎ直す
 *     場合に差は出ない)。
 * 29. 受け手の `limits` を適用する: 走っているストリームには `session.setLimits()` を渡し (下げた
 *     鍵だけがその場で動作する。encode/session.ts を参照)、新しく作るストリーム (解像度の変更や
 *     `media` で作り直すストリーム) は届いた値で作る。環境変数の上限 (`HARNESS_MAX_LONG_EDGE` /
 *     `HARNESS_FPS_DIVISOR`) とは鍵ごとに厳しいほうを取る (encode/limits.ts の `tightenLimits`)。
 *     受け手の既定 (制限なし) が届いても、環境変数で決めた上限を黙って外さないためである。
 *     届いた値と時刻は `limitsChanges` に残す。
 * 30. 画面のストリームごとに最初の寸法を変えられる (`HARNESS_SCREEN_SIZES`): 綴りは
 *     `"<幅>x<高さ>"` をカンマで並べたもの (`"1280x720,3840x2160"`)。i 番目の項目が i 番目の
 *     画面のストリームに適用され、項目のないストリーム・読めない項目・空の項目は 1920x1080 にする
 *     (sender_pure.ts の `parseScreenSizes`)。名前ラベルの板は枠の寸法に比例して描くので、寸法の
 *     違う枠を同じ OBS に並べて比べるにはこれが要る (走行中の解像度の予定表は全ストリームを同じ寸法に
 *     変えるので代わりにならない)。予定表とは重ねてよい。予定の時刻が来れば、その時刻からは全
 *     ストリームが予定の寸法になる。読めない項目を落として詰めないのは、詰めると後ろの項目が別の
 *     ストリームへずれ、どの枠が何 px かが実行の記録と食い違うため。実際に使った寸法は結果 JSON の
 *     `config.screenSizes` に出る (エンコードした寸法は `streams[].encodedFormat`。降格すれば縮む)。
 * 31. フレームだけを止める仕組み (`HARNESS_FRAMES_PAUSE_FILE`): 通知ファイルがある間、映像の
 *     材料 (`HarnessVideoFrameSource.next()`) はフレームを作らずに待つ。音・接続・ハートビート (pong)・
 *     facts・configure はそのまま動く。狙う形は「接続も音も保ったまま、映像のフレームだけが来なく
 *     なる」状態で、共有しているウィンドウを最小化したときの状態の代わりと、ドックのレベル 1 のメッセージの
 *     出し入れに使う。取り込みが止まればフレームが渡らない形にそろえ、エンコーダにも何も入れない。
 *     エンコードした後に捨てる形にはしない。受け手が持っていないフレームを参照する差分を送ることに
 *     なり、次の鍵フレームまで受け手の復号が崩れて、測りたい対象 (止まりの線と戻り) に送信側の
 *     作り物の傷が乗るため。
 *     - 再開したら現在の時刻のフレームへ飛ぶ (sender_pure.ts の `frameIndexAfterPause`。止まって
 *       いた間のフレームを後から作らない。実物と同じ)。overflow のときも飛ぶ。溜まりの飛ばし
 *       (VIDEO_STALE_SKIP_MS) は overflow のときに適用されないので、飛ばなければ止めた分を 4 倍の
 *       速さでまとめて出してしまう。
 *     - 再開のときに鍵フレームを自分からは出さない (製品の送信側はフレームが来ないことのほかに
 *       何も知らない)。止めていた間に受け手が求めていれば (onKeyframeRequest)、戻した最初の
 *       フレームが鍵フレームになる (encode/session.ts の requestKeyframe は次にエンコードする
 *       フレームに反映される)。
 *     - 中身が空なら全部の画面のストリーム、`"2"`・`"1,2"` ならその stream id のストリームだけ止める
 *       (`parseFramePauseTargets`。読める項目が 1 つも無ければ何も止めない。書き損じで全ストリームを
 *       黙って止めないため)。
 *     - `"stuck:2"` は、製品の送信側で読み出しかエンコードが止まった形で止める (`parseFramePauseStuck`)。
 *       受信側が stream_check で確認を頼むと、始め直したことにして止めるのを解き、restarted と答える
 *       (`FramePauseLedger.release`)。`"2"` で止めているストリームは Discord が送っていない形なので、
 *       not_receiving と答えて止めたままにする。受信側の受信のやり直し (映像が戻ること) を、OBS と
 *       この送信側だけで確認するための形である。答えは結果 JSON の `framesPause.checks` に残る。
 *     - レンダラーはファイルに触れられないので、FRAME_PAUSE_POLL_MS ごとに主プロセスへ尋ねる
 *       (主プロセスは読むだけで、作らない・消さない。frame_pause_file.js)。最初の材料を作る前に
 *       1 回読むので、起動のときにファイルがあれば最初のフレームから止まる。
 *     - 止めている状態は送信側に 1 つ (`FramePauseLedger`) で、解像度の変更と `media` の
 *       開き直しで作り直した材料にも適用される。
 *     - 記録は結果 JSON の `framesPause` (ストリームごとの区間。起動からの ms と、主プロセスが
 *       読んだシステム時刻と、作らなかったフレームの数)。時刻は尋ねる間隔の分 (約 100 ms) だけ粗い。
 *     - 止めている間もマーカーの音 (2 kHz) は鳴り、白のフレームは出ないので、A/V の判定
 *       (sync.py) は止めた区間を外して読む。止めていた間に過ぎた境界の白は、再開の後にも出さない
 *       (`settleSyncAfterPause`。何もしないと、再開の最初の 3 フレームが過ぎた境界の白になり、
 *       判定ツールが数秒前の音と対にする)。再開のときに初めて記録された境界は `syncEvents` に
 *       `whiteSkippedAfterPause: true` 付きで載る (音は鳴ったが白はない)。
 * 32. 実際の OBS の画面を撮るための 3 つの環境変数 (`HARNESS_SCREEN_NAMES` / `HARNESS_SCREEN_IMAGES` /
 *     `HARNESS_VOICE_USERS`): 表示名は profiles の display_name とアバターの文字に、画像はその画面のフレームに、声の
 *     userId は声のストリームの持ち主に使う。画像を描く画面には、フレーム番号の帯・動く図形・マーカーの白を描かない
 *     (frames.py と sync.py で読む実行には使わない)。画像は最初に 1 回だけ読み、解像度の変更と `media` の開き直しで
 *     作り直した材料にも同じ画像を渡す。読めなかった画像はそのストリームだけ動く図形に戻し、結果 JSON の `screenImages` に
 *     理由を残す (画像が 1 枚読めないことで送信そのものを止めない)。
 */
import {
    createAudioChunker,
    pushAudio,
    AUDIO_CHANNELS,
    AUDIO_SAMPLE_RATE_HZ,
    AUDIO_SAMPLES_PER_CHUNK,
    type AudioChunk,
} from "../../../src-vencord/audio/chunk";
import type { AudioSource } from "../../../src-vencord/audio/audio_source";
import { encodeHeader, FrameType, type FrameHeader } from "../../../src-vencord/core/wire";
import { AVATAR_CANVAS_SIZE } from "../../../src-vencord/core/avatar";
import {
    createEncodeSession,
    type EncodedVideoFormat,
    type EncodeSessionDeps,
    type EncodeSessionHandle,
    type EncodeSessionSink,
    type EncodeSessionStats,
} from "../../../src-vencord/encode/session";
import { limitsFromWire, tightenLimits, type EncodeSessionLimits } from "../../../src-vencord/encode/limits";
import { SerialGate } from "../../../src-vencord/encode/serial";
import { qpcTimebase, type SenderTimebase } from "../../../src-vencord/encode/sender_clock";
import { measureTicksOrigin, ticksOriginLogLine } from "../../../src-vencord/encode/ticks_origin";
import type { VideoFrameSource } from "../../../src-vencord/encode/frame_source";
import type { HardwareAccelerationPreference, LadderStage, SourceFormat } from "../../../src-vencord/encode/ladder";
import type {
    EncodedVideoChunk,
    EncodedVideoChunkMetadata,
    VideoEncoderConfig,
    VideoEncoderLike,
    VideoEncoderSupport,
    VideoFrame as StructuralVideoFrame,
} from "../../../src-vencord/encode/webcodecs_types";
import type { ConfigureStream, LimitsFields } from "../../../src-vencord/transport/control";
import {
    createSocket,
    type SocketHandle,
    type SocketStats,
    type WebSocketLike,
} from "../../../src-vencord/transport/socket";
import { CORDMUX_BUILD_ID } from "../../../src-vencord/version";
import {
    assignStreamIds,
    buildHarnessAvatars,
    buildHarnessFacts,
    buildHarnessProfiles,
    coverSourceRect,
    FramePauseLedger,
    type FramePauseCheck,
    harnessAvatarGlyph,
    harnessAvatarHue,
    frameIndexAfterPause,
    harnessIdentityLogLine,
    parseFailContact,
    parseFramePauseFile,
    parseHelloVersion,
    parseResolutionSchedule,
    parseScreenImages,
    parseScreenNames,
    parseScreenSizes,
    parseSyncTone,
    parseVoiceUsers,
    PerStreamWhiteFrameGate,
    settleSyncAfterPause,
    streamHasSyncTone,
    SyncCoordinatorPure,
    waitUntil,
    type FramePauseFileState,
    type FramePauseInterval,
    type HarnessFacts,
    type ResolutionChange,
    type ScreenSize,
    type SyncEvent,
    type SyncTone,
} from "./sender_pure";
import { fillTone, voiceToneIntervals, voiceToneOf, type VoiceToneInterval } from "./voice_tone";

/* ==========================================================================
   ブラウザ側 (preload.js) が晒す API の型
   ========================================================================== */

declare global {
    interface Window {
        harness: {
            report(obj: unknown): Promise<void>;
            progress(obj: unknown): void;
            readSession(): Promise<{ port: number; secret: string } | null>;
            /** 主プロセスの process.hrtime.bigint() (ns) を 10 進の文字列で返す (非同期の IPC。製品の native.ts の hrtimeNs と同じ仕組み)。 */
            hrtimeNsAsync(): Promise<string>;
            /** フレームだけを止める通知ファイルの現在の状態 (主プロセスが読む)。 */
            readFramePause(): Promise<FramePauseFileState>;
            /** 声に音を乗せるファイルの現在の状態 (主プロセスが読む。形はフレームを止めるファイルと同じ)。 */
            readVoiceTone(): Promise<FramePauseFileState>;
            /** 送ったアバターの PNG を書き出す (主プロセスが書く。avatar_export_file.js)。 */
            exportAvatar(userId: string, base64: string): Promise<AvatarExportState>;
            /** 画面に描く画像のファイルを読む (主プロセスが読む。screen_image_file.js)。 */
            readScreenImage(filePath: string): Promise<ScreenImageFileState>;
            env: Record<string, unknown>;
        };
        cordmuxSender: { run(params: HarnessRunParams): Promise<HarnessRunResult> };
    }
}

/** 主プロセスがアバターの PNG を書き出した結果 (avatar_export_file.js の exportAvatarPng が返す形)。 */
type AvatarExportState =
    | { state: "off" }
    | { state: "written"; file: string; bytes: number }
    | { state: "rejected"; reason: string }
    | { state: "error"; message: string };

/** 主プロセスが画像のファイルを読んだ結果 (screen_image_file.js の readScreenImageFile が返す形)。 */
type ScreenImageFileState =
    | { state: "off" }
    | { state: "rejected" }
    | { state: "absent" }
    | { state: "too-large"; bytes: number }
    | { state: "error"; message: string }
    | { state: "present"; bytes: Uint8Array };

/** 画面に描く画像を読んだ結果 (結果 JSON の `screenImages` の 1 行)。 */
interface ScreenImageLoad {
    /** 画面のストリームの、種類の中での番号 (0 始まり)。 */
    index: number;
    file: string;
    /** `loaded` か、主プロセスの読み取りの状態 (`absent` など) か、`decode-failed`。 */
    state: string;
    width?: number;
    height?: number;
    message?: string;
}

/* ==========================================================================
   固定値
   ========================================================================== */

const WIRE_VERSION = 1; // ワイヤ形式のバージョン (製品の index.ts の WIRE_VERSION と同じ値)。
const SCREEN_WIDTH = 1920;
const SCREEN_HEIGHT = 1080;
const BASELINE_BITRATE_BPS = 12_000_000; // encode/session.tsのBASELINE_BITRATE_BPSと同じ値。
const OVERFLOW_PAD_BYTES = 1_500_000; // backpressure.ts既定のしきい値(1MiB)を確実に超えさせる水増し量。
/** overflowのとき、待ちの目標時刻を何分の1にするか (実時間の4倍の速さで刻む)。 */
const OVERFLOW_PACE_DIVISOR = 4;
const VIDEO_STALE_SKIP_MS = 200; // 生成がこれ以上遅れたら溜まりを捨てて現在の番号へ飛ぶ。
/** 実映像の下地 (blackframe のしきい値 32 より上の luma 48)。 */
const MOVING_FRAME_BACKGROUND = "#303030";
/** 止めるときに出す黒埋め用のフレームを待つ上限。 */
const MATERIAL_DRAIN_TIMEOUT_MS = 2000;
/** フレームだけを止める通知ファイルを主プロセスへ尋ねる間隔。止まりの線の既定 2 秒に比べて十分に細かく、
 *  1 秒に 10 回の ipc は送信側の負荷にならない。 */
const FRAME_PAUSE_POLL_MS = 100;
/** 音の1ブロックぶんの時間 (480サンプル = 10ms)。メディアのタイムスタンプ (ブロックの番号 x 10 ms) に使う。 */
const AUDIO_CHUNK_US = BigInt(Math.round((AUDIO_SAMPLES_PER_CHUNK / AUDIO_SAMPLE_RATE_HZ) * 1_000_000));

function errInfo(e: unknown): { name: string; message: string } {
    if (e instanceof Error) return { name: e.name, message: e.message };
    return { name: "Error", message: String(e) };
}

/* ==========================================================================
   時計 (1つの時計だけを使う。映像・音・マーカーのすべてがこれを参照する)
   起動のときのシステム時刻を原点にし、performance.now() で進める。製品の送信側の時計
   (src-vencord/encode/sender_clock.ts) と同じく、システム時刻に合わせ直さない。
   ========================================================================== */

interface Clock {
    readonly startPerfMs: number;
    readonly epochUs0: bigint;
    /**
     * 製品の送信側の時計の決め方 (src-vencord/encode/sender_clock.ts)。ページの原点の TimeTicks を求められたときだけ qpc で、
     * この時計は製品と同じく TimeTicks (OBS の時計と同じ QueryPerformanceCounter の us) になる。求めなかったときは undefined
     * で、原点は起動のときのシステム時刻のまま (製品の page と同じく、受信側が差を決める)。
     */
    readonly timebase: SenderTimebase | undefined;
    nowUs(): bigint;
}

/**
 * ticksOriginUs (ページの原点の TimeTicks) を渡すと、製品の qpc と同じ時計にする。渡さなければ、起動のときのシステム時刻を
 * 原点にする。
 */
function createClock(ticksOriginUs?: bigint): Clock {
    const startPerfMs = performance.now();
    const epochUs0 =
        ticksOriginUs === undefined ? BigInt(Date.now()) * 1_000n : ticksOriginUs + BigInt(Math.round(startPerfMs * 1000));
    return {
        startPerfMs,
        epochUs0,
        timebase: ticksOriginUs === undefined ? undefined : qpcTimebase(ticksOriginUs),
        nowUs(): bigint {
            const elapsedUs = BigInt(Math.round((performance.now() - startPerfMs) * 1000));
            return epochUs0 + elapsedUs;
        },
    };
}

/* 待ち (waitUntil) は sender_pure.ts に切り出した (overflow のときに挟む setTimeout を
   vitest で当てるため。Clock はそこの ElapsedOrigin を満たす)。 */

/* ==========================================================================
   マーカー (白いフレーム + 2kHzの音、frames.py/sync.pyが検出する対象。syncStateと同じ設計)。
   実装 (SyncCoordinatorPure・PerStreamWhiteFrameGate) は sender_pure.ts に切り出した
   (window/OffscreenCanvasに依存しない純粋なロジックをvitestで当てるため)。
   ========================================================================== */

/* ==========================================================================
   映像の描画 (frames.pyが読む帯の形式はrenderer.jsとビット単位で一致させる)
   ========================================================================== */

/** 左上に16x16pxの升32個、白=1/黒=0、最上位ビットが左 (test/tools/obs-run/frames.pyの前提)。 */
function drawFrameNumberBits(ctx: OffscreenCanvasRenderingContext2D, frameNumber: number): void {
    const cellSize = 16;
    for (let bit = 0; bit < 32; bit++) {
        const shift = 31 - bit;
        const value = (frameNumber >>> shift) & 1;
        ctx.fillStyle = value ? "#ffffff" : "#000000";
        ctx.fillRect(bit * cellSize, 0, cellSize, cellSize);
    }
}

function drawSyncWhiteFrame(ctx: OffscreenCanvasRenderingContext2D, frameIndex: number, width: number, height: number): void {
    ctx.fillStyle = "#ffffff";
    ctx.fillRect(0, 0, width, height);
    drawFrameNumberBits(ctx, frameIndex);
}

/** 動く図形 + 人が読むための大きなフレーム番号 + 機械可読なビット帯 (ノイズ矩形は描かない)。 */
function drawMovingFrame(ctx: OffscreenCanvasRenderingContext2D, frameIndex: number, streamIndex: number, width: number, height: number): void {
    // 下地は真っ黒にしない (真っ黒だと black_span.py が実映像のフレームを
    // 黒埋め用のフレームと同じ「黒」に読む。同梱の ffmpeg で対照実験済み)。
    ctx.fillStyle = MOVING_FRAME_BACKGROUND;
    ctx.fillRect(0, 0, width, height);

    const cyclePeriod = 180; // renderer.jsと同じ周期 (フレーム数)。
    const phase = (frameIndex % cyclePeriod) / cyclePeriod;
    const sweep = phase < 0.5 ? phase * 2 : 2 - phase * 2; // 0->1->0の往復。
    const cx = 60 + sweep * (width - 120);
    const cy = height / 2;
    const hue = (streamIndex * 47) % 360; // ストリームごとに色を変え、見分けを付けやすくする。
    ctx.fillStyle = `hsl(${hue}, 70%, 55%)`;
    ctx.beginPath();
    ctx.arc(cx, cy, Math.min(width, height) * 0.05, 0, Math.PI * 2);
    ctx.fill();

    ctx.fillStyle = "#ffffff";
    ctx.font = `bold ${Math.round(height * 0.15)}px sans-serif`;
    ctx.textBaseline = "middle";
    ctx.fillText(String(frameIndex), 40, height * 0.7);

    drawFrameNumberBits(ctx, frameIndex);
}

/* ==========================================================================
   VideoFrameSource (encode/frame_source.ts のテスト用の送信側向けの実装)
   ========================================================================== */

class HarnessVideoFrameSource implements VideoFrameSource {
    readonly format: SourceFormat;
    private readonly ctx: OffscreenCanvasRenderingContext2D;
    private readonly canvas: OffscreenCanvas;
    private frameIndex: number;
    framesSkippedStale = 0; // 溜まりを捨てて飛ばしたフレーム数 (上の補足)
    private closed = false;
    private readonly stopPromise: Promise<void>;
    private resolveStop!: () => void;
    private readonly whiteGate = new PerStreamWhiteFrameGate(); // ストリームごとに白フレームの残数を持つ。

    constructor(
        private readonly streamIndex: number,
        width: number,
        height: number,
        fps: number,
        private readonly overflow: boolean,
        private readonly clock: Clock,
        private readonly sync: SyncCoordinatorPure,
        // 走行中に作り直したときは、フレーム番号 (= タイムスタンプの元) を続きから
        // 始める。0 に戻すと材料のタイムスタンプがそのストリームだけ数十秒ぶん巻き戻る。
        startFrameIndex = 0,
        // フレームだけを止める仕組み (送信側に 1 つの状態と、この材料の stream id)。使わないときは null。
        private readonly pause: { readonly ledger: FramePauseLedger; readonly streamId: number } | null = null,
        // このストリームに描く画像 (HARNESS_SCREEN_IMAGES)。null なら動く図形と帯とマーカーの白を描く。
        private readonly image: ImageBitmap | null = null,
    ) {
        this.frameIndex = startFrameIndex;
        this.format = { width, height, fps };
        this.canvas = new OffscreenCanvas(width, height);
        const ctx = this.canvas.getContext("2d");
        if (ctx === null) throw new Error("harness/sender: OffscreenCanvasの2dコンテキストを取得できません");
        this.ctx = ctx;
        // 画像は動かないので、最初に 1 回だけ描いておき、フレームごとにはこの絵をそのまま渡す。
        if (image !== null) {
            const r = coverSourceRect(image.width, image.height, width, height);
            ctx.imageSmoothingQuality = "high";
            ctx.drawImage(image, r.x, r.y, r.width, r.height, 0, 0, width, height);
        }
        this.stopPromise = new Promise((resolve) => {
            this.resolveStop = resolve;
        });
    }

    async next(): Promise<StructuralVideoFrame | null> {
        // totalFrames を持たず close() されるまでフレームを作り続ける (measured
        // window とフレームの材料が尽きる時刻がずれないようにするため。session.stop() が
        // close() を呼ぶまで、実際にどれだけ確立が遅れても末尾が無映像にならない)。
        if (this.closed) return null;
        let frameIndex = this.frameIndex++;
        // 起動や詰まりで生成が予定より VIDEO_STALE_SKIP_MS 以上遅れて
        // いたら、溜まった分のフレームは作らずに現在の番号へ飛ぶ。実物の取り込みは過去のフレームを後から
        // 作らないのに、テスト用の送信側だけが過去のタイムスタンプのフレームをまとめて出すと、受け手の OBS がそのストリームの
        // 表示の原点をまとめて出した分だけ遅く置き、ストリームごとの表示の遅れが数百 ms ずれる (画面 4 + 声 8 で実測:
        // 画面 1 に対して画面 2 から 4 の白が 130 から 340 ms 遅れて録画に出た)。
        if (!this.overflow) {
            const elapsedNowMs = Number(this.clock.nowUs() - this.clock.epochUs0) / 1000;
            const nominalMs = ((frameIndex + 1) * 1000) / this.format.fps;
            if (elapsedNowMs - nominalMs > VIDEO_STALE_SKIP_MS) {
                const caughtUp = Math.floor((elapsedNowMs * this.format.fps) / 1000);
                this.framesSkippedStale += caughtUp - frameIndex;
                frameIndex = caughtUp;
                this.frameIndex = caughtUp + 1;
            }
        }

        // fixed-rateのスケジューラ (runFixedRateLoopと同じ発想)。遅れていれば
        // waitMsが0になり、次のnext()呼び出しがすぐ返って追いつく。
        // overflowでも待ちを外さず、目標時刻を1/4 (実時間の4倍の速さ) にする。
        const paceDivisor = this.overflow ? OVERFLOW_PACE_DIVISOR : 1;
        const targetElapsedMs = ((frameIndex + 1) * 1000) / this.format.fps / paceDivisor;
        const outcome = await waitUntil(targetElapsedMs, this.clock, this.stopPromise, this.overflow);
        if (outcome === "stopped") return null;
        if (this.closed) return null;

        // 通知ファイルで止めている間はフレームを作らずに待つ (エンコーダにも何も入らない)。
        // 再開したら現在の時刻のフレームへ飛び、作らなかった数を区間に足す。
        if (this.pause !== null && this.pause.ledger.isPaused(this.pause.streamId)) {
            const resumed = await Promise.race([
                this.pause.ledger.whenResumed(this.pause.streamId).then(() => true),
                this.stopPromise.then(() => false),
            ]);
            if (!resumed || this.closed) return null;
            const elapsedNowMs = Number(this.clock.nowUs() - this.clock.epochUs0) / 1000;
            const after = frameIndexAfterPause(frameIndex, elapsedNowMs, this.format.fps, paceDivisor);
            this.pause.ledger.addWithheld(this.pause.streamId, after.withheld);
            frameIndex = after.frameIndex;
            this.frameIndex = frameIndex + 1;
            // 止めていた間に過ぎたマーカーの白を、再開の後に遅れて出さない (sender_pure.ts の settleSyncAfterPause)。
            settleSyncAfterPause(
                this.sync,
                this.whiteGate,
                this.clock.epochUs0 + BigInt(Math.round((frameIndex * 1_000_000) / this.format.fps)),
                BigInt(Math.round(1_000_000 / this.format.fps)),
            );
        }

        // 映像のメディアのタイムスタンプも音と同じく「生成の予定時刻」
        // = フレームの番号 / fps にする。生成時刻で刻むと描画の詰まりがそのままタイムスタンプに写り (実物の
        // VideoFrame.timestamp は取り込み側が付けるので写らない)、画面 4 + 声 8 で整列の飛びになる
        // (実測: 生成時刻のままでは定常の欠落が 0.23 から 0.52%)。マーカーの境界も、白のフレームとマーカーの音の両方を
        // この予定時刻で決める (sender_pure.ts の SyncCoordinatorPure)。
        const timestampUs = this.clock.epochUs0 + BigInt(Math.round((frameIndex * 1_000_000) / this.format.fps));
        this.sync.noteVideoTick(timestampUs);

        // 画像を描くストリームは、コンストラクターで描いた絵のまま (帯もマーカーの白も描かない)。
        if (this.image === null) {
            if (this.whiteGate.consume(this.sync.currentBoundary)) {
                drawSyncWhiteFrame(this.ctx, frameIndex, this.format.width, this.format.height);
            } else {
                drawMovingFrame(this.ctx, frameIndex, this.streamIndex, this.format.width, this.format.height);
            }
        }

        return new VideoFrame(this.canvas, { timestamp: Number(timestampUs) }) as unknown as StructuralVideoFrame;
    }

    close(): void {
        if (this.closed) return;
        this.closed = true;
        this.resolveStop();
    }
}

/* ==========================================================================
   AudioSource (audio/audio_source.ts のテスト用の送信側向けの実装。1kHz正弦波、マーカー中2kHz)
   ========================================================================== */

class HarnessAudioSource implements AudioSource {
    private chunkIndex = 0;
    private closed = false;
    private phase = 0; // 周波数が切り替わっても位相を連続させる (クリックノイズを避ける)。
    private readonly stopPromise: Promise<void>;
    private resolveStop!: () => void;

    constructor(
        private readonly overflow: boolean,
        private readonly clock: Clock,
        private readonly sync: SyncCoordinatorPure,
        /** このストリームがマーカーの音 (2kHz) を出すか (判定は sender_pure.ts)。 */
        private readonly hasSyncTone: boolean,
        /** 声のストリームに 1 kHz の音を乗せるか (voice_tone.ts)。null なら乗せない。チャンクごとに尋ねる。 */
        private readonly toneOn: (() => boolean) | null = null,
    ) {
        this.stopPromise = new Promise((resolve) => {
            this.resolveStop = resolve;
        });
    }

    async next(): Promise<{ samples: Float32Array; mediaTimestampUs: number } | null> {
        // totalChunks を持たず close() されるまで作り続ける。
        if (this.closed) return null;
        const chunkIndex = this.chunkIndex++;

        // overflowでも待ちを外さず、目標時刻を1/4 (実時間の4倍の速さ) にする。
        const chunkMs = Number(AUDIO_CHUNK_US) / 1000; // 10ms
        const paceDivisor = this.overflow ? OVERFLOW_PACE_DIVISOR : 1;
        const targetElapsedMs = ((chunkIndex + 1) * chunkMs) / paceDivisor;
        const outcome = await waitUntil(targetElapsedMs, this.clock, this.stopPromise, this.overflow);
        if (outcome === "stopped") return null;
        if (this.closed) return null;

        // メディアのタイムスタンプは「生成の予定時刻」= ブロックの番号 x 10ms。
        // 作るのが遅れても、その遅れはタイムスタンプに反映しない (実物の AudioData.timestamp に当たる)。
        const mediaTimestampUs = this.clock.epochUs0 + BigInt(chunkIndex) * AUDIO_CHUNK_US;
        // マーカーの音を持たないストリームは無音 (0 を詰めたブロック。ブロックの数と大きさは同じ)。
        // 判定ツール sync.py は録画のミックスで 2kHz と 1kHz のエネルギーを比べるので、他のストリームが 1kHz を
        // 出し続けるとマーカーのストリームの 2kHz が 1kHz を上回れず、マーカーが 1 つも見つからない
        // (画面 1 + 声 1 で実測: 白 36 に対してマーカーの音 0)。
        // 声に音を乗せるファイルで指定された声のストリームは、1 kHz の音を出す (voice_tone.ts)。
        const tone = voiceToneOf(
            this.hasSyncTone,
            this.toneOn !== null && this.toneOn(),
            this.sync.audioFrequencyAt(mediaTimestampUs),
        );
        const samples = new Float32Array(AUDIO_SAMPLES_PER_CHUNK * AUDIO_CHANNELS);
        this.phase = fillTone(samples, AUDIO_CHANNELS, AUDIO_SAMPLE_RATE_HZ, tone.frequencyHz, tone.amplitude, this.phase);
        return { samples, mediaTimestampUs: Number(mediaTimestampUs) };
    }

    close(): void {
        if (this.closed) return;
        this.closed = true;
        this.resolveStop();
    }
}

/* ==========================================================================
   WebCodecs / WebSocket への橋渡し (index.tsの複製)
   ========================================================================== */

function createEncoder(init: {
    output(chunk: EncodedVideoChunk, metadata: EncodedVideoChunkMetadata): void;
    error(e: unknown): void;
}): VideoEncoderLike {
    return new VideoEncoder({
        output: (chunk, metadata) => init.output(chunk as unknown as EncodedVideoChunk, metadata as EncodedVideoChunkMetadata),
        error: (e: unknown) => init.error(e),
    }) as unknown as VideoEncoderLike;
}

async function isConfigSupportedReal(config: VideoEncoderConfig): Promise<VideoEncoderSupport> {
    const support = await VideoEncoder.isConfigSupported(config as unknown as globalThis.VideoEncoderConfig);
    return { supported: support.supported === true };
}

function makeBlackFrame(width: number, height: number, timestampUs: number): StructuralVideoFrame {
    const canvas = new OffscreenCanvas(width, height);
    const ctx = canvas.getContext("2d");
    if (ctx) {
        ctx.fillStyle = "black";
        ctx.fillRect(0, 0, width, height);
    }
    return new VideoFrame(canvas, { timestamp: timestampUs }) as unknown as StructuralVideoFrame;
}

/** 縮小に使うOffscreenCanvasを寸法ごとに使い回す。 */
const scaleCanvases = new Map<string, OffscreenCanvas>();

function scaleCanvasFor(width: number, height: number): OffscreenCanvas {
    const key = `${width}x${height}`;
    const cached = scaleCanvases.get(key);
    if (cached) return cached;
    const canvas = new OffscreenCanvas(width, height);
    scaleCanvases.set(key, canvas);
    return canvas;
}

/** encode/session.tsの`deps.scaleFrame`の実物。
 *  契約どおり、成功しても失敗しても元のフレームはこの関数がcloseする。 */
function scaleFrame(frame: StructuralVideoFrame, width: number, height: number): StructuralVideoFrame {
    const source = frame as unknown as VideoFrame;
    try {
        const canvas = scaleCanvasFor(width, height);
        const ctx = canvas.getContext("2d");
        if (ctx === null) throw new Error("cordmux: OffscreenCanvas 2d context unavailable");
        ctx.drawImage(source, 0, 0, width, height);
        return new VideoFrame(canvas, {
            timestamp: source.timestamp,
            duration: source.duration ?? undefined,
            alpha: "discard",
        }) as unknown as StructuralVideoFrame;
    } finally {
        frame.close();
    }
}

/** failStage段まで故意にisConfigSupportedを不支持にする。 */
function buildEncodeDeps(gate: SerialGate, clock: Clock, failStage: number): EncodeSessionDeps {
    let consecutiveForcedFailures = 0;
    return {
        createEncoder,
        async isConfigSupported(config: VideoEncoderConfig): Promise<VideoEncoderSupport> {
            if (consecutiveForcedFailures >= 4) consecutiveForcedFailures = 0; // 次のセッションの1段目とみなす防御。
            if (consecutiveForcedFailures < failStage) {
                consecutiveForcedFailures++;
                return { supported: false };
            }
            consecutiveForcedFailures = 0;
            return isConfigSupportedReal(config);
        },
        gate,
        nowUs: () => clock.nowUs(),
        timebase: clock.timebase,
        makeBlackFrame,
        scaleFrame,
    };
}

/** 実物のWebSocketをtransport/socket.tsのWebSocketLikeへ薄く包む (index.tsの
 *  wrapWebSocketと同じ実装。TS 5.7以降のUint8Array<ArrayBufferLike>とDOMのArrayBufferView<
 *  ArrayBuffer>の食い違いも同じキャストで避ける)。 */
function wrapWebSocket(url: string): WebSocketLike {
    const raw = new WebSocket(url);
    const wrapped: WebSocketLike = {
        get bufferedAmount(): number {
            return raw.bufferedAmount;
        },
        send(data: string | Uint8Array): void {
            if (typeof data === "string") {
                raw.send(data);
                return;
            }
            raw.send(data as unknown as ArrayBufferView<ArrayBuffer>);
        },
        close(): void {
            raw.close();
        },
        onopen: null,
        onclose: null,
        onerror: null,
        onmessage: null,
    };
    raw.addEventListener("open", () => wrapped.onopen?.());
    raw.addEventListener("close", () => wrapped.onclose?.());
    raw.addEventListener("error", (ev) => wrapped.onerror?.(ev));
    raw.addEventListener("message", (ev) => wrapped.onmessage?.({ data: ev.data }));
    return wrapped;
}

/* ==========================================================================
   ワイヤへの積み出し (index.tsのvideoSink/sendAudioChunk/concatHeaderPayloadと同じ形)
   ========================================================================== */

function concatHeaderPayload(header: FrameHeader, payload: Uint8Array): Uint8Array {
    const headerBytes = encodeHeader(header);
    const out = new Uint8Array(headerBytes.length + payload.length);
    out.set(headerBytes, 0);
    out.set(payload, headerBytes.length);
    return out;
}

/** overflowのときだけAnnex Bの末尾にゼロを足して転送量を水増しする。 */
function padForOverflow(payload: Uint8Array, overflow: boolean): Uint8Array {
    if (!overflow) return payload;
    const padded = new Uint8Array(payload.length + OVERFLOW_PAD_BYTES);
    padded.set(payload, 0);
    return padded;
}

// getSocket() が null (socketHandle をまだ作っていない = ラダー確保・温めの
// 直列実行の途中) の間に作られたフレーム/音は捨てる。捨てた数は
// dropped に数えて結果に残す。
interface DroppedBeforeSocketCounter {
    video: number;
    audio: number;
}

/**
 * 受け手の `media` の現在値と、閉じている間に起きたことの計数。閉じている間の
 * 数がすべて 0 であることが、この送信側で見る「ゲートが閉じている間は何も作らない・送らない」
 * 確認の材料になる。
 */
interface HarnessMediaGate {
    open: boolean;
    /** 閉じている間に作ったエンコーダの数。 */
    encodersCreatedWhileClosed: number;
    /** 閉じている間に送った映像の実フレーム (録画材料の旗のない枠) の数。 */
    videoFramesSentWhileClosed: number;
    /** 閉じている間に送った録画材料の枠の数 (畳むときに出す黒埋め用のフレームはここに出る)。 */
    materialFramesSentWhileClosed: number;
    /** 閉じている間に作られ、送らずに捨てた音のブロックの数。 */
    audioBlocksSkippedWhileClosed: number;
}

function videoSink(
    getSocket: () => SocketHandle | null,
    overflow: boolean,
    stageHistory: LadderStage[],
    encodedFormat: { value?: EncodedVideoFormat },
    dropped: DroppedBeforeSocketCounter,
    onStageChanged: () => void,
    mediaGate: HarnessMediaGate,
): EncodeSessionSink {
    return {
        sendVideo(header, payload): void {
            const socket = getSocket();
            if (!socket) {
                dropped.video++;
                return;
            }
            // 閉じている間に出た枠を数える (止めずに数えるだけ。ストリームは閉じた時点で
            // 止めてあるので、ここに来るのは畳むときの材料か、止め損ねたストリームのフレームだけのはず)。
            if (!mediaGate.open) {
                if (header.flags.recordingOnly) mediaGate.materialFramesSentWhileClosed++;
                else mediaGate.videoFramesSentWhileClosed++;
            }
            const outPayload = padForOverflow(payload, overflow);
            socket.sendBinary(concatHeaderPayload(header, outPayload), "video");
        },
        onStage(stage, format): void {
            stageHistory.push(stage);
            // configureのwidth/height/fpsは「いま実際にエンコードしている
            // 寸法」。段が変われば寸法も変わるので保存しておき、configureを送り直す側
            // (onStageChanged) がこれを読む。
            encodedFormat.value = format;
            // 段が変われば受け手の「画質」の欄も変わるので facts を送り直す
            // (繋がる前の確保の途中もここを通るが、そのときは送り先がまだないので何もしない)。
            onStageChanged();
        },
    };
}

/** socket が無ければ dropped に数えるだけで false を返す (実際に送れたら true)。 */
function sendAudioChunk(getSocket: () => SocketHandle | null, streamId: number, chunk: AudioChunk, dropped: DroppedBeforeSocketCounter): boolean {
    const socket = getSocket();
    if (!socket) {
        dropped.audio++;
        return false;
    }
    const header: FrameHeader = {
        type: FrameType.Audio,
        streamId,
        timestampUs: chunk.timestampUs,
        flags: { keyframe: false, recordingOnly: false },
    };
    const payload = new Uint8Array(chunk.samples.buffer, chunk.samples.byteOffset, chunk.samples.byteLength);
    socket.sendBinary(concatHeaderPayload(header, payload), "audio");
    return true;
}

async function pumpAudio(
    source: AudioSource,
    streamId: number,
    getSocket: () => SocketHandle | null,
    clock: Clock,
    counter: { count: number },
    reanchor: { count: number },
    dropped: DroppedBeforeSocketCounter,
    mediaGate: HarnessMediaGate,
): Promise<void> {
    let chunker = createAudioChunker();
    let wasOpen = mediaGate.open;
    for (;;) {
        const block = await source.next();
        if (block === null) return;
        // 閉じている間は整形も送信もせず、捨てたブロックを数える。開き直したら
        // 整形の状態を作り直す (製品は開くたびに新しい汲み上げの API と整形を作る)。
        if (!mediaGate.open) {
            mediaGate.audioBlocksSkippedWhileClosed++;
            wasOpen = false;
            continue;
        }
        if (!wasOpen) {
            chunker = createAudioChunker(clock.timebase);
            wasOpen = true;
        }
        // 到着の時刻はここで測る (製品の index.ts の pumpAudio と同じ形)。
        const arrivalUs = clock.nowUs();
        const result = pushAudio(chunker, block.samples, block.mediaTimestampUs, arrivalUs);
        chunker = result.state;
        reanchor.count = chunker.clockMap.restarts;
        for (const chunk of result.chunks) {
            if (sendAudioChunk(getSocket, streamId, chunk, dropped)) counter.count++;
        }
    }
}

/* ==========================================================================
   run() の組み立て
   ========================================================================== */

export interface HarnessRunParams {
    nScreen: number;
    nVoice: number;
    seconds: number;
    fps: number;
    hw: HardwareAccelerationPreference;
    warmup: boolean;
    keyintSec: number;
    syncEverySec: number;
    bitrate: number;
    failStage: number;
    overflow: boolean;
    maxLongEdge?: number;
    fpsDivisor: 1 | 2;
    /** マーカーの音 (2kHz) をどのストリームに乗せるか (既定は "screen")。 */
    syncTone: SyncTone;
    /** 接続確立後にこの接点 id を Missing として facts.warnings に載せる (接点を 1 つ
     *  潰した状態を作るため)。未指定なら facts を1通も送らない)。 */
    failContact?: string;
    /** 走行中に解像度を変える予定表。 */
    resolutionSchedule: ResolutionChange[];
    /** 画面のストリームごとの最初の寸法。i 番目が i 番目の画面のストリーム。足りないストリームは 1920x1080。 */
    screenSizes?: ScreenSize[];
    /** 画面のストリームごとの表示名 (null はそのストリームの userId のまま)。i 番目が i 番目の画面のストリーム。 */
    screenNames?: (string | null)[];
    /** 画面のストリームごとに描く画像のファイルの場所 (null は動く図形)。i 番目が i 番目の画面のストリーム。 */
    screenImages?: (string | null)[];
    /** 声のストリームごとの userId。i 番目が i 番目の声のストリーム。未指定なら `harness-voice-<stream id>`。 */
    voiceUsers?: string[];
    /** フレームだけを止める通知ファイルの場所。未指定なら止めない (既定どおり)。 */
    framesPauseFile?: string;
    /** 声に音を乗せるファイルの場所 (voice_tone.ts)。未指定なら、声は既定どおり無音。 */
    voiceToneFile?: string;
    /** 送ったアバターの PNG を書き出すフォルダー (avatar_export_file.js)。未指定なら書き出さない。 */
    avatarExportDir?: string;
    /** hello の `version` に載せる文字列。 */
    helloVersion: string;
    /**
     * 送信側の時計。qpc なら製品と同じくページの原点の TimeTicks を求め、OBS の時計と同じ時計で送る (hello の clock_base)。
     * page なら起動のときのシステム時刻を原点にする古い形 (受信側が差を決める)。環境変数 HARNESS_CLOCK_BASE (既定 qpc)。
     */
    clockBase: "qpc" | "page";
}

export interface HarnessStreamResult {
    kind: "screen" | "voice";
    streamId: number;
    stageHistory: LadderStage[];
    encode: EncodeSessionStats | null;
    /** 最後に configure へ出した寸法とfps (結果 JSON をドックの解像度の欄と
     *  突き合わせるための診断用。声と、段が決まらなかったストリームは null)。 */
    encodedFormat: EncodedVideoFormat | null;
    /** 走行中に作り直したときの、前のセッションの最終の計数 (起きた順)。
     *  作り直していなければ空。`encode` は最後のセッションのぶんしか持てないため。 */
    encodeBefore: EncodeSessionStats[];
    audioChunksSent: number;
    /** 音の換算の定数を決め直した回数 (encode/clock_map.ts の restarts)。 */
    audioReanchorCount: number;
    /** このストリームがマーカーの音 (2kHz) を出したか 。 */
    hasSyncTone: boolean;
}

export interface HarnessRunResult {
    mode: "product";
    config: HarnessRunParams;
    establishedReached: boolean;
    socketStats: SocketStats | null;
    streams: HarnessStreamResult[];
    syncEvents: SyncEvent[];
    // socketHandle をまだ作っていない間 (ラダー確保・温めの直列実行の途中) に
    // 作られたフレーム/音は送れないので、無かったことにせず数だけでも残す。
    droppedBeforeSocket: { video: number; audio: number } | null;
    /** 実際に送った facts の最後の中身 (送るものが無ければ null)。
     *  結果 JSON を読む人が録画を見ずに「送れたか」「どの段を送ったか」を確認できる
     *  ようにするための診断用。 */
    factsSent: HarnessFacts | null;
    /** 受け手から届いた録画の ON/OFF。起動からのミリ秒と値。
     *  「途中 ON がいつ届いたか」を録画を見ずに読めるようにするための診断用。 */
    recordingChanges: { atMs: number; on: boolean }[];
    /** 走行中に実際に作り直した解像度。予定表どおりに進んだかを見る。
     *  `deferred` は `media` で閉じている間に来た変更 (寸法だけを覚え、開き直すときに使う)。 */
    resolutionChanges: { atMs: number; streamId: number; width: number; height: number; ok: boolean; deferred?: boolean }[];
    /** 受け手から届いた `media`。起動からのミリ秒と値 (値が変わったときだけ)。 */
    mediaChanges: { atMs: number; on: boolean }[];
    /** 受け手から届いた `limits`。起動からのミリ秒とワイヤの形の値。 */
    limitsChanges: { atMs: number; limits: LimitsFields }[];
    /** `media` で閉じている間に起きたことの計数。途中で失敗したときは null。 */
    gate: Omit<HarnessMediaGate, "open"> | null;
    /** 止めるときに出す黒埋め用のフレームを待ったときの実測 (待ったストリームの数と掛かったミリ秒)。 */
    materialDrain: { waitedStreams: number; waitedMs: number; timedOut: boolean } | null;
    /** フレームだけを止めた区間 (ストリームごと)。通知ファイルを使わなかったときと途中で失敗したときは null。
     *  `ignoredTargets` は中身にあって画面のストリームの id でなかった項目、`readErrors` は読めずに前の状態を保った回数。 */
    framesPause: {
        file: string;
        intervals: FramePauseInterval[];
        ignoredTargets: string[];
        readErrors: number;
        /** 受信側の stream_check に答えた記録 (FramePauseLedger.release)。 */
        checks: FramePauseCheck[];
    } | null;
    /** 声に音を乗せた区間 (ストリームごと)。ファイルを使わなかったときと、途中で失敗したときは null。
     *  `ignoredTargets` は中身にあって声のストリームの番号でなかった項目、`readErrors` は読めずに前の状態を保った回数。 */
    voiceTone: { file: string; intervals: VoiceToneInterval[]; ignoredTargets: string[]; readErrors: number } | null;
    /** 送ったアバターの PNG の書き出し (avatar_export_file.js)。フォルダーを渡さなかったときと、途中で失敗したときは null。
     *  `written` は書き出した userId、`failed` は書き出せなかった userId と結果の状態。 */
    avatarExport: { dir: string; written: string[]; failed: { userId: string; state: string }[] } | null;
    /** 画面に描く画像を読んだ結果 (画像を渡したストリームだけ)。画像を 1 枚も渡さなかったときと、途中で失敗したときは null。 */
    screenImages: ScreenImageLoad[] | null;
    /** 送信側の時計を決めたときのログの行 (decideHarnessClock)。決める前に失敗したときは null。 */
    clock: string | null;
    aborted: boolean;
    fatalError: { name: string; message: string } | null;
}

interface ScreenEntry {
    readonly streamId: number;
    readonly userId: string;
    /** 走行中の解像度の変更で作り直すので、この 2 つだけ差し替えられる。 */
    videoSource: HarnessVideoFrameSource;
    session: EncodeSessionHandle;
    readonly audioSource: HarnessAudioSource;
    readonly stageHistory: LadderStage[];
    /** いま実際にエンコードしている寸法とfps (onStageの第2引数)。
     *  段がまだ決まっていない間とUnavailableのときはvalueを持たない。 */
    readonly encodedFormat: { value?: EncodedVideoFormat };
    readonly audioSent: { count: number };
    readonly audioReanchor: { count: number };
    readonly hasSyncTone: boolean;
    /** 作り直す前のセッションの最終の計数 (新しい順ではなく起きた順)。 */
    readonly encodeBefore: EncodeSessionStats[];
    /** `media` で閉じて止めてある (開き直すときに作り直す)。 */
    gated: boolean;
    /** 現在の材料の寸法 (閉じている間に来た解像度の変更もここに覚える)。 */
    size: { width: number; height: number };
}

interface VoiceEntry {
    readonly streamId: number;
    readonly userId: string;
    readonly audioSource: HarnessAudioSource;
    readonly audioSent: { count: number };
    readonly audioReanchor: { count: number };
    readonly hasSyncTone: boolean;
}

/**
 * テスト用の人のアバターを作る。userId ごとに色が違う、AVATAR_CANVAS_SIZE 四方の PNG を base64 にして返す
 * (製品の index.ts の toPngBase64 と同じく OffscreenCanvas で描く)。中に 1 文字 (表示名があればその先頭、なければ userId の
 * 末尾。sender_pure.ts の harnessAvatarGlyph) を白で描き、並びの中でどの人か見分けられるようにする。
 */
async function makeHarnessAvatarPng(userId: string, displayName: string | null = null): Promise<string> {
    const canvas = new OffscreenCanvas(AVATAR_CANVAS_SIZE, AVATAR_CANVAS_SIZE);
    const ctx = canvas.getContext("2d");
    if (ctx === null) return "";
    ctx.fillStyle = `hsl(${harnessAvatarHue(userId)} 65% 45%)`;
    ctx.fillRect(0, 0, AVATAR_CANVAS_SIZE, AVATAR_CANVAS_SIZE);
    ctx.fillStyle = "#ffffff";
    ctx.font = `bold ${Math.round(AVATAR_CANVAS_SIZE * 0.6)}px sans-serif`;
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillText(harnessAvatarGlyph(userId, displayName), AVATAR_CANVAS_SIZE / 2, AVATAR_CANVAS_SIZE / 2);
    const png = await canvas.convertToBlob({ type: "image/png" });
    const bytes = new Uint8Array(await png.arrayBuffer());
    let binary = "";
    for (const byte of bytes) binary += String.fromCharCode(byte);
    return btoa(binary);
}

/**
 * 画面のストリームごとの画像を主プロセスに読ませ、ImageBitmap にする。i 番目が i 番目の画面のストリームで、画像を渡さなかった
 * ストリームと読めなかったストリームは null (動く図形に戻る)。読んだ結果は loads に足す (画像を渡したストリームだけ)。
 */
async function loadScreenImages(files: readonly (string | null)[], loads: ScreenImageLoad[]): Promise<(ImageBitmap | null)[]> {
    const out: (ImageBitmap | null)[] = [];
    for (const [index, file] of files.entries()) {
        if (file === null) {
            out.push(null);
            continue;
        }
        let read: ScreenImageFileState;
        try {
            read = await window.harness.readScreenImage(file);
        } catch (e) {
            read = { state: "error", message: errInfo(e).message };
        }
        if (read.state !== "present") {
            loads.push({ index, file, state: read.state, ...("message" in read ? { message: read.message } : {}) });
            out.push(null);
            continue;
        }
        try {
            // 主プロセスから届くのは Uint8Array (構造化複製)。TS 5.7 以降の BlobPart の型との食い違いは wrapWebSocket と同じキャストで避ける。
            const bitmap = await createImageBitmap(new Blob([read.bytes as unknown as ArrayBufferView<ArrayBuffer>]));
            loads.push({ index, file, state: "loaded", width: bitmap.width, height: bitmap.height });
            out.push(bitmap);
        } catch (e) {
            loads.push({ index, file, state: "decode-failed", message: errInfo(e).message });
            out.push(null);
        }
    }
    return out;
}

function sleepMs(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

function buildConfigureStreams(screens: readonly ScreenEntry[], voices: readonly VoiceEntry[]): ConfigureStream[] {
    const streams: ConfigureStream[] = [];
    for (const e of screens) {
        // 「いま実際にエンコードしている寸法」を出す (材料の解像度ではない)。
        // 段がまだ決まっていない間とUnavailableのときは3つとも省く (ワイヤ形式では任意の項目)。
        const format = e.encodedFormat.value;
        streams.push({
            stream_id: e.streamId,
            user_id: e.userId,
            kind: "screen",
            width: format?.width,
            height: format?.height,
            fps: format?.fps,
            audio: true,
        });
    }
    for (const e of voices) {
        streams.push({ stream_id: e.streamId, user_id: e.userId, kind: "voice" });
    }
    return streams;
}

/**
 * 製品と同じく、ページの原点の TimeTicks を求めて (src-vencord/encode/ticks_origin.ts)、送信側の時計を決める。params.clockBase
 * が page なら求めない。返す行は、受信側に送るログの行 (製品の index.ts の clock の行と同じ形)。
 */
async function decideHarnessClock(params: HarnessRunParams): Promise<{ clock: Clock; line: string }> {
    if (params.clockBase === "page") return { clock: createClock(), line: "clock: base=page reason=harness-setting" };
    const result = await measureTicksOrigin({
        nowMs: () => performance.now(),
        readHrtimeNs: () => window.harness.hrtimeNsAsync(),
        crossOriginIsolated: self.crossOriginIsolated === true,
    });
    return {
        clock: createClock(result.ok ? result.originUs : undefined),
        line: `${ticksOriginLogLine(result)} coi=${self.crossOriginIsolated === true}`,
    };
}

async function runProduct(params: HarnessRunParams): Promise<HarnessRunResult> {
    const { clock, line: clockLine } = await decideHarnessClock(params);
    // マーカーの境界はメディアのタイムスタンプで決める (sender_pure.ts の SyncCoordinatorPure) ので、タイムスタンプの原点を渡す。
    const sync = new SyncCoordinatorPure(params.syncEverySec, clock.epochUs0);
    const gate = new SerialGate(); // エンコーダの確保は全ストリームで直列にする。
    // 受け手の media の現在値 (既定は開けたまま = 古い受け手と同じ) と、閉じている
    // 間の計数。エンコーダの作成も数えるため、作り手を包んでから encodeDeps に渡す。
    const mediaGate: HarnessMediaGate = {
        open: true,
        encodersCreatedWhileClosed: 0,
        videoFramesSentWhileClosed: 0,
        materialFramesSentWhileClosed: 0,
        audioBlocksSkippedWhileClosed: 0,
    };
    const baseEncodeDeps = buildEncodeDeps(gate, clock, params.failStage);
    const encodeDeps: EncodeSessionDeps = {
        ...baseEncodeDeps,
        createEncoder(init) {
            if (!mediaGate.open) mediaGate.encodersCreatedWhileClosed++;
            return baseEncodeDeps.createEncoder(init);
        },
    };
    const dropped: DroppedBeforeSocketCounter = { video: 0, audio: 0 };
    // 環境変数の上限と、受け手から届いた上限 (まだ届いていなければ null)。
    const envLimits: EncodeSessionLimits =
        params.maxLongEdge === undefined
            ? { fpsDivisor: params.fpsDivisor }
            : { maxLongEdge: params.maxLongEdge, fpsDivisor: params.fpsDivisor };
    let receivedLimits: LimitsFields | null = null;
    const currentLimits = (): EncodeSessionLimits =>
        receivedLimits === null ? envLimits : tightenLimits(envLimits, limitsFromWire(receivedLimits));
    const mediaChanges: { atMs: number; on: boolean }[] = [];
    const limitsChanges: { atMs: number; limits: LimitsFields }[] = [];
    /** media が変わるたびに増える世代 (開き直しの途中で閉じたら打ち切る)。 */
    let mediaGeneration = 0;
    /** 送信の時間が終わった (後始末の後にストリームを作り直さない)。 */
    let runFinished = false;

    let socketHandle: SocketHandle | null = null;
    const getSocket = (): SocketHandle | null => socketHandle;

    const screens: ScreenEntry[] = [];
    const voices: VoiceEntry[] = [];
    const audioPumps: Promise<void>[] = [];

    // 受け手から届いた録画の現在値。新しく作るエンコーダへも渡すので、
    // 「いま ON か」をこの 1 か所だけが持つ (製品の index.ts の recordingOn と同じ形)。
    let recordingOn = false;
    const recordingChanges: { atMs: number; on: boolean }[] = [];
    const resolutionChanges: HarnessRunResult["resolutionChanges"] = [];
    const elapsedMs = (): number => Math.round(performance.now() - clock.startPerfMs);
    // 送信の時間が終わったことを予定表の待ちへ伝える (走行が終わってから解像度を変えない)。
    let resolveRunEnded!: () => void;
    const runEnded = new Promise<void>((resolve) => {
        resolveRunEnded = resolve;
    });

    // 現在の facts を組んで送る。段 (stages) はストリームごとの現在値を
    // session.stats() から読むので、この関数は「いつ呼んでも現在の値」を送る。繋がる前
    // (socketHandle がない間) と、送るものがないときは何もしない。
    let factsSent: HarnessFacts | null = null;
    const sendFactsNow = (): void => {
        // 送り先がまだない間 (エンコーダの直列確保の途中に onStage から呼ばれる) は何もしない。
        // factsSent は実際に送れたときだけ書き換える (送っていないものを「送った」と
        // 結果 JSON に残さない)。
        if (!socketHandle) return;
        const facts = buildHarnessFacts(
            params.failContact,
            screens.map((e) => ({ streamId: e.streamId, stage: e.session.stats().stage })),
            // いま画面共有している人 = 画面のストリームを持っている人 (製品の core/facts.ts と同じ形。
            // これがないと、枠に入っていない共有者が受け手の参加者一覧に 1 行も出ない)。
            screens.map((e) => e.userId),
            // 受け手から届いた録画と media の現在値は必ず載せる (製品の core/facts.ts と同じ)。
            { recording: recordingOn, media: mediaGate.open },
            // 通話に参加している人は、画面共有の人と声だけの人の全員 (声だけの人も並びに出る)。
            allUserIds(),
        );
        factsSent = facts;
        socketHandle.sendControl({ type: "facts", facts });
    };

    // テスト用の人の全員。画面共有の人と声だけの人を合わせる (声の持ち主を画面共有の人にしたときは同じ人が 2 回入る。
    // profiles と avatar と facts を組み立てる関数が重複を落とす)。
    const allUserIds = (): string[] => [...screens.map((e) => e.userId), ...voices.map((e) => e.userId)];
    // 表示名 (HARNESS_SCREEN_NAMES)。画面共有の人の userId -> 名前。
    const displayNames = new Map<string, string>();
    for (const [i, name] of (params.screenNames ?? []).entries()) {
        if (name !== null) displayNames.set(`harness-screen-${i + 1}`, name);
    }
    // テスト用の人のアバター (userId -> base64 の PNG)。参加者ごとに 1 度だけ作る。
    const avatarPngs = new Map<string, string>();
    // 送ったアバターの PNG の書き出し。フォルダーを渡されたときだけ、送った参加者ごとに 1 度書き出す (参加者ごとの画像は変えないので、
    // 接続し直して送り直しても同じバイト列である)。
    const avatarExport =
        params.avatarExportDir === undefined
            ? null
            : { dir: params.avatarExportDir, written: [] as string[], failed: [] as { userId: string; state: string }[] };
    const exportAvatar = async (userId: string, png: string): Promise<void> => {
        if (avatarExport === null || avatarExport.written.includes(userId)) return;
        let result: AvatarExportState;
        try {
            result = await window.harness.exportAvatar(userId, png);
        } catch (e) {
            result = { state: "error", message: errInfo(e).message };
        }
        if (result.state === "written") avatarExport.written.push(userId);
        else avatarExport.failed.push({ userId, state: result.state });
    };
    // profiles と avatar を、製品と同じ形で全員分送る。製品は接続し直すたびに全員分を送り直すので、確立のたびに呼ぶ。
    const sendProfilesAndAvatarsNow = async (): Promise<void> => {
        const ids = allUserIds();
        for (const id of ids) {
            if (!avatarPngs.has(id)) avatarPngs.set(id, await makeHarnessAvatarPng(id, displayNames.get(id) ?? null));
        }
        const handle = socketHandle;
        if (!handle) return;
        handle.sendControl({ type: "profiles", profiles: buildHarnessProfiles(ids, new Set(), displayNames) });
        const avatars = buildHarnessAvatars(ids, (id) => avatarPngs.get(id) ?? "");
        for (const avatar of avatars) {
            handle.sendControl({ type: "avatar", ...avatar });
        }
        // 書き出しは送り終えてから行う (主プロセスへの問い合わせの間、送るのを待たせない)。
        for (const avatar of avatars) await exportAvatar(avatar.user_id, avatar.avatar_png);
    };

    // 段が変われば「いま実際にエンコードしている寸法」も変わるので、
    // facts と一緒に configure も送り直す (製品の index.ts の videoSink.onStage と同じ形)。
    const onStageChanged = (): void => {
        sendFactsNow();
        sendConfigureNow();
    };
    // media で閉じている間はストリームを 1 つも載せない (製品は畳んだストリームを configure から消す)。
    const sendConfigureNow = (): void => {
        if (!socketHandle) return;
        const streams = mediaGate.open ? buildConfigureStreams(screens, voices) : [];
        socketHandle.sendControl({ type: "configure", streams });
    };

    // stream_id は画面と声で1つの通し番号 (重複させると、受け手
    // (stream_router.h の std::map<uint32_t, Route> による後勝ち上書き) では声の枠へ画面の
    // 映像が全部流れ込む。実測で確認済み)。
    const { screenIds, voiceIds } = assignStreamIds(params.nScreen, params.nVoice);

    // フレームだけを止める仕組み。状態は送信側に 1 つで、全部の映像の材料が共有する。最初の材料を
    // 作る前に 1 回読み (起動の時にファイルがあれば最初のフレームから止める)、その後は FRAME_PAUSE_POLL_MS ごとに
    // 主プロセスへ尋ねる (前の問いが返っていなければそのときは飛ばす)。
    const framesPause =
        params.framesPauseFile === undefined ? null : { file: params.framesPauseFile, ledger: new FramePauseLedger(screenIds) };
    let framePauseTimer: ReturnType<typeof setInterval> | null = null;
    if (framesPause !== null) {
        const pollFramePause = async (): Promise<void> => {
            let file: FramePauseFileState;
            try {
                file = await window.harness.readFramePause();
            } catch {
                file = { state: "error", wallMs: Date.now() };
            }
            framesPause.ledger.apply(file, elapsedMs());
        };
        await pollFramePause();
        let polling = false;
        framePauseTimer = setInterval(() => {
            if (polling) return;
            polling = true;
            void pollFramePause().finally(() => {
                polling = false;
            });
        }, FRAME_PAUSE_POLL_MS);
    }

    // 声に音を乗せるファイル (voice_tone.ts)。中身の読み方と区間の記録は、フレームを止めるファイルと同じ仕組みを声の
    // ストリームの番号で使う (isPaused が真のストリームに音を乗せる)。尋ねる間隔と、最初の 1 回を先に読むのも同じ。
    const voiceTone =
        params.voiceToneFile === undefined ? null : { file: params.voiceToneFile, ledger: new FramePauseLedger(voiceIds) };
    let voiceToneTimer: ReturnType<typeof setInterval> | null = null;
    if (voiceTone !== null) {
        const pollVoiceTone = async (): Promise<void> => {
            let file: FramePauseFileState;
            try {
                file = await window.harness.readVoiceTone();
            } catch {
                file = { state: "error", wallMs: Date.now() };
            }
            voiceTone.ledger.apply(file, elapsedMs());
        };
        await pollVoiceTone();
        let polling = false;
        voiceToneTimer = setInterval(() => {
            if (polling) return;
            polling = true;
            void pollVoiceTone().finally(() => {
                polling = false;
            });
        }, FRAME_PAUSE_POLL_MS);
    }

    // 画面に描く画像 (最初に 1 回だけ読む。作り直した材料にも同じ画像を渡す)。
    const screenImageLoads: ScreenImageLoad[] = [];
    const screenImageBitmaps = await loadScreenImages(params.screenImages ?? [], screenImageLoads);

    // 1 ストリームぶんの材料とエンコーダを作る。最初の 1 回と、走行中に解像度を
    // 変えて作り直すときの両方がここを通る (製品の startScreenVideo と同じ形。
    // start() より前に setRecording(現在値) を呼ぶところまで揃える)。
    const createVideoForScreen = (
        indexInKind: number,
        streamId: number,
        width: number,
        height: number,
        startFrameIndex: number,
        stageHistory: LadderStage[],
        encodedFormat: { value?: EncodedVideoFormat },
    ): { videoSource: HarnessVideoFrameSource; session: EncodeSessionHandle } => {
        const videoSource = new HarnessVideoFrameSource(
            indexInKind,
            width,
            height,
            params.fps,
            params.overflow,
            clock,
            sync,
            startFrameIndex,
            framesPause === null ? null : { ledger: framesPause.ledger, streamId },
            screenImageBitmaps[indexInKind] ?? null,
        );
        const session = createEncodeSession(
            encodeDeps,
            {
                streamId,
                source: videoSource,
                preference: params.hw,
                limits: currentLimits(),
                latencyMode: "realtime",
                bitrateFactor: params.bitrate / BASELINE_BITRATE_BPS,
                warmup: params.warmup,
                keyintSec: params.keyintSec,
            },
            videoSink(getSocket, params.overflow, stageHistory, encodedFormat, dropped, onStageChanged, mediaGate),
        );
        session.setRecording(recordingOn); // 製品の startScreenVideo と同じ位置。
        return { videoSource, session };
    };

    for (const [i, streamId] of screenIds.entries()) {
        const userId = `harness-screen-${streamId}`;
        const hasSyncTone = streamHasSyncTone(params.syncTone, "screen", i);
        const audioSource = new HarnessAudioSource(params.overflow, clock, sync, hasSyncTone);
        const stageHistory: LadderStage[] = [];
        const encodedFormat: { value?: EncodedVideoFormat } = {};
        const audioSent = { count: 0 };
        const audioReanchor = { count: 0 };
        // ストリームごとの最初の寸法 (指定のないストリームは既定の 1920x1080)。
        const initialSize = params.screenSizes?.[i] ?? { width: SCREEN_WIDTH, height: SCREEN_HEIGHT };
        const { videoSource, session } = createVideoForScreen(
            i,
            streamId,
            initialSize.width,
            initialSize.height,
            0,
            stageHistory,
            encodedFormat,
        );

        screens.push({
            streamId,
            userId,
            videoSource,
            audioSource,
            session,
            stageHistory,
            encodedFormat,
            audioSent,
            audioReanchor,
            hasSyncTone,
            encodeBefore: [],
            gated: false,
            size: { width: initialSize.width, height: initialSize.height },
        });
        audioPumps.push(pumpAudio(audioSource, streamId, getSocket, clock, audioSent, audioReanchor, dropped, mediaGate));
    }

    // media で閉じたら画面のストリームをすべて止める (音は pumpAudio が送らない)。
    const closeScreensForMedia = (): void => {
        for (const e of screens) {
            if (e.gated) continue;
            e.encodeBefore.push(e.session.stats());
            e.session.stop();
            e.gated = true;
        }
    };
    // 開き直したら、同じ寸法・続きのフレーム番号から作り直す。
    const reopenScreensForMedia = async (generation: number): Promise<void> => {
        for (const [indexInKind, e] of screens.entries()) {
            if (generation !== mediaGeneration || runFinished) return; // 開き直しの途中でまた閉じた・走行が終わった。
            if (!e.gated) continue;
            const startFrameIndex = Math.ceil((elapsedMs() * params.fps) / 1000);
            const next = createVideoForScreen(
                indexInKind,
                e.streamId,
                e.size.width,
                e.size.height,
                startFrameIndex,
                e.stageHistory,
                e.encodedFormat,
            );
            e.videoSource = next.videoSource;
            e.session = next.session;
            e.gated = false;
            try {
                await e.session.start();
                e.session.requestKeyframe();
            } catch {
                // 作り直しの失敗で送信そのものを落とさない (解像度の変更と同じ扱い)。
            }
        }
    };

    // 予定表の時刻になったら、画面のストリームごとに
    // セッションを止めて新しい寸法で作り直す。止めた側は、止めるときに出す黒埋め用のフレームも出す。
    const runResolutionSchedule = async (): Promise<void> => {
        for (const change of params.resolutionSchedule) {
            // 走行が先に終わったら残りの予定は踏まない (runEnded が先に解決する)。時刻を
            // seconds と比べて先に落とさないのは、この時計が走行の開始ではなく runProduct の
            // 入口から刻んでおり (エンコーダの確保と温めのぶん前にある)、比べる相手にならないため。
            if ((await waitUntil(change.atSec * 1000, clock, runEnded, false)) === "stopped") return;
            for (const [indexInKind, e] of screens.entries()) {
                const at = elapsedMs();
                e.size = { width: change.width, height: change.height };
                if (e.gated) {
                    // media で閉じている間は作り直さず、寸法だけを覚える
                    // (開き直すときにこの寸法で作る)。
                    resolutionChanges.push({
                        atMs: at,
                        streamId: e.streamId,
                        width: change.width,
                        height: change.height,
                        ok: true,
                        deferred: true,
                    });
                    continue;
                }
                try {
                    // 製品の stopScreenEntry -> startScreenVideo と同じ順。stop() は
                    // 「汲み上げを止める指示」で、材料は socket がまだ生きているうちに出ていく。
                    e.encodeBefore.push(e.session.stats());
                    e.session.stop();
                    const startFrameIndex = Math.ceil((at * params.fps) / 1000); // タイムスタンプを巻き戻さない。
                    const next = createVideoForScreen(
                        indexInKind,
                        e.streamId,
                        change.width,
                        change.height,
                        startFrameIndex,
                        e.stageHistory,
                        e.encodedFormat,
                    );
                    e.videoSource = next.videoSource;
                    e.session = next.session;
                    await e.session.start(); // 段が決まると onStage -> configure の送り直しが走る。
                    e.session.requestKeyframe(); // 新しい寸法の最初のアクセス単位を鍵フレームにする。
                    resolutionChanges.push({ atMs: at, streamId: e.streamId, width: change.width, height: change.height, ok: true });
                } catch {
                    // 予定表の失敗で送信そのものを落とさない (測定は残りのストリームと残りの時間で続ける)。
                    resolutionChanges.push({ atMs: at, streamId: e.streamId, width: change.width, height: change.height, ok: false });
                }
            }
        }
    };

    for (const [j, streamId] of voiceIds.entries()) {
        const userId = params.voiceUsers?.[j] ?? `harness-voice-${streamId}`;
        const hasSyncTone = streamHasSyncTone(params.syncTone, "voice", j);
        const toneOn = voiceTone === null ? null : () => voiceTone.ledger.isPaused(streamId);
        const audioSource = new HarnessAudioSource(params.overflow, clock, sync, hasSyncTone, toneOn);
        const audioSent = { count: 0 };
        const audioReanchor = { count: 0 };
        voices.push({ streamId, userId, audioSource, audioSent, audioReanchor, hasSyncTone });
        audioPumps.push(pumpAudio(audioSource, streamId, getSocket, clock, audioSent, audioReanchor, dropped, mediaGate));
    }

    // エンコーダの初期のラダー確保 (成功またはUnavailable到達) が済むまで待つ。SerialGateが
    // 直列にするので、ここでPromise.allしても実際の確保は1ストリームずつ進む。
    await Promise.all(screens.map((e) => e.session.start()));

    let establishedReached = false;
    socketHandle = createSocket(
        {
            makeWebSocket: wrapWebSocket,
            readSession: () => window.harness.readSession(),
            setTimer: (fn, ms) => setTimeout(fn, ms),
            clearTimer: (h) => clearTimeout(h as ReturnType<typeof setTimeout>),
            version: params.helloVersion,
            wireVersion: WIRE_VERSION,
            // hello と pong に載せる送信側の時計 (製品の index.ts と同じく、タイムスタンプと同じ時計を渡す)。
            clockUs: () => Number(clock.nowUs()),
            clockBase: () => (clock.timebase?.base === "qpc" ? "qpc" : undefined),
        },
        {
            onEstablished(): void {
                establishedReached = true;
                // OBS のログでテスト用の送信側だと見分ける 1 行 (確立のたびに送る)。
                socketHandle?.sendControl({ type: "log", lines: [harnessIdentityLogLine(params.helloVersion), clockLine] });
                sendConfigureNow(); // media で閉じていれば空。
                for (const e of screens) e.session.requestKeyframe();
                // 潰した接点と、ストリームごとの現在の段を facts で
                // 送る (組み立ては sender_pure.ts の buildHarnessFacts。vitest から当てるため
                // ここには書かない)。受け手 (src-obs/pipeline) は直近の facts をそのまま
                // 保持するので、変わったときだけ送れば足りる。再接続時も
                // 送り直す (facts/log の再送と同じ規律)。
                sendFactsNow();
                void sendProfilesAndAvatarsNow();
            },
            onClosed(): void {
                // 何もしない (再接続はtransport/client.ts + backoff.tsが自律的に進める)。
            },
            onKeyframeRequest(streamId): void {
                const e = screens.find((s) => s.streamId === streamId);
                e?.session.requestKeyframe();
            },
            onStreamCheck(streamId): void {
                // 受信側が、映像が届かないストリームの確認を頼んだ。通知ファイルの `stuck:N` で止めているストリームは、
                // 製品の送信側が読み出しを始め直したときと同じく止めるのを解いて restarted と答え、`N` で止めている
                // ストリームは Discord が送っていない形として not_receiving と答える (FramePauseLedger.release)。
                const outcome =
                    framesPause === null ? "sending" : framesPause.ledger.release(streamId, elapsedMs(), Date.now());
                socketHandle?.sendControl({ type: "stream_check_result", stream_id: streamId, outcome, detail: "harness" });
            },
            onRecording(on): void {
                // 製品の index.ts の handleRecording と
                // 同じ形。変わっていなければ何もしない (受け手は値が変わったときと繋がった
                // 直後の 2 回送るので、同じ値が 2 回来る)。
                if (on === recordingOn) return;
                recordingOn = on;
                recordingChanges.push({ atMs: elapsedMs(), on });
                for (const e of screens) e.session.setRecording(on);
                sendFactsNow(); // 値が変わったので facts の recording を送り直す (製品と同じ)。
            },
            onMedia(on): void {
                // hello_ok の初期値は onEstablished
                // より先に来る。変わっていなければ何もしない。
                if (on === mediaGate.open || runFinished) return;
                mediaGate.open = on;
                mediaGeneration++;
                mediaChanges.push({ atMs: elapsedMs(), on });
                if (on) void reopenScreensForMedia(mediaGeneration);
                else closeScreensForMedia();
                sendConfigureNow();
                // 値が変わったので facts の media を送り直す (hello_ok の初期値で来たときは
                // まだ確立していないので何も送らず、onEstablished が送る)。
                sendFactsNow();
            },
            onLimits(limits): void {
                // 走っているストリームには下げた鍵だけが動作する。
                // 新しく作るストリームは currentLimits() (環境変数と鍵ごとに厳しいほう) で作る。
                receivedLimits = { ...limits };
                limitsChanges.push({ atMs: elapsedMs(), limits: { ...limits } });
                const next = currentLimits();
                for (const e of screens) {
                    if (!e.gated) e.session.setLimits(next);
                }
            },
            // テスト用の送信側は Discord に接続しないので、通話の操作と視聴する範囲は受け取るだけで何もしない。
            onMainAccount(): void {},
            onReceiveScope(): void {},
            onMoveToMainCall(): void {},
            onLeaveCall(): void {},
            onWatchdogClose(): void {
                // 何もしない (切った回数は socketStats の watchdogCloses に残る)。
            },
        },
    );
    socketHandle.start();

    // 予定表は走行と並んで進む (待ち合わせは runEnded で行う)。
    const schedule = runResolutionSchedule();

    await sleepMs(Math.max(0, params.seconds) * 1000); // 締切は持たない。
    resolveRunEnded();
    runFinished = true; // 以後に届いた media ではストリームを作り直さない。
    // 尋ねるのをやめる (止めたまま終わったストリームの区間は endMs が null のまま残る)。
    if (framePauseTimer !== null) clearInterval(framePauseTimer);
    // 声に音を乗せるファイルも尋ねるのをやめる (乗せたまま終わった区間は endMs が null のまま残る)。
    if (voiceToneTimer !== null) clearInterval(voiceToneTimer);
    await schedule;

    for (const e of screens) {
        e.session.stop(); // videoSourceのclose()もこの中で呼ばれる (frame_source.tsの契約)。
        e.audioSource.close();
    }
    for (const e of voices) e.audioSource.close();
    await Promise.allSettled(audioPumps);
    // stop() の後に hasPendingWork() を読み、
    // 残りの材料があるストリームだけを待ってから socket を閉じる (製品の index.ts と同じ形)。
    const draining = screens.filter((e) => e.session.hasPendingWork());
    let materialDrain: HarnessRunResult["materialDrain"] = null;
    if (draining.length > 0) {
        const startedAt = performance.now();
        let timedOut = true;
        await Promise.race([
            Promise.all(draining.map((e) => e.session.finished())).then(() => {
                timedOut = false;
            }),
            sleepMs(MATERIAL_DRAIN_TIMEOUT_MS),
        ]);
        materialDrain = { waitedStreams: draining.length, waitedMs: Math.round(performance.now() - startedAt), timedOut };
    }
    socketHandle.stop();

    const streams: HarnessStreamResult[] = [];
    for (const e of screens) {
        streams.push({
            kind: "screen",
            streamId: e.streamId,
            stageHistory: e.stageHistory,
            encode: e.session.stats(),
            encodedFormat: e.encodedFormat.value ?? null,
            encodeBefore: [...e.encodeBefore],
            audioChunksSent: e.audioSent.count,
            audioReanchorCount: e.audioReanchor.count,
            hasSyncTone: e.hasSyncTone,
        });
    }
    for (const e of voices) {
        streams.push({
            kind: "voice",
            streamId: e.streamId,
            stageHistory: [],
            encode: null,
            encodedFormat: null,
            encodeBefore: [],
            audioChunksSent: e.audioSent.count,
            audioReanchorCount: e.audioReanchor.count,
            hasSyncTone: e.hasSyncTone,
        });
    }

    return {
        mode: "product",
        config: params,
        establishedReached,
        socketStats: socketHandle.stats(),
        streams,
        syncEvents: sync.events,
        droppedBeforeSocket: { ...dropped },
        factsSent,
        recordingChanges,
        resolutionChanges,
        mediaChanges,
        limitsChanges,
        gate: {
            encodersCreatedWhileClosed: mediaGate.encodersCreatedWhileClosed,
            videoFramesSentWhileClosed: mediaGate.videoFramesSentWhileClosed,
            materialFramesSentWhileClosed: mediaGate.materialFramesSentWhileClosed,
            audioBlocksSkippedWhileClosed: mediaGate.audioBlocksSkippedWhileClosed,
        },
        materialDrain,
        framesPause:
            framesPause === null
                ? null
                : {
                      file: framesPause.file,
                      intervals: framesPause.ledger.intervals.map((i) => ({ ...i })),
                      ignoredTargets: [...framesPause.ledger.ignoredTargets],
                      readErrors: framesPause.ledger.readErrors,
                      checks: framesPause.ledger.checks.map((c) => ({ ...c })),
                  },
        voiceTone:
            voiceTone === null
                ? null
                : {
                      file: voiceTone.file,
                      intervals: voiceToneIntervals(voiceTone.ledger.intervals),
                      ignoredTargets: [...voiceTone.ledger.ignoredTargets],
                      readErrors: voiceTone.ledger.readErrors,
                  },
        avatarExport:
            avatarExport === null
                ? null
                : { dir: avatarExport.dir, written: [...avatarExport.written], failed: [...avatarExport.failed] },
        screenImages: (params.screenImages ?? []).some((f) => f !== null) ? screenImageLoads : null,
        clock: clockLine,
        aborted: false,
        fatalError: null,
    };
}

export async function run(params: HarnessRunParams): Promise<HarnessRunResult> {
    try {
        return await runProduct(params);
    } catch (e) {
        return {
            mode: "product",
            config: params,
            establishedReached: false,
            socketStats: null,
            streams: [],
            syncEvents: [],
            droppedBeforeSocket: null,
            factsSent: null,
            recordingChanges: [],
            resolutionChanges: [],
            mediaChanges: [],
            limitsChanges: [],
            gate: null,
            materialDrain: null,
            framesPause: null,
            voiceTone: null,
            avatarExport: null,
            screenImages: null,
            clock: null,
            aborted: true,
            fatalError: errInfo(e),
        };
    }
}

/* ==========================================================================
   HARNESS_MODE=product のときの自動起動 (index-product.htmlから読まれる)
   ========================================================================== */

function parseHw(value: unknown): HardwareAccelerationPreference {
    return value === "prefer-software" ? "prefer-software" : "prefer-hardware";
}

function paramsFromEnv(env: Record<string, unknown>): HarnessRunParams {
    const fpsDivisorRaw = Number(env.fpsDivisor);
    const maxLongEdgeRaw = Number(env.maxLongEdge);
    const nScreen = Number(env.nScreen) || 1;
    const nVoice = Number(env.nVoice) || 0;
    return {
        nScreen,
        nVoice,
        seconds: Number(env.seconds) || 10,
        fps: Number(env.fps) || 60,
        hw: parseHw(env.hw),
        warmup: env.productWarmupRaw === "0" ? false : true,
        keyintSec: Number(env.keyintSec) || 2,
        syncEverySec: Number(env.syncEverySec) || 5,
        bitrate: Number(env.bitrate) || BASELINE_BITRATE_BPS,
        failStage: Number(env.failStage) || 0,
        overflow: Boolean(env.overflow),
        maxLongEdge: Number.isFinite(maxLongEdgeRaw) && maxLongEdgeRaw > 0 ? maxLongEdgeRaw : undefined,
        fpsDivisor: fpsDivisorRaw === 2 ? 2 : 1,
        syncTone: parseSyncTone(env.syncTone),
        failContact: parseFailContact(env.failContact),
        resolutionSchedule: parseResolutionSchedule(env.resolutionSchedule),
        screenSizes: parseScreenSizes(env.screenSizes, nScreen, { width: SCREEN_WIDTH, height: SCREEN_HEIGHT }),
        screenNames: parseScreenNames(env.screenNames, nScreen),
        screenImages: parseScreenImages(env.screenImages, nScreen),
        voiceUsers: parseVoiceUsers(env.voiceUsers, assignStreamIds(nScreen, nVoice).voiceIds),
        framesPauseFile: parseFramePauseFile(env.framesPauseFile),
        // 場所の読み方はフレームを止めるファイルと同じ (前後の空白を落とし、空なら使わない)。
        voiceToneFile: parseFramePauseFile(env.voiceToneFile),
        avatarExportDir: parseFramePauseFile(env.avatarExportDir),
        helloVersion: parseHelloVersion(env.helloVersion, CORDMUX_BUILD_ID),
        clockBase: env.clockBase === "page" ? "page" : "qpc",
    };
}

async function productMain(): Promise<void> {
    const params = paramsFromEnv(window.harness.env);
    const result = await run(params); // run()自身が例外を捕まえるので、ここでは待つだけでよい。
    try {
        await window.harness.report(result);
    } catch (e) {
        console.error("report failed", e);
    }
}

window.cordmuxSender = { run };

if (window.harness && window.harness.env && window.harness.env.mode === "product") {
    void productMain();
}
