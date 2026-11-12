#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# 英語だけのコメントを検出する。英字の語 (2 文字以上) が 3 つ以上あるコメントに、ひらがな、カタカナ、漢字が 1 文字も
# なければエラーにする。続けて書いた 1 行のコメントは、1 つのコメントとして数える (group_comments)。例外は is_exempt に
# ある (URL だけのコメント、SPDX の行とライセンスの表記、ツールへの指示、シェバン行など)。コメントを読めるのは、拡張子が
# EXTENSIONS のファイルだけである (ほかのファイルは、指定してもコードとして読むので、何も検出しない)。既定の対象からは、
# test/lint/vendored.txt のファイルと test/lint/fixtures を除く。
#
#   python comments_japanese.py            src-obs、src-vencord、test をチェックする
#   python comments_japanese.py PATH...     指定したファイルとフォルダーだけをチェックする
#   python comments_japanese.py --selftest  NG 例と OK 例でセルフテストを実行する

import argparse
import fnmatch
import os
import re
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
FIXTURES_DIR = SCRIPT_DIR / "fixtures"
VENDORED_FILE = SCRIPT_DIR / "vendored.txt"

DEFAULT_ROOTS = ["src-obs", "src-vencord", "test"]
# windows_api_confinement.py と同じ 9 種類に、.py と .ps1 を加えたもの。
EXTENSIONS = {".h", ".hpp", ".c", ".cc", ".cpp", ".ts", ".tsx", ".mjs", ".js", ".py", ".ps1"}
PRUNE_DIRS = {
    ".git", ".deps", "node_modules", "build", "build_x64", "release",
    "dist", "coverage", "__pycache__", ".venv", "out",
}

JAPANESE_RE = re.compile(r"[぀-ヿ一-鿿]")
WORD_RE = re.compile(r"[A-Za-z]{2,}")
URL_ONLY_RE = re.compile(r"^https?://\S+$")
DIRECTIVE_MARKERS = ("nolint", "clang-format", "eslint", "@ts-", "prettier", "noqa")
LICENSE_MARKERS = ("spdx", "license", "copyright")


# ---- コメントと文字列の切り分け (生文字列リテラルと、// を含む URL の文字列を、コメントと読み違えない) ----

def tokenize_c_like(text):
    tokens = []
    i = 0
    n = len(text)
    start = 0

    def flush(end):
        if end > start:
            tokens.append(("code", text[start:end]))

    while i < n:
        c = text[i]
        if c == "R" and i + 1 < n and text[i + 1] == '"':
            prev = text[i - 1] if i > 0 else ""
            if not (prev.isalnum() or prev == "_"):
                j = i + 2
                delim_start = j
                while j < n and text[j] != "(" and (j - delim_start) <= 16:
                    j += 1
                if j < n and text[j] == "(":
                    delim = text[delim_start:j]
                    closer = ")" + delim + '"'
                    end_idx = text.find(closer, j + 1)
                    end_idx = n if end_idx == -1 else end_idx + len(closer)
                    flush(i)
                    tokens.append(("string", text[i:end_idx]))
                    i = end_idx
                    start = i
                    continue
        if text[i:i + 2] == "//":
            flush(i)
            j = text.find("\n", i)
            j = n if j == -1 else j
            tokens.append(("comment", text[i:j]))
            i = j
            start = i
            continue
        if text[i:i + 2] == "/*":
            flush(i)
            j = text.find("*/", i + 2)
            j = n if j == -1 else j + 2
            tokens.append(("comment", text[i:j]))
            i = j
            start = i
            continue
        if c in ('"', "'", "`"):
            flush(i)
            quote = c
            j = i + 1
            while j < n:
                if text[j] == "\\" and j + 1 < n:
                    j += 2
                    continue
                if text[j] == quote:
                    j += 1
                    break
                j += 1
            tokens.append(("string", text[i:j]))
            i = j
            start = i
            continue
        i += 1
    flush(n)
    return tokens


