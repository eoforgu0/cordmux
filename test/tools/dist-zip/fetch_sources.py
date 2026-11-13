#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# 同梱するソフトウェアのソースを取得してコンポーネントごとに 1 つのアーカイブにし、それを 1 つの zip (リリースに添付する
# ファイル) にまとめる。取得するものの一覧は持たず、すでにある事実から作る: obs-deps のタグ (src-obs/buildspec.json)、その中の
# deps.ffmpeg/*.ps1 の param ブロック、src-obs/ws/ws_deps.cmake の FetchContent_Declare、src-vencord/vencord.lock.json。
# 取得する前に、その一覧とソースコードの入手先.txt のコンポーネントの行を突き合わせ、合わなければ何も取得せずに失敗する。
# 結果は 1 行の JSON (ok、out、zip、bytes、sha256、obs_deps_commit、files、skipped、error)。ネットワークに接続するのは
# --fetch のときだけ。引数は README.md と --help にある。

import argparse
import gzip
import hashlib
import io
import os
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import xml.etree.ElementTree as ElementTree
import zipfile
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_info  # noqa: E402  (タグの形を build_info.TAG_RE の 1 か所に保つ)
import dist_common  # noqa: E402  (ツールが共有する関数と定数)
from dist_common import DistError  # noqa: E402

WS_DEPS_CMAKE = dist_common.REPO_ROOT / "src-obs" / "ws" / "ws_deps.cmake"
# Vencord の取得元の URL。vencord.lock.json はコミットとタグしか持たないので、.github/workflows/vencord-plugin.yml の
# 「Vencord を固定のコミットで取得する」のステップと同じ URL をここに置く。URL を変えるときは、リポジトリの中で
# 同じ URL を書いた箇所を全部直す。
VENCORD_URI = "https://github.com/Vendicated/Vencord.git"
VENCORD_REQUIRED_PATHS = ("pnpm-lock.yaml",)
OBS_DEPS_PART_NAME = "obs-deps"
OBS_DEPS_RELEASES_SUFFIX = "/releases/download"
SCRIPTS_DIR = "deps.ffmpeg"
SHIPPED_TARGET = "x64"
# deps.ffmpeg の script のうち、同梱する DLL に入らないもの (ps1 の Name -> 理由)。obs-deps 2025-07-11 の bin で
# 確認した。
#   libpng: 同梱する DLL (src-obs/cmake/cordmux_ffmpeg_runtime.cmake の一覧) に libpng の DLL はなく、FFmpeg の
#           configure (ffmpeg.exe -version の出力) にも出ず、FFmpeg の 7 つの DLL に libpng の関数名と
#           バージョンの文字列がない。
#   libdatachannel: obs-deps の bin では別の datachannel.dll で、同梱する一覧にない (OBS 自身の WebRTC の出力が使う)。
#           FFmpeg の 7 つの DLL にも名前がない。
# 同梱するものの一覧ではなく、同梱しないものの一覧にする。FFmpeg の configure から同梱するものを導く形にすると、
# configure に出ないもの (FFmpeg が自動で見つける nv-codec-headers と AMF、ほかのライブラリの中に静的に入る ogg と
# mbedtls) の表が別に必要になるうえ、obs-deps がコンポーネントを追加したときに、そのコンポーネントが気付かれないまま
# 外れるため。同梱しないものの一覧なら、追加されたコンポーネントはソースコードの入手先.txt に行がないので失敗し、人が判断できる。
NOT_SHIPPED_SCRIPTS = {
    "libpng": "同梱する DLL のどれにも入っていない (FFmpeg の configure にも DLL にも libpng がない)",
    "libdatachannel": "同梱しない別の datachannel.dll (OBS 自身が使う)",
}
# ソースをまとめた zip に専用のアーカイブを入れない行 (dist_common.SOURCE_PARTS_WITHOUT_TARBALL) の、ソースのある場所
# (一覧のテキストに書く)。None は GitHub がタグに付ける Source code のアーカイブ。名前は、その名前の行のアーカイブの中。
WITHOUT_TARBALL_PLACES = {"cordmux": None, "userplugin": None, "pnpm-lock.yaml": "Vencord"}
ARCHIVE_SUFFIXES =(".tar.gz", ".tar.xz", ".tar.bz2", ".tgz", ".zip", ".tar")
FULL_COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")
COMMIT_IN_TEXT_RE = re.compile(r"(?<![0-9a-f])[0-9a-f]{40}(?![0-9a-f])")
PARAM_START_RE = re.compile(r"^\s*param\s*\(", re.I | re.M)
STRING_PARAM_RE = re.compile(r"\[string\]\s*\$(\w+)\s*=\s*(['\"])(.*?)\2")
TARGETS_RE = re.compile(r"\[array\]\s*\$Targets\s*=\s*@\(([^)]*)\)")
SPARSE_ARGS_RE = re.compile(r"-SparseArgs\s+(\S+)")
SCRIPT_ROOT_TOKEN = "${PSScriptRoot}/"
CMAKE_DECLARE_RE = re.compile(r"FetchContent_Declare\(\s*(\w+)(.*?)\)", re.S)
CMAKE_URL_RE = re.compile(r"^\s*URL\s+(\S+)", re.M)
CMAKE_HASH_RE = re.compile(r"^\s*URL_HASH\s+SHA256=([0-9A-Fa-f]{64})\s*$", re.M)
CMAKE_NAME_PREFIX = "cordmux_"
CLIXML_NS = "{http://schemas.microsoft.com/powershell/2004/04}"
VERSION_SAFE_RE = re.compile(r"[^A-Za-z0-9._-]")

# git に渡す設定 (git_env が GIT_CONFIG_COUNT、GIT_CONFIG_KEY_<n>、GIT_CONFIG_VALUE_<n> にする)。
# core.longpaths: Windows で作業フォルダーのパスが長いと、submodule の objects のパスが 260 文字を超えて書けない
# (Filename too long)。Linux では動作しない。ただし、これを渡しても、submodule の GIT_DIR のパスが長いと、git は
# '$GIT_DIR' too big で submodule の clone に失敗する (作業フォルダーのパスが 190 文字ほどになると起きる)。
# セルフテストは file:// の submodule を許す設定を加え、終わったら外す。
GIT_CONFIG_PAIRS: list = [("core.longpaths", "true")]
SELFTEST_GIT_CONFIG = ("protocol.file.allow", "always")


class Part:
    # 取得するコンポーネント 1 つ。kind は "git" か "archive"。

    def __init__(self, name: str, origin: str, kind: str, uri: str, version: str = "", commit: str = "",
                 want: str = "", sha256: str = "", sparse: tuple = (), required: tuple = ()):
        self.name = name            # 事実を読んだファイルでの名前 (ps1 の Name など)
        self.origin = origin        # どこから読んだか (結果の JSON とエラーの文に出力する)
        self.kind = kind
        self.uri = uri
        self.version = version
        self.commit = commit        # 40 桁 (分かっていれば)
        self.want = want or commit  # git で取得するもの (コミットか refs/tags/...)
        self.sha256 = sha256        # アーカイブの照合に使う SHA256 (小文字)
        self.sparse = sparse        # 一部だけを取得するパス (空なら全部)
        self.required = required    # 中になければエラーにするパス
        self.offer_name = ""        # 突き合わせで決まる、ソースコードの入手先.txt のコンポーネントの名前

    def pin(self) -> str:
        # ソースコードの入手先.txt のコンポーネントの行に書いてあるはずの文字列 (コミット、なければバージョン)。
        return self.commit or self.version


# ---- 事実を読む -----------------------------------------------------------------------


def label_ok(label: str) -> bool:
    # タグから v を除いた文字列か (build_info.TAG_RE に v を付けて確認する)。
    return bool(build_info.TAG_RE.match("v" + label))


def default_source_offer() -> Path:
    # 突き合わせの相手の既定: top_level_files.txt の source-offer の行のファイル (日本語のファイル名は
    # top_level_files.txt にだけ書く)。
    names = [name for kind, name in dist_common.top_level_entries() if kind == "source-offer"]
    if len(names) != 1:
        raise DistError(f"top_level_files.txt: source-offer の行は 1 つのはずが {len(names)} 行ある")
    return dist_common.DEFAULT_DIST_DIR / names[0]


def offer_part_names(text: str) -> list[str]:
    # ソースコードの入手先.txt のコンポーネントの行 (「- 名前 ...」) の最初の語の並び。
    return [name for name, _ in offer_entries(text)]


