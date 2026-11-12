// SPDX-License-Identifier: Apache-2.0

// version.ts の CORDMUX_BUILD_ID のテスト。確認するのは、+ の前が src-obs/buildspec.json の version と同じことだけで
// ある。+ の後 (リポジトリでは dev、配布用の zip では短いコミット) は CI が書き換えるので、英数字であることだけを
// 確認する。そのため、CI が書き換えた後にこのテストを実行しても失敗せず、buildspec.json の version を変えて、
// version.ts を変え忘れたときだけ失敗する。

import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

import { CORDMUX_BUILD_ID } from "../version";

const here = dirname(fileURLToPath(import.meta.url));

describe("送信側のバージョン (version.ts)", () => {
    it("+ の前の数字が、buildspec.json の version と同じ", () => {
        const buildspec = JSON.parse(readFileSync(join(here, "../../src-obs/buildspec.json"), "utf8")) as {
            version?: unknown;
        };
        expect(typeof buildspec.version).toBe("string");
        const [numbers, build] = CORDMUX_BUILD_ID.split("+");
        expect(numbers).toBe(buildspec.version);
        expect(build).toMatch(/^[0-9A-Za-z]+$/);
    });

    it("CI が書き換える 1 行の形を保っている", () => {
        const source = readFileSync(join(here, "../version.ts"), "utf8");
        const lines = source.split("\n").filter((line) => line.startsWith("export const CORDMUX_BUILD_ID = "));
        expect(lines).toHaveLength(1);
        expect(lines[0]).toMatch(/^export const CORDMUX_BUILD_ID = "[0-9]+\.[0-9]+\.[0-9]+\+[0-9A-Za-z]+";$/);
    });

    it("hello の version は version.ts から読む (index.ts にバージョンを直接書かない)", () => {
        // index.ts は Vencord を import するので、vitest から読み込めない。ソースの文字列で確認する。
        const source = readFileSync(join(here, "../index.ts"), "utf8");
        expect(source).toMatch(/^import \{ CORDMUX_BUILD_ID \} from "\.\/version";$/m);
        expect(source).toMatch(/^const WIRE_CLIENT_VERSION = CORDMUX_BUILD_ID;$/m);
        expect(source).toMatch(/^\s+version: WIRE_CLIENT_VERSION,$/m);
    });
});
