/*
 * 実際の通話の確認 (call-1 --recheck --av-check) の判定と台本を、結果をそのまま信じられる形にした直し (11-13 の 2 回の通話で見つかった物)。
 * - --av-only: 映像と音のずれ (と遅れ) だけを測って終わる形。人の操作は、追加と起動・参加・遅れの表示・退出だけ。
 * - 参加の依頼: 返事を待たず、OBS の記録の Facts の行で全員がそろったことを見て進む (callReady)。
 * - 枠の人: OBS の記録の枠の割り当てと cordmux の録画の録画ログから引く (personScreenStreams・personCellOf)。
 * - 映像と音のずれ: テスト動画の人の枠だけを判定し (judgeAvSync)、一時的な段と時間とともに広がる傾きを分ける (judgeAvTrend)。
 * - リハーサル: テスト用の送信側では確かめられない項目は判定しない (judgeCallLog の shortRunNotJudged)。
 * run-window-plan.mjs は Node の API を呼ばないので、確認の対象には値だけを渡す。人のアカウントの ID は書かない (偽の 1 だけ)。
 */
import { describe, expect, it } from "vitest";

import {
    AV_ONLY_LEAVE_AND_QUIT_MESSAGE,
    AV_ONLY_QUIT_SETTLE_MS,
    AV_TREND_DEFECT_MS_PER_MIN,
    CALL_CONTACT_LOST_GUIDANCE,
    LATENCY_RECORDING_LABEL,
    REHEARSAL_NOT_JUDGED,
    TEXT_ID_SETTINGS_UNINSTALL,
    VESKTOP_START_AFTER_UNINSTALL_MESSAGE,
    callJoinMessage,
    callReady,
    callReadyFromLog,
    compositeCellRegions,
    judgeAvSync,
    judgeAvTrend,
    judgeCallLog,
    judgeLinesSummary,
    parseArgs,
    personCellOf,
    personScreenStreams,
    planSteps,
    recheckJudgeItems,
    rosterCheckPlanned,
} from "./run-window-plan.mjs";

type Step = Record<string, any>;

const BASE = ["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-1"];
const REAL = [...BASE, "--real-profile", "--call-window", "--decoder", "software"];
const REHEARSAL = [...BASE, "--rehearsal"];
const AV_ONLY = ["--n-screen", "3", "--n-voice", "3", "--broadcaster-id", "1", "--start-with-add", "--recheck", "--av-check", "--av-only", "--latency-person", "somebody", "--voice-channel", "lobby"];

function plan(argv: string[]) {
    const parsed = parseArgs(argv);
    expect(parsed.errors).toEqual([]);
    const r = planSteps(parsed.args);
    expect(r.ok).toBe(true);
    return { args: parsed.args, steps: r.steps as Step[] };
}
const kinds = (steps: Step[]) => steps.map((s) => s.kind);
const at = (steps: Step[], pred: (s: Step) => boolean, from = 0) => {
    const i = steps.findIndex((s, k) => k >= from && pred(s));
    expect(i).toBeGreaterThanOrEqual(0);
    return i;
};

