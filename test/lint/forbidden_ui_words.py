#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# 画面の文字列 (src-obs/data/locale の ini の値) に、forbidden_ui_words.txt の記号、表記、空白の誤りがないかを
# チェックし、一致した値をエラーにする。一覧の行は名前を付けたセットに分けてあり、一致した行のセットの名前をチェック項目の
# 名前として出力する。一覧の書き方は forbidden_ui_words.txt の先頭のコメントにある。
# install_guide.py と make_release_notes.py も、load_word_rules で同じ一覧を読む (spacing のセットを使う)。
#
#   python forbidden_ui_words.py [PATH...]   ini をチェックする (省くと src-obs/data/locale の 2 つの ini)
#   python forbidden_ui_words.py --spacing B --question fullwidth   ja_style.txt の代わりにこの書き方でチェックする
#   python forbidden_ui_words.py --selftest  NG 例と OK 例でセルフテストを実行する

import argparse
import fnmatch
import os
import re
import sys
from pathlib import Path
from typing import NamedTuple

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
FIXTURES_DIR = SCRIPT_DIR / "fixtures"
WORDS_FILE = SCRIPT_DIR / "forbidden_ui_words.txt"
STYLE_FILE = SCRIPT_DIR / "ja_style.txt"
WORDING_TOOLS_DIR = REPO_ROOT / "test" / "tools" / "wording"

DEFAULT_FILES = [
    REPO_ROOT / "src-obs" / "data" / "locale" / "ja-JP.ini",
    REPO_ROOT / "src-obs" / "data" / "locale" / "en-US.ini",
]
PRUNE_DIRS = {
    ".git", ".deps", "node_modules", "build", "build_x64", "release",
    "dist", "coverage", "__pycache__", ".venv", "out",
}

# セットの行: 「# set: spacing 空白の誤り」。名前は英小文字、数字、- で書く。
GROUP_RE = re.compile(r"^#\s*set:\s*([a-z][a-z0-9-]*)(?:\s|$)")
# 行の先頭の指定 (いくつでも続けて書ける)。
MARK_RE = re.compile(r"^(ja|en|spacing=[AB]|question=(?:halfwidth|fullwidth)|not_names):")
LANGS = ("ja", "en")
# ja_style.txt の設定 (名前 -> 取りうる値)。
STYLE_CHOICES = {"spacing": ("A", "B"), "question": ("halfwidth", "fullwidth")}
STYLE_LISTS = ("name_keys", "suffix_keys")
# install_guide.py と make_release_notes.py が使うセット。
SPACING_GROUP = "spacing"
LANG_NAME_RE = re.compile(r"^(ja|en)(?:[-_.]|$)", re.IGNORECASE)
EXPECT_RE = re.compile(r"^#\s*expect(?:\[([^\]]*)\])?:\s?(.+)$")


class WordListError(ValueError):
    pass


class WordRule(NamedTuple):
    group: str          # セットの名前 ("spacing" など)
    raw: str            # 行の先頭の指定を除いた正規表現の文字列
    regex: re.Pattern
    lang: object        # 言語の指定 (None か "ja" か "en")
    when: tuple         # ((名前, 値), ...) ja_style.txt の値と合うときだけ使う
    not_names: bool     # name_keys のキーには使わない
    line_no: int


# ---- 一覧の読み込み ----

def parse_word_rules(text, name="words"):
    # 一覧の文字列を WordRule のリストにする。書き方が違えば WordListError を投げる (name はエラーのメッセージに
    # 付ける名前)。
    rules = []
    group = None
    for line_no, raw_line in enumerate(text.splitlines(), start=1):
        # 行の先頭の空白は正規表現の一部 (spacing のセットの「 \?」) なので、末尾の空白だけを除く。
        line = raw_line.rstrip()
        if not line.strip():
            continue
        if line.lstrip().startswith("#"):
            m = GROUP_RE.match(line.lstrip())
            if m:
                group = m.group(1)
            continue
        where = f"{name}:{line_no}"
        if group is None:
            raise WordListError(f"{where}: セットの行 (# set: <名前>) より前に規則の行がある")
        lang = None
        when = []
        not_names = False
        rest = line
        while True:
            m = MARK_RE.match(rest)
            if not m:
                break
            mark = m.group(1)
            rest = rest[m.end():]
            if mark in LANGS:
                if lang is not None:
                    raise WordListError(f"{where}: 言語の指定が 2 つある")
                lang = mark
            elif mark == "not_names":
                not_names = True
            else:
                key, value = mark.split("=", 1)
                if any(k == key for k, _ in when):
                    raise WordListError(f"{where}: {key} の指定が 2 つある")
                when.append((key, value))
        if not rest:
            raise WordListError(f"{where}: 指定の後ろに正規表現がない")
        try:
            # re.ASCII を付けて、\b を ASCII の \w (英数字と _) だけで判定する。既定の Unicode の \b は、かなと漢字も
            # \w に含めるので、「internalの値」のように助詞が直接続くと l と「の」の間に単語境界ができず、`\binternal\b`
            # が一致しない (fixtures/forbidden_ui_words/mechanism/ja-JP.ini で確認する)。\w を使わない行には
            # 影響しない。
            regex = re.compile(rest, re.ASCII)
        except re.error as e:
            raise WordListError(f"{where}: 正規表現が正しくない `{rest}`: {e}")
        rules.append(WordRule(group, rest, regex, lang, tuple(when), not_names, line_no))
    return rules


