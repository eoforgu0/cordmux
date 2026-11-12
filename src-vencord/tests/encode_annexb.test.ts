// SPDX-License-Identifier: Apache-2.0

// encode/annexb.ts の純粋関数のテスト。H.264 のアクセスユニット (Annex B) から SPS と PPS を読み取ることと、
// キーフレームに SPS と PPS がなければ、前に覚えた SPS と PPS を前に付け足すことを確認する。

import { describe, expect, it } from "vitest";

import {
    applyParameterSetInsurance,
    NO_PARAMETER_SETS,
    scanParameterSets,
    type ParameterSets,
} from "../encode/annexb";

// ---- テスト用の関数 -------------------------------------------------------

// NAL のヘッダー (1 バイト)。下位 5 ビットが種類で、上位は nal_ref_idc と forbidden_zero_bit である。
const SPS_HEADER = 0x67; // 種類 7 (SPS)
const PPS_HEADER = 0x68; // 種類 8 (PPS)
const IDR_HEADER = 0x65; // 種類 5 (IDR のスライス)
const SLICE_HEADER = 0x41; // 種類 1 (IDR でないスライス)

/** NAL を 4 バイトの開始コード付きで並べる。 */
function annexB(...nals: number[][]): Uint8Array {
    const out: number[] = [];
    for (const nal of nals) out.push(0, 0, 0, 1, ...nal);
    return new Uint8Array(out);
}

/** NAL を 3 バイトの開始コードで並べる (エンコーダは、3 バイトと 4 バイトのどちらも出すことがある)。 */
function annexBShort(...nals: number[][]): Uint8Array {
    const out: number[] = [];
    for (const nal of nals) out.push(0, 0, 1, ...nal);
    return new Uint8Array(out);
}

const SPS = [SPS_HEADER, 0x64, 0x00, 0x2a, 0xac];
const PPS = [PPS_HEADER, 0xee, 0x3c, 0x80];
const IDR = [IDR_HEADER, 0x88, 0x84, 0x00];

// ---- 読み取り -------------------------------------------------------------

describe("scanParameterSets", () => {
    it("4 バイトの開始コードから SPS と PPS を取り出す", () => {
        const sets = scanParameterSets(annexB(SPS, PPS, IDR));
        expect([...(sets.sps ?? [])]).toEqual(SPS);
        expect([...(sets.pps ?? [])]).toEqual(PPS);
    });

    it("3 バイトの開始コードでも取り出す", () => {
        const sets = scanParameterSets(annexBShort(SPS, PPS, IDR));
        expect([...(sets.sps ?? [])]).toEqual(SPS);
        expect([...(sets.pps ?? [])]).toEqual(PPS);
    });

    it("3 バイトと 4 バイトが混ざっていても取り出す", () => {
        const mixed = new Uint8Array([0, 0, 1, ...SPS, 0, 0, 0, 1, ...PPS, 0, 0, 1, ...IDR]);
        const sets = scanParameterSets(mixed);
        expect([...(sets.sps ?? [])]).toEqual(SPS);
        expect([...(sets.pps ?? [])]).toEqual(PPS);
    });

    it("SPS と PPS がないアクセスユニットでは、どちらも undefined", () => {
        const sets = scanParameterSets(annexB(IDR));
        expect(sets.sps).toBeUndefined();
        expect(sets.pps).toBeUndefined();
    });

    it("末尾の 0 は、覚える SPS と PPS に含めない", () => {
        const withTrailingZeros = new Uint8Array([0, 0, 0, 1, ...SPS, 0, 0, 0, 0, 0, 1, ...IDR]);
        const sets = scanParameterSets(withTrailingZeros);
        expect([...(sets.sps ?? [])]).toEqual(SPS);
    });

    it("同じ種類が 2 つあるときは、最後のものを使う", () => {
        const second = [SPS_HEADER, 0x01, 0x02];
        const sets = scanParameterSets(annexB(SPS, second, IDR));
        expect([...(sets.sps ?? [])]).toEqual(second);
    });

    it("開始コードが 1 つもなければ何も取れない (例外も投げない)", () => {
        const sets = scanParameterSets(new Uint8Array([1, 2, 3]));
        expect(sets.sps).toBeUndefined();
        expect(sets.pps).toBeUndefined();
    });

    it("空のバイト列でも例外を投げない", () => {
        const sets = scanParameterSets(new Uint8Array(0));
        expect(sets.sps).toBeUndefined();
    });
});

