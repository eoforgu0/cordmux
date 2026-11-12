#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# 配布用の zip を作る。最上位のフォルダー cordmux-<label>/ の下に、dist/ のファイル (top_level_files.txt のとおり)、
# 手順書 (guide/ の Markdown を guide_to_text.py で変換したテキスト) と cordmux/ (--plugin-dir の全部のファイル、Vencord の
# dist) を置く。並びは assemble にある。
# top_level_files.txt の fill-values の行で指定したファイルは、{{source_url}} にこのバージョンのソースの URL を差し込んでから置く。
# 中身のチェックは check_dist_zip.py がする。このツールは、入力が足りないか形が違うときだけ止まる。
# CI (.github/workflows/dist.yml) もローカルも同じ引数で実行する (引数は README.md と --help にある)。

import argparse
import os
import re
import sys
import tempfile
import time
import zipfile
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_info  # noqa: E402  (タグの形を build_info.TAG_RE の 1 か所に保つ)
import dist_common  # noqa: E402  (ツールが共有する関数と定数)
import guide_to_text  # noqa: E402  (手順書の Markdown を zip のテキストにする)
from dist_common import DistError  # noqa: E402

sys.path.insert(0, str(dist_common.REPO_ROOT / "test" / "lint"))
import install_guide  # noqa: E402  (手順書のセット (target、zip_name) と、セルフテストの手順書の見出しと role)

PLUGIN_FOLDER = "cordmux"
TOP_PREFIX = "cordmux-"
VENCORD_SUBDIRS = ("data/vencord-src", "data/vencord")
ZIP_EPOCH_MIN = 315532800  # 1980-01-01T00:00:00Z (zip の形式が持てる最も古い時刻)


def top_folder_for(out: Path) -> str:
    # 最上位のフォルダーの名前は --out のファイル名から取る。zip の名前と中のフォルダーの名前を、2 つの引数で
    # 食い違わせないため。
    name = out.name
    if not name.endswith(".zip") or not name.startswith(TOP_PREFIX) or len(name) <= len(TOP_PREFIX + ".zip"):
        raise DistError(f"--out の名前は cordmux-<label>.zip にする: {name}")
    return name[: -len(".zip")]


def label_for(top: str) -> str:
    # 最上位のフォルダーの名前から label を取る。label はビルド ID の形 (1.0.0+abc1234、ローカルの 1.0.0+dev) か、
    # タグから v を除いた形 (1.0.0、1.0.0-rc1) だけを受け入れる。label の形で source_url の書式を決めるので、
    # どちらでもない名前だと、存在しないタグのリリースの URL を差し込んでしまうため。
    label = top[len(TOP_PREFIX) :]
    if not (dist_common.BUILD_ID_RE.match(label) or build_info.TAG_RE.match("v" + label)):
        raise DistError(f"label {label!r} がビルド ID (1.0.0+abc1234) でも、v を除いたタグ (1.0.0、1.0.0-rc1) でもない")
    return label


def fill_dist(files: dict[str, bytes], label: str, cordmux_commit: str) -> dict[str, bytes]:
    # fill-values の行で指定したファイルの {{source_url}} を置き換えた dict を返す。source_url は label に + があれば
    # cordmux_commit のツリー、なければ同じタグのリリースを指す (dist_common.source_url_for)。指定していないファイル
    # (licenses/ など) と、optional-file でないファイルはそのまま。dist/ のファイル自体は書き換えない。
    values = {"source_url": dist_common.source_url_for(label, cordmux_commit)}
    filled = dict(files)
    for name in dist_common.fill_targets():
        if name not in files:
            continue
        text = files[name].decode("utf-8")
        filled[name] = dist_common.fill_text(text, values, where=name).encode("utf-8")
    return filled


def collect_guides(guide_dir: Path, label: str, cordmux_commit: str) -> dict[str, bytes]:
    # 手順書の lint の rules のセットごとに、guide_dir の target と同じ名前の Markdown (ja.md、en.md) を zip のテキストに
    # 変換し、zip_name -> 中身 で返す。手順書の URL は target (guide/ja.md など) を、このバージョンのタグかコミットで指す。
    # Markdown がないか、使ってよい記法の外の書き方があれば DistError (guide_to_text.convert)。
    try:
        blocks = install_guide.load_rules()
    except (OSError, ValueError) as exc:
        raise DistError(f"手順書の lint の rules: {exc}") from exc
    source_url = dist_common.source_url_for(label, cordmux_commit)
    files: dict[str, bytes] = {}
    for block in blocks:
        path = guide_dir / Path(block.target).name
        if not path.is_file():
            raise DistError(f"{guide_dir} に手順書 {path.name} がない")
        text = guide_to_text.convert(
            path.read_bytes().decode("utf-8"), install_guide.guide_lang(block),
            dist_common.guide_url_for(label, cordmux_commit, block.target), source_url, where=block.target,
        )
        files[block.zip_name] = text.encode("utf-8")
    return files


