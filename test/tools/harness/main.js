'use strict';

// NVENC (H.264 1080p60) を Vesktop 1.6.7 と同じ Electron/Chromium で測るハーネス。
// レンダラーから届いた結果 (または途中経過) を JSON として _work/out に書き出す。
// 検証用の起動では環境変数 HARNESS_TIMEOUT_MS で内部タイムアウトを短縮できる
// (既定 120000ms は本番用。20 秒以内に終わらせたい検証時は短い値を渡す)。

const { app, BrowserWindow, ipcMain, session } = require('electron');
const path = require('path');
const fs = require('fs');
const { readFramePauseState } = require('./frame_pause_file');
const { exportAvatarPng } = require('./avatar_export_file');
const { readScreenImageFile } = require('./screen_image_file');

// 'tests' (既定, テストA-E) / 'sustain' / 'send' の3モードの出力先 (既存のまま。変えない)。
// 結果の出力先。リポジトリのルートの _work/out。__dirname は test/tools/harness。
const OUT_DIR = path.join(__dirname, '..', '..', '..', '_work', 'out');
const PRODUCT_OUT_DIR = OUT_DIR;
const MODE = process.env.HARNESS_MODE || 'tests'; // 'tests' (既定, テストA-E) / 'sustain' / 'send' / 'product'
const SUSTAIN_SEC = Number(process.env.HARNESS_SUSTAIN_SEC) || 60;
const PRODUCT_SECONDS = Number(process.env.HARNESS_SECONDS) || 10; // product モードの長さ (秒)。
// 持続負荷/送信/product モードは指定秒数だけ回るため、既定タイムアウトは (その時間 + 30秒) にする。
// テスト A-E モードは既定どおり 120 秒。どれも HARNESS_TIMEOUT_MS で上書きできる
// (送信モードの WebSocket 接続タイムアウト 5 秒はレンダラー側の別の話で、これは外側の保険)。
const DEFAULT_TIMEOUT_MS =
  MODE === 'sustain' || MODE === 'send'
    ? (SUSTAIN_SEC + 30) * 1000
    : MODE === 'product'
      ? (PRODUCT_SECONDS + 30) * 1000
      : 120000;
const TIMEOUT_MS = Number(process.env.HARNESS_TIMEOUT_MS) || DEFAULT_TIMEOUT_MS;

let written = false;
let latestProgress = null;
let win = null;
let timeoutHandle = null;

function nowForFileName() {
  return new Date().toISOString().replace(/[:.]/g, '-');
}

function buildEnvInfo() {
  return {
    nvidiaSmiRaw: process.env.HARNESS_NVIDIA_SMI || null,
    electron: process.versions.electron,
    chrome: process.versions.chrome,
    node: process.versions.node,
    platform: process.platform,
    arch: process.arch,
  };
}

// 一度書いたら二重に書かない (result 到着後にタイムアウトが後追いで発火する場合などを想定)。
function writeResult(data, reason) {
  if (written) return;
  written = true;
  try {
    const outDir = MODE === 'product' ? PRODUCT_OUT_DIR : OUT_DIR;
    if (!fs.existsSync(outDir)) fs.mkdirSync(outDir, { recursive: true });
    const prefix =
      MODE === 'sustain' ? 'harness-sustain-' : MODE === 'send' ? 'harness-send-' : MODE === 'product' ? 'product-send-' : 'harness-';
    const file = path.join(outDir, `${prefix}${nowForFileName()}.json`);
    const payload = {
      writtenAt: new Date().toISOString(),
      writeReason: reason,
      env: buildEnvInfo(),
      data: data || null,
    };
    // BigInt は JSON にできず、そのままだと結果が 1 つも書けない (encode/session.ts の stats() の
    // clockOriginUs が bigint)。桁を落とさないよう文字列にして書く。
    const replacer = (_key, value) => (typeof value === 'bigint' ? value.toString() : value);
    fs.writeFileSync(file, JSON.stringify(payload, replacer, 2) + '\n', 'utf8');
    console.log('[harness] result written:', file);
  } catch (e) {
    console.error('[harness] failed to write result:', e);
  }
}

