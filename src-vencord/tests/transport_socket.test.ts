// SPDX-License-Identifier: Apache-2.0

// transport/socket.ts の createSocket() のテスト。hello と hello_ok での確立、hello_ng で閉じて再試行を待つこと、ping への
// pong、切断した後にトークンファイルを読み直すこと、背圧で映像を捨て、音声は上限を超えたときだけ捨てること、
// established の前に送ったものを捨てて数えること、受信側が止まったと見たら切って繋ぎ直すこと (監視) を確認する。
// WebSocket とタイマーは使わず、テスト用の FakeWebSocket と、手で進めるタイマーを渡す。

import { describe, expect, it, vi } from "vitest";

import { DEFAULT_AUDIO_CAP_BYTES } from "../transport/backpressure";
import {
    encodeControlMessage,
    type ControlMessage,
    type LimitsFields,
    type ReceiveScopeFields,
} from "../transport/control";
import { createSocket, type Session, type SocketDeps, type SocketHandlers, type WebSocketLike } from "../transport/socket";
import type { WatchdogReason } from "../transport/watchdog";
import { encodeHeader, FrameType, type FrameHeader } from "../core/wire";

/**
 * deps.setTimer と clearTimer のテスト用の実装。(delayMs, fn) の一覧を持ち、fireOne() で 1 つずつ実行する
 * (transport/client.ts は、動いているタイマーが 1 つだけであることを前提にするので、テストもその前提で 1 つずつ
 * 進める)。
 */
function createFakeTimers() {
    let nextHandle = 1;
    const pending = new Map<number, { fn: () => void; delayMs: number }>();

    return {
        setTimer(fn: () => void, delayMs: number): unknown {
            const handle = nextHandle++;
            pending.set(handle, { fn, delayMs });
            return handle;
        },
        clearTimer(handle: unknown): void {
            pending.delete(handle as number);
        },
        /** 待っているタイマーの数 (1 つだけであることを確認するために使う)。 */
        pendingCount(): number {
            return pending.size;
        },
        /** 待っているタイマーの待ち時間 (ms) の一覧。 */
        pendingDelays(): number[] {
            return [...pending.values()].map((entry) => entry.delayMs);
        },
        /** 待っているタイマーを 1 つ実行する (なければ何もしない)。 */
        fireOne(): void {
            const [handle, entry] = [...pending.entries()][0] ?? [];
            if (handle === undefined || entry === undefined) return;
            pending.delete(handle);
            entry.fn();
        },
    };
}

/**
 * WebSocketLike のテスト用の実装。send() した内容を記録し、テストから onopen、onclose、onmessage を呼び出して進める。
 */
class FakeWebSocket implements WebSocketLike {
    readonly sent: (string | Uint8Array)[] = [];
    bufferedAmount = 0;
    closeCalls = 0;
    onopen: (() => void) | null = null;
    onclose: (() => void) | null = null;
    onerror: ((error: unknown) => void) | null = null;
    onmessage: ((event: { data: unknown }) => void) | null = null;

    send(data: string | Uint8Array): void {
        this.sent.push(data);
    }

    close(): void {
        this.closeCalls++;
    }

    /** 送った文字列を、ControlMessage として読み直す (テストで比べるため)。 */
    sentMessages(): ControlMessage[] {
        return this.sent
            .filter((v): v is string => typeof v === "string")
            .map((text) => {
                const parsed = JSON.parse(text) as ControlMessage;
                return parsed;
            });
    }
}

function makeVideoFrame(streamId: number, keyframe: boolean, recordingOnly = false): Uint8Array {
    const header: FrameHeader = {
        type: FrameType.Video,
        streamId,
        timestampUs: 1_000n,
        flags: { keyframe, recordingOnly },
    };
    const headerBytes = encodeHeader(header);
    const out = new Uint8Array(headerBytes.length + 1);
    out.set(headerBytes, 0);
    out[headerBytes.length] = 0xab; // テスト用のペイロード (1 バイト)。
    return out;
}

interface Harness {
    timers: ReturnType<typeof createFakeTimers>;
    sockets: FakeWebSocket[];
    readSessionMock: ReturnType<typeof vi.fn>;
    handlers: SocketHandlers;
    events: {
        established: number;
        closed: number;
        keyframeRequests: number[];
        streamChecks: number[];
        recordings: boolean[];
        medias: boolean[];
        limits: LimitsFields[];
        mainAccounts: string[];
        scopes: ReceiveScopeFields[];
        moves: number;
        leaves: number;
        watchdogCloses: WatchdogReason[];
        /** 呼ばれた順番 (hello_ok の初期値が、onEstablished より先に届くことを確認する)。 */
        order: string[];
    };
    socket: ReturnType<typeof createSocket>;
}

