// SPDX-License-Identifier: Apache-2.0

// Discord と Vencord の内部から事実を読み、CoreEvent に変えて送る Adapter の実装 (createVencordAdapter)。判断
// (見つからないときに何を表示するか、誰を視聴するか) は持たず、core に任せる。@webpack などの Vencord の import を
// 使うので、tsconfig.json の exclude でリポジトリのルートの tsc から外し、Vencord の pnpm test:ts でチェックする
// (src-vencord/README.md)。StartAt.Init で Discord のバンドルより先に動く前提で書く。Discord の内部への依存箇所は
// ledger.ts の一覧の 9 つで、docs/architecture.md の「Discord の内部への依存箇所」に説明がある。

import { find } from "@webpack";
import {
    ApplicationStreamingStore,
    ChannelStore,
    FluxDispatcher,
    GuildMemberStore,
    GuildStore,
    PermissionsBits,
    PermissionStore,
    RTCConnectionStore,
    UserStore,
    VoiceStateStore,
} from "@webpack/common";

import type { MainCallInfo } from "../core/events";
import { trackProcessorAvailable } from "../core/track_processor";
import type { ContactId } from "../ledger";
import { callNameOf, joinableOf } from "./call_info";
import { ConnectionTracker, parseGatewayFrame } from "./connections";
import { avatarUrlOfUser } from "./default_avatar";
import { planProbeCycle, planSelfEmit, type SelfRead, shouldEmitRecheck } from "./probe_cycle";
import { uniqueUsername } from "./username";
import type {
    Adapter,
    AdapterEvent,
    CallActionResult,
    PeerStateLike,
    ProbeResult,
    Snapshot,
    SnapshotRosterUser,
    SnapshotSharingUser,
    StartResult,
    TrackEvent,
    TrackKind,
} from "./types";

/* ==========================================================================
   pcId の割り当て
   ========================================================================== */

/*
 * pcId を決め、Go Live の接続で画面共有している人を特定するのは adapter/connections.ts の ConnectionTracker で、
 * ここは実際の WebSocket と RTCPeerConnection のイベントを渡すだけにする。視聴を始める直前に、次にできる接続を
 * その人に割り当てる方法は取らない。視聴の途中でボイス接続が作り直されると、ほかの接続まで取り違え、人の画面共有を
 * 別の人の名前で配信に出すためである。
 */

/**
 * 接続の作成と終了、voice gateway のメッセージの要点を、1 行ずつ受け取る関数。省くと捨てる。どの接続がいつでき、どの
 * voice gateway が何を伝えたかを診断情報に残し、接続と人の対応付けを事実から確認できるようにする。書くのは
 * op12、op5、op0 の決まったフィールドだけで、op4 の secret_key と SDP、op0 の token と session_id、ICE の ufrag は
 * 書かない。
 */
type DiagSink = (line: string, alert?: boolean) => void;

/**
 * 現在の通話の server id。guild id を返し、guild id がない (DM とグループ通話) ときは channel id を返す。サーバーの
 * ボイスチャンネルでは、voice gateway の op0 の server_id は、ボイス接続ならそのサーバーの guild id、Go Live なら
 * 画面共有ごとに違う id になる。DM とグループ通話で、ボイス接続の server_id が channel id になるかは確認していない。
 * 読めなければ null を返し、ConnectionTracker は分類を待って試し直す。
 */
function currentCallServerId(): string | null {
    try {
        const guildId = RTCConnectionStore.getGuildId();
        if (typeof guildId === "string" && guildId !== "") return guildId;
        const channelId = RTCConnectionStore.getChannelId();
        return typeof channelId === "string" && channelId !== "" ? channelId : null;
    } catch {
        return null;
    }
}

/** RTCSessionDescription から SDP の文字列を読む (読めなければ null)。 */
function sdpOf(description: RTCSessionDescription | null): string | null {
    try {
        return typeof description?.sdp === "string" ? description.sdp : null;
    } catch {
        return null;
    }
}

/* ==========================================================================
   pc-hook: RTCPeerConnection の差し替え
   ========================================================================== */

/**
 * RTCPeerConnection を差し替え、できた接続と届いたトラックを ConnectionTracker に渡す。差し替えられなければ
 * pc-hook の missing を送って null を返す。戻り値はグローバルを元に戻す関数。RTCPeerConnection の差し替えは、
 * この 1 つだけにする。ConnectionTracker は 1 つの接続を pcCreated() で 1 回だけ知らされる前提で、差し替えを
 * 重ねると、同じ接続を 2 回数える。
 */
