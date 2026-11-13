/*
 * run-window-plan.mjs が持つ純関数 (parseArgs / planSteps / blockersFromProcessList /
 * encoderBusy / deadlineExceeded / filesToPlace、judgeBackpressure / parseStatsVideoDropped /
 * judgeConnectionHeld など) をテストする。
 * run-window-plan.mjs は Node の API を一切呼ばない前提なので、**テストの対象**には渡す値しか
 * 与えない。ただしこのテスト自身は 1 か所だけファイルを読む: ドックのラベルの文言は製品の
 * src-obs/data/locale/ja-JP.ini が持ち、実行スクリプトはそれと同じ文字列でしか押せるコンポーネントを探せない
 * (dock-click.ps1 は Name の完全一致)。**文言を 2 か所で決めない**ために、実行スクリプトの定数は
 * ini の値と突き合わせる (文言が変わると、このテストが失敗する)。
 */
import { existsSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

import {
    AC32_VOLUME_MUL_LOUD,
    AC32_VOLUME_MUL_NORMAL,
    SELECT_FOUR_SETTLE_WAIT_MS,
    mergeCallWindowArtifacts,
    BUNDLED_VENCORD_PACKAGE_JSON_REL,
    BUNDLED_VENCORD_REL,
    CALL_GATE_CONNECT_WAIT_TIMEOUT_MS,
    CALL_SCREEN_COUNT,
    CALL_WINDOW_ARTIFACTS_REL,
    CALL_STEP_VALUES,
    CALL_VOICE_COUNT,
    DECODER_VALUES,
    DEFAULT_CALL_MINUTES,
    DEFAULT_DECODER,
    DEFAULT_DOCK_INSTALL,
    DEFAULT_DOCK_MAX_LONG_EDGE,
    DEFAULT_DOCK_SCENARIO,
    DEFAULT_SYNC_TONE,
    DISCORD_FIRST_OBS_DELAY_MS,
    OBS_STATS_FIRST_WAIT_MS,
    OUTPUT_SKIPPED_RATIO_THRESHOLD,
    STATE_JSON_BEFORE_NAME,
    VESKTOP_STATE_JSON_REAL_REL,
    judgeOperationCount,
    judgeOutputSkipped,
    judgeVencordDirWritten,
    DOCK_ASSIGN_MODE_VALUES,
    DEFAULT_DOCK_ASSIGN_MODE,
    DOCK_INSTALL_VALUES,
    DOCK_SCENARIO_VALUES,
    DOCK_VIEW_FAIL_CONTACT_ID,
    DOCK_VIEW_IDLE_INTERVAL_MS,
    DOCK_VIEW_IDLE_INTERVALS,
    EXPORT_BUNDLE_FILE_NAME,
    INTEGRATION_CONFIG_VALUES,
    SECOND_OBS_EMPTY_SCENE_COLLECTION,
    SECOND_OBS_MAIN_SCENE_COLLECTION,
    SELECT_FOUR_N_SCREEN,
    SELECT_FOUR_N_VOICE,
    SYNC_TONE_VALUES,
    blockersFromProcessList,
    DEFAULT_GUARDED_PROCESS_NAMES,
    PROTECTED_PROCESSES_ENV,
    loadProtectedProcesses,
    parseProtectedProcessNames,
    buildGapFrames,
    leadingGapFrames,
    deadlineExceeded,
    encodeFactsFromHarnessData,
    encoderBusy,
    filesToPlace,
    guardBlocked,
    parseEncoderSessionPids,
    encoderUsedOnlyByCallApps,
    judgeBackpressure,
    judgeConnectionHeld,
    GUIDE_UPDATE_HINTS,
    judgeModuleLoaded,
    judgeModuleNotLoaded,
    judgeGuideUpdateHint,
    parseArgs,
    parseStatsVideoDropped,
    pickNewResultFiles,
    planSteps,
    STEP_VALUES,
    // 録画 (--step record)、webcodecs-check。
    RECORD_AFTER_RESTART_SECONDS,
    RECORD_SECOND_SESSION_SECONDS,
    RECORD_DIR_REL,
    RECORD_HARNESS_FIRST_SECONDS,
    RECORD_HARNESS_RECONNECT_SECONDS,
    RECORD_N_SCREEN,
    RECORD_N_VOICE,
    RECORD_RECONNECT_COUNT,
    RECORD_RESOLUTION_SCHEDULE,
    RECORD_SPACE_CHECK_INTERVAL_S,
    RECORD_SPACE_WARN_MINUTES,
    parseResolutionSchedule,
    resolutionScheduleArg,
    // call-1 の追加分 (human / --broadcaster-id / --rehearsal / plannedMinutes)。
    CALL_AV_RECORD_MS,
    CALL_AV_SECOND_GAP_MINUTES,
    CALL_AV_SHARE_SETTLE_MS,
    CALL_CHECK_CYCLES,
    CALL_CHECK_PERIOD_MS,
    CALL_CONNECT_WAIT_TIMEOUT_MS,
    CALL_MID_RECORD_MS,
    CALL_NOTICE_SETTLE_MS,
    CALL_ONE_DEADLINE_MIN,
    DEFAULT_DEADLINE_MIN,
    deadlineShortfallMinutes,
    CALL_ACCOUNT_CHANGE_SETTLE_MS,
    CALL_STATS_MARK_MINUTES,
    CALL_TAMPER_FILE,
    CALL_TAMPER_WAIT_MS,
    CALL_TEXT_IDS,
    CALL_VOICE_SOURCE_SETTLE_MS,
    HUMAN_JOIN_TIMEOUT_MS,
    HUMAN_TIMEOUT_MS,
    HUMAN_UNINSTALL_TIMEOUT_MS,
    HUMAN_UNTIL_VALUES,
    REHEARSAL_HUMAN_WAIT_SECONDS,
    REHEARSAL_SCALE_MIN_MS,
    REHEARSAL_TIME_FACTOR,
    ROUNDTRIP_CHECK_PERIOD_S,
    ROUNDTRIP_JUDGE_NAMES,
    VENCORD_BUNDLE_FILE_NAMES,
    VENCORD_DIR_BACKUP_REL,
    insertStatsAtMinutes,
    judgeAvDrift,
    judgeNotNamedInTexts,
    judgeRoundtrip,
    judgeVencordDirRestored,
    plannedMinutes,
    rehearseSteps,
    // 枠の区画・音の切り分け・案内と映像停止の一致の判定・使う声・参加の文・OBS の録画・期限の見積もり。
    BIT_CELL_SOURCE_PX,
    CALL_ONE_HUMAN_OVERRUN_MIN,
    DEADLINE_MARGIN_FACTOR,
    JUDGE_SECONDS_PER_RECORDED_SECOND,
    OBS_RECORDINGS_KEEP_DIR_PREFIX,
    REHEARSAL_AV_SCREEN_ACCOUNT,
    STEPS_KEEPING_OBS_RECORDINGS,
    STEPS_KEEPING_RECORDINGS,
    VESKTOP_HANG_UP_AND_QUIT_MESSAGE,
    VESKTOP_SHARE_IN_CALL_ONE,
    VESKTOP_START_AFTER_UNINSTALL_MESSAGE,
    VESKTOP_START_FOR_CALL_ONE_MESSAGE,
    VIDEO_RATE_WINDOW_LINES,
    VIDEO_STOPPED_HZ,
    callJoinMessage,
    callVoiceSourceSteps,
    cellRegionsForJudge,
    deadlineEstimate,
    deadlineMinFor,
    judgeCellArgs,
    judgeNoticeIffFramesStopped,
    obsRecordingMoves,
    parseVideoRateSamples,
    // 画面のテキストの id と textRef、型 id と slot でソースを探すツール。
    CORDMUX_SOURCE_KINDS,
    OBS_TEXT_APPLY,
    OBS_TEXT_CANCEL,
    OBS_TEXT_OK,
    OBS_TEXT_PREFIX,
    OBS_TEXT_TOOLS_MENU,
    OBS_LOCALE_FALLBACK_ORDER,
    TEXT_ID_TOOLS_MENU_UNINSTALL,
    UNINSTALL_ENTRY_VALUES,
    lookupObsText,
    stripQtMnemonic,
    TEXT_ID_ADD_TO_VESKTOP,
    TEXT_ID_DIALOG_EXPORT_TITLE,
    TEXT_ID_DIALOG_SWITCH_BUTTON,
    TEXT_ID_DIALOG_UNINSTALL_BUTTON,
    TEXT_ID_DIALOG_UNINSTALL_TITLE,
    TEXT_ID_DOCK_MENU_BUTTON,
    TEXT_ID_DOCK_WINDOW_TITLE,
    TEXT_ID_MAIN_ACCOUNT_NOT_IN_CALL,
    TEXT_ID_MAIN_ACCOUNT_NOT_IN_CALL_NO_NAME,
    TEXT_ID_MENU_EXPORT_DIAGNOSTICS,
    TEXT_ID_MENU_SETTINGS,
    TEXT_ID_NO_VIDEO,
    TEXT_ID_RECORD_START,
    TEXT_ID_RECORD_STOP,
    TEXT_ID_REINSTALL,
    TEXT_ID_ROW_MORE_ACTIONS,
    TEXT_ID_ROW_OWN_VOICE,
    TEXT_ID_ROW_VOICE_FROM,
    TEXT_ID_DOCK_ACCOUNT_CHANGE,
    TEXT_ID_SETTINGS_PAGE_ABOUT,
    TEXT_ID_SETTINGS_PAGE_ADVANCED,
    TEXT_ID_DOCK_TAB_DISPLAY,
    TEXT_ID_LOOK_SHOW,
    TEXT_ID_LOOK_HEADING_NAME,
    TEXT_ID_LOOK_SHOW_PLACEHOLDERS,
    TEXT_ID_SETTINGS_UNINSTALL,
    TEXT_ID_SETTINGS_WINDOW_TITLE,
    TEXT_ID_VENCORD_OVERWRITTEN,
    classifyCordmuxInputs,
    expandTextRefs,
    expandTextRefsDeep,
    fillTemplate,
    findCordmuxInput,
    personFromSlotName,
    screenSlotOfSceneItem,
    stepTextIds,
    textRef,
    textRefIds,
    DEFAULT_CALL_ACCOUNTS,
    callVoicePersons,
} from "./run-window-plan.mjs";

type Step = Record<string, any>;

// 製品の ja-JP.ini の id -> 値 (テストがテキストを ini と突き合わせるため。実行スクリプトの側はテキストを持たない)。
function jaIniValues(): Map<string, string> {
    const values = new Map<string, string>();
    const file = join(dirname(fileURLToPath(import.meta.url)), "..", "..", "..", "src-obs", "data", "locale", "ja-JP.ini");
    for (const line of readFileSync(file, "utf8").split(/\r?\n/)) {
        const t = line.trim();
        if (!t || t.startsWith("#") || t.indexOf("=") < 0) continue;
        values.set(t.slice(0, t.indexOf("=")).trim(), t.slice(t.indexOf("=") + 1).trim().replace(/^"|"$/g, ""));
    }
    return values;
}

const FULL_ARGV = ["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "place"];

describe("parseArgs", () => {
    it("必須 4 引数が揃えば ok:true になり、既定値が入る", () => {
        const r = parseArgs(FULL_ARGV);
        expect(r.ok).toBe(true);
        expect(r.errors).toEqual([]);
        expect(r.args).toMatchObject({
            obs: "C:/obs",
            out: "C:/out",
            plugin: "C:/plugin",
            step: "place",
            nScreen: 4,
            nVoice: 8,
            seconds: 180,
            // エンコーダの温めは既定でオン。
            warmup: true,
            hw: "prefer-hardware",
            integrationConfig: "default",
            dockInstall: "elsewhere",
            dockAssignMode: "manual",
            dockMaxLongEdge: 640,
            stream: false,
            deadlineMin: 120,
            tag: null,
            dryRun: false,
            allowUnknownEncoder: false,
        });
    });

    it("--obs / --out / --plugin / --step が無ければそれぞれエラーになる (ok:false)", () => {
        const r = parseArgs([]);
        expect(r.ok).toBe(false);
        expect(r.errors).toEqual(
            expect.arrayContaining(["--obs が要る", "--out が要る", "--plugin が要る", "--step が要る"]),
        );
    });

    it("--step に不正な値を渡すとエラーになり、args.step は既定の null のまま", () => {
        const r = parseArgs([...FULL_ARGV.slice(0, 6), "--step", "nonsense"]);
        expect(r.ok).toBe(false);
        expect(r.args.step).toBe(null);
        expect(r.errors.some((e) => e.includes("--step"))).toBe(true);
    });

    it("--hw に不正な値を渡すとエラーになる", () => {
        const r = parseArgs([...FULL_ARGV, "--hw", "auto"]);
        expect(r.ok).toBe(false);
        expect(r.errors.some((e) => e.includes("--hw"))).toBe(true);
    });

    it("--integration-config は既定 default、one-screen / one-screen-one-voice も受け付ける", () => {
        expect(INTEGRATION_CONFIG_VALUES).toEqual(["default", "one-screen", "one-screen-one-voice"]);
        for (const v of INTEGRATION_CONFIG_VALUES) {
            const r = parseArgs([...FULL_ARGV, "--integration-config", v]);
            expect(r.ok).toBe(true);
            expect(r.args.integrationConfig).toBe(v);
        }
    });

    it("--integration-config に不正な値を渡すとエラーになる", () => {
        const r = parseArgs([...FULL_ARGV, "--integration-config", "nonsense"]);
        expect(r.ok).toBe(false);
        expect(r.errors.some((e) => e.includes("--integration-config"))).toBe(true);
    });

    it("--dock-scenario を指定しなければ既定 (DEFAULT_DOCK_SCENARIO = none) になる", () => {
        const r = parseArgs(FULL_ARGV);
        expect(DEFAULT_DOCK_SCENARIO).toBe("none");
        expect(r.args.dockScenario).toBe(DEFAULT_DOCK_SCENARIO);
    });

    it("--dock-scenario は 9 の値 (none/connected/no-scene/degraded/contact-missing/idle-10min/select-four/dummy/export-bundle) をすべて受け付ける", () => {
        expect(DOCK_SCENARIO_VALUES).toEqual([
            "none",
            "connected",
            "no-scene",
            "degraded",
            "contact-missing",
            "idle-10min",
            "select-four",
            "dummy",
            "export-bundle",
        ]);
        for (const v of DOCK_SCENARIO_VALUES) {
            const r = parseArgs([...FULL_ARGV, "--dock-scenario", v]);
            expect(r.ok).toBe(true);
            expect(r.args.dockScenario).toBe(v);
        }
    });

    it("--dock-scenario に不正な値を渡すとエラーになる", () => {
        const r = parseArgs([...FULL_ARGV, "--dock-scenario", "nonsense"]);
        expect(r.ok).toBe(false);
        expect(r.errors.some((e) => e.includes("--dock-scenario"))).toBe(true);
    });

    it("--dock-max-long-edge を指定しなければ既定 (DEFAULT_DOCK_MAX_LONG_EDGE = 640) になる", () => {
        const r = parseArgs(FULL_ARGV);
        expect(DEFAULT_DOCK_MAX_LONG_EDGE).toBe(640);
        expect(r.args.dockMaxLongEdge).toBe(DEFAULT_DOCK_MAX_LONG_EDGE);
    });

    it("--dock-max-long-edge は 0 以上の整数を受け付け、負や小数はエラーになる", () => {
        const r = parseArgs([...FULL_ARGV, "--dock-max-long-edge", "320"]);
        expect(r.ok).toBe(true);
        expect(r.args.dockMaxLongEdge).toBe(320);

        const neg = parseArgs([...FULL_ARGV, "--dock-max-long-edge", "-1"]);
        expect(neg.ok).toBe(false);
        expect(neg.errors.some((e) => e.includes("--dock-max-long-edge"))).toBe(true);

        const frac = parseArgs([...FULL_ARGV, "--dock-max-long-edge", "1.5"]);
        expect(frac.ok).toBe(false);
    });

    it("--dock-install を指定しなければ既定 (DEFAULT_DOCK_INSTALL = elsewhere) になる", () => {
        const r = parseArgs(FULL_ARGV);
        expect(DEFAULT_DOCK_INSTALL).toBe("elsewhere");
        expect(r.args.dockInstall).toBe(DEFAULT_DOCK_INSTALL);
    });

    it("--dock-install は 4 つの値 (elsewhere/missing/ours/absent) をすべて受け付ける", () => {
        expect(DOCK_INSTALL_VALUES).toEqual(["elsewhere", "missing", "ours", "absent"]);
        for (const v of DOCK_INSTALL_VALUES) {
            const r = parseArgs([...FULL_ARGV, "--dock-install", v]);
            expect(r.ok).toBe(true);
            expect(r.args.dockInstall).toBe(v);
        }
    });

    it("--dock-install に不正な値を渡すとエラーになる", () => {
        const r = parseArgs([...FULL_ARGV, "--dock-install", "nonsense"]);
        expect(r.ok).toBe(false);
        expect(r.errors.some((e) => e.includes("--dock-install"))).toBe(true);
    });

    it("--assign-mode を指定しなければ既定 (DEFAULT_DOCK_ASSIGN_MODE = manual = 製品の既定) になる", () => {
        const r = parseArgs(FULL_ARGV);
        expect(DEFAULT_DOCK_ASSIGN_MODE).toBe("manual");
        expect(r.args.dockAssignMode).toBe(DEFAULT_DOCK_ASSIGN_MODE);
    });

    it("--assign-mode は 2 つの値 (manual/auto) をすべて受け付ける", () => {
        expect(DOCK_ASSIGN_MODE_VALUES).toEqual(["manual", "auto"]);
        for (const v of DOCK_ASSIGN_MODE_VALUES) {
            const r = parseArgs([...FULL_ARGV, "--assign-mode", v]);
            expect(r.ok).toBe(true);
            expect(r.args.dockAssignMode).toBe(v);
        }
    });

    it("--assign-mode に不正な値を渡すとエラーになる", () => {
        const r = parseArgs([...FULL_ARGV, "--assign-mode", "nonsense"]);
        expect(r.ok).toBe(false);
        expect(r.errors.some((e) => e.includes("--assign-mode"))).toBe(true);
    });

    it("--step は second-obs / discord-first も受け付ける", () => {
        expect(STEP_VALUES).toEqual(expect.arrayContaining(["second-obs", "discord-first"]));
        for (const v of ["second-obs", "discord-first"]) {
            const r = parseArgs([...FULL_ARGV.slice(0, 6), "--step", v]);
            expect(r.ok).toBe(true);
            expect(r.args.step).toBe(v);
        }
    });

    it("--step は mic-check も受け付ける", () => {
        expect(STEP_VALUES).toContain("mic-check");
        const r = parseArgs([...FULL_ARGV.slice(0, 6), "--step", "mic-check"]);
        expect(r.ok).toBe(true);
        expect(r.args.step).toBe("mic-check");
    });

    it("--warmup 0 は false、--warmup 1 は true になる", () => {
        const off = parseArgs([...FULL_ARGV, "--warmup", "0"]);
        expect(off.ok).toBe(true);
        expect(off.args.warmup).toBe(false);

        const on = parseArgs([...FULL_ARGV, "--warmup", "1"]);
        expect(on.ok).toBe(true);
        expect(on.args.warmup).toBe(true);
    });

    it("--warmup に 0/1 以外を渡すとエラーになる", () => {
        const r = parseArgs([...FULL_ARGV, "--warmup", "true"]);
        expect(r.ok).toBe(false);
        expect(r.errors.some((e) => e.includes("--warmup"))).toBe(true);
    });

    it("--stream と --dry-run と --allow-unknown-encoder は値を取らない真偽フラグ", () => {
        const r = parseArgs([...FULL_ARGV, "--stream", "--dry-run", "--allow-unknown-encoder"]);
        expect(r.ok).toBe(true);
        expect(r.args.stream).toBe(true);
        expect(r.args.dryRun).toBe(true);
        expect(r.args.allowUnknownEncoder).toBe(true);
    });

    it("数の引数は整数に変換される", () => {
        const r = parseArgs([...FULL_ARGV, "--n-screen", "2", "--n-voice", "3", "--seconds", "30", "--deadline-min", "45"]);
        expect(r.ok).toBe(true);
        expect(r.args).toMatchObject({ nScreen: 2, nVoice: 3, seconds: 30, deadlineMin: 45 });
    });

    it("負の数や整数でない数の引数はエラーになる", () => {
        const neg = parseArgs([...FULL_ARGV, "--n-screen", "-1"]);
        expect(neg.ok).toBe(false);
        const frac = parseArgs([...FULL_ARGV, "--seconds", "1.5"]);
        expect(frac.ok).toBe(false);
    });

    it("値を持たないフラグ (次が --で始まる、または末尾) はエラーになる", () => {
        const r = parseArgs([...FULL_ARGV, "--tag"]);
        expect(r.ok).toBe(false);
        expect(r.errors.some((e) => e.includes("--tag"))).toBe(true);
    });

    it("知らないフラグはエラーになる (無視して通さない)", () => {
        const r = parseArgs([...FULL_ARGV, "--mystery", "1"]);
        expect(r.ok).toBe(false);
        expect(r.errors.some((e) => e.includes("--mystery"))).toBe(true);
    });

    it("--tag は文字列としてそのまま入る", () => {
        const r = parseArgs([...FULL_ARGV, "--tag", "window3b-01"]);
        expect(r.args.tag).toBe("window3b-01");
    });

    // ---- call-* の旗 ------------------------------------

    it("--step は call-gate-a/-b/-c/-d と call-1 も受け付ける (--real-profile つき)", () => {
        expect(CALL_STEP_VALUES).toEqual(["call-gate-a", "call-gate-b", "call-gate-c", "call-gate-d", "call-1", "call-hold"]);
        for (const v of CALL_STEP_VALUES) {
            expect(STEP_VALUES).toContain(v);
            const r = parseArgs([...FULL_ARGV.slice(0, 6), "--step", v, "--real-profile"]);
            expect(r.ok).toBe(true);
            expect(r.args.step).toBe(v);
            expect(r.args.realProfile).toBe(true);
        }
    });

    it("call-* は --real-profile が無ければエラー (実際の保存先を使うことを呼び出し行に明示させる)", () => {
        for (const v of CALL_STEP_VALUES) {
            const r = parseArgs([...FULL_ARGV.slice(0, 6), "--step", v]);
            expect(r.ok).toBe(false);
            expect(r.errors.some((e) => e.includes("--real-profile"))).toBe(true);
        }
    });

    it("call-* 以外に --real-profile / --call-window を渡すとエラー (テスト用の送信側だけを使う実行は、実際の保存先も利用者の Vesktop も触らない)", () => {
        const real = parseArgs([...FULL_ARGV, "--real-profile"]);
        expect(real.ok).toBe(false);
        expect(real.errors.some((e) => e.includes("--real-profile"))).toBe(true);

        const call = parseArgs([...FULL_ARGV, "--call-window"]);
        expect(call.ok).toBe(false);
        expect(call.errors.some((e) => e.includes("--call-window"))).toBe(true);
    });

    it("--call-window / --real-profile / --allow-state-json-write は値を取らない真偽フラグ", () => {
        const r = parseArgs([...FULL_ARGV.slice(0, 6), "--step", "call-1", "--call-window", "--real-profile", "--allow-state-json-write"]);
        expect(r.ok).toBe(true);
        expect(r.args).toMatchObject({ callWindow: true, realProfile: true, allowStateJsonWrite: true });
    });

    // ---- 実行スクリプトだけが知る値 (後片付けで消す対象) --------------

    it("--browser-profile / --shared-content-dir は繰り返して渡せ、--temp-vencord-build は 1 つ", () => {
        const r = parseArgs([
            ...FULL_ARGV.slice(0, 6),
            "--step",
            "call-1",
            "--real-profile",
            "--browser-profile",
            "C:/tmp/p1",
            "--browser-profile",
            "C:/tmp/p2",
            "--shared-content-dir",
            "C:/tmp/share",
            "--temp-vencord-build",
            "C:/tmp/vencord",
        ]);
        expect(r.ok).toBe(true);
        expect(r.args.browserProfiles).toEqual(["C:/tmp/p1", "C:/tmp/p2"]);
        expect(r.args.sharedContentDirs).toEqual(["C:/tmp/share"]);
        expect(r.args.tempVencordBuild).toBe("C:/tmp/vencord");
        expect(parseArgs(FULL_ARGV).args).toMatchObject({ browserProfiles: [], sharedContentDirs: [], tempVencordBuild: null });
    });

    it("実行スクリプトだけが知る値は --step call-1 だけが受け付ける (ゲートの 4 つとテスト用の送信側だけの実行には消す対象がない)", () => {
        const gate = parseArgs([...FULL_ARGV.slice(0, 6), "--step", "call-gate-a", "--real-profile", "--browser-profile", "C:/tmp/p1"]);
        expect(gate.ok).toBe(false);
        expect(gate.errors.some((e) => e.includes("--browser-profile"))).toBe(true);

        const fake = parseArgs([...FULL_ARGV, "--temp-vencord-build", "C:/tmp/vencord"]);
        expect(fake.ok).toBe(false);
    });

    it("call-1 は渡された保存先を手順の一番先に書き、渡されなければその手順ごとない", () => {
        const none = planSteps(parseArgs([...FULL_ARGV.slice(0, 6), "--step", "call-1", "--real-profile"]).args);
        expect(none.steps.some((s) => s.kind === "write-call-window-artifacts")).toBe(false);

        const some = planSteps(parseArgs([...FULL_ARGV.slice(0, 6), "--step", "call-1", "--real-profile", "--browser-profile", "C:/tmp/p1"]).args);
        expect(some.steps[0]).toMatchObject({
            kind: "write-call-window-artifacts",
            path: CALL_WINDOW_ARTIFACTS_REL,
            artifacts: { tempVencordBuild: null, browserProfiles: ["C:/tmp/p1"], sharedContentDirs: [] },
        });
    });

    it("mergeCallWindowArtifacts は消さずに足すだけ (前の回に書かれた保存先を落とさない)", () => {
        const merged = mergeCallWindowArtifacts(
            { tempVencordBuild: "C:/tmp/old", browserProfiles: ["C:/tmp/p1"], sharedContentDirs: [] },
            { tempVencordBuild: null, browserProfiles: ["C:/tmp/p1", "C:/tmp/p2"], sharedContentDirs: ["C:/tmp/share"] },
        );
        expect(merged).toEqual({
            tempVencordBuild: "C:/tmp/old",
            browserProfiles: ["C:/tmp/p1", "C:/tmp/p2"],
            sharedContentDirs: ["C:/tmp/share"],
        });
        expect(mergeCallWindowArtifacts(null, { tempVencordBuild: "C:/tmp/new", browserProfiles: [], sharedContentDirs: [] })).toEqual({
            tempVencordBuild: "C:/tmp/new",
            browserProfiles: [],
            sharedContentDirs: [],
        });
    });

    it("--decoder は software / hardware だけを受け付け、既定は software (製品の既定と同じ)", () => {
        expect(DECODER_VALUES).toEqual(["software", "hardware"]);
        expect(DEFAULT_DECODER).toBe("software");
        expect(parseArgs(FULL_ARGV).args.decoder).toBe(DEFAULT_DECODER);
        for (const v of DECODER_VALUES) {
            const r = parseArgs([...FULL_ARGV, "--decoder", v]);
            expect(r.ok).toBe(true);
            expect(r.args.decoder).toBe(v);
        }
        const bad = parseArgs([...FULL_ARGV, "--decoder", "d3d11va"]);
        expect(bad.ok).toBe(false);
        expect(bad.errors.some((e) => e.includes("--decoder"))).toBe(true);
    });

    it("--call-minutes は 0 以上の整数で、既定は 20", () => {
        expect(DEFAULT_CALL_MINUTES).toBe(20);
        expect(parseArgs(FULL_ARGV).args.callMinutes).toBe(DEFAULT_CALL_MINUTES);
        const r = parseArgs([...FULL_ARGV, "--call-minutes", "10"]);
        expect(r.ok).toBe(true);
        expect(r.args.callMinutes).toBe(10);
        expect(parseArgs([...FULL_ARGV, "--call-minutes", "-1"]).ok).toBe(false);
    });
});

describe("blockersFromProcessList", () => {
    it("Program Files 配下の obs64.exe は止める", () => {
        const b = blockersFromProcessList([{ name: "obs64.exe", path: "C:\\Program Files\\obs-studio\\bin\\64bit\\obs64.exe" }]);
        expect(b).toHaveLength(1);
        expect(b[0].process).toBe("obs64.exe");
    });

    it("ポータブル OBS (Program Files の外) の obs64.exe は止めない", () => {
        const b = blockersFromProcessList([{ name: "obs64.exe", path: "C:\\Users\\user\\src\\cordmux\\_work\\obs\\bin\\64bit\\obs64.exe" }]);
        expect(b).toEqual([]);
    });

    it("--call-window のときは vesktop.exe だけを確認の対象から外す", () => {
        const list = [
            { name: "vesktop.exe", path: null },
            { name: "foo.exe", path: null },
            { name: "obs64.exe", path: "C:\\Program Files\\obs-studio\\bin\\64bit\\obs64.exe" },
        ];
        const protectedNames = ["foo.exe"];
        const withoutFlag = blockersFromProcessList(list, { protectedNames }).map((b) => b.process);
        expect(withoutFlag).toEqual(expect.arrayContaining(["vesktop.exe", "foo.exe", "obs64.exe"]));

        const withFlag = blockersFromProcessList(list, { callWindow: true, protectedNames }).map((b) => b.process);
        expect(withFlag).not.toContain("vesktop.exe");
        expect(withFlag).toEqual(expect.arrayContaining(["foo.exe", "obs64.exe"]));
    });

    it("path が分からない obs64.exe は安全側に倒して止める", () => {
        const b = blockersFromProcessList([{ name: "obs64.exe", path: null }]);
        expect(b).toHaveLength(1);
    });

    it("vesktop.exe は場所に関わらず止める。指定のないプロセスは止めない", () => {
        const b = blockersFromProcessList([
            { name: "foo.exe", path: "D:\\tools\\foo.exe" },
            { name: "vesktop.exe", path: "C:\\Users\\user\\AppData\\Local\\Programs\\vesktop\\Vesktop.exe" },
        ]);
        expect(b.map((x) => x.process)).toEqual(["vesktop.exe"]);
    });

    it("利用者が指定したプロセスは、場所に関わらず、大文字小文字を区別せずに止める", () => {
        const b = blockersFromProcessList(
            [
                { name: "FOO.EXE", path: "D:\\tools\\foo.exe" },
                { name: "bar.exe", path: null },
                { name: "baz.exe", path: null },
            ],
            { protectedNames: ["foo.exe", "Bar.exe"] },
        );
        expect(b.map((x) => x.process)).toEqual(["FOO.EXE", "bar.exe"]);
        expect(b[0].reason).toContain("利用者が指定した");
    });

    it("指定を渡さなければ、既定の一覧にないプロセスは止めない", () => {
        expect(blockersFromProcessList([{ name: "foo.exe", path: null }])).toEqual([]);
        expect(DEFAULT_GUARDED_PROCESS_NAMES).toEqual(["obs64.exe", "vesktop.exe"]);
    });

    it("名前の大文字小文字は無視する", () => {
        const b = blockersFromProcessList([{ name: "OBS64.EXE", path: "c:\\program files\\obs-studio\\obs64.exe" }]);
        expect(b).toHaveLength(1);
    });

    it("無関係なプロセスや空リストは何も返さない", () => {
        expect(blockersFromProcessList([{ name: "chrome.exe", path: "C:\\x\\chrome.exe" }])).toEqual([]);
        expect(blockersFromProcessList([])).toEqual([]);
        expect(blockersFromProcessList(undefined)).toEqual([]);
    });
});

describe("parseProtectedProcessNames と loadProtectedProcesses (守るプロセスのファイル)", () => {
    it("1 行に 1 つ読み、空行と # で始まる行と前後の空白を落とす", () => {
        const text = "# 守るプロセス\r\n\r\n  foo.exe  \r\n#bar.exe\n   \nBaz Qux.exe\n";
        expect(parseProtectedProcessNames(text)).toEqual(["foo.exe", "Baz Qux.exe"]);
    });

    it("先頭の BOM を読み飛ばし、大文字小文字だけが違う行は 1 つにまとめる", () => {
        expect(parseProtectedProcessNames("\uFEFFFoo.exe\nfoo.EXE\n")).toEqual(["Foo.exe"]);
    });

    it("空の中身や未定義は空の一覧", () => {
        expect(parseProtectedProcessNames("")).toEqual([]);
        expect(parseProtectedProcessNames(undefined)).toEqual([]);
    });

    it("環境変数が空なら何も足さず、ファイルも読まない", () => {
        const read = () => {
            throw new Error("読まれてはいけない");
        };
        expect(loadProtectedProcesses(undefined, read)).toEqual({ names: [], blockers: [] });
        expect(loadProtectedProcesses("  ", read)).toEqual({ names: [], blockers: [] });
    });

    it("指定のファイルを読んで名前を返す", () => {
        const seen: string[] = [];
        const r = loadProtectedProcesses(" C:/x/list.txt ", (p: string) => {
            seen.push(p);
            return "foo.exe\n";
        });
        expect(seen).toEqual(["C:/x/list.txt"]);
        expect(r).toEqual({ names: ["foo.exe"], blockers: [] });
    });

    it("指定したのに読めないときは、始めないための blockers を返す", () => {
        const r = loadProtectedProcesses("C:/x/missing.txt", () => {
            throw new Error("ENOENT: no such file");
        });
        expect(r.names).toEqual([]);
        expect(r.blockers).toHaveLength(1);
        expect(r.blockers[0]).toMatchObject({ process: PROTECTED_PROCESSES_ENV, path: "C:/x/missing.txt" });
        expect(r.blockers[0].reason).toContain("ENOENT");
    });
});

describe("encoderBusy", () => {
    it("0 % は busy:false", () => {
        expect(encoderBusy("0 %\n").busy).toBe(false);
    });

    it("複数 GPU で 1 つでも 0 でなければ busy:true", () => {
        const r = encoderBusy("0 %\n5 %\n");
        expect(r.busy).toBe(true);
        expect(r.values).toEqual([0, 5]);
    });

    it("単位なし (nounits) でも読める", () => {
        expect(encoderBusy("0\n").busy).toBe(false);
        expect(encoderBusy("3\n").busy).toBe(true);
    });

    it("[N/A] のような数値でない行は busy に数えない", () => {
        const r = encoderBusy("[N/A]\n");
        expect(r.busy).toBe(false);
        expect(r.values).toEqual([]);
    });

    it("複数列 (カンマ区切り) は先頭の列だけを見る", () => {
        const r = encoderBusy("4 %, 0 %, 1 %\n");
        expect(r.values).toEqual([4]);
        expect(r.busy).toBe(true);
    });

    it("空文字列は busy:false", () => {
        expect(encoderBusy("").busy).toBe(false);
        expect(encoderBusy(undefined).busy).toBe(false);
    });
});

describe("guardBlocked", () => {
    const clean = { blockers: [], encoderBusy: false, encoderValues: [0], encoderError: null };

    it("邪魔なプロセスがなく GPU 使用率 0 が読めれば通す", () => {
        expect(guardBlocked(clean)).toBe(false);
    });

    it("nvidia-smi が失敗した (encoderError あり) ときは、値が全部0でも止める", () => {
        expect(guardBlocked({ ...clean, encoderError: "spawn nvidia-smi ENOENT", encoderValues: [] })).toBe(true);
    });

    it("nvidia-smi の出力が1行も数値でない (values 空) ときも止める", () => {
        expect(guardBlocked({ ...clean, encoderValues: [] })).toBe(true);
    });

    it("allowUnknownEncoder を立てれば、確認できなくても通す (明示の抜け道)", () => {
        expect(guardBlocked({ ...clean, encoderError: "spawn nvidia-smi ENOENT", encoderValues: [], allowUnknownEncoder: true })).toBe(false);
    });

    it("邪魔なプロセスがあれば、GPU が確認できても止める", () => {
        expect(guardBlocked({ ...clean, blockers: [{ process: "vesktop.exe" }] })).toBe(true);
    });

    it("GPU 使用率が 0 でなければ止める", () => {
        expect(guardBlocked({ ...clean, encoderBusy: true, encoderValues: [5] })).toBe(true);
    });

    it("実際の通話の実行で、使い手が全部通話のアプリと確認できたときは、使用率が 0 でなくても通す", () => {
        expect(guardBlocked({ ...clean, encoderBusy: true, encoderValues: [7], encoderOnlyCallApps: true })).toBe(false);
    });

    it("使い手が通話のアプリでも、邪魔なプロセスがあれば止める", () => {
        expect(guardBlocked({ ...clean, blockers: [{ process: "obs64.exe" }], encoderBusy: true, encoderValues: [7], encoderOnlyCallApps: true })).toBe(true);
    });
});

describe("parseEncoderSessionPids / encoderUsedOnlyByCallApps (実際の通話の実行でのエンコーダの使い手)", () => {
    // 実際の通話の実行で nvidia-smi encodersessions が出した形 (一覧を繰り返す)。
    const sample = [
        "# GPU Session    Process   Codec       H       V Average     Average",
        "# Idx      Id         Id    Type     Res     Res     FPS Latency(us)",
        "    0    1951       9368   H.264    1920    1080      59         769",
        "    0    1952       4242   H.264    1280     720      30         500",
        "# GPU Session    Process   Codec       H       V Average     Average",
        "# Idx      Id         Id    Type     Res     Res     FPS Latency(us)",
        "    0    1951       9368   H.264    1920    1080      65        1163",
        "    0    1999       7777   H.264    1920    1080      60         900",
    ].join("\r\n");

    it("最初の一覧の Process Id だけを重ねずに返す", () => {
        expect(parseEncoderSessionPids(sample)).toEqual([9368, 4242]);
    });

    it("空や見出しだけなら空", () => {
        expect(parseEncoderSessionPids("")).toEqual([]);
        expect(parseEncoderSessionPids("# GPU Session Process\n# Idx Id Id")).toEqual([]);
    });

    it("全部が通話のアプリなら true (大文字小文字は問わない)", () => {
        expect(encoderUsedOnlyByCallApps([{ pid: 9368, name: "Discord.exe" }, { pid: 4242, name: "chrome.exe" }])).toBe(true);
    });

    it("1 つでもほかのアプリ・名前の引けない使い手があれば false", () => {
        expect(encoderUsedOnlyByCallApps([{ pid: 9368, name: "Discord.exe" }, { pid: 1, name: "obs64.exe" }])).toBe(false);
        expect(encoderUsedOnlyByCallApps([{ pid: 9368, name: null }])).toBe(false);
    });

    it("使い手が 1 つも読めなければ false (確認できないので止める側)", () => {
        expect(encoderUsedOnlyByCallApps([])).toBe(false);
    });
});

describe("judgeModuleLoaded", () => {
    it("Loaded Modules: の節の中に cordmux.dll があれば hit", () => {
        const lines = [
            "info: ---------------------------------",
            "info:   Loaded Modules:",
            "info:     win-dshow.dll",
            "info:     cordmux.dll",
            "info:     obs-websocket.dll",
            "info: ---------------------------------",
            "info: something else",
        ];
        const r = judgeModuleLoaded(lines, "cordmux.dll");
        expect(r.hit).toBe(true);
        expect(r.blockLines.some((l) => l.includes("cordmux.dll"))).toBe(true);
    });

    it("節の外にある「読み込み失敗」の行は誤って hit にしない", () => {
        const lines = [
            "info: ---------------------------------",
            "info: Failed to load module file 'C:/obs/plugins/cordmux/cordmux.dll', file not found",
            "info: ---------------------------------",
            "info:   Loaded Modules:",
            "info:     win-dshow.dll",
            "info:     obs-websocket.dll",
            "info: ---------------------------------",
        ];
        const r = judgeModuleLoaded(lines, "cordmux.dll");
        expect(r.hit).toBe(false);
    });

    it("Loaded Modules: の節そのものが無ければ hit:false・blockLines:[]", () => {
        const r = judgeModuleLoaded(["info: OBS 33.0.1 起動", "info: something"], "cordmux.dll");
        expect(r).toEqual({ hit: false, blockLines: [] });
    });
});

describe("judgeModuleNotLoaded (old-obs: 新しい置き方の cordmux が OBS 32 に読まれない)", () => {
    const started = [
        "info: ---------------------------------",
        "info:   Loaded Modules:",
        "info:     win-dshow.dll",
        "info:     obs-websocket.dll",
        "info: ---------------------------------",
    ];

    it("Loaded Modules: の節があり、cordmux が節にも Loading module の行にもなければ hit", () => {
        const r = judgeModuleNotLoaded(started, "cordmux.dll");
        expect(r.hit).toBe(true);
        expect(r.loadingLines).toEqual([]);
    });

    it("節に cordmux.dll があれば hit にしない", () => {
        const lines = [...started.slice(0, 3), "info:     cordmux.dll", ...started.slice(3)];
        expect(judgeModuleNotLoaded(lines, "cordmux.dll").hit).toBe(false);
    });

    it("Loading module: cordmux の行があれば、節になくても hit にしない", () => {
        const r = judgeModuleNotLoaded(["debug: Loading module: cordmux.dll", ...started], "cordmux.dll");
        expect(r.hit).toBe(false);
        expect(r.loadingLines).toHaveLength(1);
    });

    it("ログが途中までで節がなければ、何も出ていなくても hit にしない", () => {
        expect(judgeModuleNotLoaded(["info: OBS 32.2.2 起動"], "cordmux.dll").hit).toBe(false);
        expect(judgeModuleNotLoaded([], "cordmux.dll").hit).toBe(false);
    });
});

describe("judgeGuideUpdateHint (手順書の「ドックが表示されない」に OBS 33 以上への更新がある)", () => {
    const ja = { rel: "ja.md", title: "ドックが表示されない", needle: "33より古いときは、OBSを更新します" };
    const en = { rel: "en.md", title: "The dock doesn't appear", needle: "older than 33, update OBS" };
    const jaText = ["## 困ったとき", "", "### ドックが表示されない", "", "- OBSのバージョン: 確認します。33より古いときは、OBSを更新します。", "", "### 別の項目", "", "33より古いときは、OBSを更新します。"].join("\n");
    const enText = ["## Troubleshooting", "", "### The dock doesn't appear", "", "- OBS version: if it's older than 33, update OBS.", "", "### Another item"].join("\n");

    it("項の中に案内の文があれば ok (空白と改行は比べない)", () => {
        const r = judgeGuideUpdateHint([{ ...ja, text: jaText }, { ...en, text: enText }]);
        expect(r.ok).toBe(true);
    });

    it("案内の文が項の外 (次の ### か ## の項) にしかなければ ng", () => {
        const next = ["### ドックが表示されない", "", "確認します。", "", "### 別の項目", "", "33より古いときは、OBSを更新します。"].join("\n");
        expect(judgeGuideUpdateHint([{ ...ja, text: next }]).ok).toBe(false);
        const nextSection = ["### ドックが表示されない", "", "確認します。", "", "## 更新", "", "33より古いときは、OBSを更新します。"].join("\n");
        expect(judgeGuideUpdateHint([{ ...ja, text: nextSection }]).ok).toBe(false);
    });

    it("項の ### の見出しがない、古い案内 (31.1.1) のままのときは ng", () => {
        expect(judgeGuideUpdateHint([{ ...ja, text: "### 別の項目\n\n33より古いときは、OBSを更新します。" }]).ok).toBe(false);
        expect(judgeGuideUpdateHint([{ ...ja, text: "- ドックが表示されない\n\n33より古いときは、OBSを更新します。" }]).ok).toBe(false);
        expect(judgeGuideUpdateHint([{ ...ja, text: "### ドックが表示されない\n\n31.1.1より古いときは、OBSを更新します。" }]).ok).toBe(false);
        expect(judgeGuideUpdateHint([]).ok).toBe(false);
    });

    it("実際の手順書 (日本語と英語) が、実行計画の GUIDE_UPDATE_HINTS の条件を満たす", () => {
        const root = join(dirname(fileURLToPath(import.meta.url)), "..", "..", "..");
        const guides = GUIDE_UPDATE_HINTS.map((h) => ({ ...h, text: readFileSync(join(root, h.rel), "utf8") }));
        const r = judgeGuideUpdateHint(guides);
        expect(r.reason).toBe(null);
        expect(r.ok).toBe(true);
    });
});

describe("pickNewResultFiles", () => {
    it("before になく prefix で始まるものだけを新規とみなす", () => {
        const before = ["a.json", "product-send-t0.json"];
        const after = [...before, "product-send-t1.json", "other-file.json"];
        expect(pickNewResultFiles(before, after, "product-send-")).toEqual(["product-send-t1.json"]);
    });

    it("何も増えていなければ空", () => {
        const before = ["a.json"];
        expect(pickNewResultFiles(before, before, "product-send-")).toEqual([]);
    });
});

describe("deadlineExceeded", () => {
    const start = 1_000_000;
    it("期限ちょうどで超過扱いにする (>=)", () => {
        expect(deadlineExceeded(start, start + 120 * 60 * 1000, 120)).toBe(true);
    });
    it("期限の 1ms 手前はまだ超過していない", () => {
        expect(deadlineExceeded(start, start + 120 * 60 * 1000 - 1, 120)).toBe(false);
    });
    it("開始直後は超過していない", () => {
        expect(deadlineExceeded(start, start, 120)).toBe(false);
    });
});

describe("filesToPlace", () => {
    it("cordmux.dll (必須) / 同梱 DLL 一式 (任意) / data (必須) の 3 件を返す", () => {
        const list = filesToPlace("C:/plugin");
        expect(list).toHaveLength(3);

        const dll = list.find((x) => x.id === "cordmux-dll");
        expect(dll).toMatchObject({
            required: true,
            src: "C:/plugin/cordmux.dll",
            destRel: "plugins/cordmux/cordmux.dll",
        });

        const siblings = list.find((x) => x.id === "sibling-dlls");
        expect(siblings).toMatchObject({
            required: false,
            src: "C:/plugin",
            destRel: "plugins/cordmux",
        });
        expect(siblings.excludeNames).toEqual(["cordmux.dll"]);

        const data = list.find((x) => x.id === "data-dir");
        expect(data).toMatchObject({
            required: true,
            src: "C:/plugin/data",
            destRel: "plugins/cordmux/data",
        });
    });

    it("パスは文字列連結だけで組む (末尾のスラッシュを重ねない)", () => {
        const list = filesToPlace("C:/a/b");
        for (const item of list) {
            expect(item.src.includes("//")).toBe(false);
        }
    });
});

// seed-config を持たない --step (OBS の設定を作り直さない側)。
// mic-check は OBS を起動しない。call-gate-b は state.json を読むだけ、
// call-gate-c / call-gate-d は call-gate-a が置いたものをそのまま使う (置き直すと Vesktop が
// 指している先を実行の途中で作り直すことになる)。
// webcodecs-check も OBS を起動しない (Electron の短い起動だけ)。
// install-roundtrip も入れる: 先頭は作業場所の作り直しと OBS の zip の展開で、seed-config は
// その後の --shape user (利用者らしい型。dockInstall / assignMode / 録画の差し替えの鍵を持たない)。
const STEPS_WITHOUT_SEED = ["mic-check", "call-gate-b", "call-gate-c", "call-gate-d", "webcodecs-check", "install-roundtrip"];
// harness (テスト用の送信側) を呼ばない --step。call-* は実機の Vesktop が送信側である。
const STEPS_WITHOUT_HARNESS = [
    "place",
    "dock-view",
    "mic-check",
    "call-gate-a",
    "call-gate-b",
    "call-gate-c",
    "call-gate-d",
    "call-1",
    // call-hold の実際の実行でも送信側は利用者の Vesktop (テスト用の送信側を繋ぐのは --rehearsal だけ。run-window-hold.test.ts を参照)。
    "call-hold",
    "webcodecs-check",
    // 既定の場面 roundtrip はテスト用の送信側を起こさない (起こすのは force-update-tamper だけ)。
    "install-roundtrip",
    // hold は --n-screen / --n-voice を渡したときだけ起こす (既定の呼び出し行では起こさない)。
    "hold",
];

// どの --step でも ok:true になる呼び出し行 (call-* は --real-profile が必須、install-roundtrip は --obs /
// --plugin を取らず --tag / --zip / --obs-zip が必須)。
function argvFor(step: string): string[] {
    if (step === "install-roundtrip") {
        return ["--out", "C:/out", "--step", step, "--tag", "t1", "--zip", "C:/z/cordmux.zip", "--obs-zip", "C:/z/obs.zip"];
    }
    const callFlags = step.startsWith("call-") ? ["--call-window", "--real-profile"] : [];
    return ["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", step, ...callFlags];
}

describe("planSteps", () => {
    function argsFor(step: string, overrides: Record<string, unknown> = {}) {
        // call-* は --real-profile が必須で、--call-window も受け付ける。
        const r = parseArgs(argvFor(step));
        expect(r.ok).toBe(true);
        return Object.assign(r.args, overrides);
    }

    it("--step が不正なら ok:false になる", () => {
        const r = planSteps({ step: "nonsense" });
        expect(r.ok).toBe(false);
        expect(r.steps).toEqual([]);
    });

    it("18 種類すべてで、末尾は cleanup、その手前は summarize、さらに手前は write-result。seed-config を先頭に持たない 6 つ以外は先頭が seed-config", () => {
        expect(STEP_VALUES).toHaveLength(18);
        for (const stepName of STEP_VALUES) {
            const r = planSteps(argsFor(stepName));
            expect(r.ok).toBe(true);
            expect(r.steps.length).toBeGreaterThan(0);
            if (!STEPS_WITHOUT_SEED.includes(stepName)) {
                expect(r.steps[0].kind).toBe("seed-config");
            }
            expect(r.steps.at(-1).kind).toBe("cleanup");
            expect(r.steps.at(-2).kind).toBe("summarize");
            expect(r.steps.at(-3).kind).toBe("write-result");
        }
    });

    it("harness / harness-start を呼ぶすべての手順に syncTone (screen/voice/none) が明示される", () => {
        // place と dock-view は harness を呼ばない (place は配置と 2 つ目の OBS の確認だけ、
        // dock-view は配置・起動・撮影だけ)。mic-check は OBS も harness も呼ばない。
        // call-* は実機の Vesktop が送信側なので
        // テスト用の送信側を起こさない。いずれも対象から外す。
        for (const stepName of STEP_VALUES.filter((s) => !STEPS_WITHOUT_HARNESS.includes(s))) {
            const r = planSteps(argsFor(stepName));
            const harnessSteps = r.steps.filter((s) => s.kind === "harness" || s.kind === "harness-start");
            expect(harnessSteps.length).toBeGreaterThan(0);
            for (const hs of harnessSteps) {
                expect(SYNC_TONE_VALUES).toContain(hs.syncTone);
            }
        }
    });

    it("id は kind ごとの連番で、すべて一意", () => {
        const r = planSteps(argsFor("place"));
        const ids = r.steps.map((s) => s.id);
        expect(new Set(ids).size).toBe(ids.length);
        const copyFileIds = r.steps.filter((s) => s.kind === "copy-file").map((s) => s.id);
        expect(copyFileIds).toEqual(["copy-file-1"]);
    });

    it("place は filesToPlace の 3 件の配置と、2 つ目の OBS の起動・確認・後始末を含む", () => {
        const r = planSteps(argsFor("place"));
        const kinds = r.steps.map((s) => s.kind);
        expect(kinds.filter((k) => k === "copy-file")).toHaveLength(1);
        expect(kinds.filter((k) => k === "copy-dir-contents")).toHaveLength(1);
        expect(kinds.filter((k) => k === "copy-dir")).toHaveLength(1);
        expect(kinds).toContain("launch-obs-second");
        expect(kinds.filter((k) => k === "close-obs")).toHaveLength(2);
        expect(kinds.filter((k) => k === "read-obs-log")).toHaveLength(2);
    });

    it("through は harness を n=1, seconds=10 で 1 回だけ呼び、frames.py だけを判定に使う", () => {
        const r = planSteps(argsFor("through", { hw: "prefer-software", warmup: true }));
        const harnessSteps = r.steps.filter((s) => s.kind === "harness");
        expect(harnessSteps).toHaveLength(1);
        expect(harnessSteps[0]).toMatchObject({ mode: "product", n: 1, seconds: 10, hw: "prefer-software", warmup: true });
        const judgeTools = r.steps.filter((s) => s.kind === "judge").map((s) => s.tool);
        expect(judgeTools).toEqual(["frames.py"]);
    });

    it("integration は --n-screen / --n-voice / --seconds を harness にそのまま渡し、3 種の判定ツールを順に呼ぶ", () => {
        const r = planSteps(argsFor("integration", { nScreen: 4, nVoice: 8, seconds: 60 }));
        const harnessSteps = r.steps.filter((s) => s.kind === "harness");
        expect(harnessSteps).toHaveLength(1);
        expect(harnessSteps[0]).toMatchObject({ mode: "product", nScreen: 4, nVoice: 8, seconds: 60 });
        const judgeTools = r.steps.filter((s) => s.kind === "judge").map((s) => s.tool);
        expect(judgeTools).toEqual(["frames.py", "sync.py", "smooth.py"]);
    });

    it.each(["through", "integration"])(
        "%s: record-start が harness より前、record-stop が harness より後に来る",
        (stepName) => {
            const r = planSteps(argsFor(stepName, { nScreen: 4, nVoice: 8, seconds: 60 }));
            const idx = (kind: string) => r.steps.findIndex((s) => s.kind === kind);
            const startIdx = idx("record-start");
            const harnessIdx = idx("harness");
            const stopIdx = idx("record-stop");
            expect(startIdx).toBeGreaterThanOrEqual(0);
            expect(harnessIdx).toBeGreaterThanOrEqual(0);
            expect(stopIdx).toBeGreaterThanOrEqual(0);
            expect(startIdx).toBeLessThan(harnessIdx);
            expect(harnessIdx).toBeLessThan(stopIdx);
        },
    );

    it.each(["through", "integration"])("%s: --stream 無指定なら配信の受け口の手順はなく、record-start/stop の stream は false", (stepName) => {
        const r = planSteps(argsFor(stepName));
        expect(r.steps.filter((s) => s.kind === "stream-sink-start")).toHaveLength(0);
        expect(r.steps.filter((s) => s.kind === "stream-sink-stop")).toHaveLength(0);
        expect(r.steps.find((s) => s.kind === "record-start")).toMatchObject({ stream: false });
        expect(r.steps.find((s) => s.kind === "record-stop")).toMatchObject({ stream: false });
    });

    it.each(["through", "integration"])("%s: --stream ありなら配信の受け口が録画をまたぐ位置に入る", (stepName) => {
        const r = planSteps(argsFor(stepName, { stream: true }));
        const idx = (kind: string) => r.steps.findIndex((s) => s.kind === kind);
        expect(idx("stream-sink-start")).toBeGreaterThanOrEqual(0);
        expect(idx("stream-sink-stop")).toBeGreaterThanOrEqual(0);
        expect(idx("stream-sink-start")).toBeLessThan(idx("record-start"));
        expect(idx("record-stop")).toBeLessThan(idx("stream-sink-stop"));
        expect(r.steps.find((s) => s.kind === "record-start")).toMatchObject({ stream: true });
        expect(r.steps.find((s) => s.kind === "record-stop")).toMatchObject({ stream: true });
    });

    it("guard はトークンなし/違いの 2 接続と、背圧・降格・負荷を下げる手段の 3 つ、計 5 つの harness 呼び出しを含む", () => {
        const r = planSteps(argsFor("guard"));
        const probeVariants = r.steps.filter((s) => s.kind === "ws-probe").map((s) => s.variant);
        expect(probeVariants).toEqual(["no-secret", "wrong-secret"]);
        const harnessSteps = r.steps.filter((s) => s.kind === "harness");
        expect(harnessSteps).toHaveLength(5);
    });

    it("guard の降格手順は failStage に文字列ではなく数を渡す", () => {
        const r = planSteps(argsFor("guard"));
        const degrade = r.steps.filter((s) => s.kind === "harness").find((s) => s.extra && "failStage" in s.extra);
        expect(degrade).toBeDefined();
        expect(typeof degrade.extra.failStage).toBe("number");
        expect(degrade.extra.failStage).toBe(3);
    });

    it("guard は背圧の harness の直後に judge-backpressure を 1 回だけ挟む", () => {
        const r = planSteps(argsFor("guard"));
        const overflowIdx = r.steps.findIndex((s) => s.kind === "harness" && s.extra && s.extra.oversend === true);
        expect(overflowIdx).toBeGreaterThanOrEqual(0);
        expect(r.steps[overflowIdx + 1].kind).toBe("judge-backpressure");
        expect(r.steps.filter((s) => s.kind === "judge-backpressure")).toHaveLength(1);
    });

    it("guard の背圧の harness は stats_loop の 10 秒周期に届くよう seconds を明示する", () => {
        const r = planSteps(argsFor("guard"));
        const overflow = r.steps.find((s) => s.kind === "harness" && s.extra && s.extra.oversend === true);
        expect(overflow.seconds).toBeGreaterThanOrEqual(20);
    });

    it("integration の既定 (default) は --n-screen/--n-voice/--seconds どおりで syncTone=screen、3 種の判定ツールを実行する", () => {
        const r = planSteps(argsFor("integration", { nScreen: 4, nVoice: 8, seconds: 60 }));
        const harnessSteps = r.steps.filter((s) => s.kind === "harness");
        expect(harnessSteps).toHaveLength(1);
        expect(harnessSteps[0]).toMatchObject({ mode: "product", nScreen: 4, nVoice: 8, seconds: 60, syncTone: DEFAULT_SYNC_TONE });
        const judgeTools = r.steps.filter((s) => s.kind === "judge").map((s) => s.tool);
        expect(judgeTools).toEqual(["frames.py", "sync.py", "smooth.py"]);
    });

    it("integration --integration-config one-screen は画面 1・声 0・60 秒・syncTone=screen を固定で使う (--n-screen 等の指定は無視する)", () => {
        const r = planSteps(argsFor("integration", { integrationConfig: "one-screen", nScreen: 4, nVoice: 8, seconds: 999 }));
        const harnessSteps = r.steps.filter((s) => s.kind === "harness");
        expect(harnessSteps).toHaveLength(1);
        expect(harnessSteps[0]).toMatchObject({ mode: "product", nScreen: 1, nVoice: 0, seconds: 60, syncTone: DEFAULT_SYNC_TONE });
        const judgeTools = r.steps.filter((s) => s.kind === "judge").map((s) => s.tool);
        expect(judgeTools).toEqual(["frames.py", "sync.py", "smooth.py"]);
    });

    it("integration --integration-config one-screen-one-voice は画面 1・声 1・180 秒・syncTone=voice を固定で使い、sync.py だけを label 付きで実行する (A/V の測り直し: 画面 1 + 声 1 でマーカーは声)", () => {
        const r = planSteps(argsFor("integration", { integrationConfig: "one-screen-one-voice", nScreen: 4, nVoice: 8, seconds: 999 }));
        const harnessSteps = r.steps.filter((s) => s.kind === "harness");
        expect(harnessSteps).toHaveLength(1);
        expect(harnessSteps[0]).toMatchObject({ mode: "product", nScreen: 1, nVoice: 1, seconds: 180, syncTone: "voice" });
        const judgeSteps = r.steps.filter((s) => s.kind === "judge");
        expect(judgeSteps.map((s) => s.tool)).toEqual(["sync.py"]);
        expect(judgeSteps[0].label).toBe("voice_minus_screen_video_ms");
    });

    it("second-obs は place と同じ 2 つ目の OBS の確認 + シーンコレクションの往復 (obsws-request) + wait-connection + harness-start/wait を含む", () => {
        const r = planSteps(argsFor("second-obs"));
        const kinds = r.steps.map((s) => s.kind);
        expect(kinds).toContain("launch-obs-second");
        expect(kinds.filter((k) => k === "read-obs-log")).toHaveLength(1);
        expect(kinds.filter((k) => k === "close-obs")).toHaveLength(2);
        expect(kinds.filter((k) => k === "wait-connection")).toHaveLength(1);
        expect(kinds.filter((k) => k === "harness-start")).toHaveLength(1);
        expect(kinds.filter((k) => k === "harness-wait")).toHaveLength(1);
        expect(kinds.filter((k) => k === "judge-connection-held")).toHaveLength(1);

        const requests = r.steps.filter((s) => s.kind === "obsws-request").map((s) => ({ request: s.request, name: s.requestData && s.requestData.sceneCollectionName }));
        expect(requests).toEqual([
            { request: "SetCurrentSceneCollection", name: SECOND_OBS_EMPTY_SCENE_COLLECTION },
            { request: "GetSceneCollectionList", name: undefined },
            { request: "SetCurrentSceneCollection", name: SECOND_OBS_MAIN_SCENE_COLLECTION },
            { request: "GetSceneItemList", name: undefined },
        ]);
    });

    it("second-obs のシーンコレクション切り替えの 3 手順は judge-connection-held 用の track/trackField を持つ (obsws-request は失敗を握りつぶすので、切り替わったこと自体を別に確認する)", () => {
        const r = planSteps(argsFor("second-obs"));
        const tracked = r.steps.filter((s) => s.kind === "obsws-request" && s.track === "scene-collection-switch").map((s) => ({ request: s.request, trackField: s.trackField }));
        expect(tracked).toEqual([
            { request: "SetCurrentSceneCollection", trackField: "switchAwayOk" },
            { request: "GetSceneCollectionList", trackField: "duringSwitchCollectionName" },
            { request: "SetCurrentSceneCollection", trackField: "switchBackOk" },
        ]);
    });

    it("second-obs: harness-start < wait-connection < switch-away < 切り替え中確認 < switch-back < record-start < record-stop < judge-connection-held < harness-wait の順を守る", () => {
        const r = planSteps(argsFor("second-obs"));
        const idx = (kind: string) => r.steps.findIndex((s) => s.kind === kind);
        const harnessStartIdx = idx("harness-start");
        const waitConnectionIdx = idx("wait-connection");
        const switchAwayIdx = r.steps.findIndex((s) => s.kind === "obsws-request" && s.trackField === "switchAwayOk");
        const duringSwitchIdx = r.steps.findIndex((s) => s.kind === "obsws-request" && s.trackField === "duringSwitchCollectionName");
        const switchBackIdx = r.steps.findIndex((s) => s.kind === "obsws-request" && s.trackField === "switchBackOk");
        const recordStartIdx = idx("record-start");
        const recordStopIdx = idx("record-stop");
        const judgeConnectionHeldIdx = idx("judge-connection-held");
        const harnessWaitIdx = idx("harness-wait");
        expect(harnessStartIdx).toBeLessThan(waitConnectionIdx);
        expect(waitConnectionIdx).toBeLessThan(switchAwayIdx);
        expect(switchAwayIdx).toBeLessThan(duringSwitchIdx);
        expect(duringSwitchIdx).toBeLessThan(switchBackIdx);
        expect(switchBackIdx).toBeLessThan(recordStartIdx);
        expect(recordStartIdx).toBeLessThan(recordStopIdx);
        // judge-connection-held は harness-wait より前 (harness 自身が終わるときの close が
        // ログに混ざる前に、切り替え前後だけを比べるため)。
        expect(recordStopIdx).toBeLessThan(judgeConnectionHeldIdx);
        expect(judgeConnectionHeldIdx).toBeLessThan(harnessWaitIdx);
    });

    it("discord-first は harness-start -> wait -> launch-obs の順で、5 秒 (DISCORD_FIRST_OBS_DELAY_MS) 待つ", () => {
        const r = planSteps(argsFor("discord-first"));
        const kinds = r.steps.map((s) => s.kind);
        expect(kinds[0]).toBe("seed-config");
        const harnessStartIdx = kinds.indexOf("harness-start");
        const waitIdx = kinds.indexOf("wait");
        const launchObsIdx = kinds.indexOf("launch-obs");
        expect(harnessStartIdx).toBeGreaterThanOrEqual(0);
        expect(harnessStartIdx).toBeLessThan(waitIdx);
        expect(waitIdx).toBeLessThan(launchObsIdx);
        expect(r.steps[waitIdx]).toMatchObject({ ms: DISCORD_FIRST_OBS_DELAY_MS });
        expect(kinds).toContain("harness-wait");
        expect(kinds.filter((k) => k === "judge").length).toBeGreaterThan(0);
    });

    it("dock-view は seed-config -> 配置 3 件 -> launch-obs -> obsws-connect -> wait -> screenshot-window -> close-obs -> write-result -> summarize -> cleanup の順で並ぶ (ドックはタブを持たない 1 面なので切り替えがない)", () => {
        const r = planSteps(argsFor("dock-view"));
        const kinds = r.steps.map((s) => s.kind);
        expect(kinds).toEqual([
            "seed-config",
            "copy-file",
            "copy-dir-contents",
            "copy-dir",
            "launch-obs",
            "obsws-connect",
            "wait",
            "screenshot-window",
            "close-obs",
            "write-result",
            "summarize",
            "cleanup",
        ]);
    });

    it("dock-view の seed-config は --dock-install (既定 elsewhere) を持つ", () => {
        const withDefault = planSteps(argsFor("dock-view"));
        expect(withDefault.steps[0]).toMatchObject({ kind: "seed-config", dockInstall: "elsewhere" });

        for (const v of DOCK_INSTALL_VALUES) {
            const r = planSteps(argsFor("dock-view", { dockInstall: v }));
            expect(r.steps[0]).toMatchObject({ kind: "seed-config", dockInstall: v });
        }
    });

    it("seed-config を持つすべての --step で dockInstall が渡る (dock-view だけでなく全 --step の APPDATA 差し替えを見るため)", () => {
        for (const stepName of STEP_VALUES.filter((s) => !STEPS_WITHOUT_SEED.includes(s))) {
            const r = planSteps(argsFor(stepName, { dockInstall: "ours" }));
            expect(r.steps[0]).toMatchObject({ kind: "seed-config", dockInstall: "ours" });
        }
    });

    // **record だけは除く**: record の実行計画が「--assign-mode auto を渡す」と定めており、
    // 呼び出し行の指定より実行計画を優先する (手動の既定のままだと共有を始めただけでは誰も枠に
    // 入らず、OBS 側の録画が黒のままになって対の判定が成り立たない)。record の側の検査は
    // 「--step record の実行計画」の describe にある。
    it("seed-config を持つすべての --step で assignMode が渡り、既定は manual", () => {
        // record と call-1 は実行計画が auto を決める (呼び出し行より優先)。
        // call-hold も auto (こちらはドックを押さない)。
        for (const stepName of STEP_VALUES.filter((s) => !STEPS_WITHOUT_SEED.includes(s) && s !== "record" && s !== "call-1" && s !== "call-hold")) {
            const withDefault = planSteps(argsFor(stepName));
            expect(withDefault.steps[0]).toMatchObject({ kind: "seed-config", assignMode: "manual" });
            for (const v of DOCK_ASSIGN_MODE_VALUES) {
                const r = planSteps(argsFor(stepName, { dockAssignMode: v }));
                expect(r.steps[0]).toMatchObject({ kind: "seed-config", assignMode: v });
            }
        }
    });

    it("mic-check は OBS を起動せず、mic-check 手順 1 つと共通の終了処理 (write-result -> summarize -> cleanup) だけを含む", () => {
        const r = planSteps(argsFor("mic-check"));
        expect(r.steps.map((s) => s.kind)).toEqual(["mic-check", "write-result", "summarize", "cleanup"]);
        // OBS を起動する手順 (seed-config / 配置 / launch-obs / obsws-connect) を 1 つも含まない。
        const obsRelatedKinds = ["seed-config", "copy-file", "copy-dir-contents", "copy-dir", "launch-obs", "obsws-connect"];
        for (const kind of obsRelatedKinds) {
            expect(r.steps.map((s) => s.kind)).not.toContain(kind);
        }
    });

    it("dock-view の wait は 5 秒", () => {
        const r = planSteps(argsFor("dock-view"));
        const wait = r.steps.find((s) => s.kind === "wait");
        expect(wait).toMatchObject({ ms: 5000 });
    });

    it("dock-view の screenshot-window は 1 つ目の OBS を対象に 1 回撮り、合成のソースを置かない既定の回は screenshot-source を持たない", () => {
        const r = planSteps(argsFor("dock-view"));
        const shotWindows = r.steps.filter((s) => s.kind === "screenshot-window");
        expect(shotWindows).toHaveLength(1);
        expect(shotWindows[0]).toMatchObject({ target: "first", shotLabel: "dock" });
        expect(r.steps.some((s) => s.kind === "dock-tab")).toBe(false);
        expect(r.steps.some((s) => s.kind === "screenshot-source")).toBe(false);
        // 合成のソースを置く connected は、合成のソースを型 id だけで名指して撮る (名前は人が載っても変わらないが、枠の番号は持たない)。
        const connected = planSteps(argsFor("dock-view", { dockScenario: "connected" }));
        const shotSource = connected.steps.find((s) => s.kind === "screenshot-source");
        expect(shotSource).toMatchObject({ source: { kind: "composite" } });
        expect(shotSource.sourceName).toBeUndefined();
    });

    describe("dock-view の --dock-scenario", () => {
        it("--dock-scenario 未指定は none と同じ実行計画になる (既定 = 後方互換)", () => {
            const withDefault = planSteps(argsFor("dock-view"));
            const withNone = planSteps(argsFor("dock-view", { dockScenario: "none" }));
            expect(withDefault).toEqual(withNone);
        });

        it("no-scene: harness には繋ぐが Main へは合成のソースを置かない (wire-composite がない。connected との対比が「配線したか」だけになる)", () => {
            const r = planSteps(argsFor("dock-view", { dockScenario: "no-scene" }));
            const kinds = r.steps.map((s) => s.kind);
            const idx = (kind: string) => kinds.indexOf(kind);

            const harnessStart = r.steps.find((s) => s.kind === "harness-start");
            expect(harnessStart).toMatchObject({ mode: "product", n: 1, syncTone: DEFAULT_SYNC_TONE });
            expect(harnessStart.extra).toBeUndefined();

            expect(idx("harness-start")).toBeLessThan(idx("wait-connection"));
            const postConnectWait = r.steps[idx("wait-connection") + 1];
            expect(postConnectWait).toMatchObject({ kind: "wait", ms: 10000 });
            expect(idx("wait-connection")).toBeLessThan(idx("screenshot-window"));

            expect(kinds).not.toContain("wire-composite");
            expect(kinds).not.toContain("record-start");
            expect(kinds).not.toContain("judge");
            expect(kinds).toContain("screenshot-window");
            expect(kinds).not.toContain("screenshot-source");
            expect(kinds).toContain("collect-plugin-log");
            expect(kinds).toContain("harness-wait");
        });

        it("connected: harness-start (failStage なし) -> wait-connection -> wire-composite -> 10 秒待つ -> 撮影 -> 10 秒録画+judge の順 (wire-composite が no-scene との対比点)", () => {
            const r = planSteps(argsFor("dock-view", { dockScenario: "connected" }));
            const kinds = r.steps.map((s) => s.kind);
            const idx = (kind: string) => kinds.indexOf(kind);

            const harnessStart = r.steps.find((s) => s.kind === "harness-start");
            expect(harnessStart).toMatchObject({ mode: "product", n: 1, syncTone: DEFAULT_SYNC_TONE });
            expect(harnessStart.extra).toBeUndefined();

            expect(idx("harness-start")).toBeLessThan(idx("wait-connection"));
            expect(idx("wait-connection")).toBeLessThan(idx("wire-composite"));
            const postWireWait = r.steps[idx("wire-composite") + 1];
            expect(postWireWait).toMatchObject({ kind: "wait", ms: 10000 });
            expect(idx("wire-composite")).toBeLessThan(idx("screenshot-window"));
            expect(idx("screenshot-window")).toBeLessThan(idx("screenshot-source"));
            expect(idx("screenshot-source")).toBeLessThan(idx("record-start"));
            expect(idx("record-start")).toBeLessThan(idx("record-stop"));
            expect(idx("record-stop")).toBeLessThan(idx("judge"));
            expect(idx("judge")).toBeLessThan(idx("harness-wait"));

            const judge = r.steps.find((s) => s.kind === "judge");
            expect(judge).toMatchObject({ tool: "frames.py", label: "ac24_connected_baseline" });

            const recordWait = r.steps[idx("record-start") + 1];
            expect(recordWait).toMatchObject({ kind: "wait", ms: 10000 });
        });

        it("degraded: harness-start に failStage=3 (数値) が付き、judge の label が connected と異なる (突き合わせ用)。no-scene とは wire-composite の有無で分かれる", () => {
            const noScene = planSteps(argsFor("dock-view", { dockScenario: "no-scene" }));
            const connected = planSteps(argsFor("dock-view", { dockScenario: "connected" }));
            const degraded = planSteps(argsFor("dock-view", { dockScenario: "degraded" }));

            const degradedHarness = degraded.steps.find((s) => s.kind === "harness-start");
            expect(degradedHarness.extra).toMatchObject({ failStage: 3 });
            expect(typeof degradedHarness.extra.failStage).toBe("number");

            const connectedJudge = connected.steps.find((s) => s.kind === "judge");
            const degradedJudge = degraded.steps.find((s) => s.kind === "judge");
            expect(degradedJudge.label).toBe("ac24_degraded");
            expect(degradedJudge.label).not.toBe(connectedJudge.label);
            // degraded と connected は judge の形 (tool/呼び方) 以外は同じ並びを踏む
            // (harness-start.extra / harness-wait.copyEncodeFacts の中身は異なるが、
            // kind の並びそのものは変わらない)。
            expect(degraded.steps.map((s) => s.kind)).toEqual(connected.steps.map((s) => s.kind));
            // no-scene は wire-composite / record-start / judge を持たない点だけが connected と違う。
            expect(noScene.steps.map((s) => s.kind)).not.toContain("wire-composite");
            expect(connected.steps.map((s) => s.kind)).toContain("wire-composite");
        });

        it("degraded: harness-start は既定で maxLongEdge=DEFAULT_DOCK_MAX_LONG_EDGE (640) を extra に持ち、--dock-max-long-edge で変えられる。connected は maxLongEdge を持たない", () => {
            const withDefault = planSteps(argsFor("dock-view", { dockScenario: "degraded" }));
            const degradedHarness = withDefault.steps.find((s) => s.kind === "harness-start");
            expect(DEFAULT_DOCK_MAX_LONG_EDGE).toBe(640);
            expect(degradedHarness.extra).toMatchObject({ failStage: 3, maxLongEdge: DEFAULT_DOCK_MAX_LONG_EDGE });

            const overridden = planSteps(argsFor("dock-view", { dockScenario: "degraded", dockMaxLongEdge: 320 }));
            const overriddenHarness = overridden.steps.find((s) => s.kind === "harness-start");
            expect(overriddenHarness.extra).toMatchObject({ failStage: 3, maxLongEdge: 320 });

            const connected = planSteps(argsFor("dock-view", { dockScenario: "connected", dockMaxLongEdge: 320 }));
            const connectedHarness = connected.steps.find((s) => s.kind === "harness-start");
            expect(connectedHarness.extra).toBeUndefined();
        });

        it("degraded: harness-wait だけが copyEncodeFacts:true を持つ (結果 JSON へコピーする指示)。connected は持たない", () => {
            const degraded = planSteps(argsFor("dock-view", { dockScenario: "degraded" }));
            const degradedWait = degraded.steps.find((s) => s.kind === "harness-wait");
            expect(degradedWait.copyEncodeFacts).toBe(true);

            const connected = planSteps(argsFor("dock-view", { dockScenario: "connected" }));
            const connectedWait = connected.steps.find((s) => s.kind === "harness-wait");
            expect(connectedWait.copyEncodeFacts).toBeUndefined();

            for (const scenario of ["no-scene", "contact-missing", "idle-10min"]) {
                const r = planSteps(argsFor("dock-view", { dockScenario: scenario }));
                const wait = r.steps.find((s) => s.kind === "harness-wait");
                expect(wait.copyEncodeFacts).toBeUndefined();
            }
        });

        it("connected / degraded の送信側は、録画が終わるまでの最悪の見積もりの 2 倍以上の長さで動く", () => {
            // 送信側が record-stop より先に自分で止まると、frames.py の欠落が跳ね上がって
            // 「警告が出ている間も出口に差分がない」が判定できず、実行を 1 回丸ごと
            // 無駄にする。見積もりは固定値を持たないので、実行計画の中身から数え直して余裕を見る。
            const SCREENSHOT_WORST_MS = 8000; // 1 回あたり 3 から 8 秒 (README の見積もりの上限)
            for (const scenario of ["connected", "degraded"]) {
                const r = planSteps(argsFor("dock-view", { dockScenario: scenario }));
                const kinds = r.steps.map((s) => s.kind);
                const untilRecordStop = r.steps.slice(0, kinds.indexOf("record-stop") + 1);
                expect(untilRecordStop.length).toBeGreaterThan(0);

                const connectTimeoutMs = r.steps.find((s) => s.kind === "wait-connection").timeoutMs;
                const waitMs = untilRecordStop
                    .filter((s) => s.kind === "wait")
                    .reduce((sum, s) => sum + s.ms, 0);
                // dock-tab も screenshot-window/-source と同じ「子プロセス起動 + Add-Type の
                // C# コンパイルを毎回挟む」重さを払うので、同じ見積もりに含める。
                const shotMs =
                    untilRecordStop.filter((s) => s.kind === "screenshot-window" || s.kind === "screenshot-source" || s.kind === "dock-tab")
                        .length * SCREENSHOT_WORST_MS;
                const worstMs = connectTimeoutMs + waitMs + shotMs;

                const harnessStart = r.steps.find((s) => s.kind === "harness-start");
                expect(harnessStart.seconds * 1000).toBeGreaterThanOrEqual(worstMs * 2);
                // harness-wait も同じ長さを待つ (送信側の終了を取りこぼさない)。
                const harnessWait = r.steps.find((s) => s.kind === "harness-wait");
                expect(harnessWait.seconds).toBe(harnessStart.seconds);
            }
        });

        it("contact-missing: harness-start が HARNESS_FAIL_CONTACT 用の extra.failContact を持ち、録画は行わない", () => {
            const r = planSteps(argsFor("dock-view", { dockScenario: "contact-missing" }));
            const kinds = r.steps.map((s) => s.kind);
            const idx = (kind: string) => kinds.indexOf(kind);

            const harnessStart = r.steps.find((s) => s.kind === "harness-start");
            expect(harnessStart).toMatchObject({ mode: "product", n: 1, syncTone: DEFAULT_SYNC_TONE, extra: { failContact: DOCK_VIEW_FAIL_CONTACT_ID } });
            expect(DOCK_VIEW_FAIL_CONTACT_ID).toBe("roster");

            expect(idx("harness-start")).toBeLessThan(idx("wait-connection"));
            const postConnectWait = r.steps[idx("wait-connection") + 1];
            expect(postConnectWait).toMatchObject({ kind: "wait", ms: 10000 });
            expect(idx("wait-connection")).toBeLessThan(idx("screenshot-window"));

            expect(kinds).not.toContain("wire-composite");
            expect(kinds).not.toContain("record-start");
            expect(kinds).not.toContain("judge");
            expect(kinds).toContain("collect-plugin-log");
            expect(kinds).toContain("harness-wait");
        });

        it("idle-10min: 1 分待つ+撮る を 10 回繰り返し、最後にプラグインのログを回収する", () => {
            const r = planSteps(argsFor("dock-view", { dockScenario: "idle-10min" }));
            const kinds = r.steps.map((s) => s.kind);
            expect(DOCK_VIEW_IDLE_INTERVALS).toBe(10);
            expect(DOCK_VIEW_IDLE_INTERVAL_MS).toBe(60000);

            const waits = r.steps.filter((s) => s.kind === "wait");
            expect(waits).toHaveLength(DOCK_VIEW_IDLE_INTERVALS);
            for (const w of waits) expect(w.ms).toBe(DOCK_VIEW_IDLE_INTERVAL_MS);

            const shotWindows = r.steps.filter((s) => s.kind === "screenshot-window");
            expect(shotWindows).toHaveLength(DOCK_VIEW_IDLE_INTERVALS);

            // 1 分待つ・撮る が交互に 10 回続く (先頭は wait-connection の直後)。
            const startIdx = kinds.indexOf("wait-connection") + 1;
            for (let i = 0; i < DOCK_VIEW_IDLE_INTERVALS; i += 1) {
                expect(kinds[startIdx + i * 2]).toBe("wait");
                expect(kinds[startIdx + i * 2 + 1]).toBe("screenshot-window");
            }

            const idx = (kind: string) => kinds.indexOf(kind);
            const lastShotWindowIdx = kinds.lastIndexOf("screenshot-window");
            expect(kinds).not.toContain("screenshot-source");
            expect(lastShotWindowIdx).toBeLessThan(idx("collect-plugin-log"));
            expect(idx("collect-plugin-log")).toBeLessThan(idx("harness-wait"));

            const harnessStart = r.steps.find((s) => s.kind === "harness-start");
            const harnessWait = r.steps.find((s) => s.kind === "harness-wait");
            expect(harnessStart.seconds).toBeGreaterThanOrEqual((DOCK_VIEW_IDLE_INTERVALS * DOCK_VIEW_IDLE_INTERVAL_MS) / 1000);
            expect(harnessWait.seconds).toBe(harnessStart.seconds);
        });

        it("idle-10min は dock-tab を挟まない (「10 分間、同じに見えること」の確認に、毎分のタブ切り替えを混ぜない)", () => {
            const r = planSteps(argsFor("dock-view", { dockScenario: "idle-10min" }));
            expect(r.steps.map((s) => s.kind)).not.toContain("dock-tab");
        });

        it("id は idle-10min のように同じ kind が何度も出る scenario でもすべて一意 (10 回の wait/screenshot-window を含む)", () => {
            const r = planSteps(argsFor("dock-view", { dockScenario: "idle-10min" }));
            const ids = r.steps.map((s) => s.id);
            expect(new Set(ids).size).toBe(ids.length);
            expect(r.steps.filter((s) => s.kind === "wait").map((s) => s.id)).toEqual(Array.from({ length: DOCK_VIEW_IDLE_INTERVALS }, (_, i) => `wait-${i + 1}`));
        });

        it("9 の scenario すべてで、先頭は seed-config、末尾 3 つは write-result/summarize/cleanup (他の step と同じ形を踏襲)", () => {
            expect(DOCK_SCENARIO_VALUES).toHaveLength(9);
            for (const scenario of DOCK_SCENARIO_VALUES) {
                const r = planSteps(argsFor("dock-view", { dockScenario: scenario }));
                expect(r.ok).toBe(true);
                expect(r.steps[0].kind).toBe("seed-config");
                expect(r.steps.at(-1).kind).toBe("cleanup");
                expect(r.steps.at(-2).kind).toBe("summarize");
                expect(r.steps.at(-3).kind).toBe("write-result");
                expect(r.steps.at(-4).kind).toBe("close-obs");
            }
        });

        // 「obs-websocket を使わず、ドックの操作だけで載せる人を選べて 4 分割が出る」ことの確認と、
        // 自動配置・ダミー・テストデータの zip の確認を、実行スクリプトが人の手なしで行う 3 つの scenario。
        describe("select-four (載せる人の選択と 4 分割の確認、自動配置の確認)", () => {
            it("harness-start は画面 4 ストリーム + 声 0 ストリーム (SELECT_FOUR_N_SCREEN/N_VOICE) を product モードで繋ぎ、syncTone を明示する", () => {
                expect(SELECT_FOUR_N_SCREEN).toBe(4);
                expect(SELECT_FOUR_N_VOICE).toBe(0);
                const r = planSteps(argsFor("dock-view", { dockScenario: "select-four" }));
                const harnessStart = r.steps.find((s) => s.kind === "harness-start");
                expect(harnessStart).toMatchObject({ mode: "product", nScreen: 4, nVoice: 0, syncTone: DEFAULT_SYNC_TONE });
            });

            it("接続 -> 5 秒待って押す前を撮る -> 4 人の行のチェックを名前で付ける -> 5 秒待つ -> 撮る -> 合成のソースの絵 -> 配置の行 -> judge-composite-placement の順で並ぶ", () => {
                const r = planSteps(argsFor("dock-view", { dockScenario: "select-four" }));
                const kinds = r.steps.map((s) => s.kind);
                const idx = (kind: string) => kinds.indexOf(kind);

                expect(idx("harness-start")).toBeLessThan(idx("wait-connection"));
                expect(kinds).not.toContain("dock-tab");
                expect(kinds).not.toContain("dock-click-at");

                // 誰を載せるかは手動が既定なので、4 ストリーム繋いでも枠には 1 人も入っていない。
                // 行のチェックを人の見えている名前 (テスト用の送信側の userId) で名指して付ける (行の並びに頼らない)。
                const checks = r.steps.filter((s) => s.kind === "dock-click");
                expect(checks.map((c) => c.name)).toEqual(["harness-screen-1", "harness-screen-2", "harness-screen-3", "harness-screen-4"]);
                for (const c of checks) expect(c).toMatchObject({ target: "first", desired: "on", slotOp: true });
                expect(idx("wait-connection")).toBeLessThan(kinds.indexOf("dock-click"));

                // 待ちは 2 か所とも 5 秒 (一覧に人が並ぶまでと、自動配置の周期が回るまで)。
                const waits = r.steps.filter((s) => s.kind === "wait");
                expect(waits).toHaveLength(2);
                for (const w of waits) expect(w).toMatchObject({ ms: 5000 });
                expect(r.steps[r.steps.indexOf(checks[3]) + 1]).toMatchObject({ kind: "wait", ms: 5000 });

                const shotWindows = r.steps.filter((s) => s.kind === "screenshot-window");
                expect(shotWindows.map((w) => w.shotLabel)).toEqual(["before-select", "after-select"]);
                // 押す前の 1 枚は、最初のチェックより前に撮る (手動の既定では繋いでも枠が空であることの証拠)。
                expect(r.steps.indexOf(shotWindows[0])).toBeLessThan(r.steps.indexOf(checks[0]));

                const shotSources = r.steps.filter((s) => s.kind === "screenshot-source");
                expect(shotSources.map((s) => s.source)).toEqual([{ kind: "composite" }]);

                // 合成のソースは接続の後・行のチェックの前に置き、配置の行を読んで 4 等分の判定に渡す。
                expect(idx("wait-connection")).toBeLessThan(idx("wire-composite"));
                expect(idx("wire-composite")).toBeLessThan(kinds.indexOf("dock-click"));
                const snapshot = r.steps.find((s) => s.kind === "composite-layout-snapshot");
                expect(snapshot).toMatchObject({ label: "select-four" });
                expect(idx("screenshot-source")).toBeLessThan(idx("composite-layout-snapshot"));
                expect(r.steps.find((s) => s.kind === "judge-composite-placement")).toMatchObject({ label: "select-four", count: 4 });
                expect(idx("composite-layout-snapshot")).toBeLessThan(idx("judge-composite-placement"));
                expect(idx("judge-composite-placement")).toBeLessThan(idx("harness-wait"));
                expect(kinds).not.toContain("judge-auto-layout");
            });
        });

        describe("dummy (4-B の確認: ダミー表示)", () => {
            it("harness には繋がない (繋がずに設定タブへ進む)", () => {
                const r = planSteps(argsFor("dock-view", { dockScenario: "dummy" }));
                const kinds = r.steps.map((s) => s.kind);
                expect(kinds).not.toContain("harness-start");
                expect(kinds).not.toContain("harness-wait");
                expect(kinds).not.toContain("wait-connection");
            });

            it("合成のソースを置く -> ドックの [表示] タブ -> 「ダミー映像を表示」を入れる -> 待つ -> screenshot-source(合成のソース) -> judge-dummy-tile の順 (plan 1.5 の 3)", () => {
                const r = planSteps(argsFor("dock-view", { dockScenario: "dummy" }));
                const kinds = r.steps.map((s) => s.kind);
                const idx = (kind: string) => kinds.indexOf(kind);
                expect(kinds).not.toContain("dock-tab");

                const clicks = r.steps.filter((s) => s.kind === "dialog-click");
                expect(clicks.map((c) => [c.titleTextId ?? (c.popup ? "popup" : null), c.controlType, c.buttonTextId])).toEqual([
                    [TEXT_ID_DOCK_WINDOW_TITLE, "TabItem", TEXT_ID_DOCK_TAB_DISPLAY],
                    [TEXT_ID_DOCK_WINDOW_TITLE, "CheckBox", TEXT_ID_LOOK_SHOW_PLACEHOLDERS],
                ]);
                // ダミーは押すとすぐ動作する ([OK] はない)。チェックは行き先 on。
                expect(clicks[1]).toMatchObject({ desired: "on" });

                // ダミーを入れる前に Main へ配線する (配線しないと OBS が枠を 1 度も描かず、
                // 非同期映像のフレームが GPU のテクスチャへ載らないまま透明の PNG になる)。
                const wireComposite = r.steps.find((s) => s.kind === "wire-composite");
                expect(wireComposite).toMatchObject({ inputName: "cordmux" });
                expect(idx("wire-composite")).toBeLessThan(idx("dialog-click"));

                const placeholders = clicks[1];
                expect(r.steps[r.steps.indexOf(placeholders) + 2]).toMatchObject({ kind: "wait", ms: 15000 });

                const shotSource = r.steps.find((s) => s.kind === "screenshot-source");
                expect(shotSource).toMatchObject({ source: { kind: "composite" } });
                expect(r.steps.indexOf(placeholders)).toBeLessThan(idx("screenshot-source"));
                expect(idx("screenshot-source")).toBeLessThan(idx("judge-dummy-tile"));
                expect(idx("judge-dummy-tile")).toBeLessThan(idx("collect-plugin-log"));
            });
        });

        describe("export-bundle (テストデータの zip)", () => {
            it("harness には繋がない", () => {
                const r = planSteps(argsFor("dock-view", { dockScenario: "export-bundle" }));
                const kinds = r.steps.map((s) => s.kind);
                expect(kinds).not.toContain("harness-start");
                expect(kinds).not.toContain("harness-wait");
            });

            it("ドックのメニュー -> [診断情報をエクスポート...] -> 保存のウィンドウへ dialog-sendkeys -> 5 秒待つ -> check-bundle-zip の順", () => {
                const r = planSteps(argsFor("dock-view", { dockScenario: "export-bundle" }));
                const kinds = r.steps.map((s) => s.kind);
                const idx = (kind: string) => kinds.indexOf(kind);

                const clicks = r.steps.filter((s) => s.kind === "dialog-click");
                expect(clicks.map((c) => c.buttonTextId)).toEqual([TEXT_ID_DOCK_MENU_BUTTON, TEXT_ID_MENU_EXPORT_DIAGNOSTICS]);
                // 項目を押すと保存のウィンドウ (モーダル) が開くので、Invoke の戻りを待たない。
                expect(clicks[1]).toMatchObject({ popup: true, controlType: "MenuItem", noWait: true });

                const dialogSendkeys = r.steps.find((s) => s.kind === "dialog-sendkeys");
                expect(dialogSendkeys).toMatchObject({ target: "first", titleTextId: TEXT_ID_DIALOG_EXPORT_TITLE, fileNameRel: EXPORT_BUNDLE_FILE_NAME });
                expect(dialogSendkeys.title).toBeUndefined();
                expect(EXPORT_BUNDLE_FILE_NAME.endsWith(".zip")).toBe(true);

                const wait = r.steps[idx("dialog-sendkeys") + 1];
                expect(wait).toMatchObject({ kind: "wait", ms: 5000 });

                expect(idx("dialog-click")).toBeLessThan(idx("dialog-sendkeys"));
                expect(idx("dialog-sendkeys")).toBeLessThan(idx("check-bundle-zip"));
                expect(idx("check-bundle-zip")).toBeLessThan(idx("collect-plugin-log"));
            });
        });
    });

    it("同じ step + 同じ引数なら、何度呼んでも同じ実行計画になる (副作用を持たない)", () => {
        const a = planSteps(argsFor("integration"));
        const b = planSteps(argsFor("integration"));
        expect(a).toEqual(b);

        const c = planSteps(argsFor("second-obs"));
        const d = planSteps(argsFor("second-obs"));
        expect(c).toEqual(d);

        const e = planSteps(argsFor("call-1"));
        const f = planSteps(argsFor("call-1"));
        expect(e).toEqual(f);
    });

    // ---- 開始ゲート ----------------------------

    describe("開始ゲート (call-gate-a / -b / -c / -d)", () => {
        it("4 つのゲートはどれも「失敗を返す手順」(gate:true) を少なくとも 1 つ持つ (自分の終了コードで失敗を返す)", () => {
            for (const stepName of ["call-gate-a", "call-gate-b", "call-gate-c", "call-gate-d"]) {
                const r = planSteps(argsFor(stepName));
                expect(r.steps.filter((s) => s.gate === true).length).toBeGreaterThan(0);
            }
        });

        it("call-gate-a: コピーを取る -> テストを実行する人が [Vesktop に追加] を押すのを待つ (human) -> 読む -> 判定、の順", () => {
            const r = planSteps(argsFor("call-gate-a"));
            const kinds = r.steps.map((s) => s.kind);
            const idx = (kind: string) => kinds.indexOf(kind);
            expect(idx("copy-state-json")).toBeGreaterThan(idx("obsws-connect"));
            expect(idx("copy-state-json")).toBeLessThan(idx("human"));
            expect(idx("human")).toBeLessThan(idx("read-state-json"));
            expect(idx("read-state-json")).toBeLessThan(idx("judge-vencord-dir"));

            const copy = r.steps.find((s) => s.kind === "copy-state-json");
            expect(copy).toMatchObject({ appDataRel: VESKTOP_STATE_JSON_REAL_REL, destName: STATE_JSON_BEFORE_NAME });

            // **実行スクリプトは [Vesktop に追加] を押さない** (Vesktop の設定を書き換えるボタンは利用者が押す)。
            expect(r.steps.filter((s) => s.kind === "dock-click" || s.kind === "dialog-click")).toHaveLength(0);
            const human = r.steps.find((s) => s.kind === "human");
            expect(human).toMatchObject({ until: "state-json-points-to-placed", timeoutMs: HUMAN_TIMEOUT_MS, gate: true, appDataRel: VESKTOP_STATE_JSON_REAL_REL });
            expect(human.expectedDir).toBe("C:/obs/" + BUNDLED_VENCORD_REL);
            // ボタンのテキストは ini から引く textRef で持つ。別の場所を指していれば押した後に切り替えの確認のダイアログが出る。
            expect(human.message).toContain(textRef(TEXT_ID_ADD_TO_VESKTOP));
            expect(human.message).toContain(textRef(TEXT_ID_DIALOG_SWITCH_BUTTON));

            // 押した直後の判定は「他の鍵が 1 文字も変わっていない」まで見る。
            const judge = r.steps.find((s) => s.kind === "judge-vencord-dir");
            expect(judge).toMatchObject({ compareOtherKeys: true, gate: true });
            expect(judge.expectedDir).toBe("C:/obs/" + BUNDLED_VENCORD_REL);
        });

        it("call-gate-a の seed-config は --appdata を渡さず、allow_state_json_write を立て、4 ファイルを置く ours にする", () => {
            const r = planSteps(argsFor("call-gate-a"));
            expect(r.steps[0]).toMatchObject({ kind: "seed-config", useAppData: false, allowStateJsonWrite: true, decoder: "software", dockInstall: "ours", rehearsal: false });
        });

        it("call-gate-b は OBS を起動せず、読み直して同じ判定をする (他の鍵の比較はしない = Vesktop が終了時に書き直すため)", () => {
            const r = planSteps(argsFor("call-gate-b"));
            expect(r.steps.map((s) => s.kind)).toEqual(["read-state-json", "judge-vencord-dir", "write-result", "summarize", "cleanup"]);
            expect(r.steps.find((s) => s.kind === "judge-vencord-dir")).toMatchObject({ compareOtherKeys: false, gate: true });
        });

        it("call-gate-c は seed-config も配置もやり直さず、接続待ちと段階表示の PNG を持つ", () => {
            const r = planSteps(argsFor("call-gate-c"));
            const kinds = r.steps.map((s) => s.kind);
            expect(kinds[0]).toBe("launch-obs");
            expect(kinds).not.toContain("seed-config");
            expect(kinds).not.toContain("copy-file");
            const wait = r.steps.find((s) => s.kind === "wait-connection");
            expect(wait).toMatchObject({ gate: true });
            expect(wait.timeoutMs).toBe(CALL_GATE_CONNECT_WAIT_TIMEOUT_MS);
            expect(r.steps.filter((s) => s.kind === "screenshot-window").length).toBeGreaterThanOrEqual(1);
        });

        it("call-gate-d は package.json を退かしてから起動し、観測の直後に戻す (失敗は「退かせなかった」と「戻せなかった」の 2 つ)", () => {
            const r = planSteps(argsFor("call-gate-d"));
            const kinds = r.steps.map((s) => s.kind);
            const idx = (kind: string) => kinds.indexOf(kind);
            expect(idx("move-file")).toBe(0);
            expect(idx("move-file")).toBeLessThan(idx("launch-obs"));
            expect(idx("wait-connection")).toBeLessThan(idx("restore-file"));
            expect(idx("restore-file")).toBeLessThan(idx("close-obs"));

            const move = r.steps.find((s) => s.kind === "move-file");
            expect(move.path).toBe("C:/obs/" + BUNDLED_VENCORD_PACKAGE_JSON_REL);
            expect(move.restoreId).toBe(r.steps.find((s) => s.kind === "restore-file").restoreId);

            // 繋いでこないことは 想定した結果であって失敗ではない。
            expect(r.steps.find((s) => s.kind === "wait-connection").gate).toBeUndefined();
            expect(r.steps.find((s) => s.kind === "move-file")).toMatchObject({ gate: true });
            expect(r.steps.find((s) => s.kind === "restore-file")).toMatchObject({ gate: true });
        });

        it("call-gate-a はテストを実行する人に依頼する前にラベルが描かれる猶予を置く (押すラベルがない画面で依頼しない)", () => {
            const r = planSteps(argsFor("call-gate-a"));
            const humanIdx = r.steps.findIndex((s) => s.kind === "human");
            expect(r.steps[humanIdx - 1]).toMatchObject({ kind: "wait", ms: SELECT_FOUR_SETTLE_WAIT_MS });
        });
    });

    // ---- call-1 の本体と、短い項目・連続運転 ----------

    describe("call-1", () => {
        const withBroadcaster = () => {
            const r = parseArgs([...argvFor("call-1"), "--broadcaster-id", "1"]);
            expect(r.ok).toBe(true);
            return planSteps(r.args).steps;
        };
        const rehearsal = () => {
            const r = parseArgs(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-1", "--rehearsal", "--broadcaster-id", "1"]);
            expect(r.errors).toEqual([]);
            return planSteps(r.args).steps;
        };
        const at = (steps: Step[], pred: (s: Step) => boolean) => steps.findIndex(pred);

        it("合成のソースを置くのは record-start より前、枠の付け外しは record-start より後 (配信中の操作だけを数える)", () => {
            const r = planSteps(argsFor("call-1"));
            const recordStartIdx = r.steps.findIndex((s) => s.kind === "record-start");
            const buildSceneIdx = r.steps.findIndex((s) => s.kind === "wire-composite");
            // 枠への出し入れは自動の割り当てなので押下はない。行のチェックは付け外し (配信中の操作) だけ。
            const firstSlotClickIdx = r.steps.findIndex((s) => s.kind === "dock-click" && s.slotOp === true);
            expect(buildSceneIdx).toBeGreaterThanOrEqual(0);
            expect(buildSceneIdx).toBeLessThan(recordStartIdx);
            expect(firstSlotClickIdx).toBeGreaterThan(recordStartIdx);
        });

        it("枠の割り当ては自動 (seed に auto を渡し、枠へ入れる押下はない)。合成のソースを 1 つ置く", () => {
            const r = planSteps(argsFor("call-1"));
            const beforeRecord = r.steps.slice(0, r.steps.findIndex((s) => s.kind === "record-start"));
            // 行を座標で押す形は行の並びに依存して枠に届かないので、自動の割り当てにした。
            expect(beforeRecord.filter((s) => s.kind === "dock-click" && s.slotOp === true)).toHaveLength(0);
            expect(r.steps.find((s) => s.kind === "seed-config")).toMatchObject({ assignMode: "auto" });
            expect(r.steps.filter((s) => s.kind === "wire-composite")).toHaveLength(1);
            expect(r.steps.some((s) => s.kind === "wire-scene")).toBe(false);
        });

        it("--n-screen 3 --n-voice 3 で枠への押下・配線・判定の升目が 3 になる (4 アカウントの場合は自分以外の共有が 3 ストリーム)", () => {
            const r = parseArgs([...argvFor("call-1"), "--n-screen", "3", "--n-voice", "3", "--broadcaster-id", "1"]);
            expect(r.ok).toBe(true);
            const steps = planSteps(r.args).steps;
            // 外すと戻すの 2 回だけ行のチェックを押す (同じ人を名前で名指す。行の数に依らない)。
            const checks = steps.filter((s) => s.kind === "dock-click" && s.slotOp === true);
            expect(checks.map((c) => [c.name, c.desired])).toEqual([[DEFAULT_CALL_ACCOUNTS.firefox, "off"], [DEFAULT_CALL_ACCOUNTS.firefox, "on"]]);
            expect(steps.filter((s) => s.kind === "wire-composite")).toHaveLength(1);
            const cells = steps.filter((s) => s.kind === "judge" && (s.cells || s.ncells)).map((s) => s.cells || s.ncells);
            expect(cells.length).toBeGreaterThan(0);
            expect(new Set(cells)).toEqual(new Set([3]));
            // 渡さなければ 4 のまま (テスト用の送信側のリハーサルと同じ形)。record の既定 (声 8) は call-1 に持ち込まない。
            const d = planSteps(argsFor("call-1"));
            expect(d.steps.filter((s) => s.kind === "wire-composite")).toHaveLength(1);
        });

        it("配信の受け口 -> 録画と配信の開始 -> 15 秒 -> obs-stats -> --call-minutes 分 -> obs-stats -> 判定、の順。30/60/90 分の位置にも obs-stats", () => {
            const r = planSteps(argsFor("call-1", { callMinutes: 10 }));
            const kinds = r.steps.map((s) => s.kind);
            const idx = (kind: string) => kinds.indexOf(kind);
            expect(idx("stream-sink-start")).toBeLessThan(idx("record-start"));
            expect(r.steps.find((s) => s.kind === "record-start")).toMatchObject({ stream: true, label: "main" });

            const statsLabels = r.steps.filter((s) => s.kind === "obs-stats").map((s) => s.label);
            expect(statsLabels[0]).toBe("first");
            expect(statsLabels).toContain("last");
            for (const mark of CALL_STATS_MARK_MINUTES) expect(statsLabels.filter((l) => l === "t" + mark + "min")).toHaveLength(1);
            expect(statsLabels.indexOf("t30min")).toBeLessThan(statsLabels.indexOf("t60min"));
            expect(statsLabels.indexOf("t60min")).toBeLessThan(statsLabels.indexOf("t90min"));

            const firstStatsIdx = r.steps.findIndex((s) => s.kind === "obs-stats");
            expect(r.steps[firstStatsIdx - 1]).toMatchObject({ kind: "wait", ms: OBS_STATS_FIRST_WAIT_MS });
            const lastStatsIdx = r.steps.findIndex((s) => s.kind === "obs-stats" && s.label === "last");
            // 定常の待ちの直後 (30 分の位置の obs-stats が間に挟まりうる)。
            const steady = r.steps.slice(firstStatsIdx, lastStatsIdx).find((s) => s.kind === "wait" && s.ms === 10 * 60 * 1000);
            expect(steady).toBeTruthy();
            expect(r.steps.slice(firstStatsIdx, lastStatsIdx).filter((s) => !["wait", "obs-stats"].includes(s.kind))).toHaveLength(0);
            expect(r.steps[lastStatsIdx + 1]).toMatchObject({ kind: "judge-output-skipped", gate: true });
        });

        it("--call-minutes の既定は 20 分 (ソフト復号の実行)", () => {
            const r = planSteps(argsFor("call-1"));
            expect(r.steps.find((s) => s.kind === "wait" && s.ms === DEFAULT_CALL_MINUTES * 60 * 1000)).toBeTruthy();
        });

        it("付け外しの前後で配置の行と PNG を取る", () => {
            const r = planSteps(argsFor("call-1"));
            const labels = r.steps.filter((s) => s.kind === "composite-layout-snapshot" && typeof s.label === "string" && s.label.startsWith("ac30")).map((s) => s.label);
            expect(labels).toEqual(["ac30-before", "ac30-after-remove", "ac30-after-restore"]);
            const shots = r.steps.filter((s) => s.kind === "screenshot-window" && typeof s.shotLabel === "string" && s.shotLabel.startsWith("ac30"));
            expect(shots).toHaveLength(3);
            // 実際の実行では依頼文の名前の人 (sub3) を、リハーサルではテスト用の画面の 1 つ目を外して戻す。
            expect(r.steps.filter((s) => s.kind === "dock-click" && s.slotOp === true).map((s) => s.name)).toEqual([DEFAULT_CALL_ACCOUNTS.firefox, DEFAULT_CALL_ACCOUNTS.firefox]);
            expect(rehearsal().filter((s) => s.kind === "dock-click" && s.slotOp === true).map((s) => s.name)).toEqual(["harness-screen-1", "harness-screen-1"]);
        });

        it("声ごとにドックの行のミュートを往復し、区間は ac32 の録画の中にあり、押す操作はミュートの操作に数える", () => {
            const r = planSteps(argsFor("call-1"));
            const ac32StartIdx = r.steps.findIndex((s) => s.kind === "record-start" && s.label === "ac32");
            const ac32StopIdx = r.steps.findIndex((s) => s.kind === "record-stop" && s.label === "ac32");
            expect(ac32StartIdx).toBeGreaterThanOrEqual(0);
            expect(ac32StopIdx).toBeGreaterThan(ac32StartIdx);

            const inside = r.steps.slice(ac32StartIdx, ac32StopIdx);
            const clicks = inside.filter((s) => s.kind === "dock-click");
            expect(clicks.every((s) => s.muteOp === true && s.slotOp === undefined)).toBe(true);
            expect(clicks.filter((s) => s.desired === "on")).toHaveLength(callVoicePersons({}).length * 2);
            expect(clicks.filter((s) => s.desired === "off")).toHaveLength(callVoicePersons({}).length * 2);
            // 声の音量は操作するツールがないので、SetInput* の手順はもう出ない。
            expect(r.steps.some((s) => s.kind === "set-input-volume" || s.kind === "set-input-mute")).toBe(false);
        });

        it("count-operations は本体の直後 (ac32 の録画を止めた次) にあり、配信はそのまま続く (stream-stop は締め)", () => {
            const r = planSteps(argsFor("call-1"));
            const ac32StopIdx = r.steps.findIndex((s) => s.kind === "record-stop" && s.label === "ac32");
            // 録画を止めた後に、声のミュートを全員外す操作が続いてから数える (どれもミュートの操作)。
            const countIdx = ac32StopIdx + 1 + callVoicePersons({}).length;
            expect(r.steps.slice(ac32StopIdx + 1, countIdx).every((s) => s.kind === "dock-click" && s.muteOp === true)).toBe(true);
            expect(r.steps[countIdx]).toMatchObject({ kind: "count-operations", gate: true });
            const kinds = r.steps.map((s) => s.kind);
            expect(kinds.indexOf("stream-stop")).toBeGreaterThan(countIdx);
            expect(kinds.indexOf("stream-sink-stop")).toBeGreaterThan(kinds.indexOf("stream-stop"));
            expect(kinds.filter((k) => k === "stream-sink-start")).toHaveLength(1);
            // 本体の後の操作 (欄への入力・録画のラベル・わざと異常) は count-operations より後 = 数えない。
            for (const kind of ["tamper-file", "wait-finalize"]) expect(kinds.indexOf(kind) === -1 || kinds.indexOf(kind) > countIdx).toBe(true);
            // ウィンドウのコンポーネントを押す手順 (メニュー・設定ウィンドウ) も配信中の区間にはない (合成のソースの撮影は録画の前、別アカウント方式は数えた後)。
            // 行を押す前に [参加者] タブを開く手順だけはあるが、タブを選ぶだけで状態を変えないので、操作の数に数えない印 (readOnlyOp) を持つ。
            const recordStart = kinds.indexOf("record-start");
            const inWindow = r.steps.slice(recordStart, countIdx).filter((s) => s.kind === "dialog-click");
            expect(inWindow.map((s) => [s.controlType, s.buttonTextId, s.readOnlyOp])).toEqual([["TabItem", "cordmux.dock.tab_people.label", true]]);
            expect(r.steps.findIndex((s) => s.kind === "dock-click" && s.textId === TEXT_ID_RECORD_START)).toBeGreaterThan(countIdx);
        });

        it("シーンコレクションの往復は配信を止めた後の締めにある (出力中の切り替えは未実測なので踏まない)", () => {
            const r = planSteps(argsFor("call-1"));
            const kinds = r.steps.map((s) => s.kind);
            const idx = (kind: string) => kinds.indexOf(kind);
            const switchAwayIdx = r.steps.findIndex((s) => s.kind === "obsws-request" && s.request === "SetCurrentSceneCollection");
            expect(idx("stream-stop")).toBeLessThan(switchAwayIdx);
            expect(idx("stream-sink-stop")).toBeLessThan(switchAwayIdx);
            expect(switchAwayIdx).toBeLessThan(idx("judge-connection-held"));
            expect(r.steps.find((s) => s.kind === "judge-connection-held")).toMatchObject({ gate: true });
            const baselineIdx = r.steps.findIndex((s, i) => s.kind === "wait-connection" && i > idx("record-start"));
            expect(baselineIdx).toBeGreaterThan(idx("stream-stop"));
            expect(baselineIdx).toBeLessThan(switchAwayIdx);
        });

        it("戻した後に短く録って frames.py に掛ける (「戻した後に欠落がない」ことの確認)。録画は av1 / av2 / main / ac35 の 4 つ", () => {
            const r = planSteps(argsFor("call-1"));
            const kinds = r.steps.map((s) => s.kind);
            const switchBackIdx = r.steps.map((s, i) => ({ s, i })).filter((x) => x.s.kind === "obsws-request" && x.s.request === "SetCurrentSceneCollection").at(-1).i;
            const ac35RecordStartIdx = r.steps.findIndex((s) => s.kind === "record-start" && s.label === "ac35");
            const ac35RecordStopIdx = r.steps.findIndex((s) => s.kind === "record-stop" && s.label === "ac35");
            expect(switchBackIdx).toBeLessThan(ac35RecordStartIdx);
            expect(ac35RecordStartIdx).toBeLessThan(ac35RecordStopIdx);
            expect(ac35RecordStopIdx).toBeLessThan(kinds.lastIndexOf("judge-connection-held"));
            expect(r.steps.filter((s) => s.kind === "collect-recording").map((s) => s.label)).toEqual(["av1", "av2", "main", "ac35"]);
            const ac35Judge = r.steps.find((s) => s.kind === "judge" && s.recording === "ac35");
            expect(ac35Judge).toMatchObject({ tool: "frames.py", cells: CALL_SCREEN_COUNT });
            // 録画の label は重ならない (record-start と record-stop が対で、同じ label が 2 度始まらない)。
            const starts = r.steps.filter((s) => s.kind === "record-start").map((s) => s.label);
            expect(new Set(starts).size).toBe(starts.length);
            expect(r.steps.filter((s) => s.kind === "record-stop").map((s) => s.label)).toEqual(starts);
        });

        it("合成のソースの画像を、名前ラベルの見た目を変える前と後に録画より前に撮る (見た目はドックの [表示] タブの 1 か所)", () => {
            const r = planSteps(argsFor("call-1"));
            const recordStartIdx = r.steps.findIndex((s) => s.kind === "record-start");
            const shots = r.steps.filter((s) => s.kind === "screenshot-source");
            expect(shots.map((s) => [s.source, s.label])).toEqual([
                [{ kind: "composite" }, "ac31-before"],
                [{ kind: "composite" }, "ac31-after"],
            ]);
            for (const shot of shots) expect(r.steps.indexOf(shot)).toBeLessThan(recordStartIdx);
            // 見た目を変えるのはドックの [表示] タブの「名前」の [表示]: 外す (押すとすぐ反映される) -> 撮る -> 入れる。
            const firstShot = r.steps.indexOf(shots[0]);
            const changed = r.steps.indexOf(shots.at(-1)!);
            const clicks = r.steps.slice(firstShot, changed + 4).filter((s) => s.kind === "dialog-click");
            // 戻した後は [参加者] タブへ戻す (後の手順が参加者の行を押すため)。
            expect(clicks.map((c) => [c.controlType, c.buttonTextId, c.buttonGroupTextId ?? null, c.desired ?? null])).toEqual([
                ["TabItem", TEXT_ID_DOCK_TAB_DISPLAY, null, null],
                ["CheckBox", TEXT_ID_LOOK_SHOW, TEXT_ID_LOOK_HEADING_NAME, "off"],
                ["CheckBox", TEXT_ID_LOOK_SHOW, TEXT_ID_LOOK_HEADING_NAME, "on"],
                ["TabItem", "cordmux.dock.tab_people.label", null, null],
            ]);
            expect(r.steps.indexOf(clicks[1])).toBeLessThan(changed);
            expect(r.steps.indexOf(clicks[2])).toBeGreaterThan(changed);
            for (const c of clicks) expect(r.steps.indexOf(c)).toBeLessThan(recordStartIdx);
            expect(r.steps.some((s) => s.kind === "set-source-filter-setting")).toBe(false);
        });

        it("テストを実行する人が Vesktop を開き直すのは call-1 の seed-config と OBS の起動が済んだ後。開き直しと通話への参加は human で待つ", () => {
            const r = planSteps(argsFor("call-1"));
            const kinds = r.steps.map((s) => s.kind);
            const idx = (kind: string) => kinds.indexOf(kind);
            expect(idx("seed-config")).toBeLessThan(idx("human"));
            expect(idx("launch-obs")).toBeLessThan(idx("human"));
            expect(idx("obsws-connect")).toBeLessThan(idx("human"));
            expect(idx("human")).toBeLessThan(idx("wait-connection"));
            expect(r.steps[idx("wait-connection")]).toMatchObject({ gate: true, timeoutMs: CALL_CONNECT_WAIT_TIMEOUT_MS });
            // Vesktop の起動の依頼: まだ動いていれば通知領域の [Quit] で終了してから (ウィンドウの閉じるボタンではトレイに残る)。
            expect(r.steps[idx("human")].message).toBe(VESKTOP_START_FOR_CALL_ONE_MESSAGE);
            expect(r.steps[idx("human")].message).toContain("[Quit]");
            expect(r.steps[idx("human")].message).not.toContain("閉じて");
            // 通話に参加して共有を始めるのは繋がった後、配線の前。
            const join = r.steps.findIndex((s, i) => s.kind === "human" && i > idx("wait-connection"));
            expect(join).toBeLessThan(idx("wire-composite"));
            // 返事を待たず、4 つのアカウントが入って 3 人が共有を始めたことを OBS の記録で見て進む (11-13 の 1 回目は、返事の後に Vesktop が入っていなかった)。
            expect(r.steps[join]).toMatchObject({ until: "call-ready", timeoutMs: HUMAN_JOIN_TIMEOUT_MS, roster: 4, sharing: 3 });
            // ボイスチャンネルの名前は --voice-channel で渡したときだけ文に入る。渡さなければ名前を挙げない。
            expect(r.steps[join].message).toContain("指定のボイスチャンネル");
            // Chrome は実行を始める前に、実行スクリプトの側がテスト用のマイクの音つきで起動しておく (途中で閉じさせない)。
            // Vesktop の共有は依頼しない (VESKTOP_SHARE_IN_CALL_ONE)。
            expect(r.steps[join].message).toBe(callJoinMessage({ vesktopShare: VESKTOP_SHARE_IN_CALL_ONE }));
            expect(r.steps[join].message).not.toContain("閉じて");
            // テスト用のマイクの音は Chrome だけ。ほかの 3 つのマイクはミュート (スピーカーから出た音が声に混ざらない)。
            expect(r.steps[join].message).toContain("マイクは、Chrome (sub2) だけミュートを解除し");
            expect(r.steps[join].message).toContain("ほかの3つはミュートにしてください");
        });

        it("参加の文 (callJoinMessage): Vesktop の共有は旗で切り替わる。どちらの形も Chrome を閉じさせない", () => {
            expect(VESKTOP_SHARE_IN_CALL_ONE).toBe(false);
            const without = callJoinMessage({ vesktopShare: false });
            const withShare = callJoinMessage({ vesktopShare: true });
            expect(without).toContain("Vesktop (sub1) では画面共有を始めないでください");
            // Vesktop も同じボイスチャンネルに入ることを、どちらの形でも文で依頼する。
            for (const m of [without, withShare]) expect(m).toContain("Vesktop (sub1) の4つで、指定のボイスチャンネルに入ってください");
            expect(withShare).toContain("Vesktop (sub1) でも、画面共有を1つ始めてください");
            // 利用者に見せる文は、英数字と日本語の間に半角の空白を入れない。
            for (const m of [without, withShare]) expect(m).not.toMatch(/[A-Za-z0-9] [\u3041-\u3093\u30a1-\u30f6\u4e00-\u9fa0]|[\u3041-\u3093\u30a1-\u30f6\u4e00-\u9fa0] [A-Za-z0-9]/);
            for (const m of [without, withShare]) {
                expect(m).toContain("Chrome (sub2)");
                expect(m).not.toContain("Chrome は一度閉じて");
                expect(m).not.toContain("開き直します");
            }
            expect(callJoinMessage()).toBe(without);
        });

        it("締め: 診断情報の zip -> ログの回収 -> テストを実行する人が Vesktop を閉じる -> テストを実行する人がアンインストール -> state.json の判定 -> Vesktop の開き直し -> OBS を閉じる -> 判定ツール (録画を読むだけ) -> OBS の録画を残す -> 後始末", () => {
            const r = planSteps(argsFor("call-1"));
            const kinds = r.steps.map((s) => s.kind);
            const lastIdx = (kind: string) => kinds.lastIndexOf(kind);
            // 判定ツール (35.7 分) を [外す] の前に置くと、期限に達して [外す] と締めの判定が実行されない。
            // 録画を読むだけの判定ツールは テストを実行する人の操作を全部済ませ、OBS を閉じた後に実行する。
            expect(lastIdx("check-bundle-zip")).toBeLessThan(lastIdx("collect-plugin-log"));
            const restoredJudge = r.steps.findIndex((s) => s.kind === "judge-vencord-dir-restored");
            const mainJudges = r.steps.map((s, i) => ({ s, i })).filter((x) => x.s.kind === "judge" && ["main", "ac35"].includes(x.s.recording));
            expect(mainJudges).toHaveLength(4);
            for (const j of mainJudges) {
                expect(j.i).toBeGreaterThan(restoredJudge);
                expect(j.i).toBeGreaterThan(lastIdx("close-obs"));
                expect(j.i).toBeLessThan(lastIdx("write-result"));
            }
            // 送信側が要るもの (繋がったままの判定と録画、zip) は Vesktop を閉じてもらう前のまま。
            const closeVesktop = r.steps.findIndex((s) => s.kind === "human" && s.message === VESKTOP_HANG_UP_AND_QUIT_MESSAGE);
            expect(lastIdx("judge-connection-held")).toBeLessThan(closeVesktop);
            expect(r.steps.findIndex((s) => s.kind === "record-start" && s.label === "ac35")).toBeLessThan(closeVesktop);
            // OBS の録画を recordings の下へ移すのは結果を書く直前 (OBS を閉じ、判定ツールが読み終わった後)。
            expect(kinds.indexOf("keep-obs-recordings")).toBe(lastIdx("write-result") - 1);
            expect(kinds.indexOf("keep-obs-recordings")).toBeGreaterThan(mainJudges.at(-1)!.i);
            const uninstall = r.steps.findIndex((s) => s.kind === "human" && s.until === "state-json-restored");
            expect(uninstall).toBeGreaterThan(lastIdx("collect-plugin-log"));
            // [外す] はバックアップを消すので、依頼する前にバックアップを読んでおく。
            expect(r.steps[uninstall - 1]).toMatchObject({ kind: "capture-backup", backupPath: "C:/obs/" + VENCORD_DIR_BACKUP_REL, gate: true });
            // 製品は Vesktop が動いている間は [外す] を押せないので、[外す] の前に Vesktop を閉じてもらう
            // 閉じるのは、繋がったままの判定と録画、zip (送信側が繋がっている間に済ませる) の後 = 最後のログの回収の後。
            expect(r.steps[uninstall - 2]).toMatchObject({ kind: "human", until: "go-file", timeoutMs: HUMAN_TIMEOUT_MS });
            expect(r.steps[uninstall - 2].message).toBe(VESKTOP_HANG_UP_AND_QUIT_MESSAGE);
            expect(r.steps[uninstall - 2].message).toContain("[Quit]");
            expect(r.steps[uninstall - 2].message).toContain("通話を切り");
            expect(uninstall - 2).toBeGreaterThan(lastIdx("collect-plugin-log"));
            expect(uninstall - 2).toBeGreaterThan(lastIdx("check-bundle-zip"));
            expect(uninstall - 2).toBeGreaterThan(at(r.steps, (s) => s.kind === "judge-connection-held"));
            expect(r.steps[uninstall]).toMatchObject({ gate: true, timeoutMs: HUMAN_UNINSTALL_TIMEOUT_MS, appDataRel: VESKTOP_STATE_JSON_REAL_REL });
            expect(r.steps[uninstall].backupPath).toBe("C:/obs/" + VENCORD_DIR_BACKUP_REL);
            // 経路は実行手順と同じ「OBS の [ツール] -> cordmuxをアンインストール... -> 確認 -> 結果」で、テキストは ini の textRef で指す。
            // 確認のダイアログが出ないときに読む所 ([バージョン情報] のページ) も textRef で指す。ドックのメニューは指さない。
            for (const id of [OBS_TEXT_TOOLS_MENU, TEXT_ID_TOOLS_MENU_UNINSTALL, TEXT_ID_DIALOG_UNINSTALL_BUTTON, OBS_TEXT_OK, TEXT_ID_SETTINGS_PAGE_ABOUT]) {
                expect(r.steps[uninstall].message).toContain(textRef(id));
            }
            for (const id of [TEXT_ID_DOCK_MENU_BUTTON, TEXT_ID_MENU_SETTINGS, TEXT_ID_SETTINGS_UNINSTALL]) {
                expect(r.steps[uninstall].message).not.toContain(textRef(id));
            }
            const readAfter = r.steps.findIndex((s, i) => s.kind === "read-state-json" && i > uninstall);
            const judgeRestored = r.steps.findIndex((s) => s.kind === "judge-vencord-dir-restored");
            expect(readAfter).toBeLessThan(judgeRestored);
            expect(r.steps[judgeRestored]).toMatchObject({ gate: true, backupPath: "C:/obs/" + VENCORD_DIR_BACKUP_REL });
            const reopen = r.steps.findIndex((s, i) => s.kind === "human" && i > judgeRestored);
            expect(r.steps[reopen]).toMatchObject({ until: "go-file" });
            expect(r.steps[reopen].message).toBe(VESKTOP_START_AFTER_UNINSTALL_MESSAGE);
            // 終了は [外す] の前に済んでいるので、起動の文は終了の手順 ([Quit]) も「閉じて」も言わない。
            expect(r.steps[reopen].message).not.toContain("閉じて");
            expect(r.steps[reopen].message).not.toContain("[Quit]");
            expect(reopen).toBeLessThan(lastIdx("close-obs"));
            // テストを実行する人に依頼する操作は開き直しが最後 (この後に human はない)。
            expect(r.steps.slice(reopen + 1).some((s) => s.kind === "human")).toBe(false);
            // 実行スクリプトはアンインストールを押さない (実際の実行)。巻き戻しはアンインストールで済ませ、cleanup は -CallWindow なし。
            expect(r.steps.some((s) => s.kind === "dialog-click" && s.buttonTextId === TEXT_ID_SETTINGS_UNINSTALL)).toBe(false);
            expect(r.steps.some((s) => s.kind === "dialog-click" && s.buttonTextId === TEXT_ID_TOOLS_MENU_UNINSTALL)).toBe(false);
            expect(r.steps.at(-1)).toMatchObject({ kind: "cleanup", callWindow: false });
            // 判定ツールはメインの手順の録画 (label main) に対して実行する。
            const judges = r.steps.filter((s) => s.kind === "judge");
            expect(judges.filter((s) => s.recording === "main").map((s) => s.tool)).toEqual(["frames.py", "sync.py", "smooth.py"]);
            expect(judges.filter((s) => s.recording === "ac35")).toHaveLength(1);
            expect(judges.filter((s) => s.tool === "sync.py" && ["av1", "av2"].includes(s.recording))).toHaveLength(2);
        });

        it("巻き戻し (-CallWindow) は step の中では走らせない (単独で実行する形)", () => {
            for (const stepName of ["call-gate-a", "call-gate-b", "call-gate-c", "call-gate-d", "call-1"]) {
                const r = planSteps(argsFor(stepName));
                expect(r.steps.at(-1)).toMatchObject({ kind: "cleanup", callWindow: false });
            }
        });

        it("--decoder hardware は seed-config にそのまま渡り、call-* の seed は 4 ファイルを置く ours (呼び出し行の --dock-install に任せない)", () => {
            const r = planSteps(argsFor("call-1", { decoder: "hardware" }));
            expect(r.steps[0]).toMatchObject({ kind: "seed-config", decoder: "hardware", dockInstall: "ours" });
            const elsewhere = planSteps(argsFor("call-1", { dockInstall: "elsewhere" }));
            expect(elsewhere.steps[0]).toMatchObject({ kind: "seed-config", dockInstall: "ours" });
        });

        it("human の手順: until は 4 つのどれか、文は利用者向けの平易な日本語 (内部の語と ID の数字の並びを含まない)、時限と見込みを持つ", () => {
            for (const steps of [planSteps(argsFor("call-1")).steps, withBroadcaster(), planSteps(argsFor("call-gate-a")).steps]) {
                const humans = steps.filter((s) => s.kind === "human");
                expect(humans.length).toBeGreaterThan(0);
                for (const h of humans) {
                    expect(HUMAN_UNTIL_VALUES).toContain(h.until);
                    expect(typeof h.message).toBe("string");
                    expect(h.message.length).toBeGreaterThan(0);
                    expect(h.message).not.toMatch(/\d{5,}/);
                    expect(h.message).not.toMatch(/\b(step|gate|runner|harness|UIA|dock-click|go-file)\b/i);
                    expect(h.timeoutMs).toBeGreaterThan(0);
                    expect(h.waitSeconds).toBeGreaterThanOrEqual(0);
                    expect(h.appDataRel).toBe(VESKTOP_STATE_JSON_REAL_REL);
                    if (h.until === "state-json-points-to-placed") expect(h.expectedDir).toBe("C:/obs/" + BUNDLED_VENCORD_REL);
                    if (h.until === "state-json-restored") expect(h.backupPath).toBe("C:/obs/" + VENCORD_DIR_BACKUP_REL);
                }
            }
        });

        it("--broadcaster-id があると別アカウント方式の段 (欄への入力 -> 案内の判定 -> 抜ける / 戻る / ミュート / 解除) が本体の直後に出る。無ければ出ない", () => {
            const steps = withBroadcaster();
            // メインアカウントは、ドックの上部のメインの行の [変更] を押し、メニューから人の名前を選んで替える (値はすぐ保存される)。
            const accountChange = steps.findIndex((s) => s.kind === "dialog-click" && s.buttonTextId === TEXT_ID_DOCK_ACCOUNT_CHANGE);
            expect(steps[accountChange]).toMatchObject({ kind: "dialog-click", titleTextId: TEXT_ID_DOCK_WINDOW_TITLE, controlType: "Button", noWait: true, gate: true });
            expect(steps[accountChange + 1]).toMatchObject({ kind: "dialog-click", popup: true, controlType: "MenuItem", noWait: true, gate: true });
            expect(String(steps[accountChange + 1].buttonText).length).toBeGreaterThan(0);
            expect(steps[accountChange + 3]).toMatchObject({ kind: "wait", ms: CALL_ACCOUNT_CHANGE_SETTLE_MS });
            expect(steps.findIndex((s) => s.kind === "count-operations")).toBeLessThan(accountChange);
            const av1 = steps.findIndex((s) => s.kind === "record-start" && s.label === "av1");
            expect(accountChange).toBeLessThan(av1);
            // 別アカウント方式の段は、欄への入力からテスト動画の共有を依頼する human の直前まで。
            const av1Human = steps.findIndex((s) => s.kind === "human" && s.message.includes("テスト動画"));
            expect(av1Human).toBeLessThan(av1);
            const block = steps.slice(accountChange, av1Human);
            expect(block.filter((s) => s.kind === "human").map((s) => s.until)).toEqual(["go-file", "go-file", "go-file", "go-file"]);
            expect(block.filter((s) => s.kind === "human").map((s) => s.message)).toEqual([
                expect.stringContaining("抜け"),
                expect.stringContaining("戻して"),
                expect.stringContaining("ミュート"),
                expect.stringContaining("解除"),
            ]);
            // 案内はドックのウィンドウの文で判定する (ログには出ない)。案内が出るのは 1 回、消えるのは 2 回。実際の実行ではゲート。
            // スピーカーミュートでは、ミュートで映像が止まるとは限らないので、「案内が出ている」と
            // 「映像が止まっている」の一致だけを見る (ミュートの間と解除した後の 2 回。どちらも直前に映像の数を読む)。
            const judges = block.filter((s) => s.kind === "judge-rt");
            expect(judges.map((s) => s.judge)).toEqual(["not-named-in-texts", "named-in-texts", "not-named-in-texts", "notice-iff-frames-stopped", "notice-iff-frames-stopped"]);
            const measures = block.filter((s) => s.kind === "measure-video-rate");
            expect(measures.map((s) => s.label)).toEqual(["speaker-mute", "speaker-unmuted"]);
            for (const m of measures) expect(m.windowLines).toBe(VIDEO_RATE_WINDOW_LINES);
            for (const j of judges.slice(3)) {
                expect(j.inputs.videoRate).toEqual({ videoRate: j.inputs.texts.dialog });
                expect(block.indexOf(block.find((s) => s.kind === "measure-video-rate" && s.label === j.inputs.texts.dialog)!)).toBe(block.indexOf(j) - 1);
            }
            // ふだんのアカウントの見出しは、人の名前が分かるかで 2 つの鍵を使い分ける。「出る」はどちらか 1 つで成功 (anyName)。
            const notInCall = [TEXT_ID_MAIN_ACCOUNT_NOT_IN_CALL, TEXT_ID_MAIN_ACCOUNT_NOT_IN_CALL_NO_NAME];
            expect(judges.map((s) => s.inputs.names.textPrefix)).toEqual([notInCall, notInCall, notInCall, TEXT_ID_NO_VIDEO, TEXT_ID_NO_VIDEO]);
            expect(judges[1]).toMatchObject({ judge: "named-in-texts", anyName: true });
            for (const j of judges) expect(j.gate).toBe(true);
            for (const j of judges) expect(block.slice(0, block.indexOf(j)).filter((s) => s.kind === "dialog-click" && s.label === j.inputs.texts.dialog)).toHaveLength(1);
            expect(block.filter((s) => s.kind === "wait" && s.ms === CALL_NOTICE_SETTLE_MS)).toHaveLength(4);
            expect(block.filter((s) => s.kind === "obsws-request").map((s) => s.request)).toEqual(["GetInputList"]);

            const without = planSteps(argsFor("call-1")).steps;
            expect(without.some((s) => s.kind === "dialog-click" && s.buttonTextId === TEXT_ID_DOCK_ACCOUNT_CHANGE)).toBe(false);
            expect(without.some((s) => s.kind === "judge-rt" && (Array.isArray(s.inputs.names.textPrefix) || s.inputs.names.textPrefix === TEXT_ID_NO_VIDEO))).toBe(false);
            expect(without.filter((s) => s.kind === "human")).toHaveLength(steps.filter((s) => s.kind === "human").length - 4);
        });

        it("--broadcaster-id は call-1 だけが受け、値は文字列のまま (数として読まない)", () => {
            // 桁の多い数字の並びでも文字列のまま (実在の ID は書かない。桁数だけ真似る)。
            const longId = "9".repeat(20);
            const r = parseArgs([...argvFor("call-1"), "--broadcaster-id", longId]);
            expect(r.ok).toBe(true);
            expect(r.args.broadcasterId).toBe(longId);
            // 製品の欄は数字 0 から 20 桁だけを受ける (settings_window.cpp のチェック)。受けない値は呼び出し行で断る。
            expect(parseArgs([...argvFor("call-1"), "--broadcaster-id", "9".repeat(21)]).ok).toBe(false);
            expect(parseArgs([...argvFor("call-1"), "--broadcaster-id", "harness-screen-1"]).ok).toBe(false);
            expect(parseArgs([...argvFor("call-gate-a"), "--broadcaster-id", "1"]).ok).toBe(false);
            expect(parseArgs([...argvFor("record"), "--broadcaster-id", "1"]).ok).toBe(false);
            expect(parseArgs(argvFor("call-1")).args.broadcasterId).toBeNull();
        });

        it("テスト動画の A/V: 共有を依頼する -> 20 秒 -> 録画 (配信は続けたまま) 5 分 -> 止める -> 回収 -> sync.py (区画ごと)。av2 はメインの手順の録画 (main) の開始から 90 分以上あと", () => {
            for (const steps of [planSteps(argsFor("call-1")).steps, withBroadcaster()]) {
                for (const label of ["av1", "av2"]) {
                    const start = at(steps, (s) => s.kind === "record-start" && s.label === label);
                    expect(steps[start]).toMatchObject({ stream: false });
                    // 録画の間はドックの行で、声を全員ミュートにし、ほかの共有している人 (メインと Firefox) の画面共有の音をミュートにし、
                    // テスト動画を共有している人の画面共有の音のミュートを外す (合成のソースの音は全員の音を混ぜた 1 本)。止めた直後に戻す。
                    const voices = callVoicePersons({}).length;
                    const others = [DEFAULT_CALL_ACCOUNTS.main, DEFAULT_CALL_ACCOUNTS.firefox];
                    const head = start - 1 - others.length - voices;
                    expect(steps[head - 3]).toMatchObject({ kind: "human", until: "go-file" });
                    expect(steps[head - 3].message).toContain("テスト動画");
                    expect(steps[head - 2]).toMatchObject({ kind: "wait", ms: CALL_AV_SHARE_SETTLE_MS });
                    expect(steps[head - 1]).toMatchObject({ kind: "dialog-click", controlType: "TabItem", buttonTextId: "cordmux.dock.tab_people.label", readOnlyOp: true });
                    expect(steps.slice(head, head + voices).map((s) => [s.textId, s.textArgs[0], s.desired, s.muteOp])).toEqual(
                        callVoicePersons({}).map((p) => ["cordmux.dock.row_voice_mute.a11y", p, "on", true]),
                    );
                    expect(steps.slice(head + voices, start - 1).map((s) => [s.textId, s.textArgs[0], s.desired, s.absentOk])).toEqual(
                        others.map((p) => ["cordmux.dock.row_screen_mute.a11y", p, "on", true]),
                    );
                    expect(steps[start - 1]).toMatchObject({ kind: "dock-click", textId: "cordmux.dock.row_screen_mute.a11y", textArgs: [DEFAULT_CALL_ACCOUNTS.chrome], desired: "off", muteOp: true });
                    expect(steps[start + 1]).toMatchObject({ kind: "wait", ms: CALL_AV_RECORD_MS });
                    expect(steps[start + 2]).toMatchObject({ kind: "record-stop", label, stream: false });
                    expect(steps[start + 3]).toMatchObject({ kind: "dock-click", textId: "cordmux.dock.row_screen_mute.a11y", desired: "on", muteOp: true });
                    expect(steps[start + 3 + voices]).toMatchObject({ kind: "dock-click", textId: "cordmux.dock.row_voice_mute.a11y", desired: "off" });
                    expect(steps[start + 4 + voices]).toMatchObject({ kind: "collect-recording", label });
                    expect(steps[start + 5 + voices]).toMatchObject({ kind: "judge", tool: "sync.py", cells: CALL_SCREEN_COUNT, recording: label, label });
                }
                // 枠のソースを切る手順 (isolate-audio / restore-audio) はもうない。
                expect(steps.some((s) => s.kind === "isolate-audio" || s.kind === "restore-audio")).toBe(false);
                const main = at(steps, (s) => s.kind === "record-start" && s.label === "main");
                const av1 = at(steps, (s) => s.kind === "record-start" && s.label === "av1");
                const av2 = at(steps, (s) => s.kind === "record-start" && s.label === "av2");
                // 90 分の起点はメインの手順の録画の開始。メインの手順にも sync.py を掛けるので開始直後の値はそこから取れる。
                expect(main).toBeLessThan(av1);
                expect(plannedMinutes(steps.slice(main, av2))).toBeGreaterThanOrEqual(CALL_AV_SECOND_GAP_MINUTES);
                // 連続運転 (6 周 + わざと異常 12 分) を終えた時点でほぼ 90 分に達するので、埋めるための待ちはないか、あっても
                // 周期 2 つ (10 分) 未満 (別アカウント方式の段がない場合だけ数分の待ちが入る。使う声の置き換え (2 分 20 秒 x 2) は
                // 「使う声」のラベルができるまで飛ばすので、その分だけ待ちが長い)。
                const fill = steps.filter((s) => s.kind === "wait" && String(s.label).includes("2 回目の A/V まで"));
                expect(fill.length).toBeLessThanOrEqual(1);
                for (const w of fill) expect(w.ms).toBeLessThan(2 * CALL_CHECK_PERIOD_MS);
                expect(steps.filter((s) => s.kind === "judge" && s.tool === "sync.py").map((s) => s.recording)).toEqual(["av1", "av2", "main"]);
                const drift = steps.find((s) => s.kind === "judge-av-drift");
                expect(drift).toMatchObject({ first: "av1", second: "av2" });
                expect(steps.indexOf(drift)).toBeGreaterThan(av2);
                expect(drift.gate).toBeUndefined();
            }
        });

        it("途中で録画 ON: [録画を始める] -> 10 分 -> [録画を止める] -> 録画ファイルの作成の待ち -> --step record と同じ判定 3 つ (すべてゲート)", () => {
            const steps = planSteps(argsFor("call-1")).steps;
            const on = at(steps, (s) => s.kind === "dock-click" && s.textId === TEXT_ID_RECORD_START);
            const off = at(steps, (s) => s.kind === "dock-click" && s.textId === TEXT_ID_RECORD_STOP);
            expect(steps[on]).toMatchObject({ desired: "on", gate: true });
            // 録画中の [録画を終了] はテキストの後ろに経過時間が付くので、前方一致で探す。
            expect(steps[off]).toMatchObject({ desired: "off", gate: true, namePrefix: true });
            expect(steps.some((s) => s.kind === "dock-tab")).toBe(false);
            expect(steps.slice(on, off).filter((s) => s.kind === "wait").map((s) => s.ms)).toEqual([CALL_MID_RECORD_MS]);
            const finalize = at(steps, (s) => s.kind === "wait-finalize");
            expect(finalize).toBeGreaterThan(off);
            expect(steps[finalize]).toMatchObject({ gate: true, recordDirRel: RECORD_DIR_REL });
            expect(finalize).toBeLessThan(steps.map((s) => s.kind).lastIndexOf("close-obs"));
            const judges = steps.filter((s) => s.kind === "judge-record");
            expect(judges.map((s) => s.tool)).toEqual(["ledger_check.py", "black_span.py", "au_hash.py"]);
            expect(judges[1]).toMatchObject({ mode: "no-leading-black", target: "cordmux" });
            for (const j of judges) expect(j.gate).toBe(true);
            expect(steps.some((s) => s.kind === "collect-record-session")).toBe(true);
            // 途中 ON は av1 の後、使う声の置き換え (テストを実行する人に依頼する手順) の前。
            expect(on).toBeGreaterThan(at(steps, (s) => s.kind === "record-stop" && s.label === "av1"));
            expect(finalize).toBeLessThan(at(steps, (s) => s.kind === "human" && String(s.message).includes(textRef(TEXT_ID_ROW_VOICE_FROM))));
        });

        it("使う声の置き換え: call-1 は参加者の行の [...] のメニュー「使う声」を依頼する。飛ばすのは available が false のときだけ", () => {
            const steps = planSteps(argsFor("call-1")).steps;
            expect(steps.filter((s) => s.kind === "skip")).toHaveLength(0);
            const asks = steps.filter((s) => s.kind === "human" && String(s.message).includes(textRef(TEXT_ID_ROW_VOICE_FROM)));
            expect(asks).toHaveLength(2);
            expect(callVoiceSourceSteps()).toEqual(callVoiceSourceSteps(true));
            // 行のメニューを持たない版の製品で実行するときだけ、テストを実行する人に依頼せずに飛ばして理由を残す。
            const skipped = callVoiceSourceSteps(false);
            expect(skipped.map((s) => s.kind)).toEqual(["skip"]);
            expect(skipped[0]).toMatchObject({ requirement: expect.any(String) });
        });

        it("使う声の置き換え: 人への文はメニューのテキストを ini の textRef でそのまま引き、テキストをコードで組み直さない。画面共有している人は sub3・声を貸す人は sub2", () => {
            // 置き換えると声を貸す人 (Chrome のテスト用のマイクの 1 kHz) が鳴っている声として画面共有している人の枠へ移る組み合わせ。
            expect(DEFAULT_CALL_ACCOUNTS.firefox).toBe("sub3");
            expect(DEFAULT_CALL_ACCOUNTS.chrome).toBe("sub2");
            const steps = callVoiceSourceSteps();
            expect(steps[0].message).toContain(DEFAULT_CALL_ACCOUNTS.firefox + "の行の [...] (" + textRef(TEXT_ID_ROW_MORE_ACTIONS) + ")");
            expect(steps[0].message).toContain("[" + textRef(TEXT_ID_ROW_VOICE_FROM) + "] の中の「" + DEFAULT_CALL_ACCOUNTS.chrome + "」");
            expect(steps[4].message).toContain("「" + textRef(TEXT_ID_ROW_OWN_VOICE) + "」");
            // textRef を ini のテキストで置き換えた文に、textRef が残らない (引ける id だけを使っている)。
            const values = jaIniValues();
            for (const h of [steps[0], steps[4]]) {
                const out = expandTextRefs(h.message, (id: string) => values.get(id) ?? null);
                expect(out.missing).toEqual([]);
                expect(out.text).not.toContain("{{");
            }
        });

        it("使う声の置き換え: 依頼する -> 20 秒 -> PNG + ログ -> 戻してもらう -> 20 秒", () => {
            const steps = callVoiceSourceSteps();
            expect(steps.map((s) => s.kind)).toEqual(["human", "wait", "screenshot-window", "collect-plugin-log", "human", "wait"]);
            expect(steps[1]).toMatchObject({ kind: "wait", ms: CALL_VOICE_SOURCE_SETTLE_MS });
            expect(steps[2]).toMatchObject({ kind: "screenshot-window", shotLabel: "voice-source-changed" });
            expect(steps[4]).toMatchObject({ kind: "human", until: "go-file" });
            expect(steps[4].message).toContain("戻して");
            expect(steps[5]).toMatchObject({ kind: "wait", ms: CALL_VOICE_SOURCE_SETTLE_MS });
        });

        it("連続運転: 5 分周期の照合が 6 周 (周期ごとにメッセージがないことを読む) -> わざと 1 バイト足す (コピーを取る) -> 6 分 -> メッセージと [再インストール] -> コピーから戻す -> 6 分 -> 消える", () => {
            const steps = planSteps(argsFor("call-1")).steps;
            const tamper = at(steps, (s) => s.kind === "tamper-file");
            // 周期の待ちは説明の文字列で見分ける (5 分の待ちは A/V の録画にもあり、30/60/90 分の obs-stats が間に挟まりうる)。
            const cycleWaits = steps.slice(0, tamper).filter((s) => s.kind === "wait" && s.ms === CALL_CHECK_PERIOD_MS && String(s.label).includes("照合の周期"));
            expect(cycleWaits).toHaveLength(CALL_CHECK_CYCLES);
            for (const w of cycleWaits) {
                const after = steps.slice(steps.indexOf(w) + 1, steps.indexOf(w) + 5).map((x) => x.kind);
                expect(after.filter((k) => k !== "obs-stats").slice(0, 3)).toEqual(["collect-plugin-log", "dialog-click", "judge-rt"]);
            }
            const cycleJudges = steps.slice(0, tamper).filter((s) => s.kind === "judge-rt" && s.judge === "not-named-in-texts" && s.inputs.names.textPrefix === TEXT_ID_VENCORD_OVERWRITTEN);
            expect(cycleJudges).toHaveLength(CALL_CHECK_CYCLES);
            for (const j of cycleJudges) expect(j.gate).toBe(true);
            expect(steps[tamper]).toMatchObject({ name: CALL_TAMPER_FILE, dirRel: BUNDLED_VENCORD_REL, obs: "C:/obs", keepCopy: true, restoreId: "call-tamper", gate: true });
            expect(VENCORD_BUNDLE_FILE_NAMES).toContain(CALL_TAMPER_FILE);
            expect(steps[tamper + 1]).toMatchObject({ kind: "wait", ms: CALL_TAMPER_WAIT_MS });
            expect(CALL_TAMPER_WAIT_MS).toBeGreaterThan(CALL_CHECK_PERIOD_MS);
            const warned = at(steps, (s) => s.kind === "dialog-click" && s.label === "tamper-warned");
            expect(steps[warned]).toMatchObject({ listOnly: true, titleTextId: TEXT_ID_DOCK_WINDOW_TITLE, buttonTextId: TEXT_ID_REINSTALL, expect: "button", gate: true });
            // メッセージはファイルの名前を画面に出さない (名前はログへ) ので、見出しが出ることを見る。
            expect(steps[warned + 1]).toMatchObject({ kind: "judge-rt", judge: "named-in-texts", gate: true, inputs: { texts: { dialog: "tamper-warned" }, names: { textPrefix: TEXT_ID_VENCORD_OVERWRITTEN } } });
            const untamper = at(steps, (s) => s.kind === "untamper-file");
            expect(untamper).toBeGreaterThan(warned);
            expect(steps[untamper]).toMatchObject({ restoreId: "call-tamper", gate: true });
            expect(steps[untamper + 1]).toMatchObject({ kind: "wait", ms: CALL_TAMPER_WAIT_MS });
            const restored = at(steps, (s) => s.kind === "dialog-click" && s.label === "tamper-restored");
            expect(steps[restored]).toMatchObject({ buttonTextId: TEXT_ID_REINSTALL, expect: "no-button", gate: true });
            expect(steps[restored + 1]).toMatchObject({ kind: "judge-rt", judge: "not-named-in-texts", gate: true });
            // 連続運転は締め (stream-stop) より前、av2 より前。
            expect(restored).toBeLessThan(at(steps, (s) => s.kind === "record-start" && s.label === "av2"));
            expect(restored).toBeLessThan(at(steps, (s) => s.kind === "stream-stop"));
        });

        it("plannedMinutes: 実行計画が見込む待ちの合計は 120 から 140 分 (実行の長さ 2 時間 30 分からゲートと接続待ちの余裕を除いた分)", () => {
            const plain = plannedMinutes(planSteps(argsFor("call-1")).steps);
            const withId = plannedMinutes(withBroadcaster());
            for (const minutes of [plain, withId]) {
                expect(minutes).toBeGreaterThanOrEqual(120);
                expect(minutes).toBeLessThanOrEqual(140);
            }
            expect(withId).toBeGreaterThan(plain);
        });

        it("call-1 の期限の既定は実行計画の見込みより長い (--deadline-min が無ければ CALL_ONE_DEADLINE_MIN。期限に達すると締めの [外す] の判定が実行されないまま打ち切られる)", () => {
            expect(CALL_ONE_DEADLINE_MIN).toBeGreaterThanOrEqual(180);
            const plain = parseArgs(argvFor("call-1"));
            expect(plain.args.deadlineMin).toBe(CALL_ONE_DEADLINE_MIN);
            expect(plain.args.deadlineMinGiven).toBe(false);
            for (const steps of [planSteps(plain.args).steps, withBroadcaster()]) {
                expect(plannedMinutes(steps)).toBeLessThan(CALL_ONE_DEADLINE_MIN);
                expect(deadlineShortfallMinutes(steps, CALL_ONE_DEADLINE_MIN)).toBe(0);
                // テスト用の送信側だけの実行の既定 (120) では足りない = 旗のまま残すなら --deadline-min が要ることを dry-run の欄が示す。
                expect(deadlineShortfallMinutes(steps, DEFAULT_DEADLINE_MIN)).toBeGreaterThan(0);
            }
            // 明示の --deadline-min はそのまま。ゲートとリハーサルとテスト用の送信側だけの実行は既定の 120 のまま。
            expect(parseArgs([...argvFor("call-1"), "--deadline-min", "150"]).args).toMatchObject({ deadlineMin: 150, deadlineMinGiven: true });
            expect(parseArgs(argvFor("call-gate-a")).args.deadlineMin).toBe(DEFAULT_DEADLINE_MIN);
            expect(parseArgs(argvFor("record")).args.deadlineMin).toBe(DEFAULT_DEADLINE_MIN);
            expect(parseArgs(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-1", "--rehearsal"]).args.deadlineMin).toBe(DEFAULT_DEADLINE_MIN);
            expect(deadlineShortfallMinutes([{ kind: "wait", ms: 130 * 60 * 1000 }], 120)).toBe(10);
            expect(deadlineShortfallMinutes([{ kind: "wait", ms: 60 * 1000 }], 120)).toBe(0);
        });

        it("--rehearsal: call-1 だけ。--real-profile / --call-window とは両立しない", () => {
            expect(parseArgs(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-1", "--rehearsal"]).ok).toBe(true);
            expect(parseArgs([...argvFor("call-1"), "--rehearsal"]).ok).toBe(false);
            expect(parseArgs(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-1", "--rehearsal", "--real-profile"]).ok).toBe(false);
            expect(parseArgs(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-1", "--rehearsal", "--call-window"]).ok).toBe(false);
            expect(parseArgs(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-gate-a", "--rehearsal"]).ok).toBe(false);
            expect(parseArgs(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "record", "--rehearsal"]).ok).toBe(false);
        });

        it("--rehearsal の置き換え: human は秒数で自動で進み、1 分以上の待ちは 1 か所の倍率で縮み、テスト用の送信側が Vesktop の代わりに立つ", () => {
            const steps = rehearsal();
            const real = withBroadcaster();
            for (const h of steps.filter((s) => s.kind === "human")) expect(h).toMatchObject({ until: "seconds", waitSeconds: REHEARSAL_HUMAN_WAIT_SECONDS });
            expect(steps.filter((s) => s.kind === "human").length).toBeGreaterThan(0);
            // 定常の 20 分は 2 分に。数秒の待ちはそのまま。
            expect(steps.find((s) => s.kind === "wait" && s.ms === DEFAULT_CALL_MINUTES * 60 * 1000 * REHEARSAL_TIME_FACTOR)).toMatchObject({ rehearsalScaled: true });
            expect(steps.filter((s) => s.kind === "wait" && s.ms >= REHEARSAL_SCALE_MIN_MS && !s.rehearsalScaled)).toHaveLength(0);
            // 数秒の待ちは縮めない (リハーサルは [Vesktop に追加] を押す前の 5 秒が 1 つ増えるだけ)。
            expect(steps.filter((s) => s.kind === "wait" && s.ms === SELECT_FOUR_SETTLE_WAIT_MS).length).toBe(real.filter((s) => s.kind === "wait" && s.ms === SELECT_FOUR_SETTLE_WAIT_MS).length + 1);
            expect(plannedMinutes(steps)).toBeLessThan(30);
            // seed は差し替えた APPDATA に vencordDir のない偽の state.json (absent) を作り、4 ファイルは実行スクリプトがコピーする。
            expect(steps[0]).toMatchObject({ kind: "seed-config", useAppData: true, realProfile: false, dockInstall: "absent", rehearsal: true, vencordCheckConnectedS: ROUNDTRIP_CHECK_PERIOD_S });
            expect(steps[1]).toMatchObject({ kind: "place-vencord-bundle", distRel: "_work/vencord-dist", names: VENCORD_BUNDLE_FILE_NAMES, gate: true });
            // 実行スクリプトが [Vesktop に追加] を押し、テスト用の送信側 (画面 4 + 声 4、マーカーは画面) を起こして繋がるのを待つ。
            const connect = at(steps, (s) => s.kind === "dock-click" && s.textId === TEXT_ID_ADD_TO_VESKTOP);
            expect(steps[connect]).toMatchObject({ gate: true });
            expect(connect).toBeGreaterThan(at(steps, (s) => s.kind === "obsws-connect"));
            const harness = at(steps, (s) => s.kind === "harness-start");
            expect(harness).toBeGreaterThan(connect);
            expect(steps[harness]).toMatchObject({ nScreen: CALL_SCREEN_COUNT, nVoice: CALL_VOICE_COUNT, syncTone: DEFAULT_SYNC_TONE, mode: "product" });
            // 寿命は見込みの 2 倍 + 15 分 (見込みに入らない判定ツールや録画ファイルの作成の待ちの実行時間のぶん)。
            expect(steps[harness].seconds).toBeGreaterThanOrEqual(plannedMinutes(steps) * 60 * 2);
            expect(steps[harness].seconds).toBeGreaterThanOrEqual(plannedMinutes(steps) * 60 + 15 * 60);
            expect(steps[harness + 1]).toMatchObject({ kind: "wait-connection", gate: true });
            // av2 の前に送信側がまだ居ることを確認する (居なければ理由つきで失敗。実際の実行にはこの手順がない)。
            const alive = at(steps, (s) => s.kind === "harness-alive");
            expect(steps[alive]).toMatchObject({ gate: true });
            const av2Human = at(steps, (s) => s.kind === "human" && String(s.message).includes("もう一度共有"));
            expect(alive).toBe(av2Human - 1);
            expect(alive).toBeGreaterThan(at(steps, (s) => s.kind === "untamper-file"));
            expect(real.some((s) => s.kind === "harness-alive")).toBe(false);
            expect(steps.some((s) => s.kind === "human" && s.until !== "seconds")).toBe(false);
            // アンインストールは実行スクリプトが押す (ドックのメニュー -> cordmux の設定 -> [バージョン情報] -> [cordmux を
            // アンインストール...] -> 確認のダイアログ -> 結果のウィンドウ)。判定は実際の実行と同じ。もう 1 つの入口 (OBS の [ツール]) は
            // install-roundtrip が通すので、リハーサルはページのボタンの経路で押す (uninstallSteps の entry を参照)。
            expect(steps.some((s) => s.kind === "dialog-click" && s.buttonTextId === OBS_TEXT_TOOLS_MENU)).toBe(false);
            const uninstall = at(steps, (s) => s.kind === "dialog-click" && s.buttonTextId === TEXT_ID_SETTINGS_UNINSTALL);
            expect(steps[uninstall - 1]).toMatchObject({ kind: "dialog-click", controlType: "ListItem", buttonTextId: TEXT_ID_SETTINGS_PAGE_ABOUT });
            expect(steps[uninstall - 4]).toMatchObject({ kind: "capture-backup", gate: true });
            expect(steps[uninstall]).toMatchObject({ noWait: true, gate: true });
            expect(steps[uninstall + 1]).toMatchObject({ kind: "dialog-click", titleTextId: TEXT_ID_DIALOG_UNINSTALL_TITLE, buttonTextId: TEXT_ID_DIALOG_UNINSTALL_BUTTON, noWait: true, gate: true });
            expect(steps[uninstall + 2]).toMatchObject({ kind: "dialog-click", buttonTextId: OBS_TEXT_OK, listOnly: true, expect: "button", label: "uninstall-done" });
            expect(steps[uninstall + 3]).toMatchObject({ kind: "dialog-click", buttonTextId: OBS_TEXT_OK, expect: "present" });
            expect(at(steps, (s) => s.kind === "judge-vencord-dir-restored")).toBeGreaterThan(uninstall);
            expect(steps.find((s) => s.kind === "judge-vencord-dir-restored")).toMatchObject({ gate: true });
            // テストを実行する人の操作に掛かる案内の判定はゲートにしない (誰も操作しない)。異常の検知 (tamper) の判定はゲートのまま。
            // 配信者が抜ける・戻る判定の 2 つは、テスト用の送信側では人を抜けさせられないので、リハーサルでは判定しない (skip)。
            const noticeJudges = steps.filter((s) => s.kind === "judge-rt" && (Array.isArray(s.inputs.names.textPrefix) || s.inputs.names.textPrefix === TEXT_ID_NO_VIDEO));
            expect(noticeJudges.length).toBe(3);
            expect(steps.filter((s) => s.kind === "skip" && s.item === "broadcaster-left")).toHaveLength(2);
            for (const j of noticeJudges) expect(j.gate).toBeUndefined();
            for (const j of steps.filter((s) => s.kind === "judge-rt" && s.inputs.names.textPrefix === TEXT_ID_VENCORD_OVERWRITTEN)) expect(j.gate).toBe(true);
            // テスト用の送信側は OBS を閉じる前に止める。
            const stop = at(steps, (s) => s.kind === "harness-stop");
            expect(stop).toBeGreaterThan(at(steps, (s) => s.kind === "judge-vencord-dir-restored"));
            expect(stop).toBeLessThan(steps.map((s) => s.kind).lastIndexOf("close-obs"));
            expect(steps.find((s) => s.kind === "dialog-click" && s.buttonTextId === TEXT_ID_DOCK_ACCOUNT_CHANGE)).toBeDefined();
            // 同じ引数なら同じ実行計画 (副作用を持たない)。
            expect(rehearsal()).toEqual(steps);
        });

        it("call-1 / call-gate-a が名指しする ini の id (押すラベル・タブ・案内の文言) は製品の ja-JP.ini にある", () => {
            const keys = new Set<string>();
            for (const line of readFileSync(join(dirname(fileURLToPath(import.meta.url)), "..", "..", "..", "src-obs", "data", "locale", "ja-JP.ini"), "utf8").split(/\r?\n/)) {
                const t = line.trim();
                if (!t || t.startsWith("#") || t.indexOf("=") < 0) continue;
                keys.add(t.slice(0, t.indexOf("=")).trim());
            }
            for (const id of CALL_TEXT_IDS) expect(keys.has(id)).toBe(true);
            for (const steps of [planSteps(argsFor("call-1")).steps, withBroadcaster(), rehearsal(), planSteps(argsFor("call-gate-a")).steps]) {
                for (const s of steps) {
                    // 名指しの欄・判定の文言・人への文と要求の中の textRef の全部 (OBS 自身のテキストの "obs:" は OBS の ini にある = 別の確認)。
                    for (const id of stepTextIds(s).filter((x) => !x.startsWith(OBS_TEXT_PREFIX))) expect(keys.has(id), id).toBe(true);
                    if (s.kind === "judge-rt") expect(ROUNDTRIP_JUDGE_NAMES).toContain(s.judge);
                }
            }
        });
    });
});

describe("call-1 用の純関数 (plannedMinutes / insertStatsAtMinutes / rehearseSteps / judgeVencordDirRestored / judgeNotNamedInTexts / judgeAvDrift)", () => {
    it("plannedMinutes は wait の ms、human の waitSeconds、record-stop の tailSeconds だけを数える (時限や条件待ちは数えない)", () => {
        const steps = [
            { kind: "wait", ms: 60000 },
            { kind: "human", waitSeconds: 90, timeoutMs: 600000 },
            { kind: "record-stop", tailSeconds: 30 },
            { kind: "wait-connection", timeoutMs: 1200000 },
            { kind: "wait-finalize", timeoutMs: 900000 },
            { kind: "harness-wait", seconds: 600 },
        ];
        expect(plannedMinutes(steps)).toBe(3);
        expect(plannedMinutes([])).toBe(0);
        expect(plannedMinutes([{ kind: "wait", ms: 90000 }])).toBe(1.5);
    });

    it("insertStatsAtMinutes は見込みの累計が指定の分を初めて超えた手順の直後に obs-stats を差し込む (届かない分は差し込まない)", () => {
        const steps = [
            { kind: "wait", ms: 20 * 60000, label: "a" },
            { kind: "wait", ms: 20 * 60000, label: "b" },
            { kind: "close-obs", label: "c" },
        ];
        const out = insertStatsAtMinutes(steps, [30, 10, 90]);
        expect(out.map((s) => s.kind === "obs-stats" ? s.label : s.label)).toEqual(["a", "t10min", "b", "t30min", "c"]);
        expect(insertStatsAtMinutes(steps, [])).toEqual(steps);
        expect(insertStatsAtMinutes([], [30])).toEqual([]);
    });

    it("rehearseSteps は human を seconds に、1 分以上の wait を倍率で縮め、他は変えない", () => {
        const out = rehearseSteps([
            { kind: "human", until: "go-file", waitSeconds: 180, message: "m" },
            { kind: "wait", ms: 600000 },
            { kind: "wait", ms: 5000 },
            { kind: "dock-click", name: "x" },
        ]);
        expect(out[0]).toMatchObject({ kind: "human", until: "seconds", waitSeconds: REHEARSAL_HUMAN_WAIT_SECONDS, message: "m" });
        expect(out[1]).toMatchObject({ kind: "wait", ms: 600000 * REHEARSAL_TIME_FACTOR, rehearsalScaled: true });
        expect(out[2]).toEqual({ kind: "wait", ms: 5000 });
        expect(out[3]).toEqual({ kind: "dock-click", name: "x" });
        expect(REHEARSAL_TIME_FACTOR).toBeGreaterThan(0);
        expect(REHEARSAL_TIME_FACTOR).toBeLessThan(1);
    });

    it("judgeVencordDirRestored: バックアップの previous が null なら鍵がないこと、場所なら同じ場所 (区切りと大小を均す) を求める。バックアップや state.json が読めなければ失敗", () => {
        const backupNull = JSON.stringify({ previous: null, written: "C:/obs/plugins/cordmux/data/vencord", written_at: 1, dll_dir: "C:/obs/plugins/cordmux" });
        expect(judgeVencordDirRestored({ stateText: JSON.stringify({ maximized: false }), backupText: backupNull }).ok).toBe(true);
        expect(judgeVencordDirRestored({ stateText: JSON.stringify({ vencordDir: "C:/x" }), backupText: backupNull }).ok).toBe(false);
        const backupPrev = JSON.stringify({ previous: "C:\\Users\\x\\vencord\\dist", written: "C:/obs/plugins/cordmux/data/vencord", written_at: 1 });
        expect(judgeVencordDirRestored({ stateText: JSON.stringify({ vencordDir: "c:/users/X/vencord/dist/" }), backupText: backupPrev }).ok).toBe(true);
        expect(judgeVencordDirRestored({ stateText: JSON.stringify({ vencordDir: "C:/obs/plugins/cordmux/data/vencord" }), backupText: backupPrev }).ok).toBe(false);
        expect(judgeVencordDirRestored({ stateText: JSON.stringify({}), backupText: backupPrev }).ok).toBe(false);
        expect(judgeVencordDirRestored({ stateText: JSON.stringify({}), backupText: null }).ok).toBe(false);
        expect(judgeVencordDirRestored({ stateText: "not json", backupText: backupNull }).ok).toBe(false);
    });

    it("judgeNotNamedInTexts: 文が読めていて名前が 1 つも無ければ合格。名前があれば失敗、文が 1 つも無ければ失敗", () => {
        expect(judgeNotNamedInTexts({ texts: ["つながっています"], names: ["配信者のアカウント ("] }).ok).toBe(true);
        expect(judgeNotNamedInTexts({ texts: ["配信者のアカウント (x) がこの通話に参加していません"], names: ["配信者のアカウント ("] })).toMatchObject({ ok: false, found: ["配信者のアカウント ("] });
        expect(judgeNotNamedInTexts({ texts: [], names: ["a"] }).ok).toBe(false);
        expect(judgeRoundtrip("not-named-in-texts", { texts: ["x"], names: [] }).ok).toBe(true);
        expect(ROUNDTRIP_JUDGE_NAMES).toContain("not-named-in-texts");
    });

    it("judgeAvDrift: 区画ごとに av2 - av1 の平均の差を出す。両方に値のある区画が無ければ失敗 (判定はしない = 傾きは人が出す)", () => {
        const judged = (means: (number | null)[]) => ({
            perCell: means.map((m, i) => ({ cell: i, data: { paired: m === null ? 0 : 3, audio_minus_video_ms: { mean: m } } })),
        });
        const r = judgeAvDrift({ first: judged([10, null, 5, 0]), second: judged([16, 2, null, -4]) });
        expect(r.ok).toBe(true);
        expect(r.measuredCells).toBe(2);
        expect(r.cells.map((c: { deltaMs: number | null }) => c.deltaMs)).toEqual([6, null, null, -4]);
        expect(judgeAvDrift({ first: judged([null]), second: judged([1]) }).ok).toBe(false);
        expect(judgeAvDrift({ first: null, second: judged([1]) }).ok).toBe(false);
        expect(judgeAvDrift({ first: { data: { paired: 1, audio_minus_video_ms: { mean: 1 } } }, second: { data: { paired: 1, audio_minus_video_ms: { mean: 3 } } } }).cells[0].deltaMs).toBe(2);
    });
});

describe("parseStatsVideoDropped", () => {
    it("[cordmux] stats video の行から dropped= を読む (no_route_dropped= と混同しない)", () => {
        const line = "info: [cordmux] stats video received_hz=60.0 decoded_hz=60.0 handed_to_obs_hz=60.0 dropped_hz=0.5 received=600 decoded=600 handed_to_obs=595 dropped=5 no_route_dropped=9";
        expect(parseStatsVideoDropped([line])).toBe(5);
    });

    it("複数行あれば最後 (最新の累積値) を返す", () => {
        const lines = [
            "info: [cordmux] stats video received=100 decoded=100 handed_to_obs=100 dropped=0 no_route_dropped=0",
            "info: [cordmux] stats video received=200 decoded=200 handed_to_obs=180 dropped=20 no_route_dropped=0",
        ];
        expect(parseStatsVideoDropped(lines)).toBe(20);
    });

    it("現在の実装の先頭が大文字の行 (Stats) も読み、古い行と混ざっても最後を返す", () => {
        const lines = [
            "13:00:00.000: [cordmux] stats video received=100 decoded=100 handed_to_obs=100 dropped=3 no_route_dropped=0",
            "13:00:10.000: [cordmux] Stats video received_hz=60.0 decoded_hz=60.0 handed_to_obs_hz=60.0 dropped_hz=0.0 received=700 decoded=700 handed_to_obs=690 dropped=7 no_route_dropped=1",
        ];
        expect(parseStatsVideoDropped(lines)).toBe(7);
    });

    it("1 行も無ければ null (40 秒の時間切れのように測れなかったことを区別する)", () => {
        expect(parseStatsVideoDropped([])).toBe(null);
        expect(parseStatsVideoDropped(undefined)).toBe(null);
    });
});

describe("judgeBackpressure", () => {
    it("establishedReached:true かつ捨てた数が有れば ok:true。droppedByBackpressure と statsVideoDropped を両方記録する", () => {
        const data = { establishedReached: true, socketStats: { droppedBeforeEstablished: 0, droppedByBackpressure: 12 } };
        const lines = ["info: [cordmux] stats video received=100 decoded=100 handed_to_obs=90 dropped=10 no_route_dropped=0"];
        const r = judgeBackpressure(data, lines);
        expect(r).toMatchObject({ ok: true, establishedReached: true, droppedByBackpressure: 12, statsVideoDropped: 10, backpressureExercised: true });
    });

    it("establishedReached:false なら NG (40 秒の時間切れの再発)", () => {
        const r = judgeBackpressure({ establishedReached: false, socketStats: null }, []);
        expect(r.ok).toBe(false);
        expect(r.reason).toContain("接続 (established) に至らなかった");
        expect(r.droppedByBackpressure).toBe(null);
        expect(r.statsVideoDropped).toBe(null);
    });

    it("productResultData が null (harness が結果を書けなかった) でも NG のまま落ちず、establishedReached:false とは別の理由になる", () => {
        const r = judgeBackpressure(null, []);
        expect(r.ok).toBe(false);
        expect(r.establishedReached).toBe(false);
        expect(r.reason).toContain("結果 JSON が読めなかった");
    });

    it("established しても送信側/受信側のどちらにも捨てた数が無ければ NG (背圧が一度も働いていない)", () => {
        const data = { establishedReached: true, socketStats: { droppedBeforeEstablished: 0, droppedByBackpressure: 0 } };
        const r = judgeBackpressure(data, []);
        expect(r.ok).toBe(false);
        expect(r.establishedReached).toBe(true);
        expect(r.backpressureExercised).toBe(false);
        expect(r.reason).toContain("捨てた数がない");
    });
});

describe("judgeConnectionHeld", () => {
    const HELLO = "info: [cordmux] hello: 合言葉が一致した。接続を確立した";
    const CLOSE = "info: [cordmux] close: 接続が切れた";
    const STATS = "info: [cordmux] stats video received=10 decoded=10 handed_to_obs=10 dropped=0 no_route_dropped=0";

    it("切り替え前後で hello / close が増えず、切り替えも確認できていれば ok:true", () => {
        const r = judgeConnectionHeld({ beforeLines: [HELLO], afterLines: [HELLO, STATS], switchApplied: true });
        expect(r).toMatchObject({ ok: true, helloEstablishedCount: 1, closeCount: 0, switchApplied: true });
    });

    it("wait-connection が hello を見つけられなかった (beforeLines がない) なら ok:false (切り替え前の確立を確認できていない)", () => {
        const r = judgeConnectionHeld({ beforeLines: null, afterLines: [], switchApplied: true });
        expect(r.ok).toBe(false);
        expect(r.reason).toContain("確認できないまま進んだ");
        expect(r.helloEstablishedCount).toBe(null);
    });

    it("switchApplied:false なら ok:false (SetCurrentSceneCollection が失敗した/切り替わったことを確認できなかった)", () => {
        const r = judgeConnectionHeld({ beforeLines: [HELLO], afterLines: [HELLO], switchApplied: false });
        expect(r.ok).toBe(false);
        expect(r.reason).toContain("切り替え自体が確認できなかった");
        expect(r.switchApplied).toBe(false);
    });

    it("切り替えの間に hello が増えていれば ok:false (繋ぎ直しの疑い)", () => {
        const r = judgeConnectionHeld({ beforeLines: [HELLO], afterLines: [HELLO, CLOSE, HELLO], switchApplied: true });
        expect(r.ok).toBe(false);
        expect(r.reason).toContain("hello が新たに記録された");
        expect(r.helloEstablishedCount).toBe(2);
    });

    it("切り替えの間に close が増えていれば ok:false (接続が切れた疑い。hello は増えなくても NG)", () => {
        const r = judgeConnectionHeld({ beforeLines: [HELLO], afterLines: [HELLO, CLOSE], switchApplied: true });
        expect(r.ok).toBe(false);
        expect(r.reason).toContain("close が新たに記録された");
        expect(r.closeCount).toBe(1);
    });

    // 英語のログの行。以前の文言のログと同じに数え、ハートビートの切れの行
    // ("Disconnected from Vesktop: it stopped responding") は close に数えない (以前の文言の close と同じ範囲)。
    const HELLO_NOW = "19:20:01.123: [cordmux] Connected to Vesktop";
    const CLOSE_NOW = "19:20:05.000: [cordmux] Disconnected from Vesktop\r";
    const HEARTBEAT_NOW = "19:20:09.000: [cordmux] Disconnected from Vesktop: it stopped responding";

    it("現在の文言: 切り替え前後で増えなければ ok:true", () => {
        const r = judgeConnectionHeld({ beforeLines: [HELLO_NOW], afterLines: [HELLO_NOW, STATS], switchApplied: true });
        expect(r).toMatchObject({ ok: true, helloEstablishedCount: 1, closeCount: 0 });
    });

    it("現在の文言: close が増えれば ok:false。ハートビートの切れの行は close に数えない", () => {
        const r = judgeConnectionHeld({
            beforeLines: [HELLO_NOW],
            afterLines: [HELLO_NOW, CLOSE_NOW, HEARTBEAT_NOW],
            switchApplied: true,
        });
        expect(r.ok).toBe(false);
        expect(r.closeCount).toBe(1);
    });

    it("以前の文言と現在の文言が混ざったログも両方を数える", () => {
        const r = judgeConnectionHeld({ beforeLines: [HELLO], afterLines: [HELLO, CLOSE_NOW, HELLO_NOW], switchApplied: true });
        expect(r.helloEstablishedCount).toBe(2);
        expect(r.closeCount).toBe(1);
    });
});

describe("encodeFactsFromHarnessData", () => {
    // --dock-scenario degraded の確認が結果 JSON から
    // 読む値 (productResult.data の streams[].encodedFormat と factsSent.stages)。フィールド名は
    // test/tools/harness/sender.ts の HarnessRunResult / HarnessFacts のとおり。

    it("productResultData が null (harness が結果を書けなかった) なら hasData:false で、配列は空・requested は null", () => {
        const r = encodeFactsFromHarnessData(null);
        expect(r).toEqual({ hasData: false, requested: { maxLongEdge: null, failStage: null }, configure: [], stages: [] });
    });

    it("画面 1 本が段を確保できている (encodedFormat 有り) 場合、configure に寸法・fps が入り stages は factsSent.stages をそのままコピーする", () => {
        const data = {
            config: { maxLongEdge: 640, failStage: 3, nScreen: 1, nVoice: 0 },
            streams: [{ kind: "screen", streamId: 1, encodedFormat: { width: 360, height: 640, fps: 30 } }],
            factsSent: { warnings: [], sharing: { count: 0, user_ids: [] }, stages: [{ stream_id: 1, stage: "software" }] },
        };
        const r = encodeFactsFromHarnessData(data);
        expect(r).toEqual({
            hasData: true,
            requested: { maxLongEdge: 640, failStage: 3 },
            configure: [{ streamId: 1, width: 360, height: 640, fps: 30 }],
            stages: [{ stream_id: 1, stage: "software" }],
        });
    });

    it("段がまだ決まっていない/Unavailable (encodedFormat が null) なら configure の width/height/fps は null (暫定値で埋めない)", () => {
        const data = {
            config: { maxLongEdge: 640, failStage: 0 },
            streams: [{ kind: "screen", streamId: 1, encodedFormat: null }],
            factsSent: null,
        };
        const r = encodeFactsFromHarnessData(data);
        expect(r.configure).toEqual([{ streamId: 1, width: null, height: null, fps: null }]);
        expect(r.stages).toEqual([]); // factsSent がない (established に至らなかった等) なら stages も空
    });

    it("声の本 (kind:voice) は configure に含めない (段も寸法も持たないため)", () => {
        const data = {
            config: {},
            streams: [
                { kind: "screen", streamId: 1, encodedFormat: { width: 1920, height: 1080, fps: 60 } },
                { kind: "voice", streamId: 2, encodedFormat: null },
            ],
            factsSent: { warnings: [], sharing: { count: 0, user_ids: [] }, stages: [{ stream_id: 1, stage: "native" }] },
        };
        const r = encodeFactsFromHarnessData(data);
        expect(r.configure).toEqual([{ streamId: 1, width: 1920, height: 1080, fps: 60 }]);
    });

    it("config.maxLongEdge / config.failStage が数でなければ requested は null (未指定の場合、送信側の既定 (failStage=0) を根拠のない値で埋めない)", () => {
        const r = encodeFactsFromHarnessData({ config: {}, streams: [], factsSent: undefined });
        expect(r.requested).toEqual({ maxLongEdge: null, failStage: null });
    });
});

describe("judgeOutputSkipped", () => {
    const sample = (skipped: number, total: number, renderSkipped = 0, cpu = 10) => ({
        outputSkippedFrames: skipped,
        outputTotalFrames: total,
        renderSkippedFrames: renderSkipped,
        renderTotalFrames: total,
        cpuUsage: cpu,
    });

    it("2 点の差分の割合で判定する (起動時の立ち上がりを含めない定常の規則)", () => {
        // 1 回目までに 100 フレーム落ちていても、区間の差分が 0 なら合格。
        const r = judgeOutputSkipped(sample(100, 900), sample(100, 72900));
        expect(r.ok).toBe(true);
        expect(r.outputSkippedDelta).toBe(0);
        expect(r.outputTotalDelta).toBe(72000);
        expect(r.ratio).toBe(0);
    });

    it("閾値は 0.02% で、境目のちょうどは合格、超えたら失敗", () => {
        expect(OUTPUT_SKIPPED_RATIO_THRESHOLD).toBe(0.0002);
        // 区間 100000 フレームに対し 20 フレーム = 0.02% ちょうど。
        const onEdge = judgeOutputSkipped(sample(0, 0), sample(20, 100000));
        expect(onEdge.ok).toBe(true);
        expect(onEdge.ratio).toBe(0.0002);

        const over = judgeOutputSkipped(sample(0, 0), sample(21, 100000));
        expect(over.ok).toBe(false);
        expect(over.reason).toContain("閾値");
    });

    it("閾値は呼び出し元が変えられる (既定の閾値を変えるときのため)", () => {
        const r = judgeOutputSkipped(sample(0, 0), sample(21, 100000), 0.001);
        expect(r.ok).toBe(true);
        expect(r.threshold).toBe(0.001);
    });

    it("標本が揃わない・数でないときは失敗 (割合 0 として成功にしない)", () => {
        expect(judgeOutputSkipped(null, sample(0, 100)).ok).toBe(false);
        expect(judgeOutputSkipped(sample(0, 100), null).ok).toBe(false);
        expect(judgeOutputSkipped({ outputSkippedFrames: 0 }, sample(0, 100)).ok).toBe(false);
        expect(judgeOutputSkipped({ ...sample(0, 100), outputTotalFrames: "many" }, sample(0, 200)).ok).toBe(false);
    });

    it("区間に出力のフレームが 1 つも無ければ失敗 (配信が始まっていない)", () => {
        const r = judgeOutputSkipped(sample(0, 1000), sample(0, 1000));
        expect(r.ok).toBe(false);
        expect(r.reason).toContain("1 つもない");
    });

    it("計数が 2 回目で減っていれば失敗 (出力が区間の途中で作り直された)", () => {
        const r = judgeOutputSkipped(sample(0, 5000), sample(0, 100));
        expect(r.ok).toBe(false);
        expect(r.reason).toContain("減っている");
    });

    it("renderTotalFrames がなくても判定できる (必須の値に含まれないため)", () => {
        const first = { outputSkippedFrames: 0, outputTotalFrames: 0, renderSkippedFrames: 0, cpuUsage: 5 };
        const last = { outputSkippedFrames: 1, outputTotalFrames: 60000, renderSkippedFrames: 2, cpuUsage: 30 };
        const r = judgeOutputSkipped(first, last);
        expect(r.ok).toBe(true);
        expect(r.renderTotalDelta).toBe(null);
        expect(r.cpuUsage).toEqual({ first: 5, last: 30 });
    });
});

// ---- 開始ゲートの判定 ------------------------------------------

describe("judgeVencordDirWritten", () => {
    const ours = "C:\\obs\\plugins\\cordmux\\data\\vencord";
    const before = JSON.stringify({ vencordDir: "C:/Users/x/AppData/Roaming/Vencord/dist", maximized: false, windowBounds: { x: 1, y: 2 } });

    it("vencordDir がテスト用にインストールしたファイルを指し、他の鍵が変わっていなければ成功", () => {
        const after = JSON.stringify({ vencordDir: "C:/obs/plugins/cordmux/data/vencord", maximized: false, windowBounds: { x: 1, y: 2 } });
        const r = judgeVencordDirWritten({ beforeText: before, afterText: after, expectedDir: ours });
        expect(r.ok).toBe(true);
        expect(r.changedKeys).toEqual([]);
    });

    it("区切りと大文字小文字の違いだけなら同じ場所と見なす", () => {
        const after = JSON.stringify({ vencordDir: "c:\\OBS\\plugins\\cordmux\\data\\vencord\\", maximized: false, windowBounds: { x: 1, y: 2 } });
        expect(judgeVencordDirWritten({ beforeText: before, afterText: after, expectedDir: ours }).ok).toBe(true);
    });

    it("他の鍵が 1 つでも変われば失敗 (製品は vencordDir だけを差し替えて書き戻す)", () => {
        const after = JSON.stringify({ vencordDir: "C:/obs/plugins/cordmux/data/vencord", maximized: true, windowBounds: { x: 1, y: 2 } });
        const r = judgeVencordDirWritten({ beforeText: before, afterText: after, expectedDir: ours });
        expect(r.ok).toBe(false);
        expect(r.changedKeys).toEqual(["maximized"]);
    });

    it("compareOtherKeys:false では他の鍵の変化を見ない (Vesktop は終了時にウィンドウの大きさを書き直す = call-gate-b)", () => {
        const after = JSON.stringify({ vencordDir: "C:/obs/plugins/cordmux/data/vencord", maximized: true, windowBounds: { x: 9, y: 9 } });
        const r = judgeVencordDirWritten({ beforeText: before, afterText: after, expectedDir: ours, compareOtherKeys: false });
        expect(r.ok).toBe(true);
    });

    it("vencordDir が別の場所を指していれば失敗 (終了時に上書きされた)", () => {
        const after = JSON.stringify({ vencordDir: "C:/Users/x/AppData/Roaming/Vencord/dist", maximized: false });
        const r = judgeVencordDirWritten({ beforeText: before, afterText: after, expectedDir: ours, compareOtherKeys: false });
        expect(r.ok).toBe(false);
        expect(r.reason).toContain("指していない");
    });

    it("鍵がない・文字列でない・JSON として読めないときは失敗", () => {
        expect(judgeVencordDirWritten({ beforeText: before, afterText: JSON.stringify({ maximized: false }), expectedDir: ours }).ok).toBe(false);
        expect(judgeVencordDirWritten({ beforeText: before, afterText: JSON.stringify({ vencordDir: 3 }), expectedDir: ours }).ok).toBe(false);
        expect(judgeVencordDirWritten({ beforeText: before, afterText: "{壊れている", expectedDir: ours }).ok).toBe(false);
        expect(judgeVencordDirWritten({ beforeText: before, afterText: "[]", expectedDir: ours }).ok).toBe(false);
    });

    it("書き換える前のコピーが無ければ失敗 (比べる相手がないまま成功にしない)", () => {
        const after = JSON.stringify({ vencordDir: "C:/obs/plugins/cordmux/data/vencord", maximized: false });
        const r = judgeVencordDirWritten({ beforeText: null, afterText: after, expectedDir: ours });
        expect(r.ok).toBe(false);
        expect(r.reason).toContain(STATE_JSON_BEFORE_NAME);
    });
});

// ---- 配信中に触った操作の数 -----------------------------------

describe("judgeOperationCount", () => {
    const results = [
        { name: "dock-click", ok: true }, // [Vesktop に追加] (配信より前)
        { name: "dock-click", ok: true, slotOp: true }, // 枠へ入れる (配信より前)
        { name: "record-start", stream: true },
        { name: "dock-click", ok: true, slotOp: true }, // 付け外し
        { name: "obsws-request", request: "GetSceneItemList", ok: true },
        { name: "screenshot-window", ok: true },
        { name: "dock-click", ok: true, muteOp: true }, // 声のミュート (ドックの行)
        { name: "dock-click", ok: true, muteOp: true },
        { name: "stream-stop", ok: true },
        { name: "obsws-request", request: "SetCurrentSceneCollection", ok: true }, // シーンコレクションの切り替え (配信の後)
    ];

    it("配信中の操作だけを種別ごとに数える (読むだけの呼び出しと PNG は操作に数えない)", () => {
        const r = judgeOperationCount(results);
        expect(r.ok).toBe(true);
        expect(r.counts).toEqual({ slot: 1, volume: 0, mute: 2, other: 0 });
        expect(r.duringCount).toBe(3);
        // 配信より前と配信を止めた後を混ぜない (before の名前と中身を食い違わせない)。
        expect(r.beforeCount).toBe(2);
        expect(r.before.map((x: { kind: string }) => x.kind)).toEqual(["other", "slot"]);
        expect(r.afterCount).toBe(1);
        expect(r.after.map((x: { label: string }) => x.label)).toEqual(["SetCurrentSceneCollection"]);
    });

    it("ウィンドウのコンポーネントを押した dialog-click (メニュー・設定ウィンドウ) は「その他」、読むだけ (-ListOnly) は数えない", () => {
        const r = judgeOperationCount([
            { name: "record-start" },
            { name: "dialog-click", ok: true, pressed: true },
            { name: "dialog-click", ok: true, pressed: false, listOnly: true },
        ]);
        expect(r.counts.other).toBe(1);
        expect(r.duringCount).toBe(1);
        expect(r.ok).toBe(false);
    });

    it("dock-click は、行のチェック (slotOp のフラグ) だけを付け外し、行のミュートのボタン (muteOp のフラグ) だけをミュートと数える", () => {
        const slot = judgeOperationCount([
            { name: "record-start" },
            { name: "dock-click", elementName: "harness-screen-1", slotOp: true, ok: true },
        ]);
        expect(slot.counts).toEqual({ slot: 1, volume: 0, mute: 0, other: 0 });
        expect(slot.ok).toBe(true);

        const other = judgeOperationCount([
            { name: "record-start" },
            { name: "dock-click", textId: TEXT_ID_ADD_TO_VESKTOP, slotOp: false, ok: true },
        ]);
        expect(other.counts.other).toBe(1);
        expect(other.ok).toBe(false);

        const mute = judgeOperationCount([
            { name: "record-start" },
            { name: "dock-click", textId: "cordmux.dock.row_voice_mute.a11y", muteOp: true, ok: true },
        ]);
        expect(mute.counts).toEqual({ slot: 0, volume: 0, mute: 1, other: 0 });
        expect(mute.ok).toBe(true);
    });

    it("配信中に付け外し・音量・ミュート以外の操作があれば失敗", () => {
        const withExtra = [...results.slice(0, 8), { name: "obsws-request", request: "SetInputName", ok: true }, ...results.slice(8)];
        const r = judgeOperationCount(withExtra);
        expect(r.ok).toBe(false);
        expect(r.counts.other).toBe(1);
    });

    it("record-start が無ければ失敗 (配信中の区間が定まらない)", () => {
        const r = judgeOperationCount([{ name: "dock-click", ok: true }]);
        expect(r.ok).toBe(false);
        expect(r.reason).toContain("record-start");
    });

    it("stream-stop が無ければ、record-start から最後までを配信中と見なす", () => {
        const r = judgeOperationCount(results.filter((x) => x.name !== "stream-stop"));
        expect(r.counts.other).toBe(1); // SetCurrentSceneCollection が配信中に入る
        expect(r.ok).toBe(false);
    });
});

// ---- 録画 (--step record) ----------------------------------------------------------
// --step record と外せない順序、判定ツール 3 つ (frames.py は使わない)、保存先の差し替えと空き容量、
// テスト用の送信側の解像度の変更と繋ぎ直し、webcodecs-check。

describe("buildGapFrames (録画ログの穴をフレーム番号に修正する)", () => {
    const segment = (fpsNum: number, fpsDen: number, userId = "u1", media = "video") => ({
        type: "segment",
        user_id: userId,
        media,
        fps_num: fpsNum,
        fps_den: fpsDen,
    });

    it("start_us / duration_us を区間の行の fps でフレーム番号に修正する", () => {
        const rows = { segment: [segment(60, 1)] };
        const finalize = { user_id: "u1", gaps: [{ start_us: 1_000_000, duration_us: 500_000 }] };
        expect(buildGapFrames(rows as any, finalize as any)).toEqual({ fps: 60, frames: [{ start_frame: 60, frames: 30 }] });
    });

    it("fps は有理数のまま扱う (59.94 を 60 と読まない)", () => {
        const rows = { segment: [segment(60000, 1001)] };
        const finalize = { user_id: "u1", gaps: [{ start_us: 1_000_000, duration_us: 1_000_000 }] };
        const got = buildGapFrames(rows as any, finalize as any);
        expect(got.frames).toEqual([{ start_frame: 60, frames: 60 }]);
        expect(got.fps).toBeCloseTo(59.94, 2);
    });

    it("その人の映像の区間の行だけを見る (別人と音の行の fps を拾わない)", () => {
        const rows = { segment: [segment(30, 1, "u2"), segment(48000, 1, "u1", "audio"), segment(60, 1, "u1")] };
        const finalize = { user_id: "u1", gaps: [{ start_us: 0, duration_us: 1_000_000 }] };
        expect(buildGapFrames(rows as any, finalize as any).fps).toBe(60);
    });

    it("fps を決められない録画ログは失敗にする (60 と決め打ちして成功にしない)", () => {
        const rows = { segment: [segment(0, 0)] };
        const finalize = { user_id: "u1", gaps: [{ start_us: 0, duration_us: 1_000_000 }] };
        expect(buildGapFrames(rows as any, finalize as any).error).toContain("fps");
        expect(buildGapFrames({ segment: [] } as any, null as any).error).toBeTruthy();
    });

    it("長さのない穴は落とす (黒の区間として拾えないものを突き合わせの相手にしない)", () => {
        const rows = { segment: [segment(60, 1)] };
        const finalize = {
            user_id: "u1",
            gaps: [
                { start_us: 0, duration_us: 0 },
                { start_us: 2_000_000, duration_us: 1_000_000 },
            ],
        };
        expect(buildGapFrames(rows as any, finalize as any).frames).toEqual([{ start_frame: 120, frames: 60 }]);
    });
});

describe("leadingGapFrames (no-leading-black の --allow-leading)", () => {
    const rows = { segment: [{ type: "segment", user_id: "u1", media: "video", fps_num: 60, fps_den: 1 }] };

    it("録画ログに先頭の穴があればその長さ (start_us 0 の穴が 10 フレーム)", () => {
        const finalize = {
            user_id: "u1",
            gaps: [
                { start_us: 0, duration_us: 166_667 },
                { start_us: 21_133_333, duration_us: 700_000 },
            ],
        };
        expect(leadingGapFrames(rows as any, finalize as any)).toBe(10);
    });

    it("先頭の穴が無ければ 0 (頭の黒を 1 フレームも許さない)", () => {
        const finalize = { user_id: "u1", gaps: [{ start_us: 1_000_000, duration_us: 500_000 }] };
        expect(leadingGapFrames(rows as any, finalize as any)).toBe(0);
    });

    it("録画ログが読めない・fps が決められないときも 0 (見ていないものを理由に黒を通さない)", () => {
        expect(leadingGapFrames(rows as any, null as any)).toBe(0);
        const finalize = { user_id: "u1", gaps: [{ start_us: 0, duration_us: 1_000_000 }] };
        expect(leadingGapFrames({ segment: [] } as any, finalize as any)).toBe(0);
    });
});

describe("画面のテキストは ini の id と textRef で持つ (テキストの値をツールに書きコピーしない)", () => {
    const argsFor = (step: string, overrides: Record<string, unknown> = {}) => Object.assign(parseArgs(argvFor(step)).args, overrides);
    // 押せるコンポーネントの Name は ja-JP.ini の値そのもの。実行スクリプトは id を持ち、run-window.mjs が ini から引く。
    // テキストを定数に書きコピーすると、ini の書き方が変わるたびにツールも修正する必要があるため、id と textRef で持つ。
    it("cordmuxの録画のボタンは向きで名前が変わり、行き先の名前で押す (2 つの id のテキストは違う)", () => {
        const values = jaIniValues();
        expect(values.get(TEXT_ID_RECORD_START)).toBeTruthy();
        expect(values.get(TEXT_ID_RECORD_STOP)).toBeTruthy();
        expect(values.get(TEXT_ID_RECORD_START)).not.toBe(values.get(TEXT_ID_RECORD_STOP));
    });

    it("textRef は ini のテキストに置き換わり、引けない id は missing に残って textRef もそのまま残る (黙って空にしない)", () => {
        const values = jaIniValues();
        const lookup = (id: string) => values.get(id) ?? null;
        const ok = expandTextRefs("[" + textRef(TEXT_ID_ADD_TO_VESKTOP) + "] を押す", lookup);
        expect(ok.missing).toEqual([]);
        expect(ok.text).toBe("[" + values.get(TEXT_ID_ADD_TO_VESKTOP) + "] を押す");
        const ng = expandTextRefs("x " + textRef("cordmux.no.such.key") + " y", lookup);
        expect(ng.missing).toEqual(["cordmux.no.such.key"]);
        expect(ng.text).toBe("x " + textRef("cordmux.no.such.key") + " y");
        const deep = expandTextRefsDeep({ a: [textRef(TEXT_ID_ADD_TO_VESKTOP)], b: 1, c: { d: textRef("cordmux.no.such.key") } }, lookup);
        expect(deep.value).toEqual({ a: [values.get(TEXT_ID_ADD_TO_VESKTOP)], b: 1, c: { d: textRef("cordmux.no.such.key") } });
        expect(deep.missing).toEqual(["cordmux.no.such.key"]);
        expect(textRefIds(textRef("a.b") + textRef("c.d") + textRef("a.b"))).toEqual(["a.b", "c.d"]);
    });

    it("stripQtMnemonic は Qt の読み上げの名前と同じく & を外す (&& は & 1 つ、末尾の & は消える)", () => {
        expect(stripQtMnemonic("ツール(&T)")).toBe("ツール(T)");
        expect(stripQtMnemonic("&Tools")).toBe("Tools");
        expect(stripQtMnemonic("A && B")).toBe("A & B");
        expect(stripQtMnemonic("&&&x")).toBe("&x");
        expect(stripQtMnemonic("end&")).toBe("end");
        expect(stripQtMnemonic("cordmuxをアンインストール...")).toBe("cordmuxをアンインストール...");
        expect(stripQtMnemonic(null)).toBe(null);
    });

    it("製品の ini は & を 1 つも持たない (stripQtMnemonic が製品のテキストを変えない前提)", () => {
        for (const [key, value] of jaIniValues()) expect(value.includes("&"), key).toBe(false);
    });

    it("lookupObsText は ja-JP を先に、無ければ en-US のテキストを返す (OBS のフォールバックと同じ)", () => {
        expect(OBS_LOCALE_FALLBACK_ORDER).toEqual(["ja-JP", "en-US"]);
        const ja = { Cancel: "キャンセル", "Basic.MainMenu.Tools": "ツール(&T)" };
        const en = { OK: "OK", Cancel: "Cancel", "Basic.MainMenu.Tools": "&Tools" };
        expect(lookupObsText([ja, en], "OK")).toBe("OK");
        expect(lookupObsText([ja, en], "Cancel")).toBe("キャンセル");
        expect(stripQtMnemonic(lookupObsText([ja, en], "Basic.MainMenu.Tools"))).toBe("ツール(T)");
        expect(lookupObsText([ja, en], "No.Such.Key")).toBe(null);
        expect(lookupObsText([{ OK: "" }, en], "OK")).toBe("OK");
    });

    // 手元に展開した OBS (_work。repo にはない) があるときだけ、実際の ini で確認する。
    const obsLocaleDirs = ["obs-33.0.1", "obs-32.2.2"]
        .map((v) => join(dirname(fileURLToPath(import.meta.url)), "..", "..", "..", "_work", v, "data", "obs-studio", "locale"))
        .filter((dir) => existsSync(join(dir, "ja-JP.ini")) && existsSync(join(dir, "en-US.ini")));
    it.skipIf(obsLocaleDirs.length === 0)("手元の OBS の ini: ja-JP.ini に OK がなく、en-US.ini へ落とすと [OK] と [ツール(T)] が引ける", () => {
        const readIni = (file: string) => {
            const out: Record<string, string> = {};
            for (const raw of readFileSync(file, "utf8").replace(/^\uFEFF/, "").split(/\r?\n/)) {
                const eq = raw.indexOf("=");
                if (eq < 0 || raw.trim().startsWith("#")) continue;
                out[raw.slice(0, eq).trim()] = raw.slice(eq + 1).trim().replace(/^"(.*)"$/, "$1");
            }
            return out;
        };
        for (const dir of obsLocaleDirs) {
            const inOrder = OBS_LOCALE_FALLBACK_ORDER.map((locale) => readIni(join(dir, locale + ".ini")));
            expect(inOrder[0].OK, dir).toBeUndefined();
            for (const id of [OBS_TEXT_OK, OBS_TEXT_CANCEL, OBS_TEXT_APPLY, OBS_TEXT_TOOLS_MENU]) {
                expect(lookupObsText(inOrder, id.slice(OBS_TEXT_PREFIX.length)), dir + " " + id).toBeTruthy();
            }
            expect(stripQtMnemonic(lookupObsText(inOrder, OBS_TEXT_TOOLS_MENU.slice(OBS_TEXT_PREFIX.length)))).toBe("ツール(T)");
        }
    });

    it("アンインストールの入口は 2 つで、[ツール] の項目はページのボタンと同じテキスト (押すコンポーネントの種類とウィンドウで見分ける)", () => {
        expect(UNINSTALL_ENTRY_VALUES).toEqual(["tools", "settings"]);
        const values = jaIniValues();
        expect(values.get(TEXT_ID_TOOLS_MENU_UNINSTALL)).toBeTruthy();
        expect(values.get(TEXT_ID_TOOLS_MENU_UNINSTALL)).toBe(values.get(TEXT_ID_SETTINGS_UNINSTALL));
    });

    it("stepTextIds は名指しの欄・判定の文言 (1 つか並び)・人への文と要求の中の textRef を全部拾う", () => {
        expect(
            stepTextIds({
                kind: "x",
                textId: "a.a",
                titleTextId: "b.b",
                buttonTextId: OBS_TEXT_OK,
                inputs: { names: { textPrefix: ["c.c", "d.d"] } },
                message: textRef("e.e"),
                requestData: { sceneName: textRef("f.f") },
            }),
        ).toEqual(["a.a", "b.b", OBS_TEXT_OK, "c.c", "d.d", "e.e", "f.f"]);
        expect(OBS_TEXT_OK.startsWith(OBS_TEXT_PREFIX)).toBe(true);
    });

    it("全部の --step の実行計画が名指す製品のテキストの id は ja-JP.ini にあり、OBS 自身のテキストは [OK] [キャンセル] [適用] [ツール] だけ", () => {
        const values = jaIniValues();
        const plans = [
            ...STEP_VALUES.filter((x) => x !== "install-roundtrip").map((stepName) => planSteps(parseArgs(argvFor(stepName)).args).steps),
            ...DOCK_SCENARIO_VALUES.map((sc) => planSteps(argsFor("dock-view", { dockScenario: sc })).steps),
        ];
        for (const steps of plans) {
            for (const st of steps) {
                for (const id of stepTextIds(st)) {
                    if (id.startsWith(OBS_TEXT_PREFIX)) expect([OBS_TEXT_OK, OBS_TEXT_CANCEL, OBS_TEXT_APPLY, OBS_TEXT_TOOLS_MENU]).toContain(id);
                    else expect(values.has(id), id).toBe(true);
                }
            }
        }
    });

    it("ドックのタブ・ドックの設定・確認のウィンドウの id を 1 つも名指さない (ドックの座標を押す操作も使わない)", () => {
        const plans = [
            ...STEP_VALUES.filter((x) => x !== "install-roundtrip").map((stepName) => planSteps(parseArgs(argvFor(stepName)).args).steps),
            ...DOCK_SCENARIO_VALUES.map((sc) => planSteps(argsFor("dock-view", { dockScenario: sc })).steps),
        ];
        for (const steps of plans) {
            for (const st of steps) {
                expect(st.kind).not.toBe("dock-tab");
                expect(st.kind).not.toBe("dock-click-at");
                for (const id of stepTextIds(st)) expect(id).not.toMatch(/^cordmux\.dock\.(action|settings|status|dialog|people|seen)\./);
            }
        }
    });
});

describe("--step record の実行計画", () => {
    function recordPlan(overrides: Record<string, unknown> = {}) {
        const r = parseArgs(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "record"]);
        expect(r.ok).toBe(true);
        const p = planSteps(Object.assign(r.args, overrides));
        expect(p.ok).toBe(true);
        return p.steps;
    }
    const idx = (steps: any[], kind: string) => steps.findIndex((s) => s.kind === kind);
    const lastIdx = (steps: any[], kind: string) => steps.map((s) => s.kind).lastIndexOf(kind);

    it("wait-finalize は close-obs と cleanup より前にある (cleanup.ps1 が _work の ffmpeg.exe を止めるため)", () => {
        const steps = recordPlan();
        const finalize = idx(steps, "wait-finalize");
        expect(finalize).toBeGreaterThanOrEqual(0);
        expect(finalize).toBeLessThan(idx(steps, "close-obs"));
        expect(finalize).toBeLessThan(idx(steps, "cleanup"));
        // 判定ツールも録画ファイルの作成の後に来る (完成がないと判定できない)。
        expect(finalize).toBeLessThan(idx(steps, "judge-record"));
    });

    it("録画の ON は接続を確認してから押す (途中 ON)。OFF は空き容量の撮影より後", () => {
        const steps = recordPlan();
        const clicks = steps.filter((s) => s.kind === "dock-click");
        // 1 回目の ON と OFF、同じ OBS の起動の中での 2 回目の ON と OFF。
        expect(clicks).toHaveLength(4);
        for (const pair of [[clicks[0], clicks[1]], [clicks[2], clicks[3]]]) {
            expect(pair[0]).toMatchObject({ textId: TEXT_ID_RECORD_START, desired: "on" });
            expect(pair[1]).toMatchObject({ textId: TEXT_ID_RECORD_STOP, desired: "off", namePrefix: true });
            expect(pair[0].namePrefix).toBeUndefined();
        }
        const onIdx = steps.findIndex((s) => s.kind === "dock-click" && s.desired === "on");
        const offIdx = steps.findIndex((s) => s.kind === "dock-click" && s.desired === "off");
        expect(idx(steps, "wait-connection")).toBeLessThan(onIdx);
        // 途中 ON なので、ON の前に「実映像を貯める待ち」が 1 つ挟まる。
        expect(idx(steps, "wait")).toBeLessThan(onIdx);
        // 空き容量の警告は録画中にしか出ない ので OFF より前に撮る。
        const spaceShot = steps.findIndex((s) => s.kind === "screenshot-window" && s.shotLabel === "record-space");
        expect(spaceShot).toBeGreaterThanOrEqual(0);
        expect(spaceShot).toBeLessThan(offIdx);
        expect(offIdx).toBeLessThan(idx(steps, "wait-finalize"));
    });

    it("OBS 側の録画はテスト用の送信側より前に始め、OFF の後に止める (対の判定の相方)", () => {
        const steps = recordPlan();
        expect(idx(steps, "record-start")).toBeLessThan(idx(steps, "harness-start"));
        const offIdx = steps.findIndex((s) => s.kind === "dock-click" && s.desired === "off");
        expect(offIdx).toBeLessThan(idx(steps, "record-stop"));
        expect(idx(steps, "record-stop")).toBeLessThan(idx(steps, "wait-finalize"));
    });

    it("判定ツールは 3 つ (ledger_check / au_hash / black_span を 3 通り)。frames.py は 1 つも使わない", () => {
        const steps = recordPlan();
        expect(steps.filter((s) => s.kind === "judge")).toHaveLength(0);
        const tools = steps.filter((s) => s.kind === "judge-record").map((s) => s.tool);
        expect(tools).toEqual(["ledger_check.py", "au_hash.py", "black_span.py", "black_span.py", "black_span.py"]);
        const blacks = steps.filter((s) => s.kind === "judge-record" && s.tool === "black_span.py");
        // 対の判定: cordmux は「先行する黒がない」と「黒が録画ログの穴と合う」、OBS は「黒がない」。
        expect(blacks[0]).toMatchObject({ target: "cordmux", mode: "no-leading-black" });
        expect(blacks[1]).toMatchObject({ target: "cordmux", mode: "match-gaps" });
        expect(blacks[2]).toMatchObject({ target: "obs", mode: "no-black" });
        // 録画ログの穴との一致は機械で判定する (判定ツールを作っただけで実行スクリプトから呼ばない形に戻らないこと)。
        expect(blacks.filter((b) => b.mode === "match-gaps")).toHaveLength(1);
        expect(blacks.every((b) => b.gate === true)).toBe(true);
    });

    it("--assign-mode manual を渡しても auto で seed する (手動だと OBS 側の録画が黒のままになる)", () => {
        const steps = recordPlan({ dockAssignMode: "manual" });
        expect(steps[0]).toMatchObject({ kind: "seed-config", assignMode: "auto" });
    });

    it("seed-config は録画の保存先を必ず渡し、--step record では空き容量の 2 鍵も渡す", () => {
        const steps = recordPlan();
        expect(steps[0]).toMatchObject({
            kind: "seed-config",
            recordDirRel: RECORD_DIR_REL,
            recordSpaceCheckIntervalS: RECORD_SPACE_CHECK_INTERVAL_S,
            recordSpaceWarnMinutes: RECORD_SPACE_WARN_MINUTES,
        });
    });

    it("seed-config を持つすべての --step が録画の保存先の差し替えを渡す (差し替えなしで組めない形)", () => {
        for (const stepName of STEP_VALUES.filter((s) => !STEPS_WITHOUT_SEED.includes(s))) {
            const callFlags = stepName.startsWith("call-") ? ["--call-window", "--real-profile"] : [];
            const r = parseArgs(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", stepName, ...callFlags]);
            expect(r.ok).toBe(true);
            const p = planSteps(r.args);
            expect(p.steps[0]).toMatchObject({ kind: "seed-config", recordDirRel: RECORD_DIR_REL });
        }
    });

    it("テスト用の送信側は、最初の 1 つ + 繋ぎ直し 2 回 + 2 回目の録画の 1 つ + 起こし直した後の 1 つ。解像度の予定表は最初の 1 つだけが持つ", () => {
        const steps = recordPlan();
        const starts = steps.filter((s) => s.kind === "harness-start");
        // 最初の 1 つ + 繋ぎ直し RECORD_RECONNECT_COUNT 回 + 2 回目の録画の 1 つ + 録画の OFF の判定に中身を持たせる 1 つ。
        expect(starts).toHaveLength(RECORD_RECONNECT_COUNT + 3);
        expect(starts.at(-2)!.seconds).toBe(RECORD_SECOND_SESSION_SECONDS);
        expect(starts.at(-2)!.extra).toBeUndefined();
        expect(starts[0]).toMatchObject({
            mode: "product",
            nScreen: RECORD_N_SCREEN,
            nVoice: RECORD_N_VOICE,
            seconds: RECORD_HARNESS_FIRST_SECONDS,
            syncTone: DEFAULT_SYNC_TONE,
        });
        expect(starts[0].extra).toMatchObject({ resolutionSchedule: resolutionScheduleArg(RECORD_RESOLUTION_SCHEDULE) });
        for (const s of starts.slice(1, RECORD_RECONNECT_COUNT + 1)) {
            expect(s.seconds).toBe(RECORD_HARNESS_RECONNECT_SECONDS);
            expect(s.extra).toBeUndefined();
        }
        expect(starts.at(-1)!.seconds).toBe(RECORD_AFTER_RESTART_SECONDS);
        expect(starts.at(-1)!.extra).toBeUndefined();
        // 起こし直しは「殺す」のではなく「自分で終わるのを待つ」形 (タイミング 3 のデータが要る)。
        expect(steps.filter((s) => s.kind === "harness-wait")).toHaveLength(RECORD_RECONNECT_COUNT + 3);
    });

    it("同じ OBS の起動の中で 2 回目の録画をし、1 回目の判定の後、OBS を閉じる前に判定する", () => {
        const steps = recordPlan();
        const ons = steps.map((s, i) => (s.kind === "dock-click" && s.desired === "on" ? i : -1)).filter((i) => i >= 0);
        expect(ons).toHaveLength(2);
        // 2 回目の ON は 1 回目の判定ツール (いちばん新しいセッションを読む) の後。
        expect(ons[1]).toBeGreaterThan(lastIdx(steps, "judge-record"));
        const second = idx(steps, "judge-record-second");
        expect(second).toBeGreaterThan(ons[1]);
        expect(steps[second]).toMatchObject({ recordDirRel: RECORD_DIR_REL, gate: true });
        // 2 回目の録画ファイルの作成を待ってから判定し、その後に OBS を閉じる。
        const finalizes = steps.map((s, i) => (s.kind === "wait-finalize" ? i : -1)).filter((i) => i >= 0);
        expect(finalizes).toHaveLength(2);
        expect(finalizes[1]).toBeGreaterThan(ons[1]);
        expect(finalizes[1]).toBeLessThan(second);
        expect(second).toBeLessThan(idx(steps, "close-obs"));
    });

    it("起こし直した OBS へもう 1 ストリームつないでから、OFF に戻っていることを撮って判定する", () => {
        const steps = recordPlan();
        expect(steps.filter((s) => s.kind === "launch-obs")).toHaveLength(2);
        expect(steps.filter((s) => s.kind === "close-obs")).toHaveLength(2);
        const secondLaunch = lastIdx(steps, "launch-obs");
        // **繋がずに見ると判定が必ず合格になる** (区間は最初の鍵フレームが来るまで開かない)。
        const lastStart = steps.map((s) => s.kind).lastIndexOf("harness-start");
        expect(lastStart).toBeGreaterThan(secondLaunch);
        const restartShot = steps.findIndex((s) => s.kind === "screenshot-window" && s.shotLabel === "record-after-restart");
        expect(restartShot).toBeGreaterThan(lastStart);
        const offJudge = idx(steps, "judge-record-off");
        expect(offJudge).toBeGreaterThan(restartShot);
        expect(offJudge).toBeLessThan(lastIdx(steps, "harness-wait"));
        expect(lastIdx(steps, "harness-wait")).toBeLessThan(lastIdx(steps, "close-obs"));
    });

    it("後始末は録画のフォルダを残す (-KeepRecordings)。他の --step は残さない", () => {
        // **これを外すと、cleanup.ps1 の *.mov の後片付けが完成ごと消す** (あとで別の確認が
        // 開く「同じテストデータ」がなくなる)。
        expect(recordPlan().at(-1)).toMatchObject({ kind: "cleanup", keepRecordings: true });
        for (const stepName of STEP_VALUES) {
            const r = parseArgs(argvFor(stepName));
            expect(r.ok).toBe(true);
            const last = planSteps(r.args).steps.at(-1);
            // call-1 も残す: 実行の中で録った完成ファイルと av1 / av2 を後で見る。call-hold も残す。
            expect(last).toMatchObject({ kind: "cleanup", keepRecordings: ["record", "call-1", "call-hold"].includes(stepName) });
        }
    });

    it("gate を付けるのは録画ファイルの作成の待ちと判定だけ (判定に届かないまま成功にしない)", () => {
        const steps = recordPlan();
        const gated = steps.filter((s) => s.gate).map((s) => s.kind);
        expect(gated).toEqual([
            "wait-finalize",
            "judge-record",
            "judge-record",
            "judge-record",
            "judge-record",
            "judge-record",
            "wait-finalize",
            "judge-record-second",
            "judge-record-off",
        ]);
    });
});

describe("解像度の予定表の綴り", () => {
    it("並びから綴りを作り、読み戻すと元に戻る", () => {
        const text = resolutionScheduleArg(RECORD_RESOLUTION_SCHEDULE);
        expect(text).toBe("35:1280x720,60:640x360");
        expect(parseResolutionSchedule(text)).toEqual(RECORD_RESOLUTION_SCHEDULE);
    });

    it("空の並びは空の綴り。空の綴りは空の並び", () => {
        expect(resolutionScheduleArg([])).toBe("");
        expect(parseResolutionSchedule("")).toEqual([]);
    });

    it("形が違えば null (黙って半分だけ読まない)", () => {
        expect(parseResolutionSchedule("35:1280")).toBeNull();
        expect(parseResolutionSchedule("35x1280:720")).toBeNull();
        expect(parseResolutionSchedule("a:1280x720")).toBeNull();
    });
});

describe("--step webcodecs-check", () => {
    function planFor(stepName: string) {
        const r = parseArgs(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", stepName]);
        expect(r.ok).toBe(true);
        const p = planSteps(r.args);
        expect(p.ok).toBe(true);
        return p.steps;
    }

    it("webcodecs-check は OBS もテスト用の送信側も起こさない", () => {
        const steps = planFor("webcodecs-check");
        expect(steps.map((s) => s.kind)).toEqual(["webcodecs-check", "write-result", "summarize", "cleanup"]);
        expect(steps[0].gate).toBe(true);
    });
});

describe("実際の通話の実行用の純関数 (枠の区画・音の切り分け・案内と映像停止の一致・OBS の録画・期限の見積もり)", () => {
    // このバージョンの OBS の一覧の名前 (x07「cordmux画面共有 %1 (空き)」と x08「%1 (cordmux画面共有 %2)」ほか)。テストの中で
    // 名前を組むだけのツールで、実行スクリプトは名前ではなく型 id と slot で探す (classifyCordmuxInputs)。
    const PERSON_FORMS = { screen: "%1 (cordmux画面共有 %2)", voice: "%1 (cordmux声 %2)" };
    const SN = (slot: number, person = "") => (person ? fillTemplate(PERSON_FORMS.screen, [person, String(slot)]) : "cordmux画面共有 " + slot + " (空き)");
    // 合成のソースの配置の行から取った区画の読み (compositeCellRegions と同じ形)。cells の四角は録画の上の値。
    const capture = (cells: { cell: number; x: number; y: number; w: number; h: number; bitCell?: number }[], regionSource = "composite-layout") => ({
        when: "start",
        ok: cells.length > 0,
        canvas: { width: 1920, height: 1080 },
        regionSource,
        cells: cells.map((c) => ({ bitCell: 8, ...c })),
        skipped: [],
    });
    // 640x360 の枠を 2x2 の象限の左上に置いた区画 3 つ。
    const call1Cells = [
        { cell: 0, x: 0, y: 0, w: 640, h: 360 },
        { cell: 1, x: 960, y: 0, w: 640, h: 360 },
        { cell: 2, x: 0, y: 540, w: 640, h: 360 },
    ];

    it("classifyCordmuxInputs: 型 id で cordmux のソースだけを拾い、枠の番号を持たないソースの slot は null、旧い型 id は拾わない", () => {
        const raw = [
            { inputName: "cordmux", inputKind: CORDMUX_SOURCE_KINDS.composite, inputUuid: "u1" },
            { inputName: "Mic/Aux", inputKind: "wasapi_input_capture" },
            { inputName: "old", inputKind: "cordmux_screen_source" },
            { inputName: "older", inputKind: "cordmux_person" },
        ];
        const list = classifyCordmuxInputs({ inputs: raw, settings: {}, personForms: PERSON_FORMS });
        expect(list.map((x) => [x.kind, x.slot, x.person])).toEqual([["composite", null, null]]);
        expect(findCordmuxInput(list, { kind: "composite" })!.inputName).toBe("cordmux");
        expect(findCordmuxInput(list, { kind: "person" })).toBeNull();
        expect(findCordmuxInput(list, { kind: "screen", slot: 1 })).toBeNull();
        // 人の名前に %2 や括弧が入っていても、置き換えは 1 回の走査 (scene_names.h の fill_template と同じ)。
        expect(fillTemplate("%1 (x %2)", ["%2", "7"])).toBe("%2 (x 7)");
        expect(personFromSlotName(SN(3, "%2"), 3, PERSON_FORMS.screen)).toBe("%2");
        expect(personFromSlotName(SN(3, "taro"), 4, PERSON_FORMS.screen)).toBeNull();
    });

    it("cellRegionsForJudge: 始めと終わりの重なりを使い、取れない区画は前の決め打ち (regionSource で分かる)", () => {
        const start = capture(call1Cells);
        // 終わりには 2 つ目の共有が大きくなっていた (960x540)。重なりは始めの 640x360。
        const end = capture([call1Cells[0], { cell: 1, x: 960, y: 0, w: 960, h: 540 }, call1Cells[2]]);
        const regions = cellRegionsForJudge({ captures: [start, end], cells: 4 });
        expect(regions.map((r) => [r.cell, r.x, r.y, r.w, r.h, r.bitCell, r.regionSource])).toEqual([
            [0, 0, 0, 640, 360, 8, "composite-layout"],
            [1, 960, 0, 640, 360, 8, "composite-layout"],
            [2, 0, 540, 640, 360, 8, "composite-layout"],
            // 4 つ目は枠がない = 前の決め打ちの象限 (黙って前の形に戻さない)。
            [3, 960, 540, 960, 540, 8, "assumed-quadrant"],
        ]);
        expect(regions[1].changed).toBe(true);
        expect(regions[0].changed).toBe(false);
        // 始めにはまだ区画が無かった (テスト用の送信側が繋ぐ前に録画を始める step) なら、終わりだけを使う。
        const empty = capture([]);
        const full = capture([{ cell: 0, x: 0, y: 0, w: 1920, h: 1080, bitCell: 16 }]);
        expect(cellRegionsForJudge({ captures: [empty, full], cells: 1 })).toEqual([
            { cell: 0, x: 0, y: 0, w: 1920, h: 1080, bitCell: 16, regionSource: "composite-layout", changed: false, captures: 1 },
        ]);
        // 何も読めなければ、枠 1 つは画面全体、2 つ以上は象限。
        expect(cellRegionsForJudge({ captures: [], cells: 1 })[0]).toMatchObject({ x: 0, y: 0, w: 1920, h: 1080, regionSource: "full-frame" });
        expect(cellRegionsForJudge({ captures: [{ ok: false, cells: [] }], cells: 2 }).map((r) => r.regionSource)).toEqual(["assumed-quadrant", "assumed-quadrant"]);
    });

    it("judgeCellArgs: frames.py は帯を探す四角と前の帯の升の一辺、sync.py は白を見る四角、smooth.py は 1 回で全部。決め打ちのときは前と同じ引数", () => {
        const regions = cellRegionsForJudge({ captures: [capture(call1Cells)], cells: 3 });
        expect(judgeCellArgs("sync.py", regions)).toEqual([
            ["--x", "0", "--y", "0", "--w", "640", "--h", "360"],
            ["--x", "960", "--y", "0", "--w", "640", "--h", "360"],
            ["--x", "0", "--y", "540", "--w", "640", "--h", "360"],
        ]);
        // frames.py は区画の中で時計つきの帯を探す (bit_strip.py)。区画の外 (隣の枠) は探さない。
        expect(judgeCellArgs("frames.py", regions)[1]).toEqual(["--x", "960", "--y", "0", "--w", "640", "--h", "360", "--cell", "8"]);
        expect(judgeCellArgs("frames.py", cellRegionsForJudge({ captures: [], cells: 2 }))[1]).toEqual(["--x", "960", "--y", "0", "--w", "960", "--h", "540", "--cell", "8"]);
        expect(judgeCellArgs("smooth.py", regions)).toEqual([["3", "--regions", "0,0,640,360;960,0,640,360;0,540,640,360", "--cell", "8"]]);
        // 決め打ち: 枠 1 つの画面全体は引数なし (前と同じ)、smooth.py は枠の数だけ (前と同じ)。
        expect(judgeCellArgs("frames.py", cellRegionsForJudge({ captures: [], cells: 1 }))).toEqual([[]]);
        expect(judgeCellArgs("smooth.py", cellRegionsForJudge({ captures: [], cells: 4 }))).toEqual([["4"]]);
        // 一部だけ取れた (3 つ目の共有がどちらの時点でもまだ区画がない) ときも、取れた区画は実際の位置で読む。
        const partial = cellRegionsForJudge({ captures: [capture(call1Cells.slice(0, 2))], cells: 3 });
        expect(partial.map((r) => r.regionSource)).toEqual(["composite-layout", "composite-layout", "assumed-quadrant"]);
        expect(judgeCellArgs("smooth.py", partial)).toEqual([["3", "--regions", "0,0,640,360;960,0,640,360;0,540,960,540", "--cell", "8"]]);
    });

    it("リハーサルの A/V の音の切り分けは、マーカーの音を乗せたテスト用の画面 1 つ目の画面共有の音のミュートを外す (ほかの画面共有の音は共有の始めからミュート)", () => {
        const r = parseArgs(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-1", "--rehearsal"]);
        const steps = planSteps(r.args).steps;
        const unmutes = steps.filter((s) => s.kind === "dock-click" && s.textId === "cordmux.dock.row_screen_mute.a11y" && s.desired === "off");
        expect(unmutes.map((s) => s.textArgs[0])).toEqual([REHEARSAL_AV_SCREEN_ACCOUNT, REHEARSAL_AV_SCREEN_ACCOUNT]);
    });

    it("parseVideoRateSamples: プラグインのログの stats video の直近の行だけを読み、どれも 1 フレーム/秒 未満なら止まっている", () => {
        const flowing = [
            "14:28:05.113: [cordmux] stats video received_hz=72.1 decoded_hz=72.1 handed_to_obs_hz=72.1 dropped_hz=0.0 received=170289",
            "14:28:05.113: [cordmux] stats audio received_hz=400.0 handed_to_obs_hz=400.0",
            "14:28:10.000: [cordmux] facts: roster=4 speaking=0 sharing=3",
            "14:28:15.114: [cordmux] stats video received_hz=73.7 decoded_hz=73.6",
        ];
        expect(parseVideoRateSamples(flowing)).toEqual({
            samples: [
                { at: "14:28:05.113", receivedHz: 72.1 },
                { at: "14:28:15.114", receivedHz: 73.7 },
            ],
            framesStopped: false,
            windowLines: VIDEO_RATE_WINDOW_LINES,
            stoppedHz: VIDEO_STOPPED_HZ,
        });
        const stopped = [...flowing, "14:28:25.000: [cordmux] stats video received_hz=0.0 decoded_hz=0.0", "14:28:35.000: [cordmux] stats video received_hz=0.4 decoded_hz=0.4"];
        expect(parseVideoRateSamples(stopped).framesStopped).toBe(true);
        // 止まりかけ (直近 2 行のうち 1 行はまだ届いている) は止まっていない。
        expect(parseVideoRateSamples([...flowing, "14:28:25.000: [cordmux] stats video received_hz=0.0"]).framesStopped).toBe(false);
        // 行が足りなければ分からない (null)。
        expect(parseVideoRateSamples(["14:28:25.000: [cordmux] stats video received_hz=0.0"]).framesStopped).toBeNull();
        expect(parseVideoRateSamples([]).framesStopped).toBeNull();
        // 現在の実装の先頭が大文字の行 (Stats) も同じに読む。
        const current = ["14:29:05.000: [cordmux] Stats video received_hz=0.2 decoded_hz=0.2", "14:29:15.000: [cordmux] Stats video received_hz=0.0 decoded_hz=0.0"];
        expect(parseVideoRateSamples(current).samples).toEqual([
            { at: "14:29:05.000", receivedHz: 0.2 },
            { at: "14:29:15.000", receivedHz: 0.0 },
        ]);
        expect(parseVideoRateSamples(current).framesStopped).toBe(true);
    });

    it("judgeNoticeIffFramesStopped: 案内が出ていることと映像が止まっていることが一致すれば合格。映像が止まらず案内もない場合も合格", () => {
        const names = ["画面共有の映像が"];
        const shown = ["状態", "画面共有の映像が届かなくなっています"];
        const hidden = ["状態", "配信中"];
        const flowing = { framesStopped: false, samples: [{ receivedHz: 72.1 }, { receivedHz: 74.8 }] };
        const stopped = { framesStopped: true, samples: [{ receivedHz: 0 }, { receivedHz: 0 }] };
        expect(judgeNoticeIffFramesStopped({ texts: hidden, names, videoRate: flowing })).toMatchObject({ ok: true, noticePresent: false, framesStopped: false, receivedHz: [72.1, 74.8] });
        expect(judgeNoticeIffFramesStopped({ texts: shown, names, videoRate: stopped })).toMatchObject({ ok: true, noticePresent: true, framesStopped: true });
        expect(judgeNoticeIffFramesStopped({ texts: shown, names, videoRate: flowing })).toMatchObject({ ok: false, noticePresent: true });
        expect(judgeNoticeIffFramesStopped({ texts: shown, names, videoRate: flowing }).reason).toContain("still arrives");
        expect(judgeNoticeIffFramesStopped({ texts: hidden, names, videoRate: stopped })).toMatchObject({ ok: false, noticePresent: false });
        // 読めていなければ合格にしない。
        expect(judgeNoticeIffFramesStopped({ texts: [], names, videoRate: flowing }).ok).toBe(false);
        expect(judgeNoticeIffFramesStopped({ texts: hidden, names: [], videoRate: flowing }).ok).toBe(false);
        expect(judgeNoticeIffFramesStopped({ texts: hidden, names, videoRate: null }).ok).toBe(false);
        expect(judgeNoticeIffFramesStopped({ texts: hidden, names, videoRate: { framesStopped: null, samples: [] } }).ok).toBe(false);
        // judge-rt から名指しできる。
        expect(ROUNDTRIP_JUDGE_NAMES).toContain("notice-iff-frames-stopped");
        expect(judgeRoundtrip("notice-iff-frames-stopped", { texts: hidden, names, videoRate: flowing }).ok).toBe(true);
    });

    it("obsRecordingMoves: label の付いた OBS の録画を recordings/obs-<tag>/<label>-<元の名前> へ (cleanup.ps1 -KeepRecordings が残す場所)", () => {
        const r = obsRecordingMoves({
            recordings: { main: "C:/w/out/window3b/rec 13-53-16.mkv", av1: "C:\\w\\out\\window3b\\rec 14-45-30.mkv", empty: "" },
            windowOutDir: "C:/w/out/window3b/",
            tag: "t1",
        });
        expect(OBS_RECORDINGS_KEEP_DIR_PREFIX.startsWith("recordings/")).toBe(true);
        expect(r.dir).toBe("C:/w/out/window3b/recordings/obs-t1");
        expect(r.moves).toEqual([
            { label: "main", from: "C:/w/out/window3b/rec 13-53-16.mkv", to: "C:/w/out/window3b/recordings/obs-t1/main-rec 13-53-16.mkv" },
            { label: "av1", from: "C:\\w\\out\\window3b\\rec 14-45-30.mkv", to: "C:/w/out/window3b/recordings/obs-t1/av1-rec 14-45-30.mkv" },
        ]);
        expect(obsRecordingMoves({ recordings: {}, windowOutDir: "C:/w", tag: "t" }).moves).toEqual([]);
    });

    it("OBS の録画を残すのは call-1 と call-hold と record だけ (keep-obs-recordings はその締めの結果を書く直前にだけ出る)", () => {
        expect(STEPS_KEEPING_OBS_RECORDINGS).toEqual(["call-1", "call-hold", "record"]);
        expect(STEPS_KEEPING_RECORDINGS).toContain("call-1");
        expect(STEPS_KEEPING_RECORDINGS).toContain("call-hold");
        for (const stepName of STEP_VALUES) {
            const r = parseArgs(argvFor(stepName));
            if (!r.ok) continue;
            const steps = planSteps(r.args).steps;
            expect(steps.some((s) => s.kind === "keep-obs-recordings")).toBe(STEPS_KEEPING_OBS_RECORDINGS.includes(stepName));
        }
    });

    it("deadlineEstimate: 見込み + 判定ツールの読み (録画の長さ x 区画 x 秒数) + 他の手順、に余裕を掛けて人の遅れを足す", () => {
        const steps = [
            { kind: "record-start", label: "a" },
            { kind: "wait", ms: 600000 },
            { kind: "record-stop", label: "a", tailSeconds: 0 },
            { kind: "judge", tool: "sync.py", cells: 3, recording: "a" },
            { kind: "judge", tool: "smooth.py", ncells: 3, recording: "a" },
        ];
        // 判定ツール: sync.py 3 区画 x (600 x 0.1 + 15) = 225 秒、smooth.py 1 回 x (600 x 0.3 + 15) = 195 秒。
        expect(JUDGE_SECONDS_PER_RECORDED_SECOND).toMatchObject({ "sync.py": 0.1, "smooth.py": 0.3 });
        const e = deadlineEstimate(steps, { humanOverrunMin: 0 });
        expect(e).toMatchObject({ plannedMinutes: 10, judgeMinutes: 7, otherStepMinutes: 0, marginFactor: DEADLINE_MARGIN_FACTOR, humanOverrunMinutes: 0 });
        // (600 + 420 + 2 (record-start)) x 1.15 = 1175.3 秒 -> 20 分。人の遅れの既定 20 分を足すと 40 分。
        expect(e.minutes).toBe(20);
        expect(deadlineEstimate(steps).minutes).toBe(20 + CALL_ONE_HUMAN_OVERRUN_MIN);
        // label を足していない録画 (step() の第 2 引数の説明の文が label になり、始めと終わりで文字列が違う) も、直前の
        // record-start と対にして長さを出す。recording のない判定は直前に止めた録画を読む。
        const unlabeled = [
            { kind: "record-start", label: "録画を開始する" },
            { kind: "wait", ms: 600000 },
            { kind: "record-stop", label: "録画を終了する", tailSeconds: 0 },
            { kind: "judge", tool: "sync.py", cells: 3 },
            { kind: "judge", tool: "smooth.py", ncells: 3 },
        ];
        expect(deadlineEstimate(unlabeled, { humanOverrunMin: 0 })).toMatchObject({ plannedMinutes: 10, judgeMinutes: 7 });
    });

    it("call-1 の期限は実行計画から見積もる: [外す] の判定と締めの判定ツールまで届き、180 を下回らない。--deadline-min を渡せばそれ", () => {
        const argv = ["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-1", "--call-window", "--real-profile", "--n-screen", "3", "--n-voice", "3"];
        for (const extra of [[], ["--broadcaster-id", "1"]]) {
            const r = parseArgs([...argv, ...extra]);
            const steps = planSteps(r.args).steps;
            const deadline = deadlineMinFor(r.args, steps);
            const estimate = deadlineEstimate(steps);
            expect(deadline).toBe(Math.max(CALL_ONE_DEADLINE_MIN, estimate.minutes));
            expect(deadline).toBeGreaterThanOrEqual(CALL_ONE_DEADLINE_MIN);
            // 期限が 180 分では締めの操作の最中に打ち切られる。判定ツール・人の遅れを入れた見積もりはそれより長い。
            expect(estimate.minutes).toBeGreaterThan(CALL_ONE_DEADLINE_MIN);
            expect(estimate.judgeMinutes).toBeGreaterThan(0);
            // [外す] の判定と、最後の判定ツールまでの見積もりが期限に収まる。
            const restored = steps.findIndex((s) => s.kind === "judge-vencord-dir-restored");
            const lastJudge = steps.map((s) => s.kind).lastIndexOf("judge");
            expect(deadlineEstimate(steps.slice(0, restored + 1)).minutes).toBeLessThanOrEqual(deadline);
            expect(deadlineEstimate(steps.slice(0, lastJudge + 1)).minutes).toBeLessThanOrEqual(deadline);
        }
        // 明示の --deadline-min はそのまま。リハーサルとほかの step は parseArgs の値のまま。
        const given = parseArgs([...argv, "--deadline-min", "150"]);
        expect(deadlineMinFor(given.args, planSteps(given.args).steps)).toBe(150);
        const rehearsal = parseArgs(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-1", "--rehearsal"]);
        expect(deadlineMinFor(rehearsal.args, planSteps(rehearsal.args).steps)).toBe(DEFAULT_DEADLINE_MIN);
        const record = parseArgs(argvFor("record"));
        expect(deadlineMinFor(record.args, planSteps(record.args).steps)).toBe(DEFAULT_DEADLINE_MIN);
    });
});