/** sessionQueue に Error を入れると、その呼び出しの readSession() が reject する。depsOverride は SocketDeps の一部を差し替える。 */
function createHarness(
    sessionQueue: (Session | null | Error)[],
    makeWebSocketImpl?: (url: string) => FakeWebSocket,
    depsOverride: Partial<SocketDeps> = {},
): Harness {
    const timers = createFakeTimers();
    const sockets: FakeWebSocket[] = [];
    let queueIndex = 0;

    const readSessionMock = vi.fn(async (): Promise<Session | null> => {
        const value = sessionQueue[Math.min(queueIndex, sessionQueue.length - 1)] ?? null;
        queueIndex++;
        if (value instanceof Error) throw value;
        return value;
    });

    const events = {
        established: 0,
        closed: 0,
        keyframeRequests: [] as number[],
        streamChecks: [] as number[],
        recordings: [] as boolean[],
        medias: [] as boolean[],
        limits: [] as LimitsFields[],
        mainAccounts: [] as string[],
        scopes: [] as ReceiveScopeFields[],
        moves: 0,
        leaves: 0,
        watchdogCloses: [] as WatchdogReason[],
        order: [] as string[],
    };
    const handlers: SocketHandlers = {
        onEstablished: () => {
            events.established++;
            events.order.push("established");
        },
        onClosed: () => {
            events.closed++;
            events.order.push("closed");
        },
        onKeyframeRequest: (streamId) => events.keyframeRequests.push(streamId),
        onStreamCheck: (streamId) => events.streamChecks.push(streamId),
        onRecording: (on) => events.recordings.push(on),
        onMedia: (on) => {
            events.medias.push(on);
            events.order.push("media");
        },
        onLimits: (limits) => {
            events.limits.push(limits);
            events.order.push("limits");
        },
        onMainAccount: (userId) => events.mainAccounts.push(userId),
        onReceiveScope: (scope) => events.scopes.push(scope),
        onMoveToMainCall: () => {
            events.moves++;
        },
        onLeaveCall: () => {
            events.leaves++;
        },
        onWatchdogClose: (reason) => {
            events.watchdogCloses.push(reason);
            events.order.push("watchdog");
        },
    };

    const deps: SocketDeps = {
        makeWebSocket: (url: string) => {
            if (makeWebSocketImpl) return makeWebSocketImpl(url);
            const ws = new FakeWebSocket();
            sockets.push(ws);
            return ws;
        },
        readSession: readSessionMock,
        setTimer: timers.setTimer,
        clearTimer: timers.clearTimer,
        version: "0.1.0",
        wireVersion: 0,
        ...depsOverride,
    };

    const socket = createSocket(deps, handlers);
    return { timers, sockets, readSessionMock, handlers, events, socket };
}

/**
 * start() から established まで、よくある順番 (hello -> hello_ok) で 1 回だけ進める。readSession() は async なので、
 * WebSocket が作られるまで 1 周待つ。
 */
async function establish(h: Harness): Promise<FakeWebSocket> {
    h.socket.start();
    await Promise.resolve();
    const ws = h.sockets[h.sockets.length - 1];
    if (!ws) throw new Error("WebSocketが作られていない (テストの誤り)");
    ws.onopen?.();
    ws.onmessage?.({ data: encodeControlMessage({ type: "hello_ok", wire: 0, version: "0.1.0" }) });
    return ws;
}

const SECRET_A = "secret-a-value";

describe("hello -> hello_ok で established になる", () => {
    it("start() で read_session を出し、hello を送り、hello_ok で onEstablished が呼ばれる", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        h.socket.start();
        await Promise.resolve(); // readSession() の Promise を 1 周進める。

        expect(h.readSessionMock).toHaveBeenCalledTimes(1);
        expect(h.sockets).toHaveLength(1);

        const ws = h.sockets[0]!;
        ws.onopen?.();
        expect(ws.sentMessages()).toEqual([{ type: "hello", version: "0.1.0", secret: SECRET_A, wire: 0 }]);

        ws.onmessage?.({ data: encodeControlMessage({ type: "hello_ok", wire: 0, version: "0.1.0" }) });

        expect(h.socket.state()).toEqual({ phase: "established", failureCount: 0 });
        expect(h.events.established).toBe(1);
        expect(h.events.closed).toBe(0);
    });
});

