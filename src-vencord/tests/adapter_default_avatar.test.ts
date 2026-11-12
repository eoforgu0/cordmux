// SPDX-License-Identifier: Apache-2.0

// adapter/default_avatar.ts (既定のアバターの番号と CDN の URL) のテスト。新しいユーザー名の仕組みで、既定の
// アバターを使っている人を架空の userId で作り、組み立てた URL が、core/avatar.ts のホストの制限を広げずに通ることも
// 確認する。番号の決め方は、Discord の開発者向けドキュメントの Reference にある CDN Endpoints の
// 「Default User Avatar」に書かれている。

import { describe, expect, it } from "vitest";

import { avatarUrlOfUser, defaultAvatarIndex, defaultAvatarUrl } from "../adapter/default_avatar";
import { fetchAvatarPng, normalizeAvatarUrl, type AvatarDeps } from "../core/avatar";

/*
 * 架空の userId (snowflake の形の、実在しない値)。期待する番号は `(userId >> 22) % 6` を BigInt で計算した値で、
 * 下の最初のテストが同じ式で確認する。下位 22 ビットだけが違う 2 つは同じ番号になるので、番号が分かれるように、
 * 22 ビットより上で値を変えてある。
 */
const USER_INDEX_4 = "100000000020000000";
const USER_INDEX_2 = "100000000010000000";
const USER_INDEX_0 = "100000000000000001";

/** 新しいユーザー名の仕組みで、既定のアバターを使っている 2 人。 */
const DEFAULT_AVATAR_USERS = [
    { userId: USER_INDEX_4, index: 4 },
    { userId: USER_INDEX_2, index: 2 },
] as const;

describe("defaultAvatarIndex: 新しいユーザー名の仕組み ((userId >> 22) % 6)", () => {
    it("架空の 2 人の番号を、BigInt で直接計算した値と同じに返す", () => {
        for (const { userId, index } of DEFAULT_AVATAR_USERS) {
            expect(defaultAvatarIndex(userId, "0")).toBe(index);
            expect(Number((BigInt(userId) >> 22n) % 6n)).toBe(index); // 期待する値を、式で直接確認する。
        }
        expect(defaultAvatarIndex(USER_INDEX_0, "0")).toBe(0);
        expect(Number((BigInt(USER_INDEX_0) >> 22n) % 6n)).toBe(0);
    });

    it("discriminator が \"0\" / \"0000\" / 空 / ない / null なら userId から決める", () => {
        for (const discriminator of ["0", "0000", "", undefined, null]) {
            expect(defaultAvatarIndex(USER_INDEX_4, discriminator)).toBe(4);
        }
    });

    it("数字でない discriminator も userId から決める (旧来の番号として読まない)", () => {
        expect(defaultAvatarIndex(USER_INDEX_4, "abc")).toBe(4);
        // " 12" を旧来の番号として読むと 12 % 5 = 2 になる。
        expect(defaultAvatarIndex(USER_INDEX_4, " 12")).toBe(4);
    });

    it("番号は 0 から 5 に収まり、6 通りすべてが出る (>> 22 の後で割っている)", () => {
        // 下位 22 ビットは時刻のほかのフィールドなので、番号を変えるのは 22 ビットより上だけである。
        const seen = new Set<number>();
        for (let k = 0n; k < 12n; k += 1n) {
            const index = defaultAvatarIndex(String(k << 22n), "0");
            expect(index).not.toBeNull();
            seen.add(index as number);
        }
        expect([...seen].sort()).toEqual([0, 1, 2, 3, 4, 5]);
        // 下位 22 ビットだけが違う 2 つは同じ番号。
        expect(defaultAvatarIndex(String((7n << 22n) + 4194303n), "0")).toBe(defaultAvatarIndex(String(7n << 22n), "0"));
    });
});

describe("defaultAvatarIndex: 旧来の discriminator (discriminator % 5)", () => {
    it("1 以上の discriminator は % 5 で決め、userId は見ない", () => {
        // userId から決めると USER_INDEX_2 は 2、USER_INDEX_0 は 0。どの組もそれと違う値を期待する。
        expect(defaultAvatarIndex(USER_INDEX_2, "1234")).toBe(4);
        expect(defaultAvatarIndex(USER_INDEX_2, "0005")).toBe(0);
        expect(defaultAvatarIndex(USER_INDEX_2, "0001")).toBe(1);
        expect(defaultAvatarIndex(USER_INDEX_2, "9999")).toBe(4);
        expect(defaultAvatarIndex(USER_INDEX_0, "1337")).toBe(2);
    });
});

describe("defaultAvatarIndex: userId が snowflake として読めない", () => {
    it("空・空白・符号・16 進・小数は null (BigInt が通してしまう形も含む)", () => {
        for (const userId of ["", " 12 ", "-1", "0x10", "1.5", "abc", "12a"]) {
            expect(defaultAvatarIndex(userId, "0")).toBeNull();
            expect(defaultAvatarIndex(userId, "1234")).toBeNull();
        }
    });
});

