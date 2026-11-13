// SPDX-License-Identifier: Apache-2.0

// encode/session.ts のテスト。VideoEncoder、isConfigSupported、VideoFrameSource、scaleFrame をテスト用の実装に置き換え、
// フォールバック、ウォームアップ、キーフレームの要求、configure() を 1 つずつ順に行うこと、maxLongEdge の縮小、黒埋め用の
// フレーム、SPS と PPS の付け足し、走っている間の上限の変更を確認する。

import { describe, expect, it } from "vitest";

import { FrameType } from "../core/wire";
import type { VideoFrameSource } from "../encode/frame_source";
import { type SourceFormat, LadderStage, codecStringForResolution } from "../encode/ladder";
import { qpcTimebase } from "../encode/sender_clock";
import { SerialGate } from "../encode/serial";
import type { VideoTimeChange } from "../encode/video_time";
import {
    createEncodeSession,
    type ClockReanchorInfo,
    type EncodedVideoFormat,
    type EncodeSessionDeps,
    type EncodeSessionParams,
    type EncodeSessionSink,
    type RecordingMaterialOrigin,
} from "../encode/session";
import type {
    EncodedVideoChunk,
    VideoEncoderConfig,
    VideoEncoderLike,
    VideoFrame,
} from "../encode/webcodecs_types";

// ---- テスト用の関数 -------------------------------------------------------

/** マイクロタスクを n 回進める (session.ts はタイマーを持たず、Promise の連鎖だけで動くため)。 */
async function flushMicrotasks(times = 40): Promise<void> {
    for (let i = 0; i < times; i++) {
        await Promise.resolve();
    }
}

function makeVideoFrame(timestamp: number, closedLog: number[]): VideoFrame {
    return {
        timestamp,
        close: () => {
            closedLog.push(timestamp);
        },
    };
}

/** テスト用の VideoFrameSource。push() でフレームを渡し、なければ next() は待つ。 */
function createFakeSource(format: SourceFormat) {
    const queue: VideoFrame[] = [];
    let waiter: ((frame: VideoFrame | null) => void) | undefined;
    let ended = false;
    let closeCallCount = 0;

    const source: VideoFrameSource = {
        format,
        next(): Promise<VideoFrame | null> {
            if (queue.length > 0) return Promise.resolve(queue.shift() as VideoFrame);
            if (ended) return Promise.resolve(null);
            return new Promise((resolve) => {
                waiter = resolve;
            });
        },
        close(): void {
            closeCallCount++;
            if (ended) return;
            ended = true;
            if (waiter) {
                const resolve = waiter;
                waiter = undefined;
                resolve(null);
            }
        },
    };

    return {
        source,
        push(frame: VideoFrame): void {
            if (waiter) {
                const resolve = waiter;
                waiter = undefined;
                resolve(frame);
            } else {
                queue.push(frame);
            }
        },
        getCloseCallCount: () => closeCallCount,
    };
}

interface EncoderScript {
    /**
     * isConfigSupported の呼び出しの番号 (0 から。ladder.ts の Native が 0 段階目という意味ではなく、何回目の試行か) の
     * うち、false を返すものの一覧。
     */
    rejectSupportAt?: number[];
    /** configure() が同期で例外を投げる試行の番号。 */
    throwOnConfigureAt?: number[];
    /** flush() が reject する試行の番号 (ウォームアップの最中にエンコーダが失敗する場合)。 */
    rejectFlushAt?: number[];
    /**
     * encode() の呼び出しごとに呼ぶ。'throw' なら同期で例外を投げ、'error' なら init.error() を呼んで出力なしで終える
     * (VideoEncoder の非同期のエラーに当たる)。それ以外 (undefined) なら通常どおり出力する。
     */
    onEncode?: (attemptIndex: number, encodeCallIndex: number, keyFrame: boolean) => "throw" | "error" | undefined;
    /** true なら、scaleFrame() が (元のフレームを閉じてから) 例外を投げる。OffscreenCanvas が途中で失敗する場合。 */
    throwOnScale?: boolean;
    /**
     * 出力を、encode() の呼び出し N 回分遅らせる。既定 (0) のテスト用のエンコーダは encode() の中で同期で output を
     * 呼ぶので、切り替えた瞬間に前のフレームの出力が新しいフラグで出る、という形を作れない。1 以上にすると、
     * VideoEncoder と同じく出力が後ろにずれる。たまった分を出すのは flush() である (VideoEncoder は close() で捨てるので、
     * close() では出さない)。
     */
    outputDelayCalls?: number;
    /**
     * true なら、出力を自分からは 1 つも出さず、テストが `heldEncoders[作った順].release(n)` で出す (`flush()` は、
     * たまった出力が空になるまで待つ)。2 つのエンコーダの出力が届く順をテストで決め、recording-turned-on の
     * 黒埋め用のフレームと配信のフレームが交互に届く形を作るため。
     */
    holdOutputs?: boolean;
}

function createFakeDeps(script: EncoderScript = {}, gate: SerialGate = new SerialGate()) {
    let attemptIndex = -1;
    let clock = 1_000n;
    const configuredCfgs: VideoEncoderConfig[] = [];
    const closedAttempts: number[] = [];
    /** createEncoder() が呼ばれるたびに 1 つ入れる (録画が OFF の間は増えない)。 */
    const createdEncoders: number[] = [];
    const blackFramesMade: number[] = [];
    const blackFramesClosed: number[] = [];
    // scaleFrame の呼ばれ方 (解像度と、元のフレームのタイムスタンプ) と、縮小して作ったフレームが閉じられたかを
    // 記録する。
    const scaleCalls: { width: number; height: number; timestamp: number }[] = [];
    const scaledFramesClosed: number[] = [];
    /** `holdOutputs` のとき、作った順に 1 つずつ入れる (たまった出力をテストから出す関数)。 */
    const heldEncoders: { release(count: number): void; pending(): number }[] = [];

    const deps: EncodeSessionDeps = {
        async isConfigSupported(_cfg: VideoEncoderConfig) {
            attemptIndex++;
            const rejected = script.rejectSupportAt?.includes(attemptIndex) ?? false;
            return { supported: !rejected };
        },
        createEncoder(init) {
            const myAttempt = attemptIndex;
            createdEncoders.push(myAttempt);
            let encodeCallCount = 0;
            const delay = script.outputDelayCalls ?? 0;
            /** 出力を待っているチャンク (N 回分遅らせるためのキュー)。 */
            const pendingOutputs: EncodedVideoChunk[] = [];

            const emit = (chunk: EncodedVideoChunk): void => {
                init.output(chunk, {});
            };
            const hold = script.holdOutputs === true;
            /** `holdOutputs` の flush() が、たまった出力が空になるのを待つためのコールバック。 */
            const drainWaiters: (() => void)[] = [];
            const wakeDrainWaiters = (): void => {
                if (pendingOutputs.length > 0) return;
                for (const wake of drainWaiters.splice(0)) wake();
            };
            if (hold) {
                heldEncoders.push({
                    release(count: number): void {
                        for (let i = 0; i < count && pendingOutputs.length > 0; i++) {
                            const ready = pendingOutputs.shift();
                            if (ready !== undefined) emit(ready);
                        }
                        wakeDrainWaiters();
                    },
                    pending: () => pendingOutputs.length,
                });
            }

            const encoder: VideoEncoderLike = {
                encodeQueueSize: 0,
                configure(cfg: VideoEncoderConfig) {
                    configuredCfgs.push(cfg);
                    if (script.throwOnConfigureAt?.includes(myAttempt)) {
                        throw new Error(`fake configure failure (attempt ${myAttempt})`);
                    }
                },
                encode(frame, options) {
                    const keyFrame = options?.keyFrame ?? false;
                    const callIndex = encodeCallCount;
                    encodeCallCount++;
                    const action = script.onEncode?.(myAttempt, callIndex, keyFrame);
                    if (action === "throw") {
                        throw new Error(`fake encode failure (attempt ${myAttempt}, call ${callIndex})`);
                    }
                    if (action === "error") {
                        init.error(new Error(`fake async encoder error (attempt ${myAttempt}, call ${callIndex})`));
                        return;
                    }
                    const payload = new Uint8Array([1, 2, 3, 4]);
                    const chunk: EncodedVideoChunk = {
                        type: keyFrame ? "key" : "delta",
                        timestamp: frame.timestamp,
                        byteLength: payload.byteLength,
                        copyTo: (dest: Uint8Array) => dest.set(payload),
                    };
                    pendingOutputs.push(chunk);
                    if (hold) return; // 出すのはテスト (release) だけ。
                    while (pendingOutputs.length > delay) {
                        const ready = pendingOutputs.shift();
                        if (ready !== undefined) emit(ready);
                    }
                },
                async flush() {
                    if (script.rejectFlushAt?.includes(myAttempt)) {
                        throw new Error(`fake flush failure (attempt ${myAttempt})`);
                    }
                    // VideoEncoder の flush() は同期では終わらない (たまっている出力は後から出る)。ここでマイクロタスクを
                    // 1 つはさまないと、黒埋め用のフレームを出し切ってから閉じることを確認しても、呼んだ瞬間に全部出て
                    // いる形になってしまう。
                    await Promise.resolve();
                    if (hold) {
                        // テストが release() で出し切るまで待つ (VideoEncoder と同じく、出し切ってから解決する)。
                        while (pendingOutputs.length > 0) {
                            await new Promise<void>((resolve) => drainWaiters.push(resolve));
                        }
                        return;
                    }
                    // VideoEncoder と同じく、たまっている出力を出し切ってから解決する。
                    while (pendingOutputs.length > 0) {
                        const ready = pendingOutputs.shift();
                        if (ready !== undefined) emit(ready);
                    }
                },
                close() {
                    closedAttempts.push(myAttempt);
                    pendingOutputs.length = 0; // VideoEncoder は、たまっている出力を捨てる。
                    wakeDrainWaiters();
                },
            };
            return encoder;
        },
        gate,
        nowUs: () => {
            const value = clock;
            clock += 1n;
            return value;
        },
        makeBlackFrame(_width: number, _height: number, timestampUs: number): VideoFrame {
            blackFramesMade.push(timestampUs);
            return makeVideoFrame(timestampUs, blackFramesClosed);
        },
        scaleFrame(frame: VideoFrame, width: number, height: number): VideoFrame {
            scaleCalls.push({ width, height, timestamp: frame.timestamp });
            // encode/session.ts との約束で、成功しても失敗しても、元のフレームはこの関数が閉じる。
            frame.close();
            if (script.throwOnScale === true) throw new Error("fake scaleFrame failure");
            return makeVideoFrame(frame.timestamp, scaledFramesClosed);
        },
    };

    return {
        deps,
        configuredCfgs,
        closedAttempts,
        createdEncoders,
        blackFramesMade,
        blackFramesClosed,
        scaleCalls,
        scaledFramesClosed,
        heldEncoders,
        getAttemptCount: () => attemptIndex + 1,
    };
}

/** sink が受け取ったメッセージ 1 つ分。 */
type SinkRecord = { header: Parameters<EncodeSessionSink["sendVideo"]>[0]; payload: Uint8Array };

function createRecordingSink() {
    const records: SinkRecord[] = [];
    const stages: LadderStage[] = [];
    /** onStage の 2 つ目の引数 (configure に入れる解像度)。 */
    const stageFormats: (EncodedVideoFormat | undefined)[] = [];
    /** onRecordingMaterial の呼ばれ方 (黒埋め用のフレームを作った 3 つのタイミング)。 */
    const materials: { origin: RecordingMaterialOrigin; frameCount: number }[] = [];
    const sink: EncodeSessionSink = {
        sendVideo(header, payload) {
            records.push({ header, payload });
        },
        onStage(stage, format) {
            stages.push(stage);
            stageFormats.push(format);
        },
        onRecordingMaterial(origin, frameCount) {
            materials.push({ origin, frameCount });
        },
    };
    return { sink, records, stages, stageFormats, materials };
}

/** 録画だけに使うフラグ (recordingOnly) が立っていないメッセージだけ (配信に使う映像)。 */
function realFrames(records: readonly SinkRecord[]): SinkRecord[] {
    return records.filter((r) => !r.header.flags.recordingOnly);
}

/** 録画だけに使うフラグ (recordingOnly) が立ったメッセージだけ (黒埋め用のフレーム)。 */
function materialFrames(records: readonly SinkRecord[]): SinkRecord[] {
    return records.filter((r) => r.header.flags.recordingOnly);
}

const SOURCE_FORMAT: SourceFormat = { width: 1920, height: 1080, fps: 60 };

function makeParams(source: VideoFrameSource, overrides: Partial<EncodeSessionParams> = {}): EncodeSessionParams {
    return {
        streamId: 7,
        source,
        preference: "prefer-hardware",
        limits: { fpsDivisor: 1 },
        latencyMode: "realtime",
        bitrateFactor: 1,
        warmup: true,
        keyintSec: 100, // 既定は十分に長くして、テストの途中で一定の間隔のキーフレームが混ざらないようにする
        ...overrides,
    };
}

// ---- テスト ---------------------------------------------------------------

describe("createEncodeSession: 基本の成功経路", () => {
    it("Native の段階で成功し、最初のフレームだけをキーフレームで送り、timestamp_us はメディアのタイムスタンプを送信側の時計に換算した値になる", async () => {
        const { deps } = createFakeDeps();
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records, stages } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);

        push(makeVideoFrame(10, closedLog));
        await session.start();
        await flushMicrotasks();

        expect(stages).toEqual([LadderStage.Native]);
        expect(records).toHaveLength(1);
        expect(records[0]?.header.type).toBe(FrameType.Video);
        expect(records[0]?.header.streamId).toBe(7);
        expect(records[0]?.header.flags.keyframe).toBe(true);
        expect(records[0]?.header.flags.recordingOnly).toBe(false);
        expect(closedLog).toEqual([10]); // 使い終えた VideoFrame は閉じる

        push(makeVideoFrame(11, closedLog));
        await flushMicrotasks();
        expect(records).toHaveLength(2);
        expect(records[1]?.header.flags.keyframe).toBe(false); // keyintSec が長いので、2 フレーム目はキーフレームでない

        session.stop();
    });
});

