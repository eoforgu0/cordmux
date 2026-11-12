/*
 * 実際の通話の再確認の短い実行 (--recheck)。送信側が自分のアカウントを届ける動き (ログインの後に送り直す) と、
 * 時間切れで飛んだ確認 (映像の遅れ・配信者が抜けたときのドックのメッセージ・実際のアバターの並び) に加えて、
 * バージョンの表示・画面共有の音・共有の途中の解像度の変化・エンコードの設定・自分の枠・スピーカーミュートと最小化・
 * 受信側の Vesktop の抜け戻りを、実際の通話でしか答えられない物だけで実行する。
 * 手順の並び (含む物と外す物)、新しい判定 (Facts の行の self=yes の読み・ログの行の集計・入力の音量メーター)、
 * judge-lines の表の絞り方、新しい手順の種類とツールの拡張、dry-run の形を確認する。
 * run-window-plan.mjs は Node の API を呼ばないので、確認の対象には値だけを渡す。人のアカウントの ID は書かない (偽の 1 だけ)。
 */
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

import {
    ADD_TO_VESKTOP_BASELINE_LABEL,
    CALL_CONNECT_WAIT_TIMEOUT_MS,
    CALL_CONTACT_LOST_GUIDANCE,
    CALL_ONE_HUMAN_OVERRUN_MIN,
    CALL_TEXT_IDS,
    INPUT_LEVEL_THRESHOLD_DBFS,
    JUDGE_LINES,
    LATENCY_RECORDING_LABEL,
    LOG_PATTERN_CANVAS_COUNT,
    LOG_PATTERN_LIMITS_SENT,
    LOG_PATTERN_SCREEN_AUDIO_PLACED,
    LOG_PATTERN_SENDER_CHOICE,
    OBS_TEXT_APPLY,
    RECHECK_JUDGE_ITEMS,
    RESCHANGE_BROWSER_PROCESS,
    RESCHANGE_MAX_FREEZE_MS,
    RESCHANGE_RECORDING_LABEL,
    RESCHANGE_SMALL_HEIGHT,
    RESCHANGE_SMALL_WIDTH,
    ROSTER_BASELINE_LABEL,
    SELF_ACCOUNT_POLL_MS,
    SELF_ACCOUNT_WAIT_TIMEOUT_MS,
    LOG_PATTERN_SELF_OFF,
    LOG_PATTERN_SELF_OFF_ANY,
    LOG_PATTERN_SELF_ON,
    LOG_PATTERN_SELF_ON_ANY,
    SELF_FRAME_SOURCE_NAME,
    TEXT_ID_ROW_CHECK,
    TEXT_ID_DOCK_ACCOUNT_CHANGE,
    TEXT_ID_DOCK_TAB_DISPLAY,
    TEXT_ID_LOOK_SELF_FRAME_SOURCE,
    TEXT_ID_SETTINGS_ENCODE_PRIORITY,
    TEXT_ID_SETTINGS_ENCODE_PRIORITY_QUALITY,
    TEXT_ID_SETTINGS_PAGE_INCOMING_VIDEO,
    TEXT_ID_SETTINGS_VIDEO_ENCODER,
    TEXT_ID_SETTINGS_VIDEO_ENCODER_SOFTWARE,
    VESKTOP_LEAVE_AND_QUIT_MESSAGE,
    VESKTOP_START_AFTER_ADD_MESSAGE,
    VESKTOP_STOP_SHARING_MESSAGE,
    callJoinMessage,
    deadlineEstimate,
    deadlineMinFor,
    encodeRestartShareMessage,
    inputPeakFromMeterEvents,
    judgeInputLevel,
    judgeLinesSummary,
    judgeLogLines,
    judgeRosterLabels,
    judgeSelfAccount,
    keySendingSteps,
    parseArgs,
    plannedMinutes,
    planSteps,
    resolutionChangePlanned,
    rosterCheckPlanned,
    selfFrameShareStartMessage,
    selfFrameShareStopMessage,
    speakerMuteMinimizeMessage,
    speakerUnmuteLeaveMessage,
    stepTextIds,
    vesktopRejoinMessage,
    DEFAULT_CALL_ACCOUNTS,
    vesktopStartAfterAddMessage,
} from "./run-window-plan.mjs";

type Step = Record<string, any>;

const HERE = dirname(fileURLToPath(import.meta.url));
const BASE = ["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-1"];
const REAL = [...BASE, "--real-profile", "--call-window", "--decoder", "software"];
const REHEARSAL = [...BASE, "--rehearsal"];
// 呼び出し行の形 (配信者の ID と遅れを読む人の名前は作り物)。
const RECHECK = ["--n-screen", "3", "--n-voice", "3", "--broadcaster-id", "1", "--start-with-add", "--recheck", "--latency-person", "somebody", "--voice-channel", "lobby"];

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

