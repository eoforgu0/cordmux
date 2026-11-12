#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# コメント、文書、文字列、設定ファイルとファイルの名前に、読み手が開けないものへの参照と、経緯を書いた文字列がないかを
# チェックする。規則は RULES にあり、どの規則で検出しても終了コードが 1 になる。正しい使い方もある経緯の語は、
# --candidates を付けたときに候補として出力する。規則ごとに許可するものと使い方は test/lint/README.md にある。
# チェックするのは、git が追跡しているファイルと、追跡していないが無視もしていないファイルである。外部からコピーした
# ファイルと、lint の NG 例と OK 例はチェックしない。

import argparse
import contextlib
import fnmatch
import io
import json
import os
import re
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
FIXTURES_DIR = SCRIPT_DIR / "fixtures" / "references"

if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))
import comments_japanese as cj  # noqa: E402  (Python と PowerShell のコメントと文字列の切り分けを 1 か所にまとめる)
import forbidden_libobs_usage as libobs  # noqa: E402  (C 系の言語の切り分け。C++ の桁区切りとテンプレート文字列を読める)

RULES = ("missing-path", "missing-heading", "record-id", "name-id", "doc-section", "date", "history-label")
CANDIDATE_RULES = ("history-word",)

# 日本語の文字の範囲 (ひらがなとカタカナ、漢字)。正規表現の文字クラスの中に置く。
KANA = chr(0x3041) + "-" + chr(0x30FF)
KANJI = chr(0x4E00) + "-" + chr(0x9FFF)
KATAKANA = chr(0x30A1) + "-" + chr(0x30F6) + chr(0x30FC)

# ---- チェックするファイル ----

# 外部からコピーして変更していないファイルと、生成したファイルはチェックしない。文字列を変えないので、検出しても修正できない。
# src-obs/cmake と src-obs/build-aux は obs-plugintemplate からのコピーである。src-obs/cmake の中で名前が cordmux_ で
# 始まるファイルと、製品の説明の文字列を追加した resource.rc.in は、自前の文字列を含むのでチェックする。
THIRD_PARTY_GLOBS = (
    "LICENSE",
    "*/LICENSE",
    "*licenses/*",
    "*package-lock.json",
    "src-obs/cmake/*",
    "src-obs/build-aux/*",
    "src-obs/.clang-format",
    "src-obs/.gersemirc",
    "src-obs/tests/doctest.h",
)
OWN_IN_THIRD_PARTY_GLOBS = ("src-obs/cmake/cordmux_*", "src-obs/cmake/windows/resources/resource.rc.in")
# 中身がリポジトリにないものの一覧なので、コメントの行のほかはパスをチェックしない。
IGNORE_LIST_NAMES = (".gitignore", ".gitattributes")
C_LIKE_EXTS = {".c", ".cc", ".cpp", ".h", ".hpp", ".ts", ".tsx", ".js", ".mjs", ".cjs", ".jsx"}
PY_EXTS = {".py"}
PS_EXTS = {".ps1", ".psm1"}
DOC_EXTS = {".md", ".txt"}
# ビルドの出力と作業用のフォルダーの名前。ツリーの中にないのが正しいので、パスにこの名前があれば、なくても許可する。
BUILD_DIR_NAMES = {
    ".deps", "build", "build_x64", "release", "_release", "dist", "_dist", "node_modules", "_work", "_share",
    "out", "coverage", "__pycache__", ".venv",
}
WALK_PRUNE = {".git"} | (BUILD_DIR_NAMES - {"dist"})
FILE_EXTS = (
    "md|txt|py|mjs|cjs|js|ts|tsx|jsx|cpp|cc|c|h|hpp|ps1|psm1|json|jsonl|tsv|csv|yml|yaml|ini|cmake|html|css|sh|"
    "png|svg|ttf|h264|toml|log|zip|dll|exe|pdb"
)


def run_git(root, *args):
    try:
        proc = subprocess.run(["git", "-c", "core.quotepath=false", "-C", str(root), *args],
                              capture_output=True, check=False)
    except OSError:
        return None
    if proc.returncode != 0:
        return None
    return proc.stdout


def list_tree_files(root):
    """root からの相対パス (区切りは /) のリストを返す。

    root が git の作業ツリーのルートなら、追跡しているファイルと、追跡していないが無視もしていないファイル (追加した
    ばかりのファイル) を返す。無視しているファイル (ビルドの出力と作業用のフォルダー) はツリーに入れない。git の作業
    ツリーでなければ、フォルダーをたどる。
    """
    root = Path(root).resolve()
    top = run_git(root, "rev-parse", "--show-toplevel")
    if top is not None and Path(top.decode("utf-8").strip()).resolve() == root:
        raw = run_git(root, "ls-files", "-z", "--cached", "--others", "--exclude-standard")
        if raw is not None:
            names = {n.decode("utf-8") for n in raw.split(b"\0") if n}
            return sorted(n for n in names if (root / n).is_file())
    files = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in WALK_PRUNE]
        rel_dir = Path(dirpath).relative_to(root).as_posix()
        for name in filenames:
            files.append(name if rel_dir == "." else f"{rel_dir}/{name}")
    return sorted(files)


def is_third_party(rel):
    if any(fnmatch.fnmatchcase(rel, g) for g in OWN_IN_THIRD_PARTY_GLOBS):
        return False
    return any(fnmatch.fnmatchcase(rel, g) for g in THIRD_PARTY_GLOBS)


