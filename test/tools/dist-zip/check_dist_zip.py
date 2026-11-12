#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# 配布用の zip の中身をチェックし、1 行の JSON (ok と、code と detail を持つ problems) と終了コード 0 か 1 を返す。
# ファイルの並び、data/vencord-src と data/vencord の一致、cordmux-build.txt の値、トークンの形の
# 文字列と CR、ソースコードの入手先.txt のコンポーネントの行、差し込みの残りとソースの URL、手順書の URL と「互換性」の節、
# zip のファイルだけで ffmpeg.exe を起動できることを見る。それぞれの内容は check_view の中のコメントにある。
# 手順書の名前と節は test/lint/install_guide.py の load_rules (zip_name と role) と sections_by_heading で探す (見出しの
# 文字列を持たない)。zip の中の手順書は guide_to_text.py が Markdown から作ったテキストで、見出しは「## 節」の形。
# 引数は README.md と --help にある。

import argparse
import hashlib
import re
import sys
import tempfile
import zipfile
from pathlib import Path
from typing import Callable, Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dist_common  # noqa: E402  (ツールが共有する関数と定数)
import make_dist_zip  # noqa: E402  (セルフテストのデータを同じ作り方で作る)
from dist_common import DistError  # noqa: E402

sys.path.insert(0, str(dist_common.REPO_ROOT / "test" / "lint"))
import install_guide  # noqa: E402  (手順書の節を rules の role で探す関数)

PLUGIN = "cordmux"
DLL_PREFIX = f"{PLUGIN}/"  # cordmux.dll と同梱の DLL と ffmpeg.exe は、cordmux フォルダーの直下に置く
LOCALE_PREFIX = f"{PLUGIN}/data/locale/"
LOCALE_REQUIRED = ("ja-JP.ini", "en-US.ini")
ORIGINAL_DIR = f"{PLUGIN}/data/vencord-src"
SHOWN_DIR = f"{PLUGIN}/data/vencord"
PLUGIN_DLL = "cordmux.dll"
RENDERER = "vencordDesktopRenderer.js"
BINARY_SUFFIXES = (".dll", ".exe")
FORBIDDEN_SUFFIXES = (".pdb", ".map")
SECRET_RE = re.compile(r"(?<![0-9A-Za-z])[0-9a-fA-F]{64}(?![0-9A-Za-z])")
CONFIGURE_LIB_RE = re.compile(r"--enable-lib([a-z0-9]+)")
GPL_WORD_RE = re.compile(r"(?<![A-Za-z])GPL")
FFMPEG_WORD_RE = re.compile(r"ffmpeg", re.I)
# ソースコードの入手先.txt にコンポーネントの行が必要なもののうち、FFmpeg の configure から導けないもの (英数字だけにした形)。
# mbedtls は configure の --enable-lib* に出ない (srt と librist の中に静的に入っている。dist/README.md の
# licenses/ の説明) ので、ここに置く。
FIXED_COMPONENTS = ["cordmux", "vencord", "pnpmlockyaml", "obsdeps", "ffmpeg", "websocketpp", "asio", "nlohmannjson",
                    "mbedtls"]


class ZipView:
    # zip の中身を 名前 -> 中身 で持つ (セルフテストで、zip を作り直さずに、1 ファイルを抜いたものなどを用意するため)。

    def __init__(self, entries: dict[str, bytes], flags: dict[str, int], problems: Optional[list] = None):
        self.entries = entries
        self.flags = flags
        self.problems = problems or []

    def without(self, name: str) -> "ZipView":
        entries = dict(self.entries)
        del entries[name]
        return ZipView(entries, self.flags, list(self.problems))

    def with_entry(self, name: str, data: bytes) -> "ZipView":
        entries = dict(self.entries)
        entries[name] = data
        flags = dict(self.flags)
        flags.setdefault(name, dist_common.ZIP_UTF8_FLAG if not name.isascii() else 0)
        return ZipView(entries, flags, list(self.problems))


def load_view(source) -> ZipView:
    entries: dict[str, bytes] = {}
    flags: dict[str, int] = {}
    problems = []
    with zipfile.ZipFile(source) as archive:
        for info in archive.infolist():
            name = info.filename
            if name.endswith("/"):
                continue
            if name.startswith("/") or "\\" in name or ".." in name.split("/"):
                problems.append({"code": "bad_name", "detail": name})
            entries[name] = archive.read(info)
            flags[name] = info.flag_bits
    return ZipView(entries, flags, problems)


def normalize_word(word: str) -> str:
    return re.sub(r"[^a-z0-9]", "", word.lower())


