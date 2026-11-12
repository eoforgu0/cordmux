# 録画の判定スクリプト (1/3): 録画ログ (ledger.jsonl) と、実際にできたファイルを ffprobe で突き合わせる。
# 長さ・フレーム数・サイズ・fps・音の有無を比べる。
# 録画ログの形は src-obs/record/README.md と src-obs/record/record_types.h に従う。
#
# 判定はすべて整数で行う。ffprobe の duration_ts と録画ログの duration_ticks、
# -count_frames の nb_read_frames と録画ログの sample_count を、そのまま等号で比べる。
# 秒の小数にすると丸めで食い違うためである。
#
# 使い方:
#   python ledger_check.py <セッションのフォルダー> [--ffmpeg <ffmpeg の場所>]
#                          [--ffprobe <ffprobe の場所>] [--max-segments <数>]
#   python ledger_check.py --selftest [--ffmpeg <ffmpeg の場所>] [--outdir <テストデータの保存先>] [--keep]
#
# <セッションのフォルダー> は 1 回の録画のフォルダーそのもの。並びは 2 通りを読む。
#   現在の並び:   <保存先>/<YYYY-MM-DD HH-MM-SS>/ の直下に完成の mov、その下の data/ に
#             ledger.jsonl、segments/、black/ (録画ログの file の欄は data/ からの相対、output_file は
#             セッションのフォルダーからの相対)。
#   古い並び:   <保存先>/<YYYYMMDD-HHMMSS>/ の直下に ledger.jsonl、segments/、black/、完成。
# どちらかは data/ があるかで決める (製品の Recorder が録画ログを探す方法と同じ)。
#
# 結果は 1 つの JSON を標準出力に書く。ok が true なら終了コード 0、false なら 1。
# 失敗しても例外を投げずに ok:false を返す (frames.py / dock-click.ps1 と同じ
# 「失敗を握って結果に書く」形)。
#
# ---- 判断 ----------------------------------------------------------
# (1) 録画ログの行の種類は `type` のキー ("session" / "person" / "segment" / "black" /
#     "finalize") で決める。src-obs/record/README.md が、このキーを必ず書くと定めている。
#     キーの有無から推測する枝 (session=schema / person=kinds / segment=close_reason /
#     black=origin / finalize=output_file) も、古い録画ログのために残す。
#     `type` を書く前に作った録画ログ (手で作ったテストデータを含む) を読めなくしても
#     得るものがないためで、現在の製品が書く録画ログはこの枝を通らない。
# (2) ffprobe の場所。--ffprobe がなければ --ffmpeg と同じフォルダーの同じ綴りの
#     ffprobe を使い、--ffmpeg もなければ PATH の "ffprobe" を使う。CI (ubuntu) は
#     apt の ffmpeg なので PATH で足り、実機では同梱の obs-deps の bin を渡す。
# (3) 音のトラックの長さ。音の区間は time_base が 1/sample_rate になるので
#     duration_ts がそのままサンプル数になる。録画ログの sample_count と duration_ticks の
#     両方をこの 1 つの値と比べる (録画ログの segment の行は、音では
#     「sample_count = サンプル数」「timescale = 毎秒のサンプル数」と定めている)。
# (4) --selftest のテストデータは、ffmpeg だけで作れる範囲 (testsrc と sine) にした。
#     壊し方は 5 通り (フレーム数を 1 ずらす / 長さを 1 刻みずらす / サイズを変える /
#     区間のファイルを消す / 完成の長さを短くする)。正常が GREEN で、5 通りすべてが
#     RED になることを確認する。
# (5) 到着の遅れの差は測るだけで、判定に入れない。
#     人と種別ごとに「受け手の steady_clock の時刻 - 送信側のタイムスタンプ」(first_pts_steady_us - first_pts_us)
#     を映像と音で出し、映像から音を引いて結果の JSON の arrival_lag に書く。送信側のタイムスタンプは
#     受け手の steady_clock と同じ単調な時計で進むので、2 つの時計の差 (定数) は映像と音で同じで、引くと
#     消える。残る差は「同じ人の映像が音よりどれだけ遅れて届いたか」そのものになる。映像と音のそれぞれの値は、
#     時計の差を含むので出さない。比べるのは媒体ごとに録画ログで最初の区間
#     (録画を ON にした直後の最初の IDR と最初の音のブロック)。
#     判定に入れない理由: 録画ファイルのタイムスタンプは送信側のタイムスタンプで置くので、
#     この差は完成した録画の A/V に影響しない。届き方の記録であって、完成の良し悪しではない
#     (RED にすると、録画が正しくても通信の具合で判定が落ちる)。
#     全区間の組で差を出す方法は採らない (音と映像の区間は 1 対 1 に対応しない)。
#     録画ログの外 (OBS のログ) から拾う方法も採らない (録画ログの 2 つの時刻だけで足りる)。
#     steady_clock の値がない行 (0 か欠け) は数えない。
# (6) 同じものを指す行は後の行が優先する (製品の parse_ledger と同じ読み方)。キーは person が
#     user_id、segment と black が file、finalize が output_file。並びは最初に出た位置を保つ。
#     録画の側は区間を開いた瞬間にも segment の行 (truncated 真、close_reason not_closed、
#     sample_count 0) を書き、閉じたときに同じ file の行を書く (src-obs/record/recorder.h)。
#     行を全部並べると、普通に止めた録画でも開いた瞬間の行を区間として ffprobe と突き合わせて RED になる。
#     session の行は全部を残す (schema を行ごとに確認するため。製品は最後の行を使う)。
#     --selftest の normal_reopened が、開いた瞬間の行と閉じた行が並ぶ録画ログで GREEN になることを確認する。
#     not_closed の行を読み飛ばす方法は採らない (落ちた録画の、閉じていない区間の行が消え、製品の読み方と割れる)。
import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys

