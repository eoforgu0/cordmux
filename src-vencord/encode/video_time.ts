// SPDX-License-Identifier: Apache-2.0

// 受けた映像の VideoFrame.timestamp を送信側の時計に換算する純粋関数 (mapVideoTime)。ストリームごとに、タイムスタンプが
// WebRTC の描く予定の時刻 (render_time) かを見分け続け、そうなら見積もりなしに換算し (正確な換算)、そうでなければ
// 今までの換算 (encode/clock_map.ts) を使う。状態は呼び出し元 (encode/session.ts) がトラックごとに持つ。
//
// VideoFrame.timestamp の中身 (Chromium 156 の MediaStreamTrackProcessor。blink の breakout_box/
// frame_queue_underlying_source.cc と peerconnection/media_stream_remote_video_source.cc):
// - 描く予定の時刻 (render_time) の TimeTicks の値 (ms の格子)。受けた映像に取り込みの時刻が付かないとき。Discord の画面
//   共有を受ける接続は往復の時間を測らないので、こちらになる見込みである (Discord の配布物に、往復の時間を測る指定の
//   文字列 rcvr-rtt がない)。
// - 送った人の取り込みの時刻を、受けた側の時計に写した値 (TimeTicks の値)。受けた側が往復の時間を測れると付き、
//   ストリームの途中で描く予定の時刻から変わることがある。
// - 最初のフレームからの経過。すぐ描く指定のとき。
// 送信側の時計が qpc (encode/sender_clock.ts) のときだけ、TimeTicks の値をそのまま送信側の時計にできる。
//
// 見分け方。先行 = TimeTicks の値をそのまま送信側の時計にした時刻 - フレームを読んだ時刻。
// - 描く予定の時刻なら、WebRTC はデコードを終えたフレームを、描く予定の時刻より前 (描画の遅延 10 ms とデコードの時間の
//   見積もりの余り) に渡すので、すぐに読んだフレームの先行は正である。テスト用の送信側の試し (受けるだけの接続、
//   落ち着いた後) で、中央値 13.9 ms、1 パーセンタイル 5.4 ms。読むのが遅れたフレームだけが負になる。
// - 取り込みの時刻なら、フレームは取り込みの後に届くので、先行はどのフレームも負である (同じ試しの両方向の接続で
//   -35.5 ms から -16.2 ms)。
// - 経過やほかの時計の値なら、先行は秒の桁より大きく外れる。
// そこで、先行が VIDEO_EXACT_LEAD_MIN_US より大きく VIDEO_EXACT_LEAD_EVIDENCE_MAX_US 以下のフレームを、描く予定の時刻の
// 証拠にする (上限は受信側の表示の待ち行列が待つ長さ)。取り込みの時刻と経過のフレームは証拠にならないので、証拠のフレームが 1 つ来れば正確な換算に入る。
// 先行が秒の桁より外れるフレーム (経過かほかの時計の値) が来たら、すぐに今までの換算に戻る。そうでなくても、
// 正確な換算の間に、証拠のないフレームが VIDEO_EXACT_EXIT_FRAMES 個以上、読んだ時刻で VIDEO_EXACT_EXIT_US 以上続いたら、
// 描く予定の時刻でなくなったとみなして今までの換算に戻る。読むのが遅れただけなら、その間に証拠のフレームが来る。
// 戻るときは、clock_map の状態を作り直す (メディアの時計の意味が変わったので、前の定数は使えない)。正確な換算の間は
// clock_map にフレームを渡さない。

import {
    createClockMap,
    mapMediaTime,
    mediaUsOf,
    type ClockMapRestart,
    type ClockMapState,
} from "./clock_map";
import { fromTicksUs, type SenderTimebase } from "./sender_clock";

/**
 * 描く予定の時刻の証拠にする先行の下限 (us)。取り込みの時刻のフレームの先行は、取り込みから読むまでの時間の分だけ負で、
 * 換算の誤差 (タイムスタンプの 1 ms の格子と、ページの原点の TimeTicks の誤差の上限 1 ms。encode/ticks_origin.ts) を
 * 足しても 2 ms に届かない。描く予定の時刻のフレームは、すぐ読めば描画の遅延 (10 ms) ほど先行する。
 */
