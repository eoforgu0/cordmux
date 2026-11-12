#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0

// OBS の中で合成のソースの画像を撮り、CPU の参照の画像とピクセルで比べるための実行スクリプト。セットごとに、テスト用の OBS の
// settings.json と seen_people.json を書き、test/tools/obs-run/run-window.mjs の --step hold を子として起動して、
// obs-websocket で画像ソースと合成のソースを作り、声の音とミュートを切り替えながら画像を撮る。比べるのは compare.py。
// 使い方と段階の流れは README.md にある。--dry-run は、書く物と呼び出しを出すだけで、OBS もテスト用の送信側も起動しない。

import { spawn, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

import { ObsWs } from "../obs-run/obsws.mjs";
import { HUMAN_GO_FILE_NAME, parseArgs as parseRunWindowArgs, planSteps } from "../obs-run/run-window-plan.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = path.resolve(HERE, "..", "..", "..");
const RUN_WINDOW = path.join(REPO_ROOT, "test", "tools", "obs-run", "run-window.mjs");
const COMPARE_PY = path.join(HERE, "compare.py");

// ---- 人とセット ----

// 自分の枠の人と、テスト用の送信側の画面共有の人 (どちらも架空の ID と、decor_preview の見本の架空の名前)。
// テスト用の送信側を画面 1 本と声 N 本で起動すると、ストリームの番号は画面 1、声 2 から N + 1 になり、ユーザー ID は
// harness-screen-<番号> と harness-voice-<番号> になる (test/tools/harness/sender.ts)。自分の枠の人は通話に参加していないが、
// 使う声 (声 2) の人が通話にいれば、自分の枠の人の声の枠に声 2 が入る (pipeline/stream_router.h の voice_links)。
export const SELF_USER_ID = "check-self";
export const SCREEN_USER_ID = "harness-screen-1";
export const SELF_NAME = "たろう";
export const SCREEN_NAME = "Noa Kimura";
export const SELF_VOICE_STREAM = 2;
export const SCREEN_VOICE_STREAM = 3;
// 画面共有していない参加者の並びに出す人 (c8)。声 4 と声 5 の声だけの人で、使う声に結ばないので並びに出る
// (dock/build_observation.h の composite_roster_people は、ほかの人の使う声に選ばれた声 2 と声 3 の人を、名前ラベルを出す人に
// 入れない)。名前は架空。
export const ROSTER_PEOPLE = [
    { stream: 4, name: "さくら" },
    { stream: 5, name: "Ken" },
];

// 自分の枠に映す背景の画像ソースの名前。OBS が起動した後に作るので、UUID は前もって決められない。
export const BACKGROUND_SOURCE = "gc-background";

// どのセットにも入れる設定。誰を枠に入れるかは自動にし、自分の枠の人を架空の ID にする。自分の枠に映すソースは、UUID を空にして
// 名前だけを書く。製品は周期ごとに探し直し、画像ソースができたところで名前で見つけて、設定の UUID を書き直す。
export const COMMON_SETTINGS = {
    assign_mode: "auto",
    broadcaster_user_id: SELF_USER_ID,
    self_frame_source_uuid: "",
    self_frame_source_name: BACKGROUND_SOURCE,
    self_frame_on: true,
    // 参照 (decor_preview の --composite-check) は枠の背景を描かないので、枠の背景を切る (製品の既定は表示する)。
    slot_background_show: false,
};
// 途中の明るさを撮るための時間 (立ち上がりと消える。保つは既定のまま)。
const SLOW = { label_border_rise_ms: 4000, label_border_fade_ms: 4000 };

// セットの表。どのセットも 1 つずつ実行できる。GPU がするのは、画像を置く・明るさと暗さを掛けて置く・切り抜くの 3 つで、
// 背景・縁取り・不透明度・光の形は CPU で描いた画像の中にあるので、場所が重ならないセットはまとめて実行できる。
//   background: 自分の枠に置く背景の種類 (decor_preview の --composite-check-background)
//   quick: 時間が既定 (フェードインとフェードアウトが 150 ms) のセット。立ち上がりと消えるを撮る長さを短くする
//   roster: 並びに出す人 (ROSTER_PEOPLE の形)。ない組は並びが空になる
//   riseStream: 立ち上がりと消えるで音を乗せる声の番号 (既定は自分の枠の人の使う声)
//   bothStreams: 2 人以上を話させる段階で音を乗せる声の番号 (既定は自分の枠の人と画面共有の人)
export const SETS = {
    c1: { title: "既定", settings: {}, background: "stripes", quick: true },
    c2: { title: "背景あり", settings: { label_show_plate: true, ...SLOW }, background: "bright-sky" },
    c3: {
        title: "枠の光 (ぼかし 0) と背景なし",
        settings: {
            label_glow_frame: true,
            label_glow_blur_ratio: 0,
            label_glow_spread_ratio: 0.15,
            label_show_plate: false,
            ...SLOW,
        },
        background: "stripes",
        expectFrameGlow: true,
    },
    c4: { title: "枠も光らせる", settings: { label_glow_frame: true, ...SLOW }, background: "stripes", expectFrameGlow: true },
    c5: { title: "全体の不透明度 50%", settings: { label_opacity_pct: 50, ...SLOW }, background: "bright-sky" },
    c6: {
        title: "重い光",
        // 名前ラベルを右下に置くと、自分の枠 (左上) の名前ラベルの光が右上の枠へ広がり、右上の名前ラベルの下に入る。
        settings: { label_glow_blur_ratio: 3.0, label_glow_spread_ratio: 1.0, label_em_ratio_pct: 8.5, label_corner: "br", ...SLOW },
        background: "stripes",
    },
    c7: { title: "余白 0", settings: { label_margin_pct: 0, ...SLOW }, background: "stripes" },
    c8: {
        title: "画面共有していない参加者の並び",
        // 並びの置き方は既定 (右下、縦並び、アバターは右)。画面共有の人の名前ラベルもアバターを右にして、同じ形を確認する。
        settings: { roster_show: true, label_avatar_side: "right", ...SLOW },
        background: "stripes",
        roster: ROSTER_PEOPLE,
        riseStream: ROSTER_PEOPLE[0].stream,
        bothStreams: [SCREEN_VOICE_STREAM, ...ROSTER_PEOPLE.map((p) => p.stream)],
    },
    c2c5: {
        title: "背景ありと全体の不透明度 50%",
        settings: { label_show_plate: true, label_opacity_pct: 50, ...SLOW },
        background: "bright-sky",
    },
    c4c7: {
        title: "枠も光らせると余白 0",
        settings: { label_glow_frame: true, label_margin_pct: 0, ...SLOW },
        background: "stripes",
        expectFrameGlow: true,
    },
};
export const SEED = "seed";
export const DEFAULT_ORDER = [SEED, "c1", "c2c5", "c3", "c4c7", "c6", "c8"];
// 関門のセット。ここで崩れたら残りのセットを始めない。
export const GATE_SET = "c1";

const CELL_SIZE = "960x540";
const ASSET_SCENE = "gc-assets";
const COMPOSITE_SOURCE = "gc-composite";
const MAIN_SCENE = "Main";
const STALE_STATE_FILES = ["record_pending_dirs.json"];
const PLUGIN_CONFIG_REL = path.join("config", "obs-studio", "plugin_config", "cordmux");
const VOICE_TONE_ENV = "CORDMUX_HARNESS_VOICE_TONE_FILE";
const AVATAR_DIR_ENV = "CORDMUX_HARNESS_AVATAR_DIR";
// テスト用の送信側が送ったアバターの PNG を書き出すフォルダー (セットのフォルダーからの相対)。
export const AVATAR_DIR = "avatars";

/** 段階ごとの長さ (ミリ秒)。時間が既定のセットは、立ち上がりと消えるの撮影と、2 人を話させる待ちを短くする。 */
export function phasePlan(setName) {
    const quick = Boolean(SETS[setName] && SETS[setName].quick);
    return {
        riseShotMs: quick ? 1000 : 5000,
        fadeShotMs: quick ? 1000 : 5000,
        bothWaitMs: quick ? 1000 : 4500,
        pauseSettleMs: 1500,
        idleStatsMs: 10000,
        layoutWaitMs: 20000,
        labelWaitMs: 15000,
        labelStableMs: 2000,
        fixedLayoutWaitMs: 8000,
        fixedLabelWaitMs: 12000,
    };
}

/** セットの settings.json。初期設定を書き込んだ settings.json (base) に、共通の値とセットの値を上から書く。 */
export function settingsFor(setName, base = {}) {
    const set = SETS[setName];
    if (!set) throw new Error("知らないセットです: " + setName);
    return { ...base, ...COMMON_SETTINGS, ...set.settings };
}

export function voiceUserId(stream) {
    return "harness-voice-" + stream;
}

/** セットの並びに出す人。 */
export function rosterOf(setName) {
    const set = SETS[setName];
    if (!set) throw new Error("知らないセットです: " + setName);
    return set.roster || [];
}

/**
 * セットの seen_people.json。名前と使う声を、製品が読む形で書く (src-obs/config/seen_people.h)。名前ラベルの隅は参加者ごとに
 * 持たない (設定の label_corner が全員一律)。並びの人は「cordmuxでの名前」だけを書き、使う声に結ばない。
 */
export function seenPeopleFor(setName) {
    const row = (userId, name, voiceStream) => ({
        user_id: userId,
        display_name: "",
        last_seen_at_us: 0,
        always_on: false,
        name_override: name,
        ...(voiceStream ? { voice_source_user_id: voiceUserId(voiceStream) } : {}),
    });
    return [
        row(SELF_USER_ID, SELF_NAME, SELF_VOICE_STREAM),
        row(SCREEN_USER_ID, SCREEN_NAME, SCREEN_VOICE_STREAM),
        ...rosterOf(setName).map((p) => row(voiceUserId(p.stream), p.name, null)),
    ];
}

/**
 * compare.py に渡す人の一覧 (役割、ID、名前、声のストリーム、声を聞けるか、アバターの PNG のセットのフォルダーからの相対)。
 *   - 声を聞けるか (hearsVoice): 自分の枠の人は、使う声 (声 2) が自分の枠の人の声の枠に経路を持つので聞ける
 *     (dock/build_observation.h の self_frame_hears_voice)。画面共有の人と並びの人は、声の枠 (8) が埋まらないので聞ける
 *     (person_hears_voice)。聞ける人は、話していない間は名前ラベルが暗くなる。
 *   - アバター: テスト用の送信側は、送信側の人 (画面共有の人と声だけの人) の全員のアバターを送り、AVATAR_DIR に
 *     書き出す。自分の枠の人は送信側が知らない人なので、アバターがなく頭文字の円になる。
 */
export function peopleFor(setName) {
    const avatar = (userId) => AVATAR_DIR + "/" + userId + ".png";
    return [
        { role: "self", userId: SELF_USER_ID, name: SELF_NAME, voiceStream: SELF_VOICE_STREAM, hearsVoice: true, avatar: null },
        { role: "screen", userId: SCREEN_USER_ID, name: SCREEN_NAME, voiceStream: SCREEN_VOICE_STREAM, hearsVoice: true, avatar: avatar(SCREEN_USER_ID) },
        ...rosterOf(setName).map((p) => ({
            role: "roster",
            userId: voiceUserId(p.stream),
            name: p.name,
            voiceStream: p.stream,
            hearsVoice: true,
            avatar: avatar(voiceUserId(p.stream)),
        })),
    ];
}

/** 段階ごとに音を乗せる声の番号 (声に音を乗せるファイルの中身) と、それで話す人の user_id。 */
export function speakersFor(setName) {
    const set = SETS[setName];
    if (!set) throw new Error("知らないセットです: " + setName);
    const rise = [set.riseStream || SELF_VOICE_STREAM];
    const both = set.bothStreams || [SELF_VOICE_STREAM, SCREEN_VOICE_STREAM];
    const people = peopleFor(setName);
    const who = (streams) => people.filter((p) => streams.includes(p.voiceStream)).map((p) => p.userId);
    return { riseTone: rise.join(" "), bothTone: both.join(" "), rise: who(rise), both: who(both) };
}

/**
 * 合成のソースが並びの名前ラベルを描く人の数 (ログの 'Composite roster label ready' の index の数)。名前ラベルを出す人は、
 * 通話の参加者のうち使う声に選ばれていない人の全員 (画面共有の人と並びの人) で、合成のソースは、いま枠に名前を出している
 * 人の並びの名前ラベルも先に描いておく (全画面の始まりなどで並びに移ったとき、一瞬消えないように。
 * sources/composite_decor.cpp の update_roster)。自分の枠の人は通話に参加していないので入らない。並びに置くのは、
 * このうち枠に名前ラベル付きで描いていない人 (rosterOf) だけである。
 */
export function rosterLabelCountOf(setName) {
    return rosterOf(setName).length + 1;
}

/** テスト用の送信側の声の本数 (自分の枠の人と画面共有の人の使う声の 2 本と、並びの人の分)。 */
export function voiceCountOf(setName) {
    return 2 + rosterOf(setName).length;
}

/**
 * run-window.mjs の引数 (node と run-window.mjs の後ろ)。初期設定の書き込みの実行は、送信側を起動せずに OBS の設定を作る。
 * どの実行にも --keep-recordings を付ける。run-window.mjs の後片付け (cleanup.ps1) は、付けないと作業フォルダーの下に
 * あるほかの実行の録画を全部消すためである (このツールは録画しないので、残しても困らない)。
 */
export function runWindowArgs(setName, opts) {
    const base = [
        "--obs",
        opts.obs,
        "--out",
        path.join(opts.out, setName),
        "--plugin",
        opts.plugin,
        "--step",
        "hold",
        "--keep-recordings",
    ];
    if (setName === SEED) return [...base, "--hold-minutes", "5", "--tag", "gc-" + SEED];
    return [
        ...base,
        "--no-seed",
        "--n-screen",
        "1",
        "--n-voice",
        String(voiceCountOf(setName)),
        "--screen-sizes",
        CELL_SIZE,
        "--hw",
        "prefer-software",
        "--hold-minutes",
        "10",
        "--tag",
        "gc-" + setName,
    ];
}

/** セットごとに子へ足す環境変数。声に音を乗せるファイルの場所と、送ったアバターの PNG を書き出すフォルダーを、テスト用の
 *  送信側に渡す (run-window.mjs は知らない環境変数をそのまま子に渡す)。 */
export function runWindowEnv(setName, opts) {
    if (setName === SEED) return {};
    return { [VOICE_TONE_ENV]: toneFileOf(opts, setName), [AVATAR_DIR_ENV]: avatarDirOf(opts, setName) };
}

export function avatarDirOf(opts, setName) {
    return path.join(opts.out, setName, AVATAR_DIR);
}

export function toneFileOf(opts, setName) {
    return path.join(opts.out, setName, "voice-tone.flag");
}

export function holdInfoPathOf(opts, setName) {
    return path.join(opts.out, setName, "window3b", "hold-gc-" + setName + ".json");
}

/** run-window.mjs の待ちを終わらせるファイル (hold の JSON の goFile と同じ場所。run-window-plan.mjs の HUMAN_GO_FILE_NAME)。 */
export function goFileOf(opts, setName) {
    return path.join(opts.out, setName, "window3b", HUMAN_GO_FILE_NAME);
}

/** hold-<tag>.json を読み、使う値がそろっているかを確認する。足りなければ例外を投げる。 */
export function parseHoldInfo(text) {
    const info = JSON.parse(text);
    for (const key of ["wsPort", "goFile", "obsConfigDir", "obsPid"]) {
        if (info[key] === undefined || info[key] === null || info[key] === "") throw new Error("hold の JSON に " + key + " がありません");
    }
    return info;
}

// ---- OBS のログ ----

const LOG_LINE_RE = /^(\d\d):(\d\d):(\d\d)\.(\d\d\d): (.*)$/;

/** ログの 1 行の時刻を、base と同じ日の UNIX 時刻 (ミリ秒) にする。時刻のない行は null。 */
export function logLineEpochMs(line, baseMs) {
    const m = LOG_LINE_RE.exec(line);
    if (!m) return null;
    const d = new Date(baseMs);
    d.setHours(Number(m[1]), Number(m[2]), Number(m[3]), Number(m[4]));
    let t = d.getTime();
    // 日付をまたいだ行は、base の前後 12 時間に収まる日にする。
    if (t - baseMs > 12 * 3600 * 1000) t -= 24 * 3600 * 1000;
    if (baseMs - t > 12 * 3600 * 1000) t += 24 * 3600 * 1000;
    return t;
}

/** 'Composite layout ...' の行を読む。自分の枠と画面共有のタイルの矩形を返す。違う行は null。 */
export function parseLayoutLine(line) {
    const m = /Composite layout canvas=(\d+)x(\d+) count=(\d+)((?: \S+=\d+,\d+,\d+,\d+)*)\s*$/.exec(line);
    if (!m) return null;
    const tiles = {};
    for (const part of m[4].trim().split(/\s+/).filter(Boolean)) {
        const [name, rect] = part.split("=");
        tiles[name] = rect.split(",").map(Number);
    }
    return { canvas: [Number(m[1]), Number(m[2])], count: Number(m[3]), tiles };
}

/** 'Composite label ready: ...' の行を読む。違う行は null。 */
export function parseLabelReady(line) {
    const m = /Composite (label|frame glow) ready: tile=(\d+) cell=(\d+)x(\d+) paint_ms=(\d+) upload_ms=(\d+) total=(\d+)/.exec(line);
    if (!m) return null;
    return { kind: m[1] === "label" ? "label" : "frame", tile: Number(m[2]), paintMs: Number(m[5]), uploadMs: Number(m[6]), total: Number(m[7]) };
}

/** 'Composite roster label ready: ...' の行 (並びの名前ラベルを描き終えた) を読む。index は並べる順の番号。違う行は null。 */
export function parseRosterReady(line) {
    const m = /Composite roster label ready: index=(\d+) paint_ms=(\d+) upload_ms=(\d+) total=(\d+)/.exec(line);
    if (!m) return null;
    return { index: Number(m[1]), paintMs: Number(m[2]), uploadMs: Number(m[3]), total: Number(m[4]) };
}

/** 'Avatar: stored=... people_with_avatar=N ...' の行 (受信側がアバターを 1 人分受け取った) を読む。違う行は null。 */
export function parseAvatarLine(line) {
    const m = /\[cordmux\] Avatar: stored=(yes|no) people_with_avatar=(\d+) avatar_decode_failures=(\d+)/.exec(line);
    if (!m) return null;
    return { stored: m[1] === "yes", peopleWithAvatar: Number(m[2]), decodeFailures: Number(m[3]) };
}

/**
 * アバターの行を 1 行読んで、受信側がアバターを受け取った人の数の最大 (seen) と、wanted 人そろった時刻 (completeMs) を進めた
 * 値を返す。アバターの行でなければ state をそのまま返す。
 */
export function noteAvatars(state, line, wanted, nowMs) {
    const a = parseAvatarLine(line);
    if (!a) return state;
    const seen = Math.max(state.seen, a.peopleWithAvatar);
    return { seen, completeMs: state.completeMs !== null ? state.completeMs : seen >= wanted ? nowMs : null };
}

/** 行がファイルに出るまでの遅れ (見つけた時刻 - 行の時刻) が 1 秒を超えたら、ログを読みながら待つのをやめる。 */
export const LOG_LATENCY_LIMIT_MS = 1000;
export function decideLogWait(latenciesMs) {
    const max = latenciesMs.length ? Math.max(...latenciesMs) : null;
    return { maxMs: max, samples: latenciesMs.length, fixedWaits: max === null || max > LOG_LATENCY_LIMIT_MS };
}

/** OBS のログのフォルダーで、since (ミリ秒) より後に書かれた最も新しいファイル。無ければ null。 */
function newestLog(obsConfigDir, sinceMs) {
    const dir = path.join(obsConfigDir, "logs");
    if (!fs.existsSync(dir)) return null;
    let best = null;
    for (const name of fs.readdirSync(dir)) {
        const p = path.join(dir, name);
        const st = fs.statSync(p);
        if (!st.isFile() || st.mtimeMs < sinceMs) continue;
        if (!best || st.mtimeMs > best.mtimeMs) best = { path: p, mtimeMs: st.mtimeMs };
    }
    return best ? best.path : null;
}

/** ログを先頭から読み直し、まだ見ていない行を返す。行ごとに見つけた時刻を付ける。 */
class LogTail {
    constructor(obsConfigDir, sinceMs) {
        this.obsConfigDir = obsConfigDir;
        this.sinceMs = sinceMs;
        this.file = null;
        this.seen = 0;
        this.lines = [];
    }

    poll() {
        if (!this.file) this.file = newestLog(this.obsConfigDir, this.sinceMs);
        if (!this.file) return [];
        const text = fs.readFileSync(this.file, "utf8");
        const all = text.split(/\r?\n/);
        if (all.length && all[all.length - 1] === "") all.pop();
        const fresh = all.slice(this.seen).map((line) => ({ line, seenMs: Date.now() }));
        this.seen = all.length;
        this.lines.push(...fresh);
        return fresh;
    }
}

// ---- 製品の確認 ----

function sha256(file) {
    return createHash("sha256").update(fs.readFileSync(file)).digest("hex");
}

/** テスト用にインストールした cordmux.dll が、ビルドした DLL と同じか (大きさとハッシュ)。 */
export function checkDll(pluginDir, builtDll) {
    const placed = path.join(pluginDir, "cordmux.dll");
    const out = { placed, built: builtDll, ok: false };
    if (!fs.existsSync(placed) || !fs.existsSync(builtDll)) {
        out.reason = "DLL がありません";
        return out;
    }
    out.placedSize = fs.statSync(placed).size;
    out.builtSize = fs.statSync(builtDll).size;
    out.placedSha256 = sha256(placed);
    out.builtSha256 = sha256(builtDll);
    out.ok = out.placedSize === out.builtSize && out.placedSha256 === out.builtSha256;
    if (!out.ok) out.reason = "インストールした DLL がビルドした DLL と違います";
    return out;
}

// ---- 引数 ----

export function parseCliArgs(argv) {
    const args = { obs: null, plugin: null, out: null, builtDll: null, decorPreview: null, sets: DEFAULT_ORDER, dryRun: false, gate: true };
    const errors = [];
    for (let i = 0; i < argv.length; i++) {
        const a = argv[i];
        const value = () => {
            if (i + 1 >= argv.length) errors.push(a + " には値が要ります");
            return argv[++i];
        };
        if (a === "--obs") args.obs = value();
        else if (a === "--plugin") args.plugin = value();
        else if (a === "--out") args.out = value();
        else if (a === "--built-dll") args.builtDll = value();
        else if (a === "--decor-preview") args.decorPreview = value();
        else if (a === "--sets") args.sets = String(value() || "").split(",").filter(Boolean);
        else if (a === "--dry-run") args.dryRun = true;
        else if (a === "--no-gate") args.gate = false;
        else errors.push("知らない引数です: " + a);
    }
    for (const [key, flag] of [["obs", "--obs"], ["plugin", "--plugin"], ["out", "--out"], ["builtDll", "--built-dll"], ["decorPreview", "--decor-preview"]]) {
        if (!args[key]) errors.push(flag + " が要ります");
    }
    for (const s of args.sets) {
        if (s !== SEED && !SETS[s]) errors.push("知らないセットです: " + s + " (使えるセット: " + [SEED, ...Object.keys(SETS)].join(", ") + ")");
    }
    for (const key of ["obs", "plugin", "out", "builtDll", "decorPreview"]) {
        if (args[key]) args[key] = path.resolve(REPO_ROOT, args[key]);
    }
    return { args, errors };
}

// ---- 実行 ----

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function log(...parts) {
    console.log("[" + new Date().toISOString().slice(11, 23) + "]", ...parts);
}

function writeJson(file, value) {
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, JSON.stringify(value, null, 2) + "\n", "utf8");
}

