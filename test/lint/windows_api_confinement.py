#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# Windows に固有の名前 (windows_names.txt) が、src-obs/platform と src-vencord/paths.ts の外にないかをチェックする。
# ほかの OS に広げるときに、追加する所を platform の実装だけにするためである。コメントの中はチェックせず、文字列の中は
# チェックする (%APPDATA% や "win32" は文字列の中に書くため)。既定の対象は src-obs と src-vencord の、拡張子が
# EXTENSIONS のファイルで、tests フォルダーと test/lint/vendored.txt のファイルを除く。場所はリポジトリのルートからの
# パスで判定し、paths.ts は src-vencord の直下の 1 つだけを許す。
#
#   python windows_api_confinement.py            src-obs と src-vencord をチェックする
#   python windows_api_confinement.py PATH...     指定したファイルとフォルダーだけをチェックする
#   python windows_api_confinement.py --selftest  NG 例と OK 例でセルフテストを実行する

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
NAMES_FILE = SCRIPT_DIR / "windows_names.txt"

DEFAULT_ROOTS = ["src-obs", "src-vencord"]
EXTENSIONS = {".h", ".hpp", ".c", ".cc", ".cpp", ".ts", ".tsx", ".mjs", ".js"}
PRUNE_DIRS = {
    ".git", ".deps", "node_modules", "build", "build_x64", "release",
    "dist", "coverage", "__pycache__", ".venv", "out", "tests",
}


# ---- コメントだけを空白に置き換える (文字列は残す。%APPDATA% や "win32" は文字列の中に書くため) ----

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


# ---- windows_names.txt (各行は正規表現。# で始まる行はコメント) ----

def load_name_patterns():
    patterns = []
    if NAMES_FILE.exists():
        for line in NAMES_FILE.read_text(encoding="utf-8-sig").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            patterns.append((line, re.compile(line)))
    return patterns


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


