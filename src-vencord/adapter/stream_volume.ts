// SPDX-License-Identifier: Apache-2.0

// Vesktop の中の画面共有の接続 (Discord の web 版のメディアエンジンの、context が "stream" の接続) の音量を 0 にし、
// 元に戻す。掛けるかどうかは core/stream_silence.ts が決め、index.ts が adapter の setStreamSilence() で伝える。
// Discord の設定は変えない。接続の setLocalVolume() を直接呼ぶだけで、dispatch しないので、MediaEngineStore
// (localStorage) にもユーザー設定 (サーバーに同期される) にも書かない。接続が閉じれば値も消える。接続の
// setLocalVolume() は、その人の Output の audio 要素の volume を変えるだけで、トラックにも受け取りの希望
// (media sink wants) にも触れないので、cordmux が受け取る音は変わらない。ミュート (setLocalMute) は使わない。画面共有の
// タイルにミュートの印が出るうえ、声の接続ではミュートが受け取りの希望に使われるためである (docs/architecture.md の
// 「Vesktop の画面共有の音」)。
//
// StreamVolumeKeeper が値を置いて戻し、StreamVolumeDriver が、いつ置き直すか (Discord の知らせの後、周期) と
// Flux のイベントの振り分けを受け持つ。Discord の実際のオブジェクトとタイマーは adapter/vencord.ts が渡す。Vencord と
// DOM の型を使わないので、vitest でテストできる (tests/adapter_stream_volume.test.ts)。

/** Discord の MediaEngineContextTypes.STREAM の値 (画面共有の接続)。 */
export const STREAM_CONTEXT = "stream";

/**
 * 掛けている間 (と戻し損ねた人がいる間) に置き直す周期 (ms)。Discord が知らせのない経路で値を入れ直したときの
 * 受け皿で、ふつうは知らせの後の置き直しで足りる。目安の値で、測って決めたものではない。
 */
export const STREAM_VOLUME_TICK_MS = 30000;

/** Discord の画面共有の接続のうち、ここが使う部分。 */
export interface StreamConnectionLike {
    /** 人の userId -> その人の Output。鍵だけを読む (Output は、その人のトラックが届いたときにできる)。 */
    readonly outputs?: unknown;
    /** その人の音量を置く。値は MediaEngineStore の音量と同じ単位。 */
    setLocalVolume(userId: string, volume: number): void;
    /** 接続が今その人に使っている音量。古い Discord にはないことがあるので任意にする。 */
    getLocalVolume?(userId: string): number;
}

/** 画面共有している 1 人。 */
export interface SharingLike {
    readonly userId: string;
}

/** 1 人の VoiceState のうち、ここが見る部分 (VoiceStateStore.getVoiceStateForUser)。 */
export interface VoiceStateLike {
    /** その人がいるボイスチャンネル。いなければ null。 */
    readonly channelId: string | null;
    /** 画面共有しているか (VoiceState の selfStream)。 */
    readonly selfStream: boolean;
}

/** StreamVolumeKeeper が Discord と外の値を読むための関数。 */
export interface StreamVolumeDeps {
    /** context が "stream" の接続の一覧。メディアエンジンが読めないときは null。 */
    streamConnections(): readonly StreamConnectionLike[] | null;
    /**
     * Discord が保存している、その人の画面共有の音量 (MediaEngineStore.getLocalVolume(userId, "stream")。利用者が
     * 決めていなければ Discord の既定の値)。読めなければ null。戻すときに接続に入れる値で、Discord 自身が接続を
     * 作るときに入れる値と同じである。
     */
    storedVolume(userId: string): number | null;
    /** Vesktop のアカウントがいる通話で画面共有している人 (0 を置く相手)。読めなければ null。 */
    sharing(): readonly SharingLike[] | null;
    /**
     * その人の VoiceState。Vesktop のアカウントがいるチャンネルにかかわらず引ける (Vesktop のアカウントが見えるサーバーと
     * 会話の通話の中だけ)。読めないとき、ストアにないとき (見えない、ストアが空になった直後) は null。
     */
    voiceStateOf(userId: string): VoiceStateLike | null;
    /** Vesktop のアカウントの userId (読めなければ null)。自分の画面共有の音量には触れない。 */
    selfUserId(): string | null;
    /** 送信側の出来事の行を 1 行書く。 */
    log(line: string): void;
}

