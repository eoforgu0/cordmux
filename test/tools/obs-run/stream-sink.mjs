// 配信の受け口 (ffmpeg -listen 1 で rtmp を受けて flv にコピーする) を起動し、止め、送信側が切るたびに開き直す。
//
// run-window.mjs の stream-sink-start / stream-sink-stop と finally、hold と call-hold が使う。run-window.mjs は OBS を起動しないと
// 動かないので、開き直しの動きをテストで確認できるように、起動の処理をこのファイルに分けている。
//   - ffmpeg -listen 1 は送信側 (OBS) が切ると終了コード 0 で終わる (同梱の ffmpeg n7.1.1 で確認した)。rearm を立てると、
//     正常に終わるたびに次のファイル (stream-<tag>-<n>.flv) で開き直す (配信を何度か始めて止めるため)。
//   - 開き直すのは、終了コード 0 か、flv に 1 バイトでも書いて終わったとき (受け口の仕事をした)。確認したのは ffmpeg から ffmpeg へ
//     送った場合だけで、OBS の StopStream の切り方で ffmpeg の終了コードが 0 になるかは実機の実行で初めて分かるので、
//     どちらでも開き直す。0 以外で何も書かずに終わったら開き直さない (起動できない・受ける前に異常終了した場合に繰り返さない)。
//     上限 maxRearms まで開き直し、stop の後は開き直さない。
//   - 標準エラーは末尾だけ持つ (読まずに pipe を詰まらせない)。標準出力は捨てる。ウィンドウは出さない (windowsHide)。
// 1 回きりの受け口にしないのは、最初の StopStream の後の 2 回目の StartStream に受け手がなく、テスト用の OBS が接続の失敗の
// ウィンドウを出すためである。
// Node の API は spawn と fs だけで、spawn は差し替えられる (テストは偽の子プロセスで開き直しの動きを確認する)。
import { spawn as nodeSpawn } from "node:child_process";
import fs from "node:fs";
import path from "node:path";

// 受け口の最初のファイルは stream-<tag>.flv、開き直した 2 つ目からは stream-<tag>-<n>.flv。
export function streamSinkFileName(tag, index) {
  return index <= 1 ? "stream-" + tag + ".flv" : "stream-" + tag + "-" + index + ".flv";
}

// 終わった受け口を開き直すか (bytes は終わったときの flv の大きさ。無ければ 0 か null)。
export function shouldRearm({ rearm, code, rearms, maxRearms, bytes }) {
  return Boolean(rearm) && rearms < maxRearms && (code === 0 || (typeof bytes === "number" && bytes > 0));
}

// ffmpeg の引数 (上書き・警告だけ・1 回だけ受ける・コピーするだけ)。
export function streamSinkArgs(url, flvPath) {
  return ["-y", "-hide_banner", "-loglevel", "warning", "-listen", "1", "-i", url, "-c", "copy", flvPath];
}