def source_offer_problems(text: str, configure: Optional[str]) -> list[dict]:
    # ソースコードの入手先.txt に、必要なコンポーネントの行と、FFmpeg を GPL と書いた行があるかを確認する (LGPL は GPL に数えない)。
    # 必要なコンポーネントは FIXED_COMPONENTS と、FFmpeg の configure の --enable-lib* と --enable-zlib。
    # コンポーネントの行は「- 名前 ...」の形の行で、最初の語を英数字だけにして比べる (ソースコードの入手先.txt の先頭の約束で、
    # コンポーネントの名前は空白を含まない)。configure の lib の名前は、前に lib を付けた名前でもよい (librist)。
    # configure が None のときは (ffmpeg.exe -version を実行できなかったとき)、FIXED_COMPONENTS だけを見る。
    problems = []
    tokens = set()
    for line in text.split("\n"):
        if line.startswith("- "):
            words = line[2:].split()
            if words:
                tokens.add(normalize_word(words[0]))
    required = list(FIXED_COMPONENTS)
    for lib in CONFIGURE_LIB_RE.findall(configure or ""):
        if lib not in required:
            required.append(lib)
    if "--enable-zlib" in (configure or "").split():
        required.append("zlib")
    if configure is not None and not CONFIGURE_LIB_RE.search(configure):
        problems.append({"code": "source_offer", "detail": "ffmpeg の configure に --enable-lib がない (必要なコンポーネントを導けない)"})
    for component in required:
        if component not in tokens and f"lib{component}" not in tokens:
            problems.append({"code": "source_offer", "detail": f"{component} のコンポーネントの行がない"})
    if not any(FFMPEG_WORD_RE.search(line) and GPL_WORD_RE.search(line) for line in text.split("\n")):
        problems.append({"code": "source_offer", "detail": "FFmpeg を GPL と書いた行がない"})
    return problems


def load_guide_rules() -> list:
    # 手順書の lint の rules の組を読む (test/lint/install_guide.py の load_rules)。読めなければ DistError。
    try:
        return install_guide.load_rules()
    except (ValueError, OSError) as exc:
        raise DistError(f"手順書の lint の rules: {exc}") from exc


def squash(text: str) -> str:
    # 空白と改行を除く (手で折り返した行や、値の前後の空白で比べ損ねないため。lint の copied と同じ比べ方)。
    return install_guide.WHITESPACE_RE.sub("", text)


def guides_in(rel: dict[str, bytes], rules: list, add) -> list[tuple]:
    # rules の組ごとに、zip の中の手順書 (zip_name の名前) を (組, 名前, 文字列) の並びで返す。zip にない手順書は missing にする。
    guides = []
    for block in rules:
        name = block.zip_name
        if name in rel:
            guides.append((block, name, rel[name].decode("utf-8", errors="replace")))
        else:
            add("missing", f"{name} (rules の target {block.target} から作る手順書)")
    if not guides:
        add("guide", "チェックする手順書が zip にない")
    return guides


def url_problems(label: str, build_id: str, name: str, text: str, tag_url: str, commit_format: str, what: str) -> list[str]:
    # text にある URL が、zip の名前の label と合っているかを確認する (dist_common.source_url_for と同じ規則)。label に + が
    # なければ tag_url、あれば commit_format ({commit} の所が cordmux のコミット) の形で、そのコミットはビルド ID の短い
    # コミットで始まる (ローカルの +dev のような 16 進数でない値は、始まりを見ない)。cordmux のコミットは zip の中に残らない
    # ので、この形でしか確認できない。what は問題の文に入れる URL の名前。
    if "+" not in label:
        return [] if tag_url in text else [f"{name}: {what} {tag_url} がない"]
    prefix, suffix = commit_format.split("{commit}")
    commits = re.findall(re.escape(prefix) + r"([0-9a-f]+)" + re.escape(suffix), text)
    short = build_id.split("+", 1)[1] if "+" in build_id else ""
    if not commits:
        return [f"{name}: {what} (コミットのもの) がない"]
    problems = []
    for commit in commits:
        if not dist_common.FULL_COMMIT_RE.match(commit):
            problems.append(f"{name}: {what} のコミットが 40 桁の小文字の 16 進数でない")
        elif re.fullmatch(r"[0-9a-f]+", short) and not commit.startswith(short):
            problems.append(f"{name}: {what} のコミットがビルド ID {build_id} の短いコミットで始まらない")
    return problems


def source_url_problems(label: str, build_id: str, name: str, text: str) -> list[str]:
    # このバージョンのソースの URL (同じタグのリリースか、cordmux のコミットのツリー)
    return url_problems(label, build_id, name, text, dist_common.SOURCE_URL_TAG_FORMAT.format(label=label),
                        dist_common.SOURCE_URL_COMMIT_FORMAT, "ソースの URL")


def guide_url_problems(label: str, build_id: str, name: str, text: str, path: str) -> list[str]:
    # 手順書の冒頭の、整形された手順書 (リポジトリの path) の URL (同じタグか、cordmux のコミットのファイル)
    return url_problems(label, build_id, name, text, dist_common.GUIDE_URL_TAG_FORMAT.format(label=label, path=path),
                        dist_common.GUIDE_URL_COMMIT_FORMAT.replace("{path}", path), "手順書の URL")


