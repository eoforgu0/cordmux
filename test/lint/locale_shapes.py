#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# ini の値の形 (文の終わり、「...」、英語の大文字の使い方) を、キーの最後の種類の語 (.button、.status など) ごとに
# チェックする。種類ごとの形は docs/ui_guidelines.md の「ini のキーの書式」にあり、チェック項目の名前と中身は、KINDS の
# 下の、種類の語の集合のコメントにある。英語の大文字の使い方で、正しいかを決められないものは警告として出力する。
# %N の数と _one のキーは locale_ids.py がチェックする。
#
#   python locale_shapes.py [DIR]          DIR の ja-JP.ini と en-US.ini をチェックする (省くと src-obs/data/locale)
#   python locale_shapes.py --report-only  エラーがあっても 0 で終わる
#   python locale_shapes.py --selftest     NG 例と OK 例でセルフテストを実行する

import argparse
import contextlib
import io
import re
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
FIXTURES_DIR = SCRIPT_DIR / "fixtures" / "locale_shapes"
DEFAULT_DIR = REPO_ROOT / "src-obs" / "data" / "locale"

if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))
import forbidden_ui_words  # noqa: E402  (ini の読み方を 1 か所にまとめる)

# 種類の語。キーの最後の部分で、_one は種類の語の後ろに付く。種類の語で終わらないキーは unknown-kind のエラーにする。
KINDS = ("button", "menu", "page", "heading", "label", "option", "check", "status", "title", "headline", "text",
         "tip", "empty", "a11y")
KIND_RE = re.compile(r"\.([a-z0-9]+?)(?:_one)?$")
# name-ending: 名前 (ボタン、メニュー項目、ページ、見出し、入力欄のラベル、選択肢、タイトル、チェックボックス) は、
# ja を「。」「です」「ます」「ください」で、en をピリオドで終えない (末尾の "..." は除く)。チェックボックスは
# 機能の名前か短い句で書き、句点を付けないので、ここに入れる。
K1_KINDS = {"button", "menu", "page", "heading", "label", "option", "title", "check"}
# status-ending: 状態の値とメッセージの見出し (.status .headline) と、箇条書きの項目 (名前が _item で終わる .text) は、
# ja を「。」で、en をピリオドで終えない (「です」「ます」で終わるのはよい。en の末尾の "..." は除く)。
K2_KINDS = {"status", "headline"}
# sentence-ending: 文 (.text .tip .empty) は、ja を「。」か疑問符で、en をピリオドか "?" で終える。「:」で終わる
# 前置き (箇条書きや場所の行の前に置く文) と、「...」で終わる進行中の文はエラーにしない。
# .a11y は上の 3 つのチェックの対象にしない。読み上げる名前には句点を付けず、説明は文のこともあるため。
K3_KINDS = {"text", "tip", "empty"}
# ellipsis-mismatch: ボタンとメニュー項目は、ja と en の両方が「...」で終わるか、どちらも終わらない。キーからは、
# 押した後に入力や確認が必要かが分からないので、ja と en で「...」の有無がそろうことだけを確認する。
K4_KINDS = {"button", "menu"}
# en-case: en の大文字の使い方と、固有名詞と cordmux の綴り。Title Case の種類と sentence case の種類がある。.option は
# チェックしない。選択肢の大文字の使い方は、同じ設定の選択肢の全体で決まり、1 つのキーからは決まらないため。
TITLE_KINDS = {"button", "menu", "page", "heading", "label", "title"}
SENTENCE_KINDS = {"check", "status", "headline", "text", "tip", "empty", "a11y"}
# en-case: cordmux で始めない英語の文の種類 (メッセージの見出しと本文、ダイアログの主文と本文、ツールチップ、
# 押せない理由、空の一覧の文)。文の先頭が小文字になるため。
O22_KINDS = {"headline", "text", "tip", "empty"}

