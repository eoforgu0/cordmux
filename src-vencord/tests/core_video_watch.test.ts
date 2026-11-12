// SPDX-License-Identifier: Apache-2.0

// core/video_watch.ts のテスト。送信側が、映像を送れないまま 5 秒たったことを 1 回だけ伝え、送れたら戻ったことを
// 伝えることを確認する。

import { describe, expect, it } from "vitest";

import { VIDEO_WATCH_INITIAL, VIDEO_WATCH_STALL_MS, watchVideo } from "../core/video_watch";

describe("watchVideo", () => {
    it("送れないまま 5 秒たったら、止まった出来事を 1 回だけ返す", () => {
        let r = watchVideo(VIDEO_WATCH_INITIAL, 14_999, 10_000, 0);
        expect(r.event).toBeUndefined();
        r = watchVideo(r.state, 15_000, 10_000, 0);
        expect(r.event).toEqual({ type: "stalled", silentMs: VIDEO_WATCH_STALL_MS });
        r = watchVideo(r.state, 30_000, 10_000, 0);
        expect(r.event).toBeUndefined();
    });

    it("止まった後に送れたら、戻った出来事と止まっていた長さを返し、状態を戻す", () => {
        let r = watchVideo(VIDEO_WATCH_INITIAL, 20_000, 10_000, 0);
        expect(r.event?.type).toBe("stalled");
        r = watchVideo(r.state, 31_000, 30_500, 0);
        expect(r.event).toEqual({ type: "resumed", stalledMs: 20_500 });
        expect(r.state).toEqual(VIDEO_WATCH_INITIAL);
    });

    it("まだ 1 枚も送っていなければ、セッションを始めた時刻から数える (始め直した後も同じ)", () => {
        expect(watchVideo(VIDEO_WATCH_INITIAL, 104_000, undefined, 100_000).event).toBeUndefined();
        expect(watchVideo(VIDEO_WATCH_INITIAL, 105_000, undefined, 100_000).event?.type).toBe("stalled");
        // 始め直したセッションでは、前のセッションで最後に送れた時刻より、始めた時刻を使う。
        expect(watchVideo(VIDEO_WATCH_INITIAL, 104_000, 50_000, 100_000).event).toBeUndefined();
    });
});
