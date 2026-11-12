#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# 手順書 (guide/ja.md と guide/en.md。Markdown) の lint。チェックする内容は check_guide と syntax_problems のコメントにある。
# 見出しと語は install_guide_rules.txt にあり、手順書ごとに 1 つのセットを持つ。
# 手順書の Markdown の読み方 (行の種類、インラインのコードとリンク、見出しのアンカー) もここに置く。配布用の zip のツール
# (test/tools/dist-zip の guide_to_text.py、make_dist_zip.py、check_dist_zip.py、make_release_notes.py) が import して、
# 同じ読み方で手順書を読む。使う名前は test/lint/README.md の「install_guide.py」にある。
#
#   python install_guide.py                       rules の target を全部チェックし、日本語と英語の構成が同じかも見る
#   python install_guide.py --guide G [--ini I]   G を、名前で選んだセットでチェックする (I の既定はセットの ini)
#   python install_guide.py --selftest            NG 例と OK 例と、OK 例を書き換えた例でセルフテストする

import argparse
import copy
import re
import string
import sys
import unicodedata
import urllib.parse
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
RULES_FILE = SCRIPT_DIR / "install_guide_rules.txt"
# rules のキーで入れる仕組みを確認する例のセット (セルフテストだけが読む)
MECHANISM_RULES_FILE = SCRIPT_DIR / "fixtures" / "install_guide" / "mechanism" / "rules.txt"
MECHANISM_WORDS_FILE = SCRIPT_DIR / "fixtures" / "forbidden_ui_words" / "mechanism" / "words.txt"

if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))
import forbidden_ui_words  # noqa: E402  (内部の語の一覧 forbidden_ui_words.txt の読み方を 1 か所に保つ)

# ---- Markdown の行 ----
#
# 手順書で使ってよい行の形 (docs/ui_guidelines.md の「手順書の書き方」と dist/README.md の「手順書の形」):
#   # タイトル        1 行目に 1 つだけ
#   ## 節             rules の heading の並び
#   ### 項目          節の中の項目。「インストール」の手順は ### 1. の形
#   - 項目            箇条書き。下に 2 つの空白で字下げした - を 1 段置いてよい
#   1. 項目           番号付きの項目 (入れ子にしない)。下に 3 つの空白で字下げした - を 1 段置いてよい
#   段落              1 段落を 1 行で書く (手で折り返さない)
#   <!-- zip:source_url -->   zip のテキストで、このバージョンのソースコードの URL に置き換える印 (1 つだけ)
# 行の中では、インラインのコード `...` (パスとファイル名)、リンク [テキスト](リンク先) を使ってよい。

MARKER_LINE = "<!-- zip:source_url -->"
LINE_PATTERNS = (
    ("title", re.compile(r"^# (\S.*)$")),
    ("section", re.compile(r"^## (\S.*)$")),
    ("subsection", re.compile(r"^### (\S.*)$")),
    ("bullet", re.compile(r"^(- )(\S.*)$")),
    ("bullet_sub", re.compile(r"^(  - )(\S.*)$")),
    ("number_sub", re.compile(r"^(   - )(\S.*)$")),
    ("number", re.compile(r"^(([0-9]+)\. )(\S.*)$")),
)
HEADING_KINDS = ("title", "section", "subsection")
HEADING_PREFIXES = {"title": "# ", "section": "## ", "subsection": "### "}
ITEM_KINDS = ("bullet", "bullet_sub", "number_sub", "number")
# 段落と同じに扱う行 (前後に空行が要る)
PARAGRAPH_KINDS = ("paragraph", "marker")
# ### の見出しの先頭の手順の番号 (「### 1. Vesktopをインストールする」)
STEP_HEADING_RE = re.compile(r"^([0-9]+)\. ")
CODE_RE = re.compile(r"`([^`\n]+)`")
LINK_RE = re.compile(r"\[([^\[\]\n]+)\]\(([^()\s]+)\)")
URL_SCHEME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9+.-]*:")


class MdLine:
    # 手順書の 1 行。kind は blank、marker、title、section、subsection、bullet、bullet_sub、number_sub、number、
    # paragraph、indented (使ってはいけない字下げ) のどれか。text は印 (# や - や 1.) を除いた中身、prefix は印。
    __slots__ = ("no", "raw", "kind", "text", "prefix", "number")

    def __init__(self, no, raw):
        self.no = no
        self.raw = raw
        self.number = None
        self.prefix = ""
        self.text = raw
        if not raw.strip():
            self.kind = "blank"
            return
        if raw == MARKER_LINE:
            self.kind = "marker"
            return
        for kind, pattern in LINE_PATTERNS:
            m = pattern.match(raw)
            if not m:
                continue
            self.kind = kind
            if kind in HEADING_KINDS:
                self.prefix = HEADING_PREFIXES[kind]
                self.text = m.group(1).rstrip()
            elif kind == "number":
                self.prefix = m.group(1)
                self.number = int(m.group(2))
                self.text = m.group(3).rstrip()
            else:
                self.prefix = m.group(1)
                self.text = m.group(2).rstrip()
            return
        self.kind = "indented" if raw[0] in " \t" else "paragraph"


class MdDoc:
    # 手順書を行に分けたもの。section_of[i] は i 番目の行が属する ## の節の名前 (最初の ## より前は None)、
    # block_of[i] は i 番目の行が属する見出し (# か ## か ###) の行の番号 (見出しより前は 0)。
    def __init__(self, text):
        self.lines = [MdLine(no, raw) for no, raw in enumerate(text.split("\n"), start=1)]
        self.headings = [line for line in self.lines if line.kind in HEADING_KINDS]
        self.title = self.lines[0] if self.lines and self.lines[0].kind == "title" else None
        self.section_of = []
        self.block_of = []
        section, block = None, 0
        for line in self.lines:
            if line.kind == "section":
                section = plain_text(line.text)
            if line.kind in HEADING_KINDS:
                block = line.no
            self.section_of.append(section)
            self.block_of.append(block)
        self.anchors = dict(zip(github_anchors([plain_text(h.text) for h in self.headings]), self.headings))

    def section_names(self):
        return [plain_text(line.text) for line in self.lines if line.kind == "section"]

    def section_lines(self, name):
        # 節の行 (## の見出しの行を除く)。同じ名前の節が 2 つあれば、最初のもの。
        found = [i for i, line in enumerate(self.lines) if line.kind == "section" and plain_text(line.text) == name]
        if not found:
            return []
        start = found[0] + 1
        end = next((i for i in range(start, len(self.lines)) if self.lines[i].kind == "section"), len(self.lines))
        return self.lines[start:end]

    def subsection_names(self, name):
        return [plain_text(line.text) for line in self.section_lines(name) if line.kind == "subsection"]

    def heading_texts(self):
        return [plain_text(line.text) for line in self.headings]


def parse_markdown(text):
    if text.startswith("\ufeff"):
        text = text[1:]
    return MdDoc(text)


def split_inline(text):
    # 1 行の中身を [("text", 文字列) | ("code", 中身) | ("link", テキスト, リンク先)] に分ける。インラインのコードを先に
    # 切り出す (コードの中の [ ] はリンクにしない)。
    parts = []
    pos = 0
    for m in CODE_RE.finditer(text):
        parts += _split_links(text[pos:m.start()])
        parts.append(("code", m.group(1)))
        pos = m.end()
    return parts + _split_links(text[pos:])


def _split_links(segment):
    parts = []
    pos = 0
    for m in LINK_RE.finditer(segment):
        if m.start() > pos:
            parts.append(("text", segment[pos:m.start()]))
        parts.append(("link", m.group(1), m.group(2)))
        pos = m.end()
    if pos < len(segment):
        parts.append(("text", segment[pos:]))
    return parts


def render_inline(text, code=None, link=None):
    # インラインのコードとリンクを置き換えた文字列。既定は、コードは中身、リンクはテキスト (GitHub の表示と同じ文字)。
    code = code or (lambda content: content)
    link = link or (lambda label, target: label)
    out = []
    for part in split_inline(text):
        if part[0] == "text":
            out.append(part[1])
        elif part[0] == "code":
            out.append(code(part[1]))
        else:
            out.append(link(part[1], part[2]))
    return "".join(out)


def plain_text(text):
    return render_inline(text)


def github_slug(text):
    # GitHub が見出しから作るアンカー: 小文字にし、文字・数字・空白・- と _ のほかの文字を除き、空白を - にする。
    # 文字と数字は Unicode の分類 (L、N と、結合文字の M) で決める (かなと漢字も残る)。
    out = []
    for ch in text.lower():
        if ch == " ":
            out.append("-")
        elif ch in "-_" or unicodedata.category(ch)[0] in "LNM":
            out.append(ch)
    return "".join(out)


def github_anchors(heading_texts):
    # 見出しの並びのアンカー。同じものが重なったら、2 つ目から -1、-2 を付ける (GitHub の github-slugger と同じ)。
    occurrences = {}
    anchors = []
    for text in heading_texts:
        original = github_slug(text)
        slug = original
        while slug in occurrences:
            occurrences[original] += 1
            slug = f"{original}-{occurrences[original]}"
        occurrences[slug] = 0
        anchors.append(slug)
    return anchors


def sections_by_heading(text):
    # 手順書の文字列を「## の見出し -> 節の本文 (見出しの行を除く)」にする。Markdown の手順書と、guide_to_text.py が
    # 作った zip の中のテキスト (見出しの # を残す) の両方を読める。check_dist_zip.py と make_release_notes.py が、
    # rules の role の見出しで節を探すのに使う。同じ見出しが 2 つあれば最初のものを使う (並びは lint がチェックする)。
    doc = parse_markdown(text.replace("\r", ""))
    result = {}
    for name in doc.section_names():
        if name not in result:
            result[name] = "\n".join(line.raw for line in doc.section_lines(name))
    return result


def render_heading(name):
    # 節の見出しの行 (セルフテストとほかのツールが、見出しを作るのに使う)
    return "## " + name


# ---- 使ってはいけない記法 ----

# 行の先頭の、使ってはいけない記法 (段落の行で見る)
BLOCK_SYNTAX = (
    (re.compile(r"^#{4,}(\s|$)"), "#### 以下の見出しは使わない (見出しは # と ## と ### だけ)"),
    (re.compile(r"^#+(\S|$)"), "見出しの # の後ろに空白がない"),
    (re.compile(r"^>"), "引用 (>) は使わない"),
    (re.compile(r"^\|"), "表は使わない"),
    (re.compile(r"^(```|~~~)"), "コードブロックは使わない"),
    (re.compile(r"^[*+]( |$)"), "箇条書きは - で書く"),
    (re.compile(r"^[0-9]+\)"), "番号付きの項目は 1. の形で書く"),
    (re.compile(r"^([-*_=])\1{2,}\s*$"), "区切り線と、下線の形の見出しは使わない"),
    (re.compile(r"^\[[^\]]+\]:"), "リンクの定義は使わない ([テキスト](リンク先) の形で書く)"),
)
TASK_ITEM_RE = re.compile(r"^\[[ xX]\] ")
ESCAPE_RE = re.compile(r"\\[" + "".join(re.escape(c) for c in string.punctuation) + "]")
# 行の中の、使ってはいけない記法 (インラインのコードの中と、リンク先は見ない)
INLINE_SYNTAX = (
    (re.compile(r"\*"), "太字と斜体の記号 (*) は使わない"),
    (re.compile(r"(?<![A-Za-z0-9])_|_(?![A-Za-z0-9])"), "太字と斜体の記号 (_) は使わない (ファイル名は ` で囲む)"),
    (re.compile(r"~~"), "取り消し線は使わない"),
    (re.compile(r"<[A-Za-z/!?]"), "HTML は使わない (HTML のコメントは zip 用の差し込みの印の行だけ)"),
    (re.compile(r"&(?:[A-Za-z][A-Za-z0-9]*|#[0-9]+|#[xX][0-9A-Fa-f]+);"), "文字参照 (&...;) は使わない"),
    (ESCAPE_RE, "バックスラッシュのエスケープは使わない (パスは ` で囲む)"),
    (re.compile(r"\]\(|\]\["), "リンクの形が違う ([テキスト](リンク先) の形で、テキストに [ ] と ` を入れない)"),
    (re.compile(r"\{\{"), "差し込みの {{ は書かない (GitHub の表示にそのまま出る)"),
)
FOLDED_MESSAGE = "空行を挟まずに続く地の文の行 (1 段落を 1 行で書き、手で折り返さない)"


def link_problem(target, doc, base_dir):
    # リンク先の問題の文か None。リンク先は、同じファイルの見出し (#アンカー)、https の URL、リポジトリの中のファイルへの
    # 相対パスのどれか。アンカーは、%XX の形 (エディターが作る形) も GitHub は同じ見出しに解決するので、戻してから比べる。
    if target.startswith("#"):
        if urllib.parse.unquote(target[1:]) not in doc.anchors:
            return f"リンク先の見出しがない: {target}"
        return None
    if target.startswith("https://"):
        return None
    if URL_SCHEME_RE.match(target) or target.startswith("/"):
        return f"リンク先は、同じファイルの見出し (#...)、https の URL、リポジトリの中のファイルへの相対パスのどれかにする: {target}"
    path = urllib.parse.unquote(target.split("#", 1)[0])
    if base_dir is not None and not (Path(base_dir) / path).exists():
        return f"リンク先のファイルがない: {target}"
    return None


