// SPDX-License-Identifier: Apache-2.0

// 依存箇所の一覧の id と、受信側の見出しと、ini のキーを比べる。(1) 一覧の id と src-obs/ledger/rules_dock.cpp の
// kContactHeadlines の行が過不足なく一致すること、(2) その行の見出しのキーが 2 つの ini にあること、(3) ini に前の形の
// キー (cordmux.contact.*) が残っていないことを確認する。一覧の形のテストは tests/ledger.test.ts にある。
// 受信側は知らない依存箇所を最も重い結果として扱うので、送信側で行を追加して受信側に追加し忘れても画面は崩れないが、
// 軽い依存箇所が欠けただけでメッセージのレベル 1 の見出しが表示される。その追加し忘れを、このテストで見つける。

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

import { CONTACT_LEDGER } from "../ledger";

function readRepoFile(relativePathFromThisFile: string): string {
    return readFileSync(fileURLToPath(new URL(relativePathFromThisFile, import.meta.url)), "utf8");
}

/**
 * ini の `key="value"` の形から、key -> value の対応を取り出す (# と ; で始まる行はコメントとして飛ばす)。値の
 * 引用符は外すだけで、エスケープは解釈しない。
 */
function readIniEntries(relativePathFromThisFile: string): Map<string, string> {
    const entries = new Map<string, string>();
    for (const rawLine of readRepoFile(relativePathFromThisFile).split(/\r?\n/)) {
        const line = rawLine.trim();
        if (line === "" || line.startsWith("#") || line.startsWith(";")) continue;
        const eq = line.indexOf("=");
        if (eq === -1) continue;
        const key = line.slice(0, eq);
        let value = line.slice(eq + 1);
        if (value.startsWith('"') && value.endsWith('"') && value.length >= 2) {
            value = value.slice(1, -1);
        }
        entries.set(key, value);
    }
    return entries;
}

// 下の 2 つの正規表現は、二重引用符を \x22 で書く。test/lint/identifiers_english.py は、正規表現の中の二重引用符を
// 文字列の始まりと読み、続く日本語の文字を識別子として数えるためである。
/** rules_dock.cpp の kContactHeadlines の行 (一覧の id と、見出しの定数の名前)。 */
function receiverRows(): { id: string; constant: string }[] {
    const source = readRepoFile("../../src-obs/ledger/rules_dock.cpp");
    const start = source.indexOf("const ContactHeadline kContactHeadlines[] = {");
    expect(start, "rules_dock.cpp に kContactHeadlines がない").toBeGreaterThanOrEqual(0);
    const block = source.slice(start, source.indexOf("};", start));
    const rows: { id: string; constant: string }[] = [];
    for (const m of block.matchAll(/\{\x22([a-z-]+)\x22, keys::(k\w+)\}/g)) {
        const [, id, constant] = m;
        if (id !== undefined && constant !== undefined) rows.push({ id, constant });
    }
    return rows;
}

/** text_ids.h の text::keys の定数の名前 -> キー。 */
function keyConstants(): Map<string, string> {
    const header = readRepoFile("../../src-obs/ledger/text_ids.h");
    const out = new Map<string, string>();
    for (const m of header.matchAll(/inline constexpr const char \*(k\w+) = \x22(cordmux\.[^\x22]+)\x22;/g)) {
        const [, name, key] = m;
        if (name !== undefined && key !== undefined) out.set(name, key);
    }
    return out;
}

describe("依存箇所の一覧と、受信側の見出しと、ini の比較", () => {
    it("一覧の id と、受信側の kContactHeadlines の行が過不足なく一致する", () => {
        const rows = receiverRows();
        expect(rows.length, "kContactHeadlines の行が読めない").toBeGreaterThan(0);
        const ledgerIds = CONTACT_LEDGER.map(e => e.id).sort();
        const receiverIds = rows.map(r => r.id).sort();
        expect(receiverIds).toEqual(ledgerIds);
    });

    it.each(["ja-JP", "en-US"] as const)("受信側の見出しのキーがすべて %s.ini にあり、値が空でない", locale => {
        const entries = readIniEntries(`../../src-obs/data/locale/${locale}.ini`);
        const constants = keyConstants();
        for (const row of receiverRows()) {
            const key = constants.get(row.constant);
            expect(key, `text_ids.h に ${row.constant} がない`).toBeDefined();
            if (key === undefined) continue;
            expect(entries.has(key), `${locale}.ini に ${key} がない`).toBe(true);
            expect(entries.get(key), `${locale}.ini の ${key} の値が空`).not.toBe("");
        }
    });

    it.each(["ja-JP", "en-US"] as const)("%s.ini に前の形の依存箇所のキー (cordmux.contact.*) がない", locale => {
        const entries = readIniEntries(`../../src-obs/data/locale/${locale}.ini`);
        const left = [...entries.keys()].filter(k => k.startsWith("cordmux.contact."));
        expect(left).toEqual([]);
    });
});
