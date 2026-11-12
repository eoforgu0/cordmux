// SPDX-License-Identifier: Apache-2.0

// adapter/connections.ts の ConnectionTracker のテスト。voice gateway のメッセージと RTCPeerConnection のイベントを、
// 実際の通話で記録した順番で流し、どの接続がどの pcId と画面共有している人に結び付き、どのトラックがどの pcId に
// 流れるかを確認する。時計とタイマーはテストの側で進める (実際の時間を待たない)。

import { describe, expect, it, vi } from "vitest";

import type { AdapterEvent } from "../adapter/types";
import {
    ConnectionTracker,
    extractIceUfrag,
    OP2_ADJACENCY_MS,
    parseGatewayFrame,
    parseMsid,
    TRACK_HOLD_MAX_MS,
    TRACK_RECHECK_MS,
    VOICE_PC_ID,
} from "../adapter/connections";
import { initialState, reduce, userIdForSsrc } from "../core/state";

// 通話に参加している 3 人、サーバー、3 つの画面共有の server_id。id は架空の値で、実在の id は使わない。
// 記録のまま使うのは、イベントの順番と時刻だけである (ConnectionTracker は id を文字列として比べるだけで、値には
// 依存しない)。
const ALICE = "100000000000000001";
const BOB = "100000000000000002";
const CAROL = "100000000000000003";
const GUILD = "200000000000000001";
const RTC_ALICE = "300000000000000001";
const RTC_BOB = "300000000000000002";
const RTC_CAROL = "300000000000000003";

/** ログに出てはいけない文字列 (ufrag のほか)。ufrag は、テストごとに "uf-" で始める。 */
const SECRETS = ["TOKEN-SECRET", "SESSION-SECRET", "SECRET-KEY", "pwd-secret"];

interface FakePc {
    serial: number;
    remote: string | null;
    local: string | null;
}

interface FakeTrack {
    trackId: string;
    ended: boolean;
}

function sdpWith(ufrag: string): string {
    return `v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\na=ice-ufrag:${ufrag}\r\na=ice-pwd:pwd-secret-${ufrag}\r\n`;
}

function op12(userId: string, audio: number, video: number): Record<string, unknown> {
    return {
        user_id: userId,
        audio_ssrc: audio,
        video_ssrc: video,
        streams: [{ type: "screen", rid: "100", ssrc: video, rtx_ssrc: video + 1, active: true, quality: 100 }],
    };
}

function createHarness(initialCallServerId: string | null = GUILD) {
    let now = 0;
    let callServerId = initialCallServerId;
    const timers: Array<{ at: number; fn: () => void; cancelled: boolean }> = [];
    const events: AdapterEvent[] = [];
    const lines: string[] = [];
    const delivered: Array<{ trackId: string; pcId: number }> = [];
    const tracker = new ConnectionTracker({
        emit: (event) => events.push(event),
        diag: (line) => lines.push(line),
        callServerId: () => callServerId,
        now: () => now,
        schedule: (fn, ms) => {
            const timer = { at: now + ms, fn, cancelled: false };
            timers.push(timer);
            return () => {
                timer.cancelled = true;
            };
        },
    });

    /** 時計を `target` まで進め、その間に期限が来たタイマーを順に動かす。 */
    const at = (target: number): void => {
        for (;;) {
            const due = timers
                .filter((t) => !t.cancelled && t.at <= target)
                .sort((a, b) => a.at - b.at)[0];
            if (due === undefined) break;
            timers.splice(timers.indexOf(due), 1);
            now = due.at;
            due.fn();
        }
        now = Math.max(now, target);
    };

    const h = {
        tracker,
        events,
        lines,
        delivered,
        at,
        advance: (ms: number): void => at(now + ms),
        setCallServerId: (value: string | null): void => {
            callServerId = value;
        },
        wsOpen: (): number => tracker.wsOpened(),
        send: (ws: number, op: number, d: Record<string, unknown>): void => tracker.wsSent(ws, { op, d }),
        recv: (ws: number, op: number, d: Record<string, unknown>): void => tracker.wsReceived(ws, { op, d }),
        pcCreate: (): FakePc => {
            const pc: FakePc = { serial: 0, remote: null, local: null };
            pc.serial = tracker.pcCreated(() => ({ remote: pc.remote, local: pc.local }));
            return pc;
        },
        track: (pc: FakePc, kind: string, trackId: string, msid: string | null): FakeTrack => {
            const t: FakeTrack = { trackId, ended: false };
            tracker.trackArrived(pc.serial, {
                kind,
                trackId,
                msid,
                isEnded: () => t.ended,
                deliver: (pcId) => delivered.push({ trackId, pcId }),
            });
            return t;
        },
        ssrcEvents: () => events.filter((e) => e.type === "ssrc"),
        /** op0 を送る (token と session_id も入れ、ログに出ないことを確認できるようにする)。 */
        identify: (ws: number, serverId: string): void =>
            tracker.wsSent(ws, {
                op: 0,
                d: {
                    server_id: serverId,
                    user_id: "999999999999999999",
                    session_id: "SESSION-SECRET",
                    token: "TOKEN-SECRET",
                    video: true,
                    streams: [{ type: "screen", rid: "100", quality: 100 }],
                },
            }),
        /** op7 (RESUME) を送る (token と session_id も入れ、ログに出ないことを確認できるようにする)。 */
        resume: (ws: number, serverId: string): void =>
            tracker.wsSent(ws, {
                op: 7,
                d: { server_id: serverId, session_id: "SESSION-SECRET", token: "TOKEN-SECRET", seq_ack: 5 },
            }),
        /** op1 を送る。ローカルの SDP は、送る前に pc の localDescription に入っている。`ufrag` が null なら SDP を入れない。 */
        selectProtocol: (ws: number, pc: FakePc, ufrag: string | null): void => {
            if (ufrag === null) {
                tracker.wsSent(ws, { op: 1, d: { protocol: "webrtc" } });
                return;
            }
            pc.local = sdpWith(ufrag);
            tracker.wsSent(ws, { op: 1, d: { protocol: "webrtc", data: pc.local, sdp: pc.local } });
        },
        /** op4 を受ける。リモートの SDP は、受けた後に pc の remoteDescription に入る。`ufrag` が null なら SDP を入れない。 */
        sessionDescription: (ws: number, pc: FakePc, ufrag: string | null): void => {
            const sdp = ufrag === null ? undefined : sdpWith(ufrag);
            tracker.wsReceived(ws, { op: 4, d: { sdp, secret_key: "SECRET-KEY", audio_codec: "opus", video_codec: "H264" } });
            if (sdp !== undefined) pc.remote = sdp;
        },
    };
    return h;
}