export const VIDEO_EXACT_LEAD_MIN_US = 2_000n;

/**
 * ほかの時計の値とみなす先行の大きさ (us)。先行がこれ以上 (正でも負でも) のフレームは、描く予定の時刻でも取り込みの
 * 時刻でもない (経過かほかの時計の値。起動からの時間の桁で離れる) とみなし、正確な換算の間でも 1 フレームで今までの換算に
 * 戻る。受信を始めた直後の描く予定の時刻は、届く時刻より 100 ms 以上先に出ることがある (docs/architecture.md の「送信側:
 * 送信側の時計への換算」) ので、それより十分大きくした。目安の値で、測って決めたものではない。
 */
export const VIDEO_EXACT_LEAD_MAX_US = 10_000_000n;

/**
 * 描く予定の時刻の証拠にする先行の上限 (us)。受信側の表示の待ち行列は、タイムスタンプが今より先にあるフレームを、この長さ
 * までしか待たない (src-obs/pipeline/frame_pacer.h の kFramePacerMaxAheadUs と同じ値。src-vencord/tests/
 * encode_shared_constants.test.ts が比べる)。これより先のフレームで正確な換算に入ると、受信側で超えた分だけ映像が早く出る
 * ので、入る証拠にしない。正確な換算の間に来たときは、証拠のないフレームとして数える。
 */
export const VIDEO_EXACT_LEAD_EVIDENCE_MAX_US = 500_000n;

/**
 * 正確な換算をやめるのに要る、証拠のないフレームの続いた数。画面が止まっていてフレームが少ないときに、間を空けて
 * 届いた数個のフレームだけでやめないためである。目安の値で、測って決めたものではない。
 */
export const VIDEO_EXACT_EXIT_FRAMES = 10;

/**
 * 正確な換算をやめるのに要る、証拠のないフレームの続いた長さ (到着の時刻で数える us)。描画が詰まった後にたまった
 * フレームを読むときの遅れは、読み終えればすぐに消える (encode/track_source.ts は最新の 1 枚だけを持つ)。1 秒たっても
 * 証拠のフレームが来なければ、詰まりではなくタイムスタンプの意味が変わったとみなす。encode/clock_map.ts の
 * CLOCK_MAP_JUMP_CONFIRM_US と同じ長さにした。
 */
export const VIDEO_EXACT_EXIT_US = 1_000_000n;

/** 換算の方法。exact は正確な換算、estimated は今までの換算 (encode/clock_map.ts)。 */
export type VideoTimeMode = "exact" | "estimated";

/**
 * 方法を決めた理由 (ログの行に書く)。
 * - render-time: 描く予定の時刻の証拠のフレームが来た。
 * - no-ticks-origin: 送信側の時計が page で、TimeTicks の値を送信側の時計にできない。
 * - not-render-time: 先行が描く予定の時刻の範囲にない (取り込みの時刻か、読むのが遅れ続けている)。
 * - too-far-ahead: 先行が、受信側が待つ長さ (VIDEO_EXACT_LEAD_EVIDENCE_MAX_US) を超え、10 秒より小さい。
 * - other-clock: 先行が 10 秒以上外れる (経過かほかの時計の値)。
 */
export type VideoTimeReason = "render-time" | "no-ticks-origin" | "not-render-time" | "too-far-ahead" | "other-clock";

export interface VideoTimeChange {
    readonly mode: VideoTimeMode;
    readonly reason: VideoTimeReason;
    /** そのフレームの先行 (us)。送信側の時計が page なら undefined。 */
    readonly leadUs: bigint | undefined;
}

/** ストリーム 1 つの状態。トラックが替わったら作り直す。 */
export interface VideoTimeState {
    /** 今までの換算の状態。正確な換算の間は動かさない。 */
    readonly clockMap: ClockMapState;
    /** 今の方法。まだ 1 フレームも換算していなければ undefined。 */
    readonly mode: VideoTimeMode | undefined;
    /** 正確な換算の間に、証拠のないフレームが続いた数と、続き始めた到着の時刻。 */
    readonly missFrames: number;
    readonly missSinceUs: bigint | undefined;
    /** 方法を替えた回数 (最初に決めた分は数えない)。 */
    readonly switches: number;
}

