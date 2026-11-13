// SPDX-License-Identifier: Apache-2.0

// voice gateway (WebSocket) と RTCPeerConnection を対応付け、接続ごとの pcId を決め、Go Live の接続で画面共有して
// いる人を特定する (ConnectionTracker)。人の画面共有を別の人の名前で配信に出すのが、いちばん避けるべき誤りなので、
// 時刻の近さではなく、接続そのものが持つ値 (op0 の server_id、ICE の ufrag、op12 の user_id) で決め、決まらなければ
// 推測せずにログに書いて捨てる。adapter/vencord.ts が実際の WebSocket と RTCPeerConnection のイベントを渡し、ここが
// 決めた pcId でトラックと ssrc の事実を送る。DOM の型を使わないので、リポジトリのルートの tsc と vitest で
// テストできる (tests/adapter_connections.test.ts)。

import type { AdapterEvent } from "./types";

/**
 * ボイス接続の pcId。core/state.ts と index.ts の VOICE_PC_ID と同じ値にする (どちらも、この pcId の接続から来たもの
 * だけを声として扱う)。
 */
export const VOICE_PC_ID = 1;
/** 行き先 (pcId) が決まらないトラックを保持する上限 (ms)。過ぎたらログに書いて捨てる。 */
export const TRACK_HOLD_MAX_MS = 5000;
/** 保持しているトラックを見直す間隔 (ms)。 */
export const TRACK_RECHECK_MS = 250;
/**
 * op2 を受けてからこの時間 (ms) 以内にできた RTCPeerConnection だけを、最後の手段の対応付けの候補にする。
 * RTCPeerConnection は、対応する WebSocket の op2 の約 5 ms 後にできる。
 */
export const OP2_ADJACENCY_MS = 50;
/**
 * まだ対応付けていない RTCPeerConnection の情報を持つ数の上限。RTCPeerConnection は WeakRef で参照するが、情報その
 * ものは対応付けが決まるまで残るので、数を抑える。目安の値で、測って決めたものではない。1 回の通話でできる接続は、
 * ボイス接続の 1 つと視聴している画面共有の数だけなので、それより十分に多くした。
 */
const MAX_UNPAIRED_PCS = 32;
/**
 * 閉じた WebSocket の情報を持つ数の上限。op7 の続きの元を探すために持ち、超えたら古いものから捨てる。目安の値で、
 * 測って決めたものではない (MAX_UNPAIRED_PCS と同じ考え方)。
 */
const MAX_CLOSED_WS = 16;
/** 分類が決まる前に保存しておく ssrc の事実の上限 (ボイス接続の WebSocket では、参加者ごとに 1 つ)。 */
const MAX_BUFFERED_FACTS = 64;

// voice gateway の opcode。Discord の voice gateway について、実際の通話で確認した事実は次のとおり。
// - voice gateway (*.discord.media) は、どれも op0 (IDENTIFY) で d.server_id を送る。サーバーのボイスチャンネル
//   では、ボイス接続の server_id はそのサーバーの guild id で、Go Live の接続の server_id は画面共有ごとに違う。
// - 1 つの接続の中の順は、WebSocket ができる、op0 を送る、op8 と op2 を受ける、RTCPeerConnection ができる (その
//   WebSocket の op2 の約 5 ms 後)、op1 を送る、op4 を受ける、ontrack (必ず op4 の後) である。
// - Go Live の WebSocket は、トラックが届く前に op12 (user_id は画面共有している人、audio_ssrc、video_ssrc、streams)
//   を受ける。Go Live の WebSocket の op5 の user_id も画面共有している人で、op12 の直前に来ることがある。
// - トラックの msid (streams[0].id) は "<userId>-<ssrc>" の形。Go Live の接続では、画面共有している人の id と、
//   op12 が伝えた ssrc になる。
// - SSRC の番号はサーバーをまたいで重なる。ある人のボイス接続のマイクと、別の人の Go Live の映像が、同じ番号の
//   ことがあった。
// - 視聴を始める途中で、ボイス接続が作り直されることがある (Vesktop を再読み込みした後に、通話に参加し直したとき)。
// - 2 つの Go Live の WebSocket が 1 ms 違いでできることがあり、別々の接続の WebSocket と RTCPeerConnection の生成は
//   前後が入り組む。
const OP_IDENTIFY = 0;
const OP_SELECT_PROTOCOL = 1;
const OP_READY = 2;
const OP_SESSION_DESCRIPTION = 4;
const OP_SPEAKING = 5;
const OP_RESUME = 7;
const OP_VIDEO = 12;

/** voice gateway のメッセージ 1 つ (JSON の op と d だけ)。 */
export interface GatewayFrame {
    op: number;
    d: Record<string, unknown>;
}

/** voice gateway のメッセージ (JSON の文字列) を読む。文字列でない (バイナリの) メッセージと、壊れたものは null。 */
export function parseGatewayFrame(data: unknown): GatewayFrame | null {
    if (typeof data !== "string") return null;
    try {
        const parsed = JSON.parse(data) as unknown;
        if (parsed === null || typeof parsed !== "object") return null;
        const op = (parsed as Record<string, unknown>).op;
        const d = (parsed as Record<string, unknown>).d;
        if (typeof op !== "number" || d === null || typeof d !== "object") return null;
        return { op, d: d as Record<string, unknown> };
    } catch {
        return null;
    }
}

const ICE_UFRAG_RE = /a=ice-ufrag:(\S+)/;

/** SDP の文字列から、最初の ICE の ufrag を取り出す。文字列でないときと、含まないときは null。 */
export function extractIceUfrag(sdp: unknown): string | null {
    if (typeof sdp !== "string") return null;
    const m = sdp.match(ICE_UFRAG_RE);
    return m?.[1] ?? null;
}

/**
 * SDP の映像の m 行に、受けるだけの側も往復の時間を測る指定 (標準の a=rtcp-xr の rcvr-rtt と、古い a=rtcp-fb:<番号> rrtr)
 * があるか。相手の SDP にこれがあると、WebRTC は送らない側でも往復の時間を測り、受けた映像のタイムスタンプが、描く予定の
 * 時刻から送った人の取り込みの時刻に変わる (encode/video_time.ts)。読むだけで、SDP の中身はログに書かない。文字列でなければ
 * null。
 */
export function videoNonSenderRtt(sdp: unknown): boolean | null {
    if (typeof sdp !== "string") return null;
    let inVideo = false;
    for (const line of sdp.split(/\r?\n/)) {
        if (line.startsWith("m=")) inVideo = line.startsWith("m=video");
        else if (inVideo && (/^a=rtcp-xr:.*rcvr-rtt/.test(line) || /^a=rtcp-fb:\S+ rrtr\b/.test(line))) return true;
    }
    return false;
}

