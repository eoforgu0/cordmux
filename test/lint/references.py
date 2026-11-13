#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# コメント、文書、文字列、設定ファイルに、読み手が開けないものへの参照 (リポジトリにないファイルとフォルダー、文書に
# ない見出し) がないかをチェックする。規則は RULES にあり、どの規則で検出しても終了コードが 1 になる。許可するものと
# 使い方は test/lint/README.md にある。
# チェックするのは、git が追跡しているファイルと、追跡していないが無視もしていないファイルである。外部からコピーした
# ファイルと、lint の NG 例と OK 例はチェックしない。

import argparse
import contextlib
import fnmatch
import io
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

RULES = ("missing-path", "missing-heading")

# 日本語の文字の範囲 (ひらがなとカタカナ、漢字)。正規表現の文字クラスの中に置く。
KANA = chr(0x3041) + "-" + chr(0x30FF)
KANJI = chr(0x4E00) + "-" + chr(0x9FFF)

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


# ---- ツリーの索引 ----

MD_HEADING_RE = re.compile(r"^ {0,3}#{1,6}[ \t]+(.*?)(?:[ \t]+#+)?[ \t]*$")
FENCE_RE = re.compile(r"^ {0,3}(?:`{3,}|~{3,})")


class Tree:
    def __init__(self, root, files):
        self.root = Path(root).resolve()
        self.files = set(files)
        self.top_names = {f.split("/")[0] for f in files}
        self.dirs = self._dirs(self.files)
        # ファイル名だけと末尾の一致で探すのは文書なので、fixtures の下のファイルでは探さない (NG 例と OK 例のファイル名が、
        # 文書の名前と同じことがある)。出力するファイルの名前も、fixtures の下からは取らない。
        self.own_files = {f for f in self.files if "fixtures" not in f.split("/")[:-1]}
        self.suffixes = self._suffixes(self.own_files)
        self.basenames = {f.rsplit("/", 1)[-1] for f in self.own_files}
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

    def resolves(self, rel):
        if rel in self.files or rel in self.dirs:
            return True
        # 拡張子を省いた参照 (JavaScript と TypeScript の import の参照先など)
        if rel not in self._stems:
            prefix = rel + "."
            self._stems[rel] = any(f.startswith(prefix) and "/" not in f[len(prefix):] for f in self.files)
        return self._stems[rel]

    def output_names(self):
        """ツールが出力するファイルの名前 (コードの中にその名前だけの文字列として現れ、ツリーにはない名前) の集合を返す。"""
        if self._output_names is None:
            names = set()
            rx = re.compile(r"""["'`]([A-Za-z0-9_.\-]+\.[A-Za-z0-9]+)["'`]""")
            for rel in self.own_files:
                ext = os.path.splitext(rel)[1].lower()
                if ext not in C_LIKE_EXTS | PY_EXTS | PS_EXTS:
                    continue
                text = read_text(self.root / rel)
                if text is None:
                    continue
                names.update(rx.findall(text))
            all_names = {f.rsplit("/", 1)[-1] for f in self.files}
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
            if t in self.files:
                return t
        if segs[0] in (".", ".."):
            return None
        hits = [f for f in self.own_files if f == path or f.endswith("/" + path)]
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
    """(列, パス) のリストを返す。prose はコメントと文書のとき真で、./ と ../ と .tsv もチェックする。"""
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
            if any(s in BUILD_DIR_NAMES for t in targets for s in t.split("/")):
                continue
            found.append((m.start(1), tok))
            continue
        target_segs = [s for s in segs if s != "."]
        target = "/".join(target_segs)
        if prose and base_dir and tree.resolves(base_dir + "/" + target):
            continue  # 文書とコメントの中の、そのファイルの場所から書いたパス (Markdown の相対リンクと同じ)
        if target_segs[0] in tree.top_names:
            if tree.resolves(target) or any(s in BUILD_DIR_NAMES for s in target_segs):
                continue
            found.append((m.start(1), tok))
            continue
        last = target_segs[-1].lower()
        if not (last.endswith(".md") or (prose and last.endswith(".tsv") and len(target_segs) > 1)):
            continue
        if any(s in BUILD_DIR_NAMES for s in target_segs):
            continue
        if len(target_segs) == 1:
            if target in tree.basenames or target in tree.output_names():
                continue
        elif target in tree.suffixes or target_segs[-1] in tree.output_names():
            continue
        found.append((m.start(1), tok))
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


# ---- ファイルごとのチェック ----

