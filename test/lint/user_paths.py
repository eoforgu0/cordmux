#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# ユーザー名の入ったパス (その PC のユーザーフォルダーの下を指すパス) を検出する。コメント、文字列、コード、文書、
# データのすべてをチェックする。外部からコピーしたファイルと生成したファイルもチェックする (ビルドしたファイルには、
# ビルドした場所のパスが入りやすいため)。検出する形と、プレースホルダーとして許可する名前は USER_PATH_RES と
# name_allowed にあり、使い方は test/lint/README.md にある。出力では、見つけた名前を <名前> に置き換える (CI のログに
# 名前を再び出さないため。場所はファイル名と行番号で分かる)。

import argparse
import contextlib
import io
import re
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
FIXTURES_DIR = SCRIPT_DIR / "fixtures" / "user_paths"

if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))
import references  # noqa: E402  (チェックするファイルの選び方と、lint の NG 例と OK 例の除き方を 1 か所にまとめる)

RULE = "user-path"
_SEP = r"(?:\\+|/)+"
# 名前の部分: パスの区切り、空白、引用符、プレースホルダーの記号、パスに使えない文字を含まない文字の並び
_NAME = r"([^\\/\s<>%{}$\"'`:*?|,;()\[\]]+)"
USER_PATH_RES = (
    # <ドライブ>:\Users\<名前> (file:/// と vscode://file/ の後ろも同じ)
    re.compile(r"(?<![A-Za-z0-9])[A-Za-z]:" + _SEP + r"(?i:users)" + _SEP + _NAME),
    # %SystemDrive%\Users\<名前>
    re.compile(r"(?i:%systemdrive%|\$env:systemdrive)" + _SEP + r"(?i:users)" + _SEP + _NAME),
    # Git Bash の /<ドライブ>/Users/<名前>
    re.compile(r"(?<![A-Za-z0-9_.:/\\-])/[A-Za-z]/(?i:users)/" + _NAME),
    # WSL の /mnt/<ドライブ>/Users/<名前>
    re.compile(r"(?<![A-Za-z0-9_.:/\\-])/mnt/[A-Za-z]/(?i:users)/" + _NAME),
    # Cygwin の /cygdrive/<ドライブ>/Users/<名前>
    re.compile(r"(?<![A-Za-z0-9_.:/\\-])/cygdrive/[A-Za-z]/(?i:users)/" + _NAME),
    # Linux と macOS の /home/<名前> と /Users/<名前> (前に何も付かない形)
    re.compile(r"(?<![A-Za-z0-9_.:/\\$%-])/(?:home|Users)/" + _NAME),
    # 共有フォルダーの \\<PC>\<ドライブ>$\Users\<名前>
    re.compile(r"(?:\\\\+|//)[A-Za-z0-9._\-]+" + _SEP + r"[A-Za-z]\$" + _SEP + r"(?i:users)" + _SEP + _NAME),
    # パスの区切りを - に替えてつないだ名前 (C--Users-<名前>-projects のような、ツールの作業フォルダーの名前)
    re.compile(r"(?<![A-Za-z0-9])[A-Za-z]--(?i:users)-([^-\\/\s<>%{}$\"'`:*?|,;()\[\]]+)"),
)
# URL の中は読まない (空白に置き換える)。ただし、ファイルとエディターの URL (file: と vscode:) はパスを含むので読む。
URL_RE = re.compile(r"(?i)\b(?!file:|vscode(?:-insiders)?:)[a-z][a-z0-9+.\-]*://[^\s<>\"'`)\]]*")
# Windows に最初からあるユーザーフォルダーと、GitHub Actions の Windows と Linux のユーザー
SYSTEM_NAMES = {"public", "default", "all", "runneradmin", "runner"}
# テストデータに使ってよい架空の名前: 決まった名前、1 文字の名前、架空と分かる語で始まる名前
TEST_NAMES = {"alice", "bob", "carol", "dave", "eve", "user", "username", "someone", "example", "test", "tester"}
TEST_NAME_PREFIXES = ("example", "sample", "dummy", "fake", "test")
EXPECT_RE = re.compile(r"expect:\s*((?:user-path\s*,?\s*)+)")


def is_test_file(rel):
    parts = rel.split("/")
    name = parts[-1]
    return (any(p in ("tests", "fixtures") for p in parts[:-1]) or name.startswith("test_")
            or ".test." in name or ".selftest." in name)


# 省略した名前 (... と、1 文字の三点リーダー)
ELLIPSIS_CHARS = {".", chr(0x2026)}


def name_allowed(name, rel):
    low = name.lower()
    if low in SYSTEM_NAMES or set(low) <= ELLIPSIS_CHARS:
        return True
    if not is_test_file(rel):
        return False
    return low in TEST_NAMES or len(low) == 1 or low.startswith(TEST_NAME_PREFIXES)


