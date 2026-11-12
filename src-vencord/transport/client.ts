// SPDX-License-Identifier: Apache-2.0

// 受信側との接続の手順を表す、純粋関数の状態機械。idle -> connecting -> handshaking (hello を送った後) ->
// established と進み、hello_ng、hello の応答のタイムアウト、切断のときは waiting (再試行の待ち) に入る。
// established の間は ping に pong を返す。WebSocket には触らず、送る、閉じる、トークンファイルを読み直す、
// タイマーを設定する、の 4 つを ClientEffect として返す。実行するのは transport/socket.ts である。時計は
// ClientConfig の clockUs から読むだけで、渡されなければ読まない。

import { reconnectDelayMs } from "./backoff";
import { type ControlMessage, encodeControlMessage } from "./control";

/**
 * hello を送ってから hello_ok か hello_ng を待つ時間の既定値 (ms)。ハートビートの間隔と同じ値にした目安の値で、
 * 測って決めたものではない。この時間を過ぎたら、hello_ng を受けたときと同じく、接続を閉じて再試行を待つ。
 */
export const DEFAULT_HANDSHAKE_TIMEOUT_MS = 5_000;

/** hello に入れる値のうち、再接続しても変わらないもの。 */
export interface ClientConfig {
    /** hello の version。送信側のバージョン (version.ts の CORDMUX_BUILD_ID)。 */
    readonly version: string;
    /** hello の wire。ワイヤー形式のバージョン。呼び出し元が決める。 */
    readonly wireVersion: number;
    /**
     * hello の vesktop。Vesktop のバージョンで、受信側は表示にだけ使う。読めなかったときは undefined にし、hello に
     * キーを入れない (値は vesktop_version.ts が読む)。
     */
    readonly vesktopVersion?: string;
    /**
     * 送信側の時計 (encode/sender_clock.ts の us。整数)。hello と pong を作る直前に読み、clock_us に入れる。受信側は、
     * 受け取った時刻との差の最小値から、送信側の時計と OBS の時計の差を決める (docs/wire_format.md の「ping と pong の
     * 扱い」)。省いたときは clock_us を入れない。
     */
    readonly clockUs?: () => number;
    /** handshaking のタイムアウト (ms)。省略したときは DEFAULT_HANDSHAKE_TIMEOUT_MS。 */
    readonly handshakeTimeoutMs?: number;
}

/** 接続を試すたびにトークンファイルから読み直すトークン。 */
export interface Session {
    readonly secret: string;
}

export type ClientPhase = "idle" | "connecting" | "handshaking" | "established" | "waiting";

export interface ClientState {
    readonly phase: ClientPhase;
    /** 最後に hello_ok を受けてから、続けて失敗した回数 (backoff.ts の priorFailureCount に渡す)。 */
    readonly failureCount: number;
}

export type ClientEvent =
    /**
     * 呼び出し元が read_session に従って接続を開いたことを伝える。トークンは hello を作るときにだけ使い、
     * ClientState には持たない。状態を JSON にしたときに、トークンが現れないようにするためである。
     */
    | { kind: "connected"; session: Session }
    /**
     * ソケットに届いた制御用の JSON を、transport/control.ts の decodeControlMessage() で読んだもの。
     * 読めなかった入力は、呼び出し元がイベントにせずに捨てる。
     */
    | { kind: "message"; message: ControlMessage }
    /** ソケットが閉じた (相手が閉じた、エラー、read_session の失敗を含む)。 */
    | { kind: "closed" }
    /** schedule で指定した時間がたった (呼び出し元のタイマーから)。 */
    | { kind: "tick" };

export type ClientEffect =
    /** このテキストを、制御用の JSON のメッセージとして送る。 */
    | { kind: "send"; text: string }
    /** 今開いている接続を閉じる。 */
    | { kind: "close" }
    /**
     * トークンファイルを読み直し、その場で新しい接続を試す。読み直す関数は呼び出し元が持つ。読めなかったときは、
     * 呼び出し元が closed のイベントを返す。
     */
    | { kind: "read_session" }
    /**
     * delayMs ms の後に tick を 1 回返す。この状態機械は、動いているタイマーが 1 つだけであることを前提にし、
     * 古いタイマーから届いた tick を見分けない。呼び出し元は、新しい schedule を受けたときと established に
     * 入ったときに、前のタイマーを止める。
     */
    | { kind: "schedule"; delayMs: number };

