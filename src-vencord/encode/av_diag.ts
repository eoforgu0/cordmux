// SPDX-License-Identifier: Apache-2.0

// 画面共有の音声と映像の関係を決めるための診断を、1 秒ごとに集め、定期の行 (`av diag stream N:`) にする純粋関数。
// 測るだけで、動作は変えない。集めるのは 3 つである。
// - 受け取る側の待ちの差 G: 同じときに届いた音声と映像の、届いてからタイムスタンプの時刻までの長さの差。音声は
//   inbound-rtp の totalProcessingDelay (元のパケットが届いた時刻から NetEq から取り出した時刻まで) と、デコードしてから
//   自分の 10 ms が取り出されるまでの残り (パケットの長さ P のとき平均 (P - 10 ms) / 2)。映像は totalProcessingDelay
//   (最初のパケットが届いた時刻からデコードが終わった時刻まで) と、先行 (タイムスタンプ - フレームを読んだ時刻)。
//   G = 音声の待ち - 映像の待ち (正は、音声のほうが長く待つ)。累計の値しか出ないので、1 秒ごとの区間の平均で出す。
// - 音声の出力の遅れ L: media-playout の totalPlayoutDelay / totalSamplesCount の区間の差 (取り出してから音の装置まで)。
// - 読み出しの欠落: MediaStreamTrackProcessor が捨てた数 (discardedFrames)、音声の断片のタイムスタンプの飛び、読む間隔の
//   最大 (ページのメインのスレッドの止まり)。
// 行の項目の意味と読み方は src-vencord/encode/README.md の「受け取りの行」にある。
// 状態は呼び出し元 (index.ts と encode/session.ts) が持つ。Vencord も DOM の型も import しない。

import type { StatsReportLike } from "../adapter/types";
import { findInboundRtpRow } from "./playout_diag";

/** 統計を読む間隔 (ms)。G の区間の長さで、getStats() を呼ぶのはストリームごとにこの間隔で 1 回までにする。 */
export const AV_DIAG_SAMPLE_INTERVAL_MS = 1_000;

/**
 * NetEq が 1 回に取り出す長さ (ms)。WebRTC の音声の受信は 10 ms ずつ取り出すので、P ms のパケットをデコードした音声は、
 * 平均 (P - 10) / 2 ms だけ取り出されるのを待つ。
 */
export const NETEQ_PULL_MS = 10;

/** 音声のコーデックの clockRate が読めないときに使う値 (Hz)。Discord の音声は Opus で 48 kHz である。 */
export const AUDIO_STATS_FALLBACK_CLOCK_RATE_HZ = 48_000;

/**
 * 音声の断片のタイムスタンプの飛びを数える境 (前の断片の長さに対する割合)。断片が 1 つ欠けると、次の断片は前の断片の
 * 終わりより 1 断片分 (10 ms) 先に始まる。取り出す時刻の揺れ (数 ms 未満) と分けるために、断片の長さの半分を境にする。
 */
export const AUDIO_GAP_FRACTION = 0.5;

/* ==========================================================================
   getStats() の読み取り
   ========================================================================== */

/** 音声の inbound-rtp から読む累計の値。読めなかったキーは undefined。 */
export interface AudioArrivalSample {
    /** 秒の合計。 */
    readonly totalProcessingDelayS: number | undefined;
    /** パケットから取り出したサンプルの数。 */
    readonly jitterBufferEmittedCount: number | undefined;
    readonly packetsReceived: number | undefined;
    readonly packetsDiscarded: number | undefined;
    /** 隠すために作ったサンプルの数 (失われたパケットの代わり)。 */
    readonly concealedSamples: number | undefined;
    /** コーデックの clockRate (Hz)。 */
    readonly clockRateHz: number | undefined;
}

/** 映像の inbound-rtp から読む累計の値。 */
export interface VideoArrivalSample {
    readonly totalProcessingDelayS: number | undefined;
    readonly framesDecoded: number | undefined;
    readonly framesDropped: number | undefined;
    readonly freezeCount: number | undefined;
    readonly pauseCount: number | undefined;
}

/** media-playout の行から読む累計の値。 */
export interface MediaPlayoutSample {
    /** 秒の合計。 */
    readonly totalPlayoutDelayS: number | undefined;
    readonly totalSamplesCount: number | undefined;
}

