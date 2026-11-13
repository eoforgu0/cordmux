/*
 * 実際の通話での確認 (call-1 の --extra-checks) に足した 3 つを、実際の通話でしか結果が出ないものに絞ってテストする。
 *   (1) 声の音量: 使う声を置き換えた後に、人が行の [...] のメニューの「声の音量」を 200% にし、録画の音量の比を見る。
 *   (2) 実際のアバターの並び: 終わりの「共有を止めて通話を切り、Vesktop を終了」を 2 つに分け、共有を止めた後に cordmux のソースのスクリーンショットを撮る。
 *   (3) 映像のバッファの統計: メインの手順の録画の間に、ドックの [状態] タブを開いてスクリーンショットを撮り、[参加者] タブへ戻す。
 * ステップの並び、判定の純粋関数 (音量の比、並びの行)、読むだけの操作を操作の数に含めないこと、実行スクリプトの受け渡し (テキストとして読む) をテストする。
 * run-window-plan.mjs は Node の API を呼ばないので、テストする対象には値だけを渡す。人のアカウントの ID は書かない (作り物の 1 だけ)。
 */
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

import {
    AC32_GAIN_TOLERANCE_DB,
    HUMAN_TIMEOUT_MS,
    INCOMING_VIDEO_LABEL,
    JUDGE_LINES,
    ROSTER_BASELINE_LABEL,
    rosterExpectedPeople,
    ROSTER_LABEL_SETTLE_MS,
    ROSTER_SHOT_LABEL,
    TEXT_ID_AUDIO_VOICE_KIND,
    TEXT_ID_TAB_AUDIO,
    TEXT_ID_TAB_PEOPLE,
    TEXT_ID_TAB_STATUS,
    TONE_AUDIBLE_RMS,
    VESKTOP_HANG_UP_AND_QUIT_MESSAGE,
    VESKTOP_HIDDEN_ICON_HINT,
    VESKTOP_LEAVE_AND_QUIT_MESSAGE,
    VESKTOP_STOP_SHARING_MESSAGE,
    VOICE_SOURCE_CHECKS,
    VOICE_SOURCE_CHECK_RECORD_MS,
    VOICE_VOLUME_AFTER_LABEL,
    VOICE_VOLUME_BEFORE_LABEL,
    VOICE_VOLUME_CLIP_RMS,
    VOICE_VOLUME_GAIN_TOLERANCE_DB,
    VOICE_VOLUME_PCT,
    VOICE_VOLUME_SETTLE_MS,
    judgeLinesSummary,
    judgeOperationCount,
    judgeRosterLabels,
    judgeVoiceVolume,
    parseArgs,
    planSteps,
    rosterCheckPlanned,
    rosterLabelReadyFromLog,
    stepTextIds,
    DEFAULT_CALL_ACCOUNTS,
} from "./run-window-plan.mjs";

type Step = Record<string, any>;

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = join(HERE, "..", "..", "..");
const BASE = ["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-1"];
const REAL = [...BASE, "--real-profile", "--call-window", "--decoder", "software"];
const REHEARSAL = [...BASE, "--rehearsal"];
const STAGE = ["--n-screen", "3", "--n-voice", "3", "--broadcaster-id", "1", "--start-with-add", "--vesktop-share", "--skip-continuous", "--skip-speaker-mute", "--extra-checks", "--skip-export-bundle"];

function plan(argv: string[]) {
    const parsed = parseArgs(argv);
    expect(parsed.errors).toEqual([]);
    const r = planSteps(parsed.args);
    expect(r.ok).toBe(true);
    return { args: parsed.args, steps: r.steps as Step[] };
}
const at = (steps: Step[], pred: (s: Step) => boolean, from = 0) => {
    const i = steps.findIndex((s, k) => k >= from && pred(s));
    expect(i).toBeGreaterThanOrEqual(0);
    return i;
};
const text = (name: string) => readFileSync(join(HERE, name), "utf8");
function iniKeys(file: string): Set<string> {
    const keys = new Set<string>();
    for (const line of readFileSync(join(REPO, "src-obs", "data", "locale", file), "utf8").split(/\r?\n/)) {
        const t = line.trim();
        if (!t || t.startsWith("#") || t.indexOf("=") < 0) continue;
        keys.add(t.slice(0, t.indexOf("=")).trim());
    }
    return keys;
}

