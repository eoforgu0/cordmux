# 録画の判定スクリプト (3/3): 送った AU と完成の mov の AU が同じものかを見る。
# 粒度は区間ごとの framemd5 である。入口も出力も AVCC なので、区間 fMP4 の先頭 150 AU が
# 出力の先頭 150 AU と一致する。録画ファイルは -c copy の多重化で作り、avcC がビット一致する枝は
# 再エンコードしない。
#
# 見るもの: 区間 fMP4 の映像の AU の (大きさ, md5) の並びが、完成の mov の中に
# 連続して、区間の順番どおりに現れるか。pts / dts は比べない (多重化で刻みが
# 付け直され、黒埋めが間に挟まるため)。大きさと md5 だけで AU の同一性は決まる。
#
# 使い方:
#   python au_hash.py --session-dir <セッションのフォルダー> [--user <user_id>] [--ffmpeg <場所>]
#   python au_hash.py --output <完成> --segments <区間 1> <区間 2> ... [--ffmpeg <場所>]
#   python au_hash.py --selftest [--ffmpeg <場所>] [--outdir <テストデータの保存先>] [--keep]
#
# 結果は 1 つの JSON を標準出力に書く。ok が true なら終了コード 0、false なら 1。
#
# ---- 判断 ----------------------------------------------------------
# (1) 突き合わせは framemd5 の (大きさ, md5) の組だけで行う。framemd5 は
#     stream / dts / pts / duration / size / hash を出すが、時刻の 4 つは多重化で変わる。
# (2) 出力の中の位置は「探して見つける」。録画ログの欠落の情報 (GapFill) から計算しない。
#     GapFill の形は契約になく、黒埋めの並べ方が変わっても判定が壊れないほうがよい。
#     区間の先頭の AU を出力の中から探し、そこから区間の長さぶんが連続一致するかを見る。
#     次の区間は「前の区間が終わった位置より後」からだけ探す (順番の検査を兼ねる)。
# (3) 録画ログの au_hash の欄はまだ読まない。src-obs/record/record_types.h の
#     LedgerSegmentRow::au_hash は「区間ごとの framemd5」とだけ決まっていて、何を md5 に
#     するか (framemd5 の出力全体か、AU の連結か) が決まっていない。決まったらここで
#     突き合わせる。それまでは実ファイルどうしの比較だけを行う。
# (4) 音は見ない。見るのは AU (access unit = 映像の 1 フレーム) の同一性で、PCM は
#     ledger_check.py のサンプル数の突き合わせで足りる。
# (5) 区間の先頭の AU だけは「同じか長い」を一致とする (locate() のコメントを参照)。
#     mov の muxer が継ぎ目の最初のサンプルに SPS/PPS を前置きするため。
# (6) 再エンコードの枝 (ProRes) の完成は突き合わせない (from_session のコメントを参照)。
#     AU が同じであることは再エンコードしない枝についての約束で、ProRes では AU が作り直されるのが正しい。
# (7) 同じものを指す行は後の行が優先する (製品の parse_ledger と ledger_check.py の (6) と同じ読み方)。
#     segment は file、finalize は output_file をキーにし、並びは最初に出た位置を保つ。録画の側は
#     区間を開いた瞬間にも segment の行を書くので、全部を並べると同じ区間を 2 度数え、
#     2 度目の区間を前の区間の後ろに探して RED になる。--selftest の session_reopened_rows が確認する。
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys

MOV_FLAGS = "+frag_keyframe+empty_moov+default_base_moof"
SELFTEST_FPS = 60
SELFTEST_SIZE = "320x240"


def resolve_ffmpeg(ffmpeg):
    return ffmpeg or "ffmpeg"


