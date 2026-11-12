# SPDX-License-Identifier: Apache-2.0

# cordmux のログから、メッセージの表示と消去 (notices) と、映像、接続、画面共有の終わり、最初のフレームにかかる時間
# (timing) を数えるツール。読むのは OBS のログ、診断情報の zip の plugin.log、plugin.log だけで、OBS も Vesktop も
# 起動しない。結果は 1 つの JSON を標準出力に書く (--markdown なら表)。notices は --normal か --fault を渡したときだけ
# 合否を判定し、失敗なら終了コード 1、判定できない項目があれば 2 (ok は null)、成功なら 0 を返す。timing は数える
# だけで、終了コードは 0 (ログを開けないときは 2)。使い方は README.md にある。
# --selftest は、テストデータのログと、製品のソースにあるログの行の形を確認する。

import argparse
import datetime as dt
import json
import pathlib
import re
import sys
import tempfile
import zipfile

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]

# ---- 行の形 (製品のソースと同じ文字列。--selftest が、ソースに今も同じ文字列があるかを確認する) ----

# OBS のログと plugin.log の行の先頭の時刻。古いバージョンの plugin.log の行にはない。
TIME_PREFIX_RE = re.compile(r"^(?P<time>\d{2}:\d{2}:\d{2}\.\d{3}): (?P<rest>.*)$")

# メッセージの表示と消去の 1 行 (src-obs/dock/dock_poll.cpp の report_notice_changes)。subject は「slot 3」のように
# 空白を含む。
NOTICE_RE = re.compile(
    r"^(?:\[cordmux\] )?Notice (?P<kind>shown|cleared) "
    r"\(rule=(?P<rule>\S+) tier=(?P<tier>1|2|3|row) subject=(?P<subject>[^)]*)\)\s*$"
)
TIERS = ("1", "2", "3", "row")

# 接続と切断の行 (src-obs/ws/listener.cpp の log の文字列)。接続は 1 つ、切断には複数の形がある。どれも
# (現在の文字列, 古いバージョンが書いた日本語の文字列) の組で、古いバージョンのログも読む。行の全体で比べ、前方一致には
# しない。"Disconnected from Vesktop" は、ハートビートで切断した行 "Disconnected from Vesktop: it stopped responding" の
# 先頭と同じなので、前方一致にすると、ハートビートの切断を close に数える。
CONNECT_UP_MARKS = ("Connected to Vesktop", "hello: 合言葉が一致した。接続を確立した")
CONNECT_DOWN_MARKS = {
    "close": ("Disconnected from Vesktop", "close: 接続が切れた"),
    "heartbeat": ("Disconnected from Vesktop: it stopped responding", "heartbeat: 応答がないため切断した"),
    "fail": ("Connection from Vesktop failed during the handshake", "fail: ハンドシェイクに失敗した"),
}
# モジュールを解放した行。古いバージョンは小文字で書いたので、両方を読む。
MODULE_UNLOADED_MARKS = ("Module unloaded", "module unloaded")

# stats と facts の行 (src-obs/pipeline/pipeline.cpp)。バージョンによってキーが増えても読めるように、キーと値の
# 組で読む。どちらも LOG_DEBUG なので、OBS のログには OBS を --verbose で起動したときだけ書かれる (実機テストの
# ツールは --verbose で起動する)。診断情報の zip の plugin.log には常に入る。古いバージョンは先頭を小文字
# (stats、facts) で書いたので、両方を読む。
STATS_STREAM_RE = re.compile(r"^\[cordmux\] [Ss]tats stream=(?P<stream>\d+) align (?P<pairs>.*)$")
FACTS_RE = re.compile(r"^\[cordmux\] [Ff]acts: (?P<pairs>.*)$")
PAIR_RE = re.compile(r"(\w+)=(-?[\d.]+|-)")

# 送信側のログの行 (src-vencord/adapter/connections.ts の ws classified と ws closed、src-vencord/index.ts の
# screen entry と onStage)。時刻は送信側の UTC。userId と server_id は読まず、結果にも書かない。
JS_TIME_RE = re.compile(r"^\[cordmux/js\] (?P<iso>\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z) (?P<body>.*)$")
JS_CLASSIFIED_RE = re.compile(r"^ws classified: ws=\d+ role=golive pcId=(?P<pc>\d+)")
JS_SCREEN_ENTRY_RE = re.compile(r"^screen entry: pcId=(?P<pc>\d+) userId=\S+ streamId=(?P<stream>\d+) tracks=(?P<tracks>\S*)")
JS_STAGE_RE = re.compile(r"^video stream (?P<stream>\d+): stage=")
JS_WS_CLOSED_RE = re.compile(
    r"^\[cordmux/js\] (?P<iso>\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z) ws closed: ws=\d+ role=(?P<role>\w+)"
)

LOG_NAME_DATE_RE = re.compile(r"(\d{4})-(\d{2})-(\d{2}) \d{2}-\d{2}-\d{2}")


# ---- ログを読む ---------------------------------------------------------------------------


class Line:
    """ログの 1 行。at は時刻が分かる行だけが持つ (datetime。日付が分からなければ 1970-01-01 の日付で持つ)。"""

    __slots__ = ("source", "number", "at", "text")

    def __init__(self, source, number, at, text):
        self.source = source
        self.number = number
        self.at = at
        self.text = text


class Record:
    """1 つのログ。timed は行に時刻があるか、date_known は日付が分かったかを表す。"""

    def __init__(self, name, lines, timed, date_known):
        self.name = name
        self.lines = lines
        self.timed = timed
        self.date_known = date_known


def _parse_date(text):
    return dt.datetime.strptime(text, "%Y-%m-%d").date()


def _date_from_name(name):
    m = LOG_NAME_DATE_RE.search(name)
    if not m:
        return None
    return dt.date(int(m.group(1)), int(m.group(2)), int(m.group(3)))


def _taken_local(manifest_bytes):
    # 診断情報の zip の manifest.json の作成時刻 (taken_at_us、UTC のエポックからのマイクロ秒) を、そのときの時差
    # (utc_offset_sec) で現地の時刻にする。OBS のログの行の時刻と比べられるように、時差を持たない datetime で返す。
    # 読めなければ None を返す。
    try:
        m = json.loads(manifest_bytes.decode("utf-8"))
        us = int(m["taken_at_us"])
        offset = int(m.get("utc_offset_sec", 0))
    except (ValueError, KeyError, TypeError, AttributeError):
        return None
    if us <= 0:
        return None
    return dt.datetime(1970, 1, 1) + dt.timedelta(microseconds=us, seconds=offset)


def _timed_lines(name, raw_lines, base_date):
    # OBS のログの行に日付を付ける。行は時刻だけを持ち日付を持たないので、時刻が 12 時間以上前に戻ったら日付が
    # 変わったとみなす。12 時間より小さい戻りは時計の修正 (Windows の時刻の同期) とみなし、日付を進めない。
    # 時刻のない行 (複数行にわたる OBS の出力) は捨てず、直前の時刻を持たせる。
    day = base_date
    previous = None
    lines = []
    for number, raw in enumerate(raw_lines, start=1):
        m = TIME_PREFIX_RE.match(raw)
        if not m:
            if previous is not None:
                lines.append(Line(name, number, previous, raw))
            continue
        clock = dt.datetime.strptime(m.group("time"), "%H:%M:%S.%f").time()
        at = dt.datetime.combine(day, clock)
        if previous is not None and at < previous - dt.timedelta(hours=12):
            day = day + dt.timedelta(days=1)
            at = dt.datetime.combine(day, clock)
        previous = at
        lines.append(Line(name, number, at, m.group("rest")))
    return lines


def _ring_with_prefix(raw_lines):
    # plugin.log の行は OBS のログと同じ「HH:MM:SS.mmm: 」で始まり、「[cordmux] 」は付かない (cordmux の行しか
    # 入らない)。OBS のログと同じに読むため、時刻の後に「[cordmux] 」を付ける。
    out = []
    for t in raw_lines:
        m = TIME_PREFIX_RE.match(t)
        out.append("%s: [cordmux] %s" % (m.group("time"), m.group("rest")) if m else t)
    return out


