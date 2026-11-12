// SPDX-License-Identifier: Apache-2.0

// encode/clock_map.ts のテスト。メディアのタイムスタンプを「トラックごとの定数」で送信側の時計に換算する。定数は最初の
// 到着の「到着の時刻 - メディアのタイムスタンプ」から始め、2 秒の期間の中の小さいほうから 3 番目が今の定数より小さくなったときに
// だけ下げる。上げるのは、差が 200 ms を超えた状態が 1 秒続いて、メディアの時計が替わったとみなしたときだけである。最後に、音の時計 (D)、送信側の時計 (Q)、
// システム時刻 (S) を別々の速さにした 10 時間の模擬で、音と映像のタイムスタンプの関係が動かないことを確かめる。

import { afterEach, describe, expect, it, vi } from "vitest";

import { createAudioChunker, pushAudio } from "../audio/chunk";
import {
    CLOCK_MAP_JUMP_CONFIRM_US,
    CLOCK_MAP_JUMP_US,
    CLOCK_MAP_RANK,
    CLOCK_MAP_WINDOW_US,
    createClockMap,
    mapMediaTime,
    type ClockMapState,
} from "../encode/clock_map";
import { senderClockUs } from "../encode/sender_clock";

/** メディアのタイムスタンプの原点 (ブラウザーが決める値で、送信側の時計とは関係のない位置にある)。 */
const MEDIA_EPOCH_US = 5_000_000;
/** 送信側の時計の原点 (エポックに近い値に見立てた値)。 */
const SENDER_EPOCH_US = 1_700_000_000_000_000n;
/** 60 fps の 1 フレーム分。 */
const FRAME_US = 16_666;

/** 届いた時刻とメディアのタイムスタンプの組を順に流し、換算した時刻の列と、最後の状態を返す。 */
function runSequence(pairs: { mediaUs: number; arrivalUs: bigint }[]): { mapped: bigint[]; state: ClockMapState } {
    let state = createClockMap();
    const mapped: bigint[] = [];
    for (const pair of pairs) {
        const result = mapMediaTime(state, pair.mediaUs, pair.arrivalUs);
        state = result.state;
        mapped.push(result.timeUs);
    }
    return { mapped, state };
}

/** i 番目のフレームの組。lagUs は取り出しの遅れ。 */
function frame(i: number, lagUs: bigint): { mediaUs: number; arrivalUs: bigint } {
    return { mediaUs: MEDIA_EPOCH_US + i * FRAME_US, arrivalUs: SENDER_EPOCH_US + BigInt(i * FRAME_US) + lagUs };
}

describe("mapMediaTime: 一定の遅延", () => {
    it("届くのが常に同じだけ遅れているなら、換算した時刻はメディアのタイムスタンプの間隔をそのまま保つ", () => {
        const { mapped, state } = runSequence([0, 1, 2, 3, 4].map((i) => frame(i, 3_000n)));
        expect(state.offsetUs).toBe(SENDER_EPOCH_US - BigInt(MEDIA_EPOCH_US) + 3_000n);
        for (let i = 1; i < mapped.length; i++) {
            expect((mapped[i] as bigint) - (mapped[i - 1] as bigint)).toBe(BigInt(FRAME_US));
        }
        expect(mapped[0]).toBe(SENDER_EPOCH_US + 3_000n);
    });
});

