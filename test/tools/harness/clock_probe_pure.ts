/*
 * clock-probe モード (clock_probe.ts) の、DOM に触れない計算だけをここに置く。vitest (Node) から import してテストする。
 *
 * このモードが確かめること (README.md の「clock-probe モード」):
 * - ページの原点 (performance.now() が 0 の時点) の base::TimeTicks の値を、自分自身への WebRTC の接続で測る。
 *   取り込みの時刻が付いたフレームでは、MediaStreamTrackProcessor の VideoFrame.timestamp が TimeTicks の絶対値で、
 *   requestVideoFrameCallback の captureTime がページの原点からの値なので、同じフレーム (RTP のタイムスタンプで組にする) の
 *   差がページの原点の TimeTicks の値になる (pairByRtp と originCandidates)。
 * - 受けた映像の VideoFrame.timestamp が、描く予定の時刻 (render_time)、取り込みの時刻、最初のフレームからの経過の
 *   どれか (classifyVideoTimestamps)。
 * - 受けた音声の AudioData.timestamp と performance.now() の関係 (audioTimestampDomain と lagUs)。
 * - TimeTicks が QueryPerformanceCounter を使っているか (tickProfile と originFromBrackets)。
 * - render_time の列の揺れ (renderDelayResidual)。
 * - 受けるだけの接続で、受けた映像に取り込みの時刻が付かない状態を作る (withoutNonSenderRtt)。
 *
 * 時刻の単位は、名前の末尾で示す (Ms はミリ秒の小数、Us はマイクロ秒の整数)。
 */

/** 値の分布の要約。パーセンタイルは最近順位法 (並べた値のうち、割合に当たる順位の値) で取る。 */
export interface Summary {
    count: number;
    min: number;
    p1: number;
    p5: number;
    p50: number;
    p95: number;
    p99: number;
    max: number;
    mean: number;
}

/** 値の分布を要約する。値がなければ null。 */
export function summarize(values: readonly number[]): Summary | null {
    if (values.length === 0) return null;
    const sorted = [...values].sort((a, b) => a - b);
    const at = (fraction: number): number => {
        const rank = Math.max(1, Math.ceil(fraction * sorted.length));
        return sorted[rank - 1] as number;
    };
    let sum = 0;
    for (const v of sorted) sum += v;
    return {
        count: sorted.length,
        min: sorted[0] as number,
        p1: at(0.01),
        p5: at(0.05),
        p50: at(0.5),
        p95: at(0.95),
        p99: at(0.99),
        max: sorted[sorted.length - 1] as number,
        mean: sum / sorted.length,
    };
}

/** 隣り合う値の差の列 (後 - 前)。 */
export function deltas(values: readonly number[]): number[] {
    const out: number[] = [];
    for (let i = 1; i < values.length; i++) out.push((values[i] as number) - (values[i - 1] as number));
    return out;
}

/** 値を幅 binWidth の区間に数える。区間は [下端, 下端 + binWidth) で、下端の小さい順に並べる。 */
export function histogram(values: readonly number[], binWidth: number): Array<[number, number]> {
    const counts = new Map<number, number>();
    for (const v of values) {
        const lower = Math.floor(v / binWidth) * binWidth;
        counts.set(lower, (counts.get(lower) ?? 0) + 1);
    }
    return [...counts.entries()].sort((a, b) => a[0] - b[0]);
}

const RTP_MODULUS = 2 ** 32;

/**
 * 32 ビットで巡回する RTP のタイムスタンプを、巡回の回数を足した連続の値にする。前の値からの差が半周を超えたら、
 * 巡回したか、少し戻ったかとみなす (届く順が入れ替わったフレームを、1 周先と読まない)。
 */
export class RtpUnwrapper {
    private last: number | null = null;
    private offset = 0;

    unwrap(rtp: number): number {
        if (this.last !== null) {
            const diff = rtp - this.last;
            if (diff < -RTP_MODULUS / 2) this.offset += RTP_MODULUS;
            else if (diff > RTP_MODULUS / 2) this.offset -= RTP_MODULUS;
        }
        this.last = rtp;
        return rtp + this.offset;
    }
}