KNOWN_SCHEMAS = (1,)  # src-obs/record/record_types.h の kLedgerSchema

# 録画ログの行の種類を決めるキー 上から順に見る。
ROW_MARKERS = (
    ("session", "schema"),
    ("person", "kinds"),
    ("segment", "close_reason"),
    ("black", "origin"),
    ("finalize", "output_file"),
)


def resolve_tools(ffmpeg, ffprobe):
    """--ffmpeg / --ffprobe から実際に呼ぶ 2 つの実行ファイルを決める。"""
    ff = ffmpeg or "ffmpeg"
    if ffprobe:
        return ff, ffprobe
    if ffmpeg:
        d = os.path.dirname(os.path.abspath(ffmpeg))
        base = os.path.basename(ffmpeg)
        probe = os.path.join(d, base.replace("ffmpeg", "ffprobe", 1))
        if os.path.exists(probe):
            return ff, probe
    return ff, "ffprobe"


def row_kind(row):
    """1 行の dict から行の種類を返す (分からなければ None)。"""
    named = row.get("type")
    if isinstance(named, str):
        for kind, _marker in ROW_MARKERS:
            if named == kind:
                return kind
    for kind, marker in ROW_MARKERS:
        if marker in row:
            return kind
    return None


# 行の種類ごとのキー (製品の record_ledger.h の parse_ledger と同じ)。
ROW_KEYS = {"person": "user_id", "segment": "file", "black": "file", "finalize": "output_file"}


def parse_ledger(text):
    """JSONL を種類ごとに分ける。ちぎれた末尾の 1 行は読み飛ばして数える
    (書いている最中に落ちた録画ログでも、読めた範囲までを判定する)。"""
    out = {"session": [], "person": [], "segment": [], "black": [], "finalize": []}
    unknown = 0
    skipped = 0
    lines = text.split("\n")
    for i, line in enumerate(lines):
        s = line.strip()
        if not s:
            continue
        try:
            row = json.loads(s)
        except ValueError:
            # 末尾の 1 行だけは「書いている最中に落ちた」として読み飛ばす。
            if i >= len(lines) - 2:
                skipped += 1
                continue
            skipped += 1
            continue
        if not isinstance(row, dict):
            unknown += 1
            continue
        kind = row_kind(row)
        if kind is None:
            unknown += 1
            continue
        key_name = ROW_KEYS.get(kind)
        if key_name is not None:
            # 同じキーの行は後が優先され、並びは最初に出た位置を保つ。
            for j, existing in enumerate(out[kind]):
                if existing.get(key_name) == row.get(key_name):
                    out[kind][j] = row
                    break
            else:
                out[kind].append(row)
            continue
        out[kind].append(row)
    return out, skipped, unknown


def probe_streams(ffprobe, path, count_frames):
    """ffprobe の stream の一覧を返す。失敗したら (None, 理由)。"""
    cmd = [ffprobe, "-v", "error", "-show_entries",
           "stream=index,codec_type,codec_name,width,height,r_frame_rate,avg_frame_rate,time_base,"
           "duration_ts,nb_read_frames,sample_rate,channels",
           "-of", "json"]
    if count_frames:
        cmd.append("-count_frames")
    cmd.append(path)
    try:
        r = subprocess.run(cmd, capture_output=True, encoding="utf-8", errors="replace")
    except OSError as e:
        return None, "ffprobe を起動できなかった: " + str(e)
    if r.returncode != 0:
        return None, "ffprobe が失敗した (exit=" + str(r.returncode) + "): " + (r.stderr or "").strip()[:400]
    try:
        return json.loads(r.stdout).get("streams", []), None
    except ValueError:
        return None, "ffprobe の出力が JSON でなかった"


def pick_stream(streams, codec_type):
    for s in streams:
        if s.get("codec_type") == codec_type:
            return s
    return None


