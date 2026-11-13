'use strict';

// レンダラー (isolated world) からメインへ安全に結果を送るための橋渡し。
const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('harness', {
  // 最終結果。メインは受け取り次第 JSON を書いて app.quit() する。
  report: (obj) => ipcRenderer.invoke('result', obj),
  // 途中経過。応答は待たない (fire-and-forget)。タイムアウト時の partial 書き出しに使う。
  progress: (obj) => ipcRenderer.send('progress', obj),
  // product モード (HARNESS_MODE=product) が接続の試行のたびにトークンを読み直すための関数
  // (transport/socket.ts の read_session 効果に対応)。
  readSession: () => ipcRenderer.invoke('read-session'),
  // product モードがフレームだけを止める通知ファイルの現在の状態を尋ねる関数。
  // 読むのは主プロセス (frame_pause_file.js)。返す形は sender_pure.ts の FramePauseFileState。
  readFramePause: () => ipcRenderer.invoke('read-frame-pause'),
  // 声に音を乗せるファイルの現在の状態を尋ねる (voice_tone.ts)。読むのは主プロセスで、返す形は readFramePause と同じ。
  readVoiceTone: () => ipcRenderer.invoke('read-voice-tone'),
  // 送ったアバターの PNG を書き出す (avatar_export_file.js)。書くのは主プロセスで、場所も主プロセスが環境変数から読む。
  exportAvatar: (userId, base64) => ipcRenderer.invoke('export-avatar', userId, base64),
  // 画面のストリームに描く画像のファイルを読む (screen_image_file.js)。読むのは主プロセスで、HARNESS_SCREEN_IMAGES に書かれた
  // 場所だけを読む。
  readScreenImage: (filePath) => ipcRenderer.invoke('read-screen-image', filePath),
  // clock-probe モードが、メインのプロセスの process.hrtime.bigint() (ナノ秒) を 10 進の文字列で受け取る。往復の時間を
  // 短くするため同期の IPC にする (main.js の hrtime-sync)。
  hrtimeNs: () => ipcRenderer.sendSync('hrtime-sync'),
  // 同じ値を非同期の IPC で受け取る (main.js の hrtime-invoke)。製品の Vencord のプラグインの native の呼び出しと同じ
  // ipcRenderer.invoke で、contextBridge を通す形も製品の VencordNative と同じ。
  hrtimeNsAsync: () => ipcRenderer.invoke('hrtime-invoke'),
  // レンダラーは process.env に触れないため、モード切り替えに要る値だけここで渡す。
  env: {
    mode: process.env.HARNESS_MODE || null,
    sustainSec: Number(process.env.HARNESS_SUSTAIN_SEC) || 60,
    sustainN: Number(process.env.HARNESS_SUSTAIN_N) || 4,
    sustainHw: process.env.HARNESS_SUSTAIN_HW === 'prefer-software' ? 'prefer-software' : 'prefer-hardware',
    wsUrl: process.env.HARNESS_WS_URL || 'ws://127.0.0.1:4470',
    keyintSec: Number(process.env.HARNESS_KEYINT_SEC) || 2,
    syncEverySec: Number(process.env.HARNESS_SYNC_EVERY_SEC) || 5,
    fps: Number(process.env.HARNESS_FPS) || 60,
    // A/V のずれの切り分け用: 1 のときだけ、各エンコーダを configure した
    // 直後に黒 60 フレームをエンコードして捨ててから WebSocket を開く。既定は 0 (しない)。
    warmup: process.env.HARNESS_WARMUP === '1',
    // エンコーダの飽和の下調べ用 (bps)。既定は 12 Mbps。
    bitrate: Number(process.env.HARNESS_BITRATE) || 12_000_000,

    // ---- ここから product モード専用。名前は test/tools/obs-run/run-window.mjs (実行スクリプト) が
    // 渡す環境変数名に揃えてある (既存の sustain/send 用のフィールドは変えない)。
    // 画面共有のストリーム数・声のストリーム数 (既定は run-window.mjs の既定と同じ 1 ストリームと 0 ストリーム)。
    nScreen: Number(process.env.HARNESS_NSCREEN) || 1,
    nVoice: Number(process.env.HARNESS_NVOICE) || 0,
    // 送信の長さ (秒)。sustainSec とは別名 (run-window.mjsの命名に合わせた)。
    seconds: Number(process.env.HARNESS_SECONDS) || 10,
    hw: process.env.HARNESS_HW === 'prefer-software' ? 'prefer-software' : 'prefer-hardware',
    // HARNESS_WARMUP は既存の sustain/send (既定オフ) と既定の向きが逆 (product モードは
    // 温めが既定でオン)。生の文字列のまま渡し、sender.ts の
    // productMain() がモードごとに解釈を分ける (上の warmup フィールドの意味は変えない)。
    productWarmupRaw: process.env.HARNESS_WARMUP === undefined ? null : process.env.HARNESS_WARMUP,
    // 降格の 3 つの手段 (段の失敗を故意に起こす回数・長辺の上限・fps の分母)。
    failStage: Number(process.env.HARNESS_FAIL_STAGE) || 0,
    overflow: process.env.HARNESS_OVERSEND === '1',
    // マーカーの音 (2kHz) をどのストリームに乗せるか ('screen' / 'voice' / 'none'。
    // 未指定と知らない値は sender_pure.ts の parseSyncTone が既定の 'screen' に倒す)。
    syncTone: process.env.HARNESS_SYNC_TONE || null,
    maxLongEdge: Number(process.env.HARNESS_MAX_LONG_EDGE) || 0,
    fpsDivisor: Number(process.env.HARNESS_FPS_DIVISOR) || 1,
    // --dock-scenario contact-missing 向け。接点 id をそのまま渡す (空文字列は sender.ts 側で「未指定」扱い)。
    failContact: process.env.HARNESS_FAIL_CONTACT || null,
    // 走行中の解像度の変更の予定表。
    // 綴りは "<起動からの秒数>:<幅>x<高さ>" の並び ("35:1280x720,60:640x360")。綴りを決めるのは
    // 実行スクリプト (test/tools/obs-run/run-window-plan.mjs の resolutionScheduleArg) で、読むのは
    // sender.ts (sender_pure.ts の parseResolutionSchedule)。
    resolutionSchedule: process.env.HARNESS_RESOLUTION_SCHEDULE || null,
    // 画面のストリームごとの最初の寸法。綴りは "<幅>x<高さ>" の並び
    // ("1280x720,3840x2160")。i 番目が i 番目の画面のストリーム、項目のないストリームは 1920x1080。読むのは
    // sender_pure.ts の parseScreenSizes。
    screenSizes: process.env.HARNESS_SCREEN_SIZES || null,
    // 画面のストリームごとの表示名 ("Mika,Rin")。i 番目が i 番目の画面のストリーム。読むのは sender_pure.ts の parseScreenNames。
    screenNames: process.env.HARNESS_SCREEN_NAMES || null,
    // 画面のストリームごとに描く画像のファイルの場所 (カンマ区切り)。読み方は sender_pure.ts の parseScreenImages で、
    // ファイルの中身は主プロセスが読む (main.js の read-screen-image)。
    screenImages: process.env.HARNESS_SCREEN_IMAGES || null,
    // 声のストリームごとの userId ("harness-screen-2")。読むのは sender_pure.ts の parseVoiceUsers。
    voiceUsers: process.env.HARNESS_VOICE_USERS || null,
    // フレームだけを止める通知ファイルの場所。
    // ファイルがある間は映像のフレームを作らない (音・接続・ハートビートはそのまま)。中身が空なら全部の画面のストリーム、
    // "2" や "1,2" ならその stream id のストリームだけ。ここでは場所の文字列を渡すだけ (結果 JSON の config に残す)。
    // 読むのは主プロセス (main.js の read-frame-pause)。
    framesPauseFile: process.env.HARNESS_FRAMES_PAUSE_FILE || null,
    // 声に音を乗せるファイルの場所 (voice_tone.ts)。ファイルがある間、中に書いた番号の声のストリームに 1 kHz の音を
    // 乗せる。ここでは場所の文字列を渡すだけで、読むのは主プロセス (main.js の read-voice-tone)。
    voiceToneFile: process.env.CORDMUX_HARNESS_VOICE_TONE_FILE || null,
    // 送ったアバターの PNG を書き出すフォルダー (avatar_export_file.js)。ここでは場所の文字列を渡すだけで、書き出すかを
    // 決める (なければ書き出しを依頼しない) のに使う。書くのは主プロセス (main.js の export-avatar)。
    avatarExportDir: process.env.CORDMUX_HARNESS_AVATAR_DIR || null,
    // hello の version に載せる文字列。未指定なら sender.ts が
    // src-vencord/version.ts の CORDMUX_BUILD_ID を使う。CI の zip で実行するときは、実行スクリプトが zip の
    // 同梱の印 (cordmux-build.txt) の build_id を渡すと、バージョンの食い違いの行が出ない。
    helloVersion: process.env.HARNESS_HELLO_VERSION || null,
    // 送信側の時計 (product モード)。'page' なら、ページの原点の TimeTicks を求めず、起動のときのシステム時刻を原点にする
    // (受信側が差を決める古い形)。それ以外は、製品と同じく求めて QueryPerformanceCounter の時計にする。
    clockBase: process.env.HARNESS_CLOCK_BASE || null,
    // clock-probe モードの長さ (秒)。名前は product モードと同じ HARNESS_SECONDS で、既定だけが違う (60 秒)。
    clockProbeSeconds: Number(process.env.HARNESS_SECONDS) || 60,
    // HARNESS_STREAM は運ばない: --stream は OBS 側 (obs-websocket の StartStream/StopStream) と
    // 受け口 (ffmpeg -listen) だけで完結するため、この送信側には渡さない。
  },
});
