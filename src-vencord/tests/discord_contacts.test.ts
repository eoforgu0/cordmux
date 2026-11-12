// SPDX-License-Identifier: Apache-2.0

// 依存箇所の一覧 (ledger.ts の CONTACT_LEDGER) の述語を、test/tools/discord-bundle/fetch.mjs が保存した Discord の
// バンドル (BUNDLES_ROOT。git の管理外) で調べる。manifest に入っている build ごとに、すべての行の述語がちょうど
// 1 つのモジュールに一致することを確認する (pc-hook は例外。AMBIGUITY_ALLOWED)。バンドルがないときは
// it.skip で飛ばし、失敗にしない。.github/workflows/discord-contacts.yml が、このテストを週に 1 回実行する。

import { existsSync, readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

import type { LedgerRow } from "../../test/tools/discord-bundle/scan";
import { scanBuildDir } from "../../test/tools/discord-bundle/scan";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = path.resolve(HERE, "..", "..");
const BUNDLES_ROOT = path.join(REPO_ROOT, "_work", "discord-bundles");
const MANIFEST_PATH = path.join(REPO_ROOT, "test", "fixtures", "discord-bundles", "manifest.json");

// manifest.json は、1 つの build につき、ファイルの数、合計のサイズ、比べるための digest だけを持つ (ファイルごとの
// 一覧は BUNDLES_ROOT の <channel>/<build>/files.json にあり、git の管理外)。このテストは、manifest の channel と
// build からバンドルのフォルダーのパスを作るだけで、fileCount、totalBytes、digest は使わない。
interface ManifestEntry {
    fetchedAt: string;
    channel: string;
    build: string;
    fileCount: number;
    totalBytes: number;
    digest: string;
    /**
     * 404 のほかの理由で、取得に失敗したファイルが 1 つでもあった build。ファイルが欠けているかもしれないので、
     * このテストでは飛ばす (間違って失敗や成功にしない)。
     */
    incomplete?: boolean;
}

function loadManifest(): ManifestEntry[] {
    if (!existsSync(MANIFEST_PATH)) return [];
    const raw = readFileSync(MANIFEST_PATH, "utf8").trim();
    if (raw === "") return [];
    return JSON.parse(raw) as ManifestEntry[];
}

interface LedgerModuleShape {
    CONTACT_LEDGER: readonly LedgerRow[];
}

async function loadLedgerRows(): Promise<readonly LedgerRow[]> {
    const mod = (await import("../ledger")) as unknown as LedgerModuleShape;
    // 9 行そろっていなければ、すぐに失敗させる。適当な値で続けると、依存箇所が見つからなくなったことを早く伝える
    // ためのジョブ (discord-contacts.yml) が、自分で壊れても成功のままになる。
    if (!Array.isArray(mod.CONTACT_LEDGER) || mod.CONTACT_LEDGER.length !== 9) {
        throw new Error(
            `CONTACT_LEDGER は 9 行のはずが ${JSON.stringify(mod.CONTACT_LEDGER)} だった`,
        );
    }
    return mod.CONTACT_LEDGER;
}

const ledgerRows = await loadLedgerRows();
const manifest = loadManifest();

// 一致するモジュールの数は、ちょうど 1 つでなければならない。述語は、そのストアを 1 つに決める複数の名前を、すべて
// 持つことにしてある。絞っても 1 つにできない行だけを、ここに書いて例外にする。述語をゆるめずに、絞れなかったことを
// そのまま残す。
//   - pc-hook: グローバルの差し替えの前提を確認するだけなので、1 つ以上でよい (ledger.ts のコメント)。
const AMBIGUITY_ALLOWED: Readonly<Record<string, number>> = {
    "pc-hook": Number.POSITIVE_INFINITY,
};

describe("保存した Discord のバンドルで、依存箇所の一覧の述語を調べる", () => {
    it("依存箇所の一覧は 9 行そろっている (このテスト自体が壊れていないことの確認)", () => {
        expect(ledgerRows.length).toBe(9);
    });

    if (manifest.length === 0) {
        it.skip("バンドルがない (test/fixtures/discord-bundles/manifest.json が空。test/tools/discord-bundle/fetch.mjs を実行してください)", () => {});
        return;
    }

    for (const entry of manifest) {
        const buildDir = path.join(BUNDLES_ROOT, entry.channel, entry.build);
        const label = `${entry.channel}/${entry.build}`;

        if (!existsSync(buildDir)) {
            it.skip(`バンドルがない: ${label} (git の管理外の ${buildDir} がない)`, () => {});
            continue;
        }
        if (entry.incomplete === true) {
            it.skip(`バンドルが不完全: ${label} (fetch.mjs が、取得に失敗したファイルがあったと記録している。取得し直してください)`, () => {});
            continue;
        }

        it(
            `${label}: 依存箇所の一覧のすべての行の述語が、ちょうど 1 つのモジュールに一致する (壊れやすいと書いた行を除く)`,
            () => {
                const results = scanBuildDir(buildDir, ledgerRows);
                const misses: string[] = [];
                const ambiguous: string[] = [];
                for (const row of ledgerRows) {
                    const result = results.get(row.id);
                    const count = result?.matchCount ?? 0;
                    // 一覧の行と調べた結果を比べやすいように、一致した数をそのまま出力する
                    // (0 は見つからない、1 は 1 つに決まる、2 以上はあいまい)。
                    console.log(`[discord-contacts] ${label} ${row.id}: ${count}`);
                    if (count < 1) misses.push(row.id);
                    const allowed = AMBIGUITY_ALLOWED[row.id] ?? 1;
                    if (count > allowed) ambiguous.push(`${row.id} (${count} 件、上限 ${allowed})`);
                }
                expect(misses, `述語が一致しなかった依存箇所: ${misses.join(", ") || "(なし)"}`).toEqual([]);
                expect(ambiguous, `1 つに絞れていない依存箇所: ${ambiguous.join(", ") || "(なし)"}`).toEqual([]);
            },
            300000,
        );
    }
});