def is_test_file(rel):
    # テストのファイル (tests か fixtures のフォルダーの下にあるもの、名前が test_ で始まるもの、名前に .test. か
    # .selftest. を含むもの) か。user_paths.py の is_test_file と同じ判定。
    parts = rel.split("/")
    name = parts[-1]
    return (any(p in ("tests", "fixtures") for p in parts[:-1]) or name.startswith("test_")
            or ".test." in name or ".selftest." in name)


def in_fixtures(path):
    try:
        Path(path).resolve().relative_to(SCRIPT_DIR / "fixtures")
        return True
    except ValueError:
        return False


def read_text(path):
    try:
        data = Path(path).read_bytes()
    except OSError:
        return None
    if b"\0" in data[:8192]:
        return None
    return data.decode("utf-8-sig", errors="replace").replace("\r\n", "\n")


# ---- 存在しないものとして扱うパターン (--absent) ----

def load_absent_file(path):
    patterns = []
    for line in Path(path).read_text(encoding="utf-8-sig").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            patterns.append(line)
    return patterns


def make_absent(patterns):
    cleaned = [p.strip().strip("/") for p in patterns if p.strip().strip("/")]

    def is_absent(rel):
        for p in cleaned:
            if any(ch in p for ch in "*?["):
                if fnmatch.fnmatchcase(rel, p) or fnmatch.fnmatchcase(rel, p + "/*"):
                    return True
            elif rel == p or rel.startswith(p + "/"):
                return True
        return False

    return is_absent


# ---- ツリーの索引 ----

MD_HEADING_RE = re.compile(r"^ {0,3}#{1,6}[ \t]+(.*?)(?:[ \t]+#+)?[ \t]*$")
FENCE_RE = re.compile(r"^ {0,3}(?:`{3,}|~{3,})")


class Tree:
    def __init__(self, root, files, is_absent):
        self.root = Path(root).resolve()
        self.all_files = set(files)
        self.present = {f for f in files if not is_absent(f)}
        self.is_absent = is_absent
        self.top_names = {f.split("/")[0] for f in files}
        self.all_dirs = self._dirs(self.all_files)
        self.present_dirs = self._dirs(self.present)
        # ファイル名だけと末尾の一致で探すのは文書なので、fixtures の下のファイルでは探さない (NG 例と OK 例のファイル名が、
        # 削除した文書の名前と同じことがある)。依存関係のバージョンの日付と、出力するファイルの名前も、fixtures の下からは
        # 取らない。
        self.own_present = {f for f in self.present if "fixtures" not in f.split("/")[:-1]}
        docs_present = self.own_present
        docs_all = {f for f in self.all_files if "fixtures" not in f.split("/")[:-1]}
        self.present_suffixes = self._suffixes(docs_present)
        self.all_suffixes = self._suffixes(docs_all)
        self.present_basenames = {f.rsplit("/", 1)[-1] for f in docs_present}
        self.all_basenames = {f.rsplit("/", 1)[-1] for f in docs_all}
        self.version_dates = self._version_dates()
        self._stems = {}
        self._output_names = None
        self._headings = {}

    @staticmethod
    def _dirs(files):
        dirs = set()
        for f in files:
            parts = f.split("/")
            for i in range(1, len(parts)):
                dirs.add("/".join(parts[:i]))
        return dirs

    @staticmethod
    def _suffixes(files):
        out = set()
        for f in files:
            parts = f.split("/")
            for i in range(len(parts)):
                out.add("/".join(parts[i:]))
        return out

    def _version_dates(self):
        """依存関係のバージョンを表す日付の集合を返す。経緯の日付ではないので、日付として検出しない。

        buildspec.json で依存関係のバージョンの名前に使っている日付と、vencord.lock.json の date (固定した上流の
        Vencord のコミットの日付) である。
        """
        dates = set()
        for rel in self.own_present:
            if rel.rsplit("/", 1)[-1] == "vencord.lock.json":
                try:
                    lock = json.loads(read_text(self.root / rel) or "{}")
                except ValueError:
                    continue
                date = lock.get("date") if isinstance(lock, dict) else None
                if isinstance(date, str) and DATE_ISO_RE.fullmatch(date):
                    dates.add(date)
                continue
            if rel.rsplit("/", 1)[-1] != "buildspec.json":
                continue
            try:
                spec = json.loads(read_text(self.root / rel) or "{}")
            except ValueError:
                continue
            for dep in (spec.get("dependencies") or {}).values():
                version = dep.get("version") if isinstance(dep, dict) else None
                if isinstance(version, str) and DATE_ISO_RE.fullmatch(version):
                    dates.add(version)
        return dates

    def resolves(self, rel, present_only=True):
        files = self.present if present_only else self.all_files
        dirs = self.present_dirs if present_only else self.all_dirs
        if rel in files or rel in dirs:
            return True
        # 拡張子を省いた参照 (JavaScript と TypeScript の import の参照先など)
        key = (rel, present_only)
        if key not in self._stems:
            prefix = rel + "."
            self._stems[key] = any(f.startswith(prefix) and "/" not in f[len(prefix):] for f in files)
        return self._stems[key]

    def output_names(self):
        """ツールが出力するファイルの名前 (コードの中にその名前だけの文字列として現れ、ツリーにはない名前) の集合を返す。"""
        if self._output_names is None:
            names = set()
            rx = re.compile(r"""["'`]([A-Za-z0-9_.\-]+\.[A-Za-z0-9]+)["'`]""")
            for rel in self.own_present:
                ext = os.path.splitext(rel)[1].lower()
                if ext not in C_LIKE_EXTS | PY_EXTS | PS_EXTS:
                    continue
                text = read_text(self.root / rel)
                if text is None:
                    continue
                names.update(rx.findall(text))
            all_names = {f.rsplit("/", 1)[-1] for f in self.all_files}
            self._output_names = names - all_names
        return self._output_names

    def find_doc(self, rel_file, path, prose):
        """文書を指すパス path が指す、ツリーの中にある .md を返す。決まらなければ None を返す。

        コメントと文書の中では、そのファイルの場所からたどり (Markdown の相対リンクと同じ)、なければルートからたどる。
        どちらにもないときは、ツリーの中で末尾が一致するパスが 1 つに決まれば、それを使う。
        """
        base_dir = rel_file.rsplit("/", 1)[0] if "/" in rel_file else ""
        segs = path.split("/")
        tries = []
        if segs[0] in (".", ".."):
            if prose:
                tries.append(os.path.normpath(os.path.join(base_dir, path)).replace("\\", "/"))
        else:
            if prose and base_dir:
                tries.append(base_dir + "/" + path)
            tries.append(path)
        for t in tries:
            if t in self.present:
                return t
        if segs[0] in (".", ".."):
            return None
        hits = [f for f in self.own_present if f == path or f.endswith("/" + path)]
        return hits[0] if len(hits) == 1 else None

    def headings(self, rel):
        """Markdown の文書の見出しの文字列の集合を返す (コードブロックの中の # の行は見出しにしない)。"""
        if rel not in self._headings:
            found, fenced = set(), False
            for line in (read_text(self.root / rel) or "").split("\n"):
                if FENCE_RE.match(line):
                    fenced = not fenced
                    continue
                m = None if fenced else MD_HEADING_RE.match(line)
                if m:
                    found.add(m.group(1))
            self._headings[rel] = found
        return self._headings[rel]


