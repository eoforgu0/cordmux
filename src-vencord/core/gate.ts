// SPDX-License-Identifier: Apache-2.0

// トラックの読み出し (MediaStreamTrackProcessor を作って読むこと) とエンコードを許すゲート。許すのは、受信側との
// 接続が確立していて、受信側が media の制御用の JSON で許している間だけで、閉じたら読み出しを全部止める。
// planGate() が始めるトラックと止めるトラックを決める純粋関数で、createMediaGate() は状態を持ってそれを呼び、結果を
// 呼び出し元 (index.ts) の関数 (MediaGateHands) に渡す。読み出しを始める処理と止める処理の中身は index.ts にある。
// Vencord のものは import しないので、vitest でテストできる。

import type { MediaStreamTrackLike, PeerStateLike, StatsReportLike, TrackKind } from "../adapter/types";

/** ゲートの 2 つの入力。 */
export interface GateInputs {
    /** 受信側との接続が確立しているか (transport/socket.ts の onEstablished から onClosed まで)。 */
    readonly established: boolean;
    /** 受信側が読み出しとエンコードを許しているか (制御用の JSON の media)。古い受信側は送らないので true のまま。 */
    readonly media: boolean;
}

/** media の既定値。media を送らない古い受信側と接続したときは、開いたまま動く。 */
export const DEFAULT_MEDIA = true;

/** 読み出しとエンコードを許すか (確立していて、media が true)。 */
export function gateOpen(inputs: GateInputs): boolean {
    return inputs.established && inputs.media;
}

/** 保存したトラックの 1 行のうち、planGate() が見る部分。 */
export interface GateTrackState {
    readonly track: Pick<MediaStreamTrackLike, "readyState">;
}

/**
 * 保存したトラックの 1 行 (adapter/types.ts の track_added が運ぶもの)。adapter/vencord.ts は ontrack のときに
 * track_added を 1 回送るだけで、生きているトラックを後から送り直さない。そのため、ゲートが閉じている間に届いた
 * トラックや、閉じて止めたトラックを、開いたときにもう一度読み出せるように、track_removed が来るか readyState が
 * "ended" になるまで、トラックをゲートに保存する。トラックは track id で区別する。画面共有の映像と音声は同じ
 * stream_id を使うが、読み出しはトラックごとに作るので、始める単位と止める単位もトラックにする。
 */
export interface LedgerTrack extends GateTrackState {
    readonly pcId: number;
    readonly kind: TrackKind;
    readonly track: MediaStreamTrackLike;
    /** ssrc の一覧を返す関数 (track_added の ssrcs。呼び直せる。this を使わない)。 */
    readonly ssrcs: () => number[];
    /** トラックの msid (track_added の msid)。ゲートは読まずに、呼び出し元に渡すだけ。 */
    readonly msid?: string | null;
    /** レシーバーの getStats() (track_added の getStats。受信の遅延を測ってログに書くためだけに使う)。
     *  ゲートは呼ばずに、呼び出し元に渡すだけ。 */
    readonly getStats?: () => Promise<StatsReportLike>;
    /** 接続の状態を読む関数 (track_added の peerState。映像が届かないときにログに書くためだけに使う)。
     *  ゲートは呼ばずに、呼び出し元に渡すだけ。 */
    readonly peerState?: () => PeerStateLike | undefined;
}

/**
 * 読み出しを止める理由。index.ts は "disconnected" のときだけ、送り先のない黒埋め用のフレームを作らないようにする。
 * 1 つのトラックが複数に当てはまるときは、track-ended、disconnected、media-off の順に先のものにする。
 */
export type StopReason = "track-ended" | "disconnected" | "media-off";

/**
 * 読み出しを始めるトラックと止めるトラック。並びは、start が保存した順、stop が始めた順 (Map の挿入順) で、
 * テストの結果を安定させるためのものであり、意味は持たない。
 */
export interface GatePlan {
    readonly start: readonly string[];
    readonly stop: readonly { readonly trackId: string; readonly reason: StopReason }[];
}

function isLive(entry: GateTrackState | undefined): boolean {
    return entry !== undefined && entry.track.readyState === "live";
}

/**
 * 保存したトラック (ledger: track id -> 行)、確立、media と、今読み出しているトラック (running) から、始める
 * トラックと止めるトラックを決める。始めるのは、ゲートが開いていて、保存してあり、生きていて、まだ読み出して
 * いないトラック。止めるのは、読み出しているトラックのうち、そのどれかを満たさなくなったトラック。同じトラックが
 * 両方に入ることはない。
 */
export function planGate<T extends GateTrackState>(
    ledger: ReadonlyMap<string, T>,
    inputs: GateInputs,
    running: Iterable<string>,
): GatePlan {
    const open = gateOpen(inputs);
    const runningSet = new Set(running);
    const stop: { trackId: string; reason: StopReason }[] = [];
    for (const trackId of runningSet) {
        if (!isLive(ledger.get(trackId))) {
            stop.push({ trackId, reason: "track-ended" });
        } else if (!inputs.established) {
            stop.push({ trackId, reason: "disconnected" });
        } else if (!inputs.media) {
            stop.push({ trackId, reason: "media-off" });
        }
    }
    const start: string[] = [];
    if (open) {
        for (const [trackId, entry] of ledger) {
            if (isLive(entry) && !runningSet.has(trackId)) start.push(trackId);
        }
    }
    return { start, stop };
}

