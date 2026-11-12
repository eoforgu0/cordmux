// SPDX-License-Identifier: Apache-2.0

// 1 つのストリーム (streamId) の映像のエンコードのセッション (createEncodeSession)。VideoFrameSource からフレームを
// 読み出し、encode/ladder.ts の段階を順に試して VideoEncoder を確保し、ウォームアップの後、決まった間隔で
// キーフレームを入れながらエンコードして、core/wire.ts の FrameHeader と一緒に sink.sendVideo に渡す。録画が ON の
// 間は、黒埋め用のフレームも作る。VideoEncoder と isConfigSupported は deps から受け取るので、Node の vitest で
// テストできる (tests/encode_session.test.ts)。

import type { FrameHeader } from "../core/wire";
import { FrameType } from "../core/wire";
import { applyParameterSetInsurance, NO_PARAMETER_SETS, type ParameterSets } from "./annexb";
import { createClockMap, mapMediaTime, type ClockMapResult, type ClockMapState } from "./clock_map";
import {
    type ActiveLadderStage,
    type EncoderTarget,
    type HardwareAccelerationPreference,
    LadderStage,
    codecStringForResolution,
    downscaleToLongSide,
    nextLadderStage,
    outcomeForStage,
} from "./ladder";
import { assertValidLimits, sameLimits, tightenLimits, type EncodeSessionLimits } from "./limits";
import {
    addDuration,
    createEncodeLatencySpan,
    resetEncodeLatencySpan,
    type EncodeLatencySpan,
} from "./latency_span";
import type { SerialGate } from "./serial";
import { consumeWarmupFrame, createWarmupPlan, isWarming, startWarmup } from "./warmup";
import type {
    EncodedVideoChunk,
    EncodedVideoChunkMetadata,
    VideoEncoderConfig,
    VideoEncoderLike,
    VideoEncoderSupport,
    VideoFrame,
} from "./webcodecs_types";
import type { VideoFrameSource } from "./frame_source";

/** セッションが使う外部の関数 (index.ts とテスト用の送信側が実際のものを、テストがテスト用のものを渡す)。 */
export interface EncodeSessionDeps {
    /** エンコーダを 1 つ作る (実際は `new VideoEncoder(init)`)。 */
    createEncoder(init: {
        output(chunk: EncodedVideoChunk, metadata: EncodedVideoChunkMetadata): void;
        error(e: unknown): void;
    }): VideoEncoderLike;
    /** 実際の `VideoEncoder.isConfigSupported` (static)。 */
    isConfigSupported(config: VideoEncoderConfig): Promise<VideoEncoderSupport>;
    /** エンコーダの確保 (isConfigSupported、configure、ウォームアップ) を順に 1 つずつ実行する SerialGate。 */
    gate: SerialGate;
    /** 送信側の時計の今 (us。encode/sender_clock.ts。docs/wire_format.md の「timestamp_us」)。 */
    nowUs(): bigint;
    /**
     * 単調に進む時計 (us)。エンコーダに渡してから出力が出るまでと、出力を受け取ってから送るまでの所要時間を測る
     * (encode/latency_span.ts)。差を取るだけなので、原点は何でもよい。省くと、この 2 つの所要時間は測らない。
     */
    monotonicUs?(): number;
    /**
     * 黒い VideoFrame を 1 枚作る (ウォームアップと、黒埋め用のフレームに使う)。timestampUs には、このファイルが
     * 予約した範囲 (WARMUP_TIMESTAMP_BASE と MATERIAL_TIMESTAMP_BASE から始まる範囲) の値が渡る。
     */
    makeBlackFrame(width: number, height: number, timestampUs: number): VideoFrame;
    /**
     * フレームを width x height に縮小した新しいフレームを返す。元のフレームの所有権はこの関数に移る。成功しても
     * 例外を投げても、元のフレームを close するのはこの関数の実装で、このファイルは返ったフレームだけを close する。
     * OffscreenCanvas を使う実装は drawImage の途中で失敗することがあり、失敗したときに誰が閉じるかを、呼ぶ側と
     * 実装の両方に持たせないためである (VideoFrame を閉じ忘れると、描画の資源がなくなる)。タイムスタンプ
     * (timestamp) は、元のフレームの値をそのまま引き継ぐ (このファイルは timestamp をキーにして、換算した時刻を
     * 引くため)。製品の実装は、OffscreenCanvas の 2d に drawImage して
     * `new VideoFrame(canvas, { timestamp, duration, alpha: "discard" })` を返す。
     */
    scaleFrame(frame: VideoFrame, width: number, height: number): VideoFrame;
}

/**
 * 今エンコードしているサイズと fps (縮小した後の値)。呼び出し元は、これを configure のメッセージの width、height、
 * fps に入れる。
 */
export interface EncodedVideoFormat {
    readonly width: number;
    readonly height: number;
    readonly fps: number;
}

/** 画質の上限の型。定義は encode/limits.ts にある。 */
export type { EncodeSessionLimits } from "./limits";

export interface EncodeSessionParams {
    /** ワイヤー形式の stream_id。 */
    streamId: number;
    source: VideoFrameSource;
    /**
     * "prefer-software" なら、どの段階を試すときも configure の hardwareAcceleration を prefer-software にする
     * (latencyMode と bitrateFactor と同じく、そのまま渡す)。試す段階の順 (Native から) は変えない。
     */
    preference: HardwareAccelerationPreference;
    /** 最初の上限。走っている間は setLimits() で下げられる。 */
    limits: EncodeSessionLimits;
    /** 速さを優先するか画質を優先するかで変える値の 1 つ。どの段階にもそのまま渡す。 */
    latencyMode: "realtime" | "quality";
    /** 速さを優先するか画質を優先するかで変える値のもう 1 つ。既定のビットレートに掛ける。 */
    bitrateFactor: number;
    /** false ならウォームアップをしない。呼び出し元が明示する (ふつうは true)。 */
    warmup: boolean;
    /**
     * キーフレームの間隔 (秒)。実際にエンコードしたフレーム (fpsDivisor で間引いたものは数えない) で数え、
     * keyintSec x その段階の fps / fpsDivisor (整数で、最小は 1) のフレームごとにキーフレームにする。
     */
    keyintSec: number;
}

/**
 * 黒埋め用のフレームを作ったタイミング。受信側の src-obs/record/record_types.h の MaterialOrigin
 * (EncoderCreated、RecordingTurnedOn、EncoderTeardown) と同じ並びにしてある。
 */
export type RecordingMaterialOrigin = "encoder-created" | "recording-turned-on" | "encoder-teardown";

export interface EncodeSessionSink {
    /**
     * エンコードしたフレームを送る。header はバイト列ではなく FrameHeader で渡す。バイト列にする (core/wire.ts の
     * encodeHeader を呼ぶ) のは sink の実装 (index.ts) である。
     */
    sendVideo(header: FrameHeader, payload: Uint8Array): void;
    /**
     * 黒埋め用のフレームを 1 回ぶん作り終えたときに呼ぶ。ワイヤー形式はタイミングを運ばない (フラグが 1 ビット
     * あるだけ) ので、受信側がどのタイミングのものかを知るには、ここから送信側のログに書くしかない。黒埋め用の
     * フレームそのものは sendVideo で送るので、この関数がなくても作れる。そのため省いてよい。
     */
    onRecordingMaterial?(origin: RecordingMaterialOrigin, frameCount: number): void;
    /**
     * 段階が変わるたび (最初の確保、フォールバック、Unavailable になったとき) に呼ぶ。format はその段階で実際に
     * エンコードするサイズと fps (縮小した後の値)。Unavailable のときは、エンコードするサイズがないので undefined
     * (呼び出し元は configure の width、height、fps を省き、読めない値を推測で埋めない)。
     */
    onStage(stage: LadderStage, format: EncodedVideoFormat | undefined): void;
    /**
     * 到着と換算した時刻の差が 200 ms を超えた状態が 1 秒続き、メディアの時計が替わったとみなして、映像の換算の定数を
     * 決め直したときに呼ぶ (encode/clock_map.ts の CLOCK_MAP_JUMP_CONFIRM_US)。呼び出し元はログに書く。省いてよい。
     */
    onClockReanchor?(info: ClockReanchorInfo): void;
}