def offer_entries(text: str) -> list[tuple[str, str]]:
    # コンポーネントの行を (最初の語, 行) の並びにする。
    entries = []
    for line in text.split("\n"):
        line = line.rstrip("\r")
        if line.startswith("- "):
            words = line[2:].split(" ", 1)
            if words[0]:
                entries.append((words[0], line))
    return entries


def obs_deps_facts(buildspec_path: Path = dist_common.BUILDSPEC_PATH) -> tuple[str, str]:
    # obs-deps のリポジトリの URL (.git) とタグ。baseUrl はリリースのダウンロードの URL なので、後ろを外して
    # リポジトリの URL にする。
    prebuilt = dist_common.load_json(buildspec_path)["dependencies"]["prebuilt"]
    base = prebuilt["baseUrl"].rstrip("/")
    if not base.endswith(OBS_DEPS_RELEASES_SUFFIX):
        raise DistError(f"buildspec.json: prebuilt の baseUrl が {OBS_DEPS_RELEASES_SUFFIX} で終わらない: {base}")
    return base[: -len(OBS_DEPS_RELEASES_SUFFIX)] + ".git", prebuilt["version"]


def param_block(text: str) -> str:
    # ps1 の先頭の param( ... ) の中身を返す (括弧の対応は、引用符の中を数えない)。
    match = PARAM_START_RE.search(text)
    if not match:
        raise DistError("param( のブロックがない")
    depth = 0
    quote = ""
    start = match.end() - 1
    for index in range(start, len(text)):
        char = text[index]
        if quote:
            if char == quote:
                quote = ""
            continue
        if char in "'\"":
            quote = char
        elif char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return text[start + 1 : index]
    raise DistError("param( のブロックが閉じていない")


def parse_script(text: str) -> dict:
    # ps1 の param ブロックと、一部だけを取得する checkout のパス (-SparseArgs set,<パス>) を読む。
    block = param_block(text)
    values = {name: value for name, _, value in STRING_PARAM_RE.findall(block)}
    missing = [key for key in ("Name", "Version", "Uri", "Hash") if not values.get(key)]
    if missing:
        raise DistError(f"param ブロックに {missing} がない")
    targets = TARGETS_RE.search(block)
    if not targets:
        raise DistError("param ブロックに $Targets がない")
    sparse: tuple = ()
    sparse_match = SPARSE_ARGS_RE.search(text)
    if sparse_match:
        args = [arg.strip().strip("'\"") for arg in sparse_match.group(1).split(",")]
        if len(args) < 2 or args[0] != "set":
            raise DistError(f"知らない形の -SparseArgs {sparse_match.group(1)!r}")
        sparse = tuple(args[1:])
    return {
        "name": values["Name"],
        "version": values["Version"],
        "uri": values["Uri"],
        "hash": values["Hash"],
        "targets": re.findall(r"'([^']+)'|\"([^\"]+)\"", targets.group(1)),
        "sparse": sparse,
    }


def script_targets(info: dict) -> list[str]:
    return [a or b for a, b in info["targets"]]


def read_checksum(path: Path) -> str:
    # obs-deps の checksums/*.sha256 (PowerShell の Export-Clixml で書いた FileHashInfo) から SHA256 を読む。
    root = ElementTree.fromstring(path.read_text(encoding="utf-8-sig"))
    fields = {elem.get("N"): (elem.text or "").strip() for elem in root.iter(f"{CLIXML_NS}S")}
    if fields.get("Algorithm", "SHA256") != "SHA256":
        raise DistError(f"{path.name}: アルゴリズム {fields.get('Algorithm')} が SHA256 でない")
    digest = fields.get("Hash", "")
    if not re.fullmatch(r"[0-9A-Fa-f]{64}", digest):
        raise DistError(f"{path.name}: SHA256 のハッシュがない")
    return digest.lower()


def plan_from_scripts(scripts_dir: Path) -> tuple[list[Part], list[dict]]:
    # deps.ffmpeg/*.ps1 から、取得するコンポーネントと、外した script (理由付き) を返す。
    # Uri が .git で終われば git のコンポーネント (Hash はコミット)、ほかは上流のアーカイブ (Hash は obs-deps の checksums/ の
    # ファイル)。この分け方は、obs-deps の utils.pwsh の Setup-Dependency.ps1 と同じ規則である。x64 向けに
    # ビルドしない script (Targets に x64 がない) は、x64 の DLL しか配らないので、NOT_SHIPPED_SCRIPTS に書かずに外す。
    parts: list[Part] = []
    skipped: list[dict] = []
    scripts = sorted(scripts_dir.glob("*.ps1"))
    if not scripts:
        raise DistError(f"{scripts_dir} の下に *.ps1 がない")
    for path in scripts:
        origin = f"{SCRIPTS_DIR}/{path.name}"
        try:
            info = parse_script(path.read_text(encoding="utf-8-sig"))
        except DistError as exc:
            raise DistError(f"{origin}: {exc}") from exc
        if SHIPPED_TARGET not in script_targets(info):
            skipped.append({"script": origin, "reason": f"{SHIPPED_TARGET} 向けにビルドしない"})
            continue
        if info["name"] in NOT_SHIPPED_SCRIPTS:
            skipped.append({"script": origin, "reason": NOT_SHIPPED_SCRIPTS[info["name"]]})
            continue
        uri, digest = info["uri"], info["hash"]
        if uri.endswith(".git"):
            if not FULL_COMMIT_RE.match(digest):
                raise DistError(f"{origin}: git のコンポーネントなのに 40 桁のコミットがない: {digest!r}")
            parts.append(Part(info["name"], origin, "git", uri, info["version"], commit=digest, sparse=info["sparse"]))
        else:
            if not digest.startswith(SCRIPT_ROOT_TOKEN):
                raise DistError(f"{origin}: アーカイブのコンポーネントなのに checksums のファイルを指していない: {digest!r}")
            checksum = scripts_dir / digest[len(SCRIPT_ROOT_TOKEN) :]
            if not checksum.is_file():
                raise DistError(f"{origin}: checksums のファイル {checksum.name} がない")
            parts.append(Part(info["name"], origin, "archive", uri, info["version"], sha256=read_checksum(checksum)))
    return parts, skipped


def plan_from_ws_deps(text: str, origin: str = "src-obs/ws/ws_deps.cmake") -> list[Part]:
    # ws_deps.cmake の FetchContent_Declare (URL と URL_HASH) をコンポーネントにする。
    parts = []
    for name, body in CMAKE_DECLARE_RE.findall(text):
        url = CMAKE_URL_RE.search(body)
        digest = CMAKE_HASH_RE.search(body)
        if not url or not digest:
            raise DistError(f"{origin}: {name} に URL か URL_HASH SHA256 がない")
        commit = COMMIT_IN_TEXT_RE.search(url.group(1))
        short = name[len(CMAKE_NAME_PREFIX) :] if name.startswith(CMAKE_NAME_PREFIX) else name
        parts.append(Part(short, f"{origin} {name}", "archive", url.group(1),
                          commit=commit.group(0) if commit else "", sha256=digest.group(1).lower()))
    if not parts:
        raise DistError(f"{origin}: FetchContent_Declare がない")
    return parts


def vencord_part(uri: str, commit: str, tag: str) -> Part:
    return Part("Vencord", "src-vencord/vencord.lock.json", "git", uri, tag, commit=commit, required=VENCORD_REQUIRED_PATHS)


# ---- 突き合わせ ------------------------------------------------------------------------


def part_key(name: str) -> str:
    # 名前を英数字だけにし、先頭の lib を外す (libogg と ogg、libvpx と vpx を同じにする)。
    word = re.sub(r"[^a-z0-9]", "", name.lower())
    return word[3:] if word.startswith("lib") and len(word) > 3 else word


