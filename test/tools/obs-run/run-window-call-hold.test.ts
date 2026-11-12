/*
 * 実機の実行の準備として、次をテストする。
 *   - --step call-hold: セル C (実際の Vesktop と Discord) を 1 ストリームで実行する実行計画。call-gate-a の初期設定と
 *     [Vesktopに追加] -> 開き直した Vesktop の接続 -> 通知ファイルまで待つ -> call-1 の終わりの手順 (アンインストールで戻す)。
 *     --rehearsal は同じ並びを偽の state.json とテスト用の送信側で実行する。
 *   - 人が obs-websocket で手で録った OBS の録画を残す (unlabeledObsRecordings)。
 *   - hold と call-hold のリハーサルのテスト用の送信側に、フレームだけを止める通知ファイルの場所を渡す。
 *     読むのはテスト用の送信側 (ハーネスの API) なので、環境変数の名前が送信側にあることも確認する。
 * run-window-plan.mjs は Node の API を呼ばないので、テストする対象には値だけを渡す。
 */
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

import {
    CALL_GATE_CONNECT_WAIT_TIMEOUT_MS,
    CALL_HOLD_REHEARSAL_N_SCREEN,
    CALL_HOLD_REHEARSAL_N_VOICE,
    DEFAULT_DEADLINE_MIN,
    FRAMES_PAUSE_FILE_PREFIX,
    FRAMES_PAUSE_FILE_SUFFIX,
    HARNESS_FRAMES_PAUSE_ENV,
    HOLD_DEFAULT_MINUTES,
    OBS_RECORDING_EXTENSIONS,
    STATE_JSON_BEFORE_NAME,
    STEPS_KEEPING_OBS_RECORDINGS,
    STEPS_KEEPING_RECORDINGS,
    STEPS_KEEPING_UNLABELED_OBS_RECORDINGS,
    TEXT_ID_ADD_TO_VESKTOP,
    VESKTOP_HANG_UP_AND_QUIT_MESSAGE,
    VESKTOP_HIDDEN_ICON_HINT,
    VESKTOP_START_AFTER_ADD_MESSAGE,
    VESKTOP_START_AFTER_UNINSTALL_MESSAGE,
    VESKTOP_START_FOR_CALL_ONE_MESSAGE,
    callHoldHarnessCounts,
    vesktopQuitAndAddMessage,
    deadlineEstimate,
    deadlineMinFor,
    parseArgs,
    planSteps,
    unlabeledObsRecordings,
} from "./run-window-plan.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = join(HERE, "..", "..", "..");
const BASE = ["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin"];
const REAL = [...BASE, "--step", "call-hold", "--real-profile", "--call-window", "--decoder", "software"];
const REHEARSAL = [...BASE, "--step", "call-hold", "--rehearsal"];

function plan(argv: string[]) {
    const parsed = parseArgs(argv);
    expect(parsed.errors).toEqual([]);
    const r = planSteps(parsed.args);
    expect(r.ok).toBe(true);
    return { args: parsed.args, steps: r.steps as Array<Record<string, any>> };
}

function kinds(steps: Array<Record<string, any>>) {
    return steps.map((s) => s.kind);
}

describe("--step call-hold の引数", () => {
    it("実際の通話は --real-profile が要り、--call-window と --hold-minutes を受ける", () => {
        expect(parseArgs([...BASE, "--step", "call-hold"]).ok).toBe(false);
        const r = parseArgs([...REAL, "--hold-minutes", "45"]);
        expect(r.errors).toEqual([]);
        expect(r.args.holdMinutes).toBe(45);
        expect(r.args.callWindow).toBe(true);
    });

    it("hold だけのオプション (--no-seed と --screen-sizes) は断る", () => {
        expect(parseArgs([...REAL, "--no-seed"]).errors.join(" ")).toContain("--no-seed は --step hold だけ");
        expect(parseArgs([...REHEARSAL, "--n-screen", "1", "--screen-sizes", "1280x720"]).errors.join(" ")).toContain("--screen-sizes は --step hold だけ");
    });

    it("実際の通話はテスト用の送信側の数を受けない (送信側は利用者の Vesktop だけ)。リハーサルは受ける", () => {
        expect(parseArgs([...REAL, "--n-screen", "1"]).ok).toBe(false);
        expect(parseArgs([...REAL, "--n-voice", "1"]).ok).toBe(false);
        expect(parseArgs([...REHEARSAL, "--n-screen", "2", "--n-voice", "1"]).ok).toBe(true);
    });

    it("リハーサルは --real-profile と --call-window と両立しない", () => {
        expect(parseArgs([...REHEARSAL, "--real-profile"]).ok).toBe(false);
        expect(parseArgs([...REHEARSAL, "--call-window"]).ok).toBe(false);
    });

    it("期限は見積もり (開いたまま待つ分と終わりの人の操作) と既定 120 分の大きいほう。渡せばその値", () => {
        const long = plan([...REAL, "--hold-minutes", "100"]);
        const est = deadlineEstimate(long.steps, { humanOverrunMin: 10 }).minutes;
        expect(est).toBeGreaterThan(100);
        expect(deadlineMinFor(long.args, long.steps)).toBe(Math.max(DEFAULT_DEADLINE_MIN, est));
        const short = plan([...REAL, "--hold-minutes", "10"]);
        expect(deadlineMinFor(short.args, short.steps)).toBe(DEFAULT_DEADLINE_MIN);
        const given = plan([...REAL, "--deadline-min", "200"]);
        expect(deadlineMinFor(given.args, given.steps)).toBe(200);
    });
});

