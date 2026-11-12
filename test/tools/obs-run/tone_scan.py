#!/usr/bin/env python3
"""tone_scan.py: 完成の音をデコードし、マーカーの音 (既定 2 kHz) がどこに何回あるかと、全体の音量を数える。

意図した音が入っているかを見る。テスト用の送信側はマーカーを持つ 1 ストリーム (既定は
画面の 1 つ目) にだけ 2 kHz を 60 ms ずつ乗せ、他のストリームは無音 (test/tools/harness/sender_pure.ts の
streamHasSyncTone)。au_hash.py は remux の枝しか見ない (ProRes の枝はエンコードし直すのでハッシュが合わない)
ので、ProRes の枝の音は「マーカーが期待どおりのストリームにだけ、期待どおりの回数あるか」で確認する。

使い方:
    python tone_scan.py --ffmpeg <ffmpeg の実行ファイル> [--hz 2000] [--chunk-ms 20] [--ratio 0.5] FILE...

出力: 1 行の JSON。files[] に file / duration_s / rms / peak / tone_chunks / bursts / first_tone_s /
last_tone_s / error。bursts は連続するマーカーのまとまりの数 (60 ms のマーカーなら 20 ms のブロック 3 つが 1 つのまとまり)。
判定はしない (数を出すだけ。判断は呼び手が行う)。

標準ライブラリだけで書く (test/lint の方針。numpy を要求しない)。デコードは ffmpeg に f32le / mono / 48 kHz で
標準出力へ出させる。Goertzel はブロックごとに Hann 窓をかけ、ブロックの中のマーカーの成分と全体の力の比を取る
(test/tools/mic-check/renderer.js と同じ測り方)。
"""

import argparse
import array
import json
import math
import os
import subprocess
import sys

SAMPLE_RATE = 48000


