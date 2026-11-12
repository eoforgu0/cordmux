// SPDX-License-Identifier: Apache-2.0

// encode/clock_origin_log.ts のテスト。映像と音声の原点が決まったときと変わったときだけ、1 秒に 1 回まで、送信側の
// ログに 1 行書くことを確認する。

import { describe, expect, it } from "vitest";

import {
    CLOCK_ORIGIN_LOG_INTERVAL_MS,
    clockOriginLogLine,
    createClockOriginLogState,
    type ClockOriginLogState,
} from "../encode/clock_origin_log";

const T0_MS = 1_700_000_000_000;

describe("clockOriginLogLine: 行を書く条件", () => {
    it("映像も音声もまだ決まっていなければ書かない", () => {
        const state = createClockOriginLogState();
        const result = clockOriginLogLine(state, 1, undefined, undefined, T0_MS);
        expect(result.line).toBeUndefined();
        expect(result.state).toBe(state); // 状態も変わらない (同じ参照)。
    });

    it("最初に決まったとき (undefined から値になったとき) は 1 行書く", () => {
        const state = createClockOriginLogState();
        const result = clockOriginLogLine(state, 7, 1_000n, undefined, T0_MS);
        expect(result.line).toBe("clock origin stream 7: video=1000us audio=none diff=none");
        expect(result.state.lastVideoOriginUs).toBe(1_000n);
        expect(result.state.lastLoggedAtMs).toBe(T0_MS);
    });

    it("片方だけが決まっているストリームでは、もう片方を none として書く", () => {
        const state = createClockOriginLogState();
        const result = clockOriginLogLine(state, 3, undefined, 2_000n, T0_MS);
        expect(result.line).toBe("clock origin stream 3: video=none audio=2000us diff=none");
    });

    it("両方が決まっていれば、差 (video - audio) を書く", () => {
        const state = createClockOriginLogState();
        const result = clockOriginLogLine(state, 1, 5_000n, 4_500n, T0_MS);
        expect(result.line).toBe("clock origin stream 1: video=5000us audio=4500us diff=500us");
    });

    it("差が負のときも、符号を付けて書く", () => {
        const state = createClockOriginLogState();
        const result = clockOriginLogLine(state, 1, 4_500n, 5_000n, T0_MS);
        expect(result.line).toBe("clock origin stream 1: video=4500us audio=5000us diff=-500us");
    });

    it("値が変わらなければ、1 秒たっていても書かない", () => {
        const first = clockOriginLogLine(createClockOriginLogState(), 1, 1_000n, 1_000n, T0_MS);
        expect(first.line).toBeDefined();
        const second = clockOriginLogLine(first.state, 1, 1_000n, 1_000n, T0_MS + 5_000);
        expect(second.line).toBeUndefined();
        expect(second.state).toBe(first.state); // 変化がないので状態もそのまま。
    });

    it("前に書いてから 1 秒たたずに変わったら、書かない", () => {
        const first = clockOriginLogLine(createClockOriginLogState(), 1, 1_000n, undefined, T0_MS);
        const second = clockOriginLogLine(first.state, 1, 2_000n, undefined, T0_MS + 500);
        expect(second.line).toBeUndefined();
        // 書かない間は state を更新しない (次の呼び出しでも、1000 から変わったものとして扱う)。
        expect(second.state).toBe(first.state);
    });

    it("1 秒たたずに続けて変わったときは、1 秒たった後の呼び出しで、最新の値だけを 1 行にまとめて書く", () => {
        let state: ClockOriginLogState = createClockOriginLogState();
        let line: string | undefined;
        ({ state, line } = clockOriginLogLine(state, 1, 1_000n, undefined, T0_MS));
        expect(line).toBeDefined();
        // 1 秒たたずに 2 回変わるが、どちらも書かない。
        ({ state, line } = clockOriginLogLine(state, 1, 2_000n, undefined, T0_MS + 200));
        expect(line).toBeUndefined();
        ({ state, line } = clockOriginLogLine(state, 1, 3_000n, undefined, T0_MS + 400));
        expect(line).toBeUndefined();
        // ちょうど 1 秒たった呼び出しで、そのときに渡した最新の値 (4000) だけを 1 行書く。
        ({ state, line } = clockOriginLogLine(state, 1, 4_000n, undefined, T0_MS + CLOCK_ORIGIN_LOG_INTERVAL_MS));
        expect(line).toBe("clock origin stream 1: video=4000us audio=none diff=none");
    });

    it("ちょうど 1 秒たったときは書く (>= で比べる)", () => {
        const first = clockOriginLogLine(createClockOriginLogState(), 1, 1_000n, undefined, T0_MS);
        const second = clockOriginLogLine(first.state, 1, 2_000n, undefined, T0_MS + CLOCK_ORIGIN_LOG_INTERVAL_MS);
        expect(second.line).toBe("clock origin stream 1: video=2000us audio=none diff=none");
    });

    it("ストリームごとの streamId が行に入る", () => {
        const result = clockOriginLogLine(createClockOriginLogState(), 42, 1n, undefined, T0_MS);
        expect(result.line).toContain("stream 42");
    });

    it("ストリームごとに 1 秒の間隔を数える (別の streamId の状態は互いに影響しない)", () => {
        const stateA = clockOriginLogLine(createClockOriginLogState(), 1, 1_000n, undefined, T0_MS).state;
        // B は初めてのストリーム (別の初期状態) なので、A の直後でも書く。
        const resultB = clockOriginLogLine(createClockOriginLogState(), 2, 1_000n, undefined, T0_MS + 10);
        expect(resultB.line).toBe("clock origin stream 2: video=1000us audio=none diff=none");
        expect(stateA.lastVideoOriginUs).toBe(1_000n); // A の状態は、B の呼び出しで変わらない。
    });
});
