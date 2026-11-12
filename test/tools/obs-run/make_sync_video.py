#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# 実際の通話で共有する「音つきのテスト動画」を作り、作った動画そのものを sync.py でセルフテストする。
#
# 共有する動画は、テスト用の送信側と同じマーカーを含む。5 秒ごとに全面が白くなる 3 フレームと、同じ
# 時刻から 60 ms だけ 1 kHz が 2 kHz に変わる音で、sync.py がそのまま使える。
#
# 作る動画: 1280x720、60 fps、既定 10 分、H.264 (yuv420p) + AAC 48 kHz ステレオ の mp4。
#   映像: 灰色の下地の上に、左右に往復する円、人が読む大きなフレーム番号、左上にフレーム番号の
#         「時計つきの帯」(bit_strip.py の並び。share/pattern.html と同じ。frames.py が区画の中で探して読む)
#         を描く。下地を真っ黒にしないのは、black_span.py が黒埋めと見分けられなくなるため。
#         5 秒ごとの境界 (フレーム番号が 300 の倍数) から 3 フレームは全面が白になる。帯だけは白の上にも
#         描く (sender.ts の drawSyncWhiteFrame と同じ。帯は自分の黒い余白を持つので白の上でも縁が出る)。
#         帯の升は 24 px (STRIP_CELL)。Chrome のタブの共有では、動画は表示域ごと 1280x720 へ縦横別の
#         倍率で縮められて写る。そのため、枠の左上を読むだけの方式では読めず、frames.py は帯を探して読む。
#         24 px は、白のマーカーのフレームで帯 (840x144 px、動画の 13%) が白を減らしても、動画の上で
#         白と判定できる余裕が残る大きさである。pattern.html と同じ 32 px にすると、帯が動画の 23% を
#         占めて白のマーカーの平均が下がる。sync.py の白の判定は「明るくなった点の割合」(帯の分を引く)
#         なので、24 px でも 32 px でも境目 0.2 から遠い。24 px のままにしているのは、frames.py /
#         smooth.py で読めることを確認した大きさで、変えると動画を作り直すことになるため。
#         枠の 0.5 倍に縮めても升は 8 x 9.6 px で、frames.py の下限 (3 px) の上にある。
#   音:   1 kHz の正弦波 (振幅 0.251 = -12 dBFS)。同じ境界から 60 ms だけ 2 kHz。両チャンネル同じ。
#         切り替わる時刻で位相が 0 になる (5 秒と 60 ms はどちらの周期の整数倍) ので継ぎ目に
#         クリック音は出ない。
#
# 映像は ffmpeg のフィルタではなく Python (numpy) で描いて rawvideo で流し込む。同梱の ffmpeg
# (obs-deps のビルド) には drawtext (libfreetype) がなく、フレーム番号を ffmpeg だけでは描けない。
# 自前で 5x7 のビットマップ数字を描けば、フレーム番号もビット列の帯も出せ、frames.py で受け側の
# フレーム落ちを数えられる (図形だけでは目視にしか使えない)。1 フレーム 2.7 MB x 36000 フレームを
# パイプに流す代償は数分の CPU 時間である。
# 音は ffmpeg の aevalsrc で作る (Python で書くより短く、サンプルの取りこぼしがない)。
#
# セルフテスト: 生成した mp4 に sync.py (PATH の ffmpeg / ffprobe を使う。同梱の bin に ffprobe はない)
# を実行し、白と 2 kHz の対の数が 100 以上、ずれの絶対値の最大が 10 ms 以内であることを確認する。
# どちらかが外れれば終了コード 3。
#
# 保存先は _share/ (_work/ の外)。cleanup.ps1 は各手順の終わりに -WorkDir (_work)
# の下の mp4 / wav を全部消す (テストデータの後片付け) ので、実行の中で何度も使う動画を _work/ に置くと
# 最初の手順の終わりで消える。
# 実行の終わりの削除は `run-window.mjs --step call-1 --shared-content-dir _share` で登録し、
# `cleanup.ps1 -CallWindow` に任せる (README.md「後片付け (`cleanup.ps1`)」)。
#
# --seconds が 500 未満だと境界が 100 に届かずセルフテストは必ず失敗になる (短い動作確認は --skip-check)。
#
# 使い方:
#   python make_sync_video.py [--out _share/sync-video.mp4] [--seconds 600]
#                             [--ffmpeg <ffmpeg.exe>] [--skip-check] [--no-copy-html]
# 結果は末尾の 1 行 `CORDMUX_SYNC_VIDEO_JSON:<JSON>`。終了コード 0 = できてセルフテストも成功、
# 1 = ffmpeg がない・失敗、2 = 引数の誤り、3 = セルフテストが失敗。
# 生成物と同じフォルダに share/sync-video.html をコピーする (動画を流すページ。--no-copy-html で止める)。

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

