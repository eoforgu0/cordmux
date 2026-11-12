// SPDX-License-Identifier: Apache-2.0

// 制御用の JSON の facts と profiles の中身を、CoreState と、CoreState にない入力 (ストリームごとのエンコードの
// 段階、userId ごとの表示名と bot の印、録画とゲートの現在の値) から組み立てる純粋関数 (buildFacts、buildProfiles)。
// アバターを取得する相手も決める (avatarFetchTargets、avatarFetchesToStart)。index.ts は Vencord を import するので、リポジトリの
// ルートの tsc と vitest でテストできない。そこで、中身を作る処理をここに置いてテストする。形は
// docs/wire_format.md の「facts」と「profiles」にある。

import { sharing, warnings, type CoreState } from "./state";
import { LadderStage, stageWireName } from "../encode/ladder";
import type { LimitsFields } from "../transport/control";

/** facts の stages の 1 件。 */
export interface StageFact {
    stream_id: number;
    stage: string;
}

/** profiles の 1 件。 */
export interface ProfileFact {
    user_id: string;
    display_name?: string;
    /** Discord の user.avatar の値 (設定していない人は空の文字列)。まだ読めていない人は省く。 */
    avatar_key?: string;
    /** Bot のアカウントか。読めていない人は偽。 */
    bot: boolean;
}

/**
 * Map の挿入順ではなく stream_id の昇順に並べる。index.ts は前に送った facts と JSON の文字列で比べ、同じなら
 * 送らない。Map の順は呼び出し元が Map にどの順で追加したかで変わるので、並べ直さないと、中身が同じでも別の
 * JSON になって送り直してしまう。
 */
function buildStages(stagesByStreamId: ReadonlyMap<number, LadderStage>): StageFact[] {
    return [...stagesByStreamId.entries()]
        .sort(([a], [b]) => a - b)
        .map(([stream_id, stage]) => ({ stream_id, stage: stageWireName(stage) }));
}

/**
 * 録画について送信側が持っている値。どちらも CoreState にないので、呼び出し元が渡す。facts には値に関わらず必ず
 * 入れる。stages のように省くと、受信側は、値が 0 (や false) だったのか、このキーを知らない古い送信側なのかを
 * 区別できない。
 */
export interface RecordingFactsInput {
    /**
     * 送信側が持っている録画の現在の値 (受信側から recording の制御用の JSON で届いた値)。受信側はこれを自分の値と
     * 比べ、食い違えば画面に表示する。受信側は recording を送るのに失敗しても失敗を数えるだけで、ハートビートは
     * 続いて接続は正常に見えるので、送信側に届かないまま録画が進むことがあるためである。
     */
    readonly on: boolean;
    /**
     * SPS と PPS をキーフレームの前に付けた回数の合計 (ストリームごとの数を呼び出し元が合計したもの)。受信側が
     * 録画ログに書けるように入れる。
     */
    readonly spsPpsPrepended: number;
}

/**
 * ゲートについて送信側が持っている現在の値。受信側から media と limits の制御用の JSON か、hello_ok で届いた値で、
 * まだ何も届いていなければ、送信側が実際に動いている値 (media は true、limits は {} で上限なし) を入れる。
 * facts には値に関わらず必ず入れる (RecordingFactsInput と同じ理由で、既定のままなのか、このキーを知らない古い
 * 送信側なのかを、受信側が区別できるようにする)。
 */
export interface GateFactsInput {
    /** 受信側から届いた media (まだ届いていなければ true で、開いたまま)。 */
    readonly media: boolean;
    /**
     * 受信側から届いた limits。ワイヤー形式のまま持つので、受信側は自分が送った値とそのまま比べられる。
     * EncodeSessionLimits に変換すると、省いたキーと既定の値の区別がなくなる。これは新しく作るストリームに使う
     * 値で、走っているストリームの実際の上限とは限らない。上げたキーは走っているストリームには適用されず、次の画面
     * 共有から反映される (encode/limits.ts の tightenLimits) ので、上げた直後は、走っているストリームのほうが厳しい。
     * 受信側が確認するのは自分の送った値が届いたかで、ストリームごとの実際の上限は送らない。
     */
    readonly limits: LimitsFields;
}

