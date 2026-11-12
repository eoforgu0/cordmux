# SPDX-License-Identifier: Apache-2.0

# 設定の値ごとに、変えられる場所を製品のコードから集めて数える (製品のファイルを正規表現で読むだけで、ビルドしない)。
#   (a) 設定ウィンドウ ([OK]、[キャンセル]、[適用] で反映する) と、すぐ反映される場所の両方で変えられる値
#   (b) 製品も書き換える値のうち、設定ウィンドウで変えられるもの
#   (c) すぐ反映される場所に表示されるのに、押しても何もしない操作 (run_action の switch に case がないもの)
# どれも 0 なら 0、ほかは 1 で終了する。(a) か (b) が 0 でないと、ほかの場所で変わった値を、設定ウィンドウの [OK] が
# ウィンドウを開いたときの古い値で上書きすることがある。
#   python settings_places_count.py [--src <src-obs のフォルダー>] [--out <出力先>]
#   (--out で settings-places-count.md と settings-places-count.tsv を書く)

import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SRC = os.path.normpath(os.path.join(HERE, "..", ".."))

# 集め方。どれも製品のファイルを正規表現で読み、見つけた行を表の列に書く。
#   - 設定ウィンドウの値: src-obs/dock/settings_fields.cpp の表で owner が SettingsWindow のキー (settings.json の
#     キー) と、src-obs/dock/settings_window.cpp の register_field のキー (設定ウィンドウの項目。settings.json にない
#     dummy_on を含む)。2 つの集合の違いも出力する。
#   - すぐ反映される場所 (ドック、ドックのメニュー、メッセージのボタン、行のメニュー、参加者の設定ウィンドウ) の操作:
#     src-obs/ledger/rules_dock.cpp と、src-obs/dock の UI のファイル (dock_poll で始まるファイルと
#     settings_window.cpp を除く) に出てくる ActionId。
#   - 操作から処理へ: src-obs/ledger/actions.cpp の表 ({ActionId::X, IntentKind::Y, ...})。
#   - 処理から値へ: src-obs/dock/dock_poll.cpp の run_action の switch の case。case の中の「settings_.キー =」
#     (settings.json)、参加者ごとのフィールドへの代入 (seen_people.json)、CALL_VALUES の関数の呼び出しを読み、case から
#     呼ぶ DockPoll と DockPollSettingsPart の関数の中身も 1 段だけ読む。
#   - 製品が書き換える値: src-obs/dock の「settings_.キー =」のうち、run_action の case と、設定ウィンドウからコピー
#     する処理 (apply_settings_page_fields) の外にあるもの。
#
# 値を変える関数の呼び出しのパターンと、その値の名前 (settings.json と seen_people.json にない値。値を持つ場所ごと)。
CALL_VALUES = [
    (r"pipeline_\.set_recording\(", "cordmuxの録画の ON と OFF (Pipeline のメモリ)"),
    (r"pipeline_\.(assign_slot|release_slot)\(", "枠に載せるか (割り当て表)"),
    (r"sources::set_dummy_mode\(", "ダミーの表示 (配信の画面)"),
    (r"write_vencord_dir\(", "Vesktop の state.json の vencordDir (cordmux の外のファイル)"),
    (r"reinstall_vencord_files\(", "同梱の 4 つのファイル (cordmux の外のファイル)"),
    (r"uninstall_now\(|DockPollInstallPart::uninstall\(", "アンインストール (操作)"),
    (r"resume_finalize\(", "録画ファイルの作成の再開 (操作)"),
    (r"open_settings\(", "(ウィンドウを開くだけ)"),
    (r"open_participants\(", "(ウィンドウを開くだけ)"),
]
# case から呼んでも中身を読まない関数 (ウィンドウを開くだけの関数。ウィンドウの中で変わる値は、ウィンドウの側で数える)。
NOT_FOLLOWED = {"open_settings", "open_participants", "begin"}
# seen_people.json の参加者ごとのフィールド。
SEEN_FIELDS = ["always_on", "name_override", "voice_source_user_id", "roster_hidden"]


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def body_after(text, pos):
    """pos の後の最初の { から、対応する } までの文字列。中括弧の数だけを数える (文字列リテラルの中の括弧も
    区別しない)。"""
    start = text.index("{", pos)
    depth = 0
    i = start
    while i < len(text):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
        i += 1
    return text[start:]


def settings_fields(src):
    path = os.path.join(src, "dock", "settings_fields.cpp")
    text = read(path)
    rows = {}
    for m in re.finditer(r'\{"(\w+)", SettingsOwner::(\w+),.*?, Page::(\w+), (true|false)\}', text):
        rows[m.group(1)] = {"owner": m.group(2), "page": m.group(3), "restart": m.group(4) == "true",
                            "where": "dock/settings_fields.cpp:%d" % line_of(text, m.start())}
    return rows


