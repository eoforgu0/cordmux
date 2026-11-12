#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# 手順書 (guide/ja.md と guide/en.md。Markdown) を、配布用の zip に入れるテキスト (はじめに.txt と README-en.txt) にする。
# 変換の規則は convert のコメントと dist/README.md の「zip の中の手順書」にある。make_dist_zip.py は convert を import して
# zip を作るときに変換する。CI もローカルも同じ引数で実行できる。Markdown の読み方は test/lint/install_guide.py にあり、
# 手順書の lint と同じ読み方で読む。Python 3 の標準ライブラリだけを使う。
#
#   python guide_to_text.py <guide/ja.md> --label <label> [--cordmux-commit <40 桁>] [--out <テキスト>]
#   python guide_to_text.py --selftest

import argparse
import sys
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dist_common  # noqa: E402  (source_url と手順書の URL の決め方)
from dist_common import DistError  # noqa: E402

sys.path.insert(0, str(dist_common.REPO_ROOT / "test" / "lint"))
import install_guide  # noqa: E402  (Markdown の読み方と、使ってよい記法のチェック)

# タイトルの後ろに置く、整形された手順書の場所の案内と、zip 用の差し込みの印を置き換える行の先頭。
INTRO_SENTENCE = {
    "ja": "この説明書は、見出しと目次の付いた形で次の場所でも読めます。",
    "en": "You can also read this guide, with headings and a table of contents, at:",
}
SOURCE_LABEL = {
    "ja": "このバージョンのソースコード: ",
    "en": "Source code of this version: ",
}
# インラインのコード (パスとファイル名) を囲む引用符。テキストでも語の切れ目が分かるようにする。
CODE_QUOTES = {"ja": ("\u300c", "\u300d"), "en": ('"', '"')}



def inline_text(text: str, lang: str) -> str:
    # 行の中のリンクとインラインのコードを、テキストの形にする。同じファイルの見出しへのリンク (#...) とリポジトリの
    # 中のファイルへの相対リンクはテキストだけ、https のリンクは「テキスト (URL)」(テキストと URL が同じなら URL だけ)、
    # インラインのコードは ja は「」、en は "" で囲む。ja では、コードと日本語の文字の間の空白を 1 つ除く (Markdown では
    # コードの前後に空白を置いて読みやすくするが、「」で囲むと「に 「C:\...」 を」のように空白が浮くため)。
    q0, q1 = CODE_QUOTES[lang]
    parts = install_guide.split_inline(text)
    out = []
    for index, part in enumerate(parts):
        if part[0] == "code":
            out.append(q0 + part[1] + q1)
        elif part[0] == "link":
            label, target = part[1], part[2]
            if target.startswith("https://"):
                out.append(target if label == target else f"{label} ({target})")
            else:
                out.append(label)
        else:
            segment = part[1]
            if lang == "ja":
                if index > 0 and parts[index - 1][0] == "code" and segment[:1] == " " and segment[1:2] and not segment[1].isascii():
                    segment = segment[1:]
                if (index + 1 < len(parts) and parts[index + 1][0] == "code" and segment[-1:] == " "
                        and len(segment) > 1 and not segment[-2].isascii()):
                    segment = segment[:-1]
            out.append(segment)
    return "".join(out)


ITEM_PREFIX_KINDS = install_guide.ITEM_KINDS


