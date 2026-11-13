/*
 * clock-probe モード (HARNESS_MODE=clock-probe) の本体。esbuild で dist/clock_probe.js に束ね、index-clock-probe.html が読む。
 * 受けた映像と音声のタイムスタンプが何の時計かを、Discord を使わずに確かめる (README.md の「clock-probe モード」)。
 * 計算は clock_probe_pure.ts にある。
 *
 * 同じページの中に WebRTC の接続を 2 組作る。STUN と TURN のサーバーを指定せず、ICE の候補はこのページの中だけで渡すので、
 * 接続の相手は同じページの中の接続だけである。
 * - 両方向の組 (A と B): A は映像と音声を B に送り、B は小さい映像を A に送り返す。送り返すのは、B の受信の側に往復の
 *   時間が出て、受けたフレームに取り込みの時刻が付くようにするため。
 * - 受けるだけの組 (C と D): C は A と同じ映像と音声を D に送り、D は何も送らない (Discord の画面共有を受けるときと同じ形)。
 *   D に渡す offer からは rcvr-rtt と rrtr を除き、D が RTCP XR で往復の時間を測れないようにする (withoutNonSenderRtt)。
 * B と D が受けた映像は、MediaStreamTrackProcessor と、<video> の requestVideoFrameCallback の 2 か所で読む。受けた音声は
 * MediaStreamTrackProcessor で読む。
 *
 * 音を出さない: 送る音声は AudioContext の発振器から作り、MediaStreamAudioDestinationNode にだけつなぐ (ctx.destination には
 * つながない)。AudioContext は出力の装置を使わない指定 (sinkId の type が none) で作る。受けた音声は、再生されないと WebRTC が
 * 取り出さないことがあるので、待っても届かなければ、ミュートで音量 0 の <audio> に付ける。<video> もミュートにする。
 */

import {
    classifyVideoTimestamps,
    deltas,
    estimateOrigin,
    histogram,
    lagSeriesUs,
    leadSeriesUs,
    originCandidates,
    originFromBrackets,
    pairByRtp,
    renderDelayResidual,
    summarize,
    tickProfile,
    timestampDomain,
    videoRtcpAttributes,
    withoutNonSenderRtt,
    type CallbackFrame,
    type HrtimeBracket,
    type ProcessorFrame,
} from "./clock_probe_pure";

/** preload.js が渡す関数と値のうち、このモードが使うもの。 */
interface ClockProbeBridge {
    report(obj: unknown): Promise<unknown>;
    progress(obj: unknown): void;
    /** メインのプロセスの process.hrtime.bigint() (ナノ秒) を 10 進の文字列で返す。同期の IPC で呼ぶ。 */
    hrtimeNs(): string;
    env: Record<string, unknown>;
}

/** MediaStreamTrackProcessor と MediaStreamTrackGenerator は Chromium のウィンドウにだけあり、lib.dom にない。 */
interface TrackProcessor<T> {
    readable: ReadableStream<T>;
}
declare const MediaStreamTrackProcessor: new <T>(init: { track: MediaStreamTrack; maxBufferSize?: number }) => TrackProcessor<T>;
declare const MediaStreamTrackGenerator: new (init: { kind: "video" }) => MediaStreamTrack & { writable: WritableStream<VideoFrame> };

const WIDTH = 640;
const HEIGHT = 360;
/** B から A へ送り返す映像。往復の時間を出すためだけのものなので、小さく少なくする。 */
const RETURN_WIDTH = 320;
const RETURN_HEIGHT = 180;
const RETURN_FPS = 15;
const AUDIO_SAMPLE_RATE = 48_000;
/** 受けた音声がこの時間だけ届かなければ、ミュートの <audio> に付ける。 */
const AUDIO_ELEMENT_WAIT_MS = 2000;
/** process.hrtime をはさむ間隔。 */
const BRACKET_INTERVAL_MS = 100;
const STATS_INTERVAL_MS = 5000;
/** performance.now() を続けて読む時間。 */
const TICK_PROFILE_MS = 20;
/** render_time の残りの差が、これを超えたら飛びとして数える。 */
const JUMP_US = 1000;
/** 送り始めてからこの時間の後に読んだフレームを、落ち着いた後のまとめ (steady) に入れる。 */
const STEADY_AFTER_MS = 10_000;
const MAIN_FPS_DEFAULT = 60;
const SECONDS_DEFAULT = 60;

const bridge = (window as unknown as { harness: ClockProbeBridge }).harness;

