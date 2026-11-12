#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# 配布用の zip を作るツールとチェックするツール (このフォルダーの Python のファイル) が共有する関数と定数。
# 同梱する FFmpeg の一覧やバージョンのような事実はここに持たず、それを決めているファイルから読む
# (下の「事実を決めているファイルを読む」)。ここで値を決めるのは、差し込みの書式、
# リリースに添付するファイルの名前など、ツールの間の約束だけである。
# Python 3 の標準ライブラリだけを使う (test/lint/ と同じ)。

import json
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Callable, Optional

TOOL_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOL_DIR.parents[2]

BUILDSPEC_PATH = REPO_ROOT / "src-obs" / "buildspec.json"
VENCORD_LOCK_PATH = REPO_ROOT / "src-vencord" / "vencord.lock.json"
FFMPEG_RUNTIME_CMAKE = REPO_ROOT / "src-obs" / "cmake" / "cordmux_ffmpeg_runtime.cmake"
BUILD_MARK_HEADER = REPO_ROOT / "src-obs" / "install" / "build_mark.h"
TOP_LEVEL_FILES = TOOL_DIR / "top_level_files.txt"
DEFAULT_DIST_DIR = REPO_ROOT / "dist"
# 手順書の Markdown (guide/ja.md と guide/en.md) を置くフォルダー。make_dist_zip.py が変換して zip に入れる。
DEFAULT_GUIDE_DIR = REPO_ROOT / "guide"

sys.path.insert(0, str(REPO_ROOT / "test" / "tools" / "vencord"))
import check_dist  # noqa: E402  (Vencord の dist に必要なファイルと、先頭の行のチェックを 1 か所に保つ)

# ビルド ID の形 (<バージョン>+<短いコミット>)。src-vencord/tests/version.test.ts が確認する形と同じ。
BUILD_ID_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+\+[0-9A-Za-z]+$")
FULL_COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")
# Vencord のバージョン (src-vencord/vencord.lock.json の tag。上流の Vencord のタグで、v<数>.<数>.<数> の後ろに
# 接尾辞が付いてもよい)。build_info.TAG_RE と同じ形だが、build_info がこのモジュールを import するので、ここに別に置く。
VENCORD_TAG_RE = re.compile(r"^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$")

# Vencord の本体の JS の末尾で、ライセンスのコメントを書いたファイルを指す行 (esbuild の legalComments。
# 例: vencordDesktopMain.js.LEGAL.txt)。
LEGAL_REFERENCE_RE = re.compile(r"For license information please see (\S+?)\s*\*/")

ZIP_UTF8_FLAG = 0x800


class DistError(Exception):
    # 入力が足りないか形が違うために、zip を作ることも読むこともできないときのエラー。
    # 呼び出し元は 1 行の JSON にして、終了コード 1 で返す。

    pass


# ---- 事実を決めているファイルを読む ----------------------------------------------
#
# 事実ごとに、それを決めているファイルは 1 つだけで、ここにはそれを読む関数を置く。
#   - 同梱する FFmpeg の DLL と ffmpeg.exe: src-obs/cmake/cordmux_ffmpeg_runtime.cmake の 2 つのマーカーの間
#   - cordmux と OBS と obs-deps のバージョン: src-obs/buildspec.json
#   - Vencord の固定のコミットとタグ: src-vencord/vencord.lock.json
#   - cordmux-build.txt の名前とキー: src-obs/install/build_mark.h の定数
#   - Vencord の dist に必要なファイル: test/tools/vencord/check_dist.py
#   - zip の最上位に置く dist/ のファイル: test/tools/dist-zip/top_level_files.txt


def load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def buildspec_facts(buildspec_path: Path = BUILDSPEC_PATH) -> dict:
    # buildspec.json から、cordmux のバージョンと、OBS と obs-deps のバージョンを取り出す。
    spec = load_json(buildspec_path)
    deps = spec["dependencies"]
    return {
        "version": spec["version"],
        "obs_version": deps["obs-studio"]["version"],
        "obs_deps_date": deps["prebuilt"]["version"],
    }


def obs_deps_bin_dir(repo_root: Path = REPO_ROOT, buildspec_path: Path = BUILDSPEC_PATH) -> Path:
    # obs-deps の bin フォルダーのパス。src-obs/cmake/cordmux_ffmpeg_runtime.cmake と同じ規則で、
    # buildspec.json のバージョンから作る。
    facts = buildspec_facts(buildspec_path)
    return (
        repo_root
        / "src-obs"
        / ".deps"
        / f"obs-studio-{facts['obs_version']}"
        / ".deps"
        / f"obs-deps-{facts['obs_deps_date']}-x64"
        / "bin"
    )


