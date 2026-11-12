# 録画 (2x2 に 4 ストリーム、各 0.5 倍) から、枠ごとの欠落・重複を 10 秒ごとに数える。
# 番号を読めないフレーム (下の「数えないフレーム」) の前後は数えない。
# 使い方: python smooth.py <録画> [枠の数 (既定 4)] [--regions x,y,w,h;x,y,w,h;...] [--cell 8]
#   --regions がなければ 1920x1080 の 4 象限 (960x540) の左上から読む。
#   --regions があれば枠ごとの実際の位置と大きさ (実行スクリプトが OBS のシーンアイテムから求めたもの) を使う。
#   1280x720 の共有を 0.5 倍で置いた枠は 640x360 なので、象限の全体を見ると白のマーカーが
#   平均で 200 に届かず、1 つも白と見なせない。--cell は古い形の帯のセルの一辺 (既定 8 = 16 px の 0.5 倍)。
#
# 動作 (出力のキー: 帯が見つからない領域は数のキーを出さない。下の「出力のキー」):
#   - 10 秒ごとの区切りと「15 秒以降を定常」はフレームの pts で決める。「60 フレーム = 1 秒」の番号で数えると、
#     30 fps の録画では区切りが 2 倍の長さになる。
#   - フレームは ffmpeg が一定の fps に揃えずにそのまま出す (video_frames.py)。
#   - 枠の全部を囲む四角だけを ffmpeg の側で切り出す。
#   - 帯を枠の領域の中で探して読む (frames.py と同じ。bit_strip.py)。実際の共有 (pattern.html / sync-video.mp4 の
#     時計つきの帯。ブラウザのタブの列やページの余白の下に縮められて写る) では、枠の左上に帯が等倍で載っている
#     前提では番号を読めない。領域ごとに、時計つきの帯が 1 フレームでも読めればそれを使い (clock)、
#     読めなければ古い形の帯 (セル --cell の 32 個) を領域の左上の固定の位置で読み、番号が動いていればそれを使う (legacy。
#     テスト用の送信側と make_material.py の古い形の帯のテストデータ)。どちらでもなければ、その領域は数えずに
#     barcode_found: false と error を出す。「番号が動いている」の決め方は frames.py の関数をそのまま使う
#     (2 つの判定スクリプトで帯の有無の答えが食い違わないように)。そのため、古い形の帯の番号が最初から最後まで
#     1 つも動かない領域 (ずっと凍ったテスト用の送信側) は、重複の数ではなく frames.py と同じく「見つからない」になる。
#   - 数えないフレーム: clock は読めないフレーム (帯が見えない・CRC が合わない) だけ。白のマーカーのフレームも帯を白の上に描く
#     (make_sync_video.py の render_frame) ので、読めれば数える。legacy は白のマーカーのフレーム (領域の平均が
#     LEGACY_WHITE_MEAN を超える) を除く (テスト用の送信側の白は枠いっぱいなので、この境目でよい)。
#
# 出力のキー (領域ごとに足したもの): layout ("clock" / "legacy" / null)、barcode_found、clock なら readable_frames /
#   unreadable_frames / crc_failures / geometry (帯の位置が変わるたびの {at, x, y, cell_w, cell_h}。録画の座標)、
#   legacy なら white_frames (除いた白のフレームの数) と legacy (位置とセル)。見つからない領域は per10s /
#   total_missing_dup_back / steady_* を出さずに error と legacy_probe を出す (0 を並べると「欠落も重複もない」に見えるため)。
import json
import sys

import bit_strip
import frames
import video_frames

STEADY_FROM_S = 15.0  # 15 秒以降を定常とみなす
BUCKET_S = 10
LEGACY_WHITE_MEAN = 200  # 古い形の帯のテストデータで白のマーカーのフレームを除く境目 (前のまま)


def parse_regions(text):
    regions = []
    for part in str(text).split(";"):
        part = part.strip()
        if not part:
            continue
        x, y, w, h = (int(v) for v in part.split(","))
        regions.append((x, y, w, h))
    return regions


def parse_args(argv):
    positional = []
    regions = None
    cell = 8
    i = 2
    while i < len(argv):
        a = argv[i]
        if a == "--regions":
            regions = parse_regions(argv[i + 1])
            i += 2
        elif a == "--cell":
            cell = int(argv[i + 1])
            i += 2
        else:
            positional.append(a)
            i += 1
    ncells = int(positional[0]) if positional else 4
    return ncells, regions, cell