type Harness = ReturnType<typeof createHarness>;

/** よくある順番 (op0 -> op8 -> op2 -> pc -> op1 -> op4) で、接続を 1 つ作る。 */
function connect(h: Harness, serverId: string, ufrag: string): { ws: number; pc: FakePc } {
    const ws = h.wsOpen();
    h.identify(ws, serverId);
    h.recv(ws, 8, { heartbeat_interval: 13750 });
    h.recv(ws, 2, { ssrc: 1 });
    const pc = h.pcCreate();
    h.selectProtocol(ws, pc, `uf-${ufrag}-local`);
    h.sessionDescription(ws, pc, `uf-${ufrag}-remote`);
    return { ws, pc };
}

type ReplayMode = "ufrag" | "shared-remote-ufrag" | "no-sdp";

/**
 * 実際の通話で記録した順番 (時刻は、記録の時刻から同じ値を引いたミリ秒)。ボイス接続の gateway が最初の視聴の途中で
 * できる、2 つの Go Live の gateway が 1 ms 違いでできる、別々の接続の pc を作る順番が入り組む、という順番である。
 * mode: "ufrag" は接続ごとに別の ufrag、"shared-remote-ufrag" は 3 つの Go Live のリモート側の ufrag が同じ
 * (ローカル側で見分ける)、"no-sdp" は SDP がどこにもない (op2 との近さだけで結び付ける)。
 */
function replayDiag7(mode: ReplayMode) {
    const h = createHarness(GUILD);
    const local = (name: string): string | null => (mode === "no-sdp" ? null : `uf-${name}-local`);
    const remote = (name: string): string | null => {
        if (mode === "no-sdp") return null;
        if (mode === "shared-remote-ufrag" && name !== "call") return "uf-shared-remote";
        return `uf-${name}-remote`;
    };

    h.at(580); const ws1 = h.wsOpen();
    h.at(622); h.identify(ws1, GUILD);
    h.at(623); h.recv(ws1, 8, { heartbeat_interval: 13750 });
    h.at(635); h.recv(ws1, 2, { ssrc: 1 });
    h.at(641); const pc1 = h.pcCreate();
    h.at(651); h.recv(ws1, 5, { user_id: ALICE, ssrc: 93, speaking: 1 });
    h.at(666); h.selectProtocol(ws1, pc1, local("call"));
    h.at(722); h.sessionDescription(ws1, pc1, remote("call"));
    h.at(814); h.track(pc1, "audio", "t-alice-mic", `${ALICE}-93`);

    h.at(1180); const ws2 = h.wsOpen();
    h.at(1181); const ws3 = h.wsOpen();
    h.at(1228); h.identify(ws3, RTC_ALICE);
    h.at(1230); h.recv(ws3, 8, {});
    h.at(1235); h.recv(ws3, 2, {});
    h.at(1238); const pc2 = h.pcCreate();
    h.at(1243); h.recv(ws3, 5, { user_id: ALICE, ssrc: 3388, speaking: 2 });
    h.at(1245); h.recv(ws3, 12, op12(ALICE, 3388, 3389));
    h.at(1248); h.selectProtocol(ws3, pc2, local("alice"));
    h.at(1261); h.sessionDescription(ws3, pc2, remote("alice"));
    h.at(1303); h.identify(ws2, RTC_BOB);
    h.at(1305); h.recv(ws3, 12, op12(ALICE, 3388, 3389));
    h.at(1319); h.recv(ws2, 8, {});
    h.at(1327); h.recv(ws2, 2, {});
    h.at(1330); const pc3 = h.pcCreate();
    h.at(1335); h.recv(ws2, 5, { user_id: BOB, ssrc: 88648, speaking: 1 });
    h.at(1336); h.recv(ws2, 12, op12(BOB, 88648, 88649));
    h.at(1351); h.track(pc2, "video", "t-alice-video", `${ALICE}-3389`);
    h.at(1352); h.track(pc2, "audio", "t-alice-audio", `${ALICE}-3388`);
    h.at(1357); h.selectProtocol(ws2, pc3, local("bob"));
    h.at(1369); h.sessionDescription(ws2, pc3, remote("bob"));
    h.at(1414); h.track(pc3, "video", "t-bob-video", `${BOB}-88649`);
    h.at(1415); h.track(pc3, "audio", "t-bob-audio", `${BOB}-88648`);

    h.at(2080); const ws4 = h.wsOpen();
    h.at(2120); h.identify(ws4, RTC_CAROL);
    h.at(2121); h.recv(ws4, 8, {});
    h.at(2126); h.recv(ws4, 2, {});
    h.at(2129); const pc4 = h.pcCreate();
    h.at(2135); h.recv(ws4, 12, op12(CAROL, 92, 93));
    h.at(2140); h.selectProtocol(ws4, pc4, local("carol"));
    h.at(2148); h.sessionDescription(ws4, pc4, remote("carol"));
    h.at(2247); h.track(pc4, "video", "t-carol-video", `${CAROL}-93`);
    h.at(2248); h.track(pc4, "audio", "t-carol-audio", `${CAROL}-92`);
    return h;
}

const DIAG7_DELIVERED = [
    { trackId: "t-alice-mic", pcId: VOICE_PC_ID },
    { trackId: "t-alice-video", pcId: 2 },
    { trackId: "t-alice-audio", pcId: 2 },
    { trackId: "t-bob-video", pcId: 3 },
    { trackId: "t-bob-audio", pcId: 3 },
    { trackId: "t-carol-video", pcId: 4 },
    { trackId: "t-carol-audio", pcId: 4 },
];

