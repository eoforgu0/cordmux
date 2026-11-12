#!/usr/bin/env python3
"""plate_measure.py: 名前ラベルのプレート (暗い帯) の位置と高さ、プレートのピクセルの重ね方を、PNG か録画のフレームから測る。

用途は 2 つある。
  - OBS の中で倍率を知る方法 (gs_draw_quadf) で描いたプレートが、途中で解像度が変わったフレーム・入れ子のシーン・
    配信の画面の大きさの変更のあとでも、決まった高さと位置に出るかを、シーンの GetSourceScreenshot の PNG や
    OBS の録画のフレームから数で読む (目で 1 px を見分けない)。
  - 合成のソース cordmux の名前ラベルのプレート (label_show_plate を入れたとき。色は label_plate_color、不透明度は
    label_plate_opacity_pct) は、ソースの絵の中に描かれ、OBS の色ソースなどほかのソースには何も描かない。
    そのためプレートは、合成が置かれた画面の背景 (--bg に渡す色。ダミー表示の画面 1 の背景
    sources/dummy_tiles.h の 0x7A2E2E など、背景が分かる絵) の上で読む。プレートのピクセルを読み、値の上で重ねた (sRGB の値に
    1 - 不透明度を掛けた。不透明度 75% なら R は約 31) か、光の量の上で重ねた (線形の光に掛けた。同じく R は約 62) かを
    分ける。白の背景なら 64 と 137 になる同じ式である。

見つけ方 (プレートの 4 辺): プレートは黒の 75% (このツールの想定。合成の既定は 50% なので、測るときは label_plate_opacity_pct を 75 にして
撮る) なので、プレートの中のピクセルは背景の 1/4 前後まで暗くなる。背景からプレートへ
入る境目では、k ピクセル外側のピクセルに比べて ratio (既定 0.45) 以下に暗いピクセルが、境目に沿って長く並ぶ。上の辺は「上へ k
ピクセル外側のピクセルより暗い」ピクセルの横の並びがいちばん長い行、下の辺は「下へ k ピクセル」、左右の辺は列で同じ。プレートの中の文字 (白) と
アバター (色) は余白の内側にあるので辺には掛からない。角の丸み (NL の radius_ratio 0.25) は並びを少し縮めるだけ。
背景が暗すぎる (外側のピクセルが min_bg 未満) 所はプレートが見えないので数えない (見つからなければ found:false。黙って値を作らない)。

暗いピクセルの連結成分の外接の四角は使わない (背景が暗い所で背景とつながり、プレートより大きな四角を返す。暗い背景の絵で
実際に起きる)。決まったしきい値の暗さだけで読む方法も、同じ理由で使わない。

使い方:
    python plate_measure.py IMAGE.png [--region x,y,w,h] [--bg RRGGBB] [--transfer srgb|pq]
    python plate_measure.py --video REC.mkv [--every-s 1.0 | --at-s 3.5,10] [--region ...] [--ffmpeg PATH]
    python plate_measure.py --selftest
出力: 1 行の JSON。plate: {left, top, right, bottom, width, height} (端のピクセルを含む。画像の座標)、plateRgb と bgRgb
(プレートの上の余白の帯と、プレートのすぐ上の背景の中央値)、blend: {ratioMeasured, ratioValueSpace, ratioLightSpace, nearer}。
判定はしない (数を出すだけ)。

numpy と Pillow を使う (video_frames.py は numpy)。一時のファイルを書かない。
"""

import argparse
import json
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# プレートの黒の不透明度 (0xBF = 191/255 = 75%。合成の設定では label_plate_opacity_pct を 75 にしたときの値)。
PLATE_ALPHA = 0xBF / 255.0
# ダミー表示の画面 1 の背景 (sources/dummy_tiles.h の dummy_tile_color(1))。
DUMMY_SCREEN1_RGB = (0x7A, 0x2E, 0x2E)
# 外側に比べてこの割合以下の暗さなら境目とみなす。光の量の上で重ねたプレートは sRGB の値で背景の約 0.5 (白の背景で 137/255 = 0.54、
# ダミーの背景で 62/122 = 0.51) にしかならないので、0.62 にした (値の上なら約 0.25)。
DEFAULT_RATIO = 0.62
# 並びの中の 1 ピクセルの切れ目は埋める (録画の圧縮の揺れと、背景の細かい模様。2 ピクセル以上は埋めない = 背景の模様をつなげない)。
DEFAULT_GAP = 1
DEFAULT_K = 3
DEFAULT_MIN_BG = 24
DEFAULT_MIN_RUN = 24


