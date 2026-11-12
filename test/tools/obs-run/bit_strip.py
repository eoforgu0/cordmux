# 共有の中身 (share/pattern.html と make_sync_video.py の sync-video.mp4) が描く「時計つきの帯」の描き方・探し方・
# 読み方。frames.py と smooth.py が使う (描き方は make_material.py と make_sync_video.py も使う)。
#
# 実際の画面共有では、帯が枠の領域の左上に等倍で載るとは限らない。たとえば Discord が Firefox のウィンドウを
# 共有すると、ウィンドウ 1920x1080 が 1280x720 に縮む (x 2/3)。上の 76 px ほどはタブの列・アドレスの欄・
# ブックマークの欄で、ページの余白 8 px の後に canvas が中央寄せで置かれる。Chrome のタブの共有は、
# 表示域 約 1920x905 を 1280x720 に引き伸ばすので、縦と横の倍率が違う (円が縦長の楕円に写る)。
# 枠はこれを 0.5 倍で置くので、領域の左上はブラウザのタブの列 (暗い灰色) になり、固定の位置で読むと
# 全部のフレームが 0 と読める。
#
# そこで帯を「どこにどの大きさで写っても、自分で見つけて測れる形」にした。並び (u = セルの一辺):
#
#   横: 静か 1u | 33 セル | 静か 1u                                  = 35u
#   縦: 静か 1u | 時計 1u | データ 2u | 白い棒 1u | 静か 1u         = 6u
#
#   - 静か = 黒。帯の外の色 (ページの背景・白のマーカーのフレームの白) に頼らず、時計の両端と白い棒の上下の縁を必ず作る。
#   - 時計 = 33 セルを白・黒の交互 (左端の 0 番が白)。33 は奇数なので両端が白になり、黒い静かとの境で左右の縁が見える。
#     縁から縁までを 33 で割ればセルの幅が出る (横の倍率を測る)。
#   - データ = 左の 32 セルに 24 ビットのフレーム番号 (最上位が左) と、その 3 バイト (上位から) の CRC-8 (多項式 0x07、
#     初期値 0、反転なし。上位が左) の 8 ビット。白が 1。33 番のセルはいつも黒。高さは 2u (縦に少しずれてもセルの中を読む)。
#   - 白い棒 = 33 セルの幅の白。33 番の列を縦に見ると「白 1u (時計) / 黒 2u (データの 33 番) / 白 1u (棒)」になり、
#     時計の上の縁から棒の下の縁までを 4 で割ればセルの高さが出る (縦の倍率を、横とは別に測る)。
#
# 設計の理由:
#   - 縦の倍率を横とは別に測る。セルを正方形とみなして横の幅から高さを決めると、Chrome のタブの共有で縦が 17% ずれる。
#   - 時計の行で位置と横の倍率を探す。32 セルのままの帯の左上を色の境で探す方法は、帯の左端と右端が黒いセルで、
#     ページの背景 (#202020 = 32) との差が 30 ほどしかなく、縮小と圧縮の後では縁が決まらない (上位ビットが
#     0 の間は帯の左半分が全部黒い)。時間の変化 (下位のセルほど速く明滅する) で探す方法は、読むのに長い区間が要り、
#     共有が途中で動くと追えない。
#   - CRC-8 で 1 フレームずつ読みの正しさを確認し、合わなければそのフレームを「読めない」にする (番号として数えない)。
#     共有のエンコーダは、ビット列の帯の一部のマクロブロックだけを前のフレームのまま残すことがあり、混ざった読みは
#     欠落や逆行に見えるため。偶数の 1 ビット (2 か所の誤りを通す) や、番号を 2 回描く方法 (帯が倍の幅になる) は採らない。
#   - 番号は 24 ビット (60 fps で 77 時間で一巡する。pattern.html はページを開いた時から数える)。
#     32 ビット + CRC にすると 41 セルになり、canvas 1280 px に 32 px のセルが入らない。
#   - セルの大きさは描き手が決め、読み手は測る (pattern.html は 32 px、sync-video.mp4 は 24 px)。
#     読み手は 1 セルが MIN_CELL_PX 以上なら読む。
#   - 古い形の帯 (sender.ts / make_material.py のテストデータ。枠の左上に等倍で載る) は frames.py の「固定の位置で読む」形で
#     そのまま読む (この形は古い形の帯を探さない)。
#
# 使い方: python bit_strip.py --selftest   (合成の画像で、縮小・縦横の倍率違い・ぼかし・ずらしの後も見つけて読めること、
#                                          古い形の帯と暗い画面では見つからないことを確認する)
import math
import sys
from dataclasses import dataclass
from functools import lru_cache

