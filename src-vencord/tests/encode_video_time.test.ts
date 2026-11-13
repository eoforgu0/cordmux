// SPDX-License-Identifier: Apache-2.0

// encode/video_time.ts のテスト。受けた映像のタイムスタンプが描く予定の時刻 (TimeTicks の値) なら見積もりなしに換算し、
// そうでなければ今までの換算 (encode/clock_map.ts) を使う。見分けは先行 (換算した時刻 - 読んだ時刻) の符号と大きさで、
// ストリームの途中でも見分け続ける。

import { describe, expect, it } from "vitest";

import { createClockMap, mapMediaTime } from "../encode/clock_map";
import { pageTimebase, qpcTimebase } from "../encode/sender_clock";
import {
    VIDEO_EXACT_EXIT_FRAMES,
    createVideoTime,
    mapVideoTime,
    videoTimeChangeText,
    videoTimeOriginUs,
    type VideoTimeChange,
    type VideoTimeState,
} from "../encode/video_time";

/** ページの原点の TimeTicks (テスト用の送信側の試しで測った値)。 */
const TICKS_ORIGIN_US = 269_692_077_700n;
const QPC = qpcTimebase(TICKS_ORIGIN_US);
/** 60 fps の間隔 (us)。 */
const FRAME_US = 16_667n;

interface Step {
    /** 映像のタイムスタンプ (VideoFrame.timestamp。us)。 */
    mediaUs: bigint;
    /** 読んだときの送信側の時計 (us)。 */
    arrivalUs: bigint;
}

/** 列を順に換算し、送った時刻と、方法を決めたか替えたときの記録を返す。 */
function run(steps: readonly Step[], timebase = QPC, start: VideoTimeState = createVideoTime()) {
    let state = start;
    const times: bigint[] = [];
    const changes: Array<{ index: number; change: VideoTimeChange }> = [];
    steps.forEach((s, index) => {
        const r = mapVideoTime(state, timebase, Number(s.mediaUs), s.arrivalUs);
        state = r.state;
        times.push(r.timeUs);
        if (r.change !== undefined) changes.push({ index, change: r.change });
    });
    return { state, times, changes };
}

/** 描く予定の時刻のフレームの列。i 番目のフレームを、描く予定の時刻の leadUs だけ前に読む。 */
function renderTimeSteps(count: number, startTicksUs: bigint, leadUs: (i: number) => bigint): Step[] {
    const out: Step[] = [];
    for (let i = 0; i < count; i++) {
        const mediaUs = startTicksUs + BigInt(i) * FRAME_US;
        out.push({ mediaUs, arrivalUs: mediaUs - leadUs(i) });
    }
    return out;
}

describe("mapVideoTime: 描く予定の時刻", () => {
    it("最初のフレームから正確に換算し、送る時刻は TimeTicks の値そのもの (qpc)", () => {
        const steps = renderTimeSteps(120, 300_000_000_000n, () => 13_900n);
        const { state, times, changes } = run(steps);
        expect(times).toEqual(steps.map((s) => s.mediaUs));
        expect(changes).toEqual([{ index: 0, change: { mode: "exact", reason: "render-time", leadUs: 13_900n } }]);
        expect(state.mode).toBe("exact");
        expect(state.switches).toBe(0);
        expect(videoTimeOriginUs(state)).toBe(0n);
    });

    it("受信を始めた直後に描く予定の時刻が 150 ms 先にあっても、そのまま換算する (見積もりの上げ下げをしない)", () => {
        const steps = renderTimeSteps(600, 300_000_000_000n, (i) => (i < 300 ? 150_000n - BigInt(i) * 450n : 15_000n));
        const { times, changes } = run(steps);
        expect(times).toEqual(steps.map((s) => s.mediaUs));
        expect(changes).toHaveLength(1);
    });

    it("読むのが遅れたフレーム (先行が負) が数個あっても、正確な換算をやめない", () => {
        // 試しの記録の 1 フレームの -10.8 ms と、詰まりで 5 フレームが 40 ms 遅れた形。
        const steps = renderTimeSteps(300, 300_000_000_000n, (i) => (i === 50 ? -10_800n : i >= 100 && i < 105 ? -40_000n : 12_000n));
        const { state, times, changes } = run(steps);
        expect(times).toEqual(steps.map((s) => s.mediaUs));
        expect(changes).toHaveLength(1);
        expect(state.mode).toBe("exact");
    });

    it("画面が止まっていて、間を空けて届く数個のフレームが遅れても、正確な換算をやめない (数で見る)", () => {
        const steps: Step[] = renderTimeSteps(10, 300_000_000_000n, () => 12_000n);
        // 5 秒ごとに 1 フレーム。どれも読むのが 30 ms 遅れた。証拠のないフレームは 9 個で、VIDEO_EXACT_EXIT_FRAMES に届かない。
        for (let k = 1; k < VIDEO_EXACT_EXIT_FRAMES; k++) {
            const mediaUs = 300_000_000_000n + BigInt(k) * 5_000_000n;
            steps.push({ mediaUs, arrivalUs: mediaUs + 30_000n });
        }
        const { state, times } = run(steps);
        expect(state.mode).toBe("exact");
        expect(times).toEqual(steps.map((s) => s.mediaUs));
    });
});

