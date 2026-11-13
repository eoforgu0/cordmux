// SPDX-License-Identifier: Apache-2.0

// encode/av_diag.ts の、getStats() の結果の読み取り、1 秒ごとの区間の G、音声の断片の飛び、映像の先行の足し上げ、定期の行の
// 書式を確認する。

import { describe, expect, it } from "vitest";

import type { StatsReportLike } from "../adapter/types";
import {
    addVideoLead,
    AUDIO_GAP_FRACTION,
    avDiagInterval,
    avDiagReport,
    createAudioReadSpan,
    createAvDiagState,
    createVideoLeadSpan,
    medianOf,
    noteAudioRead,
    readAudioArrival,
    readMediaPlayout,
    readProcessorCount,
    readVideoArrival,
    takeAudioReadSpan,
    takeVideoLeadSpan,
    type AudioArrivalSample,
    type AvDiagInput,
    type AvDiagState,
    type VideoArrivalSample,
    type VideoLeadSpan,
} from "../encode/av_diag";

function report(rows: unknown[]): StatsReportLike {
    return { forEach: (cb) => rows.forEach((r) => cb(r)) };
}

/** 音声の累計。秒 t のとき、40 ms 待ったサンプルを 48000 x t、20 ms のパケットを 50 x t 個出した値。 */
function audioAt(t: number, extra: Partial<AudioArrivalSample> = {}): AudioArrivalSample {
    return {
        totalProcessingDelayS: 48_000 * t * 0.04,
        jitterBufferEmittedCount: 48_000 * t,
        packetsReceived: 50 * t,
        packetsDiscarded: 0,
        concealedSamples: 0,
        clockRateHz: 48_000,
        ...extra,
    };
}

/** 映像の累計。秒 t のとき、20 ms 待ったフレームを 30 x t 個デコードした値。 */
function videoAt(t: number, extra: Partial<VideoArrivalSample> = {}): VideoArrivalSample {
    return { totalProcessingDelayS: 30 * t * 0.02, framesDecoded: 30 * t, framesDropped: 0, freezeCount: 0, pauseCount: 0, ...extra };
}

/** 先行 12 ms のフレーム 30 個。 */
function lead(count = 30, leadUs = 12_000, estimated = 0): VideoLeadSpan {
    return { sumUs: count * leadUs, count, estimated };
}

function input(t: number, over: Partial<AvDiagInput> = {}): AvDiagInput {
    return {
        audio: { trackId: "a1", sample: audioAt(t) },
        video: { trackId: "v1", sample: videoAt(t) },
        playout: { totalPlayoutDelayS: 48_000 * t * 0.03, totalSamplesCount: 48_000 * t },
        lead: lead(),
        audioTimeIsFetch: true,
        ...over,
    };
}

/** t = 1, 2, ... n の区間を足した状態 (最初の区間は前の値がないので reset)。 */
function run(n: number, over: (t: number) => Partial<AvDiagInput> = () => ({})): AvDiagState {
    let state = createAvDiagState(0);
    for (let t = 1; t <= n; t++) state = avDiagInterval(state, input(t, over(t))).state;
    return state;
}

describe("readAudioArrival、readVideoArrival、readMediaPlayout", () => {
    it("inbound-rtp と、codecId が指す codec の行の clockRate を読む", () => {
        const r = report([
            { type: "codec", id: "C1", clockRate: 48_000 },
            {
                type: "inbound-rtp",
                kind: "audio",
                codecId: "C1",
                totalProcessingDelay: 12.5,
                jitterBufferEmittedCount: 480_000,
                packetsReceived: 500,
                packetsDiscarded: 2,
                concealedSamples: 960,
            },
            { type: "media-playout", id: "AP", totalPlayoutDelay: 4.2, totalSamplesCount: 96_000 },
        ]);
        expect(readAudioArrival(r)).toEqual({
            totalProcessingDelayS: 12.5,
            jitterBufferEmittedCount: 480_000,
            packetsReceived: 500,
            packetsDiscarded: 2,
            concealedSamples: 960,
            clockRateHz: 48_000,
        });
        expect(readMediaPlayout(r)).toEqual({ totalPlayoutDelayS: 4.2, totalSamplesCount: 96_000 });
    });

    it("映像の行を読み、行がなければ undefined、forEach が投げても undefined", () => {
        const r = report([
            { type: "inbound-rtp", kind: "video", totalProcessingDelay: 1.5, framesDecoded: 90, framesDropped: 1, freezeCount: 0, pauseCount: 2 },
        ]);
        expect(readVideoArrival(r)).toEqual({ totalProcessingDelayS: 1.5, framesDecoded: 90, framesDropped: 1, freezeCount: 0, pauseCount: 2 });
        expect(readAudioArrival(r)).toBeUndefined();
        expect(readMediaPlayout(r)).toBeUndefined();
        const broken: StatsReportLike = {
            forEach: () => {
                throw new Error("boom");
            },
        };
        expect(readVideoArrival(broken)).toBeUndefined();
        expect(readMediaPlayout(broken)).toBeUndefined();
        expect(readAudioArrival(undefined)).toBeUndefined();
    });

    it("codec の行がなければ clockRate は undefined", () => {
        const r = report([{ type: "inbound-rtp", kind: "audio", codecId: "C9", totalProcessingDelay: 1 }]);
        expect(readAudioArrival(r)?.clockRateHz).toBeUndefined();
    });
});

