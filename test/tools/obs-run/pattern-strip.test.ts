/*
 * share/pattern.html のストリップは時計つきのストリップである (bit_strip.py の冒頭)。
 * ストリップの描き方は pattern.html (JavaScript) と bit_strip.py (Python。frames.py が読み、make_material.py と
 * make_sync_video.py が描く) の 2 か所にある。食い違うと、実際の共有のストリップを frames.py が「CRC が合わない」と
 * 読み捨て、全部のフレームが読めなくなる。そこで pattern.html の印 (BEGIN / END strip-drawing) の間の関数を
 * 取り出し、bit_strip.py のセルフテスト (python bit_strip.py --selftest) と同じ既知の値で確認する。
 * このテストから Python を起動して突き合わせないのは、vitest の実行に Python と numpy が要るようになるためである。
 * pattern.html を外部の .js に分けないのは、共有の中身が外部のファイルを読まない 1 ページと決めているためである
 * (pattern.html の冒頭)。
 */
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

type FillTarget = { fillStyle: string; fillRect(x: number, y: number, w: number, h: number): void };
type StripApi = {
    stripCrc8(bytes: number[]): number;
    stripDataBits(frame: number): number[];
    drawStrip(target: FillTarget, frame: number, x: number, y: number, u: number): void;
};

const HERE = dirname(fileURLToPath(import.meta.url));

function loadStripApi(): StripApi {
    const html = readFileSync(join(HERE, "share", "pattern.html"), "utf8");
    const begin = html.indexOf("// BEGIN strip-drawing");
    const end = html.indexOf("// END strip-drawing");
    if (begin < 0 || end < 0 || end < begin) throw new Error("pattern.html has no strip-drawing markers");
    const source = html.slice(begin, end);
    // 印の間は関数の宣言だけ (ページの canvas や DOM に触らない) なので、そのまま評価できる。
    return new Function(source + "\nreturn { stripCrc8, stripDataBits, drawStrip };")() as StripApi;
}

// fillRect を w x h の升目 (1 = 白) にコピーする最小の描き先。
class Raster implements FillTarget {
    fillStyle = "#000000";
    readonly cells: number[][];
    constructor(readonly width: number, readonly height: number) {
        this.cells = Array.from({ length: height }, () => new Array<number>(width).fill(-1));
    }
    fillRect(x: number, y: number, w: number, h: number): void {
        const value = this.fillStyle === "#ffffff" ? 1 : 0;
        for (let yy = y; yy < y + h; yy++) {
            for (let xx = x; xx < x + w; xx++) {
                const row = this.cells[yy];
                if (row !== undefined && xx >= 0 && xx < this.width) row[xx] = value;
            }
        }
    }
    rows(): string[] {
        return this.cells.map((row) => row.map((v) => (v === 1 ? "#" : v === 0 ? "." : " ")).join(""));
    }
}

function wordBits(word: number): number[] {
    const bits: number[] = [];
    for (let i = 31; i >= 0; i--) bits.push(Math.floor(word / 2 ** i) % 2);
    return bits;
}

describe("pattern.html のストリップは bit_strip.py と同じ", () => {
    const api = loadStripApi();

    it("CRC-8 の既知の値 (b\"123456789\" で 0xF4)", () => {
        expect(api.stripCrc8(Array.from("123456789", (c) => c.charCodeAt(0)))).toBe(0xf4);
    });

    it("データの 32 升は bit_strip.strip_word と同じ (bit_strip.py --selftest と同じ値)", () => {
        const known: Array<[number, number]> = [
            [0, 0x00000000],
            [1, 0x00000107],
            [1085089, 0x108ea1ac],
            [31752, 0x007c0866],
            [16777215, 0xffffff0f],
        ];
        for (const [frame, word] of known) {
            expect(api.stripDataBits(frame)).toEqual(wordBits(word));
        }
    });

    it("番号は 24 ビットで回る", () => {
        expect(api.stripDataBits(16777216 + 5)).toEqual(api.stripDataBits(5));
    });

    it("升 1 で描いたストリップは bit_strip.strip_mask(1085089, 1) と同じ画像", () => {
        const raster = new Raster(35, 6);
        api.drawStrip(raster, 1085089, 0, 0, 1);
        // python -c "import bit_strip; ..." で bit_strip.strip_mask(1085089, 1) を # と . にしたもの。
        expect(raster.rows()).toEqual([
            "...................................",
            ".#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.",
            "....#....#...###.#.#....##.#.##....",
            "....#....#...###.#.#....##.#.##....",
            ".#################################.",
            "...................................",
        ]);
    });

    it("位置と升の大きさを渡すと、その四角 (35 升 x 6 升) だけを塗る", () => {
        const raster = new Raster(80, 16);
        api.drawStrip(raster, 7, 3, 2, 2);
        const rows = raster.rows();
        expect(rows[1]).toBe(" ".repeat(80));
        expect(rows[14]).toBe(" ".repeat(80));
        for (let y = 2; y < 14; y++) {
            expect(rows[y]?.slice(0, 3)).toBe("   ");
            expect(rows[y]?.slice(73)).toBe("       ");
            expect(rows[y]?.slice(3, 73)).not.toContain(" ");
        }
        // 時計の行 (y = 2 + 2 .. 2 + 3) は左の静かの次の升から白で始まる。
        expect(rows[4]?.slice(3, 9)).toBe("..##..");
    });
});