function runDecorPreview(exe, args) {
    const r = spawnSync(exe, args, { encoding: "utf8" });
    if (r.status !== 0) throw new Error("decor_preview が失敗しました (" + r.status + "): " + (r.stderr || "").trim());
    return r.stdout;
}

function backgroundPath(out, kind) {
    return path.join(out, "assets", "background-" + kind + ".png");
}

function ensureBackground(args, kind) {
    const p = backgroundPath(args.out, kind);
    if (!fs.existsSync(p)) {
        fs.mkdirSync(path.dirname(p), { recursive: true });
        runDecorPreview(args.decorPreview, ["--composite-check-background", kind, CELL_SIZE, p]);
    }
    return p;
}

function seedSettingsPath(out) {
    return path.join(out, SEED, "settings.json");
}

function readSeedSettings(out) {
    const p = seedSettingsPath(out);
    return fs.existsSync(p) ? JSON.parse(fs.readFileSync(p, "utf8")) : null;
}

/** 子の run-window.mjs を起動し、終わるのを待てる形で返す。出力は <out>/<セット>/driver.txt。 */
function startRunWindow(args, setName) {
    const dir = path.join(args.out, setName);
    fs.mkdirSync(dir, { recursive: true });
    const outFile = fs.openSync(path.join(dir, "driver.txt"), "a");
    const child = spawn(process.execPath, [RUN_WINDOW, ...runWindowArgs(setName, args)], {
        cwd: REPO_ROOT,
        env: { ...process.env, ...runWindowEnv(setName, args) },
        stdio: ["ignore", outFile, outFile],
    });
    const exited = new Promise((resolve) => child.on("exit", (code) => resolve(code)));
    return { child, exited, outFile };
}