describe("--step call-hold の実行計画 (実際の通話)", () => {
    const { steps } = plan(REAL);

    it("初期設定は実際の保存先のまま、ours・state.json の書き込みを許す・誰を枠に入れるかは auto", () => {
        expect(steps[0]).toMatchObject({ kind: "seed-config", step: "call-hold", useAppData: false, realProfile: true, dockInstall: "ours", allowStateJsonWrite: true, assignMode: "auto" });
    });

    it("並び: コピー -> 人が Vesktop を終了して [Vesktopに追加] -> 判定 -> 保存 -> hello の数 -> 人が起動する -> 接続 -> スクリーンショット -> hold-info -> 待つ -> 終わりの手順 -> 閉じる -> 録画を残す", () => {
        const k = kinds(steps);
        const order = [
            "copy-state-json",
            "human",
            "read-state-json",
            "judge-vencord-dir",
            "check-file-exists",
            "count-connections",
            "human",
            "wait-connection",
            "screenshot-window",
            "hold-info",
            "human",
            "collect-plugin-log",
            "human",
            "capture-backup",
            "human",
            "read-state-json",
            "judge-vencord-dir-restored",
            "human",
            "close-obs",
            "keep-obs-recordings",
            "write-result",
            "summarize",
            "cleanup",
        ];
        const picked = k.filter((x) => order.includes(x));
        expect(picked).toEqual(order);
        // テスト用の送信側は起動しない。
        expect(k).not.toContain("harness-start");
        expect(k).not.toContain("dock-click");
    });

    it("最初のコピーは call-gate-a と同じ名前 (judge-vencord-dir が他のキーを比べる相手)", () => {
        expect(steps.find((s) => s.kind === "copy-state-json")).toMatchObject({ destName: STATE_JSON_BEFORE_NAME });
    });

    it("人へのメッセージは Vesktop の終了と [Vesktopに追加] (ini の id) を 1 つの依頼にし、state.json が配置したフォルダーを指すまで待つ (gate)", () => {
        // [Vesktopに追加] は Vesktop の起動中は押せないので、終了を先に依頼する (手順書のインストールの 3 と同じ並び)。
        const add = steps.find((s) => s.kind === "human")!;
        expect(add.until).toBe("state-json-points-to-placed");
        expect(add.message).toBe(vesktopQuitAndAddMessage());
        expect(add.message).toContain("{{" + TEXT_ID_ADD_TO_VESKTOP + "}}");
        expect(add.message.indexOf("[Quit]")).toBeLessThan(add.message.indexOf("{{" + TEXT_ID_ADD_TO_VESKTOP + "}}"));
        expect(add.gate).toBe(true);
        expect(add.expectedDir).toBe("C:/obs/plugins/cordmux/data/vencord");
    });

    it("接続の待ちは開き直す前に保存した数より増えたときだけ通る (gate)", () => {
        const count = steps.find((s) => s.kind === "count-connections")!;
        const wait = steps.find((s) => s.kind === "wait-connection")!;
        expect(wait.baseline).toBe(count.label);
        expect(wait.timeoutMs).toBe(CALL_GATE_CONNECT_WAIT_TIMEOUT_MS);
        expect(wait.gate).toBe(true);
    });

    it("開いたまま待つのは通知ファイルまでで、制限時間は --hold-minutes (gate にしない = 制限時間でも終わりの手順に進む)", () => {
        const holdIndex = steps.findIndex((s) => s.kind === "hold-info");
        const hold = steps[holdIndex + 1];
        expect(hold).toMatchObject({ kind: "human", until: "go-file", timeoutMs: HOLD_DEFAULT_MINUTES * 60 * 1000 });
        expect(hold.gate).toBeUndefined();
    });

    it("終わりの手順は call-1 と同じ: Vesktop を閉じる -> バックアップ -> [ツール] からアンインストール (state.json が戻るまで) -> 判定 -> 開き直す", () => {
        const restored = steps.find((s) => s.kind === "judge-vencord-dir-restored")!;
        expect(restored.gate).toBe(true);
        const uninstall = steps.find((s) => s.kind === "human" && s.until === "state-json-restored")!;
        expect(uninstall.gate).toBe(true);
        expect(steps.filter((s) => s.kind === "human").at(-1)!.until).toBe("go-file");
    });

    it("OBS の録画と cordmux の録画のフォルダーを残す (セル C の録画を border_timing.py が読む)", () => {
        expect(STEPS_KEEPING_OBS_RECORDINGS).toContain("call-hold");
        expect(STEPS_KEEPING_UNLABELED_OBS_RECORDINGS).toEqual(["call-hold"]);
        expect(STEPS_KEEPING_RECORDINGS).toContain("call-hold");
        expect(steps.at(-1)).toMatchObject({ kind: "cleanup", callWindow: false, keepRecordings: true });
    });
});

