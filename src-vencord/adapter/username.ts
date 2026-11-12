// SPDX-License-Identifier: Apache-2.0

// Discord のユーザー名を、Discord の中で 1 人に 1 つの形に組み立てる (uniqueUsername)。受信側は、表示名が
// 同じ人を見分けるために、この値を profiles の username として受け取って表示する。呼び出し元は
// adapter/vencord.ts の computeRoster()。Vencord も DOM の型も使わないので、リポジトリのルートの tsc と vitest で
// テストできる (tests/adapter_username.test.ts)。

/** 10 進の数字だけの文字列 (adapter/default_avatar.ts と同じ判定)。 */
const DIGITS_ONLY = /^[0-9]+$/;

/**
 * Discord のユーザー名を、Discord の中で 1 人に 1 つの形で返す。新しいユーザー名の仕組みの人 (discriminator が
 * "0") は username のまま、旧来の 4 桁の discriminator を持つ人は username#1234 にする。旧来の人は、username
 * だけでは 1 人に決まらない。discriminator が数字だけで 1 以上なら旧来とし、"0"、"0000"、空、ない、数字以外は
 * 新しい仕組みとする (adapter/default_avatar.ts の defaultAvatarIndex() と同じ判定)。
 *
 * username が空でない文字列でなければ null を返し、index.ts は profiles の username を省く。空の文字列や userId で
 * 埋めると、受信側がそれをユーザー名として表示してしまう。
 *
 * Vencord v1.15.17 の src/utils/discord.tsx の getUniqueUsername() も、discriminator が "0" なら username、
 * そうでなければ user.tag を返す。それを呼ばないのは、Discord の User の getter (user.tag) を読むので、
 * Discord の内部への依存箇所が 1 つ増えるためである。
 */
export function uniqueUsername(username: unknown, discriminator: unknown): string | null {
    if (typeof username !== "string" || username === "") return null;
    if (typeof discriminator === "string" && DIGITS_ONLY.test(discriminator) && BigInt(discriminator) > 0n) {
        return `${username}#${discriminator}`;
    }
    return username;
}