async function waitForHoldInfo(args, setName, run, timeoutMs) {
    const p = holdInfoPathOf(args, setName);
    const deadline = Date.now() + timeoutMs;
    let exitCode = null;
    run.exited.then((c) => (exitCode = c));
    while (Date.now() < deadline) {
        if (fs.existsSync(p)) {
            try {
                return parseHoldInfo(fs.readFileSync(p, "utf8"));
            } catch {
                /* 書いている途中なら次に読む */
            }
        }
        if (exitCode !== null) throw new Error("run-window.mjs が hold の JSON を書く前に終わりました (終了コード " + exitCode + ")");
        await sleep(500);
    }
    throw new Error("hold の JSON が " + timeoutMs / 1000 + " 秒以内に書かれません: " + p);
}

// run-window.mjs の待ちを終わらせ、OBS を閉じて後片付けを終えるまで待つ。hold の JSON を読めなかったときも、同じ場所に
// 終わりのファイルを置く。待ちに入る前に置いたファイルは run-window.mjs が消してから待つので、終わるまで置き直す。
async function finishRunWindow(info, run, goFile, timeoutMs = 5 * 60 * 1000) {
    const target = info && info.goFile ? info.goFile : goFile;
    let code = null;
    run.exited.then((c) => (code = c === null ? "signal" : c));
    const deadline = Date.now() + timeoutMs;
    while (code === null && Date.now() < deadline) {
        if (!fs.existsSync(target)) {
            fs.mkdirSync(path.dirname(target), { recursive: true });
            fs.writeFileSync(target, "go\n");
        }
        await sleep(2000);
    }
    if (code === null) code = "timeout";
    fs.closeSync(run.outFile);
    if (code === "timeout") throw new Error("run-window.mjs が " + timeoutMs / 1000 + " 秒以内に終わりません (OBS とテスト用の送信側を確認してください)");
    return code;
}

