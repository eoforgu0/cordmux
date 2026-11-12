/*
 * run-window.mjs と seed-config.py は --obs と --out をどこでも受けると、書き違え 1 つで、利用者の OBS のフォルダーへ cordmux を
 * 置いて消す・設定を書き換える・作業の保存先の外へ書くことができてしまう。守りは run-window-plan.mjs の workPathProblems
 * (純関数) で、実行スクリプトが実行計画を組む前に呼ぶ。外で実行するときは --allow-outside-work を書く。
 * run-window-plan.mjs は Node の API を呼ばないので、確認の対象には絶対パスの文字列だけを渡す。実行スクリプトと
 * seed-config.py の配線はファイルのテキストで確認する (OBS を起動しないと動かないため。run-window-hold-flags.test.ts と同じ形)。
 * seed-config.py の守りそのものは seed-config.py --selftest の (0b) が確認する。
 */
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

import {
    ALLOW_OUTSIDE_WORK_FLAG,
    OBS_DIR_ROOTS_REL,
    OUT_DIR_ROOTS_REL,
    isPathUnderRoot,
    parseArgs,
    workPathProblems,
} from "./run-window-plan.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = "C:\\Users\\someone\\cordmux-next";
const abs = (rel: string) => REPO + "\\" + rel.replace(/\//g, "\\");
const problems = (obsAbs: string | null, outAbs: string | null, caseInsensitive = true) =>
    workPathProblems({ obsAbs, outAbs, repoRoot: REPO, caseInsensitive });

// docs/testing.md の「実機テスト」の呼び出しで使う --obs と --out。A は --out _work/out/live-a、B と C は live-b・live-c。
const RUNBOOK_OBS = ["_work/obs-33.0.1", "_work/obs-32.2.2"];
const RUNBOOK_OUT = ["_work/out/live-a", "_work/out/live-b", "_work/out/live-c"];
// dry-run のスクリプト (dry-run/cmd/dry.sh) の --out。
const GATE_OUT = "build_x64/dry-run/_work/out-hold";

describe("isPathUnderRoot: 下だけを受け、根そのもの・名前の頭が同じ隣・. と .. の段は受けない", () => {
    it("下と、区切りの違い", () => {
        expect(isPathUnderRoot("C:\\r\\_work\\obs", "C:\\r\\_work", true)).toBe(true);
        expect(isPathUnderRoot("C:/r/_work/obs", "C:\\r\\_work\\", true)).toBe(true);
        expect(isPathUnderRoot("C:\\r\\_work", "C:\\r\\_work", true)).toBe(false);
        expect(isPathUnderRoot("C:\\r\\_work\\", "C:\\r\\_work", true)).toBe(false);
        expect(isPathUnderRoot("C:\\r\\_work2\\obs", "C:\\r\\_work", true)).toBe(false);
        expect(isPathUnderRoot("C:\\r\\_work\\..\\obs", "C:\\r\\_work", true)).toBe(false);
        expect(isPathUnderRoot("C:\\r\\_work\\.\\obs", "C:\\r\\_work", true)).toBe(false);
    });

    it("大文字小文字は Windows (caseInsensitive) でだけ同じと見る", () => {
        expect(isPathUnderRoot("c:\\R\\_WORK\\obs", "C:\\r\\_work", true)).toBe(true);
        expect(isPathUnderRoot("/R/_work/obs", "/r/_work", false)).toBe(false);
        expect(isPathUnderRoot("/r/_work/obs", "/r/_work", false)).toBe(true);
    });

    it("文字列でない値・空は受けない", () => {
        expect(isPathUnderRoot("", "C:\\r", true)).toBe(false);
        expect(isPathUnderRoot("C:\\r\\x", "", true)).toBe(false);
        expect(isPathUnderRoot(null as unknown as string, "C:\\r", true)).toBe(false);
    });
});

describe("workPathProblems: --obs は _work の下、--out は _work か build_x64 の下", () => {
    it("根の並びは定義のとおり", () => {
        expect(OBS_DIR_ROOTS_REL).toEqual(["_work"]);
        expect(OUT_DIR_ROOTS_REL).toEqual(["_work", "build_x64"]);
        expect(ALLOW_OUTSIDE_WORK_FLAG).toBe("--allow-outside-work");
    });

    it("実機テストの呼び出しと dry-run の形は全部通る", () => {
        for (const obs of RUNBOOK_OBS) {
            for (const out of [...RUNBOOK_OUT, GATE_OUT]) expect(problems(abs(obs), abs(out))).toEqual([]);
        }
    });

    it("往復 (install-roundtrip) の --obs は parseArgs が入れる _work/rt-<tag>/obs で、同じ守りを通る", () => {
        const parsed = parseArgs([
            "--out", "_work/out/live-a", "--step", "install-roundtrip", "--scenario", "roundtrip", "--tag", "live311",
            "--zip", "_work/dist/cordmux-0.1.0+dev.zip", "--obs-zip", "_work/obs-zips/OBS-Studio-31.1.1-Windows-x64.zip",
        ]);
        expect(parsed.errors).toEqual([]);
        expect(problems(abs(parsed.args.obs), abs(parsed.args.out))).toEqual([]);
    });

    it("外の --obs は断る (利用者の OBS の形・偽の C:/obs・build_x64・_work そのもの・隣・..・UNC・\\\\?\\ の形)", () => {
        const outside = [
            "C:\\Program Files\\obs-studio",
            "C:/obs",
            abs("build_x64/obs"),
            abs("_work"),
            abs("_work" + "2/obs-33.0.1"),
            abs("_work/../obs-33.0.1"),
            "\\\\server\\share\\_work\\obs",
            "\\\\?\\" + abs("_work/obs-33.0.1"),
        ];
        for (const obs of outside) {
            const got = problems(obs, abs(RUNBOOK_OUT[0]));
            expect(got.length, obs).toBe(1);
            expect(got[0]).toContain("--obs は repo の _work の下だけ");
            expect(got[0]).toContain(ALLOW_OUTSIDE_WORK_FLAG);
        }
    });

    it("外の --out は断る (repo の外・build_x64 そのもの・repo の根・src-obs)", () => {
        for (const out of ["C:/out", "D:\\recordings", abs("build_x64"), REPO, abs("src-obs/build")]) {
            const got = problems(abs(RUNBOOK_OBS[0]), out);
            expect(got.length, out).toBe(1);
            expect(got[0]).toContain("--out は repo の _work か build_x64 の下だけ");
        }
    });

    it("両方が外なら 2 つ並べる。null の側は確認しない (要るかどうかは parseArgs が見る)", () => {
        expect(problems("C:/obs", "C:/out")).toHaveLength(2);
        expect(problems(null, abs(RUNBOOK_OUT[0]))).toEqual([]);
        expect(problems(abs(RUNBOOK_OBS[0]), null)).toEqual([]);
    });

    it("大文字小文字の違いは Windows でだけ通る", () => {
        expect(problems(abs(RUNBOOK_OBS[0]).toUpperCase(), abs(RUNBOOK_OUT[0]).toLowerCase(), true)).toEqual([]);
        expect(problems(abs(RUNBOOK_OBS[0]).toUpperCase(), abs(RUNBOOK_OUT[0]), false)).toHaveLength(1);
    });
});

describe("--allow-outside-work (外で実行する明示の旗)", () => {
    it("既定は false。どの step でも受け、結果 JSON の args に残る", () => {
        const base = ["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin"];
        expect(parseArgs([...base, "--step", "hold"]).args.allowOutsideWork).toBe(false);
        const r = parseArgs([...base, "--step", "hold", ALLOW_OUTSIDE_WORK_FLAG]);
        expect(r.errors).toEqual([]);
        expect(r.args.allowOutsideWork).toBe(true);
        const rt = parseArgs([
            "--out", "C:/out", "--step", "install-roundtrip", "--tag", "t1", "--zip", "C:/z/c.zip", "--obs-zip", "C:/z/o.zip", ALLOW_OUTSIDE_WORK_FLAG,
        ]);
        expect(rt.errors).toEqual([]);
        expect(rt.args.allowOutsideWork).toBe(true);
    });
});

describe("配線 (ファイルのテキストで確認する)", () => {
    const runWindow = readFileSync(join(HERE, "run-window.mjs"), "utf8");
    const seed = readFileSync(join(HERE, "seed-config.py"), "utf8");
    const mainBody = runWindow.slice(runWindow.indexOf("async function main()"));

    it("実行スクリプトは main で、実行計画を組む前・dry-run の前に守りを見て、旗が無ければ断って終える", () => {
        const guard = mainBody.indexOf("workPathProblems(");
        expect(guard).toBeGreaterThan(0);
        expect(guard).toBeLessThan(mainBody.indexOf("planSteps(args)"));
        expect(guard).toBeLessThan(mainBody.indexOf("if (args.dryRun)"));
        expect(guard).toBeLessThan(mainBody.indexOf("evaluateGuard("));
        const block = mainBody.slice(mainBody.indexOf("if (!args.allowOutsideWork)"), mainBody.indexOf("planSteps(args)"));
        expect(block).toContain("workPathProblems(");
        expect(block).toContain("repoRoot: REPO_ROOT");
        expect(block).toContain("toAbs(args.obs)");
        expect(block).toContain("toAbs(args.out)");
        expect(block).toContain("process.exitCode = 1");
        expect(block).toContain("return;");
    });

    it("旗を受けた回だけ seed-config.py に同じ旗を渡す (dry-run の seedPreview も同じ関数)", () => {
        const fn = runWindow.slice(runWindow.indexOf("function seedConfigArgv("), runWindow.indexOf("\n}\n", runWindow.indexOf("function seedConfigArgv(")));
        expect(fn).toContain("if (args.allowOutsideWork) argv.push(ALLOW_OUTSIDE_WORK_FLAG)");
        expect(runWindow).toContain("[--allow-outside-work");
    });

    it("seed-config.py は同じ旗・同じ根を持ち、外は終了コード 5 で何も書かずに止まる", () => {
        expect(seed).toContain('ALLOW_OUTSIDE_WORK_FLAG = "' + ALLOW_OUTSIDE_WORK_FLAG + '"');
        const py = (rels: string[]) => "(" + rels.map((r) => "(" + r.split("/").map((p) => '"' + p + '"').join(", ") + (r.includes("/") ? "" : ",") + ")").join(", ") + (rels.length === 1 ? "," : "") + ")";
        expect(seed).toContain("OBS_DIR_ROOTS = " + py(OBS_DIR_ROOTS_REL));
        expect(seed).toContain("OUT_DIR_ROOTS = " + py(OUT_DIR_ROOTS_REL));
        expect(seed).toContain("EXIT_WORK_PATH_GUARD = 5");
        const plan = seed.slice(seed.indexOf("def build_plan("));
        const guard = plan.indexOf("work_path_problems(obs_dir, out_dir, REPO_ROOT)");
        expect(guard).toBeGreaterThan(0);
        expect(guard).toBeLessThan(plan.indexOf("record_dir_is_inside_work(record_dir)"));
    });
});