def load_rgb(path):
    """PNG を (高さ, 幅, 3) の uint8 と、透明度の面 (透明度が無ければ None) で返す。透明なピクセルは白の上に重ねた色にする
    (合成のソースの絵は何も描かない所が透明で、その所の値は 0 のまま。値だけで読むと、透明の上のプレート (黒の 75%) が背景と同じ 0 で
    見つからない)。"""
    from PIL import Image

    with Image.open(path) as im:
        if im.mode in ("RGBA", "LA") or "transparency" in im.info:
            rgba = np.asarray(im.convert("RGBA"))
            a = rgba[:, :, 3:4].astype(np.float32) / 255.0
            over_white = np.rint(rgba[:, :, :3].astype(np.float32) * a + 255.0 * (1.0 - a)).astype(np.uint8)
            return np.ascontiguousarray(over_white), rgba[:, :, 3]
        return np.asarray(im.convert("RGB")), None


def _longest_run(mask_row):
    """1 次元の真偽の並びの、いちばん長い真の並び (長さ, 始まり)。"""
    if not mask_row.any():
        return 0, -1
    padded = np.concatenate(([False], mask_row, [False]))
    d = np.diff(padded.astype(np.int8))
    starts = np.flatnonzero(d == 1)
    ends = np.flatnonzero(d == -1)
    lengths = ends - starts
    i = int(np.argmax(lengths))
    return int(lengths[i]), int(starts[i])


def _edge_runs(luma, axis, direction, ratio, k, min_bg):
    """行 (axis 0) か列 (axis 1) ごとの、境目の条件に当たるピクセルのいちばん長い並び。戻り値: (長さの配列, 始まりの配列, 番号のずれ)。
    direction -1 = 外側が小さい番号の側 (上・左)。"""
    a = luma if axis == 0 else luma.T
    n = a.shape[0]
    if n <= 2 * k:
        return np.zeros(0, int), np.zeros(0, int), 0
    if direction < 0:
        inside, outside, offset = a[k:], a[:-k], k
    else:
        inside, outside, offset = a[:-k], a[k:], 0
    cond = (inside <= ratio * outside) & (outside >= min_bg)
    # 1 ピクセルの切れ目を埋める (両隣が真なら真)。
    if DEFAULT_GAP >= 1 and cond.shape[1] >= 3:
        filled = cond.copy()
        filled[:, 1:-1] |= cond[:, :-2] & cond[:, 2:]
        cond = filled
    runs = [_longest_run(cond[i]) for i in range(cond.shape[0])]
    return np.array([r[0] for r in runs]), np.array([r[1] for r in runs]), offset


def _edge_candidates(lengths, starts, offset, direction, min_run):
    """境目の候補 (つながった行のまとまりごとに 1 つ)。境目では k 行 (列) 続けて条件に当たるので、まとまりの外側にいちばん近い行
    (上・左の辺なら最小、下・右なら最大) を辺とする。角の丸みで辺のいちばん外の行は並びが短くなる (半径 r の丸みで
    約 2r - 2 sqrt(r) 縮む) ので、まとまりはまとまりの中のいちばん長い並びの半分までを含める。戻り値: [(辺の番号, 長さ, 始まり)]。"""
    out = []
    i = 0
    n = len(lengths)
    while i < n:
        if lengths[i] < min_run:
            i += 1
            continue
        j = i
        while j + 1 < n and lengths[j + 1] >= min_run:
            j += 1
        peak_len = int(lengths[i : j + 1].max())
        lo, hi = i, j
        while lo - 1 >= 0 and lengths[lo - 1] >= 0.5 * peak_len:
            lo -= 1
        while hi + 1 < n and lengths[hi + 1] >= 0.5 * peak_len:
            hi += 1
        idx = lo if direction < 0 else hi
        out.append((idx + offset, int(lengths[idx]), int(starts[idx]), peak_len))
        i = max(j, hi) + 1
    return out