async function screenshot(ws, sourceName, file) {
    const reqWallMs = Date.now();
    const r = await ws.call("GetSourceScreenshot", { sourceName, imageFormat: "png" });
    const resWallMs = Date.now();
    const data = String(r.imageData || "");
    const comma = data.indexOf(",");
    fs.writeFileSync(file, Buffer.from(comma >= 0 ? data.slice(comma + 1) : data, "base64"));
    return { reqWallMs, resWallMs };
}

/** 1 つのセットの実行 (README.md の「1 つのセットの流れ」の prepare から close まで)。撮った画像と時刻を <out>/<セット>/shots に書く。 */
async function runSet(args, setName) {
    const plan = phasePlan(setName);
    const set = SETS[setName];
    const dir = path.join(args.out, setName);
    const shotsDir = path.join(dir, "shots");
    fs.mkdirSync(shotsDir, { recursive: true });
    const meta = {
        set: setName,
        title: set.title,
        startedAt: new Date().toISOString(),
        people: peopleFor(setName),
        speakers: speakersFor(setName),
        avatarDir: AVATAR_DIR,
        background: { kind: set.background, file: "background.png" },
        settingsFile: "settings.json",
        seenPeopleFile: "seen_people.json",
        obsLog: "obs.log",
        expectFrameGlow: Boolean(set.expectFrameGlow),
        shots: [],
        tone: [],
        framesPause: [],
        getStats: {},
        logLatency: null,
        errors: [],
    };

    // prepare: 設定を書く (初期設定を書き込んだ settings.json に、セットの値を上から書く)。前の実行の割り当てを持ち越さない。
    const base = readSeedSettings(args.out);
    if (!base) throw new Error("初期設定を書き込んだ settings.json がありません。--sets の先頭に seed を入れてください");
    const settings = settingsFor(setName, base);
    const cfg = path.join(args.obs, PLUGIN_CONFIG_REL);
    writeJson(path.join(cfg, "settings.json"), settings);
    writeJson(path.join(cfg, "seen_people.json"), seenPeopleFor(setName));
    for (const name of STALE_STATE_FILES) fs.rmSync(path.join(cfg, name), { force: true });
    writeJson(path.join(dir, "settings.json"), settings);
    writeJson(path.join(dir, "seen_people.json"), seenPeopleFor(setName));
    fs.copyFileSync(ensureBackground(args, set.background), path.join(dir, "background.png"));
    meta.timing = {
        riseMs: settings.label_border_rise_ms ?? 150,
        holdMs: settings.label_border_hold_ms ?? 200,
        fadeMs: settings.label_border_fade_ms ?? 150,
    };
    const toneFile = toneFileOf(args, setName);
    fs.rmSync(toneFile, { force: true });
    // 前の実行のアバターの PNG を残すと、送信側が書き出せなかったときに古い画像で比べてしまうので、起動の前に消す。
    fs.rmSync(avatarDirOf(args, setName), { recursive: true, force: true });
    const setTone = (text) => {
        if (text === null) fs.rmSync(toneFile, { force: true });
        else fs.writeFileSync(toneFile, text);
        const wallMs = Date.now();
        meta.tone.push({ wallMs, text });
        return wallMs;
    };

    // launch: 起動
    const startedMs = Date.now();
    log(setName + ": run-window.mjs を起動します");
    const run = startRunWindow(args, setName);
    let info = null;
    let ws = null;
    const created = [];
    try {
        info = await waitForHoldInfo(args, setName, run, 240000);
        ws = new ObsWs(info.wsPort);
        await ws.connect(60000);
        const tail = new LogTail(info.obsConfigDir, startedMs);
        // 起動から今までの行は読み飛ばす (ファイルに出るまでの遅れは、ここから後に書かれた行で測る)。ただし、受信側が
        // アバターを受け取った行は数える。run-window.mjs はテスト用の送信側を起動して接続を待ってから hold の JSON を書く
        // (手順は harness-start、wait-connection、hold-info の順) ので、送信側が接続してすぐ送るアバターの行は、ここまでに出ている。
        // 受け取ったアバターの数は、そろう前に描いた頭文字の円の名前ラベルで比べないために、settle で使う。
        const avatarsWanted = 1 + voiceCountOf(setName);
        let avatars = { seen: 0, completeMs: null };
        for (const { line } of tail.poll()) avatars = noteAvatars(avatars, line, avatarsWanted, Date.now());
        const shot = async (phase, label, source = COMPOSITE_SOURCE, extra = {}) => {
            const seq = meta.shots.length + 1;
            const file = String(seq).padStart(3, "0") + "-" + phase + "-" + label + ".png";
            const t = await screenshot(ws, source, path.join(shotsDir, file));
            meta.shots.push({ seq, phase, label, source: source === COMPOSITE_SOURCE ? "composite" : source, file, ...t, ...extra });
        };
        const shootFor = async (phase, prefix, ms, extra) => {
            const until = Date.now() + ms;
            let n = 0;
            while (Date.now() < until) await shot(phase, prefix + "-" + String(++n).padStart(2, "0"), COMPOSITE_SOURCE, extra);
        };

        // create: 前の実行の残りを消してから、画像ソースと合成のソースを作る
        for (const name of [COMPOSITE_SOURCE, BACKGROUND_SOURCE]) await ws.call("RemoveInput", { inputName: name }).catch(() => {});
        await ws.call("RemoveScene", { sceneName: ASSET_SCENE }).catch(() => {});
        await ws.call("CreateScene", { sceneName: ASSET_SCENE });
        created.push(["scene", ASSET_SCENE]);
        await ws.call("CreateInput", {
            sceneName: ASSET_SCENE,
            inputName: BACKGROUND_SOURCE,
            inputKind: "image_source",
            inputSettings: { file: path.join(dir, "background.png") },
            sceneItemEnabled: true,
        });
        created.push(["input", BACKGROUND_SOURCE]);
        const comp = await ws.call("CreateInput", {
            sceneName: MAIN_SCENE,
            inputName: COMPOSITE_SOURCE,
            inputKind: "cordmux_composite",
            sceneItemEnabled: true,
        });
        created.push(["input", COMPOSITE_SOURCE]);
        await ws.call("SetSceneItemTransform", {
            sceneName: MAIN_SCENE,
            sceneItemId: comp.sceneItemId,
            sceneItemTransform: { positionX: 0, positionY: 0, scaleX: 1, scaleY: 1, rotation: 0 },
        });
        await ws.call("SetCurrentProgramScene", { sceneName: MAIN_SCENE }).catch(() => {});

        // 配置の行を待つ。行がファイルに出るまでの遅れも測る (1 秒を超えたら、決めた時間だけ待つ形にする)。名前ラベルを
        // 描き終えた行は配置の行のすぐ後に出ることがあるので、ここから数える。
        const latencies = [];
        const layoutFrom = Date.now();
        let layout = null;
        const ready = new Set();
        const rosterReady = new Set();
        let lastReadyMs = Date.now();
        const consume = () => {
            for (const { line, seenMs } of tail.poll()) {
                const t = logLineEpochMs(line, seenMs);
                if (t !== null && line.includes("[cordmux]")) latencies.push(seenMs - t);
                const l = parseLayoutLine(line);
                if (l) layout = l;
                const r = parseLabelReady(line);
                if (r && r.kind === "label") {
                    ready.add(r.tile);
                    lastReadyMs = Date.now();
                }
                const rr = parseRosterReady(line);
                if (rr) {
                    rosterReady.add(rr.index);
                    lastReadyMs = Date.now();
                }
                avatars = noteAvatars(avatars, line, avatarsWanted, Date.now());
            }
        };
        const fixed = readLogWait(args.out);
        if (fixed && fixed.fixedWaits) {
            await sleep(plan.fixedLayoutWaitMs);
            consume();
        } else {
            while (Date.now() - layoutFrom < plan.layoutWaitMs) {
                consume();
                if (layout && layout.count === 2 && layout.tiles.self) break;
                await sleep(100);
            }
        }
        meta.layout = layout;
        if (!fixed) {
            meta.logLatency = decideLogWait(latencies);
            writeJson(path.join(args.out, "log-wait.json"), meta.logLatency);
        } else {
            meta.logLatency = fixed;
        }
        if (!layout || layout.count !== 2 || !layout.tiles.self) {
            throw new Error("配置の行 'Composite layout ... count=2 self=...' が出ません");
        }

        // settle: 名前ラベルを描き終えるまで待つ。tile=0 と tile=1 と、並びの名前ラベルを描く人の全員 (rosterLabelCountOf。
        // 枠に名前を出している画面共有の人の分も先に描く) の名前ラベルが出て、送信側の人の全員の
        // アバターが届き、その両方から labelStableMs の間、描き直しがないこと (アバターが届く前の頭文字の円の名前ラベルで
        // 比べない)。アバターの行が見えないまま labelWaitMs が過ぎたら、待つのをやめて進める (撮った画像は compare.py が
        // アバターのある参照と比べるので、頭文字の円のままなら不合格になる)。
        await shot("settle", "image-source", BACKGROUND_SOURCE);
        const rosterCount = rosterLabelCountOf(setName);
        const settled = () => {
            if (!ready.has(0) || !ready.has(1) || avatars.completeMs === null) return false;
            for (let i = 0; i < rosterCount; i++) if (!rosterReady.has(i)) return false;
            return Date.now() - Math.max(lastReadyMs, avatars.completeMs) >= plan.labelStableMs;
        };
        if (meta.logLatency.fixedWaits) {
            await sleep(plan.fixedLabelWaitMs);
        } else {
            const from = Date.now();
            while (Date.now() - from < plan.labelWaitMs) {
                consume();
                if (settled()) break;
                await sleep(100);
            }
        }
        consume();
        meta.labelReadyTiles = [...ready];
        meta.rosterReadyIndices = [...rosterReady];
        meta.avatars = { wanted: avatarsWanted, seen: avatars.seen, completeWallMs: avatars.completeMs };
        meta.settledWallMs = Date.now();

        // silent: 明るさ 0
        await shot("silent", "level0");
        // rise: セットの話す人 (c8 は並びの 1 人目、ほかは自分の枠の人) を話させ、立ち上がりを撮り続ける
        const riseAt = setTone(meta.speakers.riseTone);
        await shootFor("rise", "rise", plan.riseShotMs, { toneChangeWallMs: riseAt });
        // speaking: 明るさ 1。撮らない間の前後で統計を取る
        meta.getStats.before = await ws.call("GetStats", {});
        await sleep(plan.idleStatsMs);
        meta.getStats.after = await ws.call("GetStats", {});
        await shot("speaking", "level1");
        // mute: ミュートと音量 0
        await ws.call("SetInputMute", { inputName: COMPOSITE_SOURCE, inputMuted: true });
        await shot("mute", "mute", COMPOSITE_SOURCE, { muted: true });
        await ws.call("SetInputMute", { inputName: COMPOSITE_SOURCE, inputMuted: false });
        await shot("mute", "unmute");
        await ws.call("SetInputVolume", { inputName: COMPOSITE_SOURCE, inputVolumeMul: 0 });
        await shot("mute", "vol0", COMPOSITE_SOURCE, { volumeMul: 0 });
        await ws.call("SetInputVolume", { inputName: COMPOSITE_SOURCE, inputVolumeMul: 1 });
        await shot("mute", "vol1");
        // scene: シーンを通った画像
        await shot("scene", "scene", MAIN_SCENE);
        // fade: 話すのをやめ、消える途中を撮り続ける
        const fadeAt = setTone(null);
        await shootFor("fade", "fade", plan.fadeShotMs, { toneChangeWallMs: fadeAt });
        // both: 2 人以上 (c8 は画面共有の人と並びの 2 人、ほかは自分の枠の人と画面共有の人) を話させてからフレームを止め、
        // 右上の背景を撮る
        setTone(meta.speakers.bothTone);
        await sleep(plan.bothWaitMs);
        if (info.framesPauseFile) {
            fs.writeFileSync(info.framesPauseFile, "");
            meta.framesPause.push({ wallMs: Date.now(), on: true });
        } else {
            meta.errors.push("hold の JSON に framesPauseFile がないので、フレームを止められません");
        }
        await sleep(plan.pauseSettleMs);
        await ws.call("SetInputMute", { inputName: COMPOSITE_SOURCE, inputMuted: true });
        await shot("both", "both-mute-1", COMPOSITE_SOURCE, { muted: true });
        await shot("both", "both-mute-2", COMPOSITE_SOURCE, { muted: true });
        await ws.call("SetInputMute", { inputName: COMPOSITE_SOURCE, inputMuted: false });
        await shot("both", "both-unmute");
        if (info.framesPauseFile) {
            fs.rmSync(info.framesPauseFile, { force: true });
            meta.framesPause.push({ wallMs: Date.now(), on: false });
        }
        setTone(null);
        meta.idleEndWallMs = Date.now();
    } catch (e) {
        meta.errors.push(String(e && e.message ? e.message : e));
    } finally {
        // close: 作った物を消し、run-window.mjs に閉じさせる (送信側を止め、OBS を閉じ、後片付けを実行する)
        fs.rmSync(toneFile, { force: true });
        if (info && info.framesPauseFile) fs.rmSync(info.framesPauseFile, { force: true });
        if (ws) {
            for (const [kind, name] of [...created].reverse()) {
                await (kind === "scene" ? ws.call("RemoveScene", { sceneName: name }) : ws.call("RemoveInput", { inputName: name })).catch(() => {});
            }
            ws.close();
        }
        log(setName + ": OBS を閉じます");
        meta.runWindowExit = await finishRunWindow(info, run, goFileOf(args, setName)).catch((e) => {
            meta.errors.push(String(e.message));
            return null;
        });
        const obsLog = info ? newestLog(info.obsConfigDir, startedMs) : null;
        if (obsLog) fs.copyFileSync(obsLog, path.join(dir, "obs.log"));
        writeJson(path.join(shotsDir, "shots.json"), meta);
    }
    if (meta.errors.length) throw new Error(setName + ": " + meta.errors.join(" / "));
    return meta;
}

