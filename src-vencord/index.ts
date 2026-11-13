// SPDX-License-Identifier: Apache-2.0

// Vencord の userplugin としての cordmux の本体 (definePlugin)。start() で adapter (adapter/vencord.ts) を作り、その
// イベントを core (core/state.ts) に渡して、届いたトラックの userId を特定する。映像は encode/session.ts でエンコードし、
// 音声は audio/chunk.ts でチャンクにして、transport/socket.ts で受信側 (cordmux.dll) に送る。トラックの読み出しと
// エンコードは、受信側と接続していて、受信側の media が true の間だけ行う (core/gate.ts)。
// Vencord のモジュールを import するので、リポジトリのルートの tsc では型を解決できず (tsconfig.json の exclude)、
// vitest のテストからも読み込めない。型は、Vencord のソースツリーの中で pnpm test:ts がチェックする。

import definePlugin, { PluginNative, StartAt } from "@utils/types";

import { parseMsid } from "./adapter/connections";
import { createVencordAdapter } from "./adapter/vencord";
import type {
    Adapter,
    AdapterEvent,
    MediaStreamTrackLike,
    PeerStateLike,
    StatsReportLike,
    TrackEvent,
    TrackKind,
} from "./adapter/types";
import { audioOriginUs, createAudioChunker, pushAudio, type AudioChunk } from "./audio/chunk";
import type { AudioSource } from "./audio/audio_source";
import { createAudioTrackSource } from "./audio/track_source";
import {
    decisions,
    initialState,
    isPlausibleVoiceSource,
    reduce,
    userIdForTrackNow,
    type CoreState,
} from "./core/state";
import {
    avatarFetchesToStart,
    avatarFetchTargets,
    AvatarOutbox,
    buildFacts,
    buildProfiles,
    type AvatarCandidate,
    type GateFactsInput,
    type ProfileFact,
    type RecordingFactsInput,
} from "./core/facts";
import { createMediaGate, DEFAULT_MEDIA, type LedgerTrack, type MediaGate, type StopReason } from "./core/gate";
import { AVATAR_CANVAS_SIZE, fetchAvatarPng, type AvatarDeps, type AvatarFetchResult } from "./core/avatar";
import { coalesceInTick } from "./core/coalesce";
import { createWatchRelease } from "./core/watch_release";
import { LogBook } from "./core/logbook";
import { trackProcessorAvailable } from "./core/track_processor";
import {
    decideStreamCheck,
    newerScreenStreamOf,
    rtpGrowth,
    type RtpCounts,
    type ScreenStreamSummary,
} from "./core/stream_check";
import { VIDEO_WATCH_INITIAL, VIDEO_WATCH_STALL_MS, watchVideo, type VideoWatchState } from "./core/video_watch";
import { encodeHeader, FrameType, type FrameHeader } from "./core/wire";
import type { LadderStage } from "./encode/ladder";
import {
    createEncodeSession,
    type EncodedVideoFormat,
    type EncodeSessionDeps,
    type EncodeSessionHandle,
    type EncodeSessionSink,
} from "./encode/session";
import {
    addClockLag,
    clockLagLine,
    createClockLagSamples,
    takeClockLagSummary,
    type ClockLagSamples,
} from "./encode/clock_lag";
import {
    clockOriginLogLine,
    createClockOriginLogState,
    type ClockOriginLogState,
} from "./encode/clock_origin_log";
import { latencyLine } from "./encode/latency_span";
import { encodeChoiceFromWire, limitsFromWire, stageLogLine, type EncodeChoice } from "./encode/limits";
import {
    createPlayoutDiagState,
    originMove,
    playoutReport,
    PLAYOUT_FIRST_REPORT_MS,
    PLAYOUT_REPORT_INTERVAL_MS,
    readInboundRtp,
    readRtpCounts,
    readStatsSafely,
    reanchorShiftUs,
    recordOriginMove,
    recordReanchor,
    shouldReportOnTick,
    type PlayoutDiagResult,
    type PlayoutDiagState,
} from "./encode/playout_diag";
import { pageTimebase, qpcTimebase, timebaseNowUs, type SenderTimebase } from "./encode/sender_clock";
import { measureTicksOrigin, ticksOriginLogLine, type TicksOriginResult } from "./encode/ticks_origin";
import { videoTimeChangeText } from "./encode/video_time";
import { SerialGate } from "./encode/serial";
import { createVideoTrackSource, type VideoMediaStreamTrack } from "./encode/track_source";
import type {
    EncodedVideoChunk,
    EncodedVideoChunkMetadata,
    VideoEncoderConfig,
    VideoEncoderLike,
    VideoEncoderSupport,
    VideoFrame as StructuralVideoFrame,
} from "./encode/webcodecs_types";
import type {
    ConfigureStream,
    ControlMessage,
    LimitsFields,
    ProfileEntry,
    ReceiveScopeFields,
    StreamCheckOutcome,
} from "./transport/control";
import { createSocket, type SocketHandle, type WebSocketLike } from "./transport/socket";
import type { WatchdogReason } from "./transport/watchdog";
import { CORDMUX_BUILD_ID } from "./version";
import { readVesktopVersion } from "./vesktop_version";

/* ==========================================================================
   固定値
   ========================================================================== */

/**
 * ボイス接続の pcId。core/state.ts と adapter/connections.ts にも同じ値の定数があるので、変えるときは 3 か所とも
 * 修正する。
 */
const VOICE_PC_ID = 1;

/**
 * 受信側との接続が切れてから、視聴をやめるまでの時間 (ms)。送信側の再接続は 5 秒以下の間隔で試すので
 * (transport/backoff.ts)、OBS が少し止まっただけなら、この間に繋ぎ直して視聴を続ける。目安の値で、測って決めたものではない。
 */
const WATCH_KEEP_AFTER_CLOSE_MS = 30000;

// トラックの userId を特定できないときに、再試行する間隔と回数 (250 ms x 20 = 5 秒)。5 秒たっても特定できなければ、
// そのトラックは使わない (エンコーダなどを作らない)。合計の 5 秒を adapter/vencord.ts の REDISCOVER_MS (5000 ms) と
// 同じにした目安の値で、測って決めたものではない。
const SSRC_RESOLVE_RETRY_INTERVAL_MS = 250;
const SSRC_RESOLVE_RETRY_MAX = 20;

// エンコードの設定。キーフレームの間隔はライブ配信で一般的な 2 秒、ビットレートは補正なしにする。どちらも目安の
// 値で、測って決めたものではない。ハードウェアかソフトウェアか、速さと画質のどちらを優先するか (latencyMode) は、
// 受信側の設定ウィンドウで選んだ値が届く (encode/limits.ts の encodeChoiceFromWire)。
const VIDEO_KEYINT_SEC = 2;
const VIDEO_BITRATE_FACTOR = 1;

/** hello の version に入れる値 (version.ts)。 */
const WIRE_CLIENT_VERSION = CORDMUX_BUILD_ID;
/** hello の wire に入れる、ワイヤー形式のバージョン。受信側 (src-obs/ws/control_messages.h の kWireVersion) と同じ値にする。 */
const WIRE_VERSION = 1;

const LOG_FLUSH_INTERVAL_MS = 5_000; // ログは、確立した直後に 1 回送り、その後は 5 秒ごとに送る。

/**
 * 映像を送れているかを確認する間隔 (ms)。core/video_watch.ts の 5 秒を、この間隔で確認する。確立している間だけ動かす
 * (playoutTimer と同じ)。止まったと気付くまでの遅れは最大でこの 1 秒で、ログの時刻の読み違いにはならない。
 */
const VIDEO_WATCH_INTERVAL_MS = 1_000;

/**
 * 受信の確認で、getStats() の受信の数を 2 回読む間隔 (ms)。間に増えたフレームの数 (framesReceived) で、Discord の側に
 * 映像が届いているかを見る (core/stream_check.ts の rtpGrowth)。動いている画面共有なら 1 秒に数枚から数十枚増えるので、
 * 1 秒あれば見分けられる。目安の値で、測って決めたものではない。
 */
const STREAM_CHECK_SAMPLE_MS = 1_000;
const FACTS_MIN_INTERVAL_MS = 1_000; // facts は、変わったときだけ、1 秒に 1 回まで送る。

/**
 * stop() で、黒埋め用のフレームを送り終えるのを待つ上限。黒埋め用のフレームは 1 秒分の黒をハードウェアで
 * エンコードするだけなので、数十 ms で終わる見込みだが、測っていない。flush() が返らなくてもソケットを開いたままに
 * しないための上限で、2 秒は目安の値である。
 */
const STOP_MATERIAL_WAIT_MS = 2_000;

/**
 * ページの原点の TimeTicks を求めるのを待つ上限 (ms。encode/ticks_origin.ts)。求める間は受信側と接続しないので、
 * Vesktop のメインのプロセスが答えないときにも接続を始められるようにする。ふつうは 48 回の呼び出しで数十 ms で終わる
 * (テスト用の送信側で、1 回の往復の中央値は 0.2 ms)。目安の値で、測って決めたものではない。
 */
const TICKS_ORIGIN_TIMEOUT_MS = 2_000;

/* ==========================================================================
   ストリームのまとめ方 (画面共有は pcId ごと、声は userId ごと)
   ========================================================================== */

/** レシーバーの getStats()。再生の遅延の診断にだけ使う。adapter が渡さなければない。 */
type ReceiverStats = () => Promise<StatsReportLike>;

/** トラックが届いた接続の状態。映像が届かないときにログに書くためだけに使う。adapter が渡さなければない。 */
type PeerStateReader = () => PeerStateLike | undefined;

interface ScreenVideoComponent {
    readonly trackId: string;
    /** 読み出しているトラック。受信のやり直しで、同じトラックから読み出しを始め直すために持つ。 */
    readonly track: MediaStreamTrackLike;
    readonly session: EncodeSessionHandle;
    /** このセッションを始めた時刻 (Date.now())。映像を送れないまま何秒たったかを、ここから数える。 */
    readonly startedAtMs: number;
    readonly getStats?: ReceiverStats;
    readonly peerState?: PeerStateReader;
}

interface ScreenAudioComponent {
    readonly trackId: string;
    readonly source: AudioSource;
    readonly getStats?: ReceiverStats;
    /** 受け取った時刻 - 換算した時刻 の値 (encode/clock_lag.ts)。pumpAudio() が足し、reportPlayout() が取り出す。 */
    readonly clockLag: ClockLagSamples;
}

/** 1 つの画面共有。Go Live の接続 1 つが 1 人に当たるので、pcId でまとめる。 */
interface ScreenEntry {
    readonly userId: string;
    readonly streamId: number;
    video?: ScreenVideoComponent;
    audio?: ScreenAudioComponent;
}

/** 1 人の声。ボイス接続 (pcId 1) に全員のトラックが載るので、userId でまとめる。 */
interface VoiceEntry {
    readonly streamId: number;
    readonly trackId: string;
    readonly source: AudioSource;
}

/* ==========================================================================
   モジュールの状態 (start() と stop() で作り直す)
   ========================================================================== */

let adapter: Adapter | null = null;
let unsubscribe: (() => void) | null = null;
let unsubscribeTracks: (() => void) | null = null;
let coreState: CoreState = initialState;

let socketHandle: SocketHandle | null = null;
let encodeDeps: EncodeSessionDeps | null = null;

/**
 * 送信側の時計の決め方 (encode/sender_clock.ts)。最初の start() で、受信側と接続を始める前にページの原点の TimeTicks を
 * 求め (encode/ticks_origin.ts)、求められたら qpc、求められなければ page に決める。決めた後は、プラグインを起動し直しても
 * ページが続く間は変えない。ページの原点の TimeTicks はページの間変わらず、stop() の後に続く前の世代の後片付け (黒埋め用の
 * フレーム、閉じる途中のソケットの pong) も同じ時計で値を出すためである。受信側は hello の clock_base で接続ごとに時計を
 * 決める。
 */
let timebase: SenderTimebase = pageTimebase(performance);
/** timebase を決め終えたか。 */
let timebaseDecided = false;
/** 送信側の時計を決めたときのログの行。接続するたびに書く (受信側の診断情報に、接続ごとに残すため)。 */
let timebaseLogLine: string | null = null;

/**
 * 録画が ON か。受信側から届く `recording` だけで変わる (受信側は、値が変わったときと接続した直後に送る)。
 * 接続が切れても OFF に戻さない。戻すと、再接続のたびに OFF から ON に変わったことになり、recording-turned-on の
 * 黒埋め用のフレームを作るための一時的なエンコーダを、毎回作る。切れている間は何も送れないので、値がずれていても
 * 困らない。プラグインの start() では false に戻す。現在の値は、facts の recording で受信側に返す。
 */
let recordingOn = false;
/**
 * 止めたセッションの、キーフレームに SPS と PPS を付け足した回数 (sps_pps_prepended) の合計。動いている
 * セッションの回数と足して facts に入れる。セッションを止めても facts の値が減らないようにするためである。
 * 止めたセッションの回数をここに移すのは、最終の値が出てからにする (stopScreenEntry())。
 */
let spsPpsPrependedRetired = 0;
/**
 * 止めたが、sps_pps_prepended の最終の値がまだ出ていないセッション。最終の値を spsPpsPrependedRetired に移すまでも
 * recordingFacts() が数え続けるので、その間に facts の値が減らない。
 */
const retiringSessions = new Set<EncodeSessionHandle>();

const screenEntries = new Map<number, ScreenEntry>(); // pcId -> entry
/**
 * userId の特定を待っている画面共有のトラック (pcId -> トラック)。先頭のトラックで特定を始める。トラックには
 * ゲートが渡した世代 (token) を付け、userId が分かったときに、現在の世代のトラックだけを使う。
 */
const pendingScreenTracks = new Map<
    number,
    Array<{
        kind: TrackKind;
        track: MediaStreamTrackLike;
        token: number;
        getStats?: ReceiverStats;
        peerState?: PeerStateReader;
    }>
>();

const voiceEntries = new Map<string, VoiceEntry>(); // userId -> entry

/**
 * streamId -> このストリームで送れた映像のフレーム (黒埋め用のフレームを除く) の累計と、最後に送れた時刻 (Date.now())。
 * 映像を送れているかの確認 (core/video_watch.ts) と、受信側の頼みの確認 (core/stream_check.ts) が読む。受信のやり直しで
 * セッションを作り直しても、同じストリームの間は数え続ける。
 */
const videoSentByStreamId = new Map<number, { count: number; lastAtMs: number }>();
/** streamId -> 映像を送れているかの確認の状態 (core/video_watch.ts)。 */
const videoWatchByStreamId = new Map<number, VideoWatchState>();
/** 受信側に頼まれて、今確認しているストリーム。同じストリームの確認を重ねない。 */
const streamChecksInFlight = new Set<number>();
/**
 * streamId -> 受信の確認の、前の結果。同じ結果が続く間 (静止した画面の共有で、30 秒ごとに not_receiving になる間など) は、
 * 異常に関わる行ではなく出来事の行に書く。異常に関わる行は OBS のログにも LOG_WARNING で出るので、同じ行が配信の間
 * ずっと続かないようにするためである。
 */
