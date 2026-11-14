// SPDX-License-Identifier: Apache-2.0

// adapter/stream_volume.ts の StreamVolumeKeeper と StreamVolumeDriver のテスト。Discord の画面共有の接続は偽物
// (FakeConn) で、接続の setLocalVolume() が値を置くだけのもの (Discord の web の接続と同じく、人ごとの値を持ち、
// getLocalVolume() は値がなければ既定の値を返す) にする。掛ける、戻す、戻し損ねた人を戻し直す、Discord が値を入れ直した
// 後の置き直し、利用者の操作の後に置き直さない (人ごとの VoiceState で観測する「1 つの画面共有」の単位)、新しい画面共有で
// また掛ける、接続が読めないときに何もしない、を確かめる。StreamVolumeDriver は、偽物のタイマーとメディアエンジンで、知らせの後の遅らせた置き直し、
// 自前の周期、Flux のイベントの振り分け、後始末を確かめる。

import { describe, expect, it } from "vitest";

import {
    type ConnectionEventSource,
    type SharingLike,
    STREAM_VOLUME_TICK_MS,
    type StreamConnectionLike,
    streamVolumeAvailable,
    StreamVolumeDriver,
    StreamVolumeKeeper,
    type StreamVolumeTimers,
    type VoiceStateLike,
} from "../adapter/stream_volume";

/** Discord の画面共有の音量の既定の値 (Discord の web の接続と MediaEngineStore の値)。 */
const DISCORD_DEFAULT = 18;

class FakeConn implements StreamConnectionLike {
    outputs: Record<string, object> = {};
    readonly volumes = new Map<string, number>();
    readonly calls: Array<[string, number]> = [];
    throwOnSet = false;

    constructor(outputUsers: readonly string[] = []) {
        for (const id of outputUsers) this.outputs[id] = {};
    }

    setLocalVolume(userId: string, volume: number): void {
        if (this.throwOnSet) throw new Error("boom");
        this.calls.push([userId, volume]);
        this.volumes.set(userId, volume);
    }

    getLocalVolume(userId: string): number {
        return this.volumes.get(userId) ?? DISCORD_DEFAULT;
    }

    /** Discord 自身が値を入れる (dispatch の処理、ユーザー設定の同期など)。calls には数えない。 */
    discordSets(userId: string, volume: number): void {
        this.volumes.set(userId, volume);
    }
}

/** getLocalVolume を持たない接続 (古い Discord)。 */
class BlindConn implements StreamConnectionLike {
    outputs: Record<string, object> = {};
    readonly calls: Array<[string, number]> = [];

    constructor(outputUsers: readonly string[] = []) {
        for (const id of outputUsers) this.outputs[id] = {};
    }

    setLocalVolume(userId: string, volume: number): void {
        this.calls.push([userId, volume]);
    }
}

interface SetupOptions {
    conns?: StreamConnectionLike[];
    sharing?: SharingLike[] | null;
    self?: string | null;
    stored?: Record<string, number | null>;
    voice?: Record<string, VoiceStateLike | null>;
}

/** 試験の既定の通話のチャンネル。 */
const CHANNEL = "c1";

function share(userId: string): SharingLike {
    return { userId };
}

function setup(options: SetupOptions = {}) {
    const state = {
        conns: options.conns ?? [],
        available: true,
        sharing: options.sharing === undefined ? ([] as SharingLike[] | null) : options.sharing,
        self: options.self ?? "self",
        stored: options.stored ?? ({} as Record<string, number | null>),
        // 人ごとの VoiceState。書いていない人は、Vesktop のいる通話の一覧 (sharing) から作る。一覧にいれば CHANNEL で
        // 画面共有中、いなければ VoiceState なし (本物の VoiceStateStore と同じく、通話にいない人はストアにいない)、一覧が
        // 読めなければ null。通話にいたまま画面共有をやめた人は、stopSharing() で selfStream 偽の VoiceState を書く。
        voice: options.voice ?? ({} as Record<string, VoiceStateLike | null>),
    };
    /** その人が通話にいたまま画面共有をやめた (VoiceState は残り、selfStream が偽になる)。 */
    const stopSharing = (userId: string): void => {
        state.sharing = (state.sharing ?? []).filter((u) => u.userId !== userId);
        state.voice[userId] = { channelId: CHANNEL, selfStream: false };
    };
    /** その人が CHANNEL で画面共有を始めた。 */
    const startSharing = (userId: string): void => {
        state.sharing = [...(state.sharing ?? []).filter((u) => u.userId !== userId), share(userId)];
        state.voice[userId] = { channelId: CHANNEL, selfStream: true };
    };
    const logs: string[] = [];
    const keeper = new StreamVolumeKeeper({
        streamConnections: () => (state.available ? state.conns : null),
        storedVolume: (userId) => (userId in state.stored ? (state.stored[userId] ?? null) : DISCORD_DEFAULT),
        sharing: () => state.sharing,
        voiceStateOf: (userId) => {
            if (userId in state.voice) return state.voice[userId] ?? null;
            if (state.sharing === null) return null;
            return state.sharing.some((u) => u.userId === userId) ? { channelId: CHANNEL, selfStream: true } : null;
        },
        selfUserId: () => state.self,
        log: (line) => logs.push(line),
    });
    keeper.setUserChangesVisible(true);
    return { state, logs, keeper, stopSharing, startSharing };
}