/**
 * facts の中身を作る。`stagesByStreamId` は、呼び出し元 (index.ts) がエンコードのセッションごとに集めた現在の段階。
 * CoreState は Discord の事実だけを持ち、エンコードの段階を持たないので、encode/session.ts の
 * EncodeSessionSink.onStage() で受け取った値を、呼び出し元が streamId ごとに集めて渡す。`recording` は録画の値、
 * `gate` はゲートの現在の値。Vesktop のアカウントの userId は `state` から読む。
 */
export function buildFacts(
    state: CoreState,
    stagesByStreamId: ReadonlyMap<number, LadderStage>,
    recording: RecordingFactsInput,
    gate: GateFactsInput,
): Record<string, unknown> {
    const sharingIds = sharing(state).map((s) => s.userId);
    // ワイヤー形式の 4 つのキーだけを、値があるときだけコピーする (呼び出し元の値にほかのキーがあっても入れない)。
    const limits: LimitsFields = {};
    if (gate.limits.max_long_edge !== undefined) limits.max_long_edge = gate.limits.max_long_edge;
    if (gate.limits.fps_divisor !== undefined) limits.fps_divisor = gate.limits.fps_divisor;
    if (gate.limits.encoder_preference !== undefined) limits.encoder_preference = gate.limits.encoder_preference;
    if (gate.limits.latency_mode !== undefined) limits.latency_mode = gate.limits.latency_mode;
    const facts: Record<string, unknown> = {
        warnings: warnings(state),
        sharing: { count: sharingIds.length, user_ids: sharingIds },
        recording: recording.on, // 値に関わらず必ず入れる (RecordingFactsInput)。
        sps_pps_prepended: recording.spsPpsPrepended,
        media: gate.media, // 値に関わらず必ず入れる (GateFactsInput)。
        limits,
    };
    if (state.roster !== "unknown") {
        const rosterIds = state.roster.map((u) => u.userId);
        facts.roster = { count: rosterIds.length, user_ids: rosterIds };
    }
    // Vesktop のアカウントの userId。roster には入るが、その声は受信側に届かず (WebRTC は自分が送った音声を返さない)、
    // 画面も届かない (送信側は自分の画面共有を視聴しない)。受信側がどの人かを知らないと、使う声の選択肢にこの人を
    // 並べ、選ぶとその枠の声が無音になる。分からない間は、roster と同じくキーごと省く。recording のように必ず入れない
    // のは、受信側が自分の値と比べる値ではなく、キーがないときの受信側の動作 (誰も外さない) は、古い送信側と接続した
    // ときと同じでよいためである。
    // 空の文字列も入れない (adapter/vencord.ts は空を null にするが、イベントを作るのはテスト用の adapter もある)。
    if (state.selfUserId !== null && state.selfUserId !== "") facts.self_user_id = state.selfUserId;
    // 通話の名前。読めない間はキーごと省く (空の文字列は入れない)。
    if (state.callName !== null && state.callName !== "") facts.call_name = state.callName;
    // メインアカウントがいる通話。Vesktop のアカウントから見えない通話に参加しているときと、メインアカウントを知らない間は、
    // キーごと省く。どの通話にもいないことを伝えるキーはない。見えない通話に参加していることと、どの通話にもいないことを
    // 区別できないためである。name と joinable も、読めなかったときはキーごと省く。
    if (state.mainCall !== null) {
        const mainCall: Record<string, unknown> = { user_id: state.mainCall.userId };
        if (state.mainCall.name !== null && state.mainCall.name !== "") mainCall.name = state.mainCall.name;
        if (state.mainCall.joinable !== null) mainCall.joinable = state.mainCall.joinable;
        facts.main_call = mainCall;
    }
    const stages = buildStages(stagesByStreamId);
    // 空なら stages のキーごと省く。ストリームが 1 つもないのは、画面共有している人がいないという 1 つの事実しか
    // 表さず、roster と違って「分からない」と区別する必要がないためである。
    if (stages.length > 0) facts.stages = stages;
    return facts;
}