/** 依存箇所 (stream-volume) が使えるか。probe と、音量を置く処理の両方が、この 1 つの判定を使う。 */
export function streamVolumeAvailable(deps: Pick<StreamVolumeDeps, "streamConnections">): boolean {
    return deps.streamConnections() !== null;
}

function outputUserIds(conn: StreamConnectionLike): string[] {
    try {
        const outputs = conn.outputs;
        if (typeof outputs !== "object" || outputs === null) return [];
        return Object.keys(outputs);
    } catch {
        return [];
    }
}

function currentVolume(conn: StreamConnectionLike, userId: string): number | undefined {
    try {
        if (typeof conn.getLocalVolume !== "function") return undefined;
        const v = conn.getLocalVolume(userId);
        return typeof v === "number" ? v : undefined;
    } catch {
        return undefined;
    }
}

function addTo<K>(map: Map<K, Set<string>>, key: K, userId: string): void {
    let set = map.get(key);
    if (set === undefined) {
        set = new Set<string>();
        map.set(key, set);
    }
    set.add(userId);
}

/**
 * 画面共有の接続の音量を 0 にして保ち、元に戻す。
 *
 * - 掛けている間 (setWanted(true))、sync() は、画面共有の接続ごとに、その接続の Output の人と画面共有している人
 *   (Vesktop のアカウントを除く) に 0 を置く。Output ができる前 (接続ができた直後) に置けば、その人の音は最初から鳴ら
 *   ない。その人を運ばない接続に置いた値は、Output がないので何も鳴らさない。Discord が値を入れ直していれば (接続が
 *   0 でない値を返せば) 置き直す。接続が今の値を返さない (getLocalVolume がない) ときは、1 回置いたら置き直さない
 *   (置いた値と比べられないので、置き直しとログを繰り返さない)。
 * - 利用者が、掛けている間に Vesktop でその人の画面共有の音量を動かした (noteUserVolumeChange) ら、その人のその
 *   画面共有の間は置き直さず、戻すときも触れず、掛けるのをやめて掛け直すときも 0 を置かない (Discord は利用者の値を
 *   すべての画面共有の接続に入れる)。「その人の 1 つの画面共有」は、その人の VoiceState が selfStream 真で、同じ
 *   channelId のまま続けて観測されている期間である。記録は、その人ごとに VoiceState を引いて比べる (observeSharing と
 *   sync())。Vesktop のアカウントがいるチャンネルの一覧では比べない (Vesktop が別の通話にいる間や通話にいない間に、
 *   一覧にいないことを、やめたことと取り違えるため)。終わりは、その人の VoiceState が読めて、selfStream が偽か、
 *   channelId が記録の時と違うときだけで、VoiceState が読めない・ないときは分からないとして記録を残す (利用者の値が
 *   残る側、鳴る側に倒す)。STREAM_DELETE と STREAM_CREATE では区切らない。どちらも、視聴を始め直すときや一時的に
 *   切れたとき (STREAM_DELETE の unavailable) にも届き、同じ画面共有の途中で利用者の値を 0 に戻してしまうためである。
 *   掛けていない間の操作は記録しない。
 * - setWanted(false) で、置いた人を Discord が保存している音量に戻す。接続が読めない、保存した音量が読めない、置けない
 *   ときは、その人を記録に残し、次の sync() (知らせの後と周期) で戻し直す。戻せる接続の人は、ほかの接続と関係なく戻す。
 * - 利用者の操作が見えない (Flux の AUDIO_SET_LOCAL_VOLUME を購読できていない) 間は、1 つの接続の 1 人に 1 回だけ置き、
 *   置き直さない。置き直すと、利用者が動かした値を cordmux が消し続けるためである (setUserChangesVisible)。
 */
