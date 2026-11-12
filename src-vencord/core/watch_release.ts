// SPDX-License-Identifier: Apache-2.0

// 受信側との接続が切れた後に、視聴をやめるまでの時間を数える。短い切断では視聴を続ける。視聴をやめて始め直すと、映像が
// 届くまで待つことになり、配信の映像が止まるためである。切れたままなら受信側はいないものとして視聴をやめる。OBS を
// 終了した後も、共有している人の画面に視聴者として出続け、帯域を使い続けることになるためである。
// 接続し直すと、受信側が現在の範囲を送り直すので、やめた視聴はそこで始め直す。
// タイマーのほかは何にも依存しない。

export interface WatchRelease {
    /** 接続が切れた。delayMs たっても cancel() がなければ release を呼ぶ。 */
    closed(): void;
    /** 接続し直した、または止める。数えている途中なら、やめる。 */
    cancel(): void;
}

export function createWatchRelease(release: () => void, delayMs: number): WatchRelease {
    let timer: ReturnType<typeof setTimeout> | null = null;
    const cancel = (): void => {
        if (timer === null) return;
        clearTimeout(timer);
        timer = null;
    };
    return {
        closed() {
            cancel();
            timer = setTimeout(() => {
                timer = null;
                release();
            }, delayMs);
        },
        cancel,
    };
}
