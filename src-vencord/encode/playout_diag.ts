// SPDX-License-Identifier: Apache-2.0

// 画面共有の音声と映像の再生の遅延を測り、送信側のログに書く行を作る (playoutReport ほか)。測るだけで、動作は
// 変えない。画面共有の音声と映像は別々の MediaStream (msid が違う) で届くので、WebRTC は音声と映像の再生をそろえ
// ない。録画で音声と映像がずれたときに、その原因を、受信側の再生の遅延の差、送信側の音声のタイムスタンプの定数の決め直し
// と映像のタイムスタンプの定数の動き (どちらも encode/clock_map.ts) に分けて読めるようにする。
// 状態は呼び出し元 (index.ts) がストリームごとに持つ。readStatsSafely() のほかは純粋関数で、Vencord も DOM の型も
// import しない。

import type { StatsReportLike, TrackKind } from "../adapter/types";
import type { AudioChunkerState } from "../audio/chunk";
import type { RtpCounts } from "../core/stream_check";

/**
 * ストリームを作ってから最初の定期の行までの時間。起動の直後の詰まり (原点が大きく下がる) と最初の置き直しが
 * 収まった後の値を取るために、少し待つ。
 */
export const PLAYOUT_FIRST_REPORT_MS = 10_000;
/** 定期の行の間隔。index.ts は、接続が確立している間だけ、この間隔で書く。 */
export const PLAYOUT_REPORT_INTERVAL_MS = 60_000;
/** getStats() を待つ上限。返らないときに、そのストリームの行が止まったままにならないようにする。 */
export const PLAYOUT_STATS_TIMEOUT_MS = 5_000;
/**
 * イベントの行の上限 (ストリームごと、定期の行と行の間の期間ごと)。超えた分は数だけを、定期の行の
 * eventLinesSkipped に書く。受信側は送信側のログを、行数に上限のあるリングバッファー (src-obs/config/settings.h の
 * js_log_ring_lines。既定は 200 行) に保存する。メディアのタイムスタンプが不連続になり続ける画面共有では、音声の
 * タイムスタンプの置き直しがチャンクごと (1 秒に 100 回) に起きて、ほかの行を押し出す。時間で間引くのでは足りない
 * (1 秒に 1 行でも、4 つのストリームで 1 分に 240 行になり、200 行が 1 分でなくなる)。
 */
export const PLAYOUT_EVENT_LINES_PER_SPAN = 3;

/** inbound-rtp の 1 行から読む数。読めなかったキーは undefined。単位は WebRTC の統計のまま。 */
export interface InboundRtpSample {
    /** 報告の時刻 (ms)。 */
    readonly timestampMs: number | undefined;
    /** 秒の合計。 */
    readonly jitterBufferDelayS: number | undefined;
    /** 音声はサンプルの数、映像はフレームの数。 */
    readonly jitterBufferEmittedCount: number | undefined;
    /** ms (NTP の時計)。 */
    readonly estimatedPlayoutTimestampMs: number | undefined;
    /** 映像だけ。 */
    readonly framesDropped: number | undefined;
    /** 映像だけ。秒の合計。 */
    readonly totalDecodeTimeS: number | undefined;
    /** 映像だけ。 */
    readonly framesDecoded: number | undefined;
}

/**
 * ストリームの、ある種類 (音声か映像) の最後の統計の値。トラックの id と組で持ち、前の値は同じトラックのときだけ
 * 使う。画面共有の音声のトラックは替わることがあり、別のトラックの累計と引き算すると意味のない値になるためである。
 */
export interface PlayoutKindSnapshot {
    readonly trackId: string;
    readonly sample: InboundRtpSample | undefined;
}

