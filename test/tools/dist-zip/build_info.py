#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# ビルド ID と、CI のワークフローが使うほかの値を出力する。ビルド ID と zip の名前の決め方、タグの確認の規則は、
# ワークフローの YAML に書かずに、ここ 1 か所に置く。
#   meta          ビルド ID、zip の名前、label などを key=value の行で出力する ($GITHUB_OUTPUT にそのまま追記できる形)。
#                 --ref がタグなら、タグの数字の部分が buildspec.json の version と同じかを確認する
#   obs-deps-bin  obs-deps の bin フォルダーのパスを出力する
#   version-ts    src-vencord/version.ts の CORDMUX_BUILD_ID の行だけを書き換える
# CI (.github/workflows/) もローカルも同じ引数で実行する (引数は README.md と --help にある)。

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dist_common  # noqa: E402  (ツールが共有する関数と定数)
from dist_common import DistError  # noqa: E402

TAG_RE = re.compile(r"^v([0-9]+\.[0-9]+\.[0-9]+)(-[0-9A-Za-z.]+)?$")
# 短いコミットは、40 桁の先頭 7 桁に固定する。git rev-parse --short は、ほかのコミットと区別するために桁が
# 伸びることがあり、CMake に渡す値と zip の名前と cordmux-build.txt が食い違いうるため。
SHORT_COMMIT_LENGTH = 7
VERSION_TS_LINE_RE = re.compile(r'^export const CORDMUX_BUILD_ID = "[^"]*";$')


def git_output(*args: str) -> str:
    completed = subprocess.run(["git", *args], cwd=str(dist_common.REPO_ROOT), capture_output=True, text=True)
    if completed.returncode != 0:
        raise DistError(f"git {' '.join(args)} が失敗した: {completed.stderr.strip()}")
    return completed.stdout.strip()


def compute_meta(version: str, commit: str, ref: str, epoch: int, vencord_commit: str) -> dict:
    # ビルド ID と名前を決める (git とファイルは呼び出し元が読む)。
    # タグでないときの zip の名前は cordmux-<build_id>、タグのときは cordmux-<タグから v を除いた文字列>。
    # -rc1 のような接尾辞は、タグと zip の名前 (label) にだけ現れる。
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise DistError(f"コミットは 40 桁の小文字の 16 進数で指定する: {commit!r}")
    short = commit[:SHORT_COMMIT_LENGTH]
    build_id = f"{version}+{short}"
    if not dist_common.BUILD_ID_RE.match(build_id):
        raise DistError(f"ビルド ID の形が違う: {build_id!r}")
    is_tag = ref.startswith("refs/tags/")
    if is_tag:
        tag = ref[len("refs/tags/") :]
        match = TAG_RE.match(tag)
        if not match:
            raise DistError(f"タグ {tag!r} が v<メジャー>.<マイナー>.<パッチ>[-接尾辞] の形でない")
        if match.group(1) != version:
            raise DistError(f"タグ {tag!r} が buildspec.json の version {version!r} と合わない")
        label = tag[1:]
    else:
        label = build_id
    return {
        "version": version,
        "commit": commit,
        "short_commit": short,
        "build_id": build_id,
        "label": label,
        "zip_name": f"cordmux-{label}",
        "source_date_epoch": str(epoch),
        "vencord_commit": vencord_commit,
        "is_tag": "true" if is_tag else "false",
    }


def command_meta(args: argparse.Namespace) -> int:
    commit = args.sha or git_output("rev-parse", "HEAD")
    epoch = int(git_output("log", "-1", "--format=%ct", commit))
    meta = compute_meta(
        dist_common.buildspec_facts()["version"],
        commit,
        args.ref or "",
        epoch,
        dist_common.vencord_lock()["commit"],
    )
    for key, value in meta.items():
        print(f"{key}={value}")
    return 0


def command_obs_deps_bin(_args: argparse.Namespace) -> int:
    # 区切りは / にする (Windows の CI で、bash と C++ の両方がそのまま読めるため)。
    print(dist_common.obs_deps_bin_dir().as_posix())
    return 0


