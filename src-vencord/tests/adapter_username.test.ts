// SPDX-License-Identifier: Apache-2.0

// adapter/username.ts のテスト。Discord で同じ名前の人を見分けるのに使う、1 人に 1 つのユーザー名を作る。読めない
// ときは適当な値で埋めずに null を返す。名前は架空の値で、実在のアカウントは使わない。

import { describe, expect, it } from "vitest";

import { uniqueUsername } from "../adapter/username";

describe("uniqueUsername: 新しいユーザー名の仕組み (discriminator が 0)", () => {
    it("username をそのまま返す", () => {
        expect(uniqueUsername("hanako_1", "0")).toBe("hanako_1");
    });

    it("discriminator が \"0000\" / 空 / ない / null / 数字以外でも username をそのまま返す (default_avatar.ts と同じ見分け方)", () => {
        for (const discriminator of ["0000", "", undefined, null, "abc", 0]) {
            expect(uniqueUsername("hanako_1", discriminator)).toBe("hanako_1");
        }
    });
});

describe("uniqueUsername: 旧来の 4 桁の discriminator を持つ人", () => {
    it("username#discriminator を返す (username だけでは 1 人に決まらない)", () => {
        expect(uniqueUsername("taro", "1234")).toBe("taro#1234");
        expect(uniqueUsername("taro", "0042")).toBe("taro#0042");
    });
});

describe("uniqueUsername: 読めないとき", () => {
    it("username が空か、文字列でないか、ないなら null を返す (受信側にはキーを送らない)", () => {
        for (const username of ["", undefined, null, 5, {}]) {
            expect(uniqueUsername(username, "0")).toBeNull();
        }
    });
});
