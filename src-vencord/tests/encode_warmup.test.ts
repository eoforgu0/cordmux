// SPDX-License-Identifier: Apache-2.0

// encode/warmup.ts のテスト。エンコーダのウォームアップ (起動した直後に送って捨てるフレーム) の数と進み方を確認する。

import { describe, expect, it } from "vitest";

import {
    consumeWarmupFrame,
    createWarmupPlan,
    DEFAULT_WARMUP_FRAME_COUNT,
    isWarming,
    startWarmup,
} from "../encode/warmup";

describe("createWarmupPlan", () => {
    it("既定の値は 60 フレーム", () => {
        expect(DEFAULT_WARMUP_FRAME_COUNT).toBe(60);
        expect(createWarmupPlan()).toEqual({ totalFrames: 60 });
    });

    it("totalFrames を指定できる (0 なら、ウォームアップしない)", () => {
        expect(createWarmupPlan(0)).toEqual({ totalFrames: 0 });
        expect(createWarmupPlan(5)).toEqual({ totalFrames: 5 });
    });

    it("負の値は例外を投げる", () => {
        expect(() => createWarmupPlan(-1)).toThrow(RangeError);
    });

    it("整数でない値は例外を投げる", () => {
        expect(() => createWarmupPlan(1.5)).toThrow(RangeError);
    });
});

describe("ウォームアップの進み方", () => {
    it("startWarmup は、remaining が totalFrames の状態を作る", () => {
        const plan = createWarmupPlan(2);
        expect(startWarmup(plan)).toEqual({ remaining: 2 });
    });

    it("totalFrames が 0 なら、始めたときから isWarming は false", () => {
        const state = startWarmup(createWarmupPlan(0));
        expect(isWarming(state)).toBe(false);
    });

    it("1 フレームずつ減らし、remaining が 0 になった回だけ justCompleted が true になる", () => {
        let state = startWarmup(createWarmupPlan(2));
        expect(isWarming(state)).toBe(true);

        const step1 = consumeWarmupFrame(state);
        expect(step1).toEqual({ state: { remaining: 1 }, discardOutput: true, justCompleted: false });
        state = step1.state;
        expect(isWarming(state)).toBe(true);

        const step2 = consumeWarmupFrame(state);
        expect(step2).toEqual({ state: { remaining: 0 }, discardOutput: true, justCompleted: true });
        state = step2.state;
        expect(isWarming(state)).toBe(false);
    });

    it("既に終わった状態で呼び出しても状態を変えず、discardOutput は false になる", () => {
        const finished = { remaining: 0 };
        const step = consumeWarmupFrame(finished);
        expect(step).toEqual({ state: finished, discardOutput: false, justCompleted: false });
    });
});
