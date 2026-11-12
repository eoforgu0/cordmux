/*
 * sender_pure.ts の純粋なロジックをテストする。
 * - assignStreamIds が stream_id を重複させないこと。
 * - SyncCoordinatorPure / PerStreamWhiteFrameGate がマーカーの白フレームをストリームごとに同じ 3 フレーム出すこと。
 * - parseSyncTone / streamHasSyncTone: マーカーの音は 1 ストリームだけに乗る。
 * - waitUntil: 背圧を測れるかどうかは「目標時刻を過ぎていても setTimeout を 1 回挟むか」で決まる。
 * - buildHarnessFacts / parseFailContact: --dock-scenario contact-missing / degraded の facts。
 * - parseResolutionSchedule: 走行中の解像度の変更の予定表の読み取り。綴りの往復は実行スクリプトの側
 *   (run-window-plan.test.ts の resolutionScheduleArg / parseResolutionSchedule) でもテストしており、両側が同じ綴りを読む。
 * - parseHelloVersion / harnessIdentityLogLine: hello の version は同梱の印と同じ文字列で、テスト用の送信側だと
 *   示すのは log の 1 行。
 * - parseFramePauseFile / parseFramePauseTargets / FramePauseLedger / frameIndexAfterPause: フレームだけを止める
 *   機能 (主プロセスの側の読み方は frame_pause_file.test.ts)。
 * - parseScreenNames / parseScreenImages / parseVoiceUsers / coverSourceRect / harnessAvatarGlyph: 表示名、画面に描く画像、
 *   声の持ち主 (画像のファイルを主プロセスで読む側は screen_image_file.test.ts)。
 */
import { describe, expect, it } from "vitest";

import { LadderStage } from "../../../src-vencord/encode/ladder";
import { CORDMUX_BUILD_ID } from "../../../src-vencord/version";
import {
    assignStreamIds,
    buildHarnessAvatars,
    buildHarnessFacts,
    buildHarnessProfiles,
    coverSourceRect,
    frameIndexAfterPause,
    FramePauseLedger,
    HARNESS_DISPLAY_NAME_MAX,
    harnessAvatarGlyph,
    harnessAvatarHue,
    harnessAvatarKey,
    HARNESS_IDENTITY,
    harnessIdentityLogLine,
    HELLO_VERSION_MAX_LENGTH,
    parseFailContact,
    parseFramePauseFile,
    parseFramePauseStuck,
    parseFramePauseTargets,
    parseHelloVersion,
    parseResolutionSchedule,
    parseScreenImages,
    parseScreenNames,
    parseScreenSize,
    parseScreenSizes,
    parseSyncTone,
    parseVoiceUsers,
    PerStreamWhiteFrameGate,
    settleSyncAfterPause,
    streamHasSyncTone,
    SyncCoordinatorPure,
    waitUntil,
} from "./sender_pure";

describe("assignStreamIds", () => {
    it("画面と声で1つの通し番号にする (重複しない)", () => {
        const { screenIds, voiceIds } = assignStreamIds(4, 8);
        expect(screenIds).toEqual([1, 2, 3, 4]);
        expect(voiceIds).toEqual([5, 6, 7, 8, 9, 10, 11, 12]);
        const all = [...screenIds, ...voiceIds];
        expect(new Set(all).size).toBe(all.length);
    });

    it("画面0ストリームでも声は1から始まる", () => {
        const { screenIds, voiceIds } = assignStreamIds(0, 3);
        expect(screenIds).toEqual([]);
        expect(voiceIds).toEqual([1, 2, 3]);
    });

    it("声0ストリームでも画面は1から始まる", () => {
        const { screenIds, voiceIds } = assignStreamIds(2, 0);
        expect(screenIds).toEqual([1, 2]);
        expect(voiceIds).toEqual([]);
    });
});

describe("parseSyncTone", () => {
    it("知っている値はそのまま、知らない値と未指定は既定の screen にする", () => {
        expect(parseSyncTone("screen")).toBe("screen");
        expect(parseSyncTone("voice")).toBe("voice");
        expect(parseSyncTone("none")).toBe("none");
        expect(parseSyncTone(undefined)).toBe("screen");
        expect(parseSyncTone("")).toBe("screen");
        expect(parseSyncTone("SCREEN")).toBe("screen");
        expect(parseSyncTone(1)).toBe("screen");
    });
});

describe("streamHasSyncTone (マーカーの音は1ストリームだけ)", () => {
    it("screen なら最初の画面のストリームだけがマーカーの音を持つ", () => {
        expect(streamHasSyncTone("screen", "screen", 0)).toBe(true);
        expect(streamHasSyncTone("screen", "screen", 1)).toBe(false);
        expect(streamHasSyncTone("screen", "voice", 0)).toBe(false);
    });

    it("voice なら最初の声のストリームだけがマーカーの音を持つ", () => {
        expect(streamHasSyncTone("voice", "voice", 0)).toBe(true);
        expect(streamHasSyncTone("voice", "voice", 1)).toBe(false);
        expect(streamHasSyncTone("voice", "screen", 0)).toBe(false);
    });

    it("none ならどのストリームもマーカーの音を持たない", () => {
        expect(streamHasSyncTone("none", "screen", 0)).toBe(false);
        expect(streamHasSyncTone("none", "voice", 0)).toBe(false);
    });

    it("画面4ストリーム+声8ストリームのどの設定でも、マーカーの音を持つストリームは高々1つ (マーカーが混ざらない)", () => {
        for (const tone of ["screen", "voice", "none"] as const) {
            const screens = [0, 1, 2, 3].filter((i) => streamHasSyncTone(tone, "screen", i));
            const voices = [0, 1, 2, 3, 4, 5, 6, 7].filter((i) => streamHasSyncTone(tone, "voice", i));
            expect(screens.length + voices.length).toBeLessThanOrEqual(1);
        }
    });
});

