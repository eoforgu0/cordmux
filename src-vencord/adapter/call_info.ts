// SPDX-License-Identifier: Apache-2.0

// 通話の名前と、サブアカウントがその通話に参加できるかを決める純粋関数。ストアから読んだ値を受け取るだけで、
// Vencord を import しないので、リポジトリのルートの tsc と vitest でテストできる。ストアを読むのは
// adapter/vencord.ts である。

/** Discord のチャンネルの種類の数値 (ChannelType)。通話を持つ種類だけを使う。 */
export const ChannelKind = {
    Dm: 1,
    GuildVoice: 2,
    GroupDm: 3,
    GuildStageVoice: 13,
} as const;

/** 通話の名前を作る材料。ストアから読めた値だけを入れる。 */
export interface CallNameParts {
    /** チャンネルの名前。DM とグループ DM で名前がないときは空の文字列。 */
    readonly channelName: string;
    /** サーバーの名前。サーバーのチャンネルでないとき、読めないときは null。 */
    readonly guildName: string | null;
    /** DM とグループ DM の相手の表示名。サーバーのチャンネルでは使わない。 */
    readonly recipientNames: readonly string[];
}

/**
 * 通話の名前。サーバーのチャンネルは「チャンネルの名前 / サーバーの名前」にする (Discord の通話のパネルと同じ順)。ドックの
 * 行は狭く末尾を省略するので、同じサーバーの別のチャンネルを見分けられるように、チャンネルの名前を先に置く。サーバーの名前は、
 * 別のサーバーの同じ名前のチャンネルを見分けるために添える。サーバーの名前が読めないときはチャンネルの名前だけにする。DM と
 * グループ DM は、名前があればその名前、なければ相手の表示名を ", " でつなぐ。どれも読めなければ null を返し、適当な名前を作らない。
 */
export function callNameOf(parts: CallNameParts): string | null {
    const channel = parts.channelName.trim();
    const guild = parts.guildName === null ? "" : parts.guildName.trim();
    if (channel !== "" && guild !== "") return `${channel} / ${guild}`;
    if (channel !== "") return channel;
    const names = parts.recipientNames.map((n) => n.trim()).filter((n) => n !== "");
    return names.length > 0 ? names.join(", ") : null;
}

/** サブアカウントが参加できるかを決める材料。読めなかった値は null にする。 */
export interface JoinableInput {
    /** チャンネルの種類 (ChannelKind の値)。チャンネルを読めなかったときは null。 */
    readonly kind: number | null;
    /** チャンネルに接続する権限があるか。読めなかったときは null。 */
    readonly canConnect: boolean | null;
    /** 参加人数の上限。0 は上限なし。 */
    readonly userLimit: number;
    /** いま通話に参加している人数。 */
    readonly occupants: number;
    /** 人数の上限を超えて入れる権限 (メンバーの移動) があるか。読めなかったときは false。 */
    readonly canBypassLimit: boolean;
}

/**
 * サブアカウントがその通話に参加できるか。true / false は読めた値から決めた結果で、決められないときは null を返す。
 * DM とグループ DM は、チャンネルがストアにあれば (サブアカウントがその会話にいる) 参加できる。サーバーの
 * ボイスチャンネルとステージは、接続の権限があり、人数の上限に達していない (上限を超えて入れる権限があれば無視する)
 * ときに参加できる。ステージには人数の上限を見ない (聴衆として入る)。
 */
export function joinableOf(input: JoinableInput): boolean | null {
    if (input.kind === null) return null;
    if (input.kind === ChannelKind.Dm || input.kind === ChannelKind.GroupDm) return true;
    if (input.kind !== ChannelKind.GuildVoice && input.kind !== ChannelKind.GuildStageVoice) return null;
    if (input.canConnect === null) return null;
    if (!input.canConnect) return false;
    if (input.kind === ChannelKind.GuildVoice && input.userLimit > 0 && input.occupants >= input.userLimit) {
        return input.canBypassLimit;
    }
    return true;
}
