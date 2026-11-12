#!/usr/bin/env python3
"""border_timing.py: 発言中の光が点く・保つ・消える時機を、OBS の録画のフレームと音から測る。

測るもの: 声の始まりから発言中の光が点くまでの時間、保つ時間、消える時間。声の遅延と揃うか、1 秒に 3 回より多く
点いたり消えたりしないかも見る。時機の式は src-obs/sources/label_border.h の speaking_level
(声が始まってから rise_ms で最大に、止まってから hold_ms 保ち、fade_ms で直線に 0 へ。合成のソース cordmux の光も
同じ式を使い、既定は rise_ms と fade_ms が 150 ms、hold_ms が 200 ms で、settings.json の label_border_rise_ms / hold_ms / fade_ms で
変わる)。測る対象は実機で録った、実際の声の録画。

測り方:
  - 映像: 光 (合成のソース cordmux が話している人に付ける、枠・アバター・名前の光。色は label_border_color) を含む箱
    (--region。録画の中の人の映像の箱) の外周の内側の帯 (--strip ピクセル) のピクセルごとに、最初のフレーム (--ref-at-s の
    フレーム。光が消えている時刻を選ぶ) の色から、光の色 (--color。既定は label_border_color の既定 #57F287) へどれだけ
    寄ったかを 0 から 1 で読み (色の差の向きへの射影)、帯の中央値をそのフレームの明るさ (level) とする。製品の明るさは
    線形に下がるので、level の 0.5 を横切った時刻を「点いた」「消えた」、0.95 を下回り始めた時刻を「消え始め」、
    0.05 を下回った時刻を「消えきった」とする。帯が光る所に掛かる設定 (枠を光らせる label_glow_frame を入れる、または
    --region を光るアバターや名前の箱にそろえる) で撮ること。
  - 音: 録画の音 (48 kHz モノラル、映像と同じ原点) の 10 ms ごとの音量 (二乗平均の根) が --voice-dbfs を超えた所を声とし、
    --min-silence-s より短い切れ目はつなぐ。声の区間ごとに、その後の枠線の出来事を並べる。
出力: 1 行の JSON。voices[] (start_s, end_s)、border.events[] (rise / fall の時刻)、pairs[] (声ごとの onDelayS = 点いた - 声の
始まり、holdS = 消え始め - 声の終わり、fadeS = 消えきった - 消え始め)、flipsMaxPerSecond (1 秒の範囲の中の点く・消えるの数の
最大)。判定はしない (数を出すだけ。設定の rise / hold / fade の値と声の遅延の設定との比べは呼び手が行う)。

使い方:
    python border_timing.py REC.mkv --region x,y,w,h [--color 57F287] [--strip 12] [--ref-at-s 0] [--ffmpeg PATH]
    python border_timing.py --selftest [--ffmpeg PATH]   (作り物の動画を呼び手の TEMP の下に作って測り、消す)
numpy を使う (video_frames.py と同じ)。
"""

import argparse
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import video_frames  # noqa: E402

# 光の既定の色 (config/settings.h の kDefaultLabelBorderColor #57F287)。
DEFAULT_COLOR = "57F287"
# 声とみなす音量 (dBFS)。話し声の録画は -30 から -15 dBFS 前後、部屋の音とエンコードの揺れは -50 以下。
DEFAULT_VOICE_DBFS = -40.0
# この長さより短い無音は声の切れ目とみなさない (秒)。製品の発言中の光も短い息継ぎでは消えない前提で、0.15 秒。
DEFAULT_MIN_SILENCE_S = 0.15
AUDIO_WINDOW_S = 0.01
LEVEL_ON = 0.5
LEVEL_FULL = 0.95
LEVEL_OFF = 0.05


def parse_color(text):
    h = text.lstrip("#")
    return np.array([int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)], dtype=np.float32)