export class StreamVolumeKeeper {
    private wanted = false;
    private userChangesVisible = false;
    // 接続ごとの、0 を置いた人 (戻す相手)。読めた一覧にない接続 (閉じた接続) は捨てる。
    private readonly applied = new Map<StreamConnectionLike, Set<string>>();
    // 接続ごとの、掛けたことをログに書いた人。戻したら消す (また掛けたときに書く)。
    private readonly announced = new Map<StreamConnectionLike, Set<string>>();
    // 掛けている間に利用者が音量を動かした人 -> その画面共有のチャンネル (記録した時に分からなければ null で、次に
    // 画面共有していると観測したときに埋める)。その人の画面共有が終わったと観測するまで持つ。
    private readonly overridden = new Map<string, string | null>();
    // 戻せなかったことをログに書いた人 (同じ人について、戻し直すたびに書かない)。
    private readonly restoreFailLogged = new Set<string>();
    // 最後に分かった、接続の一覧が読めるか (変わったときだけログに書く)。
    private lastAvailable: boolean | undefined;

    constructor(private readonly deps: StreamVolumeDeps) {}

    /** 掛けるかを受け取る。true なら掛け、true から false になったら戻す。 */
    setWanted(on: boolean): void {
        if (on) {
            this.wanted = true;
            this.sync();
            return;
        }
        if (!this.wanted) return;
        this.wanted = false;
        this.announced.clear();
        this.sync();
    }

    /** 掛けているか。 */
    isWanted(): boolean {
        return this.wanted;
    }

    /** 置き直しか戻し直しが要るか (掛けている間と、戻せなかった人がいる間)。周期のタイマーを動かすかに使う。 */
    needsTicks(): boolean {
        return this.wanted || this.applied.size > 0;
    }

    /** 利用者が音量を動かしたことを受け取れるか (Flux の AUDIO_SET_LOCAL_VOLUME を購読できているか)。 */
    setUserChangesVisible(visible: boolean): void {
        this.userChangesVisible = visible;
    }

    /** 掛けている間は 0 を置き直し、掛けていない間は戻せなかった人を戻し直す。どちらでも、利用者の操作の記録を観測と比べる。 */
    sync(): void {
        this.observeSharing();
        if (this.wanted) this.applyPass(this.readSharing());
        else if (this.applied.size > 0) this.restorePass();
    }

    /**
     * 画面共有の状態が変わったかもしれない (Discord の VOICE_STATE_UPDATES などの後)。掛けているかにかかわらず、利用者の
     * 操作の記録がある人ごとに VoiceState を引き、画面共有が終わったと観測した人の記録を消す (クラスのコメント)。
     */
    observeSharing(): void {
        for (const [userId, channelId] of [...this.overridden]) {
            const state = this.safe(() => this.deps.voiceStateOf(userId), null);
            if (state === null) continue; // 分からない間は消さない。
            if (!state.selfStream || state.channelId === null) {
                this.overridden.delete(userId);
            } else if (channelId === null) {
                this.overridden.set(userId, state.channelId);
            } else if (state.channelId !== channelId) {
                this.overridden.delete(userId);
            }
        }
    }

    /**
     * 利用者が Vesktop で、その人の画面共有の音量を動かした (Discord は、すでに利用者の値をすべての画面共有の接続に
     * 入れている)。掛けている間だけ記録し、その人のその画面共有の間は置き直さない。
     */
    noteUserVolumeChange(userId: string): void {
        if (!this.wanted) return;
        // ログは、その人とその画面共有で初めて記録したときだけ書く (スライダーを動かす間は、何十回も届くため)。
        const first = !this.overridden.has(userId);
        if (first || this.overridden.get(userId) === null) {
            const state = this.safe(() => this.deps.voiceStateOf(userId), null);
            this.overridden.set(userId, state !== null && state.selfStream && state.channelId !== null ? state.channelId : null);
        }
        // Discord が利用者の値を入れたので、戻す相手から外す。
        for (const [conn, users] of this.applied) {
            users.delete(userId);
            if (users.size === 0) this.applied.delete(conn);
        }
        if (first) {
            this.deps.log(`stream audio: the user changed the volume of userId=${userId} in Vesktop; not silencing it during this screen share`);
        }
    }

