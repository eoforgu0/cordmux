// SPDX-License-Identifier: Apache-2.0

// ワイヤー形式のバイナリメッセージのヘッダー (14 バイト、固定長、リトルエンディアン) をエンコードし、デコードする
// 純粋関数 (encodeHeader、decodeHeader)。形式は docs/wire_format.md の「バイナリメッセージ」にある。食い違いが
// 見つかったら、まず docs/wire_format.md を修正し、同じ変更でこのファイルと tests/core_wire.test.ts を修正する。
// Vencord のものは import しないので、リポジトリのルートの tsc と vitest でテストできる。

/** ヘッダーのバイト数。 */
export const HEADER_BYTES = 14;

/**
 * バイナリメッセージの種類。decodeHeader は 0 と 3 以上を unknown-type として捨てる
 * (docs/wire_format.md の「type」)。
 */
export const FrameType = {
    Video: 1,
    Audio: 2,
} as const;

export type FrameType = (typeof FrameType)[keyof typeof FrameType];

function isFrameType(value: number): value is FrameType {
    return value === FrameType.Video || value === FrameType.Audio;
}

// flags の定義済みのビット。ほかのビットが 1 つでも立っていたら、メッセージごと捨てる
// (docs/wire_format.md の「flags」)。
const FLAG_KEYFRAME = 0b0000_0001;
const FLAG_RECORDING_ONLY = 0b0000_0010;
const FLAG_KNOWN_MASK = FLAG_KEYFRAME | FLAG_RECORDING_ONLY;

export interface FrameFlags {
    /** bit0: キーフレームか。 */
    keyframe: boolean;
    /** bit1: 録画専用か (黒埋め用のフレーム。受信側はデコードも表示もしない)。 */
    recordingOnly: boolean;
}

/**
 * ヘッダーの中身。並び (オフセット: 型) は次のとおりで、合計 14 バイトになる。ペイロードの長さはヘッダーに
 * 持たない (WebSocket の 1 メッセージが 1 つのバイナリメッセージなので、メッセージの長さから求まる)。
 *   type          @0  : u8        1 = 映像 (H.264 Annex B の 1 アクセスユニット)、
 *                                 2 = 音声 (f32 のインターリーブ、48 kHz、2 ch、10 ms)
 *   stream_id     @1  : u32 (LE)  ストリームの番号。画面共有と声で 1 つの番号の空間を使う
 *   timestamp_us  @5  : u64 (LE)  送信側の時計 (encode/sender_clock.ts) の us。単調ではない
 *   flags         @13 : u8        bit0 = キーフレーム、bit1 = 録画専用
 */
export interface FrameHeader {
    type: FrameType;
    /** u32 の範囲 (0..4294967295)。 */
    streamId: number;
    /**
     * u64 の範囲。送信側の時計 (encode/sender_clock.ts) の us。number では 2^53 を超える値の精度が落ちるので、
     * bigint で持つ。
     */
    timestampUs: bigint;
    flags: FrameFlags;
}

const U32_MAX = 0xffffffff;
const U64_MAX = 0xffffffffffffffffn;

function assertUintRange(value: number, max: number, label: string): void {
    if (!Number.isInteger(value) || value < 0 || value > max) {
        throw new RangeError(`wire: ${label} が範囲外です (0..${max}): ${value}`);
    }
}

function assertU64Range(value: bigint, label: string): void {
    if (value < 0n || value > U64_MAX) {
        throw new RangeError(`wire: ${label} が範囲外です (0..${U64_MAX}): ${value}`);
    }
}

function encodeFlags(flags: FrameFlags): number {
    let value = 0;
    if (flags.keyframe) value |= FLAG_KEYFRAME;
    if (flags.recordingOnly) value |= FLAG_RECORDING_ONLY;
    return value;
}

function decodeFlags(value: number): FrameFlags | undefined {
    // 知らないビットが立っていたら捨てる (docs/wire_format.md の「flags」)。
    if ((value & ~FLAG_KNOWN_MASK) !== 0) return undefined;
    return {
        keyframe: (value & FLAG_KEYFRAME) !== 0,
        recordingOnly: (value & FLAG_RECORDING_ONLY) !== 0,
    };
}

/**
 * ヘッダーをバイト列にエンコードする。値が範囲外なら RangeError を投げる。送信側のプログラムの誤りを
 * 見逃さないためで、受け取った不正な入力を捨てるのは decodeHeader の役目である。
 */
export function encodeHeader(header: FrameHeader): Uint8Array {
    if (!isFrameType(header.type)) {
        throw new RangeError(`wire: type が不正です (1 または 2 以外): ${header.type}`);
    }
    assertUintRange(header.streamId, U32_MAX, "streamId");
    assertU64Range(header.timestampUs, "timestampUs");

    const bytes = new Uint8Array(HEADER_BYTES);
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);

    view.setUint8(0, header.type);
    view.setUint32(1, header.streamId, true);
    view.setBigUint64(5, header.timestampUs, true);
    view.setUint8(13, encodeFlags(header.flags));

    return bytes;
}

export type DecodeHeaderResult =
    | { ok: true; header: FrameHeader }
    | { ok: false; reason: "too-short" | "unknown-type" | "unknown-flags" };

/**
 * バイト列の先頭 14 バイトをヘッダーとしてデコードする。長さが足りない、type が不正、flags が不正のどれかなら、
 * 例外を投げずに { ok: false } と理由を返す。bytes はヘッダーの後ろにペイロードが続いていてよい
 * (ペイロードはここでは読まない)。
 */
export function decodeHeader(bytes: Uint8Array): DecodeHeaderResult {
    if (bytes.length < HEADER_BYTES) {
        return { ok: false, reason: "too-short" };
    }

    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);

    const typeValue = view.getUint8(0);
    if (!isFrameType(typeValue)) {
        return { ok: false, reason: "unknown-type" };
    }

    const streamId = view.getUint32(1, true);
    const timestampUs = view.getBigUint64(5, true);

    const flags = decodeFlags(view.getUint8(13));
    if (!flags) {
        return { ok: false, reason: "unknown-flags" };
    }

    return {
        ok: true,
        header: { type: typeValue, streamId, timestampUs, flags },
    };
}