describe("--av-only (映像と音のずれだけを測って終わる)", () => {
    it("--recheck --av-check と一緒にだけ受ける", () => {
        expect(parseArgs([...REAL, "--n-screen", "3", "--broadcaster-id", "1", "--start-with-add", "--recheck", "--av-only"]).errors.join(" ")).toContain("--av-only は --recheck --av-check と一緒に");
        expect(parseArgs([...REAL, "--n-screen", "3", "--av-only"]).errors.join(" ")).toContain("--av-only は --recheck --av-check と一緒に");
        expect(parseArgs([...REAL, ...AV_ONLY]).errors).toEqual([]);
        expect(parseArgs(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-hold", "--real-profile", "--av-only"]).errors.join(" ")).toContain("--av-only は --step call-1 だけ");
    });

    const { args, steps } = plan([...REAL, ...AV_ONLY]);

    it("人の操作は 6 つ: 終了と追加 -> 起動 -> 参加と共有 (返事は要らない) -> 遅れの表示 -> 退出と終了 -> 起動。テスト動画の共有をもう一度は頼まない", () => {
        const humans = steps.filter((s) => s.kind === "human");
        expect(humans.map((s) => s.until)).toEqual(["state-json-points-to-placed", "go-file", "call-ready", "go-file", "go-file", "go-file"]);
        expect(humans[2].message).toBe(callJoinMessage({ vesktopShare: false, voiceChannel: "lobby", recheck: true, avOnly: true }));
        expect(humans[2].message).toContain("マイクは4つともミュートにしてください");
        expect(humans[2].message).toContain("(映像と音のずれを確かめるためです)");
        expect(humans[2].message.endsWith(CALL_CONTACT_LOST_GUIDANCE)).toBe(true);
        expect(humans[3].message).toContain("大きく表示");
        expect(humans[4]).toMatchObject({ message: AV_ONLY_LEAVE_AND_QUIT_MESSAGE, quitVesktop: true });
        expect(humans[5].message).toBe(VESKTOP_START_AFTER_UNINSTALL_MESSAGE);
        expect(humans.some((s) => String(s.message).includes("『テスト動画』のタブを、音声も含めて共有してください"))).toBe(false);
    });

    it("測るのは 遅れ と av1 だけ。cordmux の録画が両方を覆い、録画の後に av1 (テスト動画の人の枠) と録画の中のずれを判定する", () => {
        const recStart = at(steps, (s) => s.kind === "dock-click" && s.textId === "cordmux.dock.record_start.button");
        const latency = at(steps, (s) => s.kind === "record-start" && s.label === LATENCY_RECORDING_LABEL, recStart);
        const av1 = at(steps, (s) => s.kind === "record-start" && s.label === "av1", latency);
        const recStop = at(steps, (s) => s.kind === "dock-click" && s.textId === "cordmux.dock.record_stop.button", av1);
        const avJudge = at(steps, (s) => s.kind === "judge-av-sync", recStop);
        expect(steps[avJudge]).toMatchObject({ recording: "av1", person: "sub2" });
        at(steps, (s) => s.kind === "judge-record-sync" && s.person === "sub2", avJudge);
        expect(steps.filter((s) => s.kind === "record-start").map((s) => s.label)).toEqual([LATENCY_RECORDING_LABEL, "av1"]);
    });

    it("ほかの確認 (バージョン・画面共有の音・解像度の変化・エンコード・別アカウント方式・自分の枠・抜け戻り・並び・自分のアカウントの待ち) は実行しない", () => {
        for (const kind of ["wait-self-account", "measure-input-level", "resize-window", "measure-video-rate", "count-roster-labels", "judge-roster-labels", "judge-log-lines"]) expect(kinds(steps)).not.toContain(kind);
        expect(steps.some((s) => s.kind === "dialog-click" && s.label === "about-versions")).toBe(false);
        expect(steps.some((s) => s.kind === "obsws-request" && s.request === "CreateScene")).toBe(false);
        expect(steps.some((s) => s.kind === "dialog-click" && s.buttonTextId === "cordmux.dock.account_change.button")).toBe(false);
        expect(rosterCheckPlanned(args)).toBe(false);
    });

    it("締め: 退出と終了の後に、実行スクリプトがアンインストールを押し (設定のページの入口)、vencordDir の戻りを判定する (gate)", () => {
        const quit = at(steps, (s) => s.kind === "human" && s.quitVesktop === true);
        const capture = at(steps, (s) => s.kind === "capture-backup", quit);
        // 返事の後、Vesktop の終了がドックに届くまで少し待ってから押す (起動している間は確認が開かない)。
        expect(steps.slice(quit, capture).filter((s) => s.kind === "wait").map((s) => s.ms)).toEqual([AV_ONLY_QUIT_SETTLE_MS]);
        const press = at(steps, (s) => s.kind === "dialog-click" && s.buttonTextId === TEXT_ID_SETTINGS_UNINSTALL, capture);
        expect(steps[press]).toMatchObject({ gate: true, noWait: true });
        const judge = at(steps, (s) => s.kind === "judge-vencord-dir-restored", press);
        expect(steps[judge]).toMatchObject({ gate: true });
        expect(steps.slice(capture, judge).some((s) => s.kind === "human")).toBe(false);
    });

    it("OBS を閉じた後は 遅れ・ログの読み・表。表は実行した項目だけ (追加・接続・遅れ・A/V の 2 行・整列のずれの傾き)", () => {
        const close = at(steps, (s) => s.kind === "close-obs");
        expect(steps.slice(close).filter((s) => String(s.kind).startsWith("judge")).map((s) => s.kind)).toEqual(["judge-latency", "judge-call-log", "judge-lines"]);
        expect(steps.find((s) => s.kind === "judge-lines")!.items).toEqual(recheckJudgeItems(args));
        expect(recheckJudgeItems(args)).toEqual(["add-to-vesktop", "vesktop-connects", "latency", "av-sync-av1", "record-sync", "align-drift"]);
    });

    it("リハーサル: gate の手順は、テスト用の送信側で起こせる物だけを待つ。判定しない項目は skip に理由を残す", () => {
        const reh = plan([...REHEARSAL, ...AV_ONLY.filter((a) => a !== "lobby" && a !== "--voice-channel")]).steps;
        expect(reh.filter((s) => s.kind === "skip").map((s) => s.item)).toEqual(["latency"]);
        expect(reh.find((s) => s.kind === "judge-call-log")).toMatchObject({ shortRunNotJudged: REHEARSAL_NOT_JUDGED["align-drift"] });
        expect(reh.find((s) => s.kind === "judge-av-sync")).toMatchObject({ person: "harness-screen-1" });
        expect(reh.filter((s) => s.kind === "human").every((s) => s.until === "seconds")).toBe(true);
    });
});

describe("参加と共有がそろったか (call-ready。OBS の記録の Facts の行)", () => {
    const facts = (roster: string, sharing: number) => `12:00:00.000: [cordmux] Facts: roster=${roster} sharing=${sharing} warnings=0 stages=2 self=yes unreadable_keys=0`;

    it("最後の Facts の行の人数で決める。roster が「-」(Vesktop のアカウントが通話にいない) なら人数は分からない", () => {
        expect(callReadyFromLog([facts("-", 0), facts("3", 2), "other"])).toEqual({ roster: 3, sharing: 2 });
        expect(callReadyFromLog([facts("4", 3), facts("-", 0)])).toEqual({ roster: null, sharing: 0 });
        expect(callReadyFromLog(["12:00:00.000: [cordmux] People in the call changed: count=4"])).toBeNull();
    });

    it("通話の人数と共有の人数が両方とも足りたときだけそろった", () => {
        const want = { roster: 4, sharing: 3 };
        expect(callReady([facts("4", 3)], want)).toBe(true);
        expect(callReady([facts("4", 2)], want)).toBe(false);
        expect(callReady([facts("3", 3)], want)).toBe(false);
        expect(callReady([facts("-", 3)], want)).toBe(false);
        expect(callReady([], want)).toBe(false);
        // 11-13 の 2 回目の通話で全員がそろった行の形。
        expect(callReady(["11:15:46.503: [cordmux] Facts: roster=4 sharing=3 warnings=0 stages=2 self=yes unreadable_keys=0"], want)).toBe(true);
    });
});

describe("枠の人を、枠の割り当てと cordmux の録画の録画ログから引く", () => {
    const sec = 1_000_000;
    const seg = (user: string, stream: number, startS: number, lengthS: number, extra: Step = {}) => ({
        type: "segment",
        user_id: user,
        kind: "screen",
        media: "video",
        stream_id: stream,
        first_pts_wall_us: startS * sec,
        first_pts_us: 0,
        last_pts_us: lengthS * sec,
        close_reason: "recording_toggled",
        ...extra,
    });
    const ledger = {
        person: [
            { user_id: "u-alice", display_name: "alice" },
            { user_id: "u-carol", display_name: "carol" },
        ],
        segment: [
            seg("u-alice", 4, 1000, 600),
            seg("u-carol", 2, 1000, 600),
            // 声の行は数えない。
            { ...seg("u-carol", 7, 1000, 600), kind: "voice" },
        ],
    };

    it("録画の区間に重なる、その人の画面共有のストリームの番号を返す (名前は display_name、なければ user_id として引く)", () => {
        expect(personScreenStreams({ ledger, person: "carol", fromMs: 1100 * 1000, toMs: 1200 * 1000 })).toEqual({ streams: [2], reason: null });
        expect(personScreenStreams({ ledger: { person: [], segment: [seg("harness-screen-1", 1, 0, 100)] }, person: "harness-screen-1", fromMs: 10_000, toMs: 20_000 }).streams).toEqual([1]);
        // 区間の外のセグメントは使わない (ストリームの番号は再接続で別の人に割り当てられうる)。
        expect(personScreenStreams({ ledger, person: "carol", fromMs: 2000 * 1000, toMs: 2100 * 1000 })).toMatchObject({ streams: [], reason: expect.stringContaining("no screen segment of carol") });
        // 閉じていないセグメントは今も続いているとみなす。
        const open = { person: ledger.person, segment: [seg("u-carol", 2, 1000, 0, { close_reason: "not_closed" })] };
        expect(personScreenStreams({ ledger: open, person: "carol", fromMs: 5000 * 1000, toMs: 5100 * 1000 }).streams).toEqual([2]);
        expect(personScreenStreams({ ledger: null, person: "carol", fromMs: 0, toMs: 1 })).toMatchObject({ streams: [], reason: "no cordmux recording ledger" });
    });

    it("同じ区間に別の人が同じ番号を持っていたら、その番号では決めない", () => {
        const clash = { person: ledger.person, segment: [seg("u-carol", 2, 1000, 600), seg("u-alice", 2, 1000, 600)] };
        expect(personScreenStreams({ ledger: clash, person: "carol", fromMs: 1100 * 1000, toMs: 1200 * 1000 })).toMatchObject({ streams: [], reason: expect.stringContaining("also used by another person") });
    });

    it("録画の始めと終わりの両方で、そのストリームを運んでいた枠が 1 つで同じなら、その枠 (枠の番号 - 1)", () => {
        const capture = (when: string, slotStreams: Record<number, number>) => ({ when, cells: Object.entries(slotStreams).map(([slot, stream]) => ({ cell: Number(slot) - 1, stream })) });
        // 11-13 の 2 回目: Screen slot 1 carries stream 2 (carol) -> 合成のセル 0。
        expect(personCellOf({ captures: [capture("start", { 1: 2, 2: 3, 3: 1 }), capture("end", { 1: 2, 2: 3, 3: 1 })], streams: [2] })).toEqual({ cell: 0, reason: null });
        expect(personCellOf({ captures: [capture("start", { 1: 2 }), capture("end", { 2: 2 })], streams: [2] }).reason).toContain("moved to another frame");
        expect(personCellOf({ captures: [capture("start", { 1: 2 }), capture("end", { 1: 3 })], streams: [2] }).reason).toContain("the end");
        expect(personCellOf({ captures: [], streams: [2] }).cell).toBeNull();
        expect(personCellOf({ captures: [capture("start", { 1: 2 })], streams: [] }).reason).toBe("no stream to look for");
    });

    it("配置の行のセルに、枠の割り当てのストリームの番号を入れる", () => {
        const layout = { canvas: { width: 1920, height: 1080 }, count: 2, tiles: [{ slot: 1, x: 0, y: 0, w: 960, h: 540 }, { slot: 3, x: 960, y: 0, w: 960, h: 540 }] };
        const r = compositeCellRegions({ layout, videoSettings: {}, persons: {}, slotStreams: { 1: 2, 3: 1 } });
        expect(r.cells.map((c: Step) => [c.cell, c.stream, c.person])).toEqual([[0, 2, null], [2, 1, null]]);
        expect(compositeCellRegions({ layout, videoSettings: {}, persons: {} }).cells.map((c: Step) => c.stream)).toEqual([null, null]);
    });
});

describe("映像と音のずれの判定 (テスト動画の人の枠だけ・一時的な段と広がりを分ける)", () => {
    const trendAt = (baseline: Step, extra: Step = {}) => ({ pairs: 60, span_s: 295, slope_ms_per_min: 3.12, slope_ci95_ms_per_min: [1.68, 4.57], steps: [], ends_off_baseline: false, baseline, ...extra });

    it("段を除いた基準の傾きで判定する: 段は数と高さを残し、欠陥にはしない (11-13 の 2 回目の合成のセル 0 の形)", () => {
        // 全部の組の傾きは 3.12 ms/分で 0 と分けられる (前の判定では欠陥) が、基準は段のように留まり、独立な組が少なく幅が出ない。
        const t = judgeAvTrend(trendAt(
            { pairs: 43, span_s: 245, slope_ms_per_min: 2.4, slope_ci95_ms_per_min: null, lag1_autocorr: 0.914, effective_pairs: 1.9 },
            { steps: [{ start_s: 161.8, end_s: 191.8, pairs: 7, height_ms: 25.9, returned: true }] },
        ));
        expect(t).toMatchObject({ verdict: "undetermined", fitOn: "baseline", slopeMsPerMin: 2.4, allPairsSlopeMsPerMin: 3.12, stepCount: 1, steps: [{ startS: 161.8, endS: 191.8, heightMs: 25.9, returned: true }] });
        expect(t.note).toContain("levels");
    });

    it("基準の傾きが線を超えて 0 と分けられれば欠陥 (時間とともに広がる)。線の内なら長い通話で確認する", () => {
        expect(judgeAvTrend(trendAt({ pairs: 29, span_s: 140, slope_ms_per_min: 12.18, slope_ci95_ms_per_min: [11.37, 12.99] })).verdict).toBe("defect");
        expect(judgeAvTrend(trendAt({ pairs: 24, span_s: 140, slope_ms_per_min: 0, slope_ci95_ms_per_min: [0, 0] })).verdict).toBe("long-call");
        expect(judgeAvTrend(trendAt({ pairs: 40, span_s: 245, slope_ms_per_min: 2.36, slope_ci95_ms_per_min: [-2.59, 7.31] })).verdict).toBe("undetermined");
        expect(AV_TREND_DEFECT_MS_PER_MIN).toBe(1.0);
    });

    it("録画の終わりが段の途中なら、戻るかが分からないので決めない。基準の組が少なければ判定しない", () => {
        expect(judgeAvTrend(trendAt({ pairs: 56, span_s: 275, slope_ms_per_min: -0.11, slope_ci95_ms_per_min: [-0.31, 0.09] }, { ends_off_baseline: true })).verdict).toBe("undetermined");
        expect(judgeAvTrend(trendAt({ pairs: 8, span_s: 245, slope_ms_per_min: 0, slope_ci95_ms_per_min: [0, 0] })).verdict).toBe("not-judged");
    });

    it("基準のない前の形の出力は、全部の組の傾きで判定する", () => {
        const old = { pairs: 60, span_s: 295, slope_ms_per_min: 3.12, slope_ci95_ms_per_min: [1.68, 4.57] };
        expect(judgeAvTrend(old)).toMatchObject({ verdict: "defect", fitOn: "all-pairs", steps: null });
        expect(judgeAvTrend({ pairs: 60, span_s: 295, slope_ms_per_min: 3.12 })).toMatchObject({ verdict: "not-judged" });
    });

    const cell = (paired: number, mean: number) => ({ paired, audio_minus_video_ms: { mean, min: mean, max: mean }, trend: trendAt({ pairs: paired, span_s: 245, slope_ms_per_min: 0.1, slope_ci95_ms_per_min: [-0.2, 0.4] }) });
    const judged = { ok: true, perCell: [{ cell: 0, data: cell(50, 102.6) }, { cell: 1, data: cell(50, 11.7) }, { cell: 2, data: cell(50, 300) }] };

    it("テスト動画の人の枠だけを判定し、ほかの人の枠 (画面全体の共有にテスト動画が写る) は判定しない", () => {
        const r = judgeAvSync(judged, { person: "carol", cell: 0, cellReason: null });
        expect(r).toMatchObject({ ok: true, judgedCells: 1, person: "carol", personCell: 0 });
        expect(r.cells.map((c: Step) => [c.cell, c.verdict])).toEqual([[0, "within"], [1, "other-person"], [2, "other-person"]]);
        // 人を渡さなければ、前と同じく全部の枠 (枠 2 は線の外)。
        expect(judgeAvSync(judged)).toMatchObject({ ok: false, reason: "outside the line on cell 2" });
    });

    it("テスト動画の人の枠が分からなければ判定しない (ok null。理由を残す)", () => {
        const r = judgeAvSync(judged, { person: "carol", cell: null, cellReason: "no cordmux recording covers the recording" });
        expect(r).toMatchObject({ ok: null, personCell: null, judgedCells: 0 });
        expect(r.reason).toContain("no cordmux recording covers the recording");
        // judge-lines の行にも理由が出る (判定できない行でも、なぜかが表で読める)。
        const row = judgeLinesSummary([{ name: "judge-av-sync", recording: "av1", ...r }] as any, { items: ["av-sync-av1"] }).rows[0];
        expect(row).toMatchObject({ ok: null, reason: expect.stringContaining("the frame of carol was not found") });
    });

    it("judge-lines の A/V の行は、人と、傾きの判定・基準の傾き・段を残す", () => {
        const r = judgeAvSync({ ok: true, perCell: [{ cell: 0, data: { ...cell(50, 102.6), trend: trendAt({ pairs: 43, span_s: 245, slope_ms_per_min: 2.4, slope_ci95_ms_per_min: null }, { steps: [{ start_s: 161.8, end_s: 191.8, pairs: 7, height_ms: 25.9, returned: true }] }) } }] }, { person: "carol", cell: 0 });
        const row = judgeLinesSummary([{ name: "judge-av-sync", recording: "av1", ...r }] as any, { items: ["av-sync-av1"] }).rows[0];
        expect(row).toMatchObject({ item: "av-sync-av1", ok: true });
        expect(row.detail).toEqual([{ cell: 0, person: "carol", meanMs: 102.6, paired: 50, trend: "undetermined", slopeMsPerMin: 2.4, ci95MsPerMin: null, fitOn: "baseline", steps: [{ startS: 161.8, endS: 191.8, heightMs: 25.9, returned: true }] }]);
    });
});

describe("整列のずれの傾き (judgeCallLog) の短い実行", () => {
    const statsLine = (t: number, frames: number, offsetUs: number) => {
        const hh = String(Math.floor(t / 3600)).padStart(2, "0");
        const mm = String(Math.floor((t % 3600) / 60)).padStart(2, "0");
        const ss = String(Math.floor(t % 60)).padStart(2, "0");
        return `${hh}:${mm}:${ss}.000: [cordmux] Stats stream=1 align period_us=16666.7 frames=${frames} realign=0 relock=0 offset_us=${offsetUs}`;
    };
    const short = Array.from({ length: 40 }, (_, i) => statsLine(36000 + i * 10, (i + 1) * 600, 0));

    it("傾きを読める長さのストリームがなければ、リハーサル (shortRunNotJudged) は判定せず、実際の通話は失敗", () => {
        expect(judgeCallLog(short, {})).toMatchObject({ ok: false, reason: "no stream had stats lines long enough to read the drift" });
        const reh = judgeCallLog(short, { shortRunNotJudged: "short rehearsal" });
        expect(reh).toMatchObject({ ok: null, notJudged: "short rehearsal" });
        // 長く続いたストリームがあれば、リハーサルでも判定する。
        const long = Array.from({ length: 80 }, (_, i) => statsLine(36000 + i * 10, (i + 1) * 600, 0));
        expect(judgeCallLog(long, { shortRunNotJudged: "short rehearsal" })).toMatchObject({ ok: true });
    });
});
