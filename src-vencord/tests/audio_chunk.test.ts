// SPDX-License-Identifier: Apache-2.0

// audio/chunk.ts のテスト。断片を 480 サンプル (10 ms) のチャンクに分けることと、チャンクのタイムスタンプの決め方
// (断片のメディアのタイムスタンプを encode/clock_map.ts で送信側の時計に換算し、チャンクの時刻は、その先頭サンプルが
// 入っていた断片の時刻 + 断片の中の位置) を確認する。累積のサンプル数からは時刻を作らない。

import { describe, expect, it } from "vitest";

import { AUDIO_CHANNELS, AUDIO_SAMPLES_PER_CHUNK, createAudioChunker, pushAudio } from "../audio/chunk";
import { createClockMap } from "../encode/clock_map";

const FRAME_SAMPLES = AUDIO_SAMPLES_PER_CHUNK * AUDIO_CHANNELS; // 1 チャンク分のインターリーブの長さ (960)
const CHUNK_US = 10_000n; // 480 サンプル = 10 ms

/** メディアの時計の原点 (ブラウザーが決める値で、送信側の時計とは別の位置にある)。 */
const MEDIA_EPOCH_US = 2_000_000;
/** 送信側の時計の原点 (エポックに近い値に見立てた値)。 */
const SENDER_EPOCH_US = 1_700_000_000_000_000n;

function makeSamples(length: number, fillFrom = 0): Float32Array {
    const samples = new Float32Array(length);
    for (let i = 0; i < length; i++) samples[i] = fillFrom + i;
    return samples;
}

/** n 個目の断片 (10 ms ごと) のメディアのタイムスタンプ。 */
function mediaUsForChunk(index: number): number {
    return MEDIA_EPOCH_US + index * Number(CHUNK_US);
}

/** その断片が遅れずに届いたときの送信側の時計。 */
function arrivalUsForChunk(index: number): bigint {
    return SENDER_EPOCH_US + BigInt(index) * CHUNK_US;
}

describe("createAudioChunker", () => {
    it("初期状態は空", () => {
        expect(createAudioChunker()).toEqual({
            pending: new Float32Array(0),
            pendingTimeUs: undefined,
            clockMap: createClockMap(),
        });
    });
});

describe("pushAudio: ちょうど 480 サンプルの断片", () => {
    it("1 回で 1 チャンクを作り、時刻はメディアのタイムスタンプを送信側の時計に換算した値になる", () => {
        const samples = makeSamples(FRAME_SAMPLES);
        const { state, chunks } = pushAudio(createAudioChunker(), samples, mediaUsForChunk(0), arrivalUsForChunk(0));
        expect(chunks).toHaveLength(1);
        expect(chunks[0]?.samples).toEqual(samples);
        expect(chunks[0]?.timestampUs).toBe(SENDER_EPOCH_US);
        expect(state.pending).toHaveLength(0);
        expect(state.pendingTimeUs).toBeUndefined();
    });
});

describe("pushAudio: 端数の持ち越し", () => {
    it("480 サンプル未満の断片はチャンクを作らず、pending とその先頭の時刻を持ち越す", () => {
        const half = makeSamples(FRAME_SAMPLES / 2);
        const { state, chunks } = pushAudio(createAudioChunker(), half, mediaUsForChunk(0), arrivalUsForChunk(0));
        expect(chunks).toHaveLength(0);
        expect(state.pending).toEqual(half);
        expect(state.pendingTimeUs).toBe(SENDER_EPOCH_US);
    });

    it("持ち越した分と次の断片をつないだチャンクは持ち越した分の時刻を持ち、残りは次の断片の時刻 + 位置を持つ", () => {
        // 1 つ目の断片は 240 サンプル (5 ms)、2 つ目は 480 サンプル。
        const first = makeSamples(FRAME_SAMPLES / 2);
        const s1 = pushAudio(createAudioChunker(), first, MEDIA_EPOCH_US, SENDER_EPOCH_US);
        const second = makeSamples(FRAME_SAMPLES, 10_000);
        const s2 = pushAudio(s1.state, second, MEDIA_EPOCH_US + 5_000, SENDER_EPOCH_US + 5_000n);
        expect(s2.chunks).toHaveLength(1);
        expect(s2.chunks[0]?.timestampUs).toBe(SENDER_EPOCH_US);
        expect(Array.from(s2.chunks[0]?.samples ?? []).slice(FRAME_SAMPLES / 2, FRAME_SAMPLES / 2 + 2)).toEqual([
            10_000, 10_001,
        ]);
        // 残りの 240 サンプルは、2 つ目の断片の 240 サンプル目 (5 ms 後) から始まる。
        expect(s2.state.pending).toHaveLength(FRAME_SAMPLES / 2);
        expect(s2.state.pendingTimeUs).toBe(SENDER_EPOCH_US + 5_000n + 5_000n);
    });

    it("1 回の pushAudio() で複数のチャンク分が届いたら、断片の時刻から 10 ms ずつ進めて複数のチャンクを返し、端数だけを残す", () => {
        const samples = makeSamples(FRAME_SAMPLES * 2 + 4);
        const { state, chunks } = pushAudio(createAudioChunker(), samples, mediaUsForChunk(0), arrivalUsForChunk(0));
        expect(chunks.map((c) => c.timestampUs)).toEqual([SENDER_EPOCH_US, SENDER_EPOCH_US + CHUNK_US]);
        expect(state.pending).toHaveLength(4);
        expect(state.pendingTimeUs).toBe(SENDER_EPOCH_US + 2n * CHUNK_US);
    });
});

