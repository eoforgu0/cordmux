#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# ja の文字列の書き方を、A (半角の英数字と全角の文字の間に空白を入れ、疑問符は半角の ?) と B (空白を入れず、
# 疑問符は全角の ？) の間で書き換えるツール。書き換えるのは src-obs/data/locale/ja-JP.ini の値、guide/ja.md、
# test/tools/dist-zip/release_notes_template.md の「## 日本語」の節、CHANGELOG.md の「### ja」の節で、最後に
# test/lint/ja_style.txt の spacing と question の行を書き換える (lint が同じ書き方の規則を使うように)。
#
#   python apply_ja_style.py --spacing B --question fullwidth   書き換える (現在の書き方は ja_style.txt から読む)
#   python apply_ja_style.py --spacing B --dry-run              書き換えずに、変わるファイルと行の数を書く
#   python apply_ja_style.py --selftest                         変換の例と、A -> B -> A で元に戻ることを確認する

import argparse
import fnmatch
import re
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent.parent
LINT_DIR = REPO_ROOT / "test" / "lint"
STYLE_FILE = LINT_DIR / "ja_style.txt"
JA_INI = REPO_ROOT / "src-obs" / "data" / "locale" / "ja-JP.ini"
GUIDE = REPO_ROOT / "guide" / "ja.md"
TEMPLATE = REPO_ROOT / "test" / "tools" / "dist-zip" / "release_notes_template.md"
CHANGELOG = REPO_ROOT / "CHANGELOG.md"

if str(LINT_DIR) not in sys.path:
    sys.path.insert(0, str(LINT_DIR))
import forbidden_ui_words  # noqa: E402  (ja_style.txt を読む処理を 1 か所にまとめる)

JP = "ぁ-ヿ一-鿿"          # ひらがな、カタカナ、漢字の範囲
AN = "A-Za-z0-9%"
# 数と半角の単位の間の空白 (「500 ms」) は、両側が ASCII なので一致しない。
SPACE_RES = [
    re.compile(f"(?<=[{JP}]) (?=[{AN}])"),
    re.compile(f"(?<=[{AN}]) (?=[{JP}])"),
]
INSERT_RES = [
    re.compile(f"(?<=[{JP}])(?=[{AN}])"),
    re.compile(f"(?<=[{AN}])(?=[{JP}])"),
]
# OBS の中の名前 (ja_style.txt の name_keys) の末尾の番号。空白の後ろの数か %N で、その後ろが値の終わりか ")" か
# " (" のもの。B でも、この番号の前の空白は残す (「ダミー 1」)。
NAME_NUMBER_RE = re.compile(r"(?:[0-9]+|%[0-9]+)(?=$|\)| \()")
# 後ろが英数字か URL の記号でない「?」だけを書き換える。URL の ? は書き換えない。
HALF_Q_RE = re.compile(r"\?(?![A-Za-z0-9_=&%/.#-])")
FULL_Q = "？"
# 書き換えない所: URL、`...` のコード、{{...}} のテンプレートの変数、手順書 (Markdown) のリンク先の (#見出しのアンカー) と
# (../相対パス)。アンカーに空白を入れると、見出しに届かなくなるため (見出しの書き方が変わったときのアンカーの食い違いは、
# 手順書の lint が見つける)。
PROTECT_RE = re.compile(r"https?://\S+|`[^`\n]*`|\{\{[^{}\n]*\}\}|\((?:#|\.\.?/)[^)\s]*\)")
# [ ] と「」の中 (1 行の中で閉じるもの)
SPAN_RE = re.compile(r"\[([^\[\]\n]+)\]|「([^「」\n]+)」")
ARG_RE = re.compile(r"%[0-9]+")
ELLIPSIS = "..."


def matches_any(key, patterns):
    return any(fnmatch.fnmatchcase(key, p) for p in patterns)


def convert_spacing(text, to, keep_number_space=False):
    if to == "B":
        for pattern in SPACE_RES:
            if keep_number_space:
                text = pattern.sub(lambda m: m.group(0) if NAME_NUMBER_RE.match(text, m.end()) else "", text)
            else:
                text = pattern.sub("", text)
        return text
    for pattern in INSERT_RES:
        text = pattern.sub(" ", text)
    return text


def convert_question(text, to):
    if to == "fullwidth":
        return HALF_Q_RE.sub(FULL_Q, text)
    return text.replace(FULL_Q, "?")


