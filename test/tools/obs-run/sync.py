# 映像と音のずれを測る。
# 送信側が「全面の白 3 フレーム」と「同じタイムスタンプから 60 ms だけ 2 kHz」を同時に出すマーカーを、OBS の出力の録画から
# 拾って組にし、音のマーカーが映像のマーカーに対してどれだけ遅れ (+) / 先行 (-) しているかを ms で出す
# (値は「音の時刻 - 映像の時刻」)。
# 使い方: python sync.py <録画> [--x 0 --y 0 --w 1280 --h 720] (白を判定する領域 = その人の枠の位置と大きさ)
#
# 動作 (引数と出力のキーは、キーを足しただけで互換):
#   - 映像の時刻はフレームごとの pts (video_frames.py)。「フレームの番号 / r_frame_rate」で出すと、cordmux の録画
#     (約 30 fps なのに r_frame_rate が 60000/1001) では映像の時刻が半分になる。
#   - 音の時刻の原点をファイルの先頭に揃える (先頭の無音を埋める)。音の流れの先頭を原点にすると、ずれる。
#   - 領域は ffmpeg の側で切り出す (パイプへ流す量を領域の大きさに絞る)。
#   - 2 kHz の見つけ方 (下の「2 kHz のマーカーの見つけ方」)。「2 kHz が 1 kHz の 2 倍を超えるウィンドウ」で探すと、
#     声の側に 1 kHz が鳴り続けたとき (Chrome のテスト用のマイクの音) マーカーが 1 つも見つからない。
#   - 白の見つけ方 (下の「白のマーカーの見つけ方」)。「領域の平均が 200 を超えるフレーム」で探すと、白が領域の一部にしか
#     写らないとき 1 つも見つからない (Chrome のタブの共有。動画が枠の約 53% で、白のフレームの平均は 139)。
import collections
import json
import math
import sys

import numpy as np

import video_frames

# 白のマーカーの見つけ方:
#   領域を間引いた点ごとに、直前の WHITE_REFERENCE_FRAMES フレームの最小値を基準にして、「WHITE_RISE 以上明るくなって
#   WHITE_BRIGHT 以上になった点」の割合を出し、それが WHITE_MIN_FRACTION 以上のフレームを白とする。白が続く並びの
#   先頭のフレームの pts をマーカーとする。最初のフレームは真っ黒と比べる (最初のフレームが白なら数える。
#   sync-video.mp4 は 0 フレーム目から白の 3 フレームで始まる)。
#   Chrome のタブで sync-video.html を共有すると、動画が共有の約 53% (853x573) にしか写らず、
#   枠の領域の平均は白のフレームでも 139 で、領域の平均で見る規則では 1 つも白にならない (make_material.py --scene tab で再現)。
#   割合で見れば動画の外 (ページの背景や文字) は数に入らず、基準からの上がりで見るので、動画の外がずっと明るくても
#   白のマーカーのフレームだけが立つ。
#   値は make_material.py のテストデータで当てた: タブの形 (tab) の白のフレームは 0.45、白でないフレームの最大は
#   0.04 (最初のフレームを真っ黒と比べた分)。枠いっぱいのテストデータ (normal) の白は 0.99。境目 0.2 は tab の形の半分より下で、
#   白でないフレームの最大の 5 倍。WHITE_BRIGHT 160 はテスト動画の背景 (48 から 122) より上で、デコードした白 (250 前後) より十分
#   下。WHITE_RISE 64 は、動かない明るい所 (ページの文字・ボタン) と背景の小さな揺れを数えないため。基準を 1 フレーム前では
#   なく直前 4 フレームの最小値にするのは、共有のエンコーダが白のフレームを 2 フレームに分けて描き替える (一部のマクロブロックが
#   前のフレームのまま残る) ときも、2 フレーム目を白の前の絵と比べるため。間引きは短い辺が WHITE_SAMPLE_EDGE 点ほどになる
#   間隔 (640x360 の枠で 4 px おき)。白は領域の広い部分を占めるので、間引いても割合は変わらない。
#   平均の境目を下げる方法は使わない (動画の占める割合と動画の外の明るさで白の平均が変わり、どの値でも別の形で外れる)。
#   「明るい点の割合」だけで見る方法も使わない (動画の外が明るいページだと毎フレーム白になり、立ち上がりがなくなる)。
#   領域の平均の跳ね (前のフレームとの差) で見る方法も使わない (共有が始まって枠が黒から絵に変わる所も、tab の形の白と
#   同じくらい跳ねる)。
#   領域の平均で数えた数も出力に残す (white_detector.mean_over_200_events。セルフテストが tab の形で 0 になることを確認する)。
WHITE_BRIGHT = 160
WHITE_RISE = 64
WHITE_MIN_FRACTION = 0.2
WHITE_REFERENCE_FRAMES = 4
WHITE_SAMPLE_EDGE = 90
OLD_WHITE_MEAN = 200  # 領域の平均で見る規則のしきい値。比べるための数を出すだけ

