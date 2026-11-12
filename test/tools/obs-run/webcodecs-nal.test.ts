/*
 * 同じ設定の 2 つ目のインスタンスの SPS/PPS が 1 つ目とビット一致するかを確認するための、
 * test/tools/webcodecs-check の純関数 (Annex B から SPS/PPS を取り出す部分) のテスト。
 * 枝の判定は avcC のビット一致で行う。
 *
 * **Electron は起動しない。** ウィンドウの中でしか測れない部分 (エンコーダの所要時間) とは切り離し、
 * バイト列を読む部分だけをここで固定する。
 *
 * ファイルの場所が test/tools/obs-run なのは、vitest の対象がリポジトリ直下の vitest.config.ts で
 * "test/tools/obs-run/*.test.ts" に絞られているためである。読むのは 1 つ上の webcodecs-check/nal_pure.js。
 */
import { describe, expect, it } from "vitest";

import nal from "../webcodecs-check/nal_pure.js";

// 開始コード + NAL の 1 バイト目 (nal_unit_type) + 中身、を並べた Annex B を作る。
function annexB(units: Array<{ type: number; body: number[]; longStartCode?: boolean }>): Uint8Array {
    const out: number[] = [];
    for (const u of units) {
        if (u.longStartCode) out.push(0, 0, 0, 1);
        else out.push(0, 0, 1);
        // forbidden_zero_bit = 0、nal_ref_idc = 3 (0x60) に種別を足す。
        out.push(0x60 | u.type);
        out.push(...u.body);
    }
    return Uint8Array.from(out);
}

describe("scanAnnexB", () => {
    it("3 バイトの開始コードで切り分け、種別を読む", () => {
        const bytes = annexB([
            { type: nal.NAL_TYPE_SPS, body: [0x01, 0x02] },
            { type: nal.NAL_TYPE_PPS, body: [0x03] },
            { type: nal.NAL_TYPE_IDR, body: [0x04, 0x05, 0x06] },
        ]);
        const units = nal.scanAnnexB(bytes);
        expect(units.map((u: any) => u.type)).toEqual([nal.NAL_TYPE_SPS, nal.NAL_TYPE_PPS, nal.NAL_TYPE_IDR]);
        expect(units.map((u: any) => u.end - u.start)).toEqual([3, 2, 4]);
    });

    it("4 バイトの開始コードでも同じ切り分けになる (余分な 00 を前の NAL の末尾に残さない)", () => {
        const three = nal.scanAnnexB(
            annexB([
                { type: nal.NAL_TYPE_SPS, body: [0x01, 0x02] },
                { type: nal.NAL_TYPE_PPS, body: [0x03] },
            ]),
        );
        const four = nal.scanAnnexB(
            annexB([
                { type: nal.NAL_TYPE_SPS, body: [0x01, 0x02], longStartCode: true },
                { type: nal.NAL_TYPE_PPS, body: [0x03], longStartCode: true },
            ]),
        );
        expect(four.map((u: any) => u.end - u.start)).toEqual(three.map((u: any) => u.end - u.start));
    });

    it("開始コードが 1 つも無ければ空", () => {
        expect(nal.scanAnnexB(Uint8Array.from([0x01, 0x02, 0x03]))).toEqual([]);
    });
});

describe("extractParameterSets", () => {
    it("最初の SPS と PPS を 16 進で返し、開始コードは含めない", () => {
        const bytes = annexB([
            { type: nal.NAL_TYPE_SPS, body: [0xde, 0xad] },
            { type: nal.NAL_TYPE_PPS, body: [0xbe, 0xef] },
            { type: nal.NAL_TYPE_IDR, body: [0x00] },
        ]);
        const got = nal.extractParameterSets(bytes);
        // 0x60 | 7 = 0x67 (SPS)、0x60 | 8 = 0x68 (PPS)。
        expect(got.sps).toBe("67dead");
        expect(got.pps).toBe("68beef");
        expect(got.hasIdr).toBe(true);
        expect(got.unitCount).toBe(3);
    });

    it("SPS も PPS も無ければ null (「ない」と「違う」を混ぜない)", () => {
        const got = nal.extractParameterSets(annexB([{ type: 1, body: [0x00] }]));
        expect(got.sps).toBeNull();
        expect(got.pps).toBeNull();
        expect(got.hasIdr).toBe(false);
    });

    it("2 つ目以降の SPS は無視する (最初の 1 つだけを見る)", () => {
        const bytes = annexB([
            { type: nal.NAL_TYPE_SPS, body: [0x11] },
            { type: nal.NAL_TYPE_SPS, body: [0x22] },
        ]);
        expect(nal.extractParameterSets(bytes).sps).toBe("6711");
    });
});

describe("compareParameterSets", () => {
    const a = { sps: "6711", pps: "6822", hasIdr: true, unitCount: 3 };

    it("同じなら bothEqual が真", () => {
        const r = nal.compareParameterSets(a, { ...a });
        expect(r.spsEqual).toBe(true);
        expect(r.ppsEqual).toBe(true);
        expect(r.bothEqual).toBe(true);
    });

    it("1 バイトでも違えば偽 (照合はビット一致)", () => {
        const r = nal.compareParameterSets(a, { ...a, sps: "6712" });
        expect(r.spsEqual).toBe(false);
        expect(r.bothEqual).toBe(false);
    });

    it("片方が取れていなければ null (「比べられなかった」を「違った」と読ませない)", () => {
        const r = nal.compareParameterSets(a, { ...a, sps: null });
        expect(r.spsEqual).toBeNull();
        expect(r.bothEqual).toBe(false);
    });
});
