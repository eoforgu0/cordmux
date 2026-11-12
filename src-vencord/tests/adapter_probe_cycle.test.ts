// SPDX-License-Identifier: Apache-2.0

// adapter/probe_cycle.ts のテスト。依存箇所の周期的な確認で、送る contact のイベントと次の待ち方を決める。Discord に
// ログインする前は、見つからなかった依存箇所を missing として送らない。

import { describe, expect, it } from "vitest";

import { planProbeCycle, planSelfEmit, shouldEmitRecheck } from "../adapter/probe_cycle";

describe("planProbeCycle: ログインした後", () => {
    it("全部見つかれば found だけを送り、長い間隔で確認続け、探し直しの回数を 0 に戻す", () => {
        const plan = planProbeCycle([["sharing", true], ["self", true]], false, 3, 24);
        expect(plan.emit).toEqual([
            { id: "sharing", result: "found" },
            { id: "self", result: "found" },
        ]);
        expect(plan.next).toBe("watchdog");
        expect(plan.retries).toBe(0);
    });

    it("見つからない物があれば missing も送り、回数を 1 つ進めて探し直す", () => {
        const plan = planProbeCycle([["sharing", false], ["self", true]], false, 3, 24);
        expect(plan.emit).toEqual([
            { id: "sharing", result: "missing" },
            { id: "self", result: "found" },
        ]);
        expect(plan.next).toBe("rediscover");
        expect(plan.retries).toBe(4);
    });

    it("探し直しの回数が上限に達していれば、missing を送って探し直しを止める", () => {
        const plan = planProbeCycle([["roster", false]], false, 24, 24);
        expect(plan.emit).toEqual([{ id: "roster", result: "missing" }]);
        expect(plan.next).toBe("stop");
        expect(plan.retries).toBe(24);
    });
});

describe("planProbeCycle: Discord にログインする前", () => {
    it("見つからなかった物は送らず、見つかった物だけを送る", () => {
        const plan = planProbeCycle([["sharing", false], ["roster", false], ["self", true]], true, 0, 24);
        expect(plan.emit).toEqual([{ id: "self", result: "found" }]);
    });

    it("回数を数えずに探し直し続ける (上限に達していても止めない)", () => {
        expect(planProbeCycle([["roster", false]], true, 5, 24)).toMatchObject({ next: "rediscover", retries: 5 });
        expect(planProbeCycle([["roster", false]], true, 24, 24)).toMatchObject({ next: "rediscover", retries: 24 });
    });

    it("全部見つかれば、ログインした後と同じく長い間隔に移る", () => {
        const plan = planProbeCycle([["sharing", true]], true, 7, 24);
        expect(plan.next).toBe("watchdog");
        expect(plan.retries).toBe(0);
    });
});

describe("shouldEmitRecheck", () => {
    it("見つかれば送り、見つからないときはログインした後だけ送る", () => {
        expect(shouldEmitRecheck(true, true)).toBe(true);
        expect(shouldEmitRecheck(true, false)).toBe(true);
        expect(shouldEmitRecheck(false, false)).toBe(true);
        expect(shouldEmitRecheck(false, true)).toBe(false);
    });
});

describe("planSelfEmit: 自分のアカウントを送るか", () => {
    it("まだ 1 度も送っていなければ、ログインの前 (空) でも送る", () => {
        expect(planSelfEmit({ ok: true, userId: null }, undefined)).toBe("send");
        expect(planSelfEmit({ ok: true, userId: "me" }, undefined)).toBe("send");
    });

    it("ログインの前に空で送った後、ログインして読めたら送り直す", () => {
        expect(planSelfEmit({ ok: true, userId: "me" }, null)).toBe("send");
    });

    it("同じ値なら送らない (周期的な確認と Flux のイベントのたびに送らない)", () => {
        expect(planSelfEmit({ ok: true, userId: "me" }, "me")).toBe("none");
        expect(planSelfEmit({ ok: true, userId: null }, null)).toBe("none");
    });

    it("アカウントが替わったら送り直す", () => {
        expect(planSelfEmit({ ok: true, userId: "other" }, "me")).toBe("send");
    });

    it("ストアが読めなければ送らずに再確認する (前に送った値は変えない)", () => {
        expect(planSelfEmit({ ok: false }, undefined)).toBe("recheck");
        expect(planSelfEmit({ ok: false }, "me")).toBe("recheck");
    });
});
