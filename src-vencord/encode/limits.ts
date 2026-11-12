// SPDX-License-Identifier: Apache-2.0

// 画質の上限 (EncodeSessionLimits) の形と、それを扱う純粋関数。上限は、長辺の上限 (maxLongEdge) と、fps を
// 半分にするか (fpsDivisor) の 2 つで、受信側が limits の制御用の JSON で送る。encode/session.ts、index.ts、
// テスト用の送信側 (test/tools/harness/sender.ts) が同じ関数を使い、ワイヤー形式からの変換と、
// 下げた上限だけを走っているストリームに適用する規則を 1 か所で決める。

import type { LimitsFields } from "../transport/control";

/**
 * 画質の上限。OBS の配信のエンコードと資源を取り合わないように、送信側のエンコードを軽くするために使う。
 * maxLongEdge がなければ長辺の上限はない。fpsDivisor が 1 なら間引かず、2 なら fps を半分にする。
 */
export interface EncodeSessionLimits {
    maxLongEdge?: number;
    fpsDivisor: 1 | 2;
}

/** 上限なしで間引かない、既定の上限。limits を送らない古い受信側と接続したときも、この値で動く。 */
export const NO_LIMITS: EncodeSessionLimits = { fpsDivisor: 1 };

/**
 * 上限の形を確認し、正しくなければ RangeError を投げる。意味を持たない値のまま、エンコーダの確保へ進ませない。
 * encode/session.ts の createEncodeSession と setLimits が呼ぶ。ワイヤー形式で届いた値は、範囲外のメッセージを
 * transport/control.ts が捨てているので、ここで投げるのは呼び出し元の誤りのときだけである。
 */
export function assertValidLimits(limits: EncodeSessionLimits): void {
    if (limits.fpsDivisor !== 1 && limits.fpsDivisor !== 2) {
        throw new RangeError(`encode/limits: fpsDivisor must be 1 or 2: ${String(limits.fpsDivisor)}`);
    }
    // maxLongEdge は、ない (上限なし) か正の数だけを受け付ける。0 以下や NaN のまま進むと、
    // downscaleToLongSide が意味を持たないサイズを返す。
    const { maxLongEdge } = limits;
    if (maxLongEdge !== undefined && (!Number.isFinite(maxLongEdge) || maxLongEdge <= 0)) {
        throw new RangeError(`encode/limits: maxLongEdge must be a positive number: ${maxLongEdge}`);
    }
}

/**
 * ワイヤー形式の limits (limits のメッセージと hello_ok の limits) を EncodeSessionLimits に変換する。
 * fps_divisor がなければ 1、max_long_edge がなければ上限なしにする。値の範囲は transport/control.ts が
 * 確認してあるので、ここでは確認しない。
 */
export function limitsFromWire(fields: LimitsFields): EncodeSessionLimits {
    const fpsDivisor = fields.fps_divisor ?? 1;
    return fields.max_long_edge === undefined ? { fpsDivisor } : { maxLongEdge: fields.max_long_edge, fpsDivisor };
}

/**
 * 走っているストリームに適用してよい上限を返す。current はそのストリームが今使っている上限、requested は
 * 新しく届いた上限。キーごとに厳しいほう (長辺は小さいほう、fpsDivisor は大きいほう) を取るので、下げたキー
 * だけが動作し、上げたキーは current のままになる。長辺を下げて fps を戻すように、下げと上げが 1 つの
 * メッセージに混ざっていても、下げたキーは捨てない。上げた上限は、次の画面共有で作るセッションから使う
 * (受信側の操作で、続いている画面共有の画質を上げないため)。
 */
export function tightenLimits(current: EncodeSessionLimits, requested: EncodeSessionLimits): EncodeSessionLimits {
    const fpsDivisor: 1 | 2 = current.fpsDivisor === 2 || requested.fpsDivisor === 2 ? 2 : 1;
    const a = current.maxLongEdge;
    const b = requested.maxLongEdge;
    const maxLongEdge = a === undefined ? b : b === undefined ? a : Math.min(a, b);
    return maxLongEdge === undefined ? { fpsDivisor } : { maxLongEdge, fpsDivisor };
}

/** 2 つの上限が同じかを返す。setLimits が、適用する上限が変わらないときに何もしないために使う。 */
export function sameLimits(a: EncodeSessionLimits, b: EncodeSessionLimits): boolean {
    return a.fpsDivisor === b.fpsDivisor && a.maxLongEdge === b.maxLongEdge;
}

/**
 * エンコードの設定 (EncodeSessionParams の preference と latencyMode) の組。受信側の設定ウィンドウで選んだ値が limits の
 * encoder_preference と latency_mode で届く。画質の上限と違い、走っているセッションには適用ず、次に作る
 * セッションから使う (確保済みのエンコーダの種類と latencyMode は、確保し直さないと変わらないため)。
 */
export interface EncodeChoice {
    preference: "prefer-hardware" | "prefer-software";
    latencyMode: "realtime" | "quality";
}

/** 何も届いていないとき (limits を送らない古い受信側を含む) のエンコードの設定。ハードウェアを先に試し、速さを優先する。 */
export const DEFAULT_ENCODE_CHOICE: EncodeChoice = { preference: "prefer-hardware", latencyMode: "realtime" };

/**
 * ワイヤー形式の limits からエンコードの設定を作る。encoder_preference がなければハードウェア、latency_mode がなければ
 * realtime にする。ソフトウェアを選んだときは、はしごのどの段階もソフトウェアで試す (encode/session.ts)。
 */
export function encodeChoiceFromWire(fields: LimitsFields): EncodeChoice {
    return {
        preference: fields.encoder_preference === "software" ? "prefer-software" : "prefer-hardware",
        latencyMode: fields.latency_mode === "quality" ? "quality" : "realtime",
    };
}

/** 段階が決まったときのログの行。どのエンコードの設定で確保したかを、受信側のログから読めるようにする。 */
export function stageLogLine(
    streamId: number,
    stage: number,
    format: { width: number; height: number; fps: number } | undefined,
    choice: EncodeChoice,
): string {
    const size = format ? `${format.width}x${format.height}@${format.fps}` : "none";
    return `video stream ${streamId}: stage=${stage} format=${size} preference=${choice.preference} latencyMode=${choice.latencyMode}`;
}