function readLogWait(out) {
    const p = path.join(out, "log-wait.json");
    return fs.existsSync(p) ? JSON.parse(fs.readFileSync(p, "utf8")) : null;
}

/** 初期設定の書き込み。OBS の設定とシーンコレクションを作り、obs-websocket が答えたらすぐ閉じる。 */
async function runSeed(args) {
    const startedMs = Date.now();
    log("seed: run-window.mjs を起動します");
    const run = startRunWindow(args, SEED);
    let info = null;
    try {
        info = await waitForHoldInfo(args, SEED, run, 240000);
        const ws = new ObsWs(info.wsPort);
        await ws.connect(60000);
        await ws.call("GetVersion", {});
        ws.close();
    } finally {
        await finishRunWindow(info, run, goFileOf(args, SEED));
    }
    const cfg = path.join(args.obs, PLUGIN_CONFIG_REL, "settings.json");
    if (!fs.existsSync(cfg)) throw new Error("初期設定の書き込みの後に settings.json がありません: " + cfg);
    fs.copyFileSync(cfg, seedSettingsPath(args.out));
    fs.rmSync(path.join(args.out, "log-wait.json"), { force: true });
    log("seed: 終わりました (" + Math.round((Date.now() - startedMs) / 1000) + " 秒)");
}

