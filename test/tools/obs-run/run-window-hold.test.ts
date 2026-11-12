/*
 * 実機の実行の準備 (docs/testing.md の「実機テスト」) のうち、次を確認する。
 *   - 録画の印は録画のボタンだけが打つこと (recordMarkOf)。OBS の起動待ちの 207 を見分けること (isObsNotReadyError)。
 *     後者は正規表現の \b が後退の文字 (U+0008) としてファイルに入っていて気づかれなかったことがあるので、ツールの
 *     ファイルに制御文字がないことも確認する。
 *   - --step hold と --no-seed、OBS の見た目と言語の旗、画面のストリームごとのサイズの受け渡し。
 * run-window-plan.mjs は Node の API を呼ばないので、確認の対象には値だけを渡す。このテスト自体は、
 * seed-config.py の切り替え先の名前と製品の ini と、ツールのファイルの中身を読む (名前を 2 か所で決めないため)。
 */
import { readdirSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

import {
    HOLD_CLOSE_MARGIN_MIN,
    HOLD_DEFAULT_MINUTES,
    HOLD_HARNESS_MARGIN_S,
    DUMMY_HDR_RECORDING_LABEL,
    DUMMY_PLATE_BG,
    TEXT_ID_RECORD_START,
    TEXT_ID_RECORD_STOP,
    isObsNotReadyError,
    parseArgs,
    planSteps,
    recordMarkOf,
} from "./run-window-plan.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = join(HERE, "..", "..", "..");
const BASE = ["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin"];

function iniKeys(file: string): Set<string> {
    const keys = new Set<string>();
    for (const line of readFileSync(file, "utf8").split(/\r?\n/)) {
        const at = line.indexOf("=");
        if (at > 0 && !line.startsWith(";")) keys.add(line.slice(0, at).trim());
    }
    return keys;
}

function plan(argv: string[]) {
    const parsed = parseArgs([...BASE, ...argv]);
    expect(parsed.errors).toEqual([]);
    const r = planSteps(parsed.args);
    expect(r.ok).toBe(true);
    return r.steps as Array<Record<string, any>>;
}

describe("OBS の起動待ちの 207 (NotReady) の見分け", () => {
    it("obsws.mjs の失敗メッセージの 207 には当たり、数の一部には当たらない", () => {
        expect(isObsNotReadyError("GetVersion: 207 NotReady")).toBe(true);
        expect(isObsNotReadyError("GetVersion: 207")).toBe(true);
        expect(isObsNotReadyError("GetVersion: 1207 x")).toBe(false);
        expect(isObsNotReadyError("GetVersion: 2070 x")).toBe(false);
        expect(isObsNotReadyError("GetVersion: 204 InvalidRequestType")).toBe(false);
        expect(isObsNotReadyError(undefined)).toBe(false);
    });

    it("run-window.mjs は見分けを isObsNotReadyError に任せ、正規表現を自分で持たない", () => {
        const text = readFileSync(join(HERE, "run-window.mjs"), "utf8");
        expect(text).toContain("isObsNotReadyError(e.message)");
        expect(text).not.toMatch(/\/[^\n/]*207[^\n/]*\/\.test/);
    });

    it("ツールのテキストファイルに制御文字 (タブ・改行・復帰を除く) がない", () => {
        const bad: string[] = [];
        for (const name of readdirSync(HERE)) {
            if (!/\.(mjs|ts|py|ps1|md|jsx|html)$/.test(name)) continue;
            const text = readFileSync(join(HERE, name), "utf8");
            // eslint-disable-next-line no-control-regex
            const m = text.match(/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/);
            if (m) bad.push(name + " U+" + m[0].charCodeAt(0).toString(16).padStart(4, "0"));
        }
        expect(bad).toEqual([]);
    });
});

describe("録画の印は録画のボタンを押す手順だけ", () => {
    it("録画のボタンの開始と終了だけが印を打つ", () => {
        expect(recordMarkOf({ kind: "dock-click", textId: TEXT_ID_RECORD_START, desired: "on" })).toBe("on");
        expect(recordMarkOf({ kind: "dock-click", textId: TEXT_ID_RECORD_STOP, desired: "off" })).toBe("off");
    });

    it("行のチェック (名前で押す、desired を持つ) と、ほかのボタンは印を打たない", () => {
        expect(recordMarkOf({ kind: "dock-click", name: "harness-screen-1", desired: "on", slotOp: true })).toBe(null);
        expect(recordMarkOf({ kind: "dock-click", name: "harness-screen-1", desired: "off", slotOp: true })).toBe(null);
        expect(recordMarkOf({ kind: "dock-click", textId: "cordmux.dock.reinstall.button" })).toBe(null);
        expect(recordMarkOf({ kind: "dialog-click", textId: TEXT_ID_RECORD_START, desired: "on" })).toBe(null);
        expect(recordMarkOf(null)).toBe(null);
    });

    it("select-four の行のチェックはどれも印を打たず、--step record の録画のボタンは打つ", () => {
        const four = plan(["--step", "dock-view", "--dock-scenario", "select-four"]).filter((s) => s.kind === "dock-click");
        expect(four.length).toBe(4);
        expect(four.map(recordMarkOf)).toEqual([null, null, null, null]);
        const rec = plan(["--step", "record"]).filter((s) => s.kind === "dock-click").map(recordMarkOf);
        // 1 回目と、同じ OBS の起動の中での 2 回目。印を読む OBS 側の黒の判定は、2 回目より前に済む。
        expect(rec).toEqual(["on", "off", "on", "off"]);
    });
});

describe("--step hold", () => {
    it("既定: 初期設定 -> 配置 -> 起動 -> 接続 -> hold-info -> 通知ファイルを待つ -> ログ -> 閉じる -> 後始末。送信側は起動しない", () => {
        const steps = plan(["--step", "hold"]);
        const kinds = steps.map((s) => s.kind);
        expect(kinds[0]).toBe("seed-config");
        expect(kinds).not.toContain("harness-start");
        const at = (k: string) => kinds.indexOf(k);
        expect(at("launch-obs")).toBeLessThan(at("obsws-connect"));
        expect(at("obsws-connect")).toBeLessThan(at("hold-info"));
        expect(at("hold-info")).toBeLessThan(at("human"));
        expect(at("human")).toBeLessThan(at("close-obs"));
        expect(kinds.slice(-3)).toEqual(["write-result", "summarize", "cleanup"]);
        const human = steps.find((s) => s.kind === "human")!;
        expect(human.until).toBe("go-file");
        expect(human.timeoutMs).toBe(HOLD_DEFAULT_MINUTES * 60 * 1000);
        expect(human.gate).toBeUndefined();
    });

    it("--no-seed は seed-config を持たず、配置の 3 件だけ", () => {
        const kinds = plan(["--step", "hold", "--no-seed"]).map((s) => s.kind);
        expect(kinds).not.toContain("seed-config");
        expect(kinds.slice(0, 3)).toEqual(["copy-file", "copy-dir-contents", "copy-dir"]);
    });

    it("--n-screen を渡すと送信側を待ちより長く繋ぎ、待ちの後に止める。--screen-sizes は送信側へ渡る", () => {
        const steps = plan(["--step", "hold", "--hold-minutes", "20", "--n-screen", "2", "--screen-sizes", "1280x720,3840x2160"]);
        const hs = steps.find((s) => s.kind === "harness-start")!;
        expect(hs).toMatchObject({ nScreen: 2, nVoice: 0, seconds: 20 * 60 + HOLD_HARNESS_MARGIN_S, extra: { screenSizes: "1280x720,3840x2160" } });
        const kinds = steps.map((s) => s.kind);
        expect(kinds.indexOf("human")).toBeLessThan(kinds.indexOf("harness-stop"));
        expect(kinds.indexOf("harness-stop")).toBeLessThan(kinds.indexOf("close-obs"));
    });

    it("hold だけの旗はほかの step では断る", () => {
        for (const flags of [["--no-seed"], ["--hold-minutes", "10"], ["--n-screen", "1", "--screen-sizes", "1280x720"]]) {
            const r = parseArgs([...BASE, "--step", "dock-view", ...flags]);
            expect(r.ok).toBe(false);
            const joined = r.errors.join(" ");
            expect(joined.includes("--step hold だけ") || joined.includes("--step hold / --step call-hold だけ")).toBe(true);
        }
    });

    it("待ちの長さは期限から閉じる分を引いた長さまで。--screen-sizes は形と本数を確認する", () => {
        expect(parseArgs([...BASE, "--step", "hold", "--hold-minutes", String(120 - HOLD_CLOSE_MARGIN_MIN)]).ok).toBe(true);
        expect(parseArgs([...BASE, "--step", "hold", "--hold-minutes", String(120 - HOLD_CLOSE_MARGIN_MIN + 1)]).ok).toBe(false);
        expect(parseArgs([...BASE, "--step", "hold", "--hold-minutes", "100", "--deadline-min", "115"]).ok).toBe(true);
        expect(parseArgs([...BASE, "--step", "hold", "--hold-minutes", "0"]).ok).toBe(false);
        expect(parseArgs([...BASE, "--step", "hold", "--n-screen", "1", "--screen-sizes", "1280*720"]).ok).toBe(false);
        expect(parseArgs([...BASE, "--step", "hold", "--n-screen", "1", "--screen-sizes", "1280x720,640x360"]).ok).toBe(false);
        expect(parseArgs([...BASE, "--step", "hold", "--screen-sizes", "1280x720"]).ok).toBe(false);
    });
});

describe("OBS の見た目と言語の旗", () => {
    it("hold は 6 つとも seed-config の手順へ渡す", () => {
        const seed = plan([
            "--step", "hold", "--obs-language", "en-US", "--obs-theme", "com.obsproject.Yami.Light",
            "--obs-font-scale", "10", "--obs-density", "-2", "--obs-hdr", "--obs-full-height-docks", "off",
        ]).find((s) => s.kind === "seed-config")!;
        expect(seed.obsLook).toEqual({ language: "en-US", theme: "com.obsproject.Yami.Light", fontScale: 10, density: -2, hdr: true, fullHeightDocks: false });
    });

    it("--obs-full-height-docks は on か off だけを受け、1 つだけでも seed-config の手順へ渡す", () => {
        const look = (value: string) => plan(["--step", "hold", "--obs-full-height-docks", value]).find((s) => s.kind === "seed-config")!.obsLook;
        expect(look("on").fullHeightDocks).toBe(true);
        expect(look("off").fullHeightDocks).toBe(false);
        expect(parseArgs([...BASE, "--step", "hold", "--obs-full-height-docks", "false"]).ok).toBe(false);
        expect(parseArgs([...BASE, "--step", "record", "--obs-full-height-docks", "off"]).ok).toBe(false);
    });

    it("渡さなければ seed-config の手順は obsLook を持たない (前と同じ形)", () => {
        expect(plan(["--step", "dock-view"]).find((s) => s.kind === "seed-config")!.obsLook).toBeUndefined();
    });

    it("見た目は dock-view でも受け、言語は hold だけ。--no-seed とは両立しない", () => {
        expect(plan(["--step", "dock-view", "--dock-scenario", "dummy", "--obs-hdr"]).find((s) => s.kind === "seed-config")!.obsLook.hdr).toBe(true);
        expect(parseArgs([...BASE, "--step", "dock-view", "--obs-language", "en-US"]).ok).toBe(false);
        expect(parseArgs([...BASE, "--step", "record", "--obs-theme", "com.obsproject.Yami"]).ok).toBe(false);
        expect(parseArgs([...BASE, "--step", "hold", "--no-seed", "--obs-density", "-2"]).ok).toBe(false);
        expect(parseArgs([...BASE, "--step", "hold", "--obs-density", "x"]).ok).toBe(false);
    });
});

describe("--dock-scenario dummy の名前ラベルの板の読み", () => {
    it("ダミーの地の上の PNG の板を measure-plate で読む (SDR。録画はしない)", () => {
        const steps = plan(["--step", "dock-view", "--dock-scenario", "dummy"]);
        const plates = steps.filter((s) => s.kind === "measure-plate");
        expect(plates).toHaveLength(1);
        expect(plates[0]).toMatchObject({ transfer: "srgb", bg: DUMMY_PLATE_BG, corner: "bl" });
        expect(plates[0].recording).toBeUndefined();
        expect(steps.filter((s) => s.kind === "record-start")).toHaveLength(0);
        const kinds = steps.map((s) => s.kind);
        expect(kinds.indexOf("screenshot-source")).toBeLessThan(kinds.indexOf("measure-plate"));
    });

    it("--obs-hdr のときは出力を録って、そのフレームを PQ として読む (PNG の色が HDR で何になるか分からないため)", () => {
        const steps = plan(["--step", "dock-view", "--dock-scenario", "dummy", "--obs-hdr"]);
        const plates = steps.filter((s) => s.kind === "measure-plate");
        expect(plates.map((s) => s.transfer)).toEqual(["srgb", "pq"]);
        expect(plates[1]).toMatchObject({ recording: DUMMY_HDR_RECORDING_LABEL, bg: DUMMY_PLATE_BG });
        const kinds = steps.map((s) => s.kind);
        const recStart = steps.findIndex((s) => s.kind === "record-start" && s.label === DUMMY_HDR_RECORDING_LABEL);
        const recStop = steps.findIndex((s) => s.kind === "record-stop" && s.label === DUMMY_HDR_RECORDING_LABEL);
        expect(recStart).toBeGreaterThan(-1);
        expect(recStart).toBeLessThan(recStop);
        expect(recStop).toBeLessThan(steps.indexOf(plates[1]));
        expect(kinds.slice(-4)).toEqual(["close-obs", "write-result", "summarize", "cleanup"]);
    });
});
