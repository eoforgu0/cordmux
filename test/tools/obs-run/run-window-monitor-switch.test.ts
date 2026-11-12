/*
 * 遅れを測るウィンドウキャプチャが、最大化された Vesktop のウィンドウを捉えられないことがある (WGC の初期化が失敗してソースの
 * サイズが 0 のままになる)。スクリプトは、捉えられていないときに限り、Vesktop のウィンドウが画面の全体を覆っていれば
 * 画面キャプチャへ切り替える。
 * 切り替えてよいかの判定 (windowFillsMonitor)、一覧から画面を選ぶ処理 (pickMonitorItem)、手順の並びと実行スクリプトへの
 * 受け渡しを vitest で確認する。実際の画面の名前は持たない (機種名は偽のもの)。
 */
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

import {
    LATENCY_INPUT_NAME,
    LATENCY_MONITOR_CAPTURE_KIND,
    LATENCY_MONITOR_CAPTURE_SETTINGS,
    LATENCY_MONITOR_TRIAL_NAME,
    MONITOR_FILL_OVERHANG_PX,
    latencyReferenceRect,
    latencyReferenceTransform,
    latencyRegions,
    parseArgs,
    parseMonitorItemName,
    pickMonitorItem,
    planSteps,
    windowFillsMonitor,
} from "./run-window-plan.mjs";

type Step = Record<string, any>;

const HERE = dirname(fileURLToPath(import.meta.url));
const REAL = [
    "--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-1", "--real-profile", "--call-window", "--decoder", "software",
    "--n-screen", "3", "--n-voice", "3", "--broadcaster-id", "1", "--start-with-add", "--vesktop-share", "--skip-continuous", "--skip-speaker-mute", "--extra-checks", "--skip-export-bundle", "--latency-person", "person-x", "--voice-channel", "lobby",
];
const REHEARSAL = ["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-1", "--rehearsal", "--n-screen", "3", "--n-voice", "3", "--broadcaster-id", "1", "--start-with-add", "--vesktop-share", "--skip-continuous", "--skip-speaker-mute", "--extra-checks", "--skip-export-bundle", "--latency-person", "broadcaster"];

function stepsOf(argv: string[]): Step[] {
    const parsed = parseArgs(argv);
    expect(parsed.errors).toEqual([]);
    const r = planSteps(parsed.args);
    expect(r.ok).toBe(true);
    return r.steps as Step[];
}

// 画面 1920,0 の 1920x1080 で最大化されたウィンドウ (縁が各辺 8 ピクセル外へはみ出す)。
const MONITOR = [1920, 0, 1920, 1080];
const MAXIMIZED = { found: true, count: 1, rect: [1912, -8, 1936, 1096], zoomed: true, iconic: false, monitor: MONITOR };

describe("windowFillsMonitor: 最大化されて画面の全体を覆うときだけ切り替える", () => {
    it("最大化で画面の全体を覆う (縁のはみ出しは各辺 8 ピクセル) なら真で、画面の四角を返す", () => {
        expect(windowFillsMonitor(MAXIMIZED)).toEqual({ fills: true, reason: null, monitor: { x: 1920, y: 0, w: 1920, h: 1080 } });
    });

    it("はみ出しなしでちょうど覆うウィンドウも真", () => {
        expect(windowFillsMonitor({ ...MAXIMIZED, rect: MONITOR }).fills).toBe(true);
    });

    it("縁のはみ出しは MONITOR_FILL_OVERHANG_PX まで許し、それを超えると偽 (複数の画面にまたがるウィンドウなど)", () => {
        const o = MONITOR_FILL_OVERHANG_PX;
        expect(windowFillsMonitor({ ...MAXIMIZED, rect: [1920 - o, -o, 1920 + 2 * o, 1080 + 2 * o] }).fills).toBe(true);
        expect(windowFillsMonitor({ ...MAXIMIZED, rect: [1920 - o - 1, -o, 1920 + 2 * o + 1, 1080 + 2 * o] })).toMatchObject({ fills: false, reason: "the window extends past the screen" });
        expect(windowFillsMonitor({ ...MAXIMIZED, rect: [0, 0, 3840, 1080] }).fills).toBe(false);
    });

    it("覆っていないウィンドウは偽 (最大化でもタスクバーの分が空いている・1 ピクセル足りない)", () => {
        expect(windowFillsMonitor({ ...MAXIMIZED, rect: [1912, -8, 1936, 1056] })).toMatchObject({ fills: false, reason: "the maximized window does not cover the whole screen" });
        expect(windowFillsMonitor({ ...MAXIMIZED, rect: [1921, 0, 1919, 1080] }).fills).toBe(false);
        expect(windowFillsMonitor({ ...MAXIMIZED, rect: [1920, 1, 1920, 1079] }).fills).toBe(false);
    });

    it("最大化されていないウィンドウは、画面と同じ大きさでも偽 (最大化の旗が基準)", () => {
        expect(windowFillsMonitor({ ...MAXIMIZED, zoomed: false })).toMatchObject({ fills: false, reason: "the Vesktop window is not maximized" });
        expect(windowFillsMonitor({ ...MAXIMIZED, rect: [1500, 200, 958, 539], zoomed: false }).fills).toBe(false);
    });

    it("最小化・ウィンドウがない・読めない四角は偽 (分からないときは既定の動き = 切り替えない)", () => {
        expect(windowFillsMonitor({ ...MAXIMIZED, iconic: true }).fills).toBe(false);
        expect(windowFillsMonitor({ found: false, count: 0, rect: null, zoomed: false, iconic: false, monitor: null }).fills).toBe(false);
        expect(windowFillsMonitor({ ...MAXIMIZED, monitor: null }).fills).toBe(false);
        expect(windowFillsMonitor({ ...MAXIMIZED, rect: [0, 0, 0, 0] }).fills).toBe(false);
        expect(windowFillsMonitor({ ...MAXIMIZED, rect: [1912, -8, "x", 1096] }).fills).toBe(false);
        expect(windowFillsMonitor(null).fills).toBe(false);
        expect(windowFillsMonitor(undefined).fills).toBe(false);
    });
});