def collect_plugin(plugin_dir: Path) -> dict[str, bytes]:
    # cmake --install のインストール先の cordmux/ を、相対の名前 -> 中身 で返す (.pdb を除く)。
    if not (plugin_dir / "cordmux.dll").is_file():
        raise DistError(f"{plugin_dir} に cordmux.dll がない (<インストール先>/cordmux を指定する)")
    files: dict[str, bytes] = {}
    for path in sorted(plugin_dir.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(plugin_dir).as_posix()
        if path.suffix.lower() == ".pdb":
            continue
        if any(relative.startswith(sub + "/") for sub in VENCORD_SUBDIRS):
            raise DistError(f"{plugin_dir} に {relative} が既にある (このツールが追加するファイル。前に作ったものが残っていないかを確認する)")
        files[relative] = path.read_bytes()
    return files


def collect_vencord(vencord_dist: Path, build_id: str, vencord_commit: str, vencord_tag: str) -> dict[str, bytes]:
    # data/vencord-src に置くファイルを 名前 -> 中身 で返す (Vencord の dist の 5 ファイル、本体の JS が指す
    # .LEGAL.txt、cordmux-build.txt)。
    names = dist_common.build_mark_names()
    read = dist_common.check_dist.dir_reader(vencord_dist)
    selected = dist_common.vencord_selection(read, names["file_name"])
    files: dict[str, bytes] = {}
    missing = []
    for name in selected:
        if name == names["file_name"]:
            files[name] = dist_common.build_mark_text(build_id, vencord_commit, vencord_tag, names).encode("utf-8")
            continue
        data = read(name)
        if data is None:
            missing.append(name)
            continue
        files[name] = data
    if missing:
        raise DistError(f"{vencord_dist} に {missing} がない (.LEGAL.txt は本体の JS が指すファイル)")
    return files


def collect_dist(dist_dir: Path) -> dict[str, bytes]:
    # top_level_files.txt のとおりに、dist/ から最上位に置くファイルを 名前 -> 中身 で返す。
    files: dict[str, bytes] = {}
    for kind, name in dist_common.top_level_entries():
        path = dist_dir / name
        if kind == "required-dir":
            if not path.is_dir():
                raise DistError(f"{dist_dir} にフォルダー {name} がない")
            found = [p for p in sorted(path.rglob("*")) if p.is_file()]
            if not found:
                raise DistError(f"{dist_dir / name} にファイルがない")
            for p in found:
                files[p.relative_to(dist_dir).as_posix()] = p.read_bytes()
        elif path.is_file():
            files[name] = path.read_bytes()
        elif kind != "optional-file":
            raise DistError(f"{dist_dir} に {name} がない")
    return files


def assemble(
    plugin_dir: Path,
    vencord_dist: Path,
    build_id: str,
    vencord_commit: str,
    cordmux_commit: str,
    dist_dir: Path,
    top: str,
    guide_dir: Path = dist_common.DEFAULT_GUIDE_DIR,
) -> dict[str, bytes]:
    # zip に入れるファイルを、zip の中の名前 -> 中身 で返す。並びは次のとおり。
    #   - cordmux-<label>/ (top): 最上位のフォルダー
    #   - その直下: --dist-dir の top_level_files.txt のファイル (ソースコードの入手先.txt、licenses/ など) と、--guide-dir の
    #     手順書を変換したテキスト (はじめに.txt と README-en.txt。collect_guides)
    #   - cordmux/: --plugin-dir の全部のファイル (.pdb を除く)
    #   - cordmux/data/vencord-src/: Vencord の dist の 5 ファイル、本体の JS が指す .LEGAL.txt、cordmux-build.txt
    #   - cordmux/data/vencord/: data/vencord-src と同じもの (Vesktop が読むフォルダー。data/vencord-src はコピー元)
    # Vencord の dist のほかのファイル (.map や patcher.js など) は入れない。
    if not dist_common.BUILD_ID_RE.match(build_id):
        raise DistError(f"ビルド ID の形が違う: {build_id!r}")
    if not dist_common.FULL_COMMIT_RE.match(vencord_commit):
        raise DistError("Vencord のコミットは 40 桁の小文字の 16 進数で指定する")
    label = label_for(top)
    # Vencord のコミットは src-vencord/vencord.lock.json の commit と同じでなければならない。cordmux-build.txt の
    # vencord_tag は lock の tag から取る。引数を増やすと、コミットとタグが食い違う zip を作れてしまうため。
    lock = dist_common.vencord_lock()
    if vencord_commit != lock["commit"]:
        raise DistError(f"Vencord のコミット {vencord_commit} が src-vencord/vencord.lock.json の {lock['commit']} と違う")
    vencord_tag = lock["tag"]
    if not dist_common.VENCORD_TAG_RE.match(vencord_tag):
        raise DistError(f"vencord.lock.json の tag の形が違う: {vencord_tag!r}")
    # cordmux のコミットは、source_url に使う。ビルド ID の短いコミット (ローカルの +dev のような 16 進数でない値は除く) で
    # 始まっていなければ、別のコミットの値を渡している。
    if cordmux_commit:
        if not dist_common.FULL_COMMIT_RE.match(cordmux_commit):
            raise DistError("cordmux のコミットは 40 桁の小文字の 16 進数で指定する")
        short = build_id.split("+", 1)[1]
        if re.fullmatch(r"[0-9a-f]+", short) and not cordmux_commit.startswith(short):
            raise DistError(f"cordmux のコミット {cordmux_commit} が、ビルド ID {build_id} の短いコミットで始まらない")
    entries: dict[str, bytes] = {}
    top_files = fill_dist(collect_dist(dist_dir), label, cordmux_commit)
    guides = collect_guides(guide_dir, label, cordmux_commit)
    clash = sorted(set(top_files) & set(guides))
    if clash:
        raise DistError(f"{dist_dir} に、手順書を変換して置く名前と同じファイルがある: {clash}")
    for name, data in {**top_files, **guides}.items():
        entries[f"{top}/{name}"] = data
    for name, data in collect_plugin(plugin_dir).items():
        entries[f"{top}/{PLUGIN_FOLDER}/{name}"] = data
    vencord = collect_vencord(vencord_dist, build_id, vencord_commit, vencord_tag)
    for sub in VENCORD_SUBDIRS:
        for name, data in vencord.items():
            entries[f"{top}/{PLUGIN_FOLDER}/{sub}/{name}"] = data
    return entries


def zip_date_time(epoch: int) -> tuple:
    return time.gmtime(max(epoch, ZIP_EPOCH_MIN))[:6]


def write_zip(entries: dict[str, bytes], out: Path, epoch: int, compression: int = zipfile.ZIP_DEFLATED) -> None:
    # 名前の順に、同じ時刻 (epoch。UTC) で書く。同じ入力から同じバイト列の zip を作るため。書き終えてから
    # 置き換える (途中で止まった zip を out に残さない)。
    #
    # - フォルダーの項目は入れず、ファイルの項目だけを入れる。エクスプローラーの展開は、途中のフォルダーを作る。
    # - zip を作った OS (create_system) と属性 (external_attr) は 0 にする。Windows で作っても Linux で作っても、同じ
    #   バイト列にするため。属性の 0 は、Python の zipfile が書くときに 0o600 << 16 (0x1800000) に置き換える
    #   (writestr の既定。Python 3.12.9 で確認した) ので、zip の中ではどの項目もその値になる。
    # - 名前の UTF-8 のフラグは、zipfile が ASCII でない名前 (はじめに.txt など) に付ける。ASCII だけの名前は、
    #   フラグがなくても同じ文字列として読まれる。zipfile は書くときにフラグを作り直すので、ASCII の名前に
    #   フラグを付ける方法はない。
    out.parent.mkdir(parents=True, exist_ok=True)
    partial = out.with_name(out.name + ".partial")
    date_time = zip_date_time(epoch)
    with zipfile.ZipFile(partial, "w") as archive:
        for name in sorted(entries):
            info = zipfile.ZipInfo(name, date_time=date_time)
            info.compress_type = compression
            info.create_system = 0
            info.external_attr = 0
            archive.writestr(info, entries[name])
    os.replace(partial, out)


def make(args: argparse.Namespace) -> dict:
    out = Path(args.out)
    top = top_folder_for(out)
    entries = assemble(
        Path(args.plugin_dir),
        Path(args.vencord_dist),
        args.build_id,
        args.vencord_commit,
        args.cordmux_commit or "",
        Path(args.dist_dir),
        top,
        Path(args.guide_dir),
    )
    write_zip(entries, out, int(args.source_date_epoch))
    return {"out": str(out), "top": top, "label": label_for(top), "entries": len(entries), "bytes": out.stat().st_size}


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="cordmux の配布用の zip を作る")
    parser.add_argument("--selftest", action="store_true", help="このツールのセルフテストを実行する")
    parser.add_argument("--plugin-dir", help="<cmake --install のインストール先>/cordmux")
    parser.add_argument("--vencord-dist", help="Vencord のビルドの dist フォルダー")
    parser.add_argument("--build-id", help="<バージョン>+<短いコミット>")
    parser.add_argument("--vencord-commit", help="固定した Vencord のコミット (40 桁の 16 進数)")
    parser.add_argument(
        "--cordmux-commit",
        help="cordmux のコミット (40 桁の 16 進数。ビルド ID の短いコミットで始まるもの)。label に + があるとき、ソースの URL に使う",
    )
    parser.add_argument("--source-date-epoch", help="cordmux のコミットの時刻 (秒)")
    parser.add_argument("--out", help="作る zip のパス。名前は cordmux-<label>.zip にする")
    parser.add_argument("--dist-dir", default=str(dist_common.DEFAULT_DIST_DIR), help="既定: <リポジトリ>/dist")
    parser.add_argument("--guide-dir", default=str(dist_common.DEFAULT_GUIDE_DIR),
                        help="手順書の Markdown のフォルダー (既定: <リポジトリ>/guide)")
    return parser


