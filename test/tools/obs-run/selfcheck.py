# 判定スクリプト (frames.py / sync.py / smooth.py / analyze.py) のセルフテスト。
#
# make_material.py で「正常」+ 壊し方 6 通り (欠落 1 フレーム / 100・300・500 ms の凍り /
# 音 100 ms ずれ / 重複) のテストデータを作り、4 つの判定スクリプトに渡して、期待した色 (GREEN/RED) に
# なっているかを表にする。全部一致すれば終了コード 0、1 つでも食い違えば 1。
# 次の 2 通りも作る。
#   - 1 kHz が鳴り続けるデータ (テスト用のマイクの 1 kHz が録画に混ざった形。マーカーを 2 kHz の強さだけで
#     探す方式は、マーカーを 1 つも見つけられない)。
#   - 可変フレームレートのデータ (cordmux の録画の形。sync.py はフレームの pts で時刻を出すので GREEN、
#     frames.py / smooth.py はセルの番号が 2 つずつ進むので欠落として RED)。
# 実際の画面共有の形も作る (frames.py が共有の帯を探して読むことの確認。bit_strip.py)。make_material.py の --scene で
# 「ブラウザのウィンドウの共有」(pattern.html) と「Chrome のタブの共有」(sync-video.mp4。縦横の倍率が違う) を描き、
# 1280x720 に縮めてから枠の 0.5 倍 (640x360) にして、960x540 の象限の左上に置く。frames.py には
# 実行スクリプトが渡すのと同じ領域 (--x 0 --y 0 --w 640 --h 360 --cell 8) を渡す。
#   - 正常 2 つ (GREEN。clock の帯として最初の番号まで読めること)
#   - ウィンドウの 1 フレーム抜け (RED。欠落 1)
#   - 古い形の帯のウィンドウの共有 (「帯が見つからない」と出ること。古い frames.py は全部のフレームを 0 と読み、
#     重複 1199 の RED になる。失敗だけでは「帯がない」と「凍った」を区別できない)
# sync.py の白の見つけ方と smooth.py の帯の探し方も、共有の形のデータで確認する。
#   - tab (動画が枠の約 53%) に 4 つの判定スクリプトを全部渡す。sync.py は白 3・対 3 の GREEN に加えて、
#     領域の平均が 200 超えの規則で数えた白が 0 であること (white_detector.mean_over_200_events) を確認する。
#   - tabfill (sync-video.html。動画が表示域いっぱい) にも 4 つを全部渡す。
#   - smooth.py を window (GREEN、clock) / window_drop1 (RED、欠落 1) / tab / tabfill (GREEN、clock) / window_legacy
#     (帯が見つからない) に渡す。実行スクリプトと同じ領域の引数 (--regions 0,0,640,360 --cell 8) を使う。
#   pattern.html の形 (window*) には白のマーカーも音もないので、sync.py / analyze.py には渡さない。
#
# 判定スクリプトごとの引数と、出力のキーの期待 (checks) は SCENARIOS の 5 番目の dict に書く。キーは "a.b" で入れ子を、
# "cells.0.layout" のように数字で並びの番号を指す。
#
# 使い方: python selfcheck.py [--outdir <テストデータを置くフォルダー>] [--keep]
#   既定の --outdir は build_x64/selfcheck (.gitignore の対象)。既定ではテストデータの mp4/wav は
#   判定が終わるたびに消す (--keep を付けると残す。調査用)。
import argparse
import json
import os
import pathlib
import subprocess
import sys

# 子プロセス (frames.py 等) の標準出力を常に UTF-8 として書かせ、こちらも UTF-8 で読む。
# Windows の既定コードページ (cp932 等) に頼ると、判定スクリプトの JSON 内の日本語コメント文字列
# (sync.py の note など) で環境依存の文字化け・デコード例外が起きるため (実機で確認済み)。
CHILD_ENV = dict(os.environ, PYTHONIOENCODING="utf-8")

HERE = pathlib.Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]  # test/tools/obs-run -> test/tools -> test -> リポジトリ直下
DEFAULT_OUTDIR = REPO_ROOT / "build_x64" / "selfcheck"

SYNC_THRESHOLD_MS = 20.0  # 正常時の実測 0.0 ms と、100 ms ずらした実測 100.0 ms の間

