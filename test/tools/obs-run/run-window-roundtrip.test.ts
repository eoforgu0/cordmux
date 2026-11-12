/*
 * --step install-roundtrip の実行計画 (run-window-plan.mjs の純粋関数) と、seed-config の書き込みの規則を vitest でテストする。
 * dry-run が全シナリオの実行計画を組めることも確認する。
 *
 * run-window-plan.mjs は Node の API を呼ばないので、テストする対象には値しか渡さない。このテストそのものは
 * 突き合わせのためにだけファイルを読む: 製品の ja-JP.ini (押せるコンポーネントの id が引けること)、
 * run-window.mjs (switch の case と HANDLED_KINDS と実行計画のステップの種類)、seed-config.py (コレクションの名前)、
 * dist/README.md (シナリオの名前の対応)、src-obs/buildspec.json (obs-deps の場所)。
 * テストの名前は ASCII にしている。
 */
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

import {
    DEFAULT_ROUNDTRIP_SCENARIO,
    INSTALL_ROUNDTRIP_STEP,
    OBS_LAUNCH_ARGS,
    OBS_LAUNCH_ARGS_INSTALLED,
    ROUNDTRIP_CHECK_PERIOD_S,
    ROUNDTRIP_COLLECTION_MAIN,
    ROUNDTRIP_COLLECTION_SECOND,
    ROUNDTRIP_JUDGE_NAMES,
    ROUNDTRIP_SCENARIO_VALUES,
    ROUNDTRIP_TAMPER_WAIT_MS,
    ROUNDTRIP_TEXT_IDS,
    STEPS_KEEPING_RECORDINGS,
    STEP_VALUES,
    SYNC_TONE_VALUES,
    OBS_TEXT_CANCEL,
    OBS_TEXT_OK,
    OBS_TEXT_PREFIX,
    OBS_TEXT_TOOLS_MENU,
    TEXT_ID_TOOLS_MENU_UNINSTALL,
    TEXT_ID_ADD_TO_VESKTOP,
    TEXT_ID_DIALOG_SWITCH_BUTTON,
    TEXT_ID_DIALOG_UNINSTALL_BUTTON,
    TEXT_ID_DIALOG_UNINSTALL_TITLE,
    TEXT_ID_DIALOG_VESKTOP_ADD_TITLE,
    TEXT_ID_DOCK_MENU_BUTTON,
    TEXT_ID_DOCK_TAB_DISPLAY,
    TEXT_ID_LOOK_HEADING_NAME,
    TEXT_ID_LOOK_SHOW,
    TEXT_ID_MENU_SETTINGS,
    TEXT_ID_REINSTALL,
    TEXT_ID_TAB_STATUS,
    TEXT_ID_SETTINGS_PAGE_ABOUT,
    TEXT_ID_SETTINGS_UNINSTALL,
    TEXT_ID_SETTINGS_WINDOW_TITLE,
    TEXT_ID_VENCORD_OVERWRITTEN,
    stepTextIds,
    VENCORD_BUNDLE_FILE_NAMES,
    judgeBundleRestored,
    judgeCancelKept,
    judgeFfmpegModules,
    judgeNamedInTexts,
    judgeOverwriteRefused,
    judgeRoundtrip,
    judgeStartupRewrite,
    judgeStateKept,
    judgeUpdateKept,
    judgeVencordDirEquals,
    judgeWrittenAtAdvanced,
    obsDepsBinRelFromBuildspec,
    parseArgs,
    parseBuildMarkText,
    planSteps,
    roundtripPaths,
} from "./run-window-plan.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = join(HERE, "..", "..", "..");
const read = (rel: string) => readFileSync(join(REPO, rel), "utf8");

type Step = Record<string, any>;

function rtArgv(scenario: string, extra: string[] = []): string[] {
    const argv = ["--out", "_work/out/rt", "--step", INSTALL_ROUNDTRIP_STEP, "--tag", "t1", "--zip", "C:/z/cordmux.zip", "--obs-zip", "C:/z/obs.zip", "--scenario", scenario];
    if (scenario === "update" || scenario === "update-while-running") argv.push("--old-zip", "C:/z/old.zip");
    if (scenario === "newer-libobs") argv.push("--test-dll", "C:/z/cordmux-newer.dll");
    return [...argv, ...extra];
}

function plan(scenario: string, extra: string[] = []): Step[] {
    const r = parseArgs(rtArgv(scenario, extra));
    expect(r.errors).toEqual([]);
    const p = planSteps(r.args);
    expect(p.ok).toBe(true);
    return p.steps;
}

const kinds = (steps: Step[]) => steps.map((s) => s.kind);
const idx = (steps: Step[], pred: (s: Step) => boolean) => steps.findIndex(pred);
const lastIdx = (steps: Step[], pred: (s: Step) => boolean) => {
    for (let i = steps.length - 1; i >= 0; i -= 1) if (pred(steps[i])) return i;
    return -1;
};

function iniKeys(): Set<string> {
    const keys = new Set<string>();
    for (const line of read("src-obs/data/locale/ja-JP.ini").split(/\r?\n/)) {
        const t = line.trim();
        if (!t || t.startsWith("#") || t.indexOf("=") < 0) continue;
        keys.add(t.slice(0, t.indexOf("=")).trim());
    }
    return keys;
}

