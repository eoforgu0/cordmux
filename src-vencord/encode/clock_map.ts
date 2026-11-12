// SPDX-License-Identifier: Apache-2.0

// 映像と音声のメディアのタイムスタンプ (VideoFrame.timestamp と AudioData.timestamp。単位は us) を、送信側の時計
// (encode/sender_clock.ts。ワイヤー形式の timestamp_us の時計) に換算する純粋関数 (mapMediaTime)。映像 (encode/session.ts)
// と音声 (audio/chunk.ts) が同じ規則で使い、状態は呼び出し元がトラックごとに持つ。換算は「メディアのタイムスタンプ +
// トラックごとの定数」である。Chromium のソースでは、どちらのタイムスタンプも base::TimeTicks (Windows の単調な時計) の
// 差か値で、送信側の時計も同じ時計で進むので、定数は時間で変わらない (Vesktop の Chromium のバージョンでの原点は確認して
// いないが、どちらの原点でも定数は変わらない)。定数の決め方は mapMediaTime のコメントにある。

/** 定数を下げるかを確かめる期間 (us)。期間ごとに、その期間に見た差の小さいほうから CLOCK_MAP_RANK 番目を数える。 */
export const CLOCK_MAP_WINDOW_US = 2_000_000n;

/** 定数を下げるのに要る、今の定数より小さい差の数 (期間の中の、小さいほうからの順位)。 */
export const CLOCK_MAP_RANK = 3;

/**
 * 不連続とみなす差 (us)。到着の時刻 - 換算した時刻 が、この値以上 (遅い向き) か、この値の符号を変えた値以下 (早い向き) の
 * 状態が CLOCK_MAP_JUMP_CONFIRM_US 続いたら、メディアの時計が替わったとみなして定数を決め直す。遅すぎる映像のフレームを
 * 捨てるしきい値 (encode/session.ts の STALE_FRAME_SKIP_US) と同じ 200 ms にする。同じ値でないと、捨て続けているのに
 * 決め直さない区間か、捨てていないのに決め直す区間ができる。
 */
export const CLOCK_MAP_JUMP_US = 200_000n;

/**
 * 不連続とみなすまでに、差が CLOCK_MAP_JUMP_US を超えた状態が途切れずに続く長さ (到着の時刻で数える us)。描画が詰まった後に
 * たまったフレームや断片をまとめて読んだときの遅れは、読み終えればすぐに消える (映像は encode/track_source.ts が最新の
 * 1 枚だけを持ち、音声は MediaStreamTrackProcessor の待ち行列の分だけである)。1 秒たっても差が戻らなければ、詰まりではなく
 * メディアの時計そのものが替わったとみなす。映像が止まったとみなす受信側の 2 秒 (settings の video_stall_ms) より短くし、
 * 受信側が「映像が届いていません」を表示する前に戻るようにした。目安の値で、測って決めたものではない。
 */
export const CLOCK_MAP_JUMP_CONFIRM_US = 1_000_000n;

/** メディアの時計と送信側の時計の対応。呼び出し元がトラックごとに持つ。 */
export interface ClockMapState {
    /** 今使っている定数 (送信側の時計 - メディアのタイムスタンプ)。まだ 1 度も換算していなければ undefined。 */
    readonly offsetUs: bigint | undefined;
    /** 今の期間 (CLOCK_MAP_WINDOW_US) が始まった到着の時刻。 */
    readonly windowStartUs: bigint | undefined;
    /** 今の期間に見た「到着の時刻 - メディアのタイムスタンプ」の、小さいほうから CLOCK_MAP_RANK 個 (昇順)。 */
    readonly smallest: readonly bigint[];
    /** 差が CLOCK_MAP_JUMP_US を超えた状態が始まった到着の時刻。超えていなければ undefined。 */
    readonly jumpStartUs: bigint | undefined;
    /** 不連続で定数を決め直した回数。 */
    readonly restarts: number;
}

/** 不連続で定数を決め直したときの値 (ClockMapResult の restart)。 */
export interface ClockMapRestart {
    /** 定数が動いた量 (後 - 前)。正は、タイムスタンプが後ろへ動いたことを示す。 */
    readonly shiftUs: bigint;
    /** 差がしきい値を超えた状態が続いた長さ (到着の時刻で数える)。 */
    readonly jumpForUs: bigint;
}

export interface ClockMapResult {
    readonly state: ClockMapState;
    /** 換算した時刻 (送信側の時計の us)。ワイヤー形式の timestamp_us に使う。 */
    readonly timeUs: bigint;
    /** この呼び出しで不連続とみなして定数を決め直したときだけ値を持つ。 */
    readonly restart: ClockMapRestart | undefined;
}

/** 初期状態を作る。トラックが替わったときは作り直す (メディアの時計が替わるので、定数を決め直す)。 */
export function createClockMap(): ClockMapState {
    return { offsetUs: undefined, windowStartUs: undefined, smallest: [], jumpStartUs: undefined, restarts: 0 };
}

function startedAt(deltaUs: bigint, arrivalUs: bigint, restarts: number): ClockMapState {
    return { offsetUs: deltaUs, windowStartUs: arrivalUs, smallest: [deltaUs], jumpStartUs: undefined, restarts };
}

/** 昇順の配列に値を入れ、小さいほうから CLOCK_MAP_RANK 個を返す。 */
function withSmallest(smallest: readonly bigint[], value: bigint): bigint[] {
    const out = [...smallest];
    let i = out.length;
    while (i > 0 && (out[i - 1] as bigint) > value) i--;
    out.splice(i, 0, value);
    if (out.length > CLOCK_MAP_RANK) out.length = CLOCK_MAP_RANK;
    return out;
}