def tokenize_python(text):
    tokens = []
    i = 0
    n = len(text)
    start = 0

    def flush(end):
        if end > start:
            tokens.append(("code", text[start:end]))

    while i < n:
        c = text[i]
        if c == "#":
            flush(i)
            j = text.find("\n", i)
            j = n if j == -1 else j
            tokens.append(("comment", text[i:j]))
            i = j
            start = i
            continue
        triple = text[i:i + 3]
        if triple in ('"""', "'''"):
            flush(i)
            j = i + 3
            while j < n:
                if text[j] == "\\" and j + 1 < n:
                    j += 2
                    continue
                if text[j:j + 3] == triple:
                    j += 3
                    break
                j += 1
            tokens.append(("string", text[i:j]))
            i = j
            start = i
            continue
        if c in ('"', "'"):
            flush(i)
            j = i + 1
            while j < n:
                if text[j] == "\\" and j + 1 < n:
                    j += 2
                    continue
                if text[j] == c:
                    j += 1
                    break
                if text[j] == "\n":
                    j += 1
                    break
                j += 1
            tokens.append(("string", text[i:j]))
            i = j
            start = i
            continue
        i += 1
    flush(n)
    return tokens


def tokenize_powershell(text):
    tokens = []
    i = 0
    n = len(text)
    start = 0

    def flush(end):
        if end > start:
            tokens.append(("code", text[start:end]))

    while i < n:
        if text[i:i + 2] == "<#":
            flush(i)
            j = text.find("#>", i + 2)
            j = n if j == -1 else j + 2
            tokens.append(("comment", text[i:j]))
            i = j
            start = i
            continue
        c = text[i]
        if c == "#":
            flush(i)
            j = text.find("\n", i)
            j = n if j == -1 else j
            tokens.append(("comment", text[i:j]))
            i = j
            start = i
            continue
        if text[i:i + 2] in ("@\"", "@'"):
            flush(i)
            closer = "\"@" if text[i + 1] == '"' else "'@"
            j = text.find("\n" + closer, i + 2)
            end = n if j == -1 else j + 1 + len(closer)
            tokens.append(("string", text[i:end]))
            i = end
            start = i
            continue
        if c in ('"', "'"):
            flush(i)
            quote = c
            j = i + 1
            while j < n:
                if quote == '"' and text[j] == "`" and j + 1 < n:
                    j += 2
                    continue
                if text[j] == quote:
                    if j + 1 < n and text[j + 1] == quote:
                        j += 2
                        continue
                    j += 1
                    break
                j += 1
            tokens.append(("string", text[i:j]))
            i = j
            start = i
            continue
        i += 1
    flush(n)
    return tokens


def tokenize(text, ext):
    if ext in (".h", ".hpp", ".c", ".cc", ".cpp", ".ts", ".tsx", ".mjs", ".js"):
        return tokenize_c_like(text)
    if ext == ".py":
        return tokenize_python(text)
    if ext == ".ps1":
        return tokenize_powershell(text)
    return [("code", text)]


def strip_comment_markers(comment):
    c = comment.strip()
    if c.startswith("//"):
        c = c[2:]
    elif c.startswith("/*"):
        c = c[2:]
        if c.endswith("*/"):
            c = c[:-2]
    elif c.startswith("<#"):
        c = c[2:]
        if c.endswith("#>"):
            c = c[:-2]
    elif c.startswith("#"):
        c = c[1:]
    return c.strip()


NAMESPACE_CLOSE_RE = re.compile(r"^namespace\s+[A-Za-z_][A-Za-z0-9_:]*$")
INCLUDE_GUARD_RE = re.compile(r"^[A-Z_][A-Z0-9_]*$")


def _word_count(text):
    return len(WORD_RE.findall(text))


def _remainder_word_count(low_text, markers):
    """low_text (小文字にした文字列) から markers の文字列をすべて取り除き、残りの英字の語 (2 文字以上) の数を
    返す。SPDX の行のように、markers の語が大半を占める短いコメントだけを例外にするために使う。"""
    remainder = low_text
    for marker in markers:
        remainder = remainder.replace(marker, " ")
    return _word_count(remainder)


