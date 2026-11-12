#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0

// WebCodecs のエンコーダを測るツール (測る内容は renderer.js の先頭)。テスト用の送信側 (test/tools/harness) と
// 同じ Electron を起動し、main.js が書いた結果の JSON を標準出力に書く。
//
// 使い方: node run.mjs --out <一時フォルダー> [--dry-run]
//
// --dry-run では Electron を起動せず、起動するコマンドと、結果の JSON の形だけを書く。終了コードは、結果が
// ok:true なら 0、それ以外 (ok:false、electron を起動できないなど) は 1。

import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const HARNESS_DIR = path.resolve(HERE, '..', 'harness');
const ELECTRON_BIN = path.join(HARNESS_DIR, 'node_modules', 'electron', 'dist', 'electron.exe');

const USAGE = '使い方: node run.mjs --out <一時フォルダー> [--dry-run]';

// 結果の JSON の形。--dry-run のときに書く。実際の値は renderer.js が入れる。
const RESULT_SHAPE = {
  ok: 'true なら測れた (値がよいかどうかではない。値の良し悪しは人が判断する)',
  a: {
    encoders: '順に作ったエンコーダの数',
    last: {
      configureToFirstOutputMs: 'configure から最初の出力まで (ms)',
      blackEncodeMs: 'configure から黒の最後の出力まで (ms)',
      blackToFirstRealMs: '黒の最後の出力から、最初の実際のフレームの出力まで (ms)',
      firstChunkParams: '最初のチャンクから取り出した SPS と PPS (16 進)',
    },
    all: 'エンコーダごとの同じ 3 つの値 (最後のエンコーダの値が last)',
  },
  b: {
    pairs: 'hardware-hardware / hardware-software / software-software / same-instance-second-keyframe の 4 つ。それぞれ spsEqual / ppsEqual / bothEqual',
  },
  error: '測れなかったときの理由 (測れたときは null)',
};

function parseArgs(argv) {
  const args = { out: null, dryRun: false };
  const list = argv || [];
  let i = 0;
  while (i < list.length) {
    if (list[i] === '--out') {
      args.out = list[i + 1];
      i += 2;
      continue;
    }
    if (list[i] === '--dry-run') {
      args.dryRun = true;
      i += 1;
      continue;
    }
    i += 1;
  }
  return args;
}

function emptyResult(error) {
  return { ok: false, a: null, b: null, error };
}

function main() {
  const args = parseArgs(process.argv.slice(2));
  if (!args.out) {
    console.error(USAGE);
    process.exitCode = 1;
    return;
  }
  const outDir = path.resolve(args.out);
  const resultPath = path.join(outDir, 'webcodecs-check-result.json');

  if (args.dryRun) {
    console.log(
      JSON.stringify(
        {
          dryRun: true,
          electron: ELECTRON_BIN,
          electronExists: fs.existsSync(ELECTRON_BIN),
          argv: [ELECTRON_BIN, '.'],
          cwd: HERE,
          env: { WEBCODECS_CHECK_OUT: resultPath },
          resultPath,
          resultShape: RESULT_SHAPE,
        },
        null,
        2,
      ),
    );
    return;
  }

  fs.mkdirSync(outDir, { recursive: true });
  // 前回の結果を削除してから起動する。残っていると、electron が結果を書けずに終了したときに、古い結果を今回の
  // 結果として読んでしまう。
  if (fs.existsSync(resultPath)) fs.unlinkSync(resultPath);

  if (!fs.existsSync(ELECTRON_BIN)) {
    console.log(JSON.stringify(emptyResult('harness の electron.exe がない (' + ELECTRON_BIN + '。先に test/tools/harness で npm install を実行する)')));
    process.exitCode = 1;
    return;
  }

  const env = { ...process.env, WEBCODECS_CHECK_OUT: resultPath };
  // main.js がタイムアウトを持つ (タイムアウトでも ok:false の JSON を書いて app.quit() する)。spawnSync には
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