def match_parts(parts: list[Part], offer_text: str, require_every_line: bool = True) -> list[str]:
    # コンポーネントとソースコードの入手先.txt のコンポーネントの行を突き合わせ、part.offer_name を決める。問題の文の並びを返す。
    #
    # 名前 (part_key) -> コミット -> Uri の順に探し、最初に見つかった段階で行が 1 つだけなら対応とする。名前だけで
    # 探さないのは、名前が違うコンポーネントがあるため (lame の行の名前は mp3lame、cmake の json の行は nlohmann/json)。
    # その場合も、行に同じ Uri かコミットがある。対応した行には、そのコンポーネントのコミット (なければバージョン) が
    # 書いてあることも確認する (行が古いままなら問題にする)。dist_common.SOURCE_PARTS_WITHOUT_TARBALL の行は
    # 突き合わせから外す。require_every_line が真なら、コンポーネントと対応しない行も問題にする。
    entries = [(name, line) for name, line in offer_entries(offer_text)
               if name not in dist_common.SOURCE_PARTS_WITHOUT_TARBALL]
    problems: list[str] = []
    names = [name for name, _ in entries]
    for name in sorted({n for n in names if names.count(n) > 1}):
        problems.append(f"ソースコードの入手先.txt: コンポーネントの行 {name!r} が 2 回以上ある")
    used: dict[str, str] = {}
    for part in parts:
        tiers = [
            [e for e in entries if part_key(e[0]) == part_key(part.name)],
            [e for e in entries if part.commit and part.commit in e[1]],
            [e for e in entries if part.uri and part.uri in e[1]],
        ]
        chosen = next((tier for tier in tiers if tier), [])
        if not chosen:
            problems.append(f"{part.origin}: コンポーネント {part.name!r} の行がソースコードの入手先.txt にない")
            continue
        if len(chosen) > 1:
            problems.append(f"{part.origin}: コンポーネント {part.name!r} が複数の行に対応する: {[n for n, _ in chosen]}")
            continue
        name, line = chosen[0]
        if name in used:
            problems.append(f"ソースコードの入手先.txt の行 {name!r} が、{used[name]} と {part.origin} の両方に対応する")
            continue
        used[name] = part.origin
        if part.pin() and part.pin() not in line:
            problems.append(f"ソースコードの入手先.txt の行 {name!r} に {part.pin()} が書いていない ({part.origin})")
        part.offer_name = name
    if require_every_line:
        for name, _ in entries:
            if name not in used:
                problems.append(f"ソースコードの入手先.txt の行 {name!r} に対応するコンポーネントがない")
    slugs = [dist_common.source_part_slug(p.offer_name) for p in parts if p.offer_name]
    for slug in sorted({s for s in slugs if slugs.count(s) > 1}):
        problems.append(f"ソースの zip の中のアーカイブの名前の {{part}} が {slug!r} になるコンポーネントが 2 つ以上ある")
    return problems


# ---- 取得する --------------------------------------------------------------------------


def remove_tree(path: Path) -> None:
    # git の objects は読み取り専用なので、削除できなければ書き込める属性にしてから削除し直す (Windows)。
    def retry(func, target, _exc):
        os.chmod(target, stat.S_IWRITE)
        func(target)

    if path.exists():
        shutil.rmtree(path, onexc=retry)


def git_env() -> dict:
    # git を起動するときの環境変数。呼び出し元がすでに GIT_CONFIG_COUNT で渡している設定は残し、その後ろに加える。
    env = dict(os.environ, GIT_TERMINAL_PROMPT="0")
    count = env.get("GIT_CONFIG_COUNT", "0")
    base = int(count) if count.isdigit() else 0
    for offset, (key, value) in enumerate(GIT_CONFIG_PAIRS):
        env[f"GIT_CONFIG_KEY_{base + offset}"] = key
        env[f"GIT_CONFIG_VALUE_{base + offset}"] = value
    env["GIT_CONFIG_COUNT"] = str(base + len(GIT_CONFIG_PAIRS))
    return env


def git(args: list[str], cwd: Path, check: bool = True) -> subprocess.CompletedProcess:
    env = git_env()
    completed = subprocess.run(["git", *args], cwd=str(cwd), capture_output=True, text=True, env=env)
    if check and completed.returncode != 0:
        raise DistError(f"git {' '.join(args)} が {cwd.name} で失敗した: {completed.stderr.strip()[-400:]}")
    return completed


def fetch_git(part: Part, dest: Path) -> str:
    # part を dest に取得して checkout し、submodule も取得する。取得したコミットを返す。
    # 履歴は 1 つだけ取得し (--depth 1)、上流に断られたら全部を取得し直す。submodule は obs-deps と同じく
    # 再帰で取得する (mbedtls 3.6.4 は framework の submodule を持つ)。part.sparse があれば、obs-deps が使うのは
    # そのパスだけなので、--filter=blob:none で取得して、そのパスだけを checkout する (今は AMF の公開の
    # ヘッダー amf/public/include)。
    dest.mkdir(parents=True)
    git(["init", "-q"], dest)
    git(["remote", "add", "origin", part.uri], dest)
    filters = ["--filter=blob:none"] if part.sparse else []
    shallow = git(["fetch", "--quiet", "--no-tags", "--depth", "1", *filters, "origin", part.want], dest, check=False)
    if shallow.returncode == 0:
        commit = git(["rev-parse", "FETCH_HEAD^{commit}"], dest).stdout.strip()
    else:
        git(["fetch", "--quiet", "--tags", *filters, "origin"], dest)
        resolved = git(["rev-parse", "--verify", "--quiet", f"{part.want}^{{commit}}"], dest, check=False)
        if resolved.returncode != 0:
            raise DistError(f"{part.origin}: {part.uri} に {part.want} がない")
        commit = resolved.stdout.strip()
    if part.commit and commit != part.commit:
        raise DistError(f"{part.origin}: {part.commit} のはずが {commit} を取得した")
    if part.sparse:
        git(["sparse-checkout", "set", *part.sparse], dest)
    git(["-c", "advice.detachedHead=false", "checkout", "-q", "--detach", commit], dest)
    if not part.sparse and (dest / ".gitmodules").is_file():
        submodules = git(["submodule", "update", "--init", "--recursive", "--depth", "1"], dest, check=False)
        if submodules.returncode != 0:
            git(["submodule", "update", "--init", "--recursive"], dest)
    for required in part.required:
        if git(["cat-file", "-e", f"{commit}:{required}"], dest, check=False).returncode != 0:
            raise DistError(f"{part.origin}: {commit} に {required} がない")
    return commit


def submodule_paths(repo: Path) -> list[str]:
    # checkout した submodule のパス (再帰。`repo` からの相対パス)。
    if not (repo / ".gitmodules").is_file():
        return []
    paths = []
    for line in git(["submodule", "status", "--recursive"], repo).stdout.split("\n"):
        if line.strip():
            fields = line[1:].split()
            if line[0] == "-" or len(fields) < 2:
                raise DistError(f"{repo.name}: checkout していない submodule がある: {line.strip()}")
            paths.append(fields[1])
    return paths


