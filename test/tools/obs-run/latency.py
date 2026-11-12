#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# cordmux が足す遅れ (Discord 自身の再生に対して、OBS の出力がどれだけ遅れるか) を、OBS の 1 ストリームの録画の
# 2 つの領域に写った「時計つきの帯」(bit_strip.py) の番号の差から読む。
#
# 測り方の全体: 同じ 1 つの録画に、合成のソース cordmux と、Discord のウィンドウを画面キャプチャしたものを並べて撮り、
# 同じ変化が現れるフレームの差を読む。実際の通話で測る。テスト用の OBS の空いた所に Vesktop のウィンドウの
# ウィンドウキャプチャを置き、配信者の共有 (テストパターン = share/pattern.html) を、合成のソース cordmux の中の共有と
# Vesktop のウィンドウの両方で読む。目標は 90 ms (許容限) / 45 ms (検知限)。超えても実行は止めず、設計上の問いとして記録する。
# ウィンドウキャプチャ自身の遅れ (1 フレーム前後) は偏りとして結果に書く。
#
# 帯の番号の意味: 送信側のフレーム番号ではなく時刻 (1/60 秒が 1)。
#   - share/pattern.html: floor(ページを開いてからの ms x 60 / 1000)。描いた回数ではなく経過時間で決まるので、画面の更新が
#     144 Hz でも、共有が 30 fps や不規則でも 1 秒に 60 進む (共有のフレームが間引かれると番号が飛ぶだけ)。
#   - make_sync_video.py の sync-video.mp4: 60 fps の動画のフレームの番号 (再生している間は 動画の中の時刻 x 60)。10 分で繰り返す。
#   どちらも 24 ビットで回る。帯は fps を持たないので、番号の差を ms にする係数は --band-fps (既定 60) で受ける。帯から測った
#   進み方 (録画の pts の 1 秒あたり) は確認として出し、--band-fps と 5% を超えて違えば warnings に書く。
#
# 測り方: 録画を 1 回だけデコードし (2 つの領域を囲む四角を ffmpeg の側で切り出す。video_frames.py)、出力のフレームごとに領域 A
# (合成のソース cordmux の中の共有) と領域 B (Discord のウィンドウのキャプチャ) の帯をそれぞれ領域の中で探して読む。同じ出力のフレームの中で両方を
# 読めたら 1 組とし、「A の番号 - B の番号」を ms にする (出力のフレームどうしの時刻の差ではない)。cordmux が遅いと A は古い
# 絵 = 小さい番号なので、この値は負になる。判定に使う cordmux_delay_ms は符号を返した B - A (正 = cordmux の出力が
# Discord 自身の再生より遅い)。OBS の描画の 1 フレームは両方の領域に同じだけ乗るので差には入らない。
#
# 設計の理由:
#   - 判定は cordmux_delay_ms の平均で行う。1 組の差は整数の番号の差で、両方の領域は同じ共有のフレーム (30 fps なら
#     33 ms おき) を時刻をずらして出すので、差は共有のフレームの間隔の倍数に寄る (遅れが 50 ms なら 33 ms と 67 ms が半々)。
#     中央値と百分位はその刻みの値のどれかに張り付き、平均だけが刻みの間の値を出す (出力のフレームと共有のフレームの位相は一様に
#     回るので、長い区間の平均は遅れそのものになる)。中央値・5 と 95 の百分位・最小・最大も出す。中央値で判定すると、
#     刻みの片方に張り付き、50 ms を 33 ms とも 67 ms とも読む。
#   - 1 回のデコードで 2 つの領域を読む。領域ごとに 2 回デコードすると、デコードが倍になり、2 回のフレームの数と pts が
#     食い違ったときの合わせ方が要る。領域 A と B が対角の象限にあると、切り出す四角は画面の全部になる (1080p で
#     1 フレーム 2 MB を流す)。video_frames.py は四角 1 つしか切り出さない。
#   - 白のマーカー (sync.py の形) ではなく帯を使う。白のマーカーは 5 秒に 1 回で、マーカーごとに 1 フレームの粒度しかない。
#     帯は毎フレーム読めるので、20 分で数万の組になる。
#   - 領域 B に帯が複数あることがある。配信者の共有と同じウィンドウを、ほかの人と Vesktop 自身も共有するので、
#     Vesktop のウィンドウには同じ番号の帯が経路の違う共有ごとに写りうる (自分の共有のプレビュー、ほかの人の共有の小さな枠)。
#     探すと大きい帯から見つかる (bit_strip.find_strip) が、大きい帯が一時的に隠れると小さい帯に移って留まる。そこで
#     (1) RESEARCH_EVERY_FRAMES ごとと、CRC の合わないフレームが CRC_RUN_RESEARCH 続いたときに探し直し、現在の帯のセルの
#     SMALL_BAND_RATIO 倍以上の帯が別の所にあればそちらへ移る、(2) 読めたフレームのセルの幅の基準 (いちばん大きい帯。
#     REFERENCE_MIN_FRAMES フレーム以上あるもの) の SMALL_BAND_RATIO 倍より小さい帯で読んだフレームは組にしない (small)。
#     配信者の共有を大きく表示することが前提になる。経路の違う同じ大きさの共有 (並べた表示) は見分けられない
#     ので、領域の帯の位置の移り (geometry) と、同じ帯の測り直しの揺れをまとめた所の数 (places) を出し、領域 B の所が
#     2 つ以上なら warnings に書く。
#   - ありえない差 (|A - B| が --max-abs-ms を超える) は組にせず implausible に数える (テスト動画の繰り返しの境目、関係の
#     ない帯)。数と、その差の中央値を出す (本当に数秒遅れているのか、関係のない帯なのかを読む人が分けられるように)。
#   - ウィンドウキャプチャの遅れは引かない。領域 B は Discord のウィンドウが画面に出てから OBS に届くまでに約 1 フレーム
#     遅れる見込み (測っていない) で、その分 cordmux_delay_ms は本当の遅れより小さく出る。偏りとして capture_bias に
#     書き、判定には足した場合の段 (with_capture_bias) を並べるだけにする。
#   - 判定で止めない。結果を JSON で出すときの終了コードは、目標を超えても、読めなくても 0 (run-window.mjs の runJudge は
#     execFileSync なので、0 以外だと JSON が結果に残らない。frames.py と同じ)。引数の誤りだけが 2。
#
# しきい値の初期値は実機の結果を見て見直す: RESEARCH_EVERY_FRAMES 120 (60 fps で 2 秒)、CRC_RUN_RESEARCH 3、SMALL_BAND_RATIO 0.8
# (縮めた小さな枠は大きな表示の半分より小さい見込み。同じ帯の測りの揺れ (セルの 0.2 px) よりずっと大きい)、REFERENCE_MIN_FRAMES
# 30 (0.5 秒)、--max-abs-ms 2000、--min-pairs 60 (1 秒)、進み方の警告 5%。
#
# 使い方:
#   python latency.py REC.mkv --region-a x,y,w,h --region-b x,y,w,h [--band-fps 60] [--target-ms 90] [--detect-ms 45]
#                     [--max-abs-ms 2000] [--min-pairs 60] [--max-seconds S] [--ffmpeg PATH] [--ffprobe PATH]
#   python latency.py --selftest [--ffmpeg PATH] [--outdir DIR] [--keep]
#   領域は録画の座標 (テスト用の OBS の出力 = 1920x1080 の上の位置と大きさ)。--max-seconds は録画の先頭から S 秒で読むのを
#   やめる (デコードもそこで止める)。ffmpeg は --ffmpeg、なければ同梱の obs-deps (src-obs/buildspec.json のバージョンから導く。
#   run-window.mjs の OBS_DEPS_BIN と同じ)、それもなければ PATH の ffmpeg。ffprobe は --ffprobe、なければ ffmpeg の隣、
#   なければ PATH。
# 出力: 1 つの JSON (キーは main の出力を参照)。
import argparse
import bisect
import json
import os
import pathlib
import subprocess
import sys
import tempfile

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))
import bit_strip  # noqa: E402
import video_frames  # noqa: E402