const MSID_RE = /^(\d{15,22})-(\d{1,10})$/;

/** トラックの msid ("<userId>-<ssrc>"。確認した通話では、どのトラックもこの形だった) を読む。形が違えば null。 */
export function parseMsid(msid: string | null | undefined): { userId: string; ssrc: number } | null {
    if (typeof msid !== "string") return null;
    const m = msid.match(MSID_RE);
    if (m === null) return null;
    const userId = m[1];
    const ssrcText = m[2];
    if (userId === undefined || ssrcText === undefined) return null;
    return { userId, ssrc: Number(ssrcText) };
}

/** RTCPeerConnection の remoteDescription と localDescription の SDP (読めなければ null)。 */
export interface PcDescriptions {
    remote: string | null;
    local: string | null;
}

/** ontrack で届いたトラック 1 つ。行き先 (pcId) が決まったら、deliver を 1 回だけ呼ぶ。 */
export interface IncomingTrack {
    kind: string;
    trackId: string;
    /** `ev.streams[0].id`。なければ null。 */
    msid: string | null;
    /** もう終わったトラックか。終わったトラックを後から送ると、track_removed が出ないまま残るので送らない。 */
    isEnded(): boolean;
    deliver(pcId: number): void;
}

export interface ConnectionTrackerDeps {
    /** ssrc の CoreEvent を送る関数。 */
    emit(event: AdapterEvent): void;
    /** 送信側のログに 1 行書く関数。 */
    diag(line: string): void;
    /** 現在の通話の server id (guild id、なければ channel id)。読めなければ null。 */
    callServerId(): string | null;
    /** 時刻 (ms) を返す関数。テストで差し替える。省くと performance.now()。 */
    now?: () => number;
    /** fn を ms 後に 1 回呼ぶ。戻り値は取り消す関数。省くと setTimeout。 */
    schedule?: (fn: () => void, ms: number) => () => void;
}

type WsRole = "unclassified" | "call" | "golive";

type SsrcFact =
    | { op: typeof OP_SPEAKING; userId: string | null; ssrc: number }
    | { op: typeof OP_VIDEO; userId: string | null; audioSsrc: number | null; videoSsrc: number | null };

interface WsRecord {
    serial: number;
    /** op0 か op7 で送った server_id。まだ送っていない、読めないときは null。 */
    serverId: string | null;
    /** op0 (新しいセッション) と op7 (再接続) のどちらで始めたか。まだ送っていなければ null。 */
    start: "identify" | "resume" | null;
    role: WsRole;
    /** 分類が決まるまでは 0。 */
    pcId: number;
    /** ボイス接続のセッション (そのセッションを op0 で始めた WebSocket の通し番号)。ボイス接続でなければ 0。 */
    callSession: number;
    /** 閉じた。閉じた WebSocket の情報は、op7 の続きの元として MAX_CLOSED_WS 個まで残す。 */
    closed: boolean;
    op2At: number | null;
    /** op4 で受けた SDP の ufrag (ログには書かない)。 */
    remoteUfrag: string | null;
    /** op1 で送った SDP の ufrag (ログには書かない)。 */
    localUfrag: string | null;
    /**
     * どれかの RTCPeerConnection と対応付けたか (op2 の近さで対応付けるときに、まだ対応付けていない WebSocket だけを
     * 候補にするため)。
     */
    paired: boolean;
    op12UserIds: Set<string>;
    op5UserIds: Set<string>;
    /** stopWatching で手放した (画面共有している人を答えず、ssrc も送らない)。 */
    forgotten: boolean;
    /** 分類が決まる前に届いた ssrc の事実。キーごとに最新の 1 つ。 */
    buffered: Map<string, SsrcFact>;
    seenOps: Set<number>;
    seenSentOps: Set<number>;
    seenSpeakingSsrcs: Set<number>;
    loggedCallUnknown: boolean;
    loggedFactDropped: boolean;
}

interface PcRecord {
    serial: number;
    createdAt: number;
    /** 対応付けが決まるまでだけ持つ (決まったら null にして、RTCPeerConnection を参照する関数を手放す)。 */
    readSdp: (() => PcDescriptions) | null;
    wsSerial: number | null;
    /**
     * 対応付けた WebSocket の分類のコピー (WebSocket の情報が先に消えても、ボイス接続の RTCPeerConnection は pcId 1 の
     * ままにするため)。
     */
    role: WsRole;
    pcId: number;
    /** 対応付けた WebSocket のセッションのコピー (ボイス接続でなければ 0)。 */
    callSession: number;
    loggedNotPaired: Set<string>;
}

type HoldReason = "unpaired" | "unclassified" | "owner-unknown" | "ws-closed";

interface HeldTrack {
    pcSerial: number;
    track: IncomingTrack;
    since: number;
}

type RouteResult = { kind: "done" } | { kind: "hold"; reason: HoldReason };

const DONE: RouteResult = { kind: "done" };

function hold(reason: HoldReason): RouteResult {
    return { kind: "hold", reason };
}

function firstOf(set: ReadonlySet<string>): string | undefined {
    for (const value of set) return value;
    return undefined;
}

function numberOrNull(value: unknown): number | null {
    return typeof value === "number" ? value : null;
}

function nonEmptyString(value: unknown): string | null {
    return typeof value === "string" && value !== "" ? value : null;
}