export interface VideoTimeResult {
    readonly state: VideoTimeState;
    /** 換算した時刻 (送信側の時計の us)。ワイヤー形式の timestamp_us に使う。 */
    readonly timeUs: bigint;
    /** 今までの換算が、不連続とみなして定数を決め直したときだけ値を持つ (encode/clock_map.ts)。 */
    readonly restart: ClockMapRestart | undefined;
    /** 方法を決めたか替えたときだけ値を持つ (最初のフレームでは必ず持つ)。 */
    readonly change: VideoTimeChange | undefined;
}

export function createVideoTime(): VideoTimeState {
    return { clockMap: createClockMap(), mode: undefined, missFrames: 0, missSinceUs: undefined, switches: 0 };
}

/** 証拠のないフレームの理由。 */
function missReason(leadUs: bigint): VideoTimeReason {
    if (leadUs <= -VIDEO_EXACT_LEAD_MAX_US || leadUs >= VIDEO_EXACT_LEAD_MAX_US) return "other-clock";
    return leadUs > VIDEO_EXACT_LEAD_EVIDENCE_MAX_US ? "too-far-ahead" : "not-render-time";
}

/**
 * 映像のフレーム 1 つのタイムスタンプを換算する。mediaTimestampUs は VideoFrame.timestamp、arrivalUs はフレームを読んだ
 * ときの送信側の時計 (us)。mediaTimestampUs が有限の数でなければ RangeError を投げる (encode/clock_map.ts と同じ)。
 */
export function mapVideoTime(
    state: VideoTimeState,
    timebase: SenderTimebase,
    mediaTimestampUs: number,
    arrivalUs: bigint,
): VideoTimeResult {
    const mediaUs = mediaUsOf(mediaTimestampUs);
    const exactUs = fromTicksUs(timebase, mediaUs);
    if (exactUs === undefined) {
        const mapped = mapMediaTime(state.clockMap, mediaTimestampUs, arrivalUs);
        return {
            state: { ...state, clockMap: mapped.state, mode: "estimated" },
            timeUs: mapped.timeUs,
            restart: mapped.restart,
            change: state.mode === undefined ? { mode: "estimated", reason: "no-ticks-origin", leadUs: undefined } : undefined,
        };
    }

    const leadUs = exactUs - arrivalUs;
    if (leadUs > VIDEO_EXACT_LEAD_MIN_US && leadUs <= VIDEO_EXACT_LEAD_EVIDENCE_MAX_US) {
        const entering = state.mode !== "exact";
        return {
            state: {
                ...state,
                mode: "exact",
                missFrames: 0,
                missSinceUs: undefined,
                switches: entering && state.mode !== undefined ? state.switches + 1 : state.switches,
            },
            timeUs: exactUs,
            restart: undefined,
            change: entering ? { mode: "exact", reason: "render-time", leadUs } : undefined,
        };
    }

    if (state.mode === "exact") {
        const missFrames = state.missFrames + 1;
        const missSinceUs = state.missSinceUs ?? arrivalUs;
        // 先行が秒の桁より外れる (経過かほかの時計の値) なら、待たずにすぐ戻る。読むのが遅れただけでは、先行が
        // VIDEO_EXACT_LEAD_MAX_US より外れることはない。待つ間にこのタイムスタンプを送ると、受信側でも送信側の古い
        // フレームを捨てる処理 (encode/session.ts の STALE_FRAME_SKIP_US) でも、ずっと先か過去のフレームとして扱われる。
        const otherClock = missReason(leadUs) === "other-clock";
        if (!otherClock && (missFrames < VIDEO_EXACT_EXIT_FRAMES || arrivalUs - missSinceUs < VIDEO_EXACT_EXIT_US)) {
            return { state: { ...state, missFrames, missSinceUs }, timeUs: exactUs, restart: undefined, change: undefined };
        }
        // 描く予定の時刻でなくなった。今までの換算を、このフレームから始め直す。
        const mapped = mapMediaTime(createClockMap(), mediaTimestampUs, arrivalUs);
        return {
            state: {
                clockMap: mapped.state,
                mode: "estimated",
                missFrames: 0,
                missSinceUs: undefined,
                switches: state.switches + 1,
            },
            timeUs: mapped.timeUs,
            restart: undefined,
            change: { mode: "estimated", reason: missReason(leadUs), leadUs },
        };
    }

    const mapped = mapMediaTime(state.clockMap, mediaTimestampUs, arrivalUs);
    return {
        state: { ...state, clockMap: mapped.state, mode: "estimated" },
        timeUs: mapped.timeUs,
        restart: mapped.restart,
        change: state.mode === undefined ? { mode: "estimated", reason: missReason(leadUs), leadUs } : undefined,
    };
}

