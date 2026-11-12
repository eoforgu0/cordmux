// SPDX-License-Identifier: Apache-2.0

// 依存箇所の一覧 (CONTACT_LEDGER)。cordmux が読む Discord の内部の部分 9 つを、名前ではなく、何をするものかを表す
// 述語で持つ。画面の文言は持たない。見つからないときにドックに表示する見出しは、受信側の
// src-obs/ledger/rules_dock.cpp の kContactHeadlines が id から決め、診断情報には id をそのまま書く。この一覧の
// id と kContactHeadlines が過不足なく対応することは tests/ledger_ini.test.ts が、一覧の形は tests/ledger.test.ts
// が確認する。Vencord を import しないので、リポジトリのルートの tsc と vitest でテストできる。

/**
 * 述語の種類。Vencord の探索の API に合わせた 2 つだけを持つ。
 *
 * - "props": 1 つのモジュールが、指定した名前をすべて持つ (`findByProps` と `findStore` に当たる)。
 * - "code": 1 つの関数の本文が、指定した文字列をすべて含む (`findByCode` に当たる)。
 *
 * 実行時 (adapter/vencord.ts) は、この述語を使わない。述語に一致するモジュールが見つかっても、adapter/vencord.ts
 * が実際に使う参照 (解決したストアのメソッド、フックの設置) が使えるとは限らないので、adapter/vencord.ts は
 * それらが今使えるかを直接確認する。
 *
 * この述語を使うのは、保存した Discord のバンドルを調べるチェック (tests/discord_contacts.test.ts と
 * test/tools/discord-bundle/scan.ts) だけである。実行する前のソースコード (モジュールごとの文字列) が述語に一致するかを
 * 調べる近似のチェックで、一致するモジュールがちょうど 1 つであることを確認する (pc-hook の行は 1 つ以上でよい)。
 * 絞っても 1 つにできない行は、依存箇所を減らすか、壊れやすい行としてその行のコメントに書く。
 */
export type PredicateSpec =
    | { kind: "props"; props: readonly string[] }
    | { kind: "code"; code: readonly string[] };

/**
 * 依存箇所の id。受信側の kContactHeadlines と診断情報が、この文字列をそのまま使うので、変えるときは受信側も修正する。
 */
export type ContactId =
    | "pc-hook"
    | "ssrc-map"
    | "sharing"
    | "watch-stop"
    | "roster"
    | "profile"
    | "self"
    | "call-info"
    | "voice-select";

/** 依存箇所の一覧の 1 行。 */
export interface ContactEntry {
    id: ContactId;
    /** 何で探すか。名前ではなく、何をするものかで探す。 */
    predicate: PredicateSpec;
    /**
     * 常に false。どの依存箇所が見つからなくても、既に送っている映像と音声を止めない。core/state.ts の Decision 型に
     * 止める判断がないことに加えて、この型でも約束を表す。
     */
    stopsExisting: false;
}

/**
 * 依存箇所の一覧の 9 行。各行の述語は、Vencord の src/webpack/webpack.ts (`findByProps`、`findByCode`、`findStore`
 * の実装) と src/webpack/common の stores.ts と utils.ts (`@webpack/common` で解決するストア) と、Discord の
 * クライアントが実際に持つメソッドの名前と action の type の文字列から決めた。
 *
 * pc-hook と ssrc-map の 2 行は、実行時の仕組み (RTCPeerConnection の差し替えと、voice gateway の WebSocket の
 * メッセージの読み取り) が webpack のモジュールの探索ではない。そのため、この 2 行の述語は、その仕組みの前提が
 * Discord のバンドルからまだ読み取れるかを確認する。
 */
