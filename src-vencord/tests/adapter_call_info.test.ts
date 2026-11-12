// SPDX-License-Identifier: Apache-2.0

// adapter/call_info.ts のテスト。通話の名前の作り方と、サブアカウントが参加できるかの決め方を確認する。

import { describe, expect, it } from "vitest";

import { callNameOf, ChannelKind, joinableOf, type JoinableInput } from "../adapter/call_info";

describe("callNameOf", () => {
    it("サーバーのチャンネルは「チャンネルの名前 / サーバーの名前」にする", () => {
        expect(callNameOf({ channelName: "General", guildName: "Server A", recipientNames: [] })).toBe("General / Server A");
    });

    it("サーバーの名前が読めないときは、チャンネルの名前だけにする", () => {
        expect(callNameOf({ channelName: "General", guildName: null, recipientNames: [] })).toBe("General");
        expect(callNameOf({ channelName: "General", guildName: "  ", recipientNames: [] })).toBe("General");
    });

    it("名前のない DM とグループ DM は、相手の表示名をつなぐ", () => {
        expect(callNameOf({ channelName: "", guildName: null, recipientNames: ["Ann", "Bob"] })).toBe("Ann, Bob");
        expect(callNameOf({ channelName: "", guildName: null, recipientNames: ["Ann", " "] })).toBe("Ann");
    });

    it("どれも読めないときは null を返す", () => {
        expect(callNameOf({ channelName: "", guildName: null, recipientNames: [] })).toBeNull();
        expect(callNameOf({ channelName: " ", guildName: "Server A", recipientNames: [] })).toBeNull();
    });
});

describe("joinableOf", () => {
    const base: JoinableInput = {
        kind: ChannelKind.GuildVoice,
        canConnect: true,
        userLimit: 0,
        occupants: 3,
        canBypassLimit: false,
    };

    it("接続の権限があり、人数の上限がなければ参加できる", () => {
        expect(joinableOf(base)).toBe(true);
    });

    it("接続の権限がなければ参加できない", () => {
        expect(joinableOf({ ...base, canConnect: false })).toBe(false);
    });

    it("人数の上限に達していれば参加できない。上限を超えて入れる権限があれば参加できる", () => {
        expect(joinableOf({ ...base, userLimit: 3, occupants: 3 })).toBe(false);
        expect(joinableOf({ ...base, userLimit: 3, occupants: 2 })).toBe(true);
        expect(joinableOf({ ...base, userLimit: 3, occupants: 3, canBypassLimit: true })).toBe(true);
    });

    it("ステージは人数の上限を見ない", () => {
        expect(joinableOf({ ...base, kind: ChannelKind.GuildStageVoice, userLimit: 1, occupants: 5 })).toBe(true);
    });

    it("DM とグループ DM は、権限を見ずに参加できる", () => {
        expect(joinableOf({ ...base, kind: ChannelKind.Dm, canConnect: null })).toBe(true);
        expect(joinableOf({ ...base, kind: ChannelKind.GroupDm, canConnect: null })).toBe(true);
    });

    it("チャンネルを読めないとき、通話を持たない種類のとき、権限を読めないときは null を返す", () => {
        expect(joinableOf({ ...base, kind: null })).toBeNull();
        expect(joinableOf({ ...base, kind: 0 })).toBeNull();
        expect(joinableOf({ ...base, canConnect: null })).toBeNull();
    });
});
