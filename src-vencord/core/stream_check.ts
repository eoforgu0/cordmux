// SPDX-License-Identifier: Apache-2.0

// 映像が届かない画面共有のストリームについて、読み出しとエンコードを始め直すかを決める純粋関数。受信側が
// stream_check で頼んだとき (index.ts の handleStreamCheck) と、送信側が自分で映像を送れていないと気付いたとき
// (core/video_watch.ts) に使う。Vencord のものは import しないので、vitest でテストできる。
//
// 画面が動かない共有では、Discord がフレームを送らないことがある。受信側から見ると、映像が届かないことだけでは、
// 「Discord が送っていない」と「cordmux の読み出しが止まった」を区別できない。そこで、始め直すかは送信側の証拠で
// 決める。証拠は 2 つある。
//   (1) そのトラックのレシーバーの getStats() の inbound-rtp の packetsReceived か framesReceived が増えているのに、
//       cordmux が送れたフレームがない。Discord の側には映像が届いているので、cordmux の読み出しかエンコードが
//       止まっている。始め直す。packetsReceived と framesReceived は WebRTC Statistics (webrtc-stats) の
//       RTCInboundRtpStreamStats が持つ累計の数である。
//   (2) 同じ人の、より新しい接続の映像が動いている。Discord が Go Live の接続を作り直し、古い接続のトラックに ended が
//       届かないまま残っている。古いストリームを止める (新しい接続の映像は、別のストリームとして既に送っている)。
// RTP が届いていない (数が増えない) ときは始め直さない。Discord が送っていないので、始め直しても何も得られない。
// track.muted は 2 つ目の手がかりで、getStats() が読めないときにだけ使う (adapter/types.ts の track_mute)。

import type { StreamCheckOutcome } from "../transport/control";

/** getStats() の inbound-rtp から読む、受信の累計の数。読めない数は undefined。 */
export interface RtpCounts {
    readonly packetsReceived: number | undefined;
    readonly framesReceived: number | undefined;
}

/** 確認の間に集めた証拠。 */
export interface StreamCheckEvidence {
    /** トラックの readyState が "ended" だった。 */
    readonly trackEnded: boolean;
    /** 同じ人の、より新しい接続の映像が動いている (そのストリームの stream_id。なければ undefined)。 */
    readonly newerStreamId: number | undefined;
    /** 確認を始めたときと終えたときの、このストリームで送れたフレームの累計。 */
    readonly sentBefore: number;
    readonly sentAfter: number;
    /** 確認を始めたときと終えたときの受信の数。getStats() がない、読めないときは undefined。 */
    readonly rtpBefore: RtpCounts | undefined;
    readonly rtpAfter: RtpCounts | undefined;
    /** 確認を終えたときの track.muted。読めなければ undefined。 */
    readonly muted: boolean | undefined;
}

export interface StreamCheckDecision {
    readonly outcome: StreamCheckOutcome;
    /** ログと stream_check_result の detail に入れる短い説明 (key=value の並び。userId は入れない)。 */
    readonly detail: string;
}

/**
 * 2 回の受信の数から、増えた数を返す。両方の回で framesReceived を読めればその増え方を、読めなければ packetsReceived の
 * 増え方を返す。framesReceived を先に使うのは、映像のフレームとして受け取った数だけを数えるためである。packetsReceived
 * には、フレームを運ばないパケット (帯域の推定のための詰め物など) も入りうるので、静止した画面の共有を誤って
 * 「届いている」とみなすおそれがある。どちらも比べられなければ undefined。数が減ったとき (別のレシーバーの数を読んだ
 * など) は 0 とする。
 */
export function rtpGrowth(before: RtpCounts | undefined, after: RtpCounts | undefined): number | undefined {
    if (before === undefined || after === undefined) return undefined;
    if (before.framesReceived !== undefined && after.framesReceived !== undefined) {
        return Math.max(0, after.framesReceived - before.framesReceived);
    }
    if (before.packetsReceived !== undefined && after.packetsReceived !== undefined) {
        return Math.max(0, after.packetsReceived - before.packetsReceived);
    }
    return undefined;
}

/** 証拠から、始め直すかと、その説明を決める。 */
export function decideStreamCheck(evidence: StreamCheckEvidence): StreamCheckDecision {
    const sent = Math.max(0, evidence.sentAfter - evidence.sentBefore);
    const growth = rtpGrowth(evidence.rtpBefore, evidence.rtpAfter);
    const muted = evidence.muted === undefined ? "unknown" : String(evidence.muted);
    const rtp = growth === undefined ? "unknown" : `+${growth}`;
    const detail = `sent=+${sent} rtp=${rtp} muted=${muted}`;
    if (evidence.trackEnded) return { outcome: "ended", detail };
    if (evidence.newerStreamId !== undefined) {
        return { outcome: "superseded", detail: `${detail} newer_stream=${evidence.newerStreamId}` };
    }
    if (sent > 0) return { outcome: "sending", detail };
    if (growth !== undefined) return { outcome: growth > 0 ? "restarted" : "not_receiving", detail };
    // getStats() が読めない。muted が true なら Discord から届いていないとみなし、それ以外は始め直す (止まった読み出しを
    // 放置するより、始め直す手間のほうが小さい)。
    return { outcome: evidence.muted === true ? "not_receiving" : "restarted", detail: `${detail} stats=unavailable` };
}

/** 新しい接続かを比べるための、画面共有のストリーム 1 つの要約。 */
export interface ScreenStreamSummary {
    readonly pcId: number;
    readonly streamId: number;
    readonly userId: string;
    /** 映像のトラックがあり、readyState が "live" か。 */
    readonly videoLive: boolean;
}

/**
 * target と同じ人の、より新しい接続 (pcId が大きい) で映像が生きているストリームの stream_id を返す。なければ undefined。
 * pcId は adapter/connections.ts が Go Live の接続ごとに増やすだけで使い回さないので、大きい pcId ほど新しい接続である。
 * 複数あれば最も新しい接続のものを返す。
 */
export function newerScreenStreamOf(
    target: ScreenStreamSummary,
    streams: readonly ScreenStreamSummary[],
): number | undefined {
    let best: ScreenStreamSummary | undefined;
    for (const s of streams) {
        if (s.userId !== target.userId || s.pcId <= target.pcId || !s.videoLive) continue;
        if (best === undefined || s.pcId > best.pcId) best = s;
    }
    return best?.streamId;
}
