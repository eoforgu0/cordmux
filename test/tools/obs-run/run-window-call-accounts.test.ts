/*
 * 実際の通話の実行に使うアカウントの名前 (--call-accounts)。名前をコードに書かず、JSON ファイルから受ける形を当てる:
 * ファイルの読み方、実際の通話の実行でファイルがないときの断り、ファイルの名前が実行計画と人への文に入ること、
 * ファイルがないとき (ドライランとテスト) は例の名前で組めること。
 * run-window-plan.mjs は Node の API を呼ばないので、ファイルの中身 (文字列) を渡して確認する。
 */
import { describe, expect, it } from "vitest";

import {
    CALL_ACCOUNT_ROLES,
    DEFAULT_CALL_ACCOUNTS,
    callAccounts,
    callAccountsProblem,
    callJoinMessage,
    encodeRestartShareMessage,
    parseArgs,
    parseCallAccounts,
    planSteps,
    selfFrameShareStartMessage,
    speakerMuteMinimizeMessage,
    speakerUnmuteLeaveMessage,
    vesktopRejoinMessage,
    vesktopStartAfterAddMessage,
} from "./run-window-plan.mjs";

const BASE = ["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-1"];
const REAL = [...BASE, "--real-profile", "--call-window", "--decoder", "software"];
const OTHER = { main: "alice", receiver: "bob", chrome: "carol", firefox: "dave" };

function plan(argv: string[], accounts?: Record<string, string>) {
    const parsed = parseArgs(argv);
    expect(parsed.ok).toBe(true);
    if (accounts) parsed.args.callAccounts = accounts;
    const r = planSteps(parsed.args);
    expect(r.ok).toBe(true);
    return r.steps as Array<Record<string, any>>;
}

describe("--call-accounts: ファイルの読み方", () => {
    it("4 つの役割の名前を読む。前後の空白は落とす", () => {
        const r = parseCallAccounts(JSON.stringify({ main: " alice ", receiver: "bob", chrome: "carol", firefox: "dave" }));
        expect(r.ok).toBe(true);
        expect(r.accounts).toEqual(OTHER);
    });

    it("JSON でない・オブジェクトでない・キーが足りない・知らないキー・同じ名前の重なりは理由つきで断る", () => {
        expect(parseCallAccounts("{").ok).toBe(false);
        expect(parseCallAccounts("[]").ok).toBe(false);
        expect(parseCallAccounts("null").ok).toBe(false);
        const missing = parseCallAccounts(JSON.stringify({ main: "a", receiver: "b", chrome: "c" }));
        expect(missing.ok).toBe(false);
        expect(missing.errors.join("\n")).toContain("firefox");
        const unknown = parseCallAccounts(JSON.stringify({ ...OTHER, extra: "x" }));
        expect(unknown.ok).toBe(false);
        expect(unknown.errors.join("\n")).toContain("extra");
        const dup = parseCallAccounts(JSON.stringify({ ...OTHER, receiver: "alice" }));
        expect(dup.ok).toBe(false);
        expect(dup.errors.join("\n")).toContain("別の名前");
        expect(parseCallAccounts(JSON.stringify({ ...OTHER, main: "" })).ok).toBe(false);
        expect(parseCallAccounts(JSON.stringify({ ...OTHER, main: 1 })).ok).toBe(false);
        expect(parseCallAccounts(JSON.stringify({ ...OTHER, main: "a\nb" })).ok).toBe(false);
    });

    it("例の名前は 4 つの役割を全部持つ", () => {
        expect(Object.keys(DEFAULT_CALL_ACCOUNTS).sort()).toEqual([...CALL_ACCOUNT_ROLES].sort());
        expect(callAccounts({})).toBe(DEFAULT_CALL_ACCOUNTS);
        expect(callAccounts({ callAccounts: OTHER })).toBe(OTHER);
    });
});

describe("--call-accounts: 引数と、実際の通話の実行での必須", () => {
    it("call-1 は値を受ける。値がない・call-1 以外の段は断る", () => {
        const ok = parseArgs([...REAL, "--call-accounts", "accounts.json"]);
        expect(ok.ok).toBe(true);
        expect(ok.args.callAccountsPath).toBe("accounts.json");
        expect(parseArgs([...REAL, "--call-accounts"]).ok).toBe(false);
        const hold = parseArgs(["--obs", "C:/obs", "--out", "C:/out", "--plugin", "C:/plugin", "--step", "call-hold", "--real-profile", "--call-window", "--call-accounts", "a.json"]);
        expect(hold.ok).toBe(false);
    });

    it("実際の通話の実行で --call-accounts がなければ止める。ドライランとリハーサルは要らない", () => {
        expect(callAccountsProblem(parseArgs(REAL).args)).toContain("--call-accounts");
        expect(callAccountsProblem(parseArgs([...REAL, "--dry-run"]).args)).toBeNull();
        expect(callAccountsProblem(parseArgs([...BASE, "--rehearsal"]).args)).toBeNull();
        const given = parseArgs([...REAL, "--call-accounts", "a.json"]).args;
        expect(callAccountsProblem(given)).toContain("--call-accounts");
        given.callAccounts = OTHER;
        expect(callAccountsProblem(given)).toBeNull();
    });
});

