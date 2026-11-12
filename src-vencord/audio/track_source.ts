// SPDX-License-Identifier: Apache-2.0

// AudioSource の製品の実装。MediaStreamTrackProcessor で音声のトラックから AudioData を読み、audio/chunk.ts が
// 受け取る形 (f32 のインターリーブ、2 チャンネル) に変換する。AudioWorklet は使わない。Vencord のバンドルとは
// 別の URL のスクリプトが必要になるためである。
// MediaStreamTrackProcessor と AudioData はブラウザーの実行時にしかないので、このファイルを tsconfig.json の
// exclude に入れ、Vencord の pnpm test:ts で型をチェックする。ただし、tests/audio_track_source.test.ts が import
// するので、リポジトリのルートの tsc もチェックする (使う型はこのファイルで宣言している)。このテストは、
// MediaStreamTrackProcessor をテスト用の実装に置き換えて実行する。

import type { MediaStreamTrackLike } from "../adapter/types";
import type { AudioSource } from "./audio_source";
import { AUDIO_CHANNELS, AUDIO_SAMPLE_RATE_HZ } from "./chunk";

interface AudioDataCopyToOptions {
    planeIndex: number;
    format?: "f32-planar" | "f32";
}

/** AudioData のうち、この実装が使う部分だけの型。 */
interface AudioDataLike {
    readonly sampleRate: number;
    readonly numberOfFrames: number;
    readonly numberOfChannels: number;
    /** 先頭サンプルのメディアのタイムスタンプ (us)。そのまま呼び出し元に渡す。 */
    readonly timestamp: number;
    /** planeIndex 番目のチャンネルのサンプルを、dest (長さは numberOfFrames 以上) にコピーする。 */
    copyTo(dest: Float32Array, options: AudioDataCopyToOptions): void;
    close(): void;
}

interface AudioDataStreamReader {
    read(): Promise<{ value: AudioDataLike | undefined; done: boolean }>;
    cancel(reason?: unknown): Promise<void>;
}

interface AudioDataReadableStream {
    getReader(): AudioDataStreamReader;
}

interface MediaStreamTrackProcessorLike {
    readonly readable: AudioDataReadableStream;
}

/** MediaStreamTrackProcessor のコンストラクター。Vesktop のレンダラーにだけある。 */
declare const MediaStreamTrackProcessor: {
    new (init: { track: MediaStreamTrackLike }): MediaStreamTrackProcessorLike;
};

/** createAudioTrackSource が返す値。AudioSource に、48 kHz でないために捨てた数を加えたもの。 */
export interface TrackAudioSource extends AudioSource {
    /** サンプルレートが 48 kHz でないので、中身を読まずに捨てた AudioData の数。 */
    readonly droppedForSampleRate: number;
}

function interleave(data: AudioDataLike): Float32Array {
    const frames = data.numberOfFrames;
    const channelCount = Math.min(AUDIO_CHANNELS, Math.max(1, data.numberOfChannels));
    const planes: Float32Array[] = [];
    for (let ch = 0; ch < channelCount; ch++) {
        const plane = new Float32Array(frames);
        data.copyTo(plane, { planeIndex: ch, format: "f32-planar" });
        planes.push(plane);
    }
    const out = new Float32Array(frames * AUDIO_CHANNELS);
    for (let frame = 0; frame < frames; frame++) {
        for (let ch = 0; ch < AUDIO_CHANNELS; ch++) {
            // channelCount が 1 (モノラル) のときは、同じチャンネルを L と R の両方にコピーする
            // (channelCount 以上の ch には、最後のチャンネルを使う)。
            const plane = planes[Math.min(ch, channelCount - 1)] as Float32Array;
            out[frame * AUDIO_CHANNELS + ch] = plane[frame] as number;
        }
    }
    return out;
}

/**
 * 音声のトラックを読む AudioSource を作る。copyTo({ planeIndex, format: "f32-planar" }) でチャンネルごとに読み、
 * この関数でインターリーブにする。copyTo の format: "f32" (インターリーブへの変換) は、実装による差があり、
 * Vesktop で正しく変換されるかを確認していないので使わない。
 *
 * 音声のデータは映像よりずっと小さく、捨てると音が途切れるので、encode/track_source.ts (映像) と違い、古い
 * 断片を間引かずに届いた順にすべて読む。ただし、サンプルレートが 48 kHz でない AudioData は、中身を読まずに
 * 閉じて捨て、droppedForSampleRate を 1 増やす (リサンプリングはしない)。モノラルは L と R の両方にコピーし、
 * 3 チャンネル以上は先頭の 2 チャンネルだけを使う。Discord の音声は通常モノラルかステレオで、48 kHz でない入力と
 * 3 チャンネル以上の入力が実際に届くかは確認していない。
 */
export function createAudioTrackSource(track: MediaStreamTrackLike): TrackAudioSource {
    const processor = new MediaStreamTrackProcessor({ track });
    const reader = processor.readable.getReader();

    let closed = false;
    let droppedForSampleRate = 0;

    return {
        get droppedForSampleRate(): number {
            return droppedForSampleRate;
        },
        async next(): Promise<{ samples: Float32Array; mediaTimestampUs: number } | null> {
            for (;;) {
                if (closed) return null;
                let result: { value: AudioDataLike | undefined; done: boolean };
                try {
                    result = await reader.read();
                } catch {
                    // トラックが異常終了したときなど。reject せずに null を返し、ソースが終わったものとして扱う
                    // (encode/track_source.ts と同じ)。
                    closed = true;
                    return null;
                }
                if (closed) {
                    if (result.value) result.value.close();
                    return null;
                }
                if (result.done || result.value === undefined) {
                    closed = true;
                    return null;
                }
                const data = result.value;
                if (data.sampleRate !== AUDIO_SAMPLE_RATE_HZ) {
                    droppedForSampleRate++;
                    data.close();
                    continue; // この断片は丸ごと捨て、次の AudioData を待つ。
                }
                // 取り出した時刻ではなく、AudioData.timestamp をそのまま渡す。送信側の時計への換算は
                // encode/clock_map.ts が、到着の時刻の測定は呼び出し元がする。
                const mediaTimestampUs = data.timestamp;
                const samples = interleave(data);
                data.close();
                return { samples, mediaTimestampUs };
            }
        },
        close(): void {
            if (closed) return;
            closed = true;
            reader.cancel().catch(() => {
                // 既に終わったトラックをもう一度止めたときの失敗などは無視する。
            });
        },
    };
}