def vencord_lock(lock_path: Path = VENCORD_LOCK_PATH) -> dict:
    lock = load_json(lock_path)
    return {"commit": lock["commit"], "tag": lock["tag"]}


RUNTIME_BEGIN = "# ffmpeg-runtime-begin"
RUNTIME_END = "# ffmpeg-runtime-end"
CMAKE_SET_RE = re.compile(r"set\(\s*([A-Z0-9_]+)\s+([^)]*)\)", re.S)


def parse_ffmpeg_runtime(text: str) -> dict:
    # cordmux_ffmpeg_runtime.cmake の 2 つのマーカーの間にある set(...) を読み、3 つの一覧を返す。
    #
    # マーカーは行の先頭にあるものだけを数える (src-obs/install/removal_table.h と同じ書き方)。どれかの一覧が
    # 空なら DistError にする。空の一覧を受け入れると、zip のチェックが何も確認せずに成功するため。
    lines = text.split("\n")
    starts = [i for i, line in enumerate(lines) if line.rstrip() == RUNTIME_BEGIN]
    ends = [i for i, line in enumerate(lines) if line.rstrip() == RUNTIME_END]
    if len(starts) != 1 or len(ends) != 1 or starts[0] >= ends[0]:
        raise DistError("FFmpeg の DLL の一覧: 2 つのマーカーが、行の先頭にちょうど 1 回ずつない")
    body_lines = []
    for line in lines[starts[0] + 1 : ends[0]]:
        cut = line.find("#")
        body_lines.append(line if cut < 0 else line[:cut])
    body = "\n".join(body_lines)
    found = {name: values.split() for name, values in CMAKE_SET_RE.findall(body)}
    wanted = {
        "linked": "CORDMUX_FFMPEG_RUNTIME_LINKED",
        "exe_only": "CORDMUX_FFMPEG_RUNTIME_EXE_ONLY",
        "exe": "CORDMUX_FFMPEG_RUNTIME_EXE",
    }
    result = {}
    for key, var in wanted.items():
        values = found.get(var)
        if not values:
            raise DistError(f"FFmpeg の DLL の一覧: {var} がないか空")
        result[key] = values
    return result


def ffmpeg_runtime(cmake_path: Path = FFMPEG_RUNTIME_CMAKE) -> dict:
    runtime = parse_ffmpeg_runtime(cmake_path.read_text(encoding="utf-8"))
    runtime["all"] = runtime["linked"] + runtime["exe_only"] + runtime["exe"]
    return runtime


BUILD_MARK_CONST_RE = {
    "file_name": re.compile(r'kBuildMarkFileName\s*=\s*"([^"]+)"'),
    "key_build_id": re.compile(r'kBuildMarkKeyBuildId\s*=\s*"([^"]+)"'),
    "key_vencord_commit": re.compile(r'kBuildMarkKeyVencordCommit\s*=\s*"([^"]+)"'),
    "key_vencord_tag": re.compile(r'kBuildMarkKeyVencordTag\s*=\s*"([^"]+)"'),
}


def build_mark_names(header_path: Path = BUILD_MARK_HEADER) -> dict:
    # build_mark.h から、cordmux-build.txt のファイル名と 3 つのキーの定数を文字列として読む。
    text = header_path.read_text(encoding="utf-8")
    names = {}
    for key, pattern in BUILD_MARK_CONST_RE.items():
        found = pattern.findall(text)
        if len(found) != 1:
            raise DistError(f"build_mark.h: {key} の定数がないか、2 つ以上ある")
        names[key] = found[0]
    return names


def build_mark_text(build_id: str, vencord_commit: str, vencord_tag: str, names: Optional[dict] = None) -> str:
    # cordmux-build.txt の中身を作る (build_mark.h が読む形: UTF-8、LF、キーの順は build_id、vencord_commit、
    # vencord_tag)。vencord_tag は、設定ウィンドウの [バージョン情報] ページの「同梱のVencord」に表示する文字列で、
    # src-vencord/vencord.lock.json の tag と同じ値にする。
    names = names or build_mark_names()
    return (
        f"{names['key_build_id']}: {build_id}\n"
        f"{names['key_vencord_commit']}: {vencord_commit}\n"
        f"{names['key_vencord_tag']}: {vencord_tag}\n"
    )