    private applyPass(sharing: readonly SharingLike[] | null): void {
        const conns = this.readConnections();
        if (conns === null) return;
        this.pruneClosed(conns);
        const self = this.safe(() => this.deps.selfUserId(), null);
        const sharingIds = (sharing ?? []).map((s) => s.userId);
        for (const conn of conns) {
            const withOutput = outputUserIds(conn);
            const targets = new Set<string>([...withOutput, ...sharingIds]);
            if (self !== null) targets.delete(self);
            for (const userId of targets) {
                if (this.overridden.has(userId)) continue;
                const before = this.applied.get(conn)?.has(userId) === true;
                const now = currentVolume(conn, userId);
                // 置き直すのは、利用者の操作が見えていて、接続が 0 でない値を返したときだけ (クラスのコメント)。
                if (!before || (this.userChangesVisible && now !== undefined && now !== 0)) {
                    try {
                        conn.setLocalVolume(userId, 0);
                    } catch (e) {
                        this.deps.log(`stream audio: could not silence userId=${userId} in Vesktop (${String(e)})`);
                        continue;
                    }
                    addTo(this.applied, conn, userId);
                    if (before) this.deps.log(`stream audio: silenced userId=${userId} in Vesktop again (Discord had set ${String(now)})`);
                }
                // 掛けたことは、その接続にその人の Output ができた (その人の音が届いている) ときに 1 回だけ書く。その人を
                // 運ばない接続に置いた分は書かない。
                if (withOutput.includes(userId) && this.announced.get(conn)?.has(userId) !== true) {
                    addTo(this.announced, conn, userId);
                    this.deps.log(`stream audio: silenced userId=${userId} in Vesktop`);
                }
            }
        }
    }

    private restorePass(): void {
        const conns = this.readConnections();
        if (conns === null) return; // 読めるようになったら戻す (記録は残す)。
        this.pruneClosed(conns);
        for (const [conn, users] of this.applied) {
            const withOutput = outputUserIds(conn);
            for (const userId of [...users]) {
                const volume = this.safe(() => this.deps.storedVolume(userId), null);
                if (volume === null) {
                    this.noteRestoreFailure(userId, "could not read the volume");
                    continue;
                }
                try {
                    conn.setLocalVolume(userId, volume);
                } catch (e) {
                    this.noteRestoreFailure(userId, String(e));
                    continue;
                }
                users.delete(userId);
                this.restoreFailLogged.delete(userId);
                if (withOutput.includes(userId)) this.deps.log(`stream audio: restored userId=${userId} in Vesktop (volume ${String(volume)})`);
            }
            if (users.size === 0) this.applied.delete(conn);
        }
    }

    private noteRestoreFailure(userId: string, reason: string): void {
        if (this.restoreFailLogged.has(userId)) return;
        this.restoreFailLogged.add(userId);
        this.deps.log(`stream audio: could not restore userId=${userId} in Vesktop yet (${reason}); retrying`);
    }

    /**
     * 読めた一覧にない接続 (閉じた接続) の記録を捨てる。閉じた接続の音は鳴らないので、戻す必要もない。戻す相手が
     * いなくなった人の、戻せなかったことのログの記録も捨てる (次にまた戻せなかったときに書く)。
     */
    private pruneClosed(conns: readonly StreamConnectionLike[]): void {
        const open = new Set(conns);
        for (const conn of [...this.applied.keys()]) if (!open.has(conn)) this.applied.delete(conn);
        for (const conn of [...this.announced.keys()]) if (!open.has(conn)) this.announced.delete(conn);
        for (const userId of [...this.restoreFailLogged]) {
            if (![...this.applied.values()].some((users) => users.has(userId))) this.restoreFailLogged.delete(userId);
        }
    }

    private readConnections(): readonly StreamConnectionLike[] | null {
        const conns = this.safe(() => this.deps.streamConnections(), null);
        const available = conns !== null;
        if (available !== this.lastAvailable) {
            this.lastAvailable = available;
            if (!available) this.deps.log("stream audio: the screen share connections in Vesktop can't be read; leaving the volume as it is");
        }
        return conns;
    }