function finiteNumber(value: unknown): number | undefined {
    return typeof value === "number" && Number.isFinite(value) ? value : undefined;
}

/** 条件に合う最初の行を返す。forEach が例外を投げたら undefined。 */
function findRow(
    report: StatsReportLike | undefined,
    match: (row: Record<string, unknown>) => boolean,
): Record<string, unknown> | undefined {
    if (report === undefined) return undefined;
    let found: Record<string, unknown> | undefined;
    try {
        report.forEach((value) => {
            if (found !== undefined || typeof value !== "object" || value === null) return;
            const row = value as Record<string, unknown>;
            if (match(row)) found = row;
        });
    } catch {
        return undefined;
    }
    return found;
}

/**
 * 音声のレシーバーの getStats() の結果から、G に使う累計の値を読む。inbound-rtp の行がなければ undefined。clockRate は
 * inbound-rtp の codecId が指す codec の行から読む (codecId はログに書かない)。
 */
export function readAudioArrival(report: StatsReportLike | undefined): AudioArrivalSample | undefined {
    const row = findInboundRtpRow(report, "audio");
    if (row === undefined) return undefined;
    const codecId = typeof row.codecId === "string" ? row.codecId : undefined;
    const codec = codecId === undefined ? undefined : findRow(report, (r) => r.type === "codec" && r.id === codecId);
    return {
        totalProcessingDelayS: finiteNumber(row.totalProcessingDelay),
        jitterBufferEmittedCount: finiteNumber(row.jitterBufferEmittedCount),
        packetsReceived: finiteNumber(row.packetsReceived),
        packetsDiscarded: finiteNumber(row.packetsDiscarded),
        concealedSamples: finiteNumber(row.concealedSamples),
        clockRateHz: finiteNumber(codec?.clockRate),
    };
}

/** 映像のレシーバーの getStats() の結果から、G に使う累計の値を読む。inbound-rtp の行がなければ undefined。 */
export function readVideoArrival(report: StatsReportLike | undefined): VideoArrivalSample | undefined {
    const row = findInboundRtpRow(report, "video");
    if (row === undefined) return undefined;
    return {
        totalProcessingDelayS: finiteNumber(row.totalProcessingDelay),
        framesDecoded: finiteNumber(row.framesDecoded),
        framesDropped: finiteNumber(row.framesDropped),
        freezeCount: finiteNumber(row.freezeCount),
        pauseCount: finiteNumber(row.pauseCount),
    };
}

/**
 * 音声のレシーバーの getStats() の結果から、media-playout の行を読む。Chromium はレシーバーで絞った結果に、inbound-rtp の
 * playoutId がたどる行として入れる。id ("AP") ではなく type で探す。行がなければ undefined。
 */
export function readMediaPlayout(report: StatsReportLike | undefined): MediaPlayoutSample | undefined {
    const row = findRow(report, (r) => r.type === "media-playout");
    if (row === undefined) return undefined;
    return {
        totalPlayoutDelayS: finiteNumber(row.totalPlayoutDelay),
        totalSamplesCount: finiteNumber(row.totalSamplesCount),
    };
}

/**
 * MediaStreamTrackProcessor の数 (totalFrames か discardedFrames) を読む。audio/track_source.ts と encode/track_source.ts が
 * 使う。プロパティがない (古い Chromium)、数でない、読むと例外を投げるときは undefined。
 */
export function readProcessorCount(
    processor: { readonly totalFrames?: number; readonly discardedFrames?: number },
    key: "totalFrames" | "discardedFrames",
): number | undefined {
    try {
        const value: unknown = processor[key];
        return typeof value === "number" && Number.isFinite(value) ? value : undefined;
    } catch {
        return undefined;
    }
}

/* ==========================================================================
   映像の先行 (encode/session.ts が足す)
   ========================================================================== */

/** 区間に読んだフレームの先行 (タイムスタンプ - 読んだ時刻) の合計。encode/session.ts がフレームごとに足す。 */
export interface VideoLeadSpan {
    sumUs: number;
    count: number;
    /** そのうち、見積もりの換算 (encode/video_time.ts の estimated) だったフレームの数。 */
    estimated: number;
}

