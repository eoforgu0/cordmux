// SPDX-License-Identifier: Apache-2.0

// core/wire.ts のテスト。バイナリのメッセージのヘッダーのエンコードとデコードを、往復、境界 (u32 と u64)、不正な
// type と flags を捨てることで確認する。形式は docs/wire_format.md の「ヘッダー」にある。

import { describe, expect, it } from "vitest";

import { decodeHeader, encodeHeader, FrameType, HEADER_BYTES, type FrameHeader } from "../core/wire";

function makeHeader(overrides: Partial<FrameHeader> = {}): FrameHeader {
    return {
        type: FrameType.Video,
        streamId: 1,
        timestampUs: 1_000_000n,
        flags: { keyframe: false, recordingOnly: false },
        ...overrides,
    };
}

describe("バイナリのメッセージのヘッダー", () => {
    it("ヘッダーの長さは 14 バイトである", () => {
        expect(HEADER_BYTES).toBe(14);
        expect(encodeHeader(makeHeader()).length).toBe(HEADER_BYTES);
    });

    it("決まったバイト列をデコードすると、リトルエンディアンとオフセットが期待どおりになる", () => {
        // type=1 (映像)、stream_id=0x05040302、timestamp_us=0x0d0c0b0a09080706、flags=0x01 (キーフレーム)
        const bytes = Uint8Array.from([
            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x01,
        ]);

        const result = decodeHeader(bytes);

        expect(result).toEqual({
            ok: true,
            header: {
                type: FrameType.Video,
                streamId: 0x05040302,
                timestampUs: 0x0d0c0b0a09080706n,
                flags: { keyframe: true, recordingOnly: false },
            },
        });
    });

    it("映像のキーフレームのヘッダーが往復する", () => {
        const header = makeHeader({
            type: FrameType.Video,
            streamId: 42,
            timestampUs: 123_456_789_012n,
            flags: { keyframe: true, recordingOnly: false },
        });

        const decoded = decodeHeader(encodeHeader(header));

        expect(decoded).toEqual({ ok: true, header });
    });

    it("音声で、録画だけに使うフラグ (recordingOnly) を立てたヘッダーが往復する", () => {
        const header = makeHeader({
            type: FrameType.Audio,
            streamId: 0,
            timestampUs: 0n,
            flags: { keyframe: false, recordingOnly: true },
        });

        const decoded = decodeHeader(encodeHeader(header));

        expect(decoded).toEqual({ ok: true, header });
    });

    it("各フィールドの最大値 (u32 と u64 の境界) が往復する", () => {
        const header = makeHeader({
            type: FrameType.Audio,
            streamId: 0xffffffff,
            timestampUs: 0xffffffffffffffffn,
            flags: { keyframe: true, recordingOnly: true },
        });

        const decoded = decodeHeader(encodeHeader(header));

        expect(decoded).toEqual({ ok: true, header });
    });

    it("Buffer を subarray した (byteOffset が 0 でない) 入力でもデコードできる", () => {
        const header = makeHeader({ streamId: 7 });
        const framed = new Uint8Array(HEADER_BYTES + 5);
        framed.set(encodeHeader(header), 5);
        const view = framed.subarray(5);

        expect(decodeHeader(view)).toEqual({ ok: true, header });
    });

    it("14 バイト未満の入力は捨てる", () => {
        const bytes = encodeHeader(makeHeader()).slice(0, HEADER_BYTES - 1);

        expect(decodeHeader(bytes)).toEqual({ ok: false, reason: "too-short" });
    });

    it("type が 0 の入力は捨てる", () => {
        const bytes = encodeHeader(makeHeader());
        bytes[0] = 0;

        expect(decodeHeader(bytes)).toEqual({ ok: false, reason: "unknown-type" });
    });

    it("type が 3 以上の入力は捨てる", () => {
        const bytes = encodeHeader(makeHeader());
        bytes[0] = 3;

        expect(decodeHeader(bytes)).toEqual({ ok: false, reason: "unknown-type" });
    });

    it("flags に未知のビットが立った入力は捨てる", () => {
        const bytes = encodeHeader(makeHeader());
        bytes[13] = 0b0000_0100;

        expect(decodeHeader(bytes)).toEqual({ ok: false, reason: "unknown-flags" });
    });

    it("エンコードは、範囲の外の streamId (u32 を超える) で例外を投げる", () => {
        expect(() => encodeHeader(makeHeader({ streamId: 0x100000000 }))).toThrow(RangeError);
    });

    it("エンコードは、範囲の外の streamId (負の数) で例外を投げる", () => {
        expect(() => encodeHeader(makeHeader({ streamId: -1 }))).toThrow(RangeError);
    });

    it("エンコードは、範囲の外の timestampUs で例外を投げる", () => {
        expect(() => encodeHeader(makeHeader({ timestampUs: -1n }))).toThrow(RangeError);
    });

    it("エンコードは、不正な type (0 か 3 以上) で例外を投げる", () => {
        expect(() => encodeHeader(makeHeader({ type: 0 as FrameType }))).toThrow(RangeError);
        expect(() => encodeHeader(makeHeader({ type: 3 as FrameType }))).toThrow(RangeError);
    });
});
