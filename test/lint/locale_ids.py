#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# src-obs のコードが指す ini のキーと、src-obs/data/locale の ja-JP.ini と en-US.ini のキーを、両方向に突き合わせる。
# 空の値、2 回定義したキー、ja と en の %N の数、%N の説明のコメント、_one のキー (数がちょうど 1 のときの形) と
# 元のキーの組もチェックする。キーの書式は docs/ui_guidelines.md の「ini のキーの書式」にある。コードの全体と 2 つの
# ini を合わせてチェックするので、引数はファイルではなく、リポジトリのルートとみなすフォルダー 1 つである。
#
#   python locale_ids.py [ROOT]      ROOT/src-obs と、その中の 2 つの ini をチェックする (省くとリポジトリ)
#   python locale_ids.py --selftest  NG 例と OK 例でセルフテストを実行する

import argparse
import collections
import os
import re
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
FIXTURES_DIR = SCRIPT_DIR / "fixtures" / "locale_ids"

EXTENSIONS = {".h", ".hpp", ".c", ".cc", ".cpp"}
PRUNE_DIRS = {
    ".git", ".deps", "node_modules", "build", "build_x64", "release",
    "dist", "coverage", "__pycache__", ".venv", "out",
}

# 文字列リテラルの中身の全体がこの形に一致したときだけ、ini のキーとして集める。cordmux の後に 2 つ以上の部分が
# 必要なので、連結して作るキーの先頭部分 ("cordmux.contact." のようなもの) と、"cordmux.dll" のようなファイル名は
# 集めない。
FULL_ID_RE = re.compile(r"^cordmux(\.[A-Za-z0-9_-]+){2,}$")

# 数がちょうど 1 のときに使うキーの末尾 (docs/ui_guidelines.md の「ini のキーの書式」)。
ONE_SUFFIX = "_one"

# %N の説明のコメントの形: キーの行の真上のコメントの中の `%N=` (`%N =` も認める)。
NOTE_ARG_RE = re.compile(r"%(\d+)\s*=")

# ---- コメントと文字列の切り分け (windows_api_confinement.py と同じ処理) ----

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
        # C++14 の桁区切り (2'000'000) の ' は文字リテラルの始まりではないので、直前と直後がどちらも数字のときは
        # コードとして読み飛ばす。読み飛ばさないと、ファイルの最後までを閉じない文字列として読み、その後のキーを
        # 1 つも集められない。直前だけを見ると `u8'x'` のような文字リテラルの ' も読み飛ばすので、直後も数字である
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


def to_relpath(path):
    try:
        return path.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return path.as_posix()


# ---- コードの文字列リテラルから ini のキーを集める ----

def discover_cpp_files(src_obs_root):
    files = []
    if not src_obs_root.exists():
        return files
    for dirpath, dirnames, filenames in os.walk(src_obs_root):
        dirnames[:] = [d for d in dirnames if d not in PRUNE_DIRS]
        for fname in filenames:
            if os.path.splitext(fname)[1] in EXTENSIONS:
                files.append(Path(dirpath) / fname)
    return sorted(files)


def find_ids_in_file(path):
    """(キー, 行番号) のリスト。文字列リテラルの中身の全体が FULL_ID_RE に一致したものだけを集める。"""
    try:
        text = path.read_text(encoding="utf-8-sig")
    except (UnicodeDecodeError, OSError):
        return []
    tokens = tokenize_c_like(text)
    found = []
    pos = 0
    for kind, chunk in tokens:
        if kind == "string" and len(chunk) >= 2 and chunk[0] in ('"', "'", "`") and chunk[-1] == chunk[0]:
            inner = chunk[1:-1]
            if FULL_ID_RE.fullmatch(inner):
                line = text.count("\n", 0, pos) + 1
                found.append((inner, line))
        pos += len(chunk)
    return found


def find_code_literal_ids(src_obs_root):
    """キー -> (相対パス, 行番号) (最初に見つかった場所)。"""
    ids = {}
    for path in discover_cpp_files(src_obs_root):
        for id_, line in find_ids_in_file(path):
            if id_ not in ids:
                ids[id_] = (to_relpath(path), line)
    return ids


# ---- ini のキーと値 ----

def read_ini_entries(path):
    """(entries, duplicates) を返す。entries はキー -> 最後に定義された値。duplicates は、2 回目以降に定義された
    (キー, 行番号) のリストで、check_project がエラーにする (同じキーを 2 回定義すると、前の値は使われない)。"""
    entries = {}
    duplicates = []
    try:
        text = path.read_text(encoding="utf-8-sig")
    except (UnicodeDecodeError, OSError):
        return entries, duplicates
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
        if key in entries:
            duplicates.append((key, line_no))
        entries[key] = value
    return entries, duplicates


def read_ini_notes(path):
    """キー -> (行番号, 真上のコメント)。コメントは、キーの行の直前に空行を挟まずに続く `#` の行である。
    libobs (text-lookup.c) と同じく、`#` で始まる行をコメントとして読む。キーを 2 回定義していれば、最後の行のもの。"""
    notes = {}
    try:
        text = path.read_text(encoding="utf-8-sig")
    except (UnicodeDecodeError, OSError):
        return notes
    block = []
    for line_no, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if line.startswith("#"):
            block.append(line)
            continue
        if line and not line.startswith(";"):
            eq = line.find("=")
            if eq != -1:
                notes[line[:eq]] = (line_no, "\n".join(block))
        block = []
    return notes