describe("Vesktop の終了と起動を依頼するメッセージ", () => {
    // Vesktop の Minimize to tray は既定で有効で、ウィンドウの閉じるボタンではトレイに残り Vesktop.exe が動き続ける。終了は通知領域の
    // アイコンの [Quit] (英語のまま)。閉じるだけでは wait-connection の新しい hello が来ず、アンインストールも断られる。
    const humans = (argv: string[]) => plan(argv).steps.filter((s) => s.kind === "human").map((s) => String(s.message));

    it("メッセージの文面 ([Quit] を依頼する 4 つに、隠れたアイコンの 1 文を添える)", () => {
        expect(vesktopQuitAndAddMessage()).toBe(
            "タスクバーの右端の通知領域にあるVesktopのアイコンを右クリックし、[Quit] を選んで、Vesktopを終了してください。アイコンが見当たらないときは、通知領域の [^] を押すと表示されます。Vesktopが終了したら、テスト用のOBSのドックにある [{{cordmux.dock.add_to_vesktop.button}}] を押してください (Vesktopの起動中は押せず、ボタンの下に理由が出ます)。確認のウィンドウが出たら [{{cordmux.dialog.switch.button}}] を押してください",
        );
        expect(VESKTOP_START_AFTER_ADD_MESSAGE).toBe("Vesktopを起動してください (cordmuxを読み込んで起動します)");
        expect(VESKTOP_HANG_UP_AND_QUIT_MESSAGE).toBe(
            "画面共有を止めて通話を切り、通知領域のVesktopのアイコンを右クリックして [Quit] を選んでください (テスト用のOBSのウィンドウはまだ閉じないでください)。アイコンが見当たらないときは、通知領域の [^] を押すと表示されます",
        );
        expect(VESKTOP_START_FOR_CALL_ONE_MESSAGE).toBe(
            "Vesktopを起動してください (まだ起動したままなら、通知領域のVesktopのアイコンを右クリックして [Quit] を選び、終了してから起動してください。アイコンが見当たらないときは、通知領域の [^] を押すと表示されます)",
        );
        expect(VESKTOP_START_AFTER_UNINSTALL_MESSAGE).toBe("Vesktopを起動してください (元の設定で動きます)");
    });

    it("隠れたアイコンの 1 文は guide/ja.md と同じ文面で、[Quit] を依頼するどのメッセージにも入る", () => {
        // 手順書の空白の置き方 (「の」と「[^]」の間の空白) に左右されないよう、空白と改行を両方から除いて探す。
        const squash = (t: string) => t.replace(/\s+/g, "");
        const guide = squash(readFileSync(join(REPO, "guide", "ja.md"), "utf8"));
        expect(guide).toContain(squash(VESKTOP_HIDDEN_ICON_HINT + "。"));
        expect(VESKTOP_HIDDEN_ICON_HINT).toBe("アイコンが見当たらないときは、通知領域の [^] を押すと表示されます");
        const call1 = ["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-1", "--real-profile", "--call-window", "--decoder", "software"];
        const quitMessages = [...humans(REAL), ...humans(call1)].filter((m) => m.includes("[Quit]"));
        expect(quitMessages.length).toBeGreaterThanOrEqual(3);
        for (const m of quitMessages) expect(m).toContain(VESKTOP_HIDDEN_ICON_HINT);
        // [Quit] を依頼しないメッセージ (アンインストールの後の起動) には添えない。
        expect(VESKTOP_START_AFTER_UNINSTALL_MESSAGE).not.toContain(VESKTOP_HIDDEN_ICON_HINT);
    });

    it("call-hold の実際の通話: 終了と追加 -> 起動 -> 終わりの手順での終了 -> 起動の 4 つがこの順に出る", () => {
        const m = humans(REAL);
        const quitAdd = m.indexOf(vesktopQuitAndAddMessage());
        const startAfterAdd = m.indexOf(VESKTOP_START_AFTER_ADD_MESSAGE);
        const hangUp = m.indexOf(VESKTOP_HANG_UP_AND_QUIT_MESSAGE);
        const start = m.indexOf(VESKTOP_START_AFTER_UNINSTALL_MESSAGE);
        expect(quitAdd).toBe(0);
        expect(startAfterAdd).toBeGreaterThan(quitAdd);
        expect(hangUp).toBeGreaterThan(startAfterAdd);
        expect(start).toBeGreaterThan(hangUp);
    });

    it("call-hold と call-1 のどのメッセージも Vesktop を「閉じて」「開き直して」と書かない (終了は通知領域の [Quit])", () => {
        const call1 = ["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-1", "--real-profile", "--call-window", "--decoder", "software"];
        for (const m of [...humans(REAL), ...humans(call1)]) {
            expect(m).not.toContain("Vesktopを閉じ");
            expect(m).not.toContain("Vesktopを開き直");
            if (m.includes("[Quit]")) {
                expect(m).toContain("通知領域");
                expect(m).toContain("Vesktopのアイコンを右クリック");
            }
        }
        expect(humans(call1)).toContain(VESKTOP_START_FOR_CALL_ONE_MESSAGE);
        expect(humans(call1)).toContain(VESKTOP_HANG_UP_AND_QUIT_MESSAGE);
        expect(humans(call1)).toContain(VESKTOP_START_AFTER_UNINSTALL_MESSAGE);
    });

    it("人に見せるメッセージでは、英数字と日本語の間に半角の空白を入れない ([Quit] の括弧の後の空白は、画面の文字列を指定する書き方)", () => {
        for (const m of [vesktopQuitAndAddMessage(), VESKTOP_START_AFTER_ADD_MESSAGE, VESKTOP_HANG_UP_AND_QUIT_MESSAGE, VESKTOP_START_AFTER_UNINSTALL_MESSAGE, VESKTOP_START_FOR_CALL_ONE_MESSAGE]) {
            expect(m).not.toMatch(/[A-Za-z0-9] [\u3041-\u3093\u30a1-\u30f6\u4e00-\u9fa0]|[\u3041-\u3093\u30a1-\u30f6\u4e00-\u9fa0] [A-Za-z0-9]/);
        }
    });
});