def write_git_tar(repo: Path, commit: str, prefix: str, out_path: Path, paths: tuple = ()) -> None:
    # 本体と submodule の git archive を 1 つの tar.gz にまとめる。gzip の時刻は 0 にする (同じ入力から同じ
    # バイト列にするため)。paths があれば、そのパスだけを入れる。
    streams = [(repo, commit, prefix, paths)]
    if not paths:
        streams += [(repo / sub, "HEAD", f"{prefix}{sub}/", ()) for sub in submodule_paths(repo)]
    with open(out_path, "wb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as gz, \
            tarfile.open(fileobj=gz, mode="w", format=tarfile.PAX_FORMAT, pax_headers={"comment": commit}) as out:
        for cwd, rev, sub_prefix, selected in streams:
            args = ["git", "archive", "--format=tar", f"--prefix={sub_prefix}", rev]
            if selected:
                args += ["--", *selected]
            env = git_env()
            with tempfile.TemporaryFile() as errors:
                proc = subprocess.Popen(args, cwd=str(cwd), stdout=subprocess.PIPE, stderr=errors, env=env)
                count = 0
                with tarfile.open(fileobj=proc.stdout, mode="r|") as source:
                    for member in source:
                        out.addfile(member, source.extractfile(member) if member.isfile() else None)
                        count += 1
                proc.stdout.close()
                if proc.wait() != 0:
                    errors.seek(0)
                    raise DistError(f"git archive が {cwd.name} で失敗した: {errors.read().decode(errors='replace')[-400:]}")
                if count == 0:
                    raise DistError(f"{cwd.name} の {rev} {list(selected)} の git archive が空")


def archive_ext(uri: str) -> str:
    # 上流のアーカイブの拡張子 (ソースの zip の中のアーカイブの名前の {ext})。
    name = uri.split("?", 1)[0].rstrip("/").rsplit("/", 1)[-1].lower()
    for suffix in ARCHIVE_SUFFIXES:
        if name.endswith(suffix):
            return suffix[1:]
    raise DistError(f"知らない種類のアーカイブ: {uri}")


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fetch_archive(part: Part, dest: Path) -> None:
    # curl --fail --location でダウンロードし (obs-deps の Invoke-SafeWebRequest.ps1 と同じ取得のしかた)、SHA256 を
    # 照合する。上流のアーカイブは、バイト列を変えずに置く。
    completed = subprocess.run(
        ["curl", "--fail", "--location", "--silent", "--show-error", "--retry", "3", "--output", str(dest), part.uri],
        capture_output=True, text=True,
    )
    if completed.returncode != 0:
        raise DistError(f"{part.origin}: {part.uri} のダウンロードが失敗した: {completed.stderr.strip()[-400:]}")
    got = sha256_of(dest)
    if got != part.sha256:
        raise DistError(f"{part.origin}: {part.uri} の SHA256 が {part.sha256} のはずが {got}")


def archive_prefix(part: Part) -> str:
    # tar.gz の中の最上位のフォルダー <コンポーネント>-<バージョン>/。展開したときに、コンポーネントごとのフォルダーに
    # 分かれるようにする。
    version = VERSION_SAFE_RE.sub("-", part.version) if part.version else part.commit[:12]
    return f"{dist_common.source_part_slug(part.offer_name)}-{version}/"


def file_record(path: Path, part: Part, commit: str = "") -> dict:
    record = {"name": path.name, "part": part.offer_name, "origin": part.origin, "bytes": path.stat().st_size,
              "sha256": sha256_of(path)}
    if commit:
        record["commit"] = commit
    return record


class FetchInputs:
    # 取得するものの元 (実際の実行ではリポジトリのファイルから、セルフテストではテスト用のデータから作る)。

    def __init__(self, obs_deps_uri: str, obs_deps_tag: str, ws_deps_text: str, vencord_uri: str,
                 vencord_commit: str, vencord_tag: str):
        self.obs_deps_uri = obs_deps_uri
        self.obs_deps_tag = obs_deps_tag
        self.ws_deps_text = ws_deps_text
        self.vencord_uri = vencord_uri
        self.vencord_commit = vencord_commit
        self.vencord_tag = vencord_tag


def real_inputs() -> FetchInputs:
    obs_deps_uri, obs_deps_tag = obs_deps_facts()
    lock = dist_common.vencord_lock()
    return FetchInputs(obs_deps_uri, obs_deps_tag, WS_DEPS_CMAKE.read_text(encoding="utf-8"), VENCORD_URI,
                       lock["commit"], lock["tag"])


def sources_list_text(label: str, offer_name: str, offer_text: str, parts: list[Part], members: dict[str, str],
                      places: dict = WITHOUT_TARBALL_PLACES) -> str:
    # ソースをまとめた zip に入れる一覧 (dist_common.RELEASE_SOURCE_LIST) の中身。ソースコードの入手先.txt のコンポーネントの
    # 行を同じ順に全部並べ、行ごとに、そのソースのあるファイルを次の行に書く。offer_name は配布用の zip の中のソースコードの
    # 入手先.txt の名前 (top_level_files.txt の source-offer の行。日本語のファイル名はそこにだけ書く)。members は
    # コンポーネントの名前 (offer_name) -> zip の中のアーカイブの名前、places は専用のアーカイブを入れない行の場所
    # (WITHOUT_TARBALL_PLACES)。行の文字列はソースコードの入手先.txt のまま写す (make_release_notes.py が、配布用の zip の
    # ソースコードの入手先.txt の行が全部あることを確かめる)。
    zip_name = dist_common.RELEASE_ZIP_FORMAT.format(label=label)
    lines = [
        f"cordmux {label} に同梱したコンポーネントのソースコード",
        "",
        f"{zip_name} の中の「{offer_name}」のコンポーネントの行ごとに、そのソースコードのあるファイルを書いています。"
        "git のリポジトリから取得したものは、そのバージョンのコミットを git archive で tar.gz にしたものです (submodule も"
        "含みます)。ほかのファイルは、配布元が配っているアーカイブをそのまま置いています。",
        "",
        f"Source code of the components bundled with cordmux {label}",
        "",
        f"For each component line of \"{offer_name}\" in {zip_name}, the file with its source code is shown on "
        "the next line. Files from git repositories are tar.gz files made with git archive from the commit of that "
        "version (including submodules). The other files are the archives as distributed by each project, unchanged.",
        "",
    ]
    by_name = {part.offer_name: part for part in parts if part.offer_name}
    for name, line in offer_entries(offer_text):
        lines.append(line)
        if name in dist_common.SOURCE_PARTS_WITHOUT_TARBALL:
            if name not in places:
                raise DistError(f"WITHOUT_TARBALL_PLACES に {name!r} の場所がない")
            holder = places[name]
            if holder is None:
                lines.append("  GitHub のリリースの [Source code (zip)] と [Source code (tar.gz)] / "
                             "[Source code (zip)] and [Source code (tar.gz)] of the GitHub release")
            elif holder in members:
                lines.append(f"  {members[holder]} の中 / inside {members[holder]}")
            else:
                raise DistError(f"{name!r} の行のソースがある {holder!r} のアーカイブがない")
        elif name in members:
            sparse = by_name[name].sparse
            if sparse:
                paths = "、".join(sparse)
                lines.append(f"  {members[name]} ({paths} だけ / only {', '.join(sparse)})")
            else:
                lines.append(f"  {members[name]}")
        else:
            raise DistError(f"ソースコードの入手先.txt の行 {name!r} のアーカイブがない")
    return "\n".join(lines) + "\n"


def write_sources_zip(out: Path, files: dict[str, Path], list_text: str, epoch: int) -> None:
    # ソースをまとめた zip を書く。中は最上位のフォルダー (out の名前から .zip を除いたもの) の下に、files (zip の中の名前 ->
    # 取得したファイル) と一覧のテキスト。make_dist_zip.write_zip と同じく、名前の順に同じ時刻 (epoch。UTC) で書き、
    # 書き終えてから置き換える (途中で止まった zip を out に残さない)。アーカイブはどれも圧縮してあるので、圧縮し直さずに
    # (ZIP_STORED) 取得したバイト列のまま入れる。一覧のテキストも小さいので同じく圧縮しない。大きなアーカイブを
    # メモリーに読み込まないよう、ファイルから少しずつ書く。
    top = out.name[: -len(".zip")]
    members: dict[str, Optional[Path]] = {f"{top}/{name}": path for name, path in files.items()}
    members[f"{top}/{dist_common.RELEASE_SOURCE_LIST}"] = None
    date_time = dist_common.zip_date_time(epoch)
    partial = out.with_name(out.name + ".partial")
    try:
        with zipfile.ZipFile(partial, "w") as archive:
            for name in sorted(members):
                info = dist_common.fixed_zip_info(name, date_time, zipfile.ZIP_STORED)
                source = members[name]
                if source is None:
                    archive.writestr(info, list_text.encode("utf-8"))
                    continue
                info.file_size = source.stat().st_size
                with open(source, "rb") as reader, archive.open(info, "w") as writer:
                    shutil.copyfileobj(reader, writer, 1 << 20)
        os.replace(partial, out)
    finally:
        if partial.exists():
            partial.unlink()


def fetch_all(inputs: FetchInputs, offer_text: str, label: str, out_dir: Path, work_dir: Path, epoch: int,
              offer_name: str) -> dict:
    # obs-deps を取得してコンポーネントの一覧を作り、ソースコードの入手先.txt と突き合わせてから、全部を取得し、
    # out_dir の dist_common.RELEASE_SOURCES_ZIP_FORMAT の zip にまとめる。問題があれば DistError。obs-deps 自身は、
    # タグの git archive をそのまま tar.gz にする (FFmpeg、lame、theora などに適用したパッチと、ビルドの手順を含む)。
    # 取得したアーカイブは work_dir に置き、zip の中の名前は dist_common.RELEASE_SOURCE_MEMBER_FORMAT で、{part} は
    # ソースコードの入手先.txt のコンポーネントの名前 (dist_common.source_part_slug)。out_dir には zip だけを書き、
    # 同じ名前の zip があれば、何も取得せずに DistError (上書きしない)。
    zip_path = out_dir / dist_common.RELEASE_SOURCES_ZIP_FORMAT.format(label=label)
    if zip_path.exists():
        raise DistError(f"{out_dir} に {zip_path.name} が既にある")
    obs_deps = Part(OBS_DEPS_PART_NAME, "src-obs/buildspec.json prebuilt", "git", inputs.obs_deps_uri,
                    inputs.obs_deps_tag, want=f"refs/tags/{inputs.obs_deps_tag}")
    obs_deps_repo = work_dir / "obs-deps"
    obs_deps_commit = fetch_git(obs_deps, obs_deps_repo)
    script_parts, skipped = plan_from_scripts(obs_deps_repo / SCRIPTS_DIR)
    parts = [obs_deps, *script_parts, *plan_from_ws_deps(inputs.ws_deps_text),
             vencord_part(inputs.vencord_uri, inputs.vencord_commit, inputs.vencord_tag)]
    problems = match_parts(parts, offer_text)
    if problems:
        raise DistError("ソースコードの入手先.txt と取得するコンポーネントが合わない: " + "; ".join(problems))
    staging = work_dir / "sources"
    staging.mkdir()
    files = []
    staged: dict[str, Path] = {}
    members: dict[str, str] = {}
    for index, part in enumerate(parts):
        slug = dist_common.source_part_slug(part.offer_name)
        ext = "tar.gz" if part.kind == "git" else archive_ext(part.uri)
        member = dist_common.RELEASE_SOURCE_MEMBER_FORMAT.format(part=slug, ext=ext)
        path = staging / member
        if part is obs_deps:
            write_git_tar(obs_deps_repo, obs_deps_commit, archive_prefix(part), path)
            files.append(file_record(path, part, obs_deps_commit))
        elif part.kind == "git":
            repo = work_dir / f"part-{index:02d}"
            commit = fetch_git(part, repo)
            write_git_tar(repo, commit, archive_prefix(part), path, part.sparse)
            files.append(file_record(path, part, commit))
            remove_tree(repo)
        else:
            fetch_archive(part, path)
            files.append(file_record(path, part))
        staged[member] = path
        members[part.offer_name] = member
    list_text = sources_list_text(label, offer_name, offer_text, parts, members)
    out_dir.mkdir(parents=True, exist_ok=True)
    if zip_path.exists():
        raise DistError(f"{out_dir} に {zip_path.name} が既にある")
    write_sources_zip(zip_path, staged, list_text, epoch)
    return {"ok": True, "out": str(out_dir), "zip": zip_path.name, "bytes": zip_path.stat().st_size,
            "sha256": sha256_of(zip_path), "obs_deps_commit": obs_deps_commit, "files": files, "skipped": skipped}


def run(args: argparse.Namespace) -> dict:
    # --work を省くと一時フォルダーを作り、終わったら削除する。--work は空のフォルダーだけを受け入れる。
    offer_path = Path(args.source_offer) if args.source_offer else default_source_offer()
    offer_text = offer_path.read_text(encoding="utf-8")
    out_dir = Path(args.out)
    own_work = not args.work
    work_dir = Path(tempfile.mkdtemp(prefix="cordmux-fetch-sources-")) if own_work else Path(args.work)
    if not own_work:
        work_dir.mkdir(parents=True, exist_ok=True)
        leftovers = [p.name for p in work_dir.iterdir()]
        if leftovers:
            return {"ok": False, "out": str(out_dir), "error": f"--work の {work_dir} が空でない: {leftovers[:5]}"}
    try:
        # 一覧に書く名前は、--source-offer のファイルの名前ではなく、配布用の zip の中の名前にする。
        return fetch_all(real_inputs(), offer_text, args.label, out_dir, work_dir, int(args.source_date_epoch),
                         default_source_offer().name)
    except (DistError, OSError, tarfile.TarError, ElementTree.ParseError) as exc:
        return {"ok": False, "out": str(out_dir), "error": str(exc)}
    finally:
        if own_work:
            remove_tree(work_dir)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="同梱するソフトウェアのソースを取得し、コンポーネントごとのアーカイブを 1 つの zip にまとめる "
        "(ネットワークに接続するのは --fetch のときだけ)"
    )
    parser.add_argument("--selftest", action="store_true", help="このツールのセルフテストを実行する (ネットワークに接続しない)")
    parser.add_argument("--fetch", action="store_true", help="実際にソースをダウンロードする (ネットワークに接続する)")
    parser.add_argument("--label", help="タグから v を除いた文字列。例: 1.0.0、1.0.0-rc1")
    parser.add_argument("--out", help="ソースをまとめた zip を置くフォルダー (リリースに添付するファイルを置くフォルダー)")
    parser.add_argument("--source-date-epoch", help="zip の中の時刻 (cordmux のコミットの時刻。秒)")
    parser.add_argument("--source-offer", help="コンポーネントと突き合わせるソースコードの入手先.txt (既定: dist/ のもの)")
    parser.add_argument("--work", help="ダウンロードに使う空のフォルダー (既定: 一時フォルダーを作り、終わったら削除する)")
    return parser


