'use strict';

// NVENC (H.264 1080p60) の同時に確保できるエンコーダの数と失敗の形を計測する。
// テスト A(isConfigSupported) -> B(順次) -> C(同時4個) -> D(一時的な5個目) -> E(降格) の順に行い、
// 1 つの JSON にまとめて window.harness.report() で送る。落ちても部分結果を送る。

const FRAME_US = 16667; // 60fps 相当のフレーム間隔 (マイクロ秒)

const baseConfig = {
  codec: 'avc1.64002A', // High 4.2
  width: 1920,
  height: 1080,
  framerate: 60,
  bitrate: 12_000_000,
  latencyMode: 'realtime',
  avc: { format: 'annexb' },
  hardwareAcceleration: 'prefer-hardware',
};

function errInfo(e) {
  return { name: (e && e.name) || String(e), message: (e && e.message) || String(e) };
}

// 黒い 1080p フレームの元になる共有 canvas。VideoFrame は毎回新規に作って encode 後に close する。
function makeCanvas() {
  const canvas = new OffscreenCanvas(1920, 1080);
  const ctx = canvas.getContext('2d');
  ctx.fillStyle = '#000000';
  ctx.fillRect(0, 0, 1920, 1080);
  return canvas;
}
const sharedCanvas = makeCanvas();

// keyFrame 判定関数。i は 0-indexed のフレーム番号。
function everyNth(n) {
  return (i) => (i + 1) % n === 0; // n フレーム目ごとに true (10 なら 10,20,30 フレーム目)
}
function firstOnly(i) {
  return i === 0; // 先頭だけ IDR、残りは P
}

// VideoEncoder 1 個を作って configure し、以後の状態をまとめて追跡するハンドルを返す。
// output/error コールバックは construct 時に固定されるため、以後の全ステップでこの stats を使い回す。
function createHandle(config) {
  const stats = { outputCount: 0, maxQueueSize: 0, firstChunk: null, errors: [] };
  let rejectError;
  const errorPromise = new Promise((_resolve, reject) => {
    rejectError = reject;
  });
  // race に使われなかった場合の unhandledrejection を防ぐ (rejection 自体は残る)。
  errorPromise.catch(() => {});

  const encoder = new VideoEncoder({
    output: (chunk, metadata) => {
      stats.outputCount++;
      if (!stats.firstChunk) {
        const desc =
          metadata && metadata.decoderConfig && metadata.decoderConfig.description
            ? metadata.decoderConfig.description
            : null;
        stats.firstChunk = {
          byteLength: chunk.byteLength,
          descriptionByteLength: desc ? desc.byteLength : null,
        };
      }
    },
    error: (e) => {
      const info = errInfo(e);
      stats.errors.push(info);
      rejectError(e);
    },
  });

  try {
    encoder.configure(config);
  } catch (e) {
    stats.errors.push(errInfo(e));
    rejectError(e);
  }

  return { encoder, stats, errorPromise, nextTimestamp: 0 };
}

// handle に count フレームを入れて flush する。タイムスタンプは handle をまたいで単調増加させる
// (テスト D で同じ handle に複数回フレームを足すため)。
async function feed(handle, count, keyFrameFn) {
  const { encoder, stats } = handle;
  for (let i = 0; i < count; i++) {
    const timestamp = handle.nextTimestamp;
    handle.nextTimestamp += FRAME_US;
    const keyFrame = keyFrameFn(i);
    const frame = new VideoFrame(sharedCanvas, { timestamp });
    encoder.encode(frame, { keyFrame });
    frame.close();
    if (encoder.encodeQueueSize > stats.maxQueueSize) {
      stats.maxQueueSize = encoder.encodeQueueSize;
    }
  }
  await encoder.flush();
}

// 1 ステップ実行して、成否・所要時間・このステップで増えた output 数を返す。
// feed() と、error コールバックが投げる errorPromise を競わせる。
async function runStep(handle, count, keyFrameFn) {
  const before = handle.stats.outputCount;
  const t0 = performance.now();
  let ok = true;
  let stepError = null;
  try {
    await Promise.race([feed(handle, count, keyFrameFn), handle.errorPromise]);
  } catch (e) {
    ok = false;
    stepError = errInfo(e);
  }
  const durationMs = performance.now() - t0;
  return {
    ok,
    durationMs,
    outputDelta: handle.stats.outputCount - before,
    totalOutputCount: handle.stats.outputCount,
    maxQueueSize: handle.stats.maxQueueSize,
    error: stepError,
    firstChunk: handle.stats.firstChunk,
  };
}

function safeClose(handle) {
  try {
    handle.encoder.close();
  } catch (_e) {
    // 既にエラー状態などで close が失敗しても無視 (計測には影響しない)。
  }
}

