# 録画の映像からフレーム番号の帯を毎フレーム読み取り、欠落・重複・凍りを数える。
# 使い方: python frames.py <ファイル> [--x 0 --y 0] [--w 0 --h 0] [--cell 16] [--layout auto|clock|legacy]
#   --x/--y/--w/--h は領域 (録画の座標。実行スクリプトは枠の実際の位置と大きさを渡す)。--w/--h がなければ (x, y) から
#   録画の右下まで。--cell は古い形の帯 (下の legacy) のセルの一辺 (録画の上の px)。
#
# 帯は 2 種類ある:
#   clock  (時計つきの帯。bit_strip.py): share/pattern.html と make_sync_video.py の sync-video.mp4 (実際の共有の中身)。
#          領域の中を探し、位置とセルの大きさ (縦横別) を測って読む。1 フレームずつ CRC-8 で確認し、合わないフレームは
#          「読めない」にする (番号として数えない)。
#   legacy (古い形の帯。16 px のセル 32 個、白 = 1、最上位が左): sender.ts のテスト用の送信側と make_material.py のテストデータ。
#          領域の左上 (--x, --y) にセル --cell で等倍に載っているときだけ読める (固定の位置で読む)。
#   --layout auto (既定) は両方を読み、1 フレームでも clock が読めれば clock の結果を、読めなければ legacy の結果を出す。
#   clock の読みは帯の形 (時計・静か・棒・33 番の黒) と CRC-8 の両方が合ったときだけなので、帯のない絵から偶然に
#   出ることはないとみなせる。clock の帯がある共有では、領域の左上の固定の位置を読む legacy の値に意味はない。
#   実行スクリプトが手順ごとに --layout を選ぶ方法は採らない (テスト用の送信側か実際の共有かを実行計画に持たせる手間が
#   増え、取り違えると黙って 0 を数える)。フレームごとに多い方を選ぶ方法も採らない (同じ録画の中で 2 つの帯が混ざることはない)。
#   legacy は「番号が動いていること」(異なる値が 2 つ以上、増えた回数が 1 以上で減った回数の 2 倍以上) を確認し、
#   満たさなければ「帯が見つからない」とする。暗いタブの列を読むと値が 1 種類のままで、
#   動く絵の上を読むと増減がほぼ同数になる。テスト用の送信側の録画は欠落・凍り・可変 fps でも増える一方である。
#
# 動作:
#   - フレームは ffmpeg が一定の fps に揃えずにそのまま出す (video_frames.py)。可変の録画で ffmpeg が
#     フレームを複製・間引くと、それが重複や欠落に見えるため。
#   - 凍りの長さ (max_freeze_ms) はフレームの pts の差である。
#   - 帯を領域の中で探す (clock)。実際の共有の帯は枠の左上にない (ブラウザのウィンドウの共有ではタブの列などの
#     下、Chrome のタブの共有では動画の枠の中。bit_strip.py の冒頭を参照)。枠の左上の暗いタブの列を読むと
#     全部のフレームが 0 になる。
#   - 帯が見つからなければそう言う (barcode_found: false と error)。暗い所を読んだ 0 を番号として数えると、
#     「番号が 1 つも進まない凍り」に見えるため。終了コードは 0 のままにする (実行スクリプトは JSON を結果に残す。0 以外にすると
#     execFileSync が例外になり、何が読めなかったかの JSON が結果に残らない)。
#
# 出力のキー:
#   barcode_found, layout ("clock" / "legacy" / null), region, readable_frames, unreadable_frames,
#   crc_failures (clock の形は合ったが CRC が合わなかったフレーム), geometry (clock: 帯の位置が変わるたびの
#   {at, x, y, cell_w, cell_h}。録画の座標)、gaps_across_unreadable / missing_across_unreadable (読めないフレームを
#   挟んだ不連続。その間のフレームが録画にあったかは分からないので gap_count / missing_total に入れない)、
#   見つからないときは error と legacy_probe (固定の位置で読んだ値の様子)。
# 区間の数え方 (欠落・重複・逆行・凍り) は読めたフレームの並びで行う。「at」は録画の中のフレームの番号 (読めないフレームも数える)。
import json
import sys

import bit_strip
import video_frames

LAYOUTS = ("auto", "clock", "legacy")