def int_or_none(value):
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def ratio(text):
    """"60/1" を (60, 1) にする。読めなければ (None, None)。"""
    if not isinstance(text, str) or "/" not in text:
        return None, None
    a, b = text.split("/", 1)
    return int_or_none(a), int_or_none(b)


class Report:
    """検査の結果をためる。1 つでも ok:false があれば全体が失敗。"""

    def __init__(self):
        self.checks = []

    def add(self, name, ok, detail):
        self.checks.append({"name": name, "ok": bool(ok), "detail": detail})

    def ok(self):
        return all(c["ok"] for c in self.checks)


def equal_check(rep, name, got, want, note=""):
    rep.add(name, got == want, "got=" + str(got) + " want=" + str(want) + (" " + note if note else ""))


def fps_matches(r_frame_rate, avg_frame_rate, want_num, want_den):
    """録画ログの fps (エンコーダの名目の値) と ffprobe の値を比べる。返り値は (通るか、r_frame_rate がそのまま一致したか、detail)。

    ffprobe の r_frame_rate は最小の間隔から推定した値で、区間の中に 1 組だけ 0.4 ms 離れたフレームがあると 120/1 になる
    (avg_frame_rate は 60.06)。r_frame_rate が合わないときは avg_frame_rate を四捨五入した値で比べる。実際の画面共有
    (とくに Firefox のウィンドウ) は、画面が動かない間に送り手がフレームを減らすので、平均は名目より下がる (実際の通話の
    サブアカウント 3 の区間は名目 30 に対し 13.86、名目 29 に対し 27.8)。フレームは実際の時刻で書くので、名目より少ないのは区間の
    誤りではない。名目より多い (時刻の尺度の誤りでフレームが詰まった) ときだけ通さない。フレームの数は check_video_row の
    frames で 1 枚まで比べる。
    """
    num, den = ratio(r_frame_rate)
    if num is not None and num == want_num and den == want_den:
        return True, True, ""
    avg_num, avg_den = ratio(avg_frame_rate)
    avg_rounded = int(round(avg_num / avg_den)) if avg_num is not None and avg_den else None
    ok = avg_rounded is not None and want_num is not None and want_den == 1 and avg_rounded <= want_num
    detail = ("r_frame_rate=" + str(r_frame_rate) + " avg_frame_rate=" + str(avg_frame_rate)
              + " (四捨五入 " + str(avg_rounded) + ") want<=" + str(want_num) + "/" + str(want_den))
    return ok, False, detail


def check_video_row(rep, ffprobe, session_dir, row, prefix):
    rel = row.get("file") or ""
    path = os.path.join(session_dir, rel.replace("/", os.sep))
    if not os.path.exists(path):
        rep.add(prefix + ".file", False, "ファイルがない: " + rel)
        return
    streams, err = probe_streams(ffprobe, path, count_frames=True)
    if streams is None:
        rep.add(prefix + ".probe", False, err)
        return
    v = pick_stream(streams, "video")
    if v is None:
        rep.add(prefix + ".video_stream", False, "映像のトラックがない: " + rel)
        return
    rep.add(prefix + ".video_stream", True, rel)
    equal_check(rep, prefix + ".width", int_or_none(v.get("width")), int_or_none(row.get("width")))
    equal_check(rep, prefix + ".height", int_or_none(v.get("height")), int_or_none(row.get("height")))
    want_num = int_or_none(row.get("fps_num"))
    want_den = int_or_none(row.get("fps_den"))
    ok, exact, detail = fps_matches(v.get("r_frame_rate"), v.get("avg_frame_rate"), want_num, want_den)
    if exact:
        equal_check(rep, prefix + ".fps_num", want_num, want_num)
        equal_check(rep, prefix + ".fps_den", want_den, want_den)
    else:
        rep.add(prefix + ".fps_num", ok, detail)
    tb_num, tb_den = ratio(v.get("time_base"))
    if "timescale" in row:
        equal_check(rep, prefix + ".timescale", tb_den, int_or_none(row.get("timescale")),
                    "time_base の分母 (分子 " + str(tb_num) + " は 1 のはず)")
    want_frames = int_or_none(row.get("sample_count"))
    if want_frames is None:
        want_frames = int_or_none(row.get("frame_count"))
    equal_check(rep, prefix + ".frames", int_or_none(v.get("nb_read_frames")), want_frames)
    equal_check(rep, prefix + ".duration_ticks", int_or_none(v.get("duration_ts")),
                int_or_none(row.get("duration_ticks")))