describe("createEncodeSession: タイムスタンプはメディアのタイムスタンプから作る", () => {
    it("取り出しが詰まって届く時刻がばらついても、送るタイムスタンプの間隔は VideoFrame.timestamp の間隔のまま", async () => {
        // レンダラーが詰まって、フレームがまとめて届く。届く遅延がタイムスタンプに入ると、受信側の整列が不連続になる。
        const arrivals = [5_000_000n, 5_100_000n, 5_100_100n]; // 2 フレーム目だけが 83 ms 遅れて、まとめて届く。
        let arrivalIndex = 0;
        const base = createFakeDeps();
        const deps: EncodeSessionDeps = {
            ...base.deps,
            nowUs: () => arrivals[Math.min(arrivalIndex++, arrivals.length - 1)] as bigint,
        };

        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records } = createRecordingSink();
        const closedLog: number[] = [];
        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);

        // メディアのタイムスタンプは 60 fps の等間隔 (原点はブラウザーが決めるので、システム時刻とは別の位置にある)。
        push(makeVideoFrame(1_000_000, closedLog));
        await session.start();
        await flushMicrotasks();
        push(makeVideoFrame(1_016_666, closedLog));
        await flushMicrotasks();
        push(makeVideoFrame(1_033_332, closedLog));
        await flushMicrotasks();

        expect(records).toHaveLength(3);
        // 原点は最初の差 (5_000_000 - 1_000_000)。その後の遅延では原点を上げない。
        expect(records[0]?.header.timestampUs).toBe(5_000_000n);
        expect(records[1]?.header.timestampUs).toBe(5_016_666n);
        expect(records[2]?.header.timestampUs).toBe(5_033_332n);

        session.stop();
    });

    it("最初のフレームが遅れて届き、その後の 3 フレームが時刻どおりなら、3 フレーム目で定数が下がってタイムスタンプが前の値より小さくなる", async () => {
        // 1 フレーム目だけが、起動のときの詰まりで 50 ms 遅れた。
        const arrivals = [5_050_000n, 5_016_666n, 5_033_332n, 5_049_998n];
        let arrivalIndex = 0;
        const base = createFakeDeps();
        const deps: EncodeSessionDeps = {
            ...base.deps,
            nowUs: () => arrivals[Math.min(arrivalIndex++, arrivals.length - 1)] as bigint,
        };

        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records } = createRecordingSink();
        const closedLog: number[] = [];
        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);

        push(makeVideoFrame(1_000_000, closedLog));
        await session.start();
        await flushMicrotasks();
        for (const media of [1_016_666, 1_033_332, 1_049_998]) {
            push(makeVideoFrame(media, closedLog));
            await flushMicrotasks();
        }

        // 今の定数より小さい差が 3 つそろうまでは、遅延が入ったままである (encode/clock_map.ts の CLOCK_MAP_RANK)。
        expect(records[0]?.header.timestampUs).toBe(5_050_000n);
        expect(records[1]?.header.timestampUs).toBe(5_066_666n);
        expect(records[2]?.header.timestampUs).toBe(5_083_332n);
        // 3 つそろった 4 フレーム目で定数が下がり、遅延が消えた。
        expect(records[3]?.header.timestampUs).toBe(5_049_998n);
        // 送るタイムスタンプは単調に増えない (encode/clock_map.ts)。戻る量は、詰まりの 50 ms から 1 フレーム分の
        // 16.666 ms を引いた値である。
        const backwardUs =
            (records[3]?.header.timestampUs as bigint) - (records[2]?.header.timestampUs as bigint);
        expect(backwardUs).toBe(-33_334n);

        session.stop();
    });

    it("メディアのタイムスタンプが有限の数でなくても VideoFrame を残さない (換算できずに止まるときも閉じる)", async () => {
        const { deps } = createFakeDeps();
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records, stages } = createRecordingSink();
        const closedLog: number[] = [];
        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);

        push(makeVideoFrame(Number.NaN, closedLog)); // mapMediaTime が RangeError を投げる。
        await session.start();
        await flushMicrotasks();

        expect(closedLog).toEqual([Number.NaN]); // フレームは閉じてある (VideoFrame を残さない)。
        expect(records).toHaveLength(0);
        expect(stages[stages.length - 1]).toBe(LadderStage.Unavailable); // Unavailable を伝えてから止まる。

        session.stop();
    });
});

describe("createEncodeSession: 送信側の時計が qpc なら、描く予定の時刻のタイムスタンプをそのまま送る", () => {
    const TICKS_ORIGIN_US = 269_692_077_700n;

    it("描く予定の時刻 (読んだ時刻より先) のフレームは、VideoFrame.timestamp をそのまま送り、方法を 1 回だけ伝える", async () => {
        // 読んだ時刻 (送信側の時計 = TimeTicks) は、描く予定の時刻の 12 ms 前。2 フレーム目は詰まりで 40 ms 遅れた。
        const media = [300_000_000_000, 300_000_016_667, 300_000_033_334];
        const arrivals = [299_999_988_000n, 300_000_056_667n, 300_000_021_334n];
        let arrivalIndex = 0;
        const base = createFakeDeps();
        const deps: EncodeSessionDeps = {
            ...base.deps,
            nowUs: () => arrivals[Math.min(arrivalIndex++, arrivals.length - 1)] as bigint,
            timebase: qpcTimebase(TICKS_ORIGIN_US),
        };
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const recording = createRecordingSink();
        const modes: Array<{ change: VideoTimeChange; switched: boolean }> = [];
        const sink: EncodeSessionSink = { ...recording.sink, onTimeMode: (change, switched) => modes.push({ change, switched }) };
        const closedLog: number[] = [];
        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);

        push(makeVideoFrame(media[0] as number, closedLog));
        await session.start();
        await flushMicrotasks();
        for (const m of media.slice(1)) {
            push(makeVideoFrame(m, closedLog));
            await flushMicrotasks();
        }

        expect(recording.records.map((r) => r.header.timestampUs)).toEqual(media.map((m) => BigInt(m)));
        expect(modes).toEqual([{ change: { mode: "exact", reason: "render-time", leadUs: 12_000n }, switched: false }]);
        expect(session.stats().timeMode).toBe("exact");
        expect(session.stats().timeModeSwitches).toBe(0);
        expect(session.clockOriginUs()).toBe(0n);
        session.stop();
    });

    it("途中で描く予定の時刻でなくなって今までの換算に戻ったら、そのフレームをキーフレームにする", async () => {
        // 10 フレームは描く予定の時刻、その後は取り込みの時刻 (先行 -25 ms) が 1 秒より長く続く。
        const media: number[] = [];
        const arrivals: bigint[] = [];
        for (let i = 0; i < 10; i++) {
            const m = 300_000_000_000 + i * 16_667;
            media.push(m);
            arrivals.push(BigInt(m) - 12_000n);
        }
        for (let i = 0; i < 70; i++) {
            const m = 300_000_000_000 + (10 + i) * 16_667 - 26_000;
            media.push(m);
            arrivals.push(BigInt(m) + 25_000n);
        }
        let arrivalIndex = 0;
        const base = createFakeDeps();
        const deps: EncodeSessionDeps = {
            ...base.deps,
            nowUs: () => arrivals[Math.min(arrivalIndex++, arrivals.length - 1)] as bigint,
            timebase: qpcTimebase(TICKS_ORIGIN_US),
        };
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const recording = createRecordingSink();
        const modes: Array<{ change: VideoTimeChange; switched: boolean }> = [];
        const sink: EncodeSessionSink = { ...recording.sink, onTimeMode: (change, switched) => modes.push({ change, switched }) };
        const closedLog: number[] = [];
        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);

        push(makeVideoFrame(media[0] as number, closedLog));
        await session.start();
        await flushMicrotasks();
        for (const m of media.slice(1)) {
            push(makeVideoFrame(m, closedLog));
            await flushMicrotasks();
        }

        expect(modes.map((m) => [m.change.mode, m.switched])).toEqual([
            ["exact", false],
            ["estimated", true],
        ]);
        expect(modes[1]?.change.reason).toBe("not-render-time");
        const records = realFrames(recording.records);
        const exitIndex = records.findIndex((r, i) => i > 0 && r.header.timestampUs !== BigInt(media[i] as number));
        expect(exitIndex).toBeGreaterThan(10);
        // 動きは 1 秒より小さい (取り込みの時刻から読んだ時刻への 25 ms 余り) ので、キーフレームにはしない。
        expect(records[exitIndex]?.header.flags.keyframe).toBe(false);
        // 戻った最初のフレームの時刻は、そのフレームを読んだ時刻 (作り直した clock_map の最初の差)。
        expect(records[exitIndex]?.header.timestampUs).toBe(arrivals[exitIndex]);
        expect(session.stats().timeModeSwitches).toBe(1);
        session.stop();
    });
});

describe("createEncodeSession: 元から読んだ時刻で見分け、黒埋め用のフレームは送った映像より後に置く", () => {
    const TICKS_ORIGIN_US = 269_692_077_700n;

    it("エンコードが詰まって next() が遅れても、元から読んだ時刻 (takeReadAtUs) で先行を測り、正確な換算をやめない", async () => {
        // 描く予定の時刻の 12 ms 前に元から読んだが、エンコードの詰まりで next() は 40 ms 後に返る列 (100 フレーム、1.6 秒)。
        const media: number[] = [];
        for (let i = 0; i < 100; i++) media.push(300_000_000_000 + i * 16_667);
        let index = 0;
        const base = createFakeDeps();
        const deps: EncodeSessionDeps = {
            ...base.deps,
            nowUs: () => BigInt(media[Math.min(index, media.length - 1)] as number) + 40_000n,
            timebase: qpcTimebase(TICKS_ORIGIN_US),
        };
        const fake = createFakeSource(SOURCE_FORMAT);
        const source = {
            ...fake.source,
            takeReadAtUs: (): bigint => BigInt(media[Math.min(index++, media.length - 1)] as number) - 12_000n,
        };
        const recording = createRecordingSink();
        const modes: Array<{ change: VideoTimeChange; switched: boolean }> = [];
        const sink: EncodeSessionSink = { ...recording.sink, onTimeMode: (change, switched) => modes.push({ change, switched }) };
        const closedLog: number[] = [];
        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        fake.push(makeVideoFrame(media[0] as number, closedLog));
        await session.start();
        await flushMicrotasks();
        for (const m of media.slice(1)) {
            fake.push(makeVideoFrame(m, closedLog));
            await flushMicrotasks();
        }
        expect(modes).toHaveLength(1);
        expect(modes[0]?.change.mode).toBe("exact");
        expect(realFrames(recording.records).map((r) => r.header.timestampUs)).toEqual(media.map((m) => BigInt(m)));
        session.stop();
    });

    it("黒埋め用のフレームの原点は、今の時計が送った映像のタイムスタンプより前でも、その後にする", async () => {
        // 描く予定の時刻が読んだ時刻より 150 ms 先のフレームを送った後に、録画を ON にする。
        const base = createFakeDeps();
        const nowRef = { us: 300_000_000_000n - 150_000n };
        const deps: EncodeSessionDeps = { ...base.deps, nowUs: () => nowRef.us, timebase: qpcTimebase(TICKS_ORIGIN_US) };
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const recording = createRecordingSink();
        const closedLog: number[] = [];
        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), recording.sink);
        push(makeVideoFrame(300_000_000_000, closedLog));
        await session.start();
        await flushMicrotasks();
        session.setRecording(true);
        await flushMicrotasks(200);
        const material = materialFrames(recording.records);
        expect(material.length).toBeGreaterThan(0);
        expect((material[0] as SinkRecord).header.timestampUs > 300_000_000_000n).toBe(true);
        session.stop();
    });
});