def _edge(luma, axis, direction, ratio, k, min_bg, min_run):
    """いちばん長い並びの境目 1 つ (左右の辺に使う)。戻り値: (辺の番号, 長さ, 始まり) か None。"""
    lengths, starts, offset = _edge_runs(luma, axis, direction, ratio, k, min_bg)
    cands = _edge_candidates(lengths, starts, offset, direction, min_run)
    if not cands:
        return None
    best = max(cands, key=lambda c: c[3])
    return best[0], best[1], best[2]


def _overlap(a_start, a_len, b_start, b_len):
    lo = max(a_start, b_start)
    hi = min(a_start + a_len, b_start + b_len)
    return max(0, hi - lo) / float(max(1, min(a_len, b_len)))


def _top_bottom(luma, ratio, k, min_bg, min_run):
    """上の辺と下の辺の組。上の辺 (下へ暗くなる) と下の辺 (下へ明るくなる) の候補から、上が下より上にあり、横の並びが
    重なり (短いほうの 6 割以上)、長さが近い (短いほうが長いほうの 6 割以上) 組のうち、短いほうの並びがいちばん長い組を選ぶ。
    1 辺だけの境目 (枠と枠の境目、背景の段差) は組にならない (2x2 の枠の境目で実際に拾った)。"""
    tl, ts, to = _edge_runs(luma, 0, -1, ratio, k, min_bg)
    bl, bs, bo = _edge_runs(luma, 0, 1, ratio, k, min_bg)
    tops = _edge_candidates(tl, ts, to, -1, min_run)
    bottoms = _edge_candidates(bl, bs, bo, 1, min_run)
    best = None
    for t_ in tops:
        for b_ in bottoms:
            if b_[0] - t_[0] < 4:
                continue
            shorter, longer = sorted((t_[3], b_[3]))
            if shorter < 0.6 * longer or _overlap(t_[2], t_[1], b_[2], b_[1]) < 0.6:
                continue
            score = (shorter, -(b_[0] - t_[0]))
            if best is None or score > best[0]:
                best = (score, t_, b_)
    if best is None:
        return None, None
    return best[1][:3], best[2][:3]


def find_plate(rgb, region=None, ratio=DEFAULT_RATIO, k=DEFAULT_K, min_bg=DEFAULT_MIN_BG, min_run=DEFAULT_MIN_RUN, transfer="srgb"):
    """プレートの 4 辺 (画像の座標。端のピクセルを含む) を返す。見つからなければ None。
    transfer pq (HDR の録画のフレーム): PQ の値は暗さの比を縮める (プレートの 1/4 の光が値では約 0.75 倍) ので、線形の光に戻して
    から sRGB に近い曲がり (1/2.2 乗) で読み直し、探す場所の明るい所 (99 パーセンタイル) を 255 に揃えて探す。"""
    h, w = rgb.shape[:2]
    x0, y0, rw, rh = region if region else (0, 0, w, h)
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(w, x0 + rw), min(h, y0 + rh)
    luma = rgb[y0:y1, x0:x1].max(axis=2).astype(np.float32)
    if transfer == "pq":
        lin = pq_to_linear(luma)
        top_lin = float(np.percentile(lin, 99)) if lin.size else 0.0
        luma = (255.0 * np.clip(lin / top_lin, 0.0, 1.0) ** (1.0 / 2.2)).astype(np.float32) if top_lin > 0 else np.zeros_like(luma)
    top, bottom = _top_bottom(luma, ratio, k, min_bg, min_run)
    if top is None or bottom is None or bottom[0] <= top[0]:
        return None
    # 左右の辺は、上下の辺の間の行で、上下の辺の並びの端の近くだけを探す (背景のほかの縦の境目を拾わない。ダミーの背景の
    # 動く白い線は縦に全部の行を通るので、行を絞るだけではプレートの縦の辺より長い並びになる)。角の丸み (高さの 1/4) の分
    # 上下の辺の並びは縮むので、端から外へ高さの 0.4 倍 + 4 ピクセルまで見る。
    band_h = bottom[0] - top[0] + 1
    band = luma[top[0] : bottom[0] + 1]
    run_start = min(top[2], bottom[2])
    run_end = max(top[2] + top[1], bottom[2] + bottom[1])
    reach = int(0.4 * band_h) + 4
    min_col_run = max(4, int(0.5 * band_h))
    lx0 = max(0, run_start - reach - k)
    lx1 = min(band.shape[1], run_start + 4 + k)
    rx0 = max(0, run_end - 4 - k)
    rx1 = min(band.shape[1], run_end + reach + k)
    left = _edge(band[:, lx0:lx1], 1, -1, ratio, k, min_bg, min_col_run)
    right = _edge(band[:, rx0:rx1], 1, 1, ratio, k, min_bg, min_col_run)
    if left is None or right is None or rx0 + right[0] <= lx0 + left[0]:
        return None
    box = {"left": x0 + lx0 + left[0], "top": y0 + top[0], "right": x0 + rx0 + right[0], "bottom": y0 + bottom[0]}
    box["width"] = box["right"] - box["left"] + 1
    box["height"] = box["bottom"] - box["top"] + 1
    return box