def parse_args(argv):
    opts = {"x": 0, "y": 0, "w": 0, "h": 0, "cell": 16}
    layout = "auto"
    for i, a in enumerate(argv):
        if a.startswith("--") and a[2:] in opts:
            opts[a[2:]] = int(argv[i + 1])
        elif a == "--layout":
            layout = argv[i + 1]
    if layout not in LAYOUTS:
        raise SystemExit("--layout must be one of %s" % ", ".join(LAYOUTS))
    opts["layout"] = layout
    return opts


def read_legacy(img, x, y, cell):
    """前の帯 (セル cell の 32 個、左が最上位) を固定の位置で読む。帯が画像の外にかかったセルは 0。"""
    h, w = img.shape
    cy = y + cell // 2
    bits = 0
    for b in range(32):
        cx = x + cell // 2 + b * cell
        v = int(img[cy, cx]) if 0 <= cy < h and 0 <= cx < w else 0
        bits = (bits << 1) | (1 if v > 128 else 0)
    return bits


def legacy_probe(values):
    """固定の位置で読んだ値の並びの様子と、番号が動いているとみなせるか。"""
    ups = downs = 0
    for a, b in zip(values, values[1:]):
        if b > a:
            ups += 1
        elif b < a:
            downs += 1
    distinct = len(set(values))
    plausible = distinct >= 2 and ups >= 1 and ups >= 2 * downs
    probe = {"first": values[0] if values else None, "last": values[-1] if values else None,
             "distinct": distinct, "ups": ups, "downs": downs}
    return plausible, probe


def not_found_error(tracker, probe, x, y, cell):
    """帯が見つからないときの理由の文 (smooth.py も使う)。tracker が None なら時計つきの帯は探していない、
    probe が None なら前の帯は読んでいない。"""
    reasons = []
    if tracker is not None:
        reasons.append("時計つきの帯がどのフレームにもない (探した回数 %d、形は合ったが CRC が合わないフレーム %d)"
                       % (tracker.searches, tracker.crc_failures))
    if probe is not None:
        reasons.append("固定の位置 (x=%d y=%d セル %d) の前の帯は番号が動いていない (値 %d 種類、増 %d 回、減 %d 回、最初 %s)"
                       % (x, y, cell, probe["distinct"], probe["ups"], probe["downs"], probe["first"]))
    return "フレーム番号の帯が領域の中に見つからない: " + " / ".join(reasons)


def count_frames(values, times):
    """values はフレームごとの番号 (読めないフレームは None)。欠落・重複・逆行・凍りを数えた dict を返す。"""
    out = {}
    readable = [i for i, v in enumerate(values) if v is not None]
    if not readable:
        return out
    out["first"] = values[readable[0]]
    out["last"] = values[readable[-1]]
    gaps = []        # 欠落: 前の読めたフレームより 2 以上進んだ (間に読めないフレームがない)
    across = []      # 読めないフレームを挟んで 2 以上進んだ
    dups = []        # 重複: 前の読めたフレームと同じ番号 (凍り)
    backwards = []   # 逆行: 読み取りの失敗か、時刻の乱れ
    # 凍り = 同じ番号が続いた並び。長さは「最初のフレームの pts から、並びの最後のフレームの次のフレームの pts まで」
    # (最後まで続いた凍りは、最後のフレームの pts + フレームの間隔の中央値まで)。間の読めないフレームは凍りの中とみなす
    # (両側が同じ番号なので)。
    step_s = video_frames.median_interval(times) or 0.0
    n = len(values)
    max_run = 1
    max_freeze_s = 0.0
    run_start = readable[0]
    run_last = readable[0]

    def close_run():
        nonlocal max_run, max_freeze_s
        frames_in_run = run_last - run_start + 1
        if run_last > run_start:
            end_s = times[run_last + 1] if run_last + 1 < n else times[-1] + step_s
            max_freeze_s = max(max_freeze_s, end_s - times[run_start])
            max_run = max(max_run, frames_in_run)

    for prev, i in zip(readable, readable[1:]):
        d = values[i] - values[prev]
        if d == 0:
            dups.append(i)
            run_last = i
            continue
        close_run()
        if d > 1:
            entry = {"at": i, "missing": d - 1}
            if i - prev > 1:
                entry["unreadable_between"] = i - prev - 1
                across.append(entry)
            else:
                gaps.append(entry)
        elif d < 0:
            backwards.append({"at": i, "delta": d})
        run_start = run_last = i
    close_run()
    out["expected_span"] = out["last"] - out["first"] + 1
    out["missing_total"] = sum(g["missing"] for g in gaps)
    out["gaps"] = gaps[:50]
    out["gap_count"] = len(gaps)
    out["duplicate_count"] = len(dups)
    out["max_freeze_frames"] = max_run if max_run > 1 else 0
    out["max_freeze_ms"] = round(max_freeze_s * 1000, 1) if max_run > 1 else 0
    out["backwards_count"] = len(backwards)
    out["backwards"] = backwards[:20]
    out["gaps_across_unreadable"] = across[:50]
    out["gaps_across_unreadable_count"] = len(across)
    out["missing_across_unreadable"] = sum(g["missing"] for g in across)
    return out