import bit_strip

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
RESULT_PREFIX = "CORDMUX_SYNC_VIDEO_JSON:"
SYNC_PY = HERE / "sync.py"
SHARE_HTML = HERE / "share" / "sync-video.html"
DEFAULT_OUT = REPO_ROOT / "_share" / "sync-video.mp4"
FFMPEG_CANDIDATES = [
    REPO_ROOT / "_work" / "install" / "cordmux" / "bin" / "64bit" / "ffmpeg.exe",
    REPO_ROOT / "src-obs" / ".deps" / "obs-deps-2025-07-11-x64" / "bin" / "ffmpeg.exe",
]

WIDTH = 1280
HEIGHT = 720
FPS = 60
SAMPLE_RATE = 48000
SYNC_PERIOD_FRAMES = 5 * FPS  # 5 秒ごとの境界
WHITE_FRAMES = 3
BURST_SECONDS = 0.06
AMPLITUDE = 0.251  # -12 dBFS
STRIP_CELL = 24  # 時計つきの帯の升の一辺 (由来は冒頭の注記)
DIGIT_SCALE = 20  # 5x7 のビットマップを何倍で描くか (1 桁 100x140 px)
MAX_PAIR_ABS_MS = 10.0
MIN_PAIRS = 100

# 5x7 のビットマップ数字 (1 が点)。人が読むための大きなフレーム番号に使う。
DIGITS = {
    "0": ["01110", "10001", "10011", "10101", "11001", "10001", "01110"],
    "1": ["00100", "01100", "00100", "00100", "00100", "00100", "01110"],
    "2": ["01110", "10001", "00001", "00010", "00100", "01000", "11111"],
    "3": ["11111", "00010", "00100", "00010", "00001", "10001", "01110"],
    "4": ["00010", "00110", "01010", "10010", "11111", "00010", "00010"],
    "5": ["11111", "10000", "11110", "00001", "00001", "10001", "01110"],
    "6": ["00110", "01000", "10000", "11110", "10001", "10001", "01110"],
    "7": ["11111", "00001", "00010", "00100", "01000", "01000", "01000"],
    "8": ["01110", "10001", "10001", "01110", "10001", "10001", "01110"],
    "9": ["01110", "10001", "10001", "01111", "00001", "00010", "01100"],
}


def find_ffmpeg(explicit):
    if explicit:
        p = Path(explicit)
        return p if p.is_file() else None
    for c in FFMPEG_CANDIDATES:
        if c.is_file():
            return c
    return None