# ---- パスの形をした文字列 ----

URL_RE = re.compile(r"[A-Za-z][A-Za-z0-9+.\-]*://[^\s<>\"'`)\]]*")
_SEG = r"[A-Za-z0-9_@+.\-]+"
_JA_LAST = "[" + KANA + KANJI + "][" + KANA + KANJI + r"A-Za-z0-9_\-]*\.(?:txt|md)"
PATH_TOKEN_RE = re.compile(
    r"(?<![A-Za-z0-9_@+.\-/\\:%$~*{])"
    r"((?:\.\.?/)*" + _SEG + r"(?:/(?:" + _JA_LAST + r"|" + _SEG + r"))*/?)"
    r"(?![A-Za-z0-9_@+\-/])"
)
DOMAIN_RE = re.compile(r"^[a-z0-9\-]+(?:\.[a-z0-9\-]+)*\.(?:com|org|net|io|dev|app|gg|jp|co|edu|gov)$")
ENUM_SEG_RE = re.compile(r"\.(?:" + FILE_EXTS + r")$", re.IGNORECASE)
PURE_EXT_RE = re.compile(r"^\.[A-Za-z0-9]{1,5}$")
PLACEHOLDER_SEG_RE = re.compile(r"^[A-Z][A-Z0-9_]+$")
SKIP_NEXT_CHARS = set("*?{<$%[")
# 文書の見出しを指す文字列: 「<パス>.md の「<見出し>」」(パスを ` で囲んだ形も)
HEADING_REF_RE = re.compile(r"((?:\.\.?/)*[A-Za-z0-9_@+.\-/]*[A-Za-z0-9_@+\-]\.md)`? ?の ?「([^「」\n]+)」")


def mask_urls(line):
    return URL_RE.sub(lambda m: " " * len(m.group(0)), line)


