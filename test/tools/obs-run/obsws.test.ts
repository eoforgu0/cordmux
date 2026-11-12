/*
 * obsws.mjs の call() が時間切れを持つこと (超えたら reject し、pending からも取り除く) を確認する。
 * 期限を過ぎたら計測を止めるために必要な動作である。
 * connect() のハンドシェイクは確認せず、call() だけを直接確認する (ws を直接差し替える)。
 */
import { describe, expect, it, vi } from "vitest";

import { OBS_EVENT_SUBSCRIPTION_INPUT_VOLUME_METERS, ObsWs } from "./obsws.mjs";

// WebSocket 実物の代わりに send() だけを記録する最小のスタブ (何も返信しない = 応答無限待ちを模す)。
class SilentSocket {
    sent: string[] = [];
    send(data: string): void {
        this.sent.push(data);
    }
    close(): void {
        /* 何もしない */
    }
}

describe("ObsWs.call の時限", () => {
    it("応答が来なければ timeoutMs で reject し、pending からも消える", async () => {
        vi.useFakeTimers();
        try {
            const client = new ObsWs(4460);
            const socket = new SilentSocket();
            // @ts-expect-error テスト用に接続済みの ws を直接差し込む (connect() は確認しない)。
            client.ws = socket;

            const promise = client.call("GetVersion", {}, 50);
            const assertion = expect(promise).rejects.toThrow(/timeout/);
            await vi.advanceTimersByTimeAsync(50);
            await assertion;

            expect(client.pending.size).toBe(0);
        } finally {
            vi.useRealTimers();
        }
    });

    it("timeoutMs が来る前に応答が届けば解決し、タイマーは片付く", async () => {
        vi.useFakeTimers();
        try {
            const client = new ObsWs(4460);
            const socket = new SilentSocket();
            // @ts-expect-error 同上。
            client.ws = socket;

            const promise = client.call("GetVersion", {}, 5000);
            // pending に積まれた requestId を使って、応答が届いたのと同じ経路 (#connectOnce の
            // onmessage が delete してから resolve を呼ぶ) を模す。
            const [requestId, pending] = [...client.pending.entries()][0];
            client.pending.delete(requestId);
            pending.resolve({ obsVersion: "31.1.1" });

            await expect(promise).resolves.toEqual({ obsVersion: "31.1.1" });
            expect(client.pending.has(requestId)).toBe(false);
        } finally {
            vi.useRealTimers();
        }
    });

    it("timeoutMs <= 0 なら時限を付けない (旧来どおり無期限)", async () => {
        const client = new ObsWs(4460);
        const socket = new SilentSocket();
        // @ts-expect-error 同上。
        client.ws = socket;

        client.call("GetVersion", {}, 0);
        expect(client.pending.size).toBe(1);
    });
});

// Hello に Identify で答え、Identified を返す WebSocket のスタブ。送った Identify の d を記録する。
class IdentifyingSocket {
    static identifies: Array<Record<string, unknown>> = [];
    onerror: (() => void) | null = null;
    onclose: (() => void) | null = null;
    onmessage: ((ev: { data: string }) => void) | null = null;
    constructor(_url: string) {
        queueMicrotask(() => this.onmessage?.({ data: JSON.stringify({ op: 0, d: {} }) }));
    }
    send(data: string): void {
        const msg = JSON.parse(data);
        if (msg.op === 1) {
            IdentifyingSocket.identifies.push(msg.d);
            queueMicrotask(() => this.onmessage?.({ data: JSON.stringify({ op: 2, d: {} }) }));
        }
    }
    close(): void {
        /* 何もしない */
    }
}

describe("ObsWs の Identify の eventSubscriptions", () => {
    it("既定は 0 (何も購読しない)。渡せばその値で Identify する (InputVolumeMeters は 1<<16)", async () => {
        vi.stubGlobal("WebSocket", IdentifyingSocket);
        try {
            IdentifyingSocket.identifies = [];
            await new ObsWs(4460).connect(1000);
            await new ObsWs(4460, { eventSubscriptions: OBS_EVENT_SUBSCRIPTION_INPUT_VOLUME_METERS }).connect(1000);
            await new ObsWs(4460, { eventSubscriptions: -1 }).connect(1000);
            expect(OBS_EVENT_SUBSCRIPTION_INPUT_VOLUME_METERS).toBe(65536);
            expect(IdentifyingSocket.identifies.map((d) => d.eventSubscriptions)).toEqual([0, 65536, 0]);
            expect(IdentifyingSocket.identifies.every((d) => d.rpcVersion === 1)).toBe(true);
        } finally {
            vi.unstubAllGlobals();
        }
    });

    it("届いたメッセージは events に積まれる", async () => {
        const client = new ObsWs(4460, { eventSubscriptions: OBS_EVENT_SUBSCRIPTION_INPUT_VOLUME_METERS });
        try {
            let socket: IdentifyingSocket | null = null;
            vi.stubGlobal(
                "WebSocket",
                class extends IdentifyingSocket {
                    constructor(url: string) {
                        super(url);
                        socket = this;
                    }
                },
            );
            await client.connect(1000);
            socket!.onmessage?.({ data: JSON.stringify({ op: 5, d: { eventType: "InputVolumeMeters", eventData: { inputs: [] } } }) });
            expect(client.events).toEqual([{ eventType: "InputVolumeMeters", eventData: { inputs: [] } }]);
        } finally {
            vi.unstubAllGlobals();
        }
    });
});