describe("mapMediaTime: 定数の決め方", () => {
    it("起動の直後に詰まって最初の到着だけが遅れても、その遅れは定数に残らない", () => {
        const lags = [150_000n, 120_000n, 2_000n, 3_000n, 2_500n, 4_000n, 2_000n, 3_000n];
        const { state } = runSequence(lags.map((lag, i) => frame(i, lag)));
        // 期間の中の、小さいほうから 3 番目の差 (2_500)。
        expect(CLOCK_MAP_RANK).toBe(3);
        expect(state.offsetUs).toBe(SENDER_EPOCH_US - BigInt(MEDIA_EPOCH_US) + 2_500n);
    });

    it("最初の到着だけが 250 ms 遅れても、詰まりのない到着が 3 つそろった時点で定数が下がり、決め直しにならない", () => {
        const lags = [250_000n, 2_000n, 2_000n, 2_000n, 3_000n];
        let state = createClockMap();
        const offsets: (bigint | undefined)[] = [];
        lags.forEach((lag, i) => {
            const result = mapMediaTime(state, frame(i, lag).mediaUs, frame(i, lag).arrivalUs);
            state = result.state;
            offsets.push(state.offsetUs);
            expect(result.restart).toBeUndefined();
        });
        const base = SENDER_EPOCH_US - BigInt(MEDIA_EPOCH_US);
        // 2 つ目と 3 つ目では、今の定数より小さい差がまだ 3 つそろっていないので下げない。
        expect(offsets[1]).toBe(base + 250_000n);
        expect(offsets[2]).toBe(base + 250_000n);
        expect(offsets[3]).toBe(base + 2_000n);
        expect(state.offsetUs).toBe(base + 2_000n);
        expect(state.jumpStartUs).toBeUndefined();
        expect(state.restarts).toBe(0);
    });

    it("同じ期間の中の早すぎる外れ値が 1 つか 2 つなら、定数は外れ値まで下がらない", () => {
        const lags = [3_000n, 3_000n, -60_000n, 3_000n, 3_000n, -55_000n, 3_000n, 3_000n];
        const { state } = runSequence(lags.map((lag, i) => frame(i, lag)));
        expect(state.offsetUs).toBe(SENDER_EPOCH_US - BigInt(MEDIA_EPOCH_US) + 3_000n);
    });

    it("最初の 2 秒が 15 ms 詰まっていても、後の期間に詰まりのない到着が 3 つ来れば、定数は正しい値まで下がる", () => {
        const base = SENDER_EPOCH_US - BigInt(MEDIA_EPOCH_US);
        let state = createClockMap();
        let i = 0;
        // 最初の 2 秒より長い間、どの到着も 15 ms 詰まっている (2 ms の床に 13 ms が足された形)。
        for (; BigInt(i * FRAME_US) < CLOCK_MAP_WINDOW_US + 500_000n; i++) {
            state = mapMediaTime(state, frame(i, 15_000n).mediaUs, frame(i, 15_000n).arrivalUs).state;
        }
        expect(state.offsetUs).toBe(base + 15_000n);
        // 詰まりのない到着が 2 つでは下げない。
        for (let k = 0; k < 2; k++, i++) {
            state = mapMediaTime(state, frame(i, 2_000n).mediaUs, frame(i, 2_000n).arrivalUs).state;
        }
        expect(state.offsetUs).toBe(base + 15_000n);
        // 3 つ目で、正しい値 (2 ms の床) まで下がる。
        const result = mapMediaTime(state, frame(i, 2_000n).mediaUs, frame(i, 2_000n).arrivalUs);
        expect(result.state.offsetUs).toBe(base + 2_000n);
        expect(result.restart).toBeUndefined();
    });

    it("期間が替わると数え直すので、別々の期間に 1 つずつ来た早すぎる外れ値では下がらない", () => {
        const base = SENDER_EPOCH_US - BigInt(MEDIA_EPOCH_US);
        let state = createClockMap();
        let i = 0;
        // 10 秒の間、2 秒に 1 つずつ -50 ms の外れ値が混ざる。同じ期間の中では 1 つだけである。
        for (; i < 600; i++) {
            const lag = i % 120 === 60 ? -50_000n : 5_000n;
            state = mapMediaTime(state, frame(i, lag).mediaUs, frame(i, lag).arrivalUs).state;
        }
        expect(state.offsetUs).toBe(base + 5_000n);
    });

    it("外れ値 1 つでは、早くても遅くても定数を変えず、換算した時刻はメディアのタイムスタンプの間隔のまま進む", () => {
        const pairs: { mediaUs: number; arrivalUs: bigint }[] = [];
        let i = 0;
        for (; BigInt(i * FRAME_US) <= CLOCK_MAP_WINDOW_US; i++) pairs.push(frame(i, 5_000n));
        const offset = SENDER_EPOCH_US - BigInt(MEDIA_EPOCH_US) + 5_000n;
        // 早い外れ値 (-50 ms) と遅い詰まり (+150 ms)。どちらもしきい値 (200 ms) の内。
        pairs.push(frame(i++, -50_000n));
        pairs.push(frame(i++, 150_000n));
        for (let k = 0; k < 600; k++) pairs.push(frame(i++, 5_000n));
        const { mapped, state } = runSequence(pairs);
        expect(state.offsetUs).toBe(offset);
        for (let k = pairs.length - 600; k < pairs.length; k++) {
            expect((mapped[k] as bigint) - (mapped[k - 1] as bigint)).toBe(BigInt(FRAME_US));
        }
    });
});

