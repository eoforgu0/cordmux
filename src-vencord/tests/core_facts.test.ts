// SPDX-License-Identifier: Apache-2.0

// core/facts.ts の buildFacts()、buildProfiles()、avatarFetchTargets()、avatarFetchesToStart()、AvatarOutbox の
// テスト。facts と profiles の中身 (avatar_key と bot)、アバターを取得する相手 (roster の全員)、アバターを送るのが
// avatar_key の変わった人だけで 1 秒の人数に上限があること、取得するのがゲートの開いている間だけであることを確認する。
// Vencord を import しないので、Node で実行できる。

import { describe, expect, it } from "vitest";

import type { CoreEvent } from "../core/events";
import {
    avatarFetchesToStart,
    avatarFetchTargets,
    AvatarOutbox,
    buildFacts,
    buildProfiles,
    type AvatarCandidate,
    type AvatarFetchInputs,
    type GateFactsInput,
    type RecordingFactsInput,
} from "../core/facts";
import { initialState, reduce, type CoreState } from "../core/state";
import { LadderStage } from "../encode/ladder";

function apply(events: readonly CoreEvent[], from: CoreState = initialState): CoreState {
    return events.reduce(reduce, from);
}

const NO_STAGES = new Map<number, LadderStage>();
/** 録画が OFF で、SPS と PPS を付け足したこともない状態。 */
const NO_RECORDING: RecordingFactsInput = { on: false, spsPpsPrepended: 0 };
/** 受信側から何も届いていない状態 (media が true で、上限なし)。 */
const DEFAULT_GATE: GateFactsInput = { media: true, limits: {} };

describe("buildFacts: warnings、sharing、roster", () => {
    it("初期状態では warnings、sharing が空で、roster (unknown) はキーを入れない", () => {
        const facts = buildFacts(initialState, NO_STAGES, NO_RECORDING, DEFAULT_GATE);
        expect(facts).toEqual({
            warnings: [],
            sharing: { count: 0, user_ids: [] },
            recording: false,
            sps_pps_prepended: 0,
            media: true,
            limits: {},
        });
        expect("roster" in facts).toBe(false);
    });

    it("roster が読めれば (0 人でも) キーを入れる", () => {
        const state = apply([{ type: "roster", users: [] }]);
        const facts = buildFacts(state, NO_STAGES, NO_RECORDING, DEFAULT_GATE);
        expect(facts.roster).toEqual({ count: 0, user_ids: [] });
    });
});

describe("buildFacts: 通話の名前 (call_name) とメインアカウントの通話 (main_call)", () => {
    it("読めていない間は、どちらのキーも入れない", () => {
        const facts = buildFacts(initialState, NO_STAGES, NO_RECORDING, DEFAULT_GATE);
        expect("call_name" in facts).toBe(false);
        expect("main_call" in facts).toBe(false);
    });

    it("通話の名前が読めれば call_name に入れる。空の文字列と null は入れない", () => {
        const named = apply([{ type: "call_name", name: "Server A / General" }]);
        expect(buildFacts(named, NO_STAGES, NO_RECORDING, DEFAULT_GATE).call_name).toBe("Server A / General");
        const empty = apply([{ type: "call_name", name: "" }]);
        expect("call_name" in buildFacts(empty, NO_STAGES, NO_RECORDING, DEFAULT_GATE)).toBe(false);
        const cleared = apply([{ type: "call_name", name: "x" }, { type: "call_name", name: null }]);
        expect("call_name" in buildFacts(cleared, NO_STAGES, NO_RECORDING, DEFAULT_GATE)).toBe(false);
    });

    it("メインアカウントの通話は、読めた値のキーだけを入れる", () => {
        const full = apply([{ type: "main_call", call: { userId: "m", name: "Room", joinable: false } }]);
        expect(buildFacts(full, NO_STAGES, NO_RECORDING, DEFAULT_GATE).main_call).toEqual({
            user_id: "m",
            name: "Room",
            joinable: false,
        });
        const partial = apply([{ type: "main_call", call: { userId: "m", name: null, joinable: null } }]);
        expect(buildFacts(partial, NO_STAGES, NO_RECORDING, DEFAULT_GATE).main_call).toEqual({ user_id: "m" });
    });

    it("メインアカウントの通話が見えなくなれば (null)、キーごと省く。通話にいないという値は送らない", () => {
        const gone = apply([
            { type: "main_call", call: { userId: "m", name: "Room", joinable: true } },
            { type: "main_call", call: null },
        ]);
        const facts = buildFacts(gone, NO_STAGES, NO_RECORDING, DEFAULT_GATE);
        expect("main_call" in facts).toBe(false);
        expect(JSON.stringify(facts)).not.toContain("not_in_call");
    });
});

