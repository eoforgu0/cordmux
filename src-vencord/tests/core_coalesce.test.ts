// SPDX-License-Identifier: Apache-2.0

// core/coalesce.ts の coalesceInTick() のテスト。1 つの処理の中の複数回の呼び出しが 1 回になること、次の処理では
// また呼ばれること、roster と sharing の 2 つのイベントの後に作った facts が両方の新しい値を持つことを確認する。

import { describe, expect, it } from "vitest";

import { coalesceInTick } from "../core/coalesce";
import { buildFacts } from "../core/facts";
import { initialState, reduce, type CoreState } from "../core/state";
import type { LadderStage } from "../encode/ladder";

describe("coalesceInTick", () => {
    it("1 つの処理の中の複数回の呼び出しは、処理が終わった後の 1 回になる", async () => {
        let calls = 0;
        const run = coalesceInTick(() => void calls++);
        run();
        run();
        run();
        expect(calls).toBe(0);
        await Promise.resolve();
        expect(calls).toBe(1);
    });

    it("次の処理では、また呼ばれる", async () => {
        let calls = 0;
        const run = coalesceInTick(() => void calls++);
        run();
        await Promise.resolve();
        run();
        run();
        await Promise.resolve();
        expect(calls).toBe(2);
    });

    it("予約する関数を差し替えられ、予約は 1 つまでである", () => {
        const queue: (() => void)[] = [];
        let calls = 0;
        const run = coalesceInTick(
            () => void calls++,
            (task) => void queue.push(task),
        );
        run();
        run();
        expect(queue.length).toBe(1);
        queue.shift()!();
        expect(calls).toBe(1);
        run();
        expect(queue.length).toBe(1);
    });

    it("roster と sharing の 2 つのイベントの後に作った facts は、両方の新しい値を持つ", async () => {
        let state: CoreState = initialState;
        const sent: Record<string, unknown>[] = [];
        const send = coalesceInTick(() => {
            sent.push(buildFacts(state, new Map<number, LadderStage>(), { on: false, spsPpsPrepended: 0 }, { media: true, limits: {} }));
        });
        // 1 つの処理の中で、イベントごとに送る予約をする (index.ts の applyEvent と同じ)。
        state = reduce(state, { type: "roster", users: [{ userId: "u1" }] });
        send();
        state = reduce(state, { type: "sharing", userId: "u1", streamKey: "k1", active: true, pcId: 1 });
        send();
        expect(sent.length).toBe(0);
        await Promise.resolve();
        expect(sent.length).toBe(1);
        expect((sent[0]!.roster as { user_ids: string[] }).user_ids).toEqual(["u1"]);
        expect((sent[0]!.sharing as { user_ids: string[] }).user_ids).toEqual(["u1"]);
    });
});