# 名前ラベルの隅 (名前ラベルの既定は左下)。探す場所を、枠のその隅の高さ 5 割・幅 8 割に絞る (枠の中のほかの暗い四角 =
# ゲームの画面の HUD の帯や、チャットの欄を拾わない)。
CORNER_FRACTION_H = 0.5
CORNER_FRACTION_W = 0.8


def corner_region(box, corner):
    """枠 (x, y, w, h) の隅 (bl / br / tl / tr) の探す場所 (ピクセルの整数に丸める)。"""
    x, y, w, h = (int(round(v)) for v in box)
    rw = int(round(w * CORNER_FRACTION_W))
    rh = int(round(h * CORNER_FRACTION_H))
    rx = x if corner in ("bl", "tl") else x + w - rw
    ry = y + h - rh if corner in ("bl", "br") else y
    return (rx, ry, rw, rh)


# 合成のソースの絵 (画面共有が 2 人以上なら 4 等分) の各領域の下の帯。名前ラベルは領域の下 (左下と右下の並び) にある。絵全体を探すと、
# 画面共有の中身の暗い大きな四角 (テスト用のパターンの帯やブラウザのウィンドウの枠) の組が先に選ばれて、プレートを取り逃がす。
BAND_FRACTION_H = 0.2


def band_regions(width, height, cols, rows):
    """絵を cols x rows に割った各領域の、下の帯 (x, y, w, h) の並び。領域は左上から行の順。"""
    out = []
    for r in range(rows):
        for c in range(cols):
            x0, x1 = c * width // cols, (c + 1) * width // cols
            y0, y1 = r * height // rows, (r + 1) * height // rows
            bh = max(1, int(round((y1 - y0) * BAND_FRACTION_H)))
            out.append((x0, y1 - bh, x1 - x0, bh))
    return out


def measure_bands(rgb, alpha, cols, rows, transfer="srgb", known_bg=None):
    """各領域の下の帯でプレートを探し、見つかったプレートを並べる。plate は見つかったプレートのうち幅がいちばん大きい物 (名前ラベルのプレート。
    名前を外すとアバターだけの小さなプレートが残る)。plates は領域の番号つき。見つからなければ found:false。"""
    h, w = rgb.shape[:2]
    plates = []
    for i, region in enumerate(band_regions(w, h, cols, rows)):
        box = find_plate(rgb, region, transfer=transfer)
        if box is not None:
            plates.append({"cell": i, **box})
    widest = max(plates, key=lambda p: p["width"]) if plates else None
    out = {"found": widest is not None, "plate": ({k: v for k, v in widest.items() if k != "cell"} if widest else None), "plates": plates}
    if widest is not None:
        plate_rgb, bg_rgb = sample_colors(rgb, widest)
        out["plateRgb"] = plate_rgb
        out["bgRgb"] = bg_rgb
    return out


def srgb_to_linear(v):
    v = np.asarray(v, dtype=np.float64) / 255.0
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(l):
    l = np.clip(np.asarray(l, dtype=np.float64), 0.0, 1.0)
    return 255.0 * np.where(l <= 0.0031308, l * 12.92, 1.055 * l ** (1.0 / 2.4) - 0.055)


