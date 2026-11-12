/*
 * sender.ts (test/tools/harness) は window.* / OffscreenCanvas に依存し vitest (Node) から直接 import
 * できないため、DOM に触れない純粋なロジックだけをここに切り出す。sender.ts はこのファイルを import
 * して使う (二重実装にしない)。
 *
 * - stream_id は送信側の中で一意でなければならない。エンコーダの確保は全ストリームで直列にする。
 * - sync.py はストリームごとの白フレームと 2 kHz のマーカーを対にして A/V のずれを測るため、どの
 *   ストリームにも同じ数のマーカーが出る必要がある。
 * - `waitUntil` は、送りすぎのときに待ちを外すと `next()` が微小タスクだけで回り、WebSocket の I/O が
 *   一度も進まず背圧を測れないので、目標時刻を過ぎていても `setTimeout(0)` を必ず 1 回挟む。1080p を
 *   240 フレーム毎秒で作れない以上、目標時刻を 1/4 にしただけでは常に即時解決の側に落ちる。
 *   `performance.now` と `setTimeout` しか使わず DOM に触れないので、ここでテストする。
 * - `streamHasSyncTone` / `parseSyncTone`: マーカーの音 (2 kHz) は 1 ストリームだけに乗せる。複数の
 *   ストリームのマーカーが混ざると判定スクリプト (sync.py) の対が読めず、対の数と値がばらつく。
 *   白のマーカーは全部の画面に出す (下の PerStreamWhiteFrameGate)。
 * - `buildHarnessFacts` / `parseFailContact`: --dock-scenario contact-missing (接点を 1 つ欠かす) と
 *   degraded (降格) のために、facts の組み立てをここに置く。降格は受け手の画面 (参加者ごとの「画質」の
 *   欄) が facts.stages からしか作られないので、段を載せない限り degraded は connected と同じ画面になる。
 * - `parseHelloVersion` / `harnessIdentityLogLine`: バージョンの食い違いの行の表示に関わる値の決め方を
 *   vitest からテストするために置く。
 * - `parseScreenNames` / `parseScreenImages` / `parseVoiceUsers` / `coverSourceRect` / `harnessAvatarGlyph`: 表示名と
 *   画像と声の持ち主を決めて、実際の OBS の画面を撮るときに、名前ラベルと話している人の光を実物らしく出すため。
 *
 * assignStreamIds と SyncCoordinatorPure / PerStreamWhiteFrameGate が守る性質:
 * - stream_id は画面と声を通して重複させない (assignStreamIds。受け手 (stream_router.h の
 *   std::map による後勝ち上書き) では、画面と声を別々に 1 始まりで振ると声の枠へ画面の映像が
 *   全部流れ込むため)。
 * - マーカーの白フレームはストリームごとに同じ 3 フレームが出る (SyncCoordinatorPure が境界を跨いだ回数 = boundary を
 *   配るだけにし、PerStreamWhiteFrameGate がストリームごとに「直近に見た境界」と「白の残数」を持つ。
 *   残数を全ストリームで共有すると、境界を跨いでもマーカーがストリームごとに均等に出ない)。
 */


import { AVATAR_MAX_BASE64_LENGTH } from "../../../src-vencord/core/avatar";
import { stageWireName, type LadderStage } from "../../../src-vencord/encode/ladder";
import type { AvatarMessage, ProfileEntry } from "../../../src-vencord/transport/control";

/** 経過ミリ秒の起点だけを見る形 (`sender.ts` の `Clock` はこの形を満たす)。 */
export interface ElapsedOrigin {
    /** `performance.now()` の目盛で測った起点。 */
    readonly startPerfMs: number;
}

/** targetElapsedMs (clock.startPerfMs からの経過ms) まで待つ。stopPromise が先に解決したら
 *  "stopped" を返す (frame_source.ts / audio_source.ts の「close()後のnext()はnullで解決する」
 *  契約を満たすため)。alwaysYield が真なら、目標時刻を過ぎていても setTimeout を1回挟んで
 *  イベントループを1周させる。 */
export function waitUntil(
    targetElapsedMs: number,
    clock: ElapsedOrigin,
    stopPromise: Promise<void>,
    alwaysYield = false,
): Promise<"reached" | "stopped"> {
    const waitMs = targetElapsedMs - (performance.now() - clock.startPerfMs);
    if (waitMs <= 0 && !alwaysYield) return Promise.resolve("reached");
    return Promise.race([
        new Promise<"reached">((resolve) => setTimeout(() => resolve("reached"), Math.max(0, waitMs))),
        stopPromise.then((): "stopped" => "stopped"),
    ]);
}

/** 画面 (screen) と声 (voice) で 1 つの通し番号にする (stream_id は送信側全体で一意)。
 *  screenIds は 1..nScreen、voiceIds はその続き (nScreen+1..nScreen+nVoice)。 */
export function assignStreamIds(nScreen: number, nVoice: number): { screenIds: number[]; voiceIds: number[] } {
    const screenIds: number[] = [];
    for (let i = 0; i < nScreen; i++) screenIds.push(i + 1);
    const voiceIds: number[] = [];
    for (let j = 0; j < nVoice; j++) voiceIds.push(nScreen + j + 1);
    return { screenIds, voiceIds };
}

/** マーカーの音 (2 kHz) をどのストリームに乗せるか。既定は "screen"。 */
export type SyncTone = "screen" | "voice" | "none";

