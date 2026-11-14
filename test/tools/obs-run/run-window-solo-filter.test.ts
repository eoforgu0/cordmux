/*
 * 1 人のときだけ見せるフィルタ cordmux_solo_filter を、テスト用の送信側で確かめる dock-view の solo-filter の実行計画と、撮った絵と
 * 録画のフレームの読み取り (classifySoloFrame) と判定 (judgeSoloSamples、judgeSoloRecording) と、行の [...] の名前の組み方
 * (textPrefixThroughFirstArg) をテストする。
 * run-window-plan.mjs は Node の API を呼ばないので、テストする対象には値だけを渡す。
 */
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

import {
    SOLO_FILTER_KIND,
    SOLO_FULL_PROBE_RECT,
    SOLO_MARKER_RECT,
    SOLO_FADE_MS,
    SOLO_MARKER_RGB,
    SOLO_MARKER_SCENE_NAME,
    SOLO_OTHER_SCENE_NAME,
    SOLO_RECORDING_LABEL,
    TEXT_ID_FULL_SCREEN_RESTORE,
    TEXT_ID_ROW_FULL_SCREEN,
    TEXT_ID_ROW_MORE_ACTIONS_FOR,
    classifySoloFrame,
    excludedFrameIndexes,
    judgeSoloRecording,
    judgeSoloSamples,
    parseArgs,
    planSteps,
    textPrefixThroughFirstArg,
} from "./run-window-plan.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const SCENE_NAMES_H = join(HERE, "..", "..", "..", "src-obs", "sources", "scene_names.h");
const JA_INI = join(HERE, "..", "..", "..", "src-obs", "data", "locale", "ja-JP.ini");

function soloPlan() {
    const r = parseArgs(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "dock-view", "--dock-scenario", "solo-filter"]);
    expect(r.ok).toBe(true);
    const p = planSteps(r.args);
    expect(p.ok).toBe(true);
    return p.steps as Array<Record<string, unknown>>;
}

// 幅 w、高さ h の RGB24 の絵を、地の色 bg で塗り、割合の矩形を色で塗る。
function frameOf(w: number, h: number, bg: number[], fills: Array<{ rect: { x: number; y: number; w: number; h: number }; rgb: number[] }>) {
    const buf = new Uint8Array(w * h * 3);
    for (let i = 0; i < w * h; i += 1) buf.set(bg, i * 3);
    for (const f of fills) {
        const x0 = Math.floor(f.rect.x * w);
        const y0 = Math.floor(f.rect.y * h);
        const x1 = Math.floor((f.rect.x + f.rect.w) * w);
        const y1 = Math.floor((f.rect.y + f.rect.h) * h);
        for (let y = y0; y < y1; y += 1) for (let x = x0; x < x1; x += 1) buf.set(f.rgb, (y * w + x) * 3);
    }
    return buf;
}