def path_findings(rel_file, line, tree, prose):
    """(列, パス, 理由) のリストを返す。理由は "missing" か "absent"。prose はコメントと文書のとき真で、./ と ../ と .tsv も
    チェックする。"""
    found = []
    base_dir = rel_file.rsplit("/", 1)[0] if "/" in rel_file else ""
    for m in PATH_TOKEN_RE.finditer(line):
        tok = m.group(1).rstrip(".")
        if not tok or tok.startswith("@"):
            continue  # npm のスコープ付きの名前
        cut = "..." in tok
        if cut:
            tok = tok[:tok.index("...")]  # 省略した部分の前までをチェックする
        if cut or line[m.end(1):m.end(1) + 1] in SKIP_NEXT_CHARS or tok[-1:] in "-_":
            # パターン、プレースホルダー、文字列を連結して作るパスの先頭部分は、最後の / までをフォルダーとしてチェックする
            if "/" not in tok:
                continue
            tok = tok[:tok.rindex("/") + 1]
        if "/" not in tok.rstrip("/"):
            # 区切りのない名前は、拡張子の前に文字のある .md の文書と、ルートのフォルダーの名前に / を付けたものだけを
            # チェックする
            top_dir = tok.endswith("/") and tok.rstrip("/") in tree.top_names
            if not top_dir and (not tok.lower().endswith(".md") or tok.startswith(".")):
                continue
        segs = tok.rstrip("/").split("/")
        if len(segs) > 1 and PURE_EXT_RE.match(segs[-1]):
            segs = segs[:-1]  # 「名前.h/.cpp」の形は、前の 1 つだけをチェックする
        if any(ENUM_SEG_RE.search(s) for s in segs[:-1] if s not in (".", "..")):
            continue  # 「a.ts/b.ts」の形は名前を並べたもので、パスではない
        if any(PLACEHOLDER_SEG_RE.match(s) for s in segs[:-1]) or any("xxx" in s.lower() for s in segs):
            continue  # DIR や ROOT のようなプレースホルダー
        if DOMAIN_RE.match(segs[0].lower()):
            continue  # スキームを省いた URL
        if segs[0] in (".", ".."):
            if not prose:
                continue  # 文字列とデータの中の ./ と ../ は、テストデータか外部のツールが読む値 (import の参照先はビルドで確認する)
            targets = [os.path.normpath(os.path.join(base_dir, *segs)).replace("\\", "/")]
            if segs[0] == ".":
                targets.append(os.path.normpath(os.path.join(*segs)).replace("\\", "/"))
            targets = [t for t in targets if not t.startswith("..") and t != "."]
            if not targets:
                continue
            if any(tree.resolves(t) for t in targets):
                continue
            if any(tree.is_absent(t) for t in targets):
                found.append((m.start(1), tok, "absent"))
                continue
            if any(s in BUILD_DIR_NAMES for t in targets for s in t.split("/")):
                continue
            found.append((m.start(1), tok, "missing"))
            continue
        target_segs = [s for s in segs if s != "."]
        target = "/".join(target_segs)
        if prose and base_dir and tree.resolves(base_dir + "/" + target):
            continue  # 文書とコメントの中の、そのファイルの場所から書いたパス (Markdown の相対リンクと同じ)
        if target_segs[0] in tree.top_names:
            if tree.resolves(target):
                continue
            if tree.is_absent(target) or tree.resolves(target, present_only=False):
                found.append((m.start(1), tok, "absent"))
                continue
            if any(s in BUILD_DIR_NAMES for s in target_segs):
                continue
            found.append((m.start(1), tok, "missing"))
            continue
        if tree.is_absent(target):
            # ルートのフォルダーごと削除した後も、--absent に残しておけば、その下を指すパスを検出する
            found.append((m.start(1), tok, "absent"))
            continue
        last = target_segs[-1].lower()
        if not (last.endswith(".md") or (prose and last.endswith(".tsv") and len(target_segs) > 1)):
            continue
        if any(s in BUILD_DIR_NAMES for s in target_segs):
            continue
        if len(target_segs) == 1:
            if target in tree.present_basenames or target in tree.output_names():
                continue
            reason = "absent" if target in tree.all_basenames else "missing"
        else:
            if target in tree.present_suffixes or target_segs[-1] in tree.output_names():
                continue
            reason = "absent" if target in tree.all_suffixes else "missing"
        found.append((m.start(1), tok, reason))
    return found


def heading_findings(rel_file, line, tree, prose):
    """(列, 参照) のリストを返す。指している文書はあるのに、その見出しがない参照である。文書がない参照は、missing-path で
    検出する。"""
    found = []
    for m in HEADING_REF_RE.finditer(line):
        path, heading = m.group(1), re.sub(r"^#{1,6} +", "", m.group(2))  # 「## 見出し」のように行の形で書いた参照も読む
        target = tree.find_doc(rel_file, path, prose)
        if target is not None and heading not in tree.headings(target):
            found.append((m.start(1), f"{path} の「{heading}」"))
    return found


# ---- 番号、日付、経緯の形 ----

# 英大文字 1 から 4 文字 (後ろに 2 桁までの数と、小文字 1 文字を付けてよい) と、ハイフンと数 (前に英字 1 文字を付けて
# よい)。前の :: と . は識別子の一部 (列挙子の名前など) なので検出しない。前の / は検出する (並べた番号と、パスの中の
# 番号)。前の [ は、正規表現の文字クラス ([A-Z0-9] など) のときだけ、id_allowed で除く。
HYPHEN_ID_RE = re.compile(
    r"(?<![A-Za-z0-9_.\-])(?<!::)([A-Z]{1,4}[0-9]{0,2}[a-z]?)-([A-Za-z]?[0-9]+[a-z]?)(?![A-Za-z0-9_])"
)
# 英大文字 1 から 2 文字と、1 から 2 桁の数 (後ろに小文字 1 文字を付けてよい)。前の + と { はキーの書き方なので検出しない。
# 前の :: と . は識別子の一部なので検出しない。
SHORT_ID_RE = re.compile(
    r"(?<![A-Za-z0-9_.\-/+{$%#\\\[])(?<!::)([A-Z]{1,2})([0-9]{1,2})([a-z]?)(?![A-Za-z0-9_\-+])"
)
# 漢字かカタカナ 1 から 2 文字と、ハイフンと数
CJK_ID_RE = re.compile("(?<![" + KANJI + KATAKANA + "])[" + KANJI + KATAKANA + r"]{1,2}-[0-9]+(?![0-9])")
# 正規表現の文字クラスの中の範囲 ([A-Z0-9] のような、[ の直後の 1 文字とハイフンと 1 文字)
CHAR_CLASS_RE = re.compile(r"[A-Za-z0-9]-[A-Za-z0-9](?:[A-Za-z0-9]-[A-Za-z0-9]|[\]\\])")
# 規格、形式、ライセンスの名前 (ハイフンの形の先頭部分)。番号ではない。
STANDARD_PREFIXES = {
    "UTF", "UCS", "CRC", "SHA", "MD", "AES", "ARIA", "ISO", "IEC", "RFC", "CVE", "UTC", "GMT", "ECMA",
    "GPL", "LGPL", "MPL", "BSD", "BSL", "CC", "EPL", "OFL",
}
STANDARD_EXACT = {"AC-3"}  # 音声の形式の名前
# 英大文字と数の形のうち、形式、エンコード、言語のバージョンの名前
SHORT_NAMES = {
    "AV1", "VP8", "VP9", "NV12", "MD5", "MP3", "MP4", "PE32", "DX11", "DX12", "X11", "IP4", "IP6", "ES3", "ES5", "ES6",
    "C99", "C11", "C17", "C23",
}
FUNCTION_KEY_RE = re.compile(r"^F(?:[1-9]|1[0-9]|2[0-4])$")
CONTROL_SET_RE = re.compile(r"^C[01]$")
HEX_BYTES_RE = re.compile(r"(?<![0-9A-Za-z])[0-9A-F]{2}(?: (?:[0-9A-F]{2}|xx)){2,}(?![0-9A-Za-z])")