/** MediaStreamTrackProcessor で読んだ映像のフレーム 1 つ。 */
export interface ProcessorFrame {
    /** 読み終えたときの performance.now()。 */
    readMs: number;
    /** VideoFrame.timestamp。 */
    timestampUs: number;
    /** VideoFrame.metadata().rtpTimestamp。なければ null。 */
    rtp: number | null;
}

/** requestVideoFrameCallback で受けたフレーム 1 つ。時刻はどれもページの原点からのミリ秒。 */
export interface CallbackFrame {
    rtp: number | null;
    captureTimeMs: number | null;
    receiveTimeMs: number | null;
}

/** 同じ RTP のタイムスタンプを持つ、2 か所で読んだ同じフレームの組。 */
export interface FramePair<P, C> {
    processor: P;
    callback: C;
}

/**
 * MediaStreamTrackProcessor と requestVideoFrameCallback で読んだフレームを、RTP のタイムスタンプで組にする。
 * 同じ RTP のタイムスタンプのコールバックが 2 つ以上あれば、最初の 1 つを使う (同じフレームを 2 回表示したときの重複)。
 * RTP のタイムスタンプがないフレームは組にしない。
 */
export function pairByRtp<P extends { rtp: number | null }, C extends { rtp: number | null }>(
    processorFrames: readonly P[],
    callbackFrames: readonly C[],
): Array<FramePair<P, C>> {
    const byRtp = new Map<number, C>();
    for (const c of callbackFrames) {
        if (c.rtp === null) continue;
        if (!byRtp.has(c.rtp)) byRtp.set(c.rtp, c);
    }
    const pairs: Array<FramePair<P, C>> = [];
    for (const p of processorFrames) {
        if (p.rtp === null) continue;
        const c = byRtp.get(p.rtp);
        if (c !== undefined) pairs.push({ processor: p, callback: c });
    }
    return pairs;
}

/**
 * 取り込みの時刻が付いた組から、ページの原点の TimeTicks の値の候補を 1 組に 1 つ出す。
 * 候補 = VideoFrame.timestamp - captureTime x 1000。captureTime のない組は使わない。
 */
export function originCandidates(pairs: ReadonlyArray<FramePair<ProcessorFrame, CallbackFrame>>): number[] {
    const out: number[] = [];
    for (const { processor, callback } of pairs) {
        if (callback.captureTimeMs === null) continue;
        out.push(processor.timestampUs - Math.round(callback.captureTimeMs * 1000));
    }
    return out;
}

/** ページの原点の TimeTicks の値の決め方の結果。 */
export interface OriginEstimate {
    /** 候補の中央値。この値をページの原点の TimeTicks の値として使う。 */
    originUs: number;
    count: number;
    minUs: number;
    maxUs: number;
    /** 候補の最大 - 最小。丸めの 2 つ分 (ページが cross-origin isolated でなければ 200 us 未満) に収まるはず。 */
    spreadUs: number;
    /** 前の 1 割と後ろの 1 割の候補の中央値の差 (後 - 前)。時間が経っても値が変わらないかを見る。 */
    driftUs: number;
}

/** 候補の列 (起きた順) から、ページの原点の TimeTicks の値を 1 つに決める。候補がなければ null。 */
export function estimateOrigin(candidates: readonly number[]): OriginEstimate | null {
    const all = summarize(candidates);
    if (all === null) return null;
    const tenth = Math.max(1, Math.floor(candidates.length / 10));
    const head = summarize(candidates.slice(0, tenth));
    const tail = summarize(candidates.slice(candidates.length - tenth));
    return {
        originUs: all.p50,
        count: all.count,
        minUs: all.min,
        maxUs: all.max,
        spreadUs: all.max - all.min,
        driftUs: (tail as Summary).p50 - (head as Summary).p50,
    };
}

/** performance.now() でメインのプロセスの process.hrtime.bigint() の読みをはさんだ組 1 つ。 */
export interface HrtimeBracket {
    beforeMs: number;
    /** process.hrtime.bigint() の値 (ナノ秒) を 10 進の文字列にしたもの。 */
    hrtimeNs: string;
    afterMs: number;
}

