/*
 * 合成のソース cordmux_composite を、テスト用の送信側のリハーサル (call-1 の --rehearsal) でテストする。
 *   - 実行計画: 合成のソースを置くステップ、配置の行を読むステップ、飛ばすステップ (理由つき)、OBS を閉じた後の判定。
 *   - OBS のログの配置の行 (Composite layout canvas=... count=... tile=x,y,w,h ...) の読み取りと、判定ツールのセルへの変換。
 *   - 配置の判定: 自分の枠は左上、画面共有は表示名の順、2 つ以上なら 4 等分の決まった位置、外すと詰まり戻すと元に戻る。
 *   - 表示の遅延の測定 (video_latency の shown_n) と、OBS の全体の音の遅延 (adding N milliseconds of audio buffering) の読み取り。
 * run-window-plan.mjs は Node の API を呼ばないので、テストする対象には値だけを渡す。
 */
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

import {
    AC30_ITEM_LABELS,
    CORDMUX_SOURCE_KINDS,
    REHEARSAL_NOT_JUDGED,
    audioBufferingFromLog,
    cellRegionsForJudge,
    compositeCellRegions,
    harnessPersonsFromSlotStreams,
    latencyRegions,
    parseScreenSlotLine,
    screenSlotStreamsFromLines,
    findCordmuxInput,
    judgeAudioBuffering,
    judgeCellArgs,
    judgeCompositeCount,
    judgeCompositeLayout,
    fillTextArgs,
    judgeCompositePlacement,
    judgeDisplayDelay,
    judgeLinesSummary,
    lastCompositeLayout,
    naturalCompare,
    parseArgs,
    parseCompositeLayoutLine,
    planSteps,
    sourceRefLabel,
    stepTextIds,
    videoLatencyShownFromLog,
} from "./run-window-plan.mjs";

type Step = Record<string, any>;

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = join(HERE, "..", "..", "..");
const REHEARSAL = ["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-1", "--rehearsal"];
// リハーサルの呼び出し行 (実行の内容に関わらないオプションを除いたもの)。
const CALL_LINE = ["--n-screen", "3", "--n-voice", "3", "--broadcaster-id", "1", "--start-with-add", "--vesktop-share", "--skip-continuous", "--skip-speaker-mute", "--extra-checks", "--skip-export-bundle", "--latency-person", "x", "--display-buffer-ms", "100"];

function plan(argv: string[]) {
    const parsed = parseArgs(argv);
    expect(parsed.errors).toEqual([]);
    const r = planSteps(parsed.args);
    expect(r.ok).toBe(true);
    return { args: parsed.args, steps: r.steps as Step[] };
}
const kinds = (steps: Step[]) => steps.map((s) => s.kind);

const LINE3 = "Composite layout canvas=1920x1080 count=3 self=0,0,960,540 slot2=960,0,960,540 slot1=0,540,960,540";
const prefixed = (line: string) => "12:34:56.789: [cordmux] " + line;

// 画面共有 3 人 (枠 1-3) を表示名の順に 4 等分へ置いた読み。persons は枠の番号 -> 人の名前。
function snapshot(slotsInOrder: number[], persons: Record<number, string>, opts: { self?: boolean } = {}) {
    const cells = [
        [0, 0],
        [960, 0],
        [0, 540],
        [960, 540],
    ];
    const tiles: any[] = [];
    let i = 0;
    if (opts.self) {
        tiles.push({ name: "self", self: true, slot: 0, x: cells[i][0], y: cells[i][1], w: 960, h: 540 });
        i += 1;
    }
    for (const slot of slotsInOrder) {
        tiles.push({ name: "slot" + slot, self: false, slot, x: cells[i][0], y: cells[i][1], w: 960, h: 540 });
        i += 1;
    }
    return { layout: { canvas: { width: 1920, height: 1080 }, count: tiles.length, tiles, line: "" }, persons };
}
const PEOPLE = { 1: "harness-screen-1", 2: "harness-screen-2", 3: "harness-screen-10" };

describe("合成のソースが既定の場合の引数", () => {
    it("--composite はなくなり、どの場合でも受け付けない", () => {
        expect(parseArgs([...REHEARSAL, "--composite"]).errors.length).toBeGreaterThan(0);
        expect(parseArgs(REHEARSAL).args.composite).toBeUndefined();
    });

    it("--cordmux-scenes もなくなる (製品はシーンを作らない)", () => {
        expect(parseArgs([...REHEARSAL, "--cordmux-scenes", "none"]).errors.length).toBeGreaterThan(0);
    });

    it("--display-buffer-ms は受ける (渡さなければ製品の既定)", () => {
        expect(parseArgs([...REHEARSAL, "--display-buffer-ms", "100"]).args.displayBufferMs).toBe(100);
        expect(parseArgs([...REHEARSAL]).args.displayBufferMs).toBeUndefined();
    });
});