def syntax_problems(doc, base_dir=None, quote=None):
    # 使ってよい記法 (上の「Markdown の行」) の外の書き方を探し、[(行番号, 問題の文)] を返す。チェックするもの:
    #   - 1 行目が # のタイトルで、# はそこだけ。#### 以下の見出し、引用、表、コードブロック、* と + の箇条書き、1) の
    #     番号、区切り線、下線の形の見出し、リンクの定義、タスクリスト、使ってはいけない字下げ (入れ子の番号、2 段の入れ子、
    #     4 つ以上の空白のコードブロック) がない
    #   - 2 つの空白の - は - の項目の下に、3 つの空白の - は番号付きの項目の下にだけある
    #   - 段落が空行を挟まずに続かない (手で折り返した段落)。項目の直後に、空行なしで段落が続かない (前の項目の続きに
    #     なる)。段落の直後に、空行なしで項目が続かない
    #   - 行の中に、太字と斜体、取り消し線、画像、HTML、文字参照、バックスラッシュのエスケープ、形の違うリンク、{{ がない。
    #     対のない ` がない。行末に空白とタブがない
    #   - リンク先が、実在する見出しのアンカー、https の URL、実在するファイルへの相対パスのどれか (base_dir は相対パスを
    #     たどる起点。None なら、ファイルがあるかは見ない)
    #   - quote (引用の開きと閉じの 2 文字) の中に、インラインのコードを直接置かない (zip のテキストで引用符が二重になる)
    #   - 番号付きの項目の番号が、見出しごとに 1 から順に並ぶ。### N. の見出しの番号が、節ごとに 1 から順に並ぶ
    #   - zip 用の差し込みの印の行がちょうど 1 つある
    problems = []
    lines = doc.lines
    if not lines or lines[0].kind != "title":
        problems.append((1, "1 行目が # のタイトルでない"))
    previous = None
    for i, line in enumerate(lines):
        raw = line.raw
        if "\t" in raw:
            problems.append((line.no, "タブがある (字下げは空白で書く)"))
        if raw != raw.rstrip():
            problems.append((line.no, "行末に空白がある"))
        kind = line.kind
        if kind == "title" and line.no != 1:
            problems.append((line.no, "# のタイトルは 1 行目に 1 つだけ"))
        if kind == "paragraph":
            for pattern, message in BLOCK_SYNTAX:
                if pattern.search(raw):
                    problems.append((line.no, message))
                    break
        if kind == "indented":
            problems.append((line.no, "使えない字下げ (字下げは、- の項目の下の 2 つの空白の - と、番号付きの項目の下の "
                                      "3 つの空白の - だけ。番号付きの項目は入れ子にしない)"))
        if kind in ("bullet", "number") and TASK_ITEM_RE.match(line.text):
            problems.append((line.no, "タスクリストは使わない"))
        if kind in ("bullet_sub", "number_sub"):
            parent = next((lines[j] for j in range(i - 1, -1, -1) if lines[j].kind != "blank"), None)
            allowed = ("bullet", "bullet_sub") if kind == "bullet_sub" else ("number", "number_sub")
            if parent is None or parent.kind not in allowed:
                where = "- の項目" if kind == "bullet_sub" else "番号付きの項目"
                problems.append((line.no, f"{len(line.prefix) - 2} つの空白で字下げした - は、{where}の下にだけ置く"))
        if previous is not None:
            if kind in PARAGRAPH_KINDS and previous.kind in PARAGRAPH_KINDS:
                problems.append((line.no, FOLDED_MESSAGE))
            elif kind in PARAGRAPH_KINDS and previous.kind in ITEM_KINDS:
                problems.append((line.no, "項目の直後に空行がない (前の項目の続きになる。項目は 1 項目 1 行で書く)"))
            elif kind in ("bullet", "number") and previous.kind in PARAGRAPH_KINDS:
                problems.append((line.no, "段落の直後に空行なしで項目がある (間に空行を入れる)"))
        previous = line
        if kind in ("blank", "marker"):
            continue
        # 行の中の記法
        content = raw if kind in ("paragraph", "indented") else line.text
        outside = render_inline(content, code=lambda c: " " * len(c))
        # 画像 ![...](...) はリンクの形を含むので、リンクをテキストにする前の形で探す
        if "![" in render_inline(content, code=lambda c: " " * len(c), link=lambda label, target: "[" + label + "]()"):
            problems.append((line.no, "画像は使わない"))
        if "`" in outside:
            problems.append((line.no, "対のない ` がある"))
        for pattern, message in INLINE_SYNTAX:
            if pattern.search(outside):
                problems.append((line.no, message))
        for part in split_inline(content):
            if part[0] == "link":
                found = link_problem(part[2], doc, base_dir)
                if found:
                    problems.append((line.no, found))
        if quote:
            for m in CODE_RE.finditer(content):
                if m.start() > 0 and content[m.start() - 1] == quote[0] and content[m.end():m.end() + 1] == quote[1]:
                    problems.append((line.no, f"インラインのコードを {quote[0]}{quote[1]} で囲まない (zip のテキストでは、"
                                              f"コードを {quote[0]}{quote[1]} で囲むので二重になる)"))
    # 番号
    seen = {}
    for i, line in enumerate(lines):
        if line.kind == "number":
            numbers = seen.setdefault(doc.block_of[i], [])
            if line.number != len(numbers) + 1:
                problems.append((line.no, f"番号付きの項目の番号が、見出しごとに 1 から順でない ({len(numbers) + 1} のはずが {line.number})"))
            numbers.append(line.number)
    steps = {}
    for i, line in enumerate(lines):
        if line.kind == "subsection":
            m = STEP_HEADING_RE.match(line.text)
            if m:
                numbers = steps.setdefault(doc.section_of[i], [])
                if int(m.group(1)) != len(numbers) + 1:
                    problems.append((line.no, f"### の手順の番号が、節ごとに 1 から順でない ({len(numbers) + 1} のはずが {m.group(1)})"))
                numbers.append(int(m.group(1)))
    markers = [line.no for line in lines if line.kind == "marker"]
    if len(markers) != 1:
        problems.append((markers[1] if len(markers) > 1 else len(lines),
                         f"zip 用の差し込みの印 '{MARKER_LINE}' の行が {len(markers)} 個ある (1 個のはず)"))
    return problems


# ---- rules の読み込み ----

# 節を指すキー (copied、contains、role、max_steps) の値: 【見出し】、空白、残り。英語の見出しは空白を含むので、最初の
# 空白で分けずに、括弧で見出しの終わりを決める。
SECTION_REF_RE = re.compile(r"^\u3010([^\u3010\u3011]+)\u3011[ \t]+(\S.*)$")
# role の名前。セットごとにちょうど 1 回ずつ書く。update は更新の節で、リリースノートのツールが、今使っている人に
# 更新の節を案内する文の見出しをここから取る。
ROLE_NAMES = ("maintenance", "update")
# item_titles の値: 【節】 の 1 つ
SINGLE_SECTION_RE = re.compile(r"^\u3010([^\u3010\u3011]+)\u3011$")
# 手順の部分の範囲: 【最初の節】 【最後の節】
RANGE_RE = re.compile(r"^\u3010([^\u3010\u3011]+)\u3011\s+\u3010([^\u3010\u3011]+)\u3011$")
# ini_ref の書式の中で、zip の中の手順書の名前に置き換える文字列
ZIP_NAME_FIELD = "{zip_name}"
# [ ] の名前として扱う ini のキーのうち、種類の語 (.button など) で終わらない形のもの。
BUTTON_KEY_RE = re.compile(
    r"^cordmux\.dock\.action\.(?!disabled_)[A-Za-z0-9_]+$"
    r"|^cordmux\.dock\.dialog\.cancel$"
    r"|^cordmux\.dock\.dialog\.[A-Za-z0-9_]+_confirm$"
)
ARG_TOKEN_RE = re.compile(r"%\d+")
WHITESPACE_RE = re.compile(r"[\s\u3000]+")
# [ ] で囲んだ名前 (1 行の中で閉じるもの)。
BUTTON_RE = re.compile(r"\[([^\[\]\n]+)\]")
# 内部の語のうち、forbidden_ui_words.txt にない、作業の中だけで通じる番号 (AC-<数字> の形)。
INTERNAL_ID_SOURCE = r"\bAC-[0-9]+\b"
INTERNAL_ID_RE = re.compile(INTERNAL_ID_SOURCE, re.ASCII)
# placeholder の外側の括弧 (半角と全角)。placeholder の中の文字列を取り出すときに外す。
PLACEHOLDER_BRACKETS = "()\uff08\uff09"
# [ ] だけで書くときに許す、キーの種類の語 (ボタン、メニューの項目、選択肢)
PLAIN_BUTTON_KINDS = ("button", "menu", "option")
KIND_NAMES = ("button", "menu", "page", "heading", "label", "option", "check", "status", "title",
              "headline", "text", "tip", "empty", "a11y")
KEY_KIND_RE = re.compile(r"\.([a-z0-9]+?)(?:_one)?$")
ELLIPSIS = "..."
# 引用のチェックで、ini の値の先頭だけを引用してよいときの区切り (「 - 」と、数を添える「 (」)
QUOTE_HEAD_CUTS = (" (", " - ")
QUOTE_TAIL_MARKS = ("\u3002", ".")
QUOTE_MIN_FIXED_CHARS = 3


class GuideRules:
    def __init__(self):
        self.target = None
        self.ini = None
        self.zip_name = None         # zip の中の手順書の名前 (画面の文言が指す名前)
        self.title = None            # 1 行目の # のタイトル
        self.headings = []           # ## の節の見出し (この順)
        self.procedure_ranges = []   # 手順の部分の範囲 [(最初の節, 最後の節)]
        self.max_steps = []          # [(節, 上限)]
        self.forbidden = []          # (元の文字列, 正規表現)
        self.copied = []             # (見出し, ini のキー)
        self.fixture = None          # セルフテストの OK 例のフォルダー (リポジトリのルートから)
        self.roles = {}              # role -> 見出し
        self.placeholder = None      # placeholder の文字列
        self.forbidden_all = []      # (元の文字列, 正規表現)
        self.contains = []           # (見出し, 文)
        self.target_optional = False
        self.quote_check = None      # 引用の開きと閉じの 2 文字 (なければ引用のチェックをしない)
        self.outside_text = []       # 引用のチェックで許す、ini にない文字列 (OBS や Windows の画面の文字列)
        self.ini_terms = []          # 引用のチェックで許す、ini の値の中で使う語 (ini のどれかの値にあることも確かめる)
        self.component_words = []    # [(語, (種類, ...))] [ ] の名前のチェックで、] のすぐ後ろに続く UI 要素の種類の語
        self.outside_buttons = []    # [ ] の名前のチェックで許す、ほかのアプリの UI 要素の名前
        self.item_title_sections = []  # 項目を ### の見出しで書く節 (item_titles)
        self.ini_refs = []           # [(元の文字列, 正規表現)] ini の値が手順書を指す形 (ini_ref)

    def procedure_names(self):
        # 手順の部分の節の名前 (全部の範囲)
        names = []
        for first, last in self.procedure_ranges:
            for name in self.headings[self.headings.index(first):self.headings.index(last) + 1]:
                if name not in names:
                    names.append(name)
        return names


def parse_section_ref(value, where):
    # 【見出し】 <残り> を (見出し, 残り) にする。形が違えば ValueError。
    m = SECTION_REF_RE.match(value)
    if not m:
        raise ValueError(f"{where}: '【見出し】 値' の形のはずが '{value}'")
    return m.group(1).strip(), m.group(2).strip()


def compile_rule(value, where):
    # rules の語を正規表現にする。壊れていたら ValueError にし、読めない rules として報告する (Python の例外の
    # トレースバックを出力しない)。
    # 英字の禁止語は、大文字と小文字を区別しない (Node.js、NPM、Git のような書き方も同じ語として見つける)。
    # \b は ASCII の文字だけで境界を作る (forbidden_ui_words.py の re.ASCII と同じ理由)。既定の \b は、かなと漢字も
    # 語の文字に数えるので、「cmdで」の d と で の間に境界ができず、一致しなくなるため。github.com のような URL は
    # \bgit\b に一致しない。
    try:
        return re.compile(value, re.ASCII | re.IGNORECASE)
    except re.error as e:
        raise ValueError(f"{where}: 正規表現が壊れている '{value}': {e}")


def compile_ini_ref(value, zip_name, where):
    # ini_ref の書式を正規表現にする。{zip_name} を zip の中の手順書の名前 (文字列のまま) に置き換える。heading の
    # 名前付きのグループ (見出し) が必要で、step のグループ (手順の番号) はあってもなくてもよい。
    if ZIP_NAME_FIELD not in value:
        raise ValueError(f"{where}: ini_ref に {ZIP_NAME_FIELD} がない: '{value}'")
    try:
        pattern = re.compile(value.replace(ZIP_NAME_FIELD, re.escape(zip_name)))
    except re.error as e:
        raise ValueError(f"{where}: ini_ref の正規表現が壊れている '{value}': {e}")
    if "heading" not in pattern.groupindex:
        raise ValueError(f"{where}: ini_ref に (?P<heading>...) のグループがない: '{value}'")
    return pattern


def load_rules(path=RULES_FILE):
    # rules のファイルを読み、セットの並びを返す (dist-zip のツールも使う)。形が違えば ValueError、ファイルを読めなければ OSError。
    return parse_rules(path.read_text(encoding="utf-8-sig"), path.name)