describe("(1) 声の音量", () => {
    const { steps } = plan([...REAL, ...STAGE]);

    it("置き換えの確認 (所有者・声を貸す人) の次、戻してもらう前に、所有者の声だけを鳴らして音量を上げてもらい、8 秒録る", () => {
        const fromCheck = at(steps, (s) => s.kind === "record-start" && s.label === VOICE_SOURCE_CHECKS[1].label);
        const volume = at(steps, (s) => s.kind === "record-start" && s.label === VOICE_VOLUME_AFTER_LABEL);
        const back = at(steps, (s) => s.kind === "human" && String(s.message).includes("元に戻して"));
        expect(volume).toBeGreaterThan(fromCheck);
        expect(volume).toBeLessThan(back);
        // 録画の前: 2 人の声のミュート (所有者だけ外す) -> 人の操作 -> 待ち -> 録画。
        const ask = at(steps, (s) => s.kind === "human" && String(s.message).includes("{{" + TEXT_ID_AUDIO_VOICE_KIND + "}}"), fromCheck);
        expect(ask).toBeLessThan(volume);
        const mutes = steps.slice(ask - 2, ask);
        expect(mutes.map((s) => [s.textArgs[0], s.desired])).toEqual([["sub2", "on"], [DEFAULT_CALL_ACCOUNTS.firefox, "off"]]);
        expect(mutes.every((s) => s.kind === "dock-click" && s.muteOp === true)).toBe(true);
        expect(steps[ask + 1]).toMatchObject({ kind: "wait", ms: VOICE_VOLUME_SETTLE_MS });
        expect(steps[volume]).toMatchObject({ kind: "record-start", stream: false });
        expect(steps[volume + 1]).toMatchObject({ kind: "wait", ms: VOICE_SOURCE_CHECK_RECORD_MS });
        expect(steps[volume + 2]).toMatchObject({ kind: "record-stop", label: VOICE_VOLUME_AFTER_LABEL, stream: false, tailSeconds: 0 });
        // 録画の後に全員のミュートを外す。
        expect(steps.slice(volume + 3, volume + 5).map((s) => [s.textArgs[0], s.desired])).toEqual([["sub2", "off"], ["sub3", "off"]]);
    });

    it("人への依頼は音声タブと所有者の行の「声」のつまみの画面のテキスト (ini から引く) を指定し、200% と書く", () => {
        const ask = steps.find((s) => s.kind === "human" && String(s.message).includes("{{" + TEXT_ID_AUDIO_VOICE_KIND + "}}"))!;
        // 画面のテキストは textRef (ini のキー) で指定する。dry-run が ini から引ける。
        expect(String(ask.message)).toContain("{{" + TEXT_ID_TAB_AUDIO + "}}");
        expect(String(ask.message)).toContain("{{" + TEXT_ID_AUDIO_VOICE_KIND + "}}");
        expect(String(ask.message)).toContain("{{" + TEXT_ID_TAB_PEOPLE + "}}");
        expect(String(ask.message)).toContain(DEFAULT_CALL_ACCOUNTS.firefox + "の行");
        expect(String(ask.message)).toContain(VOICE_VOLUME_PCT + "%");
        expect(ask).toMatchObject({ until: "go-file", timeoutMs: HUMAN_TIMEOUT_MS });
        for (const id of [TEXT_ID_TAB_AUDIO, TEXT_ID_TAB_PEOPLE, TEXT_ID_AUDIO_VOICE_KIND]) {
            expect(stepTextIds(ask)).toContain(id);
            for (const file of ["ja-JP.ini", "en-US.ini"]) expect(iniKeys(file).has(id)).toBe(true);
        }
    });

    it("比べる相手は置き換えの確認の 1 つ目 (所有者の声だけを鳴らす条件。100%)。--extra-checks がない場合とリハーサルの扱い", () => {
        expect(VOICE_VOLUME_BEFORE_LABEL).toBe(VOICE_SOURCE_CHECKS[0].label);
        expect(VOICE_SOURCE_CHECKS[0]).toMatchObject({ keep: "owner", phase: "switched", expectTone: true });
        const judge = steps.find((s) => s.kind === "judge-voice-volume")!;
        expect(judge).toMatchObject({ before: VOICE_VOLUME_BEFORE_LABEL, after: VOICE_VOLUME_AFTER_LABEL, pct: VOICE_VOLUME_PCT, toleranceDb: VOICE_VOLUME_GAIN_TOLERANCE_DB, hz: 1000 });
        expect(judge.recordSeconds).toBe(VOICE_SOURCE_CHECK_RECORD_MS / 1000);
        // 録画を読む判定は OBS を閉じた後。
        expect(steps.indexOf(judge)).toBeGreaterThan(at(steps, (s) => s.kind === "close-obs"));
        const without = plan([...REAL, "--broadcaster-id", "1"]).steps;
        expect(without.some((s) => s.kind === "judge-voice-volume" || (s.kind === "record-start" && s.label === VOICE_VOLUME_AFTER_LABEL))).toBe(false);
        // リハーサルはテスト用の送信側の声の名前で同じ形を実行する。
        const rehearsal = plan([...REHEARSAL, ...STAGE]).steps;
        const rec = at(rehearsal, (s) => s.kind === "record-start" && s.label === VOICE_VOLUME_AFTER_LABEL);
        expect(rehearsal.some((s) => s.kind === "judge-voice-volume")).toBe(true);
        expect(rehearsal[rec - 1]).toMatchObject({ kind: "wait", ms: VOICE_VOLUME_SETTLE_MS });
    });

    it("許容幅は声のミュートの往復と同じ 1.5 dB で、200% は +6.02 dB。判定の表に載る", () => {
        expect(VOICE_VOLUME_GAIN_TOLERANCE_DB).toBe(AC32_GAIN_TOLERANCE_DB);
        expect(VOICE_VOLUME_PCT).toBe(200);
        const row = JUDGE_LINES.find((l) => l.item === "voice-volume")!;
        expect(row).toMatchObject({ onFail: "record" });
        expect(row.line).toContain("+6.02 dB");
        expect(JUDGE_LINES.find((l) => l.item === "roster-labels")).toMatchObject({ onFail: "record" });
    });

    describe("judgeVoiceVolume", () => {
        const heard = (rms: number) => ({ rms, toneShare: 0.98 });
        it("+6.02 dB ちょうどと、幅の内は合格。幅の外は失敗で、差を返す", () => {
            const exact = judgeVoiceVolume({ before: heard(0.05), after: heard(0.1) });
            expect(exact).toMatchObject({ ok: true, decidable: true, expectedDb: 6.02, gainDb: 6.02, errorDb: 0 });
            expect(judgeVoiceVolume({ before: heard(0.05), after: heard(0.05 * 10 ** (7.4 / 20)) })).toMatchObject({ ok: true, gainDb: 7.4 });
            expect(judgeVoiceVolume({ before: heard(0.05), after: heard(0.05 * 10 ** (4.6 / 20)) })).toMatchObject({ ok: true });
            const low = judgeVoiceVolume({ before: heard(0.05), after: heard(0.05 * 10 ** (3 / 20)) });
            expect(low).toMatchObject({ ok: false, gainDb: 3, errorDb: -3.02 });
            expect(low.reason).toContain("3 dB");
            // 変わっていない (音量が反映されていない) は失敗。
            expect(judgeVoiceVolume({ before: heard(0.05), after: heard(0.05) })).toMatchObject({ ok: false, gainDb: 0 });
            // 上げた後が無音は失敗。
            expect(judgeVoiceVolume({ before: heard(0.05), after: { rms: 0, toneShare: 0 } })).toMatchObject({ ok: false, gainDb: null });
        });

        it("基準が鳴っていない・読めていない・クリップするときは判定しない (ok は null)", () => {
            expect(judgeVoiceVolume({ before: heard(0.05), after: { rms: null, toneShare: null } })).toMatchObject({ ok: null, decidable: false });
            expect(judgeVoiceVolume({})).toMatchObject({ ok: null, decidable: false });
            expect(judgeVoiceVolume({ before: heard(TONE_AUDIBLE_RMS / 2), after: heard(0.04) })).toMatchObject({ ok: null, decidable: false });
            expect(judgeVoiceVolume({ before: { rms: 0.05, toneShare: 0.2 }, after: heard(0.1) })).toMatchObject({ ok: null, decidable: false });
            const clip = judgeVoiceVolume({ before: heard(VOICE_VOLUME_CLIP_RMS), after: heard(VOICE_VOLUME_CLIP_RMS) });
            expect(clip).toMatchObject({ ok: null, decidable: false });
            expect(clip.reason).toContain("clip");
            // クリップしない大きさのぎりぎり下は判定する。
            expect(judgeVoiceVolume({ before: heard(VOICE_VOLUME_CLIP_RMS * 0.99), after: heard(VOICE_VOLUME_CLIP_RMS * 0.99 * 2) }).decidable).toBe(true);
        });

        it("クリップする大きさ: 測った比は下限としてだけ使い、下端以上なら成功 (上端は確認しない)。下端に届かなければ判定しない", () => {
            // 実際の通話の測定値 (100% が 0.4996、200% が 0.9439 = +5.53 dB。期待 +6.02 dB の幅の下端は +4.52 dB)。
            const real = judgeVoiceVolume({ before: { rms: 0.499601, toneShare: 0.997 }, after: { rms: 0.943899, toneShare: 1 } });
            expect(real).toMatchObject({ ok: true, decidable: true, clipped: true, gainIsLowerBound: true, upperChecked: false, gainDb: 5.53 });
            expect(real.note).toContain("lower bound");
            // 下端に届かない側は、クリップして低く出ただけかもしれないので判定しない。
            const low = judgeVoiceVolume({ before: heard(0.5), after: heard(0.7) });
            expect(low).toMatchObject({ ok: null, decidable: false, clipped: true, gainIsLowerBound: true });
            // 後が無音なら下限も判定できない。
            expect(judgeVoiceVolume({ before: heard(0.5), after: { rms: 0, toneShare: 0 } })).toMatchObject({ ok: null, decidable: false });
            // クリップしない大きさでは幅の両側を見る (clipped は付かない)。
            expect(judgeVoiceVolume({ before: heard(0.05), after: heard(0.1) }).clipped).toBeUndefined();
        });

        it("音量の指定が変われば期待の dB も変わる (利得は割合に比例する)", () => {
            expect(judgeVoiceVolume({ before: heard(0.05), after: heard(0.075), pct: 150 })).toMatchObject({ ok: true, expectedDb: 3.52 });
            expect(judgeVoiceVolume({ before: heard(0.05), after: heard(0.025), pct: 50 })).toMatchObject({ ok: true, expectedDb: -6.02 });
        });

        it("judgeLinesSummary が表に載せる (判定できない場合は ok null で失敗にならない)", () => {
            const rows = judgeLinesSummary([
                { name: "judge-voice-volume", ...judgeVoiceVolume({ before: heard(0.05), after: heard(0.1) }) },
                { name: "judge-roster-labels", ...judgeRosterLabels({ lines: ["20:00:00.000: [cordmux] Composite roster shows 1 participant(s)"], baselineCount: 0, expectedPeople: 3 }) },
            ] as any).rows;
            expect(rows.find((r: Step) => r.item === "voice-volume")).toMatchObject({ ok: true, detail: { gainDb: 6.02, expectedDb: 6.02 } });
            // 1 人しか並んでいない (期待は 3 人) ので失敗。3 人並べば成功。
            expect(rows.find((r: Step) => r.item === "roster-labels")).toMatchObject({ ok: false, detail: { people: 1 } });
            const threeRows = judgeLinesSummary([
                { name: "judge-roster-labels", ...judgeRosterLabels({ lines: ["20:00:00.000: [cordmux] Composite roster label ready: index=0 paint_ms=1", "20:00:00.100: [cordmux] Composite roster shows 3 participant(s)"], baselineCount: 0, expectedPeople: 3 }) },
            ] as any).rows;
            expect(threeRows.find((r: Step) => r.item === "roster-labels")).toMatchObject({ ok: true, detail: { people: 3 } });
            const undecided = judgeLinesSummary([{ name: "judge-voice-volume", ...judgeVoiceVolume({}) }] as any);
            expect(undecided.rows.find((r: Step) => r.item === "voice-volume")!.ok).toBe(null);
            expect(undecided.red).not.toContain("voice-volume");
        });
    });
});

