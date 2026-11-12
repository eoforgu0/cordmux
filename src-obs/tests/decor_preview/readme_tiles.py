#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# README の画像に使う、枠の中の 4 つの画像 (画面共有の代わり) を描く。どれも架空のゲームの画面で、実在の作品の画像は
# 使わない。見る人の目が画像ではなく名前ラベルと 4 分割に行くように、色か明るさを抑えて書き出す (加工の強さは
# SHAPES_MUTE と KENNEY_MUTE。--vivid で加工しない)。使い方は README.md の「README の画像」にある。
#
#   python readme_tiles.py --out DIR [--vivid]
#       図形だけで描いた画面 (既定)。DIR に tile_tl.png、tile_tr.png、tile_bl.png、tile_br.png を書く
#   python readme_tiles.py --out DIR --style kenney --assets KENNEY_DIR [--vivid]
#       Kenney (kenney.nl) の素材 (ライセンスは CC0-1.0) のタイルで組んだ画面。素材はリポジトリに入れず、KENNEY_DIR の
#       下にパックごとのフォルダーで置く (置き方は KENNEY_PACKS と、素材がないときに表示する案内にある)

import argparse
import math
import random
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageEnhance, ImageStat

W, H = 1280, 720  # 枠 (960 x 540) より大きく描き、decor_preview が縮める
SS = 2  # 縁をなめらかにするため、2 倍で描いてから縮める


def canvas(color):
    return Image.new("RGB", (W * SS, H * SS), color)


def finish(img):
    return img.resize((W, H), Image.LANCZOS)


def s(v):
    return int(v * SS)


def gradient(img, top, bottom, y0=0, y1=None):
    draw = ImageDraw.Draw(img)
    y1 = img.height if y1 is None else y1
    for y in range(y0, y1):
        t = (y - y0) / max(1, y1 - y0 - 1)
        c = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        draw.line([(0, y), (img.width, y)], fill=c)


def heart(draw, x, y, size, fill):
    r = size / 4
    draw.ellipse([s(x), s(y), s(x + 2 * r), s(y + 2 * r)], fill=fill)
    draw.ellipse([s(x + 2 * r), s(y), s(x + 4 * r), s(y + 2 * r)], fill=fill)
    draw.polygon([(s(x + 0.15 * r), s(y + 1.4 * r)), (s(x + 3.85 * r), s(y + 1.4 * r)), (s(x + 2 * r), s(y + 4 * r))],
                 fill=fill)


def adventure():
    # 夕暮れの山を走るアクションゲーム。左上の体力と、右上のコインの数。
    img = canvas((0, 0, 0))
    gradient(img, (255, 170, 90), (110, 60, 140), 0, s(470))
    draw = ImageDraw.Draw(img)
    draw.ellipse([s(860), s(150), s(1040), s(330)], fill=(255, 226, 150))
    layers = [((150, 80, 140), 330, 90), ((105, 55, 115), 400, 70), ((70, 40, 90), 470, 55)]
    rnd = random.Random(3)
    for color, base, amp in layers:
        points = [(0, H * SS)]
        x = 0
        while x <= W:
            points.append((s(x), s(base - rnd.randint(0, amp))))
            x += rnd.randint(70, 150)
        points += [(W * SS, s(base)), (W * SS, H * SS)]
        draw.polygon(points, fill=color)
    draw.rectangle([0, s(560), W * SS, H * SS], fill=(52, 34, 60))
    for x0 in range(0, W, 64):
        draw.rectangle([s(x0), s(560), s(x0 + 60), s(574)], fill=(88, 62, 96))
    for px, py, pw in [(330, 470, 170), (620, 400, 150), (900, 470, 180)]:
        draw.rounded_rectangle([s(px), s(py), s(px + pw), s(py + 26)], radius=s(8), fill=(120, 200, 120))
        draw.rectangle([s(px), s(py + 18), s(px + pw), s(py + 26)], fill=(90, 150, 90))
    # 主人公
    draw.ellipse([s(660), s(330), s(700), s(370)], fill=(255, 230, 200))
    draw.rounded_rectangle([s(655), s(365), s(705), s(400)], radius=s(10), fill=(60, 120, 230))
    draw.polygon([(s(705), s(370)), (s(745), s(360)), (s(705), s(388))], fill=(230, 70, 70))
    for i in range(5):
        cx = 360 + i * 32
        draw.ellipse([s(cx), s(420), s(cx + 22), s(442)], fill=(255, 210, 60))
    for i in range(3):
        heart(draw, 40 + i * 52, 36, 44, (240, 70, 90))
    draw.ellipse([s(1110), s(36), s(1150), s(76)], fill=(255, 210, 60))
    draw.rectangle([s(1162), s(48), s(1240), s(64)], fill=(255, 255, 255))
    return finish(img)