def count_steps(values, skip, times):
    """隣り合うフレームの番号の差から、10 秒ごと・全体・定常の [欠落, 重複, 逆行] を数える (数え方は前のまま)。
    skip[i] が真のフレームは、その前後の差を数えない。"""
    t0 = times[0] if times else 0.0
    buckets = {}
    total = [0, 0, 0]
    steady = [0, 0, 0]
    for i in range(1, len(values)):
        if skip[i] or skip[i - 1]:
            continue
        d = values[i] - values[i - 1]
        rel = times[i] - t0
        sec = int(rel // BUCKET_S) * BUCKET_S
        b = buckets.setdefault(sec, [0, 0, 0])
        k = 1 if d == 0 else (0 if d > 1 else (2 if d < 0 else -1))
        if k >= 0:
            v = d - 1 if k == 0 else 1
            b[k] += v
            total[k] += v
            if rel >= STEADY_FROM_S:
                steady[k] += v
    return buckets, total, steady


def cell_result(ci, region, tracker, clock_values, legacy_values, whites, times, cell, steady_frames):
    """1 つの領域の結果。帯の種類を選び (frames.py の --layout auto と同じ選び方)、選んだ帯の番号で数える。"""
    x, y, w, h = region
    entry = {"cell": ci, "region": {"x": x, "y": y, "w": w, "h": h}}
    plausible, probe = frames.legacy_probe(legacy_values)
    if tracker.ok_frames > 0:
        values = clock_values
        skip = [v is None for v in values]
        unreadable = sum(skip)
        entry.update(layout="clock", barcode_found=True, readable_frames=len(values) - unreadable,
                     unreadable_frames=unreadable, crc_failures=tracker.crc_failures,
                     geometry=[dict(g.to_dict(x, y), at=at) for at, g in tracker.geometries[:20]])
    elif plausible:
        values = legacy_values
        skip = whites
        entry.update(layout="legacy", barcode_found=True, white_frames=sum(whites), legacy={"x": x, "y": y, "cell": cell})
    else:
        entry.update(layout=None, barcode_found=False, error=frames.not_found_error(tracker, probe, x, y, cell),
                     legacy_probe=dict(probe, x=x, y=y, cell=cell))
        return entry
    buckets, total, steady = count_steps(values, skip, times)
    entry.update({"per10s": [(k, v) for k, v in sorted(buckets.items())], "total_missing_dup_back": total,
                  "steady_missing_dup_back": steady, "steady_frames": steady_frames,
                  "steady_dup_pct": round(100.0 * steady[1] / max(1, steady_frames), 1)})
    return entry


def main():
    path = sys.argv[1]
    ncells, regions, cell = parse_args(sys.argv)
    info = video_frames.probe_video(path)
    W, H = info["width"], info["height"]
    if regions is None:
        regions = [(0, 0, 960, 540), (960, 0, 960, 540), (0, 540, 960, 540), (960, 540, 960, 540)]
        regions_source = "fixed-quadrants"
    else:
        regions_source = "given"
    regions = regions[:ncells]
    clamped = []
    for r in regions:
        c = video_frames.clamp_region(r[0], r[1], r[2], r[3], W, H)
        if c is None:
            raise SystemExit("region is outside the video: %s" % (r,))
        clamped.append(c)
    # 枠の全部と古い形の帯のセルの帯を囲む四角を切り出す (古い形の帯は枠の左上から 32 セルぶん。枠より広いこともある)
    x0 = min(c[0] for c in clamped)
    y0 = min(c[1] for c in clamped)
    x1 = min(W, max(max(c[0] + c[2], c[0] + cell * 32) for c in clamped))
    y1 = min(H, max(max(c[1] + c[3], c[1] + cell) for c in clamped))
    box = (x0, y0, x1 - x0, y1 - y0)

    trackers = [bit_strip.StripTracker() for _ in clamped]
    clock_values = [[] for _ in clamped]
    legacy_values = [[] for _ in clamped]
    whites = [[] for _ in clamped]
    times = []
    for index, (pts, img) in enumerate(video_frames.iter_gray_frames(path, W, H, box)):
        times.append(pts)
        for ci, (cx, cy, cw, ch) in enumerate(clamped):
            lx, ly = cx - x0, cy - y0
            part = img[ly:ly + ch, lx:lx + cw]
            status, value = trackers[ci].feed(part, index)
            clock_values[ci].append(value if status == "ok" else None)
            legacy_values[ci].append(frames.read_legacy(img, lx, ly, cell))
            whites[ci].append(float(part.mean()) > LEGACY_WHITE_MEAN)
    n = len(times)
    t0 = times[0] if times else 0.0
    steady_frames = sum(1 for t in times if t - t0 >= STEADY_FROM_S)
    fps = video_frames.fps_from_pts(times)
    out = {"file": path, "frames": n, "fps": round(fps, 3) if fps else None, "time_source": "pts",
           "regions_source": regions_source, "cell": cell, "cells": []}
    for ci, region in enumerate(clamped):
        out["cells"].append(cell_result(ci, region, trackers[ci], clock_values[ci], legacy_values[ci], whites[ci],
                                        times, cell, steady_frames))
    print(json.dumps(out, ensure_ascii=False))


if __name__ == "__main__":
    main()