def rewrite_version_ts(text: str, build_id: str) -> str:
    # CORDMUX_BUILD_ID の行だけを書き換える。その行がちょうど 1 つでなければ DistError (version.ts の形が違う)。
    if not dist_common.BUILD_ID_RE.match(build_id):
        raise DistError(f"ビルド ID の形が違う: {build_id!r}")
    lines = text.split("\n")
    hits = [i for i, line in enumerate(lines) if VERSION_TS_LINE_RE.match(line)]
    if len(hits) != 1:
        raise DistError(f"version.ts の CORDMUX_BUILD_ID の行は 1 つのはずが {len(hits)} 個ある")
    lines[hits[0]] = f'export const CORDMUX_BUILD_ID = "{build_id}";'
    return "\n".join(lines)


def command_version_ts(args: argparse.Namespace) -> int:
    path = Path(args.file)
    text = path.read_bytes().decode("utf-8")
    if "\r" in text:
        raise DistError(f"{path} に CR がある")
    dist_common.write_text_lf(path, rewrite_version_ts(text, args.build_id))
    print(f"version_ts={path} build_id={args.build_id}")
    return 0


# ---- セルフテスト ----------------------------------------------------------------


def selftest() -> int:
    failures: list[str] = []

    def expect_error(label: str, func, *args) -> None:
        try:
            func(*args)
        except DistError:
            return
        failures.append(f"{label}: エラーになるはずが通った")

    commit = "0123456789abcdef0123456789abcdef01234567"
    vencord = "aa5a764b7ff9cd0d433201dafcc8966e0edbebd7"
    meta = compute_meta("0.1.0", commit, "refs/heads/main", 1700000000, vencord)
    if meta["build_id"] != "0.1.0+0123456" or meta["zip_name"] != "cordmux-0.1.0+0123456":
        failures.append(f"ブランチのときの meta: {meta}")
    if meta["label"] != "0.1.0+0123456" or meta["is_tag"] != "false" or meta["source_date_epoch"] != "1700000000":
        failures.append(f"ブランチのときの meta の値: {meta}")
    tag_meta = compute_meta("0.1.0", commit, "refs/tags/v0.1.0-rc1", 1, vencord)
    if tag_meta["zip_name"] != "cordmux-0.1.0-rc1" or tag_meta["label"] != "0.1.0-rc1":
        failures.append(f"-rc のタグのときの meta: {tag_meta}")
    if tag_meta["build_id"] != "0.1.0+0123456" or tag_meta["is_tag"] != "true":
        failures.append(f"-rc のタグのときのビルド ID: {tag_meta}")
    if compute_meta("0.1.0", commit, "refs/tags/v0.1.0", 1, vencord)["zip_name"] != "cordmux-0.1.0":
        failures.append("接尾辞のないタグのときの zip の名前が違う")
    # dist.yml の `release` のジョブが読む 2 つの値。is_tag は true か false の文字列。label はタグのときはタグから v を
    # 除いた文字列 (添付するファイルの名前と、make_release_notes.py の --label)、ほかのときは build_id。zip の名前は
    # 必ず cordmux-<label> で、添付する zip の名前 (dist_common.RELEASE_ZIP_FORMAT) と同じになる。
    plain_meta = compute_meta("0.1.0", commit, "refs/tags/v0.1.0", 1, vencord)
    for got, want in (
        ((meta["is_tag"], meta["label"]), ("false", "0.1.0+0123456")),
        ((tag_meta["is_tag"], tag_meta["label"]), ("true", "0.1.0-rc1")),
        ((plain_meta["is_tag"], plain_meta["label"]), ("true", "0.1.0")),
        ((compute_meta("0.1.0", commit, "refs/heads/v0.1.0", 1, vencord)["is_tag"],), ("false",)),
    ):
        if got != want:
            failures.append(f"is_tag と label: {want} のはずが {got}")
    for each in (meta, tag_meta, plain_meta):
        if dist_common.RELEASE_ZIP_FORMAT.format(label=each["label"]) != each["zip_name"] + ".zip":
            failures.append(f"zip の名前 {each['zip_name']} が、label {each['label']} の添付の名前と違う")
    expect_error("タグと version が違う", compute_meta, "0.1.0", commit, "refs/tags/v0.2.0", 1, vencord)
    expect_error("v のないタグ", compute_meta, "0.1.0", commit, "refs/tags/0.1.0", 1, vencord)
    expect_error("短いコミットを渡した", compute_meta, "0.1.0", "0123456", "", 1, vencord)
    expect_error("数字が 3 つでない version", compute_meta, "0.1", commit, "", 1, vencord)

    source = (
        "/*\n * comment mentions export const CORDMUX_BUILD_ID = \"x\"; inside\n */\n"
        'export const CORDMUX_BUILD_ID = "0.1.0+dev";\n'
    )
    rewritten = rewrite_version_ts(source, "0.1.0+abc1234")
    if 'export const CORDMUX_BUILD_ID = "0.1.0+abc1234";\n' not in rewritten or rewritten.count("CORDMUX_BUILD_ID") != 2:
        failures.append(f"version.ts の書き換え: {rewritten!r}")
    if not rewritten.startswith("/*\n * comment mentions"):
        failures.append("version.ts の書き換えで、ほかの行が変わった")
    expect_error("version.ts の行が 2 つ", rewrite_version_ts, source + 'export const CORDMUX_BUILD_ID = "1";\n', "0.1.0+a")
    expect_error("version.ts に行がない", rewrite_version_ts, "export const OTHER = 1;\n", "0.1.0+a")
    expect_error("version.ts に書くビルド ID の形が違う", rewrite_version_ts, source, "0.1.0-dev")

    # 実際の src-vencord/version.ts も、書き換えられる形であることを確認する。
    real = (dist_common.REPO_ROOT / "src-vencord" / "version.ts").read_text(encoding="utf-8")
    try:
        rewrite_version_ts(real, "0.1.0+abc1234")
    except DistError as exc:
        failures.append(f"src-vencord/version.ts を書き換えられない: {exc}")

    with tempfile.TemporaryDirectory(prefix="cordmux-build-info-") as tmp:
        # obs-deps の bin フォルダーは buildspec.json の 2 つのバージョンから作る (バージョンをここに書かない)。
        spec = Path(tmp) / "buildspec.json"
        spec.write_text(
            '{"version": "9.8.7", "dependencies": {"obs-studio": {"version": "30.0.0"},'
            ' "prebuilt": {"version": "2000-01-02", "hashes": {"windows-x64": "ab"}}}}',
            encoding="utf-8",
        )
        bin_dir = dist_common.obs_deps_bin_dir(Path(tmp), spec)
        if bin_dir.as_posix() != Path(tmp).as_posix() + "/src-obs/.deps/obs-studio-30.0.0/.deps/obs-deps-2000-01-02-x64/bin":
            failures.append(f"obs-deps の bin: {bin_dir}")

    if failures:
        for failure in failures:
            print(f"selftest 失敗: {failure}", file=sys.stderr)
        return 1
    print("selftest OK: build_info (meta、タグの確認、version.ts の書き換え、obs-deps の bin)")
    return 0


