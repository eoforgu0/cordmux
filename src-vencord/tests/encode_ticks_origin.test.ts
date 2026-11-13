// SPDX-License-Identifier: Apache-2.0

// encode/ticks_origin.ts のテスト。ページの原点の TimeTicks を、process.hrtime を performance.now() ではさんだ組から求める。
// 時計は作り物で、真の時刻 (QPC の us) を進め、performance.now() は Chromium と同じく 100 us の格子に乱数の向きで丸めた
// 「今 - ページの原点」を返す。

import { describe, expect, it } from "vitest";

import {
    TICKS_ORIGIN_ERROR_MAX_US,
    measureTicksOrigin,
    originFromBrackets,
    ticksOriginLogLine,
    ticksProfile,
    type HrtimeBracket,
} from "../encode/ticks_origin";

/** 決まった種から作る乱数 (0 以上 1 未満)。 */
function rng(seed: number): () => number {
    let s = seed >>> 0;
    return () => {
        s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
        return s / 2 ** 32;
    };
}

/** 格子の幅 q に、値ごとに決まる向きで丸める (Chromium の TimeClamper と同じ形: 下の格子か上の格子)。 */
function clampTo(q: number, seed: number): (us: number) => number {
    return (us) => {
        const lower = Math.floor(us / q) * q;
        const threshold = rng(seed ^ lower)() * q;
        return us - lower >= threshold ? lower + q : lower;
    };
}

interface FakeClock {
    /** 真の時刻 (us)。 */
    t: number;
    nowMs(): number;
    readHrtimeNs(): Promise<string>;
}

/**
 * 真の時刻を、読むたびに stepUs だけ進める時計。nowMs は (丸めた今 - 丸めたページの原点) / 1000。readHrtimeNs は、往復の
 * 時間 rttUs() の半分の後の真の時刻を ns で返し、さらに半分進める。
 */
function fakeClock(originUs: number, startUs: number, rttUs: () => number, q = 100, stepUs = 1): FakeClock {
    const clamp = clampTo(q, 7);
    const clock: FakeClock = {
        t: startUs,
        nowMs() {
            const v = (clamp(clock.t) - clamp(originUs)) / 1000;
            clock.t += stepUs;
            return v;
        },
        async readHrtimeNs() {
            const rtt = rttUs();
            clock.t += rtt / 2;
            const ns = BigInt(Math.round(clock.t * 1000));
            clock.t += rtt / 2;
            return ns.toString();
        },
    };
    return clock;
}

describe("ticksProfile", () => {
    it("1 ms あたりの異なる値の数を数える", () => {
        const reads = [10, 10, 10.1, 10.1, 10.2, 10.3, 11];
        const p = ticksProfile(reads);
        expect(p.reads).toBe(7);
        expect(p.spanMs).toBeCloseTo(1, 9);
        expect(p.distinctPerMs).toBeCloseTo(5, 9);
    });

    it("値が 1 つなら 0", () => {
        expect(ticksProfile([5]).distinctPerMs).toBe(0);
    });
});

describe("originFromBrackets", () => {
    const origin = 264_017_591_900;

    it("真の値が誤差の上限の内にあり、共通部分は往復の時間の最小より狭い", () => {
        const r = rng(3);
        const clock = fakeClock(origin, origin + 5_000_000, () => 200 + Math.floor(r() * 2000));
        const brackets: HrtimeBracket[] = [];
        return (async () => {
            for (let i = 0; i < 48; i++) {
                const beforeMs = clock.nowMs();
                const ns = BigInt(await clock.readHrtimeNs());
                const afterMs = clock.nowMs();
                brackets.push({ beforeMs, hrtimeNs: ns, afterMs });
            }
            const o = originFromBrackets(brackets, 100);
            expect(o).toBeDefined();
            if (o === undefined) return;
            expect(o.inconsistent).toBe(false);
            expect(Math.abs(o.originUs - origin)).toBeLessThanOrEqual(o.errorUs);
            expect(o.hiUs - o.loUs).toBeLessThanOrEqual(o.minRttUs);
            expect(o.errorUs).toBeLessThanOrEqual(Math.ceil(o.minRttUs / 2) + 101);
        })();
    });

    it("2 つの時計が食い違う組 (原点が 1 ms 違う) を見つける", () => {
        const brackets: HrtimeBracket[] = [
            { beforeMs: 1000, hrtimeNs: BigInt(origin + 1_000_100) * 1000n, afterMs: 1000.2 },
            { beforeMs: 2000, hrtimeNs: BigInt(origin + 1000 + 2_000_100) * 1000n, afterMs: 2000.2 },
        ];
        const o = originFromBrackets(brackets, 100);
        expect(o?.inconsistent).toBe(true);
    });

    it("丸めの範囲の内の食い違い (共通部分が少し負) は食い違いにしない", () => {
        // 下端 = 1000 - 0 = 1000、上端 = 900 (2 組目)。差 100 は丸めの 2 つ分 (200) より小さい。
        const brackets: HrtimeBracket[] = [
            { beforeMs: 0, hrtimeNs: 1_000_000n, afterMs: 0 },
            { beforeMs: 0.1, hrtimeNs: 1_000_000n, afterMs: 0.1 },
        ];
        const o = originFromBrackets(brackets, 100);
        expect(o?.inconsistent).toBe(false);
        expect(o?.originUs).toBe(950);
        expect(o?.errorUs).toBe(-50 + 100 + 1);
    });

    it("組がなければ undefined", () => {
        expect(originFromBrackets([], 100)).toBeUndefined();
    });
});

