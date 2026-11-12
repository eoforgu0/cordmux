/*
 * 自分の枠の音声ソースのしきい値を測る短い実行 (--step hold の --self-mic)。録画しない hold で、音声入力キャプチャ
 * (既定のデバイス) を obs-websocket で作り、cordmux の settings.json の self_speaking_source_name をその名前にして始め、
 * OBS のログから peak の分布と話している割合の行を抜き出す。
 *   - 旗の受け方 (hold だけ。--no-seed と両立しない)、手順の並び (録画も配信の受け口もない)、seed-config.py への受け渡し。
 *   - ログの行の読み (pipeline.cpp の Stats 行と self_speaking_source.cpp の INFO の行) と、行の並びの読み分け。
 * run-window-plan.mjs は Node の API を呼ばないので、確認の対象には値だけを渡す。実行スクリプト (run-window.mjs) と seed-config.py は
 * ファイルのテキストで確認する (OBS を起動しないと動かないため。ほかのテストと同じ形)。
 */
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

import {
    SELF_MIC_DEVICE_ID,
    SELF_MIC_FOUND_WAIT_MS,
    SELF_MIC_INPUT_KIND,
    SELF_MIC_INPUT_NAME,
    SELF_MIC_MONITOR_TYPE,
    WIRE_SCENE_NAME,
    classifySelfRow,
    judgeSelfSpeakingLog,
    parseArgs,
    planSteps,
    selfSourceEventsFromLog,
    selfSpeakingStatsFromLog,
} from "./run-window-plan.mjs";

type Step = Record<string, any>;

const HERE = dirname(fileURLToPath(import.meta.url));
const BASE = ["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin"];
const HOLD = [...BASE, "--step", "hold", "--hold-minutes", "8"];
const text = (name: string) => readFileSync(join(HERE, name), "utf8");

function plan(argv: string[]) {
    const parsed = parseArgs(argv);
    expect(parsed.errors).toEqual([]);
    const r = planSteps(parsed.args);
    expect(r.ok).toBe(true);
    return r.steps as Step[];
}
const kinds = (steps: Step[]) => steps.map((s) => s.kind);

// OBS のログの行の形 ("HH:MM:SS.mmm: [cordmux] ..."。pipeline.cpp の Stats 行の書式と同じ鍵)。
const statsLine = (time: string, p50: number, p95: number, max: number, talking: number, threshold = -26, packets = 1000) =>
    `${time}: [cordmux] Stats self_speaking_source packets=${packets} self_peak_p50_db=${p50} self_peak_p95_db=${p95} self_peak_max_db=${max} self_talking_pct=${talking} self_threshold_db=${threshold}`;

describe("--self-mic の旗", () => {
    it("hold だけが受け、既定はなし", () => {
        expect(parseArgs(HOLD).args.selfMic).toBe(false);
        const given = parseArgs([...HOLD, "--self-mic"]);
        expect(given.errors).toEqual([]);
        expect(given.args.selfMic).toBe(true);
        for (const step of ["call-hold", "dock-view", "record"]) {
            expect(parseArgs([...BASE, "--step", step, "--self-mic"]).errors.join(" ")).toContain("--self-mic は --step hold だけ");
        }
    });

    it("--no-seed とは両立しない (自分の枠の音声ソースの名前を書くのは初期設定)", () => {
        const r = parseArgs([...HOLD, "--self-mic", "--no-seed"]);
        expect(r.ok).toBe(false);
        expect(r.errors.join(" ")).toContain("--self-mic は --no-seed と両立しない");
    });
});