describe("dock-view の solo-filter の実行計画", () => {
    it("フィルタの種類の id は製品の scene_names.h の kSoloFilterTypeId と同じ", () => {
        const header = readFileSync(SCENE_NAMES_H, "utf8");
        expect(header).toContain('kSoloFilterTypeId = "' + SOLO_FILTER_KIND + '"');
    });

    it("合成のソースを置いた後に印を置き、状態を変えるたびに撮って期待と比べる (項目、シーン、ダミーの入り切りを含む)", () => {
        const steps = soloPlan();
        const kinds = steps.map((s) => s.kind);
        expect(kinds.indexOf("wire-composite")).toBeLessThan(kinds.indexOf("wire-solo-marker"));
        expect(steps.find((s) => s.kind === "wire-solo-marker")).toMatchObject({
            filterKind: SOLO_FILTER_KIND,
            rect: SOLO_MARKER_RECT,
            otherSceneName: SOLO_OTHER_SCENE_NAME,
            markerSceneName: SOLO_MARKER_SCENE_NAME,
        });
        const samples = steps.filter((s) => s.kind === "solo-sample");
        expect(samples.map((s) => [s.label, s.expect, s.sceneName])).toEqual([
            ["two", "off", "Main"],
            ["one", "on", "Main"],
            ["hidden", "off", "Main"],
            ["shown", "on", "Main"],
            ["away", "off", SOLO_OTHER_SCENE_NAME],
            ["back", "on", "Main"],
            ["away-fade", "off", SOLO_OTHER_SCENE_NAME],
            ["back-fade", "on", "Main"],
            ["dummy", "off", "Main"],
            ["dummy-off", "on", "Main"],
            ["two-again", "off", "Main"],
            ["focus", "off-then-on", "Main"],
            ["restored", "off", "Main"],
        ]);
        // ダミーも食い違いを見る (左下の矩形は灰色のときだけ全面の 1 人と読むので、色のタイルを全面と取り違えない)。全画面への
        // 切り替えの間だけは見ない (スクリーンショットのフィルタは直前の番組の描画の値を使う。同じフレームは録画で見る)。
        expect(samples.filter((s) => s.label !== "focus").every((s) => s.checkConsistency === true)).toBe(true);
        expect(samples.find((s) => s.label === "focus")).toMatchObject({ checkConsistency: false });
        expect(samples.every((s) => s.gate === true)).toBe(true);
        expect(samples.find((s) => s.label === "focus")).toMatchObject({ durationMs: 8000, intervalMs: 100 });
        // 項目の表示とシーンの切り替えは、撮る前に行う。
        expect(steps.filter((s) => s.kind === "solo-item").map((s) => s.enabled)).toEqual([false, true]);
        // カットで離れて戻り、フェードで離れて戻る。フェードだけが、録画の判定から除く区間を持つ。
        expect(steps.filter((s) => s.kind === "solo-program").map((s) => [s.sceneName, s.excludeMs ?? null])).toEqual([
            [SOLO_OTHER_SCENE_NAME, null],
            ["Main", null],
            [SOLO_OTHER_SCENE_NAME, SOLO_FADE_MS],
            ["Main", SOLO_FADE_MS],
        ]);
        expect(steps.filter((s) => s.kind === "solo-transition").map((s) => s.transitionKind)).toEqual(["fade_transition", "cut_transition"]);
    });

    it("状態を変える間ずっと出力を録画し、止めた後に録画の全部のフレームを judge-solo-recording で判定する", () => {
        const steps = soloPlan();
        const kinds = steps.map((s) => s.kind);
        const start = kinds.indexOf("record-start");
        const stop = kinds.indexOf("record-stop");
        expect(steps[start]).toMatchObject({ label: SOLO_RECORDING_LABEL, stream: false });
        expect(start).toBeLessThan(kinds.indexOf("solo-sample"));
        expect(stop).toBeGreaterThan(kinds.lastIndexOf("solo-sample"));
        expect(kinds[stop + 1]).toBe("collect-recording");
        expect(steps[stop + 2]).toMatchObject({ kind: "judge-solo-recording", recording: SOLO_RECORDING_LABEL, gate: true });
    });

    it("全画面は 3 人目の行の [...] とメニューの [全画面で表示] で始め、1 行目の [元に戻す] で戻す (直後に撮る)", () => {
        const steps = soloPlan();
        const more = steps.find((s) => s.buttonTextId === TEXT_ID_ROW_MORE_ACTIONS_FOR);
        expect(more).toMatchObject({ kind: "dialog-click", buttonTextPrefixArg: "harness-screen-3", namePrefix: true, noWait: true });
        const i = steps.indexOf(more as Record<string, unknown>);
        expect(steps[i + 1]).toMatchObject({ kind: "dialog-click", popup: true, buttonTextId: TEXT_ID_ROW_FULL_SCREEN, controlType: "MenuItem" });
        expect(steps[i + 2]).toMatchObject({ kind: "solo-sample", label: "focus" });
        expect(steps[i - 1]).toMatchObject({ kind: "solo-sample", label: "two-again" });
        expect(steps[i + 3]).toMatchObject({ kind: "dialog-click", buttonTextId: TEXT_ID_FULL_SCREEN_RESTORE });
    });

    it("名指す製品のテキストの id は ja-JP.ini にある", () => {
        const ini = readFileSync(JA_INI, "utf8");
        for (const id of [TEXT_ID_ROW_MORE_ACTIONS_FOR, TEXT_ID_ROW_FULL_SCREEN, TEXT_ID_FULL_SCREEN_RESTORE]) expect(ini).toContain(id + "=");
    });
});

describe("textPrefixThroughFirstArg (行の [...] の名前の始まり)", () => {
    it("%1 までを名前で埋め、後ろを捨てる (ユーザー名の有無で後ろの形が変わっても同じ始まりになる)", () => {
        expect(textPrefixThroughFirstArg("その他の操作 (%1)", "harness-screen-3")).toBe("その他の操作 (harness-screen-3");
        expect(textPrefixThroughFirstArg("More actions (%1)", "a")).toBe("More actions (a");
        expect(textPrefixThroughFirstArg("その他の操作", "x")).toBe("その他の操作");
    });
});

