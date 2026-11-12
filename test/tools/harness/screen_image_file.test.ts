/*
 * 画面のストリームに描く画像のファイルを主プロセスで読む口 (screen_image_file.js の readScreenImageFile) を、Electron を起こさずに
 * 当てる。fs は作り物を渡す。守る性質: 環境変数に書かれた場所だけを読む、ないファイルと大きすぎるファイルと読めないファイルを
 * 区別して返し投げない、読めたときは中身そのものを返す。
 */
import { createRequire } from "node:module";

import { describe, expect, it } from "vitest";

// main.js と同じ CommonJS のコンポーネントなので、require で読む (main.js が読むのと同じ形)。
const requireCjs = createRequire(import.meta.url);
const { readScreenImageFile, SCREEN_IMAGE_READ_LIMIT } = requireCjs("./screen_image_file.js") as {
    readScreenImageFile: (
        listValue: unknown,
        filePath: unknown,
        fsLike: { statSync(p: string): { size: number }; readFileSync(p: string): Uint8Array },
    ) => { state: string; bytes?: Uint8Array | number; message?: string };
    SCREEN_IMAGE_READ_LIMIT: number;
};

/** 場所 -> 中身の作り物の fs。sizes で大きさだけを偽れる。failOn に当たる場所は読むときに投げる。 */
function fakeFs(files: Record<string, Uint8Array>, sizes: Record<string, number> = {}, failOn: string | null = null) {
    const reads: string[] = [];
    const fs = {
        statSync(p: string): { size: number } {
            const body = files[p];
            if (body === undefined) throw Object.assign(new Error("ENOENT"), { code: "ENOENT" });
            return { size: sizes[p] ?? body.length };
        },
        readFileSync(p: string): Uint8Array {
            reads.push(p);
            if (p === failOn) throw new Error("EBUSY");
            const body = files[p];
            if (body === undefined) throw Object.assign(new Error("ENOENT"), { code: "ENOENT" });
            return body;
        },
    };
    return { fs, reads };
}

const PNG = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 1, 2, 3]);
const LIST = "C:/t/tile_tl.png, C:/t/tile tr.png ,,C:/t/missing.png";

describe("画面に描く画像のファイルの読み取り", () => {
    it("環境変数がなければ off で、何も読まない", () => {
        const f = fakeFs({ "C:/t/tile_tl.png": PNG });
        for (const list of [undefined, null, "", "  "]) {
            expect(readScreenImageFile(list, "C:/t/tile_tl.png", f.fs)).toEqual({ state: "off" });
        }
        expect(f.reads).toEqual([]);
    });

    it("環境変数の項目にない場所は読まない", () => {
        const f = fakeFs({ "C:/t/other.png": PNG });
        expect(readScreenImageFile(LIST, "C:/t/other.png", f.fs)).toEqual({ state: "rejected" });
        expect(readScreenImageFile(LIST, "", f.fs)).toEqual({ state: "rejected" });
        expect(readScreenImageFile(LIST, 3, f.fs)).toEqual({ state: "rejected" });
        expect(f.reads).toEqual([]);
    });

    it("項目にある場所は、前後の空白を落として読み、中身そのものを返す", () => {
        const f = fakeFs({ "C:/t/tile tr.png": PNG });
        const r = readScreenImageFile(LIST, " C:/t/tile tr.png ", f.fs);
        expect(r.state).toBe("present");
        expect([...(r.bytes as Uint8Array)]).toEqual([...PNG]);
    });

    it("ないファイル、大きすぎるファイル、読めないファイルを区別して返し、投げない", () => {
        const f = fakeFs({ "C:/t/tile_tl.png": PNG, "C:/t/tile tr.png": PNG }, { "C:/t/tile_tl.png": SCREEN_IMAGE_READ_LIMIT + 1 }, "C:/t/tile tr.png");
        expect(readScreenImageFile(LIST, "C:/t/missing.png", f.fs)).toEqual({ state: "absent" });
        expect(readScreenImageFile(LIST, "C:/t/tile_tl.png", f.fs)).toEqual({ state: "too-large", bytes: SCREEN_IMAGE_READ_LIMIT + 1 });
        expect(readScreenImageFile(LIST, "C:/t/tile tr.png", f.fs)).toEqual({ state: "error", message: "EBUSY" });
        expect(f.reads).toEqual(["C:/t/tile tr.png"]);
    });
});
