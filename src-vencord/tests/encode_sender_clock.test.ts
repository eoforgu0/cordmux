// SPDX-License-Identifier: Apache-2.0

// encode/sender_clock.ts のテスト。送信側の時計は「ページの原点の値 + performance.now()」で、システム時刻 (Date.now) が
// 飛んでも、速さが違っても動かない。原点の値は performance.timeOrigin (page) か、ページの原点の TimeTicks (qpc)。

import { afterEach, describe, expect, it, vi } from "vitest";

import {
    fromPageUs,
    fromTicksUs,
    pageTimebase,
    qpcTimebase,
    senderClockUs,
    timebaseNowUs,
} from "../encode/sender_clock";

describe("senderClockUs", () => {
    afterEach(() => {
        vi.restoreAllMocks();
    });

    it("performance.timeOrigin と performance.now() の和を us にする", () => {
        expect(senderClockUs({ timeOrigin: 1_700_000_000_000.25, now: () => 1_234.5 })).toBe(1_700_000_001_234_750n);
    });

    it("システム時刻 (Date.now) が 1 秒飛んでも、+-10 ppm で進んでも、値は変わらない", () => {
        const perf = { timeOrigin: 1_700_000_000_000, now: () => 60_000 };
        vi.spyOn(Date, "now").mockReturnValue(1_700_000_060_000);
        const before = senderClockUs(perf);
        vi.spyOn(Date, "now").mockReturnValue(1_700_000_061_000 + 0.6);
        expect(senderClockUs(perf)).toBe(before);
    });

    it("performance.now() が進んだ分だけ進む", () => {
        let nowMs = 10;
        const perf = { timeOrigin: 1_000, now: () => nowMs };
        const a = senderClockUs(perf);
        nowMs += 16.667;
        expect(senderClockUs(perf) - a).toBe(16_667n);
    });
});

describe("SenderTimebase", () => {
    it("page: 原点は performance.timeOrigin で、senderClockUs と同じ値になる", () => {
        const perf = { timeOrigin: 1_700_000_000_000.25, now: () => 1_234.5 };
        const tb = pageTimebase(perf);
        expect(tb).toEqual({ base: "page", pageOriginUs: 1_700_000_000_000_250n });
        expect(timebaseNowUs(tb, perf.now())).toBe(senderClockUs(perf));
        expect(fromPageUs(tb, 1_000n)).toBe(1_700_000_000_001_250n);
        expect(fromTicksUs(tb, 264_017_591_900n)).toBeUndefined();
    });

    it("qpc: 原点はページの原点の TimeTicks で、TimeTicks の値はそのまま送信側の時計になる", () => {
        const tb = qpcTimebase(264_017_591_900n);
        expect(tb).toEqual({ base: "qpc", pageOriginUs: 264_017_591_900n });
        expect(timebaseNowUs(tb, 60_000.1)).toBe(264_077_592_000n);
        expect(fromPageUs(tb, 60_000_100n)).toBe(264_077_592_000n);
        expect(fromTicksUs(tb, 264_077_592_000n)).toBe(264_077_592_000n);
    });
});
