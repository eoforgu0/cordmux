# SPDX-License-Identifier: Apache-2.0

# プレビューツールの画像を、1 型・2 型の色覚の見え方と白黒に変換して並べる。メッセージのレベルの見分けは色だけに頼らない
# (アイコンの形とパネルの有無でも見分ける) ので、ここで数えるのは、変換した後もテキストが背景から読めるか
# (contrast.tsv のコントラスト比) だけである。変換は Machado ほか (2009) の行列 (強さ 1.0。線形 RGB に 3x3 の行列を
# 掛ける) で、白黒は WCAG の相対輝度を sRGB に戻した灰色。
#   python cvd_sheet.py --png <画像> [<画像> ...] --out <出力先>   変換した画像と、4 つを横に並べた画像を書く
#   python cvd_sheet.py --contrast <contrast.tsv> [...]            テキストと背景の比を、元の見え方と変換した
#                                                                  3 つの見え方で計算し、それぞれの最小を出力する
#   python cvd_sheet.py --selftest                                 行列と変換の結果を期待値と比べる

import argparse
import csv
import os
import re
import sys

M = {
    "protan": [[0.152286, 1.052583, -0.204868], [0.114503, 0.786281, 0.099216], [-0.003882, -0.048116, 1.051998]],
    "deutan": [[0.367322, 0.860646, -0.227968], [0.280085, 0.672501, 0.047413], [-0.011820, 0.042940, 0.968881]],
}


def lin(c):
    c = c / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def unlin(v):
    v = max(0.0, min(1.0, v))
    s = 12.92 * v if v <= 0.0031308 else 1.055 * (v ** (1 / 2.4)) - 0.055
    return int(round(s * 255))


def luminance(rgb):
    r, g, b = (lin(x) for x in rgb)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def contrast(a, b):
    la, lb = luminance(a), luminance(b)
    return (max(la, lb) + 0.05) / (min(la, lb) + 0.05)


def transform(rgb, kind):
    if kind == "normal":
        return rgb
    if kind == "gray":
        g = unlin(luminance(rgb))
        return (g, g, g)
    v = [lin(x) for x in rgb]
    m = M[kind]
    return tuple(unlin(sum(m[i][j] * v[j] for j in range(3))) for i in range(3))


def hex2rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


KINDS = ["normal", "protan", "deutan", "gray"]


def do_png(paths, out):
    from PIL import Image
    os.makedirs(out, exist_ok=True)
    # 色ごとに 1 回だけ計算する (画像の色の数は少ない)。
    cache = {}
    for path in paths:
        im = Image.open(path).convert("RGB")
        # 出力先の形 face-<描画の対象>/<テーマ>/<言語>/<名前>.png から、テーマと言語を名前の先頭に付ける (テーマと言語が
        # 違う同じ場面の画像を、上書きしないように)。
        parts = os.path.normpath(path).split(os.sep)
        base = os.path.splitext(parts[-1])[0]
        if len(parts) >= 3:
            base = "%s-%s-%s" % (parts[-3], parts[-2], base)
        outs = []
        for kind in KINDS:
            if kind == "normal":
                outs.append(im)
                continue
            px = im.load()
            t = Image.new("RGB", im.size)
            tp = t.load()
            for y in range(im.height):
                for x in range(im.width):
                    c = px[x, y]
                    key = (c, kind)
                    if key not in cache:
                        cache[key] = transform(c, kind)
                    tp[x, y] = cache[key]
            t.save(os.path.join(out, "%s-%s.png" % (base, kind)))
            outs.append(t)
        sheet = Image.new("RGB", (sum(o.width for o in outs) + 8 * (len(outs) - 1), im.height), (128, 128, 128))
        x = 0
        for o in outs:
            sheet.paste(o, (x, 0))
            x += o.width + 8
        sheet.save(os.path.join(out, "%s-sheet.png" % base))
        print("wrote %s-sheet.png" % base)


def do_contrast(paths):
    worst = {k: (99.0, "") for k in KINDS}
    n = 0
    for path in paths:
        with open(path, encoding="utf-8") as f:
            for r in csv.DictReader(f, delimiter="\t", quoting=csv.QUOTE_NONE):
                if r.get("enabled") != "yes":
                    continue
                fg, bg = hex2rgb(r["fg"]), hex2rgb(r["bg"])
                n += 1
                for k in KINDS:
                    ratio = contrast(transform(fg, k), transform(bg, k))
                    if ratio < worst[k][0]:
                        worst[k] = (ratio, "%s %s %s/%s %s" % (r["shot"], r["object_name"] or r["class"], r["fg"],
                                                               r["bg"], r["text"][:16]))
    print("pairs %d" % n)
    for k in KINDS:
        print("%s\tmin %.2f\t%s" % (k, worst[k][0], worst[k][1]))


# --selftest の期待値 (色、見え方、変換後の色)。上の行列とこのファイルの変換で求めた値なので、行列か変換の式が変わると
# 合わなくなる。
EXPECTED = [
    ((192, 28, 55), "protan", (82, 77, 55)),
    ((192, 28, 55), "deutan", (122, 110, 49)),
    ((37, 162, 49), "protan", (165, 146, 33)),
    ((37, 162, 49), "deutan", (151, 137, 60)),
    ((0, 120, 215), "protan", (68, 128, 219)),
    ((0, 120, 215), "deutan", (19, 111, 213)),
    ((255, 204, 0), "protan", (230, 202, 0)),
    ((255, 204, 0), "deutan", (242, 216, 32)),
    ((192, 28, 55), "gray", (98, 98, 98)),
]


def selftest():
    ok = True
    # 行列の各行の和は 1 (白は白のまま)。値を 1 つでも書き間違えると和がずれる。
    for kind, rows in M.items():
        for i, row in enumerate(rows):
            if abs(sum(row) - 1.0) > 1e-5:
                print("行列 %s の %d 行目の和が %.6f (1 のはず)" % (kind, i, sum(row)))
                ok = False
    for rgb, kind, want in EXPECTED:
        got = transform(rgb, kind)
        if got != want:
            print("%s を %s で変換した結果が %s (期待値は %s)" % (rgb, kind, got, want))
            ok = False
    # 白と黒の比は 21、灰色は相対輝度を保つ、1 型と 2 型で赤と緑の色の隔たりが縮む (明るさの比は縮むとは限らない)。
    checks = []
    checks.append(abs(contrast((255, 255, 255), (0, 0, 0)) - 21.0) < 1e-9)
    g = transform((192, 28, 55), "gray")
    checks.append(abs(luminance(g) - luminance((192, 28, 55))) < 0.005)
    red, green = (192, 28, 55), (37, 162, 49)

    def dist(a, b):
        return sum((a[i] - b[i]) ** 2 for i in range(3)) ** 0.5

    for kind in ("protan", "deutan"):
        checks.append(dist(transform(red, kind), transform(green, kind)) < dist(red, green))
    checks.append(transform((255, 255, 255), "protan") == (255, 255, 255))
    for i, c in enumerate(checks):
        if not c:
            print("check %d failed" % i)
    ok = ok and all(checks)
    print("selftest", "ok" if ok else "FAILED")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--png", nargs="*")
    ap.add_argument("--contrast", nargs="*")
    ap.add_argument("--out")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if a.png:
        if not a.out:
            print("--out is required with --png", file=sys.stderr)
            return 2
        do_png(a.png, a.out)
    if a.contrast:
        do_contrast(a.contrast)
    return 0


if __name__ == "__main__":
    sys.exit(main())