describe("合成のソースが既定の場合の実行計画", () => {
    const steps = plan([...REHEARSAL, ...CALL_LINE]).steps;

    it("合成のソースを録画するシーンに 1 つ置き、枠のソースの配線 (wire-scene) はない", () => {
        expect(steps.filter((s) => s.kind === "wire-composite")).toHaveLength(1);
        expect(kinds(steps)).not.toContain("wire-scene");
        expect(steps.find((s) => s.kind === "wire-composite")).toMatchObject({ inputName: "cordmux" });
        // 録画の前に置く (録画のセルが最初の読み取りから取れる)。
        expect(kinds(steps).indexOf("wire-composite")).toBeLessThan(kinds(steps).indexOf("record-start"));
    });

    it("配置の行を読むステップは 4 つ (枠に載った後と 4 分割の付け外しの前後 3 つ)、判定は 1 つで付け外しと同じ 3 つの label を読む", () => {
        const snaps = steps.filter((s) => s.kind === "composite-layout-snapshot");
        expect(snaps.map((s) => s.label)).toEqual(["slot-items", AC30_ITEM_LABELS.before, AC30_ITEM_LABELS.afterRemove, AC30_ITEM_LABELS.afterRestore]);
        const judge = steps.filter((s) => s.kind === "judge-composite-layout");
        expect(judge).toHaveLength(1);
        expect(judge[0]).toMatchObject({ person: "harness-screen-1", labels: AC30_ITEM_LABELS });
        const last = Math.max(...snaps.map((s) => steps.indexOf(s)));
        expect(steps.indexOf(judge[0])).toBeGreaterThan(last);
    });

    it("枠ごとの公開のソースを前提にしたステップ (枠の画像・音の切り分け・音量の SetInput*・付け外しの項目一覧と判定) は出ない", () => {
        const k = kinds(steps);
        for (const gone of ["isolate-audio", "restore-audio", "judge-ac30", "set-input-volume", "set-input-mute", "judge-auto-layout"]) expect(k).not.toContain(gone);
        // 入力の名前で指定する画像は、枠ごとの公開のソースの画像ではない。
        const shots = steps.filter((s) => s.kind === "screenshot-source" && !s.source.inputName);
        expect(shots.length).toBeGreaterThan(0);
        for (const sh of shots) expect(sh.source).toEqual({ kind: "composite" });
        expect(steps.some((s) => s.kind === "obsws-request" && s.label === "ac30-before")).toBe(false);
    });

    it("飛ばすステップは、リハーサルで判定しない項目 (REHEARSAL_NOT_JUDGED) だけ (名前ラベルの設定・声のミュート・使う声・A/V の切り分けは合成のソースの形で実行する)", () => {
        const skipped = steps.filter((s) => s.kind === "skip");
        expect(skipped.map((s) => s.item)).toEqual(["version-row", "broadcaster-left", "broadcaster-left", "latency"]);
        for (const s of skipped) expect(Object.keys(REHEARSAL_NOT_JUDGED)).toContain(s.item);
        const k = kinds(steps);
        for (const made of ["judge-ac31", "judge-ac32", "judge-voice-source"]) expect(k).toContain(made);
    });

    it("OBS を閉じた後に、表示の遅延の測定と音の遅延の判定が付く", () => {
        const k = kinds(steps);
        expect(k).toContain("judge-display-delay");
        expect(k).toContain("judge-audio-buffering");
        expect(k.indexOf("judge-display-delay")).toBeGreaterThan(k.indexOf("close-obs"));
        expect(k.indexOf("judge-audio-buffering")).toBeGreaterThan(k.indexOf("close-obs"));
    });

    it("run-window.mjs のステップの switch が全部の種類を持つ (run-window.mjs をテキストとして読む)", () => {
        const source = readFileSync(join(HERE, "run-window.mjs"), "utf8");
        const caseKinds = new Set([...source.matchAll(/^\s+case "([a-z0-9-]+)":/gm)].map((m) => m[1]));
        const handledBlock = source.slice(source.indexOf("const HANDLED_KINDS = ["), source.indexOf("];", source.indexOf("const HANDLED_KINDS = [")));
        const handled = new Set([...handledBlock.matchAll(/"([a-z0-9-]+)"/g)].map((m) => m[1]));
        for (const k of new Set(kinds(steps))) {
            expect(caseKinds.has(k), k).toBe(true);
            expect(handled.has(k), k).toBe(true);
        }
    });

    it("指定する画面のテキストは製品の ja-JP.ini にある", () => {
        const keys = new Set<string>();
        for (const line of readFileSync(join(REPO, "src-obs", "data", "locale", "ja-JP.ini"), "utf8").split(/\r?\n/)) {
            const t = line.trim();
            if (t && !t.startsWith("#") && t.indexOf("=") > 0) keys.add(t.slice(0, t.indexOf("=")).trim());
        }
        for (const s of steps) for (const id of stepTextIds(s)) if (!id.startsWith("obs:")) expect(keys.has(id), id).toBe(true);
    });

    it("合成のソースは枠の番号を持たない指定 (種類の id だけで探せる)", () => {
        const list = [
            { inputName: "cordmux", kind: "composite", slot: null },
            { inputName: "s1", kind: "screen", slot: 1 },
        ];
        expect(findCordmuxInput(list, { kind: "composite" })).toMatchObject({ inputName: "cordmux" });
        expect(sourceRefLabel({ kind: "composite" })).toBe(CORDMUX_SOURCE_KINDS.composite);
        expect(CORDMUX_SOURCE_KINDS.composite).toBe("cordmux_composite");
    });
});