def guide_problems(guides: list[tuple], add) -> None:
    # 手順書の「互換性」の節が空でなく placeholder もないことを確認する。この節はリリースノートにそのまま載るので、
    # make_release_notes.py は -rc のタグでも placeholder をエラーにする。同じ規則で main の zip もチェックし、
    # タグを付ける前に気付けるようにする。「互換性」の節は placeholder を含むだけでエラーにする
    # (placeholder と本文が並んだ節も、まだ書き終えていない)。
    for block, name, text in guides:
        sections = install_guide.sections_by_heading(text)
        maintenance_heading = block.roles["maintenance"]
        body = sections.get(maintenance_heading)
        if body is None:
            add("guide", f"{name}: 節 {maintenance_heading} がない")
        elif not body.strip():
            add("guide", f"{name}: 節 {maintenance_heading} が空")
        elif dist_common.squash_spaces(block.placeholder) in dist_common.squash_spaces(body):
            # 空白と改行を除いて比べる (変換で、日本語とインラインのコードの間の空白が除かれることがあるため)
            add("guide", f"{name}: 節 {maintenance_heading} に placeholder が残っている")


def check_view(
    view: ZipView,
    *,
    exec_ffmpeg: bool = True,
    expect_build_id: Optional[str] = None,
    run_in_copy: Callable[[dict], dict] = dist_common.run_ffmpeg_in_copy,
    formal_release: bool = False,
    ffmpeg_configure: Optional[str] = None,
) -> dict:
    # view をチェックし、結果の dict (ok、entries、problems、top、build_id、guides、formal_release、ffmpeg) を返す。
    # exec_ffmpeg が偽なら ffmpeg.exe を起動しない (セルフテストのデータの exe は PE ではないため)。ソースコードの入手先.txt
    # に必要なコンポーネントは、起動した ffmpeg.exe -version の configure から導く。起動しないときは ffmpeg_configure
    # (セルフテストが渡す)。どちらもなければ、固定のコンポーネントだけを見る。
    problems: list[dict] = list(view.problems)

    def add(code: str, detail: str) -> None:
        problems.append({"code": code, "detail": detail})

    names = sorted(view.entries)
    result: dict = {"ok": False, "entries": len(names), "problems": problems}
    tops = {name.split("/", 1)[0] for name in names}
    if len(tops) != 1 or any("/" not in name for name in names):
        add("top_folder", f"最上位のフォルダーが 1 つでない: {sorted(tops)}")
        return result
    top = tops.pop()
    result["top"] = top
    if not top.startswith("cordmux-"):
        add("top_folder", f"最上位のフォルダーの名前が cordmux-<label> でない: {top}")
    rel = {name[len(top) + 1 :]: view.entries[name] for name in names}

    runtime = dist_common.ffmpeg_runtime()
    mark_names = dist_common.build_mark_names()
    top_entries = dist_common.top_level_entries()
    fills = dist_common.fill_targets()
    rules = load_guide_rules()
    guide_names = [block.zip_name for block in rules]
    claimed: set[str] = set(name for name in guide_names if name in rel)

    # 並び: top_level_files.txt の最上位のファイル、cordmux フォルダーの直下の cordmux.dll と FFmpeg の DLL と ffmpeg.exe
    # (一覧は src-obs/cmake/cordmux_ffmpeg_runtime.cmake)、locale の 2 つの ini。どれにも
    # 当てはまらないファイルは、後で unexpected にする。
    for kind, name in top_entries:
        if kind == "required-dir":
            inside = [r for r in rel if r.startswith(name + "/")]
            if not inside:
                add("missing", f"{name}/ (ファイルがない)")
            claimed.update(inside)
        elif name in rel:
            claimed.add(name)
        elif kind != "optional-file":
            add("missing", name)
    for name in [PLUGIN_DLL] + runtime["all"]:
        if DLL_PREFIX + name in rel:
            claimed.add(DLL_PREFIX + name)
        else:
            add("missing", DLL_PREFIX + name)
    for name in LOCALE_REQUIRED:
        if LOCALE_PREFIX + name not in rel:
            add("missing", LOCALE_PREFIX + name)
    claimed.update(r for r in rel if r.startswith(LOCALE_PREFIX) and r.endswith(".ini") and "/" not in r[len(LOCALE_PREFIX) :])

    # data/vencord-src と data/vencord のそれぞれに、Vencord の dist の 5 ファイル (test/tools/vencord/check_dist.py の
    # チェック。先頭の Standalone と Updater Disabled の行も)、本体の JS が指す .LEGAL.txt、cordmux-build.txt がある。
    for sub in (ORIGINAL_DIR, SHOWN_DIR):

        def read(name: str, sub: str = sub) -> Optional[bytes]:
            return rel.get(f"{sub}/{name}")

        for problem in dist_common.check_dist.problems_for(read):
            add("vencord_dist", f"{sub}: {problem}")
        for name in dist_common.vencord_selection(read, mark_names["file_name"]):
            if f"{sub}/{name}" in rel:
                claimed.add(f"{sub}/{name}")
            else:
                add("missing", f"{sub}/{name}")

    # data/vencord-src (コピー元) と data/vencord (Vesktop が読むフォルダー) のファイル名と中身が同じ。
    original = {r[len(ORIGINAL_DIR) + 1 :]: data for r, data in rel.items() if r.startswith(ORIGINAL_DIR + "/")}
    shown = {r[len(SHOWN_DIR) + 1 :]: data for r, data in rel.items() if r.startswith(SHOWN_DIR + "/")}
    if sorted(original) != sorted(shown):
        add("original_and_shown_differ", f"ファイル名が違う: {sorted(set(original) ^ set(shown))}")
    for name in sorted(set(original) & set(shown)):
        if hashlib.sha256(original[name]).digest() != hashlib.sha256(shown[name]).digest():
            add("original_and_shown_differ", f"中身が違う: {name}")

    # .pdb と .map は入れない。上のどれにも当てはまらないファイルは unexpected にする。
    for r in sorted(rel):
        if r.lower().endswith(FORBIDDEN_SUFFIXES):
            add("forbidden", r)
        elif r not in claimed:
            add("unexpected", r)

    # ASCII でない名前に UTF-8 のフラグがある。
    for name in names:
        if not name.isascii() and not view.flags.get(name, 0) & dist_common.ZIP_UTF8_FLAG:
            add("utf8_flag", name)

    # DLL と exe のほかに、トークンの形 (64 桁の 16 進数) の文字列と CR がない。licenses/ の CR は許す。上流の
    # ライセンスの本文は中身を変えずに置くので、上流で CRLF のファイルを作業ツリーにコピーすると、その作業ツリーでは
    # CRLF のままになる (git に入るときと CI の checkout では、.gitattributes で LF になる)。どちらで作った zip も
    # 通すため。cordmux/ と最上位のファイルはこのリポジトリで作るもので、Windows で書いて CRLF が混ざったことを
    # ここで見つける。
    for r, data in rel.items():
        if r.lower().endswith(BINARY_SUFFIXES):
            continue
        if b"\r" in data and not r.startswith("licenses/"):
            add("carriage_return", r)
        for line in data.decode("utf-8", errors="replace").split("\n"):
            if SECRET_RE.search(line):
                add("secret_shape", r)
                break

    # cordmux-build.txt は 3 つのキーがこの順にちょうどあり、値の形が正しく、vencord_commit と vencord_tag が
    # src-vencord/vencord.lock.json と一致し、data/vencord-src と data/vencord で同じ値である。
    marks = {}
    for sub in (ORIGINAL_DIR, SHOWN_DIR):
        data = rel.get(f"{sub}/{mark_names['file_name']}")
        if data is None:
            continue
        pairs, parse_problems = dist_common.parse_key_values(data.decode("utf-8", errors="replace"), allow_blank_lines=False)
        for problem in parse_problems:
            add("build_mark", f"{sub}: {problem}")
        keys = [key for key, _ in pairs]
        if keys != [mark_names["key_build_id"], mark_names["key_vencord_commit"], mark_names["key_vencord_tag"]]:
            add("build_mark", f"{sub}: キーの並びが違う {keys}")
        mark = dict(pairs)
        if not dist_common.BUILD_ID_RE.match(mark.get(mark_names["key_build_id"], "")):
            add("build_mark", f"{sub}: build_id の形が違う")
        if not dist_common.FULL_COMMIT_RE.match(mark.get(mark_names["key_vencord_commit"], "")):
            add("build_mark", f"{sub}: vencord_commit の形が違う")
        if not dist_common.VENCORD_TAG_RE.match(mark.get(mark_names["key_vencord_tag"], "")):
            add("build_mark", f"{sub}: vencord_tag の形が違う")
        marks[sub] = mark
    lock = dist_common.vencord_lock()
    for sub, mark in marks.items():
        for mark_key, lock_key in ((mark_names["key_vencord_commit"], "commit"), (mark_names["key_vencord_tag"], "tag")):
            if mark.get(mark_key) != lock[lock_key]:
                add("build_mark", f"{sub}: cordmux-build.txt の {mark_key} {mark.get(mark_key)!r} が vencord.lock.json の {lock[lock_key]!r} と違う")
    if len({tuple(sorted(mark.items())) for mark in marks.values()}) > 1:
        add("build_mark", "data/vencord-src と data/vencord の cordmux-build.txt の値が違う")
    result["build_id"] = next((mark.get(mark_names["key_build_id"], "") for mark in marks.values()), "")
    label = top[len("cordmux-") :] if top.startswith("cordmux-") else top

    # 最上位のファイルに {{ が残っていない (差し込みの漏れと、形の違う {{キー}} を見つける)。手順書には、zip 用の差し込みの
    # 印 (<!--) も残っていない。licenses/ と cordmux/ の下は見ない。上流のライセンスの本文と Vencord の本体の JS は、{{ を
    # 含むことがあるため。
    for kind, name in top_entries:
        if kind in dist_common.TOP_LEVEL_FILE_KINDS and name in rel and dist_common.FILL_OPEN.encode("ascii") in rel[name]:
            add("unfilled", name)
    for name in guide_names:
        if name in rel and (dist_common.FILL_OPEN.encode("ascii") in rel[name] or b"<!--" in rel[name]):
            add("unfilled", name)

    # fill-values の行で指定したファイルと手順書のソースの URL (source_url_problems)。手順書の冒頭の、整形された手順書の
    # URL (guide_url_problems)。
    for name in list(fills) + guide_names:
        if name in rel:
            for problem in source_url_problems(label, result["build_id"], name, rel[name].decode("utf-8", errors="replace")):
                add("source_url", problem)
    for block in rules:
        if block.zip_name in rel:
            text = rel[block.zip_name].decode("utf-8", errors="replace")
            for problem in guide_url_problems(label, result["build_id"], block.zip_name, text, block.target):
                add("guide_url", problem)

    # 手順書の「互換性」の節 (guide_problems)。--formal-release のときは、正式版として
    # 公開できるかも見る (dist_common.formal_release_problems)。正式版にするかは呼び出し元が決める。
    guides = guides_in(rel, rules, add)
    result["guides"] = [name for _, name, _ in guides]
    guide_problems(guides, add)
    if formal_release:
        texts = {name: text for _, name, text in guides}
        for problem in dist_common.formal_release_problems(texts, [block.placeholder for block in rules]):
            add("formal_release", problem)
    result["formal_release"] = formal_release

    # --expect-build-id: cordmux-build.txt、cordmux.dll、Vencord の本体の JS に、同じビルド ID がある。
    # 同じ文字列であることが、[バージョン情報] ページの「cordmux (OBS)」と「cordmux (Vesktop)」を比べる前提になる。
    # 引数を省けるのは、ローカルで古い Vencord の dist から作った zip は、ビルド ID が違って当然で、
    # 並びのチェックまで失敗させないため。CI は必ず渡す。
    if expect_build_id is not None:
        needle = expect_build_id.encode("utf-8")
        for sub, mark in marks.items():
            if mark.get(mark_names["key_build_id"]) != expect_build_id:
                add("build_id", f"{sub}: cordmux-build.txt の build_id が {expect_build_id!r} と違う")
        if needle not in rel.get(DLL_PREFIX + PLUGIN_DLL, b""):
            add("build_id", f"{PLUGIN_DLL} に {expect_build_id!r} がない")
        for sub in (ORIGINAL_DIR, SHOWN_DIR):
            if needle not in rel.get(f"{sub}/{RENDERER}", b""):
                add("build_id", f"{sub}/{RENDERER} に {expect_build_id!r} がない")

    # cordmux フォルダーの直下のファイルだけを置いた一時フォルダーで ffmpeg.exe -version を実行し、起動できることを確認する
    # (dist_common.run_ffmpeg_in_copy)。出力の configure は、ソースコードの入手先.txt のコンポーネントの確認に使う。
    if exec_ffmpeg:
        bin_files = {r[len(DLL_PREFIX) :]: data for r, data in rel.items() if r.startswith(DLL_PREFIX) and "/" not in r[len(DLL_PREFIX) :]}
        run = run_in_copy(bin_files)
        result["ffmpeg"] = {key: run.get(key) for key in ("ran", "ok", "version", "error") if key in run}
        if not run.get("ok"):
            add("ffmpeg_exec", str(run.get("error")))
        else:
            ffmpeg_configure = run.get("configure")

    # ソースコードの入手先.txt のコンポーネントの行と、FFmpeg を GPL と書いた行。
    for kind, name in top_entries:
        if kind == "source-offer" and name in rel:
            for problem in source_offer_problems(rel[name].decode("utf-8", errors="replace"), ffmpeg_configure):
                problems.append(problem)

    result["ok"] = not problems
    return result


