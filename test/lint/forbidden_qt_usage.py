#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# src-obs のコードに、cordmux が使わない Qt と Windows の使い方 (forbidden_qt_names.txt) がないかをチェックする。
# 一覧にあるのは、音の出るメッセージボックスと、cordmux のスタイルシートに書かない値 (OBS のテーマの var() など) である。
# 一覧の行の先頭に string: があれば文字列リテラルの中を、なければコメントと文字列を除いたコードをチェックする。対象の
# ファイルの選び方は forbidden_libobs_usage.py と同じ (src-obs の、tests フォルダーの外)。
#
#   python forbidden_qt_usage.py [PATH...]      チェックし、検出があれば 1 で終わる (省くと src-obs)
#   python forbidden_qt_usage.py --report-only  検出があっても 0 で終わる
#   python forbidden_qt_usage.py --selftest     NG 例と OK 例でセルフテストを実行する

import argparse
import contextlib
import io
import re
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
NAMES_FILE = SCRIPT_DIR / "forbidden_qt_names.txt"
FIXTURES_DIR = SCRIPT_DIR / "fixtures" / "forbidden_qt_usage"

if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))
import forbidden_libobs_usage as libobs  # noqa: E402  (C と C++ のコメントと文字列の切り分けと、対象のファイルの選び方を 1 か所にまとめる)

STRING_MARK = "string:"
EXPECT_RE = re.compile(r"//\s*expect:\s*(.+)$")


def load_specs(path=NAMES_FILE):
    # [(一覧の行, 正規表現, 文字列リテラルの中をチェックするか)] のリストを返す
    specs = []
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        in_string = line.startswith(STRING_MARK)
        raw = line[len(STRING_MARK):] if in_string else line
        specs.append((line, re.compile(raw), in_string))
    return specs


def check_file(path, specs):
    # [(行番号, 一覧の行, 一致した文字列)] のリストを返す
    try:
        text = path.read_text(encoding="utf-8-sig")
    except (UnicodeDecodeError, OSError):
        return []
    tokens = libobs.tokenize_c_like(text)
    code = libobs.masked_text(text, tokens, keep_kinds={"code"})
    strings = libobs.masked_text(text, tokens, keep_kinds={"string"})
    found = []
    for raw, pattern, in_string in specs:
        for m in pattern.finditer(strings if in_string else code):
            found.append((code.count("\n", 0, m.start()) + 1, raw, m.group(0)))
    return sorted(found)


def collect(paths):
    files = []
    for p in paths:
        p = Path(p)
        files.extend(libobs.collect_files(p) if p.is_dir() or p.is_file() else [])
    return sorted(set(files))


def run(files, specs, strict):
    hits = []
    counts = {}
    for path in files:
        for line_no, raw, word in check_file(path, specs):
            hits.append(f"{libobs.to_relpath(path)}:{line_no}: 使わない Qt と Windows の使い方 `{raw}` (`{word.strip()}`) がある")
            counts[raw] = counts.get(raw, 0) + 1
    for hit in hits:
        print(hit)
    summary = ", ".join(f"{raw} {n}" for raw, n in sorted(counts.items())) or "0"
    if hits and strict:
        print(f"NG: 使わない Qt と Windows の使い方が {len(hits)} 箇所ある ({summary})")
        return 1
    state = "OK" if not hits else "NG"
    print(f"{state}: {len(files)} ファイルをチェックした。検出は {len(hits)} 箇所" + (f" ({summary})" if hits else "")
          + ("" if strict else " (--report-only なので 0 で終わる)"))
    return 0


# ---- --selftest ----
#
# fixtures/forbidden_qt_usage/bad/*.cpp の行の末尾の「// expect: <一覧の行>」は、その行で、その一覧の行が一致することを
# 求める (「 | 」で区切って複数書ける)。expect のない行で一致すると失敗にする。good/*.cpp では何も一致しない。

def expectations(path):
    result = {}
    for line_no, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), start=1):
        m = EXPECT_RE.search(line)
        if m:
            result.setdefault(line_no, set()).update(x.strip() for x in m.group(1).split(" | "))
    return result


def run_selftest():
    specs = load_specs()
    failures = []
    bad = sorted((FIXTURES_DIR / "bad").glob("*.cpp"))
    good = sorted((FIXTURES_DIR / "good").glob("*.cpp"))
    if not bad or not good:
        print("selftest 失敗: NG 例か OK 例がない")
        return 1
    for path in bad + good:
        got = {}
        for line_no, raw, _ in check_file(path, specs):
            got.setdefault(line_no, set()).add(raw)
        want = expectations(path) if path in bad else {}
        if path in bad and not want:
            failures.append(f"{libobs.to_relpath(path)}: NG 例に expect がない")
        for line_no in sorted(set(got) | set(want)):
            if got.get(line_no, set()) != want.get(line_no, set()):
                failures.append(f"{libobs.to_relpath(path)}:{line_no}: 検出 {sorted(got.get(line_no, set()))}、"
                                f"期待 {sorted(want.get(line_no, set()))}")
        with contextlib.redirect_stdout(io.StringIO()):
            strict = run([path], specs, True)
            loose = run([path], specs, False)
        if strict != (1 if path in bad else 0) or loose != 0:
            failures.append(f"{libobs.to_relpath(path)}: 終了コードが、既定で {strict}、--report-only で {loose}")
    # 一覧のすべての行が、どれかの NG 例で一致すること (正規表現を誤って書き換えた行に、セルフテストが気付くようにする)
    covered = set()
    for path in bad:
        for _, raw, _ in check_file(path, specs):
            covered.add(raw)
    for raw, _, _ in specs:
        if raw not in covered:
            failures.append(f"一覧の行 `{raw}` に一致する NG 例がない")
    for failure in failures:
        print(f"selftest 失敗: {failure}")
    if failures:
        return 1
    print(f"selftest OK: forbidden_qt_usage (例 {len(bad) + len(good)}、一覧 {len(specs)} 行)")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="音の出るメッセージボックス (QMessageBox など) と、スタイルシートに書かない値が src-obs にないことを確認する")
    parser.add_argument("paths", nargs="*", help="チェックするファイルかフォルダー (省くと src-obs。tests フォルダーは除く)")
    parser.add_argument("--strict", action="store_true", help="既定と同じ (検出があれば 1 で終わる)")
    parser.add_argument("--report-only", action="store_true", help="検出があっても 0 で終わる")
    parser.add_argument("--selftest", action="store_true", help="NG 例と OK 例でセルフテストを実行する")
    args = parser.parse_args(argv)
    if args.selftest:
        return run_selftest()
    if not NAMES_FILE.exists():
        print(f"NG: {libobs.to_relpath(NAMES_FILE)} がない")
        return 1
    specs = load_specs()
    if not specs:
        print(f"NG: {libobs.to_relpath(NAMES_FILE)} に行がない")
        return 1
    files = collect(args.paths) if args.paths else libobs.discover_default_files()
    return run(files, specs, not args.report_only)


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass
    sys.exit(main())
