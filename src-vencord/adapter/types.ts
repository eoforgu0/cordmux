// SPDX-License-Identifier: Apache-2.0

// core と Discord の内部の間のインターフェイス (Adapter) と、その周りの型。Adapter は、core が使う事実の読み取りと、
// 視聴の開始と停止だけを持つ。実装は、Discord の内部を読む adapter/vencord.ts と、テスト用の adapter
// (tests/fake_adapter.ts) がある。Vencord のものは import しないので、core/ と同じく、リポジトリのルートの tsc と
// vitest でテストできる。

import type { ContactId } from "../ledger";
import type { CoreEvent } from "../core/events";

/** probe() の結果。見つからなかった理由は持たない。 */
export type ProbeResult = { found: true } | { found: false };

/**
 * adapter が core に送るイベント。core/events.ts の CoreEvent と同じ型で、adapter は事実だけを送り、
 * 判断は core が作る。
 */
export type AdapterEvent = CoreEvent;

/**
 * 実際の MediaStreamTrack が満たす最小の構造型。リポジトリのルートの tsc は lib に DOM を持たないので、DOM の型を
 * import せず、使う 4 つのフィールドだけを宣言する。実際の MediaStreamTrack はこの形を満たすので、
 * adapter/vencord.ts はキャストせずに渡せる。
 */
export interface MediaStreamTrackLike {
    readonly id: string;
    readonly kind: string;
    readonly readyState: "live" | "ended";
    /**
     * トラックの muted (Media Capture and Streams の MediaStreamTrack.muted)。テスト用のトラックは持たないことがあるので
     * 任意にする。読むのはログと、映像が届かないときの 2 つ目の手がかりだけである (index.ts の onStreamCheck)。
     */
    readonly muted?: boolean;
    stop(): void;
}

/**
 * Go Live の接続 (RTCPeerConnection) の状態。connectionState と iceConnectionState は WebRTC 1.0 の
 * RTCPeerConnectionState と RTCIceConnectionState の値をそのまま持つ。ログに書くためだけに使う。
 */
export interface PeerStateLike {
    readonly connectionState: string;
    readonly iceConnectionState: string;
}

/** track_added と track_removed が運ぶ種類 (映像か音声か)。 */
export type TrackKind = "video" | "audio";

/**
 * 実際の RTCStatsReport が満たす最小の構造型 (MediaStreamTrackLike と同じく、DOM の型を import しない)。
 * 読むのは forEach だけで、コールバックは値を 1 つだけ受け取る形にする。実際の forEach のコールバックは
 * (value, key, parent) を受け取るが、引数の少ない関数は多い関数の代わりに渡せるので、strict のまま代入できる
 * (メソッドの双変性には頼っていない)。使うのは encode/playout_diag.ts (受信の遅延を測ってログに書くだけ)。
 */
export interface StatsReportLike {
    forEach(callback: (value: unknown) => void): void;
}

/**
 * RTCPeerConnection の ontrack で届いた、映像と音声のトラックの追加と削除。トラックは事実ではなく映像と音声
 * そのものなので、CoreEvent には入れず、subscribeTracks() という別の経路で送る。pcId は adapter/connections.ts の
 * ConnectionTracker が割り当てる接続の番号で、sharing と ssrc の CoreEvent と同じ番号を使う。トラックの userId を
 * ssrc から特定するのは core/state.ts の userIdForSsrc() で、adapter では特定しない。
 */