def check_zip(path: Path, expect_build_id: Optional[str] = None, formal_release: bool = False) -> dict:
    try:
        view = load_view(path)
    except (OSError, zipfile.BadZipFile) as exc:
        return {"file": str(path), "ok": False, "problems": [{"code": "open", "detail": str(exc)}]}
    result = check_view(view, expect_build_id=expect_build_id, formal_release=formal_release)
    return {"file": str(path), **result}


# ---- セルフテスト ----------------------------------------------------------------


def _codes(result: dict) -> set[str]:
    return {problem["code"] for problem in result["problems"]}


def fill_and_guide_selftest(good: ZipView, top: str, run, failures: list[str]) -> None:
    # 差し込みの残りとソースの URL、手順書の「互換性」の節、正式版のチェックのセルフテスト。残った {{、
    # ソースの URL の食い違い、互換性の節の空と placeholder がエラーになること、
    # 正式版のチェックは --formal-release のときだけ動作することを確認する。
    rules = load_guide_rules()
    block = rules[0]
    name = block.zip_name
    guide = f"{top}/{name}"
    offer_name = [n for kind, n in dist_common.top_level_entries() if kind == "source-offer"][0]
    offer = f"{top}/{offer_name}"
    text = good.entries[guide].decode("utf-8")
    token = dist_common.FILL_OPEN + "source_url}}"
    maintenance = make_dist_zip.SAMPLE_MAINTENANCE
    commit_url = dist_common.SOURCE_URL_COMMIT_FORMAT.format(commit=make_dist_zip.SAMPLE_CORDMUX_COMMIT)

    def guide_with(new_text: str, view: ZipView = good) -> ZipView:
        return view.with_entry(guide, new_text.encode("utf-8"))

    def heading_renamed(heading: str) -> str:
        # 見出しを別の名前にする
        return text.replace(install_guide.render_heading(heading), install_guide.render_heading(heading + " x"))

    # 差し込みの残り
    run("手順書に差し込みの書式が残っている", guide_with(text + token + "\n"), "unfilled")
    run("ソースコードの入手先.txt に差し込みの書式が残っている", good.with_entry(offer, good.entries[offer] + token.encode("ascii")), "unfilled")
    run("形の違う差し込みの書式が残っている", guide_with(text + dist_common.FILL_OPEN + " source_url }}\n"), "unfilled")
    run("手順書に zip 用の差し込みの印が残っている", guide_with(text + install_guide.MARKER_LINE + "\n"), "unfilled")
    run("licenses/ の {{ は差し込みの残りに数えない", good.with_entry(f"{top}/licenses/cordmux/LICENSE", b"Apache {{x}}\n"), None)
    braces_in_bundle = good
    for sub in (ORIGINAL_DIR, SHOWN_DIR):
        renderer = f"{top}/{sub}/{RENDERER}"
        braces_in_bundle = braces_in_bundle.with_entry(renderer, good.entries[renderer] + b"var t=`{{x}}`;\n")
    run("Vencord の本体の JS の {{ は差し込みの残りに数えない", braces_in_bundle, None)

    # ソースの URL (label に + があるので、cordmux のコミットのツリー)
    run("ソースの URL がタグのリリースになっている", guide_with(text.replace(commit_url, dist_common.SOURCE_URL_TAG_FORMAT.format(label="0.1.0"))), "source_url")
    run("ソースの URL がない", guide_with(text.replace(commit_url, "")), "source_url")
    run("ソースの URL のコミットがビルド ID と合わない", guide_with(text.replace(commit_url, dist_common.SOURCE_URL_COMMIT_FORMAT.format(commit="f" * 40))), "source_url")
    run("ソースの URL のコミットが短い", guide_with(text.replace(commit_url, dist_common.SOURCE_URL_COMMIT_FORMAT.format(commit="abc1234"))), "source_url")
    guide_url = dist_common.guide_url_for(make_dist_zip.SAMPLE_BUILD_ID, make_dist_zip.SAMPLE_CORDMUX_COMMIT, block.target)
    if guide_url not in text:
        failures.append(f"テストデータの {name} に手順書の URL {guide_url} がない")
    run("手順書の URL がない", guide_with(text.replace(guide_url, "")), "guide_url")
    run("手順書の URL のコミットがビルド ID と合わない",
        guide_with(text.replace(guide_url, dist_common.guide_url_for("0.1.0+fff0000", "f" * 40, block.target))), "guide_url")
    run("手順書の URL がタグのもの", guide_with(text.replace(guide_url, dist_common.guide_url_for("0.1.0", "", block.target))), "guide_url")
    if guide_url_problems("0.1.0", "0.1.0+abc1234", "x", dist_common.guide_url_for("0.1.0", "", "guide/ja.md"), "guide/ja.md"):
        failures.append("guide_url_problems: label に + がないときのタグの URL を受け入れない")
    tag_url = dist_common.SOURCE_URL_TAG_FORMAT.format(label="0.1.0")
    if source_url_problems("0.1.0", "0.1.0+abc1234", "x", tag_url) or not source_url_problems("0.1.0", "0.1.0+abc1234", "x", commit_url):
        failures.append("source_url_problems: label に + がないときの判定が違う")
    if source_url_problems("0.1.0-rc1", "0.1.0+abc1234", "x", dist_common.SOURCE_URL_TAG_FORMAT.format(label="0.1.0-rc1")):
        failures.append("source_url_problems: -rc のタグのリリースの URL を受け入れない")

    # 手順書の「互換性」の節
    run("互換性の節がない", guide_with(heading_renamed(block.roles["maintenance"])), "guide")
    run("互換性の節が空", guide_with(text.replace(maintenance, "")), "guide")
    run("互換性の節が placeholder だけ", guide_with(text.replace(maintenance, block.placeholder)), "guide")
    run("互換性の節に placeholder がある", guide_with(text.replace(maintenance, maintenance + " " + block.placeholder)), "guide")
    half = len(block.placeholder) // 2
    run("互換性の節に、行をまたいだ placeholder がある",
        guide_with(text.replace(maintenance, maintenance + " " + block.placeholder[:half] + "\n" + block.placeholder[half:])), "guide")
    run("手順書がない", good.without(guide), "missing")
    codes: list[str] = []
    stray = install_guide.GuideRules()
    stray.target = "sample/not-shipped.txt"
    stray.zip_name = "not-shipped.txt"
    guides_in({}, [stray], lambda code, detail: codes.append(code))
    if codes != ["missing", "guide"]:
        failures.append(f"zip にない手順書: missing と guide (チェックした手順書がない) のはずが {codes}")

    # 正式版 (--formal-release のときだけ)
    run("正しいデータを正式版としてチェックする", good, None, formal=True)
    first = install_guide.render_heading(block.headings[0])
    with_placeholder = guide_with(text.replace(first, first + "\n" + block.placeholder, 1))
    run("手順書に placeholder があり、--formal-release なし", with_placeholder, None)
    run("手順書に placeholder があり、正式版", with_placeholder, "formal_release", formal=True)
    if dist_common.formal_release_problems({}, [block.placeholder]) == []:
        failures.append("formal_release_problems: 手順書が 1 つもないのに通った")


