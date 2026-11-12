// SPDX-License-Identifier: Apache-2.0

// core/state.ts のファズテスト。乱数で作ったイベントの列 (2000 個 x 200 本) を reduce() に流し、warnings()、
// sharing()、decisions() の不変条件が保たれることと、送っている映像と音声を止める判断が出ないことを
// 確認する。seed は固定で、テストの名前と失敗のメッセージに出す。疑似乱数は mulberry32 (小さく決定的で、
// package.json の devDependencies を増やさずに書ける)。

import { describe, expect, it } from "vitest";

import type { CoreEvent } from "../core/events";
import type { ContactId } from "../ledger";
import { decisions, initialState, reduce, sharing, ssrcKey, warnings } from "../core/state";
import type { CoreState } from "../core/state";

// この値を変えると、200 本のイベントの列がすべて変わる。失敗を再現するときは、この値と run の番号 (0 から 199) から
// BASE_SEED + i を計算すればよい (下の it の名前にも、同じ値を出す)。
const BASE_SEED = 20000101;
const RUNS = 200;
const STEPS_PER_RUN = 2000;

console.log(`fuzz-state: BASE_SEED=${BASE_SEED} (再現するときは BASE_SEED + run index)`);

/** mulberry32: 小さく決定的な疑似乱数の生成器。依存関係を増やさないために、ここで実装する。 */
function mulberry32(seed: number): () => number {
    let state = seed >>> 0;
    return () => {
        state = (state + 0x6d2b79f5) | 0;
        let t = Math.imul(state ^ (state >>> 15), 1 | state);
        t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

function pick<T>(rng: () => number, values: readonly T[]): T {
    const index = Math.floor(rng() * values.length);
    // values は、呼び出し元がいつも空でない配列を渡す (ファズテスト用の決まった値の一覧)。
    return values[Math.min(index, values.length - 1)] as T;
}

const USER_IDS = ["u1", "u2", "u3", "u4", "u5"] as const;
const STREAM_KEYS = ["s1", "s2", "s3"] as const;
const PC_IDS = [1, 2, 3] as const;
const CONTACT_IDS: readonly ContactId[] = [
    "pc-hook",
    "ssrc-map",
    "sharing",
    "watch-stop",
    "roster",
    "profile",
    "self",
    "call-info",
    "voice-select",
];
const CONTACT_RESULTS = ["found", "missing", "lost"] as const;
const SSRC_VALUES = [null, 1, 2, 3, 4, 5] as const;

function randomEvent(rng: () => number): CoreEvent {
    const kind = Math.floor(rng() * 11);
    switch (kind) {
        case 0:
            return { type: "contact", id: pick(rng, CONTACT_IDS), result: pick(rng, CONTACT_RESULTS) };
        case 1: {
            const count = Math.floor(rng() * (USER_IDS.length + 1));
            const users = Array.from({ length: count }, () => ({ userId: pick(rng, USER_IDS) }));
            return { type: "roster", users };
        }
        case 2:
            return {
                type: "sharing",
                userId: pick(rng, USER_IDS),
                streamKey: pick(rng, STREAM_KEYS),
                active: rng() < 0.5,
                pcId: pick(rng, PC_IDS),
            };
        case 3:
            return {
                type: "ssrc",
                userId: pick(rng, USER_IDS),
                videoSsrc: pick(rng, SSRC_VALUES),
                audioSsrc: pick(rng, SSRC_VALUES),
                pcId: pick(rng, PC_IDS),
            };
        case 4:
            return { type: "self", userId: rng() < 0.5 ? null : pick(rng, USER_IDS) };
        case 5: {
            const count = Math.floor(rng() * (USER_IDS.length + 1));
            const userIds = Array.from({ length: count }, () => pick(rng, USER_IDS));
            return { type: "selection", userIds };
        }
        case 6:
            return {
                type: "watch_result",
                userId: pick(rng, USER_IDS),
                streamKey: pick(rng, STREAM_KEYS),
                ok: rng() < 0.5,
            };
        case 7:
            return { type: "tick", nowMs: Math.floor(rng() * 1_000_000) };
        case 8: {
            // 視聴する人の範囲 (null は全員)。
            if (rng() < 0.3) return { type: "watch_scope", userIds: null };
            const count = Math.floor(rng() * (USER_IDS.length + 1));
            return { type: "watch_scope", userIds: Array.from({ length: count }, () => pick(rng, USER_IDS)) };
        }
        case 9:
            return { type: "call_name", name: rng() < 0.3 ? null : "call" };
        default:
            return {
                type: "main_call",
                call: rng() < 0.3 ? null : { userId: pick(rng, USER_IDS), name: null, joinable: null },
            };
    }
}

const VALID_CONTACT_STATUSES = new Set(["unseen", "found", "missing", "lost"]);

/** 状態そのものが壊れていない (形として正しい) ことを確認する。 */
function assertValidState(state: CoreState, context: string): void {
    for (const id of CONTACT_IDS) {
        expect(VALID_CONTACT_STATUSES.has(state.contacts[id]), `${context}: contacts.${id}`).toBe(true);
    }

    expect(state.roster === "unknown" || Array.isArray(state.roster), `${context}: roster`).toBe(true);
    expect(state.selfUserId === null || typeof state.selfUserId === "string", `${context}: selfUserId`).toBe(true);
    expect(Array.isArray(state.selection), `${context}: selection`).toBe(true);
    expect(state.watchScope === null || Array.isArray(state.watchScope), `${context}: watchScope`).toBe(true);

    // 視聴している人は、範囲があれば、範囲に入っている人だけである。
    if (state.watchScope !== null) {
        for (const userId of Object.keys(state.watching)) {
            expect(state.watchScope.includes(userId), `${context}: 範囲の外の ${userId} を視聴している`).toBe(true);
        }
    }

    // 同じ userId が 2 回出ない (sharing() の結果と、watching の中身)。
    const sharingUsers = sharing(state).map((u) => u.userId);
    expect(new Set(sharingUsers).size, `${context}: sharing 重複`).toBe(sharingUsers.length);

    // watching は Record<string,string> なので、1 つの userId につき 1 件までしか持てない (型で保証される)。
    // decisions() にも userId の重複がないことを確認する (同じ userId に、同じ種類の判断を 2 つ同時に出さない)。
    const ds = decisions(state);
    const startTargets = ds.filter((d) => d.type === "start-watching").map((d) => d.userId);
    expect(new Set(startTargets).size, `${context}: start-watching 重複`).toBe(startTargets.length);
    const stopTargets = ds.filter((d) => d.type === "stop-watching").map((d) => d.userId);
    expect(new Set(stopTargets).size, `${context}: stop-watching 重複`).toBe(stopTargets.length);

    // decisions() に、送っている映像と音声を止める種類はない (型で保証しているが、実行でも確認する)。
    for (const d of ds) {
        expect(["start-watching", "stop-watching", "mark-source-switched"].includes(d.type), `${context}: 未知の decision 種類 ${d.type}`).toBe(true);
    }
}

/** warnings() は、依存箇所の missing と lost だけから作られる。 */
function assertWarningsMatchContacts(state: CoreState, context: string): void {
    const warned = new Set(warnings(state));
    for (const id of CONTACT_IDS) {
        const status = state.contacts[id];
        const shouldWarn = status === "missing" || status === "lost";
        expect(warned.has(id), `${context}: warnings と contacts.${id}=${status} の食い違い`).toBe(shouldWarn);
    }
}

/** イベントの種類ごとの不変条件。イベント 1 つごとに、前と後の状態を比べて確認する。 */
function assertStepInvariants(prev: CoreState, event: CoreEvent, next: CoreState, context: string): void {
    if (event.type === "contact") {
        // 依存箇所が使えなくなったことで、送っている映像と音声を止める判断も始める判断も、1 件も作らない。
        expect(decisions(next), `${context}: contact のイベントから decisions が生じた`).toEqual([]);
    }

    if (event.type === "roster") {
        // roster の更新は selection を変えない。
        expect(next.selection, `${context}: roster のイベントで selection が変わった`).toEqual(prev.selection);
    }

    if (event.type === "watch_result") {
        // watch_result だけでは decisions を作らない (watching を取り消すだけ)。
        expect(decisions(next), `${context}: watch_result から decisions が生じた`).toEqual([]);
    }

    if (event.type === "ssrc") {
        // mark-source-switched が出るかどうかを、core/state.ts の reduceSsrc とは別に (prev の値から) 計算し直して
        // 比べる。
        const switches = decisions(next).filter((d) => d.type === "mark-source-switched");
        if (event.videoSsrc === null) {
            // 映像を送らない ssrc のイベント (マイクの ssrc など) からは出ない (画面共有をしたまま話しただけで、
            // 替わったと判断しない)。
            expect(switches, `${context}: 映像を送らない ssrc のイベントから mark-source-switched が出た`).toEqual([]);
        } else {
            const priorVideoPcId = prev.videoPcIdByUser[event.userId];
            const prevAtKey = prev.ssrc[ssrcKey(event.userId, event.pcId)];
            const pcChanged = priorVideoPcId !== undefined && priorVideoPcId !== event.pcId;
            const prevKnownVideo = prevAtKey?.lastNonNullVideoSsrc ?? null;
            const videoChanged = prevKnownVideo !== null && event.videoSsrc !== prevKnownVideo;
            const expectSwitch = pcChanged || videoChanged;
            expect(switches.length, `${context}: mark-source-switched の有無が一致しない`).toBe(
                expectSwitch ? 1 : 0,
            );
            if (expectSwitch) {
                expect(switches[0]).toEqual({ type: "mark-source-switched", userId: event.userId, pcId: event.pcId });
            }
        }
    }

    for (const d of decisions(next)) {
        if (d.type === "start-watching") {
            expect(next.selection.includes(d.userId), `${context}: start-watching 対象が selection にない`).toBe(true);
            const info = next.sharing[d.userId];
            expect(info?.active, `${context}: start-watching 対象が active な sharing を持たない`).toBe(true);
            expect(info?.streamKey, `${context}: start-watching の streamKey が sharing と食い違う`).toBe(d.streamKey);
        } else if (d.type === "stop-watching") {
            expect(prev.watching[d.userId], `${context}: stop-watching が直前の watching にない対象を指す`).toBe(
                d.streamKey,
            );
        }
    }
}

describe("core/state のファズテスト", () => {
    for (let run = 0; run < RUNS; run++) {
        const seed = BASE_SEED + run;

        it(`run ${run} (seed=${seed}) で ${STEPS_PER_RUN} 個のイベントを流しても、不変条件が保たれる`, () => {
            const rng = mulberry32(seed);
            let state: CoreState = initialState;
            assertValidState(state, `run${run}/seed${seed}/step0`);

            for (let step = 0; step < STEPS_PER_RUN; step++) {
                const event = randomEvent(rng);
                const next = reduce(state, event);
                const context = `run${run}/seed${seed}/step${step + 1}/event=${JSON.stringify(event)}`;

                assertValidState(next, context);
                assertWarningsMatchContacts(next, context);
                assertStepInvariants(state, event, next, context);

                state = next;
            }
        });
    }
});