def parse_rules(text, name="rules"):
    # rules の文字列をセットの並びにする。形が違えば ValueError (name はエラーの文に付ける名前)。
    # fixture はセットごとに必ず書く。fixture のないセットはセルフテストが実行されず、何も確認せずに成功するため。
    blocks = []
    current = None
    raw_refs = {}
    for line_no, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        where = f"{name}:{line_no}"
        key, sep, value = line.partition(":")
        if not sep:
            raise ValueError(f"{where}: 「キー: 値」の形でない")
        key = key.strip()
        value = value.strip()
        if key == "target":
            current = GuideRules()
            current.target = value
            blocks.append(current)
            raw_refs[id(current)] = []
            continue
        if current is None:
            raise ValueError(f"{where}: 'target' より前に '{key}' がある")
        single = {"ini": "ini", "zip_name": "zip_name", "title": "title", "fixture": "fixture", "placeholder": "placeholder"}
        if key in single:
            if getattr(current, single[key]) is not None:
                raise ValueError(f"{where}: target {current.target} に '{key}' が 2 つある")
            setattr(current, single[key], value)
        elif key == "heading":
            current.headings.append(value)
        elif key == "procedure":
            m = RANGE_RE.match(value)
            if not m:
                raise ValueError(f"{where}: '【最初の節】 【最後の節】' の形のはずが '{value}'")
            current.procedure_ranges.append((m.group(1).strip(), m.group(2).strip()))
        elif key == "max_steps":
            section, limit = parse_section_ref(value, where)
            if not limit.isdigit() or int(limit) <= 0:
                raise ValueError(f"{where}: max_steps は '【節】 <正の数>' の形にする: '{value}'")
            current.max_steps.append((section, int(limit)))
        elif key == "forbidden":
            current.forbidden.append((value, compile_rule(value, where)))
        elif key == "copied":
            current.copied.append(parse_section_ref(value, where))
        elif key == "role":
            section, role = parse_section_ref(value, where)
            if role not in ROLE_NAMES:
                raise ValueError(f"{where}: 知らない role '{role}' (使える role: {', '.join(ROLE_NAMES)})")
            if role in current.roles:
                raise ValueError(f"{where}: target {current.target} に role '{role}' が 2 つある")
            current.roles[role] = section
        elif key == "forbidden_all":
            current.forbidden_all.append((value, compile_rule(value, where)))
        elif key == "contains":
            current.contains.append(parse_section_ref(value, where))
        elif key == "target_optional":
            if value != "yes":
                raise ValueError(f"{where}: target_optional は 'yes' にする (不要なら行を消す)")
            current.target_optional = True
        elif key == "quote_check":
            if len(value) != 2:
                raise ValueError(f"{where}: quote_check には開きと閉じの引用符の 2 文字を書く: '{value}'")
            current.quote_check = value
        elif key == "outside_text":
            current.outside_text.append(value)
        elif key == "ini_term":
            current.ini_terms.append(value)
        elif key == "component_word":
            word, _, kinds = value.rpartition(" ")
            kinds = tuple(k for k in kinds.split(",") if k)
            if not word.strip() or not kinds or any(k not in KIND_NAMES for k in kinds):
                raise ValueError(f"{where}: 'component_word: <語> <種類>[,<種類>]' の形 (種類は "
                                 f"{', '.join(KIND_NAMES)} のどれか) のはずが '{value}'")
            current.component_words.append((word.strip(), kinds))
        elif key == "outside_button":
            current.outside_buttons.append(value)
        elif key == "item_titles":
            m = SINGLE_SECTION_RE.match(value)
            if not m:
                raise ValueError(f"{where}: '【節】' の形のはずが '{value}'")
            current.item_title_sections.append(m.group(1).strip())
        elif key == "ini_ref":
            raw_refs[id(current)].append((value, where))
        else:
            raise ValueError(f"{where}: 知らないキー '{key}'")
    for block in blocks:
        label = f"{name}: target {block.target}"
        missing = [key for key, val in (
            ("ini", block.ini),
            ("zip_name", block.zip_name),
            ("title", block.title),
            ("placeholder", block.placeholder),
            ("fixture", block.fixture),
        ) if val is None]
        missing += [f"role {role}" for role in ROLE_NAMES if role not in block.roles]
        if not block.headings:
            missing.append("heading")
        if not block.procedure_ranges:
            missing.append("procedure")
        if missing:
            raise ValueError(f"{label}: {missing} がない")
        if len(set(block.headings)) != len(block.headings):
            raise ValueError(f"{label}: 同じ heading が 2 つある")
        # 節を指すキーの見出しは、どれも heading の行にある
        referenced = [section for pair in block.procedure_ranges for section in pair]
        referenced += [section for section, _ in block.copied + block.contains + block.max_steps]
        referenced += list(block.roles.values())
        referenced += list(block.item_title_sections)
        for section in referenced:
            if section not in block.headings:
                raise ValueError(f"{label}: '{section}' が heading にない")
        for first, last in block.procedure_ranges:
            if block.headings.index(first) > block.headings.index(last):
                raise ValueError(f"{label}: 手順の部分の範囲 '{first}' .. '{last}' の順が逆")
        block.ini_refs = [(value, compile_ini_ref(value, block.zip_name, where)) for value, where in raw_refs[id(block)]]
    if not blocks:
        raise ValueError(f"{name}: target がない")
    names = [b.zip_name for b in blocks]
    if len(set(names)) != len(names):
        raise ValueError(f"{name}: 同じ zip_name のセットが 2 つある")
    return blocks


def guide_lang(rules):
    # セットの言語 (ini のファイル名の先頭の ja か en。forbidden_ui_words.py と同じ決め方)
    return forbidden_ui_words.lang_of(Path(rules.ini)) if rules.ini else None


# ---- ini の値 (locale_ids.py の read_ini_entries と同じ簡単な読み方で、値の引用符を外すだけ) ----

def read_ini_values(text):
    entries = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or line.startswith(";"):
            continue
        eq = line.find("=")
        if eq == -1:
            continue
        value = line[eq + 1:]
        if len(value) >= 2 and value.startswith('"') and value.endswith('"'):
            value = value[1:-1]
        entries[line[:eq]] = value
    return entries


def strip_ellipsis(value):
    # 押すとダイアログを開くボタンの末尾の「...」は、手順書に書かない
    return value[:-len(ELLIPSIS)] if value.endswith(ELLIPSIS) else value


def key_kind(key):
    # キーの最後の種類の語。種類の語で終わらないキーは None
    m = KEY_KIND_RE.search(key)
    return m.group(1) if m and m.group(1) in KIND_NAMES else None


def labels_of_kinds(ini_entries, kinds):
    return {strip_ellipsis(v) for k, v in ini_entries.items() if key_kind(k) in kinds}


def button_labels(ini_entries):
    # [ ] だけで書くときに許す名前: BUTTON_KEY_RE のキーと、種類の語がボタン、メニューの項目、選択肢のキーの値
    # (末尾の「...」を除く)
    legacy = {strip_ellipsis(v) for k, v in ini_entries.items() if BUTTON_KEY_RE.match(k)}
    return legacy | labels_of_kinds(ini_entries, PLAIN_BUTTON_KINDS)


def contains_copied(section_text, value):
    # value を %N で分けた断片が、空白を除いた section_text にこの順で現れるか。
    haystack = WHITESPACE_RE.sub("", section_text)
    pos = 0
    for fragment in ARG_TOKEN_RE.split(value):
        needle = WHITESPACE_RE.sub("", fragment)
        if not needle:
            continue
        found = haystack.find(needle, pos)
        if found == -1:
            return False
        pos = found + len(needle)
    return True


def mask_buttons(text):
    # [ ] の中の文字を同じ数の空白に置き換える。[ ] の中は ini と照合したボタンの名前なので、禁止語と forbidden_all の
    # チェックでは見ない。括弧を残して同じ数の空白にするのは、伏せた所から行頭の > や $ が新しくできないようにするため。
    # 内部の語のチェックは伏せない文字列を見る ([ ] の中は ini の値で、forbidden_ui_words.py が同じ語で ini をチェック
    # しているので、伏せなくても一致しない)。
    return BUTTON_RE.sub(lambda m: "[" + " " * len(m.group(1)) + "]", text)


def mask_for_words(text, rules):
    # forbidden と forbidden_all の語を探す前の文字列: [ ] の中と、本文の中の節の見出しの名前を同じ数の空白に置き換える。
    # 見出しの名前も伏せるのは、見出しが依頼の形の文字列を含むと、本文でその節を指しただけでエラーになるため。
    # 伏せるのは見出しと同じ文字列の所だけなので、その前後の依頼の語は見つかる。
    text = mask_buttons(text)
    for heading in sorted(rules.headings, key=len, reverse=True):
        text = text.replace(heading, " " * len(heading))
    return text


def find_words(text, patterns):
    # patterns が text に一致する所を [(位置, 規則の元の文字列, 一致した文字列)] で返す。
    found = []
    for source, pattern in patterns:
        for m in pattern.finditer(text):
            if m.end() > m.start():
                found.append((m.start(), source, m.group(0)))
    found.sort(key=lambda item: (item[0], item[1]))
    return found


def internal_word_patterns():
    # 内部の語のチェックの語: AC-<数字> の形の番号と、forbidden_ui_words.txt の internal-words のセットの行 (ini と同じく、
    # 大文字と小文字を区別する)。ほかのセット (symbols、notation) は手順書をチェックしない。手順書は、ほかのアプリの
    # 文字列を引用するため。spacing のセットは spacing_word_patterns で別にチェックする。
    return [(INTERNAL_ID_SOURCE, INTERNAL_ID_RE)] + forbidden_ui_words.load_word_patterns()


def spacing_word_patterns(rules, word_rules=None, style=None):
    # 空白のチェックの語: forbidden_ui_words.txt の spacing のセットの行のうち、rules のセットの言語と ja_style.txt の
    # 書き方で使う行。word_rules と style は、セルフテストがテスト用の一覧で実行するための引数 (省けば実際の一覧と
    # ja_style.txt)。
    if word_rules is None:
        word_rules = forbidden_ui_words.load_word_rules()
    if style is None:
        style = forbidden_ui_words.load_ja_style()
    lang = guide_lang(rules)
    return [(r.raw, r.regex) for r in word_rules
            if r.group == forbidden_ui_words.SPACING_GROUP and forbidden_ui_words.rule_applies(r, lang, style)]


# 空白のチェックの前に、[ ]、「」とインラインのコードの中を埋める文字。引用元の書き方のままにする所なので、チェックしない。
# 空白で埋めると、spacing のセットの「括弧の内側の空白」の行 (\[ など) が、伏せた跡に一致してしまう。埋める文字は、
# spacing のセットのどの行にも一致しない文字 (空白、英数字、%、かな、漢字のどれでもない) にする。mask_buttons もこの文字で
# 埋める形にしないのは、禁止語のチェックで、埋めた文字 (\w) が語の境目 \b を消してしまうため。
SPACING_MASK_CHAR = "_"
QUOTE_JA_RE = re.compile(r"\u300c([^\u300c\u300d\n]+)\u300d")


def mask_spans(text):
    # [ ] と「」の中の文字を、同じ数の SPACING_MASK_CHAR に置き換える (括弧は残す)。空白のチェックの前に使う
    text = BUTTON_RE.sub(lambda m: "[" + SPACING_MASK_CHAR * len(m.group(1)) + "]", text)
    return QUOTE_JA_RE.sub(lambda m: "\u300c" + SPACING_MASK_CHAR * len(m.group(1)) + "\u300d", text)


def squeeze(text):
    # 空白と改行を除いた文字列と、その文字列の位置 -> 元の文字列の位置 の表。
    kept = []
    index_map = []
    for i, ch in enumerate(text):
        if not ch.isspace():
            kept.append(ch)
            index_map.append(i)
    return "".join(kept), index_map


def placeholder_core(placeholder):
    # placeholder の外側の括弧と空白を除いた文字列 (placeholder を書き換えた所を探すのに使う)。
    return WHITESPACE_RE.sub("", placeholder.strip().strip(PLACEHOLDER_BRACKETS))


def quote_forms(ini_entries):
    # 引用のチェックで、引用の中の文字列と比べる ini の値の正規表現 (%N には何が入ってもよい)。値そのもの、末尾の
    # 「...」「。」「.」を省いた形、「 (」か「 - 」の前で切った先頭 (「「通話中」で始まる行」のように、先頭だけを
    # 引用するため)。
    forms = []
    for value in ini_entries.values():
        candidates = {value, strip_ellipsis(value)}
        for mark in QUOTE_TAIL_MARKS:
            candidates |= {c[:-len(mark)] for c in list(candidates) if c.endswith(mark)}
        for cut in QUOTE_HEAD_CUTS:
            candidates |= {c[:c.index(cut)] for c in list(candidates) if cut in c}
        for c in candidates:
            c = WHITESPACE_RE.sub(" ", c).strip()
            # %N を含む値のうち、固定の文字が少ないものは使わない。%1 だけの値はどんな文字列にも一致し、「%1x%2」は x を含む
            # 文字列 (cordmux など) のすべてに一致するので、古い文言の引用を見つけられなくなる。
            if ARG_TOKEN_RE.search(c) and len(WHITESPACE_RE.sub("", ARG_TOKEN_RE.sub("", c))) < QUOTE_MIN_FIXED_CHARS:
                continue
            parts = ARG_TOKEN_RE.split(c)
            forms.append(re.compile("^" + ".+?".join(re.escape(p) for p in parts) + "$"))
    return forms


def quote_spans(text, quote):
    # 引用の [(開きの位置, 中の文字列, 親の番号)]。開きと閉じが違う文字 (「」) なら入れ子を読み、親は外側の引用の番号
    # (外側がなければ None)。同じ文字 ("") なら、左から 2 つずつ組にする。
    opener, closer = quote[0], quote[1]
    if opener == closer:
        pattern = re.compile(re.escape(opener) + "([^" + re.escape(opener) + "]+?)" + re.escape(closer))
        return [(m.start(), m.group(1), None) for m in pattern.finditer(text)]
    spans = []
    stack = []
    opened = []
    for i, ch in enumerate(text):
        if ch == opener:
            stack.append(len(opened))
            opened.append([i, None, stack[-2] if len(stack) > 1 else None])
        elif ch == closer and stack:
            opened[stack.pop()][1] = i
    for start, end, parent in opened:
        if end is not None:
            spans.append((start, text[start + 1:end], parent))
    return spans


