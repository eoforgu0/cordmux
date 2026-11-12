#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# OBS のログに書く行の形をチェックする。書き方は CODESTYLE.md の「OBS のログ」にあり、チェック項目は CHECKS の
# コメントにある。src-obs (tests フォルダーを除く) の diag::logf( と blog( の呼び出し (ws フォルダーの下では log( も)
# の中の文字列リテラルを読む。状態が変わったときだけ書いているかは、行の形からは分からないのでチェックしない。
#
#   python log_lines.py [PATH...]      チェックし、検出があれば 1 で終わる (省くと src-obs)
#   python log_lines.py --report-only  検出があっても 0 で終わる
#   python log_lines.py --selftest     NG 例と OK 例でセルフテストを実行する

import argparse
import contextlib
import io
import re
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
FIXTURES_DIR = SCRIPT_DIR / "fixtures" / "log_lines"

if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))
import forbidden_libobs_usage as libobs  # noqa: E402  (C と C++ のコメントと文字列の切り分けと、対象のファイルの選び方を 1 か所にまとめる)

# 宣言 (void logf(int level, const char *format, ...)) にも一致するが、括弧の中に文字列リテラルがないので、何も検出しない。
CALL_RE = re.compile(r"(?<![A-Za-z0-9_])(?:diag::)?(?:logf|blog)\s*\(")
# ws フォルダーの下 (パスの途中に ws という名前のフォルダーがあるファイル) は、ログの行を Listener::Impl::log
# (ws::LogSink) に書く。その行も plugin-main.cpp の cordmux_log_sink を通って、「[cordmux] 」を付けて OBS のログに
# 出力される (LOG_INFO) ので、log( の呼び出しもチェックする。どのファイルでも log( を読むと、std::log のような、
# ほかの log という名前の関数の引数を検出してしまう。ws の外に同じ形の出力先を作ったときは、WS_DIR_NAMES に追加する。
WS_DIR_NAMES = ("ws",)
WS_CALL_RE = re.compile(r"(?<![A-Za-z0-9_])(?:diag::)?(?:logf|blog|log)\s*\(")


def call_re_for(path):
    return WS_CALL_RE if any(part in WS_DIR_NAMES for part in Path(path).parts[:-1]) else CALL_RE
DESIGN_NUMBER_RE = re.compile(r"\b(?:AC|R|U)-[0-9]+\b")
PREFIX = "[cordmux] "
PRODUCT = "cordmux"
ELLIPSIS = "..."
EXPECT_RE = re.compile(r"//\s*expect:\s*(.+)$")
# チェック項目:
#   ascii            文字列が ASCII だけで書いてある (日本語を書かない)
#   design-number    文字列に内部の番号 (AC-N、R-N、U-N) がない
#   cordmux-start    書式の文字列が、先頭の「[cordmux] 」を除いて cordmux で始まらない
#   lowercase-start  書式の文字列の最初の文字が英字なら、大文字である
#   period-end       書式の文字列がピリオドで終わらない (末尾の「...」はよい)
CHECKS = ("ascii", "design-number", "cordmux-start", "lowercase-start", "period-end")
# 書式の末尾の改行のエスケープ (エスケープを解かずに読むので、バックスラッシュと n の 2 文字)
ESCAPED_NEWLINE_TAIL_RE = re.compile(r"(?:\\n)+$")


def literal_body(token):
    # 文字列リテラルの中身 (u8"..." と R"(...)" の接頭辞と引用符を除く。エスケープは解かない)
    m = re.match(r'^(?:u8|u|U|L)?R"([^(]*)\((.*)\)\1"$', token, re.S)
    if m:
        return m.group(2)
    m = re.match(r'^(?:u8|u|U|L)?"(.*)"$', token, re.S)
    return m.group(1) if m else None


def calls_in(text, call_re=CALL_RE):
    # [(行番号, [文字列リテラルの中身, ...])] のリストを返す。コメントの中は読まず、文字列の中の括弧は数えない。
    # 呼び出しの括弧の中の文字列リテラルをすべて読む。書式の後ろの引数の文字列も、ログの行にそのまま入るためである。
    # 書式は最初の文字列リテラルで、隣り合う文字列リテラルはつないで 1 つの書式として読む。
    tokens = libobs.tokenize_c_like(text)
    spans = []  # (始まり, 終わり, 種類, 文字列)
    pos = 0
    for kind, chunk in tokens:
        spans.append((pos, pos + len(chunk), kind, chunk))
        pos += len(chunk)
    code = libobs.masked_text(text, tokens, keep_kinds={"code"})
    strings_at = [(start, end, chunk) for start, end, kind, chunk in spans if kind == "string"]
    result = []
    for m in call_re.finditer(code):
        depth = 0
        end = None
        for i in range(m.end() - 1, len(code)):
            ch = code[i]
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    end = i
                    break
        if end is None:
            continue
        literals = []
        last = None
        for start, stop, chunk in strings_at:
            if m.end() <= start and stop <= end:
                body = literal_body(chunk)
                if body is None:
                    continue
                # 隣り合う文字列リテラル (間が空白だけ) は 1 つにつなぐ
                if last is not None and not code[last:start].strip():
                    literals[-1] += body
                else:
                    literals.append(body)
                last = stop
        result.append((code.count("\n", 0, m.start()) + 1, literals))
    return result