describe("createEncodeSession: 古いフレームを捨てる", () => {
    // 換算の定数 (encode/clock_map.ts の offsetUs) は、「届いた時刻 - メディアのタイムスタンプ」の小さいほうから決まり、
    // 下がる向きにしか動かない。まず遅延のないフレーム (delta=0) を 1 枚通して定数を 0 に決めてから、届く時刻だけを
    // 遅らせたフレームを流す。その後の delta が 0 以上なら定数は 0 のままで、「届いた時刻 - 換算したタイムスタンプ」は、
    // 狙った遅延 (delta) と一致する。

    it("300 ms 遅れて取り出したフレームはエンコードせずに閉じ、framesSkippedStale が 1 になる", async () => {
        const arrivals = [1_000_000n, 2_300_000n]; // 1 フレーム目は delta 0、2 フレーム目は delta 300 ms
        let arrivalIndex = 0;
        const base = createFakeDeps();
        const deps: EncodeSessionDeps = {
            ...base.deps,
            nowUs: () => arrivals[Math.min(arrivalIndex++, arrivals.length - 1)] as bigint,
        };

        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records } = createRecordingSink();
        const closedLog: number[] = [];
        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);

        push(makeVideoFrame(1_000_000, closedLog));
        await session.start();
        await flushMicrotasks();
        expect(records).toHaveLength(1); // 原点を決める 1 フレーム目は、普通にエンコードする

        push(makeVideoFrame(2_000_000, closedLog));
        await flushMicrotasks();

        expect(records).toHaveLength(1); // 2 フレーム目はエンコードしない
        expect(closedLog).toContain(2_000_000); // それでも VideoFrame は閉じる
        expect(session.stats().framesSkippedStale).toBe(1);
        expect(session.stats().framesEncoded).toBe(1);

        session.stop();
    });

    it("clockOriginUs() は stats().clockOriginUs と同じ値を、新しいオブジェクトを作らずに返す (呼び出し元がフレームごとに読むため)", async () => {
        const base = createFakeDeps();
        const deps: EncodeSessionDeps = { ...base.deps, nowUs: () => 5_000_000n };
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink } = createRecordingSink();
        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);

        expect(session.clockOriginUs()).toBeUndefined(); // まだ 1 フレームも換算していない
        expect(session.stats().clockOriginUs).toBeUndefined();

        push(makeVideoFrame(1_000_000, []));
        await session.start();
        await flushMicrotasks();

        expect(session.clockOriginUs()).toBe(session.stats().clockOriginUs);
        expect(session.clockOriginUs()).not.toBeUndefined();

        session.stop();
    });

    it("100 ms の遅延ならエンコードする", async () => {
        const arrivals = [1_000_000n, 2_100_000n]; // 2 フレーム目は delta 100 ms
        let arrivalIndex = 0;
        const base = createFakeDeps();
        const deps: EncodeSessionDeps = {
            ...base.deps,
            nowUs: () => arrivals[Math.min(arrivalIndex++, arrivals.length - 1)] as bigint,
        };

        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records } = createRecordingSink();
        const closedLog: number[] = [];
        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);

        push(makeVideoFrame(1_000_000, closedLog));
        await session.start();
        await flushMicrotasks();

        push(makeVideoFrame(2_000_000, closedLog));
        await flushMicrotasks();

        expect(records).toHaveLength(2); // 100 ms はしきい値より小さいので、エンコードする
        expect(session.stats().framesSkippedStale).toBe(0);

        session.stop();
    });

    it("ちょうど 200 ms なら捨てる (しきい値を含む)", async () => {
        const arrivals = [1_000_000n, 2_200_000n]; // 2 フレーム目は、ちょうど delta 200 ms
        let arrivalIndex = 0;
        const base = createFakeDeps();
        const deps: EncodeSessionDeps = {
            ...base.deps,
            nowUs: () => arrivals[Math.min(arrivalIndex++, arrivals.length - 1)] as bigint,
        };

        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records } = createRecordingSink();
        const closedLog: number[] = [];
        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);

        push(makeVideoFrame(1_000_000, closedLog));
        await session.start();
        await flushMicrotasks();

        push(makeVideoFrame(2_000_000, closedLog));
        await flushMicrotasks();

        expect(records).toHaveLength(1); // ちょうど 200 ms はしきい値に含まれるので、捨てる
        expect(session.stats().framesSkippedStale).toBe(1);

        session.stop();
    });

    it("捨てたフレームの次のフレームは普通にエンコードし、requestKeyframe は捨てたフレームでは使われない", async () => {
        const arrivals = [1_000_000n, 2_100_000n, 3_300_000n, 4_050_000n]; // 遅延: 0、100 ms、300 ms、50 ms
        let arrivalIndex = 0;
        const base = createFakeDeps();
        const deps: EncodeSessionDeps = {
            ...base.deps,
            nowUs: () => arrivals[Math.min(arrivalIndex++, arrivals.length - 1)] as bigint,
        };

        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records } = createRecordingSink();
        const closedLog: number[] = [];
        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);

        push(makeVideoFrame(1_000_000, closedLog)); // delta 0: 原点を決め、最初のフレームとしてキーフレームになる
        await session.start();
        await flushMicrotasks();
        expect(records[0]?.header.flags.keyframe).toBe(true);

        push(makeVideoFrame(2_000_000, closedLog)); // delta 100 ms: エンコードする。keyintSec が長いので、キーフレームではない
        await flushMicrotasks();
        expect(records).toHaveLength(2);
        expect(records[1]?.header.flags.keyframe).toBe(false);

        session.requestKeyframe();

        push(makeVideoFrame(3_000_000, closedLog)); // delta 300 ms: 捨てる。キーフレームの要求はまだ残る
        await flushMicrotasks();
        expect(records).toHaveLength(2); // 捨てたフレームは送らない
        expect(session.stats().framesSkippedStale).toBe(1);

        push(makeVideoFrame(4_000_000, closedLog)); // delta 50 ms: 普通にエンコードし、要求のとおりキーフレームになる
        await flushMicrotasks();
        expect(records).toHaveLength(3);
        expect(records[2]?.header.flags.keyframe).toBe(true); // requestKeyframe の要求が、捨てたフレームの次で動作した

        session.stop();
    });
});

describe("createEncodeSession: 遅すぎるフレームが 1 秒続いたら、換算の定数を決め直す", () => {
    // 到着の時刻を、フレームを流す前に決める (deps.nowUs はフレームを取り出すたびに 1 回呼ばれる)。60 fps のフレームを、
    // メディアのタイムスタンプ + delay の時刻に取り出したことにする。
    const FRAME_US = 16_667;

    function setup() {
        let arrivalUs = 0n;
        const base = createFakeDeps();
        const deps: EncodeSessionDeps = { ...base.deps, nowUs: () => arrivalUs };
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const recording = createRecordingSink();
        const reanchors: ClockReanchorInfo[] = [];
        const sink: EncodeSessionSink = { ...recording.sink, onClockReanchor: (info) => reanchors.push(info) };
        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        const closedLog: number[] = [];
        /** メディアのタイムスタンプ mediaUs のフレームを、delayUs 遅れて取り出したことにして流す。 */
        const feed = async (mediaUs: number, delayUs: number): Promise<void> => {
            arrivalUs = BigInt(mediaUs + delayUs);
            push(makeVideoFrame(mediaUs, closedLog));
            await flushMicrotasks();
        };
        return { session, records: recording.records, reanchors, feed };
    }

    it("遅れが 300 ms に一段上がって続くと、約 1 秒で定数を決め直し、フレームがまたエンコードに回る", async () => {
        const { session, records, reanchors, feed } = setup();
        await feed(1_000_000, 0); // 原点を 0 に決める
        await session.start();
        await flushMicrotasks();
        await feed(1_000_000 + FRAME_US, 0);
        expect(records).toHaveLength(2);

        // ここから、すべてのフレームが 300 ms 遅れて届く (受信側のジッターバッファーの遅延が一度に増えた形)。
        let media = 1_000_000 + FRAME_US;
        const encodedAt: number[] = [];
        for (let i = 0; i < 90; i++) {
            media += FRAME_US;
            const before = records.length;
            await feed(media, 300_000);
            if (records.length > before) encodedAt.push(i);
        }
        // 置き直すまで (遅れが始まってから到着で 1 秒) は捨て、置き直した後は全部をエンコードする。
        expect(reanchors).toHaveLength(1);
        expect(reanchors[0]?.shiftUs).toBe(300_000n);
        expect(Number(reanchors[0]?.staleForUs)).toBeGreaterThanOrEqual(1_000_000);
        expect(Number(reanchors[0]?.staleForUs)).toBeLessThan(1_000_000 + FRAME_US);
        const firstEncoded = encodedAt[0] ?? -1;
        expect(firstEncoded).toBe(60); // 60 枚目 (1.0 秒後) で置き直し、そのフレームからエンコードする
        // 置き直したフレームはキーフレームにする (受信側の録画は、大きな前への進みでセグメントを閉じ、次をキーフレームから始める)。
        expect(records[2]?.header.flags.keyframe).toBe(true);
        expect(records[3]?.header.flags.keyframe).toBe(false);
        expect(reanchors[0]?.framesSkipped).toBe(60);
        expect(encodedAt).toHaveLength(30);
        expect(session.stats().framesSkippedStale).toBe(60);
        expect(session.stats().clockReanchors).toBe(1);

        session.stop();
    });

    it("決め直した後のタイムスタンプは到着に沿って進み、決め直す前に送った最後のフレームより戻らない", async () => {
        const { session, records, feed } = setup();
        await feed(1_000_000, 0);
        await session.start();
        await flushMicrotasks();
        let media = 1_000_000;
        for (let i = 0; i < 3; i++) {
            media += FRAME_US;
            await feed(media, 0);
        }
        const lastBefore = records[records.length - 1]?.header.timestampUs as bigint;
        for (let i = 0; i < 80; i++) {
            media += FRAME_US;
            await feed(media, 300_000);
        }
        const after = records.slice(4).map((r) => r.header.timestampUs);
        expect(after.length).toBeGreaterThan(10);
        expect((after[0] as bigint) > lastBefore).toBe(true);
        for (let i = 1; i < after.length; i++) {
            expect((after[i] as bigint) - (after[i - 1] as bigint)).toBe(BigInt(FRAME_US));
        }
        // 置き直した後は、到着の時刻そのもの (遅れが一定なので、新しい原点との差が 0 になる)。
        expect(after[after.length - 1]).toBe(BigInt(media + 300_000));

        session.stop();
    });

    it("1 秒より短い詰まり (遅れたフレームがまとめて届き、その後は時刻どおり) では、遅れたフレームだけを捨て、定数は変えない", async () => {
        const { session, records, reanchors, feed } = setup();
        await feed(1_000_000, 0);
        await session.start();
        await flushMicrotasks();
        let media = 1_000_000;
        for (let i = 0; i < 20; i++) {
            media += FRAME_US;
            await feed(media, 300_000); // 約 330 ms の間、遅れて届く
        }
        const skipped = session.stats().framesSkippedStale;
        expect(skipped).toBe(20);
        for (let i = 0; i < 30; i++) {
            media += FRAME_US;
            await feed(media, 5_000); // 時刻どおりに届く
        }
        expect(reanchors).toHaveLength(0);
        expect(session.stats().clockReanchors).toBe(0);
        expect(session.stats().framesSkippedStale).toBe(20);
        // 原点は 0 のままなので、タイムスタンプはメディアのタイムスタンプのまま。
        expect(records[records.length - 1]?.header.timestampUs).toBe(BigInt(media));
        // 遅れたフレームが 1 秒より短く何度か続いても、そのたびに数え直す (足し合わせて置き直さない)。
        for (let round = 0; round < 4; round++) {
            for (let i = 0; i < 40; i++) {
                media += FRAME_US;
                await feed(media, 300_000);
            }
            media += FRAME_US;
            await feed(media, 5_000);
        }
        expect(reanchors).toHaveLength(0);

        session.stop();
    });
});

describe("createEncodeSession: フォールバックの順番", () => {
    it("isConfigSupported が false の段階では、configure を呼ばずに次の段階に進む", async () => {
        const { deps, configuredCfgs } = createFakeDeps({ rejectSupportAt: [0, 1] }); // Native と HalfFps が失敗する
        const { source } = createFakeSource(SOURCE_FORMAT);
        const { sink, stages } = createRecordingSink();

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await session.start();

        expect(stages).toEqual([LadderStage.Downscaled]); // 3 段階目で成功する
        expect(configuredCfgs).toHaveLength(1); // Native と HalfFps は isConfigSupported で失敗するので、configure は呼ばない

        session.stop();
    });

    it("configure() が同期で例外を投げる段階では、エンコーダを閉じて次の段階に進む", async () => {
        const { deps, configuredCfgs, closedAttempts } = createFakeDeps({ throwOnConfigureAt: [0] }); // Native が失敗する
        const { source } = createFakeSource(SOURCE_FORMAT);
        const { sink, stages } = createRecordingSink();

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await session.start();

        expect(stages).toEqual([LadderStage.HalfFps]);
        expect(configuredCfgs).toHaveLength(2); // Native (失敗) と HalfFps (成功)
        expect(closedAttempts).toContain(0); // 失敗した Native のエンコーダは閉じる

        session.stop();
    });

    it("すべての段階が失敗すると Unavailable で止まり、その後は next() を呼ばない", async () => {
        const { deps } = createFakeDeps({ rejectSupportAt: [0, 1, 2, 3] }); // Native、HalfFps、Downscaled、SoftwareFloor がすべて失敗する
        const { source, push, getCloseCallCount } = createFakeSource(SOURCE_FORMAT);
        const { sink, records, stages } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await session.start();

        expect(stages).toEqual([LadderStage.Unavailable]);

        // Unavailable になった後にフレームを入れても、読み出しが動いていないので何も送らない。
        push(makeVideoFrame(1, closedLog));
        await flushMicrotasks();
        expect(records).toHaveLength(0);
        expect(closedLog).toEqual([]); // next() を呼んでいないので、入れたフレームは受け取ってもいない

        session.stop();
        expect(getCloseCallCount()).toBeGreaterThanOrEqual(0); // stop() は呼び出してよい (source.close() は呼ぶ)
    });

    it("エンコードを始めた後に encode() が非同期のエラーになると、次の段階にフォールバックして読み出しを続ける", async () => {
        // attempt0 (Native) のウォームアップの 60 フレームの直後、最初の配信のフレーム (call index 60) でエラーにする。
        const { deps } = createFakeDeps({
            onEncode: (attemptIndex, callIndex) => (attemptIndex === 0 && callIndex === 60 ? "error" : undefined),
        });
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records, stages } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(deps, makeParams(source), sink);

        push(makeVideoFrame(1, closedLog));
        await session.start();
        await flushMicrotasks();

        expect(stages).toEqual([LadderStage.Native, LadderStage.HalfFps]);
        expect(records).toHaveLength(0); // 最初のフレームはエラーになり、出力しない

        // 新しい (HalfFps の) エンコーダで次のフレームを送ると、最初のフレームとしてキーフレームになる。
        push(makeVideoFrame(2, closedLog));
        await flushMicrotasks();
        expect(records).toHaveLength(1);
        expect(records[0]?.header.flags.keyframe).toBe(true);

        session.stop();
    });
});

