#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# src-obs のコードに、cordmux が使わない libobs の使い方 (forbidden_libobs_names.txt) がないかをチェックする。
# windows_api_confinement.py と違って許す場所はなく、どこに書いてもエラーにする。行ごとの理由は
# forbidden_libobs_names.txt のコメントにある。コメントの中はチェックせず、文字列の中はチェックする。既定の対象は
# src-obs の、拡張子が EXTENSIONS のファイルで、tests フォルダーと test/lint/vendored.txt のファイルを除く。
#
#   python forbidden_libobs_usage.py            src-obs をチェックする
#   python forbidden_libobs_usage.py PATH...     指定したファイルとフォルダーだけをチェックする
#   python forbidden_libobs_usage.py --selftest  NG 例と OK 例でセルフテストを実行する

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
NAMES_FILE = SCRIPT_DIR / "forbidden_libobs_names.txt"

DEFAULT_ROOTS = ["src-obs"]
EXTENSIONS = {".h", ".hpp", ".c", ".cc", ".cpp"}
PRUNE_DIRS = {
    ".git", ".deps", "node_modules", "build", "build_x64", "release",
    "dist", "coverage", "__pycache__", ".venv", "out", "tests",
}

REQUIRES_MARKER = "  REQUIRES: "


# ---- コメントだけを空白に置き換える (windows_api_confinement.py と同じ処理) ----

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
        # C++14 の桁区切り (2'000'000) の ' は文字リテラルの始まりではないので、直前と直後がどちらも数字のときは、
        # コードとして読み飛ばす。読み飛ばさないと、ファイルの最後までを閉じない文字列として読み、その後の名前を
        # 1 つも検出できない。直前だけを見ると `u8'x'` のような文字リテラルの ' も読み飛ばすので、直後も数字である
        # ことを条件にする。
        if c == "'" and i > 0 and text[i - 1].isdigit() and i + 1 < n and text[i + 1].isdigit():
            i += 1
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


def masked_text(text, tokens, keep_kinds):
    parts = []
    for kind, chunk in tokens:
        if kind in keep_kinds:
            parts.append(chunk)
        else:
            parts.append("".join(ch if ch == "\n" else " " for ch in chunk))
    return "".join(parts)


# ---- forbidden_libobs_names.txt (各行は正規表現。# で始まる行はコメント。行の末尾に REQUIRES: を書ける) ----

def load_name_specs():
    specs = []
    if NAMES_FILE.exists():
        for line in NAMES_FILE.read_text(encoding="utf-8-sig").splitlines():
            line = line.strip("\n")
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            if REQUIRES_MARKER in line:
                raw, requires_raw = line.split(REQUIRES_MARKER, 1)
                raw = raw.rstrip()
                requires_raw = requires_raw.strip()
                specs.append((raw, re.compile(raw), re.compile(requires_raw)))
            else:
                specs.append((stripped, re.compile(stripped), None))
    return specs


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
    vendored = load_vendored_patterns()
    files = []
    for root_name in DEFAULT_ROOTS:
        root = REPO_ROOT / root_name
        if not root.exists():
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirpath_p = Path(dirpath)
            dirnames[:] = [d for d in dirnames if d not in PRUNE_DIRS]
            for fname in filenames:
                if os.path.splitext(fname)[1] not in EXTENSIONS:
                    continue
                fpath = dirpath_p / fname
                relpath = to_relpath(fpath)
                if is_vendored(relpath, vendored):
                    continue
                files.append(fpath)
    return sorted(files)


def collect_files(path):
    if path.is_file():
        return [path]
    vendored = load_vendored_patterns()
    result = []
    for dirpath, dirnames, filenames in os.walk(path):
        dirpath_p = Path(dirpath)
        dirnames[:] = [d for d in dirnames if d not in PRUNE_DIRS]
        for fname in filenames:
            if os.path.splitext(fname)[1] not in EXTENSIONS:
                continue
            fpath = dirpath_p / fname
            relpath = to_relpath(fpath)
            if is_vendored(relpath, vendored):
                continue
            result.append(fpath)
    return sorted(result)


# ---- チェック ----

def check_file(path, name_specs):
    relpath = to_relpath(path)
    try:
        text = path.read_text(encoding="utf-8-sig")
    except (UnicodeDecodeError, OSError):
        return []
    tokens = tokenize_c_like(text)
    masked = masked_text(text, tokens, keep_kinds={"code", "string"})
    violations = []
    for raw, pattern, requires_pattern in name_specs:
        if requires_pattern is not None and requires_pattern.search(masked):
            # REQUIRES: のパターン (enum_active_sources など) が同じファイルにあるので、エラーにしない。
            continue
        for m in pattern.finditer(masked):
            line = masked.count("\n", 0, m.start()) + 1
            violations.append(
                f"{relpath}:{line}: 使わない libobs の使い方 `{raw}` (`{m.group(0)}`) がある"
            )
    return violations


def check_files(paths):
    name_specs = load_name_specs()
    violations = []
    for p in paths:
        violations.extend(check_file(p, name_specs))
    return violations


# ---- --selftest ----

LINT_FIXTURES_DIR = FIXTURES_DIR / "forbidden_libobs_usage"

SELFTEST_CASES = [
    (LINT_FIXTURES_DIR / "bad" / "src-obs" / "sources" / "audio_render_bad.cpp", False),
    (LINT_FIXTURES_DIR / "bad" / "src-obs" / "sources" / "set_volume_bad.cpp", False),
    (LINT_FIXTURES_DIR / "bad" / "src-obs" / "sources" / "set_muted_bad.cpp", False),
    (LINT_FIXTURES_DIR / "bad" / "src-obs" / "sources" / "active_child_only_bad.cpp", False),
    (LINT_FIXTURES_DIR / "bad" / "src-obs" / "sources" / "active_child_missing_enum_all_bad.cpp", False),
    (LINT_FIXTURES_DIR / "bad" / "src-obs" / "sources" / "digit_separator_bad.cpp", False),
    (LINT_FIXTURES_DIR / "good" / "src-obs" / "sources" / "mirror_good.cpp", True),
    (LINT_FIXTURES_DIR / "good" / "src-obs" / "config" / "read_only_good.cpp", True),
]


def run_selftest():
    bad_count = sum(1 for _, expect_clean in SELFTEST_CASES if not expect_clean)
    good_count = sum(1 for _, expect_clean in SELFTEST_CASES if expect_clean)
    if bad_count == 0 or good_count == 0:
        print("selftest 失敗: NG 例か OK 例がない")
        return 1
    name_specs = load_name_specs()
    ok = True
    for path, expect_clean in SELFTEST_CASES:
        if not path.exists():
            print(f"selftest 失敗: 例のファイルがない: {path}")
            ok = False
            continue
        violations = check_file(path, name_specs)
        clean = len(violations) == 0
        if clean != expect_clean:
            label = "OK 例で検出された" if expect_clean else "NG 例で検出されない"
            print(f"selftest 失敗 ({label}): {path}")
            for v in violations:
                print(f"  {v}")
            ok = False
    if ok:
        print("selftest OK: forbidden_libobs_usage")
        return 0
    return 1


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="cordmux が使わない libobs の使い方が src-obs にないことを確認する"
    )
    parser.add_argument("paths", nargs="*", help="チェックするファイルかフォルダー (省くと src-obs)")
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
        print(f"NG: 使わない libobs の使い方が {len(violations)} 箇所ある")
        return 1
    print(f"OK: {len(files)} ファイルをチェックした (使わない libobs の使い方はない)")
    return 0


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass
    sys.exit(main())