def racing():
    # 昼の道を走るレースゲーム。右下に速度のメーター。
    img = canvas((0, 0, 0))
    gradient(img, (90, 160, 240), (200, 230, 255), 0, s(330))
    gradient(img, (110, 190, 90), (60, 140, 60), s(330), H * SS)
    draw = ImageDraw.Draw(img)
    for cx, cy, r in [(250, 120, 60), (330, 110, 75), (420, 125, 55), (900, 170, 50), (970, 160, 65)]:
        draw.ellipse([s(cx - r), s(cy - r * 0.6), s(cx + r), s(cy + r * 0.6)], fill=(255, 255, 255))
    draw.polygon([(s(600), s(330)), (s(680), s(330)), (s(1180), s(720)), (s(100), s(720))], fill=(80, 84, 92))
    draw.polygon([(s(600), s(330)), (s(610), s(330)), (s(140), s(720)), (s(100), s(720))], fill=(235, 235, 235))
    draw.polygon([(s(670), s(330)), (s(680), s(330)), (s(1180), s(720)), (s(1140), s(720))], fill=(235, 235, 235))
    for i in range(7):
        t0 = (i / 7) ** 2
        t1 = ((i + 0.45) / 7) ** 2
        y0, y1 = 330 + 390 * t0, 330 + 390 * t1
        w0, w1 = 2 + 10 * t0, 2 + 10 * t1
        draw.polygon([(s(640 - w0), s(y0)), (s(640 + w0), s(y0)), (s(640 + w1), s(y1)), (s(640 - w1), s(y1))],
                     fill=(250, 220, 80))
    for tx in [150, 1120]:
        draw.rectangle([s(tx - 6), s(250), s(tx + 6), s(340)], fill=(110, 70, 40))
        draw.ellipse([s(tx - 45), s(190), s(tx + 45), s(280)], fill=(50, 130, 60))
    # 車
    draw.rounded_rectangle([s(520), s(560), s(760), s(660)], radius=s(22), fill=(225, 50, 60))
    draw.rounded_rectangle([s(565), s(520), s(715), s(590)], radius=s(20), fill=(200, 40, 50))
    draw.rounded_rectangle([s(585), s(532), s(695), s(575)], radius=s(12), fill=(120, 180, 230))
    draw.rectangle([s(530), s(600), s(570), s(618)], fill=(255, 240, 160))
    draw.rectangle([s(710), s(600), s(750), s(618)], fill=(255, 240, 160))
    for wx in [540, 700]:
        draw.rounded_rectangle([s(wx), s(650), s(wx + 40), s(680)], radius=s(8), fill=(30, 30, 35))
    # 速度のメーター
    cx, cy, r = 1150, 600, 90
    draw.ellipse([s(cx - r), s(cy - r), s(cx + r), s(cy + r)], fill=(20, 24, 32))
    draw.arc([s(cx - r + 14), s(cy - r + 14), s(cx + r - 14), s(cy + r - 14)], 140, 400, fill=(90, 200, 255),
             width=s(10))
    a = math.radians(330)
    draw.line([(s(cx), s(cy)), (s(cx + 60 * math.cos(a)), s(cy + 60 * math.sin(a)))], fill=(255, 90, 80), width=s(6))
    draw.rectangle([s(40), s(36), s(220), s(60)], fill=(255, 255, 255))
    draw.rectangle([s(40), s(70), s(160), s(86)], fill=(255, 220, 80))
    return finish(img)