describe("defaultAvatarUrl: CDN の embed/avatars の URL", () => {
    it("架空の 2 人の URL を組み立てる", () => {
        expect(defaultAvatarUrl(USER_INDEX_4, "0")).toBe("https://cdn.discordapp.com/embed/avatars/4.png");
        expect(defaultAvatarUrl(USER_INDEX_2, "0")).toBe("https://cdn.discordapp.com/embed/avatars/2.png");
    });

    it("userId が読めなければ、URL を作らずに null を返す", () => {
        expect(defaultAvatarUrl("", "0")).toBeNull();
    });

    it("組み立てた URL は、アバターのホストの制限をそのまま通る (?size=64 が付く)", () => {
        for (const { userId, index } of DEFAULT_AVATAR_USERS) {
            const url = defaultAvatarUrl(userId, "0");
            expect(url).not.toBeNull();
            expect(normalizeAvatarUrl(url as string)).toBe(`https://cdn.discordapp.com/embed/avatars/${index}.png?size=64`);
        }
    });

    it("getAvatarURL() が既定のアバターに返す同梱の相対 URL は、ホストの制限を広げていないので bad_url になる", async () => {
        const fetched: string[] = [];
        const deps: AvatarDeps = {
            async fetchAvatar(url: string) {
                fetched.push(url);
                return { ok: true, contentType: "image/png", body: new Uint8Array([137, 80, 78, 71]) };
            },
            async toPngBase64() {
                return "iVBORw0KGgo=";
            },
        };

        expect(await fetchAvatarPng(deps, "/assets/0123456789abcdef.png")).toEqual({ ok: false, reason: "bad_url" });
        expect(await fetchAvatarPng(deps, "https://discord.com/assets/0123456789abcdef.png")).toEqual({
            ok: false,
            reason: "bad_url",
        });
        expect(fetched).toEqual([]);

        // defaultAvatarUrl() の URL は、取得するところまで進む。
        const url = defaultAvatarUrl(USER_INDEX_4, "0") as string;
        expect(await fetchAvatarPng(deps, url)).toEqual({ ok: true, pngBase64: "iVBORw0KGgo=" });
        expect(fetched).toEqual(["https://cdn.discordapp.com/embed/avatars/4.png?size=64"]);
    });
});

describe("avatarUrlOfUser: 自分のアバターの人は getAvatarURL()、既定のアバターの人は CDN の URL", () => {
    const OWN_URL = "https://cdn.discordapp.com/avatars/100000000020000000/abc.png?size=128";

    /** getAvatarURL() の呼び出しを数える、架空のユーザーのオブジェクト。 */
    function fakeUser(fields: Record<string, unknown>, result: unknown) {
        const calls: unknown[][] = [];
        const user = {
            ...fields,
            getAvatarURL(...args: unknown[]) {
                calls.push(args);
                return result;
            },
        };
        return { user, calls };
    }

    it("avatar が文字列の人は getAvatarURL() の値をそのまま返す", () => {
        const { user, calls } = fakeUser({ avatar: "abc", discriminator: "0" }, OWN_URL);
        expect(avatarUrlOfUser(USER_INDEX_4, user)).toBe(OWN_URL);
        expect(calls).toEqual([[undefined, 128, false]]);
    });

    it("avatar が null か空の人は、getAvatarURL() を呼ばずに既定のアバターの CDN の URL を返す", () => {
        for (const avatar of [null, ""]) {
            const { user, calls } = fakeUser({ avatar, discriminator: "0" }, "/assets/0123456789abcdef.png");
            expect(avatarUrlOfUser(USER_INDEX_4, user)).toBe("https://cdn.discordapp.com/embed/avatars/4.png");
            expect(calls).toEqual([]);
        }
    });

    it("avatar が null で discriminator が 1 以上の人は、旧来の番号の URL を返す", () => {
        const { user } = fakeUser({ avatar: null, discriminator: "1234" }, OWN_URL);
        expect(avatarUrlOfUser(USER_INDEX_2, user)).toBe("https://cdn.discordapp.com/embed/avatars/4.png");
    });

    it("avatar の型が文字列でない値 (数値など) の人も、既定のアバターの人として扱う", () => {
        const { user, calls } = fakeUser({ avatar: 0, discriminator: "0" }, OWN_URL);
        expect(avatarUrlOfUser(USER_INDEX_2, user)).toBe("https://cdn.discordapp.com/embed/avatars/2.png");
        expect(calls).toEqual([]);
    });

    it("avatar のフィールドがない (Discord の更新で消えた) ときは、既定のアバターと取り違えず getAvatarURL() を使う", () => {
        const { user, calls } = fakeUser({ discriminator: "0" }, OWN_URL);
        expect(avatarUrlOfUser(USER_INDEX_4, user)).toBe(OWN_URL);
        expect(calls.length).toBe(1);
    });

    it("既定のアバターの人でも userId が数字でなければ、URL を作らず null を返す", () => {
        const { user } = fakeUser({ avatar: null, discriminator: "0" }, OWN_URL);
        expect(avatarUrlOfUser("abc", user)).toBeNull();
    });

    it("getAvatarURL() が文字列でない値か空の文字列を返したときと、投げたときは null を返す", () => {
        for (const result of [undefined, null, 5, ""]) {
            expect(avatarUrlOfUser(USER_INDEX_4, fakeUser({ avatar: "abc" }, result).user)).toBeNull();
        }
        const throwing = {
            avatar: "abc",
            getAvatarURL() {
                throw new Error("boom");
            },
        };
        expect(avatarUrlOfUser(USER_INDEX_4, throwing)).toBeNull();
    });
});