def validate(args: argparse.Namespace) -> Optional[str]:
    # 引数を確認し、問題があれば理由の文を返す。ネットワークに接続するのは --fetch を付けたときだけにする
    # (セルフテストや打ち間違いで取得を始めないため)。--label はタグから v を除いた文字列だけを受け入れ、ビルド ID
    # (+ を含む) は受け入れない。リリースを作るのはタグのときだけで、+ は GitHub が添付するファイルの名前で
    # 置き換える文字であるため。
    if args.selftest:
        return None
    if not args.fetch:
        return "ネットワークに接続するには --fetch を付ける (付けなければ何もダウンロードしない)"
    missing = [f"--{name.replace('_', '-')}" for name in ("label", "out", "source_date_epoch") if not getattr(args, name)]
    if missing:
        return "引数が足りない: " + ", ".join(missing)
    if not label_ok(args.label):
        return f"--label にはタグから v を除いた文字列 (1.0.0 か 1.0.0-rc1) を指定する: {args.label!r}"
    if not args.source_date_epoch.isdigit():
        return f"--source-date-epoch には秒の整数を指定する: {args.source_date_epoch!r}"
    return None


# ---- セルフテスト (ネットワークに接続しない。テスト用のリポジトリと script とアーカイブで実行する) ---------------

SELFTEST_EPOCH = 1700000000  # zip の中の時刻 (2023-11-14T22:13:20Z)
SELFTEST_OFFER_NAME = "offer.txt"  # 一覧に書く、配布用の zip の中のソースコードの入手先.txt の名前

def clixml(digest: str, name: str, algorithm: str = "SHA256") -> str:
    # obs-deps の checksums/*.sha256 と同じ形のテストデータ。
    return (
        '<Objs Version="1.1.0.1" xmlns="http://schemas.microsoft.com/powershell/2004/04">\n'
        '  <Obj RefId="0">\n    <TN RefId="0">\n      <T>Microsoft.PowerShell.Commands.FileHashInfo</T>\n'
        "      <T>System.Object</T>\n    </TN>\n    <Props>\n"
        f'      <S N="Algorithm">{algorithm}</S>\n      <S N="Hash">{digest.upper()}</S>\n'
        f'      <S N="Path">{name}</S>\n    </Props>\n  </Obj>\n</Objs>\n'
    )


def script_text(name: str, version: str, uri: str, digest: str, targets: str, extra: str = "") -> str:
    # deps.ffmpeg/*.ps1 と同じ形の param ブロック (Patches の入れ子も入れて、括弧の数え方を確認する)。
    return (
        "param(\n"
        f"    [string] $Name = '{name}',\n"
        f"    [string] $Version = '{version}',\n"
        f"    [string] $Uri = '{uri}',\n"
        f'    [string] $Hash = "{digest}",\n'
        f"    [array] $Targets = @({targets}),\n"
        "    [array] $Patches = @(\n        @{\n"
        '            PatchFile = "${PSScriptRoot}/patches/x/0001-a (b).patch"\n'
        '            HashSum = "00"\n        }\n    )\n)\n\n'
        "function Setup {\n" + (extra or "    Setup-Dependency -Uri $Uri -Hash $Hash -DestinationPath $Path\n") + "}\n"
    )


