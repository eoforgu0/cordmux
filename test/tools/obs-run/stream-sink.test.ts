/*
 * hold と call-hold の配信の受け口 (stream-sink.mjs) の開き直しの動きを、偽の子プロセスで確認する。
 * ffmpeg -listen 1 は送信側 (OBS) が切ると終了コード 0 で終わる (同梱の ffmpeg n7.1.1 で確認した) ので、rearm の受け口は正常に
 * 終わるたびに次のファイルで開き直し、異常な終わり方・上限・stop の後では開き直さない。実際の ffmpeg は起動しない。
 * あわせて、親がいない受け口の確認 (stream-sink-stale.ps1) の出力のまとめ (summarizeStaleCheck) と、
 * ツールどうしのテキストの突き合わせ (ps1 の既定の URL と同梱の場所の既定が実行スクリプトと同じこと、実行スクリプトと
 * cleanup.ps1 が呼ぶ順と -Stop を渡さないこと、hold-info の driverPid) を確認する。判定そのものは
 * stream-sink-stale.ps1 -SelfTest が確認する (PowerShell と Win32_Process が要るため)。
 */
import { EventEmitter } from "node:events";
import { mkdtempSync, readFileSync, rmdirSync, unlinkSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

import { STREAM_SINK_URL } from "./run-window-plan.mjs";
import { createStreamSink, shouldRearm, streamSinkArgs, streamSinkFileName, summarizeStaleCheck } from "./stream-sink.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));

type FakeChild = EventEmitter & { pid: number; exitCode: number | null; killed: boolean; stderr: EventEmitter; kill: () => boolean };

function fakeSpawn() {
    const calls: Array<{ exe: string; args: string[]; opts: any; child: FakeChild }> = [];
    let nextPid = 100;
    const spawn = (exe: string, args: string[], opts: any) => {
        const child = new EventEmitter() as FakeChild;
        child.pid = nextPid++;
        child.exitCode = null;
        child.killed = false;
        child.stderr = new EventEmitter();
        child.kill = () => {
            child.killed = true;
            child.exitCode = null;
            child.emit("exit", null, "SIGTERM");
            return true;
        };
        calls.push({ exe, args, opts, child });
        return child;
    };
    const end = (i: number, code: number) => {
        calls[i].child.exitCode = code;
        calls[i].child.emit("exit", code, null);
    };
    return { spawn, calls, end };
}

const tick = () => new Promise((r) => setTimeout(r, 5));
const base = { exe: "ffmpeg.exe", url: "rtmp://127.0.0.1:1935/live/test", dir: "C:/out/window3b", tag: "c6", rearmDelayMs: 0 };

describe("stream-sink.mjs の名前と引数", () => {
    it("1 本目は前からの名前、2 本目からは -<n>", () => {
        expect(streamSinkFileName("c6", 1)).toBe("stream-c6.flv");
        expect(streamSinkFileName("c6", 2)).toBe("stream-c6-2.flv");
        expect(streamSinkFileName("c6", 3)).toBe("stream-c6-3.flv");
    });

    it("引数は 1 回だけ受けてコピーするだけ (前からの形)", () => {
        expect(streamSinkArgs("rtmp://127.0.0.1:1935/live/test", "x.flv")).toEqual(["-y", "-hide_banner", "-loglevel", "warning", "-listen", "1", "-i", "rtmp://127.0.0.1:1935/live/test", "-c", "copy", "x.flv"]);
    });

    it("開き直すのは rearm で上限の前で、終了コード 0 か flv に書いて終わったときだけ", () => {
        expect(shouldRearm({ rearm: true, code: 0, rearms: 0, maxRearms: 2, bytes: 0 })).toBe(true);
        expect(shouldRearm({ rearm: true, code: 1, rearms: 0, maxRearms: 2, bytes: 0 })).toBe(false);
        expect(shouldRearm({ rearm: true, code: null, rearms: 0, maxRearms: 2, bytes: null })).toBe(false);
        expect(shouldRearm({ rearm: false, code: 0, rearms: 0, maxRearms: 2, bytes: 10 })).toBe(false);
        expect(shouldRearm({ rearm: true, code: 0, rearms: 2, maxRearms: 2, bytes: 10 })).toBe(false);
        // OBS の切り方で 0 以外になっても、受けて書いた受け口は開き直す (ウィンドウで初めて分かる所を両方の形で通す)。
        expect(shouldRearm({ rearm: true, code: 1, rearms: 0, maxRearms: 2, bytes: 4096 })).toBe(true);
        expect(shouldRearm({ rearm: true, code: null, rearms: 0, maxRearms: 2, bytes: 1 })).toBe(true);
    });
});

describe("stream-sink.mjs の開き直しの輪 (偽の子プロセス)", () => {
    it("送信側が切って 0 で終わるたびに次のファイルで開き直す。ウィンドウを出さず、標準出力は捨てる", async () => {
        const f = fakeSpawn();
        const sink = createStreamSink({ ...base, rearm: true, maxRearms: 5, spawn: f.spawn });
        sink.start();
        expect(f.calls).toHaveLength(1);
        expect(f.calls[0].exe).toBe("ffmpeg.exe");
        expect(f.calls[0].args).toContain("rtmp://127.0.0.1:1935/live/test");
        expect(f.calls[0].opts).toMatchObject({ windowsHide: true, stdio: ["ignore", "ignore", "pipe"] });
        f.end(0, 0);
        await tick();
        expect(f.calls).toHaveLength(2);
        expect(f.calls[1].args.at(-1)).toMatch(/stream-c6-2\.flv$/);
        expect(sink.current()).toBe(f.calls[1].child);
        f.end(1, 0);
        await tick();
        expect(f.calls).toHaveLength(3);
        expect(sink.summary().rearms).toBe(2);
        expect(sink.summary().files.map((x) => x.file)).toEqual(["stream-c6.flv", "stream-c6-2.flv", "stream-c6-3.flv"]);
    });

    it("0 以外で終わったら開き直さない (起こせない・壊れた終わり方で輪にしない)", async () => {
        const f = fakeSpawn();
        const sink = createStreamSink({ ...base, rearm: true, maxRearms: 5, spawn: f.spawn });
        sink.start();
        f.end(0, 1);
        await tick();
        expect(f.calls).toHaveLength(1);
        expect(sink.alive()).toBe(false);
        expect(sink.summary().files[0].exitCode).toBe(1);
    });

    it("0 以外で終わっても flv に書いていれば開き直す (受け口の仕事はした)", async () => {
        const dir = mkdtempSync(join(tmpdir(), "stream-sink-test-"));
        const flv = join(dir, "stream-c6.flv");
        try {
            const f = fakeSpawn();
            const sink = createStreamSink({ ...base, dir, rearm: true, maxRearms: 5, spawn: f.spawn });
            sink.start();
            writeFileSync(flv, "FLV");
            f.end(0, 255);
            await tick();
            expect(f.calls).toHaveLength(2);
            expect(sink.summary().files[0]).toMatchObject({ exitCode: 255, bytes: 3 });
            sink.stop();
        } finally {
            unlinkSync(flv);
            rmdirSync(dir);
        }
    });

    it("起こせなかった子 (pid がなく error だけが来る) は動いていないと読む", () => {
        const f = fakeSpawn();
        const spawn = (exe: string, args: string[], opts: any) => {
            const child = f.spawn(exe, args, opts);
            (child as any).pid = undefined;
            return child;
        };
        const sink = createStreamSink({ ...base, rearm: true, maxRearms: 5, spawn });
        const entry = sink.start();
        f.calls[0].child.emit("error", new Error("spawn ffmpeg ENOENT"));
        expect(sink.alive()).toBe(false);
        expect(entry.pid).toBe(null);
        expect(entry.error).toContain("ENOENT");
    });

    it("rearm を立てない受け口 (through・integration・call-1) は 1 回きり", async () => {
        const f = fakeSpawn();
        const sink = createStreamSink({ ...base, rearm: false, maxRearms: 5, spawn: f.spawn });
        sink.start();
        f.end(0, 0);
        await tick();
        expect(f.calls).toHaveLength(1);
    });

    it("上限まで開き直したら止まる", async () => {
        const f = fakeSpawn();
        const sink = createStreamSink({ ...base, rearm: true, maxRearms: 2, spawn: f.spawn });
        sink.start();
        for (let i = 0; i < 3; i++) {
            f.end(i, 0);
            await tick();
        }
        expect(f.calls).toHaveLength(3);
        expect(sink.summary().rearms).toBe(2);
    });

    it("stop は動いている受け口を止め、その後は開き直さない", async () => {
        const f = fakeSpawn();
        const sink = createStreamSink({ ...base, rearm: true, maxRearms: 5, spawn: f.spawn });
        sink.start();
        sink.stop();
        await tick();
        expect(f.calls[0].child.killed).toBe(true);
        expect(f.calls).toHaveLength(1);
        expect(sink.summary().rearm).toBe(false);
        expect(sink.alive()).toBe(false);
    });

    it("開き直しを待つ間に stop しても起き直さない", async () => {
        const f = fakeSpawn();
        const sink = createStreamSink({ ...base, rearmDelayMs: 20, rearm: true, maxRearms: 5, spawn: f.spawn });
        sink.start();
        f.end(0, 0);
        sink.stop();
        await new Promise((r) => setTimeout(r, 40));
        expect(f.calls).toHaveLength(1);
    });

    it("標準エラーは末尾だけを持つ", () => {
        const f = fakeSpawn();
        const sink = createStreamSink({ ...base, rearm: false, maxRearms: 0, spawn: f.spawn });
        sink.start();
        f.calls[0].child.stderr.emit("data", Buffer.from("x".repeat(1000) + "END"));
        const tail = sink.summary().files[0].stderrTail;
        expect(tail.length).toBe(800);
        expect(tail.endsWith("END")).toBe(true);
    });
});

describe("親がいない受け口の確認 (stream-sink-stale.ps1) の出力のまとめ", () => {
    const line = (o: unknown) => JSON.stringify(o);

    it("止めた物・止める前に居なくなっていた物・動いている実行スクリプトの物を分けて拾う (already-gone は stopped に数えない)", () => {
        const out = line({
            ok: true,
            stop: true,
            exe: "C:\\r\\ffmpeg.exe",
            url: STREAM_SINK_URL,
            stale: [
                { pid: 11, parentPid: 21, createdAt: "t", outcome: "stopped", stopped: true, error: null },
                { pid: 12, parentPid: 22, createdAt: "t", outcome: "already-gone", stopped: false, error: null },
            ],
            owned: [{ pid: 13, parentPid: 23 }],
            undecided: [],
            remaining: [],
            refused: false,
            error: null,
        });
        expect(summarizeStaleCheck(out, 0)).toEqual({
            ok: true,
            exitCode: 0,
            stale: [11, 12],
            stopped: [11],
            alreadyGone: [12],
            owned: [13],
            undecided: [],
            remaining: [],
            refused: false,
            error: null,
        });
    });

    it("stopped:true だけの古い形でも outcome が stopped でなければ止めた物に数えない", () => {
        const out = line({ ok: true, stale: [{ pid: 12, stopped: true, error: "already gone" }], owned: [], remaining: [], refused: false, error: null });
        expect(summarizeStaleCheck(out, 0).stopped).toEqual([]);
    });

    it("見つけただけ (実行スクリプトは -Stop を渡さない) の物は stale と remaining に出て ok は偽", () => {
        const out = line({ ok: false, stop: false, stale: [{ pid: 11, parentPid: 21, outcome: "found", stopped: false, error: null }], owned: [], undecided: [], remaining: [11], refused: false, error: null });
        const r = summarizeStaleCheck(out, 1);
        expect(r.ok).toBe(false);
        expect(r.stale).toEqual([11]);
        expect(r.stopped).toEqual([]);
        expect(r.remaining).toEqual([11]);
    });

    it("決められなかった受け口 (終了コード 3) は undecided に出て ok は偽", () => {
        const out = line({ ok: false, stop: false, stale: [], owned: [], undecided: [{ pid: 15, parentPid: 25, error: "WMI" }], remaining: [], refused: false, error: "some receivers could not be decided" });
        const r = summarizeStaleCheck(out, 3);
        expect(r.ok).toBe(false);
        expect(r.undecided).toEqual([15]);
        expect(r.error).toBe("some receivers could not be decided");
    });

    it("1 つだけの配列が物 1 つで来ても並びとして読む", () => {
        const out = line({ ok: true, stale: { pid: 11, outcome: "stopped", stopped: true }, owned: { pid: 13 }, undecided: { pid: 15 }, remaining: [], refused: false, error: null });
        const r = summarizeStaleCheck(out, 0);
        expect(r.stopped).toEqual([11]);
        expect(r.owned).toEqual([13]);
        expect(r.undecided).toEqual([15]);
    });

    it("断り (終了コード 2)・出力なし・読めない出力・終了コードの食い違いは ok:false", () => {
        expect(summarizeStaleCheck(line({ ok: false, refused: true, error: "only the bundled ffmpeg.exe" }), 2)).toMatchObject({ ok: false, refused: true, error: "only the bundled ffmpeg.exe" });
        expect(summarizeStaleCheck("", null)).toMatchObject({ ok: false, error: "no result from stream-sink-stale.ps1" });
        expect(summarizeStaleCheck(undefined as unknown as string, null)).toMatchObject({ ok: false, stopped: [] });
        expect(summarizeStaleCheck("{not json", 0).ok).toBe(false);
        expect(summarizeStaleCheck(line({ ok: true, stale: [], owned: [], remaining: [] }), 1).ok).toBe(false);
    });

    it("JSON の前に別の行があっても最後の JSON の行を読む", () => {
        const out = "warning: something\r\n" + line({ ok: true, stale: [], owned: [], remaining: [], refused: false, error: null }) + "\r\n";
        expect(summarizeStaleCheck(out, 0).ok).toBe(true);
    });
});

describe("親がいない受け口のツールどうしのテキストの突き合わせ", () => {
    const staleSrc = readFileSync(join(HERE, "stream-sink-stale.ps1"), "utf8");
    const driverSrc = readFileSync(join(HERE, "run-window.mjs"), "utf8");
    const cleanupSrc = readFileSync(join(HERE, "cleanup.ps1"), "utf8");

    it("stream-sink-stale.ps1 の既定の URL は実行スクリプトの STREAM_SINK_URL と同じ文字列", () => {
        // 引用符は \x27 で書く (識別子の確認が正規表現の中の引用符を文字列の始まりと読むため)。
        const m = staleSrc.match(/^\$DefaultStreamSinkUrl = \x27([^\x27]+)\x27$/m);
        expect(m).not.toBeNull();
        expect(m![1]).toBe(STREAM_SINK_URL);
    });

    it("stream-sink-stale.ps1 の同梱の場所の既定は実行スクリプトの OBS_DEPS_BIN の既定と同じ", () => {
        const ps = staleSrc.match(/^\$FallbackDepsBinRel = \x27([^\x27]+)\x27$/m);
        const js = driverSrc.match(/return path\.join\(REPO_ROOT, ((?:\x22[^\x22]+\x22, )*\x22[^\x22]+\x22)\);\n\}\)\(\);\nconst BUNDLED_FFMPEG = path\.join\(OBS_DEPS_BIN, \x22ffmpeg\.exe\x22\);/);
        expect(ps).not.toBeNull();
        expect(js).not.toBeNull();
        const parts = [...js![1].matchAll(/\x22([^\x22]+)\x22/g)].map((x) => x[1]);
        expect(ps![1]).toBe(parts.join("\\"));
    });

    it("実行スクリプトの stream-sink-start は 1935 番の先客の確認の前に -Stop なしで呼び (見つけるだけ)、同梱の場所と URL を渡す", () => {
        const start = driverSrc.indexOf('case "stream-sink-start": {');
        const end = driverSrc.indexOf('case "record-start": {', start);
        expect(start).toBeGreaterThan(0);
        const block = driverSrc.slice(start, end);
        const call = block.indexOf("STREAM_SINK_STALE_PS1");
        expect(call).toBeGreaterThan(0);
        expect(call).toBeLessThan(block.indexOf("Get-NetTCPConnection"));
        expect(block).toMatch(/STREAM_SINK_STALE_PS1, \x22-StreamSinkExe\x22, BUNDLED_FFMPEG, \x22-StreamSinkUrl\x22, STREAM_SINK_URL\]/);
        expect(block).not.toContain('"-Stop"');
        expect(driverSrc).toMatch(/const STREAM_SINK_STALE_PS1 = path\.join\(HERE, \x22stream-sink-stale\.ps1\x22\);/);
    });

    it("cleanup.ps1 は自分のプロセスを止めた後・*.flv を消す前に呼び、-DryRun でも素でも -Stop を渡さない", () => {
        const call = cleanupSrc.indexOf("Join-Path $scriptDir 'stream-sink-stale.ps1'");
        expect(call).toBeGreaterThan(cleanupSrc.indexOf("$remainingOwn = @(Get-OwnTargetProcesses"));
        expect(call).toBeLessThan(cleanupSrc.indexOf("$deleteExtensions = "));
        const section = cleanupSrc.slice(call, cleanupSrc.indexOf("$deleteExtensions = "));
        expect(section).not.toMatch(/\x27-Stop\x27/);
        expect(cleanupSrc).toContain("streamSinkCheck = $streamSinkCheck");
    });

    it("cleanup.ps1 は終了コード 3 (決められない) を問題に書き、親がいない受け口を止めずに問題として書く", () => {
        expect(cleanupSrc).toContain("if ($staleExit -eq 3) {");
        expect(cleanupSrc).toContain("could not decide whether a stream receiver is stale (nothing was stopped): ");
        expect(cleanupSrc).toContain("(its parent process ");
        expect(cleanupSrc).toContain("cleanup.ps1 does not stop it (stop it with stream-sink-stale.ps1 -Stop)");
        expect(cleanupSrc).not.toContain("(its driver ");
    });

    it("実行スクリプトの hold-info は実際の node の pid (driverPid = process.pid) を書く (Volta の shim を打ち切っても実行スクリプトは止まらない)", () => {
        const start = driverSrc.indexOf('case "hold-info": {');
        const block = driverSrc.slice(start, driverSrc.indexOf("break;", start));
        expect(start).toBeGreaterThan(0);
        expect(block).toContain("driverPid: process.pid,");
    });
});