def selftest(material: Optional[Path]) -> int:
    failures: list[str] = []
    runtime = dist_common.ffmpeg_runtime()
    offer_name = [name for kind, name in dist_common.top_level_entries() if kind == "source-offer"][0]

    with tempfile.TemporaryDirectory(prefix="cordmux-check-dist-zip-") as tmp:
        root = Path(tmp)
        sample = make_dist_zip.write_sample_inputs(root / "in")
        out = root / f"cordmux-{make_dist_zip.SAMPLE_BUILD_ID}.zip"
        make_dist_zip.make(make_dist_zip.sample_args(sample, out))
        good = load_view(out)

    top = f"cordmux-{make_dist_zip.SAMPLE_BUILD_ID}"
    build_id = make_dist_zip.SAMPLE_BUILD_ID

    def run(label: str, view: ZipView, want: Optional[str], expect: Optional[str] = build_id, formal: bool = False) -> None:
        result = check_view(
            view, exec_ffmpeg=False, expect_build_id=expect, formal_release=formal,
            ffmpeg_configure=make_dist_zip.SAMPLE_CONFIGURE,
        )
        if want is None and not result["ok"]:
            failures.append(f"{label}: 成功のはずが失敗した: {result['problems']}")
        elif want is not None and (result["ok"] or want not in _codes(result)):
            failures.append(f"{label}: {want} のはずが {result['problems']}")

    run("正しいデータ", good, None)
    for name in [PLUGIN_DLL] + runtime["all"]:
        run(f"{name} がない", good.without(f"{top}/{DLL_PREFIX}{name}"), "missing")
    run("ja-JP.ini がない", good.without(f"{top}/{LOCALE_PREFIX}ja-JP.ini"), "missing")
    run("data に一覧にないファイル (verified.txt) が入っている", good.with_entry(f"{top}/{PLUGIN}/data/verified.txt", b"build_id: x\n"), "unexpected")
    run("ソースコードの入手先.txt がない", good.without(f"{top}/{offer_name}"), "missing")
    run("data/vencord の css がない", good.without(f"{top}/{SHOWN_DIR}/vencordDesktopRenderer.css"), "vencord_dist")
    run(".LEGAL.txt がない", good.without(f"{top}/{ORIGINAL_DIR}/vencordDesktopMain.js.LEGAL.txt"), "missing")
    mark = dist_common.build_mark_names()["file_name"]
    run("cordmux-build.txt がない", good.without(f"{top}/{SHOWN_DIR}/{mark}"), "missing")
    run(".pdb がある", good.with_entry(f"{top}/{DLL_PREFIX}cordmux.pdb", b"pdb"), "forbidden")
    run(".map がある", good.with_entry(f"{top}/{SHOWN_DIR}/vencordDesktopMain.js.map", b"{}"), "forbidden")
    run("余分な DLL がある", good.with_entry(f"{top}/{DLL_PREFIX}datachannel.dll", b"MZ"), "unexpected")
    run("patcher.js がある", good.with_entry(f"{top}/{ORIGINAL_DIR}/patcher.js", b"x"), "unexpected")
    run("最上位のフォルダーが 2 つある", good.with_entry("other/readme.txt", b"x"), "top_folder")
    shown_css = f"{top}/{SHOWN_DIR}/vencordDesktopRenderer.css"
    run("data/vencord の中身が data/vencord-src と違う", good.with_entry(shown_css, good.entries[shown_css] + b" "), "original_and_shown_differ")
    renderer = f"{top}/{ORIGINAL_DIR}/{RENDERER}"
    run(
        "更新を有効にした Vencord のビルド",
        good.with_entry(renderer, good.entries[renderer].replace(b"Updater Disabled: true", b"Updater Disabled: false")),
        "vencord_dist",
    )
    run("ini にトークンの形の文字列がある", good.with_entry(f"{top}/{LOCALE_PREFIX}en-US.ini", ("x=" + "ab" * 32 + "\n").encode()), "secret_shape")
    run("ini に CRLF がある", good.with_entry(f"{top}/{LOCALE_PREFIX}en-US.ini", b"a=b\r\n"), "carriage_return")
    run("licenses/ の CRLF は許す", good.with_entry(f"{top}/licenses/cordmux/LICENSE", b"Apache text\r\n"), None)
    offer = f"{top}/{offer_name}"
    offer_text = good.entries[offer].decode("utf-8")
    lgpl_only = "\n".join(line.replace("GPL", "LGPL") if "FFmpeg" in line else line for line in offer_text.split("\n"))
    run("ソースコードの入手先.txt で FFmpeg が LGPL だけ", good.with_entry(offer, lgpl_only.encode()), "source_offer")
    run("ソースコードの入手先.txt に svt-av1 がない", good.with_entry(offer, offer_text.replace("- svt-av1 2.3.0", "- other 2.3.0").encode()), "source_offer")
    run("ソースコードの入手先.txt に asio がない", good.with_entry(offer, offer_text.replace("- asio 1.32.0", "- other 1.32.0").encode()), "source_offer")
    run("--expect-build-id と違うビルド ID", good, "build_id", expect="0.1.0+fff0000")
    # cordmux-build.txt の 3 つ目のキー vencord_tag ([バージョン情報] ページの「同梱のVencord」に表示する値)。
    shown_mark = f"{top}/{SHOWN_DIR}/{mark}"
    mark_text = good.entries[shown_mark].decode("utf-8")
    mark_tag_line = next(line for line in mark_text.split("\n") if line.startswith("vencord_tag: "))
    run("cordmux-build.txt に vencord_tag がない", good.with_entry(shown_mark, mark_text.replace(mark_tag_line + "\n", "").encode()), "build_mark")
    run("cordmux-build.txt の vencord_tag に v がない", good.with_entry(shown_mark, mark_text.replace(mark_tag_line, "vencord_tag: 1.15.1").encode()), "build_mark")
    run("cordmux-build.txt の vencord_tag が vencord.lock.json と違う", good.with_entry(shown_mark, mark_text.replace(mark_tag_line, "vencord_tag: v9.9.9").encode()), "build_mark")
    mark_commit_line = next(line for line in mark_text.split("\n") if line.startswith("vencord_commit: "))
    run("cordmux-build.txt の vencord_commit が vencord.lock.json と違う", good.with_entry(shown_mark, mark_text.replace(mark_commit_line, "vencord_commit: " + "e" * 40).encode()), "build_mark")
    run("data/vencord-src と data/vencord の cordmux-build.txt が違う", good.with_entry(shown_mark, mark_text.replace(f"build_id: {build_id}", "build_id: 0.1.0+fff0000").encode()), "build_mark")
    run("DLL にビルド ID がない", good.with_entry(f"{top}/{DLL_PREFIX}{PLUGIN_DLL}", b"MZ other"), "build_id")
    ascii_flags = ZipView(good.entries, {name: 0 for name in good.entries})
    run("ASCII でない名前に UTF-8 のフラグがない", ascii_flags, "utf8_flag")

    fill_and_guide_selftest(good, top, run, failures)

    # テストデータの exe を起動するチェックは、import をたどる段階で止まる (ダイアログを表示しない)。
    fake = check_view(good, exec_ffmpeg=True, expect_build_id=build_id)
    if "ffmpeg_exec" not in _codes(fake) or (fake.get("ffmpeg") or {}).get("ran"):
        failures.append(f"テストデータの ffmpeg.exe: 起動せずに ffmpeg_exec になるはずが {fake.get('ffmpeg')}")

    # 実際の zip (--material): 起動のチェックまで成功し、cordmux フォルダーの直下から 1 ファイルずつ抜くと全部失敗する。
    if material is not None:
        real = load_view(material)
        real_result = check_view(real)
        if not real_result["ok"]:
            failures.append(f"--material: 成功のはずが失敗した: {real_result['problems']}")
        real_top = real_result.get("top", "")
        for name in runtime["all"]:
            broken = check_view(real.without(f"{real_top}/{DLL_PREFIX}{name}"))
            codes = _codes(broken)
            if broken["ok"] or "missing" not in codes or "ffmpeg_exec" not in codes:
                failures.append(f"--material から {name} を抜く: missing と ffmpeg_exec のはずが {broken['problems']}")
            if (broken.get("ffmpeg") or {}).get("ran"):
                failures.append(f"--material から {name} を抜く: DLL が足りないのに ffmpeg.exe を起動した")

    if failures:
        for failure in failures:
            print(f"selftest 失敗: {failure}", file=sys.stderr)
        return 1
    extent = "テストデータと --material の zip" if material is not None else "テストデータ"
    print(f"selftest OK: check_dist_zip ({extent}。正しいデータが通り、壊したデータがどれも失敗する)")
    return 0


