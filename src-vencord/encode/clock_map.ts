// SPDX-License-Identifier: Apache-2.0

// 映像と音声のメディアのタイムスタンプ (VideoFrame.timestamp と AudioData.timestamp。単位は us) を、送信側の時計
// (encode/sender_clock.ts。ワイヤー形式の timestamp_us の時計) に換算する純粋関数 (mapMediaTime)。映像 (encode/session.ts)
// と音声 (audio/chunk.ts) が同じ規則で使い、状態は呼び出し元がトラックごとに持つ。換算は「メディアのタイムスタンプ +
// トラックごとの定数」である。Chromium のソースでは、どちらのタイムスタンプも base::TimeTicks (Windows の単調な時計) から
// 作った値で、送信側の時計も同じ時計で進むので、2 つの時計の速さは同じである。ただし、映像のタイムスタンプと届く時刻の
// 関係は時間で変わる。映像の VideoFrame.timestamp は WebRTC が決めた描画の時刻 (render_time) で、描画の時刻はデコードの
// 時間の見積もり (直近 10 秒の 95 パーセンタイル。WebRTC の decode_time_percentile_filter.cc と frame_decode_timing.cc)
// を含むので、見積もりが変わると、同じように届いたフレームのタイムスタンプも動く。受信を始めた直後には、タイムスタンプが
// 届く時刻より 100 ms 以上先の値になることがある (見積もりが大きいためと考えられる)。そのため定数は、下げる向きにも
// 上げる向きにも動かす。定数の決め方は mapMediaTime のコメントにある。

/** 定数を下げるかを確かめる期間 (us)。期間ごとに、その期間に見た差の小さいほうから CLOCK_MAP_RANK 番目を数える。 */
export const CLOCK_MAP_WINDOW_US = 2_000_000n;

/** 定数を下げるのに要る、今の定数より小さい差の数 (期間の中の、小さいほうからの順位)。 */
export const CLOCK_MAP_RANK = 3;

/**
 * 定数を上げるかを決めるのに見る、終わった期間 (CLOCK_MAP_WINDOW_US) の数。直近のこの数の期間 (20 秒) のどれでも、
 * 期間の小さいほうから CLOCK_MAP_RANK 番目の差が今の定数より上なら、上げる。WebRTC のデコードの時間の見積もりは直近
 * 10 秒の値から決まるので、見積もりの変化で差が上がって見えるのは、続いても 10 秒ほどである。その 2 倍の長さにして、
 * それより短い変化と、JavaScript の数秒の詰まりでは上げない。長くするほど、受信を始めた直後に下がった分が消えるのが
 * 遅れる (下がった差が戻ってから、この長さと上げる時間がたつまで残る)。
 */
export const CLOCK_MAP_RAISE_WINDOWS = 10;

/**
 * 定数を上げる速さの上限 (送信側の時計の 1 秒あたりの us)。上げている間は、換算した時刻がメディアのタイムスタンプより
 * この割合 (1%) だけ速く進む。受信を始めた直後に 150 ms 下がった分は 15 秒で上げきり、差が戻ってから
 * CLOCK_MAP_RAISE_WINDOWS の 20 秒と合わせて、受信を始めてから 60 秒以内に戻る。受信側の整列
 * (src-obs/align/timestamp_aligner.h) は、周期の平均が 20% (relock_deviation) 以上ずれたときに再ロックするので、
 * 1% の違いでは再ロックしない。上げた分だけ表示が遅れるので、その分のフレームの重複 (150 ms なら 60 fps で
 * 9 フレーム) は、どの速さで上げても要る。遅くしても重複の量は変わらず、定数が戻るのが遅れる (5 ms なら 15 秒)
 * だけなので、10 ms にした。
 */
export const CLOCK_MAP_RAISE_RATE_US_PER_S = 10_000n;

/**
 * 1 回の呼び出しで定数を上げる量の上限 (us)。上げる量は前の呼び出しからの時間に比例させるので、フレームの間が空いた
 * (画面が止まっていてフレームが少ない、JavaScript が詰まった) 後の 1 回で大きく動かないようにする。上げる速さの上限
 * (CLOCK_MAP_RAISE_RATE_US_PER_S) で 1 ms に届くのは間が 100 ms のときなので、この上限が効くのは 10 fps より少ない
 * ときだけである。
 */
export const CLOCK_MAP_RAISE_STEP_MAX_US = 1_000n;

/**
 * 上げ始めるのに要る、目標 (直近の期間の CLOCK_MAP_RANK 番目の差の最小) と今の定数の差 (us)。上げ始めた後は、目標まで
 * 上げる。定数はそれまでに見た小さい差まで下がっているので、差の揺れだけでも目標は今の定数より少し上になる。この値が
 * ないと、揺れのたびに少し上げては下げることを続け、そのたびに原点の行 (encode/clock_origin_log.ts) を書く。2 ms は
 * 1 フレームの間隔 (60 fps で 16.7 ms) より十分小さく、上げずに残っても、映像と音声のずれとしては分からない。
 */
