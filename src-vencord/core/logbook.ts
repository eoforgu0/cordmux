// SPDX-License-Identifier: Apache-2.0

// 送信側のログの行を、受信側へ送るまで保つ (LogBook)。送信側はログのファイルを持たず、保った行を受信側へ log の
// 制御用の JSON で送る (docs/wire_format.md の「メッセージの種類」の log の扱い)。行は 3 種類に分ける。
//   - 出来事の行 (event): 状態が変わったときの行 (接続、トラックの追加と終了と mute、ゲートの開閉、段階の変更、
//     映像と音声の原点が初めて決まったときと差が大きく動いたときの clock origin など)。
//     受信側は通話の全体を残す保持 (診断情報の discord.log) に入れる。
//   - 異常に関わる出来事の行 (alert): 映像が届かない、受信のやり直しの頼み、接続の failed など。受信側は出来事の行と
//     同じ保持に入れ、OBS のログにも書く。
//   - 統計の行 (stats): 数秒から 60 秒ごとの行 (clock origin の細かい変化、playout、latency)。受信側は直近の行だけを保つ
//     (discord-stats.log)。出来事の行と同じ保持に入れると、出来事の行が数分で押し出されるので分ける。
// 行の先頭の時刻は呼び出し元が付ける。

/** drain() が返す、送る行。どれも古い順。 */
export interface LogBookDrain {
    /** 出来事の行 (log の lines)。 */
    readonly lines: string[];
    /** 異常に関わる出来事の行 (log の alerts)。 */
    readonly alerts: string[];
    /** 統計の行 (log の stats)。 */
    readonly stats: string[];
}

/**
 * 出来事の行の上限の既定値。受信側と接続していない間 (OBS を起動していない間など) も行はたまるので、上限を置く。
 * 受信側と接続している間は 5 秒ごとに送って空にするので、上限に届くのは接続していない間だけである。1000 行は、
 * 接続していない間の通話の出入り (1 回の画面共有の視聴で、接続とトラックの行が 20 行前後) を数十回分残せる大きさで、
 * 測って決めたものではない。
 */
export const LOGBOOK_EVENT_CAPACITY = 1000;
/** 統計の行の上限の既定値。受信側も直近の 200 行だけを保つ (settings の js_log_ring_lines の既定) ので、同じにする。 */
export const LOGBOOK_STATS_CAPACITY = 200;

interface EventEntry {
    readonly line: string;
    readonly alert: boolean;
}

function requireCapacity(name: string, value: number): void {
    if (!Number.isInteger(value) || value <= 0) {
        throw new RangeError(`LogBook: ${name} は正の整数にしてください: ${value}`);
    }
}

/**
 * 3 種類の行を、上限の行数まで保つ。上限を超えたら、その種類の最も古い行から捨てて数える。出来事の行と異常の行は
 * 1 つの並びで保つ (異常の行が多くても、出来事の行の順は崩れない)。捨てた数は、次の drain() で出来事の行の先頭に
 * 1 行足して伝える (途中から始まるログを、そうと分からないまま受信側に残さないため)。
 */
export class LogBook {
    private readonly eventCapacity: number;
    private readonly statsCapacity: number;
    private events: EventEntry[] = [];
    private stats: string[] = [];
    private droppedEvents = 0;
    private droppedStats = 0;

    constructor(eventCapacity = LOGBOOK_EVENT_CAPACITY, statsCapacity = LOGBOOK_STATS_CAPACITY) {
        requireCapacity("eventCapacity", eventCapacity);
        requireCapacity("statsCapacity", statsCapacity);
        this.eventCapacity = eventCapacity;
        this.statsCapacity = statsCapacity;
    }

    /** 出来事の行を 1 行足す。 */
    event(line: string): void {
        this.pushEvent({ line, alert: false });
    }

    /** 異常に関わる出来事の行を 1 行足す。 */
    alert(line: string): void {
        this.pushEvent({ line, alert: true });
    }

    /** 統計の行を 1 行足す。 */
    stat(line: string): void {
        this.stats.push(line);
        if (this.stats.length > this.statsCapacity) {
            const over = this.stats.length - this.statsCapacity;
            this.stats.splice(0, over);
            this.droppedStats += over;
        }
    }

    /** まだ送っていない行があるか。 */
    isEmpty(): boolean {
        return this.events.length === 0 && this.stats.length === 0 && this.droppedEvents === 0 && this.droppedStats === 0;
    }

    /**
     * 保っている行を、種類ごとに古い順で返し、空にする。異常の行は lines と alerts に分かれるので、1 回の drain() の
     * 中では、受信側は lines の後に alerts を積む。呼び出し元は、異常の行を足したらすぐに drain() して送る
     * (index.ts の logAlert)。そうすると、1 通の中の alerts は、その通の lines のどの行よりも後に書かれた行になり、
     * 受信側の並びが書いた順と同じになる。接続していない間にたまった行だけは、この順が崩れることがある (どの行にも
     * 時刻が付いているので、時刻で読み直せる)。
     */
    drain(): LogBookDrain {
        const lines: string[] = [];
        const alerts: string[] = [];
        if (this.droppedEvents > 0) {
            lines.push(`log: dropped ${this.droppedEvents} older event lines while waiting to send`);
        }
        if (this.droppedStats > 0) {
            lines.push(`log: dropped ${this.droppedStats} older stats lines while waiting to send`);
        }
        for (const entry of this.events) {
            if (entry.alert) alerts.push(entry.line);
            else lines.push(entry.line);
        }
        const stats = this.stats;
        this.events = [];
        this.stats = [];
        this.droppedEvents = 0;
        this.droppedStats = 0;
        return { lines, alerts, stats };
    }

    private pushEvent(entry: EventEntry): void {
        this.events.push(entry);
        if (this.events.length > this.eventCapacity) {
            const over = this.events.length - this.eventCapacity;
            this.events.splice(0, over);
            this.droppedEvents += over;
        }
    }
}