const silencedLine = (id: string): string => `stream audio: silenced userId=${id} in Vesktop`;

describe("StreamVolumeKeeper: 掛ける", () => {
    it("掛けていない間は、接続に何もしない", () => {
        const conn = new FakeConn(["a"]);
        const { keeper } = setup({ conns: [conn], sharing: [share("a")] });
        keeper.sync();
        keeper.setWanted(false);
        expect(conn.calls).toEqual([]);
        expect(keeper.needsTicks()).toBe(false);
    });

    it("掛けると、接続の Output の人と画面共有している人に 0 を置き、Output のある人だけをログに書く", () => {
        const conn = new FakeConn(["a"]);
        const { keeper, logs } = setup({ conns: [conn], sharing: [share("a"), share("b")] });
        keeper.setWanted(true);
        expect(conn.volumes.get("a")).toBe(0);
        expect(conn.volumes.get("b")).toBe(0);
        expect(logs).toEqual([silencedLine("a")]);
        expect(keeper.needsTicks()).toBe(true);
    });

    it("前から開いていた接続にも、掛け始めた時点で置く (条件が後から真になったとき)", () => {
        const conn = new FakeConn(["a"]);
        const { keeper } = setup({ conns: [conn], sharing: [share("a")] });
        keeper.sync();
        expect(conn.calls).toEqual([]);
        keeper.setWanted(true);
        expect(conn.calls).toEqual([["a", 0]]);
    });

    it("Vesktop のアカウント自身には置かない", () => {
        const conn = new FakeConn(["self"]);
        const { keeper } = setup({ conns: [conn], sharing: [share("self")] });
        keeper.setWanted(true);
        expect(conn.calls).toEqual([]);
    });

    it("同じ値のままなら、sync() を何度呼んでも置き直さない", () => {
        const conn = new FakeConn(["a"]);
        const { keeper } = setup({ conns: [conn], sharing: [share("a")] });
        keeper.setWanted(true);
        keeper.sync();
        keeper.setWanted(true);
        expect(conn.calls).toEqual([["a", 0]]);
    });

    it("Discord が値を入れ直したら、次の sync() で 0 を置き直してログに書く", () => {
        const conn = new FakeConn(["a"]);
        const { keeper, logs } = setup({ conns: [conn], sharing: [share("a")] });
        keeper.setWanted(true);
        conn.discordSets("a", 50);
        keeper.sync();
        expect(conn.volumes.get("a")).toBe(0);
        expect(logs.at(-1)).toBe("stream audio: silenced userId=a in Vesktop again (Discord had set 50)");
    });

    it("利用者の操作が見えない間は、Discord が値を入れ直しても置き直さない (利用者の値を消し続けない)", () => {
        const conn = new FakeConn(["a"]);
        const { keeper } = setup({ conns: [conn], sharing: [share("a")] });
        keeper.setUserChangesVisible(false);
        keeper.setWanted(true);
        conn.discordSets("a", 50);
        keeper.sync();
        expect(conn.volumes.get("a")).toBe(50);
        expect(conn.calls).toEqual([["a", 0]]);
    });

    it("今の値を返さない接続 (getLocalVolume がない) には 1 回だけ置き、置き直しもログも繰り返さない", () => {
        const conn = new BlindConn(["a"]);
        const { keeper, logs } = setup({ conns: [conn], sharing: [share("a")] });
        keeper.setWanted(true);
        keeper.sync();
        keeper.sync();
        expect(conn.calls).toEqual([["a", 0]]);
        expect(logs).toEqual([silencedLine("a")]);
    });

    it("Output が後からできた人 (トラックが後で届いた) は、0 のまま作られ、そのときに 1 回だけログに書く", () => {
        const conn = new FakeConn([]);
        const { keeper, logs } = setup({ conns: [conn], sharing: [share("a")] });
        keeper.setWanted(true);
        expect(conn.volumes.get("a")).toBe(0);
        expect(logs).toEqual([]);
        conn.outputs.a = {};
        keeper.sync();
        keeper.sync();
        expect(logs).toEqual([silencedLine("a")]);
        expect(conn.calls).toEqual([["a", 0]]);
    });

    it("接続が読めないときは何もせず、読めないことを変わったときに 1 行だけ書く", () => {
        const conn = new FakeConn(["a"]);
        const { keeper, logs, state } = setup({ conns: [conn], sharing: [share("a")] });
        state.available = false;
        keeper.setWanted(true);
        keeper.sync();
        keeper.sync();
        expect(conn.calls).toEqual([]);
        expect(logs).toEqual(["stream audio: the screen share connections in Vesktop can't be read; leaving the volume as it is"]);
        expect(streamVolumeAvailable({ streamConnections: () => null })).toBe(false);
        expect(streamVolumeAvailable({ streamConnections: () => [] })).toBe(true);
        state.available = true;
        keeper.sync();
        expect(conn.volumes.get("a")).toBe(0);
    });

    it("接続の setLocalVolume が例外を投げても、ほかの接続には置き、例外を外へ出さない", () => {
        const broken = new FakeConn(["a"]);
        broken.throwOnSet = true;
        const ok = new FakeConn(["b"]);
        const { keeper, logs } = setup({ conns: [broken, ok] });
        expect(() => keeper.setWanted(true)).not.toThrow();
        expect(ok.volumes.get("b")).toBe(0);
        expect(logs.some((line) => line.startsWith("stream audio: could not silence userId=a"))).toBe(true);
    });
});

