// SPDX-License-Identifier: Apache-2.0

// run-check.mjs の単体テスト。セットの表から書く settings.json と seen_people.json、compare.py に渡す人と話す人、
// run-window.mjs の呼び出しと環境変数、OBS のログの行の読み方、hold の JSON の読み方、ログを読みながら待つかの決め方、
// ドライランの参照の要求を確認する。OBS もテスト用の送信側も起動しない。

import path from "node:path";

import { describe, expect, it } from "vitest";

import {
    COMMON_SETTINGS,
    DEFAULT_ORDER,
    SEED,
    SETS,
    avatarDirOf,
    decideLogWait,
    goFileOf,
    holdInfoPathOf,
    logLineEpochMs,
    noteAvatars,
    parseAvatarLine,
    parseCliArgs,
    parseHoldInfo,
    parseLabelReady,
    parseLayoutLine,
    parseRosterReady,
    peopleFor,
    phasePlan,
    rosterLabelCountOf,
    runWindowArgs,
    runWindowEnv,
    seenPeopleFor,
    settingsFor,
    speakersFor,
    trialRequest,
} from "./run-check.mjs";
import { parseArgs as parseRunWindowArgs, planSteps } from "../obs-run/run-window-plan.mjs";

const OPTS = { obs: "C:/work/obs", out: "C:/work/out/gpu", plugin: "C:/work/plugin" };

