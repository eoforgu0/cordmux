// SPDX-License-Identifier: Apache-2.0

// encode/playout_diag.ts のテスト。画面共有のストリームごとに、音声と映像の再生の遅延がどれだけ離れているかを、送信側の
// ログの行にすることを確認する。期待する行の文字列は、encode/playout_diag.ts のコメントにある行の形と同じである。

import { describe, expect, it } from "vitest";

import type { StatsReportLike } from "../adapter/types";
import { AUDIO_CHANNELS, AUDIO_SAMPLES_PER_CHUNK, createAudioChunker, pushAudio } from "../audio/chunk";
import {
    createPlayoutDiagState,
    framePeriodUs,
    originMove,
    PLAYOUT_EVENT_LINES_PER_SPAN,
    PLAYOUT_FIRST_REPORT_MS,
    playoutReport,
    readInboundRtp,
    readRtpCounts,
    readStatsSafely,
    reanchorShiftUs,
    recordOriginMove,
    recordReanchor,
    shouldReportOnTick,
    type InboundRtpSample,
    type PlayoutDiagState,
} from "../encode/playout_diag";

const T0_MS = 1_700_000_000_000;

/** テスト用の getStats() の結果 (RTCStatsReport と同じく、行を forEach で順に渡す)。 */
function fakeReport(rows: readonly unknown[]): StatsReportLike {
    return {
        forEach(callback: (value: unknown) => void): void {
            for (const row of rows) callback(row);
        },
    };
}

/** 数だけを持つ統計 (readInboundRtp() を通さずに、playoutReport() をテストするため)。 */
function sample(fields: Partial<InboundRtpSample>): InboundRtpSample {
    return {
        timestampMs: undefined,
        jitterBufferDelayS: undefined,
        jitterBufferEmittedCount: undefined,
        estimatedPlayoutTimestampMs: undefined,
        framesDropped: undefined,
        totalDecodeTimeS: undefined,
        framesDecoded: undefined,
        ...fields,
    };
}

/** ストリームを作ってから 10 秒後の、1 回目の統計 (音声 40 ms、映像 120 ms、デコード 4 ms、音声の先行 80 ms)。 */
const AUDIO_1 = sample({
    timestampMs: 5_000,
    jitterBufferDelayS: 1_920,
    jitterBufferEmittedCount: 48_000,
    estimatedPlayoutTimestampMs: 900_000,
});
const VIDEO_1 = sample({
    timestampMs: 5_003,
    jitterBufferDelayS: 30,
    jitterBufferEmittedCount: 250,
    estimatedPlayoutTimestampMs: 899_923,
    framesDropped: 2,
    totalDecodeTimeS: 1,
    framesDecoded: 250,
});
/** 60 秒後の 2 回目 (区間の平均は音声 50 ms、映像 130 ms、デコード 5 ms。estimatedPlayout の先行は 50 ms)。 */
const AUDIO_2 = sample({
    timestampMs: 65_000,
    jitterBufferDelayS: 1_920 + 144_000,
    jitterBufferEmittedCount: 48_000 + 2_880_000,
    estimatedPlayoutTimestampMs: 960_000,
});
const VIDEO_2 = sample({
    timestampMs: 65_010,
    jitterBufferDelayS: 30 + 234,
    jitterBufferEmittedCount: 250 + 1_800,
    estimatedPlayoutTimestampMs: 959_960,
    framesDropped: 5,
    totalDecodeTimeS: 1 + 9,
    framesDecoded: 250 + 1_800,
});