function runCompare(args, sub, setName) {
    const r = spawnSync("python", ["-X", "utf8", COMPARE_PY, sub, "--run", path.join(args.out, setName), "--decor-preview", args.decorPreview], {
        encoding: "utf8",
        env: { ...process.env, PYTHONDONTWRITEBYTECODE: "1", PYTHONIOENCODING: "utf-8" },
    });
    process.stdout.write(r.stdout || "");
    process.stderr.write(r.stderr || "");
    return r.status;
}

// ---- ドライラン ----

function dryRun(args) {
    const report = { dll: checkDll(args.plugin, args.builtDll), sets: [] };
    const dryDir = path.join(args.out, "dry-run");
    const seedSettings = readSeedSettings(args.out);
    for (const setName of args.sets) {
        const argv = runWindowArgs(setName, args);
        const parsed = parseRunWindowArgs(argv);
        const steps = parsed.errors.length ? null : planSteps(parsed.args);
        const entry = {
            set: setName,
            command: ["node", RUN_WINDOW, ...argv],
            env: runWindowEnv(setName, args),
            runWindowErrors: parsed.errors,
            runWindowSteps: steps && steps.ok ? steps.steps.map((s) => s.kind) : null,
        };
        if (setName !== SEED) {
            const settings = settingsFor(setName, seedSettings || {});
            const people = seenPeopleFor(setName);
            const setDir = path.join(dryDir, setName);
            writeJson(path.join(setDir, "settings.json"), settings);
            writeJson(path.join(setDir, "seen_people.json"), people);
            entry.writes = {
                [path.join(args.obs, PLUGIN_CONFIG_REL, "settings.json")]: settings,
                [path.join(args.obs, PLUGIN_CONFIG_REL, "seen_people.json")]: people,
            };
            entry.removes = STALE_STATE_FILES.map((n) => path.join(args.obs, PLUGIN_CONFIG_REL, n));
            entry.phases = phasePlan(setName);
            entry.speakers = speakersFor(setName);
            // 参照の試し描き。製品が settings.json を読めるかも、ここで確認する (読めなければ decor_preview が止まる)。
            // テスト用の送信側のアバターはまだないので、見本のアバターを代わりに描いて、アバターのある名前ラベルを試す。
            const canvasBg = path.join(setDir, "bg-canvas.png");
            const avatarDir = path.join(setDir, AVATAR_DIR);
            const request = trialRequest(setName, path.join(setDir, "settings.json"), canvasBg, (p) => path.join(setDir, p.avatar));
            writeJson(path.join(setDir, "request.json"), request);
            try {
                fs.mkdirSync(avatarDir, { recursive: true });
                peopleFor(setName)
                    .filter((p) => p.avatar)
                    .forEach((p, i) => runDecorPreview(args.decorPreview, ["--composite-check-avatar", String((20 + 140 * i) % 360), path.join(setDir, p.avatar)]));
                makeCanvasBackground(args, ensureBackground(args, SETS[setName].background), canvasBg);
                runDecorPreview(args.decorPreview, ["--backend", "windows", "--composite-check", path.join(setDir, "request.json"), "--out", path.join(setDir, "ref")]);
                const check = JSON.parse(fs.readFileSync(path.join(setDir, "ref", "check.json"), "utf8"));
                entry.reference = {
                    layout: check.layout_log_line,
                    objects: check.objects.map((o) => o.kind + "@" + o.tile),
                    roster: check.roster.map((r) => r.user_id + " " + r.box.join(",")),
                    rosterNotPlaced: check.roster_not_placed,
                    dir: path.join(setDir, "ref"),
                    avatars: "見本のアバター (送信側の PNG ではない)",
                };
            } catch (e) {
                entry.referenceError = e.message;
            }
        }
        report.sets.push(entry);
    }
    writeJson(path.join(dryDir, "dry-run.json"), report);
    return report;
}