TOP_LEVEL_KINDS = ("required-file", "optional-file", "source-offer", "required-dir")
# 置く行の種類のうち、ファイルを置くもの (fill-values の行で指定できるもの)。
TOP_LEVEL_FILE_KINDS = ("required-file", "optional-file", "source-offer")
# 差し込みをするファイルを指定する行の種類。「fill-values <言語> <名前>」の 3 語の行 (言語はそのファイルの言語)。
FILL_VALUES_KIND = "fill-values"


def parse_top_level(text: str) -> tuple[list[tuple[str, str]], dict[str, str]]:
    # top_level_files.txt を読み、(置く行の (種類, 名前) の並び, 差し込みをするファイルの 名前 -> 言語) を返す。
    #
    # fill-values の行を置く行と別に返すのは、置く行だけを読むツールが (種類, 名前) の組のまま読めるように
    # するため。fill-values の名前がファイルを置く行にないとき、言語が FILL_LANGS にないとき、同じ名前の
    # fill-values が 2 行あるときは DistError。
    entries: list[tuple[str, str]] = []
    fills: dict[str, str] = {}
    for raw in text.split("\n"):
        line = raw.rstrip("\r")
        if not line.strip() or line.startswith("#"):
            continue
        words = line.split(" ")
        if words[0] == FILL_VALUES_KIND:
            if len(words) != 3 or not words[2]:
                raise DistError(f"top_level_files.txt: 読めない行 {line!r} ('{FILL_VALUES_KIND} <言語> <名前>' の形で書く)")
            lang, name = words[1], words[2]
            if lang not in FILL_LANGS:
                raise DistError(f"top_level_files.txt: {line!r} の言語 {lang!r} は使えない (使える言語: {', '.join(FILL_LANGS)})")
            if name in fills:
                raise DistError(f"top_level_files.txt: {name} の {FILL_VALUES_KIND} が 2 行ある")
            fills[name] = lang
            continue
        kind, _, name = line.partition(" ")
        if kind not in TOP_LEVEL_KINDS or not name or " " in name.strip():
            raise DistError(f"top_level_files.txt: 読めない行 {line!r}")
        entries.append((kind, name.strip()))
    if not entries:
        raise DistError("top_level_files.txt: 置く行が 1 つもない")
    file_names = {name for kind, name in entries if kind in TOP_LEVEL_FILE_KINDS}
    for name in fills:
        if name not in file_names:
            raise DistError(f"top_level_files.txt: {FILL_VALUES_KIND} の {name} が、ファイルを置く行にない")
    return entries, fills


def top_level_entries(path: Path = TOP_LEVEL_FILES) -> list[tuple[str, str]]:
    # top_level_files.txt の置く行を (種類, 名前) の並びで返す (fill-values の行は含めない)。
    return parse_top_level(path.read_text(encoding="utf-8"))[0]


def fill_targets(path: Path = TOP_LEVEL_FILES) -> dict[str, str]:
    # top_level_files.txt の fill-values の行を 名前 -> 言語 で返す。
    return parse_top_level(path.read_text(encoding="utf-8"))[1]


# ---- 「キー: 値」の形のファイル --------------------------------------------------

KEY_VALUE_RE = re.compile(r"^([a-z0-9_]+): (.*)$")


def parse_key_values(text: str, *, allow_blank_lines: bool) -> tuple[list[tuple[str, str]], list[str]]:
    # 1 行に 1 つの「キー: 値」を読み、(読めた組の並び, 問題の文の並び) を返す。allow_blank_lines が偽なら、
    # 空行も問題にする。
    pairs: list[tuple[str, str]] = []
    problems: list[str] = []
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines = lines[:-1]
    for number, line in enumerate(lines, start=1):
        if "\r" in line:
            problems.append(f"{number} 行目: CR がある")
            line = line.rstrip("\r")
        if not line.strip():
            if not allow_blank_lines:
                problems.append(f"{number} 行目: 空行がある")
            continue
        match = KEY_VALUE_RE.match(line)
        if not match:
            problems.append(f"{number} 行目: 「キー: 値」の形でない")
            continue
        key, value = match.group(1), match.group(2).strip()
        if not value:
            problems.append(f"{number} 行目: {key} の値が空")
        pairs.append((key, value))
    seen = set()
    for key, _ in pairs:
        if key in seen:
            problems.append(f"キー {key} が 2 回ある")
        seen.add(key)
    return pairs, problems


