#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0

// Discord の Web 版 (stable と canary) のバンドルを取得し、OUT_ROOT の下の <channel>/<build>/ に保存する。
// 初期 HTML の <script> にある eager なチャンクを取得し、その本文の webpack のランタイムから、HTML にない遅延読み込みの
// チャンクを探して取得する。保存先に files.json を書き、test/fixtures/discord-bundles/manifest.json に 1 つの build
// につき 1 つの項目を追加する (形は README.md)。404 以外の理由で取得に失敗したファイルがあれば、項目に
// incomplete: true を付け、終了コードを 1 にする。src-vencord/tests/discord_contacts.test.ts は incomplete の build を
// スキップする。

import { createHash } from "node:crypto";
import {
    existsSync,
    mkdirSync,
    readFileSync,
    renameSync,
    statSync,
    writeFileSync,
} from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = path.resolve(SCRIPT_DIR, "..", "..", "..");
const OUT_ROOT = path.join(REPO_ROOT, "_work", "discord-bundles");
const MANIFEST_PATH = path.join(REPO_ROOT, "test", "fixtures", "discord-bundles", "manifest.json");

const CHANNELS = [
    { name: "stable", url: "https://discord.com/app" },
    { name: "canary", url: "https://canary.discord.com/app" },
];

const CONCURRENCY = 4;
const MAX_RETRIES = 3;
const REQUEST_TIMEOUT_MS = 30000;

// ブラウザは起動しない。Node の既定の User-Agent だと拒否されることがあるので、ブラウザと同じ形の値を送る。
const USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) " +
    "Chrome/130.0.0.0 Safari/537.36";

/** 404 は URL の形が違うことを示し、再試行しても変わらないので、ほかの失敗と区別する。 */
class HttpStatusError extends Error {
    constructor(status, url) {
        super(`HTTP ${status}: ${url}`);
        this.status = status;
    }
}

function sleep(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

function sha256Hex(buf) {
    return createHash("sha256").update(buf).digest("hex");
}

/**
 * files.json の中身 (1 つの build のファイルの一覧) から、manifest.json の digest を作る。ファイルの名前で並べ、
 * 「名前:sha256」を改行でつないだ文字列の sha256 を返す。取得の順に左右されないように、名前で並べる。
 */
function computeFilesDigest(files) {
    const sorted = [...files].sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
    const joined = sorted.map((f) => `${f.name}:${f.sha256}`).join("\n");
    return sha256Hex(Buffer.from(joined, "utf8"));
}

async function withRetry(fn, { retries = MAX_RETRIES, isRetryable = () => true } = {}) {
    let lastErr;
    for (let attempt = 1; attempt <= retries; attempt++) {
        try {
            return await fn(attempt);
        } catch (err) {
            lastErr = err;
            if (!isRetryable(err) || attempt === retries) break;
            await sleep(400 * attempt);
        }
    }
    throw lastErr;
}

/** concurrency の数まで、worker を同時に実行する。 */
async function runPool(items, concurrency, worker) {
    const results = new Array(items.length);
    let nextIndex = 0;
    async function runOne() {
        for (;;) {
            const i = nextIndex++;
            if (i >= items.length) return;
            results[i] = await worker(items[i], i);
        }
    }
    const workerCount = Math.max(1, Math.min(concurrency, items.length));
    await Promise.all(Array.from({ length: workerCount }, runOne));
    return results;
}

async function fetchOnce(url) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);
    try {
        const res = await fetch(url, {
            headers: { "User-Agent": USER_AGENT, Accept: "*/*" },
            signal: controller.signal,
        });
        if (!res.ok) throw new HttpStatusError(res.status, url);
        return res;
    } finally {
        clearTimeout(timer);
    }
}

function isNotFound(err) {
    return err instanceof HttpStatusError && err.status === 404;
}

async function fetchTextRetrying(url) {
    return withRetry(async () => (await fetchOnce(url)).text(), {
        isRetryable: (err) => !isNotFound(err),
    });
}

async function fetchBufferRetrying(url) {
    return withRetry(
        async () => Buffer.from(await (await fetchOnce(url)).arrayBuffer()),
        { isRetryable: (err) => !isNotFound(err) },
    );
}

