#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# アンインストールで消す対象の表 (src-obs/install/removal_table.h) を文字列として読み、Python のツールが読める形かを
# チェックする。形はそのヘッダーの「Python が読む形」のコメントにあり、チェックの中身は parse_removal_table と
# table_problems のコメントにある。test/tools/obs-run/install_snapshot.py は parse_removal_table、TABLE_BEGIN、
# TABLE_END をここから import する (表の読み方を 1 か所にまとめるため)。
#
#   python removal_table.py            src-obs/install/removal_table.h をチェックする
#   python removal_table.py PATH       PATH のヘッダーをチェックする (Locate の調べ方はチェックしない)
#   python removal_table.py --selftest NG 例と OK 例でセルフテストを実行する

import argparse
import re
import sys
from pathlib import Path

sys.dont_write_bytecode = True

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
DEFAULT_TABLE = REPO_ROOT / "src-obs" / "install" / "removal_table.h"
FIXTURES_DIR = SCRIPT_DIR / "fixtures" / "removal_table"
SNAPSHOT_TOOL_DIR = REPO_ROOT / "test" / "tools" / "obs-run"

# removal_table.h のコメントの「Python が読む形」と同じ。
TABLE_BEGIN = "// removal-table-begin"
TABLE_END = "// removal-table-end"
ROW_RE = re.compile(
    r"^\s*\{RemovalItem::(\w+), Remover::(\w+), Locate::(\w+), UninstallStep::(\w+)\},\s*(//.*)?$")
COUNT_RE = re.compile(r"kRemovalItemCount\s*=\s*(\d+)\s*;")
ARRAY_OPEN_RE = re.compile(r"^\s*inline\s+constexpr\s+RemovalRow\s+kRemovalTable\[\]\s*=\s*\{\s*(//.*)?$")
ARRAY_CLOSE_RE = re.compile(r"^\s*\};\s*(//.*)?$")
BLANK_OR_COMMENT_RE = re.compile(r"^\s*(//.*)?$")
# 列挙型の宣言は、行頭 (字下げしてよい) の enum class から、行頭の }; まで (コメントの中の `enum class X { ... };` に
# 一致しないようにする)。
ENUM_RE = re.compile(r"^\s*enum\s+class\s+(\w+)\s*\{(.*?)^\s*\};", re.S | re.M)
ENUMERATOR_RE = re.compile(r"^\s*(\w+)\s*(=\s*\d+\s*)?,?\s*(//.*)?$")
REMOVERS = ("Product", "User", "Nobody")
NOBODY = "Nobody"
NOT_REMOVED = "NotRemoved"
# NG 例で検出されるはずの問題 (問題のメッセージの一部)。
EXPECT_RE = re.compile(r"^// expect: (.+)$", re.M)


class TableError(Exception):
    # 表を読めない (形が違う)。
    pass


def parse_removal_table(text):
    # removal_table.h の中身から (行のリスト, kRemovalItemCount) を返す。行は `item`、`remover`、`locate`、
    # `step`、`line` のキーを持つ辞書。次のどれかに当てはまれば TableError を投げる。
    #   - 目印の行 (`// removal-table-begin` と `// removal-table-end`) が、行頭にそれぞれ 1 回ずつ、この順にない。
    #     目印の行は後ろにコメントが続くので、行全体ではなく行頭で比べる
    #   - 目印の間に、表の行 (ROW_RE に全体が一致する 1 行)、配列の始まりと終わり、空行、コメントのどれでもない行が
    #     ある。表の 1 行を 2 行に分けたり、2 つの行を 1 行に書いたりした所を、数え落とさないためである
    #   - `kRemovalItemCount = <数>;` が 1 回だけでないか、表の行の数と違う
    lines = text.splitlines()
    begins = [i for i, line in enumerate(lines) if line.startswith(TABLE_BEGIN)]
    ends = [i for i, line in enumerate(lines) if line.startswith(TABLE_END)]
    if len(begins) != 1 or len(ends) != 1:
        raise TableError("表の目印 (removal-table-begin と removal-table-end) が、行頭にそれぞれ 1 回ずつない "
                         "(begin %d 回、end %d 回)" % (len(begins), len(ends)))
    begin, end = begins[0], ends[0]
    if end < begin:
        raise TableError("removal-table-end が removal-table-begin より前にある")
    rows = []
    for number, line in enumerate(lines[begin + 1:end], start=begin + 2):
        m = ROW_RE.match(line)
        if m:
            rows.append({"item": m.group(1), "remover": m.group(2), "locate": m.group(3), "step": m.group(4),
                         "line": number})
            continue
        if ARRAY_OPEN_RE.match(line) or ARRAY_CLOSE_RE.match(line) or BLANK_OR_COMMENT_RE.match(line):
            continue
        raise TableError("%d 行目: 目印の間の行が、表の行の形ではない (表の 1 行を 1 行のソースに書く): %s"
                         % (number, line.strip()))
    counts = COUNT_RE.findall(text)
    if len(counts) != 1:
        raise TableError("kRemovalItemCount が 1 回だけでない (%d 回)" % len(counts))
    count = int(counts[0])
    if count != len(rows):
        raise TableError("表の行は %d 行だが、kRemovalItemCount は %d" % (len(rows), count))
    return rows, count


