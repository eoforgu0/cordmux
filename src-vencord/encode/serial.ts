// SPDX-License-Identifier: Apache-2.0

// エンコーダの確保 (isConfigSupported、configure、ウォームアップ) を 1 つずつ順に実行する SerialGate。
// 複数のエンコーダを同時に configure すると、OBS が動いている PC で 1 個か 2 個が初期化のエラーになった。
// そこで、すべての EncodeSession が 1 つの SerialGate を deps.gate として共有し、VideoEncoder を操作する
// 区間を重ねない。

/**
 * 渡した task を、前の task が終わってから (成功でも失敗でも) 1 つずつ実行する。
 * task が reject しても後の task は実行する (1 つの失敗で、ほかのエンコーダの確保を止めないため)。
 */
export class SerialGate {
    private tail: Promise<void> = Promise.resolve();

    /** task を、直前に渡された task が終わってから実行し、task の結果をそのまま返す (reject も返す)。 */
    run<T>(task: () => Promise<T>): Promise<T> {
        const started = this.tail.then(task);
        // 次の run() は、この task が終わるのを待つだけで、成功か失敗かは見ない。reject を捨てるのは
        // 内部の待ち行列 (tail) だけで、呼び出し元には started がそのまま届く。
        this.tail = started.then(
            () => undefined,
            () => undefined,
        );
        return started;
    }
}