# OBS のログ、診断情報の zip、plugin.log を読む。日付は、OBS のログならファイル名 (YYYY-MM-DD HH-MM-SS.txt) から、
# zip なら manifest.json の作成時刻から取る。date_override (--date) を渡せばそれを使い、日付が分からなければ
# 1970-01-01 にする。行に時刻がない古いバージョンの plugin.log は、件数だけを数えられる Record (timed が偽) にする。
def read_record(path, date_override=None):
    path = pathlib.Path(path)
    if path.suffix.lower() == ".zip":
        with zipfile.ZipFile(path) as z:
            names = [n for n in z.namelist() if n.split("/")[-1] == "plugin.log"]
            if not names:
                raise ValueError("%s: plugin.log がない" % path)
            text = z.read(names[0]).decode("utf-8", errors="replace")
            manifest = [n for n in z.namelist() if n.split("/")[-1] == "manifest.json"]
            taken = _taken_local(z.read(manifest[0])) if manifest else None
        raw = text.splitlines()
        if not any(TIME_PREFIX_RE.match(t) for t in raw[:50]):
            # 古いバージョンの診断情報 (plugin.log の行に時刻がない)。
            lines = [Line(str(path), i, None, t) for i, t in enumerate(raw, start=1)]
            return Record(str(path), lines, timed=False, date_known=False)
        base = date_override or (taken.date() if taken else None)
        date_known = base is not None
        lines = _timed_lines(str(path), _ring_with_prefix(raw), base or dt.date(1970, 1, 1))
        if taken is not None and not date_override and lines and lines[-1].at > taken:
            # plugin.log の行は zip を作った時刻より前に書かれている。最後の行が作成時刻より後に読めたら、先頭の行は
            # 前の日である。日付の変わり目は前へ進めて数えるので、全部の行を同じ日数だけ戻す。
            back = dt.timedelta(days=(lines[-1].at - taken).days + 1)
            for line in lines:
                line.at = line.at - back
        return Record(str(path), lines, timed=True, date_known=date_known)

    text = path.read_text(encoding="utf-8", errors="replace")
    raw = text.splitlines()
    timed = any(TIME_PREFIX_RE.match(t) for t in raw[:50])
    if not timed:
        lines = [Line(str(path), i, None, t) for i, t in enumerate(raw, start=1)]
        return Record(str(path), lines, timed=False, date_known=False)
    if path.name.lower() == "plugin.log":
        raw = _ring_with_prefix(raw)
    base = date_override or _date_from_name(path.name)
    date_known = base is not None
    if base is None:
        base = dt.date(1970, 1, 1)
    return Record(str(path), _timed_lines(str(path), raw, base), timed=True, date_known=date_known)


def clip_record(record, time_from, time_to):
    # --from と --to の間の行だけを残す。準備の手順とメインの手順が 1 つのログに入るときに、メインの手順だけを数える
    # ためである。時刻だけの指定は、ログの中に入る日に当てはめる。時刻のないログは切れないので、そのまま返す。
    if not record.timed or not record.lines:
        return record
    first = next((l.at for l in record.lines if l.at is not None), None)
    last = next((l.at for l in reversed(record.lines) if l.at is not None), None)
    lo = _parse_fault_time(time_from, (first, last)) if time_from else None
    hi = _parse_fault_time(time_to, (first, last)) if time_to else None
    lines = [l for l in record.lines if (lo is None or l.at >= lo) and (hi is None or l.at <= hi)]
    return Record(record.name, lines, record.timed, record.date_known)


def _fmt_at(at, date_known):
    if at is None:
        return None
    if date_known:
        return at.isoformat(sep=" ", timespec="milliseconds")
    return at.strftime("%H:%M:%S.%f")[:-3]


# ---- メッセージの表示と消去 (notices) ------------------------------------------------------------


def notice_events(record):
    events = []
    for line in record.lines:
        m = NOTICE_RE.match(line.text)
        if not m:
            continue
        events.append(
            {
                "kind": m.group("kind"),
                "rule": m.group("rule"),
                "tier": m.group("tier"),
                "subject": m.group("subject").strip() or "-",
                "at": line.at,
                "line": line.number,
            }
        )
    return events


def find_chatter(events, count, window_s, event_kind):
    # 同じメッセージの表示を時刻の順に並べ、count 回目までが window_s 秒に入る所を拾う。
    # (1) 数えるのは表示の回数である。警報管理で言うチャタリング (1 分に 3 回以上鳴る警報) が数えるのは鳴った回数で、
    #     表示と消去の合計で数えると、2 往復で 4 回になり、2 倍ほど厳しくなる。合計で数えるときは
    #     --chatter-event both を渡す。
    # (2) 同じメッセージは、規則と対象 (subject) の組で見分け、メッセージのレベルは見ない。メッセージのレベルが 1 と 2 の間で
    #     変わっても、利用者には同じメッセージが表示されたり消えたりして見えるためである。ただし、枠に載っていない人の
    #     行の状態は対象が「-」になる (userId を書かない) ので、そういう 2 人の同じ状態は 1 つに数え、多めに数える
    #     ことがある。失敗したら、行の時刻を見て人を分ける。
    # (3) 60 秒の境も数える (最初と count 回目の差がちょうど 60.000 秒でも数える)。見逃すより数えすぎるほうが害が
    #     小さいためである。
    wanted = ("shown",) if event_kind == "shown" else ("shown", "cleared")
    groups = {}
    for e in events:
        if e["kind"] in wanted and e["at"] is not None:
            groups.setdefault((e["rule"], e["subject"]), []).append(e)
    window = dt.timedelta(seconds=window_s)
    episodes = []
    for (rule, subject), items in sorted(groups.items()):
        items.sort(key=lambda e: e["at"])
        i = 0
        while i + count - 1 < len(items):
            first = items[i]
            last = items[i + count - 1]
            if last["at"] - first["at"] <= window:
                # 続けて起きたものは 1 件にまとめ、window_s 秒に入る限り伸ばす。
                j = i + count - 1
                while j + 1 < len(items) and items[j + 1]["at"] - items[j + 2 - count]["at"] <= window:
                    j += 1
                episodes.append(
                    {
                        "rule": rule,
                        "subject": subject,
                        "tiers": sorted({e["tier"] for e in items[i : j + 1]}),
                        "from": first["at"],
                        "to": items[j]["at"],
                        "events": j - i + 1,
                    }
                )
                i = j + 1
            else:
                i += 1
    return episodes


def dwell_spans(events, record_end):
    # 表示から消去までの長さ。消えないままログが終わったメッセージは、ログの終わりで閉じ、open_at_end を真にする。
    open_at = {}
    spans = []
    for e in sorted((e for e in events if e["at"] is not None), key=lambda e: e["at"]):
        key = (e["rule"], e["subject"])
        if e["kind"] == "shown":
            open_at.setdefault(key, e)
        else:
            started = open_at.pop(key, None)
            if started is not None:
                spans.append({"rule": key[0], "subject": key[1], "tier": started["tier"],
                              "seconds": (e["at"] - started["at"]).total_seconds(), "open_at_end": False})
    for key, started in open_at.items():
        end = record_end or started["at"]
        spans.append({"rule": key[0], "subject": key[1], "tier": started["tier"],
                      "seconds": (end - started["at"]).total_seconds(), "open_at_end": True})
    return spans


UNKNOWN_FORMAT = ("読めない (メッセージの行が 0 行。メッセージの行を書かない古いバージョンのログと、メッセージが表示されなかったログを"
                  "区別できない。行を書くバージョンのログなら --build-writes-notices を付ける)")

FAULT_ARG_RE = re.compile(r"^(?P<rule>[^@]+)@(?P<at>[^/]+)/(?P<within>[\d.]+)$")