describe("配置の行の読み取り", () => {
    it("製品のテストと同じ形の行を読む (OBS の行の先頭が付いていても読める)", () => {
        for (const text of [LINE3, prefixed(LINE3)]) {
            const parsed = parseCompositeLayoutLine(text)!;
            expect(parsed.canvas).toEqual({ width: 1920, height: 1080 });
            expect(parsed.count).toBe(3);
            expect(parsed.tiles.map((t: any) => t.name)).toEqual(["self", "slot2", "slot1"]);
            expect(parsed.tiles[0]).toMatchObject({ self: true, slot: 0, x: 0, y: 0, w: 960, h: 540 });
            expect(parsed.tiles[1]).toMatchObject({ self: false, slot: 2, x: 960, y: 0 });
        }
    });

    it("count 0 の行と全面の行を読み、不正な行と置けなかった行は読まない", () => {
        expect(parseCompositeLayoutLine("Composite layout canvas=1280x720 count=0")).toMatchObject({ count: 0, tiles: [] });
        expect(parseCompositeLayoutLine("Composite layout canvas=1366x768 count=1 slot3=0,0,1366,768")!.tiles[0]).toMatchObject({ slot: 3, w: 1366, h: 768 });
        expect(parseCompositeLayoutLine("Composite layout canvas=1920x1080 count=2 self=0,0,960,540")).toBeNull();
        expect(parseCompositeLayoutLine("Composite layout has no room for 1 screen share(s)")).toBeNull();
        expect(parseCompositeLayoutLine("[cordmux] Stats stream=1 video_latency recv_n=3")).toBeNull();
        expect(parseCompositeLayoutLine(undefined as any)).toBeNull();
    });

    it("ログの行の並びから最後の配置の行を取る (無ければ null)", () => {
        const lines = [prefixed("Composite layout canvas=1920x1080 count=1 slot1=0,0,1920,1080"), "other", prefixed(LINE3), "Composite layout has no room for 1 screen share(s)", "tail"];
        expect(lastCompositeLayout(lines)!.count).toBe(3);
        expect(lastCompositeLayout(["a", "b"])).toBeNull();
        expect(lastCompositeLayout([])).toBeNull();
    });
});

describe("表示名の自然順 (製品の natural_less と同じ規則)", () => {
    it("数字の並びは数の大小で比べる", () => {
        expect(naturalCompare("harness-screen-2", "harness-screen-10")).toBeLessThan(0);
        expect(naturalCompare("harness-screen-10", "harness-screen-2")).toBeGreaterThan(0);
        expect(naturalCompare("a2", "a10")).toBeLessThan(0);
    });

    it("英字の大文字と小文字は同じとして比べ、全部同じなら元の文字の順", () => {
        expect(naturalCompare("Alice", "alice2")).toBeLessThan(0);
        expect(naturalCompare("B", "a")).toBeGreaterThan(0);
        expect(naturalCompare("x", "x")).toBe(0);
        expect(naturalCompare("A", "a")).toBeLessThan(0);
        expect(naturalCompare("a01", "a1")).not.toBe(0);
    });

    it("日本語は文字の番号の順 (ひらがなが漢字より前)", () => {
        expect(naturalCompare("あ", "漢")).toBeLessThan(0);
    });
});

