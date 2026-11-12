// SPDX-License-Identifier: Apache-2.0

// 受信側への再接続を試すまでの待ち時間を決める純粋関数。実際に待って再接続するのは、呼び出し元の
// transport/client.ts と transport/socket.ts である。

/** 1 回失敗した後の待ち時間 (ms)。失敗が続くと、ここから間隔を延ばす。 */
export const BACKOFF_INITIAL_MS = 2_000;

/**
 * 試行の間隔の上限 (ms)。OBS が起動して待ち受けを始めてから、10 秒以内に再接続できるようにするための値で、
 * 上限が 5 秒なら、待ち受けを始めてから遅くとも 5 秒で次の試行が来る。上限を 30 秒のように長くすると、
 * OBS の起動を待つ間に間隔が延び、待ち受けが始まってから次の試行まで最大 30 秒空く。
 */
export const BACKOFF_CAP_MS = 5_000;

/**
 * 続けて失敗した回数 priorFailureCount から、次の試行までの待ち時間 (ms) を返す。
 * priorFailureCount が 0 (まだ失敗していない。切断を検出した直後) なら 0 を返し、すぐに 1 回試す。
 * 1 から先は 2000、4000、5000、5000、... と延びる。2 倍ずつ延ばすのは一般的な指数バックオフに合わせたもので、
 * 上限 (BACKOFF_CAP_MS) が同じなら、ほかの延ばし方でもよい。priorFailureCount が 0 以上の整数でなければ
 * RangeError を投げる (呼び出し元の誤りを隠さないため)。
 */
export function reconnectDelayMs(priorFailureCount: number): number {
    if (!Number.isInteger(priorFailureCount) || priorFailureCount < 0) {
        throw new RangeError(
            `transport/backoff: priorFailureCount は0以上の整数にしてください: ${priorFailureCount}`,
        );
    }

    if (priorFailureCount === 0) return 0;

    const scaled = BACKOFF_INITIAL_MS * 2 ** (priorFailureCount - 1);
    return Math.min(scaled, BACKOFF_CAP_MS);
}