// テスト A: isConfigSupported の 3 モードの答え。
async function testA() {
  const modes = ['prefer-hardware', 'prefer-software', 'no-preference'];
  const out = [];
  for (const mode of modes) {
    const cfg = Object.assign({}, baseConfig, { hardwareAcceleration: mode });
    try {
      const result = await VideoEncoder.isConfigSupported(cfg);
      out.push({ mode, supported: result.supported, config: result.config });
    } catch (e) {
      out.push({ mode, error: errInfo(e) });
    }
  }
  return out;
}

// テスト B: 1 個ずつ確保。成功したエンコーダは閉じずに保持し、最大 8 個、またはエラーまで続ける。
async function testB() {
  const attempts = [];
  const liveHandles = [];
  const t0 = performance.now();
  for (let n = 1; n <= 8; n++) {
    const handle = createHandle(baseConfig);
    const step = await runStep(handle, 30, everyNth(10));
    attempts.push({
      n,
      ok: step.ok,
      outputCount: step.totalOutputCount,
      maxQueueSize: step.maxQueueSize,
      durationMs: step.durationMs,
      error: step.error,
      firstChunk: step.firstChunk,
    });
    if (!step.ok) {
      safeClose(handle);
      break;
    }
    liveHandles.push(handle);
  }
  return { totalDurationMs: performance.now() - t0, attempts, liveCount: liveHandles.length, liveHandles };
}

// テスト C: 4 個を同時 (同一マイクロタスク内で configure) に作り、それぞれ 60 フレーム入れて flush。
async function testC() {
  const t0 = performance.now();
  // 4 個とも await をはさまずに生成 + configure する (同一マイクロタスク)。
  const handles = [
    createHandle(baseConfig),
    createHandle(baseConfig),
    createHandle(baseConfig),
    createHandle(baseConfig),
  ];
  const steps = await Promise.all(handles.map((h) => runStep(h, 60, everyNth(10))));
  // 失敗した (= 既に closed 状態の) エンコーダを D に渡さない。生き残りだけを次に渡す。
  const liveHandles = handles.filter((h, i) => steps[i].ok);
  return {
    totalDurationMs: performance.now() - t0,
    results: steps.map((s, i) => ({
      index: i,
      ok: s.ok,
      outputCount: s.totalOutputCount,
      maxQueueSize: s.maxQueueSize,
      durationMs: s.durationMs,
      error: s.error,
      firstChunk: s.firstChunk,
    })),
    liveCount: liveHandles.length,
    liveHandles,
  };
}

// テスト D: C で生き残ったエンコーダを保持したまま 5 個目を作り、黒 60 フレーム (IDR 1 枚 + P) を
// 入れて flush、5 個目だけ close。その後、生き残りに 30 フレーム足して生きていることを確認する。
// 注意: C が 4 個とも成功した場合だけ「4 個保持 + 一時的な 5 個目」という仕様どおりのテストになる。
// C で一部失敗していた場合、この liveCountAtStart はその分だけ 4 より小さい。
async function testD(liveHandles) {
  const t0 = performance.now();
  const liveCountAtStart = liveHandles.length;

  const fifthHandle = createHandle(baseConfig);
  const fifthStep = await runStep(fifthHandle, 60, firstOnly);
  safeClose(fifthHandle);

  const afterFour = [];
  for (const h of liveHandles) {
    const step = await runStep(h, 30, everyNth(10));
    afterFour.push({
      ok: step.ok,
      outputCount: step.totalOutputCount,
      maxQueueSize: step.maxQueueSize,
      durationMs: step.durationMs,
      error: step.error,
    });
  }

  return {
    totalDurationMs: performance.now() - t0,
    liveCountAtStart,
    fifth: {
      ok: fifthStep.ok,
      outputCount: fifthStep.totalOutputCount,
      maxQueueSize: fifthStep.maxQueueSize,
      durationMs: fifthStep.durationMs,
      error: fifthStep.error,
      firstChunk: fifthStep.firstChunk,
    },
    afterFour,
  };
}

// テスト E: prefer-software で 1 個。降格したときの落ち先を確認する。
async function testE() {
  const cfg = Object.assign({}, baseConfig, { hardwareAcceleration: 'prefer-software' });
  let supportCheck = null;
  try {
    const r = await VideoEncoder.isConfigSupported(cfg);
    supportCheck = { supported: r.supported, config: r.config };
  } catch (e) {
    supportCheck = { error: errInfo(e) };
  }

  const handle = createHandle(cfg);
  const step = await runStep(handle, 30, everyNth(10));
  safeClose(handle);

  return {
    supportCheck,
    ok: step.ok,
    outputCount: step.totalOutputCount,
    maxQueueSize: step.maxQueueSize,
    durationMs: step.durationMs,
    error: step.error,
    firstChunk: step.firstChunk,
  };
}

// ---- 持続負荷モード (HARNESS_MODE=sustain) ----
// N 個を 1 個ずつ直列に確保し (同時生成は初期化エラーを誘発しうるため)、
// 高精度スケジューラで 60fps を刻みながら指定秒数だけ動く映像を送り続ける。