DATE_ISO_RE = re.compile(r"(?<![0-9])((?:19|20)[0-9]{2})-(0[1-9]|1[0-2])-(0[1-9]|[12][0-9]|3[01])(?![0-9])")
DATE_SLASH_RE = re.compile(r"(?<![0-9/])((?:19|20)[0-9]{2})/(0?[1-9]|1[0-2])/(0?[1-9]|[12][0-9]|3[01])(?![0-9/])")
DATE_KANJI_RE = re.compile(r"(?<![0-9])((?:19|20)[0-9]{2}) ?年 ?(0?[1-9]|1[0-2]) ?月 ?(0?[1-9]|[12][0-9]|3[01]) ?日")
# 月日だけの形は、後ろに日付の後に続く語があるときだけ検出する (値の範囲を検出しない)。スラッシュの形と漢字の形は、
# 件数の比 (12/12) と暦の説明 (2 月 29 日) が多いので、句読点と括弧の前では検出しない。
DATE_MD_WORDS = r"の|に|から|まで|頃|時点|以前|以降|夜|朝|昼"
DATE_MD_RE = re.compile(
    r"(?<![0-9A-Za-z/.:\-])(0[1-9]|1[0-2])-(0[1-9]|[12][0-9]|3[01])(?![0-9/])"
    r"(?= ?(?:" + DATE_MD_WORDS + r"|、|。|\)|）))"
)
DATE_MD_SLASH_RE = re.compile(
    r"(?<![0-9A-Za-z/.:\-])(0[1-9]|1[0-2])/(0[1-9]|[12][0-9]|3[01])(?![0-9/])(?= ?(?:" + DATE_MD_WORDS + r"))"
)
DATE_MD_KANJI_RE = re.compile(
    r"(?<![0-9])(?<!年)(?<!年 )(0?[1-9]|1[0-2]) ?月 ?(0?[1-9]|[12][0-9]|3[01]) ?日"
    r"(?= ?(?:に|から|まで|頃|時点|以前|以降|夜|朝|昼|の(?:回|試し|作業|実機|確認)))"
)
# 月と日の値の範囲 (01 から 12、01 から 31) は日付ではない。
DATE_MD_RANGES = {"01-12", "01-31"}
# 架空の日付は 2020 年より前の年で書く (実際の日付と区別できる)。
FIRST_REAL_YEAR = 2020

# このファイル自身の文字列が一致しないように、語を分けて書く。
# 「ない」をかなで書いた形も、同じラベルとして検出する。
HISTORY_LABEL_RE = re.compile("由来" + r"\s*[:：]" + "|" + "由来" + "の(?:無|な)い判断" + "|" + "退けた" + "案")
DOC_SECTION_RE = re.compile(
    r"付録 ?[A-H](?![A-Za-z])"
    r"|(?<![0-9.])[0-9]{1,2}(?:\.[0-9]+)* ?" + "章"
    + r"|第 ?[0-9]+ ?" + "部"
    + r"|(?<![0-9.])0[0-9] の (?:[0-9]+(?:\.[0-9]+)+|付録)"
)
HISTORY_WORD_RE = re.compile(
    r"(?:^|(?<=[、。\s(「（]))(?:" + "前" + "は|前の(?:版|形|作り))|以" + "前は|直" + "した|や" + "めた|再発" + "防止"
    r"|(?<![A-Za-z])(?:Previously|Formerly|[Rr]ejected alternatives?|[Ww]e used to)(?![A-Za-z])"
)

# ファイルとフォルダーの名前の中の、英字と数の並び (区切りは / . _ - と空白)
NAME_PART_SPLIT_RE = re.compile(r"[/._\-\s]+")
NAME_ID_RE = re.compile(r"[A-Za-z]+[0-9]+[A-Za-z]*")
# 名前に使ってよい英字と数の並び (形式、規格、環境の名前と、数で略した語)
NAME_ALLOW = {
    "x64", "x86", "arm64", "win32", "win64", "utf8", "utf16", "utf32", "base64", "h264", "h265", "x264", "x265",
    "av1", "vp8", "vp9", "mp3", "mp4", "md5", "sha1", "sha256", "sha512", "nv12", "i420", "d3d11", "dx11", "dx12",
    "ipv4", "ipv6", "ps1", "psm1", "psd1", "gplv2", "gplv3", "lgplv2", "lgplv3", "c11", "c17", "cpp17", "cpp20",
    "es5", "es6", "es2020", "es2022", "py3", "a11y", "i18n", "l10n",
}