# 1021 = 17 秒 x 60fps + 1。--drop-every / --dup-every は「N 番目ごとに 1 回」なので、
# 全 1200 フレーム中ちょうど 1 回だけ、1020 フレーム目 (17.0 秒) で起きる。5/10/15 秒のマーカーより後、
# 20 秒の終端より前に置いて、マーカー判定にも動画終端にも触れないようにした (freeze-at と揃えた)。
DROP_DUP_AT_17S = "1021"

# 実行スクリプトが共有の形 (枠 640x360 を象限の左上、0.5 倍) で判定スクリプトに渡す引数 (judgeCellArgs と同じ形)。
REGION_ARGS = dict(
    frames=["--x", "0", "--y", "0", "--w", "640", "--h", "360", "--cell", "8"],
    sync=["--x", "0", "--y", "0", "--w", "640", "--h", "360"],
    smooth=["1", "--regions", "0,0,640,360", "--cell", "8"],
)
# 領域の引数を渡さないときの判定スクリプトごとの引数 (テストデータそのものが枠)。
DEFAULT_ARGS = dict(frames=["--cell", "8"], sync=[], smooth=["1"], analyze=[])
# 共有の形のテストデータの共通の引数。crf 30 は共有と録画の 2 度の圧縮の代わりで、既定の 23 より粗くして
# 帯の縁を崩す。crf 42 でも 1 フレームも読み落とさなかった。
SCENE_COMMON = ["--pad-to", "960x540", "--crf", "30"]
# pattern.html の形 (ウィンドウの共有) には白のマーカーも音もないので、frames.py と smooth.py だけを当てる。
WINDOW_JUDGES = dict(sync="-", analyze="-")
SMOOTH_CLEAN = {"cells.0.layout": "clock", "cells.0.total_missing_dup_back": [0, 0, 0], "cells.0.unreadable_frames": 0}

# (名前, make_material.py への追加引数, 表示名, 期待する色 (E=GREEN想定 / R=RED想定 / N=帯が見つからない想定 /
#  -=当てない), 追加 (args: 判定スクリプトごとの引数、checks: 判定スクリプトごとの出力のキーと期待する値))
SCENARIOS = [
    ("normal", [], "正常",
     dict(frames="E", sync="E", smooth="E", analyze="E")),
    ("drop1", ["--drop-every", DROP_DUP_AT_17S], "1 フレーム抜け",
     dict(frames="R", sync="E", smooth="R", analyze="E")),
    ("freeze100", ["--freeze-ms", "100", "--freeze-at", "17"], "100 ms の凍り",
     dict(frames="R", sync="E", smooth="R", analyze="E")),
    ("freeze300", ["--freeze-ms", "300", "--freeze-at", "17"], "300 ms の凍り",
     dict(frames="R", sync="E", smooth="R", analyze="E")),
    ("freeze500", ["--freeze-ms", "500", "--freeze-at", "17"], "500 ms の凍り",
     dict(frames="R", sync="E", smooth="R", analyze="E")),
    ("audioshift100", ["--audio-shift-ms", "100"], "音 100 ms ずれ",
     dict(frames="E", sync="R", smooth="E", analyze="E")),
    ("dup1", ["--dup-every", DROP_DUP_AT_17S], "重複",
     dict(frames="R", sync="E", smooth="R", analyze="E")),
    ("competing1k", ["--competing-1k", "0.5"], "1 kHz が鳴り続ける",
     dict(frames="E", sync="E", smooth="E", analyze="E")),
    ("vfr", ["--vfr"], "可変の fps",
     dict(frames="R", sync="E", smooth="R", analyze="E")),
    ("window", ["--scene", "window", "--counter-start", "1085000"] + SCENE_COMMON, "ウィンドウの共有",
     dict(frames="E", smooth="E", **WINDOW_JUDGES),
     dict(args=REGION_ARGS, checks=dict(
         frames=dict(layout="clock", first=1085000, unreadable_frames=0),
         smooth=SMOOTH_CLEAN))),
    ("window_drop1", ["--scene", "window", "--counter-start", "1085000", "--drop-every", DROP_DUP_AT_17S] + SCENE_COMMON,
     "ウィンドウの共有 1 フレーム抜け",
     dict(frames="R", smooth="R", **WINDOW_JUDGES),
     dict(args=REGION_ARGS, checks=dict(
         frames=dict(layout="clock", gap_count=1, missing_total=1, duplicate_count=0),
         smooth={"cells.0.layout": "clock", "cells.0.total_missing_dup_back": [1, 0, 0]}))),
    # 動画が枠の約 53% の形。sync.py の前の規則 (領域の平均が 200 超え) では白が 0 = 失敗、現在の規則で成功。
    ("tab", ["--scene", "tab", "--counter-start", "31700"] + SCENE_COMMON, "タブの共有",
     dict(frames="E", sync="E", smooth="E", analyze="E"),
     dict(args=REGION_ARGS, checks=dict(
         frames=dict(layout="clock", first=31700, unreadable_frames=0),
         sync={"white_events": 3, "paired": 3, "white_detector.mean_over_200_events": 0},
         smooth=SMOOTH_CLEAN))),
    # sync-video.html (再生すると動画が表示域いっぱい)。帯は枠の左上の角に写る。
    # sync.py の trend (ずれの時間に対する傾き): ずれのないテストデータなので対 3 つで傾き 0。傾きの標準誤差 (傾きの
    # 合否で「0 と分けられるか」を読むのに使う) も、残差のないテストデータなので 0。
    ("tabfill", ["--scene", "tabfill", "--counter-start", "31700"] + SCENE_COMMON, "タブの共有 (動画が全面)",
     dict(frames="E", sync="E", smooth="E", analyze="E"),
     dict(args=REGION_ARGS, checks=dict(
         frames=dict(layout="clock", first=31700, unreadable_frames=0),
         sync={"white_events": 3, "paired": 3, "trend.pairs": 3, "trend.slope_ms_per_min": 0.0, "trend.first_mean_ms": 0.0,
               "trend.slope_se_ms_per_min": 0.0},
         smooth=SMOOTH_CLEAN))),
    ("window_legacy", ["--scene", "window", "--strip", "legacy", "--counter-start", "1085000"] + SCENE_COMMON,
     "古い形の帯のウィンドウの共有",
     dict(frames="N", smooth="N", **WINDOW_JUDGES),
     dict(args=REGION_ARGS, checks=dict(
         frames=dict(barcode_found=False, layout=None),
         smooth={"cells.0.barcode_found": False, "cells.0.layout": None}))),
]