describe("hello_ng で閉じて、再試行の待ちに入る", () => {
    it("hello_ng でソケットを閉じ、schedule のとおりのタイマーを 1 つだけ設定する", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        h.socket.start();
        await Promise.resolve();

        const ws = h.sockets[0]!;
        ws.onopen?.();
        ws.onmessage?.({ data: encodeControlMessage({ type: "hello_ng", reason: "bad_secret" }) });

        expect(ws.closeCalls).toBe(1);
        expect(h.socket.state()).toEqual({ phase: "waiting", failureCount: 1 });
        expect(h.timers.pendingCount()).toBe(1); // 動いているタイマーは 1 つだけ。
        expect(h.events.established).toBe(0);
    });

    it("hello_ng の直後に readSession がすぐに呼ばれ (次の tick で connecting になる)、前のソケットから遅れて届いた close を無視する", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }, { port: 4470, secret: "secret-b" }]);
        h.socket.start();
        await Promise.resolve();

        const firstWs = h.sockets[0]!;
        firstWs.onopen?.();
        firstWs.onmessage?.({ data: encodeControlMessage({ type: "hello_ng", reason: "bad_secret" }) });
        // failureCount が 0 回のときの待ち時間は 0 ms なので、次の tick までのタイマーは 1 つだけ設定されている。
        expect(h.timers.pendingCount()).toBe(1);

        h.timers.fireOne(); // tick -> connecting と進み、read_session を出す。
        await Promise.resolve();
        expect(h.sockets).toHaveLength(2); // 新しい WebSocket が開かれた。

        // 前のソケット (firstWs) の close が、新しい接続が始まった後に遅れて届く。
        firstWs.onclose?.();
        // 新しい接続 (connecting) を戻さない (transport/socket.ts は、イベントがどのソケットのものかを比べて、
        // 前の接続のものを無視する)。
        expect(h.socket.state().phase).toBe("connecting");

        const secondWs = h.sockets[1]!;
        secondWs.onopen?.();
        secondWs.onmessage?.({ data: encodeControlMessage({ type: "hello_ok", wire: 0, version: "0.1.0" }) });
        expect(h.socket.state()).toEqual({ phase: "established", failureCount: 0 });
        expect(h.events.established).toBe(1); // 前の接続の closed では、何も起きていない。
    });
});

describe("established: ping には pong を返し、keyframe_request は handlers に渡す", () => {
    it("ping を受けたら pong を送り返す", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        const ws = await establish(h);
        ws.sent.length = 0; // 記録した hello を消す。

        ws.onmessage?.({ data: encodeControlMessage({ type: "ping", seq: 3 }) });
        expect(ws.sentMessages()).toEqual([{ type: "pong", seq: 3 }]);
    });

    it("keyframe_request は client.ts の状態機械を通らずに、handlers.onKeyframeRequest に渡る", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        const ws = await establish(h);

        ws.onmessage?.({ data: encodeControlMessage({ type: "keyframe_request", stream_id: 7 }) });
        expect(h.events.keyframeRequests).toEqual([7]);
        expect(h.socket.state()).toEqual({ phase: "established", failureCount: 0 }); // 状態は変わらない。
    });

    it("stream_check は client.ts の状態機械を通らずに、handlers.onStreamCheck に渡る", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        const ws = await establish(h);

        ws.onmessage?.({ data: encodeControlMessage({ type: "stream_check", stream_id: 5 }) });
        expect(h.events.streamChecks).toEqual([5]);
        expect(h.socket.state()).toEqual({ phase: "established", failureCount: 0 });
    });
});

describe("閉じたら readSession でトークンファイルを読み直す", () => {
    it("established から切断すると、次の tick で readSession をもう一度呼ぶ", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }, { port: 4471, secret: "secret-b" }]);
        const ws = await establish(h);
        expect(h.events.established).toBe(1);

        ws.onclose?.();
        expect(h.socket.state()).toEqual({ phase: "waiting", failureCount: 1 });
        expect(h.events.closed).toBe(1); // established から出たときに 1 回。

        h.timers.fireOne(); // 待ち時間 0 ms の tick。
        await Promise.resolve();

        expect(h.readSessionMock).toHaveBeenCalledTimes(2);
        expect(h.sockets).toHaveLength(2);
    });

    it("readSession が null を返したら (session.json が読めない)、closed として扱って waiting に入る", async () => {
        const h = createHarness([null]);
        h.socket.start();
        await Promise.resolve();

        expect(h.sockets).toHaveLength(0); // WebSocket は開かれていない。
        expect(h.socket.state()).toEqual({ phase: "waiting", failureCount: 1 });
    });
});

