// SPDX-License-Identifier: Apache-2.0

// adapter が、Discord と Vencord の中で起きたことを core に伝えるイベントの型 (CoreEvent)。イベントは事実だけを
// 運び、判断は持たない。判断は core/state.ts の reduce() が作る。Vencord のものは import しないので、リポジトリの
// ルートの tsc と vitest でテストできる。

// ContactId は、依存箇所の一覧 (ledger.ts) の行の id。どの依存箇所があるかは一覧だけで決めるので、型も一覧から
// 取る。型だけの import なので、ledger.ts の実行時の値には依存しない。
import type { ContactId } from "../ledger";

export type { ContactId };

/**
 * Discord の内部への依存箇所 1 つを確認した結果。
 * - "found": 使えることを確認した。
 * - "missing": 使えなかった。core は、"found" の後に来た "missing" を "lost" (使えていたのに使えなくなった)
 *   として扱う。
 * - "lost": adapter が直接送ってもよい。adapter/vencord.ts は送らない。
 */
export type ContactResult = "found" | "missing" | "lost";

/**
 * roster イベントが運ぶ、通話に参加している 1 人の事実。名前とアバターは profile の依存箇所で読むので、
 * ここには持たない。
 */
export interface RosterUser {
    userId: string;
}

/**
 * メインアカウントがいる通話。Vesktop のアカウントから見えるサーバーと会話の中でだけ読める。userId は、読んだときに
 * adapter が使っていたメインアカウントの userId で、facts にそのまま入れる (受信側は、設定を変えた直後の古い値を
 * 捨てるために照合する)。name と joinable は、読めなかったときは null で、適当な値で埋めない。
 */
export interface MainCallInfo {
    userId: string;
    name: string | null;
    joinable: boolean | null;
}

/**
 * core が扱うイベント。1 つのイベントには 1 つの事実だけを入れる (同じ事実の扱いを、reduce() の 1 か所で
 * 決めるため)。
 */
export type CoreEvent =
    /** 依存箇所 1 つを確認した結果。 */
    | { type: "contact"; id: ContactId; result: ContactResult }
    /** 通話に参加している人の一覧を読めた (roster の依存箇所)。読めなかったときは、このイベントを送らない。
     *  0 人 (空の配列) と、分からない (まだ 1 度もイベントが来ていない) は、core/state.ts が区別して持つ。 */
    | { type: "roster"; users: readonly RosterUser[] }
    /** 画面共有の開始と終了 (sharing の依存箇所)。pcId は、その画面共有を運ぶ接続の番号
     *  (adapter/connections.ts の ConnectionTracker が割り当てる。まだ視聴していなければ 0)。 */
    | { type: "sharing"; userId: string; streamKey: string; active: boolean; pcId: number }
    /** voice gateway の op12 と op5 で読んだ ssrc (ssrc-map の依存箇所)。映像の送信元が替わったことの検出と、
     *  トラックの userId の特定に使う。読めない値は null。 */
    | { type: "ssrc"; userId: string; videoSsrc: number | null; audioSsrc: number | null; pcId: number }
    /** Vesktop のアカウントの userId (self の依存箇所)。分からないとき、ログインしていないときは null。 */
    | { type: "self"; userId: string | null }
    /** 視聴する人の集合。core はこの集合を作らず、受け取るだけ。adapter/vencord.ts は、画面共有している人のうち、
     *  Vesktop のアカウント以外の全員を送る。 */
    | { type: "selection"; userIds: readonly string[] }
    /**
     * adapter が startWatching() を実行した結果。視聴の開始に失敗したときに探し直す処理は adapter が持ち、
     * core は、決めた start-watching (state.watching) が実際に始まったかを受け取るだけ。ok が false で、
     * その要求がまだ最新なら、core は state.watching から取り消し、次に sharing か selection が変わったときに
     * start-watching を出し直せるようにする。ok が true なら何もしない (start-watching を決めた時点で、
     * state.watching に入れてある)。reason は、そのままドックに表示できる文言ではない。
     */
    | { type: "watch_result"; userId: string; streamKey: string; ok: boolean; reason?: string }
    /** Vesktop のアカウントがいる通話の名前 (call-info の依存箇所)。通話にいないとき、名前を読めないときは null。 */
    | { type: "call_name"; name: string | null }
    /**
     * メインアカウントがいる通話 (call-info の依存箇所)。null は、メインアカウントの
     * userId を知らない、または Vesktop のアカウントから見える通話にいない。見えない通話に参加していることと、どの通話にも
     * いないことは区別できないので、null を「通話にいない」とは扱わない。
     */
    | { type: "main_call"; call: MainCallInfo | null }
    /**
     * 受信側が選んだ、視聴する人の範囲。null は画面共有している全員、配列はその人だけ (空なら誰も視聴しない。core の初期値は空で、範囲が届くまで誰も視聴しない)。core は、selection と
     * sharing とこの範囲のすべてに入っている人を視聴する。
     */
    | { type: "watch_scope"; userIds: readonly string[] | null }
    /** 時刻が進んだ。core は時刻を保存するだけで、経過時間に基づく判断はまだ持たない。 */
    | { type: "tick"; nowMs: number };
