// SPDX-License-Identifier: Apache-2.0

// encode/session.ts が映像のフレームを 1 つずつ受け取るインターフェイス (VideoFrameSource)。実装を差し替え
// られるようにして、encode/session.ts を Node の vitest でテストできるようにする。VideoFrame は
// encode/webcodecs_types.ts の構造型を使う (tsconfig.json の lib に DOM を追加しないため)。

import type { SourceFormat } from "./ladder";
import type { VideoFrame } from "./webcodecs_types";

/**
 * エンコーダに渡す映像のフレームを 1 つずつ取り出すインターフェイス。実装は 3 つある。製品の
 * encode/track_source.ts (MediaStreamTrackProcessor)、テスト用の送信側の test/tools/harness/sender.ts
 * (OffscreenCanvas に描いたフレーム)、tests/encode_session.test.ts のテスト用の実装である。
 */
export interface VideoFrameSource {
    /** 届いているフレームの解像度と fps。画面共有ごとに違う。作ったときに 1 回だけ読み、後から変えない。 */
    readonly format: SourceFormat;

    /**
     * 次のフレームを待つ。トラックが止まった、close() が呼ばれた、などでフレームがもう来ないときは null を返す。
     * 返った VideoFrame は、呼び出し元 (encode/session.ts) が使い終えたら必ず close() する。
     */
    next(): Promise<VideoFrame | null>;

    /**
     * 直前に next() が返したフレームを、元 (MediaStreamTrackProcessor) から読んだときの送信側の時計 (us)。1 回読むと
     * undefined に戻る。実装がこの時刻を持たないときは省く (呼び出し元は、next() が返ったときの時刻を使う)。
     * エンコードが詰まって next() が遅れても、その待ちを含まない時刻で、encode/video_time.ts の先行の見分けに使う。
     */
    takeReadAtUs?(): bigint | undefined;

    /**
     * 閉じる。next() を待っている Promise があれば、null で解決する (encode/session.ts の stop() が
     * close() を呼んだときに、読み出しのループが next() で待ち続けないようにするため)。
     * 2 回以上呼んでも、2 回目からは何もしない。
     */
    close(): void;
}
