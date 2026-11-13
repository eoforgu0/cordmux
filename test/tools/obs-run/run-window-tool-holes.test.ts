/*
 * 実際の通話での確認で見つかったテストのツールの不具合 4 つの修正を確認する。
 *   1  実行をまたいで残る保存状態 (seen_people.json など) を、初期設定 (seed-config.py) が実行の始めに消す。
 *   2  診断情報の書き出しの前に同じ名前の古い zip を消し、zip がこの実行の書き出しの後に書かれたかを確認する。
 *   3  アンインストールの後にも Vencord の設定を実行の始めのコピーと比べ、違えば失敗にする。
 *   4  テスト用の送信側の Electron の userData を _work/appdata の下に向ける。
 * run-window-plan.mjs は Node の API を呼ばないので、純関数には値だけを渡す。実行スクリプト (run-window.mjs) と
 * seed-config.py の配線はファイルのテキストで確認する (OBS と Electron を起動しないと動かないため。seed-config.py の動きそのものは
 * seed-config.py --selftest の (8c) が確認する)。
 */
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

import {
    HARNESS_USER_DATA_DIR_NAME,
    VENCORD_SETTINGS_AFTER_NAME,
    VENCORD_SETTINGS_AFTER_UNINSTALL_NAME,
    VENCORD_SETTINGS_BEFORE_NAME,
    harnessElectronArgs,
    judgeBundleZipFresh,
    judgeVencordSettingsRestored,
    parseArgs,
    planSteps,
} from "./run-window-plan.mjs";

type Step = { id: string; kind: string; [key: string]: unknown };

const HERE = dirname(fileURLToPath(import.meta.url));
const read = (name: string) => readFileSync(join(HERE, name), "utf8");
const RUN_WINDOW = read("run-window.mjs");
const SEED_CONFIG = read("seed-config.py");

const BASE = ["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-1"];
const REAL = [...BASE, "--real-profile", "--call-window", "--decoder", "software"];
const STAGE = ["--n-screen", "3", "--n-voice", "3", "--broadcaster-id", "1", "--start-with-add", "--vesktop-share"];

function plan(argv: string[]) {
    const parsed = parseArgs(argv);
    expect(parsed.errors).toEqual([]);
    const r = planSteps(parsed.args);
    expect(r.ok).toBe(true);
    return r.steps as Step[];
}

describe("不具合 1: 前の回の保存状態を初期設定で消す", () => {
    const listed = /STALE_STATE_FILES = \[([^\]]*)\]/.exec(SEED_CONFIG);

    it("消す物はこれまでに見た人の一覧・録画の保存先の候補 (枠の割り当て表は保存されない)。Vesktop のバックアップは消さない", () => {
        expect(listed).not.toBeNull();
        const names = Array.from(listed![1].matchAll(/[a-z_]+\.json/g)).map((m) => m[0]);
        expect(names).toEqual(["seen_people.json", "record_pending_dirs.json"]);
        expect(names).not.toContain("vencord_dir_backup.json");
    });

    it("消すのは書き込みの前で、dry-run は消さない (計画の staleStateFiles に出すだけ)", () => {
        const main = SEED_CONFIG.slice(SEED_CONFIG.indexOf("def main("));
        const dryRun = main.indexOf("if args.dry_run:");
        const remove = main.indexOf("remove_stale_state(plan");
        const write = main.indexOf('for path, text in plan["_files"].items()');
        expect(dryRun).toBeGreaterThan(-1);
        expect(remove).toBeGreaterThan(dryRun);
        expect(write).toBeGreaterThan(remove);
        expect(SEED_CONFIG).toContain('"staleStateFiles"');
    });

    it("test の形だけ消す (導入の往復の user の形は cordmux のものを 1 つも置かない)", () => {
        expect(SEED_CONFIG).toMatch(/if args\.shape == "test":\n\s+stale_paths = /);
    });
});