import numpy as np
from numpy.lib.stride_tricks import sliding_window_view

STRIP_COLUMNS = 33        # 時計の行のセルの数 (奇数 = 両端が白)
DATA_COLUMNS = 32         # データの行で値を持つセルの数 (24 + 8)。33 番はいつも黒
COUNTER_BITS = 24
CRC_BITS = 8
COUNTER_MODULO = 1 << COUNTER_BITS
WIDTH_U = STRIP_COLUMNS + 2   # 35u (左右の静かを含む)
HEIGHT_U = 6                  # 静か 1 + 時計 1 + データ 2 + 棒 1 + 静か 1
CLOCK_TOP_U = 1
DATA_TOP_U = 2
DATA_HEIGHT_U = 2
BAR_TOP_U = 4

THRESHOLD = 128   # 白と黒の境目 (帯は真っ白と真っ黒だけで描く。限定範囲の YUV でも 16 と 235 の中点に近い)
# 1 セルが 3 px より小さいと、セルの中央の半分が 1.5 px を切り、縮小のぼかしと圧縮で読みが揺れる。
# 枠 640x360 の中で pattern.html の 32 px のセルは 10.7 px、sync-video.mp4 の 24 px のセルは 8 px で、
# どちらも十分上にある。
MIN_CELL_PX = 3.0
# 時計のセルの長さの揃い方の許し: 同じ色のセルの長さの中央値から 1.5 px か 3 割まで。縮小と 2 値化で
# 白のセルと黒のセルの長さは系統的に違ってよい (色ごとに中央値を取る) が、同じ色の中では揃う。
RUN_TOL_PX = 1.5
RUN_TOL_RATIO = 0.3
QUIET_MIN_RATIO = 0.5     # 時計の左右の静か (黒) はセルの半分以上 (描くのは 1 セル)
ASPECT_MIN = 0.4          # セルの高さ / 幅 の許す範囲 (Chrome のタブの共有は 1.17)
ASPECT_MAX = 2.5
GROUP_TOL_PX = 2          # 時計の行に見える行を 1 つの帯にまとめるときの、左右の縁のずれの許し


def crc8(data):
    """CRC-8 (多項式 0x07、初期値 0、反転なし、最後の xor なし)。b"123456789" で 0xF4。"""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def strip_word(counter):
    """データの行の 32 ビット (上位 24 ビットがフレーム番号、下位 8 ビットが CRC-8)。番号は 24 ビットで回る。"""
    n = int(counter) % COUNTER_MODULO
    return (n << CRC_BITS) | crc8(n.to_bytes(3, "big"))


def strip_bits(counter):
    """データの行の 32 セルの白黒 (左から。1 = 白)。"""
    word = strip_word(counter)
    return [(word >> (DATA_COLUMNS - 1 - i)) & 1 for i in range(DATA_COLUMNS)]


def strip_mask(counter, u):
    """帯 1 本の絵 (高さ 6u x 幅 35u、0 か 255 の uint8)。"""
    u = int(u)
    mask = np.zeros((HEIGHT_U * u, WIDTH_U * u), dtype=np.uint8)
    for i in range(0, STRIP_COLUMNS, 2):
        mask[CLOCK_TOP_U * u:(CLOCK_TOP_U + 1) * u, (1 + i) * u:(2 + i) * u] = 255
    for i, bit in enumerate(strip_bits(counter)):
        if bit:
            mask[DATA_TOP_U * u:(DATA_TOP_U + DATA_HEIGHT_U) * u, (1 + i) * u:(2 + i) * u] = 255
    mask[BAR_TOP_U * u:(BAR_TOP_U + 1) * u, u:(1 + STRIP_COLUMNS) * u] = 255
    return mask