describe("判定ツールのセル (合成のソースの配置の行から)", () => {
    const layout = parseCompositeLayoutLine(LINE3)!;

    it("枠の番号 - 1 のセルに矩形を入れ、自分の枠はセルに入れない", () => {
        const r = compositeCellRegions({ layout, videoSettings: { baseWidth: 1920, baseHeight: 1080, outputWidth: 1920, outputHeight: 1080 } });
        expect(r.ok).toBe(true);
        expect(r.regionSource).toBe("composite-layout");
        expect(r.cells.map((c: any) => c.cell)).toEqual([0, 1]);
        expect(r.cells[0]).toMatchObject({ x: 0, y: 540, w: 960, h: 540, sourceName: null });
        expect(r.cells[1]).toMatchObject({ x: 960, y: 0, w: 960, h: 540, sourceName: null });
        expect(r.self).toEqual({ x: 0, y: 0, w: 960, h: 540 });
        // ビットのセルの一辺: 1920 幅の共有を 960 幅に置くと 16 の半分。
        expect(r.cells[0].bitCell).toBe(8);
        // 枠ごとの公開のソースはないので、名前はどの枠でも null。
        const loose = compositeCellRegions({ layout: parseCompositeLayoutLine("Composite layout canvas=1920x1080 count=1 slot3=0,0,1920,1080")!, videoSettings: {} });
        expect(loose.cells[0]).toMatchObject({ cell: 2, sourceName: null, bitCell: 16 });
    });

    it("録画のサイズが基本の解像度と違うときは比で修正する", () => {
        const r = compositeCellRegions({ layout, videoSettings: { baseWidth: 1920, baseHeight: 1080, outputWidth: 1280, outputHeight: 720 } });
        expect(r.cells[1]).toMatchObject({ x: 640, y: 0, w: 640, h: 360 });
        expect(r.canvas).toEqual({ width: 1280, height: 720 });
        expect(r.cells[1].bitCell).toBe(5);
    });

    it("配置の行が無ければ ok:false で理由を書く (何も出さずに仮定の配置へ戻さない)", () => {
        const r = compositeCellRegions({ layout: null, videoSettings: {} });
        expect(r.ok).toBe(false);
        expect(r.error).toContain("no Composite layout line");
    });

    it("判定ツールに渡すセルは取得元を composite-layout と書き、smooth.py は全部のセルを渡す", () => {
        const captures = [
            { when: "start", ...compositeCellRegions({ layout, videoSettings: {} }) },
            { when: "end", ...compositeCellRegions({ layout, videoSettings: {} }) },
        ];
        const regions = cellRegionsForJudge({ captures, cells: 2 });
        expect(regions.map((r: any) => r.regionSource)).toEqual(["composite-layout", "composite-layout"]);
        const smooth = judgeCellArgs("smooth.py", regions)[0];
        expect(smooth).toContain("--regions");
        expect(smooth[smooth.indexOf("--regions") + 1]).toBe("0,540,960,540;960,0,960,540");
        const frames = judgeCellArgs("frames.py", regions);
        expect(frames[0]).toEqual(["--x", "0", "--y", "540", "--w", "960", "--h", "540", "--cell", "8"]);
    });

    it("セルが取れなかった枠は仮定の象限にフォールバックし、そのことを書く", () => {
        const captures = [{ when: "start", ...compositeCellRegions({ layout: null, videoSettings: {} }) }];
        const regions = cellRegionsForJudge({ captures, cells: 2 });
        expect(regions.map((r: any) => r.regionSource)).toEqual(["assumed-quadrant", "assumed-quadrant"]);
    });

    it("セルに取得元がないときの取得元は composite-layout", () => {
        const captures = [{ when: "start", ok: true, canvas: { width: 1920, height: 1080 }, cells: [{ cell: 0, x: 0, y: 0, w: 960, h: 540, bitCell: 8 }] }];
        expect(cellRegionsForJudge({ captures, cells: 1 })[0].regionSource).toBe("composite-layout");
    });
});

describe("配置の判定", () => {
    it("置き方: 3 人は表示名の順 (自然順) で左上から 4 等分の位置に詰まる", () => {
        const ok = judgeCompositePlacement(snapshot([1, 2, 3], PEOPLE));
        expect(ok.ok).toBe(true);
        expect(ok.names).toEqual(["harness-screen-1", "harness-screen-2", "harness-screen-10"]);
        expect(ok.mode).toBe("quarters");
        expect(ok.hasSelf).toBe(false);
    });

    it("自分の枠があれば先頭 (左上) で、画面共有は右上から詰まる", () => {
        const withSelf = judgeCompositePlacement(snapshot([1, 2], PEOPLE, { self: true }));
        expect(withSelf.ok).toBe(true);
        expect(withSelf.hasSelf).toBe(true);
        const moved = snapshot([1, 2], PEOPLE, { self: true });
        moved.layout.tiles.reverse();
        expect(judgeCompositePlacement(moved).ok).toBe(false);
        expect(judgeCompositePlacement(moved).problems.join(" ")).toContain("self tile is not the first");
    });

    it("表示名の順でない並びと、人の名前が引けないタイルは失敗", () => {
        const wrong = judgeCompositePlacement(snapshot([3, 1, 2], PEOPLE));
        expect(wrong.ok).toBe(false);
        expect(wrong.reason).toContain("not in display name order");
        const unknown = judgeCompositePlacement(snapshot([1, 2, 3], { 1: "a", 2: "b" }));
        expect(unknown.ok).toBe(false);
        expect(unknown.reason).toContain("slot3 is unknown");
    });

    it("2 つ以上で位置がセルの外、または 1 つで全面でも左上でもないなら失敗。1 つの全面は通る", () => {
        const off = snapshot([1, 2], PEOPLE);
        off.layout.tiles[1].x = 0;
        off.layout.tiles[1].y = 540;
        expect(judgeCompositePlacement(off).ok).toBe(false);
        const full = { layout: parseCompositeLayoutLine("Composite layout canvas=1920x1080 count=1 slot1=0,0,1920,1080")!, persons: PEOPLE };
        expect(judgeCompositePlacement(full)).toMatchObject({ ok: true, mode: "single-full" });
        const odd = { layout: parseCompositeLayoutLine("Composite layout canvas=1920x1080 count=1 slot1=960,540,960,540")!, persons: PEOPLE };
        expect(judgeCompositePlacement(odd).ok).toBe(false);
        expect(judgeCompositePlacement({ layout: null, persons: {} }).ok).toBe(false);
    });

    it("すき間の分だけ縮んだタイルは通り、縮みすぎは失敗", () => {
        const gap = snapshot([1, 2, 3], PEOPLE);
        gap.layout.tiles[1] = { ...gap.layout.tiles[1], x: 964, w: 956 };
        expect(judgeCompositePlacement(gap).ok).toBe(true);
        gap.layout.tiles[1] = { ...gap.layout.tiles[1], x: 1200, w: 720 };
        expect(judgeCompositePlacement(gap).ok).toBe(false);
    });
});