def id_allowed(token, line, start):
    m = HYPHEN_ID_RE.fullmatch(token)
    if m:
        if start > 0 and line[start - 1] == "[" and CHAR_CLASS_RE.match(line, start):
            return True  # 正規表現の文字クラス
        head = re.match(r"[A-Z]+", m.group(1)).group(0)
        return token in STANDARD_EXACT or head in STANDARD_PREFIXES
    if token in SHORT_NAMES:
        return True
    if FUNCTION_KEY_RE.match(token):
        return bool(re.match(r" ?(?:キー|[Kk]ey)", line[start + len(token):]))
    if CONTROL_SET_RE.match(token):
        return "制御" in line or "control" in line.lower()
    if re.fullmatch(r"[A-F][0-9A-F]", token):
        return any(h.start() <= start < h.end() for h in HEX_BYTES_RE.finditer(line))
    return False


def date_findings(line, tree):
    found = []
    for rx in (DATE_ISO_RE, DATE_SLASH_RE, DATE_KANJI_RE):
        for m in rx.finditer(line):
            if int(m.group(1)) < FIRST_REAL_YEAR or m.group(0) in tree.version_dates:
                continue
            found.append((m.start(), m.group(0)))
    for rx in (DATE_MD_RE, DATE_MD_SLASH_RE, DATE_MD_KANJI_RE):
        for m in rx.finditer(line):
            if m.group(0) not in DATE_MD_RANGES:
                found.append((m.start(), m.group(0)))
    return found


def text_findings(rel_file, line, kind, tree, candidates):
    """(列, 規則, 文字列) のリストを返す。kind は comment、string、doc、data のどれか。"""
    name = rel_file.rsplit("/", 1)[-1]
    found = []
    # 番号を検出する lint のセルフテストの例 (番号の形をした文字列) は、lint の NG 例と OK 例のファイルに置く。
    # テストのファイルの文字列リテラルの中の短い形 (SHORT_ID_RE。メッセージのレベルの名前や、形の違う引数の例) は、
    # テストデータなので検出しない。ハイフンの形の番号は、テストのファイルの文字列の中でも検出する。
    test_string = kind == "string" and is_test_file(rel_file)
    for rx in (HYPHEN_ID_RE, SHORT_ID_RE, CJK_ID_RE):
        if rx is SHORT_ID_RE and test_string:
            continue
        for m in rx.finditer(line):
            if rx is not CJK_ID_RE and id_allowed(m.group(0), line, m.start()):
                continue
            found.append((m.start(), "record-id", m.group(0)))
    if name != "CHANGELOG.md":
        for col, tok in date_findings(line, tree):
            found.append((col, "date", tok))
    for m in HISTORY_LABEL_RE.finditer(line):
        found.append((m.start(), "history-label", m.group(0)))
    if kind != "doc":
        for m in DOC_SECTION_RE.finditer(line):
            found.append((m.start(), "doc-section", m.group(0)))
    if candidates:
        for m in HISTORY_WORD_RE.finditer(line):
            found.append((m.start(), "history-word", m.group(0)))
    return found


def name_findings(rel):
    """ファイルの相対パスの中の、番号の形をした英字と数の並びのリストを返す (同じ並びは 1 回だけ)。"""
    out = []
    for part in NAME_PART_SPLIT_RE.split(rel):
        if NAME_ID_RE.fullmatch(part) and part.lower() not in NAME_ALLOW and part not in out:
            out.append(part)
    return out


# ---- ファイルごとのチェック ----

MESSAGES = {
    "missing": "リポジトリにないファイルかフォルダーを指している",
    "absent": "存在しないものとして扱う (--absent) ファイルかフォルダーを指している",
    "missing-heading": "指している文書に、その見出しがない",
    "record-id": "番号の形をした文字列",
    "name-id": "ファイルかフォルダーの名前に、番号の形をした文字列がある (名前は、何をするものかで付ける)",
    "doc-section": "文書の章、節、付録の番号 (コメントと文字列と設定ファイルでは、参照先を文書の名前と見出しで書く)",
    "date": "経緯を示す日付",
    "history-label": "経緯を示すラベル",
    "history-word": "経緯を示す語の候補 (正しい使い方なら残してよい)",
}


def split_parts(rel, text):
    """行ごとに [(文字列, 種類)] のリストを返す。種類は comment、string、code、doc、data のどれか。

    C 系の言語と Python と PowerShell は、コメント、文字列、コードに分ける (ほかの部分を空白にして、桁を保つ)。
    .md と、# の行を持たない .txt は文書とする。そのほかは、# の行 (ini は ; の行も) をコメント、残りをデータとする。
    """
    name = rel.rsplit("/", 1)[-1]
    ext = os.path.splitext(name)[1].lower()
    lines = text.split("\n")
    if ext in C_LIKE_EXTS | PY_EXTS | PS_EXTS:
        if ext in C_LIKE_EXTS:
            tokens = libobs.tokenize_c_like(text)
        elif ext in PY_EXTS:
            tokens = cj.tokenize_python(text)
        else:
            tokens = cj.tokenize_powershell(text)
        by_kind = {k: libobs.masked_text(text, tokens, keep_kinds={k}).split("\n") for k in ("comment", "string", "code")}
        return [[(by_kind["comment"][i], "comment"), (by_kind["string"][i], "string"), (by_kind["code"][i], "code")]
                for i in range(len(lines))]
    comment_marks = ("#", ";") if ext == ".ini" else ("#",)
    has_comments = any(line.lstrip().startswith(comment_marks) for line in lines)
    if ext == ".md" or (ext == ".txt" and not has_comments and name != "CMakeLists.txt"):
        return [[(line, "doc")] for line in lines]
    return [[(line, "comment" if line.lstrip().startswith(comment_marks) else "data")] for line in lines]