def bracket_problems(line, labels, kind_labels, rules):
    # [ ] の名前のチェック (1 行分)。[(名前, 問題の文)] を返す。
    problems = []
    for m in BUTTON_RE.finditer(line):
        name = m.group(1)
        if name in rules.outside_buttons:
            continue
        after = line[m.end():].lstrip(" \t\u3000")
        word = next((w for w, _ in rules.component_words
                     if after.startswith(w) and not (w[-1:].isascii() and w[-1:].isalnum()
                                                     and after[len(w):len(w) + 1].isalnum())), None)
        if word is not None:
            allowed = kind_labels[word]
            if name not in allowed:
                problems.append((name, f"[{name}] {word} は、ini のその種類の名前でない"))
        elif name not in labels:
            problems.append((name, f"[{name}] は ini のボタンの名前でない"))
    return problems


def ini_reference_problems(ini_entries, doc, rules):
    # 画面の文言から手順書への参照: ini の値の中で ini_ref の形に一致した所の見出し (heading) が、手順書の ## か ### の
    # 見出しにあり、手順の番号 (step) があれば、その ## の節に ### <番号>. の見出しがある。zip の中の手順書の名前
    # (zip_name) が値にあるのに、ini_ref のどの形にも一致しなければ、参照の形が読めないので問題にする (文言を変えて
    # チェックが空振りするのを防ぐ)。
    problems = []
    if not rules.zip_name:
        return problems
    names = set(doc.heading_texts())
    for key, raw in sorted(ini_entries.items()):
        value = raw.replace('\\"', '"')
        covered = []
        for source, pattern in rules.ini_refs:
            for m in pattern.finditer(value):
                covered.append((m.start(), m.end()))
                heading = m.group("heading")
                step = m.groupdict().get("step")
                if heading not in names:
                    problems.append(f"ini の参照: {key}: '{heading}' が手順書の ## か ### の見出しにない")
                elif step and not any(t.startswith(f"{step}. ") for t in doc.subsection_names(heading)):
                    problems.append(f"ini の参照: {key}: 節 '{heading}' に ### {step}. の見出しがない")
        pos = value.find(rules.zip_name)
        while pos != -1:
            if not any(a <= pos < b for a, b in covered):
                problems.append(f"ini の参照: {key}: {rules.zip_name} を指す形が、rules の ini_ref のどれにも一致しない")
            pos = value.find(rules.zip_name, pos + 1)
    return problems


def structure_of(doc):
    # 日本語と英語で比べる構成: ## の節ごとの (### の数, 番号付きの項目の数)
    return [(name, len(doc.subsection_names(name)),
             sum(1 for line in doc.section_lines(name) if line.kind == "number"))
            for name in doc.section_names()]


def parity_problems(named_docs):
    # named_docs ([(ラベル, MdDoc)]) の構成が、最初の手順書と同じかを確認する (## の数、節ごとの ### の数と番号付きの
    # 項目の数。節は並びの位置で比べる)。片方だけに手順や項目を足したことに気付くため。
    problems = []
    if len(named_docs) < 2:
        return problems
    base_label, base_doc = named_docs[0]
    base = structure_of(base_doc)
    for label, doc in named_docs[1:]:
        other = structure_of(doc)
        if len(other) != len(base):
            problems.append(f"{label}: ## の数が {len(other)} ある ({base_label} は {len(base)})")
            continue
        for (name_a, sub_a, num_a), (name_b, sub_b, num_b) in zip(base, other):
            if sub_a != sub_b:
                problems.append(f"{label}: 節 '{name_b}' の ### の数が {sub_b} ある ({base_label} の '{name_a}' は {sub_a})")
            if num_a != num_b:
                problems.append(f"{label}: 節 '{name_b}' の番号付きの項目の数が {num_b} ある ({base_label} の '{name_a}' は {num_a})")
    return problems


# ---- チェックの本体 ----

def check_guide(label, guide_text, ini_entries, rules, base_dir=None, spacing_patterns=None):
    # 手順書の文字列をチェックし、問題の文の並び (「label:行番号: 内容」の形) を返す。チェックするもの:
    #   - ファイル全体に CR がない
    #   - 使ってよい記法だけを使っている (syntax_problems。base_dir は相対リンクをたどる起点)
    #   - 1 行目が rules の title。## の見出しが rules の heading のとおり、同じ順に 1 回ずつある
    #   - max_steps の節の手順 (節の直下の番号付きの項目と ### N. の見出し) が上限まで。item_titles の節の項目が ### の
    #     見出しで書いてある (- の項目でない)
    #   - 手順の部分 (procedure の範囲の節) に forbidden の語がない。引用 (quote_check) の中は、ini の値、手順書の見出し、
    #     outside_text のどれか (リンクはテキストで比べる。入れ子の引用は、外側が合えば内側を見ない)
    #   - forbidden_all の語が、全部の節とタイトルにない
    #   - [ ] の名前が、ini のボタン、メニューの項目、選択肢の名前 (後ろに component_word が続くときは、その種類の名前)
    #     か outside_button のどれか。1 行の中で閉じない [ か ] も問題にする
    #   - copied の節に ini の値が、contains の節に定型の文がある (%N の所は何でもよく、空白は比べない)
    #   - ファイル全体に、内部の語 (AC-<数字> の形の番号と、forbidden_ui_words.txt の internal-words のセット) がない
    #   - placeholder の中の文字列が、placeholder のまま書いた所のほかにない
    #   - [ ]、「」とインラインのコードの外に、forbidden_ui_words.txt の spacing のセットに一致する書き方がない
    #   - ini の値が手順書を指す所 (ini_ref) の見出しと手順の番号が、手順書にある (ini_reference_problems)
    # spacing_patterns は空白のチェックの語 (省けば実際の一覧と ja_style.txt)。
    violations = []

    if "\r" in guide_text:
        violations.append(f"{label}: CR がある (改行は LF にする)")
        guide_text = guide_text.replace("\r", "")

    doc = parse_markdown(guide_text)
    violations += [f"{label}:{no}: {message}" for no, message in syntax_problems(doc, base_dir, rules.quote_check)]

    # タイトルと ## の見出し
    if doc.title is None or plain_text(doc.title.text) != rules.title:
        violations.append(f"{label}:1: 1 行目が '# {rules.title}' でない")
    sections = [(plain_text(line.text), line.no) for line in doc.lines if line.kind == "section"]
    found_names = [name for name, _ in sections]
    for name, line_no in sections:
        if name not in rules.headings:
            violations.append(f"{label}:{line_no}: 知らない見出し '## {name}'")
    for name in rules.headings:
        count = found_names.count(name)
        if count != 1:
            violations.append(f"{label}: 見出し '## {name}' が {count} 回ある (1 回のはず)")
    first_seen = []
    for n in found_names:
        if n in rules.headings and n not in first_seen:
            first_seen.append(n)
    if first_seen != [n for n in rules.headings if n in first_seen]:
        violations.append(f"{label}: 見出しの順が違う: {found_names}")

    # 手順の数と、項目を ### で書く節
    line_of_section = dict((name, no) for name, no in reversed(sections))
    for name, limit in rules.max_steps:
        body = doc.section_lines(name)
        first_sub = next((k for k, line in enumerate(body) if line.kind == "subsection"), len(body))
        count = sum(1 for line in body[:first_sub] if line.kind == "number")
        count += sum(1 for line in body if line.kind == "subsection" and STEP_HEADING_RE.match(line.text))
        if count > limit:
            violations.append(f"{label}:{line_of_section.get(name, 1)}: 節 '{name}' の手順が {count} 個ある (上限は {limit} 個。"
                              "節の直下の番号付きの項目と、### N. の見出しを数える)")
    for name in rules.item_title_sections:
        body = doc.section_lines(name)
        if name in line_of_section and not any(line.kind == "subsection" for line in body):
            violations.append(f"{label}:{line_of_section[name]}: 節 '{name}' に ### の項目がない")
        first_sub = next((k for k, line in enumerate(body) if line.kind == "subsection"), len(body))
        for line in body[:first_sub]:
            if line.kind in ITEM_KINDS:
                violations.append(f"{label}:{line.no}: 節 '{name}' の項目は ### の見出しで書く (目次とリンクの行き先になるため)")

    # 行ごとの語、[ ] の名前、引用、空白
    procedure_names = set(rules.procedure_names())
    labels = button_labels(ini_entries)
    kind_labels = {word: labels_of_kinds(ini_entries, kinds) for word, kinds in rules.component_words}
    forms = quote_forms(ini_entries) if rules.quote_check else []
    accepted_quotes = set(rules.outside_text) | set(rules.ini_terms)
    for text in doc.heading_texts():
        accepted_quotes.add(text)
        m = STEP_HEADING_RE.match(text)
        if m:
            accepted_quotes.add(text[m.end():])
    if spacing_patterns is None:
        spacing_patterns = spacing_word_patterns(rules)
    for i, line in enumerate(doc.lines):
        if line.kind in ("blank", "marker"):
            continue
        section = doc.section_of[i]
        words = plain_text(line.raw)
        masked = mask_for_words(words, rules)
        if section in procedure_names:
            for _, source, word in find_words(masked, rules.forbidden):
                violations.append(f"{label}:{line.no}: 手順の部分の禁止語: '{word.strip()}' (規則 {source})")
            if rules.quote_check:
                spans = quote_spans(words, rules.quote_check)
                accepted = {}
                for index, (_, inner, parent) in enumerate(spans):
                    if parent is not None and accepted.get(parent):
                        accepted[index] = True
                        continue
                    inner = WHITESPACE_RE.sub(" ", inner).strip()
                    ok = inner in accepted_quotes or any(form.match(inner) for form in forms)
                    accepted[index] = ok
                    if not ok:
                        violations.append(
                            f"{label}:{line.no}: 引用した文字列が ini にない: {rules.quote_check[0]}{inner}"
                            f"{rules.quote_check[1]} (ほかのアプリの文字列は outside_text に書く)")
        for _, source, word in find_words(masked, rules.forbidden_all):
            violations.append(f"{label}:{line.no}: 手順書で使わない語: '{word.strip()}' (規則 {source})")
        names_line = render_inline(line.raw, code=lambda c: " " * len(c))
        for _, problem in bracket_problems(names_line, labels, kind_labels, rules):
            violations.append(f"{label}:{line.no}: {problem}")
        rest = BUTTON_RE.sub("", names_line)
        if "[" in rest or "]" in rest:
            violations.append(f"{label}:{line.no}: 同じ行に対のない '[' か ']' がある")
        spacing_line = mask_spans(render_inline(line.raw, code=lambda c: SPACING_MASK_CHAR * len(c)))
        for source, pattern in spacing_patterns:
            m = pattern.search(spacing_line)
            if m:
                violations.append(f"{label}:{line.no}: 空白の書き方: '{m.group(0)}' (規則 {source})")

    def section_text(name):
        return "\n".join(plain_text(line.raw) for line in doc.section_lines(name) if line.kind != "marker")

    # ini からコピーした文 (copied)
    for section_name, ini_key in rules.copied:
        value = ini_entries.get(ini_key)
        if value is None:
            violations.append(f"{label}: ini に '{ini_key}' がない ('{section_name}' にコピーするキー)")
            continue
        if section_name in found_names and not contains_copied(section_text(section_name), value):
            violations.append(f"{label}: 節 '{section_name}' に {ini_key} の文がない")

    # 定型の文 (contains)
    for section_name, sentence in rules.contains:
        if section_name in found_names and WHITESPACE_RE.sub("", sentence) not in WHITESPACE_RE.sub("", section_text(section_name)):
            violations.append(f"{label}: 節 '{section_name}' に定型の文がない: {sentence}")

    # 内部の語 (ファイル全体。[ ] も伏せない)
    for start, source, word in find_words(guide_text, internal_word_patterns()):
        line_no = 1 + guide_text.count("\n", 0, start)
        violations.append(f"{label}:{line_no}: 内部の語: '{word.strip()}' (規則 {source})")

    # placeholder の書き換え。リリースのツールは placeholder を探すので、括弧を全角にした placeholder を見落とす。
    # placeholder の括弧の中の文字列 (空白を除く) が、placeholder のまま書いた所のほかにあれば問題にする。
    core = placeholder_core(rules.placeholder)
    if core:
        exact = []
        pos = guide_text.find(rules.placeholder)
        while pos != -1:
            exact.append((pos, pos + len(rules.placeholder)))
            pos = guide_text.find(rules.placeholder, pos + 1)
        squeezed, index_map = squeeze(guide_text)
        pos = squeezed.find(core)
        while pos != -1:
            start = index_map[pos]
            if not any(a <= start < b for a, b in exact):
                line_no = 1 + guide_text.count("\n", 0, start)
                violations.append(f"{label}:{line_no}: placeholder が '{rules.placeholder}' のまま 1 行に書いていない")
            pos = squeezed.find(core, pos + 1)

    # ini の値の中で使う語 (ini_term) が、ini のどれかの値にある (画面の語が変わったら、引用も直すため)
    for term in rules.ini_terms:
        if not any(term in value for value in ini_entries.values()):
            violations.append(f"{label}: ini_term '{term}' が ini のどの値にもない")

    # 画面の文言からの参照
    violations += [f"{label}: {problem}" for problem in ini_reference_problems(ini_entries, doc, rules)]
    return violations


def read_guide(path):
    text = path.read_bytes().decode("utf-8")
    return text[1:] if text.startswith("\ufeff") else text