DEFAULT_BAND_FPS = 60.0   # share/pattern.html の FPS と make_sync_video.py の FPS
TARGET_MS = 90.0          # 許容限 (ITU-R BT.1359-1、音が先の側)
DETECT_MS = 45.0          # 検知限 (同上)
MAX_ABS_MS = 2000.0
MIN_PAIRS = 60
RESEARCH_EVERY_FRAMES = 120
CRC_RUN_RESEARCH = 3
SMALL_BAND_RATIO = 0.8
REFERENCE_MIN_FRAMES = 30
REFERENCE_TOLERANCE = 0.1
RATE_WARN_RATIO = 0.05
RATE_MIN_SPAN_S = 2.0
LOOP_BACK_S = 1.0         # これより大きく番号が戻ったら、別の区間 (テスト動画の繰り返し) とみなす
GEOMETRY_LIST_MAX = 20
HISTOGRAM_MAX = 15
BY_MINUTE_MAX = 240
COUNTER_MODULO = bit_strip.COUNTER_MODULO
HALF_MODULO = COUNTER_MODULO // 2
STATUSES = ("ok", "small", "crc", "lost")
# 同梱の FFmpeg の保存先を buildspec.json から導けないときの場所 (run-window.mjs の OBS_DEPS_BIN の既定と同じ)。
FALLBACK_DEPS_BIN = REPO_ROOT / "src-obs" / ".deps" / "obs-studio-31.1.1" / ".deps" / "obs-deps-2025-07-11-x64" / "bin"


# ---- ツールの場所 ------------------------------------------------------------------------------------------

def bundled_ffmpeg_bin(repo_root=REPO_ROOT):
    """同梱の FFmpeg (obs-deps) の bin。src-obs/buildspec.json の obs-studio と prebuilt のバージョンから導く
    (run-window-plan.mjs の obsDepsBinRelFromBuildspec と同じ形)。読めなければ既定の場所。フォルダーが無ければ None。"""
    bin_dir = FALLBACK_DEPS_BIN if repo_root == REPO_ROOT else None
    try:
        spec = json.loads((pathlib.Path(repo_root) / "src-obs" / "buildspec.json").read_text(encoding="utf-8"))
        deps = spec["dependencies"]
        obs_version = deps["obs-studio"]["version"]
        prebuilt = deps["prebuilt"]["version"]
        if isinstance(obs_version, str) and isinstance(prebuilt, str) and obs_version and prebuilt:
            bin_dir = (pathlib.Path(repo_root) / "src-obs" / ".deps" / ("obs-studio-" + obs_version) / ".deps"
                       / ("obs-deps-" + prebuilt + "-x64") / "bin")
    except (OSError, ValueError, KeyError, TypeError):
        pass
    return bin_dir if bin_dir is not None and bin_dir.is_dir() else None


def resolve_tools(ffmpeg=None, ffprobe=None):
    """実際に呼ぶ ffmpeg と ffprobe を決める (使い方の欄の順)。"""
    if not ffmpeg:
        exe = "ffmpeg.exe" if os.name == "nt" else "ffmpeg"
        bin_dir = bundled_ffmpeg_bin()
        ffmpeg = str(bin_dir / exe) if bin_dir is not None and (bin_dir / exe).is_file() else "ffmpeg"
    if not ffprobe:
        ffprobe = "ffprobe"
        if os.path.dirname(ffmpeg):
            sibling = os.path.join(os.path.dirname(os.path.abspath(ffmpeg)),
                                   os.path.basename(ffmpeg).replace("ffmpeg", "ffprobe", 1))
            if os.path.isfile(sibling):
                ffprobe = sibling
    return ffmpeg, ffprobe


# ---- 純関数 (セルフテストが直に当てる) ----------------------------------------------------------------------

def parse_region(text):
    """"x,y,w,h" (0 以上の整数、w と h は 1 以上) を組にする。"""
    parts = [p.strip() for p in str(text).split(",")]
    try:
        x, y, w, h = (int(p) for p in parts)
    except ValueError:
        raise argparse.ArgumentTypeError("a region is x,y,w,h with integers: " + str(text))
    if x < 0 or y < 0 or w <= 0 or h <= 0:
        raise argparse.ArgumentTypeError("a region needs x,y >= 0 and w,h > 0: " + str(text))
    return x, y, w, h


def signed_units(a, b):
    """a - b を 24 ビットの輪の上で -2^23 以上 2^23 未満に修正する (帯の番号は 24 ビットで回る)。"""
    return ((int(a) - int(b) + HALF_MODULO) % COUNTER_MODULO) - HALF_MODULO


def delay_level(delay_ms, detect_ms=DETECT_MS, target_ms=TARGET_MS):
    """遅延 (ms、正 = cordmux が遅い) の段階。検知限・許容限は「未満なら内」。"""
    if delay_ms is None:
        return "undecidable"
    if delay_ms >= target_ms:
        return "over_target"
    if delay_ms >= detect_ms:
        return "within_target"
    return "within_detect"


def describe(values):
    """並びの数・平均・中央値・5 と 95 の百分位・最小・最大 (0.1 で丸める)。空なら値は None。"""
    arr = np.asarray(values, dtype=np.float64)
    keys = ("mean", "median", "p5", "p95", "min", "max")
    if arr.size == 0:
        return dict({"n": 0}, **{k: None for k in keys})
    vals = (arr.mean(), np.median(arr), np.percentile(arr, 5), np.percentile(arr, 95), arr.min(), arr.max())
    # + 0.0 は -0.0 を 0.0 にする (遅れ 0 の符号を返した値が -0.0 と出ないように)。
    return dict({"n": int(arr.size)}, **{k: round(float(v), 1) + 0.0 for k, v in zip(keys, vals)})