describe("mapMediaTime: 不連続", () => {
    it("差が 200 ms を超えた状態が 1 秒続いたら、その差で定数を決め直し、量を返す", () => {
        let state = createClockMap();
        let i = 0;
        for (; BigInt(i * FRAME_US) <= CLOCK_MAP_WINDOW_US; i++) {
            state = mapMediaTime(state, frame(i, 2_000n).mediaUs, frame(i, 2_000n).arrivalUs).state;
        }
        const before = state.offsetUs as bigint;
        // メディアのタイムスタンプが 300 ms 戻った (到着は同じように進む)。
        let restartAt = -1;
        let shift: bigint | undefined;
        const startArrival = frame(i, 302_000n).arrivalUs;
        for (let k = 0; k < 200 && restartAt < 0; k++, i++) {
            const f = frame(i, 302_000n);
            const result = mapMediaTime(state, f.mediaUs, f.arrivalUs);
            state = result.state;
            if (result.restart !== undefined) {
                restartAt = k;
                shift = result.restart.shiftUs;
                expect(result.restart.jumpForUs).toBeGreaterThanOrEqual(CLOCK_MAP_JUMP_CONFIRM_US);
                expect(f.arrivalUs - startArrival).toBeGreaterThanOrEqual(CLOCK_MAP_JUMP_CONFIRM_US);
                // 決め直したフレームは、届いた時刻に換算される。
                expect(result.timeUs).toBe(f.arrivalUs);
            } else {
                // 決め直すまでは、前の定数で換算する。
                expect(result.timeUs).toBe(BigInt(f.mediaUs) + before);
            }
        }
        expect(shift).toBe(300_000n);
        expect(state.restarts).toBe(1);
        expect(CLOCK_MAP_JUMP_US).toBe(200_000n);
    });

    it("メディアのタイムスタンプが先へ飛んだ (到着より 250 ms 先) ときは、到着が 3 つそろった時点で定数を下げ、決め直しにはしない", () => {
        let state = createClockMap();
        let i = 0;
        for (; i < 200; i++) state = mapMediaTime(state, frame(i, 2_000n).mediaUs, frame(i, 2_000n).arrivalUs).state;
        const before = state.offsetUs as bigint;
        const offsets: bigint[] = [];
        for (let k = 0; k < 10; k++, i++) {
            const f = frame(i, -248_000n);
            const result = mapMediaTime(state, f.mediaUs, f.arrivalUs);
            state = result.state;
            expect(result.restart).toBeUndefined();
            offsets.push(state.offsetUs as bigint);
        }
        expect(offsets[1]).toBe(before);
        expect(offsets[2]).toBe(before - 250_000n);
        expect(state.jumpStartUs).toBeUndefined();
        expect(state.restarts).toBe(0);
    });

    it("1 秒より短い詰まりでは決め直さない", () => {
        let state = createClockMap();
        let i = 0;
        for (; i < 200; i++) state = mapMediaTime(state, frame(i, 2_000n).mediaUs, frame(i, 2_000n).arrivalUs).state;
        const before = state.offsetUs;
        for (let k = 0; k < 50; k++, i++) {
            const result = mapMediaTime(state, frame(i, 400_000n).mediaUs, frame(i, 400_000n).arrivalUs);
            state = result.state;
            expect(result.restart).toBeUndefined();
        }
        state = mapMediaTime(state, frame(i, 2_000n).mediaUs, frame(i, 2_000n).arrivalUs).state;
        expect(state.offsetUs).toBe(before);
        expect(state.jumpStartUs).toBeUndefined();
        expect(state.restarts).toBe(0);
    });

    it("メディアのタイムスタンプが到着の 4 倍の速さで進む間は、定数が下がり続け、換算した時刻は到着とほぼ同じ速さで進み、決め直さない", () => {
        // テスト用の送信側 (test/tools/harness/sender.ts) の overflow の形。10 ms のチャンクを 2.5 ms ごとに作る。
        let state = createClockMap();
        let maxAheadUs = 0n;
        let first: bigint | undefined;
        let last = 0n;
        const chunks = 8_000; // 到着で 20 秒
        for (let i = 0; i < chunks; i++) {
            const arrivalUs = SENDER_EPOCH_US + BigInt(i) * 2_500n;
            const result = mapMediaTime(state, MEDIA_EPOCH_US + i * 10_000, arrivalUs);
            state = result.state;
            expect(result.restart).toBeUndefined();
            const aheadUs = result.timeUs - arrivalUs;
            if (aheadUs > maxAheadUs) maxAheadUs = aheadUs;
            first ??= result.timeUs;
            last = result.timeUs;
        }
        expect(state.restarts).toBe(0);
        // 換算した時刻は到着より先に出る。先に出る量は、期間の始めの 2 つ (3 つそろうまでは下げない) で最も大きく、
        // 4 チャンク分 (4 x 7.5 ms) である。
        expect(maxAheadUs).toBe(30_000n);
        // 20 秒の到着の間に、換算した時刻も到着とほぼ同じだけ (メディアのタイムスタンプの 80 秒ではなく) 進む。
        const spanUs = last - (first as bigint);
        const arrivalSpanUs = BigInt(chunks - 1) * 2_500n;
        expect(spanUs > arrivalSpanUs - 30_000n && spanUs < arrivalSpanUs + 30_000n).toBe(true);
    });
});