function errorText(e: unknown): string {
    if (e instanceof Error) return `${e.name}: ${e.message}`;
    return String(e);
}

function sleep(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, Math.max(0, ms)));
}

/* ==========================================================================
   送る映像と音声
   ========================================================================== */

interface FrameSource {
    track: MediaStreamTrack;
    /** generator は MediaStreamTrackGenerator に VideoFrame を書く。canvas は canvas.captureStream(0) と requestFrame。 */
    kind: "generator" | "canvas";
    draw(index: number): void;
}

function paint(ctx: OffscreenCanvasRenderingContext2D | CanvasRenderingContext2D, width: number, height: number, index: number): void {
    ctx.fillStyle = "#303030";
    ctx.fillRect(0, 0, width, height);
    const size = Math.round(height / 4);
    const x = (index * 7) % (width - size);
    const y = (index * 3) % (height - size);
    ctx.fillStyle = "#e0a030";
    ctx.fillRect(x, y, size, size);
    ctx.fillStyle = "#ffffff";
    ctx.font = `${Math.round(height / 8)}px sans-serif`;
    ctx.fillText(String(index), 8, Math.round(height / 6));
}

function makeFrameSource(width: number, height: number): FrameSource {
    if (typeof MediaStreamTrackGenerator === "function") {
        const canvas = new OffscreenCanvas(width, height);
        const ctx = canvas.getContext("2d");
        if (ctx === null) throw new Error("OffscreenCanvas の 2d が作れない");
        const generator = new MediaStreamTrackGenerator({ kind: "video" });
        const writer = generator.writable.getWriter();
        return {
            track: generator,
            kind: "generator",
            draw(index: number): void {
                paint(ctx, width, height, index);
                const frame = new VideoFrame(canvas, { timestamp: Math.round(performance.now() * 1000) });
                writer.write(frame).catch(() => frame.close());
            },
        };
    }
    const canvas = document.createElement("canvas");
    canvas.width = width;
    canvas.height = height;
    const ctx = canvas.getContext("2d");
    if (ctx === null) throw new Error("canvas の 2d が作れない");
    const track = canvas.captureStream(0).getVideoTracks()[0] as CanvasCaptureMediaStreamTrack | undefined;
    if (track === undefined) throw new Error("canvas.captureStream の映像のトラックがない");
    return {
        track,
        kind: "canvas",
        draw(index: number): void {
            paint(ctx, width, height, index);
            track.requestFrame();
        },
    };
}

interface ToneSource {
    track: MediaStreamTrack;
    ctx: AudioContext;
    /** AudioContext の sinkId の type (出力の装置を使わないなら none)。読めなければ null。 */
    sinkType: string | null;
}

function makeToneSource(): ToneSource {
    // 出力の装置を使わない指定。指定が使えない版では既定の装置になるが、ctx.destination に何もつながないので無音である。
    const options = { sampleRate: AUDIO_SAMPLE_RATE, sinkId: { type: "none" } } as AudioContextOptions;
    const ctx = new AudioContext(options);
    const oscillator = ctx.createOscillator();
    oscillator.frequency.value = 440;
    const gain = ctx.createGain();
    gain.gain.value = 0.1;
    const destination = ctx.createMediaStreamDestination();
    oscillator.connect(gain);
    gain.connect(destination);
    oscillator.start();
    const track = destination.stream.getAudioTracks()[0];
    if (track === undefined) throw new Error("MediaStreamAudioDestinationNode の音声のトラックがない");
    const sink = (ctx as unknown as { sinkId?: unknown }).sinkId;
    let sinkType: string | null = null;
    if (typeof sink === "string") sinkType = sink === "" ? "default" : "device";
    else if (sink !== null && typeof sink === "object" && typeof (sink as { type?: unknown }).type === "string") {
        sinkType = (sink as { type: string }).type;
    }
    return { track, ctx, sinkType };
}

/** fps の間隔で onTick を呼ぶ。遅れたら待たずに続け、100 ms を超えて遅れたら今の番号へ飛ぶ。 */
async function tickLoop(fps: number, endMs: number, onTick: (index: number) => void): Promise<void> {
    const t0 = performance.now();
    let index = 0;
    for (;;) {
        let target = t0 + (index * 1000) / fps;
        const now = performance.now();
        if (now - target > 100) {
            index = Math.floor(((now - t0) * fps) / 1000);
            target = t0 + (index * 1000) / fps;
        }
        if (target >= endMs) return;
        if (target > now) await sleep(target - now);
        onTick(index);
        index++;
    }
}