describe("buildFacts: Vesktop のアカウントの self_user_id", () => {
    it("Vesktop のアカウントが分かっていれば self_user_id に入れる (roster にいるかどうかにかかわらない)", () => {
        const inRoster = apply([
            { type: "roster", users: [{ userId: "u1" }, { userId: "me" }] },
            { type: "self", userId: "me" },
        ]);
        expect(buildFacts(inRoster, NO_STAGES, NO_RECORDING, DEFAULT_GATE).self_user_id).toBe("me");

        // roster がまだ読めていなくても、Vesktop のアカウントは別に読める (受信側は roster が届いたときに使う)。
        const rosterUnknown = apply([{ type: "self", userId: "me" }]);
        const facts = buildFacts(rosterUnknown, NO_STAGES, NO_RECORDING, DEFAULT_GATE);
        expect(facts.self_user_id).toBe("me");
        expect("roster" in facts).toBe(false);
    });

    it("Vesktop のアカウントが分からない間はキーを入れない (roster と同じ)", () => {
        expect("self_user_id" in buildFacts(initialState, NO_STAGES, NO_RECORDING, DEFAULT_GATE)).toBe(false);

        // 分かっていたのに分からなくなったとき (ログアウトなど) も入れない。
        const lost = apply([
            { type: "self", userId: "me" },
            { type: "self", userId: null },
        ]);
        expect("self_user_id" in buildFacts(lost, NO_STAGES, NO_RECORDING, DEFAULT_GATE)).toBe(false);
    });

    it("空の userId は入れない (userId として意味がない)", () => {
        const empty = apply([{ type: "self", userId: "" }]);
        expect("self_user_id" in buildFacts(empty, NO_STAGES, NO_RECORDING, DEFAULT_GATE)).toBe(false);
    });

    it("Vesktop のアカウントが分かったときと替わったときは JSON が変わる (変わったときだけ送る呼び出し元が気付ける)", () => {
        const before = JSON.stringify(buildFacts(initialState, NO_STAGES, NO_RECORDING, DEFAULT_GATE));
        const known = apply([{ type: "self", userId: "me" }]);
        const afterKnown = JSON.stringify(buildFacts(known, NO_STAGES, NO_RECORDING, DEFAULT_GATE));
        const switched = apply([{ type: "self", userId: "other" }], known);
        const afterSwitch = JSON.stringify(buildFacts(switched, NO_STAGES, NO_RECORDING, DEFAULT_GATE));
        expect(afterKnown).not.toBe(before);
        expect(afterSwitch).not.toBe(afterKnown);
    });
});

describe("buildFacts: stages", () => {
    it("streamId が 1 つもなければ stages のキーを入れない", () => {
        const facts = buildFacts(initialState, NO_STAGES, NO_RECORDING, DEFAULT_GATE);
        expect("stages" in facts).toBe(false);
    });

    it("LadderStage を文字列にして送る (native、halffps、downscaled、software、unavailable)", () => {
        const stages = new Map<number, LadderStage>([
            [1, LadderStage.Native],
            [2, LadderStage.HalfFps],
            [3, LadderStage.Downscaled],
            [4, LadderStage.SoftwareFloor],
            [5, LadderStage.Unavailable],
        ]);
        const facts = buildFacts(initialState, stages, NO_RECORDING, DEFAULT_GATE);
        expect(facts.stages).toEqual([
            { stream_id: 1, stage: "native" },
            { stream_id: 2, stage: "halffps" },
            { stream_id: 3, stage: "downscaled" },
            { stream_id: 4, stage: "software" },
            { stream_id: 5, stage: "unavailable" },
        ]);
    });

    it("Map に入れた順にかかわらず stream_id の昇順に並ぶ (JSON を比べて変化を見つけるため)", () => {
        const insertedHighFirst = new Map<number, LadderStage>([
            [9, LadderStage.Native],
            [2, LadderStage.HalfFps],
            [5, LadderStage.Downscaled],
        ]);
        const facts = buildFacts(initialState, insertedHighFirst, NO_RECORDING, DEFAULT_GATE);
        expect(facts.stages).toEqual([
            { stream_id: 2, stage: "halffps" },
            { stream_id: 5, stage: "downscaled" },
            { stream_id: 9, stage: "native" },
        ]);
    });

    it("同じ内容なら、入れた順が違っても JSON の文字列が一致する (変わっていないのに送らない)", () => {
        const a = new Map<number, LadderStage>([
            [1, LadderStage.Native],
            [2, LadderStage.HalfFps],
        ]);
        const b = new Map<number, LadderStage>([
            [2, LadderStage.HalfFps],
            [1, LadderStage.Native],
        ]);
        expect(JSON.stringify(buildFacts(initialState, a, NO_RECORDING, DEFAULT_GATE))).toBe(JSON.stringify(buildFacts(initialState, b, NO_RECORDING, DEFAULT_GATE)));
    });
});