describe("mapMediaTime: 不正な入力", () => {
    it("有限の数でないメディアのタイムスタンプには、例外を投げる", () => {
        expect(() => mapMediaTime(createClockMap(), Number.NaN, SENDER_EPOCH_US)).toThrow(RangeError);
        expect(() => mapMediaTime(createClockMap(), Number.POSITIVE_INFINITY, SENDER_EPOCH_US)).toThrow(RangeError);
    });

    it("小数のメディアのタイムスタンプは、整数に丸めてから使う (BigInt() が例外を投げないように)", () => {
        const result = mapMediaTime(createClockMap(), 1_000.6, SENDER_EPOCH_US);
        expect(result.timeUs).toBe(SENDER_EPOCH_US);
        expect(result.state.offsetUs).toBe(SENDER_EPOCH_US - 1_001n);
    });
});

/* ==========================================================================
   10 時間の模擬
   ========================================================================== */

// xorshift32 (同じ種からは同じ列を返す)。
function rng(seed: number): () => number {
    let x = seed >>> 0 || 1;
    return () => {
        x ^= x << 13;
        x >>>= 0;
        x ^= x >>> 17;
        x ^= x << 5;
        x >>>= 0;
        return x / 4294967296;
    };
}

interface SimResult {
    /**
     * 音のタイムスタンプ - 音の本当の再生の時刻 - (映像のタイムスタンプ - 映像の本当の描画の時刻) の、60 秒の時点の値と
     * 最後の値と、60 秒の後の隣り合う値の差の最大 (us)。
     */
    startUs: number;
    endUs: number;
    maxJumpUs: number;
    restarts: number;
}