def draw_strip(img, x, y, u, counter):
    """img (灰色の 2 次元か、色の 3 次元の uint8) の (x, y) を左上に帯を描く。はみ出した分は描かない。"""
    mask = strip_mask(counter, u)
    h, w = img.shape[0], img.shape[1]
    x0, y0 = max(0, int(x)), max(0, int(y))
    x1, y1 = min(w, int(x) + mask.shape[1]), min(h, int(y) + mask.shape[0])
    if x1 <= x0 or y1 <= y0:
        return
    part = mask[y0 - int(y):y1 - int(y), x0 - int(x):x1 - int(x)]
    if img.ndim == 3:
        img[y0:y1, x0:x1, :] = part[:, :, None]
    else:
        img[y0:y1, x0:x1] = part


@dataclass(frozen=True)
class Geometry:
    """見つけた帯の位置とセルの大きさ (読む画像の座標。x0 / top は時計の 0 番のセルの左の縁と上の縁)。"""
    x0: float
    cw: float     # セルの幅
    top: float
    uh: float     # セルの高さ

    def col_x(self, i):
        return self.x0 + (i + 0.5) * self.cw

    @property
    def clock_y(self):
        return self.top + 0.5 * self.uh

    @property
    def data_y(self):
        return self.top + 2.0 * self.uh

    @property
    def bar_y(self):
        return self.top + 3.5 * self.uh

    def close_to(self, other):
        # 同じ帯とみなせるか (縁が 1 px、セルが 0.2 px の内)。geometry の変化を記録するときだけ使う。
        return (abs(self.x0 - other.x0) <= 1 and abs(self.top - other.top) <= 1
                and abs(self.cw - other.cw) <= 0.2 and abs(self.uh - other.uh) <= 0.2)

    def to_dict(self, offset_x=0, offset_y=0):
        # 録画の座標に修正して出す (帯の静かを含まない、時計の 0 番のセルの左上とセルの大きさ)。
        return {"x": round(self.x0 + offset_x, 1), "y": round(self.top + offset_y, 1),
                "cell_w": round(self.cw, 2), "cell_h": round(self.uh, 2)}


def _clock_spans_in_row(row_bin):
    """2 値の 1 行から、時計の行に見える区間 (左の縁 x, 右の縁 x (含まない)) を返す。"""
    n = row_bin.shape[0]
    edges = np.flatnonzero(row_bin[1:] != row_bin[:-1]) + 1
    if edges.size < STRIP_COLUMNS + 1:
        return []
    starts = np.concatenate(([0], edges))
    ends = np.concatenate((edges, [n]))
    lens = ends - starts
    vals = row_bin[starts]
    last_k = lens.size - STRIP_COLUMNS - 1   # 前と後ろに暗い連なり (静か) が要る
    if last_k < 1:
        return []
    ks = np.arange(1, last_k + 1)
    ks = ks[vals[ks]]
    if ks.size == 0:
        return []
    win = sliding_window_view(lens, STRIP_COLUMNS)[ks].astype(np.float64)
    whites = win[:, 0::2]
    blacks = win[:, 1::2]
    mw = np.median(whites, axis=1)
    mb = np.median(blacks, axis=1)
    cell = (mw + mb) / 2.0
    ok = cell >= MIN_CELL_PX
    ok &= (mw <= 2.0 * mb) & (mb <= 2.0 * mw)
    tw = np.maximum(RUN_TOL_PX, RUN_TOL_RATIO * mw)
    tb = np.maximum(RUN_TOL_PX, RUN_TOL_RATIO * mb)
    ok &= np.all(np.abs(whites - mw[:, None]) <= tw[:, None], axis=1)
    ok &= np.all(np.abs(blacks - mb[:, None]) <= tb[:, None], axis=1)
    ok &= lens[ks - 1] >= QUIET_MIN_RATIO * cell
    ok &= lens[ks + STRIP_COLUMNS] >= QUIET_MIN_RATIO * cell
    return [(int(starts[k]), int(ends[k + STRIP_COLUMNS - 1])) for k in ks[ok]]


def _group_hits(hits):
    """(行, 左, 右) の並びを、縁が揃って続く行ごとにまとめる。"""
    groups = []
    for r, xs, xe in sorted(hits):
        for g in groups:
            lr, lxs, lxe = g[-1]
            if r - lr <= 2 and abs(xs - lxs) <= GROUP_TOL_PX and abs(xe - lxe) <= GROUP_TOL_PX:
                g.append((r, xs, xe))
                break
        else:
            groups.append([(r, xs, xe)])
    return groups


