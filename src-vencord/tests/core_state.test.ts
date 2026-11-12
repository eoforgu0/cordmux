// SPDX-License-Identifier: Apache-2.0

// core/state.ts のテスト。依存箇所の 4 つの状態、warnings、roster の「分からない」と「ない」の区別、
// sharing、isPlausibleVoiceSource()、userIdForTrackNow() を確認する。decisions() のテストは tests/core_state_decisions.test.ts にある。

import { describe, expect, it } from "vitest";

import type { CoreEvent } from "../core/events";
import {
    initialState,
    isPlausibleVoiceSource,
    missingContactEvent,
    reduce,
    sharing,
    userIdForTrackNow,
    warnings,
} from "../core/state";
import type { CoreState } from "../core/state";

function apply(events: readonly CoreEvent[], from: CoreState = initialState): CoreState {
    return events.reduce(reduce, from);
}

describe("依存箇所の状態", () => {
    it("初期状態では 9 行すべてが unseen で、warnings は空", () => {
        expect(warnings(initialState)).toEqual([]);
        for (const id of ["pc-hook", "ssrc-map", "sharing", "watch-stop", "roster", "profile", "self", "call-info", "voice-select"] as const) {
            expect(initialState.contacts[id]).toBe("unseen");
        }
    });

    it("一度も見つからないまま (unseen -> missing) なら missing のままで、lost にはしない", () => {
        const state = apply([{ type: "contact", id: "roster", result: "missing" }]);
        expect(state.contacts.roster).toBe("missing");
        expect(warnings(state)).toEqual(["roster"]);
    });

    it("found の後の missing は lost として扱う", () => {
        const state = apply([
            { type: "contact", id: "roster", result: "found" },
            { type: "contact", id: "roster", result: "missing" },
        ]);
        expect(state.contacts.roster).toBe("lost");
        expect(warnings(state)).toEqual(["roster"]);
    });

    it("adapter が lost を直接伝えたときも lost になる", () => {
        const state = apply([{ type: "contact", id: "roster", result: "lost" }]);
        expect(state.contacts.roster).toBe("lost");
    });

    it("lost の後にまた missing が来ても lost のまま", () => {
        const state = apply([
            { type: "contact", id: "roster", result: "found" },
            { type: "contact", id: "roster", result: "missing" },
            { type: "contact", id: "roster", result: "missing" },
        ]);
        expect(state.contacts.roster).toBe("lost");
    });

    it("missing か lost の後に found が届けば、found に戻る", () => {
        const state = apply([
            { type: "contact", id: "roster", result: "found" },
            { type: "contact", id: "roster", result: "missing" },
            { type: "contact", id: "roster", result: "found" },
        ]);
        expect(state.contacts.roster).toBe("found");
        expect(warnings(state)).toEqual([]);
    });

    it("1 つの依存箇所の変化は、ほかの依存箇所に影響しない", () => {
        const state = apply([
            { type: "contact", id: "roster", result: "missing" },
            { type: "contact", id: "self", result: "found" },
        ]);
        expect(state.contacts.roster).toBe("missing");
        expect(state.contacts.self).toBe("found");
        expect(state.contacts["pc-hook"]).toBe("unseen");
    });

    it("missingContactEvent(id) は contact の missing のイベントを作り、reduce() に渡せる", () => {
        const event = missingContactEvent("pc-hook");
        expect(event).toEqual({ type: "contact", id: "pc-hook", result: "missing" });
        const state = reduce(initialState, event);
        expect(state.contacts["pc-hook"]).toBe("missing");
    });

    it("warnings は、missing か lost の依存箇所の id と過不足なく一致する", () => {
        const state = apply([
            { type: "contact", id: "roster", result: "found" },
            { type: "contact", id: "roster", result: "missing" }, // -> lost
            { type: "contact", id: "self", result: "missing" }, // -> missing
            { type: "contact", id: "profile", result: "found" }, // -> found (warnings に出ない)
        ]);
        expect(new Set(warnings(state))).toEqual(new Set(["roster", "self"]));
    });
});

describe("roster: 「分からない」と「ない」の区別", () => {
    it("初期状態は unknown (まだ読めていない)", () => {
        expect(initialState.roster).toBe("unknown");
    });

    it("roster のイベントが空の配列でも、読めたこととして区別して持つ (unknown ではなくなる)", () => {
        const state = reduce(initialState, { type: "roster", users: [] });
        expect(state.roster).toEqual([]);
        expect(state.roster).not.toBe("unknown");
    });

    it("roster の更新は selection を変えない", () => {
        const withSelection = reduce(initialState, { type: "selection", userIds: ["u1", "u2"] });
        const afterRoster = reduce(withSelection, { type: "roster", users: [{ userId: "u9" }] });
        expect(afterRoster.selection).toEqual(["u1", "u2"]);

        // roster の依存箇所が missing になっても、selection は変わらない
        const afterMissing = reduce(afterRoster, { type: "contact", id: "roster", result: "missing" });
        expect(afterMissing.selection).toEqual(["u1", "u2"]);
    });
});