describe("StreamVolumeKeeper: 戻す", () => {
    it("戻すと、置いた人を Discord が保存している音量 (なければ既定の値) にする", () => {
        const conn = new FakeConn(["a", "b"]);
        const { keeper, logs } = setup({ conns: [conn], sharing: [share("a"), share("b")], stored: { a: 70 } });
        keeper.setWanted(true);
        keeper.setWanted(false);
        expect(conn.volumes.get("a")).toBe(70);
        expect(conn.volumes.get("b")).toBe(DISCORD_DEFAULT);
        expect(logs).toContain("stream audio: restored userId=a in Vesktop (volume 70)");
        expect(keeper.needsTicks()).toBe(false);
        conn.discordSets("a", 70);
        keeper.sync();
        expect(conn.volumes.get("a")).toBe(70);
    });

    it("戻すときに接続が読めなければ記録に残し、読めるようになった sync() で戻す", () => {
        const conn = new FakeConn(["a"]);
        const { keeper, state } = setup({ conns: [conn], sharing: [share("a")] });
        keeper.setWanted(true);
        state.available = false;
        keeper.setWanted(false);
        expect(conn.volumes.get("a")).toBe(0);
        expect(keeper.needsTicks()).toBe(true);
        state.available = true;
        keeper.sync();
        expect(conn.volumes.get("a")).toBe(DISCORD_DEFAULT);
        expect(keeper.needsTicks()).toBe(false);
    });

    it("保存した音量が読めない人は残し、ほかの人は戻す。読めるようになったら戻す。失敗のログは 1 人に 1 行", () => {
        const conn = new FakeConn(["a", "b"]);
        const { keeper, state, logs } = setup({ conns: [conn], sharing: [share("a"), share("b")], stored: { a: null } });
        keeper.setWanted(true);
        keeper.setWanted(false);
        keeper.sync();
        expect(conn.volumes.get("a")).toBe(0);
        expect(conn.volumes.get("b")).toBe(DISCORD_DEFAULT);
        expect(logs.filter((line) => line.startsWith("stream audio: could not restore userId=a"))).toHaveLength(1);
        state.stored = { a: 40 };
        keeper.sync();
        expect(conn.volumes.get("a")).toBe(40);
        expect(keeper.needsTicks()).toBe(false);
    });

    it("戻せなかった接続が閉じたら、戻せなかったことのログの記録も捨て、次にまた戻せなかったときに書く", () => {
        const first = new FakeConn(["a"]);
        const { keeper, state, logs } = setup({ conns: [first], sharing: [share("a")], stored: { a: null } });
        keeper.setWanted(true);
        keeper.setWanted(false);
        state.conns = [];
        keeper.sync();
        expect(keeper.needsTicks()).toBe(false);
        state.conns = [new FakeConn(["a"])];
        keeper.setWanted(true);
        keeper.setWanted(false);
        expect(logs.filter((line) => line.startsWith("stream audio: could not restore userId=a"))).toHaveLength(2);
    });

    it("1 つの接続で置けなくても、ほかの接続の人は戻し、置けなかった接続は後で戻し直す", () => {
        const broken = new FakeConn(["a"]);
        const ok = new FakeConn(["b"]);
        const { keeper } = setup({ conns: [broken, ok], sharing: [share("a"), share("b")] });
        keeper.setWanted(true);
        broken.throwOnSet = true;
        keeper.setWanted(false);
        expect(ok.volumes.get("b")).toBe(DISCORD_DEFAULT);
        expect(broken.volumes.get("a")).toBe(0);
        expect(keeper.needsTicks()).toBe(true);
        broken.throwOnSet = false;
        keeper.sync();
        expect(broken.volumes.get("a")).toBe(DISCORD_DEFAULT);
        expect(keeper.needsTicks()).toBe(false);
    });

    it("閉じた接続 (一覧から消えた接続) は戻さずに記録から捨てる", () => {
        const conn = new FakeConn(["a"]);
        const { keeper, state } = setup({ conns: [conn], sharing: [share("a")] });
        keeper.setWanted(true);
        state.available = false;
        keeper.setWanted(false);
        state.available = true;
        state.conns = [];
        keeper.sync();
        expect(keeper.needsTicks()).toBe(false);
        expect(conn.volumes.get("a")).toBe(0);
    });

    it("戻して、また掛けると、ログをもう一度書く", () => {
        const conn = new FakeConn(["a"]);
        const { keeper, logs } = setup({ conns: [conn], sharing: [share("a")] });
        keeper.setWanted(true);
        keeper.setWanted(false);
        keeper.setWanted(true);
        expect(logs.filter((line) => line === silencedLine("a"))).toHaveLength(2);
        expect(conn.volumes.get("a")).toBe(0);
    });
});

