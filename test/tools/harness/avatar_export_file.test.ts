/*
 * 送ったアバターの PNG を書き出す口 (avatar_export_file.js の exportAvatarPng) を、Electron を起こさずに当てる。fs は
 * 作り物を渡す。守る性質: 場所が渡されていなければ何も作らない、userId ごとに送ったバイト列そのものを書く、ファイル名に
 * できない userId ではフォルダーの外に書かない、書けなければ error を返して投げない。
 */
import path from "node:path";
import { createRequire } from "node:module";

import { describe, expect, it } from "vitest";

// main.js と同じ CommonJS のコンポーネントなので、require で読む (main.js が読むのと同じ形)。
const requireCjs = createRequire(import.meta.url);
const { exportAvatarPng } = requireCjs("./avatar_export_file.js") as {
    exportAvatarPng: (
        dir: unknown,
        userId: unknown,
        base64: unknown,
        fsLike: { mkdirSync(p: string, o: { recursive: boolean }): void; writeFileSync(p: string, b: Uint8Array): void },
    ) => { state: string; file?: string; bytes?: number; reason?: string; message?: string };
};

/** 作ったフォルダーと書いたファイルを覚える作り物の fs。failOn に当たる場所では投げる。 */
function fakeFs(failOn: string | null = null) {
    const dirs: string[] = [];
    const files = new Map<string, Uint8Array>();
    const fs = {
        mkdirSync(p: string): void {
            if (p === failOn) throw new Error("EACCES");
            dirs.push(p);
        },
        writeFileSync(p: string, b: Uint8Array): void {
            if (p === failOn) throw new Error("EACCES");
            files.set(p, new Uint8Array(b));
        },
    };
    return { fs, dirs, files };
}

const PNG_BYTES = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 1, 2, 3]);
const PNG_BASE64 = Buffer.from(PNG_BYTES).toString("base64");

describe("送ったアバターの PNG の書き出し", () => {
    it("場所が渡されていなければ、フォルダーもファイルも作らない", () => {
        for (const dir of [undefined, null, "", "   "]) {
            const f = fakeFs();
            expect(exportAvatarPng(dir, "harness-voice-2", PNG_BASE64, f.fs)).toEqual({ state: "off" });
            expect(f.dirs).toEqual([]);
            expect(f.files.size).toBe(0);
        }
    });

    it("userId ごとに、送った base64 をデコードしたバイト列そのものを <場所>/<userId>.png に書く", () => {
        const f = fakeFs();
        const dir = "C:/work/out/c8/avatars";
        const r = exportAvatarPng(" " + dir + " ", "harness-voice-4", PNG_BASE64, f.fs);
        const file = path.join(dir, "harness-voice-4.png");
        expect(r).toEqual({ state: "written", file, bytes: PNG_BYTES.length });
        expect(f.dirs).toEqual([dir]);
        expect([...f.files.get(file)!]).toEqual([...PNG_BYTES]);
    });

    it("ファイル名にできない userId と、空の中身は書かない", () => {
        const f = fakeFs();
        for (const userId of ["../x", "a/b", "a\\b", "", "x".repeat(65), 12]) {
            expect(exportAvatarPng("C:/d", userId, PNG_BASE64, f.fs)).toEqual({ state: "rejected", reason: "user_id" });
        }
        expect(exportAvatarPng("C:/d", "harness-screen-1", "", f.fs)).toEqual({ state: "rejected", reason: "empty" });
        expect(f.dirs).toEqual([]);
        expect(f.files.size).toBe(0);
    });

    it("書けなければ投げずに error を返す", () => {
        const f = fakeFs("C:/locked");
        const r = exportAvatarPng("C:/locked", "harness-screen-1", PNG_BASE64, f.fs);
        expect(r.state).toBe("error");
        expect(r.message).toContain("EACCES");
    });
});