def puzzle():
    # 宝石をそろえるパズルゲーム。右に得点の枠。
    img = canvas((0, 0, 0))
    gradient(img, (40, 30, 80), (20, 60, 110))
    draw = ImageDraw.Draw(img)
    colors = [(240, 80, 100), (80, 200, 255), (255, 210, 70), (120, 220, 120), (190, 120, 255)]
    rnd = random.Random(11)
    gx, gy, cell, n = 250, 70, 72, 8
    draw.rounded_rectangle([s(gx - 16), s(gy - 16), s(gx + cell * n + 16), s(gy + cell * n + 16)], radius=s(18),
                           fill=(20, 18, 48))
    for row in range(n):
        for col in range(n):
            x, y = gx + col * cell, gy + row * cell
            if (row + col) % 2 == 0:
                draw.rectangle([s(x), s(y), s(x + cell), s(y + cell)], fill=(30, 28, 66))
            c = colors[rnd.randrange(len(colors))]
            kind = rnd.randrange(3)
            m = 12
            if kind == 0:
                draw.ellipse([s(x + m), s(y + m), s(x + cell - m), s(y + cell - m)], fill=c)
            elif kind == 1:
                draw.polygon([(s(x + cell / 2), s(y + m)), (s(x + cell - m), s(y + cell / 2)),
                              (s(x + cell / 2), s(y + cell - m)), (s(x + m), s(y + cell / 2))], fill=c)
            else:
                draw.rounded_rectangle([s(x + m), s(y + m), s(x + cell - m), s(y + cell - m)], radius=s(10), fill=c)
            draw.ellipse([s(x + m + 8), s(y + m + 6), s(x + m + 20), s(y + m + 16)], fill=(255, 255, 255))
    draw.rounded_rectangle([s(880), s(70), s(1160), s(250)], radius=s(18), fill=(20, 18, 48))
    draw.rectangle([s(910), s(110), s(1080), s(130)], fill=(160, 150, 220))
    draw.rectangle([s(910), s(160), s(1130), s(200)], fill=(255, 220, 90))
    draw.rounded_rectangle([s(880), s(290), s(1160), s(500)], radius=s(18), fill=(20, 18, 48))
    for i, c in enumerate(colors[:4]):
        draw.ellipse([s(915 + i * 58), s(330), s(955 + i * 58), s(370)], fill=c)
    draw.rectangle([s(910), s(410), s(1130), s(430)], fill=(120, 110, 190))
    draw.rectangle([s(910), s(450), s(1060), s(470)], fill=(120, 110, 190))
    return finish(img)


def strategy():
    # 上から見た地図の戦略ゲーム。川と道と建物、右上に小さな地図。
    img = canvas((96, 168, 92))
    draw = ImageDraw.Draw(img)
    rnd = random.Random(5)
    for _ in range(260):
        x, y = rnd.randint(0, W), rnd.randint(0, H)
        draw.ellipse([s(x), s(y), s(x + 18), s(y + 12)], fill=(84, 150, 80))
    river = [(-20, 520), (200, 470), (420, 500), (620, 430), (820, 470), (1020, 400), (1300, 430)]
    draw.line([(s(x), s(y)) for x, y in river], fill=(70, 150, 220), width=s(70), joint="curve")
    draw.line([(s(x), s(y)) for x, y in river], fill=(110, 185, 240), width=s(30), joint="curve")
    draw.line([(s(640), s(-10)), (s(560), s(300)), (s(600), s(740))], fill=(200, 180, 130), width=s(26),
              joint="curve")
    draw.rectangle([s(555), s(440), s(645), s(500)], fill=(150, 110, 70))
    for tx, ty in [(150, 120), (230, 200), (110, 300), (980, 600), (1080, 560), (1180, 640), (820, 150)]:
        draw.ellipse([s(tx - 40), s(ty - 40), s(tx + 40), s(ty + 40)], fill=(50, 120, 60))
        draw.ellipse([s(tx - 22), s(ty - 30), s(tx + 18), s(ty + 4)], fill=(80, 150, 80))
    for bx, by, roof in [(380, 180, (210, 80, 70)), (460, 260, (70, 110, 200)), (720, 250, (230, 180, 60)),
                         (300, 620, (210, 80, 70))]:
        draw.rectangle([s(bx), s(by), s(bx + 70), s(by + 60)], fill=(240, 230, 210))
        draw.polygon([(s(bx - 8), s(by)), (s(bx + 35), s(by - 34)), (s(bx + 78), s(by))], fill=roof)
    for ux, uy, c in [(820, 330, (60, 110, 230)), (860, 350, (60, 110, 230)), (360, 400, (230, 70, 70))]:
        draw.ellipse([s(ux - 16), s(uy - 16), s(ux + 16), s(uy + 16)], fill=(255, 255, 255))
        draw.ellipse([s(ux - 11), s(uy - 11), s(ux + 11), s(uy + 11)], fill=c)
    draw.rectangle([s(1040), s(24), s(1256), s(178)], fill=(30, 40, 30))
    draw.rectangle([s(1048), s(32), s(1248), s(170)], fill=(96, 168, 92))
    draw.line([(s(1048), s(130)), (s(1248), s(112))], fill=(110, 185, 240), width=s(8))
    draw.rectangle([s(1120), s(70), s(1170), s(108)], outline=(255, 255, 255), width=s(3))
    draw.rounded_rectangle([s(24), s(24), s(300), s(70)], radius=s(12), fill=(30, 40, 30))
    for i, c in enumerate([(230, 200, 80), (170, 120, 80), (160, 160, 170)]):
        draw.ellipse([s(40 + i * 88), s(36), s(62 + i * 88), s(58)], fill=c)
        draw.rectangle([s(68 + i * 88), s(42), s(108 + i * 88), s(52)], fill=(240, 240, 240))
    return finish(img)