describe("createEncodeSession: ウォームアップ", () => {
    it("ウォームアップが有効なら、60 フレーム分 encode() するが、出力は 1 つも送らず、黒いフレームはすべて閉じる", async () => {
        const { deps, blackFramesMade, blackFramesClosed } = createFakeDeps();
        const { source } = createFakeSource(SOURCE_FORMAT);
        const { sink, records, stages } = createRecordingSink();

        const session = createEncodeSession(deps, makeParams(source, { warmup: true }), sink);
        await session.start();

        expect(stages).toEqual([LadderStage.Native]);
        expect(blackFramesMade).toHaveLength(60);
        expect(blackFramesClosed).toHaveLength(60);
        expect(records).toHaveLength(0); // ウォームアップの出力は 1 つも送らない

        session.stop();
    });

    it("ウォームアップが無効 (warmup: false) なら、黒いフレームを 1 つも作らない", async () => {
        const { deps, blackFramesMade } = createFakeDeps();
        const { source } = createFakeSource(SOURCE_FORMAT);
        const { sink } = createRecordingSink();

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await session.start();

        expect(blackFramesMade).toHaveLength(0);

        session.stop();
    });
});

describe("createEncodeSession: キーフレームの間隔と requestKeyframe", () => {
    it("keyintSec から求めたフレーム数ごとに、キーフレームを入れる", async () => {
        // fps=60、keyintSec=0.05 から、keyintFrames = round(0.05 x 60) = 3 になる。
        const { deps } = createFakeDeps();
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(deps, makeParams(source, { warmup: false, keyintSec: 0.05 }), sink);
        await session.start();

        for (let i = 0; i < 7; i++) {
            push(makeVideoFrame(i, closedLog));
            await flushMicrotasks();
        }

        expect(records).toHaveLength(7);
        const keyFlags = records.map((r) => r.header.flags.keyframe);
        expect(keyFlags).toEqual([true, false, false, true, false, false, true]);

        session.stop();
    });

    it("requestKeyframe() は、次の 1 フレームだけをキーフレームにする", async () => {
        const { deps } = createFakeDeps();
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records } = createRecordingSink();
        const closedLog: number[] = [];

        // keyintSec を十分に長くして、一定の間隔のキーフレームが混ざらないようにする。
        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await session.start();

        push(makeVideoFrame(0, closedLog));
        await flushMicrotasks();
        expect(records[0]?.header.flags.keyframe).toBe(true); // 最初の 1 フレームは、常にキーフレーム

        push(makeVideoFrame(1, closedLog));
        await flushMicrotasks();
        expect(records[1]?.header.flags.keyframe).toBe(false);

        session.requestKeyframe();
        push(makeVideoFrame(2, closedLog));
        await flushMicrotasks();
        expect(records[2]?.header.flags.keyframe).toBe(true);

        push(makeVideoFrame(3, closedLog));
        await flushMicrotasks();
        expect(records[3]?.header.flags.keyframe).toBe(false); // 要求で変わるのは 1 回だけ

        session.stop();
    });
});

describe("createEncodeSession: fpsDivisor", () => {
    it("fpsDivisor が 2 のときは、2 フレームに 1 フレームだけを encode() し、残りは捨てて閉じる", async () => {
        const { deps } = createFakeDeps();
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(
            deps,
            makeParams(source, { warmup: false, limits: { fpsDivisor: 2 } }),
            sink,
        );
        await session.start();

        for (let i = 0; i < 4; i++) {
            push(makeVideoFrame(i, closedLog));
            await flushMicrotasks();
        }

        expect(records).toHaveLength(2); // 4 フレームのうち 2 フレームだけをエンコードする
        expect(closedLog).toEqual([0, 1, 2, 3]); // 捨てた分も閉じる
        expect(session.stats().framesDroppedByDivisor).toBe(2);
        expect(session.stats().framesEncoded).toBe(2);

        session.stop();
    });
});

describe("createEncodeSession: configure を 1 つずつ順に行う", () => {
    it("2 つの EncodeSession が同じ SerialGate を使うと、片方の試行が終わるまで、もう片方は始まらない", async () => {
        const gate = new SerialGate();
        const log: string[] = [];

        function makeLoggingDeps(label: string): EncodeSessionDeps {
            return {
                async isConfigSupported(_cfg) {
                    await Promise.resolve();
                    await Promise.resolve();
                    log.push(`${label}:isConfigSupported`);
                    return { supported: true };
                },
                createEncoder(init) {
                    return {
                        encodeQueueSize: 0,
                        configure(_cfg) {
                            log.push(`${label}:configure`);
                        },
                        encode(frame, options) {
                            init.output(
                                {
                                    type: options?.keyFrame ? "key" : "delta",
                                    timestamp: frame.timestamp,
                                    byteLength: 1,
                                    copyTo: (dest: Uint8Array) => dest.set([0]),
                                },
                                {},
                            );
                        },
                        async flush() {
                            await Promise.resolve();
                            await Promise.resolve();
                            log.push(`${label}:flush`);
                        },
                        close() {
                            // 何もしない (このテストでは close の中身を確認しない)。
                        },
                    };
                },
                gate,
                nowUs: () => 0n,
                makeBlackFrame: (_w, _h, ts) => ({ timestamp: ts, close: () => {} }),
                scaleFrame: (frame) => frame, // このテストでは縮小を使わない (maxLongEdge を指定しない)。
            };
        }

        const { source: sourceA } = createFakeSource(SOURCE_FORMAT);
        const { source: sourceB } = createFakeSource(SOURCE_FORMAT);
        const { sink: sinkA } = createRecordingSink();
        const { sink: sinkB } = createRecordingSink();

        const sessionA = createEncodeSession(makeLoggingDeps("A"), makeParams(sourceA, { streamId: 1 }), sinkA);
        const sessionB = createEncodeSession(makeLoggingDeps("B"), makeParams(sourceB, { streamId: 2 }), sinkB);

        // start() を続けて呼ぶ (await をはさまない)。gate.run() に入れた順が、そのまま実行の順になる。
        const startA = sessionA.start();
        const startB = sessionB.start();

        await startA;
        await startB;

        expect(log).toEqual(["A:isConfigSupported", "A:configure", "A:flush", "B:isConfigSupported", "B:configure", "B:flush"]);

        sessionA.stop();
        sessionB.stop();
    });
});

describe("createEncodeSession: Level は実際に configure する解像度から選ぶ (3 段階目と 4 段階目は縮小した後の解像度)", () => {
    it("1920x1080 の画面共有では、4 つの段階すべての codec が、実際に configure する解像度と effectiveFps から選んだ Level と一致し、3 段階目と 4 段階目は長い辺を 1280 に縮小した解像度になる", async () => {
        // すべての段階の isConfigSupported を true にしたまま configure() で例外を投げさせ、4 つの段階の設定を記録する
        // (throwOnConfigureAt にない最後の段階の SoftwareFloor だけが成功して、start() が終わる)。
        const { deps, configuredCfgs } = createFakeDeps({ throwOnConfigureAt: [0, 1, 2] });
        const { source } = createFakeSource(SOURCE_FORMAT); // 1920x1080@60
        const { sink, stages } = createRecordingSink();

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await session.start();

        expect(stages).toEqual([LadderStage.SoftwareFloor]); // 4 段階目で、やっと成功する
        expect(configuredCfgs).toHaveLength(4);

        // どの段階でも、実際に configure する width、height、framerate から selectH264Level で選び直した codec と
        // 一致する (target.codec の値を使っていない)。これが崩れると、指定した Level の MaxFS が実際にエンコードする
        // 面積に足りず、isConfigSupported が必ず false になる (必ず成功するはずの最後の段階がなくなる)。
        for (const cfg of configuredCfgs) {
            expect(cfg.codec).toBe(codecStringForResolution(cfg.width, cfg.height, cfg.framerate ?? 0));
        }

        // 1 段階目 (Native): 1920x1080@60、prefer-hardware。縮小しない。
        expect(configuredCfgs[0]).toMatchObject({
            codec: "avc1.64002A", // Level 4.2 (MaxFS 8704 >= 実際の面積 8160)
            width: 1920,
            height: 1080,
            framerate: 60,
            hardwareAcceleration: "prefer-hardware",
            bitrate: 12_000_000,
        });
        // 2 段階目 (HalfFps): fps だけを半分にする。解像度はそのまま。
        expect(configuredCfgs[1]).toMatchObject({
            codec: "avc1.640028", // Level 4.0 (MaxFS 8192 >= 8160)
            width: 1920,
            height: 1080,
            framerate: 30,
            hardwareAcceleration: "prefer-hardware",
            bitrate: 6_000_000,
        });
        // 3 段階目 (Downscaled): 長い辺を 1280 に縮小して、1280x720@60 にする。Level も、縮小した後の面積
        // (3600 マクロブロック) から選ぶ。
        expect(configuredCfgs[2]).toMatchObject({
            // Level 3.2 (面積の上限 5120 が 3600 を、毎秒の上限 216000 が 216000 を満たす)
            codec: "avc1.640020",
            width: 1280,
            height: 720,
            framerate: 60,
            hardwareAcceleration: "prefer-hardware",
            bitrate: 5_333_333, // 12 Mbps x (1280x720x60) / (1920x1080x60)
        });
        // 4 段階目 (SoftwareFloor): 1280x720@30、prefer-software。縮小した後の面積と fps から Level 3.1 (MaxFS 3600、
        // MaxMBPS 108000) を選び、実際の面積 3600 を満たす最後の段階になる。
        expect(configuredCfgs[3]).toMatchObject({
            codec: "avc1.64001F",
            width: 1280,
            height: 720,
            framerate: 30,
            hardwareAcceleration: "prefer-software",
            bitrate: 2_666_667,
        });

        session.stop();
    });
});

describe("createEncodeSession: maxLongEdge の縮小", () => {
    it("maxLongEdge を渡すと、縮小した後の解像度で scaleFrame を呼び、configure と onStage もその解像度になる", async () => {
        const { deps, configuredCfgs, scaleCalls, scaledFramesClosed } = createFakeDeps();
        const { source, push } = createFakeSource(SOURCE_FORMAT); // 1920x1080@60
        const { sink, records, stages, stageFormats } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(
            deps,
            makeParams(source, { warmup: false, limits: { maxLongEdge: 1000, fpsDivisor: 1 } }),
            sink,
        );

        push(makeVideoFrame(10, closedLog));
        await session.start();
        await flushMicrotasks();

        // 1920x1080 の長い辺を 1000 にすると、縦横比を保って偶数に丸め、1000x562 になる。
        expect(stages).toEqual([LadderStage.Native]); // 段階は下がっていない (maxLongEdge は段階とは別に動作する)。
        expect(configuredCfgs[0]).toMatchObject({ width: 1000, height: 562, framerate: 60 });
        expect(stageFormats[0]).toEqual({ width: 1000, height: 562, fps: 60 });
        expect(scaleCalls).toEqual([{ width: 1000, height: 562, timestamp: 10 }]);
        expect(records).toHaveLength(1);
        expect(closedLog).toEqual([10]); // 元のフレームは scaleFrame が閉じた。
        expect(scaledFramesClosed).toEqual([10]); // 縮小して作ったフレームも、エンコードした後に閉じる。

        session.stop();
    });

    it("maxLongEdge を指定しなければ、Native の段階では scaleFrame を 1 回も呼ばない", async () => {
        const { deps, configuredCfgs, scaleCalls } = createFakeDeps();
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records, stages } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);

        push(makeVideoFrame(10, closedLog));
        await session.start();
        await flushMicrotasks();
        push(makeVideoFrame(11, closedLog));
        await flushMicrotasks();
        push(makeVideoFrame(12, closedLog));
        await flushMicrotasks();

        expect(stages).toEqual([LadderStage.Native]);
        expect(records).toHaveLength(3);
        expect(scaleCalls).toEqual([]); // 縮小しない段階では、scaleFrame を呼ばない。
        expect(configuredCfgs[0]).toMatchObject({ width: 1920, height: 1080 });

        session.stop();
    });

    it("長い辺の上限は min(maxLongEdge, 段階の target の長い辺) で、フォールバックした後は target の 1280 になる", async () => {
        // maxLongEdge=1600 より、Downscaled の段階の target の長い辺 1280 のほうが小さい。
        const { deps, configuredCfgs, scaleCalls } = createFakeDeps({ rejectSupportAt: [0, 1] });
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, stages, stageFormats } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(
            deps,
            makeParams(source, { warmup: false, limits: { maxLongEdge: 1600, fpsDivisor: 1 } }),
            sink,
        );

        push(makeVideoFrame(10, closedLog));
        await session.start();
        await flushMicrotasks();

        expect(stages).toEqual([LadderStage.Downscaled]);
        expect(configuredCfgs[0]).toMatchObject({ width: 1280, height: 720 }); // 1600 ではなく 1280。
        expect(stageFormats[0]).toEqual({ width: 1280, height: 720, fps: 60 });
        expect(scaleCalls).toEqual([{ width: 1280, height: 720, timestamp: 10 }]);

        session.stop();
    });

    it("長い辺の上限は min(maxLongEdge, 段階の target の長い辺) で、Native の段階では maxLongEdge になる", async () => {
        const { deps, configuredCfgs } = createFakeDeps();
        const { source } = createFakeSource(SOURCE_FORMAT);
        const { sink } = createRecordingSink();

        const session = createEncodeSession(
            deps,
            makeParams(source, { warmup: false, limits: { maxLongEdge: 1600, fpsDivisor: 1 } }),
            sink,
        );
        await session.start();

        expect(configuredCfgs[0]).toMatchObject({ width: 1600, height: 900 });

        session.stop();
    });

    it("maxLongEdge が source の長い辺以上なら縮小しない (scaleFrame も呼ばない)", async () => {
        const { deps, configuredCfgs, scaleCalls } = createFakeDeps();
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(
            deps,
            makeParams(source, { warmup: false, limits: { maxLongEdge: 1920, fpsDivisor: 1 } }),
            sink,
        );
        push(makeVideoFrame(10, closedLog));
        await session.start();
        await flushMicrotasks();

        expect(configuredCfgs[0]).toMatchObject({ width: 1920, height: 1080 });
        expect(scaleCalls).toEqual([]);
        expect(records).toHaveLength(1);

        session.stop();
    });

    it("ウォームアップの黒いフレームも、縮小した後の解像度で作る (エンコーダに渡す解像度とそろえる)", async () => {
        const { deps, blackFramesMade } = createFakeDeps();
        const { source } = createFakeSource(SOURCE_FORMAT);
        const { sink } = createRecordingSink();
        const sizes: { width: number; height: number }[] = [];
        const depsWithSizes: EncodeSessionDeps = {
            ...deps,
            makeBlackFrame(width, height, timestampUs) {
                sizes.push({ width, height });
                return deps.makeBlackFrame(width, height, timestampUs);
            },
        };

        const session = createEncodeSession(
            depsWithSizes,
            makeParams(source, { warmup: true, limits: { maxLongEdge: 1000, fpsDivisor: 1 } }),
            sink,
        );
        await session.start();

        expect(blackFramesMade.length).toBeGreaterThan(0);
        for (const size of sizes) expect(size).toEqual({ width: 1000, height: 562 });

        session.stop();
    });

    it("maxLongEdge が 0 以下なら RangeError で止まる (意味のない解像度で進まない)", () => {
        const { deps } = createFakeDeps();
        const { source } = createFakeSource(SOURCE_FORMAT);
        const { sink } = createRecordingSink();

        expect(() =>
            createEncodeSession(deps, makeParams(source, { limits: { maxLongEdge: 0, fpsDivisor: 1 } }), sink),
        ).toThrow(RangeError);
    });

    it("scaleFrame が失敗したら、フレームごとに繰り返さずに Unavailable を伝える", async () => {
        const { deps, scaleCalls } = createFakeDeps({ throwOnScale: true });
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records, stages, stageFormats } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(
            deps,
            makeParams(source, { warmup: false, limits: { maxLongEdge: 1000, fpsDivisor: 1 } }),
            sink,
        );

        push(makeVideoFrame(10, closedLog));
        await session.start();
        await flushMicrotasks();

        expect(scaleCalls).toHaveLength(1); // 1 回試して、繰り返さない。
        expect(closedLog).toEqual([10]); // 失敗したときも、元のフレームは残らない (scaleFrame との約束)。
        expect(records).toHaveLength(0);
        expect(stages[stages.length - 1]).toBe(LadderStage.Unavailable);
        expect(stageFormats[stageFormats.length - 1]).toBeUndefined(); // 映像を送れないので、解像度も伝えない。

        session.stop();
    });
});