describe("(2) 実際のアバターの並び", () => {
    const { args, steps } = plan([...REAL, ...STAGE]);

    it("--extra-checks の実際の通話だけ。リハーサルとオプションがない場合は 1 つのメッセージのまま", () => {
        expect(rosterCheckPlanned(args)).toBe(true);
        expect(rosterCheckPlanned(plan([...REHEARSAL, ...STAGE]).args)).toBe(false);
        expect(rosterCheckPlanned(plan([...REAL, "--broadcaster-id", "1"]).args)).toBe(false);
        expect(rosterCheckPlanned(plan([...BASE.slice(0, -2), "--step", "call-hold", "--real-profile", "--call-window", "--decoder", "software"]).args)).toBe(false);
        for (const argv of [[...REHEARSAL, ...STAGE], [...REAL, "--broadcaster-id", "1"]]) {
            const other = plan(argv).steps;
            expect(other.some((s) => s.kind === "count-roster-labels" || s.kind === "judge-roster-labels")).toBe(false);
            expect(other.some((s) => s.kind === "human" && s.message === VESKTOP_STOP_SHARING_MESSAGE)).toBe(false);
        }
        expect(plan([...REAL, "--broadcaster-id", "1"]).steps.some((s) => s.kind === "human" && s.message === VESKTOP_HANG_UP_AND_QUIT_MESSAGE)).toBe(true);
    });

    it("保存する -> 共有を止めてもらう -> 待つ -> cordmux のソースと PNG のスクリーンショット -> 行を数える -> 通話を切って Vesktop を終了 -> バックアップ -> アンインストール", () => {
        const count = at(steps, (s) => s.kind === "count-roster-labels");
        expect(steps[count]).toMatchObject({ label: ROSTER_BASELINE_LABEL });
        expect(steps[count + 1]).toMatchObject({ kind: "human", message: VESKTOP_STOP_SHARING_MESSAGE, until: "go-file", timeoutMs: HUMAN_TIMEOUT_MS });
        expect(steps[count + 2]).toMatchObject({ kind: "wait", ms: ROSTER_LABEL_SETTLE_MS });
        expect(steps[count + 3]).toMatchObject({ kind: "screenshot-source", source: { kind: "composite" }, label: ROSTER_SHOT_LABEL });
        expect(steps[count + 4]).toMatchObject({ kind: "screenshot-window", target: "first", shotLabel: ROSTER_SHOT_LABEL });
        // 期待の人数は docs/spec.md の並びの決まりから出す (rosterExpectedPeople): 通話の 4 つのアカウントから、別アカウント方式の
        // サブアカウント (Vesktop) を引いた 3 人。メインアカウントは、自分の枠が出ていなければ並ぶ (11-13 の実際の通話でも 3 人並んだ)。
        expect(steps[count + 5]).toMatchObject({ kind: "judge-roster-labels", baseline: ROSTER_BASELINE_LABEL, expectedPeople: rosterExpectedPeople({ broadcasterId: "1" }) });
        expect(rosterExpectedPeople({ broadcasterId: "1" })).toBe(3);
        // メインアカウントを選ばない (--broadcaster-id がない) と、Vesktop のアカウントもサブアカウントとして外れないので 4 人。
        expect(rosterExpectedPeople({ broadcasterId: null })).toBe(4);
        expect(steps[count + 5].gate).toBeUndefined();
        const quit = steps[count + 6];
        expect(quit).toMatchObject({ kind: "human", message: VESKTOP_LEAVE_AND_QUIT_MESSAGE, quitVesktop: true, until: "go-file", timeoutMs: HUMAN_TIMEOUT_MS });
        // 終了のメッセージの後に Vencord の設定の比較、その後にバックアップの読み取りとアンインストール。
        expect(steps[count + 7]).toMatchObject({ kind: "compare-vencord-settings" });
        expect(steps[count + 8]).toMatchObject({ kind: "capture-backup" });
        // 共有を止めてもらうのは、通話の判定とログの回収の後で、アンインストールより前。
        expect(count).toBeGreaterThan(at(steps, (s) => s.kind === "collect-plugin-log"));
        expect(count).toBeLessThan(at(steps, (s) => s.kind === "human" && s.until === "state-json-restored"));
    });

    it("メッセージ: 共有を止めるメッセージは通話に居続けるよう伝え、終了のメッセージは 4 つとも抜けて [Quit] を依頼し、隠れたアイコンの 1 文を添える", () => {
        expect(VESKTOP_STOP_SHARING_MESSAGE).toContain("画面共有を止めて");
        expect(VESKTOP_STOP_SHARING_MESSAGE).toContain("抜けないでください");
        expect(VESKTOP_STOP_SHARING_MESSAGE).not.toContain("[Quit]");
        expect(VESKTOP_LEAVE_AND_QUIT_MESSAGE).toContain("[Quit]");
        expect(VESKTOP_LEAVE_AND_QUIT_MESSAGE).toContain("通知領域のVesktopのアイコンを右クリック");
        expect(VESKTOP_LEAVE_AND_QUIT_MESSAGE).toContain(VESKTOP_HIDDEN_ICON_HINT);
        expect(VESKTOP_LEAVE_AND_QUIT_MESSAGE).not.toContain("Vesktopを閉じ");
        // 分けない場合のために、1 つのメッセージも残す。
        expect(VESKTOP_HANG_UP_AND_QUIT_MESSAGE).toContain("通話を切り");
        // 人に見せるメッセージでは、英数字と日本語の間に半角の空白を入れない。
        for (const m of [VESKTOP_STOP_SHARING_MESSAGE, VESKTOP_LEAVE_AND_QUIT_MESSAGE]) {
            expect(m).not.toMatch(/[A-Za-z0-9] [\u3041-\u3093\u30a1-\u30f6\u4e00-\u9fa0]|[\u3041-\u3093\u30a1-\u30f6\u4e00-\u9fa0] [A-Za-z0-9]/);
        }
    });

    describe("rosterLabelReadyFromLog と judgeRosterLabels", () => {
        const line = (time: string, index: number) => `${time}: [cordmux] Composite roster label ready: index=${index} paint_ms=3 upload_ms=1 total=${index + 1} strips=2 upload_max_ms=0.40`;
        const lines = [line("20:00:00.000", 0), "20:00:01.000: [cordmux] Composite layout: x", line("20:30:10.000", 0), line("20:30:10.500", 1), line("20:30:11.000", 2), line("20:30:12.000", 1)];

        it("行の番号と時刻を拾い、ほかの行と似たテキストは読まない", () => {
            expect(rosterLabelReadyFromLog(lines)).toEqual([
                { index: 0, t: 20 * 3600 },
                { index: 0, t: 20 * 3600 + 30 * 60 + 10 },
                { index: 1, t: 20 * 3600 + 30 * 60 + 10.5 },
                { index: 2, t: 20 * 3600 + 30 * 60 + 11 },
                { index: 1, t: 20 * 3600 + 30 * 60 + 12 },
            ]);
            expect(rosterLabelReadyFromLog(["[cordmux] Composite roster label failed: index=1", "Composite roster label ready: index=x"])).toEqual([]);
        });

        const shows = (time: string, n: number) => `${time}: [cordmux] Composite roster shows ${n} participant(s)`;

        it("並んだ人数は「Composite roster shows N participant(s)」の最後の値で数え、期待の人数と等しいときだけ成功", () => {
            // 4 人並んだ (期待は 3 人): 並べない人 (サブアカウント) が並んだ形は失敗。名前ラベルの行の index (0 と 1 だけ) には引きずられない。
            const four = judgeRosterLabels({ lines: [...lines.slice(0, 4), shows("20:30:10.600", 4)], baselineCount: 1, expectedPeople: 3 });
            expect(four).toMatchObject({ ok: false, baselineCount: 1, addedLines: 2, people: 4, expectedPeople: 3, complete: false });
            expect(four.reason).toContain("the roster shows 4 people, expected 3");
            // 途中で 4 人になってから 3 人に落ち着いた: 最後の値で成功。
            const settled = [shows("20:30:09.000", 4), line("20:30:10.000", 0), shows("20:30:11.000", 3)];
            expect(judgeRosterLabels({ lines: settled, baselineCount: 0, expectedPeople: 3 })).toMatchObject({ ok: true, people: 3, peopleAt: 20 * 3600 + 30 * 60 + 11, complete: true });
            // 1 人しか並ばない (並ぶはずの人が並ばない): 失敗。
            expect(judgeRosterLabels({ lines: [shows("20:30:10.000", 1)], baselineCount: 0, expectedPeople: 3 })).toMatchObject({ ok: false, people: 1, complete: false });
            // 0 人の行も読む。
            expect(judgeRosterLabels({ lines: [shows("20:30:10.000", 0)], baselineCount: 0, expectedPeople: 3 })).toMatchObject({ ok: false, people: 0 });
            // 期待の人数がなければ、決めつけずに失敗 (理由つき)。
            expect(judgeRosterLabels({ lines: [shows("20:30:10.000", 3)], baselineCount: 0 })).toMatchObject({ ok: false, expectedPeople: null, reason: "the expected number of people was not given" });
        });

        it("人数の行が無ければ失敗 (理由つき。古いバージョンのプラグイン)。保存した数がない・読めない値は 0 から数える", () => {
            const none = judgeRosterLabels({ lines, baselineCount: 1, expectedPeople: 3 });
            expect(none).toMatchObject({ ok: false, people: null, addedLines: 4 });
            expect(none.reason).toContain("no roster count line");
            expect(judgeRosterLabels({ lines: [], baselineCount: 0 }).ok).toBe(false);
            expect(judgeRosterLabels({ lines, baselineCount: undefined as any }).addedLines).toBe(5);
            expect(judgeRosterLabels({ lines, baselineCount: -3 }).baselineCount).toBe(0);
            expect(judgeRosterLabels({ lines: ["Composite roster shows x participant(s)", "[cordmux] Composite roster shows 2 participants"], baselineCount: 0 }).people).toBe(null);
        });
    });

    it("実行スクリプトが追加したステップの種類を処理する (テキストとして読む)", () => {
        const source = text("run-window.mjs");
        const handled = source.slice(source.indexOf("const HANDLED_KINDS = ["), source.indexOf("];", source.indexOf("const HANDLED_KINDS = [")));
        for (const kind of ["count-roster-labels", "judge-roster-labels", "judge-voice-volume"]) {
            expect(handled).toContain('"' + kind + '"');
            expect(source).toContain('case "' + kind + '": {');
        }
    });
});