describe("readProcessorCount", () => {
    it("数だけを返し、ない、数でない、例外のときは undefined", () => {
        expect(readProcessorCount({ discardedFrames: 3, totalFrames: 10 }, "discardedFrames")).toBe(3);
        expect(readProcessorCount({}, "totalFrames")).toBeUndefined();
        const throwing = Object.defineProperty({}, "discardedFrames", {
            get() {
                throw new Error("boom");
            },
        });
        expect(readProcessorCount(throwing, "discardedFrames")).toBeUndefined();
    });
});

describe("映像の先行の足し上げ", () => {
    it("取り出すと空に戻り、見積もりの換算のフレームを数える", () => {
        const span = createVideoLeadSpan();
        addVideoLead(span, 10_000, true);
        addVideoLead(span, 14_000, false);
        addVideoLead(span, Number.NaN, true);
        expect(takeVideoLeadSpan(span)).toEqual({ sumUs: 24_000, count: 2, estimated: 1 });
        expect(takeVideoLeadSpan(span)).toEqual({ sumUs: 0, count: 0, estimated: 0 });
    });
});

describe("noteAudioRead: 音声の断片の飛びと読む間隔", () => {
    it("10 ms の断片が 1 つ欠けると飛びを 1 つ数え、取り出しの揺れは数えない", () => {
        const span = createAudioReadSpan();
        // 480 サンプル (10 ms) ずつ。3 つ目の後の 1 つが欠け、2 つ目は 1 ms 遅れて取り出された。
        noteAudioRead(span, 0, 480, 48_000, 0n);
        noteAudioRead(span, 11_000, 480, 48_000, 10_000n);
        noteAudioRead(span, 21_000, 480, 48_000, 20_000n);
        noteAudioRead(span, 41_000, 480, 48_000, 150_000n);
        const s = takeAudioReadSpan(span);
        expect(s.blocks).toBe(4);
        expect(s.gaps).toBe(1);
        expect(s.gapMaxUs).toBe(10_000);
        expect(s.gapMinUs).toBe(0);
        expect(s.readMaxUs).toBe(130_000);
    });

    it("境は前の断片の長さの AUDIO_GAP_FRACTION 倍", () => {
        const span = createAudioReadSpan();
        noteAudioRead(span, 0, 480, 48_000, 0n);
        noteAudioRead(span, 10_000 + 10_000 * AUDIO_GAP_FRACTION, 480, 48_000, 1n); // ちょうど境は数えない。
        expect(takeAudioReadSpan(span).gaps).toBe(0);
    });

    it("取り出すと数は 0 に戻るが、前の断片は持ち越して、期間の境をまたいだ飛びも数える", () => {
        const span = createAudioReadSpan();
        noteAudioRead(span, 0, 480, 48_000, 0n);
        takeAudioReadSpan(span);
        noteAudioRead(span, 30_000, 480, 48_000, 10_000n);
        const s = takeAudioReadSpan(span);
        expect(s).toEqual({ blocks: 1, gaps: 1, gapMaxUs: 20_000, gapMinUs: 20_000, readMaxUs: 10_000 });
        expect(takeAudioReadSpan(span)).toEqual({ blocks: 0, gaps: 0, gapMaxUs: undefined, gapMinUs: undefined, readMaxUs: undefined });
    });

    it("タイムスタンプが有限でない断片は差を数えず、次の断片からまた数える", () => {
        const span = createAudioReadSpan();
        noteAudioRead(span, 0, 480, 48_000, 0n);
        noteAudioRead(span, Number.NaN, 480, 48_000, 1n);
        noteAudioRead(span, 50_000, 480, 48_000, 2n);
        const s = takeAudioReadSpan(span);
        expect(s.blocks).toBe(3);
        expect(s.gaps).toBe(0);
        expect(s.gapMaxUs).toBeUndefined();
    });
});

