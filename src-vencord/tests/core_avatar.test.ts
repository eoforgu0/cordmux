// SPDX-License-Identifier: Apache-2.0

// core/avatar.ts のテスト。アバターは cdn.discordapp.com の ?size=64 の URL から取得し、Content-Type が PNG、WebP、
// JPEG のものだけを 256 KiB まで読み、PNG に変換して 32 KiB の上限に収めてから送る。fetch と OffscreenCanvas は Node
// にないので、core/avatar.ts は関数を受け取る形になっていて、テストではテスト用の関数を渡す。

import { describe, expect, it } from "vitest";

import {
    AVATAR_CANVAS_SIZE,
    AVATAR_MAX_BASE64_LENGTH,
    AVATAR_MAX_SOURCE_BYTES,
    AVATAR_SIZE_VALUE,
    fetchAvatarPng,
    normalizeAvatarUrl,
    normalizeContentType,
    type AvatarDeps,
    type AvatarFetchResult,
} from "../core/avatar";

// ---- テスト用の関数 -------------------------------------------------------

interface FakeDepsScript {
    /** fetchAvatar が返す値 (省略すると、1x1 の PNG に見立てた小さな本文を返す)。 */
    response?: Partial<AvatarFetchResult>;
    /** fetchAvatar が例外を投げる (通信が失敗したとき)。 */
    fetchThrows?: boolean;
    /** toPngBase64 が返す値 (省略すると、PNG に見立てた短い base64 を返す)。 */
    png?: string | null;
    /** toPngBase64 が例外を投げる (PNG への変換に失敗したとき)。 */
    encodeThrows?: boolean;
}

function createFakeDeps(script: FakeDepsScript = {}) {
    const fetchedUrls: string[] = [];
    const encodedTypes: string[] = [];
    const deps: AvatarDeps = {
        async fetchAvatar(url: string): Promise<AvatarFetchResult> {
            fetchedUrls.push(url);
            if (script.fetchThrows === true) throw new Error("fake fetch failure");
            // contentType は、指定しなかったときと null を指定したときを分けるので、?? ではなくキーがあるかで見る
            // (null も、ヘッダーがないときの値としてテストするため)。
            const hasContentType = script.response !== undefined && "contentType" in script.response;
            return {
                ok: script.response?.ok ?? true,
                contentType: hasContentType ? (script.response?.contentType ?? null) : "image/png",
                body: script.response?.body ?? new Uint8Array([137, 80, 78, 71]),
            };
        },
        async toPngBase64(_body: Uint8Array, contentType: string): Promise<string | null> {
            encodedTypes.push(contentType);
            if (script.encodeThrows === true) throw new Error("fake encode failure");
            return script.png === undefined ? "iVBORw0KGgo=" : script.png;
        },
    };
    return { deps, fetchedUrls, encodedTypes };
}

// ---- テスト ---------------------------------------------------------------

describe("AVATAR_CANVAS_SIZE (PNG に変換するときの canvas の大きさ)", () => {
    it("64 である (?size=64 とそろえる)", () => {
        expect(AVATAR_CANVAS_SIZE).toBe(64);
    });

    it("AVATAR_SIZE_VALUE (URL の ?size=) と数として一致する (2 つの定数が食い違わない)", () => {
        expect(String(AVATAR_CANVAS_SIZE)).toBe(AVATAR_SIZE_VALUE);
    });
});

describe("normalizeAvatarUrl: ホストを cdn.discordapp.com だけにし、?size=64 を付け直す", () => {
    it("UserStore が返した大きさ (size=128) を 64 に替える", () => {
        expect(normalizeAvatarUrl("https://cdn.discordapp.com/avatars/1/abc.webp?size=128")).toBe(
            "https://cdn.discordapp.com/avatars/1/abc.webp?size=64",
        );
    });

    it("クエリ文字列のない既定のアバターにも size=64 を付ける", () => {
        expect(normalizeAvatarUrl("https://cdn.discordapp.com/embed/avatars/3.png")).toBe(
            "https://cdn.discordapp.com/embed/avatars/3.png?size=64",
        );
    });

    it("size のほかのクエリ文字列とフラグメントは取り除く", () => {
        expect(normalizeAvatarUrl("https://cdn.discordapp.com/avatars/1/abc.png?size=512&quality=lossless#x")).toBe(
            "https://cdn.discordapp.com/avatars/1/abc.png?size=64",
        );
    });

    it("ホストが違えば通さない (似た名前も含む)", () => {
        expect(normalizeAvatarUrl("https://cdn.discordapp.com.example.invalid/avatars/1/abc.png")).toBeNull();
        expect(normalizeAvatarUrl("https://media.discordapp.net/avatars/1/abc.png")).toBeNull();
        expect(normalizeAvatarUrl("https://evil.invalid/avatars/1/abc.png")).toBeNull();
    });

    it("https でないもの、ポート番号が付いたもの、ユーザー情報が付いたものは通さない", () => {
        expect(normalizeAvatarUrl("http://cdn.discordapp.com/avatars/1/abc.png")).toBeNull();
        expect(normalizeAvatarUrl("https://cdn.discordapp.com:8443/avatars/1/abc.png")).toBeNull();
        expect(normalizeAvatarUrl("https://someone@cdn.discordapp.com/avatars/1/abc.png")).toBeNull();
    });

    it("URL として読めない文字列は通さない", () => {
        expect(normalizeAvatarUrl("")).toBeNull();
        expect(normalizeAvatarUrl("avatars/1/abc.png")).toBeNull();
    });
});

