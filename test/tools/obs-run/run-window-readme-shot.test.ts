/*
 * 実際の OBS の画面を撮るための hold の旗 (--screen-names / --screen-images / --voice-users / --label-em-ratio-pct /
 * --wire-composite) を確認する。引数の検査と実行計画は run-window-plan.mjs の純粋関数で、OBS を起動しない。実行スクリプト
 * (run-window.mjs) と seed-config.py は、ほかのテストと同じくファイルのテキストで確認する (OBS を起動しないと動かないため)。
 * 名前の大きさの範囲は、製品 (src-obs/config/settings.h) と実行計画と seed-config.py の 3 か所が同じであることも確認する。
 */
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

import {
    HARNESS_VOICE_TONE_ENV,
    HOLD_SCREEN_NAME_MAX,
    LABEL_EM_RATIO_PCT_MAX,
    LABEL_EM_RATIO_PCT_MIN,
    VOICE_TONE_FILE_PREFIX,
    VOICE_TONE_FILE_SUFFIX,
    parseArgs,
    parseLabelEmRatioPct,
    planSteps,
    screenImageFiles,
    screenImagesProblem,
    screenNamesProblem,
    voiceUsersBeyondScreens,
    voiceUsersProblem,
} from "./run-window-plan.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = join(HERE, "..", "..", "..");
const BASE = ["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin"];
const HOLD = [...BASE, "--step", "hold"];
const IMAGES = "_work/tmp/tiles/tile_tl.png,_work/tmp/tiles/tile_tr.png,_work/tmp/tiles/tile_bl.png,_work/tmp/tiles/tile_br.png";
// 撮影の 1 回の呼び出しと同じ形 (表示名は 4 等分の左上から名前の順になる並び)。
const SHOT = [
    ...HOLD,
    "--assign-mode", "auto",
    "--label-em-ratio-pct", "4",
    "--wire-composite",
    "--n-screen", "4",
    "--n-voice", "1",
    "--screen-sizes", "1280x720,1280x720,1280x720,1280x720",
    "--screen-names", "Mika,Rin,さくら,ゆず",
    "--screen-images", IMAGES,
    "--voice-users", "harness-screen-1",
    "--hold-minutes", "8",
];

function plan(argv: string[]) {
    const parsed = parseArgs(argv);
    expect(parsed.errors).toEqual([]);
    const r = planSteps(parsed.args);
    expect(r.ok).toBe(true);
    return { args: parsed.args, steps: r.steps as Array<Record<string, any>> };
}

const errorsOf = (argv: string[]) => parseArgs(argv).errors.join(" ");

describe("撮影の 1 回の呼び出しの実行計画", () => {
    it("初期設定に名前の大きさと自動の載せ方を渡し、接続の後に合成のソースを置いてから送信側を繋ぐ", () => {
        const { steps } = plan(SHOT);
        const seed = steps.find((s) => s.kind === "seed-config")!;
        expect(seed).toMatchObject({ assignMode: "auto", labelEmRatioPct: 4 });
        expect(seed.label).toContain("--label-em-ratio-pct 4.0");
        const kinds = steps.map((s) => s.kind);
        expect(kinds.indexOf("obsws-connect")).toBeLessThan(kinds.indexOf("wire-composite"));
        expect(kinds.indexOf("wire-composite")).toBeLessThan(kinds.indexOf("harness-start"));
        expect(kinds.indexOf("harness-start")).toBeLessThan(kinds.indexOf("hold-info"));
        expect(kinds.filter((k) => k === "wire-composite")).toHaveLength(1);
    });

    it("送信側に表示名と画像と声の持ち主を渡し、声に音を乗せる通知ファイルの場所を決めさせる", () => {
        const hs = plan(SHOT).steps.find((s) => s.kind === "harness-start")!;
        expect(hs).toMatchObject({
            nScreen: 4,
            nVoice: 1,
            framesPause: true,
            voiceTone: true,
            extra: {
                screenSizes: "1280x720,1280x720,1280x720,1280x720",
                screenNames: "Mika,Rin,さくら,ゆず",
                screenImages: IMAGES,
                voiceUsers: "harness-screen-1",
            },
        });
        expect(hs.label).toContain("表示名 Mika,Rin,さくら,ゆず");
    });

    it("旗を渡さない hold は今までどおり (合成のソースを置かず、声の通知ファイルも決めない)", () => {
        const { steps } = plan([...HOLD, "--n-screen", "1"]);
        expect(steps.map((s) => s.kind)).not.toContain("wire-composite");
        const hs = steps.find((s) => s.kind === "harness-start")!;
        expect(hs).not.toHaveProperty("voiceTone");
        expect(hs).not.toHaveProperty("extra");
        expect(steps.find((s) => s.kind === "seed-config")).not.toHaveProperty("labelEmRatioPct");
    });

    it("--wire-composite だけでも、送信側なしで合成のソースを置く", () => {
        const kinds = plan([...HOLD, "--wire-composite"]).steps.map((s) => s.kind);
        expect(kinds).toContain("wire-composite");
        expect(kinds).not.toContain("harness-start");
    });
});