describe("readStatsSafely: getStats() を待つ関数は例外を投げない", () => {
    /** 上限を短くして、実際のタイマーでテストする。 */
    const SHORT_TIMEOUT_MS = 20;
    const REPORT = fakeReport([{ type: "inbound-rtp", kind: "audio", timestamp: 1 }]);

    it("レシーバーがなければ undefined を返す", async () => {
        await expect(readStatsSafely(undefined, SHORT_TIMEOUT_MS)).resolves.toBeUndefined();
    });

    it("返った結果はそのまま渡す", async () => {
        await expect(readStatsSafely(() => Promise.resolve(REPORT), SHORT_TIMEOUT_MS)).resolves.toBe(REPORT);
    });

    it("同期で例外を投げたとき、reject したとき、Promise でない値を返したときは undefined を返す (reject しない)", async () => {
        const throwing = (): Promise<StatsReportLike> => {
            throw new Error("closed");
        };
        await expect(readStatsSafely(throwing, SHORT_TIMEOUT_MS)).resolves.toBeUndefined();
        await expect(readStatsSafely(() => Promise.reject(new Error("closed")), SHORT_TIMEOUT_MS)).resolves.toBeUndefined();
        const notPromise = (() => undefined) as unknown as () => Promise<StatsReportLike>;
        await expect(readStatsSafely(notPromise, SHORT_TIMEOUT_MS)).resolves.toBeUndefined();
    });

    it("上限まで返らなければ undefined で終わり、後から返った結果は捨てる", async () => {
        let late: ((report: StatsReportLike) => void) | undefined;
        const pending = new Promise<StatsReportLike>((resolve) => {
            late = resolve;
        });
        const startedAt = Date.now();
        await expect(readStatsSafely(() => pending, SHORT_TIMEOUT_MS)).resolves.toBeUndefined();
        expect(Date.now() - startedAt).toBeGreaterThanOrEqual(SHORT_TIMEOUT_MS - 5); // 上限まで待った。
        late?.(REPORT); // 遅れて返っても、決まった結果は変わらない (例外も投げない)。
        await pending;
    });

    it("返らないままでも、もう一方の読み取りを止めない (index.ts は音声と映像を Promise.all で待つ)", async () => {
        const never = new Promise<StatsReportLike>(() => undefined);
        await expect(
            Promise.all([readStatsSafely(() => never, SHORT_TIMEOUT_MS), readStatsSafely(() => Promise.resolve(REPORT), SHORT_TIMEOUT_MS)]),
        ).resolves.toEqual([undefined, REPORT]);
    });
});

describe("readInboundRtp: getStats() の結果から数だけを読む", () => {
    it("指定した種類の inbound-rtp の行を選び、ほかの行は見ない", () => {
        const report = fakeReport([
            { type: "codec", mimeType: "video/H264" },
            { type: "remote-outbound-rtp", kind: "video", timestamp: 1 },
            { type: "inbound-rtp", kind: "audio", timestamp: 2, jitterBufferDelay: 9, jitterBufferEmittedCount: 9 },
            {
                type: "inbound-rtp",
                kind: "video",
                timestamp: 3,
                jitterBufferDelay: 30,
                jitterBufferEmittedCount: 250,
                estimatedPlayoutTimestamp: 899_923,
                framesDropped: 2,
                totalDecodeTime: 1,
                framesDecoded: 250,
            },
        ]);
        expect(readInboundRtp(report, "video")).toEqual({
            timestampMs: 3,
            jitterBufferDelayS: 30,
            jitterBufferEmittedCount: 250,
            estimatedPlayoutTimestampMs: 899_923,
            framesDropped: 2,
            totalDecodeTimeS: 1,
            framesDecoded: 250,
        });
        expect(readInboundRtp(report, "audio")?.timestampMs).toBe(2);
    });

    it("種類のキーが古い形 (mediaType) の行も、種類のキーがない行も受け取る", () => {
        const legacy = fakeReport([{ type: "inbound-rtp", mediaType: "audio", timestamp: 7 }]);
        expect(readInboundRtp(legacy, "audio")?.timestampMs).toBe(7);
        expect(readInboundRtp(legacy, "video")).toBeUndefined();
        const kindless = fakeReport([{ type: "inbound-rtp", timestamp: 8 }]);
        expect(readInboundRtp(kindless, "video")?.timestampMs).toBe(8);
    });

    it("数でないキーと有限の数でないキーは undefined にする (値で埋めない)", () => {
        const report = fakeReport([
            { type: "inbound-rtp", kind: "audio", timestamp: "5000", jitterBufferDelay: Number.NaN, jitterBufferEmittedCount: Infinity },
        ]);
        expect(readInboundRtp(report, "audio")).toEqual(sample({}));
    });

    it("結果がないとき、inbound-rtp がないとき、forEach が例外を投げるときは undefined を返す (例外を外に出さない)", () => {
        expect(readInboundRtp(undefined, "audio")).toBeUndefined();
        expect(readInboundRtp(fakeReport([{ type: "codec" }, null, 3]), "audio")).toBeUndefined();
        const throwing: StatsReportLike = {
            forEach(): void {
                throw new Error("closed");
            },
        };
        expect(readInboundRtp(throwing, "audio")).toBeUndefined();
    });
});