/**
 * アバターを取得してよい相手。roster の全員である。アバターは profiles ではなく avatar で参加者ごとに送り (送るのは
 * avatar_key が変わったときだけ)、1 通の大きさが人数に比例しないためである。roster が "unknown" の間は、profiles
 * も送らないので、誰も取得しない。呼び出し元は、この集合を、取得する相手とキャッシュを残してよい相手の両方に使う
 * (roster から抜けた人のキャッシュを残さない)。
 */
export function avatarFetchTargets(state: CoreState): ReadonlySet<string> {
    if (state.roster === "unknown") return new Set();
    return new Set(state.roster.map((u) => u.userId));
}

/** avatarFetchesToStart() の入力。どれも呼び出し元 (index.ts) が持つ値。 */
export interface AvatarFetchInputs {
    /** ゲートが開いているか (core/gate.ts の MediaGate.isOpen())。 */
    readonly gateOpen: boolean;
    /** 取得してよい相手 (avatarFetchTargets())。 */
    readonly targets: ReadonlySet<string>;
    /** userId -> profile の依存箇所が返したアバターの URL。 */
    readonly urls: ReadonlyMap<string, string>;
    /** userId -> 取得した結果のキャッシュ (取得できなかった結果も URL つきで持つ)。 */
    readonly cached: ReadonlyMap<string, { readonly url: string }>;
    /** 今取得している最中の userId。 */
    readonly inFlight: ReadonlySet<string>;
}

/**
 * 今取得を始めるアバターを返す。ゲートが閉じていれば 1 つも返さない。機能を OFF にしている間 (media が false で
 * 届く) は、cordmux からの通信を起こさないためである。開いていれば、取得してよい相手のうち、URL があり、同じ URL の
 * キャッシュがなく、取得の最中でない人を、targets の順に返す。
 */
export function avatarFetchesToStart(inputs: AvatarFetchInputs): { userId: string; url: string }[] {
    if (!inputs.gateOpen) return [];
    const fetches: { userId: string; url: string }[] = [];
    for (const userId of inputs.targets) {
        const url = inputs.urls.get(userId);
        if (url === undefined) continue; // profile の依存箇所がアバターの URL を返さなかった人。
        if (inputs.cached.get(userId)?.url === url) continue; // 同じ URL は取得し直さない。
        if (inputs.inFlight.has(userId)) continue;
        fetches.push({ userId, url });
    }
    return fetches;
}

/**
 * profiles の中身を作る。roster が "unknown" の間は、送るものがないので undefined を返し、呼び出し元はメッセージを
 * 送らない (roster と同じく、分からないことと 0 人を区別する)。
 *
 * names は userId -> 表示名で、呼び出し元が adapter.snapshot() の roster[].name から作る。CoreState は表示名を持たず、
 * snapshot() は非同期なので、呼び出し元が読んだ結果を渡し、この関数は純粋関数のままにする。names にない人は
 * display_name を省く。avatarKeys は userId -> Discord の user.avatar の値で、ない人は avatar_key を省く。bots は
 * Bot のアカウントの userId の集合で、ない人は bot を偽にする。
 *
 * selfUserId は Vesktop のアカウントの userId。通話の外でもサブアカウントの名前を受信側に伝えるため、roster にいなくても、
 * 表示名を読めていれば、その人の分を 1 つ足す (roster が "unknown" の間も、その 1 人だけは送る)。足す分は display_name
 * だけで、avatar_key は付けない (アバターは roster の人にだけ送る)。
 */
