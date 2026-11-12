/*
 * フレームだけを止める通知ファイルについて、主プロセスの側の読み方
 * (frame_pause_file.js の readFramePauseState) を、Electron を起こさずに当てる。fs と時計は作り物を渡す。
 * 守る性質: 読むだけ (書き込みの関数を呼ばない)、ない場所は absent、読めない時は error (レンダラーが前の状態を
 * 保つ)、中身は先頭の上限までの文字列。
 */
import { createRequire } from "node:module";

import { describe, expect, it } from "vitest";

// main.js と同じ CommonJS のコンポーネントなので、require で読む (main.js が読むのと同じ形)。
const requireCjs = createRequire(import.meta.url);
const { readFramePauseState, FRAME_PAUSE_READ_LIMIT } = requireCjs("./frame_pause_file.js") as {
    readFramePauseState: (
        filePath: unknown,
        fsLike: { readFileSync(p: string): Uint8Array },
        now: () => number,
    ) => { state: string; text?: string; wallMs: number };
    FRAME_PAUSE_READ_LIMIT: number;
};

function fsError(code: string): Error {
    const e = new Error(code) as Error & { code: string };
    e.code = code;
    return e;
}

/** 読んだ場所を覚え、書き込みの関数が呼ばれたら投げる作り物の fs。 */
function fakeFs(files: Record<string, string | Error>) {
    const reads: string[] = [];
    const fs = {
        readFileSync(p: string): Uint8Array {
            reads.push(p);
            const v = files[p];
            if (v === undefined) throw fsError("ENOENT");
            if (v instanceof Error) throw v;
            return new TextEncoder().encode(v);
        },
        writeFileSync(): never {
            throw new Error("書いてはいけない");
        },
        unlinkSync(): never {
            throw new Error("消してはいけない");
        },
    };
    return { fs, reads };
}

describe("readFramePauseState (主プロセスが通知ファイルを読む)", () => {
    const now = () => 1_700_000_000_000;

    it("場所がない・空・空白だけなら off (ファイルに触れない)", () => {
        const { fs, reads } = fakeFs({});
        expect(readFramePauseState(undefined, fs, now)).toEqual({ state: "off", wallMs: now() });
        expect(readFramePauseState("", fs, now)).toEqual({ state: "off", wallMs: now() });
        expect(readFramePauseState("  ", fs, now)).toEqual({ state: "off", wallMs: now() });
        expect(reads).toEqual([]);
    });

    it("ないファイル (と、途中がない場所) は absent", () => {
        const { fs } = fakeFs({ "C:/w/notdir/pause": fsError("ENOTDIR") });
        expect(readFramePauseState("C:/w/pause", fs, now)).toEqual({ state: "absent", wallMs: now() });
        expect(readFramePauseState("C:/w/notdir/pause", fs, now)).toEqual({ state: "absent", wallMs: now() });
    });

    it("あれば present と中身 (場所の前後の空白は落として読む)", () => {
        const { fs, reads } = fakeFs({ "C:/w/pause": "1,2\n", "C:/w/empty": "" });
        expect(readFramePauseState(" C:/w/pause ", fs, now)).toEqual({ state: "present", text: "1,2\n", wallMs: now() });
        expect(readFramePauseState("C:/w/empty", fs, now)).toEqual({ state: "present", text: "", wallMs: now() });
        expect(reads).toEqual(["C:/w/pause", "C:/w/empty"]);
    });

    it("読めない (使われている・場所がフォルダ) は error", () => {
        const { fs } = fakeFs({ "C:/w/busy": fsError("EBUSY"), "C:/w/dir": fsError("EISDIR") });
        expect(readFramePauseState("C:/w/busy", fs, now).state).toBe("error");
        expect(readFramePauseState("C:/w/dir", fs, now).state).toBe("error");
    });

    it("中身は先頭の上限までしか読まない", () => {
        const { fs } = fakeFs({ "C:/w/big": "1".repeat(FRAME_PAUSE_READ_LIMIT + 100) });
        expect(readFramePauseState("C:/w/big", fs, now).text).toHaveLength(FRAME_PAUSE_READ_LIMIT);
    });
});