/** ゲートが呼ぶ、呼び出し元の関数 (実装は index.ts)。 */
export interface MediaGateHands {
    /** このトラックの読み出し (とエンコード) を始める。token は isCurrent() に渡す世代の番号。 */
    start(track: LedgerTrack, token: number): void;
    /** このトラックの読み出し (とエンコード) を止める。始めたトラックなら、userId を特定している途中でも呼ぶ。 */
    stop(trackId: string, reason: StopReason): void;
    /** 開閉が変わったときに、start と stop を呼び終えた後で 1 回呼ぶ。 */
    onOpenChanged?(open: boolean): void;
}

export interface MediaGate {
    /** track_added を受け取る。同じ id のトラックが保存してあれば何もしない。 */
    trackAdded(track: LedgerTrack): void;
    /** track_removed を受け取る。保存から消し、読み出していれば止める。 */
    trackRemoved(trackId: string): void;
    /**
     * 保存したトラックの readyState を読み直して、決め直す。RTCPeerConnection.close() は、レシーバーのトラックの
     * readyState を ended にするが、ended のイベントを送らない (WebRTC 1.0)。そのため、終わったと分かったときに呼ぶ。
     */
    refresh(): void;
    /** 接続が確立した (onEstablished)。 */
    connected(): void;
    /** 接続が切れた (onClosed)。読み出しを全部止め、media を既定値に戻す。 */
    disconnected(): void;
    /** 受信側の media (と hello_ok の初期値) を受け取る。値が変わったら true を返す。 */
    setMedia(on: boolean): boolean;
    /** 現在の media (facts に入れる値)。 */
    media(): boolean;
    /** 読み出しとエンコードを許しているか。 */
    isOpen(): boolean;
    /** 確立しているか (connected() から disconnected() までの間だけ true)。 */
    isConnected(): boolean;
    /**
     * token がこのトラックの現在の世代か。userId を特定する処理は最長 5 秒かかる非同期の処理で、その間にゲートが
     * 閉じて開き直すと、同じトラックについて古い処理と新しい処理が並ぶ。呼び出し元は処理が終わったときにこれを
     * 確認し、古い世代の処理からは何も作らない。
     */
    isCurrent(trackId: string, token: number): boolean;
    /** 今読み出しているトラックの数 (ログに書くため)。 */
    runningCount(): number;
}

/**
 * ゲートを作る。どのトラックを始めて止めるかは planGate() が決め、ここは状態を持って hands を呼ぶ。hands の中から
 * このゲートの関数をまた呼んでもよい (外側の呼び出しが、変わらなくなるまで決め直す)。
 */
export function createMediaGate(hands: MediaGateHands): MediaGate {
    const ledger = new Map<string, LedgerTrack>();
    /** track id -> 世代の番号。 */
    const running = new Map<string, number>();
    let established = false;
    let media = DEFAULT_MEDIA;
    let lastToken = 0;
    let reconciling = false;
    let again = false;
    /** 最後に onOpenChanged に伝えた開閉 (最初は、確立していないので閉じている)。 */
    let lastOpen = false;

    const inputs = (): GateInputs => ({ established, media });

    function step(): void {
        const plan = planGate(ledger, inputs(), running.keys());
        for (const { trackId, reason } of plan.stop) {
            running.delete(trackId);
            hands.stop(trackId, reason);
        }
        // 終わったトラックは生き返らないので、保存から消す (track_removed が来なかったときのため。
        // adapter/vencord.ts は ended を監視できなくても接続を止めない)。
        for (const [trackId, entry] of ledger) {
            if (!isLive(entry)) ledger.delete(trackId);
        }
        for (const trackId of plan.start) {
            const entry = ledger.get(trackId);
            if (entry === undefined) continue;
            lastToken++;
            running.set(trackId, lastToken);
            hands.start(entry, lastToken);
        }
    }

    /**
     * hands の中からゲートがまた呼ばれても (入れ子の呼び出し)、外側の呼び出しが、変わらなくなるまで step() を
     * 繰り返す。
     */
    function reconcile(): void {
        if (reconciling) {
            again = true;
            return;
        }
        reconciling = true;
        try {
            do {
                again = false;
                step();
            } while (again);
        } finally {
            reconciling = false;
        }
        const nowOpen = gateOpen(inputs());
        if (nowOpen !== lastOpen) {
            lastOpen = nowOpen;
            hands.onOpenChanged?.(nowOpen);
        }
    }

    return {
        trackAdded(track: LedgerTrack): void {
            if (ledger.has(track.track.id)) return;
            ledger.set(track.track.id, track);
            reconcile();
        },
        trackRemoved(trackId: string): void {
            ledger.delete(trackId);
            reconcile();
        },
        refresh(): void {
            reconcile();
        },
        connected(): void {
            established = true;
            reconcile();
        },
        disconnected(): void {
            established = false;
            // 切れるたびに既定値に戻す。次に接続する受信側が media を送らない古いものだと、前の接続の false を
            // 持ち越して何も送らなくなる。新しい受信側の初期値は hello_ok にあって、確立の通知より先に届く
            // (transport/socket.ts) ので、確立のときではなく、切れたときに戻す。
            media = DEFAULT_MEDIA;
            reconcile();
        },
        setMedia(on: boolean): boolean {
            if (media === on) return false;
            media = on;
            reconcile();
            return true;
        },
        media(): boolean {
            return media;
        },
        isOpen(): boolean {
            return gateOpen(inputs());
        },
        isConnected(): boolean {
            return established;
        },
        isCurrent(trackId: string, token: number): boolean {
            return running.get(trackId) === token;
        },
        runningCount(): number {
            return running.size;
        },
    };
}