// ---- SPS と PPS の付け足し ------------------------------------------------

describe("applyParameterSetInsurance", () => {
    it("キーフレームに SPS と PPS が入っていれば何も付けず、覚えた値だけを更新する", () => {
        const au = annexB(SPS, PPS, IDR);
        const result = applyParameterSetInsurance(au, true, NO_PARAMETER_SETS);
        expect(result.prepended).toBe(false);
        expect(result.missing).toBe(false);
        expect(result.payload).toBe(au); // 同じオブジェクトをそのまま返す (コピーを作らない)
        expect([...(result.remembered.sps ?? [])]).toEqual(SPS);
        expect([...(result.remembered.pps ?? [])]).toEqual(PPS);
    });

    it("キーフレームでないフレームは、調べずにそのまま返す", () => {
        const au = annexB(SPS, PPS, [SLICE_HEADER, 0x11]);
        const result = applyParameterSetInsurance(au, false, NO_PARAMETER_SETS);
        expect(result.prepended).toBe(false);
        expect(result.payload).toBe(au);
        expect(result.remembered).toBe(NO_PARAMETER_SETS); // 覚えた値は変わらない
    });

    it("SPS と PPS のない IDR には、覚えた値を前に付け、付け足した回数として数えられる形で返す", () => {
        const first = applyParameterSetInsurance(annexB(SPS, PPS, IDR), true, NO_PARAMETER_SETS);
        const bare = annexB(IDR);
        const second = applyParameterSetInsurance(bare, true, first.remembered);

        expect(second.prepended).toBe(true);
        expect(second.missing).toBe(false);
        expect([...second.payload]).toEqual([0, 0, 0, 1, ...SPS, 0, 0, 0, 1, ...PPS, ...bare]);
        // 付けた後も、覚えた値は同じ (中身は変わっていない)。
        expect([...(second.remembered.sps ?? [])]).toEqual(SPS);
    });

    it("片方だけが欠けていても、SPS と PPS の両方を前に付ける", () => {
        const first = applyParameterSetInsurance(annexB(SPS, PPS, IDR), true, NO_PARAMETER_SETS);
        const onlyPps = annexB(PPS, IDR);
        const second = applyParameterSetInsurance(onlyPps, true, first.remembered);

        expect(second.prepended).toBe(true);
        expect([...second.payload]).toEqual([0, 0, 0, 1, ...SPS, 0, 0, 0, 1, ...PPS, ...onlyPps]);
    });

    it("覚えた値がまだないときに、最初の IDR に SPS と PPS がなければ、missing を立ててそのまま通す", () => {
        const bare = annexB(IDR);
        const result = applyParameterSetInsurance(bare, true, NO_PARAMETER_SETS);
        expect(result.prepended).toBe(false);
        expect(result.missing).toBe(true);
        expect(result.payload).toBe(bare);
    });

    it("覚えたのが SPS だけで、PPS が一度も届いていなければ missing にする (片方だけを付けない)", () => {
        const spsOnly: ParameterSets = { sps: new Uint8Array(SPS), pps: undefined };
        const result = applyParameterSetInsurance(annexB(IDR), true, spsOnly);
        expect(result.prepended).toBe(false);
        expect(result.missing).toBe(true);
    });

    it("前に付けたバイト列を読み直すと、元と同じ SPS と PPS が読める (受信側が avcC を作るのに使う)", () => {
        const first = applyParameterSetInsurance(annexB(SPS, PPS, IDR), true, NO_PARAMETER_SETS);
        const second = applyParameterSetInsurance(annexB(IDR), true, first.remembered);
        const rescanned = scanParameterSets(second.payload);
        expect([...(rescanned.sps ?? [])]).toEqual(SPS);
        expect([...(rescanned.pps ?? [])]).toEqual(PPS);
    });
});
