// SPDX-License-Identifier: Apache-2.0

// マイクの代わりに WAV ファイルを音声の入力にできるかを確認するツールの、Electron のメインプロセス。
// run.mjs が、テスト用の送信側 (test/tools/harness) の Electron を子プロセスとして起動する。
// 環境変数 MIC_CHECK_WAV (読ませる WAV の絶対パス) と MIC_CHECK_OUT (結果の JSON を書くファイルの絶対パス) が必要。
// 測定は renderer.js が行い、preload.js を通して結果を返す。20 秒たっても結果が届かなければ、ok:false の JSON を
// 書いて終了する。

'use strict';

const { app, BrowserWindow, ipcMain, session } = require('electron');
const path = require('path');
const fs = require('fs');

const WAV_PATH = process.env.MIC_CHECK_WAV;
const OUT_PATH = process.env.MIC_CHECK_OUT;
const TIMEOUT_MS = 20000;

let written = false;
let win = null;

function emptyResult(error) {
  return { ok: false, deviceLabel: null, rms: null, toneRatio: null, sampleRate: null, error };
}

// 結果は 1 回だけ書く。結果が届いた後に、タイムアウトと process の exit からも呼ばれるためである。
function writeResult(data) {
  if (written) return;
  written = true;
  try {
    if (OUT_PATH) {
      fs.mkdirSync(path.dirname(OUT_PATH), { recursive: true });
      fs.writeFileSync(OUT_PATH, JSON.stringify(data, null, 2) + '\n', 'utf8');
    }
    console.log('[mic-check] result written:', OUT_PATH, JSON.stringify(data));
  } catch (e) {
    console.error('[mic-check] failed to write result:', e);
  }
}

if (!WAV_PATH || !OUT_PATH) {
  console.error('[mic-check] MIC_CHECK_WAV と MIC_CHECK_OUT の両方が必要');
  process.exitCode = 1;
  app.whenReady().then(() => app.quit());
} else {
  // フェイクデバイスを使う起動時のオプション。PC の設定は変えない。app.whenReady() より前に付ける必要がある。
  app.commandLine.appendSwitch('use-fake-device-for-media-stream');
  app.commandLine.appendSwitch('use-file-for-fake-audio-capture', WAV_PATH);
  // ウィンドウを表示せず、人が操作しないので、自動再生の制限で AudioContext が停止したままにならないようにする。
  app.commandLine.appendSwitch('autoplay-policy', 'no-user-gesture-required');

  app.whenReady().then(() => {
    // 権限の要求のうち、メディアだけを許可する。フェイクデバイスを指定しているので、実際のマイクにはつながらない。
    session.defaultSession.setPermissionRequestHandler((webContents, permission, callback) => {
      callback(permission === 'media');
    });
    session.defaultSession.setPermissionCheckHandler((webContents, permission) => permission === 'media');

    win = new BrowserWindow({
      show: false,
      width: 320,
      height: 240,
      webPreferences: {
        preload: path.join(__dirname, 'preload.js'),
        contextIsolation: true,
      },
    });

    win.webContents.on('console-message', (event, level, message) => {
      console.log('[renderer]', message);
    });
    win.webContents.on('did-fail-load', (event, errorCode, errorDescription) => {
      writeResult(emptyResult('did-fail-load: ' + errorCode + ':' + errorDescription));
      app.quit();
    });
    win.webContents.on('render-process-gone', (event, details) => {
      writeResult(emptyResult('render-process-gone: ' + (details && details.reason)));
      app.quit();
    });

    win.loadFile(path.join(__dirname, 'index.html'));

    setTimeout(() => {
      writeResult(emptyResult('timeout-' + TIMEOUT_MS + 'ms'));
      app.quit();
    }, TIMEOUT_MS);
  });

  ipcMain.handle('mic-check-result', (event, json) => {
    writeResult(json);
    app.quit();
    return true;
  });

  app.on('window-all-closed', () => {
    app.quit();
  });

  // 想定していない経路で終了したときも、最後に 1 回だけ書く。exit のイベントでは同期の処理しか使えない。
  process.on('exit', () => {
    writeResult(emptyResult('process-exit-fallback'));
  });
}