/** 環境変数などの文字列を SyncTone に修正する。知らない値と未指定は既定の "screen" にする。 */
export function parseSyncTone(value: unknown): SyncTone {
    if (value === "voice") return "voice";
    if (value === "none") return "none";
    return "screen";
}

/**
 * このストリームがマーカーの音を持つか。"screen" なら最初の画面のストリームだけ、"voice" なら
 * 最初の声のストリームだけが 2 kHz を出し、他のストリームは無音にする (sender.ts の HarnessAudioSource。判定スクリプトが録画のミックスで 2 kHz と 1 kHz を比べるため)。"none" はどのストリームも出さない。
 * indexInKind はその種別の中での通し番号 (0 始まり)。
 */
export function streamHasSyncTone(syncTone: SyncTone, kind: "screen" | "voice", indexInKind: number): boolean {
    if (syncTone === "none") return false;
    return syncTone === kind && indexInKind === 0;
}

export interface SyncEvent {
    readonly timestampUs: number;
    /** フレームを止めている間に跨いだ境界で、再開のときに白のフレームを出さなかった
     *  (マーカーの音はその時刻に鳴っている)。A/V の対からは外す。無ければ白を出した (既定どおり)。 */
    readonly whiteSkippedAfterPause?: true;
}

/** マーカー (白3フレーム+2kHz 60ms) の境界そのものを管理する。境界を跨いだ回数 (currentBoundary) を
 *  配るだけで、「どのストリームが何フレーム白にすべきか」は呼び出し元 (PerStreamWhiteFrameGate) が持つ。
 *  複数の映像ストリームが同じ境界を共有して呼んでも、跨いだ判定は1回だけ進む。
 *
 *  **境界は白のフレームもマーカーの音もメディアのタイムスタンプ (生成の予定時刻) だけで決める**。
 *  映像の生成が実時間で境界を跨いだ時に音のマーカーの区間を立てる方式では、音の生成が映像より遅れると
 *  2 kHz が遅れた位置から鳴り始め、進んでいると区間が立つまで鳴らないので、測った A/V のずれに送信側の中の
 *  生成の先後がそのまま乗る。そこで epochUs0 からのタイムスタンプが everySec の倍数を跨いだフレームを白にし、同じ倍数から
 *  60 ms の音を 2 kHz にする。生成がどれだけ遅れても、白とマーカーの音のタイムスタンプは揃う (製品はタイムスタンプで並べるので、
 *  出口のずれは製品の分だけになる)。 */
export class SyncCoordinatorPure {
    private boundaryIndex = 0;
    private readonly everyUs: bigint;
    readonly events: SyncEvent[] = [];

    constructor(
        private readonly everySec: number,
        private readonly epochUs0: bigint = 0n,
    ) {
        this.everyUs = everySec > 0 ? BigInt(Math.round(everySec * 1_000_000)) : 0n;
    }

    /** 映像ループがフレームを描く直前に、そのフレームのタイムスタンプで呼ぶ。タイムスタンプが次の境界を跨いだ (誰が呼んでも)
     *  最初の1回だけ boundaryIndex を進める。 */
    noteVideoTick(timestampUs: bigint): void {
        if (this.everyUs <= 0n) return;
        const media = timestampUs - this.epochUs0;
        if (media < this.everyUs) return;
        const index = Number(media / this.everyUs);
        if (index <= this.boundaryIndex) return;
        this.boundaryIndex = index;
        this.events.push({ timestampUs: Number(this.epochUs0 + BigInt(index) * this.everyUs) });
    }

    /** 直近に跨いだ境界の通し番号 (0 なら未到達)。ストリームごとの gate はこの値の変化を見て
     *  自分の白フレームの残数を リセットする。 */
    get currentBoundary(): number {
        return this.boundaryIndex;
    }

    /** このタイムスタンプの音声トーン周波数 (通常1000Hz、境界から60msの間だけ2000Hz)。映像の生成の進み具合に依らない。 */
    audioFrequencyAt(timestampUs: bigint): number {
        if (this.everyUs <= 0n) return 1000;
        const media = timestampUs - this.epochUs0;
        if (media < this.everyUs) return 1000;
        return media % this.everyUs < 60_000n ? 2000 : 1000;
    }
}

/** ストリーム (HarnessVideoFrameSource) ごとに持つ、白フレームの残数の状態機械。SyncCoordinatorPure の
 *  currentBoundary が自分の知っている値より進んでいたら、新しい境界に入ったとみなして
 *  残数を3にリセットする。これにより、映像が何ストリームあっても、どのストリームにも同じ3フレームの白が出る。 */
export class PerStreamWhiteFrameGate {
    private lastBoundarySeen = 0;
    private whiteRemaining = 0;

    /** フレームを1つ描く直前に呼ぶ。このフレームを白にすべきなら true (呼ぶたびに残数を1つ消費する)。 */
    consume(currentBoundary: number): boolean {
        if (currentBoundary > this.lastBoundarySeen) {
            this.lastBoundarySeen = currentBoundary;
            this.whiteRemaining = 3;
        }
        if (this.whiteRemaining > 0) {
            this.whiteRemaining--;
            return true;
        }
        return false;
    }

    /** この境界までを「もう見た」にし、残っている白を捨てる (止めていた間に過ぎたマーカーを、
     *  再開の後に遅れて白にしないため)。前へは戻さない。 */
    skipTo(boundary: number): void {
        if (boundary > this.lastBoundarySeen) this.lastBoundarySeen = boundary;
        this.whiteRemaining = 0;
    }
}

