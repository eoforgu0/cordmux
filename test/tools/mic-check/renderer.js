// SPDX-License-Identifier: Apache-2.0

// getUserMedia で読んだ音 (フェイクデバイスが読む WAV) を 3 秒ぶん集め、RMS と 1 kHz の成分の強さ (Goertzel 法) を
// 測って、window.micCheck.report (preload.js) で main.js へ送る。

'use strict';

const READ_SECONDS = 3;
const TONE_FREQ_HZ = 1000;
const RMS_OK_MIN = 0.05;
const TONE_RATIO_OK_MIN = 0.5;

function emptyResult(error) {
  return { ok: false, deviceLabel: null, rms: null, toneRatio: null, sampleRate: null, error };
}

function computeRms(samples) {
  let sumSquares = 0;
  for (let i = 0; i < samples.length; i += 1) sumSquares += samples[i] * samples[i];
  return Math.sqrt(sumSquares / samples.length);
}

// ScriptProcessor の 1 チャンク (4096 サンプル) ごとに、Hann 関数で重みを付けた Goertzel 法で 1 kHz の振幅を求める。
// 周波数はビンに丸めず、omega = 2 pi f / fs をそのまま使う。4096 サンプルは 1 kHz の約 85 周期なので、重みを
// 付ければ漏れは小さい。3 秒ぶんを 1 つにつないで測ると、ScriptProcessor の呼び出しが遅れたときにチャンクの境目で
// 入力が抜けて位相が不連続になり、純音でも 1 kHz の成分が散る。実測では、RMS は -6 dBFS の正弦波のとおり 0.354 だった
// のに、toneRatio が 0.11 になった。チャンクごとに測って中央値を取れば、境目の影響を受けない。
function goertzelAmplitudeWindowed(samples, freqHz, sampleRate) {
  const n = samples.length;
  const omega = (2 * Math.PI * freqHz) / sampleRate;
  const coeff = 2 * Math.cos(omega);
  let s1 = 0;
  let s2 = 0;
  let windowSum = 0;
  for (let i = 0; i < n; i += 1) {
    const w = 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / (n - 1));
    windowSum += w;
    const s0 = samples[i] * w + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  const real = s1 - s2 * Math.cos(omega);
  const imag = s2 * Math.sin(omega);
  return (2 * Math.sqrt(real * real + imag * imag)) / windowSum;
}

// 1 チャンクの「1 kHz の振幅 / (RMS x sqrt 2)」。純音なら 1 に近い。
function chunkToneRatio(samples, freqHz, sampleRate) {
  const rms = computeRms(samples);
  if (rms <= 0) return 0;
  return goertzelAmplitudeWindowed(samples, freqHz, sampleRate) / (rms * Math.SQRT2);
}

function median(values) {
  const v = Array.from(values).sort((a, b) => a - b);
  if (v.length === 0) return 0;
  const m = Math.floor(v.length / 2);
  return v.length % 2 === 1 ? v[m] : (v[m - 1] + v[m]) / 2;
}

async function run() {
  let stream;
  try {
    // エコーキャンセル、ノイズ抑制、自動ゲイン調整を切る。既定のままだと正弦波が加工され、1 kHz の成分の割合が
    // 下がる (実測で toneRatio が 0.44 になり、しきい値の 0.5 を下回った)。Discord が加える処理は Discord の設定で
    // 決まるので、ここでは確認しない。
    stream = await navigator.mediaDevices.getUserMedia({
      audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false },
      video: false,
    });
  } catch (e) {
    window.micCheck.report(emptyResult('getUserMedia が失敗した: ' + e.message));
    return;
  }

  const track = stream.getAudioTracks()[0];
  if (!track) {
    window.micCheck.report(emptyResult('音声トラックがない'));
    for (const t of stream.getTracks()) t.stop();
    return;
  }
  const deviceLabel = track.label || null;

  let audioCtx;
  try {
    audioCtx = new AudioContext();
    if (audioCtx.state === 'suspended') await audioCtx.resume();
  } catch (e) {
    window.micCheck.report(emptyResult('AudioContext の初期化が失敗した: ' + e.message));
    for (const t of stream.getTracks()) t.stop();
    return;
  }

  const sampleRate = audioCtx.sampleRate;
  const needed = Math.ceil(sampleRate * READ_SECONDS);
  const source = audioCtx.createMediaStreamSource(stream);
  const processor = audioCtx.createScriptProcessor(4096, 1, 1);
  // ScriptProcessorNode は destination までつながっていないと onaudioprocess が呼ばれない (Web Audio は出力の側から
  // 処理を進める)。音を出さないように、gain を 0 にしたノードを間に挟んで出力の音量を 0 にする。
  const mute = audioCtx.createGain();
  mute.gain.value = 0;

  const chunks = [];
  let collected = 0;
  let finished = false;

  function finish(result) {
    if (finished) return;
    finished = true;
    try {
      processor.disconnect();
    } catch (e) {
      /* 既に切断されていれば何もしない */
    }
    try {
      mute.disconnect();
    } catch (e) {
      /* 同上 */
    }
    try {
      source.disconnect();
    } catch (e) {
      /* 同上 */
    }
    try {
      audioCtx.close();
    } catch (e) {
      /* 同上 */
    }
    for (const t of stream.getTracks()) t.stop();
    window.micCheck.report(result);
  }

  processor.onaudioprocess = (ev) => {
    if (finished) return;
    const input = ev.inputBuffer.getChannelData(0);
    chunks.push(Float32Array.from(input));
    collected += input.length;
    if (collected < needed) return;

    const samples = new Float32Array(collected);
    let offset = 0;
    for (const c of chunks) {
      samples.set(c, offset);
      offset += c.length;
    }
    try {
      const rms = computeRms(samples);
      const ratios = chunks.map((c) => chunkToneRatio(c, TONE_FREQ_HZ, sampleRate));
      const toneRatio = median(ratios);
      const ok = rms > RMS_OK_MIN && toneRatio > TONE_RATIO_OK_MIN;
      finish({ ok, deviceLabel, rms, toneRatio, sampleRate, chunks: ratios.length, error: null });
    } catch (e) {
      finish(emptyResult('測定中の例外: ' + e.message));
    }
  };

  source.connect(processor);
  processor.connect(mute);
  mute.connect(audioCtx.destination);
}

run().catch((e) => {
  window.micCheck.report(emptyResult('想定外の例外: ' + e.message));
});
