// SPDX-License-Identifier: Apache-2.0

// encode/clock_map.ts のテスト。メディアのタイムスタンプを「トラックごとの定数」で送信側の時計に換算する。定数は最初の
// 到着の「到着の時刻 - メディアのタイムスタンプ」から始め、2 秒の期間の中の小さいほうから 3 番目が今の定数より小さくなったときに
// すぐ下げる。上げるのは、直近 20 秒の期間のどれでも 3 番目の差が今の定数より上のときで、少しずつ上げる。差が 200 ms を超えた
// 状態が 1 秒続いたときは、メディアの時計が替わったとみなして決め直す。最後に、音の時計 (D)、送信側の時計 (Q)、
// システム時刻 (S) を別々の速さにした 10 時間の模擬で、音と映像のタイムスタンプの関係が動かないことを確かめる。

import { afterEach, describe, expect, it, vi } from "vitest";

import { createAudioChunker, pushAudio } from "../audio/chunk";
import {
    CLOCK_MAP_JUMP_CONFIRM_US,
    CLOCK_MAP_JUMP_US,
    CLOCK_MAP_RAISE_MIN_US,
    CLOCK_MAP_RAISE_RATE_US_PER_S,
    CLOCK_MAP_RAISE_STEP_MAX_US,
    CLOCK_MAP_RAISE_WINDOWS,
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

/* ==========================================================================
   上げる向き
   ========================================================================== */

/** runVideo() の結果。配列はフレームごと。 */
interface VideoRun {
    /** 始めからの時刻 (us)。 */
    tUs: number[];
    /** 換算した時刻 - 詰まりのない到着の時刻 (us)。メディアのタイムスタンプが先に出ている間も、この値が変わらないのが正しい。 */
    errUs: number[];
    offsets: bigint[];
    restarts: number;
    /** 決め直しでなく、定数が前のフレームより上がった回数。 */
    raises: number;
    /** 決め直しでなく上がったときの、1 フレームで上がった量の最大 (us)。 */
    maxRaiseStepUs: bigint;
}

/**
 * 60 fps の映像を seconds 秒流す。aheadUs(t) は、メディアのタイムスタンプが本当の時刻より先に出る量 (WebRTC のデコードの
 * 時間の見積もりが大きい間は正。到着の時刻 - メディアのタイムスタンプ はその分だけ小さくなる)。delayUs(t) は詰まりで、
 * 到着がその分だけ遅れる。取り出しの遅れは 1 から 6 ms で、100 フレームに 1 つ 80 ms 詰まる (10 時間の模擬と同じ)。
 * aheadUs と delayUs は乱数を使わないので、同じ種なら、取り出しの遅れの列はどの呼び出しでも同じである。
 */
function runVideo(
    seconds: number,
    aheadUs: (tUs: number) => number,
    delayUs: (tUs: number) => number,
    seed: number,
): VideoRun {
    const rand = rng(seed);
    let state = createClockMap();
    const out: VideoRun = { tUs: [], errUs: [], offsets: [], restarts: 0, raises: 0, maxRaiseStepUs: 0n };
    const frames = Math.floor((seconds * 1_000_000) / FRAME_US);
    for (let i = 0; i < frames; i++) {
        const tUs = i * FRAME_US;
        const lagUs = Math.round(1_000 + rand() * 5_000 + (rand() < 0.01 ? 80_000 : 0) + delayUs(tUs));
        const before = state.offsetUs;
        const result = mapMediaTime(state, MEDIA_EPOCH_US + tUs + Math.round(aheadUs(tUs)), SENDER_EPOCH_US + BigInt(tUs + lagUs));
        state = result.state;
        const after = state.offsetUs as bigint;
        if (result.restart === undefined && before !== undefined && after > before) {
            out.raises++;
            if (after - before > out.maxRaiseStepUs) out.maxRaiseStepUs = after - before;
        }
        out.tUs.push(tUs);
        out.errUs.push(Number(result.timeUs - (SENDER_EPOCH_US + BigInt(tUs))));
        out.offsets.push(after);
    }
    out.restarts = state.restarts;
    return out;
}

/** 区間 [fromUs, toUs) で value から 0 まで、またはその逆に、直線で動く量。 */
function ramp(tUs: number, fromUs: number, toUs: number, fromValue: number, toValue: number): number {
    if (tUs <= fromUs) return fromValue;
    if (tUs >= toUs) return toValue;
    return fromValue + ((toValue - fromValue) * (tUs - fromUs)) / (toUs - fromUs);
}

/** 受信を始めた直後の下がり。1-2 秒でメディアのタイムスタンプが 150 ms 先に出て、10 秒から 20 秒の間に戻る。 */
function startDipAhead(tUs: number): number {
    return tUs < 10_000_000 ? ramp(tUs, 1_000_000, 2_000_000, 0, 150_000) : ramp(tUs, 10_000_000, 20_000_000, 150_000, 0);
}

/** 正しい値 (同じ取り出しの遅れで、下がりのない列の定数) との差が 1 ms 以内に戻り、その後ずっと保った時刻 (us)。 */
function recoveredAtUs(run: VideoRun, reference: VideoRun): number {
    let last = 0;
    for (let i = 0; i < run.offsets.length; i++) {
        const diff = (run.offsets[i] as bigint) - (reference.offsets[i] as bigint);
        if (diff > 1_000n || diff < -1_000n) last = run.tUs[i + 1] ?? Number.POSITIVE_INFINITY;
    }
    return last;
}

/** 区間 [fromUs, 終わり) の、隣り合うフレームの errUs の差の最大 (us)。 */
function maxErrJumpUs(run: VideoRun, fromUs: number): number {
    let max = 0;
    for (let i = 1; i < run.errUs.length; i++) {
        if ((run.tUs[i] as number) < fromUs) continue;
        max = Math.max(max, Math.abs((run.errUs[i] as number) - (run.errUs[i - 1] as number)));
    }
    return max;
}

const NO_SHIFT = (): number => 0;

describe("mapMediaTime: 上げる向き", () => {
    it("値の関係: 20 秒 (2 秒の期間 10 個) の後に、1 秒あたり 10 ms まで、1 回で 1 ms まで上げる。2 ms より小さい差では上げ始めない", () => {
        expect(BigInt(CLOCK_MAP_RAISE_WINDOWS) * CLOCK_MAP_WINDOW_US).toBe(20_000_000n);
        expect(CLOCK_MAP_RAISE_RATE_US_PER_S).toBe(10_000n);
        expect(CLOCK_MAP_RAISE_STEP_MAX_US).toBe(1_000n);
        expect(CLOCK_MAP_RAISE_MIN_US).toBe(2_000n);
    });

    it("受信を始めた直後に差が 150 ms 下がり、10 秒から 20 秒の間に戻る列では、定数が 60 秒以内に正しい値へ戻り、戻る間に 5 ms を超える飛びがない", () => {
        const reference = runVideo(120, NO_SHIFT, NO_SHIFT, 7);
        const run = runVideo(120, startDipAhead, NO_SHIFT, 7);
        const recovered = recoveredAtUs(run, reference);
        console.log(
            `直線で戻る下がり: 正しい値に戻った時刻 ${(recovered / 1e6).toFixed(1)} s, 戻る間の最大の飛び ` +
                `${(maxErrJumpUs(run, 10_000_000) / 1000).toFixed(3)} ms, 上げた回数 ${run.raises}`,
        );
        expect(run.restarts).toBe(0);
        expect(reference.restarts).toBe(0);
        expect(reference.raises).toBe(0);
        // 下がった定数は、差が戻った後も直近 20 秒の期間に下がった差が残っている間は上がらない。
        const at30 = run.tUs.findIndex((t) => t >= 30_000_000);
        expect((reference.offsets[at30] as bigint) - (run.offsets[at30] as bigint)).toBeGreaterThan(140_000n);
        // その後 15 秒ほどで上がりきる (目安: 直近の期間の 3 番目の差が上がり始めるのが 32 秒、1 秒あたり 10 ms で 150 ms)。
        expect(recovered).toBeLessThanOrEqual(60_000_000);
        expect(recovered).toBeGreaterThan(40_000_000);
        expect(maxErrJumpUs(run, 10_000_000)).toBeLessThanOrEqual(5_000);
        expect(run.maxRaiseStepUs).toBeLessThanOrEqual(CLOCK_MAP_RAISE_STEP_MAX_US);
        // 戻った後は、換算した時刻の誤差が下がりのない列と同じになる。
        const last = run.errUs.length - 1;
        expect(Math.abs((run.errUs[last] as number) - (reference.errUs[last] as number))).toBeLessThanOrEqual(1_000);
    });

    it("差が 20 秒の時点でまとめて戻る列 (戻りの遅い側) でも、定数は 60 秒以内に正しい値へ戻る", () => {
        const stepAhead = (tUs: number): number => (tUs < 20_000_000 ? ramp(tUs, 1_000_000, 2_000_000, 0, 150_000) : 0);
        const reference = runVideo(120, NO_SHIFT, NO_SHIFT, 8);
        const run = runVideo(120, stepAhead, NO_SHIFT, 8);
        const recovered = recoveredAtUs(run, reference);
        console.log(`20 秒で戻る下がり: 正しい値に戻った時刻 ${(recovered / 1e6).toFixed(1)} s`);
        expect(run.restarts).toBe(0);
        expect(recovered).toBeLessThanOrEqual(60_000_000);
        // 上げる間の飛びは小さい (差がまとめて戻る 20 秒の 1 フレームは、メディアのタイムスタンプそのものの飛びなので除く)。
        expect(maxErrJumpUs(run, 21_000_000)).toBeLessThanOrEqual(5_000);
    });

    it("上げている途中に差がまた下がると、その期間に見た差まで下げ、それより上には上げない", () => {
        // 10 秒まで 150 ms 先に出て戻る (32 秒ごろから上げ始める)。38 秒から 60 秒まではまた 100 ms 先に出る。
        const ahead = (tUs: number): number =>
            tUs >= 1_000_000 && tUs < 10_000_000 ? 150_000 : tUs >= 38_000_000 && tUs < 60_000_000 ? 100_000 : 0;
        const reference = runVideo(70, NO_SHIFT, NO_SHIFT, 15);
        const run = runVideo(70, ahead, NO_SHIFT, 15);
        const at = (s: number): number => run.tUs.findIndex((t) => t >= s * 1_000_000);
        // 37 秒には上げている途中 (正しい値より 50 ms 以上低く、100 ms 以上は低くない)。
        const gap37 = (reference.offsets[at(37)] as bigint) - (run.offsets[at(37)] as bigint);
        expect(gap37 > 50_000n && gap37 < 100_000n).toBe(true);
        // 38 秒の後に初めて下げてから 59 秒までは、定数は 1 度も上がらない。下げた期間が終わる前に、前の期間までの差を
        // 目標にして、上げては下げることを繰り返さない。
        expect((reference.offsets[at(39)] as bigint) - (run.offsets[at(39)] as bigint)).toBeGreaterThan(98_000n);
        let dropped = at(38);
        while ((run.offsets[dropped] as bigint) >= (run.offsets[dropped - 1] as bigint)) dropped++;
        let raisedAfterDrop = 0;
        for (let i = dropped + 1; i < at(59); i++) {
            if ((run.offsets[i] as bigint) > (run.offsets[i - 1] as bigint)) raisedAfterDrop++;
        }
        expect(raisedAfterDrop).toBe(0);
        expect(run.restarts).toBe(0);
    });

    it("300 ms の詰まりが 1 秒より長く続くと決め直し、詰まりが終われば元の定数に戻り、上げる規則は働かない", () => {
        const jam = (tUs: number): number => (tUs >= 30_000_000 && tUs < 33_000_000 ? 300_000 : 0);
        const reference = runVideo(70, NO_SHIFT, NO_SHIFT, 9);
        let state = createClockMap();
        const rand = rng(9);
        let restart: { shiftUs: bigint; jumpForUs: bigint; atUs: number } | undefined;
        let raised = false;
        const frames = Math.floor(70_000_000 / FRAME_US);
        for (let i = 0; i < frames; i++) {
            const tUs = i * FRAME_US;
            const lagUs = Math.round(1_000 + rand() * 5_000 + (rand() < 0.01 ? 80_000 : 0) + jam(tUs));
            const before = state.offsetUs;
            const result = mapMediaTime(state, MEDIA_EPOCH_US + tUs, SENDER_EPOCH_US + BigInt(tUs + lagUs));
            state = result.state;
            if (result.restart !== undefined) {
                expect(restart).toBeUndefined(); // 決め直しは 1 回だけ
                restart = { shiftUs: result.restart.shiftUs, jumpForUs: result.restart.jumpForUs, atUs: tUs };
            } else if (before !== undefined && (state.offsetUs as bigint) > before) {
                raised = true;
            }
        }
        expect(restart).toBeDefined();
        // 詰まりが始まってから 1 秒で決め直す (到着の時刻で数えるので、取り出しの遅れの揺れとたまの 80 ms の詰まりの分だけ
        // 前後する)。量は詰まりの 300 ms に、決め直したフレームの取り出しの遅れ (1 から 6 ms、たまに 80 ms 足される) を足した値。
        expect(restart?.jumpForUs ?? 0n).toBeGreaterThanOrEqual(CLOCK_MAP_JUMP_CONFIRM_US);
        expect((restart?.atUs ?? 0) - 30_000_000).toBeGreaterThan(900_000);
        expect((restart?.atUs ?? 0) - 30_000_000).toBeLessThan(1_100_000);
        expect(Number(restart?.shiftUs ?? 0n)).toBeGreaterThan(295_000);
        expect(Number(restart?.shiftUs ?? 0n)).toBeLessThan(390_000);
        expect(raised).toBe(false);
        expect(state.restarts).toBe(1);
        // 詰まりが終わると、詰まりのない到着が 3 つそろった時点で下げ、下がりのない列の定数との差は揺れの分だけになる。
        const end = (reference.offsets[reference.offsets.length - 1] as bigint) - (state.offsetUs as bigint);
        expect(end >= -1_000n && end <= 1_000n).toBe(true);
    });

    it("20 秒より短い詰まり、繰り返す詰まり、ゆっくりした揺れでは、定数を上げない", () => {
        // 18 秒続く 30 ms の詰まり。
        const once = runVideo(120, NO_SHIFT, (t) => (t >= 30_000_000 && t < 48_000_000 ? 30_000 : 0), 11);
        // 15 秒の 30 ms の詰まりと 5 秒の詰まりのない区間の繰り返し (5 分)。
        const repeated = runVideo(300, NO_SHIFT, (t) => (t % 20_000_000 < 15_000_000 ? 30_000 : 0), 12);
        // JavaScript の数秒の詰まりがたびたび起きる列 (5 分)。1 秒ごとに 2 割の確率で、0.5 から 3 秒の 20 から 100 ms の詰まり。
        const jamRand = rng(99);
        const jams: [number, number, number][] = [];
        for (let s = 0; s < 300; s++) {
            if (jamRand() < 0.2) jams.push([s * 1e6, s * 1e6 + 500_000 + jamRand() * 2_500_000, 20_000 + jamRand() * 80_000]);
        }
        const hiccups = runVideo(300, NO_SHIFT, (t) => jams.find(([a, b]) => t >= a && t < b)?.[2] ?? 0, 13);
        // メディアのタイムスタンプが 15 秒の周期で 0 から 20 ms 先に出る揺れ (デコードの時間の見積もりの揺れ) (5 分)。
        const wave = runVideo(300, (t) => 10_000 * (1 - Math.cos((2 * Math.PI * t) / 15_000_000)), NO_SHIFT, 14);
        const lines: string[] = [];
        for (const [name, run] of [
            ["18 秒の詰まり", once],
            ["繰り返す詰まり", repeated],
            ["数秒の詰まり", hiccups],
            ["15 秒の周期の揺れ", wave],
        ] as const) {
            lines.push(`${name}: 上げた回数 ${run.raises}, 決め直し ${run.restarts}`);
            expect(run.raises).toBe(0);
            expect(run.restarts).toBe(0);
        }
        console.log(lines.join("\n"));
    });

    it("音の列 (10 ms の断片、取り出しの揺れ、起動の直後とたまの詰まりでまとめて届く) では、定数は上がらない", () => {
        const rand = rng(21);
        let chunker = createAudioChunker();
        let raises = 0;
        let stallUntilUs = 300_000; // 起動の直後は、始めの 300 ms 分がまとめて届く。
        const fragments = 60_000; // 10 分
        const samples = new Float32Array(960);
        for (let i = 0; i < fragments; i++) {
            const tUs = i * 10_000;
            // 200 個に 1 つ、30 から 150 ms の詰まりが始まり、その間の断片は詰まりが終わったときにまとめて届く。
            if (tUs >= stallUntilUs && rand() < 0.005) stallUntilUs = tUs + 30_000 + rand() * 120_000;
            const arrival = Math.max(tUs, stallUntilUs) + 1_000 + rand() * 3_000;
            const media = MEDIA_EPOCH_US + tUs + Math.round((rand() - 0.5) * 2_000);
            const before = chunker.clockMap.offsetUs;
            const result = pushAudio(chunker, samples, media, SENDER_EPOCH_US + BigInt(Math.round(arrival)));
            chunker = result.state;
            const after = chunker.clockMap.offsetUs as bigint;
            if (before !== undefined && after > before) raises++;
        }
        expect(raises).toBe(0);
        expect(chunker.clockMap.restarts).toBe(0);
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
    /** 音と映像の定数が、決め直しでなく前の値より上がった回数の合計。 */
    raises: number;
}

/** 定数が、決め直しでなく前の値より上がったら 1。 */
function raisedOnce(before: ClockMapState, after: ClockMapState): number {
    return before.offsetUs !== undefined && after.restarts === before.restarts && (after.offsetUs as bigint) > before.offsetUs
        ? 1
        : 0;
}

/**
 * 送信側の 10 時間の模擬。Q は送信側の時計と同じ単調な時計で、AudioData.timestamp (音を再生のために取り出した時刻) と
 * VideoFrame.timestamp (WebRTC が決めた描画の時刻) はどちらも Q から作った値である。音は D の速さで 10 ms ずつ取り出され
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
    const videoOriginUs = -2_000_000; // VideoFrame.timestamp の原点 (Q からの差)
    let audio = createClockMap();
    let video = createClockMap();
    const d = dPpm * 1e-6;
    const chunkQUs = 10_000 / (1 + d);
    let nextFrame = 0;
    let videoErrUs = 0;
    const result: SimResult = { startUs: Number.NaN, endUs: 0, maxJumpUs: 0, restarts: 0, raises: 0 };
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
            result.raises += raisedOnce(video, mapped.state);
            video = mapped.state;
            videoErrUs = Number(mapped.timeUs) - (renderQUs + 1_699_999_000_000_000 - 3_000_000_000);
        }
        // 音の取り出しの時刻の揺れは +-1 ms。届くまでの遅れは 1 から 4 ms で、200 個に 1 つ、50 ms 詰まる。
        const tsQUs = playQUs + (rand() - 0.5) * 2_000;
        qNowUs = playQUs + 1_000 + rand() * 3_000 + (rand() < 0.005 ? 50_000 : 0);
        const mapped = mapMediaTime(audio, Math.round(tsQUs + audioOriginUs), senderClockUs(perf));
        result.raises += raisedOnce(audio, mapped.state);
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
 * 引いて、音と映像のタイムスタンプの関係とする。videoAheadUs(t) は、映像のメディアのタイムスタンプが本当の描画の時刻より
 * 先に出る量 (受信を始めた直後のデコードの時間の見積もりの分。t は始めからの Q の us)。startUs と maxJumpUs は、
 * measureFromUs (既定は 60 秒) から測る。
 */
function simulateChunks(
    dPpm: number,
    hours: number,
    seed: number,
    videoAheadUs: (tUs: number) => number = () => 0,
    measureFromUs = 60_000_000,
): SimResult {
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
    const result: SimResult = { startUs: Number.NaN, endUs: 0, maxJumpUs: 0, restarts: 0, raises: 0 };
    let prev: number | undefined;
    const fragments = Math.floor((hours * 3_600_000_000) / (fragmentFrames * sampleQUs));
    for (let f = 0; f < fragments; f++) {
        const playQUs = q0Us + f * fragmentFrames * sampleQUs;
        for (;;) {
            const renderQUs = q0Us + 20_000 + nextFrame * (1_000_000 / 60);
            if (renderQUs > playQUs) break;
            nextFrame++;
            qNowUs = renderQUs + 1_000 + rand() * 5_000;
            const media = renderQUs - q0Us - 2_000_000 + videoAheadUs(renderQUs - q0Us);
            const mapped = mapMediaTime(video, Math.round(media), senderClockUs(perf));
            result.raises += raisedOnce(video, mapped.state);
            video = mapped.state;
            videoErrUs = Number(mapped.timeUs) - (renderQUs + senderOfQ);
        }
        qNowUs = playQUs + 1_000 + rand() * 3_000;
        const tsUs = Math.round(playQUs + (rand() - 0.5) * 2_000 + 7_777_000);
        const pushed = pushAudio(chunker, samples, tsUs, senderClockUs(perf));
        result.raises += raisedOnce(chunker.clockMap, pushed.state.clockMap);
        chunker = pushed.state;
        for (const chunk of pushed.chunks) {
            // このチャンクの先頭のサンプルの本当の取り出しの時刻 (送信側の時計)。
            const trueUs = q0Us + chunkIndex * 480 * sampleQUs + senderOfQ;
            chunkIndex++;
            const av = Number(chunk.timestampUs) - trueUs - videoErrUs;
            const tQUs = q0Us + chunkIndex * 480 * sampleQUs;
            if (tQUs - q0Us >= measureFromUs && Number.isNaN(result.startUs)) result.startUs = av;
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
                        `決め直し ${r.restarts}, 上げた回数 ${r.raises}`,
                );
                // タイムスタンプの揺れ (音 +-1 ms、映像 +-0.2 ms) を除けば、関係は一定である。差の揺れは上げ始める
                // 2 ms より小さいので、定数は上がらない。
                expect(Math.abs(r.endUs - r.startUs)).toBeLessThan(2_500);
                expect(r.maxJumpUs).toBeLessThan(5_000);
                expect(r.restarts).toBe(0);
                expect(r.raises).toBe(0);
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
                        `最大の飛び ${(r.maxJumpUs / 1000).toFixed(3)} ms, 決め直し ${r.restarts}, 上げた回数 ${r.raises}`,
                );
                // 断片の時刻の揺れ (+-1 ms) と映像の揺れを除けば、関係は一定である。
                expect(Math.abs(r.endUs - r.startUs)).toBeLessThan(2_500);
                expect(r.maxJumpUs).toBeLessThan(5_000);
                expect(r.restarts).toBe(0);
                expect(r.raises).toBe(0);
            }
            console.log(lines.join("\n"));
        },
        300_000,
    );

    it("pushAudio を通し、映像だけが受信を始めた直後に 150 ms 下がる列でも、60 秒の時点で音と映像の関係は下がりのない列と同じになり、戻る間に 5 ms を超える飛びがない", () => {
        const lines: string[] = [];
        let seed = 201;
        for (const dPpm of [100, -100]) {
            const reference = simulateChunks(dPpm, 3 / 60, seed);
            const dipped = simulateChunks(dPpm, 3 / 60, seed, startDipAhead);
            // 差が戻り始める 10 秒から測る (下がる 1-2 秒は、メディアのタイムスタンプが先へ動くのに定数が追いつく間)。
            const returning = simulateChunks(dPpm, 3 / 60, seed, startDipAhead, 10_000_000);
            seed++;
            lines.push(
                `pushAudio、映像の下がり, D ${dPpm} ppm: 60 秒の時点の差 ${((dipped.startUs - reference.startUs) / 1000).toFixed(3)} ms, ` +
                    `終わりの差 ${((dipped.endUs - reference.endUs) / 1000).toFixed(3)} ms, 10 秒からの最大の飛び ` +
                    `${(returning.maxJumpUs / 1000).toFixed(3)} ms, 上げた回数 ${dipped.raises}`,
            );
            expect(Math.abs(dipped.startUs - reference.startUs)).toBeLessThan(1_000);
            expect(Math.abs(dipped.endUs - reference.endUs)).toBeLessThan(1_000);
            expect(returning.maxJumpUs).toBeLessThan(5_000);
            expect(dipped.restarts).toBe(0);
            expect(dipped.raises).toBeGreaterThan(0);
            expect(reference.raises).toBe(0);
        }
        console.log(lines.join("\n"));
    });

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