ELLIPSIS = "..."
JA_K1_ENDINGS = ("。", "です", "ます", "ください")
JA_PERIOD = "。"
JA_QUESTIONS = ("?", "？")
COLONS = (":", "：")

# 固有名詞。この綴りならエラーにせず、大文字と小文字が違う綴りはエラーにする。
PROPER_NOUNS = ("OBS", "Discord", "Vesktop", "Vencord", "Windows", "FFmpeg", "FPS", "ID", "GPU", "ProRes", "H.264")
PRODUCT = "cordmux"
# Microsoft Writing Style Guide の title-style capitalization で、途中なら小文字にする語 (冠詞、等位接続詞、
# 4 文字以下の前置詞)
MINOR_WORDS = {"a", "an", "the", "and", "but", "or", "nor", "yet", "so", "as", "at", "by", "for", "from", "in", "into",
               "of", "off", "on", "onto", "out", "over", "per", "to", "up", "down", "via", "with"}
# 小文字で書く単位 (大文字の使い方をチェックしない)。dB は国際単位系の表記どおり、d を小文字、B を大文字で書く。
UNITS = {"ms", "px", "min", "fps", "kbps", "mb", "gb", "db"}
# UNITS のうち、小文字だけでなく、この綴りも単位として認める語
UNIT_SPELLINGS = {"dB"}
ARG_RE = re.compile(r"%[0-9]+")
PAREN_RE = re.compile(r"\([^()]*\)")
# 次の語を文の先頭として扱う区切り (状態を並べる「 - 」、「名前: 値」のコロン、文の終わり)
SEGMENT_SPLIT_RE = re.compile(r"((?<=[.?!])\s+|:\s+| - )")
EXPECT_RE = re.compile(r"^#\s*expect:\s*(.+)$")


def kind_of(key):
    m = KIND_RE.search(key)
    return m.group(1) if m and m.group(1) in KINDS else None


def name_part(key):
    # キーの、種類の語の前の部分 (箇条書きの項目は _item で終わる)
    return key.rsplit(".", 2)[-2] if key.count(".") >= 2 else ""


def strip_ellipsis(value):
    return value[:-len(ELLIPSIS)] if value.endswith(ELLIPSIS) else value


# ---- 文の終わりと「...」 ----

def ending_problems(key, kind, ja, en):
    # [(チェックの名前, 言語, メッセージ)] のリストを返す
    problems = []
    item = name_part(key).endswith("_item")
    if kind in K1_KINDS:
        if ja is not None and ja.endswith(JA_K1_ENDINGS):
            problems.append(("name-ending", "ja", "文の終わりの形 (「。」「です」「ます」「ください」) で終わっている (名前には付けない)"))
        if en is not None and strip_ellipsis(en).endswith("."):
            problems.append(("name-ending", "en", "ピリオドで終わっている (名前には付けない)"))
    if kind in K2_KINDS or (kind == "text" and item):
        if ja is not None and ja.endswith(JA_PERIOD):
            problems.append(("status-ending", "ja", "「。」で終わっている (状態の値、見出し、箇条書きの項目には付けない)"))
        if en is not None and not en.endswith(ELLIPSIS) and en.endswith("."):
            problems.append(("status-ending", "en", "ピリオドで終わっている (状態の値、見出し、箇条書きの項目には付けない)"))
    if kind in K3_KINDS and not item:
        if ja is not None and not (ja.endswith((JA_PERIOD,) + JA_QUESTIONS + COLONS) or ja.endswith(ELLIPSIS)):
            problems.append(("sentence-ending", "ja", "「。」か疑問符で終わっていない (文はどちらかで終える)"))
        if en is not None and not (en.endswith((".", "?") + COLONS)):
            problems.append(("sentence-ending", "en", "ピリオドか疑問符で終わっていない (文はどちらかで終える)"))
    if kind in K4_KINDS and ja is not None and en is not None and ja.endswith(ELLIPSIS) != en.endswith(ELLIPSIS):
        problems.append(("ellipsis-mismatch", "ja/en", "ja と en の片方だけが「...」で終わっている (両方に付けるか、どちらにも付けない)"))
    return problems


