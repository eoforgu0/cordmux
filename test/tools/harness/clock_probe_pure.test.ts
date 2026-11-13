/*
 * clock_probe_pure.ts の計算をテストする。
 * - summarize / deltas / histogram: 分布の要約。
 * - RtpUnwrapper: 32 ビットの巡回をまたいでも、RTP のタイムスタンプが続いた値になる。
 * - pairByRtp / originCandidates / estimateOrigin: 2 か所で読んだ同じフレームを組にし、ページの原点の TimeTicks の値を出す。
 * - originFromBrackets: process.hrtime をはさんだ組から、ページの原点の範囲を出す。
 * - tickProfile: performance.now() の刻み。
 * - timestampDomain / classifyVideoTimestamps: 時刻の値の基準と、VideoFrame.timestamp の場合。
 * - renderDelayResidual: render_time の列の、取り込みの間隔からの残り。
 * - videoRtcpAttributes / withoutNonSenderRtt: SDP の映像の RTCP の指定と、送らない側の往復の時間の仕組みを除く書き換え。
 */
import { describe, expect, it } from "vitest";

import {
    RtpUnwrapper,
    classifyVideoTimestamps,
    deltas,
    estimateOrigin,
    histogram,
    lagSeriesUs,
    originCandidates,
    originFromBrackets,
    pairByRtp,
    renderDelayResidual,
    summarize,
    tickProfile,
    timestampDomain,
    videoRtcpAttributes,
    withoutNonSenderRtt,
    type CallbackFrame,
    type ProcessorFrame,
} from "./clock_probe_pure";

describe("summarize", () => {
    it("値がなければ null", () => {
        expect(summarize([])).toBeNull();
    });

    it("最近順位法でパーセンタイルを取る", () => {
        const values = Array.from({ length: 100 }, (_, i) => 100 - i); // 100..1 を逆順で渡す
        const s = summarize(values);
        expect(s).toEqual({ count: 100, min: 1, p1: 1, p5: 5, p50: 50, p95: 95, p99: 99, max: 100, mean: 50.5 });
    });

    it("値が 1 つなら、どのパーセンタイルもその値", () => {
        const s = summarize([7]);
        expect(s?.p1).toBe(7);
        expect(s?.p99).toBe(7);
    });
});

describe("deltas と histogram", () => {
    it("隣り合う値の差", () => {
        expect(deltas([1, 4, 4, 2])).toEqual([3, 0, -2]);
        expect(deltas([5])).toEqual([]);
    });

    it("区間の下端の小さい順に数える。負の値は下の区間に入る", () => {
        expect(histogram([0, 99, 100, 250, -1], 100)).toEqual([
            [-100, 1],
            [0, 2],
            [100, 1],
            [200, 1],
        ]);
    });
});

describe("RtpUnwrapper", () => {
    it("巡回をまたぐと 2^32 を足す", () => {
        const u = new RtpUnwrapper();
        expect(u.unwrap(4294967000)).toBe(4294967000);
        expect(u.unwrap(1200)).toBe(4294967296 + 1200);
        expect(u.unwrap(2700)).toBe(4294967296 + 2700);
    });

    it("巡回の直後に前の周の値が来たら、前の周として読む", () => {
        const u = new RtpUnwrapper();
        u.unwrap(4294967000);
        u.unwrap(1200);
        expect(u.unwrap(4294967100)).toBe(4294967100);
    });

    it("少し戻った値は戻ったまま読む (1 周先にしない)", () => {
        const u = new RtpUnwrapper();
        u.unwrap(9000);
        expect(u.unwrap(6000)).toBe(6000);
    });
});

describe("pairByRtp と originCandidates と estimateOrigin", () => {
    // ページの原点の TimeTicks の値を 250_000_000_000 us とし、取り込みの時刻を 1 フレームずつ作る。
    const origin = 250_000_000_000;
    const processor: ProcessorFrame[] = [
        { readMs: 1000.5, timestampUs: origin + 980_000, rtp: 100 },
        { readMs: 1017.2, timestampUs: origin + 996_700, rtp: 1600 },
        { readMs: 1033.9, timestampUs: origin + 1_013_400, rtp: null },
        { readMs: 1050.0, timestampUs: origin + 1_030_100, rtp: 4600 },
    ];
    const callbacks: CallbackFrame[] = [
        { rtp: 1600, captureTimeMs: 996.7, receiveTimeMs: 1010 },
        { rtp: 1600, captureTimeMs: 999.9, receiveTimeMs: 1011 },
        { rtp: 100, captureTimeMs: 980.1, receiveTimeMs: 995 },
        { rtp: 4600, captureTimeMs: null, receiveTimeMs: 1040 },
        { rtp: null, captureTimeMs: 1013.4, receiveTimeMs: 1030 },
    ];

    it("RTP のタイムスタンプが同じフレームだけを組にし、重複は最初のコールバックを使う", () => {
        const pairs = pairByRtp(processor, callbacks);
        expect(pairs.map((p) => p.processor.rtp)).toEqual([100, 1600, 4600]);
        expect(pairs[1]?.callback.captureTimeMs).toBe(996.7);
    });

    it("captureTime のある組だけから候補を出す。小数のミリ秒は四捨五入してマイクロ秒にする", () => {
        const candidates = originCandidates(pairByRtp(processor, callbacks));
        expect(candidates).toEqual([origin - 100, origin]);
    });

    it("候補の中央値と幅と、前後の 1 割の差を返す", () => {
        const candidates = [origin, origin + 100, origin - 100, origin, origin + 50, origin, origin, origin, origin, origin + 100];
        const e = estimateOrigin(candidates);
        expect(e?.originUs).toBe(origin);
        expect(e?.spreadUs).toBe(200);
        expect(e?.count).toBe(10);
        expect(e?.driftUs).toBe(100);
    });

    it("候補がなければ null", () => {
        expect(estimateOrigin([])).toBeNull();
    });
});