def check_text(rel, text, tree, candidates=False):
    """[(行番号, 規則, 文字列, 理由)] のリストを返す。理由は MESSAGES のキー (missing-path は missing か absent、ほかは
    規則と同じ)。"""
    findings = []
    ignore_list = rel.rsplit("/", 1)[-1] in IGNORE_LIST_NAMES
    for line_no, parts in enumerate(split_parts(rel, text), start=1):
        for part, kind in parts:
            if not part.strip():
                continue
            masked = mask_urls(part)
            prose = kind in ("comment", "doc")
            if not (ignore_list and kind != "comment"):
                for _, tok, reason in path_findings(rel, masked, tree, prose):
                    findings.append((line_no, "missing-path", tok, reason))
            if kind == "code":
                continue
            for _, tok in heading_findings(rel, masked, tree, prose):
                findings.append((line_no, "missing-heading", tok, "missing-heading"))
            for _, rule, tok in text_findings(rel, masked, kind, tree, candidates):
                findings.append((line_no, rule, tok, rule))
    findings.sort(key=lambda f: (f[0], f[1], f[2]))
    return findings


def select_files(tree, paths, skip_third_party=True):
    """チェックするファイルを、(表示する名前, ツリーの中の相対パス, 実際のパス, ツリーの中か) のリストで返す。

    ツリーの外のファイルも、パスを指定すればチェックする (相対パスはファイル名にする)。lint の NG 例と OK 例は除く。
    ただし、--root に fixtures の下のフォルダーを渡したときは、そのツリーのすべてをチェックする。
    """
    root = tree.root
    candidates = sorted(tree.present)
    skip_fixtures = not in_fixtures(root)

    def skipped(rel, full):
        return (skip_third_party and is_third_party(rel)) or (skip_fixtures and in_fixtures(full))

    if not paths:
        return [(rel, rel, root / rel, True) for rel in candidates if not skipped(rel, root / rel)]
    out = []
    seen = set()
    for p in paths:
        p = Path(p).resolve()
        try:
            rel = p.relative_to(root).as_posix()
        except ValueError:
            rel = None
        if rel is None or (p.is_file() and rel not in tree.all_files):
            if p.is_file() and p not in seen:
                seen.add(p)
                out.append((p.as_posix(), rel or p.name, p, False))
            continue
        for f in candidates:
            if rel in ("", ".") or f == rel or f.startswith(rel + "/"):
                full = root / f
                if full in seen or skipped(f, full):
                    continue
                seen.add(full)
                out.append((f, f, full, True))
    return out


def check_file(rel, full, in_tree, tree, candidates=False):
    """1 つのファイルの [(行番号, 規則, 文字列, 理由)] のリストを返す。名前の規則の検出は、行番号を 0 にする。"""
    findings = []
    if in_tree:
        findings.extend((0, "name-id", part, "name-id") for part in name_findings(rel))
    text = read_text(full)
    if text is not None:
        findings.extend(check_text(rel, text, tree, candidates))
    return findings


def run(root, paths, absent_patterns, candidates=False, summary=False, out=print):
    tree = Tree(root, list_tree_files(root), make_absent(absent_patterns))
    counts = {r: 0 for r in RULES + CANDIDATE_RULES}
    selected = select_files(tree, paths)
    total = 0
    for shown, rel, full, in_tree in selected:
        for line_no, rule, tok, reason in check_file(rel, full, in_tree, tree, candidates):
            counts[rule] += 1
            if rule in RULES:
                total += 1
            if not summary:
                out(f"{shown}:{line_no}: {rule}: {MESSAGES[reason]}「{tok}」")
    text = ", ".join(f"{r} {counts[r]}" for r in RULES)
    if candidates:
        text += f" (候補: history-word {counts['history-word']})"
    if total:
        out(f"NG: {len(selected)} ファイルをチェックした。検出は {total} 箇所 ({text})")
        return 1
    out(f"OK: {len(selected)} ファイルをチェックした ({text})")
    return 0


# ---- --selftest ----
#
# fixtures/references/good と bad は、それぞれをリポジトリのルートとみなす架空のツリーである。
# bad の行の「expect: <規則>, <規則>」は、その行で検出されるはずの規則である (同じ規則が 2 回なら 2 回書く)。missing-path は、
# 理由が missing (ツリーにない) のものである。
# good の行の「expect-absent: missing-path」は、--absent に SELFTEST_ABSENT (ツリーにある retired と、ツリーにない gone を
# パターンで書いた gone*) を渡して実行したときだけ検出されるはずの、理由が absent の missing-path である。
# どちらのツリーの「expect-candidate: history-word」も、--candidates を付けたときだけ出力されるはずの候補である。
# 「expect-name: name-id」は、その行があるファイルの名前で検出されるはずの name-id (行番号 0) である。
# 書いていない行では何も検出されない。すべての規則が bad で 1 回以上検出されること、2 つの理由のメッセージが違うこと、
# 終了コードも確認する。

EXPECT_RE = re.compile(r"expect(-absent|-candidate|-name)?:\s*([a-z\-]+(?:\s*,\s*[a-z\-]+)*)")
SELFTEST_ABSENT = ("retired", "gone*")


