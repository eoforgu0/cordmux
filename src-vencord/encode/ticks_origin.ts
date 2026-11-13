// SPDX-License-Identifier: Apache-2.0

// ページの原点 (performance.now() が 0 の時点) の、Chromium の base::TimeTicks の値 (以下「ページの原点の TimeTicks」) を、
// Vesktop のメインのプロセスの process.hrtime を performance.now() ではさんで求める (measureTicksOrigin)。この値があると、
// 受信した映像の VideoFrame.timestamp (TimeTicks の値) と、音声の AudioData.timestamp と performance.now() (どちらも
// ページの原点からの時間) を、見積もりなしに同じ時計の上に並べられる (encode/sender_clock.ts の SenderTimebase)。
//
// 時計の事実 (Chromium 156 と Node のソース):
// - Windows の base::TimeTicks は、QueryPerformanceCounter (QPC) を使う経路では「QPC x 1000000 / QPC の周波数」(us の整数)
//   である (Chromium の base/time/time_win.cc)。CPU の時刻のカウンターが一定の速さで進まない PC では、1 ms の分解能の別の
//   時計を使う。
// - performance.now() は「丸めた今の TimeTicks - 丸めたページの原点の TimeTicks」である。丸めは、ページが cross-origin
//   isolated でなければ 100 us、そうなら 5 us の格子に、乱数で決まる向きに寄せる (blink の core/timing/performance.cc と
//   time_clamper.cc)。1 つの値の丸めの誤差は格子の幅より小さい。
// - Node の process.hrtime は Windows では「QPC x 10^9 / QPC の周波数」(ns) である (libuv の uv_hrtime)。QPC の値は、
//   同じ PC のどのプロセスで読んでもそろう。
// したがって QPC の経路なら、ページの原点の TimeTicks (丸めた値) は、はさんだ組ごとに
// [hrtime - 後の読み - 丸めの幅, hrtime - 前の読み + 丸めの幅] にある (読みはどれも us)。全部の組の共通部分の真ん中を値にし、
// 共通部分の幅の半分 (丸めの幅を含む) を誤差の上限にする。メインのプロセスを呼んで返るまでの時間 (往復の時間) が最も短い
// 組で、共通部分の幅が決まる。
//
// 値を返さないとき: performance.now() の刻みが粗い (QPC の経路でない)、組どうしが食い違う (共通部分が空で、丸めでは説明
// できない)、誤差の上限が TICKS_ORIGIN_ERROR_MAX_US を超える。呼び出し元は、そのときは今までの換算 (encode/clock_map.ts)
// を使う。時計は deps から読む (このファイルは performance も IPC も直接は使わない)。

/** performance.now() を続けて読む長さ (ms)。刻みの細かさを確かめる。 */
export const TICKS_PROFILE_MS = 3;

/**
 * QPC の経路とみなす、1 ms あたりの performance.now() の異なる値の数の下限。QPC の経路なら丸めの格子の数 (100 us の格子で
 * 約 10)、1 ms の分解能の経路なら約 1 になる。その間の値で分ける。
 */
export const TICKS_DISTINCT_PER_MS_MIN = 5;

/** メインのプロセスの process.hrtime をはさむ回数。続けて呼び、往復の時間が短い組が 1 つでもあれば幅が狭くなる。 */
export const TICKS_ORIGIN_BRACKETS = 48;

/**
 * 誤差の上限の上限 (us)。これを超えたら値を返さない。映像の VideoFrame.timestamp は 1 ms の格子の値なので、映像の換算の
 * 誤差はもともと 0.5 ms ある。それと同じ大きさまでは、見積もりの換算 (誤差は取り出しの遅れで、ふつう数 ms) より小さい。
 */
export const TICKS_ORIGIN_ERROR_MAX_US = 1000;

/** process.hrtime と TimeTicks の丸めの違い (us)。どちらも同じ QPC の値を割って切り捨てるので、us では 1 までしか違わない。 */
const HRTIME_TICKS_ROUNDING_US = 1;

/** performance.now() の丸めの格子の幅 (us)。 */
export function pageClampUs(crossOriginIsolated: boolean): number {
    return crossOriginIsolated ? 5 : 100;
}

