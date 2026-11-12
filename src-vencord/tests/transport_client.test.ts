// SPDX-License-Identifier: Apache-2.0

// transport/client.ts の状態機械のテスト。接続の手順、hello の応答のタイムアウト、ping への pong、再試行の待ち時間、
// トークンを状態に残さないことを確認する。

import { describe, expect, it } from "vitest";

import { reconnectDelayMs } from "../transport/backoff";
import { decodeControlMessage, type ControlMessage } from "../transport/control";
import {
    createClient,
    transition,
    DEFAULT_HANDSHAKE_TIMEOUT_MS,
    type ClientConfig,
    type ClientState,
} from "../transport/client";

const CONFIG: ClientConfig = { version: "0.1.0", wireVersion: 0 };
const SECRET = "sup3r-secret-value";

function decodeSentText(effects: ReturnType<typeof transition>["effects"]): ControlMessage {
    const sendEffect = effects.find((effect) => effect.kind === "send");
    if (!sendEffect || sendEffect.kind !== "send") throw new Error("send の効果がない (テストの誤り)");
    const decoded = decodeControlMessage(sendEffect.text);
    if (!decoded) throw new Error("送ったテキストを ControlMessage としてデコードできない (テストの誤り)");
    return decoded;
}

describe("createClient", () => {
    it("idle で、failureCount が 0 の状態から始まる", () => {
        expect(createClient()).toEqual({ phase: "idle", failureCount: 0 });
    });
});

describe("接続の手順: idle -> connecting -> handshaking -> established", () => {
    it("idle で tick を受けると connecting になり、read_session を出す", () => {
        const result = transition(CONFIG, createClient(), { kind: "tick" });
        expect(result.state).toEqual({ phase: "connecting", failureCount: 0 });
        expect(result.effects).toEqual([{ kind: "read_session" }]);
    });

    it("connecting で connected(session) を受けると handshaking になり、hello と、タイムアウトの schedule を出す", () => {
        const connecting: ClientState = { phase: "connecting", failureCount: 0 };
        const result = transition(CONFIG, connecting, { kind: "connected", session: { secret: SECRET } });

        expect(result.state).toEqual({ phase: "handshaking", failureCount: 0 });
        expect(decodeSentText(result.effects)).toEqual({
            type: "hello",
            version: CONFIG.version,
            secret: SECRET,
            wire: CONFIG.wireVersion,
        });
        expect(result.effects).toContainEqual({ kind: "schedule", delayMs: DEFAULT_HANDSHAKE_TIMEOUT_MS });
    });

    it("handshaking で message(hello_ok) を受けると established になり、failureCount が 0 に戻る", () => {
        const handshaking: ClientState = { phase: "handshaking", failureCount: 3 };
        const result = transition(CONFIG, handshaking, {
            kind: "message",
            message: { type: "hello_ok", wire: 0, version: "0.1.0" },
        });

        expect(result.state).toEqual({ phase: "established", failureCount: 0 });
        expect(result.effects).toEqual([]);
    });
});

describe("handshaking のタイムアウト (既定は 5 秒)", () => {
    it("handshaking で hello_ok も hello_ng も届かないまま tick を受けたら、hello_ng と同じく close して waiting に入る", () => {
        const handshaking: ClientState = { phase: "handshaking", failureCount: 0 };
        const result = transition(CONFIG, handshaking, { kind: "tick" });

        expect(result.state).toEqual({ phase: "waiting", failureCount: 1 });
        expect(result.effects).toEqual([{ kind: "close" }, { kind: "schedule", delayMs: 0 }]);
    });

    it("handshakeTimeoutMs を指定すると、connected のときの schedule がその値になる", () => {
        const config: ClientConfig = { ...CONFIG, handshakeTimeoutMs: 1234 };
        const connecting: ClientState = { phase: "connecting", failureCount: 0 };
        const result = transition(config, connecting, { kind: "connected", session: { secret: SECRET } });

        expect(result.effects).toContainEqual({ kind: "schedule", delayMs: 1234 });
    });
});

