// SPDX-License-Identifier: Apache-2.0

// 制御用の JSON のメッセージ (ControlMessage) の型と、エンコードとデコード。メッセージの種類とキーは
// docs/wire_format.md の「制御用の JSON」にある。メッセージの向き (送信側から受信側か、その逆か) は、
// コードでは確認しない。decodeControlMessage() は例外を投げず、形が不正なメッセージには undefined を返す。

/** configure で送る、1 ストリームの設定。width、height、fps、audio は省略できる。 */
export interface ConfigureStream {
    stream_id: number;
    user_id: string;
    kind: "screen" | "voice";
    width?: number;
    height?: number;
    fps?: number;
    /** そのストリームに音声があるか。 */
    audio?: boolean;
}

/**
 * profiles で送る 1 人分の情報。user_id のほかは省略できる。core/facts.ts の ProfileFact は、この型から
 * username を除いた形なので、そのまま渡せる。アバターの画像は profiles に載せず、avatar で参加者ごとに送る。
 */
export interface ProfileEntry {
    user_id: string;
    display_name?: string;
    /**
     * Discord の user.avatar の値 (アバターを設定していない人は空の文字列)。読めていない人はキーを入れない。
     * avatar のメッセージの avatar_key と同じ値になる。
     */
    avatar_key?: string;
    /** Bot のアカウントなら true。 */
    bot?: boolean;
    /** Discord のユーザー名。読めなかったときはキーを入れない。 */
    username?: string;
}

/**
 * avatar で送る 1 人分のアバター。avatar_png は 64 x 64 の PNG を base64 にした文字列 (32768 文字まで)。
 * avatar_key は、その画像を取った時の Discord の user.avatar の値 (設定していない人は空の文字列)。
 */
export interface AvatarMessage {
    user_id: string;
    avatar_key: string;
    avatar_png: string;
}

/**
 * `limits` で受け取る、画質の上限の 2 つのキーと、エンコードの設定の 2 つのキー。どれも省略でき、省略したキーは
 * 制限しない (エンコードの設定は、省略したら既定の設定)。`hello_ok` の `limits` も同じ形である。
 */
export interface LimitsFields {
    /** 長い辺の上限 (ピクセル。正の整数)。省略したときは上限なし。 */
    max_long_edge?: number;
    /** フレームレートを割る数。1 はそのまま、2 は半分。省略したときは 1。 */
    fps_divisor?: 1 | 2;
    /** エンコーダの好み。省略したときはハードウェア。ハードウェアが使えなければ、はしごに従ってソフトウェアへ落ちる。 */
    encoder_preference?: "hardware" | "software";
    /** WebCodecs の latencyMode。省略したときは realtime (速さ優先)。 */
    latency_mode?: "realtime" | "quality";
}

/**
 * `receive_scope` で受け取る、視聴する人の範囲。"everyone" は画面共有している全員、"slots" は userIds の人だけ。
 * "everyone" のとき userIds は持たない。
 */
export type ReceiveScopeFields = { scope: "everyone" } | { scope: "slots"; user_ids: string[] };

/**
 * `stream_check_result` で送る、受信側から頼まれた確認の結果 (docs/wire_format.md の「メッセージの種類」の stream_check の扱い)。
 * - restarted: そのトラックに RTP が届いているのに送れるフレームがなかったので、読み出しとエンコードを始め直した。
 * - superseded: 同じ人の、より新しい接続の映像が動いていたので、このストリームを止めた。
 * - not_receiving: そのトラックに RTP が届いていない (Discord が送っていない)。始め直しても何も得られないので、始め直さない。
 * - sending: 確認の間もフレームを送っていた (送信側では止まっていない)。
 * - ended: トラックが終わっていた (このストリームは止まる)。
 * - unknown_stream: そのストリームがない。
 */
export type StreamCheckOutcome = "restarted" | "superseded" | "not_receiving" | "sending" | "ended" | "unknown_stream";

/**
 * 制御用の JSON のメッセージ。hello_ng の reason は "bad_secret"、"bad_wire"、"malformed" のどれかだが、値が
 * 増えても読めるように、文字列のまま受け取る。facts の中身は受信側が読む値なので、送信側では形を確認しない。
 */
