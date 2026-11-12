// SPDX-License-Identifier: Apache-2.0

// core/gate.ts の planGate() と createMediaGate() のテスト。送信側は、受信側と接続していて media が true の間だけ
// トラックを読み出してエンコードし、切れたらすべて止める。最後のテストは、ゲートのコールバックに encode/session.ts
// (エンコーダはテスト用) をつなぎ、ゲートが閉じている間に createEncoder、makeBlackFrame、アバターの取得が 1 回も
// 起きないことを数える。

import { describe, expect, it } from "vitest";

import type { MediaStreamTrackLike } from "../adapter/types";
import { avatarFetchesToStart } from "../core/facts";
import {
    createMediaGate,
    DEFAULT_MEDIA,
    gateOpen,
    planGate,
    type GateInputs,
    type GateTrackState,
    type LedgerTrack,
    type StopReason,
} from "../core/gate";
import type { VideoFrameSource } from "../encode/frame_source";
import { limitsFromWire } from "../encode/limits";
import { SerialGate } from "../encode/serial";
import { createEncodeSession, type EncodeSessionDeps, type EncodeSessionHandle } from "../encode/session";
import type { EncodedVideoChunk, VideoEncoderLike, VideoFrame } from "../encode/webcodecs_types";

// ---- テスト用の関数 ----------------------------------------------------------

/** readyState を後から "ended" にできる、テスト用のトラック。 */
function fakeTrack(id: string, kind: "video" | "audio" = "video"): MediaStreamTrackLike & { end(): void } {
    let state: "live" | "ended" = "live";
    return {
        id,
        kind,
        get readyState() {
            return state;
        },
        stop(): void {
            state = "ended";
        },
        end(): void {
            state = "ended";
        },
    };
}

function ledgerTrack(id: string, pcId = 2, kind: "video" | "audio" = "video") {
    const track = fakeTrack(id, kind);
    const entry: LedgerTrack = { pcId, kind, track, ssrcs: () => [] };
    return { entry, track };
}

/** 呼ばれたコールバックを順に記録するゲート。 */
function recordingGate() {
    const calls: string[] = [];
    const tokens = new Map<string, number>();
    const gate = createMediaGate({
        start(track, token) {
            calls.push(`start ${track.track.id}`);
            tokens.set(track.track.id, token);
        },
        stop(trackId, reason) {
            calls.push(`stop ${trackId} ${reason}`);
        },
        onOpenChanged(open) {
            calls.push(open ? "open" : "closed");
        },
    });
    return { gate, calls, tokens };
}

/** 決まった並びの疑似乱数 (テストを毎回同じにする)。 */
function lcg(seed: number): () => number {
    let s = seed >>> 0;
    return () => {
        s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
        return s / 0x100000000;
    };
}

// ---- planGate (純粋関数) -------------------------------------------------------