describe("read_session が失敗しても、状態機械は止まらない", () => {
    it("readSession() が reject しても (IPC のハンドラーが登録されていないときなど)、null のときと同じく waiting に入り、次の tick で読み直す", async () => {
        const h = createHarness([new Error("no handler registered"), { port: 4470, secret: SECRET_A }]);
        h.socket.start();
        await Promise.resolve();

        expect(h.sockets).toHaveLength(0); // reject したので、WebSocket は開かれていない。
        expect(h.socket.state()).toEqual({ phase: "waiting", failureCount: 1 });
        expect(h.timers.pendingCount()).toBe(1); // 再試行のタイマーが 1 つだけ設定されている (止まったままではない)。

        h.timers.fireOne(); // 待ち時間の後の tick で connecting に進み、read_session をもう一度出す。
        await Promise.resolve();

        expect(h.readSessionMock).toHaveBeenCalledTimes(2);
        expect(h.sockets).toHaveLength(1); // 2 回目は成功して、WebSocket が開かれる。
    });

    it("readSession() が成功しても makeWebSocket() が例外を投げたら、同じく waiting に入って読み直す", async () => {
        let calls = 0;
        const h = createHarness([{ port: 65536, secret: SECRET_A }, { port: 4470, secret: SECRET_A }], (url) => {
            calls++;
            if (calls === 1) throw new SyntaxError(`invalid port in ${url}`);
            const ws = new FakeWebSocket();
            return ws;
        });
        h.socket.start();
        await Promise.resolve();

        expect(h.socket.state()).toEqual({ phase: "waiting", failureCount: 1 });
        expect(h.timers.pendingCount()).toBe(1);

        h.timers.fireOne();
        await Promise.resolve();

        expect(h.readSessionMock).toHaveBeenCalledTimes(2);
        expect(calls).toBe(2); // 1 回目は例外を投げたが、2 回目は WebSocket を開けている。
    });
});

describe("established でない間の sendBinary は捨てて数える", () => {
    it("connecting のときの sendBinary は送らずに、droppedBeforeEstablished を増やす", () => {
        const h = createHarness([null]); // まだどこにも接続しない。
        h.socket.start();

        h.socket.sendBinary(makeVideoFrame(1, true), "video");
        h.socket.sendBinary(makeVideoFrame(1, true), "audio");

        expect(h.socket.stats().droppedBeforeEstablished).toBe(2);
    });

    it("established になれば送れる (捨てない)", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        const ws = await establish(h);
        ws.sent.length = 0;

        h.socket.sendBinary(makeVideoFrame(1, true), "video");
        expect(ws.sent).toHaveLength(1);
        expect(h.socket.stats().droppedBeforeEstablished).toBe(0);
    });
});

describe("背圧で映像を捨てる (音声は上限までは送る)", () => {
    it("bufferedAmount がしきい値を超えている間、キーフレームでない P フレームを捨てる", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        const ws = await establish(h);
        ws.sent.length = 0;
        ws.bufferedAmount = 2_000_000; // 既定のしきい値 (1 MiB) を超える値。

        h.socket.sendBinary(makeVideoFrame(1, false), "video"); // P フレームなので捨てる。
        expect(ws.sent).toHaveLength(0);
        expect(h.socket.stats().droppedByBackpressure).toBe(1);

        h.socket.sendBinary(makeVideoFrame(1, true), "audio"); // 音声は、上限 (16 MiB) を超えるまでは送る。
        expect(ws.sent).toHaveLength(1);
    });

    it("bufferedAmount が音声の上限を超えたら、音声も捨てて数える", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        const ws = await establish(h);
        ws.sent.length = 0;
        ws.bufferedAmount = DEFAULT_AUDIO_CAP_BYTES + 1;

        h.socket.sendBinary(makeVideoFrame(1, true), "audio");
        expect(ws.sent).toHaveLength(0);
        expect(h.socket.stats().droppedAudioByBackpressure).toBe(1);
        expect(h.socket.stats().droppedByBackpressure).toBe(0); // 映像の数とは分けて数える。

        ws.bufferedAmount = DEFAULT_AUDIO_CAP_BYTES; // 上限と等しければ送る。
        h.socket.sendBinary(makeVideoFrame(1, true), "audio");
        expect(ws.sent).toHaveLength(1);
    });

    it("音声の上限は deps の audioCapBytes で変えられる", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }], undefined, { audioCapBytes: 100 });
        const ws = await establish(h);
        ws.sent.length = 0;
        ws.bufferedAmount = 101;

        h.socket.sendBinary(makeVideoFrame(1, true), "audio");
        expect(ws.sent).toHaveLength(0);
        expect(h.socket.stats().droppedAudioByBackpressure).toBe(1);
    });

    it("しきい値を超えている間は、キーフレームも捨てる", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        const ws = await establish(h);
        ws.sent.length = 0;
        ws.bufferedAmount = 2_000_000;

        h.socket.sendBinary(makeVideoFrame(1, true), "video");
        expect(ws.sent).toHaveLength(0);
    });

    it("しきい値以下に戻り、キーフレームが届いたら、送信を再開する", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        const ws = await establish(h);
        ws.sent.length = 0;
        ws.bufferedAmount = 2_000_000;
        h.socket.sendBinary(makeVideoFrame(1, false), "video"); // 捨て始める。

        ws.bufferedAmount = 0;
        h.socket.sendBinary(makeVideoFrame(1, false), "video"); // まだ P フレームなので捨てる。
        expect(ws.sent).toHaveLength(0);

        h.socket.sendBinary(makeVideoFrame(1, true), "video"); // キーフレームで再開する。
        expect(ws.sent).toHaveLength(1);
    });
});

