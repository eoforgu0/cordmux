// SPDX-License-Identifier: Apache-2.0

// 受信した音声の断片を、ワイヤー形式の音声のチャンク (f32 のインターリーブ、48 kHz、2 チャンネル、480 サンプル =
// 10 ms) に分け、チャンクごとのタイムスタンプを決める純粋関数。形式は docs/wire_format.md の「音声のペイロード」に
// ある。状態は呼び出し元が持ち、トラックごとに 1 つ作る。

import { createClockMap, mapMediaTime, mediaUsOf, type ClockMapState } from "../encode/clock_map";
import { fromPageUs, fromTicksUs, type SenderTimebase } from "../encode/sender_clock";

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

/**
 * メディアのタイムスタンプ (AudioData.timestamp) の時刻の基準。
 * - page: ページの原点からの時間 (performance.now() と同じ基準)。Chromium 156 の MediaStreamTrackProcessor の音声はこれで
 *   ある (blink の breakout_box/media_stream_audio_track_underlying_source.cc。機能 kBreakoutBoxExposePageRelativeAudioCaptureTime
 *   が既定で有効)。
 * - ticks: Chromium の TimeTicks の値 (その機能が無効になったバージョン)。送信側の時計が qpc のときだけ使える。
 * - estimated: どちらでもない。今までの換算 (encode/clock_map.ts) を使う。
 */
export type AudioTimeDomain = "page" | "ticks" | "estimated";

/**
 * 時刻の基準を決めるのに許す、読んだ時刻とタイムスタンプの差 (us)。正しい基準なら差は取り出しから読むまでの遅れ (ふつう
 * 1 ms 未満) で、違う基準なら、ページの原点の TimeTicks (PC を起動してからページを開くまでの時間) かそれ以上離れる。
 * 起動したばかりのページで読むのが詰まっても入り、ふつうの PC でページを開くまでの時間よりは小さい値にした。目安の値で、
 * 測って決めたものではない。
 */
export const AUDIO_DOMAIN_TOLERANCE_US = 5_000_000n;

/** チャンカーの状態。480 サンプルに満たない端数 (次の呼び出しに持ち越す分) と、メディアの時計の対応を持つ。 */
export interface AudioChunkerState {
    /** 持ち越す分 (f32 のインターリーブ)。長さは常に AUDIO_CHANNELS の倍数で、FRAMES_PER_CHUNK_INTERLEAVED より短い。 */
    readonly pending: Float32Array;
    /** 持ち越す分の先頭サンプルの時刻 (送信側の時計の us)。持ち越す分がなければ undefined。 */
    readonly pendingTimeUs: bigint | undefined;
    /** メディアのタイムスタンプと送信側の時計の対応 (encode/clock_map.ts)。時刻の基準が estimated のときだけ使う。 */
    readonly clockMap: ClockMapState;
    /** 送信側の時計の決め方。undefined なら、時刻の基準を見ずに今までの換算を使う。 */
    readonly timebase: SenderTimebase | undefined;
    /** メディアのタイムスタンプの時刻の基準。最初の断片で決め、トラックの間は変えない。決める前は undefined。 */
    readonly domain: AudioTimeDomain | undefined;
}

/**
 * チャンカーを初期状態で作る。トラックが新しくなったときは作り直す。メディアの時計との対応を、トラックごとに決め直す
 * ためである。timebase を渡さなければ、今までの換算だけを使う。
 */
export function createAudioChunker(timebase?: SenderTimebase): AudioChunkerState {
    return { pending: new Float32Array(0), pendingTimeUs: undefined, clockMap: createClockMap(), timebase, domain: undefined };
}

function absUs(value: bigint): bigint {
    return value < 0n ? -value : value;
}

/**
 * 断片 1 つのメディアのタイムスタンプ (us) と、それを読んだ送信側の時計 (us) から、時刻の基準を決める。page と ticks の
 * それぞれで送信側の時計にした値のうち、読んだ時刻に近いほうを選ぶ。どちらも AUDIO_DOMAIN_TOLERANCE_US より離れていれば
 * estimated。
 */