export type TrackEvent =
    | {
        type: "track_added";
        pcId: number;
        kind: TrackKind;
        track: MediaStreamTrackLike;
        /** 呼んだ時点の RTCRtpReceiver.getSynchronizationSources() が返す ssrc の一覧。最初のパケットが届くまでは
         *  空の配列になることがあるので、値ではなく、呼び直せる関数にしてある。 */
        ssrcs(): number[];
        /** ontrack の streams[0].id。Discord では "<userId>-<ssrc>" の形で、確認した通話ではどのトラックもこの形
         *  だった。なければ null。トラックが自分で示す値なので、これだけで userId を特定せず、voice gateway の事実と
         *  一致したときだけ使う。 */
        msid: string | null;
        /** そのトラックのレシーバーの RTCRtpReceiver.getStats() を呼ぶ関数 (this を使わない)。受信の遅延を測って
         *  ログに書くためだけに使い、なくても (テスト用の adapter) 動作は変わらない。レシーバーそのものは渡さない。
         *  渡すと、adapter の外からレシーバーのほかの機能 (transport や jitterBufferTarget の書き込みなど) を
         *  操作できてしまい、adapter が事実を送るだけという境界が崩れる。 */
        getStats?(): Promise<StatsReportLike>;
        /** そのトラックが届いた接続の状態を読む関数 (this を使わない)。接続が捨てられていれば undefined を返す。
         *  映像が届かないときにログに書くためだけに使い、なくても (テスト用の adapter) 動作は変わらない。 */
        peerState?(): PeerStateLike | undefined;
    }
    | { type: "track_removed"; pcId: number; kind: TrackKind; trackId: string }
    /**
     * トラックの mute と unmute のイベント (Media Capture and Streams の MediaStreamTrack の mute と unmute)。WebRTC 1.0 は、
     * 相手が送るのをやめた (SDP の向きから受信がなくなった) トラックを mute にし、受信を再開したら unmute にする。RTP が
     * 一時的に途切れただけで mute になるかはブラウザーの実装による (Chromium で確認していない)。そのため、映像が届かない
     * 原因を分けるための手がかりとしてログに書くだけにし、判断は getStats() の受信の数で行う (index.ts の onStreamCheck)。
     */
    | { type: "track_mute"; pcId: number; kind: TrackKind; trackId: string; muted: boolean };

/** 視聴を始めた結果。視聴を始める関数か画面共有が見つからない、または関数が例外を投げたときは ok が false になる。 */
export interface StartResult {
    ok: boolean;
    /** ok が false のときの理由。ドックに表示する文言ではない。 */
    reason?: string;
}

/** snapshot() が返す、通話に参加している 1 人の事実。 */
export interface SnapshotRosterUser {
    userId: string;
    name: string;
    /**
     * Discord のユーザー名 (Discord の中で 1 人に 1 つ。旧来の 4 桁の discriminator を持つ人は name#1234)。
     * 読めなければ null で、適当な値で埋めない。受信側は、表示名が同じ人を見分けるために表示する。
     * 作るのは adapter/username.ts。
     */
    username: string | null;
    avatarUrl: string | null;
    /**
     * Discord の user.avatar の値 (アバターの画像が変わると変わる)。自分のアバターを設定していない人と、読めない人は
     * 空の文字列。受信側は、この値が変わったときだけアバターを差し替える。
     */
    avatarKey: string;
    /** Discord の user.bot。Bot のアカウントなら true。読めなければ false (人として出す側に倒す)。 */
    bot: boolean;
}

/**
 * snapshot() が返す、画面共有している 1 人の事実。core/events.ts の sharing イベントと同じ形 (pcId、active) に
 * してある。snapshot() が返すのは画面共有している人だけなので、active は常に true になる。
 */
export interface SnapshotSharingUser {
    userId: string;
    streamKey: string;
    /** その画面共有を運ぶ接続の番号。まだ視聴していなければ 0 (接続がないことを示す値)。 */
    pcId: number;
    active: boolean;
}

/**
 * roster、self、sharing の現在の値をまとめて読んだもの。index.ts は、表示名、ユーザー名、アバターの URL を読むために
 * 使う。
 */
export interface Snapshot {
    selfUserId: string | null;
    /**
     * Vesktop のアカウント自身の表示名とユーザー名など。通話の外でも読める。読めなければ null。受信側は、通話の外でも
     * サブアカウントの名前を表示するために、これを profiles に載せる。
     */
    selfUser: SnapshotRosterUser | null;
    roster: readonly SnapshotRosterUser[];
    sharing: readonly SnapshotSharingUser[];
}