def _parse_fault_time(text, span):
    # span はログの最初と最後の時刻。時刻だけの指定は、途中で日付が変わるログでも、ログの中に入る日に当てはめる。
    text = text.strip()
    for fmt in ("%Y-%m-%d %H:%M:%S.%f", "%Y-%m-%d %H:%M:%S", "%Y-%m-%dT%H:%M:%S.%f", "%Y-%m-%dT%H:%M:%S"):
        try:
            return dt.datetime.strptime(text, fmt)
        except ValueError:
            pass
    for fmt in ("%H:%M:%S.%f", "%H:%M:%S"):
        try:
            clock = dt.datetime.strptime(text, fmt).time()
        except ValueError:
            continue
        first, last = span
        day = first.date()
        while day <= last.date():
            candidate = dt.datetime.combine(day, clock)
            if first <= candidate <= last:
                return candidate
            day += dt.timedelta(days=1)
        return dt.datetime.combine(first.date(), clock)
    raise ValueError("時刻の形が読めない: %s" % text)


def parse_faults(fault_args, faults_file, span):
    # --fault <規則>@<時刻>/<秒> と、TSV (列: rule, at, within_s, note。# の行と空行は飛ばす)。
    faults = []
    for arg in fault_args or []:
        m = FAULT_ARG_RE.match(arg)
        if not m:
            raise ValueError("--fault の形は <規則>@<時刻>/<秒>: %s" % arg)
        faults.append({"rule": m.group("rule"), "at": _parse_fault_time(m.group("at"), span),
                       "within_s": float(m.group("within")), "note": ""})
    if faults_file:
        for raw in pathlib.Path(faults_file).read_text(encoding="utf-8").splitlines():
            if not raw.strip() or raw.lstrip().startswith("#"):
                continue
            cols = raw.split("\t")
            if cols[0] == "rule":
                continue
            if len(cols) < 3:
                raise ValueError("faults の行は rule, at, within_s, note の 3 列以上: %s" % raw)
            faults.append({"rule": cols[0], "at": _parse_fault_time(cols[1], span),
                           "within_s": float(cols[2]), "note": cols[3] if len(cols) > 3 else ""})
    return faults


def judge_faults(events, faults):
    # 故障を起こした時刻の後、上限の秒数以内にその規則のメッセージが表示されたか。故障の前から表示されていたメッセージは
    # 数えない。上限は規則ごとに違う (待ってから表示する推測の状態と、待たずに表示するイベントがある) ので、このツールは
    # 既定の上限を持たず、故障ごとに --fault の /<秒> で受け取る。
    results = []
    for f in faults:
        hits = [e for e in events if e["kind"] == "shown" and e["rule"] == f["rule"] and e["at"] is not None
                and e["at"] >= f["at"]]
        hits.sort(key=lambda e: e["at"])
        if hits:
            latency = (hits[0]["at"] - f["at"]).total_seconds()
            ok = latency <= f["within_s"]
            results.append({"rule": f["rule"], "fault_at": f["at"], "within_s": f["within_s"], "latency_s": latency,
                            "tier": hits[0]["tier"], "ok": ok, "note": f["note"]})
        else:
            results.append({"rule": f["rule"], "fault_at": f["at"], "within_s": f["within_s"], "latency_s": None,
                            "tier": None, "ok": False, "note": f["note"]})
    return results


def count_notices(records, args):
    per_record = []
    all_ok = True
    unreadable = False
    verdict_used = False
    for record in records:
        events = notice_events(record)
        timed = record.timed
        end = next((l.at for l in reversed(record.lines) if l.at is not None), None)
        shown = [e for e in events if e["kind"] == "shown"]
        by_tier = {t: sum(1 for e in shown if e["tier"] == t) for t in TIERS}
        by_rule = {}
        for e in events:
            row = by_rule.setdefault((e["rule"], e["tier"]), {"rule": e["rule"], "tier": e["tier"], "shown": 0, "cleared": 0})
            row[e["kind"]] += 1
        result = {
            "record": record.name,
            "timed": timed,
            "date_known": record.date_known,
            "notice_lines": len(events),
            "shown_by_tier": by_tier,
            "by_rule": sorted(by_rule.values(), key=lambda r: (TIERS.index(r["tier"]), r["rule"])),
        }
        if len(events) == 0:
            result["note"] = ("メッセージの表示と消去の行が 1 つもない。メッセージの行を書かない古いバージョンのログか、"
                              "メッセージが 1 度も表示されなかったログのどちらかで、このログだけでは区別できない")
        if timed:
            chatter = find_chatter(events, args.chatter_count, args.chatter_window_s, args.chatter_event)
            result["chatter"] = [dict(c, **{"from": _fmt_at(c["from"], record.date_known),
                                            "to": _fmt_at(c["to"], record.date_known)}) for c in chatter]
            spans = dwell_spans(events, end)
            result["dwell"] = spans
        else:
            chatter = None
            result["chatter"] = "読めない (行に時刻がない)"
            result["dwell"] = "読めない (行に時刻がない)"

        checks = []
        # メッセージの行が 0 行のログは、行を書くバージョンだと分かっているとき (--build-writes-notices) だけ 0 件として
        # 数える。cordmux はバージョンをログに書かないので、行を書かない古いバージョンのログと区別できない。0 行を成功に
        # すると、古いバージョンのログが成功になってしまう。
        format_unknown = len(events) == 0 and not args.build_writes_notices
        if args.normal:
            verdict_used = True
            if format_unknown:
                checks.append({"check": "normal-tiers: メッセージのレベル 1 と 2 が 0 件", "ok": None, "value": UNKNOWN_FORMAT})
            else:
                checks.append({"check": "normal-tiers: メッセージのレベル 1 と 2 が 0 件", "ok": by_tier["1"] == 0 and by_tier["2"] == 0,
                               "value": {"1": by_tier["1"], "2": by_tier["2"]}})
            if format_unknown:
                checks.append({"check": "normal-chatter: 1 分に %d 回以上の表示 (チャタリング) が 0 件" % args.chatter_count,
                               "ok": None, "value": UNKNOWN_FORMAT})
            elif chatter is None:
                checks.append({"check": "normal-chatter: 1 分に %d 回以上の表示 (チャタリング) が 0 件" % args.chatter_count,
                               "ok": None, "value": "読めない"})
            else:
                checks.append({"check": "normal-chatter: 1 分に %d 回以上の表示 (チャタリング) が 0 件" % args.chatter_count,
                               "ok": len(chatter) == 0, "value": len(chatter)})
            checks.append({"check": "normal-false-alarm: メッセージのレベル 1 は実際に問題が起きたときだけ (誤検知 0)", "ok": None,
                           "value": "人が判断する (メッセージのレベル 1 の行の時刻を、ログの前後の行と見比べる)"})
        if args.fault or args.faults:
            verdict_used = True
            first = next((l.at for l in record.lines if l.at is not None), dt.datetime(1970, 1, 1))
            faults = parse_faults(args.fault, args.faults, (first, end or first))
            judged = judge_faults(events, faults) if timed else []
            if not timed:
                checks.append({"check": "fault: 故障から決めた時間以内に表示された", "ok": None, "value": "読めない (行に時刻がない)"})
            for r in judged:
                checks.append({"check": "fault: %s が %.1f 秒以内に表示された" % (r["rule"], r["within_s"]), "ok": r["ok"],
                               "value": {"fault_at": _fmt_at(r["fault_at"], record.date_known), "latency_s": r["latency_s"],
                                         "tier": r["tier"], "note": r["note"]}})
        if checks:
            result["checks"] = checks
            if any(c["ok"] is False for c in checks):
                all_ok = False
            # 人が判断する normal-false-alarm を除き、判定できない項目が 1 つでもあれば、全体を読めないとする
            # (成功にしない)。
            if any(c["ok"] is None and not c["check"].startswith("normal-false-alarm") for c in checks):
                unreadable = True
        per_record.append(result)
    out = {"tool": "notice_count notices", "chatter_rule": {"count": args.chatter_count, "window_s": args.chatter_window_s,
                                                              "event": args.chatter_event}, "records": per_record}
    if not verdict_used:
        return out, 0
    if not all_ok:
        out["ok"] = False
        return out, 1
    if unreadable:
        out["ok"] = None
        return out, 2
    out["ok"] = True
    return out, 0


# ---- 時間の集計 (timing) ---------------------------------------------------------------------