/** performance.now() でメインのプロセスの process.hrtime.bigint() をはさんだ組 1 つ。 */
export interface HrtimeBracket {
    /** 呼ぶ前の performance.now() (ms)。 */
    readonly beforeMs: number;
    /** process.hrtime.bigint() の値 (ns)。 */
    readonly hrtimeNs: bigint;
    /** 返った後の performance.now() (ms)。 */
    readonly afterMs: number;
}

/** performance.now() を続けて読んだ値の刻みの様子。 */
export interface TicksProfile {
    readonly reads: number;
    readonly spanMs: number;
    /** 1 ms あたりの異なる値の数。 */
    readonly distinctPerMs: number;
}

/** performance.now() を続けて読んだ値 (読んだ順) から、刻みの様子を出す。 */
export function ticksProfile(readsMs: readonly number[]): TicksProfile {
    let distinct = readsMs.length > 0 ? 1 : 0;
    for (let i = 1; i < readsMs.length; i++) if (readsMs[i] !== readsMs[i - 1]) distinct++;
    const spanMs = readsMs.length > 1 ? (readsMs[readsMs.length - 1] as number) - (readsMs[0] as number) : 0;
    return { reads: readsMs.length, spanMs, distinctPerMs: spanMs > 0 ? distinct / spanMs : 0 };
}

/** はさんだ組から決めた、ページの原点の TimeTicks。 */
export interface BracketOrigin {
    readonly count: number;
    /** 共通部分の真ん中 (us)。 */
    readonly originUs: number;
    /** 誤差の上限 (us)。共通部分の幅の半分 + 丸めの幅 + process.hrtime と TimeTicks の丸めの違い。 */
    readonly errorUs: number;
    /** 組ごとの範囲の下端の最大と上端の最小 (us。丸めの幅を足す前)。下端 > 上端のこともある (丸めの範囲の内)。 */
    readonly loUs: number;
    readonly hiUs: number;
    /** 往復の時間 (us) の最小と中央値。 */
    readonly minRttUs: number;
    readonly medianRttUs: number;
    /** 共通部分が丸めでは説明できないほど食い違っているか (下端 - 上端 >= 丸めの幅 x 2)。 */
    readonly inconsistent: boolean;
}

/** はさんだ組から、ページの原点の TimeTicks を求める。組がなければ undefined。 */
export function originFromBrackets(brackets: readonly HrtimeBracket[], clampUs: number): BracketOrigin | undefined {
    if (brackets.length === 0) return undefined;
    let lo = -Infinity;
    let hi = Infinity;
    const rtts: number[] = [];
    for (const b of brackets) {
        const hrUs = Number(b.hrtimeNs / 1000n);
        const sampleLo = hrUs - Math.round(b.afterMs * 1000);
        const sampleHi = hrUs - Math.round(b.beforeMs * 1000);
        if (sampleLo > lo) lo = sampleLo;
        if (sampleHi < hi) hi = sampleHi;
        rtts.push(sampleHi - sampleLo);
    }
    rtts.sort((a, b) => a - b);
    return {
        count: brackets.length,
        originUs: Math.round((lo + hi) / 2),
        errorUs: Math.ceil((hi - lo) / 2) + clampUs + HRTIME_TICKS_ROUNDING_US,
        loUs: lo,
        hiUs: hi,
        minRttUs: rtts[0] as number,
        medianRttUs: rtts[Math.floor((rtts.length - 1) / 2)] as number,
        inconsistent: lo - hi >= 2 * clampUs,
    };
}

/** 求めた結果。ok が false のときは、今までの換算を使う。 */
export type TicksOriginResult =
    | {
          readonly ok: true;
          /** ページの原点の TimeTicks (us)。 */
          readonly originUs: bigint;
          readonly errorUs: number;
          readonly brackets: BracketOrigin;
          readonly ticks: TicksProfile;
          readonly clampUs: number;
      }
    | {
          readonly ok: false;
          /**
           * coarse-ticks: performance.now() の刻みが粗い (QPC の経路でない)。inconsistent: 組どうしが食い違う。
           * too-wide: 誤差の上限が TICKS_ORIGIN_ERROR_MAX_US を超えた。call-failed: メインのプロセスを呼べなかった。
           */
          readonly reason: "coarse-ticks" | "inconsistent" | "too-wide" | "call-failed";
          readonly brackets: BracketOrigin | undefined;
          readonly ticks: TicksProfile | undefined;
          readonly clampUs: number;
          readonly detail?: string;
      };