describe("createEncodeSession: ウォームアップの最中にエンコーダが失敗しても、止まらずに次の段階にフォールバックする", () => {
    it("最初の段階のウォームアップで flush() が reject しても、start() は例外を投げずに次の段階で成功する", async () => {
        const { deps, closedAttempts } = createFakeDeps({ rejectFlushAt: [0] }); // Native のウォームアップが失敗する
        const { source } = createFakeSource(SOURCE_FORMAT);
        const { sink, stages } = createRecordingSink();

        const session = createEncodeSession(deps, makeParams(source, { warmup: true }), sink);
        await expect(session.start()).resolves.toBeUndefined(); // reject しない

        expect(stages).toEqual([LadderStage.HalfFps]); // Native はウォームアップで失敗し、HalfFps で成功する
        expect(closedAttempts).toContain(0); // 失敗した Native のエンコーダは閉じる

        session.stop();
    });

    it("動いている間のフォールバックでも、フォールバック先のウォームアップが失敗したら、さらに次の段階に進む", async () => {
        // attempt0 (Native) はウォームアップ (60 フレーム) の後に成功させ、最初の配信のフレーム (ウォームアップの後の
        // call index 60) を非同期のエラーにして、フォールバックを起こす。フォールバック先の attempt1 (HalfFps) は
        // ウォームアップの flush が失敗し、attempt2 (Downscaled) で、やっと成功する。
        const { deps } = createFakeDeps({
            rejectFlushAt: [1],
            onEncode: (attemptIndex, callIndex) => (attemptIndex === 0 && callIndex === 60 ? "error" : undefined),
        });
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, stages } = createRecordingSink();

        const session = createEncodeSession(deps, makeParams(source, { warmup: true }), sink);
        const closedLog: number[] = [];
        push(makeVideoFrame(1, closedLog));
        await session.start();
        await flushMicrotasks();

        // HalfFps (attempt1) はウォームアップで失敗して次の段階に進むので、onStage には出ない
        // (isConfigSupported が false の段階と同じ扱い)。
        expect(stages).toEqual([LadderStage.Native, LadderStage.Downscaled]);

        session.stop();
    });

    it("エンコーダを作るところ (createEncoder) が同期で例外を投げても、次の段階に進む", async () => {
        let attemptIndex = -1;
        const deps: EncodeSessionDeps = {
            async isConfigSupported() {
                attemptIndex++;
                return { supported: true };
            },
            createEncoder(init) {
                if (attemptIndex === 0) {
                    throw new Error("fake createEncoder failure");
                }
                return {
                    encodeQueueSize: 0,
                    configure() {},
                    encode(frame, options) {
                        init.output(
                            {
                                type: options?.keyFrame ? "key" : "delta",
                                timestamp: frame.timestamp,
                                byteLength: 1,
                                copyTo: (dest: Uint8Array) => dest.set([0]),
                            },
                            {},
                        );
                    },
                    async flush() {},
                    close() {},
                };
            },
            gate: new SerialGate(),
            nowUs: () => 0n,
            makeBlackFrame: (_w, _h, ts) => ({ timestamp: ts, close: () => {} }),
            scaleFrame: (frame) => frame, // このテストでは縮小を使わない (maxLongEdge を指定しない)。
        };
        const { source } = createFakeSource(SOURCE_FORMAT);
        const { sink, stages } = createRecordingSink();

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await expect(session.start()).resolves.toBeUndefined();

        expect(stages).toEqual([LadderStage.HalfFps]);

        session.stop();
    });

    it("読み出しの最中に source.next() が reject しても、Unavailable を伝えて stats().stage を更新する (想定外の失敗への備え)", async () => {
        const { deps } = createFakeDeps();
        const { sink, stages } = createRecordingSink();
        const source: VideoFrameSource = {
            format: SOURCE_FORMAT,
            next: () => Promise.reject(new Error("boom")),
            close: () => {},
        };

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await session.start(); // Native の段階の確保は成功する
        await flushMicrotasks();

        expect(stages).toEqual([LadderStage.Native, LadderStage.Unavailable]);
        expect(session.stats().stage).toBe(LadderStage.Unavailable);

        session.stop();
    });
});

describe("createEncodeSession: stop", () => {
    it("stop() は読み出しを止め、エンコーダと source を閉じ、その後の next() の結果は無視する", async () => {
        const { deps, closedAttempts } = createFakeDeps();
        const { source, push, getCloseCallCount } = createFakeSource(SOURCE_FORMAT);
        const { sink, records } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        push(makeVideoFrame(1, closedLog));
        await session.start();
        await flushMicrotasks();
        expect(records).toHaveLength(1);

        session.stop();
        expect(getCloseCallCount()).toBe(1);
        expect(closedAttempts).toContain(0);

        push(makeVideoFrame(2, closedLog));
        await flushMicrotasks();
        expect(records).toHaveLength(1); // stop() の後は増えない

        session.stop(); // 2 回呼び出してもよい
    });
});

// ---- 黒埋め用のフレームを作る 3 つのタイミング ------------------------------

/** 1 回分の黒埋め用のフレームの数 (session.ts は cfg.framerate の数だけ作る)。SOURCE_FORMAT は 60 fps。 */
const MATERIAL_FRAMES = 60;
/** ウォームアップのフレームの数 (encode/warmup.ts の既定の値)。 */
const WARMUP_FRAMES = 60;
/** 黒埋め用のフレームの間隔 (60 fps 分。session.ts の frameIntervalUs と同じ丸め方)。 */
const MATERIAL_FRAME_INTERVAL_US = BigInt(Math.round(1_000_000 / MATERIAL_FRAMES));

describe("createEncodeSession: 録画が OFF の間は何も増えない", () => {
    it("録画を一度も ON にしなければ、黒いフレームはウォームアップの 60 枚だけ、エンコーダは 1 つだけ、黒埋め用のフレームは 0", async () => {
        const { deps, blackFramesMade, createdEncoders } = createFakeDeps();
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records, materials } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(deps, makeParams(source, { warmup: true }), sink);
        await session.start();
        push(makeVideoFrame(10, closedLog));
        await flushMicrotasks();

        expect(blackFramesMade).toHaveLength(WARMUP_FRAMES);
        expect(createdEncoders).toHaveLength(1);
        expect(materialFrames(records)).toHaveLength(0);
        expect(materials).toHaveLength(0);
        expect(session.stats().recordingMaterialFramesSent).toBe(0);

        session.stop();
        await session.finished();
        // encoder-teardown の黒埋め用のフレームも作らない (録画が OFF)。stop() の中で同期で閉じている。
        expect(blackFramesMade).toHaveLength(WARMUP_FRAMES);
        expect(createdEncoders).toHaveLength(1);
    });

    it("setRecording(false) を何度呼び出しても、黒埋め用のフレームは作らない", async () => {
        const { deps, blackFramesMade, createdEncoders } = createFakeDeps();
        const { source } = createFakeSource(SOURCE_FORMAT);
        const { sink, records } = createRecordingSink();

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        session.setRecording(false);
        await session.start();
        session.setRecording(false);
        await flushMicrotasks();

        expect(blackFramesMade).toHaveLength(0);
        expect(createdEncoders).toHaveLength(1);
        expect(materialFrames(records)).toHaveLength(0);

        session.stop();
        await session.finished();
    });
});

describe("createEncodeSession: encoder-created (エンコーダを作った直後)", () => {
    it("ウォームアップの後、最初の配信のフレームより前に、IDR 1 枚と P フレームを、録画だけに使うフラグを付けて送る", async () => {
        const { deps, blackFramesMade } = createFakeDeps();
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records, materials } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(deps, makeParams(source, { warmup: true }), sink);
        session.setRecording(true); // start() より前に伝えておく (index.ts と同じつなぎ方)。
        await session.start();

        // このときには黒埋め用のフレームはすべて出ていて、配信のフレームはまだ 1 枚もない。
        expect(materialFrames(records)).toHaveLength(MATERIAL_FRAMES);
        expect(realFrames(records)).toHaveLength(0);
        expect(blackFramesMade).toHaveLength(WARMUP_FRAMES + MATERIAL_FRAMES);
        expect(materials).toEqual([{ origin: "encoder-created", frameCount: MATERIAL_FRAMES }]);

        const material = materialFrames(records);
        expect(material[0]?.header.flags.keyframe).toBe(true); // 先頭だけが IDR
        expect(material.slice(1).every((r) => !r.header.flags.keyframe)).toBe(true);
        expect(material.every((r) => r.header.type === FrameType.Video)).toBe(true);
        expect(material.every((r) => r.header.streamId === 7)).toBe(true);

        push(makeVideoFrame(10, closedLog));
        await flushMicrotasks();
        expect(realFrames(records)).toHaveLength(1);
        expect(session.stats().recordingMaterialFramesSent).toBe(MATERIAL_FRAMES);
        expect(session.stats().framesEncoded).toBe(1); // 黒埋め用のフレームは、配信のフレームに数えない

        session.stop();
        await session.finished();
    });

    it("黒埋め用のフレームのタイムスタンプは、その黒埋めの最初のシステム時刻から、フレームの間隔ずつ進む", async () => {
        // フレームごとにそのときのシステム時刻を入れると、分解能が 1 ms しかない値が、60 フレーム分ほとんど同じ値で並ぶ
        // (黒埋め用のフレームは、同期の繰り返しと flush で一度に出る)。受信側は最初のフレームを原点にして、差をそのまま
        // 黒の位置にするので、1 秒分の黒が数十 ms になる。テスト用のシステム時刻は呼ばれるたびに 1 進むので、差が
        // ちょうどフレームの間隔であれば、原点を 1 回しか読んでいないことが分かる。
        const { deps } = createFakeDeps();
        const { source } = createFakeSource(SOURCE_FORMAT);
        const { sink, records } = createRecordingSink();

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        session.setRecording(true);
        await session.start();

        const stamps = materialFrames(records).map((r) => r.header.timestampUs);
        expect(stamps).toHaveLength(MATERIAL_FRAMES);

        const steps: bigint[] = [];
        for (let i = 1; i < stamps.length; i++) {
            const prev = stamps[i - 1];
            const current = stamps[i];
            if (prev === undefined || current === undefined) continue;
            steps.push(current - prev);
        }
        expect(steps).toHaveLength(MATERIAL_FRAMES - 1);
        expect(steps.every((step) => step === MATERIAL_FRAME_INTERVAL_US)).toBe(true);
        // 先頭から最後までで 1 秒分になる (最後の 1 フレーム分の長さは、受信側が足す)。
        const span = steps.reduce((total, step) => total + step, 0n);
        expect(span).toBe(BigInt(MATERIAL_FRAMES - 1) * MATERIAL_FRAME_INTERVAL_US);
        expect(span).toBeGreaterThan(900_000n);

        session.stop();
        await session.finished();
    });

    it("ウォームアップをしない設定でも、encoder-created の黒埋め用のフレームを作る", async () => {
        const { deps, blackFramesMade } = createFakeDeps();
        const { source } = createFakeSource(SOURCE_FORMAT);
        const { sink, records } = createRecordingSink();

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        session.setRecording(true);
        await session.start();

        expect(blackFramesMade).toHaveLength(MATERIAL_FRAMES);
        expect(materialFrames(records)).toHaveLength(MATERIAL_FRAMES);

        session.stop();
        await session.finished();
    });

    it("フォールバックして新しいエンコーダになれば、その新しいインスタンスの黒埋め用のフレームを、もう 1 回作る", async () => {
        // 1 段階目の 2 枚目の配信のフレームで非同期の error を起こし、次の段階にフォールバックさせる。1 段階目の encode の
        // 呼び出しは、0 から 59 が黒埋め用のフレーム (encoder-created)、60 が最初の配信のフレーム (キーフレーム)、61 が
        // 2 枚目の配信のフレームである。
        const { deps, blackFramesMade } = createFakeDeps({
            onEncode: (attempt, call) => (attempt === 0 && call === MATERIAL_FRAMES + 1 ? "error" : undefined),
        });
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records, stages, materials } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        session.setRecording(true);
        await session.start();
        expect(materials).toHaveLength(1);

        push(makeVideoFrame(10, closedLog)); // 最初の配信のフレームはキーフレーム (成功する)
        await flushMicrotasks();
        push(makeVideoFrame(11, closedLog)); // 2 枚目は error になり、フォールバックする
        await flushMicrotasks();

        expect(stages.length).toBeGreaterThanOrEqual(2);
        // 2 段階目 (HalfFps) は 30 fps なので、黒埋め用のフレームも、その段階の 1 秒分の 30 フレームになる
        // (黒埋め用のフレームの数は cfg.framerate。受信側は黒を並べて欠落を埋めるので、段階ごとの fps に合った枚数で
        // よい)。
        expect(materials).toEqual([
            { origin: "encoder-created", frameCount: MATERIAL_FRAMES },
            { origin: "encoder-created", frameCount: MATERIAL_FRAMES / 2 },
        ]);
        expect(blackFramesMade).toHaveLength(MATERIAL_FRAMES + MATERIAL_FRAMES / 2);
        expect(materialFrames(records)).toHaveLength(MATERIAL_FRAMES + MATERIAL_FRAMES / 2);

        session.stop();
        await session.finished();
    });
});