/** はさんだ組から決めた、ページの原点の process.hrtime の値 (マイクロ秒) の範囲。 */
export interface BracketOrigin {
    count: number;
    /** 組ごとの範囲の共通部分の下端と上端。下端 > 上端なら、組どうしが食い違っている。 */
    loUs: number;
    hiUs: number;
    /** 往復の時間が最も短い組の範囲の中央と、その往復の時間。 */
    tightestMidUs: number;
    tightestRttUs: number;
    rttUs: Summary;
}

/**
 * 組ごとに、ページの原点の process.hrtime の値は [hrtime - after x 1000, hrtime - before x 1000] にある。
 * 全部の組の共通部分を取る。performance.now() の丸め (100 us の格子に、乱数で決まる向き) の分だけ、共通部分が
 * 空になることがある (下端 > 上端)。組がなければ null。
 */
export function originFromBrackets(brackets: readonly HrtimeBracket[]): BracketOrigin | null {
    if (brackets.length === 0) return null;
    let lo = -Infinity;
    let hi = Infinity;
    let tightestMid = 0;
    let tightestRtt = Infinity;
    const rtts: number[] = [];
    for (const b of brackets) {
        const hrUs = Number(BigInt(b.hrtimeNs) / 1000n);
        const sampleLo = hrUs - Math.round(b.afterMs * 1000);
        const sampleHi = hrUs - Math.round(b.beforeMs * 1000);
        lo = Math.max(lo, sampleLo);
        hi = Math.min(hi, sampleHi);
        const rtt = sampleHi - sampleLo;
        rtts.push(rtt);
        if (rtt < tightestRtt) {
            tightestRtt = rtt;
            tightestMid = (sampleLo + sampleHi) / 2;
        }
    }
    return {
        count: brackets.length,
        loUs: lo,
        hiUs: hi,
        tightestMidUs: tightestMid,
        tightestRttUs: tightestRtt,
        rttUs: summarize(rtts) as Summary,
    };
}

/** performance.now() を続けて読んだ値の刻みの様子。 */
export interface TickProfile {
    reads: number;
    /** 前の値から変わった回数 + 1。 */
    distinct: number;
    spanMs: number;
    /** 1 ms あたりの、異なる値の数。QueryPerformanceCounter の経路で 100 us の丸めなら約 10、1 ms の分解能の経路なら約 1。 */
    distinctPerMs: number;
    /** 値が変わったときの差 (マイクロ秒)。 */
    stepUs: Summary | null;
    /** 0.1 ms の格子に乗っていない値の数。 */
    offGrid100us: number;
}

/** performance.now() を続けて読んだ値の列から、刻みの様子を出す。値がなければ null。 */
export function tickProfile(readsMs: readonly number[]): TickProfile | null {
    if (readsMs.length === 0) return null;
    const steps: number[] = [];
    let distinct = 1;
    let offGrid = 0;
    for (let i = 0; i < readsMs.length; i++) {
        const v = readsMs[i] as number;
        if (Math.abs(v * 10 - Math.round(v * 10)) > 1e-6) offGrid++;
        if (i > 0 && v !== readsMs[i - 1]) {
            distinct++;
            steps.push(Math.round((v - (readsMs[i - 1] as number)) * 1000));
        }
    }
    const spanMs = (readsMs[readsMs.length - 1] as number) - (readsMs[0] as number);
    return {
        reads: readsMs.length,
        distinct,
        spanMs,
        distinctPerMs: spanMs > 0 ? distinct / spanMs : 0,
        stepUs: summarize(steps),
        offGrid100us: offGrid,
    };
}

/** 時刻の値が、ページの原点からか、TimeTicks の絶対値か。どちらとも言えなければ unknown。 */
export type TimestampDomain = "page-relative" | "absolute" | "unknown";

/** 値と読んだ時刻の差がこの幅に入れば、その時刻の基準とみなす。描く予定の時刻の先行と、届くまでの遅れより十分広い。 */
export const DOMAIN_TOLERANCE_US = 5_000_000;

/**
 * 時刻の値の基準を、読んだ時刻との近さで決める。ページの原点からの値なら readMs x 1000 に近く、TimeTicks の絶対値なら
 * originUs + readMs x 1000 に近い。originUs が分からなければ、絶対値かどうかは決めない。
 */