describe("--step call-hold --rehearsal", () => {
    const { args, steps } = plan(REHEARSAL);

    it("偽の state.json (absent) と、配置した Vencord のコピーを使い、[Vesktopに追加] は実行スクリプトが押す", () => {
        expect(steps[0]).toMatchObject({ kind: "seed-config", useAppData: true, rehearsal: true, dockInstall: "absent", assignMode: "auto" });
        expect(kinds(steps)).toContain("place-vencord-bundle");
        const click = steps.find((s) => s.kind === "dock-click")!;
        expect(click).toMatchObject({ textId: TEXT_ID_ADD_TO_VESKTOP, gate: true });
    });

    it("人の操作は待つ 1 つだけ (開き直す・閉じる・アンインストールは実行スクリプトが代わる)", () => {
        const humans = steps.filter((s) => s.kind === "human");
        expect(humans).toHaveLength(1);
        expect(humans[0].until).toBe("go-file");
    });

    it("テスト用の送信側は既定で画面 1 ストリーム。フレームを止める通知ファイルを渡し、待ちの後に止める", () => {
        expect(callHoldHarnessCounts(args)).toEqual({ nScreen: CALL_HOLD_REHEARSAL_N_SCREEN, nVoice: CALL_HOLD_REHEARSAL_N_VOICE, any: true });
        const start = steps.find((s) => s.kind === "harness-start")!;
        expect(start).toMatchObject({ mode: "product", nScreen: 1, nVoice: 0, framesPause: true });
        const k = kinds(steps);
        expect(k.indexOf("harness-start")).toBeLessThan(k.indexOf("wait-connection"));
        expect(k.indexOf("harness-stop")).toBeGreaterThan(k.indexOf("hold-info"));
        expect(k.indexOf("harness-stop")).toBeLessThan(k.indexOf("capture-backup"));
    });

    it("実際の通話ではないので callHoldHarnessCounts は any が false を返す", () => {
        expect(callHoldHarnessCounts(plan(REAL).args).any).toBe(false);
    });
});