describe("createEncodeSession: recording-turned-on (録画を ON にしたとき)", () => {
    it("動いているインスタンスとは別のエンコーダを作り、configure のすべてのフィールドが一致する", async () => {
        const { deps, configuredCfgs, createdEncoders, closedAttempts } = createFakeDeps();
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records, materials } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await session.start();
        push(makeVideoFrame(10, closedLog));
        await flushMicrotasks();
        expect(createdEncoders).toHaveLength(1);

        session.setRecording(true);
        await flushMicrotasks();

        expect(createdEncoders).toHaveLength(2); // 別のインスタンス
        expect(configuredCfgs).toHaveLength(2);
        expect(configuredCfgs[1]).toEqual(configuredCfgs[0]); // すべてのフィールドが一致する
        expect(configuredCfgs[1]).toBe(configuredCfgs[0]); // コピーしておいた設定を、そのまま渡している
        expect(materials).toEqual([{ origin: "recording-turned-on", frameCount: MATERIAL_FRAMES }]);
        expect(materialFrames(records)).toHaveLength(MATERIAL_FRAMES);
        // 一時的なエンコーダは、使い終えたら閉じる (残しておかない)。
        expect(closedAttempts).toHaveLength(1);

        // 配信の映像は止まっていない。
        push(makeVideoFrame(11, closedLog));
        await flushMicrotasks();
        expect(realFrames(records)).toHaveLength(2);

        session.stop();
        await session.finished();
    });

    it("isConfigSupported を呼び直さない (エンコーダに空きがあるかを返さないので、呼ぶ意味がない)", async () => {
        const { deps, getAttemptCount } = createFakeDeps();
        const { source } = createFakeSource(SOURCE_FORMAT);
        const { sink } = createRecordingSink();

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await session.start();
        expect(getAttemptCount()).toBe(1);

        session.setRecording(true);
        await flushMicrotasks();
        expect(getAttemptCount()).toBe(1);

        session.stop();
        await session.finished();
    });

    it("同じ値でもう一度呼び出しても、何も起きない (変わっていなければ何もしない)", async () => {
        const { deps, createdEncoders } = createFakeDeps();
        const { source } = createFakeSource(SOURCE_FORMAT);
        const { sink, materials } = createRecordingSink();

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await session.start();
        session.setRecording(true);
        session.setRecording(true);
        await flushMicrotasks();

        expect(createdEncoders).toHaveLength(2);
        expect(materials).toHaveLength(1);

        session.stop();
        await session.finished();
    });

    it("黒埋め用のフレームを作った後に OFF から ON にしても、作り直さない (インスタンスごとに 1 回)", async () => {
        const { deps, createdEncoders } = createFakeDeps();
        const { source } = createFakeSource(SOURCE_FORMAT);
        const { sink, materials } = createRecordingSink();

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await session.start();
        session.setRecording(true);
        await flushMicrotasks();
        session.setRecording(false);
        session.setRecording(true);
        await flushMicrotasks();

        expect(createdEncoders).toHaveLength(2); // 配信のエンコーダと、recording-turned-on の 1 つだけ
        expect(materials).toHaveLength(1);

        session.stop();
        await session.finished();
    });

    it("encoder-created で黒埋め用のフレームを作ってあれば、recording-turned-on では作らない (先に来たほうだけ)", async () => {
        const { deps, createdEncoders } = createFakeDeps();
        const { source } = createFakeSource(SOURCE_FORMAT);
        const { sink, materials } = createRecordingSink();

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        session.setRecording(true);
        await session.start();
        expect(materials).toEqual([{ origin: "encoder-created", frameCount: MATERIAL_FRAMES }]);

        session.setRecording(false);
        session.setRecording(true);
        await flushMicrotasks();

        expect(createdEncoders).toHaveLength(1);
        expect(materials).toHaveLength(1);

        session.stop();
        await session.finished();
    });
});

/**
 * 受信側の録画 (src-obs/record/recorder.cpp の handle_material と handle_video) と同じ読み方で、最初の黒埋め用の
 * キーフレームから数えて、黒埋めに使われるフレームの数を返す。受信側は、同じストリームの配信のフレームが届いた
 * ときに黒埋めの範囲を閉じ、閉じた後の黒埋め用の P フレームは捨てる。
 */
function materialRunAsReceiverSees(records: readonly SinkRecord[]): number {
    const start = records.findIndex((r) => r.header.flags.recordingOnly && r.header.flags.keyframe);
    if (start < 0) return 0;
    let run = 0;
    for (let i = start; i < records.length; i++) {
        const r = records[i];
        if (r === undefined || !r.header.flags.recordingOnly) break;
        run++;
    }
    return run;
}

describe("createEncodeSession: 1 回分の黒埋め用のフレームを続けて渡す", () => {
    it("recording-turned-on: 黒埋め用のフレームの出力の間に配信のフレームが届いても、cfg.framerate 枚が途切れずに sink に届く", async () => {
        const { deps, heldEncoders, configuredCfgs } = createFakeDeps({ holdOutputs: true });
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records, materials } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await session.start();
        push(makeVideoFrame(10, closedLog));
        await flushMicrotasks();
        heldEncoders[0]?.release(1); // 配信のエンコーダの、1 枚目のフレーム
        expect(realFrames(records)).toHaveLength(1);

        session.setRecording(true);
        await flushMicrotasks();
        expect(heldEncoders).toHaveLength(2); // recording-turned-on の一時的なエンコーダ
        const spare = heldEncoders[1];
        const framerate = configuredCfgs[1]?.framerate ?? 0;
        expect(spare?.pending()).toBe(framerate);

        // 黒埋め用のフレームが出始めた後に、配信のフレームが割り込む (実際には 60 fps で出続けている)。
        spare?.release(1);
        push(makeVideoFrame(11, closedLog));
        await flushMicrotasks();
        heldEncoders[0]?.release(1);
        spare?.release(framerate - 1);
        await flushMicrotasks();
        await session.finished();

        expect(realFrames(records)).toHaveLength(2);
        expect(materialFrames(records)).toHaveLength(framerate);
        expect(materials).toEqual([{ origin: "recording-turned-on", frameCount: framerate }]);
        // 受信側の読み方で、黒埋めの範囲が 1 秒分すべて残る。
        expect(materialRunAsReceiverSees(records)).toBe(framerate);

        session.stop();
        await session.finished();
    });

    it("encoder-teardown: 黒埋め用のフレームの出力が 1 枚ずつ遅れて出ても、cfg.framerate 枚が途切れずに sink に届く", async () => {
        const { deps, heldEncoders, configuredCfgs, closedAttempts } = createFakeDeps({ holdOutputs: true });
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records, materials } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await session.start();
        push(makeVideoFrame(10, closedLog));
        await flushMicrotasks();

        // recording-turned-on の処理が始まる前に止めるので、encoder-teardown で、動いているエンコーダを使って
        // 黒埋め用のフレームを作る。
        session.setRecording(true);
        session.stop();
        const framerate = configuredCfgs[0]?.framerate ?? 0;
        const live = heldEncoders[0];
        // 止める前に渡してあった配信のフレーム 1 枚と、黒埋め用のフレーム。
        expect(live?.pending()).toBe(1 + framerate);
        const done = session.finished();
        for (let i = 0; i < 1 + framerate; i++) {
            live?.release(1);
            await flushMicrotasks(2);
        }
        await done;

        expect(materialFrames(records)).toHaveLength(framerate);
        expect(materials).toEqual([{ origin: "encoder-teardown", frameCount: framerate }]);
        expect(materialRunAsReceiverSees(records)).toBe(framerate);
        expect(closedAttempts).toEqual([0]); // 閉じたのは、黒埋め用のフレームを渡し終えた後
    });

    it("黒埋め用のフレームの途中で flush() が失敗したときは、ためたフレームを 1 枚も渡さない", async () => {
        const { deps } = createFakeDeps({ rejectFlushAt: [0] });
        const { source } = createFakeSource(SOURCE_FORMAT);
        const { sink, records, materials } = createRecordingSink();

        // ウォームアップなし、録画 ON で始めると、最初の flush() は encoder-created の黒埋め用のフレームのものになる。
        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        session.setRecording(true);
        await session.start();

        expect(materialFrames(records)).toHaveLength(0);
        expect(materials).toEqual([]);
        expect(session.stats().recordingMaterialFailures).toBe(1);

        session.stop();
        await session.finished();
    });
});

describe("createEncodeSession: encoder-teardown (エンコーダを閉じるとき)", () => {
    it("黒埋め用のフレームがすべて出るまで閉じない (stop() から返ったときには、まだ閉じていない)", async () => {
        const { deps, closedAttempts } = createFakeDeps({ outputDelayCalls: 3 });
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await session.start();
        push(makeVideoFrame(10, closedLog));
        await flushMicrotasks();

        // recording-turned-on の処理をゲートに入れただけで、まだ始めずに止める (入れた処理は stopped を見てやめ、
        // encoder-teardown が代わりに作る)。
        session.setRecording(true);
        session.stop();
        expect(closedAttempts).toEqual([]); // 黒埋め用のフレームの flush を待っている最中
        // 出力が 3 回分遅れているので、このときには、まだすべては出ていない。
        expect(materialFrames(records).length).toBeLessThan(MATERIAL_FRAMES);

        await session.finished();
        expect(closedAttempts).toEqual([0]);
        expect(materialFrames(records)).toHaveLength(MATERIAL_FRAMES);
    });

    it("録画が ON のまま閉じると、close の前に黒埋め用のフレームがすべて出る (出力が遅れるエンコーダでも)", async () => {
        const { deps, closedAttempts, blackFramesMade } = createFakeDeps({ outputDelayCalls: 5 });
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records, materials } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await session.start();
        push(makeVideoFrame(10, closedLog));
        await flushMicrotasks();

        // recording-turned-on を作らずに encoder-teardown だけを起こすため、ゲートに入れた recording-turned-on の処理が
        // 始まる前に stop() する。
        session.setRecording(true);
        session.stop();
        await session.finished();

        expect(materials).toEqual([{ origin: "encoder-teardown", frameCount: MATERIAL_FRAMES }]);
        expect(blackFramesMade).toHaveLength(MATERIAL_FRAMES);
        expect(materialFrames(records)).toHaveLength(MATERIAL_FRAMES);
        expect(closedAttempts).toEqual([0]); // 閉じたのは、黒埋め用のフレームを出し切った後の 1 回だけ
    });

    it("encoder-teardown の黒埋め用のフレームも、その黒埋めの最初の送信側の時計から、フレームの間隔ずつ進む", async () => {
        // encoder-teardown は、動いているエンコーダ (配信のフレームも出している出力のコールバック) をそのまま使うので、
        // 原点を置き直す先を間違えると、フレームごとの送信側の時計に戻る。
        const { deps } = createFakeDeps();
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await session.start();
        push(makeVideoFrame(10, closedLog));
        await flushMicrotasks();

        session.setRecording(true);
        session.stop(); // recording-turned-on の処理が始まる前に止めるので、encoder-teardown が代わりに作る。
        await session.finished();

        const stamps = materialFrames(records).map((r) => r.header.timestampUs);
        expect(stamps).toHaveLength(MATERIAL_FRAMES);
        const steps: bigint[] = [];
        for (let i = 1; i < stamps.length; i++) {
            const prev = stamps[i - 1];
            const current = stamps[i];
            if (prev === undefined || current === undefined) continue;
            steps.push(current - prev);
        }
        expect(steps).toHaveLength(MATERIAL_FRAMES - 1);
        expect(steps.every((step) => step === MATERIAL_FRAME_INTERVAL_US)).toBe(true);
    });

    it("recording-turned-on で作ってあれば、encoder-teardown では作らず、stop() の中で閉じる", async () => {
        const { deps, closedAttempts } = createFakeDeps();
        const { source } = createFakeSource(SOURCE_FORMAT);
        const { sink, materials } = createRecordingSink();

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await session.start();
        session.setRecording(true);
        await flushMicrotasks();
        expect(materials).toHaveLength(1);

        session.stop();
        expect(closedAttempts).toContain(0); // 同期で閉じている
        await session.finished();
        expect(materials).toHaveLength(1);
    });

    it("finished() は、stop() より前に呼び出しても解決する (reject しない)", async () => {
        const { deps } = createFakeDeps();
        const { source } = createFakeSource(SOURCE_FORMAT);
        const { sink } = createRecordingSink();

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await session.start();
        await expect(session.finished()).resolves.toBeUndefined();
        session.stop();
        await expect(session.finished()).resolves.toBeUndefined();
    });

    it("黒埋め用のフレームの flush が失敗しても、finished() は解決し、エンコーダは閉じる", async () => {
        const { deps, closedAttempts } = createFakeDeps({ rejectFlushAt: [0], outputDelayCalls: 1 });
        const { source } = createFakeSource(SOURCE_FORMAT);
        const { sink, materials } = createRecordingSink();

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await session.start();
        session.setRecording(true);
        session.stop();
        await expect(session.finished()).resolves.toBeUndefined();

        expect(materials).toHaveLength(0); // 出し切れなかったので、作ったタイミングは記録しない
        expect(session.stats().recordingMaterialFailures).toBe(1);
        expect(closedAttempts).toContain(0);
    });
});