def load_word_rules(path=WORDS_FILE):
    # 一覧のファイルを読む。ファイルがないか書き方が違えば WordListError を投げる (呼び出し元はエラーにする。
    # 行が抜けたまま成功にしないため)。
    if not path.exists():
        raise WordListError(f"{to_relpath(path)} がない")
    return parse_word_rules(path.read_text(encoding="utf-8-sig"), to_relpath(path))


def parse_ja_style(text, name="ja_style"):
    style = {}
    for line_no, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        where = f"{name}:{line_no}"
        key, sep, value = line.partition("=")
        key = key.strip()
        value = value.strip()
        if not sep:
            raise WordListError(f"{where}: 「名前=値」の形でない")
        if key in style:
            raise WordListError(f"{where}: {key} が 2 回ある")
        if key in STYLE_CHOICES:
            if value not in STYLE_CHOICES[key]:
                raise WordListError(f"{where}: {key} の値 `{value}` は {' / '.join(STYLE_CHOICES[key])} のどれでもない")
            style[key] = value
        elif key in STYLE_LISTS:
            style[key] = value.split()
        else:
            raise WordListError(f"{where}: 知らない名前 `{key}`")
    missing = [key for key in STYLE_CHOICES if key not in style]
    if missing:
        raise WordListError(f"{name}: {', '.join(missing)} がない")
    for key in STYLE_LISTS:
        style.setdefault(key, [])
    return style


def load_ja_style(path=STYLE_FILE):
    # ja_style.txt を {名前: 値} にする。name_keys と suffix_keys の値はパターンのリスト。ファイルがないか書き方が
    # 違えば WordListError を投げる。
    if not path.exists():
        raise WordListError(f"{to_relpath(path)} がない")
    return parse_ja_style(path.read_text(encoding="utf-8-sig"), to_relpath(path))


def is_name_key(key, style):
    return any(fnmatch.fnmatchcase(key, pattern) for pattern in style.get("name_keys", []))


def style_holds(rule, style):
    return all(style.get(key) == value for key, value in rule.when)


def rule_applies(rule, lang, style, key=None):
    # 言語が lang の ini の、キー key の値に、この規則を使うか。key を省くと not_names を見ない。
    if rule.lang is not None and rule.lang != lang:
        return False
    if not style_holds(rule, style):
        return False
    if rule.not_names and key is not None and is_name_key(key, style):
        return False
    return True


# ---- ini の読み込み ----

def parse_ini_values(text):
    # [(行番号, キー, 値)] のリストを返す。値の前後の " を外すだけで、エスケープは解釈しない。cordmux の ini には
    # バックスラッシュや " を含む値がないので、これで足りる。src-vencord/tests/ledger_ini.test.ts の readIniEntries も
    # 同じ読み方をする。
    entries = []
    for line_no, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith("#") or line.startswith(";"):
            continue
        eq = line.find("=")
        if eq == -1:
            continue
        key = line[:eq]
        value = line[eq + 1:]
        if len(value) >= 2 and value.startswith('"') and value.endswith('"'):
            value = value[1:-1]
        entries.append((line_no, key, value))
    return entries


def lang_of(path):
    # ini の言語を、ファイル名の先頭 (ja-JP.ini の ja、en-US.ini の en) で決める。決まらなければ None。
    m = LANG_NAME_RE.match(Path(path).name)
    return m.group(1).lower() if m else None