describe("buildFacts: 録画の 2 つのフィールド", () => {
    it("録画が ON なら recording が true になる", () => {
        const facts = buildFacts(initialState, NO_STAGES, { on: true, spsPpsPrepended: 0 }, DEFAULT_GATE);
        expect(facts.recording).toBe(true);
    });

    it("OFF でも recording のキーを入れる (OFF だったのか、古い送信側なのかを受信側が区別できる)", () => {
        const facts = buildFacts(initialState, NO_STAGES, NO_RECORDING, DEFAULT_GATE);
        expect("recording" in facts).toBe(true);
        expect(facts.recording).toBe(false);
    });

    it("SPS と PPS を付け足した回数をそのまま入れ、0 でもキーを入れる", () => {
        expect(buildFacts(initialState, NO_STAGES, { on: true, spsPpsPrepended: 3 }, DEFAULT_GATE).sps_pps_prepended).toBe(3);
        expect("sps_pps_prepended" in buildFacts(initialState, NO_STAGES, NO_RECORDING, DEFAULT_GATE)).toBe(true);
    });

    it("値が変われば JSON も変わる (変わったときだけ送る呼び出し元が気付ける)", () => {
        const off = JSON.stringify(buildFacts(initialState, NO_STAGES, NO_RECORDING, DEFAULT_GATE));
        const on = JSON.stringify(buildFacts(initialState, NO_STAGES, { on: true, spsPpsPrepended: 0 }, DEFAULT_GATE));
        const fired = JSON.stringify(buildFacts(initialState, NO_STAGES, { on: true, spsPpsPrepended: 1 }, DEFAULT_GATE));
        expect(off).not.toBe(on);
        expect(on).not.toBe(fired);
    });
});

const NO_KEYS = new Map<string, string>();
const NO_BOTS = new Set<string>();