export type ControlMessage =
    | {
          type: "hello";
          version: string;
          secret: string;
          wire: number;
          vesktop?: string;
          clock_us?: number;
          /** 送信側の時計の種類。今の値は "qpc" だけで、知らない値は受け取った側がないものとして扱う。 */
          clock_base?: string;
      }
    | { type: "hello_ok"; wire: number; version: string; media?: boolean; limits?: LimitsFields }
    | { type: "hello_ng"; reason: string }
    | { type: "ping"; seq: number }
    | { type: "pong"; seq: number; clock_us?: number }
    | { type: "configure"; streams: ConfigureStream[] }
    | { type: "keyframe_request"; stream_id: number }
    | { type: "stream_check"; stream_id: number }
    | { type: "stream_check_result"; stream_id: number; outcome: StreamCheckOutcome; detail?: string }
    | { type: "recording"; on: boolean }
    | { type: "media"; on: boolean }
    | ({ type: "limits" } & LimitsFields)
    | { type: "main_account"; user_id: string }
    | ({ type: "receive_scope" } & ReceiveScopeFields)
    | { type: "move_to_main_call" }
    | { type: "leave_call" }
    /** lines は出来事の行、alerts は異常に関わる出来事の行、stats は統計の行 (core/logbook.ts)。alerts と stats は任意。 */
    | { type: "log"; lines: string[]; alerts?: string[]; stats?: string[] }
    | { type: "facts"; facts: unknown }
    | { type: "profiles"; profiles: ProfileEntry[] }
    | ({ type: "avatar" } & AvatarMessage);

/** StreamCheckOutcome の値の一覧。 */
export const STREAM_CHECK_OUTCOMES: readonly StreamCheckOutcome[] = [
    "restarted",
    "superseded",
    "not_receiving",
    "sending",
    "ended",
    "unknown_stream",
];

/** ControlMessage の type の値の一覧。 */
export type ControlMessageType = ControlMessage["type"];

function isPlainObject(value: unknown): value is Record<string, unknown> {
    return typeof value === "object" && value !== null && !Array.isArray(value);
}

function isFiniteNumber(value: unknown): value is number {
    return typeof value === "number" && Number.isFinite(value);
}

function isNonNegativeInteger(value: unknown): value is number {
    return typeof value === "number" && Number.isInteger(value) && value >= 0;
}

/** clock_us の値 (送信側の時計の us)。JSON の数で正確に表せる 0 以上の整数だけを受け付ける。 */
function isClockUs(value: unknown): value is number {
    return isNonNegativeInteger(value) && Number.isSafeInteger(value);
}

function isPositiveFiniteNumber(value: unknown): value is number {
    return isFiniteNumber(value) && value > 0;
}

function isStringArray(value: unknown): value is string[] {
    return Array.isArray(value) && value.every((item) => typeof item === "string");
}

function isConfigureStream(value: unknown): value is ConfigureStream {
    if (!isPlainObject(value)) return false;
    if (
        !isNonNegativeInteger(value.stream_id) ||
        typeof value.user_id !== "string" ||
        (value.kind !== "screen" && value.kind !== "voice")
    ) {
        return false;
    }
    // width、height、fps、audio は省略できる。付いているときは、型と、意味のある範囲 (正の有限の数) かを
    // 確認する。意味のない値を通す理由はないので、不正な形として扱う。
    if (value.width !== undefined && !isPositiveFiniteNumber(value.width)) return false;
    if (value.height !== undefined && !isPositiveFiniteNumber(value.height)) return false;
    if (value.fps !== undefined && !isPositiveFiniteNumber(value.fps)) return false;
    if (value.audio !== undefined && typeof value.audio !== "boolean") return false;
    return true;
}

function isPositiveInteger(value: unknown): value is number {
    return typeof value === "number" && Number.isInteger(value) && value > 0;
}

/**
 * オブジェクトから `limits` の 4 つのキーを読む (`limits` のメッセージと `hello_ok` の `limits` で使う)。
 * 型が違うキーか範囲の外のキーが 1 つでもあれば undefined を返し、呼び出し元はメッセージごと捨てる。不正な値を
 * encode/session.ts まで渡すと、そこで RangeError になるためである。基準は受信側の
 * src-obs/ws/control_messages.h の parse_limits_fields() と limits_in_range() と同じ。知らないキーは無視し、
 * 戻り値には 4 つのキーのうち付いていたものだけを入れる。
 */
export function readLimitsFields(value: unknown): LimitsFields | undefined {
    if (!isPlainObject(value)) return undefined;
    const fields: LimitsFields = {};
    if (value.max_long_edge !== undefined) {
        if (!isPositiveInteger(value.max_long_edge) || value.max_long_edge > 0xffffffff) return undefined;
        fields.max_long_edge = value.max_long_edge;
    }
    if (value.fps_divisor !== undefined) {
        if (value.fps_divisor !== 1 && value.fps_divisor !== 2) return undefined;
        fields.fps_divisor = value.fps_divisor;
    }
    if (value.encoder_preference !== undefined) {
        if (value.encoder_preference !== "hardware" && value.encoder_preference !== "software") return undefined;
        fields.encoder_preference = value.encoder_preference;
    }
    if (value.latency_mode !== undefined) {
        if (value.latency_mode !== "realtime" && value.latency_mode !== "quality") return undefined;
        fields.latency_mode = value.latency_mode;
    }
    return fields;
}

