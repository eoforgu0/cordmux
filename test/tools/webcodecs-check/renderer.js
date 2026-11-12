// SPDX-License-Identifier: Apache-2.0

// WebCodecs のエンコーダで次の 2 つを測り、window.webcodecsCheck.report (preload.js) で main.js へ送る。
// (a) エンコーダを作った直後に黒のフレームを 1 秒ぶんエンコードしたとき、最初の実際のフレームが何 ms 遅れるか。
//     エンコーダを 4 つ順に作り、最後の 1 つの値を last とする。
// (b) 同じ設定の 2 つのエンコーダの SPS と PPS が一致するか。組み合わせは、ハードウェア同士、ハードウェアと
//     ソフトウェア、ソフトウェア同士、同じインスタンスの 2 回目の 4 つ。
// 合否は判定せず、測った値をそのまま結果に書く。ok は測れたかどうかを表す。SPS と PPS は nal_pure.js で取り出す。

'use strict';

const NAL = window.cordmuxNal;
const ENV = window.webcodecsCheck.env;

// エンコーダの設定。製品 (src-vencord/encode) と同じく、Annex B で出力する。
function buildConfig(hardwareAcceleration) {
  return {
    codec: 'avc1.42E01F',
    width: ENV.width,
    height: ENV.height,
    bitrate: ENV.bitrate,
    framerate: ENV.fps,
    latencyMode: 'realtime',
    hardwareAcceleration: hardwareAcceleration,
    avc: { format: 'annexb' },
  };
}

function nowMs() {
  return performance.now();
}

// 黒のフレームと黒でないフレームを描く OffscreenCanvas を作る (表示しない)。
function makeCanvas() {
  const canvas = new OffscreenCanvas(ENV.width, ENV.height);
  const ctx = canvas.getContext('2d');
  return { canvas: canvas, ctx: ctx };
}

function drawBlack(c) {
  c.ctx.fillStyle = '#000000';
  c.ctx.fillRect(0, 0, ENV.width, ENV.height);
}

function drawReal(c, i) {
  c.ctx.fillStyle = '#204080';
  c.ctx.fillRect(0, 0, ENV.width, ENV.height);
  c.ctx.fillStyle = '#ffffff';
  c.ctx.fillRect((i * 13) % ENV.width, (i * 7) % ENV.height, 160, 120);
}

function frameFrom(c, timestampUs) {
  return new VideoFrame(c.canvas, { timestamp: timestampUs });
}

// エンコーダを 1 つ作り、出力を sink にためる。出力は非同期に届くので、呼び出し元が flush で待つ。
function createEncoder(hardwareAcceleration, sink) {
  const encoder = new VideoEncoder({
    output: (chunk, meta) => {
      const bytes = new Uint8Array(chunk.byteLength);
      chunk.copyTo(bytes);
      sink.push({ atMs: nowMs(), type: chunk.type, bytes: bytes, meta: meta || null });
    },
    error: (e) => {
      sink.error = String((e && e.message) || e);
    },
  });
  encoder.configure(buildConfig(hardwareAcceleration));
  return encoder;
}

// ---- (a) 黒のフレームが最初の実際のフレームをどれだけ遅らせるか ----------------------

// 1 つのエンコーダで (a) を測る。configure から最初の出力まで、configure から黒の最後の出力まで、黒の最後の
// 出力から実際のフレームの出力までの時間 (ms) を返す。
async function measureOne(hardwareAcceleration) {
  const sink = [];
  sink.error = null;
  const c = makeCanvas();
  const tConfigure = nowMs();
  const encoder = createEncoder(hardwareAcceleration, sink);

  // 黒のフレーム。最初の 1 つはキーフレームにする。製品の黒埋め用のフレームは、それだけでデコードできる必要が
  // あるためである。
  for (let i = 0; i < ENV.blackFrames; i += 1) {
    drawBlack(c);
    const frame = frameFrom(c, Math.round((i * 1000000) / ENV.fps));
    encoder.encode(frame, { keyFrame: i === 0 });
    frame.close();
  }
  await encoder.flush();
  const blackOutputs = sink.length;
  const tBlackDone = blackOutputs > 0 ? sink[blackOutputs - 1].atMs : null;
  const tFirstOutput = blackOutputs > 0 ? sink[0].atMs : null;

  // 黒のフレームの直後の、実際のフレーム 1 つ。
  drawReal(c, 1);
  const realFrame = frameFrom(c, Math.round((ENV.blackFrames * 1000000) / ENV.fps));
  encoder.encode(realFrame, { keyFrame: true });
  realFrame.close();
  await encoder.flush();
  const tFirstReal = sink.length > blackOutputs ? sink[blackOutputs].atMs : null;

  const params = blackOutputs > 0 ? NAL.extractParameterSets(sink[0].bytes) : { sps: null, pps: null, hasIdr: false, unitCount: 0 };
  encoder.close();

  return {
    hardwareAcceleration: hardwareAcceleration,
    error: sink.error,
    blackFrames: ENV.blackFrames,
    blackOutputs: blackOutputs,
    configureToFirstOutputMs: tFirstOutput === null ? null : Math.round(tFirstOutput - tConfigure),
    blackEncodeMs: tBlackDone === null ? null : Math.round(tBlackDone - tConfigure),
    blackToFirstRealMs: tFirstReal === null || tBlackDone === null ? null : Math.round(tFirstReal - tBlackDone),
    firstChunkParams: params,
  };
}

