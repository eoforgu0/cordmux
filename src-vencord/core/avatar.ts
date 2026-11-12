// SPDX-License-Identifier: Apache-2.0

// profiles の avatar_png を 1 人ぶん作る (fetchAvatarPng)。アバターはレンダラーが fetch で取得し、PNG に再エンコード
// してから送る (OBS からは取得しない)。外部に接続するホストを絞るので、取得するのは cdn.discordapp.com の画像だけに
// する。fetch と PNG への再エンコード (OffscreenCanvas) は呼び出し元 (index.ts) が渡す関数にして、URL、Content-Type、
// 大きさの判断を Node の vitest でテストできるようにする。Vencord も DOM の型も使わない。

/** アバターを取得してよい、ただ 1 つのホスト。 */
export const AVATAR_HOST = "cdn.discordapp.com";

/**
 * URL に付ける size の値。profile の依存箇所が返す URL には size=128 などが付いていることがあるので、
 * normalizeAvatarUrl() は元のクエリ文字列を捨てて size=64 だけを付ける (クエリ文字列のない既定のアバターの URL にも
 * 付ける)。
 */
export const AVATAR_SIZE_VALUE = "64";

/**
 * PNG に再エンコードするときの正方形の一辺 (px)。AVATAR_SIZE_VALUE から作り、URL の size と食い違わないように
 * する。CDN は size を無視することがある (既定のアバターは size に関わらず大きさが一定) ので、取得した画像を
 * この大きさに描き直す。元の大きさのまま再エンコードすると、AVATAR_MAX_BASE64_LENGTH に収まらず、
 * png_too_large で省かれることがある。
 */
export const AVATAR_CANVAS_SIZE = Number(AVATAR_SIZE_VALUE);

/**
 * 取得した画像の本体の上限 (バイト)。超えていたら、再エンコードせずに省く。途中で切ったバイト列は画像として
 * 壊れているので、切り詰めて使うことはしない。
 */
export const AVATAR_MAX_SOURCE_BYTES = 256 * 1024;

/**
 * avatar_png の base64 の文字数の上限。受信側は base64 の文字列の長さで数え、これを超えるものを捨てる
 * (src-obs/ws/control_messages.h の kMaxAvatarPngBase64Bytes) ので、送信側も同じ数え方にする。数え方が違うと、
 * 送信側が送ったつもりのものを受信側が捨てる。
 */
export const AVATAR_MAX_BASE64_LENGTH = 32 * 1024;

/** 受け付ける Content-Type。 */
const ALLOWED_CONTENT_TYPES: readonly string[] = ["image/png", "image/webp", "image/jpeg"];

/**
 * アバターを省いた理由。呼び出し元 (index.ts) はこれをログに 1 行書く。理由がないと、アバターが表示されないときに
 * 手がかりが残らない。ログに書く値なので英語にする。
 */
export type AvatarSkipReason =
    | "bad_url"
    | "fetch_failed"
    | "http_error"
    | "bad_content_type"
    | "source_too_large"
    | "encode_failed"
    | "png_too_large";

export type AvatarResult = { ok: true; pngBase64: string } | { ok: false; reason: AvatarSkipReason };

/** fetchAvatar が返す、取得した結果 (実際の Response の部分集合)。 */
export interface AvatarFetchResult {
    /** HTTP として成功したか (Response.ok)。 */
    readonly ok: boolean;
    /** Content-Type ヘッダーの値のまま (なければ null)。 */
    readonly contentType: string | null;
    /** 本体のバイト列。 */
    readonly body: Uint8Array;
}

/**
 * 呼び出し元 (index.ts) が渡す 2 つの関数。fetch も OffscreenCanvas もレンダラーにしかないので、両方を関数として
 * 受け取り、Node のテストでは置き換える。
 */
export interface AvatarDeps {
    /**
     * normalizeAvatarUrl() が受け付けた URL だけを受け取る。index.ts の実装は fetch(url, { ... })。取得できなければ
     * 例外を投げてよく、fetchAvatarPng() は fetch_failed として扱う。
     */
    fetchAvatar(url: string): Promise<AvatarFetchResult>;
    /**
     * 画像のバイト列を PNG に再エンコードし、base64 で返す。できなければ null。index.ts の実装は createImageBitmap
     * と OffscreenCanvas。取得した画像の元の大きさに関わらず、AVATAR_CANVAS_SIZE x AVATAR_CANVAS_SIZE に描き直す。
     */
    toPngBase64(body: Uint8Array, contentType: string): Promise<string | null>;
}

/**
 * profile の依存箇所が返したアバターの URL を、取得してよい形に修正する。URL として読めない、https でない、ホストが
 * cdn.discordapp.com でない、のどれかなら null を返す。
 */
export function normalizeAvatarUrl(raw: string): string | null {
    let url: URL;
    try {
        url = new URL(raw);
    } catch {
        return null;
    }
    if (url.protocol !== "https:") return null;
    if (url.hostname !== AVATAR_HOST) return null;
    // 既定のポート (443) 以外を指す URL と、ユーザー情報を含む URL は受け付けない (ホストの制限を、URL の書き方で
    // 回避できないようにするため)。
    if (url.port !== "") return null;
    if (url.username !== "" || url.password !== "") return null;
    url.search = `size=${AVATAR_SIZE_VALUE}`; // 元のクエリ文字列は捨てる (AVATAR_SIZE_VALUE)。
    url.hash = "";
    return url.toString();
}

/** `image/png; charset=...` のような値から、型の名前だけを取り出して小文字にする。 */
export function normalizeContentType(raw: string | null): string | null {
    if (raw === null) return null;
    const head = raw.split(";")[0];
    if (head === undefined) return null;
    const trimmed = head.trim().toLowerCase();
    return trimmed === "" ? null : trimmed;
}

/**
 * 1 人ぶんのアバターを取得し、PNG の base64 にして返す。例外は投げない。省くときは理由つきで { ok: false } を返し、
 * 呼び出し元は avatar_png を省く。
 */
export async function fetchAvatarPng(deps: AvatarDeps, rawUrl: string): Promise<AvatarResult> {
    const url = normalizeAvatarUrl(rawUrl);
    if (url === null) return { ok: false, reason: "bad_url" };

    let fetched: AvatarFetchResult;
    try {
        fetched = await deps.fetchAvatar(url);
    } catch {
        return { ok: false, reason: "fetch_failed" };
    }
    if (!fetched.ok) return { ok: false, reason: "http_error" };

    const contentType = normalizeContentType(fetched.contentType);
    if (contentType === null || !ALLOWED_CONTENT_TYPES.includes(contentType)) {
        return { ok: false, reason: "bad_content_type" };
    }
    if (fetched.body.byteLength > AVATAR_MAX_SOURCE_BYTES) {
        return { ok: false, reason: "source_too_large" };
    }

    let pngBase64: string | null;
    try {
        pngBase64 = await deps.toPngBase64(fetched.body, contentType);
    } catch {
        return { ok: false, reason: "encode_failed" };
    }
    if (pngBase64 === null || pngBase64 === "") return { ok: false, reason: "encode_failed" };
    if (pngBase64.length > AVATAR_MAX_BASE64_LENGTH) {
        return { ok: false, reason: "png_too_large" };
    }
    return { ok: true, pngBase64 };
}
