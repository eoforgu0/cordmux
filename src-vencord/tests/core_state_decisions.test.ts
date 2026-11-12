// SPDX-License-Identifier: Apache-2.0

// core/state.ts の decisions() のテスト。selection と sharing の差から作る start-watching と stop-watching、映像の
// SSRC か接続が替わったときの mark-source-switched を確認する。decisions() が、直前の reduce() 1 回で生じた判断だけを
// 返すことと、watch_result (adapter が startWatching() を実行した結果) の反映も確認する。

import { describe, expect, it } from "vitest";

import type { CoreEvent } from "../core/events";
import { decisions, initialState, reduce } from "../core/state";

// 受信側が everyone を送った後の状態 (画面共有している全員を視聴する)。初期状態は、範囲が届くまで誰も視聴しない。
const everyoneState = reduce(initialState, { type: "watch_scope", userIds: null });

function apply(events: readonly CoreEvent[], from: ReturnType<typeof reduce> = everyoneState): ReturnType<typeof reduce> {
    return events.reduce(reduce, from);
}

describe("decisions: start-watching と stop-watching", () => {
    it("選ばれていて、画面共有をしている人には start-watching を出す", () => {
        const s1 = reduce(everyoneState, { type: "sharing", userId: "u1", streamKey: "s1", active: true, pcId: 10 });
        const s2 = reduce(s1, { type: "selection", userIds: ["u1"] });
        expect(decisions(s2)).toEqual([{ type: "start-watching", userId: "u1", streamKey: "s1" }]);
    });

    it("選ばれていても、画面共有をしていなければ何も決めない", () => {
        const s1 = reduce(everyoneState, { type: "selection", userIds: ["u1"] });
        expect(decisions(s1)).toEqual([]);
    });

    it("画面共有をしていても、選ばれていなければ何も決めない", () => {
        const s1 = reduce(everyoneState, { type: "sharing", userId: "u1", streamKey: "s1", active: true, pcId: 10 });
        expect(decisions(s1)).toEqual([]);
    });

    it("decisions() は、直前の reduce() 1 回で生じた判断だけを返す", () => {
        const s1 = reduce(everyoneState, { type: "sharing", userId: "u1", streamKey: "s1", active: true, pcId: 10 });
        const s2 = reduce(s1, { type: "selection", userIds: ["u1"] });
        expect(decisions(s2).length).toBe(1);

        // 何も変えないイベント (tick) を 1 つはさむと、同じ start-watching を繰り返し出さない
        const s3 = reduce(s2, { type: "tick", nowMs: 1 });
        expect(decisions(s3)).toEqual([]);
    });

    it("選ばれなくなったら stop-watching を出す", () => {
        const s1 = reduce(everyoneState, { type: "sharing", userId: "u1", streamKey: "s1", active: true, pcId: 10 });
        const s2 = reduce(s1, { type: "selection", userIds: ["u1"] });
        const s3 = reduce(s2, { type: "selection", userIds: [] });
        expect(decisions(s3)).toEqual([{ type: "stop-watching", userId: "u1", streamKey: "s1" }]);
    });

    it("選ばれたまま画面共有が終わると stop-watching を出す", () => {
        const s1 = reduce(everyoneState, { type: "sharing", userId: "u1", streamKey: "s1", active: true, pcId: 10 });
        const s2 = reduce(s1, { type: "selection", userIds: ["u1"] });
        const s3 = reduce(s2, { type: "sharing", userId: "u1", streamKey: "s1", active: false, pcId: 10 });
        expect(decisions(s3)).toEqual([{ type: "stop-watching", userId: "u1", streamKey: "s1" }]);
    });

    it("選ばれたまま別の画面共有 (streamKey が違う) に替わると、前の視聴を止めてから新しい視聴を始める", () => {
        const s1 = reduce(everyoneState, { type: "sharing", userId: "u1", streamKey: "s1", active: true, pcId: 10 });
        const s2 = reduce(s1, { type: "selection", userIds: ["u1"] });
        const s3 = reduce(s2, { type: "sharing", userId: "u1", streamKey: "s2", active: true, pcId: 11 });
        expect(decisions(s3)).toEqual([
            { type: "stop-watching", userId: "u1", streamKey: "s1" },
            { type: "start-watching", userId: "u1", streamKey: "s2" },
        ]);
    });

    it("複数の人の選択と画面共有の差を、まとめて出す", () => {
        const s1 = reduce(everyoneState, { type: "sharing", userId: "u1", streamKey: "s1", active: true, pcId: 10 });
        const s2 = reduce(s1, { type: "sharing", userId: "u2", streamKey: "s2", active: true, pcId: 20 });
        const s3 = reduce(s2, { type: "selection", userIds: ["u1", "u2"] });
        expect(new Set(decisions(s3))).toEqual(
            new Set([
                { type: "start-watching", userId: "u1", streamKey: "s1" },
                { type: "start-watching", userId: "u2", streamKey: "s2" },
            ]),
        );
    });

    it("依存箇所が missing か lost になっても、decisions() は 0 件のまま (送っている映像と音声を止める判断は作らない)", () => {
        const s1 = reduce(everyoneState, { type: "sharing", userId: "u1", streamKey: "s1", active: true, pcId: 10 });
        const s2 = reduce(s1, { type: "selection", userIds: ["u1"] }); // ここで start-watching が 1 件出る
        expect(decisions(s2).length).toBe(1);

        const s3 = reduce(s2, { type: "contact", id: "roster", result: "missing" });
        expect(decisions(s3)).toEqual([]);

        const s4 = reduce(s3, { type: "contact", id: "pc-hook", result: "lost" });
        expect(decisions(s4)).toEqual([]);
    });
});