def is_exempt(comment, at_file_start):
    if at_file_start and comment.lstrip().startswith("#!"):
        return True
    inner = strip_comment_markers(comment)
    if inner == "":
        return True
    if URL_ONLY_RE.match(inner):
        return True
    low = inner.lower()
    # ライセンスの表記 (SPDX、license、copyright の語を含むもの)。語を含むだけでは例外にしない。
    # 「Copyright holder list and the reason we keep this structure around」のような英語の説明文まで例外に
    # しないように、語を取り除いた残りの語が 3 つより少ない (表記そのもので、説明文ではない) ときだけ例外にする。
    if any(marker in low for marker in LICENSE_MARKERS):
        if _remainder_word_count(low, LICENSE_MARKERS) < 3:
            return True
    # ツールへの指示 (NOLINT、clang-format、eslint、@ts-、prettier、noqa)。指示はコメントの先頭に書く
    # (`eslint-disable-next-line ...` など) ので、先頭が一致するときだけ例外にする。eslint の語を含むだけの
    # 英語の説明文は例外にしない。
    if any(low.startswith(marker) for marker in DIRECTIVE_MARKERS):
        return True
    # clang-format が FixNamespaceComments で書き足す namespace の終わりのコメント (`// namespace X::Y`) と、
    # #endif の後でインクルードガードの名前を繰り返すだけのコメントは、名前のコピーで説明文ではないので、例外にする。
    # 日本語に書き換えても、clang-format が元の形に書き直す。
    if NAMESPACE_CLOSE_RE.match(inner):
        return True
    if INCLUDE_GUARD_RE.match(inner):
        return True
    return False


# ---- vendored.txt ----

def load_vendored_patterns():
    patterns = []
    if VENDORED_FILE.exists():
        for line in VENDORED_FILE.read_text(encoding="utf-8-sig").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            patterns.append(line)
    return patterns


def is_vendored(relpath_posix, patterns):
    return any(fnmatch.fnmatch(relpath_posix, p) for p in patterns)


def to_relpath(path):
    try:
        return path.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def discover_default_files():
    patterns = load_vendored_patterns()
    fixtures_resolved = FIXTURES_DIR.resolve()
    files = []
    for root_name in DEFAULT_ROOTS:
        root = REPO_ROOT / root_name
        if not root.exists():
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirpath_p = Path(dirpath)
            dirnames[:] = [
                d for d in dirnames
                if d not in PRUNE_DIRS and (dirpath_p / d).resolve() != fixtures_resolved
            ]
            for fname in filenames:
                if os.path.splitext(fname)[1] not in EXTENSIONS:
                    continue
                fpath = dirpath_p / fname
                relpath = to_relpath(fpath)
                if is_vendored(relpath, patterns):
                    continue
                files.append(fpath)
    return sorted(files)


def collect_files(path):
    if path.is_file():
        return [path]
    patterns = load_vendored_patterns()
    result = []
    for dirpath, dirnames, filenames in os.walk(path):
        dirpath_p = Path(dirpath)
        dirnames[:] = [d for d in dirnames if d not in PRUNE_DIRS]
        for fname in filenames:
            if os.path.splitext(fname)[1] not in EXTENSIONS:
                continue
            fpath = dirpath_p / fname
            relpath = to_relpath(fpath)
            if is_vendored(relpath, patterns):
                continue
            result.append(fpath)
    return sorted(result)


def line_style(chunk):
    if chunk.startswith("//"):
        return "//"
    if chunk.startswith("#") and not chunk.startswith("<#"):
        return "#"
    return None  # ブロックのコメント (/* */ と <# #>) は、1 つずつ別のコメントとして扱う