describe("planGate: トラックの一覧、確立、media から、始めるトラックと止めるトラックを決める", () => {
    type Row = readonly [string, GateTrackState];
    const live = (id: string): Row => [id, { track: { readyState: "live" } }];
    const ended = (id: string): Row => [id, { track: { readyState: "ended" } }];
    const OPEN: GateInputs = { established: true, media: true };

    it("開いていれば、一覧にあって動いていて、まだ始めていないトラックを、一覧の順に始める", () => {
        const ledger = new Map([live("a"), live("b"), ended("c"), live("d")]);
        expect(planGate(ledger, OPEN, ["b"])).toEqual({ start: ["a", "d"], stop: [] });
    });

    it("確立していなければ何も始めず、動かしているトラックをすべて disconnected で止める", () => {
        const ledger = new Map([live("a"), live("b")]);
        expect(planGate(ledger, { established: false, media: true }, ["a", "b"])).toEqual({
            start: [],
            stop: [
                { trackId: "a", reason: "disconnected" },
                { trackId: "b", reason: "disconnected" },
            ],
        });
    });

    it("media が false なら何も始めず、動かしているトラックをすべて media-off で止める", () => {
        const ledger = new Map([live("a"), live("b")]);
        expect(planGate(ledger, { established: true, media: false }, ["b"])).toEqual({
            start: [],
            stop: [{ trackId: "b", reason: "media-off" }],
        });
    });

    it("一覧から消えたトラックと終わったトラックは、開いていても track-ended で止める (ほかの理由より先に決める)", () => {
        const ledger = new Map([live("a"), ended("b")]);
        expect(planGate(ledger, OPEN, ["a", "b", "gone"])).toEqual({
            start: [],
            stop: [
                { trackId: "b", reason: "track-ended" },
                { trackId: "gone", reason: "track-ended" },
            ],
        });
        expect(planGate(ledger, { established: false, media: false }, ["b"]).stop).toEqual([
            { trackId: "b", reason: "track-ended" },
        ]);
    });

    it("差分を適用すると、動かしているトラックが、開いていれば一覧の動いているトラックと、閉じていれば空と過不足なく一致する", () => {
        const rand = lcg(20000101);
        const ids = ["t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7"];
        for (let round = 0; round < 500; round++) {
            const ledger = new Map<string, { track: { readyState: "live" | "ended" } }>();
            for (const id of ids) {
                const r = rand();
                if (r < 0.45) ledger.set(id, { track: { readyState: "live" } });
                else if (r < 0.6) ledger.set(id, { track: { readyState: "ended" } });
            }
            const running = ids.filter(() => rand() < 0.4);
            const inputs: GateInputs = { established: rand() < 0.7, media: rand() < 0.7 };

            const plan = planGate(ledger, inputs, running);
            const startSet = new Set(plan.start);
            const stopIds = plan.stop.map((s) => s.trackId);
            const stopSet = new Set(stopIds);

            // 同じトラックを 2 回出さない。始めるものと止めるものに同じトラックがない。始めるのは動かしていない
            // トラックで、止めるのは動かしているトラックである。
            expect(startSet.size).toBe(plan.start.length);
            expect(stopSet.size).toBe(stopIds.length);
            for (const id of plan.start) {
                expect(running.includes(id)).toBe(false);
                expect(stopSet.has(id)).toBe(false);
            }
            for (const id of stopIds) expect(running.includes(id)).toBe(true);

            const after = new Set(running.filter((id) => !stopSet.has(id)));
            for (const id of plan.start) after.add(id);
            const desired = new Set(
                gateOpen(inputs) ? [...ledger].filter(([, e]) => e.track.readyState === "live").map(([id]) => id) : [],
            );
            expect([...after].sort()).toEqual([...desired].sort());
        }
    });
});

// ---- createMediaGate (状態を持つ部分) ---------------------------------------------