def check_audio_row(rep, ffprobe, session_dir, row, prefix):
    rel = row.get("file") or ""
    path = os.path.join(session_dir, rel.replace("/", os.sep))
    if not os.path.exists(path):
        rep.add(prefix + ".file", False, "ファイルがない: " + rel)
        return
    streams, err = probe_streams(ffprobe, path, count_frames=False)
    if streams is None:
        rep.add(prefix + ".probe", False, err)
        return
    a = pick_stream(streams, "audio")
    if a is None:
        rep.add(prefix + ".audio_stream", False, "音のトラックがない: " + rel)
        return
    rep.add(prefix + ".audio_stream", True, rel)
    tb_num, tb_den = ratio(a.get("time_base"))
    if "timescale" in row:
        equal_check(rep, prefix + ".timescale", tb_den, int_or_none(row.get("timescale")))
    got = int_or_none(a.get("duration_ts"))
    # 音は time_base が 1/sample_rate なので duration_ts = サンプル数。
    equal_check(rep, prefix + ".duration_ticks", got, int_or_none(row.get("duration_ticks")))
    if "sample_count" in row:
        equal_check(rep, prefix + ".samples", got, int_or_none(row.get("sample_count")))


def check_finalize_row(rep, ffprobe, session_dir, row, prefix):
    rel = row.get("output_file") or ""
    path = os.path.join(session_dir, rel.replace("/", os.sep))
    if not os.path.exists(path):
        rep.add(prefix + ".file", False, "完成のファイルがない: " + rel)
        return
    equal_check(rep, prefix + ".exit_code", int_or_none(row.get("exit_code")), 0)
    streams, err = probe_streams(ffprobe, path, count_frames=False)
    if streams is None:
        rep.add(prefix + ".probe", False, err)
        return
    v = pick_stream(streams, "video")
    a = pick_stream(streams, "audio")
    # 音の有無。画面の完成は映像と音の 2 ストリーム、声の完成は音だけ。
    kind = str(row.get("kind", "")).lower()
    if kind.startswith("voice"):
        rep.add(prefix + ".audio_stream", a is not None, "声の完成に音のトラックがあるか")
        rep.add(prefix + ".no_video_stream", v is None, "声の完成に映像のトラックがないか")
        length_stream = a
    else:
        rep.add(prefix + ".video_stream", v is not None, "画面の完成に映像のトラックがあるか")
        rep.add(prefix + ".audio_stream", a is not None, "画面の完成に音のトラックがあるか")
        length_stream = v
    if length_stream is None:
        return
    tb_num, tb_den = ratio(length_stream.get("time_base"))
    want_ts = int_or_none(row.get("output_timescale"))
    if want_ts is not None:
        equal_check(rep, prefix + ".output_timescale", tb_den, want_ts)
    equal_check(rep, prefix + ".output_duration_ticks", int_or_none(length_stream.get("duration_ts")),
                int_or_none(row.get("output_duration_ticks")))


def is_audio_row(row):
    media = str(row.get("media", "")).lower()
    if media:
        return media.startswith("audio")
    # media がない録画ログでも、avcc_hex が空なら音として読む (音なら空)。
    return not row.get("avcc_hex")


def arrival_lag_by_person(segments):
    """人と種別ごとに、映像と音の「到着の遅れ」(受け手の steady_clock の時刻 - 送り手のタイムスタンプ) の差を
    出す。比べるのは媒体ごとに録画ログで最初の区間。映像と音の両方が
    揃った組だけを返す。値はミリ秒 (小数 1 桁)。判定には使わない。"""
    groups = {}
    order = []
    for g in segments:
        steady = int_or_none(g.get("first_pts_steady_us"))
        pts = int_or_none(g.get("first_pts_us"))
        if not steady or pts is None:
            continue
        key = (str(g.get("user_id", "")), str(g.get("kind", "")))
        if key not in groups:
            groups[key] = {"video": [], "audio": []}
            order.append(key)
        groups[key]["audio" if is_audio_row(g) else "video"].append(steady - pts)
    out = []
    for key in order:
        lags = groups[key]
        if not lags["video"] or not lags["audio"]:
            continue
        video = lags["video"][0]
        audio = lags["audio"][0]
        out.append({
            "user_id": key[0],
            "kind": key[1],
            "video_minus_audio_ms": round((video - audio) / 1000.0, 1),
            "segments": {"video": len(lags["video"]), "audio": len(lags["audio"])},
        })
    return out


# テストデータのフォルダーの名前 (src-obs/record/record_types.h の kMaterialsDirName と同じ綴り)。
MATERIALS_DIR_NAME = "data"


def materials_dir_of(session_dir):
    """セッションのフォルダーから録画ログと区間の保存先を決める。現在の並びなら
    <セッションのフォルダー>/data、古い並び (録画ログが直下) ならセッションのフォルダーそのもの。"""
    candidate = os.path.join(session_dir, MATERIALS_DIR_NAME)
    if os.path.isdir(candidate):
        return candidate
    return session_dir