def _pairs(text):
    return {k: v for k, v in PAIR_RE.findall(text)}


def _quantile(sorted_values, q):
    if not sorted_values:
        return None
    pos = q * (len(sorted_values) - 1)
    lo = int(pos)
    hi = min(lo + 1, len(sorted_values) - 1)
    return sorted_values[lo] + (sorted_values[hi] - sorted_values[lo]) * (pos - lo)


def _utc_offset(text, sample_date):
    if text:
        m = re.match(r"^([+-])(\d{2}):?(\d{2})$", text)
        if not m:
            raise ValueError("--utc-offset の形は +09:00: %s" % text)
        sign = 1 if m.group(1) == "+" else -1
        return dt.timedelta(hours=sign * int(m.group(2)), minutes=sign * int(m.group(3)))
    # 渡されなければ、このツールを実行する PC の時間帯の、その日の差を使う。ログを取った PC で実行する前提である。
    local = dt.datetime.combine(sample_date, dt.time(12, 0)).astimezone()
    return local.utcoffset()


def frame_windows(record):
    # ストリームごとに、続く 2 つの stats の行の差から、その周期のフレーム数と不連続 (jump) の数を求める。ログは
    # 1 フレームずつの時刻を持たない (stats の行は周期ごとの累計だけ) ので、フレームの間隔の分布は読めない。読めるのは
    # 周期の中のフレーム数 (毎秒の数) と、推定した周期の 1.5 倍を超えた間隔の回数である。毎秒の数から間隔を推定すると、
    # 1 回の長い途切れと、均等な遅さを区別できない。
    last = {}
    windows = []
    for line in record.lines:
        m = STATS_STREAM_RE.match(line.text)
        if not m or line.at is None:
            continue
        stream = int(m.group("stream"))
        p = _pairs(m.group("pairs"))
        try:
            frames = int(p["frames"])
            jump = int(p.get("jump", "0"))
        except (KeyError, ValueError):
            continue
        period_us = float(p.get("period_us", "0") or 0)
        prev = last.get(stream)
        last[stream] = (line.at, frames, jump)
        if prev is None:
            continue
        seconds = (line.at - prev[0]).total_seconds()
        if frames < prev[1] or seconds <= 0:
            continue  # 数え直し (同じ番号の別のストリーム) か、時刻の戻り
        windows.append({"stream": stream, "at": line.at, "seconds": seconds, "frames": frames - prev[1],
                        "jumps": max(0, jump - prev[2]), "period_us": period_us})
    return windows


def _zero_runs(windows):
    # フレームが 0 の周期が続く所を、ストリームごとに、途中 (後でまたフレームが届いた) と終わり (そのストリームの
    # 最後まで 0) に分ける。
    by_stream = {}
    for w in windows:
        by_stream.setdefault(w["stream"], []).append(w)
    middle = []
    tail = []
    for stream, items in sorted(by_stream.items()):
        run = []
        for w in items:
            if w["frames"] == 0:
                run.append(w)
                continue
            if run:
                middle.append({"stream": stream, "from": run[0]["at"], "windows": len(run),
                               "seconds": sum(x["seconds"] for x in run)})
                run = []
        if run:
            tail.append({"stream": stream, "from": run[0]["at"], "windows": len(run),
                         "seconds": sum(x["seconds"] for x in run)})
    return middle, tail


def connection_spans(record):
    events = []
    for line in record.lines:
        text = line.text
        if not text.startswith("[cordmux] ") or line.at is None:
            continue
        body = text[len("[cordmux] "):].rstrip()
        if body in CONNECT_UP_MARKS:
            events.append(("up", None, line.at))
        elif body.startswith(MODULE_UNLOADED_MARKS):
            events.append(("end", None, line.at))
        else:
            for kind, marks in CONNECT_DOWN_MARKS.items():
                if body in marks:
                    events.append(("down", kind, line.at))
    drops = []
    # None は、ログの先頭ではまだ分からないことを表す (--from で途中から切ったログ)。分からない間の切断も数える。
    connected = None
    down = None
    for kind, why, at in events:
        if kind == "up":
            if down is not None:
                down["back_after_s"] = (at - down["at"]).total_seconds()
                drops.append(down)
                down = None
            connected = True
        elif kind == "down" and connected is not False:
            down = {"why": why, "at": at, "back_after_s": None}
            connected = False
        elif kind == "end":
            if down is not None:
                down["ended_by"] = "module unloaded"
                drops.append(down)
                down = None
            connected = False
    if down is not None:
        drops.append(down)
    return drops


def _js_utc(iso):
    text = iso.replace("Z", "")
    return dt.datetime.strptime(text, "%Y-%m-%dT%H:%M:%S.%f" if "." in text else "%Y-%m-%dT%H:%M:%S")


def first_frames(record, watch_window_s=5.0):
    # 最初のフレームまでの時間を、送信側の 3 つの行の時刻 (どれも送信側の時計) だけで数える。視聴の開始は、golive の
    # 接続を分類した時刻 (ws classified) から、その接続の映像の screen entry ができて最初のエンコードの段階
    # (video stream N: stage=) が始まるまで。分類から watch_window_s 秒より後にできた screen entry は再接続とする。
    # cordmux へ再接続すると、既にある screen entry をもう一度書き、視聴の開始の時刻がないためである。
    # OBS 側にはフレームが最初に届いた行がない (stats は 10 秒ごとの累計) ので、枠に表示されるまでの残りの時間は
    # 読めない。OBS 側の stats でフレームが最初に増えた周期を使うと 10 秒刻みになり、2 秒前後のしきい値を決める
    # のに使えない。
    classified = {}
    pending = {}
    watch = []
    reentry = []
    for line in record.lines:
        m = JS_TIME_RE.match(line.text)
        if not m:
            continue
        at = _js_utc(m.group("iso"))
        body = m.group("body")
        c = JS_CLASSIFIED_RE.match(body)
        if c:
            classified[c.group("pc")] = at
            continue
        e = JS_SCREEN_ENTRY_RE.match(body)
        if e:
            if "video" not in e.group("tracks").split(","):
                continue
            stream = e.group("stream")
            if stream in pending:
                continue  # 同じ screen entry に音声のトラックが加わって書き直されたもの (最初の時刻を保つ)
            since = classified.get(e.group("pc"))
            is_watch = since is not None and 0 <= (at - since).total_seconds() <= watch_window_s
            pending[stream] = (since if is_watch else at, is_watch, at)
            continue
        s = JS_STAGE_RE.match(body)
        if s and s.group("stream") in pending:
            start, is_watch, entry_at = pending.pop(s.group("stream"))
            item = {"from_watch_s": (at - start).total_seconds() if is_watch else None,
                    "from_entry_s": (at - entry_at).total_seconds()}
            (watch if is_watch else reentry).append(item)
    return watch, reentry, len(pending)


def share_end_arrivals(record, utc_offset):
    # 送信側の「ws closed ... role=golive」(送信側の時刻) から、OBS 側の facts の sharing が減った行までの差を数える。
    # 画面共有をやめる操作の時刻はどのログにもないので、Discord のサーバーを通る前半は読めない。通話を抜けたことも、
    # Discord 側に時刻の付いた行がないので、facts の roster が減った回数だけを数える。
    facts = []
    closes = []
    for line in record.lines:
        if line.at is None:
            continue
        m = FACTS_RE.match(line.text)
        if m:
            p = _pairs(m.group("pairs"))
            facts.append((line.at, p.get("sharing"), p.get("roster")))
            continue
        m = JS_WS_CLOSED_RE.match(line.text)
        if m and m.group("role") == "golive":
            utc = dt.datetime.strptime(m.group("iso").replace("Z", ""), "%Y-%m-%dT%H:%M:%S.%f"
                                       if "." in m.group("iso") else "%Y-%m-%dT%H:%M:%S")
            closes.append(utc + utc_offset)
    decreases = []
    roster_decreases = 0
    prev_share = None
    prev_roster = None
    for at, sharing, roster in facts:
        s = int(sharing) if sharing not in (None, "-") else None
        r = int(roster) if roster not in (None, "-") else None
        if s is not None and prev_share is not None and s < prev_share:
            decreases.append(at)
        if r is not None and prev_roster is not None and r < prev_roster:
            roster_decreases += 1
        if s is not None:
            prev_share = s
        if r is not None:
            prev_roster = r
    arrivals = []
    used = set()
    for closed_at in closes:
        best = None
        for i, at in enumerate(decreases):
            if i in used:
                continue
            delta = (at - closed_at).total_seconds()
            if -5.0 <= delta <= 30.0 and (best is None or abs(delta) < abs(best[1])):
                best = (i, delta)
        if best is not None:
            used.add(best[0])
            arrivals.append({"ws_closed_at": closed_at, "seconds": best[1]})
        else:
            arrivals.append({"ws_closed_at": closed_at, "seconds": None})
    return arrivals, len(decreases), roster_decreases