describe("waitUntil (送りすぎのときは目標時刻を過ぎていてもsetTimeoutを1回挟む)", () => {
    /** 起点よりずっと前 = 目標時刻はとうに過ぎている。 */
    const passedTargetMs = -1_000_000;
    /** close() されないストリーム (stopPromise は永遠に解決しない)。 */
    const neverStops = new Promise<void>(() => {});

    it("alwaysYieldが真なら、目標時刻を過ぎていてもイベントループを1周する", async () => {
        const clock = { startPerfMs: performance.now() };
        let ranBefore = false;
        const timer = setTimeout(() => {
            ranBefore = true;
        }, 0);

        const outcome = await waitUntil(passedTargetMs, clock, neverStops, true);

        clearTimeout(timer);
        expect(outcome).toBe("reached");
        // 先に積んでおいた setTimeout が走った = 待っている間に I/O (WebSocket の送出) が進める。
        // ここが偽だと、接続が一度も進まず、背圧を測れない。
        expect(ranBefore).toBe(true);
    });

    it("alwaysYieldが偽なら、目標時刻を過ぎていると微小タスクだけで返る (定常の刻みは変えない)", async () => {
        const clock = { startPerfMs: performance.now() };
        let ranBefore = false;
        const timer = setTimeout(() => {
            ranBefore = true;
        }, 0);

        const outcome = await waitUntil(passedTargetMs, clock, neverStops, false);

        clearTimeout(timer);
        expect(outcome).toBe("reached");
        expect(ranBefore).toBe(false); // setTimeout を挟んでいない。
    });

    it("目標時刻がまだ先なら、そこまで待ってからreachedを返す", async () => {
        const clock = { startPerfMs: performance.now() };
        const outcome = await waitUntil(20, clock, neverStops);
        expect(outcome).toBe("reached");
        expect(performance.now() - clock.startPerfMs).toBeGreaterThanOrEqual(15);
    });

    it("close() された (stopPromiseが先に解決した) らstoppedを返す", async () => {
        const clock = { startPerfMs: performance.now() };
        let resolveStop!: () => void;
        const stopPromise = new Promise<void>((resolve) => {
            resolveStop = resolve;
        });

        const pending = waitUntil(50, clock, stopPromise);
        resolveStop();

        expect(await pending).toBe("stopped");
    });
});

describe("SyncCoordinatorPure + PerStreamWhiteFrameGate", () => {
    it("境界を跨ぐと、画面が何ストリームあっても同じ3フレームが白になる (全ストリーム合わせて3フレームしか出ない形にならない)", () => {
        const sync = new SyncCoordinatorPure(5);
        const gates = [0, 1, 2, 3].map(() => new PerStreamWhiteFrameGate());

        // 5秒未満はまだ境界を跨がない。
        sync.noteVideoTick(4_999_000n);
        for (const g of gates) expect(g.consume(sync.currentBoundary)).toBe(false);

        // 5秒を跨ぐ (複数のストリームが同じ境界を呼んでも、跨いだ判定は1回だけ進む)。
        sync.noteVideoTick(5_000_000n);
        sync.noteVideoTick(5_001_000n); // 他のストリームが少し遅れて同じ境界を跨いで呼ぶ想定。
        expect(sync.currentBoundary).toBe(1);

        for (const g of gates) expect(g.consume(sync.currentBoundary)).toBe(true); // 1フレーム目: 全ストリーム白
        for (const g of gates) expect(g.consume(sync.currentBoundary)).toBe(true); // 2フレーム目: 全ストリーム白
        for (const g of gates) expect(g.consume(sync.currentBoundary)).toBe(true); // 3フレーム目: 全ストリーム白
        for (const g of gates) expect(g.consume(sync.currentBoundary)).toBe(false); // 4フレーム目: もう白ではない
    });

    it("次の境界でまた3フレーム白になる", () => {
        const sync = new SyncCoordinatorPure(5);
        const gate = new PerStreamWhiteFrameGate();
        sync.noteVideoTick(5_000_000n);
        expect(gate.consume(sync.currentBoundary)).toBe(true);
        expect(gate.consume(sync.currentBoundary)).toBe(true);
        expect(gate.consume(sync.currentBoundary)).toBe(true);
        expect(gate.consume(sync.currentBoundary)).toBe(false);

        sync.noteVideoTick(10_000_000n);
        expect(gate.consume(sync.currentBoundary)).toBe(true);
    });

    it("音のマーカーの周波数は境界から60msだけ2000Hz、それ以外は1000Hz", () => {
        const sync = new SyncCoordinatorPure(5);
        expect(sync.audioFrequencyAt(0n)).toBe(1000);
        sync.noteVideoTick(5_000_000n);
        expect(sync.audioFrequencyAt(5_000_000n)).toBe(2000);
        expect(sync.audioFrequencyAt(5_059_999n)).toBe(2000);
        expect(sync.audioFrequencyAt(5_060_000n)).toBe(1000);
    });

    it("マーカーの音はタイムスタンプだけで決まり、映像の生成の先後に依らない (生成の遅れが A/V のずれに乗らない)", () => {
        const epoch = 1_000_000_000n;
        const sync = new SyncCoordinatorPure(5, epoch);
        // 音の生成が映像より進んでいる (映像がまだ境界を跨いでいない) ときも、境界からの 60 ms は 2 kHz。
        expect(sync.audioFrequencyAt(epoch + 5_000_000n)).toBe(2000);
        // 音の生成が映像より遅れていても、境界より前の音は 1 kHz のまま。
        sync.noteVideoTick(epoch + 5_000_000n);
        expect(sync.audioFrequencyAt(epoch + 4_990_000n)).toBe(1000);
        expect(sync.audioFrequencyAt(epoch + 4_900_000n)).toBe(1000);
        // 白のフレームの記録は境界のタイムスタンプそのもの。
        expect(sync.events).toEqual([{ timestampUs: Number(epoch + 5_000_000n) }]);
        // 2 つ目の境界も同じ。最初の境界の前は鳴らない。
        expect(sync.audioFrequencyAt(epoch + 10_030_000n)).toBe(2000);
        expect(sync.audioFrequencyAt(epoch + 30_000n)).toBe(1000);
    });

    it("everySec <= 0 なら境界を跨がない (マーカーを出さない)", () => {
        const sync = new SyncCoordinatorPure(0);
        sync.noteVideoTick(1_000_000_000n);
        expect(sync.currentBoundary).toBe(0);
        expect(sync.events).toEqual([]);
    });
});