/** 呼び出し元がストリーム (streamId) ごとに持つ状態。 */
export interface PlayoutDiagState {
    /** ストリームを作った時刻 (呼び出し元の Date.now())。 */
    readonly createdAtMs: number;
    /** 前の定期の行を書いた時刻。まだなければ undefined。 */
    readonly lastReportAtMs: number | undefined;
    readonly audio: PlayoutKindSnapshot | undefined;
    readonly video: PlayoutKindSnapshot | undefined;
    readonly reanchorTotal: number;
    readonly reanchorSpan: number;
    /** この期間の置き直しの量の合計 (正は、音声のタイムスタンプが後ろへ動いたこと)。 */
    readonly reanchorShiftSpanUs: bigint;
    readonly originDownTotal: number;
    readonly originDownSpan: number;
    readonly originUpTotal: number;
    readonly originUpSpan: number;
    /** この期間に数えた原点の動きの合計 (正は、映像のタイムスタンプが後ろへ動いたこと)。 */
    readonly originShiftSpanUs: bigint;
    readonly eventLinesSpan: number;
    readonly eventLinesSkippedSpan: number;
}

export function createPlayoutDiagState(nowMs: number): PlayoutDiagState {
    return {
        createdAtMs: nowMs,
        lastReportAtMs: undefined,
        audio: undefined,
        video: undefined,
        reanchorTotal: 0,
        reanchorSpan: 0,
        reanchorShiftSpanUs: 0n,
        originDownTotal: 0,
        originDownSpan: 0,
        originUpTotal: 0,
        originUpSpan: 0,
        originShiftSpanUs: 0n,
        eventLinesSpan: 0,
        eventLinesSkippedSpan: 0,
    };
}

/** 結果の 1 行 (書かないときは undefined) と、次の状態。 */
export interface PlayoutDiagResult {
    readonly state: PlayoutDiagState;
    readonly line: string | undefined;
}

/* ==========================================================================
   getStats() の読み取り
   ========================================================================== */

/**
 * レシーバーの getStats() を呼んで待つ。getStats がない、同期に例外を投げる、reject する、timeoutMs のあいだ返らない、
 * のどれでも undefined で終わり、この関数は reject しない。上限より後に返った結果は捨てる。返らない getStats() を
 * 取り消す方法はないので、そのままにする。index.ts ではなくここに置くのは、例外を外へ出さないことを vitest で
 * テストするためである (index.ts は Vencord を import するので、vitest でテストできない)。
 */
export function readStatsSafely(
    getStats: (() => Promise<StatsReportLike>) | undefined,
    timeoutMs: number = PLAYOUT_STATS_TIMEOUT_MS,
): Promise<StatsReportLike | undefined> {
    if (getStats === undefined) return Promise.resolve(undefined);
    return new Promise((resolve) => {
        const timer = setTimeout(() => resolve(undefined), timeoutMs);
        const finish = (report: StatsReportLike | undefined): void => {
            clearTimeout(timer);
            resolve(report);
        };
        try {
            // Promise でない値が返った (テスト用のレシーバーなど) ときは .then が例外を投げ、下の catch で
            // 読めなかったことになる。
            getStats().then(finish, () => finish(undefined));
        } catch {
            finish(undefined);
        }
    });
}

function finiteNumber(value: unknown): number | undefined {
    return typeof value === "number" && Number.isFinite(value) ? value : undefined;
}

/**
 * getStats() の結果から、指定した種類の inbound-rtp の行を 1 つ探し、数だけを読む。ない、読めない、形が違う
 * (forEach が例外を投げる、など) ときは undefined を返し、例外を外へ出さない。1 つのレシーバーの getStats() には
 * inbound-rtp が 1 行だけある前提で、複数あれば最初の行を使う。種類のキー (kind、古い形は mediaType) がない行は、
 * どちらの種類としても受け取る (レシーバーは 1 つの種類しか持たないため)。行にある id、ssrc、trackIdentifier、mid、
 * codecId、transportId、remoteId は読まない。ログに書く識別子を、今ある行の userId より増やさないためである。
 */