def measure_timing(records, args):
    per_record = []
    for record in records:
        if not record.timed:
            per_record.append({"record": record.name, "note": "読めない (行に時刻がない。OBS のログを渡す)"})
            continue
        windows = frame_windows(record)
        rates = sorted(w["frames"] / w["seconds"] for w in windows)
        nonzero = sorted(w["frames"] / w["seconds"] for w in windows if w["frames"] > 0)
        middle, tail = _zero_runs(windows)
        jumps_total = sum(w["jumps"] for w in windows)
        stream_seconds = sum(w["seconds"] for w in windows)
        drops = connection_spans(record)
        first_at = next((l.at for l in record.lines if l.at is not None), None)
        offset = _utc_offset(args.utc_offset, first_at.date() if first_at else dt.date(1970, 1, 1))
        arrivals, share_decreases, roster_decreases = share_end_arrivals(record, offset)
        last_at = next((l.at for l in reversed(record.lines) if l.at is not None), None)
        watch, reentry, unmatched = first_frames(record)
        per_record.append({
            "record": record.name,
            "span_s": (last_at - first_at).total_seconds() if first_at and last_at else None,
            "frames": {
                "streams": len({w["stream"] for w in windows}),
                "windows": len(windows),
                "stream_hours": stream_seconds / 3600.0,
                "fps_all": {"min": rates[0] if rates else None, "p01": _quantile(rates, 0.01), "p05": _quantile(rates, 0.05),
                            "median": _quantile(rates, 0.5), "max": rates[-1] if rates else None},
                "fps_nonzero": {"min": nonzero[0] if nonzero else None, "p01": _quantile(nonzero, 0.01),
                                "p05": _quantile(nonzero, 0.05), "median": _quantile(nonzero, 0.5)},
                "zero_windows_middle": [dict(z, **{"from": _fmt_at(z["from"], record.date_known)}) for z in middle],
                "zero_windows_tail": len(tail),
                "jumps_total": jumps_total,
                "jumps_per_stream_hour": (jumps_total / (stream_seconds / 3600.0)) if stream_seconds else None,
                "windows_with_jumps": sum(1 for w in windows if w["jumps"] > 0),
                "max_jumps_in_window": max((w["jumps"] for w in windows), default=0),
                "per_frame_interval": "読めない (ログはフレームごとの時刻を持たない)",
            },
            "connection": {
                "drops": [{"why": d["why"], "at": _fmt_at(d["at"], record.date_known), "back_after_s": d["back_after_s"],
                           "ended_by": d.get("ended_by")} for d in drops],
                "drops_total": len(drops),
                "heartbeat_drops": sum(1 for d in drops if d["why"] == "heartbeat"),
            },
            "share_end": {
                "sharing_decreases": share_decreases,
                "renderer_to_obs_s": [a["seconds"] for a in arrivals],
                "sender_to_discord": "読めない (画面共有をやめた操作の時刻がログにない)",
            },
            "leave_call": {
                "roster_decreases": roster_decreases,
                "latency": "読めない (Discord 側に時刻の付いた行がない)",
            },
            "first_frame": {
                "watch_start_s": [w["from_watch_s"] for w in watch],
                "watch_entry_s": [w["from_entry_s"] for w in watch],
                "reentry_s": [w["from_entry_s"] for w in reentry],
                "no_stage_after_entry": unmatched,
                "scope": "送信側の時刻だけ。OBS の枠に表示されるまでの残り (送信とデコード) は含まない",
            },
        })
    return {"tool": "notice_count timing", "records": per_record}, 0


# ---- Markdown の表 -----------------------------------------------------------------------------------


def _num(v, digits=1):
    if v is None:
        return "-"
    if isinstance(v, float):
        return ("%." + str(digits) + "f") % v
    return str(v)


def markdown_notices(out):
    rows = ["| ログ | 時刻 | 行 | メッセージのレベル 1 | メッセージのレベル 2 | メッセージのレベル 3 | 人の行の状態 | 1 分に %d 回以上 | 合否 |" % out["chatter_rule"]["count"],
            "|---|---|---|---|---|---|---|---|---|"]
    for r in out["records"]:
        chatter = r["chatter"] if isinstance(r["chatter"], str) else str(len(r["chatter"]))
        verdict = "-"
        if "checks" in r:
            if any(c["ok"] is False for c in r["checks"]):
                verdict = "失敗"
            elif any(c["ok"] is None and not c["check"].startswith("normal-false-alarm") for c in r["checks"]):
                verdict = "読めない"
            else:
                verdict = "成功 (人が判断する項目を除く)"
        rows.append("| %s | %s | %d | %d | %d | %d | %d | %s | %s |" % (
            pathlib.Path(r["record"]).name, "あり" if r["timed"] else "なし", r["notice_lines"],
            r["shown_by_tier"]["1"], r["shown_by_tier"]["2"], r["shown_by_tier"]["3"], r["shown_by_tier"]["row"],
            chatter, verdict))
    return "\n".join(rows)


def markdown_timing(out):
    rows = ["| ログ | 長さ (分) | ストリーム | 周期 | 毎秒のフレーム (0 を除く) 最小 / 1% / 5% / 中央 | 途中のフレーム 0 の周期 | 不連続 (ストリーム x 時間あたり) | 切断 (うちハートビート) と再接続まで (秒) | 画面共有の終わり: 送信側から OBS まで (秒) | 通話を抜けた (回) |",
            "|---|---|---|---|---|---|---|---|---|---|"]
    for r in out["records"]:
        if "frames" not in r:
            rows.append("| %s | %s |  |  |  |  |  |  |  |  |" % (pathlib.Path(r["record"]).name, r["note"]))
            continue
        f = r["frames"]
        c = r["connection"]
        nz = f["fps_nonzero"]
        middle = f["zero_windows_middle"]
        middle_text = "0" if not middle else "; ".join("ストリーム %d %s から %d 周期" % (z["stream"], z["from"], z["windows"]) for z in middle)
        drops = "%d (%d)" % (c["drops_total"], c["heartbeat_drops"])
        if c["drops"]:
            drops += ": " + ", ".join(_num(d["back_after_s"]) if d["back_after_s"] is not None else (d.get("ended_by") or "再接続せず")
                                     for d in c["drops"])
        share = ", ".join(_num(s, 2) if s is not None else "対応なし" for s in r["share_end"]["renderer_to_obs_s"]) or "-"
        rows.append("| %s | %s | %d | %d | %s / %s / %s / %s | %s | %d (%s) | %s | %s | %d |" % (
            pathlib.Path(r["record"]).name, _num(r["span_s"] / 60.0 if r["span_s"] else None), f["streams"], f["windows"],
            _num(nz["min"]), _num(nz["p01"]), _num(nz["p05"]), _num(nz["median"]), middle_text,
            f["jumps_total"], _num(f["jumps_per_stream_hour"]), drops, share, r["leave_call"]["roster_decreases"]))
    rows += ["", "| ログ | 視聴の開始から最初のフレーム (秒) 最小 / 中央 / 最大 (件) | 再接続の screen entry から最初のフレーム (秒) 最小 / 中央 / 最大 (件) | screen entry の後に最初のフレームがない |",
             "|---|---|---|---|"]
    for r in out["records"]:
        if "first_frame" not in r:
            continue
        ff = r["first_frame"]
        rows.append("| %s | %s | %s | %d |" % (pathlib.Path(r["record"]).name, _spread(ff["watch_start_s"]),
                                              _spread(ff["reentry_s"]), ff["no_stage_after_entry"]))
    return "\n".join(rows)