const streamCheckOutcomeByStreamId = new Map<number, StreamCheckOutcome>();
/** 映像を送れない状態を、異常に関わる行で伝えたストリーム。送れるようになったことも、同じ種類の行で伝える。 */
const videoWatchAlerted = new Set<number>();
/**
 * trackId -> 画面共有のトラックの mute の状態。mute が TRACK_MUTE_LOG_MS 続いたときと、VIDEO_WATCH_STALL_MS 続いたときに、
 * 出来事の行をそれぞれ 1 回だけ書く (watchVideoOnTick)。異常に関わる行 (OBS のログの警告) にはしない。Chromium が RTP の
 * 途切れで受信のトラックを mute にするかは確認していないので、mute にする実装なら、静止した画面の共有のたびに利用者の
 * OBS のログに警告が出てしまうためである。出来事の行は通話の全体が診断情報に残るので、原因を追うにはそれで足りる。
 */
const trackMuteById = new Map<string, { pcId: number; kind: TrackKind; sinceMs: number; logged: boolean; longLogged: boolean }>();
/**
 * mute がこの時間 (ms) 続いたら、出来事の行に書く。WebRTC の受信のトラックの mute は、相手が送るのをやめたときに立ち、
 * 受信を再開すると消える (adapter/types.ts の track_mute)。静止した画面の共有などで、短い mute と unmute が繰り返される
 * と、ログの行がそれだけで埋まるので、1 秒続いたものだけを書く。1 秒は目安の値で、測って決めたものではない。
 */
const TRACK_MUTE_LOG_MS = 1_000;
/** 確立している間、VIDEO_WATCH_INTERVAL_MS ごとに映像を送れているかを確認するタイマー (playoutTimer と同じ扱い)。 */
let videoWatchTimer: ReturnType<typeof setInterval> | null = null;

/**
 * トラックの読み出しとエンコードのゲート。判断は core/gate.ts にあり、このファイルは、トラックの追加と削除、確立と
 * 切断、`media` をゲートに渡し、ゲートが呼ぶ startTrackWork() と stopTrackWork() でストリームを始めたり止めたり
 * するだけである。ゲートは届いたトラックの一覧を持つ。adapter は動いているトラックを届け直さないので、ゲートが
 * 開き直したときにこの一覧から始める。start() で作り、stop() で捨てる (一覧も捨てる)。
 */
let mediaGate: MediaGate | null = null;
/**
 * 受信側から届いた画質の上限 (ワイヤー形式の形のまま。facts にもこのまま入れる)。新しく作るセッションはこの値で
 * 作り、動いているセッションには setLimits() で渡す。確立が切れたら、上限なし ({}) に戻す。古い受信側は
 * `limits` を送らず、新しい受信側の値は、確立の通知より先に hello_ok で届く。
 */
let currentLimitsWire: LimitsFields = {};

/**
 * 次に割り当てる stream_id。画面共有と声に 1 つの通し番号を使い、プラグインの start() から stop() までの間は
 * 使い回さない。使い回すと、受信側に前の configure が残っている間に、前と現在のストリームが一時的に混ざるおそれが
 * ある。実際の枠への割り当ては、受信側が user_id から決めるので、ここでは重ならない番号であればよい。u32 の範囲で
 * 増え続けてよい。
 */
let nextStreamId = 1;

/**
 * start() と stop() のたびに増える世代。userId の特定 (resolveUserId()) やアバターの取得などは止められないので、
 * stop() の後や、start() し直した後に結果が届くことがある。結果を使う前に、始めたときの世代と比べ、違えば何もしない。
 * 前の世代の結果で、新しい世代のストリームや一覧を壊さないためである。
 */
let epoch = 0;

let logFlushTimer: ReturnType<typeof setInterval> | null = null;
let lastFactsJson: string | null = null;
let lastFactsSentAtMs = 0;
let pendingFactsTimer: ReturnType<typeof setTimeout> | null = null;

/**
 * streamId -> 直近の onStage() で知らされたエンコードの段階 (facts の stages)。CoreState は段階を持たないので、
 * ここで持つ。映像が止まったら消す。
 */
const videoStagesByStreamId = new Map<number, LadderStage>();
/**
 * streamId -> 今エンコードしている解像度と fps (configure の width、height、fps)。送信側で縮小するので、元の解像度を
 * configure に入れると、受信側に違う解像度が表示される。最初の段階が決まるまでと Unavailable の間は、値がない。
 */
const videoFormatByStreamId = new Map<number, EncodedVideoFormat>();
/** userId -> 表示名 (profiles の display_name)。adapter.snapshot() から読み直す。 */
const profileNamesByUserId = new Map<string, string>();
/** userId -> ユーザー名 (profiles の username)。adapter.snapshot() から読み直す。 */
const profileUsernamesByUserId = new Map<string, string>();
/** userId -> アバターの URL。adapter.snapshot() から読み直す。 */
const profileAvatarUrlsByUserId = new Map<string, string>();
/** userId -> Discord の user.avatar の値 (profiles の avatar_key)。adapter.snapshot() から読み直す。 */
const profileAvatarKeysByUserId = new Map<string, string>();
/** Bot のアカウントの userId (profiles の bot)。adapter.snapshot() から読み直す。 */
const profileBotUserIds = new Set<string>();
/** Vesktop のアカウントの userId (adapter.snapshot() の selfUser)。通話の外でも profiles に自分の名前を載せるために覚える。 */
let profileSelfUserId: string | null = null;
/**
 * userId -> アバターを取得した結果。取得できなかったときも、URL と一緒に覚える (`png` が undefined)。覚えないと、
 * 発言のたびに届く core のイベントごとに、同じ URL を取得し直すことになる。URL が変わったときだけ取得し直す。
 * `key` は、取得を始めたときの avatar_key で、avatar のメッセージにこの値を入れる (画像と食い違わせないため)。
 */
const avatarsByUserId = new Map<string, { url: string; key: string; png?: string }>();
/** avatar を送る人数の上限 (AVATAR_SEND_WINDOW_MS の間に送る人数)。 */
const AVATAR_SEND_LIMIT = 4;
const AVATAR_SEND_WINDOW_MS = 1000;
/** アバターの送り方 (変わった人だけ、1 秒に上限まで)。 */
const avatarOutbox = new AvatarOutbox(AVATAR_SEND_LIMIT, AVATAR_SEND_WINDOW_MS);
/** 上限で送れなかった人を送り直すタイマー。 */
let avatarRetryTimer: ReturnType<typeof setTimeout> | null = null;
/** 今アバターを取得している userId (同じ人を 2 回取得しないため)。 */
const avatarFetchesInFlight = new Set<string>();
/**
 * 解像度 -> 縮小に使う OffscreenCanvas。解像度ごとに使い回す。フレームごとに作ると、毎秒 30 から 60 枚 x
 * ストリームの数だけ作り直すことになる。start() と stop() で捨てる。
 */
const scaleCanvases = new Map<string, OffscreenCanvas>();
let lastProfilesJson: string | null = null;

// 映像と音声の原点 (送信側の時計とメディアのタイムスタンプの差。encode/clock_map.ts の定数)。原点が決まったときと
// 変わったときに、ログに 1 行書く (encode/clock_origin_log.ts)。最初の行と差が大きく動いたときは出来事の行、ほかは
// 統計の行にする。原点は、映像ではセッションが、音声では pumpAudio() の chunker が持つので、ストリームごとにここに
// コピーする。
/** streamId -> 音声の原点 (pushAudio() の戻り値から読む)。 */
const audioClockOriginByStreamId = new Map<number, bigint | undefined>();
/**
 * streamId -> 映像の原点 (videoSink() がフレームを送るたびに書く)。pumpAudio() が同じストリームの映像の原点を知る
 * ためにある。これがないと、音声から書くときに映像の原点を none と書くので、映像から書いた行との間で、1 秒ごとに
 * 行の中身が入れ替わる。
 */
const videoClockOriginByStreamId = new Map<number, bigint | undefined>();
/** streamId -> 原点のログの、ストリームごとの状態。 */
const clockOriginLogStateByStreamId = new Map<number, ClockOriginLogState>();
/**
 * streamId -> 再生の遅延の診断 (encode/playout_diag.ts) の、ストリームごとの状態。画面共有のストリームを作ったときに
 * 作り、ストリームがなくなったら消す。診断は測るだけで、動作は変えない。
 */
const playoutDiagByStreamId = new Map<number, PlayoutDiagState>();
/** 今 getStats() を待っているストリーム。同じストリームの読み取りを重ねない。 */
const playoutReportsInFlight = new Set<number>();
/**
 * 確立している間、60 秒ごとに再生の遅延の行を書くタイマー。ログを送るタイマー (logFlushTimer) と同じく、
 * handleEstablished() で始め、handleClosed() と stop() で止める。接続していない間は統計も取らない。
 * ストリームごとのタイマーにしないのは、止める場所が増え、止め忘れるとなくなったレシーバーに getStats() を
 * 呼び続けるためである。
 */
let playoutTimer: ReturnType<typeof setInterval> | null = null;

const logBook = new LogBook();

/**
 * 出来事の行を 1 行書く (core/logbook.ts)。ログは受信側に送り、受信側は診断情報 (discord.log) に残し、OBS のログには
 * --verbose のときだけ書く。書くのは、状態が変わったときの行 (接続、トラックの追加と終了と mute、ゲートの開閉、エンコードの
 * 段階の変化、userId を特定できなかったこと、想定外の失敗など)、問題を調べるのに使うものだけである。
 */
function log(line: string): void {
    logBook.event(`${new Date().toISOString()} ${line}`);
}

/** 数秒から 60 秒ごとの統計の行を 1 行書く (clock origin の細かい変化、playout、latency、clock lag)。受信側は直近の行だけを保つ (discord-stats.log)。 */
function logStats(line: string): void {
    logBook.stat(`${new Date().toISOString()} ${line}`);
}

/**
 * 異常に関わる出来事の行を 1 行書く (映像が届かない、受信のやり直し、接続の failed など)。受信側は OBS のログにも
 * LOG_WARNING で書くので、この行には Discord のユーザー ID を書かない (OBS のログは、診断情報と違って ID を置き換えずに
 * 人に渡される)。書いたらすぐに送る (core/logbook.ts の drain() の、行の順のため)。
 */
function logAlert(line: string): void {
    logBook.alert(`${new Date().toISOString()} ${line}`);
    if (isEstablished()) flushLog(socketHandle);
}

/* ==========================================================================
   core (adapter.subscribe) とのつなぎ
   ========================================================================== */

/**
 * adapter のイベントを core に渡し、送るものを送る。core/state.ts の decisions() は、直前の reduce() で新しく
 * 生じた判断だけを返すので、reduce() のたびに読み、返った判断だけを adapter に頼む。"mark-source-switched" (その
 * 人の画面共有の接続か映像の SSRC が替わった) では何もしない。前の枠に新しい映像を表示しないようにするのは、
 * 受信側の役目である。
 *
 * startWatching() の結果は "watch_result" のイベントとして core に返す。core は、失敗したときだけ視聴を頼んだ
 * 状態を取り消す。stop() で adapter が替わった後に届いた結果は無視する (requestedAdapter と比べる)。
 */
function applyEvent(event: AdapterEvent): void {
    coreState = reduce(coreState, event);
    scheduleFactsAfterEvent();
    // 通話に参加している人が変わると、アバターを取得する相手が変わる。アバターの URL は下の
    // refreshProfileSnapshot() が読み直す。
    refreshAvatars(coreState);
    maybeSendProfiles(coreState);
    maybeSendAvatars(coreState);
    // 表示名とアバターの URL は、通話に参加している人が変わったときだけ読み直す。名前は、参加している人と関係の
    // ないイベントでは変わらないので、イベントのたびに snapshot() を呼び出す理由がない。
    if (event.type === "roster" || event.type === "self") refreshProfileSnapshot();
    if (adapter === null) return;
    for (const decision of decisions(coreState)) {
        if (decision.type === "start-watching") {
            const { userId, streamKey } = decision;
            const requestedAdapter = adapter;
            void requestedAdapter.startWatching(userId, streamKey).then((result) => {
                if (adapter !== requestedAdapter) return;
                applyEvent({ type: "watch_result", userId, streamKey, ok: result.ok, reason: result.reason });
            });
        } else if (decision.type === "stop-watching") {
            void adapter.stopWatching(decision.userId, decision.streamKey);
        }
        // "mark-source-switched" は受信側が扱うので、ここでは何もしない。
    }
}

/* ==========================================================================
   subscribeTracks とのつなぎ
   ========================================================================== */

/**
 * トラックがどの userId のものかを特定する。トラックの SSRC (ssrcs() は呼び出すたびに現在の値を返す) を、core が持つ
 * SSRC と userId の対応と比べる。ssrcs() は、統計が届く前は空の配列を返すことがあるので、特定できない間は
 * SSRC_RESOLVE_RETRY_INTERVAL_MS ごとに再試行し、SSRC_RESOLVE_RETRY_MAX 回で打ち切って undefined を返す。
 *
 * `requestedEpoch` は、呼び出し元が呼び出したときの epoch である。epoch が変わったら (stop() の後)、その場で
 * undefined を返して再試行をやめる。呼び出し元の `.then()` で比べるだけでは、stop() の後も 5 秒分のタイマーが
 * 動き続けるためである。`stillWanted` が false になったとき (ゲートが閉じた、トラックが終わった) も同じくやめる。
 */
function resolveUserId(
    pcId: number,
    ssrcs: () => number[],
    requestedEpoch: number,
    stillWanted: () => boolean,
    msid: string | null = null,
): Promise<string | undefined> {
    return new Promise((resolve) => {
        let attempts = 0;
        const tryOnce = (): void => {
            if (epoch !== requestedEpoch || !stillWanted()) {
                resolve(undefined); // stop() の後か、止めたストリーム。ここで再試行をやめる。
                return;
            }
            // Go Live の接続では、adapter がその接続で画面共有している人 (接続の voice gateway が op12 で伝えた人) を
            // 特定し、トラックの msid と食い違わないことを確認してからトラックを渡す (adapter/connections.ts)。SSRC と
            // userId の対応も同じ情報から作られるので、先にこの人を使う。SSRC から先に探すと、SSRC で特定できない
            // ときに 5 秒待つことになり、その間は映像が表示されない。
            // トラックの msid ("<userId>-<ssrc>") が示す userId と、voice gateway が伝えた同じ接続の SSRC の
            // userId が一致すれば、その userId にする。話していない人の声のトラックには音声が届かず、ssrcs() が
            // 空のままになるので、SSRC だけでは 5 秒たっても特定できない。
            const found = userIdForTrackNow(coreState, pcId, {
                owner: pcId !== VOICE_PC_ID ? adapter?.ownerOfConnection?.(pcId) : undefined,
                declared: parseMsid(msid),
                ssrcs: ssrcs(),
            });
            if (found !== undefined) {
                resolve(found);
                return;
            }
            attempts++;
            if (attempts >= SSRC_RESOLVE_RETRY_MAX) {
                // SSRC と userId の対応から特定できなかった。Go Live の接続なら、その接続の voice gateway が伝えた
                // 人にする (Go Live の接続 1 つで画面共有している人は 1 人だけである。adapter が op12 から特定する)。
                // 最初に視聴したときは、SSRC と userId の対応が間に合わないことがある。ボイス接続 (VOICE_PC_ID) には
                // 複数の人の音声が載るので、この方法を使わない。
                const owner = pcId !== VOICE_PC_ID ? adapter?.ownerOfConnection?.(pcId) : undefined;
                resolve(owner);
                return;
            }
            setTimeout(tryOnce, SSRC_RESOLVE_RETRY_INTERVAL_MS);
        };
        tryOnce();
    });
}