async function measureSerial() {
  const all = [];
  // エンコーダは 1 つずつ順に作る。同時に初期化すると失敗することがあるためである。
  for (let i = 0; i < ENV.serialEncoders; i += 1) {
    window.webcodecsCheck.progress({ stage: 'a', index: i });
    all.push(await measureOne('prefer-hardware'));
  }
  return { encoders: ENV.serialEncoders, all: all, last: all.length > 0 ? all[all.length - 1] : null };
}

// ---- (b) 同じ設定の 2 つのエンコーダの SPS と PPS がビット単位で一致するか --------------------------
// 1 バイトでも違うと、その人の録画は Remux ではなく ProRes で録画ファイルを作成することになる。

// エンコーダを 1 つ作ってキーフレームを 1 つエンコードし、その SPS と PPS を返して閉じる。
async function paramsOf(hardwareAcceleration) {
  const sink = [];
  sink.error = null;
  const c = makeCanvas();
  const encoder = createEncoder(hardwareAcceleration, sink);
  drawReal(c, 0);
  const frame = frameFrom(c, 0);
  encoder.encode(frame, { keyFrame: true });
  frame.close();
  await encoder.flush();
  const params = sink.length > 0 ? NAL.extractParameterSets(sink[0].bytes) : { sps: null, pps: null, hasIdr: false, unitCount: 0 };
  encoder.close();
  if (sink.error) params.error = sink.error;
  return params;
}

// 同じインスタンスで、閉じずにキーフレームを 2 回エンコードしたときの、それぞれの SPS と PPS を返す。
async function paramsSameInstance(hardwareAcceleration) {
  const sink = [];
  sink.error = null;
  const c = makeCanvas();
  const encoder = createEncoder(hardwareAcceleration, sink);
  const shots = [];
  for (let round = 0; round < 2; round += 1) {
    const before = sink.length;
    drawReal(c, round);
    const frame = frameFrom(c, Math.round((round * 1000000) / ENV.fps));
    encoder.encode(frame, { keyFrame: true });
    frame.close();
    await encoder.flush();
    shots.push(sink.length > before ? NAL.extractParameterSets(sink[before].bytes) : { sps: null, pps: null, hasIdr: false, unitCount: 0 });
  }
  encoder.close();
  return shots;
}

async function measurePairs() {
  const pairs = [];

  window.webcodecsCheck.progress({ stage: 'b', pair: 'hardware-hardware' });
  const hw1 = await paramsOf('prefer-hardware');
  const hw2 = await paramsOf('prefer-hardware');
  pairs.push({ name: 'hardware-hardware', ...NAL.compareParameterSets(hw1, hw2) });

  window.webcodecsCheck.progress({ stage: 'b', pair: 'hardware-software' });
  const sw1 = await paramsOf('prefer-software');
  pairs.push({ name: 'hardware-software', ...NAL.compareParameterSets(hw1, sw1) });

  window.webcodecsCheck.progress({ stage: 'b', pair: 'software-software' });
  const sw2 = await paramsOf('prefer-software');
  pairs.push({ name: 'software-software', ...NAL.compareParameterSets(sw1, sw2) });

  window.webcodecsCheck.progress({ stage: 'b', pair: 'same-instance' });
  const shots = await paramsSameInstance('prefer-hardware');
  pairs.push({ name: 'same-instance-second-keyframe', ...NAL.compareParameterSets(shots[0], shots[1]) });

  return { pairs: pairs };
}

// ---- 実行 ----------------------------------------------------------------

function emptyResult(error) {
  return { ok: false, a: null, b: null, error: error };
}

async function run() {
  if (typeof VideoEncoder === 'undefined') {
    window.webcodecsCheck.report(emptyResult('VideoEncoder がない (WebCodecs が使えない)'));
    return;
  }
  let a = null;
  let b = null;
  try {
    a = await measureSerial();
  } catch (e) {
    window.webcodecsCheck.report(emptyResult('(a) の測定で例外: ' + ((e && e.message) || e)));
    return;
  }
  try {
    b = await measurePairs();
  } catch (e) {
    window.webcodecsCheck.report({ ok: false, a: a, b: null, error: '(b) の測定で例外: ' + ((e && e.message) || e) });
    return;
  }
  // ok は測れたかどうかで、値がよいかどうかではない。値の良し悪しは人が判断する。
  const measured = Boolean(a && a.last && a.last.configureToFirstOutputMs !== null && b && b.pairs.length === 4);
  window.webcodecsCheck.report({ ok: measured, a: a, b: b, error: measured ? null : '値がそろわなかった' });
}

run().catch((e) => {
  window.webcodecsCheck.report(emptyResult('想定外の例外: ' + ((e && e.message) || e)));
});