describe("セットの表", () => {
    it("起動は初期設定の書き込みと 6 つのセットの 7 回で、関門の c1 が初期設定の書き込みの次に来る", () => {
        expect(DEFAULT_ORDER).toEqual([SEED, "c1", "c2c5", "c3", "c4c7", "c6", "c8"]);
    });

    it("まとめたセットの設定は、まとめる前の 2 つのセットの設定を合わせたものになる", () => {
        expect(SETS.c2c5.settings).toEqual({ ...SETS.c2.settings, ...SETS.c5.settings });
        expect(SETS.c4c7.settings).toEqual({ ...SETS.c4.settings, ...SETS.c7.settings });
        expect(SETS.c2c5.background).toBe(SETS.c2.background);
        expect(SETS.c4c7.expectFrameGlow).toBe(true);
        expect(SETS.c3.settings.label_glow_frame).toBe(true);
        expect(SETS.c3.settings.label_glow_blur_ratio).toBe(0);
        expect(SETS.c3.settings.label_show_plate).toBe(false);
        expect(SETS.c3.expectFrameGlow).toBe(true);
    });

    it("c1 だけが時間を既定のままにし、ほかのセットは立ち上がりと消えるを 4000 ms にする", () => {
        expect(SETS.c1.settings).toEqual({});
        for (const name of Object.keys(SETS).filter((n) => n !== "c1")) {
            expect(SETS[name].settings.label_border_rise_ms).toBe(4000);
            expect(SETS[name].settings.label_border_fade_ms).toBe(4000);
            expect(SETS[name].settings.label_border_hold_ms).toBeUndefined();
        }
        expect(phasePlan("c1").riseShotMs).toBe(1000);
        expect(phasePlan("c3").riseShotMs).toBe(5000);
    });

    it("settings.json は初期設定の値に共通の値とセットの値を上から書き、前のセットの値を残さない", () => {
        const base = { record_dir: "C:/work/rec", assign_mode: "manual", label_margin_pct: 3 };
        const c6 = settingsFor("c6", base);
        expect(c6.record_dir).toBe("C:/work/rec");
        expect(c6.assign_mode).toBe(COMMON_SETTINGS.assign_mode);
        expect(c6.broadcaster_user_id).toBe("check-self");
        expect(c6.label_em_ratio_pct).toBe(8.5);
        expect(c6.label_glow_blur_ratio).toBe(3.0);
        // 初期設定を書き込んだ settings.json は名前ラベルの鍵を書かないので、ほかのセットの値は初期設定の値からは入らない。
        expect(settingsFor("c1", { record_dir: "C:/work/rec" })).toEqual({ record_dir: "C:/work/rec", ...COMMON_SETTINGS });
        expect(() => settingsFor("c9")).toThrow();
    });

    it("seen_people.json は 2 人の名前と使う声を書き、名前ラベルの隅は参加者ごとに書かない (c6 は設定の label_corner で右下)", () => {
        const rows = seenPeopleFor("c1");
        expect(rows.map((r) => [r.user_id, r.name_override, r.voice_source_user_id])).toEqual([
            ["check-self", "たろう", "harness-voice-2"],
            ["harness-screen-1", "Noa Kimura", "harness-voice-3"],
        ]);
        for (const name of Object.keys(SETS)) {
            expect(seenPeopleFor(name).every((r) => !("label_corner" in r))).toBe(true);
        }
        expect(SETS.c6.settings.label_corner).toBe("br");
    });

    it("c8 は声だけの 2 人を名前だけ書いて並びに出し (使う声に結ばない)、そのうち 1 人を立ち上がりで話させる", () => {
        const rows = seenPeopleFor("c8");
        expect(rows.slice(2).map((r) => [r.user_id, r.name_override, "voice_source_user_id" in r])).toEqual([
            ["harness-voice-4", "さくら", false],
            ["harness-voice-5", "Ken", false],
        ]);
        expect(SETS.c8.settings.roster_show).toBe(true);
        const people = peopleFor("c8");
        expect(people.map((p) => [p.role, p.userId, p.voiceStream])).toEqual([
            ["self", "check-self", 2],
            ["screen", "harness-screen-1", 3],
            ["roster", "harness-voice-4", 4],
            ["roster", "harness-voice-5", 5],
        ]);
        // 並びの名前ラベルは、並びの 2 人と、枠に名前を出している画面共有の人の分を描く (使う声の人と、通話にいない自分の枠の人は
        // 入らない)。並びのないセットでも、画面共有の人の 1 人分を描く。
        expect(rosterLabelCountOf("c8")).toBe(3);
        expect(rosterLabelCountOf("c1")).toBe(1);
        expect(speakersFor("c8")).toEqual({
            riseTone: "4",
            bothTone: "3 4 5",
            rise: ["harness-voice-4"],
            both: ["harness-screen-1", "harness-voice-4", "harness-voice-5"],
        });
        // ほかのセットは並びが空で、自分の枠の人を立ち上がりで、自分の枠の人と画面共有の人を 2 人で話させる。
        expect(peopleFor("c1").map((p) => p.role)).toEqual(["self", "screen"]);
        expect(speakersFor("c1")).toEqual({ riseTone: "2", bothTone: "2 3", rise: ["check-self"], both: ["check-self", "harness-screen-1"] });
    });

    it("compare.py に渡す人は全員が声を聞ける人で、アバターは送信側の人だけがセットのフォルダーの PNG を持つ", () => {
        const people = peopleFor("c8");
        expect(people.every((p) => p.hearsVoice === true)).toBe(true);
        expect(people.map((p) => p.avatar)).toEqual([
            null,
            "avatars/harness-screen-1.png",
            "avatars/harness-voice-4.png",
            "avatars/harness-voice-5.png",
        ]);
    });
});

