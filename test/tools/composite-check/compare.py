#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""OBS の中で撮った合成のソースの画像と、CPU で描いた参照の画像をピクセルで比べる。

参照は decor_preview.exe の --composite-check が、製品と同じ関数で描く (このスクリプトは別の式で描き直さない)。
使い方と出力は同じフォルダーの README.md にある。サブコマンドは次の 4 つ。

  judge         run-check.mjs が書いた 1 セットのフォルダーを比べ、judge/summary.json と合わなかった所の切り抜きを書く
  gate          c1 の関門だけを見る (run-check.mjs が残りのセットを始めるかを決める。合格は終了コード 0、不合格は 3)
  sample-check  --composite-check が --composite-sample と同じ画像を描くかを確認する
  --selftest    作った画像で、一致・1 px のずれ・sRGB で混ぜた画像・色だけに掛けた画像・予備の描き方・暗さをアルファにも
                掛けた画像を見分けられるか

numpy と Pillow が要る。
"""

import argparse
import datetime
import json
import re
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

# ---- 許す差 (8 ビットの値。理由は test/tools/composite-check/README.md の「合格の基準」) ----
TOL_SINGLE = 1        # 置く物が 1 つのピクセル
TOL_OVERLAP = 2       # 置く物が 2 つ以上重なるピクセルと、明るさを合わせた画像
BIAS_LIMIT = 0.05     # 置いたピクセルの差の平均 (符号つき) の上限
BIAS_MIN_PIXELS = 500 # 偏りを見るのに要るピクセルの数
FIT_FORM_RANGE = (0.2, 0.8)  # 掛け方の形を見分けられる明るさの範囲
SLOPE_TOLERANCE = 0.15       # 明るさの時間の傾きの許す外れ (撮った時刻は要求と返事の間のどこか)
MIN_SLOW_MS = 1000           # 立ち上がりと消えるがこの長さ以上のセットでは、途中の明るさの画像が撮れるはず
MIN_MIDDLE_SHOTS = 3         # そういうセットで要る、途中の明るさ (FIT_FORM_RANGE) の画像の数
FORM_SAMPLE_PIXELS = 20000   # 退ける形で明るさを合わせるときに使うピクセルの数の上限
SHIFT_RANGE = 2              # 位置を確認するときにずらす幅 (ピクセル)
GLOW_LIT_DIFF = 16           # 光が点いたと見る、明るさ 0 の画像との差
STATS_PERIOD_S = 10.0        # 製品が統計の行を書く周期 (秒)
UPLOAD_LIMIT_MS = 5.0        # 統計の decor_upload_max_ms (コンテキストに 1 回、帯 1 つで入っていた時間の最大) の上限
SWAP_LIMIT_MS = 5.0          # 統計の decor_swap_max_ms の上限

BAD_LOG_LINES = [
    "Couldn't create the speaking glow effect",
    "Composite layout has no room",
    "Composite roster has no room",
    "Couldn't create the screen share tile",
]
LOG_LINE_RE = re.compile(r"^(\d\d):(\d\d):(\d\d)\.(\d\d\d): (.*)$")
LAYOUT_RE = re.compile(r"Composite layout canvas=(\d+)x(\d+) count=(\d+)((?: \S+=\d+,\d+,\d+,\d+)*)\s*$")
LABEL_READY_RE = re.compile(r"Composite label ready: tile=(\d+) cell=(\d+)x(\d+) paint_ms=(\d+) upload_ms=(\d+) total=(\d+)")
FRAME_READY_RE = re.compile(r"Composite frame glow ready: tile=(\d+) cell=(\d+)x(\d+) paint_ms=(\d+) upload_ms=(\d+) total=(\d+)")
ROSTER_READY_RE = re.compile(r"Composite roster label ready: index=(\d+) paint_ms=(\d+) upload_ms=(\d+) total=(\d+)")
# 並びに置いた人の数 (変わったときだけ出る行。sources/composite_decor.cpp の log_roster_shown)。
ROSTER_SHOWS_RE = re.compile(r"Composite roster shows (\d+) participant\(s\)")
STATS_RE = re.compile(r"Stats composite_decor (.*)$")

# 置く物の種類 (decor_preview の check.json の kind)。光は明るさを掛けて置き、名前ラベルは色に暗さの倍率を掛けて置く。
GLOW_KINDS = ("frame_glow", "label_glow", "roster_glow")
LABEL_KINDS = ("label", "roster_label")
ROSTER_KINDS = ("roster_glow", "roster_label")


def eprint(*args):
    print(*args, file=sys.stderr)


# ---- 画像 ----

def load_raw(path):
    """PNG の 4 つの値を、そのままの並び (R, G, B, A) の uint8 で読む。乗算済みかどうかは呼び出し元が決める。"""
    image = Image.open(path)
    if image.mode != "RGBA":
        image = image.convert("RGBA")
    return np.asarray(image, dtype=np.uint8).copy()


def save_raw(array, path):
    Image.fromarray(np.ascontiguousarray(array.astype(np.uint8))).save(path)


def over(src, dst):
    """乗算済みアルファの「元 + 下 x (1 - 元のアルファ)」(0 から 1 の float)。"""
    return src + dst * (1.0 - src[..., 3:4])


def to_byte(values):
    return np.clip(np.rint(values * 255.0), 0, 255)


# ---- OBS のログ ----

def read_log(path):
    """ログの行を (その日の秒, 本文) の並びにする。時刻のない行は前の行の時刻を使う。"""
    lines = []
    last = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            m = LOG_LINE_RE.match(raw.rstrip("\r\n"))
            if m:
                last = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) + int(m.group(4)) / 1000.0
                lines.append((last, m.group(5)))
            elif last is not None:
                lines.append((last, raw.rstrip("\r\n")))
    return lines


def parse_layout_line(text):
    """'Composite layout ...' の行から、キャンバスとタイルの矩形を読む。読めなければ None。"""
    i = text.find("Composite layout ")
    if i < 0:
        return None
    m = LAYOUT_RE.search(text[i:])
    if not m:
        return None
    tiles = {}
    for part in m.group(4).split():
        name, rect = part.split("=", 1)
        tiles[name] = [int(v) for v in rect.split(",")]
    return {"canvas": [int(m.group(1)), int(m.group(2))], "count": int(m.group(3)), "tiles": tiles,
            "line": text[i:].strip()}


def parse_stats_line(text):
    m = STATS_RE.search(text)
    if not m:
        return None
    out = {}
    for pair in m.group(1).split():
        if "=" in pair:
            key, value = pair.split("=", 1)
            try:
                out[key] = float(value)
            except ValueError:
                out[key] = value
    return out


def wall_ms_to_day_seconds(wall_ms):
    t = datetime.datetime.fromtimestamp(wall_ms / 1000.0)
    return t.hour * 3600 + t.minute * 60 + t.second + t.microsecond / 1e6


def summarize_log(lines, settled_wall_ms=None, end_wall_ms=None):
    """ログで見る行をまとめる。settled_wall_ms から end_wall_ms の間 (何も変えていない間) の描き直しも数える。"""
    out = {"bad_lines": [], "layout_lines": [], "label_ready": [], "frame_ready": [], "roster_ready": [], "roster_shows": [],
           "stats": []}
    for t, text in lines:
        if "[cordmux]" not in text and "Composite" not in text:
            continue
        for bad in BAD_LOG_LINES:
            if bad in text:
                out["bad_lines"].append(text)
        layout = parse_layout_line(text)
        if layout:
            layout["t"] = t
            out["layout_lines"].append(layout)
        m = LABEL_READY_RE.search(text)
        if m:
            out["label_ready"].append({"t": t, "tile": int(m.group(1)), "paint_ms": int(m.group(4)),
                                       "upload_ms": int(m.group(5)), "total": int(m.group(6))})
        m = FRAME_READY_RE.search(text)
        if m:
            out["frame_ready"].append({"t": t, "tile": int(m.group(1)), "paint_ms": int(m.group(4)),
                                       "upload_ms": int(m.group(5)), "total": int(m.group(6))})
        m = ROSTER_READY_RE.search(text)
        if m:
            out["roster_ready"].append({"t": t, "index": int(m.group(1)), "paint_ms": int(m.group(2)),
                                        "upload_ms": int(m.group(3)), "total": int(m.group(4))})
        m = ROSTER_SHOWS_RE.search(text)
        if m:
            out["roster_shows"].append({"t": t, "count": int(m.group(1))})
        stats = parse_stats_line(text)
        if stats is not None:
            stats["t"] = t
            out["stats"].append(stats)
    if settled_wall_ms is not None:
        start = wall_ms_to_day_seconds(settled_wall_ms)
        end = wall_ms_to_day_seconds(end_wall_ms) if end_wall_ms is not None else float("inf")
        if end < start:
            end += 86400.0
        out["repaints_while_idle"] = [r for r in out["label_ready"] + out["frame_ready"] + out["roster_ready"]
                                      if start <= r["t"] <= end]
        # 統計の行は 10 秒ごとに、その周期に数えた物を書く。落ち着く前の描き直しを数えた行が落ち着いた後に出るので、
        # 落ち着いてから 1 周期の後の行だけを見る。
        out["stats_while_idle"] = [s for s in out["stats"] if start + STATS_PERIOD_S + 0.5 <= s["t"] <= end]
    return out


def layout_settled_at(log, final_line):
    """最後の配置 (final_line) が最初に出た時刻。見つからなければ None。

    合成のソースを置いた直後は、画面共有の人がまだ枠に載っておらず (合成のソースが現れてから送信側が流し始めるまで、
    1 秒ほど)、配置が 0 人、1 人、2 人と続けて変わる。その間に描いた並びや、すぐ次の配置に置き換わった描き直しは、
    配置が落ち着いた後の約束で数えない。"""
    return next((entry["t"] for entry in log["layout_lines"] if entry["line"] == final_line), None)


def roster_ready_after_layout(log, final_line):
    """配置が落ち着いた後の、並びの名前ラベルを描き終えた行。落ち着いた時刻が分からなければ全部を返す。"""
    settled = layout_settled_at(log, final_line)
    if settled is None:
        return list(log["roster_ready"])
    return [r for r in log["roster_ready"] if r["t"] >= settled]


def check_stats(stats_lines, settled_t=None):
    """統計の行の約束 (test/tools/composite-check/README.md の「合格の基準」)。破った行を返す。

    settled_t (配置が落ち着いた時刻) より前に始まった周期の行では、decor_dropped を見ない (配置が続けて変わる間は、
    描き直しの仕事が次の仕事に置き換わって捨てられるのが正しい動きのため)。"""
    problems = []
    for s in stats_lines:
        upload = s.get("decor_upload_max_ms", 0.0)
        if upload > UPLOAD_LIMIT_MS:
            problems.append({"reason": "decor_upload_max_ms が %g ms を超えた" % UPLOAD_LIMIT_MS, **s})
        swap = s.get("decor_swap_max_ms")
        if swap is None:
            problems.append({"reason": "decor_swap_max_ms がない (この確認が前提にする製品の行ではない)", **s})
        elif swap > SWAP_LIMIT_MS:
            problems.append({"reason": "decor_swap_max_ms が %g ms を超えた" % SWAP_LIMIT_MS, **s})
        settling = settled_t is not None and "t" in s and s["t"] - STATS_PERIOD_S < settled_t
        if s.get("decor_dropped", 0) != 0 and not settling:
            problems.append({"reason": "decor_dropped が 0 でない", **s})
    return problems


# ---- 参照 (decor_preview の --composite-check) ----

def run_decor_preview(exe, args, backend):
    cmd = [str(exe)]
    if backend:
        cmd += ["--backend", backend]
    cmd += [str(a) for a in args]
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise RuntimeError("decor_preview が失敗しました (終了コード %d): %s" % (r.returncode, r.stderr.strip()))
    return r.stdout


def object_name(o):
    """置く物の名前 (summary.json の鍵)。画面共有の枠の物はタイルの番号、並びの物は置いた順の番号を付ける。"""
    return "%s-%s%d" % (o["kind"], "r" if o["kind"] in ROSTER_KINDS else "t", o["tile"])


class Reference:
    """--composite-check の出力 (check.json と画像) を読んだもの。物ごとに、誰の物か (owner。user_id) も持つ。"""

    def __init__(self, out_dir):
        self.dir = Path(out_dir)
        self.check = json.load(open(self.dir / "check.json", encoding="utf-8"))
        self.canvas = tuple(self.check["canvas"])
        w, h = self.canvas
        tiles = self.check["tiles"]
        roster = self.check.get("roster", [])
        self.objects = []
        for o in self.check["objects"]:
            x, y, cw, ch = o["clip"]
            full = load_raw(self.dir / o["file"])
            crop = full[y:y + ch, x:x + cw].copy()
            if o["kind"] in ROSTER_KINDS:
                owner = roster[o["tile"]]["user_id"] if o["tile"] < len(roster) else None
            else:
                owner = tiles[o["tile"]].get("user_id") if o["tile"] < len(tiles) else None
            self.objects.append({"kind": o["kind"], "tile": o["tile"], "clip": (x, y, cw, ch), "rgba": crop,
                                 "mask": crop.max(axis=2) > 0, "owner": owner})
        self.label_alpha = np.zeros((h, w), dtype=np.uint8)
        for o in self.objects:
            if o["kind"] in LABEL_KINDS:
                x, y, cw, ch = o["clip"]
                region = self.label_alpha[y:y + ch, x:x + cw]
                np.maximum(region, o["rgba"][..., 3], out=region)

    def render(self, name):
        for r in self.check["renders"]:
            if r["name"] == name:
                return r
        raise KeyError(name)

    def has_render(self, name):
        return any(r["name"] == name for r in self.check["renders"])

    def image(self, name):
        return load_raw(self.dir / self.render(name)["file"])

    def layers(self, name):
        """その参照で置く物を、置く順に (物, 明るさ, 暗さの倍率) の並びで返す。"""
        by_key = {(o["kind"], o["tile"]): o for o in self.objects}
        return [(by_key[(d["kind"], d["tile"])], d["level"], d.get("dim", 1.0)) for d in self.render(name)["draws"]]

    def active_objects(self, name):
        return [o for o, _level, _dim in self.layers(name)]

    def coverage(self, name):
        w, h = self.canvas
        cov = np.zeros((h, w), dtype=np.uint8)
        for o in self.active_objects(name):
            x, y, cw, ch = o["clip"]
            cov[y:y + ch, x:x + cw] += o["mask"].astype(np.uint8)
        return cov

    def tile_rect(self, index):
        return self.check["tiles"][index]["rect"]


# ---- 比べ方 ----

def rect_mask(shape, rect):
    m = np.zeros(shape, dtype=bool)
    x, y, w, h = rect
    m[max(0, y):y + h, max(0, x):x + w] = True
    return m


def object_mask(shape, o):
    """物の画像の置かれるピクセル (キャンバスの大きさの印)。"""
    m = np.zeros(shape, dtype=bool)
    x, y, w, h = o["clip"]
    m[y:y + h, x:x + w] = o["mask"]
    return m


def compare(shot, ref, cov, mask, fitted=False):
    """mask のピクセルで shot と ref を比べる。許す差は、置く物のないピクセル 0、1 つ TOL_SINGLE、2 つ以上 TOL_OVERLAP。
    fitted は、明るさを合わせて描き直した参照と比べるとき。置いたピクセルは全部 TOL_OVERLAP まで許し、偏りでは
    不合格にしない (合わせた明るさのわずかな誤差が、広い光の全部のピクセルで同じ向きの丸めの差になるため)。"""
    diff = shot.astype(np.int16) - ref.astype(np.int16)
    absd = np.abs(diff).max(axis=2)
    tol = np.where(cov == 0, 0, np.where(cov == 1, TOL_SINGLE, TOL_OVERLAP))
    if fitted:
        tol = np.where(cov == 0, 0, TOL_OVERLAP)
    over_tol = mask & (absd > tol)
    placed = mask & (cov > 0)
    bias = float(diff[placed].mean()) if placed.sum() >= BIAS_MIN_PIXELS else None
    return {
        "compared": int(mask.sum()),
        "placed": int(placed.sum()),
        "over_tolerance": int(over_tol.sum()),
        "max_diff": int(absd[mask].max()) if mask.any() else 0,
        "diff_1": int((mask & (absd == 1)).sum()),
        "bare_mismatch": int((mask & (cov == 0) & (absd > 0)).sum()),
        "bias": bias,
        "ok": bool(over_tol.sum() == 0 and (fitted or bias is None or abs(bias) <= BIAS_LIMIT)),
        "_over_mask": over_tol,
    }


def strip_private(result):
    return {k: v for k, v in result.items() if not k.startswith("_")}


# ---- 明るさと暗さの掛け方の形 ----
#
# 製品の形 (product): 光は 4 つの値すべてに明るさ level を掛けて置き、名前ラベルは色の 3 つの値だけに暗さの倍率
# f = 1 - d x (1 - level) を掛けて置く (d は話していない人の暗さの割合。声を聞けない人は 0)。退ける形は 4 つ:
#   color_only  光の色だけに明るさを掛ける (OBS の DrawMultiply の形。アルファが残る)
#   srgb        光を sRGB の光の量で混ぜる
#   fallback    効果を作れなかったときの予備の描き方 (明るさ 0.5 以上で光をそのまま置き、未満で置かない。暗くしない)
#   dim_alpha   名前ラベルの 4 つの値すべてに暗さの倍率を掛ける (名前ラベルが透けて、下の物が見える)

FORMS = ["product", "color_only", "srgb", "fallback", "dim_alpha"]


def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)


def dim_factor(level, d):
    return 1.0 - d * (1.0 - level)


def predict_stack(form, level, B, layers, d):
    """B (下の値。乗算済み、0 から 1) の上に、話す人の物 layers ((「glow」か「label」, 値) の置く順の並び。値は最大の
    明るさで暗くしていない乗算済みの 0 から 1) を、明るさ level と形 form で重ねた値。"""
    acc = B.astype(np.float64).copy()
    f = dim_factor(level, d)
    for role, X in layers:
        if role == "glow":
            if form == "color_only":
                src = X.copy()
                src[..., :3] *= level
                acc = over(src, acc)
            elif form == "srgb":
                ga = X[..., 3:4]
                straight = np.where(ga > 0, X[..., :3] / np.maximum(ga, 1e-6), 0.0)
                a = ga * level
                rgb = linear_to_srgb(srgb_to_linear(straight) * a + srgb_to_linear(acc[..., :3]) * (1.0 - a))
                acc = np.concatenate([rgb, a + acc[..., 3:4] * (1.0 - a)], axis=-1)
            elif form == "fallback":
                if level >= 0.5:
                    acc = over(X, acc)
            else:
                acc = over(X * level, acc)
        else:
            if form == "dim_alpha":
                src = X * f
            elif form == "fallback":
                src = X
            else:
                src = X.copy()
                src[..., :3] *= f
            acc = over(src, acc)
    return acc


class FitPixels:
    """途中の明るさを 1 つの数で合わせるピクセルと、そこに置く話す人の物。

    選ぶのは、製品の形で値が明るさの 1 次式になる 2 種類のピクセルである。
      光のピクセル: 話す人の光が 1 つだけ置かれ、どの名前ラベルも置かれない。値 = B + level x (G - G のアルファ x B)
      名前ラベルのピクセル: 話す人の名前ラベルのアルファが 255 で、ほかの人の名前ラベルが置かれない。色 = L x f
    どちらも背景が分かり (known)、背景のアルファが 0 か 255 のピクセルだけ。名前ラベルのピクセルは、暗さをアルファにも
    掛ける形を見分けるのに要る (光のピクセルは暗さに依らない)。"""

    def __init__(self, objects, owner, known, B, d):
        h, w = known.shape
        self.d = float(d)
        speaker = [o for o in objects if o["owner"] == owner]
        glow_count = np.zeros((h, w), dtype=np.uint8)
        speaker_alpha = np.zeros((h, w), dtype=np.uint8)
        other_labels = np.zeros((h, w), dtype=bool)
        for o in objects:
            x, y, cw, ch = o["clip"]
            if o["owner"] == owner and o["kind"] in GLOW_KINDS:
                glow_count[y:y + ch, x:x + cw] += o["mask"].astype(np.uint8)
            elif o["owner"] == owner and o["kind"] in LABEL_KINDS:
                region = speaker_alpha[y:y + ch, x:x + cw]
                np.maximum(region, o["rgba"][..., 3], out=region)
            elif o["kind"] in LABEL_KINDS:
                other_labels |= object_mask((h, w), o)
        speaker_label = np.zeros((h, w), dtype=bool)
        for o in speaker:
            if o["kind"] in LABEL_KINDS:
                speaker_label |= object_mask((h, w), o)
        clear_or_opaque = (B[..., 3] == 0) | (B[..., 3] == 255)
        base = known & clear_or_opaque & ~other_labels
        self.sel_glow = base & (glow_count == 1) & ~speaker_label
        self.sel_label = base & (speaker_alpha == 255)
        self.n_glow = int(self.sel_glow.sum())
        self.n_label = int(self.sel_label.sum())
        ys = np.concatenate([np.nonzero(self.sel_glow)[0], np.nonzero(self.sel_label)[0]])
        xs = np.concatenate([np.nonzero(self.sel_glow)[1], np.nonzero(self.sel_label)[1]])
        self.ys, self.xs = ys, xs
        self.B = B[ys, xs].astype(np.float64) / 255.0
        self.layers = []
        for o in speaker:
            x, y, cw, ch = o["clip"]
            vals = np.zeros((len(ys), 4), dtype=np.float64)
            inside = (xs >= x) & (xs < x + cw) & (ys >= y) & (ys < y + ch)
            vals[inside] = o["rgba"][ys[inside] - y, xs[inside] - x] / 255.0
            self.layers.append(("glow" if o["kind"] in GLOW_KINDS else "label", vals))

    def applicable_forms(self):
        """形を見分けられるか。光のピクセルがなければ光の形は、暗さがないか名前ラベルのピクセルがなければ暗さの形は
        製品の形と同じ値になるので、見分けない。"""
        forms = []
        if self.n_glow:
            forms += ["color_only", "srgb"]
        if self.n_glow or (self.d > 0 and self.n_label):
            forms.append("fallback")
        if self.d > 0 and self.n_label:
            forms.append("dim_alpha")
        return forms

    def product_level(self, C):
        """製品の形の明るさを最小二乗で求める (どちらの種類のピクセルも明るさの 1 次式)。"""
        ng = self.n_glow
        num = 0.0
        den = 0.0
        if ng:
            G = sum(v[:ng] for role, v in self.layers if role == "glow")
            x = G - G[:, 3:4] * self.B[:ng]
            y = C[:ng] - self.B[:ng]
            num += float((x * y).sum())
            den += float((x * x).sum())
        if self.n_label and self.d > 0:
            L = sum(v[ng:] for role, v in self.layers if role == "label")
            x = L[:, :3] * self.d
            y = C[ng:, :3] - L[:, :3] * (1.0 - self.d)
            num += float((x * y).sum())
            den += float((x * x).sum())
        if den <= 0:
            return None
        return min(1.0, max(0.0, num / den))

    def residual(self, form, level, C, pick=None):
        idx = slice(None) if pick is None else pick
        layers = [(role, v[idx]) for role, v in self.layers]
        pred = predict_stack(form, level, self.B[idx], layers, self.d)
        return float(np.abs(to_byte(pred) - to_byte(C[idx])).max()) if len(C[idx]) else None

    def search_level(self, form, C, pick):
        """製品の形以外の明るさを、二乗の誤差が最小になる値として探す。"""
        layers = [(role, v[pick]) for role, v in self.layers]
        B = self.B[pick]
        Cs = C[pick]

        def err(level):
            return float(((predict_stack(form, level, B, layers, self.d) - Cs) ** 2).sum())

        if form == "fallback":
            return min((0.0, 1.0), key=err)
        grid = np.linspace(0.0, 1.0, 41)
        errs = [err(g) for g in grid]
        k = int(np.argmin(errs))
        lo, hi = grid[max(0, k - 1)], grid[min(len(grid) - 1, k + 1)]
        for _ in range(25):
            m1, m2 = lo + (hi - lo) / 3, hi - (hi - lo) / 3
            if err(m1) < err(m2):
                hi = m2
            else:
                lo = m1
        return (lo + hi) / 2

    def sample(self):
        """退ける形を合わせるピクセルの添字。光と名前ラベルのピクセルから、それぞれ FORM_SAMPLE_PIXELS の半分まで等間隔に
        選ぶ (重い光の画像では数十万ピクセルになるため)。"""
        half = FORM_SAMPLE_PIXELS // 2
        parts = []
        for start, count in ((0, self.n_glow), (self.n_glow, self.n_label)):
            if count:
                parts.append(start + np.unique(np.linspace(0, count - 1, min(count, half)).astype(np.int64)))
        return np.concatenate(parts) if parts else np.zeros(0, dtype=np.int64)

    def fit(self, shot):
        """製品の形で明るさを合わせる。明るさが形を見分けられる範囲の近くなら、ほかの形でも合わせる。"""
        C = shot[self.ys, self.xs].astype(np.float64) / 255.0
        out = {"pixels": self.n_glow + self.n_label, "glow_pixels": self.n_glow, "label_pixels": self.n_label}
        level = self.product_level(C) if len(C) else None
        out["product"] = {"level": level, "residual": self.residual("product", level, C) if level is not None else None}
        near = level is not None and FIT_FORM_RANGE[0] - 0.05 <= level <= FIT_FORM_RANGE[1] + 0.05
        pick = self.sample() if near else None
        for form in FORMS[1:]:
            if near and form in self.applicable_forms():
                lv = self.search_level(form, C, pick)
                out[form] = {"level": lv, "residual": self.residual(form, lv, C, pick)}
            else:
                out[form] = {"level": None, "residual": None}
        out["forms_checked"] = self.applicable_forms() if near else []
        return out


def form_verdict(fit):
    """明るさが FIT_FORM_RANGE の中なら、製品の形だけが残り TOL_OVERLAP 以下で合うかを見る。"""
    level = fit["product"]["level"]
    if level is None or not (FIT_FORM_RANGE[0] <= level <= FIT_FORM_RANGE[1]):
        return None
    product_ok = fit["product"]["residual"] <= TOL_OVERLAP
    others = {f: fit[f]["residual"] for f in fit.get("forms_checked", []) if fit[f]["residual"] is not None}
    forms = ["product"] + list(others)
    return {"product_ok": product_ok, "others_rejected": all(r > TOL_OVERLAP for r in others.values()),
            "rejected_forms": sorted(f for f, r in others.items() if r > TOL_OVERLAP),
            "best": min(forms, key=lambda f: fit[f]["residual"])}


def shift_search(shot, B, layers, target, known_or_opaque):
    """target の物だけを -SHIFT_RANGE から +SHIFT_RANGE ずらして重ね直し、範囲の差の平均が最小になるずらしを返す。
    layers は (物, 明るさ, 暗さの倍率) の置く順の並び (Reference.layers)。"""
    x, y, cw, ch = target["clip"]
    pad = SHIFT_RANGE
    x0, y0 = max(0, x - pad), max(0, y - pad)
    x1, y1 = min(shot.shape[1], x + cw + pad), min(shot.shape[0], y + ch + pad)
    region_shot = shot[y0:y1, x0:x1].astype(np.float32) / 255.0
    region_b = B[y0:y1, x0:x1].astype(np.float32) / 255.0
    region_mask = known_or_opaque[y0:y1, x0:x1]
    # 比べられるピクセル (背景が分かるか、名前ラベルのアルファが 255) が無ければ、どのずらしも同じ値になるので判定しない。
    # 映像の上にある名前ラベルを、全体の不透明度を下げて描いたときがそうである。
    if not region_mask.any():
        return {"best": None, "at_zero": None, "next_best": None, "pixels": 0, "ok": None}
    scores = {}
    for dy in range(-pad, pad + 1):
        for dx in range(-pad, pad + 1):
            acc = region_b.copy()
            for o, level, dim in layers:
                if level <= 0:
                    continue
                ox, oy, ow, oh = o["clip"]
                if o is target:
                    ox += dx
                    oy += dy
                src = np.zeros_like(acc)
                ax0, ay0 = max(ox, x0), max(oy, y0)
                ax1, ay1 = min(ox + ow, x1), min(oy + oh, y1)
                if ax1 <= ax0 or ay1 <= ay0:
                    continue
                src[ay0 - y0:ay1 - y0, ax0 - x0:ax1 - x0] = o["rgba"][ay0 - oy:ay1 - oy, ax0 - ox:ax1 - ox] / 255.0 * level
                src[..., :3] *= dim
                acc = over(src, acc)
            d = np.abs(to_byte(acc) - to_byte(region_shot)).max(axis=2)
            scores[(dx, dy)] = float(d[region_mask].mean())
    best = min(scores, key=scores.get)
    others = sorted(v for k, v in scores.items() if k != (0, 0))
    return {"best": list(best), "at_zero": scores[(0, 0)], "next_best": others[0] if others else None,
            "pixels": int(region_mask.sum()), "ok": best == (0, 0) and (not others or scores[(0, 0)] < others[0])}


def value_order(shot, ref, cov, known):
    """撮った PNG の 4 つの値が、乗算済みのまま (premultiplied) か、アルファで割り戻した値 (straight) かを、背景が
    分かる半透明のピクセルから見分ける。そういうピクセルがなければ None。"""
    a = ref[..., 3].astype(np.int32)
    sel = known & (cov > 0) & (a > 0) & (a < 255)
    if not sel.any():
        return {"pixels": 0, "order": None}
    s = shot[sel].astype(np.int32)
    r = ref[sel].astype(np.int32)
    pm = int((np.abs(s - r).max(axis=1) <= TOL_OVERLAP).sum())
    straight_ref = r.copy()
    straight_ref[:, :3] = np.clip(np.rint(r[:, :3] * 255.0 / np.maximum(r[:, 3:4], 1)), 0, 255)
    st = int((np.abs(s - straight_ref).max(axis=1) <= TOL_OVERLAP).sum())
    n = int(sel.sum())
    order = "premultiplied" if pm == n else ("straight" if st == n else "neither")
    return {"pixels": n, "premultiplied_match": pm, "straight_match": st, "order": order}


# ---- 1 セットの実行を比べる ----

class Run:
    """run-check.mjs が書いた 1 セットのフォルダー (shots.json と画像と、コピーした settings.json と OBS のログ)。"""

    def __init__(self, run_dir):
        self.dir = Path(run_dir)
        self.meta = json.load(open(self.dir / "shots" / "shots.json", encoding="utf-8"))
        self.judge_dir = self.dir / "judge"
        self.judge_dir.mkdir(exist_ok=True)
        log_path = self.dir / self.meta.get("obsLog", "obs.log")
        self.log_lines = read_log(log_path) if log_path.exists() else None

    def shots(self, phase=None, prefix=None):
        out = []
        for s in self.meta["shots"]:
            if phase and s["phase"] != phase:
                continue
            if prefix and not s["label"].startswith(prefix):
                continue
            out.append(s)
        return out

    def shot_image(self, s):
        return load_raw(self.dir / "shots" / s["file"])

    def layout(self):
        if not self.log_lines:
            return None
        found = None
        for _t, text in self.log_lines:
            layout = parse_layout_line(text)
            if layout:
                found = layout
        return found

    def tile_people(self):
        return [p for p in self.meta["people"] if p["role"] in ("self", "screen")]

    def roster_people(self):
        return [p for p in self.meta["people"] if p["role"] == "roster"]

    def speakers(self, phase):
        """段階 (rise か both) で話させる人の user_id。"""
        return list(self.meta.get("speakers", {}).get(phase, []))

    def missing_avatars(self):
        """アバターの PNG があるはずの人で、ファイルがない人 (テスト用の送信側が書き出す。README の「出力」)。"""
        return [p["userId"] for p in self.meta["people"] if p.get("avatar") and not (self.dir / p["avatar"]).exists()]


def screen_slot(layout):
    if layout:
        for name in layout["tiles"]:
            if name.startswith("slot"):
                return int(name[4:])
    return 1


def avatar_path(run, p):
    return str((run.dir / p["avatar"]).resolve()) if p.get("avatar") else ""


def build_request(run, layout, renders, bg_canvas):
    """--composite-check の要求。参加者ごとに、名前、アバターの PNG、声を聞けるか (shots.json の people の hearsVoice) を渡す。"""
    people = []
    slot = screen_slot(layout)
    for p in run.tile_people():
        people.append({
            "slot": 0 if p["role"] == "self" else slot,
            "user_id": p["userId"],
            "name": p["name"],
            "avatar_png": avatar_path(run, p),
            "video": True,
            "hears_voice": bool(p.get("hearsVoice", True)),
        })
    roster = [{"user_id": p["userId"], "name": p["name"], "avatar_png": avatar_path(run, p),
               "hears_voice": bool(p.get("hearsVoice", True))} for p in run.roster_people()]
    return {
        "canvas": [1920, 1080] if not layout else layout["canvas"],
        "settings_file": str((run.dir / run.meta["settingsFile"]).resolve()),
        "self_shown": True,
        "people": people,
        "roster": roster,
        "background_png": str(bg_canvas.resolve()),
        "renders": renders,
    }


def make_render(run, name, speaking, mixer_audible=True, level=1.0, background=None):
    """参照の 1 枚。speaking の人 (user_id) の明るさを level、ほかの人を 0 にする。"""
    render = {"name": name,
              "levels": [level if p["userId"] in speaking else 0.0 for p in run.tile_people()],
              "roster_levels": [level if p["userId"] in speaking else 0.0 for p in run.roster_people()],
              "mixer_audible": mixer_audible}
    if background is not None:
        render["background_png"] = str(Path(background).resolve())
    return render


def make_background_canvas(run, layout, path):
    """自分の枠のセルに背景の PNG を置き、ほかを透明にしたキャンバス。背景が分かるピクセルの印も返す。"""
    w, h = layout["canvas"] if layout else (1920, 1080)
    canvas = np.zeros((h, w, 4), dtype=np.uint8)
    known = np.ones((h, w), dtype=bool)
    self_rect = layout["tiles"].get("self") if layout else [0, 0, w // 2, h // 2]
    bg = load_raw(run.dir / run.meta["background"]["file"])
    x, y, cw, ch = self_rect
    canvas[y:y + ch, x:x + cw] = bg[:ch, :cw]
    for name, rect in (layout["tiles"].items() if layout else []):
        if name != "self":
            known &= ~rect_mask((h, w), rect)
    save_raw(canvas, path)
    return canvas, known


def crop_diff(shot, ref, rect, path):
    x, y, w, h = rect
    x0, y0 = max(0, x - 8), max(0, y - 8)
    a = shot[y0:y + h + 8, x0:x + w + 8].astype(np.int16)
    b = ref[y0:y + h + 8, x0:x + w + 8].astype(np.int16)
    d = np.clip(np.abs(a - b).max(axis=2) * 32, 0, 255).astype(np.uint8)
    dimg = np.stack([d, d, d, np.full_like(d, 255)], axis=-1)
    sheet = np.concatenate([a.astype(np.uint8), b.astype(np.uint8), dimg], axis=1)
    save_raw(sheet, path)


def speaker_dim(ref, people, user_id):
    """話す人の名前ラベルの暗さの割合 d (0 から 1)。声を聞けない人は暗くしないので 0。"""
    person = next((p for p in people if p["userId"] == user_id), None)
    if person is None or not person.get("hearsVoice", True):
        return 0.0
    return max(0.0, min(100.0, float(ref.check.get("dim_pct", 0)))) / 100.0


def judge_run(run, exe, backend, gate_only=False):
    summary = {"set": run.meta.get("set"), "checks": {}, "failures": []}

    def fail(name, reason):
        summary["failures"].append({"check": name, "reason": reason})

    roster_count = len(run.roster_people())

    # ログ
    layout = run.layout()
    if run.log_lines is None:
        fail("log", "OBS のログがありません")
    else:
        log = summarize_log(run.log_lines, run.meta.get("settledWallMs"), run.meta.get("idleEndWallMs"))
        summary["checks"]["log"] = {
            "bad_lines": log["bad_lines"],
            "layout": layout["line"] if layout else None,
            "label_ready": len(log["label_ready"]),
            "frame_ready": len(log["frame_ready"]),
            "roster_ready": len(log["roster_ready"]),
            "roster_ready_indices": sorted({r["index"] for r in log["roster_ready"]}),
            "roster_ready_before_layout": len(log["roster_ready"])
            - len(roster_ready_after_layout(log, layout["line"] if layout else None)),
            "repaints_while_idle": len(log.get("repaints_while_idle", [])),
            "stats_problems": check_stats(log["stats"], layout_settled_at(log, layout["line"] if layout else None)),
            "stats_lines": len(log["stats"]),
            "stats_while_idle": len(log.get("stats_while_idle", [])),
            "paint_max_ms": max([s.get("decor_paint_max_ms", 0) for s in log["stats"]], default=None),
            "upload_max_ms": max([s.get("decor_upload_max_ms", 0) for s in log["stats"]], default=None),
            "swap_max_ms": max([s.get("decor_swap_max_ms", 0) for s in log["stats"]], default=None),
        }
        if log["bad_lines"]:
            fail("log", "ログに出てはいけない行があります: " + " / ".join(sorted(set(log["bad_lines"]))))
        if not layout or layout["count"] != 2 or "self" not in layout["tiles"]:
            fail("layout", "配置の行 'Composite layout ... count=2 self=...' がありません")
        # 並び: 最後に並びに置いた人の数 (Composite roster shows) が、並びの人の数と同じ (並びの人がいないセットでは 0)。並びの
        # 名前ラベルは、名前ラベルを出す人の全員 (並びの人と、枠に名前を出している画面共有の人。run-check.mjs の
        # rosterLabelCountOf) の分を描く。枠の人の分は、並びに移ったときに一瞬消えないように先に描いておくもので、置かない。
        roster_indices = {r["index"] for r in log["roster_ready"]}
        label_targets = roster_count + 1
        shows = log["roster_shows"][-1]["count"] if log["roster_shows"] else None
        summary["checks"]["log"]["roster_shows_last"] = shows
        if shows != roster_count:
            fail("roster", "最後に並びに置いた人の数 (Composite roster shows) が %s で、%d ではありません" % (shows, roster_count))
        if not set(range(label_targets)) <= roster_indices:
            fail("roster", "Composite roster label ready が index 0 から %d の全部に出ていません (%s)"
                 % (label_targets - 1, sorted(roster_indices)))
        if not gate_only:
            tiles_ready = {r["tile"] for r in log["label_ready"]}
            if not {0, 1} <= tiles_ready:
                fail("log", "Composite label ready が tile=0 と tile=1 の両方に出ていません")
            if log.get("repaints_while_idle"):
                fail("log", "何も変えていない間に描き直しました (%d 行)" % len(log["repaints_while_idle"]))
            if summary["checks"]["log"]["stats_problems"]:
                fail("stats", "統計の行の約束を破った周期があります")
            if log.get("stats_while_idle"):
                fail("stats", "何も変えていない間に統計の行が出ました (描き直しかテクスチャの上げ直し。%d 行)"
                     % len(log["stats_while_idle"]))
            if run.meta.get("expectFrameGlow") and not log["frame_ready"]:
                fail("log", "Composite frame glow ready が出ていません")
    if layout is None:
        layout = {"canvas": [1920, 1080], "count": 0,
                  "tiles": {"self": [0, 0, 960, 540], "slot1": [960, 0, 960, 540]}, "line": None}

    # アバター。テスト用の送信側が送った PNG がなければ、製品と同じ名前ラベルを描けないので、ここで止める。
    missing = run.missing_avatars()
    if missing:
        fail("avatar", "テスト用の送信側が書き出したアバターの PNG がありません: %s" % ", ".join(missing))
        summary["ok"] = False
        return summary

    rise_speakers = run.speakers("rise")
    both_speakers = run.speakers("both")

    # 決まった明るさの参照 (途中の明るさの参照は、明るさを合わせた後に描く)
    bg_canvas_path = run.judge_dir / "bg-canvas.png"
    bg_canvas, known = make_background_canvas(run, layout, bg_canvas_path)
    renders = [make_render(run, "l0", []),
               make_render(run, "l1rise", rise_speakers),
               make_render(run, "lmute", [], mixer_audible=False)]
    both_mute = run.shots("both", "both-mute-1")
    rt_rect = next((r for n, r in layout["tiles"].items() if n != "self"), None)
    if both_mute and rt_rect and not gate_only:
        bg_both = bg_canvas.copy()
        mute_img = run.shot_image(both_mute[0])
        x, y, w, h = rt_rect
        bg_both[y:y + h, x:x + w] = mute_img[y:y + h, x:x + w]
        save_raw(bg_both, run.judge_dir / "bg-both.png")
        renders.append(make_render(run, "l1both", both_speakers, background=run.judge_dir / "bg-both.png"))
    req = build_request(run, layout, renders, bg_canvas_path)
    req_path = run.judge_dir / "request.json"
    json.dump(req, open(req_path, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    run_decor_preview(exe, ["--composite-check", req_path, "--out", run.judge_dir / "ref"], backend)
    ref = Reference(run.judge_dir / "ref")
    if layout.get("line") and ref.check["layout_log_line"] != layout["line"]:
        fail("layout", "OBS の配置の行が参照の配置と違います: %s / %s" % (layout["line"], ref.check["layout_log_line"]))
    summary["checks"]["reference"] = {
        "layout_log_line": ref.check["layout_log_line"],
        "dim_pct": ref.check.get("dim_pct"),
        "tiles": [{k: t.get(k) for k in ("slot", "user_id", "hears_voice", "name_drawn", "initial_drawn",
                                          "font_family_used", "label", "glow")} for t in ref.check["tiles"]],
        "roster": [{k: r.get(k) for k in ("user_id", "hears_voice", "name_drawn", "initial_drawn", "box", "label")}
                   for r in ref.check.get("roster", [])],
        "roster_not_placed": ref.check.get("roster_not_placed"),
    }
    if len(ref.check.get("roster", [])) != roster_count:
        fail("roster", "参照が並びに置いた人は %d 人で、並びに出るはずの人 %d 人と違います"
             % (len(ref.check.get("roster", [])), roster_count))
    label_alpha = ref.label_alpha
    opaque_label = label_alpha == 255
    rt_mask = rect_mask(known.shape, rt_rect) if rt_rect else np.zeros_like(known)
    usual_mask = known | opaque_label
    both_mask = (known | rt_mask) & ((label_alpha == 0) | opaque_label | ~rt_mask)

    shot_results = []

    def judge_fixed(s, render_name, mask, fitted=False):
        img = run.shot_image(s)
        r = compare(img, ref.image(render_name), ref.coverage(render_name), mask, fitted)
        entry = {"file": s["file"], "phase": s["phase"], "label": s["label"], "against": render_name,
                 **strip_private(r)}
        if not r["ok"]:
            boxes = [("t%d" % i, t["label"]) for i, t in enumerate(ref.check["tiles"]) if t.get("label")]
            boxes += [("r%d" % i, t["label"]) for i, t in enumerate(ref.check.get("roster", [])) if t.get("label")]
            for key, rect in boxes:
                crop_diff(img, ref.image(render_name), rect,
                          run.judge_dir / ("diff-%s-%s.png" % (Path(s["file"]).stem, key)))
        shot_results.append(entry)
        return r, img

    # 背景の関門 (明るさ 0 の 1 枚で、何も置かないピクセルが完全に一致する)
    silent = run.shots("silent")
    if not silent:
        fail("background_gate", "明るさ 0 (silent) の画像がありません")
    else:
        r, silent_img = judge_fixed(silent[0], "l0", usual_mask)
        bare = known & (ref.coverage("l0") == 0)
        mism = int((np.abs(silent_img.astype(np.int16) - bg_canvas.astype(np.int16)).max(axis=2)[bare] > 0).sum())
        summary["checks"]["background_gate"] = {"bare_pixels": int(bare.sum()), "mismatch": mism}
        if mism:
            fail("background_gate", "何も置かないピクセルが背景と一致しません (%d ピクセル)。拡大か sRGB の変換か、"
                 "参照にない物 (並びの名前ラベルなど) があります" % mism)
        image_shots = run.shots("settle", "image-source")
        if image_shots:
            img = run.shot_image(image_shots[0])
            bg = load_raw(run.dir / run.meta["background"]["file"])
            hh, ww = min(img.shape[0], bg.shape[0]), min(img.shape[1], bg.shape[1])
            summary["checks"]["image_source"] = {
                "size": [int(img.shape[1]), int(img.shape[0])],
                "max_diff_vs_png": int(np.abs(img[:hh, :ww].astype(np.int16) - bg[:hh, :ww].astype(np.int16)).max())}

    # 光が点いたか (立ち上がり rise の終わりまでに、話す人の光が)。ほかの人の物が重なる所では、どちらが点いたか分からない
    # ので見ない。
    owner_masks = {}
    for o in ref.objects:
        m = owner_masks.setdefault(o["owner"], np.zeros(known.shape, dtype=bool))
        m |= object_mask(known.shape, o)
    glow_objs = [o for o in ref.objects if o["kind"] in GLOW_KINDS and o["owner"] in rise_speakers]
    lit = []
    if silent:
        for s in run.shots("rise"):
            img = run.shot_image(s)
            for o in glow_objs:
                x, y, w, h = o["clip"]
                others = np.zeros((h, w), dtype=bool)
                for owner, m in owner_masks.items():
                    if owner != o["owner"]:
                        others |= m[y:y + h, x:x + w]
                sel = o["mask"] & known[y:y + h, x:x + w] & (label_alpha[y:y + h, x:x + w] == 0) & ~others
                if not sel.any():
                    continue
                d = np.abs(img[y:y + h, x:x + w].astype(np.int16) - silent_img[y:y + h, x:x + w].astype(np.int16)).max(axis=2)
                if int(d[sel].max()) >= GLOW_LIT_DIFF and object_name(o) not in lit:
                    lit.append(object_name(o))
    summary["checks"]["glow_lit_by_rise_end"] = {"objects": lit, "glow_objects": len(glow_objs),
                                                 "speakers": rise_speakers}
    if glow_objs and not lit:
        fail("glow_lit", "立ち上がり (rise) の終わりまでに、話す人の光が点きませんでした")

    # PNG の値の並び
    orders = []
    for s in run.shots("speaking") + run.shots("both", "both-unmute"):
        name = "l1rise" if s["phase"] == "speaking" else "l1both"
        if not ref.has_render(name):
            continue
        orders.append(value_order(run.shot_image(s), ref.image(name), ref.coverage(name), known))
    order_names = {o["order"] for o in orders if o["order"]}
    summary["checks"]["value_order"] = orders
    if "neither" in order_names:
        fail("value_order", "撮った PNG の値の並びが、乗算済みとも割り戻した値とも合いません")

    if gate_only:
        summary["ok"] = not summary["failures"]
        return summary

    # 決まった明るさの画像。ミュートと音量 0 の間は、光らせず暗くもしない (lmute)。
    for s in run.shots("speaking"):
        judge_fixed(s, "l1rise", usual_mask)
    for s in run.shots("mute"):
        judge_fixed(s, "lmute" if s["label"] in ("mute", "vol0") else "l1rise", usual_mask)
    for s in run.shots("both", "both-mute"):
        judge_fixed(s, "lmute", usual_mask)
    for s in run.shots("both", "both-unmute"):
        if ref.has_render("l1both"):
            judge_fixed(s, "l1both", both_mask)
    mutes = run.shots("both", "both-mute")
    if len(mutes) >= 2 and rt_rect:
        a, b = run.shot_image(mutes[0]), run.shot_image(mutes[1])
        x, y, w, h = rt_rect
        same = int(np.abs(a[y:y + h, x:x + w].astype(np.int16) - b[y:y + h, x:x + w].astype(np.int16)).max())
        summary["checks"]["frames_stopped"] = {"rt_max_diff_between_mutes": same}
        if same:
            fail("frames_stopped", "フレームを止めた後の 2 枚の右上が違います (止まっていない)")
    scene = run.shots("scene")
    vol1 = run.shots("mute", "vol1")
    if scene and vol1:
        sa, sb = run.shot_image(scene[0]), run.shot_image(vol1[0])
        d = np.abs(sa.astype(np.int16) - sb.astype(np.int16)).max(axis=2)
        summary["checks"]["scene_vs_source"] = {"max_diff_outside_rt": int(d[~rt_mask].max())}
        judge_fixed(scene[0], "l1rise", usual_mask)
        if int(d[~rt_mask].max()):
            fail("scene", "シーン Main の画像が合成のソースの画像と違います")

    # 物ごとに分けた比べ方。まとめたセット (枠の光と、余白 0 の名前ラベルの光など) で違いが出たときに、どの物の
    # ピクセルかを分けられるように、明るさ 1 の画像で、その物だけが置かれたピクセルと、セルの外へ出たピクセルを比べる。
    # 並びの物はセルに属さないので、キャンバスの外だけを外と数える。
    if run.shots("speaking"):
        img = run.shot_image(run.shots("speaking")[0])
        ref_img = ref.image("l1rise")
        cov = ref.coverage("l1rise")
        canvas_rect = [0, 0, known.shape[1], known.shape[0]]
        separation = {}
        active = ref.active_objects("l1rise")
        d = np.abs(img.astype(np.int16) - ref_img.astype(np.int16)).max(axis=2)
        for o in active:
            mask = object_mask(known.shape, o)
            only = mask & usual_mask & (cov == 1)
            cell = rect_mask(known.shape, canvas_rect if o["kind"] in ROSTER_KINDS else ref.tile_rect(o["tile"]))
            outside = mask & usual_mask & ~cell
            entry = {"owner": o["owner"], "only_pixels": int(only.sum()),
                     "only_max_diff": int(d[only].max()) if only.any() else None,
                     "outside_cell_pixels": int(outside.sum()),
                     "outside_cell_max_diff": int(d[outside].max()) if outside.any() else None}
            for other in active:
                if other is o or other["owner"] != o["owner"]:
                    continue
                entry["overlap_with_%s" % other["kind"]] = int((mask & object_mask(known.shape, other)).sum())
            separation[object_name(o)] = entry
        summary["checks"]["separation"] = separation

    # 位置 (名前ラベルをずらして比べる。並びの名前ラベルも)
    if run.shots("speaking"):
        img = run.shot_image(run.shots("speaking")[0])
        layers = ref.layers("l1rise")
        shifts = {}
        for o in ref.objects:
            if o["kind"] not in LABEL_KINDS:
                continue
            r = shift_search(img, bg_canvas, layers, o, usual_mask)
            shifts[object_name(o)] = r
            if r["ok"] is False:
                fail("position", "名前ラベル (%s) は (0, 0) より %s のずらしで合います" % (object_name(o), r["best"]))
        summary["checks"]["position"] = shifts

    # 途中の明るさ。立ち上がりと消えるの間は、話す人 1 人だけの光と名前ラベルの明るさが変わる (ほかの人は黙っている)。
    fits = []
    timing = run.meta.get("timing", {})
    if len(rise_speakers) != 1:
        fail("fade", "立ち上がりで話す人が 1 人ではありません (%s)" % rise_speakers)
    else:
        speaker = rise_speakers[0]
        others_silent = [(o, level, dim) for o, level, dim in ref.layers("l0") if o["owner"] != speaker]
        silent_bg = bg_canvas.astype(np.float64) / 255.0
        # 合わせるピクセルの下の値は、背景にほかの人の物 (黙っている人の名前ラベル) を重ねた値。合わせるピクセルは
        # ほかの人の名前ラベルを除くので、ここで重ねる物は、光の広がりの下にある物だけである。
        for o, level, dim in others_silent:
            x, y, w, h = o["clip"]
            src = o["rgba"].astype(np.float64) / 255.0 * level
            src[..., :3] *= dim
            silent_bg[y:y + h, x:x + w] = over(src, silent_bg[y:y + h, x:x + w])
        base_bytes = to_byte(silent_bg).astype(np.uint8)
        fitter = FitPixels([o for o in ref.objects if o["owner"] == speaker or o["kind"] in LABEL_KINDS],
                           speaker, known, base_bytes, speaker_dim(ref, run.meta["people"], speaker))
        summary["checks"]["fit_pixels"] = {"glow": fitter.n_glow, "label": fitter.n_label, "d": fitter.d,
                                           "speaker": speaker}
        for phase in ("rise", "fade"):
            for s in run.shots(phase):
                f = fitter.fit(run.shot_image(s))
                f.update({"file": s["file"], "phase": phase,
                          "t_ms": (s["reqWallMs"] + s["resWallMs"]) / 2.0 - s.get("toneChangeWallMs", s["reqWallMs"]),
                          "verdict": form_verdict(f)})
                fits.append(f)
        renders2 = []
        for i, f in enumerate(fits):
            level = f["product"]["level"]
            if level is not None and 0.0 < level < 1.0:
                renders2.append(make_render(run, "fit%d" % i, [speaker], level=level))
        if renders2:
            req2 = build_request(run, layout, renders2, bg_canvas_path)
            req2_path = run.judge_dir / "request-fit.json"
            json.dump(req2, open(req2_path, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
            run_decor_preview(exe, ["--composite-check", req2_path, "--out", run.judge_dir / "ref-fit"], backend)
            ref2 = Reference(run.judge_dir / "ref-fit")
            for i, f in enumerate(fits):
                name = "fit%d" % i
                if not ref2.has_render(name):
                    continue
                img = run.shot_image({"file": f["file"]})
                r = compare(img, ref2.image(name), ref2.coverage(name), usual_mask, fitted=True)
                f["redrawn"] = strip_private(r)
                if not r["ok"]:
                    fail("fade", "%s は合わせた明るさ %.3f で描き直した参照と合いません (最大 %d)" %
                         (f["file"], f["product"]["level"], r["max_diff"]))
    verdicts = [f["verdict"] for f in fits if f["verdict"]]
    for v in verdicts:
        if not v["product_ok"]:
            fail("fade_form", "途中の明るさで、製品の掛け方が残り %d 以下で合いません" % TOL_OVERLAP)
            break
    fade_shots = []
    for f in fits:
        entry = {k: f[k] for k in ("file", "phase", "t_ms", "pixels", "glow_pixels", "label_pixels", "forms_checked",
                                   "verdict", *FORMS)}
        if "redrawn" in f:
            entry["redrawn_max"] = f["redrawn"]["max_diff"]
        fade_shots.append(entry)
    summary["checks"]["fade"] = {
        "shots": fade_shots,
        "form_checked": len(verdicts),
        "others_rejected": all(v["others_rejected"] for v in verdicts) if verdicts else None,
        "slopes": fade_slopes(fits, timing),
    }
    slopes = summary["checks"]["fade"]["slopes"]
    for key in ("rise", "fade"):
        sl = slopes.get(key)
        if sl and sl.get("ok") is False:
            fail("fade_time", "%s: %s" % (key, sl.get("reason", "明るさの時間の傾きが設定と合いません")))

    # GetStats
    stats = run.meta.get("getStats", {})
    if "before" in stats and "after" in stats:
        skipped = stats["after"].get("renderSkippedFrames", 0) - stats["before"].get("renderSkippedFrames", 0)
        summary["checks"]["get_stats"] = {"render_skipped_delta": skipped,
                                          "average_frame_render_time": stats["after"].get("averageFrameRenderTime")}
        if skipped:
            fail("get_stats", "撮らない 10 秒の間に renderSkippedFrames が %d 増えました" % skipped)

    summary["shots"] = shot_results
    for e in shot_results:
        if not e["ok"]:
            fail("pixels", "%s が %s と合いません (許す差を超えたピクセル %d、最大 %d、偏り %s)" %
                 (e["file"], e["against"], e["over_tolerance"], e["max_diff"], e["bias"]))
    summary["ok"] = not summary["failures"]
    return summary


def fade_slopes(fits, timing):
    """立ち上がりと消える途中の明るさを時刻に並べた傾きと、設定の時間から決まる傾きを比べる。"""
    out = {}
    for key, phase, ms_key in (("rise", "rise", "riseMs"), ("fade", "fade", "fadeMs")):
        pts = [(f["t_ms"], f["product"]["level"]) for f in fits
               if f["phase"] == phase and f["product"]["level"] is not None and 0.05 < f["product"]["level"] < 0.95]
        expected_ms = timing.get(ms_key)
        middle = sum(1 for _t, v in pts if FIT_FORM_RANGE[0] <= v <= FIT_FORM_RANGE[1])
        if expected_ms and expected_ms >= MIN_SLOW_MS and middle < MIN_MIDDLE_SHOTS:
            # 時間を長くしたセットで途中の明るさが現れないのは、明るさを掛けずに 0 か 1 で置く予備の描き方の形である。
            out[key] = {"points": len(pts), "middle": middle, "ok": False,
                        "reason": "途中の明るさの画像が %d 枚しかありません" % middle}
            continue
        if len(pts) < 2 or not expected_ms:
            out[key] = {"points": len(pts), "ok": None}
            continue
        t = np.array([p[0] for p in pts])
        v = np.array([p[1] for p in pts])
        slope = float(np.polyfit(t, v, 1)[0])
        expected = (1.0 if key == "rise" else -1.0) / float(expected_ms)
        out[key] = {"points": len(pts), "slope_per_ms": slope, "expected_per_ms": expected,
                    "ok": abs(slope - expected) <= abs(expected) * SLOPE_TOLERANCE}
    return out


def write_summary(run, summary, name):
    path = run.judge_dir / name
    json.dump(summary, open(path, "w", encoding="utf-8"), ensure_ascii=False, indent=1, default=float)
    return path


# ---- --composite-check と --composite-sample が同じ画像を描くか ----

SAMPLE_PEOPLE = [
    # (slot, 名前, アバターの色相。None は画像なし)。decor_preview の composite_scene.cpp の見本の 4 人と同じ。
    (0, "Mika", 200),
    (1, "Noa Kimura", 20),
    (2, "たろう", None),
    (3, "Alexander Featherstonehaugh-Montgomery", 120),
]
SAMPLE_BACKGROUNDS = ["dark-game", "bright-sky", "stripes", "snowy-field"]


def sample_check(exe, work, backend):
    work = Path(work)
    work.mkdir(parents=True, exist_ok=True)
    for kind in SAMPLE_BACKGROUNDS:
        run_decor_preview(exe, ["--composite-check-background", kind, "960x540", work / ("bg-%s.png" % kind)], backend)
    for _slot, _name, hue in SAMPLE_PEOPLE:
        if hue is not None:
            run_decor_preview(exe, ["--composite-check-avatar", str(hue), work / ("avatar-%d.png" % hue)], backend)
    canvas = np.zeros((1080, 1920, 4), dtype=np.uint8)
    canvas[..., 3] = 255
    for kind, (x, y) in zip(SAMPLE_BACKGROUNDS, [(0, 0), (960, 0), (0, 540), (960, 540)]):
        canvas[y:y + 540, x:x + 960] = load_raw(work / ("bg-%s.png" % kind))
    save_raw(canvas, work / "bg-canvas.png")
    (work / "settings.json").write_text("{}\n", encoding="utf-8")
    people = []
    for slot, name, hue in SAMPLE_PEOPLE:
        # 見本は話していない人を暗くしない (枠の明るさだけを描く) ので、全員を声を聞けない人にして暗さを掛けない。
        p = {"slot": slot, "user_id": str(slot), "name": name, "hears_voice": False}
        if hue is not None:
            p["avatar_png"] = str((work / ("avatar-%d.png" % hue)).resolve())
        people.append(p)
    results = []
    # 見本の --speaker は枠の位置 (タイルの並び)。見本の配置では右上が Alexander (people の 4 人目)、左上が自分。
    for speaker, levels in (("1", [0, 0, 0, 1]), ("none", [0, 0, 0, 0]), ("0", [1, 0, 0, 0])):
        req = {"canvas": [1920, 1080], "settings_file": str((work / "settings.json").resolve()), "people": people,
               "background_png": str((work / "bg-canvas.png").resolve()),
               "renders": [{"name": "s", "levels": levels}]}
        req_path = work / ("request-%s.json" % speaker)
        json.dump(req, open(req_path, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
        run_decor_preview(exe, ["--composite-check", req_path, "--out", work / ("check-%s" % speaker)], backend)
        run_decor_preview(exe, ["--composite-sample", "--out", work / "sample", "--tag", "s" + speaker,
                                "--speaker", speaker], backend)
        a = load_raw(work / ("check-%s" % speaker) / "ref-s.png")[..., :3].astype(np.int16)
        b = load_raw(work / "sample" / ("composite_s%s.png" % speaker))[..., :3].astype(np.int16)
        d = np.abs(a - b).max(axis=2)
        results.append({"speaker": speaker, "diff_pixels": int((d > 0).sum()), "max_diff": int(d.max())})
    return results


# ---- セルフテスト ----

def synthetic_scene(w=160, h=90, clear=False):
    """縞の背景 (clear なら透明)、四角い名前ラベル、そのまわりのぼけた光 (どれも乗算済みの uint8)。"""
    B = np.zeros((h, w, 4), dtype=np.uint8)
    if not clear:
        for x in range(w):
            B[:, x] = (240, 140, 40, 255) if (x // 6) % 2 else (30, 120, 130, 255)
    yy, xx = np.mgrid[0:h, 0:w]
    label = np.zeros((h, w, 4), dtype=np.uint8)
    inside = (xx >= 50) & (xx < 110) & (yy >= 35) & (yy < 55)
    edge = ((xx == 49) | (xx == 110)) & (yy >= 35) & (yy < 55)
    label[inside] = (255, 255, 255, 255)
    label[edge] = (128, 128, 128, 128)
    dist = np.sqrt(np.maximum(0, np.maximum(50 - xx, xx - 109)) ** 2 + np.maximum(0, np.maximum(35 - yy, yy - 54)) ** 2)
    ga = np.clip(1.0 - dist / 18.0, 0, 1) * 0.9
    ga[inside] = 0
    color = np.array([0x57, 0xF2, 0x87], dtype=np.float32) / 255.0
    glow = np.zeros((h, w, 4), dtype=np.float32)
    glow[..., :3] = color * ga[..., None]
    glow[..., 3] = ga
    glow = to_byte(glow).astype(np.uint8)
    return B, label, glow


def synthetic_objects(label, glow, roster=False, owner="speaker"):
    h, w = label.shape[:2]
    kinds = ("roster_glow", "roster_label") if roster else ("label_glow", "label")
    return [{"kind": kinds[0], "tile": 0, "clip": (0, 0, w, h), "rgba": glow, "mask": glow.max(axis=2) > 0,
             "owner": owner},
            {"kind": kinds[1], "tile": 0, "clip": (0, 0, w, h), "rgba": label, "mask": label.max(axis=2) > 0,
             "owner": owner}]


def synthetic_shot(form, level, B, label, glow, shift=(0, 0), d=0.0):
    L = np.roll(label, shift=(shift[1], shift[0]), axis=(0, 1)) / 255.0
    return to_byte(predict_stack(form, level, B / 255.0, [("glow", glow / 255.0), ("label", L)], d)).astype(np.uint8)


def synthetic_layers(objects, level, d):
    """synthetic_objects を、その明るさの参照の (物, 明るさ, 暗さの倍率) の並びにする。"""
    return [(o, level if o["kind"] in GLOW_KINDS else 1.0, dim_factor(level, d) if o["kind"] in LABEL_KINDS else 1.0)
            for o in objects]


def selftest():
    failures = []

    def check(cond, name):
        print(("ok    " if cond else "NG    ") + name)
        if not cond:
            failures.append(name)

    B, label, glow = synthetic_scene()
    objects = synthetic_objects(label, glow)
    known = np.ones(B.shape[:2], dtype=bool)
    cov = np.zeros(B.shape[:2], dtype=np.uint8)
    for o in objects:
        cov += o["mask"].astype(np.uint8)
    ref = synthetic_shot("product", 0.5, B, label, glow)
    plain = FitPixels(objects, "speaker", known, B, 0.0)

    # 一致
    shot = synthetic_shot("product", 0.5, B, label, glow)
    r = compare(shot, ref, cov, known)
    check(r["ok"] and r["max_diff"] == 0, "一致: 製品と同じ式の画像は許す差の中で合う")
    f = plain.fit(shot)
    v = form_verdict(f)
    check(abs(f["product"]["level"] - 0.5) < 0.01 and v and v["product_ok"] and v["others_rejected"],
          "一致: 明るさ 0.5 に合い、ほかの形は退けられる")
    check("dim_alpha" not in f["forms_checked"], "暗さがなければ、暗さをアルファにも掛ける形は見分けない")

    # 1 px のずれ
    shifted = synthetic_shot("product", 1.0, B, label, glow, shift=(1, 0))
    ref1 = synthetic_shot("product", 1.0, B, label, glow)
    r = compare(shifted, ref1, cov, known)
    check(not r["ok"], "1 px のずれ: 許す差を超える")
    s = shift_search(shifted, B, synthetic_layers(objects, 1.0, 0.0), objects[1], known)
    check(s["best"] == [1, 0] and not s["ok"], "1 px のずれ: ずらしの探しが (1, 0) を返す")
    s0 = shift_search(ref1, B, synthetic_layers(objects, 1.0, 0.0), objects[1], known)
    check(s0["best"] == [0, 0] and s0["ok"], "ずれのない画像では、ずらしの探しが (0, 0) を返す")
    s_none = shift_search(ref1, B, synthetic_layers(objects, 1.0, 0.0), objects[1], np.zeros_like(known))
    check(s_none["ok"] is None and s_none["pixels"] == 0, "比べられるピクセルが無ければ、ずらしの探しは判定しない")

    # 掛け方の形の違い
    for form, name in (("srgb", "sRGB で混ぜた画像"), ("color_only", "色だけに掛けた画像"),
                       ("fallback", "予備の描き方")):
        shot = synthetic_shot(form, 0.5 if form != "fallback" else 0.6, B, label, glow)
        r = compare(shot, ref, cov, known)
        check(not r["ok"], name + ": 明るさ 0.5 の参照と合わない")
        f = plain.fit(shot)
        v = form_verdict(f)
        if form == "fallback":
            # 予備の描き方は明るさ 1 で置くので、製品の形で合わせると 1 になる (途中の明るさにならない)。
            C = shot[plain.ys, plain.xs] / 255.0
            fallback_residual = plain.residual("fallback", plain.search_level("fallback", C, np.arange(len(C))), C)
            check(f["product"]["level"] > FIT_FORM_RANGE[1] and fallback_residual <= TOL_OVERLAP,
                  name + ": 途中の明るさが現れず、予備の描き方で合う")
        else:
            check(v is not None and not v["product_ok"] and v["best"] == form,
                  name + ": 製品の形では合わず、その形がいちばん合う")

    # 暗さ: 話していない人の名前ラベルの色を f = 1 - d x (1 - level) 倍にする。明るさで光と名前ラベルが一緒に変わる。
    d = 0.3
    dimmed = FitPixels(objects, "speaker", known, B, d)
    check(dimmed.n_label > 0 and dimmed.n_glow > 0, "暗さ: 光のピクセルと名前ラベルのピクセルの両方を選ぶ")
    ref_d = synthetic_shot("product", 0.5, B, label, glow, d=d)
    f = dimmed.fit(ref_d)
    v = form_verdict(f)
    check(abs(f["product"]["level"] - 0.5) < 0.01 and v and v["product_ok"] and v["others_rejected"]
          and "dim_alpha" in v["rejected_forms"],
          "暗さ: 製品の形で明るさ 0.5 に合い、暗さをアルファにも掛ける形を含めてほかの形は退けられる")
    shot = synthetic_shot("dim_alpha", 0.5, B, label, glow, d=d)
    r = compare(shot, ref_d, cov, known)
    check(not r["ok"], "暗さをアルファにも掛けた画像: 製品の形の参照と合わない")
    f = dimmed.fit(shot)
    v = form_verdict(f)
    check(v is not None and not v["product_ok"] and v["best"] == "dim_alpha",
          "暗さをアルファにも掛けた画像: 製品の形では合わず、その形がいちばん合う")
    # 光だけで明るさを合わせると、名前ラベルの暗さを見ない (暗くしていない名前ラベルでも同じ明るさに合う)。名前ラベルの
    # ピクセルも使えば、暗くしていない画像は製品の形で合わない。
    undimmed = synthetic_shot("product", 0.5, B, label, glow, d=0.0)
    f = dimmed.fit(undimmed)
    check(f["product"]["residual"] > TOL_OVERLAP, "暗さ: 名前ラベルを暗くしていない画像は、暗さを入れた製品の形で合わない")
    s = shift_search(ref_d, B, synthetic_layers(objects, 0.5, d), objects[1], known)
    check(s["best"] == [0, 0] and s["ok"], "暗さ: 暗くした名前ラベルでも、ずらしの探しが (0, 0) を返す")

    # 並び: 透明の上 (並びは映像の枠の外に置く) に置いた、画面共有していない参加者の名前ラベルと光。
    Bc, label_c, glow_c = synthetic_scene(clear=True)
    roster_objs = synthetic_objects(label_c, glow_c, roster=True)
    roster_fit = FitPixels(roster_objs, "speaker", known, Bc, d)
    shot = synthetic_shot("product", 0.4, Bc, label_c, glow_c, d=d)
    f = roster_fit.fit(shot)
    v = form_verdict(f)
    check(abs(f["product"]["level"] - 0.4) < 0.01 and v and v["product_ok"] and v["others_rejected"],
          "並び: 透明の上でも明るさ 0.4 に合い、ほかの形は退けられる")
    shot = synthetic_shot("dim_alpha", 0.4, Bc, label_c, glow_c, d=d)
    f = roster_fit.fit(shot)
    v = form_verdict(f)
    check(v is not None and not v["product_ok"] and v["best"] == "dim_alpha",
          "並び: 暗さをアルファにも掛けた画像は、透明の上ではアルファの値で見分ける")
    other = synthetic_objects(label_c, glow_c, roster=True, owner="other")
    check(FitPixels(other, "speaker", known, Bc, d).n_glow == 0,
          "並び: ほかの人の物は、合わせるピクセルに入れない")

    # 明るさの時間の傾き。4000 ms で立ち上がるセットで、途中の明るさが傾きどおりに並ぶ / 0 と 1 しか現れない。
    smooth = [{"phase": "rise", "t_ms": t, "product": {"level": t / 4000.0}} for t in range(400, 3700, 300)]
    sl = fade_slopes(smooth, {"riseMs": 4000, "fadeMs": 4000})
    check(sl["rise"]["ok"] is True and abs(sl["rise"]["slope_per_ms"] - 1 / 4000.0) < 1e-6,
          "明るさの時間: 傾き 1/4000 ms の並びを合格にする")
    steep = [{"phase": "rise", "t_ms": t, "product": {"level": t / 2000.0}} for t in range(200, 1900, 150)]
    check(fade_slopes(steep, {"riseMs": 4000})["rise"]["ok"] is False, "明るさの時間: 傾きが 2 倍なら不合格にする")
    jumps = [{"phase": "rise", "t_ms": t, "product": {"level": 0.0 if t < 2000 else 1.0}} for t in range(200, 4000, 300)]
    check(fade_slopes(jumps, {"riseMs": 4000})["rise"]["ok"] is False, "予備の描き方: 途中の明るさが現れなければ不合格にする")

    # 値の並び (半透明のピクセル)
    ref_t = to_byte(predict_stack("product", 1.0, np.zeros_like(B, dtype=np.float64), [("glow", glow / 255.0)],
                                  0.0)).astype(np.uint8)
    cov_t = (glow.max(axis=2) > 0).astype(np.uint8)
    o = value_order(ref_t.copy(), ref_t, cov_t, known)
    check(o["order"] == "premultiplied", "値の並び: 乗算済みのままの PNG を見分ける")
    st = ref_t.copy()
    a = np.maximum(st[..., 3:4].astype(np.int32), 1)
    st[..., :3] = np.clip(np.rint(st[..., :3].astype(np.int32) * 255.0 / a), 0, 255).astype(np.uint8)
    o = value_order(st, ref_t, cov_t, known)
    check(o["order"] == "straight", "値の並び: アルファで割り戻した PNG を見分ける")

    # ログ
    sample = ["16:49:32.123: [cordmux] Composite layout canvas=1920x1080 count=2 self=0,0,960,540 slot1=960,0,960,540",
              "16:49:33.000: [cordmux] Composite label ready: tile=0 cell=960x540 paint_ms=5 upload_ms=0 total=1 "
              "strips=2 upload_max_ms=0.40",
              "16:49:33.100: [cordmux] Composite roster label ready: index=1 paint_ms=3 upload_ms=0 total=2",
              "16:49:33.200: [cordmux] Composite roster shows 1 participant(s)",
              "16:49:34.000: [cordmux] Composite roster shows 0 participant(s)",
              "16:49:40.000: [cordmux] Stats composite_decor decor_submitted=2 decor_painted=2 decor_dropped=0 "
              "decor_paint_max_ms=12.0 decor_uploads=2 decor_upload_max_ms=1.00 decor_swap_max_ms=0.10",
              "16:49:50.000: [cordmux] Stats composite_decor decor_submitted=1 decor_painted=1 decor_dropped=0 "
              "decor_paint_max_ms=12.0 decor_uploads=1 decor_upload_max_ms=1.00 decor_swap_max_ms=7.50",
              "16:50:00.000: [cordmux] Stats composite_decor decor_submitted=1 decor_painted=1 decor_dropped=0 "
              "decor_paint_max_ms=12.0 decor_uploads=1 decor_upload_max_ms=1.00",
              "16:50:01.000: [cordmux] Composite roster has no room for 2 participant(s)"]
    lines = []
    for s in sample:
        m = LOG_LINE_RE.match(s)
        lines.append((int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) + int(m.group(4)) / 1000.0,
                      m.group(5)))
    log = summarize_log(lines)
    check(log["layout_lines"] and log["layout_lines"][0]["tiles"]["slot1"] == [960, 0, 960, 540],
          "ログ: 配置の行を読む")
    check(log["label_ready"][0]["total"] == 1 and log["roster_ready"][0]["index"] == 1,
          "ログ: 名前ラベルと並びの名前ラベルを描き終えた行を読む (末尾の帯の数と 1 回の最大は読み飛ばす)")
    check([r["count"] for r in log["roster_shows"]] == [1, 0],
          "ログ: 並びに置いた人の数の行を読む (最後の値で、並びの人の数と比べる)")
    early = [(1.0, "[cordmux] Composite layout canvas=1920x1080 count=1 self=0,0,1920,1080"),
             (1.1, "[cordmux] Composite roster label ready: index=0 paint_ms=4 upload_ms=0 total=1"),
             (1.5, "[cordmux] Composite layout canvas=1920x1080 count=2 self=0,0,960,540 slot1=960,0,960,540"),
             (2.0, "[cordmux] Composite roster label ready: index=1 paint_ms=4 upload_ms=0 total=2")]
    early_log = summarize_log(early)
    final = early_log["layout_lines"][-1]["line"]
    check([r["index"] for r in roster_ready_after_layout(early_log, final)] == [1]
          and len(roster_ready_after_layout(early_log, None)) == 2,
          "ログ: 配置が落ち着く前の並びの名前ラベルは、並びの人がいないことの判定に数えない")
    dropped = {"decor_dropped": 1.0, "decor_upload_max_ms": 0.1, "decor_swap_max_ms": 0.0}
    check(not check_stats([{**dropped, "t": 11.0}], 1.5) and len(check_stats([{**dropped, "t": 25.0}], 1.5)) == 1
          and len(check_stats([{**dropped, "t": 11.0}])) == 1,
          "ログ: 配置が落ち着く前に始まった周期の捨てた描き直しは数えず、落ち着いた後の周期と時刻が分からないときは数える")
    problems = check_stats(log["stats"])
    check(not check_stats(log["stats"][:1]) and len(problems) == 2
          and any("decor_swap_max_ms が" in p["reason"] for p in problems)
          and any("decor_swap_max_ms がない" in p["reason"] for p in problems),
          "ログ: 統計の行の、置き換えの時間の上限と、その値のない行を見分ける")
    check(any("Composite roster has no room" in b for b in log["bad_lines"]), "ログ: 並びに入りきらない行を出てはいけない行にする")

    print("セルフテスト: %d 件の失敗" % len(failures))
    return 0 if not failures else 1


# ---- 入口 ----

def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except AttributeError:
            pass
    parser = argparse.ArgumentParser(description="OBS で撮った合成のソースの画像と、CPU の参照の画像をピクセルで比べる")
    parser.add_argument("--selftest", action="store_true", help="作った画像で見分けられるかを確認する")
    sub = parser.add_subparsers(dest="command")
    for name, text in (("judge", "1 セットを比べる"), ("gate", "c1 の関門だけを見る")):
        p = sub.add_parser(name, help=text)
        p.add_argument("--run", required=True, help="run-check.mjs が書いた 1 セットのフォルダー")
        p.add_argument("--decor-preview", required=True, help="decor_preview.exe のパス")
        p.add_argument("--backend", default="windows",
                       help="参照を描く Qt のプラットフォームプラグイン (既定は OBS と同じ windows)")
    p = sub.add_parser("sample-check", help="--composite-check が --composite-sample と同じ画像を描くか")
    p.add_argument("--decor-preview", required=True)
    p.add_argument("--work", required=True, help="作業のフォルダー")
    p.add_argument("--backend", default="")
    args = parser.parse_args(argv)

    if args.selftest:
        return selftest()
    if args.command in ("judge", "gate"):
        run = Run(args.run)
        summary = judge_run(run, Path(args.decor_preview), args.backend, gate_only=args.command == "gate")
        path = write_summary(run, summary, "gate.json" if args.command == "gate" else "summary.json")
        for f in summary["failures"]:
            print("不合格: [%s] %s" % (f["check"], f["reason"]))
        print(("合格: " if summary["ok"] else "不合格: ") + str(path))
        if args.command == "gate":
            return 0 if summary["ok"] else 3
        return 0 if summary["ok"] else 1
    if args.command == "sample-check":
        results = sample_check(Path(args.decor_preview), args.work, args.backend)
        for r in results:
            print("--speaker %s: 違うピクセル %d、最大の差 %d" % (r["speaker"], r["diff_pixels"], r["max_diff"]))
        return 0 if all(r["diff_pixels"] == 0 for r in results) else 1
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