describe("フレームだけを止める通知ファイルの受け渡し", () => {
    it("hold のテスト用の送信側にも framesPause が立つ (送信側を接続しない hold には harness-start がない)", () => {
        const withHarness = plan([...BASE, "--step", "hold", "--n-screen", "1"]).steps;
        expect(withHarness.find((s) => s.kind === "harness-start")).toMatchObject({ framesPause: true });
        expect(plan([...BASE, "--step", "hold"]).steps.some((s) => s.kind === "harness-start")).toBe(false);
    });

    it("名前と保存先の形", () => {
        expect(HARNESS_FRAMES_PAUSE_ENV).toBe("HARNESS_FRAMES_PAUSE_FILE");
        expect(FRAMES_PAUSE_FILE_PREFIX + "h3" + FRAMES_PAUSE_FILE_SUFFIX).toBe("frames-pause-h3.flag");
    });

    it("実行スクリプトは同じ名前で渡し、hold-info に書き、finally で消す", () => {
        const source = readFileSync(join(HERE, "run-window.mjs"), "utf8");
        expect(source).toContain("harnessEnv[HARNESS_FRAMES_PAUSE_ENV] = step.extra.framesPauseFile");
        expect(source).toContain("framesPauseFile: state.harnessChild && state.framesPauseFile ? state.framesPauseFile : null");
        const finallyBlock = source.slice(source.lastIndexOf("} finally {"));
        expect(finallyBlock).toContain("fs.unlinkSync(state.framesPauseFile)");
    });

    it("テスト用の送信側 (ハーネスの API) が同じ名前を読む", () => {
        const harnessDir = join(REPO, "test", "tools", "harness");
        const text = ["preload.js", "main.js", "sender.ts"].map((f) => readFileSync(join(harnessDir, f), "utf8")).join("\n");
        expect(text).toContain(HARNESS_FRAMES_PAUSE_ENV);
    });
});

describe("unlabeledObsRecordings (人が手で録った OBS の録画)", () => {
    const since = 1_000_000;
    const entries = [
        { name: "rec-b.mkv", path: "C:/out/window3b/rec-b.mkv", isFile: true, mtimeMs: since + 10 },
        { name: "rec-a.mkv", path: "C:/out/window3b/rec-a.mkv", isFile: true, mtimeMs: since + 5 },
        { name: "old.mkv", path: "C:/out/window3b/old.mkv", isFile: true, mtimeMs: since - 1 },
        { name: "main.mkv", path: "C:/out/window3b/main.mkv", isFile: true, mtimeMs: since + 1 },
        { name: "run-x.log", path: "C:/out/window3b/run-x.log", isFile: true, mtimeMs: since + 1 },
        { name: "recordings", path: "C:/out/window3b/recordings", isFile: false, mtimeMs: since + 1 },
    ];

    it("実行の開始より後の動画だけを、名前の順に extra-<n> で選ぶ。label 付きで記録した物と動画でない物は選ばない", () => {
        const picked = unlabeledObsRecordings({ entries, known: ["C:\\out\\window3b\\main.mkv"], sinceMs: since });
        expect(picked).toEqual([
            { label: "extra-1", from: "C:/out/window3b/rec-a.mkv" },
            { label: "extra-2", from: "C:/out/window3b/rec-b.mkv" },
        ]);
    });

    it("拡張子は OBS の録画のもの (大文字も)", () => {
        expect(OBS_RECORDING_EXTENSIONS).toContain(".mkv");
        const picked = unlabeledObsRecordings({ entries: [{ name: "A.MP4", path: "C:/o/A.MP4", isFile: true, mtimeMs: 5 }], known: [], sinceMs: 0 });
        expect(picked).toHaveLength(1);
    });

    it("空や不正な入力でも例外を投げない", () => {
        expect(unlabeledObsRecordings({ entries: undefined as any, known: undefined as any, sinceMs: 0 })).toEqual([]);
        expect(unlabeledObsRecordings({ entries: [null, { name: 1 }] as any, known: [], sinceMs: 0 })).toEqual([]);
    });
});