/**
 * フレームを止めていた材料が再開する最初のフレームで、マーカーの白を決め直す 。
 * 止めている間も音は鳴り続け、マーカーの 2 kHz はその時刻に出ている。白のフレームはフレームを作るときにしか出ないので、
 * 何もしないと再開の最初の 3 フレームが、とうに過ぎた境界の白になる (判定スクリプトが数秒前の音と対にしてしまう)。
 * - 再開のフレームのタイムスタンプで境界を進め (`noteVideoTick`)、現在の境界の時刻がこのフレームの 1 フレーム前より後なら
 *   (= このフレームが境界の後の最初のフレーム)、白は正しいので触らない (`"kept"`)。
 * - そうでなければ、その境界までを見たことにして白を出さない (`"skipped"`)。この呼び出しで新しく記録された
 *   境界には `whiteSkippedAfterPause` を付ける (ほかのストリームが先に記録していた境界は、そのストリームが白を出している)。
 * - 境界がまだ 1 つも無ければ何もしない (`"none"`)。
 */
export function settleSyncAfterPause(
    sync: SyncCoordinatorPure,
    gate: PerStreamWhiteFrameGate,
    resumedTimestampUs: bigint,
    frameDurationUs: bigint,
): "none" | "kept" | "skipped" {
    const before = sync.events.length;
    sync.noteVideoTick(resumedTimestampUs);
    const last = sync.events[sync.events.length - 1];
    if (last === undefined) return "none";
    if (BigInt(last.timestampUs) > resumedTimestampUs - frameDurationUs) return "kept";
    gate.skipTo(sync.currentBoundary);
    if (sync.events.length > before) sync.events[sync.events.length - 1] = { ...last, whiteSkippedAfterPause: true };
    return "skipped";
}

/** facts.stages の1件。 */
export interface HarnessStageFact {
    stream_id: number;
    stage: string;
}

/** テスト用の送信側が送る facts (必須の 3 つの鍵 + 任意の stages。
 *  roster は通話に参加している人が 1 人以上いるときだけ載せる = いなければ鍵ごと省く)。 */
export interface HarnessFacts {
    warnings: string[];
    sharing: { count: number; user_ids: string[] };
    /** 受け手から届いた録画の現在値 (製品の core/facts.ts と同じ)。値に関わらず必ず載せる。 */
    recording: boolean;
    /** 受け手から届いた media の現在値 (製品の core/facts.ts と同じ)。値に関わらず必ず載せる。 */
    media: boolean;
    stages?: HarnessStageFact[];
    /** 通話に参加している人。テスト用の送信側では、画面共有の人と声だけの人の全員。 */
    roster?: { count: number; user_ids: string[] };
}

/** facts に必ず載せる 2 つの現在値 (受け手から届いた `recording` と `media`)。 */
export interface HarnessGateFacts {
    recording: boolean;
    media: boolean;
}

/** 段を報告するストリーム 1 つ (エンコーダを持つ画面共有のストリームだけ。声は対象外)。 */
export interface HarnessStageEntry {
    streamId: number;
    stage: LadderStage;
}

/** 画面の状態のうち送信側で作る 2 つの値
 *  (接点を 1 つ欠かす = --dock-scenario contact-missing、降格 = --dock-scenario degraded)
 *  と、**いま画面共有している人** (`sharing`) の facts を組み立てる。
 *
 *  - `contactId` があれば、その接点だけを Missing として warnings に載せる。接点idの妥当性は
 *    ここでは検査しない (facts は ws/ の層ではopaque。どの接点idを選ぶかは
 *    test/tools/obs-run/run-window-plan.mjs の DOCK_VIEW_FAIL_CONTACT_ID が決める)。
 *  - `stages` があれば、ストリームごとの現在の段を stages に載せる。段の綴りは製品と同じ
 *    `encode/ladder.ts` の `stageWireName` を通す (綴りを2か所で決めない)。並びは stream_id の
 *    昇順にする (`src-vencord/core/facts.ts` と同じ。呼び出し元の集め方で並びが変わらない
 *    ようにする)。報告する段が1件も無ければ鍵ごと省く。
 *  - `sharingUserIds` は画面共有している人の userId (昇順にして重複を落とす)。**製品は
 *    `src-vencord/core/facts.ts` が `state.sharing` からこれを必ず載せている**ので、テスト用の
 *    送信側も同じように載せる。0 人のまま送ると、受け手の参加者一覧は roster と sharing から人の行を作る
 *    (`src-obs/dock/build_observation.cpp` の people_of) ので、0 人のままだと**枠に入って
 *    いない共有者が 1 行も出ない**。誰を載せるかは手動で、その行の [枠に載せる] を押すところから
 *    操作が始まるため、ここを製品と同じ形にそろえる必要がある。
 *
 *  - `gate` の `recording` と `media` は**値に関わらず必ず載せる** (製品の
 *    `src-vencord/core/facts.ts` と同じ。受け手は facts の `recording` を自分が送った
 *    値と照らし、食い違えばドックに「録画していることが Discord 側に届いていません」を出す。
 *    載せないと、受け手側でこのメッセージが出る)。製品も繋がるたびに必ず送る。
 *
 *  画面のストリームが 1 つでもあれば、繋がるたびに facts を 1 通送る。受け手の接点の行は
 *  「まだ受け取っていない」から「見つかっている」へ変わる。製品も繋がるたびに facts を
 *  必ず送るので、この動きのほうが実態に近い。 */