const DIAG7_SSRC_EVENTS: AdapterEvent[] = [
    { type: "ssrc", userId: ALICE, videoSsrc: null, audioSsrc: 93, pcId: VOICE_PC_ID },
    { type: "ssrc", userId: ALICE, videoSsrc: null, audioSsrc: 3388, pcId: 2 },
    { type: "ssrc", userId: ALICE, videoSsrc: 3389, audioSsrc: 3388, pcId: 2 },
    { type: "ssrc", userId: ALICE, videoSsrc: 3389, audioSsrc: 3388, pcId: 2 },
    { type: "ssrc", userId: BOB, videoSsrc: null, audioSsrc: 88648, pcId: 3 },
    { type: "ssrc", userId: BOB, videoSsrc: 88649, audioSsrc: 88648, pcId: 3 },
    { type: "ssrc", userId: CAROL, videoSsrc: 93, audioSsrc: 92, pcId: 4 },
];

function expectNoSecretsLogged(lines: readonly string[]): void {
    const text = lines.join("\n");
    expect(text).not.toContain("uf-");
    for (const secret of SECRETS) expect(text).not.toContain(secret);
}

describe("実際の通話で記録した接続の順番を流し直す", () => {
    it("ボイス接続は pcId 1 に、3 つの Go Live はそれぞれ画面共有している人の pcId に結び付く (ufrag で対にする)", () => {
        const h = replayDiag7("ufrag");
        expect(h.delivered).toEqual(DIAG7_DELIVERED);
        expect(h.tracker.ownerOf(VOICE_PC_ID)).toBeUndefined();
        expect(h.tracker.ownerOf(2)).toBe(ALICE);
        expect(h.tracker.ownerOf(3)).toBe(BOB);
        expect(h.tracker.ownerOf(4)).toBe(CAROL);
        expect(h.tracker.pcIdForUser(ALICE)).toBe(2);
        expect(h.tracker.pcIdForUser(BOB)).toBe(3);
        expect(h.tracker.pcIdForUser(CAROL)).toBe(4);
        for (const pc of [1, 2, 3, 4]) {
            expect(h.lines.some((l) => l.startsWith(`pc paired: pc=${pc} `) && l.includes("route=remote-ufrag"))).toBe(true);
        }
    });

    it("SSRC 93 を 2 人が使っていても (alice のマイクと carol の画面共有の映像)、接続ごとに別の事実として流れる", () => {
        const h = replayDiag7("ufrag");
        expect(h.ssrcEvents()).toEqual(DIAG7_SSRC_EVENTS);
        // ボイス接続の alice のマイク (93) と、carol の画面共有の映像 (93) は、別の pcId に載る。
        expect(h.delivered.find((d) => d.trackId === "t-alice-mic")?.pcId).toBe(VOICE_PC_ID);
        expect(h.delivered.find((d) => d.trackId === "t-carol-video")?.pcId).toBe(4);
    });

    it("Go Live のリモート側の ufrag が同じでも、ローカル側の ufrag で見分けて結び付ける", () => {
        const h = replayDiag7("shared-remote-ufrag");
        expect(h.delivered).toEqual(DIAG7_DELIVERED);
        expect(h.ssrcEvents()).toEqual(DIAG7_SSRC_EVENTS);
        expect(h.lines.some((l) => l.startsWith("pc paired: pc=3 ") && l.includes("route=local-ufrag"))).toBe(true);
        expect(h.lines.some((l) => l.startsWith("pc paired: pc=4 ") && l.includes("route=local-ufrag"))).toBe(true);
    });

    it("SDP がどこにもなければ、最後の手段として op2 の直後に作られた pc を結び付ける (作る順番が入り組んでいても 1 つに絞れる)", () => {
        const h = replayDiag7("no-sdp");
        expect(h.delivered).toEqual(DIAG7_DELIVERED);
        for (const pc of [1, 2, 3, 4]) {
            expect(h.lines.some((l) => l.startsWith(`pc paired: pc=${pc} `) && l.includes("route=op2-adjacency"))).toBe(true);
        }
    });

    it("ログに ICE の ufrag、token、session_id、op4 の secret_key を出さず、op0 の行には streams の type を出す", () => {
        const h = replayDiag7("ufrag");
        expectNoSecretsLogged(h.lines);
        expect(h.lines).toContain(`ws send op0: ws=3 server_id=${RTC_ALICE} video=true streams=1 types=screen`);
        expect(h.lines.some((l) => l.startsWith("ws classified: ws=1 role=call pcId=1 "))).toBe(true);
        expect(h.lines.some((l) => l.startsWith("ws classified: ws=3 role=golive pcId=2 "))).toBe(true);
    });
});