def lookup(data, dotted):
    """"a.b" で入れ子のキーを、"cells.0.layout" のように数字で並びの番号を引く (無ければ None)。"""
    value = data
    for part in dotted.split("."):
        if isinstance(value, list) and part.isdigit():
            value = value[int(part)] if int(part) < len(value) else None
        elif isinstance(value, dict):
            value = value.get(part)
        else:
            return None
    return value


def run_json(cmd):
    r = subprocess.run(cmd, capture_output=True, encoding="utf-8", errors="replace", env=CHILD_ENV)
    if r.returncode != 0:
        raise RuntimeError(f"failed: {cmd}\n{r.stderr}")
    return json.loads(r.stdout)


# 各判定スクリプトの生の出力を GREEN/RED に読み替える。しきい値の根拠はそれぞれ:
#   frames: 欠落・重複・逆行は 1 件でも RED (2 値)。
#   sync  : 正常の実測 0.0 ms、100 ms ずらした実測 100.0 ms。
#           間を取って 20 ms を境目にする (60fps の 1 フレーム = 16.7 ms より少し大きい)。
#   smooth: frames と同じ 2 値化。実測すると 100/300/500 ms の凍りでも重複 (dup-every) でも
#           もっと短い凍りでもすでに 0 でなくなり、smooth.py 自体に ms のしきい値はない
#           (README-selfcheck.md の「smooth.py の判定の境目」)。
#   analyze: 無音か目的の周波数がなければ RED (analyze.py 自身の audio_is_silent /
#           audio_has_target_tone をそのまま使う)。
def verdict_frames(d):
    # 帯が見つからない (barcode_found: false) は RED とも GREEN とも別の NOTFOUND にする (共有の形を「凍り」と取り違えない)。
    if not d.get("barcode_found", True):
        return "NOTFOUND", "barcode_found=False legacy_probe=" + json.dumps(d.get("legacy_probe"), ensure_ascii=False)
    bad = d["gap_count"] > 0 or d["duplicate_count"] > 0 or d["backwards_count"] > 0
    detail = (f"layout={d.get('layout')} first={d.get('first')} gap={d['gap_count']} dup={d['duplicate_count']} "
              f"back={d['backwards_count']} freeze_ms={d['max_freeze_ms']} unreadable={d.get('unreadable_frames')}")
    return ("RED" if bad else "GREEN"), detail


def verdict_sync(d):
    m = d["audio_minus_video_ms"]["mean"]
    bad = d["paired"] == 0 or m is None or abs(m) > SYNC_THRESHOLD_MS
    detail = f"paired={d['paired']} mean_ms={m}"
    return ("RED" if bad else "GREEN"), detail