/** 換算の定数を決め直したときの値 (EncodeSessionSink の onClockReanchor)。 */
export interface ClockReanchorInfo {
    /** 定数が動いた量 (us。後 - 前)。正は、タイムスタンプが後ろへ動いたことを示す。 */
    readonly shiftUs: bigint;
    /** 差がしきい値を超えた状態が続いた時間 (到着の時刻で数えた us)。 */
    readonly staleForUs: bigint;
    /** その間に、遅すぎるとして続けて捨てたフレームの数。 */
    readonly framesSkipped: number;
}

export interface EncodeSessionStats {
    readonly stage: LadderStage;
    readonly framesEncoded: number;
    readonly keyframesSent: number;
    readonly framesDroppedByDivisor: number;
    readonly warmupFramesSent: number;
    /** 取り出した時刻が、換算したタイムスタンプより STALE_FRAME_SKIP_US 以上遅れていたので捨てたフレームの数。 */
    readonly framesSkippedStale: number;
    /** メディアの時計が替わったとみなして、換算の定数を決め直した回数 (ClockReanchorInfo)。 */
    readonly clockReanchors: number;
    /**
     * 今使っている encode/clock_map.ts の定数 (送信側の時計 - メディアのタイムスタンプ)。まだ 1 フレームも換算して
     * いなければ undefined。
     */
    readonly clockOriginUs: bigint | undefined;
    /** 録画専用のフラグを付けて送った黒埋め用のフレームの数 (3 つのタイミングの合計)。 */
    readonly recordingMaterialFramesSent: number;
    /** 黒埋め用のフレームを作ろうとして失敗した回数 (失敗しても、その段階はやめない)。 */
    readonly recordingMaterialFailures: number;
    /** SPS と PPS を前に付けて送ったキーフレームの数。 */
    readonly spsPpsPrependedCount: number;
    /**
     * キーフレームに SPS か PPS がなく、付ける SPS と PPS もまだなかった回数 (そのインスタンスの最初の IDR)。
     * 0 でなければ、受信側の録画のセグメントが始まらない。
     */
    readonly keyframesWithoutParameterSets: number;
}

export interface EncodeSessionHandle {
    /**
     * 前に呼んでから今までの、段階ごとの所要時間の足し上げを取り出し、空に戻す (encode/latency_span.ts)。定期の
     * ログの行を書くときに呼ぶ。戻す値は呼び出し元のもので、この後セッションが書き換えない。
     */
    takeLatencySpan(): EncodeLatencySpan;
    /** 最初の段階の確保が終わる (成功するか、Unavailable になる) まで待つ。その後の読み出しは中で続く。 */
    start(): Promise<void>;
    /** 次にエンコードするフレームをキーフレームにする。 */
    requestKeyframe(): void;
    /**
     * 読み出しを止め、source を close する。エンコーダを閉じるのは、encoder-teardown の黒埋め用のフレームを送り
     * 終えてから。黒埋め用のフレームを作らないときは、この呼び出しの中で閉じる。
     */
    stop(): void;
    /**
     * stop() が始めた後始末 (encoder-teardown の黒埋め用のフレームの flush と close、実行中の recording-turned-on)
     * が終わるまで待つ。reject しない。stop() の前に呼んでもよく、そのときは今実行中の処理だけを待つ。
     */
    finished(): Promise<void>;
    /**
     * finished() が待つ処理が今 1 つでもあるか。呼び出し元が、待つ必要があるかを見分けるための関数で、false なら
     * finished() はすぐに解決する。stop() の中で encoder-teardown の処理が増えるので、呼ぶのは stop() の後。段階の
     * 確保 (start() とフォールバック) も、encoder-created の黒埋め用のフレームを含みうる処理として数える (録画が
     * OFF でも、確保の途中は true になる)。
     */
    hasPendingWork(): boolean;
    /**
     * 録画の ON と OFF を伝える。変わっていなければ何もしない。true にしたときに動いているエンコーダがあり、まだ
     * 黒埋め用のフレームがなければ、recording-turned-on の処理を始める。start() の前に呼んでおけば、encoder-created
     * のタイミングで作る。
     */
    setRecording(on: boolean): void;
    /**
     * 画質の上限を変える。下げたキーだけをこのストリームに適用、次のフレームの境目で、同じ段階から確保し直す。
     * 上げたキーはこのストリームには適用されない (次の画面共有から)。適用する上限が変わらなければ何もしない。形が
     * 正しくなければ RangeError を投げる (呼び出し元の誤り。ワイヤー形式で届いた値は transport/control.ts が
     * 確認してある)。start() の前に呼べば、最初の確保で使う。stop() の後は何もしない。
     */
    setLimits(limits: EncodeSessionLimits): void;
    stats(): EncodeSessionStats;
    /**
     * 今使っている encode/clock_map.ts の原点だけを読む。stats() と同じ値を返すが、オブジェクトを作らない。
     * 呼び出し元 (index.ts) は映像のフレームごとに読むので、stats() だと、4 ストリーム x 60 fps で 1 秒に 240 個の
     * オブジェクトを作ることになる。
     */
    clockOriginUs(): bigint | undefined;
    /**
     * SPS と PPS を前に付けた回数だけを読む。呼び出し元は facts を作るたびに読むので、clockOriginUs() と同じ理由で、
     * オブジェクトを作らない。
     */
    spsPpsPrependedCount(): number;
    /**
     * SPS と PPS を前に付けた回数の最終値。stop() が始めた後始末 (encoder-teardown の黒埋め用のフレームの flush) が
     * 終わってから解決するので、呼ぶのは stop() の後。reject しない (finished() と同じ)。stop() の直後に
     * spsPpsPrependedCount() を読むと、encoder-teardown のぶんが入らない。黒埋め用のフレームが出るのは flush() が
     * 解決した後で、stop() はその前に返るためである。呼び出し元 (index.ts) は、これが解決するまでこのセッションを
     * 数え続け、解決した値を合計に移す (facts の値を減らさないため)。
     */
    finalSpsPpsPrependedCount(): Promise<number>;
}

const BASELINE_BITRATE_BPS = 12_000_000; // 1080p60 の基準
const BASELINE_PIXELS = 1920 * 1080;
const BASELINE_FPS = 60;

// 取り出した時刻 (到着) が、換算したタイムスタンプよりこの値以上遅れているフレームは、エンコードせずに捨てる。
// 描画が詰まった後に MediaStreamTrackProcessor にたまったフレームをまとめて読むと、過去のタイムスタンプのフレームが
// まとめて出る。libobs は非同期のソースごとに、最初のフレームのタイムスタンプと到着の時刻で表示の原点を決めて、
// 実時間で進めるので、まとめて送ると、そのストリームの表示が、まとめた分だけ遅れたままになる。音声は捨てない (OBS が
// 古い音声を捨てるので、映像だけを捨てれば、映像と音声はずれない)。目安の値で、測って決めたものではない。
// 遅すぎるフレームが 1 秒途切れずに続いたときは、詰まりではなくメディアの時計が替わったとみなして、換算の定数を
// 決め直す (encode/clock_map.ts の CLOCK_MAP_JUMP_US と CLOCK_MAP_JUMP_CONFIRM_US。この値と同じ 200 ms にしてある)。
const STALE_FRAME_SKIP_US = 200_000n;

