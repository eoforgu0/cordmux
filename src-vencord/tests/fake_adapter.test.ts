// SPDX-License-Identifier: Apache-2.0

// tests/fake_adapter.ts の killedAdapters() が作る、依存箇所を 1 つ使えなくした Adapter を core (reduce()) に流し、
// (1) 使えなくした行の id が warnings に出ること、(2) 送っている映像と音声を止める判断が 1 つも出ない (decisions が
// 0 件のままで、視聴も続く) こと、(3) ほかの 8 行が found のままであることを確認する。(2) を意味のあるテストにする
// ため、使えなくする前に視聴を 1 つ始めておく。何も動いていない状態では、decisions が 0 件でも何も確認していない。

import { describe, expect, it } from "vitest";

import type { CoreEvent } from "../core/events";
import { CONTACT_LEDGER } from "../ledger";
import { decisions, initialState, reduce, warnings } from "../core/state";
import type { CoreState } from "../core/state";
import { killedAdapters } from "./fake_adapter";

function apply(events: readonly CoreEvent[], from: CoreState = initialState): CoreState {
    return events.reduce(reduce, from);
}

/** 視聴を 1 つ始めた状態 (受信側の範囲が everyone で、u1 が s1 で画面共有をしていて、選ばれていて、視聴の開始が成功している)。 */
function stateWithOneActiveWatch(): CoreState {
    const state = apply([
        // 初期状態は、受信側から範囲が届くまで誰も視聴しない。everyone が届いた後の状態から始める。
        { type: "watch_scope", userIds: null },
        { type: "sharing", userId: "u1", streamKey: "s1", active: true, pcId: 1 },
        { type: "selection", userIds: ["u1"] },
    ]);
    expect(decisions(state)).toEqual([{ type: "start-watching", userId: "u1", streamKey: "s1" }]);
    return apply([{ type: "watch_result", userId: "u1", streamKey: "s1", ok: true }], state);
}

describe("テスト用の adapter: 依存箇所の一覧の行を 1 つずつ使えなくする", () => {
    for (const { killedId, adapter } of killedAdapters()) {
        it(`${killedId} を使えなくすると id が warnings に出て、送っている映像と音声を止める判断は 0 件のまま`, () => {
            const before = stateWithOneActiveWatch();
            expect(before.watching.u1).toBe("s1");

            const events: CoreEvent[] = [];
            const unsubscribe = adapter.subscribe(e => events.push(e));
            unsubscribe();
            expect(events.length).toBe(CONTACT_LEDGER.length);

            const after = apply(events, before);

            // 使えなくした行だけが warnings に出る。core は ContactId をそのまま返す。画面の文言にするのは、受信側の
            // src-obs/ledger/rules_dock.cpp の kContactHeadlines である。
            expect(warnings(after)).toEqual([killedId]);
            expect(CONTACT_LEDGER.some(e => e.id === killedId), `依存箇所の一覧に ${killedId} がない`).toBe(true);

            // ほかの 8 行は found のまま。
            for (const other of CONTACT_LEDGER) {
                if (other.id === killedId) continue;
                expect(after.contacts[other.id], `${other.id} が found のままでない`).toBe("found");
            }

            // 送っている映像と音声を止める判断は、1 つも出ない。型 (Decision) にもその種類はないが、ここでは実行した
            // 結果としても 0 件であることを確認する。
            expect(decisions(after)).toEqual([]);
            expect(after.watching.u1, "進行中の視聴 (u1) が消えた").toBe("s1");
        });
    }
});