/* ==========================================================================
   接続
   ========================================================================== */

function vp8Codecs(): RTCRtpCodec[] {
    const caps = RTCRtpReceiver.getCapabilities("video");
    if (caps === null) return [];
    return caps.codecs.filter((c) => c.mimeType.toLowerCase() === "video/vp8");
}

/** ICE の候補を集め終えるまで待つ。候補はこのページの中だけで、SDP に入れて渡す。 */
function waitGathering(pc: RTCPeerConnection): Promise<void> {
    if (pc.iceGatheringState === "complete") return Promise.resolve();
    return new Promise((resolve) => {
        const timer = setTimeout(resolve, 5000);
        const onChange = (): void => {
            if (pc.iceGatheringState !== "complete") return;
            clearTimeout(timer);
            pc.removeEventListener("icegatheringstatechange", onChange);
            resolve();
        };
        pc.addEventListener("icegatheringstatechange", onChange);
    });
}

/** SDP の候補の数と、mDNS の名前 (.local) を使っているか。アドレスそのものは記録しない。 */
interface CandidateFacts {
    count: number;
    mdns: boolean;
}

function candidateFacts(sdp: string | undefined): CandidateFacts {
    const lines = (sdp ?? "").split(/\r?\n/).filter((l) => l.startsWith("a=candidate:"));
    return { count: lines.length, mdns: lines.some((l) => l.includes(".local ")) };
}

/** 接続の SDP について記録すること。 */
interface NegotiationFacts {
    offerCandidates: CandidateFacts;
    answerCandidates: CandidateFacts;
    /** 答える側に渡した offer と、答えた answer の、映像の RTCP の指定。 */
    offerRtcp: string[];
    answerRtcp: string[];
}

/**
 * offer と answer を交わす。mungeOffer があれば、答える側には書き換えた offer を渡す (送る側の local の SDP は
 * 書き換えない)。
 */
async function negotiate(
    offerer: RTCPeerConnection,
    answerer: RTCPeerConnection,
    beforeAnswer: () => Promise<void>,
    mungeOffer: (sdp: string) => string = (sdp) => sdp,
): Promise<NegotiationFacts> {
    await offerer.setLocalDescription(await offerer.createOffer());
    await waitGathering(offerer);
    const offer = offerer.localDescription;
    if (offer === null) throw new Error("offer がない");
    const offerSdp = mungeOffer(offer.sdp);
    await answerer.setRemoteDescription({ type: "offer", sdp: offerSdp });
    await beforeAnswer();
    await answerer.setLocalDescription(await answerer.createAnswer());
    await waitGathering(answerer);
    const answer = answerer.localDescription;
    if (answer === null) throw new Error("answer がない");
    await offerer.setRemoteDescription(answer);
    return {
        offerCandidates: candidateFacts(offer.sdp),
        answerCandidates: candidateFacts(answer.sdp),
        offerRtcp: videoRtcpAttributes(offerSdp),
        answerRtcp: videoRtcpAttributes(answer.sdp),
    };
}

function receiverTrack(pc: RTCPeerConnection, kind: "video" | "audio"): MediaStreamTrack {
    const t = pc.getTransceivers().find((x) => x.receiver.track.kind === kind);
    if (t === undefined) throw new Error(`${kind} の受信のトランシーバーがない`);
    return t.receiver.track;
}

/* ==========================================================================
   受けた映像と音声を読む
   ========================================================================== */

interface CallbackRecord extends CallbackFrame {
    /** コールバックの引数の now (ページの原点からのミリ秒)。 */
    nowMs: number;
    expectedDisplayTimeMs: number;
    mediaTime: number;
    presentedFrames: number;
}

interface VideoSink {
    label: string;
    processor: ProcessorFrame[];
    callbacks: CallbackRecord[];
    /** 最初に読んだ VideoFrame の metadata() のうち、数と文字列と真偽の値。 */
    firstMetadata: Record<string, unknown> | null;
    processorError: string | null;
    playError: string | null;
    cancel: () => void;
}

interface AudioSample {
    readMs: number;
    timestampUs: number;
    frames: number;
    sampleRate: number;
}

interface AudioSink {
    label: string;
    samples: AudioSample[];
    /** ミュートの <audio> に付けた時刻 (performance.now())。付けなければ null。 */
    elementAttachedAtMs: number | null;
    error: string | null;
    cancel: () => void;
}

