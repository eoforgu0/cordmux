// SPDX-License-Identifier: Apache-2.0

// encode/limits.ts の純粋関数のテスト。limits のキーは max_long_edge と fps_divisor で、どちらも省略できる (既定では
// 上限なしで、フレームレートもそのまま)。下げた上限はすぐに適用され、上げた上限は次の画面共有から反映される。

import { describe, expect, it } from "vitest";

import {
    assertValidLimits,
    DEFAULT_ENCODE_CHOICE,
    encodeChoiceFromWire,
    limitsFromWire,
    NO_LIMITS,
    sameLimits,
    stageLogLine,
    tightenLimits,
    type EncodeSessionLimits,
} from "../encode/limits";

describe("limitsFromWire (ワイヤー形式の形から変換する)", () => {
    it("キーをすべて省くと、上限なしでフレームレートもそのまま (既定の値と同じ)", () => {
        expect(limitsFromWire({})).toEqual({ fpsDivisor: 1 });
        expect(sameLimits(limitsFromWire({}), NO_LIMITS)).toBe(true);
    });

    it("max_long_edge だけ、fps_divisor だけ、両方のどれでも変換できる", () => {
        expect(limitsFromWire({ max_long_edge: 1280 })).toEqual({ maxLongEdge: 1280, fpsDivisor: 1 });
        expect(limitsFromWire({ fps_divisor: 2 })).toEqual({ fpsDivisor: 2 });
        expect(limitsFromWire({ max_long_edge: 640, fps_divisor: 2 })).toEqual({ maxLongEdge: 640, fpsDivisor: 2 });
    });

    it("変換した値は形のチェックを通る (RangeError にならない)", () => {
        for (const wire of [{}, { max_long_edge: 1 }, { fps_divisor: 2 as const }, { max_long_edge: 7680, fps_divisor: 1 as const }]) {
            expect(() => assertValidLimits(limitsFromWire(wire))).not.toThrow();
        }
    });
});

describe("tightenLimits (動いているセッションに、すぐに適用してよい上限)", () => {
    const none: EncodeSessionLimits = { fpsDivisor: 1 };
    const edge1280: EncodeSessionLimits = { maxLongEdge: 1280, fpsDivisor: 1 };
    const edge960: EncodeSessionLimits = { maxLongEdge: 960, fpsDivisor: 1 };
    const half: EncodeSessionLimits = { fpsDivisor: 2 };

    it("下げたキーはそのまま動作する", () => {
        expect(tightenLimits(none, edge1280)).toEqual(edge1280);
        expect(tightenLimits(edge1280, edge960)).toEqual(edge960);
        expect(tightenLimits(none, half)).toEqual(half);
    });

    it("上げたキーは動作しない (元のまま)", () => {
        expect(tightenLimits(edge960, edge1280)).toEqual(edge960);
        expect(tightenLimits(edge1280, none)).toEqual(edge1280);
        expect(tightenLimits(half, none)).toEqual(half);
    });

    it("下げたキーと上げたキーが混ざったメッセージでは、キーごとに厳しいほうを使う", () => {
        // 長い辺は下げ (なし -> 1280)、fps は上げる (1/2 -> 1/1)。
        expect(tightenLimits({ fpsDivisor: 2 }, { maxLongEdge: 1280, fpsDivisor: 1 })).toEqual({
            maxLongEdge: 1280,
            fpsDivisor: 2,
        });
        // 長い辺は上げ (960 -> なし)、fps は下げる (1/1 -> 1/2)。
        expect(tightenLimits(edge960, half)).toEqual({ maxLongEdge: 960, fpsDivisor: 2 });
    });

    it("同じ値なら同じ値を返す (sameLimits で、変わっていないと分かる)", () => {
        for (const limits of [none, edge1280, half, { maxLongEdge: 640, fpsDivisor: 2 as const }]) {
            expect(sameLimits(tightenLimits(limits, limits), limits)).toBe(true);
        }
    });
});

describe("sameLimits と assertValidLimits", () => {
    it("キーがあるかどうかまで比べる", () => {
        expect(sameLimits({ fpsDivisor: 1 }, { fpsDivisor: 1 })).toBe(true);
        expect(sameLimits({ fpsDivisor: 1 }, { maxLongEdge: 1280, fpsDivisor: 1 })).toBe(false);
        expect(sameLimits({ fpsDivisor: 1 }, { fpsDivisor: 2 })).toBe(false);
    });

    it("意味のない値には RangeError を投げる (呼び出し元の誤り)", () => {
        expect(() => assertValidLimits({ fpsDivisor: 3 as unknown as 1 })).toThrow(RangeError);
        expect(() => assertValidLimits({ maxLongEdge: 0, fpsDivisor: 1 })).toThrow(RangeError);
        expect(() => assertValidLimits({ maxLongEdge: Number.NaN, fpsDivisor: 1 })).toThrow(RangeError);
        expect(() => assertValidLimits({ maxLongEdge: -1, fpsDivisor: 1 })).toThrow(RangeError);
    });
});

describe("encodeChoiceFromWire (エンコードの設定)", () => {
    it("キーを省くと、ハードウェアを先に試して速さを優先する (既定の設定)", () => {
        expect(encodeChoiceFromWire({})).toEqual(DEFAULT_ENCODE_CHOICE);
        expect(DEFAULT_ENCODE_CHOICE).toEqual({ preference: "prefer-hardware", latencyMode: "realtime" });
    });

    it("encoder_preference と latency_mode が、session に渡す preference と latencyMode になる", () => {
        expect(encodeChoiceFromWire({ encoder_preference: "software" })).toEqual({
            preference: "prefer-software",
            latencyMode: "realtime",
        });
        expect(encodeChoiceFromWire({ latency_mode: "quality" })).toEqual({
            preference: "prefer-hardware",
            latencyMode: "quality",
        });
        expect(encodeChoiceFromWire({ encoder_preference: "software", latency_mode: "quality" })).toEqual({
            preference: "prefer-software",
            latencyMode: "quality",
        });
        expect(encodeChoiceFromWire({ encoder_preference: "hardware", latency_mode: "realtime" })).toEqual(
            DEFAULT_ENCODE_CHOICE,
        );
    });

    it("エンコードの設定は画質の上限に影響しない", () => {
        expect(limitsFromWire({ encoder_preference: "software", latency_mode: "quality" })).toEqual({ fpsDivisor: 1 });
    });
});

describe("stageLogLine (段階のログの行)", () => {
    it("段階と大きさに、好みと latencyMode を付ける", () => {
        const choice = encodeChoiceFromWire({ encoder_preference: "software", latency_mode: "quality" });
        expect(stageLogLine(7, 4, { width: 1280, height: 720, fps: 30 }, choice)).toBe(
            "video stream 7: stage=4 format=1280x720@30 preference=prefer-software latencyMode=quality",
        );
        expect(stageLogLine(7, 5, undefined, DEFAULT_ENCODE_CHOICE)).toBe(
            "video stream 7: stage=5 format=none preference=prefer-hardware latencyMode=realtime",
        );
    });
});
