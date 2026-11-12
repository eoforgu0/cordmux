'use strict';

// フレームだけを止める通知ファイルを主プロセスで読む関数。
// レンダラーはファイルに触れられない (contextIsolation と sandbox) ので、レンダラーが ipc で尋ねるたびに
// 主プロセスがここで現在の状態を読んで返す。**読むだけで、作らない・消さない・書かない** (通知ファイルを
// 置くのも消すのも実行スクリプトか人)。fs と時計を引数で受けるのは vitest から当てるため (frame_pause_file.test.ts)。

// 中身として読む長さの上限 (任意の値。止めるストリームの並びには十分。大きなファイルを置き違えても読み込みで詰まらない)。
const FRAME_PAUSE_READ_LIMIT = 4096;

// 返す形は sender_pure.ts の FramePauseFileState と同じ:
//   { state: 'off' }                 場所が渡されていない
//   { state: 'absent' }              ファイルがない (どのストリームも止めない)
//   { state: 'present', text }       ファイルがある (text は中身の先頭。空なら全部の画面のストリーム)
//   { state: 'error' }               あるらしいが読めなかった (レンダラーは前の状態を保つ)
// どれにも wallMs (読んだ時刻) を付ける。
function readFramePauseState(filePath, fsLike, now) {
  const wallMs = now();
  if (typeof filePath !== 'string' || filePath.trim().length === 0) return { state: 'off', wallMs };
  const target = filePath.trim();
  let buffer;
  try {
    buffer = fsLike.readFileSync(target);
  } catch (e) {
    if (e && (e.code === 'ENOENT' || e.code === 'ENOTDIR')) return { state: 'absent', wallMs };
    return { state: 'error', wallMs };
  }
  const text = Buffer.from(buffer).subarray(0, FRAME_PAUSE_READ_LIMIT).toString('utf8');
  return { state: 'present', text, wallMs };
}

module.exports = { readFramePauseState, FRAME_PAUSE_READ_LIMIT };
