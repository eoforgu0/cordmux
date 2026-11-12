// SPDX-License-Identifier: Apache-2.0

// 受信した音声の断片を、ワイヤー形式の音声のチャンク (f32 のインターリーブ、48 kHz、2 チャンネル、480 サンプル =
// 10 ms) に分け、チャンクごとのタイムスタンプを決める純粋関数。形式は docs/wire_format.md の「音声のペイロード」に
// ある。状態は呼び出し元が持ち、トラックごとに 1 つ作る。

import { createClockMap, mapMediaTime, type ClockMapState } from "../encode/clock_map";

export const AUDIO_SAMPLE_RATE_HZ = 48_000;
export const AUDIO_CHANNELS = 2;
/** 1 チャンクのサンプル数 (10 ms)。 */
export const AUDIO_SAMPLES_PER_CHUNK = 480;
export const AUDIO_BYTES_PER_SAMPLE = 4; // f32
/** 1 チャンクのバイト数 (480 x 2 チャンネル x 4 バイト = 3840 バイト)。 */
export const AUDIO_CHUNK_BYTES = AUDIO_SAMPLES_PER_CHUNK * AUDIO_CHANNELS * AUDIO_BYTES_PER_SAMPLE;

const FRAMES_PER_CHUNK_INTERLEAVED = AUDIO_SAMPLES_PER_CHUNK * AUDIO_CHANNELS;

/** サンプル数 (チャンネルごとのフレーム数) を経過時間 (us) に換算する。 */
function elapsedUsForFrames(frames: number): bigint {
    return (BigInt(frames) * 1_000_000n) / BigInt(AUDIO_SAMPLE_RATE_HZ);
}

/** ちょうど 480 サンプルの 1 チャンク。samples は f32 のインターリーブ (2 チャンネル)。 */
export interface AudioChunk {
    samples: Float32Array;
    /** このチャンクの先頭サンプルの時刻。送信側の時計 (encode/sender_clock.ts) の us で、ワイヤー形式の timestamp_us に入れる。 */
    timestampUs: bigint;
}

/** チャンカーの状態。480 サンプルに満たない端数 (次の呼び出しに持ち越す分) と、メディアの時計の対応を持つ。 */
export interface AudioChunkerState {
    /** 持ち越す分 (f32 のインターリーブ)。長さは常に AUDIO_CHANNELS の倍数で、FRAMES_PER_CHUNK_INTERLEAVED より短い。 */
    readonly pending: Float32Array;
    /** 持ち越す分の先頭サンプルの時刻 (送信側の時計の us)。持ち越す分がなければ undefined。 */
    readonly pendingTimeUs: bigint | undefined;
    /** メディアのタイムスタンプと送信側の時計の対応 (encode/clock_map.ts)。 */
    readonly clockMap: ClockMapState;
}

/**
 * チャンカーを初期状態で作る。トラックが新しくなったときは作り直す。メディアの時計との対応を、トラックごとに決め直す
 * ためである。
 */
export function createAudioChunker(): AudioChunkerState {
    return { pending: new Float32Array(0), pendingTimeUs: undefined, clockMap: createClockMap() };
}

export interface PushAudioResult {
    state: AudioChunkerState;
    /** この呼び出しで 480 サンプルがそろったチャンク。0 個のこともある。 */
    chunks: AudioChunk[];
}

/**
 * 受信した音声の断片 (f32 のインターリーブ) を加え、480 サンプルごとのチャンクに分ける。端数は次の呼び出しに
 * 持ち越す。断片の長さが AUDIO_CHANNELS の倍数でなければ RangeError を投げる。
 *
 * mediaTimestampUs は断片の先頭サンプルのメディアのタイムスタンプ (製品では AudioData.timestamp。Chromium が受信した音を
 * 再生のために取り出した時刻)、arrivalUs は断片を受け取ったときの送信側の時計 (us)。チャンクのタイムスタンプは、映像と
 * 同じ規則で決める。
 *
 * - 断片の時刻は、メディアのタイムスタンプを encode/clock_map.ts で送信側の時計に換算した値にする (映像の
 *   VideoFrame.timestamp と同じ換算)。届いた時刻は換算の定数を決めるのにだけ使い、時刻そのものには使わない。
 *   受け取るまでの詰まりが時刻に入らないためである。
 * - チャンクの時刻は、そのチャンクの先頭サンプルが入っていた断片の時刻 + 断片の中のサンプルの位置 にする。前の断片から
 *   持ち越したサンプルは、その断片の時刻から数える。
 * - 累積のサンプル数から時刻を作らない。音を取り出す時計 (音の装置の時計) と送信側の時計の速さの差は、サンプル数で
 *   数えると時間に比例してずれていく。断片ごとの時刻で付け、速さの差は受信側が連続的に吸収する
 *   (src-obs/align/audio_drift.h)。
 */
export function pushAudio(
    state: AudioChunkerState,
    samples: Float32Array,
    mediaTimestampUs: number,
    arrivalUs: bigint,
): PushAudioResult {
    if (samples.length % AUDIO_CHANNELS !== 0) {
        throw new RangeError(
            `audio/chunk: 断片の長さ (${samples.length}) がチャンネル数 (${AUDIO_CHANNELS}) の倍数ではありません`,
        );
    }

    const mapped = mapMediaTime(state.clockMap, mediaTimestampUs, arrivalUs);
    const pendingFrames = state.pending.length / AUDIO_CHANNELS;
    const pendingTimeUs = state.pendingTimeUs ?? mapped.timeUs;
    // 合わせた並びの frame 番目のサンプルの時刻。持ち越した分は前の断片の時刻から、この断片の分はこの断片の時刻から数える。
    const timeOfFrame = (frame: number): bigint =>
        frame < pendingFrames
            ? pendingTimeUs + elapsedUsForFrames(frame)
            : mapped.timeUs + elapsedUsForFrames(frame - pendingFrames);

    const combined = new Float32Array(state.pending.length + samples.length);
    combined.set(state.pending, 0);
    combined.set(samples, state.pending.length);

    const chunks: AudioChunk[] = [];
    let offset = 0;
    while (combined.length - offset >= FRAMES_PER_CHUNK_INTERLEAVED) {
        chunks.push({
            samples: combined.slice(offset, offset + FRAMES_PER_CHUNK_INTERLEAVED),
            timestampUs: timeOfFrame(offset / AUDIO_CHANNELS),
        });
        offset += FRAMES_PER_CHUNK_INTERLEAVED;
    }

    const rest = combined.slice(offset);
    return {
        state: {
            pending: rest,
            pendingTimeUs: rest.length > 0 ? timeOfFrame(offset / AUDIO_CHANNELS) : undefined,
            clockMap: mapped.state,
        },
        chunks,
    };
}