REQUIRED_ARGS = ("plugin_dir", "vencord_dist", "build_id", "vencord_commit", "source_date_epoch", "out")


# ---- セルフテストのデータ (check_dist_zip.py のセルフテストも使う) -------------------


SAMPLE_BUILD_ID = "0.1.0+abc1234"
# Vencord のコミットとタグは lock と同じ値にする (zip を作るときに lock と照合するため)。
SAMPLE_VENCORD_COMMIT = dist_common.vencord_lock()["commit"]
SAMPLE_VENCORD_TAG = dist_common.vencord_lock()["tag"]
SAMPLE_CORDMUX_COMMIT = "abc1234def0123456789abcdef0123456789abcd"
SAMPLE_EPOCH = 1790000000
SAMPLE_CONFIGURE = (
    "--enable-libaom --enable-libsvtav1 --enable-libtheora --enable-libmp3lame --enable-gpl "
    "--enable-libx264 --enable-libopus --enable-libvorbis --enable-libvpx --enable-librist "
    "--enable-libsrt --enable-shared --enable-zlib"
)
SAMPLE_MAINTENANCE = "sample maintenance text"
SAMPLE_SOURCE_OFFER = (
    "cordmux\n\n"
    "FFmpeg is distributed under the GPL (version 3 or later).\n"
    "Source of this version: " + dist_common.FILL_OPEN + "source_url}}\n\n"
    "- cordmux tag (Apache-2.0): https://example.invalid/cordmux\n"
    "- Vencord v1.15.1 (GPL-3.0-or-later): https://example.invalid/vencord\n"
    "- pnpm-lock.yaml same commit: https://example.invalid/lock\n"
    "- obs-deps 2025-07-11: https://example.invalid/obs-deps\n"
    "- FFmpeg n7.1.1 (GPL-3.0-or-later): https://example.invalid/ffmpeg\n"
    "- x264 r3106: https://example.invalid/x264\n"
    "- srt 1.5.2: https://example.invalid/srt\n"
    "- librist 0.2.7: https://example.invalid/librist\n"
    "- mbedtls 3.6.4: https://example.invalid/mbedtls\n"
    "- zlib 1.3.1: https://example.invalid/zlib\n"
    "- aom 3.12.0: https://example.invalid/aom\n"
    "- svt-av1 2.3.0: https://example.invalid/svt-av1\n"
    "- theora 1.1.1: https://example.invalid/theora\n"
    "- mp3lame 3.100: https://example.invalid/lame\n"
    "- opus 1.5.2: https://example.invalid/opus\n"
    "- vorbis 1.3.7: https://example.invalid/vorbis\n"
    "- vpx 1.14.1: https://example.invalid/vpx\n"
    "- websocketpp 0.8.2: https://example.invalid/websocketpp\n"
    "- asio 1.32.0: https://example.invalid/asio\n"
    "- nlohmann/json 3.11.3: https://example.invalid/json\n"
)


