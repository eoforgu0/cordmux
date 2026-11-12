/*
 * コピーした物に Discord のユーザー ID の形 (17-20 桁の数) が残っていないかを数える id-scan.mjs のテスト。
 * ツールは数そのものを出さない (ファイルと行の番号と個数だけ) ことも確認する。
 * テストデータの数はどれも偽のもの (人のアカウントの ID は書かない)。ファイルは作らず、バイト列を直接渡す。
 */
import { describe, expect, it } from "vitest";

import { decodeText, isIdBearingName, scanEntries, scanTextForIds } from "./id-scan.mjs";

const FAKE_ID = "12345678901234567";

describe("id-scan.mjs", () => {
    it("decodeText: BOM で UTF-16 (LE / BE) と UTF-8 を分け、NUL を含む物はテキストでないとみなす", () => {
        expect(decodeText(Buffer.from("abc " + FAKE_ID, "utf8"))).toBe("abc " + FAKE_ID);
        expect(decodeText(Buffer.concat([Buffer.from([0xef, 0xbb, 0xbf]), Buffer.from("あ", "utf8")]))).toBe("あ");
        expect(decodeText(Buffer.concat([Buffer.from([0xff, 0xfe]), Buffer.from("x" + FAKE_ID, "utf16le")]))).toBe("x" + FAKE_ID);
        const be = Buffer.from("y", "utf16le");
        be.swap16();
        expect(decodeText(Buffer.concat([Buffer.from([0xfe, 0xff]), be]))).toBe("y");
        expect(decodeText(Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x00, 0x01]))).toBeNull();
    });

    it("scanTextForIds: 17-20 桁の数の個数と行の番号だけを返す (数そのものは返さない)", () => {
        const text = ["no ids here 1234567890123456", "a " + FAKE_ID + " b", "21 digits 123456789012345678901", "two " + FAKE_ID + " and 12345678901234567890"].join("\r\n");
        const r = scanTextForIds(text);
        expect(r).toEqual({ count: 3, lines: [2, 4] });
        expect(JSON.stringify(r)).not.toContain(FAKE_ID);
        expect(scanTextForIds("")).toEqual({ count: 0, lines: [] });
    });

    it("isIdBearingName: Vencord の設定のコピー・Vesktop のウィンドウの PNG・cordmux の設定の保存先は名前で赤", () => {
        expect(isIdBearingName("C:\\out\\window3b\\vencord-settings-before.json")).toBe(true);
        expect(isIdBearingName("shot-t-check-window-capture-1-latency-reference.png")).toBe(true);
        expect(isIdBearingName("obs-33.0.1/config/obs-studio/plugin_config/cordmux/settings.json")).toBe(true);
        expect(isIdBearingName("shot-t-screenshot-window-3-people-after-slot-1.png")).toBe(false);
    });

    it("scanEntries: 数が見つかるか ID の入りうる名前があれば失敗。テキストでない物は数えずに並べる", () => {
        const clean = scanEntries([
            { file: "a.json", bytes: Buffer.from('{"x": 1}') },
            { file: "b.png", bytes: Buffer.from([0x89, 0x50, 0x00]) },
        ]);
        expect(clean).toEqual({ ok: true, scannedFiles: 2, hits: [], idBearingNames: [], skippedBinary: ["b.png"] });
        const dirty = scanEntries([{ file: "r.json", bytes: Buffer.from('{"v": "' + FAKE_ID + '"}') }, { file: "x-latency-reference-1.png", bytes: Buffer.from([0x00]) }]);
        expect(dirty.ok).toBe(false);
        expect(dirty.hits).toEqual([{ file: "r.json", count: 1, lines: [1] }]);
        expect(dirty.idBearingNames).toEqual(["x-latency-reference-1.png"]);
        expect(JSON.stringify(dirty)).not.toContain(FAKE_ID);
    });
});