export const CLOCK_MAP_RAISE_MIN_US = 2_000n;

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
    /**
     * 終わった期間ごとの、小さいほうから CLOCK_MAP_RANK 番目の差 (古い順)。直近の CLOCK_MAP_RAISE_WINDOWS 個まで持つ。
     * 差が CLOCK_MAP_RANK 個に満たなかった期間は入れない。
     */
    readonly recentRanks: readonly bigint[];
    /** 定数を目標まで上げている途中か。 */
    readonly raising: boolean;
    /** 前の呼び出しの到着の時刻。上げる量を、ここからの時間で決める。 */
    readonly lastArrivalUs: bigint | undefined;
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
    /** 換算した時刻 (送信側の時計の us)。ワイヤー形式の timestamp_us に使う。常に メディアのタイムスタンプ + state.offsetUs。 */
    readonly timeUs: bigint;
    /** この呼び出しで不連続とみなして定数を決め直したときだけ値を持つ。 */
    readonly restart: ClockMapRestart | undefined;
}

/** 初期状態を作る。トラックが替わったときは作り直す (メディアの時計が替わるので、定数を決め直す)。 */
export function createClockMap(): ClockMapState {
    return {
        offsetUs: undefined,
        windowStartUs: undefined,
        smallest: [],
        recentRanks: [],
        raising: false,
        lastArrivalUs: undefined,
        jumpStartUs: undefined,
        restarts: 0,
    };
}