export function createVideoLeadSpan(): VideoLeadSpan {
    return { sumUs: 0, count: 0, estimated: 0 };
}

/** フレーム 1 つの先行 (us) を足す。有限でない値は足さない。 */
export function addVideoLead(span: VideoLeadSpan, leadUs: number, exact: boolean): void {
    if (!Number.isFinite(leadUs)) return;
    span.sumUs += leadUs;
    span.count++;
    if (!exact) span.estimated++;
}

/** 区間の値を返し、空に戻す。返した値は、この後に足した値で変わらない。 */
export function takeVideoLeadSpan(span: VideoLeadSpan): VideoLeadSpan {
    const taken = { sumUs: span.sumUs, count: span.count, estimated: span.estimated };
    span.sumUs = 0;
    span.count = 0;
    span.estimated = 0;
    return taken;
}

/* ==========================================================================
   音声の断片の読み出し (index.ts の pumpAudio() が足す)
   ========================================================================== */

/** 音声の断片の飛びと読む間隔。定期の行のたびに取り出して数を 0 に戻す (前の断片は持ち越す)。 */
export interface AudioReadSpan {
    /** 期間に読んだ断片の数。 */
    blocks: number;
    /** 期間に、前の断片の終わりより断片の長さの AUDIO_GAP_FRACTION 倍を超えて先に始まった断片の数。 */
    gaps: number;
    /** 期間の、前の断片の終わりとの差の最大と最小 (us)。正は先に飛んだこと。差を出せた断片がなければ undefined。 */
    gapMaxUs: number | undefined;
    gapMinUs: number | undefined;
    /** 期間の、前の断片を読んでからの間隔の最大 (us)。 */
    readMaxUs: number | undefined;
    /** 前の断片の終わり (メディアのタイムスタンプの us) と長さ (us) と、読んだ時刻。 */
    prevEndUs: number | undefined;
    prevDurationUs: number | undefined;
    prevReadUs: bigint | undefined;
}

export function createAudioReadSpan(): AudioReadSpan {
    return {
        blocks: 0,
        gaps: 0,
        gapMaxUs: undefined,
        gapMinUs: undefined,
        readMaxUs: undefined,
        prevEndUs: undefined,
        prevDurationUs: undefined,
        prevReadUs: undefined,
    };
}

/**
 * 断片 1 つを読んだことを足す。mediaTimestampUs は AudioData.timestamp、frames はチャンネルごとのサンプル数、readUs は
 * 読んだ時刻 (送信側の時計)。タイムスタンプが有限でない、sampleRateHz が正でないときは、その断片の差は数えない。
 */
export function noteAudioRead(
    span: AudioReadSpan,
    mediaTimestampUs: number,
    frames: number,
    sampleRateHz: number,
    readUs: bigint,
): void {
    span.blocks++;
    if (span.prevReadUs !== undefined) {
        const intervalUs = Number(readUs - span.prevReadUs);
        if (span.readMaxUs === undefined || intervalUs > span.readMaxUs) span.readMaxUs = intervalUs;
    }
    span.prevReadUs = readUs;
    if (!Number.isFinite(mediaTimestampUs) || !(sampleRateHz > 0) || !(frames >= 0)) {
        span.prevEndUs = undefined;
        span.prevDurationUs = undefined;
        return;
    }
    const durationUs = (frames * 1_000_000) / sampleRateHz;
    if (span.prevEndUs !== undefined && span.prevDurationUs !== undefined) {
        const gapUs = mediaTimestampUs - span.prevEndUs;
        if (gapUs > span.prevDurationUs * AUDIO_GAP_FRACTION) span.gaps++;
        if (span.gapMaxUs === undefined || gapUs > span.gapMaxUs) span.gapMaxUs = gapUs;
        if (span.gapMinUs === undefined || gapUs < span.gapMinUs) span.gapMinUs = gapUs;
    }
    span.prevEndUs = mediaTimestampUs + durationUs;
    span.prevDurationUs = durationUs;
}

/** 期間の数 (AudioReadSpan のうち、前の断片を除いたもの)。 */
export interface AudioReadSummary {
    readonly blocks: number;
    readonly gaps: number;
    readonly gapMaxUs: number | undefined;
    readonly gapMinUs: number | undefined;
    readonly readMaxUs: number | undefined;
}