describe("playoutReport: 定期の行", () => {
    function firstReport(): { state: PlayoutDiagState; line: string | undefined } {
        const state = createPlayoutDiagState(T0_MS);
        return playoutReport(state, 3, T0_MS + 10_000, { trackId: "a1", sample: AUDIO_1 }, { trackId: "v1", sample: VIDEO_1 });
    }

    it("1 回目は、ストリームを作ってからの秒数と、トラックの始めからの平均を書く", () => {
        expect(firstReport().line).toBe(
            "playout stream 3: span=10.0s a.jb=40.0ms a.jbAll=40.0ms v.jb=120.0ms v.jbAll=120.0ms v.dec=4.0ms" +
                " v.dropped=+2/2 lead.jb=+80.0ms lead.ept=+80.0ms reanchor=+0/0 reanchorShift=+0.0ms" +
                " originDown=+0/0 originUp=+0/0 originShift=+0.0ms eventLinesSkipped=0",
        );
    });

    it("2 回目は前の行からの区間の平均を書き、通算の比も並べる", () => {
        const second = playoutReport(
            firstReport().state,
            3,
            T0_MS + 70_000,
            { trackId: "a1", sample: AUDIO_2 },
            { trackId: "v1", sample: VIDEO_2 },
        );
        expect(second.line).toBe(
            "playout stream 3: span=60.0s a.jb=50.0ms a.jbAll=49.8ms v.jb=130.0ms v.jbAll=128.8ms v.dec=5.0ms" +
                " v.dropped=+3/5 lead.jb=+80.0ms lead.ept=+50.0ms reanchor=+0/0 reanchorShift=+0.0ms" +
                " originDown=+0/0 originUp=+0/0 originShift=+0.0ms eventLinesSkipped=0",
        );
    });

    it("lead.ept は報告の時刻を引いてから差を取る (2 回の getStats() の間の時間を混ぜない)", () => {
        // 映像の報告だけが 30 ms 遅く作られ、estimatedPlayoutTimestamp もその分だけ先に外挿されている。
        // そのまま差を取ると、音声の先行が -30 ms ずれて見えるが、報告の時刻を引けば同じ +80 ms のまま。
        const lateVideo = sample({ ...VIDEO_1, timestampMs: 5_033, estimatedPlayoutTimestampMs: 899_953 });
        const result = playoutReport(
            createPlayoutDiagState(T0_MS),
            3,
            T0_MS + 10_000,
            { trackId: "a1", sample: AUDIO_1 },
            { trackId: "v1", sample: lateVideo },
        );
        expect(result.line).toContain(" lead.ept=+80.0ms ");
    });

    it("音声が映像より後なら、lead を負の数で書く", () => {
        // 48000 サンプルで合計 7200 秒 = 1 サンプルあたり 150 ms。
        const slowAudio = sample({ ...AUDIO_1, jitterBufferDelayS: 7_200, estimatedPlayoutTimestampMs: 899_900 });
        const result = playoutReport(
            createPlayoutDiagState(T0_MS),
            3,
            T0_MS + 10_000,
            { trackId: "a1", sample: slowAudio },
            { trackId: "v1", sample: VIDEO_1 },
        );
        expect(result.line).toContain(" lead.jb=-30.0ms lead.ept=-20.0ms ");
    });

    it("トラックが替わったら前の統計を使わず、新しいトラックの始めからの平均にする", () => {
        const replaced = playoutReport(
            firstReport().state,
            3,
            T0_MS + 70_000,
            { trackId: "a2", sample: sample({ jitterBufferDelayS: 30, jitterBufferEmittedCount: 1_000 }) },
            { trackId: "v1", sample: VIDEO_2 },
        );
        expect(replaced.line).toContain(" a.jb=30.0ms a.jbAll=30.0ms v.jb=130.0ms ");
    });

    it("数が戻ったら (同じ id のまま作り直されたら)、通算の平均にする", () => {
        const reset = playoutReport(
            firstReport().state,
            3,
            T0_MS + 70_000,
            { trackId: "a1", sample: sample({ jitterBufferDelayS: 20, jitterBufferEmittedCount: 1_000 }) },
            { trackId: "v1", sample: sample({ ...VIDEO_2, framesDropped: 1 }) },
        );
        expect(reset.line).toContain(" a.jb=20.0ms a.jbAll=20.0ms ");
        expect(reset.line).toContain(" v.dropped=+1/1 ");
    });

    it("区間の中で 1 つも再生していなければ、区間の平均は none にする (0 で割らない)", () => {
        const still = playoutReport(
            firstReport().state,
            3,
            T0_MS + 70_000,
            { trackId: "a1", sample: AUDIO_1 },
            { trackId: "v1", sample: VIDEO_1 },
        );
        expect(still.line).toContain(" a.jb=none a.jbAll=40.0ms v.jb=none v.jbAll=120.0ms v.dec=none v.dropped=+0/2 lead.jb=none ");
    });

    it("読めなかったときは none を書き、前の統計を残す (次の行は 2 区間分の平均になる)", () => {
        const failed = playoutReport(
            firstReport().state,
            3,
            T0_MS + 70_000,
            { trackId: "a1", sample: undefined },
            { trackId: "v1", sample: undefined },
        );
        expect(failed.line).toBe(
            "playout stream 3: span=60.0s a.jb=none a.jbAll=none v.jb=none v.jbAll=none v.dec=none" +
                " v.dropped=none lead.jb=none lead.ept=none reanchor=+0/0 reanchorShift=+0.0ms" +
                " originDown=+0/0 originUp=+0/0 originShift=+0.0ms eventLinesSkipped=0",
        );
        const next = playoutReport(failed.state, 3, T0_MS + 130_000, { trackId: "a1", sample: AUDIO_2 }, { trackId: "v1", sample: VIDEO_2 });
        expect(next.line).toContain(" a.jb=50.0ms ");
        expect(next.line).toContain(" v.dropped=+3/5 ");
    });

    it("音声だけか映像だけのストリームでは、ないほうを none で書く", () => {
        const videoOnly = playoutReport(createPlayoutDiagState(T0_MS), 3, T0_MS + 10_000, undefined, { trackId: "v1", sample: VIDEO_1 });
        expect(videoOnly.line).toContain(" a.jb=none a.jbAll=none v.jb=120.0ms ");
        expect(videoOnly.line).toContain(" lead.jb=none lead.ept=none ");
    });

    it("行には読んだ数のほかに、統計の行の識別子 (id、ssrc、trackIdentifier など) を 1 つも書かない", () => {
        const identifiers = {
            id: "IT01V3141592653",
            ssrc: 3_141_592_653,
            trackIdentifier: "synthetic-track-identifier",
            mid: "synthetic-mid",
            codecId: "synthetic-codec-id",
            transportId: "synthetic-transport-id",
            remoteId: "synthetic-remote-id",
            playoutId: "synthetic-playout-id",
        };
        const report = fakeReport([
            { type: "inbound-rtp", kind: "audio", ...identifiers, timestamp: 5_000, jitterBufferDelay: 1_920, jitterBufferEmittedCount: 48_000 },
            { type: "inbound-rtp", kind: "video", ...identifiers, timestamp: 5_003, jitterBufferDelay: 30, jitterBufferEmittedCount: 250 },
        ]);
        const result = playoutReport(
            createPlayoutDiagState(T0_MS),
            3,
            T0_MS + 10_000,
            { trackId: "a1", sample: readInboundRtp(report, "audio") },
            { trackId: "v1", sample: readInboundRtp(report, "video") },
        );
        expect(result.line).toContain(" a.jb=40.0ms ");
        for (const value of Object.values(identifiers)) expect(result.line).not.toContain(String(value));
        // トラックの id も書かない (前の統計を見分けるキーとして持つだけ)。
        expect(result.line).not.toContain("a1");
        expect(result.line).not.toContain("v1");
    });
});

