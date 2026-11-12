// SPDX-License-Identifier: Apache-2.0
'use strict';

// 画面のストリームに描く画像のファイル (環境変数 HARNESS_SCREEN_IMAGES) を主プロセスで読む関数。レンダラーはファイルに
// 触れられない (contextIsolation と sandbox) ので、レンダラーが場所を渡して尋ね、主プロセスがここで読んでバイト列を返す。
// 読むのは HARNESS_SCREEN_IMAGES に書かれた場所だけで、レンダラーから渡された別の場所は読まない (主プロセスが読む場所は
// 環境変数で決める。frame_pause_file.js と avatar_export_file.js と同じ)。どの画面にどの画像を描くかは sender_pure.ts の
// parseScreenImages が決める。fs を引数で受けるのは vitest から当てるため (screen_image_file.test.ts)。

// 読む大きさの上限 (任意の値。1080p の PNG に十分で、置き違えた大きなファイルで主プロセスを詰まらせない)。
const SCREEN_IMAGE_READ_LIMIT = 32 * 1024 * 1024;

// 返す形:
//   { state: 'off' }                   HARNESS_SCREEN_IMAGES がない
//   { state: 'rejected' }              渡された場所が HARNESS_SCREEN_IMAGES の項目のどれでもない
//   { state: 'absent' }                ファイルがない
//   { state: 'too-large', bytes }      SCREEN_IMAGE_READ_LIMIT を超える
//   { state: 'error', message }        読めなかった
//   { state: 'present', bytes }        読めた (bytes は中身そのもの。形式は見ない。デコードはレンダラーの createImageBitmap)
function readScreenImageFile(listValue, filePath, fsLike) {
  if (typeof listValue !== 'string' || listValue.trim().length === 0) return { state: 'off' };
  const target = typeof filePath === 'string' ? filePath.trim() : '';
  const listed = listValue.split(',').map((item) => item.trim()).filter((item) => item.length > 0);
  if (target.length === 0 || !listed.includes(target)) return { state: 'rejected' };
  let size;
  try {
    size = fsLike.statSync(target).size;
  } catch (e) {
    if (e && (e.code === 'ENOENT' || e.code === 'ENOTDIR')) return { state: 'absent' };
    return { state: 'error', message: String(e && e.message ? e.message : e) };
  }
  if (size > SCREEN_IMAGE_READ_LIMIT) return { state: 'too-large', bytes: size };
  try {
    return { state: 'present', bytes: new Uint8Array(fsLike.readFileSync(target)) };
  } catch (e) {
    return { state: 'error', message: String(e && e.message ? e.message : e) };
  }
}

module.exports = { readScreenImageFile, SCREEN_IMAGE_READ_LIMIT };