function plainValues(obj: Record<string, unknown>): Record<string, unknown> {
    const out: Record<string, unknown> = {};
    for (const [k, v] of Object.entries(obj)) {
        if (typeof v === "number" || typeof v === "string" || typeof v === "boolean") out[k] = v;
        else out[k] = typeof v;
    }
    return out;
}

function metadataOf(frame: VideoFrame): Record<string, unknown> | null {
    const fn = (frame as unknown as { metadata?: () => Record<string, unknown> }).metadata;
    if (typeof fn !== "function") return null;
    try {
        return fn.call(frame);
    } catch {
        return null;
    }
}

function newVideoSink(label: string): VideoSink {
    return { label, processor: [], callbacks: [], firstMetadata: null, processorError: null, playError: null, cancel: () => {} };
}

async function readVideo(track: MediaStreamTrack, sink: VideoSink, isStopped: () => boolean): Promise<void> {
    let reader: ReadableStreamDefaultReader<VideoFrame>;
    try {
        reader = new MediaStreamTrackProcessor<VideoFrame>({ track, maxBufferSize: 30 }).readable.getReader();
    } catch (e) {
        sink.processorError = errorText(e);
        return;
    }
    sink.cancel = () => {
        reader.cancel().catch(() => {});
    };
    for (;;) {
        let result: ReadableStreamReadResult<VideoFrame>;
        try {
            result = await reader.read();
        } catch (e) {
            if (!isStopped()) sink.processorError = errorText(e);
            return;
        }
        if (result.done) return;
        const readMs = performance.now();
        const frame = result.value;
        if (!isStopped()) {
            const md = metadataOf(frame);
            const rtp = md !== null && typeof md.rtpTimestamp === "number" ? md.rtpTimestamp : null;
            sink.processor.push({ readMs, timestampUs: frame.timestamp, rtp });
            if (sink.firstMetadata === null && md !== null) sink.firstMetadata = plainValues(md);
        }
        frame.close();
    }
}

/** 受けた映像を <video> に付け、requestVideoFrameCallback で読む。要素は文書に付け、見える大きさにする。 */
function watchVideo(track: MediaStreamTrack, sink: VideoSink, isStopped: () => boolean): HTMLVideoElement {
    const video = document.createElement("video");
    video.muted = true;
    video.autoplay = true;
    video.playsInline = true;
    video.width = 64;
    video.height = 36;
    document.body.appendChild(video);
    video.srcObject = new MediaStream([track]);
    video.play().catch((e: unknown) => {
        sink.playError = errorText(e);
    });
    const onFrame = (now: number, md: VideoFrameCallbackMetadata): void => {
        if (isStopped()) return;
        sink.callbacks.push({
            nowMs: now,
            rtp: md.rtpTimestamp ?? null,
            captureTimeMs: md.captureTime ?? null,
            receiveTimeMs: md.receiveTime ?? null,
            expectedDisplayTimeMs: md.expectedDisplayTime,
            mediaTime: md.mediaTime,
            presentedFrames: md.presentedFrames,
        });
        video.requestVideoFrameCallback(onFrame);
    };
    video.requestVideoFrameCallback(onFrame);
    return video;
}

function newAudioSink(label: string): AudioSink {
    return { label, samples: [], elementAttachedAtMs: null, error: null, cancel: () => {} };
}

async function readAudio(track: MediaStreamTrack, sink: AudioSink, isStopped: () => boolean): Promise<void> {
    let reader: ReadableStreamDefaultReader<AudioData>;
    try {
        reader = new MediaStreamTrackProcessor<AudioData>({ track }).readable.getReader();
    } catch (e) {
        sink.error = errorText(e);
        return;
    }
    sink.cancel = () => {
        reader.cancel().catch(() => {});
    };
    for (;;) {
        let result: ReadableStreamReadResult<AudioData>;
        try {
            result = await reader.read();
        } catch (e) {
            if (!isStopped()) sink.error = errorText(e);
            return;
        }
        if (result.done) return;
        const readMs = performance.now();
        const data = result.value;
        if (!isStopped()) {
            sink.samples.push({ readMs, timestampUs: data.timestamp, frames: data.numberOfFrames, sampleRate: data.sampleRate });
        }
        data.close();
    }
}