describe("pushAudio: 時刻は断片ごとのメディアのタイムスタンプから決める", () => {
    it("音の時計が送信側の時計より 1000 ppm 遅いと、チャンクの時刻は 10 ms ではなく 10.01 ms ずつ進む (サンプル数で数えない)", () => {
        let state = createAudioChunker();
        const stamps: bigint[] = [];
        for (let i = 0; i < 100; i++) {
            const media = MEDIA_EPOCH_US + i * 10_010;
            const result = pushAudio(state, makeSamples(FRAME_SAMPLES), media, SENDER_EPOCH_US + BigInt(i * 10_010) + 1_000n);
            state = result.state;
            for (const chunk of result.chunks) stamps.push(chunk.timestampUs);
        }
        for (let i = 1; i < stamps.length; i++) expect((stamps[i] as bigint) - (stamps[i - 1] as bigint)).toBe(10_010n);
    });

    it("起動のときの詰まりで最初の断片が遅れて届いても、その遅れはタイムスタンプに残らない", () => {
        let state = createAudioChunker();
        const stamps: bigint[] = [];
        const lags = [150_000n, 140_000n, 2_000n, 3_000n, 2_000n, 2_500n, 2_000n];
        lags.forEach((lag, i) => {
            const result = pushAudio(state, makeSamples(FRAME_SAMPLES), mediaUsForChunk(i), arrivalUsForChunk(i) + lag);
            state = result.state;
            for (const chunk of result.chunks) stamps.push(chunk.timestampUs);
        });
        // 定数は小さいほうから 3 番目の遅れ (2_000 us)。最後の断片の時刻は、遅れずに届いた時刻 + 2 ms。
        expect(stamps[stamps.length - 1]).toBe(arrivalUsForChunk(lags.length - 1) + 2_000n);
    });

    it("取り出しが詰まって遅れて届いた断片は、タイムスタンプをずらさない", () => {
        let state = createAudioChunker();
        const stamps: bigint[] = [];
        for (let i = 0; i < 300; i++) {
            const lag = i === 250 ? 120_000n : 1_000n;
            const result = pushAudio(state, makeSamples(FRAME_SAMPLES), mediaUsForChunk(i), arrivalUsForChunk(i) + lag);
            state = result.state;
            for (const chunk of result.chunks) stamps.push(chunk.timestampUs);
        }
        for (let i = 1; i < stamps.length; i++) expect((stamps[i] as bigint) - (stamps[i - 1] as bigint)).toBe(CHUNK_US);
    });
});

describe("pushAudio: 不正な入力", () => {
    it("チャンネル数の倍数でない長さの断片は例外を投げる", () => {
        const oddLength = new Float32Array(AUDIO_CHANNELS + 1);
        expect(() => pushAudio(createAudioChunker(), oddLength, mediaUsForChunk(0), arrivalUsForChunk(0))).toThrow(
            RangeError,
        );
    });

    it("長さ 0 の断片ではチャンクを作らず、端数とその時刻を変えない", () => {
        const partial = makeSamples(FRAME_SAMPLES / 2);
        const { state: withPartial } = pushAudio(createAudioChunker(), partial, mediaUsForChunk(0), arrivalUsForChunk(0));
        const { state, chunks } = pushAudio(withPartial, new Float32Array(0), MEDIA_EPOCH_US + 5_000, arrivalUsForChunk(1));
        expect(chunks).toHaveLength(0);
        expect(state.pending).toEqual(withPartial.pending);
        expect(state.pendingTimeUs).toBe(withPartial.pendingTimeUs);
        expect(state.clockMap.restarts).toBe(0);
    });
});