/**
 * メディアのタイムスタンプを送信側の時計に換算する。arrivalUs は、そのフレームか断片を受け取ったときの送信側の時計 (us)。
 * mediaTimestampUs が有限の数でなければ RangeError を投げる。
 *
 * 定数の決め方。定数は「到着の時刻 - メディアのタイムスタンプ」から決める。この差は、メディアの時計の原点と送信側の
 * 時計の原点の差 (時間で変わらない) に、取り出しの遅れ (JavaScript が受け取るまでの時間。詰まると増える) を足した値で
 * ある。最初の到着の差を定数にし、その後は下げる向きにだけ動かす。
 * - 下げるのは、CLOCK_MAP_WINDOW_US (2 秒) の期間の中で見た差の、小さいほうから CLOCK_MAP_RANK (3) 番目が、今の定数より
 *   小さいときで、その値まで下げる。取り出しの遅れは詰まると増える向きにだけ動くので、小さい差ほど原点の差に近い。
 *   起動の直後やトラックの途中に詰まっていた分は、後で詰まりのない到着が同じ期間に 3 つ来れば定数から消える。トラックの
 *   始めに決めた値で固めると、そのときの詰まりが、そのトラックの間ずっと残る (映像と音声は別々に決めるので、残った分は
 *   映像と音声のずれになる)。
 * - 3 つ要るのは、外れ値 (上流の描画の時刻が 1 フレームだけ遅い値を持つ、など) で定数が下がらないためである。同じ期間の
 *   外れ値が 2 つまでなら下がらない。最初の 1 つは比べる相手がないので、そのまま使う。
 * - 上げる向きには、下の不連続のときにしか動かさない。2 つの時計はどちらも同じ単調な時計で進み、本当の差は時間で
 *   変わらないので、差が上がって見えるのは詰まりである。詰まりで定数を上げると、その分が段になって映像と音声の関係に入る。
 * 定数が下がったときは、返すタイムスタンプが前の値より小さくなることがある。受信側はこれを吸収する (src-obs/align/ の
 * 整列と追従)。
 *
 * 不連続。到着の時刻 - 換算した時刻 が CLOCK_MAP_JUMP_US 以上か -CLOCK_MAP_JUMP_US 以下の状態が、到着の時刻で
 * CLOCK_MAP_JUMP_CONFIRM_US 途切れずに続いたら、メディアの時計が替わったとみなし、その呼び出しの差から定数を決め直す
 * (restart に量を入れる)。差は、その呼び出しで下げた後の定数と比べる。早い向き (メディアのタイムスタンプが先へ飛んだ) は、
 * 3 つの到着で定数が下がるので、普通は不連続になる前に戻る。
 */
export function mapMediaTime(state: ClockMapState, mediaTimestampUs: number, arrivalUs: bigint): ClockMapResult {
    const mediaUs = mediaUsOf(mediaTimestampUs);
    const deltaUs = arrivalUs - mediaUs;
    if (state.offsetUs === undefined || state.windowStartUs === undefined) {
        const first = startedAt(deltaUs, arrivalUs, state.restarts);
        return { state: first, timeUs: mediaUs + deltaUs, restart: undefined };
    }

    // 期間が終わっていれば、次の期間を始める。期間の中の差を数え、3 番目が今の定数より小さければ下げる。
    const newWindow = arrivalUs - state.windowStartUs >= CLOCK_MAP_WINDOW_US;
    const smallest = withSmallest(newWindow ? [] : state.smallest, deltaUs);
    const rankUs = smallest.length >= CLOCK_MAP_RANK ? smallest[CLOCK_MAP_RANK - 1] : undefined;
    const offsetUs = rankUs !== undefined && rankUs < state.offsetUs ? rankUs : state.offsetUs;
    let next: ClockMapState = {
        ...state,
        offsetUs,
        windowStartUs: newWindow ? arrivalUs : state.windowStartUs,
        smallest,
    };

    const lagUs = deltaUs - offsetUs; // 到着の時刻 - 換算した時刻
    if (lagUs >= CLOCK_MAP_JUMP_US || lagUs <= -CLOCK_MAP_JUMP_US) {
        const jumpStartUs = state.jumpStartUs ?? arrivalUs;
        if (arrivalUs - jumpStartUs >= CLOCK_MAP_JUMP_CONFIRM_US) {
            const restarted = startedAt(deltaUs, arrivalUs, state.restarts + 1);
            return {
                state: restarted,
                timeUs: mediaUs + deltaUs,
                restart: { shiftUs: deltaUs - offsetUs, jumpForUs: arrivalUs - jumpStartUs },
            };
        }
        return { state: { ...next, jumpStartUs }, timeUs: mediaUs + offsetUs, restart: undefined };
    }
    if (next.jumpStartUs !== undefined) next = { ...next, jumpStartUs: undefined };
    return { state: next, timeUs: mediaUs + offsetUs, restart: undefined };
}

/**
 * メディアのタイムスタンプを、整数の us にする。有限の数でなければ RangeError を投げる。メディアのタイムスタンプは整数の
 * us だが、型 (number) は小数を許す。BigInt() は小数で例外を投げるので、丸める。
 */
export function mediaUsOf(mediaTimestampUs: number): bigint {
    if (!Number.isFinite(mediaTimestampUs)) {
        throw new RangeError(`encode/clock_map: メディアのタイムスタンプが有限の数ではありません: ${mediaTimestampUs}`);
    }
    return BigInt(Math.round(mediaTimestampUs));
}