def convert(md_text: str, lang: str, guide_url: str, source_url: str, where: str = "guide") -> str:
    # 手順書の Markdown を、zip に入れるテキストにする。規則:
    #   1. タイトル (# の行) の後ろに、空行と、整形された手順書の場所の案内の 1 文と、その URL (guide_url) の行を置く
    #   2. zip 用の差し込みの印の行を、「このバージョンのソースコード: <source_url>」の段落にする
    #   3. 見出しの # の印はそのまま残す (見出しの行は折り返さない)
    #   4. リンクとインラインのコードは inline_text の形にする
    #   5. 段落と項目は折り返さず、1 つを 1 行にする。Windows 11 のメモ帳は、既定でウィンドウの幅に合わせて折り返す。
    #      決まった桁で改行を入れると、ウィンドウがそれより狭いときに、行の途中に短い行ができて読みにくい
    #   6. 見出し、段落、リストの前後の空行を 1 行にそろえる (続けて書いた項目の間には空行を入れない)。末尾は改行 1 つ
    #   7. 出力に {{ か <!-- が残れば DistError
    # 手順書が使ってよい記法の外の書き方をしていれば (install_guide.syntax_problems)、変換せずに DistError にする。
    if lang not in INTRO_SENTENCE:
        raise DistError(f"{where}: 知らない言語 {lang!r} (使える言語: {', '.join(INTRO_SENTENCE)})")
    if md_text.startswith("\ufeff"):
        md_text = md_text[1:]
    if "\r" in md_text:
        raise DistError(f"{where}: CR がある (改行は LF にする)")
    doc = install_guide.parse_markdown(md_text)
    quote = "".join(CODE_QUOTES[lang])
    problems = install_guide.syntax_problems(doc, None, quote)
    if problems:
        shown = "; ".join(f"{no} 行目: {message}" for no, message in problems[:5])
        raise DistError(f"{where}: 手順書の記法の問題が {len(problems)} 件ある (install_guide.py で確かめる): {shown}")

    blocks: list[tuple[str, list[str], bool]] = []   # (種類, 行, 前に空行があったか)
    blank_before = False
    for line in doc.lines:
        kind = line.kind
        if kind == "blank":
            blank_before = True
            continue
        if kind in install_guide.HEADING_KINDS:
            blocks.append(("heading", [line.prefix + inline_text(line.text, lang)], blank_before))
            if kind == "title":
                intro = [INTRO_SENTENCE[lang], guide_url]
                blocks.append(("paragraph", intro, True))
        elif kind == "marker":
            blocks.append(("paragraph", [SOURCE_LABEL[lang] + source_url], blank_before))
        elif kind in ITEM_PREFIX_KINDS:
            prefix = line.prefix
            blocks.append(("item", [prefix + inline_text(line.text, lang)], blank_before))
        else:
            blocks.append(("paragraph", [inline_text(line.raw, lang)], blank_before))
        blank_before = False

    out: list[str] = []
    for index, (kind, lines, blank) in enumerate(blocks):
        if index > 0:
            previous = blocks[index - 1][0]
            if not (kind == "item" and previous == "item" and not blank):
                out.append("")
        out += lines
    text = "\n".join(out) + "\n"
    for leftover in (dist_common.FILL_OPEN, "<!--"):
        if leftover in text:
            raise DistError(f"{where}: 変換したテキストに '{leftover}' が残っている")
    return text


def lang_of_guide(path: Path) -> str:
    # 手順書の言語: rules のセットの target と同じパスならそのセットの言語、なければファイル名 (ja.md、en.md)。
    try:
        for block in install_guide.load_rules():
            if (dist_common.REPO_ROOT / block.target).resolve() == path.resolve():
                return install_guide.guide_lang(block)
    except (OSError, ValueError) as exc:
        raise DistError(f"手順書の lint の rules: {exc}") from exc
    return path.stem


def convert_file(path: Path, label: str, cordmux_commit: str, repo_path: Optional[str] = None,
                 lang: Optional[str] = None) -> str:
    # path の手順書を変換する。repo_path は手順書のリポジトリの中のパス (手順書の URL に使う。省けば path から作る)。
    if repo_path is None:
        try:
            repo_path = path.resolve().relative_to(dist_common.REPO_ROOT).as_posix()
        except ValueError:
            raise DistError(f"{path} はリポジトリの外にある (--repo-path で、リポジトリの中のパスを指定する)")
    lang = lang or lang_of_guide(path)
    text = path.read_bytes().decode("utf-8")
    return convert(text, lang, dist_common.guide_url_for(label, cordmux_commit, repo_path),
                   dist_common.source_url_for(label, cordmux_commit), where=path.name)


# ---- セルフテスト ----------------------------------------------------------------

SAMPLE_GUIDE_URL = "https://github.com/eoforgu0/cordmux/blob/v1.0.0/guide/ja.md"
SAMPLE_SOURCE_URL = "https://github.com/eoforgu0/cordmux/releases/tag/v1.0.0"


def sample_md(body: str, title: str = "# Title") -> str:
    return f"{title}\n\n{body}\n\n<!-- zip:source_url -->\n"


