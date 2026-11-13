// SPDX-License-Identifier: Apache-2.0

// encode/latency_span.ts の足し上げと行の書式、encode/session.ts が段階ごとの所要時間を測ること (偽の時計で) を確認する。

import { describe, expect, it } from "vitest";

import type { VideoFrameSource } from "../encode/frame_source";
import {
    addDuration,
    createDurationStat,
    createEncodeLatencySpan,
    hasLatencySamples,
    latencyLine,
    resetEncodeLatencySpan,
    summarizeDuration,
} from "../encode/latency_span";
import type { SourceFormat } from "../encode/ladder";
import { SerialGate } from "../encode/serial";
import { createEncodeSession, type EncodeSessionDeps, type EncodeSessionSink } from "../encode/session";
import type { EncodedVideoChunk, VideoEncoderLike, VideoFrame } from "../encode/webcodecs_types";

describe("addDuration と summarizeDuration", () => {
    it("何も足していなければ、数は 0 で平均と最大はない", () => {
        const s = summarizeDuration(createDurationStat());
        expect(s.count).toBe(0);
        expect(s.avgMs).toBeUndefined();
        expect(s.maxMs).toBeUndefined();
    });

    it("平均と最大を ms で返す", () => {
        const stat = createDurationStat();
        addDuration(stat, 1_000);
        addDuration(stat, 2_000);
        addDuration(stat, 6_000);
        const s = summarizeDuration(stat);
        expect(s.count).toBe(3);
        expect(s.avgMs).toBeCloseTo(3.0, 9);
        expect(s.maxMs).toBeCloseTo(6.0, 9);
    });

    it("負の値は 0 として数え、有限でない値は足さない", () => {
        const stat = createDurationStat();
        addDuration(stat, -5_000);
        addDuration(stat, Number.NaN);
        addDuration(stat, Number.POSITIVE_INFINITY);
        addDuration(stat, 4_000);
        const s = summarizeDuration(stat);
        expect(s.count).toBe(2);
        expect(s.avgMs).toBeCloseTo(2.0, 9);
        expect(s.maxMs).toBeCloseTo(4.0, 9);
    });
});

describe("latencyLine", () => {
    it("1 つも足していなければ行を書かない", () => {
        const span = createEncodeLatencySpan();
        expect(hasLatencySamples(span)).toBe(false);
        expect(latencyLine(2, span)).toBeUndefined();
    });

    it("段ごとの平均と最大と数を 1 行に書き、足していない段は none と書く", () => {
        const span = createEncodeLatencySpan();
        addDuration(span.read, 1_000);
        addDuration(span.read, 3_000);
        addDuration(span.encode, 2_500);
        expect(latencyLine(2, span)).toBe(
            "latency stream 2: read.avg=2.0ms read.max=3.0ms enc.avg=2.5ms enc.max=2.5ms" +
                " prep.avg=none prep.max=none send.avg=none send.max=none n=2/1/0/0",
        );
    });

    it("行を作っても span は変わらず、reset で空に戻る", () => {
        const span = createEncodeLatencySpan();
        addDuration(span.send, 100);
        latencyLine(1, span);
        expect(span.send.count).toBe(1);
        resetEncodeLatencySpan(span);
        expect(span.send.count).toBe(0);
        expect(span.send.maxUs).toBe(0);
        expect(hasLatencySamples(span)).toBe(false);
    });
});

// ---- session.ts が測ること --------------------------------------------------

const FORMAT: SourceFormat = { width: 1920, height: 1080, fps: 60 };

async function flushMicrotasks(times = 40): Promise<void> {
    for (let i = 0; i < times; i++) await Promise.resolve();
}

function makeFrame(timestamp: number): VideoFrame {
    return { timestamp, close: () => undefined };
}

/** push() で渡したフレームを next() で返す。なければ待つ。 */
function createSource() {
    const queue: VideoFrame[] = [];
    let waiter: ((frame: VideoFrame | null) => void) | undefined;
    let ended = false;
    const source: VideoFrameSource = {
        format: FORMAT,
        next() {
            if (queue.length > 0) return Promise.resolve(queue.shift() as VideoFrame);
            if (ended) return Promise.resolve(null);
            return new Promise((resolve) => {
                waiter = resolve;
            });
        },
        close() {
            ended = true;
            waiter?.(null);
            waiter = undefined;
        },
    };
    return {
        source,
        push(frame: VideoFrame) {
            if (waiter) {
                const resolve = waiter;
                waiter = undefined;
                resolve(frame);
            } else {
                queue.push(frame);
            }
        },
    };
}

/**
 * encode() の中で同期に出力するエンコーダと、値を順に返す 2 つの時計を持つ deps を作る。nowUs はフレームの到着の時刻
 * (フレームごとに 1 回)、monotonicUs は 1 フレームにつき 4 回 (エンコーダに渡す前、出力を受け取った時、sendVideo の前、
 * sendVideo の後) 読まれる。
 */
function makeDeps(arrivals: bigint[], monotonic: number[] | undefined): EncodeSessionDeps {
    let arrivalIndex = 0;
    let monotonicIndex = 0;
    return {
        async isConfigSupported() {
            return { supported: true };
        },
        createEncoder(init) {
            const encoder: VideoEncoderLike = {
                encodeQueueSize: 0,
                configure() {},
                encode(frame, options) {
                    const payload = new Uint8Array([1, 2, 3, 4]);
                    const chunk: EncodedVideoChunk = {
                        type: options?.keyFrame ? "key" : "delta",
                        timestamp: frame.timestamp,
                        byteLength: payload.byteLength,
                        copyTo: (dest: Uint8Array) => dest.set(payload),
                    };
                    init.output(chunk, {});
                },
                async flush() {},
                close() {},
            };
            return encoder;
        },
        gate: new SerialGate(),
        nowUs: () => arrivals[Math.min(arrivalIndex++, arrivals.length - 1)] as bigint,
        ...(monotonic === undefined
            ? {}
            : { monotonicUs: () => monotonic[Math.min(monotonicIndex++, monotonic.length - 1)] as number }),
        makeBlackFrame: (_w: number, _h: number, timestampUs: number) => makeFrame(timestampUs),
        scaleFrame: (frame: VideoFrame) => frame,
    };
}

