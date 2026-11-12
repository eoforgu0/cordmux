#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# 実機の結合テストの結果 JSON (run-window.mjs が書く result-integration-*.json) と OBS のログから、
# 数え方の要約 (定常 = 開始 15 秒より後、白のマーカーで番号が読めないフレームは
# 除く) と、ストリームごとの整列と A/V のずらし量 (プラグインの stats の行) を 1 画面に出す。
# 実行中に判定スクリプト (frames.py / sync.py / smooth.py) の生の出力を読み直す手間をなくすためのツール。
#
# 使い方:
#   python summarize-window.py <result-integration-*.json> [<OBS のログ .txt>] [--fps 60]
#   python summarize-window.py --selftest
#
# 出力の読み方 (1 行 1 領域または 1 ストリーム):
#   dock-view scenario=...: --step dock-view の結果 JSON だけに出す先頭の 1 行 (args.dockScenario
#                 と args.tag。2 つの結果 JSON を人が並べて
#                 見比べるとき、どちらがどの scenario かを本文を開かずに区別するため)
#   frames cellN: steady_missing = 定常の欠落フレーム数 (割合)、events = 欠落の起きた回数、
#                 dup_total = 重複、max_freeze_ms = 最長の凍り、backwards = 番号の逆行。
#                 judge の手順が label を持てば末尾に label=... を添える (ac24_connected_baseline / ac24_degraded のような
#                 場合を見分けるため)。新しい frames.py の結果なら
#                 layout (clock / legacy) と unreadable (読めないフレーム) と missing_across_unreadable も添え、
#                 帯が領域の中に見つからなかった領域は数えずに "BARCODE NOT FOUND (理由)" と出す
#   sync cellN:   white / tone = マーカーの検出数、paired = 対になった数、audio-video ms = 音 - 映像
#   smooth cellN: 20 秒から 180 秒の欠落 / 重複 / 凍りの合計と、最も悪かった 10 秒の開始秒。新しい smooth.py の
#                 結果なら layout と unreadable (clock の読めないフレーム) も添え、帯が領域の中に見つからなかった領域は
#                 数えずに "BARCODE NOT FOUND (理由)" と出す
#   stream K:     プラグインの stats stream=K の最後の行 (整列の不連続 / 戻り、A/V のずらし量)
#   screenshots:  --step dock-view の撮影。screenshot-window /
#                 screenshot-source の各手順が実際に撮った枚数の合計 (失敗した手順は 0 枚として
#                 数える。手順そのものが結果 JSON になければこの行は出さない)
#   stages=... configure=...: --dock-scenario degraded の確認。run-window.mjs が結果 JSON へコピーした "encode-facts" (harness-wait の
#                 直後、copyEncodeFacts を持つ手順だけが足す) から、ストリームごとの段 (facts.stages) と
#                 いま実際にエンコードしているサイズ (streams[].encodedFormat の WxH。声のストリームや、段が
#                 まだ決まっていない/Unavailable のストリームは none) を並べる。参加者タブの 2 枚目の
#                 PNG (画質の欄・解像度) と並べて、送った値 (HARNESS_MAX_LONG_EDGE / failStage) が
#                 実際に反映されているかを見る。"encode-facts" が結果 JSON に無ければこの行は出さない
#   HUMAN:        人の操作待ちの手順 (run-window.mjs の "human")。依頼した文と、何で進んだか
#                 (satisfiedBy: go-file / state-json / seconds。進まなかったら timeout) と待った秒数。結果 JSON の
#                 steps の並びのまま出す (どの操作をどの順で依頼したかを本文を開かずに読めるようにするため)
#   NOTE:         プラグインのログのうち warning / error / dropped を含む行 (先頭 10 件)
import io
import json
import re
import sys
import tempfile
import os

MISSING_SENTINEL = 2 ** 31  # frames.py が「番号が読めないフレーム」に入れる番付の値