describe("人の出し入れの判定 (4 分割の付け外しの前後 3 つの読み取り)", () => {
    const before = snapshot([1, 2, 3], PEOPLE);
    const afterRemove = snapshot([2, 3], PEOPLE);
    const afterRestore = snapshot([1, 2, 3], PEOPLE);

    it("外すと後ろが詰まり、戻すと元の並びと位置に戻れば通る", () => {
        const r = judgeCompositeLayout({ before, afterRemove, afterRestore, person: "harness-screen-1" });
        expect(r.ok).toBe(true);
        expect(r.names.afterRemove).toEqual(["harness-screen-2", "harness-screen-10"]);
        expect(r.counts).toEqual({ before: 3, afterRemove: 2, afterRestore: 3 });
    });

    it("外した後に人が残っている / 詰まっていない / 戻した後に並びが違う / 読み取りがない は失敗", () => {
        const stay = judgeCompositeLayout({ before, afterRemove: before, afterRestore, person: "harness-screen-1" });
        expect(stay.ok).toBe(false);
        expect(stay.reason).toContain("still in a tile after removing");
        const gapped = snapshot([2, 3], PEOPLE);
        gapped.layout.tiles[0].x = 960;
        gapped.layout.tiles[1].x = 0;
        gapped.layout.tiles[1].y = 540;
        expect(judgeCompositeLayout({ before, afterRemove: gapped, afterRestore, person: "harness-screen-1" }).ok).toBe(false);
        const swapped = snapshot([2, 1, 3], PEOPLE);
        const wrongOrder = judgeCompositeLayout({ before, afterRemove, afterRestore: swapped, person: "harness-screen-1" });
        expect(wrongOrder.ok).toBe(false);
        const unread = judgeCompositeLayout({ before, afterRemove: { layout: null, persons: {} }, afterRestore, person: "harness-screen-1" });
        expect(unread.ok).toBe(false);
        expect(unread.reason).toContain("afterRemove");
        const none = judgeCompositeLayout({ before, afterRemove, afterRestore, person: "" });
        expect(none.ok).toBe(false);
    });

    it("外す人が前の読み取りにいなければ失敗", () => {
        const r = judgeCompositeLayout({ before, afterRemove, afterRestore, person: "nobody" });
        expect(r.ok).toBe(false);
        expect(r.reason).toContain("was not in a tile before removing");
    });
});

describe("表示の遅延の測定 (video_latency の shown_n)", () => {
    const line = (stream: number, handed: number, shown: number, avg = 12.3, p95 = 30.1) =>
        "10:00:00.000: [cordmux] Stats stream=" + stream + " video_latency recv_avg_ms=5.0 recv_p95_ms=9.0 recv_n=60 decoded_avg_ms=4.0 decoded_p95_ms=8.0 decoded_n=60 handed_avg_ms=1.0 handed_p95_ms=2.0 handed_n=" +
        handed + " shown_avg_ms=" + avg + " shown_p95_ms=" + p95 + " shown_n=" + shown;

    it("ストリームごとに足し、shown_n が 0 でない行があれば通る", () => {
        const streams = videoLatencyShownFromLog([line(1, 60, 58), line(1, 60, 0), line(2, 60, 60, 10, 20), "unrelated"]);
        expect(streams).toHaveLength(2);
        expect(streams[0]).toMatchObject({ stream: 1, lines: 2, handedN: 120, shownN: 58, shownLines: 1, shownAvgMs: 12.3, shownP95MaxMs: 30.1 });
        const j = judgeDisplayDelay(streams);
        expect(j.ok).toBe(true);
        expect(j.totalShown).toBe(118);
        expect(j.withoutShown).toEqual([]);
    });

    it("shown_n が全部 0 なら失敗で理由を書き、フレームを渡したのに測れなかったストリームを残す。行がなくても失敗", () => {
        const j = judgeDisplayDelay(videoLatencyShownFromLog([line(1, 60, 0), line(2, 60, 0)]));
        expect(j.ok).toBe(false);
        expect(j.reason).toContain("shown_n was 0");
        expect(j.withoutShown).toEqual([1, 2]);
        expect(judgeDisplayDelay([]).reason).toContain("no video_latency line");
        const partial = judgeDisplayDelay(videoLatencyShownFromLog([line(1, 60, 5), line(2, 60, 0)]));
        expect(partial.ok).toBe(true);
        expect(partial.withoutShown).toEqual([2]);
    });
});

