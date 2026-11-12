// SPDX-License-Identifier: Apache-2.0

// 受信側 (OBS) が止まったことを、送信側が自分で見つけるための監視。純粋関数で、時刻は持たない。呼び出し元の
// transport/socket.ts が、確立している間 WatchdogConfig の intervalMs ごとに 1 回呼ぶ。
//
// OBS のプロセスが丸ごと止まると、受信側は ping を送らず、送ったデータも読まない。送信側の送る待ち
// (bufferedAmount) は増え続ける。音は送る待ちが上限 (backpressure.ts の DEFAULT_AUDIO_CAP_BYTES) を超えたら
// 捨てるが、止まった接続を持ち続けると、OBS が戻っても、たまった古いデータを送り終えるまで新しい映像と音が
// 遅れる。そこで、ping が一定の時間来ないか、送る待ちが上限を超えたままの時間が続いたら、接続を切って繋ぎ直す
// (繋ぎ直しの間隔は transport/backoff.ts)。Discord の通話には触らない。

/** 監視の間隔と、切るまでの時間。どれも ms。 */
export interface WatchdogConfig {
    /** 監視を呼ぶ間隔。 */
    readonly intervalMs: number;
    /** ping が来ないまま、この時間がたったら切る。 */
    readonly pingSilenceMs: number;
    /** 送る待ちが音の上限を超えたまま、この時間がたったら切る。 */
    readonly overCapMs: number;
}

/**
 * 既定の値。受信側は 5 秒ごとに ping を送る (src-obs/ws/listener.h の ListenerConfig::ping_interval_ms) ので、
 * ping が来ないまま切るまでを 4 回分の 20 秒にした。どれも目安の値で、測って決めたものではない。
 */
export const DEFAULT_WATCHDOG_CONFIG: WatchdogConfig = {
    intervalMs: 5000,
    pingSilenceMs: 20000,
    overCapMs: 15000,
};

/** 切る理由。ping-silence は ping が来なかった、over-cap は送る待ちが音の上限を超えたままだった。 */
export type WatchdogReason = "ping-silence" | "over-cap";

/** 監視の状態。ping が来ないまま過ぎた時間と、送る待ちが上限を超えたまま過ぎた時間を持つ。 */
export interface WatchdogState {
    readonly silentMs: number;
    readonly overCapMs: number;
}

/** 確立した直後の状態。 */
export function createWatchdogState(): WatchdogState {
    return { silentMs: 0, overCapMs: 0 };
}

export interface WatchdogDecision {
    state: WatchdogState;
    /** null でなければ、呼び出し元は接続を切って繋ぎ直す。値は切る理由。 */
    reason: WatchdogReason | null;
}

/**
 * 1 回分の監視。observed.pingSeen は前の回からこの回までに ping が届いたか、observed.overCap は現在の送る待ちが
 * 音の上限を超えているか。前の回から config.intervalMs たったものとして数える。タイマーが遅れて、実際にはもっと
 * たっていたときは短く数えるので、切るのが遅れる側に倒れる。
 */
export function stepWatchdog(
    state: WatchdogState,
    observed: { pingSeen: boolean; overCap: boolean },
    config: WatchdogConfig = DEFAULT_WATCHDOG_CONFIG,
): WatchdogDecision {
    const next: WatchdogState = {
        silentMs: observed.pingSeen ? 0 : state.silentMs + config.intervalMs,
        overCapMs: observed.overCap ? state.overCapMs + config.intervalMs : 0,
    };
    if (next.silentMs >= config.pingSilenceMs) return { state: next, reason: "ping-silence" };
    if (next.overCapMs >= config.overCapMs) return { state: next, reason: "over-cap" };
    return { state: next, reason: null };
}