def _run_from(col, start, value):
    """col[start] から同じ値が続く終わり (含まない) を返す。"""
    i = start
    n = col.shape[0]
    while i < n and bool(col[i]) == value:
        i += 1
    return i


def _measure_vertical(img, x0, cw, yc):
    """33 番の列 (白 / 黒 / 白) を縦に見て、時計の上の縁とセルの高さを測る。合わなければ None。"""
    h, w = img.shape
    xc = x0 + (STRIP_COLUMNS - 0.5) * cw
    xa = int(math.floor(xc - cw / 4.0))
    xb = max(xa + 1, int(math.ceil(xc + cw / 4.0)))
    if xa < 0 or xb > w:
        return None
    col = img[:, xa:xb].mean(axis=1) > THRESHOLD
    y = int(round(yc))
    if y < 0 or y >= h or not col[y]:
        return None
    top = y
    while top > 0 and col[top - 1]:
        top -= 1
    if top == 0:
        return None   # 時計の上の縁 (静かとの境) が領域の外
    clock_end = _run_from(col, top, True)
    data_end = _run_from(col, clock_end, False)
    bar_end = _run_from(col, data_end, True)
    quiet_end = _run_from(col, bar_end, False)
    clock_len = clock_end - top
    data_len = data_end - clock_end
    bar_len = bar_end - data_end
    quiet_len = quiet_end - bar_end
    if bar_end >= h or clock_len < 1 or data_len < 1 or bar_len < 1:
        return None
    unit = (clock_len + bar_len) / 2.0
    if abs(clock_len - bar_len) > max(RUN_TOL_PX, RUN_TOL_RATIO * unit + 0.5):
        return None
    if abs(data_len - 2.0 * unit) > max(2.0, RUN_TOL_RATIO * 2.0 * unit + 0.5):
        return None
    if quiet_len < QUIET_MIN_RATIO * unit:
        return None
    uh = (bar_end - top) / 4.0
    if not (ASPECT_MIN <= uh / cw <= ASPECT_MAX):
        return None
    return Geometry(x0=float(x0), cw=float(cw), top=float(top), uh=float(uh))


def find_strip(img):
    """灰色の画像 (2 次元の uint8) の中から帯を探す。見つからなければ None。"""
    if img.ndim != 2 or img.shape[0] < HEIGHT_U * MIN_CELL_PX or img.shape[1] < WIDTH_U * MIN_CELL_PX:
        return None
    binary = img > THRESHOLD
    transitions = np.count_nonzero(binary[:, 1:] != binary[:, :-1], axis=1)
    hits = []
    for r in np.flatnonzero(transitions >= STRIP_COLUMNS + 1):
        for xs, xe in _clock_spans_in_row(binary[r]):
            hits.append((int(r), xs, xe))
    if not hits:
        return None
    for group in sorted(_group_hits(hits), key=len, reverse=True):
        rows = np.array([g[0] for g in group], dtype=np.float64)
        x0 = float(np.median([g[1] for g in group]))
        x1 = float(np.median([g[2] for g in group]))
        cw = (x1 - x0) / STRIP_COLUMNS
        geom = _measure_vertical(img, x0, cw, float(np.median(rows)))
        if geom is not None and read_strip(img, geom)[0] != "lost":
            return geom
    return None


def _start(center, size):
    """center を中心に size px を並べるときの最初の px の番付。"""
    return int(math.floor(center - size / 2.0 + 0.5))