/**
 * voice gateway と RTCPeerConnection のイベントを受け取り、接続ごとの pcId を決め、Go Live の接続で画面共有している
 * 人を特定する。外から呼ぶメソッドは、どれも例外を投げない。Discord の WebSocket と RTCPeerConnection の処理の中から
 * 呼ばれるので、例外で Discord の処理を止めないためである。決め方は次のとおり。
 *
 * (1) 分類: op0 を送ったときの server_id を、そのときの通話の server id (RTCConnectionStore の guild id、なければ
 *     channel id) と比べ、同じならボイス接続 (pcId 1)、違えば Go Live (2 以上の新しい番号。使い回さない) にする。
 *     通話の server id が読めなければ分類を待ち、その WebSocket にメッセージが届くたびと、保持しているトラックを
 *     見直すたびに試し直す。待つ間は ssrc の事実を送らず、op5 は参加者ごとに、op12 は 1 つ、最新のものを保存しておき、
 *     分類が決まったときに送る。op0 のログの行には d.streams の type の一覧も書く (今は判断には使わない)。
 * (2) 再接続: op7 (RESUME) を送った WebSocket は、それより前にできて同じ server_id を送っていた WebSocket の続きとし、
 *     分類、pcId、画面共有している人の手がかり、ufrag、forgotten を引き継ぐ。元の WebSocket に対応付けていた
 *     RTCPeerConnection は、新しい WebSocket に付け替える。RESUME では RTCPeerConnection も SSRC も続くためである。
 *     続きの元がなければ (1) と同じに分類する。
 * (3) ボイス接続のセッション: op0 で始めたボイス接続の WebSocket のうち、通し番号が最も新しいものを現在のセッションと
 *     する。セッションが替わったら、それまで pcId 1 で ssrc を送った参加者ごとに ssrc を null にしたイベントを送って
 *     core の値を消し、古いセッションの WebSocket の op5 と、古いセッションの RTCPeerConnection のトラックは送らない。
 *     SSRC の番号はサーバーをまたいで重なるので、古いセッションの事実で、新しいセッションのトラックの userId を
 *     特定しないためである。ボイス接続の値を消すのに専用の CoreEvent は追加しない。ssrc のイベントは読めない値を
 *     null で運ぶので、null で送り直せば同じ結果になる。
 * (4) 対応付け: WebSocket ごとに、受けた op4 の d.sdp と、送った op1 の d.sdp (なければ文字列の d.data) から ICE の
 *     ufrag を取り出して持つ。トラックが届いたときに RTCPeerConnection の remoteDescription と localDescription の
 *     ufrag を読み、op4 の ufrag が remote と同じ WebSocket と、op1 の ufrag が local と同じ WebSocket を探す。1 つに
 *     決まり、2 つが別々の WebSocket を指さないときだけ対応付ける。remote の ufrag は Go Live のサーバーが使い回す
 *     かもしれないので、remote で複数が一致したら local との重なりで 1 つに絞る。どちらも一致しないときだけ、最後の
 *     手段として、その RTCPeerConnection ができる前 50 ms 以内に op2 を受けた、まだ対応付けていない開いた WebSocket
 *     がちょうど 1 つなら対応付ける。時刻の近さを主な方法にしないのは、別々の接続の生成が入り組むので取り違えるため
 *     である。使った方法 (remote-ufrag、local-ufrag、op2-adjacency) をログに書く。
 * (5) pcId: RTCPeerConnection の pcId は、対応付けた WebSocket の pcId にし、推測はしない。まだ対応付けていない、
 *     分類が決まっていない、画面共有している人が分からない、Go Live の WebSocket が閉じて (2) の続きを待っている、
 *     のどれかに当たる RTCPeerConnection のトラックは保持し、250 ms ごとに見直し、5 秒たっても決まらなければログに
 *     書いて捨てる。
 * (6) 画面共有している人: Go Live の接続で画面共有している人は、その WebSocket が受けた op12 の user_id (op12 が
 *     まだなければ op5 の user_id) にする。どちらかに別々の人が 2 人以上出たら、分からないとする。Go Live の
 *     トラックは、msid の userId がその人と違えば送らずにログに書く (msid が "<userId>-<ssrc>" の形でなければ照合
 *     しない)。msid だけで userId を特定しないのは、同じ人のマイクと画面共有の音声が同じ userId を持ち、どちらの
 *     接続のものかが msid から分からないためである。ボイス接続のトラックは照合しない (何人ものマイクが乗るので、
 *     userId は core が SSRC で特定する)。
 * (7) stopWatching() の後: その人が画面共有している接続を forget(pcId) で手放す。WebSocket が閉じたら、その接続で
 *     画面共有している人を答えない。
 *
 * ICE の ufrag は対応付けのためだけに文字列で持ち、ログには書かない。op0 と op7 の token と session_id、op4 の
 * secret_key と SDP の残りは持たず、ログにも書かない。
 *
 * 実際の通話で確認していないことは次のとおり。op7 で再接続する場面は見ていない ((2) は voice gateway の RESUME の
 * 仕様から決めた)。DM とグループ通話では、通話の server id に channel id を使うが、op0 の server_id が channel id に
 * なるかは確認していない (DM の通話で op0 のログの行を見れば分かる)。op0 のときに RTCConnectionStore が別の通話を
 * 指していると、分類を誤る。ボイス接続の WebSocket を Go Live と取り違えた場合、op5 に何人も出れば画面共有している人を
 * 特定できずトラックは送られないが、まだ 1 人しか話していなければ、その人のマイクがその人の画面共有の音声として
 * 送られうる。セッションが替わる直前に送ったボイス接続のトラックの userId を、index.ts が SSRC でまだ特定している
 * 途中 (最初のパケットを待つ間) にセッションが替わると、新しいセッションの事実で特定してしまうことがある (トラックは
 * op5 の後にでき、話している人の音声はすぐに届くので、その間は短い)。
 */
export class ConnectionTracker {
    readonly #deps: ConnectionTrackerDeps;
    readonly #now: () => number;
    readonly #schedule: (fn: () => void, ms: number) => () => void;
    // 1 はボイス接続の番号。Go Live の番号はここから増やすだけで、使い回さない。
    #nextPcId = VOICE_PC_ID + 1;
    #nextWsSerial = 0;
    #nextPcSerial = 0;
    readonly #ws = new Map<number, WsRecord>();
    readonly #pcs = new Map<number, PcRecord>();
    /** 閉じた WebSocket の通し番号 (閉じた順)。MAX_CLOSED_WS 個を超えたら、古いものから情報を捨てる。 */
    #closedOrder: number[] = [];
    /** 現在のボイス接続のセッション。まだボイス接続の WebSocket を分類していなければ 0。 */
    #currentCallSession = 0;
    /** 現在のセッションで pcId 1 の ssrc を送った人 (セッションが替わったときに値を消す相手)。 */
    readonly #callFactUserIds = new Set<string>();
    #held: HeldTrack[] = [];
    #cancelTimer: (() => void) | null = null;