def group_comments(tokens):
    """人が 1 つの説明として読む単位に、comment のトークンをまとめる。`//` か `#` の 1 行のコメントが、空行
    (2 つ以上の改行) とコードを挟まずに同じ書き方で続いていれば、1 つのコメントにつなぐ。折り返した続きの行だけが
    日本語を含まないときに、その行を検出しないためである。(つないだ文字列, 先頭の位置) のリストを返す。"""
    groups = []
    pending = None  # つないでいる途中のコメント。{"style", "texts": [...], "start": int} の辞書
    offset = 0
    for kind, chunk in tokens:
        if kind == "comment":
            style = line_style(chunk)
            if pending is not None and pending["style"] is not None and pending["style"] == style:
                pending["texts"].append(chunk)
            else:
                if pending is not None:
                    groups.append(pending)
                pending = {"style": style, "texts": [chunk], "start": offset}
        else:
            if pending is not None:
                # 空白 (改行が 1 つだけ) を挟んだ次の行に、同じ書き方のコメントが続く間はつなぐ。改行が 2 つ以上
                # (空行) か、コードを挟んだら、そこで区切る。
                if chunk.strip() != "" or chunk.count("\n") > 1 or pending["style"] is None:
                    groups.append(pending)
                    pending = None
        offset += len(chunk)
    if pending is not None:
        groups.append(pending)
    return [("\n".join(g["texts"]), g["start"]) for g in groups]


# ---- チェック ----

def check_file(path):
    try:
        text = path.read_text(encoding="utf-8-sig")
    except (UnicodeDecodeError, OSError) as exc:
        return [f"{to_relpath(path)}: 読み込めない ({exc})"]
    tokens = tokenize(text, path.suffix)
    relpath = to_relpath(path)
    violations = []
    for merged, start in group_comments(tokens):
        word_count = len(WORD_RE.findall(merged))
        has_japanese = bool(JAPANESE_RE.search(merged))
        if word_count >= 3 and not has_japanese and not is_exempt(merged, start == 0):
            line = text.count("\n", 0, start) + 1
            preview = merged.strip().splitlines()[0][:60]
            violations.append(
                f"{relpath}:{line}: 英語の語が {word_count} 個あり、日本語がないコメント: {preview!r}"
            )
    return violations


def check_files(paths):
    violations = []
    for p in paths:
        violations.extend(check_file(p))
    return violations


# ---- --selftest ----

SELFTEST_CASES = [
    (FIXTURES_DIR / "comments_japanese" / "bad.ts", False),
    (FIXTURES_DIR / "comments_japanese" / "bad_directive_prose.ts", False),
    (FIXTURES_DIR / "comments_japanese" / "bad_license_prose.ts", False),
    (FIXTURES_DIR / "comments_japanese" / "good.ts", True),
]


def run_selftest():
    bad_count = sum(1 for _, expect_clean in SELFTEST_CASES if not expect_clean)
    good_count = sum(1 for _, expect_clean in SELFTEST_CASES if expect_clean)
    if bad_count == 0 or good_count == 0:
        print("selftest 失敗: NG 例か OK 例がない")
        return 1
    ok = True
    for path, expect_clean in SELFTEST_CASES:
        if not path.exists():
            print(f"selftest 失敗: 例のファイルがない: {path}")
            ok = False
            continue
        violations = check_file(path)
        clean = len(violations) == 0
        if clean != expect_clean:
            label = "OK 例で検出された" if expect_clean else "NG 例で検出されない"
            print(f"selftest 失敗 ({label}): {path}")
            for v in violations:
                print(f"  {v}")
            ok = False
    if ok:
        print("selftest OK: comments_japanese")
        return 0
    return 1


def main(argv=None):
    parser = argparse.ArgumentParser(description="英語だけのコメントがないことを確認する")
    parser.add_argument("paths", nargs="*", help="チェックするファイルかフォルダー (省くと src-obs、src-vencord、test)")
    parser.add_argument("--selftest", action="store_true", help="NG 例と OK 例でセルフテストを実行する")
    args = parser.parse_args(argv)

    if args.selftest:
        return run_selftest()

    if args.paths:
        files = []
        for p in args.paths:
            files.extend(collect_files(Path(p)))
        files = sorted(set(files))
    else:
        files = discover_default_files()

    violations = check_files(files)
    for v in violations:
        print(v)
    if violations:
        print(f"NG: 日本語がないコメントが {len(violations)} 箇所ある")
        return 1
    print(f"OK: {len(files)} ファイルをチェックした (英語だけのコメントはない)")
    return 0


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass
    sys.exit(main())