def main(argv):
    if "--selftest" in argv:
        return selftest()
    args = [a for a in argv[1:] if not a.startswith("--")]
    fps = 60
    if "--fps" in argv:
        fps = int(argv[argv.index("--fps") + 1])
    if not args:
        print("usage: summarize-window.py <result-integration-*.json> [<obs-log.txt>] [--fps 60]")
        return 2
    result = json.load(io.open(args[0], encoding="utf-8"))
    steady_from = 15 * fps
    step_args = result.get("args") or {}
    if step_args.get("step") == "dock-view":
        # 2 つの結果 JSON (connected と degraded 等) を人が並べて見るとき、
        # どちらの scenario かを本文を開かずに区別できるようにする。
        print(f"dock-view scenario={step_args.get('dockScenario')} tag={result.get('tag')}")
    shot_kinds = ("screenshot-window", "screenshot-source")
    shot_counts = {k: 0 for k in shot_kinds}
    saw_shot_step = False
    for step in result.get("steps", []):
        name = step.get("name")
        if name in shot_kinds:
            saw_shot_step = True
            shot_counts[name] += len(step.get("shots") or [])
        if name == "encode-facts":
            # run-window.mjs が harness-wait の直後にコピーした configure のサイズと
            # facts.stages (フィールド名は encodeFactsFromHarnessData と同じ)。
            print(encode_facts_line(step))
        if name == "human":
            print(human_line(step))
        if name != "judge" or not step.get("ok"):
            continue
        tool = step.get("tool")
        label_suffix = f" label={step['label']}" if step.get("label") else ""
        if tool == "frames.py":
            for cell in per_cell(step):
                d = cell["data"]
                if not d.get("barcode_found", True):
                    # frames.py は帯を領域の中に見つけられなければ数えずにそう言う (bit_strip.py)。
                    # 暗い所を読んだ 0 を番号として数えると、全部のフレームが重複 (凍り) に見える。
                    print(f"frames cell{cell['cell']}: BARCODE NOT FOUND ({d.get('error')}){label_suffix}")
                    continue
                real = [g for g in d["gaps"] if g["missing"] < MISSING_SENTINEL]
                steady = [g for g in real if g["at"] >= steady_from]
                steady_frames = max(1, d["frames"] - steady_from)
                miss = sum(g["missing"] for g in steady)
                first_gap = real[0]["at"] if real else None
                # layout / unreadable は新しい frames.py だけが出す (古い結果 JSON にはないので出さない)。
                extra = ""
                if "layout" in d:
                    extra = (f" layout={d['layout']} unreadable={d.get('unreadable_frames')}"
                             f" missing_across_unreadable={d.get('missing_across_unreadable', 0)}")
                print(
                    f"frames cell{cell['cell']}: steady_missing={miss} ({100.0 * miss / steady_frames:.3f}%) "
                    f"events={len(steady)} dup_total={d['duplicate_count']} max_freeze_ms={d['max_freeze_ms']} "
                    f"backwards={d['backwards_count']} first_gap_at={first_gap}{extra}{label_suffix}"
                )
        elif tool == "sync.py":
            for cell in per_cell(step):
                d = cell["data"]
                m = d["audio_minus_video_ms"]
                print(
                    f"sync cell{cell['cell']}: white={d['white_events']} tone={d['tone_events']} paired={d['paired']} "
                    f"audio-video ms mean={m['mean']} sd={m['stdev']} min={m['min']} max={m['max']}"
                )
        elif tool == "smooth.py":
            for cell in step["data"]["cells"]:
                if not cell.get("barcode_found", True):
                    # smooth.py も帯を領域の中で探し、見つからない領域は数を出さない (frames.py と同じ)。
                    print(f"smooth cell{cell['cell']}: BARCODE NOT FOUND ({cell.get('error')})")
                    continue
                rows = [x for x in cell["per10s"] if 20 <= x[0] < 180]
                sums = [sum(v[1][k] for v in rows) for k in range(3)]
                worst = max(rows, key=lambda v: v[1][0] + v[1][1])[0] if rows else None
                # layout / unreadable は新しい smooth.py だけが出す (古い結果 JSON にはないので出さない)。
                extra = ""
                if "layout" in cell:
                    extra = f" layout={cell['layout']}"
                    if "unreadable_frames" in cell:
                        extra += f" unreadable={cell['unreadable_frames']}"
                print(
                    f"smooth cell{cell['cell']}: 20-180s missing={sums[0]} dup={sums[1]} freeze={sums[2]} worst10s={worst}{extra}"
                )
    if saw_shot_step:
        total = sum(shot_counts.values())
        print(f"screenshots: window={shot_counts['screenshot-window']} source={shot_counts['screenshot-source']} total={total}")
    if len(args) > 1:
        summarize_log(args[1])
    return 0


def per_cell(step):
    return step.get("perCell") or [{"cell": 0, "data": step["data"]}]


