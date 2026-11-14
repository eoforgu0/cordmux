// SPDX-License-Identifier: Apache-2.0

// core/stream_silence.ts の shouldSilenceStreams() のテスト。入力の真偽の組み合わせをすべて回し、true になるのは
// 設定が ON、受信側と接続していて読み出しを許している、メインアカウントとサブアカウントの運用、メインアカウントが同じ
// 通話にいる、のすべてが成り立つときだけであることを確かめる。

import { describe, expect, it } from "vitest";

import {
    shouldSilenceStreams,
    type StreamSilenceInputs,
    StreamSilenceSwitch,
    streamSilenceInputs,
    type StreamSilenceSources,
} from "../core/stream_silence";

const MAIN = "100";
const SUB = "200";
const OTHER = "300";

/** 運用の 4 通り (メインアカウントの userId と Vesktop のアカウントの組)。sub だけがサブアカウントの運用。 */
const ACCOUNTS: readonly { name: string; mainUserId: string | null; selfUserId: string | null; sub: boolean }[] = [
    { name: "サブアカウント", mainUserId: MAIN, selfUserId: SUB, sub: true },
    { name: "メインアカウントが Vesktop", mainUserId: MAIN, selfUserId: MAIN, sub: false },
    { name: "メインアカウントを使わない", mainUserId: null, selfUserId: SUB, sub: false },
    { name: "Vesktop のアカウントが分からない", mainUserId: MAIN, selfUserId: null, sub: false },
];

/** 参加者の一覧の 4 通り。inCall はメインアカウントが同じ通話にいるか。 */
const ROSTERS: readonly { name: string; roster: StreamSilenceInputs["roster"]; inCall: boolean }[] = [
    { name: "メインアカウントがいる", roster: [{ userId: SUB }, { userId: MAIN }, { userId: OTHER }], inCall: true },
    { name: "メインアカウントがいない", roster: [{ userId: SUB }, { userId: OTHER }], inCall: false },
    { name: "空", roster: [], inCall: false },
    { name: "まだ読めていない", roster: "unknown", inCall: false },
];

describe("shouldSilenceStreams", () => {
    it("すべての組み合わせで、4 つの条件がそろったときだけ true になる", () => {
        let trueCount = 0;
        for (const enabled of [true, false]) {
            for (const gateOpen of [true, false]) {
                for (const account of ACCOUNTS) {
                    for (const r of ROSTERS) {
                        const got = shouldSilenceStreams({
                            enabled,
                            gateOpen,
                            mainUserId: account.mainUserId,
                            selfUserId: account.selfUserId,
                            roster: r.roster,
                        });
                        const want = enabled && gateOpen && account.sub && r.inCall;
                        expect(got, `${String(enabled)} ${String(gateOpen)} ${account.name} ${r.name}`).toBe(want);
                        if (got) trueCount++;
                    }
                }
            }
        }
        expect(trueCount).toBe(1);
    });

    it("メインアカウントの userId が空の文字列 (メインアカウントを使わない構成) なら false", () => {
        expect(shouldSilenceStreams({ enabled: true, gateOpen: true, mainUserId: "", selfUserId: SUB, roster: [{ userId: "" }] })).toBe(false);
    });

    it("メインアカウントが Vesktop にいる運用では、参加者の一覧にいても false", () => {
        expect(shouldSilenceStreams({ enabled: true, gateOpen: true, mainUserId: MAIN, selfUserId: MAIN, roster: [{ userId: MAIN }] })).toBe(false);
    });
});

// index.ts のつなぎ (refreshStreamSilence、handleMainAccount、handleClosed、stop) と同じ組み立てで、受信側の値が変わったときに
// adapter に伝わるものを確かめる。
describe("streamSilenceInputs と StreamSilenceSwitch (index.ts のつなぎ)", () => {
    /** 掛ける条件がそろった、index.ts の値 (メインアカウントは StreamSilenceSwitch が持つ)。 */
    const ready = (): StreamSilenceSources => ({
        limits: {},
        gateOpen: true,
        state: { selfUserId: SUB, roster: [{ userId: SUB }, { userId: MAIN }] },
    });

    function wire() {
        const applied: boolean[] = [];
        const logs: string[] = [];
        const sw = new StreamSilenceSwitch((on) => applied.push(on), (line) => logs.push(line));
        sw.setMainUser(MAIN);
        return { sw, applied, logs };
    }

    it("limits の silence_sub_stream_audio を省いたときは ON、false のときだけ OFF", () => {
        expect(streamSilenceInputs(ready(), MAIN).enabled).toBe(true);
        expect(streamSilenceInputs({ ...ready(), limits: { silence_sub_stream_audio: true } }, MAIN).enabled).toBe(true);
        expect(streamSilenceInputs({ ...ready(), limits: { silence_sub_stream_audio: false } }, MAIN).enabled).toBe(false);
    });

    it("条件がそろったら 1 回だけ掛け、同じ値では伝えない", () => {
        const { sw, applied, logs } = wire();
        sw.update(ready());
        sw.update(ready());
        expect(applied).toEqual([true]);
        expect(logs).toEqual(["stream audio: silencing screen shares in Vesktop"]);
        expect(sw.isOn()).toBe(true);
    });

    it("OBS と切れた (ゲートが閉じた) ら戻す", () => {
        const { sw, applied } = wire();
        sw.update(ready());
        sw.connectionClosed();
        sw.update({ ...ready(), gateOpen: false });
        expect(applied).toEqual([true, false]);
    });

    it("つなぎ直した直後は、受信側がメインアカウントを送り直すまで、前の接続の値で掛けない", () => {
        const { sw, applied } = wire();
        sw.update(ready());
        sw.connectionClosed();
        sw.update({ ...ready(), gateOpen: false });
        // 確立してゲートが開いたが、main_account はまだ届いていない。
        sw.update(ready());
        expect(applied).toEqual([true, false]);
        sw.setMainUser(MAIN);
        sw.update(ready());
        expect(applied).toEqual([true, false, true]);
    });

    it("main_account の空の文字列 (メインアカウントを使わない構成) では掛けない", () => {
        const { sw, applied } = wire();
        sw.setMainUser("");
        sw.update(ready());
        expect(applied).toEqual([]);
    });

    it("limits で OFF が届いたら戻し、ON に戻ったらまた掛ける", () => {
        const { sw, applied } = wire();
        sw.update(ready());
        sw.update({ ...ready(), limits: { silence_sub_stream_audio: false } });
        sw.update(ready());
        expect(applied).toEqual([true, false, true]);
    });

    it("メインアカウントが通話を抜けたら戻す", () => {
        const { sw, applied } = wire();
        sw.update(ready());
        sw.update({ ...ready(), state: { selfUserId: SUB, roster: [{ userId: SUB }] } });
        expect(applied).toEqual([true, false]);
    });

    it("stop で、掛けていたかにかかわらず adapter に戻させる", () => {
        const on = wire();
        on.sw.update(ready());
        on.sw.stop();
        expect(on.applied).toEqual([true, false]);
        expect(on.logs.at(-1)).toBe("stream audio: not silencing screen shares in Vesktop");
        const off = wire();
        off.sw.stop();
        expect(off.applied).toEqual([false]);
        expect(off.logs).toEqual([]);
    });
});