app.whenReady().then(() => {
  win = new BrowserWindow({
    show: false,
    width: 640,
    height: 360,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      // 隠しウィンドウでも 60fps のタイマが間引かれないようにする (持続負荷モード用)。
      backgroundThrottling: false,
    },
  });

  // レンダラーの console.log をメインのログにも出す (診断用。起動回数が限られるため)。
  win.webContents.on('console-message', (event, level, message, line, sourceId) => {
    console.log('[renderer]', message);
  });
  win.webContents.on('did-fail-load', (event, errorCode, errorDescription) => {
    console.error('[harness] did-fail-load:', errorCode, errorDescription);
    writeResult(latestProgress, 'did-fail-load:' + errorCode + ':' + errorDescription);
    app.quit();
  });
  win.webContents.on('render-process-gone', (event, details) => {
    console.error('[harness] render-process-gone:', details && details.reason);
    writeResult(latestProgress, 'render-process-gone:' + (details && details.reason));
    app.quit();
  });

  if (MODE === 'product') {
    // Vesktop (Discord の Electron クライアント) が discord.com を読み込んで送る Origin を
    // 模す (cordmux.dll 側の Origin 許可一覧 = discord.com/canary.discord.com/ptb.discord.com)。他の3モードは fake-sink.mjs 等 Origin を見ない
    // 相手にしか繋がないため、この書き換えは product モードのときだけ登録する。
    session.defaultSession.webRequest.onBeforeSendHeaders(
      { urls: ['ws://127.0.0.1/*'], types: ['webSocket'] },
      (details, callback) => {
        details.requestHeaders['Origin'] = 'https://discord.com';
        callback({ requestHeaders: details.requestHeaders });
      },
    );
  }

  win.loadFile(path.join(__dirname, MODE === 'product' ? 'index-product.html' : 'index.html'));

  timeoutHandle = setTimeout(() => {
    writeResult(latestProgress, 'timeout-' + TIMEOUT_MS + 'ms');
    app.quit();
  }, TIMEOUT_MS);
});

ipcMain.on('progress', (event, json) => {
  latestProgress = json;
});

// native.ts (Vencord側) の相当品。transport/socket.ts の read_session 効果に対応し、
// 接続の試行のたびにトークンを読み直す (トークンは接続のたびに読み直す)。
// CORDMUX_HANDSHAKE_DIR はテスト用の送信側専用の環境変数 (test/tools/obs-run/run-window.mjs の
// localAppDataEnv() が渡す)。これがないときは実機の %LOCALAPPDATA%\cordmux には
// 絶対に触らない (null を返すだけ)。読めない・壊れている・形が不正、のいずれも理由を区別せず
// null に丸め、トークンをログや例外メッセージに出さない (native.ts の readSession と同じ規約)。
ipcMain.handle('read-session', async () => {
  const dir = process.env.CORDMUX_HANDSHAKE_DIR;
  if (!dir) return null;

  let text;
  try {
    text = fs.readFileSync(path.join(dir, 'session.json'), 'utf8');
  } catch (e) {
    return null; // ファイルがない (OBSがまだ起きていない等)・権限で読めない。
  }

  let parsed;
  try {
    parsed = JSON.parse(text);
  } catch (e) {
    return null; // 壊れたJSON (書き込みの途中に読んだ等)。
  }
  if (typeof parsed !== 'object' || parsed === null) return null;

  const port = parsed.port;
  const secret = parsed.secret;
  const valid = Number.isInteger(port) && port > 0 && typeof secret === 'string' && secret.length > 0;
  if (!valid) return null;
  return { port, secret };
});

// フレームだけを止める通知ファイル。レンダラーが
// 決まった間隔で尋ね、ここは現在の状態を読んで返すだけ (ファイルを作らない・消さない)。場所は環境変数
// HARNESS_FRAMES_PAUSE_FILE (未指定なら state: 'off' で、送信側は既定どおり)。
ipcMain.handle('read-frame-pause', () => readFramePauseState(process.env.HARNESS_FRAMES_PAUSE_FILE, fs, Date.now));

// 声に音を乗せるファイル (voice_tone.ts)。読み方はフレームを止めるファイルと同じで、現在の状態を読んで返すだけ
// (ファイルを作らない・消さない)。場所は環境変数 CORDMUX_HARNESS_VOICE_TONE_FILE で、なければ state: 'off'。
ipcMain.handle('read-voice-tone', () => readFramePauseState(process.env.CORDMUX_HARNESS_VOICE_TONE_FILE, fs, Date.now));

// 送ったアバターの PNG を書き出す (avatar_export_file.js)。場所は環境変数 CORDMUX_HARNESS_AVATAR_DIR で、レンダラーからは
// 受け取らない。なければ state: 'off' で、何も作らない。
ipcMain.handle('export-avatar', (event, userId, base64) =>
  exportAvatarPng(process.env.CORDMUX_HARNESS_AVATAR_DIR, userId, base64, fs),
);

// 画面のストリームに描く画像のファイル (screen_image_file.js)。読むのは環境変数 HARNESS_SCREEN_IMAGES に書かれた場所だけで、
// レンダラーが渡した場所がその中になければ読まない。なければ state: 'off' で、送信側は動く図形を描く。
ipcMain.handle('read-screen-image', (event, filePath) => readScreenImageFile(process.env.HARNESS_SCREEN_IMAGES, filePath, fs));

ipcMain.handle('result', (event, json) => {
  if (timeoutHandle) clearTimeout(timeoutHandle);
  writeResult(json, 'reported');
  app.quit();
  return true;
});

app.on('window-all-closed', () => {
  app.quit();
});

// 想定外の終了経路でも最後に一度だけ書く (同期処理のみ使用可能)。
process.on('exit', () => {
  writeResult(latestProgress, 'process-exit-fallback');
});
