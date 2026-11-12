// SPDX-License-Identifier: Apache-2.0

// ストリームごとに、映像と音声のタイムスタンプの原点 (encode/clock_map.ts の offsetUs) とその差を、送信側の
// ログに 1 行書くかを決める純粋関数 (clockOriginLogLine)。映像 (encode/session.ts) と音声 (audio/chunk.ts) は
// 原点を別々に持つ。同じストリームの 2 つの原点が食い違うと映像と音声がずれるので、それをログから読めるように
// する。状態は呼び出し元 (index.ts) がストリームごとに持つ。

/**
 * 呼び出し元がストリーム (streamId) ごとに持つ状態。間隔の制限はストリームごとに数える。あるストリームの原点が
 * 変わっても、ほかのストリームの行を止める理由がないためである。
 */
export interface ClockOriginLogState {
    readonly lastLoggedAtMs: number | undefined;
    readonly lastVideoOriginUs: bigint | undefined;
    readonly lastAudioOriginUs: bigint | undefined;
}

export function createClockOriginLogState(): ClockOriginLogState {
    return { lastLoggedAtMs: undefined, lastVideoOriginUs: undefined, lastAudioOriginUs: undefined };
}

export interface ClockOriginLogResult {
    readonly state: ClockOriginLogState;
    /** ログのリングバッファーに追加する 1 行。値が変わっていないか、前の行から 1 秒たっていないときは undefined。 */
    readonly line: string | undefined;
}

/** 同じストリームの行を書く最短の間隔 (ms)。 */
export const CLOCK_ORIGIN_LOG_INTERVAL_MS = 1_000;

function formatUs(value: bigint | undefined): string {
    return value === undefined ? "none" : `${value}us`;
}

/**
 * 映像と音声の原点 (encode/clock_map.ts の offsetUs) から、送信側のログに書く 1 行を作るかを決める。
 * 原点が決まったとき (undefined から値になったときを含む) と変わったときだけ書き、同じストリームでは 1 秒に
 * 1 回までにする。1 秒たっていないときは書かずに状態も変えないので、1 秒たった後の呼び出しが、その時点の値を
 * 1 行にまとめて書く。タイマーで書き直しを予約しないので、ストリームが止まる直前の最後の変化は書かれないことが
 * ある。呼び出し元はフレームとチャンクのたびに呼ぶので、ストリームが続いている間は 1 秒以内に次の呼び出しが来る。
 *
 * 映像も音声もまだ決まっていないストリームは書かない。片方だけ決まっていれば、もう片方を none として書く。
 * diff は video - audio で、両方が決まっているときだけ計算する。streamId はワイヤー形式の stream_id で、
 * 画面共有の映像と音声は同じ番号を使う。声には映像がないので videoOriginUs は常に undefined。nowMs は
 * 呼び出し元の Date.now()。
 */
export function clockOriginLogLine(
    state: ClockOriginLogState,
    streamId: number,
    videoOriginUs: bigint | undefined,
    audioOriginUs: bigint | undefined,
    nowMs: number,
): ClockOriginLogResult {
    if (videoOriginUs === undefined && audioOriginUs === undefined) {
        return { state, line: undefined }; // 映像も音声も、まだ原点が決まっていない。
    }

    const changed = videoOriginUs !== state.lastVideoOriginUs || audioOriginUs !== state.lastAudioOriginUs;
    if (!changed) return { state, line: undefined };

    if (state.lastLoggedAtMs !== undefined && nowMs - state.lastLoggedAtMs < CLOCK_ORIGIN_LOG_INTERVAL_MS) {
        return { state, line: undefined }; // 1 秒たっていない。状態を変えずに、後の呼び出しに任せる。
    }

    const diffUs = videoOriginUs !== undefined && audioOriginUs !== undefined ? videoOriginUs - audioOriginUs : undefined;
    const line = `clock origin stream ${streamId}: video=${formatUs(videoOriginUs)} audio=${formatUs(audioOriginUs)} diff=${formatUs(diffUs)}`;

    return {
        state: { lastLoggedAtMs: nowMs, lastVideoOriginUs: videoOriginUs, lastAudioOriginUs: audioOriginUs },
        line,
    };
}