describe("buildProfiles", () => {
    it("roster が unknown の間は undefined を返す (呼び出し元は profiles を送らない)", () => {
        expect(buildProfiles(initialState.roster, new Map(), NO_KEYS, NO_BOTS)).toBeUndefined();
    });

    it("roster が読めていて 0 人なら空の配列を返す (unknown と区別する)", () => {
        const state = apply([{ type: "roster", users: [] }]);
        expect(buildProfiles(state.roster, new Map(), NO_KEYS, NO_BOTS)).toEqual([]);
    });

    it("表示名を読めた userId には display_name を付け、読めない userId には付けない。bot は必ず入れる", () => {
        const state = apply([{ type: "roster", users: [{ userId: "u1" }, { userId: "u2" }] }]);
        const names = new Map([["u1", "たろう"]]);
        expect(buildProfiles(state.roster, names, NO_KEYS, NO_BOTS)).toEqual([
            { user_id: "u1", bot: false, display_name: "たろう" },
            { user_id: "u2", bot: false },
        ]);
    });

    it("avatar_key は読めた人だけに付ける。アバターを設定していない人の空の文字列も、そのまま付ける", () => {
        const state = apply([{ type: "roster", users: [{ userId: "u1" }, { userId: "u2" }, { userId: "u3" }] }]);
        const keys = new Map([
            ["u1", "abc123"],
            ["u2", ""],
        ]);
        const profiles = buildProfiles(state.roster, new Map(), keys, NO_BOTS);
        expect(profiles?.[0]?.avatar_key).toBe("abc123");
        expect(profiles?.[1]).toHaveProperty("avatar_key", "");
        expect(profiles?.[2]).not.toHaveProperty("avatar_key");
    });

    it("アバターの画像は profiles に載せない (avatar_png のキーは入らない)", () => {
        const state = apply([{ type: "roster", users: [{ userId: "u1" }] }]);
        const profiles = buildProfiles(state.roster, new Map([["u1", "たろう"]]), new Map([["u1", "k"]]), NO_BOTS);
        expect(profiles?.[0]).not.toHaveProperty("avatar_png");
    });

    it("Vesktop のアカウントが roster にいなくても、表示名を読めていれば、その人の分を足す", () => {
        const state = apply([{ type: "roster", users: [{ userId: "u1" }] }]);
        const names = new Map([
            ["u1", "たろう"],
            ["me", "サブ"],
        ]);
        expect(buildProfiles(state.roster, names, NO_KEYS, NO_BOTS, "me")).toEqual([
            { user_id: "u1", bot: false, display_name: "たろう" },
            { user_id: "me", bot: false, display_name: "サブ" },
        ]);
    });

    it("Vesktop のアカウントが roster にいるときは、重ねて足さない", () => {
        const state = apply([{ type: "roster", users: [{ userId: "me" }] }]);
        const profiles = buildProfiles(state.roster, new Map([["me", "サブ"]]), NO_KEYS, NO_BOTS, "me");
        expect(profiles).toEqual([{ user_id: "me", bot: false, display_name: "サブ" }]);
    });

    it("roster が unknown の間も、Vesktop のアカウントの分だけは返す。名前を読めていなければ undefined", () => {
        expect(buildProfiles(initialState.roster, new Map([["me", "サブ"]]), NO_KEYS, NO_BOTS, "me")).toEqual([
            { user_id: "me", bot: false, display_name: "サブ" },
        ]);
        expect(buildProfiles(initialState.roster, new Map(), NO_KEYS, NO_BOTS, "me")).toBeUndefined();
        expect(buildProfiles(initialState.roster, new Map([["me", "サブ"]]), NO_KEYS, NO_BOTS, null)).toBeUndefined();
    });

    it("bots にいる人だけ bot が真になる。roster にいない bots の人は出ない", () => {
        const state = apply([{ type: "roster", users: [{ userId: "u1" }, { userId: "b1" }] }]);
        const profiles = buildProfiles(state.roster, new Map(), NO_KEYS, new Set(["b1", "gone"]));
        expect(profiles).toEqual([
            { user_id: "u1", bot: false },
            { user_id: "b1", bot: true },
        ]);
    });

    it("avatar_key や bot が変われば JSON も変わる (変わったときだけ送る呼び出し元が気付ける)", () => {
        const state = apply([{ type: "roster", users: [{ userId: "u1" }] }]);
        const base = JSON.stringify(buildProfiles(state.roster, new Map(), new Map([["u1", "a"]]), NO_BOTS));
        const keyChanged = JSON.stringify(buildProfiles(state.roster, new Map(), new Map([["u1", "b"]]), NO_BOTS));
        const botChanged = JSON.stringify(buildProfiles(state.roster, new Map(), new Map([["u1", "a"]]), new Set(["u1"])));
        expect(keyChanged).not.toBe(base);
        expect(botChanged).not.toBe(base);
    });
});

describe("avatarFetchTargets (アバターを取得してよい相手。roster の全員)", () => {
    it("画面共有していない人も、Vesktop のアカウントでない人も、roster にいれば取得する", () => {
        const state = apply([
            { type: "roster", users: [{ userId: "u1" }, { userId: "u2" }, { userId: "me" }] },
            { type: "self", userId: "me" },
            { type: "sharing", userId: "u1", streamKey: "k1", pcId: 2, active: true },
        ]);
        expect([...avatarFetchTargets(state)].sort()).toEqual(["me", "u1", "u2"]);
    });

    it("roster が unknown の間は誰も取得しない (画面共有している人も、Vesktop のアカウントも)", () => {
        const state = apply([
            { type: "self", userId: "me" },
            { type: "sharing", userId: "u1", streamKey: "k1", pcId: 2, active: true },
        ]);
        expect(state.roster).toBe("unknown");
        expect([...avatarFetchTargets(state)]).toEqual([]);
    });

    it("画面共有の一覧に残ったまま roster から抜けた人は、取得する相手から外れる (profiles は roster の人の分しか作らないため)", () => {
        const state = apply([
            { type: "roster", users: [{ userId: "u1" }] },
            { type: "sharing", userId: "u1", streamKey: "k1", pcId: 2, active: true },
            // u1 が roster から抜ける (sharing にはまだ残る。roster のイベントの reduce() は state.sharing を
            // 変えない)。
            { type: "roster", users: [] },
        ]);
        expect([...avatarFetchTargets(state)]).toEqual([]);
    });
});

