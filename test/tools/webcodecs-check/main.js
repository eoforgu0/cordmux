// SPDX-License-Identifier: Apache-2.0

// WebCodecs のエンコーダを測るツールの、Electron のメインプロセス (測る内容は renderer.js の先頭)。
// run.mjs が、テスト用の送信側 (test/tools/harness) の Electron を子プロセスとして起動する。
// 環境変数 WEBCODECS_CHECK_OUT (結果の JSON を書くファイルの絶対パス) が必要。ほかの WEBCODECS_CHECK_* の既定値は
// preload.js にある。測定は renderer.js が行い、preload.js を通して結果を返す。タイムアウトまでに結果が届かなければ、
// そこまでの途中経過を付けた JSON を書いて終了する。

'use strict';

const { app, BrowserWindow, ipcMain } = require('electron');
const path = require('path');
const fs = require('fs');

const OUT_PATH = process.env.WEBCODECS_CHECK_OUT;
// (a) の 4 つのエンコーダ (それぞれ黒の 60 フレームと実際のフレーム 1 つ) と、(b) の 5 つのエンコーダを作る時間が
// 入る長さ。エンコーダを作るだけで数秒かかることがあるので、mic-check の 20 秒では足りない。
const TIMEOUT_MS = Number(process.env.WEBCODECS_CHECK_TIMEOUT_MS) || 120000;

let written = false;
let win = null;
let latestProgress = null;

function emptyResult(error) {
  return { ok: false, a: null, b: null, error: error };
}

function buildEnvInfo() {
  return {
    electron: process.versions.electron,
    chrome: process.versions.chrome,
    node: process.versions.node,
    platform: process.platform,
    arch: process.arch,
  };
}

// 結果は 1 回だけ書く。結果が届いた後に、タイムアウトと process の exit からも呼ばれるためである。
function writeResult(data) {
  if (written) return;
  written = true;
  try {
    if (OUT_PATH) {
      fs.mkdirSync(path.dirname(OUT_PATH), { recursive: true });
      const payload = Object.assign({ writtenAt: new Date().toISOString(), env: buildEnvInfo(), progress: latestProgress }, data);
      fs.writeFileSync(OUT_PATH, JSON.stringify(payload, null, 2) + '\n', 'utf8');
    }
    console.log('[webcodecs-check] result written:', OUT_PATH);
  } catch (e) {
    console.error('[webcodecs-check] failed to write result:', e);
  }
}

if (!OUT_PATH) {
  console.error('[webcodecs-check] WEBCODECS_CHECK_OUT が必要');
  process.exitCode = 1;
  app.whenReady().then(() => app.quit());
} else {
  app.whenReady().then(() => {
    win = new BrowserWindow({
      show: false,
      width: 320,
      height: 240,
      webPreferences: {
        preload: path.join(__dirname, 'preload.js'),
        contextIsolation: true,
        // 表示しないウィンドウでも、エンコードのタイマーが間引かれないようにする。
        backgroundThrottling: false,
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

  ipcMain.on('webcodecs-check-progress', (event, json) => {
    latestProgress = json;
  });

  ipcMain.handle('webcodecs-check-result', (event, json) => {
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