# %1 や %2 (TextRef::args を埋め込む位置)。ja-JP.ini と en-US.ini で、%N ごとの数がそろっていることを
# check_project が確認する。
ARG_TOKEN_RE = re.compile(r"%(\d+)")


def arg_counts(value):
    return collections.Counter(ARG_TOKEN_RE.findall(value))


def format_counts(counts):
    return "[" + ", ".join(f"%{n} x{counts[n]}" for n in sorted(counts, key=int)) + "]"


# ---- チェック: リポジトリのルートとみなす 1 つのフォルダー (リポジトリか、NG 例と OK 例のフォルダー) ----

def check_project(root):
    violations = []

    src_obs_root = root / "src-obs"

    code_ids = find_code_literal_ids(src_obs_root)

    expected = set(code_ids.keys())

    locale_entries = {}
    for locale in ("ja-JP", "en-US"):
        ini_path = root / "src-obs" / "data" / "locale" / f"{locale}.ini"
        if not ini_path.exists():
            violations.append(f"{to_relpath(ini_path)}: missing-ini: ファイルがない")
            continue
        entries, duplicates = read_ini_entries(ini_path)
        notes = read_ini_notes(ini_path)
        locale_entries[locale] = (ini_path, entries)
        ini_keys = {k for k in entries if k.startswith("cordmux.")}

        # `<キー>_one` は、`<キー>` が参照されていれば参照されたとみなす。コードは 2 つのキーの両方を持っても、
        # 持たなくてもよい (src-obs/ledger/plural.h の CountedId は両方を持つ)。
        referenced = expected | {
            k for k in ini_keys if k.endswith(ONE_SUFFIX) and k[: -len(ONE_SUFFIX)] in expected
        }

        for key, line_no in duplicates:
            if key.startswith("cordmux."):
                violations.append(f"{to_relpath(ini_path)}:{line_no}: duplicate-key: {key} を 2 回定義している")

        for key in sorted(expected - ini_keys):
            loc = code_ids[key]
            violations.append(f"{to_relpath(ini_path)}: missing-key: {key} がない ({loc[0]}:{loc[1]} が指している)")

        for key in sorted(ini_keys - referenced):
            violations.append(f"{to_relpath(ini_path)}: orphan-key: {key} を指しているコードがない")

        for key in sorted(ini_keys & referenced):
            if entries.get(key, "") == "":
                violations.append(f"{to_relpath(ini_path)}: empty-value: {key} の値が空")

        # %N を持つキーの行の真上のコメントに、%N ごとに `%N=中身` の形で何が入るかを書く。
        for key in sorted(ini_keys):
            args = set(ARG_TOKEN_RE.findall(entries[key]))
            if not args:
                continue
            line_no, note = notes.get(key, (0, ""))
            missing = sorted(args - set(NOTE_ARG_RE.findall(note)), key=int)
            if missing:
                marks = " ".join(f"%{n}" for n in missing)
                violations.append(
                    f"{to_relpath(ini_path)}:{line_no}: arg-comment: {key} の {marks} に何が入るかが、キーの行の"
                    f"真上のコメントに `%N=中身` の形で書かれていない"
                )

    # ja-JP.ini と en-US.ini が両方読めたときだけ、2 つを突き合わせる。
    if "ja-JP" in locale_entries and "en-US" in locale_entries:
        ja_path, ja_entries = locale_entries["ja-JP"]
        en_path, en_entries = locale_entries["en-US"]
        ja_keys = {k for k in ja_entries if k.startswith("cordmux.")}
        en_keys = {k for k in en_entries if k.startswith("cordmux.")}

        # 2 つの ini のキーの集合が一致することを確認する。片方の ini にだけある `_one` のキーは、参照されたとみなすので
        # orphan-key では検出できない。
        for key in sorted(ja_keys - en_keys):
            violations.append(f"{to_relpath(en_path)}: locale-mismatch: {key} がない (ja-JP.ini にはある)")
        for key in sorted(en_keys - ja_keys):
            violations.append(f"{to_relpath(ja_path)}: locale-mismatch: {key} がない (en-US.ini にはある)")

        # ja と en で、%N ごとの数を比べる。集合で比べると、片方だけで同じ %N を 2 回使ったり、1 回落としたり
        # しても見つからない。
        for key in sorted(ja_keys & en_keys):
            ja_args = arg_counts(ja_entries[key])
            en_args = arg_counts(en_entries[key])
            if ja_args != en_args:
                violations.append(
                    f"{to_relpath(ja_path)}: arg-mismatch: {key} の %N が en-US.ini と食い違う "
                    f"(ja-JP: {format_counts(ja_args)} / en-US: {format_counts(en_args)})"
                )

        # `<キー>_one` があれば、同じ ini に元のキーもある。en の `_one` は、元のキーと同じ %N を同じ数だけ持つ。
        for key in sorted((ja_keys | en_keys)):
            if not key.endswith(ONE_SUFFIX):
                continue
            base = key[: -len(ONE_SUFFIX)]
            for path, entries, keys in ((ja_path, ja_entries, ja_keys), (en_path, en_entries, en_keys)):
                if key in keys and base not in keys:
                    violations.append(f"{to_relpath(path)}: one-without-base: {key} の元のキー {base} がない")
            if key in en_keys and base in en_keys:
                one_args = arg_counts(en_entries[key])
                base_args = arg_counts(en_entries[base])
                if one_args != base_args:
                    violations.append(
                        f"{to_relpath(en_path)}: one-arg-mismatch: {key} の %N が元のキー {base} と食い違う "
                        f"({format_counts(one_args)} / {format_counts(base_args)})。数を書く所にも同じ番号の %N を置く"
                    )
            # ja の `_one` の値は元のキーと違ってよい (「このシーンコレクション」と「これらのシーンコレクション」の
            # ように、ja でも数で語が変わることがある)。ja の `_one` の %N の数は別に確認しない。ja と en の比較と、
            # en の `_one` と元のキーの比較から、同じになることが分かるため。

    return violations