describe("classifySoloFrame (撮った 1 枚の読み取り)", () => {
    const W = 480;
    const H = 270;
    const VIDEO = [48, 48, 48];

    it("2 人を上の枠に描いていて印がない絵は、印なし・全面でない", () => {
        const frame = frameOf(W, H, [0, 0, 0], [{ rect: { x: 0, y: 0, w: 1, h: 0.5 }, rgb: VIDEO }]);
        expect(classifySoloFrame(frame, 0, W, H)).toMatchObject({ marker: false, full: false });
    });

    it("1 人を全面に描き、印が見える絵は、印あり・全面", () => {
        const frame = frameOf(W, H, VIDEO, [{ rect: SOLO_MARKER_RECT, rgb: SOLO_MARKER_RGB }]);
        expect(classifySoloFrame(frame, 0, W, H)).toMatchObject({ marker: true, full: true });
    });

    it("全面なのに印がない絵と、全面でないのに印がある絵も、そのとおりに読む (判定で食い違いになる)", () => {
        expect(classifySoloFrame(frameOf(W, H, VIDEO, []), 0, W, H)).toMatchObject({ marker: false, full: true });
        const onlyMarker = frameOf(W, H, [0, 0, 0], [{ rect: SOLO_MARKER_RECT, rgb: SOLO_MARKER_RGB }]);
        expect(classifySoloFrame(onlyMarker, 0, W, H)).toMatchObject({ marker: true, full: false });
    });

    it("ダミーの色のタイル、白の線、黒は、全面の 1 人と読まない (灰色だけを全面と読む)", () => {
        for (const rgb of [
            [122, 46, 46],
            [46, 122, 58],
            [46, 58, 122],
            [122, 106, 46],
            [255, 255, 255],
            [0, 0, 0],
        ]) {
            expect(classifySoloFrame(frameOf(W, H, rgb, []), 0, W, H).full).toBe(false);
        }
        // 録画の圧縮で少し揺れた灰色は全面と読む。
        expect(classifySoloFrame(frameOf(W, H, [52, 46, 50], []), 0, W, H).full).toBe(true);
    });

    it("つないだ絵の列の 2 枚目を offset で読む", () => {
        const a = frameOf(W, H, [0, 0, 0], []);
        const b = frameOf(W, H, VIDEO, [{ rect: SOLO_MARKER_RECT, rgb: SOLO_MARKER_RGB }]);
        const both = new Uint8Array(a.length + b.length);
        both.set(a, 0);
        both.set(b, a.length);
        expect(classifySoloFrame(both, a.length, W, H)).toMatchObject({ marker: true, full: true });
    });

    it("読む矩形は、印 (右上) と全面の判定 (左下の 4 等分の中) が重ならない", () => {
        expect(SOLO_MARKER_RECT.x).toBeGreaterThan(0.5);
        expect(SOLO_MARKER_RECT.y + SOLO_MARKER_RECT.h).toBeLessThan(0.5);
        expect(SOLO_FULL_PROBE_RECT.x + SOLO_FULL_PROBE_RECT.w).toBeLessThan(0.5);
        expect(SOLO_FULL_PROBE_RECT.y).toBeGreaterThan(0.5);
    });
});

describe("judgeSoloRecording (出力の録画の全部のフレームの判定)", () => {
    const off = { marker: false, full: false };
    const on = { marker: true, full: true };

    it("食い違うフレームがなく、印が見えるフレームと見えないフレームが両方あれば合格", () => {
        expect(judgeSoloRecording([off, on, on, off])).toMatchObject({ ok: true, count: 4, onCount: 2, offCount: 2, inconsistentCount: 0 });
    });

    it("食い違うフレームが 1 つでもあれば失敗し、番号を残す (隠した直後に印だけが残る、全面なのに印が出ない)", () => {
        const r = judgeSoloRecording([off, on, { marker: true, full: false }, off, { marker: false, full: true }, on]);
        expect(r.ok).toBe(false);
        expect(r.inconsistentCount).toBe(2);
        expect(r.inconsistent).toEqual([2, 4]);
    });

    it("除く番号のフレーム (フェードの途中) は、食い違っていても数えない", () => {
        const r = judgeSoloRecording([off, { marker: true, full: false }, { marker: false, full: true }, on], [1, 2]);
        expect(r).toMatchObject({ ok: true, count: 4, excludedCount: 2, onCount: 1, offCount: 1, inconsistentCount: 0 });
    });

    it("ON のフレームか OFF のフレームがない (状態を変えられていない) か、フレームを読めなければ失敗", () => {
        expect(judgeSoloRecording([off, off]).ok).toBe(false);
        expect(judgeSoloRecording([on, on]).ok).toBe(false);
        expect(judgeSoloRecording([]).ok).toBe(false);
    });

    it("食い違いの番号は 20 個までにする", () => {
        const many = Array.from({ length: 30 }, () => ({ marker: true, full: false }));
        const r = judgeSoloRecording([off, ...many]);
        expect(r.inconsistentCount).toBe(30);
        expect(r.inconsistent).toHaveLength(20);
    });
});