def enum_members(text, name, allow_values):
    # enum class <name> { ... }; の列挙子の名前のリストを返す。1 行に 1 つずつでなければ TableError を投げる。
    bodies = [body for enum_name, body in ENUM_RE.findall(text) if enum_name == name]
    if len(bodies) != 1:
        raise TableError("enum class %s が 1 回だけでない (%d 回)" % (name, len(bodies)))
    members = []
    for line in bodies[0].split("\n"):
        if BLANK_OR_COMMENT_RE.match(line):
            continue
        m = ENUMERATOR_RE.match(line)
        if not m:
            raise TableError("enum class %s: 列挙子を 1 行に 1 つずつ書く: %s" % (name, line.strip()))
        if m.group(2) and not allow_values:
            raise TableError("enum class %s: 列挙子に値を書かない: %s" % (name, line.strip()))
        members.append(m.group(1))
    return members


def known_locate_views():
    # install_snapshot.py が調べ方を持つ Locate の名前 (LOCATE_VIEWS)。
    if str(SNAPSHOT_TOOL_DIR) not in sys.path:
        sys.path.insert(0, str(SNAPSHOT_TOOL_DIR))
    import install_snapshot  # noqa: E402 (install_snapshot はこのファイルの parse_removal_table を読む)
    return set(install_snapshot.LOCATE_VIEWS)


def table_problems(text, locate_views=None):
    # 表の問題のリストを返す (空なら問題はない)。parse_removal_table の問題に加えて、次をチェックする。
    #   - enum class RemovalItem の列挙子が 1 行に 1 つずつ、値を書かずに並び、その数が kRemovalItemCount と同じで、
    #     どの列挙子も表にちょうど 1 回ずつある。列挙子を追加して、表と数への追加を忘れた所を見つけるためである
    #     (src-obs/tests/test_install_removal_table.cpp も、同じことを C++ の側で確認する)
    #   - 表の Remover、Locate、UninstallStep の名前が、それぞれの enum class の列挙子にある。Remover の列挙子は
    #     Product、User、Nobody の 3 つだけ
    #   - 消さない行 (Remover::Nobody) の段階は UninstallStep::NotRemoved で、ほかの行の段階は NotRemoved ではない
    #     (enum class UninstallStep のコメントの約束)
    #   - locate_views を渡したときだけ、Locate の列挙子ごとに、install_snapshot.py の LOCATE_VIEWS に調べ方がある。
    #     調べ方のない Locate を追加すると、インストールとアンインストールの前後のスナップショットを比べるテストが、
    #     ツールのエラーで止まるためである
    try:
        rows, count = parse_removal_table(text)
        items = enum_members(text, "RemovalItem", allow_values=False)
        removers = enum_members(text, "Remover", allow_values=False)
        locates = enum_members(text, "Locate", allow_values=False)
        steps = enum_members(text, "UninstallStep", allow_values=True)
    except TableError as e:
        return [str(e)]
    problems = []
    if len(items) != count:
        problems.append("enum RemovalItem の列挙子は %d 個だが、kRemovalItemCount は %d" % (len(items), count))
    if sorted(removers) != sorted(REMOVERS):
        problems.append("enum Remover の列挙子が %s の 3 つではない (%s)" % ("、".join(REMOVERS), "、".join(removers)))
    seen = {}
    for row in rows:
        where = "%d 行目" % row["line"]
        seen[row["item"]] = seen.get(row["item"], 0) + 1
        if row["item"] not in items:
            problems.append("%s: RemovalItem::%s は列挙子にない" % (where, row["item"]))
        if row["remover"] not in removers:
            problems.append("%s: Remover::%s は列挙子にない" % (where, row["remover"]))
        if row["locate"] not in locates:
            problems.append("%s: Locate::%s は列挙子にない" % (where, row["locate"]))
        if row["step"] not in steps:
            problems.append("%s: UninstallStep::%s は列挙子にない" % (where, row["step"]))
        if (row["remover"] == NOBODY) != (row["step"] == NOT_REMOVED):
            problems.append("%s: Remover::Nobody と UninstallStep::NotRemoved は、どちらも書くか、どちらも書かない (%s、%s)"
                            % (where, row["remover"], row["step"]))
    for item in items:
        if seen.get(item, 0) != 1:
            problems.append("RemovalItem::%s が表に %d 回ある (1 回だけにする)" % (item, seen.get(item, 0)))
    if locate_views is not None:
        for locate in locates:
            if locate not in locate_views:
                problems.append("Locate::%s の調べ方が test/tools/obs-run/install_snapshot.py の LOCATE_VIEWS にない" % locate)
    return problems