// 自分で作るフレーム (ウォームアップと黒埋め用のフレーム) のタイムスタンプに予約する範囲。実際のフレームの
// タイムスタンプ (メディアのタイムスタンプ) はこの範囲に入らない。1 つの範囲は 1000 秒ぶんあり、ウォームアップの
// 60 フレームにも、黒埋め用のフレームの 1 秒ぶんにも十分に広い。値は、メディアのタイムスタンプが取らない値なら
// 何でもよい。負の値にしないのは、エンコーダが負のタイムスタンプを受け付けるかを確認していないためである。
// 2 の 53 乗より十分に小さいので、number で正確に表せる。
const WARMUP_TIMESTAMP_BASE = 4_000_000_000_000_000;
const SYNTHETIC_BAND_US = 1_000_000_000;
const MATERIAL_TIMESTAMP_BASE = WARMUP_TIMESTAMP_BASE + SYNTHETIC_BAND_US;

/**
 * 出力されたフレームの種類。ウォームアップ中か、黒埋め用のフレームを作っている最中かを真偽値で切り替えると、
 * encode() は非同期なので、切り替えたときに前のフレームの出力が新しい値で出てしまう。そこで、予約した
 * タイムスタンプの範囲で分ける。
 */
type FrameClass = "material" | "warmup" | "real";

function classifyTimestamp(timestampUs: number): FrameClass {
    if (timestampUs >= MATERIAL_TIMESTAMP_BASE && timestampUs < MATERIAL_TIMESTAMP_BASE + SYNTHETIC_BAND_US) {
        return "material";
    }
    if (timestampUs >= WARMUP_TIMESTAMP_BASE && timestampUs < WARMUP_TIMESTAMP_BASE + SYNTHETIC_BAND_US) {
        return "warmup";
    }
    return "real";
}

/** 1080p60 で 12 Mbps を基準に、ピクセル数と fps に比例させ、bitrateFactor を掛ける。 */
function computeBitrate(width: number, height: number, fps: number, bitrateFactor: number): number {
    const scale = (width * height * fps) / (BASELINE_PIXELS * BASELINE_FPS);
    return Math.max(1, Math.round(BASELINE_BITRATE_BPS * scale * bitrateFactor));
}

function validateParams(params: EncodeSessionParams): void {
    if (!Number.isFinite(params.keyintSec) || params.keyintSec <= 0) {
        throw new RangeError(`encode/session: keyintSec は正の数にしてください: ${params.keyintSec}`);
    }
    if (!Number.isFinite(params.bitrateFactor) || params.bitrateFactor <= 0) {
        throw new RangeError(`encode/session: bitrateFactor は正の数にしてください: ${params.bitrateFactor}`);
    }
    // fpsDivisor は 1 か 2、maxLongEdge はない (上限なし) か正の数だけを受け付ける。setLimits() と同じ確認の方法を
    // 使う (encode/limits.ts)。
    assertValidLimits(params.limits);
}

/** エンコードに失敗した (isConfigSupported が false、configure か encode が error) ことを表す。 */
type StageFailure = { ok: false };

interface ErrorBox {
    error: unknown;
}

/**
 * error コールバックが最初の 1 回だけ書き込む ErrorBox と、それを待つための notify を作る。WebCodecs の仕様では、
 * error の後の VideoEncoder は closed になり、output も error も呼ばれない (実機では確認していない)。この前提で、
 * フォールバックした後に古いエンコーダの output を無視する仕組み (世代の番号など) は持たない。
 */
function createErrorBox(): { box: ErrorBox; notify: Promise<void>; mark(e: unknown): void } {
    const box: ErrorBox = { error: undefined };
    let resolveNotify!: () => void;
    const notify = new Promise<void>((resolve) => {
        resolveNotify = resolve;
    });
    return {
        box,
        notify,
        mark(e: unknown) {
            if (box.error === undefined) {
                box.error = e;
                resolveNotify();
            }
        },
    };
}

/**
 * エンコーダ 1 つの出力を受け取る関数。handle を createEncoder に渡し、黒埋め用のフレームを作る側は、1 回ぶんの
 * 始めに beginMaterial() を呼ぶ (黒埋め用のフレームのタイムスタンプの原点を、そこで 1 度だけ取る)。
 */
interface EncoderOutput {
    handle(chunk: EncodedVideoChunk, metadata: EncodedVideoChunkMetadata): void;
    /** 黒埋め用のフレームの 1 回ぶんを始める (タイムスタンプの原点を取り、フレームをため始める)。 */
    beginMaterial(): void;
    /**
     * 黒埋め用のフレームの 1 回ぶんを終える。deliver が true なら、ためたフレームを続けて sink に渡し、false なら
     * 捨てる。
     */
    endMaterial(deliver: boolean): void;
}

interface ActiveEncoder {
    readonly encoder: VideoEncoderLike;
    /** このエンコーダに渡した出力の関数 (encoder-teardown で、黒埋め用のフレームの原点を取り直すのに使う)。 */
    readonly output: EncoderOutput;
    readonly stage: ActiveLadderStage;
    readonly keyintFrames: number;
    readonly errorBox: ErrorBox;
    /** このエンコーダに実際に configure したサイズと fps。 */
    readonly format: EncodedVideoFormat;
    /**
     * このエンコーダに実際に渡した VideoEncoderConfig。recording-turned-on の一時的なインスタンスは、これをそのまま
     * 使う (buildConfig で作り直さない。今は同じ値になるが、走っている間に設定を変えると食い違う)。
     */
    readonly config: VideoEncoderConfig;
    /** このエンコーダを作ったときの上限 (間引きも、この値で行う)。 */
    readonly limits: EncodeSessionLimits;
    /** このエンコーダを作ったときの limitsVersion。セッションの値と違えば、次のループで確保し直す。 */
    readonly limitsVersion: number;
    /**
     * encoder-created のタイミングで黒埋め用のフレームを試したか。試して失敗したときは、差し替えた直後に
     * recording-turned-on をやり直さない (失敗の回数を増やさないため)。
     */
    readonly materialTriedAtCreation: boolean;
    /** このインスタンスの黒埋め用のフレームの進み具合 (3 つのタイミングのうち、先に来たもので 1 回だけ作る)。 */
    material: MaterialState;
    /** 閉じ始めたか (stop() と確保し直しの両方から呼ばれても、閉じるのは 1 回だけ)。 */
    retired: boolean;
}

/**
 * 黒埋め用のフレームの進み具合。予約 (scheduled) と実行中 (running) を分けるのは、recording-turned-on の処理を
 * SerialGate に渡してから実際に動くまでの間に、stop() が呼ばれることがあるためである。
 * - scheduled のまま止まったら、encoder-teardown が引き取る (渡した処理は stopped を見て、何もせずに終わる)。
 * - running になってから止まったら、encoder-teardown は作らない (一時的なエンコーダが送り終える。finished() が
 *   その終わりを待つ)。
 * この区別がないと、ON にした直後に止めたときに、黒埋め用のフレームが 1 回も出ないか、2 回出るかのどちらかになる。
 */
type MaterialState = "none" | "scheduled" | "running" | "done";

function safeClose(encoder: VideoEncoderLike): void {
    try {
        encoder.close();
    } catch {
        // error で閉じたエンコーダを、もう一度閉じたときの例外は無視する。
    }
}