describe("OBS の全体の音の遅延 (adding N milliseconds of audio buffering)", () => {
    const lines = [
        "14:21:36.615: adding 128 milliseconds of audio buffering, total audio buffering is now 128 milliseconds (source: harness-voice-4 (cordmux声 1))",
        "14:21:40.000: something else",
        "14:23:27.229: adding 42 milliseconds of audio buffering, total audio buffering is now 170 milliseconds (source: harness-screen-2 (cordmux画面共有 2))",
        "18:02:46.846: adding 149 milliseconds of audio buffering, total audio buffering is now 149 milliseconds (source: harness-voice-5 (cordmux声 2))",
    ];

    it("時刻・足した量・合計・元のソースの名前 (括弧を含む名前も) を読む", () => {
        const entries = audioBufferingFromLog(lines);
        expect(entries).toEqual([
            { time: "14:21:36.615", addedMs: 128, totalMs: 128, source: "harness-voice-4 (cordmux声 1)" },
            { time: "14:23:27.229", addedMs: 42, totalMs: 170, source: "harness-screen-2 (cordmux画面共有 2)" },
            { time: "18:02:46.846", addedMs: 149, totalMs: 149, source: "harness-voice-5 (cordmux声 2)" },
        ]);
    });

    it("時刻の先頭がない行も読み、判定はしきい値を決めていないので ok は null (数と取得元だけ)", () => {
        const bare = audioBufferingFromLog(["adding 10 milliseconds of audio buffering, total audio buffering is now 10 milliseconds (source: s)"]);
        expect(bare).toEqual([{ time: null, addedMs: 10, totalMs: 10, source: "s" }]);
        const j = judgeAudioBuffering(audioBufferingFromLog(lines));
        expect(j.ok).toBeNull();
        expect(j.count).toBe(3);
        expect(j.finalTotalMs).toBe(149);
        expect(j.bySource["harness-voice-4 (cordmux声 1)"]).toEqual({ count: 1, addedMs: 128 });
    });

    it("行が無ければ数は 0 (増えなかった)", () => {
        const j = judgeAudioBuffering(audioBufferingFromLog(["nothing"]));
        expect(j).toMatchObject({ ok: null, count: 0, finalTotalMs: 0, entries: [] });
    });
});

describe("判定の表 (judge-lines)", () => {
    it("合成の判定の行が結果を持ち、結果のない行は数えない", () => {
        const results = [
            { name: "judge-composite-layout", ok: true },
            { name: "judge-ac31", ok: true },
            { name: "judge-display-delay", ok: true, totalShown: 10, withoutShown: [] },
            { name: "judge-audio-buffering", ok: null, count: 2, finalTotalMs: 40, bySource: {} },
        ];
        const t = judgeLinesSummary(results) as any;
        const row = (item: string) => t.rows.find((r: any) => r.item === item);
        expect(row("ac30").ok).toBe(true);
        expect(row("ac31").ok).toBe(true);
        expect(row("display-delay").ok).toBe(true);
        expect(row("display-delay").detail.totalShown).toBe(10);
        expect(row("audio-buffering").ok).toBeNull();
        expect(row("audio-buffering").detail.count).toBe(2);
        expect(row("ac32").ok).toBeNull();
        expect(t.red).toEqual([]);
        const red = judgeLinesSummary([{ name: "judge-composite-layout", ok: false, reason: "x" }]) as any;
        expect(red.red).toEqual(["ac30"]);
    });
});