def pq_to_linear(v):
    """PQ (SMPTE ST 2084) の 8 ビットの値から、10000 nits を 1 とした線形の光。"""
    m1, m2 = 2610 / 16384, 2523 / 4096 * 128
    c1, c2, c3 = 3424 / 4096, 2413 / 4096 * 32, 2392 / 4096 * 32
    e = np.clip(np.asarray(v, dtype=np.float64) / 255.0, 0.0, 1.0) ** (1.0 / m2)
    return (np.maximum(e - c1, 0.0) / (c2 - c3 * e)) ** (1.0 / m1)


def _luminance(lin_rgb):
    lin_rgb = np.asarray(lin_rgb, dtype=np.float64)
    return float(0.2126 * lin_rgb[0] + 0.7152 * lin_rgb[1] + 0.0722 * lin_rgb[2])


def expected_blend(bg_rgb, alpha=PLATE_ALPHA):
    """背景の sRGB の値から、プレートの 2 つの重ね方のプレートの値 (sRGB) と、光の量の比 (プレート / 背景) を返す。"""
    bg = np.asarray(bg_rgb, dtype=np.float64)
    value = bg * (1.0 - alpha)
    light = linear_to_srgb(srgb_to_linear(bg) * (1.0 - alpha))
    y_bg = _luminance(srgb_to_linear(bg))
    return {
        "valueSpaceRgb": [round(float(c), 1) for c in value],
        "lightSpaceRgb": [round(float(c), 1) for c in light],
        "ratioValueSpace": round(_luminance(srgb_to_linear(value)) / y_bg, 4) if y_bg > 0 else None,
        "ratioLightSpace": round(1.0 - alpha, 4),
    }


def sample_colors(rgb, box):
    """プレートの上の余白の帯 (文字とアバターの外) と、プレートのすぐ上の背景の、色の中央値。ダミーの背景の動く白い線は中央値で外れる。"""
    h = box["height"]
    inset_x = max(2, int(round(h * 0.3)))
    pad = max(1, int(round(h * 0.1)))
    xs = slice(box["left"] + inset_x, box["right"] - inset_x + 1)
    plate = rgb[box["top"] + 1 : box["top"] + 1 + pad, xs]
    above = rgb[max(0, box["top"] - 10) : max(0, box["top"] - 3), xs]
    med = lambda a: [int(v) for v in np.median(a.reshape(-1, 3), axis=0)] if a.size else None  # noqa: E731
    return med(plate), med(above)


def blend_report(plate_rgb, bg_rgb, transfer="srgb", known_bg=None):
    """測ったプレートと背景の色から、光の量の比 (プレート / 背景) を出し、2 つの重ね方の期待とどちらが近いかを書く。"""
    if plate_rgb is None or bg_rgb is None:
        return None
    decode = pq_to_linear if transfer == "pq" else srgb_to_linear
    y_plate = _luminance(decode(plate_rgb))
    y_bg = _luminance(decode(bg_rgb))
    exp = expected_blend(known_bg if known_bg is not None else bg_rgb)
    out = {"transfer": transfer, "ratioMeasured": round(y_plate / y_bg, 4) if y_bg > 0 else None, **exp}
    if out["ratioMeasured"] and out["ratioMeasured"] > 0:
        dv = abs(math.log(out["ratioMeasured"] / exp["ratioValueSpace"]))
        dl = abs(math.log(out["ratioMeasured"] / exp["ratioLightSpace"]))
        out["nearer"] = "value-space" if dv < dl else "light-space"
        out["logDistance"] = {"valueSpace": round(dv, 3), "lightSpace": round(dl, 3)}
    return out


def measure_image(rgb, alpha=None, region=None, transfer="srgb", known_bg=None):
    box = find_plate(rgb, region, transfer=transfer)
    out = {"found": box is not None, "plate": box}
    if box is not None:
        plate_rgb, bg_rgb = sample_colors(rgb, box)
        out["plateRgb"] = plate_rgb
        out["bgRgb"] = bg_rgb
        out["blend"] = blend_report(plate_rgb, bg_rgb, transfer, known_bg)
        if alpha is not None:
            a = alpha[box["top"] : box["bottom"] + 1, box["left"] : box["right"] + 1]
            out["plateAlphaMin"] = int(a.min())
            if int(a.min()) < 250:
                out["warning"] = "the plate is not opaque in this PNG (the source texture was not drawn?); colors are read without alpha"
    return out