/**
 * ドライランの参照の試し描きの要求 (decor_preview の --composite-check)。compare.py の build_request と同じ欄 (参加者ごとの
 * 声を聞けるか、アバター、並びの人) で、明るさ 0、1、0.5 の 3 枚を描く。avatarPath は人からアバターの PNG のパスを返す。
 */
export function trialRequest(setName, settingsFile, canvasBg, avatarPath) {
    const people = peopleFor(setName);
    const tiles = people.filter((p) => p.role !== "roster");
    const roster = people.filter((p) => p.role === "roster");
    const levels = (v) => ({ levels: tiles.map(() => v), roster_levels: roster.map(() => v) });
    return {
        canvas: [1920, 1080],
        settings_file: settingsFile,
        self_shown: true,
        people: tiles.map((p) => ({
            slot: p.role === "self" ? 0 : 1,
            user_id: p.userId,
            name: p.name,
            avatar_png: p.avatar ? avatarPath(p) : "",
            video: true,
            hears_voice: p.hearsVoice,
        })),
        roster: roster.map((p) => ({ user_id: p.userId, name: p.name, avatar_png: p.avatar ? avatarPath(p) : "", hears_voice: p.hearsVoice })),
        background_png: canvasBg,
        renders: [
            { name: "l0", ...levels(0) },
            { name: "l1", ...levels(1) },
            { name: "l05", ...levels(0.5) },
        ],
    };
}