export function audioTimeDomain(timebase: SenderTimebase | undefined, mediaUs: bigint, arrivalUs: bigint): AudioTimeDomain {
    if (timebase === undefined) return "estimated";
    let best: AudioTimeDomain = "estimated";
    let bestGapUs = AUDIO_DOMAIN_TOLERANCE_US;
    const pageGapUs = absUs(arrivalUs - fromPageUs(timebase, mediaUs));
    if (pageGapUs < bestGapUs) {
        best = "page";
        bestGapUs = pageGapUs;
    }
    const ticksUs = fromTicksUs(timebase, mediaUs);
    if (ticksUs !== undefined && absUs(arrivalUs - ticksUs) < bestGapUs) best = "ticks";
    return best;
}

/**
 * 今の換算の原点 (送信側の時計 - メディアのタイムスタンプ。us)。page と ticks では送信側の時計の決め方から決まる定数、
 * estimated では clock_map の定数。まだ決まっていなければ undefined。
 */
export function audioOriginUs(state: AudioChunkerState): bigint | undefined {
    if (state.timebase !== undefined && state.domain === "page") return fromPageUs(state.timebase, 0n);
    if (state.timebase !== undefined && state.domain === "ticks") return fromTicksUs(state.timebase, 0n);
    return state.clockMap.offsetUs;
}

export interface PushAudioResult {
    state: AudioChunkerState;
    /** この呼び出しで 480 サンプルがそろったチャンク。0 個のこともある。 */
    chunks: AudioChunk[];
    /** 受け取った断片の先頭サンプルの時刻 (メディアのタイムスタンプを送信側の時計にした値。us)。 */
    timeUs: bigint;
}

/**
 * 受信した音声の断片 (f32 のインターリーブ) を加え、480 サンプルごとのチャンクに分ける。端数は次の呼び出しに
 * 持ち越す。断片の長さが AUDIO_CHANNELS の倍数でなければ RangeError を投げる。
 *
 * mediaTimestampUs は断片の先頭サンプルのメディアのタイムスタンプ (製品では AudioData.timestamp。Chromium が受信した音を
 * 再生のために取り出した時刻)、arrivalUs は断片を受け取ったときの送信側の時計 (us)。
 *
 * - 断片の時刻は、メディアのタイムスタンプを送信側の時計にした値にする。時刻の基準 (AudioTimeDomain) は最初の断片で
 *   決める。page と ticks では、送信側の時計の決め方 (encode/sender_clock.ts) から見積もりなしに決まる。estimated では、
 *   encode/clock_map.ts で換算する (届いた時刻は換算の定数を決めるのにだけ使う)。どれでも、届いた時刻は時刻そのものには
 *   使わない。受け取るまでの詰まりが時刻に入らないためである。
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

    // 基準を決める前と、見積もりなしの換算のときだけ、ここで整数の us にする (estimated の断片は clock_map が同じことをする)。
    const domain =
        state.domain ??
        (state.timebase === undefined
            ? "estimated"
            : audioTimeDomain(state.timebase, mediaUsOf(mediaTimestampUs), arrivalUs));
    let clockMap = state.clockMap;
    let timeUs: bigint;
    if (domain === "page" && state.timebase !== undefined) {
        timeUs = fromPageUs(state.timebase, mediaUsOf(mediaTimestampUs));
    } else if (domain === "ticks" && state.timebase !== undefined) {
        const mediaUs = mediaUsOf(mediaTimestampUs);
        timeUs = fromTicksUs(state.timebase, mediaUs) ?? mediaUs;
    } else {
        const mapped = mapMediaTime(state.clockMap, mediaTimestampUs, arrivalUs);
        clockMap = mapped.state;
        timeUs = mapped.timeUs;
    }
    const pendingFrames = state.pending.length / AUDIO_CHANNELS;
    const pendingTimeUs = state.pendingTimeUs ?? timeUs;
    // 合わせた並びの frame 番目のサンプルの時刻。持ち越した分は前の断片の時刻から、この断片の分はこの断片の時刻から数える。
    const timeOfFrame = (frame: number): bigint =>
        frame < pendingFrames
            ? pendingTimeUs + elapsedUsForFrames(frame)
            : timeUs + elapsedUsForFrames(frame - pendingFrames);

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
            clockMap,
            timebase: state.timebase,
            domain,
        },
        chunks,
        timeUs,
    };
}