/** measureTicksOrigin が読む時計と、メインのプロセスの呼び出し。 */
export interface TicksOriginDeps {
    /** ページの今の時刻 (performance.now()。ms)。 */
    nowMs(): number;
    /** メインのプロセスの process.hrtime.bigint() (ns) を 10 進の文字列で返す (native.ts の hrtimeNs)。 */
    readHrtimeNs(): Promise<string>;
    /** self.crossOriginIsolated。丸めの幅を決める。 */
    readonly crossOriginIsolated: boolean;
}

/** performance.now() を TICKS_PROFILE_MS の間続けて読み、刻みの様子を出す。この間はスレッドを使い続ける。 */
export function sampleTicks(nowMs: () => number): TicksProfile {
    const reads: number[] = [];
    const end = nowMs() + TICKS_PROFILE_MS;
    for (;;) {
        const v = nowMs();
        reads.push(v);
        if (v >= end || reads.length >= 1_000_000) break;
    }
    return ticksProfile(reads);
}

/**
 * ページの原点の TimeTicks を求める。先に performance.now() の刻みを確かめ (粗ければ呼ばずに返す)、メインのプロセスを
 * count 回続けて呼んで、はさんだ組から決める。reject しない (呼び出しの失敗は call-failed で返す)。
 */
export async function measureTicksOrigin(deps: TicksOriginDeps, count = TICKS_ORIGIN_BRACKETS): Promise<TicksOriginResult> {
    const clampUs = pageClampUs(deps.crossOriginIsolated);
    const ticks = sampleTicks(() => deps.nowMs());
    if (ticks.distinctPerMs < TICKS_DISTINCT_PER_MS_MIN) {
        return { ok: false, reason: "coarse-ticks", brackets: undefined, ticks, clampUs };
    }
    const brackets: HrtimeBracket[] = [];
    try {
        for (let i = 0; i < count; i++) {
            const beforeMs = deps.nowMs();
            const text = await deps.readHrtimeNs();
            const afterMs = deps.nowMs();
            brackets.push({ beforeMs, hrtimeNs: BigInt(text), afterMs });
        }
    } catch (e) {
        return { ok: false, reason: "call-failed", brackets: originFromBrackets(brackets, clampUs), ticks, clampUs, detail: String(e) };
    }
    const origin = originFromBrackets(brackets, clampUs) as BracketOrigin;
    if (origin.inconsistent) return { ok: false, reason: "inconsistent", brackets: origin, ticks, clampUs };
    if (origin.errorUs > TICKS_ORIGIN_ERROR_MAX_US) return { ok: false, reason: "too-wide", brackets: origin, ticks, clampUs };
    return { ok: true, originUs: BigInt(origin.originUs), errorUs: origin.errorUs, brackets: origin, ticks, clampUs };
}

/** 結果を、ログの 1 行 (英語) にする。 */
export function ticksOriginLogLine(result: TicksOriginResult): string {
    const b = result.brackets;
    const bracketText =
        b === undefined
            ? "brackets=0"
            : `brackets=${b.count} rtt_min_us=${b.minRttUs} rtt_p50_us=${b.medianRttUs} width_us=${b.hiUs - b.loUs}`;
    const ticksText = result.ticks === undefined ? "" : ` ticks_per_ms=${result.ticks.distinctPerMs.toFixed(1)}`;
    const head = result.ok
        ? `clock: base=qpc ticks_origin_us=${result.originUs} error_us=${result.errorUs}`
        : `clock: base=page reason=${result.reason}`;
    const detail = !result.ok && result.detail !== undefined ? ` (${result.detail})` : "";
    return `${head} ${bracketText}${ticksText} clamp_us=${result.clampUs}${detail}`;
}