/** 受け手から何も届いていないときの現在値 (録画は OFF、media は開けたまま)。 */
const GATE_DEFAULT = { recording: false, media: true };

describe("buildHarnessFacts (--dock-scenario contact-missing / degraded)", () => {
    it("指定した接点idだけを warnings に載せ、sharing は0人で必須の鍵を満たす", () => {
        const facts = buildHarnessFacts("roster", [], [], GATE_DEFAULT);
        expect(facts).toEqual({
            warnings: ["roster"],
            sharing: { count: 0, user_ids: [] },
            recording: false,
            media: true,
        });
    });

    it("接点idが変われば warnings の中身もそのまま変わる (id の妥当性はここでは検査しない)", () => {
        expect(buildHarnessFacts("sharing", [], [], GATE_DEFAULT).warnings).toEqual(["sharing"]);
        expect(buildHarnessFacts("nonsense-not-a-real-contact", [], [], GATE_DEFAULT).warnings).toEqual([
            "nonsense-not-a-real-contact",
        ]);
    });

    it("段があれば stages に載せる。綴りは encode/ladder.ts の stageWireName と同じ", () => {
        const facts = buildHarnessFacts(
            undefined,
            [
                { streamId: 1, stage: LadderStage.Native },
                { streamId: 2, stage: LadderStage.HalfFps },
                { streamId: 3, stage: LadderStage.Downscaled },
                { streamId: 4, stage: LadderStage.SoftwareFloor },
                { streamId: 5, stage: LadderStage.Unavailable },
            ],
            [],
            GATE_DEFAULT,
        );
        expect(facts).toEqual({
            warnings: [],
            sharing: { count: 0, user_ids: [] },
            recording: false,
            media: true,
            stages: [
                { stream_id: 1, stage: "native" },
                { stream_id: 2, stage: "halffps" },
                { stream_id: 3, stage: "downscaled" },
                { stream_id: 4, stage: "software" },
                { stream_id: 5, stage: "unavailable" },
            ],
        });
    });

    it("stages は stream_id の昇順に並べる (呼び出し元の集め方で並びが変わらない)", () => {
        const facts = buildHarnessFacts(
            undefined,
            [
                { streamId: 3, stage: LadderStage.SoftwareFloor },
                { streamId: 1, stage: LadderStage.Native },
                { streamId: 2, stage: LadderStage.HalfFps },
            ],
            [],
            GATE_DEFAULT,
        );
        expect(facts.stages?.map((s) => s.stream_id)).toEqual([1, 2, 3]);
    });

    it("段が1件も無ければ stages の鍵ごと省く", () => {
        const facts = buildHarnessFacts("roster", [], [], GATE_DEFAULT);
        expect(facts).not.toHaveProperty("stages");
    });

    it("接点も段も共有者もなくても 1 通を組む (recording と media は必ず載せる = 製品の core/facts.ts と同じ)", () => {
        expect(buildHarnessFacts(undefined, [], [], GATE_DEFAULT)).toEqual({
            warnings: [],
            sharing: { count: 0, user_ids: [] },
            recording: false,
            media: true,
        });
    });

    it("受け手から届いた recording / media の値をそのまま載せる (偽でも鍵を省かない)", () => {
        const on = buildHarnessFacts(undefined, [], ["harness-screen-1"], { recording: true, media: true });
        expect(on.recording).toBe(true);
        expect(on.media).toBe(true);
        const off = buildHarnessFacts(undefined, [], ["harness-screen-1"], { recording: false, media: false });
        expect(off).toHaveProperty("recording", false);
        expect(off).toHaveProperty("media", false);
    });

    it("共有している人を sharing に載せる (製品の core/facts.ts と同じ形。受け手の参加者一覧の行になる)", () => {
        const facts = buildHarnessFacts(undefined, [], ["harness-screen-2", "harness-screen-1"], GATE_DEFAULT);
        expect(facts).toEqual({
            warnings: [],
            // 昇順に並べる (呼び出し元の集め方で並びが変わらない = stages と同じ規律)。
            sharing: { count: 2, user_ids: ["harness-screen-1", "harness-screen-2"] },
            recording: false,
            media: true,
        });
    });

    it("同じ userId が 2 回来ても 1 行にする (重複と空の userId を落とす)", () => {
        const facts = buildHarnessFacts(undefined, [], ["a", "a", "", "b"], GATE_DEFAULT);
        expect(facts.sharing).toEqual({ count: 2, user_ids: ["a", "b"] });
    });

    it("接点と段は同じ1通に同居する (degraded で接点も潰した呼び方をしても両方届く)", () => {
        const facts = buildHarnessFacts("roster", [{ streamId: 1, stage: LadderStage.Downscaled }], [], GATE_DEFAULT);
        expect(facts.warnings).toEqual(["roster"]);
        expect(facts.stages).toEqual([{ stream_id: 1, stage: "downscaled" }]);
    });

    it("渡した配列を並べ替えない (呼び出し元が持っている並びを壊さない)", () => {
        const entries = [
            { streamId: 2, stage: LadderStage.Native },
            { streamId: 1, stage: LadderStage.Native },
        ];
        buildHarnessFacts(undefined, entries, [], GATE_DEFAULT);
        expect(entries.map((e) => e.streamId)).toEqual([2, 1]);
    });
});