describe("avDiagInterval: 1 秒ごとの区間の G", () => {
    it("G = [音声の待ち + (P - 10) / 2] - [映像の待ち + 先行] を区間の差で出す", () => {
        // 音声: 40 ms 待ち、P = 20 ms -> 45 ms。映像: 20 ms 待ち + 先行 12 ms -> 32 ms。G = +13 ms。
        const first = avDiagInterval(createAvDiagState(0), input(1));
        expect(first.skipped).toBe("reset");
        const second = avDiagInterval(first.state, input(2));
        expect(second.skipped).toBeUndefined();
        expect(second.gMs).toBeCloseTo(13, 9);
    });

    it("clockRate が読めなければ 48 kHz とする", () => {
        const s1 = avDiagInterval(createAvDiagState(0), input(1, { audio: { trackId: "a1", sample: audioAt(1, { clockRateHz: undefined }) } }));
        const s2 = avDiagInterval(s1.state, input(2, { audio: { trackId: "a1", sample: audioAt(2, { clockRateHz: undefined }) } }));
        expect(s2.gMs).toBeCloseTo(13, 9);
    });

    it("出せない区間は理由を返す", () => {
        const base = avDiagInterval(createAvDiagState(0), input(1)).state;
        expect(avDiagInterval(base, input(2, { video: undefined })).skipped).toBe("no-track");
        expect(avDiagInterval(base, input(2, { audio: { trackId: "a1", sample: undefined } })).skipped).toBe("stats");
        expect(avDiagInterval(base, input(2, { video: { trackId: "v2", sample: videoAt(2) } })).skipped).toBe("reset");
        expect(avDiagInterval(base, input(2, { audio: { trackId: "a1", sample: audioAt(0.5) } })).skipped).toBe("reset");
        expect(avDiagInterval(base, input(2, { audio: { trackId: "a1", sample: audioAt(1) } })).skipped).toBe("audio");
        expect(avDiagInterval(base, input(2, { video: { trackId: "v1", sample: videoAt(1) } })).skipped).toBe("video");
        expect(avDiagInterval(base, input(2, { lead: lead(0) })).skipped).toBe("lead");
        expect(avDiagInterval(base, input(2, { audioTimeIsFetch: false })).skipped).toBe("domain");
    });

    it("読めなかった区間の後は、同じトラックの前の値との差 (2 区間の平均) で出す", () => {
        let state = avDiagInterval(createAvDiagState(0), input(1)).state;
        state = avDiagInterval(state, input(2, { audio: { trackId: "a1", sample: undefined } })).state;
        const third = avDiagInterval(state, input(3));
        expect(third.skipped).toBeUndefined();
        expect(third.gMs).toBeCloseTo(13, 9);
    });

    it("隠したサンプルか止まりがあった区間は、乱れのない G に入れない", () => {
        let state = run(3);
        state = avDiagInterval(state, input(4, { audio: { trackId: "a1", sample: audioAt(4, { concealedSamples: 960 }) } })).state;
        state = avDiagInterval(state, input(5, { audio: { trackId: "a1", sample: audioAt(5, { concealedSamples: 960 }) }, video: { trackId: "v1", sample: videoAt(5, { freezeCount: 1 }) } })).state;
        const line = avDiagReport(state, 3, 60_000, { audio: undefined, video: undefined }).line;
        expect(line).toContain(" n=4/5 ");
        expect(line).toContain(" G.clean=2 ");
    });
});