describe("hold だけが受ける", () => {
    it("ほかの step では 5 つとも断る", () => {
        for (const flags of [
            ["--n-screen", "1", "--screen-names", "a"],
            ["--n-screen", "1", "--screen-images", "a.png"],
            ["--n-voice", "1", "--voice-users", "harness-screen-1"],
            ["--label-em-ratio-pct", "4"],
            ["--wire-composite"],
        ]) {
            const r = parseArgs([...BASE, "--step", "dock-view", ...flags]);
            expect(r.ok).toBe(false);
            expect(r.errors.join(" ")).toContain("は --step hold だけ");
        }
    });

    it("--label-em-ratio-pct は初期設定が書くので --no-seed と両立しない", () => {
        expect(errorsOf([...HOLD, "--no-seed", "--label-em-ratio-pct", "4"])).toContain("--label-em-ratio-pct は --no-seed と両立しない");
        expect(parseArgs([...HOLD, "--no-seed", "--wire-composite"]).ok).toBe(true);
    });
});

describe("画面ごと・声ごとの値の数と形", () => {
    it("表示名と画像は --n-screen と一緒に、数は --n-screen まで", () => {
        expect(errorsOf([...HOLD, "--screen-names", "a,b"])).toContain("--screen-names は --n-screen と一緒に");
        expect(errorsOf([...HOLD, "--screen-images", "a.png"])).toContain("--screen-images は --n-screen と一緒に");
        expect(errorsOf([...HOLD, "--n-screen", "1", "--screen-names", "a,b"])).toContain("--screen-names の数 (2) が --n-screen (1) より多い");
        expect(errorsOf([...HOLD, "--n-screen", "1", "--screen-images", "a.png,b.png"])).toContain("--screen-images の数 (2) が --n-screen (1) より多い");
        expect(parseArgs([...HOLD, "--n-screen", "3", "--screen-names", "a,b"]).ok).toBe(true);
    });

    it("声の持ち主は --n-voice と一緒に、数は --n-voice まで、画面の人を指すならその画面がある", () => {
        expect(errorsOf([...HOLD, "--voice-users", "harness-screen-1"])).toContain("--voice-users は --n-voice と一緒に");
        expect(errorsOf([...HOLD, "--n-voice", "1", "--voice-users", "x,y"])).toContain("--voice-users の数 (2) が --n-voice (1) より多い");
        expect(errorsOf([...HOLD, "--n-screen", "2", "--n-voice", "1", "--voice-users", "harness-screen-3"])).toContain("harness-screen-3 は送信側の画面にいない");
        expect(parseArgs([...HOLD, "--n-screen", "2", "--n-voice", "1", "--voice-users", "harness-screen-2"]).ok).toBe(true);
        expect(parseArgs([...HOLD, "--n-voice", "1", "--voice-users", "someone"]).ok).toBe(true);
    });

    it("screenNamesProblem: 空の項目、長すぎる名前、制御文字を断る", () => {
        expect(screenNamesProblem("Mika,Rin,さくら,ゆず")).toBe(null);
        expect(screenNamesProblem("a,,b")).toContain("空の項目");
        expect(screenNamesProblem("a,")).toContain("空の項目");
        expect(screenNamesProblem("x".repeat(HOLD_SCREEN_NAME_MAX))).toBe(null);
        expect(screenNamesProblem("x".repeat(HOLD_SCREEN_NAME_MAX + 1))).toContain("文字まで");
        expect(screenNamesProblem("a\u0007b")).toContain("制御文字");
    });

    it("screenImagesProblem: 空の項目と PNG と JPEG でないファイルを断る", () => {
        expect(screenImagesProblem("C:/a/tile.PNG,b.jpg,c.jpeg")).toBe(null);
        expect(screenImagesProblem("a.png,")).toContain("空の項目");
        expect(screenImagesProblem("a.gif")).toContain(".png / .jpg / .jpeg");
    });

    it("voiceUsersProblem: 使えない文字、既定の userId、同じ userId の 2 回目を断る", () => {
        expect(voiceUsersProblem("harness-screen-2,alice_1")).toBe(null);
        expect(voiceUsersProblem("さくら")).toContain("英数字と _ と -");
        expect(voiceUsersProblem("")).toContain("空");
        expect(voiceUsersProblem("harness-voice-5")).toContain("既定の userId");
        expect(voiceUsersProblem("a,a")).toContain("2 回");
        expect(voiceUsersBeyondScreens("harness-screen-0,harness-screen-4,harness-screen-5,bob", 4)).toEqual(["harness-screen-0", "harness-screen-5"]);
    });
});