def reference_cell_width(widths, min_frames=REFERENCE_MIN_FRAMES, tolerance=REFERENCE_TOLERANCE):
    """読めたフレームの帯のセルの幅の並びから、基準の幅を返す: 幅 c の前後 tolerance の内に min_frames フレーム以上 (読めたフレームが
    それより少なければ全部) ある c のうち、いちばん大きいもの。無ければ最大の幅。空なら None。"""
    w = np.sort(np.asarray(widths, dtype=np.float64))
    if w.size == 0:
        return None
    need = min(min_frames, int(w.size))
    for c in np.unique(w)[::-1]:
        lo = np.searchsorted(w, c * (1.0 - tolerance), "left")
        hi = np.searchsorted(w, c * (1.0 + tolerance), "right")
        if hi - lo >= need:
            return float(c)
    return float(w[-1])


def advance_rate(times, values, band_fps=DEFAULT_BAND_FPS):
    """読めたフレームの (pts, 番号) から、番号の進み方 (pts の 1 秒あたり) と、使った区間の長さ (秒) を返す。
    番号は 24 ビットの輪で開き、LOOP_BACK_S 秒より大きく戻った所 (テスト動画の繰り返し) で区間を切り、区間ごとの
    端から端の差を足し合わせる (途中で共有が止まっても、時刻の番号は後で追いつくので端から端の差は変わらない)。"""
    if len(values) < 2:
        return None, 0.0
    t = np.asarray(times, dtype=np.float64)
    d = np.diff(np.asarray(values, dtype=np.int64))
    d = np.where(d < -HALF_MODULO, d + COUNTER_MODULO, np.where(d >= HALF_MODULO, d - COUNTER_MODULO, d))
    cuts = np.flatnonzero(d < -band_fps * LOOP_BACK_S)
    bounds = [-1] + [int(c) for c in cuts] + [len(values) - 1]
    total_units = 0
    total_s = 0.0
    for k in range(len(bounds) - 1):
        start, end = bounds[k] + 1, bounds[k + 1]
        if end > start:
            total_units += int(d[start:end].sum())
            total_s += float(t[end] - t[start])
    return (total_units / total_s if total_s > 0 else None), total_s


def slope_per_minute(times, values):
    """値の時間に対する最小二乗の傾き (分あたり)。組が 3 つ未満か区間が 0 なら None。"""
    if len(values) < 3:
        return None
    t = np.asarray(times, dtype=np.float64) / 60.0
    v = np.asarray(values, dtype=np.float64)
    tc = t - t.mean()
    denom = float(np.sum(tc * tc))
    if denom <= 0:
        return None
    return round(float(np.sum(tc * (v - v.mean())) / denom), 2)


# ---- 帯を読む -------------------------------------------------------------------------------------------

def same_band(g1, g2):
    """2 つの Geometry が同じ所の同じ大きさの帯か。探し直すたびの測りの揺れ (セルの高さで 0.25 px ほど。bit_strip の
    close_to は 0.2 px までしか同じとみなさない) を許す: 位置はセルの半分か 2 px、セルの幅と高さは 1 割まで。"""
    return (abs(g1.x0 - g2.x0) <= max(2.0, 0.5 * g2.cw) and abs(g1.top - g2.top) <= max(2.0, 0.5 * g2.uh)
            and abs(g1.cw - g2.cw) <= 0.1 * g2.cw and abs(g1.uh - g2.uh) <= 0.1 * g2.uh)


def count_places(geoms):
    """Geometry の並びを same_band でまとめた所の数。"""
    reps = []
    for g in geoms:
        if not any(same_band(g, r) for r in reps):
            reps.append(g)
    return len(reps)


class BandReader:
    """領域 1 つの帯を読む。bit_strip.StripTracker に、定期の探し直しと CRC が続いたときの探し直しを足す
    (冒頭の「領域 B に帯が複数あることがある」)。"""

    def __init__(self):
        self.tracker = bit_strip.StripTracker()
        self.crc_run = 0
        self.forced_searches = 0
        self.switches = 0

    def feed(self, img, index):
        """(状態, 番号, 読んだ帯の Geometry) を返す。状態は ok / crc / lost (番号と Geometry は ok のときだけ)。"""
        t = self.tracker
        if t.geom is not None and (index % RESEARCH_EVERY_FRAMES == 0 or self.crc_run >= CRC_RUN_RESEARCH):
            self.forced_searches += 1
            self.crc_run = 0
            found = bit_strip.find_strip(img)
            if found is not None and not same_band(found, t.geom) and found.cw >= SMALL_BAND_RATIO * t.geom.cw:
                t.geometries.append((index, found))
                t.geom = found
                self.switches += 1
        status, value = t.feed(img, index)
        self.crc_run = self.crc_run + 1 if status == "crc" else 0
        if status == "ok":
            return status, value, t.geom
        return status, None, None


def summarize_region(region, requested, reader, rows, times, band_fps):
    """領域 1 つの結果と、フレームごとの使える番号 (使えないフレームは None) と状態の並びを返す。"""
    x, y, _, _ = region
    t = reader.tracker
    ok_index = [i for i, r in enumerate(rows) if r[0] == "ok"]
    ref = reference_cell_width([rows[i][2].cw for i in ok_index])
    statuses = []
    usable = []
    for status, value, geom in rows:
        if status == "ok" and geom.cw < SMALL_BAND_RATIO * ref:
            status = "small"
        statuses.append(status)
        usable.append(value if status == "ok" else None)
    counts = {s: statuses.count(s) for s in STATUSES}
    # 帯の位置の移り (時計の 0 番のセルの左上とセルの大きさ。録画の座標) と、その位置で使えたフレームの数。
    ats = [at for at, _ in t.geometries]
    per_geom = [0] * len(t.geometries)
    for i, v in enumerate(usable):
        if v is not None and ats:
            k = bisect.bisect_right(ats, i) - 1
            if k >= 0:
                per_geom[k] += 1
    is_small = [bool(ref is not None and g.cw < SMALL_BAND_RATIO * ref) for _, g in t.geometries]
    geometry = [dict(g.to_dict(x, y), at=at, usable_frames=per_geom[k], small=is_small[k])
                for k, (at, g) in enumerate(t.geometries[:GEOMETRY_LIST_MAX])]
    places = count_places([g for k, (_, g) in enumerate(t.geometries) if per_geom[k] > 0 and not is_small[k]])
    use_t = [times[i] for i, v in enumerate(usable) if v is not None]
    use_v = [v for v in usable if v is not None]
    rate, rate_span = advance_rate(use_t, use_v, band_fps)
    out = {
        "region": {"x": region[0], "y": region[1], "w": region[2], "h": region[3]},
        "barcode_found": counts["ok"] + counts["small"] > 0,
        "usable_frames": counts["ok"],
        "status": counts,
        "reference_cell_w": round(ref, 2) if ref is not None else None,
        "first": use_v[0] if use_v else None,
        "last": use_v[-1] if use_v else None,
        "span_s": round(use_t[-1] - use_t[0], 3) if use_t else None,
        "advance_per_s": round(rate, 3) if rate is not None else None,
        "advance_span_s": round(rate_span, 3),
        "searches": t.searches,
        "forced_searches": reader.forced_searches,
        "switches": reader.switches,
        "geometry": geometry,
        "geometry_changes": max(0, len(t.geometries) - 1),
        "places": places,
    }
    if tuple(requested) != tuple(region):
        out["region_requested"] = {"x": requested[0], "y": requested[1], "w": requested[2], "h": requested[3]}
    return out, usable, statuses