def _spread(values):
    v = sorted(x for x in values if x is not None)
    if not v:
        return "- (0)"
    return "%s / %s / %s (%d)" % (_num(v[0], 2), _num(_quantile(v, 0.5), 2), _num(v[-1], 2), len(v))


def _emit(text):
    # 端末の文字コードによらず UTF-8 で書く。Windows の既定の cp932 では、文字化けするか例外になる。
    sys.stdout.buffer.write((text + "\n").encode("utf-8"))
    sys.stdout.flush()


def _json_default(v):
    if isinstance(v, (dt.datetime, dt.date)):
        return v.isoformat()
    raise TypeError(type(v))


# ---- セルフテスト -----------------------------------------------------------------------------------


def _source_text(rel):
    return (REPO_ROOT / rel).read_text(encoding="utf-8")


def _render_c_format(fmt, values):
    # C の書式の %s を順に埋める (このツールが読む 2 つの行は %s だけでできている)。
    parts = fmt.split("%s")
    if len(parts) - 1 != len(values):
        raise AssertionError("書式の %%s の数が合わない: %s" % fmt)
    out = parts[0]
    for v, rest in zip(values, parts[1:]):
        out += v + rest
    return out


def _args(**kw):
    base = dict(build_writes_notices=False, chatter_count=3, chatter_window_s=60.0, chatter_event="shown", normal=False, fault=None, faults=None,
                utc_offset=None, date=None, markdown=False)
    base.update(kw)
    return argparse.Namespace(**base)