def write_text_lf(path: Path, text: str) -> None:
    # UTF-8 と LF で書く。Windows でテキストとして書くと改行が CRLF になるので、バイト列で書く。
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(text.encode("utf-8"))


def legal_names_referenced(js_bytes: bytes) -> list[str]:
    # Vencord の本体の JS が末尾のコメントで指す .LEGAL.txt のファイル名を、重複を除いて返す。
    text = js_bytes.decode("utf-8", errors="replace")
    names = []
    for name in LEGAL_REFERENCE_RE.findall(text):
        if name not in names:
            names.append(name)
    return names


def vencord_selection(read: Callable[[str], Optional[bytes]], mark_file_name: str) -> list[str]:
    # data/vencord-src と data/vencord に置くファイル名の一覧を返す。check_dist.REQUIRED_FILES の 5 つ、本体の JS が
    # 指す .LEGAL.txt、mark_file_name (cordmux-build.txt) の順。read は名前から中身 (なければ None) を返す関数で、
    # check_dist.problems_for に渡すものと同じ形。
    names = list(check_dist.REQUIRED_FILES)
    for js_name in check_dist.HEADER_FILES:
        data = read(js_name)
        if data is None:
            continue
        for legal in legal_names_referenced(data):
            if legal not in names:
                names.append(legal)
    names.append(mark_file_name)
    return names


# ---- PE の import と ffmpeg.exe の起動 ------------------------------------------------


class PeError(Exception):
    # PE (exe と DLL の形式) として読めないときのエラー。

    pass


def pe_import_names(data: bytes) -> list[str]:
    # PE の import と delay import の表にある DLL の名前を返す。
    try:
        if data[:2] != b"MZ":
            raise PeError("MZ のヘッダーがない")
        pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe_offset : pe_offset + 4] != b"PE\0\0":
            raise PeError("PE のシグネチャがない")
        coff = pe_offset + 4
        section_count = struct.unpack_from("<H", data, coff + 2)[0]
        optional_size = struct.unpack_from("<H", data, coff + 16)[0]
        optional = coff + 20
        magic = struct.unpack_from("<H", data, optional)[0]
        if magic == 0x20B:
            rva_count_offset, directories = optional + 108, optional + 112
        elif magic == 0x10B:
            rva_count_offset, directories = optional + 92, optional + 96
        else:
            raise PeError("オプションヘッダーの magic が知らない値")
        rva_count = struct.unpack_from("<I", data, rva_count_offset)[0]
        section_table = optional + optional_size
        sections = [
            struct.unpack_from("<IIII", data, section_table + 40 * i + 8) for i in range(section_count)
        ]

        def to_offset(rva: int) -> int:
            for virtual_size, virtual_address, raw_size, raw_pointer in sections:
                if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
                    return rva - virtual_address + raw_pointer
            raise PeError(f"RVA 0x{rva:x} がどのセクションにも入らない")

        def c_string(offset: int) -> str:
            end = data.index(b"\0", offset)
            return data[offset:end].decode("ascii")

        names: list[str] = []
        if rva_count > 1:
            import_rva = struct.unpack_from("<I", data, directories + 8)[0]
            if import_rva:
                offset = to_offset(import_rva)
                while True:
                    fields = struct.unpack_from("<5I", data, offset)
                    if not any(fields):
                        break
                    names.append(c_string(to_offset(fields[3])))
                    offset += 20
        if rva_count > 13:
            delay_rva = struct.unpack_from("<I", data, directories + 8 * 13)[0]
            if delay_rva:
                offset = to_offset(delay_rva)
                while True:
                    fields = struct.unpack_from("<8I", data, offset)
                    if not any(fields):
                        break
                    names.append(c_string(to_offset(fields[1])))
                    offset += 32
        return names
    except (struct.error, ValueError, UnicodeDecodeError) as exc:
        raise PeError(str(exc)) from exc


def is_system_dll(name: str) -> bool:
    # OS の DLL か (API セットの名前か、System32 にある DLL)。
    low = name.lower()
    if low.startswith(("api-ms-win-", "ext-ms-")):
        return True
    system_root = os.environ.get("SystemRoot")
    if not system_root:
        return False
    return (Path(system_root) / "System32" / name).is_file()