describe("--call-accounts: 実行計画と人への文の名前", () => {
    const everything = (steps: Array<Record<string, any>>) => JSON.stringify(steps);

    const RECHECK = [...REAL, "--n-screen", "3", "--n-voice", "3", "--broadcaster-id", "1", "--start-with-add", "--recheck", "--voice-channel", "lobby", "--latency-person", "x"];
    const EXTRA = [...REAL, "--n-screen", "3", "--n-voice", "3", "--broadcaster-id", "1", "--start-with-add", "--extra-checks", "--voice-channel", "lobby", "--latency-person", "x"];

    it("ファイルの名前で組むと、例の名前は 1 つも残らない", () => {
        for (const argv of [RECHECK, EXTRA]) {
            const text = everything(plan(argv, OTHER));
            for (const name of Object.values(OTHER)) expect(text).toContain(name);
            for (const example of [DEFAULT_CALL_ACCOUNTS.receiver, DEFAULT_CALL_ACCOUNTS.chrome, DEFAULT_CALL_ACCOUNTS.firefox]) expect(text).not.toContain(example);
            expect(text).not.toContain("Discord (" + DEFAULT_CALL_ACCOUNTS.main + ")");
            expect(text).not.toContain('"name":"' + DEFAULT_CALL_ACCOUNTS.main + '"');
        }
    });

    it("ファイルがなければ例の名前で組む (ドライランとテスト)", () => {
        for (const argv of [RECHECK, EXTRA]) {
            const text = everything(plan(argv));
            for (const example of [DEFAULT_CALL_ACCOUNTS.receiver, DEFAULT_CALL_ACCOUNTS.chrome, DEFAULT_CALL_ACCOUNTS.firefox]) expect(text).toContain(example);
            expect(text).toContain("Discord (" + DEFAULT_CALL_ACCOUNTS.main + ")");
        }
    });

    it("手順の数は名前に依らない", () => {
        for (const argv of [RECHECK, EXTRA]) expect(plan(argv, OTHER)).toHaveLength(plan(argv).length);
    });

    it("人への文は、渡された名前で組む", () => {
        const a = OTHER;
        expect(vesktopStartAfterAddMessage(a)).toContain("Vesktop (bob)");
        expect(encodeRestartShareMessage("lobby", a)).toContain("Firefox (dave)");
        expect(encodeRestartShareMessage("lobby", a)).toContain("Chrome (carol)");
        expect(encodeRestartShareMessage("lobby", a)).toContain("Discord (alice)");
        expect(selfFrameShareStartMessage("lobby", a)).toContain("Discord (alice)");
        expect(speakerMuteMinimizeMessage("lobby", a)).toContain("Vesktop (bob)");
        expect(speakerUnmuteLeaveMessage("lobby", a)).toContain("Vesktop (bob)");
        expect(vesktopRejoinMessage("lobby", a)).toContain("Vesktop (bob)");
        const join = callJoinMessage({ vesktopShare: true, voiceChannel: "lobby", recheck: true, accounts: a });
        for (const n of ["Discord (alice)", "Firefox (dave)", "Chrome (carol)", "Vesktop (bob)"]) expect(join).toContain(n);
    });

    it("役割と手順の対応: 声の行・画面共有の音・使う声の置き換え・外して戻す人", () => {
        const steps = plan(EXTRA, OTHER);
        // 声のミュートは Chrome と Firefox のアカウント (メインアカウントの声は既定でミュート)。
        const voiceMutes = steps.filter((s) => s.kind === "dock-click" && s.textId === "cordmux.dock.row_voice_mute.a11y").map((s) => s.textArgs[0]);
        expect(new Set(voiceMutes)).toEqual(new Set(["carol", "dave"]));
        // 画面共有の音: テスト動画の人 (Chrome) のミュートを外し、ほかの共有している人 (メインと Firefox) はミュートにする (出ていなければ失敗にしない)。
        const screenMutes = steps.filter((s) => s.kind === "dock-click" && s.textId === "cordmux.dock.row_screen_mute.a11y");
        expect(new Set(screenMutes.filter((s) => s.desired === "off").map((s) => s.textArgs[0]))).toEqual(new Set(["carol"]));
        expect(new Set(screenMutes.filter((s) => s.absentOk === true).map((s) => s.textArgs[0]))).toEqual(new Set(["alice", "dave"]));
        const swap = steps.find((s) => s.kind === "human" && String(s.message).includes("の行の [...]"));
        expect(String(swap?.message)).toContain("daveの行の [...]");
        expect(String(swap?.message)).toContain("「carol」を選んで");
        const ac30 = steps.filter((s) => s.kind === "dock-click" && s.slotOp === true).map((s) => s.name);
        expect(ac30).toEqual(["dave", "dave"]);
        const self = plan(RECHECK, OTHER).filter((s) => s.kind === "dock-click" && s.name === "alice");
        expect(self.length).toBeGreaterThan(0);
    });
});
