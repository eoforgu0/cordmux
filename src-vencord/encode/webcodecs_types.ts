// SPDX-License-Identifier: Apache-2.0

// WebCodecs の VideoFrame、VideoEncoder、EncodedVideoChunk、VideoEncoderConfig のうち、encode/ が使う部分だけの
// 構造型。WebCodecs の型は lib.dom.d.ts にある。tsconfig.json の lib に DOM を追加すると、Node の vitest で動く
// core/ と encode/ の純粋関数がブラウザーにしかない API を使っても、型チェックで検出できなくなる。実際の値は
// この構造を満たすので、そのまま渡せる。

import type { HardwareAccelerationPreference } from "./ladder";

/**
 * 実際の VideoFrame のうち、encode/ が使う部分だけの構造。encode/session.ts は timestamp をキーにして、
 * エンコード後の EncodedVideoChunk (timestamp が同じ値になる) に、送信側の時計に換算したタイムスタンプを対応付ける。
 */
export interface VideoFrame {
    readonly timestamp: number;
    /** 使い終えたら必ず呼ぶ。呼ぶのは VideoFrame を受け取った側 (VideoFrameSource の約束)。 */
    close(): void;
}

/** VideoEncoder.configure に渡す設定 (実際の VideoEncoderConfig の部分集合)。 */
export interface VideoEncoderConfig {
    /** avc1.PPCCLL の形式 (encode/ladder.ts の codecStringForResolution が作る)。 */
    codec: string;
    width: number;
    height: number;
    framerate?: number;
    /** ビットレート (bps)。encode/session.ts は、1080p60 で 12 Mbps を基準に、ピクセル数と fps に比例させる。 */
    bitrate?: number;
    latencyMode?: "quality" | "realtime";
    /**
     * annexb は、SPS と PPS をキーフレームのビットストリームの中に入れる形式
     * (docs/wire_format.md の「映像のペイロード」)。
     */
    avc?: { format: "annexb" | "avc" };
    hardwareAcceleration?: HardwareAccelerationPreference | "no-preference";
}

/** VideoEncoder.encode の第 2 引数 (実際の VideoEncoderEncodeOptions の部分集合)。 */
export interface VideoEncoderEncodeOptions {
    keyFrame?: boolean;
}

/** VideoEncoder.isConfigSupported の解決値 (実際の VideoEncoderSupport の部分集合)。 */
export interface VideoEncoderSupport {
    supported: boolean;
}

/** VideoEncoder の output コールバックに渡る、エンコード済みの 1 アクセスユニット。 */
export interface EncodedVideoChunk {
    readonly type: "key" | "delta";
    /** エンコードした VideoFrame の timestamp と同じ値になる (WebCodecs の仕様)。 */
    readonly timestamp: number;
    readonly byteLength: number;
    /** dest (長さ byteLength 以上) へペイロードをコピーする (実際の copyTo)。 */
    copyTo(dest: Uint8Array): void;
}

/**
 * output コールバックの第 2 引数。annexb の形式では SPS と PPS がビットストリームの中にあるので、
 * decoderConfig は読まない。
 */
export type EncodedVideoChunkMetadata = Record<string, unknown>;

/** new VideoEncoder(init) の init (実際の VideoEncoderInit の部分集合)。 */
export interface VideoEncoderInit {
    output(chunk: EncodedVideoChunk, metadata: EncodedVideoChunkMetadata): void;
    error(error: unknown): void;
}

/**
 * VideoEncoder のインスタンスのうち、encode/ が使う部分だけの構造。
 * encode/session.ts の EncodeSessionDeps.createEncoder(init) が返す。
 */
export interface VideoEncoderLike {
    readonly encodeQueueSize: number;
    configure(config: VideoEncoderConfig): void;
    encode(frame: VideoFrame, options?: VideoEncoderEncodeOptions): void;
    flush(): Promise<void>;
    close(): void;
}
