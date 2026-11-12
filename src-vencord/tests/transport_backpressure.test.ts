// SPDX-License-Identifier: Apache-2.0

// transport/backpressure.ts のテスト。映像は bufferedAmount がしきい値を超えたら捨てて数え、次のキーフレームまで
// 捨て続けることと、音声は上限 (DEFAULT_AUDIO_CAP_BYTES) を超えたときだけ捨てて数えることを確認する。

import { describe, expect, it } from "vitest";

import {
    applyBackpressure,
    createBackpressureState,
    DEFAULT_AUDIO_CAP_BYTES,
    DEFAULT_BACKPRESSURE_THRESHOLD_BYTES,
} from "../transport/backpressure";

describe("applyBackpressure: 映像", () => {
    it("しきい値未満なら送る", () => {
        const state = createBackpressureState();
        const result = applyBackpressure(state, "video", 10, 100);
        expect(result.send).toBe(true);
        expect(result.state.droppedVideoFrames).toBe(0);
    });

    it("しきい値と等しいときは、超えていないので送る", () => {
        const state = createBackpressureState();
        const result = applyBackpressure(state, "video", 100, 100);
        expect(result.send).toBe(true);
        expect(result.state.droppedVideoFrames).toBe(0);
    });

    it("しきい値を 1 でも超えたら捨て、数を 1 増やす", () => {
        const state = createBackpressureState();
        const result = applyBackpressure(state, "video", 101, 100);
        expect(result.send).toBe(false);
        expect(result.state.droppedVideoFrames).toBe(1);
    });

    it("捨てた数は、状態を渡すたびに増えていく (キーフレームが届くまでは、しきい値を下回っても捨て続ける)", () => {
        let state = createBackpressureState();
        state = applyBackpressure(state, "video", 200, 100).state;
        state = applyBackpressure(state, "video", 50, 100).state; // 下回ったが、まだ P フレームなので捨て続ける
        state = applyBackpressure(state, "video", 300, 100).state;
        expect(state.droppedVideoFrames).toBe(3);
    });

    it("既定のしきい値は正の数である", () => {
        expect(DEFAULT_BACKPRESSURE_THRESHOLD_BYTES).toBeGreaterThan(0);
    });

    it("しきい値を省略すると既定値が使われる", () => {
        const state = createBackpressureState();
        const overThreshold = DEFAULT_BACKPRESSURE_THRESHOLD_BYTES + 1;
        expect(applyBackpressure(state, "video", overThreshold).send).toBe(false);
    });
});

describe("applyBackpressure: キーフレームが届くまで捨て続ける", () => {
    it("しきい値を下回っても、キーフレームが届くまでは P フレームを送らない", () => {
        let state = createBackpressureState();
        state = applyBackpressure(state, "video", 200, 100).state; // 超えたので捨てる。
        const result = applyBackpressure(state, "video", 0, 100); // 大きく下回ったが P フレーム
        expect(result.send).toBe(false);
        expect(result.state.droppingUntilKeyframe).toBe(true);
    });

    it("しきい値以下に戻ってからキーフレームが届いたら送り、捨てるのをやめる", () => {
        let state = createBackpressureState();
        state = applyBackpressure(state, "video", 200, 100).state; // 超えたので捨てる。
        const keyframeResult = applyBackpressure(state, "video", 50, 100, true); // 下回ったときのキーフレーム
        expect(keyframeResult.send).toBe(true);
        expect(keyframeResult.state.droppingUntilKeyframe).toBe(false);

        // やめた後は、通常の判定に戻る。
        const afterResult = applyBackpressure(keyframeResult.state, "video", 0, 100);
        expect(afterResult.send).toBe(true);
    });

    it("しきい値を超えている間は、キーフレームでも送らずに捨てる (たまったキューに大きいフレームを積まない)", () => {
        const state = createBackpressureState();
        const result = applyBackpressure(state, "video", 200, 100, true);
        expect(result.send).toBe(false);
        expect(result.state.droppedVideoFrames).toBe(1);
        expect(result.state.droppingUntilKeyframe).toBe(true);
    });

    it("しきい値と等しいときのキーフレームは、超えていないので送る", () => {
        const state = createBackpressureState();
        const result = applyBackpressure(state, "video", 100, 100, true);
        expect(result.send).toBe(true);
        expect(result.state.droppingUntilKeyframe).toBe(false);
    });

    it("初期状態では droppingUntilKeyframe が false", () => {
        expect(createBackpressureState().droppingUntilKeyframe).toBe(false);
    });
});

describe("applyBackpressure: 音声は上限までは捨てない", () => {
    it("bufferedAmount が映像のしきい値を超えていても、音声の上限以下なら音声は送る", () => {
        const state = createBackpressureState();
        const result = applyBackpressure(state, "audio", DEFAULT_AUDIO_CAP_BYTES, 1);
        expect(result.send).toBe(true);
        expect(result.state.droppedVideoFrames).toBe(0);
        expect(result.state.droppedAudioMessages).toBe(0);
    });

    it("bufferedAmount が音声の上限を超えたら、音声も捨てて数える (映像の数と、キーフレームを待つ状態は変えない)", () => {
        const state = { ...createBackpressureState(), droppedVideoFrames: 2, droppingUntilKeyframe: true };
        const result = applyBackpressure(state, "audio", DEFAULT_AUDIO_CAP_BYTES + 1);
        expect(result.send).toBe(false);
        expect(result.state.droppedAudioMessages).toBe(1);
        expect(result.state.droppedVideoFrames).toBe(2);
        expect(result.state.droppingUntilKeyframe).toBe(true);
    });

    it("音声の上限は引数で変えられる", () => {
        const state = createBackpressureState();
        expect(applyBackpressure(state, "audio", 11, 1, false, 10).send).toBe(false);
        expect(applyBackpressure(state, "audio", 10, 1, false, 10).send).toBe(true);
    });
});

describe("applyBackpressure: 映像を捨てても、音声を捨てた数は変えない", () => {
    it("映像を捨てたときも、送ったときも droppedAudioMessages はそのまま", () => {
        const state = { ...createBackpressureState(), droppedAudioMessages: 5 };
        expect(applyBackpressure(state, "video", 2, 1).state.droppedAudioMessages).toBe(5);
        expect(applyBackpressure(state, "video", 0, 1, true).state.droppedAudioMessages).toBe(5);
    });
});

describe("applyBackpressure: 不正な入力", () => {
    it("bufferedAmount が負の数なら例外を投げる", () => {
        expect(() => applyBackpressure(createBackpressureState(), "video", -1, 100)).toThrow(RangeError);
    });

    it("bufferedAmount が有限の数でない (Infinity か NaN) なら例外を投げる", () => {
        expect(() => applyBackpressure(createBackpressureState(), "video", Infinity, 100)).toThrow(RangeError);
        expect(() => applyBackpressure(createBackpressureState(), "video", NaN, 100)).toThrow(RangeError);
    });

    it("thresholdBytes が負の数なら例外を投げる", () => {
        expect(() => applyBackpressure(createBackpressureState(), "video", 0, -1)).toThrow(RangeError);
    });
});
