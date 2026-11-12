// SPDX-License-Identifier: Apache-2.0

// core の状態 (CoreState) と、ただ 1 つの遷移関数 reduce()。adapter からのイベントで、誰が通話に参加していて誰が
// 画面共有しているかを持ち、警告 (warnings())、視聴を始めるか止めるかの判断 (decisions())、
// ssrc に対応する userId (userIdForSsrc()) を返す。どれも副作用のない純粋関数である。core は、誰を視聴するか
// (selection) を自分で決めず、selection イベントで受け取るだけにする。Vencord のものは import しない。

import type { ContactId } from "../ledger";
import type { CoreEvent, MainCallInfo, RosterUser } from "./events";

export type { ContactId };

/**
 * 依存箇所 1 つの状態。
 * - "unseen": まだ 1 度も確認していない。起動の直後はこの状態で、異常ではない。
 * - "found": 使えることを確認した。
 * - "missing": 1 度も使えたことがなく、使えない。
 * - "lost": 使えていた ("found") のに、使えなくなった。
 */
export type ContactStatus = "unseen" | "found" | "missing" | "lost";

/**
 * warnings() が返す値。どの依存箇所が "missing" か "lost" かだけを表し、値は ContactId そのもの。画面の文言は、
 * 受信側 (src-obs/ledger/rules_dock.cpp の kContactHeadlines) が id から決め、core は持たない。
 */
export type WarningId = ContactId;

/** 今続いている画面共有 1 件 (sharing() の要素)。 */
export interface SharingUser {
    userId: string;
    streamKey: string;
    pcId: number;
}

/**
 * core が出す判断。この 3 種類だけで、送っている映像や音声を止める判断は型としてない。依存箇所が見つからなく
 * なっても、送っている映像と音声は止めない。
 */
export type Decision =
    | { type: "start-watching"; userId: string; streamKey: string }
    | { type: "stop-watching"; userId: string; streamKey: string }
    | { type: "mark-source-switched"; userId: string; pcId: number };

/** ボイス接続の番号。 */
const VOICE_PC_ID = 1;

interface SharingInfo {
    streamKey: string;
    pcId: number;
    active: boolean;
}

interface SsrcInfo {
    videoSsrc: number | null;
    /**
     * 最後に読めた null でない videoSsrc。間に null を挟んでも、送信元が替わったことを検出するために持つ
     * (100 -> null -> 200 でも、100 と 200 の違いを検出する)。
     */
    lastNonNullVideoSsrc: number | null;
    audioSsrc: number | null;
}

/**
 * ssrc の値を保存するキー。ssrc の値は (userId, pcId) ごとに持つ。話しながら画面共有している人には、マイクの ssrc
 * (ボイス接続) と画面共有の ssrc (Go Live の接続) が同じ userId で届く。userId だけで 1 つにすると、マイクの値が
 * 画面共有の値を上書きし、映像の送信元が替わったと誤って検出する。tests/core_state_fuzz.test.ts が前後の ssrc の
 * 値を直接比べるので export する (区切りの文字をテストに書かせないため)。
 */
export function ssrcKey(userId: string, pcId: number): string {
    return `${userId}:${pcId}`;
}

