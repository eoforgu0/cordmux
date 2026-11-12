#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# Vencord のビルドの dist に、Vesktop が読む 5 つのファイル (REQUIRED_FILES) があり、空でなく、3 つの JS の先頭に
# スタンドアロンで更新を無効にしたビルドの行があることを確認する。package.json が必要な理由は REQUIRED_FILES の
# コメントにある。problems_for は「名前 -> 中身」の関数を受け取るので、フォルダーにも zip の中にも使える
# (test/tools/dist-zip/check_dist_zip.py が zip の中の 2 か所に使う)。
#
#   python check_dist.py <dist のフォルダー>
#   python check_dist.py --selftest

import argparse
import sys
import tempfile
from pathlib import Path
from typing import Callable, Optional

# Vesktop が読む Vencord の 5 つのファイル。3 つの JS と CSS のほかに、package.json も必要である。
# Vesktop 1.6.7 は、起動したときに vencordDir にこの 5 つがそろっているかを確認 (src/main/utils/vencordLoader.ts の
# isValidVencordInstall)、1 つでもなければ、公式の最新の Vencord をダウンロードして vencordDir に書く (同じファイルの
# ensureVencordFiles。src/main/mainWindow.ts の createWindows から呼ばれる)。cordmux の Vencord はそれで上書きされる。
#   - 中身は {} のままにし、バージョンを書かない (バージョンは cordmux-build.txt が持つ)。中身の照合もしない。
#     Vesktop も、Vencord をダウンロードしたときに、中身が {} の package.json を書く。
#   - pnpm build は package.json を書かないので、CI (.github/workflows/vencord-plugin.yml) が、ないときだけ {} を
#     書く。上書きしないのは、Vencord のビルドが package.json を書くようになったときに、その中身を捨てないため。
REQUIRED_FILES = [
    "package.json",
    "vencordDesktopMain.js",
    "vencordDesktopPreload.js",
    "vencordDesktopRenderer.js",
    "vencordDesktopRenderer.css",
]

# 先頭に Vencord のビルドの情報の行を持つ 3 つの JS (CSS と package.json にはない)。
HEADER_FILES = [
    "vencordDesktopMain.js",
    "vencordDesktopPreload.js",
    "vencordDesktopRenderer.js",
]

# pnpm build --standalone --disable-updater でビルドしたときに、JS の先頭に書かれる行。ファイルの有無だけを見ると、
# 空のファイルや、更新を無効にしていないビルド (公式の Vencord で上書きされる形) でも通ってしまうため、この 2 行を見る。
REQUIRED_HEADER_LINES = [
    "// Standalone: true",
    "// Updater Disabled: true",
]

# 先頭の行を探すのは 8 行目まで。現在の Vencord のビルドでは情報の行は 4 行で、2 行目と 4 行目に目的の行がある。
# 本文の中に同じ文字列があっても数えないため、探す範囲を先頭に限る。
HEADER_SCAN_LINES = 8


def header_problems(name: str, data: bytes) -> list[str]:
    # 1 つの JS の先頭の行を見て、足りない行を問題の文の並びで返す。
    head = data.decode("utf-8", errors="replace").split("\n")[:HEADER_SCAN_LINES]
    head = [line.rstrip("\r") for line in head]
    return [
        f"先頭の行: {name} の最初の {HEADER_SCAN_LINES} 行に '{expected}' がない"
        for expected in REQUIRED_HEADER_LINES
        if expected not in head
    ]


def problems_for(read: Callable[[str], Optional[bytes]]) -> list[str]:
    # read(名前) が中身 (なければ None) を返す場所について、5 つのファイルの問題 (ない、空、先頭の行が足りない) の
    # 文を並べて返す。問題がなければ空の並び。
    problems: list[str] = []
    for name in REQUIRED_FILES:
        data = read(name)
        if data is None:
            problems.append(f"ない: {name}")
            continue
        if len(data) == 0:
            problems.append(f"空: {name}")
            continue
        if name in HEADER_FILES:
            problems.extend(header_problems(name, data))
    return problems


def dir_reader(dist_dir: Path) -> Callable[[str], Optional[bytes]]:
    # フォルダーを、problems_for に渡せる「名前 -> 中身」の関数にする。

    def read(name: str) -> Optional[bytes]:
        path = dist_dir / name
        return path.read_bytes() if path.is_file() else None

    return read


def missing_files(dist_dir: Path) -> list[str]:
    # dist_dir にない REQUIRED_FILES の名前を、REQUIRED_FILES の順に返す。
    return [name for name in REQUIRED_FILES if not (dist_dir / name).is_file()]