/**
 * 送信側の時計の今 (us)。映像と音声の到着の時刻と、黒埋め用のフレームのタイムスタンプに使う。Date.now() は使わない
 * (encode/sender_clock.ts)。
 */
function nowUs(): bigint {
    return timebaseNowUs(timebase, performance.now());
}

/**
 * ページの原点の TimeTicks を求め、送信側の時計の決め方と、そのログの行を返す。求められなければ page にする。reject しない。
 */
async function decideTimebase(): Promise<{ timebase: SenderTimebase; line: string }> {
    let timer: ReturnType<typeof setTimeout> | undefined;
    const timeout = new Promise<TicksOriginResult>((resolve) => {
        timer = setTimeout(
            () => resolve({ ok: false, reason: "call-failed", brackets: undefined, ticks: undefined, clampUs: 0, detail: "timeout" }),
            TICKS_ORIGIN_TIMEOUT_MS,
        );
    });
    let result: TicksOriginResult;
    try {
        result = await Promise.race([
            measureTicksOrigin({
                nowMs: () => performance.now(),
                readHrtimeNs: () => Native.hrtimeNs(),
                crossOriginIsolated: self.crossOriginIsolated === true,
            }),
            timeout,
        ]);
    } catch (e) {
        // measureTicksOrigin は reject しないが、Native の関数がない (古いビルドの native) などで同期に投げたときも、page で続ける。
        result = { ok: false, reason: "call-failed", brackets: undefined, ticks: undefined, clampUs: 0, detail: String(e) };
    } finally {
        if (timer !== undefined) clearTimeout(timer);
    }
    return {
        timebase: result.ok ? qpcTimebase(result.originUs) : pageTimebase(performance),
        line: `${ticksOriginLogLine(result)} coi=${self.crossOriginIsolated === true}`,
    };
}

function concatHeaderPayload(header: FrameHeader, payload: Uint8Array): Uint8Array {
    const headerBytes = encodeHeader(header);
    const out = new Uint8Array(headerBytes.length + payload.length);
    out.set(headerBytes, 0);
    out.set(payload, headerBytes.length);
    return out;
}

/**
 * 映像と音声の原点 (encode/clock_map.ts) をログに書くかを確認し、書くときだけリングバッファーに書く。書くかどうかと、
 * 出来事の行にするか統計の行にするかの判断は、encode/clock_origin_log.ts の純粋関数がする。`videoOriginUs` は
 * 呼び出し元が渡す。videoSink() は自分のセッションから読み、pumpAudio() は videoClockOriginByStreamId から読む。
 */
function maybeLogClockOrigin(streamId: number, videoOriginUs: bigint | undefined): void {
    const audioOriginUs = audioClockOriginByStreamId.get(streamId);
    const state = clockOriginLogStateByStreamId.get(streamId) ?? createClockOriginLogState();
    const result = clockOriginLogLine(state, streamId, videoOriginUs, audioOriginUs, Date.now());
    clockOriginLogStateByStreamId.set(streamId, result.state);
    if (result.line === undefined) return;
    if (result.isEvent) log(result.line);
    else logStats(result.line);
}

/* ==========================================================================
   再生の遅延の診断 (測るだけで、動作は変えない)
   ========================================================================== */

// 画面共有の音声と映像は別々の MediaStream で届き、WebRTC は 2 つの再生のタイミングをそろえない。そのため、
// ストリームごとに、音声と映像の再生の遅延がどれだけ離れているかを測ってログに書く。行の中身と書く条件は
// encode/playout_diag.ts の純粋関数が決め、このファイルは、書くタイミングと、3 つの値 (レシーバーの getStats()、
// 映像の原点の動き、音声の換算の定数の決め直し) を渡す処理だけを持つ。声 (ボイス接続) には映像がないので測らない。
// 診断が失敗しても、映像と音声の送信は止めない。送信の処理に加えた診断の部分は try/catch で囲み、失敗はログにも
// 書かない (フレームとチャンクごとの処理で、ログの行を増やさないため)。

/** ストリームの診断の状態を結果で更新し、書く行があればリングバッファーに書く。 */
function applyPlayoutResult(streamId: number, result: PlayoutDiagResult): void {
    playoutDiagByStreamId.set(streamId, result.state);
    if (result.line !== undefined) logStats(result.line);
}

/**
 * 映像の原点が、前に送ったフレームから 1 フレームの間隔より大きく動いていれば数える。送るフレームごとに呼び出す。
 * 1 フレームの間隔は、configure に入れる fps (videoFormatByStreamId) から求める。動いていなければ何もしない。
 */
function notePlayoutOriginMove(streamId: number, previousUs: bigint | undefined, currentUs: bigint | undefined): void {
    try {
        const move = originMove(previousUs, currentUs, videoFormatByStreamId.get(streamId)?.fps);
        if (move === undefined) return;
        const state = playoutDiagByStreamId.get(streamId);
        if (state === undefined) return;
        applyPlayoutResult(streamId, recordOriginMove(state, streamId, move));
    } catch {
        // 診断が失敗しても、映像の送信は止めない。
    }
}

/** 画面共有の音声の、換算の定数の決め直しを数える (pumpAudio() に渡すコールバック)。 */
function notePlayoutReanchor(streamId: number, shiftUs: bigint): void {
    try {
        const state = playoutDiagByStreamId.get(streamId);
        if (state === undefined) return;
        applyPlayoutResult(streamId, recordReanchor(state, streamId, shiftUs));
    } catch {
        // 診断が失敗しても、音声の読み出しは止めない。
    }
}

/**
 * 画面共有のストリーム 1 つの、再生の遅延の行を書く。getStats() は readStatsSafely() (encode/playout_diag.ts) で
 * 待つ。getStats() が例外を投げても、reject しても、返らなくても、readStatsSafely() は undefined を返すので、ほかの
 * 処理は止まらない。同じストリームの読み取りは重ねない (playoutReportsInFlight)。待った後は、世代と、ストリームが
 * 同じエントリーのままかを確認し、待つ間に数えた値を落とさないように、待った後の最新の状態を更新する。
 */
async function reportPlayout(pcId: number, entry: ScreenEntry): Promise<void> {
    const streamId = entry.streamId;
    if (playoutReportsInFlight.has(streamId) || !playoutDiagByStreamId.has(streamId)) return;
    const requestedEpoch = epoch;
    playoutReportsInFlight.add(streamId);
    try {
        // 待つ前に、どのトラックの統計かを決めておく。待つ間にトラックが替わっても、統計とトラックの組み合わせが
        // ずれないようにするためである。
        const audio = entry.audio;
        const video = entry.video;
        const [audioReport, videoReport] = await Promise.all([
            readStatsSafely(audio?.getStats),
            readStatsSafely(video?.getStats),
        ]);
        if (epoch !== requestedEpoch || screenEntries.get(pcId) !== entry) return;
        const state = playoutDiagByStreamId.get(streamId); // 待つ間に数えた値を含む、最新の状態。
        if (state === undefined) return;
        applyPlayoutResult(
            streamId,
            playoutReport(
                state,
                streamId,
                Date.now(),
                audio === undefined ? undefined : { trackId: audio.trackId, sample: readInboundRtp(audioReport, "audio") },
                video === undefined ? undefined : { trackId: video.trackId, sample: readInboundRtp(videoReport, "video") },
            ),
        );
        // 送信側の段階ごとの所要時間 (encode/latency_span.ts)。playout の行の直後に別の行で書く。足されたものが
        // なければ書かない。取り出すと空に戻るので、行を書けなくても次の期間に持ち越さない。
        const latency = entry.video?.session.takeLatencySpan();
        const latencyText = latency === undefined ? undefined : latencyLine(streamId, latency);
        if (latencyText !== undefined) logStats(latencyText);
        // 受け取った時刻 - 換算した時刻 のパーセンタイル (encode/clock_lag.ts)。latency の行の直後に、映像と音声を 1 行に
        // 書く。どちらも足されたものがなければ書かない。取り出すと空に戻る。
        const lagText = clockLagLine(
            streamId,
            entry.video?.session.takeClockLag(),
            entry.audio === undefined ? undefined : takeClockLagSummary(entry.audio.clockLag),
        );
        if (lagText !== undefined) logStats(lagText);
    } catch (e) {
        log(`playout stream ${streamId}: 行を作れなかった (${String(e)})`);
    } finally {
        // stop() と start() をはさんだときは消さない。新しい世代が、同じ streamId で読み取っている最中かもしれない。
        if (epoch === requestedEpoch) playoutReportsInFlight.delete(streamId);
    }
}

/** 60 秒ごとのタイマーで呼ぶ。最初の行をまだ書いていないストリームは飛ばす (shouldReportOnTick())。 */
function reportPlayoutOnTick(): void {
    const nowMs = Date.now();
    for (const [pcId, entry] of screenEntries) {
        const state = playoutDiagByStreamId.get(entry.streamId);
        if (state === undefined || !shouldReportOnTick(state, nowMs)) continue;
        void reportPlayout(pcId, entry);
    }
}

function videoSink(entry: ScreenEntry, choice: EncodeChoice): EncodeSessionSink {
    const streamId = entry.streamId;
    // このストリームが使うソケットを、ここで覚えておく。encoder-teardown の黒埋め用のフレームは stop() の後の
    // マイクロタスクで送られるので、そのときにモジュールの変数を読むと、null か、作り直した別のソケットになる。
    const socket = socketHandle;
    // 前に送ったフレームの原点。セッションごとに作るクロージャーに持つので、別のセッションの原点とは比べない。
    // videoClockOriginByStreamId から前の値を読むと、トラックが替わった直後に、関係のない 2 つの原点の差を数える。
    let previousOriginUs: bigint | undefined;
    return {
        sendVideo(header, payload): void {
            socket?.sendBinary(concatHeaderPayload(header, payload), "video");
            if (header.flags.recordingOnly) {
                // 黒埋め用のフレームは黒をエンコードしただけで、メディアの時計を動かさない。原点のログにも数えない
                // (数えると、黒埋め用のフレームのたびに原点の行の中身が変わる)。送れたフレームの数にも入れない。
                return;
            }
            noteVideoSent(streamId);
            // 原点は自分のセッションから直接読む (フレームごとに screenEntries を探さない)。stats() ではなく原点だけを
            // 返す関数を使うのは、エンコードの処理でフレームごとにオブジェクトを作らないためである。entry.video は
            // createEncodeSession() が返った直後に入るので、最初のフレームが届くときには必ずある (なければ undefined
            // になり、原点がまだないのと同じに扱われる)。セッションを替えた直後に、止めた前のセッションのフレームが
            // 届いたときは、新しいセッションの原点を読む (startScreenVideo() のコメント)。
            const videoOriginUs = entry.video?.session.clockOriginUs();
            videoClockOriginByStreamId.set(streamId, videoOriginUs);
            maybeLogClockOrigin(streamId, videoOriginUs);
            notePlayoutOriginMove(streamId, previousOriginUs, videoOriginUs);
            previousOriginUs = videoOriginUs;
            // sps_pps_prepended は facts に入れるが、facts を送るきっかけは core のイベントと段階の変化にしかなく、
            // 画面が止まっていると、どちらも起きないまま値だけが変わる。そのため、キーフレームのたびにもきっかけを
            // 作る。キーフレームの間隔は 2 秒なので 1 ストリームあたり毎秒 0.5 回で、facts の 1 秒に 1 回までの
            // 間引きはそのまま動作する。
            if (header.flags.keyframe) maybeSendFacts(coreState);
        },
        onRecordingMaterial(origin, frameCount): void {
            // ワイヤー形式は、黒埋め用のフレームをいつ作ったかを運ばないので、このログの行が受信側に伝える唯一の
            // 手段になる。
            log(`video stream ${streamId}: 黒埋め用のフレームを ${frameCount} 個作った (タイミング: ${origin})`);
        },
        onClockReanchor(info): void {
            // 到着と換算した時刻の差が 200 ms を超えた状態が 1 秒続いたので、メディアの時計が替わったとみなして、
            // 映像の換算の定数を決め直した (encode/clock_map.ts)。その間、このストリームの映像は送れていなかったか、
            // 先の時刻で送っていたので、異常に関わる行にする。
            logAlert(
                `video clock: stream=${streamId} origin moved by ${Number(info.shiftUs / 1000n)}ms ` +
                    `after ${secondsText(Number(info.staleForUs / 1000n))}s off skipped=${info.framesSkipped}`,
            );
        },
        onTimeMode(change, switched): void {
            // 描く予定の時刻から今までの換算に戻ったときは、前提 (受けた画面共有のタイムスタンプは描く予定の時刻) が
            // ストリームの途中で崩れたので、異常に関わる行にする。最初に決めたときと、描く予定の時刻に入ったときは、
            // 出来事の行にする。
            const line = `video time stream ${streamId}: ${videoTimeChangeText(change)}`;
            if (switched && change.mode === "estimated") logAlert(line);
            else log(line);
        },
        onStage(stage, format): void {
            log(stageLogLine(streamId, stage, format, choice));
            // facts の stages に入れる。段階の変化 (最初の段階が決まったときとフォールバック) は applyEvent()
            // (Discord のイベント) を通らないので、ここで送るきっかけを作らないと、次に関係のないイベントが起きるまで
            // 受信側に届かない。
            videoStagesByStreamId.set(streamId, stage);
            // configure の width、height、fps は今エンコードしている解像度なので、段階が変わったら覚え直して、
            // configure を送り直す。
            if (format === undefined) videoFormatByStreamId.delete(streamId);
            else videoFormatByStreamId.set(streamId, format);
            maybeSendFacts(coreState);
            resendConfigure();
        },
    };
}

function sendAudioChunk(streamId: number, chunk: AudioChunk): void {
    const header: FrameHeader = {
        type: FrameType.Audio,
        streamId,
        timestampUs: chunk.timestampUs,
        flags: { keyframe: false, recordingOnly: false },
    };
    // f32 のインターリーブのバイト列。このプロセスが動く x86 と x64 の Electron はリトルエンディアンなので、
    // Float32Array のメモリーの中身をそのままバイト列として送る。
    const payload = new Uint8Array(chunk.samples.buffer, chunk.samples.byteOffset, chunk.samples.byteLength);
    socketHandle?.sendBinary(concatHeaderPayload(header, payload), "audio");
}

/**
 * AudioSource から読み続け、480 サンプルのチャンクにするたびに送る (声と画面共有の音声の両方で使う)。
 * `onReanchor` は、メディアの時計が替わったとみなして換算の定数を決め直したときに、その量を受け取るコールバックで、
 * 画面共有の音声だけが渡す (声の診断の状態を作らないため)。量は、pushAudio() の前後の定数の差である
 * (encode/playout_diag.ts)。`clockLag` も画面共有の音声だけが渡し、断片ごとに 受け取った時刻 - 換算した時刻 を足す
 * (encode/clock_lag.ts)。
 */