def main(argv: Optional[list[str]] = None) -> int:
    dist_common.use_utf8_console()
    parser = argparse.ArgumentParser(description="配布のワークフローが使う、ビルド ID とほかの値を出力する")
    parser.add_argument("--selftest", action="store_true", help="このツールのセルフテストを実行する")
    sub = parser.add_subparsers(dest="command")

    meta = sub.add_parser("meta", help="ビルド ID、zip の名前、ソースの日時などを key=value の行で出力する")
    meta.add_argument("--sha", help="40 桁のコミット (既定: HEAD)")
    meta.add_argument("--ref", help="github.ref。refs/tags/v... なら buildspec.json の version と照合する")
    meta.set_defaults(func=command_meta)

    deps = sub.add_parser("obs-deps-bin", help="buildspec.json から作った obs-deps の bin フォルダーのパスを出力する")
    deps.set_defaults(func=command_obs_deps_bin)

    version_ts = sub.add_parser("version-ts", help="version.ts の CORDMUX_BUILD_ID の行を書き換える")
    version_ts.add_argument("--build-id", required=True)
    version_ts.add_argument("--file", required=True)
    version_ts.set_defaults(func=command_version_ts)

    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    if not args.command:
        parser.error("--selftest を付けないときは、コマンドを指定する")
    try:
        return args.func(args)
    except DistError as exc:
        print(f"エラー: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