/* ===================================================================
   webpack のランタイムのチャンクの表を読む

   チャンクの URL を作るランタイムの関数には、次の 2 つの形がある (discord.com の web.<hash>.js で確認した)。

     形 A (一部のチャンクだけの分岐): "<id>"===e?""+e+".<hash>.<ext>": ...
       -> URL は /assets/<id>.<hash>.<ext> (HTML にある eager なチャンクと同じ形)

     形 B (残りのチャンクの対応表。1 つの build に数千の項目がある): ""+({<id>:"<hash>",...}[e])+"."+ext
       (おおよその形)。区切りの文字が少し違うことがあるので、下の正規表現は「id:"16 進のハッシュ" が 6 つ以上
       並ぶ部分」をゆるく探し、直後の拡張子で js と css を分ける。
       -> URL は /assets/<hash>.<ext> (id を含まない。/assets/<id>.<hash>.js は 404 を返す)

   どちらの形も .css の URL を作ることがあるが、scan.ts は JavaScript だけを読むので、.js だけを取得する。

   この探し方は正規表現の一致で、webpack のランタイムの構造を解釈していない。Discord のビルドの構成が変わると、
   遅延読み込みのチャンクを取りこぼす。取りこぼしても、HTML にある eager なチャンクは取得できる。依存箇所の
   一覧が探す通話の基本の機能は、eager なチャンクにあるという前提で使う。
   =================================================================== */

// 正規表現の中の \x22 は二重引用符 1 文字を表す。そのまま " を書くと、test/lint/identifiers_english.py の文字列句解析
// (引用符の対応だけで文字列とコメントを分ける) が正規表現の中の " を文字列の始まりとみなし、それより後の行の
// 日本語のコメントを識別子として検出することがある。
const NAMED_CHUNK_RE =
    /\x22(\d+)\x22===[A-Za-z_$][\w$]*\?\x22\x22\+[A-Za-z_$][\w$]*\+\x22\.([0-9a-f]{6,20})\.(js|css)\x22/g;
const MAP_CHUNK_RE =
    /\{(?:[\w$]+:\x22[0-9a-f]{6,20}\x22,){5,}[\w$]+:\x22[0-9a-f]{6,20}\x22\}\)?\[[A-Za-z_$][\w$]*\](?:\|\|[A-Za-z_$][\w$]*\))?\+\x22\.(js|css)\x22/g;
const MAP_ENTRY_RE = /:\x22([0-9a-f]{6,20})\x22/g;

/** 1 つのファイルのテキストから、追加で取得するチャンクの候補を集める。 */
function discoverExtraChunks(text) {
    const found = new Map(); // 保存するファイルの名前 -> { url, name }

    NAMED_CHUNK_RE.lastIndex = 0;
    for (let m; (m = NAMED_CHUNK_RE.exec(text)); ) {
        const [, id, hash, ext] = m;
        if (ext !== "js") continue;
        const name = `${id}.${hash}.js`;
        found.set(name, { url: `/assets/${name}`, name });
    }

    MAP_CHUNK_RE.lastIndex = 0;
    for (let m; (m = MAP_CHUNK_RE.exec(text)); ) {
        const ext = m[1];
        if (ext !== "js") continue;
        MAP_ENTRY_RE.lastIndex = 0;
        for (let hm; (hm = MAP_ENTRY_RE.exec(m[0])); ) {
            const hash = hm[1];
            const name = `${hash}.js`;
            found.set(name, { url: `/assets/${name}`, name });
        }
    }

    return [...found.values()];
}

/** /assets/<name>.<hash>.js のようなパスからハッシュ部分 (拡張子の直前) を取り出す。 */
function extractHashFromAssetPath(assetPath) {
    const base = path.posix.basename(assetPath);
    const parts = base.split(".");
    if (parts.length < 2) return null;
    return parts[parts.length - 2];
}

/* ===================================================================
   manifest.json の読み書き
   =================================================================== */

function readManifest() {
    if (!existsSync(MANIFEST_PATH)) return [];
    const raw = readFileSync(MANIFEST_PATH, "utf8").trim();
    if (raw === "") return [];
    return JSON.parse(raw);
}

function findManifestEntry(channel, build) {
    return readManifest().find((e) => e.channel === channel && e.build === build) ?? null;
}

