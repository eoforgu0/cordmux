// SPDX-License-Identifier: Apache-2.0

// H.264 の SPS と PPS をキーフレームに必ず入れて送るための純粋関数 (applyParameterSetInsurance)。キーフレームの
// アクセスユニット (Annex B) に SPS か PPS がなければ、同じエンコーダで最後に見た SPS と PPS を前に付ける。
// デコードは SPS と PPS を取り逃しても次のキーフレームで回復するが、受信側の録画は、セグメントの最初の IDR に
// 入っている SPS と PPS から avcC を作るので、ないとそのセグメントを書き始められない。WebCodecs のエンコーダが
// キーフレームごとに SPS と PPS を入れるかは確認していない。WebCodecs も Vencord も使わない。

/**
 * エンコーダのインスタンス 1 つで最後に見た SPS と PPS (開始コードを除いた NAL の中身)。呼び出し元はインスタンス
 * ごとに 1 つ持つ。ほかのインスタンスの SPS を付けると、解像度の違う SPS から受信側が avcC を作り、avcC と中身の
 * 解像度が食い違う。
 */
export interface ParameterSets {
    readonly sps: Uint8Array | undefined;
    readonly pps: Uint8Array | undefined;
}

/** まだ 1 つも見ていない状態。 */
export const NO_PARAMETER_SETS: ParameterSets = { sps: undefined, pps: undefined };

export interface ParameterSetResult {
    /** 実際に送るバイト列 (前に付けなかったときは引数の `au` がそのまま返る)。 */
    readonly payload: Uint8Array;
    /** 次の呼び出しに渡す、最後に見た SPS と PPS。 */
    readonly remembered: ParameterSets;
    /** この呼び出しで SPS と PPS を前に付けたか。呼び出し元はこの回数を facts の sps_pps_prepended に入れる。 */
    readonly prepended: boolean;
    /** キーフレームに SPS か PPS がなく、付ける SPS と PPS もまだなかったか。 */
    readonly missing: boolean;
}

const NAL_TYPE_SPS = 7;
const NAL_TYPE_PPS = 8;
const NAL_TYPE_MASK = 0x1f;
const START_CODE = new Uint8Array([0, 0, 0, 1]);

interface StartCode {
    /** 開始コードそのものの先頭 (前の NAL の終わりを表す位置)。 */
    readonly start: number;
    /** 開始コードの直後 (NAL の中身の先頭)。 */
    readonly payloadStart: number;
}

/** `from` 以降で最初の開始コード (00 00 01 または 00 00 00 01) を探す。 */
function findStartCode(au: Uint8Array, from: number): StartCode | undefined {
    for (let i = from; i + 2 < au.length; i++) {
        if (au[i] !== 0 || au[i + 1] !== 0) continue;
        if (au[i + 2] === 1) return { start: i, payloadStart: i + 3 };
        if (au[i + 2] === 0 && au[i + 3] === 1) return { start: i, payloadStart: i + 4 };
    }
    return undefined;
}

/**
 * [start, end) を、末尾の 0 を除いてコピーする。末尾の 0 は次の開始コードの一部としても読めるので、除いておく
 * (受信側の走査が、同じ NAL を違う長さで読まないため)。中身が空なら undefined。
 */
function copyNal(au: Uint8Array, start: number, end: number): Uint8Array | undefined {
    let last = end;
    while (last > start && au[last - 1] === 0) last--;
    if (last <= start) return undefined;
    return au.slice(start, last);
}

/** アクセスユニットを走査し、最後に現れた SPS と PPS を取り出す (なければ undefined)。 */
export function scanParameterSets(au: Uint8Array): ParameterSets {
    let sps: Uint8Array | undefined;
    let pps: Uint8Array | undefined;
    let current = findStartCode(au, 0);
    while (current !== undefined) {
        const next = findStartCode(au, current.payloadStart);
        const end = next === undefined ? au.length : next.start;
        const header = au[current.payloadStart];
        if (header !== undefined) {
            const type = header & NAL_TYPE_MASK;
            if (type === NAL_TYPE_SPS || type === NAL_TYPE_PPS) {
                const nal = copyNal(au, current.payloadStart, end);
                if (nal !== undefined) {
                    if (type === NAL_TYPE_SPS) sps = nal;
                    else pps = nal;
                }
            }
        }
        current = next;
    }
    return { sps, pps };
}

/**
 * キーフレームのアクセスユニットに SPS か PPS がなければ、remembered の SPS と PPS を前に付けて返す。前に付けるとき
 * は 4 バイトの開始コード (00 00 00 01) を付ける。キーフレームでないアクセスユニットは、走査せずにそのまま返す。
 * SPS と PPS がキーフレーム以外に現れることは H.264 では許されるが、WebCodecs のエンコーダがそう出す理由がなく、
 * 毎フレーム走査すると 4 ストリーム x 60 fps の走査が続く。取り逃しても missing として数えるので、気付かないまま
 * 壊れることはない。
 *
 * 片方だけがなくても、SPS と PPS を両方とも前に付ける。ないほうだけを付けると PPS, SPS, ..., IDR の順になることが
 * あり、PPS を読むのに先に SPS が必要なデコーダーがある。両方付けると入っていたほうが重複するが、同じエンコーダの
 * インスタンスでは SPS と PPS は変わらないので同じバイト列の重複であり、H.264 は同じ SPS と PPS の繰り返しを許す。
 */
export function applyParameterSetInsurance(
    au: Uint8Array,
    isKeyframe: boolean,
    remembered: ParameterSets,
): ParameterSetResult {
    if (!isKeyframe) {
        return { payload: au, remembered, prepended: false, missing: false };
    }

    const found = scanParameterSets(au);
    const next: ParameterSets = {
        sps: found.sps ?? remembered.sps,
        pps: found.pps ?? remembered.pps,
    };
    if (found.sps !== undefined && found.pps !== undefined) {
        return { payload: au, remembered: next, prepended: false, missing: false };
    }

    const { sps, pps } = next;
    if (sps === undefined || pps === undefined) {
        // 付ける SPS と PPS がまだない (そのインスタンスの最初の IDR に SPS か PPS がなかった)。送信側では修正できない
        // ので、missing を返して呼び出し元に数えさせる。
        return { payload: au, remembered: next, prepended: false, missing: true };
    }

    const prefixBytes = START_CODE.length * 2 + sps.length + pps.length;
    const payload = new Uint8Array(prefixBytes + au.length);
    let offset = 0;
    payload.set(START_CODE, offset);
    offset += START_CODE.length;
    payload.set(sps, offset);
    offset += sps.length;
    payload.set(START_CODE, offset);
    offset += START_CODE.length;
    payload.set(pps, offset);
    offset += pps.length;
    payload.set(au, offset);
    return { payload, remembered: next, prepended: true, missing: false };
}
