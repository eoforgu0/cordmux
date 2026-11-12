// SPDX-License-Identifier: Apache-2.0

// 受信した音声を、受け取った長さのまま 1 つずつ取り出すインターフェイス AudioSource。このフォルダーでは、この
// 1 つ分を断片と呼び、ワイヤー形式の 480 サンプルのチャンクと区別する。実装は、製品の audio/track_source.ts
// (MediaStreamTrackProcessor で読む) と、テスト用の送信側の test/tools/harness/sender.ts (正弦波を作る) にある。

/**
 * 受信した音声の断片 (f32 のインターリーブ) を 1 つずつ取り出す。断片の長さは、audio/chunk.ts の AUDIO_CHANNELS の
 * 倍数ならいくつでもよい。480 サンプルのチャンクに分けるのは audio/chunk.ts の pushAudio() で、ここでは届いた
 * 断片のまま渡す。
 */
export interface AudioSource {
    /**
     * 次の断片を待つ。ソースが終わったときは null を返す (VideoFrameSource.next() と同じ約束)。
     * mediaTimestampUs は、断片の先頭サンプルのメディアのタイムスタンプ (us)。製品では AudioData.timestamp を
     * そのまま渡す。0 の位置はブラウザーが決めるので、送信側の時計への換算は encode/clock_map.ts がする。
     * 受信した時刻はここでは返さない。取り出した時刻をタイムスタンプにしないためで、到着の時刻は、呼び出し元が
     * next() の解決の直後に測る。
     */
    next(): Promise<{ samples: Float32Array; mediaTimestampUs: number } | null>;

    /**
     * ソースを閉じる。next() を待っている Promise があれば、null で解決する。何度呼び出してもよい
     * (2 回目からは何もしない)。
     */
    close(): void;
}