export interface ClientTransitionResult {
    state: ClientState;
    effects: ClientEffect[];
}

/** 状態機械を初期状態 (idle。まだ何も試していない) で作る。 */
export function createClient(): ClientState {
    return { phase: "idle", failureCount: 0 };
}

function buildHello(config: ClientConfig, session: Session): ClientEffect {
    const message: Extract<ControlMessage, { type: "hello" }> = {
        type: "hello",
        version: config.version,
        secret: session.secret,
        wire: config.wireVersion,
    };
    // 省略できるキーは、値があるときだけ入れる (値が undefined のキーを作らない)。
    if (config.vesktopVersion !== undefined) message.vesktop = config.vesktopVersion;
    // 時計は送る直前に読む (読んでから送るまでの時間が短いほど、受信側の差の見積もりに入る遅れが小さい)。
    if (config.clockUs !== undefined) message.clock_us = config.clockUs();
    return { kind: "send", text: encodeControlMessage(message) };
}

function enterWaiting(failureCount: number): ClientTransitionResult {
    const delayMs = reconnectDelayMs(failureCount);
    return {
        state: { phase: "waiting", failureCount: failureCount + 1 },
        effects: [{ kind: "schedule", delayMs }],
    };
}

/**
 * イベントを 1 つ処理し、次の状態と、呼び出し元が実行する ClientEffect の一覧を返す。この関数は何も実行しない。
 * established の間に届いた ping 以外の制御用の JSON (configure、keyframe_request など) は、接続の手順に関わらない
 * ので何もしない。呼び出し元は、同じメッセージを自分で処理してよい。
 */
export function transition(config: ClientConfig, state: ClientState, event: ClientEvent): ClientTransitionResult {
    switch (event.kind) {
        case "tick": {
            if (state.phase === "idle" || state.phase === "waiting") {
                return {
                    state: { phase: "connecting", failureCount: state.failureCount },
                    effects: [{ kind: "read_session" }],
                };
            }
            if (state.phase === "handshaking") {
                // hello を送ってから handshakeTimeoutMs の間、hello_ok も hello_ng も届かなかった。
                // hello_ng を受けたときと同じく、接続を閉じて再試行を待つ。
                const { state: nextState, effects } = enterWaiting(state.failureCount);
                return { state: nextState, effects: [{ kind: "close" }, ...effects] };
            }
            // established のときの tick は、止め損ねた古いタイマーから届いたもの。無視する。
            return { state, effects: [] };
        }

        case "connected": {
            if (state.phase !== "connecting") {
                // connecting のとき以外の接続の通知は想定していないので、無視する。
                return { state, effects: [] };
            }
            const handshakeTimeoutMs = config.handshakeTimeoutMs ?? DEFAULT_HANDSHAKE_TIMEOUT_MS;
            return {
                state: { phase: "handshaking", failureCount: state.failureCount },
                effects: [buildHello(config, event.session), { kind: "schedule", delayMs: handshakeTimeoutMs }],
            };
        }

        case "message": {
            if (state.phase === "handshaking") {
                if (event.message.type === "hello_ok") {
                    return { state: { phase: "established", failureCount: 0 }, effects: [] };
                }
                if (event.message.type === "hello_ng") {
                    const { state: nextState, effects } = enterWaiting(state.failureCount);
                    return { state: nextState, effects: [{ kind: "close" }, ...effects] };
                }
                // hello の応答のほかは、ここでは扱わない。
                return { state, effects: [] };
            }

            if (state.phase === "established") {
                if (event.message.type === "ping") {
                    const pong: Extract<ControlMessage, { type: "pong" }> = { type: "pong", seq: event.message.seq };
                    if (config.clockUs !== undefined) pong.clock_us = config.clockUs();
                    return { state, effects: [{ kind: "send", text: encodeControlMessage(pong) }] };
                }
                // ping のほか (configure、keyframe_request など) は、この状態機械では扱わない。
                return { state, effects: [] };
            }

            // idle、connecting、waiting のときのメッセージは想定していないので、無視する。
            return { state, effects: [] };
        }

        case "closed": {
            if (state.phase === "idle" || state.phase === "waiting") {
                // hello_ng とタイムアウトでは、close と同時に waiting に入るので、その後に届く closed は無視する
                // (schedule を 2 回出さないため)。
                return { state, effects: [] };
            }
            return enterWaiting(state.failureCount);
        }
    }
}