def verdict_smooth(d):
    cell = d["cells"][0]
    # 帯が見つからない領域は数を出さない (frames.py と同じく NOTFOUND。共有の形を「欠落も重複もない」と取り違えない)。
    if not cell.get("barcode_found", True):
        return "NOTFOUND", "barcode_found=False legacy_probe=" + json.dumps(cell.get("legacy_probe"), ensure_ascii=False)
    total = cell["total_missing_dup_back"]
    bad = any(x > 0 for x in total)
    detail = f"layout={cell.get('layout')} missing_dup_back={total}"
    return ("RED" if bad else "GREEN"), detail


def verdict_analyze(d):
    silent = d.get("audio_is_silent", True)
    has_tone = d.get("audio_has_target_tone", False)
    bad = silent or not has_tone
    detail = f"silent={silent} has_tone={has_tone}"
    return ("RED" if bad else "GREEN"), detail


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--outdir", type=pathlib.Path, default=DEFAULT_OUTDIR)
    ap.add_argument("--keep", action="store_true", help="テストデータの mp4/wav を消さずに残す")
    args = ap.parse_args()
    args.outdir.mkdir(parents=True, exist_ok=True)

    rows = []
    all_ok = True
    want_of = {"E": "GREEN", "R": "RED", "N": "NOTFOUND", "-": "-"}
    for i, scenario in enumerate(SCENARIOS, 1):
        name, extra, label, expect = scenario[:4]
        more = scenario[4] if len(scenario) > 4 else {}
        material = args.outdir / f"{name}.mp4"
        print(f"[{i}/{len(SCENARIOS)}] {label} ({name}) を生成・判定中...", file=sys.stderr)

        subprocess.run(
            [sys.executable, str(HERE / "make_material.py"), str(material)] + extra,
            check=True, capture_output=True, encoding="utf-8", errors="replace", env=CHILD_ENV)

        judge_args = dict(DEFAULT_ARGS, **more.get("args", {}))
        verdicts = dict(frames=verdict_frames, sync=verdict_sync, smooth=verdict_smooth, analyze=verdict_analyze)
        judges = {det: ([sys.executable, str(HERE / f"{det}.py"), str(material)] + judge_args[det], verdicts[det])
                  for det in ("frames", "sync", "smooth", "analyze")}
        got = {}
        details = {}
        row_ok = True
        for det, (cmd, verdict) in judges.items():
            if expect[det] == "-":
                got[det], details[det] = "-", "当てない"
                continue
            data = run_json(cmd)
            got[det], details[det] = verdict(data)
            if got[det] != want_of[expect[det]]:
                row_ok = False
            # 色に加えて、出力のキーが期待どおりか (帯の種類・最初の番号・読めないフレームの数・前の規則の白の数など)。
            for key, value in more.get("checks", {}).get(det, {}).items():
                got_value = lookup(data, key)
                if got_value != value:
                    row_ok = False
                    details[det] += f" [check {key}: want {value!r} got {got_value!r}]"
        all_ok = all_ok and row_ok
        rows.append((name, label, expect, got, details, row_ok))

        if not args.keep:
            material.unlink(missing_ok=True)

    # 表を出す
    print()
    header = f"{'テストデータ':14s} {'frames':17s} {'sync':22s} {'smooth':22s} {'analyze':22s} {'判定'}"
    print(header)
    print("-" * len(header))
    for name, label, expect, got, details, row_ok in rows:
        def cell(det):
            if expect[det] == "-":
                return "-"
            mark = "PASS" if got[det] == want_of[expect[det]] else "FAIL"
            return f"{got[det]:5s}[{mark}]"
        print(f"{label:14s} {cell('frames'):17s} {cell('sync'):22s} {cell('smooth'):22s} {cell('analyze'):22s} "
              f"{'PASS' if row_ok else 'FAIL'}")

    print()
    print("詳細:")
    for name, label, expect, got, details, row_ok in rows:
        print(f"  {label} ({name}):")
        for det in ("frames", "sync", "smooth", "analyze"):
            print(f"    {det:8s} {got[det]:5s} {details[det]}")

    print()
    if all_ok:
        print(f"全 {len(SCENARIOS)} 通り PASS")
        sys.exit(0)
    else:
        print("FAIL したテストデータがある (上の表を参照)")
        sys.exit(1)


if __name__ == "__main__":
    main()