describe("黒埋め用のフレームは、背圧にかかわらずそのまま送る", () => {
    it("しきい値を超えていても黒埋め用のフレームは送り、捨てた数も増えない", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        const ws = await establish(h);
        ws.sent.length = 0;
        ws.bufferedAmount = 2_000_000; // 既定のしきい値 (1 MiB) を超える値。

        h.socket.sendBinary(makeVideoFrame(1, true, true), "video");
        h.socket.sendBinary(makeVideoFrame(1, false, true), "video");
        expect(ws.sent).toHaveLength(2);
        expect(h.socket.stats().droppedByBackpressure).toBe(0);
    });

    it("黒埋め用のキーフレームで、配信の映像がキーフレームを待っている状態を解除しない", async () => {
        // 黒埋め用の IDR で droppingUntilKeyframe が false に戻ると、配信の映像はまだ GOP の途中の P フレームなのに、
        // 送信が再開される (GOP が途中から送られる)。
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        const ws = await establish(h);
        ws.sent.length = 0;
        ws.bufferedAmount = 2_000_000;
        h.socket.sendBinary(makeVideoFrame(1, false), "video"); // 捨て始める。
        expect(h.socket.stats().droppedByBackpressure).toBe(1);

        ws.bufferedAmount = 0;
        h.socket.sendBinary(makeVideoFrame(1, true, true), "video"); // 黒埋め用の IDR。
        expect(ws.sent).toHaveLength(1); // 黒埋め用のフレームは送られる

        h.socket.sendBinary(makeVideoFrame(1, false), "video"); // 配信の映像の P フレーム。
        expect(ws.sent).toHaveLength(1); // まだ捨てている (黒埋め用のフレームで解除されていない)
        expect(h.socket.stats().droppedByBackpressure).toBe(2);

        h.socket.sendBinary(makeVideoFrame(1, true), "video"); // 配信の映像のキーフレームで再開する。
        expect(ws.sent).toHaveLength(2);
    });

    it("established でない間は、黒埋め用のフレームもほかのメッセージと同じく捨てて数える", () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        h.socket.start();

        h.socket.sendBinary(makeVideoFrame(1, true, true), "video");
        expect(h.socket.stats().droppedBeforeEstablished).toBe(1);
        expect(h.socket.stats().droppedByBackpressure).toBe(0);
    });
});

describe("知らない type は捨てて数え、切断はしない", () => {
    it("知らない type が届いても established のままで、その後の ping にも答え続ける", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        const ws = await establish(h);
        ws.sent.length = 0;

        ws.onmessage?.({ data: JSON.stringify({ type: "future_type_from_a_newer_receiver", value: 1 }) });
        expect(h.socket.stats().droppedInvalidControl).toBe(1);
        expect(ws.closeCalls).toBe(0);
        expect(h.socket.state()).toEqual({ phase: "established", failureCount: 0 });

        ws.onmessage?.({ data: encodeControlMessage({ type: "ping", seq: 7 }) });
        expect(ws.sentMessages()).toEqual([{ type: "pong", seq: 7 }]);
    });

    it("知っている type に知らないフィールドが付いていても捨てない", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        const ws = await establish(h);

        ws.onmessage?.({ data: JSON.stringify({ type: "recording", on: true, future_field: "x" }) });
        expect(h.socket.stats().droppedInvalidControl).toBe(0);
        expect(h.events.recordings).toEqual([true]);
    });
});

describe("stop()", () => {
    it("stop() の後は何を呼び出しても何も起きない (2 回目の stop() と、遅れて届いたイベントも)", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        const ws = await establish(h);

        h.socket.stop();
        expect(ws.closeCalls).toBe(1);

        ws.onclose?.(); // 止めた後に遅れて届いたイベント。
        ws.onmessage?.({ data: encodeControlMessage({ type: "ping", seq: 1 }) });
        h.socket.sendBinary(makeVideoFrame(1, true), "video");
        h.socket.sendControl({ type: "log", lines: ["x"] });

        expect(h.events.closed).toBe(0); // stop() は handleEvent() を通らないので、onClosed を呼ばない。
        h.socket.stop(); // 2 回目の stop()。
        expect(ws.closeCalls).toBe(1); // 増えない。
    });
});

