// SPDX-License-Identifier: Apache-2.0

// renderer.js から main.js へ結果を送る関数を、contextBridge で公開する。

'use strict';

const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('micCheck', {
  // 結果を送る。main.js は受け取るとすぐに JSON を書き、app.quit() を呼ぶ。
  report: (obj) => ipcRenderer.invoke('mic-check-result', obj),
});