describe("decisions: mark-source-switched", () => {
    it("最初の ssrc のイベントでは何も決めない (替わったのではなく、初めて見たため)", () => {
        const s1 = reduce(everyoneState, { type: "ssrc", userId: "u1", videoSsrc: 100, audioSsrc: 1, pcId: 10 });
        expect(decisions(s1)).toEqual([]);
    });

    it("同じ userId の videoSsrc が変わったら mark-source-switched を出す", () => {
        const s1 = reduce(everyoneState, { type: "ssrc", userId: "u1", videoSsrc: 100, audioSsrc: 1, pcId: 10 });
        const s2 = reduce(s1, { type: "ssrc", userId: "u1", videoSsrc: 200, audioSsrc: 1, pcId: 10 });
        expect(decisions(s2)).toEqual([{ type: "mark-source-switched", userId: "u1", pcId: 10 }]);
    });

    it("同じ userId の pcId (接続) が変わったら mark-source-switched を出す", () => {
        const s1 = reduce(everyoneState, { type: "ssrc", userId: "u1", videoSsrc: 100, audioSsrc: 1, pcId: 10 });
        const s2 = reduce(s1, { type: "ssrc", userId: "u1", videoSsrc: 100, audioSsrc: 1, pcId: 11 });
        expect(decisions(s2)).toEqual([{ type: "mark-source-switched", userId: "u1", pcId: 11 }]);
    });

    it("同じ値が繰り返し届いても、もう一度は出さない", () => {
        const s1 = reduce(everyoneState, { type: "ssrc", userId: "u1", videoSsrc: 100, audioSsrc: 1, pcId: 10 });
        const s2 = reduce(s1, { type: "ssrc", userId: "u1", videoSsrc: 100, audioSsrc: 1, pcId: 10 });
        expect(decisions(s2)).toEqual([]);
    });

    it("videoSsrc が null を挟んで別の値に変わっても見逃さない (100 -> null -> 200)", () => {
        const s1 = reduce(everyoneState, { type: "ssrc", userId: "u1", videoSsrc: 100, audioSsrc: 1, pcId: 10 });
        const s2 = reduce(s1, { type: "ssrc", userId: "u1", videoSsrc: null, audioSsrc: 1, pcId: 10 });
        expect(decisions(s2)).toEqual([]); // null になった時点ではまだ「変わった」と断定しない
        const s3 = reduce(s2, { type: "ssrc", userId: "u1", videoSsrc: 200, audioSsrc: 1, pcId: 10 });
        expect(decisions(s3)).toEqual([{ type: "mark-source-switched", userId: "u1", pcId: 10 }]);
    });

    it("videoSsrc が一度も分からなかった (null から値になった) ときは、初めて見たものとして扱い、switched を出さない", () => {
        const s1 = reduce(everyoneState, { type: "ssrc", userId: "u1", videoSsrc: null, audioSsrc: 1, pcId: 10 });
        const s2 = reduce(s1, { type: "ssrc", userId: "u1", videoSsrc: 100, audioSsrc: 1, pcId: 10 });
        expect(decisions(s2)).toEqual([]);
    });

    it("画面共有をしたまま話しただけ (マイクの ssrc のイベントで、videoSsrc は常に null) では、switched を誤って出さない", () => {
        // 画面共有 (pcId 2) の映像が確立した後、ボイス接続 (pcId 1) からマイクの ssrc のイベントが届く。
        // userId は画面共有をしている人と同じ (画面共有をしながら話す、いちばんよくある形)。
        const s1 = reduce(everyoneState, { type: "ssrc", userId: "u1", videoSsrc: 555, audioSsrc: 1, pcId: 2 });
        const s2 = reduce(s1, { type: "ssrc", userId: "u1", videoSsrc: null, audioSsrc: 999, pcId: 1 });
        expect(decisions(s2)).toEqual([]);

        // 画面共有 (pcId 2) の映像の値は、マイクのイベントで変わっていない
        // (同じ userId でも、pcId ごとに ssrc を分けて持つ)。
        const s3 = reduce(s2, { type: "ssrc", userId: "u1", videoSsrc: 777, audioSsrc: 1, pcId: 2 });
        expect(decisions(s3)).toEqual([{ type: "mark-source-switched", userId: "u1", pcId: 2 }]);
    });
});