def decode_f32(ffmpeg, path):
    """ffmpeg で f32le / mono / 48 kHz にデコードして float の配列を返す。"""
    proc = subprocess.run(
        [ffmpeg, "-v", "error", "-i", path, "-vn", "-f", "f32le", "-ac", "1", "-ar", str(SAMPLE_RATE), "-"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError("ffmpeg failed: " + proc.stderr.decode("utf-8", "replace").strip()[:300])
    samples = array.array("f")
    samples.frombytes(proc.stdout[: len(proc.stdout) - (len(proc.stdout) % 4)])
    if sys.byteorder != "little":
        samples.byteswap()
    return samples


def goertzel_ratio(chunk, hz):
    """Hann 窓をかけたブロックの、hz の成分の力 / 全体の力 (0 から 1 程度)。無音なら 0。"""
    n = len(chunk)
    if n == 0:
        return 0.0, 0.0
    omega = 2.0 * math.pi * hz / SAMPLE_RATE
    coeff = 2.0 * math.cos(omega)
    s_prev = 0.0
    s_prev2 = 0.0
    total = 0.0
    for i in range(n):
        w = 0.5 - 0.5 * math.cos(2.0 * math.pi * i / (n - 1)) if n > 1 else 1.0
        x = chunk[i] * w
        total += x * x
        s = x + coeff * s_prev - s_prev2
        s_prev2 = s_prev
        s_prev = s
    power = s_prev2 * s_prev2 + s_prev * s_prev - coeff * s_prev * s_prev2
    # Goertzel の力は N^2 / 4 で正規化するとブロックの中の正弦波の振幅^2 に近い。全体の力は N で割る。
    tone = power / (n * n / 4.0) if n > 0 else 0.0
    mean_sq = total / n
    ratio = (tone / 2.0) / mean_sq if mean_sq > 1e-12 else 0.0
    return min(ratio, 1.5), mean_sq


def scan(samples, hz, chunk_ms, ratio_threshold):
    chunk = int(SAMPLE_RATE * chunk_ms / 1000)
    n_chunks = len(samples) // chunk
    tone_chunks = 0
    bursts = 0
    in_burst = False
    first = None
    last = None
    total_sq = 0.0
    peak = 0.0
    for c in range(n_chunks):
        part = samples[c * chunk : (c + 1) * chunk]
        ratio, mean_sq = goertzel_ratio(part, hz)
        total_sq += mean_sq * chunk
        for v in part:
            a = abs(v)
            if a > peak:
                peak = a
        # 無音のブロック (二乗平均が -60 dBFS より下) はマーカーと数えない。
        is_tone = ratio >= ratio_threshold and mean_sq > 1e-6
        if is_tone:
            tone_chunks += 1
            t = c * chunk_ms / 1000.0
            if first is None:
                first = t
            last = t
            if not in_burst:
                bursts += 1
                in_burst = True
        else:
            in_burst = False
    rms = math.sqrt(total_sq / len(samples)) if len(samples) else 0.0
    return {
        "duration_s": round(len(samples) / SAMPLE_RATE, 3),
        "rms": round(rms, 6),
        "peak": round(peak, 6),
        "tone_chunks": tone_chunks,
        "bursts": bursts,
        "first_tone_s": first,
        "last_tone_s": last,
    }


def parse_windows(text):
    """--windows の字 ("pre=2.0:5.5,held=14:20") を [(label, start_s, end_s)] に。形が違えば ValueError。"""
    out = []
    for part in (text or "").split(","):
        part = part.strip()
        if not part:
            continue
        label, _, span = part.partition("=")
        start, _, end = span.partition(":")
        if not label or not start or not end:
            raise ValueError("window must be label=start:end (got " + part + ")")
        s, e = float(start), float(end)
        if not (0 <= s < e):
            raise ValueError("window " + label + " needs 0 <= start < end")
        out.append((label, s, e))
    return out


def scan_windows(samples, windows, hz, chunk_ms, ratio_threshold):
    """区間ごとの音量 (二乗平均の根) と、hz の成分が主なブロックの割合 (0-1)。

    1 つの録画の中で「ミュートの前は鳴る・切り替えて保留の間は
    鳴らない・[再開] の後は鳴る」を見るため、録画の始めからの秒で区間を切って数える。判定はしない (呼び手がしきい値で読む)。
    区間が録画の外にはみ出した分は数えない (samples が足りなければ duration_s が短く出る)。
    """
    chunk = int(SAMPLE_RATE * chunk_ms / 1000)
    results = []
    for label, start_s, end_s in windows:
        a = int(start_s * SAMPLE_RATE)
        b = min(len(samples), int(end_s * SAMPLE_RATE))
        part = samples[a:b] if b > a else samples[0:0]
        n_chunks = len(part) // chunk if chunk > 0 else 0
        tone = 0
        total_sq = 0.0
        for c in range(n_chunks):
            piece = part[c * chunk : (c + 1) * chunk]
            r, mean_sq = goertzel_ratio(piece, hz)
            # 音量は Hann 窓をかける前のサンプルで数える (goertzel_ratio の mean_sq は Hann 窓をかけた後の値で、正弦波なら 3/8 に縮む)。
            total_sq += sum(v * v for v in piece)
            if r >= ratio_threshold and mean_sq > 1e-6:
                tone += 1
        counted = n_chunks * chunk
        results.append({
            "label": label,
            "start_s": start_s,
            "end_s": end_s,
            "duration_s": round(counted / SAMPLE_RATE, 3),
            "rms": round(math.sqrt(total_sq / counted), 6) if counted else None,
            "tone_share": round(tone / n_chunks, 3) if n_chunks else None,
        })
    return results


def selftest():
    """作り物の音 (配列。ファイルも ffmpeg も使わない) に scan_windows と parse_windows を当てる。"""
    failures = []

    def check(cond, label):
        print(("ok   " if cond else "FAIL ") + label)
        if not cond:
            failures.append(label)

    # 0-2 秒は 1 kHz (振幅 0.3 = テスト用の送信側と同じ)、2-4 秒は無音、4-6 秒は 1 kHz。
    samples = array.array("f", [0.0] * (SAMPLE_RATE * 6))
    for i in list(range(0, SAMPLE_RATE * 2)) + list(range(SAMPLE_RATE * 4, SAMPLE_RATE * 6)):
        samples[i] = 0.3 * math.sin(2.0 * math.pi * 1000.0 * i / SAMPLE_RATE)
    got = {w["label"]: w for w in scan_windows(samples, parse_windows("pre=0.2:1.8,held=2.2:3.8,resumed=4.2:5.8"), 1000.0, 20, 0.5)}
    check(abs(got["pre"]["rms"] - 0.3 / math.sqrt(2)) < 0.01 and got["pre"]["tone_share"] == 1.0, "a 1 kHz window reads rms 0.21 and tone_share 1")
    check(got["held"]["rms"] == 0.0 and got["held"]["tone_share"] == 0.0, "a silent window reads rms 0 and tone_share 0")
    check(got["resumed"]["tone_share"] == 1.0, "the tone after the gap is found again")
    other = scan_windows(samples, parse_windows("pre=0.2:1.8"), 2000.0, 20, 0.5)[0]
    check(other["tone_share"] == 0.0 and other["rms"] > 0.2, "a 2 kHz probe does not count the 1 kHz tone but still reads the level")
    beyond = scan_windows(samples, parse_windows("late=5.5:9"), 1000.0, 20, 0.5)[0]
    check(abs(beyond["duration_s"] - 0.5) < 0.021, "a window past the end counts only the samples that exist")
    for bad in ("x=3:2", "nolabel", "y=:2"):
        try:
            parse_windows(bad)
            check(False, "malformed window '" + bad + "' is refused")
        except ValueError:
            check(True, "malformed window '" + bad + "' is refused")
    print("selftest: %d failure(s)" % len(failures))
    return 1 if failures else 0


def main(argv):
    if len(argv) > 1 and argv[1] == "--selftest":
        return selftest()
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--hz", type=float, default=2000.0)
    parser.add_argument("--chunk-ms", type=int, default=20)
    parser.add_argument("--ratio", type=float, default=0.5)
    parser.add_argument("--windows", default=None,
                        help="label=start:end seconds from the start of the file, comma separated; adds windows[] (rms, tone_share) per file")
    parser.add_argument("files", nargs="+")
    args = parser.parse_args(argv[1:])
    windows = parse_windows(args.windows) if args.windows else None
    # 子プロセスの起動は「/ 区切りの相対パス」の実行ファイルを見つけられないことがあるので、絶対パスに修正する。
    # 区切りを含まない名前 ("ffmpeg") は PATH から探させる (実行スクリプトは同梱の ffmpeg が無ければ名前だけを渡す)。
    ffmpeg = os.path.abspath(args.ffmpeg) if (os.sep in args.ffmpeg or "/" in args.ffmpeg) else args.ffmpeg
    out = {"hz": args.hz, "chunk_ms": args.chunk_ms, "ratio": args.ratio, "files": []}
    for path in args.files:
        entry = {"file": path}
        try:
            samples = decode_f32(ffmpeg, path)
            entry.update(scan(samples, args.hz, args.chunk_ms, args.ratio))
            if windows is not None:
                entry["windows"] = scan_windows(samples, windows, args.hz, args.chunk_ms, args.ratio)
        except Exception as e:  # noqa: BLE001 (数を出すツール。落ちずに理由を残す)
            entry["error"] = str(e)
        out["files"].append(entry)
    print(json.dumps(out, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
