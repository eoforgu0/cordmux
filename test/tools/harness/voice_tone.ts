// SPDX-License-Identifier: Apache-2.0

// テスト用の送信側が、声のストリームに音を乗せるかを決める部分。環境変数 CORDMUX_HARNESS_VOICE_TONE_FILE の
// ファイルがある間、中に書いたストリームの番号の声に 1 kHz の音を乗せる (受信側のミキサーが「聞こえる声」と数え、
// 合成のソースが話している人の光を点ける)。中身の読み方と区間の記録は、映像のフレームを止めるファイルと同じ
// (sender_pure.ts の parseFramePauseTargets と FramePauseLedger)。DOM に触れないので、vitest から確認する。

import type { FramePauseInterval } from "./sender_pure";

/** 声に乗せる音の高さ。同期用のマーカーの音 (2 kHz) と区別できる高さにする。 */
export const VOICE_TONE_FREQUENCY_HZ = 1000;

/**
 * 乗せる音の振れ幅 (1 が最大)。同期用のマーカーの音と同じ 0.3 (約 -10 dBFS) で、受信側のミキサーが聞こえる声と
 * 数える下限 (-60 dBFS。src-obs/pipeline/audio_mixer.h) より十分に大きい。
 */
export const VOICE_TONE_AMPLITUDE = 0.3;

/**
 * 音声のチャンク 1 つに乗せる音の高さと振れ幅を決める。
 * - hasSyncTone: このストリームが同期用のマーカーの音を出すストリームか。真なら、既定どおり syncFrequencyHz の音。
 * - toneOn: このストリームの番号がファイルに書かれているか。真なら 1 kHz の音。
 * - どちらでもなければ無音 (振れ幅 0)。チャンクの数と大きさは変わらない。
 */
export function voiceToneOf(
    hasSyncTone: boolean,
    toneOn: boolean,
    syncFrequencyHz: number,
): { frequencyHz: number; amplitude: number } {
    if (hasSyncTone) return { frequencyHz: syncFrequencyHz, amplitude: VOICE_TONE_AMPLITUDE };
    if (toneOn) return { frequencyHz: VOICE_TONE_FREQUENCY_HZ, amplitude: VOICE_TONE_AMPLITUDE };
    return { frequencyHz: syncFrequencyHz, amplitude: 0 };
}

/**
 * samples (チャンネルを交互に並べたサンプル) を正弦波で埋め、次の位相を返す。phase はストリームの最初からの
 * サンプルの番号で、音を出さないチャンクでも進める。音を出し始めたときも、波形がストリームの時刻とつながる。
 */
export function fillTone(
    samples: Float32Array,
    channels: number,
    sampleRateHz: number,
    frequencyHz: number,
    amplitude: number,
    phase: number,
): number {
    const frames = Math.floor(samples.length / channels);
    let p = phase;
    for (let s = 0; s < frames; s++) {
        const v = amplitude * Math.sin((2 * Math.PI * frequencyHz * p) / sampleRateHz);
        for (let ch = 0; ch < channels; ch++) samples[s * channels + ch] = v;
        p++;
    }
    return p;
}

/** 音を乗せた区間 1 つ。時刻の意味は FramePauseInterval と同じ。 */
export interface VoiceToneInterval {
    streamId: number;
    startMs: number;
    startWallMs: number;
    endMs: number | null;
    endWallMs: number | null;
}

/** FramePauseLedger の区間を、結果 JSON の voiceTone.intervals の形にする (フレームの数は声にはないので落とす)。 */
export function voiceToneIntervals(intervals: readonly FramePauseInterval[]): VoiceToneInterval[] {
    return intervals.map((i) => ({
        streamId: i.streamId,
        startMs: i.startMs,
        startWallMs: i.startWallMs,
        endMs: i.endMs,
        endWallMs: i.endWallMs,
    }));
}