/**
 * 1 つのストリームのエンコードのセッションを作る。params の形が正しくなければ RangeError を投げる。
 *
 * 黒埋め用のフレーム: 録画が ON の間だけ、エンコーダのインスタンスごとに 1 回、1 秒ぶんの黒 (IDR 1 枚と P フレーム) を
 * エンコードし、録画専用のフラグを付けて送る。作るのは次の 3 つのタイミングのうち、先に来たものである。録画が OFF の
 * 間は、黒埋め用のフレームのための makeBlackFrame と createEncoder の呼び出しは 1 つも増えない。
 * - encoder-created: ウォームアップが成功した直後で、最初の実際のフレームの前 (attemptStage)。start() も
 *   フォールバックも attemptFromStage を通るので、ここ 1 か所で済む。
 * - recording-turned-on: setRecording(true) のときに動いているインスタンスのために、SerialGate に渡した別の
 *   インスタンスで作る。動いているエンコーダに黒いフレームを流すと、表示が欠ける。isConfigSupported は呼ばず
 *   (どの hardwareAcceleration でも supported: true を返し、空きがあるかを教えない)、ActiveEncoder.config を使う。
 * - encoder-teardown: stop() は読み出しを止めるだけにし、黒埋め用のフレームを送り終えて (flush() を待って) から
 *   close() する。作らないとき (録画が OFF、もう作った、エンコーダが error になった) は、stop() の中で閉じる
 *   (録画が OFF の間に、停止後に残る処理を作らない)。この flush() では、止める前にエンコーダに渡してあった実際の
 *   フレームも出る。stop() が pendingTimestamps を空にした後なので、そのフレームはその時点の送信側の時計で送る。
 *   変換した値との差は小さいのでそのままにしているが、録画の最後のフレームの位置に影響しないかは確認していない。
 *
 * 黒埋め用のフレームのタイムスタンプ (timestamp_us) は「その 1 回ぶんの始めの送信側の時計 + 予約した範囲の中の
 * 位置」にする。受信側は録画専用のフレームをデコードも表示もしないので、整列にも古いフレームの扱いにも関わらないが、
 * 録画はこのタイムスタンプをそのまま使い、最初のフレームを原点にした差を pts にする。その時点の送信側の時計を使うと、
 * ループと flush() でまとめて出る 60 フレームがほぼ同じ値になり、1 秒ぶんの黒が数十 ms の mov に
 * なって、録画ファイルの作成の長さのチェックが失敗する。範囲の中の位置は、作るときに置いた i * frameIntervalUs なので、原点に
 * 足せば 1 フレームの周期ごとに進む。
 *
 * 黒埋め用のフレームは 1 回ぶんをためておき、flush() の後に続けて sink に渡す。受信側の録画は、同じストリームの
 * 実際のフレームが届いた時点で、黒埋め用のフレームのファイルを閉じる (src-obs/record/recorder.cpp の handle_video)。
 * recording-turned-on では、動いているエンコーダが実際のフレームを出し続けている横で、別のインスタンスが黒埋め用の
 * フレームを作るので、届いた順に渡すと、間に入った実際のフレームでファイルが閉じ、残りの黒埋め用のフレームが
 * 捨てられる。ループで続けて渡せば、同じ WebSocket の上で間に実際のフレームが入らない。途中で失敗したときは、
 * ためたフレームを渡さない (1 回ぶんを全部渡すか、渡さないか)。
 *
 * 黒埋め用のフレームが作れなくても、その段階はやめず、配信を続ける。黒埋め用のフレームのない人は ProRes で録画ファイルを作成する
 * 方法が残るが、映像そのものを失うと残る方法がない。失敗の回数は stats().recordingMaterialFailures に入れる。
 *
 * SPS と PPS: encode/annexb.ts で、キーフレームに SPS か PPS がなければ、最後に見たものを前に付ける。最後に見た
 * SPS と PPS は、エンコーダのインスタンスごと (makeOutput() の中) に持つ。回数は stats() に入れ、呼び出し元が
 * facts に入れる (受信側が録画ログに書けるように)。
 *
 * 上限の変更 (setLimits()): 走っているストリームは、フォールバックと同じ入口 (pumpLoop の先頭) から、同じ段階で、
 * 新しい上限で確保し直す。確保し直すのはフレームの境目で、次のループの先頭で気付く (エンコードしているフレームの
 * 途中で差し替えない)。確保の間は読み出しが止まり、たまったフレームは古いフレームとして捨てる (フォールバックと
 * 同じ)。古いエンコーダを先に閉じてから (stop() と同じ retireEncoder。録画が ON で黒埋め用のフレームがまだなければ、
 * それを送り終えてから閉じる)、新しいものを作る。新しいものを先に作る方法は取らない。確保の間にハードウェアの
 * エンコーダを 1 つ余分に使い、失敗したときに戻す処理も必要になるためである。上限を下げた後の確保は下げる前より
 * 軽いので、フォールバックと同じく、古いエンコーダがない状態で確保する。
 * Unavailable になったストリーム (読み出しを止めたもの) は確保し直さない (確保の入口は pumpLoop の中だけで、
 * Unavailable はフォールバックの終わり。下げた上限は次の画面共有から反映される)。
 */