export function timestampDomain(timestampUs: number, readMs: number, originUs: number | null): TimestampDomain {
    const readUs = Math.round(readMs * 1000);
    if (Math.abs(timestampUs - readUs) < DOMAIN_TOLERANCE_US) return "page-relative";
    if (originUs !== null && Math.abs(timestampUs - originUs - readUs) < DOMAIN_TOLERANCE_US) return "absolute";
    return "unknown";
}

/**
 * 受けた映像の VideoFrame.timestamp の中身の場合。
 * - render-time: 描く予定の時刻の TimeTicks の絶対値 (取り込みの時刻が付かないとき)。
 * - capture-time: 取り込みの時刻を受信側の時計に写した値 (TimeTicks の絶対値)。
 * - elapsed: 最初のフレームからの経過 (すぐ描く指定のとき)。
 */
export type VideoTimestampKind = "render-time" | "capture-time" | "elapsed" | "unknown";

export interface VideoTimestampClass {
    kind: VideoTimestampKind;
    /** 最初のフレームの時刻の基準。 */
    domain: TimestampDomain;
    /** requestVideoFrameCallback が captureTime を 1 度でも返したか。 */
    captureTimeSeen: boolean;
    /** 換算した時刻 - 読んだ時刻 (マイクロ秒) の中央値。描く予定の時刻なら正、取り込みの時刻なら負になるはず。 */
    leadMedianUs: number | null;
    /** 先行の符号が kind の予想と合うか。決まらなければ null。 */
    signAgrees: boolean | null;
}

/** 換算した時刻 (VideoFrame.timestamp - originUs) から読んだ時刻を引いた値の列 (マイクロ秒)。 */
export function leadSeriesUs(frames: readonly ProcessorFrame[], originUs: number): number[] {
    return frames.map((f) => f.timestampUs - originUs - Math.round(f.readMs * 1000));
}

/**
 * 受けた映像の VideoFrame.timestamp の場合を決める。最初のフレームの値の大きさで、絶対値か経過かを分け、絶対値なら
 * captureTime の有無で、描く予定の時刻か取り込みの時刻かを分ける。先行の符号は、分けた結果の裏付けとして別に返す。
 */
export function classifyVideoTimestamps(
    frames: readonly ProcessorFrame[],
    captureTimeSeen: boolean,
    originUs: number | null,
): VideoTimestampClass {
    const first = frames[0];
    if (first === undefined) {
        return { kind: "unknown", domain: "unknown", captureTimeSeen, leadMedianUs: null, signAgrees: null };
    }
    const domain = timestampDomain(first.timestampUs, first.readMs, originUs);
    let kind: VideoTimestampKind = "unknown";
    // 経過は 0 から始まり、読んだ時刻 (ページを開いてからの時間) より小さい。page-relative と区別しにくいので、
    // 最初のフレームの値が小さい (1 フレームの間隔の数個分) ことで見る。
    if (first.timestampUs < 100_000) kind = "elapsed";
    else if (domain === "absolute") kind = captureTimeSeen ? "capture-time" : "render-time";
    const lead = originUs === null ? null : summarize(leadSeriesUs(frames, originUs));
    const leadMedianUs = lead === null ? null : lead.p50;
    let signAgrees: boolean | null = null;
    if (leadMedianUs !== null && kind === "render-time") signAgrees = leadMedianUs > 0;
    if (leadMedianUs !== null && kind === "capture-time") signAgrees = leadMedianUs < 0;
    return { kind, domain, captureTimeSeen, leadMedianUs, signAgrees };
}

/** render_time の列から、RTP のタイムスタンプが示す取り込みの間隔を引いた残り。 */
export interface RenderDelayResidual {
    /** フレームごとの残り (マイクロ秒、最初のフレームを 0 とする)。WebRTC の今の遅延の動きに当たる。 */
    residualUs: number[];
    /** 残りの、隣り合うフレームの差の分布。 */
    stepUs: Summary | null;
    /** 差の絶対値が jumpUs を超えた回数と、そのうち負 (戻った) の回数。 */
    jumps: number;
    negativeJumps: number;
    /** 差が最も小さい (最も戻った) 5 つ。[フレームの番号, 差]。 */
    largestDrops: Array<[number, number]>;
    /** VideoFrame.timestamp が前のフレームより戻った回数。 */
    timestampRegressions: number;
}