def convert_plain(text, dst, src, keep_number_space=False):
    # PROTECT_RE の所 (URL など) の外だけを書き換える
    out = []
    pos = 0
    for m in PROTECT_RE.finditer(text):
        out.append(convert_piece(text[pos:m.start()], dst, src, keep_number_space))
        out.append(m.group(0))
        pos = m.end()
    out.append(convert_piece(text[pos:], dst, src, keep_number_space))
    return "".join(out)


def convert_piece(text, dst, src, keep_number_space):
    if dst["spacing"] != src["spacing"]:
        text = convert_spacing(text, dst["spacing"], keep_number_space)
    if dst["question"] != src["question"]:
        text = convert_question(text, dst["question"])
    return text


def convert_value(key, value, dst, src):
    # ini の 1 つの値を書き換える。キーによって例外がある。名前のキー (name_keys) は末尾の番号の前の空白を残し、
    # 単位のキー (suffix_keys) は、値の先頭の空白を A で入れ、B で外す (「 分」と「分」)。
    name = matches_any(key, dst.get("name_keys", []))
    out = convert_plain(value, dst, src, keep_number_space=name)
    if matches_any(key, dst.get("suffix_keys", [])) and dst["spacing"] != src["spacing"]:
        if dst["spacing"] == "B" and re.match(f" [{JP}]", out):
            out = out[1:]
        elif dst["spacing"] == "A" and re.match(f"[{JP}]", out):
            out = " " + out
    return out


INI_LINE_RE = re.compile(r'^(\s*)([^#;=\s][^=]*)=(")?(.*?)(")?(\s*)$')


def iter_ini_values(text):
    # (キー, 値) の並び (forbidden_ui_words.parse_ini_values と同じ読み方)
    return [(key, value) for _, key, value in forbidden_ui_words.parse_ini_values(text)]


def convert_ini_text(text, dst, src):
    # ini の値だけを書き換える (キー、コメント、引用符、行の並びはそのまま)
    out_lines = []
    for line in text.split("\n"):
        m = INI_LINE_RE.match(line)
        if not m or line.strip().startswith(("#", ";")):
            out_lines.append(line)
            continue
        indent, key, q1, value, q2, tail = m.groups()
        quoted = bool(q1) and bool(q2)
        if not quoted:
            value = (q1 or "") + value + (q2 or "")
        new = convert_value(key.strip(), value, dst, src)
        out_lines.append(f'{indent}{key}={chr(34) if quoted else ""}{new}{chr(34) if quoted else ""}{tail}')
    return "\n".join(out_lines)


def value_pattern(value):
    # ini の値を、%N の所に何が入ってもよい正規表現にする。末尾の「...」は省いても一致する。
    if value.endswith(ELLIPSIS):
        value = value[:-len(ELLIPSIS)]
    parts = ARG_RE.split(value)
    return re.compile("^" + ".+?".join(re.escape(p) for p in parts) + "$")


def literal_size(value):
    # %N、空白、記号を除いた文字の数。どんな文字列にも一致する値 ("%1" や "%1 (%2)") を、キーを探すのに使わないため。
    return len(re.sub(r"%[0-9]+|[\s\W_]", "", value))


class IniIndex:
    # 現在の書き方の ini の値から、[ ] と「」の中の文字列がどのキーの値かを探す。文字の多い値から順に照合する。短い値が
    # 先に一致して、別のキーの規則で書き換えるのを防ぐためである。文字が 2 つより少ない値は使わない。
    def __init__(self, entries):
        usable = [(key, value) for key, value in entries if literal_size(value) >= 2]
        usable.sort(key=lambda kv: -literal_size(kv[1]))
        self.items = [(key, value_pattern(value)) for key, value in usable]

    def key_for(self, inner):
        for key, pattern in self.items:
            if pattern.match(inner):
                return key
        return None


def convert_text_with_spans(text, dst, src, index):
    # 手順書、テンプレート、変更履歴の文字列を書き換える。[ ] と「」の中は、現在の ini の値 (末尾の「...」と %N を許す) と
    # 一致するときだけ、そのキーの規則で書き換える。一致しない文字列はほかのアプリの画面の文字列 (Discord の
    # [ユーザーIDをコピー]、OBS の「OBS Studioについて」など) で、画面と同じ文字列で示すことを空白の規則より優先する
    # ので、書き換えない。変更履歴の、現在の ini にない古いバージョンの名前も、そのときの画面の文字列のまま書き換えない。
    out = []
    pos = 0
    for m in SPAN_RE.finditer(text):
        inner = m.group(1) if m.group(1) is not None else m.group(2)
        out.append(convert_plain(text[pos:m.start()], dst, src))
        key = index.key_for(inner) if index is not None else None
        new_inner = inner if key is None else convert_value(key, inner, dst, src)
        whole = m.group(0)
        out.append(whole[0] + new_inner + whole[-1])
        pos = m.end()
    out.append(convert_plain(text[pos:], dst, src))
    return "".join(out)


