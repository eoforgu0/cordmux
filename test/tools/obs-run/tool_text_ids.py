#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# 実機テストのツール (test/tools/obs-run の .mjs / .ps1 / .py) が指す、製品の画面のテキストのキー (cordmux.<...>) を
# 拾い、製品の ja-JP.ini と en-US.ini の両方にあることを確認する。
# test/lint/locale_ids.py は C と C++ しか読まないので、ツールが消したキーを引いていても気付けない。
# このツールがそれを補う。画面のテキストは ini だけが持つ。
#
# 見ること (どれか 1 つでも該当すれば失敗):
#   (1) ツールの文字列リテラルの中のキーが、ja-JP.ini か en-US.ini のどちらかにない。
#   (2) キーが cordmux.<面>.<物>.<種類> の形でない (面と種類は下の一覧。複数形の _one は種類の後ろ)。
#       古い形のキー (cordmux.dock.action.write_vencord_dir ほか) は全部消えたので、ini に残っていても
#       ここで失敗にする。
# 「消す日の決まったキーの一覧にある」ことは見ない (読む対象がなくなり、該当しない枝になったため)。
# 消したキーをツールが引けば (1) が失敗にする。
# コメント内のキーは見ない (説明に古いキーを書くことがある)。テスト (*.test.ts) も見ない
# (ないキーを渡して失敗を確認するテストがある)。"obs:" で始まる OBS 自身のテキストの id は OBS の ini にあり、このツールは
# OBS を持たないので見ない (run-window.mjs --dry-run が --obs の OBS の ini で確認する)。
#
# 使い方:
#   python tool_text_ids.py             test/tools/obs-run のツールを、repo の ja-JP.ini と en-US.ini と突き合わせる
#   python tool_text_ids.py --selftest  手で組んだ例 (ファイルを書かない) で、失敗と成功の分け方を確認する
# 終了コード: 0 = 合格、1 = 失敗 (見つけたものを 1 行ずつ出す)、2 = 読むべきファイルがない。

import io
import os
import pathlib
import re
import sys
import tokenize

HERE = pathlib.Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
LOCALE_DIR = REPO_ROOT / "src-obs" / "data" / "locale"

TOOL_EXTENSIONS = {".mjs", ".js", ".ts", ".ps1", ".py"}
SKIP_DIRS = {"__pycache__", "node_modules"}
# キーの形 (cordmux の後に 2 つ以上の区切り)。cordmux.dll のような 1 つだけの物はキーではない。
KEY_PATTERN = re.compile(r"cordmux(?:\.[a-z0-9_]+){2,}")
# キーの面と種類。
FACES = {"dock", "look", "status", "settings", "participants", "dialog", "source", "scene", "filter"}
KINDS = {"button", "menu", "page", "heading", "label", "option", "check", "status", "title", "headline", "text",
         "tip", "empty", "a11y"}


# ---- 文字列リテラルを拾う (コメントを除く) ----

def js_strings(text):
    """.mjs / .js / .ts の文字列リテラル ("..."、'...'、`...`) の中身の並び。コメント (// と /* */) と
    正規表現のリテラルは飛ばす。正規表現かどうかは、直前の意味のある文字で決める (よく使われる見分け方)。"""
    out = []
    i = 0
    n = len(text)
    prev = ""
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if c in "\"'`":
            j = i + 1
            buf = []
            while j < n and text[j] != c:
                if text[j] == "\\" and j + 1 < n:
                    buf.append(text[j:j + 2])
                    j += 2
                    continue
                buf.append(text[j])
                j += 1
            out.append("".join(buf))
            i = j + 1
            prev = c
            continue
        if c == "/" and (prev == "" or prev in "(,=:[!&|?{};+-*%<>~^"):
            # 正規表現のリテラル。[...] の中の / は区切りではない。
            j = i + 1
            in_class = False
            while j < n and text[j] != "\n":
                ch = text[j]
                if ch == "\\":
                    j += 2
                    continue
                if ch == "[":
                    in_class = True
                elif ch == "]":
                    in_class = False
                elif ch == "/" and not in_class:
                    break
                j += 1
            i = j + 1
            prev = "/"
            continue
        if not c.isspace():
            prev = c
        i += 1
    return out


