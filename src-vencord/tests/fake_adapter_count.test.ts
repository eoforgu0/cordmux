// SPDX-License-Identifier: Apache-2.0

// 依存箇所の一覧の行と、killedAdapters() が作る Adapter が、このテストに書いた 9 つの id と過不足なく一致することを
// 確認する。CONTACT_LEDGER.length と killedAdapters().length を比べるだけでは、一覧の行をまとめて減らしたときに
// 両方が同じだけ減るので、減ったことに気付けない。決まった 9 つの id と比べれば、行が増えても減っても気付ける
// (tests/ledger.test.ts の EXPECTED_IDS と同じ考え方)。

import { describe, expect, it } from "vitest";

import type { ContactId } from "../ledger";
import { CONTACT_LEDGER } from "../ledger";
import { killedAdapters } from "./fake_adapter";

/** 期待する 9 つの id。依存箇所を追加するか減らすときは、ここも変える。 */
const AXIS_G_CONTACT_IDS: readonly ContactId[] = [
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

describe("依存箇所の一覧の行と、テスト用の adapter の数を比べる", () => {
    it("期待する 9 つの id と、CONTACT_LEDGER の行と、killedAdapters() の Adapter が過不足なく一致する", () => {
        const ledgerIds = CONTACT_LEDGER.map(e => e.id);
        const killed = killedAdapters();
        const killedIds = killed.map(k => k.killedId);

        expect([...ledgerIds].sort()).toEqual([...AXIS_G_CONTACT_IDS].sort());
        expect([...killedIds].sort()).toEqual([...AXIS_G_CONTACT_IDS].sort());
        expect(new Set(killedIds).size, "killedAdapters() の id に重複がある").toBe(killedIds.length);
        expect(killed.length).toBe(CONTACT_LEDGER.length);
        expect(CONTACT_LEDGER.length).toBe(AXIS_G_CONTACT_IDS.length);
    });
});
