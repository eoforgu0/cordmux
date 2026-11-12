// SPDX-License-Identifier: Apache-2.0

// 送信のキューがたまったときに、映像のフレームを送るか捨てるかを決める純粋関数 (背圧)。WebSocket の
// bufferedAmount (まだ送っていないバイト数) を読むのは、呼び出し元の transport/socket.ts である。

/** メッセージの種類。音声は、送る待ちが DEFAULT_AUDIO_CAP_BYTES を超えるまで送る。 */
export type FrameKind = "video" | "audio";

/**
 * bufferedAmount のしきい値の既定値 (バイト)。1 MiB は、エンコードした映像を約 10 MB/s で送るときの約 0.1 秒分に
 * 当たる。目安の値で、測って決めたものではない。呼び出し元は applyBackpressure() の thresholdBytes で変えられる。
 */
export const DEFAULT_BACKPRESSURE_THRESHOLD_BYTES = 1_048_576;

/**
 * 音声も捨て始める bufferedAmount (バイト)。音声は 1 本あたり約 384 KB/s (32 ビット浮動小数点、2 チャンネル、48 kHz。
 * audio/chunk.ts) で、声 8 本と画面共有の音を合わせると 1 秒に数 MB になる。16 MiB は、その約 3 秒分に当たる。ここまで
 * たまるのは受信側 (OBS) が止まったときで、たまった音は配信に使えないので、Vesktop のメモリを使い続けないように
 * 捨てる。目安の値で、測って決めたものではない。
 */
export const DEFAULT_AUDIO_CAP_BYTES = 16 * 1_048_576;

/** 背圧の状態。捨てた映像のフレームの数と、キーフレームを待って捨て続けているかを持つ。 */
export interface BackpressureState {
    readonly droppedVideoFrames: number;
    /** 送る待ちが DEFAULT_AUDIO_CAP_BYTES を超えていて捨てた音声のメッセージの数。 */
    readonly droppedAudioMessages: number;
    /** フレームを捨ててから、キーフレームが届いて送るのを再開するまでの間 true。 */
    readonly droppingUntilKeyframe: boolean;
}

/**
 * 背圧の状態を初期状態で作る。ストリームごとに持つか、接続全体で 1 つ持つかは呼び出し元が決める
 * (transport/socket.ts は接続全体で 1 つ持つ)。
 */
export function createBackpressureState(): BackpressureState {
    return { droppedVideoFrames: 0, droppedAudioMessages: 0, droppingUntilKeyframe: false };
}

export interface BackpressureDecision {
    state: BackpressureState;
    /** このメッセージを今送ってよいか。false なら、呼び出し元は送らずに捨てる。 */
    send: boolean;
}

/**
 * bufferedAmount がしきい値を超えているかと、キーフレームを待っているかから、このメッセージを送ってよいかを
 * 決める。音声 (kind === "audio") は、bufferedAmount が audioCapBytes を超えるまで送る。音声のデータは映像よりずっと
 * 小さく、捨てると音が途切れるためである。audioCapBytes を超えるのは受信側が止まったときだけである
 * (DEFAULT_AUDIO_CAP_BYTES)。bufferedAmount がしきい値と等しいときは、超えていないので送る。
 *
 * 映像は、しきい値を超えている間はキーフレームでも送らない。キーフレームは P フレームより大きく、たまったキューに
 * 積むと、さらに詰まる。一度捨て始めたら、しきい値以下に戻っても、次のキーフレームが届くまで捨て続ける
 * (droppingUntilKeyframe)。GOP の途中の P フレームだけを捨てて残りを送っても、受信側のデコーダーは参照先が欠けて
 * デコードに失敗するので、帯域の無駄になるためである。捨てるのをやめるのは、しきい値以下のときにキーフレームが
 * 届いたときだけである。映像の乱れは、受信側からのキーフレームの要求 (keyframe_request) か、エンコーダが一定の
 * 間隔で入れるキーフレームで回復する。
 *
 * bufferedAmount か thresholdBytes が負の数か、有限の数でなければ RangeError を投げる。
 */
export function applyBackpressure(
    state: BackpressureState,
    kind: FrameKind,
    bufferedAmount: number,
    thresholdBytes: number = DEFAULT_BACKPRESSURE_THRESHOLD_BYTES,
    isKeyframe: boolean = false,
    audioCapBytes: number = DEFAULT_AUDIO_CAP_BYTES,
): BackpressureDecision {
    if (!Number.isFinite(bufferedAmount) || bufferedAmount < 0) {
        throw new RangeError(
            `transport/backpressure: bufferedAmount は0以上の有限な数にしてください: ${bufferedAmount}`,
        );
    }
    if (!Number.isFinite(thresholdBytes) || thresholdBytes < 0) {
        throw new RangeError(
            `transport/backpressure: thresholdBytes は0以上の有限な数にしてください: ${thresholdBytes}`,
        );
    }

    if (kind === "audio") {
        if (bufferedAmount > audioCapBytes) {
            return { state: { ...state, droppedAudioMessages: state.droppedAudioMessages + 1 }, send: false };
        }
        return { state, send: true };
    }

    if (bufferedAmount > thresholdBytes || (state.droppingUntilKeyframe && !isKeyframe)) {
        return {
            state: { ...state, droppedVideoFrames: state.droppedVideoFrames + 1, droppingUntilKeyframe: true },
            send: false,
        };
    }

    // しきい値以下で、捨てていなかったか、キーフレームが届いた。
    return { state: { ...state, droppingUntilKeyframe: false }, send: true };
}
