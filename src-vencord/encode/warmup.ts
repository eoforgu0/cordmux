// SPDX-License-Identifier: Apache-2.0

// エンコーダのウォームアップ (黒いフレームをエンコードして捨ててから、映像のエンコードを始める) の計画と、
// 次のフレームがウォームアップの途中かの判定。ウォームアップをすると、エンコードを始めた直後の欠落が
// 35 フレームほどから 5 フレームほどに減った。WebCodecs は使わない。黒いフレームを作って VideoEncoder に
// 渡すのは encode/session.ts である。

/**
 * ウォームアップでエンコードして捨てるフレームの数の既定値。目安の値で、測って決めたものではない
 * (測ったのは、ウォームアップをする場合としない場合の違いだけ)。
 */
export const DEFAULT_WARMUP_FRAME_COUNT = 60;

export interface WarmupPlan {
    readonly totalFrames: number;
}

/**
 * ウォームアップの計画を作る。totalFrames が負か整数でなければ RangeError を投げる。
 * 0 は、ウォームアップをしない計画になる。
 */
export function createWarmupPlan(totalFrames: number = DEFAULT_WARMUP_FRAME_COUNT): WarmupPlan {
    if (!Number.isInteger(totalFrames) || totalFrames < 0) {
        throw new RangeError(`encode/warmup: totalFrames は0以上の整数にしてください: ${totalFrames}`);
    }
    return { totalFrames };
}

/** ウォームアップの進み具合。remaining は、終わるまでにあと何フレームをエンコードするか。 */
export interface WarmupState {
    readonly remaining: number;
}

/** 計画から、ウォームアップを始める前の状態を作る。 */
export function startWarmup(plan: WarmupPlan): WarmupState {
    return { remaining: plan.totalFrames };
}

/** ウォームアップが残っているか。残っていれば、次にエンコードするフレームは捨てる黒いフレームである。 */
export function isWarming(state: WarmupState): boolean {
    return state.remaining > 0;
}

export interface WarmupStep {
    state: WarmupState;
    /** このフレームのエンコード結果を捨てるか (ウォームアップの途中は常に true)。 */
    discardOutput: boolean;
    /** この呼び出しでウォームアップが終わったか (この後は実際の映像を送ってよい)。 */
    justCompleted: boolean;
}

/**
 * 黒いフレームを 1 つエンコードするたびに呼ぶ。ウォームアップが終わった後に呼ばれたとき (呼び出し元の誤り) は、
 * 状態を変えずに discardOutput: false を返す。捨てないほうを既定にしておけば、呼び続けても実際の映像を捨てない。
 */
export function consumeWarmupFrame(state: WarmupState): WarmupStep {
    if (state.remaining <= 0) {
        return { state, discardOutput: false, justCompleted: false };
    }
    const remaining = state.remaining - 1;
    return { state: { remaining }, discardOutput: true, justCompleted: remaining === 0 };
}