# ---- 録画のフレーム ------------------------------------------------------------


def measure_video(path, region=None, every_s=None, at_s=None, ffmpeg="ffmpeg", ffprobe="ffprobe", transfer="srgb", known_bg=None, corner=None):
    """録画から決めた時刻のフレーム (at_s) か、every_s 秒ごとのフレームを読み、フレームごとに measure_image を当てる。
    corner を渡すと、region (無ければフレーム全体) のその隅だけを探す。"""
    import video_frames

    info = video_frames.probe_video(path, ffprobe=ffprobe)
    w, h = info["width"], info["height"]
    if corner:
        region = corner_region(region or (0, 0, w, h), corner)
    frames = []
    wanted = sorted(at_s) if at_s else None
    next_t = 0.0
    for pts, rgb in video_frames.iter_frames(path, w, h, ffmpeg=ffmpeg, pix_fmt="rgb24"):
        take = False
        if wanted:
            if pts >= wanted[0]:
                take = True
                wanted.pop(0)
        elif every_s:
            if pts + 1e-6 >= next_t:
                take = True
                next_t = pts + every_s
        if take:
            m = measure_image(rgb, None, region, transfer, known_bg)
            frames.append({"pts": round(pts, 3), **m})
        if wanted is not None and not wanted:
            break
    return {"file": path, "width": w, "height": h, "frames": frames}


# ---- セルフテスト (作り物の絵。ファイルを書かない) --------------------------------------


