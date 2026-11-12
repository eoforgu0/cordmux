# 録画の判定スクリプト (2/3): 黒の連続区間と無音の連続区間を拾い、録画ログの切れ目と一致するか見る。
# 対の判定として、cordmux の mov の最初の実フレームより前に黒がないことと、同じ時刻の OBS 側の録画に
# 黒がないことを見る。共有をやめた瞬間に黒が 1 秒だけ配信に出る不具合の検知にも使う。
#
# frames.py は録画の判定に使わない。frames.py は左上のセルの番号を読むツールなので、黒の区間は
# セルが全部 0 に読めて「逆行」に見える。このツールは番号を読まず、
# 「黒いフレームの番号の並び」と「0 のサンプルの並び」だけを見る。
#
# 使い方:
#   python black_span.py <動画> [--mode spans|no-black|no-leading-black|match-gaps]
#                        [--ffmpeg <場所>] [--ffprobe <場所>]
#                        [--grid 2x2 --cell <行>,<列>] [--crop W:H:X:Y]
#                        [--from-frame N] [--to-frame N] [--min-frames N]
#                        [--audio <音のファイル>] [--gaps <欠落の一覧の JSON>]
#                        [--allow-leading N]
#   python black_span.py --selftest [--ffmpeg <場所>] [--outdir <テストデータの保存先>] [--keep]
#
# mode の意味:
#   spans             拾うだけ (ok は常に true)。実行結果に残す用。
#   no-black          その範囲に黒の区間が 1 つもなければ GREEN (OBS 側の録画に使う)。
#   no-leading-black  最初のフレームから黒が続いていなければ GREEN (cordmux の mov に使う。
#                     最初の実フレームより前に黒が出ていないことを見る)。
#                     --allow-leading N を渡すと、先頭の黒が N フレーム以内 (許容 1 フレーム) なら合格。
#                     N は録画ログの先頭の欠落 (start_frame 0 の欠落) の長さ。
#   match-gaps        --gaps の一覧と黒の区間が 1 対 1 で合えば GREEN。
#
# 結果は 1 つの JSON を標準出力に書く。ok が true なら終了コード 0、false なら 1。
#
# ---- 判断 ----------------------------------------------------------
# (1) 黒の拾い方は blackframe (フレーム番号) で、blackdetect (秒) は使わない。判定を整数で
#     行うには、秒の小数ではなくフレーム番号が要る。blackframe が
#     出すのは「黒いフレームの番号」だけなので、総フレーム数は ffprobe -count_frames で別に数え、
#     出てこなかった番号を実フレームとみなす。
# (2) 黒のしきい値は amount=98 / threshold=32 (blackframe の既定 98/32 と同じ)。黒埋めの
#     テストデータは真っ黒の H.264 なので既定で足り、値を下げると実映像の暗い場面を黒と誤る。
# (3) 黒とみなす最小の連続フレーム数は 2 (既定)。1 フレームだけの黒は場面の切り替わりでも出る。
#     黒埋めのテストデータは 1 秒ぶんなので、2 フレームの下限で取りこぼさない。
# (4) 無音は f32 にデコードしてサンプルをそのまま見る。録画の PCM は受信したまま f32 で、
#     欠落は 0 で埋めるので、しきい値は既定 0.0 (完全な 0 だけを無音とする)。
#     --silence-threshold で緩められる。
# (5) 無音とみなす最小の連続サンプル数は 0.1 秒ぶん (既定)。10 ms のブロック 1 つぶんの取り落とし
#     を無音の区間と呼ばないため。
# (6) match-gaps の許容は前後 1 フレーム。区間の先頭の pts は 0 に揃えるが、
#     timescale 60000 の刻みを 60 fps のフレームへ丸めるところで 1 刻みずれうる。
# (7) no-leading-black は、録画ログに先頭の欠落があればその長さまで黒を許す (--allow-leading)。
#     セッションの原点は全区間の先頭の最小値なので、遅れて始まった人の完成の頭は
#     正しく黒で埋まる (例: 録画ログの gaps[0] が start_us 0 の 10 フレームなら、
#     完成の頭の黒も 10 フレーム)。旗の配線が働かず黒が配信に漏れる不具合は、
#     OBS 側の no-black が見ている。ここで RED にするのは「録画ログに先頭の欠落がない
#     (または短い) のに頭が黒」の場合だけ。許容は (6) と同じ 1 フレーム。--allow-leading を渡さない
#     (0) ときは 1 フレームの黒も RED。
import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