def label(rule, reason):
    return f"{rule}:{reason}" if rule == "missing-path" else rule


def expectations(root, marker):
    result = {}
    for rel in list_tree_files(root):
        text = read_text(Path(root) / rel)
        if text is None:
            continue
        for line_no, line in enumerate(text.split("\n"), start=1):
            for m in EXPECT_RE.finditer(line):
                kind = m.group(1) or ""
                if kind != marker:
                    continue
                key = (rel, 0) if kind == "-name" else (rel, line_no)
                reason = "absent" if kind == "-absent" else "missing"
                result.setdefault(key, []).extend(label(r.strip(), reason) for r in m.group(2).split(","))
    return {k: sorted(v) for k, v in result.items()}


def collect(root, absent, candidates):
    tree = Tree(root, list_tree_files(root), make_absent(absent))
    got = {}
    for _, rel, full, in_tree in select_files(tree, []):
        for line_no, rule, _, reason in check_file(rel, full, in_tree, tree, candidates):
            got.setdefault((rel, line_no), []).append(label(rule, reason))
    return {k: sorted(v) for k, v in got.items()}


def merged(*parts):
    out = {}
    for part in parts:
        for key, labels in part.items():
            out[key] = sorted(out.get(key, []) + labels)
    return out


def compare(name, got, want, failures):
    for key in sorted(set(got) | set(want)):
        if got.get(key, []) != want.get(key, []):
            failures.append(f"{name} {key[0]}:{key[1]}: 検出 {got.get(key, [])}、期待 {want.get(key, [])}")


def run_selftest():
    good = FIXTURES_DIR / "good"
    bad = FIXTURES_DIR / "bad"
    failures = []
    if not good.is_dir() or not bad.is_dir() or not list_tree_files(good) or not list_tree_files(bad):
        print("selftest 失敗: NG 例か OK 例がない")
        return 1
    if MESSAGES["missing"] == MESSAGES["absent"]:
        failures.append("missing と absent のメッセージが同じ (どちらの理由で検出したかが分からない)")
    want_bad = merged(expectations(bad, ""), expectations(bad, "-name"))
    if not want_bad:
        failures.append("bad に expect の行がない")
    compare("bad", collect(bad, [], False), want_bad, failures)
    compare("good", collect(good, [], False), {}, failures)
    want_absent = expectations(good, "-absent")
    if not want_absent:
        failures.append("good に expect-absent の行がない (--absent を確認できない)")
    compare("good --absent", collect(good, list(SELFTEST_ABSENT), False), want_absent, failures)
    for root, name, base in ((good, "good", {}), (bad, "bad", want_bad)):
        want = merged(base, expectations(root, "-candidate"))
        compare(f"{name} --candidates", collect(root, [], True), want, failures)
    if not expectations(good, "-candidate"):
        failures.append("good に expect-candidate の行がない (候補が終了コードを変えないことを確認できない)")
    seen = {lab.split(":")[0] for labels in want_bad.values() for lab in labels}
    for rule in RULES:
        if rule not in seen:
            failures.append(f"規則 {rule} を検出する NG 例がない")
    with contextlib.redirect_stdout(io.StringIO()):
        bad_exit = main(["--root", str(bad)])
        good_exit = main(["--root", str(good), "--candidates"])
        absent_exit = main(["--root", str(good)] + [a for name in SELFTEST_ABSENT for a in ("--absent", name)])
    if (bad_exit, good_exit, absent_exit) != (1, 0, 1):
        failures.append(f"終了コード: bad {bad_exit}、good (--candidates) {good_exit}、good (--absent) {absent_exit}")
    for failure in failures:
        print(f"selftest 失敗: {failure}")
    if failures:
        return 1
    print(f"selftest OK: references (NG 例の行 {len(want_bad)}、規則 {len(RULES)} 種類と候補)")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="リポジトリにないファイルと見出しへの参照、番号の形をした文字列、経緯を示す日付とラベルを検出する。"
                    "パスを省くとリポジトリ全体をチェックし、検出があれば 1 で終わる")
    parser.add_argument("paths", nargs="*", help="チェックするファイルかフォルダー (省くとリポジトリ全体。存在の判定はリポジトリ全体で行う)")
    parser.add_argument("--absent", action="append", default=[], metavar="PATTERN",
                        help="まだあっても、存在しないものとして扱うパス (ルートからの相対パス。* などのパターンも書ける。複数指定できる)")
    parser.add_argument("--absent-file", action="append", default=[], metavar="FILE",
                        help="--absent の PATTERN を 1 行に 1 つ書いたファイル (# の行と空行は読み飛ばす)")
    parser.add_argument("--candidates", action="store_true", help="経緯を示す語の候補も出力する (終了コードは変えない)")
    parser.add_argument("--summary", action="store_true", help="規則ごとの件数だけを出力する")
    parser.add_argument("--root", default=str(REPO_ROOT), help="リポジトリのルートとみなすフォルダー")
    parser.add_argument("--selftest", action="store_true", help="fixtures/references の NG 例と OK 例でセルフテストを実行する")
    args = parser.parse_args(argv)
    if args.selftest:
        return run_selftest()
    patterns = list(args.absent)
    for f in args.absent_file:
        patterns.extend(load_absent_file(f))
    return run(Path(args.root), args.paths, patterns, args.candidates, args.summary)


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass
    sys.exit(main())