describe("parseArgs for --step install-roundtrip", () => {
    it("needs --tag, --zip and --obs-zip, refuses --obs and --plugin, and derives the OBS folder", () => {
        const ok = parseArgs(rtArgv("roundtrip"));
        expect(ok.ok).toBe(true);
        expect(ok.args.obs).toBe(roundtripPaths("t1").obsRel);
        expect(ok.args.obs).toBe("_work/rt-t1/obs");
        expect(ok.args.scenario).toBe(DEFAULT_ROUNDTRIP_SCENARIO);
        expect(ok.args.layout).toBe("plugins");

        const missing = parseArgs(["--out", "C:/out", "--step", INSTALL_ROUNDTRIP_STEP]);
        expect(missing.ok).toBe(false);
        expect(missing.errors.join(" ")).toMatch(/--tag/);
        expect(missing.errors.join(" ")).toMatch(/--zip/);
        expect(missing.errors.join(" ")).toMatch(/--obs-zip/);

        const withObs = parseArgs([...rtArgv("roundtrip"), "--obs", "C:/obs", "--plugin", "C:/plugin"]);
        expect(withObs.ok).toBe(false);
        expect(withObs.errors.join(" ")).toMatch(/--obs is not accepted/);
        expect(withObs.errors.join(" ")).toMatch(/--plugin is not accepted/);
    });

    it("refuses a tag that is not a plain folder name", () => {
        for (const tag of ["../x", "a b", "a/b", ""]) {
            const argv = rtArgv("roundtrip").map((v, i, all) => (all[i - 1] === "--tag" ? tag : v));
            expect(parseArgs(argv).ok).toBe(false);
        }
    });

    it("knows the 13 scenario names of dist/README.md and refuses others", () => {
        expect(ROUNDTRIP_SCENARIO_VALUES).toHaveLength(13);
        expect(parseArgs(rtArgv("nonsense")).ok).toBe(false);
        // dist/README.md の --scenario の表に出る名前と同じ (異常系も含む)。
        const readme = read("dist/README.md");
        // 表は roundtrip の行から newer-libobs の行まで (見出しのテキストに依存せず、表の最初と最後の行で切る)。
        const start = readme.indexOf("| `roundtrip` |");
        const lastRow = readme.indexOf("| `newer-libobs` |");
        expect(start).toBeGreaterThan(0);
        expect(lastRow).toBeGreaterThan(start);
        const section = readme.slice(start, readme.indexOf("\n", lastRow));
        const names = new Set<string>();
        for (const line of section.split(/\r?\n/)) {
            if (!line.startsWith("| `")) continue;
            const cell = line.split("|")[1];
            for (const m of cell.matchAll(/`([a-z0-9-]+)`/g)) names.add(m[1]);
        }
        expect([...names].sort()).toEqual([...ROUNDTRIP_SCENARIO_VALUES].sort());
    });

    it("accepts the roundtrip flags only with --step install-roundtrip", () => {
        for (const extra of [["--scenario", "move"], ["--layout", "plugins"], ["--zip", "z"], ["--obs-zip", "o"], ["--old-zip", "z"], ["--test-dll", "d"], ["--keep-work"], ["--vm"]]) {
            const r = parseArgs(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "place", ...extra]);
            expect(r.ok).toBe(false);
        }
    });

    it("needs --old-zip exactly for the update scenarios and --test-dll exactly for newer-libobs", () => {
        const base = ["--out", "C:/out", "--step", INSTALL_ROUNDTRIP_STEP, "--tag", "t", "--zip", "z", "--obs-zip", "o"];
        expect(parseArgs([...base, "--scenario", "update"]).ok).toBe(false);
        expect(parseArgs([...base, "--scenario", "update", "--old-zip", "p"]).ok).toBe(true);
        expect(parseArgs([...base, "--scenario", "roundtrip", "--old-zip", "p"]).ok).toBe(false);
        expect(parseArgs([...base, "--scenario", "newer-libobs"]).ok).toBe(false);
        expect(parseArgs([...base, "--scenario", "newer-libobs", "--test-dll", "d"]).ok).toBe(true);
        expect(parseArgs([...base, "--scenario", "motw", "--test-dll", "d"]).ok).toBe(false);
    });

    it("runs --layout programdata only with --vm and only for the roundtrip scenario", () => {
        expect(parseArgs(rtArgv("roundtrip", ["--layout", "programdata"])).ok).toBe(false);
        expect(parseArgs(rtArgv("roundtrip", ["--layout", "programdata", "--vm"])).ok).toBe(true);
        expect(parseArgs(rtArgv("move", ["--layout", "programdata", "--vm"])).ok).toBe(false);
        expect(parseArgs(rtArgv("roundtrip", ["--vm"])).ok).toBe(false);
    });

    it("--layout plugins (portable OBS 33 or later) is the default for every scenario, without --vm; the old portable layout is gone", () => {
        const ok = parseArgs(rtArgv("roundtrip", ["--layout", "plugins"]));
        expect(ok.ok).toBe(true);
        expect(ok.args.layout).toBe("plugins");
        expect(parseArgs(rtArgv("move", ["--layout", "plugins"])).ok).toBe(true);
        expect(parseArgs(rtArgv("roundtrip", ["--layout", "plugins", "--vm"])).ok).toBe(false);
        // 旧方式の置き方 (portable) の値はない。
        const old = parseArgs(rtArgv("roundtrip", ["--layout", "portable"]));
        expect(old.ok).toBe(false);
        expect(old.errors.join(" ")).toMatch(/--layout must be one of plugins\|programdata/);
        // portable の OBS の起動の仕方と設定の保存先のまま、置く手順に置き方を渡す。
        const steps = plan("roundtrip", ["--layout", "plugins"]);
        const places = steps.filter((s) => s.kind === "rt-place");
        expect(places.length).toBeGreaterThan(0);
        for (const s of places) expect(s.layout).toBe("plugins");
        const seed = steps.find((s) => s.kind === "seed-config");
        expect(seed?.installedObs).toBe(false);
    });

    it("accepts --allow-state-json-write only for call-*", () => {
        const nonCall = parseArgs(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "dock-view", "--allow-state-json-write"]);
        expect(nonCall.ok).toBe(false);
        expect(nonCall.errors.join(" ")).toMatch(/--allow-state-json-write is accepted only for/);
        const call = parseArgs(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-1", "--call-window", "--real-profile", "--allow-state-json-write"]);
        expect(call.ok).toBe(true);
        expect(parseArgs([...rtArgv("roundtrip"), "--allow-state-json-write"]).ok).toBe(false);
    });

    it("no longer has --cordmux-scenes (the product creates no scenes) and seed-config carries no such value", () => {
        const base = ["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "dock-view"];
        expect(planSteps(parseArgs(base).args).steps[0]).toMatchObject({ kind: "seed-config", step: "dock-view", shape: "test", realProfile: false });
        expect(planSteps(parseArgs(base).args).steps[0]).not.toHaveProperty("cordmuxScenes");
        expect(parseArgs([...base, "--cordmux-scenes", "none"]).ok).toBe(false);
    });

    it("passes the flag to seed-config only for call-* and says realProfile for them", () => {
        for (const step of STEP_VALUES) {
            if (step === INSTALL_ROUNDTRIP_STEP) continue;
            const callFlags = step.startsWith("call-") ? ["--call-window", "--real-profile"] : [];
            const r = parseArgs(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", step, ...callFlags]);
            for (const s of planSteps(r.args).steps.filter((x: Step) => x.kind === "seed-config")) {
                expect(s.allowStateJsonWrite).toBe(step.startsWith("call-"));
                expect(s.realProfile).toBe(step.startsWith("call-"));
                expect(s.useAppData).toBe(!step.startsWith("call-"));
                expect(s.step).toBe(step);
            }
        }
    });
});

describe("install-roundtrip plans", () => {
    it("every scenario plans, ends with copy-obs-logs, write-result, summarize, cleanup, and deletes the work folder", () => {
        for (const scenario of ROUNDTRIP_SCENARIO_VALUES) {
            const steps = plan(scenario);
            expect(kinds(steps).slice(-4)).toEqual(["copy-obs-logs", "write-result", "summarize", "cleanup"]);
            expect(steps.at(-1)).toMatchObject({ kind: "cleanup", callWindow: false, keepRecordings: false, rtWorkRel: "_work/rt-t1", keepWork: false, watchProgramData: false });
            expect(steps.slice(0, 2).map((s) => s.kind)).toEqual(["rt-prepare", "rt-extract"]);
            expect(steps[1]).toMatchObject({ what: "obs", zip: "C:/z/obs.zip", destRel: "_work/rt-t1/obs" });
            const ids = steps.map((s) => s.id);
            expect(new Set(ids).size).toBe(ids.length);
        }
    });

    it("every seed-config of a scenario writes the user-like shape with the swapped APPDATA and no flag", () => {
        for (const scenario of ROUNDTRIP_SCENARIO_VALUES) {
            const seeds = plan(scenario).filter((s) => s.kind === "seed-config");
            expect(seeds).toHaveLength(1);
            expect(seeds[0]).toMatchObject({ shape: "user", step: INSTALL_ROUNDTRIP_STEP, useAppData: true, realProfile: false, allowStateJsonWrite: false, appDataRel: "_work/rt-t1/appdata", installedObs: false });
        }
    });

    it("--keep-work keeps the work folder", () => {
        expect(plan("roundtrip", ["--keep-work"]).at(-1)).toMatchObject({ kind: "cleanup", keepWork: true });
    });

    it("roundtrip: calibrate twice, install, connect, build in both collections, snapshot (installed), uninstall, remove, snapshot (uninstalled)", () => {
        const steps = plan("roundtrip");
        const snap = (label: string) => idx(steps, (s) => s.kind === "snapshot" && s.label === label);
        const cmp = (label: string) => idx(steps, (s) => s.kind === "snapshot-compare" && s.label === label);
        const seed = idx(steps, (s) => s.kind === "seed-config");
        const extractZip = idx(steps, (s) => s.kind === "rt-extract" && s.what === "cordmux");
        const place = idx(steps, (s) => s.kind === "rt-place");
        const connect = idx(steps, (s) => s.kind === "dialog-click" && s.buttonTextId === TEXT_ID_ADD_TO_VESKTOP);
        const uninstall = idx(steps, (s) => s.kind === "dialog-click" && s.buttonTextId === TEXT_ID_SETTINGS_UNINSTALL);
        const remove = idx(steps, (s) => s.kind === "rt-remove-placed");
        const cleanup = idx(steps, (s) => s.kind === "cleanup");

        expect(seed).toBeLessThan(snap("calib-1"));
        expect(snap("calib-1")).toBeLessThan(snap("calib-2"));
        expect(snap("calib-2")).toBeLessThan(cmp("calibration"));
        expect(cmp("calibration")).toBeLessThan(extractZip);
        expect(extractZip).toBeLessThan(place);
        expect(place).toBeLessThan(connect);
        expect(connect).toBeLessThan(snap("installed"));
        expect(snap("installed")).toBeLessThan(cmp("ac04"));
        expect(cmp("ac04")).toBeLessThan(uninstall);
        expect(uninstall).toBeLessThan(remove);
        expect(remove).toBeLessThan(snap("uninstalled"));
        expect(snap("uninstalled")).toBeLessThan(cmp("ac69"));
        // スナップショットを取るのは後片付けより前。
        expect(lastIdx(steps, (s) => s.kind === "snapshot" || s.kind === "snapshot-compare")).toBeLessThan(cleanup);

        // ベースラインの測定は cordmux を置く前に 2 回起動する (起動のたびに閉じてからスナップショットを取る)。
        const launchesBeforePlace = steps.slice(0, place).filter((s) => s.kind === "launch-obs");
        expect(launchesBeforePlace).toHaveLength(2);
        for (const label of ["calib-1", "calib-2", "installed", "uninstalled"]) {
            expect(steps[snap(label) - 1].kind).toBe("close-obs");
        }
        // 比べる相手とばらつき。ベースラインに利用者のソースが入ったら止める。
        expect(steps[cmp("calibration")]).toMatchObject({ mode: "calibration", base: "calib-1", other: "calib-2", gate: true, stopOnFail: true });
        expect(steps[cmp("ac04")]).toMatchObject({ mode: "installed", base: "calib-1", other: "installed", wobble: "calibration", gate: true });
        expect(steps[cmp("ac69")]).toMatchObject({ mode: "uninstalled", base: "calib-1", other: "uninstalled", wobble: "calibration", gate: true });
        for (const s of steps.filter((x) => x.kind === "snapshot")) {
            expect(s.roots).toEqual(["obs_config", "obs_program", "appdata", "localappdata", "custom_vencord"]);
            expect(s.gate).toBe(true);
        }
        expect(steps[remove].gate).toBe(true);
    });

    it("roundtrip: the Vencord switch dialog must not appear without a vencordDir, and the composite source is placed in both collections", () => {
        const steps = plan("roundtrip");
        const connect = idx(steps, (s) => s.kind === "dialog-click" && s.buttonTextId === TEXT_ID_ADD_TO_VESKTOP);
        expect(steps[connect]).toMatchObject({ gate: true, noWait: true, titleTextId: "cordmux.dock.window.title" });
        expect(steps[connect + 1]).toMatchObject({ kind: "dialog-click", titleTextId: TEXT_ID_DIALOG_VESKTOP_ADD_TITLE, expect: "absent", listOnly: true, gate: true });
        const wires = steps.filter((s) => s.kind === "wire-composite");
        expect(wires.map((s) => s.sceneName)).toEqual(["Opening", "Chat"]);
        for (const w of wires) expect(w).toMatchObject({ inputName: "cordmux", gate: true });
        // 置くのは合成のソースだけで、cordmux枠 (参加者ごとのソース) は置かない。
        expect(steps.some((s) => s.kind === "wire-person")).toBe(false);
        expect(steps.some((s) => s.kind === "dock-click" && s.textId === "cordmux.dock.create_scenes.button")).toBe(false);
        // 切り替えは 3 組: ベースラインの 1 つ目・2 つ目 (OBS に 2 つ目のコレクションの既定のキーを書かせる) と、インストール後の合成のソースの置き直し。
        const switches = steps.filter((s) => s.kind === "obsws-request" && s.request === "SetCurrentSceneCollection").map((s) => s.requestData.sceneCollectionName);
        const pair = [ROUNDTRIP_COLLECTION_SECOND, ROUNDTRIP_COLLECTION_MAIN];
        expect(switches).toEqual([...pair, ...pair, ...pair]);
        const firstWire = idx(steps, (s) => s.kind === "wire-composite");
        const secondWire = lastIdx(steps, (s) => s.kind === "wire-composite");
        const toSecond = idx(steps, (s) => s.kind === "obsws-request" && s.requestData && s.requestData.sceneCollectionName === ROUNDTRIP_COLLECTION_SECOND && steps.indexOf(s) > firstWire);
        expect(firstWire).toBeLessThan(toSecond);
        expect(toSecond).toBeLessThan(secondWire);
    });

    it("roundtrip: uninstall goes OBS [Tools] -> uninstall cordmux -> (read the settings window) -> confirm -> result ([OK] by the OBS text), then closes before removing", () => {
        const steps = plan("roundtrip");
        const tools = idx(steps, (s) => s.kind === "dialog-click" && s.buttonTextId === OBS_TEXT_TOOLS_MENU);
        // OBS の [ツール] -> 「cordmuxをアンインストール...」の順に操作する。ページのボタンは押さない。
        expect(steps.slice(tools, tools + 3).map((s) => [s.popup ? "popup" : s.titleTextId, s.controlType ?? null, s.buttonTextId, !!s.listOnly])).toEqual([
            ["popup", "MenuItem", OBS_TEXT_TOOLS_MENU, false],
            ["popup", "MenuItem", TEXT_ID_TOOLS_MENU_UNINSTALL, false],
            [TEXT_ID_SETTINGS_WINDOW_TITLE, null, TEXT_ID_SETTINGS_UNINSTALL, true],
        ]);
        // [ツール] はメニューを開き、項目は設定ウィンドウと確認ウィンドウ (QDialog::exec) を開くので、どちらも Invoke の戻りを待たない。
        expect(steps[tools]).toMatchObject({ noWait: true, gate: true });
        expect(steps[tools + 1]).toMatchObject({ noWait: true, gate: true });
        // 設定ウィンドウを読むステップは押さず、ゲートにしない (確認ウィンドウが出ないときの理由のテキストを残すだけ)。
        expect(steps[tools + 2]).toMatchObject({ kind: "dialog-click", listOnly: true, expect: "button", label: "uninstall-entry-page" });
        expect(steps[tools + 2].gate).toBeUndefined();
        expect(steps.some((s) => s.kind === "dialog-click" && s.buttonTextId === TEXT_ID_SETTINGS_UNINSTALL && !s.listOnly)).toBe(false);
        expect(steps.some((s) => s.kind === "dialog-click" && s.buttonTextId === TEXT_ID_MENU_SETTINGS)).toBe(false);
        const uninstall = tools + 2;
        // 押すとアンインストールが実行されて結果ダイアログが開く [アンインストール] は Invoke が戻らない (noWait)。
        expect(steps[uninstall + 1]).toMatchObject({ kind: "dialog-click", titleTextId: TEXT_ID_DIALOG_UNINSTALL_TITLE, buttonTextId: TEXT_ID_DIALOG_UNINSTALL_BUTTON, noWait: true, expect: "present", gate: true });
        // 結果ダイアログは [フォルダーを開く] と [OK] の 2 つ。メッセージを読んでから [OK] (OBS 自身のテキスト) を指定して押す。
        expect(steps[uninstall + 2]).toMatchObject({ kind: "dialog-click", titleTextId: TEXT_ID_DIALOG_UNINSTALL_TITLE, buttonTextId: OBS_TEXT_OK, listOnly: true, expect: "button", label: "uninstall-done", gate: true });
        expect(steps[uninstall + 3]).toMatchObject({ kind: "dialog-click", titleTextId: TEXT_ID_DIALOG_UNINSTALL_TITLE, buttonTextId: OBS_TEXT_OK, expect: "present" });
        const remove = idx(steps, (s) => s.kind === "rt-remove-placed");
        expect(steps[remove - 1].kind).toBe("close-obs");
        // 外した後の起動は cordmux がないので、Loaded Modules の cordmux をゲートにしない。
        const afterLaunch = idx(steps, (s, ) => s.kind === "launch-obs" && steps.indexOf(s) > remove);
        const readsAfter = steps.slice(afterLaunch).filter((s) => s.kind === "read-obs-log");
        expect(readsAfter).toHaveLength(0);
    });

    it("every launch with cordmux gates on cordmux.dll being loaded, except old-obs and newer-libobs", () => {
        for (const scenario of ROUNDTRIP_SCENARIO_VALUES) {
            const steps = plan(scenario);
            const loaded = steps.filter((s) => s.kind === "read-obs-log" && s.pattern === "module-loaded");
            expect(loaded.length).toBeGreaterThan(0);
            for (const s of loaded) {
                expect(Boolean(s.gate)).toBe(scenario !== "old-obs" && scenario !== "newer-libobs");
            }
            for (const s of steps.filter((x) => x.kind === "launch-obs")) expect(s.launchArgs).toEqual(OBS_LAUNCH_ARGS);
        }
    });

    it("other-vencord: the self-built Vencord is seeded, the Vencord switch dialog is confirmed, and vencordDir comes back after uninstall", () => {
        const steps = plan("other-vencord");
        expect(steps.find((s) => s.kind === "seed-config")).toMatchObject({ vencordState: "custom", customVencordRel: "_work/rt-t1/custom-vencord/dist" });
        const dialog = steps.find((s) => s.kind === "dialog-click" && s.label === "switch-dialog-confirm");
        expect(dialog).toMatchObject({ titleTextId: TEXT_ID_DIALOG_VESKTOP_ADD_TITLE, buttonTextId: TEXT_ID_DIALOG_SWITCH_BUTTON, expect: "present", gate: true });
        const judge = steps.filter((s) => s.kind === "judge-rt").at(-1);
        expect(judge).toMatchObject({ judge: "vencord-dir-equals", gate: true, inputs: { expectedDir: { dir: "custom_vencord" } } });
        expect(idx(steps, (s) => s.kind === "snapshot-compare" && s.label === "ac69")).toBeLessThan(steps.indexOf(judge));
    });

    it("other-vencord-cancel: [cancel] keeps state.json and writes no backup", () => {
        const steps = plan("other-vencord-cancel");
        const before = idx(steps, (s) => s.kind === "capture-file" && s.label === "state-before-cancel");
        const cancel = idx(steps, (s) => s.kind === "dialog-click" && s.buttonTextId === OBS_TEXT_CANCEL);
        const judge = idx(steps, (s) => s.kind === "judge-rt" && s.judge === "cancel-kept");
        expect(before).toBeLessThan(cancel);
        expect(cancel).toBeLessThan(judge);
        expect(kinds(steps)).not.toContain("snapshot");
    });

    it("move: rename the whole OBS folder between two starts and judge the startup rewrite", () => {
        const steps = plan("move");
        const rename = idx(steps, (s) => s.kind === "rt-rename-obs");
        expect(steps[rename]).toMatchObject({ fromRel: "_work/rt-t1/obs", toRel: "_work/rt-t1/obs-moved", gate: true });
        expect(steps.slice(0, rename).filter((s) => s.kind === "close-obs").length).toBeGreaterThan(0);
        expect(lastIdx(steps.slice(0, rename), (s) => s.kind === "launch-obs")).toBeLessThan(lastIdx(steps.slice(0, rename), (s) => s.kind === "close-obs"));
        const moved = steps.slice(rename).find((s) => s.kind === "launch-obs");
        expect(moved.obs).toBe("_work/rt-t1/obs-moved");
        // 押さずに起動する: 名前の変更の後に [Vesktop に追加] を押さない (読むだけの dialog-click の -ListOnly は押さない)。
        expect(steps.slice(rename).some((s) => s.kind === "dock-click" || (s.kind === "dialog-click" && !s.listOnly))).toBe(false);
        const judge = steps.find((s) => s.kind === "judge-rt");
        expect(judge).toMatchObject({ judge: "startup-rewrite", gate: true, inputs: { expectedDir: { dir: "bundle_shown" } } });
        expect(idx(steps, (s) => s.kind === "capture-file" && s.label === "state-before-move")).toBeLessThan(rename);
        expect(idx(steps, (s) => s.kind === "capture-file" && s.label === "backup-after-move")).toBeGreaterThan(rename);
    });

    it("move-foreign: the user's value is set before moving, [fix] is offered and state.json is kept", () => {
        const steps = plan("move-foreign");
        const edit = idx(steps, (s) => s.kind === "edit-state-json");
        const rename = idx(steps, (s) => s.kind === "rt-rename-obs");
        expect(edit).toBeLessThan(rename);
        expect(steps[edit]).toMatchObject({ vencordDir: "custom_vencord" });
        expect(steps.find((s) => s.kind === "dialog-click" && s.buttonTextId === TEXT_ID_ADD_TO_VESKTOP && s.listOnly)).toMatchObject({ expect: "button", gate: true, titleTextId: "cordmux.dock.window.title" });
        expect(steps.find((s) => s.kind === "judge-rt")).toMatchObject({ judge: "state-kept", gate: true });
    });

    it("force-update-missing: delete the 4 files before starting, then the overwritten notice, [reinstall], hashes, written_at", () => {
        const steps = plan("force-update-missing");
        const del = idx(steps, (s) => s.kind === "delete-bundle-files");
        expect(steps[del].names).toEqual(VENCORD_BUNDLE_FILE_NAMES);
        const launchAfter = idx(steps, (s) => s.kind === "launch-obs" && steps.indexOf(s) > del);
        expect(launchAfter).toBeGreaterThan(del);
        const list = idx(steps, (s) => s.kind === "dialog-click" && s.buttonTextId === TEXT_ID_REINSTALL);
        const named = idx(steps, (s) => s.kind === "judge-rt" && s.judge === "named-in-texts");
        const press = idx(steps, (s) => s.kind === "dock-click" && s.textId === TEXT_ID_REINSTALL);
        const hash = idx(steps, (s) => s.kind === "hash-bundle");
        const restored = idx(steps, (s) => s.kind === "judge-rt" && s.judge === "bundle-restored");
        const written = idx(steps, (s) => s.kind === "judge-rt" && s.judge === "written-at-advanced");
        expect([launchAfter < list, list < named, named < press, press < hash, hash < restored, restored < written]).toEqual([true, true, true, true, true, true]);
        // メッセージはファイルの名前を画面に出さない (rules_dock.cpp の VencordOverwritten。名前はログに出す) ので、見出しを確認する。
        expect(steps[named].inputs.names).toEqual({ textPrefix: TEXT_ID_VENCORD_OVERWRITTEN });
    });

    it("force-update-tamper: short periods, a connected test sender, 1 byte appended while running, then the same reinstall checks", () => {
        const steps = plan("force-update-tamper");
        expect(steps.find((s) => s.kind === "seed-config")).toMatchObject({ vencordCheckDisconnectedS: ROUNDTRIP_CHECK_PERIOD_S, vencordCheckConnectedS: ROUNDTRIP_CHECK_PERIOD_S });
        const harness = idx(steps, (s) => s.kind === "harness-start");
        const tamper = idx(steps, (s) => s.kind === "tamper-file");
        expect(SYNC_TONE_VALUES).toContain(steps[harness].syncTone);
        expect(harness).toBeLessThan(tamper);
        // cordmux のソースを置いてから上書きする (ソースがないと、そのメッセージが 1 行目に出て [再インストール] が隠れる)。
        const wire = lastIdx(steps.slice(0, harness), (s) => s.kind === "wire-composite");
        expect(wire).toBeGreaterThan(lastIdx(steps.slice(0, harness), (s) => s.kind === "launch-obs"));
        // 接続している間の上書きはレベル 2 のメッセージで、[再インストール] は状態タブのカードにある。先に状態タブを選ぶ。
        const tab = idx(steps, (s) => s.kind === "dialog-click" && s.controlType === "TabItem" && s.buttonTextId === TEXT_ID_TAB_STATUS);
        expect(steps[tab]).toMatchObject({ namePrefix: true });
        expect(tab).toBeGreaterThan(tamper);
        expect(tab).toBeLessThan(idx(steps, (s) => s.kind === "dock-click" && s.textId === TEXT_ID_REINSTALL));
        // 接続していない間の欠けは参加者タブのカードに出るので、タブを選ばない。
        expect(plan("force-update-missing").some((s) => s.kind === "dialog-click" && s.controlType === "TabItem")).toBe(false);
        expect(steps[tamper + 1]).toMatchObject({ kind: "wait" });
        expect(steps[tamper + 1].ms).toBeGreaterThanOrEqual(3 * ROUNDTRIP_CHECK_PERIOD_S * 1000);
        expect(ROUNDTRIP_TAMPER_WAIT_MS).toBeGreaterThanOrEqual(3 * ROUNDTRIP_CHECK_PERIOD_S * 1000);
        expect(steps.find((s) => s.kind === "judge-rt" && s.judge === "bundle-restored")).toBeTruthy();
        // テスト用の送信側の終了を待ってから閉じる。
        expect(idx(steps, (s) => s.kind === "harness-wait")).toBeLessThan(lastIdx(steps, (s) => s.kind === "close-obs"));
    });

    it("update: install the previous zip, change a setting, close, overwrite with the new zip, start, judge", () => {
        const steps = plan("update");
        const extracts = steps.filter((s) => s.kind === "rt-extract" && s.what !== "obs").map((s) => s.what);
        expect(extracts).toEqual(["cordmux-old", "cordmux"]);
        // 利用者が変えた設定の代わりに、ドックの [表示] タブの「名前」の [表示] を外す。押す前のドックのスクリーンショットを 1 枚撮る。
        const toggle = idx(steps, (s) => s.kind === "dialog-click" && s.buttonTextId === TEXT_ID_LOOK_SHOW);
        expect(steps[toggle]).toMatchObject({ controlType: "CheckBox", desired: "off", gate: true, buttonGroupTextId: TEXT_ID_LOOK_HEADING_NAME });
        expect(steps[toggle - 1]).toMatchObject({ kind: "dialog-click", controlType: "TabItem", buttonTextId: TEXT_ID_DOCK_TAB_DISPLAY });
        expect(steps[toggle - 2]).toMatchObject({ kind: "screenshot-window", shotLabel: "before-toggle" });
        const newPlace = lastIdx(steps, (s) => s.kind === "rt-place");
        const closeBefore = lastIdx(steps.slice(0, newPlace), (s) => s.kind === "close-obs");
        const launchBefore = lastIdx(steps.slice(0, newPlace), (s) => s.kind === "launch-obs");
        expect(toggle).toBeLessThan(closeBefore);
        expect(launchBefore).toBeLessThan(closeBefore); // 更新の前に OBS を閉じる
        expect(idx(steps, (s) => s.kind === "capture-file" && s.label === "settings-before-update")).toBeLessThan(newPlace);
        expect(steps.find((s) => s.kind === "judge-rt")).toMatchObject({ judge: "update-kept", gate: true });
    });

    it("update-while-running: the new zip is placed while OBS still runs, tolerantly, and the DLL must not be replaced", () => {
        const steps = plan("update-while-running");
        const newPlace = lastIdx(steps, (s) => s.kind === "rt-place");
        expect(steps[newPlace]).toMatchObject({ tolerant: true, label: "overwrite-while-running" });
        expect(steps[newPlace].gate).toBeUndefined();
        const firstLaunch = idx(steps, (s) => s.kind === "launch-obs");
        expect(steps.slice(firstLaunch, newPlace).some((s) => s.kind === "close-obs")).toBe(false);
        expect(steps.find((s) => s.kind === "judge-rt")).toMatchObject({ judge: "overwrite-refused", gate: true });
    });

    it("motw: mark every placed file before starting, then the module and the placed ffmpeg.exe must run", () => {
        const steps = plan("motw");
        const place = idx(steps, (s) => s.kind === "rt-place");
        const mark = idx(steps, (s) => s.kind === "mark-motw");
        const launch = idx(steps, (s) => s.kind === "launch-obs");
        expect(place).toBeLessThan(mark);
        expect(mark).toBeLessThan(launch);
        expect(steps[mark]).toMatchObject({ zoneId: 3, gate: true });
        expect(steps.find((s) => s.kind === "run-placed-ffmpeg")).toMatchObject({ gate: true });
        expect(steps.find((s) => s.kind === "read-obs-log" && s.pattern === "ffmpeg-versions")).toMatchObject({ gate: true });
    });

    it("dll-clash, old-obs, newer-libobs: record what is loaded; the older OBS may not answer obs-websocket", () => {
        expect(plan("dll-clash").find((s) => s.kind === "list-modules")).toMatchObject({ gate: true });
        const old = plan("old-obs");
        expect(old.find((s) => s.kind === "obsws-connect")).toMatchObject({ tolerant: true });
        expect(old.find((s) => s.kind === "read-obs-log" && s.pattern === "cordmux-lines")).toBeTruthy();
        // OBS 33 の置き方の cordmux を OBS 32 に置く場面: 読み込まれなかったことと、手順書の案内をゲートにする。
        expect(old.find((s) => s.kind === "read-obs-log" && s.pattern === "module-not-loaded")).toMatchObject({ gate: true, moduleName: "cordmux" });
        expect(old.find((s) => s.kind === "check-guide-update-hint")).toMatchObject({ gate: true });
        expect(old.find((s) => s.kind === "read-obs-log" && s.pattern === "module-loaded")).toMatchObject({ moduleName: "cordmux" });
        expect(old.find((s) => s.kind === "read-obs-log" && s.pattern === "module-loaded")?.gate).toBeUndefined();
        const newer = plan("newer-libobs");
        const place = idx(newer, (s) => s.kind === "rt-place");
        const replace = idx(newer, (s) => s.kind === "rt-replace-dll");
        const launch = idx(newer, (s) => s.kind === "launch-obs");
        expect(place).toBeLessThan(replace);
        expect(replace).toBeLessThan(launch);
        expect(newer[replace]).toMatchObject({ testDll: "C:/z/cordmux-newer.dll", gate: true });
        expect(newer.filter((s) => s.kind === "screenshot-window")).toHaveLength(2);
    });

    it("--layout programdata (VM): installed OBS launch args, a ProgramData root, the seed writes the installed config, cleanup watches ProgramData", () => {
        const steps = plan("roundtrip", ["--layout", "programdata", "--vm"]);
        for (const s of steps.filter((x) => x.kind === "launch-obs")) {
            expect(s.launchArgs).toEqual(OBS_LAUNCH_ARGS_INSTALLED);
            expect(s.launchArgs).not.toContain("--portable");
        }
        expect(steps.find((s) => s.kind === "seed-config")).toMatchObject({ installedObs: true, vm: true });
        expect(steps.find((s) => s.kind === "snapshot").roots).toContain("programdata_plugins");
        expect(steps.find((s) => s.kind === "rt-place")).toMatchObject({ layout: "programdata" });
        expect(steps.at(-1)).toMatchObject({ kind: "cleanup", watchProgramData: true });
    });

    it("every text id the plans use exists in the product's ja-JP.ini", () => {
        const keys = iniKeys();
        for (const id of ROUNDTRIP_TEXT_IDS) expect(keys.has(id)).toBe(true);
        for (const scenario of ROUNDTRIP_SCENARIO_VALUES) {
            for (const s of plan(scenario)) {
                // OBS 自身のテキスト ("obs:") は OBS の ini にある ([OK] と [キャンセル] と [ツール] だけ)。
                for (const id of stepTextIds(s)) {
                    if (id.startsWith(OBS_TEXT_PREFIX)) expect([OBS_TEXT_OK, OBS_TEXT_CANCEL, OBS_TEXT_TOOLS_MENU]).toContain(id);
                    else expect(keys.has(id), id).toBe(true);
                }
            }
        }
    });

    it("every judge a plan names is known to judgeRoundtrip", () => {
        for (const scenario of ROUNDTRIP_SCENARIO_VALUES) {
            for (const s of plan(scenario).filter((x) => x.kind === "judge-rt")) expect(ROUNDTRIP_JUDGE_NAMES).toContain(s.judge);
        }
    });
});

describe("run-window.mjs and the plans agree", () => {
    // switch の case と HANDLED_KINDS のテキストを読む (run-window.mjs は import すると main が実行されるのでテキストとして読む)。
    const source = read("test/tools/obs-run/run-window.mjs");
    const caseKinds = new Set([...source.matchAll(/^\s+case "([a-z0-9-]+)":/gm)].map((m) => m[1]));
    const handledBlock = source.slice(source.indexOf("const HANDLED_KINDS = ["), source.indexOf("];", source.indexOf("const HANDLED_KINDS = [")));
    const handled = new Set([...handledBlock.matchAll(/"([a-z0-9-]+)"/g)].map((m) => m[1]));

    it("HANDLED_KINDS lists exactly the step kinds of the switch", () => {
        const stepCases = [...caseKinds].filter((k) => handled.has(k) || !["absent", "present", "button"].includes(k));
        expect([...handled].sort()).toEqual(stepCases.sort());
    });

    it("every kind any plan uses (all steps, all scenarios) has a case in the switch", () => {
        const used = new Set<string>();
        for (const step of STEP_VALUES) {
            if (step === INSTALL_ROUNDTRIP_STEP) continue;
            const callFlags = step.startsWith("call-") ? ["--call-window", "--real-profile"] : [];
            for (const s of planSteps(parseArgs(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", step, ...callFlags]).args).steps) used.add(s.kind);
        }
        for (const scenario of ROUNDTRIP_SCENARIO_VALUES) for (const s of plan(scenario)) used.add(s.kind);
        for (const s of plan("roundtrip", ["--layout", "programdata", "--vm"])) used.add(s.kind);
        // 別アカウント方式のステップ (--broadcaster-id) とリハーサル (--rehearsal) の実行計画も全部。
        const callBase = ["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-1"];
        for (const s of planSteps(parseArgs([...callBase, "--call-window", "--real-profile", "--broadcaster-id", "1"]).args).steps) used.add(s.kind);
        for (const s of planSteps(parseArgs([...callBase, "--rehearsal", "--broadcaster-id", "1"]).args).steps) used.add(s.kind);
        const missing = [...used].filter((k) => !caseKinds.has(k) || !handled.has(k));
        expect(missing).toEqual([]);
    });

    it("the finally cleanup keeps the recordings for the same steps as the plans", () => {
        // ステップの cleanup が残しても、finally が -KeepRecordings なしで呼び直すと録画ファイルは消える。
        const finallyBlock = source.slice(source.lastIndexOf("} finally {"));
        // 一覧と hold の --keep-recordings を実行計画と同じ 1 つの関数で決める (keepsRecordings)。
        expect(finallyBlock).toContain("keepRecordings: keepsRecordings(args)");
        expect(STEPS_KEEPING_RECORDINGS).toEqual(["record", "call-1", "call-hold"]);
    });

    it("the finally keeps the call-1 OBS recordings before cleanup.ps1", () => {
        // 期限でステップの途中から finally に入ると、cleanup.ps1 が OBS の録画 (main / av1 / av2 / ac35) を消してしまう。
        // ミュートを戻すのは obs-websocket を閉じる前、録画を移すのは cleanup.ps1 の前でなければ意味がない。
        const finallyBlock = source.slice(source.lastIndexOf("} finally {"));
        expect(finallyBlock).not.toContain("restoreAudioIsolation");
        const keepAt = finallyBlock.indexOf("await keepObsRecordings()");
        const cleanupAt = finallyBlock.indexOf("runCleanup(false");
        expect(keepAt).toBeGreaterThan(0);
        expect(keepAt).toBeLessThan(cleanupAt);
        expect(finallyBlock).toContain("STEPS_KEEPING_OBS_RECORDINGS.includes(args.step)");
        // OBS を閉じた後 (書き込む側が手放した後) に移す。
        expect(finallyBlock.indexOf("closeObsGracefully(state.firstPid)")).toBeLessThan(keepAt);
    });

    it("seed-config.py names the user-like collections the same way as the plan", () => {
        const seed = read("test/tools/obs-run/seed-config.py");
        expect(seed).toContain('USER_COLLECTION_MAIN = "' + ROUNDTRIP_COLLECTION_MAIN + '"');
        expect(seed).toContain('USER_COLLECTION_SECOND = "' + ROUNDTRIP_COLLECTION_SECOND + '"');
    });

    it("the obs-deps bin folder comes from src-obs/buildspec.json", () => {
        const spec = JSON.parse(read("src-obs/buildspec.json"));
        const rel = obsDepsBinRelFromBuildspec(spec);
        expect(rel).toBe("src-obs/.deps/obs-studio-" + spec.dependencies["obs-studio"].version + "/.deps/obs-deps-" + spec.dependencies.prebuilt.version + "-x64/bin");
        expect(obsDepsBinRelFromBuildspec({})).toBeNull();
        expect(obsDepsBinRelFromBuildspec(null)).toBeNull();
    });
});

describe("roundtrip judges (pure)", () => {
    const expected = "C:\\rt\\obs-moved\\plugins\\cordmux\\data\\vencord";
    const stateBefore = JSON.stringify({ maximized: false, vencordDir: "C:/rt/obs/plugins/cordmux/data/vencord", windowBounds: { x: 1 } });
    const backupBefore = JSON.stringify({ previous: null, written: "C:/rt/obs/plugins/cordmux/data/vencord", written_at: 10, dll_dir: "C:/rt/obs/plugins/cordmux" });

    it("startup-rewrite: green when only vencordDir moved and the backup advanced; red otherwise", () => {
        const stateAfter = JSON.stringify({ maximized: false, vencordDir: "C:/rt/obs-moved/plugins/cordmux/data/vencord/", windowBounds: { x: 1 } });
        const backupAfter = JSON.stringify({ previous: null, written: expected, written_at: 20, dll_dir: "C:/rt/obs-moved/plugins/cordmux" });
        expect(judgeStartupRewrite({ stateBefore, stateAfter, backupBefore, backupAfter, expectedDir: expected }).ok).toBe(true);
        expect(judgeStartupRewrite({ stateBefore, stateAfter: stateBefore, backupBefore, backupAfter, expectedDir: expected }).ok).toBe(false);
        const otherKey = JSON.stringify({ maximized: true, vencordDir: expected, windowBounds: { x: 1 } });
        expect(judgeStartupRewrite({ stateBefore, stateAfter: otherKey, backupBefore, backupAfter, expectedDir: expected }).reason).toMatch(/maximized/);
        const prevChanged = JSON.stringify({ previous: "C:/x", written: expected, written_at: 20 });
        expect(judgeStartupRewrite({ stateBefore, stateAfter, backupBefore, backupAfter: prevChanged, expectedDir: expected }).ok).toBe(false);
        expect(judgeStartupRewrite({ stateBefore, stateAfter, backupBefore: null, backupAfter, expectedDir: expected }).ok).toBe(false);
    });

    it("startup-rewrite: red when written_at or dll_dir did not advance, or another backup key changed", () => {
        const stateAfter = JSON.stringify({ maximized: false, vencordDir: expected, windowBounds: { x: 1 } });
        const base = { previous: null, written: expected, written_at: 20, dll_dir: "C:/rt/obs-moved/plugins/cordmux" };
        const judge = (b: object) => judgeStartupRewrite({ stateBefore, stateAfter, backupBefore, backupAfter: JSON.stringify(b), expectedDir: expected });
        expect(judge(base).ok).toBe(true);
        expect(judge({ ...base, written_at: 10 }).reason).toMatch(/written_at/);
        expect(judge({ ...base, dll_dir: "C:/rt/obs/plugins/cordmux" }).reason).toMatch(/dll_dir/);
        expect(judge({ ...base, extra: 1 }).reason).toMatch(/extra/);
    });

    it("state-kept and cancel-kept", () => {
        expect(judgeStateKept({ stateBefore, stateAfter: stateBefore }).ok).toBe(true);
        expect(judgeStateKept({ stateBefore, stateAfter: JSON.stringify({ maximized: false, vencordDir: expected, windowBounds: { x: 1 } }) }).ok).toBe(false);
        expect(judgeCancelKept({ stateBefore, stateAfter: stateBefore, backupAfter: null }).ok).toBe(true);
        expect(judgeCancelKept({ stateBefore, stateAfter: stateBefore, backupAfter: "{}" }).ok).toBe(false);
    });

    it("vencord-dir-equals compares paths the same way as the gate (separators, case, trailing slash)", () => {
        expect(judgeVencordDirEquals({ state: JSON.stringify({ vencordDir: "c:/RT/obs-moved/plugins/cordmux/data/vencord/" }), expectedDir: expected }).ok).toBe(true);
        expect(judgeVencordDirEquals({ state: JSON.stringify({}), expectedDir: expected }).ok).toBe(false);
    });

    it("update-kept: same settings, backup, state; a new mark equal to the new zip's", () => {
        const s = JSON.stringify({ auto_layout: false });
        const mark = (id: string) => "build_id: " + id + "\nvencord_commit: abc\n";
        const base = { settingsBefore: s, settingsAfter: s, backupBefore, backupAfter: backupBefore, stateBefore, stateAfter: stateBefore, markBefore: mark("0.1.0+aaa"), markAfter: mark("0.1.0+bbb"), markInZip: mark("0.1.0+bbb") };
        expect(judgeUpdateKept(base).ok).toBe(true);
        expect(judgeUpdateKept({ ...base, settingsAfter: JSON.stringify({ auto_layout: true }) }).ok).toBe(false);
        expect(judgeUpdateKept({ ...base, markAfter: mark("0.1.0+aaa") }).ok).toBe(false);
        expect(judgeUpdateKept({ ...base, markInZip: mark("0.1.0+ccc") }).ok).toBe(false);
        expect(judgeUpdateKept({ ...base, markAfter: null }).ok).toBe(false);
    });

    it("parseBuildMarkText reads the loose key: value form of build_mark.h", () => {
        expect(parseBuildMarkText("\uFEFFbuild_id:0.1.0+dev\r\nvencord_commit:   abc  \r\nother: x\n")).toEqual({ build_id: "0.1.0+dev", vencord_commit: "abc" });
        expect(parseBuildMarkText("build_id: a\nbuild_id: b\nvencord_commit: c\n")).toBeNull();
        expect(parseBuildMarkText("build_id: a\n")).toBeNull();
        expect(parseBuildMarkText(null)).toBeNull();
    });

    it("bundle-restored, named-in-texts, written-at-advanced", () => {
        expect(judgeBundleRestored({ hashes: { src: { a: "1", b: "2" }, shown: { a: "1", b: "2", c: "9" } } }).ok).toBe(true);
        expect(judgeBundleRestored({ hashes: { src: { a: "1", b: "2" }, shown: { a: "1" } } }).ok).toBe(false);
        expect(judgeBundleRestored({ hashes: { src: {}, shown: {} } }).ok).toBe(false);
        expect(judgeNamedInTexts({ texts: ["x (vencordDesktopRenderer.js)"], names: ["vencordDesktopRenderer.js"] }).ok).toBe(true);
        expect(judgeNamedInTexts({ texts: ["x"], names: ["vencordDesktopRenderer.js"] }).ok).toBe(false);
        expect(judgeNamedInTexts({ texts: [], names: [] }).ok).toBe(false);
        const after = JSON.stringify({ previous: null, written: "C:/rt/obs/plugins/cordmux/data/vencord", written_at: 11, dll_dir: "C:/rt/obs/plugins/cordmux" });
        expect(judgeWrittenAtAdvanced({ backupBefore, backupAfter: after }).ok).toBe(true);
        expect(judgeWrittenAtAdvanced({ backupBefore, backupAfter: backupBefore }).ok).toBe(false);
        const moved = JSON.stringify({ previous: null, written: "C:/else", written_at: 11, dll_dir: "C:/rt/obs/plugins/cordmux" });
        expect(judgeWrittenAtAdvanced({ backupBefore, backupAfter: moved }).ok).toBe(false);
    });

    it("overwrite-refused and ffmpeg-modules", () => {
        expect(judgeOverwriteRefused({ placeReport: { failed: [{ dest: "C:\\rt\\obs\\plugins\\cordmux\\cordmux.dll" }] }, hashes: {} }).ok).toBe(true);
        expect(judgeOverwriteRefused({ placeReport: { failed: [] }, hashes: { placed_dll: "a", zip_dll: "b" } }).ok).toBe(true);
        expect(judgeOverwriteRefused({ placeReport: { failed: [] }, hashes: { placed_dll: "a", zip_dll: "a" } }).ok).toBe(false);
        const listed = judgeFfmpegModules({ modules: ["C:\\rt\\obs\\bin\\64bit\\avcodec-61.dll", "C:\\rt\\obs\\plugins\\cordmux\\avformat-61.dll", "C:\\x\\swscale-8.dll"], obsDir: "C:\\rt\\obs" });
        expect(listed.ok).toBe(true);
        expect(listed.modules.map((m: { where: string }) => m.where)).toEqual(["obs-bin", "plugin-dir", "other"]);
        expect(judgeFfmpegModules({ modules: [], obsDir: "C:\\rt\\obs" }).ok).toBe(false);
        expect(judgeRoundtrip("no-such-judge", {}).ok).toBe(false);
    });
});