describe("recording のメッセージ", () => {
    it("established の間に届いた recording は handlers.onRecording に渡り、状態機械は変わらない", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        const ws = await establish(h);

        ws.onmessage?.({ data: encodeControlMessage({ type: "recording", on: true }) });
        ws.onmessage?.({ data: encodeControlMessage({ type: "recording", on: false }) });
        expect(h.events.recordings).toEqual([true, false]);
        expect(h.socket.state()).toEqual({ phase: "established", failureCount: 0 });
    });

    it("切断しても OFF に戻さない (socket は自分から onRecording を呼ばない)", async () => {
        // 戻すと、再接続のたびに OFF から ON に変わったことになり、送信側が recording-turned-on の黒埋め用のフレームの
        // ための一時的なエンコーダを、毎回作ることになる。受信側は再接続した直後に現在の値を送るので、値がずれるのは
        // 切れている間だけである。
        const h = createHarness([{ port: 4470, secret: SECRET_A }, { port: 4471, secret: "secret-b" }]);
        const ws = await establish(h);
        ws.onmessage?.({ data: encodeControlMessage({ type: "recording", on: true }) });
        expect(h.events.recordings).toEqual([true]);

        ws.onclose?.(); // 切断する。
        expect(h.events.closed).toBe(1);
        expect(h.events.recordings).toEqual([true]); // OFF は呼ばれない

        h.timers.fireOne(); // 再接続する。
        await Promise.resolve();
        const next = h.sockets[h.sockets.length - 1];
        next?.onopen?.();
        next?.onmessage?.({ data: encodeControlMessage({ type: "hello_ok", wire: 0, version: "0.1.0" }) });
        expect(h.events.recordings).toEqual([true]); // 再接続しても呼ばれない

        // 受信側は、接続した直後に現在の値を送る。同じ値でも届く。
        next?.onmessage?.({ data: encodeControlMessage({ type: "recording", on: true }) });
        expect(h.events.recordings).toEqual([true, true]);
    });

    it("形が不正な制御用の JSON は捨てて数える (切断はしない)", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        const ws = await establish(h);
        expect(h.socket.stats().droppedInvalidControl).toBe(0);

        ws.onmessage?.({ data: JSON.stringify({ type: "recording" }) }); // on がない。
        ws.onmessage?.({ data: JSON.stringify({ type: "recording", on: 1 }) }); // 真偽値でない。
        ws.onmessage?.({ data: "{not json" });
        ws.onmessage?.({ data: JSON.stringify({ type: "unknown_future_type" }) });

        expect(h.socket.stats().droppedInvalidControl).toBe(4);
        expect(h.events.recordings).toEqual([]);
        expect(h.socket.state()).toEqual({ phase: "established", failureCount: 0 });
    });
});

describe("main_account、receive_scope、move_to_main_call、leave_call", () => {
    it("established の間に届いたものは handlers に渡り、状態機械は変わらない", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        const ws = await establish(h);

        ws.onmessage?.({ data: encodeControlMessage({ type: "main_account", user_id: "u1" }) });
        ws.onmessage?.({ data: encodeControlMessage({ type: "main_account", user_id: "" }) });
        ws.onmessage?.({ data: encodeControlMessage({ type: "receive_scope", scope: "slots", user_ids: ["a", "b"] }) });
        ws.onmessage?.({ data: encodeControlMessage({ type: "receive_scope", scope: "everyone" }) });
        ws.onmessage?.({ data: encodeControlMessage({ type: "move_to_main_call" }) });
        ws.onmessage?.({ data: encodeControlMessage({ type: "leave_call" }) });

        expect(h.events.mainAccounts).toEqual(["u1", ""]);
        expect(h.events.scopes).toEqual([{ scope: "slots", user_ids: ["a", "b"] }, { scope: "everyone" }]);
        expect(h.events.moves).toBe(1);
        expect(h.events.leaves).toBe(1);
        expect(h.socket.state()).toEqual({ phase: "established", failureCount: 0 });
    });

    it("hello_ok の直後に受信側が送る receive_scope と main_account は、必ず handlers に渡る (範囲が届かずに誰も視聴しないまま止まらない)", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        h.socket.start();
        await Promise.resolve();
        const ws = h.sockets[0]!;
        ws.onopen?.();
        // 受信側は hello_ok の次に、同じ順で送る。onmessage は 1 通ずつ同期して処理するので、hello_ok で established に入った後に処理される。
        ws.onmessage?.({ data: encodeControlMessage({ type: "hello_ok", wire: 0, version: "0.1.0" }) });
        ws.onmessage?.({ data: encodeControlMessage({ type: "main_account", user_id: "u1" }) });
        ws.onmessage?.({ data: encodeControlMessage({ type: "receive_scope", scope: "slots", user_ids: [] }) });

        expect(h.events.mainAccounts).toEqual(["u1"]);
        expect(h.events.scopes).toEqual([{ scope: "slots", user_ids: [] }]);
    });

    it("形が違うものは捨てて数え、handlers に渡さない (切断はしない)", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        const ws = await establish(h);

        ws.onmessage?.({ data: JSON.stringify({ type: "main_account" }) });
        ws.onmessage?.({ data: JSON.stringify({ type: "main_account", user_id: 1 }) });
        ws.onmessage?.({ data: JSON.stringify({ type: "receive_scope", scope: "slots" }) });
        ws.onmessage?.({ data: JSON.stringify({ type: "receive_scope", scope: "slots", user_ids: [1] }) });
        ws.onmessage?.({ data: JSON.stringify({ type: "receive_scope", scope: "all" }) });

        expect(h.socket.stats().droppedInvalidControl).toBe(5);
        expect(h.events.mainAccounts).toEqual([]);
        expect(h.events.scopes).toEqual([]);
        expect(h.socket.state()).toEqual({ phase: "established", failureCount: 0 });
    });

    it("established でない間に届いたものは、handlers に渡さない", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        h.socket.start();
        await Promise.resolve();
        const ws = h.sockets[0]!;
        ws.onopen?.();

        ws.onmessage?.({ data: encodeControlMessage({ type: "move_to_main_call" }) });
        ws.onmessage?.({ data: encodeControlMessage({ type: "leave_call" }) });

        expect(h.events.moves).toBe(0);
        expect(h.events.leaves).toBe(0);
    });
});