# ---- --selftest ----

# (例のフォルダー, 問題がないはずか, 期待するエラーの文字列)。NG 例では、出たエラーのすべてが期待する文字列の
# どれかを含み、期待する文字列のどれもが 1 回は出ることを求める (ほかの理由のエラーが出て、確認したいチェックが
# 働いていないのにセルフテストが成功するのを防ぐ)。
SELFTEST_CASES = [
    (FIXTURES_DIR / "bad_code_missing_from_ini", False, ("missing-key: cordmux.dock.extra_missing_from_ini.status がない",)),
    (FIXTURES_DIR / "bad_ini_orphan", False, (": orphan-key: ",)),
    (FIXTURES_DIR / "bad_locale_mismatch", False, ("export_diagnostics.button がない",)),
    (FIXTURES_DIR / "bad_duplicate_key", False, (": duplicate-key: ",)),
    (FIXTURES_DIR / "bad_empty_value", False, (": empty-value: ",)),
    (FIXTURES_DIR / "bad_arg_mismatch", False, (": arg-mismatch: ",)),
    (FIXTURES_DIR / "bad_arg_count_mismatch", False, (": arg-mismatch: ",)),
    (FIXTURES_DIR / "bad_arg_unannotated", False, (": arg-comment: ",)),
    (FIXTURES_DIR / "bad_one_without_base", False, (": one-without-base: ", "summary_call.status がない")),
    (FIXTURES_DIR / "bad_one_args_mismatch", False, (": arg-mismatch: ", ": one-arg-mismatch: ")),
    (FIXTURES_DIR / "bad_one_one_locale", False, (": locale-mismatch: ",)),
    (FIXTURES_DIR / "good", True, ()),
]


def run_selftest():
    bad_count = sum(1 for _, expect_clean, _ in SELFTEST_CASES if not expect_clean)
    good_count = sum(1 for _, expect_clean, _ in SELFTEST_CASES if expect_clean)
    if bad_count == 0 or good_count == 0:
        print("selftest 失敗: NG 例か OK 例がない")
        return 1
    ok = True
    for root, expect_clean, reasons in SELFTEST_CASES:
        if not root.exists():
            print(f"selftest 失敗: 例のフォルダーがない: {root}")
            ok = False
            continue
        violations = check_project(root)
        clean = len(violations) == 0
        if clean != expect_clean:
            label = "OK 例でエラーが出た" if expect_clean else "NG 例でエラーが出ない"
            print(f"selftest 失敗 ({label}): {root}")
            for v in violations:
                print(f"  {v}")
            ok = False
            continue
        if expect_clean:
            continue
        stray = [v for v in violations if not any(r in v for r in reasons)]
        unseen = [r for r in reasons if not any(r in v for v in violations)]
        if stray or unseen:
            print(f"selftest 失敗 (期待と違うエラー): {root}")
            for v in stray:
                print(f"  期待していないエラー: {v}")
            for r in unseen:
                print(f"  出なかったエラー: {r}")
            ok = False
    if ok:
        print("selftest OK: locale_ids")
        return 0
    return 1


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="src-obs のコードが指す ini のキーと、ja-JP.ini と en-US.ini のキーを両方向に突き合わせる"
    )
    parser.add_argument("root", nargs="?", help="リポジトリのルートとみなすフォルダー (省くとリポジトリ)")
    parser.add_argument("--selftest", action="store_true", help="NG 例と OK 例でセルフテストを実行する")
    args = parser.parse_args(argv)

    if args.selftest:
        return run_selftest()

    root = Path(args.root).resolve() if args.root else REPO_ROOT
    violations = check_project(root)
    for v in violations:
        print(v)
    if violations:
        print(f"NG: コードと ini のキーに {len(violations)} 個の問題がある")
        return 1
    print("OK: コードが指すキーと、ja-JP.ini と en-US.ini のキーと値に問題はない")
    return 0


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass
    sys.exit(main())