def ps1_strings(text):
    """.ps1 の文字列リテラル ("..."、'...'、@" "@、@' '@) の中身の並び。コメント (# と <# #>) は飛ばす。"""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if text.startswith("<#", i):
            j = text.find("#>", i + 2)
            i = n if j < 0 else j + 2
            continue
        if c == "#":
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if text.startswith('@"', i) or text.startswith("@'", i):
            close = "\n" + text[i + 1] + "@"
            j = text.find(close, i + 2)
            end = n if j < 0 else j
            out.append(text[i + 2:end])
            i = n if j < 0 else j + len(close)
            continue
        if c in "\"'":
            j = i + 1
            buf = []
            while j < n and text[j] != c:
                if c == '"' and text[j] == "`" and j + 1 < n:
                    buf.append(text[j:j + 2])
                    j += 2
                    continue
                buf.append(text[j])
                j += 1
            out.append("".join(buf))
            i = j + 1
            continue
        i += 1
    return out


def py_strings(text):
    """.py の文字列リテラルの中身 (tokenize で拾う。コメントは拾わない)。"""
    out = []
    try:
        for tok in tokenize.generate_tokens(io.StringIO(text).readline):
            name = tokenize.tok_name.get(tok.type, "")
            if tok.type == tokenize.STRING or name == "FSTRING_MIDDLE":
                out.append(tok.string)
    except (tokenize.TokenError, IndentationError, SyntaxError):
        # 読めなかったファイルは、コメントも含めて全部を文字列として見る (見落とすより、余分に失敗にする側に倒す)。
        out.append(text)
    return out


def strings_of(name, text):
    ext = os.path.splitext(name)[1].lower()
    if ext == ".ps1":
        return ps1_strings(text)
    if ext == ".py":
        return py_strings(text)
    return js_strings(text)


def keys_in(name, text):
    keys = set()
    for literal in strings_of(name, text):
        for m in KEY_PATTERN.finditer(literal):
            keys.add(m.group(0))
    return keys


# ---- ini と、消す日の決まったキー ----

def ini_keys(text):
    keys = set()
    for raw in text.replace("﻿", "").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        keys.add(line.split("=", 1)[0].strip())
    return keys


def shape_ok(key):
    parts = key.split(".")
    if len(parts) != 4 or parts[0] != "cordmux" or parts[1] not in FACES:
        return False
    kind = parts[3][:-4] if parts[3].endswith("_one") else parts[3]
    return kind in KINDS


def check(tool_files, ja_keys, en_keys):
    """tool_files: {名前: 中身}。返す物: 見つけた物の行の並び (空なら合格) と、拾ったキーの数。"""
    findings = []
    total = set()
    for name in sorted(tool_files):
        for key in sorted(keys_in(name, tool_files[name])):
            total.add(key)
            if key not in ja_keys:
                findings.append(f"{name}: {key}: ja-JP.ini にない")
            if key not in en_keys:
                findings.append(f"{name}: {key}: en-US.ini にない")
            if not shape_ok(key):
                findings.append(f"{name}: {key}: cordmux.<面>.<物>.<種類> の形でない古い形のキー")
    return findings, len(total)


# ---- repo を読む ----

def tool_files_in(root):
    files = {}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fname in filenames:
            ext = os.path.splitext(fname)[1].lower()
            if ext not in TOOL_EXTENSIONS or fname.endswith(".test.ts"):
                continue
            path = pathlib.Path(dirpath) / fname
            if path.resolve() == pathlib.Path(__file__).resolve():
                continue
            rel = path.relative_to(root).as_posix()
            files[rel] = path.read_text(encoding="utf-8-sig", errors="replace")
    return files