def _synthetic(seed, bg_kind, plate, text=True, avatar=True, alpha=PLATE_ALPHA, blend="value"):
    """作り物の 1 枚。plate = (left, top, width, height)。角の丸みは高さの 1/4 (NL の radius_ratio)。"""
    rng = np.random.default_rng(seed)
    h, w = 360, 640
    if bg_kind == "flat":
        img = np.zeros((h, w, 3), np.float64) + np.array(DUMMY_SCREEN1_RGB, np.float64)
        img[:, 300:306] = 255.0  # ダミーの動く白い線
    elif bg_kind == "harness":
        # テスト用の送信側の画面 (背景 0x30 の灰色に、左下の大きな白い数字)。
        img = np.zeros((h, w, 3), np.float64) + 48.0
        img[200:250, 30:150] = 255.0
    elif bg_kind == "gradient":
        img = np.dstack([np.tile(np.linspace(60, 230, w), (h, 1))] * 3) * np.array([1.0, 0.9, 0.8])
    else:  # noise
        img = rng.uniform(70, 230, (h, w, 3))
    l, t, pw, ph = plate
    r = ph * 0.25
    yy, xx = np.mgrid[0:h, 0:w]
    inside = (xx >= l) & (xx < l + pw) & (yy >= t) & (yy < t + ph)
    for cx, cy in ((l + r, t + r), (l + pw - r, t + r), (l + r, t + ph - r), (l + pw - r, t + ph - r)):
        corner = ((xx < l + r) if cx < l + pw / 2 else (xx >= l + pw - r)) & ((yy < t + r) if cy < t + ph / 2 else (yy >= t + ph - r))
        inside &= ~(corner & ((xx + 0.5 - cx) ** 2 + (yy + 0.5 - cy) ** 2 > r * r))
    if blend == "value":
        img[inside] = img[inside] * (1.0 - alpha)
    else:
        img[inside] = linear_to_srgb(srgb_to_linear(img[inside]) * (1.0 - alpha))
    pad = max(2, ph // 6)
    if avatar:
        ar = (ph - 2 * pad) / 2
        acx, acy = l + pad + ar, t + ph / 2
        img[((xx - acx) ** 2 + (yy - acy) ** 2) <= ar * ar] = (90, 140, 220)
    if text:
        x = int(l + ph)
        while x + 6 < l + pw - pad:
            img[t + pad : t + ph - pad, x : x + 4] = 255.0
            x += 9
    return np.clip(np.rint(img), 0, 255).astype(np.uint8)


def selftest():
    failures = []

    def check(cond, label):
        print(("ok   " if cond else "FAIL ") + label)
        if not cond:
            failures.append(label)

    for seed, bg, plate in ((1, "flat", (20, 290, 224, 57)), (2, "gradient", (40, 250, 180, 40)), (3, "noise", (12, 300, 300, 48)),
                            (4, "noise", (100, 120, 120, 30)), (5, "gradient", (8, 320, 90, 24)), (10, "harness", (12, 300, 190, 48))):
        img = _synthetic(seed, bg, plate)
        box = find_plate(img)
        l, t, pw, ph = plate
        ok = box is not None and abs(box["left"] - l) <= 1 and abs(box["top"] - t) <= 1 and abs(box["width"] - pw) <= 1 and abs(box["height"] - ph) <= 1
        check(ok, "finds the plate %s on a %s background (got %s)" % (plate, bg, box and (box["left"], box["top"], box["width"], box["height"])))
    # プレートが無ければ見つけない (黙って値を作らない)。
    plain = _synthetic(6, "gradient", (0, 0, 0, 0), text=False, avatar=False)
    check(find_plate(plain) is None, "no plate is reported on a background without a plate")
    # 背景が暗すぎてプレートが見えない所は数えない。
    dark = np.zeros((200, 300, 3), np.uint8) + 10
    check(find_plate(dark) is None, "a near-black background reports no plate")
    # 探す場所を絞ると、絞った外のプレートは見ない。
    img = _synthetic(7, "noise", (20, 290, 224, 57))
    check(find_plate(img, (300, 0, 340, 200)) is None, "a region that excludes the plate finds nothing")
    box = find_plate(img, (0, 200, 640, 160))
    check(box is not None and box["top"] == 290, "a region that includes the plate finds it at image coordinates")
    # ダミーの画面 1 の背景の上のプレートの重ね方。値の上なら R 約 31、光の量の上なら R 約 62。
    exp = expected_blend(DUMMY_SCREEN1_RGB)
    check(abs(exp["valueSpaceRgb"][0] - 30.6) < 0.6 and abs(exp["lightSpaceRgb"][0] - 62.3) < 0.8, "expected plate R on 0x7A2E2E: value-space 31, light-space 62")
    white = expected_blend((255, 255, 255))
    check(abs(white["valueSpaceRgb"][0] - 64) < 0.5 and abs(white["lightSpaceRgb"][0] - 137) < 1.0, "expected plate on white: 64 and 137 (appendix F)")
    for blend, want in (("value", "value-space"), ("light", "light-space")):
        img = _synthetic(8, "flat", (20, 290, 224, 57), blend=blend)
        m = measure_image(img)
        got = m["blend"]["nearer"] if m["found"] and m["blend"] else None
        check(got == want, "a %s-blended plate on the dummy background reads as %s (got %s, plate %s)" % (blend, want, got, m.get("plateRgb")))
    # PQ の値にした同じ絵でも、光の量の比は変わらない (比で分けるので HDR の画面でも読める)。
    img = _synthetic(9, "flat", (20, 290, 224, 57), blend="light")
    lin = srgb_to_linear(img) * (200.0 / 10000.0)
    m1, m2 = 2610 / 16384, 2523 / 4096 * 128
    c1, c2, c3 = 3424 / 4096, 2413 / 4096 * 32, 2392 / 4096 * 32
    pq = 255.0 * ((c1 + c2 * lin ** m1) / (1 + c3 * lin ** m1)) ** m2
    pq_img = np.clip(np.rint(pq), 0, 255).astype(np.uint8)
    m = measure_image(pq_img, transfer="pq", known_bg=DUMMY_SCREEN1_RGB)
    check(m["found"] and m["blend"]["nearer"] == "light-space", "the same light-space plate in PQ code values reads as light-space (%s)" % (m.get("blend") or {}).get("ratioMeasured"))
    # 合成のソースの絵の形: 上に大きな黒い帯 (テスト用のパターン)、下に名前ラベルのプレート。透明な所のプレートも白の上の色で読む。
    img = _synthetic(11, "noise", (20, 290, 224, 57)).copy()
    img[10:90, 0:400] = 0
    wide = measure_bands(img, None, 1, 1)
    check(wide["found"] and abs(wide["plate"]["width"] - 224) <= 1, "the bottom band finds the name label plate under a big black bar (got %s)" % (wide["plate"],))
    small = _synthetic(11, "noise", (20, 290, 57, 57)).copy()
    small[10:90, 0:400] = 0
    narrow = measure_bands(small, None, 1, 1)
    check(narrow["found"] and narrow["plate"]["width"] < 0.85 * wide["plate"]["width"], "an avatar-only plate reads narrower than the name label plate")
    check(not measure_bands(_synthetic(12, "noise", (0, 0, 0, 0), text=False, avatar=False), None, 1, 1)["found"], "the bottom band reports nothing without a plate")
    bands = band_regions(1920, 1080, 2, 2)
    check(bands[0] == (0, 432, 960, 108) and bands[3] == (960, 972, 960, 108), "the bands of a 2x2 grid are the bottom 20% of each cell")
    with_alpha = np.zeros((120, 200, 4), np.uint8)
    with_alpha[40:80, 30:150] = (0, 0, 0, 191)
    from PIL import Image
    import io

    buf = io.BytesIO()
    Image.fromarray(with_alpha).save(buf, "PNG")
    buf.seek(0)
    over, a = load_rgb(buf)
    check(int(over[0, 0, 0]) == 255 and abs(int(over[60, 60, 0]) - 64) <= 1 and int(a.min()) == 0, "transparent pixels read as white and a 75% black plate as 64")
    print("selftest: %d failure(s)" % len(failures))
    return 1 if failures else 0


def _ints(text, n):
    parts = [int(p) for p in text.split(",")]
    if len(parts) != n:
        raise argparse.ArgumentTypeError("expected %d comma separated integers" % n)
    return tuple(parts)


def main(argv):
    if argv[1:] == ["--selftest"]:
        return selftest()
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", nargs="?")
    parser.add_argument("--video", default=None)
    parser.add_argument("--every-s", type=float, default=None)
    parser.add_argument("--at-s", default=None, help="comma separated seconds")
    parser.add_argument("--region", default=None, type=lambda t: _ints(t, 4), help="x,y,w,h to search in (the tile)")
    parser.add_argument("--corner", choices=["bl", "br", "tl", "tr"], default=None,
                        help="search only that corner of the region (height 50%%, width 80%%; the label corner, default bl)")
    parser.add_argument("--tile-bands", default=None, help="COLSxROWS: search the bottom band of each cell of that grid (the composite source picture is 2x2) and report the widest plate")
    parser.add_argument("--bg", default=None, help="known background RRGGBB (e.g. 7A2E2E for the dummy screen 1)")
    parser.add_argument("--transfer", choices=["srgb", "pq"], default="srgb")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    args = parser.parse_args(argv[1:])
    known_bg = None
    if args.bg:
        h = args.bg.lstrip("#")
        known_bg = (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16))
    if args.video:
        at = [float(v) for v in args.at_s.split(",")] if args.at_s else None
        if not at and not args.every_s:
            args.every_s = 1.0
        out = measure_video(args.video, args.region, args.every_s, at, args.ffmpeg, args.ffprobe, args.transfer, known_bg, args.corner)
        print(json.dumps(out, ensure_ascii=False))
        return 0
    if not args.image:
        parser.error("give an image, --video or --selftest")
    rgb, alpha = load_rgb(args.image)
    if args.tile_bands:
        cols, rows = (int(v) for v in args.tile_bands.lower().split("x"))
        out = {"file": args.image, "width": int(rgb.shape[1]), "height": int(rgb.shape[0]), **measure_bands(rgb, alpha, cols, rows, args.transfer, known_bg)}
        print(json.dumps(out, ensure_ascii=False))
        return 0
    if args.corner:
        args.region = corner_region(args.region or (0, 0, int(rgb.shape[1]), int(rgb.shape[0])), args.corner)
    out = {"file": args.image, "width": int(rgb.shape[1]), "height": int(rgb.shape[0]), **measure_image(rgb, alpha, args.region, args.transfer, known_bg)}
    print(json.dumps(out, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
