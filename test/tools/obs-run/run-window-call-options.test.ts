/*
 * 実際の通話の再確認用に call-1 へ足したオプション (CALL_ONE_OPTION_FLAGS) の実行計画と、--extra-checks の純粋関数をテストする。
 *   - オプションはどれも既定はなしで、指定しなければ標準の実行計画のままになる (標準の実行計画は run-window-plan.test.ts の call-1 の節がテストする)。
 *   - --start-with-add: 開始ゲートを実行せず、call-1 の先頭で Vesktop の終了と [Vesktopに追加] -> Vesktop の起動 -> 増えた接続を待つ (call-hold の先頭と同じ並び)。
 *   - --vesktop-share / --skip-continuous / --skip-av2 / --skip-speaker-mute: 参加のメッセージと、省くステップ。
 *   - --extra-checks: (a) 接続が切れていないか、(b) バージョンの表示、(c) 使う声の音、(d) cordmux の録画の参加者ごとの A/V、(e) OBS のログの
 *     整列のずれの傾き・フォールバック・アイコン・接続の数。どれも gate にしない。
 * run-window-plan.mjs は Node の API を呼ばないので、テストする対象には値だけを渡す。人のアカウントの ID は書かない (作り物の 1 だけ)。
 */
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

import {
    ADD_TO_VESKTOP_BASELINE_LABEL,
    ALIGN_DRIFT_MAX_PERCENT,
    ALIGN_DRIFT_MIN_SPAN_S,
    CALL_CONNECT_WAIT_TIMEOUT_MS,
    CALL_ONE_DEADLINE_MIN,
    CALL_ONE_HUMAN_OVERRUN_MIN,
    CALL_ONE_OPTION_FLAGS,
    deadlineEstimate,
    TONE_AUDIBLE_RMS,
    JOIN_BASELINE_LABEL,
    MAIN_BASELINE_LABEL,
    OBS_TEXT_CANCEL,
    OBS_TEXT_PREFIX,
    REHEARSAL_AV_SCREEN_ACCOUNT,
    REHEARSAL_HUMAN_WAIT_SECONDS,
    ROUNDTRIP_JUDGE_NAMES,
    STATE_JSON_BEFORE_NAME,
    TEXT_ID_ADD_TO_VESKTOP,
    TEXT_ID_DOCK_ACCOUNT_CHANGE,
    TEXT_ID_SETTINGS_PAGE_ABOUT,
    TEXT_ID_SETTINGS_VERSION_VESKTOP,
    VESKTOP_START_AFTER_ADD_MESSAGE,
    VESKTOP_START_FOR_CALL_ONE_MESSAGE,
    VOICE_SOURCE_CHECKS,
    VOICE_SOURCE_CHECK_RECORD_MS,
    VOICE_SOURCE_TONE_HZ,
    VOICE_VOLUME_AFTER_LABEL,
    alignDriftOf,
    alignStatsFromLog,
    avatarIssuesFromLog,
    callJoinMessage,
    deadlineMinFor,
    judgeCallLog,
    judgeConnectionsKept,
    judgeRoundtrip,
    judgeVersionRowShown,
    judgeVoiceSourceSwitch,
    DISPLAY_BUFFER_MS_MAX,
    parseArgs,
    pickPersonScreenOutput,
    plannedMinutes,
    planSteps,
    senderStagesFromLog,
    stepTextIds,
    voiceSourcePeople,
    DEFAULT_CALL_ACCOUNTS,
} from "./run-window-plan.mjs";

type Step = Record<string, any>;

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = join(HERE, "..", "..", "..");
const BASE = ["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-1"];
const REAL = [...BASE, "--real-profile", "--call-window", "--decoder", "software"];
const REHEARSAL = [...BASE, "--rehearsal"];
// 再確認の実際の通話での呼び出し行の形 (配信者の ID は作り物の 1。実際の ID は実行時にだけ渡す)。
const STAGE = ["--n-screen", "3", "--n-voice", "3", "--broadcaster-id", "1", "--start-with-add", "--vesktop-share", "--skip-continuous", "--skip-speaker-mute", "--extra-checks"];

function plan(argv: string[]) {
    const parsed = parseArgs(argv);
    expect(parsed.errors).toEqual([]);
    const r = planSteps(parsed.args);
    expect(r.ok).toBe(true);
    return { args: parsed.args, steps: r.steps as Step[] };
}
const kinds = (steps: Step[]) => steps.map((s) => s.kind);
const at = (steps: Step[], pred: (s: Step) => boolean) => steps.findIndex(pred);

function jaIniKeys(): Set<string> {
    const keys = new Set<string>();
    for (const line of readFileSync(join(REPO, "src-obs", "data", "locale", "ja-JP.ini"), "utf8").split(/\r?\n/)) {
        const t = line.trim();
        if (!t || t.startsWith("#") || t.indexOf("=") < 0) continue;
        keys.add(t.slice(0, t.indexOf("=")).trim());
    }
    return keys;
}