def sample_guide_text(block) -> str:
    # rules の組 1 つ分の、テストデータの手順書 (Markdown) を作る。rules のタイトルと ## の見出しを順に並べ、maintenance の
    # 節に 1 文を置き、最後の節に zip 用の差し込みの印を置く。見出しの文字列は rules から取り、ここには持たない。
    lines = [f"# {block.title}", ""]
    for heading in block.headings:
        lines += [install_guide.render_heading(heading), ""]
        lines += [SAMPLE_MAINTENANCE if heading == block.roles["maintenance"] else "sample text", ""]
    lines += [install_guide.MARKER_LINE, ""]
    return "\n".join(lines)


def write_sample_inputs(root: Path, build_id: str = SAMPLE_BUILD_ID) -> dict:
    # 実際の入力と同じ形の小さな入力を root の下に作り、make の引数に使うパスを返す。
    # DLL と exe の中身は PE ではないので、このデータでは ffmpeg.exe を起動するチェックはできない。
    runtime = dist_common.ffmpeg_runtime()
    plugin = root / "install" / PLUGIN_FOLDER
    bin_dir = plugin  # cordmux.dll と同梱のファイルは、インストール先の cordmux の直下に置く
    bin_dir.mkdir(parents=True)
    (bin_dir / "cordmux.dll").write_bytes(b"MZ fake cordmux " + build_id.encode("ascii") + b"\0")
    (bin_dir / "cordmux.pdb").write_bytes(b"fake pdb 0000001f0000001f0000001f0000001f0000001f0000001f0000001f0000001f")
    for name in runtime["all"]:
        (bin_dir / name).write_bytes(b"MZ fake " + name.encode("ascii"))
    locale = plugin / "data" / "locale"
    locale.mkdir(parents=True)
    for name in ("ja-JP.ini", "en-US.ini"):
        (locale / name).write_bytes(b"cordmux.dock.title=cordmux\n")

    vencord = root / "vencord-dist"
    vencord.mkdir()
    header = "// Vencord aa5a764b\n// Standalone: true\n// Platform: Universal\n// Updater Disabled: true\n"
    renderer = header + f'"use strict";var v="{build_id}";\n/*! For license information please see vencordDesktopRenderer.js.LEGAL.txt */\n'
    main_js = header + '"use strict";\n/*! For license information please see vencordDesktopMain.js.LEGAL.txt */\n'
    (vencord / "vencordDesktopRenderer.js").write_bytes(renderer.encode("utf-8"))
    (vencord / "vencordDesktopMain.js").write_bytes(main_js.encode("utf-8"))
    (vencord / "vencordDesktopPreload.js").write_bytes((header + '"use strict";\n').encode("utf-8"))
    (vencord / "vencordDesktopRenderer.css").write_bytes(b"body{}\n")
    (vencord / "package.json").write_bytes(b"{}\n")
    (vencord / "vencordDesktopRenderer.js.LEGAL.txt").write_bytes(b"license notes renderer\n")
    (vencord / "vencordDesktopMain.js.LEGAL.txt").write_bytes(b"license notes main\n")
    (vencord / "vencordDesktopRenderer.js.map").write_bytes(b"{}\n")
    (vencord / "patcher.js").write_bytes(b"patcher\n")

    dist_dir = root / "dist"
    (dist_dir / "licenses" / "cordmux").mkdir(parents=True)
    (dist_dir / "licenses" / "cordmux" / "LICENSE").write_bytes(b"Apache-2.0 text\n")
    (dist_dir / "README.md").write_bytes(b"not shipped\n")
    # 手順書の Markdown は rules の組ごとに作る。
    guide_dir = root / "guide"
    guide_dir.mkdir()
    for block in install_guide.load_rules():
        (guide_dir / Path(block.target).name).write_bytes(sample_guide_text(block).encode("utf-8"))
    for kind, name in dist_common.top_level_entries():
        if kind == "source-offer":
            (dist_dir / name).write_bytes(SAMPLE_SOURCE_OFFER.encode("utf-8"))
        elif kind == "required-file" and not (dist_dir / name).exists():
            (dist_dir / name).write_bytes(b"guide\n")

    return {
        "plugin_dir": plugin,
        "vencord_dist": vencord,
        "cordmux_commit": SAMPLE_CORDMUX_COMMIT,
        "dist_dir": dist_dir,
        "guide_dir": guide_dir,
        "build_id": build_id,
        "vencord_commit": SAMPLE_VENCORD_COMMIT,
        "epoch": SAMPLE_EPOCH,
    }