// 1 個分のフレームを描く: 動く図形 + 大きなフレーム番号 + ノイズ矩形 200 個。
// ノイズはエンコードを軽くしすぎない (真っ黒だと圧縮が動作すぎて実運用の負荷に近づかない) ために入れる。
// 機械可読な 32bit フレーム番号: 左上に 16x16px の升を 32 個、白=1 / 黒=0、最上位ビットが左
// (x=0..511, y=0..15)。frames.py がここだけ読む前提なので、他の描画はこの帯に入れない。
function drawFrameNumberBits(ctx, frameNumber) {
  const cellSize = 16;
  for (let bit = 0; bit < 32; bit++) {
    const shift = 31 - bit;
    const value = (frameNumber >>> shift) & 1;
    ctx.fillStyle = value ? '#ffffff' : '#000000';
    ctx.fillRect(bit * cellSize, 0, cellSize, cellSize);
  }
}

function drawSustainFrame(ctx, frameIndex) {
  ctx.fillStyle = '#000000';
  ctx.fillRect(0, 0, 1920, 1080);

  // 動く円: 往復運動。
  const cyclePeriod = 180; // フレーム数
  const phase = (frameIndex % cyclePeriod) / cyclePeriod;
  const sweep = phase < 0.5 ? phase * 2 : 2 - phase * 2; // 0->1->0
  const cx = 140 + sweep * (1920 - 280);
  ctx.fillStyle = '#3366ff';
  ctx.beginPath();
  ctx.arc(cx, 540, 80, 0, Math.PI * 2);
  ctx.fill();

  // 大きなフレーム番号 (人間の目視用)。
  ctx.fillStyle = '#ffffff';
  ctx.font = 'bold 200px sans-serif';
  ctx.textBaseline = 'middle';
  ctx.fillText(String(frameIndex), 80, 900);

  // ノイズ: ランダムな小さい矩形 200 個。左上のフレーム番号の帯 (y<32) には掛からないようにする。
  for (let i = 0; i < 200; i++) {
    const rx = Math.random() * 1920;
    const ry = 32 + Math.random() * (1080 - 32);
    const rw = 4 + Math.random() * 12;
    const rh = 4 + Math.random() * 12;
    const r = (Math.random() * 255) | 0;
    const g = (Math.random() * 255) | 0;
    const b = (Math.random() * 255) | 0;
    ctx.fillStyle = `rgb(${r},${g},${b})`;
    ctx.fillRect(rx, ry, rw, rh);
  }

  // 機械可読なフレーム番号は最後に描いて必ず一番上に出す。
  drawFrameNumberBits(ctx, frameIndex);
}

// 全エンコーダに frameIndex 番目のフレームを送る。既に止まったエンコーダはスキップする。
// frameUs: 1 フレームの時間刻み (マイクロ秒、HARNESS_FPS 由来)。keyintFrames: 何フレームごとに IDR か。
function emitSustainFrame(encoders, frameIndex, frameUs, keyintFrames) {
  const keyFrame = frameIndex % keyintFrames === 0;
  const timestamp = frameIndex * frameUs;
  for (const enc of encoders) {
    if (enc.stopped) continue;
    if (enc.handle.stats.errors.length > 0) {
      enc.stopped = true;
      enc.stopError = enc.handle.stats.errors[enc.handle.stats.errors.length - 1];
      continue;
    }
    try {
      drawSustainFrame(enc.ctx, frameIndex);
      const frame = new VideoFrame(enc.canvas, { timestamp });
      enc.handle.encoder.encode(frame, { keyFrame });
      frame.close();
      enc.sentCount++;
      const q = enc.handle.encoder.encodeQueueSize;
      if (q > enc.handle.stats.maxQueueSize) enc.handle.stats.maxQueueSize = q;
      if (q > enc.windowMaxQueueSize) enc.windowMaxQueueSize = q;
    } catch (e) {
      enc.stopped = true;
      enc.stopError = errInfo(e);
    }
  }
}

// performance.now() 基準の高精度スケジューラ。setInterval は使わない。
// 遅れた場合は次の待ち時間が 0 になるため、追いつくまで連続でフレームを出す。
async function runSustainScheduler(encoders, durationSec, fps, onSecondTick) {
  const totalFrames = Math.round(durationSec * fps);
  const frameIntervalMs = 1000 / fps;
  const frameUs = Math.round(1_000_000 / fps);
  const keyintFrames = Math.max(1, Math.round(2 * fps)); // 約 2 秒に 1 回 IDR
  const t0 = performance.now();
  let lastSecondBoundary = 0;

  for (let frameIndex = 0; frameIndex < totalFrames; frameIndex++) {
    const targetTime = t0 + (frameIndex + 1) * frameIntervalMs;
    const waitMs = Math.max(0, targetTime - performance.now());
    await new Promise((resolve) => setTimeout(resolve, waitMs));

    emitSustainFrame(encoders, frameIndex, frameUs, keyintFrames);

    const elapsedSec = Math.floor((performance.now() - t0) / 1000);
    if (elapsedSec > lastSecondBoundary) {
      lastSecondBoundary = elapsedSec;
      onSecondTick(lastSecondBoundary);
    }
  }
  const finalElapsedSec = Math.ceil((performance.now() - t0) / 1000);
  if (finalElapsedSec > lastSecondBoundary) onSecondTick(finalElapsedSec);

  return (performance.now() - t0) / 1000;
}