export function buildProfiles(
    roster: CoreState["roster"],
    names: ReadonlyMap<string, string>,
    avatarKeys: ReadonlyMap<string, string>,
    bots: ReadonlySet<string>,
    selfUserId: string | null = null,
): ProfileFact[] | undefined {
    const selfOnly = (): ProfileFact[] => {
        if (selfUserId === null || selfUserId === "") return [];
        const name = names.get(selfUserId);
        return name === undefined ? [] : [{ user_id: selfUserId, bot: false, display_name: name }];
    };
    if (roster === "unknown") {
        const self = selfOnly();
        return self.length > 0 ? self : undefined;
    }
    const inRoster = new Set(roster.map((u) => u.userId));
    const extra = selfUserId !== null && inRoster.has(selfUserId) ? [] : selfOnly();
    const fromRoster = roster.map((u) => {
        const profile: ProfileFact = { user_id: u.userId, bot: bots.has(u.userId) };
        const name = names.get(u.userId);
        if (name !== undefined) profile.display_name = name;
        const key = avatarKeys.get(u.userId);
        if (key !== undefined) profile.avatar_key = key;
        return profile;
    });
    return [...fromRoster, ...extra];
}

/** AvatarOutbox が送る 1 人分のアバター。 */
export interface AvatarToSend {
    userId: string;
    avatarKey: string;
    pngBase64: string;
}

/** AvatarOutbox.pick() に渡す、送れる状態になっているアバター。 */
export type AvatarCandidate = AvatarToSend;

/** AvatarOutbox.pick() の結果。 */
export interface AvatarPick {
    /** 今送るアバター。pick() が送ったものとして覚える。 */
    send: AvatarToSend[];
    /** 上限のために送れなかったアバターがあるとき、次に送れるようになるまでのミリ秒。なければ null。 */
    retryInMs: number | null;
}

/**
 * アバターを送るかどうかを決める。参加者ごとに、前に送った avatar_key と違うときだけ送り、1 秒に送る人数に上限を
 * 付ける (人数の多い通話に参加した直後に、まとめて送らない)。受信側が接続し直したら reset() で全員分を送り直す。
 * 時刻は呼び出し元が渡す (純粋にテストできるようにするため)。
 */
export class AvatarOutbox {
    private readonly sentKeys = new Map<string, string>();
    private sentAt: number[] = [];

    constructor(
        private readonly maxPerWindow: number,
        private readonly windowMs: number,
    ) {}

    /** 受信側が覚えていないものとして、全員分を送り直せる状態に戻す。 */
    reset(): void {
        this.sentKeys.clear();
        this.sentAt = [];
    }

    /** roster から抜けた人の記録を捨てる。同じ人が戻ってきたら、もう一度送る。 */
    keepOnly(userIds: ReadonlySet<string>): void {
        for (const userId of [...this.sentKeys.keys()]) {
            if (!userIds.has(userId)) this.sentKeys.delete(userId);
        }
    }

    /**
     * candidates のうち、前に送った avatar_key と違う人を、上限の範囲で先頭から選ぶ。選んだ人は送ったものとして
     * 覚える。上限で残った人がいれば、次に枠が空くまでの時間を retryInMs に入れる。
     */
    pick(now: number, candidates: readonly AvatarCandidate[]): AvatarPick {
        this.sentAt = this.sentAt.filter((t) => now - t < this.windowMs);
        const send: AvatarToSend[] = [];
        let blocked = false;
        for (const c of candidates) {
            if (this.sentKeys.get(c.userId) === c.avatarKey) continue;
            if (this.sentAt.length >= this.maxPerWindow) {
                blocked = true;
                break;
            }
            this.sentKeys.set(c.userId, c.avatarKey);
            this.sentAt.push(now);
            send.push(c);
        }
        const oldest = this.sentAt[0];
        const retryInMs = blocked && oldest !== undefined ? Math.max(1, oldest + this.windowMs - now) : null;
        return { send, retryInMs };
    }
}