describe("parseMonitorItemName / pickMonitorItem: 一覧の名前の位置で画面を選ぶ", () => {
    const items = [
        { itemName: "Model A: 1920x1080 @ 0,0", itemValue: "\\\\?\\DISPLAY#A#1", itemEnabled: true },
        { itemName: "Model B: 1920x1080 @ 1920,0", itemValue: "\\\\?\\DISPLAY#B#2", itemEnabled: true },
        { itemName: "Model C: 1280x1024 @ -1280,0", itemValue: "\\\\?\\DISPLAY#C#3", itemEnabled: true },
    ];

    it("名前の最後の 大きさ @ 位置 を読む (負の位置も。形が違えば null)", () => {
        expect(parseMonitorItemName("Model B: 1920x1080 @ 1920,0")).toEqual({ w: 1920, h: 1080, x: 1920, y: 0 });
        expect(parseMonitorItemName("Model C: 1280x1024 @ -1280,-10")).toEqual({ w: 1280, h: 1024, x: -1280, y: -10 });
        expect(parseMonitorItemName("Display 2")).toBeNull();
        expect(parseMonitorItemName(undefined)).toBeNull();
    });

    it("位置と大きさが同じ画面を選び、値は一覧のまま (バックスラッシュを変えない)", () => {
        const picked = pickMonitorItem(items, { x: 1920, y: 0, w: 1920, h: 1080 });
        expect(picked.reason).toBeNull();
        expect(picked.item).toBe(items[1]);
        expect(picked.item!.itemValue).toBe("\\\\?\\DISPLAY#B#2");
        expect(pickMonitorItem(items, { x: -1280, y: 0, w: 1280, h: 1024 }).item).toBe(items[2]);
    });

    it("画面が見つからない・大きさが違う・一覧が空・四角がないときは選ばない (理由つき)", () => {
        expect(pickMonitorItem(items, { x: 3840, y: 0, w: 1920, h: 1080 })).toMatchObject({ item: null });
        expect(pickMonitorItem(items, { x: 1920, y: 0, w: 1280, h: 720 }).item).toBeNull();
        expect(pickMonitorItem([], { x: 0, y: 0, w: 1920, h: 1080 }).reason).toContain("no screen in the capture list");
        expect(pickMonitorItem(undefined, { x: 0, y: 0, w: 1920, h: 1080 }).item).toBeNull();
        expect(pickMonitorItem(items, null).item).toBeNull();
    });

    it("同じ位置と大きさが 2 つ以上あるとき、値が空のときは選ばない (別の画面をキャプチャしない)", () => {
        const dup = [...items, { itemName: "Model D: 1920x1080 @ 1920,0", itemValue: "\\\\?\\DISPLAY#D#4", itemEnabled: true }];
        expect(pickMonitorItem(dup, { x: 1920, y: 0, w: 1920, h: 1080 })).toMatchObject({ item: null });
        expect(pickMonitorItem(dup, { x: 1920, y: 0, w: 1920, h: 1080 }).reason).toContain("more than one");
        expect(pickMonitorItem([{ itemName: "Model B: 1920x1080 @ 1920,0", itemValue: "", itemEnabled: true }], { x: 1920, y: 0, w: 1920, h: 1080 }).item).toBeNull();
    });
});