describe("StreamVolumeKeeper: 利用者の操作", () => {
    const overrideLine = (id: string): string =>
        `stream audio: the user changed the volume of userId=${id} in Vesktop; not silencing it during this screen share`;

    it("掛けている間に動かした人は、その画面共有の間は置き直さず、戻すときも触れない", () => {
        const conn = new FakeConn(["a"]);
        const { keeper, logs } = setup({ conns: [conn], sharing: [share("a")] });
        keeper.setWanted(true);
        conn.discordSets("a", 40);
        keeper.noteUserVolumeChange("a");
        expect(logs.at(-1)).toBe(overrideLine("a"));
        keeper.sync();
        expect(conn.volumes.get("a")).toBe(40);
        keeper.setWanted(false);
        expect(conn.volumes.get("a")).toBe(40);
    });

    it("条件が一時的に崩れて掛け直しても (OBS との切断、退出など)、同じ画面共有の間は利用者の値を残す", () => {
        const conn = new FakeConn(["a"]);
        const { keeper } = setup({ conns: [conn], sharing: [share("a")] });
        keeper.setWanted(true);
        conn.discordSets("a", 40);
        keeper.noteUserVolumeChange("a");
        keeper.setWanted(false);
        keeper.observeSharing(); // 掛けていない間も観測は続く。その人はまだ画面共有している。
        keeper.setWanted(true);
        expect(conn.volumes.get("a")).toBe(40);
    });

    it("見直し (STREAM_DELETE と STREAM_CREATE、接続の作り直し) があっても、画面共有が続いていれば記録は残る", () => {
        const first = new FakeConn(["a"]);
        const { keeper, state } = setup({ conns: [first], sharing: [share("a")] });
        keeper.setWanted(true);
        first.discordSets("a", 40);
        keeper.noteUserVolumeChange("a");
        // 視聴をやめて見直す。その間も、その人の selfStream は真のまま (一覧にいる)。
        state.conns = [];
        keeper.observeSharing();
        keeper.sync();
        const second = new FakeConn(["a"]);
        second.discordSets("a", 40); // Discord は保存した値で接続を作る
        state.conns = [second];
        keeper.observeSharing();
        keeper.sync();
        expect(second.volumes.get("a")).toBe(40);
        expect(second.calls).toEqual([]);
    });

    it("一時的に切れた (STREAM_DELETE の unavailable) 間も、画面共有が続いていれば記録は残る", () => {
        const conn = new FakeConn(["a"]);
        const { keeper, state } = setup({ conns: [conn], sharing: [share("a")] });
        keeper.setWanted(true);
        conn.discordSets("a", 40);
        keeper.noteUserVolumeChange("a");
        state.conns = []; // 接続がつなぎ直されるまでの間
        keeper.sync();
        state.conns = [conn];
        keeper.sync();
        expect(conn.volumes.get("a")).toBe(40);
    });

    it("その人が画面共有をやめたと観測したら記録を消し、次の画面共有では 0 を置く", () => {
        const first = new FakeConn(["a"]);
        const { keeper, state, stopSharing, startSharing } = setup({ conns: [first], sharing: [share("a")] });
        keeper.setWanted(true);
        first.discordSets("a", 40);
        keeper.noteUserVolumeChange("a");
        stopSharing("a");
        state.conns = [];
        keeper.observeSharing();
        const second = new FakeConn(["a"]);
        state.conns = [second];
        startSharing("a");
        keeper.observeSharing();
        keeper.sync();
        expect(second.volumes.get("a")).toBe(0);
    });

    it("掛けていない間に画面共有をやめて始め直した (観測できた) ときは、次に掛けたときに 0 を置く", () => {
        const first = new FakeConn(["a"]);
        const { keeper, state, stopSharing, startSharing } = setup({ conns: [first], sharing: [share("a")] });
        keeper.setWanted(true);
        first.discordSets("a", 40);
        keeper.noteUserVolumeChange("a");
        keeper.setWanted(false);
        stopSharing("a");
        keeper.observeSharing();
        const second = new FakeConn(["a"]);
        second.discordSets("a", 40);
        state.conns = [second];
        startSharing("a");
        keeper.observeSharing();
        keeper.setWanted(true);
        expect(second.volumes.get("a")).toBe(0);
    });

    it("その人の VoiceState が読めない間は、記録を消さない", () => {
        const conn = new FakeConn(["a"]);
        const { keeper, state } = setup({ conns: [conn], sharing: [share("a")] });
        keeper.setWanted(true);
        conn.discordSets("a", 40);
        keeper.noteUserVolumeChange("a");
        state.sharing = null;
        keeper.observeSharing();
        keeper.sync();
        state.sharing = [share("a")];
        keeper.observeSharing();
        keeper.sync();
        expect(conn.volumes.get("a")).toBe(40);
    });

    it("利用者の操作のログは、その人とその画面共有で初めて記録したときだけ書く", () => {
        const conn = new FakeConn(["a"]);
        const { keeper, logs, stopSharing, startSharing } = setup({ conns: [conn], sharing: [share("a")] });
        keeper.setWanted(true);
        for (let i = 0; i < 20; i++) keeper.noteUserVolumeChange("a"); // スライダーを動かす間
        expect(logs.filter((line) => line === overrideLine("a"))).toHaveLength(1);
        stopSharing("a");
        keeper.observeSharing();
        startSharing("a");
        keeper.noteUserVolumeChange("a");
        expect(logs.filter((line) => line === overrideLine("a"))).toHaveLength(2);
    });

    it("掛けていない間の操作は記録しない (次に掛けたときに 0 にする)", () => {
        const conn = new FakeConn(["a"]);
        const { keeper, logs } = setup({ conns: [conn], sharing: [share("a")] });
        conn.discordSets("a", 40);
        keeper.noteUserVolumeChange("a");
        expect(logs).toEqual([]);
        keeper.setWanted(true);
        expect(conn.volumes.get("a")).toBe(0);
    });

    it("2 人を同時に見ていて、Discord が 1 人の値を両方の接続に書いても、その人は誤って置き直さず、もう 1 人は 0 のまま", () => {
        const connA = new FakeConn(["a"]);
        const connB = new FakeConn(["b"]);
        const { keeper, logs } = setup({ conns: [connA, connB], sharing: [share("a"), share("b")] });
        keeper.setWanted(true);
        // 右クリックの音量 (AUDIO_SET_LOCAL_VOLUME) は、Discord がすべての画面共有の接続に入れる。
        connA.discordSets("a", 40);
        connB.discordSets("a", 40);
        keeper.noteUserVolumeChange("a");
        keeper.sync();
        expect(connA.volumes.get("a")).toBe(40);
        expect(connB.volumes.get("a")).toBe(40);
        expect(connA.volumes.get("b")).toBe(0);
        expect(connB.volumes.get("b")).toBe(0);
        expect(logs.some((line) => line.includes("again"))).toBe(false);
    });

    it("Output がまだない間の操作も記録する", () => {
        const conn = new FakeConn([]);
        const { keeper } = setup({ conns: [conn], sharing: [share("a")] });
        keeper.setWanted(true);
        conn.discordSets("a", 40);
        keeper.noteUserVolumeChange("a");
        conn.outputs.a = {};
        keeper.sync();
        expect(conn.volumes.get("a")).toBe(40);
    });
});