def line_problems(literals):
    # [(チェック項目の名前, メッセージ)] のリストを返す
    problems = []
    for body in literals:
        if any(ord(ch) > 0x7F for ch in body):
            problems.append(("ascii", "ログの文字列に ASCII でない文字がある (英語だけで書く)"))
            break
    for body in literals:
        m = DESIGN_NUMBER_RE.search(body)
        if m:
            problems.append(("design-number", f"ログの文字列に内部の番号 '{m.group(0)}' がある"))
            break
    if literals:
        fmt = literals[0]
        if fmt.startswith(PREFIX):
            fmt = fmt[len(PREFIX):]
        if fmt.startswith(PRODUCT):
            problems.append(("cordmux-start", "ログの行が cordmux で始まっている (文の主語をほかのものにする)"))
        elif fmt[:1].isalpha() and fmt[:1].islower():
            problems.append(("lowercase-start", "ログの行が小文字で始まっている (最初の文字を大文字にする)"))
        # 書式が % で始まるとき (「%s%.*s」のような形) は、ピリオドをチェックしない。文の中身は別の所で決まるためである。
        tail = ESCAPED_NEWLINE_TAIL_RE.sub("", fmt)
        if not fmt.startswith("%") and tail.endswith(".") and not tail.endswith(ELLIPSIS):
            problems.append(("period-end", "ログの行がピリオドで終わっている (ピリオドを付けない)"))
    return problems


def check_file(path):
    # [(行番号, チェック項目の名前, メッセージ)] のリストを返す
    try:
        text = path.read_text(encoding="utf-8-sig")
    except (UnicodeDecodeError, OSError):
        return []
    found = []
    for line_no, literals in calls_in(text, call_re_for(path)):
        for name, message in line_problems(literals):
            found.append((line_no, name, message))
    return found


def collect(paths):
    files = []
    for p in paths:
        p = Path(p)
        if p.exists():
            files.extend(libobs.collect_files(p))
    return sorted(set(files))


def run(files, strict):
    counts = {name: 0 for name in CHECKS}
    total = 0
    for path in files:
        for line_no, name, message in check_file(path):
            print(f"{libobs.to_relpath(path)}:{line_no}: {name}: {message}")
            counts[name] += 1
            total += 1
    summary = ", ".join(f"{name} {n}" for name, n in counts.items())
    if total and strict:
        print(f"NG: 書き方の規則に合わないログの行が {total} 箇所ある ({summary})")
        return 1
    print(f"{'OK' if not total else 'NG'}: {len(files)} ファイルをチェックした。検出は {total} 箇所 ({summary})"
          + ("" if strict else " (--report-only なので 0 で終わる)"))
    return 0


# ---- --selftest ----
#
# fixtures/log_lines/bad の .cpp の、呼び出しの行の末尾の「// expect: <名前> | <名前>」は、その行で検出されるはずの
# チェック項目である。expect のない呼び出しでは何も検出されない。good の .cpp では何も検出されない。

def expectations(path):
    result = {}
    for line_no, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), start=1):
        m = EXPECT_RE.search(line)
        if m:
            result[line_no] = {x.strip() for x in m.group(1).split("|")}
    return result


def run_selftest():
    failures = []
    # ws の下の例 (bad/ws、good/ws) では、log( の呼び出しも読む (WS_CALL_RE)。
    bad = sorted((FIXTURES_DIR / "bad").rglob("*.cpp"))
    good = sorted((FIXTURES_DIR / "good").rglob("*.cpp"))
    if not bad or not good:
        print("selftest 失敗: NG 例か OK 例がない")
        return 1
    seen = set()
    for path in bad + good:
        got = {}
        for line_no, name, _ in check_file(path):
            got.setdefault(line_no, set()).add(name)
            seen.add(name)
        want = expectations(path) if path in bad else {}
        for line_no in sorted(set(got) | set(want)):
            if got.get(line_no, set()) != want.get(line_no, set()):
                failures.append(f"{libobs.to_relpath(path)}:{line_no}: 検出 {sorted(got.get(line_no, set()))}、"
                                f"期待 {sorted(want.get(line_no, set()))}")
        with contextlib.redirect_stdout(io.StringIO()):
            default = main([str(path)])
            strict = main([str(path), "--strict"])
            loose = main([str(path), "--report-only"])
        want_exit = 1 if path in bad else 0
        if default != want_exit or strict != want_exit or loose != 0:
            failures.append(f"{libobs.to_relpath(path)}: 終了コードが、既定で {default}、--strict で {strict}、--report-only で {loose}")
    for name in CHECKS:
        if name not in seen:
            failures.append(f"チェック項目 {name} を検出する NG 例がない")
    for failure in failures:
        print(f"selftest 失敗: {failure}")
    if failures:
        return 1
    print(f"selftest OK: log_lines (例 {len(bad) + len(good)}、チェック項目 {len(CHECKS)} 種類)")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="OBS のログに書く行の形をチェックする (CODESTYLE.md の「OBS のログ」)")
    parser.add_argument("paths", nargs="*", help="チェックするファイルかフォルダー (省くと src-obs。tests フォルダーは除く)")
    parser.add_argument("--strict", action="store_true", help="既定と同じ (検出があれば 1 で終わる)")
    parser.add_argument("--report-only", action="store_true", help="検出があっても 0 で終わる")
    parser.add_argument("--selftest", action="store_true", help="NG 例と OK 例でセルフテストを実行する")
    args = parser.parse_args(argv)
    if args.selftest:
        return run_selftest()
    files = collect(args.paths) if args.paths else libobs.discover_default_files()
    return run(files, not args.report_only)


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass
    sys.exit(main())