describe("createMediaGate: 確立している間だけ始め、切れたらすべて止める", () => {
    it("確立する前に届いたトラックは始めず、確立したときに一覧から始める", () => {
        const { gate, calls } = recordingGate();
        gate.trackAdded(ledgerTrack("a").entry);
        gate.trackAdded(ledgerTrack("b").entry);
        expect(calls).toEqual([]);
        expect(gate.isOpen()).toBe(false);

        expect(gate.isConnected()).toBe(false);
        gate.connected();
        expect(gate.isConnected()).toBe(true);
        expect(calls).toEqual(["start a", "start b", "open"]);
        expect(gate.runningCount()).toBe(2);
        gate.disconnected();
        expect(gate.isConnected()).toBe(false);
    });

    it("切れたらすべて disconnected で止め、再接続したら同じトラックを新しい世代で始め直す", () => {
        const { gate, calls, tokens } = recordingGate();
        gate.trackAdded(ledgerTrack("a").entry);
        gate.connected();
        const firstToken = tokens.get("a") ?? -1;
        expect(gate.isCurrent("a", firstToken)).toBe(true);

        gate.disconnected();
        expect(calls.slice(-2)).toEqual(["stop a disconnected", "closed"]);
        expect(gate.isCurrent("a", firstToken)).toBe(false); // 途中だった userId の特定は、何も作らない

        gate.connected();
        const secondToken = tokens.get("a") ?? -1;
        expect(secondToken).not.toBe(firstToken);
        expect(gate.isCurrent("a", secondToken)).toBe(true);
        expect(gate.isCurrent("a", firstToken)).toBe(false);
    });

    it("動かしているトラックが終われば track-ended で止め、一覧からも消す", () => {
        const { gate, calls } = recordingGate();
        const a = ledgerTrack("a");
        gate.trackAdded(a.entry);
        gate.connected();
        gate.trackRemoved("a");
        expect(calls).toEqual(["start a", "open", "stop a track-ended"]);
        gate.disconnected();
        gate.connected();
        expect(calls.filter((c) => c === "start a")).toHaveLength(1); // 消えたトラックは始め直さない
    });

    it("track_removed が届かなくても readyState が ended なら、次に計算し直すときに止めて一覧から消す", () => {
        const { gate, calls } = recordingGate();
        const a = ledgerTrack("a");
        gate.trackAdded(a.entry);
        gate.connected();
        a.track.end();
        gate.trackAdded(ledgerTrack("b").entry); // ほかのイベントで計算し直す
        expect(calls).toEqual(["start a", "open", "stop a track-ended", "start b"]);
        gate.disconnected();
        gate.connected();
        expect(calls.slice(-3)).toEqual(["closed", "start b", "open"]);
    });

    it("閉じている間に消えたトラックは、stop のコールバックを呼ばずに一覧から消える", () => {
        const { gate, calls } = recordingGate();
        gate.trackAdded(ledgerTrack("a").entry);
        gate.trackRemoved("a");
        gate.connected();
        expect(calls).toEqual(["open"]);
    });

    it("同じ id のトラックが 2 回届いても 1 回しか始めない", () => {
        const { gate, calls } = recordingGate();
        gate.connected();
        const a = ledgerTrack("a");
        gate.trackAdded(a.entry);
        gate.trackAdded(a.entry);
        expect(calls.filter((c) => c === "start a")).toHaveLength(1);
    });
});

describe("createMediaGate: media", () => {
    it("hello_ok の初期値が false なら、確立しても始めず、true になったときに始める", () => {
        const { gate, calls } = recordingGate();
        gate.trackAdded(ledgerTrack("a").entry);
        expect(gate.setMedia(false)).toBe(true); // 確立の通知より先に届く (transport/socket.ts)
        gate.connected();
        expect(calls).toEqual([]);
        expect(gate.isOpen()).toBe(false);

        expect(gate.setMedia(true)).toBe(true);
        expect(calls).toEqual(["start a", "open"]);
    });

    it("false になったらすべて media-off で止め、同じ値がもう一度届いても何もしない", () => {
        const { gate, calls } = recordingGate();
        gate.trackAdded(ledgerTrack("a").entry);
        gate.trackAdded(ledgerTrack("b", 1, "audio").entry);
        gate.connected();
        expect(gate.setMedia(true)).toBe(false); // 変わっていない
        expect(gate.setMedia(false)).toBe(true);
        expect(calls).toEqual(["start a", "start b", "open", "stop a media-off", "stop b media-off", "closed"]);
        expect(gate.setMedia(false)).toBe(false);
        expect(calls).toHaveLength(6);
    });

    it("media を送らない古い受信側なら開いたままにし、切れたら既定の値 (true) に戻して、前の接続の false を持ち越さない", () => {
        const { gate } = recordingGate();
        expect(gate.media()).toBe(DEFAULT_MEDIA);
        expect(DEFAULT_MEDIA).toBe(true);
        gate.connected();
        expect(gate.isOpen()).toBe(true);

        gate.setMedia(false);
        expect(gate.media()).toBe(false);
        gate.disconnected();
        expect(gate.media()).toBe(true);
        gate.connected(); // 次の受信側は、media を送らない古い受信側
        expect(gate.isOpen()).toBe(true);
    });

    it("コールバックの中からゲートを操作しても (入れ子)、変わらなくなるまで計算し直す", () => {
        const calls: string[] = [];
        const b = ledgerTrack("b");
        let nested = false;
        const gate = createMediaGate({
            start(track) {
                calls.push(`start ${track.track.id}`);
                if (!nested) {
                    nested = true;
                    gate.trackAdded(b.entry); // 始めている最中に、別のトラックが届いた
                }
            },
            stop(trackId: string, reason: StopReason) {
                calls.push(`stop ${trackId} ${reason}`);
            },
        });
        gate.trackAdded(ledgerTrack("a").entry);
        gate.connected();
        expect(calls).toEqual(["start a", "start b"]);
        expect(gate.runningCount()).toBe(2);
    });
});