describe("run-window.mjs の呼び出し", () => {
    it("初期設定の書き込みは送信側を起動せず、セットの実行は初期設定を書き込まずに画面 1 本と声 2 本で起動する", () => {
        expect(runWindowArgs(SEED, OPTS)).not.toContain("--no-seed");
        expect(runWindowArgs(SEED, OPTS)).not.toContain("--n-screen");
        const c1 = runWindowArgs("c1", OPTS);
        expect(c1).toEqual(expect.arrayContaining(["--no-seed", "--n-screen", "1", "--n-voice", "2", "--screen-sizes", "960x540"]));
        const c8 = runWindowArgs("c8", OPTS);
        expect(c8[c8.indexOf("--n-voice") + 1]).toBe("4");
        expect(c1[c1.indexOf("--tag") + 1]).toBe("gc-c1");
        expect(c1[c1.indexOf("--out") + 1]).toBe(path.join(OPTS.out, "c1"));
        // 後片付けがほかの実行の録画を消さないように、どの実行にも付ける。
        expect(runWindowArgs(SEED, OPTS)).toContain("--keep-recordings");
        expect(c1).toContain("--keep-recordings");
    });

    it("run-window-plan.mjs がセットの呼び出しを受け付け、hold の手順に送信側の起動と終わりのファイルの待ちが入る", () => {
        for (const name of [SEED, "c1", "c4c7", "c8"]) {
            const parsed = parseRunWindowArgs(runWindowArgs(name, OPTS));
            expect(parsed.errors).toEqual([]);
            const plan = planSteps(parsed.args);
            expect(plan.ok).toBe(true);
            const kinds = plan.steps.map((s: { kind: string }) => s.kind);
            expect(kinds).toContain("hold-info");
            expect(kinds).toContain("human");
            expect(kinds.includes("harness-start")).toBe(name !== SEED);
        }
    });

    it("声に音を乗せるファイルの場所とアバターを書き出すフォルダーは、セットの実行だけ環境変数で渡す", () => {
        expect(runWindowEnv(SEED, OPTS)).toEqual({});
        expect(runWindowEnv("c3", OPTS)).toEqual({
            CORDMUX_HARNESS_VOICE_TONE_FILE: path.join(OPTS.out, "c3", "voice-tone.flag"),
            CORDMUX_HARNESS_AVATAR_DIR: path.join(OPTS.out, "c3", "avatars"),
        });
        expect(avatarDirOf(OPTS, "c8")).toBe(path.join(OPTS.out, "c8", "avatars"));
        expect(holdInfoPathOf(OPTS, "c3")).toBe(path.join(OPTS.out, "c3", "window3b", "hold-gc-c3.json"));
        expect(goFileOf(OPTS, "c3")).toBe(path.join(OPTS.out, "c3", "window3b", "human-go.txt"));
    });

    it("引数の誤りを全部集め、知らないセットを断る", () => {
        const { errors } = parseCliArgs(["--sets", "c1,c9"]);
        expect(errors).toEqual(
            expect.arrayContaining(["--obs が要ります", "--built-dll が要ります", expect.stringContaining("知らないセットです: c9")]),
        );
        const ok = parseCliArgs(["--obs", "a", "--plugin", "b", "--out", "c", "--built-dll", "d", "--decor-preview", "e", "--sets", "c2,c5"]);
        expect(ok.errors).toEqual([]);
        expect(ok.args.sets).toEqual(["c2", "c5"]);
    });
});