/**
 * moveToMainCall() と leaveCall() の結果。実行できなかったときは ok が false で、reason に理由 (ログに書く文言で、
 * ドックに表示する文言ではない) を入れる。
 */
export interface CallActionResult {
    ok: boolean;
    reason?: string;
}

/**
 * core が Discord の事実を読み、視聴を始めて止めるためのインターフェイス。
 *
 * probe、subscribe、snapshot は読むだけで、Discord の状態を変えない。Discord の状態を変えるのは startWatching、
 * stopWatching、moveToMainCall、leaveCall だけである。startWatching と stopWatching は core の decisions() が出した
 * 判断に従って index.ts が呼ぶ。moveToMainCall と leaveCall は、利用者が受信側のドックで [移動] と [退出] を押した
 * ときだけ、index.ts が呼ぶ (接続、再接続、通話の変化では呼ばない)。
 */
export interface Adapter {
    /**
     * 依存箇所 1 つが今使えるかを確認する。見つからなかった結果はキャッシュしない。Discord のモジュールは後から
     * 読み込まれることがあり、キャッシュすると、後で使えるようになっても見つからないままになる。
     */
    probe(id: ContactId): Promise<ProbeResult>;

    /** Flux などで届いた Discord のイベントを CoreEvent に変えて、listener に送る。戻り値は購読をやめる関数。 */
    subscribe(listener: (event: AdapterEvent) => void): () => void;

    /**
     * 視聴を始める。Discord の視聴を始める関数には forceMultiple: true と noFocus: true を必ず渡す。渡さないと、
     * Discord がほかの視聴を閉じ、その人の映像が配信から消える。
     */
    startWatching(userId: string, streamKey: string): Promise<StartResult>;

    /**
     * 視聴をやめる。視聴をやめる関数が見つからなくても、見つかるまで探し直す。探すのをやめると、視聴が続いた
     * まま止められず、受信とデコードの負荷が残る。
     */
    stopWatching(userId: string, streamKey: string): Promise<void>;
    /**
     * Go Live の接続 (pcId) で画面共有している人の userId。その接続の voice gateway が op12 (まだなければ op5) で
     * 伝えた user_id である。ボイス接続、閉じた接続、分からないときは undefined を返す。index.ts は Go Live の
     * トラックの userId を特定するときに、SSRC の対応より先にこれを使う (Go Live の接続 1 つで画面共有している人は
     * 1 人だけ)。テスト用の adapter は実装しなくてよい。
     */
    ownerOfConnection?(pcId: number): string | undefined;

    /** roster、self、sharing の現在の値をまとめて読む。 */
    snapshot(): Promise<Snapshot>;

    /**
     * メインアカウントの userId を伝える。null か空の文字列は、メインアカウントを使わない (または分からない)。
     * 値が変わったら、メインアカウントがいる通話を読み直して、main_call のイベントを送る。
     */
    setMainUser(userId: string | null): void;

    /**
     * Vesktop のアカウントを、いまメインアカウントがいる通話へ移動する。呼ばれた時点のメインアカウントの通話を
     * 読み直して決める (引数で移動先を受け取らない)。メインアカウントが見える通話にいない、参加できない、通話を移る
     * 関数が見つからないときは、移動せずに理由を返す。
     */
    moveToMainCall(): CallActionResult;

    /** Vesktop のアカウントを、現在の通話から退出させる。通話を移る関数が見つからないときは、理由を返す。 */
    leaveCall(): CallActionResult;

    /**
     * ontrack で届いた映像と音声のトラックの追加と削除を、listener に送る。トラックは事実ではないので、
     * subscribe() の AdapterEvent とは別の経路にする。戻り値は購読をやめる関数。
     */
    subscribeTracks(listener: (event: TrackEvent) => void): () => void;
}