# ---- 英語の大文字の使い方 ----

def case_words(value):
    # [(語, 位置, 値の最後の語か)] のリストを返す。位置は start (値の先頭)、sentence (文の終わりの後の先頭)、free
    # (「名前: 値」の値の先頭と「 - 」の後。大文字と小文字のどちらで始めてもよい)、hyphen (途中にある、ハイフンで
    # つないだ語の前の部分で、前置詞の形のもの)、mid (それ以外)。括弧の中 (補足は、Title Case の名前の後ろでも
    # sentence case で書く)、%N、数字か「.」を含む語 (32-bit、H.264、README-en.txt)、1 文字の語 (%1x%2 の x) は除き、
    # 除いた語の後ろの語は mid にする。
    value = strip_ellipsis(value)
    value = PAREN_RE.sub(" ", value)
    out = []
    place = "start"
    for index, piece in enumerate(SEGMENT_SPLIT_RE.split(value)):
        if index % 2 == 1:
            place = "free" if (":" in piece or "-" in piece) else "sentence"
            continue
        out += segment_words(piece, place)
    return [(word, where, i == len(out) - 1) for i, (word, where) in enumerate(out)]


def segment_words(segment, place):
    out = []
    if ARG_RE.match(segment.strip()) or segment.strip()[:1].isdigit():
        place = "free"  # %N か数字で始まる部分では、次の語が大文字で始まることを求めない
    segment = ARG_RE.sub(" ", segment)
    for raw in segment.split():
        word = raw.strip("\"'.,;:!?[]")
        if word.endswith("'s"):
            word = word[:-2]
        if not word or any(ch.isdigit() for ch in word) or "." in word or "*" in word:
            place = "mid"
            continue
        parts = word.split("-")
        for i, part in enumerate(parts):
            if len(part) <= 1 or not part.isalpha():
                continue
            # ハイフンでつないだ語の前の部分 ("Per-Person" の Per) は、前置詞の形でも小文字にすることを求めない
            hyphen_head = i == 0 and len(parts) > 1 and part.lower() in MINOR_WORDS and place == "mid"
            out.append((part, "hyphen" if hyphen_head else (place if i == 0 else "mid")))
        place = "mid"
    return out


def proper_problem(word):
    # 固有名詞と cordmux の綴りの違い (Obs、Cordmux) のメッセージを返す。正しい綴りなら "ok"、固有名詞でなければ None
    if word.lower() == PRODUCT:
        return "ok" if word == PRODUCT else f"'{word}' (cordmux は常に小文字で書く)"
    for noun in PROPER_NOUNS:
        if word.lower() == noun.lower():
            return "ok" if word == noun else f"'{word}' ({noun} と書く)"
    return None


def case_problems(kind, en, item=False):
    # ([(チェックの名前, 言語, メッセージ)], [警告のメッセージ]) を返す。item は箇条書きの項目 (_item) かどうか
    problems, reviews = [], []
    if en is None or kind not in TITLE_KINDS | SENTENCE_KINDS:
        return problems, reviews
    words = case_words(en)
    if kind in O22_KINDS and not item and words and words[0][0] == PRODUCT and words[0][1] == "start":
        # 英語の文は cordmux で始めない。ラベル、名前、状態の値は cordmux で始めてよい
        problems.append(("en-case", "en", "文が cordmux で始まっている (主語をほかのものにするか、'The cordmux ...' と書く)"))
    for word, where, last in words:
        proper = proper_problem(word)
        if proper == "ok":
            continue
        if proper is not None:
            problems.append(("en-case", "en", proper))
            continue
        if word.lower() in UNITS and (word.islower() or word in UNIT_SPELLINGS):
            continue
        upper = word[0].isupper()
        if kind in TITLE_KINDS:
            if where == "hyphen":
                continue
            if where != "mid" or last:
                if not upper:
                    problems.append(("en-case", "en", f"'{word}' が大文字で始まっていない (Title Case では最初と最後の語を大文字で始める)"))
            elif word.lower() in MINOR_WORDS:
                if upper:
                    reviews.append(f"'{word}' がタイトルの途中で大文字で始まっている (そこで前置詞、冠詞、接続詞として"
                                   "使っていなければ正しい)")
            elif not upper:
                problems.append(("en-case", "en", f"'{word}' が大文字で始まっていない (Title Case)"))
        else:
            if where in ("start", "sentence"):
                if not upper:
                    problems.append(("en-case", "en", f"文の最初の語 '{word}' が小文字で始まっている (sentence case)"))
            elif where in ("mid", "hyphen") and upper and not word.isupper():
                reviews.append(f"'{word}' が文の途中で大文字で始まっている (ページやウィンドウのタイトルのような、"
                               "画面に表示する名前なら正しい)")
    return problems, reviews