describe("hold の JSON と OBS のログ", () => {
    it("hold の JSON は使う値がそろっていなければ断る", () => {
        const info = { wsPort: 4460, goFile: "C:/o/human-go.txt", obsConfigDir: "C:/obs/config/obs-studio", obsPid: 10, framesPauseFile: null };
        expect(parseHoldInfo(JSON.stringify(info)).wsPort).toBe(4460);
        expect(() => parseHoldInfo(JSON.stringify({ ...info, obsPid: null }))).toThrow("obsPid");
    });

    it("配置の行と、名前ラベルと枠の光を描き終えた行を読む", () => {
        const layout = parseLayoutLine(
            "12:00:01.250: [cordmux] Composite layout canvas=1920x1080 count=2 self=0,0,960,540 slot1=960,0,960,540",
        );
        expect(layout).toEqual({ canvas: [1920, 1080], count: 2, tiles: { self: [0, 0, 960, 540], slot1: [960, 0, 960, 540] } });
        expect(parseLayoutLine("12:00:01.250: [cordmux] Composite layout canvas=1920x1080 count=0")?.count).toBe(0);
        expect(parseLayoutLine("12:00:01.250: [cordmux] Screen slot 1 attached")).toBeNull();
        expect(parseLabelReady("x: [cordmux] Composite label ready: tile=1 cell=960x540 paint_ms=5 upload_ms=0 total=2")).toEqual({
            kind: "label",
            tile: 1,
            paintMs: 5,
            uploadMs: 0,
            total: 2,
        });
        expect(parseLabelReady("x: [cordmux] Composite frame glow ready: tile=0 cell=960x540 paint_ms=9 upload_ms=1 total=1")?.kind).toBe("frame");
    });

    it("並びの名前ラベルを描き終えた行と、受信側がアバターを受け取った行を読む", () => {
        expect(parseRosterReady("x: [cordmux] Composite roster label ready: index=1 paint_ms=4 upload_ms=0 total=3")).toEqual({
            index: 1,
            paintMs: 4,
            uploadMs: 0,
            total: 3,
        });
        expect(parseRosterReady("x: [cordmux] Composite label ready: tile=1 cell=960x540 paint_ms=5 upload_ms=0 total=2")).toBeNull();
        expect(parseAvatarLine("x: [cordmux] Avatar: stored=yes people_with_avatar=5 avatar_decode_failures=0")).toEqual({
            stored: true,
            peopleWithAvatar: 5,
            decodeFailures: 0,
        });
        expect(parseAvatarLine("x: [cordmux] Profiles: people=5 avatar_decode_failures=0")).toBeNull();
    });

    it("アバターの行から受け取った人の数の最大を数え、そろった最初の時刻だけを覚える", () => {
        const line = (n: number) => "x: [cordmux] Avatar: stored=yes people_with_avatar=" + n + " avatar_decode_failures=0";
        let state = { seen: 0, completeMs: null as number | null };
        state = noteAvatars(state, line(2), 3, 100);
        expect(state).toEqual({ seen: 2, completeMs: null });
        state = noteAvatars(state, "x: [cordmux] Composite label ready: tile=0 cell=960x540 paint_ms=5 upload_ms=0 total=1", 3, 150);
        expect(state).toEqual({ seen: 2, completeMs: null });
        state = noteAvatars(state, line(3), 3, 200);
        expect(state).toEqual({ seen: 3, completeMs: 200 });
        // 後の行で数が減っても (profiles で人が入れ替わったときなど)、そろった時刻と最大は変えない。
        state = noteAvatars(state, line(1), 3, 300);
        expect(state).toEqual({ seen: 3, completeMs: 200 });
    });

    it("ログの行の時刻を同じ日の時刻にし、1 秒を超えて遅れたら決めた時間だけ待つ形にする", () => {
        const base = new Date(2000, 0, 1, 12, 0, 2, 0).getTime();
        expect(logLineEpochMs("12:00:01.250: [cordmux] x", base)).toBe(base - 750);
        expect(logLineEpochMs("not a log line", base)).toBeNull();
        // 日付をまたいだ行 (base は 0 時 0 分の直後、行は前の日の 23 時台)
        const after = new Date(2000, 0, 2, 0, 0, 1, 0).getTime();
        expect(logLineEpochMs("23:59:59.500: x", after)).toBe(after - 1500);
        expect(decideLogWait([20, 80, 150])).toEqual({ maxMs: 150, samples: 3, fixedWaits: false });
        expect(decideLogWait([20, 1500]).fixedWaits).toBe(true);
        expect(decideLogWait([]).fixedWaits).toBe(true);
    });
});

describe("ドライランの参照の要求", () => {
    it("compare.py と同じ欄で、画面共有の枠の人と並びの人を分け、明るさ 0、1、0.5 の 3 枚を描く", () => {
        const req = trialRequest("c8", "C:/d/settings.json", "C:/d/bg.png", (p: { avatar: string }) => "C:/d/" + p.avatar);
        expect(req.people.map((p: { slot: number; user_id: string; hears_voice: boolean }) => [p.slot, p.user_id, p.hears_voice])).toEqual([
            [0, "check-self", true],
            [1, "harness-screen-1", true],
        ]);
        expect(req.people[0].avatar_png).toBe("");
        expect(req.people[1].avatar_png).toBe("C:/d/avatars/harness-screen-1.png");
        expect(req.roster.map((p: { user_id: string; name: string }) => [p.user_id, p.name])).toEqual([
            ["harness-voice-4", "さくら"],
            ["harness-voice-5", "Ken"],
        ]);
        expect(req.renders).toEqual([
            { name: "l0", levels: [0, 0], roster_levels: [0, 0] },
            { name: "l1", levels: [1, 1], roster_levels: [1, 1] },
            { name: "l05", levels: [0.5, 0.5], roster_levels: [0.5, 0.5] },
        ]);
        // 参加者ごとの隅は要求に書かない (参照は設定の label_corner を全員に使う)。
        expect(JSON.stringify(req)).not.toContain("corner");
        expect(trialRequest("c1", "s", "b", () => "a").roster).toEqual([]);
    });
});