/** 期間の数を返し、0 に戻す。前の断片は持ち越すので、期間の境をまたいだ飛びも数える。 */
export function takeAudioReadSpan(span: AudioReadSpan): AudioReadSummary {
    const summary: AudioReadSummary = {
        blocks: span.blocks,
        gaps: span.gaps,
        gapMaxUs: span.gapMaxUs,
        gapMinUs: span.gapMinUs,
        readMaxUs: span.readMaxUs,
    };
    span.blocks = 0;
    span.gaps = 0;
    span.gapMaxUs = undefined;
    span.gapMinUs = undefined;
    span.readMaxUs = undefined;
    return summary;
}

/* ==========================================================================
   1 秒ごとの区間
   ========================================================================== */

/**
 * 区間から G を出せなかった理由。
 * - no-track: そのストリームに音声か映像のトラックがない。
 * - stats: getStats() が返らない、inbound-rtp の行か必要な値がない。
 * - reset: 同じトラックの前の値がない (最初の区間、トラックが替わった) か、累計が戻った。
 * - audio: 区間に音声のサンプルかパケットが 1 つも出ていない。
 * - video: 区間にデコードしたフレームがない。
 * - lead: 区間に読んだフレームがない (先行がない)。
 * - domain: 音声のタイムスタンプが取り出した時刻でない (audio/chunk.ts の estimated か、まだ決まっていない)。
 */
export type AvSkipReason = "no-track" | "stats" | "reset" | "audio" | "video" | "lead" | "domain";

export const AV_SKIP_REASONS: readonly AvSkipReason[] = ["no-track", "stats", "reset", "audio", "video", "lead", "domain"];

/** 区間の入力。audio と video は、そのストリームにその種類のトラックがなければ undefined。 */
export interface AvDiagInput {
    readonly audio: { readonly trackId: string; readonly sample: AudioArrivalSample | undefined } | undefined;
    readonly video: { readonly trackId: string; readonly sample: VideoArrivalSample | undefined } | undefined;
    /** 音声のレシーバーの結果の media-playout の行。 */
    readonly playout: MediaPlayoutSample | undefined;
    /** 前の区間から今までに読んだフレームの先行 (takeVideoLeadSpan() の値)。 */
    readonly lead: VideoLeadSpan;
    /** 音声のタイムスタンプが、取り出した時刻を見積もりなしに換算した値か (audio/chunk.ts の page か ticks)。 */
    readonly audioTimeIsFetch: boolean;
}

/** 区間の値を足し上げたもの (期間ごと)。 */
interface AvSpanSums {
    /** G を出せた区間の G (ms)。 */
    readonly gMs: readonly number[];
    /** そのうち、乱れのない区間 (隠したサンプル、捨てたフレーム、止まり、一時停止がない) の G (ms)。 */
    readonly gCleanMs: readonly number[];
    /** 統計を読んだ区間の数。 */
    readonly intervals: number;
    readonly skipped: Readonly<Record<AvSkipReason, number>>;
    /** G を出せた区間の差の合計。 */
    readonly audioDelayS: number;
    readonly audioSamples: number;
    readonly audioPackets: number;
    /** パケットの長さ (ms) x パケットの数の合計 (区間ごとに clockRate が違っても足せるように、長さで持つ)。 */
    readonly audioPacketMsSum: number;
    readonly videoDelayS: number;
    readonly videoFrames: number;
    readonly leadSumUs: number;
    readonly leadCount: number;
    readonly leadEstimated: number;
    /** media-playout の差の合計と、行を読めた区間の数。 */
    readonly playoutDelayS: number;
    readonly playoutSamples: number;
    readonly playoutRows: number;
}

function emptySums(): AvSpanSums {
    return {
        gMs: [],
        gCleanMs: [],
        intervals: 0,
        skipped: { "no-track": 0, stats: 0, reset: 0, audio: 0, video: 0, lead: 0, domain: 0 },
        audioDelayS: 0,
        audioSamples: 0,
        audioPackets: 0,
        audioPacketMsSum: 0,
        videoDelayS: 0,
        videoFrames: 0,
        leadSumUs: 0,
        leadCount: 0,
        leadEstimated: 0,
        playoutDelayS: 0,
        playoutSamples: 0,
        playoutRows: 0,
    };
}