function takeSustainSnapshot(encoders, secondIndex) {
  return encoders.map((enc) => {
    const sentThisSec = enc.sentCount - enc.lastSentCount;
    const outputsThisSec = enc.handle.stats.outputCount - enc.lastOutputCount;
    const snap = {
      second: secondIndex,
      index: enc.index,
      outputsPerSec: outputsThisSec,
      maxQueueSizeThisSec: enc.windowMaxQueueSize,
      sentThisSec,
      totalSent: enc.sentCount,
      totalOutput: enc.handle.stats.outputCount,
      stopped: enc.stopped,
      error: enc.stopError,
    };
    enc.lastSentCount = enc.sentCount;
    enc.lastOutputCount = enc.handle.stats.outputCount;
    enc.windowMaxQueueSize = 0;
    return snap;
  });
}

async function runSustain() {
  const env = window.harness.env;
  const sustainSec = env.sustainSec;
  const sustainN = env.sustainN;
  const fps = env.fps;
  const config = Object.assign({}, baseConfig, { hardwareAcceleration: env.sustainHw, framerate: fps });

  const encoders = [];
  for (let i = 0; i < sustainN; i++) {
    const canvas = new OffscreenCanvas(1920, 1080);
    const ctx = canvas.getContext('2d');
    const handle = createHandle(config);
    encoders.push({
      index: i,
      handle,
      canvas,
      ctx,
      sentCount: 0,
      lastSentCount: 0,
      lastOutputCount: 0,
      windowMaxQueueSize: 0,
      stopped: false,
      stopError: null,
    });
    // 次の 1 個を作る前に一呼吸置く。同時生成は初期化エラーを誘発しうるので、
    // ここでは同時に作らず直列に確保する。
    if (i < sustainN - 1) {
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
  }

  const perSecond = [];
  const actualDurationSec = await runSustainScheduler(encoders, sustainSec, fps, (secondIndex) => {
    const snap = takeSustainSnapshot(encoders, secondIndex);
    perSecond.push(snap);
    window.harness.progress({ mode: 'sustain', secondsElapsed: secondIndex, perSecond });
  });

  // 生きているエンコーダは flush してから、全部 close する。
  for (const enc of encoders) {
    if (!enc.stopped) {
      try {
        await enc.handle.encoder.flush();
      } catch (e) {
        enc.stopError = enc.stopError || errInfo(e);
      }
    }
    safeClose(enc.handle);
  }

  const totalFramesEmitted = encoders.reduce((max, e) => Math.max(max, e.sentCount), 0);

  const perEncoderTotals = encoders.map((enc) => ({
    index: enc.index,
    totalSent: enc.sentCount,
    totalOutput: enc.handle.stats.outputCount,
    dropped: enc.sentCount - enc.handle.stats.outputCount,
    maxQueueSize: enc.handle.stats.maxQueueSize,
    stopped: enc.stopped,
    error: enc.stopError,
  }));

  return {
    mode: 'sustain',
    config: { sustainSec, sustainN, sustainHw: env.sustainHw, fps },
    actualDurationSec,
    effectiveFps: totalFramesEmitted / actualDurationSec,
    perSecond,
    perEncoderTotals,
    aborted: false,
    fatalError: null,
  };
}

async function sustainMain() {
  let result;
  try {
    result = await runSustain();
  } catch (e) {
    result = { mode: 'sustain', aborted: true, fatalError: errInfo(e) };
  }
  try {
    await window.harness.report(result);
  } catch (e) {
    console.error('report failed', e);
  }
}

// ---- 送信モード (HARNESS_MODE=send) ----
// 持続負荷モードと同じ描画・直列 configure を使い、VideoEncoder の出力を WebSocket で
// プラグインへ送る。音声 (1kHz 正弦波) も、映像と同じシステム時刻 (epochUs0 起点) の timestampUs で送る。

// 画面いっぱいの白 (同期のマーカー用)。フレーム番号の帯は白地に上書きして番号の連続性を保つ。
function drawSyncWhiteFrame(ctx, frameIndex) {
  ctx.fillStyle = '#ffffff';
  ctx.fillRect(0, 0, 1920, 1080);
  drawFrameNumberBits(ctx, frameIndex);
}

// フレームをバイナリへパックする: u8 type / u32 streamId(LE) / u64 timestampUs(LE) / u8 flags / payload。
function packFrame(type, streamId, timestampUsBig, keyFrame, payload) {
  const buf = new Uint8Array(14 + payload.byteLength);
  const view = new DataView(buf.buffer);
  view.setUint8(0, type);
  view.setUint32(1, streamId, true);
  view.setBigUint64(5, timestampUsBig, true);
  view.setUint8(13, keyFrame ? 1 : 0);
  buf.set(payload, 14);
  return buf.buffer;
}

// 配列 (ms) から mean/p95/max を計算する。空なら全部 null。
function summarizeLatency(samplesMs) {
  if (!samplesMs.length) return { meanMs: null, p95Ms: null, maxMs: null };
  const sorted = samplesMs.slice().sort((a, b) => a - b);
  const mean = samplesMs.reduce((sum, v) => sum + v, 0) / samplesMs.length;
  const p95Index = Math.min(sorted.length - 1, Math.ceil(0.95 * sorted.length) - 1);
  return { meanMs: mean, p95Ms: sorted[p95Index], maxMs: sorted[sorted.length - 1] };
}

// WebSocket に接続する。timeoutMs 以内に open しなければ諦める。
function connectWebSocket(url, timeoutMs) {
  return new Promise((resolve, reject) => {
    let settled = false;
    const ws = new WebSocket(url);
    ws.binaryType = 'arraybuffer';
    const timer = setTimeout(() => {
      if (settled) return;
      settled = true;
      try {
        ws.close();
      } catch (_e) {
        // 接続前の close 失敗は無視。
      }
      reject(new Error(`WebSocket connect timeout after ${timeoutMs}ms (${url})`));
    }, timeoutMs);
    ws.addEventListener('open', () => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      resolve(ws);
    });
    ws.addEventListener('error', () => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      reject(new Error(`WebSocket error while connecting (${url})`));
    });
  });
}