// 親がいない受け口の確認 (stream-sink-stale.ps1) の出力をログの形にまとめる。
// 判定 (同梱の ffmpeg の場所・-listen と URL・親の pid に誰も居ない) は stream-sink-stale.ps1 の 1 か所で、cleanup.ps1 も同じ物を呼ぶ。
// 実行スクリプトは -Stop を渡さない (見つけるだけ。実行スクリプトの受け口は node と一緒に終わり、止める相手がないため。
// stream-sink-stale.ps1 の先頭のコメント)。stale は見つけた pid、stopped は outcome が stopped の物だけ (止める前に居なくなっていた
// already-gone は止めていないので入れない)。undecided は親の読みが失敗して決められなかった受け口 (終了コード 3)。
// ここは 1 行の JSON を読むだけ: 読めない出力は ok:false (stream-sink-start は続く 1935 番の先客の確認で断るかを決める)。
export function summarizeStaleCheck(stdout, exitCode) {
  const asArray = (v) => (Array.isArray(v) ? v : v == null ? [] : [v]);
  const line = String(stdout || "")
    .split(/\r?\n/)
    .map((s) => s.trim())
    .filter((s) => s.startsWith("{"))
    .pop();
  const base = { ok: false, exitCode: exitCode ?? null, stale: [], stopped: [], alreadyGone: [], owned: [], undecided: [], remaining: [], refused: false, error: null };
  if (!line) return { ...base, error: "no result from stream-sink-stale.ps1" };
  let j;
  try {
    j = JSON.parse(line);
  } catch (e) {
    return { ...base, error: "unreadable result from stream-sink-stale.ps1: " + e.message };
  }
  const stale = asArray(j.stale).filter((s) => s);
  return {
    ok: j.ok === true && exitCode === 0,
    exitCode: exitCode ?? null,
    stale: stale.map((s) => s.pid),
    stopped: stale.filter((s) => s.outcome === "stopped").map((s) => s.pid),
    alreadyGone: stale.filter((s) => s.outcome === "already-gone").map((s) => s.pid),
    owned: asArray(j.owned).map((o) => o && o.pid),
    undecided: asArray(j.undecided).map((u) => u && u.pid),
    remaining: asArray(j.remaining),
    refused: j.refused === true,
    error: j.error || null,
  };
}

export function createStreamSink({ exe, url, dir, tag, rearm, maxRearms, spawn = nodeSpawn, rearmDelayMs = 500, onEvent = null }) {
  const info = { url, exe, rearm: Boolean(rearm), rearms: 0, files: [] };
  let current = null;
  let currentEntry = null;
  let stopped = false;

  function start() {
    const index = info.files.length + 1;
    const name = streamSinkFileName(tag, index);
    const flvPath = path.join(dir, name);
    const child = spawn(exe, streamSinkArgs(url, flvPath), { stdio: ["ignore", "ignore", "pipe"], windowsHide: true });
    const entry = { file: name, path: flvPath, pid: child.pid ?? null, startedAt: new Date().toISOString(), exitCode: null, signal: null, bytes: null, stderrTail: "" };
    info.files.push(entry);
    current = child;
    currentEntry = entry;
    if (child.stderr) child.stderr.on("data", (d) => (entry.stderrTail = (entry.stderrTail + d.toString()).slice(-800)));
    child.on("error", (e) => (entry.error = e.message));
    child.on("exit", (code, signal) => {
      entry.exitCode = code;
      entry.signal = signal || null;
      entry.endedAt = new Date().toISOString();
      try {
        entry.bytes = fs.existsSync(flvPath) ? fs.statSync(flvPath).size : 0;
      } catch {
        entry.bytes = null;
      }
      if (current === child) {
        current = null;
        currentEntry = null;
      }
      if (onEvent) onEvent({ type: "exit", entry });
      if (!stopped && shouldRearm({ rearm: info.rearm, code, rearms: info.rearms, maxRearms, bytes: entry.bytes })) {
        info.rearms += 1;
        setTimeout(() => {
          if (!stopped && info.rearm) {
            const next = start();
            if (onEvent) onEvent({ type: "rearm", entry: next });
          }
        }, rearmDelayMs);
      }
    });
    return entry;
  }

  // 開き直しを止めてから、動いている受け口を止める (Windows の kill は信号の名前によらず打ち切り)。
  function stop() {
    stopped = true;
    info.rearm = false;
    if (current && current.exitCode === null && !current.killed) {
      try {
        current.kill();
      } catch {
        /* 既に居なければ何もしない */
      }
    }
  }

  return {
    info,
    start,
    stop,
    // いま動いている受け口 (無ければ null)。
    current: () => current,
    // 起動できなかった子 (実行ファイルがない = Node は error だけを出して exit を出さず、pid もない) は動いていない。
    alive: () => current !== null && current.exitCode === null && Boolean(current.pid) && !(currentEntry && currentEntry.error),
    summary: () => ({ url: info.url, exe: info.exe, rearm: info.rearm, rearms: info.rearms, files: info.files.map((f) => ({ ...f })) }),
  };
}
