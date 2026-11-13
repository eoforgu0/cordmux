// SPDX-License-Identifier: Apache-2.0

// 送信側の時計 (senderClockUs と SenderTimebase)。ワイヤー形式の timestamp_us と、hello と pong の clock_us は、この時計の
// us で表す。送信側の時計は「ページの原点からの時間 (performance.now() の us) + ページの原点の送信側の時計の値」で、原点の
// 値の決め方が 2 つある (SenderTimebase)。
// - qpc: ページの原点の Chromium の TimeTicks の値 (encode/ticks_origin.ts で求める)。送信側の時計は TimeTicks そのもので、
//   Windows では QueryPerformanceCounter の us になり、OBS の時計 (os_gettime_ns / 1000) と同じ値である。受けた映像の
//   VideoFrame.timestamp (TimeTicks の値) は、そのまま送信側の時計の値になる。
// - page: performance.timeOrigin。ページの原点の TimeTicks を求められなかったとき。原点の値はページを作ったときの
//   システム時刻で決まり、TimeTicks との差は分からない。
// どちらも単調な時計で進み、システム時刻を合わせ直しても動かない。Date.now() は使わない。Chromium の Date.now() は、単調な
// 時計で進めながら 60 秒ごとにシステム時刻に合わせ直し、ms に丸めるので、NTP の合わせ直しやシステム時刻の速さの違いが
// タイムスタンプに入る。

/** performance のうち、この時計が使う部分。 */
export interface PerformanceLike {
    /** ページの時刻の原点 (ms。原点を決めたときのシステム時刻で、その後は変わらない)。 */
    readonly timeOrigin: number;
    /** 原点からの経過 (ms。単調な時計で進む)。 */
    now(): number;
}

/**
 * page の時計の今 (us)。値はエポックに近い大きさから始まるが、システム時刻ではない (ページを開いた後のシステム時刻の
 * 変更には追従しない)。受信側は、OBS の時計との差を接続ごとに決めて換算する (docs/wire_format.md の「timestamp_us」)。
 */
export function senderClockUs(perf: PerformanceLike): bigint {
    return BigInt(Math.round(perf.timeOrigin * 1000)) + BigInt(Math.round(perf.now() * 1000));
}

/** 送信側の時計の種類。qpc のときだけ、hello の clock_base で受信側に伝える (docs/wire_format.md の「hello の扱い」)。 */
export type SenderClockBase = "qpc" | "page";

/** 送信側の時計の決め方。プラグインを起動したときに 1 回決め、プラグインが動いている間は変えない。 */
export interface SenderTimebase {
    readonly base: SenderClockBase;
    /**
     * ページの原点 (performance.now() が 0 の時点) の、送信側の時計の値 (us)。qpc ではページの原点の TimeTicks、page では
     * performance.timeOrigin。
     */
    readonly pageOriginUs: bigint;
}

/** page の時計。performance.timeOrigin を原点にする (senderClockUs と同じ値になる)。 */
export function pageTimebase(perf: Pick<PerformanceLike, "timeOrigin">): SenderTimebase {
    return { base: "page", pageOriginUs: BigInt(Math.round(perf.timeOrigin * 1000)) };
}

/** qpc の時計。ticksOriginUs はページの原点の TimeTicks (encode/ticks_origin.ts の measureTicksOrigin)。 */
export function qpcTimebase(ticksOriginUs: bigint): SenderTimebase {
    return { base: "qpc", pageOriginUs: ticksOriginUs };
}

/** 送信側の時計の今 (us)。nowMs は performance.now()。 */
export function timebaseNowUs(timebase: SenderTimebase, nowMs: number): bigint {
    return timebase.pageOriginUs + BigInt(Math.round(nowMs * 1000));
}

/** ページの原点からの時間 (us。performance.now() と AudioData.timestamp と同じ基準) を、送信側の時計にする。 */
export function fromPageUs(timebase: SenderTimebase, pageUs: bigint): bigint {
    return timebase.pageOriginUs + pageUs;
}

/**
 * Chromium の TimeTicks の値 (us。受けた映像の VideoFrame.timestamp と同じ基準) を、送信側の時計にする。qpc では
 * TimeTicks がそのまま送信側の時計なので、そのまま返す。page では TimeTicks との差が分からないので undefined。
 */
export function fromTicksUs(timebase: SenderTimebase, ticksUs: bigint): bigint | undefined {
    return timebase.base === "qpc" ? ticksUs : undefined;
}