describe("originFromBrackets", () => {
    it("組ごとの範囲の共通部分と、往復の最も短い組の中央を返す", () => {
        // 原点 = 250_000_000_000 us。hrtime = 原点 + 読んだ時刻。
        const o = originFromBrackets([
            { beforeMs: 100.0, hrtimeNs: String((250_000_000_000 + 100_200) * 1000), afterMs: 100.5 },
            { beforeMs: 200.0, hrtimeNs: String((250_000_000_000 + 200_100) * 1000), afterMs: 200.3 },
        ]);
        expect(o?.loUs).toBe(250_000_000_000 - 200);
        expect(o?.hiUs).toBe(250_000_000_000 + 100);
        expect(o?.tightestRttUs).toBe(300);
        expect(o?.tightestMidUs).toBe(250_000_000_000 - 50);
        expect(o?.count).toBe(2);
    });

    it("ナノ秒の端数は切り捨ててマイクロ秒にする", () => {
        const o = originFromBrackets([{ beforeMs: 0, hrtimeNs: "1999", afterMs: 0 }]);
        expect(o?.loUs).toBe(1);
        expect(o?.hiUs).toBe(1);
    });

    it("組がなければ null", () => {
        expect(originFromBrackets([])).toBeNull();
    });
});

describe("tickProfile", () => {
    it("0.1 ms の刻みなら、1 ms あたり約 10 の異なる値", () => {
        const reads: number[] = [];
        for (let i = 0; i <= 100; i++) reads.push(5000 + i * 0.1, 5000 + i * 0.1);
        const p = tickProfile(reads);
        expect(p?.distinct).toBe(101);
        expect(p?.distinctPerMs).toBeCloseTo(10.1, 5);
        expect(p?.stepUs?.p50).toBe(100);
        expect(p?.offGrid100us).toBe(0);
    });

    it("1 ms の刻みなら、1 ms あたり約 1 の異なる値", () => {
        const reads = [10, 10, 10, 11, 11, 12, 13];
        const p = tickProfile(reads);
        expect(p?.distinct).toBe(4);
        expect(p?.distinctPerMs).toBeCloseTo(4 / 3, 5);
        expect(p?.stepUs?.min).toBe(1000);
    });

    it("0.1 ms の格子に乗らない値を数える", () => {
        expect(tickProfile([1.0, 1.05, 1.1])?.offGrid100us).toBe(1);
    });
});