# ---- チェック ----

def read_pairs(folder):
    # {キー: [ja の (行番号, 値) か None, en の (行番号, 値) か None]}
    pairs = {}
    for index, name in enumerate(("ja-JP.ini", "en-US.ini")):
        path = folder / name
        if not path.exists():
            raise FileNotFoundError(f"{to_relpath(path)} がない")
        for line_no, key, value in forbidden_ui_words.parse_ini_values(path.read_text(encoding="utf-8-sig")):
            pairs.setdefault(key, [None, None])[index] = (line_no, value)
    return pairs


def to_relpath(path):
    try:
        return path.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def check_folder(folder):
    # (エラー [(ファイル, 行, キー, チェックの名前, メッセージ)], 警告 [(ファイル, 行, キー, メッセージ)],
    # 種類の語がないキーの数) を返す。種類の語がないキーは unknown-kind のエラーにも入れる。
    pairs = read_pairs(folder)
    names = {"ja": to_relpath(folder / "ja-JP.ini"), "en": to_relpath(folder / "en-US.ini")}
    violations, reviews = [], []
    untyped = 0
    for key, (ja_entry, en_entry) in sorted(pairs.items()):
        kind = kind_of(key)
        if kind is None:
            untyped += 1
            entry = ja_entry or en_entry
            name = names["ja"] if entry is ja_entry else names["en"]
            violations.append((name, entry[0], key, "unknown-kind", "キーの最後の部分が種類の語 (button、status など) ではない"))
            continue
        ja = ja_entry[1] if ja_entry else None
        en = en_entry[1] if en_entry else None

        def where(lang):
            entry = ja_entry if lang == "ja" or (lang == "ja/en" and ja_entry) else en_entry
            return (names["ja"] if entry is ja_entry else names["en"]), entry[0]

        for rule, lang, text in ending_problems(key, kind, ja, en):
            violations.append(where(lang) + (key, rule, text))
        found, review = case_problems(kind, en, name_part(key).endswith("_item"))
        for rule, lang, text in found:
            violations.append(where(lang) + (key, rule, text))
        for text in review:
            reviews.append(where("en") + (key, text))
    return violations, reviews, untyped


def run(folder, strict):
    try:
        violations, reviews, untyped = check_folder(folder)
    except FileNotFoundError as e:
        print(f"NG: {e}")
        return 1
    for path, line, key, text in reviews:
        print(f"{path}:{line}: en-case: 警告: {key}: {text}")
    for path, line, key, rule, text in violations:
        print(f"{path}:{line}: {rule}: {key}: {text}")
    summary = (f"エラー {len(violations)} 件 (うち種類の語がないキー {untyped} 個)、警告 {len(reviews)} 件")
    if violations and strict:
        print(f"NG: {summary}")
        return 1
    print(f"{'OK' if not violations else 'NG'}: {summary}" + ("" if strict else " (--report-only なので 0 で終わる)"))
    return 0