    private readSharing(): readonly SharingLike[] | null {
        return this.safe(() => this.deps.sharing(), null);
    }

    private safe<T>(fn: () => T, fallback: T): T {
        try {
            return fn();
        } catch {
            return fallback;
        }
    }
}

/** StreamVolumeDriver が使うタイマー (テストでは偽物を渡す)。 */
export interface StreamVolumeTimers {
    setTimeout(fn: () => void, ms: number): unknown;
    clearTimeout(handle: unknown): void;
    setInterval(fn: () => void, ms: number): unknown;
    clearInterval(handle: unknown): void;
}

/** Discord のメディアエンジンのうち、"connection" の知らせを購読する部分。 */
export interface ConnectionEventSource {
    on?(event: string, listener: (connection: unknown) => void): void;
    removeListener?(event: string, listener: (connection: unknown) => void): void;
}

/** StreamVolumeDriver の外とのつなぎ。 */
export interface StreamVolumeDriverDeps {
    readonly keeper: StreamVolumeKeeper;
    readonly timers: StreamVolumeTimers;
    /** "connection" の知らせを出すメディアエンジン。読めなければ null。 */
    engine(): ConnectionEventSource | null;
    /** 依存箇所 stream-volume が今使えるか (掛け始めたときに確認し、使えなければ recheck を呼ぶ)。 */
    available(): boolean;
    /** 依存箇所 stream-volume を確認し直す (adapter/vencord.ts の recheckOne)。今の処理の後に呼ぶ。 */
    recheck(): void;
}

/**
 * StreamVolumeKeeper をいつ動かすかを決める。
 *
 * - 置き直しは、知らせ (メディアエンジンの "connection"、USER_SETTINGS_PROTO_UPDATE、CONNECTION_OPEN、画面共有の変化) の
 *   後に setTimeout(0) で 1 回だけ行う。Discord は、その知らせの処理の中で保存した音量を接続に入れるので、どちらが先に
 *   動くかに頼らず、後から置き直す。
 * - 掛けている間と、戻せなかった人がいる間は、STREAM_VOLUME_TICK_MS ごとにも動かす (自前のタイマー。ほかの依存箇所の
 *   確認の周期には頼らない)。要らなくなったら止める。
 * - 掛け始めたときに依存箇所が使えなければ、確認し直しを今の処理の後に頼む (呼び出し元の処理の途中で、別の
 *   イベントを送らない)。メディアエンジンの "connection" を購読できていなければ、周期のたびに購読し直す。
 * - VoiceStateStore の selfStream を変える Flux のイベント (VOICE_STATE_UPDATES と PASSIVE_UPDATE_V2) と
 *   CONNECTION_OPEN を受けたとき、また adapter/vencord.ts の emitSharing (購読できないときの読み直しを含む) から
 *   observeSharing() が呼ばれたときに、掛けているかにかかわらず、利用者の操作の記録を人ごとの VoiceState と比べる。
 *   Vesktop のアカウントが通話にいない間も比べる。
 */
export class StreamVolumeDriver {
    private syncTimer: unknown = null;
    private tickTimer: unknown = null;
    private listenedEngine: ConnectionEventSource | null = null;
    private readonly onConnection = (): void => this.scheduleSync();

    constructor(private readonly deps: StreamVolumeDriverDeps) {}

    /** 掛けるかを受け取る (Adapter.setStreamSilence)。 */
    setWanted(on: boolean): void {
        const keeper = this.deps.keeper;
        const was = keeper.isWanted();
        if (on) this.ensureEngineListener();
        keeper.setWanted(on);
        if (on && !was && !this.deps.available()) this.deps.timers.setTimeout(() => this.deps.recheck(), 0);
        this.updateTicker();
    }

    /** 知らせの後の置き直しを、今の処理の後に 1 回だけ行う。要らなければ何もしない。 */
    scheduleSync(): void {
        if (this.syncTimer !== null || !this.deps.keeper.needsTicks()) return;
        this.syncTimer = this.deps.timers.setTimeout(() => {
            this.syncTimer = null;
            this.run();
        }, 0);
    }