describe("手順の並び: 切り替えは捉えたかの確認の後・録画の前", () => {
    it("check-window-capture の直後に switch-to-monitor-capture があり、その後に録画が始まる (実際の回もリハーサルも)", () => {
        for (const steps of [stepsOf(REAL), stepsOf(REHEARSAL)]) {
            const kinds = steps.map((s) => s.kind);
            const check = kinds.indexOf("check-window-capture");
            const sw = kinds.indexOf("switch-to-monitor-capture");
            expect(check).toBeGreaterThan(-1);
            expect(sw).toBe(check + 1);
            expect(kinds[sw + 1]).toBe("record-start");
            expect(kinds.filter((k) => k === "switch-to-monitor-capture")).toHaveLength(1);
            // 外す手順は録画の後に 1 つだけ (どちらの方式でも同じ名前を外す)。
            const remove = steps.find((s) => s.kind === "remove-window-capture")!;
            expect(remove.inputName).toBe(LATENCY_INPUT_NAME);
            expect(kinds.indexOf("remove-window-capture")).toBeGreaterThan(kinds.indexOf("record-stop"));
        }
    });

    it("切り替えの手順は、元と同じ名前と区画、画面キャプチャの種類と設定 (指は入れない) を持つ", () => {
        const steps = stepsOf(REAL);
        const sw = steps.find((s) => s.kind === "switch-to-monitor-capture")!;
        const place = steps.find((s) => s.kind === "place-window-capture")!;
        expect(sw).toMatchObject({
            inputName: LATENCY_INPUT_NAME,
            trialName: LATENCY_MONITOR_TRIAL_NAME,
            inputKind: LATENCY_MONITOR_CAPTURE_KIND,
            inputSettings: LATENCY_MONITOR_CAPTURE_SETTINGS,
            sceneName: place.sceneName,
        });
        expect(LATENCY_MONITOR_CAPTURE_KIND).toBe("monitor_capture");
        expect(LATENCY_MONITOR_CAPTURE_SETTINGS).toMatchObject({ method: 1, capture_cursor: false });
        expect(LATENCY_MONITOR_TRIAL_NAME).not.toBe(LATENCY_INPUT_NAME);
        // 画面の値 (バックスラッシュ入り) は手順の引数に持たない (一覧から選ぶ)。
        expect(JSON.stringify(sw)).not.toContain("monitor_id");
    });

    it("実行スクリプト: 手順の種類が登録され、捉えていれば何もせず、切り替えの手順は同じ変形で置き、外すのは名前を付け替えた後", () => {
        const source = readFileSync(join(HERE, "run-window.mjs"), "utf8");
        expect(source).toContain('"switch-to-monitor-capture"');
        // check-window-capture は捉えたかを覚え、切り替えは hooked が偽のときだけ進む。
        expect(source).toContain("ref.hooked = checked.hooked;");
        expect(source).toContain("if (ref.hooked !== false) {");
        expect(source).toContain("windowFillsMonitor(info)");
        expect(source).toContain("pickMonitorItem(listed && listed.propertyItems, verdict.monitor)");
        expect(source).toContain("latencyReferenceTransform(ref.rect)");
        const removeAt = source.indexOf('await ws.call("RemoveInput", { inputName: ref.inputName });');
        const renameAt = source.indexOf('await ws.call("SetInputName", { inputName: s.trialName, newInputName: s.inputName });');
        expect(removeAt).toBeGreaterThan(-1);
        expect(renameAt).toBeGreaterThan(removeAt);
        // 切り替えた後も確認 (大きさと黒でないか) をやり直し、方式をログに書く。
        expect(source).toContain('method: "monitor"');
        expect(source).toContain("const checked = await checkLatencyCapture(s, state.latencyReference);");
    });
});

describe("領域 B はウィンドウでも画面でも同じ象限 (judge-latency と latencyRegions は変えずに使える)", () => {
    it("置く四角 (latencyReferenceRect) と変形は方式によらない。latencyRegions の領域 B はその録画の座標", () => {
        const rect = latencyReferenceRect({ cell: 3, videoSettings: { baseWidth: 1920, baseHeight: 1080, outputWidth: 1920, outputHeight: 1080 } });
        expect(rect.output).toEqual({ x: 960, y: 540, w: 960, h: 540 });
        expect(latencyReferenceTransform(rect)).toMatchObject({ positionX: 960, positionY: 540, boundsType: "OBS_BOUNDS_SCALE_INNER", boundsWidth: 960, boundsHeight: 540 });
        const regions = latencyRegions({ captures: [], cells: [], person: "person-x", reference: rect.output });
        expect(regions.regionB).toEqual(rect.output);
    });
});