describe("parseFailContact", () => {
    it("非空の文字列はそのまま返す", () => {
        expect(parseFailContact("roster")).toBe("roster");
    });

    it("空文字列・undefined・非文字列はすべて undefined (送らない) に倒す", () => {
        expect(parseFailContact("")).toBeUndefined();
        expect(parseFailContact(undefined)).toBeUndefined();
        expect(parseFailContact(null)).toBeUndefined();
        expect(parseFailContact(0)).toBeUndefined();
    });
});

// hello の version は同梱の印と同じ文字列。
describe("parseHelloVersion", () => {
    it("defaults to the product CORDMUX_BUILD_ID so it matches the bundled mark build_id", () => {
        // バージョンの食い違いの行は bundled_vencord != discord_side で出る。手元の印は
        // CORDMUX_BUILD_ID と同じ文字列なので、既定のままならテスト用の送信側でこの行は出ない。
        expect(parseHelloVersion(undefined, CORDMUX_BUILD_ID)).toBe(CORDMUX_BUILD_ID);
        expect(parseHelloVersion(null, CORDMUX_BUILD_ID)).toBe(CORDMUX_BUILD_ID);
        expect(CORDMUX_BUILD_ID.startsWith("cordmux-harness")).toBe(false);
    });

    it("uses the value given by the runner (the build_id of the zip mark) as is", () => {
        expect(parseHelloVersion("0.1.0+4474aeb", "0.1.0+dev")).toBe("0.1.0+4474aeb");
        const longest = "a".repeat(HELLO_VERSION_MAX_LENGTH);
        expect(parseHelloVersion(longest, "0.1.0+dev")).toBe(longest);
    });

    it("falls back for empty, whitespace or control chars, non-ASCII, too long, or non-string values", () => {
        const fallback = "0.1.0+dev";
        expect(parseHelloVersion("", fallback)).toBe(fallback);
        expect(parseHelloVersion("0.1.0 +dev", fallback)).toBe(fallback);
        expect(parseHelloVersion("0.1.0+dev\n", fallback)).toBe(fallback);
        expect(parseHelloVersion("0.1.0+d\u00e9v", fallback)).toBe(fallback);
        expect(parseHelloVersion("a".repeat(HELLO_VERSION_MAX_LENGTH + 1), fallback)).toBe(fallback);
        expect(parseHelloVersion(1, fallback)).toBe(fallback);
    });
});

// テスト用の送信側だと示すのは log の 1 行。
describe("harnessIdentityLogLine", () => {
    it("names the harness and the hello version, in printable ASCII only", () => {
        const line = harnessIdentityLogLine("0.1.0+dev");
        expect(line).toContain(HARNESS_IDENTITY);
        expect(line).toContain("0.1.0+dev");
        expect(/^[\x20-\x7e]+$/.test(line)).toBe(true);
    });
});

describe("parseResolutionSchedule", () => {
    it("実行スクリプトが渡す綴り (35:1280x720,60:640x360) を読む", () => {
        expect(parseResolutionSchedule("35:1280x720,60:640x360")).toEqual([
            { atSec: 35, width: 1280, height: 720 },
            { atSec: 60, width: 640, height: 360 },
        ]);
    });

    it("未指定・空文字列・非文字列は空の予定表", () => {
        expect(parseResolutionSchedule(undefined)).toEqual([]);
        expect(parseResolutionSchedule("")).toEqual([]);
        expect(parseResolutionSchedule(null)).toEqual([]);
        expect(parseResolutionSchedule(12)).toEqual([]);
    });

    it("読めない項目だけを落とし、残りは生かす (実行全体を失敗させない)", () => {
        expect(parseResolutionSchedule("35:1280x720,ごみ,60:640,70:0x360,80:-1x2,90:640x360")).toEqual([
            { atSec: 35, width: 1280, height: 720 },
            { atSec: 90, width: 640, height: 360 },
        ]);
    });

    it("順不同で渡しても時刻の昇順に修正する", () => {
        expect(parseResolutionSchedule("60:640x360,35:1280x720").map((c) => c.atSec)).toEqual([35, 60]);
    });

    it("大文字の X と前後の空白を許す (実行スクリプトの綴りに揺れがあっても読めるほうへ倒す)", () => {
        expect(parseResolutionSchedule(" 35:1280X720 , 60:640x360 ")).toEqual([
            { atSec: 35, width: 1280, height: 720 },
            { atSec: 60, width: 640, height: 360 },
        ]);
    });
});

describe("parseScreenSize", () => {
    it("<幅>x<高さ> を読み、大文字の X と前後の空白を許す", () => {
        expect(parseScreenSize("1280x720")).toEqual({ width: 1280, height: 720 });
        expect(parseScreenSize(" 3840X2160 ")).toEqual({ width: 3840, height: 2160 });
    });

    it("正の整数の 2 つでなければ null", () => {
        for (const bad of ["", "x", "1280", "1280x", "x720", "0x720", "1280x0", "-1x2", "1.5x2", "1x2x3", "ax b"]) {
            expect(parseScreenSize(bad)).toBeNull();
        }
    });
});