/**
 * 送信側の 10 時間の模擬。Q は送信側の時計と同じ単調な時計で、AudioData.timestamp (音を再生のために取り出した時刻) と
 * VideoFrame.timestamp (描画の時刻からの差) はどちらも Q の値である。音は D の速さで 10 ms ずつ取り出され
 * (D は Q より dPpm 速い)、到着の時刻は senderClockUs (performance) から読む。システム時刻 (Date.now) は sPpm で進み、
 * 5 時間目に 1 秒飛ぶ。製品のコードが Date.now を読めば、その飛びが結果に入る。
 */
function simulate(dPpm: number, sPpm: number, hours: number, seed: number): SimResult {
    const rand = rng(seed);
    const q0Us = 3_600_000_000; // 模擬を始める Q (us)
    let qNowUs = q0Us;
    const perf = { timeOrigin: 1_699_999_000_000, now: () => qNowUs / 1000 - 3_000_000 };
    vi.spyOn(Date, "now").mockImplementation(() => {
        const elapsedUs = qNowUs - q0Us;
        const jumpUs = elapsedUs >= 5 * 3_600_000_000 ? 1_000_000 : 0;
        return Math.floor((1_700_000_000_000_000 + elapsedUs * (1 + sPpm * 1e-6) + jumpUs) / 1000);
    });
    const audioOriginUs = 7_777_000; // AudioData.timestamp の原点 (Q からの差)
    const videoOriginUs = -2_000_000; // VideoFrame.timestamp の原点 (最初のフレームの描画の時刻)
    let audio = createClockMap();
    let video = createClockMap();
    const d = dPpm * 1e-6;
    const chunkQUs = 10_000 / (1 + d);
    let nextFrame = 0;
    let videoErrUs = 0;
    const result: SimResult = { startUs: Number.NaN, endUs: 0, maxJumpUs: 0, restarts: 0 };
    let prev: number | undefined;
    const chunks = Math.floor((hours * 3_600_000_000) / chunkQUs);
    for (let i = 0; i < chunks; i++) {
        const playQUs = q0Us + i * chunkQUs;
        // この音より前に描画される映像を換算する。
        for (;;) {
            const renderQUs = q0Us + 20_000 + nextFrame * (1_000_000 / 60);
            if (renderQUs > playQUs) break;
            nextFrame++;
            // 届くまでの遅れは 1 から 6 ms。100 フレームに 1 つ、80 ms 詰まる。
            const lag = 1_000 + rand() * 5_000 + (rand() < 0.01 ? 80_000 : 0);
            qNowUs = renderQUs + lag;
            const media = Math.round(renderQUs - q0Us + videoOriginUs + (rand() - 0.5) * 400);
            const mapped = mapMediaTime(video, media, senderClockUs(perf));
            video = mapped.state;
            videoErrUs = Number(mapped.timeUs) - (renderQUs + 1_699_999_000_000_000 - 3_000_000_000);
        }
        // 音の取り出しの時刻の揺れは +-1 ms。届くまでの遅れは 1 から 4 ms で、200 個に 1 つ、50 ms 詰まる。
        const tsQUs = playQUs + (rand() - 0.5) * 2_000;
        qNowUs = playQUs + 1_000 + rand() * 3_000 + (rand() < 0.005 ? 50_000 : 0);
        const mapped = mapMediaTime(audio, Math.round(tsQUs + audioOriginUs), senderClockUs(perf));
        audio = mapped.state;
        const audioErrUs = Number(mapped.timeUs) - (playQUs + 1_699_999_000_000_000 - 3_000_000_000);
        const av = audioErrUs - videoErrUs;
        // 始めの間は、起動の直後の詰まりが消えるまで定数が下がる向きに動く。測るのは 60 秒の後である。
        if (i * chunkQUs >= 60_000_000 && Number.isNaN(result.startUs)) result.startUs = av;
        if (prev !== undefined && !Number.isNaN(result.startUs)) {
            result.maxJumpUs = Math.max(result.maxJumpUs, Math.abs(av - prev));
        }
        prev = av;
        result.endUs = av;
    }
    result.restarts = audio.restarts + video.restarts;
    return result;
}