describe("枠の番号と人の対応 (ログの Screen slot の行から)", () => {
    const lines = [
        "10:00:00.000: [cordmux] Screen slot 1 carries stream 1",
        "10:00:00.100: [cordmux] Screen slot 2 carries stream 2",
        "10:00:00.200: [cordmux] Screen slot 3 carries stream 3",
        "10:00:09.000: [cordmux] Screen slot 1 carries no stream",
        "10:00:09.500: [cordmux] Screen slot 1 carries stream 1",
        "10:00:10.000: [cordmux] Screen slot 2 carries no stream",
    ];

    it("1 行を読む (載った・空いた。関係ない行と不正な行は読まない)", () => {
        expect(parseScreenSlotLine(lines[0])).toEqual({ slot: 1, stream: 1 });
        expect(parseScreenSlotLine(lines[3])).toEqual({ slot: 1, stream: null });
        expect(parseScreenSlotLine("Screen slot 2 carries stream")).toBeNull();
        expect(parseScreenSlotLine("Screen slot x carries stream 1")).toBeNull();
        expect(parseScreenSlotLine(undefined as any)).toBeNull();
    });

    it("枠ごとに最後の行が反映される。時点までの行だけを渡せばその時点の対応になる", () => {
        expect(screenSlotStreamsFromLines(lines)).toEqual({ 1: 1, 2: null, 3: 3 });
        expect(screenSlotStreamsFromLines(lines.slice(0, 4))).toEqual({ 1: null, 2: 2, 3: 3 });
        expect(screenSlotStreamsFromLines(["x"])).toEqual({});
    });

    it("ストリーム M の人は harness-screen-M。空いた枠と行のない枠は入らない (人が不明になる)", () => {
        expect(harnessPersonsFromSlotStreams(screenSlotStreamsFromLines(lines))).toEqual({ 1: "harness-screen-1", 3: "harness-screen-3" });
        expect(harnessPersonsFromSlotStreams({})).toEqual({});
    });

    it("人が引けない枠のタイルがあれば配置の判定は失敗 (通さない)", () => {
        const persons = harnessPersonsFromSlotStreams(screenSlotStreamsFromLines(lines));
        const r = judgeCompositePlacement(snapshot([1, 2, 3], persons as any));
        expect(r.ok).toBe(false);
        expect(r.reason).toContain("slot2 is unknown");
    });

    it("人の名前が 1 つも引けない場合 (実際の通話) は名前の判定をせず、数と位置だけを見る", () => {
        const r = judgeCompositePlacement(snapshot([1, 2, 3], {}));
        expect(r.ok).toBe(true);
        expect(r.namesUnknown).toBe(true);
    });

    it("セルに人を持たせ、遅延を読む人の枠を人の名前で見つける (枠の公開のソースなし)", () => {
        const layout = parseCompositeLayoutLine(LINE3)!;
        const persons = { 1: "harness-screen-1", 2: "harness-screen-2" };
        const cap = { when: "start", ...compositeCellRegions({ layout, videoSettings: {}, persons }) };
        expect(cap.cells.map((c: any) => c.person)).toEqual(["harness-screen-1", "harness-screen-2"]);
        const r = latencyRegions({ captures: [cap], cells: 2, person: "harness-screen-2", personForm: "%1 (x %2)", reference: { x: 0, y: 0, w: 1, h: 1 } });
        expect(r.personFound).toBe(true);
        expect(r.cell).toBe(1);
    });
});

describe("画面共有 count 個の 4 等分の判定 (select-four)", () => {
    it("画面共有が count 個で、4 等分の決まった位置なら通る。数が違えば理由つきで失敗。自分の枠は数えない", () => {
        const four = snapshot([1, 2, 3, 4], { 1: "a", 2: "b", 3: "c", 4: "d" });
        expect(judgeCompositeCount({ ...four, count: 4 })).toMatchObject({ ok: true, shares: 4 });
        const three = judgeCompositeCount({ ...snapshot([1, 2, 3], { 1: "a", 2: "b", 3: "c" }, { self: true }), count: 4 });
        expect(three.ok).toBe(false);
        expect(three.reason).toContain("expected 4");
        expect(judgeCompositeCount({ layout: null, persons: {}, count: 4 }).ok).toBe(false);
    });
});

describe("人の名前がログにない場合 (実際の通話) の人の出し入れの判定", () => {
    it("数と位置が合っても判定できない (null)。タイルの数が減らなければ失敗", () => {
        const before = snapshot([1, 2, 3], {});
        const removed = snapshot([2, 3], {});
        const r = judgeCompositeLayout({ before, afterRemove: removed, afterRestore: before, person: "sub3" });
        expect(r.ok).toBeNull();
        expect(r.namesUnknown).toBe(true);
        const stuck = judgeCompositeLayout({ before, afterRemove: before, afterRestore: before, person: "sub3" });
        expect(stuck.ok).toBe(false);
        expect(stuck.reason).toContain("did not drop");
    });
});

describe("ドックの行のボタンの名前 (ini のテキストの %1)", () => {
    it("fillTextArgs は %1 %2 を順に置き換え、置いた文字列の中の % は置き換えない", () => {
        expect(fillTextArgs("声をミュート (%1)", ["alice"])).toBe("声をミュート (alice)");
        expect(fillTextArgs("%1 と %2", ["%2", "x"])).toBe("%2 と x");
        expect(fillTextArgs("そのまま", undefined)).toBe("そのまま");
    });
});