// 自分の枠のセルに背景を置いた 1920x1080 のキャンバスを、compare.py と同じ形で作る (ほかは透明)。
function makeCanvasBackground(args, cellPng, outPng) {
    const code = [
        "import sys",
        "from PIL import Image",
        "c = Image.new('RGBA', (1920, 1080), (0, 0, 0, 0))",
        "c.paste(Image.open(sys.argv[1]).convert('RGBA'), (0, 0))",
        "c.save(sys.argv[2])",
    ].join("\n");
    const r = spawnSync("python", ["-X", "utf8", "-c", code, cellPng, outPng], { encoding: "utf8", env: { ...process.env, PYTHONDONTWRITEBYTECODE: "1" } });
    if (r.status !== 0) throw new Error("背景のキャンバスを作れません: " + (r.stderr || "").trim());
}

async function main() {
    const { args, errors } = parseCliArgs(process.argv.slice(2));
    if (errors.length) {
        for (const e of errors) console.error("run-check: " + e);
        return 2;
    }
    fs.mkdirSync(args.out, { recursive: true });
    if (args.dryRun) {
        const report = dryRun(args);
        console.log(JSON.stringify(report, null, 2));
        const ok = report.dll.ok && report.sets.every((s) => !s.runWindowErrors.length && !s.referenceError && s.runWindowSteps);
        console.log(ok ? "ドライラン: 問題はありません" : "ドライラン: 問題があります (上の dll、runWindowErrors、referenceError を見てください)");
        return ok ? 0 : 1;
    }
    const dll = checkDll(args.plugin, args.builtDll);
    if (!dll.ok) {
        console.error("run-check: " + dll.reason + " " + JSON.stringify(dll));
        return 1;
    }
    for (const setName of args.sets) {
        if (setName === SEED) {
            await runSeed(args);
            continue;
        }
        const started = Date.now();
        try {
            await runSet(args, setName);
        } catch (e) {
            console.error("run-check: " + e.message);
            console.error("run-check: 残りのセットを始めずに止めます");
            return 1;
        }
        log(setName + ": 撮り終えました (" + Math.round((Date.now() - started) / 1000) + " 秒)");
        if (setName === GATE_SET && args.gate) {
            const status = runCompare(args, "gate", setName);
            if (status !== 0) {
                console.error("run-check: " + GATE_SET + " の関門に通りませんでした。残りのセットを始めずに止めます (" + path.join(args.out, setName, "judge", "gate.json") + ")");
                return 3;
            }
        }
    }
    log("全部のセットを撮り終えました。比べるのは compare.py judge (README.md)");
    return 0;
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
    main().then(
        (code) => process.exit(code),
        (e) => {
            console.error("run-check: " + (e && e.stack ? e.stack : e));
            process.exit(1);
        },
    );
}
