// SPDX-License-Identifier: Apache-2.0

// encode/serial.ts の SerialGate のテスト。VideoEncoder の configure() を同時に呼び出すと初期化のエラーになるので、
// エンコーダの確保を 1 つずつ順に行う。

import { describe, expect, it } from "vitest";

import { SerialGate } from "../encode/serial";

function deferred<T>(): { promise: Promise<T>; resolve: (v: T) => void; reject: (e: unknown) => void } {
    let resolve!: (v: T) => void;
    let reject!: (e: unknown) => void;
    const promise = new Promise<T>((res, rej) => {
        resolve = res;
        reject = rej;
    });
    return { promise, resolve, reject };
}

describe("SerialGate", () => {
    it("task が 1 つなら、そのまま実行して結果を返す", async () => {
        const gate = new SerialGate();
        const result = await gate.run(async () => 42);
        expect(result).toBe(42);
    });

    it("先に入れた task が終わるまで、後の task は始めない (1 つずつ順に実行する)", async () => {
        const gate = new SerialGate();
        const log: string[] = [];
        const first = deferred<void>();

        const p1 = gate.run(async () => {
            log.push("1:start");
            await first.promise;
            log.push("1:end");
        });
        // p1 がまだ終わっていない (first.promise を待っている) うちに、p2 を入れる。
        const p2 = gate.run(async () => {
            log.push("2:start");
        });

        // p1 が first を待っている間は、p2 はまだ何も記録していない。
        await Promise.resolve();
        await Promise.resolve();
        await Promise.resolve();
        expect(log).toEqual(["1:start"]);

        first.resolve();
        await p1;
        await p2;

        expect(log).toEqual(["1:start", "1:end", "2:start"]);
    });

    it("task が reject しても、後の task は実行される (ゲートが止まらない)", async () => {
        const gate = new SerialGate();
        const log: string[] = [];

        const p1 = gate.run(async () => {
            log.push("1");
            throw new Error("fake failure");
        });
        const p2 = gate.run(async () => {
            log.push("2");
            return "ok";
        });

        await expect(p1).rejects.toThrow("fake failure");
        await expect(p2).resolves.toBe("ok");
        expect(log).toEqual(["1", "2"]);
    });

    it("同時に入れた複数の task は、入れた順に実行される", async () => {
        const gate = new SerialGate();
        const order: number[] = [];
        const runs = [1, 2, 3].map((n) =>
            gate.run(async () => {
                order.push(n);
            }),
        );
        await Promise.all(runs);
        expect(order).toEqual([1, 2, 3]);
    });
});