def import_closure_problems(
    files: dict[str, bytes], root: str, is_system: Callable[[str], bool] = is_system_dll
) -> list[str]:
    # root から import をたどり、files にも OS にもない DLL を「import する側 -> DLL の名前」の文字列の並びで返す。
    #
    # files は同じフォルダーのファイル (名前 -> 中身)。起動する前にこれで確認しておけば、DLL が足りないときに
    # Windows の「DLL が見つからない」というエラーのダイアログを表示させずに済む。
    by_lower = {name.lower(): name for name in files}
    problems: list[str] = []
    seen: set[str] = set()
    queue = [root]
    while queue:
        current = queue.pop(0)
        key = current.lower()
        if key in seen:
            continue
        seen.add(key)
        try:
            imports = pe_import_names(files[by_lower[key]])
        except PeError as exc:
            problems.append(f"{current}: PE として読めない ({exc})")
            continue
        for name in imports:
            if name.lower() in by_lower:
                queue.append(by_lower[name.lower()])
            elif not is_system(name):
                problems.append(f"{current} -> {name}")
    return problems


# 子プロセスに引き継がれるエラーモード。重大なエラー、クラッシュ、ファイルを開けないときのダイアログを
# 表示させない。ダイアログを閉じるまで ffmpeg.exe が終わらず、閉じる人のいない CI のランナーでは止まったままになるため。
_SEM_FAILCRITICALERRORS = 0x0001
_SEM_NOGPFAULTERRORBOX = 0x0002
_SEM_NOOPENFILEERRORBOX = 0x8000


def _quiet_error_mode() -> None:
    if os.name != "nt":
        return
    import ctypes

    kernel32 = ctypes.WinDLL("kernel32")
    kernel32.SetErrorMode(_SEM_FAILCRITICALERRORS | _SEM_NOGPFAULTERRORBOX | _SEM_NOOPENFILEERRORBOX)