/** 読み出し口の数 (MediaStreamTrackProcessor の discardedFrames と、encode/track_source.ts が捨てた数)。累計。 */
export interface ReadDropCounts {
    /** MediaStreamTrackProcessor.discardedFrames。読めなければ undefined。 */
    readonly discarded: number | undefined;
    /** MediaStreamTrackProcessor.totalFrames。 */
    readonly total: number | undefined;
}

/** 呼び出し元がストリーム (streamId) ごとに持つ状態。 */
export interface AvDiagState {
    readonly createdAtMs: number;
    readonly lastReportAtMs: number | undefined;
    readonly prevAudio: { readonly trackId: string; readonly sample: AudioArrivalSample } | undefined;
    readonly prevVideo: { readonly trackId: string; readonly sample: VideoArrivalSample } | undefined;
    readonly prevPlayout: MediaPlayoutSample | undefined;
    readonly sums: AvSpanSums;
    /**
     * 前の行の読み出し口の累計 (読み出し口の id と組。読み出し口が替わったら前の値を使わない)。同じトラックから読み出しを
     * 始め直すと (受信のやり直し)、累計は 0 から数え直すので、トラックの id ではなく読み出し口ごとの id で比べる。
     */
    readonly prevAudioDrops: { readonly readerId: string; readonly counts: ReadDropCounts } | undefined;
    readonly prevVideoDrops: { readonly readerId: string; readonly counts: ReadDropCounts; readonly replaced: number } | undefined;
}

export function createAvDiagState(nowMs: number): AvDiagState {
    return {
        createdAtMs: nowMs,
        lastReportAtMs: undefined,
        prevAudio: undefined,
        prevVideo: undefined,
        prevPlayout: undefined,
        sums: emptySums(),
        prevAudioDrops: undefined,
        prevVideoDrops: undefined,
    };
}

/** 2 つの累計の差。どちらかがない、戻ったときは undefined。 */
function grow(prev: number | undefined, value: number | undefined): number | undefined {
    if (prev === undefined || value === undefined || value < prev) return undefined;
    return value - prev;
}

/** 乱れの数の差 (読めない値は 0 として、増えた分だけを数える)。 */
function grewAny(pairs: ReadonlyArray<readonly [number | undefined, number | undefined]>): boolean {
    return pairs.some(([prev, value]) => prev !== undefined && value !== undefined && value > prev);
}

function skip(sums: AvSpanSums, reason: AvSkipReason): AvSpanSums {
    return { ...sums, skipped: { ...sums.skipped, [reason]: sums.skipped[reason] + 1 } };
}

/** 区間の G の計算の結果 (テストと index.ts のため)。 */
export interface AvIntervalResult {
    readonly state: AvDiagState;
    /** G を出せたときの値 (ms)。出せなければ undefined。 */
    readonly gMs: number | undefined;
    readonly skipped: AvSkipReason | undefined;
}

/**
 * 1 秒ごとに読んだ統計を 1 区間として足す。G = [音声の待ち + (P - NETEQ_PULL_MS) / 2] - [映像の待ち + 先行] を、区間の差の
 * 平均で出す。音声の待ち = totalProcessingDelay の差 / jitterBufferEmittedCount の差。P = jitterBufferEmittedCount の差 /
 * (packetsReceived - packetsDiscarded の差) / clockRate。映像の待ち = totalProcessingDelay の差 / framesDecoded の差。
 * 先行 = 区間に読んだフレームの平均。出せない区間は理由を数える。media-playout は G と別に、増えた区間の差を足す。
 */
