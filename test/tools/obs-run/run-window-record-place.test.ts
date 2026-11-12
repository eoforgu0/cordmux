/*
 * 録画の場所の食い違いを防ぐ。製品は、設定の record_dir に入っている場所そのものに、録画ごとのフォルダー (<開始日時>) を作る
 * (src-obs/record/record_location.h)。seed-config.py がテスト用の OBS の設定に書く
 * record_dir と、run-window.mjs が録画を探す場所がずれると、録画を作れても判定が「録画がない」で落ちる。
 * 場所を決める純関数 (run-window-plan.mjs の recordSessionsRoot) と、実行スクリプトと seed-config.py の配線 (ファイルのテキスト。
 * OBS を起動しないと動かないため。run-window-work-paths.test.ts と同じ形) を確認する。実際の値は、run-window.mjs --dry-run の出力で
 * 実行計画の recordDirRel と seed-config.py の record_dir を並べて見られる。
 */
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

import { RECORD_DIR_REL, isRecordSessionDirName, recordSessionsRoot } from "./run-window-plan.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const read = (name: string) => readFileSync(join(HERE, name), "utf8");

describe("録画のフォルダーを並べる場所は、設定の record_dir そのもの", () => {
    it("保存先の下に cordmux などを挟まない", () => {
        const recordDir = "C:\\r\\_work\\out\\live-a\\window3b\\" + RECORD_DIR_REL;
        expect(recordSessionsRoot(recordDir)).toBe(recordDir);
    });

    it("seed-config.py の既定の保存先は <out>/<RECORD_DIR_REL> で、実行スクリプトも同じ場所から探す", () => {
        expect(RECORD_DIR_REL).toBe("recordings");
        // seed-config.py: --record-dir がなければ <out>/recordings (settings.json の record_dir と basic.ini の FilePath に書く)。
        expect(read("seed-config.py")).toContain('os.path.join(out_dir, "recordings")');
        const runner = read("run-window.mjs");
        // 録画を探す所は、どれも path.join(windowOutDir, s.recordDirRel || "recordings") を recordSessionsRoot に通す。
        expect(runner).toContain("recordSessionsRoot(recordDirAbs)");
        expect(runner).not.toMatch(/RECORD_ROOT_DIR_NAME/);
        expect(runner).not.toMatch(/path\.join\(recordDirAbs,\s*["']cordmux["']\)/);
    });

    it("実行計画は録画の保存先として RECORD_DIR_REL だけを渡す (seed-config.py の --record-dir)", () => {
        expect(read("run-window.mjs")).toContain('if (s.recordDirRel) argv.push("--record-dir", path.join(where.windowOutDir, s.recordDirRel));');
    });
});

describe("isRecordSessionDirName: 録画ごとのフォルダーの名前だけを数える", () => {
    it("日時の名前、同じ秒の 2 つ目からの番号付き、古い並びの名前", () => {
        expect(isRecordSessionDirName("2000-01-01 15-18-12")).toBe(true);
        expect(isRecordSessionDirName("2000-01-01 15-18-12 (2)")).toBe(true);
        expect(isRecordSessionDirName("20000101-151812")).toBe(true);
    });

    it("保存先の直下にあるほかのフォルダー (OBS の録画を移した obs-<tag> など) は数えない", () => {
        expect(isRecordSessionDirName("obs-call-1")).toBe(false);
        expect(isRecordSessionDirName("cordmux")).toBe(false);
        expect(isRecordSessionDirName("2000-01-01 15-18-12 backup")).toBe(false);
        expect(isRecordSessionDirName("2000-01-01")).toBe(false);
        expect(isRecordSessionDirName("")).toBe(false);
    });
});