describe("不具合 2: 診断情報の zip", () => {
    it("judgeBundleZipFresh: 書き出しを始めた後に書かれた zip だけ新しい", () => {
        expect(judgeBundleZipFresh({ mtimeMs: 2000, notBeforeMs: 1000 })).toEqual({ fresh: true, reason: null });
        expect(judgeBundleZipFresh({ mtimeMs: 1000, notBeforeMs: 1000 }).fresh).toBe(true);
        expect(judgeBundleZipFresh({ mtimeMs: 999, notBeforeMs: 1000 }).fresh).toBe(false);
    });

    it("judgeBundleZipFresh: zip がない・書き出しを始めていないときは新しいと言わない", () => {
        expect(judgeBundleZipFresh({ mtimeMs: null, notBeforeMs: 1000 }).fresh).toBe(false);
        expect(judgeBundleZipFresh({ mtimeMs: 2000, notBeforeMs: null }).fresh).toBe(false);
        expect(judgeBundleZipFresh({ mtimeMs: undefined, notBeforeMs: undefined }).fresh).toBe(false);
    });

    it("dialog-sendkeys は打つ前に古い zip を消し、消せなければ打たない", () => {
        const body = RUN_WINDOW.slice(RUN_WINDOW.indexOf('case "dialog-sendkeys": {'), RUN_WINDOW.indexOf('case "screenshot-source": {'));
        const unlink = body.indexOf("fs.unlinkSync(fullPath)");
        const startedAt = body.indexOf("state.exportBundleStartedAtMs = Date.now()");
        const send = body.indexOf("DIALOG_SENDKEYS_PS1");
        expect(unlink).toBeGreaterThan(-1);
        expect(startedAt).toBeGreaterThan(unlink);
        expect(send).toBeGreaterThan(startedAt);
        expect(body.slice(unlink, startedAt)).toContain("break;");
    });

    it("check-bundle-zip は zip の更新時刻を見て、古ければ ok を false にする (成功の経路と失敗の経路の両方)", () => {
        const body = RUN_WINDOW.slice(RUN_WINDOW.indexOf('case "check-bundle-zip": {'), RUN_WINDOW.indexOf('case "mic-check": {'));
        expect(body.match(/withBundleZipFreshness\(/g)).toHaveLength(2);
        const fn = RUN_WINDOW.slice(RUN_WINDOW.indexOf("function withBundleZipFreshness"), RUN_WINDOW.indexOf("// ---- harness ("));
        expect(fn).toContain("fs.statSync(zipPath).mtimeMs");
        expect(fn).toContain("ok: result.ok === true && fresh.fresh");
    });
});

describe("不具合 3: アンインストールの後の Vencord の設定の比較", () => {
    const compareSteps = (steps: Step[]) => steps.filter((s) => s.kind === "compare-vencord-settings");

    it("実際の通話の実行では比較が 2 つ。1 つ目は Vesktop を終了した直後 (結果を残すだけ)、2 つ目は judge-vencord-dir-restored の後 (判定して失敗にする)", () => {
        const steps = plan([...REAL, ...STAGE]);
        const compares = compareSteps(steps);
        expect(compares).toHaveLength(2);
        const quit = steps.findIndex((s) => s.kind === "human" && s.quitVesktop === true);
        expect(steps[quit + 1]).toBe(compares[0]);
        expect(compares[0].mustMatch).toBeUndefined();
        expect(compares[0].gate).toBeUndefined();
        const restored = steps.findIndex((s) => s.kind === "judge-vencord-dir-restored");
        expect(steps[restored + 1]).toBe(compares[1]);
        expect(compares[1]).toMatchObject({ beforeName: VENCORD_SETTINGS_BEFORE_NAME, afterName: VENCORD_SETTINGS_AFTER_UNINSTALL_NAME, mustMatch: true, gate: true });
        expect(compares[1].afterName).not.toBe(VENCORD_SETTINGS_AFTER_NAME);
        expect(steps[restored + 2].kind).toBe("human");
    });

    it("コピーしない実行 (リハーサル・--start-with-add なし) には比較がない", () => {
        expect(compareSteps(plan([...BASE, "--rehearsal", ...STAGE]))).toHaveLength(0);
        expect(compareSteps(plan([...REAL, "--broadcaster-id", "1"]))).toHaveLength(0);
    });

    it("judgeVencordSettingsRestored: 違いが無ければ合格、あれば失敗", () => {
        expect(judgeVencordSettingsRestored({ copied: true, compared: { ok: true, changed: 0 } })).toEqual({ ok: true, reason: null });
        const red = judgeVencordSettingsRestored({ copied: true, compared: { ok: true, changed: 2 } });
        expect(red.ok).toBe(false);
        expect(red.reason).toContain("2 settings key(s) differ");
    });

    it("judgeVencordSettingsRestored: 実行の始めのコピーがない・比較が失敗したときは失敗 (違いがないとは言えない)", () => {
        expect(judgeVencordSettingsRestored({ copied: false, compared: { ok: true, changed: 0 } }).ok).toBe(false);
        expect(judgeVencordSettingsRestored({ copied: true, compared: { ok: false, error: "a settings file is not valid JSON" } }).ok).toBe(false);
        expect(judgeVencordSettingsRestored({ copied: true, compared: null }).ok).toBe(false);
    });

    it("実行スクリプトは mustMatch の比べを判定に通す", () => {
        const fn = RUN_WINDOW.slice(RUN_WINDOW.indexOf("function compareVencordSettings"), RUN_WINDOW.indexOf("const placedListPath"));
        expect(fn).toContain("s.mustMatch");
        expect(fn).toContain("judgeVencordSettingsRestored(");
    });
});

describe("不具合 4: テスト用の送信側の userData", () => {
    it("harnessElectronArgs: --user-data-dir を渡し、アプリの場所 (.) を続ける", () => {
        expect(harnessElectronArgs("C:/work/appdata/" + HARNESS_USER_DATA_DIR_NAME)).toEqual(["--user-data-dir=C:/work/appdata/cordmux-harness", "."]);
    });

    it("実行スクリプトは _work/appdata の下を渡して起動する。実際のプロファイルの側に向く起動はない", () => {
        const launch = RUN_WINDOW.slice(RUN_WINDOW.indexOf("function launchHarness"), RUN_WINDOW.indexOf("async function killHarnessTree"));
        expect(launch).toContain("path.join(toAbs(APPDATA_OVERRIDE_REL), HARNESS_USER_DATA_DIR_NAME)");
        expect(launch).toContain("spawn(electronBin, harnessElectronArgs(userDataDir)");
        expect(RUN_WINDOW).not.toContain('spawn(electronBin, ["."]');
    });
});