def main(argv: Optional[list[str]] = None) -> int:
    dist_common.use_utf8_console()
    parser = argparse.ArgumentParser(description="cordmux の配布用の zip の中身をチェックする")
    parser.add_argument("zip", nargs="?", help="cordmux-<label>.zip")
    parser.add_argument("--expect-build-id", help="cordmux-build.txt、DLL、Vencord の本体の JS に、このビルド ID があることも確認する")
    parser.add_argument(
        "--formal-release",
        action="store_true",
        help="正式版として公開できるかも確認する (手順書に placeholder がない)",
    )
    parser.add_argument("--selftest", action="store_true", help="このツールのセルフテストを実行する")
    parser.add_argument("--material", help="--selftest と一緒に使う。実際の zip がチェックに通り、1 ファイルずつ抜くと失敗することも確認する")
    args = parser.parse_args(argv)
    try:
        if args.selftest:
            return selftest(Path(args.material) if args.material else None)
        if not args.zip:
            parser.error("--selftest を付けないときは、zip を指定する")
        result = check_zip(Path(args.zip), args.expect_build_id, args.formal_release)
    except DistError as exc:
        dist_common.print_json({"file": args.zip, "ok": False, "problems": [{"code": "setup", "detail": str(exc)}]})
        return 1
    dist_common.print_json(result)
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
