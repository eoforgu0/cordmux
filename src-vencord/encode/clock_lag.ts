// SPDX-License-Identifier: Apache-2.0

// 画面共有の映像と音声の「受け取った時刻 - 換算した時刻」(encode/clock_map.ts で送信側の時計に換算した時刻と、その
// フレームか断片を受け取った時刻の差) を、定期の行と行の間の期間ごとに集め、5、50、95 パーセンタイルをログの行にする。
// 測るだけで、動作は変えない。換算の定数が正しければ、この値は取り出しの遅れ (数 ms) で、50 パーセンタイルは小さい。
// 定数が低いまま残ると、換算した時刻がその分だけ前に出るので、この値がその分だけ大きくなる。受信を始めた直後の
// ずれの形と、定数が戻った後の値を、映像と音声で別々に読むために使う。
// 値は呼び出し元が渡す (このファイルは時計を持たない)。映像は encode/session.ts が、音声は index.ts の pumpAudio() が
// 足し、60 秒ごとの playout の行のときに index.ts が取り出す。Vencord も DOM の型も import しない。

/**
 * 1 つの期間に持つ値の数の上限 (種類ごと)。60 fps の映像の 60 秒 (3600 個) が収まる大きさにした。音声は 10 ms の断片
 * ごとに足すので 60 秒で 6000 個になり、上限に届く。届いたら、持っている値を 1 つおきに捨て、その後は足す値も同じ
 * 間隔で間引く。期間の全体から同じ間隔で取った値のパーセンタイルになる。
 */
export const CLOCK_LAG_MAX_SAMPLES = 4096;

/** 1 つの種類 (映像か音声) の、期間の値。呼び出し元が持ち、addClockLag で足す。 */
export interface ClockLagSamples {
    /** 持っている値 (us)。足した順。 */
    values: number[];
    /** values に入れる間隔 (足した値のいくつに 1 つを入れるか)。上限に届くたびに 2 倍にする。 */
    stride: number;
    /** 前に values に入れてから足した値の数。 */
    sinceKept: number;
    /** 期間に足した値の数 (間引いた値も数える)。 */
    count: number;
}

/** 期間の値をまとめたもの。count が 0 のとき、パーセンタイルは undefined。 */
export interface ClockLagSummary {
    readonly count: number;
    readonly p5Us: number | undefined;
    readonly p50Us: number | undefined;
    readonly p95Us: number | undefined;
}

export function createClockLagSamples(): ClockLagSamples {
    return { values: [], stride: 1, sinceKept: 0, count: 0 };
}

/** 「受け取った時刻 - 換算した時刻」(us) を 1 つ足す。負の値もそのまま足す。有限でない値は足さない。 */
export function addClockLag(samples: ClockLagSamples, lagUs: number): void {
    if (!Number.isFinite(lagUs)) return;
    samples.count++;
    samples.sinceKept++;
    if (samples.sinceKept < samples.stride) return;
    samples.sinceKept = 0;
    samples.values.push(lagUs);
    if (samples.values.length >= CLOCK_LAG_MAX_SAMPLES) {
        samples.values = samples.values.filter((_, i) => i % 2 === 0);
        samples.stride *= 2;
    }
}

/** 昇順に並べた値の p パーセンタイル (最も近い順位の値。p は 0 より大きく 100 以下)。 */
function percentile(sorted: readonly number[], p: number): number | undefined {
    if (sorted.length === 0) return undefined;
    const rank = Math.ceil((p / 100) * sorted.length);
    return sorted[Math.min(Math.max(rank, 1), sorted.length) - 1];
}

/** 期間の値をまとめ、空に戻す。返した値は、この後に足した値で変わらない。 */
export function takeClockLagSummary(samples: ClockLagSamples): ClockLagSummary {
    const sorted = [...samples.values].sort((a, b) => a - b);
    const summary: ClockLagSummary = {
        count: samples.count,
        p5Us: percentile(sorted, 5),
        p50Us: percentile(sorted, 50),
        p95Us: percentile(sorted, 95),
    };
    samples.values = [];
    samples.stride = 1;
    samples.sinceKept = 0;
    samples.count = 0;
    return summary;
}

function formatSignedMs(valueUs: number | undefined): string {
    if (valueUs === undefined) return "none";
    const rounded = Math.round(valueUs / 100) / 10;
    const normalized = rounded === 0 ? 0 : rounded; // -0 を +0.0 にそろえる。
    return `${normalized >= 0 ? "+" : ""}${normalized.toFixed(1)}ms`;
}

function kindText(name: string, summary: ClockLagSummary | undefined): string {
    const s = summary ?? { count: 0, p5Us: undefined, p50Us: undefined, p95Us: undefined };
    return (
        `${name}.p5=${formatSignedMs(s.p5Us)} ${name}.p50=${formatSignedMs(s.p50Us)} ${name}.p95=${formatSignedMs(s.p95Us)}` +
        ` ${name}.n=${s.count}`
    );
}

/**
 * ストリーム 1 つの期間の行を作る。映像も音声も 1 つも足していなければ undefined (行を書かない)。video と audio は、
 * そのストリームにその種類のトラックがなければ undefined。書式の例:
 *
 *   clock lag stream 3: v.p5=+0.8ms v.p50=+2.1ms v.p95=+7.4ms v.n=3600 a.p5=+0.3ms a.p50=+1.2ms a.p95=+3.9ms a.n=6000
 *
 * (実際は 1 行。) 値は「受け取った時刻 - 換算した時刻」の 5、50、95 パーセンタイル (ms) で、正は、換算した時刻より後に
 * 受け取ったこと。n は期間に足した数。その種類の値がなければ none と n=0。
 */
export function clockLagLine(
    streamId: number,
    video: ClockLagSummary | undefined,
    audio: ClockLagSummary | undefined,
): string | undefined {
    if ((video?.count ?? 0) === 0 && (audio?.count ?? 0) === 0) return undefined;
    return `clock lag stream ${streamId}: ${kindText("v", video)} ${kindText("a", audio)}`;
}