export function buildHarnessFacts(
    contactId: string | undefined,
    stages: readonly HarnessStageEntry[],
    sharingUserIds: readonly string[],
    gate: HarnessGateFacts,
    rosterUserIds: readonly string[] = [],
): HarnessFacts {
    const sharing = [...new Set(sharingUserIds.filter((id) => id.length > 0))].sort();
    const facts: HarnessFacts = {
        warnings: contactId === undefined ? [] : [contactId],
        sharing: { count: sharing.length, user_ids: sharing },
        recording: gate.recording,
        media: gate.media,
    };
    // 製品は通話の参加者を読めているときだけ roster を入れる。人が 1 人もいない間は、分からない状態と同じに省く。
    const roster = sortedUniqueUserIds(rosterUserIds);
    if (roster.length > 0) facts.roster = { count: roster.length, user_ids: roster };
    if (stages.length > 0) {
        facts.stages = [...stages]
            .sort((a, b) => a.streamId - b.streamId)
            .map((e) => ({ stream_id: e.streamId, stage: stageWireName(e.stage) }));
    }
    return facts;
}

/** 環境変数などの文字列を failContact に修正する。空文字列と未指定はどちらも「送らない」
 *  (undefined) に倒す。 */
export function parseFailContact(value: unknown): string | undefined {
    return typeof value === "string" && value.length > 0 ? value : undefined;
}

/** `parseHelloVersion` が通す文字列の長さの上限 (任意の値。`<version>+<短いコミット>` に十分な長さ)。 */
export const HELLO_VERSION_MAX_LENGTH = 64;

/**
 * テスト用の送信側が hello の `version` に載せる文字列を決める。
 * 環境変数 (`HARNESS_HELLO_VERSION`) の値が空でなく、表示できる ASCII (0x21-0x7e) だけで
 * `HELLO_VERSION_MAX_LENGTH` 文字以内ならそれを使い、それ以外 (未指定・空・空白や制御文字・長すぎ) は
 * `fallback` (呼び出し元は `src-vencord/version.ts` の `CORDMUX_BUILD_ID` を渡す) に倒す。
 * 文字の範囲を絞るのは、受け手がこの文字列を同梱の印の `build_id` と比べ、
 * ドックの画面にそのまま並べるため (印の `build_id` も同じ範囲の文字でできている)。
 */
export function parseHelloVersion(value: unknown, fallback: string): string {
    if (typeof value !== "string") return fallback;
    if (value.length === 0 || value.length > HELLO_VERSION_MAX_LENGTH) return fallback;
    for (let i = 0; i < value.length; i++) {
        const code = value.charCodeAt(i);
        if (code < 0x21 || code > 0x7e) return fallback;
    }
    return value;
}

/** テスト用の送信側の名。 */
export const HARNESS_IDENTITY = "cordmux-harness";

/**
 * 確立のたびに `log` で送る 1 行。hello の `version` は製品と同じ文字列なので、OBS のログで実際の Vencord からの
 * 接続と見分ける手がかりはこの行だけになる
 * (受け手は `log` の行をトークンの監視を通して OBS のログへ出す = src-obs/pipeline/pipeline.cpp の
 * handle_log)。表示できる ASCII だけで書く。
 */
export function harnessIdentityLogLine(helloVersion: string): string {
    return `${HARNESS_IDENTITY}: test sender (not the Vencord plugin), hello version=${helloVersion}`;
}

/** 走行中に解像度を変える予定の 1 件。`atSec` は送信側の起動からの秒数。 */
export interface ResolutionChange {
    atSec: number;
    width: number;
    height: number;
}

/**
 * 走行中の解像度の変更の予定表を読む。綴りは
 * `"<起動からの秒数>:<幅>x<高さ>"` をカンマで並べたもの (例: `"35:1280x720,60:640x360"`)。
 * 綴りを決めるのは実行スクリプトの側 (`test/tools/obs-run/run-window-plan.mjs` の
 * `resolutionScheduleArg`) で、読むのはこちらである。
 *
 * **読めない項目は黙って落とす**: 予定表はテスト用の映像を作るための指示であって、送信側の
 * 生死に関わる値ではない。1 項目が壊れていても残りの予定と本来の送信は続けるほうが、
 * 実行全体を失敗させるより得られるものが多い (落とした数は呼び出し元が `length` の差で読める)。
 * 並びは時刻の昇順に修正する (実行スクリプトが順不同で渡しても同じ動きにするため)。
 */
export function parseResolutionSchedule(value: unknown): ResolutionChange[] {
    if (typeof value !== "string" || value.length === 0) return [];
    const out: ResolutionChange[] = [];
    for (const rawItem of value.split(",")) {
        const item = rawItem.trim();
        if (item.length === 0) continue;
        const colon = item.indexOf(":");
        if (colon <= 0) continue;
        const atSec = Number(item.slice(0, colon));
        const size = parseScreenSize(item.slice(colon + 1));
        if (size === null) continue;
        if (!Number.isFinite(atSec) || atSec < 0) continue;
        out.push({ atSec, width: size.width, height: size.height });
    }
    return out.sort((a, b) => a.atSec - b.atSec);
}

/** 画面のストリームの寸法 (px)。 */
export interface ScreenSize {
    width: number;
    height: number;
}

/**
 * `"<幅>x<高さ>"` を 1 つ読む (予定表とストリームごとの寸法の両方がここを通り、同じ綴りを受ける)。
 * 大文字の X と前後の空白を許す。幅と高さは正の整数だけ。読めなければ null。
 */
