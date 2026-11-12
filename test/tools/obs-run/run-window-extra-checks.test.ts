/*
 * 実際の通話の準備の確認用に run-window-plan.mjs へ足したオプション・ステップ・純粋関数をテストする。
 *   --latency-person: Vesktop のウィンドウのウィンドウキャプチャを空いたセルに置き、人が配信者の共有を大きく表示している間だけ録って
 *           latency.py に掛ける (ツールが無ければ「ツールがない」と記録して進む)。
 *   --skip-export-bundle: 終わりの診断情報の書き出し (保存ダイアログへ SendKeys) を省く。キーを送るステップの数え (keySendingSteps)。
 *   合否のしきい値 (映像と音のずれ・傾き・av2 - av1・4 分割の付け外し・声のミュート・遅延) と、実行の途中で止めるもの (stopOnFail) / 記録するだけのもの。
 *   Vencord の設定ファイルを [Vesktopに追加] の前にコピーし、Vesktop を終了してもらった後にキーの単位で比べる (戻さない)。
 *   finally で vencordDir を戻すか (finallyVencordRestorePlan)、cleanup.ps1 のフラグ、最後の確認と、止める通知ファイル。
 *   配信者の ID を結果とログに書かない (argsForResult)、17-20 桁の数の数え。
 *   バージョンの行の読み (並びに依存しない)、整列の傾きの区切り、参加のメッセージに Vesktop も入る。
 *   使う声の tone_scan.py を --windows で読む (tone_scan.py の実際の出力の形でテストする)。
 *   ログの文字列の全部から配信者の ID を伏せる (redactBroadcasterId。設定ウィンドウの dialog-click の edits と texts に残っていた)。
 *   遅延の録画の枠ごとの名前と矩形を judge-latency のログにコピーする (latencyCellsSeen。人が見つからないときに手で読み直せる)。
 *   ウィンドウキャプチャは人が目印を確認した後に置く。judgeLatency を latency.py の実際の出力 (遅延 3 フレーム) でテストする。
 * run-window-plan.mjs は Node の API を呼ばないので、テストする対象には値だけを渡す。人のアカウントの ID は書かない (作り物だけ)。
 */
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

import {
    AC30_ITEM_LABELS,
    AC31_LABELS,
    TEXT_ID_LOOK_SHOW,
    TEXT_ID_DOCK_ACCOUNT_CHANGE,
    callVoicePersons,
    judgeAc31,
    AC32_SEGMENT_MS,
    ADD_TO_VESKTOP_BASELINE_LABEL,
    AV_DRIFT_DELTA_LIMIT_MS,
    CALL_ONE_OPTION_FLAGS,
    CALL_ONE_VALUE_FLAGS,
    TONE_AUDIBLE_RMS,
    ID_BEARING_FILE_PATTERNS,
    JUDGE_LINES,
    KEY_SENDING_KINDS,
    LATENCY_INPUT_NAME,
    LATENCY_RECORDING_LABEL,
    LATENCY_RECORD_MS,
    LATENCY_TOOL,
    LATENCY_WINDOW_CAPTURE_KIND,
    LATENCY_WINDOW_CAPTURE_SETTINGS,
    REDACTED_BROADCASTER_ID,
    STOP_FILE_NAME,
    TEXT_ID_SETTINGS_UNAVAILABLE_A11Y,
    TEXT_ID_SETTINGS_VERSION_LABELS,
    TEXT_ID_SETTINGS_VERSION_OBS_PLUGIN,
    VENCORD_SETTINGS_BEFORE_NAME,
    VENCORD_SETTINGS_REAL_REL,
    VOICE_SOURCE_CHECKS,
    ac32MarkLabel,
    ac32Windows,
    alignDriftOf,
    alignSegments,
    argsForResult,
    callJoinMessage,
    cleanupFlags,
    countDiscordIdLike,
    deadlineEstimate,
    finallyVencordRestorePlan,
    isDiscordIdShape,
    judgeAc32,
    judgeAvDrift,
    judgeAvSync,
    judgeAvSyncLines,
    AV_MIN_PAIRS,
    AV_TREND_MIN_PAIRS,
    judgeAvTrend,
    judgeLatency,
    judgeLinesSummary,
    judgeVersionRowShown,
    judgeVoiceSourceSwitch,
    keySendingSteps,
    latencyCellsSeen,
    latencyFreeCell,
    latencyHumanMessage,
    latencyReferenceRect,
    latencyReferenceTransform,
    latencyRegions,
    latencyToolArgs,
    latencyValueOf,
    parseArgs,
    plannedMinutes,
    planSteps,
    redactBroadcasterId,
    settingsKeyDiff,
    toneScanWindowsArg,
    toneWindowValues,
    vencordDirFinalCheck,
    vencordSettingsCopied,
    voiceSourceScanWindow,
    DEFAULT_CALL_ACCOUNTS,
} from "./run-window-plan.mjs";

type Step = Record<string, any>;

const HERE = dirname(fileURLToPath(import.meta.url));
const BASE = ["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-1"];
const REAL = [...BASE, "--real-profile", "--call-window", "--decoder", "software"];
const REHEARSAL = [...BASE, "--rehearsal"];
// 実際の通話での呼び出し行の形。配信者の ID は作り物の 1、遅延を読む人の名前も作り物 (実際の値は呼び出し行でだけ渡す)。
const PERSON = "person-x";
// 参加のメッセージのボイスチャンネルの名前も作り物 (実際の名前は、実行するときの呼び出し行でだけ渡す)。
const CHANNEL = "lobby";
const STAGE = [
    "--n-screen", "3", "--n-voice", "3", "--broadcaster-id", "1",
    "--start-with-add", "--vesktop-share", "--skip-continuous", "--skip-speaker-mute", "--extra-checks",
    "--skip-export-bundle", "--latency-person", PERSON, "--voice-channel", CHANNEL,
];
// 画面の枠の名前の形 (受信側が組む名前の形の例)。
const SCREEN_FORM = "%1 (cordmux画面共有 %2)";

function plan(argv: string[]) {
    const parsed = parseArgs(argv);
    expect(parsed.errors).toEqual([]);
    const r = planSteps(parsed.args);
    expect(r.ok).toBe(true);
    return { args: parsed.args, steps: r.steps as Step[] };
}
const at = (steps: Step[], pred: (s: Step) => boolean) => steps.findIndex(pred);
const kinds = (steps: Step[]) => steps.map((s) => s.kind);

// tone_scan.py の実際の出力 (作成した WAV に、同梱の ffmpeg で実行して得た 1 行の JSON をそのままコピーした)。
//   tone8.wav: 8 秒の 1 kHz (振幅 0.3)、quiet8.wav: 8 秒の無音。--hz 1000 --windows "all=0.5:7.5"。
//   ac32.wav: 声 1 の基準 3 秒 (振幅 0.2) -> 200% 3 秒 (0.4) -> 100% 3 秒 (0.2) -> ミュート 3 秒、声 2 と声 3 は 12 秒ずつ無音。
const TONE_SCAN_TONE = {"hz": 1000.0, "chunk_ms": 20, "ratio": 0.5, "files": [{"file": "tone8.wav", "duration_s": 8.0, "rms": 0.129832, "peak": 0.299988, "tone_chunks": 400, "bursts": 1, "first_tone_s": 0.0, "last_tone_s": 7.98, "windows": [{"label": "all", "start_s": 0.5, "end_s": 7.5, "duration_s": 7.0, "rms": 0.212125, "tone_share": 1.0}]}]};
const TONE_SCAN_QUIET = {"hz": 1000.0, "chunk_ms": 20, "ratio": 0.5, "files": [{"file": "quiet8.wav", "duration_s": 8.0, "rms": 0.0, "peak": 0.0, "tone_chunks": 0, "bursts": 0, "first_tone_s": null, "last_tone_s": null, "windows": [{"label": "all", "start_s": 0.5, "end_s": 7.5, "duration_s": 7.0, "rms": 0.0, "tone_share": 0.0}]}]};
const TONE_SCAN_NO_WINDOWS = {"hz": 1000.0, "chunk_ms": 20, "ratio": 0.5, "files": [{"file": "tone8.wav", "duration_s": 8.0, "rms": 0.129832, "peak": 0.299988, "tone_chunks": 400, "bursts": 1, "first_tone_s": 0.0, "last_tone_s": 7.98}]};
const TONE_SCAN_AC32_WINDOWS = [1, 2, 3].flatMap((v) =>
    ["base", "muted", "restored"].map((seg, k) => ({
        label: "v" + v + "-" + seg,
        start_s: (v - 1) * 12 + k * 3 + 0.5,
        end_s: (v - 1) * 12 + k * 3 + 2.5,
        duration_s: 2.0,
        rms: v === 1 && seg !== "muted" ? 0.141414 : 0.0,
        tone_share: v === 1 && seg !== "muted" ? 1.0 : 0.0,
    })),
);
// 上の ac32.wav の区間のマーカー (録画の開始からの ms)。
function ac32Marks(): Record<string, number> {
    const marks: Record<string, number> = {};
    for (let v = 1; v <= 3; v += 1) {
        ["base", "muted", "restored", "end"].forEach((seg, k) => {
            marks[ac32MarkLabel(v, seg)] = (v - 1) * 12000 + k * 3000;
        });
    }
    return marks;
}