describe("shouldReportOnTick: 60 秒ごとのタイマーで行を書くか", () => {
    it("最初の行をまだ書いていなくて、10 秒たっていなければ書かない (10 秒後の 1 回が書く)", () => {
        const state = createPlayoutDiagState(T0_MS);
        expect(shouldReportOnTick(state, T0_MS + PLAYOUT_FIRST_REPORT_MS - 1)).toBe(false);
        expect(shouldReportOnTick(state, T0_MS + PLAYOUT_FIRST_REPORT_MS)).toBe(true);
    });

    it("一度書いたストリームでは、いつでも書く", () => {
        const reported = playoutReport(createPlayoutDiagState(T0_MS), 1, T0_MS + 1, undefined, undefined).state;
        expect(shouldReportOnTick(reported, T0_MS + 2)).toBe(true);
    });
});

describe("originMove: 映像の原点の動き", () => {
    it("1 フレームの間隔は fps から求め、数でないときと 0 以下のときは求めない (割り算で例外を投げない)", () => {
        expect(framePeriodUs(30)).toBe(33_333n);
        expect(framePeriodUs(60)).toBe(16_667n);
        expect(framePeriodUs(undefined)).toBeUndefined();
        expect(framePeriodUs(0)).toBeUndefined();
        expect(framePeriodUs(-30)).toBeUndefined();
        expect(framePeriodUs(Number.NaN)).toBeUndefined();
        expect(framePeriodUs(Infinity)).toBeUndefined();
    });

    it("動いていないとき、前の値がないとき、fps が分からないときは数えない", () => {
        expect(originMove(1_000_000n, 1_000_000n, 30)).toBeUndefined();
        expect(originMove(undefined, 1_000_000n, 30)).toBeUndefined();
        expect(originMove(1_000_000n, undefined, 30)).toBeUndefined();
        expect(originMove(1_000_000n, 800_000n, undefined)).toBeUndefined();
        expect(originMove(1_000_000n, 800_000n, 0)).toBeUndefined();
    });

    it("ちょうど 1 フレームの間隔までは数えず、それより大きい動きだけを数える", () => {
        expect(originMove(1_000_000n, 1_000_000n - 33_333n, 30)).toBeUndefined();
        expect(originMove(1_000_000n, 1_000_000n - 33_334n, 30)).toEqual({ deltaUs: -33_334n, framePeriodUs: 33_333n });
    });

    it("上がる向きも数える", () => {
        expect(originMove(1_000_000n, 1_120_400n, 30)).toEqual({ deltaUs: 120_400n, framePeriodUs: 33_333n });
    });
});