def check_session(session_dir, ffprobe, max_segments):
    rep = Report()
    materials_dir = materials_dir_of(session_dir)
    ledger_path = os.path.join(materials_dir, "ledger.jsonl")
    if not os.path.exists(ledger_path):
        rep.add("ledger.exists", False, "ledger.jsonl がない: " + ledger_path)
        return rep, {}
    text = pathlib.Path(ledger_path).read_text(encoding="utf-8", errors="replace")
    rows, skipped, unknown = parse_ledger(text)
    rep.add("ledger.exists", True, ledger_path)
    # ちぎれた末尾の 1 行は許す。2 行以上なら録画ログそのものが壊れている。
    rep.add("ledger.skipped_lines", skipped <= 1, "skipped=" + str(skipped))
    rep.add("ledger.unknown_rows", unknown == 0, "unknown=" + str(unknown))
    rep.add("ledger.session_row", len(rows["session"]) >= 1, "session=" + str(len(rows["session"])))
    for i, s in enumerate(rows["session"]):
        equal_check(rep, "session[" + str(i) + "].schema_known",
                    int_or_none(s.get("schema")) in KNOWN_SCHEMAS, True,
                    "schema=" + str(s.get("schema")))
    # 区間の人が person の行に載っているか (表示名のコピー)。
    people = set(p.get("user_id") for p in rows["person"])
    seg_users = set(g.get("user_id") for g in rows["segment"])
    rep.add("person.covers_segments", seg_users <= people,
            "segment の人 " + str(sorted(seg_users)) + " / person の人 " + str(sorted(people)))

    segments = rows["segment"]
    if max_segments and len(segments) > max_segments:
        segments = segments[:max_segments]
    for i, g in enumerate(segments):
        prefix = "segment[" + str(i) + "]"
        if is_audio_row(g):
            check_audio_row(rep, ffprobe, materials_dir, g, prefix)
        else:
            check_video_row(rep, ffprobe, materials_dir, g, prefix)
    for i, b in enumerate(rows["black"]):
        check_video_row(rep, ffprobe, materials_dir, b, "black[" + str(i) + "]")
    # 完成はセッションのフォルダーからの相対 (古い並びでは録画ログと同じ段なので同じ読み方になる)。
    for i, f in enumerate(rows["finalize"]):
        check_finalize_row(rep, ffprobe, session_dir, f, "finalize[" + str(i) + "]")

    counts = {k: len(v) for k, v in rows.items()}
    counts["layout"] = "legacy" if materials_dir == session_dir else "materials"
    counts["skipped_lines"] = skipped
    counts["segments_checked"] = len(segments)
    # 測るだけで判定に入れない。--max-segments に関わらず全区間から出す。
    counts["arrival_lag"] = arrival_lag_by_person(rows["segment"])
    return rep, counts


# ---- --selftest --------------------------------------------------------------
# ffmpeg だけで小さなセッションのフォルダーを作り、正常が合格、わざと壊した 5 通りが失敗に
# なることを確認する。

SELFTEST_FPS = 60
SELFTEST_FRAMES = 60
SELFTEST_TIMESCALE = 60000
SELFTEST_SAMPLE_RATE = 48000
SELFTEST_WIDTH = 320
SELFTEST_HEIGHT = 240
MOV_FLAGS = "+frag_keyframe+empty_moov+default_base_moof"