MESSAGES = {
    "missing-path": "リポジトリにないファイルかフォルダーを指している",
    "missing-heading": "指している文書に、その見出しがない",
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


def check_text(rel, text, tree):
    """[(行番号, 規則, 文字列)] のリストを返す。"""
    findings = []
    ignore_list = rel.rsplit("/", 1)[-1] in IGNORE_LIST_NAMES
    for line_no, parts in enumerate(split_parts(rel, text), start=1):
        for part, kind in parts:
            if not part.strip():
                continue
            masked = mask_urls(part)
            prose = kind in ("comment", "doc")
            if not (ignore_list and kind != "comment"):
                for _, tok in path_findings(rel, masked, tree, prose):
                    findings.append((line_no, "missing-path", tok))
            if kind == "code":
                continue
            for _, tok in heading_findings(rel, masked, tree, prose):
                findings.append((line_no, "missing-heading", tok))
    findings.sort()
    return findings


def select_files(tree, paths):
    """チェックするファイルを、(表示する名前, ツリーの中の相対パス, 実際のパス) のリストで返す。

    ツリーの外のファイルも、パスを指定すればチェックする (相対パスはファイル名にする)。外部からコピーしたファイルと、
    lint の NG 例と OK 例は除く。ただし、--root に fixtures の下のフォルダーを渡したときは、そのツリーの NG 例と OK 例も
    チェックする。
    """
    root = tree.root
    candidates = sorted(tree.files)
    skip_fixtures = not in_fixtures(root)

    def skipped(rel, full):
        return is_third_party(rel) or (skip_fixtures and in_fixtures(full))

    if not paths:
        return [(rel, rel, root / rel) for rel in candidates if not skipped(rel, root / rel)]
    out = []
    seen = set()
    for p in paths:
        p = Path(p).resolve()
        try:
            rel = p.relative_to(root).as_posix()
        except ValueError:
            rel = None
        if rel is None or (p.is_file() and rel not in tree.files):
            if p.is_file() and p not in seen:
                seen.add(p)
                out.append((p.as_posix(), rel or p.name, p))
            continue
        for f in candidates:
            if rel in ("", ".") or f == rel or f.startswith(rel + "/"):
                full = root / f
                if full in seen or skipped(f, full):
                    continue
                seen.add(full)
                out.append((f, f, full))
    return out


def check_file(rel, full, tree):
    """1 つのファイルの [(行番号, 規則, 文字列)] のリストを返す。"""
    text = read_text(full)
    return [] if text is None else check_text(rel, text, tree)


def run(root, paths, summary=False, out=print):
    tree = Tree(root, list_tree_files(root))
    counts = {r: 0 for r in RULES}
    selected = select_files(tree, paths)
    for shown, rel, full in selected:
        for line_no, rule, tok in check_file(rel, full, tree):
            counts[rule] += 1
            if not summary:
                out(f"{shown}:{line_no}: {rule}: {MESSAGES[rule]}「{tok}」")
    total = sum(counts.values())
    text = ", ".join(f"{r} {counts[r]}" for r in RULES)
    if total:
        out(f"NG: {len(selected)} ファイルをチェックした。検出は {total} 箇所 ({text})")
        return 1
    out(f"OK: {len(selected)} ファイルをチェックした ({text})")
    return 0


# ---- --selftest ----
#
# fixtures/references/good と bad は、それぞれをリポジトリのルートとみなす架空のツリーである。
# bad の行の「expect: <規則>, <規則>」は、その行で検出されるはずの規則である (同じ規則が 2 回なら 2 回書く)。書いていない
# 行と、good のどの行でも何も検出されない。すべての規則が bad で 1 回以上検出されることと、終了コードも確認する。

EXPECT_RE = re.compile(r"expect:\s*([a-z\-]+(?:\s*,\s*[a-z\-]+)*)")


def expectations(root):
    result = {}
    for rel in list_tree_files(root):
        text = read_text(Path(root) / rel)
        if text is None:
            continue
        for line_no, line in enumerate(text.split("\n"), start=1):
            for m in EXPECT_RE.finditer(line):
                result.setdefault((rel, line_no), []).extend(r.strip() for r in m.group(1).split(","))
    return {k: sorted(v) for k, v in result.items()}


def collect(root):
    tree = Tree(root, list_tree_files(root))
    got = {}
    for _, rel, full in select_files(tree, []):
        for line_no, rule, _ in check_file(rel, full, tree):
            got.setdefault((rel, line_no), []).append(rule)
    return {k: sorted(v) for k, v in got.items()}


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
    want_bad = expectations(bad)
    if not want_bad:
        failures.append("bad に expect の行がない")
    compare("bad", collect(bad), want_bad, failures)
    compare("good", collect(good), {}, failures)
    seen = {rule for rules in want_bad.values() for rule in rules}
    for rule in RULES:
        if rule not in seen:
            failures.append(f"規則 {rule} を検出する NG 例がない")
    with contextlib.redirect_stdout(io.StringIO()):
        bad_exit = main(["--root", str(bad)])
        good_exit = main(["--root", str(good)])
    if (bad_exit, good_exit) != (1, 0):
        failures.append(f"終了コード: bad {bad_exit}、good {good_exit}")
    for failure in failures:
        print(f"selftest 失敗: {failure}")
    if failures:
        return 1
    print(f"selftest OK: references (NG 例の行 {len(want_bad)}、規則 {len(RULES)} 種類)")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="リポジトリにないファイルと文書の見出しへの参照を検出する。"
                    "パスを省くとリポジトリ全体をチェックし、検出があれば 1 で終わる")
    parser.add_argument("paths", nargs="*", help="チェックするファイルかフォルダー (省くとリポジトリ全体。存在の判定はリポジトリ全体で行う)")
    parser.add_argument("--summary", action="store_true", help="規則ごとの件数だけを出力する")
    parser.add_argument("--root", default=str(REPO_ROOT), help="リポジトリのルートとみなすフォルダー")
    parser.add_argument("--selftest", action="store_true", help="fixtures/references の NG 例と OK 例でセルフテストを実行する")
    args = parser.parse_args(argv)
    if args.selftest:
        return run_selftest()
    return run(Path(args.root), args.paths, args.summary)


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass
    sys.exit(main())