describe("イベントの行と数", () => {
    it("原点が下がった回数と上がった回数を向きごとに数え、量の合計を定期の行に書く", () => {
        let state = createPlayoutDiagState(T0_MS);
        const down = recordOriginMove(state, 3, { deltaUs: -120_400n, framePeriodUs: 33_333n });
        expect(down.line).toBe("playout event stream 3: video origin -120.4ms (frame 33.3ms)");
        state = down.state;
        const up = recordOriginMove(state, 3, { deltaUs: 118_000n, framePeriodUs: 33_333n });
        expect(up.line).toBe("playout event stream 3: video origin +118.0ms (frame 33.3ms)");
        state = up.state;
        const report = playoutReport(state, 3, T0_MS + 10_000, undefined, undefined);
        expect(report.line).toContain(" originDown=+1/1 originUp=+1/1 originShift=-2.4ms ");
        // 区間の数は 0 に戻り、通算の数は残る。
        const next = playoutReport(report.state, 3, T0_MS + 70_000, undefined, undefined);
        expect(next.line).toContain(" originDown=+0/1 originUp=+0/1 originShift=+0.0ms ");
    });

    it("音声の換算の定数の決め直しを数え、量の合計と通算の数を書く", () => {
        const first = recordReanchor(createPlayoutDiagState(T0_MS), 3, 45_200n);
        expect(first.line).toBe("playout event stream 3: audio reanchor +45.2ms (total 1)");
        const second = recordReanchor(first.state, 3, -41_000n);
        expect(second.line).toBe("playout event stream 3: audio reanchor -41.0ms (total 2)");
        const report = playoutReport(second.state, 3, T0_MS + 10_000, undefined, undefined);
        expect(report.line).toContain(" reanchor=+2/2 reanchorShift=+4.2ms ");
    });

    it("イベントの行は、1 区間にストリームごとに 3 行までで、超えた分は数だけを定期の行に書き、次の区間でまた書く", () => {
        let state = createPlayoutDiagState(T0_MS);
        const lines: Array<string | undefined> = [];
        for (let i = 0; i < PLAYOUT_EVENT_LINES_PER_SPAN + 2; i++) {
            const result = recordReanchor(state, 3, 40_000n);
            state = result.state;
            lines.push(result.line);
        }
        expect(lines.filter((line) => line !== undefined)).toHaveLength(PLAYOUT_EVENT_LINES_PER_SPAN);
        // 原点の動きも、同じ上限で数える (上限はストリームごとに 1 つ)。
        const origin = recordOriginMove(state, 3, { deltaUs: -50_000n, framePeriodUs: 33_333n });
        expect(origin.line).toBeUndefined();
        state = origin.state;
        const report = playoutReport(state, 3, T0_MS + 10_000, undefined, undefined);
        expect(report.line).toContain(" reanchor=+5/5 reanchorShift=+200.0ms originDown=+1/1 ");
        expect(report.line).toMatch(/ eventLinesSkipped=3$/);
        expect(recordReanchor(report.state, 3, 40_000n).line).toBe("playout event stream 3: audio reanchor +40.0ms (total 6)");
    });
});

