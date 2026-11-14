// SPDX-License-Identifier: Apache-2.0

// サブアカウントの Vesktop で、画面共有の音を鳴らさないかを決める。メインアカウントとサブアカウントの運用で、利用者が
// メインアカウントの Discord で同じ通話を聞いているときは、Vesktop から鳴る画面共有の音が二重になる。そのときだけ、
// Vesktop の中の画面共有の接続の音量を 0 にする (実行は adapter/stream_volume.ts)。cordmux が受け取るトラックは
// 変わらない。shouldSilenceStreams() が決める純粋関数で、streamSilenceInputs() が index.ts の値から入力を作り、
// StreamSilenceSwitch が、受信側から届いたメインアカウントを持ち、値が変わったときだけ adapter に伝える。Vencord のものは import しないので、vitest で
// テストできる。

import type { LimitsFields } from "../transport/control";

/** shouldSilenceStreams() の入力。 */
export interface StreamSilenceInputs {
    /** 受信側の設定 (settings.json の silence_sub_stream_audio。制御用の JSON の limits の同じ名前のキー)。 */
    readonly enabled: boolean;
    /** 受信側との接続が確立していて、受信側が media で読み出しを許しているか (core/gate.ts の gateOpen)。 */
    readonly gateOpen: boolean;
    /** 受信側から届いたメインアカウントの userId。メインアカウントを使わない、または分からないときは null。 */
    readonly mainUserId: string | null;
    /** Vesktop のアカウントの userId。分からなければ null。 */
    readonly selfUserId: string | null;
    /**
     * Vesktop のアカウントがいる通話の参加者。"unknown" は、まだ読めていないこと (core/state.ts の CoreState.roster)。
     */
    readonly roster: "unknown" | readonly { readonly userId: string }[];
}

/**
 * Vesktop の画面共有の音を鳴らさないか。次のすべてが成り立つときだけ true を返す。
 *
 * - 受信側の設定が ON で、受信側と接続していて、受信側が読み出しを許している (cordmux が動いていないときは、Vesktop は
 *   元どおりに鳴る)。
 * - メインアカウントとサブアカウントの運用である。メインアカウントが分かっていて、Vesktop のアカウントも分かっていて、
 *   2 つが違う。Vesktop のアカウントが分からない間は false にする (受信側の has_sub_account() は分からない間も
 *   メインアカウントの userId だけで決めるが、ここでは、メインアカウントが Vesktop に入る運用で音を消さないことを先にする)。
 * - メインアカウントが Vesktop のアカウントと同じ通話にいる (参加者の一覧に入っている)。いないときに消すと、利用者は
 *   どこでも画面共有の音を聞けなくなる。一覧が読めていない間は false にする。
 */
export function shouldSilenceStreams(inputs: StreamSilenceInputs): boolean {
    if (!inputs.enabled || !inputs.gateOpen) return false;
    const main = inputs.mainUserId;
    const self = inputs.selfUserId;
    if (main === null || main === "" || self === null || self === "" || main === self) return false;
    if (inputs.roster === "unknown") return false;
    return inputs.roster.some((user) => user.userId === main);
}

/** streamSilenceInputs() が読む、index.ts の値。 */
export interface StreamSilenceSources {
    /** 受信側から届いた limits (ワイヤー形式)。silence_sub_stream_audio を省いたときは ON。 */
    readonly limits: LimitsFields;
    /** core/gate.ts のゲートが開いているか (受信側と接続していて、media が true)。 */
    readonly gateOpen: boolean;
    /** core の状態のうち、Vesktop のアカウントと参加者の一覧。 */
    readonly state: { readonly selfUserId: string | null; readonly roster: StreamSilenceInputs["roster"] };
}

/** index.ts の値と、受信側から届いたメインアカウントの userId から、shouldSilenceStreams() の入力を作る。 */
export function streamSilenceInputs(sources: StreamSilenceSources, mainUserId: string | null): StreamSilenceInputs {
    return {
        enabled: sources.limits.silence_sub_stream_audio !== false,
        gateOpen: sources.gateOpen,
        mainUserId,
        selfUserId: sources.state.selfUserId,
        roster: sources.state.roster,
    };
}

/**
 * shouldSilenceStreams() の結果を、変わったときだけ apply (Adapter.setStreamSilence) に渡し、ログに 1 行書く。掛けている
 * 間の置き直し (新しい画面共有、Discord が値を入れ直したとき) は adapter が自分で行うので、ここは変わったときだけ伝える。
 *
 * 受信側から届いたメインアカウントの userId (main_account) もここで持つ。受信側と切れたら (connectionClosed) 忘れる。
 * 受信側は確立のたびに送り直すので、つなぎ直した直後に、前の接続のメインアカウントで決めないためである。
 */
export class StreamSilenceSwitch {
    private on = false;
    private mainUserId: string | null = null;

    constructor(
        private readonly apply: (on: boolean) => void,
        private readonly log: (line: string) => void,
    ) {}

    /** 受信側から届いたメインアカウントの userId を受け取る。空の文字列は、メインアカウントを使わない構成。 */
    setMainUser(userId: string | null): void {
        this.mainUserId = userId === null || userId === "" ? null : userId;
    }

    /** 受信側と切れた。メインアカウントを忘れる (決め直しは、続けて呼ぶ update が行う)。 */
    connectionClosed(): void {
        this.mainUserId = null;
    }

    /** 今の値から決め直し、変わっていれば伝える。 */
    update(sources: StreamSilenceSources): void {
        const next = shouldSilenceStreams(streamSilenceInputs(sources, this.mainUserId));
        if (next === this.on) return;
        this.on = next;
        this.log(`stream audio: ${next ? "silencing" : "not silencing"} screen shares in Vesktop`);
        this.apply(next);
    }

    /** プラグインを止めるときに呼ぶ。掛けていたかにかかわらず、adapter に戻させる。 */
    stop(): void {
        if (this.on) this.log("stream audio: not silencing screen shares in Vesktop");
        this.on = false;
        this.apply(false);
    }

    /** 今掛けているか。 */
    isOn(): boolean {
        return this.on;
    }
}