describe("(3) 映像のバッファの統計 (ドックの [状態] タブ)", () => {
    const { steps } = plan([...REAL, ...STAGE]);

    it("定常状態の待ちと判定の後、メインの手順の録画を止める前に、ドックの [状態] タブを選んで読み、スクリーンショットを撮り、[参加者] タブへ戻す", () => {
        const stats = at(steps, (s) => s.kind === "obs-stats" && s.label === "last");
        const skipped = at(steps, (s) => s.kind === "judge-output-skipped");
        const first = at(steps, (s) => s.kind === "dialog-click" && s.buttonTextId === TEXT_ID_TAB_STATUS);
        const stop = at(steps, (s) => s.kind === "record-stop" && s.label === "main");
        expect(skipped).toBeGreaterThan(stats);
        expect(first).toBeGreaterThan(skipped);
        expect(first).toBeLessThan(stop);
        // メッセージの件数があるとタブの名前は「状態 (N)」になるので、名前の先頭で探す。
        expect(steps[first]).toMatchObject({ controlType: "TabItem", namePrefix: true });
        const body = steps.slice(first + 1, stop);
        expect(body[0]).toMatchObject({ kind: "wait" });
        expect(body[1]).toMatchObject({ kind: "dialog-click", listOnly: true, label: INCOMING_VIDEO_LABEL, buttonTextId: null });
        expect(body[2]).toMatchObject({ kind: "screenshot-window", shotLabel: INCOMING_VIDEO_LABEL });
        expect(body[3]).toMatchObject({ kind: "dialog-click", buttonTextId: TEXT_ID_TAB_PEOPLE, controlType: "TabItem" });
        expect(body).toHaveLength(4);
    });

    it("定常状態の待ちは 1 つのまま (判定は先頭と最後の stats の差)。オプションがない場合にはない", () => {
        const waits = steps.filter((s) => s.kind === "wait" && s.ms === 20 * 60 * 1000);
        expect(waits).toHaveLength(1);
        const stats = steps.indexOf(steps.find((s) => s.kind === "obs-stats" && s.label === "last")!);
        expect(steps.indexOf(waits[0])).toBeLessThan(stats);
        const without = plan([...REAL, "--broadcaster-id", "1"]).steps;
        expect(without.some((s) => s.kind === "dialog-click" && s.buttonTextId === TEXT_ID_TAB_STATUS)).toBe(false);
    });

    it("押すステップは全部 readOnlyOp (変更しない操作) で、画面のテキストは ini の両方にある", () => {
        const first = at(steps, (s) => s.kind === "dialog-click" && s.buttonTextId === TEXT_ID_TAB_STATUS);
        const stop = at(steps, (s) => s.kind === "record-stop" && s.label === "main");
        const pressed = steps.slice(first, stop).filter((s) => s.kind === "dialog-click" && !s.listOnly);
        expect(pressed).toHaveLength(2);
        expect(pressed.every((s) => s.readOnlyOp === true)).toBe(true);
        // バージョンの表示や名前ラベルの設定ウィンドウの操作 (録画の前) にはフラグを付けない。フラグを付けるのは、タブを選ぶだけの手順
        // (名前ラベルの設定の後に [参加者] タブへ戻す手順と、行を押す前に [参加者] タブを開く手順) だけ。
        const flagged = steps.slice(0, first).filter((s) => s.readOnlyOp === true);
        expect(flagged.length).toBeGreaterThan(0);
        expect(flagged.every((s) => s.kind === "dialog-click" && s.controlType === "TabItem" && s.buttonTextId === "cordmux.dock.tab_people.label")).toBe(true);
        for (const file of ["ja-JP.ini", "en-US.ini"]) expect(iniKeys(file).has(TEXT_ID_TAB_STATUS)).toBe(true);
    });

    it("操作の数: readOnlyOp の押下は配信中でも数えず、フラグのない押下は数える。実行スクリプトはフラグを結果へコピーする", () => {
        const results = (readOnly: boolean) => [
            { name: "record-start" },
            { name: "dialog-click", pressed: true, ...(readOnly ? { readOnlyOp: true } : {}) },
            { name: "dialog-click", pressed: true, ...(readOnly ? { readOnlyOp: true } : {}) },
            { name: "dock-click", muteOp: true },
            { name: "stream-stop" },
        ];
        expect(judgeOperationCount(results(true))).toMatchObject({ ok: true, counts: { other: 0, mute: 1 }, duringCount: 1 });
        expect(judgeOperationCount(results(false))).toMatchObject({ ok: false, counts: { other: 2 } });
        const source = text("run-window.mjs");
        const click = source.slice(source.indexOf('case "dialog-click": {'), source.indexOf('case "capture-file": {'));
        expect(click).toContain("readOnlyOp: true");
        expect(click).toContain("s.readOnlyOp === true");
    });
});

describe("実行全体", () => {
    it("実際の通話: 見込みは 95 分より短く、期限に足りる。ステップの種類はどれも実行スクリプトが処理し、ini にないテキストを引かない", () => {
        const { args, steps } = plan([...REAL, ...STAGE]);
        expect(steps.length).toBeGreaterThan(240);
        const source = text("run-window.mjs");
        const handled = new Set([...source.slice(source.indexOf("const HANDLED_KINDS = ["), source.indexOf("];", source.indexOf("const HANDLED_KINDS = ["))).matchAll(/"([a-z0-9-]+)"/g)].map((m) => m[1]));
        expect([...new Set(steps.map((s) => s.kind))].filter((k) => !handled.has(k))).toEqual([]);
        const ja = iniKeys("ja-JP.ini");
        const en = iniKeys("en-US.ini");
        for (const id of new Set(steps.flatMap((s) => stepTextIds(s))).values()) {
            if (id.startsWith("obs:")) continue;
            expect(ja.has(id) && en.has(id), id).toBe(true);
        }
        expect(args.extraChecks).toBe(true);
    });
});