describe("normalizeContentType", () => {
    it("パラメーター付きの値から、メディアタイプだけを小文字で取り出す", () => {
        expect(normalizeContentType("Image/PNG; charset=binary")).toBe("image/png");
        expect(normalizeContentType("image/webp")).toBe("image/webp");
    });

    it("ヘッダーがないか空なら null を返す", () => {
        expect(normalizeContentType(null)).toBeNull();
        expect(normalizeContentType("  ")).toBeNull();
    });
});

describe("fetchAvatarPng: 通す条件と諦める条件", () => {
    it("PNG を取得して base64 で返し、取得する URL は ?size=64 になっている", async () => {
        const { deps, fetchedUrls, encodedTypes } = createFakeDeps();
        const result = await fetchAvatarPng(deps, "https://cdn.discordapp.com/avatars/1/abc.png?size=128");

        expect(result).toEqual({ ok: true, pngBase64: "iVBORw0KGgo=" });
        expect(fetchedUrls).toEqual(["https://cdn.discordapp.com/avatars/1/abc.png?size=64"]);
        expect(encodedTypes).toEqual(["image/png"]);
    });

    it("WebP と JPEG も PNG に変換して通す", async () => {
        for (const contentType of ["image/webp", "image/jpeg"]) {
            const { deps } = createFakeDeps({ response: { contentType } });
            const result = await fetchAvatarPng(deps, "https://cdn.discordapp.com/avatars/1/abc.webp");
            expect(result.ok).toBe(true);
        }
    });

    it("ホストが違えば fetch を 1 回も呼ばない (bad_url)", async () => {
        const { deps, fetchedUrls } = createFakeDeps();
        const result = await fetchAvatarPng(deps, "https://evil.invalid/avatars/1/abc.png");

        expect(result).toEqual({ ok: false, reason: "bad_url" });
        expect(fetchedUrls).toEqual([]);
    });

    it("通信が失敗したら fetch_failed", async () => {
        const { deps } = createFakeDeps({ fetchThrows: true });
        const result = await fetchAvatarPng(deps, "https://cdn.discordapp.com/avatars/1/abc.png");
        expect(result).toEqual({ ok: false, reason: "fetch_failed" });
    });

    it("HTTP が成功でなければ http_error", async () => {
        const { deps, encodedTypes } = createFakeDeps({ response: { ok: false } });
        const result = await fetchAvatarPng(deps, "https://cdn.discordapp.com/avatars/1/abc.png");
        expect(result).toEqual({ ok: false, reason: "http_error" });
        expect(encodedTypes).toEqual([]); // PNG への変換まで進まない。
    });

    it("許可した 3 つのほかの Content-Type は、変換せずに諦める", async () => {
        for (const contentType of ["image/gif", "image/svg+xml", "text/html", null]) {
            const { deps, encodedTypes } = createFakeDeps({ response: { contentType } });
            const result = await fetchAvatarPng(deps, "https://cdn.discordapp.com/avatars/1/abc.png");
            expect(result).toEqual({ ok: false, reason: "bad_content_type" });
            expect(encodedTypes).toEqual([]);
        }
    });

    it("256 KiB を超えるレスポンスの本文は、変換せずに諦める (ちょうど 256 KiB は通す)", async () => {
        const tooLarge = createFakeDeps({ response: { body: new Uint8Array(AVATAR_MAX_SOURCE_BYTES + 1) } });
        await expect(fetchAvatarPng(tooLarge.deps, "https://cdn.discordapp.com/avatars/1/abc.png")).resolves.toEqual({
            ok: false,
            reason: "source_too_large",
        });
        expect(tooLarge.encodedTypes).toEqual([]);

        const justFits = createFakeDeps({ response: { body: new Uint8Array(AVATAR_MAX_SOURCE_BYTES) } });
        await expect(fetchAvatarPng(justFits.deps, "https://cdn.discordapp.com/avatars/1/abc.png")).resolves.toEqual({
            ok: true,
            pngBase64: "iVBORw0KGgo=",
        });
    });

    it("PNG に変換できなければ encode_failed (null を返したときも、例外を投げたときも)", async () => {
        const { deps: nullDeps } = createFakeDeps({ png: null });
        await expect(fetchAvatarPng(nullDeps, "https://cdn.discordapp.com/avatars/1/abc.png")).resolves.toEqual({
            ok: false,
            reason: "encode_failed",
        });

        const { deps: throwDeps } = createFakeDeps({ encodeThrows: true });
        await expect(fetchAvatarPng(throwDeps, "https://cdn.discordapp.com/avatars/1/abc.png")).resolves.toEqual({
            ok: false,
            reason: "encode_failed",
        });
    });

    it("32 KiB を超える base64 は送らない (ちょうど 32 KiB は通す)", async () => {
        const tooLong = createFakeDeps({ png: "A".repeat(AVATAR_MAX_BASE64_LENGTH + 1) });
        await expect(fetchAvatarPng(tooLong.deps, "https://cdn.discordapp.com/avatars/1/abc.png")).resolves.toEqual({
            ok: false,
            reason: "png_too_large",
        });

        const exact = "A".repeat(AVATAR_MAX_BASE64_LENGTH);
        const justFits = createFakeDeps({ png: exact });
        await expect(fetchAvatarPng(justFits.deps, "https://cdn.discordapp.com/avatars/1/abc.png")).resolves.toEqual({
            ok: true,
            pngBase64: exact,
        });
    });
});