    /**
     * Flux のイベントを受け取る。AUDIO_SET_LOCAL_VOLUME は画面共有の音量 (context が "stream") のときだけ利用者の操作として
     * 渡す。VOICE_STATE_UPDATES、PASSIVE_UPDATE_V2、CONNECTION_OPEN では利用者の操作の記録を観測と比べ、
     * USER_SETTINGS_PROTO_UPDATE と CONNECTION_OPEN では置き直す。ほかのイベントは見ない。Flux の購読者は、Discord の
     * ストアがそのイベントを処理した後に呼ばれるので、VoiceStateStore は新しい値になっている。
     */
    onFluxEvent(event: Readonly<Record<string, unknown>>): void {
        const type = event.type;
        if (type === "AUDIO_SET_LOCAL_VOLUME") {
            if (event.context === STREAM_CONTEXT && typeof event.userId === "string") {
                this.deps.keeper.noteUserVolumeChange(event.userId);
                this.updateTicker();
            }
        } else if (type === "VOICE_STATE_UPDATES" || type === "PASSIVE_UPDATE_V2") {
            this.deps.keeper.observeSharing();
        } else if (type === "USER_SETTINGS_PROTO_UPDATE") {
            this.scheduleSync();
        } else if (type === "CONNECTION_OPEN") {
            this.deps.keeper.observeSharing();
            this.scheduleSync();
        }
    }

    /** 画面共有の状態が変わったかもしれない。利用者の操作の記録を人ごとの VoiceState と比べる (掛けているかにかかわらず)。 */
    observeSharing(): void {
        this.deps.keeper.observeSharing();
    }

    /** 利用者の操作を受け取れるか (Flux の AUDIO_SET_LOCAL_VOLUME の購読に成功したか)。 */
    setUserChangesVisible(visible: boolean): void {
        this.deps.keeper.setUserChangesVisible(visible);
    }

    /**
     * 購読をやめるときに呼ぶ。戻せる人を戻し、タイマーとメディアエンジンの購読を外す。先に setWanted(false) で戻し損ねた
     * 人がいれば、周期を止める前に 1 回戻し直す (プラグインの stop() は、掛けるのをやめた後にこれを呼ぶ)。
     */
    dispose(): void {
        const keeper = this.deps.keeper;
        if (keeper.isWanted()) keeper.setWanted(false);
        else if (keeper.needsTicks()) keeper.sync();
        keeper.setUserChangesVisible(false);
        if (this.syncTimer !== null) {
            this.deps.timers.clearTimeout(this.syncTimer);
            this.syncTimer = null;
        }
        if (this.tickTimer !== null) {
            this.deps.timers.clearInterval(this.tickTimer);
            this.tickTimer = null;
        }
        if (this.listenedEngine !== null) {
            try {
                this.listenedEngine.removeListener?.("connection", this.onConnection);
            } catch {
                // 外せなくても止めない (scheduleSync は、要らなければ何もしない)。
            }
            this.listenedEngine = null;
        }
    }

    private run(): void {
        if (this.deps.keeper.isWanted()) this.ensureEngineListener();
        this.deps.keeper.sync();
        this.updateTicker();
    }

    private updateTicker(): void {
        const need = this.deps.keeper.needsTicks();
        if (need && this.tickTimer === null) {
            this.tickTimer = this.deps.timers.setInterval(() => this.run(), STREAM_VOLUME_TICK_MS);
        } else if (!need && this.tickTimer !== null) {
            this.deps.timers.clearInterval(this.tickTimer);
            this.tickTimer = null;
        }
    }

    private ensureEngineListener(): void {
        if (this.listenedEngine !== null) return;
        let engine: ConnectionEventSource | null;
        try {
            engine = this.deps.engine();
        } catch {
            engine = null;
        }
        if (engine === null || typeof engine.on !== "function") return;
        try {
            engine.on("connection", this.onConnection);
            this.listenedEngine = engine;
        } catch {
            // 購読できなくても、ほかの知らせと周期で置き直す。
        }
    }
}