def to_relpath(path):
    try:
        return path.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def discover_ini_files(path):
    if path.is_file():
        return [path] if path.suffix == ".ini" else []
    result = []
    for dirpath, dirnames, filenames in os.walk(path):
        dirnames[:] = [d for d in dirnames if d not in PRUNE_DIRS]
        for fname in filenames:
            if fname.endswith(".ini"):
                result.append(Path(dirpath) / fname)
    return sorted(result)


# ---- チェック ----

def find_hits(entries, rules, lang, style):
    # [(行番号, キー, 規則, 一致した文字列)]
    hits = []
    for line_no, key, value in entries:
        for rule in rules:
            if not rule_applies(rule, lang, style, key):
                continue
            m = rule.regex.search(value)
            if m:
                hits.append((line_no, key, rule, m.group(0)))
    return hits


def skipped_lang_rules(rules, lang, style):
    # 言語が分からない ini で、言語の指定があるために使わなかった規則の数。言語が分からない ini に両方の言語の行を
    # 使うと、ja と en で逆の向きの行 (全角と半角の疑問符) が同時に一致するので、使わずに NOTE の行で伝える。
    if lang is not None:
        return 0
    return sum(1 for r in rules if r.lang is not None and style_holds(r, style))


def check_text(relpath, text, word_rules, style, lang=None):
    # (エラー, NOTE) の 2 つのリストを返す。
    entries = parse_ini_values(text)
    violations = [
        f"{relpath}:{line_no}: {rule.group}: `{rule.raw}` (`{word}`) が {key} の値にある"
        for line_no, key, rule, word in find_hits(entries, word_rules, lang, style)
    ]
    notes = []
    skipped = skipped_lang_rules(word_rules, lang, style)
    if skipped:
        notes.append(f"NOTE: {relpath}: ファイル名から言語が分からないので、言語を指定した {skipped} 行を使っていない")
    return violations, notes


def check_file(path, word_rules, style):
    try:
        text = path.read_text(encoding="utf-8-sig")
    except (UnicodeDecodeError, OSError):
        return [f"{to_relpath(path)}: 読めない"], []
    return check_text(to_relpath(path), text, word_rules, style, lang_of(path))


# ---- --selftest ----
#
# 例の ini (bad と mechanism のフォルダー) の「# expect: <規則の正規表現>」の行は、次の値の行にその規則が
# 一致することを求める。
# 「# expect[spacing=B]: ...」のように [ ] に書き方の条件を書くと、その書き方のときだけ求める。同じ行にほかの規則が
# 一致しても失敗にしない (spacing のセットの行が「internalの値」に一致しても、\binternal\b が一致することは確認
# できる)。expect のない値の行に規則が一致すると失敗にする。OK 例は expect を持たない。

LINT_FIXTURES_DIR = FIXTURES_DIR / "forbidden_ui_words"
STYLE_COMBOS = [
    {"spacing": s, "question": q} for s in STYLE_CHOICES["spacing"] for q in STYLE_CHOICES["question"]
]


def parse_expectations(text, name):
    # {値の行番号: [(条件の dict, 規則の正規表現の文字列), ...]}
    result = {}
    pending = []
    for line_no, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if not line:
            continue
        if line.startswith("#"):
            m = EXPECT_RE.match(line)
            if m:
                cond = {}
                for part in (m.group(1) or "").split(","):
                    part = part.strip()
                    if not part:
                        continue
                    key, sep, value = part.partition("=")
                    if not sep or key not in STYLE_CHOICES or value not in STYLE_CHOICES[key]:
                        raise WordListError(f"{name}:{line_no}: expect の条件 `{part}` が読めない")
                    cond[key] = value
                pending.append((cond, m.group(2).rstrip()))
            continue
        if "=" in line and pending:
            result[line_no] = pending
            pending = []
    if pending:
        raise WordListError(f"{name}: 最後の expect の後ろに値の行がない")
    return result


def judge_fixture(relpath, text, lang, rules, style, kind_label):
    # 一致のしかたが expect の行のとおりかを確認し、失敗のメッセージのリストを返す。
    expectations = parse_expectations(text, relpath)
    hits = {}
    for line_no, key, rule, word in find_hits(parse_ini_values(text), rules, lang, style):
        hits.setdefault(line_no, []).append(rule.raw)
    failures = []
    known = {r.raw for r in rules}
    style_label = f"spacing={style['spacing']} question={style['question']}"
    for line_no, wants in expectations.items():
        for cond, raw in wants:
            if raw not in known:
                failures.append(f"{relpath}:{line_no}: expect の規則 `{raw}` が{kind_label}にない")
                continue
            if any(style.get(k) != v for k, v in cond.items()):
                continue
            if raw not in hits.get(line_no, []):
                failures.append(f"{relpath}:{line_no}: `{raw}` が一致するはずが一致しない ({style_label})")
    for line_no, raws in hits.items():
        applicable = [raw for cond, raw in expectations.get(line_no, [])
                      if all(style.get(k) == v for k, v in cond.items())]
        if not applicable:
            failures.append(f"{relpath}:{line_no}: 一致しないはずが一致した {raws} ({style_label})")
    return failures