describe("同じ人の新しい接続 (op0 で始め直す) と、gateway が閉じたとき", () => {
    it("同じ人の新しい gateway と pc は新しい pcId になり、古い gateway が閉じると、古い pcId の ownerOf() は undefined になる", () => {
        const h = createHarness();
        connect(h, GUILD, "call");
        const old = connect(h, RTC_ALICE, "old");
        h.recv(old.ws, 12, op12(BOB, 10, 11));
        h.track(old.pc, "video", "v-old", `${BOB}-11`);
        expect(h.delivered).toEqual([{ trackId: "v-old", pcId: 2 }]);

        h.advance(1000);
        const next = connect(h, RTC_ALICE, "next");
        h.recv(next.ws, 12, op12(BOB, 20, 21));
        // 両方開いている間は新しいほうを答える。番号は使い回さない。
        expect(h.tracker.pcIdForUser(BOB)).toBe(3);
        h.track(next.pc, "video", "v-next", `${BOB}-21`);
        h.track(next.pc, "audio", "a-next", `${BOB}-20`);

        h.tracker.wsClosed(old.ws);
        expect(h.tracker.ownerOf(2)).toBeUndefined();
        expect(h.tracker.ownerOf(3)).toBe(BOB);
        expect(h.tracker.pcIdForUser(BOB)).toBe(3);

        // 閉じた gateway の pc に遅れて届いたトラックは流さない。
        h.track(old.pc, "audio", "a-old-late", `${BOB}-10`);
        h.advance(TRACK_HOLD_MAX_MS);
        expect(h.delivered).toEqual([
            { trackId: "v-old", pcId: 2 },
            { trackId: "v-next", pcId: 3 },
            { trackId: "a-next", pcId: 3 },
        ]);
        expect(h.lines.some((l) => l.startsWith("track dropped:") && l.includes("trackId=a-old-late"))).toBe(true);
        expect(h.ssrcEvents().filter((e) => e.type === "ssrc" && e.pcId === 3)).toEqual([
            { type: "ssrc", userId: BOB, videoSsrc: 21, audioSsrc: 20, pcId: 3 },
        ]);
    });

    it("トラックが 1 つも届かないうちにボイス接続の gateway が閉じても、その pc に後から届く音声は pcId 1 で流す", () => {
        const h = createHarness();
        const call = connect(h, GUILD, "call");
        h.tracker.wsClosed(call.ws);
        h.advance(1000);
        h.track(call.pc, "audio", "a-later", `${CAROL}-77`);
        expect(h.delivered).toEqual([{ trackId: "a-later", pcId: VOICE_PC_ID }]);
        expect(h.lines.some((l) => l.startsWith(`pc paired: pc=${call.pc.serial} ws=${call.ws} route=remote-ufrag role=call`))).toBe(true);
    });

    it("ボイス接続の gateway が閉じても、その pc に後から届く音声は pcId 1 のまま流す", () => {
        const h = createHarness();
        const call = connect(h, GUILD, "call");
        h.track(call.pc, "audio", "a1", `${ALICE}-93`);
        h.tracker.wsClosed(call.ws);
        h.track(call.pc, "audio", "a2", `${CAROL}-77`);
        expect(h.delivered).toEqual([
            { trackId: "a1", pcId: VOICE_PC_ID },
            { trackId: "a2", pcId: VOICE_PC_ID },
        ]);
    });
});

describe("Go Live の gateway の再接続 (op7)", () => {
    it("再接続した gateway は同じ pcId と画面共有している人を受け継ぎ、動き続ける pc に後から届くトラックも同じ pcId で流す", () => {
        const h = createHarness();
        connect(h, GUILD, "call");
        const g = connect(h, RTC_CAROL, "g");
        h.recv(g.ws, 12, op12(CAROL, 10, 11));
        h.track(g.pc, "video", "v1", `${CAROL}-11`);
        expect(h.delivered).toEqual([{ trackId: "v1", pcId: 2 }]);

        h.tracker.wsClosed(g.ws);
        // 閉じている間は、画面共有している人を答えない。その間に届いたトラックは、再接続を待って保留する。
        expect(h.tracker.ownerOf(2)).toBeUndefined();
        expect(h.tracker.pcIdForUser(CAROL)).toBe(0);
        h.track(g.pc, "audio", "a-gap", `${CAROL}-10`);
        expect(h.lines).toContain(`track held: pc=${g.pc.serial} trackId=a-gap reason=ws-closed`);

        h.advance(1500);
        const g2 = h.wsOpen();
        h.resume(g2, RTC_CAROL);
        expect(h.lines.some((l) => l.startsWith(`ws resumed: ws=${g2} from_ws=${g.ws} role=golive pcId=2 pcs=1 `))).toBe(true);
        expect(h.tracker.ownerOf(2)).toBe(CAROL);
        expect(h.tracker.pcIdForUser(CAROL)).toBe(2);
        h.recv(g2, 12, op12(CAROL, 10, 11));
        h.track(g.pc, "audio", "a-late", `${CAROL}-10`);

        expect(h.delivered).toEqual([
            { trackId: "v1", pcId: 2 },
            { trackId: "a-gap", pcId: 2 },
            { trackId: "a-late", pcId: 2 },
        ]);
        // 再接続の後の事実も、同じ pcId で流れる (新しい番号は作られない)。
        expect(h.ssrcEvents()).toEqual([
            { type: "ssrc", userId: CAROL, videoSsrc: 11, audioSsrc: 10, pcId: 2 },
            { type: "ssrc", userId: CAROL, videoSsrc: 11, audioSsrc: 10, pcId: 2 },
        ]);
        expect(h.lines.some((l) => l.includes("pcId=3"))).toBe(false);
        expectNoSecretsLogged(h.lines);
    });

    it("古い gateway の close が op7 より遅れて届いても、再接続として同じ pcId と画面共有している人を受け継ぐ", () => {
        const h = createHarness();
        connect(h, GUILD, "call");
        const g = connect(h, RTC_CAROL, "g");
        h.recv(g.ws, 12, op12(CAROL, 10, 11));
        h.track(g.pc, "video", "v1", `${CAROL}-11`);

        // Discord が、応答のなくなった gateway を自分で閉じ、close が届く前に新しい gateway で op7 を送る。
        const g2 = h.wsOpen();
        h.resume(g2, RTC_CAROL);
        expect(h.lines.some((l) => l.startsWith(`ws resumed: ws=${g2} from_ws=${g.ws} role=golive pcId=2 pcs=1 `))).toBe(true);
        // 遅れて届いた古い gateway の close とメッセージでは、何もしない。
        h.tracker.wsClosed(g.ws);
        h.recv(g.ws, 12, op12(ALICE, 3388, 3389));
        expect(h.lines.some((l) => l.startsWith(`ws closed: ws=${g.ws} `))).toBe(false);
        expect(h.tracker.ownerOf(2)).toBe(CAROL);
        expect(h.tracker.pcIdForUser(CAROL)).toBe(2);
        h.track(g.pc, "audio", "a-late", `${CAROL}-10`);

        expect(h.delivered).toEqual([
            { trackId: "v1", pcId: 2 },
            { trackId: "a-late", pcId: 2 },
        ]);
        expect(h.ssrcEvents()).toEqual([{ type: "ssrc", userId: CAROL, videoSsrc: 11, audioSsrc: 10, pcId: 2 }]);
    });

    it("5 秒以内に再接続しなければ、待っていたトラックはログに書いて捨てる", () => {
        const h = createHarness();
        const g = connect(h, RTC_CAROL, "g");
        h.recv(g.ws, 12, op12(CAROL, 10, 11));
        h.tracker.wsClosed(g.ws);
        h.track(g.pc, "video", "v", `${CAROL}-11`);
        h.advance(TRACK_HOLD_MAX_MS);
        expect(h.delivered).toEqual([]);
        expect(h.lines).toContain(`track dropped: pc=${g.pc.serial} trackId=v reason=ws-closed held_ms=${TRACK_HOLD_MAX_MS}`);
    });

    it("手放した (forget) 接続は、再接続しても手放したままにする", () => {
        const h = createHarness();
        const g = connect(h, RTC_CAROL, "g");
        h.recv(g.ws, 12, op12(CAROL, 10, 11));
        h.tracker.wsClosed(g.ws);
        h.tracker.forget(2);
        const g2 = h.wsOpen();
        h.resume(g2, RTC_CAROL);
        h.recv(g2, 12, op12(CAROL, 10, 11));
        h.track(g.pc, "video", "v", `${CAROL}-11`);
        expect(h.tracker.ownerOf(2)).toBeUndefined();
        expect(h.delivered).toEqual([]);
        expect(h.ssrcEvents()).toHaveLength(1);
    });

    it("再接続の元がない op7 は op0 と同じに仕分ける (server_id が違う閉じた gateway は元にしない)", () => {
        const h = createHarness();
        const g = connect(h, RTC_ALICE, "g");
        h.recv(g.ws, 12, op12(ALICE, 10, 11));
        h.tracker.wsClosed(g.ws);
        const other = h.wsOpen();
        h.resume(other, RTC_BOB);
        expect(h.lines.some((l) => l.startsWith(`ws classified: ws=${other} role=golive pcId=3 `))).toBe(true);
        const call = h.wsOpen();
        h.resume(call, GUILD);
        expect(h.lines.some((l) => l.startsWith(`ws classified: ws=${call} role=call pcId=1 `))).toBe(true);
    });
});

