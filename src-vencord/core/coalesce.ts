// SPDX-License-Identifier: Apache-2.0

// 同じ処理の中で何度呼ばれても、処理が終わった後に 1 回だけ fn を実行する関数を返す。adapter は Flux の 1 つの
// 処理の中で roster と sharing のイベントを続けて送るので、イベントごとに facts を送ると、新しい roster と古い
// sharing の組み合わせがすぐ送られる。受信側は同じ facts の roster と sharing で枠を空けるので、2 つは同じ時点の
// 値でなければならない。schedule は処理が終わった後に呼ぶ関数を予約する関数で、既定はマイクロタスクである。
// 純粋な小さな関数で、DOM にも Vencord にも依存しない。

export function coalesceInTick(fn: () => void, schedule: (task: () => void) => void = queueMicrotask): () => void {
    let scheduled = false;
    return () => {
        if (scheduled) return;
        scheduled = true;
        schedule(() => {
            scheduled = false;
            fn();
        });
    };
}