export function createEncodeSession(
    deps: EncodeSessionDeps,
    params: EncodeSessionParams,
    sink: EncodeSessionSink,
): EncodeSessionHandle {
    validateParams(params);

    let stopped = false;
    let started = false;
    let active: ActiveEncoder | undefined;
    let keyframeRequested = false;
    /** 録画の現在の値。setRecording だけが変える。 */
    let recording = false;
    /** このストリームに適用している上限。setLimits が下げる向きにだけ変える。 */
    let limits: EncodeSessionLimits = { ...params.limits };
    /** limits を変えるたびに増える番号。 */
    let limitsVersion = 0;
    /**
     * 実行中の後始末 (recording-turned-on の一時的なエンコーダ、encoder-teardown の黒埋め用のフレームと close)。
     * finished() が待つ。
     */
    const pendingWork = new Set<Promise<unknown>>();

    // EncodedVideoChunk.timestamp (エンコードした VideoFrame の timestamp と同じ値) から、そのフレームのメディアの
    // タイムスタンプを換算した時刻を引くための表 (キーは frame.timestamp のまま。換算は clock_map.ts)。
    const pendingTimestamps = new Map<number, bigint>();
    // エンコーダに渡した時刻 (deps.monotonicUs)。キーは pendingTimestamps と同じ。monotonicUs がなければ使わない。
    const pendingSubmitUs = new Map<number, number>();
    // 段階ごとの所要時間の足し上げ (takeLatencySpan が取り出して空に戻す)。
    const latencySpan: EncodeLatencySpan = createEncodeLatencySpan();
    // メディアの時計と送信側の時計の対応。フォールバックでエンコーダが替わっても、フレームの読み出し元 (source) と
    // メディアの時計は同じなので、作り直さない。
    let clockMap: ClockMapState = createClockMap();
    // 続けて遅すぎるとして捨てたフレームの数 (定数を決め直したときに ClockReanchorInfo に入れる)。遅すぎないフレームで 0 に戻す。
    let staleSkipped = 0;

    const stats = {
        stage: LadderStage.Unavailable as LadderStage,
        framesEncoded: 0,
        keyframesSent: 0,
        framesDroppedByDivisor: 0,
        warmupFramesSent: 0,
        framesSkippedStale: 0,
        clockReanchors: 0,
        recordingMaterialFramesSent: 0,
        recordingMaterialFailures: 0,
        spsPpsPrependedCount: 0,
        keyframesWithoutParameterSets: 0,
    };

    /**
     * 実行中の後始末が 1 つもなくなるまで待つ。reject しない。処理の中から次の処理が生まれることがある
     * (recording-turned-on の途中で stop() が呼ばれた、など) ので、空になるまで繰り返し待つ。finished() と
     * finalSpsPpsPrependedCount() が使う。
     */
    async function waitFinished(): Promise<void> {
        for (;;) {
            const inFlight = [...pendingWork];
            if (inFlight.length === 0) return;
            await Promise.all(inFlight.map((work) => work.then(() => undefined, () => undefined)));
        }
    }

    /** 後始末の処理を 1 つ登録する (終わったら外れる)。 */
    function trackPendingWork(work: Promise<unknown>): void {
        pendingWork.add(work);
        void work.then(
            () => pendingWork.delete(work),
            () => pendingWork.delete(work),
        );
    }

    /**
     * 実際にエンコードするサイズを決める。長辺は min(maxLongEdge, その段階の target の長辺) で、maxLongEdge がなければ
     * target の長辺。丸め (縦横比を保ち、偶数に切り捨てる) は encode/ladder.ts の downscaleToLongSide に任せる。
     * source がもとからその長辺以下なら縮小しない (同じサイズが返る)。
     */
    function encodedFormatFor(
        target: EncoderTarget,
        effectiveFps: number,
        stageLimits: EncodeSessionLimits,
    ): EncodedVideoFormat {
        const targetLongEdge = Math.max(target.width, target.height);
        const limit = stageLimits.maxLongEdge;
        const longEdge = limit === undefined ? targetLongEdge : Math.min(limit, targetLongEdge);
        const { width, height } = downscaleToLongSide(
            params.source.format.width,
            params.source.format.height,
            longEdge,
        );
        return { width, height, fps: effectiveFps };
    }

    function buildConfig(target: EncoderTarget, format: EncodedVideoFormat): VideoEncoderConfig {
        const hardwareAcceleration =
            params.preference === "prefer-software" ? "prefer-software" : target.hardwareAcceleration;
        // codec (Level) と bitrate は、target.width と target.height ではなく、実際に configure するサイズ (縮小した
        // 後) と fps から選ぶ。target.codec を使うと、実際にエンコードする面積に対して Level の MaxFS が小さく
        // (または大きく) なり、isConfigSupported が false を返して、段階 3 と 4 でもエンコーダを確保できなくなる。
        const { width, height, fps } = format;
        return {
            codec: codecStringForResolution(width, height, fps),
            width,
            height,
            framerate: fps,
            bitrate: computeBitrate(width, height, fps, params.bitrateFactor),
            latencyMode: params.latencyMode,
            avc: { format: "annexb" },
            hardwareAcceleration,
        };
    }

    /**
     * エンコーダ 1 つの出力を受け取る関数を作る。最後に見た SPS と PPS は、この中 (エンコーダのインスタンスごと) に
     * 持つ。ほかのインスタンスの SPS を付けると、解像度の違う SPS から受信側が avcC を作る。黒埋め用のフレームの
     * タイムスタンプの原点も、同じ理由でインスタンスごとに持つ。
     */
    function makeOutput(): EncoderOutput {
        let parameterSets: ParameterSets = NO_PARAMETER_SETS;
        /** 黒埋め用のフレームの 1 回ぶんの始めの送信側の時計 (beginMaterial() が入れる)。 */
        let materialOriginUs: bigint | undefined;
        /** 黒埋め用のフレームの 1 回ぶんをためておく配列 (beginMaterial() から endMaterial() まで)。 */
        let materialHeld: { header: FrameHeader; payload: Uint8Array }[] | undefined;
        const handle = (chunk: EncodedVideoChunk, _metadata: EncodedVideoChunkMetadata): void => {
            const frameClass = classifyTimestamp(chunk.timestamp);
            if (frameClass === "warmup") return; // ウォームアップの出力は送らない (タイムスタンプの範囲で分ける)
            // 出力を受け取った時刻 (所要時間の測定。deps.nowUs は呼ばない: テストが呼ぶ順に値を返すため)。
            const outputAtUs = deps.monotonicUs?.();
            const isKeyframe = chunk.type === "key";

            const encoded = new Uint8Array(chunk.byteLength);
            chunk.copyTo(encoded);
            // SPS と PPS を前に付ける。最後に見た SPS と PPS の更新と判定は、ここ 1 か所で行う。
            const insured = applyParameterSetInsurance(encoded, isKeyframe, parameterSets);
            parameterSets = insured.remembered;
            if (insured.prepended) stats.spsPpsPrependedCount++;
            if (insured.missing) stats.keyframesWithoutParameterSets++;

            if (frameClass === "material") {
                // 黒埋め用のフレームのタイムスタンプは「1 回ぶんの始めの送信側の時計 + 範囲の中の位置」にする
                // (createEncodeSession のコメント)。
                const offsetUs = Math.max(0, chunk.timestamp - MATERIAL_TIMESTAMP_BASE);
                const header: FrameHeader = {
                    type: FrameType.Video,
                    streamId: params.streamId,
                    timestampUs: (materialOriginUs ?? deps.nowUs()) + BigInt(offsetUs),
                    flags: { keyframe: isKeyframe, recordingOnly: true },
                };
                // 1 回ぶんをためてから、続けて渡す (createEncodeSession のコメント)。
                if (materialHeld !== undefined) {
                    materialHeld.push({ header, payload: insured.payload });
                    return;
                }
                stats.recordingMaterialFramesSent++;
                sink.sendVideo(header, insured.payload);
                return;
            }

            const mappedUs = pendingTimestamps.get(chunk.timestamp);
            pendingTimestamps.delete(chunk.timestamp);
            // 表にない (想定外の) ときだけ、その時点の送信側の時計で代える。
            const timestampUs = mappedUs ?? deps.nowUs();
            const header: FrameHeader = {
                type: FrameType.Video,
                streamId: params.streamId,
                timestampUs,
                flags: { keyframe: isKeyframe, recordingOnly: false },
            };
            stats.framesEncoded++;
            if (isKeyframe) stats.keyframesSent++;
            const submittedAtUs = pendingSubmitUs.get(chunk.timestamp);
            pendingSubmitUs.delete(chunk.timestamp);
            const beforeSendUs = deps.monotonicUs?.();
            if (outputAtUs !== undefined && beforeSendUs !== undefined) {
                if (submittedAtUs !== undefined) addDuration(latencySpan.encode, outputAtUs - submittedAtUs);
                addDuration(latencySpan.prepare, beforeSendUs - outputAtUs);
            }
            sink.sendVideo(header, insured.payload);
            const afterSendUs = deps.monotonicUs?.();
            if (beforeSendUs !== undefined && afterSendUs !== undefined) {
                addDuration(latencySpan.send, afterSendUs - beforeSendUs);
            }
        };
        return {
            handle,
            beginMaterial(): void {
                materialOriginUs = deps.nowUs();
                materialHeld = [];
            },
            endMaterial(deliver: boolean): void {
                const held = materialHeld ?? [];
                materialHeld = undefined;
                if (!deliver) return;
                // ループで続けて渡す (間に実際のフレームの sendVideo が入らないようにする)。
                for (const { header, payload } of held) {
                    stats.recordingMaterialFramesSent++;
                    sink.sendVideo(header, payload);
                }
            },
        };
    }

    /**
     * ウォームアップ: 黒いフレームをエンコードして捨ててから、実際のフレームの読み出しに進む。タイムスタンプは予約した
     * 範囲 (WARMUP_TIMESTAMP_BASE) の値で、ウォームアップの出力は、この範囲にあることだけを理由に捨てる。
     */
    async function runWarmup(
        encoder: VideoEncoderLike,
        cfg: VideoEncoderConfig,
        errorBox: ErrorBox,
        notify: Promise<void>,
    ): Promise<boolean> {
        if (!params.warmup) return true;

        const frameIntervalUs = Math.max(1, Math.round(1_000_000 / (cfg.framerate ?? BASELINE_FPS)));

        const work = (async () => {
            let plan = startWarmup(createWarmupPlan());
            let i = 0;
            while (isWarming(plan)) {
                const frame = deps.makeBlackFrame(
                    cfg.width,
                    cfg.height,
                    WARMUP_TIMESTAMP_BASE + i * frameIntervalUs,
                );
                try {
                    encoder.encode(frame, { keyFrame: i === 0 });
                    stats.warmupFramesSent++;
                } finally {
                    frame.close();
                }
                plan = consumeWarmupFrame(plan).state;
                i++;
            }
            await encoder.flush();
        })();

        const outcome = await Promise.race([
            work.then(() => "done" as const),
            notify.then(() => "error" as const),
        ]);
        if (outcome === "error") return false;

        // flush() が解決した時点で、もう error が来ていないかも確認する (error の後に呼ばれないという前提が崩れた
        // ときのため。来ていれば、ウォームアップが終わっていても、この段階は使わない)。
        return errorBox.error === undefined;
    }

    /**
     * 黒埋め用のフレームを 1 回ぶん作る。cfg.framerate ぶんのフレーム (最初だけキーフレームで、IDR 1 枚と P フレーム)
     * をエンコードし、flush() で出し切ってから返る。例外を投げない。失敗しても回数を数えて false を返すだけで、その
     * 段階はやめない。
     */
    async function runRecordingMaterial(
        encoder: VideoEncoderLike,
        out: EncoderOutput,
        cfg: VideoEncoderConfig,
        origin: RecordingMaterialOrigin,
    ): Promise<boolean> {
        const frameCount = Math.max(1, Math.round(cfg.framerate ?? BASELINE_FPS));
        const frameIntervalUs = Math.max(1, Math.round(1_000_000 / frameCount));
        // 黒埋め用のフレームのタイムスタンプの原点を、最初の encode() の前に 1 度だけ取る (makeOutput() が、これに
        // 範囲の中の位置を足す)。
        out.beginMaterial();
        try {
            for (let i = 0; i < frameCount; i++) {
                const frame = deps.makeBlackFrame(
                    cfg.width,
                    cfg.height,
                    MATERIAL_TIMESTAMP_BASE + i * frameIntervalUs,
                );
                try {
                    encoder.encode(frame, { keyFrame: i === 0 });
                } finally {
                    frame.close();
                }
            }
            await encoder.flush();
        } catch {
            // 途中までのフレームは渡さない (1 回ぶんを全部渡すか、渡さないか)。
            out.endMaterial(false);
            stats.recordingMaterialFailures++;
            return false;
        }
        // flush() が解決した時点で、出力はすべてたまっている (WebCodecs の flush() の約束)。
        out.endMaterial(true);
        sink.onRecordingMaterial?.(origin, frameCount);
        return true;
    }

    /** 1 つの段階を試す。失敗したら undefined を返す (呼び出し元が次の段階に進む)。 */
    async function attemptStage(stage: LadderStage): Promise<ActiveEncoder | undefined> {
        const outcome = outcomeForStage(stage, params.source.format);
        if (outcome.stage === LadderStage.Unavailable) return undefined;
        const { target } = outcome;

        // この試行で使う上限と limitsVersion を、ここでコピーする (確保の途中で setLimits が呼ばれても、作った
        // エンコーダがどの上限のものかを取り違えない。食い違えば pumpLoop がやり直す)。
        const stageLimits = limits;
        const stageLimitsVersion = limitsVersion;
        const effectiveFps = Math.max(1, Math.round(target.fps / stageLimits.fpsDivisor));
        const format = encodedFormatFor(target, effectiveFps, stageLimits);
        const cfg = buildConfig(target, format);

        let supported: VideoEncoderSupport;
        try {
            supported = await deps.isConfigSupported(cfg);
        } catch {
            return undefined;
        }
        if (!supported.supported) return undefined;

        const { box: errorBox, notify, mark } = createErrorBox();

        let encoder: VideoEncoderLike;
        const out = makeOutput();
        try {
            encoder = deps.createEncoder({
                output: out.handle, // handle は makeOutput() の中の関数そのもの (this を使わない)。
                error: mark,
            });
        } catch {
            // エンコーダそのものを作れなかった (deps.createEncoder が同期に例外を投げた)。閉じるものがないので、
            // そのまま次の段階に進む。
            return undefined;
        }

        try {
            encoder.configure(cfg);
        } catch {
            safeClose(encoder);
            return undefined;
        }

        // runWarmup() は中で encoder.encode() と encoder.flush() を待つので、ウォームアップ中にエンコーダが失敗する
        // (encode が同期に例外を投げる、flush が reject する) と、ここで例外になる。configure() の失敗と同じく、
        // 閉じて undefined を返し、次の段階に進む (失敗したエンコーダに encode を続けると InvalidStateError になる)。
        let warmedUp: boolean;
        try {
            warmedUp = await runWarmup(encoder, cfg, errorBox, notify);
        } catch {
            safeClose(encoder);
            return undefined;
        }
        if (!warmedUp) {
            safeClose(encoder);
            return undefined;
        }

        const keyintFrames = Math.max(1, Math.round(params.keyintSec * effectiveFps));
        // encoder-created: ウォームアップの直後、最初の実際のフレームの前。stopped の間は作らない (止まりかけの
        // セッションで作っても、送り先がない)。
        const tryMaterial = recording && !stopped && errorBox.error === undefined;
        const result: ActiveEncoder = {
            encoder,
            output: out,
            stage: outcome.stage,
            keyintFrames,
            errorBox,
            format,
            config: cfg,
            limits: stageLimits,
            limitsVersion: stageLimitsVersion,
            materialTriedAtCreation: tryMaterial,
            material: "none",
            retired: false,
        };

        if (tryMaterial) {
            result.material = "running";
            const ok = await runRecordingMaterial(encoder, out, cfg, "encoder-created");
            result.material = ok ? "done" : "none";
        }
        return result;
    }

    /** stage から Unavailable まで、成功するか尽きるまで順に試す (再帰。段階は 5 つなので、深さは 4 まで)。 */
    async function attemptFromStage(stage: LadderStage): Promise<ActiveEncoder | StageFailure> {
        if (stage === LadderStage.Unavailable) return { ok: false };
        const attempt = await attemptStage(stage);
        if (attempt) return attempt;
        return attemptFromStage(nextLadderStage(stage));
    }

    function isSuccess(result: ActiveEncoder | StageFailure): result is ActiveEncoder {
        return "encoder" in result;
    }

    /**
     * エンコーダ 1 つを閉じる (encoder-teardown)。stop() と確保し直しの両方が呼ぶが、1 つのエンコーダにつき 1 回だけ
     * 閉じる (retired)。録画が ON で黒埋め用のフレームがまだなければ、それを送り終えてから閉じ、その処理を返す
     * (finished() が待つ処理にもする)。作らないときは、ここで閉じて undefined を返す (録画が OFF の間に、停止後に
     * 残る処理を作らない)。material が running のときは、recording-turned-on の一時的なエンコーダが送り終えるので、
     * ここでは作らない (finished() がその終わりを待つ)。
     */
    function retireEncoder(target: ActiveEncoder): Promise<void> | undefined {
        if (target.retired) return undefined;
        target.retired = true;
        const needsMaterial = recording && target.material !== "done" && target.material !== "running";
        if (!needsMaterial || target.errorBox.error !== undefined) {
            safeClose(target.encoder);
            return undefined;
        }
        target.material = "done";
        const work = (async () => {
            await runRecordingMaterial(target.encoder, target.output, target.config, "encoder-teardown");
            safeClose(target.encoder);
        })();
        trackPendingWork(work);
        return work;
    }

    /**
     * 確保したエンコーダを使い始める (最初の確保、フォールバック、確保し直しで共通)。差し替えた直後に、録画が ON で、
     * 黒埋め用のフレームがなく、encoder-created でも試していなければ、recording-turned-on をやり直す。ウォームアップの
     * 直後の判定と差し替えの間に setRecording(true) が呼ばれると、recording-turned-on は古いエンコーダを対象にして
     * 何もせずに終わり、新しいエンコーダは黒埋め用のフレームを持たないまま動くためである。
     */
    function adoptEncoder(next: ActiveEncoder): void {
        active = next;
        stats.stage = next.stage;
        sink.onStage(next.stage, next.format);
        if (recording && !stopped && next.material === "none" && !next.materialTriedAtCreation) {
            scheduleMaterialOnSpareEncoder();
        }
    }

    async function pumpLoop(): Promise<void> {
        let pulled = 0;
        let sinceKeyframe = 0;
        // 新しいエンコーダの最初のフレームは、必ずキーフレームにする (SPS と PPS をビットストリームの中で送るため)。
        let forceKeyForFirstFrame = true;

        for (;;) {
            if (stopped) return;
            const current = active;
            if (!current) return;

            const failed = current.errorBox.error !== undefined;
            // 作った後に上限が変わった (setLimits) なら、同じ段階から確保し直す。
            const outdated = current.limitsVersion !== limitsVersion;
            if (failed || outdated) {
                // 段階の確保には encoder-created の黒埋め用のフレームが含まれるので、finished() が待てるように登録して
                // から待つ (確保の途中で stop() が呼ばれたときに、黒埋め用のフレームの flush の途中で WebSocket を閉じ
                // ないため)。attemptFromStage は reject しない。フォールバック (failed) は次の段階から、確保し直し
                // (outdated だけ) は同じ段階から試す。
                const fromStage = failed ? nextLadderStage(current.stage) : current.stage;
                const acquisition = deps.gate.run(async (): Promise<ActiveEncoder | StageFailure> => {
                    if (stopped) return { ok: false };
                    // 確保し直しでは、古いエンコーダを先に閉じる (黒埋め用のフレームを送り終えてから閉じる)。
                    // フォールバックでは、失敗したエンコーダなので触らない。
                    if (!failed) await retireEncoder(current);
                    return attemptFromStage(fromStage);
                });
                trackPendingWork(acquisition);
                const next = await acquisition;
                if (stopped) {
                    if (isSuccess(next)) safeClose(next.encoder);
                    return;
                }
                if (!isSuccess(next)) {
                    active = undefined;
                    stats.stage = LadderStage.Unavailable;
                    sink.onStage(LadderStage.Unavailable, undefined);
                    return;
                }
                adoptEncoder(next);
                sinceKeyframe = 0;
                forceKeyForFirstFrame = true;
                // 間引きの周期も、新しいエンコーダの最初のフレームから数える (差し替えた直後のキーフレームを
                // 間引かないため。フォールバックでも同じ)。
                pulled = 0;
                continue;
            }

            const frame = await params.source.next();
            if (stopped) {
                if (frame) frame.close();
                return;
            }
            if (frame === null) return; // フレームがもう来ない

            // タイムスタンプは、取り出した時刻 (arrivalUs) ではなく、メディアのタイムスタンプ (frame.timestamp) を
            // clock_map で送信側の時計に換算した値にする (音声と同じ規則)。fpsDivisor で捨てるフレームも換算には使う
            // (定数を決める期間に見る到着が多いほど、定数が取り出しの遅れの少ない値に近づくため)。
            const arrivalUs = deps.nowUs();
            let mapped: ClockMapResult;
            try {
                mapped = mapMediaTime(clockMap, frame.timestamp, arrivalUs);
            } catch (e) {
                // メディアのタイムスタンプが有限でなければ、mapMediaTime は例外を投げる。そのときも VideoFrame を
                // 残さない (close してから投げ直す)。この例外の行き先 (start() の catch が Unavailable を伝える)
                // は変えない。
                frame.close();
                throw e;
            }
            clockMap = mapped.state;
            // 読み出しの遅れ (latency_span.ts の read)。捨てるフレームも、間引くフレームも数える。
            addDuration(latencySpan.read, Number(arrivalUs - mapped.timeUs));

            // 到着と換算した時刻の差が 200 ms を超えた状態が 1 秒続いたので、メディアの時計が替わったとみなして定数を
            // 決め直した (encode/clock_map.ts)。決め直した後のタイムスタンプは、決め直す前に最後に送ったフレームから
            // 不連続に動く。受信側では、整列 (src-obs/align/timestamp_aligner.h) が予想位置より max(3 周期, 100 ms) 以上
            // 離れたフレームを送信側の時計の不連続として再整列し、録画 (src-obs/record/record_clock.h の classify_pts) は
            // 1 秒以上の進みか戻りでセグメントを閉じて次のセグメントをキーフレームから始める。そのため、決め直した
            // フレームをキーフレームにする (次の一定の間隔のキーフレームまで録画が始まらないことを防ぐ)。
            if (mapped.restart !== undefined) {
                stats.clockReanchors++;
                const info: ClockReanchorInfo = {
                    shiftUs: mapped.restart.shiftUs,
                    staleForUs: mapped.restart.jumpForUs,
                    framesSkipped: staleSkipped,
                };
                staleSkipped = 0;
                keyframeRequested = true;
                sink.onClockReanchor?.(info);
            }

            // 取り出した時刻が、換算したタイムスタンプより STALE_FRAME_SKIP_US 以上遅れているフレームは、エンコード
            // せずに捨てる。fpsDivisor の間引き (`pulled` を増やす所) より前に置き、間引きの周期にもキーフレームの
            // 周期 (sinceKeyframe) にも数えない。間引きは段階の fps に対して等間隔に捨てる (`pulled % fpsDivisor`)
            // ので、捨てたフレームを `pulled` に数えると周期が不規則にずれる。keyframeRequested はここより後でしか
            // 読まないので、捨てたフレームで消費されない (次にエンコードするフレームがキーフレームになる)。
            if (arrivalUs - mapped.timeUs >= STALE_FRAME_SKIP_US) {
                frame.close();
                stats.framesSkippedStale++;
                staleSkipped++;
                continue;
            }
            staleSkipped = 0;

            // 間引きは、このエンコーダを作ったときの上限で行う (エンコーダの fps とそろえる)。
            const shouldEncode = pulled % current.limits.fpsDivisor === 0;
            pulled++;
            if (!shouldEncode) {
                frame.close();
                stats.framesDroppedByDivisor++;
                continue;
            }

            // この段階で実際にエンコードするサイズに縮小してから、エンコードする。縮小が不要な段階 (縮小した後の
            // サイズが source と同じ) では呼ばない。呼んだ時点で、元のフレームの所有権は deps.scaleFrame に移る
            // (この後このファイルが閉じるのは encodeFrame だけ)。scaleFrame が例外を投げたときは、そのまま pumpLoop の
            // 外の catch に任せる (Unavailable をメッセージ、同じ失敗を毎フレーム繰り返さない)。
            const encodeFrame =
                current.format.width === params.source.format.width &&
                current.format.height === params.source.format.height
                    ? frame
                    : deps.scaleFrame(frame, current.format.width, current.format.height);

            // 今回のフレームも、前のキーフレームからの数に入れてから判定する (1 から数え、keyintFrames 番目で
            // キーフレームにする)。
            sinceKeyframe++;
            const isKey = forceKeyForFirstFrame || keyframeRequested || sinceKeyframe >= current.keyintFrames;
            // キーは、エンコーダに渡すフレームの timestamp にする (縮小したフレームも元の値を引き継ぐ約束だが、
            // EncodedVideoChunk.timestamp と必ず同じ値になるほうをキーにする)。
            pendingTimestamps.set(encodeFrame.timestamp, mapped.timeUs);
            const submitUs = deps.monotonicUs?.();
            if (submitUs !== undefined) pendingSubmitUs.set(encodeFrame.timestamp, submitUs);
            try {
                current.encoder.encode(encodeFrame, { keyFrame: isKey });
                if (isKey) {
                    sinceKeyframe = 0;
                    keyframeRequested = false;
                    forceKeyForFirstFrame = false;
                }
            } catch (e) {
                pendingTimestamps.delete(encodeFrame.timestamp);
                pendingSubmitUs.delete(encodeFrame.timestamp);
                current.errorBox.error = e; // 次のループの先頭で検出して、フォールバックする
            } finally {
                encodeFrame.close();
            }
        }
    }

    /**
     * recording-turned-on: 今動いているインスタンスのために、別のインスタンスで黒埋め用のフレームを作る。SerialGate に
     * 渡す (複数のエンコーダを同時に configure しないため)。isConfigSupported は呼ばず、ActiveEncoder.config を
     * そのまま使う。
     */
    function scheduleMaterialOnSpareEncoder(): void {
        const target = active;
        // 閉じ始めたエンコーダ (確保し直しの途中) の黒埋め用のフレームは、閉じる側 (encoder-teardown) が作る。
        if (target === undefined || target.material !== "none" || target.retired) return;
        // 予約として、ここで scheduled にする。渡してから実際に動くまでの間に encoder-teardown が来たら、そちらが
        // この予約を引き取る。
        target.material = "scheduled";

        /** 予約を取り消す (encoder-teardown か、次の setRecording(true) が作り直せるように戻す)。 */
        const releaseReservation = (): void => {
            if (target.material === "scheduled" || target.material === "running") target.material = "none";
        };

        const work = deps.gate
            .run(async () => {
                if (stopped || active !== target || target.retired || target.errorBox.error !== undefined) {
                    releaseReservation();
                    return;
                }
                target.material = "running"; // ここから先は、encoder-teardown が作らない。
                let spare: VideoEncoderLike;
                const spareOut = makeOutput();
                try {
                    spare = deps.createEncoder({ output: spareOut.handle, error: () => undefined });
                } catch {
                    stats.recordingMaterialFailures++;
                    releaseReservation();
                    return;
                }
                try {
                    spare.configure(target.config);
                } catch {
                    safeClose(spare);
                    stats.recordingMaterialFailures++;
                    releaseReservation();
                    return;
                }
                const ok = await runRecordingMaterial(spare, spareOut, target.config, "recording-turned-on");
                safeClose(spare);
                if (ok) target.material = "done";
                else releaseReservation();
            })
            .catch(() => {
                // SerialGate そのものが想定外に失敗した。予約を戻す。
                releaseReservation();
            });
        trackPendingWork(work);
    }

    return {
        async start(): Promise<void> {
            if (started || stopped) return;
            started = true;

            // 最初の段階の確保にも encoder-created の黒埋め用のフレームが含まれるので、finished() が待つ処理にする
            // (起動の途中で止めたときに、黒埋め用のフレームの flush の途中で WebSocket を閉じないため)。
            const acquisition = deps.gate.run(() => attemptFromStage(LadderStage.Native));
            trackPendingWork(acquisition);
            const result = await acquisition;
            if (stopped) {
                if (isSuccess(result)) safeClose(result.encoder);
                return;
            }
            if (!isSuccess(result)) {
                stats.stage = LadderStage.Unavailable;
                sink.onStage(LadderStage.Unavailable, undefined);
                return;
            }

            adoptEncoder(result); // 差し替えた直後の recording-turned-on のやり直しも、ここで行う。

            // 読み出しは deps.gate の外で続ける (SerialGate はエンコーダを操作する区間だけを順に実行するため)。
            // source.next() などが約束に反して reject したときに、start() が返った後の未処理の reject にしないため、
            // ここで catch する。attemptStage() は reject しないが、それでも想定外の失敗が pumpLoop の外に出たときに、
            // ログもメッセージもないまま止まらないよう、Unavailable にして呼び出し元に伝える (フォールバックで送り続け
            // られないときは、分かる形で止まる)。
            pumpLoop().catch(() => {
                if (stopped) return;
                if (active) safeClose(active.encoder);
                active = undefined;
                stats.stage = LadderStage.Unavailable;
                sink.onStage(LadderStage.Unavailable, undefined);
            });
        },

        requestKeyframe(): void {
            keyframeRequested = true;
        },

        stop(): void {
            if (stopped) return;
            stopped = true;
            const current = active;
            active = undefined;
            pendingTimestamps.clear();
            pendingSubmitUs.clear();
            params.source.close();
            if (current === undefined) return;

            // encoder-teardown: 黒埋め用のフレームを作るときだけ、送り終えてから閉じる。作らないときは、ここで閉じる
            // (録画が OFF の間に、停止後に残る処理を作らない)。確保し直しの途中で閉じ始めたエンコーダなら何もしない
            // (retireEncoder は 1 回だけ閉じる)。
            void retireEncoder(current);
        },

        finished(): Promise<void> {
            return waitFinished();
        },

        hasPendingWork(): boolean {
            return pendingWork.size > 0;
        },

        setRecording(on: boolean): void {
            if (recording === on) return; // 変わっていなければ何もしない。
            recording = on;
            if (!on || stopped) return;
            scheduleMaterialOnSpareEncoder(); // recording-turned-on のタイミングで作る。
        },

        setLimits(requested: EncodeSessionLimits): void {
            assertValidLimits(requested);
            if (stopped) return;
            // 下げたキーだけを適用する (上げたキーは、次の画面共有で作るセッションから)。
            const next = tightenLimits(limits, requested);
            if (sameLimits(next, limits)) return;
            limits = next;
            limitsVersion++; // 走っているエンコーダは、pumpLoop の次のループで確保し直す。
        },

        stats(): EncodeSessionStats {
            return { ...stats, clockOriginUs: clockMap.offsetUs };
        },

        takeLatencySpan(): EncodeLatencySpan {
            const taken: EncodeLatencySpan = {
                read: { ...latencySpan.read },
                encode: { ...latencySpan.encode },
                prepare: { ...latencySpan.prepare },
                send: { ...latencySpan.send },
            };
            resetEncodeLatencySpan(latencySpan);
            return taken;
        },

        clockOriginUs(): bigint | undefined {
            return clockMap.offsetUs;
        },

        spsPpsPrependedCount(): number {
            return stats.spsPpsPrependedCount;
        },

        async finalSpsPpsPrependedCount(): Promise<number> {
            // encoder-teardown の黒埋め用のフレームは flush() の後に出るので、後始末が終わってから読む。
            await waitFinished();
            return stats.spsPpsPrependedCount;
        },
    };
}