describe("mapVideoTime: 描く予定の時刻でないとき", () => {
    it("途中で取り込みの時刻に変わったら、証拠のないフレームが 10 個かつ 1 秒続いた所で今までの換算に戻る", () => {
        // 2.16 秒 (130 フレーム) まで描く予定の時刻、その後は取り込みの時刻 (先行 -25 ms)。試しの記録の両方向の組の形。
        const steps = renderTimeSteps(130, 300_000_000_000n, () => 12_000n);
        const switchAt = steps.length;
        const firstCapture = (steps[switchAt - 1] as Step).mediaUs + FRAME_US - 26_000n; // 残りが -26 ms 跳ぶ
        for (let i = 0; i < 180; i++) {
            const mediaUs = firstCapture + BigInt(i) * FRAME_US;
            steps.push({ mediaUs, arrivalUs: mediaUs + 25_000n });
        }
        const { state, times, changes } = run(steps);
        expect(changes).toHaveLength(2);
        const exit = changes[1];
        // 1 秒 (60 フレーム) 続いた所で戻る。それまでは取り込みの時刻のまま送る。
        expect(exit?.change.mode).toBe("estimated");
        expect(exit?.change.reason).toBe("not-render-time");
        const exitIndex = exit?.index as number;
        expect(exitIndex - switchAt).toBeGreaterThanOrEqual(59);
        expect(exitIndex - switchAt).toBeLessThanOrEqual(61);
        for (let i = switchAt; i < exitIndex; i++) expect(times[i]).toBe((steps[i] as Step).mediaUs);
        // 戻った後は、作り直した clock_map で換算する (最初の差 = 到着 - メディア が定数)。
        let cm = createClockMap();
        for (let i = exitIndex; i < steps.length; i++) {
            const s = steps[i] as Step;
            const m = mapMediaTime(cm, Number(s.mediaUs), s.arrivalUs);
            cm = m.state;
            expect(times[i]).toBe(m.timeUs);
        }
        expect(state.mode).toBe("estimated");
        expect(state.switches).toBe(1);
    });

    it("正確な換算の間に先行が秒の桁より外れるフレーム (ほかの時計の値) が来たら、待たずにすぐ戻る", () => {
        const steps = renderTimeSteps(30, 300_000_000_000n, () => 12_000n);
        const last = steps[steps.length - 1] as Step;
        // 経過の値 (0 から始まる) に変わった。
        steps.push({ mediaUs: 16_667n, arrivalUs: last.arrivalUs + FRAME_US });
        const { state, times, changes } = run(steps);
        expect(changes.map((c) => [c.index, c.change.mode, c.change.reason])).toEqual([
            [0, "exact", "render-time"],
            [30, "estimated", "other-clock"],
        ]);
        // 作り直した clock_map の最初の値 (読んだ時刻)。経過の値をそのまま送らない。
        expect(times[30]).toBe(last.arrivalUs + FRAME_US);
        expect(state.mode).toBe("estimated");
    });

    it("最初から取り込みの時刻なら、最初のフレームから今までの換算で、描く予定の時刻には入らない", () => {
        const steps: Step[] = [];
        for (let i = 0; i < 300; i++) {
            const mediaUs = 300_000_000_000n + BigInt(i) * FRAME_US;
            steps.push({ mediaUs, arrivalUs: mediaUs + 5_300n + BigInt(i % 7) * 3_000n });
        }
        const { state, changes } = run(steps);
        expect(changes).toEqual([{ index: 0, change: { mode: "estimated", reason: "not-render-time", leadUs: -5_300n } }]);
        expect(state.mode).toBe("estimated");
        expect(state.switches).toBe(0);
    });

    it("最初のフレームからの経過なら other-clock で今までの換算", () => {
        const steps: Step[] = [];
        for (let i = 0; i < 10; i++) steps.push({ mediaUs: BigInt(i) * FRAME_US, arrivalUs: TICKS_ORIGIN_US + 60_000_000n + BigInt(i) * FRAME_US });
        const { changes } = run(steps);
        expect(changes[0]?.change.mode).toBe("estimated");
        expect(changes[0]?.change.reason).toBe("other-clock");
    });

    it("今までの換算の間に描く予定の時刻のフレームが来たら、正確な換算に入る", () => {
        const steps: Step[] = [];
        for (let i = 0; i < 30; i++) {
            const mediaUs = 300_000_000_000n + BigInt(i) * FRAME_US;
            steps.push({ mediaUs, arrivalUs: mediaUs + 30_000n });
        }
        steps.push(...renderTimeSteps(30, 300_000_000_000n + 30n * FRAME_US + 30_000n, () => 12_000n));
        const { state, changes, times } = run(steps);
        expect(changes.map((c) => c.change.mode)).toEqual(["estimated", "exact"]);
        expect(changes[1]?.index).toBe(30);
        expect(times[30]).toBe((steps[30] as Step).mediaUs);
        expect(state.switches).toBe(1);
    });

    it("送信側の時計が page なら、どのフレームも今までの換算で、no-ticks-origin を 1 回だけ伝える", () => {
        const page = pageTimebase({ timeOrigin: 1_794_285_743_088 });
        const steps = renderTimeSteps(30, 300_000_000_000n, () => 13_000n).map((s) => ({
            mediaUs: s.mediaUs,
            arrivalUs: s.arrivalUs + 1_794_285_743_088_000n - TICKS_ORIGIN_US,
        }));
        const { times, changes, state } = run(steps, page);
        expect(changes).toEqual([{ index: 0, change: { mode: "estimated", reason: "no-ticks-origin", leadUs: undefined } }]);
        let cm = createClockMap();
        steps.forEach((s, i) => {
            const m = mapMediaTime(cm, Number(s.mediaUs), s.arrivalUs);
            cm = m.state;
            expect(times[i]).toBe(m.timeUs);
        });
        expect(videoTimeOriginUs(state)).toBe(cm.offsetUs);
    });

    it("メディアのタイムスタンプが有限の数でなければ RangeError を投げる", () => {
        expect(() => mapVideoTime(createVideoTime(), QPC, Number.NaN, 1n)).toThrow(RangeError);
    });
});

describe("videoTimeChangeText", () => {
    it("方法、理由、先行 (ms) を書く", () => {
        expect(videoTimeChangeText({ mode: "exact", reason: "render-time", leadUs: 13_940n })).toBe(
            "time=exact reason=render-time lead=13.9ms",
        );
        expect(videoTimeChangeText({ mode: "estimated", reason: "no-ticks-origin", leadUs: undefined })).toBe(
            "time=estimated reason=no-ticks-origin",
        );
    });
});