@lru_cache(maxsize=64)
def _sampler(geom, height, width):
    """geom を読むための、セルごとの読み取りの四角 (セルの中央の半分) の位置。画像の外にかかれば None。
    毎フレーム同じ geom を読むので、位置の計算は 1 回だけにする (1 フレームあたりの読みを numpy の数回の操作にする)。"""
    n_w = max(1, int(round(geom.cw * 0.5)))
    n_thin = max(1, int(round(geom.uh * 0.5)))
    n_data = max(1, int(round(geom.uh * 1.0)))
    centers = [geom.col_x(i) for i in range(STRIP_COLUMNS)]
    centers += [geom.x0 - 0.5 * geom.cw, geom.x0 + (STRIP_COLUMNS + 0.5) * geom.cw]   # 左右の静か
    starts = np.array([_start(c, n_w) for c in centers], dtype=np.intp)
    cols = starts[:, None] + np.arange(n_w, dtype=np.intp)[None, :]
    rows = {"clock": (_start(geom.clock_y, n_thin), n_thin), "bar": (_start(geom.bar_y, n_thin), n_thin),
            "data": (_start(geom.data_y, n_data), n_data)}
    if cols.min() < 0 or cols.max() >= width:
        return None
    if any(y < 0 or y + n > height for y, n in rows.values()):
        return None
    return cols, rows


def _means(img, cols, row):
    y, n = row
    return img[y:y + n][:, cols].mean(axis=(0, 2))


def read_strip(img, geom):
    """geom の位置の帯を読む。("ok", 番号) / ("crc", None) = 形は合うが CRC が合わない /
    ("lost", None) = その位置に帯の形がない (時計・静か・棒・33 番の黒のどれかが合わない)。"""
    sampler = _sampler(geom, img.shape[0], img.shape[1])
    if sampler is None:
        return "lost", None
    cols, rows = sampler
    clock = _means(img, cols, rows["clock"]) > THRESHOLD   # 33 セル + 左右の静か
    if not (np.array_equal(clock[:STRIP_COLUMNS], _CLOCK_PATTERN) and not clock[STRIP_COLUMNS:].any()):
        return "lost", None
    if not (_means(img, cols[_BAR_CHECK_COLUMNS], rows["bar"]) > THRESHOLD).all():
        return "lost", None
    data = _means(img, cols[:STRIP_COLUMNS], rows["data"]) > THRESHOLD
    if data[STRIP_COLUMNS - 1]:
        return "lost", None   # 33 番のセルはいつも黒
    word = 0
    for bit in data[:DATA_COLUMNS]:
        word = (word << 1) | int(bit)
    counter = word >> CRC_BITS
    if crc8(counter.to_bytes(3, "big")) != (word & 0xFF):
        return "crc", None
    return "ok", counter