// VideoEncoder 1 個を作り、output をそのまま WebSocket 送信につなぐ (送信モード専用)。
// エンコードの遅延 (encode() を呼んだ時刻 -> output が来た時刻) も chunk.timestamp で対応付けて集計する。
function createSendHandle(config, streamId, sendBinary) {
  const stats = {
    outputCount: 0,
    maxQueueSize: 0,
    firstChunk: null,
    errors: [],
    encodeStartTimes: new Map(), // timestamp -> encode() 呼び出し時刻 (performance.now())
    latencyWindow: [], // 直近 1 秒分 (毎秒のスナップショットで読んでリセットする)
    latencyAll: [], // 全区間 (終了時の mean/p95/max 用)
    warmingUp: false, // HARNESS_WARMUP=1 の間だけ true (呼び出し側が立てる)。この間の出力は捨てる
  };
  const encoder = new VideoEncoder({
    output: (chunk, metadata) => {
      // ウォームアップ中の出力は送らない (捨てるためだけにエンコードする)。
      if (stats.warmingUp) return;
      stats.outputCount++;
      const startTime = stats.encodeStartTimes.get(chunk.timestamp);
      if (startTime !== undefined) {
        const latencyMs = performance.now() - startTime;
        stats.encodeStartTimes.delete(chunk.timestamp);
        stats.latencyWindow.push(latencyMs);
        stats.latencyAll.push(latencyMs);
      }
      const payload = new Uint8Array(chunk.byteLength);
      chunk.copyTo(payload);
      if (!stats.firstChunk) {
        const desc =
          metadata && metadata.decoderConfig && metadata.decoderConfig.description
            ? metadata.decoderConfig.description
            : null;
        stats.firstChunk = { byteLength: chunk.byteLength, descriptionByteLength: desc ? desc.byteLength : null };
      }
      const keyFrame = chunk.type === 'key';
      const frameBuf = packFrame(1, streamId, BigInt(Math.round(chunk.timestamp)), keyFrame, payload);
      sendBinary(frameBuf);
    },
    error: (e) => {
      stats.errors.push(errInfo(e));
    },
  });
  try {
    encoder.configure(config);
  } catch (e) {
    stats.errors.push(errInfo(e));
  }
  return { encoder, stats };
}

// HARNESS_WARMUP=1 のときだけ呼ぶ (A/V のずれの切り分け用)。
// configure 済みのエンコーダへ黒 60 フレーム (fps 分 = 約 1 秒) を encode() して flush() で出し切り、
// 出力は createSendHandle 側の stats.warmingUp で捨てる (呼び出し側が立てて、終わったら戻す)。
// timestamp は wallClockUs() を使い、直後に始まる本番送信と同じ時間軸へ連続させる
// (エンコーダのレート制御に、ウォームアップと本番の間で桁の飛んだ timestamp を渡さないため)。
async function warmupEncoder(handle, fps, wallClockUs) {
  const canvas = new OffscreenCanvas(1920, 1080);
  const ctx = canvas.getContext('2d');
  ctx.fillStyle = '#000000';
  ctx.fillRect(0, 0, 1920, 1080);
  const frameIntervalUs = Math.round(1000000 / fps);
  const t0 = wallClockUs();
  for (let i = 0; i < 60; i++) {
    if (handle.stats.errors.length > 0) break;
    const frame = new VideoFrame(canvas, { timestamp: t0 + i * frameIntervalUs });
    try {
      handle.encoder.encode(frame, { keyFrame: i === 0 });
    } catch (e) {
      handle.stats.errors.push(errInfo(e));
    } finally {
      frame.close();
    }
  }
  try {
    await handle.encoder.flush();
  } catch (e) {
    handle.stats.errors.push(errInfo(e));
  }
}