describe("established: ping に pong を返す", () => {
    it("受け取った seq をそのまま入れて pong を送る", () => {
        const established: ClientState = { phase: "established", failureCount: 0 };
        const result = transition(CONFIG, established, { kind: "message", message: { type: "ping", seq: 7 } });

        expect(result.state).toEqual(established);
        expect(decodeSentText(result.effects)).toEqual({ type: "pong", seq: 7 });
    });

    it("clockUs を渡すと、hello と pong を作る直前に読んだ送信側の時計を clock_us に入れる", () => {
        let clock = 1_700_000_000_000_000;
        const config: ClientConfig = { ...CONFIG, clockUs: () => clock++ };
        const connecting: ClientState = { phase: "connecting", failureCount: 0 };
        const hello = transition(config, connecting, { kind: "connected", session: { secret: SECRET } });
        expect(decodeSentText(hello.effects)).toEqual({
            type: "hello",
            version: CONFIG.version,
            secret: SECRET,
            wire: CONFIG.wireVersion,
            clock_us: 1_700_000_000_000_000,
        });
        const established: ClientState = { phase: "established", failureCount: 0 };
        const pong = transition(config, established, { kind: "message", message: { type: "ping", seq: 9 } });
        expect(decodeSentText(pong.effects)).toEqual({ type: "pong", seq: 9, clock_us: 1_700_000_000_000_001 });
    });

    it("ping のほかの制御用の JSON (configure など) では、この状態機械は何もしない", () => {
        const established: ClientState = { phase: "established", failureCount: 0 };
        const result = transition(CONFIG, established, {
            kind: "message",
            message: { type: "configure", streams: [] },
        });

        expect(result.state).toEqual(established);
        expect(result.effects).toEqual([]);
    });
});

describe("hello_ng の後の再試行", () => {
    it("hello_ng では close と schedule を両方出して waiting に進む (失敗が 0 回なので、すぐに試す)", () => {
        const handshaking: ClientState = { phase: "handshaking", failureCount: 0 };
        const result = transition(CONFIG, handshaking, {
            kind: "message",
            message: { type: "hello_ng", reason: "secret mismatch" },
        });

        expect(result.state).toEqual({ phase: "waiting", failureCount: 1 });
        expect(result.effects).toEqual([{ kind: "close" }, { kind: "schedule", delayMs: 0 }]);
    });

    it("2 回目からの hello_ng では、backoff.ts のとおりに間隔が延びる", () => {
        const handshaking: ClientState = { phase: "handshaking", failureCount: 1 };
        const result = transition(CONFIG, handshaking, {
            kind: "message",
            message: { type: "hello_ng", reason: "secret mismatch" },
        });

        expect(result.state).toEqual({ phase: "waiting", failureCount: 2 });
        expect(result.effects).toEqual([{ kind: "close" }, { kind: "schedule", delayMs: reconnectDelayMs(1) }]);
    });

    it("waiting のときに遅れて届いた closed は無視する (schedule を 2 回出さない)", () => {
        const waiting: ClientState = { phase: "waiting", failureCount: 1 };
        const result = transition(CONFIG, waiting, { kind: "closed" });

        expect(result.state).toEqual(waiting);
        expect(result.effects).toEqual([]);
    });

    it("waiting で tick を受けると connecting に戻り、failureCount を変えずに read_session を出す (トークンを読み直す)", () => {
        const waiting: ClientState = { phase: "waiting", failureCount: 1 };
        const result = transition(CONFIG, waiting, { kind: "tick" });

        expect(result.state).toEqual({ phase: "connecting", failureCount: 1 });
        expect(result.effects).toEqual([{ kind: "read_session" }]);
    });
});