def selftest() -> int:
    failures: list[str] = []

    def check(label: str, got, want) -> None:
        if got != want:
            failures.append(f"{label}: {want!r} のはずが {got!r}")

    def lines_of(body: str, lang: str = "ja", title: str = "# Title") -> list[str]:
        return convert(sample_md(body, title), lang, SAMPLE_GUIDE_URL, SAMPLE_SOURCE_URL).split("\n")

    # 1. タイトルの後ろの案内と URL
    out = lines_of("本文です。")
    check("タイトルの後ろの案内", out[:5], ["# Title", "", INTRO_SENTENCE["ja"], SAMPLE_GUIDE_URL, ""])
    check("英語の案内", lines_of("Body.", "en")[2], INTRO_SENTENCE["en"])
    # 2. 差し込みの印の行
    text = "\n".join(out)
    check("差し込みの印が残らない", "<!--" in text, False)
    check("ソースコードの行 (案内と URL を 1 行に)", out[-2:], ["このバージョンのソースコード: " + SAMPLE_SOURCE_URL, ""])
    # 3. 見出しの # を残す
    out = lines_of("## 節\n\n### 項目\n\n本文です。")
    check("## と ### の見出し", [line for line in out if line.startswith("#")], ["# Title", "## 節", "### 項目"])
    # 4. リンクとインラインのコード
    check("見出しへのリンクはテキストだけ", lines_of("## 困ったとき\n\n「[困ったとき](#困ったとき)」を見ます。")[7], "「困ったとき」を見ます。")
    check("相対リンクはテキストだけ", lines_of("「[licenses](../dist/licenses)」にあります。")[5], "「licenses」にあります。")
    check("https のリンクはテキストと URL", lines_of("[公式サイト](https://vesktop.dev/)から入れます。")[5],
          "公式サイト (https://vesktop.dev/)から入れます。")
    check("テキストと URL が同じなら URL だけ", lines_of("[https://vesktop.dev/](https://vesktop.dev/) です。")[5],
          "https://vesktop.dev/ です。")
    check("ja のコードは「」(日本語との間の空白を除く)", lines_of("ここに `C:\\ProgramData` を入力します。")[5], "ここに「C:\\ProgramData」を入力します。")
    check("ja のコードと英字の間の空白は残す", lines_of("OBS `plugins` x")[5], "OBS 「plugins」 x")
    check("en のコードは \"\"", lines_of("Open `C:\\ProgramData`.", "en")[5], 'Open "C:\\ProgramData".')
    # 5. 折り返さない
    long_ja = "\u3042" * 100
    check("長い段落も 1 行", lines_of(long_ja)[5:7], [long_ja, ""])
    check("- の長い項目も 1 行", lines_of("- " + long_ja)[5:7], ["- " + long_ja, ""])
    check("1. の長い項目も 1 行", lines_of("1. " + long_ja)[5:7], ["1. " + long_ja, ""])
    check("入れ子の - は字下げを残す", lines_of("- a\n  - " + long_ja)[5:8], ["- a", "  - " + long_ja, ""])
    check("番号の下の - は字下げを残す", lines_of("1. a\n   - " + long_ja)[5:8], ["1. a", "   - " + long_ja, ""])
    url = "https://example.com/" + "a" * 70
    check("URL も同じ行", lines_of("\u3042" * 10 + url + "\u3044")[5:7], ["\u3042" * 10 + url + "\u3044", ""])
    words = "word " * 30 + "last"
    check("英語の長い段落も 1 行", lines_of(words, "en")[5:7], [words, ""])
    # 6. 空行
    out = lines_of("段落1。\n\n\n\n段落2。\n\n- a\n- b")
    check("空行は 1 行、続けて書いた項目の間に空行を入れない", out[5:10], ["段落1。", "", "段落2。", "", "- a"])
    check("項目の続き", out[10], "- b")
    check("末尾の改行は 1 つ", convert(sample_md("x"), "ja", SAMPLE_GUIDE_URL, SAMPLE_SOURCE_URL).endswith("\n\n"), False)
    check("見出しの直後の空行", lines_of("## 節\n本文。")[5:8], ["## 節", "", "本文。"])
    # 7. 記法の問題と、残った {{ と <!--
    for label, body in (("太字", "**x**"), ("差し込みの書式", "{{source_url}}"), ("手で折り返した段落", "a\nb"),
                        ("HTML のコメント", "<!-- x -->")):
        try:
            convert(sample_md(body), "ja", SAMPLE_GUIDE_URL, SAMPLE_SOURCE_URL)
            failures.append(f"{label}: 変換しないはずが変換した")
        except DistError:
            pass
    try:
        convert(sample_md("x"), "ja", "https://x/{{a}}", SAMPLE_SOURCE_URL)
        failures.append("出力に {{ が残る: 失敗のはずが変換した")
    except DistError:
        pass
    try:
        convert(sample_md("x"), "fr", SAMPLE_GUIDE_URL, SAMPLE_SOURCE_URL)
        failures.append("知らない言語: 失敗のはずが変換した")
    except DistError:
        pass
    # URL の決め方 (dist_common.guide_url_for)
    check("タグの手順書の URL", dist_common.guide_url_for("1.0.0", "", "guide/ja.md"), SAMPLE_GUIDE_URL)
    commit = "0123456789abcdef0123456789abcdef01234567"
    check("コミットの手順書の URL", dist_common.guide_url_for("1.0.0+0123456", commit, "guide/en.md"),
          f"https://github.com/eoforgu0/cordmux/blob/{commit}/guide/en.md")

    # 実際の手順書: 変換でき、Markdown の 1 行が 1 行になり (タイトルの後ろの案内の 2 行を除く)、## の節が Markdown と同じ
    checked = 0
    try:
        blocks = install_guide.load_rules()
    except (OSError, ValueError) as exc:
        blocks = []
        failures.append(f"手順書の lint の rules を読めない: {exc}")
    for block in blocks:
        path = dist_common.REPO_ROOT / block.target
        if not path.exists():
            failures.append(f"{block.target} がない")
            continue
        lang = install_guide.guide_lang(block)
        try:
            text = convert_file(path, "1.0.0+0123456", commit)
        except DistError as exc:
            failures.append(f"{block.target}: {exc}")
            continue
        md_text = path.read_bytes().decode("utf-8")
        md_lines = [line for line in md_text.split("\n") if line.strip()]
        text_lines = [line for line in text.split("\n") if line.strip()]
        if len(text_lines) != len(md_lines) + 2:
            failures.append(f"{block.target}: 変換したテキストの行の数 {len(text_lines)} が Markdown の行の数 {len(md_lines)} + 2 と違う")
        md_sections = list(install_guide.sections_by_heading(md_text))
        if list(install_guide.sections_by_heading(text)) != md_sections:
            failures.append(f"{block.target}: 変換したテキストの ## の節が Markdown と違う")
        checked += 1

    if failures:
        for failure in failures:
            print(f"selftest 失敗: {failure}", file=sys.stderr)
        return 1
    print(f"selftest OK: guide_to_text (変換の規則ごとの例と、実際の手順書 {checked} つ)")
    return 0


