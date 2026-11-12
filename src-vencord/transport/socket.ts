// SPDX-License-Identifier: Apache-2.0

// transport/client.ts の状態機械を、実際の WebSocket とタイマーで動かす。WebSocket を作る関数、トークンファイルを
// 読む関数、タイマーは呼び出し元が渡す (SocketDeps) ので、Node の vitest でテストできる。製品では index.ts が、
// Vesktop のレンダラーの WebSocket を WebSocketLike に包んで渡す。tsconfig.json の lib は ES2022 で DOM の型を
// 持たないので、WebSocket の型は使わず、このファイルが使う部分だけを WebSocketLike として定義する。

import {
    applyBackpressure,
    createBackpressureState,
    DEFAULT_AUDIO_CAP_BYTES,
    DEFAULT_BACKPRESSURE_THRESHOLD_BYTES,
    type BackpressureState,
    type FrameKind,
} from "./backpressure";
import {
    createClient,
    transition,
    type ClientConfig,
    type ClientEffect,
    type ClientEvent,
    type ClientState,
} from "./client";
import {
    decodeControlMessage,
    encodeControlMessage,
    type ControlMessage,
    type LimitsFields,
    type ReceiveScopeFields,
} from "./control";
import {
    createWatchdogState,
    DEFAULT_WATCHDOG_CONFIG,
    stepWatchdog,
    type WatchdogConfig,
    type WatchdogReason,
    type WatchdogState,
} from "./watchdog";
import { decodeHeader } from "../core/wire";

/**
 * `WebSocket` のうち、このファイルが使う部分だけの型。イベントは on* のプロパティに代入して受け取り、
 * addEventListener は使わない。index.ts は `WebSocket` をキャストせずに、この形のラッパーを作って渡すので、
 * DOM の `WebSocket` の型との互換 (関数の引数の型など) を考えなくてよい。
 */
export interface WebSocketLike {
    readonly bufferedAmount: number;
    send(data: string | Uint8Array): void;
    close(): void;
    onopen: (() => void) | null;
    onclose: (() => void) | null;
    onerror: ((error: unknown) => void) | null;
    onmessage: ((event: { data: unknown }) => void) | null;
}

/** 接続を試すたびにトークンファイルから読み直す、接続先のポートとトークン。 */
export interface Session {
    readonly port: number;
    readonly secret: string;
}

export interface SocketDeps {
    /** `new WebSocket(url)` で作った WebSocket を、WebSocketLike に包んで返す。 */
    makeWebSocket(url: string): WebSocketLike;
    /** トークンファイルを読み直す (製品では native.ts の readSession())。読めなければ null を返す。 */
    readSession(): Promise<Session | null>;
    /** `setTimeout` と同じ。戻り値は `clearTimer` にそのまま渡す。 */
    setTimer(fn: () => void, ms: number): unknown;
    clearTimer(handle: unknown): void;
    /** hello の version。送信側のバージョン (version.ts の CORDMUX_BUILD_ID)。 */
    version: string;
    /** hello の wire。ワイヤー形式のバージョン。 */
    wireVersion: number;
    /** hello の vesktop。Vesktop のバージョン。読めなかったときは undefined にし、hello にキーを入れない。 */
    vesktopVersion?: string;
    /** hello と pong の clock_us を読む送信側の時計 (client.ts の ClientConfig の clockUs)。省くと clock_us を入れない。 */
    clockUs?: () => number;
    /**
     * 音声も捨て始める bufferedAmount (バイト)。省略したときは backpressure.ts の DEFAULT_AUDIO_CAP_BYTES。
     * 測って決めた値ではないので、変えられるようにしてある (watchdogConfig も同じ)。
     */
    audioCapBytes?: number;
    /** 受信側が止まったと見て切るまでの時間 (transport/watchdog.ts)。省略したときは DEFAULT_WATCHDOG_CONFIG。 */
    watchdogConfig?: WatchdogConfig;
}

