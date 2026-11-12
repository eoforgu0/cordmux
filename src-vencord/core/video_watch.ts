// SPDX-License-Identifier: Apache-2.0

// 送信側が自分で、読み出しているトラックの映像を送れていないことに気付くための純粋関数 (watchVideo)。index.ts は
// 確立している間、1 秒ごとに画面共有の映像のストリームごとに呼び、届いた出来事 (止まった、戻った) をログに書く。
// 止まったときは、トラックの状態、接続の状態、同じ人の別の接続の映像があるか、RTP が届いているかをログに書き、
// 別の接続の生きている映像があれば、そちらに切り替える (index.ts の handleVideoStall)。受信側の頼み
// (stream_check) とは別に動く。受信側が枠に載せていない人の映像も、送信側では読み出していることがあるためである。
// Vencord のものは import しないので、vitest でテストできる。

/**
 * 映像を送れないまま、この時間 (ms) がたったら止まったとみなす。受信側が受信のやり直しを頼むまでの時間の既定
 * (src-obs/config/settings.h の kReceiveRestartWaitDefaultMs。映像が止まったとみなす 2 秒の後、さらに 3 秒) と
 * 同じ考えで 5 秒にした。画面が動かない共有では、Discord がフレームを送らないことがあるので、止まったとみなしても
 * ログに書いて証拠を集めるだけで、同じトラックを始め直すことはしない (始め直すかは受信側の頼みと証拠で決める)。
 */
export const VIDEO_WATCH_STALL_MS = 5000;

/** ストリームごとの状態。 */
export interface VideoWatchState {
    /** 止まったと伝えたときの、最後に送れた時刻 (なければセッションを始めた時刻)。伝えていなければ undefined。 */
    readonly stalledFromMs: number | undefined;
}

export const VIDEO_WATCH_INITIAL: VideoWatchState = { stalledFromMs: undefined };

export type VideoWatchEvent =
    | { readonly type: "stalled"; readonly silentMs: number }
    | { readonly type: "resumed"; readonly stalledMs: number };

export interface VideoWatchResult {
    readonly state: VideoWatchState;
    readonly event: VideoWatchEvent | undefined;
}

/**
 * 1 回分を進める。lastSentMs はこのストリームで最後にフレームを送れた時刻 (まだなければ undefined)、startedMs は
 * 映像のセッションを始めた時刻である。送れないまま thresholdMs がたったら、止まった出来事を 1 回だけ返す。その後に
 * 送れたら、戻った出来事を返して状態を戻す。
 */
export function watchVideo(
    prev: VideoWatchState,
    nowMs: number,
    lastSentMs: number | undefined,
    startedMs: number,
    thresholdMs: number = VIDEO_WATCH_STALL_MS,
): VideoWatchResult {
    const reference = lastSentMs === undefined ? startedMs : Math.max(lastSentMs, startedMs);
    if (prev.stalledFromMs !== undefined) {
        if (lastSentMs !== undefined && lastSentMs > prev.stalledFromMs) {
            return {
                state: VIDEO_WATCH_INITIAL,
                event: { type: "resumed", stalledMs: lastSentMs - prev.stalledFromMs },
            };
        }
        return { state: prev, event: undefined };
    }
    const silentMs = nowMs - reference;
    if (silentMs >= thresholdMs) {
        return { state: { stalledFromMs: reference }, event: { type: "stalled", silentMs } };
    }
    return { state: prev, event: undefined };
}
