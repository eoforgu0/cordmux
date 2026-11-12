#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# コミットメッセージの件名 (1 行目) に日本語 (ひらがな、カタカナ、漢字) があるかをチェックする。マージコミットは
# チェックしない (git log --no-merges)。件名だけを読む (--format=%s) ので、Co-Authored-By などのトレーラーは
# 対象に入らない。
#
#   python commit_messages_japanese.py            HEAD までのすべてのコミットをチェックする
#   python commit_messages_japanese.py A..B        指定した範囲のコミットをチェックする
#   python commit_messages_japanese.py --message-file PATH   ファイルの 1 行目を件名としてチェックする
#   python commit_messages_japanese.py --selftest  NG 例と OK 例でセルフテストを実行する

import argparse
import re
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
FIXTURES_DIR = SCRIPT_DIR / "fixtures"

JAPANESE_RE = re.compile(r"[぀-ヿ一-鿿]")


def subject_is_ok(subject):
    return bool(JAPANESE_RE.search(subject))


def get_subjects(rev_range):
    cmd = ["git", "-C", str(REPO_ROOT), "log", "--no-merges", "--format=%s"]
    if rev_range:
        cmd.append(rev_range)
    result = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8")
    if result.returncode != 0:
        return None, result.stderr.strip()
    subjects = [line for line in result.stdout.split("\n") if line != ""]
    return subjects, None


def check_range(rev_range):
    subjects, err = get_subjects(rev_range)
    if err is not None:
        label = rev_range if rev_range else "(すべてのコミット)"
        return [f"{label}: git log が失敗した ({err})"]
    label = rev_range if rev_range else "(すべてのコミット)"
    violations = []
    for subject in subjects:
        if not subject_is_ok(subject):
            violations.append(f"{label}: 件名に日本語がない: {subject!r}")
    return violations


def check_message_file(path):
    try:
        text = path.read_text(encoding="utf-8-sig")
    except (UnicodeDecodeError, OSError) as exc:
        return [f"{path}: 読み込めない ({exc})"]
    lines = text.splitlines()
    subject = lines[0] if lines else ""
    if subject_is_ok(subject):
        return []
    return [f"{path}: 件名に日本語がない: {subject!r}"]


# ---- --selftest ----

SELFTEST_CASES = [
    (FIXTURES_DIR / "commit_messages_japanese" / "bad_subject.txt", False),
    (FIXTURES_DIR / "commit_messages_japanese" / "good_subject.txt", True),
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
        violations = check_message_file(path)
        clean = len(violations) == 0
        if clean != expect_clean:
            label = "OK 例で検出された" if expect_clean else "NG 例で検出されない"
            print(f"selftest 失敗 ({label}): {path}")
            for v in violations:
                print(f"  {v}")
            ok = False
    if ok:
        print("selftest OK: commit_messages_japanese")
        return 0
    return 1


def main(argv=None):
    parser = argparse.ArgumentParser(description="コミットメッセージの件名に日本語があることを確認する")
    parser.add_argument("range", nargs="?", default=None, help="コミットの範囲 (A..B など)。省くと HEAD までのすべてのコミット")
    parser.add_argument("--message-file", metavar="PATH", help="ファイルの 1 行目を件名としてチェックする")
    parser.add_argument("--selftest", action="store_true", help="NG 例と OK 例でセルフテストを実行する")
    args = parser.parse_args(argv)

    if args.selftest:
        return run_selftest()

    if args.message_file:
        violations = check_message_file(Path(args.message_file))
    else:
        violations = check_range(args.range)

    for v in violations:
        print(v)
    if violations:
        print(f"NG: 日本語がない件名が {len(violations)} 件ある")
        return 1
    print("OK: 件名はすべて日本語を含む")
    return 0


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass
    sys.exit(main())
