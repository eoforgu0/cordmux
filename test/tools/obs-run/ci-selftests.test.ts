/*
 * obs-run のツールにセルフテストを足しても CI (.github/workflows/lint.yml) に載せ忘れると、ツールの不具合が手元の実行か
 * 実機の実行の当日まで分からない。そこで、セルフテストを持つ obs-run のツールをファイルのテキストから探し、lint.yml が
 * 全部を実行すること (実行しない物は下の除外の表に理由つきで載ること) と、PowerShell のツールは Windows の job で
 * 実行することを確認する。
 * 探し方: Python は引数の文字列 `"--selftest"` (引用符つき。argparse の旗と argv の比較の両方の書き方を拾う)、
 * PowerShell は param の `[switch]$SelfTest`。
 */
import { readdirSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

const HERE = dirname(fileURLToPath(import.meta.url));
const LINT_YML = join(HERE, "..", "..", "..", ".github", "workflows", "lint.yml");

// CI で実行しないセルフテストと理由 (lint.yml の windows-tools-selftest の先頭のコメントと同じ理由)。手元の実行では実行する。
const CI_EXCLUDED: Record<string, string> = {
    "a11y-record.ps1":
        "子のプロセスの見えないウィンドウから UI Automation のイベントが届くまで待つ形 (届かなければ約 45 秒で失敗)。hosted の runner の画面の" +
        "セッションで届くかを手元の PC から確認できず、失敗すると dist.yml の Release を止める",
};

function selfTestCommands(): Array<{ file: string; command: string; windowsOnly: boolean }> {
    const out: Array<{ file: string; command: string; windowsOnly: boolean }> = [];
    for (const name of readdirSync(HERE).sort()) {
        const text = () => readFileSync(join(HERE, name), "utf8");
        if (name.endsWith(".py") && /["']--selftest["']/.test(text())) {
            out.push({ file: name, command: "test/tools/obs-run/" + name + " --selftest", windowsOnly: false });
        } else if (name.endsWith(".ps1") && /\[switch\]\$SelfTest\b/.test(text())) {
            out.push({ file: name, command: "test/tools/obs-run/" + name + " -SelfTest", windowsOnly: true });
        }
    }
    // セルフテストが別のファイルにある物 (後始末と判定スクリプト 4 種)。
    out.push({ file: "cleanup.selftest.ps1", command: "test/tools/obs-run/cleanup.selftest.ps1", windowsOnly: true });
    out.push({ file: "selfcheck.py", command: "test/tools/obs-run/selfcheck.py", windowsOnly: false });
    return out;
}

// lint.yml を job ごとに分ける (2 文字下げの `<名前>:` の行から次の同じ形の行まで)。runs-on はその中の最初の行。
function jobsOf(yml: string): Array<{ name: string; runsOn: string; body: string }> {
    const lines = yml.split(/\r?\n/);
    const jobsAt = lines.findIndex((l) => l === "jobs:");
    const jobs: Array<{ name: string; runsOn: string; body: string }> = [];
    let cur: { name: string; lines: string[] } | null = null;
    for (const line of lines.slice(jobsAt + 1)) {
        const m = /^  ([A-Za-z0-9_-]+):\s*$/.exec(line);
        if (m) {
            if (cur) jobs.push(finish(cur));
            cur = { name: m[1], lines: [] };
        } else if (cur) {
            cur.lines.push(line);
        }
    }
    if (cur) jobs.push(finish(cur));
    return jobs;

    function finish(j: { name: string; lines: string[] }) {
        const body = j.lines.join("\n");
        const r = /^    runs-on:\s*(\S+)/m.exec(body);
        return { name: j.name, runsOn: r ? r[1] : "", body };
    }
}

describe("CI (lint.yml) は obs-run のセルフテストを全部実行する", () => {
    const yml = readFileSync(LINT_YML, "utf8");
    const jobs = jobsOf(yml);
    const commands = selfTestCommands();

    it("探し方が現在のツールを拾う (Python 12 個以上、PowerShell 6 個以上。数が減ったら探し方の文字列が壊れている)", () => {
        expect(commands.filter((c) => c.file.endsWith(".py") && c.command.endsWith("--selftest")).length).toBeGreaterThanOrEqual(12);
        expect(commands.filter((c) => c.command.endsWith("-SelfTest")).length).toBeGreaterThanOrEqual(6);
        expect(jobs.map((j) => j.name)).toContain("windows-tools-selftest");
    });

    it("除外の表にないセルフテストは lint.yml のどれかの job が実行し、PowerShell の物は Windows の job で実行する", () => {
        const missing: string[] = [];
        const wrongOs: string[] = [];
        for (const c of commands) {
            if (CI_EXCLUDED[c.file]) continue;
            const where = jobs.filter((j) => j.body.includes(c.command));
            if (where.length === 0) missing.push(c.command);
            else if (c.windowsOnly && !where.some((j) => j.runsOn.startsWith("windows-"))) wrongOs.push(c.command + " (" + where.map((j) => j.runsOn).join(",") + ")");
        }
        expect(missing).toEqual([]);
        expect(wrongOs).toEqual([]);
    });

    it("除外の表の物は実在し、lint.yml は実行せず、先頭のコメントに理由を書いている", () => {
        const files = new Set(commands.map((c) => c.file));
        for (const [file, reason] of Object.entries(CI_EXCLUDED)) {
            expect(files.has(file), file).toBe(true);
            expect(reason.length).toBeGreaterThan(0);
            const cmd = commands.find((c) => c.file === file)!.command;
            expect(jobs.some((j) => j.body.includes(cmd)), cmd).toBe(false);
            expect(yml).toContain("実行しないもの: " + file);
        }
    });

    it("PowerShell のツールの構文チェックも Windows の job で実行する", () => {
        const win = jobs.filter((j) => j.runsOn.startsWith("windows-"));
        expect(win.some((j) => j.body.includes("Language.Parser]::ParseFile") && j.body.includes("test/tools/obs-run"))).toBe(true);
    });
});