# 2 kHz のマーカーの見つけ方 (値はセルフテストのテストデータ normal / competing1k / audioshift100 / vfr で当てた):
#   1. 候補のウィンドウ (5 ms): 2 kHz の強さが (a) 絶対の下限 (1e-3) を超え、(b) 自分の近く (自分と 1 つ前の
#      0.5 秒のブロック) の中央値の BURST_RATIO 倍を超え、(c) そのウィンドウで 1 kHz も跳ねている (1 kHz の近くの中央値の
#      BROADBAND_RATIO 倍を超える) なら、2 kHz が 1 kHz の 2 倍を超えていること。(c) は
#      「全部の周波数が一度に上がる音 (手を叩く音・ノイズ)」を、鳴り続ける 1 kHz とは分けて防ぐ。
#   2. 候補のウィンドウを、MERGE_GAP_WINDOWS ウィンドウまでの切れ目はつないで 1 続きにする。
#   3. 続きの中で 2 kHz が最大値の ONSET_FRACTION 以上のウィンドウ (芯) の先頭をマーカーの始まりとし、芯の長さが
#      BURST_MIN_WINDOWS 以上 BURST_MAX_WINDOWS 以下のときだけマーカーとみなす (60 ms = 12 ウィンドウ)。
#      芯で決めるのは、AAC のエンコードがマーカーの直前 25 ms ほどに 2 kHz を薄く漏らす (セルフテストのテストデータで最大値の
#      1/1000 以下) ためで、候補のウィンドウの先頭を取ると始まりが 1 から 5 ウィンドウ早く出る。
# 2 kHz を 1 kHz と比べるのをやめて絶対の強さだけで見る方法は使わない (声や物音の 2 kHz を拾う)。
# 1 kHz の比を 2 倍から下げる方法も使わない (鳴り続ける 1 kHz がマーカーより大きいと、どの比でも見つからない)。
WINDOW_S = 0.005
BLOCK_WINDOWS = 100  # 0.5 秒のブロック (マーカー 12 ウィンドウが混ざっても中央値は動かない)
ABS_FLOOR = 1e-3
BURST_RATIO = 4.0
BROADBAND_RATIO = 2.0
MERGE_GAP_WINDOWS = 2
ONSET_FRACTION = 0.5
BURST_MIN_WINDOWS = 4
BURST_MAX_WINDOWS = 40
PAIR_MAX_S = 0.5
# 時間に対するずれの傾き (出力の trend)。cordmux の録画では音の先行が 10 分で 142 ms から
# 177 ms へ広がることがあり、それを組の並びから読む (pairs は先頭の 50 組しか出さないので、長い録画では後ろが読めない)。
# 全部の組で最小二乗の傾き (分あたりの ms) と、先頭と末尾の TREND_EDGE_PAIRS 組の平均を出す。判定はしない (読むのは人と実行スクリプト)。
# 傾きの標準誤差 (slope_se_ms_per_min) と 95% の幅 (slope_ci95_ms_per_min) も出す。
# 「傾きは 1 ms/分を超えて 0 と分けられれば欠陥」と判定するには、0 と分けられるかを読むのに傾きの揺れの大きさが要る (組の並びの残差から出す。
# t 分布の 2 側 95% の値は自由度 n-2 で引く)。合否を決めるのは呼び出し元 (run-window-plan.mjs の judgeAvSyncLines)。
TREND_EDGE_PAIRS = 5
# t 分布の 2 側 95% の値 (自由度 1-30)。31 以上は下の trend_t95 の段で近似する (標準の表の値)。
T95_BY_DOF = [12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228, 2.201, 2.179, 2.160, 2.145, 2.131,
              2.120, 2.110, 2.101, 2.093, 2.086, 2.080, 2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042]