def main(argv: Optional[list[str]] = None) -> int:
    dist_common.use_utf8_console()
    parser = argparse.ArgumentParser(description="手順書 (Markdown) を、zip に入れるテキストにする")
    parser.add_argument("guide", nargs="?", help="手順書 (guide/ja.md など)")
    parser.add_argument("--label", help="zip の名前の label (1.0.0、1.0.0-rc1、1.0.0+abc1234)")
    parser.add_argument("--cordmux-commit", help="label に + があるときの cordmux のコミット (40 桁)")
    parser.add_argument("--repo-path", help="手順書のリポジトリの中のパス (既定: 手順書のパスから作る)")
    parser.add_argument("--out", help="書くテキストのパス (省けば標準出力)")
    parser.add_argument("--selftest", action="store_true", help="このツールのセルフテストを実行する")
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    if not args.guide or not args.label:
        parser.error("手順書と --label を指定する")
    try:
        text = convert_file(Path(args.guide), args.label, args.cordmux_commit or "", args.repo_path)
    except (DistError, OSError) as exc:
        dist_common.print_json({"guide": args.guide, "ok": False, "error": str(exc)})
        return 1
    if args.out:
        dist_common.write_text_lf(Path(args.out), text)
        dist_common.print_json({"guide": args.guide, "ok": True, "out": args.out, "lines": text.count("\n")})
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