def to_relpath(path, root=REPO_ROOT):
    try:
        return path.resolve().relative_to(Path(root).resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def is_allowed_location(relpath_posix):
    if "src-obs/platform/" in relpath_posix:
        return True
    # paths.ts は、src-vencord の直下の 1 つだけを許す (深いフォルダーの paths.ts は許さない)。
    if relpath_posix == "src-vencord/paths.ts":
        return True
    return False


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

def find_violations(path, name_patterns, root=REPO_ROOT):
    """(一覧の行, 行番号, 一致した文字列) のリストを返す。許す場所のファイルなら空のリストを返す。"""
    relpath = to_relpath(path, root)
    if is_allowed_location(relpath):
        return []
    try:
        text = path.read_text(encoding="utf-8-sig")
    except (UnicodeDecodeError, OSError):
        return []
    tokens = tokenize_c_like(text)
    masked = masked_text(text, tokens, keep_kinds={"code", "string"})
    found = []
    for raw, pattern in name_patterns:
        for m in pattern.finditer(masked):
            line = masked.count("\n", 0, m.start()) + 1
            found.append((raw, line, m.group(0)))
    return found


def check_file(path, name_patterns, root=REPO_ROOT):
    relpath = to_relpath(path, root)
    return [
        f"{relpath}:{line}: Windows の名前 `{raw}` (`{text}`) が platform/ と paths.ts の外にある"
        for raw, line, text in find_violations(path, name_patterns, root)
    ]


def check_files(paths):
    name_patterns = load_name_patterns()
    violations = []
    for p in paths:
        violations.extend(check_file(p, name_patterns))
    return violations


# ---- --selftest ----

SELFTEST_BAD_ROOT = FIXTURES_DIR / "windows_api_confinement" / "bad"
SELFTEST_GOOD_ROOT = FIXTURES_DIR / "windows_api_confinement" / "good"

# NG 例のうち下の 2 つは、名前ごとに許す場所の外に書いた例である。ファイルで何かが検出されるだけでなく、ここに
# 挙げた名前 (windows_names.txt の行) がそれぞれ最低の回数だけ検出されることまで確認する。1 つの名前の行が一覧から
# 消えても、ほかの名前が検出されると、セルフテストが成功してしまうためである。
SELFTEST_EXPECTED_TS = {
    "['`]win32['`]": 2,  # 単引用符とテンプレート文字列の 2 か所
    r"navigator\.platform": 1,
    "userAgentData": 1,
    "USERPROFILE": 1,
}
SELFTEST_EXPECTED_CPP = {
    name: 1
    for name in (
        r"\bDeleteFile[AW]?\b", r"\bRemoveDirectory[AW]?\b", r"\bMoveFile(Ex|WithProgress)?[AW]?\b",
        "MOVEFILE_", r"\bReplaceFile[AW]?\b", r"\bCopyFile(2|Ex)?[AW]?\b", r"\bSHFileOperation[AW]?\b",
        r"\bFindFirstFile(Ex)?[AW]?\b", r"\bFindNextFile[AW]?\b", r"\bFindClose\b", "WIN32_FIND_DATA",
        "FindExInfo", "IO_REPARSE_TAG_", "FILE_ATTRIBUTE_", "FlushFileBuffers", "GetLastError",
        "SetFileAttributes", "RtlGetVersion", "OSVERSIONINFO", "RegGetValue", "RegOpenKey",
        "RegQueryValue", "ntdll", "GetModuleHandle", r"\.exe\b",
    )
}

# (例のファイル, 検出がないはずか, リポジトリのルートとみなすフォルダー, 名前ごとの最低の回数)
SELFTEST_CASES = [
    (SELFTEST_BAD_ROOT / "src-obs" / "ws" / "listener.cpp", False, SELFTEST_BAD_ROOT, {}),
    (SELFTEST_BAD_ROOT / "src-obs" / "ws" / "digit_separator_bad.cpp", False, SELFTEST_BAD_ROOT, {}),
    (SELFTEST_BAD_ROOT / "src-obs" / "install" / "remove_files.cpp", False, SELFTEST_BAD_ROOT,
     SELFTEST_EXPECTED_CPP),
    (SELFTEST_BAD_ROOT / "src-vencord" / "transport" / "platform_sniff.ts", False, SELFTEST_BAD_ROOT,
     SELFTEST_EXPECTED_TS),
    # src-vencord の直下でない paths.ts は許さない。
    (SELFTEST_BAD_ROOT / "src-vencord" / "transport" / "paths.ts", False, SELFTEST_BAD_ROOT,
     {'"win32"': 1}),
    (SELFTEST_GOOD_ROOT / "src-obs" / "platform" / "platform_win.cpp", True, SELFTEST_GOOD_ROOT, {}),
    # 単語境界を付けた行が、Windows と関係のない識別子 (CopyFileFn など) に一致しないこと。
    (SELFTEST_GOOD_ROOT / "src-obs" / "dock" / "copy_fn.cpp", True, SELFTEST_GOOD_ROOT, {}),
    (SELFTEST_GOOD_ROOT / "src-vencord" / "paths.ts", True, SELFTEST_GOOD_ROOT, {}),
]


def run_selftest():
    bad_count = sum(1 for _, expect_clean, _, _ in SELFTEST_CASES if not expect_clean)
    good_count = sum(1 for _, expect_clean, _, _ in SELFTEST_CASES if expect_clean)
    if bad_count == 0 or good_count == 0:
        print("selftest 失敗: NG 例か OK 例がない")
        return 1
    name_patterns = load_name_patterns()
    known_raws = {raw for raw, _ in name_patterns}
    ok = True
    for path, expect_clean, root, expected in SELFTEST_CASES:
        if not path.exists():
            print(f"selftest 失敗: 例のファイルがない: {path}")
            ok = False
            continue
        violations = check_file(path, name_patterns, root)
        clean = len(violations) == 0
        if clean != expect_clean:
            label = "OK 例で検出された" if expect_clean else "NG 例で検出されない"
            print(f"selftest 失敗 ({label}): {path}")
            for v in violations:
                print(f"  {v}")
            ok = False
        found = find_violations(path, name_patterns, root)
        for raw, minimum in expected.items():
            if raw not in known_raws:
                print(f"selftest 失敗: windows_names.txt に `{raw}` の行がない ({path.name} で検出するはずの名前)")
                ok = False
                continue
            count = sum(1 for r, _, _ in found if r == raw)
            if count < minimum:
                print(f"selftest 失敗: `{raw}` が {path.name} で {count} 回検出された (最低 {minimum} 回のはず)")
                ok = False
    if ok:
        print("selftest OK: windows_api_confinement")
        return 0
    return 1


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Windows に固有の名前が、src-obs/platform と src-vencord/paths.ts の外にないことを確認する"
    )
    parser.add_argument("paths", nargs="*", help="チェックするファイルかフォルダー (省くと src-obs と src-vencord)")
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
        print(f"NG: platform/ と paths.ts の外に Windows の名前が {len(violations)} 箇所ある")
        return 1
    print(f"OK: {len(files)} ファイルをチェックした (platform/ と paths.ts の外に Windows の名前はない)")
    return 0


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass
    sys.exit(main())