def import_style_converter():
    # 書き方を変換する関数を持つ apply_ja_style.py を読み込む (変換の処理を 1 か所にまとめるため)
    if str(WORDING_TOOLS_DIR) not in sys.path:
        sys.path.insert(0, str(WORDING_TOOLS_DIR))
    import apply_ja_style  # noqa: E402
    return apply_ja_style


def fixture_text_in_style(path, style):
    # ja の NG 例と OK 例は spacing=A と半角の ? で書いてある。値だけを style の書き方に変換した文字列を返す。
    # ja_style.txt の書き方を替えても、同じ例でセルフテストを実行できるようにするため。
    text = path.read_text(encoding="utf-8-sig")
    if lang_of(path) != "ja" or (style["spacing"] == "A" and style["question"] == "halfwidth"):
        return text
    converter = import_style_converter()
    return converter.convert_ini_text(text, style, {"spacing": "A", "question": "halfwidth"})


def selftest_real_lists(failures, counts):
    # 実際の一覧と現在の ja_style.txt で、bad と good の例を確認する
    words = load_word_rules()
    style = load_ja_style()
    bad = sorted((LINT_FIXTURES_DIR / "bad").glob("*.ini"))
    good = sorted((LINT_FIXTURES_DIR / "good").glob("*.ini"))
    if not bad or not good:
        failures.append("NG 例か OK 例がない")
        return
    for path in bad + good:
        relpath = to_relpath(path)
        # OK 例は 4 通りの書き方のすべてで、その書き方に変換してから確認する。NG 例は現在の書き方で確認する。
        styles = [dict(style, **combo) for combo in STYLE_COMBOS] if path in good else [style]
        for one in styles:
            text = fixture_text_in_style(path, one)
            failures.extend(judge_fixture(relpath, text, lang_of(path), words, one, "一覧"))
            counts["fixtures"] += 1
        expectations = parse_expectations(path.read_text(encoding="utf-8-sig"), relpath)
        if path in bad and not expectations:
            failures.append(f"{relpath}: NG 例に expect の行がない")
        if path in good and expectations:
            failures.append(f"{relpath}: OK 例に expect の行がある")
    # install_guide.py と make_release_notes.py が使うセットがあること
    if not any(r.group == SPACING_GROUP for r in words):
        failures.append(f"forbidden_ui_words.txt に {SPACING_GROUP} の行がない (手順書とリリースノートのチェックが使う)")


def selftest_mechanism(failures, counts):
    # セット、言語の指定、書き方の指定、not_names の読み方を、テスト用の一覧 (mechanism/words.txt) で、4 通りの書き方の
    # すべてについて確認する
    base = LINT_FIXTURES_DIR / "mechanism"
    rules = load_word_rules(base / "words.txt")
    files = sorted(base.glob("*.ini"))
    if not files or not any(lang_of(p) is None for p in files):
        failures.append("mechanism の例 (言語が分かる ini と分からない ini) がない")
        return
    for combo in STYLE_COMBOS:
        style = dict(combo, name_keys=["cordmux.source.*.label"], suffix_keys=[])
        for path in files:
            failures.extend(judge_fixture(to_relpath(path), path.read_text(encoding="utf-8-sig"),
                                          lang_of(path), rules, style, "テスト用の一覧"))
            counts["mechanism"] += 1
    # 言語が分からない ini では、言語を指定した行を使わなかったことを NOTE の行で伝える
    style = dict(STYLE_COMBOS[0], name_keys=[], suffix_keys=[])
    other = [p for p in files if lang_of(p) is None][0]
    _, notes = check_file(other, rules, style)
    if not notes:
        failures.append(f"{to_relpath(other)}: 言語を指定した行を使わなかったことを伝える NOTE が出ない")