/** core の状態。どのフィールドも reduce() だけが書き換える。直接書き換えない。 */
export interface CoreState {
    /** 依存箇所の一覧の 9 行それぞれの状態。 */
    contacts: Readonly<Record<ContactId, ContactStatus>>;
    /**
     * 通話に参加している人の一覧。"unknown" は、まだ 1 度も読めていないこと。空の配列は、読めていて 0 人という
     * 別の事実で、"unknown" とは区別する。
     */
    roster: "unknown" | readonly RosterUser[];
    /** Vesktop のアカウントの userId (self の依存箇所)。分からなければ null。 */
    selfUserId: string | null;
    /**
     * 外から受け取った、視聴する人の集合。core は selection イベントのほかでは書き換えない。roster が読めなくなって
     * も変えない。
     */
    selection: readonly string[];
    /**
     * 受信側が選んだ視聴する人の範囲。null は画面共有している全員 (受信側の設定が everyone のとき)。配列のときは、その
     * 人だけを視聴する。受信側から何も届いていない間は空の配列で、誰も視聴しない (OBS を起動していない間や、無効に
     * している間に、共有している人の画面へ視聴者として出ない)。
     */
    watchScope: readonly string[] | null;
    /** Vesktop のアカウントがいる通話の名前 (call_name)。分からなければ null。 */
    callName: string | null;
    /** メインアカウントがいる通話 (main_call)。null は、分からない、または見えない通話に参加している。 */
    mainCall: MainCallInfo | null;
    /** userId -> 最後に届いた画面共有の状態。 */
    sharing: Readonly<Record<string, SharingInfo>>;
    /** (userId, pcId) -> 最後に届いた ssrc の値。キーは ssrcKey()。 */
    ssrc: Readonly<Record<string, SsrcInfo>>;
    /**
     * userId -> 最後に null でない videoSsrc を運んだ接続の pcId。映像の送信元の接続が替わったかを比べるためだけに
     * 使う。マイクの ssrc のイベントは videoSsrc が常に null なので、ここには書かない (マイクの活動を映像の送信元の
     * 切り替えと取り違えないため)。
     */
    videoPcIdByUser: Readonly<Record<string, number>>;
    /**
     * userId -> 最後に start-watching を決めた streamKey。同じ人の 2 つの画面共有を同時に視聴しないように、
     * 1 人に 1 件だけ持つ。start-watching を決めた時点で、実際に始まったかを待たずに書き込み、watch_result イベントの
     * ok が false のときだけ取り消す。ok が true なら何もしない。
     */
    watching: Readonly<Record<string, string>>;
    /**
     * 最後の reduce() で新しく出た判断だけを持つ。decisions() はこれをそのまま返す。呼び出し元は reduce() のたびに
     * decisions() を読み、返った判断だけを adapter に実行させる。
     */
    pendingDecisions: readonly Decision[];
    /** 最後の tick イベントの時刻。今は保存するだけで、これに基づく判断はない。 */
    nowMs: number;
}

// 依存箇所の一覧の 9 つの id。Record<ContactId, true> にしておくと、ContactId (ledger.ts) が増えたり減ったりした
// ときに、ここを修正しなければ型チェックが失敗する。
const ALL_CONTACT_IDS_MAP: Record<ContactId, true> = {
    "pc-hook": true,
    "ssrc-map": true,
    sharing: true,
    "watch-stop": true,
    roster: true,
    profile: true,
    self: true,
    "call-info": true,
    "voice-select": true,
};
const ALL_CONTACT_IDS = Object.keys(ALL_CONTACT_IDS_MAP) as readonly ContactId[];

function initialContacts(): Readonly<Record<ContactId, ContactStatus>> {
    const entries = ALL_CONTACT_IDS.map((id) => [id, "unseen" as ContactStatus] as const);
    return Object.freeze(Object.fromEntries(entries)) as Readonly<Record<ContactId, ContactStatus>>;
}

export const initialState: CoreState = Object.freeze({
    contacts: initialContacts(),
    roster: "unknown",
    selfUserId: null,
    selection: Object.freeze([]),
    watchScope: Object.freeze([]),
    callName: null,
    mainCall: null,
    sharing: Object.freeze({}),
    ssrc: Object.freeze({}),
    videoPcIdByUser: Object.freeze({}),
    watching: Object.freeze({}),
    pendingDecisions: Object.freeze([]),
    nowMs: 0,
}) satisfies CoreState;

/**
 * 依存箇所 1 つが見つからないことを表すイベントを作る。依存箇所を 1 つずつ見つからなくしたテスト用の adapter
 * (tests/fake_adapter.ts) が、CONTACT_LEDGER の行ごとに使う。
 */