describe("ボイス接続が作り直されたとき (通話のセッション)", () => {
    const A = ALICE;
    const B = CAROL;

    /** 届いた ssrc の事実を core に通し、pcId 1 の SSRC から userId を特定できる状態を返す。 */
    function coreAfter(events: readonly AdapterEvent[]) {
        let state = reduce(initialState, { type: "roster", users: [{ userId: A }, { userId: B }] });
        for (const event of events) state = reduce(state, event);
        return state;
    }

    it("前のセッションの pcId 1 の SSRC の事実で、新しいセッションのトラックの userId を特定しない (サーバーが替わって番号が重なっても)", () => {
        const h = createHarness();
        const s1 = connect(h, GUILD, "s1");
        h.recv(s1.ws, 5, { user_id: A, ssrc: 93, speaking: 1 });
        h.recv(s1.ws, 5, { user_id: B, ssrc: 94, speaking: 1 });
        h.track(s1.pc, "audio", "a-s1", `${A}-93`);

        // ボイス接続が別のメディアサーバーで作り直され、B に 93 が割り当てられる。
        h.advance(1000);
        const s2 = connect(h, GUILD, "s2");
        expect(h.lines).toContain(`call session changed: ws=${s2.ws} previous_ws=${s1.ws} cleared=2`);
        h.recv(s2.ws, 5, { user_id: B, ssrc: 93, speaking: 1 });
        h.track(s2.pc, "audio", "b-s2", `${B}-93`);

        expect(h.delivered).toEqual([
            { trackId: "a-s1", pcId: VOICE_PC_ID },
            { trackId: "b-s2", pcId: VOICE_PC_ID },
        ]);
        expect(h.ssrcEvents()).toEqual([
            { type: "ssrc", userId: A, videoSsrc: null, audioSsrc: 93, pcId: VOICE_PC_ID },
            { type: "ssrc", userId: B, videoSsrc: null, audioSsrc: 94, pcId: VOICE_PC_ID },
            { type: "ssrc", userId: A, videoSsrc: null, audioSsrc: null, pcId: VOICE_PC_ID },
            { type: "ssrc", userId: B, videoSsrc: null, audioSsrc: null, pcId: VOICE_PC_ID },
            { type: "ssrc", userId: B, videoSsrc: null, audioSsrc: 93, pcId: VOICE_PC_ID },
        ]);
        const state = coreAfter(h.events);
        // SSRC 93 は、roster の先頭にいる A の古い値ではなく、新しいセッションの B と特定する。B が前のセッションで
        // 使っていた 94 も残らない。
        expect(userIdForSsrc(state, VOICE_PC_ID, 93)).toBe(B);
        expect(userIdForSsrc(state, VOICE_PC_ID, 94)).toBeUndefined();
    });

    it("前のセッションの gateway の op5 と、前のセッションの pc に遅れて届いたトラックは流さない", () => {
        const h = createHarness();
        const s1 = connect(h, GUILD, "s1");
        h.recv(s1.ws, 5, { user_id: A, ssrc: 93, speaking: 1 });
        h.advance(1000);
        const s2 = connect(h, GUILD, "s2");
        h.recv(s2.ws, 5, { user_id: B, ssrc: 93, speaking: 1 });
        const before = h.ssrcEvents().length;

        // 古い gateway がまだ開いていて op5 を受けても、新しいセッションの行を上書きしない。
        h.recv(s1.ws, 5, { user_id: A, ssrc: 95, speaking: 1 });
        expect(h.ssrcEvents()).toHaveLength(before);
        expect(h.lines).toContain(`ssrc not emitted: ws=${s1.ws} pcId=${VOICE_PC_ID} reason=superseded-call`);
        h.track(s1.pc, "audio", "a-s1-late", `${A}-95`);
        expect(h.delivered).toEqual([]);
        expect(h.lines).toContain(`track dropped: pc=${s1.pc.serial} pcId=${VOICE_PC_ID} trackId=a-s1-late reason=superseded-call`);
        expect(userIdForSsrc(coreAfter(h.events), VOICE_PC_ID, 93)).toBe(B);
    });

    it("ボイス接続の gateway を op7 で再接続したときは同じセッションとして、事実を消さず、同じ pc のトラックを pcId 1 で流す", () => {
        const h = createHarness();
        const call = connect(h, GUILD, "call");
        h.recv(call.ws, 5, { user_id: A, ssrc: 93, speaking: 1 });
        h.tracker.wsClosed(call.ws);
        h.advance(500);
        const ws2 = h.wsOpen();
        h.resume(ws2, GUILD);
        h.recv(ws2, 5, { user_id: B, ssrc: 94, speaking: 1 });
        h.track(call.pc, "audio", "b", `${B}-94`);

        expect(h.lines.some((l) => l.startsWith(`ws resumed: ws=${ws2} from_ws=${call.ws} role=call pcId=1 `))).toBe(true);
        expect(h.lines.some((l) => l.startsWith("call session changed:"))).toBe(false);
        expect(h.ssrcEvents()).toEqual([
            { type: "ssrc", userId: A, videoSsrc: null, audioSsrc: 93, pcId: VOICE_PC_ID },
            { type: "ssrc", userId: B, videoSsrc: null, audioSsrc: 94, pcId: VOICE_PC_ID },
        ]);
        expect(h.delivered).toEqual([{ trackId: "b", pcId: VOICE_PC_ID }]);
        const state = coreAfter(h.events);
        expect(userIdForSsrc(state, VOICE_PC_ID, 93)).toBe(A);
        expect(userIdForSsrc(state, VOICE_PC_ID, 94)).toBe(B);
        expectNoSecretsLogged(h.lines);
    });

    it("仕分けが遅れた古いボイス接続の gateway は、新しいセッションを古いセッションに戻さない", () => {
        const h = createHarness(null);
        const late = h.wsOpen();
        h.identify(late, GUILD); // ボイス接続の server id が読めないので、仕分けを保留する。
        h.setCallServerId(GUILD);
        const current = connect(h, GUILD, "current");
        h.recv(current.ws, 5, { user_id: B, ssrc: 93, speaking: 1 });
        h.recv(late, 5, { user_id: A, ssrc: 93, speaking: 1 }); // ここで古い gateway が仕分けられる。
        expect(h.lines).toContain(`call session superseded: ws=${late} current_ws=${current.ws}`);
        expect(h.ssrcEvents()).toEqual([{ type: "ssrc", userId: B, videoSsrc: null, audioSsrc: 93, pcId: VOICE_PC_ID }]);
    });
});