def make_repo(root: Path, name: str, files: dict, submodule: Optional[Path] = None) -> tuple[Path, str]:
    # テスト用の上流の git リポジトリを作り、(パス, コミット) を返す。2 つ目の空のコミットを作り、取得するコミットを
    # 先端から外す (--depth 1 で先端だけを取得しても、目的のコミットにならない形にする)。
    repo = root / f"{name}.git"
    repo.mkdir(parents=True)
    git(["init", "-q"], repo)
    for rel, data in files.items():
        target = repo / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    if submodule is not None:
        git(["submodule", "add", "-q", submodule.as_uri(), "framework"], repo)
    git(["add", "-A"], repo)
    git(["-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "one"], repo)
    commit = git(["rev-parse", "HEAD"], repo).stdout.strip()
    git(["-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "--allow-empty", "-m", "two"], repo)
    # 一部だけを取得するテストのため、filter を受け付ける側にする (既定の file:// は filter を無視して全部を返す)。
    git(["config", "uploadpack.allowFilter", "true"], repo)
    return repo, commit


def tar_names(data: bytes) -> list[str]:
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
        return sorted(member.name for member in archive.getmembers() if member.isfile())


def selftest_world(root: Path) -> dict:
    # テスト用の obs-deps、コンポーネント、アーカイブ、ws_deps、Vencord、ソースコードの入手先.txt を作る。
    files = root / "files"
    files.mkdir()
    theora = files / "libtheora-1.1.1.zip"
    theora.write_bytes(b"theora archive bytes\n")
    lame = files / "lame-3.100.tar.gz"
    lame.write_bytes(b"lame archive bytes\n")
    asio_commit = "03ae834edbace31a96157b89bf50e5ee464e5ef9"
    asio = files / f"{asio_commit}.zip"
    asio.write_bytes(b"asio archive bytes\n")

    framework, _ = make_repo(root, "framework", {"f.txt": b"framework\n"})
    zlib, zlib_commit = make_repo(root, "zlib", {"zlib.h": b"zlib\n"})
    mbedtls, mbedtls_commit = make_repo(root, "mbedtls", {"m.txt": b"mbedtls\n"}, submodule=framework)
    amf, amf_commit = make_repo(root, "amf", {"amf/public/include/core.h": b"core\n", "Thirdparty/big.bin": b"big\n" * 1000})
    amf_big_blob = git(["rev-parse", f"{amf_commit}:Thirdparty/big.bin"], amf).stdout.strip()
    vencord, vencord_commit = make_repo(root, "vencord", {"pnpm-lock.yaml": b"lock\n", "package.json": b"{}\n"})
    lockless, lockless_commit = make_repo(root, "lockless", {"package.json": b"{}\n"})

    root_token = SCRIPT_ROOT_TOKEN
    deps = {
        f"{SCRIPTS_DIR}/10-zlib.ps1": script_text("zlib", "1.3.1", zlib.as_uri(), zlib_commit, "'x64', 'arm64'"),
        f"{SCRIPTS_DIR}/60-mbedtls.ps1": script_text("mbedtls", "3.6.4", mbedtls.as_uri(), mbedtls_commit, "'x64', 'arm64'"),
        f"{SCRIPTS_DIR}/80-amf.ps1": script_text(
            "amf", "1.4.36", amf.as_uri(), amf_commit, "'x64'",
            "    Invoke-GitCheckout $Uri $Hash -Sparse -SparseArgs set,amf/public/include\n"),
        f"{SCRIPTS_DIR}/50-libtheora.ps1": script_text(
            "libtheora", "1.1.1", theora.as_uri(), root_token + "checksums/libtheora-1.1.1.zip.sha256", "'x64', 'arm64'"),
        f"{SCRIPTS_DIR}/60-lame.ps1": script_text(
            "lame", "3.100", lame.as_uri(), root_token + "checksums/lame-3.100.tar.gz.win.sha256", "'x64', 'arm64'"),
        f"{SCRIPTS_DIR}/40-gas-preprocessor.ps1": script_text(
            "gas-preprocessor", "0.0.0", zlib.as_uri(), zlib_commit, "'arm64'"),
        f"{SCRIPTS_DIR}/20-libpng.ps1": script_text(
            "libpng", "1.6.47", theora.as_uri(), root_token + "checksums/libtheora-1.1.1.zip.sha256", "'x64', 'arm64'"),
        f"{SCRIPTS_DIR}/checksums/libtheora-1.1.1.zip.sha256": clixml(sha256_of(theora), theora.name),
        f"{SCRIPTS_DIR}/checksums/lame-3.100.tar.gz.win.sha256": clixml(sha256_of(lame), lame.name),
    }
    obs_deps, obs_deps_commit = make_repo(root, "obs-deps", {k: v.encode("utf-8") for k, v in deps.items()})
    tag = "2025-07-11"
    git(["-c", "user.name=t", "-c", "user.email=t@t", "tag", "-a", "-m", "tag", tag, obs_deps_commit], obs_deps)
    ws_text = (
        "FetchContent_Declare(\n  cordmux_asio\n"
        f"  URL {asio.as_uri()} # asio-1-32-0\n  URL_HASH SHA256={sha256_of(asio).upper()}\n)\n"
    )
    offer = "\n".join([
        "title line",
        "- cordmux same tag (Apache-2.0): https://example.invalid/cordmux",
        f"- Vencord v1.15.1 commit {vencord_commit} (GPL-3.0-or-later): https://example.invalid/v",
        f"- pnpm-lock.yaml commit {vencord_commit} (GPL-3.0-or-later): https://example.invalid/p",
        "- userplugin same tag (Apache-2.0): https://example.invalid/u",
        f"- obs-deps {tag} (build steps): https://example.invalid/obs-deps/tree/{tag}",
        f"- zlib 1.3.1 commit {zlib_commit} (Zlib): https://example.invalid/zlib",
        f"- mbedtls 3.6.4 commit {mbedtls_commit} (Apache-2.0): https://example.invalid/mbedtls",
        f"- AMF 1.4.36 commit {amf_commit} (MIT): https://example.invalid/amf",
        f"- theora 1.1.1 (BSD-3-Clause): {theora.as_uri()}",
        f"- mp3lame 3.100 (LGPL-2.0-or-later): {lame.as_uri()}",
        f"- asio 1.32.0 commit {asio_commit} (BSL-1.0): https://example.invalid/asio",
        "",
    ])
    return {
        "inputs": FetchInputs(obs_deps.as_uri(), tag, ws_text, vencord.as_uri(), vencord_commit, "v1.15.1"),
        "offer": offer,
        "obs_deps_commit": obs_deps_commit,
        "zlib_commit": zlib_commit,
        "theora": theora,
        "lame": lame,
        "lockless": (lockless, lockless_commit),
        "amf_big_blob": amf_big_blob,
    }


def selftest_fetch(failures: list[str]) -> None:
    # テスト用のデータで全部を取得して zip にまとめ、zip の中の名前と中身と一覧を確認し、同じ入力から同じ zip になることと、
    # 壊したデータで失敗することを確認する。
    GIT_CONFIG_PAIRS.append(SELFTEST_GIT_CONFIG)
    root = Path(tempfile.mkdtemp(prefix="cordmux-fetch-selftest-"))
    try:
        world = selftest_world(root)
        label = "0.1.0-rc1"
        out = root / "assets"
        result = fetch_all(world["inputs"], world["offer"], label, out, root / "work", SELFTEST_EPOCH, SELFTEST_OFFER_NAME)
        zip_name = dist_common.RELEASE_SOURCES_ZIP_FORMAT.format(label=label)
        top = f"cordmux-{label}-sources/"
        got_files = sorted(path.name for path in out.iterdir())
        if got_files != [zip_name]:
            failures.append(f"取得: 出力先のファイルが [{zip_name}] のはずが {got_files}")
        archives = {"obs-deps.tar.gz", "zlib.tar.gz", "mbedtls.tar.gz", "amf.tar.gz", "theora.zip", "mp3lame.tar.gz",
                    "asio.zip", "vencord.tar.gz"}
        want = {top + name for name in archives} | {top + dist_common.RELEASE_SOURCE_LIST}
        with zipfile.ZipFile(out / zip_name) as archive:
            infos = archive.infolist()
            data = {info.filename: archive.read(info) for info in infos}
        names = [info.filename for info in infos]
        if set(names) != want or len(names) != len(want):
            failures.append(f"取得: zip の中が {sorted(want)} のはずが {names}")
        if names != sorted(names):
            failures.append("取得: zip の中が名前の順に並んでいない")
        if any(info.compress_type != zipfile.ZIP_STORED for info in infos):
            failures.append("取得: zip の中に圧縮し直した項目がある")
        if any(info.date_time != dist_common.zip_date_time(SELFTEST_EPOCH) for info in infos):
            failures.append("取得: zip の中に時刻の違う項目がある")
        if result.get("zip") != zip_name or result.get("sha256") != sha256_of(out / zip_name):
            failures.append(f"取得: 結果の zip と sha256 が出力のファイルと合わない: {result.get('zip')}")
        if result.get("obs_deps_commit") != world["obs_deps_commit"]:
            failures.append(f"取得: obs-deps のコミットが違う: {result.get('obs_deps_commit')}")
        skipped = {entry["script"] for entry in result.get("skipped", [])}
        if skipped != {f"{SCRIPTS_DIR}/40-gas-preprocessor.ps1", f"{SCRIPTS_DIR}/20-libpng.ps1"}:
            failures.append(f"取得: 外した script が違う: {sorted(skipped)}")
        records = {record["name"]: record for record in result.get("files", [])}
        if set(records) != archives or any(
                r["sha256"] != hashlib.sha256(data.get(top + n, b"")).hexdigest() for n, r in records.items()):
            failures.append("取得: 結果の files が zip の中のファイルと合わない")
        checks = [
            ("mbedtls.tar.gz", "mbedtls-3.6.4/framework/f.txt", True),
            ("mbedtls.tar.gz", "mbedtls-3.6.4/m.txt", True),
            ("amf.tar.gz", "amf-1.4.36/amf/public/include/core.h", True),
            ("amf.tar.gz", "amf-1.4.36/Thirdparty/big.bin", False),
            ("vencord.tar.gz", "vencord-v1.15.1/pnpm-lock.yaml", True),
            ("obs-deps.tar.gz", f"obs-deps-2025-07-11/{SCRIPTS_DIR}/10-zlib.ps1", True),
        ]
        for name, member, present in checks:
            if top + name in data and (member in tar_names(data[top + name])) != present:
                failures.append(f"取得: {name} に {member} が{'ない' if present else 'ある'}")
        for name, source in (("theora.zip", world["theora"]), ("mp3lame.tar.gz", world["lame"])):
            if top + name in data and data[top + name] != source.read_bytes():
                failures.append(f"取得: {name} が上流のバイト列と違う")
        # 一覧: ソースコードの入手先.txt のコンポーネントの行が全部、同じ順に同じ文字列であり、次の行にソースのある場所がある
        listing = data.get(top + dist_common.RELEASE_SOURCE_LIST, b"").decode("utf-8")
        lines = listing.split("\n")
        entries = offer_entries(world["offer"])
        positions = [lines.index(line) if line in lines else -1 for _, line in entries]
        if -1 in positions or positions != sorted(positions):
            failures.append("一覧: ソースコードの入手先.txt のコンポーネントの行が全部、同じ順にない")
        else:
            following = {name: lines[pos + 1] for (name, _), pos in zip(entries, positions)}
            for name, start in (("zlib", "  zlib.tar.gz"), ("mp3lame", "  mp3lame.tar.gz"),
                                ("AMF", "  amf.tar.gz (amf/public/include だけ / only amf/public/include)"),
                                ("pnpm-lock.yaml", "  vencord.tar.gz の中 / inside vencord.tar.gz"),
                                ("cordmux", "  GitHub のリリースの [Source code (zip)]"),
                                ("userplugin", "  GitHub のリリースの [Source code (zip)]")):
                if not following.get(name, "").startswith(start):
                    failures.append(f"一覧: {name} の行の次が {start!r} で始まらない: {following.get(name)!r}")
        if "\r" in listing or not listing.endswith("\n"):
            failures.append("一覧: 改行が LF でないか、末尾に改行がない")
        if f"「{SELFTEST_OFFER_NAME}」" not in listing or f"\"{SELFTEST_OFFER_NAME}\"" not in listing:
            failures.append(f"一覧: 日本語と英語の説明に、渡したソースコードの入手先.txt の名前 {SELFTEST_OFFER_NAME} がない")

        # 同じ入力からもう一度作ると、同じバイト列の zip になる
        fetch_all(world["inputs"], world["offer"], label, root / "assets-again", root / "work-again", SELFTEST_EPOCH,
                  SELFTEST_OFFER_NAME)
        if (root / "assets-again" / zip_name).read_bytes() != (out / zip_name).read_bytes():
            failures.append("同じ入力から 2 回作った zip のバイト列が違う")
        # 一部だけを取得した AMF は、checkout しないパスの中身 (大きな blob) をダウンロードしていない (filter が動作した)。
        # rev-list の --missing=print は、ないオブジェクトを取得せずに ? を付けて出力する。
        amf_parts = [p for p in plan_from_scripts(root / "work" / "obs-deps" / SCRIPTS_DIR)[0] if p.name == "amf"]
        if not amf_parts or amf_parts[0].sparse != ("amf/public/include",):
            failures.append("一覧: amf が一部だけを取得するコンポーネントになっていない")
        else:
            amf_repo = root / "w-amf"
            amf_commit = fetch_git(amf_parts[0], amf_repo)
            listing = git(["rev-list", "--objects", "--missing=print", amf_commit], amf_repo).stdout
            if f"?{world['amf_big_blob']}" not in listing.split():
                failures.append("一部だけの取得: checkout しないパスの blob をダウンロードした")

        # 同じ出力先へもう一度取得する: すでにある zip を上書きせず、取得も始めない
        try:
            fetch_all(world["inputs"], world["offer"], label, out, root / "work2", SELFTEST_EPOCH, SELFTEST_OFFER_NAME)
            failures.append("zip がすでにあるフォルダーへの取得: エラーになるはずが通った")
        except DistError:
            pass
        if (root / "work2").exists():
            failures.append("zip がすでにあるフォルダーへの取得: 取得を始めた")

        def expect_fetch_error(what: str, func) -> None:
            try:
                func()
            except DistError:
                return
            failures.append(f"{what}: エラーになるはずが通った")

        # 照合の値と違うアーカイブはエラー
        theora_part = Part("libtheora", "t", "archive", world["theora"].as_uri(), "1.1.1", sha256="0" * 64)
        expect_fetch_error("SHA256 が違うアーカイブ", lambda: fetch_archive(theora_part, root / "bad.zip"))
        # pnpm-lock.yaml のない Vencord はエラー
        lockless, lockless_commit = world["lockless"]
        expect_fetch_error("pnpm-lock.yaml のない Vencord",
                           lambda: fetch_git(vencord_part(lockless.as_uri(), lockless_commit, "v0"), root / "w-lockless"))
        # 上流にないコミットはエラー
        missing = Part("zlib", "z", "git", world["inputs"].obs_deps_uri.replace("obs-deps.git", "zlib.git"),
                       "1.3.1", commit="0123456789abcdef0123456789abcdef01234567")
        expect_fetch_error("上流にないコミット", lambda: fetch_git(missing, root / "w-missing"))
        # ソースコードの入手先.txt に行のないコンポーネントが obs-deps に追加されたら、何も取得せずにエラー
        bad_offer = "\n".join(line for line in world["offer"].split("\n") if not line.startswith("- zlib "))
        expect_fetch_error("ソースコードの入手先.txt に zlib の行がない",
                           lambda: fetch_all(world["inputs"], bad_offer, label, root / "out3", root / "work3",
                                             SELFTEST_EPOCH, SELFTEST_OFFER_NAME))
        if (root / "out3").exists() and any((root / "out3").iterdir()):
            failures.append("ソースコードの入手先.txt と合わない: 突き合わせの前にファイルを書いた")
        # 一覧: 専用のアーカイブを入れない行の場所がない、行のアーカイブがない
        parts = [Part("zlib", "z", "git", "u", "1.3.1", commit="a" * 40)]
        parts[0].offer_name = "zlib"
        no_cordmux = {name: place for name, place in WITHOUT_TARBALL_PLACES.items() if name != "cordmux"}
        for what, offer, members, places in (
            ("場所を決めていない行", "- zlib 1.3.1\n- cordmux tag\n", {"zlib": "zlib.tar.gz"}, no_cordmux),
            ("行のアーカイブがない", "- zlib 1.3.1\n", {}, WITHOUT_TARBALL_PLACES),
            ("pnpm-lock.yaml の入った Vencord のアーカイブがない", "- zlib 1.3.1\n- pnpm-lock.yaml x\n",
             {"zlib": "zlib.tar.gz"}, WITHOUT_TARBALL_PLACES),
        ):
            expect_fetch_error(f"一覧 ({what})", lambda offer=offer, members=members, places=places:
                               sources_list_text(label, SELFTEST_OFFER_NAME, offer, parts, members, places))
    except (DistError, OSError, tarfile.TarError, zipfile.BadZipFile) as exc:
        failures.append(f"取得のテストデータ: {exc}")
    finally:
        remove_tree(root)
        while SELFTEST_GIT_CONFIG in GIT_CONFIG_PAIRS:
            GIT_CONFIG_PAIRS.remove(SELFTEST_GIT_CONFIG)


def selftest_match(failures: list[str]) -> None:
    # 突き合わせのセルフテスト。片方にしかないコンポーネント、古い行、あいまいな行がエラーになり、3 段階の探し方で
    # 対応が付くことを確認する。
    commit = "a" * 40
    parts = lambda: [  # noqa: E731
        Part("libogg", "s1", "git", "https://x/ogg.git", "1.3.5", commit=commit),
        Part("lame", "s2", "archive", "https://x/lame-3.100.tar.gz", "3.100", sha256="0" * 64),
        Part("json", "s3", "archive", f"https://x/{'b' * 40}.zip", commit="b" * 40, sha256="0" * 64),
    ]
    good = "\n".join([
        f"- ogg 1.3.5 commit {commit} (BSD): https://x/ogg",
        "- mp3lame 3.100 (LGPL): https://x/lame-3.100.tar.gz",
        f"- nlohmann/json 3.11.3 commit {'b' * 40} (MIT): https://x/json",
        "- cordmux tag: https://x/c",
        "- userplugin tag: https://x/u",
        "- pnpm-lock.yaml tag: https://x/p",
    ])
    listed = parts()
    problems = match_parts(listed, good)
    if problems or [p.offer_name for p in listed] != ["ogg", "mp3lame", "nlohmann/json"]:
        failures.append(f"正しい突き合わせ: {problems} {[p.offer_name for p in listed]}")
    cases = {
        "行がない": good.replace("- ogg 1.3.5", "- other 1.3.5").replace(commit, "c" * 40),
        "余分な行がある": good + "\n- foo 1.0 (MIT): https://x/foo",
        "行のコミットが古い": good.replace(f"commit {commit}", "commit " + "c" * 40),
        # 上流のバージョンが上がると Uri も変わるので、行のバージョンと Uri の両方が古い形
        "行のバージョンが古い": good.replace("3.100", "3.99"),
        "同じコンポーネントの行が 2 つある": good + f"\n- ogg 1.3.5 commit {commit} (BSD): https://x/ogg2",
        "zip の中のアーカイブの名前が重なる": good + "\n- OGG 1 (BSD): x",
    }
    for label, text in cases.items():
        if not match_parts(parts(), text):
            failures.append(f"突き合わせ ({label}): 問題になるはずが通った")
    # コンポーネントが 1 つ追加された (行がない) ときも問題にする
    if not match_parts(parts() + [Part("newlib", "s4", "git", "https://x/new.git", "1", commit="d" * 40)], good):
        failures.append("コンポーネントが追加された突き合わせ: 問題になるはずが通った")


def selftest_parse(failures: list[str]) -> None:
    # param ブロック、Clixml、cmake の読み方と、読めない形がエラーになることを確認する。
    info = parse_script(script_text("FFmpeg", "7.1.1", "https://x/FFmpeg.git", "d" * 40, "'x64', 'arm64'"))
    if (info["name"], info["version"], script_targets(info)) != ("FFmpeg", "7.1.1", ["x64", "arm64"]):
        failures.append(f"parse_script: {info}")
    for label, text in (("Targets がない", "param(\n [string] $Name = 'a',\n [string] $Version = '1',\n"
                                        " [string] $Uri = 'u.git',\n [string] $Hash = 'h'\n)\n"),
                        ("閉じていない", "param(\n [string] $Name = 'a'\n"),
                        ("param がない", "function Setup {}\n")):
        try:
            parse_script(text)
            failures.append(f"parse_script ({label}): エラーになるはずが通った")
        except DistError:
            pass
    with tempfile.TemporaryDirectory(prefix="cordmux-fetch-parse-") as tmp:
        good = Path(tmp) / "a.sha256"
        good.write_text(clixml("ab" * 32, "a.zip"), encoding="utf-8")
        if read_checksum(good) != "ab" * 32:
            failures.append("read_checksum: 値が違う")
        sha1 = Path(tmp) / "b.sha256"
        sha1.write_text(clixml("ab" * 32, "b.zip", algorithm="SHA1"), encoding="utf-8")
        try:
            read_checksum(sha1)
            failures.append("read_checksum (SHA1): エラーになるはずが通った")
        except DistError:
            pass
    for uri, ext in (("https://x/a-1.tar.gz", "tar.gz"), ("https://x/b.zip?x=1", "zip"), ("https://x/c.tar.xz", "tar.xz")):
        if archive_ext(uri) != ext:
            failures.append(f"archive_ext({uri})")
    try:
        archive_ext("https://x/d.rar")
        failures.append("archive_ext (知らない種類): エラーになるはずが通った")
    except DistError:
        pass


def selftest_real_files(failures: list[str]) -> None:
    # リポジトリの実際のファイル (ネットワークに接続せずに確認できる範囲): ws_deps.cmake の 3 つと Vencord が、
    # 現在のソースコードの入手先.txt の行と対応し、行のコミットが古くないこと。obs-deps のリポジトリと buildspec.json の読み方。
    try:
        offer = default_source_offer().read_text(encoding="utf-8")
        parts = plan_from_ws_deps(WS_DEPS_CMAKE.read_text(encoding="utf-8"))
        lock = dist_common.vencord_lock()
        parts.append(vencord_part(VENCORD_URI, lock["commit"], lock["tag"]))
        problems = match_parts(parts, offer, require_every_line=False)
        if problems or len(parts) != 4:
            failures.append(f"実際の ws_deps.cmake と Vencord をソースコードの入手先.txt と突き合わせた: {problems}")
        uri, tag = obs_deps_facts()
        if not uri.endswith("/obs-deps.git") or tag != dist_common.buildspec_facts()["obs_deps_date"]:
            failures.append(f"obs-deps の URL とタグ: {uri} {tag}")
        names = offer_part_names(offer)
        if not names:
            failures.append("ソースコードの入手先.txt: コンポーネントの行がない")
        folded = [dist_common.source_part_slug(name) for name in names]
        if len(set(folded)) != len(folded):
            failures.append(f"ソースコードの入手先.txt: zip の中のアーカイブの名前にすると重なるコンポーネントがある: {folded}")
        absent = [name for name in dist_common.SOURCE_PARTS_WITHOUT_TARBALL if name not in names]
        if absent:
            failures.append(f"ソースコードの入手先.txt: 専用のアーカイブを入れないコンポーネントの行がない: {absent}")
        if sorted(WITHOUT_TARBALL_PLACES) != sorted(dist_common.SOURCE_PARTS_WITHOUT_TARBALL):
            failures.append(f"WITHOUT_TARBALL_PLACES の名前が SOURCE_PARTS_WITHOUT_TARBALL と違う: {sorted(WITHOUT_TARBALL_PLACES)}")
        holders = [place for place in WITHOUT_TARBALL_PLACES.values() if place is not None and place not in names]
        if holders:
            failures.append(f"ソースコードの入手先.txt: 専用のアーカイブを入れない行のソースがある行がない: {holders}")
        if OBS_DEPS_PART_NAME not in names:
            failures.append("ソースコードの入手先.txt: obs-deps の行がない")
    except (DistError, OSError, KeyError) as exc:
        failures.append(f"実際のファイル: {exc}")


def selftest() -> int:
    failures: list[str] = []
    parser = build_parser()

    def expect(argv: list[str], want_error: bool, label: str) -> None:
        error = validate(parser.parse_args(argv))
        if (error is not None) != want_error:
            failures.append(f"{label}: {'受け入れない' if want_error else '受け入れる'}はずが {error!r}")

    epoch = ["--source-date-epoch", "1700000000"]
    expect(["--fetch", "--label", "0.1.0-rc1", "--out", "assets", *epoch], False, "正しい引数")
    expect(["--fetch", "--label", "0.1.0", "--out", "assets", *epoch, "--work", "w", "--source-offer", "s.txt"], False,
           "全部の引数")
    expect(["--label", "0.1.0", "--out", "assets", *epoch], True, "--fetch がない")
    expect(["--fetch", "--out", "assets", *epoch], True, "--label がない")
    expect(["--fetch", "--label", "0.1.0", *epoch], True, "--out がない")
    expect(["--fetch", "--label", "0.1.0", "--out", "assets"], True, "--source-date-epoch がない")
    expect(["--fetch", "--label", "0.1.0", "--out", "assets", "--source-date-epoch", "1.5"], True, "整数でない時刻")
    expect(["--fetch", "--label", "0.1.0+abc1234", "--out", "assets", *epoch], True, "label にビルド ID")
    expect(["--fetch", "--label", "v0.1.0", "--out", "assets", *epoch], True, "label の先頭に v")

    # zip の中のアーカイブの名前の規則 (README.md の例と同じ)
    slugs = {"FFmpeg": "ffmpeg", "nlohmann/json": "nlohmann-json", "obs-deps": "obs-deps", "svt-av1": "svt-av1", "AMF": "amf"}
    for name, want in slugs.items():
        if dist_common.source_part_slug(name) != want:
            failures.append(f"{name!r} の zip の中のアーカイブの名前の {{part}} が {want!r} のはずが {dist_common.source_part_slug(name)!r}")

    selftest_parse(failures)
    selftest_match(failures)
    selftest_real_files(failures)
    selftest_fetch(failures)

    if failures:
        for failure in failures:
            print(f"selftest 失敗: {failure}", file=sys.stderr)
        return 1
    print("selftest OK: fetch_sources (引数、読み方、突き合わせ、実際のファイル、ローカルでの取得と zip へのまとめ。ネットワークに接続しない)")
    return 0


def main(argv: Optional[list[str]] = None) -> int:
    dist_common.use_utf8_console()
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    error = validate(args)
    if error:
        parser.error(error)
    result = run(args)
    dist_common.print_json(result)
    return 0 if result.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