_CLOCK_PATTERN = np.array([i % 2 == 0 for i in range(STRIP_COLUMNS)])
_BAR_CHECK_COLUMNS = [0, STRIP_COLUMNS // 2, STRIP_COLUMNS - 1]


# 帯を見失ってから次に探すまでのフレーム数。探すのは 640x360 で 1 回数 ms (1080p の画面全体で十数 ms)
# かかる。帯のない録画 (古い形の帯のテストデータ・共有の前の空の枠) で毎フレーム探すと判定スクリプトの見積もり
# (run-window-plan.mjs の JUDGE_SECONDS_PER_RECORDED_SECOND) を超えるので、見つからなかったら 15 フレーム
# (60 fps で 0.25 秒) は探さない。見失った直後のフレーム (前の位置で読めなかったフレーム) はすぐに探す。
SEARCH_EVERY_FRAMES = 15


class StripTracker:
    """フレームを 1 枚ずつ受け取り、前の位置で読み、読めなければ探し直す。"""

    def __init__(self, search_every=SEARCH_EVERY_FRAMES):
        self.search_every = max(1, int(search_every))
        self.geom = None
        self.next_search = 0
        self.geometries = []   # (フレームの番付, Geometry): 位置が変わるたびに足す
        self.searches = 0
        self.crc_failures = 0
        self.ok_frames = 0

    def _count(self, status, counter):
        if status == "crc":
            self.crc_failures += 1
        elif status == "ok":
            self.ok_frames += 1
        return status, counter

    def feed(self, img, index):
        if self.geom is not None:
            status, counter = read_strip(img, self.geom)
            if status != "lost":
                return self._count(status, counter)
        if index < self.next_search:
            return "lost", None
        self.searches += 1
        geom = find_strip(img)
        if geom is None:
            self.next_search = index + self.search_every
            return "lost", None
        if self.geom is None or not geom.close_to(self.geom):
            self.geometries.append((index, geom))
        self.geom = geom
        return self._count(*read_strip(img, geom))


# ---- セルフテスト ----------------------------------------------------------------------------------------

def _area_weights(n_in, n_out):
    """長さ n_in を n_out に面積の平均で縮める重みの行列 (n_out x n_in)。"""
    edges = np.linspace(0.0, float(n_in), n_out + 1)
    left = np.arange(n_in, dtype=np.float64)
    lo = np.maximum(edges[:-1, None], left[None, :])
    hi = np.minimum(edges[1:, None], left[None, :] + 1.0)
    overlap = np.clip(hi - lo, 0.0, None)
    return overlap / overlap.sum(axis=1, keepdims=True)


def _resize_area(img, out_w, out_h):
    """面積の平均で縮める (縦横の倍率は別でよい)。自己検査の合成の縮小用。"""
    h, w = img.shape
    out = _area_weights(h, out_h) @ img.astype(np.float64) @ _area_weights(w, out_w).T
    return np.clip(np.round(out), 0, 255).astype(np.uint8)


def _box_blur(img, k):
    if k <= 1:
        return img
    pad = k // 2
    f = np.pad(img.astype(np.float64), pad, mode="edge")
    c = np.cumsum(np.cumsum(f, axis=0), axis=1)
    c = np.pad(c, ((1, 0), (1, 0)))
    h, w = img.shape
    s = c[k:k + h, k:k + w] - c[0:h, k:k + w] - c[k:k + h, 0:w] + c[0:h, 0:w]
    return np.clip(np.round(s / (k * k)), 0, 255).astype(np.uint8)


def _synthetic(counter, u, sx, sy, blur, legacy=False):
    """ブラウザのウィンドウに似た 1920x1080 の絵 (上に明るい文字に似た棒の並ぶ帯、背景 32、canvas 58) に帯を描き、
    sx x sy 倍に縮めてぼかした灰色の画像を返す。帯の時計の 0 番の左上の、縮めた後の座標も返す。"""
    win = np.full((1080, 1920), 32, dtype=np.uint8)
    win[0:114, :] = 40
    rng = np.random.default_rng(7)
    for _ in range(160):   # 文字に似た明るい短い棒 (タブのタイトル・アドレス)。探すときの紛らわしいもの
        x = int(rng.integers(0, 1900))
        y = int(rng.integers(4, 108))
        win[y:y + 3, x:x + int(rng.integers(2, 9))] = 230
    cx, cy = 320, 122
    win[cy:cy + 720, cx:cx + 1280] = 58
    if legacy:
        for b in range(32):
            if (counter >> (31 - b)) & 1:
                win[cy:cy + 16, cx + b * 16:cx + (b + 1) * 16] = 255
            else:
                win[cy:cy + 16, cx + b * 16:cx + (b + 1) * 16] = 0
    else:
        draw_strip(win, cx, cy, u, counter)
    out = _resize_area(win, int(round(1920 * sx)), int(round(1080 * sy)))
    return _box_blur(out, blur), ((cx + u) * sx, (cy + u) * sy)


def selftest():
    failures = []

    def check(name, ok):
        if not ok:
            failures.append(name)

    check("crc8(123456789) = 0xF4", crc8(b"123456789") == 0xF4)
    check("strip_word(0) = 0", strip_word(0) == 0)
    # 同じ値を test/tools/obs-run/pattern-strip.test.ts が pattern.html の描き方に当てる (言語をまたいだ一致)。
    check("strip_word(1) = 0x00000107", strip_word(1) == 0x00000107)
    check("strip_word(1085089) = 0x108EA1AC", strip_word(1085089) == 0x108EA1AC)
    check("strip_word(31752) = 0x007C0866", strip_word(31752) == 0x007C0866)
    check("strip_word(16777215) = 0xFFFFFF0F", strip_word(16777215) == 0xFFFFFF0F)
    check("番号は 24 ビットで回る", strip_word(COUNTER_MODULO + 5) == strip_word(5))
    mask = strip_mask(1085089, 1)
    check("帯の大きさ 6x35", mask.shape == (6, 35))
    check("時計の 0 番と 32 番は白、1 番は黒", mask[1, 1] == 255 and mask[1, 33] == 255 and mask[1, 2] == 0)
    check("データの 33 番のセルは黒", mask[2, 33] == 0 and mask[3, 33] == 0)
    check("白い棒は 33 セル", int((mask[4] == 255).sum()) == 33)

    # 縮めて (縦横別の倍率も)、ぼかして、帯を見つけて読めること。値は実際の共有の形を含む:
    # ウィンドウの共有 x 2/3 を枠で 0.5 倍 (1/3)、Chrome のタブの縦横違い (1/3 x 0.398)。
    cases = [
        ("pattern 32px 1/3 (ウィンドウの共有)", 1085089, 32, 1 / 3, 1 / 3, 2),
        ("sync 24px 1/3 x 0.398 (タブの共有)", 31752, 24, 1 / 3, 0.398, 2),
        ("pattern 32px 1/6 (4K のウィンドウ)", 123456, 32, 1 / 6, 1 / 6, 1),
        ("pattern 32px 1.0", 7, 32, 1.0, 1.0, 3),
        ("pattern 32px 0.5 x 0.6", 16777215, 32, 0.5, 0.6, 3),
    ]
    for name, counter, u, sx, sy, blur in cases:
        img, (ex, ey) = _synthetic(counter, u, sx, sy, blur)
        geom = find_strip(img)
        check(name + ": 見つかる", geom is not None)
        if geom is None:
            continue
        check(name + ": 位置 (%.1f, %.1f) と期待 (%.1f, %.1f)" % (geom.x0, geom.top, ex, ey),
              abs(geom.x0 - ex) <= 1.5 and abs(geom.top - ey) <= 1.5)
        check(name + ": セルの幅 %.2f と期待 %.2f" % (geom.cw, u * sx), abs(geom.cw - u * sx) <= 0.25)
        check(name + ": セルの高さ %.2f と期待 %.2f" % (geom.uh, u * sy), abs(geom.uh - u * sy) <= 0.6)
        status, value = read_strip(img, geom)
        check(name + ": 読める (%s %s)" % (status, value), status == "ok" and value == counter % COUNTER_MODULO)

    # 見つからないもの: 古い形の帯 (32 セル、時計なし) と、暗い画面、画面の端で切れた帯。
    legacy, _ = _synthetic(1085089, 16, 1 / 3, 1 / 3, 1, legacy=True)
    check("古い形の帯は見つからない", find_strip(legacy) is None)
    check("暗い画面では見つからない", find_strip(np.full((360, 640), 32, dtype=np.uint8)) is None)
    img, (ex, ey) = _synthetic(99, 32, 1 / 3, 1 / 3, 1)
    cut = img[:, int(ex + 20 * 32 / 3):]
    check("右半分しかない帯は見つからない", find_strip(cut) is None)

    # 追い方: 前の位置で読めないフレームは CRC か形の違いで分ける。
    img, _ = _synthetic(4242, 32, 1 / 3, 1 / 3, 1)
    tracker = StripTracker()
    check("追う: 最初のフレームで見つけて読む", tracker.feed(img, 0) == ("ok", 4242))
    broken = img.copy()
    g = tracker.geom
    xa, xb = int(g.col_x(30) - 0.4 * g.cw), int(g.col_x(30) + 0.4 * g.cw)
    ya, yb = int(g.data_y - 0.8 * g.uh), int(g.data_y + 0.8 * g.uh)
    broken[ya:yb, xa:xb] = 255 - broken[ya:yb, xa:xb]   # データの 30 番のセルを反転
    check("追う: データの 1 セルが違えば crc", tracker.feed(broken, 1)[0] == "crc")
    check("追う: 帯が消えたら lost", tracker.feed(np.full_like(img, 32), 2) == ("lost", None))
    check("追う: 探して見つからなければ 15 フレームは探さない", tracker.next_search == 2 + SEARCH_EVERY_FRAMES)
    check("追う: その間も前の位置ではまず読む", tracker.feed(img, 3) == ("ok", 4242) and tracker.searches == 2)
    check("追う: 前の位置で読めず、探す時期でもなければ lost", tracker.feed(np.full_like(img, 32), 4) == ("lost", None)
          and tracker.searches == 2)

    if failures:
        print("selftest NG: bit_strip")
        for f in failures:
            print("  " + f)
        return 1
    print("selftest OK: bit_strip")
    return 0


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(selftest())
    print("usage: python bit_strip.py --selftest")
    sys.exit(2)
