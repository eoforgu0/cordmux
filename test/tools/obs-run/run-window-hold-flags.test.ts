/*
 * 実機の実行で、hold と call-hold に足した旗を確認する。
 *   - --keep-recordings (hold だけ): 後始末 (手順の cleanup と実行スクリプトの finally の cleanup) が cordmux の録画のフォルダーを残す。
 *     録画の途中でテスト用の OBS を落として起動し直す実行があるので、起動し直す前に区間と録画ログを消さない。
 *   - --stream-sink (hold と call-hold): hold-info の前に配信の受け口 (127.0.0.1 だけ) を起動し、待ちの後に配信を止め、
 *     OBS を閉じた後に受け口を閉じる。call-hold は最後のアンインストールの前に配信を止める (配信中はアンインストールできない)。
 *   - --replay-buffer (hold だけ): 初期設定が seed-config.py の --replay-buffer を渡す。--no-seed とは両立しない。
 * run-window-plan.mjs は Node の API を呼ばないので、確認の対象には値だけを渡す。実行スクリプト (run-window.mjs) と seed-config.py は
 * ファイルのテキストで確認する (OBS を起動しないと動かないため。ほかのテストと同じ形)。
 */
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

import {
    STEPS_KEEPING_RECORDINGS,
    STREAM_SINK_MAX_REARMS,
    STREAM_SINK_PORT,
    STREAM_SINK_STEPS,
    STREAM_SINK_URL,
    keepsRecordings,
    parseArgs,
    planSteps,
} from "./run-window-plan.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const BASE = ["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin"];
const HOLD = [...BASE, "--step", "hold"];
const CALL_HOLD = [...BASE, "--step", "call-hold", "--real-profile", "--call-window", "--decoder", "software"];
const CALL_HOLD_REHEARSAL = [...BASE, "--step", "call-hold", "--rehearsal"];

function plan(argv: string[]) {
    const parsed = parseArgs(argv);
    expect(parsed.errors).toEqual([]);
    const r = planSteps(parsed.args);
    expect(r.ok).toBe(true);
    return { args: parsed.args, steps: r.steps as Array<Record<string, any>> };
}

const runWindowText = () => readFileSync(join(HERE, "run-window.mjs"), "utf8");

describe("2 行目: hold の --keep-recordings", () => {
    it("hold だけが受け、--no-seed と両立する", () => {
        expect(parseArgs([...HOLD, "--keep-recordings"]).errors).toEqual([]);
        expect(parseArgs([...HOLD, "--keep-recordings", "--no-seed"]).errors).toEqual([]);
        expect(parseArgs([...CALL_HOLD, "--keep-recordings"]).errors.join(" ")).toContain("--keep-recordings は --step hold だけ");
        expect(parseArgs([...BASE, "--step", "record", "--keep-recordings"]).ok).toBe(false);
    });

    it("keepsRecordings: 一覧の step は常に残し、hold は旗があるときだけ残す。ほかの step は旗を見ない", () => {
        for (const step of STEPS_KEEPING_RECORDINGS) expect(keepsRecordings({ step })).toBe(true);
        expect(keepsRecordings({ step: "hold" })).toBe(false);
        expect(keepsRecordings({ step: "hold", keepRecordings: true })).toBe(true);
        expect(keepsRecordings({ step: "dock-view", keepRecordings: true })).toBe(false);
        expect(keepsRecordings(null)).toBe(false);
        expect(STEPS_KEEPING_RECORDINGS).not.toContain("hold");
    });

    it("手順の cleanup は旗があるときだけ -KeepRecordings (旗なしは前のまま消す)", () => {
        expect(plan([...HOLD, "--keep-recordings"]).steps.at(-1)).toMatchObject({ kind: "cleanup", keepRecordings: true });
        expect(plan([...HOLD, "--keep-recordings", "--no-seed"]).steps.at(-1)).toMatchObject({ kind: "cleanup", keepRecordings: true });
        expect(plan(HOLD).steps.at(-1)).toMatchObject({ kind: "cleanup", keepRecordings: false });
    });

    it("実行スクリプトの finally の cleanup と hold-info と dry-run の出力は同じ keepsRecordings を読む", () => {
        const text = runWindowText();
        const finallyBlock = text.slice(text.lastIndexOf("} finally {"));
        expect(finallyBlock).toContain("keepRecordings: keepsRecordings(args)");
        expect(text).not.toContain("STEPS_KEEPING_RECORDINGS.includes(");
        const holdInfo = text.slice(text.indexOf('case "hold-info": {'), text.indexOf('case "hash-bundle": {'));
        expect(holdInfo).toContain("keepRecordings: keepsRecordings(args)");
        const dry = text.slice(text.indexOf("if (args.dryRun) {"), text.indexOf("if (guard.blocked) {"));
        expect(dry).toContain("keepRecordings: keepsRecordings(args)");
        expect(dry).toContain("{ keepRecordings: keepsRecordings(args) }");
    });
});

