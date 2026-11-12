// SPDX-License-Identifier: Apache-2.0

// core/logbook.ts のテスト。送信側のログを、出来事の行、異常に関わる出来事の行、統計の行に分けて保ち、種類ごとの
// 上限で古い行から捨て、捨てた数を次に送るときに伝えることを確認する。

import { describe, expect, it } from "vitest";

import { LogBook } from "../core/logbook";

describe("LogBook", () => {
    it("3 種類の行を、種類ごとに古い順で返し、返した後は空になる", () => {
        const book = new LogBook(10, 10);
        book.event("e1");
        book.stat("s1");
        book.alert("a1");
        book.event("e2");
        expect(book.isEmpty()).toBe(false);
        expect(book.drain()).toEqual({ lines: ["e1", "e2"], alerts: ["a1"], stats: ["s1"] });
        expect(book.isEmpty()).toBe(true);
        expect(book.drain()).toEqual({ lines: [], alerts: [], stats: [] });
    });

    it("統計の行が多くても、出来事の行は押し出されない (上限は種類ごと)", () => {
        const book = new LogBook(3, 2);
        book.event("e1");
        for (let i = 0; i < 100; i++) book.stat(`s${i}`);
        const out = book.drain();
        expect(out.lines).toEqual(["log: dropped 98 older stats lines while waiting to send", "e1"]);
        expect(out.stats).toEqual(["s98", "s99"]);
    });

    it("出来事の行と異常の行は 1 つの上限で古いものから捨て、捨てた数を次の 1 回だけ先頭に書く", () => {
        const book = new LogBook(2, 2);
        book.event("e1");
        book.alert("a1");
        book.event("e2");
        const out = book.drain();
        expect(out.lines).toEqual(["log: dropped 1 older event lines while waiting to send", "e2"]);
        expect(out.alerts).toEqual(["a1"]);
        book.event("e3");
        expect(book.drain().lines).toEqual(["e3"]);
    });

    it("上限が正の整数でなければ RangeError を投げる", () => {
        expect(() => new LogBook(0, 1)).toThrow(RangeError);
        expect(() => new LogBook(1, 1.5)).toThrow(RangeError);
    });
});