describe("parseScreenSizes (ストリームごとの最初の寸法)", () => {
    const fb = { width: 1920, height: 1080 };

    it("例 (1280x720,3840x2160) をストリームの順に当てる", () => {
        expect(parseScreenSizes("1280x720,3840x2160", 2, fb)).toEqual([
            { width: 1280, height: 720 },
            { width: 3840, height: 2160 },
        ]);
    });

    it("未指定・空文字列・非文字列は全部のストリームが既定", () => {
        for (const v of [undefined, null, "", 12]) {
            expect(parseScreenSizes(v, 3, fb)).toEqual([fb, fb, fb]);
        }
    });

    it("項目が足りないストリームは既定、多すぎる項目は使わない", () => {
        expect(parseScreenSizes("1280x720", 3, fb)).toEqual([{ width: 1280, height: 720 }, fb, fb]);
        expect(parseScreenSizes("1280x720,3840x2160,640x360", 1, fb)).toEqual([{ width: 1280, height: 720 }]);
    });

    it("読めない項目と空の項目はそのストリームだけ既定にし、後ろの項目をずらさない", () => {
        expect(parseScreenSizes("ごみ,3840x2160", 2, fb)).toEqual([fb, { width: 3840, height: 2160 }]);
        expect(parseScreenSizes(",3840x2160", 2, fb)).toEqual([fb, { width: 3840, height: 2160 }]);
        expect(parseScreenSizes("1280x720, ,0x9,640X360", 4, fb)).toEqual([
            { width: 1280, height: 720 },
            fb,
            fb,
            { width: 640, height: 360 },
        ]);
    });

    it("返す並びの長さはストリームの数 (0 ストリーム・不正な数は空)", () => {
        expect(parseScreenSizes("1280x720", 0, fb)).toEqual([]);
        expect(parseScreenSizes("1280x720", -1, fb)).toEqual([]);
        expect(parseScreenSizes("1280x720", 1.5, fb)).toEqual([]);
    });

    it("既定の寸法はストリームごとに別の物を返す (1 ストリームの寸法を書き換えても他のストリームに移らない)", () => {
        const sizes = parseScreenSizes("", 2, fb);
        sizes[0].width = 1;
        expect(sizes[1]).toEqual(fb);
        expect(fb).toEqual({ width: 1920, height: 1080 });
    });
});

describe("parseFramePauseFile (フレームだけを止める通知ファイルの場所)", () => {
    it("空でない文字列はそのまま (前後の空白は落とす)", () => {
        expect(parseFramePauseFile("C:/work/pause.flag")).toBe("C:/work/pause.flag");
        expect(parseFramePauseFile("  _work/out/pause  ")).toBe("_work/out/pause");
    });

    it("未指定・空・空白だけ・文字列でない値は使わない (undefined)", () => {
        expect(parseFramePauseFile(undefined)).toBeUndefined();
        expect(parseFramePauseFile(null)).toBeUndefined();
        expect(parseFramePauseFile("")).toBeUndefined();
        expect(parseFramePauseFile("   ")).toBeUndefined();
        expect(parseFramePauseFile(1)).toBeUndefined();
    });
});

describe("parseFramePauseTargets (止めるストリームの並び)", () => {
    const screens = [1, 2, 3];

    it("中身が空か空白だけなら全部の画面のストリーム", () => {
        expect(parseFramePauseTargets("", screens)).toEqual({ streams: [1, 2, 3], ignored: [] });
        expect(parseFramePauseTargets(" \r\n", screens)).toEqual({ streams: [1, 2, 3], ignored: [] });
    });

    it("カンマか空白で区切った stream id のストリームだけ (昇順、重複なし)", () => {
        expect(parseFramePauseTargets("2", screens)).toEqual({ streams: [2], ignored: [] });
        expect(parseFramePauseTargets("3, 1\n3", screens)).toEqual({ streams: [1, 3], ignored: [] });
    });

    it("画面のストリームでない項目は止めずに返し、読める項目が無ければ何も止めない", () => {
        expect(parseFramePauseTargets("2,4,x", screens)).toEqual({ streams: [2], ignored: ["4", "x"] });
        expect(parseFramePauseTargets("9", screens)).toEqual({ streams: [], ignored: ["9"] });
        expect(parseFramePauseTargets("1.0,-1,01", screens)).toEqual({ streams: [1], ignored: ["1.0", "-1"] });
    });
});