export function avDiagInterval(state: AvDiagState, input: AvDiagInput): AvIntervalResult {
    let sums: AvSpanSums = { ...state.sums, intervals: state.sums.intervals + 1 };

    // media-playout は音声と映像の組とは別に足す (レシーバーごとではなく、レンダラーの音声の出力全体の値)。
    let prevPlayout = state.prevPlayout;
    if (input.playout !== undefined) {
        sums = { ...sums, playoutRows: sums.playoutRows + 1 };
        const dDelay = grow(prevPlayout?.totalPlayoutDelayS, input.playout.totalPlayoutDelayS);
        const dSamples = grow(prevPlayout?.totalSamplesCount, input.playout.totalSamplesCount);
        if (dDelay !== undefined && dSamples !== undefined && dSamples > 0) {
            sums = { ...sums, playoutDelayS: sums.playoutDelayS + dDelay, playoutSamples: sums.playoutSamples + dSamples };
        }
        prevPlayout = input.playout;
    }

    const a = input.audio?.sample;
    const v = input.video?.sample;
    // 次の区間に残す前の値。読めなかったときは、同じトラックの前の値を残す (差は 2 区間の平均になる)。
    const prevA = input.audio !== undefined && state.prevAudio?.trackId === input.audio.trackId ? state.prevAudio : undefined;
    const prevV = input.video !== undefined && state.prevVideo?.trackId === input.video.trackId ? state.prevVideo : undefined;
    const nextAudio = input.audio === undefined ? undefined : a !== undefined ? { trackId: input.audio.trackId, sample: a } : prevA;
    const nextVideo = input.video === undefined ? undefined : v !== undefined ? { trackId: input.video.trackId, sample: v } : prevV;
    const done = (s: AvSpanSums, gMs: number | undefined, skipped: AvSkipReason | undefined): AvIntervalResult => ({
        state: { ...state, prevAudio: nextAudio, prevVideo: nextVideo, prevPlayout, sums: s },
        gMs,
        skipped,
    });

    if (input.audio === undefined || input.video === undefined) return done(skip(sums, "no-track"), undefined, "no-track");
    if (
        a === undefined ||
        v === undefined ||
        a.totalProcessingDelayS === undefined ||
        a.jitterBufferEmittedCount === undefined ||
        a.packetsReceived === undefined ||
        v.totalProcessingDelayS === undefined ||
        v.framesDecoded === undefined
    ) {
        return done(skip(sums, "stats"), undefined, "stats");
    }
    if (prevA === undefined || prevV === undefined) return done(skip(sums, "reset"), undefined, "reset");
    const pa = prevA.sample;
    const pv = prevV.sample;
    const dADelay = grow(pa.totalProcessingDelayS, a.totalProcessingDelayS);
    const dASamples = grow(pa.jitterBufferEmittedCount, a.jitterBufferEmittedCount);
    const dAPackets = grow(
        pa.packetsReceived === undefined ? undefined : pa.packetsReceived - (pa.packetsDiscarded ?? 0),
        a.packetsReceived - (a.packetsDiscarded ?? 0),
    );
    const dVDelay = grow(pv.totalProcessingDelayS, v.totalProcessingDelayS);
    const dVFrames = grow(pv.framesDecoded, v.framesDecoded);
    if (dADelay === undefined || dASamples === undefined || dAPackets === undefined || dVDelay === undefined || dVFrames === undefined) {
        return done(skip(sums, "reset"), undefined, "reset");
    }
    if (dASamples <= 0 || dAPackets <= 0) return done(skip(sums, "audio"), undefined, "audio");
    if (dVFrames <= 0) return done(skip(sums, "video"), undefined, "video");
    if (input.lead.count <= 0) return done(skip(sums, "lead"), undefined, "lead");
    if (!input.audioTimeIsFetch) return done(skip(sums, "domain"), undefined, "domain");

    const clockRate = a.clockRateHz !== undefined && a.clockRateHz > 0 ? a.clockRateHz : AUDIO_STATS_FALLBACK_CLOCK_RATE_HZ;
    const audioWaitMs = (dADelay / dASamples) * 1000;
    const packetMs = (dASamples / dAPackets / clockRate) * 1000;
    const videoWaitMs = (dVDelay / dVFrames) * 1000;
    const leadMs = input.lead.sumUs / input.lead.count / 1000;
    const gMs = audioWaitMs + (packetMs - NETEQ_PULL_MS) / 2 - (videoWaitMs + leadMs);
    const disturbed = grewAny([
        [pa.concealedSamples, a.concealedSamples],
        [pv.framesDropped, v.framesDropped],
        [pv.freezeCount, v.freezeCount],
        [pv.pauseCount, v.pauseCount],
    ]);
    sums = {
        ...sums,
        gMs: [...sums.gMs, gMs],
        gCleanMs: disturbed ? sums.gCleanMs : [...sums.gCleanMs, gMs],
        audioDelayS: sums.audioDelayS + dADelay,
        audioSamples: sums.audioSamples + dASamples,
        audioPackets: sums.audioPackets + dAPackets,
        audioPacketMsSum: sums.audioPacketMsSum + packetMs * dAPackets,
        videoDelayS: sums.videoDelayS + dVDelay,
        videoFrames: sums.videoFrames + dVFrames,
        leadSumUs: sums.leadSumUs + input.lead.sumUs,
        leadCount: sums.leadCount + input.lead.count,
        leadEstimated: sums.leadEstimated + input.lead.estimated,
    };
    return done(sums, gMs, undefined);
}