def run_repo():
    needed = [LOCALE_DIR / "ja-JP.ini", LOCALE_DIR / "en-US.ini"]
    missing = [str(p) for p in needed if not p.exists()]
    if missing:
        print("NG: 読むべきファイルがない: " + ", ".join(missing))
        return 2
    ja = ini_keys((LOCALE_DIR / "ja-JP.ini").read_text(encoding="utf-8-sig"))
    en = ini_keys((LOCALE_DIR / "en-US.ini").read_text(encoding="utf-8-sig"))
    findings, count = check(tool_files_in(HERE), ja, en)
    for f in findings:
        print(f)
    if findings:
        print(f"NG: 実機テストのツールが指すキーのうち {len(findings)} 件が ini と合わない")
        return 1
    print(f"OK: 実機テストのツールが指すキー {count} 個は ja-JP.ini と en-US.ini にあり、どれも現在の形のキー")
    return 0


# ---- セルフテスト (ファイルを書かない) ----

def selftest():
    ja = {"cordmux.dock.add_to_vesktop.button", "cordmux.dock.window.title", "cordmux.dock.no_video.headline_one",
          "cordmux.dock.action.write_vencord_dir", "cordmux.dock.settings.auto_layout", "cordmux.settings.only_ja.label"}
    en = ja - {"cordmux.settings.only_ja.label"}
    failures = []

    def expect(label, cond):
        if not cond:
            failures.append(label)

    good = {
        "a.mjs": '// コメントの "cordmux.dock.action.write_vencord_dir" は見ない\n'
                 'export const A = "cordmux.dock.add_to_vesktop.button";\n'
                 'const r = /^"|"$/g; const t = `x ${"cordmux.dock.window.title"}`;\n'
                 '/* "cordmux.no.such.key" */ const u = "cordmux.dll";\n'
                 'const v = "cordmux.dock.no_video.headline_one";\n',
        "b.ps1": '# "cordmux.no.such.key"\n<# "cordmux.no.such.key" #>\n$t = "cordmux.dock.window.title"\n',
        "c.py": '# "cordmux.no.such.key"\nNAME = "cordmux.dock.window.title"\n',
    }
    findings, count = check(good, ja, en)
    expect("正しい例は合格", findings == [])
    expect("正しい例で拾うキーは 3 個 (コメントと cordmux.dll は拾わない)", count == 3)

    cases = [
        ("ini にない", {"x.mjs": 'const a = "cordmux.dock.missing_key.button";'}, "ja-JP.ini にない"),
        ("en にない", {"x.mjs": 'const a = "cordmux.settings.only_ja.label";'}, "en-US.ini にない"),
        ("ps1 の '...' の中も拾う", {"x.ps1": "$a = 'cordmux.dock.settings.auto_layout'"}, "古い形のキー"),
        ("古い形のキー", {"x.py": 'A = "cordmux.dock.action.write_vencord_dir"'}, "古い形のキー"),
        ("正規表現の後の文字列も拾う", {"x.mjs": 'const r = /a"b/; const k = "cordmux.dock.action.write_vencord_dir";'}, "古い形のキー"),
        ("ヒア文字列の中も拾う", {"x.ps1": '$s = @"\ncordmux.dock.action.write_vencord_dir\n"@\n'}, "古い形のキー"),
    ]
    for label, files, reason in cases:
        found, _ = check(files, ja, en)
        expect(label, any(reason in f for f in found))

    expect("形: 4 つの区切りと面と種類", shape_ok("cordmux.dialog.uninstall.title") and not shape_ok("cordmux.dock.title"))
    expect("形: 数の組の _one", shape_ok("cordmux.dock.no_video.headline_one") and not shape_ok("cordmux.dock.x.headline_two"))

    for f in failures:
        print("selftest 失敗: " + f)
    if failures:
        return 1
    print("selftest OK: tool_text_ids")
    return 0


def main(argv):
    if argv and argv[0] == "--selftest":
        return selftest()
    return run_repo()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