export const CONTACT_LEDGER: readonly ContactEntry[] = [
    {
        id: "pc-hook",
        // RTCPeerConnection を startAt: Init で差し替える。差し替えはグローバルの置き換えで、findByProps や
        // findByCode で探す webpack のモジュールではない。この述語は、Discord のメディアエンジンが今も
        // RTCPeerConnection で画面と音声を受け取っているという前提が崩れていないかを確認する。差し替えが実際に
        // 動作しているか (`.toString()` に `[native code]` がないか) は、フックを設置する側が確認する。
        predicate: { kind: "code", code: ["RTCPeerConnection"] },
        stopsExisting: false,
    },
    {
        id: "ssrc-map",
        // voice gateway の op12 (`video_ssrc`) と op5 (speaking) から、SSRC と userId の対応を読む。op12 のフィールドの
        // 名前 video_ssrc を、そのまま探す。WebSocket のメッセージの読み取りは、pc-hook と同じく webpack の
        // モジュールの探索ではない。
        predicate: { kind: "code", code: ["video_ssrc"] },
        stopsExisting: false,
    },
    {
        id: "sharing",
        // 画面共有している人。Go Live の接続 1 つが、画面共有している人 1 人に対応する。ApplicationStreamingStore の
        // getAnyStreamForUser で読む。getAnyStreamForUser だけでは 11 から 16 個のモジュールに一致するので、
        // 保存したバンドル (stable の 611316 と canary の 612347) でちょうど 1 つに一致する組み合わせにした。
        predicate: { kind: "props", props: ["getAnyStreamForUser", "getAllActiveStreamsForChannel"] },
        stopsExisting: false,
    },
    {
        id: "watch-stop",
        // 視聴の開始と停止。dispatch する action の type を探す。findByCode は部分文字列が含まれるかを見る (正規表現では
        // ない) ので、いちばん絞れる STREAM_WATCH を使う。開始と停止の関数を実際に解決するのは、adapter/vencord.ts の
        // startWatching と stopWatching である。STREAM_WATCH だけでは 9 から 11 個のモジュールに一致し、視聴の開始で
        // 使う forceMultiple を一緒に含むモジュールに絞ると、保存したバンドルでちょうど 1 つになる。
        predicate: { kind: "code", code: ["STREAM_WATCH", "forceMultiple"] },
        stopsExisting: false,
    },
    {
        id: "roster",
        // 通話に参加している人。今いるボイスチャンネルは RTCConnectionStore.getChannelId() で読む。getChannelId だけでは
        // 176 から 218 個のモジュールに一致して探せないので、RTCConnectionStore が一緒に持つ getHostname と
        // getOutboundLossRate も含めると、保存したバンドルでちょうど 1 つになる。
        predicate: { kind: "props", props: ["getChannelId", "getHostname", "getOutboundLossRate"] },
        stopsExisting: false,
    },
    {
        id: "profile",
        // 名前とアバター。UserStore と GuildMemberStore から読む。この行が確認するのは UserStore である
        // (GuildMemberStore のニックネームは、UserStore の名前を補うだけ)。UserStore.getUser で読み、getUser だけでは
        // 210 から 292 個のモジュールに一致するので、UserStore が一緒に持つ getUserStoreVersion と findByTag も
        // 含めると、保存したバンドルでちょうど 1 つになる。
        predicate: { kind: "props", props: ["getUser", "getUserStoreVersion", "findByTag"] },
        stopsExisting: false,
    },
    {
        id: "self",
        // Vesktop のアカウント。UserStore.getCurrentUser で読む。profile と同じ UserStore を指すが、見つからない
        // ときの利用者への伝え方の重さが違う (自分が誰か分からないのは、名前とアバターが読めないことより根本的な問題で
        // ある) ので、別の行にする。getCurrentUser だけでは 410 から 591 個のモジュールに一致するので、profile と同じ
        // 組み合わせ (getUserStoreVersion と findByTag) で UserStore に絞る。
        predicate: { kind: "props", props: ["getCurrentUser", "getUserStoreVersion", "findByTag"] },
        stopsExisting: false,
    },
    {
        id: "call-info",
        // 通話の名前と、メインアカウントがいる通話。ChannelStore.getChannel でチャンネルの名前と種類を、GuildStore で
        // サーバーの名前を、VoiceStateStore.getVoiceStateForUser でメインアカウントのいる通話を、PermissionStore で
        // 接続の権限を読む。述語が確認するのは ChannelStore である (GuildStore、VoiceStateStore、PermissionStore は
        // 名前と参加できるかを補うだけで、読めないときはそのキーだけを省く)。getChannel だけでは複数のモジュールに
        // 一致するので、ChannelStore が一緒に持つ getMutableBasicGuildChannelsForGuild と getDMFromUserId も含めると、
        // 保存したバンドル (stable の 611316 から canary の 653876 まで 7 つ) でちょうど 1 つになる。
        predicate: { kind: "props", props: ["getChannel", "getMutableBasicGuildChannelsForGuild", "getDMFromUserId"] },
        stopsExisting: false,
    },
    {
        id: "voice-select",
        // [移動] と [退出] を実行する関数。selectVoiceChannel(channelId) で通話を移り、selectVoiceChannel(null) で
        // 退出する (Discord 自身も null を渡して退出する)。この関数は props の述語では 1 つに絞れない (関数の名前を
        // 呼ぶモジュールが、ほかに 2 つある) ので、関数の本文を探す。本文にある isGuildVocalOrThread と
        // getMediaEngine().interact() を含む組み合わせが、保存したバンドルでちょうど 1 つになる。実行時に関数を見つけるのは
        // adapter/vencord.ts の findByProps("selectVoiceChannel", "selectChannel") である。
        predicate: { kind: "code", code: ["isGuildVocalOrThread", "getMediaEngine().interact()"] },
        stopsExisting: false,
    },
];