def check(dist_dir: Path) -> int:
    # dist_dir をチェックする。問題がなければ 0、あれば 1 を返し、問題を 1 行ずつ出力する。
    problems = problems_for(dir_reader(dist_dir))
    if not problems:
        print(f"OK: {dist_dir}: 5 つのファイルがあり、JS の先頭の行はスタンドアロンで更新が無効")
        return 0

    for problem in problems:
        print(f"NG: {dist_dir}: {problem}", file=sys.stderr)
    return 1


# ---- セルフテスト ----------------------------------------------------------------

GOOD_HEADER = (
    "// Vencord 0123abcd\n"
    "// Standalone: true\n"
    "// Platform: Universal\n"
    "// Updater Disabled: true\n"
    '"use strict";\n'
)


def _write_all(dist_dir: Path) -> None:
    for name in REQUIRED_FILES:
        if name in HEADER_FILES:
            text = GOOD_HEADER
        elif name == "package.json":
            text = "{}\n"
        else:
            text = "body{}\n"
        (dist_dir / name).write_bytes(text.encode("utf-8"))


def selftest() -> int:
    # そろっていれば成功し、1 つずつ消す・空にする・先頭の行を変えると失敗することを確認する。
    failures: list[str] = []

    def expect(label: str, dist_dir: Path, want_problem: Optional[str]) -> None:
        problems = problems_for(dir_reader(dist_dir))
        if want_problem is None:
            if problems:
                failures.append(f"{label}: 成功のはずが {problems}")
        elif not any(want_problem in p for p in problems):
            failures.append(f"{label}: {want_problem!r} を含む問題のはずが {problems}")

    with tempfile.TemporaryDirectory(prefix="cordmux-check-dist-") as tmp:
        dist_dir = Path(tmp)
        _write_all(dist_dir)
        expect("すべてそろっている", dist_dir, None)

        for name in REQUIRED_FILES:
            target = dist_dir / name
            saved = target.read_bytes()
            target.unlink()
            expect(f"{name} を消す", dist_dir, f"ない: {name}")
            if missing_files(dist_dir) != [name]:
                failures.append(f"{name} を消す: missing_files が {missing_files(dist_dir)} を返した")
            target.write_bytes(b"")
            expect(f"{name} を空にする", dist_dir, f"空: {name}")
            target.write_bytes(saved)

        for name in HEADER_FILES:
            target = dist_dir / name
            saved = target.read_bytes()
            for good, bad in (
                ("// Standalone: true", "// Standalone: false"),
                ("// Updater Disabled: true", "// Updater Disabled: false"),
            ):
                target.write_bytes(saved.replace(good.encode(), bad.encode()))
                expect(f"{name} が {bad}", dist_dir, f"{name} の最初の {HEADER_SCAN_LINES} 行に '{good}' がない")
            # 先頭の行が HEADER_SCAN_LINES より後ろにしかないものは数えない。
            target.write_bytes(("\n" * HEADER_SCAN_LINES).encode() + saved)
            expect(f"{name} の先頭の行が後ろにある", dist_dir, f"{name} の最初の {HEADER_SCAN_LINES} 行に '// Standalone: true' がない")
            target.write_bytes(saved)

        # CRLF の行も読める (Windows で改行が変わった dist を、誤って失敗にしない)。
        crlf = dist_dir / HEADER_FILES[0]
        saved = crlf.read_bytes()
        crlf.write_bytes(saved.replace(b"\n", b"\r\n"))
        expect("先頭の行が CRLF", dist_dir, None)
        crlf.write_bytes(saved)

    if failures:
        for failure in failures:
            print(f"selftest 失敗: {failure}", file=sys.stderr)
        return 1

    print("selftest OK: check_dist (そろっていれば成功し、ない・空・先頭の行が違うときはどれも失敗する)")
    return 0


def _use_utf8_console() -> None:
    # 標準出力と標準エラー出力を UTF-8 にする。Windows のコンソールの既定のコードページでは、日本語のメッセージと
    # パスが文字化けするため。
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8")


def main() -> int:
    _use_utf8_console()

    parser = argparse.ArgumentParser(
        description="Vencord の dist の 5 つのファイルと、JS の先頭の行をチェックする"
    )
    parser.add_argument("dist_dir", nargs="?", help="チェックする dist のフォルダー")
    parser.add_argument(
        "--selftest",
        action="store_true",
        help="わざと壊したデータで、このツールのセルフテストを実行する",
    )
    args = parser.parse_args()

    if args.selftest:
        return selftest()

    if not args.dist_dir:
        parser.error("--selftest を付けないときは、dist のフォルダーを指定する")

    dist_dir = Path(args.dist_dir)
    if not dist_dir.is_dir():
        print(f"NG: {dist_dir} はフォルダーでない", file=sys.stderr)
        return 1

    return check(dist_dir)


if __name__ == "__main__":
    sys.exit(main())