describe("閉じた gateway の記録を捨てる前に対を決めるときは、ufrag だけを使う", () => {
    it("op2 から 50 ms より後に作られた pc は、SDP がないうちに閉じた gateway の記録が捨てられても、後から自分の gateway と ufrag で結び付く", () => {
        const h = createHarness();
        h.at(100);
        const x = h.wsOpen();
        h.identify(x, RTC_ALICE);
        h.recv(x, 2, {});
        h.recv(x, 12, op12(ALICE, 10, 11));
        h.at(140);
        const y = h.wsOpen();
        h.identify(y, RTC_BOB);
        h.recv(y, 2, {});
        h.recv(y, 12, op12(BOB, 20, 21));
        h.at(170);
        const pcX = h.pcCreate();
        expect(170 - 100).toBeGreaterThan(OP2_ADJACENCY_MS);
        // 関係のない gateway がたくさん閉じ、閉じた gateway の記録が上限を超えて、古いものから捨てられる (捨てる前に
        // 対を決めようとする)。
        h.at(171);
        for (let i = 0; i < 17; i++) h.tracker.wsClosed(h.wsOpen());
        expect(h.lines.some((l) => l.startsWith("ws record dropped:"))).toBe(true);
        expect(h.lines.some((l) => l.startsWith(`pc paired: pc=${pcX.serial} `))).toBe(false);

        h.at(180);
        h.selectProtocol(x, pcX, "uf-x-local");
        h.sessionDescription(x, pcX, "uf-x-remote");
        h.track(pcX, "video", "v-x", `${ALICE}-11`);
        expect(h.lines).toContain(`pc paired: pc=${pcX.serial} ws=${x} route=remote-ufrag role=golive pcId=2`);
        expect(h.delivered).toEqual([{ trackId: "v-x", pcId: 2 }]);
        expect(h.lines.some((l) => l.includes("route=op2-adjacency"))).toBe(false);
    });
});

describe("pc の close", () => {
    it("閉じた pc の、保留していたトラックを捨て、その後に届くトラックも流さない", () => {
        const h = createHarness();
        const g = connect(h, RTC_CAROL, "g");
        h.track(g.pc, "video", "v", `${CAROL}-11`); // 画面共有している人がまだ分からないので、保留する。
        h.tracker.pcClosed(g.pc.serial);
        expect(h.lines).toContain(`pc closed: pc=${g.pc.serial} held_dropped=1`);
        h.tracker.pcClosed(g.pc.serial); // 2 回目は何もしない。
        h.recv(g.ws, 12, op12(CAROL, 10, 11));
        h.track(g.pc, "audio", "a", `${CAROL}-10`);
        expect(h.delivered).toEqual([]);
        expect(h.lines).toContain(`track dropped: pc=${g.pc.serial} trackId=a reason=unknown-pc`);
    });
});

describe("時計", () => {
    it("既定の時計はシステム時刻ではない (Date.now が大きく進んでも、保留しているトラックを早く捨てない)", () => {
        const timers: Array<() => void> = [];
        const delivered: number[] = [];
        const tracker = new ConnectionTracker({
            emit: () => {},
            diag: () => {},
            callServerId: () => GUILD,
            schedule: (fn) => {
                timers.push(fn);
                return () => {};
            },
        });
        const ws = tracker.wsOpened();
        tracker.wsSent(ws, { op: 0, d: { server_id: RTC_CAROL } });
        tracker.wsReceived(ws, { op: 2, d: {} });
        const local = sdpWith("uf-clock-local");
        const remote = sdpWith("uf-clock-remote");
        const pc = tracker.pcCreated(() => ({ remote, local }));
        tracker.wsSent(ws, { op: 1, d: { sdp: local } });
        tracker.wsReceived(ws, { op: 4, d: { sdp: remote } });
        tracker.trackArrived(pc, {
            kind: "video",
            trackId: "v",
            msid: `${CAROL}-11`,
            isEnded: () => false,
            deliver: (pcId) => delivered.push(pcId),
        });
        const spy = vi.spyOn(Date, "now").mockReturnValue(Date.now() + 60 * 60 * 1000);
        try {
            timers.shift()?.();
            tracker.wsReceived(ws, { op: 12, d: op12(CAROL, 10, 11) });
        } finally {
            spy.mockRestore();
            tracker.dispose();
        }
        expect(delivered).toEqual([2]);
    });
});