def check_file(path, locate_views=None):
    try:
        text = Path(path).read_text(encoding="utf-8")
    except OSError as e:
        return ["%s を読めない: %s" % (path, e)]
    return table_problems(text, locate_views)


def report(path, problems):
    for p in problems:
        print("%s: %s" % (path, p))
    if problems:
        print("NG: %s に問題が %d 個ある" % (path, len(problems)))
    else:
        print("OK: %s をチェックした (問題はない)" % path)


def selftest():
    failures = []

    def check(cond, failure):
        # cond が偽なら、failure を失敗のメッセージとして記録する
        if not cond:
            failures.append(failure)

    goods = sorted(FIXTURES_DIR.glob("good*/removal_table.h"))
    bads = sorted(FIXTURES_DIR.glob("bad_*/removal_table.h"))
    # OK 例か NG 例が 1 つもないときは、セルフテストを失敗にする (例がなくなると、何も確認せずに成功するため)。
    check(len(goods) > 0, "OK 例がない")
    check(len(bads) > 0, "NG 例がない")
    for path in goods:
        problems = check_file(path)
        check(not problems, "OK 例で問題が検出された: %s %s" % (path.parent.name, problems[:1]))
    # NG 例は、2 行目の `// expect: <文字列>` を含む問題で検出されること。別の問題で検出されると、その NG 例で
    # 確認したいチェックが壊れても気付けないためである。
    for path in bads:
        problems = check_file(path)
        m = EXPECT_RE.search(path.read_text(encoding="utf-8"))
        want = m.group(1).strip() if m else None
        check(want is not None and any(want in p for p in problems),
              "NG 例で、expect の文字列を含む問題が検出されない: %s (expect %r) %s" % (path.parent.name, want, problems[:1]))

    # Locate の調べ方のチェック: 既定の表のコピーに調べ方のない Locate を追加すると検出され、既定の表では検出されない。
    views = known_locate_views()
    real = DEFAULT_TABLE.read_text(encoding="utf-8")
    check(not table_problems(real, views), "リポジトリの表で問題が検出された (調べ方のチェックを含む)")
    added = real.replace("\tObsLogDir,", "\tObsLogDir,\n\tNowhereYet,", 1)
    check(added != real, "リポジトリの表のコピーに Locate を追加できない")
    problems = table_problems(added, views)
    check(any("NowhereYet" in p and "LOCATE_VIEWS" in p for p in problems),
          "install_snapshot.py に調べ方がない Locate が検出されない")

    for failure in failures:
        print("selftest 失敗: %s" % failure)
    if failures:
        return 1
    print("selftest OK: removal_table (OK 例 %d、NG 例 %d、リポジトリの表と Locate の調べ方)" % (len(goods), len(bads)))
    return 0


def main(argv):
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description="アンインストールで消す対象の表が、Python のツールで読める形かをチェックする")
    parser.add_argument("path", nargs="?", help="チェックするヘッダー (省くと src-obs/install/removal_table.h)")
    parser.add_argument("--selftest", action="store_true", help="NG 例と OK 例でセルフテストを実行する")
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    if args.path:
        # 渡されたヘッダーには、Locate の調べ方のチェックをしない。NG 例と OK 例の小さな表は、LOCATE_VIEWS と
        # 関係のない名前を持つことがあるため。
        problems = check_file(args.path)
        report(args.path, problems)
    else:
        problems = check_file(DEFAULT_TABLE, known_locate_views())
        report(DEFAULT_TABLE.relative_to(REPO_ROOT).as_posix(), problems)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