const sink: EncodeSessionSink = {
    sendVideo() {},
    onStage() {},
    onRecordingMaterial() {},
};

function startSession(deps: EncodeSessionDeps) {
    const { source, push } = createSource();
    const session = createEncodeSession(
        deps,
        {
            streamId: 7,
            source,
            preference: "prefer-hardware",
            limits: { fpsDivisor: 1 },
            latencyMode: "realtime",
            bitrateFactor: 1,
            warmup: false,
            keyintSec: 100,
        },
        sink,
    );
    return { session, push };
}

describe("createEncodeSession: 段階ごとの所要時間", () => {
    it("読み出しの遅れ、エンコード、出力の準備、送信を、偽の時計の差で測る", async () => {
        // 1 フレーム目: メディア 10 us、到着 1_000_010 us -> 原点 1_000_000 us。
        // 2 フレーム目: メディア 16_677 us、到着 1_000_000 + 16_677 + 5_000 us -> 変換した時刻より 5 ms 遅れて読めた。
        const arrivals = [1_000_010n, 1_021_677n];
        const monotonic = [
            100, 2_100, 2_300, 2_350, // 1 フレーム目: encode 2.0 ms、prepare 0.2 ms、send 0.05 ms
            10_000, 10_500, 10_520, 10_600, // 2 フレーム目: encode 0.5 ms、prepare 0.02 ms、send 0.08 ms
        ];
        const { session, push } = startSession(makeDeps(arrivals, monotonic));
        push(makeFrame(10));
        await session.start();
        await flushMicrotasks();
        push(makeFrame(16_677));
        await flushMicrotasks();

        const span = session.takeLatencySpan();
        expect(span.read.count).toBe(2);
        expect(summarizeDuration(span.read).avgMs).toBeCloseTo(2.5, 9);
        expect(summarizeDuration(span.read).maxMs).toBeCloseTo(5.0, 9);
        expect(span.encode.count).toBe(2);
        expect(summarizeDuration(span.encode).avgMs).toBeCloseTo(1.25, 9);
        expect(summarizeDuration(span.encode).maxMs).toBeCloseTo(2.0, 9);
        expect(summarizeDuration(span.prepare).avgMs).toBeCloseTo(0.11, 9);
        expect(summarizeDuration(span.prepare).maxMs).toBeCloseTo(0.2, 9);
        expect(summarizeDuration(span.send).avgMs).toBeCloseTo(0.065, 9);
        expect(summarizeDuration(span.send).maxMs).toBeCloseTo(0.08, 9);

        session.stop();
    });

    it("取り出すと空に戻り、取り出した値は後のフレームで変わらない", async () => {
        const arrivals = [1_000_010n, 1_016_687n];
        const monotonic = [0, 1_000, 1_100, 1_200, 5_000, 6_000, 6_100, 6_200];
        const { session, push } = startSession(makeDeps(arrivals, monotonic));
        push(makeFrame(10));
        await session.start();
        await flushMicrotasks();

        const first = session.takeLatencySpan();
        expect(first.encode.count).toBe(1);
        const emptied = session.takeLatencySpan();
        expect(emptied.read.count).toBe(0);
        expect(emptied.encode.count).toBe(0);
        expect(hasLatencySamples(emptied)).toBe(false);

        push(makeFrame(16_677));
        await flushMicrotasks();
        expect(first.encode.count).toBe(1); // 取り出した値は書き換わらない
        expect(session.takeLatencySpan().encode.count).toBe(1);

        session.stop();
    });

    it("読み出した時刻 - 換算した時刻 を、負の値も残してパーセンタイルにし、取り出すと空に戻る (encode/clock_lag.ts)", async () => {
        // 1 フレーム目で原点が決まる (遅れ 0)。2 フレーム目は換算した時刻より 3 ms 早く読めた (定数を下げるには、同じ期間に
        // 3 つ要るので、まだ下げない)。読み出しの遅れ (read) は負の値を 0 として数えるが、こちらは -3 ms のまま数える。
        const arrivals = [1_000_010n, 1_013_677n];
        const { session, push } = startSession(makeDeps(arrivals, undefined));
        push(makeFrame(10));
        await session.start();
        await flushMicrotasks();
        push(makeFrame(16_677));
        await flushMicrotasks();

        const lag = session.takeClockLag();
        expect(lag).toEqual({ count: 2, p5Us: -3_000, p50Us: -3_000, p95Us: 0 });
        expect(summarizeDuration(session.takeLatencySpan().read).avgMs).toBeCloseTo(0, 9);
        expect(session.takeClockLag().count).toBe(0);

        session.stop();
    });

    it("monotonicUs を渡さなければ、読み出しの遅れだけを測り、ほかの段は測らない", async () => {
        const { session, push } = startSession(makeDeps([1_000_010n], undefined));
        push(makeFrame(10));
        await session.start();
        await flushMicrotasks();

        const span = session.takeLatencySpan();
        expect(span.read.count).toBe(1);
        expect(span.encode.count).toBe(0);
        expect(span.prepare.count).toBe(0);
        expect(span.send.count).toBe(0);

        session.stop();
    });
});