function installPeerConnectionHook(
    tracker: ConnectionTracker,
    emit: (event: AdapterEvent) => void,
    emitTrack: (event: TrackEvent) => void,
    diag: DiagSink,
): (() => void) | null {
    const native = globalThis.RTCPeerConnection;
    if (typeof native !== "function") {
        emit({ type: "contact", id: "pc-hook", result: "missing" });
        return null;
    }

    class Hooked extends native {
        /** ConnectionTracker での通し番号 (メッセージられなかったときは 0)。 */
        #serial = 0;

        constructor(config?: RTCConfiguration) {
            super(config);
            try {
                // この接続を ConnectionTracker に 1 回だけ伝える (ontrack も、この差し替えの中で受け取る)。pcId は
                // ここでは決めず、どの voice gateway と対応付けるかは、トラックが届いたときの ICE の ufrag で
                // ConnectionTracker が決める。SDP を読む関数は RTCPeerConnection を WeakRef で参照する (対応付けの
                // 決まらない情報が、捨てられた RTCPeerConnection を残し続けないため)。
                const pcRef = new WeakRef(this as RTCPeerConnection);
                const serial = tracker.pcCreated(() => {
                    const pc = pcRef.deref();
                    return {
                        remote: sdpOf(pc?.remoteDescription ?? null),
                        local: sdpOf(pc?.localDescription ?? null),
                    };
                });
                this.#serial = serial;
                // ログに書くためだけに受け取る。close() では connectionstatechange が届かない (WebRTC の仕様) ので、
                // 閉じたときの後始末は下の close() で行う。failed と disconnected は、映像が届かなくなる原因になり
                // うるので、異常に関わる行として書く (受信側は OBS のログにも書く)。
                this.addEventListener("connectionstatechange", () => {
                    try {
                        const state = this.connectionState;
                        diag(
                            `pc state: pc=${serial} state=${state} ice=${this.iceConnectionState}`,
                            state === "failed" || state === "disconnected",
                        );
                    } catch {
                        // ログに書けなくても、接続は動かす。
                    }
                });
                // 映像が届かないときに、トラックの接続の状態をログに書くための関数。RTCPeerConnection は WeakRef で
                // 参照する (捨てられた接続を残し続けないため)。
                const peerState = (): PeerStateLike | undefined => {
                    const pc = pcRef.deref();
                    if (pc === undefined) return undefined;
                    return { connectionState: pc.connectionState, iceConnectionState: pc.iceConnectionState };
                };
                this.addEventListener("track", (ev: RTCTrackEvent) => {
                    try {
                        const track = ev.track;
                        tracker.trackArrived(serial, {
                            kind: track.kind,
                            trackId: track.id,
                            msid: ev.streams[0]?.id ?? null,
                            isEnded: () => track.readyState === "ended",
                            // 行き先が決まったら (保持していたなら後で) 1 回だけ呼ばれる。
                            deliver: (pcId: number) => handleTrack(pcId, ev, emitTrack, peerState),
                        });
                    } catch {
                        // 1 つのトラックの処理に失敗しても、Discord の通話は止めない。
                    }
                });
            } catch {
                // ここで例外を投げると、Discord の通話が止まる。追跡できなくても、接続そのものは動かす。
            }
        }

        override close(): void {
            // close() では connectionstatechange が届かないので、閉じたことはここで ConnectionTracker に伝える
            // (その情報と、行き先を待っていたトラックを捨てる)。
            try {
                if (this.#serial !== 0) tracker.pcClosed(this.#serial);
            } catch {
                // 後始末に失敗しても、閉じる処理は止めない。
            }
            super.close();
        }
    }
    // 差し替えたクラスの name も RTCPeerConnection にするので、差し替えたかは name では判定できない。
    // PROBE_CHECKS の pc-hook は、.toString() に [native code] がないかで判定する。
    try {
        Object.defineProperty(Hooked, "name", { value: "RTCPeerConnection" });
    } catch {
        // 名前を変えられなくても、差し替えは続ける。
    }
    // webkitRTCPeerConnection は、元からなければ作らない。ない名前を作ると、Discord が古い Chrome 向けの分岐に入り、
    // Discord の動作が変わる。
    const hadWebkitAlias = "webkitRTCPeerConnection" in globalThis;

    try {
        (globalThis as unknown as { RTCPeerConnection: typeof RTCPeerConnection; }).RTCPeerConnection =
            Hooked as unknown as typeof RTCPeerConnection;
        if (hadWebkitAlias) {
            (globalThis as unknown as { webkitRTCPeerConnection: typeof RTCPeerConnection; }).webkitRTCPeerConnection =
                Hooked as unknown as typeof RTCPeerConnection;
        }
    } catch {
        emit({ type: "contact", id: "pc-hook", result: "missing" });
        return null;
    }

    emit({ type: "contact", id: "pc-hook", result: "found" });
    return () => {
        // Discord は差し替えたクラスへの参照を既に持っているので、グローバルを元に戻しても、Discord が作る新しい
        // 接続には適用されない。グローバルが差し替えたクラスのままのときだけ戻す。
        try {
            if (globalThis.RTCPeerConnection === (Hooked as unknown as typeof RTCPeerConnection)) {
                globalThis.RTCPeerConnection = native;
            }
        } catch {
            // 戻せなくても止めない。
        }
    };
}

/* ==========================================================================
   track-hook: RTCPeerConnection の ontrack
   ========================================================================== */

/**
 * ontrack で届いたトラック 1 つを TrackEvent に変えて送る。installPeerConnectionHook() の Hooked が、
 * ConnectionTracker が行き先 (pcId) を決めたときに呼ぶ (保持していたトラックは、決まったときにここに来る)。
 * ssrcs() は、呼ばれた時点の RTCRtpReceiver.getSynchronizationSources() を読み直す関数にする。最初のパケットが
 * 届くまでは空なので、値を固定せずに、呼び出し元 (index.ts) が読み直せるようにする。track_removed は、トラックの
 * ended イベントで送る。RTCPeerConnection が閉じたときに ended が届くかは確認していない。
 */
function handleTrack(
    pcId: number,
    ev: RTCTrackEvent,
    emitTrack: (event: TrackEvent) => void,
    peerState: () => PeerStateLike | undefined,
): void {
    const track = ev.track;
    const receiver = ev.receiver;
    const kind: TrackKind = track.kind === "video" ? "video" : "audio";

    const ssrcs = (): number[] => {
        try {
            return receiver.getSynchronizationSources().map(s => s.source);
        } catch {
            return [];
        }
    };

    let msid: string | null = null;
    try {
        msid = ev.streams[0]?.id ?? null;
    } catch {
        msid = null;
    }
    // 再生の遅延を測ってログに書くための関数。レシーバーそのものは渡さない (adapter/types.ts の TrackEvent)。
    // 例外と reject の扱いは呼び出し元が持つ (ここは包むだけ)。
    const getStats = (): Promise<RTCStatsReport> => receiver.getStats();
    emitTrack({ type: "track_added", pcId, kind, track, ssrcs, msid, getStats, peerState });

    try {
        track.addEventListener("ended", () => {
            emitTrack({ type: "track_removed", pcId, kind, trackId: track.id });
        });
    } catch {
        // 監視できなくても、接続そのものは動かす (track_removed が出ないだけ)。
    }
    // mute と unmute は、映像が届かないときに原因を分けるための手がかりとして送る (adapter/types.ts の track_mute)。
    try {
        track.addEventListener("mute", () => {
            emitTrack({ type: "track_mute", pcId, kind, trackId: track.id, muted: true });
        });
        track.addEventListener("unmute", () => {
            emitTrack({ type: "track_mute", pcId, kind, trackId: track.id, muted: false });
        });
    } catch {
        // 監視できなくても、接続そのものは動かす (ログの行が出ないだけ)。
    }
}

/* ==========================================================================
   ssrc-map: voice gateway の op12 (video_ssrc) と op5 (speaking)
   ========================================================================== */

const VOICE_GATEWAY_HOST = /(^|\.)discord\.media$/i;

/**
 * WebSocket を差し替え、voice gateway の WebSocket で送ったメッセージと受け取ったメッセージを、そのまま
 * ConnectionTracker に渡す。op5 と op12 から ssrc の CoreEvent を作るのも、ボイス接続と Go Live を分けるのも
 * ConnectionTracker である。op5 は、ボイス接続の voice gateway ではその user_id のマイクの ssrc を、Go Live の
 * voice gateway では画面共有の音声の ssrc を示す。どちらも、届いたらすぐに送る。マイクの ssrc は最初のパケットが
 * 届くまで getStats に出ないが、op5 は話し始めたときに届くので、すぐに送らないと、最初のひと言が配信に乗らない。
 */
function installVoiceGatewayHook(
    tracker: ConnectionTracker,
    emit: (event: AdapterEvent) => void,
): (() => void) | null {
    const native = globalThis.WebSocket;
    if (typeof native !== "function") {
        emit({ type: "contact", id: "ssrc-map", result: "missing" });
        return null;
    }

    class Hooked extends native {
        /** ConnectionTracker での通し番号 (voice gateway のときだけ入る)。 */
        #serial = 0;

        constructor(url: string | URL, protocols?: string | string[]) {
            super(url, protocols);
            try {
                this.#watch(String(url));
            } catch {
                // Discord の中で例外を外へ出さない。
            }
        }

        override send(data: Parameters<WebSocket["send"]>[0]): void {
            try {
                if (this.#serial !== 0) {
                    const frame = parseGatewayFrame(data);
                    if (frame !== null) tracker.wsSent(this.#serial, frame);
                }
            } catch {
                // 読めなくても送る。
            }
            super.send(data);
        }

        #watch(url: string): void {
            let host: string;
            try {
                host = new URL(url).hostname;
            } catch {
                host = url;
            }
            if (!VOICE_GATEWAY_HOST.test(host)) return;

            // まだ確認していないことと、見つからないことを分ける。実際に voice gateway の WebSocket ができたときだけ
            // found を送る。1 度もできないままでも missing にはしない (まだ通話に参加していないだけかもしれないため)。
            emit({ type: "contact", id: "ssrc-map", result: "found" });
            const serial = tracker.wsOpened();
            this.#serial = serial;
            this.addEventListener("close", () => {
                try {
                    tracker.wsClosed(serial);
                } catch {
                    // 後始末に失敗しても、Discord の通話は止めない。
                }
            });
            this.addEventListener("message", (ev: MessageEvent) => {
                try {
                    const frame = parseGatewayFrame(ev.data);
                    if (frame !== null) tracker.wsReceived(serial, frame);
                } catch {
                    // 1 つのメッセージを読めなくても、Discord の通話は止めない。
                }
            });
        }
    }
    try {
        Object.defineProperty(Hooked, "name", { value: "WebSocket" });
    } catch {
        // 名前を変えられなくても続ける。
    }

    try {
        globalThis.WebSocket = Hooked as unknown as typeof WebSocket;
    } catch {
        emit({ type: "contact", id: "ssrc-map", result: "missing" });
        return null;
    }

    return () => {
        try {
            if (globalThis.WebSocket === (Hooked as unknown as typeof WebSocket)) {
                globalThis.WebSocket = native;
            }
        } catch {
            // 戻せなくても止めない。
        }
    };
}

/* ==========================================================================
   roster、sharing、self、profile: Vencord が解決したストアを読む
   ========================================================================== */

// Discord のストアは自分で探さず、Vencord が解決したもの (@webpack/common) を使う。値が入るのが遅れることがある
// ので、使うたびに読む。

function displayName(user: { globalName?: string; username: string; id: string }, guildId: string | null): string {
    if (guildId !== null) {
        try {
            const nick = GuildMemberStore.getNick(guildId, user.id);
            if (typeof nick === "string" && nick !== "") return nick;
        } catch {
            // ニックネームが取れなくても、次の名前を使う (profile の依存箇所の中心は UserStore)。
        }
    }
    if (typeof user.globalName === "string" && user.globalName !== "") return user.globalName;
    return user.username;
}

/**
 * profile の依存箇所からアバターの URL を読む。取得してよいかは core/avatar.ts の normalizeAvatarUrl() が決め、
 * ここは URL を返すだけ。UserStore から取ったユーザーのオブジェクトを avatarUrlOfUser() (adapter/default_avatar.ts)
 * に渡す。既定のアバターの人に getAvatarURL() を使わないわけと、その分岐は、そちらに書いてある。
 */
function avatarUrl(userId: string): string | null {
    try {
        const user = UserStore.getUser(userId);
        if (!user) return null;
        return avatarUrlOfUser(userId, user);
    } catch {
        return null;
    }
}

/**
 * RTCConnectionStore.getChannelId() を読んだ結果を 3 つに分ける。ok が true で channelId が文字列なら、通話に参加
 * している。ok が true で null なら、通話に参加していない。ok が false なら、ストアが読めない (分からない)。参加
 * していないことと分からないことを同じ null にすると、読めないときに、roster を 0 人として送ってしまう。
 * 今いる通話のチャンネルは RTCConnectionStore.getChannelId() で読む。SelectedChannelStore.getVoiceChannelId() は
 * 選択しているチャンネルを返し、通話に接続していても空になることがある。
 */
type ChannelIdRead = { ok: true; channelId: string | null } | { ok: false };

function currentVoiceChannelId(): ChannelIdRead {
    try {
        const id = RTCConnectionStore.getChannelId();
        return { ok: true, channelId: typeof id === "string" && id !== "" ? id : null };
    } catch {
        return { ok: false };
    }
}

/**
 * 通話に参加している人の一覧。読めた一覧 (0 人でも配列) と、読めなかったこと (null) を区別して返す。呼び出し元は、
 * 読めなかったときは roster のイベントを送らず、roster の contact の missing だけを送る。
 */
function computeRoster(): SnapshotRosterUser[] | null {
    const channel = currentVoiceChannelId();
    if (!channel.ok) return null;
    if (channel.channelId === null) return [];

    let guildId: string | null = null;
    try {
        guildId = RTCConnectionStore.getGuildId() ?? null;
    } catch {
        guildId = null;
    }

    const out: SnapshotRosterUser[] = [];
    try {
        const states = VoiceStateStore.getVoiceStatesForChannel(channel.channelId);
        for (const userId of Object.keys(states)) {
            try {
                const user = UserStore.getUser(userId);
                if (!user) continue;
                out.push({
                    userId,
                    name: displayName(user, guildId),
                    username: uniqueUsername(user.username, user.discriminator),
                    avatarUrl: avatarUrl(userId),
                    avatarKey: typeof user.avatar === "string" ? user.avatar : "",
                    // bot は Discord のバージョンで変わりうるので、真のときだけ真にする (読めなければ人として出す)。
                    bot: (user as { bot?: unknown }).bot === true,
                });
            } catch {
                // 1 人を読めなくても、残りの人は読む。
            }
        }
    } catch {
        return null; // VoiceStateStore が読めない (0 人ではなく、分からない)。
    }
    return out;
}

/**
 * Vesktop のアカウント自身の表示名とユーザー名など。通話に参加していなくても読める (UserStore の現在のユーザー)。
 * 通話の外ではサーバーのニックネームを引かず、globalName、username の順に使う。読めなければ null。
 */
function computeSelfUser(): SnapshotRosterUser | null {
    try {
        const me = UserStore.getCurrentUser();
        if (!me || typeof me.id !== "string" || me.id === "") return null;
        let guildId: string | null = null;
        try {
            guildId = RTCConnectionStore.getGuildId() ?? null;
        } catch {
            guildId = null;
        }
        return {
            userId: me.id,
            name: displayName(me, guildId),
            username: uniqueUsername(me.username, me.discriminator),
            avatarUrl: avatarUrl(me.id),
            avatarKey: typeof me.avatar === "string" ? me.avatar : "",
            bot: false,
        };
    } catch {
        return null;
    }
}

function computeSelfRead(): SelfRead {
    try {
        const me = UserStore.getCurrentUser();
        return { ok: true, userId: typeof me?.id === "string" && me.id !== "" ? me.id : null };
    } catch {
        return { ok: false };
    }
}

/** snapshot() 向けの簡単な形 (読めなかったときも null にする)。読み分けが必要なときは computeSelfRead() を使う。 */
function computeSelf(): string | null {
    const read = computeSelfRead();
    return read.ok ? read.userId : null;
}

/**
 * 画面共有のストリームのキー (streamKey) を作る。ApplicationStreamingStore が返す Stream は { channelId, guildId,
 * ownerId, streamType } を持つが、streamKey は持たないので、ここで作る。キーの形は Discord のもので、サーバー
 * (guild) の中では guild:<guildId>:<channelId>:<ownerId>、DM とグループ通話では call:<channelId>:<ownerId> になる。
 */
function buildStreamKey(stream: { channelId?: string; guildId?: string | null; ownerId?: string }): string | null {
    const owner = stream.ownerId;
    const channel = stream.channelId;
    if (typeof owner !== "string" || typeof channel !== "string") return null;
    return typeof stream.guildId === "string" && stream.guildId !== ""
        ? `guild:${stream.guildId}:${channel}:${owner}`
        : `call:${channel}:${owner}`;
}

/**
 * 画面共有している人の一覧を、core/events.ts の sharing イベントと同じ形 (pcId、active) で返す
 * (adapter/types.ts の SnapshotSharingUser)。返すのは画面共有している人だけなので、active は常に true。pcId は
 * ConnectionTracker から引く (まだ視聴していなければ 0)。読めた一覧 (0 件でも配列) と、読めなかったこと (null) を
 * 区別する。
 *
 * 画面共有している人は、通話の各人の VoiceState の selfStream (画面共有中のフラグ) で見つける。
 * ApplicationStreamingStore.getAllActiveStreamsForChannel は、自分が視聴している画面共有だけを返すので使わない。
 * それを使うと、誰も視聴していない間は 0 件になり、この一覧に載った人だけを視聴するので、映像が 1 つも来ないまま
 * になる。streamKey は getAnyStreamForUser の Stream から作り、Stream がまだ読めなければ、サーバーとチャンネルと
 * 人の id から作る (Discord のキーの形のまま)。
 */
function computeSharing(tracker: ConnectionTracker): SnapshotSharingUser[] | null {
    const channel = currentVoiceChannelId();
    if (!channel.ok) return null;
    if (channel.channelId === null) return [];
    const channelId = channel.channelId;

    let guildId: string | null = null;
    try {
        guildId = RTCConnectionStore.getGuildId() ?? null;
    } catch {
        guildId = null;
    }

    let states: Record<string, { selfStream?: unknown } | undefined>;
    try {
        states = VoiceStateStore.getVoiceStatesForChannel(channelId) as Record<string, { selfStream?: unknown } | undefined>;
    } catch {
        return null; // VoiceStateStore が読めない (0 件ではなく、分からない)。
    }

    const streaming = ApplicationStreamingStore as unknown as { getAnyStreamForUser?: (userId: string) => unknown };
    const out: SnapshotSharingUser[] = [];
    for (const userId of Object.keys(states)) {
        const state = states[userId];
        if (!state || state.selfStream !== true) continue;
        let streamKey: string | null = null;
        try {
            const stream = typeof streaming.getAnyStreamForUser === "function" ? streaming.getAnyStreamForUser(userId) : null;
            if (stream && typeof stream === "object") {
                streamKey = buildStreamKey(stream as { channelId?: string; guildId?: string | null; ownerId?: string });
            }
        } catch {
            streamKey = null;
        }
        if (streamKey === null) streamKey = buildStreamKey({ channelId, guildId, ownerId: userId });
        if (streamKey === null) continue;
        out.push({ userId, streamKey, pcId: tracker.pcIdForUser(userId), active: true });
    }
    return out;
}

/* ==========================================================================
   call-info: 通話の名前と、メインアカウントがいる通話
   ========================================================================== */

interface ChannelLike {
    name?: unknown;
    type?: unknown;
    guild_id?: unknown;
    recipients?: unknown;
    userLimit?: unknown;
}

function userNameOf(userId: string): string | null {
    try {
        const user = UserStore.getUser(userId);
        if (!user) return null;
        if (typeof user.globalName === "string" && user.globalName !== "") return user.globalName;
        return typeof user.username === "string" ? user.username : null;
    } catch {
        return null;
    }
}

/**
 * 通話のチャンネルの名前。サーバーのチャンネルはサーバーの名前を添える (call_info.ts の callNameOf)。読めなければ null
 * を返し、適当な名前を作らない。
 */
function callNameOfChannel(channelId: string): string | null {
    try {
        const channel = ChannelStore.getChannel(channelId) as ChannelLike | undefined;
        if (!channel) return null;
        const guildId = typeof channel.guild_id === "string" && channel.guild_id !== "" ? channel.guild_id : null;
        let guildName: string | null = null;
        if (guildId !== null) {
            try {
                const guild = GuildStore.getGuild(guildId) as { name?: unknown } | undefined;
                if (typeof guild?.name === "string") guildName = guild.name;
            } catch {
                // サーバーの名前が読めなくても、チャンネルの名前は使う。
            }
        }
        const recipientNames: string[] = [];
        if (guildId === null && Array.isArray(channel.recipients)) {
            for (const id of channel.recipients) {
                const name = typeof id === "string" ? userNameOf(id) : null;
                if (name !== null) recipientNames.push(name);
            }
        }
        return callNameOf({
            channelName: typeof channel.name === "string" ? channel.name : "",
            guildName,
            recipientNames,
        });
    } catch {
        return null;
    }
}

/** 権限のビット。PermissionsBits は遅れて見つかる値で、見つからないときに読むと例外になることがあるので、読めなければ undefined。 */
function permissionBit(name: "CONNECT" | "MOVE_MEMBERS"): bigint | undefined {
    try {
        const bit = PermissionsBits?.[name];
        return typeof bit === "bigint" ? bit : undefined;
    } catch {
        return undefined;
    }
}

/** 権限を 1 つ読む。読めなければ null。 */
function canIn(permission: bigint | undefined, channel: ChannelLike): boolean | null {
    try {
        if (typeof permission !== "bigint") return null;
        return PermissionStore.can(permission, channel as never) === true;
    } catch {
        return null;
    }
}

/**
 * メインアカウントがいる通話を読む。VoiceStateStore は、Vesktop のアカウントが参加しているサーバーと会話の通話の状態
 * だけを持つので、メインアカウントが見えない通話に参加しているときも、どの通話にもいないときも、null を返す (この 2 つは区別
 * できない)。名前と参加できるかは、読めた値だけで決める (call_info.ts)。
 */
function readMainCall(mainUserId: string | null): MainCallInfo | null {
    if (mainUserId === null || mainUserId === "") return null;
    try {
        const state = VoiceStateStore.getVoiceStateForUser(mainUserId);
        const channelId = state?.channelId;
        if (typeof channelId !== "string" || channelId === "") return null;
        const channel = ChannelStore.getChannel(channelId) as ChannelLike | undefined;
        let joinable: boolean | null = null;
        if (channel) {
            let occupants = 0;
            try {
                occupants = Object.keys(VoiceStateStore.getVoiceStatesForChannel(channelId)).length;
            } catch {
                occupants = 0;
            }
            joinable = joinableOf({
                kind: typeof channel.type === "number" ? channel.type : null,
                canConnect: canIn(permissionBit("CONNECT"), channel),
                userLimit: typeof channel.userLimit === "number" ? channel.userLimit : 0,
                occupants,
                canBypassLimit: canIn(permissionBit("MOVE_MEMBERS"), channel) === true,
            });
        }
        return { userId: mainUserId, name: callNameOfChannel(channelId), joinable };
    } catch {
        return null;
    }
}

/** call-info の依存箇所が今使えるか。名前と参加できるかを読むのに使う、ストアのメソッドと権限の値を確認する。 */
function callInfoAvailable(): boolean {
    try {
        return (
            typeof ChannelStore?.getChannel === "function"
            && typeof GuildStore?.getGuild === "function"
            && typeof PermissionStore?.can === "function"
            && typeof VoiceStateStore?.getVoiceStateForUser === "function"
            && permissionBit("CONNECT") !== undefined
        );
    } catch {
        return false;
    }
}

/* ==========================================================================
   voice-select: 通話の移動と退出
   ========================================================================== */

type SelectVoiceChannel = (channelId: string | null) => void;

/**
 * 通話を選ぶ関数 (selectVoiceChannel)。channelId を渡すとその通話に参加し、null を渡すと退出する (Discord 自身も
 * null を渡して退出する)。Discord のバンドルより先に走るので、起動時にはなく、使うたびに探す。
 */
function findSelectVoiceChannel(): SelectVoiceChannel | null {
    try {
        const container = find(
            ((value: unknown): boolean => {
                if (value === null || typeof value !== "object") return false;
                const candidate = value as { selectVoiceChannel?: unknown; selectChannel?: unknown };
                return typeof candidate.selectVoiceChannel === "function" && typeof candidate.selectChannel === "function";
            }) as (v: unknown) => boolean,
            { isIndirect: true },
        ) as { selectVoiceChannel?: SelectVoiceChannel } | null | undefined;
        const fn = container?.selectVoiceChannel;
        return typeof fn === "function" ? fn.bind(container) : null;
    } catch {
        return null;
    }
}

/* ==========================================================================
   watch-stop: dispatch する action の type で、視聴の開始と停止の関数を探す
   ========================================================================== */

interface WatchFunctions {
    start: ((stream: unknown, options: { forceMultiple: boolean; noFocus: boolean; }) => void) | null;
    stop: ((key: string, feedback: boolean) => void) | null;
}

const WATCH_RE = /dispatch\([^)]{0,160}"?STREAM_WATCH"?/;
const STOP_RE = /dispatch\([^)]{0,160}"?STREAM_STOP"?/;
const CLOSE_RE = /dispatch\([^)]{0,160}"?STREAM_CLOSE"?/;

/** webpack のモジュールの exports から、本文が re に一致する関数を 1 つ選ぶ。 */
function pickBy(container: object, re: RegExp): ((...args: never[]) => void) | null {
    let keys: string[];
    try {
        keys = Object.keys(container);
    } catch {
        return null;
    }
    for (const key of keys) {
        let value: unknown;
        try {
            value = (container as Record<string, unknown>)[key];
        } catch {
            continue;
        }
        if (typeof value !== "function") continue;
        let src = "";
        try {
            src = Function.prototype.toString.call(value);
        } catch {
            continue;
        }
        if (re.test(src)) return value as (...args: never[]) => void;
    }
    return null;
}

/**
 * 視聴の開始と停止の関数を探す。開始の関数があるモジュールには停止の関数も置かれている (Discord のソースで
 * 確認した) ので、開始の関数が見つかったモジュールの、ほかの関数をすべて調べる。findByCode は関数を 1 つずつ
 * しか見ないので、モジュールの exports を自分で調べる。
 */
function discoverWatchFunctions(): WatchFunctions {
    const out: WatchFunctions = { start: null, stop: null };

    const containerFilter = (value: unknown): boolean => {
        if (value === null || typeof value !== "object") return false;
        return pickBy(value, WATCH_RE) !== null;
    };

    try {
        const container = find(containerFilter as (v: unknown) => boolean, { isIndirect: true });
        if (container !== null && typeof container === "object") {
            out.start = pickBy(container, WATCH_RE) as WatchFunctions["start"];
            out.stop = (pickBy(container, STOP_RE) ?? pickBy(container, CLOSE_RE)) as WatchFunctions["stop"];
        }
    } catch {
        // 見つからなければ両方とも null のまま (結果をキャッシュしないので、呼び直せば探し直す)。
    }
    return out;
}

/* ==========================================================================
   Adapter を作る
   ========================================================================== */

/** 見つからない関数と依存箇所を探し直す間隔 (ms)。 */
const REDISCOVER_MS = 5000;
/** 購読する Flux のイベント。 */
const WATCHED_EVENTS = [
    "CONNECTION_OPEN",
    "VOICE_STATE_UPDATES",
    "VOICE_CHANNEL_SELECT",
    "CHANNEL_SELECT",
    "RTC_CONNECTION_STATE",
    "STREAM_CREATE",
    "STREAM_DELETE",
    "STREAM_UPDATE",
] as const;

/**
 * 周期的に確認する 7 つの依存箇所。pc-hook と ssrc-map は、フックの設置の成否が found と missing を決め、周期的に
 * 再確認するものではないので、ここには入れない (PROBE_CHECKS には両方とも入れて、Adapter.probe() には答える)。
 */
const PROBED_IDS: readonly ContactId[] = [
    "sharing",
    "watch-stop",
    "roster",
    "profile",
    "self",
    "call-info",
    "voice-select",
];
const PROBE_RETRY_MAX = 24; // 5 秒間隔で 24 回 (2 分)。それでも見つからなければ、次に失敗したときまで待つ。
/** すべて found の間も確認続ける間隔 (ms)。目安の値で、測って決めたものではない。 */
const FOUND_WATCHDOG_MS = 30000;
/**
 * FluxDispatcher に購読が付くのを待つ上限 (ms)。過ぎたら、購読なしで自分のアカウント・参加者・画面共有の読み取りと依存箇所の確認を始め、
 * 購読が付くまで FLUX_FALLBACK_POLL_MS ごとに読み直す。待ち続けると、何も届かないのにメッセージも出ない (確認が始まらないので missing も
 * 送られない)。Discord のバンドルは起動の数秒で読み込まれるので、ふつうは上限の前に付く。目安の値で、測って決めたものではない。
 */
const FLUX_ATTACH_GRACE_MS = 10000;
/** 購読が付かない間に読み直す間隔 (ms)。変化が届くのが最大この分だけ遅れる。読むのはストアの値だけで軽い。 */
const FLUX_FALLBACK_POLL_MS = 1000;

/**
 * Discord と Vencord の内部から事実を読み、CoreEvent に変えて送る Adapter を作る。index.ts が
 * startAt: StartAt.Init を指定し、Discord のバンドルより先に動く前提で書く。その時点では FluxDispatcher がまだ
 * ないのが正常なので、使えるようになるまで待つ。options.diag は、接続のログの行を受け取る関数 (省くと捨てる)。
 */
export function createVencordAdapter(options: { diag?: DiagSink } = {}): Adapter {
    const diag: DiagSink = options.diag ?? (() => {});
    // 視聴は、何人ぶんを同時に始めてもよい。接続と人の対応付けは ConnectionTracker が接続そのものの値で決めるので、
    // どの接続が先にできても取り違えない。
    const startWatchingNow = async (userId: string): Promise<StartResult> => {
        const fns = ensureWatchFunctions();
        if (fns.start === null) {
            // 視聴を始めようとして失敗したときに再確認する。Discord のモジュールは遅れて読み込まれ、Vesktop の
            // アカウントが 1 度も通話に参加していないと、視聴の関数がまだないことがあるためである。
            recheckOne("watch-stop");
            return { ok: false, reason: "視聴を始める操作が見つかりません" };
        }
        // start() には streamKey ではなく、ApplicationStreamingStore から取得した Stream を渡す (中で streamKey に
        // 変換される)。
        let stream: unknown;
        try {
            stream = ApplicationStreamingStore.getAnyStreamForUser(userId);
        } catch {
            stream = null;
        }
        if (stream === null || stream === undefined) {
            recheckOne("sharing");
            return { ok: false, reason: "共有の実体が見つかりません" };
        }
        try {
            // forceMultiple と noFocus を必ず渡す。渡さないと Discord がほかの視聴を閉じ、その人の映像が配信から
            // 消える。
            fns.start(stream, { forceMultiple: true, noFocus: true });
            return { ok: true };
        } catch (e) {
            return { ok: false, reason: String(e) };
        }
    };

    // subscribeTracks() の購読者の一覧。RTCPeerConnection の差し替え (installPeerConnectionHook) は subscribe() が
    // 呼ばれたときに 1 回だけ行うが、この Set は createVencordAdapter() が返す adapter と同じ寿命なので、
    // subscribeTracks() を subscribe() の前に呼んでも後に呼んでも、差し替えが動き出した時点の購読者に届く。
    const trackListeners = new Set<(event: TrackEvent) => void>();
    const emitTrack = (event: TrackEvent): void => {
        for (const listener of trackListeners) {
            try {
                listener(event);
            } catch {
                // 1 つの listener の不具合で、ほかの listener と Discord の通話を止めない。
            }
        }
    };

    let watchFns: WatchFunctions = { start: null, stop: null };
    let watchScanned = false;
    let lastWatchScan = 0;

    const ensureWatchFunctions = (): WatchFunctions => {
        // 停止の関数も探し直す (探すのをやめると、視聴を止められないまま、受信とデコードの負荷が残る)。開始と停止の
        // どちらかがない間だけ、REDISCOVER_MS ごとに探し直す。
        const whole = watchFns.start !== null && watchFns.stop !== null;
        if (!whole) {
            const now = Date.now();
            if (!watchScanned || now - lastWatchScan >= REDISCOVER_MS) {
                lastWatchScan = now;
                watchScanned = true;
                watchFns = discoverWatchFunctions();
            }
        }
        return watchFns;
    };

    // subscribe() が有効な間だけ null でない。startWatching() のように subscribe() の外で失敗したときも、ここを
    // 通して、すぐに contact のイベントを 1 つ送れるようにする (失敗したときに再確認するには、失敗した場所から
    // 送る経路が必要なため)。
    let activeEmit: ((event: AdapterEvent) => void) | null = null;
    // subscribe() が有効な間の、自分のアカウントを読み直して変わっていれば送る関数。runProbeCycle() からも呼ぶ
    // (ログインの完了を Flux のイベントで受け取れなかったときも、周期的な確認で送り直すため)。
    let activeRefreshSelf: (() => void) | null = null;
    // subscribe() が有効な間の、通話の名前とメインアカウントがいる通話を読み直して、変わっていれば送る関数。
    let activeRefreshCalls: (() => void) | null = null;
    // subscribe() が有効な間の、参加者と画面共有を読み直して、変わっていれば送る関数。runProbeCycle() が周期的に呼ぶ。
    // Flux のイベントでしか読み直さないと、Discord がイベントの名前を変えたときに、ストアは見つかる (依存箇所は
    // 見つかったまま) のに、参加者と画面共有だけが黙って古くなる。値が変わったときだけ送るので、負荷は小さい。
    let activeRefreshPeople: (() => void) | null = null;
    // 受信側から届いたメインアカウントの userId。メインアカウントを使わない、または分からないときは null。
    let mainUserId: string | null = null;

    // 接続ごとの pcId を決め、画面共有している人を特定する (adapter/connections.ts)。ssrc のイベントは、
    // subscribe() が有効な間の activeEmit に送る (フックは subscribe() の中で付けるので、それより前には呼ばれない)。
    const tracker = new ConnectionTracker({
        emit: (event: AdapterEvent) => activeEmit?.(event),
        diag,
        callServerId: currentCallServerId,
    });

    /**
     * 依存箇所 9 つのそれぞれが今使えるかを、直接確認する関数。ledger.ts の述語は使わず、この adapter が実際に使う
     * 参照 (解決したストアのメソッド、フックの設置) を確認する。述語は、保存した Discord のバンドルで数百のモジュールに
     * 一致したことがあり、実行時の判定に使うと、壊れていても見つかったことになるためである。
     */
    const PROBE_CHECKS: Readonly<Record<ContactId, () => boolean>> = {
        "pc-hook": () => {
            const ctor = globalThis.RTCPeerConnection;
            // 差し替えたかは、.toString() に [native code] がないかで判定する (差し替えたクラスも name は
            // RTCPeerConnection なので、name では判定できない)。
            // 受け取ったトラックを読む MediaStreamTrackProcessor もここで確認する (core/track_processor.ts)。なければ、
            // 差し替えが動いていても映像と音声を読めないので、同じ「画面共有と声を受け取れない」として伝える。
            return (
                typeof ctor === "function"
                && !Function.prototype.toString.call(ctor).includes("[native code]")
                && trackProcessorAvailable()
            );
        },
        "ssrc-map": () => {
            const ctor = globalThis.WebSocket;
            return typeof ctor === "function" && !Function.prototype.toString.call(ctor).includes("[native code]");
        },
        // sharing、roster、self は、実際に呼んで読めるかを確認する。emitSharing() などと同じ computeSharing() などを
        // 使う。判定が食い違うと、片方が found、もう片方が missing を送り、found と lost が交互に入れ替わる。
        sharing: () => computeSharing(tracker) !== null,
        "watch-stop": () => ensureWatchFunctions().start !== null,
        roster: () => computeRoster() !== null,
        profile: () => typeof UserStore?.getUser === "function",
        self: () => computeSelfRead().ok,
        "call-info": () => callInfoAvailable(),
        "voice-select": () => findSelectVoiceChannel() !== null,
    };

    let probeTimer: ReturnType<typeof setTimeout> | null = null;
    let probeRetries = 0;

    const scheduleProbe = (delayMs: number): void => {
        if (probeTimer !== null) clearTimeout(probeTimer);
        probeTimer = setTimeout(() => {
            probeTimer = null;
            runProbeCycle();
        }, delayMs);
    };

    /** Discord にまだログインしていないか (自分のアカウントが分からない間)。adapter/probe_cycle.ts の決め方に渡す。 */
    const waitingForLogin = (): boolean => {
        const read = computeSelfRead();
        return read.ok && read.userId === null;
    };

    /**
     * PROBED_IDS を 1 回ずつ確認し、それぞれの contact のイベントを送る。すべて found なら FOUND_WATCHDOG_MS の間隔で
     * 確認続け (見つかった後も確認続けないと、使えなくなったことを検出できない)、1 つでも missing なら
     * PROBE_RETRY_MAX 回まで REDISCOVER_MS の間隔で探し直す。上限に達した後は、次に読み取りか視聴の開始に失敗した
     * とき (recheckOne()) に、また探し始める。Discord にログインする前は、missing を送らずに探し直し続ける
     * (adapter/probe_cycle.ts)。
     */
    function runProbeCycle(): void {
        const emit = activeEmit;
        if (emit === null) return;
        const results = PROBED_IDS.map((id) => [id, PROBE_CHECKS[id]()] as const);
        const plan = planProbeCycle(results, waitingForLogin(), probeRetries, PROBE_RETRY_MAX);
        for (const e of plan.emit) emit({ type: "contact", id: e.id, result: e.result });
        activeRefreshSelf?.();
        // 通話の名前の変更は Flux のイベントを購読していないので、周期的な確認のたびに読み直す。
        activeRefreshCalls?.();
        // 参加者と画面共有も、イベントを受け取れなかったときのために、周期的な確認のたびに読み直す。
        activeRefreshPeople?.();
        probeRetries = plan.retries;
        if (plan.next === "watchdog") scheduleProbe(FOUND_WATCHDOG_MS);
        else if (plan.next === "rediscover") scheduleProbe(REDISCOVER_MS);
    }

    /**
     * 読み取りや視聴の開始に失敗したときに呼ぶ。その依存箇所 1 つをすぐに再確認、まだ見つからなければ、回数の
     * 上限を戻して探し直しを再開する。Discord にログインする前に見つからなかったときは、missing を送らない。
     */
    const recheckOne = (id: ContactId): void => {
        const emit = activeEmit;
        if (emit === null) return;
        const found = PROBE_CHECKS[id]();
        if (shouldEmitRecheck(found, waitingForLogin())) emit({ type: "contact", id, result: found ? "found" : "missing" });
        if (!found) {
            probeRetries = 0;
            scheduleProbe(REDISCOVER_MS);
        }
    };

    // stopWatching() で、停止の関数が見つかるまで探し直すためのタイマー。userId と streamKey の組ごとに 1 つだけ持つ。
    const pendingStopRetries = new Map<string, ReturnType<typeof setTimeout>>();
    const stopRetryKey = (userId: string, streamKey: string): string => `${userId}:${streamKey}`;

    const attemptStop = (userId: string, streamKey: string): void => {
        const key = stopRetryKey(userId, streamKey);
        const existing = pendingStopRetries.get(key);
        if (existing !== undefined) {
            clearTimeout(existing);
            pendingStopRetries.delete(key);
        }

        const fns = ensureWatchFunctions();
        if (fns.stop === null) {
            // 停止の関数が見つからなくても、見つかるまで REDISCOVER_MS の間隔で探し直す。
            recheckOne("watch-stop");
            const timer = setTimeout(() => attemptStop(userId, streamKey), REDISCOVER_MS);
            pendingStopRetries.set(key, timer);
            return;
        }
        try {
            // 第 2 引数は、視聴をやめた後に感想を聞く画面を表示するか。false で表示されないことを確認した。
            fns.stop(streamKey, false);
        } catch {
            // 1 人の停止に失敗しても、ほかの処理は止めない。
        }
    };

    return {
        async probe(id: ContactId): Promise<ProbeResult> {
            try {
                return { found: PROBE_CHECKS[id]() };
            } catch {
                // Discord の中で例外を外へ出さない。
                return { found: false };
            }
        },

        subscribe(listener: (event: AdapterEvent) => void): () => void {
            const cleanups: Array<() => void> = [];
            let cancelled = false;

            const emit = (event: AdapterEvent): void => {
                try {
                    listener(event);
                } catch {
                    // Discord の中で例外を外へ出さない。listener (core) の不具合で、Discord の通話を止めない。
                }
            };
            activeEmit = emit;

            const pcCleanup = installPeerConnectionHook(tracker, emit, emitTrack, diag);
            if (pcCleanup !== null) cleanups.push(pcCleanup);

            const gatewayCleanup = installVoiceGatewayHook(tracker, emit);
            if (gatewayCleanup !== null) cleanups.push(gatewayCleanup);

            // 前回の emitSharing() で送った、画面共有している人の集合。今回と比べ、消えた人には active: false を
            // 送る。送らないと、core はその人が画面共有を続けていると扱い続け、stop-watching を出さない。
            let lastSharingByUser = new Map<string, SnapshotSharingUser>();

            const emitRoster = (): void => {
                const roster = computeRoster();
                if (roster === null) {
                    // 読めなかった (0 人ではなく、分からない)。roster のイベントは送らず、roster の contact の
                    // missing だけを送る。
                    recheckOne("roster");
                    return;
                }
                emit({ type: "contact", id: "roster", result: "found" });
                // core/events.ts の RosterUser は userId だけを持つ (名前とアバターは profile の依存箇所で読む)。
                // ここで userId だけにして送る。
                emit({ type: "roster", users: roster.map(u => ({ userId: u.userId })) });
            };
            const emitSharing = (): void => {
                const rows = computeSharing(tracker);
                if (rows === null) {
                    recheckOne("sharing");
                    return;
                }
                emit({ type: "contact", id: "sharing", result: "found" });
                const nextByUser = new Map(rows.map(r => [r.userId, r] as const));
                for (const [userId, prevRow] of lastSharingByUser) {
                    if (!nextByUser.has(userId)) {
                        emit({
                            type: "sharing",
                            userId,
                            streamKey: prevRow.streamKey,
                            pcId: prevRow.pcId,
                            active: false,
                        });
                    }
                }
                for (const row of rows) {
                    emit({ type: "sharing", ...row });
                }
                lastSharingByUser = nextByUser;
                // 視聴する人 (selection) は、画面共有している人の全員 (Vesktop のアカウントを除く)。core は
                // selection と sharing の両方に入っている人だけを視聴する。誰を枠に載せるかは、受信側が届いた
                // ストリームの中から決める (src-obs/pipeline/stream_router.h) ので、送信側は画面共有している人を
                // 全員送る。Vesktop のアカウントの画面共有は視聴しない (自分の枠は受信側が作る)。
                const self = computeSelf();
                emit({ type: "selection", userIds: rows.map(r => r.userId).filter(id => id !== self) });
            };
            // 前に送った自分のアカウント (まだ 1 度も送っていなければ undefined)。変わったときだけ送り直す (adapter/probe_cycle.ts)。
            let sentSelf: string | null | undefined;
            const emitSelf = (): void => {
                const read = computeSelfRead();
                const plan = planSelfEmit(read, sentSelf);
                if (plan === "recheck") recheckOne("self");
                if (plan !== "send" || !read.ok) return;
                emit({ type: "contact", id: "self", result: "found" });
                emit({ type: "self", userId: read.userId });
                sentSelf = read.userId;
            };
            activeRefreshSelf = emitSelf;

            // 前に送った通話の名前と、メインアカウントがいる通話 (JSON の文字列)。変わったときだけ送る。初期値は、
            // core の初期状態 (どちらも null) と同じにして、読めない間に null を送らない。
            let sentCallName: string | null = null;
            let sentMainCallJson = "null";
            const emitCalls = (): void => {
                const channel = currentVoiceChannelId();
                const name = channel.ok && channel.channelId !== null ? callNameOfChannel(channel.channelId) : null;
                if (name !== sentCallName) {
                    sentCallName = name;
                    emit({ type: "call_name", name });
                }
                const main = readMainCall(mainUserId);
                const json = JSON.stringify(main);
                if (json !== sentMainCallJson) {
                    sentMainCallJson = json;
                    emit({ type: "main_call", call: main });
                }
            };
            activeRefreshCalls = emitCalls;
            const refreshPeople = (): void => {
                emitRoster();
                emitSharing();
            };
            activeRefreshPeople = refreshPeople;

            // Flux の購読を definePlugin の flux (Vencord v1.15.17 の src/api/PluginManager.ts の
            // subscribePluginFluxEvents) に移さない。下の自前の待ちが、10 秒で付かなければログを書き、1 秒ごとの
            // 読み直しに切り替える動きは、Discord の内部が見つからない状態の唯一の手がかりで、
            // flux では作れない。参加者と画面共有の古くなる心配は、runProbeCycle() の周期の読み直しが受け持つ。
            // FluxDispatcher は @webpack/common の export let で、後から値が入る。startAt: Init の時点ではまだ値が
            // ないのが正常なので、使えるまで待つ。待たずにやめると、起動のたびに間に合うかどうかが変わり、間に
            // 合わなかった起動では、VOICE_STATE_UPDATES などの Flux のイベントを 1 つも受け取れない。
            let attachedHandler: ((event: Record<string, unknown>) => void) | null = null;
            const tryAttach = (): boolean => {
                if (typeof FluxDispatcher?.subscribe !== "function") return false;
                const handler = (event: Record<string, unknown>): void => {
                    const type = event.type;
                    // ログインが済むと CONNECTION_OPEN が来る。どのイベントでも読み直し、変わったときだけ送る。
                    emitSelf();
                    if (type === "CONNECTION_OPEN") {
                        emitRoster();
                        emitSharing();
                        emitCalls();
                    } else if (
                        type === "VOICE_STATE_UPDATES"
                        || type === "VOICE_CHANNEL_SELECT"
                        || type === "CHANNEL_SELECT"
                        || type === "RTC_CONNECTION_STATE"
                    ) {
                        emitRoster();
                        // 画面共有の開始と終了は、VoiceState の selfStream でも届くので、ここでも読み直す。通話に
                        // 参加する前に始まっていた画面共有では、STREAM_CREATE などが来ない。
                        emitSharing();
                        emitCalls();
                    } else if (type === "STREAM_CREATE" || type === "STREAM_DELETE" || type === "STREAM_UPDATE") {
                        emitSharing();
                    }
                };
                for (const eventType of WATCHED_EVENTS) {
                    try {
                        FluxDispatcher.subscribe(eventType, handler);
                    } catch {
                        // 1 種類の購読に失敗しても、残りは購読する。
                    }
                }
                attachedHandler = handler;
                return true;
            };

            // Discord のバンドルと Vencord の @webpack/common がまだそろっていない間に読むと、まだ確認していない
            // 状態 (unseen) を飛ばして、missing や null になる。起動の直後にストアがまだないのは正常なので、
            // tryAttach() が成功する (FluxDispatcher が使えるようになる) まで、self、roster、sharing の最初の
            // 読み取りと、最初の runProbeCycle() を待つ。
            const afterAttach = (): void => {
                emitSelf();
                emitRoster();
                emitSharing();
                emitCalls();
                runProbeCycle();
            };

            if (tryAttach()) {
                afterAttach();
            } else {
                // FLUX_ATTACH_GRACE_MS を過ぎても付かなければ、購読なしで読み取りを始め、付くまで読み直す (fallback)。
                const waitStartedAt = Date.now();
                let fallback: ReturnType<typeof setInterval> | null = null;
                const readAll = (): void => {
                    emitSelf();
                    emitRoster();
                    emitSharing();
                    emitCalls();
                };
                const poll: ReturnType<typeof setInterval> = setInterval(() => {
                    if (cancelled) return;
                    if (tryAttach()) {
                        clearInterval(poll);
                        if (fallback === null) {
                            afterAttach();
                        } else {
                            clearInterval(fallback);
                            fallback = null;
                            diag("flux attached after polling");
                            readAll();
                        }
                        return;
                    }
                    if (fallback === null && Date.now() - waitStartedAt >= FLUX_ATTACH_GRACE_MS) {
                        diag(`flux not attached after ${FLUX_ATTACH_GRACE_MS} ms; reading every ${FLUX_FALLBACK_POLL_MS} ms`);
                        afterAttach();
                        fallback = setInterval(() => {
                            if (!cancelled) readAll();
                        }, FLUX_FALLBACK_POLL_MS);
                    }
                }, 250);
                cleanups.push(() => {
                    clearInterval(poll);
                    if (fallback !== null) clearInterval(fallback);
                });
            }

            cleanups.push(() => {
                cancelled = true;
                if (activeEmit === emit) activeEmit = null;
                if (activeRefreshSelf === emitSelf) activeRefreshSelf = null;
                if (activeRefreshCalls === emitCalls) activeRefreshCalls = null;
                if (activeRefreshPeople === refreshPeople) activeRefreshPeople = null;
                if (probeTimer !== null) {
                    clearTimeout(probeTimer);
                    probeTimer = null;
                }
                for (const timer of pendingStopRetries.values()) clearTimeout(timer);
                pendingStopRetries.clear();
                // 行き先を待っているトラックの見直しのタイマーも止める (保持していたトラックは捨てる)。
                tracker.dispose();
                if (attachedHandler !== null) {
                    for (const eventType of WATCHED_EVENTS) {
                        try {
                            FluxDispatcher.unsubscribe(eventType, attachedHandler);
                        } catch {
                            // 外せなくても止めない。
                        }
                    }
                }
            });

            return () => {
                for (const cleanup of cleanups) {
                    try {
                        cleanup();
                    } catch {
                        // 1 つの後始末に失敗しても、残りは続ける。
                    }
                }
            };
        },

        startWatching(userId: string): Promise<StartResult> {
            // streamKey は使わない (Discord の視聴を始める関数は、streamKey ではなく Stream を受け取る)。
            return startWatchingNow(userId);
        },

        ownerOfConnection(pcId: number): string | undefined {
            // Go Live の接続の voice gateway が op12 (なければ op5) で伝えた、画面共有している人
            // (adapter/connections.ts)。
            return tracker.ownerOf(pcId);
        },

        async stopWatching(userId: string, streamKey: string): Promise<void> {
            // その人が画面共有している接続を、すべて手放す (再接続の途中で 2 つ開いていることがある)。forget() は
            // 手放した接続を pcIdForUser() から外すので、ループは必ず終わるが、念のため回数に上限を置く。
            for (let i = 0; i < 8; i++) {
                const pcId = tracker.pcIdForUser(userId);
                if (pcId === 0) break;
                tracker.forget(pcId);
            }
            attemptStop(userId, streamKey);
        },

        setMainUser(userId: string | null): void {
            const next = userId === null || userId === "" ? null : userId;
            if (next === mainUserId) return;
            mainUserId = next;
            activeRefreshCalls?.();
        },

        moveToMainCall(): CallActionResult {
            // 押された時点のメインアカウントの通話を読み直して移動する。受信側が前に送った値は使わない。
            const select = findSelectVoiceChannel();
            if (select === null) {
                recheckOne("voice-select");
                return { ok: false, reason: "the function to select a voice channel was not found" };
            }
            const mainId = mainUserId;
            if (mainId === null) return { ok: false, reason: "the main account is not set" };
            let channelId: unknown;
            try {
                channelId = VoiceStateStore.getVoiceStateForUser(mainId)?.channelId;
            } catch {
                channelId = undefined;
            }
            if (typeof channelId !== "string" || channelId === "") {
                return { ok: false, reason: "the main account is not in a call that this account can see" };
            }
            const main = readMainCall(mainId);
            if (main === null || main.joinable !== true) {
                const cannot = main !== null && main.joinable === false;
                return {
                    ok: false,
                    reason: cannot ? "this account cannot join that call" : "could not confirm that this account can join that call",
                };
            }
            try {
                select(channelId);
                return { ok: true };
            } catch (e) {
                return { ok: false, reason: String(e) };
            }
        },

        leaveCall(): CallActionResult {
            const select = findSelectVoiceChannel();
            if (select === null) {
                recheckOne("voice-select");
                return { ok: false, reason: "the function to select a voice channel was not found" };
            }
            try {
                select(null);
                return { ok: true };
            } catch (e) {
                return { ok: false, reason: String(e) };
            }
        },

        async snapshot(): Promise<Snapshot> {
            return {
                selfUserId: computeSelf(),
                selfUser: computeSelfUser(),
                roster: computeRoster() ?? [],
                sharing: computeSharing(tracker) ?? [],
            };
        },

        subscribeTracks(listener: (event: TrackEvent) => void): () => void {
            // RTCPeerConnection の差し替えは、installPeerConnectionHook() が、subscribe() が呼ばれたときに 1 回だけ
            // 行う。ここは購読者の一覧 (trackListeners) に追加と削除をするだけで、差し替えを重ねない。
            trackListeners.add(listener);
            return () => {
                trackListeners.delete(listener);
            };
        },
    };
}