describe("StreamVolumeKeeper: 人ごとの VoiceState で観測する「1 つの画面共有」", () => {
    /** a の画面共有で、利用者が音量を動かした状態を作る。 */
    function overriddenA() {
        const conn = new FakeConn(["a"]);
        const env = setup({ conns: [conn], sharing: [share("a")], voice: { a: { channelId: "x", selfStream: true } } });
        env.keeper.setWanted(true);
        conn.discordSets("a", 40);
        env.keeper.noteUserVolumeChange("a");
        return { conn, ...env };
    }

    /** 記録が残っているかを、同じ画面共有の新しい接続に 0 を置くかで確かめる。 */
    function stillOverridden(env: ReturnType<typeof overriddenA>): boolean {
        const next = new FakeConn(["a"]);
        next.discordSets("a", 40);
        env.state.conns = [next];
        env.state.sharing = [share("a")];
        env.keeper.sync();
        return next.volumes.get("a") === 40;
    }

    it("Vesktop が別の通話にいて、その一覧に a がいなくても、a が X で画面共有を続けていれば記録は残る", () => {
        const env = overriddenA();
        env.state.sharing = []; // Vesktop は Y にいて、Y の一覧には a がいない
        env.keeper.observeSharing();
        env.keeper.setWanted(false);
        env.keeper.observeSharing();
        // X に戻ってまた掛ける。
        env.keeper.setWanted(true);
        expect(stillOverridden(env)).toBe(true);
    });

    it("Vesktop が通話にいない間に a がやめたと観測できたら、記録を消す", () => {
        const env = overriddenA();
        env.keeper.setWanted(false);
        env.state.sharing = null; // Vesktop は通話にいない
        env.state.voice.a = { channelId: "x", selfStream: false };
        env.keeper.observeSharing();
        env.state.voice.a = { channelId: "x", selfStream: true }; // また始めた
        env.keeper.setWanted(true);
        expect(stillOverridden(env)).toBe(false);
    });

    it("ストアが空になって a の VoiceState がない間は記録を残し、また読めたときも同じ画面共有を続けていれば残す", () => {
        const env = overriddenA();
        env.state.voice.a = null; // GUILD_CREATE などでストアが空になった直後
        env.keeper.observeSharing();
        env.state.voice.a = { channelId: "x", selfStream: true }; // ストアが埋め直された。a は画面共有を続けている
        env.keeper.observeSharing();
        expect(stillOverridden(env)).toBe(true);
    });

    it("a が通話を抜ける (VoiceState がない、記録は残る) -> 戻る (selfStream 偽、記録が消える) -> 新しい画面共有では 0", () => {
        const env = overriddenA();
        // 通話を抜けた。VoiceStateStore から a がいなくなる。記録は残る。
        env.state.voice.a = null;
        env.state.sharing = [];
        env.keeper.observeSharing();
        // 通話に戻った。まだ画面共有していない (selfStream 偽)。ここで記録が消える。
        env.state.voice.a = { channelId: "x", selfStream: false };
        env.keeper.observeSharing();
        // 新しい画面共有を始めた。0 を置く。
        env.state.voice.a = { channelId: "x", selfStream: true };
        expect(stillOverridden(env)).toBe(false);
    });

    it("a が通話を抜けている間 (VoiceState がない) は、記録を残す", () => {
        const env = overriddenA();
        env.state.voice.a = null;
        env.state.sharing = [];
        env.keeper.observeSharing();
        env.keeper.sync();
        env.state.voice.a = { channelId: "x", selfStream: true }; // 抜けた形のまま比べないように、確かめる直前だけ戻す
        expect(stillOverridden(env)).toBe(true);
    });

    it("a が画面共有したまま別のチャンネルに移ったら、別の画面共有として記録を消す", () => {
        const env = overriddenA();
        env.state.voice.a = { channelId: "z", selfStream: true };
        env.keeper.observeSharing();
        expect(stillOverridden(env)).toBe(false);
    });

    it("記録した時にチャンネルが分からなければ、次に画面共有していると観測したチャンネルで埋める", () => {
        const conn = new FakeConn(["a"]);
        const env = setup({ conns: [conn], sharing: [share("a")], voice: { a: null } });
        env.keeper.setWanted(true);
        conn.discordSets("a", 40);
        env.keeper.noteUserVolumeChange("a");
        env.state.voice.a = { channelId: "x", selfStream: true };
        env.keeper.observeSharing();
        expect(stillOverridden({ conn, ...env })).toBe(true);
        env.state.voice.a = { channelId: "z", selfStream: true };
        env.keeper.observeSharing();
        expect(stillOverridden({ conn, ...env })).toBe(false);
    });
});

