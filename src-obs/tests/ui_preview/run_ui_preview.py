#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# プレビューツール (ui_preview.exe) をビルドしてから実行する。どちらかが失敗したらそこで止め、その終了コードを返す。
#   python src-obs/tests/ui_preview/run_ui_preview.py --out <出力先> [ui_preview.exe の引数 ...]
# このスクリプトが読まない引数は、ui_preview.exe にそのまま渡す。ui_preview.exe の引数は README.md にある。
# Python 3 の標準ライブラリだけを使う。

import argparse
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
TESTS_DIR = SCRIPT_DIR.parent
REPO_ROOT = TESTS_DIR.parent.parent
BUILD_DIR = REPO_ROOT / "build_x64" / "align-tests"
EXE = BUILD_DIR / "Release" / "ui_preview.exe"


def run(command, cwd):
    # 実行するコマンドを表示してから実行する (何を実行したかを出力から読めるように)。
    print("+ " + " ".join(str(part) for part in command) + "    (in " + str(cwd) + ")", flush=True)
    return subprocess.run([str(part) for part in command], cwd=str(cwd)).returncode


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    # このスクリプトだけが読む引数。--preset はビルドに使う CMake のプリセットで、既定の tests-x64-local は、
    # src-obs/tests の CMakeUserPresets.json (Git の管理の外) に、CORDMUX_BUILD_PREVIEW_TOOLS を ON にして用意する
    # プリセットの名前 (docs/testing.md の「プレビューツール」)。--no-build はビルドを飛ばし、--reconfigure は CMake の
    # キャッシュがあっても cmake --preset を実行する。
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--preset", default="tests-x64-local")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--reconfigure", action="store_true")
    parser.add_argument("-h", "--help", action="store_true")
    own, rest = parser.parse_known_args()

    if own.help:
        print("usage: run_ui_preview.py [--preset NAME] [--no-build] [--reconfigure] --out DIR [ui_preview args ...]")
        print("ui_preview args: --face dock|settings:<page>|participants|dialog:<id>|menu:dock|menu:row")
        print("                 --themes yami,yami-light  --langs ja-JP,en-US  --widths-em 15,20,26,32,45")
        print("                 --sizes WxH,...  --height PX  --only SUBSTR[,SUBSTR...]  --list  --dump-theme")
        print("                 --probe-fonts  --backend cordmuxpreview|minimal")
        print("                 --obs-source DIR  --ini-dir DIR  --first-line-scale X  --l1-border N")
        print("                 --tier-icons theme|windows")
        return 0

    # build_x64/align-tests に CMake のキャッシュがなければ構成し、ui_preview をビルドする (キャッシュがあれば構成を
    # 飛ばす。CMakeLists.txt が変わっていれば、ビルドが構成し直す)。
    if not own.no_build:
        if own.reconfigure or not (BUILD_DIR / "CMakeCache.txt").exists():
            code = run(["cmake", "--preset", own.preset], TESTS_DIR)
            if code != 0:
                return code
        code = run(["cmake", "--build", "--preset", own.preset, "--target", "ui_preview"], TESTS_DIR)
        if code != 0:
            return code

    if not EXE.exists():
        print("ui_preview.exe not found: " + str(EXE) + " (build first)", file=sys.stderr)
        return 2
    # このスクリプトを実行したフォルダーで実行する (--out や --ini-dir の相対パスを、指定したとおりの場所として読む)。
    return run([EXE] + rest, Path.cwd())


if __name__ == "__main__":
    sys.exit(main())