def window_field_keys(src):
    path = os.path.join(src, "dock", "settings_window.cpp")
    text = read(path)
    keys = {}
    for m in re.finditer(r'register_field\(\s*[^,]+,\s*"([^"]+)"', text):
        for k in re.split(r"[,+]", m.group(1)):
            keys[k.strip()] = "dock/settings_window.cpp:%d" % line_of(text, m.start())
    return keys


def action_intents(src):
    text = read(os.path.join(src, "ledger", "actions.cpp"))
    return {m.group(1): m.group(2) for m in re.finditer(r"\{ActionId::(\w+), IntentKind::(\w+)", text)}


def immediate_actions(src):
    """すぐ反映される場所に出てくる ActionId から、見つけた行の一覧への辞書。"""
    files = [os.path.join("ledger", "rules_dock.cpp")]
    for name in sorted(os.listdir(os.path.join(src, "dock"))):
        if name.endswith(".cpp") and not name.startswith("dock_poll") and name not in ("settings_window.cpp",):
            files.append(os.path.join("dock", name))
    out = {}
    for rel in files:
        text = read(os.path.join(src, rel))
        for m in re.finditer(r"ActionId::(\w+)", text):
            out.setdefault(m.group(1), []).append("%s:%d" % (rel.replace(os.sep, "/"), line_of(text, m.start())))
    return out


def dock_function_bodies(src):
    """dock_poll で始まるファイルにある DockPoll* のメンバー関数の定義の中身 (名前から (中身, 見つけた行) への辞書)。"""
    out = {}
    for name in sorted(os.listdir(os.path.join(src, "dock"))):
        if not (name.startswith("dock_poll") and name.endswith(".cpp")):
            continue
        text = read(os.path.join(src, "dock", name))
        for m in re.finditer(r"^\w[\w:<>&*, ]*?\b(DockPoll\w*)::(\w+)\([^;{]*\)\s*(const\s*)?\{", text, re.M):
            out[m.group(2)] = (body_after(text, m.start()), "dock/%s:%d" % (name, line_of(text, m.start())))
    return out


def values_in(block):
    vals = set()
    for m in re.finditer(r"settings_\.(\w+)\s*=(?!=)", block):
        vals.add("settings.json " + m.group(1))
    for f in SEEN_FIELDS:
        if re.search(r"\b%s\b\s*=(?!=)|\b%s\.reset\(\)" % (f, f), block):
            vals.add("seen_people.json " + f)
    for pat, name in CALL_VALUES:
        if re.search(pat, block):
            vals.add(name)
    return vals


def intent_values(src):
    """IntentKind から (値の集合, 見つけた行) への辞書。run_action の switch に case のない IntentKind は入らない。"""
    path = os.path.join(src, "dock", "dock_poll.cpp")
    text = read(path)
    m = re.search(r"void DockPoll::run_action\(", text)
    run = body_after(text, m.start())
    sw = run[run.index("switch (intent.kind)"):]
    sw = body_after(sw, 0)
    bodies = dock_function_bodies(src)
    out = {}
    parts = re.split(r"case ledger::IntentKind::(\w+):", sw)
    # parts は [前, 名前1, 中身1, 名前2, 中身2, ...]。続けて書いた case (中身が空) は、次の case と同じ処理。
    pending = []
    for i in range(1, len(parts), 2):
        kind, block = parts[i], parts[i + 1]
        block = block.split("default:")[0]
        pending.append(kind)
        if not block.strip():
            continue
        vals = values_in(block)
        for call in re.findall(r"(?:DockPollSettingsPart::|DockPollInstallPart::|\b)(\w+)\(", block):
            # ウィンドウを開く関数の中身 (閉じたときの大きさの保存、ウィンドウのボタンの処理) は、ウィンドウの側の
            # 値なので読まない。
            if call in bodies and call not in NOT_FOLLOWED:
                vals |= values_in(bodies[call][0])
        where = "dock/dock_poll.cpp:%d" % line_of(text, text.index("case ledger::IntentKind::%s:" % kind))
        for k in pending:
            out[k] = (vals, where)
        pending = []
    return out


def product_writes(src):
    """製品が書き換える settings.json のキー (run_action の case と、設定ウィンドウからコピーする処理の外にある
    「settings_.キー =」)。"""
    out = {}
    text_poll = read(os.path.join(src, "dock", "dock_poll.cpp"))
    m = re.search(r"void DockPoll::run_action\(", text_poll)
    run_span = (m.start(), m.start() + len(body_after(text_poll, m.start())))
    action_bodies = set()
    bodies = dock_function_bodies(src)
    for name in ("set_feature", "set_recording", "set_dummy_mode", "set_slot"):
        if name in bodies:
            action_bodies.add(bodies[name][1])
    for name in sorted(os.listdir(os.path.join(src, "dock"))):
        if not name.endswith(".cpp"):
            continue
        text = read(os.path.join(src, "dock", name))
        for w in re.finditer(r"settings_\.(\w+)\s*=(?!=)", text):
            if name == "dock_poll.cpp" and run_span[0] <= w.start() < run_span[1]:
                continue
            # case から呼ぶ関数 (利用者の操作の処理) の中は除く。
            inside_action = False
            for b, where in ((bodies[n][0], bodies[n][1]) for n in bodies):
                if where.startswith("dock/%s:" % name) and n_contains(text, b, w.start()):
                    if where in action_bodies:
                        inside_action = True
            if inside_action:
                continue
            out.setdefault(w.group(1), []).append("dock/%s:%d" % (name, line_of(text, w.start())))
    return out