export function readInboundRtp(report: StatsReportLike | undefined, kind: TrackKind): InboundRtpSample | undefined {
    const row = findInboundRtpRow(report, kind);
    if (row === undefined) return undefined;
    return {
        timestampMs: finiteNumber(row.timestamp),
        jitterBufferDelayS: finiteNumber(row.jitterBufferDelay),
        jitterBufferEmittedCount: finiteNumber(row.jitterBufferEmittedCount),
        estimatedPlayoutTimestampMs: finiteNumber(row.estimatedPlayoutTimestamp),
        framesDropped: finiteNumber(row.framesDropped),
        totalDecodeTimeS: finiteNumber(row.totalDecodeTime),
        framesDecoded: finiteNumber(row.framesDecoded),
    };
}

/** readInboundRtp() と readRtpCounts() が使う、指定した種類の inbound-rtp の行を 1 つ探す関数。 */
function findInboundRtpRow(report: StatsReportLike | undefined, kind: TrackKind): Record<string, unknown> | undefined {
    if (report === undefined) return undefined;
    const matches: Record<string, unknown>[] = [];
    try {
        report.forEach((value) => {
            if (matches.length > 0 || typeof value !== "object" || value === null) return;
            const candidate = value as Record<string, unknown>;
            if (candidate.type !== "inbound-rtp") return;
            const rowKind = typeof candidate.kind === "string" ? candidate.kind : candidate.mediaType;
            if (typeof rowKind === "string" && rowKind !== kind) return;
            matches.push(candidate);
        });
    } catch {
        return undefined;
    }
    return matches[0];
}

/**
 * getStats() の結果から、受信の累計の数 (inbound-rtp の packetsReceived と framesReceived) を読む。映像が届かない
 * ときに、Discord の側に RTP が届いているかを見るため (core/stream_check.ts) だけに使う。行の探し方と、読めないときに
 * undefined を返すことは readInboundRtp() と同じである。
 */
export function readRtpCounts(report: StatsReportLike | undefined, kind: TrackKind): RtpCounts | undefined {
    const row = findInboundRtpRow(report, kind);
    if (row === undefined) return undefined;
    return { packetsReceived: finiteNumber(row.packetsReceived), framesReceived: finiteNumber(row.framesReceived) };
}

/* ==========================================================================
   定期の行
   ========================================================================== */

/**
 * 合計 (秒) と個数の 2 つの累計から、1 個あたりの平均 (ms) を出す。前の値が使えれば期間の平均、使えなければ
 * (ない、数が戻った) 通算の平均。個数が増えていなければ undefined。
 */
function averageMs(
    prevSum: number | undefined,
    prevCount: number | undefined,
    sum: number | undefined,
    count: number | undefined,
): number | undefined {
    if (sum === undefined || count === undefined) return undefined;
    if (prevSum !== undefined && prevCount !== undefined && count >= prevCount && sum >= prevSum) {
        const deltaCount = count - prevCount;
        return deltaCount > 0 ? ((sum - prevSum) / deltaCount) * 1000 : undefined;
    }
    return count > 0 ? (sum / count) * 1000 : undefined;
}

/** 通算の平均 (ms)。 */
function cumulativeMs(sum: number | undefined, count: number | undefined): number | undefined {
    return averageMs(undefined, undefined, sum, count);
}

/** 期間の増分。前の値が使えないとき (ない、数が戻った) は通算の値をそのまま返す。 */
function delta(prev: number | undefined, value: number | undefined): number | undefined {
    if (value === undefined) return undefined;
    if (prev === undefined || value < prev) return value;
    return value - prev;
}