def check_text(rel, text):
    """[(行番号, 名前を <名前> に置き換えた文字列)] のリストを返す。"""
    found = []
    for line_no, line in enumerate(text.split("\n"), start=1):
        low = line.lower()
        if "users" not in low and "/home/" not in low:
            continue
        line = URL_RE.sub(lambda m: " " * len(m.group(0)), line)
        taken = []
        for rx in USER_PATH_RES:
            for m in rx.finditer(line):
                # 2 つの形が同じ名前に一致したときは、1 つと数える (/c/Users/<名前> と /Users/<名前> など)
                if any(s <= m.start(1) < e for s, e in taken):
                    continue
                taken.append((m.start(1), m.end(1)))
                if name_allowed(m.group(1), rel):
                    continue
                found.append((line_no, line[m.start():m.start(1)] + "<名前>"))
    return found


def mask_names(text):
    """文字列の中の、ユーザー名の入ったパスの名前を <名前> に置き換える (リポジトリの外のファイルの場所を出力するときに使う)。"""
    for rx in USER_PATH_RES:
        text = rx.sub(lambda m: m.group(0)[:m.start(1) - m.start()] + "<名前>", text)
    return text


def scan(root, paths):
    """(チェックしたファイルの数, [(表示する名前, ツリーの中の相対パス, 行番号, 名前を置き換えた文字列)]) を返す。"""
    files = references.list_tree_files(root)
    tree = references.Tree(root, files, references.make_absent([]))
    selected = references.select_files(tree, paths, skip_third_party=False)
    found = []
    for shown_path, rel, full, _in_tree in selected:
        text = references.read_text(full)
        if text is None:
            continue
        for line_no, shown in check_text(rel, text):
            found.append((mask_names(shown_path), rel, line_no, shown))
    return len(selected), found


def run(root, paths, out=print):
    count, found = scan(root, paths)
    for shown_path, _rel, line_no, shown in found:
        out(f"{shown_path}:{line_no}: {RULE}: ユーザー名の入ったパス「{shown}」 (例は %USERPROFILE% か <名前> で書く)")
    if found:
        out(f"NG: {count} ファイルをチェックした。ユーザー名の入ったパスが {len(found)} 箇所ある")
        return 1
    out(f"OK: {count} ファイルをチェックした (ユーザー名の入ったパスはない)")
    return 0


# ---- --selftest ----
#
# fixtures/user_paths/bad の行の「expect: user-path」(1 行に 2 つなら「expect: user-path, user-path」) は、その行で
# 検出されるはずの数である。good では 1 つも検出されない。検出はリポジトリをチェックするときと同じ scan で集める
# (外部からコピーしたファイルをチェックすることも確認する)。どの形も bad で 1 回以上検出されることと、終了コードも
# 確認する。

def run_selftest():
    good = FIXTURES_DIR / "good"
    bad = FIXTURES_DIR / "bad"
    failures = []
    if not good.is_dir() or not bad.is_dir():
        print("selftest 失敗: NG 例か OK 例がない")
        return 1
    expected_total = 0
    for root, is_bad in ((bad, True), (good, False)):
        rels = references.list_tree_files(root)
        if not rels:
            failures.append(f"{root.name} にファイルがない")
        got = {}
        for _shown, rel, line_no, _text in scan(root, [])[1]:
            got[(rel, line_no)] = got.get((rel, line_no), 0) + 1
        want = {}
        if is_bad:
            for rel in rels:
                for line_no, line in enumerate((references.read_text(root / rel) or "").split("\n"), start=1):
                    m = EXPECT_RE.search(line)
                    if m:
                        want[(rel, line_no)] = m.group(1).count("user-path")
            expected_total += sum(want.values())
        for key in sorted(set(got) | set(want)):
            if got.get(key, 0) != want.get(key, 0):
                failures.append(f"{root.name}/{key[0]}:{key[1]}: 検出 {got.get(key, 0)}、期待 {want.get(key, 0)}")
    if expected_total == 0:
        failures.append("bad に expect の行がない")
    for rx_index, rx in enumerate(USER_PATH_RES):
        hit = False
        for rel in references.list_tree_files(bad):
            text = references.read_text(bad / rel) or ""
            if rx.search(URL_RE.sub(" ", text)):
                hit = True
        if not hit:
            failures.append(f"形 {rx_index + 1} に一致する NG 例がない")
    with contextlib.redirect_stdout(io.StringIO()):
        bad_exit = main(["--root", str(bad)])
        good_exit = main(["--root", str(good)])
    if (bad_exit, good_exit) != (1, 0):
        failures.append(f"終了コード: bad {bad_exit}、good {good_exit}")
    for failure in failures:
        print(f"selftest 失敗: {failure}")
    if failures:
        return 1
    print(f"selftest OK: user_paths (NG 例の検出 {expected_total}、形 {len(USER_PATH_RES)} 種類)")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="ユーザー名の入ったパスを検出する。パスを省くとリポジトリ全体をチェックし、検出があれば 1 で終わる")
    parser.add_argument("paths", nargs="*",
                        help="チェックするファイルかフォルダー (省くとリポジトリ全体。リポジトリの外のファイルも、指定すればチェックする)")
    parser.add_argument("--root", default=str(REPO_ROOT), help="リポジトリのルートとみなすフォルダー")
    parser.add_argument("--selftest", action="store_true", help="fixtures/user_paths の NG 例と OK 例でセルフテストを実行する")
    args = parser.parse_args(argv)
    if args.selftest:
        return run_selftest()
    return run(Path(args.root), args.paths)


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass
    sys.exit(main())