export function missingContactEvent(id: ContactId): CoreEvent {
    return { type: "contact", id, result: "missing" };
}

function reduceContact(state: CoreState, event: { id: ContactId; result: "found" | "missing" | "lost" }): CoreState {
    const prevStatus = state.contacts[event.id];
    let nextStatus: ContactStatus;
    if (event.result === "found") {
        nextStatus = "found";
    } else if (event.result === "lost") {
        nextStatus = "lost";
    } else {
        // event.result は "missing"。"found" か "lost" の後に来た "missing" だけを、使えなくなった ("lost") と
        // して扱う。1 度も "found" になっていない "missing" は "missing" のまま。
        nextStatus = prevStatus === "found" || prevStatus === "lost" ? "lost" : "missing";
    }

    return {
        ...state,
        contacts: { ...state.contacts, [event.id]: nextStatus },
        // 依存箇所の状態の変化からは判断を作らない (依存箇所が壊れたことを理由に、送っている映像と音声を止めない)。
        pendingDecisions: [],
    };
}

/**
 * selection と sharing と watchScope の現在の値から視聴する人を求め直し、watching との違いを start-watching と stop-watching に
 * する。sharing イベントの後でも selection イベントの後でも、同じように呼ぶ。
 */
function recomputeWatching(base: CoreState): CoreState {
    const desired = new Map<string, string>();
    const inScope = base.watchScope === null ? null : new Set(base.watchScope);
    for (const userId of base.selection) {
        if (inScope !== null && !inScope.has(userId)) continue;
        const info = base.sharing[userId];
        if (info && info.active) {
            desired.set(userId, info.streamKey);
        }
    }

    const decisions: Decision[] = [];
    const nextWatching: Record<string, string> = {};

    for (const [userId, streamKey] of desired) {
        const prevStreamKey = base.watching[userId];
        if (prevStreamKey === undefined) {
            decisions.push({ type: "start-watching", userId, streamKey });
        } else if (prevStreamKey !== streamKey) {
            // 同じ人が別の画面共有に切り替えた。同じ人の 2 つの画面共有を同時に視聴しないように、古いほうを
            // 止めてから新しいほうを始める。
            decisions.push({ type: "stop-watching", userId, streamKey: prevStreamKey });
            decisions.push({ type: "start-watching", userId, streamKey });
        }
        nextWatching[userId] = streamKey;
    }

    for (const [userId, streamKey] of Object.entries(base.watching)) {
        if (!desired.has(userId)) {
            decisions.push({ type: "stop-watching", userId, streamKey });
        }
    }

    return { ...base, watching: nextWatching, pendingDecisions: decisions };
}

function reduceSharing(
    state: CoreState,
    event: { userId: string; streamKey: string; active: boolean; pcId: number },
): CoreState {
    const nextSharing: Record<string, SharingInfo> = {
        ...state.sharing,
        [event.userId]: { streamKey: event.streamKey, pcId: event.pcId, active: event.active },
    };
    return recomputeWatching({ ...state, sharing: nextSharing });
}

function reduceSelection(state: CoreState, event: { userIds: readonly string[] }): CoreState {
    return recomputeWatching({ ...state, selection: event.userIds });
}

function reduceWatchScope(state: CoreState, event: { userIds: readonly string[] | null }): CoreState {
    return recomputeWatching({ ...state, watchScope: event.userIds });
}