describe("sharing: 今アクティブな画面共有の一覧", () => {
    it("active な画面共有だけを返す", () => {
        const state = apply([
            { type: "sharing", userId: "u1", streamKey: "s1", active: true, pcId: 10 },
            { type: "sharing", userId: "u2", streamKey: "s2", active: false, pcId: 11 },
        ]);
        expect(sharing(state)).toEqual([{ userId: "u1", streamKey: "s1", pcId: 10 }]);
    });

    it("同じ userId の新しい sharing のイベントが、古い値を上書きする", () => {
        const state = apply([
            { type: "sharing", userId: "u1", streamKey: "s1", active: true, pcId: 10 },
            { type: "sharing", userId: "u1", streamKey: "s1", active: false, pcId: 10 },
        ]);
        expect(sharing(state)).toEqual([]);
    });

    it("同じ userId が、2 つのエントリー (sharing の 2 行) として出ない", () => {
        const state = apply([
            { type: "sharing", userId: "u1", streamKey: "s1", active: true, pcId: 10 },
            { type: "sharing", userId: "u1", streamKey: "s2", active: true, pcId: 12 },
        ]);
        const result = sharing(state);
        expect(result.length).toBe(1);
        expect(result[0]).toEqual({ userId: "u1", streamKey: "s2", pcId: 12 });
    });
});

describe("isPlausibleVoiceSource: pcId の取り違えに備えた、声の追加の確認", () => {
    it("sharing の事実がない userId は、声として扱ってよい (通常の場合)", () => {
        expect(isPlausibleVoiceSource(initialState, "u1")).toBe(true);
    });

    it("別の pcId (10) で画面共有をしている userId は、声 (pcId 1) として扱ってよい (話しながら画面共有をするのは普通のことである)", () => {
        const state = apply([{ type: "sharing", userId: "u1", streamKey: "s1", active: true, pcId: 10 }]);
        expect(isPlausibleVoiceSource(state, "u1")).toBe(true);
    });

    it("画面共有の pcId が 1 (VOICE_PC_ID) と同じになった userId は、声として扱わない (接続の取り違え)", () => {
        // Go Live の接続が pcId 1 になった場合。現在の ConnectionTracker ではこの取り違えは起きないが、念のための確認として
        // 残している動作を確認する。
        const state = apply([{ type: "sharing", userId: "u1", streamKey: "s1", active: true, pcId: 1 }]);
        expect(isPlausibleVoiceSource(state, "u1")).toBe(false);
    });

    it("画面共有をやめた (active: false) 後は、pcId が 1 のままでも声として扱ってよい", () => {
        const state = apply([
            { type: "sharing", userId: "u1", streamKey: "s1", active: true, pcId: 1 },
            { type: "sharing", userId: "u1", streamKey: "s1", active: false, pcId: 1 },
        ]);
        expect(isPlausibleVoiceSource(state, "u1")).toBe(true);
    });

    it("ほかの userId が pcId 1 で画面共有をしていても、この userId には影響しない (userId ごとに独立している)", () => {
        const state = apply([{ type: "sharing", userId: "other", streamKey: "s1", active: true, pcId: 1 }]);
        expect(isPlausibleVoiceSource(state, "u1")).toBe(true);
    });
});

describe("userIdForTrackNow: トラックが届いた時点の手がかりから userId を特定する", () => {
    /** u1 の声の SSRC (audioSsrc 77) が pcId 1 で知らされている状態。 */
    function stateWithVoiceSsrc(): CoreState {
        return apply([
            { type: "roster", users: [{ userId: "u1" }, { userId: "u2" }] },
            { type: "ssrc", userId: "u1", videoSsrc: null, audioSsrc: 77, pcId: 1 },
        ]);
    }

    it("まだ話していない人の声のトラック (ssrcs が空) でも、msid が示す人と SSRC のメッセージが一致すれば、すぐ特定する", () => {
        const found = userIdForTrackNow(stateWithVoiceSsrc(), 1, {
            owner: undefined,
            declared: { userId: "u1", ssrc: 77 },
            ssrcs: [],
        });
        expect(found).toBe("u1");
    });

    it("msid が示す人と SSRC のメッセージが食い違えば、msid は使わない (別の人の SSRC を取り違えない)", () => {
        const found = userIdForTrackNow(stateWithVoiceSsrc(), 1, {
            owner: undefined,
            declared: { userId: "u2", ssrc: 77 },
            ssrcs: [],
        });
        expect(found).toBeUndefined();
    });

    it("msid を使えなくても、トラックに届いた SSRC がメッセージと一致すれば特定する", () => {
        const found = userIdForTrackNow(stateWithVoiceSsrc(), 1, { owner: undefined, declared: null, ssrcs: [5, 77] });
        expect(found).toBe("u1");
    });

    it("接続で画面共有している人がいれば、SSRC より先にその人にする", () => {
        const found = userIdForTrackNow(stateWithVoiceSsrc(), 2, {
            owner: "u2",
            declared: { userId: "u1", ssrc: 77 },
            ssrcs: [77],
        });
        expect(found).toBe("u2");
    });

    it("pcId が違えば SSRC の番号が同じでも特定しない。roster が読めていない間も特定しない", () => {
        expect(userIdForTrackNow(stateWithVoiceSsrc(), 2, { owner: undefined, declared: null, ssrcs: [77] })).toBeUndefined();
        const unknownRoster = reduce(initialState, { type: "ssrc", userId: "u1", videoSsrc: null, audioSsrc: 77, pcId: 1 });
        expect(userIdForTrackNow(unknownRoster, 1, { owner: undefined, declared: { userId: "u1", ssrc: 77 }, ssrcs: [77] })).toBeUndefined();
    });
});