describe("AvatarOutbox (アバターは、avatar_key が変わった人だけ、上限の範囲で送る)", () => {
    const png = (userId: string, avatarKey: string): AvatarCandidate => ({
        userId,
        avatarKey,
        pngBase64: `png-${userId}-${avatarKey}`,
    });

    it("初めての人は送り、同じ avatar_key の人は送らない", () => {
        const outbox = new AvatarOutbox(4, 1000);
        const first = outbox.pick(0, [png("u1", "a"), png("u2", "")]);
        expect(first.send.map((a) => a.userId)).toEqual(["u1", "u2"]);
        expect(first.retryInMs).toBeNull();
        const again = outbox.pick(10_000, [png("u1", "a"), png("u2", "")]);
        expect(again.send).toEqual([]);
    });

    it("avatar_key が変わった人だけ、もう一度送る (設定していない人の空の文字列から設定した値への変化も)", () => {
        const outbox = new AvatarOutbox(4, 1000);
        outbox.pick(0, [png("u1", "a"), png("u2", "")]);
        const changed = outbox.pick(5000, [png("u1", "a"), png("u2", "z")]);
        expect(changed.send.map((a) => `${a.userId}:${a.avatarKey}`)).toEqual(["u2:z"]);
    });

    it("reset() の後は、同じ avatar_key の人も全員分を送り直す", () => {
        const outbox = new AvatarOutbox(4, 1000);
        outbox.pick(0, [png("u1", "a"), png("u2", "b")]);
        outbox.reset();
        const resent = outbox.pick(5000, [png("u1", "a"), png("u2", "b")]);
        expect(resent.send.map((a) => a.userId)).toEqual(["u1", "u2"]);
    });

    it("1 秒に送る人数に上限がある。残った人は、枠が空く時刻 (retryInMs) の後に送る", () => {
        const outbox = new AvatarOutbox(4, 1000);
        const many = ["u1", "u2", "u3", "u4", "u5", "u6"].map((id) => png(id, "a"));
        const first = outbox.pick(100, many);
        expect(first.send.map((a) => a.userId)).toEqual(["u1", "u2", "u3", "u4"]);
        expect(first.retryInMs).toBe(1000); // 最初に送った 100 の 1 秒後 (1100) まで 1000 ms。
        // 枠が空く前に呼んでも、送らない (残りの時間は減る)。
        const early = outbox.pick(600, many);
        expect(early.send).toEqual([]);
        expect(early.retryInMs).toBe(500);
        const later = outbox.pick(1100, many);
        expect(later.send.map((a) => a.userId)).toEqual(["u5", "u6"]);
        expect(later.retryInMs).toBeNull();
    });

    it("上限で送れなかった人は、送ったものとして覚えない (後で必ず送る)", () => {
        const outbox = new AvatarOutbox(1, 1000);
        const two = [png("u1", "a"), png("u2", "b")];
        expect(outbox.pick(0, two).send.map((a) => a.userId)).toEqual(["u1"]);
        expect(outbox.pick(1000, two).send.map((a) => a.userId)).toEqual(["u2"]);
        expect(outbox.pick(2000, two).send).toEqual([]);
    });

    it("keepOnly() で roster から抜けた人の記録を捨てると、戻ってきたときにもう一度送る", () => {
        const outbox = new AvatarOutbox(4, 1000);
        outbox.pick(0, [png("u1", "a"), png("u2", "b")]);
        outbox.keepOnly(new Set(["u1"]));
        const back = outbox.pick(5000, [png("u1", "a"), png("u2", "b")]);
        expect(back.send.map((a) => a.userId)).toEqual(["u2"]);
    });
});