describe("オプション (--skip-export-bundle・--latency-person)", () => {
    it("--skip-export-bundle は値を取らないオプションで、--latency-person は名前を取る。どちらも call-1 だけ", () => {
        expect(CALL_ONE_OPTION_FLAGS).toContain("--skip-export-bundle");
        expect(CALL_ONE_VALUE_FLAGS).toEqual(["--latency-person", "--voice-channel"]);
        const p = parseArgs([...REAL, ...STAGE]);
        expect(p.errors).toEqual([]);
        expect(p.args).toMatchObject({ skipExportBundle: true, latencyPerson: PERSON });
        const plain = parseArgs(REAL).args;
        expect(plain).toMatchObject({ skipExportBundle: false, latencyPerson: null });
        for (const flag of [["--skip-export-bundle"], ["--latency-person", PERSON]]) {
            expect(parseArgs(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-hold", "--real-profile", ...flag]).errors.join(" ")).toContain(flag[0] + " は --step call-1 だけ");
        }
    });

    it("--latency-person は画面の枠 2 か 3 つと一緒に (空いたセルが要る)。空の名前と制御文字は断る", () => {
        expect(parseArgs([...REAL, "--latency-person", PERSON]).errors.join(" ")).toContain("--n-screen 2 か 3");
        expect(parseArgs([...REAL, "--n-screen", "4", "--latency-person", PERSON]).ok).toBe(false);
        expect(parseArgs([...REAL, "--n-screen", "1", "--latency-person", PERSON]).ok).toBe(false);
        expect(parseArgs([...REAL, "--n-screen", "2", "--latency-person", PERSON]).ok).toBe(true);
        expect(parseArgs([...REAL, "--n-screen", "3", "--latency-person", "a\u0007b"]).ok).toBe(false);
        expect(parseArgs([...REAL, "--n-screen", "3", "--latency-person", "x".repeat(65)]).ok).toBe(false);
    });

    it("--voice-channel は名前を取り、call-1 だけが受ける。前後の空白は除き、空の名前・制御文字・65 文字以上は断る", () => {
        expect(parseArgs([...REAL, ...STAGE]).args).toMatchObject({ voiceChannel: CHANNEL });
        expect(parseArgs(REAL).args).toMatchObject({ voiceChannel: null });
        expect(parseArgs([...REAL, "--voice-channel", " " + CHANNEL + " "]).args).toMatchObject({ voiceChannel: CHANNEL });
        expect(parseArgs(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-hold", "--real-profile", "--voice-channel", CHANNEL]).errors.join(" ")).toContain("--voice-channel は --step call-1 だけ");
        expect(parseArgs([...REAL, "--voice-channel", " "]).errors.join(" ")).toContain("--voice-channel は 1 から 64 文字の名前");
        expect(parseArgs([...REAL, "--voice-channel", "a\u0007b"]).ok).toBe(false);
        expect(parseArgs([...REAL, "--voice-channel", "x".repeat(64)]).ok).toBe(true);
        expect(parseArgs([...REAL, "--voice-channel", "x".repeat(65)]).ok).toBe(false);
    });

    it("リハーサルも同じオプションを受ける", () => {
        expect(parseArgs([...REHEARSAL, ...STAGE]).errors).toEqual([]);
    });
});

describe("キーを送るステップ", () => {
    it("実際の通話とリハーサルにキーを送るステップはない。オプションが無ければ終わりの書き出しが 1 つ残る (標準の実行計画は変えない)", () => {
        expect(KEY_SENDING_KINDS).toEqual(["dialog-sendkeys", "send-keys"]);
        expect(keySendingSteps(plan([...REAL, ...STAGE]).steps)).toEqual([]);
        expect(keySendingSteps(plan([...REHEARSAL, ...STAGE]).steps)).toEqual([]);
        const old = plan([...REAL, "--broadcaster-id", "1"]).steps;
        expect(keySendingSteps(old).map((s: Step) => s.kind)).toEqual(["dialog-sendkeys"]);
        expect(old.some((s) => s.kind === "check-bundle-zip")).toBe(true);
        expect(plan([...REAL, ...STAGE]).steps.some((s) => s.kind === "check-bundle-zip" || s.kind === "dialog-sendkeys")).toBe(false);
    });

    it("別アカウント方式はドックの [変更] のメニューで選ぶ (キーを送らない)", () => {
        const steps = plan([...REAL, ...STAGE]).steps;
        const menu = steps.findIndex((s) => s.kind === "dialog-click" && s.buttonTextId === TEXT_ID_DOCK_ACCOUNT_CHANGE);
        expect(menu).toBeGreaterThanOrEqual(0);
        expect(steps[menu]).toMatchObject({ controlType: "Button", noWait: true, gate: true });
        expect(steps[menu + 1]).toMatchObject({ popup: true, controlType: "MenuItem", noWait: true, gate: true });
        expect(typeof steps[menu + 1].buttonText).toBe("string");
        expect(keySendingSteps([{ kind: "dialog-sendkeys", label: "x" }, { kind: "dialog-click", label: "z" }])).toEqual([{ id: null, kind: "dialog-sendkeys", label: "x" }]);
    });
});

describe("遅延を測る区間", () => {
    const { steps } = plan([...REAL, ...STAGE]);
    const rehearsal = plan([...REHEARSAL, ...STAGE]).steps;

    it("並び: 操作の数の記録の後・別アカウント方式の前に、人が大きく表示 -> 置く -> 待つ -> 捉えたか -> 2 分録る -> 外す (置くのは人が目印を確認した後)", () => {
        for (const list of [steps, rehearsal]) {
            const place = at(list, (s) => s.kind === "place-window-capture");
            const start = place - 1;
            expect(start).toBeGreaterThan(at(list, (s) => s.kind === "count-operations"));
            expect(place).toBeLessThan(at(list, (s) => s.kind === "dialog-click" && s.buttonTextId === TEXT_ID_DOCK_ACCOUNT_CHANGE));
            expect(kinds(list.slice(start, start + 9))).toEqual(["human", "place-window-capture", "wait", "check-window-capture", "switch-to-monitor-capture", "record-start", "wait", "record-stop", "remove-window-capture"]);
            expect(list[start + 5]).toMatchObject({ label: LATENCY_RECORDING_LABEL, stream: false });
            // 録る長さは実際の通話で 2 分 (リハーサルは待ちを縮める)。
            if (list === steps) expect(list[start + 6]).toMatchObject({ ms: LATENCY_RECORD_MS });
            expect(list[start + 7]).toMatchObject({ label: LATENCY_RECORDING_LABEL, tailSeconds: 0 });
            expect(list[start + 8]).toMatchObject({ inputName: LATENCY_INPUT_NAME });
            // 置く先は画面の枠 3 つの 2x2 の空き (0 始まりの 3 = 右下)。
            expect(list[place]).toMatchObject({ cell: 3, sceneName: "Main", inputKind: LATENCY_WINDOW_CAPTURE_KIND, inputName: LATENCY_INPUT_NAME });
            // 置いてから外すまでの間に人の操作の待ちはない (Vesktop のウィンドウが配信と録画に入るのは、目印の後の待ち・確認・2 分だけ)。
            const remove = at(list, (s) => s.kind === "remove-window-capture");
            expect(list.slice(place, remove).some((s) => s.kind === "human")).toBe(false);
        }
    });

    it("人へのメッセージは呼び出し行の名前を使い (文字列として持たない)、録る長さと最小化しないことを伝える", () => {
        const human = steps[at(steps, (s) => s.kind === "place-window-capture") - 1];
        expect(human.kind).toBe("human");
        expect(human.message).toBe(latencyHumanMessage(PERSON));
        expect(human.message).toContain(PERSON);
        expect(human.message).toContain("最小化しない");
        // 人に見せるメッセージでは、英数字と日本語の間に半角の空白を入れない。
        expect(human.message).not.toMatch(/[A-Za-z0-9] [\u3041-\u3093\u30a1-\u30f6\u4e00-\u9fa0]|[\u3041-\u3093\u30a1-\u30f6\u4e00-\u9fa0] [A-Za-z0-9]/);
    });

    it("ウィンドウキャプチャは vesktop.exe のウィンドウだけ (実行ファイルを優先)、クラスは空でない (空だと OBS はウィンドウを探さない)、タイトルを持たない、音とカーソルを入れない", () => {
        const [title, cls, exe] = LATENCY_WINDOW_CAPTURE_SETTINGS.window.split(":");
        expect(title).toBe("");
        expect(cls).toBe("Chrome_WidgetWin_1");
        expect(exe).toBe("vesktop.exe");
        expect(LATENCY_WINDOW_CAPTURE_SETTINGS).toMatchObject({ priority: 2, method: 2, cursor: false, capture_audio: false });
    });

    it("判定 (latency.py) は OBS を閉じた後・OBS の録画を残す前。人はリハーサルではテスト用の送信側の画面の 1 つ目。gate にしない", () => {
        for (const [list, person] of [[steps, PERSON], [rehearsal, "harness-screen-1"]] as const) {
            const i = at(list, (s) => s.kind === "judge-latency");
            expect(i).toBeGreaterThan(list.map((s) => s.kind).lastIndexOf("close-obs"));
            expect(i).toBeLessThan(at(list, (s) => s.kind === "keep-obs-recordings"));
            expect(list[i]).toMatchObject({ recording: LATENCY_RECORDING_LABEL, person, cells: 3, tool: LATENCY_TOOL });
            expect(list[i].gate).toBeUndefined();
        }
        const starts = steps.filter((s) => s.kind === "record-start").map((s) => s.label);
        expect(new Set(starts).size).toBe(starts.length);
    });

    it("オプションが無ければ遅延の区間はない", () => {
        const without = plan([...REAL, ...STAGE.slice(0, STAGE.indexOf("--latency-person"))]).steps;
        expect(without.some((s) => ["place-window-capture", "check-window-capture", "switch-to-monitor-capture", "remove-window-capture", "judge-latency"].includes(s.kind))).toBe(false);
    });

    it("空いたセルと置き方: 2x2 の右下 (3 つ) / 左下 (2 つ)。縦横の比を保ってセルに収める。録画の上の矩形は出力のサイズで割り直す", () => {
        expect([1, 2, 3, 4].map(latencyFreeCell)).toEqual([null, 2, 3, null]);
        const rect = latencyReferenceRect({ cell: 3, videoSettings: { baseWidth: 1920, baseHeight: 1080, outputWidth: 1280, outputHeight: 720 } });
        expect(rect.base).toEqual({ x: 960, y: 540, w: 960, h: 540 });
        expect(rect.output).toEqual({ x: 640, y: 360, w: 640, h: 360 });
        expect(latencyReferenceRect({ cell: 2, videoSettings: {} }).base).toEqual({ x: 0, y: 540, w: 960, h: 540 });
        expect(latencyReferenceTransform(rect)).toMatchObject({ positionX: 960, positionY: 540, boundsType: "OBS_BOUNDS_SCALE_INNER", boundsWidth: 960, boundsHeight: 540, alignment: 5 });
    });

    it("latencyRegions: a は名前の %1 で探した人の枠 (録画の開始と終了の重なり)、b は置いたセル。人の枠が無ければ測らない", () => {
        const capture = (when: string) => ({
            when,
            ok: true,
            canvas: { width: 1920, height: 1080 },
            cells: [
                { cell: 0, sourceName: "other (cordmux画面共有 1)", x: 0, y: 0, w: 960, h: 540, bitCell: 8 },
                { cell: 1, sourceName: PERSON + " (cordmux画面共有 2)", x: 960, y: 0, w: 960, h: 540, bitCell: 8 },
            ],
        });
        const reference = { x: 960, y: 540, w: 960, h: 540 };
        const r = latencyRegions({ captures: [capture("start"), capture("end")], cells: 3, person: PERSON, personForm: SCREEN_FORM, reference });
        expect(r).toMatchObject({ personFound: true, cell: 1, regionB: reference, reason: null });
        expect(r.regionA).toMatchObject({ cell: 1, x: 960, y: 0, w: 960, h: 540, regionSource: "composite-layout" });
        const missing = latencyRegions({ captures: [capture("start")], cells: 3, person: "nobody", personForm: SCREEN_FORM, reference });
        expect(missing).toMatchObject({ personFound: false, regionA: null });
        expect(missing.reason).toContain("nobody");
        // latency.py は --json を持たない。--help にあるバージョンだけに足す。
        const argv = { recording: "C:/rec/latency.mkv", regionA: { x: 1, y: 2, w: 3, h: 4 }, regionB: { x: 5, y: 6, w: 7, h: 8 } };
        expect(latencyToolArgs(argv)).toEqual(["C:/rec/latency.mkv", "--region-a", "1,2,3,4", "--region-b", "5,6,7,8"]);
        expect(latencyToolArgs({ ...argv, jsonFlag: true }).at(-1)).toBe("--json");
    });

    // latency.py の出力の形 (judge.delay_ms = cordmux_delay_ms の平均。正 = cordmux が遅い。capture_bias.approx_ms = 1 フレーム)。
    const tool = (delay: number, extra: Record<string, any> = {}) => ({
        measured: true,
        pairs: 7000,
        cordmux_delay_ms: { mean: delay, median: delay },
        judge: { statistic: "mean", delay_ms: delay, level: "within_detect" },
        capture_bias: { approx_ms: 16.7, subtracted: false },
        warnings: [],
        ...extra,
    });

    it("judgeLatency: latency.py の平均にウィンドウキャプチャの 1 フレームを足した見積もりを目標と比べる (45 ms 以上は設計の見直しが必要、90 ms 以上は失敗)", () => {
        expect(judgeLatency({ data: tool(20), fps: 60 })).toMatchObject({ ok: true, measuredMs: 20, captureBiasMs: 16.7, estimatedMs: 36.7, withinDetectionTarget: true, designQuestion: false, statistic: "mean", pairs: 7000 });
        expect(judgeLatency({ data: tool(40), fps: 60 })).toMatchObject({ ok: true, estimatedMs: 56.7, withinDetectionTarget: false, designQuestion: true });
        const over = judgeLatency({ data: tool(80), fps: 60 });
        expect(over).toMatchObject({ ok: false, estimatedMs: 96.7, withinAcceptableTarget: false });
        expect(over.reason).toContain("90");
        // 境界は latency.py と同じ (以上で超える)。
        expect(judgeLatency({ data: tool(28.3), fps: 60 })).toMatchObject({ estimatedMs: 45, withinDetectionTarget: false });
        // 測れなかった (measured false) は理由つきで失敗。
        expect(judgeLatency({ data: { measured: false, judge: { reason: "領域 B に帯が見つからない" } }, fps: 60 })).toMatchObject({ ok: false, reason: "領域 B に帯が見つからない", measuredMs: null });
        expect(judgeLatency({ data: {}, fps: 60 })).toMatchObject({ ok: false, measuredMs: null });
        expect(judgeLatency({ data: tool(-30), fps: 60 }).note).toContain("negative");
        // 偏りは latency.py の値を先に使い、無ければ fps から求める。別の形 (median_ms) も読む。
        expect(judgeLatency({ data: { median_ms: 30 }, fps: null })).toMatchObject({ estimatedMs: 30, captureBiasMs: null, statistic: "median" });
        expect(judgeLatency({ data: { median_ms: 30 }, fps: 30 })).toMatchObject({ captureBiasMs: 33.3, estimatedMs: 63.3 });
    });

    // latency.py の実際の出力 (latency.py --selftest --keep が作った delay3_wrap.mkv (960x540・60 fps・
    // 3 秒。セル B の帯よりセル A の帯が 3 フレーム遅れ、24 ビットの循環をまたぐ) に、同梱の ffmpeg で --region-a 0,0,480,270
    // --region-b 480,270,480,270 を指定して得た JSON をそのままコピーした。実行した PC のパスを持つ file と ffmpeg の 2 つのキーだけ置き換えた)。
    const LATENCY_PY_DELAY3 = {"file":"delay3_wrap.mkv","measured":true,"width":960,"height":540,"r_frame_rate":"60/1","time_source":"pts","fps":60.007,"frames":180,"output_frame_ms":16.66,"region_a":{"region":{"x":0,"y":0,"w":480,"h":270},"barcode_found":true,"usable_frames":180,"status":{"ok":180,"small":0,"crc":0,"lost":0},"reference_cell_w":6,"first":16777113,"last":76,"span_s":2.983,"advance_per_s":60.007,"advance_span_s":2.983,"searches":1,"forced_searches":1,"switches":0,"geometry":[{"x":46,"y":36,"cell_w":6,"cell_h":6,"at":0,"usable_frames":180,"small":false}],"geometry_changes":0,"places":1},"region_b":{"region":{"x":480,"y":270,"w":480,"h":270},"barcode_found":true,"usable_frames":180,"status":{"ok":180,"small":0,"crc":0,"lost":0},"reference_cell_w":8,"first":16777116,"last":79,"span_s":2.983,"advance_per_s":60.007,"advance_span_s":2.983,"searches":1,"forced_searches":1,"switches":0,"geometry":[{"x":548,"y":328,"cell_w":8,"cell_h":8,"at":0,"usable_frames":180,"small":false}],"geometry_changes":0,"places":1},"frame_status":{"ok":{"ok":180,"small":0,"crc":0,"lost":0},"small":{"ok":0,"small":0,"crc":0,"lost":0},"crc":{"ok":0,"small":0,"crc":0,"lost":0},"lost":{"ok":0,"small":0,"crc":0,"lost":0}},"buckets":{"paired":180,"implausible":0,"a_only":0,"b_only":0,"neither":0},"pairs":180,"implausible":{"n":0,"max_abs_ms":2000,"median_a_minus_b_ms":null},"band_rate":{"used_per_s":60,"source":"--band-fps","ms_per_unit":16.667,"measured_a_per_s":60.007,"measured_b_per_s":60.007},"a_minus_b_ms":{"n":180,"mean":-50,"median":-50,"p5":-50,"p95":-50,"min":-50,"max":-50},"cordmux_delay_ms":{"n":180,"mean":50,"median":50,"p5":50,"p95":50,"min":50,"max":50},"delay_histogram":[{"units":3,"ms":50,"pairs":180}],"by_minute":[{"minute":0,"pairs":180,"mean_ms":50}],"slope_ms_per_min":0,"capture_bias":{"approx_ms":16.7,"subtracted":false,"note":"セル B (Discord のウィンドウのウィンドウキャプチャ) はウィンドウが画面に出てから約 1 フレーム遅れて OBS に届く見込み (測っていない)。その分 cordmux_delay_ms は実際の遅延より小さく出る。引かずに偏りとして記録する"},"judge":{"statistic":"mean","delay_ms":50,"detect_ms":45,"target_ms":90,"level":"within_target","over_detect":true,"over_target":false,"share_over_detect":1,"share_over_target":0,"with_capture_bias":{"delay_ms":66.7,"level":"within_target"},"gate":false,"min_pairs":60},"warnings":[],"note":"a_minus_b_ms = A (cordmux の枠) の番号 - B (Discord のウィンドウ) の番号を ms にした値で、cordmux が遅いと負。cordmux_delay_ms = B - A (正 = cordmux の出力が Discord 自身の再生より遅い)。1 組の値は番号の整数の差なので、中央値・百分位・最小・最大は 16.7 ms の刻み (共有のフレームの間隔の倍数に寄る)。判定は cordmux_delay_ms の平均で、検知限 45.0 ms・許容限 90.0 ms (ITU-R BT.1359-1、音が先の側)。実行は止めない (gate: false)","ffmpeg":"ffmpeg"};

    it("judgeLatency (latency.py の実際の出力): 3 フレームの遅延 50.0 ms に偏り 1 フレーム 16.7 ms を足した 66.7 ms は許容限の内・検知限の外 (設計の見直しが必要)。実行スクリプトが読むキーは latency.py が書くキーと合う", () => {
        expect(latencyValueOf(LATENCY_PY_DELAY3)).toEqual({ value: 50, statistic: "mean", reason: null });
        const r = judgeLatency({ data: LATENCY_PY_DELAY3, fps: 60 });
        expect(r).toMatchObject({ ok: true, measuredMs: 50, captureBiasMs: 16.7, estimatedMs: 66.7, withinDetectionTarget: false, withinAcceptableTarget: true, designQuestion: true, statistic: "mean", pairs: 180, toolLevel: "within_target", warnings: [] });
        // 実行スクリプトの見積もり (estimatedMs) は latency.py の偏りを足した段階 (judge.with_capture_bias) と同じ値。judge.level は偏りを足す前の段階。
        expect(r.estimatedMs).toBe(LATENCY_PY_DELAY3.judge.with_capture_bias.delay_ms);
        // 片方がキーの名前を変えたときに気付けるように、実行スクリプトが読むキー (latencyValueOf・judgeLatency) を latency.py のテキストから探す。
        const tool = readFileSync(join(HERE, LATENCY_TOOL), "utf8");
        for (const key of ["measured", "pairs", "judge", "delay_ms", "statistic", "level", "reason", "with_capture_bias", "cordmux_delay_ms", "capture_bias", "approx_ms", "warnings"]) {
            expect(tool, key).toContain('"' + key + '"');
        }
    });

    it("latencyCellsSeen: 遅延の録画の開始と終了に読んだ枠ごとの名前と矩形をコピーする (人が見つからないときに名前でセル A を選べる)", () => {
        const captures = [
            {
                when: "start",
                ok: true,
                canvas: { width: 1920, height: 1080 },
                cells: [{ cell: 0, sourceName: "other (cordmux画面共有 1)", x: 0, y: 0, w: 960, h: 540, bitCell: 8, scaleX: 1, scaleY: 1, sourceWidth: 1920, sourceHeight: 1080 }],
                skipped: [{ cell: 1, sourceName: "cordmux画面共有 2", reason: "the source has no size yet (no frame has arrived)" }],
            },
            { when: "end", ok: false, error: "x", cells: [], skipped: [] },
            null,
        ];
        expect(latencyCellsSeen(captures)).toEqual([
            {
                when: "start",
                ok: true,
                cells: [{ cell: 0, sourceName: "other (cordmux画面共有 1)", x: 0, y: 0, w: 960, h: 540 }],
                skipped: [{ cell: 1, sourceName: "cordmux画面共有 2", reason: "the source has no size yet (no frame has arrived)" }],
            },
            { when: "end", ok: false, cells: [], skipped: [] },
        ]);
        expect(latencyCellsSeen(undefined)).toEqual([]);
    });

    it("実行スクリプト: judge-latency のログに枠ごとの名前と矩形をコピーし、record-start と record-stop の枠のログにも名前を書く (テキストとして読む)", () => {
        const source = readFileSync(join(HERE, "run-window.mjs"), "utf8");
        expect(source).toContain("const cellsSeen = latencyCellsSeen(state.cellRegionCaptures[s.recording] || []);");
        expect(source).toContain("const base = { recording, person: s.person || null, regions, cellsSeen, fps: ref ? ref.fps : null };");
        expect(source).toContain("cells: (capture.cells || []).map((c) => ({ cell: c.cell, sourceName: c.sourceName ?? null,");
    });
});

describe("映像と音のずれのしきい値 (音の時刻 - 映像の時刻。負 = 音が先)", () => {
    const syncData = (mean: number | null, paired: number, trend?: Record<string, any>) => ({
        paired,
        audio_minus_video_ms: { mean, min: mean, max: mean },
        trend: trend || { pairs: paired, span_s: 600, slope_ms_per_min: 0.1, slope_ci95_ms_per_min: [-0.2, 0.4] },
    });

    it("音が先 45 ms 未満・音が遅れ 125 ms 未満がしきい値の内 (境界は外)", () => {
        expect(judgeAvSyncLines(syncData(-44.9, 60))).toMatchObject({ ok: true, verdict: "within", direction: "audio-leads" });
        expect(judgeAvSyncLines(syncData(-45, 60))).toMatchObject({ ok: false, verdict: "outside" });
        expect(judgeAvSyncLines(syncData(124.9, 60))).toMatchObject({ ok: true, direction: "audio-lags" });
        expect(judgeAvSyncLines(syncData(125, 60))).toMatchObject({ ok: false });
        expect(judgeAvSyncLines(syncData(null, 0))).toMatchObject({ ok: null, verdict: "no-pairs" });
    });

    it("傾き: 1 ms/分を超えて 95% の幅が 0 を含まなければ欠陥。超えても幅が 0 を含めば判定できない。しきい値の内は長い通話で確認する。組が少ない・短い録画は判定しない", () => {
        expect(judgeAvTrend({ pairs: 120, span_s: 595, slope_ms_per_min: 3.69, slope_ci95_ms_per_min: [3.41, 3.97] }).verdict).toBe("defect");
        expect(judgeAvTrend({ pairs: 120, span_s: 595, slope_ms_per_min: -3.5, slope_ci95_ms_per_min: [-4.0, -3.0] }).verdict).toBe("defect");
        expect(judgeAvTrend({ pairs: 60, span_s: 295, slope_ms_per_min: 1.5, slope_ci95_ms_per_min: [-0.1, 3.1] }).verdict).toBe("undetermined");
        expect(judgeAvTrend({ pairs: 60, span_s: 295, slope_ms_per_min: 0.74, slope_ci95_ms_per_min: [-0.08, 1.55] }).verdict).toBe("long-call");
        expect(judgeAvTrend({ pairs: 6, span_s: 25, slope_ms_per_min: 9, slope_ci95_ms_per_min: [8, 10] }).verdict).toBe("not-judged");
        expect(judgeAvTrend({ pairs: 60, span_s: 295, slope_ms_per_min: 2 }).verdict).toBe("not-judged");
        // 傾きが欠陥なら、平均がしきい値の内でも失敗。
        expect(judgeAvSyncLines(syncData(20, 120, { pairs: 120, span_s: 595, slope_ms_per_min: 3.69, slope_ci95_ms_per_min: [3.41, 3.97] }))).toMatchObject({ ok: false, verdict: "within" });
    });

    it("judgeAvSync: 組のあるセルだけを判定し、どれもしきい値の内なら合格。組のあるセルが 1 つも無ければ失敗 (測れていない)", () => {
        const judged = { ok: true, perCell: [{ cell: 0, data: syncData(null, 0) }, { cell: 1, data: syncData(30, 240) }, { cell: 2, data: syncData(null, 0) }] };
        const r = judgeAvSync(judged);
        expect(r).toMatchObject({ ok: true, judgedCells: 1 });
        expect(r.cells.map((c: any) => c.ok)).toEqual([null, true, null]);
        expect(judgeAvSync({ ok: true, perCell: [{ cell: 0, data: syncData(null, 0) }] }).ok).toBe(false);
        expect(judgeAvSync({ ok: true, perCell: [{ cell: 1, data: syncData(150, 10) }] })).toMatchObject({ ok: false, reason: "outside the line on cell 1" });
        expect(judgeAvSync(null).ok).toBe(false);
        expect(judgeAvSync({ ok: false, error: "録画がない" }).reason).toContain("録画がない");
        // av1 / av2 の 1 セルの形 (data だけ)。
        expect(judgeAvSync({ ok: true, data: syncData(-10, 60) }).ok).toBe(true);
    });

    it("judgeAvDrift: deltaLimitMs を渡すとセルごとの |av2 - av1| < 20 ms を判定する。渡さなければ判定しない", () => {
        const judged = (means: Array<number | null>) => ({ perCell: means.map((m, cell) => ({ cell, data: { paired: m === null ? 0 : 5, audio_minus_video_ms: { mean: m } } })) });
        const within = judgeAvDrift({ first: judged([10, null]), second: judged([29.9, 5]), deltaLimitMs: AV_DRIFT_DELTA_LIMIT_MS });
        expect(within).toMatchObject({ ok: true, deltaLimitMs: 20, measuredCells: 1 });
        expect(within.cells[0]).toMatchObject({ deltaMs: expect.closeTo(19.9, 5), withinLimit: true });
        expect(within.cells[1].withinLimit).toBeNull();
        expect(judgeAvDrift({ first: judged([10]), second: judged([30]), deltaLimitMs: 20 })).toMatchObject({ ok: false, reason: "av2 - av1 is 20 ms or more on cell 0" });
        expect(judgeAvDrift({ first: judged([10]), second: judged([90]) })).toMatchObject({ ok: true });
    });

    it("実行計画: av1 と av2 の sync.py の直後とメインの手順の sync.py の後 (OBS を閉じた後) にしきい値で判定し、av2 - av1 にしきい値を渡す。最後に表にまとめる", () => {
        const { steps } = plan([...REAL, ...STAGE]);
        for (const label of ["av1", "av2"]) {
            const j = at(steps, (s) => s.kind === "judge" && s.tool === "sync.py" && s.label === label);
            expect(steps[j + 1]).toMatchObject({ kind: "judge-av-sync", recording: label });
        }
        const mainSync = at(steps, (s) => s.kind === "judge" && s.tool === "sync.py" && s.recording === "main");
        expect(at(steps, (s) => s.kind === "judge-av-sync" && s.recording === "main")).toBeGreaterThan(mainSync);
        expect(steps.find((s) => s.kind === "judge-av-drift")).toMatchObject({ deltaLimitMs: AV_DRIFT_DELTA_LIMIT_MS });
        const lines = at(steps, (s) => s.kind === "judge-lines");
        expect(lines).toBeGreaterThan(at(steps, (s) => s.kind === "judge-call-log"));
        expect(lines).toBeLessThan(at(steps, (s) => s.kind === "keep-obs-recordings"));
        for (const s of steps.filter((x) => ["judge-av-sync", "judge-lines", "judge-ac30", "judge-ac32", "judge-latency"].includes(x.kind))) expect(s.gate).toBeUndefined();
        // --extra-checks が無ければしきい値を渡さない。
        expect(plan([...REAL, "--broadcaster-id", "1"]).steps.find((s) => s.kind === "judge-av-drift").deltaLimitMs).toBeUndefined();
    });
});

describe("4 分割の付け外しを配置の行で判定する", () => {
    it("実行計画: 配置の行 3 つの後に判定を置き、外す人は実際の通話が DEFAULT_CALL_ACCOUNTS.firefox・リハーサルがテスト用の送信側の画面の 1 つ目", () => {
        for (const [argv, person] of [[[...REAL, ...STAGE], DEFAULT_CALL_ACCOUNTS.firefox], [[...REHEARSAL, ...STAGE], "harness-screen-1"]] as const) {
            const steps = plan(argv as string[]).steps;
            const restoreShot = at(steps, (s) => s.kind === "screenshot-window" && s.shotLabel === AC30_ITEM_LABELS.afterRestore);
            expect(steps[restoreShot + 1]).toMatchObject({ kind: "judge-composite-layout", person, labels: AC30_ITEM_LABELS });
        }
    });
});

describe("名前ラベルの設定が合成のソースの画像に反映される", () => {
    it("実行計画: 設定を変える前の画像 -> ドックの [表示] タブで名前の [表示] を外す -> 後の画像 -> 戻す -> 判定 (録画と配信の前)", () => {
        const steps = plan([...REAL, ...STAGE]).steps;
        const before = at(steps, (s) => s.kind === "screenshot-source" && s.label === AC31_LABELS.before);
        const after = at(steps, (s) => s.kind === "screenshot-source" && s.label === AC31_LABELS.after);
        const judge = at(steps, (s) => s.kind === "judge-ac31");
        expect(steps[before].source).toEqual({ kind: "composite" });
        expect(before).toBeLessThan(after);
        expect(after).toBeLessThan(judge);
        expect(steps[judge]).toMatchObject({ before: AC31_LABELS.before, after: AC31_LABELS.after });
        expect(steps[judge].gate).toBeUndefined();
        const off = at(steps, (s) => s.kind === "dialog-click" && s.buttonTextId === TEXT_ID_LOOK_SHOW && s.desired === "off");
        const on = at(steps, (s) => s.kind === "dialog-click" && s.buttonTextId === TEXT_ID_LOOK_SHOW && s.desired === "on");
        expect(before).toBeLessThan(off);
        expect(off).toBeLessThan(after);
        expect(after).toBeLessThan(on);
        expect(judge).toBeLessThan(at(steps, (s) => s.kind === "record-start" && s.label === "main"));
    });

    it("judgeAc31: 外した後に背景が見つからないか、幅が前の 85% 以下なら合格。変わらなければ失敗。前に背景が見つからなければ判定できない", () => {
        const found = (w: number) => ({ found: true, plate: { width: w } });
        expect(judgeAc31({ before: found(200), after: { found: false, plate: null } }).ok).toBe(true);
        expect(judgeAc31({ before: found(200), after: found(120) }).ok).toBe(true);
        expect(judgeAc31({ before: found(200), after: found(200) }).ok).toBe(false);
        expect(judgeAc31({ before: found(200), after: found(190) }).ok).toBe(false);
        expect(judgeAc31({ before: { found: false }, after: found(100) }).ok).toBeNull();
        expect(judgeAc31({ before: found(200), after: { found: null, error: "x" } }).ok).toBeNull();
    });
});

describe("声のミュートをドックの行のボタンで往復して、録画の音量で判定する", () => {
    it("実行計画: 録画の前に声を全員ミュート、録画の中で 1 人ずつ外す -> ミュート -> 外す の区間にマーカーを打ち、終わりに全員の声のミュートを外す", () => {
        const { steps } = plan([...REAL, ...STAGE]);
        const start = at(steps, (s) => s.kind === "record-start" && s.label === "ac32");
        const stop = at(steps, (s) => s.kind === "record-stop" && s.label === "ac32");
        const block = steps.slice(start, stop + 1);
        expect(block.filter((s) => s.kind === "mark-time").map((s) => s.label)).toEqual(callVoicePersons({}).map((_, i) => i + 1).flatMap((v) => ["base", "muted", "restored", "end"].map((seg) => ac32MarkLabel(v, seg))));
        const clicks = block.filter((s) => s.kind === "dock-click");
        expect(clicks.every((s) => s.muteOp === true && s.textId === "cordmux.dock.row_voice_mute.a11y")).toBe(true);
        expect(clicks.map((s) => [s.textArgs[0], s.desired])).toEqual(
            callVoicePersons({}).flatMap((p) => [[p, "off"], [p, "on"], [p, "off"], [p, "on"]]),
        );
        expect(block.filter((s) => s.kind === "wait").every((s) => s.ms === AC32_SEGMENT_MS)).toBe(true);
        // 録画の前に全員をミュート、録画の後に全員のミュートを外す。
        expect(steps.slice(start - callVoicePersons({}).length, start).map((s) => [s.textArgs[0], s.desired])).toEqual(callVoicePersons({}).map((p) => [p, "on"]));
        expect(steps.slice(stop + 1, stop + 1 + callVoicePersons({}).length).map((s) => [s.textArgs[0], s.desired])).toEqual(callVoicePersons({}).map((p) => [p, "off"]));
        // 判定は OBS を閉じた後。
        expect(at(steps, (s) => s.kind === "judge-ac32")).toBeGreaterThan(steps.map((s) => s.kind).lastIndexOf("close-obs"));
    });

    it("callVoicePersons: 実際の通話は受信用のアカウントを除く依頼メッセージの 2 人、リハーサルはテスト用の送信側の声のストリーム (画面のストリームの後の番号)", () => {
        expect(callVoicePersons({})).toEqual(["sub2", "sub3"]);
        expect(callVoicePersons({ rehearsal: true, nScreenGiven: true, nScreen: 3, nVoiceGiven: true, nVoice: 2 })).toEqual(["harness-voice-4", "harness-voice-5"]);
    });

    it("ac32Windows: マーカーから区間を組み、両端を 0.5 秒削る。tone_scan.py の --windows の文字列にする", () => {
        const built = ac32Windows({ marks: ac32Marks(), voices: 3, marginS: 0.5 });
        expect(built.problems).toEqual([]);
        expect(built.windows).toHaveLength(9);
        expect(built.windows[0]).toEqual({ label: "v1-base", slot: 1, segment: "base", start: 0.5, end: 2.5 });
        expect(toneScanWindowsArg(built.windows).split(",")[1]).toBe("v1-muted=3.5:5.5");
        const partial = ac32Windows({ marks: { [ac32MarkLabel(1, "base")]: 0 }, voices: 1, marginS: 0.5 });
        expect(partial.windows).toEqual([]);
        expect(partial.problems[0]).toContain("v1-base");
    });

    it("judgeAc32: 鳴る声 1 人がミュートで無音・外し直すと基準に戻れば合格。ほかの声は判定できない (音の出る声は 1 人の前提)", () => {
        const built = ac32Windows({ marks: ac32Marks(), voices: 3, marginS: 0.5 });
        const r = judgeAc32({ windows: built.windows, scanned: TONE_SCAN_AC32_WINDOWS });
        expect(r).toMatchObject({ ok: true, soundingSlots: 1 });
        expect(r.parts[0]).toMatchObject({ slot: 1, decidable: true, restoredOk: true, mutedOk: true });
        expect(r.parts.slice(1).map((p: any) => p.decidable)).toEqual([false, false]);
        expect(r.premise).toContain("one voice");
        // 外し直しても半分の音量にしか戻らない = 失敗。
        const weak = TONE_SCAN_AC32_WINDOWS.map((w) => (w.label === "v1-restored" ? { ...w, rms: 0.07 } : w));
        expect(judgeAc32({ windows: built.windows, scanned: weak })).toMatchObject({ ok: false, reason: "not as expected on voice 1" });
        // ミュートでも鳴る = 失敗。
        const leak = TONE_SCAN_AC32_WINDOWS.map((w) => (w.label === "v1-muted" ? { ...w, rms: 0.05 } : w));
        expect(judgeAc32({ windows: built.windows, scanned: leak }).parts[0].mutedOk).toBe(false);
        // どの声も鳴っていない (リハーサルのテスト用の送信側の声) = 判定できない (null。失敗にしない)。
        const quiet = TONE_SCAN_AC32_WINDOWS.map((w) => ({ ...w, rms: 0 }));
        expect(judgeAc32({ windows: built.windows, scanned: quiet })).toMatchObject({ ok: null, soundingSlots: 0 });
        expect(TONE_AUDIBLE_RMS).toBeLessThan(0.141414);
    });
});

describe("使う声の tone_scan.py を --windows で読む", () => {
    it("区間の文字列は 8 秒の録画の先頭と末尾を 0.5 秒ずつ削った 1 区間", () => {
        expect(voiceSourceScanWindow({})).toBe("all=0.5:7.5");
        expect(voiceSourceScanWindow({ recordSeconds: 4, marginSeconds: 0.25 })).toBe("all=0.25:3.75");
    });

    it("toneWindowValues (実際の出力): 区間の rms と tone_share を取り出す。--windows のない出力は「区間がない」というエラーを返す", () => {
        expect(toneWindowValues(TONE_SCAN_TONE)).toEqual({ rms: 0.212125, toneShare: 1, windowSeconds: 7, error: null });
        expect(toneWindowValues(TONE_SCAN_QUIET)).toEqual({ rms: 0, toneShare: 0, windowSeconds: 7, error: null });
        const none = toneWindowValues(TONE_SCAN_NO_WINDOWS);
        expect(none).toMatchObject({ rms: null, toneShare: null });
        expect(none.error).toContain("--windows");
        expect(toneWindowValues({ files: [{ file: "x", error: "ffmpeg failed" }] }).error).toBe("ffmpeg failed");
        expect(toneWindowValues({}).error).toContain("no file");
    });

    it("実際の出力から組んだ 4 つの確認で judgeVoiceSourceSwitch が合格になる (置き換えた後は所有者の枠で 1 kHz、戻すと声を貸す人の枠で 1 kHz)", () => {
        const tone = toneWindowValues(TONE_SCAN_TONE);
        const quiet = toneWindowValues(TONE_SCAN_QUIET);
        const checks = VOICE_SOURCE_CHECKS.map((c) => ({ label: c.label, expectTone: c.expectTone, targetFound: true, ...(c.expectTone ? tone : quiet) }));
        const r = judgeVoiceSourceSwitch({ checks });
        expect(r.ok).toBe(true);
        expect(r.parts.map((p: any) => p.heard)).toEqual([true, false, false, true]);
        expect(r.parts.every((p: any) => p.note === null)).toBe(true);
        // 区間のない出力の files[0].tone_share を読む方法では 4 つとも「読めていない」になる。
        const old = toneWindowValues(TONE_SCAN_NO_WINDOWS);
        const oldChecks = VOICE_SOURCE_CHECKS.map((c) => ({ label: c.label, expectTone: c.expectTone, targetFound: true, ...old }));
        expect(judgeVoiceSourceSwitch({ checks: oldChecks }).parts.every((p: any) => p.heard === null)).toBe(true);
    });
});

describe("バージョンの行の読み (ラベルと値の並びに依存しない)", () => {
    const labels = ["cordmux (OBS)", "cordmux (Vesktop)", "Vesktop", "同梱のVencord", "OBS", "Windows", "GPU"];
    const opts = { names: ["Vesktop"], labels, reference: ["cordmux (OBS)"], unavailable: ["取得できません"] };

    it("ラベルが先の並びも値が先の並びも読む (1 行目のラベルの前後で決める)。読んだ全部の行を rows に返す", () => {
        const labelFirst = ["バージョン", "cordmux (OBS)", "0.1.0+dev", "cordmux (Vesktop)", "0.1.0+dev", "Vesktop", "1.6.7", "同梱のVencord", "v1.15.11", "OBS", "31.1.1"];
        const r = judgeVersionRowShown({ texts: labelFirst, ...opts });
        expect(r).toMatchObject({ ok: true, value: "1.6.7", orientation: "label-first" });
        expect(r.rows["cordmux (OBS)"]).toBe("0.1.0+dev");
        expect(r.rows["OBS"]).toBe("31.1.1");
        // 値が先: 1 行目のラベルの前がバージョンのテキスト、後ろは次の行の値 (ここでは読めない値)。
        const valueFirst = ["バージョン", "0.1.0+dev", "cordmux (OBS)", "取得できません", "cordmux (Vesktop)", "1.6.7", "Vesktop", "v1.15.11", "同梱のVencord"];
        expect(judgeVersionRowShown({ texts: valueFirst, ...opts })).toMatchObject({ ok: true, value: "1.6.7", orientation: "value-first" });
    });

    it("値が読めない行 (読み上げの名前「取得できません」・「-」・空) は失敗。並びが確認できなければ失敗", () => {
        const unreadable = ["cordmux (OBS)", "0.1.0", "Vesktop", "取得できません", "同梱のVencord", "取得できません"];
        expect(judgeVersionRowShown({ texts: unreadable, ...opts })).toMatchObject({ ok: false, value: "取得できません", orientation: "label-first" });
        expect(judgeVersionRowShown({ texts: ["cordmux (OBS)", "0.1.0", "Vesktop", "-"], ...opts }).ok).toBe(false);
        expect(judgeVersionRowShown({ texts: ["cordmux (OBS)", "0.1.0", "Vesktop", "同梱のVencord"], ...opts })).toMatchObject({ ok: false, value: "" });
        expect(judgeVersionRowShown({ texts: ["Vesktop", "1.6.7"], ...opts }).reason).toContain("reference row");
        // 1 行目のラベルの前後がどちらもバージョンのテキスト (どちらが先か決められない) と、どちらも違うとき。
        expect(judgeVersionRowShown({ texts: ["0.1.0", "cordmux (OBS)", "0.1.0", "Vesktop", "1.6.7"], ...opts }).reason).toContain("order");
        expect(judgeVersionRowShown({ texts: ["cordmux (OBS)", "Vesktop", "1.6.7"], ...opts }).reason).toContain("order");
    });

    it("実行計画はラベルの全部・cordmux (OBS)・読めない値の名前の id を渡し、id は ini から引く", () => {
        const { steps } = plan([...REAL, ...STAGE]);
        const judge = steps.find((s) => s.kind === "judge-rt" && s.judge === "version-row-shown")!;
        expect(judge.inputs).toMatchObject({
            labels: { textPrefix: TEXT_ID_SETTINGS_VERSION_LABELS },
            reference: { textPrefix: TEXT_ID_SETTINGS_VERSION_OBS_PLUGIN },
            unavailable: { textPrefix: TEXT_ID_SETTINGS_UNAVAILABLE_A11Y },
        });
        expect(TEXT_ID_SETTINGS_VERSION_LABELS).toHaveLength(7);
    });
});

describe("整列の傾きを frames が減った所で区切る", () => {
    // 10 秒ごとの行。前半 25 分で整列をやり直し (frames が 0 に戻り offset も段になる)、後半も同じ速さで溜まる (0.08%)。
    function series() {
        const samples: Array<Record<string, number>> = [];
        for (let i = 0; i < 480; i += 1) {
            const t = 1000 + i * 10;
            const restart = i >= 150;
            const k = restart ? i - 150 : i;
            samples.push({ t, periodUs: 16667, frames: k * 600, realign: restart ? 1 : 0, offsetUs: k * 10 * 1e6 * 0.0008 });
        }
        return samples;
    }

    it("区切りごとの傾きの最大で判定する (区切らずに当てると薄まる形でも、本当の傾き 0.08% を上限 0.05% の外と読む)", () => {
        const samples = series();
        expect(alignSegments(samples).map((s) => s.length)).toEqual([150, 330]);
        const d = alignDriftOf(samples);
        expect(d.restarts).toBe(1);
        expect(d.segments).toHaveLength(2);
        expect(d.slopePercent).toBeCloseTo(0.08, 4);
        expect(d).toMatchObject({ judged: true, ok: false });
        expect(d.realigns).toBeGreaterThanOrEqual(0);
        expect(d.samples).toBe(480);
    });

    it("短い区切りは傾きを判定せず、長い区切りだけで決める", () => {
        const samples = series().slice(0, 200);
        const d = alignDriftOf(samples);
        expect(d.segments.map((s: any) => s.judged)).toEqual([true, false]);
        expect(d.judged).toBe(true);
    });
});

describe("参加のメッセージに Vesktop も入る", () => {
    it("--vesktop-share のメッセージは Vesktop にも同じボイスチャンネルに入ることを依頼する", () => {
        expect(callJoinMessage({ vesktopShare: true })).toContain(") でも同じボイスチャンネルに入り、画面共有を1つ始めてください");
        expect(callJoinMessage({ vesktopShare: true }).indexOf("Vesktop (")).toBeGreaterThan(-1);
    });

    it("ボイスチャンネルの名前は --voice-channel の値だけから入る (コードに名前を持たない)", () => {
        const named = callJoinMessage({ vesktopShare: true, voiceChannel: CHANNEL });
        expect(named).toContain("でボイスチャンネル「" + CHANNEL + "」に入ってください。");
        expect(named).toContain(") でも「" + CHANNEL + "」に入り、画面共有を1つ始めてください");
        const unnamed = callJoinMessage({ vesktopShare: true });
        expect(unnamed).toContain("で指定のボイスチャンネルに入ってください。");
        expect(unnamed).not.toContain("「");
        const join = plan([...REAL, ...STAGE]).steps.find((s) => s.kind === "human" && s.until === "go-file" && String(s.message).includes("ボイスチャンネル"))!;
        expect(join.message).toBe(named);
    });
});

describe("Vencord の設定ファイルをコピーして比べる (戻さない)", () => {
    it("--start-with-add の実際の通話だけ: [Vesktopに追加] の依頼より前にコピーし、Vesktop を終了してもらった直後に比べる", () => {
        const { args, steps } = plan([...REAL, ...STAGE]);
        expect(vencordSettingsCopied(args)).toBe(true);
        const copy = at(steps, (s) => s.kind === "copy-vencord-settings");
        expect(steps[copy]).toMatchObject({ appDataRel: VENCORD_SETTINGS_REAL_REL, destName: VENCORD_SETTINGS_BEFORE_NAME });
        expect(copy).toBeLessThan(at(steps, (s) => s.kind === "human"));
        expect(copy).toBeGreaterThan(at(steps, (s) => s.kind === "obsws-connect"));
        // 実際のアバターの並びを撮る場合は終わりのメッセージを 2 つに分けるので、Vesktop を終了してもらうステップはフラグ (quitVesktop) で探す。
        const quit = at(steps, (s) => s.kind === "human" && s.quitVesktop === true);
        expect(steps[quit + 1]).toMatchObject({ kind: "compare-vencord-settings", beforeName: VENCORD_SETTINGS_BEFORE_NAME });
        expect(VENCORD_SETTINGS_REAL_REL).toBe("Vesktop/settings/settings.json");
        for (const argv of [[...REHEARSAL, ...STAGE], [...REAL, "--broadcaster-id", "1"]]) {
            const other = plan(argv).steps;
            expect(other.some((s) => s.kind === "copy-vencord-settings" || s.kind === "compare-vencord-settings")).toBe(false);
        }
    });

    it("settingsKeyDiff: キーのパスと変わり方だけ (値を返さない)。キーの中の 17-20 桁の数は伏せる", () => {
        const before = { plugins: { TenorGifSearch: { enabled: true, apiKey: "secret-value" }, Keep: { enabled: true } }, notes: "a", ids: ["1"], "12345678901234567": { x: 1 } };
        const after = { plugins: { GifProviderSwitcher: { enabled: true }, Keep: { enabled: false } }, notes: 3, ids: ["2"], "12345678901234567": { x: 1 } };
        const diff = settingsKeyDiff(before, after);
        expect(diff.changes).toEqual([
            { path: "ids", change: "changed", type: "array" },
            { path: "notes", change: "type", from: "string", to: "number" },
            { path: "plugins.GifProviderSwitcher", change: "added", type: "object" },
            { path: "plugins.Keep.enabled", change: "changed", type: "boolean" },
            { path: "plugins.TenorGifSearch", change: "removed", type: "object" },
        ]);
        expect(JSON.stringify(diff)).not.toContain("secret-value");
        const idKey = settingsKeyDiff({ "12345678901234567": 1 }, {});
        expect(idKey.changes[0].path).toBe("<ID>");
        expect(settingsKeyDiff({}, {}).changes).toEqual([]);
    });
});

describe("終わりの手順の前に終了した場合でも vencordDir を戻す", () => {
    it("finallyVencordRestorePlan: 実際の保存先で終わりの手順で戻す実行が、戻ったと確認する前に終了し、バックアップがあれば -CallWindow", () => {
        const base = { realProfile: true, planHasRestoreJudge: true, restoredOk: false, backupExists: true };
        expect(finallyVencordRestorePlan(base).action).toBe("call-window");
        expect(finallyVencordRestorePlan({ ...base, restoredOk: true }).action).toBe("skip");
        expect(finallyVencordRestorePlan({ ...base, backupExists: false }).action).toBe("report-only");
        expect(finallyVencordRestorePlan({ ...base, realProfile: false }).action).toBe("skip");
        expect(finallyVencordRestorePlan({ ...base, planHasRestoreJudge: false }).reason).toContain("start gates");
    });

    it("実行計画: call-1 と call-hold は終わりの手順で戻す (judge-vencord-dir-restored を持つ)、開始ゲート a は持たない", () => {
        expect(plan([...REAL, ...STAGE]).steps.some((s) => s.kind === "judge-vencord-dir-restored")).toBe(true);
        expect(plan(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-hold", "--real-profile", "--call-window"]).steps.some((s) => s.kind === "judge-vencord-dir-restored")).toBe(true);
        expect(plan(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-gate-a", "--real-profile", "--call-window"]).steps.some((s) => s.kind === "judge-vencord-dir-restored")).toBe(false);
    });

    it("cleanupFlags: finally だけが -CallWindow を足す。ほかのフラグの並びは変わらない", () => {
        expect(cleanupFlags({ dryRun: true })).toEqual(["-DryRun", "-Json"]);
        expect(cleanupFlags({ obsDir: "C:/obs", artifactsPath: "C:/a.json", realProfile: true, keepRecordings: true })).toEqual(["-Json", "-ObsDir", "C:/obs", "-ArtifactsPath", "C:/a.json", "-RealProfile", "-KeepRecordings"]);
        expect(cleanupFlags({ realProfile: true, callWindow: true }).slice(-2)).toEqual(["-RealProfile", "-CallWindow"]);
        expect(cleanupFlags({ realProfile: true, callWindow: false })).not.toContain("-CallWindow");
    });

    it("vencordDirFinalCheck: 実行の前のコピーと vencordDir (とキーの有無) が同じか。区切りと大文字小文字は比べない", () => {
        const before = JSON.stringify({ vencordDir: "C:\\v\\dist", windowBounds: {} });
        expect(vencordDirFinalCheck({ stateText: JSON.stringify({ vencordDir: "c:/v/dist/" }), beforeText: before }).same).toBe(true);
        expect(vencordDirFinalCheck({ stateText: JSON.stringify({ vencordDir: "C:/obs/plugins/cordmux/data/vencord" }), beforeText: before })).toMatchObject({ same: false });
        expect(vencordDirFinalCheck({ stateText: JSON.stringify({}), beforeText: JSON.stringify({}) }).same).toBe(true);
        expect(vencordDirFinalCheck({ stateText: JSON.stringify({ vencordDir: "x" }), beforeText: JSON.stringify({}) }).same).toBe(false);
        expect(vencordDirFinalCheck({ stateText: null, beforeText: before }).same).toBeNull();
        expect(vencordDirFinalCheck({ stateText: "{}", beforeText: null }).same).toBeNull();
    });

    it("実行スクリプト: finally が -CallWindow の判断と最後の確認を書き、止める通知ファイルで期限と同じ処理の流れを抜ける (テキストとして読む)", () => {
        const source = readFileSync(join(HERE, "run-window.mjs"), "utf8");
        const end = source.indexOf("  return { gateFailures: state.gateFailures, aborted, vencordFinal };");
        const fin = source.slice(source.lastIndexOf("  } finally {", end), end);
        expect(fin).toContain("finallyVencordRestorePlan({");
        expect(fin).toContain('callWindow: restorePlan.action === "call-window"');
        expect(fin).toContain("VENCORD_DIR_FINAL_PREFIX + tag");
        expect(STOP_FILE_NAME).toBe("stop-run.txt");
        expect(source).toContain("return stopRequested() || deadlineExceeded(startMs, Date.now(), args.deadlineMin);");
        expect(source).toContain('aborted = "stopped";');
    });
});

describe("実行の途中で止めるもの (stopOnFail) と記録するだけのもの", () => {
    it("--start-with-add の実際の通話は、[Vesktopに追加] が届かない・起動し直した Vesktop が接続しない、の 2 つだけで止める", () => {
        const { steps } = plan([...REAL, ...STAGE]);
        const stops = steps.filter((s) => s.stopOnFail);
        expect(stops.map((s) => [s.kind, s.until || s.baseline])).toEqual([["human", "state-json-points-to-placed"], ["wait-connection", ADD_TO_VESKTOP_BASELINE_LABEL]]);
        for (const s of stops) expect(s.gate).toBe(true);
        expect(JUDGE_LINES.filter((l) => l.onFail === "stop").map((l) => l.item)).toEqual(["add-to-vesktop", "vesktop-connects"]);
        expect(JUDGE_LINES.filter((l) => l.onFail === "record").length).toBe(JUDGE_LINES.length - 3);
        expect(JUDGE_LINES.filter((l) => l.onFail === "gate").map((l) => l.item)).toEqual(["self-account"]);
        // リハーサルと call-hold と標準の実行計画は止めない。
        for (const argv of [[...REHEARSAL, ...STAGE], [...REAL, "--broadcaster-id", "1"], ["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-hold", "--real-profile", "--call-window"]]) {
            expect(plan(argv).steps.some((s) => s.stopOnFail)).toBe(false);
        }
    });

    it("judgeLinesSummary: 行ごとに結果を集め、失敗の行を挙げる。この実行にない行は null (数えない)", () => {
        const results = [
            { name: "human", until: "state-json-points-to-placed", ok: true, satisfiedBy: "state-json" },
            { name: "wait-connection", baseline: ADD_TO_VESKTOP_BASELINE_LABEL, ok: true },
            { name: "judge-av-sync", recording: "main", ok: true, cells: [{ cell: 2, ok: true, meanMs: 23.4, paired: 240, trend: { verdict: "long-call", slopeMsPerMin: 0.2 } }] },
            { name: "judge-av-sync", recording: "av1", ok: false, reason: "outside the line on cell 2", cells: [] },
            { name: "judge-latency", ok: true, measuredMs: 30, estimatedMs: 46.7, designQuestion: true },
            { name: "judge-connections-kept", label: "after-join", ok: true, closes: 0, hellos: 0 },
            { name: "judge-connections-kept", label: "after-main", ok: true, closes: 0, hellos: 0 },
        ];
        const r = judgeLinesSummary(results);
        expect(r).toMatchObject({ ok: false, red: ["av-sync-av1"] });
        const row = (item: string) => r.rows.find((x: any) => x.item === item);
        expect(row("add-to-vesktop")).toMatchObject({ ok: true, onFail: "stop" });
        expect(row("av-sync-main").detail).toEqual([{ cell: 2, meanMs: 23.4, paired: 240, trend: "long-call", slopeMsPerMin: 0.2 }]);
        expect(row("av-sync-av1")).toMatchObject({ ok: false, reason: "outside the line on cell 2", onFail: "record" });
        expect(row("latency").detail).toMatchObject({ estimatedMs: 46.7, designQuestion: true });
        expect(row("connections-kept")).toMatchObject({ ok: true });
        expect(row("ac32").ok).toBeNull();
        expect(judgeLinesSummary([]).ok).toBe(true);
    });
});

describe("配信者の ID を書かない", () => {
    it("argsForResult: broadcasterId を伏せたコピーを返す (もとの args は変えない)", () => {
        const args = { step: "call-1", broadcasterId: "12345678901234567" };
        expect(argsForResult(args)).toEqual({ step: "call-1", broadcasterId: REDACTED_BROADCASTER_ID });
        expect(args.broadcasterId).toBe("12345678901234567");
        const none = { step: "call-1", broadcasterId: null };
        expect(argsForResult(none)).toBe(none);
    });

    it("countDiscordIdLike: 17-20 桁の数だけを数える (16 桁と 21 桁は数えない)", () => {
        expect(countDiscordIdLike("a 12345678901234567 b 123456789012345678901 c 1234567890123456")).toBe(1);
        expect(countDiscordIdLike("x12345678901234567890y")).toBe(1);
        expect(countDiscordIdLike(null)).toBe(0);
    });

    it("実行スクリプト: 結果 JSON の 2 か所の書き込みが伏せた args を書く (テキストとして読む)", () => {
        const source = readFileSync(join(HERE, "run-window.mjs"), "utf8");
        expect(source.match(/args: resultArgs/g)).toHaveLength(2);
        expect(source).toContain("const resultArgs = argsForResult(args);");
        expect(source).toContain("args: argsForResult(args),");
        expect(ID_BEARING_FILE_PATTERNS).toEqual(expect.arrayContaining([VENCORD_SETTINGS_BEFORE_NAME, "latency-reference"]));
    });

    // Edit に ID が入っているウィンドウの [OK] (dialog-click) のログの形。dialog-click.ps1 の New-Result のキーと、
    // Read-Edits がウィンドウの Edit の名前と値を並べた edits、テキストと edits を並べた texts (値は作り物。ID は 1 から 7 を並べた作り物の 17 桁)。
    const FAKE_ID = "12345678901234567";
    const dialogClickWithIdEdit = {
        ok: true, pid: 4242, title: "ウィンドウ", popup: false, controlType: "Button", found: true, button: "OK", index: 0, listOnly: false,
        buttonFound: true, invoked: true, pressed: true, expanded: false, returned: null, toggleBefore: null, desired: null,
        buttons: ["OK", "キャンセル"], texts: ["録画", "欄の名前", "欄の名前", FAKE_ID, "保存先", "C:/rec"],
        edits: ["欄の名前", FAKE_ID, "保存先", "C:/rec"], controls: [], error: null, rawOk: true, expect: "present", label: null,
    };

    it("redactBroadcasterId: 実際の ID の形なら、ステップを問わずログの文字列の全部から伏せる (dialog-click の edits と texts)。ほかの値と形は変えない", () => {
        const r = redactBroadcasterId(dialogClickWithIdEdit, FAKE_ID);
        expect(countDiscordIdLike(JSON.stringify(r))).toBe(0);
        expect(r.edits).toEqual(["欄の名前", REDACTED_BROADCASTER_ID, "保存先", "C:/rec"]);
        expect(r.texts[3]).toBe(REDACTED_BROADCASTER_ID);
        expect({ ...r, edits: dialogClickWithIdEdit.edits, texts: dialogClickWithIdEdit.texts }).toEqual(dialogClickWithIdEdit);
        // もとのログは変えない (コピーを返す)。
        expect(dialogClickWithIdEdit.edits[1]).toBe(FAKE_ID);
        // ログの行 (log) の文字列と、長い文字列の中に埋まった ID も伏せる。
        expect(redactBroadcasterId("dialog-click " + JSON.stringify(dialogClickWithIdEdit), FAKE_ID)).not.toContain(FAKE_ID);
        expect(redactBroadcasterId({ error: "powershell -Value " + FAKE_ID + " failed" }, FAKE_ID).error).toBe("powershell -Value " + REDACTED_BROADCASTER_ID + " failed");
    });

    it("redactBroadcasterId: 作り物の短い ID と、ID の入っていないログは同じ物を返す。文字列のほかの値 (数・NaN・undefined) は変えない", () => {
        const short = { ...dialogClickWithIdEdit, edits: ["欄の名前", "1"], texts: ["1", "10 分"] };
        expect(redactBroadcasterId(short, "1")).toBe(short);
        const clean = { ok: true, meanMs: Number.NaN, limit: Number.POSITIVE_INFINITY, note: undefined, texts: ["a"] };
        expect(redactBroadcasterId(clean, FAKE_ID)).toBe(clean);
        const mixed = redactBroadcasterId({ value: FAKE_ID, meanMs: Number.NaN, n: 3, note: undefined, nested: [{ t: "x " + FAKE_ID }] }, FAKE_ID);
        expect(Number.isNaN(mixed.meanMs)).toBe(true);
        expect(mixed).toMatchObject({ value: REDACTED_BROADCASTER_ID, n: 3, nested: [{ t: "x " + REDACTED_BROADCASTER_ID }] });
        expect(Object.prototype.hasOwnProperty.call(mixed, "note")).toBe(true);
        expect(redactBroadcasterId(null, FAKE_ID)).toBeNull();
        expect(redactBroadcasterId(undefined, FAKE_ID)).toBeUndefined();
        expect([FAKE_ID, "1", "1234567890123456", "123456789012345678901", null].map(isDiscordIdShape)).toEqual([true, false, false, false, false]);
    });

    it("実行スクリプト: record とログの行 (log) と結果 JSON の summary が伏せる (テキストとして読む)", () => {
        const source = readFileSync(join(HERE, "run-window.mjs"), "utf8");
        expect(source).toContain("const safe = redactBroadcasterId(data, args.broadcasterId);");
        expect(source).toContain("state.results.push({ name, ...safe });");
        expect(source).toContain("log(name, JSON.stringify(safe).slice(0, 2000));");
        expect(source).toContain("const line = redactBroadcasterId(`[${new Date().toISOString()}] ${a.join(\" \")}`, args.broadcasterId);");
        expect(source).toContain("summary: redactBroadcasterId(summaryText, args.broadcasterId)");
        expect(source).not.toContain("JSON.parse(JSON.stringify(out, (key, v)");
    });
});

describe("実行スクリプトの switch と期限の見積もり", () => {
    it("実際の通話とリハーサルが使うステップの種類を、実行スクリプトの switch と HANDLED_KINDS が全部持つ (テキストとして読む)", () => {
        const source = readFileSync(join(HERE, "run-window.mjs"), "utf8");
        const caseKinds = new Set([...source.matchAll(/^\s+case "([a-z0-9-]+)":/gm)].map((m) => m[1]));
        const handledBlock = source.slice(source.indexOf("const HANDLED_KINDS = ["), source.indexOf("];", source.indexOf("const HANDLED_KINDS = [")));
        const handled = new Set([...handledBlock.matchAll(/"([a-z0-9-]+)"/g)].map((m) => m[1]));
        const used = new Set([...plan([...REAL, ...STAGE]).steps, ...plan([...REHEARSAL, ...STAGE]).steps].map((s) => s.kind));
        expect([...used].filter((k) => !caseKinds.has(k) || !handled.has(k))).toEqual([]);
    });

    it("実際の通話: 見込みは 95 分より短く、人の操作は 15 (遅延の 1 つ、声の音量と共有を止めるの 2 つを含む)、期限は 180 分。判定ツールの見積もりに遅延と声のミュートの分が入る", () => {
        const { steps } = plan([...REAL, ...STAGE]);
        expect(steps.filter((s) => s.kind === "human")).toHaveLength(15);
        expect(plannedMinutes(steps)).toBeLessThan(95);
        const without = plan([...REAL, ...STAGE.filter((x) => x !== "--latency-person" && x !== PERSON)]).steps;
        expect(deadlineEstimate(steps).judgeMinutes).toBeGreaterThan(deadlineEstimate(without).judgeMinutes);
        expect(deadlineEstimate(steps).judgeRecordMinutes).toBeGreaterThan(deadlineEstimate(plan([...REAL, "--broadcaster-id", "1"]).steps).judgeRecordMinutes);
    });
});

describe("cordmux枠 (参加者ごとのソース) のステップはない", () => {
    const personKinds = ["wire-person", "read-person-sizes", "judge-person-source", "remove-person-check"];

    it("call-1 のリハーサルにも実際の通話にも、cordmux枠を置くステップと確認するステップは入らない", () => {
        for (const line of [REHEARSAL, REAL]) {
            const steps = plan([...line, ...STAGE]).steps;
            expect(steps.filter((s) => personKinds.includes(s.kind))).toEqual([]);
            // 枠に載った後の配置の行の読み取りは残る。
            const snapshot = at(steps, (s) => s.kind === "composite-layout-snapshot" && s.label === "slot-items");
            expect(snapshot).toBeGreaterThanOrEqual(0);
        }
    });
});

describe("平均の判定の組数の下限 (傾きと同じ AV_MIN_PAIRS)", () => {
    const data = (mean: number, paired: number) => ({
        paired,
        audio_minus_video_ms: { mean, min: mean, max: mean },
        trend: { pairs: paired, span_s: paired > 0 ? 0 : 600, slope_ms_per_min: null, slope_ci95_ms_per_min: null },
    });

    it("組が下限未満のセルは平均がしきい値の外でも判定しない (ok は null)。下限ちょうどは判定する", () => {
        expect(AV_TREND_MIN_PAIRS).toBe(AV_MIN_PAIRS);
        const few = judgeAvSyncLines(data(-458, 2));
        expect(few).toMatchObject({ ok: null, verdict: "too-few-pairs", paired: 2, meanMs: -458 });
        expect(few.note).toContain("10");
        expect(judgeAvSyncLines(data(-458, AV_MIN_PAIRS - 1)).ok).toBeNull();
        expect(judgeAvSyncLines(data(-458, AV_MIN_PAIRS))).toMatchObject({ ok: false, verdict: "outside" });
    });

    it("組が足りないセルだけなら全体は null (失敗にしない)。組が 0 のセルだけなら失敗。60 組のセルは判定する", () => {
        const main = judgeAvSync({ ok: true, perCell: [{ cell: 0, data: data(0, 0) }, { cell: 2, data: data(-458, 2) }] });
        expect(main.ok).toBeNull();
        expect(main.reason).toContain("too few pairs");
        expect(judgeAvSync({ ok: true, perCell: [{ cell: 0, data: data(0, 0) }] }).ok).toBe(false);
        // av1 / av2 の実測 (60 組。平均 69.8 と 64.8)。
        const av = (mean: number, slope: number, ci: number[]) => ({ paired: 60, audio_minus_video_ms: { mean, min: mean, max: mean }, trend: { pairs: 60, span_s: 295, slope_ms_per_min: slope, slope_ci95_ms_per_min: ci } });
        expect(judgeAvSync({ ok: true, data: av(69.8, -2.12, [-4.55, 0.32]) })).toMatchObject({ ok: true, judgedCells: 1 });
        expect(judgeAvSync({ ok: true, data: av(64.8, -0.6, [-1.93, 0.73]) })).toMatchObject({ ok: true, judgedCells: 1 });
        // 組のあるセルが下限を満たして外なら、足りないセルがあっても失敗。
        const mixed = judgeAvSync({ ok: true, perCell: [{ cell: 0, data: data(-458, 2) }, { cell: 1, data: data(200, 60) }] });
        expect(mixed.ok).toBe(false);
    });
});

describe("名前ラベルの背景の幅 (合成のソースの画像の下の帯で測った最大の背景)", () => {
    it("名前ラベルの背景 (幅 185) が、名前を外した後のアバターだけの背景 (幅 55) に狭まれば合格。背景が見つからなくなっても成功", () => {
        const before = { found: true, plate: { width: 185, height: 47 } };
        expect(judgeAc31({ before, after: { found: true, plate: { width: 55, height: 47 } } })).toMatchObject({ ok: true, widthBefore: 185, widthAfter: 55 });
        expect(judgeAc31({ before, after: { found: false, plate: null } }).ok).toBe(true);
        expect(judgeAc31({ before, after: { found: true, plate: { width: 180, height: 47 } } }).ok).toBe(false);
        expect(judgeAc31({ before: { found: false, plate: null }, after: before }).ok).toBeNull();
    });

    it("run-window.mjs は画像を 2x2 の下の帯で測る", () => {
        const source = readFileSync(join(HERE, "run-window.mjs"), "utf8");
        expect(source).toContain('[PLATE_MEASURE_PY, file, "--tile-bands", "2x2"]');
    });
});