def run_ffmpeg_version(folder: Path, exe_name: str = "ffmpeg.exe") -> dict:
    # folder の exe_name を -version で起動し、結果を dict で返す (ran、ok と、分かったものだけ returncode、
    # version、configure、error)。
    #
    # PATH は System32 と Windows のフォルダーだけにする。PATH に別の ffmpeg があると、足りない DLL をそこから
    # 読み込んでしまい、DLL がそろっているかを確認できないため。起動する前に import をたどり、DLL が
    # 足りなければ起動しない。
    result: dict = {"ran": False, "ok": False}
    if os.name != "nt":
        result["error"] = "ffmpeg.exe を起動できるのは Windows だけ"
        return result
    files = {p.name: p.read_bytes() for p in folder.iterdir() if p.is_file()}
    if exe_name not in files:
        result["error"] = f"{exe_name} がない"
        return result
    closure = import_closure_problems(files, exe_name)
    if closure:
        result["error"] = "見つからない DLL がある: " + "; ".join(closure)
        return result
    system_root = os.environ.get("SystemRoot", "")
    env = {key: value for key, value in os.environ.items() if key.upper() in ("SYSTEMROOT", "WINDIR", "TEMP", "TMP")}
    env["PATH"] = os.pathsep.join([str(Path(system_root) / "System32"), system_root])
    _quiet_error_mode()
    try:
        completed = subprocess.run(
            [str(folder / exe_name), "-hide_banner", "-version"],
            cwd=str(folder),
            env=env,
            capture_output=True,
            timeout=60,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        result["error"] = f"起動できない: {exc}"
        return result
    result["ran"] = True
    result["returncode"] = completed.returncode
    stdout = completed.stdout.decode("utf-8", errors="replace").replace("\r\n", "\n")
    parsed = parse_ffmpeg_version_output(stdout)
    result.update(parsed)
    if completed.returncode != 0:
        result["error"] = f"終了コード {completed.returncode} (0x{completed.returncode & 0xFFFFFFFF:08x})"
    elif not parsed.get("version") or not parsed.get("configure"):
        result["error"] = "出力にバージョンか configuration の行がない"
    else:
        result["ok"] = True
    return result


FFMPEG_VERSION_RE = re.compile(r"^ffmpeg version (\S+)", re.M)
FFMPEG_CONFIGURE_RE = re.compile(r"^configuration: (.*)$", re.M)


def parse_ffmpeg_version_output(stdout: str) -> dict:
    # ffmpeg -version の出力から、バージョンと configuration の行を取り出す。見つからなければ空の文字列。
    version = FFMPEG_VERSION_RE.search(stdout)
    configure = FFMPEG_CONFIGURE_RE.search(stdout)
    return {
        "version": version.group(1) if version else "",
        "configure": configure.group(1).strip() if configure else "",
    }


def run_ffmpeg_in_copy(files: dict[str, bytes], exe_name: str = "ffmpeg.exe") -> dict:
    # files (zip の cordmux フォルダーの直下のファイル) だけを一時フォルダーにコピーし、そこで ffmpeg.exe を起動する。
    # zip に入っているファイルだけで起動できるかを確認するため。結果の形は run_ffmpeg_version と同じ。
    with tempfile.TemporaryDirectory(prefix="cordmux-dist-bin-") as tmp:
        folder = Path(tmp)
        for name, data in files.items():
            (folder / name).write_bytes(data)
        return run_ffmpeg_version(folder, exe_name)


# ---- 出力 --------------------------------------------------------------------------


def use_utf8_console() -> None:
    # 標準出力と標準エラー出力を UTF-8 にする。Windows のコンソールの既定のコードページでは、日本語の
    # メッセージとファイル名が文字化けするため。
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8")


def print_json(payload: dict) -> None:
    print(json.dumps(payload, ensure_ascii=False))


# ---- ツールの間の約束 ------------------------------------------------------------
#
# 差し込みの書式、リポジトリの URL、リリースに添付するファイルの名前。make_dist_zip.py、
# check_dist_zip.py、make_release_notes.py、fetch_sources.py は、どれもここから値と関数を使う。同じ規則を
# ツールごとに書くと、片方だけを修正したときに、作った zip とチェックが食い違うため。

# top_level_files.txt の fill-values の行に書ける言語 (手順書の言語)。
FILL_LANGS = ("ja", "en")
# 差し込みの書式 {{キー}}。キーは小文字の英数字と _ だけで書く。使えるキーは FILL_KEYS。
FILL_TOKEN_RE = re.compile(r"\{\{([a-z0-9_]+)\}\}")
FILL_EXTRA_KEYS = ("source_url",)

# このリポジトリの URL と、source_url の 2 つの書式。zip の名前の label に + がなければ同じタグのリリースを、
# あれば cordmux_commit のツリーを指す。
REPO_URL = "https://github.com/eoforgu0/cordmux"
SOURCE_URL_TAG_FORMAT = REPO_URL + "/releases/tag/v{label}"
SOURCE_URL_COMMIT_FORMAT = REPO_URL + "/tree/{commit}"
# 手順書 (guide/*.md) を GitHub で読む URL。source_url と同じく、label に + がなければタグ、あればコミットのファイルを指す。
GUIDE_URL_TAG_FORMAT = REPO_URL + "/blob/v{label}/{path}"
GUIDE_URL_COMMIT_FORMAT = REPO_URL + "/blob/{commit}/{path}"

# リリースに添付するファイルの名前の書式。label はタグから v を除いた文字列。
RELEASE_ZIP_FORMAT = "cordmux-{label}.zip"
RELEASE_PDB_ZIP_FORMAT = "cordmux-{label}-pdb.zip"
RELEASE_PDB_MEMBER = "cordmux.pdb"
RELEASE_SOURCE_FORMAT = "cordmux-{label}-src-{part}.{ext}"
# ソースコードの入手先.txt のコンポーネントの行のうち、専用のソースのアーカイブを添付しないもの。cordmux と userplugin の
# ソースは、GitHub がタグに付ける Source code のアーカイブにある。pnpm-lock.yaml は Vencord のアーカイブに入っている。
SOURCE_PARTS_WITHOUT_TARBALL = ("cordmux", "userplugin", "pnpm-lock.yaml")
_PART_SLUG_RE = re.compile(r"[^a-z0-9._-]")


def source_part_slug(name: str) -> str:
    # ソースコードの入手先.txt のコンポーネントの行の最初の語 (コンポーネントの名前) を、添付するファイルの名前の {part} に
    # する。小文字にし、英数字と ._- のほかの文字を - にする。GitHub は、添付するファイルの名前にある特殊な文字を、
    # 別の文字に置き換えるため。
    return _PART_SLUG_RE.sub("-", name.lower())


# ---- 差し込み ----------------------------------------------------------------------

# 差し込んでよいキー (source_url だけ)。
FILL_KEYS = FILL_EXTRA_KEYS
# 差し込みの書式の始まり。FILL_TOKEN_RE に一致しない形 (括弧の中に空白や大文字がある、など) で残っていれば、
# make_dist_zip.py は zip を作らず、check_dist_zip.py はエラーにする。
FILL_OPEN = "{{"


def source_url_for(label: str, cordmux_commit: str) -> str:
    # このバージョンのソースの URL を返す。label (zip の名前から cordmux- と .zip を除いた文字列) に + があれば
    # コミットのツリー、なければ同じタグのリリース。+ があるのに cordmux_commit が 40 桁でなければ DistError。
    # cordmux_commit は zip を作るときの引数 (--cordmux-commit)。
    if "+" in label:
        if not FULL_COMMIT_RE.match(cordmux_commit or ""):
            raise DistError(f"source_url を作れない: cordmux_commit が 40 桁の小文字の 16 進数でない: {cordmux_commit!r}")
        return SOURCE_URL_COMMIT_FORMAT.format(commit=cordmux_commit)
    return SOURCE_URL_TAG_FORMAT.format(label=label)


def guide_url_for(label: str, cordmux_commit: str, path: str) -> str:
    # リポジトリの中のファイル path (guide/ja.md など) を、このバージョンの GitHub のページで指す URL を返す。決め方は
    # source_url_for と同じ (label に + があればコミット、なければタグ)。+ があるのに cordmux_commit が 40 桁でなければ DistError。
    if "+" in label:
        if not FULL_COMMIT_RE.match(cordmux_commit or ""):
            raise DistError(f"手順書の URL を作れない: cordmux_commit が 40 桁の小文字の 16 進数でない: {cordmux_commit!r}")
        return GUIDE_URL_COMMIT_FORMAT.format(commit=cordmux_commit, path=path)
    return GUIDE_URL_TAG_FORMAT.format(label=label, path=path)


def fill_text(text: str, values: dict, where: str = "text") -> str:
    # text の {{キー}} を values の値に置き換える。知らないキー、values にないキー、FILL_TOKEN_RE に一致しない {{ が
    # あれば DistError。where はエラーの文に付ける名前。
    keys = FILL_TOKEN_RE.findall(text)
    unknown = sorted({key for key in keys if key not in FILL_KEYS})
    if unknown:
        raise DistError(f"{where}: 知らない差し込みのキー {unknown} (使えるのは {', '.join(FILL_EXTRA_KEYS)})")
    absent = sorted({key for key in keys if key not in values})
    if absent:
        raise DistError(f"{where}: 値のない差し込みのキー {absent}")
    if FILL_OPEN in FILL_TOKEN_RE.sub("", text):
        raise DistError(f"{where}: 差し込みの書式でない '" + FILL_OPEN + "' がある (書式は " + FILL_OPEN + "key}}。key は [a-z0-9_] で書く)")
    return FILL_TOKEN_RE.sub(lambda match: values[match.group(1)], text)


# ---- 正式版のチェック ------------------------------------------------------------


def formal_release_problems(guide_texts: dict, placeholders) -> list[str]:
    # 正式版として公開してよいかを確認し、問題の文の並びを返す (空なら公開してよい)。正式版にするかは呼び出し元が
    # 決める (make_release_notes.py の is_prerelease)。guide_texts は 手順書の名前 -> 差し込んだ後の文字列、
    # placeholders は rules の placeholder (後で書く所に置く仮の文字列) の並び。どの手順書にも、どの placeholder も
    # 残っていないことを確認する。空白と改行を除いて比べる (zip の中の手順書は、変換で日本語とインラインのコードの
    # 間の空白が除かれることがあるため)。
    problems: list[str] = []
    marks = [mark for mark in placeholders if mark]
    if not guide_texts:
        problems.append("placeholder を探す手順書がない")
    if not marks:
        problems.append("探す placeholder がない")
    for name, text in guide_texts.items():
        for mark in marks:
            if squash_spaces(mark) in squash_spaces(text):
                problems.append(f"{name} に placeholder {mark} が残っている")
    return problems


def squash_spaces(text: str) -> str:
    # 空白 (全角も) と改行を除いた文字列
    return re.sub(r"[\s　]+", "", text)