export function parseScreenSize(value: string): ScreenSize | null {
    const size = value.trim().toLowerCase().split("x");
    if (size.length !== 2) return null;
    const width = Number(size[0]);
    const height = Number(size[1]);
    if (!Number.isInteger(width) || !Number.isInteger(height) || width <= 0 || height <= 0) return null;
    return { width, height };
}

/**
 * 画面のストリームごとの最初の寸法を読む (720p と 2160p の枠を同じ OBS に並べて
 * 名前ラベルの板を比べるため)。綴りは `"<幅>x<高さ>"` を
 * カンマで並べたもの (例: `"1280x720,3840x2160"`)。**i 番目の項目が i 番目の画面のストリーム** (stream id は
 * i + 1。`assignStreamIds` と同じ並び) に適用される。
 *
 * - 項目がストリームの数より少なければ、残りのストリームは `fallback` (既定の 1920x1080)。
 * - **読めない項目と空の項目はそのストリームだけ `fallback`** にする。予定表のように落として詰めると、
 *   後ろの項目が別のストリームへずれて「どの枠が何 px か」が実行の記録と食い違うため。空の項目は
 *   「このストリームは既定のまま」と書く手段にもなる (`",3840x2160"` = 2 ストリーム目だけ 2160p)。
 * - ストリームの数より多い項目は使わない。
 * - 未指定・空文字列・非文字列は全部のストリームが `fallback` (既定どおり)。
 *
 * 走行中の予定表 (`parseResolutionSchedule`) はこれまでどおり全部のストリームに適用され、ここで決めた
 * 寸法を上書きする。返す並びの長さは常に `count` (負や非整数は 0 ストリーム)。
 */
export function parseScreenSizes(value: unknown, count: number, fallback: ScreenSize): ScreenSize[] {
    const n = Number.isInteger(count) && count > 0 ? count : 0;
    const items = typeof value === "string" && value.length > 0 ? value.split(",") : [];
    const out: ScreenSize[] = [];
    for (let i = 0; i < n; i++) {
        const item = items[i] ?? "";
        const size = item.trim().length > 0 ? parseScreenSize(item) : null;
        out.push(size ?? { width: fallback.width, height: fallback.height });
    }
    return out;
}

/** テスト用の参加者の userId として受ける形 (avatar_export_file.js がファイル名にする文字と同じ)。 */
export const HARNESS_USER_ID_RE = /^[A-Za-z0-9_-]{1,64}$/;

/** 表示名として受ける長さの上限 (コードポイントの数)。任意の値で、名前ラベルの省略を試すのに十分な長さ。 */
export const HARNESS_DISPLAY_NAME_MAX = 64;

/** カンマで区切った値を、i 番目の項目が i 番目のストリームになるように count 個に分ける (足りない分は空の文字列)。 */
function splitPerStream(value: unknown, count: number): string[] {
    const n = Number.isInteger(count) && count > 0 ? count : 0;
    const items = typeof value === "string" && value.length > 0 ? value.split(",") : [];
    const out: string[] = [];
    for (let i = 0; i < n; i++) out.push((items[i] ?? "").trim());
    return out;
}

/**
 * 画面のストリームごとの表示名を読む (`HARNESS_SCREEN_NAMES`)。綴りは表示名をカンマで並べたもので、i 番目の項目が
 * i 番目の画面のストリーム (`harness-screen-<i + 1>`) の profiles の display_name になる。
 * - 前後の空白を落とす。空の項目、制御文字を含む項目、`HARNESS_DISPLAY_NAME_MAX` を超える項目は null
 *   (そのストリームは userId のまま)。読めない項目を落として詰めないのは `parseScreenSizes` と同じ理由。
 * - 項目がないストリームは null。ストリームの数より多い項目は使わない。返す長さは常に `count`。
 */
export function parseScreenNames(value: unknown, count: number): (string | null)[] {
    return splitPerStream(value, count).map((item) => {
        if (item.length === 0 || Array.from(item).length > HARNESS_DISPLAY_NAME_MAX) return null;
        for (const ch of item) {
            const code = ch.codePointAt(0) ?? 0;
            if (code < 0x20 || code === 0x7f) return null;
        }
        return item;
    });
}

/**
 * 画面のストリームごとに描く画像のファイルの場所を読む (`HARNESS_SCREEN_IMAGES`)。綴りはファイルの場所をカンマで並べたもので、
 * i 番目の項目が i 番目の画面のストリームになる (カンマを含む場所は書けない)。前後の空白を落とし、空の項目と項目がない
 * ストリームは null (そのストリームは動く図形を描く)。場所の正しさは見ない (読むのは主プロセスで、読めなければ動く図形に戻る)。
 * 返す長さは常に `count`。
 */
export function parseScreenImages(value: unknown, count: number): (string | null)[] {
    return splitPerStream(value, count).map((item) => (item.length > 0 ? item : null));
}

/**
 * 声のストリームごとの userId を読む (`HARNESS_VOICE_USERS`)。綴りは userId をカンマで並べたもので、i 番目の項目が i 番目の声の
 * ストリーム (stream id は `voiceIds[i]`) になる。画面共有している人の userId (`harness-screen-<n>`) を書くと、その人が話す
 * 声になり、その人の枠に話している人の光が出る。
 * - 項目がない、空、`HARNESS_USER_ID_RE` に合わない項目は既定の `harness-voice-<stream id>`。
 * - 前の声のストリームが使った userId と、ほかの声のストリームの既定の userId は使わずに既定にする (1 人に声を 2 本持たせない)。
 */
