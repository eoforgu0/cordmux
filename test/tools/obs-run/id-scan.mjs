#!/usr/bin/env node
// id-scan.mjs: コピーした物に Discord のユーザー ID の形 (17-20 桁の数) が残っていないかを数える。
//
// 実機の実行の結果 JSON・ログ・PNG を build_x64 などへコピーするとき、配信者の ID の入った物 (結果 JSON の args の前の形、
// テスト用の OBS の cordmux の settings.json、Vencord の設定のコピー) は
// コピーしない。コピーした後に 17-20 桁の数を数えて 0 を確認するためのツールである。grep で数を探すと、見つけた数そのものが
// 端末とログに出る (= ID をどこかに書くことになる) ので、このツールは**数を出さず**、ファイルと行の番号と数の個数だけを出す。
// grep -c を使わないのは、行の番号が出ず、UTF-16 のログを読めないためである。Python のツールにセルフテストを付けないのは、
// CI の lint.yml が全部のセルフテストを実行することを ci-selftests.test.ts が確認するので、lint.yml を変えずに足せないためである。
// このツールのテストは vitest の id-scan.test.ts にある。
//
// 使い方: node test/tools/obs-run/id-scan.mjs <ファイルかフォルダ>...
// 出力: 1 行の JSON { ok, scannedFiles, hits: [{ file, count, lines }], idBearingNames: [ファイル], skippedBinary: [ファイル] }
//   - 読むのはテキストのファイル (UTF-8、BOM つきの UTF-8 / UTF-16)。NUL を含むテキストでないファイル (PNG など) は数えずに
//     skippedBinary に並べる。
//   - 名前が ID の入りうる物 (run-window-plan.mjs の ID_BEARING_FILE_PATTERNS) のファイルは、中を読めなくても idBearingNames に
//     挙げて失敗にする (PNG の中の文字は数えられないため)。
//   - 17-20 桁の数は ID でない物 (19 桁のナノ秒のタイムスタンプなど) も拾う。拾った物は許す一覧を持たずに報告する (読むのは人)。
// 終了コード: 0 = 0 件、1 = 見つかった (数か名前)、2 = 引数の誤り・読めない場所。
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { ID_BEARING_FILE_PATTERNS, countDiscordIdLike } from "./run-window-plan.mjs";

// バイト列を文字列にデコードする。BOM で UTF-16 (LE / BE) と UTF-8 を分け、BOM が無ければ UTF-8。NUL を含む (BOM のない) 物は文字でない
// とみなして null を返す。
export function decodeText(bytes) {
  const b = Buffer.from(bytes);
  if (b.length >= 2 && b[0] === 0xff && b[1] === 0xfe) return b.subarray(2).toString("utf16le");
  if (b.length >= 2 && b[0] === 0xfe && b[1] === 0xff) {
    const swapped = Buffer.from(b.subarray(2));
    swapped.swap16();
    return swapped.toString("utf16le");
  }
  const body = b.length >= 3 && b[0] === 0xef && b[1] === 0xbb && b[2] === 0xbf ? b.subarray(3) : b;
  if (body.includes(0)) return null;
  return body.toString("utf8");
}

// 文字列の中の 17-20 桁の数の個数と、それがある行の番号 (1 から)。数そのものは返さない。
export function scanTextForIds(text) {
  const lines = [];
  let count = 0;
  String(text == null ? "" : text)
    .split(/\r?\n/)
    .forEach((line, i) => {
      const n = countDiscordIdLike(line);
      if (n > 0) {
        count += n;
        lines.push(i + 1);
      }
    });
  return { count, lines };
}

// ファイルの名前が ID の入りうる物か (ID_BEARING_FILE_PATTERNS のパターンを含む)。
export function isIdBearingName(name) {
  const base = String(name || "").replace(/\\/g, "/");
  return ID_BEARING_FILE_PATTERNS.some((p) => base.includes(p));
}

// 読んだファイルの並び ([{ file, bytes }]) を数える (純関数。テストが確認する)。
export function scanEntries(entries) {
  const hits = [];
  const idBearingNames = [];
  const skippedBinary = [];
  for (const e of entries || []) {
    if (isIdBearingName(e.file)) idBearingNames.push(e.file);
    const text = decodeText(e.bytes);
    if (text === null) {
      skippedBinary.push(e.file);
      continue;
    }
    const found = scanTextForIds(text);
    if (found.count > 0) hits.push({ file: e.file, count: found.count, lines: found.lines.slice(0, 50) });
  }
  return { ok: hits.length === 0 && idBearingNames.length === 0, scannedFiles: (entries || []).length, hits, idBearingNames, skippedBinary };
}

function listFiles(target) {
  const st = fs.statSync(target);
  if (st.isFile()) return [target];
  const out = [];
  for (const name of fs.readdirSync(target).sort()) {
    const full = path.join(target, name);
    const s = fs.statSync(full);
    if (s.isDirectory()) out.push(...listFiles(full));
    else if (s.isFile()) out.push(full);
  }
  return out;
}

function main(argv) {
  const targets = argv.filter((a) => !a.startsWith("--"));
  if (targets.length === 0) {
    console.error("使い方: node id-scan.mjs <ファイルかフォルダ>...");
    return 2;
  }
  const entries = [];
  for (const t of targets) {
    if (!fs.existsSync(t)) {
      console.error("id-scan: ない場所: " + t);
      return 2;
    }
    for (const file of listFiles(t)) entries.push({ file, bytes: fs.readFileSync(file) });
  }
  const report = scanEntries(entries);
  console.log(JSON.stringify(report));
  return report.ok ? 0 : 1;
}

// ツールとして起動されたときだけ main を実行する (テストが読み込むときは実行しない)。Windows はドライブ文字と大文字小文字の違う書き方でも
// 同じ場所なので、小文字にして比べる (比べ損ねると何も出さずに終了 0 = 「0 件」と取り違える)。
const invokedAs = process.argv[1] ? path.resolve(process.argv[1]) : "";
const selfPath = fileURLToPath(import.meta.url);
if (invokedAs && (process.platform === "win32" ? invokedAs.toLowerCase() === selfPath.toLowerCase() : invokedAs === selfPath)) {
  process.exitCode = main(process.argv.slice(2));
}