/** 待っても音声が届かなければ、ミュートで音量 0 の <audio> に付けて、WebRTC に取り出させる。 */
async function attachAudioElementIfSilent(track: MediaStreamTrack, sink: AudioSink): Promise<HTMLAudioElement | null> {
    await sleep(AUDIO_ELEMENT_WAIT_MS);
    if (sink.samples.length > 0) return null;
    const audio = document.createElement("audio");
    audio.muted = true;
    audio.volume = 0;
    audio.autoplay = true;
    document.body.appendChild(audio);
    audio.srcObject = new MediaStream([track]);
    audio.play().catch((e: unknown) => {
        sink.error = errorText(e);
    });
    sink.elementAttachedAtMs = performance.now();
    return audio;
}

/* ==========================================================================
   時計の読み
   ========================================================================== */

/** performance.now() を続けて読み、刻みの様子と、異なる値の先頭の数個を返す。 */
function sampleTicks(): { profile: ReturnType<typeof tickProfile>; firstDistinctMs: number[] } {
    const reads: number[] = [];
    const end = performance.now() + TICK_PROFILE_MS;
    for (;;) {
        const v = performance.now();
        reads.push(v);
        if (v >= end) break;
    }
    const firstDistinct: number[] = [];
    for (let i = 0; i < reads.length && firstDistinct.length < 50; i++) {
        if (i === 0 || reads[i] !== reads[i - 1]) firstDistinct.push(reads[i] as number);
    }
    return { profile: tickProfile(reads), firstDistinctMs: firstDistinct };
}

function bracket(): HrtimeBracket {
    const beforeMs = performance.now();
    const hrtimeNs = bridge.hrtimeNs();
    const afterMs = performance.now();
    return { beforeMs, hrtimeNs, afterMs };
}

/** getStats() の中から、時刻と遅延に関わる値だけを残す。 */
function pickStats(report: RTCStatsReport): Array<Record<string, unknown>> {
    const codecs = new Map<string, string>();
    report.forEach((s: Record<string, unknown>) => {
        if (s.type === "codec" && typeof s.id === "string" && typeof s.mimeType === "string") codecs.set(s.id, s.mimeType);
    });
    const keep: Record<string, string[]> = {
        "inbound-rtp": [
            "kind",
            "framesReceived",
            "framesDecoded",
            "framesDropped",
            "packetsLost",
            "jitter",
            "jitterBufferDelay",
            "jitterBufferTargetDelay",
            "jitterBufferMinimumDelay",
            "jitterBufferEmittedCount",
            "totalDecodeTime",
            "totalProcessingDelay",
            "decoderImplementation",
            "estimatedPlayoutTimestamp",
        ],
        "outbound-rtp": ["kind", "framesEncoded", "framesSent", "encoderImplementation", "packetsSent"],
        "remote-inbound-rtp": ["kind", "roundTripTime", "roundTripTimeMeasurements", "totalRoundTripTime"],
        "remote-outbound-rtp": ["kind", "roundTripTime", "roundTripTimeMeasurements", "remoteTimestamp", "reportsSent"],
        "candidate-pair": ["state", "nominated", "currentRoundTripTime"],
    };
    const out: Array<Record<string, unknown>> = [];
    report.forEach((s: Record<string, unknown>) => {
        const fields = typeof s.type === "string" ? keep[s.type] : undefined;
        if (fields === undefined) return;
        if (s.type === "candidate-pair" && s.nominated !== true) return;
        const row: Record<string, unknown> = { type: s.type };
        for (const f of fields) if (s[f] !== undefined) row[f] = s[f];
        if (typeof s.codecId === "string" && codecs.has(s.codecId)) row.codec = codecs.get(s.codecId);
        out.push(row);
    });
    return out;
}

/* ==========================================================================
   まとめ
   ========================================================================== */

function columns<T extends object>(rows: readonly T[], keys: ReadonlyArray<keyof T>): Record<string, unknown[]> {
    const out: Record<string, unknown[]> = {};
    for (const k of keys) out[String(k)] = rows.map((r) => r[k]);
    return out;
}

/**
 * 受けた映像をまとめる。両方向の組は、往復の時間が出るまでの最初の数秒だけ描く予定の時刻で、その後は取り込みの時刻に
 * 変わるので、steadyFromMs (performance.now()) 以降に読んだフレームだけのまとめ (steady) も別に出す。
 */