    constructor(deps: ConnectionTrackerDeps) {
        this.#deps = deps;
        // 間隔 (保持の 5 秒、op2 の近さの 50 ms) を測るだけなので、システム時刻ではなく単調な時計を使う (OS の時刻
        // 合わせやスリープからの復帰でシステム時刻が不連続に変わると、トラックを早く捨てたり、いつまでも持ったりする)。
        this.#now = deps.now ?? (() => performance.now());
        this.#schedule = deps.schedule ?? ((fn, ms) => {
            const handle = setTimeout(fn, ms);
            return () => clearTimeout(handle);
        });
    }

    /* ---------------- voice gateway ---------------- */

    /** voice gateway の WebSocket ができた。この WebSocket の通し番号を返す。 */
    wsOpened(): number {
        const serial = ++this.#nextWsSerial;
        try {
            this.#ws.set(serial, {
                serial,
                serverId: null,
                start: null,
                role: "unclassified",
                pcId: 0,
                callSession: 0,
                closed: false,
                op2At: null,
                remoteUfrag: null,
                localUfrag: null,
                paired: false,
                op12UserIds: new Set(),
                op5UserIds: new Set(),
                forgotten: false,
                buffered: new Map(),
                seenOps: new Set(),
                seenSentOps: new Set(),
                seenSpeakingSsrcs: new Set(),
                loggedCallUnknown: false,
                loggedFactDropped: false,
            });
            this.#log(`ws created: ws=${serial}`);
        } catch {
            // 情報を作れなくても WebSocket は動かす (この WebSocket の事実が送られないだけ)。
        }
        return serial;
    }

    /** その WebSocket から送ったメッセージ。op0 で分類し、op7 で続きの元を探し、op1 で local の ufrag を保存する。 */
    wsSent(serial: number, frame: GatewayFrame): void {
        try {
            const ws = this.#ws.get(serial);
            if (ws === undefined) return;
            const d = frame.d;
            if (frame.op === OP_IDENTIFY || frame.op === OP_RESUME) {
                // ログに書くのは server_id、d.video、streams の数と type だけ (token と session_id は書かない)。
                ws.serverId = nonEmptyString(d.server_id);
                if (ws.start === null) ws.start = frame.op === OP_RESUME ? "resume" : "identify";
                const streams = Array.isArray(d.streams) ? (d.streams as unknown[]) : null;
                const types = streams === null
                    ? "none"
                    : streams
                        .map(st => (st !== null && typeof st === "object" ? String((st as Record<string, unknown>).type) : "?"))
                        .join("/");
                this.#log(
                    `ws send op${frame.op}: ws=${serial} server_id=${ws.serverId ?? "none"} video=${String(d.video)}` +
                    ` streams=${streams === null ? "none" : streams.length} types=${types === "" ? "empty" : types}`,
                );
                if (this.#tryClassify(ws, `op${frame.op}`)) this.#recheckHeld();
                return;
            }
            if (frame.op === OP_SELECT_PROTOCOL) {
                const ufrag = extractIceUfrag(d.sdp) ?? extractIceUfrag(d.data);
                if (ufrag !== null) ws.localUfrag = ufrag;
            }
            if (!ws.seenSentOps.has(frame.op)) {
                ws.seenSentOps.add(frame.op);
                this.#log(`ws send op seen: ws=${serial} pcId=${ws.pcId} op=${frame.op}`);
            }
            if (frame.op === OP_SELECT_PROTOCOL) this.#recheckHeld();
        } catch {
            // 1 つのメッセージを読めなくても、Discord の通話は止めない。
        }
    }

    /** その WebSocket が受け取ったメッセージ。 */
    wsReceived(serial: number, frame: GatewayFrame): void {
        try {
            const ws = this.#ws.get(serial);
            if (ws === undefined) return;
            // 通話の server id が読めずに分類を待っている WebSocket は、メッセージが届くたびに試し直す。
            let changed = this.#tryClassify(ws, `op${frame.op}`);
            const d = frame.d;
            switch (frame.op) {
                case OP_READY:
                    ws.op2At = this.#now();
                    this.#logOpSeen(ws, frame.op);
                    break;
                case OP_SESSION_DESCRIPTION: {
                    // SDP からは ufrag だけを取り出す (secret_key と SDP の残りは持たず、ログにも書かない)。
                    const ufrag = extractIceUfrag(d.sdp);
                    if (ufrag !== null) ws.remoteUfrag = ufrag;
                    this.#logOpSeen(ws, frame.op);
                    changed = true;
                    break;
                }
                case OP_SPEAKING:
                    if (this.#onSpeaking(ws, d)) changed = true;
                    break;
                case OP_VIDEO:
                    if (this.#onVideo(ws, d)) changed = true;
                    break;
                default:
                    this.#logOpSeen(ws, frame.op);
            }
            if (changed) this.#recheckHeld();
        } catch {
            // 1 つのメッセージを読めなくても、Discord の通話は止めない。
        }
    }

    /**
     * その WebSocket が閉じた。Go Live なら、画面共有している人を答えなくなる。情報は op7 の続きの元として
     * MAX_CLOSED_WS 個まで残す (ufrag も残すので、まだトラックの来ていない RTCPeerConnection を後から対応付けられる)。
     */
    wsClosed(serial: number): void {
        try {
            const ws = this.#ws.get(serial);
            if (ws === undefined || ws.closed) return;
            ws.closed = true;
            this.#closedOrder.push(serial);
            this.#log(`ws closed: ws=${serial} role=${ws.role} pcId=${ws.pcId}`);
            this.#dropOldClosedWs();
        } catch {
            // 後始末に失敗しても、Discord の通話は止めない。
        }
    }

    /* ---------------- RTCPeerConnection ---------------- */

    /** RTCPeerConnection ができた。readSdp は対応付けを決めるときに呼ぶ (決まったら手放す)。通し番号を返す。 */
    pcCreated(readSdp: () => PcDescriptions): number {
        const serial = ++this.#nextPcSerial;
        try {
            this.#pcs.set(serial, {
                serial,
                createdAt: this.#now(),
                readSdp,
                wsSerial: null,
                role: "unclassified",
                pcId: 0,
                callSession: 0,
                loggedNotPaired: new Set(),
            });
            this.#log(`pc created: pc=${serial}`);
            this.#pruneUnpairedPcs();
        } catch {
            // 情報を作れなくても、接続は動かす。
        }
        return serial;
    }

    /**
     * RTCPeerConnection が閉じた (close() が呼ばれた)。その情報と、保持しているそのトラックを捨てる。この後に、その
     * RTCPeerConnection に届くトラックは送らない。何度呼んでもよい。
     */
    pcClosed(serial: number): void {
        try {
            if (!this.#pcs.delete(serial)) return;
            const before = this.#held.length;
            this.#held = this.#held.filter(h => h.pcSerial !== serial);
            this.#log(`pc closed: pc=${serial} held_dropped=${before - this.#held.length}`);
        } catch {
            // 後始末に失敗しても、Discord の通話は止めない。
        }
    }

    /** ontrack で届いたトラック。行き先が決まれば deliver を呼び、決まらなければ保持する。 */
    trackArrived(pcSerial: number, track: IncomingTrack): void {
        try {
            this.#log(`pc track: pc=${pcSerial} kind=${track.kind} trackId=${track.trackId} msid=${track.msid ?? "none"}`);
            // 同じ RTCPeerConnection に先に届いたトラックを先に送る (映像と音声の順を入れ替えない)。
            this.#recheckHeld();
            const result = this.#route(pcSerial, track);
            if (result.kind === "hold") {
                this.#held.push({ pcSerial, track, since: this.#now() });
                this.#log(`track held: pc=${pcSerial} trackId=${track.trackId} reason=${result.reason}`);
                this.#ensureTimer();
            }
        } catch {
            // 1 つのトラックの処理に失敗しても、Discord の通話は止めない。
        }
    }

    /* ---------------- 画面共有している人の問い合わせ ---------------- */

    /**
     * その pcId の Go Live の接続で画面共有している人の userId。ボイス接続、閉じた接続、手放した接続、分からない
     * ときは undefined。
     */
    ownerOf(pcId: number): string | undefined {
        try {
            if (pcId === VOICE_PC_ID) return undefined;
            for (const ws of this.#ws.values()) {
                if (ws.role !== "golive" || ws.pcId !== pcId) continue;
                const owner = this.#ownerOfWs(ws);
                if (owner !== undefined) return owner;
            }
        } catch {
            // 読めなければ、分からないと答える。
        }
        return undefined;
    }

    /** その人が画面共有している、開いている Go Live の接続の pcId (複数あれば最も新しいもの)。なければ 0。 */
    pcIdForUser(userId: string): number {
        try {
            let best: WsRecord | null = null;
            for (const ws of this.#ws.values()) {
                if (ws.role !== "golive" || this.#ownerOfWs(ws) !== userId) continue;
                if (best === null || ws.serial > best.serial) best = ws;
            }
            return best?.pcId ?? 0;
        } catch {
            return 0;
        }
    }

    /**
     * その pcId の接続を手放す (stopWatching)。この後は画面共有している人を答えず、ssrc もトラックも送らない。
     * 閉じた WebSocket の情報にも forgotten を付ける (op7 で再接続した続きが引き継ぐ)。
     */
    forget(pcId: number): void {
        try {
            for (const ws of this.#ws.values()) {
                if (ws.role === "golive" && ws.pcId === pcId && !ws.forgotten) {
                    ws.forgotten = true;
                    this.#log(`ws forgotten: ws=${ws.serial} pcId=${pcId}`);
                }
            }
        } catch {
            // 手放せなくても、処理は続ける。
        }
    }

    /** 見直しのタイマーを止め、保持しているトラックを捨てる。 */
    dispose(): void {
        if (this.#cancelTimer !== null) {
            try {
                this.#cancelTimer();
            } catch {
                // 止められなくても続ける。
            }
            this.#cancelTimer = null;
        }
        this.#held = [];
    }

    /* ---------------- 内部 ---------------- */

    #log(line: string): void {
        try {
            this.#deps.diag(line);
        } catch {
            // ログに書けなくても動かす。
        }
    }

    #emit(event: AdapterEvent): void {
        try {
            this.#deps.emit(event);
        } catch {
            // イベントを受け取る側の不具合で、Discord の通話を止めない。
        }
    }

    #logOpSeen(ws: WsRecord, op: number): void {
        if (ws.seenOps.has(op)) return;
        ws.seenOps.add(op);
        this.#log(`ws op seen: ws=${ws.serial} pcId=${ws.pcId} op=${op}`);
    }

    /**
     * 分類を試す。この呼び出しで分類が決まったら true を返す (保存していた ssrc の事実もここで送る)。op7 で始めた
     * WebSocket は、まず続きの元 (同じ server_id の前の WebSocket) を探す。
     */
    #tryClassify(ws: WsRecord, trigger: string): boolean {
        if (ws.role !== "unclassified" || ws.serverId === null) return false;
        if (ws.start === "resume" && this.#tryResume(ws, trigger)) {
            this.#flushBuffered(ws);
            return true;
        }
        let call: string | null;
        try {
            call = nonEmptyString(this.#deps.callServerId());
        } catch {
            call = null;
        }
        if (call === null) {
            if (!ws.loggedCallUnknown) {
                ws.loggedCallUnknown = true;
                this.#log(`ws unclassified: ws=${ws.serial} server_id=${ws.serverId} reason=call-server-id-unknown (${trigger})`);
            }
            return false;
        }
        if (ws.serverId === call) {
            ws.role = "call";
            ws.pcId = VOICE_PC_ID;
            // 続きの元のない op7 も、新しいセッションとする (SSRC が前のセッションから続いているかを確認できない
            // ため)。
            ws.callSession = ws.serial;
        } else {
            ws.role = "golive";
            ws.pcId = this.#nextPcId++;
        }
        this.#log(
            `ws classified: ws=${ws.serial} role=${ws.role} pcId=${ws.pcId} server_id=${ws.serverId}` +
            ` call_server_id=${call} (${trigger})`,
        );
        // 前のセッションの事実を消してから、この WebSocket の保存した事実を送る (逆の順にすると、新しい事実まで
        // 消える)。
        if (ws.role === "call") this.#enterCallSession(ws);
        this.#flushBuffered(ws);
        return true;
    }

    #flushBuffered(ws: WsRecord): void {
        const facts = [...ws.buffered.values()];
        ws.buffered.clear();
        for (const fact of facts) this.#emitFact(ws, fact);
    }

    /**
     * op7 で始めた WebSocket を、それより前にできて同じ server_id を送っていた WebSocket (分類が決まったもののうち
     * 最も新しいもの) の続きにする。見つかれば true を返す。まだ閉じていない WebSocket も元にする。Discord が応答の
     * 止まった WebSocket を自分で閉じて再接続すると、古い WebSocket の close のイベントは閉じる手順を待った後に届き、
     * 新しい WebSocket の op7 より遅れることがあるためである。server_id は Go Live なら画面共有ごとに違い、ボイス接続
     * なら guild (か channel) ごとに 1 つなので、同じ server_id の最も新しい前の WebSocket が、その op7 が続ける接続に
     * なる。元の情報は消すので、後から届く元の WebSocket の close やメッセージでは何もしない。
     */
    #tryResume(ws: WsRecord, trigger: string): boolean {
        let from: WsRecord | undefined;
        for (const candidate of this.#ws.values()) {
            if (candidate.serial >= ws.serial || candidate.role === "unclassified") continue;
            if (candidate.serverId !== ws.serverId) continue;
            if (from === undefined || candidate.serial > from.serial) from = candidate;
        }
        if (from === undefined) return false;
        const fromSerial = from.serial;
        ws.role = from.role;
        ws.pcId = from.pcId;
        ws.callSession = from.callSession;
        // RESUME では SDP を交換し直さないので、まだ対応付けていない RTCPeerConnection のために ufrag も引き継ぐ。
        ws.remoteUfrag = from.remoteUfrag;
        ws.localUfrag = from.localUfrag;
        ws.paired = from.paired;
        ws.op12UserIds = from.op12UserIds;
        ws.op5UserIds = from.op5UserIds;
        ws.forgotten = from.forgotten;
        let moved = 0;
        for (const pc of this.#pcs.values()) {
            if (pc.wsSerial !== fromSerial) continue;
            pc.wsSerial = ws.serial;
            moved++;
        }
        this.#ws.delete(fromSerial);
        this.#closedOrder = this.#closedOrder.filter(s => s !== fromSerial);
        this.#log(
            `ws resumed: ws=${ws.serial} from_ws=${fromSerial} role=${ws.role} pcId=${ws.pcId} pcs=${moved} (${trigger})`,
        );
        return true;
    }

    /**
     * op0 で始めたボイス接続の WebSocket を分類した。通し番号が現在のセッションより新しければ新しいセッションに入り、
     * 前のセッションで pcId 1 の ssrc を送った人の値を、null の ssrc を送って消す。現在のセッションより古ければ
     * (分類が遅れた古い WebSocket)、その WebSocket は古いセッションのままにし、何も送らない。
     */
    #enterCallSession(ws: WsRecord): void {
        if (ws.callSession <= this.#currentCallSession) {
            this.#log(`call session superseded: ws=${ws.serial} current_ws=${this.#currentCallSession}`);
            return;
        }
        const previous = this.#currentCallSession;
        this.#currentCallSession = ws.callSession;
        const cleared = [...this.#callFactUserIds];
        this.#callFactUserIds.clear();
        for (const userId of cleared) {
            this.#emit({ type: "ssrc", userId, videoSsrc: null, audioSsrc: null, pcId: VOICE_PC_ID });
        }
        if (previous !== 0) {
            this.#log(`call session changed: ws=${ws.serial} previous_ws=${previous} cleared=${cleared.length}`);
        }
    }

    #ownerOfWs(ws: WsRecord): string | undefined {
        if (ws.role !== "golive" || ws.forgotten || ws.closed) return undefined;
        if (ws.op12UserIds.size > 0) return ws.op12UserIds.size === 1 ? firstOf(ws.op12UserIds) : undefined;
        return ws.op5UserIds.size === 1 ? firstOf(ws.op5UserIds) : undefined;
    }

    /** op5 を受け取った。画面共有している人の手がかりが増えたら true を返す。 */
    #onSpeaking(ws: WsRecord, d: Record<string, unknown>): boolean {
        const ssrc = d.ssrc;
        if (typeof ssrc !== "number") return false;
        const userId = nonEmptyString(d.user_id);
        if (!ws.seenSpeakingSsrcs.has(ssrc)) {
            ws.seenSpeakingSsrcs.add(ssrc);
            this.#log(
                `op5: ws=${ws.serial} pcId=${ws.pcId} role=${ws.role} user_id=${userId ?? "none"}` +
                ` ssrc=${ssrc} speaking=${String(d.speaking)}`,
            );
        }
        let ownerHint = false;
        if (userId !== null && ws.role !== "call" && !ws.op5UserIds.has(userId)) {
            ws.op5UserIds.add(userId);
            ownerHint = true;
            if (ws.op5UserIds.size === 2) this.#log(`ws owner hint ambiguous: ws=${ws.serial} source=op5 user_ids=${[...ws.op5UserIds].join(",")}`);
        }
        const fact: SsrcFact = { op: OP_SPEAKING, userId, ssrc };
        if (ws.role === "unclassified") {
            this.#buffer(ws, `5:${userId ?? "none"}`, fact);
        } else {
            this.#emitFact(ws, fact);
        }
        return ownerHint;
    }

    /** op12 を受け取った。画面共有している人の手がかりが増えたら true を返す。 */
    #onVideo(ws: WsRecord, d: Record<string, unknown>): boolean {
        const userId = nonEmptyString(d.user_id);
        const audioSsrc = numberOrNull(d.audio_ssrc);
        const videoSsrc = numberOrNull(d.video_ssrc);
        const streams = Array.isArray(d.streams)
            ? (d.streams as unknown[])
                .map((raw) => {
                    const st = (raw !== null && typeof raw === "object" ? raw : {}) as Record<string, unknown>;
                    return `${String(st.type)}/${String(st.rid)}/${String(st.ssrc)}/${String(st.rtx_ssrc)}/${String(st.active)}/${String(st.quality)}`;
                })
                .join(",")
            : "none";
        this.#log(
            `op12: ws=${ws.serial} pcId=${ws.pcId} role=${ws.role} user_id=${userId ?? "none"}` +
            ` audio=${String(d.audio_ssrc)} video=${String(d.video_ssrc)} rtx=${String(d.rtx_ssrc)} streams=${streams}`,
        );
        let ownerHint = false;
        if (userId !== null && ws.role !== "call" && !ws.op12UserIds.has(userId)) {
            ws.op12UserIds.add(userId);
            ownerHint = true;
            if (ws.op12UserIds.size === 2) this.#log(`ws owner hint ambiguous: ws=${ws.serial} source=op12 user_ids=${[...ws.op12UserIds].join(",")}`);
        }
        const fact: SsrcFact = { op: OP_VIDEO, userId, audioSsrc, videoSsrc };
        if (ws.role === "unclassified") {
            this.#buffer(ws, "12", fact);
        } else {
            this.#emitFact(ws, fact);
        }
        return ownerHint;
    }

    #buffer(ws: WsRecord, key: string, fact: SsrcFact): void {
        ws.buffered.delete(key);
        ws.buffered.set(key, fact);
        while (ws.buffered.size > MAX_BUFFERED_FACTS) {
            const oldest = ws.buffered.keys().next();
            if (oldest.done === true) break;
            ws.buffered.delete(oldest.value);
        }
    }

    /**
     * 分類が決まった WebSocket の ssrc の事実を CoreEvent にして送る。ボイス接続の op5 は、その user_id のマイクの
     * ssrc (pcId 1。現在のセッションの WebSocket のものだけ)。Go Live の op5 と op12 は、画面共有している人の画面共有の
     * ssrc (その接続の pcId)。op5 は Go Live の voice gateway にも届き、そこで示される ssrc は画面共有の音声なので、
     * マイクの ssrc と分ける。ボイス接続の op12 (カメラ) は送らない。
     */
    #emitFact(ws: WsRecord, fact: SsrcFact): void {
        if (ws.role === "call") {
            if (fact.op !== OP_SPEAKING || fact.userId === null) return;
            if (ws.callSession !== this.#currentCallSession) {
                if (!ws.loggedFactDropped) {
                    ws.loggedFactDropped = true;
                    this.#log(`ssrc not emitted: ws=${ws.serial} pcId=${VOICE_PC_ID} reason=superseded-call`);
                }
                return;
            }
            this.#callFactUserIds.add(fact.userId);
            this.#emit({ type: "ssrc", userId: fact.userId, videoSsrc: null, audioSsrc: fact.ssrc, pcId: VOICE_PC_ID });
            return;
        }
        if (ws.role !== "golive") return;
        const owner = this.#ownerOfWs(ws);
        if (owner === undefined) {
            if (!ws.loggedFactDropped) {
                ws.loggedFactDropped = true;
                this.#log(`ssrc not emitted: ws=${ws.serial} pcId=${ws.pcId} reason=${ws.forgotten ? "forgotten" : "owner-unknown"}`);
            }
            return;
        }
        if (fact.op === OP_SPEAKING) {
            this.#emit({ type: "ssrc", userId: owner, videoSsrc: null, audioSsrc: fact.ssrc, pcId: ws.pcId });
        } else {
            this.#emit({ type: "ssrc", userId: owner, videoSsrc: fact.videoSsrc, audioSsrc: fact.audioSsrc, pcId: ws.pcId });
        }
    }

    /**
     * まだ対応付けていない RTCPeerConnection の情報を、MAX_UNPAIRED_PCS 個までにする (古いものから捨てる。保持している
     * トラックのあるものは残す)。捨てる前に ufrag だけで対応付けを試し、決まったものは残す (トラックがまだ来ていない
     * だけのボイス接続の RTCPeerConnection を捨てないため)。op2 の近さは、トラックが届いたときの最後の手段なので、
     * ここでは使わない。
     */
    #pruneUnpairedPcs(): void {
        const unpaired = [...this.#pcs.values()].filter(
            pc => pc.wsSerial === null && !this.#held.some(h => h.pcSerial === pc.serial),
        );
        for (let i = 0; i < unpaired.length - MAX_UNPAIRED_PCS; i++) {
            const pc = unpaired[i];
            if (pc === undefined || this.#tryPair(pc, false)) continue;
            this.#pcs.delete(pc.serial);
            this.#log(`pc record dropped: pc=${pc.serial} reason=too-many-unpaired`);
        }
    }

    /**
     * 閉じた WebSocket の情報を、MAX_CLOSED_WS 個までにする (古いものから捨てる)。捨てる前に、まだ対応付けていない
     * RTCPeerConnection を ufrag だけで対応付けておく。その WebSocket の ufrag が消えるためで、ボイス接続の WebSocket
     * だけが閉じて RTCPeerConnection が続いたときに、後から参加した人の音声を pcId 1 で送れるようにする。Go Live の
     * RTCPeerConnection の情報は一緒に捨てる (画面共有している人をもう答えられず、続きの元にもならない)。ボイス接続の
     * RTCPeerConnection の情報は、pcId 1 のまま残す。
     */
    #dropOldClosedWs(): void {
        while (this.#closedOrder.length > MAX_CLOSED_WS) {
            const serial = this.#closedOrder.shift();
            if (serial === undefined) break;
            const ws = this.#ws.get(serial);
            if (ws === undefined) continue;
            for (const pc of this.#pcs.values()) {
                if (pc.wsSerial === null && pc.readSdp !== null) this.#tryPair(pc, false);
            }
            this.#ws.delete(serial);
            if (ws.role !== "call") {
                for (const [pcSerial, pc] of this.#pcs) {
                    if (pc.wsSerial === serial && !this.#held.some(h => h.pcSerial === pcSerial)) this.#pcs.delete(pcSerial);
                }
            }
            this.#log(`ws record dropped: ws=${serial} role=${ws.role} pcId=${ws.pcId} reason=too-many-closed`);
        }
    }

    #notPaired(pc: PcRecord, reason: string): false {
        if (!pc.loggedNotPaired.has(reason)) {
            pc.loggedNotPaired.add(reason);
            this.#log(`pc not paired: pc=${pc.serial} reason=${reason}`);
        }
        return false;
    }

    /**
     * RTCPeerConnection と WebSocket を対応付ける。ufrag の文字列はここで比べるだけで、ログには書かない。ufrag では、
     * 閉じた WebSocket の情報も探す (ufrag は接続ごとに違うので、閉じていても取り違えない)。allowAdjacency を true に
     * するのは、トラックが届いたときの呼び出しだけである (RTCPeerConnection の SDP がまだないうちに op2 の近さで
     * 対応付けると、正しい ufrag が後からそろっても対応付け直さないため)。決まったら true を返す。
     */
    #tryPair(pc: PcRecord, allowAdjacency: boolean): boolean {
        let desc: PcDescriptions = { remote: null, local: null };
        try {
            if (pc.readSdp !== null) desc = pc.readSdp();
        } catch {
            desc = { remote: null, local: null };
        }
        const remoteUfrag = extractIceUfrag(desc.remote);
        const localUfrag = extractIceUfrag(desc.local);
        const known = [...this.#ws.values()];
        const byRemote = remoteUfrag === null ? [] : known.filter(ws => ws.remoteUfrag === remoteUfrag);
        const byLocal = localUfrag === null ? [] : known.filter(ws => ws.localUfrag === localUfrag);
        // 一方の ufrag で選んだ WebSocket が、もう一方の ufrag で食い違っていないか (別の RTCPeerConnection の op1 か
        // op4 を持っている)。
        const contradicts = (ws: WsRecord): boolean =>
            (remoteUfrag !== null && ws.remoteUfrag !== null && ws.remoteUfrag !== remoteUfrag)
            || (localUfrag !== null && ws.localUfrag !== null && ws.localUfrag !== localUfrag);

        let chosen: WsRecord | undefined;
        let route: string;
        if (byRemote.length > 0 && byLocal.length > 0) {
            const both = byRemote.filter(ws => byLocal.includes(ws));
            if (both.length === 0) return this.#notPaired(pc, "conflict");
            if (both.length > 1) return this.#notPaired(pc, "ambiguous");
            chosen = both[0];
            route = byRemote.length === 1 ? "remote-ufrag" : "local-ufrag";
        } else if (byRemote.length > 0) {
            if (byRemote.length > 1) return this.#notPaired(pc, "ambiguous");
            chosen = byRemote[0];
            route = "remote-ufrag";
        } else if (byLocal.length > 0) {
            if (byLocal.length > 1) return this.#notPaired(pc, "ambiguous");
            chosen = byLocal[0];
            route = "local-ufrag";
        } else {
            if (!allowAdjacency) return false;
            // 最後の手段: op2 を受けてから 50 ms 以内にできた RTCPeerConnection。まだ対応付けていない、開いた
            // WebSocket がちょうど 1 つのときだけ。
            const near = known.filter(ws =>
                !ws.closed
                && !ws.paired
                && ws.op2At !== null
                && ws.op2At <= pc.createdAt
                && pc.createdAt - ws.op2At <= OP2_ADJACENCY_MS
                && !contradicts(ws));
            if (near.length !== 1) return false;
            chosen = near[0];
            route = "op2-adjacency";
        }
        if (chosen === undefined) return false;
        if (contradicts(chosen)) return this.#notPaired(pc, "conflict");

        pc.wsSerial = chosen.serial;
        pc.readSdp = null;
        if (chosen.role !== "unclassified") {
            // 分類をコピーしておく (WebSocket の情報が先に消えても、ボイス接続のトラックを pcId 1 で送れる)。
            pc.role = chosen.role;
            pc.pcId = chosen.pcId;
            pc.callSession = chosen.callSession;
        }
        chosen.paired = true;
        // rcvr_rtt は、相手の SDP が受けるだけの側にも往復の時間を測らせるか (videoNonSenderRtt)。Discord の画面共有の
        // 映像のタイムスタンプが描く予定の時刻になる前提を、実際の通話のログで確かめるために書く。
        const rcvrRtt = videoNonSenderRtt(desc.remote);
        this.#log(
            `pc paired: pc=${pc.serial} ws=${chosen.serial} route=${route} role=${chosen.role} pcId=${chosen.pcId} ` +
                `rcvr_rtt=${rcvrRtt === null ? "unknown" : rcvrRtt ? "yes" : "no"}`,
        );
        return true;
    }

    /** トラック 1 つの行き先を決めて送る。決まらなければ、保持する理由を返す。 */
    #route(pcSerial: number, track: IncomingTrack): RouteResult {
        if (track.isEnded()) {
            this.#log(`track dropped: pc=${pcSerial} trackId=${track.trackId} reason=ended-before-routing`);
            return DONE;
        }
        const pc = this.#pcs.get(pcSerial);
        if (pc === undefined) {
            this.#log(`track dropped: pc=${pcSerial} trackId=${track.trackId} reason=unknown-pc`);
            return DONE;
        }
        if (pc.wsSerial === null && !this.#tryPair(pc, true)) return hold("unpaired");

        const ws = pc.wsSerial === null ? undefined : this.#ws.get(pc.wsSerial);
        if (ws !== undefined && ws.role === "unclassified") this.#tryClassify(ws, "track");
        if (ws !== undefined && ws.role !== "unclassified") {
            pc.role = ws.role;
            pc.pcId = ws.pcId;
            pc.callSession = ws.callSession;
        }
        if (pc.role === "call") {
            // 前のセッションの RTCPeerConnection のトラックは送らない (その SSRC は、新しいセッションの別の人の番号と
            // 重なることがある)。
            if (pc.callSession !== this.#currentCallSession) {
                this.#log(`track dropped: pc=${pcSerial} pcId=${VOICE_PC_ID} trackId=${track.trackId} reason=superseded-call`);
                return DONE;
            }
            // ボイス接続には何人ものマイクが乗るので、msid は照合しない (userId は ssrc で特定する)。
            this.#deliver(pc, track, VOICE_PC_ID, null);
            return DONE;
        }
        if (ws === undefined) {
            this.#log(`track dropped: pc=${pcSerial} trackId=${track.trackId} reason=connection-closed`);
            return DONE;
        }
        if (ws.role === "unclassified") return hold("unclassified");
        if (ws.forgotten) {
            this.#log(`track dropped: pc=${pcSerial} pcId=${ws.pcId} trackId=${track.trackId} reason=forgotten`);
            return DONE;
        }
        // Go Live の WebSocket が閉じた。op7 で再接続されれば続きに付け替わるので、それまで保持する。
        if (ws.closed) return hold("ws-closed");
        const owner = this.#ownerOfWs(ws);
        if (owner === undefined) return hold("owner-unknown");
        const msid = parseMsid(track.msid);
        if (msid !== null && msid.userId !== owner) {
            this.#log(
                `track dropped: pc=${pcSerial} pcId=${ws.pcId} trackId=${track.trackId} reason=msid-owner-mismatch` +
                ` msid_user=${msid.userId} owner=${owner}`,
            );
            return DONE;
        }
        this.#deliver(pc, track, ws.pcId, owner);
        return DONE;
    }

    #deliver(pc: PcRecord, track: IncomingTrack, pcId: number, owner: string | null): void {
        this.#log(
            `track routed: pc=${pc.serial} pcId=${pcId} kind=${track.kind} trackId=${track.trackId}` +
            (owner === null ? "" : ` owner=${owner}`),
        );
        try {
            track.deliver(pcId);
        } catch {
            // 1 つのトラックを渡すのに失敗しても、残りは止めない。
        }
    }

    /** 保持しているトラックを見直す。送れたものと、5 秒を過ぎたもの (ログに書いて捨てる) を外す。 */
    #recheckHeld(): void {
        if (this.#held.length === 0) return;
        const items = this.#held;
        this.#held = [];
        const keep: HeldTrack[] = [];
        for (const item of items) {
            const result = this.#route(item.pcSerial, item.track);
            if (result.kind === "done") continue;
            const heldMs = this.#now() - item.since;
            if (heldMs >= TRACK_HOLD_MAX_MS) {
                this.#log(
                    `track dropped: pc=${item.pcSerial} trackId=${item.track.trackId} reason=${result.reason} held_ms=${heldMs}`,
                );
                continue;
            }
            keep.push(item);
        }
        // 見直しの途中で新しく保持したもの (deliver の中から trackArrived() が呼ばれたとき) を後ろに残す。
        this.#held = keep.concat(this.#held);
    }

    #ensureTimer(): void {
        if (this.#cancelTimer !== null || this.#held.length === 0) return;
        this.#cancelTimer = this.#schedule(() => {
            this.#cancelTimer = null;
            try {
                this.#recheckHeld();
            } catch {
                // 見直しに失敗しても、Discord の通話は止めない。
            }
            this.#ensureTimer();
        }, TRACK_RECHECK_MS);
    }
}