# ---- Kenney の素材で組む画面 (--style kenney) ----
#
# どの画面も次の 3 つを守る。
# - 名前ラベルが載る左下 (1280 x 720 の画像で x < 360、y > 570) には、素材を置かず、模様のない面だけにする。
#   decor_preview の見本では、名前ラベルは枠 (960 x 540) の x 11 から 212、y 458 から 529 にある。
# - 左下の 2 つの枠 (下の段) の左上には目立つ物を置かない。上の段の枠の名前ラベルのすぐ下に当たるためである。
# - 赤い大きな面を作らない (赤い屋根、赤い車、赤い看板、赤い宝石を使わない)。
# ドット絵 (Pixel Platformer、Tiny Town、Tiny Dungeon) は整数倍 (4 倍) に最近傍で拡大し、縁をぼかさない。

# (フォルダーの名前, パックの名前, 公式の頁, あるかを確かめるファイル)
KENNEY_PACKS = [
    ("pixel-platformer", "Pixel Platformer", "https://kenney.nl/assets/pixel-platformer",
     "Tiles/Backgrounds/tile_0000.png"),
    ("racing-pack", "Racing Pack", "https://kenney.nl/assets/racing-pack", "PNG/Cars/car_blue_1.png"),
    ("puzzle-pack-2", "Puzzle Pack 2", "https://kenney.nl/assets/puzzle-pack-2", "PNG/Tiles blue/tileBlue_01.png"),
    ("tiny-town", "Tiny Town", "https://kenney.nl/assets/tiny-town", "Tiles/tile_0000.png"),
    ("tiny-dungeon", "Tiny Dungeon", "https://kenney.nl/assets/tiny-dungeon", "Tiles/tile_0000.png"),
]

# 加工の強さ (彩度、コントラスト、明るさの倍率。Pillow の ImageEnhance の Color、Contrast、Brightness の順にかける)。
# 図形の画面は、色が素材の色ではないので、彩度とコントラストを大きく下げる。Kenney の画面は、彩度を大きく下げると
# わざと灰色にしたように見えるので、色は素材に近いまま、明るさを下げて名前ラベルに目が行くようにする
# (6 通りの加工を見比べて選んだ値)。
SHAPES_MUTE = (0.30, 0.70, 0.80)
KENNEY_MUTE = (0.85, 0.95, 0.65)

# Kenney の画面は、加工の後で 4 枚の明るさ (Pillow の L の平均) を 4 枚の平均にそろえる。暗い画面を明るくしすぎると
# 色が白っぽく抜けるので、明るさの倍率はこの範囲に止める。
KENNEY_EVEN_RANGE = (0.6, 1.4)


class Kenney:
    """素材のフォルダーから PNG を読む。読んだ画像は覚えておく。"""

    def __init__(self, root):
        self.root = Path(root)
        self.cache = {}

    def image(self, rel, scale=1.0, smooth=False):
        key = (rel, scale, smooth)
        if key not in self.cache:
            img = Image.open(self.root / rel).convert("RGBA")
            if scale != 1.0:
                size = (round(img.width * scale), round(img.height * scale))
                img = img.resize(size, Image.LANCZOS if smooth else Image.NEAREST)
            self.cache[key] = img
        return self.cache[key]


def missing_kenney_packs(root):
    root = Path(root)
    return [pack for pack in KENNEY_PACKS if not (root / pack[0] / pack[3]).is_file()]