describe("createEncodeSession: hasPendingWork (呼び出し元が、待つ必要があるかを見分ける)", () => {
    it("録画が OFF のときは、stop() の後に残る処理がない (呼び出し元は、すぐにソケットを閉じてよい)", async () => {
        const { deps } = createFakeDeps();
        const { source } = createFakeSource(SOURCE_FORMAT);
        const { sink } = createRecordingSink();

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await session.start();
        expect(session.hasPendingWork()).toBe(false);

        session.stop();
        expect(session.hasPendingWork()).toBe(false);
        await session.finished();
    });

    it("録画が ON のときは、stop() の後に残る処理があり、finished() でなくなる", async () => {
        const { deps } = createFakeDeps({ outputDelayCalls: 5 });
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await session.start();
        push(makeVideoFrame(10, closedLog));
        await flushMicrotasks();
        expect(session.hasPendingWork()).toBe(false);

        session.setRecording(true);
        session.stop();
        expect(session.hasPendingWork()).toBe(true);

        await session.finished();
        expect(session.hasPendingWork()).toBe(false);
    });

    it("段階を確保している最中は true (その中で encoder-created の黒埋め用のフレームを作ることがある)", async () => {
        const { deps } = createFakeDeps();
        const { source } = createFakeSource(SOURCE_FORMAT);
        const { sink } = createRecordingSink();

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        const starting = session.start();
        expect(session.hasPendingWork()).toBe(true);

        await starting;
        expect(session.hasPendingWork()).toBe(false);

        session.stop();
        await session.finished();
    });
});

describe("createEncodeSession: 出力の種類は、タイムスタンプの範囲で分ける", () => {
    it("出力が N 回分遅れても、ウォームアップの出力を配信のフレームとして送らない", async () => {
        // 真偽値のフラグで分けていると、ウォームアップの最後の出力が flush の後 (warming=false) に出て、配信のフレーム
        // として送られてしまう。タイムスタンプの範囲で分ければ、この場合も 0 枚のまま。
        const { deps } = createFakeDeps({ outputDelayCalls: 4 });
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(deps, makeParams(source, { warmup: true }), sink);
        await session.start();
        expect(records).toHaveLength(0); // ウォームアップの 60 フレームは 1 枚も出ない

        push(makeVideoFrame(10, closedLog));
        await flushMicrotasks();
        // 遅れているので、このときにはまだ出ていなくてよい。出たものがあれば、配信のフレームである。
        expect(realFrames(records).every((r) => r.header.timestampUs > 0n)).toBe(true);
        expect(materialFrames(records)).toHaveLength(0);

        session.stop();
        await session.finished();
    });

    it("出力が N 回分遅れても、黒埋め用のフレームを配信のフレームのフラグで送らない", async () => {
        const { deps } = createFakeDeps({ outputDelayCalls: 4 });
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(deps, makeParams(source, { warmup: true }), sink);
        session.setRecording(true);
        await session.start();

        // encoder-created の黒埋め用のフレームは flush で出し切られ、すべてが録画だけに使うフラグ付きで出ている。
        expect(materialFrames(records)).toHaveLength(MATERIAL_FRAMES);
        expect(realFrames(records)).toHaveLength(0);

        push(makeVideoFrame(10, closedLog));
        push(makeVideoFrame(11, closedLog));
        push(makeVideoFrame(12, closedLog));
        push(makeVideoFrame(13, closedLog));
        push(makeVideoFrame(14, closedLog));
        await flushMicrotasks();
        // 遅れて出てきた配信のフレームには、録画だけに使うフラグは付かない。
        expect(materialFrames(records)).toHaveLength(MATERIAL_FRAMES);
        expect(realFrames(records).length).toBeGreaterThan(0);

        session.stop();
        await session.finished();
    });
});

describe("createEncodeSession: SPS と PPS の付け足し", () => {
    /** 開始コード付きの NAL を並べた、テスト用のエンコーダの出力を作る。 */
    function auBytes(...nals: number[][]): Uint8Array {
        const out: number[] = [];
        for (const nal of nals) out.push(0, 0, 0, 1, ...nal);
        return new Uint8Array(out);
    }

    const SPS = [0x67, 0x64, 0x00, 0x2a];
    const PPS = [0x68, 0xee, 0x3c];
    const IDR = [0x65, 0x88, 0x84];
    const SLICE = [0x41, 0x9a, 0x21];

    /**
     * キーフレームの出力に SPS と PPS を入れるかどうかを指定できる、テスト用のエンコーダ。`deferUntilFlush` が true なら、
     * 出力は `encode()` では出さず、`flush()` で出し切る (VideoEncoder と同じ形で、黒埋め用のフレームが `stop()` から
     * 返った後に出る場合を作る)。
     */
    function createDepsWithPayloads(keyframePayloads: Uint8Array[], deferUntilFlush = false) {
        const base = createFakeDeps();
        let keyIndex = 0;
        const deps: EncodeSessionDeps = {
            ...base.deps,
            createEncoder(init) {
                /** 出力を待っているチャンク (エンコーダのインスタンスごと)。 */
                const pendingOutputs: EncodedVideoChunk[] = [];
                return {
                    encodeQueueSize: 0,
                    configure: () => undefined,
                    encode(frame, options) {
                        const keyFrame = options?.keyFrame ?? false;
                        const payload = keyFrame
                            ? (keyframePayloads[Math.min(keyIndex++, keyframePayloads.length - 1)] ??
                              auBytes(SPS, PPS, IDR))
                            : auBytes(SLICE);
                        const chunk: EncodedVideoChunk = {
                            type: keyFrame ? "key" : "delta",
                            timestamp: frame.timestamp,
                            byteLength: payload.byteLength,
                            copyTo: (dest: Uint8Array) => dest.set(payload),
                        };
                        if (deferUntilFlush) {
                            pendingOutputs.push(chunk);
                            return;
                        }
                        init.output(chunk, {});
                    },
                    async flush() {
                        // VideoEncoder の flush() は同期では終わらない (たまっている出力は後から出る)。
                        await Promise.resolve();
                        while (pendingOutputs.length > 0) {
                            const ready = pendingOutputs.shift();
                            if (ready !== undefined) init.output(ready, {});
                        }
                    },
                    close: () => undefined,
                };
            },
        };
        return deps;
    }

    it("キーフレームに SPS と PPS がなければ、最後に見たものを前に付けて送り、回数を数える", async () => {
        const deps = createDepsWithPayloads([auBytes(SPS, PPS, IDR), auBytes(IDR)]);
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(
            deps,
            makeParams(source, { warmup: false, keyintSec: 100 }),
            sink,
        );
        await session.start();

        push(makeVideoFrame(10, closedLog)); // 1 枚目は必ずキーフレーム (SPS と PPS がある)
        await flushMicrotasks();
        expect(session.stats().spsPpsPrependedCount).toBe(0);

        session.requestKeyframe();
        push(makeVideoFrame(11, closedLog)); // 2 枚目のキーフレームには SPS と PPS がない
        await flushMicrotasks();

        expect(session.stats().spsPpsPrependedCount).toBe(1);
        expect(session.spsPpsPrependedCount()).toBe(1);
        const second = records[1]?.payload;
        expect([...(second ?? [])]).toEqual([0, 0, 0, 1, ...SPS, 0, 0, 0, 1, ...PPS, ...auBytes(IDR)]);

        session.stop();
        await session.finished();
    });

    it("最初のキーフレームに SPS と PPS がなければ、前に付けずに数える (覚えた値がない)", async () => {
        const deps = createDepsWithPayloads([auBytes(IDR)]);
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await session.start();
        push(makeVideoFrame(10, closedLog));
        await flushMicrotasks();

        expect(session.stats().keyframesWithoutParameterSets).toBe(1);
        expect(session.stats().spsPpsPrependedCount).toBe(0);
        expect([...(records[0]?.payload ?? [])]).toEqual([...auBytes(IDR)]); // そのまま通す

        session.stop();
        await session.finished();
    });

    it("SPS と PPS が入っているキーフレームはそのまま送る (付け足した回数は増えない)", async () => {
        const deps = createDepsWithPayloads([auBytes(SPS, PPS, IDR)]);
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await session.start();
        push(makeVideoFrame(10, closedLog));
        await flushMicrotasks();

        expect(session.stats().spsPpsPrependedCount).toBe(0);
        expect(session.stats().keyframesWithoutParameterSets).toBe(0);
        expect([...(records[0]?.payload ?? [])]).toEqual([...auBytes(SPS, PPS, IDR)]);

        session.stop();
        await session.finished();
    });

    it("encoder-teardown の黒埋め用のフレームで付け足した分は、止めた後の最終の値にだけ入る", async () => {
        // 止めたセッションの付け足した回数を stop() のときに読むと、encoder-teardown の黒埋め用のフレーム (flush() が
        // 解決した後に出る) で付け足した分が、一度も facts に入らない。出力を flush() までためるテスト用のエンコーダで、
        // その分が finalSpsPpsPrependedCount() にだけ入ることを確認する。
        const deps = createDepsWithPayloads([auBytes(SPS, PPS, IDR), auBytes(IDR)], true);
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(
            deps,
            makeParams(source, { warmup: false, keyintSec: 100 }),
            sink,
        );
        await session.start();
        push(makeVideoFrame(10, closedLog)); // 配信のフレームのキーフレーム (SPS と PPS があり、これを覚える)
        await flushMicrotasks();

        session.setRecording(true);
        session.stop(); // recording-turned-on の処理が始まる前に止めるので、encoder-teardown が代わりに作る。
        // stop() は同期で返り、黒埋め用のフレームはまだ 1 つも出ていない (このときに読むと、その分を数えない)。
        expect(session.spsPpsPrependedCount()).toBe(0);
        expect(materialFrames(records)).toHaveLength(0);

        // 最終の値は、encoder-teardown の黒埋め用の IDR (SPS と PPS がない) で付け足した 1 回分を含む。
        expect(await session.finalSpsPpsPrependedCount()).toBe(1);
        expect(session.spsPpsPrependedCount()).toBe(1); // 最終の値は、そのときの現在の値とも一致する
        expect(materialFrames(records)).toHaveLength(MATERIAL_FRAMES);
        // 黒埋め用の最初のフレームには、覚えた SPS と PPS が前に付いている。
        const firstMaterial = materialFrames(records)[0];
        expect([...(firstMaterial?.payload ?? [])]).toEqual([
            0, 0, 0, 1, ...SPS, 0, 0, 0, 1, ...PPS, ...auBytes(IDR),
        ]);
    });

    it("最終の値を返す関数は、stop() より前に呼び出しても解決する (reject しない)", async () => {
        const deps = createDepsWithPayloads([auBytes(SPS, PPS, IDR)]);
        const { source } = createFakeSource(SOURCE_FORMAT);
        const { sink } = createRecordingSink();

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await session.start();
        await expect(session.finalSpsPpsPrependedCount()).resolves.toBe(0);

        session.stop();
        await expect(session.finalSpsPpsPrependedCount()).resolves.toBe(0);
    });
});

// ---- 動いている間の上限の変更 (setLimits) -----------------------------------