/**
 * pushAudio を通す送信側の模擬。音は D の速さで 1000 サンプルずつの断片 (チャンクの 480 サンプルの倍数にせず、端数の
 * 持ち越しを通す。10 時間分を短い時間で流すため、実物の 480 サンプルより長くした) で取り出され、各断片の AudioData.timestamp は、その先頭のサンプルを取り出した Q の時刻 (+-1 ms の揺れ) で
 * ある。到着は senderClockUs (製品の index.ts の nowUs と同じ関数) に、Q で進む performance を渡して読む。チャンクごとに、
 * チャンクの timestampUs - そのチャンクの先頭のサンプルの本当の取り出しの時刻 を測り、同じ時点の映像の換算の誤差を
 * 引いて、音と映像のタイムスタンプの関係とする。
 */
function simulateChunks(dPpm: number, hours: number, seed: number): SimResult {
    const rand = rng(seed);
    const q0Us = 3_600_000_000;
    let qNowUs = q0Us;
    const perf = { timeOrigin: 1_699_999_000_000, now: () => qNowUs / 1000 - 3_000_000 };
    const senderOfQ = 1_699_999_000_000_000 - 3_000_000_000; // 送信側の時計 - Q (us)
    const fragmentFrames = 1000;
    const samples = new Float32Array(fragmentFrames * 2);
    const d = dPpm * 1e-6;
    const sampleQUs = 1_000_000 / (48_000 * (1 + d)); // 1 サンプルを取り出す Q の間隔
    let chunker = createAudioChunker();
    let video = createClockMap();
    let nextFrame = 0;
    let videoErrUs = 0;
    let chunkIndex = 0;
    const result: SimResult = { startUs: Number.NaN, endUs: 0, maxJumpUs: 0, restarts: 0 };
    let prev: number | undefined;
    const fragments = Math.floor((hours * 3_600_000_000) / (fragmentFrames * sampleQUs));
    for (let f = 0; f < fragments; f++) {
        const playQUs = q0Us + f * fragmentFrames * sampleQUs;
        for (;;) {
            const renderQUs = q0Us + 20_000 + nextFrame * (1_000_000 / 60);
            if (renderQUs > playQUs) break;
            nextFrame++;
            qNowUs = renderQUs + 1_000 + rand() * 5_000;
            const mapped = mapMediaTime(video, Math.round(renderQUs - q0Us - 2_000_000), senderClockUs(perf));
            video = mapped.state;
            videoErrUs = Number(mapped.timeUs) - (renderQUs + senderOfQ);
        }
        qNowUs = playQUs + 1_000 + rand() * 3_000;
        const tsUs = Math.round(playQUs + (rand() - 0.5) * 2_000 + 7_777_000);
        const pushed = pushAudio(chunker, samples, tsUs, senderClockUs(perf));
        chunker = pushed.state;
        for (const chunk of pushed.chunks) {
            // このチャンクの先頭のサンプルの本当の取り出しの時刻 (送信側の時計)。
            const trueUs = q0Us + chunkIndex * 480 * sampleQUs + senderOfQ;
            chunkIndex++;
            const av = Number(chunk.timestampUs) - trueUs - videoErrUs;
            const tQUs = q0Us + chunkIndex * 480 * sampleQUs;
            if (tQUs - q0Us >= 60_000_000 && Number.isNaN(result.startUs)) result.startUs = av;
            if (prev !== undefined && !Number.isNaN(result.startUs)) {
                result.maxJumpUs = Math.max(result.maxJumpUs, Math.abs(av - prev));
            }
            prev = av;
            result.endUs = av;
        }
    }
    result.restarts = chunker.clockMap.restarts + video.restarts;
    return result;
}