describe("timestampDomain と classifyVideoTimestamps", () => {
    const origin = 250_000_000_000;

    it("読んだ時刻に近ければページの原点から、原点を足して近ければ絶対値", () => {
        expect(timestampDomain(10_000_000, 10_002.3, null)).toBe("page-relative");
        expect(timestampDomain(origin + 10_050_000, 10_000, origin)).toBe("absolute");
        expect(timestampDomain(origin + 10_050_000, 10_000, null)).toBe("unknown");
    });

    it("captureTime がなく、絶対値で、読んだ時刻より先なら render-time で、符号が合う", () => {
        const frames: ProcessorFrame[] = [
            { readMs: 5000, timestampUs: origin + 5_012_000, rtp: 1 },
            { readMs: 5016.7, timestampUs: origin + 5_028_500, rtp: 2 },
        ];
        const c = classifyVideoTimestamps(frames, false, origin);
        expect(c.kind).toBe("render-time");
        expect(c.domain).toBe("absolute");
        expect(c.signAgrees).toBe(true);
    });

    it("captureTime があり、読んだ時刻より前なら capture-time で、符号が合う", () => {
        const frames: ProcessorFrame[] = [{ readMs: 5000, timestampUs: origin + 4_960_000, rtp: 1 }];
        const c = classifyVideoTimestamps(frames, true, origin);
        expect(c.kind).toBe("capture-time");
        expect(c.leadMedianUs).toBe(-40_000);
        expect(c.signAgrees).toBe(true);
    });

    it("render-time なのに読んだ時刻より前なら、符号が合わない", () => {
        const frames: ProcessorFrame[] = [{ readMs: 5000, timestampUs: origin + 4_990_000, rtp: 1 }];
        expect(classifyVideoTimestamps(frames, false, origin).signAgrees).toBe(false);
    });

    it("0 に近い値から始まれば elapsed", () => {
        const frames: ProcessorFrame[] = [{ readMs: 5000, timestampUs: 0, rtp: 1 }];
        expect(classifyVideoTimestamps(frames, false, origin).kind).toBe("elapsed");
    });

    it("原点が分からなければ、絶対値かを決めず unknown", () => {
        const frames: ProcessorFrame[] = [{ readMs: 5000, timestampUs: origin + 5_012_000, rtp: 1 }];
        const c = classifyVideoTimestamps(frames, false, null);
        expect(c.kind).toBe("unknown");
        expect(c.leadMedianUs).toBeNull();
    });

    it("フレームがなければ unknown", () => {
        expect(classifyVideoTimestamps([], false, origin).kind).toBe("unknown");
    });
});

describe("renderDelayResidual", () => {
    it("取り込みの間隔どおりなら残りは 0 で、遅延が下がった所を戻りとして数える", () => {
        // 60 fps (RTP で 1500 刻み)。4 つ目で遅延が 8 ms 下がる。
        const base = 250_000_000_000;
        const frames: ProcessorFrame[] = [
            { readMs: 0, timestampUs: base, rtp: 1000 },
            { readMs: 0, timestampUs: base + 16_667, rtp: 2500 },
            { readMs: 0, timestampUs: base + 33_333, rtp: 4000 },
            { readMs: 0, timestampUs: base + 42_000, rtp: 5500 },
            { readMs: 0, timestampUs: base + 58_667, rtp: 7000 },
        ];
        const r = renderDelayResidual(frames, 1000);
        expect(r.residualUs).toEqual([0, 0, 0, -8000, -8000]);
        expect(r.jumps).toBe(1);
        expect(r.negativeJumps).toBe(1);
        expect(r.largestDrops[0]).toEqual([3, -8000]);
        expect(r.timestampRegressions).toBe(0);
    });

    it("前のフレームより戻った timestamp を数え、RTP のないフレームは残りに入れない", () => {
        const frames: ProcessorFrame[] = [
            { readMs: 0, timestampUs: 1_000_000, rtp: 0 },
            { readMs: 0, timestampUs: 990_000, rtp: null },
            { readMs: 0, timestampUs: 1_016_667, rtp: 1500 },
        ];
        const r = renderDelayResidual(frames, 1000);
        expect(r.residualUs).toEqual([0, 0]);
        expect(r.timestampRegressions).toBe(1);
    });
});

describe("videoRtcpAttributes と withoutNonSenderRtt", () => {
    const sdp = [
        "v=0",
        "m=audio 9 UDP/TLS/RTP/SAVPF 111",
        "a=rtcp-fb:111 transport-cc",
        "a=rtcp-xr:rcvr-rtt=all",
        "m=video 9 UDP/TLS/RTP/SAVPF 96 97",
        "a=rtcp-xr:rcvr-rtt=all",
        "a=rtcp-fb:96 nack pli",
        "a=rtcp-fb:96 rrtr",
        "a=rtcp-fb:97 rrtr",
        "a=rtcp-fb:96 transport-cc",
        "",
    ].join("\r\n");

    it("映像の m 行の rtcp-fb の種類と rtcp-xr を、重複を除いて並べる", () => {
        expect(videoRtcpAttributes(sdp)).toEqual(["nack pli", "rrtr", "rtcp-xr:rcvr-rtt=all", "transport-cc"]);
    });

    it("rcvr-rtt と rrtr の行だけを、音声と映像の両方から除き、CRLF の区切りを保つ", () => {
        const out = withoutNonSenderRtt(sdp);
        expect(out).not.toContain("rrtr");
        expect(out).not.toContain("rcvr-rtt");
        expect(out).toContain("a=rtcp-fb:96 nack pli\r\na=rtcp-fb:96 transport-cc\r\n");
        expect(out).toContain("a=rtcp-fb:111 transport-cc\r\nm=video");
        expect(videoRtcpAttributes(out)).toEqual(["nack pli", "transport-cc"]);
    });
});

describe("lagSeriesUs", () => {
    it("読んだ時刻 - タイムスタンプ", () => {
        expect(lagSeriesUs([{ readMs: 10.5, timestampUs: 9_000 }])).toEqual([1_500]);
    });
});