def strip_mask(h, w, strip):
    m = np.zeros((h, w), dtype=bool)
    s = max(1, min(strip, h // 2, w // 2))
    m[:s, :] = True
    m[-s:, :] = True
    m[:, :s] = True
    m[:, -s:] = True
    return m


def frame_level(rgb, ref, target, mask):
    """帯のピクセルごとに、ref の色から target の色へどれだけ寄ったか (0-1) の中央値。"""
    px = rgb[mask].astype(np.float32)
    base = ref[mask].astype(np.float32)
    d = target[None, :] - base
    denom = (d * d).sum(axis=1)
    ok = denom > 400.0  # 地が線の色に近すぎるピクセルは読めない (差が 20 未満)
    if not ok.any():
        return None
    t = ((px - base) * d).sum(axis=1)[ok] / denom[ok]
    return float(np.median(np.clip(t, 0.0, 1.0)))


def voice_segments(samples, sample_rate, dbfs, min_silence_s):
    win = max(1, int(sample_rate * AUDIO_WINDOW_S))
    n = len(samples) // win
    if n == 0:
        return []
    frames = np.asarray(samples[: n * win], dtype=np.float64).reshape(n, win)
    rms = np.sqrt((frames * frames).mean(axis=1))
    loud = rms >= 10 ** (dbfs / 20.0)
    segs = []
    i = 0
    while i < n:
        if not loud[i]:
            i += 1
            continue
        j = i
        while j < n:
            if loud[j]:
                j += 1
                continue
            k = j
            while k < n and not loud[k]:
                k += 1
            if (k - j) * AUDIO_WINDOW_S < min_silence_s and k < n:
                j = k
                continue
            break
        segs.append({"start_s": round(i * AUDIO_WINDOW_S, 3), "end_s": round(j * AUDIO_WINDOW_S, 3)})
        i = j
    return segs


def border_events(levels):
    """[(pts, level)] から、0.5 を上へ・下へ横切った時刻と、消え始め (0.95 を下回った)・消えきった (0.05 を下回った)。"""
    events = []
    prev = None
    for pts, lv in levels:
        if lv is None:
            continue
        if prev is not None:
            p_pts, p_lv = prev
            for name, th, up in (("rise", LEVEL_ON, True), ("fall", LEVEL_ON, False), ("full", LEVEL_FULL, True),
                                 ("fadeStart", LEVEL_FULL, False), ("off", LEVEL_OFF, False), ("start", LEVEL_OFF, True)):
                if up and p_lv < th <= lv or (not up) and p_lv >= th > lv:
                    events.append({"type": name, "t": round(pts, 3)})
        prev = (pts, lv)
    return events


def pair_voices(voices, events):
    pairs = []
    for i, v in enumerate(voices):
        nxt = voices[i + 1]["start_s"] if i + 1 < len(voices) else math.inf
        after = lambda typ, t0, t1=math.inf: next((e["t"] for e in events if e["type"] == typ and t0 <= e["t"] < t1), None)  # noqa: E731
        start = after("start", v["start_s"] - 0.2, nxt)
        rise = after("rise", v["start_s"] - 0.2, nxt)
        fade = after("fadeStart", v["end_s"], nxt)
        off = after("off", fade if fade is not None else v["end_s"], nxt + 2.0)
        pairs.append({
            "voice": v,
            "borderStartS": start,
            "borderRiseS": rise,
            "fadeStartS": fade,
            "offS": off,
            "onDelayS": round(rise - v["start_s"], 3) if rise is not None else None,
            "holdS": round(fade - v["end_s"], 3) if fade is not None else None,
            "fadeS": round(off - fade, 3) if (off is not None and fade is not None) else None,
        })
    return pairs


def flips_max_per_second(events):
    times = sorted(e["t"] for e in events if e["type"] in ("rise", "fall"))
    best = 0
    j = 0
    for i in range(len(times)):
        while times[i] - times[j] > 1.0:
            j += 1
        best = max(best, i - j + 1)
    return best


def measure(path, region=None, color=DEFAULT_COLOR, strip=12, ref_at_s=0.0, ffmpeg="ffmpeg", ffprobe="ffprobe",
            voice_dbfs=DEFAULT_VOICE_DBFS, min_silence_s=DEFAULT_MIN_SILENCE_S):
    info = video_frames.probe_video(path, ffprobe=ffprobe)
    fw, fh = info["width"], info["height"]
    reg = video_frames.clamp_region(*(region or (0, 0, 0, 0)), fw, fh)
    if reg is None:
        raise ValueError("region is outside the video")
    x, y, w, h = reg
    target = parse_color(color)
    mask = strip_mask(h, w, strip)
    ref = None
    levels = []
    for pts, rgb in video_frames.iter_frames(path, fw, fh, region=(x, y, w, h), ffmpeg=ffmpeg, pix_fmt="rgb24"):
        if ref is None and pts + 1e-6 >= ref_at_s:
            ref = rgb.copy()
        levels.append((pts, frame_level(rgb, ref, target, mask) if ref is not None else None))
    samples = video_frames.read_mono_pcm(path, ffmpeg=ffmpeg)
    voices = voice_segments(samples, 48000, voice_dbfs, min_silence_s)
    events = border_events(levels)
    return {
        "file": path,
        "region": [x, y, w, h],
        "color": color,
        "strip": strip,
        "refAtS": ref_at_s,
        "frames": len(levels),
        "fps": video_frames.fps_from_pts([p for p, _ in levels]),
        "voices": voices,
        "border": {"events": events, "levelMax": max((lv for _, lv in levels if lv is not None), default=None)},
        "pairs": pair_voices(voices, events),
        "flipsMaxPerSecond": flips_max_per_second(events),
    }


# ---- セルフテスト ---------------------------------------------------------------


def _make_selftest_video(ffmpeg, out_path):
    """640x360 30 fps 4 秒。地は 0x303030 (テスト用の送信側の地と同じ灰色)、枠線は 8 px の 0x57F287 を 1.2-2.5 秒に全部、
    2.5-2.7 秒に半分の明るさ (地と半々の色) で描く (消えかけの代わり)。音は 440 Hz を 1.0-2.0 秒だけ鳴らす。"""
    half = "0x%02X%02X%02X" % tuple(int(round((c + 0x30) / 2)) for c in (0x57, 0xF2, 0x87))
    vf = ("drawbox=x=0:y=0:w=iw:h=ih:color=0x57F287:t=8:enable='between(t,1.2,2.5)',"
          "drawbox=x=0:y=0:w=iw:h=ih:color=" + half + ":t=8:enable='between(t,2.5001,2.7)'")
    cmd = [ffmpeg, "-hide_banner", "-nostats", "-loglevel", "error", "-y",
           "-f", "lavfi", "-i", "color=c=0x303030:s=640x360:r=30:d=4",
           "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=4",
           "-vf", vf, "-af", "volume='if(between(t,1,2),0.5,0)':eval=frame",
           "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "12", "-c:a", "aac", "-b:a", "128k", "-shortest", out_path]
    subprocess.run(cmd, check=True)


# セルフテストの保存先が条件に合うか。合えば None、合わなければ理由。保存先は呼び出し元の TEMP
# (tempfile.gettempdir() = 環境変数 TEMP / TMP) の下で、リポジトリの _work の下ではないこと。_work は実機テストの
# 作業用フォルダー (実行の準備が中身を保存し、実行スクリプトの後始末が消す場所) で、セルフテストがそこに書くと
# 実行の前後の比較に混じる。保存先は呼び出し元が TEMP で決める (ローカルでは build_x64 の下を TEMP に渡す。CI の ubuntu は /tmp)。
# build_x64 の決まった場所にすると、CI とローカルで保存先が違ってしまう。
def selftest_place_problem(path, temp_dir, repo_root):
    full = os.path.normcase(os.path.abspath(path))
    temp_full = os.path.normcase(os.path.abspath(temp_dir))
    work_full = os.path.normcase(os.path.abspath(os.path.join(repo_root, "_work")))
    if full != temp_full and not full.startswith(temp_full.rstrip(os.sep) + os.sep):
        return "not under the caller's TEMP (" + temp_dir + ")"
    if full == work_full or full.startswith(work_full + os.sep):
        return "under _work (the live-run work folder)"
    return None


def selftest(ffmpeg="ffmpeg", ffprobe="ffprobe"):
    failures = []

    def check(cond, label):
        print(("ok   " if cond else "FAIL ") + label)
        if not cond:
            failures.append(label)

    # 純関数の部分 (ファイルなし)。
    ev = border_events([(0.0, 0.0), (0.1, 0.0), (0.2, 1.0), (0.3, 1.0), (0.4, 0.6), (0.5, 0.3), (0.6, 0.0)])
    types = [e["type"] for e in ev]
    check(types == ["rise", "full", "start", "fadeStart", "fall", "off"], "levels give rise/full then fadeStart/fall/off (%s)" % types)
    flips = flips_max_per_second([{"type": "rise", "t": 0.0}, {"type": "fall", "t": 0.3}, {"type": "rise", "t": 0.6}, {"type": "fall", "t": 0.9}])
    check(flips == 4, "four flips inside one second are counted as 4")
    sr = 48000
    tone = np.zeros(sr * 3, np.float32)
    tone[sr // 2 : sr] = 0.3
    tone[sr + sr // 20 : 2 * sr] = 0.3  # 0.05 秒の切れ目はつなぐ
    segs = voice_segments(tone, sr, DEFAULT_VOICE_DBFS, DEFAULT_MIN_SILENCE_S)
    check(len(segs) == 1 and abs(segs[0]["start_s"] - 0.5) < 0.011 and abs(segs[0]["end_s"] - 2.0) < 0.011,
          "a 50 ms gap is bridged into one voice 0.5-2.0 s (%s)" % segs)

    # 作り物の動画 (ffmpeg が要る)。保存先は呼び出し元の TEMP の下 (上の selftest_place_problem のコメント)。
    repo_root = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
    temp_dir = tempfile.gettempdir()
    tmp = tempfile.mkdtemp(prefix="border-selftest-")
    print("selftest work: " + tmp)
    check(selftest_place_problem(tmp, temp_dir, repo_root) is None, "the work folder is under the caller's TEMP and outside _work")
    check(selftest_place_problem(os.path.join(repo_root, "_work", "border-selftest-x"), temp_dir, repo_root) is not None,
          "the old place under _work is refused")
    check(selftest_place_problem(os.path.join(temp_dir + "2", "x"), temp_dir, repo_root) is not None,
          "a sibling folder whose name starts with TEMP is not under TEMP")
    try:
        video = os.path.join(tmp, "border.mkv")
        try:
            _make_selftest_video(ffmpeg, video)
        except (OSError, subprocess.CalledProcessError) as e:
            check(False, "make the synthetic video with ffmpeg (%s)" % e)
            video = None
        if video:
            r = measure(video, region=(0, 0, 640, 360), strip=12, ffmpeg=ffmpeg, ffprobe=ffprobe)
            check(len(r["voices"]) == 1 and abs(r["voices"][0]["start_s"] - 1.0) < 0.03 and abs(r["voices"][0]["end_s"] - 2.0) < 0.06,
                  "the voice is found at 1.0-2.0 s (%s)" % r["voices"])
            p = r["pairs"][0] if r["pairs"] else {}
            frame = 1.0 / 30.0
            check(p.get("onDelayS") is not None and abs(p["onDelayS"] - 0.2) <= frame + 0.01, "the border rises 0.2 s after the voice (%s)" % p.get("onDelayS"))
            check(p.get("holdS") is not None and abs(p["holdS"] - 0.5) <= frame + 0.06, "the border holds 0.5 s after the voice ends (%s)" % p.get("holdS"))
            check(p.get("fadeS") is not None and abs(p["fadeS"] - 0.2) <= frame + 0.01, "the half-bright step lasts 0.2 s before it is off (%s)" % p.get("fadeS"))
            check(r["flipsMaxPerSecond"] <= 2, "one rise and one fall only (%s)" % r["flipsMaxPerSecond"])
            check(r["border"]["levelMax"] is not None and r["border"]["levelMax"] > 0.8, "the full border reads close to 1 (%s)" % r["border"]["levelMax"])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    check(not os.path.exists(tmp), "the work folder is removed")
    print("selftest: %d failure(s)" % len(failures))
    return 1 if failures else 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("video", nargs="?")
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--region", default=None, help="x,y,w,h of the person's tile in the recording")
    parser.add_argument("--color", default=DEFAULT_COLOR)
    parser.add_argument("--strip", type=int, default=12, help="width in pixels of the band inside the tile edge")
    parser.add_argument("--ref-at-s", type=float, default=0.0, help="a time when the border is off (the reference colors)")
    parser.add_argument("--voice-dbfs", type=float, default=DEFAULT_VOICE_DBFS)
    parser.add_argument("--min-silence-s", type=float, default=DEFAULT_MIN_SILENCE_S)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    args = parser.parse_args(argv[1:])
    if args.selftest:
        return selftest(args.ffmpeg, args.ffprobe)
    if not args.video:
        parser.error("give a recording or --selftest")
    region = tuple(int(v) for v in args.region.split(",")) if args.region else None
    out = measure(args.video, region, args.color, args.strip, args.ref_at_s, args.ffmpeg, args.ffprobe, args.voice_dbfs, args.min_silence_s)
    print(json.dumps(out, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