function isProfileEntry(value: unknown): value is ProfileEntry {
    if (!isPlainObject(value)) return false;
    if (typeof value.user_id !== "string") return false;
    // display_name、avatar_key、bot、username は省略できる。付いているときは型だけを確認する。1 つでも壊れた
    // 要素があれば、メッセージ全体を不正とする。
    if (value.display_name !== undefined && typeof value.display_name !== "string") return false;
    if (value.avatar_key !== undefined && typeof value.avatar_key !== "string") return false;
    if (value.bot !== undefined && typeof value.bot !== "boolean") return false;
    if (value.username !== undefined && typeof value.username !== "string") return false;
    return true;
}

/**
 * JSON として読んだ値を確認し、ControlMessage として正しければそれを返す。型が違う、省略できないキーがない、
 * type を知らない、のどれかのときは undefined を返す (例外は投げない)。知らないキーは無視する。キーが増えても、
 * 古い送信側が読めるようにするためである。
 */
export function validateControlMessage(value: unknown): ControlMessage | undefined {
    if (!isPlainObject(value)) return undefined;

    switch (value.type) {
        case "hello":
            if (
                typeof value.version === "string" &&
                typeof value.secret === "string" &&
                isNonNegativeInteger(value.wire)
            ) {
                const hello: Extract<ControlMessage, { type: "hello" }> = {
                    type: "hello",
                    version: value.version,
                    secret: value.secret,
                    wire: value.wire,
                };
                // vesktop は省略できる。文字列でなければ、hello は捨てずに、このキーだけをないものとして扱う。
                // 表示にだけ使う値の誤りで、接続を止めないためである。
                if (typeof value.vesktop === "string") hello.vesktop = value.vesktop;
                // clock_us も省略できる。0 以上の安全な整数でなければ、このキーだけをないものとして扱う (vesktop と同じ)。
                if (isClockUs(value.clock_us)) hello.clock_us = value.clock_us;
                // clock_base も省略できる。文字列でなければ、このキーだけをないものとして扱う。値の意味は受信側が決める
                // (知らない値は、ないものとして扱う)。
                if (typeof value.clock_base === "string") hello.clock_base = value.clock_base;
                return hello;
            }
            return undefined;

        case "hello_ok": {
            if (!isNonNegativeInteger(value.wire) || typeof value.version !== "string") return undefined;
            const helloOk: Extract<ControlMessage, { type: "hello_ok" }> = {
                type: "hello_ok",
                wire: value.wire,
                version: value.version,
            };
            // media と limits は受信側が入れる初期値で、省略できる。形が壊れていても hello_ok は捨てずに、その
            // キーだけをないものとして扱う。hello_ok を捨てると、transport/client.ts が handshaking のまま待ち、
            // 接続できなくなる。同じ値は、接続した直後の media と limits のメッセージでも届く。キーがないのは
            // 古い受信側で、送信側は media が true、上限なしとして動く。
            if (typeof value.media === "boolean") helloOk.media = value.media;
            if (value.limits !== undefined) {
                const limits = readLimitsFields(value.limits);
                if (limits !== undefined) helloOk.limits = limits;
            }
            return helloOk;
        }

        case "hello_ng":
            if (typeof value.reason === "string") {
                return { type: "hello_ng", reason: value.reason };
            }
            return undefined;

        case "ping":
            if (isNonNegativeInteger(value.seq)) {
                return { type: "ping", seq: value.seq };
            }
            return undefined;

        case "pong":
            if (isNonNegativeInteger(value.seq)) {
                const pong: Extract<ControlMessage, { type: "pong" }> = { type: "pong", seq: value.seq };
                if (isClockUs(value.clock_us)) pong.clock_us = value.clock_us;
                return pong;
            }
            return undefined;

        case "configure":
            if (Array.isArray(value.streams) && value.streams.every(isConfigureStream)) {
                return { type: "configure", streams: value.streams };
            }
            return undefined;

        case "keyframe_request":
            if (isNonNegativeInteger(value.stream_id)) {
                return { type: "keyframe_request", stream_id: value.stream_id };
            }
            return undefined;

        case "stream_check":
            if (isNonNegativeInteger(value.stream_id)) {
                return { type: "stream_check", stream_id: value.stream_id };
            }
            return undefined;

        case "stream_check_result":
            // 送信側は送るだけで受け取らないが、テスト用の受け手と単体テストが読む。outcome は知っている名前だけを通す。
            if (
                isNonNegativeInteger(value.stream_id) &&
                typeof value.outcome === "string" &&
                (STREAM_CHECK_OUTCOMES as readonly string[]).includes(value.outcome) &&
                (value.detail === undefined || typeof value.detail === "string")
            ) {
                const result: Extract<ControlMessage, { type: "stream_check_result" }> = {
                    type: "stream_check_result",
                    stream_id: value.stream_id,
                    outcome: value.outcome as StreamCheckOutcome,
                };
                if (typeof value.detail === "string") result.detail = value.detail;
                return result;
            }
            return undefined;

        case "recording":
            // 録画の ON と OFF を cordmux.dll から受け取る。on は省略できない。真偽値でなければ (0 と 1、"true" も)
            // このメッセージを捨てる。受信側の設定の読み取り (src-obs/config/settings.h の read_bool()) と同じく、
            // 形が違う値は通さない。
            if (typeof value.on === "boolean") {
                return { type: "recording", on: value.on };
            }
            return undefined;

        case "media":
            // false の間、送信側はトラックの読み出しも、エンコードも、アバターの取得もしない。on は省略できず、
            // 真偽値でなければこのメッセージを捨てる (recording と同じ)。
            if (typeof value.on === "boolean") {
                return { type: "media", on: value.on };
            }
            return undefined;

        case "limits": {
            // 型が違うキーか範囲の外のキーがあれば、このメッセージを捨てる (readLimitsFields())。
            const limits = readLimitsFields(value);
            if (limits === undefined) return undefined;
            return { type: "limits", ...limits };
        }

        case "main_account":
            // user_id は空の文字列でもよい (メインアカウントを使わない構成)。文字列でなければ捨てる。
            if (typeof value.user_id === "string") {
                return { type: "main_account", user_id: value.user_id };
            }
            return undefined;

        case "receive_scope":
            // "slots" の user_ids は省けない (省くと、誰も視聴しない範囲か全員かが決まらない)。配列に文字列でない
            // 要素が 1 つでもあれば、メッセージごと捨てる。"everyone" の user_ids は無視する。
            if (value.scope === "everyone") return { type: "receive_scope", scope: "everyone" };
            if (value.scope === "slots" && isStringArray(value.user_ids)) {
                return { type: "receive_scope", scope: "slots", user_ids: value.user_ids };
            }
            return undefined;

        // 移動先も退出の対象も運ばない。移動先は、送信側が押された時点のメインアカウントの通話を読み直して決める。
        case "move_to_main_call":
            return { type: "move_to_main_call" };

        case "leave_call":
            return { type: "leave_call" };

        case "log": {
            // alerts と stats は任意。あるのに文字列の配列でなければ、メッセージごと捨てる (受信側と同じ)。
            if (!isStringArray(value.lines)) return undefined;
            if (value.alerts !== undefined && !isStringArray(value.alerts)) return undefined;
            if (value.stats !== undefined && !isStringArray(value.stats)) return undefined;
            const log: Extract<ControlMessage, { type: "log" }> = { type: "log", lines: value.lines };
            if (value.alerts !== undefined) log.alerts = value.alerts;
            if (value.stats !== undefined) log.stats = value.stats;
            return log;
        }

        case "facts":
            if ("facts" in value) {
                return { type: "facts", facts: value.facts };
            }
            return undefined;

        case "profiles":
            if (Array.isArray(value.profiles) && value.profiles.every(isProfileEntry)) {
                return { type: "profiles", profiles: value.profiles };
            }
            return undefined;

        case "avatar":
            // 3 つのキーはどれも省略できない。avatar_png の長さの上限は見ない。送信側は avatar を送るだけで受け取らず、
            // 長すぎる avatar_png を捨てるのは受信側 (src-obs/ws/control_messages.h) である。
            if (
                typeof value.user_id === "string" &&
                typeof value.avatar_key === "string" &&
                typeof value.avatar_png === "string"
            ) {
                return { type: "avatar", user_id: value.user_id, avatar_key: value.avatar_key, avatar_png: value.avatar_png };
            }
            return undefined;

        default:
            return undefined;
    }
}

/** ControlMessage を、テキストのメッセージで送る JSON の文字列にする。 */
export function encodeControlMessage(message: ControlMessage): string {
    return JSON.stringify(message);
}

/**
 * テキストのメッセージで受け取った JSON の文字列を読む。JSON として読めないときと、ControlMessage として不正な
 * 形のときは undefined を返す (例外は投げない)。
 */
export function decodeControlMessage(text: string): ControlMessage | undefined {
    let parsed: unknown;
    try {
        parsed = JSON.parse(text);
    } catch {
        return undefined;
    }
    return validateControlMessage(parsed);
}