def convert_sections(text, dst, src, index, start_re, end_re):
    # start_re の行から end_re の行の手前までの行だけを書き換える (Markdown の ja の節)
    lines = text.split("\n")
    inside = False
    for i, line in enumerate(lines):
        if start_re.match(line):
            inside = True
            continue
        if inside and end_re.match(line):
            inside = False
        if inside:
            lines[i] = convert_text_with_spans(line, dst, src, index)
    return "\n".join(lines)


TEMPLATE_START = re.compile("^## 日本語\\s*$")
TEMPLATE_END = re.compile(r"^## ")
CHANGELOG_START = re.compile(r"^### ja\s*$")
CHANGELOG_END = re.compile(r"^#{1,3} ")


def write_style(path, dst):
    text = path.read_text(encoding="utf-8")
    text = re.sub(r"(?m)^spacing=.*$", f"spacing={dst['spacing']}", text)
    text = re.sub(r"(?m)^question=.*$", f"question={dst['question']}", text)
    path.write_text(text, encoding="utf-8", newline="\n")


def plan_changes(root, dst, src):
    # [(パス, 書き換える前の文字列, 後の文字列)] を返す。ないファイルは NOTE を書いて飛ばす。
    changes = []
    notes = []
    ja_ini = root / JA_INI.relative_to(REPO_ROOT)
    if not ja_ini.exists():
        raise FileNotFoundError(f"{ja_ini} がない")
    ini_text = ja_ini.read_text(encoding="utf-8")
    changes.append((ja_ini, ini_text, convert_ini_text(ini_text, dst, src)))
    index = IniIndex(iter_ini_values(ini_text))
    for path, kind in ((root / GUIDE.relative_to(REPO_ROOT), "guide"),
                       (root / TEMPLATE.relative_to(REPO_ROOT), "template"),
                       (root / CHANGELOG.relative_to(REPO_ROOT), "changelog")):
        if not path.exists():
            notes.append(f"NOTE: {path.name} がないので飛ばした")
            continue
        text = path.read_text(encoding="utf-8")
        if kind == "guide":
            new = "\n".join(convert_text_with_spans(line, dst, src, index) for line in text.split("\n"))
        elif kind == "template":
            new = convert_sections(text, dst, src, index, TEMPLATE_START, TEMPLATE_END)
        else:
            new = convert_sections(text, dst, src, index, CHANGELOG_START, CHANGELOG_END)
        changes.append((path, text, new))
    return changes, notes


def changed_lines(before, after):
    return sum(1 for a, b in zip(before.split("\n"), after.split("\n")) if a != b)


# ---- --selftest ----

SELFTEST_STYLE_LISTS = {
    "name_keys": ["cordmux.source.*.label"],
    "suffix_keys": ["cordmux.settings.unit_*.label"],
}
A_HALF = dict(spacing="A", question="halfwidth", **SELFTEST_STYLE_LISTS)
B_HALF = dict(spacing="B", question="halfwidth", **SELFTEST_STYLE_LISTS)
A_FULL = dict(spacing="A", question="fullwidth", **SELFTEST_STYLE_LISTS)
B_FULL = dict(spacing="B", question="fullwidth", **SELFTEST_STYLE_LISTS)

# (キー, A と半角の ? の文字列, B と全角の疑問符の文字列)
VALUE_CASES = [
    ("k.obs", "OBS の設定", "OBSの設定"),
    ("k.people", "通話中 (%1 人) - 表示中 (%2 人)", "通話中 (%1人) - 表示中 (%2人)"),
    ("k.others", "(他 2 件)", "(他2件)"),
    ("k.progress", "8 個中 3 個完了", "8個中3個完了"),
    ("k.unit_ascii", "500 ms ごと", "500 msごと"),
    ("k.percent", "50% の音量", "50%の音量"),
    ("k.colon", "声: %1", "声: %1"),
    ("k.question", "切り替えますか?", "切り替えますか？"),
    ("k.question_id", "ユーザー ID?", "ユーザーID？"),
    ("k.url", "https://example.com/a?b=1 を開きます", "https://example.com/a?b=1 を開きます"),
    ("k.range", "1-4 人", "1-4人"),
    ("cordmux.settings.self_frame_source.tip", "OBS のソース (キャプチャーボードなど) を左上の枠に出すときに選びます。", "OBSのソース (キャプチャーボードなど) を左上の枠に出すときに選びます。"),
    ("cordmux.source.placeholder.label", "ダミー %1", "ダミー %1"),
    ("cordmux.source.composite.label", "cordmux", "cordmux"),
    ("cordmux.settings.unit_minutes.label", " 分", "分"),
    ("cordmux.settings.unit_ms.label", " ms", " ms"),
]

