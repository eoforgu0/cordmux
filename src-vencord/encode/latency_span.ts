// SPDX-License-Identifier: Apache-2.0

// 送信側の映像の、段階ごとの所要時間を期間 (60 秒の定期の行と行の間) ごとに足し上げ、ログの行にする。測るだけで、
// 動作は変えない。受信側の遅れの行 (src-obs/pipeline の video_latency) と並べて読み、遅れの原因が送信側のどの段か
// を分ける。足し上げは数と合計と最大だけで、フレームごとに呼んでも負荷にならない。時計は呼び出し元が読んで差を
// 渡す (このファイルは時計を持たない)。Vencord も DOM の型も import しない。

/** 1 つの段階の、期間の足し上げ。単位は us。 */
export interface DurationStat {
    count: number;
    sumUs: number;
    maxUs: number;
}

/**
 * 映像のストリーム 1 つの、期間の足し上げ。
 * - read: トラックのフレームのタイムスタンプを送信側の時計に換算した値と、そのフレームを読み出した時刻の差 (読み出しの遅れ)。
 *   encode/clock_map.ts の原点は到着の差の最小値なので、これは「いちばん早く読めたときを 0 とした遅れ」である。
 *   原点そのものの絶対的なずれは、この値に入らない。
 * - encode: エンコーダに渡してから、その出力が出るまで。
 * - prepare: 出力を受け取ってから、sink.sendVideo を呼ぶ直前まで (バイト列のコピーと SPS と PPS の付け足し)。
 * - send: sink.sendVideo を呼んでから戻るまで (ヘッダーとの連結と WebSocket の send の呼び出し。送信の待ち行列で
 *   待つ時間は含まない)。
 */
export interface EncodeLatencySpan {
    read: DurationStat;
    encode: DurationStat;
    prepare: DurationStat;
    send: DurationStat;
}

/** 取り出した 1 つの段階の結果。count が 0 のとき、平均と最大は undefined。 */
export interface DurationSummary {
    readonly count: number;
    readonly avgMs: number | undefined;
    readonly maxMs: number | undefined;
}

export function createDurationStat(): DurationStat {
    return { count: 0, sumUs: 0, maxUs: 0 };
}

export function createEncodeLatencySpan(): EncodeLatencySpan {
    return {
        read: createDurationStat(),
        encode: createDurationStat(),
        prepare: createDurationStat(),
        send: createDurationStat(),
    };
}

/** 所要時間 (us) を 1 つ足す。負の値 (時計の食い違い) は 0 として数える。有限でない値は足さない。 */
export function addDuration(stat: DurationStat, durationUs: number): void {
    if (!Number.isFinite(durationUs)) return;
    const us = durationUs < 0 ? 0 : durationUs;
    stat.count++;
    stat.sumUs += us;
    if (us > stat.maxUs) stat.maxUs = us;
}

/** 平均と最大 (ms) にする。stat は変えない。 */
export function summarizeDuration(stat: DurationStat): DurationSummary {
    if (stat.count === 0) return { count: 0, avgMs: undefined, maxMs: undefined };
    return { count: stat.count, avgMs: stat.sumUs / stat.count / 1000, maxMs: stat.maxUs / 1000 };
}

/** 4 つの段階を空に戻す。 */
export function resetEncodeLatencySpan(span: EncodeLatencySpan): void {
    for (const stat of [span.read, span.encode, span.prepare, span.send]) {
        stat.count = 0;
        stat.sumUs = 0;
        stat.maxUs = 0;
    }
}

/** 4 つの段階のうち、1 つでも足していれば true。 */
export function hasLatencySamples(span: EncodeLatencySpan): boolean {
    return span.read.count + span.encode.count + span.prepare.count + span.send.count > 0;
}

function formatMs(value: number | undefined): string {
    return value === undefined ? "none" : `${value.toFixed(1)}ms`;
}

function stageText(name: string, stat: DurationStat): string {
    const s = summarizeDuration(stat);
    return `${name}.avg=${formatMs(s.avgMs)} ${name}.max=${formatMs(s.maxMs)}`;
}

/**
 * 期間の行を作る。1 つも足していなければ undefined (行を書かない)。この関数は span を変えない (空に戻すのは
 * 呼び出し元が resetEncodeLatencySpan で行う)。書式の例:
 *
 *   latency stream 2: read.avg=1.3ms read.max=9.0ms enc.avg=2.1ms enc.max=14.0ms prep.avg=0.1ms prep.max=0.6ms
 *     send.avg=0.0ms send.max=0.2ms n=3600/3600/3600/3600
 *
 * (実際は 1 行。) 各段階の読み方は src-vencord/encode/README.md の「遅れの行」にある。ある段の avg が none なのは、その
 * 期間に足されなかったこと (enc、prep、send は、時計 monotonicUs が渡されていないときも none)。n は
 * read/enc/prep/send の足した数。
 */
export function latencyLine(streamId: number, span: EncodeLatencySpan): string | undefined {
    if (!hasLatencySamples(span)) return undefined;
    return (
        `latency stream ${streamId}: ${stageText("read", span.read)} ${stageText("enc", span.encode)}` +
        ` ${stageText("prep", span.prepare)} ${stageText("send", span.send)}` +
        ` n=${span.read.count}/${span.encode.count}/${span.prepare.count}/${span.send.count}`
    );
}