describe("FramePauseLedger (止めている状態と区間の記録)", () => {
    const at = (state: "off" | "absent" | "present" | "error", wallMs: number, text?: string) =>
        text === undefined ? { state, wallMs } : { state, wallMs, text };

    it("ファイルができたら全部のストリームの区間を開き、消えたら閉じる", () => {
        const ledger = new FramePauseLedger([1, 2]);
        ledger.apply(at("absent", 1000), 10);
        expect(ledger.isPaused(1)).toBe(false);
        ledger.apply(at("present", 2000, ""), 20);
        expect(ledger.isPaused(1)).toBe(true);
        expect(ledger.isPaused(2)).toBe(true);
        ledger.apply(at("absent", 5000), 50);
        expect(ledger.isPaused(1)).toBe(false);
        expect(ledger.intervals).toEqual([
            { streamId: 1, startMs: 20, startWallMs: 2000, endMs: 50, endWallMs: 5000, framesWithheld: 0 },
            { streamId: 2, startMs: 20, startWallMs: 2000, endMs: 50, endWallMs: 5000, framesWithheld: 0 },
        ]);
    });

    it("中身を書き替えると、変わったストリームだけ開け閉めする", () => {
        const ledger = new FramePauseLedger([1, 2]);
        ledger.apply(at("present", 100, "1"), 1);
        ledger.apply(at("present", 200, "1,2"), 2);
        ledger.apply(at("present", 300, "2"), 3);
        expect(ledger.intervals).toEqual([
            { streamId: 1, startMs: 1, startWallMs: 100, endMs: 3, endWallMs: 300, framesWithheld: 0 },
            { streamId: 2, startMs: 2, startWallMs: 200, endMs: null, endWallMs: null, framesWithheld: 0 },
        ]);
        expect(ledger.isPaused(1)).toBe(false);
        expect(ledger.isPaused(2)).toBe(true);
    });

    it("off と error は状態を変えない (error は数える)", () => {
        const ledger = new FramePauseLedger([1]);
        ledger.apply(at("present", 100, ""), 1);
        ledger.apply(at("error", 200), 2);
        ledger.apply(at("off", 300), 3);
        expect(ledger.isPaused(1)).toBe(true);
        expect(ledger.readErrors).toBe(1);
        expect(ledger.intervals).toHaveLength(1);
    });

    it("画面のストリームでない項目を重複なしで覚え、何も止めない", () => {
        const ledger = new FramePauseLedger([1]);
        ledger.apply(at("present", 100, "5"), 1);
        ledger.apply(at("present", 200, "5"), 2);
        expect(ledger.isPaused(1)).toBe(false);
        expect(ledger.ignoredTargets).toEqual(["5"]);
        expect(ledger.intervals).toEqual([]);
    });

    it("whenResumed は止めていなければすぐ、止めていれば解いた時に解決する", async () => {
        const ledger = new FramePauseLedger([1, 2]);
        await expect(ledger.whenResumed(1)).resolves.toBeUndefined();
        ledger.apply(at("present", 100, "1"), 1);
        let resumed = false;
        const waiting = ledger.whenResumed(1).then(() => {
            resumed = true;
        });
        ledger.apply(at("present", 200, "1"), 2); // 同じ組: 解かない
        await Promise.resolve();
        expect(resumed).toBe(false);
        ledger.apply(at("absent", 300), 3);
        await waiting;
        expect(resumed).toBe(true);
    });

    it("作らなかったフレームの数はそのストリームの直近の区間に足す", () => {
        const ledger = new FramePauseLedger([1, 2]);
        ledger.apply(at("present", 100, ""), 1);
        ledger.apply(at("absent", 200), 2);
        ledger.addWithheld(1, 120);
        ledger.addWithheld(2, 0);
        ledger.addWithheld(3, 5); // 区間のないストリームは何もしない
        expect(ledger.intervals.map((i) => [i.streamId, i.framesWithheld])).toEqual([
            [1, 120],
            [2, 0],
        ]);
    });
});

describe("frameIndexAfterPause (再開のときのフレームの番号)", () => {
    it("現在の時刻のフレームへ飛び、飛ばした数を返す", () => {
        // 60 fps で 10 フレーム目の後に止め、3 秒後 (3000 ms) に再開 = 180 フレーム目へ。
        expect(frameIndexAfterPause(10, 3000, 60, 1)).toEqual({ frameIndex: 180, withheld: 170 });
    });

    it("送りすぎの回は番号も 4 倍の速さで進む", () => {
        expect(frameIndexAfterPause(10, 3000, 60, 4)).toEqual({ frameIndex: 720, withheld: 710 });
    });

    it("現在の番号より前へは戻さない", () => {
        expect(frameIndexAfterPause(200, 3000, 60, 1)).toEqual({ frameIndex: 200, withheld: 0 });
        expect(frameIndexAfterPause(180, 3000, 60, 1)).toEqual({ frameIndex: 180, withheld: 0 });
    });
});

describe("settleSyncAfterPause (止めていた間に過ぎたマーカーの白を再開の後に出さない)", () => {
    const frameUs = 16_667n; // 60 fps の 1 フレーム

    /** 白のフレームを出すかを n フレームぶん数える (gate.consume を呼ぶのと同じ形)。 */
    const whites = (gate: PerStreamWhiteFrameGate, sync: SyncCoordinatorPure, n: number): number => {
        let count = 0;
        for (let i = 0; i < n; i++) if (gate.consume(sync.currentBoundary)) count++;
        return count;
    };

    it("止めている間に境界を跨いだら、再開の後に白を出さず、その境界に印を付ける", () => {
        const sync = new SyncCoordinatorPure(5);
        const gate = new PerStreamWhiteFrameGate();
        sync.noteVideoTick(4_000_000n);
        expect(whites(gate, sync, 1)).toBe(0);
        expect(settleSyncAfterPause(sync, gate, 7_000_000n, frameUs)).toBe("skipped");
        expect(whites(gate, sync, 5)).toBe(0);
        expect(sync.events).toEqual([{ timestampUs: 5_000_000, whiteSkippedAfterPause: true }]);
    });

    it("再開のフレームが境界の後の最初のフレームなら、白は正しいので 3 フレーム出す", () => {
        const sync = new SyncCoordinatorPure(5);
        const gate = new PerStreamWhiteFrameGate();
        expect(settleSyncAfterPause(sync, gate, 5_000_000n, frameUs)).toBe("kept");
        expect(whites(gate, sync, 5)).toBe(3);
        expect(sync.events).toEqual([{ timestampUs: 5_000_000 }]);
    });

    it("ほかのストリームが先に記録した境界は印を付けない (そのストリームは白を出している)", () => {
        const sync = new SyncCoordinatorPure(5);
        const paused = new PerStreamWhiteFrameGate();
        sync.noteVideoTick(5_000_000n); // 止めていないストリームが跨いだ
        expect(settleSyncAfterPause(sync, paused, 7_000_000n, frameUs)).toBe("skipped");
        expect(whites(paused, sync, 5)).toBe(0);
        expect(sync.events).toEqual([{ timestampUs: 5_000_000 }]);
    });

    it("白の途中で止めたら、再開の後に残りの白を出さない", () => {
        const sync = new SyncCoordinatorPure(5);
        const gate = new PerStreamWhiteFrameGate();
        sync.noteVideoTick(5_000_000n);
        expect(whites(gate, sync, 1)).toBe(1);
        expect(settleSyncAfterPause(sync, gate, 6_000_000n, frameUs)).toBe("skipped");
        expect(whites(gate, sync, 5)).toBe(0);
    });

    it("境界がまだ無ければ何もしない", () => {
        const sync = new SyncCoordinatorPure(5);
        const gate = new PerStreamWhiteFrameGate();
        expect(settleSyncAfterPause(sync, gate, 3_000_000n, frameUs)).toBe("none");
        expect(sync.events).toEqual([]);
        sync.noteVideoTick(5_000_000n);
        expect(whites(gate, sync, 5)).toBe(3);
    });
});

