// SPDX-License-Identifier: Apache-2.0

// 送信側の時計 (senderClockUs)。ワイヤー形式の timestamp_us と、hello と pong の clock_us は、この時計の us で表す。
// performance.timeOrigin + performance.now() は Windows の単調な時計 (QueryPerformanceCounter) で進み、システム時刻を
// 合わせ直しても動かない。Date.now() は使わない。Chromium の Date.now() は、単調な時計で進めながら 60 秒ごとにシステム時刻に
// 合わせ直し、ms に丸めるので、NTP の合わせ直しやシステム時刻の速さの違いがタイムスタンプに入る。

/** performance のうち、この時計が使う部分。 */
export interface PerformanceLike {
    /** ページの時刻の原点 (ms。原点を決めたときのシステム時刻で、その後は変わらない)。 */
    readonly timeOrigin: number;
    /** 原点からの経過 (ms。単調な時計で進む)。 */
    now(): number;
}

/**
 * 送信側の時計の今 (us)。値はエポックに近い大きさから始まるが、システム時刻ではない (ページを開いた後のシステム時刻の
 * 変更には追従しない)。受信側は、OBS の時計との差を接続ごとに決めて換算する (docs/wire_format.md の「timestamp_us」)。
 */
export function senderClockUs(perf: PerformanceLike): bigint {
    return BigInt(Math.round(perf.timeOrigin * 1000)) + BigInt(Math.round(perf.now() * 1000));
}