describe("buildFacts: ゲートの 2 つのフィールド", () => {
    it("何も届いていない間は、media が true で上限なしの値を入れる (キーを入れる)", () => {
        const facts = buildFacts(initialState, NO_STAGES, NO_RECORDING, DEFAULT_GATE);
        expect(facts.media).toBe(true);
        expect(facts.limits).toEqual({});
        expect("media" in facts).toBe(true);
        expect("limits" in facts).toBe(true);
    });

    it("受信側から届いた現在の値を、ワイヤー形式の形のまま入れる (false でも、キーを省いた limits でも)", () => {
        const closed = buildFacts(initialState, NO_STAGES, NO_RECORDING, { media: false, limits: { fps_divisor: 2 } });
        expect(closed.media).toBe(false);
        expect(closed.limits).toEqual({ fps_divisor: 2 });

        const both = buildFacts(initialState, NO_STAGES, NO_RECORDING, {
            media: true,
            limits: { max_long_edge: 1280, fps_divisor: 1 },
        });
        expect(both.limits).toEqual({ max_long_edge: 1280, fps_divisor: 1 });
    });

    it("エンコードの設定の 2 つのキーも、ワイヤー形式のまま入れる", () => {
        const facts = buildFacts(initialState, NO_STAGES, NO_RECORDING, {
            media: true,
            limits: { encoder_preference: "software", latency_mode: "quality" },
        });
        expect(facts.limits).toEqual({ encoder_preference: "software", latency_mode: "quality" });
    });

    it("limits はワイヤー形式の 4 つのキーだけをコピーする (呼び出し元の値にほかのキーがあっても入れない)", () => {
        const withExtra = { max_long_edge: 960, prefer_software: true } as GateFactsInput["limits"];
        const facts = buildFacts(initialState, NO_STAGES, NO_RECORDING, { media: true, limits: withExtra });
        expect(facts.limits).toEqual({ max_long_edge: 960 });
    });

    it("値が変われば JSON も変わる (変わったときだけ送る呼び出し元が気付ける)", () => {
        const base = JSON.stringify(buildFacts(initialState, NO_STAGES, NO_RECORDING, DEFAULT_GATE));
        const mediaOff = JSON.stringify(buildFacts(initialState, NO_STAGES, NO_RECORDING, { media: false, limits: {} }));
        const limited = JSON.stringify(
            buildFacts(initialState, NO_STAGES, NO_RECORDING, { media: true, limits: { max_long_edge: 640 } }),
        );
        expect(mediaOff).not.toBe(base);
        expect(limited).not.toBe(base);
        expect(limited).not.toBe(mediaOff);
    });
});

describe("avatarFetchesToStart (アバターは、ゲートが開いている間だけ取得する)", () => {
    const open: AvatarFetchInputs = {
        gateOpen: true,
        targets: new Set(["u1", "u2", "u3", "u4"]),
        urls: new Map([
            ["u1", "https://cdn.discordapp.com/avatars/u1/a.png"],
            ["u2", "https://cdn.discordapp.com/avatars/u2/b.png"],
            ["u3", "https://cdn.discordapp.com/avatars/u3/c.png"],
            // u4 は、UserStore が URL を返さなかった。
        ]),
        cached: new Map([["u2", { url: "https://cdn.discordapp.com/avatars/u2/b.png" }]]),
        inFlight: new Set(["u3"]),
    };

    it("ゲートが開いていれば、URL があり、同じ URL の結果がなく、取得している最中でない人だけを取得する", () => {
        expect(avatarFetchesToStart(open)).toEqual([{ userId: "u1", url: "https://cdn.discordapp.com/avatars/u1/a.png" }]);
    });

    it("URL が変わった人は取得し直す", () => {
        const changed: AvatarFetchInputs = {
            ...open,
            cached: new Map([["u2", { url: "https://cdn.discordapp.com/avatars/u2/old.png" }]]),
        };
        expect(avatarFetchesToStart(changed).map((f) => f.userId)).toEqual(["u1", "u2"]);
    });

    it("ゲートが閉じていれば (切れている、media が false) 1 つも取得しない", () => {
        expect(avatarFetchesToStart({ ...open, gateOpen: false })).toEqual([]);
        expect(avatarFetchesToStart({ ...open, gateOpen: false, cached: new Map(), inFlight: new Set() })).toEqual([]);
    });
});