def n_contains(text, body, pos):
    start = text.find(body)
    return start >= 0 and start <= pos < start + len(body)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=DEFAULT_SRC)
    ap.add_argument("--out")
    a = ap.parse_args()
    src = a.src

    fields = settings_fields(src)
    window_keys = window_field_keys(src)
    intents = action_intents(src)
    immediate = immediate_actions(src)
    handled = intent_values(src)
    writes = product_writes(src)

    ok_values = {}
    for key, row in fields.items():
        if row["owner"] == "SettingsWindow":
            ok_values["settings.json " + key] = "設定ウィンドウ %s (%s)" % (row["page"], row["where"])
    for key, where in window_keys.items():
        if key not in fields:
            # dummy_on の名前は、CALL_VALUES のダミーの名前と同じにする ((a) は名前の一致で数える)。
            name = "ダミーの表示 (配信の画面)" if key == "dummy_on" else "設定ウィンドウだけの値 " + key
            ok_values[name] = "設定ウィンドウ (%s)" % where

    imm_values = {}
    unwired = []
    for action, wheres in sorted(immediate.items()):
        kind = intents.get(action)
        if kind is None:
            continue
        if kind not in handled:
            unwired.append((action, kind, wheres[0]))
            continue
        for v in handled[kind][0]:
            imm_values.setdefault(v, set()).add(action)

    both = sorted(set(ok_values) & set(imm_values))
    product_in_ok = sorted(k for k in writes if "settings.json " + k in ok_values)
    owner_dock_in_ok = sorted(k for k, r in fields.items() if r["owner"] == "Dock" and "settings.json " + k in ok_values)
    table_only = sorted(k for k, r in fields.items() if r["owner"] == "SettingsWindow" and k not in window_keys)
    window_only = sorted(k for k in window_keys if k not in fields and k != "dummy_on")

    lines = ["# 値ごとの変えられる場所 (製品のコードから集めた表)", ""]
    lines.append("| 値 | すぐ反映される場所 (操作) | 設定ウィンドウ | 製品が書き換える場所 |")
    lines.append("|---|---|---|---|")
    tsv = ["value\timmediate_actions\tok_window\tproduct_writes"]
    for v in sorted(set(ok_values) | set(imm_values) | {"settings.json " + k for k in writes}):
        imm = ", ".join(sorted(imm_values.get(v, [])))
        ok = ok_values.get(v, "")
        pw = ", ".join(writes.get(v[len("settings.json "):], [])) if v.startswith("settings.json ") else ""
        lines.append("| %s | %s | %s | %s |" % (v, imm or "-", ok or "-", pw or "-"))
        tsv.append("%s\t%s\t%s\t%s" % (v, imm, ok, pw))
    lines.append("")
    lines.append("## 集計")
    lines.append("")
    lines.append("- (a) 設定ウィンドウと、すぐ反映される場所の両方で変えられる値: **%d** %s" %
                 (len(both), ", ".join(both)))
    lines.append("- (b) 製品も書き換える値のうち、設定ウィンドウで変えられるもの: **%d** %s" %
                 (len(product_in_ok) + len(owner_dock_in_ok), ", ".join(product_in_ok + owner_dock_in_ok)))
    lines.append("- (c) すぐ反映される場所に表示されるのに、押しても何もしない操作: **%d** %s" %
                 (len(unwired), ", ".join("%s (IntentKind::%s、%s)" % u for u in unwired)))
    lines.append("- 参考: 設定ウィンドウにない SettingsWindow のキー %d %s、表にない設定ウィンドウの項目 %d %s" %
                 (len(table_only), ", ".join(table_only), len(window_only), ", ".join(window_only)))
    lines.append("- 参考: 設定ウィンドウの値 %d、すぐ反映される場所の値 %d、すぐ反映される場所に表示される操作 %d" %
                 (len(ok_values), len(imm_values), len(immediate)))
    report = "\n".join(lines) + "\n"
    sys.stdout.write(report)
    if a.out:
        os.makedirs(a.out, exist_ok=True)
        with open(os.path.join(a.out, "settings-places-count.md"), "w", encoding="utf-8", newline="\n") as f:
            f.write(report)
        with open(os.path.join(a.out, "settings-places-count.tsv"), "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(tsv) + "\n")
    return 0 if not (both or product_in_ok or owner_dock_in_ok or unwired) else 1


if __name__ == "__main__":
    sys.exit(main())