describe("excludedFrameIndexes (録画の判定から除くフレーム)", () => {
    it("区間に前後の余白を足し、フレームの時刻 (番号 / fps) がその中のフレームを除く", () => {
        // 30 fps。1000 ms から 1300 ms のフェードに 100 ms の余白 -> 900 ms から 1400 ms -> 27 から 42 番。
        const out = excludedFrameIndexes(60, 30, [{ fromMs: 1000, toMs: 1300 }], 100);
        expect(out[0]).toBe(27);
        expect(out.at(-1)).toBe(42);
        expect(out).toHaveLength(16);
    });

    it("fps が分からないときと、区間がないときは何も除かない", () => {
        expect(excludedFrameIndexes(60, 0, [{ fromMs: 0, toMs: 1000 }], 0)).toEqual([]);
        expect(excludedFrameIndexes(60, 30, [], 100)).toEqual([]);
        expect(excludedFrameIndexes(60, 30, [{ fromMs: null, toMs: null }], 100)).toEqual([]);
    });
});

describe("judgeSoloSamples (撮った絵の列の判定)", () => {
    const off = { marker: false, full: false };
    const on = { marker: true, full: true };

    it("off と on は、全部の絵がそのとおりで食い違いがなければ合格", () => {
        expect(judgeSoloSamples({ expect: "off", samples: [off, off] }).ok).toBe(true);
        expect(judgeSoloSamples({ expect: "on", samples: [on, on] }).ok).toBe(true);
        expect(judgeSoloSamples({ expect: "off", samples: [off, on] }).ok).toBe(false);
        expect(judgeSoloSamples({ expect: "on", samples: [on, off] }).ok).toBe(false);
    });

    it("印と合成のソースの状態が食い違う絵が 1 枚でもあれば失敗 (切り替え中に ON、全面なのに OFF)", () => {
        const early = judgeSoloSamples({ expect: "off-then-on", samples: [off, { marker: true, full: false }, on] });
        expect(early.ok).toBe(false);
        expect(early.inconsistent).toEqual([1]);
        const late = judgeSoloSamples({ expect: "off-then-on", samples: [off, { marker: false, full: true }, on] });
        expect(late.ok).toBe(false);
        expect(late.inconsistent).toEqual([1]);
    });

    it("off-then-on は、ON になった後に消えず最後が ON なら合格。届く前の OFF を撮れたかは preArrivalObserved に残し、合否に入れない", () => {
        const seen = judgeSoloSamples({ expect: "off-then-on", samples: [off, off, on, on] });
        expect(seen).toMatchObject({ ok: true, preArrivalObserved: true, onCount: 2 });
        const notSeen = judgeSoloSamples({ expect: "off-then-on", samples: [on, on] });
        expect(notSeen).toMatchObject({ ok: true, preArrivalObserved: false });
        expect(judgeSoloSamples({ expect: "off-then-on", samples: [off, on, off] }).ok).toBe(false);
        expect(judgeSoloSamples({ expect: "off-then-on", samples: [off, off] }).ok).toBe(false);
    });

    it("食い違いを見ない設定では、印が見えなければ合格", () => {
        const dummy = { marker: false, full: true };
        expect(judgeSoloSamples({ expect: "off", samples: [dummy, dummy], checkConsistency: false }).ok).toBe(true);
        expect(judgeSoloSamples({ expect: "off", samples: [dummy], checkConsistency: true }).ok).toBe(false);
    });

    it("撮れた絵がないときと、読めない絵があるときは失敗", () => {
        expect(judgeSoloSamples({ expect: "off", samples: [] }).ok).toBe(false);
        expect(judgeSoloSamples({ expect: "off", samples: [off, { marker: null, full: null }] })).toMatchObject({ ok: false, unreadable: 1 });
    });
});
