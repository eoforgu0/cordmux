// SPDX-License-Identifier: Apache-2.0

// encode/clock_lag.ts の、受け取った時刻 - 換算した時刻 の集め方 (間引きを含む)、パーセンタイル、行の書式を確認する。

import { describe, expect, it } from "vitest";

import {
    addClockLag,
    CLOCK_LAG_MAX_SAMPLES,
    clockLagLine,
    createClockLagSamples,
    takeClockLagSummary,
} from "../encode/clock_lag";

describe("addClockLag と takeClockLagSummary", () => {
    it("何も足していなければ、数は 0 でパーセンタイルはない", () => {
        const s = takeClockLagSummary(createClockLagSamples());
        expect(s).toEqual({ count: 0, p5Us: undefined, p50Us: undefined, p95Us: undefined });
    });

    it("最も近い順位の値を返し、負の値もそのまま数える", () => {
        const samples = createClockLagSamples();
        // -10 から 89 までの 100 個を、順序を混ぜて足す。
        for (let i = 0; i < 100; i++) addClockLag(samples, ((i * 37) % 100) - 10);
        const s = takeClockLagSummary(samples);
        expect(s.count).toBe(100);
        expect(s.p5Us).toBe(-6); // 5 番目
        expect(s.p50Us).toBe(39); // 50 番目
        expect(s.p95Us).toBe(84); // 95 番目
    });

    it("値が 1 つなら、3 つのパーセンタイルはどれもその値", () => {
        const samples = createClockLagSamples();
        addClockLag(samples, -2_500);
        expect(takeClockLagSummary(samples)).toEqual({ count: 1, p5Us: -2_500, p50Us: -2_500, p95Us: -2_500 });
    });

    it("有限でない値は足さない", () => {
        const samples = createClockLagSamples();
        addClockLag(samples, Number.NaN);
        addClockLag(samples, Number.POSITIVE_INFINITY);
        addClockLag(samples, 1_000);
        expect(takeClockLagSummary(samples).count).toBe(1);
    });

    it("取り出すと空に戻り、取り出した値は後で足した値で変わらない", () => {
        const samples = createClockLagSamples();
        addClockLag(samples, 1_000);
        const first = takeClockLagSummary(samples);
        addClockLag(samples, 9_000);
        addClockLag(samples, 9_000);
        expect(first).toEqual({ count: 1, p5Us: 1_000, p50Us: 1_000, p95Us: 1_000 });
        expect(takeClockLagSummary(samples).p50Us).toBe(9_000);
        expect(takeClockLagSummary(samples).count).toBe(0);
    });

    it("上限を超えても持つ値の数は上限より少なく、期間の全体から同じ間隔で取った値のパーセンタイルになる", () => {
        const samples = createClockLagSamples();
        const n = 20_000; // 上限 (4096) の 4 倍を超える
        for (let i = 0; i < n; i++) addClockLag(samples, i);
        expect(samples.values.length).toBeLessThan(CLOCK_LAG_MAX_SAMPLES);
        expect(samples.stride).toBe(8);
        const s = takeClockLagSummary(samples);
        expect(s.count).toBe(n);
        // 間引いた分 (8 個に 1 個) だけ、本当のパーセンタイル (1000、10000、19000) からずれる。
        expect(Math.abs((s.p5Us as number) - 1_000)).toBeLessThanOrEqual(16);
        expect(Math.abs((s.p50Us as number) - 10_000)).toBeLessThanOrEqual(16);
        expect(Math.abs((s.p95Us as number) - 19_000)).toBeLessThanOrEqual(16);
        // 取り出した後は、間引かずに足し直す。
        expect(samples.stride).toBe(1);
        expect(samples.values).toHaveLength(0);
    });
});

describe("clockLagLine", () => {
    it("映像と音声のどちらも足していなければ行を書かない", () => {
        expect(clockLagLine(3, undefined, undefined)).toBeUndefined();
        const empty = takeClockLagSummary(createClockLagSamples());
        expect(clockLagLine(3, empty, empty)).toBeUndefined();
    });

    it("映像と音声のパーセンタイルを ms で 1 行に書き、ない種類は none と n=0 にする", () => {
        const video = createClockLagSamples();
        for (const v of [800, 2_100, 7_400]) addClockLag(video, v);
        const line = clockLagLine(3, takeClockLagSummary(video), undefined);
        expect(line).toBe(
            "clock lag stream 3: v.p5=+0.8ms v.p50=+2.1ms v.p95=+7.4ms v.n=3 a.p5=none a.p50=none a.p95=none a.n=0",
        );
    });

    it("負の値は - を付け、0 に丸まる値は +0.0ms にする", () => {
        const audio = createClockLagSamples();
        for (const v of [-1_260, -40, 150_000]) addClockLag(audio, v);
        expect(clockLagLine(5, undefined, takeClockLagSummary(audio))).toBe(
            "clock lag stream 5: v.p5=none v.p50=none v.p95=none v.n=0 a.p5=-1.3ms a.p50=+0.0ms a.p95=+150.0ms a.n=3",
        );
    });
});