function reduceSsrc(
    state: CoreState,
    event: { userId: string; videoSsrc: number | null; audioSsrc: number | null; pcId: number },
): CoreState {
    const key = ssrcKey(event.userId, event.pcId);
    const prevAtKey = state.ssrc[key];
    const decisions: Decision[] = [];

    // 映像の送信元が替わったかは、映像を運ぶイベント (videoSsrc が null でない) だけで判定する。マイクの ssrc の
    // イベント (ボイス接続の voice gateway から来るもの) は videoSsrc が常に null なので、ここで除かないと、画面共有
    // しながら話しただけで pcId が変わって見え、mark-source-switched を誤って出す。
    if (event.videoSsrc !== null) {
        const priorVideoPcId = state.videoPcIdByUser[event.userId];
        const pcChanged = priorVideoPcId !== undefined && priorVideoPcId !== event.pcId;
        const prevKnownVideo = prevAtKey?.lastNonNullVideoSsrc ?? null;
        const videoChanged = prevKnownVideo !== null && event.videoSsrc !== prevKnownVideo;
        // 前の枠に新しい映像を表示しないようにするのは、受信側の役目である。core は替わったことを伝えるだけ。
        if (pcChanged || videoChanged) {
            decisions.push({ type: "mark-source-switched", userId: event.userId, pcId: event.pcId });
        }
    }

    const nextLastNonNullVideo =
        event.videoSsrc !== null ? event.videoSsrc : (prevAtKey?.lastNonNullVideoSsrc ?? null);

    return {
        ...state,
        ssrc: {
            ...state.ssrc,
            [key]: {
                videoSsrc: event.videoSsrc,
                lastNonNullVideoSsrc: nextLastNonNullVideo,
                audioSsrc: event.audioSsrc,
            },
        },
        videoPcIdByUser:
            event.videoSsrc !== null
                ? { ...state.videoPcIdByUser, [event.userId]: event.pcId }
                : state.videoPcIdByUser,
        pendingDecisions: decisions,
    };
}

/**
 * watch_result (adapter が startWatching() を実行した結果) を反映する。ok が true なら何もしない (start-watching を
 * 決めた時点で watching に入れてある)。ok が false で、その要求がまだ最新 (watching が同じ streamKey) なら取り消し、
 * 次に sharing か selection が変わったときに start-watching を出し直せるようにする。探し直す処理は adapter が持ち、
 * ここでは core の watching を戻すだけ。遅れて届いた古い結果 (その間に別の streamKey に替わった、など) は、
 * 新しい watching を壊さないように無視する。
 */
function reduceWatchResult(
    state: CoreState,
    event: { userId: string; streamKey: string; ok: boolean },
): CoreState {
    if (event.ok) return { ...state, pendingDecisions: [] };
    if (state.watching[event.userId] !== event.streamKey) return { ...state, pendingDecisions: [] };

    const nextWatching = { ...state.watching };
    delete nextWatching[event.userId];
    return { ...state, watching: nextWatching, pendingDecisions: [] };
}

/** core のただ 1 つの遷移関数。1 つのイベントを状態に反映した、新しい状態を返す。副作用はない。 */
export function reduce(state: CoreState, event: CoreEvent): CoreState {
    switch (event.type) {
        case "contact":
            return reduceContact(state, event);
        case "roster":
            // 読めなかった間は、このイベントが来ない (adapter は代わりに roster の contact の missing を送る)。
            // 0 人 (空の配列) は、分からない ("unknown" のまま) と区別して、そのまま入れる。selection は変えない。
            return { ...state, roster: event.users, pendingDecisions: [] };
        case "sharing":
            return reduceSharing(state, event);
        case "ssrc":
            return reduceSsrc(state, event);
        case "self":
            return { ...state, selfUserId: event.userId, pendingDecisions: [] };
        case "selection":
            return reduceSelection(state, event);
        case "watch_scope":
            return reduceWatchScope(state, event);
        case "call_name":
            return { ...state, callName: event.name, pendingDecisions: [] };
        case "main_call":
            return { ...state, mainCall: event.call, pendingDecisions: [] };
        case "watch_result":
            return reduceWatchResult(state, event);
        case "tick":
            return { ...state, nowMs: event.nowMs, pendingDecisions: [] };
    }
}

/** 警告を返す。警告は、依存箇所の "missing" と "lost" からだけ作る。 */
export function warnings(state: CoreState): readonly WarningId[] {
    const result: WarningId[] = [];
    for (const id of ALL_CONTACT_IDS) {
        const status = state.contacts[id];
        if (status === "missing" || status === "lost") {
            result.push(id);
        }
    }
    return result;
}