/** estimatedPlayoutTimestamp から報告の時刻を引いた値。 */
function playoutOffsetMs(sample: InboundRtpSample | undefined): number | undefined {
    if (sample?.estimatedPlayoutTimestampMs === undefined || sample.timestampMs === undefined) return undefined;
    return sample.estimatedPlayoutTimestampMs - sample.timestampMs;
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

function usToMs(value: bigint): number {
    return Number(value) / 1000;
}

function formatCount(span: number | undefined, total: number | undefined): string {
    return span === undefined || total === undefined ? "none" : `+${span}/${total}`;
}

/** 前の統計の値のうち、今回と同じトラックのものだけを返す (PlayoutKindSnapshot)。 */
function previousSample(
    prev: PlayoutKindSnapshot | undefined,
    current: PlayoutKindSnapshot | undefined,
): InboundRtpSample | undefined {
    if (prev === undefined || current === undefined || prev.trackId !== current.trackId) return undefined;
    return prev.sample;
}

/**
 * 次の状態に残す統計の値。読めなかったときは、同じトラックの前の値を残す。そのため次の行の期間の平均は 2 つの期間の
 * 平均になる (平均としては正しい)。span は前の行からの秒数のままなので、読めなかった行 (a.jb=none a.jbAll=none)
 * の次の行だけは、平均の期間が span より長い。
 */
function nextSnapshot(
    prev: PlayoutKindSnapshot | undefined,
    current: PlayoutKindSnapshot | undefined,
): PlayoutKindSnapshot | undefined {
    if (current === undefined) return undefined;
    if (current.sample === undefined && prev !== undefined && prev.trackId === current.trackId) return prev;
    return current;
}

/**
 * 60 秒ごとのタイマーで、このストリームの行を書くかを返す。最初の行 (ストリームを作ってから 10 秒後) をまだ書いて
 * おらず、その時刻にもなっていないストリームは書かない (10 秒後の行の前に 60 秒ごとのタイマーが来たストリームで、
 * 2 行が数秒の差で並ばないため)。10 秒後の行の直後に 60 秒ごとのタイマーが来たときは、span の短い行が 1 つ書かれる
 * (期間の平均としては正しいので、タイミングはそろえない)。
 */
export function shouldReportOnTick(state: PlayoutDiagState, nowMs: number): boolean {
    if (state.lastReportAtMs !== undefined) return true;
    return nowMs - state.createdAtMs >= PLAYOUT_FIRST_REPORT_MS;
}

/**
 * 定期の行を作り、期間ごとの数を 0 に戻した状態を返す。audio と video は、今そのストリームにある種類の統計の値 (その
 * 種類のトラックがなければ undefined。トラックはあるが読めなければ sample が undefined)。
 *
 * 行の形 (実際は 1 行):
 *   playout stream 3: span=60.0s a.jb=41.2ms a.jbAll=43.0ms v.jb=118.7ms v.jbAll=120.1ms v.dec=3.2ms
 *     v.dropped=+0/12 lead.jb=+77.5ms lead.ept=+80.3ms reanchor=+0/1 reanchorShift=+0.0ms
 *     originDown=+0/0 originUp=+0/0 originShift=+0.0ms eventLinesSkipped=0
 *
 * - stream N: ワイヤー形式の stream_id。誰の画面共有かは、同じログの `screen entry: ... userId=... streamId=N` の行で
 *   調べる (この行には識別子を増やさない)。
 * - span: 前の定期の行から (最初の行はストリームを作ってから) の秒数。
 * - a.jb、v.jb: 音声と映像の inbound-rtp の jitterBufferDelay の増分 / jitterBufferEmittedCount の増分 (ms)。前の行
 *   からの期間の平均で、時間とともに変わるずれを読むために使う (通算の平均は、後になるほど最近の変化を薄める)。前の
 *   値がない、トラックが替わった、数が戻ったときは、そのトラックの始めからの平均にする。期間に 1 つも出ていなければ
 *   none。
 * - a.jbAll、v.jbAll: 同じ比の通算 (トラックの始めから)。長い期間の値と比べるために並べる。
 * - v.dec: 映像の totalDecodeTime の増分 / framesDecoded の増分 (ms)。cordmux は映像をデコードの後で取る
 *   (MediaStreamTrackProcessor) ので、映像の再生の遅延は jitter buffer とデコードの合計になる。音声は NetEQ の後で
 *   取る。totalProcessingDelay は使わない。音声の側で値があるかが Chromium のバージョンで違うためである。
 * - v.dropped: 映像の framesDropped の「+期間/通算」。最初の行と、トラックが替わったか数が戻った後の行では、期間の値は
 *   そのトラックの始めからの数 (通算と同じ)。
 * - lead.jb: v.jb - a.jb (ms)。正は音声が映像より先 (映像のほうが jitter buffer に長くいれば、同じ時刻に作られた
 *   音声が先に再生される)。v.dec は含まない。jitter buffer の差とデコードを別々に読めるようにするためで、測った先行と
 *   比べるのは lead.jb + v.dec である。
 * - lead.ept: (音声の estimatedPlayoutTimestamp - 音声の報告の timestamp) - (映像の同じ差) (ms)。符号は lead.jb と
 *   同じ。estimatedPlayoutTimestamp は、今再生している音声やフレームが送信元で作られた時刻 (NTP) を、報告の時刻まで
 *   外挿した値である。音声と映像の getStats() は別々の呼び出しで報告の時刻が違うので、報告の時刻を引いてから差を取る
 *   (2 回の呼び出しの間の時間が差に入らない)。RTCP の送信報告 (SR) が届いていなければ none。音声と映像の SR が別々の
 *   時計で作られていれば値に意味はない。Discord の中継サーバーが SR を作り直しているかは確認していないので、lead.jb と
 *   並べて読む。
 * - reanchor: 送信側の音声のタイムスタンプの定数の決め直し (encode/clock_map.ts の restarts。メディアの時計が替わったと
 *   みなしたとき) の「+期間/通算」。音声のトラックが替わっても、ストリームの通算は続く。
 * - reanchorShift: 期間の決め直しの量の合計 (ms)。正は、音声のタイムスタンプが後ろへ動いたこと。
 * - originDown、originUp: 送信側の映像のタイムスタンプの定数 (encode/clock_map.ts) が、前に送ったフレームから
 *   1 フレームの周期 (configure の fps) より大きく下がった回数と上がった回数の「+期間/通算」。定数はトラックの始めの
 *   2 秒で決まり、その後はメディアの時計が替わったとみなしたときにだけ動く。
 * - originShift: 期間に数えた原点の動きの合計 (ms)。正は、映像のタイムスタンプが後ろへ動いたこと。
 * - eventLinesSkipped: 期間に、上限を超えて書かなかったイベントの行の数。
 *
 * 読み方: 録画か OBS の出力で測った音声の先行と比べる。(1) lead.jb + v.dec (と lead.ept) が測った先行に近ければ、
 * 原因は受信側の再生の遅延の差 (WebRTC が音声と映像をそろえない)。(2) 測った先行が急に変わった時刻にイベントの行が
 * あれば、原因は送信側のタイムスタンプ。先行の変化は「映像のタイムスタンプの動き - 音声のタイムスタンプの動き」に
 * なる (映像の原点が 120 ms 下がれば先行は 120 ms 減り、音声が 45 ms 置き直されて後ろへ動けば先行は 45 ms 減る)。
 * (3) どちらでも説明できない分は、受信側 (整列、録画、OBS) を疑う。
 */
export function playoutReport(
    state: PlayoutDiagState,
    streamId: number,
    nowMs: number,
    audio: PlayoutKindSnapshot | undefined,
    video: PlayoutKindSnapshot | undefined,
): PlayoutDiagResult {
    const a = audio?.sample;
    const v = video?.sample;
    const prevA = previousSample(state.audio, audio);
    const prevV = previousSample(state.video, video);

    const aJb = averageMs(prevA?.jitterBufferDelayS, prevA?.jitterBufferEmittedCount, a?.jitterBufferDelayS, a?.jitterBufferEmittedCount);
    const vJb = averageMs(prevV?.jitterBufferDelayS, prevV?.jitterBufferEmittedCount, v?.jitterBufferDelayS, v?.jitterBufferEmittedCount);
    const vDec = averageMs(prevV?.totalDecodeTimeS, prevV?.framesDecoded, v?.totalDecodeTimeS, v?.framesDecoded);
    const droppedDelta = delta(prevV?.framesDropped, v?.framesDropped);
    const leadJb = aJb !== undefined && vJb !== undefined ? vJb - aJb : undefined;
    const aPlayout = playoutOffsetMs(a);
    const vPlayout = playoutOffsetMs(v);
    const leadEpt = aPlayout !== undefined && vPlayout !== undefined ? aPlayout - vPlayout : undefined;
    const spanSeconds = (nowMs - (state.lastReportAtMs ?? state.createdAtMs)) / 1000;

    const line =
        `playout stream ${streamId}: span=${spanSeconds.toFixed(1)}s` +
        ` a.jb=${formatMs(aJb)} a.jbAll=${formatMs(cumulativeMs(a?.jitterBufferDelayS, a?.jitterBufferEmittedCount))}` +
        ` v.jb=${formatMs(vJb)} v.jbAll=${formatMs(cumulativeMs(v?.jitterBufferDelayS, v?.jitterBufferEmittedCount))}` +
        ` v.dec=${formatMs(vDec)} v.dropped=${formatCount(droppedDelta, v?.framesDropped)}` +
        ` lead.jb=${formatSignedMs(leadJb)} lead.ept=${formatSignedMs(leadEpt)}` +
        ` reanchor=${formatCount(state.reanchorSpan, state.reanchorTotal)}` +
        ` reanchorShift=${formatSignedMs(usToMs(state.reanchorShiftSpanUs))}` +
        ` originDown=${formatCount(state.originDownSpan, state.originDownTotal)}` +
        ` originUp=${formatCount(state.originUpSpan, state.originUpTotal)}` +
        ` originShift=${formatSignedMs(usToMs(state.originShiftSpanUs))}` +
        ` eventLinesSkipped=${state.eventLinesSkippedSpan}`;

    return {
        state: {
            ...state,
            lastReportAtMs: nowMs,
            audio: nextSnapshot(state.audio, audio),
            video: nextSnapshot(state.video, video),
            reanchorSpan: 0,
            reanchorShiftSpanUs: 0n,
            originDownSpan: 0,
            originUpSpan: 0,
            originShiftSpanUs: 0n,
            eventLinesSpan: 0,
            eventLinesSkippedSpan: 0,
        },
        line,
    };
}

/* ==========================================================================
   イベントの行
   ========================================================================== */

/** 数える価値のある映像の原点の動き (originMove() が返す)。 */
export interface OriginMove {
    /** 動いた量 (後 - 前)。負は下がったこと、つまり映像のタイムスタンプが前へ動いたこと。 */
    readonly deltaUs: bigint;
    /** 比べた 1 フレームの周期。 */
    readonly framePeriodUs: bigint;
}

/** fps から 1 フレームの周期 (us) を出す。数でない、0 以下のときは undefined (割り算で例外を投げないため)。 */
export function framePeriodUs(fps: number | undefined): bigint | undefined {
    if (fps === undefined || !Number.isFinite(fps) || fps <= 0) return undefined;
    const period = Math.round(1_000_000 / fps);
    return period > 0 ? BigInt(period) : undefined;
}

/**
 * 前に送ったフレームの原点と今回の原点を比べ、1 フレームの周期より大きく動いていれば返す。動いていない (いちばん
 * 多い) ときは、何も作らずに undefined を返す (送るフレームごとに呼ばれるため)。上がる向きも数える。
 * encode/clock_map.ts は、メディアの時計が替わったとみなすと、どちらの向きにも定数を決め直すので、いつ動いたかがないと、
 * 録画のずれの変化と対応させられない。周期は呼び出し元が渡す fps (index.ts では configure の fps、つまり間引いた後の fps) から
 * 求め、分からなければ数えない (通常のジッターと不連続を分けられないため)。決まった ms のしきい値にしないのは、
 * fps の違う画面共有で同じ意味にならないためである。見るのは送ったフレーム (index.ts の videoSink) だけなので、
 * 間引いたフレームや古くて捨てたフレームの上で起きた動きは、次に送ったフレームの動きに含まれる。
 */
export function originMove(
    previousUs: bigint | undefined,
    currentUs: bigint | undefined,
    fps: number | undefined,
): OriginMove | undefined {
    if (previousUs === undefined || currentUs === undefined || previousUs === currentUs) return undefined;
    const period = framePeriodUs(fps);
    if (period === undefined) return undefined;
    const deltaUs = currentUs - previousUs;
    const magnitude = deltaUs < 0n ? -deltaUs : deltaUs;
    return magnitude > period ? { deltaUs, framePeriodUs: period } : undefined;
}

/**
 * pushAudio の前後の状態から、置き直しで原点が動いた量を返す。置き直しがなかった、原点がまだないときは undefined。
 * 決め直しは定数だけを動かす (encode/clock_map.ts の restart) ので、前後の定数の差が、決め直しでタイムスタンプが動いた
 * 量になる。これで audio/chunk.ts の状態を変えずに測れる。定数を決める期間の中で定数が下がった分は数えない。
 */
export function reanchorShiftUs(before: AudioChunkerState, after: AudioChunkerState): bigint | undefined {
    if (after.clockMap.restarts <= before.clockMap.restarts) return undefined;
    const beforeUs = before.clockMap.offsetUs;
    const afterUs = after.clockMap.offsetUs;
    if (beforeUs === undefined || afterUs === undefined) return undefined;
    return afterUs - beforeUs;
}

/** イベントの行を書くかを決め、上限 (PLAYOUT_EVENT_LINES_PER_SPAN) を超えた分は数えるだけにする。 */
function withEventLine(state: PlayoutDiagState, line: string): PlayoutDiagResult {
    if (state.eventLinesSpan < PLAYOUT_EVENT_LINES_PER_SPAN) {
        return { state: { ...state, eventLinesSpan: state.eventLinesSpan + 1 }, line };
    }
    return { state: { ...state, eventLinesSkippedSpan: state.eventLinesSkippedSpan + 1 }, line: undefined };
}

/**
 * 映像の原点の動き (originMove() が返したもの) を数え、イベントの行を作る。イベントの行は、起きたときに書く。
 * 録画のずれが変わった時刻と照らし合わせられるようにするためである。行の形は
 * `playout event stream 3: video origin -120.4ms (frame 33.3ms)`。
 */
export function recordOriginMove(state: PlayoutDiagState, streamId: number, move: OriginMove): PlayoutDiagResult {
    const down = move.deltaUs < 0n;
    const counted: PlayoutDiagState = {
        ...state,
        originDownTotal: state.originDownTotal + (down ? 1 : 0),
        originDownSpan: state.originDownSpan + (down ? 1 : 0),
        originUpTotal: state.originUpTotal + (down ? 0 : 1),
        originUpSpan: state.originUpSpan + (down ? 0 : 1),
        originShiftSpanUs: state.originShiftSpanUs + move.deltaUs,
    };
    return withEventLine(
        counted,
        `playout event stream ${streamId}: video origin ${formatSignedMs(usToMs(move.deltaUs))} (frame ${formatMs(usToMs(move.framePeriodUs))})`,
    );
}

/**
 * 音声の置き直し (reanchorShiftUs() が返した量) を数え、イベントの行を作る。行の形は
 * `playout event stream 3: audio reanchor +45.2ms (total 2)`。
 */
export function recordReanchor(state: PlayoutDiagState, streamId: number, shiftUs: bigint): PlayoutDiagResult {
    const counted: PlayoutDiagState = {
        ...state,
        reanchorTotal: state.reanchorTotal + 1,
        reanchorSpan: state.reanchorSpan + 1,
        reanchorShiftSpanUs: state.reanchorShiftSpanUs + shiftUs,
    };
    return withEventLine(
        counted,
        `playout event stream ${streamId}: audio reanchor ${formatSignedMs(usToMs(shiftUs))} (total ${counted.reanchorTotal})`,
    );
}
