// SPDX-License-Identifier: Apache-2.0

// 送信側と受信側で同じ値でなければならない定数を比べる。受信側 (C++) の値は、ヘッダーのファイルの定義の行から読む。
// - 送信側の時計が先に出うる誤差 (ページの原点の TimeTicks の誤差の上限と performance.now() の丸めと 1 us) と、受信側の
//   許す幅 (src-obs/pipeline/sender_clock.h の kSharedClockToleranceUs)。

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

import { pageClampUs, TICKS_ORIGIN_ERROR_MAX_US } from "../encode/ticks_origin";

function readHeader(relative: string): string {
    return readFileSync(fileURLToPath(new URL(relative, import.meta.url)), "utf-8");
}

/** `inline constexpr std::int64_t <name> = <式>;` の式を、数と + だけの式として計算する。 */
function constexprValue(text: string, name: string): number {
    const m = text.match(new RegExp(`inline constexpr std::int64_t ${name} = ([0-9+ ]+);`));
    if (m === null || m[1] === undefined) throw new Error(`${name} の定義が見つからない`);
    return m[1].split("+").reduce((sum, part) => sum + Number(part.trim()), 0);
}

describe("送信側と受信側の定数", () => {
    it("受信側が許す、送信側の時計が先に出る幅は、送信側の誤差の上限 + 丸め + 1 us と同じ", () => {
        const clock = readHeader("../../src-obs/pipeline/sender_clock.h");
        expect(constexprValue(clock, "kSharedClockToleranceUs")).toBe(TICKS_ORIGIN_ERROR_MAX_US + pageClampUs(false) + 1);
    });
});