def check_files(guide_path, ini_path, rules, spacing_patterns=None):
    label = to_relpath(guide_path)
    if not guide_path.exists():
        return [f"{label}: ファイルがない"]
    if not ini_path.exists():
        return [f"{to_relpath(ini_path)}: ファイルがない"]
    ini_entries = read_ini_values(ini_path.read_text(encoding="utf-8-sig"))
    return check_guide(label, read_guide(guide_path), ini_entries, rules, guide_path.parent, spacing_patterns)


def check_target(rules, root=REPO_ROOT, spacing_patterns=None):
    # rules の 1 つのセットの target を、そのセットの ini でチェックし、(問題の並び, NOTE の並び) を返す。target が
    # ないとき、target_optional なら NOTE を返して飛ばし、そうでなければ問題にする。target があるのに
    # target_optional が残っていても問題にする。
    guide = root / rules.target
    if not guide.exists():
        if rules.target_optional:
            return [], [f"NOTE: {rules.target} がないので飛ばした (rules が target_optional: yes)"]
        return [f"{rules.target}: ファイルがない"], []
    violations = []
    if rules.target_optional:
        violations.append(f"{rules.target}: ファイルがあるのに、rules に target_optional: yes が残っている (その行を消す)")
    return violations + check_files(guide, root / rules.ini, rules, spacing_patterns), []


def select_block(blocks, guide_path, ini_path=None):
    # --guide で使うセットを選ぶ。手順書のパスの名前が target の名前と同じセット、なければ ini の名前が、セットの ini の
    # 名前と同じセットを選ぶ (NG 例と OK 例の手順書は target と同じ名前なので、名前で決まる)。決まらなければ ValueError。
    found = [b for b in blocks if Path(b.target).name == guide_path.name]
    if not found and ini_path is not None:
        found = [b for b in blocks if Path(b.ini).name == ini_path.name]
    if len(found) != 1:
        names = ", ".join(f"{Path(b.target).name} / {Path(b.ini).name}" for b in blocks)
        raise ValueError(
            f"{guide_path.name} をどの rules のセットでチェックするか決まらない "
            f"(手順書か --ini のファイルの名前を、次のどれかと同じにする: {names})"
        )
    return found[0]


def to_relpath(path):
    try:
        return path.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return path.as_posix()


# ---- セルフテスト ----
#
# rules のセットごとに、fixture の OK 例 (target と同じ名前の手順書と、セットの ini と同じ名前の ini) で問題がなく、同じ親フォルダーの
# bad_* の NG 例でどれも問題があり、その問題の文が全部 expect.txt の文字列を含むことを確認する。expect.txt で理由まで
# 見るのは、OK 例に節を追加したときに、NG 例が別の理由で失敗し続けても気付けるようにするため。加えて、OK 例をその場で
# 書き換えた例で問題になること、手順の部分の外に禁止語を追加した例などで問題にならないことを確認する。書き換えに使う
# 禁止語、forbidden_all の語、定型の文は rules と OK 例から取る (日本語のセットと英語のセットを、同じ書き換え方で
# 確認するため)。
#
# OK 例に置くもの (なければセルフテストが失敗する): forbidden の規則ごとに、手順の部分の外でその語に一致する行
# (外なら問題にならず、手順の部分へ移すと問題になる)。contains の文の全部。ini_ref の形の参照 (手順の番号のあるもの) を
# 持つ ini の値。
# forbidden_all の語は全部の節で使えないので、OK 例の手順書には置けない。代わりに OK 例のフォルダーの
# forbidden_all_samples.txt に、forbidden_all の規則ごとにその語に一致する行を置く。セルフテストは、OK 例の手順書に
# その語がないことを確認し、サンプルの行を全部の節とタイトルの後ろに挿入すると問題になることを確認する。
FORBIDDEN_ALL_SAMPLES_FILE = "forbidden_all_samples.txt"


def fixture_guide_name(rules):
    # OK 例と NG 例の手順書のファイル名 (target と同じ名前)
    return Path(rules.target).name


def load_forbidden_all_samples(good_dir):
    # OK 例のフォルダーの forbidden_all_samples.txt の行 (空の行と # の行を除く)。ファイルがなければ空。
    path = good_dir / FORBIDDEN_ALL_SAMPLES_FILE
    if not path.exists():
        return []
    return [line for line in path.read_text(encoding="utf-8-sig").split("\n")
            if line.strip() and not line.startswith("#")]


def section_line_index(lines, heading):
    target = render_heading(heading)
    for i, line in enumerate(lines):
        if line == target:
            return i
    raise AssertionError(f"セルフテスト: OK 例に見出し '{target}' がない")


def insert_after_line(text, index, insert):
    # index の行の後ろに、前後を空行で挟んで insert を入れる (段落や項目として読まれるように)。
    lines = text.split("\n")
    return "\n".join(lines[:index + 1] + [""] + insert.split("\n") + [""] + lines[index + 1:])


def mutate_insert_after_heading(text, heading, insert):
    return insert_after_line(text, section_line_index(text.split("\n"), heading), insert)


def mutate_insert_after_title(text, insert):
    return insert_after_line(text, 0, insert)


def mutate_replace_section_body(text, heading, body_lines):
    # 節の本文 (## の行の次から、次の ## の手前まで) を、body_lines の段落 (空行で区切る) に置き換える。
    lines = text.split("\n")
    i = section_line_index(lines, heading)
    end = next((j for j in range(i + 1, len(lines)) if lines[j].startswith("## ")), len(lines))
    body = [""]
    for paragraph in body_lines:
        body += [paragraph, ""]
    return "\n".join(lines[:i + 1] + body + lines[end:])


def mutate_swap_headings(text, a, b):
    lines = text.split("\n")
    try:
        ia, ib = section_line_index(lines, a), section_line_index(lines, b)
    except AssertionError:
        raise AssertionError("セルフテスト: 入れ替える見出しが OK 例にない")
    lines[ia], lines[ib] = lines[ib], lines[ia]
    return "\n".join(lines)


def mutate_drop_heading(text, heading):
    lines = text.split("\n")
    i = section_line_index(lines, heading)
    return "\n".join(lines[:i] + lines[i + 1:])


def find_sample_line(doc, names, pattern, rules):
    # names の節の中で、伏せた文字列 (mask_for_words) に pattern が一致する最初の行 (段落の行。なければ None)。
    for i, line in enumerate(doc.lines):
        if doc.section_of[i] in names and line.kind == "paragraph":
            if pattern.search(mask_for_words(plain_text(line.raw), rules)):
                return line.raw
    return None


def find_sample_in_lines(lines, pattern, rules):
    # lines の中で、伏せた文字列に pattern が一致する最初の行 (なければ None)。
    return next((line for line in lines if pattern.search(mask_for_words(line, rules))), None)


# AC-<数字> の形の番号のテストデータ。references.py が、このファイルの番号の形の文字列として検出しないよう、分けて書く。
SAMPLE_INTERNAL_ID = "AC" + "-61"


def markdown_mutations(good_text, rules, ins, install, after, stats):
    # 記法のチェックの書き換えた例。[(名前, 文字列, 期待)]。
    doc = parse_markdown(good_text)
    q0, q1 = (rules.quote_check or "\u300c\u300d")[0], (rules.quote_check or "\u300c\u300d")[1]
    sub = next((h for h in doc.headings if h.kind == "subsection"), None)
    if sub is None:
        raise AssertionError(f"セルフテスト: {rules.fixture} の OK 例に ### の見出しがない")
    anchor = next(a for a, h in doc.anchors.items() if h is sub)
    encoded = urllib.parse.quote(anchor)
    stats["anchors"] += 1
    lines = good_text.split("\n")
    marker_index = next(i for i, line in enumerate(lines) if line == MARKER_LINE)
    ini_name = Path(rules.ini).name
    cases = [
        ("md_table", ins(good_text, after, "| a | b |"), "表は使わない"),
        ("md_image", ins(good_text, after, "![x](https://example.com/x.png)"), "画像は使わない"),
        ("md_bold", ins(good_text, after, "a **b** c"), "太字と斜体"),
        ("md_italic_underscore", ins(good_text, after, "a _b_ c"), "太字と斜体"),
        ("md_snake_case", ins(good_text, after, "a b_c d"), None),
        ("md_quote", ins(good_text, after, "> quoted"), "引用 (>) は使わない"),
        ("md_fence", ins(good_text, after, "```"), "コードブロックは使わない"),
        ("md_indented_code", ins(good_text, after, "    code"), "使えない字下げ"),
        ("md_h4", ins(good_text, after, "#### x"), "#### 以下の見出しは使わない"),
        ("md_hash_without_space", ins(good_text, after, "#x"), "後ろに空白がない"),
        ("md_second_title", ins(good_text, after, "# x"), "1 行目に 1 つだけ"),
        ("md_title_not_first", "\n" + good_text, "1 行目が"),
        ("md_star_bullet", ins(good_text, after, "* x"), "箇条書きは - で書く"),
        ("md_paren_number", ins(good_text, after, "1) x"), "1. の形で書く"),
        ("md_rule", ins(good_text, after, "---"), "区切り線"),
        ("md_link_definition", ins(good_text, after, "[a]: https://example.com"), "リンクの定義"),
        ("md_task", ins(good_text, after, "- [x] done"), "タスクリスト"),
        ("md_nested_number", ins(good_text, after, "1. a\n   1. b"), "使えない字下げ"),
        ("md_deep_bullet", ins(good_text, after, "- a\n  - b\n    - c"), "使えない字下げ"),
        ("md_sub_bullet_ok", ins(good_text, after, "- a\n  - b"), None),
        ("md_number_sub_ok", ins(good_text, after, "1. a\n   - b"), None),
        ("md_sub_bullet_under_number", ins(good_text, after, "1. a\n  - b"), "- の項目の下にだけ"),
        ("md_number_sub_under_bullet", ins(good_text, after, "- a\n   - b"), "番号付きの項目の下にだけ"),
        ("md_html", ins(good_text, after, "a <b>c</b>"), "HTML は使わない"),
        ("md_comment", ins(good_text, after, "<!-- note -->"), "HTML は使わない"),
        ("md_autolink", ins(good_text, after, "<https://example.com>"), "HTML は使わない"),
        ("md_entity", ins(good_text, after, "a &amp; b"), "文字参照"),
        ("md_escape", ins(good_text, after, "a \\* b"), "エスケープ"),
        ("md_strike", ins(good_text, after, "a ~~b~~"), "取り消し線"),
        ("md_fill", ins(good_text, after, "URL: {{source_url}}"), "{{ は書かない"),
        ("md_fill_in_title", mutate_insert_after_title(good_text, "{{source_url}}"), "{{ は書かない"),
        ("md_trailing_space", ins(good_text, after, "a "), "行末に空白"),
        ("md_tab", ins(good_text, after, "a\tb"), "タブ"),
        ("md_backtick", ins(good_text, after, "a ` b"), "対のない `"),
        ("md_folded_paragraph", ins(good_text, after, "a\nb"), "空行を挟まずに続く"),
        ("md_item_then_text", ins(good_text, after, "- a\nb"), "項目の直後に空行がない"),
        ("md_text_then_item", ins(good_text, after, "a\n- b"), "段落の直後に空行なしで項目"),
        ("md_numbers_out_of_order", ins(good_text, after, "1. a\n3. b"), "1 から順でない"),
        ("md_numbers_ok", ins(good_text, after, "1. a\n2. b"), None),
        ("md_anchor_ok", ins(good_text, after, f"[x](#{anchor})"), None),
        ("md_anchor_encoded", ins(good_text, after, f"[x](#{encoded})"), None),
        ("md_anchor_broken", ins(good_text, after, f"[x](#{anchor}-nosuch)"), "リンク先の見出しがない"),
        # 引用の中のリンク: 手順の部分の引用のチェックは、リンクのテキスト (見出し) で比べる
        ("md_anchor_in_quote", ins(good_text, install, f"{q0}[{plain_text(sub.text)}](#{anchor}){q1}"), None),
        ("md_link_https", ins(good_text, after, "[x](https://example.com/a)"), None),
        ("md_link_http", ins(good_text, after, "[x](http://example.com/a)"), "リンク先は"),
        ("md_link_absolute", ins(good_text, after, "[x](/dist/a.txt)"), "リンク先は"),
        ("md_link_relative_ok", ins(good_text, after, f"[x]({ini_name})"), None),
        ("md_link_relative_missing", ins(good_text, after, "[x](no-such-file.txt)"), "リンク先のファイルがない"),
        ("md_code_ok", ins(good_text, after, "`C:\\x\\y` z"), None),
        ("md_code_in_quote", ins(good_text, after, f"{q0}`plugins`{q1}"), "二重になる"),
        ("md_marker_missing", "\n".join(lines[:marker_index] + lines[marker_index + 1:]), "差し込みの印"),
        ("md_marker_twice", ins(good_text, after, MARKER_LINE), "差し込みの印"),
        ("md_marker_inline", ins(good_text, after, "a " + MARKER_LINE), "HTML は使わない"),
        ("md_marker_folded", ins(good_text, after, "a\n" + MARKER_LINE), "空行を挟まずに続く"),
    ]
    # 手順の番号の見出し (### N.) の番号の並び
    for name in rules.headings:
        steps = [line for line in doc.section_lines(name) if line.kind == "subsection" and STEP_HEADING_RE.match(line.text)]
        if steps:
            stats["step_headings"] += 1
            last = steps[-1]
            wrong = good_text.replace(last.raw, "### 9. " + last.text[STEP_HEADING_RE.match(last.text).end():], 1)
            cases.append((f"md_step_heading_order[{name}]", wrong, "### の手順の番号が"))
            break
    return cases