/** 同じ (channel, build) の項目が既にあれば、何もしない。 */
function appendManifestEntry(entry) {
    const list = readManifest();
    if (list.some((e) => e.channel === entry.channel && e.build === entry.build)) return;
    list.push(entry);
    mkdirSync(path.dirname(MANIFEST_PATH), { recursive: true });
    writeFileSync(MANIFEST_PATH, `${JSON.stringify(list, null, 2)}\n`, "utf8");
}

/* ===================================================================
   1 つのファイルのダウンロード (保存済みのファイルは読み込んで sha256 を計算するだけで、取得し直さない)
   =================================================================== */

async function downloadOne(target, buildDir, { allow404 = false } = {}) {
    const destPath = path.join(buildDir, target.name);
    try {
        if (existsSync(destPath) && statSync(destPath).size > 0) {
            const buf = readFileSync(destPath);
            return { name: target.name, bytes: buf.length, sha256: sha256Hex(buf), reused: true };
        }
        const buf = await fetchBufferRetrying(target.url);
        mkdirSync(buildDir, { recursive: true });
        // 途中で止まったときに書きかけのファイルを保存済みとして扱わないように、一時的な名前で書いてから名前を変える。
        const tmpPath = `${destPath}.part`;
        writeFileSync(tmpPath, buf);
        renameSync(tmpPath, destPath);
        return { name: target.name, bytes: buf.length, sha256: sha256Hex(buf), reused: false };
    } catch (err) {
        if (allow404 && isNotFound(err)) {
            return { name: target.name, notFound: true };
        }
        console.error(`[fetch] 取得できなかった: ${target.url} -- ${err.message}`);
        return { name: target.name, failed: true, error: String(err.message ?? err) };
    }
}

/* ===================================================================
   1 チャンネル分
   =================================================================== */