export function parseVoiceUsers(value: unknown, voiceIds: readonly number[]): string[] {
    const items = splitPerStream(value, voiceIds.length);
    const defaults = voiceIds.map((id) => `harness-voice-${id}`);
    const used = new Set<string>();
    const out: string[] = [];
    for (const [i, item] of items.entries()) {
        const fallback = defaults[i] ?? "";
        const otherDefault = defaults.some((d, j) => j !== i && d === item);
        const userId = HARNESS_USER_ID_RE.test(item) && !used.has(item) && !otherDefault ? item : fallback;
        used.add(userId);
        out.push(userId);
    }
    return out;
}

/**
 * 画像を枠いっぱいに描くときに使う、画像の中の矩形。縦横比を保って枠を覆う大きさに拡大し、はみ出た分を中央で切る
 * (src-obs/tests/decor_preview の `--tile-images` と同じ置き方)。寸法のどれかが 0 以下なら画像の全体を返す。
 */
export function coverSourceRect(
    srcWidth: number,
    srcHeight: number,
    dstWidth: number,
    dstHeight: number,
): { x: number; y: number; width: number; height: number } {
    if (!(srcWidth > 0 && srcHeight > 0 && dstWidth > 0 && dstHeight > 0)) {
        return { x: 0, y: 0, width: Math.max(0, srcWidth), height: Math.max(0, srcHeight) };
    }
    if (srcWidth * dstHeight > dstWidth * srcHeight) {
        const width = (srcHeight * dstWidth) / dstHeight;
        return { x: (srcWidth - width) / 2, y: 0, width, height: srcHeight };
    }
    const height = (srcWidth * dstHeight) / dstWidth;
    return { x: 0, y: (srcHeight - height) / 2, width: srcWidth, height };
}

/**
 * テスト用の参加者のアバターに描く 1 文字。表示名があればその先頭の 1 文字 (コードポイント)、なければ userId の末尾の 1 文字。
 * 名前ラベルに並ぶアバターが、その人の名前と結び付いて見えるようにする。
 */
export function harnessAvatarGlyph(userId: string, displayName?: string | null): string {
    if (typeof displayName === "string" && displayName.length > 0) return Array.from(displayName)[0] ?? "";
    return userId.slice(-1);
}

/**
 * フレームだけを止める通知ファイルの場所を読む。
 * 前後の空白を落として空でなければその文字列、それ以外 (未指定・空・空白だけ・文字列でない) は undefined
 * (= この機能を使わない。既定どおり)。場所の正しさはここでは見ない (ないファイルは「止めない」と同じ)。
 */
export function parseFramePauseFile(value: unknown): string | undefined {
    if (typeof value !== "string") return undefined;
    const trimmed = value.trim();
    return trimmed.length > 0 ? trimmed : undefined;
}

/**
 * 主プロセスが通知ファイルを読んだ結果 (`frame_pause_file.js` の `readFramePauseState` が返す形)。
 * - `off`: 場所が渡されていない (この機能を使わない)。
 * - `absent`: ファイルがない (= どのストリームも止めない)。
 * - `present`: ファイルがある。`text` は中身の先頭 (止めるストリームの並び。空なら全部の画面のストリーム)。
 * - `error`: あるらしいが読めなかった (書いている途中など)。前の状態を保つ。
 * `wallMs` は主プロセスが読んだ時刻 (`Date.now()`。OBS の記録の時刻と比べるため)。
 */
export interface FramePauseFileState {
    state: "off" | "absent" | "present" | "error";
    text?: string;
    wallMs: number;
}

/**
 * 通知ファイルの中身から、止める画面のストリーム (stream id) を決める。
 * - 中身が空か空白だけなら、全部の画面のストリームを止める (1 ストリームだけのときは中身を書かずに済む)。
 * - そうでなければ、カンマか空白で区切った stream id の並び (`"2"`・`"1,2"`)。画面のストリームの id でない項目
 *   (声のストリーム・ないストリーム・数でない文字列) は止めずに `ignored` に返す。**読める項目が 1 つも無ければ何も止めない**
 *   (書き損じで全部のストリームを黙って止めると、実行の記録が「1 人だけ止めた」と食い違うため)。
 * 返す `streams` は昇順で重複なし。
 */
export function parseFramePauseTargets(
    text: string,
    screenIds: readonly number[],
): { streams: number[]; ignored: string[] } {
    const tokens = text.split(/[\s,]+/).filter((t) => t.length > 0);
    if (tokens.length === 0) return { streams: [...screenIds].sort((a, b) => a - b), ignored: [] };
    const known = new Set(screenIds);
    const streams = new Set<number>();
    const ignored: string[] = [];
    for (const token of tokens) {
        // `stuck:N` は parseFramePauseStuck が読む (読み出しが止まった形)。ここでは止めるストリームにも、読めない
        // 項目にも数えない。
        if (STUCK_TOKEN_RE.test(token)) continue;
        const id = /^[0-9]+$/.test(token) ? Number(token) : NaN;
        if (known.has(id)) streams.add(id);
        else ignored.push(token);
    }
    return { streams: [...streams].sort((a, b) => a - b), ignored };
}

const STUCK_TOKEN_RE = /^stuck:([0-9]+)$/;