/**
 * 今の換算の原点 (送信側の時計 - メディアのタイムスタンプ。us)。正確な換算では、TimeTicks の値をそのまま送信側の時計に
 * するので 0。今までの換算では clock_map の定数で、まだ 1 フレームも換算していなければ undefined。
 */
export function videoTimeOriginUs(state: VideoTimeState): bigint | undefined {
    return state.mode === "exact" ? 0n : state.clockMap.offsetUs;
}

/** 方法を決めたか替えたときの、ログの行 (英語) の本体。 */
export function videoTimeChangeText(change: VideoTimeChange): string {
    const lead = change.leadUs === undefined ? "" : ` lead=${(Number(change.leadUs) / 1000).toFixed(1)}ms`;
    return `time=${change.mode} reason=${change.reason}${lead}`;
}

/** 方法を替えたことを 1 行ずつ書く回数 (ストリームごと)。これを超えた分は、VIDEO_TIME_LOG_SUMMARY_MS ごとにまとめて書く。 */
export const VIDEO_TIME_LOG_FIRST_SWITCHES = 3;

/**
 * 方法を替えたことをまとめて書く間隔 (ms)。重い負荷で正確な換算を抜けて戻ることを繰り返しても、長い配信でログの行
 * (出来事の行は 5000 行まで) を押し出さないようにする。目安の値で、測って決めたものではない。
 */
export const VIDEO_TIME_LOG_SUMMARY_MS = 60_000;

/** 方法の行を書くかの、ストリームごとの状態。 */
export interface VideoTimeLogState {
    /** 方法を替えた回数 (最初に決めた分は数えない)。 */
    readonly switches: number;
    /** 前の行から、書かずに数えた替えた回数。 */
    readonly unlogged: number;
    /** まとめた行を前に書いた時刻 (ms)。 */
    readonly lastSummaryMs: number | undefined;
}

export function createVideoTimeLogState(): VideoTimeLogState {
    return { switches: 0, unlogged: 0, lastSummaryMs: undefined };
}

/**
 * 方法を決めたか替えたときに、ログの行を書くかを決める。最初に決めたときと、替えた最初の VIDEO_TIME_LOG_FIRST_SWITCHES 回は
 * 毎回書く。その後は、前にまとめた行から VIDEO_TIME_LOG_SUMMARY_MS たったときだけ、書かずに数えた回数を添えて書く。
 */
export function videoTimeLogLine(
    state: VideoTimeLogState,
    streamId: number,
    change: VideoTimeChange,
    switched: boolean,
    nowMs: number,
): { state: VideoTimeLogState; line: string | undefined } {
    const head = `video time stream ${streamId}: ${videoTimeChangeText(change)}`;
    if (!switched) return { state, line: head };
    const switches = state.switches + 1;
    if (switches <= VIDEO_TIME_LOG_FIRST_SWITCHES) return { state: { ...state, switches }, line: head };
    const unlogged = state.unlogged + 1;
    if (state.lastSummaryMs !== undefined && nowMs - state.lastSummaryMs < VIDEO_TIME_LOG_SUMMARY_MS) {
        return { state: { ...state, switches, unlogged }, line: undefined };
    }
    return {
        state: { switches, unlogged: 0, lastSummaryMs: nowMs },
        line: `${head} switches=${switches} (${unlogged} since the last line)`,
    };
}