describe("名前ラベルの背景 (label_show_plate) の初期設定", () => {
    it("call-1 は seed-config に labelPlate を渡し、名前ラベルの確認を持たないほかのステップは渡さない", () => {
        const seed = (argv: string[]) => plan(argv).steps.find((s) => s.kind === "seed-config")!;
        expect(seed([...REHEARSAL, ...CALL_LINE]).labelPlate).toBe(true);
        expect(seed(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "dock-view", "--dock-scenario", "dummy"]).labelPlate).toBeUndefined();
        expect(seed(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "through"]).labelPlate).toBeUndefined();
    });
});

describe("実際の通話の人の出し入れの判定 (ストリームの番号の並びで見る)", () => {
    // 実際の通話で読んだ配置の行とストリームの番号 (枠 3 = ストリーム 4、枠 1 = 1、枠 2 = 2)。人の名前は読み取りにない。
    const L3 = "Composite layout canvas=1920x1080 count=3 slot3=0,0,960,540 slot1=960,0,960,540 slot2=0,540,960,540";
    const L2 = "Composite layout canvas=1920x1080 count=2 slot3=0,0,960,540 slot1=960,0,960,540";
    const read = (line: string, slotStreams: Record<number, number | null>) => ({ layout: parseCompositeLayoutLine(line)!, persons: {}, slotStreams });
    const before = read(L3, { 1: 1, 2: 2, 3: 4 });
    const afterRemove = read(L2, { 1: 1, 2: null, 3: 4 });
    const afterRestore = read(L3, { 1: 1, 2: 2, 3: 4 });

    it("外れたのが 1 ストリームで、残りが順を保って詰まり、戻すと前と同じなら成功 (表示名の順は判定しないと書く)", () => {
        const r = judgeCompositeLayout({ before, afterRemove, afterRestore, person: "alice" });
        expect(r.ok).toBe(true);
        expect(r.nameOrderJudged).toBe(false);
        expect(r.streams).toEqual({ before: [4, 1, 2], afterRemove: [4, 1], afterRestore: [4, 1, 2] });
        expect(r.note).toContain("display name order was not judged");
    });

    it("外れたのが 0 ストリームまたは 2 ストリーム、順が入れ替わった、戻した後の並びが違う、は失敗", () => {
        const stuck = judgeCompositeLayout({ before, afterRemove: before, afterRestore, person: "alice" });
        expect(stuck.ok).toBe(false);
        const L1 = "Composite layout canvas=1920x1080 count=1 slot3=0,0,1920,1080";
        const two = judgeCompositeLayout({ before, afterRemove: read(L1, { 1: null, 2: null, 3: 4 }), afterRestore, person: "alice" });
        expect(two.ok).toBe(false);
        const swapped = read("Composite layout canvas=1920x1080 count=2 slot1=0,0,960,540 slot3=960,0,960,540", { 1: 1, 2: null, 3: 4 });
        const reordered = judgeCompositeLayout({ before, afterRemove: swapped, afterRestore, person: "alice" });
        expect(reordered.ok).toBe(false);
        expect(reordered.reason).toContain("did not keep their order");
        const moved = read("Composite layout canvas=1920x1080 count=3 slot1=0,0,960,540 slot3=960,0,960,540 slot2=0,540,960,540", { 1: 1, 2: 2, 3: 4 });
        expect(judgeCompositeLayout({ before, afterRemove, afterRestore: moved, person: "alice" }).ok).toBe(false);
        // 枠の矩形は前と同じでも、戻した枠に別のストリームが載っていれば失敗。
        const otherBack = judgeCompositeLayout({ before, afterRemove, afterRestore: read(L3, { 1: 1, 2: 5, 3: 4 }), person: "alice" });
        expect(otherBack.ok).toBe(false);
        expect(otherBack.reason).toContain("streams after restoring differ");
    });

    it("ストリームの番号が読み取りにない枠があれば合格にしない", () => {
        const noStream = read(L3, { 1: 1, 2: 2 } as any);
        const r = judgeCompositeLayout({ before: noStream, afterRemove, afterRestore, person: "alice" });
        expect(r.ok).toBe(false);
        expect(r.reason).toContain("no stream number");
    });

    it("実行計画: 実際の通話の読み取りは namesUnknown を付け、リハーサルは付けない", () => {
        const real = plan(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-1", "--real-profile", ...CALL_LINE]);
        const snaps = real.steps.filter((s) => s.kind === "composite-layout-snapshot" && Object.values(AC30_ITEM_LABELS).includes(s.label));
        expect(snaps).toHaveLength(3);
        expect(snaps.every((s) => s.namesUnknown === true)).toBe(true);
        const reh = plan([...REHEARSAL, ...CALL_LINE]);
        const rehSnaps = reh.steps.filter((s) => s.kind === "composite-layout-snapshot" && Object.values(AC30_ITEM_LABELS).includes(s.label));
        expect(rehSnaps.length).toBe(3);
        expect(rehSnaps.some((s) => s.namesUnknown)).toBe(false);
    });
});