describe("call-1 のオプションの引数 (CALL_ONE_OPTION_FLAGS)", () => {
    it("9 つとも値を取らない真偽のオプションで、既定は全部なし (7 つ目の --skip-export-bundle は書き出しのバンドルの確認を省く、8 つ目の --recheck は短い再確認、9 つ目の --av-check は再確認に A/V の測定を足す)", () => {
        expect(CALL_ONE_OPTION_FLAGS).toEqual(["--start-with-add", "--vesktop-share", "--skip-continuous", "--skip-av2", "--skip-speaker-mute", "--extra-checks", "--skip-export-bundle", "--recheck", "--av-check"]);
        const keys = ["startWithAdd", "vesktopShare", "skipContinuous", "skipAv2", "skipSpeakerMute", "extraChecks", "skipExportBundle"];
        const plain = parseArgs(REAL).args;
        for (const key of [...keys, "recheck", "avCheck"]) expect(plain[key]).toBe(false);
        const all = parseArgs([...REAL, ...STAGE, "--skip-av2", "--skip-export-bundle"]);
        expect(all.errors).toEqual([]);
        for (const key of keys) expect(all.args[key]).toBe(true);
    });

    it("call-1 だけが受ける (ゲート・call-hold・テスト用の送信側だけの実行では誤り)", () => {
        for (const flag of CALL_ONE_OPTION_FLAGS.filter((f) => f !== "--skip-speaker-mute")) {
            expect(parseArgs(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-hold", "--real-profile", flag]).errors.join(" ")).toContain(flag + " は --step call-1 だけ");
            expect(parseArgs(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-gate-a", "--real-profile", flag]).ok).toBe(false);
            expect(parseArgs(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "record", flag]).ok).toBe(false);
        }
    });

    it("--skip-speaker-mute は --broadcaster-id と一緒にだけ (省くステップがない場合は誤り)", () => {
        expect(parseArgs([...REAL, "--skip-speaker-mute"]).errors.join(" ")).toContain("--skip-speaker-mute は --broadcaster-id と一緒に");
        expect(parseArgs([...REAL, "--skip-speaker-mute", "--broadcaster-id", "1"]).ok).toBe(true);
    });

    it("リハーサルも同じオプションを受ける (実際の通話と同じ呼び出し行から --real-profile と --call-window を外し --rehearsal を足すだけ)", () => {
        expect(parseArgs([...REHEARSAL, ...STAGE]).errors).toEqual([]);
    });
});

describe("--start-with-add: 開始ゲートを実行せずに call-1 の先頭で [Vesktopに追加]", () => {
    const { steps } = plan([...REAL, "--start-with-add"]);

    it("並び: コピー -> 人が Vesktop を終了して [Vesktopに追加] -> 判定 -> 保存 -> hello の数 -> 人が Vesktop を起動 -> 増えた接続を待つ -> 参加のメッセージ", () => {
        const head = steps.slice(0, at(steps, (s) => s.kind === "wire-composite"));
        const order = ["seed-config", "launch-obs", "obsws-connect", "copy-state-json", "human", "read-state-json", "judge-vencord-dir", "check-file-exists", "count-connections", "human", "wait-connection", "human"];
        expect(kinds(head).filter((k) => order.includes(k))).toEqual(order);
        const humans = head.filter((s) => s.kind === "human");
        expect(humans[0]).toMatchObject({ until: "state-json-points-to-placed", gate: true, expectedDir: "C:/obs/plugins/cordmux/data/vencord" });
        expect(humans[0].message).toContain("{{" + TEXT_ID_ADD_TO_VESKTOP + "}}");
        expect(humans[0].message).toContain("[Quit]");
        expect(humans[1]).toMatchObject({ until: "go-file", message: VESKTOP_START_AFTER_ADD_MESSAGE });
        expect(humans[2].message).toBe(callJoinMessage());
        expect(head.find((s) => s.kind === "copy-state-json")).toMatchObject({ destName: STATE_JSON_BEFORE_NAME });
    });

    it("接続の待ちは保存した数より増えたときだけ通り (gate)、上限は標準の実行計画と同じ 20 分", () => {
        const count = steps.find((s) => s.kind === "count-connections")!;
        const wait = steps.find((s) => s.kind === "wait-connection")!;
        expect(count.label).toBe(ADD_TO_VESKTOP_BASELINE_LABEL);
        expect(wait).toMatchObject({ baseline: ADD_TO_VESKTOP_BASELINE_LABEL, timeoutMs: CALL_CONNECT_WAIT_TIMEOUT_MS, gate: true });
        expect(at(steps, (s) => s.kind === "wait-connection")).toBeGreaterThan(at(steps, (s) => s.kind === "human" && s.message === VESKTOP_START_AFTER_ADD_MESSAGE));
    });

    it("標準の実行計画の「起動してください」(開始ゲートの後の 3 つ目の依頼) は出ない。終わりの手順は同じ (Vesktop の終了 -> アンインストール -> 起動)", () => {
        expect(steps.some((s) => s.kind === "human" && s.message === VESKTOP_START_FOR_CALL_ONE_MESSAGE)).toBe(false);
        expect(plan(REAL).steps.some((s) => s.kind === "human" && s.message === VESKTOP_START_FOR_CALL_ONE_MESSAGE)).toBe(true);
        const restoredIdx = at(steps, (s) => s.kind === "judge-vencord-dir-restored");
        expect(steps[restoredIdx]).toMatchObject({ gate: true });
        expect(steps.at(-1)).toMatchObject({ kind: "cleanup", callWindow: false });
    });

    it("オプションのない場合の実行計画は、オプションを足す前と同じ (オプションは既定を変えない)。リハーサルは元から実行スクリプトが押すのでオプションで変わらない", () => {
        const plain = plan(REAL).steps;
        expect(plain.some((s) => s.kind === "copy-state-json")).toBe(false);
        expect(plan([...REHEARSAL, "--start-with-add"]).steps).toEqual(plan(REHEARSAL).steps);
    });
});

describe("--vesktop-share / --skip-continuous / --skip-av2 / --skip-speaker-mute", () => {
    it("--vesktop-share: 参加のメッセージが Vesktop の共有を依頼し、Chrome 以外のマイクのミュートを依頼する", () => {
        const join = plan([...REAL, "--vesktop-share"]).steps.find((s) => s.kind === "human" && String(s.message).includes("ボイスチャンネル"))!;
        expect(join.message).toBe(callJoinMessage({ vesktopShare: true }));
        // Vesktop も同じボイスチャンネルに入ることを依頼する (名前は --voice-channel で渡したときだけメッセージに入る)。
        expect(join.message).toContain("でも同じボイスチャンネルに入り、画面共有を1つ始めてください");
        const named = plan([...REAL, "--vesktop-share", "--voice-channel", "lobby"]).steps.find((s) => s.kind === "human" && String(s.message).includes("ボイスチャンネル"))!;
        expect(named.message).toBe(callJoinMessage({ vesktopShare: true, voiceChannel: "lobby" }));
        expect(named.message).toContain("でも「lobby」に入り、画面共有を1つ始めてください");
        expect(join.message).toContain("Chrome以外のマイクはミュートにしてください");
        expect(join.message).not.toContain("では画面共有を始めないでください");
        // 人に見せるメッセージでは、英数字と日本語の間に半角の空白を入れない。
        expect(join.message).not.toMatch(/[A-Za-z0-9] [\u3041-\u3093\u30a1-\u30f6\u4e00-\u9fa0]|[\u3041-\u3093\u30a1-\u30f6\u4e00-\u9fa0] [A-Za-z0-9]/);
    });

    it("--skip-continuous: 照合の周期・わざと異常・av2 までの待ちがない。av2 は残る", () => {
        const { steps } = plan([...REAL, "--skip-continuous"]);
        expect(steps.some((s) => s.kind === "tamper-file" || s.kind === "untamper-file")).toBe(false);
        expect(steps.some((s) => s.kind === "wait" && String(s.label).includes("照合の周期"))).toBe(false);
        expect(steps.some((s) => s.kind === "wait" && String(s.label).includes("2 回目の A/V まで"))).toBe(false);
        expect(steps.some((s) => s.kind === "record-start" && s.label === "av2")).toBe(true);
        expect(steps.find((s) => s.kind === "judge-av-drift")).toMatchObject({ first: "av1", second: "av2" });
        expect(plannedMinutes(steps)).toBeLessThan(plannedMinutes(plan(REAL).steps) - 40);
    });

    it("--skip-av2: av2 の録画と判定と av1 との差がない (リハーサルの av2 の前の送信側の確認もない)", () => {
        for (const argv of [[...REAL, "--skip-av2"], [...REHEARSAL, "--skip-av2"]]) {
            const { steps } = plan(argv);
            expect(steps.some((s) => s.label === "av2" || s.recording === "av2")).toBe(false);
            expect(steps.some((s) => s.kind === "judge-av-drift")).toBe(false);
            expect(steps.some((s) => s.kind === "harness-alive")).toBe(false);
            expect(steps.filter((s) => s.kind === "collect-recording").map((s) => s.label)).toEqual(["av1", "main", "ac35"]);
        }
    });

    it("--skip-speaker-mute: 別アカウント方式のステップは抜ける・戻るの 2 つだけ (映像の数の測定と判定がない)", () => {
        const { steps } = plan([...REAL, "--broadcaster-id", "1", "--skip-speaker-mute"]);
        const accountChange = at(steps, (s) => s.kind === "dialog-click" && s.buttonTextId === TEXT_ID_DOCK_ACCOUNT_CHANGE);
        const av1Human = at(steps, (s) => s.kind === "human" && String(s.message).includes("テスト動画"));
        const block = steps.slice(accountChange, av1Human);
        expect(block.filter((s) => s.kind === "human").map((s) => s.message)).toEqual([expect.stringContaining("抜け"), expect.stringContaining("戻して")]);
        expect(block.some((s) => s.kind === "measure-video-rate")).toBe(false);
        expect(block.filter((s) => s.kind === "judge-rt").map((s) => s.judge)).toEqual(["not-named-in-texts", "named-in-texts", "not-named-in-texts"]);
    });
});

describe("--extra-checks の実行計画", () => {
    const { steps } = plan([...REAL, ...STAGE]);
    const rehearsal = plan([...REHEARSAL, ...STAGE]).steps;

    it("(a) 参加のメッセージの直前に接続の数を保存して共有の後 (枠の一覧の直後) に比べ、保存し直した数とメインの手順の後 (合成の確認の前) に比べる。どれも gate にしない", () => {
        for (const list of [steps, rehearsal]) {
            const count = at(list, (s) => s.kind === "count-connections" && s.label === JOIN_BASELINE_LABEL);
            expect(count).toBeGreaterThan(0);
            expect(list[count + 1]).toMatchObject({ kind: "human" });
            const afterJoin = at(list, (s) => s.kind === "judge-connections-kept" && s.label === "after-join");
            expect(list[afterJoin - 1]).toMatchObject({ kind: "composite-layout-snapshot", label: "slot-items" });
            expect(list[afterJoin]).toMatchObject({ baseline: JOIN_BASELINE_LABEL });
            // 共有でクラッシュして起動し直した場合でも、メインの手順の間に切れたかを分けて読めるように保存し直す。
            expect(list[afterJoin + 1]).toMatchObject({ kind: "count-connections", label: MAIN_BASELINE_LABEL });
            const afterMain = at(list, (s) => s.kind === "judge-connections-kept" && s.label === "after-main");
            expect(list[afterMain]).toMatchObject({ baseline: MAIN_BASELINE_LABEL });
            expect(list[afterMain - 1]).toMatchObject({ kind: "record-stop", label: "main" });
            expect(list[afterMain + 1]).toMatchObject({ kind: "composite-layout-snapshot", label: "ac30-before" });
            expect(afterJoin + 1).toBeLessThan(at(list, (s) => s.kind === "record-start" && s.label === "main"));
            for (const s of list.filter((x) => x.kind === "judge-connections-kept")) expect(s.gate).toBeUndefined();
        }
    });

    it("(b) バージョンの表示は名前ラベルの確認の後・配信の入力の前に読み、読むだけなので [キャンセル] で閉じる", () => {
        const page = at(steps, (s) => s.kind === "dialog-click" && s.controlType === "ListItem" && s.buttonTextId === TEXT_ID_SETTINGS_PAGE_ABOUT);
        const read = at(steps, (s) => s.kind === "dialog-click" && s.label === "about-versions");
        expect(page).toBeGreaterThan(at(steps, (s) => s.kind === "screenshot-source" && s.label === "ac31-after"));
        expect(read).toBe(page + 1);
        expect(steps[read]).toMatchObject({ listOnly: true, titleTextId: "cordmux.settings.window.title", buttonTextId: null });
        expect(steps[read + 1]).toMatchObject({ kind: "screenshot-window", shotLabel: "about-versions" });
        expect(steps[read + 2]).toMatchObject({ kind: "judge-rt", judge: "version-row-shown", inputs: { texts: { dialog: "about-versions" }, names: { textPrefix: TEXT_ID_SETTINGS_VERSION_VESKTOP } } });
        expect(steps[read + 2].gate).toBeUndefined();
        expect(steps[read + 3]).toMatchObject({ kind: "dialog-click", buttonTextId: OBS_TEXT_CANCEL });
        expect(read + 3).toBeLessThan(at(steps, (s) => s.kind === "stream-sink-start"));
        expect(ROUNDTRIP_JUDGE_NAMES).toContain("version-row-shown");
    });

    it("(c) 使う声: 置き換えの後に所有者・声を貸す人の順で 1 つずつ、戻した後にも同じ 2 つ (枠を 1 つだけ鳴らして録り、戻す)", () => {
        const ask = at(steps, (s) => s.kind === "human" && String(s.message).includes(DEFAULT_CALL_ACCOUNTS.chrome + "」を選んで"));
        const back = at(steps, (s) => s.kind === "human" && String(s.message).includes("元に戻して"));
        expect(ask).toBeGreaterThan(0);
        expect(back).toBeGreaterThan(ask);
        // 録画ごとに、その前でドックの行の声のミュートを (残す人だけ外し、ほかをミュートに) 押し、録画の後に全員のミュートを外す。
        // 声の音量の録画 (VOICE_VOLUME_AFTER_LABEL) は、間に人の操作が挟まる別の形なので run-window-real-call-checks.test.ts がテストする。
        const recs = steps.filter((s) => s.kind === "record-start" && String(s.label).startsWith("vs-") && s.label !== VOICE_VOLUME_AFTER_LABEL);
        const everyone = ["sub2", "sub3"];
        const keeps: Record<string, string> = { "vs-switched-owner": DEFAULT_CALL_ACCOUNTS.firefox, "vs-switched-from": DEFAULT_CALL_ACCOUNTS.chrome, "vs-restored-owner": DEFAULT_CALL_ACCOUNTS.firefox, "vs-restored-from": DEFAULT_CALL_ACCOUNTS.chrome };
        expect(recs.map((s) => s.label)).toEqual(Object.keys(keeps));
        for (const rec of recs) {
            const i = steps.indexOf(rec);
            const before = steps.slice(i - everyone.length, i);
            expect(before.every((s) => s.kind === "dock-click" && s.muteOp === true && s.textId === "cordmux.dock.row_voice_mute.a11y")).toBe(true);
            expect(before.map((s) => [s.textArgs[0], s.desired])).toEqual(everyone.map((p) => [p, p === keeps[rec.label] ? "off" : "on"]));
            expect(kinds(steps.slice(i, i + 3))).toEqual(["record-start", "wait", "record-stop"]);
            expect(steps[i + 1]).toMatchObject({ ms: VOICE_SOURCE_CHECK_RECORD_MS });
            expect(steps[i + 2]).toMatchObject({ label: rec.label, stream: false, tailSeconds: 0 });
            const after = steps.slice(i + 3, i + 3 + everyone.length);
            expect(after.map((s) => [s.textArgs[0], s.desired])).toEqual(everyone.map((p) => [p, "off"]));
        }
        const isolations = recs;
        expect(steps.indexOf(isolations[0])).toBeGreaterThan(ask);
        expect(steps.indexOf(isolations[1])).toBeLessThan(back);
        expect(steps.indexOf(isolations[2])).toBeGreaterThan(back);
        // リハーサルはテスト用の送信側の 1 つ目と 2 つ目の声 (置き換わらないので判定は記録だけ)。テスト用の送信側は声のストリームに
        // 画面のストリームの後の番号を付ける (assignStreamIds) ので、画面 3 ストリームなら harness-voice-4 と 5、--n-screen が無ければ (画面 4 ストリーム) 5 と 6。
        expect(voiceSourcePeople({ rehearsal: true })).toEqual({ owner: "harness-voice-5", from: "harness-voice-6" });
        expect(voiceSourcePeople({ rehearsal: true, nScreenGiven: true, nScreen: 3 })).toEqual({ owner: "harness-voice-4", from: "harness-voice-5" });
        const rehearsalRecs = rehearsal.filter((s) => s.kind === "record-start" && String(s.label).startsWith("vs-") && s.label !== VOICE_VOLUME_AFTER_LABEL);
        const unmuted = rehearsalRecs.map((r) => rehearsal.slice(rehearsal.indexOf(r) - 3, rehearsal.indexOf(r)).filter((s) => s.desired === "off").map((s) => s.textArgs[0]));
        expect(unmuted).toEqual([["harness-voice-4"], ["harness-voice-5"], ["harness-voice-4"], ["harness-voice-5"]]);
        // 録画の label は重ならない (record-start と record-stop が対)。
        const starts = steps.filter((s) => s.kind === "record-start").map((s) => s.label);
        expect(new Set(starts).size).toBe(starts.length);
        expect(steps.filter((s) => s.kind === "record-stop").map((s) => s.label)).toEqual(starts);
    });

    it("(c)(d)(e) の判定は OBS を閉じた後 (録画を読むだけ)、OBS の録画を残す前。どれも gate にしない", () => {
        for (const [list, person] of [[steps, DEFAULT_CALL_ACCOUNTS.chrome], [rehearsal, REHEARSAL_AV_SCREEN_ACCOUNT]] as const) {
            const close = list.map((s) => s.kind).lastIndexOf("close-obs");
            const keep = at(list, (s) => s.kind === "keep-obs-recordings");
            for (const kind of ["judge-record-sync", "judge-voice-source", "judge-call-log"]) {
                const i = at(list, (s) => s.kind === kind);
                expect(i, kind).toBeGreaterThan(close);
                expect(i, kind).toBeLessThan(keep);
                expect(list[i].gate, kind).toBeUndefined();
            }
            expect(list.find((s) => s.kind === "judge-record-sync")).toMatchObject({ person, recordDirRel: "recordings" });
            expect(list.find((s) => s.kind === "judge-voice-source")).toMatchObject({ hz: VOICE_SOURCE_TONE_HZ, checks: VOICE_SOURCE_CHECKS.map((c) => ({ label: c.label, expectTone: c.expectTone })) });
            expect(list.find((s) => s.kind === "judge-call-log")).toMatchObject({ maxDriftPercent: ALIGN_DRIFT_MAX_PERCENT, minSpanSeconds: ALIGN_DRIFT_MIN_SPAN_S });
        }
    });

    it("オプションが無ければ (a)-(e) のステップは 1 つもない", () => {
        const plain = plan([...REAL, "--broadcaster-id", "1"]).steps;
        for (const kind of ["judge-connections-kept", "judge-record-sync", "judge-voice-source", "judge-call-log"]) expect(plain.some((s) => s.kind === kind)).toBe(false);
        expect(plain.some((s) => s.kind === "record-start" && String(s.label).startsWith("vs-"))).toBe(false);
        expect(plain.some((s) => s.kind === "dialog-click" && s.label === "about-versions")).toBe(false);
    });

    it("指定する画面のテキストは製品の ja-JP.ini にあり、OBS 自身のテキストは OK / キャンセル / 適用 / ツールだけ", () => {
        const keys = jaIniKeys();
        expect(keys.has(TEXT_ID_SETTINGS_VERSION_VESKTOP)).toBe(true);
        for (const list of [steps, rehearsal]) {
            for (const s of list) {
                for (const id of stepTextIds(s)) {
                    if (id.startsWith(OBS_TEXT_PREFIX)) expect(["obs:OK", "obs:Cancel", "obs:Apply", "obs:Basic.MainMenu.Tools"]).toContain(id);
                    else expect(keys.has(id), id).toBe(true);
                }
                if (s.kind === "judge-rt") expect(ROUNDTRIP_JUDGE_NAMES).toContain(s.judge);
            }
        }
    });

    it("実行スクリプトの switch がどのステップの種類も持つ (run-window.mjs をテキストとして読む)", () => {
        const source = readFileSync(join(HERE, "run-window.mjs"), "utf8");
        const caseKinds = new Set([...source.matchAll(/^\s+case "([a-z0-9-]+)":/gm)].map((m) => m[1]));
        const handledBlock = source.slice(source.indexOf("const HANDLED_KINDS = ["), source.indexOf("];", source.indexOf("const HANDLED_KINDS = [")));
        const handled = new Set([...handledBlock.matchAll(/"([a-z0-9-]+)"/g)].map((m) => m[1]));
        const used = new Set([...steps, ...rehearsal, ...plan([...REAL, "--start-with-add"]).steps].map((s) => s.kind));
        expect([...used].filter((k) => !caseKinds.has(k) || !handled.has(k))).toEqual([]);
    });

    it("再確認の実際の通話: 見込み (待ちと人の操作の見込みの和) は 60 から 95 分 (連続の確認とスピーカーミュートを省き、av2 はメインの手順から 90 分あけない)、期限は見積もり (人の遅れ 90 分を含む) と 180 分の大きいほう", () => {
        const { args, steps: s } = plan([...REAL, ...STAGE]);
        const minutes = plannedMinutes(s);
        expect(minutes).toBeGreaterThan(60);
        expect(minutes).toBeLessThan(95);
        // 標準の実行計画 (連続の確認と 90 分の間あけを含む) より 35 分以上短い (声の音量・実際のアバターの並び・映像のバッファの表の
        // 3 つの確認で約 5 分を足した後も、省いた分が大きい)。
        expect(minutes).toBeLessThan(plannedMinutes(plan([...REAL, "--broadcaster-id", "1"]).steps) - 35);
        expect(deadlineMinFor(args, s)).toBe(Math.max(CALL_ONE_DEADLINE_MIN, deadlineEstimate(s).minutes));
        // 人の遅れを 90 分見込む (人がすぐ操作できるとは限らない)。期限は見込みに 90 分を足した長さより長い。
        expect(CALL_ONE_HUMAN_OVERRUN_MIN).toBe(90);
        expect(deadlineMinFor(args, s)).toBeGreaterThan(minutes + CALL_ONE_HUMAN_OVERRUN_MIN);
        // 人に依頼する操作は 14 個 ([Vesktopに追加]・終了と起動・参加・抜ける・戻る・テスト動画 x2・置き換え・音量・戻す・共有を止める・終わりの手順 3 つ)。
        expect(s.filter((x) => x.kind === "human")).toHaveLength(14);
    });

    it("再確認のリハーサル: 人の操作は全部秒数で進み、テスト用の送信側の動作時間は見込みの 2 倍以上", () => {
        for (const h of rehearsal.filter((s) => s.kind === "human")) expect(h).toMatchObject({ until: "seconds", waitSeconds: REHEARSAL_HUMAN_WAIT_SECONDS });
        const harness = rehearsal.find((s) => s.kind === "harness-start")!;
        expect(harness).toMatchObject({ nScreen: 3, nVoice: 3 });
        expect(harness.seconds).toBeGreaterThanOrEqual(plannedMinutes(rehearsal) * 60 * 2);
        expect(plannedMinutes(rehearsal)).toBeLessThan(20);
    });
});

describe("--extra-checks の純粋関数", () => {
    it("(a) judgeConnectionsKept: 保存した数から close も hello も増えていなければ合格。保存した数が無ければ失敗", () => {
        expect(judgeConnectionsKept({ before: { hello: 2, close: 1 }, now: { hello: 2, close: 1 } })).toMatchObject({ ok: true, closes: 0, hellos: 0 });
        expect(judgeConnectionsKept({ before: { hello: 2, close: 1 }, now: { hello: 3, close: 2 } })).toMatchObject({ ok: false, closes: 1, hellos: 1 });
        expect(judgeConnectionsKept({ before: { hello: 2, close: 1 }, now: { hello: 3, close: 1 } }).reason).toContain("connected again");
        expect(judgeConnectionsKept({ before: null, now: { hello: 1, close: 0 } }).ok).toBe(false);
        expect(judgeConnectionsKept({ before: { hello: 1, close: 0 }, now: null }).ok).toBe(false);
    });

    it("(b) judgeVersionRowShown: 行の名前と同じテキストの次のテキストが値。「-」・空・数字のない値は失敗", () => {
        const texts = ["バージョン", "cordmux (OBS)", "0.1.0+dev", "cordmux (Vesktop)", "0.1.0+dev", "Vesktop", "1.6.7", "同梱のVencord", "v1.15.11"];
        expect(judgeVersionRowShown({ texts, names: ["Vesktop"] })).toMatchObject({ ok: true, value: "1.6.7" });
        expect(judgeVersionRowShown({ texts: ["Vesktop", "-"], names: ["Vesktop"] })).toMatchObject({ ok: false, value: "-" });
        expect(judgeVersionRowShown({ texts: ["Vesktop"], names: ["Vesktop"] })).toMatchObject({ ok: false, value: "" });
        expect(judgeVersionRowShown({ texts: ["cordmux (Vesktop)", "0.1.0"], names: ["Vesktop"] }).ok).toBe(false);
        expect(judgeVersionRowShown({ texts: [], names: ["Vesktop"] }).ok).toBe(false);
        expect(judgeRoundtrip("version-row-shown", { texts, names: ["Vesktop"] }).ok).toBe(true);
    });

    it("(c) judgeVoiceSourceSwitch: 置き換えの後に所有者の枠で 1 kHz が聞こえ、ほかが期待どおりなら合格", () => {
        const loud = { rms: 0.2, toneShare: 0.98 };
        const quiet = { rms: 0.0, toneShare: 0.0 };
        const make = (values: Array<Record<string, any>>) => VOICE_SOURCE_CHECKS.map((c, i) => ({ label: c.label, expectTone: c.expectTone, targetFound: true, ...values[i] }));
        expect(judgeVoiceSourceSwitch({ checks: make([loud, quiet, quiet, loud]) }).ok).toBe(true);
        // 戻した後に所有者の声の枠がない (所有者のマイクがミュートで声のストリームが来ない) のは判定できない = 成功のまま。
        const restoredOwnerGone = make([loud, quiet, quiet, loud]);
        restoredOwnerGone[2] = { ...restoredOwnerGone[2], targetFound: false };
        const gone = judgeVoiceSourceSwitch({ checks: restoredOwnerGone });
        expect(gone.ok).toBe(true);
        expect(gone.parts[2]).toMatchObject({ heard: null, matches: null });
        // 置き換えても所有者の枠が鳴らない = 失敗。
        expect(judgeVoiceSourceSwitch({ checks: make([quiet, quiet, quiet, loud]) })).toMatchObject({ ok: false, reason: "not as expected: vs-switched-owner" });
        // 置き換えの間も声を貸す人の枠が鳴る (二重に出る) = 失敗。
        expect(judgeVoiceSourceSwitch({ checks: make([loud, loud, quiet, loud]) }).ok).toBe(false);
        // 所有者の枠が見つからない = 判定の基準を決められないので失敗。
        const ownerMissing = make([loud, quiet, quiet, loud]);
        ownerMissing[0] = { ...ownerMissing[0], targetFound: false };
        expect(judgeVoiceSourceSwitch({ checks: ownerMissing }).ok).toBe(false);
        // 鳴っているとも鳴っていないとも言えない間 (1 kHz の成分が 3 割) は判定できない。
        const between = judgeVoiceSourceSwitch({ checks: make([{ rms: TONE_AUDIBLE_RMS * 5, toneShare: 0.3 }, quiet, quiet, loud]) });
        expect(between.ok).toBe(false);
        expect(between.parts[0]).toMatchObject({ heard: null });
    });

    it("(d) pickPersonScreenOutput: person の行の表示名から user_id を引き、その画面の完成ファイルを返す。名簿のないテスト用の送信側は user_id で引く", () => {
        const rows = {
            person: [{ user_id: "11", display_name: "person-a" }, { user_id: "22", display_name: "other" }],
            finalize: [
                { user_id: "11", kind: "voice", output_file: "person-a_voice.mov" },
                { user_id: "11", kind: "screen", output_file: "person-a_screen.mov" },
                { user_id: "22", kind: "screen", output_file: "other_screen.mov" },
                { user_id: "harness-screen-1", kind: "screen", output_file: "harness-screen-1_screen.mov" },
            ],
        };
        expect(pickPersonScreenOutput({ rows, person: "person-a" })).toEqual({ file: "person-a_screen.mov", reason: null });
        expect(pickPersonScreenOutput({ rows, person: "harness-screen-1" })).toEqual({ file: "harness-screen-1_screen.mov", reason: null });
        expect(pickPersonScreenOutput({ rows, person: "nobody" }).file).toBeNull();
        expect(pickPersonScreenOutput({ rows: null, person: "x" }).file).toBeNull();
    });

    // OBS のログの行のテストデータ (製品の書式どおり。秒は 10 秒ごと)。
    function statsLine(t: number, stream: number, frames: number, offsetUs: number, periodUs = 16667, realign = 0) {
        const hh = String(Math.floor(t / 3600) % 24).padStart(2, "0");
        const mm = String(Math.floor(t / 60) % 60).padStart(2, "0");
        const ss = String(Math.floor(t) % 60).padStart(2, "0");
        return `${hh}:${mm}:${ss}.000: [cordmux] Stats stream=${stream} align period_us=${periodUs.toFixed(1)} frames=${frames} realign=${realign} relock=0 jump=0 wobble=0 backward=0 mono_fix=0 slew_capped=0 excuse_capped=0 offset_us=${offsetUs} err_abs_max_us=0 av video_offset_us=${offsetUs} offset_changes=0 audio=0 passthrough=0`;
    }

    it("(e) alignDriftOf / judgeCallLog: Firefox の形 (1% で溜まる) は上限の外、平らなストリームは内。短いストリームは傾きを判定しない", () => {
        const start = 13 * 3600;
        const lines: string[] = [];
        for (let i = 0; i <= 120; i += 1) {
            const t = start + i * 10;
            lines.push(statsLine(t, 4, i * 600, 20000 + (i % 3) * 300)); // 平ら (揺れだけ)
            lines.push(statsLine(t, 6, i * 140, i * 10 * 10000, 71000)); // 1% (10 秒で 100 ms)
            if (i < 30) lines.push(statsLine(t, 7, i * 600, 0)); // 5 分だけのストリーム
        }
        const byStream = alignStatsFromLog(lines);
        expect([...byStream.keys()].sort()).toEqual([4, 6, 7]);
        const flat = alignDriftOf(byStream.get(4));
        expect(flat).toMatchObject({ judged: true, ok: true, spanSeconds: 1200 });
        expect(Math.abs(flat.slopePercent)).toBeLessThan(ALIGN_DRIFT_MAX_PERCENT);
        const firefox = alignDriftOf(byStream.get(6));
        expect(firefox).toMatchObject({ judged: true, ok: false, medianPeriodMs: 71 });
        expect(firefox.slopePercent).toBeCloseTo(1, 3);
        expect(firefox.slopeMsPerMinute).toBeCloseTo(600, 1);
        expect(alignDriftOf(byStream.get(7))).toMatchObject({ judged: false, ok: null });
        const judged = judgeCallLog(lines);
        expect(judged.ok).toBe(false);
        expect(judged.reason).toContain("stream 6");
        expect(judgeCallLog(lines.filter((l) => !l.includes("stream=6 "))).ok).toBe(true);
        expect(judgeCallLog([]).ok).toBe(false);
    });

    it("(e) 共有が止まっている間 (frames が進まない行) は傾きに入れない。日付をまたいでも時刻が戻らない", () => {
        const lines: string[] = [];
        const start = 23 * 3600 + 55 * 60;
        for (let i = 0; i <= 80; i += 1) {
            const frames = i < 40 ? i * 600 : 40 * 600; // 40 行目から止まる
            const offset = i < 40 ? 5000 : 5000 + (i - 40) * 50000; // 止まっている間の値は溜まって見える
            lines.push(statsLine(start + i * 10, 2, frames, offset));
        }
        const samples = alignStatsFromLog(lines).get(2);
        expect(samples.at(-1).t).toBeGreaterThan(samples[0].t);
        const d = alignDriftOf(samples);
        // 止まった後の行は 1 つ目 (止まる前の最後の 1 フレームぶん進んだ行) だけが入る。
        expect(d.samples).toBe(41);
        expect(Math.abs(d.slopePercent)).toBeLessThan(0.001);
    });

    it("(e) senderStagesFromLog と avatarIssuesFromLog: フォールバックの段階をストリームごとに、アイコンを送らなかった行を理由ごとに数え、人の ID は出さない", () => {
        const lines = [
            "13:43:53.404: [cordmux/js] 2000-01-01T04:43:48.996Z video stream 4: stage=1 format=1920x1080@31",
            "13:43:53.404: [cordmux/js] 2000-01-01T04:43:49.746Z video stream 6: stage=1 format=1272x720@13",
            "13:50:00.000: [cordmux/js] 2000-01-01T04:50:00.000Z video stream 6: stage=2 format=1272x720@7",
            "13:48:42.645: [cordmux/js] 2000-01-01T04:48:42.516Z avatar (userId=1111): 送らない (bad_url)",
            "13:48:42.646: [cordmux/js] 2000-01-01T04:48:42.517Z avatar (userId=2222): 送らない (bad_url)",
            "13:48:42.647: [cordmux/js] 2000-01-01T04:48:42.518Z avatar (userId=2222): 送らない (bad_url)",
            // 受信側の M は接続の間に捨てた数の合計で、切れると 0 に戻る。1 つ目の接続で 2 つ、2 つ目の接続で 1 つ捨てた。
            "13:48:42.510: [cordmux] Profiles: people=4 avatar_decode_failures=0",
            "13:48:42.512: [cordmux] Avatar: stored=no people_with_avatar=3 avatar_decode_failures=1",
            "13:48:42.516: [cordmux] Avatar: stored=no people_with_avatar=3 avatar_decode_failures=2",
            "13:48:43.000: [cordmux] Profiles: people=4 avatar_decode_failures=2",
            "13:48:50.000: [cordmux] Disconnected from Vesktop: it stopped responding",
            "13:48:50.001: [cordmux] Disconnected from Vesktop; cleared screen share decoding and routes",
            "13:48:55.000: [cordmux] Profiles: people=4 avatar_decode_failures=0",
            "13:48:55.010: [cordmux] Avatar: stored=no people_with_avatar=3 avatar_decode_failures=1",
            "13:48:20.000: [cordmux] hello: 合言葉が一致した。接続を確立した",
            "13:48:32.274: [cordmux] close: 接続が切れた",
        ];
        expect(senderStagesFromLog(lines)).toEqual([
            { stream: 4, stages: [1], lastFormat: "1920x1080@31", maxStage: 1, degraded: false },
            { stream: 6, stages: [1, 2], lastFormat: "1272x720@7", maxStage: 2, degraded: true },
        ]);
        const avatars = avatarIssuesFromLog(lines);
        expect(avatars).toEqual({ notSent: [{ reason: "bad_url", lines: 3, people: 2 }], decodeFailures: 3 });
        // 先頭が小文字の行 (現在の受信側は書かない形) は読まない。
        expect(avatarIssuesFromLog(["13:48:42.510: [cordmux] profiles: people=4 avatar_decode_failures=1"]).decodeFailures).toBe(0);
        expect(JSON.stringify(avatars)).not.toContain("1111");
        const judged = judgeCallLog(lines);
        expect(judged.connections).toEqual({ helloEstablishedCount: 1, closeCount: 1 });
        expect(JSON.stringify(judged)).not.toContain("2222");
    });
});

describe("表示の緩衝 (--display-buffer-ms。製品の display_buffer_ms)", () => {
    const seedOf = (steps: Step[]) => steps.find((s) => s.kind === "seed-config") as Step;

    it("渡せば seed-config のステップに値が載り、渡さなければ null (製品の既定のまま書かない)", () => {
        expect(seedOf(plan([...REHEARSAL, "--display-buffer-ms", "150"]).steps).displayBufferMs).toBe(150);
        expect(seedOf(plan([...REHEARSAL, "--display-buffer-ms", "0"]).steps).displayBufferMs).toBe(0);
        expect(seedOf(plan(REHEARSAL).steps).displayBufferMs).toBeNull();
    });

    it("範囲は製品の clamp と同じ 0 から 1000 で、外れた値と整数でない値は誤り", () => {
        expect(DISPLAY_BUFFER_MS_MAX).toBe(1000);
        expect(parseArgs([...REHEARSAL, "--display-buffer-ms", "1000"]).errors).toEqual([]);
        expect(parseArgs([...REHEARSAL, "--display-buffer-ms", "1001"]).errors.join(" ")).toContain("--display-buffer-ms は 0 から 1000");
        expect(parseArgs([...REHEARSAL, "--display-buffer-ms", "12.5"]).errors.join(" ")).toContain("--display-buffer-ms は 0 以上の整数");
        expect(parseArgs([...REHEARSAL, "--display-buffer-ms", "-5"]).errors.length).toBeGreaterThan(0);
    });
});