/* ==========================================================================
   定期の行
   ========================================================================== */

/** 中央値 (要素が偶数なら中の 2 つの平均)。空なら undefined。 */
export function medianOf(values: readonly number[]): number | undefined {
    if (values.length === 0) return undefined;
    const sorted = [...values].sort((x, y) => x - y);
    const mid = Math.floor(sorted.length / 2);
    return sorted.length % 2 === 1 ? sorted[mid] : ((sorted[mid - 1] as number) + (sorted[mid] as number)) / 2;
}

function formatMs(value: number | undefined): string {
    return value === undefined ? "none" : `${value.toFixed(1)}ms`;
}

function formatSignedMs(value: number | undefined): string {
    if (value === undefined) return "none";
    const rounded = Math.round(value * 10) / 10;
    const normalized = rounded === 0 ? 0 : rounded; // -0 を +0.0 にそろえる。
    return `${normalized >= 0 ? "+" : ""}${normalized.toFixed(1)}ms`;
}

function usToMs(value: number | undefined): number | undefined {
    return value === undefined ? undefined : value / 1000;
}

/** 累計の「+期間/通算」。前の値が同じ読み出し口のものでなければ、期間の値は通算と同じ。読めなければ none。 */
function formatDrops(prev: number | undefined, value: number | undefined): string {
    if (value === undefined) return "none";
    const span = prev === undefined || value < prev ? value : value - prev;
    return `+${span}/${value}`;
}

/** 定期の行に添える、読み出し口の値。そのストリームにその種類のトラックがなければ undefined。 */
export interface AvReportExtras {
    /** readerId は読み出し口を作るたびに変わる値。同じトラックから読み出しを始め直すと変わる。 */
    readonly audio: { readonly readerId: string; readonly read: AudioReadSummary; readonly drops: ReadDropCounts } | undefined;
    readonly video: { readonly readerId: string; readonly drops: ReadDropCounts; readonly replaced: number } | undefined;
}

/**
 * 定期の行を作り、期間の値を 0 に戻した状態を返す。行の形 (実際は 1 行):
 *
 *   av diag stream 3: span=60.0s n=57/60 G=+8.6ms G.med=+8.4ms G.min=+2.0ms G.max=+15.1ms G.cleanMed=+8.1ms G.clean=50
 *     a.proc=45.1ms a.pkt=20.0ms v.proc=30.2ms v.lead=+12.3ms v.leadEst=0
 *     skip.no-track=0 skip.stats=0 skip.reset=1 skip.audio=0 skip.video=1 skip.lead=1 skip.domain=0
 *     L=42.0ms L.rows=60/60 a.blocks=6000 a.gaps=0 a.gapMax=+0.4ms a.gapMin=-0.3ms a.readMax=21.0ms
 *     a.discarded=+0/0 v.discarded=+3/120 v.replaced=+0/0
 *
 * 項目の意味は src-vencord/encode/README.md の「受け取りの行」にある。
 */