describe("watch_result: adapter が startWatching() を実行した結果の反映", () => {
    it("ok: true なら watching をそのまま保つ (start-watching を決めたときに、成功するものとして入れてある)", () => {
        const s1 = reduce(everyoneState, { type: "sharing", userId: "u1", streamKey: "s1", active: true, pcId: 10 });
        const s2 = reduce(s1, { type: "selection", userIds: ["u1"] });
        expect(s2.watching.u1).toBe("s1");

        const s3 = reduce(s2, { type: "watch_result", userId: "u1", streamKey: "s1", ok: true });
        expect(s3.watching.u1).toBe("s1");
        expect(decisions(s3)).toEqual([]);
    });

    it("ok: false なら watching から取り消し、decisions() は 0 件のまま (送っている映像と音声を止める判断ではない)", () => {
        const s1 = reduce(everyoneState, { type: "sharing", userId: "u1", streamKey: "s1", active: true, pcId: 10 });
        const s2 = reduce(s1, { type: "selection", userIds: ["u1"] });
        expect(s2.watching.u1).toBe("s1");

        const s3 = reduce(s2, { type: "watch_result", userId: "u1", streamKey: "s1", ok: false, reason: "見つかりません" });
        expect(s3.watching.u1).toBeUndefined();
        expect(decisions(s3)).toEqual([]);

        // 取り消した後、sharing か selection がもう一度変われば、start-watching を出し直せる
        // (探し直すか、やめるかは adapter の役目である。ここでは、core が視聴を頼んだ記録を戻しただけであることを
        // 確認する)。
        const s4 = reduce(s3, { type: "tick", nowMs: 1 });
        const s5 = reduce(s4, { type: "selection", userIds: [] });
        const s6 = reduce(s5, { type: "selection", userIds: ["u1"] });
        expect(decisions(s6)).toEqual([{ type: "start-watching", userId: "u1", streamKey: "s1" }]);
    });

    it("遅れて届いた古い ok: false (既に別の streamKey に替わっている) は、新しい watching を壊さない", () => {
        const s1 = reduce(everyoneState, { type: "sharing", userId: "u1", streamKey: "s1", active: true, pcId: 10 });
        const s2 = reduce(s1, { type: "selection", userIds: ["u1"] });
        const s3 = reduce(s2, { type: "sharing", userId: "u1", streamKey: "s2", active: true, pcId: 11 });
        expect(s3.watching.u1).toBe("s2");

        // s1 (古い streamKey) の失敗が遅れて届いても、現在の s2 は消えない。
        const s4 = reduce(s3, { type: "watch_result", userId: "u1", streamKey: "s1", ok: false });
        expect(s4.watching.u1).toBe("s2");
        expect(decisions(s4)).toEqual([]);
    });
});