function startedAt(deltaUs: bigint, arrivalUs: bigint, restarts: number): ClockMapState {
    return {
        offsetUs: deltaUs,
        windowStartUs: arrivalUs,
        smallest: [deltaUs],
        recentRanks: [],
        raising: false,
        lastArrivalUs: arrivalUs,
        jumpStartUs: undefined,
        restarts,
    };
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

/** 期間の小さいほうから CLOCK_MAP_RANK 番目の差。まだ CLOCK_MAP_RANK 個ないときは undefined。 */
function rankOf(smallest: readonly bigint[]): bigint | undefined {
    return smallest.length >= CLOCK_MAP_RANK ? smallest[CLOCK_MAP_RANK - 1] : undefined;
}

/** 終わった期間の CLOCK_MAP_RANK 番目の差を足し、直近の CLOCK_MAP_RAISE_WINDOWS 個を返す。 */
function withRecentRank(recentRanks: readonly bigint[], rankUs: bigint | undefined): readonly bigint[] {
    if (rankUs === undefined) return recentRanks;
    const out = [...recentRanks, rankUs];
    if (out.length > CLOCK_MAP_RAISE_WINDOWS) out.splice(0, out.length - CLOCK_MAP_RAISE_WINDOWS);
    return out;
}

/**
 * 定数を上げる 1 回分を決める。直近の CLOCK_MAP_RAISE_WINDOWS 個の期間がそろっていて、その CLOCK_MAP_RANK 番目の差の
 * 最小 (目標) が今の定数より上なら、目標に向かって上げる。今の期間の CLOCK_MAP_RANK 番目の差 (currentRankUs。まだ
 * なければ undefined) のほうが小さければ、それを目標にする (今の期間で見た差より上には上げない)。上げ始めるのは、
 * 目標との差が CLOCK_MAP_RAISE_MIN_US 以上のときだけである。上げる量は elapsedUs (前の呼び出しからの時間) に
 * CLOCK_MAP_RAISE_RATE_US_PER_S を掛けた量で、CLOCK_MAP_RAISE_STEP_MAX_US と目標までの差を超えない。
 */
function raiseStep(
    offsetUs: bigint,
    raising: boolean,
    recentRanks: readonly bigint[],
    currentRankUs: bigint | undefined,
    elapsedUs: bigint,
): { offsetUs: bigint; raising: boolean } {
    if (recentRanks.length < CLOCK_MAP_RAISE_WINDOWS) return { offsetUs, raising: false };
    let targetUs = recentRanks[0] as bigint;
    for (const rankUs of recentRanks) if (rankUs < targetUs) targetUs = rankUs;
    if (currentRankUs !== undefined && currentRankUs < targetUs) targetUs = currentRankUs;
    const gapUs = targetUs - offsetUs;
    if (gapUs <= 0n || (!raising && gapUs < CLOCK_MAP_RAISE_MIN_US)) return { offsetUs, raising: false };
    let stepUs = elapsedUs > 0n ? (elapsedUs * CLOCK_MAP_RAISE_RATE_US_PER_S) / 1_000_000n : 0n;
    if (stepUs > CLOCK_MAP_RAISE_STEP_MAX_US) stepUs = CLOCK_MAP_RAISE_STEP_MAX_US;
    if (stepUs >= gapUs) return { offsetUs: targetUs, raising: false };
    return { offsetUs: offsetUs + stepUs, raising: true };
}

/**
 * メディアのタイムスタンプを送信側の時計に換算する。arrivalUs は、そのフレームか断片を受け取ったときの送信側の時計 (us)。
 * mediaTimestampUs が有限の数でなければ RangeError を投げる。
 *
 * 定数の決め方。定数は「到着の時刻 - メディアのタイムスタンプ」から決める。この差は、メディアの時計の原点と送信側の
 * 時計の原点の差に、取り出しの遅れ (JavaScript が受け取るまでの時間。詰まると増える) を足した値である。原点の差は、
 * 映像ではデコードの時間の見積もりが変わると動く (ファイルの先頭のコメント)。最初の到着の差を定数にし、その後は
 * 下げる向きにはすぐに、上げる向きにはゆっくり動かす。
 * - 下げるのは、CLOCK_MAP_WINDOW_US (2 秒) の期間の中で見た差の、小さいほうから CLOCK_MAP_RANK (3) 番目が、今の定数より
 *   小さいときで、その値まで下げる。取り出しの遅れは詰まると増える向きにだけ動くので、小さい差ほど原点の差に近い。
 *   起動の直後やトラックの途中に詰まっていた分は、後で詰まりのない到着が同じ期間に 3 つ来れば定数から消える。トラックの
 *   始めに決めた値で固めると、そのときの詰まりが、そのトラックの間ずっと残る (映像と音声は別々に決めるので、残った分は
 *   映像と音声のずれになる)。
 * - 3 つ要るのは、外れ値 (上流の描画の時刻が 1 フレームだけ遅い値を持つ、など) で定数が下がらないためである。同じ期間の
 *   外れ値が 2 つまでなら下がらない。最初の 1 つは比べる相手がないので、そのまま使う。
 * - 上げるのは、終わった期間ごとの 3 番目の差を直近 CLOCK_MAP_RAISE_WINDOWS 個 (20 秒) 見て、その最小 (目標) が今の定数より
 *   CLOCK_MAP_RAISE_MIN_US 以上大きいときで、目標まで少しずつ上げる (raiseStep)。差が上がって見えるのは、詰まりか、原点の
 *   差が上がったときである。詰まりで期間の 3 番目の差が上がるのは、期間のほとんどの到着が遅れたときだけで、20 秒のどの
 *   期間でも上がったままなら、原点の差が上がったとみなす。映像では、受信を始めた直後に大きかったデコードの時間の
 *   見積もりが、10 秒ほどで小さくなって起きる。上げないと、その前に下げた分が、そのトラックの間ずっと映像と音声のずれ
 *   として残る。段で上げると、その分が急なずれになって映像と音声の関係に入るので、少しずつ上げる。
 * 定数が下がったときは、返すタイムスタンプが前の値より小さくなることがある。受信側はこれを吸収する (src-obs/align/ の
 * 整列と追従)。
 *
 * 不連続。到着の時刻 - 換算した時刻 が CLOCK_MAP_JUMP_US 以上か -CLOCK_MAP_JUMP_US 以下の状態が、到着の時刻で
 * CLOCK_MAP_JUMP_CONFIRM_US 途切れずに続いたら、メディアの時計が替わったとみなし、その呼び出しの差から定数を決め直す
 * (restart に量を入れる)。差は、その呼び出しで下げるか上げた後の定数と比べる。早い向き (メディアのタイムスタンプが
 * 先へ飛んだ) は、3 つの到着で定数が下がるので、普通は不連続になる前に戻る。遅い向きは、上げるには 20 秒かかるので、
 * 先に決め直しになる。
 */
export function mapMediaTime(state: ClockMapState, mediaTimestampUs: number, arrivalUs: bigint): ClockMapResult {
    const mediaUs = mediaUsOf(mediaTimestampUs);
    const deltaUs = arrivalUs - mediaUs;
    if (state.offsetUs === undefined || state.windowStartUs === undefined) {
        const first = startedAt(deltaUs, arrivalUs, state.restarts);
        return { state: first, timeUs: mediaUs + deltaUs, restart: undefined };
    }

    // 期間が終わっていれば、その期間の 3 番目の差を上げるための記録に足し、次の期間を始める。今の期間の 3 番目が
    // 今の定数より小さければ下げ、そうでなければ上げるかを確かめる。
    const newWindow = arrivalUs - state.windowStartUs >= CLOCK_MAP_WINDOW_US;
    const recentRanks = newWindow ? withRecentRank(state.recentRanks, rankOf(state.smallest)) : state.recentRanks;
    const smallest = withSmallest(newWindow ? [] : state.smallest, deltaUs);
    const rankUs = rankOf(smallest);
    const elapsedUs = arrivalUs - (state.lastArrivalUs ?? arrivalUs);
    const moved =
        rankUs !== undefined && rankUs < state.offsetUs
            ? { offsetUs: rankUs, raising: false }
            : raiseStep(state.offsetUs, state.raising, recentRanks, rankUs, elapsedUs);
    const offsetUs = moved.offsetUs;
    let next: ClockMapState = {
        ...state,
        offsetUs,
        windowStartUs: newWindow ? arrivalUs : state.windowStartUs,
        smallest,
        recentRanks,
        raising: moved.raising,
        lastArrivalUs: arrivalUs,
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