export function avDiagReport(
    state: AvDiagState,
    streamId: number,
    nowMs: number,
    extras: AvReportExtras,
): { state: AvDiagState; line: string } {
    const s = state.sums;
    const spanSeconds = (nowMs - (state.lastReportAtMs ?? state.createdAtMs)) / 1000;
    const audioWaitMs = s.audioSamples > 0 ? (s.audioDelayS / s.audioSamples) * 1000 : undefined;
    const packetMs = s.audioPackets > 0 ? s.audioPacketMsSum / s.audioPackets : undefined;
    const videoWaitMs = s.videoFrames > 0 ? (s.videoDelayS / s.videoFrames) * 1000 : undefined;
    const leadMs = s.leadCount > 0 ? s.leadSumUs / s.leadCount / 1000 : undefined;
    const spanG =
        audioWaitMs !== undefined && packetMs !== undefined && videoWaitMs !== undefined && leadMs !== undefined
            ? audioWaitMs + (packetMs - NETEQ_PULL_MS) / 2 - (videoWaitMs + leadMs)
            : undefined;
    const playoutMs = s.playoutSamples > 0 ? (s.playoutDelayS / s.playoutSamples) * 1000 : undefined;
    const gMin = s.gMs.length > 0 ? Math.min(...s.gMs) : undefined;
    const gMax = s.gMs.length > 0 ? Math.max(...s.gMs) : undefined;
    const skipText = AV_SKIP_REASONS.map((r) => `skip.${r}=${s.skipped[r]}`).join(" ");

    const prevAudioDrops = extras.audio !== undefined && state.prevAudioDrops?.readerId === extras.audio.readerId ? state.prevAudioDrops : undefined;
    const prevVideoDrops = extras.video !== undefined && state.prevVideoDrops?.readerId === extras.video.readerId ? state.prevVideoDrops : undefined;
    const audioText =
        extras.audio === undefined
            ? "a.blocks=none a.gaps=none a.gapMax=none a.gapMin=none a.readMax=none a.discarded=none"
            : `a.blocks=${extras.audio.read.blocks} a.gaps=${extras.audio.read.gaps}` +
              ` a.gapMax=${formatSignedMs(usToMs(extras.audio.read.gapMaxUs))}` +
              ` a.gapMin=${formatSignedMs(usToMs(extras.audio.read.gapMinUs))}` +
              ` a.readMax=${formatMs(usToMs(extras.audio.read.readMaxUs))}` +
              ` a.discarded=${formatDrops(prevAudioDrops?.counts.discarded, extras.audio.drops.discarded)}`;
    const videoText =
        extras.video === undefined
            ? "v.discarded=none v.replaced=none"
            : `v.discarded=${formatDrops(prevVideoDrops?.counts.discarded, extras.video.drops.discarded)}` +
              ` v.replaced=${formatDrops(prevVideoDrops?.replaced, extras.video.replaced)}`;

    const line =
        `av diag stream ${streamId}: span=${spanSeconds.toFixed(1)}s n=${s.gMs.length}/${s.intervals}` +
        ` G=${formatSignedMs(spanG)} G.med=${formatSignedMs(medianOf(s.gMs))}` +
        ` G.min=${formatSignedMs(gMin)} G.max=${formatSignedMs(gMax)}` +
        ` G.cleanMed=${formatSignedMs(medianOf(s.gCleanMs))} G.clean=${s.gCleanMs.length}` +
        ` a.proc=${formatMs(audioWaitMs)} a.pkt=${formatMs(packetMs)}` +
        ` v.proc=${formatMs(videoWaitMs)} v.lead=${formatSignedMs(leadMs)} v.leadEst=${s.leadEstimated}` +
        ` ${skipText}` +
        ` L=${formatMs(playoutMs)} L.rows=${s.playoutRows}/${s.intervals}` +
        ` ${audioText} ${videoText}`;

    return {
        state: {
            ...state,
            lastReportAtMs: nowMs,
            sums: emptySums(),
            prevAudioDrops:
                extras.audio === undefined
                    ? undefined
                    : extras.audio.drops.discarded === undefined && prevAudioDrops !== undefined
                      ? prevAudioDrops
                      : { readerId: extras.audio.readerId, counts: extras.audio.drops },
            prevVideoDrops:
                extras.video === undefined
                    ? undefined
                    : extras.video.drops.discarded === undefined && prevVideoDrops !== undefined
                      ? { ...prevVideoDrops, replaced: extras.video.replaced }
                      : { readerId: extras.video.readerId, counts: extras.video.drops, replaced: extras.video.replaced },
        },
        line,
    };
}
