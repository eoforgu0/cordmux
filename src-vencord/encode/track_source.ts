// SPDX-License-Identifier: Apache-2.0

// VideoFrameSource の製品の実装 (createVideoTrackSource)。MediaStreamTrackProcessor でトラックから VideoFrame を
// 読み出す。読んだ VideoFrame はそのまま渡し、timestamp (メディアのタイムスタンプ) も変えない。送信側の時計に換算
// するのは encode/clock_map.ts、到着の時刻を測るのは encode/session.ts である。MediaStreamTrackProcessor は
// ブラウザーにしかないので、tsconfig.json の exclude でリポジトリのルートの tsc から外し、Vencord の pnpm test:ts で
// チェックする (src-vencord/README.md)。vitest ではテストしない。DOM の型は使わず、このファイルで宣言した最小の
// 構造型と、adapter/types.ts の MediaStreamTrackLike を使う。

import type { MediaStreamTrackLike } from "../adapter/types";
import type { VideoFrameSource } from "./frame_source";
import type { SourceFormat } from "./ladder";
import type { VideoFrame } from "./webcodecs_types";

/** MediaStreamTrackProcessor.readable.getReader() が返すリーダー (実際の構造の最小限)。 */
interface VideoFrameStreamReader {
    read(): Promise<{ value: VideoFrame | undefined; done: boolean }>;
    cancel(reason?: unknown): Promise<void>;
}

interface VideoFrameReadableStream {
    getReader(): VideoFrameStreamReader;
}

interface MediaStreamTrackProcessorLike {
    readonly readable: VideoFrameReadableStream;
}

/** MediaStreamTrackProcessor のコンストラクター (ブラウザーの中、つまり Vesktop のレンダラーにだけある)。 */
declare const MediaStreamTrackProcessor: {
    new (init: { track: MediaStreamTrackLike }): MediaStreamTrackProcessorLike;
};

/** track.getSettings() が返す値のうち、読む 3 つだけ (実際の値の部分集合)。 */
interface VideoTrackSettingsLike {
    width?: number;
    height?: number;
    frameRate?: number;
}

/** getSettings() を持つ映像のトラック。 */
export interface VideoMediaStreamTrack extends MediaStreamTrackLike {
    getSettings(): VideoTrackSettingsLike;
}

// getSettings() が値を返さなかったときに使う値。目安の値で、測って決めたものではない (encode/ladder.ts の段階 4 と
// 同じ 1280x720 30 fps)。
const FALLBACK_FORMAT: SourceFormat = { width: 1280, height: 720, fps: 30 };

/**
 * track から VideoFrame を読み出す VideoFrameSource を作る。待ち行列は持たず、まだ渡していない最新の 1 枚だけを
 * 持ち、古いフレームはすぐに close する (フレームをためると、レンダラーの描画が遅れるため)。バックグラウンドで
 * 読み続け、next() が呼ばれたときに、届いているフレームがあればそれを、なければ次に届いた 1 枚を返す。古いフレームを
 * 捨てても、残ったフレームの timestamp はずれない。
 *
 * width、height、frameRate は、作ったときに 1 回だけ getSettings() から読む (VideoFrameSource の format の約束)。
 * 画面共有の途中で解像度が変わっても、format は変わらない。
 */
export function createVideoTrackSource(track: VideoMediaStreamTrack, nowUs?: () => bigint): VideoFrameSource {
    const processor = new MediaStreamTrackProcessor({ track });
    const reader = processor.readable.getReader();

    const settings = track.getSettings();
    const format: SourceFormat = {
        width: settings.width ?? FALLBACK_FORMAT.width,
        height: settings.height ?? FALLBACK_FORMAT.height,
        fps: settings.frameRate ?? FALLBACK_FORMAT.fps,
    };

    let closed = false;
    let held: VideoFrame | undefined; // まだ渡していない最新の 1 枚
    let heldReadUs: bigint | undefined; // held を MediaStreamTrackProcessor から読んだ時刻
    let lastReadUs: bigint | undefined; // 最後に next() で渡したフレームを読んだ時刻 (takeReadAtUs)
    let waiter: ((frame: VideoFrame | null) => void) | undefined; // next() が待っている間の resolve

    function deliver(frame: VideoFrame | null, readUs: bigint | undefined): void {
        if (waiter) {
            const resolve = waiter;
            waiter = undefined;
            lastReadUs = readUs;
            resolve(frame);
            return;
        }
        if (frame && held) held.close(); // 前の 1 枚を渡す前に次が来た。古いほうを捨てる。
        held = frame ?? undefined;
        heldReadUs = readUs;
    }

    // バックグラウンドで読み続け、まだ渡していない最新の 1 枚だけを残す。
    void (async () => {
        try {
            for (;;) {
                const result = await reader.read();
                if (closed) {
                    if (result.value) result.value.close();
                    return;
                }
                if (result.done || result.value === undefined) {
                    closed = true;
                    deliver(null, undefined);
                    return;
                }
                // 読んだ時刻は、フレームを受け取った最も早い時点で測る。エンコードが詰まって next() が遅れても、
                // その待ちを入れない (encode/video_time.ts の先行の見分けに使う)。
                deliver(result.value, nowUs?.());
            }
        } catch {
            // reader.read() が失敗した (トラックが異常終了した、など)。フレームの供給を終える。
            closed = true;
            deliver(null, undefined);
        }
    })();

    return {
        format,
        next(): Promise<VideoFrame | null> {
            if (closed && held === undefined) return Promise.resolve(null);
            if (held !== undefined) {
                const frame = held;
                held = undefined;
                lastReadUs = heldReadUs;
                heldReadUs = undefined;
                return Promise.resolve(frame);
            }
            return new Promise((resolve) => {
                waiter = resolve;
            });
        },
        takeReadAtUs(): bigint | undefined {
            const readUs = lastReadUs;
            lastReadUs = undefined;
            return readUs;
        },
        close(): void {
            if (closed) return;
            closed = true;
            if (held) {
                held.close();
                held = undefined;
            }
            if (waiter) {
                const resolve = waiter;
                waiter = undefined;
                resolve(null);
            }
            reader.cancel().catch(() => {
                // 終わったトラックをもう一度止めたときなどの失敗は無視する。
            });
        },
    };
}