describe("リモート側とローカル側の ufrag が食い違えば結び付けない", () => {
    it("リモート側の ufrag が gateway X を、ローカル側の ufrag が gateway Y を指す pc のトラックは流さず、ログに書いて 5 秒後に捨てる", () => {
        const h = createHarness();
        const x = connect(h, RTC_ALICE, "x");
        h.recv(x.ws, 12, op12(ALICE, 10, 11));
        const y = connect(h, RTC_BOB, "y");
        h.recv(y.ws, 12, op12(BOB, 20, 21));
        h.advance(1000);
        const p = h.pcCreate();
        p.remote = sdpWith("uf-x-remote");
        p.local = sdpWith("uf-y-local");
        h.track(p, "video", "v-p", `${ALICE}-11`);
        expect(h.delivered).toEqual([]);
        expect(h.lines.some((l) => l === `pc not paired: pc=${p.serial} reason=conflict`)).toBe(true);
        h.advance(TRACK_HOLD_MAX_MS);
        expect(h.delivered).toEqual([]);
        expect(h.lines.some((l) => l.startsWith(`track dropped: pc=${p.serial} trackId=v-p reason=unpaired`))).toBe(true);
        expectNoSecretsLogged(h.lines);
    });

    it("片方の ufrag だけが一致しても、その gateway のローカル側の ufrag が pc のものと違えば結び付けない", () => {
        const h = createHarness();
        const x = connect(h, RTC_ALICE, "x");
        h.recv(x.ws, 12, op12(ALICE, 10, 11));
        h.advance(1000);
        const p = h.pcCreate();
        p.remote = sdpWith("uf-x-remote");
        p.local = sdpWith("uf-nobody-local");
        h.track(p, "video", "v-p", `${ALICE}-11`);
        expect(h.lines.some((l) => l === `pc not paired: pc=${p.serial} reason=conflict`)).toBe(true);
        h.advance(TRACK_HOLD_MAX_MS);
        expect(h.delivered).toEqual([]);
    });
});

describe("msid の userId と、接続で画面共有している人の照合", () => {
    it("Go Live の接続に、画面共有している人と違う人の msid のトラックが届いたら流さない", () => {
        const h = createHarness();
        connect(h, GUILD, "call");
        const g = connect(h, RTC_CAROL, "g");
        h.recv(g.ws, 12, op12(CAROL, 92, 93));
        h.track(g.pc, "video", "v-wrong", `${ALICE}-93`);
        h.track(g.pc, "video", "v-right", `${CAROL}-93`);
        expect(h.delivered).toEqual([{ trackId: "v-right", pcId: 2 }]);
        expect(h.lines).toContain(
            `track dropped: pc=${g.pc.serial} pcId=2 trackId=v-wrong reason=msid-owner-mismatch msid_user=${ALICE} owner=${CAROL}`,
        );
    });

    it("msid が決まった形でなければ照合せず、画面共有している人の接続として流す", () => {
        const h = createHarness();
        const g = connect(h, RTC_CAROL, "g");
        h.recv(g.ws, 12, op12(CAROL, 92, 93));
        h.track(g.pc, "video", "v-plain", "default");
        h.track(g.pc, "audio", "a-none", null);
        expect(h.delivered).toEqual([
            { trackId: "v-plain", pcId: 2 },
            { trackId: "a-none", pcId: 2 },
        ]);
    });

    it("op12 の user_id が 2 人に分かれた接続は、画面共有している人が分からないので、トラックも ssrc も流さない", () => {
        const h = createHarness();
        const g = connect(h, RTC_CAROL, "g");
        h.recv(g.ws, 12, op12(CAROL, 92, 93));
        h.recv(g.ws, 12, op12(ALICE, 3388, 3389));
        h.track(g.pc, "video", "v", `${CAROL}-93`);
        h.advance(TRACK_HOLD_MAX_MS);
        expect(h.delivered).toEqual([]);
        expect(h.tracker.ownerOf(2)).toBeUndefined();
        expect(h.ssrcEvents()).toEqual([{ type: "ssrc", userId: CAROL, videoSsrc: 93, audioSsrc: 92, pcId: 2 }]);
        expect(h.lines.some((l) => l.startsWith(`track dropped: pc=${g.pc.serial} trackId=v reason=owner-unknown`))).toBe(true);
    });
});

describe("行き先が決まらないトラックの保留", () => {
    it("対のない pc のトラックは 250 ms ごとに再確認、5 秒たったらログに書いて捨てる", () => {
        const h = createHarness();
        connect(h, GUILD, "call");
        h.advance(1000); // どの gateway の op2 からも 50 ms より後に作られる pc。
        const p = h.pcCreate();
        p.remote = sdpWith("uf-unknown-remote");
        h.track(p, "video", "v", null);
        expect(h.lines).toContain(`track held: pc=${p.serial} trackId=v reason=unpaired`);
        h.advance(TRACK_HOLD_MAX_MS - 1);
        expect(h.delivered).toEqual([]);
        expect(h.lines.some((l) => l.startsWith("track dropped:"))).toBe(false);
        h.advance(1);
        expect(h.lines).toContain(`track dropped: pc=${p.serial} trackId=v reason=unpaired held_ms=${TRACK_HOLD_MAX_MS}`);
        h.advance(TRACK_HOLD_MAX_MS);
        expect(h.delivered).toEqual([]);
    });

    it("画面共有している人が分かる前に届いたトラックは保留し、op12 が届いたときに流す", () => {
        const h = createHarness();
        const g = connect(h, RTC_BOB, "g");
        h.track(g.pc, "video", "v", `${BOB}-88649`);
        expect(h.delivered).toEqual([]);
        expect(h.lines).toContain(`track held: pc=${g.pc.serial} trackId=v reason=owner-unknown`);
        h.advance(300);
        h.recv(g.ws, 12, op12(BOB, 88648, 88649));
        expect(h.delivered).toEqual([{ trackId: "v", pcId: 2 }]);
        expect(h.ssrcEvents()).toEqual([{ type: "ssrc", userId: BOB, videoSsrc: 88649, audioSsrc: 88648, pcId: 2 }]);
    });

    it("op12 がまだなければ、その接続の op5 の user_id を画面共有している人とする", () => {
        const h = createHarness();
        const g = connect(h, RTC_BOB, "g");
        h.recv(g.ws, 5, { user_id: BOB, ssrc: 88648, speaking: 2 });
        h.track(g.pc, "audio", "a", `${BOB}-88648`);
        expect(h.delivered).toEqual([{ trackId: "a", pcId: 2 }]);
        expect(h.tracker.ownerOf(2)).toBe(BOB);
        expect(h.ssrcEvents()).toEqual([{ type: "ssrc", userId: BOB, videoSsrc: null, audioSsrc: 88648, pcId: 2 }]);
    });

    it("保留している間に終わったトラックは流さない", () => {
        const h = createHarness();
        const g = connect(h, RTC_BOB, "g");
        const t = h.track(g.pc, "video", "v", `${BOB}-88649`);
        t.ended = true;
        h.recv(g.ws, 12, op12(BOB, 88648, 88649));
        expect(h.delivered).toEqual([]);
        expect(h.lines).toContain(`track dropped: pc=${g.pc.serial} trackId=v reason=ended-before-routing`);
    });
});