# ---- 測る -----------------------------------------------------------------------------------------------

def early_error(out, message, detect_ms, target_ms, min_pairs):
    """録画を読む前に決められなくなったときの結果 (読む側が measured・pairs・judge.level をいつも引けるように)。"""
    out.update(measured=False, pairs=0, error=message, warnings=[])
    out["judge"] = {"statistic": "mean", "delay_ms": None, "detect_ms": detect_ms, "target_ms": target_ms,
                    "level": "undecidable", "gate": False, "min_pairs": min_pairs, "reason": message}
    return out


def measure(path, region_a, region_b, band_fps=DEFAULT_BAND_FPS, target_ms=TARGET_MS, detect_ms=DETECT_MS,
            max_abs_ms=MAX_ABS_MS, min_pairs=MIN_PAIRS, max_seconds=None, ffmpeg="ffmpeg", ffprobe="ffprobe"):
    """録画 1 本を読み、結果の dict を返す (例外を投げずに error へ書く)。"""
    out = {"file": str(path), "measured": False}
    try:
        info = video_frames.probe_video(str(path), ffprobe=ffprobe)
    except Exception as e:  # noqa: BLE001
        stderr = getattr(e, "stderr", None)
        detail = (stderr.strip()[-300:] if isinstance(stderr, str) and stderr.strip() else str(e)[:300])
        return early_error(out, "ffprobe could not read the recording: %s" % (detail,), detect_ms, target_ms, min_pairs)
    fw, fh = info["width"], info["height"]
    out.update(width=fw, height=fh, r_frame_rate=info["r_frame_rate"], time_source="pts")
    regions = {}
    for key, req in (("a", region_a), ("b", region_b)):
        clamped = video_frames.clamp_region(req[0], req[1], req[2], req[3], fw, fh)
        if clamped is None:
            return early_error(out, "region %s is outside the %dx%d recording: %s" % (key.upper(), fw, fh, ",".join(map(str, req))),
                               detect_ms, target_ms, min_pairs)
        regions[key] = clamped
    bx0 = min(r[0] for r in regions.values())
    by0 = min(r[1] for r in regions.values())
    bx1 = max(r[0] + r[2] for r in regions.values())
    by1 = max(r[1] + r[3] for r in regions.values())
    box = (bx0, by0, bx1 - bx0, by1 - by0)
    readers = {"a": BandReader(), "b": BandReader()}
    rows = {"a": [], "b": []}
    times = []
    decode_error = None
    frames = video_frames.iter_gray_frames(str(path), fw, fh, box, ffmpeg=ffmpeg)
    try:
        for index, (pts, img) in enumerate(frames):
            if max_seconds is not None and times and pts - times[0] >= max_seconds:
                break
            times.append(pts)
            for key in ("a", "b"):
                rx, ry, rw, rh = regions[key]
                part = img[ry - by0:ry - by0 + rh, rx - bx0:rx - bx0 + rw]
                rows[key].append(readers[key].feed(part, index))
    except (RuntimeError, OSError) as e:
        decode_error = "ffmpeg could not decode the recording: %s" % (str(e)[:300],)
    finally:
        frames.close()
    fps = video_frames.fps_from_pts(times)
    frame_ms = 1000.0 / fps if fps else None
    out.update(fps=round(fps, 3) if fps else None, frames=len(times),
               output_frame_ms=round(frame_ms, 2) if frame_ms else None)
    if max_seconds is not None:
        out["max_seconds"] = max_seconds
    region_out = {}
    usable = {}
    statuses = {}
    for key, req in (("a", region_a), ("b", region_b)):
        region_out[key], usable[key], statuses[key] = summarize_region(
            regions[key], req, readers[key], rows[key], times, band_fps)
    out["region_a"] = region_out["a"]
    out["region_b"] = region_out["b"]

    # 同じ出力のフレームの中の組。A の状態ごとに B の状態を数える (合計 = frames)。
    table = {sa: {sb: 0 for sb in STATUSES} for sa in STATUSES}
    for sa, sb in zip(statuses["a"], statuses["b"]):
        table[sa][sb] += 1
    ms_per_unit = 1000.0 / band_fps
    pair_t = []
    pair_units = []
    implausible = []
    for i, (va, vb) in enumerate(zip(usable["a"], usable["b"])):
        if va is None or vb is None:
            continue
        d = signed_units(va, vb)
        if abs(d) * ms_per_unit > max_abs_ms:
            implausible.append(d)
        else:
            pair_t.append(times[i])
            pair_units.append(d)
    a_only = sum(1 for va, vb in zip(usable["a"], usable["b"]) if va is not None and vb is None)
    b_only = sum(1 for va, vb in zip(usable["a"], usable["b"]) if va is None and vb is not None)
    neither = sum(1 for va, vb in zip(usable["a"], usable["b"]) if va is None and vb is None)
    out["frame_status"] = table
    out["buckets"] = {"paired": len(pair_units), "implausible": len(implausible), "a_only": a_only,
                      "b_only": b_only, "neither": neither}
    out["pairs"] = len(pair_units)
    out["implausible"] = {
        "n": len(implausible), "max_abs_ms": max_abs_ms,
        "median_a_minus_b_ms": round(float(np.median(implausible)) * ms_per_unit, 1) if implausible else None,
    }

    # 番号を ms にする係数と、帯から測った進み方 (確認。警告は下の warnings)。
    out["band_rate"] = {"used_per_s": band_fps, "source": "--band-fps", "ms_per_unit": round(ms_per_unit, 3),
                        "measured_a_per_s": out["region_a"]["advance_per_s"],
                        "measured_b_per_s": out["region_b"]["advance_per_s"]}

    a_minus_b = [d * ms_per_unit for d in pair_units]
    delay = [-v for v in a_minus_b]
    out["a_minus_b_ms"] = describe(a_minus_b)
    out["cordmux_delay_ms"] = describe(delay)
    hist = {}
    for d in pair_units:
        hist[-d] = hist.get(-d, 0) + 1
    top = sorted(hist.items(), key=lambda kv: (-kv[1], kv[0]))[:HISTOGRAM_MAX]
    out["delay_histogram"] = [{"units": u, "ms": round(u * ms_per_unit, 1), "pairs": n} for u, n in sorted(top)]
    by_minute = {}
    t0 = times[0] if times else 0.0
    for t, v in zip(pair_t, delay):
        by_minute.setdefault(int((t - t0) // 60.0), []).append(v)
    out["by_minute"] = [{"minute": m, "pairs": len(v), "mean_ms": round(float(np.mean(v)), 1)}
                        for m, v in sorted(by_minute.items())[:BY_MINUTE_MAX]]
    out["slope_ms_per_min"] = slope_per_minute(pair_t, delay)

    bias_ms = round(frame_ms, 1) if frame_ms else None
    out["capture_bias"] = {
        "approx_ms": bias_ms, "subtracted": False,
        "note": "領域 B (Discord のウィンドウのウィンドウキャプチャ) はウィンドウが画面に出てから約 1 フレーム遅れて OBS に届く見込み (測っていない)。"
                "その分 cordmux_delay_ms は本当の遅れより小さく出る。引かずに偏りとして記録する",
    }

    measured = len(pair_units) >= max(1, min_pairs) and decode_error is None
    mean_delay = out["cordmux_delay_ms"]["mean"] if measured else None
    judge = {
        "statistic": "mean", "delay_ms": mean_delay, "detect_ms": detect_ms, "target_ms": target_ms,
        "level": delay_level(mean_delay, detect_ms, target_ms),
        "over_detect": (mean_delay >= detect_ms) if mean_delay is not None else None,
        "over_target": (mean_delay >= target_ms) if mean_delay is not None else None,
        "share_over_detect": round(sum(1 for v in delay if v >= detect_ms) / len(delay), 3) if measured else None,
        "share_over_target": round(sum(1 for v in delay if v >= target_ms) / len(delay), 3) if measured else None,
        "with_capture_bias": {
            "delay_ms": round(mean_delay + bias_ms, 1) if (mean_delay is not None and bias_ms is not None) else None,
            "level": delay_level(mean_delay + bias_ms if (mean_delay is not None and bias_ms is not None) else None,
                                 detect_ms, target_ms),
        },
        "gate": False, "min_pairs": min_pairs,
    }
    warnings = []
    if decode_error:
        out["error"] = decode_error
        judge["reason"] = decode_error
    elif not measured:
        why = []
        if not out["region_a"]["barcode_found"]:
            why.append("領域 A に帯が見つからない")
        if not out["region_b"]["barcode_found"]:
            why.append("領域 B に帯が見つからない")
        if implausible:
            why.append("両方を読めたフレーム %d のうち %d がありえない差 (|A - B| > %s ms)" % (
                len(implausible) + len(pair_units), len(implausible), max_abs_ms))
        why.append("組 %d が下限 %d に届かない" % (len(pair_units), min_pairs))
        judge["reason"] = "。".join(why)
        out["error"] = judge["reason"]
    for key, label in (("a", "A"), ("b", "B")):
        r = out["region_" + key]
        rate = r["advance_per_s"]
        if rate is not None and r["advance_span_s"] >= RATE_MIN_SPAN_S and abs(rate / band_fps - 1.0) > RATE_WARN_RATIO:
            warnings.append("領域 %s の帯の進み方が 1 秒に %.2f で、--band-fps %s と %d%% を超えて違う (共有の中身がテストパターンで"
                            "ないか、共有が長く止まった)" % (label, rate, band_fps, round(RATE_WARN_RATIO * 100)))
        if r["status"]["small"] > 0:
            warnings.append("領域 %s の %d フレームは基準 (セル %s px) より小さい帯で読んだので組にしていない (別の共有の小さな枠の見込み)"
                            % (label, r["status"]["small"], r["reference_cell_w"]))
        if key == "b" and r["places"] >= 2:
            warnings.append("領域 B の帯の位置が %d か所に分かれた (ウィンドウの並びが変わったか、同じ大きさの別の共有に移った見込み。"
                            "geometry と撮った絵で確認する)" % r["places"])
    if implausible:
        warnings.append("ありえない差のフレーム %d (差の中央値 %s ms) は組にしていない" % (
            len(implausible), out["implausible"]["median_a_minus_b_ms"]))
    if mean_delay is not None and frame_ms is not None and mean_delay < -2.0 * frame_ms:
        warnings.append("合成のソース cordmux の共有 (A) が Discord のウィンドウ (B) より先に進んでいる。B が配信者の共有の大きな表示を映しているか "
                        "(自分の共有のプレビューや別の人の共有でないか) を確認する")
    out["judge"] = judge
    out["measured"] = measured
    out["warnings"] = warnings
    out["note"] = ("a_minus_b_ms = A (合成のソース cordmux の共有) の番号 - B (Discord のウィンドウ) の番号を ms にした値で、cordmux が遅いと負。"
                   "cordmux_delay_ms = B - A (正 = cordmux の出力が Discord 自身の再生より遅い)。1 組の値は番号の整数の差なので、"
                   "中央値・百分位・最小・最大は %.1f ms の刻み (共有のフレームの間隔の倍数に寄る)。判定は cordmux_delay_ms の平均で、"
                   "検知限 %s ms・許容限 %s ms (ITU-R BT.1359-1、音が先の側)。実行は止めない (gate: false)" % (
                       ms_per_unit, detect_ms, target_ms))
    return out


# ---- セルフテスト --------------------------------------------------------------------------------------------
# ffmpeg で、同じ帯を 2 つの領域に、片方を決まったフレーム数だけ遅らせて描いた 3 秒の動画を作り、遅れを読ませる。
# 絵は numpy と bit_strip.draw_strip で描き、rawvideo で ffmpeg に流して libx264 の mkv にする (make_sync_video.py と
# 同じ描き方。同梱の ffmpeg には drawtext がない)。領域 A は左上の象限、領域 B は右下の象限 (切り出す四角が画面の全部に
# なる形)。テストデータは --outdir (既定は呼び出し元の TEMP) の下の latency-selftest-* に作り、終わったら作ったファイルを 1 つずつ
# 消してからフォルダーを消す (--keep で残す)。

ST_W, ST_H, ST_FPS, ST_FRAMES = 960, 540, 60, 180
ST_REGION_A = (0, 0, 480, 270)
ST_REGION_B = (480, 270, 480, 270)
ST_A = (40, 30, 6)          # 領域 A の中の帯の左上とセル (合成のソース cordmux の中の共有)
ST_B = (60, 50, 8)          # 領域 B の中の帯の左上とセル (Discord のウィンドウの中の大きな表示)
ST_B_MOVES = [(60, 50, 8), (150, 180, 9), (20, 200, 8), (160, 20, 9)]
ST_DECOY = (320, 230, 4)    # 領域 B の中の小さな枠 (別の経路の同じ帯)
ST_BASE = 1085000
ST_UNIT_MS = 1000.0 / ST_FPS


def _st_background():
    rows = np.linspace(40, 110, ST_H, dtype=np.float32)
    bg = np.repeat(rows[:, None], ST_W, axis=1)
    yy, xx = np.mgrid[0:ST_H, 0:ST_W]
    bg += (((yy // 30) + (xx // 30)) % 2).astype(np.float32) * 8
    img = np.clip(bg, 0, 255).astype(np.uint8)
    rng = np.random.default_rng(11)
    for _ in range(160):   # 文字に似た明るい短い棒 (探すときの紛らわしいもの)
        x = int(rng.integers(0, ST_W - 10))
        y = int(rng.integers(0, ST_H - 4))
        img[y:y + 3, x:x + int(rng.integers(2, 9))] = 220
    return img


def _st_draw(img, region, spot, counter, corrupt=False):
    x, y, u = region[0] + spot[0], region[1] + spot[1], spot[2]
    bit_strip.draw_strip(img, x, y, u, counter)
    if corrupt:   # データの 30 番のセルを反転する (形は合うが CRC が合わないフレーム)
        xa, ya = x + (1 + 30) * u, y + 2 * u
        img[ya:ya + 2 * u, xa:xa + u] = 255 - img[ya:ya + 2 * u, xa:xa + u]


def _st_scenarios():
    """(名前, フレーム n を描く関数) の並び。関数は背景のコピーに帯を描く。"""
    def delayed(k, base=ST_BASE):
        def f(img, n):
            _st_draw(img, ST_REGION_A, ST_A, base + n - k)
            _st_draw(img, ST_REGION_B, ST_B, base + n)
        return f

    def moving(img, n):
        _st_draw(img, ST_REGION_A, ST_A, ST_BASE + n - 3)
        _st_draw(img, ST_REGION_B, ST_B_MOVES[(n // 20) % len(ST_B_MOVES)], ST_BASE + n)

    def unreadable(img, n):
        if not 60 <= n < 75:
            _st_draw(img, ST_REGION_A, ST_A, ST_BASE + n - 3)
        _st_draw(img, ST_REGION_B, ST_B, ST_BASE + n, corrupt=120 <= n < 130)

    def decoy(img, n):
        _st_draw(img, ST_REGION_A, ST_A, ST_BASE + n - 3)
        if not 60 <= n < 90:
            _st_draw(img, ST_REGION_B, ST_B, ST_BASE + n)
        _st_draw(img, ST_REGION_B, ST_DECOY, ST_BASE + n - 30)

    def content30(img, n):
        _st_draw(img, ST_REGION_A, ST_A, ST_BASE + 2 * ((n - 3) // 2))
        _st_draw(img, ST_REGION_B, ST_B, ST_BASE + 2 * (n // 2))

    def no_band_b(img, n):
        _st_draw(img, ST_REGION_A, ST_A, ST_BASE + n)

    def unrelated_b(img, n):
        _st_draw(img, ST_REGION_A, ST_A, ST_BASE + n - 3)
        _st_draw(img, ST_REGION_B, ST_B, 31700 + n)

    return [("delay0", delayed(0)), ("delay3_wrap", delayed(3, COUNTER_MODULO - 100)), ("delay6", delayed(6)),
            ("moving_b", moving), ("unreadable", unreadable), ("decoy", decoy), ("content30", content30),
            ("no_band_b", no_band_b), ("unrelated_b", unrelated_b)]


def _st_encode(ffmpeg, out_path, draw):
    """背景に帯を描いた ST_FRAMES フレームを libx264 の mkv にする。失敗なら理由の文字列、できたら None。"""
    cmd = [ffmpeg, "-hide_banner", "-nostats", "-loglevel", "error", "-y",
           "-f", "rawvideo", "-pix_fmt", "gray", "-s", "%dx%d" % (ST_W, ST_H), "-r", str(ST_FPS), "-i", "-",
           "-c:v", "libx264", "-preset", "veryfast", "-crf", "20", "-pix_fmt", "yuv420p", "-g", str(ST_FPS),
           str(out_path)]
    bg = _st_background()
    try:
        proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stderr=subprocess.PIPE)
    except OSError as e:
        return "could not start ffmpeg: %s" % e
    try:
        for n in range(ST_FRAMES):
            img = bg.copy()
            draw(img, n)
            proc.stdin.write(img.tobytes())
    except (BrokenPipeError, OSError):
        pass
    finally:
        proc.stdin.close()
    err = proc.stderr.read().decode("utf-8", "replace").strip()
    proc.wait()
    if proc.returncode != 0 or not os.path.isfile(out_path):
        return "ffmpeg failed (%s): %s" % (proc.returncode, err[-300:])
    return None


def selftest_place_problem(path, base_dir, repo_root=REPO_ROOT):
    """自己検査の保存先が base_dir の下で、repo の _work (実機テストの作業用フォルダー) の外か。合えば None、合わなければ理由
    (border_timing.py の selftest_place_problem と同じ考え)。"""
    full = os.path.normcase(os.path.abspath(path))
    base_full = os.path.normcase(os.path.abspath(base_dir))
    work_full = os.path.normcase(os.path.abspath(os.path.join(str(repo_root), "_work")))
    if full != base_full and not full.startswith(base_full.rstrip(os.sep) + os.sep):
        return "not under " + str(base_dir)
    if full == work_full or full.startswith(work_full + os.sep):
        return "under _work (the live window's folder)"
    return None


def selftest(ffmpeg, ffprobe, outdir=None, keep=False):
    failures = []

    def check(cond, label):
        print(("ok   " if cond else "FAIL ") + label)
        if not cond:
            failures.append(label)

    # 純関数 (ファイルなし)。
    check(signed_units(5, COUNTER_MODULO - 3) == 8 and signed_units(COUNTER_MODULO - 3, 5) == -8,
          "the difference wraps on the 24-bit counter")
    check(signed_units(ST_BASE, ST_BASE + 3) == -3, "a lagging A gives a negative A - B")
    check([delay_level(v) for v in (None, -20.0, 44.9, 45.0, 89.9, 90.0)] ==
          ["undecidable", "within_detect", "within_detect", "within_target", "within_target", "over_target"],
          "levels: below 45 is within_detect, 45 up to below 90 is within_target, 90 and more is over_target")
    st = describe([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10])
    check(st["n"] == 11 and st["mean"] == 5.0 and st["median"] == 5.0 and st["p5"] == 0.5 and st["p95"] == 9.5
          and st["min"] == 0.0 and st["max"] == 10.0, "describe gives mean, median and the 5th and 95th percentiles (%s)" % st)
    check(describe([])["mean"] is None, "describe of nothing gives None")
    check(reference_cell_width([4.0] * 100 + [8.0] * 40) == 8.0, "the reference cell is the largest band with enough frames")
    check(reference_cell_width([4.0] * 100 + [8.0] * 5) == 4.0, "a band seen in only a few frames is not the reference")
    tt = [i / 60.0 for i in range(600)]
    loop = [(35900 + i) % 36000 for i in range(600)]   # テスト動画の繰り返し (36000 で 0 に戻る)
    rate, span = advance_rate(tt, loop)
    check(rate is not None and abs(rate - 60.0) < 0.2 and span > 9.0, "the advance rate survives a loop of the test video (%s)" % rate)
    wrap = [(COUNTER_MODULO - 50 + i) % COUNTER_MODULO for i in range(600)]
    rate, _ = advance_rate(tt, wrap)
    check(rate is not None and abs(rate - 60.0) < 0.2, "the advance rate survives the 24-bit wrap (%s)" % rate)
    frozen = [100 + i for i in range(200)] + [300] * 200 + [500 + i for i in range(200)]   # 途中で止まって追いつく
    rate, _ = advance_rate(tt, frozen)
    check(rate is not None and abs(rate - 60.0) < 0.2, "a freeze in the middle does not bend the advance rate (%s)" % rate)
    check(slope_per_minute([0.0, 60.0, 120.0], [10.0, 12.0, 14.0]) == 2.0, "the slope is per minute")
    g1 = bit_strip.Geometry(x0=237.0, cw=13.33, top=124.0, uh=13.25)
    g2 = bit_strip.Geometry(x0=237.0, cw=13.33, top=124.0, uh=13.5)   # 探し直しが出す揺れ
    g3 = bit_strip.Geometry(x0=237.0 + 3 * 13.33, cw=13.33, top=124.0, uh=13.25)
    check(same_band(g1, g2) and not g1.close_to(g2), "a re-measured band 0.25 px taller is the same band")
    check(not same_band(g1, g3) and count_places([g1, g2, g3, g1]) == 2, "a band 3 cells away is another place")
    missing_file = measure(os.path.join(str(HERE), "no-such-recording.mkv"), ST_REGION_A, ST_REGION_B, ffmpeg=ffmpeg, ffprobe=ffprobe)
    check(missing_file["measured"] is False and missing_file["pairs"] == 0 and missing_file["judge"]["level"] == "undecidable"
          and bool(missing_file.get("error")), "an unreadable recording still gives measured, pairs and judge.level")

    # 動画のテストデータ (ffmpeg が要る)。
    base_dir = outdir or tempfile.gettempdir()
    os.makedirs(base_dir, exist_ok=True)
    work = tempfile.mkdtemp(prefix="latency-selftest-", dir=base_dir)
    print("selftest work: " + work)
    check(selftest_place_problem(work, base_dir) is None, "the work folder is under the given place and outside _work")
    check(selftest_place_problem(os.path.join(str(REPO_ROOT), "_work", "x"), str(REPO_ROOT)) is not None,
          "a folder under _work is refused")
    created = []
    results = {}
    try:
        for name, draw in _st_scenarios():
            video = os.path.join(work, name + ".mkv")
            err = _st_encode(ffmpeg, video, draw)
            if os.path.isfile(video):
                created.append(video)
            if err:
                check(False, "%s: make the video with ffmpeg (%s)" % (name, err))
                continue
            results[name] = measure(video, ST_REGION_A, ST_REGION_B, ffmpeg=ffmpeg, ffprobe=ffprobe)
            if name == "delay3_wrap":   # 同じテストデータを間違った係数で読む (進み方の警告の確認)
                results["delay3_fps30"] = measure(video, ST_REGION_A, ST_REGION_B, band_fps=30.0, ffmpeg=ffmpeg, ffprobe=ffprobe)
    finally:
        if not keep:
            for f in created:
                try:
                    os.remove(f)
                except OSError as e:
                    check(False, "remove %s (%s)" % (f, e))
            try:
                os.rmdir(work)
            except OSError as e:
                check(False, "remove the work folder %s (%s)" % (work, e))
    if not keep:
        check(not os.path.exists(work), "the work folder is removed")

    def delay_is(r, ms, exact=True):
        d = r["cordmux_delay_ms"]
        want = round(ms, 1)
        if exact:
            return d["mean"] == want and d["median"] == want and d["p5"] == want and d["p95"] == want and d["max"] == want
        return d["mean"] is not None and abs(d["mean"] - want) <= 1.0

    def show(r):
        return "pairs=%s delay=%s status_a=%s status_b=%s buckets=%s level=%s" % (
            r.get("pairs"), r.get("cordmux_delay_ms"), r.get("region_a", {}).get("status"),
            r.get("region_b", {}).get("status"), r.get("buckets"), r.get("judge", {}).get("level"))

    r = results.get("delay0")
    if r:
        check(r["measured"] and r["pairs"] == ST_FRAMES and delay_is(r, 0.0) and r["judge"]["level"] == "within_detect",
              "delay0: every frame pairs and the delay is 0 ms (%s)" % show(r))
        rate = r["region_b"]["advance_per_s"]
        check(rate is not None and abs(rate - ST_FPS) < 0.6 and not r["warnings"],
              "delay0: the band advances 60 per second and nothing is warned (%s, warnings=%d)" % (rate, len(r["warnings"])))
        check(r["capture_bias"]["approx_ms"] is not None and abs(r["capture_bias"]["approx_ms"] - ST_UNIT_MS) < 0.5,
              "delay0: the capture bias is one output frame (%s)" % r["capture_bias"]["approx_ms"])
    r = results.get("delay3_wrap")
    if r:
        check(r["measured"] and r["pairs"] == ST_FRAMES and delay_is(r, 3 * ST_UNIT_MS),
              "delay3_wrap: 3 frames late reads 50.0 ms across the 24-bit wrap (%s)" % show(r))
        check(r["a_minus_b_ms"]["mean"] == -50.0, "delay3_wrap: A - B is -50.0 ms (%s)" % r["a_minus_b_ms"])
        check(r["judge"]["level"] == "within_target" and r["judge"]["over_detect"] is True and r["judge"]["over_target"] is False,
              "delay3_wrap: 50 ms is over the detection limit and within the target (%s)" % r["judge"])
        check(r["region_a"]["first"] > r["region_a"]["last"], "delay3_wrap: the counter wrapped inside the clip")
    r = results.get("delay3_fps30")
    if r:
        check(r["cordmux_delay_ms"]["mean"] == 100.0 and r["band_rate"]["used_per_s"] == 30.0
              and sum(1 for w in r["warnings"] if "進み方" in w) == 2,
              "delay3_fps30: --band-fps 30 on a 60 per second band doubles the reading and warns for both regions (%s, warnings=%d)"
              % (r["cordmux_delay_ms"]["mean"], len(r["warnings"])))
    r = results.get("delay6")
    if r:
        check(r["measured"] and delay_is(r, 6 * ST_UNIT_MS) and r["judge"]["level"] == "over_target"
              and r["judge"]["share_over_target"] == 1.0, "delay6: 6 frames late reads 100.0 ms, over the target (%s)" % show(r))
    r = results.get("moving_b")
    if r:
        moves = ST_FRAMES // 20 - 1
        check(r["measured"] and delay_is(r, 3 * ST_UNIT_MS) and r["pairs"] >= ST_FRAMES - moves,
              "moving_b: the band of B moves %d times and the delay stays 50.0 ms (%s)" % (moves, show(r)))
        check(r["region_b"]["geometry_changes"] >= moves, "moving_b: each move is a new geometry (%s)" % r["region_b"]["geometry_changes"])
        check(r["region_b"]["places"] == len(ST_B_MOVES) and any("か所に分かれた" in w for w in r["warnings"]),
              "moving_b: the 4 places are counted and warned (places=%s)" % r["region_b"]["places"])
    r = results.get("unreadable")
    if r:
        fs = r["frame_status"]
        check(fs["lost"]["ok"] == 15 and fs["ok"]["crc"] == 10 and fs["ok"]["ok"] == ST_FRAMES - 25 and r["pairs"] == ST_FRAMES - 25,
              "unreadable: 15 frames without the band in A and 10 bad-CRC frames in B are not paired (%s)" % fs)
        check(delay_is(r, 3 * ST_UNIT_MS) and r["buckets"]["b_only"] == 15 and r["buckets"]["a_only"] == 10,
              "unreadable: the other frames still read 50.0 ms (%s)" % show(r))
    r = results.get("decoy")
    if r:
        check(r["region_b"]["status"]["small"] == 60 and r["pairs"] == ST_FRAMES - 60 and delay_is(r, 3 * ST_UNIT_MS),
              "decoy: frames read on the small band are not paired and the large band gives 50.0 ms (%s)" % show(r))
        check(r["region_b"]["switches"] >= 1 and any("小さい帯" in w for w in r["warnings"]),
              "decoy: the reader goes back to the large band and warns (switches=%s warnings=%d)"
              % (r["region_b"]["switches"], len(r["warnings"])))
        check(r["region_b"]["places"] == 1 and not any("か所に分かれた" in w for w in r["warnings"]),
              "decoy: the small band is not counted as a place of the large band (places=%s)" % r["region_b"]["places"])
    r = results.get("content30")
    if r:
        d = r["cordmux_delay_ms"]
        check(r["measured"] and delay_is(r, 3 * ST_UNIT_MS, exact=False) and d["p5"] == round(2 * ST_UNIT_MS, 1)
              and d["p95"] == round(4 * ST_UNIT_MS, 1),
              "content30: a 30 fps share reads 33.3 and 66.7 ms per pair and the mean is 50 ms (%s)" % show(r))
    r = results.get("no_band_b")
    if r:
        check(not r["measured"] and r["judge"]["level"] == "undecidable" and not r["region_b"]["barcode_found"]
              and r["region_a"]["barcode_found"] and r["pairs"] == 0 and bool(r.get("error")),
              "no_band_b: no band in B is undecidable with a reason (%s)" % show(r))
    r = results.get("unrelated_b")
    if r:
        check(not r["measured"] and r["implausible"]["n"] == ST_FRAMES and r["pairs"] == 0 and r["judge"]["level"] == "undecidable",
              "unrelated_b: an unrelated counter in B is implausible, not a delay (%s)" % show(r))
    missing = [name for name, _ in _st_scenarios() if name not in results]
    check(not missing, "every clip was measured (missing: %s)" % missing)
    print("selftest: %d failure(s)" % len(failures))
    return 1 if failures else 0


def main(argv):
    parser = argparse.ArgumentParser(description="cordmux が足す遅れを 2 つの領域の時計つきの帯から読む (冒頭の注記)。")
    parser.add_argument("video", nargs="?")
    parser.add_argument("--region-a", type=parse_region, default=None, help="x,y,w,h of the cordmux tile in the recording")
    parser.add_argument("--region-b", type=parse_region, default=None, help="x,y,w,h of the captured Discord window in the recording")
    parser.add_argument("--band-fps", type=float, default=DEFAULT_BAND_FPS, help="band counts per second (60)")
    parser.add_argument("--target-ms", type=float, default=TARGET_MS)
    parser.add_argument("--detect-ms", type=float, default=DETECT_MS)
    parser.add_argument("--max-abs-ms", type=float, default=MAX_ABS_MS)
    parser.add_argument("--min-pairs", type=int, default=MIN_PAIRS)
    parser.add_argument("--max-seconds", type=float, default=None)
    parser.add_argument("--ffmpeg", default=None)
    parser.add_argument("--ffprobe", default=None)
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--outdir", default=None, help="selftest: where the work folder is made (the caller's TEMP)")
    parser.add_argument("--keep", action="store_true", help="selftest: keep the clips")
    args = parser.parse_args(argv[1:])
    ffmpeg, ffprobe = resolve_tools(args.ffmpeg, args.ffprobe)
    if args.selftest:
        return selftest(ffmpeg, ffprobe, args.outdir, args.keep)
    if not args.video or args.region_a is None or args.region_b is None:
        parser.error("give a recording, --region-a and --region-b (or --selftest)")
    if args.band_fps <= 0 or args.max_abs_ms <= 0 or args.min_pairs < 1:
        parser.error("--band-fps and --max-abs-ms must be positive and --min-pairs at least 1")
    result = measure(args.video, args.region_a, args.region_b, band_fps=args.band_fps, target_ms=args.target_ms,
                     detect_ms=args.detect_ms, max_abs_ms=args.max_abs_ms, min_pairs=args.min_pairs,
                     max_seconds=args.max_seconds, ffmpeg=ffmpeg, ffprobe=ffprobe)
    result["ffmpeg"] = ffmpeg
    print(json.dumps(result, indent=1, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