describe("buildHarnessFacts の roster (声だけの人も通話に参加している人として載せる)", () => {
    it("人を渡すと roster に昇順で重複なく載せ、空の userId は落とす", () => {
        const facts = buildHarnessFacts(undefined, [], ["harness-screen-1"], GATE_DEFAULT, [
            "harness-voice-3",
            "harness-screen-1",
            "harness-voice-2",
            "harness-voice-3",
            "",
        ]);
        expect(facts.roster).toEqual({
            count: 3,
            user_ids: ["harness-screen-1", "harness-voice-2", "harness-voice-3"],
        });
    });

    it("人を渡さなければ roster は鍵ごと省く (分からない状態と区別する)", () => {
        expect("roster" in buildHarnessFacts(undefined, [], [], GATE_DEFAULT)).toBe(false);
        expect("roster" in buildHarnessFacts(undefined, [], [], GATE_DEFAULT, [])).toBe(false);
    });
});

describe("buildHarnessProfiles と buildHarnessAvatars (製品と同じ形の profiles と avatar)", () => {
    const IDS = ["harness-voice-3", "harness-screen-1", "harness-voice-2"];

    it("profiles は user_id、display_name、avatar_key、bot を持ち、アバターの画像は載せない", () => {
        const profiles = buildHarnessProfiles(IDS);
        expect(profiles.map((p) => p.user_id)).toEqual(["harness-screen-1", "harness-voice-2", "harness-voice-3"]);
        for (const p of profiles) {
            expect(p.display_name).toBe(p.user_id);
            expect(p.avatar_key).toBe(harnessAvatarKey(p.user_id));
            expect(p.bot).toBe(false);
            expect(p).not.toHaveProperty("avatar_png");
        }
    });

    it("bot の人を指定すると、その人だけ bot が真になる", () => {
        const profiles = buildHarnessProfiles(IDS, new Set(["harness-voice-2"]));
        expect(profiles.map((p) => p.bot)).toEqual([false, true, false]);
    });

    it("avatar_key は userId ごとに決まり、同じ userId なら変わらず、違う userId なら違う", () => {
        expect(harnessAvatarKey("harness-voice-2")).toBe(harnessAvatarKey("harness-voice-2"));
        expect(harnessAvatarKey("harness-voice-2")).not.toBe(harnessAvatarKey("harness-voice-3"));
        expect(harnessAvatarKey("harness-voice-2")).toMatch(/^harness-[0-9a-f]{8}$/);
        const hue = harnessAvatarHue("harness-voice-2");
        expect(hue).toBeGreaterThanOrEqual(0);
        expect(hue).toBeLessThan(360);
    });

    it("avatar は参加者ごとに 1 通で、profiles の avatar_key と同じ値を持つ", () => {
        const avatars = buildHarnessAvatars(IDS, (id) => `png-${id}`);
        const profiles = buildHarnessProfiles(IDS);
        expect(avatars).toHaveLength(3);
        for (const [i, a] of avatars.entries()) {
            expect(a.user_id).toBe(profiles[i]?.user_id);
            expect(a.avatar_key).toBe(profiles[i]?.avatar_key);
            expect(a.avatar_png).toBe(`png-${a.user_id}`);
        }
    });

    it("画像を作れなかった人と、base64 が 32768 文字を超える人には avatar を送らない", () => {
        const avatars = buildHarnessAvatars(IDS, (id) => {
            if (id === "harness-voice-2") return "";
            if (id === "harness-voice-3") return "A".repeat(32 * 1024 + 1);
            return "A".repeat(32 * 1024);
        });
        expect(avatars.map((a) => a.user_id)).toEqual(["harness-screen-1"]);
    });
});

describe("読み出しが止まった形 (stuck:N) と、受信側の stream_check への答え", () => {
    const at = (state: "absent" | "present", wallMs: number, text?: string) =>
        text === undefined ? { state, wallMs } : { state, wallMs, text };

    it("stuck:N を読み、画面のストリームでない項目は返さない。N だけの項目とは別に読み、どちらも読めない項目にしない", () => {
        expect(parseFramePauseStuck("stuck:2, 1 stuck:9 stuck:x", [1, 2, 3])).toEqual([2]);
        expect(parseFramePauseTargets("stuck:2", [1, 2, 3])).toEqual({ streams: [], ignored: [] });
        expect(parseFramePauseTargets("stuck:2,1", [1, 2, 3])).toEqual({ streams: [1], ignored: [] });
    });

    it("stuck:N のストリームは、stream_check で restarted と答えて止めるのを解き、ファイルから消えるまで止めない", () => {
        const ledger = new FramePauseLedger([1, 2]);
        ledger.apply(at("present", 100, "stuck:2"), 1);
        expect(ledger.isPaused(2)).toBe(true);
        expect(ledger.release(2, 5, 500)).toBe("restarted");
        expect(ledger.isPaused(2)).toBe(false);
        // 同じ中身を読み直しても、始め直したストリームは止めない。
        ledger.apply(at("present", 600, "stuck:2"), 6);
        expect(ledger.isPaused(2)).toBe(false);
        expect(ledger.release(2, 7, 700)).toBe("sending");
        // ファイルから消えた後にもう一度書けば、また止まる。
        ledger.apply(at("absent", 800), 8);
        ledger.apply(at("present", 900, "stuck:2"), 9);
        expect(ledger.isPaused(2)).toBe(true);
        expect(ledger.intervals).toEqual([
            { streamId: 2, startMs: 1, startWallMs: 100, endMs: 5, endWallMs: 500, framesWithheld: 0 },
            { streamId: 2, startMs: 9, startWallMs: 900, endMs: null, endWallMs: null, framesWithheld: 0 },
        ]);
        expect(ledger.checks).toEqual([
            { atMs: 5, streamId: 2, outcome: "restarted" },
            { atMs: 7, streamId: 2, outcome: "sending" },
        ]);
    });

    it("N だけで止めているストリームは Discord が送っていない形なので、not_receiving と答えて止めたままにする", () => {
        const ledger = new FramePauseLedger([1, 2]);
        ledger.apply(at("present", 100, "1"), 1);
        expect(ledger.release(1, 2, 200)).toBe("not_receiving");
        expect(ledger.isPaused(1)).toBe(true);
        expect(ledger.release(2, 3, 300)).toBe("sending");
    });
});