BLACKFRAME_RE = re.compile(r"blackframe.*?\bframe:(\d+)\b.*?\bpblack:(\d+)\b")

DEFAULT_MIN_BLACK_FRAMES = 2
DEFAULT_SILENCE_THRESHOLD = 0.0
DEFAULT_MIN_SILENCE_RATIO = 0.1  # 秒
MATCH_GAPS_TOLERANCE_FRAMES = 1


def resolve_tools(ffmpeg, ffprobe):
    """ledger_check.py と同じ決め方 (--ffprobe が無ければ --ffmpeg の隣を見る)。"""
    ff = ffmpeg or "ffmpeg"
    if ffprobe:
        return ff, ffprobe
    if ffmpeg:
        d = os.path.dirname(os.path.abspath(ffmpeg))
        base = os.path.basename(ffmpeg)
        probe = os.path.join(d, base.replace("ffmpeg", "ffprobe", 1))
        if os.path.exists(probe):
            return ff, probe
    return ff, "ffprobe"


def probe_video(ffprobe, path):
    cmd = [ffprobe, "-v", "error", "-select_streams", "v:0", "-count_frames",
           "-show_entries", "stream=width,height,nb_read_frames,r_frame_rate,time_base",
           "-of", "json", path]
    r = subprocess.run(cmd, capture_output=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise RuntimeError("ffprobe が失敗した: " + (r.stderr or "").strip()[:400])
    streams = json.loads(r.stdout).get("streams", [])
    if not streams:
        raise RuntimeError("映像のトラックがない: " + path)
    return streams[0]


def probe_audio(ffprobe, path):
    cmd = [ffprobe, "-v", "error", "-select_streams", "a:0",
           "-show_entries", "stream=sample_rate,channels", "-of", "json", path]
    r = subprocess.run(cmd, capture_output=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise RuntimeError("ffprobe が失敗した: " + (r.stderr or "").strip()[:400])
    streams = json.loads(r.stdout).get("streams", [])
    if not streams:
        return None
    return streams[0]


def crop_for_cell(width, height, grid, cell):
    """2x2 などのセルの 1 つを切り出す crop の引数を作る。"""
    rows, cols = grid
    r, c = cell
    w = width // cols
    h = height // rows
    return str(w) + ":" + str(h) + ":" + str(c * w) + ":" + str(r * h)


def black_frame_numbers(ffmpeg, path, crop):
    """黒いフレームの番号の集合を返す。"""
    chain = []
    if crop:
        chain.append("crop=" + crop)
    chain.append("blackframe=amount=98:threshold=32")
    cmd = [ffmpeg, "-hide_banner", "-nostats", "-i", path, "-vf", ",".join(chain),
           "-an", "-f", "null", "-"]
    r = subprocess.run(cmd, capture_output=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise RuntimeError("ffmpeg (blackframe) が失敗した: " + (r.stderr or "").strip()[-400:])
    found = set()
    for line in (r.stderr or "").split("\n"):
        m = BLACKFRAME_RE.search(line)
        if m:
            found.add(int(m.group(1)))
    return found


def runs_from_numbers(numbers, min_len, lo, hi):
    """番号の集合から [lo, hi] の範囲の連続区間を作る。"""
    spans = []
    start = None
    prev = None
    for n in sorted(numbers):
        if n < lo or n > hi:
            continue
        if start is None:
            start = n
        elif n != prev + 1:
            spans.append((start, prev))
            start = n
        prev = n
    if start is not None:
        spans.append((start, prev))
    return [{"start_frame": a, "end_frame": b, "frames": b - a + 1}
            for a, b in spans if (b - a + 1) >= min_len]


def silence_spans(ffmpeg, path, threshold, min_samples):
    """無音の連続区間をサンプル数で返す。numpy を使う。"""
    import numpy as np

    cmd = [ffmpeg, "-hide_banner", "-loglevel", "error", "-i", path,
           "-map", "0:a:0", "-f", "f32le", "-acodec", "pcm_f32le", "-"]
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode != 0:
        raise RuntimeError("ffmpeg (音のデコード) が失敗した: " + r.stderr.decode("utf-8", "replace")[-400:])
    raw = np.frombuffer(r.stdout, dtype="<f4")
    return raw


def silence_runs(raw, channels, threshold, min_samples):
    import numpy as np

    if channels > 1:
        usable = (raw.size // channels) * channels
        frames = np.abs(raw[:usable].reshape(-1, channels)).max(axis=1)
    else:
        frames = np.abs(raw)
    quiet = frames <= threshold
    spans = []
    # 0/1 の並びの境目だけを見る (全サンプルを Python で回さない)。
    edges = np.flatnonzero(np.diff(quiet.astype(np.int8)))
    starts = []
    ends = []
    if quiet.size and quiet[0]:
        starts.append(0)
    for e in edges:
        if quiet[e + 1]:
            starts.append(int(e) + 1)
        else:
            ends.append(int(e))
    if quiet.size and quiet[-1]:
        ends.append(int(quiet.size) - 1)
    for a, b in zip(starts, ends):
        if (b - a + 1) >= min_samples:
            spans.append({"start_sample": a, "end_sample": b, "samples": b - a + 1})
    return spans, int(frames.size)


def first_nonblack_frame(black, total, start=0):
    """start から見て最初の実フレーム (黒でないフレーム) の番号。全部黒なら None。"""
    n = start
    while n < total and n in black:
        n += 1
    return n if n < total else None


def judge(mode, spans, first_nonblack, gaps, lo, hi, allow_leading=0):
    """mode ごとの合否と理由を返す。"""
    failed = []
    if mode == "spans":
        return True, failed
    if mode == "no-black":
        if spans:
            failed.append("範囲 [" + str(lo) + "," + str(hi) + "] に黒の区間が " + str(len(spans)) + " つある")
        return not failed, failed
    if mode == "no-leading-black":
        if first_nonblack is None:
            failed.append("実フレームが 1 つもない (全部黒)")
        else:
            leading = first_nonblack - lo
            # 録画ログに先頭の欠落が無ければ 1 フレームの黒も失敗。あればその長さ + 許容 1 フレームまで成功。
            limit = allow_leading + MATCH_GAPS_TOLERANCE_FRAMES if allow_leading > 0 else 0
            if leading > limit:
                failed.append("最初の実フレームの前に黒が " + str(leading) + " フレームある (録画ログの先頭の欠落 "
                              + str(allow_leading) + " フレーム)")
        return not failed, failed
    if mode == "match-gaps":
        want = sorted(gaps, key=lambda g: g["start_frame"])
        got = sorted(spans, key=lambda s: s["start_frame"])
        if len(want) != len(got):
            failed.append("欠落の数が合わない: 録画ログ " + str(len(want)) + " / 黒 " + str(len(got)))
            return False, failed
        for w, g in zip(want, got):
            if abs(w["start_frame"] - g["start_frame"]) > MATCH_GAPS_TOLERANCE_FRAMES:
                failed.append("欠落の始まりが合わない: 録画ログ " + str(w["start_frame"]) + " / 黒 " + str(g["start_frame"]))
            if abs(w["frames"] - g["frames"]) > MATCH_GAPS_TOLERANCE_FRAMES:
                failed.append("欠落の長さが合わない: 録画ログ " + str(w["frames"]) + " / 黒 " + str(g["frames"]))
        return not failed, failed
    failed.append("知らない mode: " + str(mode))
    return False, failed


def run_one(args, ffmpeg, ffprobe):
    info = probe_video(ffprobe, args.video)
    width = int(info["width"])
    height = int(info["height"])
    total = int(info["nb_read_frames"])
    crop = args.crop
    if not crop and args.cell:
        crop = crop_for_cell(width, height, args.grid, args.cell)
    black = black_frame_numbers(ffmpeg, args.video, crop)
    lo = args.from_frame if args.from_frame is not None else 0
    hi = args.to_frame if args.to_frame is not None else total - 1
    spans = runs_from_numbers(black, args.min_frames, lo, hi)
    # **範囲の始まり (lo) から見た**最初の実フレーム。--from-frame を渡したときに 0 から数え直すと
    # 範囲の外の黒を「先行する黒」と呼んでしまう。
    first_real = first_nonblack_frame(black, total, lo)

    result = {
        "video": args.video,
        "mode": args.mode,
        "width": width,
        "height": height,
        "crop": crop,
        "total_frames": total,
        "range": {"from_frame": lo, "to_frame": hi},
        "black_spans": spans,
        "first_nonblack_frame": first_real,
    }

    if args.audio:
        audio_info = probe_audio(ffprobe, args.audio)
        if audio_info is None:
            result["silence"] = {"ok": False, "error": "音のトラックがない: " + args.audio}
        else:
            rate = int(audio_info["sample_rate"])
            channels = int(audio_info["channels"])
            raw = silence_spans(ffmpeg, args.audio, args.silence_threshold, 0)
            min_samples = args.min_silence_samples
            if min_samples is None:
                min_samples = int(rate * DEFAULT_MIN_SILENCE_RATIO)
            runs, frame_count = silence_runs(raw, channels, args.silence_threshold, min_samples)
            result["silence"] = {"sample_rate": rate, "channels": channels,
                                 "total_samples": frame_count, "spans": runs}

    gaps = []
    if args.gaps:
        gaps = json.loads(pathlib.Path(args.gaps).read_text(encoding="utf-8"))
    allow_leading = max(0, int(getattr(args, "allow_leading", 0) or 0))
    result["allow_leading"] = allow_leading
    ok, failed = judge(args.mode, spans, first_real, gaps, lo, hi, allow_leading)
    result["ok"] = ok
    result["failed"] = failed
    return result


# ---- --selftest --------------------------------------------------------------
# ffmpeg だけでテストデータを作り、正常が合格、わざと壊した 3 通りが失敗になることを確認する。
#   normal        頭が実映像、途中に黒 1 秒 -> no-leading-black は合格、match-gaps も成功
#   leading       頭に黒 0.5 秒 -> no-leading-black が失敗 (最初の実フレームより前の黒の検知)
#                 録画ログに先頭の欠落 30 フレームがある (--allow-leading 30) -> 成功
#                 録画ログの先頭の欠落が短い (--allow-leading 10) -> 失敗
#   extra_black   途中の黒を 2 つにする -> match-gaps が失敗 (欠落の数が合わない)
#   no_black_bad  黒のあるテストデータに no-black を当てる -> 失敗 (OBS 側の録画に黒があった場合)

SELFTEST_FPS = 60
SELFTEST_SIZE = "320x240"


def run_ffmpeg(ffmpeg, args):
    cmd = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y"] + args
    r = subprocess.run(cmd, capture_output=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise RuntimeError("ffmpeg が失敗した: " + " ".join(cmd) + "\n" + (r.stderr or "")[:800])


def make_material(ffmpeg, path, parts):
    """parts は ("real"|"black", 秒) の並び。つないで 1 本の mov にする。"""
    inputs = []
    labels = []
    for i, (kind, seconds) in enumerate(parts):
        src = ("testsrc=size=" + SELFTEST_SIZE if kind == "real" else "color=c=black:size=" + SELFTEST_SIZE)
        inputs += ["-f", "lavfi", "-i", src + ":rate=" + str(SELFTEST_FPS) + ":duration=" + str(seconds)]
        labels.append("[" + str(i) + ":v]")
    graph = "".join(labels) + "concat=n=" + str(len(parts)) + ":v=1[v]"
    run_ffmpeg(ffmpeg, inputs + ["-filter_complex", graph, "-map", "[v]",
                                 "-c:v", "libx264", "-g", str(SELFTEST_FPS), "-pix_fmt", "yuv420p",
                                 "-video_track_timescale", "60000", path])


def make_audio(ffmpeg, path, parts):
    """parts は ("tone"|"silence", 秒) の並び。f32 の PCM の mov にする。"""
    inputs = []
    labels = []
    for i, (kind, seconds) in enumerate(parts):
        src = ("sine=frequency=1000:sample_rate=48000" if kind == "tone"
               else "anullsrc=r=48000:cl=stereo")
        inputs += ["-f", "lavfi", "-t", str(seconds), "-i", src]
        labels.append("[" + str(i) + ":a]")
    graph = "".join(labels) + "concat=n=" + str(len(parts)) + ":v=0:a=1[a]"
    run_ffmpeg(ffmpeg, inputs + ["-filter_complex", graph, "-map", "[a]",
                                 "-ac", "2", "-c:a", "pcm_f32le",
                                 "-movflags", "+frag_keyframe+empty_moov+default_base_moof",
                                 "-frag_duration", "1000000", path])


class SelftestArgs:
    """run_one に渡す引数の束 (argparse の Namespace の代わり)。"""

    def __init__(self, video, mode, gaps=None, audio=None, allow_leading=0):
        self.video = video
        self.mode = mode
        self.allow_leading = allow_leading
        self.crop = None
        self.cell = None
        self.grid = (2, 2)
        self.from_frame = None
        self.to_frame = None
        self.min_frames = DEFAULT_MIN_BLACK_FRAMES
        self.audio = audio
        self.gaps = gaps
        self.silence_threshold = DEFAULT_SILENCE_THRESHOLD
        self.min_silence_samples = None


def selftest(ffmpeg, ffprobe, outdir, keep):
    base = pathlib.Path(outdir) / "black-span"
    if base.exists():
        shutil.rmtree(str(base), ignore_errors=True)
    base.mkdir(parents=True, exist_ok=True)
    results = []
    all_ok = True
    try:
        # 実 1 秒 + 黒 1 秒 + 実 1 秒。黒は 60 フレーム目から 60 フレーム。
        normal = str(base / "normal.mov")
        make_material(ffmpeg, normal, [("real", 1), ("black", 1), ("real", 1)])
        leading = str(base / "leading.mov")
        make_material(ffmpeg, leading, [("black", 0.5), ("real", 1), ("black", 1), ("real", 1)])
        extra = str(base / "extra.mov")
        make_material(ffmpeg, extra, [("real", 1), ("black", 1), ("real", 1), ("black", 1), ("real", 1)])
        audio_ok = str(base / "audio.mov")
        make_audio(ffmpeg, audio_ok, [("tone", 1), ("silence", 1), ("tone", 1)])

        gaps_path = str(base / "gaps.json")
        pathlib.Path(gaps_path).write_text(
            json.dumps([{"start_frame": 60, "frames": 60}]) + "\n", encoding="utf-8", newline="\n")

        cases = [
            ("normal_no_leading", "頭が実映像 -> no-leading-black", SelftestArgs(normal, "no-leading-black"), "GREEN"),
            ("normal_match_gaps", "途中の黒 1 つ -> match-gaps", SelftestArgs(normal, "match-gaps", gaps_path), "GREEN"),
            ("leading_black", "頭に黒 -> no-leading-black が失敗", SelftestArgs(leading, "no-leading-black"), "RED"),
            # 頭の黒は 0.5 秒 = 30 フレーム。録画ログに同じ長さの先頭の欠落があれば合格、短ければ失敗。
            ("leading_in_ledger", "録画ログに先頭の欠落 30 フレーム -> no-leading-black が合格",
             SelftestArgs(leading, "no-leading-black", allow_leading=30), "GREEN"),
            ("leading_over_ledger", "録画ログの先頭の欠落 10 フレームより長い黒 -> no-leading-black が失敗",
             SelftestArgs(leading, "no-leading-black", allow_leading=10), "RED"),
            ("extra_black", "黒が 2 つ -> match-gaps が失敗", SelftestArgs(extra, "match-gaps", gaps_path), "RED"),
            ("black_in_obs", "黒のあるテストデータに no-black -> 失敗", SelftestArgs(normal, "no-black"), "RED"),
            ("silence_found", "無音 1 秒を拾えるか", SelftestArgs(normal, "spans", None, audio_ok), "GREEN"),
        ]
        for name, label, sargs, want in cases:
            try:
                r = run_one(sargs, ffmpeg, ffprobe)
                got = "GREEN" if r["ok"] else "RED"
                detail = r["failed"][:3]
                if name == "silence_found":
                    spans = r.get("silence", {}).get("spans", [])
                    # 1 秒 (48000 サンプル) の無音がちょうど 1 つ拾えていれば合格。
                    good = len(spans) == 1 and abs(spans[0]["samples"] - 48000) <= 2
                    got = "GREEN" if good else "RED"
                    detail = spans[:3]
            except Exception as e:  # noqa: BLE001
                got = "ERROR"
                detail = [str(e)[:300]]
            ok = got == want
            all_ok = all_ok and ok
            results.append({"case": name, "label": label, "want": want, "got": got, "ok": ok, "detail": detail})
    finally:
        if not keep:
            shutil.rmtree(str(base), ignore_errors=True)
    return {"ok": all_ok, "selftest": results}


def parse_grid(text):
    a, b = text.lower().split("x", 1)
    return int(a), int(b)


def parse_cell(text):
    a, b = text.split(",", 1)
    return int(a), int(b)


def main():
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument("video", nargs="?", default=None)
    parser.add_argument("--mode", default="spans",
                        choices=["spans", "no-black", "no-leading-black", "match-gaps"])
    parser.add_argument("--ffmpeg", default=None)
    parser.add_argument("--ffprobe", default=None)
    parser.add_argument("--grid", default="2x2", help="セルの割り方 (--cell と組で使う)")
    parser.add_argument("--cell", default=None, help="<行>,<列> (0 始まり)")
    parser.add_argument("--crop", default=None, help="W:H:X:Y (--grid/--cell より優先)")
    parser.add_argument("--from-frame", type=int, default=None)
    parser.add_argument("--to-frame", type=int, default=None)
    parser.add_argument("--min-frames", type=int, default=DEFAULT_MIN_BLACK_FRAMES)
    parser.add_argument("--audio", default=None, help="無音の区間も見る音のファイル")
    parser.add_argument("--silence-threshold", type=float, default=DEFAULT_SILENCE_THRESHOLD)
    parser.add_argument("--min-silence-samples", type=int, default=None)
    parser.add_argument("--gaps", default=None, help="match-gaps が読む欠落の一覧 (JSON)")
    parser.add_argument("--allow-leading", type=int, default=0,
                        help="no-leading-black が許す先頭の黒のフレーム数 (録画ログの先頭の欠落の長さ)")
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--outdir", default=None)
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()

    ffmpeg, ffprobe = resolve_tools(args.ffmpeg, args.ffprobe)
    if args.selftest:
        here = pathlib.Path(__file__).resolve().parent
        outdir = args.outdir or str(here.parents[2] / "build_x64" / "selfcheck")
        os.makedirs(outdir, exist_ok=True)
        result = selftest(ffmpeg, ffprobe, outdir, args.keep)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        sys.exit(0 if result["ok"] else 1)

    if not args.video:
        print(json.dumps({"ok": False, "error": "動画を渡すか --selftest を付ける"}, ensure_ascii=False))
        sys.exit(1)
    args.grid = parse_grid(args.grid)
    args.cell = parse_cell(args.cell) if args.cell else None
    try:
        result = run_one(args, ffmpeg, ffprobe)
    except Exception as e:  # noqa: BLE001 (失敗を握って結果に書く)
        result = {"ok": False, "error": str(e)[:600]}
    print(json.dumps(result, ensure_ascii=False, indent=2))
    sys.exit(0 if result.get("ok") else 1)


if __name__ == "__main__":
    main()