/**
 * 通知ファイルの中身から、読み出しが止まった形で止める画面のストリーム (`stuck:N` の項目) を決める。製品の送信側で、
 * Discord には映像が届いているのに cordmux の読み出しかエンコードが止まった状態の代わりである。`N` だけの項目
 * (Discord が送っていない形) と違い、受信側の stream_check に「始め直した」と答えると映像が戻る
 * (FramePauseLedger.release)。画面のストリームの id でない項目は返さない。返す値は昇順で重複なし。
 */
export function parseFramePauseStuck(text: string, screenIds: readonly number[]): number[] {
    const known = new Set(screenIds);
    const out = new Set<number>();
    for (const token of text.split(/[\s,]+/)) {
        const m = STUCK_TOKEN_RE.exec(token);
        if (m === null) continue;
        const id = Number(m[1]);
        if (known.has(id)) out.add(id);
    }
    return [...out].sort((a, b) => a - b);
}

/** 受信側の stream_check に答えた記録 1 つ。 */
export interface FramePauseCheck {
    /** 送信側の起動からのミリ秒。 */
    atMs: number;
    streamId: number;
    outcome: "restarted" | "not_receiving" | "sending";
}

/** フレームを止めた区間 1 つ (ストリームごと)。`endMs` が null の区間は走行の終わりまで止まっていた。 */
export interface FramePauseInterval {
    streamId: number;
    /** 送信側の起動からのミリ秒 (結果 JSON のほかの `atMs` と同じ目盛)。 */
    startMs: number;
    /** 主プロセスが通知ファイルを読んだシステム時刻 (`Date.now()`)。 */
    startWallMs: number;
    endMs: number | null;
    endWallMs: number | null;
    /** この区間で作らなかったフレームの数 (再開のときに飛ばした番号の数)。 */
    framesWithheld: number;
}

/**
 * フレームだけを止める仕組みの状態と記録。**送信側に 1 つだけ作り、全部の映像の材料が共有する**
 * (解像度の変更と `media` の開き直しで材料を作り直しても、止めている状態が落ちないように)。
 * DOM に触れない (vitest から当てる)。
 */
export class FramePauseLedger {
    readonly intervals: FramePauseInterval[] = [];
    /** 中身にあって画面のストリームの id でなかった項目 (重複なし、見た順)。 */
    readonly ignoredTargets: string[] = [];
    /** `error` を受けた回数 (前の状態を保った回数)。 */
    readErrors = 0;
    /** 受信側の stream_check に答えた記録 (古い順)。 */
    readonly checks: FramePauseCheck[] = [];
    private readonly paused = new Set<number>();
    private readonly open = new Map<number, FramePauseInterval>();
    private readonly waiters = new Map<number, (() => void)[]>();
    /** 通知ファイルが `N` で止めているストリーム (Discord が送っていない形)。 */
    private plain = new Set<number>();
    /** 通知ファイルが `stuck:N` で止めているストリーム (読み出しが止まった形)。 */
    private stuck = new Set<number>();
    /** `stuck:N` のうち、stream_check で始め直したストリーム。ファイルから `stuck:N` が消えるまで止めない。 */
    private readonly released = new Set<number>();
    private lastWallMs = 0;

    constructor(private readonly screenIds: readonly number[]) {}

    /** 主プロセスが読んだ結果を当てる。止めるストリームの組が変わったストリームだけ、区間を開くか閉じる。 */
    apply(file: FramePauseFileState, atMs: number): void {
        if (file.state === "off") return;
        if (file.state === "error") {
            this.readErrors++;
            return;
        }
        let next: number[] = [];
        let stuck: number[] = [];
        if (file.state === "present") {
            const parsed = parseFramePauseTargets(file.text ?? "", this.screenIds);
            next = parsed.streams;
            stuck = parseFramePauseStuck(file.text ?? "", this.screenIds);
            for (const token of parsed.ignored) {
                if (!this.ignoredTargets.includes(token)) this.ignoredTargets.push(token);
            }
        }
        this.plain = new Set(next);
        this.stuck = new Set(stuck);
        for (const id of [...this.released]) {
            if (!this.stuck.has(id)) this.released.delete(id);
        }
        this.lastWallMs = file.wallMs;
        this.applyPaused(atMs, file.wallMs);
    }

    /**
     * 受信側の stream_check に答える。`stuck:N` で止めているストリームは始め直したことにして止めるのを解き
     * (`restarted`)、`N` で止めているストリームは Discord が送っていない形なので解かない (`not_receiving`)。止めて
     * いなければ `sending`。答えは checks に残す。
     */
    release(streamId: number, atMs: number, wallMs: number = this.lastWallMs): FramePauseCheck["outcome"] {
        let outcome: FramePauseCheck["outcome"] = "sending";
        if (this.plain.has(streamId)) outcome = "not_receiving";
        else if (this.stuck.has(streamId) && !this.released.has(streamId)) {
            this.released.add(streamId);
            this.applyPaused(atMs, wallMs);
            outcome = "restarted";
        }
        this.checks.push({ atMs, streamId, outcome });
        return outcome;
    }