async function pumpAudio(
    source: AudioSource,
    streamId: number,
    onReanchor?: (shiftUs: bigint) => void,
    clockLag?: ClockLagSamples,
): Promise<void> {
    let chunker = createAudioChunker(timebase);
    for (;;) {
        const block = await source.next();
        if (block === null) return; // ソースが終わった (close() された)。
        // 届いた時刻は、断片を受け取ったここで測る。タイムスタンプはメディアのタイムスタンプから作る
        // (audio/chunk.ts と encode/clock_map.ts)。
        const arrivalUs = nowUs();
        const before = chunker;
        const result = pushAudio(chunker, block.samples, block.mediaTimestampUs, arrivalUs);
        chunker = result.state;
        if (before.domain === undefined && chunker.domain !== undefined) {
            // 時刻の基準は最初の断片で決まる (audio/chunk.ts)。page と ticks は見積もりなしの換算、estimated は今までの換算。
            const lagMs = (Number(arrivalUs - result.timeUs) / 1000).toFixed(1);
            log(`audio time stream ${streamId}: domain=${chunker.domain} lag=${lagMs}ms`);
        }
        if (onReanchor !== undefined && chunker.clockMap.restarts !== before.clockMap.restarts) {
            const shiftUs = reanchorShiftUs(before, chunker);
            if (shiftUs !== undefined) onReanchor(shiftUs);
        }
        if (clockLag !== undefined) addClockLag(clockLag, Number(arrivalUs - result.timeUs));
        for (const chunk of result.chunks) sendAudioChunk(streamId, chunk);
        audioClockOriginByStreamId.set(streamId, audioOriginUs(chunker));
        maybeLogClockOrigin(streamId, videoClockOriginByStreamId.get(streamId));
    }
}

/** VideoEncoder を、encode/webcodecs_types.ts の VideoEncoderLike として使えるように包む。 */
function createEncoder(init: {
    output(chunk: EncodedVideoChunk, metadata: EncodedVideoChunkMetadata): void;
    error(e: unknown): void;
}): VideoEncoderLike {
    return new VideoEncoder({
        output: (chunk, metadata) => init.output(chunk as unknown as EncodedVideoChunk, metadata as EncodedVideoChunkMetadata),
        error: (e: unknown) => init.error(e),
    }) as unknown as VideoEncoderLike;
}

async function isConfigSupported(config: VideoEncoderConfig): Promise<VideoEncoderSupport> {
    const support = await VideoEncoder.isConfigSupported(config as unknown as globalThis.VideoEncoderConfig);
    return { supported: support.supported === true };
}

/** 黒い VideoFrame を 1 枚作る。エンコーダのウォームアップ (60 フレーム) と黒埋め用のフレームに使う。 */
function makeBlackFrame(width: number, height: number, timestampUs: number): StructuralVideoFrame {
    const canvas = new OffscreenCanvas(width, height);
    const ctx = canvas.getContext("2d");
    if (ctx) {
        ctx.fillStyle = "black";
        ctx.fillRect(0, 0, width, height);
    }
    return new VideoFrame(canvas, { timestamp: timestampUs }) as unknown as StructuralVideoFrame;
}

/** 縮小に使う OffscreenCanvas を、解像度ごとに使い回す (scaleCanvases)。 */
function scaleCanvasFor(width: number, height: number): OffscreenCanvas {
    const key = `${width}x${height}`;
    const cached = scaleCanvases.get(key);
    if (cached) return cached;
    const canvas = new OffscreenCanvas(width, height);
    scaleCanvases.set(key, canvas);
    return canvas;
}

/**
 * フレームを width x height に縮小する。OffscreenCanvas の 2d に drawImage() し、その canvas から新しい VideoFrame を
 * 作る。encode/session.ts との約束で、成功しても失敗しても、元のフレームはこの関数が閉じる。alpha は "discard" に
 * する (画面共有に透過はない)。
 */
function scaleFrame(frame: StructuralVideoFrame, width: number, height: number): StructuralVideoFrame {
    // VideoFrame は encode/webcodecs_types.ts の型より多くを持つ (drawImage() に渡せ、duration を持つ) ので、
    // ここでだけ DOM の VideoFrame の型として扱う。
    const source = frame as unknown as VideoFrame;
    try {
        const canvas = scaleCanvasFor(width, height);
        const ctx = canvas.getContext("2d");
        if (ctx === null) throw new Error("cordmux: OffscreenCanvas 2d context unavailable");
        ctx.drawImage(source, 0, 0, width, height);
        return new VideoFrame(canvas, {
            timestamp: source.timestamp,
            duration: source.duration ?? undefined,
            alpha: "discard",
        }) as unknown as StructuralVideoFrame;
    } finally {
        frame.close();
    }
}

/** 映像のフレームを 1 枚送れたことを数える (videoSentByStreamId)。 */
function noteVideoSent(streamId: number): void {
    const nowMs = Date.now();
    const sent = videoSentByStreamId.get(streamId);
    if (sent === undefined) videoSentByStreamId.set(streamId, { count: 1, lastAtMs: nowMs });
    else {
        sent.count++;
        sent.lastAtMs = nowMs;
    }
}

/**
 * 映像のセッションを止め、sps_pps_prepended の値を止めたセッションの合計に移す。止める理由が確立が切れたこと
 * (disconnected) なら、送り先のない黒埋め用のフレームを作らせない。stopScreenEntry() と、同じストリームの映像の
 * セッションを替えるとき (startScreenVideo()) が使う。
 */
function retireVideoSession(session: EncodeSessionHandle, reason: StopReason | "replaced"): void {
    // stop() は読み出しを止める指示で、録画が ON の間は、encoder-teardown の黒埋め用のフレームを送り終えてから
    // エンコーダを閉じる。ソケットがまだ使えれば、黒埋め用のフレームはそのまま送られる。
    // 確立が切れて止めるときは、送り先のない黒埋め用のフレームを作らせない (socket.ts が捨てて数えるだけになる)。
    // recordingOn は変えないので、再接続した後に作るセッションは、録画が ON のまま始まる。media が false になって
    // 止めるときと、セッションを替えるときは、接続していて届くので、黒埋め用のフレームを作る。
    if (reason === "disconnected") session.setRecording(false);
    session.stop();
    // このセッションの sps_pps_prepended を spsPpsPrependedRetired に移す。移すのは最終の値が出てからにする。
    // encoder-teardown の黒埋め用のフレームで増えた分は stop() が返った後に増えるので、すぐに移すと、その分が一度も
    // facts に入らない。移すまでの間は retiringSessions が数え続けるので、facts の値は減らない。
    // hasPendingWork() は stop() の後に読む (後片付けの処理は stop() の中で始まる)。
    if (session.hasPendingWork()) {
        retiringSessions.add(session);
        const requestedEpoch = epoch; // 前の世代の値を、新しい世代に足さない。
        void session.finalSpsPpsPrependedCount().then((count) => {
            retiringSessions.delete(session);
            if (epoch !== requestedEpoch) return;
            spsPpsPrependedRetired += count;
            maybeSendFacts(coreState); // 最終の値を送るきっかけを、ここで作る。
        });
    } else {
        // 後片付けが残っていないとき (録画が OFF のときなど) は、すぐに値を決める。録画が OFF の間に、非同期の
        // 処理を 1 つも残さないためである。
        spsPpsPrependedRetired += session.spsPpsPrependedCount();
    }
}

/**
 * 画面共有の映像の読み出しとエンコードを始める。同じストリームに映像のセッションがもうあれば、先に止めてから替える。
 * 替えるのは 2 つの場合である。(1) 同じ接続 (同じ pcId) に 2 つ目の映像のトラックが届いた (ネゴシエーションのし直しで
 * トラックが作り直された、など)。(2) 受信のやり直し (restartScreenVideo()) で、同じトラックから読み出しを始め直す。
 * 前のセッションを止めずに替えると、前のセッションは stopTrackWork() から見つからなくなり (trackId が合わない)、
 * エンコーダを持ったまま残る。同じ stream_id に 2 つのセッションのフレームが混ざるおそれもある。
 * stream_id は替えない。受信側の枠の割り当て、経路、録画のセグメントは stream_id と user_id に結び付くので、同じ ID の
 * まま作り直すと configure が変わらず、受信側は、エンコーダを確保し直したとき (setLimits の確保し直しやフォールバック)
 * と同じ道を通る。新しい ID にすると、受信側は古い ID の経路を消して新しい ID の経路を作り、その人の最後のフレームの
 * 時刻 (映像が届いていない人の判定に使う) も一度消える。
 */
function startScreenVideo(
    entry: ScreenEntry,
    track: MediaStreamTrackLike,
    getStats?: ReceiverStats,
    peerState?: PeerStateReader,
): void {
    if (encodeDeps === null) return; // stop() の後に届いたイベント。
    if (!noteTrackProcessorMissing()) return;
    const previous = entry.video;
    if (previous !== undefined) {
        log(`screen video replaced: streamId=${entry.streamId} old=${previous.trackId} new=${track.id}`);
        entry.video = undefined;
        retireVideoSession(previous.session, "replaced");
        videoClockOriginByStreamId.delete(entry.streamId);
        videoWatchByStreamId.delete(entry.streamId);
    }
    const source = createVideoTrackSource(track as VideoMediaStreamTrack, nowUs);
    // エンコードの設定は、このセッションを作る時点の値で決める (動いているセッションには適用ない)。
    const choice = encodeChoiceFromWire(currentLimitsWire);
    const session = createEncodeSession(
        encodeDeps,
        {
            streamId: entry.streamId,
            source,
            preference: choice.preference,
            // 受信側から届いた上限で作る (届いていなければ、上限なしでフレームレートもそのまま)。
            limits: limitsFromWire(currentLimitsWire),
            latencyMode: choice.latencyMode,
            bitrateFactor: VIDEO_BITRATE_FACTOR,
            warmup: true,
            keyintSec: VIDEO_KEYINT_SEC,
        },
        videoSink(entry, choice),
    );
    // getStats は再生の遅延の診断と受信の確認に、peerState はログにだけ使う。
    entry.video = { trackId: track.id, track, session, startedAtMs: Date.now(), getStats, peerState };
    // start() より前に、録画が ON かを伝える。ON なら、encoder-created の黒埋め用のフレームを作る。
    session.setRecording(recordingOn);
    void session.start().catch((e) => {
        // start() は、エンコーダを確保できないときも Unavailable を伝えて resolve する。それでも想定外の失敗で
        // reject したときに、処理されない reject のまま、ログにも残らずに消えないように書く。
        log(`video stream ${entry.streamId}: session.start()が失敗した (${String(e)})`);
    });
}

function startScreenAudio(entry: ScreenEntry, track: MediaStreamTrackLike, getStats?: ReceiverStats): void {
    if (encodeDeps === null) return; // stop() の後に届いたイベント。
    if (!noteTrackProcessorMissing()) return;
    if (entry.audio !== undefined) {
        // 同じストリーム (同じ pcId) に 2 つ目の音声のトラックが届いた (Go Live の接続のネゴシエーションをし直して
        // 音声のトラックが作り直された、op7 で再接続した接続が同じ pcId のまま続いた、など)。1 つのストリームで
        // 読み出しを 2 つ動かすと音声の順番が崩れるので、前のトラックを閉じてから替える。原点のコピーも作り直す。
        log(`screen audio replaced: streamId=${entry.streamId} old=${entry.audio.trackId} new=${track.id}`);
        entry.audio.source.close();
        entry.audio = undefined;
        audioClockOriginByStreamId.delete(entry.streamId);
    }
    const source = createAudioTrackSource(track);
    const clockLag = createClockLagSamples();
    entry.audio = { trackId: track.id, source, getStats, clockLag }; // getStats は再生の遅延の診断にだけ使う。
    const streamId = entry.streamId;
    void pumpAudio(source, streamId, (shiftUs) => notePlayoutReanchor(streamId, shiftUs), clockLag).catch((e) => {
        // audio/track_source.ts の next() は reject しないが、pumpAudio() と sendAudioChunk() の想定外の失敗で
        // reject したときのためにログに書く。
        log(`screen audio stream ${entry.streamId}: pumpAudioが失敗して止まった (${String(e)})`);
    });
}

function attachScreenComponent(
    entry: ScreenEntry,
    kind: TrackKind,
    track: MediaStreamTrackLike,
    getStats?: ReceiverStats,
    peerState?: PeerStateReader,
): void {
    if (kind === "video") {
        startScreenVideo(entry, track, getStats, peerState);
    } else {
        startScreenAudio(entry, track, getStats);
    }
    resendConfigure();
}

/**
 * 画面共有 (Go Live の接続の映像と音声) のトラックの読み出しを始める。ゲート (startTrackWork()) だけが呼び出す。
 * `token` は、ゲートがこのトラックに渡した世代である。同じ接続の最初のトラックで userId の特定を始め、特定できるまでに
 * 届いたトラックは pendingScreenTracks で待つ。userId が分かったら、ストリームを作って stream_id を割り当てる。
 */
function handleScreenTrackAdded(
    pcId: number,
    kind: TrackKind,
    track: MediaStreamTrackLike,
    ssrcs: () => number[],
    token: number,
    getStats?: ReceiverStats,
    peerState?: PeerStateReader,
): void {
    // どの接続のトラックをどのストリームに結び付けたかを、ログで追えるように書く。
    log(`screen track added: pcId=${pcId} kind=${kind} trackId=${track.id} ssrcs=${ssrcs().join(",")}`);
    const existing = screenEntries.get(pcId);
    if (existing) {
        log(`screen track joins existing entry: pcId=${pcId} kind=${kind} streamId=${existing.streamId} userId=${existing.userId}`);
        attachScreenComponent(existing, kind, track, getStats, peerState);
        return;
    }

    const queued = pendingScreenTracks.get(pcId);
    if (queued) {
        queued.push({ kind, track, token, getStats, peerState }); // userId を特定している最中。分かったときに、まとめて使う。
        return;
    }
    pendingScreenTracks.set(pcId, [{ kind, track, token, getStats, peerState }]);
    const requestedEpoch = epoch;
    // 待っているトラックに現在の世代のものが 1 つもなければ、再試行を続ける理由がない。
    const stillWanted = (): boolean =>
        (pendingScreenTracks.get(pcId) ?? []).some((item) => mediaGate?.isCurrent(item.track.id, item.token) === true);

    void resolveUserId(pcId, ssrcs, requestedEpoch, stillWanted).then((userId) => {
        // stop() をはさんだときは、pendingScreenTracks などが新しい世代のものになっているかもしれないので、何にも
        // 触れずに戻る。
        if (epoch !== requestedEpoch) return;

        const waiting = pendingScreenTracks.get(pcId) ?? [];
        pendingScreenTracks.delete(pcId);
        // 特定を待つ間に止めたトラック (ゲートが閉じた、トラックが終わった) からは何も作らない。
        const current = waiting.filter((item) => mediaGate?.isCurrent(item.track.id, item.token) === true);
        if (current.length === 0) return;

        // 特定できなかったトラックは、ゲートが次に開いたときに、もう一度特定する。
        if (userId === undefined) {
            log(`screen track (pcId=${pcId}) の userId を解決できず諦めた`);
            return;
        }
        if (encodeDeps === null) return; // 念のため (stop() の後なら、epoch の比較で既に戻っている)。
        if (screenEntries.has(pcId)) return; // 同じ接続で 2 回特定した。想定していないので、何もしない。

        const entry: ScreenEntry = { userId, streamId: nextStreamId++ };
        screenEntries.set(pcId, entry);
        log(`screen entry: pcId=${pcId} userId=${userId} streamId=${entry.streamId} tracks=${current.map((c) => c.kind).join(",")}`);
        // 再生の遅延の診断の状態を、映像と音声より先に作る。最初のフレームとチャンクから数えるためである。
        playoutDiagByStreamId.set(entry.streamId, createPlayoutDiagState(Date.now()));
        for (const item of current) attachScreenComponent(entry, item.kind, item.track, item.getStats, item.peerState);
        // ストリームを作ってから 10 秒後に、再生の遅延の行を 1 回書く。このタイマーは止めず、時間になったときに、世代と、
        // ストリームが同じエントリーのままかを確認する。
        const firstReportEpoch = epoch;
        setTimeout(() => {
            if (epoch !== firstReportEpoch || screenEntries.get(pcId) !== entry) return;
            void reportPlayout(pcId, entry);
        }, PLAYOUT_FIRST_REPORT_MS);
    });
}