def framemd5_units(ffmpeg, path):
    """映像の AU の (大きさ, md5) の並びを返す 。"""
    cmd = [ffmpeg, "-hide_banner", "-loglevel", "error", "-i", path,
           "-map", "0:v:0", "-c", "copy", "-f", "framemd5", "-"]
    r = subprocess.run(cmd, capture_output=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise RuntimeError("ffmpeg (framemd5) が失敗した: " + path + "\n" + (r.stderr or "")[-400:])
    units = []
    for line in (r.stdout or "").split("\n"):
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        fields = [f.strip() for f in s.split(",")]
        if len(fields) < 6:
            continue
        try:
            size = int(fields[4])
        except ValueError:
            continue
        units.append((size, fields[5]))
    return units


def find_run(haystack, needle, start_at):
    """needle が haystack の start_at 以降に連続して現れる位置を返す (無ければ -1)。"""
    if not needle:
        return -1
    limit = len(haystack) - len(needle)
    i = start_at
    while i <= limit:
        if haystack[i] == needle[0] and haystack[i:i + len(needle)] == needle:
            return i
        i += 1
    return -1


def locate(out_units, units, cursor):
    """区間の AU の並びを出力の中から探す。返すのは (先頭の位置, 先頭の AU の扱い) か
    (-1, 理由)。

    先頭の AU だけは大きさが増えうる。mov の muxer は区間の
    継ぎ目の最初のサンプルに SPS/PPS を前置きするので、区間 fMP4 の先頭 AU (3538 バイト)
    が完成では 3576 バイトになる実測がある (差はちょうど avcC の中身ぶん)。
    2 番目以降の AU は 1 バイトも変わらない。したがって
    「2 番目以降がそのまま連続一致し、先頭は同じか長い」を一致とする。
    """
    at = find_run(out_units, units, cursor)
    if at >= 0:
        return at, "exact"
    if len(units) < 2:
        return -1, "見つからない (AU が 1 つだけの区間)"
    tail = units[1:]
    at_tail = find_run(out_units, tail, cursor + 1)
    if at_tail < 1:
        return -1, "見つからない"
    head = out_units[at_tail - 1]
    if head[0] < units[0][0]:
        return -1, "2 番目以降は合うが、先頭の AU が区間より小さい"
    return at_tail - 1, "head_grew_by_" + str(head[0] - units[0][0])


def compare(ffmpeg, output_path, segment_paths):
    out_units = framemd5_units(ffmpeg, output_path)
    checks = []
    cursor = 0
    ok = True
    for i, seg in enumerate(segment_paths):
        name = os.path.basename(seg)
        if not os.path.exists(seg):
            checks.append({"segment": name, "ok": False, "detail": "区間のファイルがない"})
            ok = False
            continue
        units = framemd5_units(ffmpeg, seg)
        at, note = locate(out_units, units, cursor)
        if at < 0:
            # どこにもないのか、順番が逆なのかを分けて書く (切り分けのため)。
            anywhere, _ = locate(out_units, units, 0)
            detail = (note if anywhere < 0
                      else "見つかったが順番が前 (位置 " + str(anywhere) + " < " + str(cursor) + ")")
            checks.append({"segment": name, "ok": False, "au_count": len(units), "detail": detail})
            ok = False
            continue
        checks.append({"segment": name, "ok": True, "au_count": len(units), "at": at, "head": note})
        cursor = at + len(units)
    return {"ok": ok, "output": output_path, "output_au_count": len(out_units), "checks": checks}


# ---- 録画ログからの読み取り ------------------------------------------------------
# ledger_check.py と同じ見分け方 (type のキーがあればそれ、無ければ他にないキーの有無) を
# 使うが、このツールが要るのは segment と finalize の 2 種類だけなので、判定スクリプトどうしを
# 依存させずに小さく持つ。


# テストデータのフォルダーの名前 (src-obs/record/record_types.h の kMaterialsDirName と同じ綴り)。
MATERIALS_DIR_NAME = "data"


def materials_dir_of(session_dir):
    """セッションのフォルダーから録画ログと区間の保存先を決める (ledger_check.py と同じ)。
    現在の並びなら <セッションのフォルダー>/data、古い並び (録画ログが直下) ならセッションのフォルダーそのもの。
    区間の file の欄はここからの相対、完成の output_file はセッションのフォルダーからの相対。"""
    candidate = os.path.join(session_dir, MATERIALS_DIR_NAME)
    if os.path.isdir(candidate):
        return candidate
    return session_dir


def read_rows(session_dir):
    text = pathlib.Path(os.path.join(materials_dir_of(session_dir), "ledger.jsonl")).read_text(
        encoding="utf-8", errors="replace")
    segments = []
    finalizes = []
    for line in text.split("\n"):
        s = line.strip()
        if not s:
            continue
        try:
            row = json.loads(s)
        except ValueError:
            continue  # ちぎれた末尾の 1 行
        if not isinstance(row, dict):
            continue
        named = row.get("type")
        if named == "segment" or (named is None and "close_reason" in row):
            upsert(segments, row, "file")
        elif named == "finalize" or (named is None and "output_file" in row):
            upsert(finalizes, row, "output_file")
    return segments, finalizes


def upsert(rows, row, key):
    """同じキーの行は後が優先され、並びは最初に出た位置を保つ。"""
    for i, existing in enumerate(rows):
        if existing.get(key) == row.get(key):
            rows[i] = row
            return
    rows.append(row)


def is_video_row(row):
    media = str(row.get("media", "")).lower()
    if media:
        return media.startswith("video")
    return bool(row.get("avcc_hex"))


def is_prores_branch(row):
    """その完成が再エンコードの枝 (ProRes 422 HQ) かどうか。"""
    return str(row.get("branch", "")).lower().startswith("prores")


def from_session(ffmpeg, session_dir, user_filter):
    segments, finalizes = read_rows(session_dir)
    results = []
    skipped = []
    ok = True
    for f in finalizes:
        if str(f.get("kind", "")).lower().startswith("voice"):
            continue  # 音だけの完成は AU を持たない
        user = f.get("user_id")
        if user_filter and user != user_filter:
            continue
        # 再エンコードの枝は見ない。「送った AU と mov の AU が同じ」は再エンコードしない枝 (Remux) についての
        # 約束で、ProRes の枝では AU が作り直されるので一致しないのが正しい。
        # 繋ぎ直しで avcC が食い違う枝を実機で踏ませる実行では、ここで RED にすると、狙いどおりに
        # 動いたときほど RED になる。
        if is_prores_branch(f):
            skipped.append({"user_id": user, "output_file": f.get("output_file"), "branch": f.get("branch")})
            continue
        out = os.path.join(session_dir, str(f.get("output_file", "")).replace("/", os.sep))
        seg_paths = [os.path.join(materials_dir_of(session_dir), str(g.get("file", "")).replace("/", os.sep))
                     for g in segments if g.get("user_id") == user and is_video_row(g)]
        if not seg_paths:
            results.append({"user_id": user, "ok": False, "detail": "映像の区間が 1 つもない"})
            ok = False
            continue
        r = compare(ffmpeg, out, seg_paths)
        r["user_id"] = user
        ok = ok and r["ok"]
        results.append(r)
    if not results and not skipped:
        return {"ok": False, "session_dir": session_dir, "error": "画面の録画ファイルの作成の行が 1 つもない"}
    # 数を出す: 全部が ProRes の枝だった場合は「1 つも突き合わせていない」ので、成功でも
    # 中身がない。読む側がそれを見分けられるようにする。
    return {"ok": ok, "session_dir": session_dir,
            "remux_checked": len(results), "prores_skipped": len(skipped),
            "results": results, "skipped": skipped}


# ---- --selftest --------------------------------------------------------------
# ffmpeg だけで区間 2 つと完成を作り、正常が合格、わざと壊した 3 通りが失敗になることを
# 確認する。
#   normal      区間 2 つを -c copy でつないだ完成 -> 成功
#   reencoded   完成を作り直す (再エンコード) -> AU が変わるので失敗
#   reordered   区間の順番を逆にして突き合わせる -> 順番の検査で失敗
#   missing     区間の 1 つを完成に入れない -> 失敗


def run_ffmpeg(ffmpeg, args):
    cmd = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y"] + args
    r = subprocess.run(cmd, capture_output=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise RuntimeError("ffmpeg が失敗した: " + " ".join(cmd) + "\n" + (r.stderr or "")[:800])


def make_segment(ffmpeg, path, pattern, seconds):
    run_ffmpeg(ffmpeg, [
        "-f", "lavfi", "-i", pattern + "=size=" + SELFTEST_SIZE + ":rate=" + str(SELFTEST_FPS) +
        ":duration=" + str(seconds),
        "-c:v", "libx264", "-g", str(SELFTEST_FPS), "-pix_fmt", "yuv420p",
        "-video_track_timescale", "60000", "-movflags", MOV_FLAGS, path])


def concat_copy(ffmpeg, out_path, parts, base):
    listing = os.path.join(base, "concat.txt")
    with open(listing, "w", encoding="utf-8", newline="\n") as f:
        for p in parts:
            f.write("file '" + os.path.basename(p) + "'\n")
    run_ffmpeg(ffmpeg, ["-f", "concat", "-safe", "0", "-i", listing, "-c", "copy",
                        "-video_track_timescale", "60000", out_path])


def build_session_fixture(base, name, seg1, seg2, remux_out, prores_out, prores_branch, legacy=False,
                          reopened=False):
    """録画ログから読む経路 (from_session) を当てるための小さなセッションのフォルダを作る。

    2 人ぶん置く: u1 は再エンコードゼロの枝 (区間をそのままつないだ完成)、u2 は同じ区間から
    作り直した完成。u2 の枝を ProRes と名乗れば飛ばされて成功、Remux と名乗れば失敗になる。
    legacy が偽なら現在の並び (録画ログと区間は data/ の下)、真なら古い並び (全部が直下)。
    reopened が真なら、区間の行の前に同じ file の開いた瞬間の行 (truncated 真・not_closed) を書く
    (製品の録画ログの形)。
    """
    session_dir = os.path.join(base, name, "20000101-000000" if legacy else "2000-01-01 00-00-00")
    materials_dir = session_dir if legacy else os.path.join(session_dir, MATERIALS_DIR_NAME)
    segments_dir = os.path.join(materials_dir, "segments")
    os.makedirs(segments_dir, exist_ok=True)
    for user in ("u1", "u2"):
        shutil.copyfile(seg1, os.path.join(segments_dir, user + "_screen_video_000001.mov"))
        shutil.copyfile(seg2, os.path.join(segments_dir, user + "_screen_video_000002.mov"))
    taro_out = "taro_screen.mov" if legacy else "taro Screen Share 2000-01-01 00-00-00.mov"
    hanako_out = "hanako_screen.mov" if legacy else "hanako Screen Share 2000-01-01 00-00-00.mov"
    shutil.copyfile(remux_out, os.path.join(session_dir, taro_out))
    shutil.copyfile(prores_out, os.path.join(session_dir, hanako_out))

    rows = []
    for user in ("u1", "u2"):
        for seq in (1, 2):
            row = {
                "type": "segment", "user_id": user, "kind": "screen", "media": "video",
                "file": "segments/" + user + "_screen_video_" + str(seq).zfill(6) + ".mov",
                "avcc_hex": "01", "close_reason": "recording_toggled",
            }
            if reopened:
                rows.append(dict(row, truncated=True, close_reason="not_closed"))
            rows.append(row)
    rows.append({"type": "finalize", "user_id": "u1", "kind": "screen", "branch": "remux",
                 "output_file": taro_out, "exit_code": 0})
    rows.append({"type": "finalize", "user_id": "u2", "kind": "screen",
                 "branch": "prores" if prores_branch else "remux",
                 "output_file": hanako_out, "exit_code": 0})
    text = "".join(json.dumps(r, ensure_ascii=False) + "\n" for r in rows)
    with open(os.path.join(materials_dir, "ledger.jsonl"), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return session_dir


def selftest(ffmpeg, outdir, keep):
    base = pathlib.Path(outdir) / "au-hash"
    if base.exists():
        shutil.rmtree(str(base), ignore_errors=True)
    base.mkdir(parents=True, exist_ok=True)
    results = []
    all_ok = True
    try:
        s1 = str(base / "seg1.mov")
        s2 = str(base / "seg2.mov")
        make_segment(ffmpeg, s1, "testsrc", 0.5)
        make_segment(ffmpeg, s2, "smptebars", 0.5)
        good = str(base / "good.mov")
        concat_copy(ffmpeg, good, [s1, s2], str(base))
        reenc = str(base / "reenc.mov")
        run_ffmpeg(ffmpeg, ["-i", good, "-c:v", "libx264", "-g", str(SELFTEST_FPS),
                            "-pix_fmt", "yuv420p", "-video_track_timescale", "60000", reenc])
        only1 = str(base / "only1.mov")
        run_ffmpeg(ffmpeg, ["-i", s1, "-c", "copy", "-video_track_timescale", "60000", only1])

        cases = [
            ("normal", "区間 2 つをつないだ完成", good, [s1, s2], "GREEN"),
            ("reencoded", "完成を再エンコードした", reenc, [s1, s2], "RED"),
            ("reordered", "区間の順番が逆", good, [s2, s1], "RED"),
            ("missing", "区間 2 が完成にない", only1, [s1, s2], "RED"),
        ]
        for name, label, out, segs, want in cases:
            try:
                r = compare(ffmpeg, out, segs)
                got = "GREEN" if r["ok"] else "RED"
                detail = [c for c in r["checks"] if not c["ok"]][:3]
            except Exception as e:  # noqa: BLE001
                got = "ERROR"
                detail = [str(e)[:300]]
            ok = got == want
            all_ok = all_ok and ok
            results.append({"case": name, "label": label, "want": want, "got": got, "ok": ok, "detail": detail})

        # 録画ログから読む経路で、ProRes の枝の完成を飛ばすこと。
        # 古い並び (録画ログが直下) のセッションも同じ読み方で GREEN になること。
        for name, label, prores_branch, want, legacy, reopened in [
            ("session_prores_skipped", "ProRes の枝は突き合わせない", True, "GREEN", False, False),
            ("session_prores_as_remux", "同じものを Remux と名乗ると失敗", False, "RED", False, False),
            ("session_legacy_layout", "古い並びのセッションも読む", True, "GREEN", True, False),
            ("session_reopened_rows", "区間ごとに開いた瞬間の行と閉じた行が並ぶ録画ログも成功", True,
             "GREEN", False, True),
        ]:
            try:
                session_dir = build_session_fixture(str(base), name, s1, s2, good, reenc, prores_branch,
                                                    legacy, reopened)
                r = from_session(ffmpeg, session_dir, None)
                got = "GREEN" if r["ok"] else "RED"
                detail = {"remux_checked": r.get("remux_checked"), "prores_skipped": r.get("prores_skipped")}
                # 成功のときは「1 つは本当に突き合わせた」ことも確認する (中身のない成功を通さない)。
                if got == "GREEN" and r.get("remux_checked", 0) < 1:
                    got = "RED"
            except Exception as e:  # noqa: BLE001
                got = "ERROR"
                detail = str(e)[:300]
            ok = got == want
            all_ok = all_ok and ok
            results.append({"case": name, "label": label, "want": want, "got": got, "ok": ok, "detail": detail})
    finally:
        if not keep:
            shutil.rmtree(str(base), ignore_errors=True)
    return {"ok": all_ok, "selftest": results}


def main():
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument("--session-dir", default=None)
    parser.add_argument("--user", default=None)
    parser.add_argument("--output", default=None)
    parser.add_argument("--segments", nargs="*", default=None)
    parser.add_argument("--ffmpeg", default=None)
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--outdir", default=None)
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()

    ffmpeg = resolve_ffmpeg(args.ffmpeg)
    if args.selftest:
        here = pathlib.Path(__file__).resolve().parent
        outdir = args.outdir or str(here.parents[2] / "build_x64" / "selfcheck")
        os.makedirs(outdir, exist_ok=True)
        result = selftest(ffmpeg, outdir, args.keep)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        sys.exit(0 if result["ok"] else 1)

    try:
        if args.session_dir:
            result = from_session(ffmpeg, os.path.abspath(args.session_dir), args.user)
        elif args.output and args.segments:
            result = compare(ffmpeg, args.output, args.segments)
        else:
            result = {"ok": False, "error": "--session-dir か (--output と --segments) か --selftest のどれかが要る"}
    except Exception as e:  # noqa: BLE001 (失敗を握って結果に書く)
        result = {"ok": False, "error": str(e)[:600]}
    print(json.dumps(result, ensure_ascii=False, indent=2))
    sys.exit(0 if result.get("ok") else 1)


if __name__ == "__main__":
    main()
