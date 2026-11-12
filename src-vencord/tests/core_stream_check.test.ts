// SPDX-License-Identifier: Apache-2.0

// core/stream_check.ts のテスト。受信側の頼み (stream_check) で、読み出しとエンコードを始め直すかを、送信側の証拠
// (getStats() の受信の数、送れたフレーム、トラックの状態、同じ人の新しい接続) で決めることを確認する。

import { describe, expect, it } from "vitest";

import {
    decideStreamCheck,
    newerScreenStreamOf,
    rtpGrowth,
    type ScreenStreamSummary,
    type StreamCheckEvidence,
} from "../core/stream_check";

const base: StreamCheckEvidence = {
    trackEnded: false,
    newerStreamId: undefined,
    sentBefore: 100,
    sentAfter: 100,
    rtpBefore: { packetsReceived: 5000, framesReceived: 300 },
    rtpAfter: { packetsReceived: 5000, framesReceived: 300 },
    muted: false,
};

describe("rtpGrowth", () => {
    it("両方の回で framesReceived を読めればその増え方を、読めなければ packetsReceived の増え方を返す", () => {
        expect(rtpGrowth({ packetsReceived: 10, framesReceived: 1 }, { packetsReceived: 40, framesReceived: 3 })).toBe(2);
        // フレームが増えずにパケットだけが増えた (フレームを運ばないパケット) ときは、届いていないとみなす。
        expect(rtpGrowth({ packetsReceived: 10, framesReceived: 5 }, { packetsReceived: 40, framesReceived: 5 })).toBe(0);
        expect(rtpGrowth({ packetsReceived: 10, framesReceived: undefined }, { packetsReceived: 40, framesReceived: 4 })).toBe(30);
    });

    it("比べられなければ undefined、減ったら 0 を返す", () => {
        expect(rtpGrowth(undefined, base.rtpAfter)).toBeUndefined();
        expect(
            rtpGrowth({ packetsReceived: undefined, framesReceived: undefined }, { packetsReceived: 1, framesReceived: 1 }),
        ).toBeUndefined();
        expect(rtpGrowth({ packetsReceived: 50, framesReceived: undefined }, { packetsReceived: 10, framesReceived: 2 })).toBe(0);
    });
});

describe("decideStreamCheck", () => {
    it("RTP が届いているのに送れたフレームがなければ、始め直す", () => {
        const d = decideStreamCheck({ ...base, rtpAfter: { packetsReceived: 5400, framesReceived: 330 } });
        expect(d.outcome).toBe("restarted");
        expect(d.detail).toBe("sent=+0 rtp=+30 muted=false");
    });

    it("RTP が届いていなければ (Discord が送っていない、静止した画面の共有など)、始め直さない", () => {
        expect(decideStreamCheck(base).outcome).toBe("not_receiving");
        expect(decideStreamCheck({ ...base, muted: true }).outcome).toBe("not_receiving");
    });

    it("確認の間もフレームを送れていれば、始め直さない", () => {
        const d = decideStreamCheck({ ...base, sentAfter: 130, rtpAfter: { packetsReceived: 5400, framesReceived: 330 } });
        expect(d.outcome).toBe("sending");
    });

    it("同じ人の新しい接続の映像があれば、古いストリームを止める (RTP の数より先に見る)", () => {
        const d = decideStreamCheck({ ...base, newerStreamId: 9, rtpAfter: { packetsReceived: 5400, framesReceived: 330 } });
        expect(d.outcome).toBe("superseded");
        expect(d.detail).toContain("newer_stream=9");
    });

    it("トラックが終わっていれば ended (何よりも先に見る)", () => {
        expect(decideStreamCheck({ ...base, trackEnded: true, newerStreamId: 9 }).outcome).toBe("ended");
    });

    it("getStats() が読めないときは muted を手がかりにし、muted でなければ始め直す", () => {
        const noStats = { ...base, rtpBefore: undefined, rtpAfter: undefined };
        expect(decideStreamCheck({ ...noStats, muted: true }).outcome).toBe("not_receiving");
        expect(decideStreamCheck({ ...noStats, muted: false }).outcome).toBe("restarted");
        expect(decideStreamCheck({ ...noStats, muted: undefined }).outcome).toBe("restarted");
        expect(decideStreamCheck({ ...noStats, muted: undefined }).detail).toBe(
            "sent=+0 rtp=unknown muted=unknown stats=unavailable",
        );
    });
});

describe("newerScreenStreamOf", () => {
    const old: ScreenStreamSummary = { pcId: 3, streamId: 4, userId: "u1", videoLive: true };

    it("同じ人で pcId が大きく、映像が生きているストリームを返す (複数なら最も新しい接続)", () => {
        const streams: ScreenStreamSummary[] = [
            old,
            { pcId: 5, streamId: 8, userId: "u1", videoLive: true },
            { pcId: 7, streamId: 11, userId: "u1", videoLive: true },
        ];
        expect(newerScreenStreamOf(old, streams)).toBe(11);
    });

    it("別の人、古い接続、映像が終わった接続は数えない", () => {
        const streams: ScreenStreamSummary[] = [
            old,
            { pcId: 5, streamId: 8, userId: "u2", videoLive: true },
            { pcId: 2, streamId: 1, userId: "u1", videoLive: true },
            { pcId: 6, streamId: 9, userId: "u1", videoLive: false },
        ];
        expect(newerScreenStreamOf(old, streams)).toBeUndefined();
    });
});