export interface SocketHandlers {
    /** established に入ったときに 1 回呼ぶ。index.ts は、ここで configure を送り直し、ログの送信を始める。 */
    onEstablished(): void;
    /**
     * established から出たときに 1 回呼ぶ。一度も established に入っていない間の失敗では呼ばない。呼び出し元が
     * 知る必要があるのは、確立していた接続が切れたことだけである。
     */
    onClosed(): void;
    /**
     * established の間に届いた keyframe_request で呼ぶ。受信側は established の間にしか送らないが、念のため、
     * established でない間に届いたものでは呼ばない。
     */
    onKeyframeRequest(streamId: number): void;
    /**
     * established の間に届いた `stream_check` で呼ぶ。受信側が、映像が届かないストリームの確認を頼んだときに届く。
     * 呼び出し元は確認して、結果を `stream_check_result` で返す。省略できない (onRecording と同じ理由)。
     */
    onStreamCheck(streamId: number): void;
    /**
     * established の間に届いた `recording` で呼ぶ。状態機械 (client.ts) では扱わないので、ここから直接渡す。
     * 省略できない。省略できるようにすると、届いた値を捨ててしまう呼び出し元を型で見つけられない (index.ts と
     * テスト用の送信側 test/tools/harness/sender.ts の両方が渡す)。
     */
    onRecording(on: boolean): void;
    /**
     * トラックの読み出しとエンコードを許すか。established の間に届いた `media` と、`hello_ok` に入っていた初期値で
     * 呼ぶ。初期値は、established に入ったときに onEstablished() より先に渡す。onEstablished() の中で configure を
     * 送るときに、呼び出し元がもう値を知っているようにするためである。hello_ok にキーがなければ (古い受信側)
     * 呼ばない。切断しても onMedia(false) は呼ばない。切断したときに処理を止めるのは、呼び出し元が onClosed() で
     * する。省略できない (テスト用の送信側も渡す)。
     */
    onMedia(on: boolean): void;
    /**
     * 画質の上限。形を確認した値だけを渡す (省略したキーは制限しない)。呼ぶタイミングは onMedia() と同じ。
     * 省略できない。
     */
    onLimits(limits: LimitsFields): void;
    /**
     * established の間に届いた `main_account` で呼ぶ。空の文字列は、メインアカウントを使わない構成。受信側は、
     * 値が変わったときと確立した直後に送る。省略できない。
     */
    onMainAccount(userId: string): void;
    /**
     * established の間に届いた `receive_scope` で呼ぶ。視聴する人の範囲で、値が変わったときと確立した直後に届く。
     * 省略できない。
     */
    onReceiveScope(scope: ReceiveScopeFields): void;
    /**
     * established の間に届いた `move_to_main_call` で呼ぶ。利用者が [移動] を押したときだけ届く。省略できない。
     */
    onMoveToMainCall(): void;
    /** established の間に届いた `leave_call` で呼ぶ。利用者が [退出] を押したときだけ届く。省略できない。 */
    onLeaveCall(): void;
    /**
     * 受信側が止まったと見て、established の接続を切ったときに呼ぶ (transport/watchdog.ts)。この後に onClosed() も
     * 呼び、繋ぎ直しは client.ts が切断のときと同じく進める。理由をログに残すためにある。省略できない。
     */
    onWatchdogClose(reason: WatchdogReason): void;
}

/** 捨てたメッセージの数。 */
export interface SocketStats {
    /** established でない間に `sendBinary` で捨てたメッセージの数 (映像と音声の合計)。 */
    readonly droppedBeforeEstablished: number;
    /** 背圧 (bufferedAmount がしきい値を超えた) で捨てた映像のフレームの数 (backpressure.ts)。 */
    readonly droppedByBackpressure: number;
    /** bufferedAmount が音声の上限 (SocketDeps の audioCapBytes) を超えていて捨てた音声のメッセージの数。 */
    readonly droppedAudioByBackpressure: number;
    /** 受信側が止まったと見て、接続を切った回数 (SocketHandlers の onWatchdogClose)。 */
    readonly watchdogCloses: number;
    /**
     * 形が不正で捨てた制御用の JSON の数。JSON として読めないもの、`type` を知らないもの、省略できないキーがない
     * もの (`recording` に `on` がない、など) をまとめて数える。捨てても接続は切らない。受信側の同じ数は
     * src-obs/ws/listener.h の ListenerStats::dropped_unknown_control である。問題の切り分けにだけ使う。
     */
    readonly droppedInvalidControl: number;
}

