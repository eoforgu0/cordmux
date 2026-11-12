'use strict';

// テスト用の送信側が送ったアバターの PNG を、環境変数 CORDMUX_HARNESS_AVATAR_DIR のフォルダーに userId ごとに書き出す関数。
// test/tools/composite-check の参照が、OBS が受け取ったのと同じ PNG で名前ラベルを描くために使う (テスト用の送信側は
// Chromium の OffscreenCanvas でアバターを描くので、参照の側では同じ画像を作れない)。レンダラーはファイルに触れられない
// (contextIsolation と sandbox) ので、主プロセスがここで書く。場所が渡されていなければ、フォルダーもファイルも作らない。
// fs を引数で受けるのは vitest から当てるため (avatar_export_file.test.ts)。

const path = require('path');

// userId をそのままファイル名にするので、書ける文字を決める (テスト用の参加者の userId は harness-screen-1 や harness-voice-2)。
// 区切りの文字や .. を含む userId で、フォルダーの外に書かない。
const USER_ID_RE = /^[A-Za-z0-9_-]{1,64}$/;

// 返す形:
//   { state: 'off' }                     場所が渡されていない (何も作らない)
//   { state: 'written', file, bytes }    書いた (bytes は書いたバイト数)
//   { state: 'rejected', reason }        userId がファイル名にできない ('user_id') か、中身が空 ('empty')
//   { state: 'error', message }          フォルダーかファイルを書けなかった
// 中身は送った base64 をデコードしたバイト列そのもの (受信側が decode_base64 で得るのと同じ PNG)。
function exportAvatarPng(dir, userId, base64, fsLike) {
  if (typeof dir !== 'string' || dir.trim().length === 0) return { state: 'off' };
  if (typeof userId !== 'string' || !USER_ID_RE.test(userId)) return { state: 'rejected', reason: 'user_id' };
  if (typeof base64 !== 'string' || base64.length === 0) return { state: 'rejected', reason: 'empty' };
  const bytes = Buffer.from(base64, 'base64');
  if (bytes.length === 0) return { state: 'rejected', reason: 'empty' };
  const target = dir.trim();
  const file = path.join(target, userId + '.png');
  try {
    fsLike.mkdirSync(target, { recursive: true });
    fsLike.writeFileSync(file, bytes);
  } catch (e) {
    return { state: 'error', message: String(e && e.message ? e.message : e) };
  }
  return { state: 'written', file, bytes: bytes.length };
}

module.exports = { exportAvatarPng };