def sample_args(sample: dict, out: Path) -> argparse.Namespace:
    return build_parser().parse_args(
        [
            "--plugin-dir", str(sample["plugin_dir"]),
            "--vencord-dist", str(sample["vencord_dist"]),
            "--build-id", sample["build_id"],
            "--vencord-commit", sample["vencord_commit"],
            "--cordmux-commit", sample["cordmux_commit"],
            "--source-date-epoch", str(sample["epoch"]),
            "--out", str(out),
            "--dist-dir", str(sample["dist_dir"]),
            "--guide-dir", str(sample["guide_dir"]),
        ]
    )


def selftest() -> int:
    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="cordmux-make-dist-zip-") as tmp:
        root = Path(tmp)
        sample = write_sample_inputs(root / "in")
        out = root / "out" / f"cordmux-{SAMPLE_BUILD_ID}.zip"
        make(sample_args(sample, out))
        top = f"cordmux-{SAMPLE_BUILD_ID}"
        with zipfile.ZipFile(out) as archive:
            infos = archive.infolist()
            names = [info.filename for info in infos]
            if names != sorted(names):
                failures.append("項目が名前の順に並んでいない")
            if any(not name.startswith(top + "/") for name in names):
                failures.append("最上位のフォルダーの外に項目がある")
            if any(info.date_time != zip_date_time(SAMPLE_EPOCH) for info in infos):
                failures.append("時刻の違う項目がある")
            non_ascii = [info for info in infos if not info.filename.isascii()]
            if not non_ascii or any(not info.flag_bits & dist_common.ZIP_UTF8_FLAG for info in non_ascii):
                failures.append("ASCII でない名前に UTF-8 のフラグがない (またはそのような名前が 1 つもない)")
            if any(name.lower().endswith((".pdb", ".map")) or name.endswith("/patcher.js") for name in names):
                failures.append(".pdb、.map、patcher.js が zip に入っている")
            if any(name.endswith("/README.md") for name in names):
                failures.append("dist/README.md が zip に入っている")
            mark_name = dist_common.build_mark_names()["file_name"]
            for sub in VENCORD_SUBDIRS:
                mark = archive.read(f"{top}/{PLUGIN_FOLDER}/{sub}/{mark_name}")
                if mark != dist_common.build_mark_text(SAMPLE_BUILD_ID, SAMPLE_VENCORD_COMMIT, SAMPLE_VENCORD_TAG).encode("utf-8"):
                    failures.append(f"{sub} の cordmux-build.txt が違う: {mark!r}")
                for legal in ("vencordDesktopRenderer.js.LEGAL.txt", "vencordDesktopMain.js.LEGAL.txt"):
                    if f"{top}/{PLUGIN_FOLDER}/{sub}/{legal}" not in names:
                        failures.append(f"{sub} に {legal} がない")
            if f"{top}/licenses/cordmux/LICENSE" not in names:
                failures.append("licenses/ がコピーされていない")
        first = out.read_bytes()
        make(sample_args(sample, out))
        if out.read_bytes() != first:
            failures.append("同じ入力から 2 回作ると、バイト列が違う")

        def refused(label: str, mutate, restore) -> None:
            mutate()
            try:
                make(sample_args(sample, root / "out" / f"cordmux-{SAMPLE_BUILD_ID}.zip"))
                failures.append(f"{label}: 作らないはずが作った")
            except DistError:
                pass
            finally:
                restore()

        legal = sample["vencord_dist"] / "vencordDesktopMain.js.LEGAL.txt"
        saved = legal.read_bytes()
        refused(".LEGAL.txt がない", legal.unlink, lambda: legal.write_bytes(saved))
        css = sample["vencord_dist"] / "vencordDesktopRenderer.css"
        saved_css = css.read_bytes()
        refused("Vencord の dist の 5 ファイルの 1 つがない", css.unlink, lambda: css.write_bytes(saved_css))
        offer_name = [name for kind, name in dist_common.top_level_entries() if kind == "source-offer"][0]
        offer = sample["dist_dir"] / offer_name
        saved_offer = offer.read_bytes()
        refused("ソースコードの入手先.txt がない", offer.unlink, lambda: offer.write_bytes(saved_offer))
        stray = sample["plugin_dir"] / "data" / "vencord" / "stray.txt"
        stray.parent.mkdir(parents=True, exist_ok=True)
        refused("インストール先に前の Vencord のフォルダーが残っている", lambda: stray.write_bytes(b"x"), stray.unlink)
        # Vencord のコミットが lock と違うとき、cordmux のコミットの形が違うかビルド ID と合わないときは zip を作らない。
        for label, field, value in (
            ("Vencord のコミットが lock と違う", "vencord_commit", "f" * 40),
            ("cordmux のコミットがビルド ID の短いコミットで始まらない", "cordmux_commit", "f" * 40),
            ("cordmux のコミットが 40 桁でない", "cordmux_commit", "abc1234"),
        ):
            bad_args = sample_args(sample, out)
            setattr(bad_args, field, value)
            try:
                make(bad_args)
                failures.append(f"{label}: 作らないはずが作った")
            except DistError:
                pass
        args = sample_args(sample, root / "out" / "wrong-name.zip")
        try:
            make(args)
            failures.append("--out の名前が違う: 作らないはずが作った")
        except DistError:
            pass
        args = sample_args(sample, out)
        args.build_id = "0.1.0"
        try:
            make(args)
            failures.append("ビルド ID の形が違う: 作らないはずが作った")
        except DistError:
            pass

        fill_selftest(root, sample, refused, failures)

    top_level_selftest(failures)

    if failures:
        for failure in failures:
            print(f"selftest 失敗: {failure}", file=sys.stderr)
        return 1
    print("selftest OK: make_dist_zip (並び、時刻、UTF-8 のフラグ、入れないファイル、cordmux-build.txt、同じバイト列、作らない入力、差し込みと手順書の変換)")
    return 0