def build_mutations(good_text, ini_entries, rules, stats, spacing_patterns=()):
    # [(名前, 文字列, 期待, ini)] を返す。期待は None (問題がない) か文字列 (問題があり、問題の文のどれかがその文字列を
    # 含む。空の文字列なら、問題があれば何でもよい)。ini は None (OK 例の ini) か、書き換えた ini の値。OK 例に置くはずの
    # ものがなければ AssertionError。
    headings = rules.headings
    procedure = rules.procedure_names()
    procedure_names = set(procedure)
    # 書き換えに使う手順の節: 手順の部分の 2 つ目の節 (なければ最初の節) と、最後の節。手順の部分の外の節は最後の節。
    install = procedure[1] if len(procedure) > 1 else procedure[0]
    trouble = procedure[-1]
    outside = [h for h in headings if h not in procedure_names]
    if not outside:
        raise AssertionError(f"セルフテスト: {rules.target} の rules に、手順の部分の外の節がない")
    after = outside[-1]
    doc = parse_markdown(good_text)
    base_dir = REPO_ROOT / rules.fixture

    def ins(text, heading, insert):
        return mutate_insert_after_heading(text, heading, insert)

    def ins_title(text, insert):
        return mutate_insert_after_title(text, insert)

    cases = [
        ("mut_powershell", ins(good_text, install, "Open PowerShell."), ""),
        ("mut_node_js", ins(good_text, trouble, "Install Node.js first."), ""),
        ("mut_git_upper", ins(good_text, install, "Run GIT clone."), ""),
        ("mut_prompt_gt", ins(good_text, install, "> run.exe"), ""),
        ("mut_prompt_dollar", ins(good_text, trouble, "$ run.exe"), ""),
        ("mut_unknown_button", ins(good_text, install, "Press [NoSuchButton]."), "ボタンの名前でない"),
        ("mut_heading_swap", mutate_swap_headings(good_text, headings[1], headings[2]), "見出しの順が違う"),
        ("mut_heading_drop", mutate_drop_heading(good_text, headings[-1]), "が 0 回ある"),
        ("mut_heading_twice", good_text + "\n" + render_heading(headings[0]) + "\n", "が 2 回ある"),
        ("mut_heading_unknown", good_text + "\n" + render_heading("Extra") + "\n", "知らない見出し"),
        ("mut_title", good_text.replace(doc.lines[0].raw, "# Other title", 1), "1 行目が"),
        ("mut_crlf", good_text.replace("\n", "\r\n"), "CR"),
        ("mut_word_outside_steps", ins(good_text, after, "PowerShell, npm, node, git."), None),
        ("mut_github_url", ins(good_text, install, "https://github.com/example"), None),
        # [ ] の名前のチェックは全部の節とタイトルが対象
        ("mut_unknown_button_in_title", ins_title(good_text, "[NoSuchButton]"), "ボタンの名前でない"),
        ("mut_unknown_button_in_last", ins(good_text, after, "[NoSuchButton]"), "ボタンの名前でない"),
        ("mut_unknown_button_in_heading", ins(good_text, after, "### [NoSuchButton] x"), "ボタンの名前でない"),
        ("mut_link_is_not_button", ins(good_text, install, "[NoSuchButton](https://example.com/)"), None),
        ("mut_button_in_code_ignored", ins(good_text, install, "`[NoSuchButton]`"), None),
        # 内部の語 (全体)
        ("mut_internal_id_in_last", ins(good_text, after, f"See {SAMPLE_INTERNAL_ID}."), "内部の語"),
        ("mut_internal_id_in_title", ins_title(good_text, f"See {SAMPLE_INTERNAL_ID}."), "内部の語"),
        # placeholder: そのままの形なら手順の部分でも問題にならず、括弧を全角にしたものは問題になる
        ("mut_placeholder_in_steps", ins(good_text, install, rules.placeholder), None),
        # 本文で見出しの名前を指しても、依頼の語や禁止語に数えない (見出しが依頼の形の文字列を含むことがある)
        ("mut_heading_name_in_steps", ins(good_text, install, 'See "' + headings[0] + '".'), None),
    ]
    known = sorted(button_labels(ini_entries))
    if known:
        cases.append(("mut_known_button_in_title", ins_title(good_text, f"[{known[0]}]"), None))
        name = known[0]
        cut = max(1, len(name) // 2)
        cases.append(("mut_button_unpaired", ins(good_text, install, f"Press [{name[:cut]}."), "対のない"))
    wide = rules.placeholder.replace("(", "\uff08").replace(")", "\uff09")
    if wide != rules.placeholder:
        cases.append(("mut_placeholder_wide_brackets", ins(good_text, install, wide), "placeholder"))
    # 手順の数の上限: 節の直下に、上限より 1 つ多い番号付きの項目を足すと問題になる
    for section, limit in rules.max_steps:
        stats["max_steps"] += 1
        steps = "\n".join(f"{n}. step" for n in range(1, limit + 2))
        cases.append((f"mut_max_steps[{section}]", ins(good_text, section, steps), "の手順が"))
    # 項目を ### で書く節: 節の直下の - の項目は問題になる
    for section in rules.item_title_sections:
        stats["item_titles"] += 1
        cases.append((f"mut_item_title_bullet[{section}]", ins(good_text, section, "- item"), "### の見出しで書く"))

    # forbidden の規則ごとに、手順の部分の外にあるサンプルの行を手順の節へ移すと問題になる (OK 例に問題がないので、
    # 外なら問題にならない)
    for source, pattern in rules.forbidden:
        sample = find_sample_line(doc, set(outside), pattern, rules)
        if sample is None:
            raise AssertionError(f"セルフテスト: {rules.fixture} の手順の部分の外に、forbidden の規則 {source} のサンプルの行がない")
        cases.append((f"mut_forbidden_in_steps[{source}]", ins(good_text, install, sample), f"(規則 {source})"))

    # forbidden_all の規則ごとに、OK 例のフォルダーのサンプルの行 (forbidden_all_samples.txt) を使う。OK 例の手順書に
    # その語がないことを確認し、サンプルの行を全部の節と、タイトルの後ろに挿入すると問題になる。
    samples = load_forbidden_all_samples(base_dir)
    masked_good = mask_for_words("\n".join(plain_text(line.raw) for line in doc.lines if line.kind != "marker"), rules)
    for source, pattern in rules.forbidden_all:
        if find_words(masked_good, [(source, pattern)]):
            raise AssertionError(f"セルフテスト: {rules.fixture} の OK 例の手順書に、forbidden_all の規則 {source} の語がある")
        sample = find_sample_in_lines(samples, pattern, rules)
        if sample is None:
            raise AssertionError(
                f"セルフテスト: {rules.fixture} の {FORBIDDEN_ALL_SAMPLES_FILE} に、forbidden_all の規則 {source} のサンプルの行がない")
        for heading in headings:
            cases.append((f"mut_forbidden_all[{source}][{heading}]", ins(good_text, heading, sample), f"(規則 {source})"))
        cases.append((f"mut_forbidden_all[{source}][title]", ins_title(good_text, sample), f"(規則 {source})"))

    # contains: 節の文を 1 つずつ消すと問題になる。全部を並べ直しただけなら問題にならない (並べ直したことが問題の理由で
    # ないことを確認する)。同じ節に copied でコピーした文があれば、その ini の値も残す。
    for section in dict.fromkeys(h for h, _ in rules.contains):
        sentences = [s for h, s in rules.contains if h == section]
        kept_copies = [ini_entries[k] for h, k in rules.copied if h == section and k in ini_entries]
        cases.append((f"mut_contains_all_kept[{section}]",
                      mutate_replace_section_body(good_text, section, sentences + kept_copies), None))
        for i in range(len(sentences)):
            rest = sentences[:i] + sentences[i + 1:]
            cases.append((f"mut_contains_drop[{section}][{i}]",
                          mutate_replace_section_body(good_text, section, rest + kept_copies), "定型の文がない"))

    # 禁止語のチェックで [ ] を伏せる: 禁止語に一致するボタンの名前は、[ ] の中なら問題にならず、[ ] の外なら問題になる
    for label in sorted(button_labels(ini_entries)):
        if any(pattern.search(label) for _, pattern in rules.forbidden):
            stats["masked"] += 1
            cases.append((f"mut_button_masked[{label}]", ins(good_text, install, f"Press [{label}]."), None))
            cases.append((f"mut_button_unmasked[{label}]", ins(good_text, install, f"Press {label}."), "手順の部分の禁止語"))

    # forbidden_ui_words.txt の語: 規則の文字列から \b を除いた文字列がその規則に一致するものをサンプルにし、タイトルの
    # 後ろに追加すると問題になる
    for source, pattern in forbidden_ui_words.load_word_patterns():
        sample = source.replace("\\b", "")
        if sample and pattern.search(sample):
            stats["internal_samples"] += 1
            cases.append((f"mut_internal_word[{source}]", ins_title(good_text, f"x {sample} x"), f"(規則 {source})"))

    cases += new_mechanism_mutations(good_text, ini_entries, rules, stats, ins, install, after, spacing_patterns)
    cases += markdown_mutations(good_text, rules, ins, install, after, stats)
    cases = [case + (None,) for case in cases]
    cases += ini_ref_mutations(good_text, ini_entries, rules, stats)
    # ini_term: ini のどの値からもその語を消すと問題になる
    for term in rules.ini_terms:
        stats["ini_terms"] += 1
        removed = {k: v.replace(term, "x") for k, v in ini_entries.items()}
        cases.append((f"mut_ini_term_missing[{term}]", good_text, "ini のどの値にもない", removed))
    return cases


def ini_ref_mutations(good_text, ini_entries, rules, stats):
    # 画面の文言からの参照: OK 例の ini の、ini_ref に一致する値を書き換える。見出しを知らない名前にすると問題になり、
    # 手順の番号を 9 にすると問題になり、zip の中の名前だけを残して形を崩すと「形が読めない」問題になる。
    cases = []
    if not rules.ini_refs:
        return cases
    with_step = next(((key, raw.replace('\\"', '"'), m)
                      for key, raw in sorted(ini_entries.items())
                      for _, pattern in rules.ini_refs
                      for m in pattern.finditer(raw.replace('\\"', '"'))
                      if m.groupdict().get("step")), None)
    if with_step is None:
        raise AssertionError(f"セルフテスト: {rules.fixture} の ini に、ini_ref に一致する手順の番号のある値がない")
    stats["ini_refs"] += 1
    key, value, m = with_step

    def with_value(new_value):
        changed = dict(ini_entries)
        changed[key] = new_value
        return changed

    def replace_group(name, new):
        return value[:m.start(name)] + new + value[m.end(name):]

    cases.append(("ini_ref_heading_unknown", good_text, "の見出しにない", with_value(replace_group("heading", "NoSuchHeading"))))
    cases.append(("ini_ref_step_unknown", good_text, "### 9. の見出しがない", with_value(replace_group("step", "9"))))
    cases.append(("ini_ref_shape_unknown", good_text, "ini_ref のどれにも一致しない",
                  with_value(value[:m.start()] + rules.zip_name + value[m.end():])))
    return cases


def new_mechanism_mutations(good_text, ini_entries, rules, stats, ins, install, after, spacing_patterns):
    cases = []
    # 末尾の「...」: ini の値から外した名前は問題にならず、外さない名前は問題になる
    for key, value in sorted(ini_entries.items()):
        if value.endswith(ELLIPSIS) and (BUTTON_KEY_RE.match(key) or key_kind(key) in PLAIN_BUTTON_KINDS):
            stats["ellipsis"] += 1
            cases.append((f"mut_ellipsis_stripped[{key}]", ins(good_text, after, f"[{strip_ellipsis(value)}]"), None))
            cases.append((f"mut_ellipsis_kept[{key}]", ins(good_text, after, f"[{value}]"), "ボタンの名前でない"))
            break
    # UI 要素の種類の語: その種類の名前は問題にならず、ボタンの名前に種類の語を付けると問題になり、種類の語のない [ ] も
    # 問題になる
    plain = sorted(button_labels(ini_entries))
    for word, kinds in rules.component_words:
        names = sorted(labels_of_kinds(ini_entries, kinds))
        if not names:
            raise AssertionError(f"セルフテスト: {rules.fixture} の ini に、component_word {word} の種類 {kinds} の値がない")
        stats["component"] += 1
        cases.append((f"mut_component_ok[{word}]", ins(good_text, after, f"[{names[0]}] {word}"), None))
        wrong = next((p for p in plain if p not in names), None)
        if wrong is not None:
            cases.append((f"mut_component_wrong_kind[{word}]", ins(good_text, after, f"[{wrong}] {word}"),
                          "その種類の名前でない"))
        # 種類の語を外すと問題になる名前 (ボタンの名前でも、ほかのアプリの UI 要素の名前でもないもの)。英語の [About] は、
        # OBS の [Help] メニューの項目と cordmux の設定のページの両方の名前なので、除く。
        bare = next((n for n in names if n not in plain and n not in rules.outside_buttons), None)
        if bare is not None:
            cases.append((f"mut_component_without_word[{word}]", ins(good_text, after, f"[{bare}]"),
                          "ボタンの名前でない"))
    for name in rules.outside_buttons:
        cases.append((f"mut_outside_button[{name}]", ins(good_text, install, f"[{name}]"), None))
    # 引用の中の文字列: ini の値、見出し、outside_text なら問題にならず、知らない文字列は手順の部分で問題になり、外なら
    # 問題にならない。入れ子の引用は、外側が見出しなら内側を見ない。
    if rules.quote_check:
        stats["quote"] += 1
        q0, q1 = rules.quote_check[0], rules.quote_check[1]
        doc = parse_markdown(good_text)
        sub_title = next(plain_text(h.text) for h in doc.headings if h.kind == "subsection")
        cases.append(("mut_quote_unknown", ins(good_text, install, f"{q0}NoSuchQuotedText{q1}"), "引用した文字列が ini にない"))
        cases.append(("mut_quote_unknown_outside_steps", ins(good_text, after, f"{q0}NoSuchQuotedText{q1}"), None))
        cases.append(("mut_quote_heading", ins(good_text, install, f"{q0}{rules.headings[0]}{q1}"), None))
        cases.append(("mut_quote_subsection", ins(good_text, install, f"{q0}{sub_title}{q1}"), None))
        cases.append(("mut_quote_subsection_changed", ins(good_text, install, f"{q0}{sub_title}x{q1}"),
                      "引用した文字列が ini にない"))
        for text in rules.outside_text + rules.ini_terms:
            cases.append((f"mut_quote_outside_text[{text}]", ins(good_text, install, f"{q0}{text}{q1}"), None))
        if q0 != q1:
            # 外側が知らない文字列なら問題になる。外側が見出し (手順の部分の外の ### に、知らない文字列の引用を含むもの) なら、
            # 内側を見ない。
            stats["nested_quote"] += 1
            cases.append(("mut_quote_nested_outer_unknown", ins(good_text, install, f"{q0}NoSuch{q0}{rules.headings[0]}{q1}{q1}"),
                          "引用した文字列が ini にない"))
            inner_heading = f"{q0}NoSuchInner{q1} item"
            with_heading = ins(good_text, after, f"### {inner_heading}")
            cases.append(("mut_quote_nested_outer_heading", ins(with_heading, install, f"{q0}{inner_heading}{q1}"), None))
        for key, value in sorted(ini_entries.items()):
            cut = next((c for c in QUOTE_HEAD_CUTS if c in value), None)
            if cut and ARG_TOKEN_RE.search(value):
                head = value[:value.index(cut)]
                filled = ARG_TOKEN_RE.sub("2", value)
                cases.append((f"mut_quote_head[{key}]", ins(good_text, install, f"{q0}{head}{q1}"), None))
                cases.append((f"mut_quote_filled[{key}]", ins(good_text, install, f"{q0}{filled}{q1}"), None))
                cases.append((f"mut_quote_cut_inside[{key}]", ins(good_text, install, f"{q0}{head[:-1]}{q1}"),
                              "引用した文字列が ini にない"))
                break
    # 2 つ目からの範囲の節も手順の部分
    for first, last in rules.procedure_ranges[1:]:
        stats["ranges"] += 1
        cases.append((f"mut_powershell_in_range[{first}]", ins(good_text, first, "Open PowerShell."), "手順の部分の禁止語"))
    # spacing のセット: [ ] と「」の外で一致すれば問題になり、「」の中とインラインのコードの中なら問題にならない
    for sample in ("OBS\u306e\u8a2d\u5b9a", "\u8a2d\u5b9aOBS"):
        hit = [source for source, pattern in spacing_patterns if pattern.search(sample)]
        if hit:
            stats["spacing"] += 1
            cases.append((f"mut_spacing[{sample}]", ins(good_text, after, sample), "空白の書き方"))
            cases.append((f"mut_spacing_quoted[{sample}]", ins(good_text, after, "\u300c" + sample + "\u300d"), None))
            cases.append((f"mut_spacing_code[{sample}]", ins(good_text, after, "`" + sample + "` x"), None))
    return cases


def judge(name, violations, expect, every=False):
    # 期待どおりなら None、違えば失敗の文を返す。every は「問題の文が全部 expect を含む」(bad_* のフォルダー)。
    if expect is None:
        if violations:
            return f"問題がないはずが問題があった: {name}\n" + "\n".join(f"  {v}" for v in violations)
        return None
    if not violations:
        return f"問題があるはずが問題がなかった: {name}"
    if every:
        stray = [v for v in violations if expect not in v]
        if stray:
            return f"'{expect}' とは別の理由の問題があった: {name}\n" + "\n".join(f"  {v}" for v in stray)
    elif expect and not any(expect in v for v in violations):
        return f"問題はあったが、'{expect}' を含む行がない: {name}\n" + "\n".join(f"  {v}" for v in violations)
    return None


def read_expect(folder):
    # bad_* の expect.txt の、最初の中身の行 (空の行と # の行を除く)。なければ None。
    path = folder / "expect.txt"
    if not path.exists():
        return None
    for raw in path.read_text(encoding="utf-8-sig").splitlines():
        line = raw.strip()
        if line and not line.startswith("#"):
            return line
    return None


def selftest_block(rules, stats, spacing_patterns=None):
    # 1 つのセットのセルフテスト。失敗の文の並びを返し、数を stats に足す。spacing_patterns は空白のチェックの語 (省けば
    # 実際の一覧と ja_style.txt。仕組みの例のセットはテスト用の一覧で実行する)。
    failures = []
    good_dir = REPO_ROOT / rules.fixture
    ini_name = Path(rules.ini).name
    guide_name = fixture_guide_name(rules)
    good_guide = good_dir / guide_name
    good_ini = good_dir / ini_name
    if not good_guide.exists() or not good_ini.exists():
        return [f"OK 例がない: {to_relpath(good_dir)} ({guide_name} と {ini_name} が必要)"]

    cases = [(good_guide, good_ini, None)]
    bad_dirs = sorted(p for p in good_dir.parent.iterdir() if p.is_dir() and p.name.startswith("bad_"))
    if not bad_dirs:
        failures.append(f"{to_relpath(good_dir)} の隣に bad_* の NG 例がない")
    for d in bad_dirs:
        expect = read_expect(d)
        if expect is None:
            failures.append(f"{to_relpath(d)}: expect.txt がないか空")
            continue
        ini = d / ini_name
        cases.append((d / guide_name, ini if ini.exists() else good_ini, expect))
    for guide, ini, expect in cases:
        failure = judge(to_relpath(guide), check_files(guide, ini, rules, spacing_patterns), expect, every=True)
        if failure:
            failures.append(failure)
    stats["fixtures"] += len(cases)

    good_text = read_guide(good_guide)
    ini_entries = read_ini_values(good_ini.read_text(encoding="utf-8-sig"))
    # ほかのツールが節を探す関数: OK 例の手順書から、rules の role の見出しの節を取れる
    sections = sections_by_heading(good_text)
    for role, heading in rules.roles.items():
        if not sections.get(heading, "").strip():
            failures.append(f"sections_by_heading で、{rules.fixture} の role '{role}' の節 ('{heading}') が取れない")
    try:
        mutations = build_mutations(good_text, ini_entries, rules, stats,
                                    spacing_word_patterns(rules) if spacing_patterns is None else spacing_patterns)
    except AssertionError as e:
        return failures + [str(e)]
    for name, text, expect, ini in mutations:
        found = check_guide(name, text, ini_entries if ini is None else ini, rules, good_dir, spacing_patterns)
        failure = judge(f"{rules.target} {name}", found, expect)
        if failure:
            failures.append(failure)
    stats["mutations"] += len(mutations)

    # target_optional の扱い: OK 例を target にしたセットで、ファイルの有無と target_optional の有無の 4 通り
    probe = copy.copy(rules)
    probe.ini = f"{rules.fixture}/{ini_name}"
    for target, optional, want_violation, want_note in (
        (f"{rules.fixture}/{guide_name}", False, None, False),
        (f"{rules.fixture}/{guide_name}", True, "target_optional", False),
        (f"{rules.fixture}/no-such-guide", True, None, True),
        (f"{rules.fixture}/no-such-guide", False, "ファイルがない", False),
    ):
        probe.target = target
        probe.target_optional = optional
        violations, notes = check_target(probe, spacing_patterns=spacing_patterns)
        got_ok = (not violations) if want_violation is None else any(want_violation in v for v in violations)
        if not got_ok or bool(notes) != want_note:
            failures.append(f"check_target({target}, target_optional={optional}): {violations} {notes}")
        stats["targets"] += 1
    return failures


def parity_selftest(blocks, stats):
    # 日本語と英語の構成の比べ方: rules のセットの OK 例どうしは同じ構成で、片方の ### を 1 つ消す、番号付きの項目を
    # 1 つ足す、## を 1 つ消すと問題になる。
    failures = []
    docs = []
    for block in blocks:
        path = REPO_ROOT / block.fixture / fixture_guide_name(block)
        if not path.exists():
            return [f"構成の比べ方: OK 例がない: {to_relpath(path)}"]
        docs.append((block.fixture, read_guide(path)))
    if len(docs) < 2:
        return ["構成の比べ方: rules のセットが 2 つない"]
    base = [(label, parse_markdown(text)) for label, text in docs]
    if parity_problems(base):
        failures.append(f"構成の比べ方: OK 例どうしで問題があった: {parity_problems(base)}")
    label, text = docs[1]
    lines = text.split("\n")
    sub = next(i for i, line in enumerate(lines) if line.startswith("### "))
    number = next(i for i, line in enumerate(lines) if re.match(r"^[0-9]+\. ", line))
    section = next(i for i, line in enumerate(lines) if line.startswith("## "))
    for what, changed in (
        ("### を消す", lines[:sub] + lines[sub + 1:]),
        ("番号付きの項目を足す", lines[:number + 1] + [f"{int(lines[number].split('.')[0]) + 1}. extra"] + lines[number + 1:]),
        ("## を消す", lines[:section] + lines[section + 1:]),
    ):
        found = parity_problems([base[0], (label, parse_markdown("\n".join(changed)))])
        if not found:
            failures.append(f"構成の比べ方: {what}と問題になるはずが問題がなかった")
        stats["parity"] += 1
    return failures


def anchor_selftest():
    # GitHub のアンカーの作り方の例 (github-slugger と同じ結果になる例)
    failures = []
    for headings, want in (
        (["2. cordmuxをOBSのプラグインのフォルダーに入れる"], ["2-cordmuxをobsのプラグインのフォルダーに入れる"]),
        (["ドックの上部に「サブ」の行がない"], ["ドックの上部にサブの行がない"]),
        (["[ツール] メニューに [cordmuxをアンインストール] がない"], ["ツール-メニューに-cordmuxをアンインストール-がない"]),
        (["The dock doesn't appear"], ["the-dock-doesnt-appear"]),
        (["OBS・Vesktop (ベータ版)"], ["obsvesktop-ベータ版"]),
        (["必要なもの", "必要なもの", "必要なもの-1"], ["必要なもの", "必要なもの-1", "必要なもの-1-1"]),
        (["a_b-c d"], ["a_b-c-d"]),
    ):
        got = github_anchors(headings)
        if got != want:
            failures.append(f"github_anchors({headings}) は {want} のはずが {got}")
    return failures


# rules の読み方のセルフテスト。テスト用の rules は、空白を含む英語の見出しで書く (英語の見出しの読み方も確認するため)。
# 正しい rules が読めて各キーの値がそのとおりになり、壊した rules がどれも ValueError になることを確認する。
SAMPLE_LB = "\u3010"
SAMPLE_RB = "\u3011"
SAMPLE_CONTAINS_SECTION = "Introduction"
SAMPLE_CONTAINS_TEXT = "There is nothing for them to install or set up."
SAMPLE_PLACEHOLDER = "(to be written after checking)"
SAMPLE_RULES_LINES = [
    "target: sample/guide-sample.txt",
    "target_optional: yes",
    "ini: sample/en-US.ini",
    "zip_name: README-sample.txt",
    "title: Sample Guide",
    "fixture: sample/en/good",
    "heading: Introduction",
    "heading: What you need",
    "heading: Removing",
    "heading: About maintenance",
    f"procedure: {SAMPLE_LB}What you need{SAMPLE_RB} {SAMPLE_LB}Removing{SAMPLE_RB}",
    f"max_steps: {SAMPLE_LB}Removing{SAMPLE_RB} 4",
    f"role: {SAMPLE_LB}About maintenance{SAMPLE_RB} maintenance",
    f"role: {SAMPLE_LB}Removing{SAMPLE_RB} update",
    f"placeholder: {SAMPLE_PLACEHOLDER}",
    "forbidden_all: have them",
    f"contains: {SAMPLE_LB}{SAMPLE_CONTAINS_SECTION}{SAMPLE_RB} {SAMPLE_CONTAINS_TEXT}",
    f"copied: {SAMPLE_LB}Removing{SAMPLE_RB} cordmux.dock.dialog.uninstall_before",
    'ini_ref: (?:step (?P<step>[0-9]+) of )?"(?P<heading>[^"]+)" in {zip_name}',
]


def sample_rules_text(drop=(), add=()):
    # テスト用の rules の文字列。drop は消す行の先頭、add は追加する行。
    lines = [line for line in SAMPLE_RULES_LINES if not any(line.startswith(d) for d in drop)]
    return "\n".join(lines + list(add)) + "\n"


def rules_parse_selftest():
    # (失敗の文の並び, 確認した rules の数) を返す。
    failures = []
    try:
        blocks = parse_rules(sample_rules_text(), "sample")
        block = blocks[0]
        expected = {
            "roles": {"maintenance": "About maintenance", "update": "Removing"},
            "placeholder": SAMPLE_PLACEHOLDER,
            "contains": [(SAMPLE_CONTAINS_SECTION, SAMPLE_CONTAINS_TEXT)],
            "copied": [("Removing", "cordmux.dock.dialog.uninstall_before")],
            "target_optional": True,
            "fixture": "sample/en/good",
            "zip_name": "README-sample.txt",
            "title": "Sample Guide",
            "max_steps": [("Removing", 4)],
            "procedure_ranges": [("What you need", "Removing")],
        }
        for attr, want in expected.items():
            if getattr(block, attr) != want:
                failures.append(f"テスト用の rules: {attr} が {want!r} のはずが {getattr(block, attr)!r}")
        if [source for source, _ in block.forbidden_all] != ["have them"]:
            failures.append(f"テスト用の rules: forbidden_all が違う: {block.forbidden_all!r}")
        ref = block.ini_refs[0][1].search('Repeat step 2 of "Removing" in README-sample.txt.')
        if not ref or ref.group("heading") != "Removing" or ref.group("step") != "2":
            failures.append("テスト用の rules: ini_ref の {zip_name} を置き換えた形で読めない")
        new_keys = parse_rules(sample_rules_text(add=[
            f"procedure: {SAMPLE_LB}Introduction{SAMPLE_RB} {SAMPLE_LB}Introduction{SAMPLE_RB}",
            'quote_check: ""',
            "outside_text: Extract All",
            "component_word: check box check",
            "component_word: page page,menu",
            "outside_button: Copy User ID",
            f"item_titles: {SAMPLE_LB}Removing{SAMPLE_RB}",
            "ini_term: cordmux in Vesktop",
        ]), "sample-new")[0]
        if new_keys.ini_terms != ["cordmux in Vesktop"]:
            failures.append(f"テスト用の rules: ini_terms が違う: {new_keys.ini_terms!r}")
        got = (new_keys.procedure_ranges, new_keys.quote_check, new_keys.outside_text,
               new_keys.component_words, new_keys.outside_buttons, new_keys.roles.get("update"),
               new_keys.procedure_names(), new_keys.item_title_sections)
        want = ([("What you need", "Removing"), ("Introduction", "Introduction")], '""', ["Extract All"],
                [("check box", ("check",)), ("page", ("page", "menu"))], ["Copy User ID"], "Removing",
                ["What you need", "Removing", "Introduction"], ["Removing"])
        if got != want:
            failures.append(f"テスト用の rules (仕組みのキー): {want!r} のはずが {got!r}")
    except ValueError as e:
        failures.append(f"テスト用の rules: 読めるはずが {e}")

    role_line = f"role: {SAMPLE_LB}About maintenance{SAMPLE_RB} maintenance"
    broken = [
        ("drop_role", sample_rules_text(drop=[role_line])),
        ("role_twice", sample_rules_text(add=[f"role: {SAMPLE_LB}Introduction{SAMPLE_RB} maintenance"])),
        ("unknown_role", sample_rules_text(add=[f"role: {SAMPLE_LB}Introduction{SAMPLE_RB} notes"])),
        ("role_not_heading", sample_rules_text(
            drop=[role_line], add=[f"role: {SAMPLE_LB}Nowhere{SAMPLE_RB} maintenance"])),
        ("no_placeholder", sample_rules_text(drop=["placeholder:"])),
        ("placeholder_twice", sample_rules_text(add=["placeholder: (again)"])),
        ("no_zip_name", sample_rules_text(drop=["zip_name:"])),
        ("no_title", sample_rules_text(drop=["title:"])),
        ("title_twice", sample_rules_text(add=["title: Other"])),
        ("no_procedure", sample_rules_text(drop=["procedure:"])),
        ("heading_twice", sample_rules_text(add=["heading: Removing"])),
        ("copied_without_brackets", sample_rules_text(
            drop=["copied:"], add=["copied: Removing cordmux.dock.dialog.uninstall_before"])),
        ("contains_not_heading", sample_rules_text(add=[f"contains: {SAMPLE_LB}Nowhere{SAMPLE_RB} text"])),
        ("contains_without_text", sample_rules_text(add=[f"contains: {SAMPLE_LB}Removing{SAMPLE_RB}"])),
        ("target_optional_not_yes", sample_rules_text(drop=["target_optional:"], add=["target_optional: no"])),
        ("fixture_twice", sample_rules_text(add=["fixture: sample/other"])),
        ("no_fixture", sample_rules_text(drop=["fixture:"])),
        ("bad_regex_forbidden_all", sample_rules_text(add=["forbidden_all: (unclosed"])),
        ("bad_regex_forbidden", sample_rules_text(add=["forbidden: [unclosed"])),
        ("unknown_key", sample_rules_text(add=["fixtures: sample/en/good"])),
        ("removed_key_heading_style", sample_rules_text(add=["heading_style: underline"])),
        ("removed_key_max_columns", sample_rules_text(add=["max_columns: 80"])),
        ("procedure_range_not_heading", sample_rules_text(add=[f"procedure: {SAMPLE_LB}Nowhere{SAMPLE_RB} {SAMPLE_LB}Removing{SAMPLE_RB}"])),
        ("procedure_range_backwards", sample_rules_text(add=[f"procedure: {SAMPLE_LB}Removing{SAMPLE_RB} {SAMPLE_LB}What you need{SAMPLE_RB}"])),
        ("procedure_range_one_heading", sample_rules_text(add=[f"procedure: {SAMPLE_LB}Removing{SAMPLE_RB}"])),
        ("max_steps_not_number", sample_rules_text(add=[f"max_steps: {SAMPLE_LB}Removing{SAMPLE_RB} four"])),
        ("max_steps_without_section", sample_rules_text(add=["max_steps: 4"])),
        ("max_steps_not_heading", sample_rules_text(add=[f"max_steps: {SAMPLE_LB}Nowhere{SAMPLE_RB} 4"])),
        ("quote_check_one_char", sample_rules_text(add=['quote_check: "'])),
        ("component_word_unknown_kind", sample_rules_text(add=["component_word: page pages"])),
        ("component_word_no_kind", sample_rules_text(add=["component_word: page"])),
        ("update_role_twice", sample_rules_text(add=[f"role: {SAMPLE_LB}Introduction{SAMPLE_RB} update"])),
        ("item_titles_not_heading", sample_rules_text(add=[f"item_titles: {SAMPLE_LB}Nowhere{SAMPLE_RB}"])),
        ("item_titles_without_brackets", sample_rules_text(add=["item_titles: Removing"])),
        ("drop_update_role", sample_rules_text(drop=[f"role: {SAMPLE_LB}Removing{SAMPLE_RB} update"])),
        ("ini_ref_without_zip_name", sample_rules_text(add=['ini_ref: "(?P<heading>[^"]+)" in README.txt'])),
        ("ini_ref_without_heading", sample_rules_text(add=['ini_ref: "([^"]+)" in {zip_name}'])),
        ("ini_ref_bad_regex", sample_rules_text(add=['ini_ref: ("(?P<heading>[^"]+)" in {zip_name}'])),
        ("zip_name_twice_in_file", sample_rules_text() + sample_rules_text(drop=["target:"], add=["target: sample/guide-other.txt"])),
    ]
    for label, text in broken:
        try:
            parse_rules(text, label)
            failures.append(f"テスト用の rules ({label}): ValueError のはずが読めた")
        except ValueError:
            pass
        except re.error as e:
            failures.append(f"テスト用の rules ({label}): 正規表現のエラーが、読めない rules として報告されなかった: {e}")
    return failures, 1 + len(broken)


def judge_selftest():
    # NG 例の判定のセルフテスト: 別の理由の問題が 1 つでも混ざれば失敗、問題がなくても失敗、全部が期待の文字列を
    # 含めば成功。
    failures = []
    for violations, every, want_pass in (
        (["a: 手順の部分の禁止語: x", "a: 見出し 'y' が 0 回ある (1 回のはず)"], True, False),
        (["a: 手順の部分の禁止語: x", "a:2: 手順の部分の禁止語: z"], True, True),
        ([], True, False),
        (["a: 見出し 'y' が 0 回ある (1 回のはず)"], False, False),
    ):
        passed = judge("probe", violations, "手順の部分の禁止語", every=every) is None
        if passed != want_pass:
            failures.append(f"judge({violations}, every={every}) の結果が {want_pass} のはずが {passed}")
    return failures


def select_selftest(blocks):
    # --guide のセットの選び方: 手順書の名前でも ini の名前でも、そのセットに決まる。どちらでもない名前では決まらない。
    failures = []
    for block in blocks:
        try:
            by_guide = select_block(blocks, Path("x") / Path(block.target).name)
            by_ini = select_block(blocks, Path("x") / "unnamed", Path("x") / Path(block.ini).name)
        except ValueError as e:
            failures.append(f"select_block: {e}")
            continue
        if by_guide is not block or by_ini is not block:
            failures.append(f"select_block が {block.target} のセットを選ばない")
    try:
        select_block(blocks, Path("x") / "unnamed")
        failures.append("select_block が、--ini のない知らない名前の手順書にセットを選んだ")
    except ValueError:
        pass
    return failures


def run_selftest():
    try:
        blocks = load_rules()
    except (OSError, ValueError) as e:
        print(f"selftest 失敗: rules を読めない: {e}")
        return 1
    failures, parse_cases = rules_parse_selftest()
    failures += select_selftest(blocks)
    failures += judge_selftest()
    failures += anchor_selftest()
    stats = {"fixtures": 0, "mutations": 0, "targets": 0, "masked": 0, "internal_samples": 0, "ellipsis": 0,
             "component": 0, "quote": 0, "nested_quote": 0, "ranges": 0, "spacing": 0, "item_titles": 0,
             "max_steps": 0, "ini_refs": 0, "ini_terms": 0, "anchors": 0, "step_headings": 0, "parity": 0}
    for rules in blocks:
        failures += [f"[{rules.target}] {f}" for f in selftest_block(rules, stats)]
    failures += parity_selftest(blocks, stats)
    # rules のキーで入れる仕組みの例のセット (fixtures/install_guide/mechanism/rules.txt)。実際の rules とは別の組み合わせ
    # (空白の書き方が spacing=A のときなど) も確認するため、テスト用の rules で実行する。空白のチェックの語は、
    # forbidden_ui_words のテスト用の一覧 (spacing のセットに spacing=A の行を持つ) から取る。
    try:
        mechanism = load_rules(MECHANISM_RULES_FILE)
        mechanism_words = forbidden_ui_words.load_word_rules(MECHANISM_WORDS_FILE)
    except (OSError, ValueError) as e:
        mechanism = []
        failures.append(f"仕組みの例の rules か、語の一覧を読めない: {e}")
    mechanism_style = {"spacing": "A", "question": "halfwidth", "name_keys": [], "suffix_keys": []}
    for rules in mechanism:
        patterns = spacing_word_patterns(rules, mechanism_words, mechanism_style)
        failures += [f"[{rules.target}] {f}" for f in selftest_block(rules, stats, patterns)]
    # どれかのセットで 1 回は確認していなければならないもの (どのセットの例にもないと、その仕組みを確認しないまま
    # 成功するため)
    for key, what in (
        ("masked", "禁止語に一致するボタンの名前 ([ ] を伏せる処理)"),
        ("internal_samples", "サンプルに使える forbidden_ui_words.txt の語"),
        ("ellipsis", "末尾が「...」のボタンの値"),
        ("component", "[ ] の後ろの UI 要素の種類の語"),
        ("quote", "quote_check"),
        ("nested_quote", "入れ子の引用 (「」)"),
        ("ranges", "2 つ目の手順の部分の範囲"),
        ("spacing", "手順書の空白の語"),
        ("item_titles", "item_titles (項目を ### で書く節)"),
        ("max_steps", "max_steps (手順の数の上限)"),
        ("ini_refs", "ini_ref (画面の文言から手順書への参照)"),
        ("ini_terms", "ini_term (ini の値の中で使う語)"),
        ("anchors", "アンカーへのリンク"),
        ("step_headings", "### N. の手順の見出し"),
        ("parity", "日本語と英語の構成の比べ方"),
    ):
        if stats[key] == 0:
            failures.append(f"{what} を確認した例がない")

    for failure in failures:
        print(f"selftest 失敗: {failure}")
    if failures:
        return 1
    print(
        f"selftest OK: install_guide (rules のセット {len(blocks)} と仕組みの例のセット {len(mechanism)}、"
        f"NG 例と OK 例 {stats['fixtures']}、"
        f"書き換えた例 {stats['mutations']}、target の扱い {stats['targets']}、構成の比べ方 {stats['parity']}、"
        f"rules の例 {parse_cases})"
    )
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="手順書 (Markdown) をチェックする (記法、見出し、語、[ ] の名前、引用、定型の文、画面の文言からの参照など)")
    parser.add_argument("--guide", help="rules の target の代わりにチェックする手順書")
    parser.add_argument("--ini", help="--guide と一緒に使う ini (既定: 選んだセットの ini)")
    parser.add_argument("--selftest", action="store_true", help="NG 例と OK 例でセルフテストを実行する")
    args = parser.parse_args(argv)

    if args.selftest:
        return run_selftest()

    try:
        blocks = load_rules()
    except (OSError, ValueError) as e:
        print(f"NG: rules を読めない: {e}")
        return 1

    violations = []
    notes = []
    if args.guide:
        guide = Path(args.guide).resolve()
        ini = Path(args.ini).resolve() if args.ini else None
        try:
            rules = select_block(blocks, guide, ini)
        except ValueError as e:
            print(f"NG: {e}")
            return 1
        violations = check_files(guide, ini or (REPO_ROOT / rules.ini), rules)
    else:
        for rules in blocks:
            found, found_notes = check_target(rules)
            violations += found
            notes += found_notes
        # 日本語と英語の構成 (全部の target があるときだけ)
        present = [(rules.target, REPO_ROOT / rules.target) for rules in blocks]
        if all(path.exists() for _, path in present):
            violations += parity_problems([(label, parse_markdown(read_guide(path))) for label, path in present])
        else:
            notes.append("NOTE: target がそろっていないので、日本語と英語の構成は比べなかった")

    for v in violations:
        print(v)
    for note in notes:
        print(note)
    if violations:
        print(f"NG: 手順書の問題が {len(violations)} 件ある")
        return 1
    print("OK: 手順書に問題はない")
    return 0


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass
    sys.exit(main())