/** 今続いている画面共有の一覧。 */
export function sharing(state: CoreState): readonly SharingUser[] {
    const result: SharingUser[] = [];
    for (const [userId, info] of Object.entries(state.sharing)) {
        if (info.active) {
            result.push({ userId, streamKey: info.streamKey, pcId: info.pcId });
        }
    }
    return result;
}

/**
 * ある userId から届いた音声を、声として扱ってよいかを返す。呼び出し元 (index.ts) は接続の番号 (pcId が
 * VOICE_PC_ID か) で声かどうかを決めるが、その確認を補う。state.sharing で、その人の画面共有の pcId が VOICE_PC_ID
 * なら、その音声は画面共有の音かもしれないので、声として扱わない。adapter/connections.ts は voice gateway の
 * server_id でボイス接続を見分けるので、Go Live の接続が pcId 1 になることはなく、ふつうはいつも true を返す。
 */
export function isPlausibleVoiceSource(state: CoreState, userId: string): boolean {
    const sharingInfo = state.sharing[userId];
    return !(sharingInfo?.active && sharingInfo.pcId === VOICE_PC_ID);
}

/** 最後の reduce() で新しく出た判断だけを返す。次の reduce() で置き換わるので、reduce() のたびに読む。 */
export function decisions(state: CoreState): readonly Decision[] {
    return state.pendingDecisions;
}

/**
 * voice gateway の ssrc の事実から、トラックの userId を特定する。roster にいる人を順に、指定した pcId の
 * ssrcKey(userId, pcId) で引き、videoSsrc か audioSsrc が一致した人を返す。指定した pcId の値しか見ないので、
 * 別の接続の ssrc と取り違えない (SSRC の番号は、接続をまたいで重なることがある)。roster が読めていない
 * ("unknown") か、一致する人がいなければ undefined を返し、値を推測しない。ボイス接続の pcId (1) は、ボイス接続が
 * 作り直されても同じ番号なので、前の接続の値は adapter (adapter/connections.ts) が null の ssrc を送って消す
 * (ここでは区別しない)。
 */
export function userIdForSsrc(state: CoreState, pcId: number, ssrc: number): string | undefined {
    if (state.roster === "unknown") return undefined;
    for (const user of state.roster) {
        const info = state.ssrc[ssrcKey(user.userId, pcId)];
        if (info === undefined) continue;
        if (info.videoSsrc === ssrc || info.audioSsrc === ssrc) return user.userId;
    }
    return undefined;
}

/**
 * トラックが届いた時点で分かっている手がかりから、userId を今すぐ特定する。手がかりは、使う順に次の 3 つである。
 * 接続で画面共有している人 (owner。Go Live の接続だけ)、トラックの msid が示す人 (declared。voice gateway が同じ接続の
 * SSRC を同じ人のものと伝えているときだけ使う)、トラックに届いた音声や映像の SSRC。msid を SSRC より先にするのは、
 * まだ話していない人の声のトラックには音声が届かず、SSRC が空のままで、最初の一言まで特定できなくなるためである。
 * どれでも特定できなければ undefined を返し、呼び出し元が少し待ってからやり直す。
 */
export function userIdForTrackNow(
    state: CoreState,
    pcId: number,
    clues: {
        readonly owner: string | undefined;
        readonly declared: { readonly userId: string; readonly ssrc: number } | null;
        readonly ssrcs: readonly number[];
    },
): string | undefined {
    if (clues.owner !== undefined) return clues.owner;
    if (clues.declared !== null && userIdForSsrc(state, pcId, clues.declared.ssrc) === clues.declared.userId) {
        return clues.declared.userId;
    }
    for (const ssrc of clues.ssrcs) {
        const userId = userIdForSsrc(state, pcId, ssrc);
        if (userId !== undefined) return userId;
    }
    return undefined;
}