describe("表示名と画像と声の持ち主 (実際の OBS の画面を撮るときの名前ラベルと話している人の光)", () => {
    it("parseScreenNames: i 番目の項目が i 番目の画面の表示名になり、読めない項目はそのストリームだけ userId のまま", () => {
        expect(parseScreenNames("Mika, Rin ,さくら,ゆず", 4)).toEqual(["Mika", "Rin", "さくら", "ゆず"]);
        expect(parseScreenNames(",Rin", 3)).toEqual([null, "Rin", null]);
        expect(parseScreenNames("a,b,c", 2)).toEqual(["a", "b"]);
        expect(parseScreenNames("a\tb,ok", 2)).toEqual([null, "ok"]);
        expect(parseScreenNames("x".repeat(HARNESS_DISPLAY_NAME_MAX), 1)).toEqual(["x".repeat(HARNESS_DISPLAY_NAME_MAX)]);
        expect(parseScreenNames("x".repeat(HARNESS_DISPLAY_NAME_MAX + 1), 1)).toEqual([null]);
        for (const value of [undefined, null, "", 3]) expect(parseScreenNames(value, 2)).toEqual([null, null]);
        expect(parseScreenNames("a", 0)).toEqual([]);
    });

    it("parseScreenImages: i 番目の項目が i 番目の画面の画像になり、空の項目と足りない分は null", () => {
        expect(parseScreenImages("C:/t/a.png, C:/t/b c.png ,,", 4)).toEqual(["C:/t/a.png", "C:/t/b c.png", null, null]);
        expect(parseScreenImages("a.png,b.png,c.png", 2)).toEqual(["a.png", "b.png"]);
        for (const value of [undefined, null, "", 1]) expect(parseScreenImages(value, 1)).toEqual([null]);
    });

    it("parseVoiceUsers: 項目の userId を声の持ち主にし、読めない項目と重なる項目は既定の harness-voice-<番号> にする", () => {
        expect(parseVoiceUsers("harness-screen-1", [5])).toEqual(["harness-screen-1"]);
        expect(parseVoiceUsers(" harness-screen-2 ,", [5, 6])).toEqual(["harness-screen-2", "harness-voice-6"]);
        expect(parseVoiceUsers(undefined, [3, 4])).toEqual(["harness-voice-3", "harness-voice-4"]);
        // 使えない文字、同じ人に 2 本、ほかの声の既定の userId は使わない。
        expect(parseVoiceUsers("さくら,a b,x", [5, 6, 7])).toEqual(["harness-voice-5", "harness-voice-6", "x"]);
        expect(parseVoiceUsers("harness-screen-1,harness-screen-1", [5, 6])).toEqual(["harness-screen-1", "harness-voice-6"]);
        expect(parseVoiceUsers("harness-voice-6,", [5, 6])).toEqual(["harness-voice-5", "harness-voice-6"]);
        expect(parseVoiceUsers("y".repeat(65), [2])).toEqual(["harness-voice-2"]);
        const ids = parseVoiceUsers("harness-voice-6,harness-voice-5", [5, 6]);
        expect(new Set(ids).size).toBe(ids.length);
    });

    it("coverSourceRect: 縦横比を保って枠を覆い、はみ出た分を中央で切る", () => {
        expect(coverSourceRect(1280, 720, 1920, 1080)).toEqual({ x: 0, y: 0, width: 1280, height: 720 });
        expect(coverSourceRect(1000, 1000, 1920, 1080)).toEqual({ x: 0, y: 218.75, width: 1000, height: 562.5 });
        expect(coverSourceRect(2000, 500, 1000, 500)).toEqual({ x: 500, y: 0, width: 1000, height: 500 });
        expect(coverSourceRect(0, 720, 1920, 1080)).toEqual({ x: 0, y: 0, width: 0, height: 720 });
    });

    it("buildHarnessProfiles: 表示名を渡した人だけ display_name がその名前になる", () => {
        const names = new Map([["harness-screen-1", "Mika"]]);
        const profiles = buildHarnessProfiles(["harness-screen-1", "harness-screen-2"], new Set(), names);
        expect(profiles.map((p) => p.display_name)).toEqual(["Mika", "harness-screen-2"]);
        expect(profiles[0]?.avatar_key).toBe(harnessAvatarKey("harness-screen-1"));
    });

    it("harnessAvatarGlyph: 表示名があれば先頭の 1 文字、なければ userId の末尾の 1 文字", () => {
        expect(harnessAvatarGlyph("harness-screen-3", "さくら")).toBe("さ");
        expect(harnessAvatarGlyph("harness-screen-3", "Mika")).toBe("M");
        expect(harnessAvatarGlyph("harness-screen-3", "\u{1F600}x")).toBe("\u{1F600}");
        expect(harnessAvatarGlyph("harness-screen-3", null)).toBe("3");
        expect(harnessAvatarGlyph("harness-voice-12")).toBe("2");
    });
});