/** 入れた処理を、テストから 1 つずつ実行するゲート (確保し直しと recording-turned-on の順番をテストで決めるため)。 */
function createManualGate() {
    const queue: Array<() => void> = [];
    const gate = {
        run<T>(task: () => Promise<T>): Promise<T> {
            return new Promise<T>((resolve, reject) => {
                queue.push(() => {
                    task().then(resolve, reject);
                });
            });
        },
    } as unknown as SerialGate;
    return {
        gate,
        pending: (): number => queue.length,
        releaseNext(): void {
            queue.shift()?.();
        },
    };
}

describe("createEncodeSession: setLimits", () => {
    it("下げた上限はすぐに適用され、次のフレームの境目で同じ段階から確保し直して、その後は新しい解像度でエンコードする", async () => {
        const { deps, configuredCfgs, closedAttempts, createdEncoders, scaleCalls } = createFakeDeps();
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records, stages, stageFormats } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await session.start();
        push(makeVideoFrame(10, closedLog));
        await flushMicrotasks();
        expect(configuredCfgs).toHaveLength(1);

        session.setLimits({ maxLongEdge: 1280, fpsDivisor: 1 });
        push(makeVideoFrame(11, closedLog)); // 読み出しを待っていたときのフレームは、古いエンコーダで出る
        await flushMicrotasks();
        push(makeVideoFrame(12, closedLog));
        await flushMicrotasks();

        expect(createdEncoders).toHaveLength(2);
        expect(configuredCfgs[1]).toMatchObject({ width: 1280, height: 720, framerate: 60 });
        expect(stages).toEqual([LadderStage.Native, LadderStage.Native]); // フォールバックではなく、同じ段階から
        expect(stageFormats[1]).toEqual({ width: 1280, height: 720, fps: 60 });
        expect(closedAttempts).toEqual([0]); // 古いエンコーダは閉じた
        expect(scaleCalls).toEqual([{ width: 1280, height: 720, timestamp: 12 }]);
        const real = realFrames(records);
        expect(real).toHaveLength(3);
        expect(real[2]?.header.flags.keyframe).toBe(true); // 新しいエンコーダの最初のフレームは、キーフレーム

        session.stop();
    });

    it("fps を下げた (1/1 -> 1/2) ときも確保し直し、新しいエンコーダの上限で間引く", async () => {
        const { deps, configuredCfgs } = createFakeDeps();
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await session.start();
        session.setLimits({ fpsDivisor: 2 });
        for (let t = 10; t < 16; t++) {
            push(makeVideoFrame(t, closedLog));
            await flushMicrotasks();
        }

        expect(configuredCfgs).toHaveLength(2);
        expect(configuredCfgs[1]).toMatchObject({ width: 1920, height: 1080, framerate: 30 });
        // 10 は古いエンコーダ (1/1) で出る。11 から 15 は新しいエンコーダで、その最初のフレーム (11) から数えて 1 つおきに
        // 出る (11、13、15。最初のフレームはキーフレーム)。
        const real = realFrames(records);
        expect(real).toHaveLength(1 + 3);
        expect(real[1]?.header.flags.keyframe).toBe(true);
        expect(session.stats().framesDroppedByDivisor).toBe(2);

        session.stop();
    });

    it("上げた上限はこのセッションには適用されない (次の画面共有から反映され、確保し直さない)", async () => {
        const { deps, configuredCfgs, createdEncoders } = createFakeDeps();
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(
            deps,
            makeParams(source, { warmup: false, limits: { maxLongEdge: 1280, fpsDivisor: 2 } }),
            sink,
        );
        await session.start();
        session.setLimits({ fpsDivisor: 1 }); // 長い辺も fps も上げる向き
        for (let t = 10; t < 14; t++) {
            push(makeVideoFrame(t, closedLog));
            await flushMicrotasks();
        }

        expect(createdEncoders).toHaveLength(1);
        expect(configuredCfgs).toHaveLength(1);
        expect(configuredCfgs[0]).toMatchObject({ width: 1280, height: 720, framerate: 30 });
        expect(realFrames(records)).toHaveLength(2); // 1/2 に間引いたまま
        expect(session.stats().framesDroppedByDivisor).toBe(2);

        session.stop();
    });

    it("同じ上限をもう一度渡しても、何もしない", async () => {
        const { deps, createdEncoders, getAttemptCount } = createFakeDeps();
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(
            deps,
            makeParams(source, { warmup: false, limits: { maxLongEdge: 960, fpsDivisor: 1 } }),
            sink,
        );
        await session.start();
        session.setLimits({ maxLongEdge: 960, fpsDivisor: 1 });
        session.setLimits({ maxLongEdge: 1280, fpsDivisor: 1 }); // 上げる向きなので動作せず、同じ値のまま
        push(makeVideoFrame(10, closedLog));
        await flushMicrotasks();
        push(makeVideoFrame(11, closedLog));
        await flushMicrotasks();

        expect(createdEncoders).toHaveLength(1);
        expect(getAttemptCount()).toBe(1);

        session.stop();
    });

    it("下げたキーと上げたキーが混ざったメッセージでは、下げたキーだけが動作する (長い辺を下げ、fps を戻す)", async () => {
        const { deps, configuredCfgs } = createFakeDeps();
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, stageFormats } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(deps, makeParams(source, { warmup: false, limits: { fpsDivisor: 2 } }), sink);
        await session.start();
        session.setLimits({ maxLongEdge: 1280, fpsDivisor: 1 });
        push(makeVideoFrame(10, closedLog));
        await flushMicrotasks();
        push(makeVideoFrame(11, closedLog));
        await flushMicrotasks();

        expect(configuredCfgs).toHaveLength(2);
        expect(configuredCfgs[1]).toMatchObject({ width: 1280, height: 720, framerate: 30 }); // fps は 1/2 のまま
        expect(stageFormats[1]).toEqual({ width: 1280, height: 720, fps: 30 });

        session.stop();
    });

    it("start() より前に下げれば、最初の確保でその上限を使う (確保し直さない)", async () => {
        const { deps, configuredCfgs } = createFakeDeps();
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        session.setLimits({ maxLongEdge: 640, fpsDivisor: 1 });
        await session.start();
        push(makeVideoFrame(10, closedLog));
        await flushMicrotasks();

        expect(configuredCfgs).toHaveLength(1);
        expect(configuredCfgs[0]).toMatchObject({ width: 640, height: 360 });

        session.stop();
    });

    it("最初の確保の途中で下げたら、確保し終えた後の回でやり直す (古い上限のエンコーダのまま動かない)", async () => {
        const { deps, configuredCfgs } = createFakeDeps();
        let release!: () => void;
        const held = new Promise<void>((resolve) => {
            release = resolve;
        });
        let first = true;
        const slowDeps: EncodeSessionDeps = {
            ...deps,
            async isConfigSupported(cfg) {
                if (first) {
                    first = false;
                    await held; // 最初の試行はここで止めておく (上限は既にコピーしてある)
                }
                return deps.isConfigSupported(cfg);
            },
        };
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(slowDeps, makeParams(source, { warmup: false }), sink);
        const starting = session.start();
        await flushMicrotasks();
        session.setLimits({ maxLongEdge: 1280, fpsDivisor: 1 });
        release();
        await starting;
        await flushMicrotasks();
        push(makeVideoFrame(10, closedLog));
        await flushMicrotasks();

        expect(configuredCfgs).toHaveLength(2);
        expect(configuredCfgs[0]).toMatchObject({ width: 1920, height: 1080 });
        expect(configuredCfgs[1]).toMatchObject({ width: 1280, height: 720 });

        session.stop();
    });

    it("stop() の後は何もせず、形が正しくなければ RangeError を投げる", async () => {
        const { deps, createdEncoders } = createFakeDeps();
        const { source } = createFakeSource(SOURCE_FORMAT);
        const { sink } = createRecordingSink();

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await session.start();
        expect(() => session.setLimits({ maxLongEdge: 0, fpsDivisor: 1 })).toThrow(RangeError);
        expect(() => session.setLimits({ fpsDivisor: 3 as unknown as 1 })).toThrow(RangeError);
        session.stop();
        session.setLimits({ maxLongEdge: 640, fpsDivisor: 2 });
        await flushMicrotasks();
        expect(createdEncoders).toHaveLength(1);
    });

    it("確保し直した後の recording-turned-on の黒埋め用のフレームは、新しい設定で作る", async () => {
        const { deps, configuredCfgs, createdEncoders } = createFakeDeps();
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, records, materials } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        await session.start();
        session.setLimits({ maxLongEdge: 1280, fpsDivisor: 1 });
        push(makeVideoFrame(10, closedLog));
        await flushMicrotasks();
        push(makeVideoFrame(11, closedLog));
        await flushMicrotasks();
        expect(createdEncoders).toHaveLength(2);

        session.setRecording(true);
        await flushMicrotasks(200);

        expect(createdEncoders).toHaveLength(3); // recording-turned-on の一時的なエンコーダ
        expect(configuredCfgs).toHaveLength(3);
        expect(configuredCfgs[2]).toBe(configuredCfgs[1]); // 今動いている (新しい) エンコーダの設定そのもの
        expect(configuredCfgs[2]).toMatchObject({ width: 1280, height: 720, framerate: 60 });
        expect(configuredCfgs[2]).not.toEqual(configuredCfgs[0]);
        expect(materials).toEqual([{ origin: "recording-turned-on", frameCount: MATERIAL_FRAMES }]);
        expect(materialFrames(records)).toHaveLength(MATERIAL_FRAMES);

        session.stop();
        await session.finished();
    });

    it("録画が ON のときに確保し直すと、古いエンコーダを閉じてから新しいほうを作り、新しいほうは encoder-created の黒埋め用のフレームを作る", async () => {
        const { deps, configuredCfgs, closedAttempts, createdEncoders } = createFakeDeps();
        const closedWhenCreated: number[][] = [];
        const create = deps.createEncoder.bind(deps);
        deps.createEncoder = (init) => {
            closedWhenCreated.push([...closedAttempts]);
            return create(init);
        };
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, materials } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        session.setRecording(true);
        await session.start();
        expect(materials).toEqual([{ origin: "encoder-created", frameCount: MATERIAL_FRAMES }]);

        session.setLimits({ maxLongEdge: 1280, fpsDivisor: 1 });
        push(makeVideoFrame(10, closedLog));
        await flushMicrotasks(200);

        expect(createdEncoders).toHaveLength(2);
        expect(closedWhenCreated[1]).toEqual([0]); // 新しいエンコーダを作ったときには、古いほうは閉じていた
        expect(configuredCfgs[1]).toMatchObject({ width: 1280, height: 720 });
        // 古いほうは encoder-created で作ってあるので、閉じるときには作らない (インスタンスごとに 1 回)。
        expect(materials).toEqual([
            { origin: "encoder-created", frameCount: MATERIAL_FRAMES },
            { origin: "encoder-created", frameCount: MATERIAL_FRAMES },
        ]);

        session.stop();
        await session.finished();
    });

    it("黒埋め用のフレームをまだ作っていないエンコーダを確保し直すときは、encoder-teardown の黒埋め用のフレームを出し切ってから閉じる (予約した recording-turned-on はやめる)", async () => {
        const manual = createManualGate();
        const { deps, configuredCfgs, createdEncoders } = createFakeDeps({}, manual.gate);
        const { source, push } = createFakeSource(SOURCE_FORMAT);
        const { sink, materials } = createRecordingSink();
        const closedLog: number[] = [];

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        const starting = session.start();
        manual.releaseNext(); // 最初の確保
        await starting;

        session.setLimits({ maxLongEdge: 1280, fpsDivisor: 1 });
        push(makeVideoFrame(10, closedLog));
        await flushMicrotasks();
        expect(manual.pending()).toBe(1); // 確保し直す処理が入っていて、待っている

        session.setRecording(true); // recording-turned-on を予約する (確保し直す処理の後ろに入る)
        expect(manual.pending()).toBe(2);

        manual.releaseNext(); // 確保し直す: 古いほうは黒埋め用のフレームがないので、encoder-teardown で作ってから閉じる
        await flushMicrotasks(300);
        manual.releaseNext(); // recording-turned-on: 対象の古いエンコーダはもう使っていないので、やめる
        await flushMicrotasks(100);

        expect(materials).toEqual([
            { origin: "encoder-teardown", frameCount: MATERIAL_FRAMES },
            { origin: "encoder-created", frameCount: MATERIAL_FRAMES },
        ]);
        expect(createdEncoders).toHaveLength(2); // recording-turned-on の一時的なエンコーダは作らない
        expect(configuredCfgs[1]).toMatchObject({ width: 1280, height: 720 });

        session.stop();
        await session.finished();
    });

    it("encoder-created の判定と差し替えの間に録画が ON になっても、新しいエンコーダのために recording-turned-on を入れ直す", async () => {
        let hook: (() => void) | undefined;
        const inner = new SerialGate();
        const hookGate = {
            async run<T>(task: () => Promise<T>): Promise<T> {
                const result = await inner.run(task);
                const once = hook;
                hook = undefined;
                once?.();
                return result;
            },
        } as unknown as SerialGate;
        const { deps, createdEncoders } = createFakeDeps({}, hookGate);
        const { source } = createFakeSource(SOURCE_FORMAT);
        const { sink, materials } = createRecordingSink();

        const session = createEncodeSession(deps, makeParams(source, { warmup: false }), sink);
        // 最初の確保が返った直後 (encoder-created の判定は済んでいて、まだ使い始めていない) に ON にする。
        hook = () => session.setRecording(true);
        await session.start();
        await flushMicrotasks(200);

        expect(materials).toEqual([{ origin: "recording-turned-on", frameCount: MATERIAL_FRAMES }]);
        expect(createdEncoders).toHaveLength(2);

        session.stop();
        await session.finished();
    });
});
