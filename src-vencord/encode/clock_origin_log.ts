// SPDX-License-Identifier: Apache-2.0

// ストリームごとに、映像と音声のタイムスタンプの原点 (encode/clock_map.ts の offsetUs) とその差を、送信側の
// ログに 1 行書くかを決める純粋関数 (clockOriginLogLine)。映像 (encode/session.ts) と音声 (audio/chunk.ts) は
// 原点を別々に持つ。同じストリームの 2 つの原点が食い違うと映像と音声がずれるので、それをログから読めるように
// する。状態は呼び出し元 (index.ts) がストリームごとに持つ。
//
// 書く行は 2 種類に分ける。原点が初めて決まったときと、差が大きく動いたときは出来事の行にする。受信側は出来事の行を
// 通話の全体に残す (discord.log) ので、ずれたと言われた後でも原因を追える。それ以外の細かい変化は統計の行にする。
// 原点は、下げる向きにはその場で、上げる向きには少しずつ動く (encode/clock_map.ts)。通話が安定すれば変わらなくなるが、
// 揺れが大きいと下がる回数が増え、上げている間はフレームごとに変わる。そのすべてを出来事の行にすると、ほかの出来事の
// 行を押し出しかねない。少しずつ上げる間は、差が CLOCK_ORIGIN_EVENT_DIFF_US 動くごとに出来事の行になる (受信を始めた
// 直後に下がった 150 ms を戻すなら 6 から 7 行)。

/**
 * 呼び出し元がストリーム (streamId) ごとに持つ状態。間隔の制限はストリームごとに数える。あるストリームの原点が
 * 変わっても、ほかのストリームの行を止める理由がないためである。
 */
export interface ClockOriginLogState {
    readonly lastLoggedAtMs: number | undefined;
    readonly lastVideoOriginUs: bigint | undefined;
    readonly lastAudioOriginUs: bigint | undefined;
    /** 最後に出来事の行にした差 (video - audio)。出来事の行にした差がなければ undefined。 */
    readonly lastEventDiffUs: bigint | undefined;
}

export function createClockOriginLogState(): ClockOriginLogState {
    return { lastLoggedAtMs: undefined, lastVideoOriginUs: undefined, lastAudioOriginUs: undefined, lastEventDiffUs: undefined };
}

export interface ClockOriginLogResult {
    readonly state: ClockOriginLogState;
    /** ログのリングバッファーに追加する 1 行。値が変わっていないか、前の行から 1 秒たっていないときは undefined。 */
    readonly line: string | undefined;
    /** line を出来事の行にするか (true)、統計の行にするか (false)。line が undefined のときは false。 */
    readonly isEvent: boolean;
}

/** 同じストリームの行を書く最短の間隔 (ms)。 */
export const CLOCK_ORIGIN_LOG_INTERVAL_MS = 1_000;

/**
 * 出来事の行にする、映像と音声の原点の差 (video - audio。us) の動き。前に出来事の行にした差からこの値以上動いたときに、
 * 出来事の行にする。1 フレームの間隔 (60 fps で約 16.7 ms) より少し大きい 20 ms にした。目安の値で、測って決めたものでは
 * ない。これより小さい動きは、統計の行に書く。
 */
export const CLOCK_ORIGIN_EVENT_DIFF_US = 20_000n;

function abs(value: bigint): bigint {
    return value < 0n ? -value : value;
}

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
 * 出来事の行にするかは isEvent で返す (上の説明)。映像も音声もまだ決まっていないストリームは書かない。片方だけ
 * 決まっていれば、もう片方を none として書く。diff は video - audio で、両方が決まっているときだけ計算する。streamId はワイヤー形式の stream_id で、
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
        return { state, line: undefined, isEvent: false }; // 映像も音声も、まだ原点が決まっていない。
    }

    const changed = videoOriginUs !== state.lastVideoOriginUs || audioOriginUs !== state.lastAudioOriginUs;
    if (!changed) return { state, line: undefined, isEvent: false };

    if (state.lastLoggedAtMs !== undefined && nowMs - state.lastLoggedAtMs < CLOCK_ORIGIN_LOG_INTERVAL_MS) {
        return { state, line: undefined, isEvent: false }; // 1 秒たっていない。状態を変えずに、後の呼び出しに任せる。
    }

    const diffUs = videoOriginUs !== undefined && audioOriginUs !== undefined ? videoOriginUs - audioOriginUs : undefined;
    const line = `clock origin stream ${streamId}: video=${formatUs(videoOriginUs)} audio=${formatUs(audioOriginUs)} diff=${formatUs(diffUs)}`;

    // 出来事の行にするのは、このストリームの最初の行、差が初めて決まったとき、前に出来事の行にした差から大きく動いたとき。
    let isEvent = state.lastLoggedAtMs === undefined;
    if (diffUs !== undefined) {
        const moved = state.lastEventDiffUs === undefined || abs(diffUs - state.lastEventDiffUs) >= CLOCK_ORIGIN_EVENT_DIFF_US;
        if (moved) isEvent = true;
    }

    return {
        state: {
            lastLoggedAtMs: nowMs,
            lastVideoOriginUs: videoOriginUs,
            lastAudioOriginUs: audioOriginUs,
            lastEventDiffUs: isEvent && diffUs !== undefined ? diffUs : state.lastEventDiffUs,
        },
        line,
        isEvent,
    };
}