function summarizeVideo(sink: VideoSink, originUs: number | null, steadyFromMs: number): Record<string, unknown> {
    const steadyFrames = sink.processor.filter((f) => f.readMs >= steadyFromMs);
    const steadyResidual = renderDelayResidual(steadyFrames, JUMP_US);
    const { residualUs: _steadySeries, ...steadyResidualSummary } = steadyResidual;
    const steadyLead = originUs === null ? [] : leadSeriesUs(steadyFrames, originUs);
    const steadySteps = deltas(steadyFrames.map((f) => f.timestampUs));
    const steady = {
        fromMs: steadyFromMs,
        frames: steadyFrames.length,
        classification: classifyVideoTimestamps(steadyFrames, sink.callbacks.some((c) => c.captureTimeMs !== null), originUs),
        leadUs: summarize(steadyLead),
        leadUsHistogram1ms: histogram(steadyLead, 1000),
        timestampStepUs: summarize(steadySteps),
        timestampStepUsHistogram1ms: histogram(steadySteps, 1000),
        renderDelayResidual: steadyResidualSummary,
        renderDelayResidualUs: summarize(steadyResidual.residualUs),
    };
    const pairs = pairByRtp(sink.processor, sink.callbacks);
    const withCapture = sink.callbacks.filter((c) => c.captureTimeMs !== null);
    const withReceive = sink.callbacks.filter((c) => c.receiveTimeMs !== null);
    const timestamps = sink.processor.map((f) => f.timestampUs);
    const steps = deltas(timestamps);
    const lead = originUs === null ? [] : leadSeriesUs(sink.processor, originUs);
    const renderMinusReceive =
        originUs === null
            ? []
            : pairs
                  .filter((p) => p.callback.receiveTimeMs !== null)
                  .map((p) => p.processor.timestampUs - originUs - Math.round((p.callback.receiveTimeMs as number) * 1000));
    const residual = renderDelayResidual(sink.processor, JUMP_US);
    const { residualUs: _residualSeries, ...residualSummary } = residual;
    return {
        label: sink.label,
        processorFrames: sink.processor.length,
        callbacks: sink.callbacks.length,
        callbacksWithCaptureTime: withCapture.length,
        callbacksWithReceiveTime: withReceive.length,
        firstCaptureTimeCallbackNowMs: withCapture[0]?.nowMs ?? null,
        pairs: pairs.length,
        firstMetadata: sink.firstMetadata,
        processorError: sink.processorError,
        playError: sink.playError,
        classification: classifyVideoTimestamps(sink.processor, withCapture.length > 0, originUs),
        leadUs: summarize(lead),
        leadUsHistogram1ms: histogram(lead, 1000),
        renderMinusReceiveUs: summarize(renderMinusReceive),
        timestampStepUs: summarize(steps),
        timestampStepUsHistogram1ms: histogram(steps, 1000),
        renderDelayResidual: residualSummary,
        renderDelayResidualUs: summarize(residual.residualUs),
        // WebRTC の時刻はミリ秒で持つものがあるので、値の細かさを見る。
        timestampsOnMillisecond: timestamps.filter((t) => t % 1000 === 0).length,
        steady,
    };
}

function summarizeAudio(sink: AudioSink, originUs: number | null): Record<string, unknown> {
    const first = sink.samples[0];
    const lag = lagSeriesUs(sink.samples);
    const steps = deltas(sink.samples.map((s) => s.timestampUs));
    return {
        label: sink.label,
        count: sink.samples.length,
        elementAttachedAtMs: sink.elementAttachedAtMs,
        error: sink.error,
        domain: first === undefined ? null : timestampDomain(first.timestampUs, first.readMs, originUs),
        lagUs: summarize(lag),
        lagUsHistogram1ms: histogram(lag, 1000),
        timestampStepUs: summarize(steps),
        timestampStepUsHistogram500us: histogram(steps, 500),
        framesPerData: summarize(sink.samples.map((s) => s.frames)),
        sampleRates: [...new Set(sink.samples.map((s) => s.sampleRate))],
    };
}

/* ==========================================================================
   本体
   ========================================================================== */