describe("名前の大きさ (--label-em-ratio-pct)", () => {
    it("1.0 から 8.5 の 0.1 刻みだけを受け、整数も受ける", () => {
        expect(parseLabelEmRatioPct("4")).toEqual({ value: 4, error: null });
        expect(parseLabelEmRatioPct("4.0")).toEqual({ value: 4, error: null });
        expect(parseLabelEmRatioPct("1.0").value).toBe(1);
        expect(parseLabelEmRatioPct("8.5").value).toBe(8.5);
        for (const bad of ["0.9", "8.6", "4.05", "abc", "", "-4", "4."]) expect(parseLabelEmRatioPct(bad).error).not.toBe(null);
        expect(errorsOf([...HOLD, "--label-em-ratio-pct", "9"])).toContain("--label-em-ratio-pct は 1.0 から 8.5");
    });

    it("範囲は製品 (settings.h)、実行計画、seed-config.py で同じ", () => {
        const settings = readFileSync(join(REPO, "src-obs", "config", "settings.h"), "utf8");
        expect(settings).toMatch(new RegExp("kLabelEmRatioMinPct = " + LABEL_EM_RATIO_PCT_MIN.toFixed(1) + ";"));
        expect(settings).toMatch(new RegExp("kLabelEmRatioMaxPct = " + LABEL_EM_RATIO_PCT_MAX.toFixed(1) + ";"));
        const seed = readFileSync(join(HERE, "seed-config.py"), "utf8");
        expect(seed).toContain("LABEL_EM_RATIO_PCT_MIN = " + LABEL_EM_RATIO_PCT_MIN.toFixed(1));
        expect(seed).toContain("LABEL_EM_RATIO_PCT_MAX = " + LABEL_EM_RATIO_PCT_MAX.toFixed(1));
        expect(seed).toContain('settings["label_em_ratio_pct"]');
    });
});

describe("画像のファイルの確認 (OBS を起動する前)", () => {
    it("場所を絶対にし、ないファイルを全部挙げる", () => {
        const toAbs = (p: string) => (p.startsWith("C:/") ? p : "C:/repo/" + p);
        const exists = (p: string) => p !== "C:/repo/b.png";
        expect(screenImageFiles(" a.png , b.png,C:/x/c.png", toAbs, exists)).toEqual({
            files: ["C:/repo/a.png", "C:/repo/b.png", "C:/x/c.png"],
            problems: ["--screen-images の画像がない: C:/repo/b.png"],
        });
        expect(screenImageFiles("a.png", toAbs, () => true).problems).toEqual([]);
    });
});

describe("実行スクリプトの受け渡し (run-window.mjs のテキスト)", () => {
    const text = readFileSync(join(HERE, "run-window.mjs"), "utf8");

    it("送信側に 3 つの環境変数と声の通知ファイルの場所を渡し、画像の場所は絶対にする", () => {
        expect(text).toContain("harnessEnv.HARNESS_SCREEN_NAMES = step.extra.screenNames");
        expect(text).toContain("harnessEnv.HARNESS_SCREEN_IMAGES = screenImageFiles(step.extra.screenImages, toAbs");
        expect(text).toContain("harnessEnv.HARNESS_VOICE_USERS = step.extra.voiceUsers");
        expect(text).toContain("harnessEnv[HARNESS_VOICE_TONE_ENV] = step.extra.voiceToneFile");
        expect(HARNESS_VOICE_TONE_ENV).toBe("CORDMUX_HARNESS_VOICE_TONE_FILE");
        expect(text).toContain("VOICE_TONE_FILE_PREFIX + tag + VOICE_TONE_FILE_SUFFIX");
        expect(VOICE_TONE_FILE_PREFIX + "x" + VOICE_TONE_FILE_SUFFIX).toBe("voice-tone-x.flag");
    });

    it("hold-info に声の通知ファイルの場所を書き、finally で消す。OBS の前に画像を確かめる。初期設定に名前の大きさを渡す", () => {
        expect(text).toMatch(/voiceToneFile: state\.harnessChild && state\.voiceToneFile \? state\.voiceToneFile : null/);
        expect(text).toContain("fs.unlinkSync(state.voiceToneFile)");
        expect(text.indexOf("screenImageFiles(args.screenImages, toAbs, (file) => fs.existsSync(file))")).toBeLessThan(text.indexOf("const planResult = planSteps(args);"));
        expect(text).toContain('argv.push("--label-em-ratio-pct", s.labelEmRatioPct.toFixed(1))');
    });

    it("送信側 (test/tools/harness/preload.js) は同じ 3 つの環境変数を読む", () => {
        const preload = readFileSync(join(REPO, "test", "tools", "harness", "preload.js"), "utf8");
        for (const name of ["HARNESS_SCREEN_NAMES", "HARNESS_SCREEN_IMAGES", "HARNESS_VOICE_USERS"]) expect(preload).toContain("process.env." + name);
        const main = readFileSync(join(REPO, "test", "tools", "harness", "main.js"), "utf8");
        expect(main).toContain("process.env." + HARNESS_VOICE_TONE_ENV);
        expect(main).toContain("readScreenImageFile(process.env.HARNESS_SCREEN_IMAGES");
    });
});