// ---- ゲートが閉じている間は、エンコーダ、黒いフレーム、アバターの取得が 0 回 --------------------

/** 呼ばれた回数だけを数える、テスト用のエンコードの関数 (encode/session.ts にそのまま渡す)。 */
function countingEncodeDeps() {
    const counts = { createEncoder: 0, makeBlackFrame: 0, isConfigSupported: 0 };
    const deps: EncodeSessionDeps = {
        async isConfigSupported() {
            counts.isConfigSupported++;
            return { supported: true };
        },
        createEncoder(init) {
            counts.createEncoder++;
            const encoder: VideoEncoderLike = {
                encodeQueueSize: 0,
                configure() {},
                encode(frame, options) {
                    const chunk: EncodedVideoChunk = {
                        type: options?.keyFrame === true ? "key" : "delta",
                        timestamp: frame.timestamp,
                        byteLength: 1,
                        copyTo: (dest: Uint8Array) => dest.set([0]),
                    };
                    init.output(chunk, {});
                },
                async flush() {},
                close() {},
            };
            return encoder;
        },
        gate: new SerialGate(),
        nowUs: () => 1_000n,
        makeBlackFrame(_w, _h, timestamp): VideoFrame {
            counts.makeBlackFrame++;
            return { timestamp, close() {} };
        },
        scaleFrame(frame): VideoFrame {
            return frame;
        },
    };
    return { deps, counts };
}

/** フレームを 1 つも返さずに close() を待つ映像のソース (製品の読み出しの代わり)。 */
function idleSource(): VideoFrameSource & { closed: boolean } {
    let waiter: ((frame: VideoFrame | null) => void) | undefined;
    const source = {
        closed: false,
        format: { width: 1920, height: 1080, fps: 60 },
        next(): Promise<VideoFrame | null> {
            if (source.closed) return Promise.resolve(null);
            return new Promise<VideoFrame | null>((resolve) => {
                waiter = resolve;
            });
        },
        close(): void {
            source.closed = true;
            waiter?.(null);
            waiter = undefined;
        },
    };
    return source;
}

async function settle(times = 200): Promise<void> {
    for (let i = 0; i < times; i++) await Promise.resolve();
}