describe("media、limits と、hello_ok の初期値", () => {
    it("established の間に届いた media と limits は handlers に渡り、状態機械は変わらない", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        const ws = await establish(h);

        ws.onmessage?.({ data: encodeControlMessage({ type: "media", on: false }) });
        ws.onmessage?.({ data: encodeControlMessage({ type: "limits", max_long_edge: 1280, fps_divisor: 2 }) });
        ws.onmessage?.({ data: encodeControlMessage({ type: "limits" }) }); // キーをすべて省いたもの (上限なし)

        expect(h.events.medias).toEqual([false]);
        expect(h.events.limits).toEqual([{ max_long_edge: 1280, fps_divisor: 2 }, {}]);
        expect(h.socket.state()).toEqual({ phase: "established", failureCount: 0 });
    });

    it("範囲の外か型が違う media と limits は捨てて数え、handlers に渡さない (切断はしない)", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        const ws = await establish(h);

        ws.onmessage?.({ data: JSON.stringify({ type: "media" }) });
        ws.onmessage?.({ data: JSON.stringify({ type: "media", on: 0 }) });
        ws.onmessage?.({ data: JSON.stringify({ type: "limits", fps_divisor: 3 }) });
        ws.onmessage?.({ data: JSON.stringify({ type: "limits", max_long_edge: 0 }) });

        expect(h.socket.stats().droppedInvalidControl).toBe(4);
        expect(h.events.medias).toEqual([]);
        expect(h.events.limits).toEqual([]);
        expect(h.socket.state()).toEqual({ phase: "established", failureCount: 0 });
    });

    it("hello_ok の初期値は onEstablished より先に渡る", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        h.socket.start();
        await Promise.resolve();
        const ws = h.sockets[0]!;
        ws.onopen?.();
        ws.onmessage?.({
            data: encodeControlMessage({
                type: "hello_ok",
                wire: 0,
                version: "0.1.0+dev",
                media: false,
                limits: { max_long_edge: 960 },
            }),
        });

        expect(h.events.order).toEqual(["media", "limits", "established"]);
        expect(h.events.medias).toEqual([false]);
        expect(h.events.limits).toEqual([{ max_long_edge: 960 }]);
    });

    it("hello_ok に初期値がなければ (古い受信側)、onMedia と onLimits を呼ばない", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        await establish(h);
        expect(h.events.order).toEqual(["established"]);
    });

    it("hello_ok の省略できるキーが壊れていても接続し、そのキーだけを渡さない", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        h.socket.start();
        await Promise.resolve();
        const ws = h.sockets[0]!;
        ws.onopen?.();
        ws.onmessage?.({
            data: JSON.stringify({ type: "hello_ok", wire: 0, version: "v", media: 1, limits: { fps_divisor: 3 } }),
        });

        expect(h.socket.state()).toEqual({ phase: "established", failureCount: 0 });
        expect(h.events.order).toEqual(["established"]);
    });

    it("established の前に届いた media と limits は渡さない", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        h.socket.start();
        await Promise.resolve();
        const ws = h.sockets[0]!;
        ws.onopen?.();
        ws.onmessage?.({ data: encodeControlMessage({ type: "media", on: true }) });
        ws.onmessage?.({ data: encodeControlMessage({ type: "limits", fps_divisor: 2 }) });

        expect(h.events.medias).toEqual([]);
        expect(h.events.limits).toEqual([]);
    });

    it("切断しても、socket は自分から onMedia を呼ばない (止めるのは呼び出し元の onClosed)", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        const ws = await establish(h);
        ws.onmessage?.({ data: encodeControlMessage({ type: "media", on: true }) });
        ws.onclose?.();
        expect(h.events.closed).toBe(1);
        expect(h.events.medias).toEqual([true]);
    });
});