function stopScreenEntry(
    pcId: number,
    entry: ScreenEntry,
    kind: TrackKind,
    trackId: string,
    reason: StopReason,
): void {
    if (kind === "video" && entry.video?.trackId === trackId) {
        retireVideoSession(entry.video.session, reason);
        entry.video = undefined;
        videoWatchByStreamId.delete(entry.streamId);
        // 段階を伝えるセッションがなくなったので、facts の stages からも消す。
        videoStagesByStreamId.delete(entry.streamId);
        videoFormatByStreamId.delete(entry.streamId);
        videoClockOriginByStreamId.delete(entry.streamId);
    } else if (kind === "audio" && entry.audio?.trackId === trackId) {
        entry.audio.source.close();
        entry.audio = undefined;
        audioClockOriginByStreamId.delete(entry.streamId);
    } else {
        return; // 対象の映像か音声がない (想定していない組み合わせ)。
    }
    if (entry.video === undefined && entry.audio === undefined) {
        screenEntries.delete(pcId);
        clockOriginLogStateByStreamId.delete(entry.streamId);
        playoutDiagByStreamId.delete(entry.streamId);
        videoSentByStreamId.delete(entry.streamId);
        streamCheckOutcomeByStreamId.delete(entry.streamId);
        videoWatchAlerted.delete(entry.streamId);
        log(`screen entry stopped: pcId=${pcId} streamId=${entry.streamId} reason=${reason}`);
    }
    resendConfigure();
    maybeSendFacts(coreState); // stages から消えたことも変化として送る。
}

let trackProcessorMissingLogged = false;

/**
 * トラックを読み出せるかを確認し、読み出せなければログに 1 回だけ書いて false を返す (呼び出し元は読み出しを始めない)。
 * MediaStreamTrackProcessor がないと、読み出しを始める `new` が例外を投げ、adapter がそれを捨てて何も伝えないので、
 * ここで止める。ドックへの伝達は、依存箇所の pc-hook の周期の確認が出す (core/track_processor.ts)。
 */
function noteTrackProcessorMissing(): boolean {
    if (trackProcessorAvailable()) return true;
    if (!trackProcessorMissingLogged) {
        trackProcessorMissingLogged = true;
        log("MediaStreamTrackProcessor is unavailable; screen and voice tracks are not read");
    }
    return false;
}

/**
 * 声 (ボイス接続の pcId 1) のトラックの読み出しを始める。ゲート (startTrackWork()) だけが呼び出す。声はすべての人の
 * トラックが同じ pcId に載るので、userId が分かってから userId ごとの VoiceEntry を作る。
 */
function handleVoiceTrackAdded(
    track: MediaStreamTrackLike,
    ssrcs: () => number[],
    token: number,
    msid: string | null,
): void {
    const requestedEpoch = epoch;
    const stillWanted = (): boolean => mediaGate?.isCurrent(track.id, token) === true;

    void resolveUserId(VOICE_PC_ID, ssrcs, requestedEpoch, stillWanted, msid).then((userId) => {
        if (epoch !== requestedEpoch) return; // stop() をはさんだので、何にも触れずに戻る。
        // 特定を待つ間に止めたトラック (ゲートが閉じた、トラックが終わった) からは何も作らない。
        if (mediaGate?.isCurrent(track.id, token) !== true) return;

        if (userId === undefined) {
            log(`voice track (trackId=${track.id}) の userId を解決できず諦めた (ssrcs=${ssrcs().join(",")})`);
            return;
        }
        if (encodeDeps === null) return; // 念のため (stop() の後なら、epoch の比較で既に戻っている)。

        if (!isPlausibleVoiceSource(coreState, userId)) {
            // その人がボイス接続と同じ pcId で画面共有をしていることになっているなら、声として扱わない。画面共有の
            // 音声を声の枠に渡さないためで、判断がつかないときは声として扱わない。adapter は voice gateway の
            // server_id でボイス接続を見分けるので、Go Live の接続が pcId 1 になることはないが、念のために確認する。
            log(`voice track (trackId=${track.id}, userId=${userId}) はpcId=${VOICE_PC_ID}がその人の画面共有とも一致するため、声として扱わない`);
            return;
        }

        if (!noteTrackProcessorMissing()) return;
        const previous = voiceEntries.get(userId);
        if (previous) {
            // 同じ人の前のトラックがまだ残っている (再接続したときなど)。前のトラックを閉じてから替える。
            previous.source.close();
        }

        const streamId = nextStreamId++;
        const source = createAudioTrackSource(track);
        voiceEntries.set(userId, { streamId, trackId: track.id, source });
        void pumpAudio(source, streamId).catch((e) => {
            log(`voice audio stream ${streamId}: pumpAudioが失敗して止まった (${String(e)})`);
        });
        resendConfigure();
    });
}

function handleVoiceTrackRemoved(trackId: string): void {
    for (const [userId, entry] of voiceEntries) {
        if (entry.trackId === trackId) {
            entry.source.close();
            voiceEntries.delete(userId);
            audioClockOriginByStreamId.delete(entry.streamId);
            clockOriginLogStateByStreamId.delete(entry.streamId);
            resendConfigure();
            return;
        }
    }
    // userId が分かる前にトラックが終わったときなど。voiceEntries にまだないので、何もしない。
}

/**
 * adapter.subscribeTracks() のコールバック。トラックの追加と削除を、ゲートのトラックの一覧に渡すだけである。読み出しを
 * 始めたり止めたりするのは、ゲートが startTrackWork() と stopTrackWork() を呼んだときだけである。
 */
function handleTrackEvent(event: TrackEvent): void {
    if (mediaGate === null) return; // stop() の後に届いたイベント。
    if (event.type === "track_added") {
        // ボイス接続の映像のトラック (カメラ) は使わないので、無視してゲートの一覧にも入れない。無視したことはログに
        // 書く。adapter は接続を ICE の ufrag で結び付けるので、画面共有の映像がボイス接続のものと取り違えられる
        // ことはない。SSRC で Go Live の接続を探し直すことはしない。SSRC の番号は、接続が違えば別の人と重なることが
        // ある (実際の通話で重なったことがある)。
        if (event.pcId === VOICE_PC_ID && event.kind !== "audio") {
            log(`video track on the voice connection ignored: trackId=${event.track.id}`);
            return;
        }
        mediaGate.trackAdded({
            pcId: event.pcId,
            kind: event.kind,
            track: event.track,
            ssrcs: event.ssrcs,
            msid: event.msid,
            getStats: event.getStats, // 再生の遅延の診断と、受信の確認にだけ使う。
            peerState: event.peerState, // ログにだけ使う。
        });
        return;
    }
    if (event.type === "track_mute") {
        noteTrackMute(event.pcId, event.kind, event.trackId, event.muted);
        return;
    }
    noteTrackEnded(event.pcId, event.kind, event.trackId);
    mediaGate.trackRemoved(event.trackId);
}

/**
 * 画面共有のトラックの mute と unmute を覚える。ログの行は、mute が続いたときに watchVideoOnTick() が書く。声
 * (ボイス接続) のトラックは扱わない。Discord は話していない人の声の RTP を送らないので、ブラウザーが RTP の途切れで
 * mute にする実装なら、話すたびに mute と unmute が繰り返され、ログが埋まるためである。
 */
function noteTrackMute(pcId: number, kind: TrackKind, trackId: string, muted: boolean): void {
    if (pcId === VOICE_PC_ID) return;
    if (muted) {
        if (!trackMuteById.has(trackId)) {
            trackMuteById.set(trackId, { pcId, kind, sinceMs: Date.now(), logged: false, longLogged: false });
        }
        return;
    }
    const state = trackMuteById.get(trackId);
    trackMuteById.delete(trackId);
    if (state === undefined || !state.logged) return; // 短い mute は書かない (TRACK_MUTE_LOG_MS)。
    log(`track unmuted: pcId=${pcId} kind=${kind} trackId=${trackId} after=${secondsText(Date.now() - state.sinceMs)}s`);
}

/**
 * トラックが終わった (ended) ことをログに書く。画面共有の映像のトラックが、その人がまだ画面共有している (Discord の
 * 状態で) 間に終わったときは、映像が止まる原因になりうるので、異常に関わる行にする。画面共有をやめたときの終わりは
 * ふつうの出来事なので、出来事の行にする。
 */
function noteTrackEnded(pcId: number, kind: TrackKind, trackId: string): void {
    trackMuteById.delete(trackId);
    const found = screenEntries.get(pcId);
    const entry =
        found !== undefined && (found.video?.trackId === trackId || found.audio?.trackId === trackId) ? found : undefined;
    const stillSharing = entry !== undefined && coreState.sharing[entry.userId]?.active === true;
    const line =
        `track ended: pcId=${pcId} kind=${kind} trackId=${trackId}` +
        (entry !== undefined ? ` streamId=${entry.streamId}` : "") +
        (stillSharing ? " still_sharing=true" : "");
    if (kind === "video" && stillSharing) logAlert(line);
    else log(line);
}

/** ゲートが呼ぶコールバック。このトラックの読み出し (と映像のエンコード) を始める。 */
function startTrackWork(entry: LedgerTrack, token: number): void {
    if (entry.pcId === VOICE_PC_ID) {
        handleVoiceTrackAdded(entry.track, entry.ssrcs, token, entry.msid ?? null);
    } else {
        handleScreenTrackAdded(entry.pcId, entry.kind, entry.track, entry.ssrcs, token, entry.getStats, entry.peerState);
    }
}

/**
 * ゲートが呼ぶコールバック。このトラックの読み出し (と映像のエンコード) を止める。まだエントリーがない (userId を
 * 特定している最中か、特定できなかった) トラックでは何もしない。特定の処理が後で終わっても、世代を比べて何も作らない。
 * トラックの stop() は呼ばない。トラックは Discord の RTCPeerConnection が持つもので、止めると Discord の再生も
 * 止まる。
 */
function stopTrackWork(trackId: string, reason: StopReason): void {
    for (const [pcId, entry] of screenEntries) {
        if (entry.video?.trackId === trackId) {
            stopScreenEntry(pcId, entry, "video", trackId, reason);
            return;
        }
        if (entry.audio?.trackId === trackId) {
            stopScreenEntry(pcId, entry, "audio", trackId, reason);
            return;
        }
    }
    handleVoiceTrackRemoved(trackId);
}

/* ==========================================================================
   受信の確認 (stream_check) と、映像を送れているかの確認
   ========================================================================== */

/** ms を、小数 1 桁の秒の文字列にする (ログの行に書く)。 */
function secondsText(ms: number): string {
    return (Math.max(0, ms) / 1000).toFixed(1);
}