# ---- --selftest ----
#
# fixtures/locale_shapes/<例>/ の ja-JP.ini と en-US.ini を使う。値の行の前の「# expect: name-ending」の行 (いくつでも
# 書ける) には、その値の行で検出されるはずのチェック項目の名前を書く。「# expect: review」は、その行で警告が出ることを
# 示す。expect のない行では何も検出されないことを確認する。

def expectations(path):
    result = {}
    pending = []
    for line_no, raw in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), start=1):
        line = raw.strip()
        if not line:
            continue
        m = EXPECT_RE.match(line)
        if m:
            pending.append(m.group(1).strip())
            continue
        if line.startswith("#"):
            continue
        if "=" in line:
            if pending:
                result[line_no] = sorted(pending)
            pending = []
    return result


def selftest_folder(folder):
    failures = []
    violations, reviews, _ = check_folder(folder)
    got = {}
    for path, line, key, rule, text in violations:
        got.setdefault((path, line), []).append(rule)
    for path, line, key, text in reviews:
        got.setdefault((path, line), []).append("review")
    want = {}
    for name in ("ja-JP.ini", "en-US.ini"):
        for line, rules in expectations(folder / name).items():
            want[(to_relpath(folder / name), line)] = rules
    for place in sorted(set(got) | set(want)):
        if sorted(set(got.get(place, []))) != sorted(set(want.get(place, []))):
            failures.append(f"{place[0]}:{place[1]}: 出た {sorted(set(got.get(place, [])))}、期待 "
                            f"{sorted(set(want.get(place, [])))}")
    return failures, bool(violations), bool(want)


def run_selftest():
    failures = []
    folders = sorted(p for p in FIXTURES_DIR.iterdir() if p.is_dir()) if FIXTURES_DIR.exists() else []
    good = [p for p in folders if p.name.startswith("good")]
    bad = [p for p in folders if p.name.startswith("bad")]
    if not good or not bad:
        print("selftest 失敗: fixtures/locale_shapes に good と bad の例がない")
        return 1
    for folder in folders:
        got, has_violations, has_expect = selftest_folder(folder)
        failures += got
        if folder in good and has_violations:
            failures.append(f"{to_relpath(folder)}: OK 例でエラーが出た")
        if folder in bad and not has_expect:
            failures.append(f"{to_relpath(folder)}: NG 例に expect の行がない")
        # 終了コード: NG 例は 1、OK 例は 0。--report-only ではどちらも 0
        with contextlib.redirect_stdout(io.StringIO()):
            strict = run(folder, True)
            loose = run(folder, False)
        if strict != (1 if folder in bad else 0) or loose != 0:
            failures.append(f"{to_relpath(folder)}: 終了コードが、既定で {strict}、--report-only で {loose}")
    for case, want in (("cordmux.dock.add.button", "button"), ("cordmux.dock.summary.status_one", "status"),
                       ("cordmux.dock.old_key", None), ("cordmux.dock.x.textual", None)):
        if kind_of(case) != want:
            failures.append(f"kind_of({case}) が {kind_of(case)} (期待は {want})")
    for failure in failures:
        print(f"selftest 失敗: {failure}")
    if failures:
        return 1
    print(f"selftest OK: locale_shapes (例のフォルダー {len(folders)} 個)")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="ini の値の形を、キーの最後の種類の語ごとにチェックする")
    parser.add_argument("folder", nargs="?", help="ja-JP.ini と en-US.ini があるフォルダー (省くと src-obs/data/locale)")
    parser.add_argument("--strict", action="store_true", help="既定と同じ (エラーがあれば 1 で終わる)")
    parser.add_argument("--report-only", action="store_true", help="エラーがあっても 0 で終わる")
    parser.add_argument("--selftest", action="store_true", help="NG 例と OK 例でセルフテストを実行する")
    args = parser.parse_args(argv)
    if args.selftest:
        return run_selftest()
    return run(Path(args.folder) if args.folder else DEFAULT_DIR, not args.report_only)


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass
    sys.exit(main())
