// SPDX-License-Identifier: Apache-2.0

// core/watch_release.ts の createWatchRelease() のテスト。接続が切れたまま時間がたつと 1 回だけ呼ばれること、途中で
// 再接続すると呼ばれないこと、core の状態に渡すと視聴をやめ、範囲が届くと視聴を再開することを確認する。

import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

import { initialState, reduce, type CoreState } from "../core/state";
import { createWatchRelease } from "../core/watch_release";

describe("createWatchRelease", () => {
    beforeEach(() => {
        vi.useFakeTimers();
    });
    afterEach(() => {
        vi.useRealTimers();
    });

    it("切れたまま時間がたつと、1 回だけ呼ばれる", () => {
        let calls = 0;
        const watch = createWatchRelease(() => void calls++, 30000);
        watch.closed();
        vi.advanceTimersByTime(29999);
        expect(calls).toBe(0);
        vi.advanceTimersByTime(1);
        expect(calls).toBe(1);
        vi.advanceTimersByTime(60000);
        expect(calls).toBe(1);
    });

    it("途中で接続し直すと、呼ばれない", () => {
        let calls = 0;
        const watch = createWatchRelease(() => void calls++, 30000);
        watch.closed();
        vi.advanceTimersByTime(10000);
        watch.cancel();
        vi.advanceTimersByTime(60000);
        expect(calls).toBe(0);
    });

    it("もう一度切れると、切れた時刻から数え直す", () => {
        let calls = 0;
        const watch = createWatchRelease(() => void calls++, 30000);
        watch.closed();
        vi.advanceTimersByTime(20000);
        watch.cancel();
        watch.closed();
        vi.advanceTimersByTime(29999);
        expect(calls).toBe(0);
        vi.advanceTimersByTime(1);
        expect(calls).toBe(1);
    });

    it("呼ばれたら視聴をやめ、接続し直して範囲が届くと視聴を始め直す", () => {
        let state: CoreState = reduce(initialState, {
            type: "sharing",
            userId: "alice",
            streamKey: "key-alice",
            pcId: 2,
            active: true,
        });
        state = reduce(state, { type: "selection", userIds: ["alice"] });
        state = reduce(state, { type: "watch_scope", userIds: ["alice"] });
        expect(state.watching).toEqual({ alice: "key-alice" });

        const watch = createWatchRelease(() => {
            state = reduce(state, { type: "watch_scope", userIds: [] });
        }, 30000);
        watch.closed();
        vi.advanceTimersByTime(30000);
        expect(state.watching).toEqual({});
        expect(state.pendingDecisions).toEqual([{ type: "stop-watching", userId: "alice", streamKey: "key-alice" }]);

        state = reduce(state, { type: "watch_scope", userIds: ["alice"] });
        expect(state.watching).toEqual({ alice: "key-alice" });
        expect(state.pendingDecisions).toEqual([{ type: "start-watching", userId: "alice", streamKey: "key-alice" }]);
    });
});