describe("op0 の時点で通話の server id が読めないとき", () => {
    it("仕分けを保留して ssrc を取っておき、読めるようになった後に届いたメッセージで仕分けて、取っておいた ssrc を流す", () => {
        const h = createHarness(null);
        const call = connect(h, GUILD, "call");
        expect(h.lines).toContain(`ws unclassified: ws=${call.ws} server_id=${GUILD} reason=call-server-id-unknown (op0)`);
        h.recv(call.ws, 5, { user_id: ALICE, ssrc: 1, speaking: 1 });
        h.recv(call.ws, 5, { user_id: CAROL, ssrc: 2, speaking: 1 });
        h.recv(call.ws, 5, { user_id: ALICE, ssrc: 3, speaking: 1 });
        h.track(call.pc, "audio", "a", `${ALICE}-3`);
        expect(h.ssrcEvents()).toEqual([]);
        expect(h.delivered).toEqual([]);
        expect(h.lines).toContain(`track held: pc=${call.pc.serial} trackId=a reason=unclassified`);

        h.setCallServerId(GUILD);
        h.recv(call.ws, 6, {});
        // 参加者ごとに最新の 1 つだけを、最後に届いた順に流す。
        expect(h.ssrcEvents()).toEqual([
            { type: "ssrc", userId: CAROL, videoSsrc: null, audioSsrc: 2, pcId: VOICE_PC_ID },
            { type: "ssrc", userId: ALICE, videoSsrc: null, audioSsrc: 3, pcId: VOICE_PC_ID },
        ]);
        expect(h.delivered).toEqual([{ trackId: "a", pcId: VOICE_PC_ID }]);
    });

    it("後から読めた server id と違えば Go Live とし、取っておいた op12 を画面共有している人の事実として流す", () => {
        const h = createHarness(null);
        const g = connect(h, RTC_CAROL, "g");
        h.recv(g.ws, 12, op12(CAROL, 92, 93));
        expect(h.ssrcEvents()).toEqual([]);
        h.setCallServerId(GUILD);
        h.recv(g.ws, 6, {});
        expect(h.ssrcEvents()).toEqual([{ type: "ssrc", userId: CAROL, videoSsrc: 93, audioSsrc: 92, pcId: 2 }]);
        expect(h.tracker.ownerOf(2)).toBe(CAROL);
    });

    it("メッセージが届かなくても、保留しているトラックを再確認するときに、仕分けを試し直す", () => {
        const h = createHarness(null);
        const g = connect(h, RTC_CAROL, "g");
        h.recv(g.ws, 12, op12(CAROL, 92, 93));
        h.track(g.pc, "video", "v", `${CAROL}-93`);
        expect(h.delivered).toEqual([]);
        h.setCallServerId(GUILD);
        h.advance(TRACK_RECHECK_MS);
        expect(h.delivered).toEqual([{ trackId: "v", pcId: 2 }]);
    });
});

describe("視聴をやめた接続 (forget)", () => {
    it("手放した接続は、画面共有している人を答えず、ssrc もトラックも流さない", () => {
        const h = createHarness();
        const g = connect(h, RTC_CAROL, "g");
        h.recv(g.ws, 12, op12(CAROL, 92, 93));
        h.tracker.forget(2);
        expect(h.tracker.ownerOf(2)).toBeUndefined();
        expect(h.tracker.pcIdForUser(CAROL)).toBe(0);
        h.recv(g.ws, 12, op12(CAROL, 92, 93));
        h.track(g.pc, "video", "v", `${CAROL}-93`);
        expect(h.ssrcEvents()).toHaveLength(1);
        expect(h.delivered).toEqual([]);
    });
});

describe("読み取りの補助関数", () => {
    it("extractIceUfrag は SDP の最初の ufrag を返し、なければ null", () => {
        expect(extractIceUfrag(sdpWith("abcd"))).toBe("abcd");
        expect(extractIceUfrag("v=0\r\n")).toBeNull();
        expect(extractIceUfrag(undefined)).toBeNull();
    });

    it("parseMsid は 「userId-ssrc」 の形だけを読む", () => {
        expect(parseMsid(`${ALICE}-93`)).toEqual({ userId: ALICE, ssrc: 93 });
        expect(parseMsid("default")).toBeNull();
        expect(parseMsid("12345-93")).toBeNull();
        expect(parseMsid(null)).toBeNull();
    });

    it("parseGatewayFrame は JSON の文字列だけを読み、バイナリや壊れたものは null", () => {
        expect(parseGatewayFrame(JSON.stringify({ op: 5, d: { ssrc: 1 } }))).toEqual({ op: 5, d: { ssrc: 1 } });
        expect(parseGatewayFrame("{")).toBeNull();
        expect(parseGatewayFrame(new Uint8Array([1, 2]))).toBeNull();
        expect(parseGatewayFrame(JSON.stringify({ op: 5 }))).toBeNull();
    });
});