/** 呼ばれた順に並べるだけの偽物のタイマー。run() で、期限の来たものを動かす。 */
class FakeTimers implements StreamVolumeTimers {
    private next = 1;
    readonly timeouts = new Map<number, () => void>();
    readonly intervals = new Map<number, { fn: () => void; ms: number }>();

    setTimeout(fn: () => void): unknown {
        const id = this.next++;
        this.timeouts.set(id, fn);
        return id;
    }

    clearTimeout(handle: unknown): void {
        this.timeouts.delete(handle as number);
    }

    setInterval(fn: () => void, ms: number): unknown {
        const id = this.next++;
        this.intervals.set(id, { fn, ms });
        return id;
    }

    clearInterval(handle: unknown): void {
        this.intervals.delete(handle as number);
    }

    /** 待っている setTimeout をすべて動かす。 */
    flush(): void {
        const pending = [...this.timeouts];
        this.timeouts.clear();
        for (const [, fn] of pending) fn();
    }

    /** 周期のタイマーを 1 回ずつ動かす。 */
    tick(): void {
        for (const { fn } of [...this.intervals.values()]) fn();
    }
}

class FakeEngine implements ConnectionEventSource {
    readonly listeners = new Set<(c: unknown) => void>();

    on(event: string, listener: (c: unknown) => void): void {
        if (event === "connection") this.listeners.add(listener);
    }

    removeListener(event: string, listener: (c: unknown) => void): void {
        if (event === "connection") this.listeners.delete(listener);
    }

    emitConnection(): void {
        for (const l of this.listeners) l({});
    }
}

function setupDriver(options: SetupOptions = {}) {
    const base = setup(options);
    const timers = new FakeTimers();
    const engine = new FakeEngine();
    let rechecks = 0;
    const driver = new StreamVolumeDriver({
        keeper: base.keeper,
        timers,
        engine: () => engine,
        available: () => base.state.available,
        recheck: () => {
            rechecks++;
        },
    });
    driver.setUserChangesVisible(true);
    return { ...base, timers, engine, driver, rechecks: () => rechecks };
}

