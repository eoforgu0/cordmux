// SPDX-License-Identifier: Apache-2.0

// transport/backoff.ts のテスト。切断した直後の 1 回はすぐに試し、その後は 2 秒から始めて、5 秒を上限に間隔を
// 延ばすことを確認する。

import { describe, expect, it } from "vitest";

import { BACKOFF_CAP_MS, BACKOFF_INITIAL_MS, reconnectDelayMs } from "../transport/backoff";

describe("reconnectDelayMs", () => {
    it("失敗が 0 回 (切断した直後の最初の再試行) なら、すぐに試す (0 ms)", () => {
        expect(reconnectDelayMs(0)).toBe(0);
    });

    it("1 回目の失敗の後は、2 秒から始まる", () => {
        expect(reconnectDelayMs(1)).toBe(BACKOFF_INITIAL_MS);
        expect(reconnectDelayMs(1)).toBe(2_000);
    });

    it("失敗が続くと、待ち時間は 5 秒で上限に達する", () => {
        expect(reconnectDelayMs(2)).toBe(4_000);
        expect(reconnectDelayMs(3)).toBe(BACKOFF_CAP_MS);
        expect(reconnectDelayMs(3)).toBe(5_000);
    });

    it("上限に達した後は、何度失敗しても 5 秒のまま", () => {
        expect(reconnectDelayMs(4)).toBe(5_000);
        expect(reconnectDelayMs(10)).toBe(5_000);
        expect(reconnectDelayMs(1000)).toBe(5_000);
    });

    it("失敗の回数が増えても、待ち時間は減らない", () => {
        let previous = -1;
        for (let n = 0; n <= 20; n++) {
            const delay = reconnectDelayMs(n);
            expect(delay).toBeGreaterThanOrEqual(previous);
            previous = delay;
        }
    });

    it("上限を超えない", () => {
        for (let n = 0; n <= 30; n++) {
            expect(reconnectDelayMs(n)).toBeLessThanOrEqual(BACKOFF_CAP_MS);
        }
    });

    it("負の値は例外を投げる", () => {
        expect(() => reconnectDelayMs(-1)).toThrow(RangeError);
    });

    it("整数でない値は例外を投げる", () => {
        expect(() => reconnectDelayMs(1.5)).toThrow(RangeError);
    });

    it("NaN は例外を投げる", () => {
        expect(() => reconnectDelayMs(NaN)).toThrow(RangeError);
    });
});