GUIDE_A = "\n".join([
    "## 概要",
    "1. OBS の [Vesktop に追加] を押します。",
    "   - Discord の [ユーザーIDをコピー] を選びます。",
    "   - OBS の「OBS Studioについて」を開きます。",
    "   - 「ダミー 2」が表示されます。",
    "   - https://example.com/?q=1 を見ますか?",
    "   - 「[OBS のプレビュー](#obsのプレビュー)」と「[licenses](../dist/licenses)」を見ます。",
    "",
])
GUIDE_INI_A = "\n".join([
    'cordmux.dock.add_to_vesktop.button="Vesktop に追加..."',
    'cordmux.source.placeholder.label="ダミー %1"',
    "",
])
GUIDE_B_EXPECT = "\n".join([
    "## 概要",
    "1. OBSの [Vesktopに追加] を押します。",
    "   - Discordの [ユーザーIDをコピー] を選びます。",
    "   - OBSの「OBS Studioについて」を開きます。",
    "   - 「ダミー 2」が表示されます。",
    "   - https://example.com/?q=1 を見ますか？",
    "   - 「[OBS のプレビュー](#obsのプレビュー)」と「[licenses](../dist/licenses)」を見ます。",
    "",
])
TEMPLATE_A = "\n".join([
    "## 日本語",
    "",
    "- `{{zip_asset}}`: OBS のzip ファイルです。",
    "",
    "## English",
    "",
    "- OBS の is not changed?",
    "",
])
TEMPLATE_B_EXPECT = TEMPLATE_A.replace("- `{{zip_asset}}`: OBS のzip ファイル", "- `{{zip_asset}}`: OBSのzipファイル")


def selftest():
    failures = []
    for key, a_text, b_text in VALUE_CASES:
        got_b = convert_value(key, a_text, B_FULL, A_HALF)
        if got_b != b_text:
            failures.append(f"A->B {key}: {got_b!r} (want {b_text!r})")
        back = convert_value(key, got_b, A_HALF, B_FULL)
        if back != a_text:
            failures.append(f"A->B->A {key}: {back!r} (want {a_text!r})")
        # 片方だけを切り替える組み合わせ (空白だけ、疑問符だけ) も元に戻る
        for mid in (B_HALF, A_FULL):
            there = convert_value(key, a_text, mid, A_HALF)
            again = convert_value(key, there, A_HALF, mid)
            if again != a_text:
                failures.append(f"A->{mid['spacing']}/{mid['question']}->A {key}: {again!r}")
    ini_a = "\n".join([
        "# コメント OBS の文字列は変えない",
        'k.obs="OBS の設定"',
        'cordmux.settings.unit_minutes.label=" 分"',
        "",
    ])
    ini_b = convert_ini_text(ini_a, B_FULL, A_HALF)
    if ini_b.split("\n")[0] != ini_a.split("\n")[0]:
        failures.append("ini のコメントが変わった")
    if 'k.obs="OBSの設定"' not in ini_b or 'unit_minutes.label="分"' not in ini_b:
        failures.append(f"ini の値が変わらない: {ini_b!r}")
    if convert_ini_text(ini_b, A_HALF, B_FULL) != ini_a:
        failures.append("ini の A -> B -> A が元に戻らない")
    index_a = IniIndex(iter_ini_values(GUIDE_INI_A))
    guide_b = "\n".join(convert_text_with_spans(line, B_FULL, A_HALF, index_a) for line in GUIDE_A.split("\n"))
    if guide_b != GUIDE_B_EXPECT:
        failures.append(f"手順書の A -> B: {guide_b!r}")
    index_b = IniIndex(iter_ini_values(convert_ini_text(GUIDE_INI_A, B_FULL, A_HALF)))
    guide_back = "\n".join(convert_text_with_spans(line, A_HALF, B_FULL, index_b) for line in guide_b.split("\n"))
    if guide_back != GUIDE_A:
        failures.append(f"手順書の A -> B -> A: {guide_back!r}")
    template_b = convert_sections(TEMPLATE_A, B_FULL, A_HALF, index_a, TEMPLATE_START, TEMPLATE_END)
    if template_b != TEMPLATE_B_EXPECT:
        failures.append(f"テンプレートの A -> B (ja の節だけ。{{{{...}}}} と ` の中は書き換えない): {template_b!r}")
    changelog = "## 1.2.0\n### ja\n#### 変更点\n- [4 分割を作る] の名前 OBS の\n### en\n- OBS の\n"
    changelog_b = convert_sections(changelog, B_FULL, A_HALF, index_a, CHANGELOG_START, CHANGELOG_END)
    want = changelog.replace("名前 OBS の", "名前OBSの")
    if changelog_b != want:
        failures.append(f"変更履歴の A -> B (古いバージョンの [ ] と en の節は書き換えない): {changelog_b!r}")
    real = selftest_real_files()
    failures += real["failures"]
    for failure in failures:
        print(f"selftest 失敗: {failure}")
    if failures:
        return 1
    print(f"selftest OK: apply_ja_style (値 {len(VALUE_CASES)} 例、手順書とテンプレートと変更履歴の例、実際のファイルの "
          f"A -> B -> A {real['checked']} 行 (A の規則に合わない {real['skipped']} 行は数えない))")
    return 0


