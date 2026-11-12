#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# コード (コメントと文字列を除いた部分) に ASCII でない文字がないかをチェックする。識別子は英語で付けるので、
# コードに残った ASCII でない文字は、識別子に日本語などを使った所である。チェックするのは拡張子が EXTENSIONS の
# ファイルだけで、既定の対象からは test/lint/vendored.txt のファイルと test/lint/fixtures を除く。
#
#   python identifiers_english.py            src-obs、src-vencord、test をチェックする
#   python identifiers_english.py PATH...     指定したファイルとフォルダーだけをチェックする
#   python identifiers_english.py --selftest  NG 例と OK 例でセルフテストを実行する

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

NON_ASCII_RE = re.compile(r"[^\x00-\x7F]")


# ---- コメントと文字列の切り分け (生文字列リテラルと、// を含む URL の文字列を、コメントと読み違えない) ----

def tokenize_c_like(text):
    """C と C++、TypeScript、JavaScript 用。(種類, 文字列) のリストを返す。種類は code、comment、string のどれか。"""
    tokens = []
    i = 0
    n = len(text)
    start = 0

    def flush(end):
        if end > start:
            tokens.append(("code", text[start:end]))

    while i < n:
        c = text[i]
        # 生文字列リテラル R"delim(...)delim"
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


def masked_text(text, tokens, keep_kinds):
    """tokens のうち、種類が keep_kinds でないものを空白に置き換える (行番号が変わらないように、改行は残す)。"""
    parts = []
    for kind, chunk in tokens:
        if kind in keep_kinds:
            parts.append(chunk)
        else:
            parts.append("".join(ch if ch == "\n" else " " for ch in chunk))
    return "".join(parts)


# ---- vendored.txt (外部からコピーしたファイルを除く) ----

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


# ---- ファイルの列挙 ----

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
        # 拡張子が EXTENSIONS にないファイルは、コメントの書き方が分からないのでチェックしない。チェックすると、コメントも
        # コードとして読むので、渡された .cmake の日本語のコメントを、識別子の文字として検出してしまう。
        return [path] if path.suffix in EXTENSIONS else []
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


# ---- チェック ----

def check_file(path):
    try:
        text = path.read_text(encoding="utf-8-sig")
    except (UnicodeDecodeError, OSError) as exc:
        return [f"{to_relpath(path)}: 読み込めない ({exc})"]
    tokens = tokenize(text, path.suffix)
    masked = masked_text(text, tokens, keep_kinds={"code"})
    violations = []
    relpath = to_relpath(path)
    for m in NON_ASCII_RE.finditer(masked):
        line = masked.count("\n", 0, m.start()) + 1
        ch = masked[m.start()]
        violations.append(
            f"{relpath}:{line}: コードに ASCII でない文字 U+{ord(ch):04X} ({ch!r}) がある"
        )
    return violations


def check_files(paths):
    violations = []
    for p in paths:
        violations.extend(check_file(p))
    return violations


# ---- --selftest ----

SELFTEST_CASES = [
    (FIXTURES_DIR / "identifiers_english" / "bad.cpp", False),
    (FIXTURES_DIR / "identifiers_english" / "good.cpp", True),
]


def run_selftest():
    ok = True
    bad_count = sum(1 for _, expect_clean in SELFTEST_CASES if not expect_clean)
    good_count = sum(1 for _, expect_clean in SELFTEST_CASES if expect_clean)
    if bad_count == 0 or good_count == 0:
        print("selftest 失敗: NG 例か OK 例がない")
        return 1
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
        print("selftest OK: identifiers_english")
        return 0
    return 1


def main(argv=None):
    parser = argparse.ArgumentParser(description="コード (コメントと文字列を除いた部分) に ASCII でない文字がないことを確認する")
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
        print(f"NG: コードに ASCII でない文字が {len(violations)} 箇所ある")
        return 1
    print(f"OK: {len(files)} ファイルをチェックした (コードに ASCII でない文字はない)")
    return 0


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass
    sys.exit(main())