async function fetchChannel(channel) {
    const t0 = Date.now();
    const origin = new URL(channel.url).origin;

    console.log(`[fetch] ${channel.name}: ${channel.url} の HTML を取得します`);
    const html = await fetchTextRetrying(channel.url);

    // \x22 は " 1 文字 (NAMED_CHUNK_RE の上のコメントと同じ理由)。
    const scriptPaths = [
        ...new Set(
            [...html.matchAll(/<script[^>]+src=\x22(\/assets\/[^\x22]+\.js)\x22/g)].map((m) => m[1]),
        ),
    ];

    const buildNumber = html.match(/\x22BUILD_NUMBER\x22:\x22(\d+)\x22/)?.[1] ?? null;
    const versionHash = html.match(/\x22VERSION_HASH\x22:\x22([0-9a-f]+)\x22/)?.[1] ?? null;
    const releaseChannelField = html.match(/\x22RELEASE_CHANNEL\x22:\x22([a-z]+)\x22/)?.[1] ?? null;

    let buildId = buildNumber ?? (versionHash ? versionHash.slice(0, 12) : null);
    let buildIdSource = buildNumber ? "BUILD_NUMBER" : versionHash ? "VERSION_HASH" : null;
    if (!buildId) {
        // BUILD_NUMBER も VERSION_HASH もないときは、初期のスクリプトのハッシュ (ファイル名に入っている) をつないだ
        // 文字列の sha256 の先頭 12 文字を使う。同じ build でも HTML の <script> の順が変わらないとは限らない (確認して
        // いない) ので、並べ替えてからつなぐ。
        const hashes = scriptPaths
            .map(extractHashFromAssetPath)
            .filter(Boolean)
            .sort()
            .join(",");
        buildId = sha256Hex(hashes).slice(0, 12);
        buildIdSource = "sha256(初期スクリプトのハッシュの連結)";
    }

    console.log(`[fetch] ${channel.name}: build=${buildId} (${buildIdSource}), eager script 数=${scriptPaths.length}`);

    // manifest.json に同じ build があっても、ダウンロードはやめない。CI はチェックアウトしたばかりの状態で始まり、
    // 保存済みのファイルがないので、ここでやめるとテストがバンドルなしでスキップされ、Discord の現在のバンドルを
    // 確認できなくなる。保存済みのファイルは、downloadOne が読み込むだけで取得し直さない。manifest.json に同じ
    // 項目を 2 回追加しないことは、appendManifestEntry が守る。
    if (findManifestEntry(channel.name, buildId)) {
        console.log(`[fetch] ${channel.name}/${buildId}: manifest には既にあるが、バンドルを取得し直す (保存済みのファイルは読み直すだけ)`);
    }

    const buildDir = path.join(OUT_ROOT, channel.name, buildId);

    // 1. HTML にある eager なチャンクを取得する。
    const eagerTargets = scriptPaths.map((p) => ({ url: origin + p, name: path.posix.basename(p) }));
    const eagerResults = await runPool(eagerTargets, CONCURRENCY, (t) => downloadOne(t, buildDir));

    // 2. 取得した本文から、ランタイムのチャンクの表にある追加のチャンクを探す。
    const discovered = new Map();
    for (const t of eagerTargets) {
        const filePath = path.join(buildDir, t.name);
        if (!existsSync(filePath)) continue;
        let text;
        try {
            text = readFileSync(filePath, "utf8");
        } catch {
            continue;
        }
        for (const c of discoverExtraChunks(text)) discovered.set(c.name, c);
    }
    for (const t of eagerTargets) discovered.delete(t.name); // eager で取得したチャンクは除く

    const extraTargets = [...discovered.values()].map((c) => ({ url: origin + c.url, name: c.name }));
    console.log(`[fetch] ${channel.name}/${buildId}: ランタイムのチャンク表から追加候補 ${extraTargets.length} 件`);
    const extraResults = await runPool(extraTargets, CONCURRENCY, (t) => downloadOne(t, buildDir, { allow404: true }));

    const files = [];
    let totalBytes = 0;
    let notFoundCount = 0;
    let failedCount = 0;
    for (const r of [...eagerResults, ...extraResults]) {
        if (!r) continue;
        if (r.notFound) {
            notFoundCount++;
            continue;
        }
        if (r.failed) {
            failedCount++;
            continue;
        }
        files.push({ name: r.name, sha256: r.sha256, bytes: r.bytes });
        totalBytes += r.bytes;
    }

    const elapsedMs = Date.now() - t0;

    // ファイルごとの一覧 (名前、sha256、バイト数) は、git で管理しない files.json に置く。git で管理する
    // manifest.json に全部のファイルの sha256 を持つと、取得のたびに大きくなる。manifest.json には件数、合計サイズ、
    // digest だけを書く。
    mkdirSync(buildDir, { recursive: true });
    writeFileSync(path.join(buildDir, "files.json"), `${JSON.stringify(files, null, 2)}\n`, "utf8");

    // 404 以外の失敗があったまま取得済みとして書くと、ファイルが欠けていたことが後から分からなくなる。欠けたチャンクに
    // だけ一致する述語があると、テストの結果を誤るので、incomplete のフラグを残す。
    const incomplete = failedCount > 0;
    appendManifestEntry({
        fetchedAt: new Date().toISOString(),
        channel: channel.name,
        build: buildId,
        fileCount: files.length,
        totalBytes,
        digest: computeFilesDigest(files),
        ...(incomplete ? { incomplete: true } : {}),
    });

    const summary = {
        channel: channel.name,
        build: buildId,
        buildIdSource,
        releaseChannelField,
        versionHash,
        eagerCount: eagerTargets.length,
        extraDiscoveredCount: extraTargets.length,
        extraNotFoundCount: notFoundCount,
        failedCount,
        fileCount: files.length,
        totalBytes,
        elapsedMs,
    };
    console.log(`[fetch] ${channel.name}/${buildId} 完了:`, summary);
    return summary;
}

async function main() {
    mkdirSync(OUT_ROOT, { recursive: true });
    const summaries = [];
    for (const channel of CHANNELS) {
        summaries.push(await fetchChannel(channel));
    }
    console.log("[fetch] 全チャンネル完了");
    console.log(JSON.stringify(summaries, null, 2));

    // 404 以外の理由で取得に失敗したファイルがあるチャンネルがあれば、終了コードでも伝える (manifest.json の
    // incomplete と両方に残す)。
    const incompleteChannels = summaries.filter((s) => s.failedCount > 0).map((s) => `${s.channel}/${s.build}`);
    if (incompleteChannels.length > 0) {
        console.error(`[fetch] 取得に失敗したファイルがあり、バンドルが不完全: ${incompleteChannels.join(", ")}`);
        process.exitCode = 1;
    }
}

main().catch((err) => {
    console.error("[fetch] 致命的なエラー:", err);
    process.exitCode = 1;
});
