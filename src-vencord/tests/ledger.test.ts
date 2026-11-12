// SPDX-License-Identifier: Apache-2.0

// 依存箇所の一覧 (ledger.ts の CONTACT_LEDGER) の形のテスト。一覧の id と受信側の見出しと ini の比較は
// tests/ledger_ini.test.ts にある。Vencord を import しないので、Node で実行できる。

import { describe, expect, it } from "vitest";

import { CONTACT_LEDGER, type ContactId } from "../ledger";

/** 期待する 9 つの id。依存箇所を追加するか減らすときは、ここも変える。 */
const EXPECTED_IDS: readonly ContactId[] = [
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

describe("依存箇所の一覧 (CONTACT_LEDGER)", () => {
    it("9 行ある", () => {
        expect(CONTACT_LEDGER).toHaveLength(9);
    });

    it("id に重複がなく、期待する 9 つの id と過不足なく一致する", () => {
        const ids = CONTACT_LEDGER.map(entry => entry.id);
        expect(new Set(ids).size).toBe(ids.length);
        expect([...ids].sort()).toEqual([...EXPECTED_IDS].sort());
    });

    it("stopsExisting は常に false である (送っている映像と音声を止める判断はない)", () => {
        for (const entry of CONTACT_LEDGER) {
            expect(entry.stopsExisting).toBe(false);
        }
    });

    it("述語 (props または code) は 1 つ以上の文字列を持つ", () => {
        for (const entry of CONTACT_LEDGER) {
            const list = entry.predicate.kind === "props" ? entry.predicate.props : entry.predicate.code;
            expect(Array.isArray(list)).toBe(true);
            expect(list.length).toBeGreaterThan(0);
            for (const item of list) {
                expect(typeof item).toBe("string");
                expect(item.length).toBeGreaterThan(0);
            }
        }
    });

    it("一覧の行は画面の文言のキーを持たない (見出しの文言は受信側が持つ)", () => {
        for (const entry of CONTACT_LEDGER) {
            expect(Object.keys(entry).sort()).toEqual(["id", "predicate", "stopsExisting"]);
        }
    });
});