describe("--recheck の旗の引数", () => {
    it("--start-with-add と一緒にだけ受ける。メインの手順の長さを決める旗とは両立しない", () => {
        expect(parseArgs([...REAL, "--broadcaster-id", "1", "--recheck"]).errors.join(" ")).toContain("--recheck は --start-with-add と一緒に");
        expect(parseArgs([...REAL, "--broadcaster-id", "1", "--start-with-add", "--recheck"]).errors).toEqual([]);
        for (const flag of ["--vesktop-share", "--extra-checks", "--skip-continuous", "--skip-av2", "--skip-export-bundle"]) {
            expect(parseArgs([...REAL, "--broadcaster-id", "1", "--start-with-add", "--recheck", flag]).errors.join(" ")).toContain(flag + " は --recheck と一緒に渡せない");
        }
        expect(parseArgs([...REAL, "--broadcaster-id", "1", "--start-with-add", "--recheck", "--skip-speaker-mute"]).errors.join(" ")).toContain("--skip-speaker-mute は --recheck と一緒に渡せない");
    });

    it("--broadcaster-id と一緒にだけ受ける (並びの人数の期待は、メインアカウントを自分の枠の人にする前提)", () => {
        expect(parseArgs([...REAL, "--start-with-add", "--recheck"]).errors.join(" ")).toContain("--recheck は --broadcaster-id と一緒に");
    });

    it("call-1 だけが受ける (call-hold では誤り)。リハーサルとは両立する", () => {
        expect(parseArgs(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-hold", "--real-profile", "--start-with-add", "--recheck", "--broadcaster-id", "1"]).errors.join(" ")).toContain("--recheck は --step call-1 だけ");
        expect(parseArgs([...REHEARSAL, "--broadcaster-id", "1", "--start-with-add", "--recheck"]).errors).toEqual([]);
    });
});

describe("--av-check (--recheck にテスト動画の A/V の測定を足す)", () => {
    it("--recheck と一緒にだけ受ける", () => {
        expect(parseArgs([...REAL, "--broadcaster-id", "1", "--start-with-add", "--av-check"]).errors.join(" ")).toContain("--av-check は --recheck と一緒に");
        expect(parseArgs([...REAL, ...RECHECK, "--av-check"]).errors).toEqual([]);
    });

    it("遅れの測定の後に、cordmux の録画を始め -> av1 の録画と sync.py -> 合否の基準 -> cordmux の録画を終える -> 作成を待つ -> 録画の中の A/V を読む", () => {
        const { steps } = plan([...REAL, ...RECHECK, "--av-check"]);
        const latencyStop = at(steps, (s) => s.kind === "record-stop" && s.label === LATENCY_RECORDING_LABEL);
        const recStart = at(steps, (s) => s.kind === "dock-click" && s.textId === "cordmux.dock.record_start.button", latencyStop);
        const avStart = at(steps, (s) => s.kind === "record-start" && s.label === "av1", recStart);
        const avStop = at(steps, (s) => s.kind === "record-stop" && s.label === "av1", avStart);
        const avJudge = at(steps, (s) => s.kind === "judge-av-sync" && s.recording === "av1", avStop);
        const recStop = at(steps, (s) => s.kind === "dock-click" && s.textId === "cordmux.dock.record_stop.button", avJudge);
        const finalize = at(steps, (s) => s.kind === "wait-finalize", recStop);
        at(steps, (s) => s.kind === "judge-record-sync" && s.person === "sub2", finalize);
        // 人の操作はテスト動画の共有の 1 つだけ増える。
        expect(steps.filter((s) => s.kind === "human")).toHaveLength(17);
    });

    it("渡さなければ、A/V の手順は入らない (--recheck の既定は前と同じ)", () => {
        const { steps } = plan([...REAL, ...RECHECK]);
        for (const kind of ["judge-av-sync", "judge-record-sync", "wait-finalize"]) expect(kinds(steps)).not.toContain(kind);
        expect(steps.filter((s) => s.label === "av1")).toEqual([]);
    });
});

describe("--recheck の手順の並び (実際の通話)", () => {
    const { args, steps } = plan([...REAL, ...RECHECK]);

    it("先頭は --start-with-add と同じ。接続の後に自分のアカウントの待ちが入り、参加の文・参加の後の画像が続く", () => {
        const order = ["seed-config", "launch-obs", "obsws-connect", "copy-state-json", "human", "read-state-json", "judge-vencord-dir", "check-file-exists", "count-connections", "human", "wait-connection", "wait-self-account", "human", "wire-composite"];
        expect(kinds(steps).filter((k) => order.includes(k)).slice(0, order.length)).toEqual(order);
        const wait = at(steps, (s) => s.kind === "wait-connection");
        expect(steps[wait + 1]).toMatchObject({ kind: "wait-self-account", timeoutMs: SELF_ACCOUNT_WAIT_TIMEOUT_MS, pollMs: SELF_ACCOUNT_POLL_MS, baseline: ADD_TO_VESKTOP_BASELINE_LABEL, gate: true });
        expect(steps[wait]).toMatchObject({ baseline: ADD_TO_VESKTOP_BASELINE_LABEL, timeoutMs: CALL_CONNECT_WAIT_TIMEOUT_MS });
        expect(SELF_ACCOUNT_WAIT_TIMEOUT_MS).toBe(120000);
        // 実行は続ける (stopOnFail を付けない)。
        expect(steps[wait + 1].stopOnFail).toBeUndefined();
        // 参加の文は Vesktop の共有を頼まない (--vesktop-share がない形と同じ)。Chrome の共有にタブの音声を含めてもらう 1 文と、
        // 連絡が途絶えたときの案内が付く。
        const join = steps[wait + 2];
        expect(join).toMatchObject({ kind: "human", message: callJoinMessage({ vesktopShare: false, voiceChannel: "lobby", recheck: true }) });
        expect(join.message).toContain("では画面共有を始めないでください");
        expect(join.message).toContain("タブの音声も共有する");
        expect(join.message.endsWith(CALL_CONTACT_LOST_GUIDANCE)).toBe(true);
        expect(callJoinMessage({ vesktopShare: false, voiceChannel: "lobby" })).not.toContain("タブの音声も共有する");
        expect(steps[wait + 3]).toMatchObject({ kind: "wire-composite" });
        expect(args.recheck).toBe(true);
    });

    it("Vesktop の起動の文: 追加の依頼で終了した Vesktop を起動し、ログインした状態の画面に戻ったら返事をしてもらう ([Restart] は使わない)", () => {
        // [Vesktopに追加] は Vesktop の起動中は押せないので、起動したまま追加して [Restart] する前の形はできない。
        const wait = at(steps, (s) => s.kind === "wait-connection");
        const start = steps.slice(0, wait).filter((s) => s.kind === "human").at(-1)!;
        expect(start.message).toBe(vesktopStartAfterAddMessage());
        expect(start.message).not.toContain("[Restart]");
        expect(start.message).toContain("ログインした状態の画面になったら");
        expect(start.message).not.toBe(VESKTOP_START_AFTER_ADD_MESSAGE);
        expect(start.message.endsWith(CALL_CONTACT_LOST_GUIDANCE)).toBe(true);
        // その前の依頼は、Vesktop を終了して [Vesktopに追加] を押す 1 つの依頼。
        const add = steps.slice(0, wait).filter((s) => s.kind === "human").at(-2)!;
        expect(add).toMatchObject({ until: "state-json-points-to-placed" });
        expect(add.message).toContain("[Quit]");
        // 起動の後の接続の待ちは今までと同じ (20 分、止める)。
        expect(steps[wait]).toMatchObject({ stopOnFail: true, timeoutMs: CALL_CONNECT_WAIT_TIMEOUT_MS });
        // --recheck でない --start-with-add の実行は、起動だけを依頼する短い文。
        const normal = plan([...REAL, "--n-screen", "3", "--n-voice", "3", "--broadcaster-id", "1", "--start-with-add"]).steps;
        const normalWait = normal.findIndex((s) => s.kind === "wait-connection");
        expect(normal.slice(0, normalWait).filter((s) => s.kind === "human").at(-1)!.message).toBe(VESKTOP_START_AFTER_ADD_MESSAGE);
    });

    it("続く本体は バージョンの表示 -> 画面共有の音 -> 遅れ -> 解像度の変化 -> エンコードの設定 -> 別アカウント方式 -> 自分の枠 -> ミュートと最小化・抜け戻り -> 実際のアバターの並び -> 締め", () => {
        const slot = at(steps, (s) => s.kind === "composite-layout-snapshot" && s.label === "slot-items");
        const about = at(steps, (s) => s.kind === "dialog-click" && s.label === "about-versions");
        const meter = at(steps, (s) => s.kind === "measure-input-level");
        const place = at(steps, (s) => s.kind === "place-window-capture");
        const remove = at(steps, (s) => s.kind === "remove-window-capture");
        const shrink = at(steps, (s) => s.kind === "resize-window" && s.mode === "set");
        const restore = at(steps, (s) => s.kind === "resize-window" && s.mode === "restore");
        const encoder = at(steps, (s) => s.kind === "dialog-click" && s.controlType === "ComboBox" && s.buttonTextId === TEXT_ID_SETTINGS_VIDEO_ENCODER);
        const accountChange = at(steps, (s) => s.kind === "dialog-click" && s.buttonTextId === TEXT_ID_DOCK_ACCOUNT_CHANGE);
        const selfCombo = at(steps, (s) => s.kind === "dialog-click" && s.controlType === "ComboBox" && s.buttonTextId === TEXT_ID_LOOK_SELF_FRAME_SOURCE);
        const mute = at(steps, (s) => s.kind === "measure-video-rate" && s.label === "mute-minimized");
        const back = at(steps, (s) => s.kind === "measure-video-rate" && s.label === "vesktop-rejoined");
        const count = at(steps, (s) => s.kind === "count-roster-labels");
        const order = [slot, about, meter, place, remove, shrink, restore, encoder, accountChange, selfCombo, mute, back, count];
        expect(order).toEqual([...order].sort((a, b) => a - b));
        // 録画は 2 本 (遅れ・解像度の変化)。どちらも配信なし。
        const records = steps.filter((s) => s.kind === "record-start");
        expect(records.map((s) => s.label)).toEqual([LATENCY_RECORDING_LABEL, RESCHANGE_RECORDING_LABEL]);
        expect(records.every((s) => s.stream === false)).toBe(true);
        expect(steps.filter((s) => s.kind === "record-stop").map((s) => s.label)).toEqual([LATENCY_RECORDING_LABEL, RESCHANGE_RECORDING_LABEL]);
        // 別アカウント方式の手順 (メニューから選ぶ -> 抜ける -> 戻る) の人の操作は 抜ける・戻る の 2 つだけ。
        const selfScene = at(steps, (s) => s.kind === "obsws-request" && s.request === "CreateScene");
        const broadcaster = steps.slice(accountChange, selfScene);
        expect(broadcaster.filter((s) => s.kind === "human").map((s) => s.message)).toEqual([expect.stringContaining("抜け"), expect.stringContaining("戻して")]);
        expect(broadcaster.some((s) => s.kind === "measure-video-rate")).toBe(false);
        // 受け取りが続くかを読む手順は、別アカウント方式の手順の後 (自分の枠の後) だけにある。
        expect(steps.slice(0, selfScene).some((s) => s.kind === "measure-video-rate")).toBe(false);
        // 並びの確認は通常の --extra-checks の (g) と同じ並び (バックアップ -> 共有を止める -> 待ち -> 画像 2 つ -> 判定 -> 通話を切って Vesktop を終了)。
        expect(steps[count]).toMatchObject({ label: ROSTER_BASELINE_LABEL });
        expect(steps[count + 1]).toMatchObject({ kind: "human", message: VESKTOP_STOP_SHARING_MESSAGE });
        expect(steps[count + 5]).toMatchObject({ kind: "judge-roster-labels", expectedPeople: 2 });
        expect(steps[count + 6]).toMatchObject({ kind: "human", message: VESKTOP_LEAVE_AND_QUIT_MESSAGE, quitVesktop: true });
        expect(rosterCheckPlanned(args)).toBe(true);
    });

    it("外す物がない: メインの手順の録画と配信・AC の判定・av1/av2・途中で録画・使う声と音量・連続実行・書き出し・ドックのスピーカーミュートの手順", () => {
        const dropped = [
            "stream-sink-start", "stream-stop", "stream-sink-stop", "judge-output-skipped", "count-operations", "judge-composite-layout", "judge-ac31", "judge-ac32",
            "judge-av-sync", "judge-av-drift", "judge-record-sync", "judge-voice-source", "judge-voice-volume", "tamper-file", "untamper-file",
            "judge-display-delay", "judge-audio-buffering", "harness-start",
        ];
        for (const kind of dropped) expect(kinds(steps)).not.toContain(kind);
        // 診断情報の書き出しはキーを送る手順なので 0。解像度の変化のウィンドウサイズの変更もキーを送らない。
        expect(keySendingSteps(steps)).toEqual([]);
        // 設定のウィンドウの [映像の受信] [バージョン情報] を開く手順 (dialog-click の ListItem) が**ある** (エンコードの設定とバージョンの表示)。
        // 自分の枠に映す映像と名前ラベルは、ドックの [表示] タブ (TabItem) で選ぶ。
        const pages = steps.filter((s) => s.kind === "dialog-click" && s.controlType === "ListItem").map((s) => s.buttonTextId);
        expect(pages).toContain(TEXT_ID_SETTINGS_PAGE_INCOMING_VIDEO);
        expect(pages).toContain("cordmux.settings.about.page");
        expect(steps.filter((s) => s.kind === "dialog-click" && s.controlType === "TabItem").map((s) => s.buttonTextId)).toContain(TEXT_ID_DOCK_TAB_DISPLAY);
        // 録画に絡む label は 遅れ と 解像度の変化 だけ (メインの手順・av・AC の録画はない)。
        expect(steps.filter((s) => s.label === "main" || s.label === "av1" || s.label === "av2" || s.label === "ac35" || s.label === "ac32")).toEqual([]);
        // 配信の受け口を使う手順はない (解像度の変化も配信しない)。
        expect(steps.some((s) => s.kind === "record-start" && s.stream)).toBe(false);
    });

    it("締めは 通話を切って Vesktop を [Quit] -> 設定の比べ -> バックアップ -> アンインストール -> 戻りの判定(gate) -> 比べ -> Vesktop を起動 -> OBS を閉じる -> 後始末", () => {
        const order = ["human", "compare-vencord-settings", "capture-backup", "human", "wait", "read-state-json", "judge-vencord-dir-restored", "compare-vencord-settings", "human", "close-obs"];
        const start = at(steps, (s) => s.kind === "human" && s.quitVesktop === true);
        expect(kinds(steps.slice(start)).filter((k) => order.includes(k)).slice(0, order.length)).toEqual(order);
        expect(steps[at(steps, (s) => s.kind === "judge-vencord-dir-restored")]).toMatchObject({ gate: true });
        expect(steps.at(-1)).toMatchObject({ kind: "cleanup" });
    });

    it("OBS を閉じた後の判定は 解像度の変化の録画 (frames.py)・遅れ・ログの読み・表 だけ。表は --recheck で実行した項目だけ", () => {
        const close = at(steps, (s) => s.kind === "close-obs");
        const afterAll = steps.slice(close);
        const after = afterAll.filter((s) => String(s.kind).startsWith("judge"));
        expect(after.map((s) => s.kind)).toEqual(["judge", "judge-latency", "judge-call-log", "judge-lines"]);
        expect(afterAll.find((s) => s.kind === "collect-recording")).toMatchObject({ label: RESCHANGE_RECORDING_LABEL });
        expect(after[0]).toMatchObject({ tool: "frames.py", recording: RESCHANGE_RECORDING_LABEL, cells: 3 });
        expect(after[1]).toMatchObject({ recording: LATENCY_RECORDING_LABEL, person: "somebody" });
        expect(after[3].items).toEqual(RECHECK_JUDGE_ITEMS);
        for (const item of RECHECK_JUDGE_ITEMS) expect(JUDGE_LINES.map((l) => l.item)).toContain(item);
        // 足した行はどれも結果を残すだけ (gate にしない)。
        for (const item of ["screen-audio", "format-change", "encode-limits", "encode-choice", "self-frame", "receive-continues", "rejoin"]) {
            expect(JUDGE_LINES.find((l) => l.item === item)).toMatchObject({ onFail: "record" });
            expect(RECHECK_JUDGE_ITEMS).toContain(item);
        }
        expect(RECHECK_JUDGE_ITEMS).toContain("version-row");
        // 通常の実行の judge-lines は絞らない。
        const full = plan([...REAL, "--n-screen", "3", "--n-voice", "3", "--broadcaster-id", "1", "--start-with-add", "--extra-checks", "--latency-person", "somebody"]).steps;
        expect(full.find((s) => s.kind === "judge-lines")!.items).toBeUndefined();
    });

    it("人の操作は 16 (テストを実行する人に依頼する物だけ)。見込みと期限は --recheck の長さ", () => {
        const humans = steps.filter((s) => s.kind === "human");
        expect(humans).toHaveLength(16);
        const planned = plannedMinutes(steps);
        const fullPlanned = plannedMinutes(plan([...REAL, "--n-screen", "3", "--n-voice", "3", "--broadcaster-id", "1", "--start-with-add", "--vesktop-share", "--skip-continuous", "--skip-speaker-mute", "--extra-checks", "--latency-person", "somebody", "--skip-export-bundle"]).steps);
        // --recheck は --extra-checks の実行より短い。
        expect(planned).toBeLessThan(fullPlanned);
        const est = deadlineEstimate(steps);
        expect(est.minutes).toBeGreaterThan(planned);
        expect(est.humanOverrunMinutes).toBe(CALL_ONE_HUMAN_OVERRUN_MIN);
        // 期限の見積もりは現在の値 (見積もりと 180 分の大きいほう)。人への文の「長くても約3時間」「3時間半」は 180 分の期限に合わせた文言なので、
        // 見積もりが 180 分を超えたら文を修正する。
        expect(deadlineMinFor(args, steps)).toBe(Math.max(180, est.minutes));
        expect(est.minutes).toBeLessThanOrEqual(180);
    });

    it("足した 6 つの人への文: 位置と順、それだけ読んで分かる形 (現在の状態・操作・「OK」の通知・連絡が途絶えたときの案内)", () => {
        const messages = steps.filter((s) => s.kind === "human").map((s) => s.message as string);
        const added = [
            encodeRestartShareMessage("lobby"),
            selfFrameShareStartMessage("lobby"),
            selfFrameShareStopMessage("lobby"),
            speakerMuteMinimizeMessage("lobby"),
            speakerUnmuteLeaveMessage("lobby"),
            vesktopRejoinMessage("lobby"),
        ];
        // 展開は run-window.mjs が行う印 ({{id}}) のまま入っている。位置は 参加の文の後・遅れの文の後・別アカウント方式の後・並びの前。
        const at6 = added.map((m) => messages.indexOf(m));
        expect(at6.every((i) => i >= 0)).toBe(true);
        expect(at6).toEqual([...at6].sort((a, b) => a - b));
        const latency = messages.findIndex((m) => m.includes("somebody"));
        const leave = messages.findIndex((m) => m.includes("抜けさせて"));
        const comeBack = messages.findIndex((m) => m.includes("戻してください"));
        expect(at6[0]).toBeGreaterThan(latency);
        expect(at6[0]).toBeLessThan(leave);
        expect(at6[1]).toBeGreaterThan(comeBack);
        expect(messages.indexOf(VESKTOP_STOP_SHARING_MESSAGE)).toBeGreaterThan(at6[5]);
        for (const m of added) {
            expect(m).toContain("「OK」と書いてください");
            expect(m).toContain("テスト用のOBSのウィンドウには触らないでください");
            expect(m.endsWith(CALL_CONTACT_LOST_GUIDANCE)).toBe(true);
            // 「テスト用」は --voice-channel の値 (lobby)。文字列として持たない。
            expect(m).not.toContain("テスト用」");
            expect(m).toContain("「lobby」");
        }
        // 自分の枠の 2 つの文は、人に何かを見てもらわない (チェックは実行スクリプトが dock-click で付け外しし、ログで読む)。
        for (const m of [selfFrameShareStartMessage("lobby"), selfFrameShareStopMessage("lobby")]) {
            expect(m).not.toContain("{{" + TEXT_ID_ROW_CHECK + "}}");
            expect(m).not.toContain("見ておいてください");
            expect(m).not.toContain("あとで私が聞きます");
        }
        // エンコードの文は、選んだ値を ini のテキストで挙げる。
        expect(encodeRestartShareMessage("lobby")).toContain("{{" + TEXT_ID_SETTINGS_VIDEO_ENCODER_SOFTWARE + "}}");
        expect(encodeRestartShareMessage("lobby")).toContain("{{" + TEXT_ID_SETTINGS_ENCODE_PRIORITY_QUALITY + "}}");
        // 名前が渡されないときも読める (名前を挙げずに書く)。
        expect(vesktopRejoinMessage(null)).toContain("指定のボイスチャンネル");
        // 待つ時限は 30 分 (ほかの人の操作と同じ長さ)。
        for (const h of steps.filter((s) => s.kind === "human" && added.includes(s.message))) expect(h.timeoutMs).toBe(30 * 60 * 1000);
        // 期限 180 分に合わせた文言。
        expect(CALL_CONTACT_LOST_GUIDANCE).toContain("約3時間");
        expect(CALL_CONTACT_LOST_GUIDANCE).toContain("3時間半");
    });

    it("新しい手順の種類は 4 つ (wait-self-account・resize-window・measure-input-level・judge-log-lines)。画面のテキストの参照は ini のキーで解ける", () => {
        const known = new Set(
            plan([...REAL, "--n-screen", "3", "--n-voice", "3", "--broadcaster-id", "1", "--start-with-add", "--extra-checks", "--latency-person", "somebody", "--skip-export-bundle"]).steps.map((s) => s.kind),
        );
        const added = [...new Set(kinds(steps))].filter((k) => !known.has(k));
        expect(added.sort()).toEqual(["judge-log-lines", "measure-input-level", "resize-window", "wait-self-account"]);
        expect(stepTextIds(steps.find((s) => s.kind === "wait-self-account")!)).toEqual([]);
        // 足した画面のテキストは dry-run が引く一覧 (CALL_TEXT_IDS) に入っている。
        for (const id of [
            TEXT_ID_LOOK_SELF_FRAME_SOURCE,
            TEXT_ID_SETTINGS_VIDEO_ENCODER,
            TEXT_ID_SETTINGS_VIDEO_ENCODER_SOFTWARE,
            TEXT_ID_SETTINGS_ENCODE_PRIORITY,
            TEXT_ID_SETTINGS_ENCODE_PRIORITY_QUALITY,
            TEXT_ID_ROW_CHECK,
            "cordmux.dock.summary_connected.status",
        ]) {
            expect(CALL_TEXT_IDS).toContain(id);
        }
        // 項目を選ぶ手順のテキストは stepTextIds が拾う (ComboBox のラベルと項目)。
        const combo = steps.find((s) => s.kind === "dialog-click" && s.controlType === "ComboBox" && s.buttonTextId === TEXT_ID_SETTINGS_VIDEO_ENCODER)!;
        expect(combo.selectTextId).toBe(TEXT_ID_SETTINGS_VIDEO_ENCODER_SOFTWARE);
        expect(stepTextIds(combo)).toEqual(expect.arrayContaining([TEXT_ID_SETTINGS_VIDEO_ENCODER, TEXT_ID_SETTINGS_VIDEO_ENCODER_SOFTWARE]));
        // すべての手順のテキストの参照 (人への文の印を含む) が一覧に入っている。
        const referenced = new Set(steps.flatMap((s) => stepTextIds(s)));
        expect(referenced.has(TEXT_ID_SETTINGS_ENCODE_PRIORITY_QUALITY)).toBe(true);
    });

    it("実行スクリプトが新しい手順の種類を処理する (ファイルのテキストで確認する)。ComboBox は -Select で、ウィンドウサイズは window-size.ps1 の -ProcessName で渡す", () => {
        const source = readFileSync(join(HERE, "run-window.mjs"), "utf8");
        const handled = source.slice(source.indexOf("const HANDLED_KINDS = ["), source.indexOf("];", source.indexOf("const HANDLED_KINDS = [")));
        for (const kind of ["wait-self-account", "resize-window", "measure-input-level", "judge-log-lines"]) {
            expect(handled).toContain('"' + kind + '"');
            expect(source).toContain('case "' + kind + '": {');
        }
        expect(source).toContain("judgeLinesSummary(state.results, { items: s.items })");
        expect(source).toContain('cli.push("-Select", select)');
        expect(source).toContain('"-ProcessName", s.processName');
        expect(source).toContain("OBS_EVENT_SUBSCRIPTION_INPUT_VOLUME_METERS");
        // ウィンドウサイズは SendKeys を使わない。
        expect(readFileSync(join(HERE, "window-size.ps1"), "utf8")).not.toMatch(/SendKeys|SetForegroundWindow/);
    });
});

describe("--recheck の足した手順 (実際の通話)", () => {
    const { steps } = plan([...REAL, ...RECHECK]);
    const labelOf = (s: Step) => s.label;

    it("バージョンの表示: 参加の後の画像の後に、[バージョン情報] を読んで Vesktop の行を確認する (人の操作なし)", () => {
        const slot = at(steps, (s) => s.kind === "composite-layout-snapshot" && s.label === "slot-items");
        const read = at(steps, (s) => s.kind === "dialog-click" && s.label === "about-versions");
        const judge = at(steps, (s) => s.kind === "judge-rt" && s.judge === "version-row-shown");
        expect(read).toBeGreaterThan(slot);
        expect(judge).toBeGreaterThan(read);
        expect(steps.slice(slot, judge).some((s) => s.kind === "human")).toBe(false);
    });

    it("画面共有の音: 声をミュート -> 行数を保存する -> ミュートを外す -> メーターを読む -> ログの行 -> ミュートに戻す -> 声のミュートを戻す", () => {
        const before = at(steps, (s) => s.kind === "judge-log-lines" && s.label === "sa-before");
        expect(steps[before].checks).toEqual([]);
        // 合成のソースのメーターは声の音も含むので、メーターを読む間は声を全員ミュートにする (Chrome のテスト用のマイクの 1 kHz が混ざらない)。
        const voiceMute = (s: Step, desired: string) => s.kind === "dock-click" && s.muteOp === true && s.textId === "cordmux.dock.row_voice_mute.a11y" && s.desired === desired;
        const isolated = steps.slice(0, before).filter((s) => voiceMute(s, "on")).slice(-2);
        expect(isolated.map((s) => s.textArgs[0])).toEqual(["sub2", "sub3"]);
        expect(steps.slice(before - 2, before).every((s) => voiceMute(s, "on"))).toBe(true);
        const off = at(steps, (s) => s.kind === "dock-click" && s.muteOp === true && s.desired === "off", before);
        expect(steps[off]).toMatchObject({ textArgs: ["sub2"], textId: "cordmux.dock.row_screen_mute.a11y" });
        const meter = at(steps, (s) => s.kind === "measure-input-level", off);
        expect(steps[meter]).toMatchObject({ inputName: "cordmux", seconds: 10, thresholdDbfs: INPUT_LEVEL_THRESHOLD_DBFS, label: "screen-audio" });
        expect(INPUT_LEVEL_THRESHOLD_DBFS).toBe(-60);
        const after = at(steps, (s) => s.kind === "judge-log-lines" && s.label === "sa-after", meter);
        expect(steps[after]).toMatchObject({ since: "sa-before" });
        expect(steps[after].checks).toEqual([{ id: "placed", pattern: LOG_PATTERN_SCREEN_AUDIO_PLACED, min: 1 }]);
        const on = at(steps, (s) => s.kind === "dock-click" && s.muteOp === true && s.textId === "cordmux.dock.row_screen_mute.a11y" && s.desired === "on", after);
        expect(steps[on]).toMatchObject({ textArgs: ["sub2"] });
        // 声のミュートは画面共有の音声をミュートに戻した後に戻す。
        expect(steps.slice(on + 1, on + 3).every((s) => voiceMute(s, "off"))).toBe(true);
        expect(steps.slice(on + 1, on + 3).map((s) => s.textArgs[0])).toEqual(["sub2", "sub3"]);
        expect(steps[meter].gate).toBeUndefined();
        // 見込みの分には、メーターを読む長さが入る。
        expect(plannedMinutes([steps[meter]])).toBeCloseTo(10 / 60, 1);
    });

    it("共有の途中の解像度の変化: 録画 -> Chrome のウィンドウを縮める -> 猶予 -> 画像と配置 -> 戻す -> 録画を止める。ウィンドウが見つからなくても先へ進む (gate なし)", () => {
        const start = at(steps, (s) => s.kind === "record-start" && s.label === RESCHANGE_RECORDING_LABEL);
        const shrink = at(steps, (s) => s.kind === "resize-window" && s.mode === "set");
        const shrunk = at(steps, (s) => s.kind === "screenshot-source" && s.label === "reschange-shrunk");
        const restore = at(steps, (s) => s.kind === "resize-window" && s.mode === "restore");
        const stop = at(steps, (s) => s.kind === "record-stop" && s.label === RESCHANGE_RECORDING_LABEL);
        expect([start, shrink, shrunk, restore, stop]).toEqual([start, shrink, shrunk, restore, stop].sort((a, b) => a - b));
        expect(steps[shrink]).toMatchObject({ processName: RESCHANGE_BROWSER_PROCESS, width: RESCHANGE_SMALL_WIDTH, height: RESCHANGE_SMALL_HEIGHT, label: RESCHANGE_RECORDING_LABEL });
        expect(steps[restore]).toMatchObject({ processName: RESCHANGE_BROWSER_PROCESS, label: RESCHANGE_RECORDING_LABEL });
        expect(steps[restore].width).toBeUndefined();
        expect(steps[shrink].gate).toBeUndefined();
        expect(steps[restore].gate).toBeUndefined();
        // ログの行で format の変化を見る手順はない (format は共有の途中で変わらない)。録画の判定が frames.py で読む。
        expect(steps.some((s) => s.kind === "judge-log-lines" && String(s.label).startsWith("fmt-"))).toBe(false);
        // 縮めた後の待ちは、送信側がエンコーダを確保し直すのに足りる長さ。録画の中に縮めと戻しが入る。
        const wait = steps[shrink + 1];
        expect(wait.kind).toBe("wait");
        expect(wait.ms).toBeGreaterThanOrEqual(15000);
        expect(resolutionChangePlanned(parseArgs([...REAL, ...RECHECK]).args)).toBe(true);
        expect(resolutionChangePlanned(parseArgs([...REHEARSAL, ...RECHECK]).args)).toBe(false);
    });

    it("エンコードの設定: [映像の受信] で 2 つの ComboBox を選び -> [適用] -> ログ -> 人の操作 (Firefox の共有を始め直す) -> 送信側のエンコーダの確保のログの行。戻しは入れない", () => {
        const page = at(steps, (s) => s.kind === "dialog-click" && s.controlType === "ListItem" && s.buttonTextId === TEXT_ID_SETTINGS_PAGE_INCOMING_VIDEO);
        const enc = at(steps, (s) => s.kind === "dialog-click" && s.controlType === "ComboBox" && s.buttonTextId === TEXT_ID_SETTINGS_VIDEO_ENCODER, page);
        const prio = at(steps, (s) => s.kind === "dialog-click" && s.controlType === "ComboBox" && s.buttonTextId === TEXT_ID_SETTINGS_ENCODE_PRIORITY, enc);
        expect(steps[prio]).toMatchObject({ selectTextId: TEXT_ID_SETTINGS_ENCODE_PRIORITY_QUALITY });
        const apply = at(steps, (s) => s.kind === "dialog-click" && s.buttonTextId === OBS_TEXT_APPLY, prio);
        const limits = at(steps, (s) => s.kind === "judge-log-lines" && s.label === "encode-limits", apply);
        const ask = at(steps, (s) => s.kind === "human" && s.message === encodeRestartShareMessage("lobby"), limits);
        const choice = at(steps, (s) => s.kind === "judge-log-lines" && s.label === "encode-choice", ask);
        expect(steps[limits].checks).toEqual([{ id: "limits-sent", pattern: LOG_PATTERN_LIMITS_SENT, min: 1 }]);
        expect(steps[choice].checks).toEqual([{ id: "sender-choice", pattern: LOG_PATTERN_SENDER_CHOICE, min: 1 }]);
        expect(steps[limits]).toMatchObject({ since: "encode-before" });
        expect(steps[choice]).toMatchObject({ since: "encode-before" });
        // 戻しは入れない: ComboBox を選ぶのはエンコードの 2 つと自分の枠の 1 つだけ。
        expect(steps.filter((s) => s.kind === "dialog-click" && s.controlType === "ComboBox")).toHaveLength(3);
        // ログの文字列は製品が書く行と合う (正規表現が実際の行に合う)。
        expect(new RegExp(LOG_PATTERN_LIMITS_SENT).test("12:00:00.000: [cordmux] Quality limits sent to Vesktop (max_long_edge=0 fps_divisor=1 encoder=software latency=quality)")).toBe(true);
        expect(new RegExp(LOG_PATTERN_LIMITS_SENT).test("12:00:00.000: [cordmux] Quality limits sent to Vesktop (max_long_edge=0 fps_divisor=1 encoder=hardware latency=speed)")).toBe(false);
        expect(new RegExp(LOG_PATTERN_SENDER_CHOICE).test("12:00:01.000: [cordmux/js] 12:00:01 video stream 3: stage=1 format=1280x720@30 preference=prefer-software latencyMode=quality")).toBe(true);
    });

    it("自分の枠 (ほかの人の行と同じ動き): 選ぶ -> 共有の前の読み -> 人が共有を始める (出ない) -> チェックを付ける (出る) -> 人が共有を止める (残る) -> チェックを外す (消える)", () => {
        const scene = at(steps, (s) => s.kind === "obsws-request" && s.request === "CreateScene");
        expect(steps[scene].requestData.sceneName).not.toBe("Main");
        const input = at(steps, (s) => s.kind === "obsws-request" && s.request === "CreateInput", scene);
        expect(steps[input].requestData).toMatchObject({ sceneName: steps[scene].requestData.sceneName, inputName: SELF_FRAME_SOURCE_NAME, inputKind: "color_source_v3" });
        const general = at(steps, (s) => s.kind === "dialog-click" && s.controlType === "TabItem" && s.buttonTextId === TEXT_ID_DOCK_TAB_DISPLAY, input);
        const combo = at(steps, (s) => s.kind === "dialog-click" && s.controlType === "ComboBox" && s.buttonTextId === TEXT_ID_LOOK_SELF_FRAME_SOURCE, general);
        expect(steps[combo]).toMatchObject({ selectText: SELF_FRAME_SOURCE_NAME });
        const before = at(steps, (s) => s.kind === "judge-log-lines" && s.label === "self-before", combo);
        const start = at(steps, (s) => s.kind === "human" && s.message === selfFrameShareStartMessage("lobby"), before);
        const share = at(steps, (s) => s.kind === "judge-log-lines" && s.label === "self-share", start);
        const on = at(steps, (s) => s.kind === "dock-click" && s.name === DEFAULT_CALL_ACCOUNTS.main && s.desired === "on", share);
        const during = at(steps, (s) => s.kind === "judge-log-lines" && s.label === "self-during", on);
        const stop = at(steps, (s) => s.kind === "human" && s.message === selfFrameShareStopMessage("lobby"), during);
        const stopped = at(steps, (s) => s.kind === "judge-log-lines" && s.label === "self-stopped", stop);
        const off = at(steps, (s) => s.kind === "dock-click" && s.name === DEFAULT_CALL_ACCOUNTS.main && s.desired === "off", stopped);
        const after = at(steps, (s) => s.kind === "judge-log-lines" && s.label === "self-after", off);
        expect(after).toBeGreaterThan(off);
        const ids = (s: Step) => s.checks.map((c: Step) => c.id);
        // 手動のときは、共有を始めても自分の枠は出ない: 付いたログの行が増えず、配置の行も増えない (配置の行は変わったときだけ出る)。
        expect(steps[share]).toMatchObject({ since: "self-before" });
        expect(ids(steps[share])).toEqual(["self-not-on", "layout-unchanged"]);
        expect(steps[share].checks[0]).toMatchObject({ pattern: LOG_PATTERN_SELF_ON_ANY, min: 0, max: 0 });
        expect(steps[share].checks[1]).toMatchObject({ pattern: LOG_PATTERN_CANVAS_COUNT, min: 0, max: 0 });
        // チェックを付けると出る: 枠が 1 つ増え、self= が入る。
        expect(steps[during]).toMatchObject({ since: "self-share" });
        expect(ids(steps[during])).toEqual(["self-on", "canvas-count", "canvas-self"]);
        expect(steps[during].checks[0].pattern).toBe(LOG_PATTERN_SELF_ON);
        expect(steps[during].checks[1].delta).toEqual({ from: "self-before", equals: 1 });
        // 共有を止めても、チェックは外れず自分の枠は出たまま。
        expect(steps[stopped]).toMatchObject({ since: "self-during" });
        expect(ids(steps[stopped])).toEqual(["self-not-off", "layout-unchanged"]);
        expect(steps[stopped].checks[0]).toMatchObject({ pattern: LOG_PATTERN_SELF_OFF_ANY, min: 0, max: 0 });
        // チェックを外すと消え、枠の数が元に戻る。
        expect(steps[after]).toMatchObject({ since: "self-stopped" });
        expect(ids(steps[after])).toEqual(["self-off", "canvas-count"]);
        expect(steps[after].checks[0].pattern).toBe(LOG_PATTERN_SELF_OFF);
        expect(steps[after].checks[1].delta).toEqual({ from: "self-before", equals: 0 });
        expect(steps[before].checks).toEqual([{ id: "canvas-count", pattern: LOG_PATTERN_CANVAS_COUNT, min: 1, group: 1 }]);
        // 付けた行と外した行は、製品のログの文字列に合う。共有の始まりと終わりの文字列 (screen share started / ended) には頼らない。
        expect(new RegExp(LOG_PATTERN_SELF_ON).test("12:00:00.000: [cordmux] Self frame turned on: dock")).toBe(true);
        expect(new RegExp(LOG_PATTERN_SELF_OFF).test("12:00:00.000: [cordmux] Self frame turned off: dock")).toBe(true);
        expect(new RegExp(LOG_PATTERN_SELF_ON_ANY).test("12:00:00.000: [cordmux] Self frame turned on: screen share")).toBe(true);
        expect(new RegExp(LOG_PATTERN_SELF_ON).test("12:00:00.000: [cordmux] Self frame turned on: screen share")).toBe(false);
        expect(JSON.stringify(steps)).not.toContain("screen share started");
        // 共有の前後と、チェックを付けた後・外した後に、配置・ドックの文・ドックの PNG を読む (目で読む材料)。
        for (const label of ["self-before", "self-share", "self-during", "self-stopped", "self-after"]) {
            expect(steps.some((s) => s.kind === "composite-layout-snapshot" && s.label === label)).toBe(true);
            expect(steps.some((s) => s.kind === "dialog-click" && s.listOnly === true && s.label === label)).toBe(true);
            expect(steps.some((s) => s.kind === "screenshot-window" && s.shotLabel === label)).toBe(true);
        }
    });

    it("スピーカーミュートと最小化 -> 受け手の Vesktop の抜け戻り: 接続の数を保存し、受け取りと接続の保ちを読む", () => {
        const baseline = at(steps, (s) => s.kind === "count-connections" && s.label === "before-vesktop-leave");
        const m1 = at(steps, (s) => s.kind === "human" && s.message === speakerMuteMinimizeMessage("lobby"), baseline);
        const rate1 = at(steps, (s) => s.kind === "measure-video-rate" && s.label === "mute-minimized", m1);
        const m2 = at(steps, (s) => s.kind === "human" && s.message === speakerUnmuteLeaveMessage("lobby"), rate1);
        const kept = at(steps, (s) => s.kind === "judge-connections-kept" && s.label === "vesktop-left", m2);
        const v2 = at(steps, (s) => s.kind === "human" && s.message === vesktopRejoinMessage("lobby"), kept);
        const rate2 = at(steps, (s) => s.kind === "measure-video-rate" && s.label === "vesktop-rejoined", v2);
        expect(rate2).toBeGreaterThan(v2);
        expect(steps[kept]).toMatchObject({ baseline: "before-vesktop-leave" });
        // 受け取りが続くまでの待ち (1 分) が、ミュートと最小化を依頼する文の後にある。
        expect(steps.slice(m1, rate1).some((s) => s.kind === "wait" && s.ms >= 60000)).toBe(true);
        // 「映像が止まっています」のメッセージがないこと、「Vesktopと接続済み」で通話に参加していないこと、戻った後の行は結果を残すだけ (gate なし)。
        const judges = steps.slice(baseline).filter((s) => s.kind === "judge-rt");
        expect(judges.length).toBeGreaterThanOrEqual(3);
        for (const j of judges) expect(j.gate).toBeUndefined();
        expect(labelOf(steps[rate1])).toBe("mute-minimized");
    });

    it("足した確認はどれも gate にしない (終了コードを変えない)", () => {
        const added = ["resize-window", "measure-input-level", "judge-log-lines"];
        for (const s of steps.filter((x) => added.includes(x.kind))) expect(s.gate).toBeUndefined();
        const recheckOnly = steps.filter((s) => s.kind === "dialog-click" && s.controlType === "ComboBox");
        for (const s of recheckOnly) expect(s.gate).toBeUndefined();
    });
});

describe("judgeLogLines (ログの行を正規表現で数える)", () => {
    it("行の数の下限 (既定 1) と上限。合った数と理由を返す", () => {
        const lines = ["a Self frame turned on: screen share started", "b", "c Self frame turned on: screen share started"];
        expect(judgeLogLines({ lines, checks: [{ id: "on", pattern: "Self frame turned on" }] })).toMatchObject({ ok: true, checks: [{ id: "on", ok: true, count: 2 }] });
        const none = judgeLogLines({ lines, checks: [{ id: "off", pattern: "Self frame turned off" }] });
        expect(none).toMatchObject({ ok: false, checks: [{ id: "off", ok: false, count: 0 }] });
        expect(none.checks[0].reason).toContain("expected at least 1");
        expect(judgeLogLines({ lines, checks: [{ id: "on", pattern: "Self frame turned on", max: 1 }] }).ok).toBe(false);
        expect(judgeLogLines({ lines, checks: [{ id: "none", pattern: "zzz", min: 0, max: 0 }] }).ok).toBe(true);
    });

    it("checks がない手は行数を保存するだけで合格。lineCount は渡された行の数", () => {
        expect(judgeLogLines({ lines: ["x", "y", "z"], checks: [] })).toMatchObject({ ok: true, lineCount: 3, checks: [] });
        expect(judgeLogLines()).toMatchObject({ ok: true, lineCount: 0 });
    });

    it("group: 最後に合った行の群を数で残す", () => {
        const lines = ["Composite layout canvas=1920x1080 count=2 slot1=0,0,1,1", "Composite layout canvas=1920x1080 count=3 self=0,0,1,1"];
        expect(judgeLogLines({ lines, checks: [{ id: "n", pattern: LOG_PATTERN_CANVAS_COUNT, group: 1 }] }).checks[0]).toMatchObject({ ok: true, count: 2, last: 3 });
    });

    it("distinct: 同じ key で value が 2 種類以上に変わるか (解像度の変化)", () => {
        const spec = { id: "format-change", pattern: "video stream (\\d+): stage=\\S+ format=(\\d+x\\d+@[\\d.]+)", distinct: { key: 1, value: 2, min: 2 } };
        const line = (id: number, fmt: string) => `12:00:00.000: [cordmux/js] 12:00:00 video stream ${id}: stage=1 format=${fmt} preference=prefer-hardware latencyMode=realtime`;
        const changed = judgeLogLines({ lines: [line(3, "1280x720@30"), line(4, "640x360@30"), line(3, "800x450@30")], checks: [spec] });
        expect(changed).toMatchObject({ ok: true, checks: [{ ok: true, count: 3, distinctMax: 2 }] });
        // 別々のストリームで種類が違うだけでは変化と言わない。
        const other = judgeLogLines({ lines: [line(3, "1280x720@30"), line(4, "640x360@30")], checks: [spec] });
        expect(other).toMatchObject({ ok: false, checks: [{ ok: false, distinctMax: 1 }] });
        expect(other.checks[0].reason).toContain("distinct");
        // format=none (確保できなかった手順) は数えない。
        expect(judgeLogLines({ lines: ["video stream 3: stage=1 format=none preference=x latencyMode=y"], checks: [spec] }).checks[0].count).toBe(0);
    });

    it("delta: 前の手の結果の last に equals を足した値であること。前の結果がない・数でないときは理由つきの失敗", () => {
        const prev = judgeLogLines({ lines: ["Composite layout canvas=1x1 count=2"], checks: [{ id: "canvas-count", pattern: LOG_PATTERN_CANVAS_COUNT, group: 1 }] });
        const spec = (equals: number) => ({ id: "canvas-count", pattern: LOG_PATTERN_CANVAS_COUNT, group: 1, delta: { from: "self-before", equals } });
        const lines = ["Composite layout canvas=1x1 count=3"];
        expect(judgeLogLines({ lines, checks: [spec(1)], previous: { "self-before": prev } }).ok).toBe(true);
        const wrong = judgeLogLines({ lines, checks: [spec(0)], previous: { "self-before": prev } });
        expect(wrong.ok).toBe(false);
        expect(wrong.checks[0]).toMatchObject({ last: 3, expectedLast: 2 });
        expect(judgeLogLines({ lines, checks: [spec(1)], previous: {} }).checks[0].reason).toContain("cannot compare with 'self-before'");
        expect(judgeLogLines({ lines: ["no layout"], checks: [{ ...spec(1), min: 0 }], previous: { "self-before": prev } }).checks[0].reason).toContain("cannot compare");
    });

    it("正規表現が壊れていれば、その check だけ理由つきの失敗", () => {
        const r = judgeLogLines({ lines: ["a"], checks: [{ id: "bad", pattern: "(" }, { id: "good", pattern: "a" }] });
        expect(r.checks[0]).toMatchObject({ id: "bad", ok: false });
        expect(r.checks[0].reason).toContain("invalid pattern");
        expect(r.checks[1]).toMatchObject({ id: "good", ok: true });
        expect(r.ok).toBe(false);
    });

    it("画面共有の音の行: placed_frames が 0 の行は数えず、1 以上の screen_audio の行だけを数える", () => {
        const lines = [
            "Stats mixer_track voice slot=2 placed_frames=480 late_frames=0",
            "Stats mixer_track screen_audio slot=1 placed_frames=0 late_frames=0",
            "Stats mixer_track screen_audio slot=1 placed_frames=480 late_frames=0",
        ];
        expect(judgeLogLines({ lines, checks: [{ id: "placed", pattern: LOG_PATTERN_SCREEN_AUDIO_PLACED }] }).checks[0]).toMatchObject({ ok: true, count: 1 });
    });
});

describe("入力の音量メーター (InputVolumeMeters)", () => {
    const meter = (inputName: string, ...peaks: number[]) => ({
        eventType: "InputVolumeMeters",
        eventData: { inputs: [{ inputName, inputLevelsMul: peaks.map((p) => [p / 2, p, p]) }, { inputName: "other", inputLevelsMul: [[1, 1, 1]] }] },
    });

    it("peak の最大を dBFS で返す。他の入力のメッセージは数えない", () => {
        const r = inputPeakFromMeterEvents([meter("cordmux", 0.001, 0.1), meter("cordmux", 0.5, 0.02)], "cordmux");
        expect(r.samples).toBe(2);
        expect(r.maxPeakDbfs).toBeCloseTo(-6.02, 1);
        expect(inputPeakFromMeterEvents([meter("x", 0.5)], "cordmux")).toEqual({ samples: 0, maxPeakDbfs: null });
        expect(inputPeakFromMeterEvents([{ eventType: "Other", eventData: {} }, null, {}] as any, "cordmux").samples).toBe(0);
    });

    it("judgeInputLevel: 線より大きければ合格。無音・メッセージなし・線以下は理由つきの失敗", () => {
        expect(judgeInputLevel({ events: [meter("cordmux", 0.05)], inputName: "cordmux", thresholdDbfs: -60 })).toMatchObject({ ok: true, samples: 1 });
        const quiet = judgeInputLevel({ events: [meter("cordmux", 0.0001)], inputName: "cordmux", thresholdDbfs: -60 });
        expect(quiet.ok).toBe(false);
        expect(quiet.reason).toContain("not above -60");
        const silent = judgeInputLevel({ events: [meter("cordmux", 0, 0)], inputName: "cordmux", thresholdDbfs: -60 });
        expect(silent).toMatchObject({ ok: false, maxPeakDbfs: null });
        expect(silent.reason).toContain("silent");
        const none = judgeInputLevel({ events: [], inputName: "cordmux" });
        expect(none).toMatchObject({ ok: false, samples: 0, thresholdDbfs: INPUT_LEVEL_THRESHOLD_DBFS });
        expect(none.reason).toContain("no InputVolumeMeters event");
    });
});

describe("judge-lines の足した行 (結果を残すだけ)", () => {
    const logRes = (label: string, ok: boolean) => ({ name: "judge-log-lines", label, ok, checks: [{ id: "c", ok, count: ok ? 1 : 0, last: 3 }], reason: ok ? null : "no match" });
    const reschangeRes = (cells: any[]) => ({ name: "judge", tool: "frames.py", label: "reschange-frames", ok: true, cells: cells.length, perCell: cells.map((data, cell) => ({ cell, data })) });
    const rate = (label: string, stopped: boolean | null) => ({ name: "measure-video-rate", label, framesStopped: stopped, samples: [{ receivedHz: stopped ? 0 : 30 }] });
    const summary = (results: any[]) => judgeLinesSummary(results, { items: RECHECK_JUDGE_ITEMS });
    const row = (s: any, item: string) => s.rows.find((r: Step) => r.item === item);

    it("実行していない行は ok が null (失敗にならない)", () => {
        const s = summary([]);
        for (const item of ["screen-audio", "format-change", "encode-limits", "encode-choice", "self-frame", "receive-continues", "rejoin"]) expect(row(s, item)).toMatchObject({ ok: null, onFail: "record" });
        expect(s.red).toEqual([]);
    });

    it("画面共有の音: メーターとログの行の両方が合格なら合格。片方が失敗なら失敗、片方しか無ければ決められない", () => {
        const meter = { name: "measure-input-level", label: "screen-audio", ok: true, maxPeakDbfs: -20, samples: 100 };
        expect(row(summary([meter, logRes("sa-after", true)]), "screen-audio")).toMatchObject({ ok: true, detail: { maxPeakDbfs: -20, samples: 100 } });
        const red = summary([{ ...meter, ok: false, reason: "silent" }, logRes("sa-after", true)]);
        expect(row(red, "screen-audio")).toMatchObject({ ok: false, reason: "silent" });
        expect(red.red).toEqual(["screen-audio"]);
        expect(row(summary([meter]), "screen-audio").ok).toBeNull();
    });

    it("エンコードの設定・解像度の変化は、対応するログの行の結果", () => {
        const s = summary([logRes("encode-limits", true), logRes("encode-choice", false), reschangeRes([{ crc_failures: 0, unreadable_frames: 0, max_freeze_ms: 0 }, { crc_failures: 0, unreadable_frames: 0, max_freeze_ms: RESCHANGE_MAX_FREEZE_MS }])]);
        expect(row(s, "encode-limits").ok).toBe(true);
        expect(row(s, "encode-choice")).toMatchObject({ ok: false, reason: "no match" });
        expect(row(s, "format-change")).toMatchObject({ ok: true });
    });

    it("解像度の変化の録画: 全部の枠が CRC の失敗 0・読めないフレーム 0・凍りが上限以下なら合格。1 枠でも外れれば失敗。数を読めない枠があれば決められない", () => {
        const good = { crc_failures: 0, unreadable_frames: 0, max_freeze_ms: 40 };
        expect(row(summary([reschangeRes([good, good])]), "format-change").ok).toBe(true);
        expect(row(summary([reschangeRes([good, { ...good, crc_failures: 1 }])]), "format-change")).toMatchObject({ ok: false, reason: expect.stringContaining("cell 1") });
        expect(row(summary([reschangeRes([{ ...good, unreadable_frames: 2 }])]), "format-change").ok).toBe(false);
        expect(row(summary([reschangeRes([{ ...good, max_freeze_ms: RESCHANGE_MAX_FREEZE_MS + 1 }])]), "format-change").ok).toBe(false);
        expect(row(summary([reschangeRes([good, { barcode_found: false }])]), "format-change").ok).toBeNull();
        expect(row(summary([{ name: "judge", tool: "frames.py", label: "reschange-frames", ok: false, error: "boom" }]), "format-change")).toMatchObject({ ok: false, reason: "boom" });
        expect(row(summary([]), "format-change").ok).toBeNull();
    });

    it("遅れの区間: 実際の通話の judge-latency は namesUnknown を持ち、テスト用の送信側の場合は持たない。人が見つからないときの行は失敗にせず手で読む理由を残す", () => {
        const real = plan([...REAL, ...RECHECK]);
        expect(real.steps.find((s) => s.kind === "judge-latency")?.namesUnknown).toBe(true);
        const reh = plan([...REHEARSAL, ...RECHECK]);
        expect(reh.steps.find((s) => s.kind === "judge-latency")?.namesUnknown).toBeUndefined();
        const handRead = { name: "judge-latency", ok: null, handRead: true, reason: "枠に載っている人が記録から分からないので、PNG で枠を読み、latency.py を手で実行する" };
        expect(row(summary([handRead]), "latency")).toMatchObject({ ok: null, reason: handRead.reason, detail: { handRead: true } });
    });

    it("自分の枠: 4 つの読み (共有を始めた後・チェックを付けた後・共有を止めた後・チェックを外した後) が全部合格なら合格", () => {
        const all = (oks: boolean[]) => ["self-share", "self-during", "self-stopped", "self-after"].map((label, i) => logRes(label, oks[i]));
        expect(row(summary(all([true, true, true, true])), "self-frame").ok).toBe(true);
        expect(row(summary(all([true, true, true, false])), "self-frame").ok).toBe(false);
        expect(row(summary(all([false, true, true, true])), "self-frame").ok).toBe(false);
        // 途中までしか実行されていなければ決められない。
        expect(row(summary([logRes("self-share", true), logRes("self-during", true)]), "self-frame").ok).toBeNull();
        expect(row(summary([]), "self-frame").ok).toBeNull();
    });

    it("受け取りの継続と抜け戻り: 映像が止まっていなければ合格。数が足りなければ決められない", () => {
        expect(row(summary([rate("mute-minimized", false)]), "receive-continues")).toMatchObject({ ok: true, detail: { receivedHz: [30] } });
        const stopped = summary([rate("mute-minimized", true)]);
        expect(row(stopped, "receive-continues")).toMatchObject({ ok: false });
        expect(row(stopped, "receive-continues").reason).toContain("video stopped");
        expect(row(summary([rate("mute-minimized", null)]), "receive-continues").ok).toBeNull();
        const kept = { name: "judge-connections-kept", label: "vesktop-left", ok: true, closes: 0, hellos: 0 };
        expect(row(summary([kept, rate("vesktop-rejoined", false)]), "rejoin")).toMatchObject({ ok: true, detail: { connectionsKept: { ok: true } } });
        expect(row(summary([{ ...kept, ok: false, reason: "close increased" }, rate("vesktop-rejoined", false)]), "rejoin").ok).toBe(false);
        expect(row(summary([kept]), "rejoin").ok).toBeNull();
    });

    it("version-row の行は --recheck の表にも並ぶ (バージョンの表示を実行するため)", () => {
        expect(row(summary([{ name: "judge-rt", judge: "version-row-shown", ok: true }]), "version-row").ok).toBe(true);
    });
});

describe("ツールの拡張 (dialog-click.ps1 の ComboBox・window-size.ps1 の -ProcessName・obsws.mjs の eventSubscriptions)", () => {
    const text = (name: string) => readFileSync(join(HERE, name), "utf8");

    it("dialog-click.ps1: -ControlType ComboBox と -Select を受け、-Select のない選びは断る。選ぶ前後の値を残す", () => {
        const ps = text("dialog-click.ps1");
        expect(ps).toMatch(/ValidateSet\('Button', 'CheckBox', 'MenuItem', 'ListItem', 'ComboBox', 'TabItem'\)/);
        expect(ps).toMatch(/\[string\]\$Select = ''/);
        expect(ps).toContain("-Select is required for -ControlType ComboBox unless -ListOnly is given");
        // 開いた一覧のウィンドウにだけ、矢印キーと Enter を PostMessage で送る (矢印キーは拡張キーの印つき)。
        expect(ps).toContain("ExpandCollapsePattern");
        expect(ps).toContain("Find-ComboPopup");
        expect(ps).toContain("VK_RETURN");
        expect(ps).toContain("1L << 24");
        for (const field of ["valueBefore", "valueAfter", "comboValue", "items", "selected"]) expect(ps).toContain(field);
        // 前面のウィンドウにキーを送らない。
        expect(ps).not.toMatch(/SendKeys\(|SetForegroundWindow\(/);
        // セルフテストの呼び出しの書き方 (ListItem と同じ) を壊していない。
        expect(ps).toContain("-ListOnly           -> ok:true, found:true");
    });

    it("dialog-click.ps1 の -NamePrefix: 名前の先頭で探せる。実行スクリプトは手順の namePrefix を渡す", () => {
        expect(text("dialog-click.ps1")).toMatch(/\[switch\]\$NamePrefix/);
        expect(text("dialog-click.ps1")).toMatch(/\$NamePrefix -and \$wanted\.Names\[\$i\]\.StartsWith\(\$Button/);
        expect(text("run-window.mjs")).toContain('if (s.namePrefix) cli.push("-NamePrefix")');
    });

    it("window-size.ps1: -ProcessName で一番大きいウィンドウを探し、-Pid とは両立しない。最大化のウィンドウは戻せる", () => {
        const ps = text("window-size.ps1");
        expect(ps).toMatch(/\[string\]\$ProcessName = ''/);
        expect(ps).toContain("FindLargestOfProcesses");
        expect(ps).toContain("pass only one of -Pid and -ProcessName");
        expect(ps).toContain("[switch]$Maximize");
        expect(ps).toContain("IsIconic");
        expect(ps).toMatch(/SWP_NOMOVE \| SWP_NOZORDER \| SWP_NOACTIVATE/);
        // セルフテストに -ProcessName の探し方の確認が入る (CI の windows の job で実行される)。
        expect(ps).toContain("a hidden window is not the largest window of its process");
    });
});


describe("--recheck のリハーサル (テスト用の送信側で手順の並びを通す)", () => {
    const { steps } = plan([...REHEARSAL, "--n-screen", "3", "--n-voice", "3", "--broadcaster-id", "1", "--start-with-add", "--recheck", "--latency-person", "somebody"]);

    it("テスト用の送信側の接続の後に自分のアカウントの待ち (バックアップの名前なし) が入り、メインの手順と AC の判定は入らない", () => {
        const conn = at(steps, (s) => s.kind === "wait-connection");
        expect(steps[conn + 1]).toMatchObject({ kind: "wait-self-account", gate: true });
        expect(steps[conn + 1].baseline).toBeUndefined();
        for (const kind of ["stream-sink-start", "judge-output-skipped", "judge-ac32", "count-operations", "count-roster-labels"]) expect(kinds(steps)).not.toContain(kind);
        expect(steps.filter((s) => s.kind === "record-start").map((s) => s.label)).toEqual([LATENCY_RECORDING_LABEL]);
        expect(kinds(steps)).toContain("harness-stop");
        expect(rosterCheckPlanned(plan([...REHEARSAL, "--broadcaster-id", "1", "--start-with-add", "--recheck"]).args)).toBe(false);
        // リハーサルでは人の操作がすべて秒の待ちに替わる。
        expect(steps.filter((s) => s.kind === "human").every((s) => s.until === "seconds")).toBe(true);
    });

    it("テスト用の送信側には Chrome のウィンドウがないので解像度の変化は入れない。画面共有の音はマーカーの音を乗せた偽の画面で、エンコード・自分の枠・抜け戻りは入る", () => {
        expect(kinds(steps)).not.toContain("resize-window");
        expect(steps.some((s) => s.kind === "judge-log-lines" && String(s.label).startsWith("fmt-"))).toBe(false);
        expect(kinds(steps.slice(steps.findIndex((s) => s.kind === "close-obs")))).not.toContain("collect-recording");
        const off = steps.find((s) => s.kind === "dock-click" && s.muteOp === true && s.desired === "off" && s.textId === "cordmux.dock.row_screen_mute.a11y")!;
        expect(off.textArgs).toEqual(["harness-screen-1"]);
        expect(kinds(steps)).toEqual(expect.arrayContaining(["measure-input-level", "judge-log-lines", "measure-video-rate", "judge-connections-kept"]));
        expect(steps.filter((s) => s.kind === "dialog-click" && s.controlType === "ComboBox")).toHaveLength(3);
        expect(steps.filter((s) => s.kind === "human")).toHaveLength(11);
    });
});

describe("judgeSelfAccount (OBS のログの Facts の行の self=yes の読み)", () => {
    const hello = (time: string) => `${time}: [cordmux] Connected to Vesktop`;
    const facts = (time: string, self: string) => `${time}: [cordmux] Facts: roster=3 sharing=1 warnings=0 stages=- self=${self} unreadable_keys=0`;

    it("接続の後に self=yes が出れば合格。最初の yes までの秒数を残す", () => {
        const lines = [hello("12:00:00.000"), facts("12:00:05.000", "-"), facts("12:00:35.500", "yes"), facts("12:01:05.500", "yes")];
        expect(judgeSelfAccount({ lines })).toMatchObject({ ok: true, connected: true, factsYes: 2, factsDash: 1, firstYesAfterSec: 35.5, yesBeforeConnection: 0 });
    });

    it("self=- だけ、Facts の行が 1 つもない、は失敗 (理由つき)。接続の行がないときは接続がない理由", () => {
        const dashOnly = judgeSelfAccount({ lines: [hello("12:00:00.000"), facts("12:00:05.000", "-"), facts("12:00:35.000", "-")] });
        expect(dashOnly).toMatchObject({ ok: false, factsYes: 0, factsDash: 2, firstYesAfterSec: null });
        expect(dashOnly.reason).toContain("no Facts line with self=yes after the connection (2 lines with self=-)");
        expect(judgeSelfAccount({ lines: [hello("12:00:00.000")] })).toMatchObject({ ok: false, factsYes: 0, factsDash: 0 });
        const none = judgeSelfAccount({ lines: [facts("12:00:05.000", "yes")] });
        expect(none).toMatchObject({ ok: false, connected: false, yesBeforeConnection: 1 });
        expect(none.reason).toContain("no connection line");
        expect(judgeSelfAccount({ lines: [] }).ok).toBe(false);
        expect(judgeSelfAccount().ok).toBe(false);
    });

    it("接続の行より前の self=yes は数えない (前の接続の残りで合格にしない)。保存した数より後の接続から見る", () => {
        const lines = [hello("11:00:00.000"), facts("11:00:10.000", "yes"), "11:30:00.000: [cordmux] Disconnected from Vesktop", hello("12:00:00.000"), facts("12:00:05.000", "-")];
        expect(judgeSelfAccount({ lines, afterHello: 1 })).toMatchObject({ ok: false, factsYes: 0, factsDash: 1, yesBeforeConnection: 1, afterHello: 1 });
        // バックアップが 0 なら最初の接続から見るので、最初の接続の後の yes が数えられる。
        expect(judgeSelfAccount({ lines, afterHello: 0 })).toMatchObject({ ok: true, factsYes: 1 });
        // 保存した数より後の接続がない。
        expect(judgeSelfAccount({ lines, afterHello: 2 })).toMatchObject({ ok: false, connected: false });
        // 読めないバックアップは 0 と同じ。
        expect(judgeSelfAccount({ lines, afterHello: "x" as any }).afterHello).toBe(0);
    });

    it("日付をまたいで時刻が戻っても、最初の yes までの秒数は正になる。似た文字列の行は読まない", () => {
        const lines = [hello("23:59:50.000"), facts("00:00:20.000", "yes")];
        expect(judgeSelfAccount({ lines })).toMatchObject({ ok: true, firstYesAfterSec: 30 });
        const other = [hello("12:00:00.000"), "12:00:01.000: [cordmux] facts: roster=4 speaking=0 sharing=3", "12:00:02.000: [cordmux] Facts: roster=3 self=maybe", "12:00:03.000: Facts: roster=3 self=yes"];
        expect(judgeSelfAccount({ lines: other }).ok).toBe(false);
    });

    it("古い接続の文字列 (日本語の行) も接続として数える", () => {
        const lines = ["12:00:00.000: [cordmux] hello: 合言葉が一致した。接続を確立した", facts("12:00:05.000", "yes")];
        expect(judgeSelfAccount({ lines })).toMatchObject({ ok: true, connected: true });
    });
});

describe("judge-lines の表の絞り方", () => {
    const results = [
        { name: "wait-self-account", ...judgeSelfAccount({ lines: ["12:00:00.000: [cordmux] Connected to Vesktop", "12:00:05.000: [cordmux] Facts: roster=3 self=-"] }) },
        { name: "judge-roster-labels", ...judgeRosterLabels({ lines: ["20:00:00.000: [cordmux] Composite roster label ready: index=0 paint_ms=1", "20:00:00.100: [cordmux] Composite roster shows 2 participant(s)"], baselineCount: 0 }) },
    ] as any;

    it("items を渡すとその項目だけを並べる。渡さなければ全項目", () => {
        const all = judgeLinesSummary(results);
        expect(all.rows.map((r: Step) => r.item)).toEqual(JUDGE_LINES.map((l) => l.item));
        const some = judgeLinesSummary(results, { items: RECHECK_JUDGE_ITEMS });
        expect(some.rows.map((r: Step) => r.item)).toEqual(JUDGE_LINES.map((l) => l.item).filter((i) => RECHECK_JUDGE_ITEMS.includes(i)));
        expect(some.rows.map((r: Step) => r.item)).not.toContain("ac30");
        expect(some.rows.map((r: Step) => r.item)).not.toContain("av-sync-main");
        // 実行していない項目が失敗にならない: 結果にない行は ok が null。
        expect(some.rows.find((r: Step) => r.item === "latency")).toMatchObject({ ok: null });
    });

    it("自分のアカウントが届かなければ表で失敗 (gate の行)。届けば合格。並びは人数が 2 のとき合格", () => {
        const red = judgeLinesSummary(results, { items: RECHECK_JUDGE_ITEMS });
        expect(red.rows.find((r: Step) => r.item === "self-account")).toMatchObject({ ok: false, onFail: "gate", detail: { factsDash: 1, factsYes: 0 } });
        expect(red.red).toEqual(["self-account"]);
        const green = judgeLinesSummary(
            [{ name: "wait-self-account", ...judgeSelfAccount({ lines: ["12:00:00.000: [cordmux] Connected to Vesktop", "12:00:05.000: [cordmux] Facts: roster=3 self=yes"] }) }, results[1]] as any,
            { items: RECHECK_JUDGE_ITEMS },
        );
        expect(green.rows.find((r: Step) => r.item === "self-account")).toMatchObject({ ok: true, detail: { factsYes: 1, firstYesAfterSec: 5 } });
        expect(green.rows.find((r: Step) => r.item === "roster-labels")).toMatchObject({ ok: true, detail: { people: 2 } });
        expect(green.red).toEqual([]);
    });
});