    /** plain と、まだ始め直していない stuck を止めるストリームにして、変わったストリームだけ区間を開くか閉じる。 */
    private applyPaused(atMs: number, wallMs: number): void {
        const nextSet = new Set<number>(this.plain);
        for (const id of this.stuck) {
            if (!this.released.has(id)) nextSet.add(id);
        }
        const next = [...nextSet].sort((a, b) => a - b);
        const file = { wallMs };
        for (const id of [...this.paused]) {
            if (nextSet.has(id)) continue;
            this.paused.delete(id);
            const interval = this.open.get(id);
            if (interval) {
                interval.endMs = atMs;
                interval.endWallMs = file.wallMs;
                this.open.delete(id);
            }
            const list = this.waiters.get(id) ?? [];
            this.waiters.delete(id);
            for (const resolve of list) resolve();
        }
        for (const id of next) {
            if (this.paused.has(id)) continue;
            this.paused.add(id);
            const interval: FramePauseInterval = {
                streamId: id,
                startMs: atMs,
                startWallMs: file.wallMs,
                endMs: null,
                endWallMs: null,
                framesWithheld: 0,
            };
            this.intervals.push(interval);
            this.open.set(id, interval);
        }
    }

    isPaused(streamId: number): boolean {
        return this.paused.has(streamId);
    }

    /** そのストリームが止まっていなければすぐ、止まっていれば止めるのを解いた時に解決する。 */
    whenResumed(streamId: number): Promise<void> {
        if (!this.paused.has(streamId)) return Promise.resolve();
        return new Promise((resolve) => {
            const list = this.waiters.get(streamId) ?? [];
            list.push(resolve);
            this.waiters.set(streamId, list);
        });
    }

    /** 作らなかったフレームの数を、そのストリームの直近の区間に足す (再開の直後に呼ぶので、区間はもう閉じている)。 */
    addWithheld(streamId: number, frames: number): void {
        if (!(frames > 0)) return;
        for (let i = this.intervals.length - 1; i >= 0; i--) {
            const interval = this.intervals[i];
            if (interval && interval.streamId === streamId) {
                interval.framesWithheld += frames;
                return;
            }
        }
    }
}

/**
 * 止めていた材料が再開するときに、次に作るフレームの番号を決める。
 * 実物の取り込みは止まっていた間のフレームを後から作らないので、現在の時刻のフレームへ飛ぶ。
 * `paceDivisor` は送りすぎのときの 4 (目標時刻を 1/4 にしている分だけ番号も速く進む)。
 * 飛ばした数 (`withheld`) は 0 以上。現在の番号より前へは戻さない。
 */
export function frameIndexAfterPause(
    frameIndex: number,
    elapsedNowMs: number,
    fps: number,
    paceDivisor: number,
): { frameIndex: number; withheld: number } {
    const caughtUp = Math.floor((elapsedNowMs * fps * paceDivisor) / 1000);
    if (!(caughtUp > frameIndex)) return { frameIndex, withheld: 0 };
    return { frameIndex: caughtUp, withheld: caughtUp - frameIndex };
}

/** テスト用の参加者の表示名と avatar_key の材料にする、userId の 32 bit のハッシュ (FNV 法)。 */
function hashUserId(userId: string): number {
    let h = 0x811c9dc5;
    for (let i = 0; i < userId.length; i++) {
        h ^= userId.charCodeAt(i);
        h = Math.imul(h, 0x01000193) >>> 0;
    }
    return h >>> 0;
}

/** テスト用の参加者の avatar_key。userId が同じなら必ず同じ値で、userId が違えば (ほぼ) 違う値になる。 */
export function harnessAvatarKey(userId: string): string {
    return `harness-${hashUserId(userId).toString(16).padStart(8, "0")}`;
}

/** テスト用の参加者のアバターの色相 (0 以上 360 未満)。userId ごとに違う色にして、並びの中で見分けられるようにする。 */
export function harnessAvatarHue(userId: string): number {
    return hashUserId(userId) % 360;
}

/** 重複と空の userId を落として昇順に並べる (呼び出し元の集め方で、送る順が変わらないようにする)。 */
function sortedUniqueUserIds(userIds: readonly string[]): string[] {
    return [...new Set(userIds.filter((id) => id.length > 0))].sort();
}

/**
 * テスト用の送信側が送る profiles の中身。製品の core/facts.ts の buildProfiles と同じ形 (user_id、display_name、
 * avatar_key、bot)。display_name は displayNames にあればその名前、なければ userId をそのまま使う
 * (`HARNESS_SCREEN_NAMES` を渡さなければ、テスト用の参加者には本当の名前がない)。bot が真の人は botUserIds で指定する
 * (既定は全員が人)。
 */
export function buildHarnessProfiles(
    userIds: readonly string[],
    botUserIds: ReadonlySet<string> = new Set(),
    displayNames: ReadonlyMap<string, string> = new Map(),
): ProfileEntry[] {
    return sortedUniqueUserIds(userIds).map((id) => ({
        user_id: id,
        display_name: displayNames.get(id) ?? id,
        avatar_key: harnessAvatarKey(id),
        bot: botUserIds.has(id),
    }));
}

/**
 * テスト用の送信側が送る avatar の一覧。製品と同じ形 (user_id、avatar_key、avatar_png) で、参加者ごとに 1 通。pngFor が
 * 空の文字列を返した人と、base64 が上限を超える人には送らない (製品が取得できなかった人に送らないのと同じ)。
 */
export function buildHarnessAvatars(userIds: readonly string[], pngFor: (userId: string) => string): AvatarMessage[] {
    const out: AvatarMessage[] = [];
    for (const id of sortedUniqueUserIds(userIds)) {
        const png = pngFor(id);
        if (png === "" || png.length > AVATAR_MAX_BASE64_LENGTH) continue;
        out.push({ user_id: id, avatar_key: harnessAvatarKey(id), avatar_png: png });
    }
    return out;
}