function delayMs(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

/** そのレシーバーの受信の累計の数を読む。getStats() がない、失敗した、返らないときは undefined (readStatsSafely())。 */
async function sampleRtp(getStats: ReceiverStats | undefined): Promise<RtpCounts | undefined> {
    return readRtpCounts(await readStatsSafely(getStats), "video");
}

/** streamId の画面共有のストリームを探す。 */
function findScreenEntry(streamId: number): { pcId: number; entry: ScreenEntry } | undefined {
    for (const [pcId, entry] of screenEntries) {
        if (entry.streamId === streamId) return { pcId, entry };
    }
    return undefined;
}

function screenSummaryOf(pcId: number, entry: ScreenEntry): ScreenStreamSummary {
    return {
        pcId,
        streamId: entry.streamId,
        userId: entry.userId,
        videoLive: entry.video !== undefined && entry.video.track.readyState === "live",
    };
}

function screenSummaries(): ScreenStreamSummary[] {
    return [...screenEntries].map(([pcId, entry]) => screenSummaryOf(pcId, entry));
}

/**
 * ログの行に書く、映像のストリームの状態 (トラック、接続、セッションの数)。userId は書かない (異常に関わる行は OBS の
 * ログにも書かれる)。read は読み出してエンコードに回したフレーム、stale は遅れすぎて捨てたフレーム
 * (encode/session.ts の STALE_FRAME_SKIP_US)、divisor は間引いたフレームの数で、トラックからフレームは来ているのに
 * 送れていないのかを、この 3 つで見分ける。reanchor は、遅すぎるフレームが続いたので換算の定数を決め直した回数である。
 */
function videoStateText(video: ScreenVideoComponent): string {
    const peer = video.peerState?.();
    const stats = video.session.stats();
    const muted = video.track.muted === undefined ? "unknown" : String(video.track.muted);
    return (
        `track=${video.trackId} readyState=${video.track.readyState} muted=${muted} ` +
        `pc=${peer?.connectionState ?? "unknown"} ice=${peer?.iceConnectionState ?? "unknown"} ` +
        `stage=${stats.stage} read=${stats.framesEncoded} stale=${stats.framesSkippedStale} ` +
        `divisor=${stats.framesDroppedByDivisor} reanchor=${stats.clockReanchors}`
    );
}

/**
 * 同じトラックから、映像の読み出しとエンコードを始め直す (受信のやり直し)。stream_id は替えない (startScreenVideo() の
 * コメント)。音声は始め直さない。音声のトラックは別に読み出していて、映像が止まった原因に関わらないためである。
 */
function restartScreenVideo(entry: ScreenEntry): void {
    const video = entry.video;
    if (video === undefined || encodeDeps === null) return;
    startScreenVideo(entry, video.track, video.getStats, video.peerState);
}

/**
 * 同じ人の新しい接続の映像が動いているので、古いストリームを止める。トラックをゲートの一覧から外すと、ゲートが
 * stopTrackWork() でストリームを止め、configure から消える。新しい接続の映像は別のストリームとして送っているので、
 * 受信側の枠には新しいストリームの映像が映り続ける。古いトラックの ended が届かないまま残った場合に、古いトラックを
 * 読み出し続けないためである。
 */
function retireScreenEntry(entry: ScreenEntry): void {
    const trackIds = [entry.video?.trackId, entry.audio?.trackId].filter((id): id is string => id !== undefined);
    for (const trackId of trackIds) mediaGate?.trackRemoved(trackId);
}

function replyStreamCheck(streamId: number, outcome: StreamCheckOutcome, detail: string): void {
    socketHandle?.sendControl({ type: "stream_check_result", stream_id: streamId, outcome, detail });
}

/**
 * 受信側から stream_check が届いた (枠に載せている人の映像が、受信側に届かないまま待つ時間がたった)。証拠を集めて、
 * 読み出しとエンコードを始め直すかを決め (core/stream_check.ts)、結果を stream_check_result で返す。getStats() の
 * 受信の数は STREAM_CHECK_SAMPLE_MS あけて 2 回読む。同じストリームの確認は重ねない。
 */
function handleStreamCheck(streamId: number): void {
    void runStreamCheck(streamId).catch((e) => {
        log(`stream check: stream=${streamId} failed (${String(e)})`);
    });
}

async function runStreamCheck(streamId: number): Promise<void> {
    const requestedEpoch = epoch;
    // 頼まれたことは出来事の行にする。受信側が頼んだことを自分のログに書くので、OBS のログに 2 行出さない。
    log(`stream check: requested by the receiver stream=${streamId}`);
    const found = findScreenEntry(streamId);
    if (found === undefined || found.entry.video === undefined) {
        logAlert(`stream check: stream=${streamId} outcome=unknown_stream`);
        replyStreamCheck(streamId, "unknown_stream", "no video for this stream");
        return;
    }
    if (streamChecksInFlight.has(streamId)) {
        log(`stream check: stream=${streamId} is already being checked`);
        return;
    }
    streamChecksInFlight.add(streamId);
    try {
        const { pcId, entry } = found;
        const first = found.entry.video;
        const sentBefore = videoSentByStreamId.get(streamId)?.count ?? 0;
        const rtpBefore = await sampleRtp(first.getStats);
        await delayMs(STREAM_CHECK_SAMPLE_MS);
        if (epoch !== requestedEpoch) return; // stop() をはさんだ。
        const video = entry.video;
        if (screenEntries.get(pcId) !== entry || video === undefined) {
            logAlert(`stream check: stream=${streamId} outcome=unknown_stream (stopped while checking)`);
            replyStreamCheck(streamId, "unknown_stream", "stopped while checking");
            return;
        }
        const rtpAfter = await sampleRtp(video.getStats);
        if (epoch !== requestedEpoch || screenEntries.get(pcId) !== entry || entry.video !== video) return;
        const decision = decideStreamCheck({
            trackEnded: video.track.readyState === "ended",
            newerStreamId: newerScreenStreamOf(screenSummaryOf(pcId, entry), screenSummaries()),
            sentBefore,
            sentAfter: videoSentByStreamId.get(streamId)?.count ?? 0,
            // 待つ間にトラックが替わったら、1 回目の数は別のレシーバーの数なので比べない。
            rtpBefore: video === first ? rtpBefore : undefined,
            rtpAfter,
            muted: video.track.muted,
        });
        const resultLine =
            `stream check: stream=${streamId} pcId=${pcId} outcome=${decision.outcome} ${decision.detail} ${videoStateText(video)}`;
        // 始め直したときと切り替えたときは、毎回異常に関わる行にする。ほかの結果は、前と違うときだけにする
        // (streamCheckOutcomeByStreamId)。
        const acted = decision.outcome === "restarted" || decision.outcome === "superseded";
        if (acted || streamCheckOutcomeByStreamId.get(streamId) !== decision.outcome) logAlert(resultLine);
        else log(resultLine);
        streamCheckOutcomeByStreamId.set(streamId, decision.outcome);
        if (decision.outcome === "restarted") restartScreenVideo(entry);
        else if (decision.outcome === "superseded") retireScreenEntry(entry);
        else if (decision.outcome === "ended") mediaGate?.refresh();
        replyStreamCheck(streamId, decision.outcome, decision.detail);
    } finally {
        // stop() と start() をはさんだときは消さない。新しい世代が、同じ streamId を確認している最中かもしれない。
        if (epoch === requestedEpoch) streamChecksInFlight.delete(streamId);
    }
}

/**
 * 映像を送れないまま VIDEO_WATCH_STALL_MS がたった (core/video_watch.ts)。原因を後から追えるように、トラック、接続、
 * セッションの状態と、RTP が届いているかをログに書く。同じ人の新しい接続の映像が動いていれば、古いストリームを止める。
 * 同じトラックを始め直すかは、ここでは決めない。画面が動かない共有では Discord がフレームを送らないことがあり、
 * 受信側の頼み (stream_check) と同じ証拠で決めるためである。
 */
async function handleVideoStall(pcId: number, entry: ScreenEntry, silentMs: number): Promise<void> {
    const video = entry.video;
    if (video === undefined) return;
    const requestedEpoch = epoch;
    const rtpBefore = await sampleRtp(video.getStats);
    await delayMs(STREAM_CHECK_SAMPLE_MS);
    if (epoch !== requestedEpoch || screenEntries.get(pcId) !== entry || entry.video !== video) return;
    const rtpAfter = await sampleRtp(video.getStats);
    if (epoch !== requestedEpoch || screenEntries.get(pcId) !== entry || entry.video !== video) return;
    const growth = rtpGrowth(rtpBefore, rtpAfter);
    const newer = newerScreenStreamOf(screenSummaryOf(pcId, entry), screenSummaries());
    const line =
        `video watch: stream=${entry.streamId} pcId=${pcId} no frames sent for ${secondsText(silentMs)}s ` +
        `rtp=${growth === undefined ? "unknown" : `+${growth}`} newer_stream=${newer ?? "none"} ${videoStateText(video)}`;
    // Discord からフレームが届いていない (静止した画面の共有など) なら、送れないのは当然なので出来事の行にする。
    // フレームが届いているのに送れていない、届いているかが分からない、新しい接続があるときだけ、異常に関わる行にする。
    // 画面が動かない共有では、5 秒止まるたびにこの行が出るので、全部を異常の行にすると OBS のログが埋まる。
    if (growth === undefined || growth > 0 || newer !== undefined) {
        videoWatchAlerted.add(entry.streamId);
        logAlert(line);
    } else {
        log(line);
    }
    if (newer !== undefined) {
        logAlert(`video watch: stream=${entry.streamId} is superseded by stream=${newer}; stopping the older stream`);
        retireScreenEntry(entry);
    }
}

/** VIDEO_WATCH_INTERVAL_MS ごとに、画面共有の映像を送れているかと、トラックの mute が続いていないかを確認する。 */
function watchVideoOnTick(): void {
    const nowMs = Date.now();
    for (const [pcId, entry] of screenEntries) {
        const video = entry.video;
        if (video === undefined) {
            videoWatchByStreamId.delete(entry.streamId);
            continue;
        }
        const prev = videoWatchByStreamId.get(entry.streamId) ?? VIDEO_WATCH_INITIAL;
        const lastSentMs = videoSentByStreamId.get(entry.streamId)?.lastAtMs;
        const result = watchVideo(prev, nowMs, lastSentMs, video.startedAtMs);
        videoWatchByStreamId.set(entry.streamId, result.state);
        if (result.event?.type === "stalled") {
            const silentMs = result.event.silentMs;
            void handleVideoStall(pcId, entry, silentMs).catch((e) => {
                log(`video watch: stream=${entry.streamId} failed (${String(e)})`);
            });
        } else if (result.event?.type === "resumed") {
            // 止まったことを異常の行で伝えたときだけ、戻ったことも異常の行にする (OBS のログで対になるように)。
            const line = `video watch: stream=${entry.streamId} sending again after ${secondsText(result.event.stalledMs)}s`;
            if (videoWatchAlerted.delete(entry.streamId)) logAlert(line);
            else log(line);
        }
    }
    for (const [trackId, mute] of trackMuteById) {
        const mutedMs = nowMs - mute.sinceMs;
        if (!mute.logged && mutedMs >= TRACK_MUTE_LOG_MS) {
            mute.logged = true;
            log(`track muted: pcId=${mute.pcId} kind=${mute.kind} trackId=${trackId}`);
        }
        if (!mute.longLogged && mutedMs >= VIDEO_WATCH_STALL_MS) {
            mute.longLogged = true;
            log(`track muted for ${secondsText(mutedMs)}s: pcId=${mute.pcId} kind=${mute.kind} trackId=${trackId}`);
        }
    }
}

/** ゲートが開いたか閉じた。閉じている間はアバターを取得しないので、開いたら取得し直す。 */
function handleGateOpenChanged(open: boolean): void {
    log(`media gate: ${open ? "open" : "closed"} (running=${mediaGate?.runningCount() ?? 0})`);
    if (open) refreshAvatars(coreState);
}

/* ==========================================================================
   configure、facts、log の送信
   ========================================================================== */

function buildConfigureStreams(): ConfigureStream[] {
    const streams: ConfigureStream[] = [];
    for (const entry of screenEntries.values()) {
        // 段階が決まるまでと Unavailable の間は、width、height、fps を入れない (videoFormatByStreamId)。適当な値を
        // 入れて後で書き換えることはしない。
        const format = entry.video === undefined ? undefined : videoFormatByStreamId.get(entry.streamId);
        streams.push({
            stream_id: entry.streamId,
            user_id: entry.userId,
            kind: "screen",
            width: format?.width,
            height: format?.height,
            fps: format?.fps,
            audio: entry.audio !== undefined,
        });
    }
    for (const [userId, entry] of voiceEntries) {
        streams.push({ stream_id: entry.streamId, user_id: userId, kind: "voice" });
    }
    return streams;
}

/**
 * configure を送り直す。established でなければ、sendControl() が送らずに捨てる (transport/socket.ts)。
 */
function resendConfigure(): void {
    socketHandle?.sendControl({ type: "configure", streams: buildConfigureStreams() });
}

function sendFactsNow(json: string, facts: Record<string, unknown>): void {
    lastFactsJson = json;
    lastFactsSentAtMs = Date.now();
    socketHandle?.sendControl({ type: "facts", facts });
}

/**
 * facts に入れる、録画についての値。sps_pps_prepended は、オブジェクトを作らずに回数だけを返す関数で読み、止めた
 * セッションの分 (spsPpsPrependedRetired) と足す。値が減らないようにするためである。
 */
function recordingFacts(): RecordingFactsInput {
    let spsPpsPrepended = spsPpsPrependedRetired;
    for (const entry of screenEntries.values()) {
        const session = entry.video?.session;
        if (session !== undefined) spsPpsPrepended += session.spsPpsPrependedCount();
    }
    // 止めたが最終の値がまだ出ていないセッションも、spsPpsPrependedRetired に移すまで数え続ける (値が減らない)。
    for (const session of retiringSessions) spsPpsPrepended += session.spsPpsPrependedCount();
    return { on: recordingOn, spsPpsPrepended };
}

/**
 * 受信側との接続が確立しているか。onEstablished() から onClosed() までの間だけ true を返す。ゲートが持つ値を読み、
 * ソケットの状態は見ない。hello_ok の初期値は、ソケットが established になった後、onEstablished() より前に届く
 * ためである。
 */
function isEstablished(): boolean {
    return socketHandle !== null && mediaGate?.isConnected() === true;
}

/** facts に入れる、ゲートの現在の値 (media と limits)。値にかかわらず必ず入れる。 */
function gateFacts(): GateFactsInput {
    return { media: mediaGate?.media() ?? DEFAULT_MEDIA, limits: currentLimitsWire };
}

/**
 * applyEvent() からの facts の送信。adapter は Flux の 1 つの処理の中で roster と sharing のイベントを続けて送る。
 * イベントごとに送ると、新しい roster と古い sharing の組み合わせがすぐ送られ、受信側が同じ facts の 2 つで枠を
 * 空けるので、人を誤って外す。処理が終わった後に 1 回だけ、そのときの最新の coreState で送る。stop() の後に走っても、
 * maybeSendFacts() の isEstablished() のガードで何も送らない。
 */
const scheduleFactsAfterEvent = coalesceInTick(() => maybeSendFacts(coreState));

/**
 * facts を、変わったときだけ、1 秒に 1 回まで送る。前に送ってから 1 秒以上たっていれば、すぐに送る。1 秒たって
 * いなければ、1 秒たったときに 1 回だけ送るように予約し (予約は 1 つまで)、そのときの最新の coreState から作り
 * 修正して送る。中身は core/facts.ts の純粋関数 buildFacts() が作る。
 */
function maybeSendFacts(state: CoreState): void {
    // 確立していない間は、facts を作らない。送れない値を送ったと覚えると、1 秒の間引きの時計が進み、確立した直後の
    // facts が最大 1 秒遅れる。ストリームを止める処理と hello_ok の初期値の処理は、確立していない間にもこの関数を呼ぶ。
    if (!isEstablished()) return;
    const facts = buildFacts(state, videoStagesByStreamId, recordingFacts(), gateFacts());
    const json = JSON.stringify(facts);
    if (json === lastFactsJson) return;

    const elapsed = Date.now() - lastFactsSentAtMs;
    if (elapsed >= FACTS_MIN_INTERVAL_MS) {
        sendFactsNow(json, facts);
        return;
    }
    if (pendingFactsTimer !== null) return; // 既に 1 回送る予約がある。
    pendingFactsTimer = setTimeout(() => {
        pendingFactsTimer = null;
        if (!isEstablished()) return; // 予約した後に切れた (確立した直後に送り直す)。
        const latest = buildFacts(coreState, videoStagesByStreamId, recordingFacts(), gateFacts());
        const latestJson = JSON.stringify(latest);
        if (latestJson === lastFactsJson) return; // 予約した後に、前に送った値に戻ったときなど。
        sendFactsNow(latestJson, latest);
    }, FACTS_MIN_INTERVAL_MS - elapsed);
}

/** buildProfiles() の各要素に、ユーザー名を読めた人だけ username を加える。 */
function withUsernames(profiles: readonly ProfileFact[]): ProfileEntry[] {
    return profiles.map((profile) => {
        const username = profileUsernamesByUserId.get(profile.user_id);
        return username === undefined ? profile : { ...profile, username };
    });
}

function sendProfilesNow(json: string, profiles: ProfileEntry[]): void {
    lastProfilesJson = json;
    socketHandle?.sendControl({ type: "profiles", profiles });
}

/**
 * profiles を、変わったときだけ送る。facts の 1 秒の間引きはしない。中身は core/facts.ts の buildProfiles() が
 * 作る。確立した直後は、変わっていなくても送り直す (handleEstablished())。アバターの取得と違って、ゲートが閉じて
 * いる間も送る。制御用の JSON を送るだけで、Discord との通信は増えないためである。
 */
function maybeSendProfiles(state: CoreState): void {
    if (socketHandle === null) return;
    const built = buildProfiles(
        state.roster,
        profileNamesByUserId,
        profileAvatarKeysByUserId,
        profileBotUserIds,
        profileSelfUserId,
    );
    if (built === undefined) return; // roster が "unknown" の間は、profiles を送らない。
    const profiles = withUsernames(built);
    const json = JSON.stringify(profiles);
    if (json === lastProfilesJson) return;
    sendProfilesNow(json, profiles);
}

/** avatar を送った記録と、送り直しのタイマーを捨てる。次の maybeSendAvatars() で、全員分を送り直す。 */
function resetAvatarOutbox(): void {
    avatarOutbox.reset();
    if (avatarRetryTimer !== null) {
        clearTimeout(avatarRetryTimer);
        avatarRetryTimer = null;
    }
}

/**
 * 取得できたアバターを、avatar で参加者ごとに送る。送るのは、前に送った avatar_key と違う人だけで、1 秒に送る人数には
 * 上限がある (AvatarOutbox)。上限で残った人は、枠が空いたときに送り直す。ゲートが閉じている間も、取得済みのものは
 * 送る (制御用の JSON を送るだけで、Discord との通信は増えないため)。
 */
function maybeSendAvatars(state: CoreState): void {
    if (socketHandle === null || state.roster === "unknown") return;
    const rosterIds = new Set(state.roster.map((u) => u.userId));
    avatarOutbox.keepOnly(rosterIds);
    const candidates: AvatarCandidate[] = [];
    for (const userId of rosterIds) {
        const cached = avatarsByUserId.get(userId);
        // URL が変わった後の、古い画像は送らない (取得し直した後に送る)。
        if (cached?.png === undefined || cached.url !== profileAvatarUrlsByUserId.get(userId)) continue;
        candidates.push({ userId, avatarKey: cached.key, pngBase64: cached.png });
    }
    const { send, retryInMs } = avatarOutbox.pick(Date.now(), candidates);
    for (const a of send) {
        socketHandle.sendControl({ type: "avatar", user_id: a.userId, avatar_key: a.avatarKey, avatar_png: a.pngBase64 });
    }
    if (retryInMs !== null && avatarRetryTimer === null) {
        const requestedEpoch = epoch;
        avatarRetryTimer = setTimeout(() => {
            avatarRetryTimer = null;
            if (epoch !== requestedEpoch) return; // stop() の後。
            maybeSendAvatars(coreState);
        }, retryInMs);
    }
}

/**
 * アバターを取得する 2 つの関数 (fetch() と、OffscreenCanvas での PNG への変換)。何を取得してよいか (ホスト、
 * ?size=64、Content-Type、サイズの上限) は core/avatar.ts が決める。
 */
const avatarDeps: AvatarDeps = {
    async fetchAvatar(url: string): Promise<AvatarFetchResult> {
        // cookie も認証情報も送らない (credentials: "omit")。cordmux の通信は必要最小限にする。
        const response = await fetch(url, { credentials: "omit", cache: "default" });
        const body = new Uint8Array(await response.arrayBuffer());
        return { ok: response.ok, contentType: response.headers.get("content-type"), body };
    },
    async toPngBase64(body: Uint8Array, contentType: string): Promise<string | null> {
        // TypeScript 5.7 から、Uint8Array<ArrayBufferLike> と BlobPart の型が合わないので、wrapWebSocket() と同じ
        // 形のキャストで避ける (このプロジェクトの Uint8Array は、SharedArrayBuffer を使わない)。
        const blob = new Blob([body as unknown as BlobPart], { type: contentType });
        const bitmap = await createImageBitmap(blob);
        try {
            // canvas は AVATAR_CANVAS_SIZE x AVATAR_CANVAS_SIZE に固定する (core/avatar.ts)。?size=64 を無視して大きな
            // 画像を返す CDN でも、PNG に変換するときに 32 KiB の上限に収めるためである。drawImage() の描画先の幅と
            // 高さを指定して、元の画像の大きさにかかわらず、この正方形に拡大か縮小する。
            const canvas = new OffscreenCanvas(AVATAR_CANVAS_SIZE, AVATAR_CANVAS_SIZE);
            const ctx = canvas.getContext("2d");
            if (ctx === null) return null;
            ctx.drawImage(bitmap, 0, 0, AVATAR_CANVAS_SIZE, AVATAR_CANVAS_SIZE);
            const png = await canvas.convertToBlob({ type: "image/png" });
            const bytes = new Uint8Array(await png.arrayBuffer());
            let binary = "";
            for (const byte of bytes) binary += String.fromCharCode(byte);
            return btoa(binary);
        } finally {
            bitmap.close();
        }
    },
};

/**
 * アバターを取得する相手 (core/facts.ts の avatarFetchTargets()) のうち、URL が変わった人のアバターを取得し直す。
 * 取得できた結果も、できなかった結果も覚える (avatarsByUserId)。取得するのは、ゲートが開いている間だけである
 * (誰を取得するかは avatarFetchesToStart() が決める)。profiles は roster の人の分しか作らないので、roster に含まれない
 * 人のアバターを取得しても送られない。アバターを送らないときは、理由をログに 1 行書く。
 */
function refreshAvatars(state: CoreState): void {
    if (adapter === null) return;
    const targets = avatarFetchTargets(state);
    // targets に含まれない人の結果は捨てる。覚える人数を通話の人数までに抑えるためである。取得する相手と覚える相手を
    // 同じにすることで、roster から抜けたのに画面共有の一覧に残っている人を、捨てては取得し直すことを繰り返さない。
    for (const userId of [...avatarsByUserId.keys()]) {
        if (!targets.has(userId)) avatarsByUserId.delete(userId);
    }
    const fetches = avatarFetchesToStart({
        gateOpen: mediaGate?.isOpen() === true,
        targets,
        urls: profileAvatarUrlsByUserId,
        cached: avatarsByUserId,
        inFlight: avatarFetchesInFlight,
    });
    for (const { userId, url } of fetches) {
        const key = profileAvatarKeysByUserId.get(userId) ?? "";
        avatarFetchesInFlight.add(userId);
        const requestedEpoch = epoch;
        void fetchAvatarPng(avatarDeps, url)
            .then((result) => {
                if (epoch !== requestedEpoch) return; // stop() の後に届いた結果。
                // avatarFetchesInFlight は stop() がまとめて消す。epoch を比べる前に消すと、新しい世代で同じ人の
                // アバターを取得している最中でも、その userId を avatarFetchesInFlight から消してしまうので、epoch を
                // 先に比べる。
                avatarFetchesInFlight.delete(userId);
                if (!result.ok) log(`avatar (userId=${userId}): 送らない (${result.reason})`);
                avatarsByUserId.set(userId, { url, key, png: result.ok ? result.pngBase64 : undefined });
                // 取得している間に URL が変わっていれば、この呼び出しでもう一度取得する。
                refreshAvatars(coreState);
                maybeSendAvatars(coreState);
            })
            .catch((e) => {
                // fetchAvatarPng() は、失敗しても理由を付けて resolve するので、ここには来ない (念のため)。
                avatarFetchesInFlight.delete(userId);
                log(`avatar (userId=${userId}): 取得が想定外に失敗した (${String(e)})`);
            });
    }
}

/**
 * 表示名、ユーザー名、アバターの URL を adapter.snapshot() から読み直す。snapshot() は非同期で、純粋関数にできない
 * ので、roster のイベントのたびに読み直し、結果を覚えてから buildProfiles() に渡す。resolveUserId() と同じく
 * epoch と adapter を比べ、stop() の後に届いた結果は無視する。変わっていれば maybeSendProfiles() で送る。
 */
function refreshProfileSnapshot(): void {
    if (adapter === null) return;
    const requestedAdapter = adapter;
    const requestedEpoch = epoch;
    void requestedAdapter
        .snapshot()
        .then((snap) => {
            if (adapter !== requestedAdapter || epoch !== requestedEpoch) return; // stop() の後に届いた結果。
            profileNamesByUserId.clear();
            profileUsernamesByUserId.clear();
            profileAvatarUrlsByUserId.clear();
            profileAvatarKeysByUserId.clear();
            profileBotUserIds.clear();
            profileSelfUserId = null;
            // Vesktop のアカウント自身。roster にいる間は下の roster の値で上書きされ、同じ値になる。
            if (snap.selfUser !== null) {
                profileSelfUserId = snap.selfUser.userId;
                profileNamesByUserId.set(snap.selfUser.userId, snap.selfUser.name);
                if (snap.selfUser.username !== null) {
                    profileUsernamesByUserId.set(snap.selfUser.userId, snap.selfUser.username);
                }
            }
            for (const user of snap.roster) {
                profileNamesByUserId.set(user.userId, user.name);
                profileAvatarKeysByUserId.set(user.userId, user.avatarKey);
                if (user.bot) profileBotUserIds.add(user.userId);
                if (user.username !== null) profileUsernamesByUserId.set(user.userId, user.username);
                if (user.avatarUrl !== null) profileAvatarUrlsByUserId.set(user.userId, user.avatarUrl);
            }
            refreshAvatars(coreState);
            maybeSendProfiles(coreState);
            maybeSendAvatars(coreState);
        })
        .catch((e) => {
            log(`profiles: snapshot()が失敗した (${String(e)})`);
        });
}

/**
 * 保っているログの行を送る (core/logbook.ts)。送り先を引数で受け取るのは、stop() がモジュールの変数を null にした後、
 * ソケットを閉じる直前にも送るためである。送り先がなければ、行を残したままにする。alerts と stats は、行があるときだけ
 * キーを入れる。
 */
function flushLog(target: SocketHandle | null): void {
    if (target === null || logBook.isEmpty()) return;
    const out = logBook.drain();
    const message: Extract<ControlMessage, { type: "log" }> = { type: "log", lines: out.lines };
    if (out.alerts.length > 0) message.alerts = out.alerts;
    if (out.stats.length > 0) message.stats = out.stats;
    target.sendControl(message);
}

/* ==========================================================================
   transport/socket.ts とのつなぎ
   ========================================================================== */

/**
 * WebSocket を transport/socket.ts の WebSocketLike に包む。WebSocket を WebSocketLike にキャストせず、
 * addEventListener() で受けて on* を呼ぶので、DOM の WebSocket の on* の型の細かい違いを考えなくてよい。
 */
function wrapWebSocket(url: string): WebSocketLike {
    const raw = new WebSocket(url);
    const wrapped: WebSocketLike = {
        get bufferedAmount(): number {
            return raw.bufferedAmount;
        },
        send(data: string | Uint8Array): void {
            if (typeof data === "string") {
                raw.send(data);
                return;
            }
            // TypeScript 5.7 から、Uint8Array は既定で ArrayBufferLike (ArrayBuffer か SharedArrayBuffer) を使う
            // ジェネリック型になり、DOM の WebSocket.send() が受け取る ArrayBufferView<ArrayBuffer> と型が合わない。
            // このプロジェクトの Uint8Array は、どれも `new Uint8Array(n)` で作ったもので、SharedArrayBuffer を
            // 使わない (Vencord の test:ts で型の違いを確認した)。
            raw.send(data as unknown as ArrayBufferView<ArrayBuffer>);
        },
        close(): void {
            raw.close();
        },
        onopen: null,
        onclose: null,
        onerror: null,
        onmessage: null,
    };
    raw.addEventListener("open", () => wrapped.onopen?.());
    raw.addEventListener("close", () => wrapped.onclose?.());
    raw.addEventListener("error", (ev) => wrapped.onerror?.(ev));
    raw.addEventListener("message", (ev) => wrapped.onmessage?.({ data: ev.data }));
    return wrapped;
}

/**
 * 受信側との接続が切れたまま WATCH_KEEP_AFTER_CLOSE_MS がたったら、誰も視聴しない範囲にする (core/watch_release.ts)。
 * 接続し直すと、受信側が現在の範囲を送り直す。
 */
const watchRelease = createWatchRelease(() => {
    log("receive_scope: cleared because the link stayed down");
    applyEvent({ type: "watch_scope", userIds: [] });
}, WATCH_KEEP_AFTER_CLOSE_MS);

/**
 * 受信側と確立した (再接続を含む)。受信側は再接続したときに前の接続の状態を覚えていないものとして、configure、
 * facts、profiles を送り直し、ログを送り始める。
 */
function handleEstablished(): void {
    watchRelease.cancel();
    // ゲートの一覧にある、動いているトラックの読み出しを始める (受信側の media が許していれば)。media と limits の
    // 初期値 (hello_ok) は、これより先に届いている。
    mediaGate?.connected();
    resendConfigure(); // 再接続した直後の受信側には configure がない。
    if (timebaseLogLine !== null) log(timebaseLogLine); // この接続のタイムスタンプの時計 (hello の clock_base と同じ)。
    flushLog(socketHandle); // 確立した直後に、たまったログを送る。
    if (logFlushTimer !== null) clearInterval(logFlushTimer);
    logFlushTimer = setInterval(() => flushLog(socketHandle), LOG_FLUSH_INTERVAL_MS);
    // 再生の遅延の行は、確立している間だけ 60 秒ごとに書く (playoutTimer)。
    if (playoutTimer !== null) clearInterval(playoutTimer);
    playoutTimer = setInterval(reportPlayoutOnTick, PLAYOUT_REPORT_INTERVAL_MS);
    // 映像を送れているかの確認も、確立している間だけ行う (受信側がいなければ、送れなくても困らない)。
    if (videoWatchTimer !== null) clearInterval(videoWatchTimer);
    videoWatchTimer = setInterval(watchVideoOnTick, VIDEO_WATCH_INTERVAL_MS);
    lastFactsJson = null; // 次の maybeSendFacts() で必ず送り直す。
    maybeSendFacts(coreState);
    lastProfilesJson = null;
    // 表示名とアバターの URL も読み直す (受信側が再接続の前の値を覚えているとは考えない)。取得したアバター
    // (avatarsByUserId) はそのまま使う。再接続しても Discord の側は変わらないので、同じ URL を取得し直す理由がない。
    // avatar は受信側が覚えていないものとして、取得済みの全員分を送り直す。
    resetAvatarOutbox();
    refreshProfileSnapshot();
    maybeSendProfiles(coreState);
    maybeSendAvatars(coreState);
}

/**
 * すべてのセッションが黒埋め用のフレームを送り終えてから (または STOP_MATERIAL_WAIT_MS で打ち切って)、ソケットを
 * 閉じる。プラグインの stop() は待たずに返り、後片付けだけが続く。閉じる直前に、ログをもう一度送る
 * (encoder-teardown の黒埋め用のフレームのログの行が、そこで書かれるため)。
 */
function closeSocketAfterMaterial(target: SocketHandle | null, draining: Promise<void>[]): void {
    if (target === null) return;
    if (draining.length === 0) {
        flushLog(target);
        target.stop();
        return;
    }
    let closed = false;
    let timer: ReturnType<typeof setTimeout> | null = null;
    const closeOnce = (): void => {
        if (closed) return;
        closed = true;
        if (timer !== null) clearTimeout(timer);
        flushLog(target);
        target.stop();
    };
    timer = setTimeout(closeOnce, STOP_MATERIAL_WAIT_MS);
    // finished() は reject しないが、念のため、reject したときも閉じる。
    void Promise.all(draining).then(closeOnce, closeOnce);
}

function handleClosed(): void {
    if (logFlushTimer !== null) {
        clearInterval(logFlushTimer);
        logFlushTimer = null;
    }
    if (playoutTimer !== null) {
        // 確立が切れたら、再生の遅延の行も止める (ストリームは、この下でゲートがすべて止める)。
        clearInterval(playoutTimer);
        playoutTimer = null;
    }
    if (videoWatchTimer !== null) {
        clearInterval(videoWatchTimer);
        videoWatchTimer = null;
    }
    // 切れたら、すべてのストリームを止める。media は既定の値 (true) に戻る。
    mediaGate?.disconnected();
    // 上限も既定の値 (上限なし) に戻す。次の受信側の値は hello_ok で届く。
    currentLimitsWire = {};
    // 視聴は、切れてすぐにはやめない (短い切断で映像が止まらないように)。切れたままなら watchRelease がやめる。
    watchRelease.closed();
}

/**
 * 受信側が止まったと見て、socket が接続を切った (transport/watchdog.ts)。理由をログに残す。ログは繋ぎ直した後に
 * 受信側へ送る。この後に handleClosed() が呼ばれる。
 */
function handleWatchdogClose(reason: WatchdogReason): void {
    log(`transport: closed the connection because the receiver looked stopped (${reason})`);
}

/**
 * トラックの読み出しとエンコードを許すかが、cordmux.dll から届いた。hello_ok の初期値は onEstablished() より先に
 * 届く。変わっていなければ何もしない。
 */
function handleMedia(on: boolean): void {
    if (mediaGate === null) return;
    if (!mediaGate.setMedia(on)) return;
    log(`media: receiver set media=${on ? "true" : "false"}`);
    maybeSendFacts(coreState); // facts の media を送るきっかけを、ここで作る。
}

/**
 * 画質の上限が、cordmux.dll から届いた。新しく作るセッションはこの値で作り、動いているセッションには setLimits() で
 * 渡す (下げたキーだけがすぐに適用され、上げたキーは次の画面共有から反映される)。エンコードの設定 (encoder_preference と
 * latency_mode) は動いているセッションには適用ず、次の画面共有から使う。同じ値なら何もしない (受信側は、値が
 * 変わったときと確立した直後に送る)。
 */
function handleLimits(fields: LimitsFields): void {
    const next: LimitsFields = {};
    if (fields.max_long_edge !== undefined) next.max_long_edge = fields.max_long_edge;
    if (fields.fps_divisor !== undefined) next.fps_divisor = fields.fps_divisor;
    if (fields.encoder_preference !== undefined) next.encoder_preference = fields.encoder_preference;
    if (fields.latency_mode !== undefined) next.latency_mode = fields.latency_mode;
    if (JSON.stringify(next) === JSON.stringify(currentLimitsWire)) return;
    currentLimitsWire = next;
    log(
        `limits: max_long_edge=${next.max_long_edge ?? "none"} fps_divisor=${next.fps_divisor ?? "default"} ` +
            `encoder_preference=${next.encoder_preference ?? "default"} latency_mode=${next.latency_mode ?? "default"}`,
    );
    const limits = limitsFromWire(next);
    for (const entry of screenEntries.values()) entry.video?.session.setLimits(limits);
    maybeSendFacts(coreState); // facts の limits を送るきっかけを、ここで作る。
}

/**
 * 録画の ON と OFF が cordmux.dll から届いた。値を覚え、動いているすべてのセッションに伝える (ON なら、
 * recording-turned-on の黒埋め用のフレームを作る)。変わっていなければ何もしない (受信側は、値が変わったときと
 * 接続した直後に送るので、同じ値がもう一度届くことがある)。
 */
function handleRecording(on: boolean): void {
    if (on === recordingOn) return;
    recordingOn = on;
    log(`recording: 録画が ${on ? "ON" : "OFF"} になった (cordmux.dll から)`);
    for (const entry of screenEntries.values()) entry.video?.session.setRecording(on);
    maybeSendFacts(coreState); // facts の recording を送るきっかけを、ここで作る。
}

/**
 * メインアカウントの userId が、cordmux.dll から届いた。空の文字列は、メインアカウントを使わない構成。adapter が
 * メインアカウントがいる通話を読み直し、変わっていれば main_call のイベントを送る。
 */
function handleMainAccount(userId: string): void {
    adapter?.setMainUser(userId === "" ? null : userId);
}

/**
 * 視聴する人の範囲が、cordmux.dll から届いた。core が、範囲に入った人の視聴を始め、外れた人の視聴をやめる。接続が
 * 切れたら、すぐには範囲を変えず、切れたままなら watchRelease が誰も視聴しない範囲にする。同じ範囲が届いたときは何もしない。
 */
function handleReceiveScope(scope: ReceiveScopeFields): void {
    const userIds = scope.scope === "slots" ? scope.user_ids : null;
    if (JSON.stringify(userIds) === JSON.stringify(coreState.watchScope)) return;
    log("receive_scope: " + (userIds === null ? "everyone" : "slots (" + userIds.length + ")"));
    applyEvent({ type: "watch_scope", userIds });
}

/**
 * 利用者が受信側のドックで [移動] を押した。いま押された時点のメインアカウントの通話へ移動する (adapter が読み直す)。
 * 失敗しても、理由をログに書くだけで、やり直さない。
 */
function handleMoveToMainCall(): void {
    const result = adapter?.moveToMainCall() ?? { ok: false, reason: "the adapter is not running" };
    log(result.ok ? "move_to_main_call: moved" : "move_to_main_call: failed (" + (result.reason ?? "unknown") + ")");
}

/** 利用者が受信側のドックで [退出] を押した。現在の通話から退出する。失敗しても、理由をログに書くだけである。 */
function handleLeaveCall(): void {
    const result = adapter?.leaveCall() ?? { ok: false, reason: "the adapter is not running" };
    log(result.ok ? "leave_call: left" : "leave_call: failed (" + (result.reason ?? "unknown") + ")");
}

function handleKeyframeRequest(streamId: number): void {
    for (const entry of screenEntries.values()) {
        if (entry.streamId === streamId) {
            entry.video?.session.requestKeyframe();
            return;
        }
    }
    // 声はエンコードせずにそのまま送るので、キーフレームはない。
}

const Native = VencordNative.pluginHelpers.cordmux as PluginNative<typeof import("./native")>;

/** 受信側との接続を作って始める (start() が、送信側の時計を決めた後に呼ぶ)。 */
function startTransport(): void {
    socketHandle = createSocket(
        {
            makeWebSocket: wrapWebSocket,
            readSession: () => Native.readSession(),
            setTimer: (fn, ms) => setTimeout(fn, ms),
            clearTimer: (handle) => clearTimeout(handle as ReturnType<typeof setTimeout>),
            version: WIRE_CLIENT_VERSION,
            wireVersion: WIRE_VERSION,
            // Vesktop のバージョン。同期の IPC なので、起動ごとに 1 回だけ読む (再接続では読み直さない)。Vesktop の
            // バージョンは、プロセスが動いている間は変わらない。
            vesktopVersion: readVesktopVersion(globalThis),
            // hello と pong に載せる送信側の時計。受信側が、送信側の時計と OBS の時計の差を決めるのに使う。
            clockUs: () => Number(nowUs()),
            // 送信側の時計が OBS の時計と同じ QueryPerformanceCounter の us なら、hello で伝える (受信側は差を 0 にする)。
            clockBase: timebase.base === "qpc" ? "qpc" : undefined,
        },
        {
            onEstablished: handleEstablished,
            onClosed: handleClosed,
            onKeyframeRequest: handleKeyframeRequest,
            onStreamCheck: handleStreamCheck,
            onRecording: handleRecording,
            onMedia: handleMedia,
            onLimits: handleLimits,
            onMainAccount: handleMainAccount,
            onReceiveScope: handleReceiveScope,
            onMoveToMainCall: handleMoveToMainCall,
            onLeaveCall: handleLeaveCall,
            onWatchdogClose: handleWatchdogClose,
        },
    );
    socketHandle.start();
}

export default definePlugin({
    name: "cordmux",
    // Vencord のプラグインの一覧に表示する説明。Vencord の description は文字列 1 つで、言語ごとに分けられないので、
    // 英語の 1 文の後に日本語の 1 文を続ける。Vencord の画面は英語だけで、一覧のカードの説明は 2 行で切れる
    // (Vencord の AddonCard.css の .vc-addon-note) ので、最初に読まれる英語を先にする。止め方は cordmux のドックに
    // あるので、ここには書かない。
    description: "Sends the screen shares and voices in your call to cordmux in OBS. 通話の画面共有と声を OBS の cordmux へ送ります。",
    // 作者は名前 (GitHub の ID) だけを表示する。Vencord のプラグインの画面は、id が 0n なら name だけを表示し、
    // Discord のアカウントの id なら、そのアカウントの名前とアバターを表示する (Vencord v1.15.17 の PluginModal.tsx)。
    authors: [{ name: "eoforgu0", id: 0n }],
    // Vencord の userplugin は、既定では無効のまま起動するので、常に有効にする。cordmux を使うかどうかは、OBS の
    // ドックの機能の ON と OFF で切り替える。
    required: true,
    startAt: StartAt.Init,

    start() {
        epoch++; // start() のたびに新しい世代に入る。
        coreState = initialState;
        screenEntries.clear();
        pendingScreenTracks.clear();
        voiceEntries.clear();
        nextStreamId = 1;
        // ゲートと画質の上限は、プラグインを起動するたびに作り直す (トラックの一覧も空から始める)。
        mediaGate = createMediaGate({
            start: startTrackWork,
            stop: stopTrackWork,
            onOpenChanged: handleGateOpenChanged,
        });
        currentLimitsWire = {};
        lastFactsJson = null;
        lastFactsSentAtMs = 0;
        videoStagesByStreamId.clear();
        videoFormatByStreamId.clear();
        profileNamesByUserId.clear();
        profileUsernamesByUserId.clear();
        profileAvatarUrlsByUserId.clear();
        profileAvatarKeysByUserId.clear();
        profileBotUserIds.clear();
        profileSelfUserId = null;
        avatarsByUserId.clear();
        avatarFetchesInFlight.clear();
        resetAvatarOutbox();
        scaleCanvases.clear();
        lastProfilesJson = null;
        audioClockOriginByStreamId.clear();
        videoClockOriginByStreamId.clear();
        clockOriginLogStateByStreamId.clear();
        playoutDiagByStreamId.clear();
        playoutReportsInFlight.clear();
        videoSentByStreamId.clear();
        videoWatchByStreamId.clear();
        streamChecksInFlight.clear();
        streamCheckOutcomeByStreamId.clear();
        videoWatchAlerted.clear();
        trackMuteById.clear();
        // 録画は OFF から始める (受信側は、接続した直後に現在の値を送る)。接続が切れても OFF に戻さないのは接続が
        // 切れたときの話で、プラグインを起動し直したときは OFF に戻す。
        recordingOn = false;
        spsPpsPrependedRetired = 0;

        adapter = createVencordAdapter({ diag: log });
        unsubscribe = adapter.subscribe(applyEvent);
        unsubscribeTracks = adapter.subscribeTracks(handleTrackEvent);

        encodeDeps = {
            createEncoder,
            isConfigSupported,
            gate: new SerialGate(), // エンコーダの確保は、すべてのストリームで 1 つずつ順に行う。
            nowUs,
            timebase,
            monotonicUs: () => performance.now() * 1000,
            makeBlackFrame,
            scaleFrame,
        };

        // 送信側の時計を決めてから、受信側と接続する (hello の clock_base と、すべてのタイムスタンプが同じ時計の上に
        // あるように)。映像と音声の読み出しは接続した後に始まる (core/gate.ts) ので、それまでに決まっている。
        if (timebaseDecided) {
            startTransport(); // ページの原点の TimeTicks は、前の start() で決めたものを使い続ける。
            return;
        }
        const startedEpoch = epoch;
        void decideTimebase().then((decided) => {
            if (epoch !== startedEpoch) return; // 待つ間に stop() か、次の start() が呼ばれた。次の start() が求め直す。
            timebase = decided.timebase;
            timebaseLogLine = decided.line;
            timebaseDecided = true;
            if (encodeDeps !== null) encodeDeps = { ...encodeDeps, timebase };
            startTransport();
        });
    },

    stop() {
        epoch++; // stop() でも世代を進め、途中の処理の結果を使わないようにする。
        // ソケットはここでは閉じず、encoder-teardown の黒埋め用のフレームを送り終えてから閉じる。モジュールの変数は先に
        // null にし、この後このソケットに送るのは、videoSink() が覚えたソケットだけにする。
        const closingSocket = socketHandle;
        socketHandle = null;
        encodeDeps = null;
        // ゲートを捨てる (トラックの一覧も捨てる)。ストリームは、この下ですべて止める。userId を特定している途中の
        // 処理は、epoch と isCurrent() を比べて、何も作らない。
        mediaGate = null;
        currentLimitsWire = {};

        if (logFlushTimer !== null) {
            clearInterval(logFlushTimer);
            logFlushTimer = null;
        }
        if (playoutTimer !== null) {
            clearInterval(playoutTimer);
            playoutTimer = null;
        }
        if (videoWatchTimer !== null) {
            clearInterval(videoWatchTimer);
            videoWatchTimer = null;
        }
        if (pendingFactsTimer !== null) {
            clearTimeout(pendingFactsTimer);
            pendingFactsTimer = null;
        }
        watchRelease.cancel();

        const drainingSessions: Promise<void>[] = [];
        for (const entry of screenEntries.values()) {
            if (entry.video !== undefined) {
                entry.video.session.stop();
                // 待つのは、後片付けの処理が実際に残っているセッションだけにする。録画が OFF で段階の確保も終わって
                // いれば、stop() の中でエンコーダを閉じるので処理が残らず、ソケットもすぐに閉じる。受信側は 1 つの
                // クライアントしか受け付けず、2 つ目を 503 で断る (src-obs/ws/listener.cpp)。そのため、閉じるのが
                // 遅れると、プラグインを起動し直したときの最初の接続が断られ、再接続の間隔の分だけ確立が遅れる。
                // 段階の確保の最中は、その中で encoder-created の黒埋め用のフレームを作ることがあるので待つ。
                // hasPendingWork() は stop() の後に読む (後片付けの処理は stop() の中で始まる)。
                if (entry.video.session.hasPendingWork()) {
                    drainingSessions.push(entry.video.session.finished());
                }
            }
            entry.audio?.source.close();
        }
        closeSocketAfterMaterial(closingSocket, drainingSessions);
        screenEntries.clear();
        // 止めている途中のセッションも、ここで捨てる (ソケットを閉じるので facts はもう送らない。届く最終の値は、
        // epoch を比べて、新しい世代に足さない)。
        retiringSessions.clear();
        pendingScreenTracks.clear();
        videoStagesByStreamId.clear();
        videoFormatByStreamId.clear();
        profileNamesByUserId.clear();
        profileUsernamesByUserId.clear();
        profileAvatarUrlsByUserId.clear();
        profileAvatarKeysByUserId.clear();
        profileBotUserIds.clear();
        profileSelfUserId = null;
        avatarsByUserId.clear();
        avatarFetchesInFlight.clear();
        resetAvatarOutbox();
        scaleCanvases.clear();
        audioClockOriginByStreamId.clear();
        videoClockOriginByStreamId.clear();
        clockOriginLogStateByStreamId.clear();
        playoutDiagByStreamId.clear();
        playoutReportsInFlight.clear();
        videoSentByStreamId.clear();
        videoWatchByStreamId.clear();
        streamChecksInFlight.clear();
        streamCheckOutcomeByStreamId.clear();
        videoWatchAlerted.clear();
        trackMuteById.clear();

        for (const entry of voiceEntries.values()) entry.source.close();
        voiceEntries.clear();

        unsubscribeTracks?.();
        unsubscribeTracks = null;
        unsubscribe?.();
        unsubscribe = null;
        adapter = null;
        coreState = initialState;
    },
});
