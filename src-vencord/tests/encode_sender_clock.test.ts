// SPDX-License-Identifier: Apache-2.0

// encode/sender_clock.ts のテスト。送信側の時計は performance.timeOrigin + performance.now() で、システム時刻
// (Date.now) が飛んでも、速さが違っても動かない。

import { afterEach, describe, expect, it, vi } from "vitest";

import { senderClockUs } from "../encode/sender_clock";

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