def encode_facts_line(step):
    # step は "encode-facts" (run-window-plan.mjs の encodeFactsFromHarnessData の戻り値を
    # そのまま結果 JSON へコピーしたもの)。configure は画面のストリームだけ (声は含まない)。
    # 段がまだ決まっていない/Unavailable のストリームは width/height が None のまま = none として出す
    # (暫定値で埋めない)。
    stages = step.get("stages") or []
    stages_text = ",".join(f"{e['stream_id']}:{e['stage']}" for e in stages) if stages else "none"
    configure = step.get("configure") or []
    conf_texts = [f"{c['width']}x{c['height']}" if c.get("width") is not None and c.get("height") is not None else "none" for c in configure]
    configure_text = ",".join(conf_texts) if conf_texts else "none"
    return f"stages={stages_text} configure={configure_text}"


def human_line(step):
    # step は "human" (run-window.mjs の record("human", ...))。依頼した文と、何で進んだかと待った秒数。
    # ok でなければ timeout (時限か期限で進めなかった)。
    how = step.get("satisfiedBy") or "timeout"
    waited_s = int(round((step.get("waitedMs") or 0) / 1000.0))
    return f"HUMAN: {step.get('message')} -> {how} ({waited_s}s)"


def summarize_log(path):
    lines = io.open(path, encoding="utf-8", errors="replace").read().splitlines()
    per_stream = {}
    for line in lines:
        # 行の先頭は大文字 (Stats) と小文字 (stats) の両方を読む。
        m = re.search(r"[Ss]tats stream=(\d+) (.*)$", line)
        if m:
            per_stream[m.group(1)] = m.group(2)
    for key in sorted(per_stream, key=int):
        print("stream", key, per_stream[key])
    last_video = [l for l in lines if "stats video" in l.lower()]
    last_audio = [l for l in lines if "stats audio" in l.lower()]
    if last_video:
        print(last_video[-1].split("[cordmux] ")[1])
    if last_audio:
        print(last_audio[-1].split("[cordmux] ")[1])
    buffering = [l for l in lines if "audio buffering" in l]
    for l in buffering[:5]:
        print("OBS:", l[:200])
    notes = [
        l for l in lines
        if "[cordmux]" in l and any(w in l.lower() for w in ("warning", "error", "dropped=", "nullptr"))
        and "stats" not in l
    ]
    for l in notes[:10]:
        print("NOTE:", l[:200])