describe("監視: 受信側が止まったと見たら、切って繋ぎ直す", () => {
    it("established の間だけ監視のタイマーが 1 つ動き、切断と stop() で止まる", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }, { port: 4470, secret: SECRET_A }]);
        const ws = await establish(h);
        expect(h.timers.pendingCount()).toBe(1); // 監視のタイマー (hello の応答を待つタイマーは止めてある)。

        ws.onclose?.();
        expect(h.timers.pendingCount()).toBe(1); // 繋ぎ直しのタイマーだけ。監視は止まっている。

        h.timers.fireOne();
        await Promise.resolve();
        const next = h.sockets[1]!;
        next.onopen?.();
        next.onmessage?.({ data: encodeControlMessage({ type: "hello_ok", wire: 0, version: "0.1.0" }) });
        expect(h.timers.pendingCount()).toBe(1); // 確立し直したので、監視がまた動く。

        h.socket.stop();
        expect(h.timers.pendingCount()).toBe(0);
    });

    it("ping が 20 秒 (監視 4 回分) 来なければ、理由を渡してから切り、切断と同じく繋ぎ直す", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }, { port: 4470, secret: SECRET_A }]);
        const ws = await establish(h);

        for (let i = 0; i < 3; i++) h.timers.fireOne(); // 15 秒。まだ切らない。
        expect(h.socket.state().phase).toBe("established");
        expect(ws.closeCalls).toBe(0);

        h.timers.fireOne(); // 20 秒。
        expect(h.events.watchdogCloses).toEqual(["ping-silence"]);
        expect(h.events.order.slice(-2)).toEqual(["watchdog", "closed"]);
        expect(ws.closeCalls).toBe(1);
        expect(h.socket.state()).toEqual({ phase: "waiting", failureCount: 1 });
        expect(h.socket.stats().watchdogCloses).toBe(1);

        ws.onclose?.(); // 切った接続の close が遅れて届いても、もう 1 回切断としては扱わない。
        expect(h.events.closed).toBe(1);

        h.timers.fireOne(); // 繋ぎ直しの tick (待ち時間 0 ms)。
        await Promise.resolve();
        expect(h.readSessionMock).toHaveBeenCalledTimes(2);
        expect(h.sockets).toHaveLength(2);
    });

    it("ping が届いている間は切らない", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        const ws = await establish(h);

        for (let i = 0; i < 10; i++) {
            ws.onmessage?.({ data: encodeControlMessage({ type: "ping", seq: i }) });
            h.timers.fireOne();
        }
        expect(h.socket.state().phase).toBe("established");
        expect(h.events.watchdogCloses).toEqual([]);
    });

    it("送る待ちが音声の上限を超えたまま 15 秒たったら、ping が届いていても切る", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        const ws = await establish(h);
        ws.bufferedAmount = DEFAULT_AUDIO_CAP_BYTES + 1;

        for (let i = 0; i < 2; i++) {
            ws.onmessage?.({ data: encodeControlMessage({ type: "ping", seq: i }) });
            h.timers.fireOne();
        }
        expect(h.socket.state().phase).toBe("established");

        ws.onmessage?.({ data: encodeControlMessage({ type: "ping", seq: 2 }) });
        h.timers.fireOne(); // 15 秒。
        expect(h.events.watchdogCloses).toEqual(["over-cap"]);
        expect(h.socket.state().phase).toBe("waiting");
    });

    it("送る待ちが上限を下回ったら、超えていた時間を数え直す", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        const ws = await establish(h);

        for (const buffered of [DEFAULT_AUDIO_CAP_BYTES + 1, DEFAULT_AUDIO_CAP_BYTES + 1, 0, DEFAULT_AUDIO_CAP_BYTES + 1]) {
            ws.bufferedAmount = buffered;
            ws.onmessage?.({ data: encodeControlMessage({ type: "ping", seq: 0 }) });
            h.timers.fireOne();
        }
        expect(h.socket.state().phase).toBe("established");
    });

    it("established の前に届いた ping は数えない (確立した時から数える)", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }]);
        h.socket.start();
        await Promise.resolve();
        const ws = h.sockets[0]!;
        ws.onopen?.();
        ws.onmessage?.({ data: encodeControlMessage({ type: "ping", seq: 0 }) });
        ws.onmessage?.({ data: encodeControlMessage({ type: "hello_ok", wire: 0, version: "0.1.0" }) });

        for (let i = 0; i < 4; i++) h.timers.fireOne();
        expect(h.events.watchdogCloses).toEqual(["ping-silence"]);
    });

    it("間隔と切るまでの時間は deps の watchdogConfig で変えられる", async () => {
        const h = createHarness([{ port: 4470, secret: SECRET_A }], undefined, {
            watchdogConfig: { intervalMs: 1000, pingSilenceMs: 2000, overCapMs: 1000 },
        });
        const ws = await establish(h);
        expect(h.timers.pendingDelays()).toEqual([1000]);

        h.timers.fireOne();
        expect(h.socket.state().phase).toBe("established");
        expect(h.timers.pendingDelays()).toEqual([1000]);

        h.timers.fireOne(); // 2 回で 2000 ms。
        expect(h.events.watchdogCloses).toEqual(["ping-silence"]);
        expect(ws.closeCalls).toBe(1);
    });
});