def print_kenney_help(root, missing):
    lines = [f"Kenney の素材が見つかりません: {root}",
             "次のパックを公式の頁の [Download] から取得し、zip の中身をそのまま、パックごとの空のフォルダーに展開して",
             "ください (ライセンスはどれも CC0-1.0)。zip の中のスクリプトや実行ファイルは使いません。"]
    for folder, name, url, probe in missing:
        lines.append(f"  {Path(root) / folder}   {name}   {url}")
        lines.append(f"      (このファイルがあるかで確かめます: {probe})")
    print("\n".join(lines), file=sys.stderr)


def put(img, tile, x, y):
    img.alpha_composite(tile, (int(x), int(y)))


def put_center(img, tile, cx, cy):
    put(img, tile, cx - tile.width // 2, cy - tile.height // 2)


def hud_panel(img, box, radius=14, fill=(24, 28, 40, 200)):
    layer = Image.new("RGBA", img.size)
    ImageDraw.Draw(layer).rounded_rectangle(box, radius=radius, fill=fill)
    img.alpha_composite(layer)


def kenney_platformer(kn):
    # 横スクロールのアクション (Pixel Platformer)。左下は穴で、背景の下の面だけが見える。
    img = Image.new("RGBA", (W, H))
    t = 72  # 18 px のタイルを 4 倍

    def tile(n):
        return kn.image(f"pixel-platformer/Tiles/tile_{n:04d}.png", 4)

    def bg(n):
        return kn.image(f"pixel-platformer/Tiles/Backgrounds/tile_{n:04d}.png", 4)

    def char(n):
        return kn.image(f"pixel-platformer/Tiles/Characters/tile_{n:04d}.png", 4)

    # 背景: 砂漠の組 (空 0004/0005、丘 0012/0013、下の面 0020/0021)。24 px を 4 倍の 96 px で敷く。
    b = 96
    for i, x in enumerate(range(0, W, b)):
        alt = i % 2
        for y in range(0, 4 * b, b):
            put(img, bg(4 + alt), x, y)
        put(img, bg(12 + (1 if i % 3 == 1 else 0)), x, 4 * b)
        for y in range(5 * b, H, b):
            put(img, bg(20 + alt), x, y)

    rnd = random.Random(7)

    def fill():
        return tile(rnd.choice([122, 122, 104]))

    # 地面: 8 段目 (y 576) から下。0 から 4 列目は穴にする。13 列目から右は 2 段高い。
    for col in range(5, 18):
        x = col * t
        if col < 13:
            put(img, tile(21 if col == 5 else 22), x, 8 * t)
            put(img, tile(121) if col == 5 else fill(), x, 9 * t)
        else:
            put(img, tile(21 if col == 13 else 22), x, 6 * t)
            put(img, tile(121) if col == 13 else fill(), x, 7 * t)
            put(img, tile(25) if col == 13 else fill(), x, 8 * t)
            put(img, fill(), x, 9 * t)
    # 浮いている足場と、その上のコイン
    for col, row, length in [(6, 5, 3), (10, 3, 3)]:
        for k in range(length):
            put(img, tile(1 if k == 0 else 3 if k == length - 1 else 2), (col + k) * t, row * t)
    for col in range(6, 9):
        put(img, tile(151), col * t, 4 * t)
    put(img, tile(10), 14 * t, 2 * t)  # [!] のブロック
    # 飾り: 看板、サボテン、草
    put(img, tile(88), 6 * t, 7 * t)
    put(img, tile(127), 11 * t, 7 * t)
    put(img, tile(124), 15 * t, 5 * t)
    # 主人公 (緑) と敵
    put(img, char(0), 9 * t - 12, 8 * t - 96)
    put(img, char(18), 16 * t - 12, 6 * t - 96)
    put(img, char(24), 12 * t, 1 * t + 20)
    # 左上の体力と、右上のコインの数 (3 倍の大きさ)
    for i in range(3):
        put(img, kn.image(f"pixel-platformer/Tiles/tile_{44:04d}.png", 3), 20 + i * 52, 16)
    small = [kn.image(f"pixel-platformer/Tiles/tile_{n:04d}.png", 3) for n in (151, 158, 161, 162)]
    for i, im in enumerate(small):
        put(img, im, W - 40 - 54 * (len(small) - i), 16)
    return img.convert("RGB")


def kenney_racing(kn):
    # 上から見たレース (Racing Pack)。縦の直線のコースを右寄りに置き、左下は芝生だけにする。
    img = Image.new("RGBA", (W, H))
    t = 96  # 128 px のタイルを 0.75 倍

    def tile(rel):
        return kn.image(f"racing-pack/PNG/Tiles/{rel}", 0.75, smooth=True)

    def obj(name, scale=0.75):
        return kn.image(f"racing-pack/PNG/Objects/{name}.png", scale, smooth=True)

    for x in range(0, W, t):
        for y in range(0, H, t):
            put(img, tile("Grass/land_grass11.png"), x, y)
    # コース: 6 から 9 列目 (左の縁石、舗装 2 枚、右の縁石)。3 段目にスタートの線
    lanes = [21, 22, 22, 23]
    start = [69, 70, 70, 71]
    for row in range(0, H // t + 1):
        for k in range(4):
            n = start[k] if row == 3 else lanes[k]
            put(img, tile(f"Asphalt road/road_asphalt{n:02d}.png"), (6 + k) * t, row * t)
    # 車 (上に向かって走る。赤い車は使わない)
    def car(name):
        return kn.image(f"racing-pack/PNG/Cars/{name}.png", 0.75, smooth=True)

    road_x = 6 * t
    for name, lane, y in [("car_blue_1", 1, 150), ("car_yellow_3", 2, 250), ("car_green_2", 0, 420),
                          ("car_black_4", 3, 470)]:
        put_center(img, car(name), road_x + lane * t + t // 2, y)
    for y in (540, 600):
        put_center(img, obj("skidmark_short_2"), road_x + t // 2, y)
    # コースの外: 右にテントとタイヤの山と木、左の上半分に木とタイヤの山。左下には置かない。
    for cy in (250, 300, 350):
        put_center(img, obj("barrel_blue", 0.6), 10 * t + 60, cy)
    for x in (10 * t + 30, 6 * t - 40):
        for y in (90, 140):
            put_center(img, obj("tires_white"), x, y)
    for cx, cy, name in [(1180, 560, "tree_large"), (1080, 660, "tree_small"), (1210, 90, "tree_small"),
                         (130, 80, "tree_large"), (330, 150, "tree_large"), (200, 300, "tree_small"),
                         (440, 340, "tree_small")]:
        put_center(img, obj(name), cx, cy)
    # 右上の小さな地図 (コースの形と車の点)
    hud_panel(img, (W - 220, 20, W - 24, 176))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((W - 196, 44, W - 48, 152), radius=30, outline=(200, 205, 215, 255), width=8)
    for cx, cy in [(W - 196, 90), (W - 120, 44), (W - 48, 120)]:
        d.ellipse((cx - 8, cy - 8, cx + 8, cy + 8), fill=(110, 170, 240, 255))
    return img.convert("RGB")


def kenney_puzzle(kn):
    # 宝石をそろえるパズル (Puzzle Pack 2)。盤を右に寄せ、左は模様のない暗い背景にする (名前ラベルの白い文字が
    # 読みやすいように、左下を暗くする)。
    img = Image.new("RGBA", (W, H))
    grad = Image.new("RGB", (1, H))
    for y in range(H):
        k = y / (H - 1)
        grad.putpixel((0, y), (int(60 - 10 * k), int(68 - 10 * k), int(98 - 12 * k)))
    img.paste(grad.resize((W, H)), (0, 0))
    d = ImageDraw.Draw(img)
    cell, n = 72, 8
    gx, gy = 400, 72
    d.rounded_rectangle((gx - 14, gy - 14, gx + cell * n + 14, gy + cell * n + 14), radius=18, fill=(28, 32, 50))
    # マスは明るい灰色の素材を 6 割の明るさにして使う (そのままでは盤が画面でいちばん明るい面になり、目を引く)。
    back = ImageEnhance.Brightness(kn.image("puzzle-pack-2/PNG/Back tiles/BackTile_05.png", cell / 256,
                                            smooth=True)).enhance(0.6)
    for row in range(n):
        for col in range(n):
            put(img, back, gx + col * cell, gy + row * cell)
    # 色ごとに形を決める (青は宝石、緑はひし形、黄は星、桃はハート、橙は円)。赤い宝石は使わない。灰の円は白く
    # 光って目を引くので使わない。番号と形の対応は色ごとに違う (橙だけ 1 つずれている)。
    kinds = [("blue", "Blue", 10), ("green", "Green", 4), ("yellow", "Yellow", 9), ("pink", "Pink", 12),
             ("orange", "Orange", 10)]

    def gem(k, size=58):
        folder, name, num = kinds[k]
        im = kn.image(f"puzzle-pack-2/PNG/Tiles {folder}/tile{name}_{num:02d}.png")
        f = size / max(im.width, im.height)
        return kn.image(f"puzzle-pack-2/PNG/Tiles {folder}/tile{name}_{num:02d}.png", f, smooth=True)

    rnd = random.Random(21)
    board = [[rnd.randrange(len(kinds)) for _ in range(n)] for _ in range(n)]
    for col in (2, 3, 4):
        board[5][col] = 2  # そろいかけの 3 つ (黄の星)
    for row in range(n):
        for col in range(n):
            put_center(img, gem(board[row][col]), gx + col * cell + cell // 2, gy + row * cell + cell // 2)
    spark = kn.image("puzzle-pack-2/PNG/Particles white/particleWhite_1.png", 0.18, smooth=True)
    for col, dx, dy in [(2, -26, -26), (3, 4, 28), (4, 26, -22)]:
        put_center(img, spark, gx + col * cell + cell // 2 + dx, gy + 5 * cell + cell // 2 + dy)
    # 右の得点と次の宝石
    px = gx + cell * n + 44
    hud_panel(img, (px, 72, W - 32, 250), radius=16, fill=(34, 40, 62, 255))
    coin = kn.image("puzzle-pack-2/PNG/Coins/coin_01.png", 0.45, smooth=True)
    put(img, coin, px + 22, 100)
    d = ImageDraw.Draw(img)
    d.rectangle((px + 92, 116, W - 60, 136), fill=(150, 160, 190))
    d.rectangle((px + 22, 180, W - 60, 214), fill=(220, 200, 120))
    hud_panel(img, (px, 290, W - 32, 500), radius=16, fill=(34, 40, 62, 255))
    for i, k in enumerate((0, 3, 1)):
        put_center(img, gem(k, 52), px + 50 + i * 70, 350)
    d = ImageDraw.Draw(img)
    d.rectangle((px + 22, 420, W - 60, 438), fill=(110, 120, 160))
    d.rectangle((px + 22, 456, W - 120, 474), fill=(110, 120, 160))
    return img.convert("RGB")


def kenney_rpg(kn):
    # 上から見た RPG の町 (Tiny Town と、人物は Tiny Dungeon)。左上と左下は草だけにする。
    img = Image.new("RGBA", (W, H))
    t = 64  # 16 px のタイルを 4 倍

    def town(n):
        return kn.image(f"tiny-town/Tiles/tile_{n:04d}.png", 4)

    def person(n):
        return kn.image(f"tiny-dungeon/Tiles/tile_{n:04d}.png", 4)

    cols, rows = W // t, (H + t - 1) // t
    for c in range(cols):
        for r in range(rows):
            put(img, town(0), c * t, r * t)
    # 広場 (6 から 13 列目、5 から 8 段目) と、そこから上と右へ延びる道
    def dirt(c, r, n):
        put(img, town(n), c * t, r * t)

    c0, c1, r0, r1 = 6, 13, 5, 8
    for c in range(c0, c1 + 1):
        for r in range(r0, r1 + 1):
            top, bottom, left, right = r == r0, r == r1, c == c0, c == c1
            n = 25
            if top:
                n = 12 if left else 14 if right else 13
            elif bottom:
                n = 36 if left else 38 if right else 37
            elif left:
                n = 24
            elif right:
                n = 26
            dirt(c, r, n)
    for r in range(0, r0):
        dirt(9, r, 24)
        dirt(10, r, 26)
    dirt(9, r0, 25)
    dirt(10, r0, 25)
    for c in range(c1 + 1, cols):
        dirt(c, 6, 13)
        dirt(c, 7, 37)
    dirt(c1, 6, 25)
    dirt(c1, 7, 25)
    # 家 (灰色の屋根と木の壁、石の壁)。赤い屋根は使わない。
    def house(c, r, wall):
        for k, n in enumerate([48, 51, 50]):
            put(img, town(n), (c + k) * t, r * t)
        for k, n in enumerate([60, 61, 62]):
            put(img, town(n), (c + k) * t, (r + 1) * t)
        row = [84, 86, 84] if wall == "wood" else [88, 90, 88]
        for k, n in enumerate(row):
            put(img, town(n), (c + k) * t, (r + 2) * t)

    house(6, 1, "wood")
    house(12, 1, "stone")
    house(16, 2, "wood")
    # 木と柵。木は葉の濃い 0016 と 0028 と茂み 0005 を使う (0004 の葉は草と同じ色で、輪郭だけに見える)。
    for c, r, n in [(5, 2, 28), (11, 0, 16), (19, 5, 16), (17, 9, 16), (19, 8, 28), (15, 9, 5),
                    (12, 10, 5), (19, 10, 16)]:
        put(img, town(n), c * t, r * t)
    for k, n in enumerate([80, 81, 81, 81, 82]):
        put(img, town(n), (14 + k) * t, 5 * t)
    put(img, town(83), 14 * t, 8 * t)  # 看板
    # 人物: 主人公 (剣士)、町の人、草むらの敵
    put(img, person(96), 9 * t, 6 * t)
    put(img, person(99), 13 * t, 4 * t)
    put(img, person(84), 7 * t, 7 * t)
    put(img, person(108), 17 * t, 7 * t + 10)
    # 右上の小さな状態のパネル (顔と 2 本のバー)
    hud_panel(img, (W - 300, 20, W - 24, 112), fill=(24, 28, 40, 210))
    put(img, kn.image(f"tiny-dungeon/Tiles/tile_{96:04d}.png", 4), W - 288, 34)
    d = ImageDraw.Draw(img)
    d.rectangle((W - 206, 44, W - 48, 60), fill=(110, 200, 120, 255))
    d.rectangle((W - 206, 76, W - 96, 92), fill=(100, 150, 230, 255))
    return img.convert("RGB")


def mute(img, strength):
    # strength は (彩度, コントラスト, 明るさ) の倍率 (SHAPES_MUTE か KENNEY_MUTE)。
    color, contrast, brightness = strength
    img = ImageEnhance.Color(img).enhance(color)
    img = ImageEnhance.Contrast(img).enhance(contrast)
    return ImageEnhance.Brightness(img).enhance(brightness)


def even_brightness(images):
    # 4 枚の明るさ (L の平均) を 4 枚の平均にそろえる。倍率は KENNEY_EVEN_RANGE の範囲に止める。返す値は
    # (画像, 倍率) の並び。
    lumas = [ImageStat.Stat(img.convert("L")).mean[0] for img in images]
    target = sum(lumas) / len(lumas)
    lo, hi = KENNEY_EVEN_RANGE
    result = []
    for img, luma in zip(images, lumas):
        factor = min(hi, max(lo, target / luma))
        result.append((ImageEnhance.Brightness(img).enhance(factor), factor))
    return result


def main():
    parser = argparse.ArgumentParser(description="README の画像に使う、枠の中の 4 つの画像を描く")
    parser.add_argument("--out", required=True, help="書き出すフォルダー")
    parser.add_argument("--vivid", action="store_true", help="加工しない (素材のまま書き出す)")
    parser.add_argument("--style", choices=["shapes", "kenney"], default="shapes",
                        help="shapes は図形だけで描いた画面 (既定)、kenney は Kenney の素材で組んだ画面")
    parser.add_argument("--assets", help="--style kenney の素材のフォルダー (パックごとのフォルダーの親)")
    args = parser.parse_args()
    out = Path(args.out)

    if args.style == "kenney":
        root = Path(args.assets) if args.assets else None
        missing = KENNEY_PACKS if root is None else missing_kenney_packs(root)
        if missing:
            print_kenney_help(root if root is not None else Path("<素材のフォルダー>"), missing)
            if root is None:
                print("--assets で素材のフォルダーを指定してください。", file=sys.stderr)
            return 2
        kn = Kenney(root)
        makers = [("tile_tl", lambda: kenney_platformer(kn)), ("tile_tr", lambda: kenney_racing(kn)),
                  ("tile_bl", lambda: kenney_puzzle(kn)), ("tile_br", lambda: kenney_rpg(kn))]
    else:
        makers = [("tile_tl", adventure), ("tile_tr", racing), ("tile_bl", puzzle), ("tile_br", strategy)]

    names = [name for name, _ in makers]
    images = [make() for _, make in makers]
    notes = [""] * len(images)
    if not args.vivid:
        strength = KENNEY_MUTE if args.style == "kenney" else SHAPES_MUTE
        images = [mute(img, strength) for img in images]
        if args.style == "kenney":
            evened = even_brightness(images)
            images = [img for img, _ in evened]
            notes = [f"  (明るさの倍率 {factor:.2f})" for _, factor in evened]

    out.mkdir(parents=True, exist_ok=True)
    for name, img, note in zip(names, images, notes):
        img.save(out / f"{name}.png")
        print(f"{out / f'{name}.png'}{note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