describe("hold --self-mic の実行計画", () => {
    const steps = plan([...HOLD, "--self-mic"]);

    it("初期設定が自分の枠の音声ソースの名前を渡し、起動と接続の後に入力を作る", () => {
        const seed = steps.find((s) => s.kind === "seed-config")!;
        expect(seed.selfSpeakingSourceName).toBe(SELF_MIC_INPUT_NAME);
        expect(plan(HOLD).find((s) => s.kind === "seed-config")!.selfSpeakingSourceName).toBeUndefined();
        const connect = steps.findIndex((s) => s.kind === "obsws-connect");
        const create = steps.findIndex((s) => s.kind === "obsws-request" && s.request === "CreateInput");
        expect(create).toBeGreaterThan(connect);
        expect(steps[create].requestData).toEqual({
            sceneName: WIRE_SCENE_NAME,
            inputName: SELF_MIC_INPUT_NAME,
            inputKind: "wasapi_input_capture",
            inputSettings: { device_id: "default" },
            sceneItemEnabled: true,
        });
        expect(SELF_MIC_INPUT_KIND).toBe("wasapi_input_capture");
        expect(SELF_MIC_DEVICE_ID).toBe("default");
    });

    it("入力の監視は「監視なし」にして読み直し、製品が見つけるまで待ってから待ちに入る", () => {
        const create = steps.findIndex((s) => s.kind === "obsws-request" && s.request === "CreateInput");
        expect(steps[create + 1]).toMatchObject({ request: "SetInputAudioMonitorType", requestData: { inputName: SELF_MIC_INPUT_NAME, monitorType: "OBS_MONITORING_TYPE_NONE" } });
        expect(SELF_MIC_MONITOR_TYPE).toBe("OBS_MONITORING_TYPE_NONE");
        expect(steps[create + 2]).toMatchObject({ request: "GetInputAudioMonitorType", requestData: { inputName: SELF_MIC_INPUT_NAME } });
        expect(steps[create + 3]).toMatchObject({ kind: "wait", ms: SELF_MIC_FOUND_WAIT_MS });
        expect(create + 3).toBeLessThan(steps.findIndex((s) => s.kind === "hold-info"));
    });

    it("録画も配信の受け口もない。OBS を閉じた後にログを読み、結果を書く前に済ませる", () => {
        const k = kinds(steps);
        for (const bad of ["record-start", "record-stop", "stream-sink-start", "stream-stop", "harness-start", "wire-composite"]) expect(k).not.toContain(bad);
        const close = k.indexOf("close-obs");
        const collect = k.indexOf("collect-self-speaking-log");
        expect(collect).toBeGreaterThan(close);
        expect(collect).toBeLessThan(k.indexOf("write-result"));
        // 待ちは通知ファイルまで。人の待ちは hold の分数。
        expect(steps.find((s) => s.kind === "human")).toMatchObject({ until: "go-file", waitSeconds: 8 * 60 });
    });

    it("旗がない hold は今までと同じ (入力も読みもない)", () => {
        const k = kinds(plan(HOLD));
        expect(k).not.toContain("obsws-request");
        expect(k).not.toContain("collect-self-speaking-log");
    });

    it("入力の名前は OBS の既定のマイク入力の名前と重ならない文字列で、アカウントの名前を含まない", () => {
        expect(SELF_MIC_INPUT_NAME).toBe("マイク");
        expect(SELF_MIC_INPUT_NAME).not.toContain("/");
    });
});

describe("実行スクリプトと seed-config.py への受け渡し (ファイルのテキストで確認する)", () => {
    const runWindow = text("run-window.mjs");
    const seed = text("seed-config.py");

    it("seed-config.py の引数に名前を渡し、hold-info に入力の名前を書き、新しい手順の種類を処理する", () => {
        expect(runWindow).toContain('argv.push("--self-speaking-source-name", s.selfSpeakingSourceName)');
        const holdInfo = runWindow.slice(runWindow.indexOf('case "hold-info": {'), runWindow.indexOf('case "hash-bundle": {'));
        expect(holdInfo).toContain("selfMic:");
        expect(holdInfo).toContain("inputName: SELF_MIC_INPUT_NAME");
        const handled = runWindow.slice(runWindow.indexOf("const HANDLED_KINDS = ["), runWindow.indexOf("];", runWindow.indexOf("const HANDLED_KINDS = [")));
        expect(handled).toContain('"collect-self-speaking-log"');
        expect(runWindow).toContain('case "collect-self-speaking-log": {');
    });

    it("音が出ない: seed-config.py はプロファイルの監視の出力を disabled で書き、しきい値は書かない", () => {
        expect(seed.match(/MonitoringDeviceId=disabled/g)!.length).toBeGreaterThanOrEqual(1);
        const block = seed.slice(seed.indexOf('if args.self_speaking_source_name is not None:\n            settings["self_speaking_source_name"]'));
        expect(block.slice(0, 200)).not.toContain("threshold");
        expect(seed).not.toMatch(/settings\["self_speaking_threshold_db"\]/);
        expect(seed).not.toMatch(/settings\["self_speaking_source_uuid"\]/);
    });
});