def selftest():
    failures = []

    def check(name, cond, detail=""):
        if not cond:
            failures.append("%s %s" % (name, detail))

    # 1. 製品のソースにある行の形を、このツールが読めるか。形が変わると失敗するので、ツールをソースに合わせる。
    dock = _source_text("src-obs/dock/dock_poll.cpp")
    formats = re.findall(r'"(Notice (?:shown|cleared) \(rule=%s tier=%s subject=%s\))"', dock)
    check("dock_poll.cpp にメッセージの表示と消去の 2 行がある", sorted(set(formats)) == [
        "Notice cleared (rule=%s tier=%s subject=%s)", "Notice shown (rule=%s tier=%s subject=%s)"], str(formats))
    for fmt in set(formats):
        for tier in TIERS:
            for subject in ("slot 3", "-"):
                line = "[cordmux] " + _render_c_format(fmt, ["RowStatus.Held", tier, subject])
                m = NOTICE_RE.match(line)
                check("行を読める: " + line, m is not None and m.group("subject") == subject and m.group("tier") == tier)
    for tier in TIERS:
        check("dock_poll.cpp のメッセージのレベルの名前 %s" % tier, ('return "%s";' % tier) in dock)
    tracker = _source_text("src-obs/ledger/notice_tracker.cpp")
    check("notice_tracker.cpp の対象は slot <n>", '"slot " + std::to_string' in tracker)
    check("plugin-main.cpp の「[cordmux] 」", '"[cordmux] "' in _source_text("src-obs/plugin-main.cpp"))
    listener = _source_text("src-obs/ws/listener.cpp")
    # 現在の文字列 (組の最初) がソースにあること。古い文字列は、古いログを読むためだけに持つ。
    for mark in [CONNECT_UP_MARKS[0]] + [marks[0] for marks in CONNECT_DOWN_MARKS.values()]:
        check("listener.cpp の文字列: " + mark, ('log("%s")' % mark) in listener)
    # plugin.log の時刻: pipeline.cpp の push_plugin_log が OBS のログと同じ形の先頭を付ける (書式は log_ring.h)。
    check("log_ring.h の時刻の形", '"%02d:%02d:%02d.%03d: "' in _source_text("src-obs/pipeline/log_ring.h"))
    pipeline = _source_text("src-obs/pipeline/pipeline.cpp")
    for frag in ('"Stats stream=%u align period_us=%.1f frames=%lld', "jump=%lld", '"Stats video received_hz=',
                 "sharing=%zu", "roster=%s", '"Facts: '):
        check("pipeline.cpp の文字列: " + frag, frag in pipeline)
    check("plugin-main.cpp の Module unloaded", ('"%s"' % MODULE_UNLOADED_MARKS[0]) in _source_text("src-obs/plugin-main.cpp"))
    index_ts = _source_text("src-vencord/index.ts")
    check("index.ts の screen entry", "screen entry: pcId=${pcId} userId=${userId} streamId=${entry.streamId} tracks=" in index_ts)
    check("limits.ts の stage", "video stream ${streamId}: stage=${stage}" in _source_text("src-vencord/encode/limits.ts"))
    check("connections.ts の ws classified", "ws classified: ws=" in _source_text("src-vencord/adapter/connections.ts"))
    check("connections.ts の ws closed", "ws closed: ws=${serial} role=${ws.role}" in
          _source_text("src-vencord/adapter/connections.ts"))

    # テストデータのログを作って数える。読み違いで例外になっても失敗として返す。途中で例外のまま終わると、失敗の
    # 理由が残らないためである。
    try:
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)

            # 2. メッセージの数え方 (メッセージのレベルごとの数、チャタリング、表示されたまま終わったメッセージ、合否)。
            log = tmp / "2000-01-10 23-59-00.txt"
            log.write_text("\n".join([
                "23:59:00.000: Command Line Arguments: --portable",
                "23:59:01.000: [cordmux] Notice shown (rule=DoubleAudio tier=2 subject=-)",
                "23:59:03.500: [cordmux] Notice cleared (rule=DoubleAudio tier=2 subject=-)",
                # 3 回目までの差がちょうど 60 秒 (境も数える)。途中で日付が変わる。
                "23:59:10.000: [cordmux] Notice shown (rule=RowStatus.Held tier=row subject=slot 2)",
                "23:59:20.000: [cordmux] Notice cleared (rule=RowStatus.Held tier=row subject=slot 2)",
                "23:59:40.000: [cordmux] Notice shown (rule=RowStatus.Held tier=row subject=slot 2)",
                "23:59:50.000: [cordmux] Notice cleared (rule=RowStatus.Held tier=row subject=slot 2)",
                "00:00:10.000: [cordmux] Notice shown (rule=RowStatus.Held tier=row subject=slot 2)",
                "00:00:12.000: [cordmux] Notice cleared (rule=RowStatus.Held tier=row subject=slot 2)",
                # 対象が違えば別のメッセージ。60 秒を 1 ms 超える 3 回は数えない。
                "00:01:00.000: [cordmux] Notice shown (rule=RowStatus.Held tier=row subject=slot 3)",
                "00:01:01.000: [cordmux] Notice cleared (rule=RowStatus.Held tier=row subject=slot 3)",
                "00:01:30.000: [cordmux] Notice shown (rule=RowStatus.Held tier=row subject=slot 3)",
                "00:01:31.000: [cordmux] Notice cleared (rule=RowStatus.Held tier=row subject=slot 3)",
                "00:02:00.001: [cordmux] Notice shown (rule=RowStatus.Held tier=row subject=slot 3)",
                # 故障から 2.5 秒でメッセージのレベル 1 が表示され、消えないまま終わる。
                "00:03:00.000: [cordmux] Notice shown (rule=AllVideoStoppedHint tier=1 subject=-)",
                "00:03:30.000: unrelated line",
                "00:04:00.000: [cordmux] module unloaded",
            ]) + "\n", encoding="utf-8")
            rec = read_record(log)
            check("日付をファイル名から取る", rec.date_known and rec.lines[0].at.date() == dt.date(2000, 1, 10))
            out, code = count_notices([rec], _args(normal=True, fault=["AllVideoStoppedHint@00:02:57.500/5",
                                                                       "RecordStoppedSpaceExhausted@00:02:00/10"]))
            r = out["records"][0]
            check("メッセージのレベルごとの数", r["shown_by_tier"] == {"1": 1, "2": 1, "3": 0, "row": 6}, str(r["shown_by_tier"]))
            check("チャタリング 1 件 (slot 2、ちょうど 60 秒)", len(r["chatter"]) == 1 and r["chatter"][0]["subject"] == "slot 2"
                  and r["chatter"][0]["events"] == 3, str(r["chatter"]))
            check("日付が変わった後の時刻", r["chatter"][0]["to"].startswith("2000-01-11 00:00:10"), str(r["chatter"]))
            open_spans = [s for s in r["dwell"] if s["open_at_end"]]
            check("表示されたまま終わったメッセージ", len(open_spans) == 2 and {s["rule"] for s in open_spans} ==
                  {"AllVideoStoppedHint", "RowStatus.Held"}, str(open_spans))
            fault_checks = [c for c in r["checks"] if c["check"].startswith("fault")]
            check("故障から 2.5 秒で表示された", fault_checks[0]["ok"] is True and abs(fault_checks[0]["value"]["latency_s"] - 2.5) < 1e-6,
                  str(fault_checks))
            check("表示されなかった故障は失敗", fault_checks[1]["ok"] is False and fault_checks[1]["value"]["latency_s"] is None)
            check("正常なときの合否は失敗 (メッセージのレベル 1 と 2、チャタリング)", out["ok"] is False and code == 1)
            out_both, _ = count_notices([rec], _args(chatter_event="both"))
            check("表示と消去の合計で数えるオプション", len(out_both["records"][0]["chatter"]) == 2, str(out_both["records"][0]["chatter"]))

            # 3. 正常なときは成功。行がないログは note を持つ。
            clean = tmp / "2000-01-10 10-00-00.txt"
            clean.write_text("10:00:00.000: [cordmux] Notice shown (rule=RecordGap tier=3 subject=-)\n"
                             "10:00:05.000: [cordmux] Notice cleared (rule=RecordGap tier=3 subject=-)\n", encoding="utf-8")
            out, code = count_notices([read_record(clean)], _args(normal=True))
            check("正常なときは成功", out["ok"] is True and code == 0, json.dumps(out, default=_json_default))
            empty = tmp / "2000-01-10 11-00-00.txt"
            empty.write_text("11:00:00.000: [cordmux] decoder=software\n", encoding="utf-8")
            out, _ = count_notices([read_record(empty)], _args())
            check("行がないログの note", "note" in out["records"][0])
            out, code = count_notices([read_record(empty)], _args(normal=True))
            check("行が 0 行のログは読めない", out["ok"] is None and code == 2 and
                  "読めない" in markdown_notices(out), markdown_notices(out))
            out, code = count_notices([read_record(empty)], _args(normal=True, build_writes_notices=True))
            check("行を書くバージョンと分かれば 0 行は成功", out["ok"] is True and "成功" in markdown_notices(out), markdown_notices(out))

            # 4. 古いバージョンの診断情報の zip の plugin.log (時刻も「[cordmux] 」もない)。
            bundle = tmp / "bundle.zip"
            with zipfile.ZipFile(bundle, "w") as z:
                z.writestr("plugin.log", "Notice shown (rule=DoubleAudio tier=2 subject=-)\n"
                                         "Notice cleared (rule=DoubleAudio tier=2 subject=-)\n")
            out, code = count_notices([read_record(bundle)], _args(normal=True))
            r = out["records"][0]
            check("zip の行を数える", r["shown_by_tier"]["2"] == 1 and r["timed"] is False)
            check("zip ではチャタリングは読めない", isinstance(r["chatter"], str))
            check("zip でもメッセージのレベル 2 は失敗", out["ok"] is False and code == 1)

            # 4b. 現在のバージョンの zip (plugin.log の行に OBS のログと同じ時刻があり、「[cordmux] 」はない)。
            # 作成時刻 (manifest.json) は 2000-01-02 00:00:30 (現地。UTC の 2000-01-01 15:00:30 と時差 +9 時間)。
            # 先頭の行は前の日の 23:59 台で、日付が変わった後の行は作成した日、先頭の行は前の日として読む。
            bundle2 = tmp / "bundle2.zip"
            taken_us = int((dt.datetime(2000, 1, 1, 15, 0, 30) - dt.datetime(1970, 1, 1)).total_seconds()) * 1000000
            with zipfile.ZipFile(bundle2, "w") as z:
                z.writestr("manifest.json", json.dumps({"taken_at_us": taken_us, "utc_offset_sec": 9 * 3600}))
                z.writestr("plugin.log", "# dropped 3 older lines (ring limit)\n"
                                         "23:59:50.000: Connected to Vesktop\n"
                                         "23:59:55.000: Notice shown (rule=DoubleAudio tier=2 subject=-)\n"
                                         "00:00:05.000: Disconnected from Vesktop: it stopped responding\n"
                                         "00:00:10.000: Notice cleared (rule=DoubleAudio tier=2 subject=-)\n")
            rec2 = read_record(bundle2)
            check("現在のバージョンの zip は時刻を持つ", rec2.timed and rec2.date_known)
            check("plugin.log の先頭の行は前の日", rec2.lines[0].at == dt.datetime(2000, 1, 1, 23, 59, 50), str(rec2.lines[0].at))
            check("日付が変わった後の行は作成した日", rec2.lines[-1].at == dt.datetime(2000, 1, 2, 0, 0, 10), str(rec2.lines[-1].at))
            out, code = count_notices([rec2], _args(normal=True))
            r = out["records"][0]
            check("現在のバージョンの zip の行を数える", r["shown_by_tier"]["2"] == 1 and r["timed"] is True)
            check("現在のバージョンの zip ではチャタリングを読める", not isinstance(r["chatter"], str), str(r["chatter"]))
            out, _ = measure_timing([read_record(bundle2)], _args(utc_offset="+09:00"))
            c = out["records"][0]["connection"]
            check("現在のバージョンの zip のハートビートの切断", c["drops_total"] == 1 and c["heartbeat_drops"] == 1, str(c))

            # 5. 時間の集計 (timing)。古いバージョンと現在のバージョンの stats の行、途中と終わりのフレーム 0 の周期、
            # 切断、画面共有の終わり。
            tlog = tmp / "2000-01-01 10-00-44.txt"
            tlog.write_text("\n".join([
                "10:00:48.000: [cordmux] hello: 合言葉が一致した。接続を確立した",
                "10:00:48.100: [cordmux] facts: roster=4 speaking=0 sharing=3 warnings=0 stages=- unreadable_keys=0",
                "10:01:00.000: [cordmux] stats stream=1 align period_us=16666.7 frames=100 relock=0 jump=0 wobble=0",
                "10:01:10.000: [cordmux] stats stream=1 align period_us=16666.7 frames=400 relock=0 jump=2 wobble=0",
                "10:01:20.000: [cordmux] stats stream=1 align period_us=16666.7 frames=400 realign=0 relock=0 jump=2 wobble=0",
                "10:01:30.000: [cordmux] stats stream=1 align period_us=16666.7 frames=520 realign=0 relock=0 jump=5 wobble=0",
                # ストリーム 2 は現在のバージョンの文字列 (先頭が大文字)。ストリーム 1 は古いバージョンの文字列。
                "10:01:00.000: [cordmux] Stats stream=2 align period_us=33333.3 frames=10 relock=0 jump=0",
                "10:01:10.000: [cordmux] Stats stream=2 align period_us=33333.3 frames=310 relock=0 jump=0",
                "10:01:20.000: [cordmux] Stats stream=2 align period_us=33333.3 frames=310 relock=0 jump=0",
                # 接続の行: 最初の接続は古いバージョンの文字列で、後は現在の文字列。ハートビートの切断の行の
                # 先頭が close の行と同じなので、行の全体で比べていないと、ハートビートの切断の数が 0 になる。
                "10:01:31.000: [cordmux] Disconnected from Vesktop",
                "10:01:36.500: [cordmux] Connected to Vesktop",
                "10:01:40.000: [cordmux] facts: roster=3 speaking=0 sharing=3 warnings=0 stages=3 unreadable_keys=0",
                "10:01:41.000: [cordmux] Facts: roster=3 speaking=0 sharing=2 warnings=0 stages=3 unreadable_keys=0",
                "10:01:43.000: [cordmux/js] 2000-01-01T01:01:40.200Z ws closed: ws=2 role=golive pcId=2",
                # 視聴の開始 (0.7 秒、screen entry から 0.6 秒)、再接続の screen entry (1.0 秒)、
                # 最初のフレームが来ない screen entry 1 つ。
                "10:01:43.000: [cordmux/js] 2000-01-01T01:01:41.000Z ws classified: ws=5 role=golive pcId=5 server_id=1 call_server_id=1 (op0)",
                "10:01:43.000: [cordmux/js] 2000-01-01T01:01:41.100Z screen entry: pcId=5 userId=1 streamId=6 tracks=video",
                "10:01:43.000: [cordmux/js] 2000-01-01T01:01:41.200Z screen entry: pcId=5 userId=1 streamId=6 tracks=video,audio",
                "10:01:43.000: [cordmux/js] 2000-01-01T01:01:41.700Z video stream 6: stage=1 format=1280x720@30",
                "10:01:43.000: [cordmux/js] 2000-01-01T01:01:42.000Z screen entry: pcId=9 userId=1 streamId=7 tracks=video,audio",
                "10:01:43.000: [cordmux/js] 2000-01-01T01:01:43.000Z video stream 7: stage=1 format=1280x720@30",
                "10:01:43.000: [cordmux/js] 2000-01-01T01:01:43.500Z video stream 7: stage=2 format=1280x720@30",
                "10:01:43.000: [cordmux/js] 2000-01-01T01:01:44.000Z screen entry: pcId=9 userId=1 streamId=8 tracks=video",
                "10:02:00.000: [cordmux] Disconnected from Vesktop: it stopped responding",
                "10:02:30.000: [cordmux] Module unloaded",
            ]) + "\n", encoding="utf-8")
            out, _ = measure_timing([read_record(tlog)], _args(utc_offset="+09:00"))
            r = out["records"][0]
            f = r["frames"]
            check("周期の数", f["windows"] == 5, str(f))
            check("途中のフレーム 0 の周期 (ストリーム 1 の 1 周期)", len(f["zero_windows_middle"]) == 1 and f["zero_windows_middle"][0]["stream"] == 1,
                  str(f["zero_windows_middle"]))
            check("終わりのフレーム 0 の周期 (ストリーム 2)", f["zero_windows_tail"] == 1)
            check("不連続の数", f["jumps_total"] == 5 and f["windows_with_jumps"] == 2, str(f))
            check("毎秒のフレームの最小 (0 を除く)", abs(f["fps_nonzero"]["min"] - 12.0) < 1e-6, str(f["fps_nonzero"]))
            c = r["connection"]
            check("切断 2 回 (うちハートビート 1)", c["drops_total"] == 2 and c["heartbeat_drops"] == 1, str(c))
            check("再接続まで 5.5 秒", abs(c["drops"][0]["back_after_s"] - 5.5) < 1e-6, str(c))
            check("ハートビートの切断は module unloaded で終わる", c["drops"][1]["ended_by"] == "module unloaded", str(c))
            s = r["share_end"]["renderer_to_obs_s"]
            check("画面共有の終わり 0.8 秒", len(s) == 1 and abs(s[0] - 0.8) < 1e-6, str(s))
            check("通話を抜けた回数", r["leave_call"]["roster_decreases"] == 1)
            ff = r["first_frame"]
            check("視聴の開始から最初のフレーム", len(ff["watch_start_s"]) == 1 and abs(ff["watch_start_s"][0] - 0.7) < 1e-6
                  and abs(ff["watch_entry_s"][0] - 0.6) < 1e-6, str(ff))
            check("再接続の screen entry", len(ff["reentry_s"]) == 1 and abs(ff["reentry_s"][0] - 1.0) < 1e-6, str(ff))
            check("最初のフレームが来ない screen entry", ff["no_stage_after_entry"] == 1, str(ff))
            clipped, _ = measure_timing([clip_record(read_record(tlog), "10:01:15", "10:01:35")], _args(utc_offset="+09:00"))
            check("時刻で切る", clipped["records"][0]["frames"]["windows"] == 1 and
                  clipped["records"][0]["connection"]["drops_total"] == 1, json.dumps(clipped, default=_json_default))
            check("表を書ける", "|" in markdown_timing(out) and "|" in markdown_notices(count_notices([rec], _args())[0]))
    except Exception as e:  # noqa: BLE001 (セルフテストは、どの例外でも失敗として理由を残す)
        failures.append("例外: %s: %s" % (type(e).__name__, e))

    if failures:
        _emit(json.dumps({"tool": "notice_count --selftest", "ok": False, "failures": failures}, ensure_ascii=False, indent=1))
        return 1
    _emit(json.dumps({"tool": "notice_count --selftest", "ok": True}, ensure_ascii=False))
    return 0


