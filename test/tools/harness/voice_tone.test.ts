// SPDX-License-Identifier: Apache-2.0

// voice_tone.ts の単体テスト。声に乗せる音の高さと振れ幅、音を出し始めても波形がストリームの時刻とつながること、
// ファイルの中身から音を乗せる声のストリームを決める読み方 (フレームを止めるファイルと同じ FramePauseLedger を
// 声のストリームの番号で使う)、結果 JSON の区間の形を確認する。主プロセスがファイルを読む部分は
// frame_pause_file.test.ts で確認している (同じ関数を使う)。
import { describe, expect, it } from "vitest";

import { FramePauseLedger } from "./sender_pure";
import { fillTone, VOICE_TONE_AMPLITUDE, VOICE_TONE_FREQUENCY_HZ, voiceToneIntervals, voiceToneOf } from "./voice_tone";

describe("voiceToneOf", () => {
    it("同期用のマーカーの音を出すストリームは、ファイルにかかわらずマーカーの高さで鳴る", () => {
        expect(voiceToneOf(true, false, 2000)).toEqual({ frequencyHz: 2000, amplitude: VOICE_TONE_AMPLITUDE });
        expect(voiceToneOf(true, true, 1000)).toEqual({ frequencyHz: 1000, amplitude: VOICE_TONE_AMPLITUDE });
    });

    it("ファイルで指定された声のストリームは 1 kHz で鳴り、指定されていなければ無音になる", () => {
        expect(voiceToneOf(false, true, 2000)).toEqual({ frequencyHz: VOICE_TONE_FREQUENCY_HZ, amplitude: 0.3 });
        expect(voiceToneOf(false, false, 2000).amplitude).toBe(0);
    });
});

describe("fillTone", () => {
    it("無音のチャンクでも位相が進むので、音を出し始めたチャンクの波形はストリームの時刻とつながる", () => {
        const rate = 48000;
        const first = new Float32Array(480 * 2);
        const second = new Float32Array(480 * 2);
        let phase = fillTone(first, 2, rate, 1000, 0, 0);
        expect(phase).toBe(480);
        expect(first.every((v) => v === 0)).toBe(true);
        phase = fillTone(second, 2, rate, 1000, 0.3, phase);
        expect(phase).toBe(960);
        for (let s = 0; s < 480; s++) {
            const expected = 0.3 * Math.sin((2 * Math.PI * 1000 * (480 + s)) / rate);
            expect(second[s * 2]).toBeCloseTo(expected, 6);
            expect(second[s * 2 + 1]).toBe(second[s * 2]);
        }
    });
});

describe("声のストリームの番号で使う FramePauseLedger", () => {
    const voiceIds = [2, 3];
    const at = (state: "absent" | "present" | "error", text?: string) => ({ state, text, wallMs: 1000 });

    it("書いた番号の声だけに音を乗せ、空のファイルなら全部の声に乗せる", () => {
        const ledger = new FramePauseLedger(voiceIds);
        ledger.apply(at("present", "2"), 10);
        expect(ledger.isPaused(2)).toBe(true);
        expect(ledger.isPaused(3)).toBe(false);
        ledger.apply(at("present", "2 3"), 20);
        expect(ledger.isPaused(3)).toBe(true);
        ledger.apply(at("absent"), 30);
        expect(ledger.isPaused(2)).toBe(false);
        expect(ledger.isPaused(3)).toBe(false);
        ledger.apply(at("present", ""), 40);
        expect(ledger.isPaused(2) && ledger.isPaused(3)).toBe(true);
    });

    it("画面のストリームの番号は声に適用されず、ignoredTargets に残る", () => {
        const ledger = new FramePauseLedger(voiceIds);
        ledger.apply(at("present", "1,2"), 10);
        expect(ledger.isPaused(2)).toBe(true);
        expect(ledger.ignoredTargets).toEqual(["1"]);
    });

    it("結果 JSON の区間は、ストリームと始まりと終わりの時刻だけを持つ", () => {
        const ledger = new FramePauseLedger(voiceIds);
        ledger.apply({ state: "present", text: "3", wallMs: 5000 }, 100);
        ledger.apply({ state: "absent", wallMs: 9000 }, 4100);
        ledger.apply({ state: "present", text: "3", wallMs: 9500 }, 4600);
        expect(voiceToneIntervals(ledger.intervals)).toEqual([
            { streamId: 3, startMs: 100, startWallMs: 5000, endMs: 4100, endWallMs: 9000 },
            { streamId: 3, startMs: 4600, startWallMs: 9500, endMs: null, endWallMs: null },
        ]);
    });
});