describe("avDiagReport: 定期の行", () => {
    it("行の形と、期間の値を 0 に戻すこと", () => {
        const state = run(5);
        const extras = {
            audio: {
                readerId: "r1",
                read: { blocks: 500, gaps: 1, gapMaxUs: 10_400, gapMinUs: -300, readMaxUs: 21_000 },
                drops: { discarded: 2, total: 500 },
            },
            video: { readerId: "r2", drops: { discarded: 3, total: 150 }, replaced: 1 },
        };
        const result = avDiagReport(state, 3, 60_000, extras);
        expect(result.line).toBe(
            "av diag stream 3: span=60.0s n=4/5 G=+13.0ms G.med=+13.0ms G.min=+13.0ms G.max=+13.0ms G.cleanMed=+13.0ms G.clean=4" +
                " a.proc=40.0ms a.pkt=20.0ms v.proc=20.0ms v.lead=+12.0ms v.leadEst=0" +
                " skip.no-track=0 skip.stats=0 skip.reset=1 skip.audio=0 skip.video=0 skip.lead=0 skip.domain=0" +
                " L=30.0ms L.rows=5/5" +
                " a.blocks=500 a.gaps=1 a.gapMax=+10.4ms a.gapMin=-0.3ms a.readMax=21.0ms a.discarded=+2/2" +
                " v.discarded=+3/3 v.replaced=+1/1",
        );
        // 次の期間は、同じ読み出し口なら、その数を前の行からの差で書く。
        const next = avDiagReport(result.state, 3, 120_000, {
            audio: { ...extras.audio, drops: { discarded: 5, total: 900 } },
            video: { ...extras.video, drops: { discarded: 3, total: 300 }, replaced: 4 },
        });
        expect(next.line).toContain("span=60.0s n=0/0 G=none G.med=none");
        expect(next.line).toContain("L=none L.rows=0/0");
        expect(next.line).toContain("a.discarded=+3/5 v.discarded=+0/3 v.replaced=+3/4");
        // 読み出し口が替わったら、期間の値は通算と同じ。
        const replaced = avDiagReport(next.state, 3, 180_000, {
            audio: { ...extras.audio, readerId: "r3", drops: { discarded: 1, total: 10 } },
            video: undefined,
        });
        expect(replaced.line).toContain("a.discarded=+1/1 v.discarded=none v.replaced=none");
    });

    it("同じトラックから読み出しを始め直すと、累計が前の値以上でも期間の値は通算と同じ", () => {
        // 受信のやり直し (restartScreenVideo) は同じトラックで読み出し口を作り直し、累計は 0 から数え直す。
        const audio = { readerId: "r1", read: { blocks: 0, gaps: 0, gapMaxUs: undefined, gapMinUs: undefined, readMaxUs: undefined }, drops: { discarded: 0, total: 0 } };
        const first = avDiagReport(createAvDiagState(0), 3, 60_000, { audio, video: { readerId: "r2", drops: { discarded: 5, total: 100 }, replaced: 2 } });
        const restarted = avDiagReport(first.state, 3, 120_000, { audio, video: { readerId: "r4", drops: { discarded: 7, total: 80 }, replaced: 3 } });
        expect(restarted.line).toContain("v.discarded=+7/7 v.replaced=+3/3");
    });

    it("映像の discarded が読めない行の後も、前の行の累計から差を書く", () => {
        const audio = { readerId: "r1", read: { blocks: 0, gaps: 0, gapMaxUs: undefined, gapMinUs: undefined, readMaxUs: undefined }, drops: { discarded: 0, total: 0 } };
        const a = avDiagReport(createAvDiagState(0), 3, 60_000, { audio, video: { readerId: "r2", drops: { discarded: 5, total: 100 }, replaced: 1 } });
        const b = avDiagReport(a.state, 3, 120_000, { audio, video: { readerId: "r2", drops: { discarded: undefined, total: undefined }, replaced: 2 } });
        expect(b.line).toContain("v.discarded=none v.replaced=+1/2");
        const c = avDiagReport(b.state, 3, 180_000, { audio, video: { readerId: "r2", drops: { discarded: 9, total: 300 }, replaced: 2 } });
        expect(c.line).toContain("v.discarded=+4/9 v.replaced=+0/2");
    });

    it("期間に何も読めなければ none を書き、媒体の値がなければ none", () => {
        const line = avDiagReport(createAvDiagState(0), 7, 10_000, { audio: undefined, video: undefined }).line;
        expect(line).toBe(
            "av diag stream 7: span=10.0s n=0/0 G=none G.med=none G.min=none G.max=none G.cleanMed=none G.clean=0" +
                " a.proc=none a.pkt=none v.proc=none v.lead=none v.leadEst=0" +
                " skip.no-track=0 skip.stats=0 skip.reset=0 skip.audio=0 skip.video=0 skip.lead=0 skip.domain=0" +
                " L=none L.rows=0/0" +
                " a.blocks=none a.gaps=none a.gapMax=none a.gapMin=none a.readMax=none a.discarded=none" +
                " v.discarded=none v.replaced=none",
        );
    });

    it("G.med は区間の中央値で、G は期間の合計から出した値", () => {
        // 2 区間目だけ先行を 22 ms にする (G = +3 ms)。ほかは +13 ms。
        const state = run(5, (t) => (t === 3 ? { lead: lead(30, 22_000) } : {}));
        const line = avDiagReport(state, 1, 60_000, { audio: undefined, video: undefined }).line;
        expect(line).toContain("G=+10.5ms G.med=+13.0ms G.min=+3.0ms G.max=+13.0ms");
        expect(line).toContain("v.lead=+14.5ms");
    });
});

describe("medianOf", () => {
    it("奇数と偶数と空", () => {
        expect(medianOf([3, 1, 2])).toBe(2);
        expect(medianOf([4, 1, 3, 2])).toBe(2.5);
        expect(medianOf([])).toBeUndefined();
    });
});