# ---- エントリポイント ---------------------------------------------------------------------------------------


def main(argv=None):
    if argv is None:
        argv = sys.argv[1:]
    if argv[:1] == ["--selftest"]:
        return selftest()
    parser = argparse.ArgumentParser(description="cordmux のログから、メッセージの表示と消去と、映像や接続にかかる時間を数える")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("notices", help="メッセージの表示と消去を数える")
    p.add_argument("records", nargs="+")
    p.add_argument("--date", type=_parse_date, help="OBS のログの日付 (ファイル名にないとき)")
    p.add_argument("--normal", action="store_true", help="正常な実行のログとして、メッセージのレベル 1 と 2 が 0 件であることと、チャタリングが 0 件であることを判定する")
    p.add_argument("--fault", action="append", help="<規則>@<時刻>/<秒>: 故障を起こした時刻と、メッセージが表示されるまでの上限")
    p.add_argument("--faults", help="故障の一覧の TSV (列: rule, at, within_s, note)")
    p.add_argument("--build-writes-notices", action="store_true",
                   help="ログを書いた cordmux はメッセージの行を書くバージョンである (行が 0 行でも 0 件として判定する)")
    p.add_argument("--chatter-count", type=int, default=3)
    p.add_argument("--chatter-window-s", type=float, default=60.0)
    p.add_argument("--chatter-event", choices=("shown", "both"), default="shown")
    p.add_argument("--markdown", action="store_true")

    t = sub.add_parser("timing", help="映像、接続、画面共有の終わり、最初のフレームにかかる時間を数える")
    t.add_argument("records", nargs="+")
    t.add_argument("--date", type=_parse_date)
    t.add_argument("--utc-offset", help="送信側の UTC の時刻を OBS のログの時刻に修正する差 (既定は、このツールを実行する PC の時間帯)")
    t.add_argument("--from", dest="time_from", help="この時刻からの行だけを数える (HH:MM:SS か YYYY-MM-DD HH:MM:SS)")
    t.add_argument("--to", dest="time_to", help="この時刻までの行だけを数える")
    t.add_argument("--markdown", action="store_true")

    args = parser.parse_args(argv)
    try:
        records = [read_record(r, args.date) for r in args.records]
        if getattr(args, "time_from", None) or getattr(args, "time_to", None):
            records = [clip_record(r, args.time_from, args.time_to) for r in records]
    except (OSError, ValueError, zipfile.BadZipFile) as e:
        _emit(json.dumps({"ok": False, "error": str(e)}, ensure_ascii=False))
        return 2
    try:
        if args.command == "notices":
            out, code = count_notices(records, args)
            text = markdown_notices(out) if args.markdown else json.dumps(out, ensure_ascii=False, indent=1, default=_json_default)
        else:
            out, code = measure_timing(records, args)
            text = markdown_timing(out) if args.markdown else json.dumps(out, ensure_ascii=False, indent=1, default=_json_default)
    except ValueError as e:
        _emit(json.dumps({"ok": False, "error": str(e)}, ensure_ascii=False))
        return 2
    _emit(text)
    return code


if __name__ == "__main__":
    sys.exit(main())