def trend_t95(dof):
    """自由度 dof の t 分布の 2 側 95% の値。30 を超える自由度は 60 まで 2.000、120 まで 1.980、その先 1.960 (表の値を上から丸めた側)。"""
    if dof < 1:
        return None
    if dof <= len(T95_BY_DOF):
        return T95_BY_DOF[dof - 1]
    if dof <= 60:
        return 2.000
    if dof <= 120:
        return 1.980
    return 1.960


def offset_trend(pairs):
    """組の並び (video_s と audio_minus_video_ms) から、ずれの傾きと先頭と末尾の平均を返す。組が 3 つ未満なら値は None。"""
    n = len(pairs)
    out = {"pairs": n, "span_s": None, "slope_ms_per_min": None, "slope_se_ms_per_min": None, "slope_ci95_ms_per_min": None,
           "first_mean_ms": None, "last_mean_ms": None, "edge_pairs": TREND_EDGE_PAIRS}
    if n < 3:
        return out
    t = np.array([p["video_s"] for p in pairs], dtype=float)
    v = np.array([p["audio_minus_video_ms"] for p in pairs], dtype=float)
    span = float(t[-1] - t[0])
    out["span_s"] = round(span, 1)
    tc = t - t.mean()
    sxx = float(np.sum(tc * tc))
    if span > 0 and sxx > 0:
        slope = float(np.sum(tc * (v - v.mean())) / sxx)
        out["slope_ms_per_min"] = round(slope * 60.0, 2)
        residual = v - (v.mean() + slope * tc)
        dof = n - 2
        se = math.sqrt(float(np.sum(residual * residual)) / dof / sxx) * 60.0
        t95 = trend_t95(dof)
        out["slope_se_ms_per_min"] = round(se, 3)
        out["slope_ci95_ms_per_min"] = [round(slope * 60.0 - t95 * se, 2), round(slope * 60.0 + t95 * se, 2)]
    edge = min(TREND_EDGE_PAIRS, n)
    out["first_mean_ms"] = round(float(np.mean(v[:edge])), 1)
    out["last_mean_ms"] = round(float(np.mean(v[-edge:])), 1)
    return out


def parse_args(argv):
    opts = {"x": 0, "y": 0, "w": 0, "h": 0}
    for i, a in enumerate(argv):
        if a.startswith("--") and a[2:] in opts:
            opts[a[2:]] = int(argv[i + 1])
    return opts


class WhiteDetector:
    """フレームを 1 枚ずつ受け取り、白のマーカーのフレームかどうかと、明るくなった点の割合を返す (上の「白のマーカーの見つけ方」)。"""

    def __init__(self, region_w, region_h):
        self.stride = max(1, int(round(min(region_w, region_h) / float(WHITE_SAMPLE_EDGE))))
        self.history = collections.deque(maxlen=WHITE_REFERENCE_FRAMES)
        self.max_fraction = 0.0

    def feed(self, img):
        sample = img[::self.stride, ::self.stride].astype(np.int16)
        if self.history:
            reference = np.min(np.stack(self.history), axis=0)
        else:
            reference = np.zeros_like(sample)   # 最初のフレームは真っ黒と比べる
        rose = (sample >= WHITE_BRIGHT) & (sample - reference >= WHITE_RISE)
        fraction = float(np.count_nonzero(rose)) / rose.size
        self.history.append(sample)
        self.max_fraction = max(self.max_fraction, fraction)
        return fraction >= WHITE_MIN_FRACTION, fraction