def selftest():
    # test/lint/*.py の --selftest と同じ形 (一時フォルダーに架空のテストデータを作り、
    # 実際の実行結果には一切触れない)。summarize-window.py 自体は test/lint/ の
    # 対象外だが、screenshots 行と dock-view scenario 行 (この 2 つは実機で実行しないと
    # 人の目でしか確認できない) を、実機なしでチェックできるようにする。
    import contextlib

    with tempfile.TemporaryDirectory(prefix="summarize-window-selftest-") as tmp:
        result_path = os.path.join(tmp, "result-dock-view-fake.json")
        fake = {
            "tag": "dock-view-selftest",
            "args": {"step": "dock-view", "dockScenario": "degraded"},
            "steps": [
                {"name": "screenshot-window", "ok": True, "shots": [{"path": "a.png"}, {"path": "b.png"}]},
                {"name": "screenshot-window", "ok": False, "error": "boom"},
                {"name": "screenshot-source", "ok": True, "shots": [{"path": "c.png"}]},
                {
                    "name": "judge",
                    "ok": True,
                    "tool": "frames.py",
                    "label": "ac24_degraded",
                    "data": {"frames": 600, "gaps": [], "duplicate_count": 0, "max_freeze_ms": 0, "backwards_count": 0},
                },
                {
                    # 新しい frames.py: 領域ごと (perCell) に、帯が見つかった領域と見つからない領域。
                    "name": "judge",
                    "ok": True,
                    "tool": "frames.py",
                    "cells": 2,
                    "perCell": [
                        {"cell": 0, "data": {"frames": 600, "gaps": [], "duplicate_count": 0, "max_freeze_ms": 0, "backwards_count": 0,
                                             "barcode_found": True, "layout": "clock", "unreadable_frames": 3,
                                             "missing_across_unreadable": 2}},
                        {"cell": 1, "data": {"frames": 600, "barcode_found": False, "layout": None,
                                             "error": "フレーム番号の帯が領域の中に見つからない"}},
                    ],
                },
                {
                    # 新しい smooth.py: 帯が見つかった領域 (clock) と見つからない領域。
                    "name": "judge",
                    "ok": True,
                    "tool": "smooth.py",
                    "data": {"cells": [
                        {"cell": 0, "layout": "clock", "barcode_found": True, "unreadable_frames": 4,
                         "per10s": [[20, [1, 2, 0]], [30, [0, 0, 0]]]},
                        {"cell": 1, "layout": None, "barcode_found": False,
                         "error": "フレーム番号の帯が領域の中に見つからない (smooth)"},
                    ]},
                },
                {
                    # 人の操作待ち (run-window.mjs の record("human", ...))。
                    "name": "human", "ok": True, "message": "テストの通知", "until": "go-file", "satisfiedBy": "go-file", "waitedMs": 12400,
                },
                {"name": "human", "ok": False, "message": "進まなかった通知", "until": "state-json-restored", "satisfiedBy": None, "waitedMs": 300000},
                {
                    # encodeFactsFromHarnessData (run-window-plan.mjs) の戻り値を
                    # そのままコピーした形 (run-window.mjs の record("encode-facts", ...))。
                    "name": "encode-facts",
                    "hasData": True,
                    "requested": {"maxLongEdge": 640, "failStage": 3},
                    "configure": [{"streamId": 1, "width": 360, "height": 640, "fps": 30}],
                    "stages": [{"stream_id": 1, "stage": "software"}],
                },
            ],
        }
        with io.open(result_path, "w", encoding="utf-8") as f:
            json.dump(fake, f)

        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = main(["summarize-window.py", result_path])
        out = buf.getvalue()

        checks = [
            ("exit code 0", rc == 0),
            ("dock-view scenario 行", "dock-view scenario=degraded tag=dock-view-selftest" in out),
            ("screenshots 行 (window=2 source=1 total=3。失敗した1枚は0枚として数える)", "screenshots: window=2 source=1 total=3" in out),
            ("frames の label 付与", "label=ac24_degraded" in out),
            ("frames の帯の種類と読めないフレーム (新しい形)",
             "frames cell0: steady_missing=0 (0.000%) events=0 dup_total=0 max_freeze_ms=0 backwards=0 first_gap_at=None "
             "layout=clock unreadable=3 missing_across_unreadable=2" in out),
            ("frames の帯が見つからない領域は数えずにそう出す",
             "frames cell1: BARCODE NOT FOUND (フレーム番号の帯が領域の中に見つからない)" in out),
            ("smooth の帯の種類と読めないフレーム (新しい形)",
             "smooth cell0: 20-180s missing=1 dup=2 freeze=0 worst10s=20 layout=clock unreadable=4" in out),
            ("smooth の帯が見つからない領域は数えずにそう出す",
             "smooth cell1: BARCODE NOT FOUND (フレーム番号の帯が領域の中に見つからない (smooth))" in out),
            ("古い形の frames の結果 (layout がない) には layout を出さない",
             "label=ac24_degraded" in out and "layout=" not in out.split("label=ac24_degraded")[0].splitlines()[-1]),
            ("stages=... configure=... 行", "stages=1:software configure=360x640" in out),
            ("HUMAN 行 (進んだ通知)", "HUMAN: テストの通知 -> go-file (12s)" in out),
            ("HUMAN 行 (進まなかった通知は timeout)", "HUMAN: 進まなかった通知 -> timeout (300s)" in out),
            ("HUMAN 行は steps の並びのまま", out.index("HUMAN: テストの通知") < out.index("HUMAN: 進まなかった通知")),
        ]

        # 段がまだ決まっていない/Unavailable (width/height が None) や
        # factsSent がなかった (stages が空) 場合は "none" と出す (暫定値で埋めない)。
        none_line = encode_facts_line({"configure": [{"streamId": 1, "width": None, "height": None, "fps": None}], "stages": []})
        checks.append(('configure/stages が空なら "none" (段が決まっていない等)', none_line == "stages=none configure=none"))

        # "encode-facts" 自体が結果 JSON にない scenario (connected/no-scene 等) では
        # stages=.../configure=... の行を出さない (harness-wait に copyEncodeFacts がない)。
        no_encode_facts = dict(fake, steps=[s for s in fake["steps"] if s.get("name") != "encode-facts"])
        no_encode_facts_path = os.path.join(tmp, "result-dock-view-no-encode-facts.json")
        with io.open(no_encode_facts_path, "w", encoding="utf-8") as f:
            json.dump(no_encode_facts, f)
        buf2 = io.StringIO()
        with contextlib.redirect_stdout(buf2):
            main(["summarize-window.py", no_encode_facts_path])
        checks.append(('"encode-facts" が無ければ stages=.../configure=... 行を出さない', "stages=" not in buf2.getvalue()))

        failed = [name for name, ok in checks if not ok]
        if failed:
            print("selftest NG:", ", ".join(failed))
            print("--- 実際の出力 ---")
            print(out)
            return 1

    print("selftest OK: summarize-window")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