// performance.now() 基準の固定レートスケジューラ。遅れた分は待たずにまとめて emit して追いつく
// (setTimeout(0) の連鎖によるクランプを避けるため、1 tick ごとに待つのではなく
// 「現在のところ何 tick 出ているべきか」を都度計算してまとめて出す)。
async function runFixedRateLoop(t0, intervalMs, totalTicks, onTick) {
  // tick i (0-indexed) の目標時刻は (i+1)*intervalMs。
  // つまり 最後の tick はちょうど totalTicks*intervalMs (名目上の総時間) で出る。
  let emitted = 0;
  while (emitted < totalTicks) {
    const elapsedMs = performance.now() - t0;
    const due = Math.min(totalTicks, Math.floor(elapsedMs / intervalMs));
    while (emitted < due) {
      onTick(emitted);
      emitted++;
    }
    if (emitted >= totalTicks) break;
    const nextTargetMs = (emitted + 1) * intervalMs;
    const waitMs = Math.max(0, nextTargetMs - (performance.now() - t0));
    await new Promise((resolve) => setTimeout(resolve, waitMs));
  }
}

async function runSend() {
  const env = window.harness.env;
  const wsUrl = env.wsUrl;
  const sustainSec = env.sustainSec;
  const sustainN = env.sustainN;
  const keyintSec = env.keyintSec;
  const syncEverySec = env.syncEverySec;
  const fps = env.fps;
  const bitrate = env.bitrate; // HARNESS_BITRATE (既定 12 Mbps)
  const warmup = env.warmup; // HARNESS_WARMUP (既定は 0。1 のときだけ下の (*) を行う)
  const config = Object.assign({}, baseConfig, { hardwareAcceleration: env.sustainHw, framerate: fps, bitrate });

  // システム時刻のマイクロ秒。VideoFrame/バイナリの timestampUs・音声のタイムスタンプ・syncEvents すべてこれで揃える
  // (プラグイン側が「受信時のシステム時刻 - timestampUs」で遅延を出すため)。WebCodecs は timestamp の
  // 絶対値を気にしない (差分だけ見る) ので巨大な値でも問題ない。
  const epochUs0 = Date.now() * 1000;
  const p0 = performance.now();
  function wallClockUs() {
    return Math.round(epochUs0 + (performance.now() - p0) * 1000);
  }

  // WebSocket はエンコーダの用意 (と (*) のウォームアップ) が全部終わってから開く。sendBinary は
  // 下のループがエンコーダを作る時点では ws がまだないが、実際に呼ばれる (エンコードの出力が来る) のは
  // ウォームアップ中でない実データのエンコードからなので、ws の代入が先に済んでいる。
  let ws = null;
  let wsDead = false;
  let wsDeadReason = null;
  let bytesSentThisSec = 0;
  let maxBufferedThisSec = 0;
  function sendBinary(buf) {
    if (wsDead || !ws) return;
    try {
      ws.send(buf);
      bytesSentThisSec += buf.byteLength;
      const b = ws.bufferedAmount;
      if (b > maxBufferedThisSec) maxBufferedThisSec = b;
    } catch (e) {
      wsDead = true;
      wsDeadReason = errInfo(e).message;
    }
  }

  // N 個を 1 個ずつ直列に確保する (同時生成は初期化エラーを誘発しうるため)。
  // (*) HARNESS_WARMUP=1 のときは、configure した直後 (次の 1 個を確保する前) に黒 60 フレームを
  // エンコードして捨てる。WebSocket はまだ開いていないので、
  // 捨てる出力が誤って送られることはない (createSendHandle 側の stats.warmingUp で防ぐ)。
  const encoders = [];
  for (let i = 0; i < sustainN; i++) {
    const streamId = i + 1;
    const canvas = new OffscreenCanvas(1920, 1080);
    const ctx = canvas.getContext('2d');
    const handle = createSendHandle(config, streamId, sendBinary);
    if (warmup) {
      handle.stats.warmingUp = true;
      await warmupEncoder(handle, fps, wallClockUs);
      handle.stats.warmingUp = false;
    }
    encoders.push({
      index: i,
      streamId,
      handle,
      canvas,
      ctx,
      sentCount: 0,
      lastSentCount: 0,
      lastOutputCount: 0,
      windowMaxQueueSize: 0,
      stopped: false,
      stopError: null,
      audioSentCount: 0,
    });
    if (i < sustainN - 1) {
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
  }

  try {
    ws = await connectWebSocket(wsUrl, 5000);
  } catch (e) {
    for (const enc of encoders) safeClose(enc.handle);
    return {
      mode: 'send',
      config: { wsUrl, sustainSec, sustainN, fps, bitrate, warmup: !!warmup },
      aborted: true,
      fatalError: errInfo(e),
    };
  }
  ws.addEventListener('close', () => {
    wsDead = true;
    wsDeadReason = wsDeadReason || 'closed';
  });
  ws.addEventListener('error', () => {
    wsDead = true;
    wsDeadReason = wsDeadReason || 'error';
  });

  const keyintFrames = Math.max(1, Math.round(keyintSec * fps));
  // マーカー (同期マーカー) の共有状態。映像ループが立て、音声ループはタイムスタンプで判定する。
  const syncState = {
    whiteFramesRemaining: 0,
    audioSyncStartUs: null,
    audioSyncEndUs: null,
    nextSyncBoundarySec: syncEverySec,
    events: [],
  };

  const perSecond = [];
  let lastSecondBoundary = 0;

  function maybeSnapshot(elapsedMs) {
    const elapsedSec = Math.floor(elapsedMs / 1000);
    if (elapsedSec <= lastSecondBoundary) return;
    lastSecondBoundary = elapsedSec;
    const perEncoder = encoders.map((enc) => {
      const sentThisSec = enc.sentCount - enc.lastSentCount;
      const outputsThisSec = enc.handle.stats.outputCount - enc.lastOutputCount;
      const latency = summarizeLatency(enc.handle.stats.latencyWindow);
      const s = {
        index: enc.index,
        outputsPerSec: outputsThisSec,
        maxQueueSizeThisSec: enc.windowMaxQueueSize,
        sentThisSec,
        totalSent: enc.sentCount,
        totalOutput: enc.handle.stats.outputCount,
        encodeLatencyMeanMs: latency.meanMs,
        encodeLatencyMaxMs: latency.maxMs,
        stopped: enc.stopped,
        error: enc.stopError,
      };
      enc.lastSentCount = enc.sentCount;
      enc.lastOutputCount = enc.handle.stats.outputCount;
      enc.windowMaxQueueSize = 0;
      enc.handle.stats.latencyWindow = [];
      return s;
    });
    perSecond.push({
      second: lastSecondBoundary,
      perEncoder,
      bytesSent: bytesSentThisSec,
      maxBufferedAmount: maxBufferedThisSec,
    });
    window.harness.progress({ mode: 'send', secondsElapsed: lastSecondBoundary, perSecond });
    bytesSentThisSec = 0;
    maxBufferedThisSec = 0;
  }

  // 映像・音声の tick スケジューラは t0 起点 (ペース制御専用、名目時刻)。
  // フレーム/チャンクに刻む timestampUs は上の wallClockUs() (システム時刻) を別に使う。
  const t0 = performance.now();

  const videoTotalTicks = Math.round(sustainSec * fps);
  const videoPromise = runFixedRateLoop(t0, 1000 / fps, videoTotalTicks, (frameIndex) => {
    const elapsedMs = performance.now() - t0;
    const frameTimestampUs = wallClockUs();

    const elapsedSec = Math.floor(elapsedMs / 1000);
    if (elapsedSec >= syncState.nextSyncBoundarySec) {
      syncState.whiteFramesRemaining = 3;
      syncState.audioSyncStartUs = frameTimestampUs;
      syncState.audioSyncEndUs = frameTimestampUs + 60000;
      syncState.events.push({ timestampUs: frameTimestampUs });
      syncState.nextSyncBoundarySec += syncEverySec;
    }

    const keyFrame = frameIndex % keyintFrames === 0;
    for (const enc of encoders) {
      if (enc.stopped) continue;
      if (enc.handle.stats.errors.length > 0) {
        enc.stopped = true;
        enc.stopError = enc.handle.stats.errors[enc.handle.stats.errors.length - 1];
        continue;
      }
      try {
        if (syncState.whiteFramesRemaining > 0) drawSyncWhiteFrame(enc.ctx, frameIndex);
        else drawSustainFrame(enc.ctx, frameIndex);
        const frame = new VideoFrame(enc.canvas, { timestamp: frameTimestampUs });
        enc.handle.stats.encodeStartTimes.set(frameTimestampUs, performance.now());
        enc.handle.encoder.encode(frame, { keyFrame });
        frame.close();
        enc.sentCount++;
        const q = enc.handle.encoder.encodeQueueSize;
        if (q > enc.handle.stats.maxQueueSize) enc.handle.stats.maxQueueSize = q;
        if (q > enc.windowMaxQueueSize) enc.windowMaxQueueSize = q;
      } catch (e) {
        enc.stopped = true;
        enc.stopError = errInfo(e);
      }
    }
    if (syncState.whiteFramesRemaining > 0) syncState.whiteFramesRemaining--;

    maybeSnapshot(elapsedMs);
  });

  // 音声: 10ms (480 サンプル) ごと、48kHz ステレオ f32 インターリーブ、1kHz 正弦波 (振幅 0.3)。
  // timestampUs はシステム時刻 (音声ループ開始時点の wallClockUs()) を起点にサンプル数で刻む
  // (10ms 間隔の精度はサンプル数基準のまま保ちつつ、映像と同じシステム時刻の原点に揃える)。
  const audioBaseUs = wallClockUs();
  const sampleRate = 48000;
  const samplesPerChunk = 480;
  const samplePos = new Array(sustainN).fill(0);
  const audioTotalTicks = Math.round(sustainSec * (sampleRate / samplesPerChunk));
  const audioPromise = runFixedRateLoop(t0, 10, audioTotalTicks, (tickIndex) => {
    const timestampUs = audioBaseUs + tickIndex * 10000;
    const inSyncWindow =
      syncState.audioSyncStartUs !== null &&
      timestampUs >= syncState.audioSyncStartUs &&
      timestampUs < syncState.audioSyncEndUs;
    const freq = inSyncWindow ? 2000 : 1000;

    for (const enc of encoders) {
      if (enc.stopped) continue;
      const pcm = new Float32Array(samplesPerChunk * 2);
      let sp = samplePos[enc.index];
      for (let s = 0; s < samplesPerChunk; s++) {
        const v = 0.3 * Math.sin((2 * Math.PI * freq * sp) / sampleRate);
        pcm[s * 2] = v;
        pcm[s * 2 + 1] = v;
        sp++;
      }
      samplePos[enc.index] = sp;
      const payload = new Uint8Array(pcm.buffer);
      const frameBuf = packFrame(2, enc.streamId, BigInt(timestampUs), false, payload);
      sendBinary(frameBuf);
      enc.audioSentCount++;
    }
  });

  await Promise.all([videoPromise, audioPromise]);
  // ループ終了直後の端数秒分もスナップショットへ残す (持続負荷モードと同じ理由)。
  maybeSnapshot(performance.now() - t0);

  // 生きているエンコーダは flush してから、全部 close する。
  for (const enc of encoders) {
    if (!enc.stopped) {
      try {
        await enc.handle.encoder.flush();
      } catch (e) {
        enc.stopError = enc.stopError || errInfo(e);
      }
    }
    safeClose(enc.handle);
  }

  try {
    ws.close();
  } catch (_e) {
    // 送信終了後の close 失敗は無視。
  }

  const perEncoderTotals = encoders.map((enc) => {
    const latency = summarizeLatency(enc.handle.stats.latencyAll);
    return {
      index: enc.index,
      streamId: enc.streamId,
      totalVideoSent: enc.sentCount,
      totalVideoOutput: enc.handle.stats.outputCount,
      droppedVideo: enc.sentCount - enc.handle.stats.outputCount,
      totalAudioSent: enc.audioSentCount,
      maxQueueSize: enc.handle.stats.maxQueueSize,
      encodeLatencyMeanMs: latency.meanMs,
      encodeLatencyP95Ms: latency.p95Ms,
      encodeLatencyMaxMs: latency.maxMs,
      stopped: enc.stopped,
      error: enc.stopError,
    };
  });

  return {
    mode: 'send',
    config: {
      wsUrl,
      sustainSec,
      sustainN,
      sustainHw: env.sustainHw,
      keyintSec,
      syncEverySec,
      fps,
      bitrate,
      warmup: !!warmup,
    },
    perSecond,
    perEncoderTotals,
    syncEvents: syncState.events,
    wsError: wsDead ? wsDeadReason : null,
    aborted: false,
    fatalError: null,
  };
}

async function sendMain() {
  let result;
  try {
    result = await runSend();
  } catch (e) {
    result = { mode: 'send', aborted: true, fatalError: errInfo(e) };
  }
  try {
    await window.harness.report(result);
  } catch (e) {
    console.error('report failed', e);
  }
}

async function main() {
  const results = { a: null, b: null, c: null, d: null, e: null, aborted: false, fatalError: null };

  try {
    results.a = await testA();
    window.harness.progress(results);

    const b = await testB();
    results.b = { totalDurationMs: b.totalDurationMs, attempts: b.attempts, liveCount: b.liveCount };
    window.harness.progress(results);

    // C の前に B で生き残ったエンコーダを全部閉じる。
    for (const h of b.liveHandles) safeClose(h);

    const c = await testC();
    results.c = { totalDurationMs: c.totalDurationMs, results: c.results, liveCount: c.liveCount };
    window.harness.progress(results);

    const d = await testD(c.liveHandles);
    results.d = d;
    window.harness.progress(results);

    // D が終わったら C/D で使った 4 個を閉じる。
    for (const h of c.liveHandles) safeClose(h);

    results.e = await testE();
    window.harness.progress(results);
  } catch (e) {
    results.aborted = true;
    results.fatalError = errInfo(e);
  } finally {
    try {
      await window.harness.report(results);
    } catch (e) {
      console.error('report failed', e);
    }
  }
}

window.addEventListener('error', (event) => {
  console.error('[harness] window error:', event.message, event.filename, event.lineno);
});
window.addEventListener('unhandledrejection', (event) => {
  console.error('[harness] unhandledrejection:', errInfo(event.reason));
});

if (window.harness.env && window.harness.env.mode === 'sustain') {
  sustainMain();
} else if (window.harness.env && window.harness.env.mode === 'send') {
  sendMain();
} else {
  main();
}