describe("10 時間の模擬", () => {
    afterEach(() => {
        vi.restoreAllMocks();
    });

    it(
        "音の時計が Q より +-50 と +-100 ppm、システム時刻が +-10 ppm で 1 秒飛んでも、音と映像のタイムスタンプの関係は動かない",
        () => {
            const cases: [number, number][] = [
                [100, 10],
                [50, -10],
                [-50, 10],
                [-100, -10],
            ];
            const lines: string[] = [];
            let seed = 1;
            for (const [dPpm, sPpm] of cases) {
                const r = simulate(dPpm, sPpm, 10, seed++);
                lines.push(
                    `D ${dPpm} ppm, S ${sPpm} ppm: 始め ${(r.startUs / 1000).toFixed(3)} ms, 終わり-始め ` +
                        `${((r.endUs - r.startUs) / 1000).toFixed(3)} ms, 最大の飛び ${(r.maxJumpUs / 1000).toFixed(3)} ms, ` +
                        `決め直し ${r.restarts}`,
                );
                // タイムスタンプの揺れ (音 +-1 ms、映像 +-0.2 ms) を除けば、関係は一定である。
                expect(Math.abs(r.endUs - r.startUs)).toBeLessThan(2_500);
                expect(r.maxJumpUs).toBeLessThan(5_000);
                expect(r.restarts).toBe(0);
            }
            console.log(lines.join("\n"));
        },
        120_000,
    );

    it(
        "pushAudio を通しても、音の時計が Q より +-100 ppm ずれた 10 時間で、チャンクの時刻と映像の換算の関係は動かない",
        () => {
            const lines: string[] = [];
            let seed = 101;
            for (const dPpm of [100, -100]) {
                const r = simulateChunks(dPpm, 10, seed++);
                lines.push(
                    `pushAudio, D ${dPpm} ppm: 終わり-始め ${((r.endUs - r.startUs) / 1000).toFixed(3)} ms, ` +
                        `最大の飛び ${(r.maxJumpUs / 1000).toFixed(3)} ms, 決め直し ${r.restarts}`,
                );
                // 断片の時刻の揺れ (+-1 ms) と映像の揺れを除けば、関係は一定である。
                expect(Math.abs(r.endUs - r.startUs)).toBeLessThan(2_500);
                expect(r.maxJumpUs).toBeLessThan(5_000);
                expect(r.restarts).toBe(0);
            }
            console.log(lines.join("\n"));
        },
        300_000,
    );

    it("システム時刻 (Date.now) が飛んでも、換算したタイムスタンプは 1 us も変わらない", () => {
        const run = (sJumpUs: number): bigint[] => {
            let qNowUs = 10_000_000_000;
            const perf = { timeOrigin: 1_699_999_000_000, now: () => qNowUs / 1000 };
            vi.spyOn(Date, "now").mockImplementation(() => Math.floor((1_700_000_000_000_000 + qNowUs + sJumpUs) / 1000));
            let chunker = createAudioChunker();
            const out: bigint[] = [];
            for (let i = 0; i < 1_000; i++) {
                qNowUs += 10_000;
                const result = pushAudio(chunker, new Float32Array(960), 123_000 + i * 10_000, senderClockUs(perf));
                chunker = result.state;
                for (const chunk of result.chunks) out.push(chunk.timestampUs);
            }
            vi.restoreAllMocks();
            return out;
        };
        expect(run(1_000_000)).toEqual(run(0));
    });
});