def fill_selftest(root: Path, sample: dict, refused, failures: list[str]) -> None:
    # 差し込みと手順書の変換のセルフテスト。{{ と <!-- が残らないこと、label の + の有無で source_url と手順書の URL が
    # 変わること、知らないキー・形の違う {{・値のないキー・どちらの形でもない label・cordmux のコミットがない + 付きの
    # label・手順書の Markdown がないか記法の外の書き方があるときは zip を作らないこと、fill-values で指定していない
    # ファイルは変えないことを確認する。
    fills = dist_common.fill_targets()
    offer_name = [name for kind, name in dist_common.top_level_entries() if kind == "source-offer"][0]
    commit_url = dist_common.SOURCE_URL_COMMIT_FORMAT.format(commit=SAMPLE_CORDMUX_COMMIT)
    blocks = install_guide.load_rules()
    guide_names = [block.zip_name for block in blocks]
    if offer_name not in fills:
        failures.append(f"fill-values の行が、ソースコードの入手先.txt を指定していない: {fills}")
    if any(name in fills for name in guide_names):
        failures.append(f"fill-values の行が手順書を指定している (手順書は Markdown から変換するときに URL を入れる): {fills}")

    def build(label: str) -> dict[str, bytes]:
        out = root / "fill" / f"cordmux-{label}.zip"
        make(sample_args(sample, out))
        with zipfile.ZipFile(out) as archive:
            return {info.filename.split("/", 1)[1]: archive.read(info) for info in archive.infolist()}

    got = build(SAMPLE_BUILD_ID)
    for name in list(fills) + guide_names:
        if name in got and (dist_common.FILL_OPEN.encode("ascii") in got[name] or b"<!--" in got[name]):
            failures.append(f"{name}: 差し込みの書式か印が残っている")
    for block in blocks:
        name = block.zip_name
        guide_url = dist_common.guide_url_for(SAMPLE_BUILD_ID, SAMPLE_CORDMUX_COMMIT, block.target)
        if name not in got:
            failures.append(f"{name}: zip にない")
        elif commit_url.encode("utf-8") not in got[name] or guide_url.encode("utf-8") not in got[name]:
            failures.append(f"{name}: ソースの URL か手順書の URL (コミット) が手順書にない")
        elif SAMPLE_MAINTENANCE.encode("utf-8") not in got[name]:
            failures.append(f"{name}: 手順書の本文が入っていない")
    offer_text = got.get(offer_name, b"").decode("utf-8")
    if commit_url not in offer_text or "/releases/tag/" in offer_text:
        failures.append(f"+ のある label: source_url はコミットのツリーのはずが {offer_text.splitlines()[:4]}")
    for label in ("0.1.0-rc1", "0.1.0"):
        tag_url = dist_common.SOURCE_URL_TAG_FORMAT.format(label=label)
        files = build(label)
        offer_text = files.get(offer_name, b"").decode("utf-8")
        if tag_url + "\n" not in offer_text or commit_url in offer_text:
            failures.append(f"label {label}: source_url は {tag_url} のはずが {offer_text.splitlines()[:4]}")
        for block in blocks:
            guide_text = files.get(block.zip_name, b"").decode("utf-8")
            guide_url = dist_common.guide_url_for(label, "", block.target)
            if tag_url not in guide_text or guide_url not in guide_text or commit_url in guide_text:
                failures.append(f"label {label}: {block.zip_name} の URL がタグのものでない")
    try:
        dist_common.fill_text("a " + dist_common.FILL_OPEN + "source_url}}", {})
        failures.append("fill_text: 値のないキーを受け入れた")
    except DistError:
        pass

    offer = sample["dist_dir"] / offer_name
    saved_offer = offer.read_bytes()
    for label, extra in (
        ("知らない差し込みのキー", dist_common.FILL_OPEN + "no_such_key}}"),
        ("空白を含む差し込みの書式", dist_common.FILL_OPEN + " source_url }}"),
        ("大文字を含む差し込みの書式", dist_common.FILL_OPEN + "Source_url}}"),
        ("閉じていない差し込みの書式", dist_common.FILL_OPEN + "source_url"),
    ):
        refused(label, lambda extra=extra: offer.write_bytes(saved_offer + ("\n" + extra + "\n").encode("utf-8")),
                lambda: offer.write_bytes(saved_offer))
    # 手順書の Markdown: ない、差し込みの書式がある (GitHub の表示にそのまま出る)、手で折り返した段落がある
    guide = sample["guide_dir"] / Path(blocks[0].target).name
    saved_guide = guide.read_bytes()
    refused("手順書の Markdown がない", guide.unlink, lambda: guide.write_bytes(saved_guide))
    for label, extra in (
        ("手順書の Markdown に差し込みの書式", "\n" + dist_common.FILL_OPEN + "source_url}}\n"),
        ("手順書の Markdown に手で折り返した段落", "\na\nb\n"),
    ):
        refused(label, lambda extra=extra: guide.write_bytes(saved_guide + extra.encode("utf-8")),
                lambda: guide.write_bytes(saved_guide))
    # top_level_files.txt が手順書と同じ名前のファイルを置くと、どちらを置くか決まらないので作らない
    # (dist/ のファイルは top_level_files.txt に書いたものだけを読むので、行を足した形で確かめる)
    stale = sample["dist_dir"] / blocks[0].zip_name
    original_entries = dist_common.top_level_entries

    def add_stale_line() -> None:
        stale.write_bytes(b"old\n")
        dist_common.top_level_entries = lambda path=dist_common.TOP_LEVEL_FILES: (
            original_entries(path) + [("optional-file", blocks[0].zip_name)])

    def remove_stale_line() -> None:
        dist_common.top_level_entries = original_entries
        stale.unlink()

    refused("top_level_files.txt が手順書と同じ名前のファイルを置く", add_stale_line, remove_stale_line)
    # label に + があるのに cordmux のコミットがないと、source_url を作れない。
    no_commit = sample_args(sample, root / "fill" / f"cordmux-{SAMPLE_BUILD_ID}.zip")
    no_commit.cordmux_commit = None
    try:
        make(no_commit)
        failures.append("+ のある label で cordmux のコミットがない: 作らないはずが作った")
    except DistError:
        pass
    for label in ("foo", "0.1", "0.1.0+"):
        try:
            make(sample_args(sample, root / "fill" / f"cordmux-{label}.zip"))
            failures.append(f"label {label!r}: 作らないはずが作った")
        except DistError:
            pass

    # fill-values で指定していないファイルは変えない (licenses/ の {{ はそのまま)。
    notice = sample["dist_dir"] / "licenses" / "cordmux" / "NOTICE"
    notice.write_bytes(b"upstream text " + dist_common.FILL_OPEN.encode("ascii") + b"x}}\n")
    try:
        if build(SAMPLE_BUILD_ID).get("licenses/cordmux/NOTICE") != notice.read_bytes():
            failures.append("fill-values で指定していないファイルが変わった")
    finally:
        notice.unlink()


