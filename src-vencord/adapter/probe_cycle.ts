// SPDX-License-Identifier: Apache-2.0

// 依存箇所の周期的な確認 (adapter/vencord.ts の runProbeCycle と recheckOne) で、確認した結果から、送る contact の
// イベントと、次に確認するまでの待ち方を決める。Vencord のモジュールを読まない純粋関数なので、Node.js のテストから
// 呼べる。
//
// Discord にログインする前 (自分のアカウントがまだ分からない間) は、見つからなかった依存箇所を missing として送らない。
// ログインの画面の間は、Discord のモジュールがまだ読み込まれていないことがあり、見つからないのは異常ではない。
// missing を送ると、受信側のドックが「Discord の内部の一部を読み取れません」のメッセージを出してしまう。見つかった物は
// 送り、探し直しの回数は数えずに、ログインするまで同じ間隔で探し直す。

import type { ContactId } from "../ledger";

/** 1 回の確認の結果から決めたこと。 */
export type ProbeCyclePlan = {
    /** 送る contact のイベント (確認した順)。 */
    emit: { id: ContactId; result: "found" | "missing" }[];
    /** 次に確認するまでの待ち方。watchdog は全部見つかったときの長い間隔、rediscover は探し直しの短い間隔、stop は探し直しの上限に達して待つ。 */
    next: "watchdog" | "rediscover" | "stop";
    /** 次の確認に持ち越す探し直しの回数。 */
    retries: number;
};

/**
 * 周期的な確認の 1 回分を決める。results は確認した依存箇所と見つかったか、waitingForLogin は Discord にまだ
 * ログインしていないか、retries はこれまでの探し直しの回数、maxRetries はその上限。
 */
export function planProbeCycle(
    results: readonly (readonly [ContactId, boolean])[],
    waitingForLogin: boolean,
    retries: number,
    maxRetries: number,
): ProbeCyclePlan {
    const emit: ProbeCyclePlan["emit"] = [];
    let allFound = true;
    for (const [id, found] of results) {
        if (!found) allFound = false;
        if (found || !waitingForLogin) emit.push({ id, result: found ? "found" : "missing" });
    }
    if (allFound) return { emit, next: "watchdog", retries: 0 };
    if (waitingForLogin) return { emit, next: "rediscover", retries };
    if (retries < maxRetries) return { emit, next: "rediscover", retries: retries + 1 };
    return { emit, next: "stop", retries };
}

/**
 * 読み取りや視聴の開始に失敗したときに、依存箇所 1 つを再確認した結果を送るか。ログインの前に見つからなかったときは
 * 送らない (planProbeCycle と同じ理由)。
 */
export function shouldEmitRecheck(found: boolean, waitingForLogin: boolean): boolean {
    return found || !waitingForLogin;
}

/** 自分のアカウントを読んだ結果。ストアが読めないことと、自分が誰か分からないこと (ログインの前) を分ける。 */
export type SelfRead = { ok: true; userId: string | null } | { ok: false };

/**
 * 自分のアカウントを読んだ結果 read と、前に送った値 sent (まだ 1 度も送っていなければ undefined) から、することを決める。
 * ストアが読めなければ再確認する (recheck)。読めて、まだ送っていないか値が変わっていれば送る (send)。同じなら何もしない (none)。
 * FluxDispatcher は Discord へのログインより先に使えるようになるので、最初に読んだときは自分のアカウントがまだ空のことが
 * ある。値が変わったときに送り直さないと、受信側は受け取りに使うアカウントを最後まで見分けられない (画面共有していない
 * 参加者の並びに、受け取りに使うアカウントが出る)。
 */
export function planSelfEmit(read: SelfRead, sent: string | null | undefined): "recheck" | "send" | "none" {
    if (!read.ok) return "recheck";
    if (sent === undefined || read.userId !== sent) return "send";
    return "none";
}
