// SPDX-License-Identifier: Apache-2.0

// 単体テスト用の Adapter (adapter/types.ts)。killedAdapters() は、依存箇所の一覧の行を 1 つずつ使えなくした
// Adapter を作る (使えなくした 1 行だけが missing で、ほかの 8 行は found になる)。createTrackFeedAdapter() は
// subscribeTracks() のテストに使い、feed() で TrackEvent を流せる。Vencord を import しない。

import { CONTACT_LEDGER, type ContactId } from "../ledger";
import type {
    Adapter,
    AdapterEvent,
    CallActionResult,
    ProbeResult,
    Snapshot,
    StartResult,
    TrackEvent,
} from "../adapter/types";
import { missingContactEvent } from "../core/state";

/** killedAdapters() が作る 1 件。使えなくした依存箇所と、その Adapter の組み合わせ。 */
export interface KilledAdapter {
    readonly killedId: ContactId;
    readonly adapter: Adapter;
}

/**
 * killedId だけが missing で、ほかの行はすべて found になる Adapter を作る。subscribe() は、呼び出されたときに
 * 一覧のすべての行の contact のイベントを 1 回だけ流す。roster、self、sharing は空か null のままにする。
 * startWatching は常に成功し、stopWatching は何もしない (このテストが見るのは、依存箇所が 1 つ欠けたときの core の
 * 動作だけで、視聴そのものではない)。
 */
function createKilledAdapter(killedId: ContactId): Adapter {
    return {
        probe(id: ContactId): Promise<ProbeResult> {
            return Promise.resolve({ found: id !== killedId });
        },

        subscribe(listener: (event: AdapterEvent) => void): () => void {
            for (const entry of CONTACT_LEDGER) {
                listener(
                    entry.id === killedId
                        ? missingContactEvent(entry.id)
                        : { type: "contact", id: entry.id, result: "found" },
                );
            }
            return () => {
                // 解除する購読がないので、何もしない。
            };
        },

        startWatching(): Promise<StartResult> {
            return Promise.resolve({ ok: true });
        },

        stopWatching(): Promise<void> {
            return Promise.resolve();
        },

        snapshot(): Promise<Snapshot> {
            return Promise.resolve({ selfUserId: null, selfUser: null, roster: [], sharing: [] });
        },

        // このテストが見るのは依存箇所の状態だけで、通話の操作は見ない。Adapter のメソッドをすべて実装するための、
        // 何もしない関数。
        setMainUser(): void {
            // 何もしない。
        },

        moveToMainCall(): CallActionResult {
            return { ok: false, reason: "not implemented in this fake" };
        },

        leaveCall(): CallActionResult {
            return { ok: false, reason: "not implemented in this fake" };
        },

        // このテストが見るのは依存箇所の状態だけで、トラックの追加と削除は見ない (adapter_tracks.test.ts が見る)。
        // Adapter のメソッドをすべて実装するための、何もしない関数。
        subscribeTracks(): () => void {
            return () => {
                // 購読していないので何もしない。
            };
        },
    };
}

/**
 * CONTACT_LEDGER の行を 1 つずつ使えなくした Adapter を、一覧の行の数だけ作る。行の数と、作った Adapter の数が
 * 合うことは、tests/fake_adapter_count.test.ts が確認する。
 */
export function killedAdapters(): readonly KilledAdapter[] {
    return CONTACT_LEDGER.map(entry => ({ killedId: entry.id, adapter: createKilledAdapter(entry.id) }));
}

/**
 * `subscribeTracks()` のテスト用の Adapter (adapter_tracks.test.ts が使う)。probe、subscribe、startWatching、
 * stopWatching、snapshot はテストの対象ではないので、何もしないか失敗を返すだけにする。`feed()` で、テストから
 * 好きな TrackEvent を流せる。
 */
export interface TrackFeedAdapter extends Adapter {
    /** 購読しているリスナーに、渡した TrackEvent をそのまま流す。 */
    feed(event: TrackEvent): void;
}

/** TrackFeedAdapter を作る。subscribeTracks() で登録したリスナーに、feed() で渡した TrackEvent をそのまま流す。 */
export function createTrackFeedAdapter(): TrackFeedAdapter {
    const listeners = new Set<(event: TrackEvent) => void>();

    return {
        probe(): Promise<ProbeResult> {
            return Promise.resolve({ found: false });
        },

        subscribe(): () => void {
            return () => {
                // 購読していないので何もしない。
            };
        },

        startWatching(): Promise<StartResult> {
            return Promise.resolve({ ok: false, reason: "このテスト用の adapter は視聴を実装しない" });
        },

        stopWatching(): Promise<void> {
            return Promise.resolve();
        },

        snapshot(): Promise<Snapshot> {
            return Promise.resolve({ selfUserId: null, selfUser: null, roster: [], sharing: [] });
        },

        setMainUser(): void {
            // 何もしない (このテスト用の adapter は通話の操作を実装しない)。
        },

        moveToMainCall(): CallActionResult {
            return { ok: false, reason: "not implemented in this fake" };
        },

        leaveCall(): CallActionResult {
            return { ok: false, reason: "not implemented in this fake" };
        },

        subscribeTracks(listener: (event: TrackEvent) => void): () => void {
            listeners.add(listener);
            return () => {
                listeners.delete(listener);
            };
        },

        feed(event: TrackEvent): void {
            for (const listener of listeners) listener(event);
        },
    };
}