describe("ゲートが閉じている間は、createEncoder、makeBlackFrame、アバターの取得が 0 回", () => {
    /**
     * index.ts と同じ形のつなぎ方 (ゲートのコールバックでセッションを始めたり止めたりし、録画が ON かと画質の上限を、
     * 新しいセッションに渡し、動いているセッションにも伝える)。数えるのは、エンコードの関数とアバターを取得した回数
     * である。index.ts そのものはテストしない (Vencord を import するので、vitest から読めない)。index.ts のつなぎ方が
     * このコピーと違っていても、このテストは成功するので、index.ts のつなぎ方は実機テストで確認する。
     */
    function wiredSender() {
        const { deps, counts } = countingEncodeDeps();
        const sessions = new Map<string, { session: EncodeSessionHandle; source: ReturnType<typeof idleSource> }>();
        const sourcesMade: string[] = [];
        let recordingOn = false;
        let limitsWire: { max_long_edge?: number; fps_divisor?: 1 | 2 } = {};
        let avatarFetches = 0;

        const gate = createMediaGate({
            start(track) {
                sourcesMade.push(track.track.id); // 読み出しのソースを作った
                const source = idleSource();
                const session = createEncodeSession(
                    deps,
                    {
                        streamId: sessions.size + 1,
                        source,
                        preference: "prefer-hardware",
                        limits: limitsFromWire(limitsWire),
                        latencyMode: "realtime",
                        bitrateFactor: 1,
                        warmup: true,
                        keyintSec: 2,
                    },
                    { sendVideo() {}, onStage() {} },
                );
                session.setRecording(recordingOn);
                void session.start();
                sessions.set(track.track.id, { session, source });
            },
            stop(trackId, reason) {
                const running = sessions.get(trackId);
                if (running === undefined) return;
                if (reason === "disconnected") running.session.setRecording(false);
                running.session.stop();
                sessions.delete(trackId);
            },
        });

        const refreshAvatars = (): void => {
            const fetches = avatarFetchesToStart({
                gateOpen: gate.isOpen(),
                targets: new Set(["u1", "u2"]),
                urls: new Map([
                    ["u1", "https://cdn.discordapp.com/avatars/u1/a.png"],
                    ["u2", "https://cdn.discordapp.com/avatars/u2/b.png"],
                ]),
                cached: new Map(),
                inFlight: new Set(),
            });
            avatarFetches += fetches.length;
        };

        return {
            gate,
            counts,
            sourcesMade,
            sessions,
            avatarFetches: () => avatarFetches,
            refreshAvatars,
            setRecording(on: boolean): void {
                recordingOn = on;
                for (const { session } of sessions.values()) session.setRecording(on);
            },
            setLimits(fields: { max_long_edge?: number; fps_divisor?: 1 | 2 }): void {
                limitsWire = fields;
                for (const { session } of sessions.values()) session.setLimits(limitsFromWire(fields));
            },
        };
    }

    it("確立していない間は、トラックが届いても、録画が ON になっても、上限が変わっても、1 回も増えない", async () => {
        const s = wiredSender();
        s.gate.trackAdded(ledgerTrack("screen-1").entry);
        s.gate.trackAdded(ledgerTrack("voice-1", 1, "audio").entry);
        s.setRecording(true);
        s.setLimits({ max_long_edge: 1280, fps_divisor: 2 });
        s.refreshAvatars();
        await settle();

        expect(s.counts).toEqual({ createEncoder: 0, makeBlackFrame: 0, isConfigSupported: 0 });
        expect(s.sourcesMade).toEqual([]);
        expect(s.avatarFetches()).toBe(0);
    });

    it("確立しても media が false の間は 0 回で、true になると増え、false に戻して止めた後は増えない", async () => {
        const s = wiredSender();
        s.gate.trackAdded(ledgerTrack("screen-1").entry);
        s.gate.setMedia(false); // hello_ok の初期値
        s.gate.connected();
        s.setRecording(true);
        s.refreshAvatars();
        await settle();
        expect(s.counts).toEqual({ createEncoder: 0, makeBlackFrame: 0, isConfigSupported: 0 });
        expect(s.avatarFetches()).toBe(0);

        // 開くと作る (ウォームアップの 60 枚と、録画が ON なので encoder-created の黒埋め用のフレーム)。アバターも
        // 取得する。
        s.gate.setMedia(true);
        s.refreshAvatars();
        await settle();
        expect(s.counts.createEncoder).toBe(1);
        expect(s.counts.makeBlackFrame).toBeGreaterThan(0);
        expect(s.avatarFetches()).toBe(2);

        // 閉じて止め終えたときの回数を基準に、閉じている間のイベントで 1 回も増えないことを確認する。
        s.setRecording(false);
        s.gate.setMedia(false);
        await settle();
        expect(s.sessions.size).toBe(0);
        const closedAt = { ...s.counts };
        const fetchedAt = s.avatarFetches();

        s.gate.trackAdded(ledgerTrack("screen-2").entry);
        s.setRecording(true);
        s.setLimits({ max_long_edge: 640 });
        s.refreshAvatars();
        await settle();
        expect(s.counts).toEqual(closedAt);
        expect(s.avatarFetches()).toBe(fetchedAt);
        expect(s.sourcesMade).toEqual(["screen-1"]);
    });

    it("切れて止めるときは、送り先のない黒埋め用のフレームを作らず、media が false で止めるときは作る", async () => {
        // 録画が ON になった直後 (recording-turned-on の黒埋め用のフレームを、まだ作っていないセッション) に止める。
        const disconnected = wiredSender();
        disconnected.gate.trackAdded(ledgerTrack("screen-1").entry);
        disconnected.gate.connected();
        await settle();
        const warmupOnly = disconnected.counts.makeBlackFrame;
        disconnected.setRecording(true);
        disconnected.gate.disconnected();
        await settle();
        expect(disconnected.counts.makeBlackFrame).toBe(warmupOnly); // encoder-teardown の分も recording-turned-on の分も作らない
        expect(disconnected.sessions.size).toBe(0);

        const mediaOff = wiredSender();
        mediaOff.gate.trackAdded(ledgerTrack("screen-1").entry);
        mediaOff.gate.connected();
        await settle();
        const before = mediaOff.counts.makeBlackFrame;
        mediaOff.setRecording(true);
        mediaOff.gate.setMedia(false); // 確立しているので、encoder-teardown の黒埋め用のフレームは届く
        await settle();
        expect(mediaOff.counts.makeBlackFrame).toBe(before + 60);

        // どちらも、閉じた後のイベントでは増えない。
        const settled = { ...disconnected.counts };
        disconnected.gate.trackAdded(ledgerTrack("screen-2").entry);
        disconnected.refreshAvatars();
        await settle();
        expect(disconnected.counts).toEqual(settled);
        expect(disconnected.avatarFetches()).toBe(0);
    });
});