describe("切断で再試行の待ちに入る (established、connecting、handshaking で同じ)", () => {
    it("established で closed を受けたときは、直後の 1 回をすぐに試す (failureCount 0 -> delay 0)", () => {
        const established: ClientState = { phase: "established", failureCount: 0 };
        const result = transition(CONFIG, established, { kind: "closed" });

        expect(result.state).toEqual({ phase: "waiting", failureCount: 1 });
        expect(result.effects).toEqual([{ kind: "schedule", delayMs: 0 }]);
    });

    it("connecting で closed を受けたときは、2 回目の失敗として 2 秒になる", () => {
        const connecting: ClientState = { phase: "connecting", failureCount: 1 };
        const result = transition(CONFIG, connecting, { kind: "closed" });

        expect(result.state).toEqual({ phase: "waiting", failureCount: 2 });
        expect(result.effects).toEqual([{ kind: "schedule", delayMs: 2_000 }]);
    });

    it("失敗を繰り返すと、2 秒から 5 秒の上限まで、待ち時間がいつも backoff.ts の値と一致する", () => {
        let state: ClientState = createClient();
        const observedDelays: number[] = [];

        for (let i = 0; i < 6; i++) {
            state = transition(CONFIG, state, { kind: "tick" }).state; // -> connecting
            const closedResult = transition(CONFIG, state, { kind: "closed" }); // すぐに失敗する
            state = closedResult.state;
            const scheduleEffect = closedResult.effects[0];
            if (scheduleEffect?.kind === "schedule") observedDelays.push(scheduleEffect.delayMs);
        }

        expect(observedDelays).toEqual([0, 2_000, 4_000, 5_000, 5_000, 5_000]);
    });
});

describe("established になると failureCount が 0 に戻る (次の切断でも、またすぐに試す)", () => {
    it("一度確立してから切断すると、またすぐに試すところ (delay 0) から始まる", () => {
        let state: ClientState = { phase: "handshaking", failureCount: 4 }; // 何度か失敗した後の状態
        state = transition(CONFIG, state, {
            kind: "message",
            message: { type: "hello_ok", wire: 0, version: "0.1.0" },
        }).state;
        expect(state.failureCount).toBe(0);

        const result = transition(CONFIG, state, { kind: "closed" });
        expect(result.effects).toEqual([{ kind: "schedule", delayMs: 0 }]);
    });
});

describe("トークンを状態に残さない", () => {
    it("connected の後の handshaking の状態を JSON にしても、トークンの文字列は出ない", () => {
        const connecting: ClientState = { phase: "connecting", failureCount: 0 };
        const result = transition(CONFIG, connecting, { kind: "connected", session: { secret: SECRET } });

        expect(JSON.stringify(result.state)).not.toContain(SECRET);
    });
});

describe("想定していないイベントは無視する", () => {
    it("idle のときの message は無視する", () => {
        const idle: ClientState = { phase: "idle", failureCount: 0 };
        const result = transition(CONFIG, idle, {
            kind: "message",
            message: { type: "hello_ok", wire: 0, version: "0.1.0" },
        });
        expect(result).toEqual({ state: idle, effects: [] });
    });

    it("established のときの connected は無視する", () => {
        const established: ClientState = { phase: "established", failureCount: 0 };
        const result = transition(CONFIG, established, { kind: "connected", session: { secret: SECRET } });
        expect(result).toEqual({ state: established, effects: [] });
    });

    it("established のときの tick は無視する (止め損ねた古いタイマー)", () => {
        const established: ClientState = { phase: "established", failureCount: 0 };
        const result = transition(CONFIG, established, { kind: "tick" });
        expect(result).toEqual({ state: established, effects: [] });
    });

    it("handshaking のときに届いた、hello の応答のほかのメッセージは無視する", () => {
        const handshaking: ClientState = { phase: "handshaking", failureCount: 0 };
        const result = transition(CONFIG, handshaking, { kind: "message", message: { type: "ping", seq: 1 } });
        expect(result).toEqual({ state: handshaking, effects: [] });
    });
});
