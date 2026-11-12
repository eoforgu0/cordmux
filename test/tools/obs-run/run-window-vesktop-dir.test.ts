/*
 * 製品 (cordmux.dll) は環境変数の APPDATA を読まず、Vesktop の設定の場所を VENCORD_USER_DATA_DIR か Windows の既知のフォルダーで
 * 決める。実行スクリプトが VENCORD_USER_DATA_DIR を作業のフォルダーの下へ向けずに OBS を起動すると、製品が利用者の実際の
 * %APPDATA%\Vesktop\state.json を書き換える。次の 2 つで守る。
 *  - vesktopDirSwapProblem (純関数): 作業のフォルダーの下でなければ理由を返す。run-window.mjs の overriddenEnv が OBS を起動する前に呼ぶ。
 *  - 実行スクリプトの配線 (ファイルのテキストで確認する。OBS を起動しないと動かないため。run-window-work-paths.test.ts と同じ形)。
 */
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

import { VESKTOP_DATA_DIR_REL, VESKTOP_STATE_JSON_REL, vesktopDirSwapProblem } from "./run-window-plan.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const WORK = "C:\\Users\\someone\\cordmux-next\\_work";
const text = (name: string) => readFileSync(join(HERE, name), "utf-8");

describe("vesktopDirSwapProblem: VENCORD_USER_DATA_DIR が作業のフォルダーの下でなければ止める", () => {
    it("作業のフォルダーの下は通す", () => {
        expect(vesktopDirSwapProblem({ VENCORD_USER_DATA_DIR: WORK + "\\appdata\\Vesktop" }, WORK)).toBeNull();
        expect(vesktopDirSwapProblem({ VENCORD_USER_DATA_DIR: WORK + "/appdata/Vesktop" }, WORK)).toBeNull();
    });

    it("未設定、空は止める (製品が実際の Vesktop の設定を使う)", () => {
        expect(vesktopDirSwapProblem({}, WORK)).toMatch(/not set/);
        expect(vesktopDirSwapProblem({ VENCORD_USER_DATA_DIR: "" }, WORK)).toMatch(/not set/);
        expect(vesktopDirSwapProblem({ VENCORD_USER_DATA_DIR: "  " }, WORK)).toMatch(/not set/);
        expect(vesktopDirSwapProblem(undefined, WORK)).toMatch(/not set/);
    });

    it("実際の APPDATA の下、作業のフォルダーそのもの、隣のフォルダー、.. を含む場所は止める", () => {
        expect(
            vesktopDirSwapProblem({ VENCORD_USER_DATA_DIR: "C:\\Users\\someone\\AppData\\Roaming\\Vesktop" }, WORK),
        ).toMatch(/not under/);
        expect(vesktopDirSwapProblem({ VENCORD_USER_DATA_DIR: WORK }, WORK)).toMatch(/not under/);
        expect(vesktopDirSwapProblem({ VENCORD_USER_DATA_DIR: WORK + "2\\appdata\\Vesktop" }, WORK)).toMatch(
            /not under/,
        );
        expect(vesktopDirSwapProblem({ VENCORD_USER_DATA_DIR: WORK + "\\..\\Vesktop" }, WORK)).toMatch(/not under/);
    });
});

describe("差し替え先の定数", () => {
    it("差し替えた Vesktop のフォルダーは APPDATA の差し替え先の下で、state.json はその直下", () => {
        expect(VESKTOP_DATA_DIR_REL).toBe("_work/appdata/Vesktop");
        expect(VESKTOP_STATE_JSON_REL).toBe(VESKTOP_DATA_DIR_REL + "/state.json");
    });
});

describe("実行スクリプトの配線", () => {
    const run = text("run-window.mjs");

    it("差し替えた環境変数に VENCORD_USER_DATA_DIR を入れ、同じ名前のキーを先に落とし、作業のフォルダーの下でなければ例外にする", () => {
        expect(run).toContain('VENCORD_USER_DATA_DIR: path.join(appDataDir, "Vesktop")');
        expect(run).toContain('vesktopDirSwapProblem(env, toAbs("_work"))');
        expect(run).toContain('throw new Error("refusing to run: " + problem)');
        // 差し替えの組の作り方 (落としてから入れ直す) に VENCORD_USER_DATA_DIR も含まれる。
        expect(run).toContain("Object.keys(overrides).map((k) => k.toLowerCase())");
    });

    it("実際の通話 (--real-profile) は何も差し替えない", () => {
        expect(run).toContain("if (args.realProfile) return { ...process.env };");
    });

    it("--dry-run が、製品の読む state.json と、作業のフォルダーの下かを出す", () => {
        expect(run).toContain("productStateJson");
        expect(run).toContain("vesktopDirUnderWork");
    });
});

describe("cleanup.ps1 と自己テスト", () => {
    it("-StateJsonPath の既定は VENCORD_USER_DATA_DIR を先に読む", () => {
        const ps1 = text("cleanup.ps1");
        expect(ps1).toContain("[string]$StateJsonPath = $(if ($env:VENCORD_USER_DATA_DIR)");
        expect(ps1).toContain("Join-Path $env:VENCORD_USER_DATA_DIR 'state.json'");
        expect(ps1).toContain('"$env:APPDATA\\Vesktop\\state.json"');
    });

    it("自己テストは実機の VENCORD_USER_DATA_DIR が残らないように空にしてから cleanup.ps1 を呼ぶ", () => {
        const selftest = text("cleanup.selftest.ps1");
        expect((selftest.match(/\$env:VENCORD_USER_DATA_DIR = \$null/g) ?? []).length).toBe(2);
    });
});