def main():
    path = sys.argv[1]
    opts = parse_args(sys.argv)
    info = video_frames.probe_video(path)
    w, h = info["width"], info["height"]
    cell = max(1, opts["cell"])
    rw = opts["w"] if opts["w"] > 0 else w - opts["x"]
    rh = opts["h"] if opts["h"] > 0 else h - opts["y"]
    region = video_frames.clamp_region(opts["x"], opts["y"], rw, rh, w, h)
    if region is None:
        raise SystemExit("the region is outside the video: x=%d y=%d w=%d h=%d" % (opts["x"], opts["y"], rw, rh))
    layout = opts["layout"]
    use_clock = layout in ("auto", "clock")
    use_legacy = layout in ("auto", "legacy")
    # ffmpeg で切り出す四角 = 探す領域 + 前の帯のセルの帯 (帯は領域より右にはみ出すことがある)。
    boxes = [region]
    if use_legacy:
        strip = video_frames.clamp_region(opts["x"], opts["y"], cell * 32, cell, w, h)
        if strip is not None:
            boxes.append(strip)
    bx0 = min(b[0] for b in boxes)
    by0 = min(b[1] for b in boxes)
    bx1 = max(b[0] + b[2] for b in boxes)
    by1 = max(b[1] + b[3] for b in boxes)
    box = (bx0, by0, bx1 - bx0, by1 - by0)
    rx0, ry0 = region[0] - bx0, region[1] - by0

    tracker = bit_strip.StripTracker()
    clock_values = []
    legacy_values = []
    times = []
    for index, (pts, img) in enumerate(video_frames.iter_gray_frames(path, w, h, box)):
        times.append(pts)
        if use_clock:
            status, value = tracker.feed(img[ry0:ry0 + region[3], rx0:rx0 + region[2]], index)
            clock_values.append(value if status == "ok" else None)
        if use_legacy:
            legacy_values.append(read_legacy(img, opts["x"] - bx0, opts["y"] - by0, cell))

    fps = video_frames.fps_from_pts(times)
    out = {"file": path, "width": w, "height": h, "fps": round(fps, 3) if fps else None, "r_frame_rate": info["r_frame_rate"],
           "time_source": "pts", "frames": len(times),
           "region": {"x": region[0], "y": region[1], "w": region[2], "h": region[3]}}
    plausible, probe = legacy_probe(legacy_values) if use_legacy else (False, None)
    chosen = None
    if use_clock and tracker.ok_frames > 0:
        chosen = "clock"
        values = clock_values
        out["crc_failures"] = tracker.crc_failures
        out["geometry"] = [dict(g.to_dict(region[0], region[1]), at=at) for at, g in tracker.geometries[:20]]
        out["geometry_changes"] = max(0, len(tracker.geometries) - 1)
    elif use_legacy and plausible:
        chosen = "legacy"
        values = legacy_values
        out["legacy"] = {"x": opts["x"], "y": opts["y"], "cell": cell}
    out["layout"] = chosen
    out["barcode_found"] = chosen is not None
    if chosen is None:
        out["error"] = not_found_error(tracker if use_clock else None, probe, opts["x"], opts["y"], cell)
        if probe is not None:
            out["legacy_probe"] = dict(probe, x=opts["x"], y=opts["y"], cell=cell)
        print(json.dumps(out, indent=1, ensure_ascii=False))
        return
    readable = sum(1 for v in values if v is not None)
    out["readable_frames"] = readable
    out["unreadable_frames"] = len(values) - readable
    out.update(count_frames(values, times))
    print(json.dumps(out, indent=1, ensure_ascii=False))


if __name__ == "__main__":
    main()