// ---- 原因の見立ての確認 (実際の通話で、4 人の映像が同時に止まり、チェックの付け直しで戻った件) ------------------
//
// 見立て: Discord が Go Live の接続を作り直し、古い接続が閉じても古いトラックに ended が届かないと、ゲートが古い
// トラックに縛られ、同じ人の新しい接続のトラックを読み出し始めないのではないか。ゲートはトラックごとに判断し、人や
// 接続でまとめないので、そうはならないことを確認する。

describe("createMediaGate: 古い接続のトラックに ended が届かなくても、新しい接続のトラックを始める", () => {
    it("古いトラックを読み出している間に、同じ人の新しい接続 (別の pcId) のトラックが届けば、すぐに始める", () => {
        const { gate, calls } = recordingGate();
        gate.connected();
        const old = ledgerTrack("old-video", 3);
        gate.trackAdded(old.entry);
        calls.length = 0;

        // 古い接続は閉じたが、ended のイベントは届かない (track_removed も来ない)。readyState もまだ live のまま。
        const fresh = ledgerTrack("new-video", 4);
        gate.trackAdded(fresh.entry);
        expect(calls).toEqual(["start new-video"]);
        expect(gate.runningCount()).toBe(2);
    });

    it("RTCPeerConnection.close() で readyState だけが ended になった古いトラックは、次に一覧が変わったときに止める", () => {
        // WebRTC 1.0 の close() は、レシーバーのトラックの readyState を ended にするが、ended のイベントを送らない。
        const { gate, calls } = recordingGate();
        gate.connected();
        const old = ledgerTrack("old-video", 3);
        gate.trackAdded(old.entry);
        old.track.end();
        calls.length = 0;

        const fresh = ledgerTrack("new-video", 4);
        gate.trackAdded(fresh.entry);
        expect(calls).toEqual(["stop old-video track-ended", "start new-video"]);
        expect(gate.runningCount()).toBe(1);
    });
});

describe("createMediaGate: refresh", () => {
    it("readyState だけが ended になったトラックを、refresh() で止める", () => {
        const { gate, calls } = recordingGate();
        gate.connected();
        const t = ledgerTrack("v", 2);
        gate.trackAdded(t.entry);
        t.track.end();
        calls.length = 0;
        gate.refresh();
        expect(calls).toEqual(["stop v track-ended"]);
        expect(gate.runningCount()).toBe(0);
    });
});
