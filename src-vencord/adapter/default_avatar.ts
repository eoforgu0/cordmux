// SPDX-License-Identifier: Apache-2.0

// 自分のアバターを設定していない人の既定のアバターの URL を、Discord の CDN の形で組み立てる (defaultAvatarUrl)。
// 呼び出し元は adapter/vencord.ts の avatarUrl()。Discord の getAvatarURL() が既定のアバターに返す URL は
// cdn.discordapp.com の外を指し、core/avatar.ts の normalizeAvatarUrl() が受け付けない (bad_url) ので、CDN の URL を
// ここで作る。Vencord も DOM の型も使わないので、リポジトリのルートの tsc と vitest でテストできる
// (tests/adapter_default_avatar.test.ts)。

/**
 * 既定のアバターの画像がある場所。`embed/avatars/4.png?size=64` を取得すると、200 を返し、Content-Type は
 * image/png、本体は 1334 バイト、Access-Control-Allow-Origin は `*` だった。core/avatar.ts の Content-Type と
 * バイト数の上限のチェックも、レンダラーの fetch の CORS も通る。Discord の CSP (connect-src) を通るかは、自分の
 * アバターの URL と同じく確認していない (どちらも cdn.discordapp.com)。
 *
 * getAvatarURL() が既定のアバターに返す文字列そのものは確認していない。Vencord の ViewIcons
 * (v1.15.17 の src/plugins/viewIcons/index.tsx) は "/" で始まる URL を Discord に同梱した画像として扱うので、
 * `/assets/<ハッシュ>.png` のような相対 URL だと考えられる。どちらの形でも CDN の外なので、この実装は
 * 返す形に依存しない。同梱した画像の名前はハッシュなので、そこから番号を読み戻すこともできない。
 *
 * core/avatar.ts の許すホストを discord.com まで広げて同梱の画像を通す方法は取らない。同じ画像が CDN に番号つきで
 * あるので、外部に接続するホストの制限を、既定のアバターのためだけに緩める理由がない。
 */
const DEFAULT_AVATAR_BASE = "https://cdn.discordapp.com/embed/avatars/";

/** 新しいユーザー名の仕組みの人の既定のアバターの数 (番号は 0 から 5)。 */
const DEFAULT_AVATAR_COUNT_BY_ID = 6n;

/** 旧来の discriminator の人の既定のアバターの数 (番号は 0 から 4)。 */
const DEFAULT_AVATAR_COUNT_BY_DISCRIMINATOR = 5n;

/** 10 進の数字だけの文字列 (`BigInt("")` は 0n になり、`BigInt(" 12 ")` も通るので、先にこれで確認する)。 */
const DIGITS_ONLY = /^[0-9]+$/;

/**
 * 既定のアバターの番号を返す。userId が数字だけの文字列でなければ (snowflake として読めなければ) null を返す。
 * discriminator が 1 以上の数として読めれば旧来の仕組みで `discriminator % 5`、それ以外 ("0"、"0000"、空、
 * ない、数字以外) は新しい仕組みで `(userId >> 22) % 6` にする。
 *
 * 番号の決め方は、Discord の開発者向けドキュメントの Reference の「Image Formatting」(CDN Endpoints の
 * 「Default User Avatar」と、その注) にある (https://docs.discord.com/developers/reference)。discord.js の
 * User.defaultAvatarURL も同じ分け方をする。番号の式は公開されているので、Discord の内部の関数
 * (IconUtils の既定のアバターの関数) は呼ばない。呼ぶと Discord の内部への依存箇所が 1 つ増え、Discord なしで
 * テストできなくなる。
 */
export function defaultAvatarIndex(userId: string, discriminator: string | null | undefined): number | null {
    if (!DIGITS_ONLY.test(userId)) return null;
    if (typeof discriminator === "string" && DIGITS_ONLY.test(discriminator)) {
        const legacy = BigInt(discriminator);
        if (legacy > 0n) return Number(legacy % DEFAULT_AVATAR_COUNT_BY_DISCRIMINATOR);
    }
    return Number((BigInt(userId) >> 22n) % DEFAULT_AVATAR_COUNT_BY_ID);
}

/**
 * 既定のアバターの CDN の URL を返す。番号が決まらなければ null を返す。この URL は PNG だけで、`?size=` を
 * 付けても大きさは変わらない (Discord の同じドキュメント)。core/avatar.ts は 64 x 64 に描き直すので、
 * 大きさが違っても送れる。
 */
export function defaultAvatarUrl(userId: string, discriminator: string | null | undefined): string | null {
    const index = defaultAvatarIndex(userId, discriminator);
    return index === null ? null : `${DEFAULT_AVATAR_BASE}${index}.png`;
}

/** avatarUrlOfUser() が読む、Discord のユーザーのオブジェクトの部分。 */
export interface AvatarUserLike {
    discriminator?: string | null;
    getAvatarURL(guildId: string | undefined, size: number, canAnimate: boolean): unknown;
}

/**
 * ユーザーのオブジェクトからアバターの URL を返す (adapter/vencord.ts の avatarUrl() が UserStore から取った
 * オブジェクトを渡す)。自分のアバターを設定していない人 (user.avatar のフィールドはあるが、値が null か空) には、
 * getAvatarURL() を使わず、CDN の既定のアバターの URL を返す。getAvatarURL() が既定のアバターに返す URL は
 * cdn.discordapp.com の外を指し、core/avatar.ts の normalizeAvatarUrl() が受け付けないためである。型の上では
 * avatar は string だが、既定のアバターの人の avatar は null になる (Discord の開発者向けドキュメントの
 * User Object)。空の文字列も同じに扱う。先に
 * "avatar" in user を確認するのは、Discord のバージョンが変わって avatar のフィールドがなくなったときに、全員を
 * 既定のアバターと取り違え、本人のものでない画像をログも出さずに送らないためである。フィールドがなければ
 * getAvatarURL() を使うので、自分のアバターの人はそのまま取得でき、既定のアバターの人はログに bad_url が残る。
 * getAvatarURL() が投げたとき、文字列でないか空の値を返したときは null を返す。
 */
export function avatarUrlOfUser(userId: string, user: AvatarUserLike): string | null {
    try {
        if ("avatar" in user && (typeof user.avatar !== "string" || user.avatar === "")) {
            return defaultAvatarUrl(userId, user.discriminator);
        }
        const raw = user.getAvatarURL(undefined, 128, false);
        return typeof raw === "string" && raw !== "" ? raw : null;
    } catch {
        return null;
    }
}