def build_background():
    # 上から下へ 48 -> 112 の灰色の縦の勾配に、薄い格子を重ねる (動きのない所でもエンコーダが
    # 「一様な面」として潰さない程度の模様。全面の平均輝度は 80 前後 = 白の判定 200 に遠い)。
    rows = np.linspace(48, 112, HEIGHT, dtype=np.float32)
    bg = np.repeat(rows[:, None], WIDTH, axis=1)
    yy, xx = np.mgrid[0:HEIGHT, 0:WIDTH]
    grid = (((yy // 40) + (xx // 40)) % 2).astype(np.float32) * 10
    img = np.clip(bg + grid, 0, 255).astype(np.uint8)
    return np.repeat(img[:, :, None], 3, axis=2).copy()


def build_digit_sprites():
    sprites = {}
    for ch, rows in DIGITS.items():
        bitmap = np.array([[c == "1" for c in row] for row in rows], dtype=bool)
        sprites[ch] = np.kron(bitmap, np.ones((DIGIT_SCALE, DIGIT_SCALE), dtype=bool))
    return sprites


def build_circle_mask(radius):
    yy, xx = np.mgrid[-radius:radius + 1, -radius:radius + 1]
    return (xx * xx + yy * yy) <= radius * radius


def hsl_to_rgb(h, s, l):
    # sender.ts の `hsl(hue, 70%, 55%)` と同じ見え方にするための小さな変換。
    c = (1 - abs(2 * l - 1)) * s
    hp = (h % 360) / 60.0
    x = c * (1 - abs(hp % 2 - 1))
    r, g, b = {0: (c, x, 0), 1: (x, c, 0), 2: (0, c, x), 3: (0, x, c), 4: (x, 0, c), 5: (c, 0, x)}[int(hp) % 6]
    m = l - c / 2
    return tuple(int(round((v + m) * 255)) for v in (r, g, b))


def draw_bits(frame, n):
    # 動画の左上に時計つきの帯 (bit_strip.py。share/pattern.html の drawStrip と同じ並び)。
    bit_strip.draw_strip(frame, 0, 0, STRIP_CELL, n)


def draw_number(frame, sprites, n, x, y):
    for ch in str(n):
        sp = sprites[ch]
        h, w = sp.shape
        region = frame[y:y + h, x:x + w, :]
        region[sp] = 255
        x += w + DIGIT_SCALE


def draw_circle(frame, mask, cx, cy, color):
    r = mask.shape[0] // 2
    x0, y0 = cx - r, cy - r
    region = frame[y0:y0 + mask.shape[0], x0:x0 + mask.shape[1], :]
    region[mask] = color


def render_frame(out, bg, sprites, circle, n):
    if n % SYNC_PERIOD_FRAMES < WHITE_FRAMES:
        out[:] = 255
        draw_bits(out, n)
        return
    np.copyto(out, bg)
    # 円: 180 フレームで往復 (sender.ts の cyclePeriod と同じ)。色はフレーム番号でゆっくり変わる。
    phase = (n % 180) / 180.0
    sweep = phase * 2 if phase < 0.5 else 2 - phase * 2
    cx = int(60 + sweep * (WIDTH - 120))
    draw_circle(out, circle, cx, HEIGHT // 2, hsl_to_rgb((n // 60) * 7, 0.7, 0.55))
    draw_number(out, sprites, n, 40, int(HEIGHT * 0.6))
    draw_bits(out, n)


def audio_source(seconds):
    # 1 kHz、5 秒ごとの境界から 60 ms だけ 2 kHz。両チャンネルに同じ式。
    expr = "%s*sin(2*PI*if(lt(mod(t\\,5)\\,%s)\\,2000\\,1000)*t)" % (AMPLITUDE, BURST_SECONDS)
    return "aevalsrc=exprs=%s|%s:s=%d:c=stereo:d=%s" % (expr, expr, SAMPLE_RATE, seconds)


def ffmpeg_command(ffmpeg, out_path, seconds):
    return [
        str(ffmpeg), "-hide_banner", "-nostats", "-loglevel", "error", "-y",
        "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "%dx%d" % (WIDTH, HEIGHT), "-r", str(FPS), "-i", "-",
        "-f", "lavfi", "-i", audio_source(seconds),
        "-map", "0:v:0", "-map", "1:a:0",
        "-c:v", "libx264", "-preset", "veryfast", "-crf", "18", "-pix_fmt", "yuv420p", "-g", str(FPS),
        "-c:a", "aac", "-b:a", "192k", "-ar", str(SAMPLE_RATE), "-ac", "2",
        "-movflags", "+faststart", "-t", str(seconds), str(out_path),
    ]


def generate(ffmpeg, out_path, seconds):
    frames_total = seconds * FPS
    bg = build_background()
    sprites = build_digit_sprites()
    circle = build_circle_mask(int(min(WIDTH, HEIGHT) * 0.05))
    frame = np.empty_like(bg)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    started = time.time()
    proc = subprocess.Popen(ffmpeg_command(ffmpeg, out_path, seconds), stdin=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        for n in range(frames_total):
            render_frame(frame, bg, sprites, circle, n)
            proc.stdin.write(frame.tobytes())
    except BrokenPipeError:
        pass
    finally:
        proc.stdin.close()
    stderr = proc.stderr.read().decode("utf-8", "replace")
    proc.wait()
    return {"frames_written": frames_total, "encode_seconds": round(time.time() - started, 1), "returncode": proc.returncode, "stderr": stderr.strip()[-2000:]}


def self_check(out_path):
    # sync.py は PATH の ffmpeg / ffprobe を呼ぶ。stdout の JSON をそのまま読む。子の Python の
    # 標準出力は既定だと cp932 になり日本語の注記で崩れるので、UTF-8 に固定して起こす。
    env = dict(os.environ, PYTHONIOENCODING="utf-8")
    res = subprocess.run([sys.executable, str(SYNC_PY), str(out_path)], capture_output=True, text=True,
                         encoding="utf-8", errors="replace", env=env)
    if res.returncode != 0:
        return {"ok": False, "error": "sync.py failed", "stderr": res.stderr.strip()[-2000:]}
    data = json.loads(res.stdout)
    stats = data["audio_minus_video_ms"]
    worst = max(abs(stats["min"]), abs(stats["max"])) if stats["min"] is not None else None
    ok = data["paired"] >= MIN_PAIRS and worst is not None and worst <= MAX_PAIR_ABS_MS
    return {
        "ok": ok, "fps": data["fps"], "frames": data["frames"],
        "white_events": data["white_events"], "tone_events": data["tone_events"], "paired": data["paired"],
        "audio_minus_video_ms": stats, "worst_abs_ms": worst,
        "limits": {"min_pairs": MIN_PAIRS, "max_abs_ms": MAX_PAIR_ABS_MS},
    }


def copy_html(out_path):
    if not SHARE_HTML.is_file():
        return None
    dest = out_path.parent / SHARE_HTML.name
    shutil.copyfile(SHARE_HTML, dest)
    return str(dest)


def emit(result, code):
    print(RESULT_PREFIX + json.dumps(result, ensure_ascii=False))
    return code


def main(argv):
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--seconds", type=int, default=600)
    ap.add_argument("--ffmpeg", default=None)
    ap.add_argument("--skip-check", action="store_true")
    ap.add_argument("--no-copy-html", action="store_true")
    try:
        args = ap.parse_args(argv)
    except SystemExit:
        return 2
    if args.seconds <= 0 or args.seconds % 5 != 0:
        return emit({"ok": False, "error": "--seconds must be a positive multiple of 5"}, 2)
    ffmpeg = find_ffmpeg(args.ffmpeg)
    if ffmpeg is None:
        return emit({"ok": False, "error": "ffmpeg not found", "candidates": [str(c) for c in FFMPEG_CANDIDATES]}, 1)
    out_path = Path(args.out).resolve()
    gen = generate(ffmpeg, out_path, args.seconds)
    result = {"ok": False, "out": str(out_path), "ffmpeg": str(ffmpeg), "seconds": args.seconds, "generate": gen}
    if gen["returncode"] != 0 or not out_path.is_file():
        return emit(result, 1)
    result["size_bytes"] = os.path.getsize(out_path)
    if not args.no_copy_html:
        result["html"] = copy_html(out_path)
    if args.skip_check:
        result["ok"] = True
        result["check"] = None
        return emit(result, 0)
    check = self_check(out_path)
    result["check"] = check
    result["ok"] = bool(check.get("ok"))
    return emit(result, 0 if result["ok"] else 3)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