async function run(): Promise<Record<string, unknown>> {
    const env = bridge.env;
    const seconds = Number(env.clockProbeSeconds) || SECONDS_DEFAULT;
    const fps = Number(env.fps) || MAIN_FPS_DEFAULT;
    const facts: Record<string, unknown> = {
        crossOriginIsolated: self.crossOriginIsolated,
        visibilityState: document.visibilityState,
        timeOriginUs: Math.round(performance.timeOrigin * 1000),
        generatorAvailable: typeof MediaStreamTrackGenerator === "function",
    };
    const ticksAtStart = sampleTicks();

    let stopped = false;
    const isStopped = (): boolean => stopped;

    const main = makeFrameSource(WIDTH, HEIGHT);
    const back = makeFrameSource(RETURN_WIDTH, RETURN_HEIGHT);
    const tone = makeToneSource();
    if (tone.ctx.state === "suspended") await tone.ctx.resume();
    facts.sourceKind = main.kind;
    facts.audioSinkType = tone.sinkType;
    facts.audioContextState = tone.ctx.state;

    const vp8 = vp8Codecs();
    facts.vp8Available = vp8.length > 0;

    // 両方向の組。A の映像の m 行を、B が答える前に sendrecv にして送り返しの映像を載せる。
    const pcA = new RTCPeerConnection({ iceServers: [] });
    const pcB = new RTCPeerConnection({ iceServers: [] });
    const tvA = pcA.addTransceiver(main.track, { direction: "sendrecv" });
    if (vp8.length > 0) tvA.setCodecPreferences(vp8);
    pcA.addTransceiver(tone.track, { direction: "sendonly" });
    const iceAB = await negotiate(pcA, pcB, async () => {
        const tv = pcB.getTransceivers().find((t) => t.receiver.track.kind === "video");
        if (tv === undefined) throw new Error("B の映像のトランシーバーがない");
        tv.direction = "sendrecv";
        await tv.sender.replaceTrack(back.track);
    });

    // 受けるだけの組。D に渡す offer から rcvr-rtt と rrtr を除く。除かないと、D は RTCP XR で往復の時間を測り、
    // 受けた映像に取り込みの時刻が付く (Discord の画面共有を受けるときは付かない)。
    const pcC = new RTCPeerConnection({ iceServers: [] });
    const pcD = new RTCPeerConnection({ iceServers: [] });
    const tvC = pcC.addTransceiver(main.track, { direction: "sendonly" });
    if (vp8.length > 0) tvC.setCodecPreferences(vp8);
    pcC.addTransceiver(tone.track, { direction: "sendonly" });
    const iceCD = await negotiate(pcC, pcD, async () => {}, withoutNonSenderRtt);
    facts.negotiation = { ab: iceAB, cd: iceCD };

    const videoB = newVideoSink("bidirectional");
    const videoD = newVideoSink("receive-only");
    const audioB = newAudioSink("bidirectional");
    const audioD = newAudioSink("receive-only");
    const trackBv = receiverTrack(pcB, "video");
    const trackDv = receiverTrack(pcD, "video");
    const trackBa = receiverTrack(pcB, "audio");
    const trackDa = receiverTrack(pcD, "audio");
    const readers = [
        readVideo(trackBv, videoB, isStopped),
        readVideo(trackDv, videoD, isStopped),
        readAudio(trackBa, audioB, isStopped),
        readAudio(trackDa, audioD, isStopped),
    ];
    const elements: HTMLMediaElement[] = [watchVideo(trackBv, videoB, isStopped), watchVideo(trackDv, videoD, isStopped)];
    void attachAudioElementIfSilent(trackBa, audioB).then((el) => el && elements.push(el));
    void attachAudioElementIfSilent(trackDa, audioD).then((el) => el && elements.push(el));

    const startMs = performance.now();
    const endMs = startMs + seconds * 1000;
    const sentMs: number[] = [];
    const brackets: HrtimeBracket[] = [];
    const stats: Array<{ atMs: number; pc: string; rows: Array<Record<string, unknown>> }> = [];
    const pcs: Array<[string, RTCPeerConnection]> = [
        ["A", pcA],
        ["B", pcB],
        ["C", pcC],
        ["D", pcD],
    ];

    const collectStats = async (): Promise<void> => {
        for (const [name, pc] of pcs) {
            try {
                stats.push({ atMs: performance.now(), pc: name, rows: pickStats(await pc.getStats()) });
            } catch (e) {
                stats.push({ atMs: performance.now(), pc: name, rows: [{ error: errorText(e) }] });
            }
        }
    };

    const loops = [
        tickLoop(fps, endMs, (i) => {
            main.draw(i);
            sentMs.push(performance.now());
        }),
        tickLoop(RETURN_FPS, endMs, (i) => back.draw(i)),
        (async () => {
            while (performance.now() < endMs) {
                brackets.push(bracket());
                await sleep(BRACKET_INTERVAL_MS);
            }
        })(),
        (async () => {
            while (performance.now() < endMs) {
                await sleep(STATS_INTERVAL_MS);
                await collectStats();
                bridge.progress({
                    mode: "clock-probe",
                    elapsedMs: performance.now() - startMs,
                    counts: {
                        sent: sentMs.length,
                        videoB: [videoB.processor.length, videoB.callbacks.length],
                        videoD: [videoD.processor.length, videoD.callbacks.length],
                        audioB: audioB.samples.length,
                        audioD: audioD.samples.length,
                    },
                });
            }
        })(),
    ];
    await Promise.all(loops);

    stopped = true;
    const ticksAtEnd = sampleTicks();
    for (const sink of [videoB, videoD]) sink.cancel();
    for (const sink of [audioB, audioD]) sink.cancel();
    await Promise.race([Promise.all(readers), sleep(1000)]);
    for (const el of elements) {
        el.pause();
        el.srcObject = null;
        el.remove();
    }
    facts.connectionStates = Object.fromEntries(
        pcs.map(([name, pc]) => [name, { connection: pc.connectionState, ice: pc.iceConnectionState }]),
    );
    for (const [, pc] of pcs) pc.close();
    main.track.stop();
    back.track.stop();
    tone.track.stop();
    await tone.ctx.close();

    // ページの原点の TimeTicks の値は、取り込みの時刻が付いた両方向の組のフレームから決める。
    const candidatesB = originCandidates(pairByRtp(videoB.processor, videoB.callbacks));
    const candidatesD = originCandidates(pairByRtp(videoD.processor, videoD.callbacks));
    const origin = estimateOrigin(candidatesB);
    const originUs = origin === null ? null : origin.originUs;
    const fromBrackets = originFromBrackets(brackets);
    const timeOriginUs = facts.timeOriginUs as number;
    const sendIntervals = deltas(sentMs).map((d) => Math.round(d * 1000));

    return {
        mode: "clock-probe",
        config: { seconds, fps, width: WIDTH, height: HEIGHT, returnFps: RETURN_FPS, returnWidth: RETURN_WIDTH, returnHeight: RETURN_HEIGHT },
        facts,
        summary: {
            origin,
            originCandidatesReceiveOnly: candidatesD.length,
            hrtime:
                fromBrackets === null
                    ? null
                    : {
                          ...fromBrackets,
                          originMinusTightestMidUs: originUs === null ? null : originUs - fromBrackets.tightestMidUs,
                          originMinusLoUs: originUs === null ? null : originUs - fromBrackets.loUs,
                          hiMinusOriginUs: originUs === null ? null : fromBrackets.hiUs - originUs,
                      },
            // performance.timeOrigin (システム時刻から作る) と、ページの原点の TimeTicks の値の差。
            timeOriginMinusOriginUs: originUs === null ? null : timeOriginUs - originUs,
            ticksAtStart,
            ticksAtEnd,
            sendIntervalUs: summarize(sendIntervals),
            sendIntervalUsHistogram1ms: histogram(sendIntervals, 1000),
            video: [summarizeVideo(videoB, originUs, startMs + STEADY_AFTER_MS), summarizeVideo(videoD, originUs, startMs + STEADY_AFTER_MS)],
            audio: [summarizeAudio(audioB, originUs), summarizeAudio(audioD, originUs)],
        },
        raw: {
            sentMs,
            originCandidatesUs: candidatesB,
            brackets: columns(brackets, ["beforeMs", "hrtimeNs", "afterMs"]),
            video: [videoB, videoD].map((s) => ({
                label: s.label,
                processor: columns(s.processor, ["readMs", "timestampUs", "rtp"]),
                callbacks: columns(s.callbacks, [
                    "nowMs",
                    "rtp",
                    "captureTimeMs",
                    "receiveTimeMs",
                    "expectedDisplayTimeMs",
                    "mediaTime",
                    "presentedFrames",
                ]),
            })),
            audio: [audioB, audioD].map((s) => ({
                label: s.label,
                samples: columns(s.samples, ["readMs", "timestampUs", "frames"]),
            })),
            stats,
        },
    };
}

async function clockProbeMain(): Promise<void> {
    let result: Record<string, unknown>;
    try {
        result = await run();
    } catch (e) {
        result = { mode: "clock-probe", aborted: true, fatalError: errorText(e) };
    }
    try {
        await bridge.report(result);
    } catch (e) {
        console.error("report failed", e);
    }
}

if (bridge && bridge.env && bridge.env.mode === "clock-probe") {
    void clockProbeMain();
}
