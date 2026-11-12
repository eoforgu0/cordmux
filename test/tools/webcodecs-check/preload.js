// SPDX-License-Identifier: Apache-2.0

// renderer.js から main.js へ結果と途中経過を送る関数と、測り方を決める値を、contextBridge で公開する。

'use strict';

const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('webcodecsCheck', {
  // 結果を送る。main.js は受け取るとすぐに JSON を書き、app.quit() を呼ぶ。
  report: (obj) => ipcRenderer.invoke('webcodecs-check-result', obj),
  // 途中経過を送る。タイムアウトしたときに、どこまで測れたかを結果に残すためである。応答は待たない。
  progress: (obj) => ipcRenderer.send('webcodecs-check-progress', obj),
  // renderer.js は process.env を読めないので、測り方を変える値をここで渡す。
  env: {
    // (a) で順に作るエンコーダの数。黒のフレームのエンコードにかかる時間は、後に作るエンコーダにも及ぶので、
    // 最後のエンコーダの遅延を見る。
    serialEncoders: Number(process.env.WEBCODECS_CHECK_ENCODERS) || 4,
    // 黒のフレームの数 (既定は 1 秒ぶんの 60 フレーム)。
    blackFrames: Number(process.env.WEBCODECS_CHECK_BLACK_FRAMES) || 60,
    width: Number(process.env.WEBCODECS_CHECK_WIDTH) || 1280,
    height: Number(process.env.WEBCODECS_CHECK_HEIGHT) || 720,
    fps: Number(process.env.WEBCODECS_CHECK_FPS) || 60,
    bitrate: Number(process.env.WEBCODECS_CHECK_BITRATE) || 12000000,
  },
});