/** RTP の映像のタイムスタンプの刻み (90 kHz)。 */
export const VIDEO_RTP_CLOCK_HZ = 90_000;

/**
 * 受けた映像の列 (読んだ順) から、render_time と取り込みの時刻の差の動きを出す。
 * 残り = (timestamp - 最初の timestamp) - (RTP - 最初の RTP) / 90 kHz。RTP のタイムスタンプのないフレームは飛ばす。
 */
export function renderDelayResidual(frames: readonly ProcessorFrame[], jumpUs: number): RenderDelayResidual {
    const unwrapper = new RtpUnwrapper();
    const residual: number[] = [];
    let ts0: number | null = null;
    let rtp0 = 0;
    let regressions = 0;
    let prevTs: number | null = null;
    for (const f of frames) {
        if (prevTs !== null && f.timestampUs < prevTs) regressions++;
        prevTs = f.timestampUs;
        if (f.rtp === null) continue;
        const rtp = unwrapper.unwrap(f.rtp);
        if (ts0 === null) {
            ts0 = f.timestampUs;
            rtp0 = rtp;
        }
        const captureUs = Math.round(((rtp - rtp0) * 1_000_000) / VIDEO_RTP_CLOCK_HZ);
        residual.push(f.timestampUs - ts0 - captureUs);
    }
    const steps = deltas(residual);
    let jumps = 0;
    let negativeJumps = 0;
    const indexed: Array<[number, number]> = [];
    steps.forEach((d, i) => {
        if (Math.abs(d) > jumpUs) {
            jumps++;
            if (d < 0) negativeJumps++;
        }
        indexed.push([i + 1, d]);
    });
    indexed.sort((a, b) => a[1] - b[1]);
    return {
        residualUs: residual,
        stepUs: summarize(steps),
        jumps,
        negativeJumps,
        largestDrops: indexed.slice(0, 5),
        timestampRegressions: regressions,
    };
}

/**
 * SDP の映像の m 行にある、RTCP の指定 (rtcp-fb の種類と、rtcp-xr の値)。重複を除いて並べる。rtcp-fb は「nack pli」の
 * ように種類だけを、rtcp-xr は「rtcp-xr:rcvr-rtt=all」のように属性ごと返す。
 */
export function videoRtcpAttributes(sdp: string): string[] {
    const kinds = new Set<string>();
    let inVideo = false;
    for (const line of sdp.split(/\r?\n/)) {
        if (line.startsWith("m=")) inVideo = line.startsWith("m=video");
        else if (inVideo && line.startsWith("a=rtcp-fb:")) {
            const space = line.indexOf(" ");
            if (space > 0) kinds.add(line.slice(space + 1));
        } else if (inVideo && line.startsWith("a=rtcp-xr:")) kinds.add(line.slice(2));
    }
    return [...kinds].sort();
}

/**
 * SDP から、送らない側が往復の時間を測る仕組み (RTCP XR の受信側の基準時刻の報告と、その答え) を求める行を除く。
 * 求める書き方は、標準の a=rtcp-xr:rcvr-rtt と、古い a=rtcp-fb:<番号> rrtr の 2 つ。受けるだけの側は、この仕組みが
 * ないと往復の時間を測れず、受けた映像に取り込みの時刻が付かない。行の区切りは元のまま (CRLF) にする。
 */
export function withoutNonSenderRtt(sdp: string): string {
    return sdp
        .split("\r\n")
        .filter((line) => !/^a=rtcp-fb:\S+ rrtr$/.test(line) && !/^a=rtcp-xr:rcvr-rtt/.test(line))
        .join("\r\n");
}

/** 音声の AudioData を読んだときの、読んだ時刻 - タイムスタンプ (マイクロ秒) の列。 */
export function lagSeriesUs(samples: ReadonlyArray<{ readMs: number; timestampUs: number }>): number[] {
    return samples.map((s) => Math.round(s.readMs * 1000) - s.timestampUs);
}