BROKEN_LISTS = [
    ("rule_before_group", "abc\n# set: sample x\n"),
    ("bad_regex", "# set: sample x\n(unclosed\n"),
    ("two_langs", "# set: sample x\nja:en:abc\n"),
    ("two_spacing", "# set: sample x\nspacing=A:spacing=B:abc\n"),
    ("mark_only", "# set: sample x\nja:\n"),
]
BROKEN_STYLES = [
    ("no_question", "spacing=A\n"),
    ("bad_value", "spacing=C\nquestion=halfwidth\n"),
    ("unknown_name", "spacing=A\nquestion=halfwidth\ncolour=red\n"),
    ("twice", "spacing=A\nspacing=B\nquestion=halfwidth\n"),
    ("no_equal", "spacing A\nquestion=halfwidth\n"),
]


def selftest_parsers(failures, counts):
    for label, text in BROKEN_LISTS:
        try:
            parse_word_rules(text, label)
            failures.append(f"書き方の違う一覧 {label} を読めてしまった")
        except WordListError:
            pass
        counts["parsers"] += 1
    for label, text in BROKEN_STYLES:
        try:
            parse_ja_style(text, label)
            failures.append(f"書き方の違う ja_style {label} を読めてしまった")
        except WordListError:
            pass
        counts["parsers"] += 1
    rules = parse_word_rules("# set: symbols x\nja:question=fullwidth:not_names:\\?\n", "marks")
    r = rules[0]
    if (r.group, r.raw, r.lang, r.when, r.not_names) != ("symbols", "\\?", "ja", (("question", "fullwidth"),), True):
        failures.append(f"行の先頭の指定の読み方が違う: {r}")
    counts["parsers"] += 1
    for name, want in (("ja-JP.ini", "ja"), ("en-US.ini", "en"), ("ja_natural.ini", "ja"), ("value_has_word.ini", None),
                       ("japan.ini", None), ("EN-us.ini", "en")):
        if lang_of(Path(name)) != want:
            failures.append(f"lang_of({name}) が {want} でない")
        counts["parsers"] += 1


def run_selftest():
    failures = []
    counts = {"fixtures": 0, "mechanism": 0, "parsers": 0}
    try:
        selftest_parsers(failures, counts)
        selftest_real_lists(failures, counts)
        selftest_mechanism(failures, counts)
    except WordListError as e:
        failures.append(f"読めない: {e}")
    for failure in failures:
        print(f"selftest 失敗: {failure}")
    if failures:
        return 1
    print(f"selftest OK: forbidden_ui_words (NG 例と OK 例 {counts['fixtures']}、テスト用の一覧の書き方ごとの確認 "
          f"{counts['mechanism']}、一覧と ja_style.txt の読み方 {counts['parsers']})")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="ini の値に、forbidden_ui_words.txt の記号、表記、空白の誤りがないことを確認する"
    )
    parser.add_argument("paths", nargs="*", help="チェックするファイルかフォルダー (省くと src-obs/data/locale の 2 つの ini)")
    parser.add_argument("--spacing", choices=STYLE_CHOICES["spacing"], help="ja_style.txt の spacing の代わり")
    parser.add_argument("--question", choices=STYLE_CHOICES["question"], help="ja_style.txt の question の代わり")
    parser.add_argument("--selftest", action="store_true", help="NG 例と OK 例でセルフテストを実行する")
    args = parser.parse_args(argv)

    if args.selftest:
        return run_selftest()

    if args.paths:
        files = []
        for p in args.paths:
            files.extend(discover_ini_files(Path(p)))
        files = sorted(set(files))
    else:
        files = [p for p in DEFAULT_FILES if p.exists()]

    # チェックする ini が 1 つもないときはエラーにする。0 個のファイルをチェックして成功と出力すると、パスの誤りに
    # 気付けない。
    if not files:
        print("NG: チェックする ini が 1 つもない")
        return 1

    try:
        word_rules = load_word_rules()
        style = load_ja_style()
    except WordListError as e:
        print(f"NG: {e}")
        return 1
    if args.spacing:
        style["spacing"] = args.spacing
    if args.question:
        style["question"] = args.question
    if not word_rules:
        print(f"NG: {to_relpath(WORDS_FILE)} にパターンが 1 つもない")
        return 1

    violations, notes = [], []
    for p in files:
        v, n = check_file(p, word_rules, style)
        violations += v
        notes += n
    for line in notes + violations:
        print(line)
    if violations:
        print(f"NG: 一覧に一致する値が ini に {len(violations)} 箇所ある")
        return 1
    print(f"OK: {len(files)} ファイルをチェックした (一覧に一致する値は ini にない。書き方は spacing={style['spacing']} "
          f"question={style['question']})")
    return 0


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass
    sys.exit(main())