def local_baseline(values):
    """ウィンドウごとに、自分のブロックと 1 つ前のブロックの中央値の大きいほうを返す (マーカーそのものには引っ張られない)。"""
    n = len(values)
    if n == 0:
        return values
    nb = (n + BLOCK_WINDOWS - 1) // BLOCK_WINDOWS
    med = np.array([float(np.median(values[b * BLOCK_WINDOWS:(b + 1) * BLOCK_WINDOWS])) for b in range(nb)])
    prev = np.concatenate([med[:1], med[:-1]])
    per_block = np.maximum(med, prev)
    return np.repeat(per_block, BLOCK_WINDOWS)[:n]


def find_tone_events(samples, sr):
    """2 kHz のマーカーの始まりの時刻 (秒) の並びと、見つけ方の数を返す。"""
    win = int(round(sr * WINDOW_S))
    nwin = max(0, (len(samples) - win) // win)
    if nwin == 0:
        return [], {"windows": 0}
    t = np.arange(win) / sr
    # ウィンドウに切った音はコピーせずに見る (20 分の録画で数百 MB のコピーを作らない)。実部と虚部を別に掛ける。
    segs = np.asarray(samples[: nwin * win], dtype=np.float32).reshape(nwin, win)

    def strength(freq):
        c = np.cos(2 * math.pi * freq * t).astype(np.float32)
        s = np.sin(2 * math.pi * freq * t).astype(np.float32)
        re = (segs @ c).astype(np.float64)
        im = (segs @ s).astype(np.float64)
        return np.sqrt(re * re + im * im)

    p1 = strength(1000)
    p2 = strength(2000)
    base1 = local_baseline(p1)
    base2 = local_baseline(p2)
    broadband = p1 > base1 * BROADBAND_RATIO
    candidate = (p2 > ABS_FLOOR) & (p2 > base2 * BURST_RATIO) & (~broadband | (p2 > p1 * 2))
    idx = np.flatnonzero(candidate)
    events = []
    rejected_runs = 0
    if len(idx) == 0:
        return events, {"windows": int(nwin), "rejected_runs": 0}
    # 切れ目が MERGE_GAP_WINDOWS ウィンドウ以下の候補をつないだ続き [start, end] の並び
    breaks = np.flatnonzero(np.diff(idx) > MERGE_GAP_WINDOWS + 1)
    starts = np.concatenate([idx[:1], idx[breaks + 1]])
    ends = np.concatenate([idx[breaks], idx[-1:]])
    for start, end in zip(starts, ends):
        seg = p2[start:end + 1]
        core = np.flatnonzero(seg >= seg.max() * ONSET_FRACTION)
        core_len = int(core[-1] - core[0] + 1)
        if BURST_MIN_WINDOWS <= core_len <= BURST_MAX_WINDOWS:
            events.append(int(start + core[0]) * win / sr)
        else:
            rejected_runs += 1
    return events, {"windows": int(nwin), "rejected_runs": rejected_runs}


def main():
    path = sys.argv[1]
    opts = parse_args(sys.argv)
    info = video_frames.probe_video(path)
    W, H = info["width"], info["height"]
    region = video_frames.clamp_region(opts["x"], opts["y"], opts["w"], opts["h"], W, H)
    if region is None:
        raise SystemExit("region is outside the video: " + json.dumps(opts))
    rx, ry, rw, rh = region

    # 1. 映像: 上の「白のマーカーの見つけ方」で白のフレームを決め、連続する白の先頭フレームの pts をマーカーとする。
    #    前の規則 (領域の平均が 200 を超える) で数えた数も並べて数える (出力の white_detector)。
    detector = WhiteDetector(rw, rh)
    white_events = []
    event_fractions = []
    old_rule_events = 0
    times = []
    prev_white = False
    prev_old_white = False
    for pts, img in video_frames.iter_gray_frames(path, W, H, region):
        times.append(pts)
        is_white, fraction = detector.feed(img)
        if is_white and not prev_white:
            white_events.append(pts)
            event_fractions.append(fraction)
        prev_white = is_white
        old_white = float(img.mean()) > OLD_WHITE_MEAN
        if old_white and not prev_old_white:
            old_rule_events += 1
        prev_old_white = old_white

    # 2. 音: 48 kHz モノラル (先頭の無音を埋めてファイルの先頭を原点にする) を 5 ms のウィンドウに切り、上の見つけ方で拾う
    sr = 48000
    samples = video_frames.read_mono_pcm(path, sr)
    tone_events, detector_counts = find_tone_events(samples, sr)

    # 3. 組にする: 最も近い相手 (0.5 秒以内) を取る
    pairs = []
    used = set()
    for tv in white_events:
        best = None
        for j, ta in enumerate(tone_events):
            if j in used:
                continue
            d = ta - tv
            if abs(d) <= PAIR_MAX_S and (best is None or abs(d) < abs(best[1])):
                best = (j, d)
        if best is not None:
            used.add(best[0])
            pairs.append({"video_s": round(tv, 4), "audio_s": round(tone_events[best[0]], 4), "audio_minus_video_ms": round(best[1] * 1000, 1)})

    offsets = [p["audio_minus_video_ms"] for p in pairs]
    trend = offset_trend(pairs)
    fps = video_frames.fps_from_pts(times)
    out = {
        "file": path, "fps": round(fps, 3) if fps else None, "r_frame_rate": info["r_frame_rate"], "time_source": "pts",
        "frames": len(times), "region": {"x": rx, "y": ry, "w": rw, "h": rh},
        "white_events": len(white_events), "tone_events": len(tone_events), "paired": len(pairs),
        "pairs": pairs[:50],
        "audio_minus_video_ms": {
            "mean": round(float(np.mean(offsets)), 1) if offsets else None,
            "stdev": round(float(np.std(offsets)), 1) if offsets else None,
            "min": round(min(offsets), 1) if offsets else None,
            "max": round(max(offsets), 1) if offsets else None,
        },
        "trend": trend,
        "detector": {
            "burst_ratio": BURST_RATIO, "broadband_ratio": BROADBAND_RATIO, "abs_floor": ABS_FLOOR, "onset_fraction": ONSET_FRACTION,
            "min_windows": BURST_MIN_WINDOWS, "max_windows": BURST_MAX_WINDOWS, **detector_counts,
        },
        # 白の見つけ方の値と様子。fraction_max は録画の全部のフレームで明るくなった点の割合の最大 (白が 0 のとき、境目に
        # どれだけ届かなかったか)。event_fraction_min は白とみなしたマーカーの中の最小。mean_over_200_events は前の規則の数。
        "white_detector": {
            "bright": WHITE_BRIGHT, "rise": WHITE_RISE, "min_fraction": WHITE_MIN_FRACTION,
            "reference_frames": WHITE_REFERENCE_FRAMES, "stride": detector.stride,
            "fraction_max": round(detector.max_fraction, 3),
            "event_fraction_min": round(min(event_fractions), 3) if event_fractions else None,
            "mean_over_200_events": old_rule_events,
        },
        "note": "値は音の時刻 - 映像の時刻。正の値 = 音が映像より遅れ、負の値 = 音が先。ITU-R BT.1359-1 の検知限は 音が先 -45 / 音が遅れ +125 ms、許容限は -90 / +185 ms。映像の時刻はフレームの pts (先頭からの秒) なので粒度は 1 フレーム (60fps で 16.7 ms、30fps で 33.3 ms)",
    }
    print(json.dumps(out, indent=1, ensure_ascii=False))


if __name__ == "__main__":
    main()
