// SPDX-License-Identifier: Apache-2.0

// subscribeTracks() のテスト。tests/fake_adapter.ts の createTrackFeedAdapter() で track_added を流し、届いた
// TrackEvent の ssrcs() と pcId を core/state.ts の userIdForSsrc() に渡して userId を特定できること、pcId が
// 違えば特定しないこと、知らない SSRC なら undefined になることを確認する。

import { describe, expect, it } from "vitest";

import type { CoreEvent } from "../core/events";
import { initialState, reduce, userIdForSsrc } from "../core/state";
import type { CoreState } from "../core/state";
import type { TrackEvent } from "../adapter/types";
import { createTrackFeedAdapter } from "./fake_adapter";

function apply(events: readonly CoreEvent[], from: CoreState = initialState): CoreState {
    return events.reduce(reduce, from);
}

/** u1 が pcId 2 の接続で videoSsrc 111 を送っている状態 (roster にもいる)。 */
function stateWithKnownSsrc(): CoreState {
    return apply([
        { type: "roster", users: [{ userId: "u1" }] },
        { type: "ssrc", userId: "u1", videoSsrc: 111, audioSsrc: null, pcId: 2 },
    ]);
}

/** テスト用の adapter に track_added を 1 つ流し、届いた TrackEvent を返す。 */
function feedOneTrackAdded(pcId: number, ssrcValues: readonly number[]): TrackEvent {
    const adapter = createTrackFeedAdapter();
    const received: TrackEvent[] = [];
    const unsubscribe = adapter.subscribeTracks(ev => received.push(ev));

    adapter.feed({
        type: "track_added",
        pcId,
        kind: "video",
        track: { id: "t1", kind: "video", readyState: "live", stop() { /* テストでは何もしない */ } },
        ssrcs: () => [...ssrcValues],
        msid: null,
    });
    unsubscribe();

    expect(received).toHaveLength(1);
    const event = received[0];
    if (event === undefined) throw new Error("track_added が届いていない");
    return event;
}

describe("subscribeTracks の track_added から、core の userIdForSsrc で userId を特定する", () => {
    it("pcId が同じで SSRC を知っていれば、userId が分かる", () => {
        const state = stateWithKnownSsrc();
        const event = feedOneTrackAdded(2, [111]);
        if (event.type !== "track_added") throw new Error("track_added のはず");

        const ssrcs = event.ssrcs();
        const ssrc = ssrcs[0];
        if (ssrc === undefined) throw new Error("ssrcs() が空");

        expect(userIdForSsrc(state, event.pcId, ssrc)).toBe("u1");
    });

    it("SSRC の値が同じでも、pcId が違えば userId を特定しない", () => {
        const state = stateWithKnownSsrc();
        // u1 の SSRC 111 は pcId 2 のもの。別の接続 (pcId 3) から届いたことにして調べる。
        const event = feedOneTrackAdded(3, [111]);
        if (event.type !== "track_added") throw new Error("track_added のはず");

        const ssrcs = event.ssrcs();
        const ssrc = ssrcs[0];
        if (ssrc === undefined) throw new Error("ssrcs() が空");

        expect(userIdForSsrc(state, event.pcId, ssrc)).toBeUndefined();
    });

    it("知らない SSRC なら undefined を返す", () => {
        const state = stateWithKnownSsrc();
        const event = feedOneTrackAdded(2, [999]);
        if (event.type !== "track_added") throw new Error("track_added のはず");

        const ssrcs = event.ssrcs();
        const ssrc = ssrcs[0];
        if (ssrc === undefined) throw new Error("ssrcs() が空");

        expect(userIdForSsrc(state, event.pcId, ssrc)).toBeUndefined();
    });

    it("roster が読めていない ('unknown') 間は userId を特定しない", () => {
        // roster のイベントを 1 つも流していない状態 (roster は初期値の 'unknown' のまま)。
        const state = apply([{ type: "ssrc", userId: "u1", videoSsrc: 111, audioSsrc: null, pcId: 2 }]);
        expect(userIdForSsrc(state, 2, 111)).toBeUndefined();
    });

    it("track_removed も subscribeTracks から届く (core のイベントには混ぜない)", () => {
        const adapter = createTrackFeedAdapter();
        const received: TrackEvent[] = [];
        const unsubscribe = adapter.subscribeTracks(ev => received.push(ev));

        adapter.feed({ type: "track_removed", pcId: 2, kind: "video", trackId: "t1" });
        unsubscribe();

        expect(received).toEqual([{ type: "track_removed", pcId: 2, kind: "video", trackId: "t1" }]);
    });

    it("unsubscribe の後に feed() しても届かない", () => {
        const adapter = createTrackFeedAdapter();
        const received: TrackEvent[] = [];
        const unsubscribe = adapter.subscribeTracks(ev => received.push(ev));
        unsubscribe();

        adapter.feed({ type: "track_removed", pcId: 1, kind: "audio", trackId: "t2" });
        expect(received).toEqual([]);
    });
});