def a_clean(text):
    # A の空白の規則に合う文字列か (全角の文字と英数字が、空白なしで隣り合う所がないか)
    return not any(p.search(text) for p in INSERT_RES)


def selftest_real_files():
    # 実際の ja-JP.ini と手順書を、ファイルに書かずに A -> B -> A と書き換える。A の規則に合う行は元に戻る。
    # 現在の書き方が A と半角の ? のときだけ確認する。
    result = {"failures": [], "checked": 0, "skipped": 0}
    try:
        style = forbidden_ui_words.load_ja_style()
    except forbidden_ui_words.WordListError as e:
        result["failures"].append(str(e))
        return result
    if style["spacing"] != "A" or style["question"] != "halfwidth":
        return result
    src = style
    dst = dict(style, spacing="B", question="fullwidth")
    changes, _ = plan_changes(REPO_ROOT, dst, src)
    for path, before, after in changes:
        if path == JA_INI:
            index = None
            back = convert_ini_text(after, src, dst)
        else:
            index = IniIndex(iter_ini_values(convert_ini_text(JA_INI.read_text(encoding="utf-8"), dst, src)))
            if path == GUIDE:
                back = "\n".join(convert_text_with_spans(line, src, dst, index) for line in after.split("\n"))
            elif path == TEMPLATE:
                back = convert_sections(after, src, dst, index, TEMPLATE_START, TEMPLATE_END)
            else:
                back = convert_sections(after, src, dst, index, CHANGELOG_START, CHANGELOG_END)
        for line_no, (a, b) in enumerate(zip(before.split("\n"), back.split("\n")), start=1):
            if not a_clean(a):
                result["skipped"] += 1
                continue
            result["checked"] += 1
            if a != b:
                result["failures"].append(f"{path.name}:{line_no}: A -> B -> A で戻らない: {a!r} -> {b!r}")
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description="ja の文字列の、英数字の前後の空白と疑問符の書き方を書き換える")
    parser.add_argument("--spacing", choices=("A", "B"), help="空白の書き方 (省くと現在のまま)")
    parser.add_argument("--question", choices=("halfwidth", "fullwidth"), help="疑問符の幅 (省くと現在のまま)")
    parser.add_argument("--dry-run", action="store_true", help="書き換えずに、変わるファイルと行の数を出す")
    parser.add_argument("--selftest", action="store_true", help="変換の例と、元に戻ることを確認するセルフテスト")
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    try:
        src = forbidden_ui_words.load_ja_style()
    except forbidden_ui_words.WordListError as e:
        print(f"NG: {e}")
        return 1
    dst = dict(src)
    if args.spacing:
        dst["spacing"] = args.spacing
    if args.question:
        dst["question"] = args.question
    if dst["spacing"] == src["spacing"] and dst["question"] == src["question"]:
        print(f"OK: 現在の書き方 (spacing={src['spacing']} question={src['question']}) のまま。何も変えない")
        return 0
    try:
        changes, notes = plan_changes(REPO_ROOT, dst, src)
    except FileNotFoundError as e:
        print(f"NG: {e}")
        return 1
    for note in notes:
        print(note)
    for path, before, after in changes:
        print(f"{path.relative_to(REPO_ROOT).as_posix()}: {changed_lines(before, after)} 行が変わる")
        if not args.dry_run and before != after:
            path.write_text(after, encoding="utf-8", newline="\n")
    if args.dry_run:
        print("dry-run: 何も書いていない")
        return 0
    write_style(STYLE_FILE, dst)
    print(f"OK: spacing={dst['spacing']} question={dst['question']} に書き換えた (ja_style.txt も)")
    return 0


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass
    sys.exit(main())
