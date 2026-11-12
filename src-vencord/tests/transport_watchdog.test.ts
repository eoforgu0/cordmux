// SPDX-License-Identifier: Apache-2.0

// transport/watchdog.ts のテスト。ping が来ないまま過ぎた時間と、送る待ちが音の上限を超えたまま過ぎた時間を数え、
// どちらかが限度に達したら切ると決めることを確認する。

import { describe, expect, it } from "vitest";

import {
    createWatchdogState,
    DEFAULT_WATCHDOG_CONFIG,
    stepWatchdog,
    type WatchdogDecision,
    type WatchdogState,
} from "../transport/watchdog";

/** observed を順に与えて、最後の判断を返す。 */
function run(steps: { pingSeen: boolean; overCap: boolean }[], config = DEFAULT_WATCHDOG_CONFIG) {
    let state: WatchdogState = createWatchdogState();
    let last: WatchdogDecision = { state, reason: null };
    for (const observed of steps) {
        last = stepWatchdog(state, observed, config);
        state = last.state;
    }
    return last;
}

const silent = { pingSeen: false, overCap: false };
const alive = { pingSeen: true, overCap: false };
const aliveOverCap = { pingSeen: true, overCap: true };

describe("stepWatchdog: ping", () => {
    it("ping が来ないまま pingSilenceMs に届いた時点で切る (既定は 5 秒ごとに 4 回で 20 秒)", () => {
        expect(run([silent, silent, silent])).toMatchObject({ reason: null });
        expect(run([silent, silent, silent, silent])).toEqual({
            state: { silentMs: 20000, overCapMs: 0 },
            reason: "ping-silence",
        });
    });

    it("ping が届いた時点で数え直す", () => {
        expect(run([silent, silent, silent, alive, silent, silent, silent])).toMatchObject({
            state: { silentMs: 15000 },
            reason: null,
        });
    });
});

describe("stepWatchdog: 送る待ち", () => {
    it("上限を超えたまま overCapMs に届いた時点で切る (既定は 15 秒)", () => {
        expect(run([aliveOverCap, aliveOverCap])).toMatchObject({ reason: null });
        expect(run([aliveOverCap, aliveOverCap, aliveOverCap])).toEqual({
            state: { silentMs: 0, overCapMs: 15000 },
            reason: "over-cap",
        });
    });

    it("上限を下回った時点で数え直す", () => {
        expect(run([aliveOverCap, aliveOverCap, alive, aliveOverCap, aliveOverCap])).toMatchObject({
            state: { overCapMs: 10000 },
            reason: null,
        });
    });
});

describe("stepWatchdog: 両方", () => {
    it("同じ回に両方が限度に達したら、理由は ping-silence にする", () => {
        const config = { intervalMs: 5000, pingSilenceMs: 10000, overCapMs: 10000 };
        const overCapSilent = { pingSeen: false, overCap: true };
        expect(run([overCapSilent, overCapSilent], config)).toMatchObject({ reason: "ping-silence" });
    });

    it("間隔と限度は config で変えられる", () => {
        const config = { intervalMs: 1000, pingSilenceMs: 3000, overCapMs: 60000 };
        expect(run([silent, silent], config)).toMatchObject({ state: { silentMs: 2000 }, reason: null });
        expect(run([silent, silent, silent], config)).toMatchObject({ reason: "ping-silence" });
    });
});