describe("3 行目: hold と call-hold の --stream-sink", () => {
    it("受け口は 127.0.0.1 だけ。seed-config.py の test の形の配信の行き先 (server と鍵 test) と同じ", () => {
        expect(STREAM_SINK_URL).toBe("rtmp://127.0.0.1:" + STREAM_SINK_PORT + "/live/test");
        expect(STREAM_SINK_STEPS).toEqual(["hold", "call-hold"]);
        expect(STREAM_SINK_MAX_REARMS).toBeGreaterThan(0);
        const seed = readFileSync(join(HERE, "seed-config.py"), "utf8");
        expect(seed).toContain('"server": "rtmp://127.0.0.1:' + STREAM_SINK_PORT + '/live"');
        expect(seed).toContain('service_json_text("test")');
    });

    it("hold と call-hold (実際の回とリハーサル) だけが受ける", () => {
        expect(parseArgs([...HOLD, "--stream-sink"]).errors).toEqual([]);
        expect(parseArgs([...CALL_HOLD, "--stream-sink"]).errors).toEqual([]);
        expect(parseArgs([...CALL_HOLD_REHEARSAL, "--stream-sink"]).errors).toEqual([]);
        expect(parseArgs([...BASE, "--step", "call-1", "--real-profile", "--stream-sink"]).errors.join(" ")).toContain("--stream-sink は --step hold / --step call-hold だけ");
        expect(parseArgs([...BASE, "--step", "dock-view", "--stream-sink"]).ok).toBe(false);
    });

    it("旗が無ければ受け口の手順はない (前のまま)", () => {
        for (const argv of [HOLD, CALL_HOLD, CALL_HOLD_REHEARSAL]) {
            const kinds = plan(argv).steps.map((s) => s.kind);
            expect(kinds).not.toContain("stream-sink-start");
            expect(kinds).not.toContain("stream-sink-stop");
            expect(kinds).not.toContain("stream-stop");
        }
    });

    it("hold: 受け口 -> hold-info -> 待つ -> 配信を止める (していれば) -> OBS を閉じる -> 受け口を閉じる -> 後始末", () => {
        const { steps } = plan([...HOLD, "--stream-sink", "--n-screen", "1"]);
        const kinds = steps.map((s) => s.kind);
        const at = (k: string) => kinds.indexOf(k);
        const wait = steps.findIndex((s, i) => s.kind === "human" && i > at("hold-info"));
        expect(at("wait-connection")).toBeLessThan(at("stream-sink-start"));
        expect(at("stream-sink-start")).toBe(at("hold-info") - 1);
        expect(wait).toBe(at("hold-info") + 1);
        expect(at("stream-stop")).toBeGreaterThan(wait);
        expect(at("stream-stop")).toBeLessThan(at("close-obs"));
        expect(at("stream-sink-stop")).toBeGreaterThan(at("close-obs"));
        expect(at("stream-sink-stop")).toBeLessThan(at("write-result"));
        expect(steps[at("stream-sink-start")]).toMatchObject({ url: STREAM_SINK_URL, rearm: true });
        expect(steps[at("stream-stop")]).toMatchObject({ ifActive: true });
        expect(steps[at("stream-stop")].gate).toBeUndefined();
        expect(kinds.filter((k) => k === "stream-sink-start")).toHaveLength(1);
    });

    it("call-hold: hold-info の前に起こし、締めのアンインストールの前に配信を止め、OBS を閉じた後に受け口を閉じる", () => {
        for (const argv of [CALL_HOLD, CALL_HOLD_REHEARSAL]) {
            const { steps } = plan([...argv, "--stream-sink"]);
            const kinds = steps.map((s) => s.kind);
            const at = (k: string) => kinds.indexOf(k);
            expect(at("stream-sink-start")).toBe(at("hold-info") - 1);
            expect(at("stream-stop")).toBeGreaterThan(at("hold-info"));
            expect(at("stream-stop")).toBeLessThan(at("capture-backup"));
            expect(at("stream-sink-stop")).toBeGreaterThan(at("close-obs"));
            expect(at("stream-sink-stop")).toBeLessThan(at("keep-obs-recordings"));
            if (argv === CALL_HOLD) {
                const uninstall = steps.findIndex((s) => s.kind === "human" && s.until === "state-json-restored");
                expect(at("stream-stop")).toBeLessThan(uninstall);
            }
        }
    });

    it("実行スクリプト: 受け口は stream-sink.mjs で、同梱の ffmpeg を先に使い、行き先は STREAM_SINK_URL だけ。finally で止め、hold-info に書く", () => {
        const text = runWindowText();
        // 親がいない受け口の確認のまとめ (summarizeStaleCheck) も同じ所から読む。
        expect(text).toContain('import { createStreamSink, summarizeStaleCheck } from "./stream-sink.mjs";');
        const start = text.slice(text.indexOf('case "stream-sink-start": {'), text.indexOf('case "record-start": {'));
        expect(start).toContain("exe: fs.existsSync(BUNDLED_FFMPEG) ? BUNDLED_FFMPEG : \"ffmpeg\"");
        expect(start).toContain("url: STREAM_SINK_URL");
        expect(start).toContain("rearm: !!s.rearm");
        expect(start).toContain("Get-NetTCPConnection -State Listen -LocalPort");
        // rtmp の行き先の文字列は実行計画の定数の 1 か所だけ (実行スクリプトに直接書かない = 127.0.0.1 の外へ向ける余地を作らない)。
        expect(text).not.toContain("rtmp://");
        const finallyBlock = text.slice(text.lastIndexOf("} finally {"));
        expect(finallyBlock).toContain("stopStreamSink();");
        const holdInfo = text.slice(text.indexOf('case "hold-info": {'), text.indexOf('case "hash-bundle": {'));
        expect(holdInfo).toContain("streamSink: state.streamSinkInfo");
        const stop = text.slice(text.indexOf('case "stream-stop": {'), text.indexOf('case "count-operations": {'));
        expect(stop).toContain('ws.call("GetStreamStatus")');
        expect(stop).toContain("if (s.ifActive) {");
    });
});