describe("measureTicksOrigin", () => {
    const origin = 264_017_591_900;

    it("QPC の経路で、往復の時間が短い組があれば値を返す", async () => {
        const r = rng(11);
        const clock = fakeClock(origin, origin + 9_000_000, () => 150 + Math.floor(r() * 800));
        const result = await measureTicksOrigin({
            nowMs: () => clock.nowMs(),
            readHrtimeNs: () => clock.readHrtimeNs(),
            crossOriginIsolated: false,
        });
        expect(result.ok).toBe(true);
        if (!result.ok) return;
        expect(Math.abs(Number(result.originUs) - origin)).toBeLessThanOrEqual(result.errorUs);
        expect(result.errorUs).toBeLessThanOrEqual(TICKS_ORIGIN_ERROR_MAX_US);
        expect(result.ticks.distinctPerMs).toBeGreaterThan(5);
        expect(ticksOriginLogLine(result)).toMatch(/^clock: base=qpc ticks_origin_us=\d+ error_us=\d+ brackets=48 /);
    });

    it("performance.now() の刻みが 1 ms なら、呼ばずに coarse-ticks を返す", async () => {
        let calls = 0;
        const clock = fakeClock(origin, origin + 9_000_000, () => 200, 1000);
        const result = await measureTicksOrigin({
            nowMs: () => clock.nowMs(),
            readHrtimeNs: () => {
                calls++;
                return clock.readHrtimeNs();
            },
            crossOriginIsolated: false,
        });
        expect(result.ok).toBe(false);
        if (result.ok) return;
        expect(result.reason).toBe("coarse-ticks");
        expect(calls).toBe(0);
        expect(ticksOriginLogLine(result)).toMatch(/^clock: base=page reason=coarse-ticks brackets=0 ticks_per_ms=/);
    });

    it("往復の時間がどれも長ければ too-wide を返す", async () => {
        const clock = fakeClock(origin, origin + 9_000_000, () => 5_000);
        const result = await measureTicksOrigin({
            nowMs: () => clock.nowMs(),
            readHrtimeNs: () => clock.readHrtimeNs(),
            crossOriginIsolated: false,
        });
        expect(result.ok).toBe(false);
        if (!result.ok) expect(result.reason).toBe("too-wide");
    });

    it("呼び出しが失敗したら call-failed を返す (reject しない)", async () => {
        const clock = fakeClock(origin, origin + 9_000_000, () => 200);
        const result = await measureTicksOrigin({
            nowMs: () => clock.nowMs(),
            readHrtimeNs: () => Promise.reject(new Error("no handler")),
            crossOriginIsolated: false,
        });
        expect(result.ok).toBe(false);
        if (!result.ok) {
            expect(result.reason).toBe("call-failed");
            expect(ticksOriginLogLine(result)).toContain("(Error: no handler)");
        }
    });

    it("hrtime が別の時計 (原点が 2 ms ずつずれていく) なら inconsistent を返す", async () => {
        const clock = fakeClock(origin, origin + 9_000_000, () => 200);
        let drift = 0;
        const result = await measureTicksOrigin({
            nowMs: () => clock.nowMs(),
            readHrtimeNs: async () => {
                drift += 2_000;
                return (BigInt(await clock.readHrtimeNs()) + BigInt(drift) * 1000n).toString();
            },
            crossOriginIsolated: false,
        });
        expect(result.ok).toBe(false);
        if (!result.ok) expect(result.reason).toBe("inconsistent");
    });
});
