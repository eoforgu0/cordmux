#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0

// マイクの代わりに WAV ファイルを音声の入力にできるかを確認するツール。gen-tone.py で WAV を作り、テスト用の
// 送信側 (test/tools/harness) と同じ Electron をフェイクデバイスを使うオプション付きで起動して、main.js が書いた
// 結果の JSON を標準出力に書く。
//
// 使い方: node run.mjs --out <一時フォルダー>
//
// WAV は毎回 --out の下に作り直す。終了コードは、結果が ok:true なら 0、それ以外 (ok:false、electron を起動できない
// など) は 1。

import { execFileSync, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const HARNESS_DIR = path.resolve(HERE, '..', 'harness');
const GEN_TONE_PY = path.join(HERE, 'gen-tone.py');
const ELECTRON_BIN = path.join(HARNESS_DIR, 'node_modules', 'electron', 'dist', 'electron.exe');

const USAGE = '使い方: node run.mjs --out <一時フォルダー>';

function parseArgs(argv) {
  const args = { out: null };
  const list = argv || [];
  let i = 0;
  while (i < list.length) {
    if (list[i] === '--out') {
      args.out = list[i + 1];
      i += 2;
      continue;
    }
    i += 1;
  }
  return args;
}

function emptyResult(error) {
  return { ok: false, deviceLabel: null, rms: null, toneRatio: null, sampleRate: null, error };
}

function main() {
  const args = parseArgs(process.argv.slice(2));
  if (!args.out) {
    console.error(USAGE);
    process.exitCode = 1;
    return;
  }
  const outDir = path.resolve(args.out);
  fs.mkdirSync(outDir, { recursive: true });
  const wavPath = path.join(outDir, 'mic-check-tone.wav');
  const resultPath = path.join(outDir, 'mic-check-result.json');
  // 前回の結果を削除してから起動する。残っていると、electron が結果を書けずに終了したときに、古い結果を今回の
  // 結果として読んでしまう。
  if (fs.existsSync(resultPath)) fs.unlinkSync(resultPath);

  try {
    execFileSync('python', [GEN_TONE_PY, wavPath], { stdio: 'inherit' });
  } catch (e) {
    console.log(JSON.stringify(emptyResult('gen-tone.py が失敗した: ' + e.message)));
    process.exitCode = 1;
    return;
  }

  if (!fs.existsSync(ELECTRON_BIN)) {
    console.log(
      JSON.stringify(
        emptyResult('harness の electron.exe がない (' + ELECTRON_BIN + '。先に test/tools/harness で npm install を実行する)'),
      ),
    );
    process.exitCode = 1;
    return;
  }

  const env = { ...process.env, MIC_CHECK_WAV: wavPath, MIC_CHECK_OUT: resultPath };
  // main.js が 20 秒のタイムアウトを持つ (タイムアウトでも ok:false の JSON を書いて app.quit() する)。spawnSync には
  // timeout を付けない。2 つ持つと、どちらで止まったのかが結果から分からなくなる。
  const spawned = spawnSync(ELECTRON_BIN, ['.'], { cwd: HERE, env, encoding: 'utf8' });

  let result = null;
  if (fs.existsSync(resultPath)) {
    try {
      result = JSON.parse(fs.readFileSync(resultPath, 'utf8'));
    } catch (e) {
      result = null;
    }
  }
  if (!result) {
    const detail = (spawned.stderr || '').toString().slice(0, 2000) || (spawned.error ? spawned.error.message : '');
    result = emptyResult('結果 JSON が読めなかった (electron の起動失敗またはクラッシュの可能性)。exit=' + spawned.status + ' ' + detail);
  }
  console.log(JSON.stringify(result, null, 2));
  process.exitCode = result.ok ? 0 : 1;
}

main();
