// SPDX-License-Identifier: Apache-2.0

// audio/track_source.ts のテスト。reader.read() が失敗しても next() が reject せずにソースを終わらせることと、
// AudioData の変換を確認する。MediaStreamTrackProcessor はブラウザーの実行時にしかないので、vi.stubGlobal で
// テスト用の実装に置き換えてから createAudioTrackSource() を呼び出す。

import { afterEach, describe, expect, it, vi } from "vitest";

import type { MediaStreamTrackLike } from "../adapter/types";
import { createAudioTrackSource } from "../audio/track_source";

interface FakeAudioData {
    sampleRate: number;
    numberOfFrames: number;
    numberOfChannels: number;
    timestamp: number;
    copyTo(dest: Float32Array, options: { planeIndex: number }): void;
    close(): void;
}

function makeFakeAudioData(frames: number, sampleRate: number, channels: number, timestamp = 0): { data: FakeAudioData; closeCalls: () => number } {
    let closed = 0;
    const data: FakeAudioData = {
        sampleRate,
        numberOfFrames: frames,
        numberOfChannels: channels,
        timestamp,
        copyTo(dest, { planeIndex }) {
            for (let i = 0; i < frames; i++) dest[i] = planeIndex + 1; // チャンネルごとに違う値にする。
        },
        close() {
            closed++;
        },
    };
    return { data, closeCalls: () => closed };
}

type ReadResult = { value: FakeAudioData | undefined; done: boolean };

/**
 * MediaStreamTrackProcessor.readable.getReader() が返す reader の、テスト用の実装。`steps` の要素を Error にすると、
 * その呼び出しの read() が reject する。
 */
function makeFakeReader(steps: (ReadResult | Error)[]) {
    let i = 0;
    return {
        read: vi.fn(async (): Promise<ReadResult> => {
            const step = steps[Math.min(i, steps.length - 1)];
            i++;
            if (step === undefined) return { value: undefined, done: true };
            if (step instanceof Error) throw step;
            return step;
        }),
        cancel: vi.fn(async () => {
            // 何もしない (テストの対象ではない)。
        }),
    };
}

function makeFakeTrack(): MediaStreamTrackLike {
    return { id: "track-1", kind: "audio", readyState: "live", stop: vi.fn() };
}

function stubProcessor(reader: ReturnType<typeof makeFakeReader>): void {
    vi.stubGlobal(
        "MediaStreamTrackProcessor",
        class {
            readable = { getReader: () => reader };
        },
    );
}

afterEach(() => {
    vi.unstubAllGlobals();
});

describe("createAudioTrackSource: reader.read() が失敗しても、reject せずにソースを終わらせる", () => {
    it("read() が最初から reject しても、next() は reject せずに null を返す", async () => {
        const reader = makeFakeReader([new Error("track ended abnormally")]);
        stubProcessor(reader);

        const source = createAudioTrackSource(makeFakeTrack());
        await expect(source.next()).resolves.toBeNull();
        // 一度失敗したら、その後も null を返し続ける (closed になっていて、reader を呼び出さない)。
        await expect(source.next()).resolves.toBeNull();
        expect(reader.read).toHaveBeenCalledTimes(1);
    });

    it("何回か読めた後に失敗しても、それまでの断片は届き、失敗した回から null になる", async () => {
        const { data: goodData } = makeFakeAudioData(480, 48_000, 2);
        const reader = makeFakeReader([{ value: goodData, done: false }, new Error("boom")]);
        stubProcessor(reader);

        const source = createAudioTrackSource(makeFakeTrack());
        const first = await source.next();
        expect(first).not.toBeNull();
        expect(first?.samples.length).toBe(480 * 2); // 480 フレーム x 2 チャンネル (f32 のインターリーブ)。

        await expect(source.next()).resolves.toBeNull();
    });

    it("close() を呼んだ後に read() が失敗しても、例外にならずに null を返す", async () => {
        const reader = makeFakeReader([new Error("boom")]);
        stubProcessor(reader);

        const source = createAudioTrackSource(makeFakeTrack());
        source.close();
        await expect(source.next()).resolves.toBeNull();
    });
});

describe("createAudioTrackSource: 正しい入力", () => {
    it("48 kHz、2 チャンネルの AudioData を、f32 のインターリーブに変換して返す", async () => {
        const { data } = makeFakeAudioData(4, 48_000, 2, 123_456);
        const reader = makeFakeReader([{ value: data, done: false }]);
        stubProcessor(reader);

        const source = createAudioTrackSource(makeFakeTrack());
        const result = await source.next();
        expect(result).not.toBeNull();
        // 取り出した時刻ではなく、AudioData.timestamp をそのまま返す。
        expect(result?.mediaTimestampUs).toBe(123_456);
        expect(result?.samples.length).toBe(8); // 4 フレーム x 2 チャンネル。
    });

    it("48 kHz でない AudioData は中身を読まずに捨て、droppedForSampleRate を増やす", async () => {
        const { data: badData, closeCalls } = makeFakeAudioData(4, 44_100, 2, 10_000);
        const { data: goodData } = makeFakeAudioData(4, 48_000, 2, 20_000);
        const reader = makeFakeReader([{ value: badData, done: false }, { value: goodData, done: false }]);
        stubProcessor(reader);

        const source = createAudioTrackSource(makeFakeTrack());
        const result = await source.next();
        expect(result).not.toBeNull(); // 44.1 kHz を飛ばして、次の 48 kHz を返す。
        expect(result?.mediaTimestampUs).toBe(20_000); // 捨てた方のタイムスタンプは返らない。
        expect(closeCalls()).toBe(1);
        expect(source.droppedForSampleRate).toBe(1);
    });
});