describe("reanchorShiftUs: 決め直しの量は、前と後の定数の差", () => {
    const FRAME_SAMPLES = AUDIO_SAMPLES_PER_CHUNK * AUDIO_CHANNELS;
    const MEDIA_EPOCH_US = 2_000_000;
    const WALL_EPOCH_US = 1_700_000_000_000_000n;

    it("メディアのタイムスタンプが 500 ms 戻ったまま到着の時刻で 1 秒続いて定数を決め直したら、+500 ms を返す", () => {
        let chunker = createAudioChunker();
        // 定数を決める期間 (2 秒) を、遅れのない断片で終える。期間の中で定数が動いても数えない。
        let i = 0;
        for (; i < 250; i++) {
            const result = pushAudio(
                chunker,
                new Float32Array(FRAME_SAMPLES),
                MEDIA_EPOCH_US + i * 10_000,
                WALL_EPOCH_US + BigInt(i) * 10_000n,
            );
            expect(reanchorShiftUs(chunker, result.state)).toBeUndefined();
            chunker = result.state;
        }
        let shift: bigint | undefined;
        for (; i < 500 && shift === undefined; i++) {
            const before = chunker;
            chunker = pushAudio(
                chunker,
                new Float32Array(FRAME_SAMPLES),
                MEDIA_EPOCH_US + i * 10_000 - 500_000,
                WALL_EPOCH_US + BigInt(i) * 10_000n,
            ).state;
            shift = reanchorShiftUs(before, chunker);
        }
        expect(shift).toBe(500_000n);
        expect(chunker.clockMap.restarts).toBe(1);
    });

    it("最初の断片 (定数がまだない) は、決め直しに数えない", () => {
        const before = createAudioChunker();
        const after = pushAudio(before, new Float32Array(FRAME_SAMPLES), MEDIA_EPOCH_US, WALL_EPOCH_US).state;
        expect(reanchorShiftUs(before, after)).toBeUndefined();
    });
});

describe("readRtpCounts: getStats() の結果から受信の累計の数を読む", () => {
    it("指定した種類の inbound-rtp の packetsReceived と framesReceived を読み、ない数は undefined にする", () => {
        const report = {
            forEach(callback: (value: unknown) => void): void {
                callback({ type: "inbound-rtp", kind: "audio", packetsReceived: 1 });
                callback({ type: "inbound-rtp", kind: "video", packetsReceived: 900, framesReceived: 60 });
            },
        };
        expect(readRtpCounts(report, "video")).toEqual({ packetsReceived: 900, framesReceived: 60 });
        expect(readRtpCounts(report, "audio")).toEqual({ packetsReceived: 1, framesReceived: undefined });
        expect(readRtpCounts(undefined, "video")).toBeUndefined();
    });
});