export interface SocketHandle {
    /** 接続を始める。2 回目からと、stop() の後は何もしない。 */
    start(): void;
    /**
     * タイマーを止めて接続を閉じ、以後は接続し直さない。onClosed() は呼ばないので、後片付けは呼び出し元がする。
     * 何度呼び出してもよい。
     */
    stop(): void;
    /**
     * 映像か音声のメッセージを送る。established でない間は捨てて、SocketStats の droppedBeforeEstablished に
     * 数える。映像は背圧で、音声は送る待ちが上限を超えたときに捨てることがある (backpressure.ts)。
     */
    sendBinary(buf: Uint8Array, kind: FrameKind): void;
    /**
     * 制御用の JSON を送る。established でない間は捨てて、数えない。hello が最初のメッセージでなければならない
     * ので、確立する前に configure、log、facts などを送らないためである。hello と pong は client.ts の send から
     * 送るので、この関数を通らない。呼び出し元の index.ts は、確立するたびに configure などを送り直す。
     */
    sendControl(message: ControlMessage): void;
    state(): ClientState;
    stats(): SocketStats;
}

const WS_URL_HOST = "127.0.0.1"; // 受信側は 127.0.0.1 でだけ待ち受ける。

/**
 * 受信側との接続を扱う SocketHandle を作る。start() を呼ぶまでは接続しない。handlers のコールバックは、どれも省略
 * できない。プラグインを起動するたびに 1 つ作る (stop() の後に start() を呼んでも何もしないので、作り直す)。
 */