describe("StreamVolumeDriver", () => {
    it("掛けると、メディアエンジンの connection を購読し、自前の周期を動かす", () => {
        const conn = new FakeConn(["a"]);
        const { driver, engine, timers } = setupDriver({ conns: [conn], sharing: [share("a")] });
        driver.setWanted(true);
        expect(engine.listeners.size).toBe(1);
        expect([...timers.intervals.values()].map((i) => i.ms)).toEqual([STREAM_VOLUME_TICK_MS]);
        expect(conn.volumes.get("a")).toBe(0);
    });

    it("新しい接続の知らせの後、今の処理が終わってから置く (知らせの中では置かない)", () => {
        const { driver, engine, timers, state } = setupDriver({ sharing: [share("a")] });
        driver.setWanted(true);
        const conn = new FakeConn([]);
        state.conns = [conn];
        engine.emitConnection();
        expect(conn.calls).toEqual([]);
        timers.flush();
        expect(conn.volumes.get("a")).toBe(0);
    });

    it("置き直しの予約は 1 回にまとめる", () => {
        const { driver, timers } = setupDriver({ conns: [new FakeConn(["a"])], sharing: [share("a")] });
        driver.setWanted(true);
        driver.scheduleSync();
        driver.scheduleSync();
        driver.onFluxEvent({ type: "USER_SETTINGS_PROTO_UPDATE" });
        expect(timers.timeouts.size).toBe(1);
    });

    it("USER_SETTINGS_PROTO_UPDATE と CONNECTION_OPEN の後に、Discord が入れ直した値に 0 を置き直す", () => {
        const conn = new FakeConn(["a"]);
        const { driver, timers } = setupDriver({ conns: [conn], sharing: [share("a")] });
        driver.setWanted(true);
        for (const type of ["USER_SETTINGS_PROTO_UPDATE", "CONNECTION_OPEN"]) {
            conn.discordSets("a", 50);
            driver.onFluxEvent({ type });
            expect(conn.volumes.get("a")).toBe(50);
            timers.flush();
            expect(conn.volumes.get("a")).toBe(0);
        }
    });

    it("周期でも置き直す (ほかの依存箇所の確認の周期には頼らない)", () => {
        const conn = new FakeConn(["a"]);
        const { driver, timers } = setupDriver({ conns: [conn], sharing: [share("a")] });
        driver.setWanted(true);
        conn.discordSets("a", 50);
        timers.tick();
        expect(conn.volumes.get("a")).toBe(0);
    });

    it("AUDIO_SET_LOCAL_VOLUME は context が stream のときだけ利用者の操作として扱う", () => {
        const conn = new FakeConn(["a"]);
        const { driver, timers } = setupDriver({ conns: [conn], sharing: [share("a")] });
        driver.setWanted(true);
        conn.discordSets("a", 40);
        driver.onFluxEvent({ type: "AUDIO_SET_LOCAL_VOLUME", context: "default", userId: "a", volume: 40 });
        timers.tick();
        expect(conn.volumes.get("a")).toBe(0); // 声の音量の操作では記録しない
        conn.discordSets("a", 40);
        driver.onFluxEvent({ type: "AUDIO_SET_LOCAL_VOLUME", context: "stream", userId: "a", volume: 40 });
        timers.tick();
        expect(conn.volumes.get("a")).toBe(40);
    });

    it("ほかの種類の Flux のイベントでは何もしない", () => {
        const { driver, timers } = setupDriver({ conns: [new FakeConn(["a"])], sharing: [share("a")] });
        driver.setWanted(true);
        driver.onFluxEvent({ type: "VOICE_STATE_UPDATES" });
        expect(timers.timeouts.size).toBe(0);
    });

    it("戻し終えたら周期を止め、戻し損ねた人がいる間は周期で戻し直す", () => {
        const conn = new FakeConn(["a"]);
        const { driver, timers, state } = setupDriver({ conns: [conn], sharing: [share("a")] });
        driver.setWanted(true);
        state.available = false;
        driver.setWanted(false);
        expect(timers.intervals.size).toBe(1);
        state.available = true;
        timers.tick();
        expect(conn.volumes.get("a")).toBe(DISCORD_DEFAULT);
        expect(timers.intervals.size).toBe(0);
    });

    it("掛けていない間は周期を動かさない", () => {
        const { driver, timers } = setupDriver({ conns: [new FakeConn(["a"])], sharing: [share("a")] });
        driver.setWanted(false);
        driver.scheduleSync();
        expect(timers.intervals.size).toBe(0);
        expect(timers.timeouts.size).toBe(0);
    });

    it("掛け始めたときに使えなければ、依存箇所の確認し直しを今の処理の後に頼む", () => {
        const { driver, timers, state, rechecks } = setupDriver({});
        state.available = false;
        driver.setWanted(true);
        expect(rechecks()).toBe(0);
        timers.flush();
        expect(rechecks()).toBe(1);
    });

    it("利用者の操作が見えない (購読に失敗した) ときは、1 つの接続の 1 人に 1 回だけ置く", () => {
        const conn = new FakeConn(["a"]);
        const { driver, timers } = setupDriver({ conns: [conn], sharing: [share("a")] });
        driver.setUserChangesVisible(false);
        driver.setWanted(true);
        conn.discordSets("a", 40);
        timers.tick();
        expect(conn.volumes.get("a")).toBe(40);
    });

    it("STREAM_DELETE と STREAM_CREATE では記録を消さず、observeSharing で画面共有をやめたと観測したときだけ消す", () => {
        const first = new FakeConn(["a"]);
        const { driver, timers, state, stopSharing, startSharing } = setupDriver({ conns: [first], sharing: [share("a")] });
        driver.setWanted(true);
        first.discordSets("a", 40);
        driver.onFluxEvent({ type: "AUDIO_SET_LOCAL_VOLUME", context: "stream", userId: "a", volume: 40 });
        driver.onFluxEvent({ type: "STREAM_DELETE", streamKey: "call:c1:a", unavailable: false, reason: "user_requested" });
        driver.onFluxEvent({ type: "STREAM_CREATE", streamKey: "call:c1:a" });
        timers.flush();
        timers.tick();
        expect(first.volumes.get("a")).toBe(40);
        // その人が画面共有をやめたと観測し、また始めた。
        stopSharing("a");
        driver.observeSharing();
        startSharing("a");
        const second = new FakeConn(["a"]);
        second.discordSets("a", 40);
        state.conns = [second];
        driver.observeSharing();
        driver.scheduleSync();
        timers.flush();
        expect(second.volumes.get("a")).toBe(0);
    });

    it("VOICE_STATE_UPDATES と PASSIVE_UPDATE_V2 で、利用者の操作の記録を人ごとの VoiceState と比べる", () => {
        for (const type of ["VOICE_STATE_UPDATES", "PASSIVE_UPDATE_V2"]) {
            const conn = new FakeConn(["a"]);
            const { driver, state } = setupDriver({ conns: [conn], sharing: [share("a")], voice: { a: { channelId: "x", selfStream: true } } });
            driver.setWanted(true);
            conn.discordSets("a", 40);
            driver.onFluxEvent({ type: "AUDIO_SET_LOCAL_VOLUME", context: "stream", userId: "a", volume: 40 });
            driver.setWanted(false);
            state.voice.a = { channelId: "x", selfStream: false };
            driver.onFluxEvent({ type });
            state.voice.a = { channelId: "x", selfStream: true };
            driver.setWanted(true);
            expect(conn.volumes.get("a"), type).toBe(0);
        }
    });

    it("掛けていない間も observeSharing で観測する", () => {
        const conn = new FakeConn(["a"]);
        const { driver, stopSharing, startSharing } = setupDriver({ conns: [conn], sharing: [share("a")] });
        driver.setWanted(true);
        conn.discordSets("a", 40);
        driver.onFluxEvent({ type: "AUDIO_SET_LOCAL_VOLUME", context: "stream", userId: "a", volume: 40 });
        driver.setWanted(false);
        stopSharing("a");
        driver.observeSharing();
        startSharing("a");
        driver.setWanted(true);
        expect(conn.volumes.get("a")).toBe(0);
    });

    it("メディアエンジンを購読できなかったときは、周期の中で購読し直す", () => {
        const base = setup({ conns: [new FakeConn(["a"])], sharing: [share("a")] });
        const timers = new FakeTimers();
        const engine = new FakeEngine();
        let engineReady = false;
        const driver = new StreamVolumeDriver({
            keeper: base.keeper,
            timers,
            engine: () => (engineReady ? engine : null),
            available: () => true,
            recheck: () => {},
        });
        driver.setWanted(true);
        expect(engine.listeners.size).toBe(0);
        engineReady = true;
        timers.tick();
        expect(engine.listeners.size).toBe(1);
        timers.tick();
        expect(engine.listeners.size).toBe(1);
    });

    it("掛けるのをやめて戻し損ねた後に後始末が呼ばれたら (stop() の順)、周期を止める前に 1 回戻し直す", () => {
        const conn = new FakeConn(["a"]);
        const { driver, timers, state } = setupDriver({ conns: [conn], sharing: [share("a")] });
        driver.setWanted(true);
        state.available = false;
        driver.setWanted(false);
        expect(conn.volumes.get("a")).toBe(0);
        state.available = true;
        driver.dispose();
        expect(conn.volumes.get("a")).toBe(DISCORD_DEFAULT);
        expect(timers.intervals.size).toBe(0);
    });

    it("後始末で戻し、タイマーとメディアエンジンの購読を外す", () => {
        const conn = new FakeConn(["a"]);
        const { driver, timers, engine } = setupDriver({ conns: [conn], sharing: [share("a")] });
        driver.setWanted(true);
        driver.scheduleSync();
        driver.dispose();
        expect(conn.volumes.get("a")).toBe(DISCORD_DEFAULT);
        expect(engine.listeners.size).toBe(0);
        expect(timers.intervals.size).toBe(0);
        expect(timers.timeouts.size).toBe(0);
    });
});