describe("decisions: 視聴する人の範囲 (watch_scope)", () => {
    const sharingOf = (userId: string): CoreEvent => ({
        type: "sharing",
        userId,
        streamKey: `k-${userId}`,
        active: true,
        pcId: 0,
    });

    it("範囲が届いていない間 (初期状態) は、selection と sharing の両方に入っている人がいても、誰も視聴しない", () => {
        const s = apply([sharingOf("u1"), sharingOf("u2"), { type: "selection", userIds: ["u1", "u2"] }], initialState);
        expect(initialState.watchScope).toEqual([]);
        expect(s.watching).toEqual({});
        expect(decisions(s)).toEqual([]);
    });

    it("範囲が届くまで start-watching を出さず、受信側が everyone を送ると全員の視聴を始める", () => {
        const waiting = apply([sharingOf("u1"), sharingOf("u2"), { type: "selection", userIds: ["u1", "u2"] }], initialState);
        const tick = reduce(waiting, { type: "tick", nowMs: 1 });
        expect(decisions(tick)).toEqual([]);
        const everyone = reduce(waiting, { type: "watch_scope", userIds: null });
        expect(decisions(everyone)).toEqual([
            { type: "start-watching", userId: "u1", streamKey: "k-u1" },
            { type: "start-watching", userId: "u2", streamKey: "k-u2" },
        ]);
        expect(Object.keys(everyone.watching).sort()).toEqual(["u1", "u2"]);
    });

    it("受信側が slots を送ると、その人だけの視聴を始める", () => {
        const waiting = apply([sharingOf("u1"), sharingOf("u2"), { type: "selection", userIds: ["u1", "u2"] }], initialState);
        const slots = reduce(waiting, { type: "watch_scope", userIds: ["u2"] });
        expect(decisions(slots)).toEqual([{ type: "start-watching", userId: "u2", streamKey: "k-u2" }]);
    });

    it("範囲に入っていない人は視聴を始めない", () => {
        const s = apply([
            { type: "watch_scope", userIds: ["u1"] },
            sharingOf("u1"),
            sharingOf("u2"),
            { type: "selection", userIds: ["u1", "u2"] },
        ]);
        expect(Object.keys(s.watching)).toEqual(["u1"]);
    });

    it("範囲から外した人は視聴をやめ、範囲に加えた人は視聴を始める", () => {
        const before = apply([
            { type: "watch_scope", userIds: ["u1"] },
            sharingOf("u1"),
            sharingOf("u2"),
            { type: "selection", userIds: ["u1", "u2"] },
        ]);
        const swapped = reduce(before, { type: "watch_scope", userIds: ["u2"] });
        expect(decisions(swapped)).toEqual([
            { type: "start-watching", userId: "u2", streamKey: "k-u2" },
            { type: "stop-watching", userId: "u1", streamKey: "k-u1" },
        ]);
    });

    it("範囲を全員 (null) に戻すと、範囲の外にいた人の視聴を始める", () => {
        const before = apply([
            { type: "watch_scope", userIds: [] },
            sharingOf("u1"),
            { type: "selection", userIds: ["u1"] },
        ]);
        expect(Object.keys(before.watching)).toEqual([]);
        const after = reduce(before, { type: "watch_scope", userIds: null });
        expect(decisions(after)).toEqual([{ type: "start-watching", userId: "u1", streamKey: "k-u1" }]);
    });

    it("範囲の変化は、同じ範囲を繰り返し受け取っても判断を出し直さない", () => {
        const s1 = apply([
            { type: "watch_scope", userIds: ["u1"] },
            sharingOf("u1"),
            { type: "selection", userIds: ["u1"] },
        ]);
        const s2 = reduce(s1, { type: "watch_scope", userIds: ["u1"] });
        expect(decisions(s2)).toEqual([]);
    });
});