def top_level_selftest(failures: list[str]) -> None:
    # top_level_files.txt の fill-values の行の読み方のセルフテスト。
    base = "required-file guide.txt\noptional-file en.txt\nsource-offer offer.txt\nrequired-dir licenses\n"
    entries, fills = dist_common.parse_top_level(base + "fill-values ja guide.txt\nfill-values en en.txt\n")
    if [kind for kind, _ in entries] != list(dist_common.TOP_LEVEL_KINDS) or fills != {"guide.txt": "ja", "en.txt": "en"}:
        failures.append(f"top_level_files.txt: 正しい行の読み方が違う: {entries} / {fills}")
    for label, extra in (
        ("2 語の行", "fill-values guide.txt\n"),
        ("4 語の行", "fill-values ja guide.txt extra\n"),
        ("知らない言語", "fill-values fr guide.txt\n"),
        ("同じ名前が 2 行", "fill-values ja guide.txt\nfill-values en guide.txt\n"),
        ("置く行にない名前", "fill-values ja other.txt\n"),
        ("フォルダーの名前", "fill-values ja licenses\n"),
    ):
        try:
            dist_common.parse_top_level(base + extra)
            failures.append(f"top_level_files.txt ({label}): 受け入れないはずが受け入れた")
        except DistError:
            pass


def main(argv: Optional[list[str]] = None) -> int:
    dist_common.use_utf8_console()
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    missing = [f"--{name.replace('_', '-')}" for name in REQUIRED_ARGS if not getattr(args, name)]
    if missing:
        parser.error("引数が足りない: " + ", ".join(missing))
    try:
        result = make(args)
    except (DistError, OSError, ValueError) as exc:
        dist_common.print_json({"out": args.out, "ok": False, "error": str(exc)})
        return 1
    result["ok"] = True
    dist_common.print_json(result)
    return 0


if __name__ == "__main__":
    sys.exit(main())