describe("4 行目: hold の --replay-buffer", () => {
    it("hold だけが受け、--no-seed とは両立しない", () => {
        expect(parseArgs([...HOLD, "--replay-buffer"]).errors).toEqual([]);
        expect(parseArgs([...HOLD, "--replay-buffer", "--no-seed"]).errors.join(" ")).toContain("--replay-buffer は --no-seed と両立しない");
        expect(parseArgs([...CALL_HOLD, "--replay-buffer"]).errors.join(" ")).toContain("--replay-buffer は --step hold だけ");
    });

    it("初期設定の手順が replayBuffer を持ち、旗が無ければ持たない", () => {
        expect(plan([...HOLD, "--replay-buffer"]).steps[0]).toMatchObject({ kind: "seed-config", replayBuffer: true });
        expect(plan(HOLD).steps[0].replayBuffer).toBeUndefined();
    });

    it("実行スクリプトは seed-config.py に --replay-buffer を渡し、dry-run の seedPreview に出す。seed-config.py はその旗を持つ", () => {
        const text = runWindowText();
        const argv = text.slice(text.indexOf("function seedConfigArgv("), text.indexOf("function readLocaleTexts("));
        expect(argv).toContain('if (s.replayBuffer) argv.push("--replay-buffer");');
        expect(text).toContain("replayBuffer: p.replayBuffer ?? null");
        const seed = readFileSync(join(HERE, "seed-config.py"), "utf8");
        expect(seed).toContain('parser.add_argument("--replay-buffer"');
        expect(seed).toContain('"replayBuffer": bool(args.replay_buffer)');
    });
});