describe("ログの行の読み", () => {
    const log = [
        "20:00:00.000: [cordmux] Self slot audio source not found",
        "20:00:02.500: [cordmux] Self slot audio source found (match=name)",
        // 順番と知らないキーに頼らない。
        "20:00:12.000: [cordmux] Stats self_speaking_source self_threshold_db=-26 packets=995 self_peak_max_db=-8 extra_key=1 self_peak_p95_db=-14 self_peak_p50_db=-31 self_talking_pct=64",
        "20:00:22.000: [cordmux] Stats video received_hz=60.0 decoded_hz=60.0",
        "20:00:22.100: [cordmux] Self slot video source found (match=name)",
    ];

    it("Stats 行の鍵=値を拾い、別の Stats 行と映像の入力の行は読まない", () => {
        const rows = selfSpeakingStatsFromLog(log);
        expect(rows).toEqual([{ t: 20 * 3600 + 12, packets: 995, p50Db: -31, p95Db: -14, maxDb: -8, talkingPct: 64, thresholdDb: -26 }]);
        const events = selfSourceEventsFromLog(log);
        expect(events).toEqual([
            { t: 20 * 3600, event: "not-found", match: null },
            { t: 20 * 3600 + 2.5, event: "found", match: "name" },
        ]);
    });

    it("日付をまたいだら 1 日足す", () => {
        const rows = selfSpeakingStatsFromLog([statsLine("23:59:55.000", -30, -12, -6, 90), statsLine("00:00:05.000", -60, -50, -48, 0)]);
        expect(rows[1].t! - rows[0].t!).toBeCloseTo(10, 3);
    });

    it("classifySelfRow: 話している / 大きいのに話していない (ミュート) / 静か / 読めない", () => {
        const row = (talkingPct: number | null, p95Db: number | null, thresholdDb: number | null = -26) => ({ talkingPct, p95Db, thresholdDb });
        expect(classifySelfRow(row(80, -12) as any)).toBe("talking");
        expect(classifySelfRow(row(0, -12) as any)).toBe("loud-silent");
        expect(classifySelfRow(row(0, -55) as any)).toBe("quiet");
        expect(classifySelfRow(row(null, -55) as any)).toBe("unknown");
        expect(classifySelfRow(null as any)).toBe("unknown");
    });
});

describe("judgeSelfSpeakingLog: 話す 30 秒、黙る 30 秒、ミュートして話す 30 秒の並び", () => {
    const rows = selfSpeakingStatsFromLog([
        statsLine("20:00:10.000", -30, -12, -6, 85),
        statsLine("20:00:20.000", -29, -11, -5, 90),
        statsLine("20:00:30.000", -31, -13, -7, 80),
        statsLine("20:00:40.000", -62, -55, -50, 0),
        statsLine("20:00:50.000", -63, -56, -51, 0),
        statsLine("20:01:00.000", -62, -54, -49, 0),
        statsLine("20:01:10.000", -30, -12, -6, 0),
        statsLine("20:01:20.000", -29, -11, -5, 0),
        statsLine("20:01:30.000", -31, -13, -7, 0),
    ]);
    const found = [{ t: 20 * 3600, event: "found", match: "name" }];

    it("連続する同じ読みをまとめた並びと、数と、最大の peak を返す", () => {
        const r = judgeSelfSpeakingLog({ rows, events: found });
        expect(r.ok).toBe(true);
        expect(r.sequence).toEqual([
            { kind: "talking", rows: 3 },
            { kind: "quiet", rows: 3 },
            { kind: "loud-silent", rows: 3 },
        ]);
        expect(r.counts).toEqual({ talking: 3, "loud-silent": 3, quiet: 3, unknown: 0 });
        expect(r.rowCount).toBe(9);
        expect(r.maxDb).toBe(-5);
        expect(r.thresholdDb).toBe(-26);
        expect(r.foundMatch).toBe("name");
    });

    it("入力を見つけた行がない、または Stats の行がないときは失敗で、理由を書く", () => {
        const noFound = judgeSelfSpeakingLog({ rows, events: [{ t: 0, event: "not-found", match: null }] });
        expect(noFound.ok).toBe(false);
        expect(noFound.reason).toContain("never reported finding");
        const noRows = judgeSelfSpeakingLog({ rows: [], events: found });
        expect(noRows.ok).toBe(false);
        expect(noRows.reason).toContain("no self_speaking_source stats row");
        expect(judgeSelfSpeakingLog({ rows: undefined, events: undefined }).ok).toBe(false);
    });
});