def run_ffmpeg(ffmpeg, args):
    cmd = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y"] + args
    r = subprocess.run(cmd, capture_output=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise RuntimeError("ffmpeg が失敗した: " + " ".join(cmd) + "\n" + (r.stderr or "")[:800])


def build_selftest_session(ffmpeg, session_dir, legacy=False, reopened=False):
    """legacy が偽なら現在の並び (テストデータは data/ の下)、真なら古い並び (全部が直下)。
    reopened が真なら、区間の行の前に同じ file の開いた瞬間の行 (truncated 真・not_closed・sample_count 0) を
    書く (製品の録画ログの形)。"""
    out_dir = session_dir
    if not legacy:
        session_dir = os.path.join(session_dir, MATERIALS_DIR_NAME)
    segments = os.path.join(session_dir, "segments")
    black = os.path.join(session_dir, "black")
    os.makedirs(segments, exist_ok=True)
    os.makedirs(black, exist_ok=True)
    seconds = SELFTEST_FRAMES / SELFTEST_FPS

    video_rel = "segments/u1_screen_video_000001.mov"
    audio_rel = "segments/u1_screen_audio_000001.mov"
    black_rel = "black/u1_enc1.mov"
    out_rel = "taro_screen.mov" if legacy else "taro Screen Share 2000-01-01 00-00-00.mov"

    run_ffmpeg(ffmpeg, [
        "-f", "lavfi", "-i", "testsrc=size=" + str(SELFTEST_WIDTH) + "x" + str(SELFTEST_HEIGHT) +
        ":rate=" + str(SELFTEST_FPS) + ":duration=" + str(seconds),
        "-c:v", "libx264", "-g", str(SELFTEST_FRAMES), "-pix_fmt", "yuv420p",
        "-video_track_timescale", str(SELFTEST_TIMESCALE), "-movflags", MOV_FLAGS,
        os.path.join(session_dir, video_rel.replace("/", os.sep)),
    ])
    run_ffmpeg(ffmpeg, [
        "-f", "lavfi", "-i", "color=c=black:size=" + str(SELFTEST_WIDTH) + "x" + str(SELFTEST_HEIGHT) +
        ":rate=" + str(SELFTEST_FPS) + ":duration=" + str(seconds),
        "-c:v", "libx264", "-g", str(SELFTEST_FRAMES), "-pix_fmt", "yuv420p",
        "-video_track_timescale", str(SELFTEST_TIMESCALE), "-movflags", MOV_FLAGS,
        os.path.join(session_dir, black_rel.replace("/", os.sep)),
    ])
    run_ffmpeg(ffmpeg, [
        "-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=" + str(SELFTEST_SAMPLE_RATE) +
        ":duration=" + str(seconds),
        "-ac", "2", "-c:a", "pcm_f32le", "-movflags", MOV_FLAGS, "-frag_duration", "1000000",
        os.path.join(session_dir, audio_rel.replace("/", os.sep)),
    ])
    # 完成 (映像 + 音の 1 本)。区間をそのまま多重化するだけ (再エンコードしない枝の形)。
    run_ffmpeg(ffmpeg, [
        "-i", os.path.join(session_dir, video_rel.replace("/", os.sep)),
        "-i", os.path.join(session_dir, audio_rel.replace("/", os.sep)),
        "-map", "0:v:0", "-map", "1:a:0", "-c", "copy",
        "-video_track_timescale", str(SELFTEST_TIMESCALE),
        os.path.join(out_dir, out_rel.replace("/", os.sep)),
    ])

    video_ticks = SELFTEST_FRAMES * SELFTEST_TIMESCALE // SELFTEST_FPS
    audio_samples = SELFTEST_SAMPLE_RATE * SELFTEST_FRAMES // SELFTEST_FPS
    rows = [
        {"type": "session", "schema": 1, "session_id": "20260101-000000",
         "started_at_wall_us": 0, "started_at_steady_us": 0, "ended_at_steady_us": 1000000,
         "stopped_reason": "toggled", "finalize_state": "done",
         "plugin_version": "selftest", "ffmpeg_version": "selftest", "dummy_intervals": []},
        {"type": "person", "user_id": "u1", "display_name": "taro", "kinds": ["screen_video", "screen_audio"]},
        {"type": "segment", "user_id": "u1", "kind": "screen", "stream_id": 1,
         "encoder_instance_id": "enc1", "file": video_rel, "media": "video",
         "avcc_hex": "01", "width": SELFTEST_WIDTH, "height": SELFTEST_HEIGHT,
         "fps_num": SELFTEST_FPS, "fps_den": 1, "timescale": SELFTEST_TIMESCALE,
         "first_pts_us": 0, "last_pts_us": 983333, "sample_count": SELFTEST_FRAMES,
         "duration_ticks": video_ticks, "bytes": 0, "starts_with_idr": True,
         "close_reason": "recording_toggled"},
        {"type": "segment", "user_id": "u1", "kind": "screen", "stream_id": 1,
         "encoder_instance_id": "enc1", "file": audio_rel, "media": "audio",
         "avcc_hex": "", "timescale": SELFTEST_SAMPLE_RATE,
         "first_pts_us": 0, "last_pts_us": 983333, "sample_count": audio_samples,
         "duration_ticks": audio_samples, "bytes": 0, "starts_with_idr": False,
         "close_reason": "recording_toggled"},
        {"type": "black", "user_id": "u1", "encoder_instance_id": "enc1", "file": black_rel,
         "avcc_hex": "01", "width": SELFTEST_WIDTH, "height": SELFTEST_HEIGHT,
         "fps_num": SELFTEST_FPS, "fps_den": 1, "frame_count": SELFTEST_FRAMES,
         "duration_ticks": video_ticks, "origin": "encoder_created", "first_frame_delay_us": 0},
        {"type": "finalize", "user_id": "u1", "kind": "screen", "branch": "remux",
         "output_file": out_rel, "args": [], "exit_code": 0,
         "output_duration_ticks": video_ticks, "output_timescale": SELFTEST_TIMESCALE,
         "check": "passed", "gaps": []},
    ]
    written = []
    for r in rows:
        if reopened and r.get("type") == "segment":
            opened = dict(r)
            opened.update({"last_pts_us": r.get("first_pts_us", 0), "sample_count": 0, "duration_ticks": 0,
                           "bytes": 0, "truncated": True, "close_reason": "not_closed"})
            written.append(opened)
        written.append(r)
    write_ledger(out_dir, written)
    return rows


def write_ledger(session_dir, rows):
    text = "".join(json.dumps(r, ensure_ascii=False) + "\n" for r in rows)
    with open(os.path.join(materials_dir_of(session_dir), "ledger.jsonl"), "w", encoding="utf-8",
              newline="\n") as f:
        f.write(text)


def find_row(rows, kind, media=None):
    for r in rows:
        if r.get("type") != kind:
            continue
        if media is not None and r.get("media") != media:
            continue
        return r
    return None


def break_frame_count(session_dir, rows, ffmpeg):
    find_row(rows, "segment", "video")["sample_count"] += 1
    write_ledger(session_dir, rows)


def break_duration_ticks(session_dir, rows, ffmpeg):
    find_row(rows, "segment", "video")["duration_ticks"] += 1
    write_ledger(session_dir, rows)


def break_size(session_dir, rows, ffmpeg):
    find_row(rows, "segment", "video")["width"] += 2
    write_ledger(session_dir, rows)


def break_missing_file(session_dir, rows, ffmpeg):
    os.remove(os.path.join(materials_dir_of(session_dir),
                           find_row(rows, "segment", "video")["file"].replace("/", os.sep)))


def break_short_output(session_dir, rows, ffmpeg):
    out = find_row(rows, "finalize")["output_file"].replace("/", os.sep)
    src = os.path.join(session_dir, out)
    tmp = src + ".tmp.mov"
    # 完成を半分の長さに作り直す (録画ログの output_duration_ticks は元のまま = 食い違う)。
    run_ffmpeg(ffmpeg, ["-i", src, "-t", "0.5", "-c", "copy",
                        "-video_track_timescale", str(SELFTEST_TIMESCALE), tmp])
    os.replace(tmp, src)


BREAKAGES = [
    ("frame_count", "フレーム数を 1 ずらす", break_frame_count),
    ("duration_ticks", "長さを 1 刻みずらす", break_duration_ticks),
    ("size", "サイズを変える", break_size),
    ("missing_file", "区間のファイルを消す", break_missing_file),
    ("short_output", "完成を半分の長さにする", break_short_output),
]


def selftest_arrival_lag():
    """到着の遅れの差の数え方を ffmpeg なしで確認する。映像が音より 140 ms 余計に
    遅れて届いた人は 140.0、steady_clock の値がない行は数えず、片方の媒体しかない人は出さない。
    送信側のタイムスタンプと steady_clock の原点は違う (差は定数 OFFSET) が、引くと消える。"""
    offset = 3_600_000_000 - 1_700_000_000_000_000
    rows = [
        {"type": "segment", "user_id": "u1", "kind": "screen", "media": "audio",
         "first_pts_us": 1_700_000_001_000_000, "first_pts_steady_us": 1_700_000_001_012_000 + offset,
         "close_reason": "recording_toggled"},
        {"type": "segment", "user_id": "u1", "kind": "screen", "media": "video",
         "first_pts_us": 1_700_000_001_050_000, "first_pts_steady_us": 1_700_000_001_202_000 + offset,
         "close_reason": "recording_toggled"},
        # 2 本目の区間は比べない (媒体ごとに最初の区間だけ)。
        {"type": "segment", "user_id": "u1", "kind": "screen", "media": "video",
         "first_pts_us": 1_700_000_009_000_000, "first_pts_steady_us": 1_700_000_009_500_000 + offset,
         "close_reason": "format_changed"},
        # steady_clock の値がない行は数えない。
        {"type": "segment", "user_id": "u2", "kind": "screen", "media": "video",
         "first_pts_us": 1_700_000_001_000_000, "first_pts_steady_us": 0, "close_reason": "recording_toggled"},
        {"type": "segment", "user_id": "u2", "kind": "screen", "media": "audio",
         "first_pts_us": 1_700_000_001_000_000, "first_pts_steady_us": 1_700_000_001_010_000 + offset,
         "close_reason": "recording_toggled"},
        # 声は音だけなので組にならない。
        {"type": "segment", "user_id": "u3", "kind": "voice", "media": "audio",
         "first_pts_us": 1_700_000_000_000_000, "first_pts_steady_us": 1_700_000_000_008_000 + offset,
         "close_reason": "recording_toggled"},
    ]
    got = arrival_lag_by_person(rows)
    ok = (len(got) == 1 and got[0]["user_id"] == "u1" and got[0]["video_minus_audio_ms"] == 140.0
          and got[0]["segments"] == {"video": 2, "audio": 1})
    return {"case": "arrival_lag", "label": "到着の遅れの差を数える", "want": "140.0",
            "got": json.dumps(got, ensure_ascii=False), "ok": ok, "failed": [] if ok else ["arrival_lag"]}


def selftest_fps():
    """fps の比べ方を ffmpeg なしで確認する。名目と同じ、平均が名目より少ない (送り手がフレームを減らした) は通し、
    平均が名目より多い、録画ログの分母が 1 でない、平均が読めないは通さない。"""
    cases = [
        (("30/1", "30/1", 30, 1), True),
        (("120/1", "3003/50", 60, 1), True),      # r_frame_rate が外れても平均の四捨五入 60 で通す
        (("58/1", "13800000/496313", 29, 1), True),  # 平均 27.8 は名目 29 より少ない
        (("60/1", "31/1", 29, 1), False),         # 平均 31 は名目 29 より多い
        (("60/1", "28/1", 29, 2), False),         # 録画ログの分母が 1 でない
        (("0/0", "0/0", 30, 1), False),           # 平均が読めない
    ]
    got = []
    ok = True
    for (r, avg, num, den), want in cases:
        passed = fps_matches(r, avg, num, den)[0]
        got.append(passed)
        ok = ok and passed == want
    return {"case": "fps", "label": "fps の名目と平均を比べる", "want": json.dumps([w for _, w in cases]),
            "got": json.dumps(got), "ok": ok, "failed": [] if ok else ["fps"]}


def selftest(ffmpeg, ffprobe, outdir, keep):
    base = pathlib.Path(outdir)
    results = [selftest_arrival_lag(), selftest_fps()]
    all_ok = all(r["ok"] for r in results)
    # 古い並び (録画ログが直下) のセッションも同じく GREEN になること。
    cases = [("normal", "正常", None), ("normal_legacy", "正常 (古い並び)", None),
             ("normal_reopened", "正常 (区間ごとに開いた瞬間の行と閉じた行が並ぶ)", None)] + BREAKAGES
    for name, label, breaker in cases:
        legacy = name == "normal_legacy"
        reopened = name == "normal_reopened"
        session_dir = str(base / ("ledger-check-" + name) /
                          ("20000101-000000" if legacy else "2000-01-01 00-00-00"))
        if os.path.exists(session_dir):
            shutil.rmtree(session_dir, ignore_errors=True)
        os.makedirs(session_dir, exist_ok=True)
        try:
            rows = build_selftest_session(ffmpeg, session_dir, legacy, reopened)
            if breaker is not None:
                breaker(session_dir, rows, ffmpeg)
            rep, _counts = check_session(session_dir, ffprobe, 0)
            got = "GREEN" if rep.ok() else "RED"
            failed = [c["name"] for c in rep.checks if not c["ok"]]
        except Exception as e:  # noqa: BLE001 (テストデータ作りの失敗も結果に出す)
            got = "ERROR"
            failed = [str(e)[:300]]
        want = "GREEN" if breaker is None else "RED"
        ok = got == want
        all_ok = all_ok and ok
        results.append({"case": name, "label": label, "want": want, "got": got, "ok": ok, "failed": failed[:6]})
        if not keep:
            shutil.rmtree(str(base / ("ledger-check-" + name)), ignore_errors=True)
    return {"ok": all_ok, "selftest": results}


def main():
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument("session_dir", nargs="?", default=None)
    parser.add_argument("--ffmpeg", default=None)
    parser.add_argument("--ffprobe", default=None)
    parser.add_argument("--max-segments", type=int, default=0,
                        help="先頭から何区間まで調べるか (0 = 全部)")
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--outdir", default=None, help="--selftest のテストデータの保存先")
    parser.add_argument("--keep", action="store_true", help="--selftest のテストデータを消さない")
    args = parser.parse_args()

    ffmpeg, ffprobe = resolve_tools(args.ffmpeg, args.ffprobe)
    if args.selftest:
        here = pathlib.Path(__file__).resolve().parent
        default_outdir = here.parents[2] / "build_x64" / "selfcheck"
        outdir = args.outdir or str(default_outdir)
        os.makedirs(outdir, exist_ok=True)
        result = selftest(ffmpeg, ffprobe, outdir, args.keep)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        sys.exit(0 if result["ok"] else 1)

    if not args.session_dir:
        print(json.dumps({"ok": False, "error": "セッションのフォルダーを渡すか --selftest を付ける"},
                         ensure_ascii=False))
        sys.exit(1)
    session_dir = os.path.abspath(args.session_dir)
    rep, counts = check_session(session_dir, ffprobe, args.max_segments)
    arrival_lag = counts.pop("arrival_lag", [])
    result = {"ok": rep.ok(), "session_dir": session_dir, "counts": counts,
              "failed": [c for c in rep.checks if not c["ok"]],
              "checks": len(rep.checks),
              "arrival_lag": arrival_lag}
    print(json.dumps(result, ensure_ascii=False, indent=2))
    sys.exit(0 if result["ok"] else 1)


if __name__ == "__main__":
    main()