export function createSocket(deps: SocketDeps, handlers: SocketHandlers): SocketHandle {
    const config: ClientConfig = {
        version: deps.version,
        wireVersion: deps.wireVersion,
        vesktopVersion: deps.vesktopVersion,
        clockUs: deps.clockUs,
    };
    const audioCapBytes = deps.audioCapBytes ?? DEFAULT_AUDIO_CAP_BYTES;
    const watchdogConfig = deps.watchdogConfig ?? DEFAULT_WATCHDOG_CONFIG;

    let clientState: ClientState = createClient();
    let ws: WebSocketLike | null = null;
    let timerHandle: unknown = null;
    // 背圧の状態は、接続全体で 1 つにする。sendBinary() はストリームを引数に取らないためで、1 つの映像が詰まると、
    // ほかのストリームの P フレームも捨てる。ストリームごとに分けるには、buf のヘッダーから stream_id を読み、
    // stream_id ごとに状態を持てばよい。分ける必要があるかは確認していない。
    let backpressure: BackpressureState = createBackpressureState();
    let droppedBeforeEstablished = 0;
    let droppedInvalidControl = 0;
    // 監視のタイマーは、client.ts の schedule のタイマー (timerHandle) とは別に持つ。established の間だけ動かす。
    let watchdogHandle: unknown = null;
    let watchdog: WatchdogState = createWatchdogState();
    /** 前の監視から ping が届いたか。 */
    let pingSeen = false;
    let watchdogCloses = 0;
    let started = false;
    let stopped = false;
    /** 直近の hello_ok に入っていた初期値。established に入ったときに渡して、空に戻す。 */
    let pendingHelloOk: { media?: boolean; limits?: LimitsFields } | null = null;

    function clearScheduledTimer(): void {
        if (timerHandle !== null) {
            deps.clearTimer(timerHandle);
            timerHandle = null;
        }
    }

    function stopWatchdog(): void {
        if (watchdogHandle !== null) {
            deps.clearTimer(watchdogHandle);
            watchdogHandle = null;
        }
    }

    function startWatchdog(): void {
        stopWatchdog();
        watchdog = createWatchdogState();
        pingSeen = false;
        watchdogHandle = deps.setTimer(runWatchdog, watchdogConfig.intervalMs);
    }

    /** 監視の 1 回分。受信側が止まったと見たら、接続を切って、切断と同じ扱いにする。 */
    function runWatchdog(): void {
        watchdogHandle = null;
        if (stopped || clientState.phase !== "established") return;
        const overCap = ws !== null && ws.bufferedAmount > audioCapBytes;
        const decision = stepWatchdog(watchdog, { pingSeen, overCap }, watchdogConfig);
        watchdog = decision.state;
        pingSeen = false;
        if (decision.reason !== null) {
            watchdogCloses++;
            handlers.onWatchdogClose(decision.reason);
            // closeSocket() は先に ws を null にするので、閉じた接続の onclose は handleEvent に届かない。closed は
            // ここで 1 回だけ作る。
            closeSocket();
            handleEvent({ kind: "closed" });
            return;
        }
        watchdogHandle = deps.setTimer(runWatchdog, watchdogConfig.intervalMs);
    }

    function closeSocket(): void {
        const current = ws;
        ws = null;
        if (current === null) return;
        try {
            current.close();
        } catch {
            // 既に閉じているときなどは無視する (2 回閉じてもよい)。
        }
    }

    /**
     * WebSocket を 1 つ開き、そのイベントを client.ts のイベントにする。前の接続から遅れて届いたイベントは、
     * `ws !== socket` で見分けて無視する。ws を差し替えるのは、closeSocket() とこの関数だけである。
     */
    function openConnection(session: Session): void {
        const socket = deps.makeWebSocket(`ws://${WS_URL_HOST}:${session.port}`);
        ws = socket;

        socket.onopen = () => {
            if (ws !== socket) return;
            handleEvent({ kind: "connected", session: { secret: session.secret } });
        };
        socket.onclose = () => {
            const wasCurrent = ws === socket;
            if (wasCurrent) ws = null;
            if (!wasCurrent) return; // 既に別の接続に替わっている (前の接続のイベント)。
            handleEvent({ kind: "closed" });
        };
        socket.onerror = () => {
            // 何もしない。WebSocket は error の後に必ず close を発生させるので、再接続は onclose だけで扱い、
            // closed のイベントを 2 回作らない。
        };
        socket.onmessage = (event) => {
            if (ws !== socket) return;
            handleReceivedMessage(event.data);
        };
    }

    function handleReceivedMessage(data: unknown): void {
        if (typeof data !== "string") return; // 受信側から届くのは、テキストのメッセージだけである。
        const message = decodeControlMessage(data);
        if (message === undefined) {
            // 不正な形のメッセージは捨てるが、切断はしない。数えるのはここだけで、control.ts は形を確認するだけである。
            droppedInvalidControl++;
            return;
        }

        // keyframe_request、stream_check、recording、media、limits、main_account、receive_scope、move_to_main_call、leave_call は client.ts の状態機械では扱わないので、状態機械に渡すのとは
        // 別に、ここから直接 handlers に渡す。
        if (message.type === "keyframe_request" && clientState.phase === "established") {
            handlers.onKeyframeRequest(message.stream_id);
        }
        if (message.type === "stream_check" && clientState.phase === "established") {
            handlers.onStreamCheck(message.stream_id);
        }
        if (message.type === "recording" && clientState.phase === "established") {
            handlers.onRecording(message.on);
        }
        if (message.type === "media" && clientState.phase === "established") {
            handlers.onMedia(message.on);
        }
        if (message.type === "limits" && clientState.phase === "established") {
            const { type: _type, ...limits } = message;
            handlers.onLimits(limits);
        }
        if (message.type === "main_account" && clientState.phase === "established") {
            handlers.onMainAccount(message.user_id);
        }
        if (message.type === "receive_scope" && clientState.phase === "established") {
            const { type: _type, ...scope } = message;
            handlers.onReceiveScope(scope);
        }
        if (message.type === "move_to_main_call" && clientState.phase === "established") {
            handlers.onMoveToMainCall();
        }
        if (message.type === "leave_call" && clientState.phase === "established") {
            handlers.onLeaveCall();
        }
        if (message.type === "ping" && clientState.phase === "established") {
            pingSeen = true; // 監視 (runWatchdog) が読む。pong は client.ts が返す。
        }
        if (message.type === "hello_ok" && clientState.phase !== "established") {
            pendingHelloOk = { media: message.media, limits: message.limits };
        }
        handleEvent({ kind: "message", message });
        pendingHelloOk = null; // hello_ok を受けても established に入らなかったときは、初期値を捨てる。
    }

    function runEffects(effects: readonly ClientEffect[]): void {
        for (const effect of effects) {
            switch (effect.kind) {
                case "send":
                    ws?.send(effect.text);
                    break;
                case "close":
                    closeSocket();
                    break;
                case "read_session":
                    // readSession() が reject したときと、openConnection() (makeWebSocket() を含む) が例外を
                    // 投げたときも、null が返ったときと同じく closed にする。closed にしないと、状態が connecting の
                    // まま schedule も出ず、再接続を試さなくなる。native.ts の readSession() は reject しないが、
                    // VencordNative.pluginHelpers.cordmux の IPC は、ハンドラーが登録されていないときなどに
                    // reject する。不正なポートでは new WebSocket() が SyntaxError を投げる (native.ts はポートの
                    // 上限を確認しない)。then に 2 つの関数を渡すのは、catch をつなぐと 1 周遅れて処理され、
                    // その間は処理されない reject になるためである。
                    void deps.readSession().then(
                        (session) => {
                            if (stopped) return;
                            if (session === null) {
                                handleEvent({ kind: "closed" });
                                return;
                            }
                            try {
                                openConnection(session);
                            } catch {
                                handleEvent({ kind: "closed" });
                            }
                        },
                        () => {
                            if (stopped) return;
                            handleEvent({ kind: "closed" });
                        },
                    );
                    break;
                case "schedule":
                    clearScheduledTimer();
                    timerHandle = deps.setTimer(() => handleEvent({ kind: "tick" }), effect.delayMs);
                    break;
            }
        }
    }

    function handleEvent(event: ClientEvent): void {
        if (stopped) return;
        const prevPhase = clientState.phase;
        const result = transition(config, clientState, event);
        clientState = result.state;
        runEffects(result.effects);

        if (clientState.phase === "established") {
            // handshaking のタイムアウトのタイマーが残っていれば止める。client.ts は established の間の tick を
            // 無視するので、止めなくても動作は変わらないが、動いているタイマーを 1 つだけにする約束
            // (client.ts の schedule) を守る。
            clearScheduledTimer();
            if (prevPhase !== "established") {
                // hello_ok の初期値を、onEstablished() より先に渡す (SocketHandlers の onMedia のコメント)。
                const initial = pendingHelloOk;
                pendingHelloOk = null;
                if (initial?.media !== undefined) handlers.onMedia(initial.media);
                if (initial?.limits !== undefined) handlers.onLimits(initial.limits);
                startWatchdog();
                handlers.onEstablished();
            }
        } else if (prevPhase === "established") {
            stopWatchdog();
            handlers.onClosed();
        }
    }

    return {
        start(): void {
            if (started || stopped) return;
            started = true;
            handleEvent({ kind: "tick" }); // idle から connecting に進み、最初の read_session を出す。
        },

        stop(): void {
            if (stopped) return;
            stopped = true;
            clearScheduledTimer();
            stopWatchdog();
            closeSocket();
            clientState = createClient();
        },

        sendBinary(buf: Uint8Array, kind: FrameKind): void {
            if (clientState.phase !== "established" || ws === null) {
                droppedBeforeEstablished++;
                return;
            }
            const decoded = decodeHeader(buf);
            // decodeHeader() が失敗するのは、呼び出し元 (encode/session.ts など) が壊れたヘッダーを渡したときだけで、
            // 通常は起きない。そのときは送らない。
            if (!decoded.ok) return;
            // 黒埋め用のフレームは、背圧を判断せず、状態も変えずにそのまま送る。applyBackpressure() は配信の映像か
            // 黒埋め用かを区別しないので、黒埋め用のキーフレームで droppingUntilKeyframe が解除され、配信の映像が
            // GOP の途中から送られてしまうためである。黒埋め用のフレームは真っ黒な 1 秒分で小さい (2,710 バイトの
            // 実測がある) ので、そのまま送っても詰まりは悪くならない。
            if (decoded.header.flags.recordingOnly) {
                ws.send(buf);
                return;
            }
            const isKeyframe = decoded.header.flags.keyframe;

            const decision = applyBackpressure(
                backpressure,
                kind,
                ws.bufferedAmount,
                DEFAULT_BACKPRESSURE_THRESHOLD_BYTES,
                isKeyframe,
                audioCapBytes,
            );
            backpressure = decision.state;
            if (!decision.send) return;
            ws.send(buf);
        },

        sendControl(message: ControlMessage): void {
            // established の間だけ送る (SocketHandle の sendControl のコメント)。
            if (clientState.phase !== "established" || ws === null) return;
            ws.send(encodeControlMessage(message));
        },

        state(): ClientState {
            return clientState;
        },

        stats(): SocketStats {
            return {
                droppedBeforeEstablished,
                droppedByBackpressure: backpressure.droppedVideoFrames,
                droppedAudioByBackpressure: backpressure.droppedAudioMessages,
                watchdogCloses,
                droppedInvalidControl,
            };
        },
    };
}
