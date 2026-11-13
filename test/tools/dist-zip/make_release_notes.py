#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# リリースノート (日本語と英語) を作る。使うものは、CHANGELOG.md の label のバージョンの節、手順書 (guide/*.md) の
# 互換性の節と更新の見出し、--assets-dir の cordmux-<label>.zip の中の手順書、cordmux-build.txt、ソースコードの入手先.txt。
# 添付するファイルが 3 つだけで、ソースをまとめた zip に、ソースコードの入手先.txt の行のソースがそろっていることも確かめる。
# 文の形と日英の決まった文は
# release_notes_template.md にあり、ここで {{キー}} と {{#フラグ}}...{{/フラグ}} を埋める。並びは、このバージョンの
# 変更点、互換性、添付するファイルの順。問題が 1 つでもあれば文を書かない (問題になるものは
# compose のコメントにある)。結果は 1 行の JSON (ok、out、title、prerelease、problems) で、`release` のジョブは
# この title と prerelease を使う。CI (.github/workflows/dist.yml のタグのとき) もローカルも同じ引数で実行する。

import argparse
import json
import posixpath
import re
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path
from typing import Optional

TOOL_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOL_DIR))
import build_info  # noqa: E402  (タグの形を build_info.TAG_RE の 1 か所に保つ)
import dist_common  # noqa: E402  (ツールが共有する関数と定数)
from dist_common import DistError  # noqa: E402
import fetch_sources  # noqa: E402  (上流のアーカイブの拡張子の一覧 ARCHIVE_SUFFIXES と、コンポーネントの行の読み方)

sys.path.insert(0, str(dist_common.REPO_ROOT / "test" / "lint"))
import install_guide  # noqa: E402  (手順書の節を rules の role で探す関数)
import forbidden_ui_words  # noqa: E402  (spacing のセット)

TEMPLATE_PATH = TOOL_DIR / "release_notes_template.md"
CHANGELOG_PATH = dist_common.REPO_ROOT / "CHANGELOG.md"
VENCORD_LOCK_REPO_PATH = "src-vencord/vencord.lock.json"
# zip の中の cordmux-build.txt のパス (最上位のフォルダーの下。make_dist_zip.py と check_dist_zip.py と同じ)。
BUILD_MARK_IN_ZIP = "cordmux/data/vencord/" + dist_common.build_mark_names()["file_name"]
INTERNAL_NUMBER_RE = re.compile(r"\bAC-[0-9]+\b")
FENCE = "```"
MAX_BLANK_RUN_RE = re.compile(r"\n{3,}")
# テンプレートの条件のブロック {{#フラグ}}...{{/フラグ}} (フラグが真のときだけ中身を残す)。使えるフラグは compose が渡す。
FLAG_BLOCK_RE = re.compile(r"\{\{#([a-z_]+)\}\}(.*?)\{\{/\1\}\}", re.S)

# ---- 変更履歴の書き方 ----
# CHANGELOG.md の見出しの文字列と、報告先の語はここに持つ。見出しはリリースノートに差し込む値ではなく、変更履歴を
# 読む側の約束であるため。書き方は docs/release.md の「変更履歴の書き方」にある。
# 言語ごとの 3 つの見出し (この順に並べる)。
CHANGE_HEADINGS = {
    "ja": ("新機能", "変更点", "不具合の修正"),
    "en": ("New Features", "Changes", "Bug Fixes"),
}
JA_PERIOD = "。"
VERSION_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
VERSION_HEADING_RE = re.compile(r"^## (?!#)(.*?)\s*$")
LANG_HEADING_RE = re.compile(r"^### (?!#)(.*?)\s*$")
KIND_HEADING_RE = re.compile(r"^#### (?!#)(.*?)\s*$")
ITEM_RE = re.compile(r"^- (\S.*?)\s*$")
# 英語の項目の先頭の、過去形の動詞 (OBS の変更履歴と同じ書き方。Added、Changed、Fixed、Removed、Updated、Renamed など)。
# -ed で終わる語と、よく使う不規則な過去形。
EN_ITEM_START_RE = re.compile(r"^(?:[A-Z][a-z]+ed|Made|Kept|Left|Reset|Set|Split|Built|Rebuilt|Rewrote|Gave|Let|Put|Hid)\b")
EN_PERSON_RE = re.compile(r"\b(?:[Ww]e|[Oo]ur|[Uu]s)\b")
# 変更履歴の項目に書かない、内部の番号とコミット。項目の全体をチェックする。
CHANGELOG_INTERNAL_PATTERNS = [
    # 作業の中だけで通じる番号の形 (英大文字 1 字か 2 字と、ハイフンと、数字)
    (r"internal id", re.compile(r"\b[A-Z]{1,2}[0-9]{0,2}-[A-Z]?[0-9]+\b", re.ASCII)),
    # リポジトリにない文書の名前と行番号の形 (2 桁の数字とハイフンで始まる名前、2 桁の数字とコロンと行番号)
    (r"internal doc", re.compile(r"\b0[0-9]-[A-Z][A-Z-]*\b|\b0[0-9]:[0-9]{3,4}\b", re.ASCII)),
    # 作業の中だけで通じる語 (このファイル自身が検出されないよう、語を分けて書く)
    (r"internal word", re.compile("決めた" + "こと|" + "節" + "目")),
    # コミットの形 (英字と数字の両方を含む 7 桁以上の 16 進数)
    (r"commit", re.compile(r"\b(?=[0-9a-f]*[a-f])(?=[0-9a-f]*[0-9])[0-9a-f]{7,40}\b", re.ASCII)),
]
# 関数と識別子の形。前の画面の語やファイルの名前を引用した所 ([ ]、「」、""、`) の外だけを見る。
CHANGELOG_CODE_PATTERNS = [
    (r"function call", re.compile(r"\b[A-Za-z_][A-Za-z0-9_]*\(\)", re.ASCII)),
    (r"snake_case identifier", re.compile(r"\b[a-z][a-z0-9]*_[a-z0-9_]+\b", re.ASCII)),
]
# 不具合の報告先 (GitHub の Issues や連絡先) を示す語。作ったリリースノートの全体と、変更履歴の項目をチェックする。
# 手順書と画面にも報告先を書かないので、リリースノートだけに報告先があると食い違うため。手順書の rules の
# forbidden_all とは別に持つのは、rules の英語の \bissues?\b が、変更履歴の "Fixed an issue where" に一致するため。
REPORT_TO_PATTERNS = [
    (r"GitHub Issues", re.compile(r"\bIssues\b|github\.com/[^\s)]*/issues", re.ASCII)),
    (r"issue tracker", re.compile(r"\bissue\s+tracker\b", re.ASCII | re.IGNORECASE)),
    (r"contact", re.compile(r"\bcontact\s+(?:us|me|the\s+author)\b", re.ASCII | re.IGNORECASE)),
    (r"inquiry (ja)", re.compile("問い合わせ")),
]
# 前の画面の語や名前を引用する所 (spacing のセットと識別子のチェックの前に、中を伏せる)。
QUOTED_SPAN_RE = re.compile(r"\[[^\[\]\n]*\]|「[^「」\n]*」|\"[^\"\n]*\"|`[^`\n]*`")


def label_ok(label: str) -> bool:
    # タグから v を除いた文字列か (build_info.TAG_RE に v を付けて確認する)。
    return bool(build_info.TAG_RE.match("v" + label))


def is_prerelease(label: str) -> bool:
    # label に - の接尾辞があればプレリリースにする。-rc のほかの接尾辞でもプレリリースにし、接尾辞のある label を
    # 正式版として公開しない。判定はこの 1 か所に置き、`release` のジョブは結果の JSON の prerelease を使う (規則を
    # YAML に書かない)。
    match = build_info.TAG_RE.match("v" + label)
    if not match:
        raise ValueError(f"リリースの label でない: {label!r}")
    return match.group(2) is not None


def version_of(label: str) -> str:
    # label の x.y.z (変更履歴の節の名前)。-rc も正式版も、接尾辞を除いた同じ節を使う。-rc はそのバージョンの候補で、
    # 変更点は正式版と同じであるため。label ごとに節を作ると、正式版を出すときに -rc の節を移す手間がかかり、同じ
    # 項目を 2 か所に持つことになる。
    match = build_info.TAG_RE.match("v" + label)
    if not match:
        raise ValueError(f"リリースの label でない: {label!r}")
    return match.group(1)


def title_for(label: str) -> str:
    # リリースのタイトル。タグの名前をそのまま使う。
    return f"cordmux v{label}"


def zip_path_for(assets_dir: Path, label: str) -> Path:
    # 文を作るのに使う zip は、添付する zip そのもの (--assets-dir と --label から名前の規則で決め、添付と違う zip から
    # 文を作らない)。
    return assets_dir / dist_common.RELEASE_ZIP_FORMAT.format(label=label)


def pack_pdb(pdb: Path, assets_dir: Path, label: str) -> Path:
    # cordmux.pdb を、名前を変えずに最上位に 1 つだけ入れた zip を作る。デバッガーは DLL の中に書かれた名前で
    # pdb を探すため。添付するファイルの名前の規則を YAML に書かないよう、pdb の zip もこのツールで作る。
    if pdb.name != dist_common.RELEASE_PDB_MEMBER:
        raise DistError(f"--pdb のファイル名は {dist_common.RELEASE_PDB_MEMBER} にする (デバッガーは名前で探す): {pdb.name}")
    if not pdb.is_file():
        raise DistError(f"--pdb の {pdb} がない")
    target = assets_dir / dist_common.RELEASE_PDB_ZIP_FORMAT.format(label=label)
    if target.exists():
        raise DistError(f"{assets_dir} に {target.name} が既にある")
    with zipfile.ZipFile(target, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.write(pdb, dist_common.RELEASE_PDB_MEMBER)
    return target


# ---- 前のリリース (Vencord の tag を比べる相手) -------------------------------------------


def _suffix_key(suffix: str) -> tuple:
    # 接尾辞の比べ方 (rc2 < rc10)。数字の並びは数として、ほかは文字列として比べる。
    return tuple((0, int(part), "") if part.isdigit() else (1, 0, part) for part in re.findall(r"[0-9]+|[^0-9]+", suffix))


def previous_release_tag(label: str, tags: list[str]) -> Optional[str]:
    # label の x.y.z より小さい x.y.z のタグのうち、最も新しいもの (同じ x.y.z なら正式版、次に接尾辞の新しいもの)。
    # 同じ x.y.z の -rc は前のリリースに数えない。変更履歴の節は同じ x.y.z の変更を全部持つので、比べる相手も
    # 前の x.y.z にするため。
    current = tuple(int(part) for part in version_of(label).split("."))
    best = None
    for tag in tags:
        match = build_info.TAG_RE.match(tag)
        if not match:
            continue
        version = tuple(int(part) for part in match.group(1).split("."))
        if version >= current:
            continue
        suffix = match.group(2)
        key = (version, suffix is None, _suffix_key(suffix or ""))
        if best is None or key > best[0]:
            best = (key, tag)
    return best[1] if best else None


def git_text(*args: str) -> str:
    # git の出力 (UTF-8)。失敗すれば DistError。build_info.git_output はコンソールの文字コードで読むので、
    # 日本語を含む vencord.lock.json の reason を読めないため、こちらを使う。
    completed = subprocess.run(["git", *args], cwd=str(dist_common.REPO_ROOT), capture_output=True)
    if completed.returncode != 0:
        raise DistError(f"git {' '.join(args)} が失敗した: {completed.stderr.decode('utf-8', 'replace').strip()}")
    return completed.stdout.decode("utf-8")


def previous_lock_for(label: str, require_own_tag: bool) -> Optional[dict]:
    # 前のリリースのタグの src-vencord/vencord.lock.json (前のリリースがなければ None)。タグは git のタグから読む
    # (`release` のジョブは fetch-depth: 0 で checkout し、タグも取得する)。require_own_tag が真 (タグのとき) は、
    # v<label> のタグが見えることも確認する。タグを取得していないと前のリリースが見つからず、Vencord のチェックが
    # 気付かれないまま動作しなくなるため。
    tags = git_text("tag", "--list", "v*").split()
    if require_own_tag and f"v{label}" not in tags:
        raise DistError(f"タグ v{label} が checkout にない (タグを取得しないと、Vencord のチェックが前のリリースを見つけられない)")
    previous = previous_release_tag(label, tags)
    if previous is None:
        return None
    try:
        return json.loads(git_text("show", f"{previous}:{VENCORD_LOCK_REPO_PATH}"))
    except json.JSONDecodeError as exc:
        raise DistError(f"{previous}:{VENCORD_LOCK_REPO_PATH}: {exc}") from exc


# ---- 変更履歴 -------------------------------------------------------------------------


class ChangelogSection:
    # 1 つのバージョンの節。langs は 言語 -> [(見出し, [項目])]。
    def __init__(self, version: str, line_no: int):
        self.version = version
        self.line_no = line_no
        self.langs: dict = {}

    def items(self, lang: str) -> list[str]:
        return [item for _, items in self.langs.get(lang, []) for item in items]


def parse_changelog(text: str, name: str = "CHANGELOG.md") -> tuple[dict, list[str]]:
    # 変更履歴を読み、(バージョン -> ChangelogSection, 形の問題の文の並び) を返す。最初の ## より前 (タイトル) は読まない。
    # 形は次のとおり。
    #   ## <x.y.z>           バージョンの節。新しい順に並べる。v も -rc も付けない
    #   ### ja / ### en      言語の節。両方が必要 (dist_common.FILL_LANGS)
    #   #### <見出し>        CHANGE_HEADINGS の 3 つだけを、この順に書く。項目のない見出しは書かない
    #   - <項目>             1 行に 1 つ (続きの行を書かない)
    sections: dict = {}
    problems: list[str] = []
    order: list[tuple[int, ...]] = []
    section: Optional[ChangelogSection] = None
    lang: Optional[str] = None
    kind_list: Optional[list] = None

    def close_section() -> None:
        if section is None:
            return
        for want in dist_common.FILL_LANGS:
            if want not in section.langs:
                problems.append(f"{name}:{section.line_no}: {section.version} に '### {want}' の節がない")
            elif not section.langs[want]:
                problems.append(f"{name}:{section.line_no}: {section.version} の '### {want}' に見出しがない")
            else:
                for heading, items in section.langs[want]:
                    if not items:
                        problems.append(f"{name}:{section.line_no}: {section.version} の {want} の見出し '{heading}' が空")

    for line_no, line in enumerate(text.split("\n"), start=1):
        if not line.strip():
            continue
        match = VERSION_HEADING_RE.match(line)
        if match:
            close_section()
            version = match.group(1)
            section, lang, kind_list = ChangelogSection(version, line_no), None, None
            if not VERSION_RE.match(version):
                problems.append(f"{name}:{line_no}: バージョンの見出し '{version}' が x.y.z の形でない (v も -rc も付けない)")
                continue
            if version in sections:
                problems.append(f"{name}:{line_no}: バージョン {version} の節が 2 つある")
                continue
            sections[version] = section
            numbers = tuple(int(part) for part in version.split("."))
            if order and numbers >= order[-1]:
                problems.append(f"{name}:{line_no}: バージョン {version} が上の節より古くない (新しい順に並べる)")
            order.append(numbers)
            continue
        if section is None:
            continue  # タイトルと前書き
        match = LANG_HEADING_RE.match(line)
        if match:
            lang, kind_list = match.group(1), None
            if lang not in CHANGE_HEADINGS:
                problems.append(f"{name}:{line_no}: 知らない言語の見出し '### {lang}' (使える言語: {', '.join(CHANGE_HEADINGS)})")
                lang = None
            elif lang in section.langs:
                problems.append(f"{name}:{line_no}: {section.version} に '### {lang}' が 2 つある")
                lang = None
            else:
                section.langs[lang] = []
            continue
        match = KIND_HEADING_RE.match(line)
        if match:
            kind_list = None
            heading = match.group(1)
            if lang is None:
                problems.append(f"{name}:{line_no}: 見出し '{heading}' が言語の節の外にある")
                continue
            known = CHANGE_HEADINGS[lang]
            if heading not in known:
                problems.append(f"{name}:{line_no}: 知らない見出し '{heading}' ({lang} で使える見出し: {', '.join(known)})")
                continue
            used = [h for h, _ in section.langs[lang]]
            if heading in used:
                problems.append(f"{name}:{line_no}: {section.version} の {lang} に見出し '{heading}' が 2 つある")
                continue
            if used and known.index(heading) < known.index(used[-1]):
                problems.append(f"{name}:{line_no}: 見出し '{heading}' の順が違う ({', '.join(known)} の順に並べる)")
            kind_list = []
            section.langs[lang].append((heading, kind_list))
            continue
        match = ITEM_RE.match(line)
        if match:
            if kind_list is None:
                problems.append(f"{name}:{line_no}: 見出しの外に項目がある")
                continue
            kind_list.append(match.group(1))
            continue
        problems.append(f"{name}:{line_no}: 見出しでも、1 行の「- 」の項目でもない: {line.strip()[:60]!r}")
    close_section()
    return sections, problems


def masked_quotes(text: str, fill: str = " ") -> str:
    # [ ]、「」、""、` の中を同じ数の fill に置き換える (括弧は残す)。
    return QUOTED_SPAN_RE.sub(lambda m: m.group(0)[0] + fill * (len(m.group(0)) - 2) + m.group(0)[-1], text)


# spacing のセットのチェックの前に、伏せた中を埋める文字。空白で埋めると、spacing のセットの「括弧の内側の空白」の行が伏せた跡に
# 一致してしまう (手順書の lint の SPACING_MASK_CHAR と同じ文字、同じ理由)。識別子のチェック (CHANGELOG_CODE_PATTERNS) は
# 空白で伏せた文字列を見る (_ で埋めると snake_case の形に見えるため)。
SPACING_MASK_CHAR = "_"


def spacing_rules(lang: str) -> list:
    # forbidden_ui_words.txt の spacing のセットの行のうち、言語と ja_style.txt の書き方で使う行 (手順書の lint と同じ選び方)。
    style = forbidden_ui_words.load_ja_style()
    return [r for r in forbidden_ui_words.load_word_rules()
            if r.group == forbidden_ui_words.SPACING_GROUP and forbidden_ui_words.rule_applies(r, lang, style)]


def item_problems(section: ChangelogSection, name: str = "CHANGELOG.md") -> list[str]:
    # 1 つのバージョンの節の項目が、決まった文の形か、内部の語と報告先の語を含まないかを確認する。日本語の項目は「。」で
    # 終える。英語の項目は過去形の動詞で始め、末尾に「.」を付けず、we、our、us を使わない。前の画面の語を引用する
    # ため、forbidden_ui_words.txt の画面の文字列のセット (symbols、notation) はチェックしない。
    # spacing のセットは、引用の中を伏せてからチェックする (引用した所は、元の画面の書き方のままにするため)。
    problems: list[str] = []
    for lang in dist_common.FILL_LANGS:
        spacing = spacing_rules(lang)
        for item in section.items(lang):
            where = f"{name} {section.version} {lang} の項目 {item[:40]!r}"
            if lang == "ja" and not item.endswith(JA_PERIOD):
                problems.append(f"{where}: 日本語の項目が「。」で終わっていない")
            if lang == "en":
                if not EN_ITEM_START_RE.match(item):
                    problems.append(f"{where}: 英語の項目が過去形の動詞 (Added、Changed、Fixed など) で始まっていない")
                if item.endswith("."):
                    problems.append(f"{where}: 英語の項目の末尾に「.」がある")
                if EN_PERSON_RE.search(item):
                    problems.append(f"{where}: 英語の項目に we、our、us がある")
            for source, pattern in CHANGELOG_INTERNAL_PATTERNS:
                if pattern.search(item):
                    problems.append(f"{where}: 内部の語か番号がある ({source})")
            for source, pattern in REPORT_TO_PATTERNS:
                if pattern.search(item):
                    problems.append(f"{where}: 不具合の報告先がある ({source})")
            outside = masked_quotes(item)
            for source, pattern in CHANGELOG_CODE_PATTERNS:
                if pattern.search(outside):
                    problems.append(f"{where}: コードの名前がある ({source})")
            spacing_outside = masked_quotes(item, SPACING_MASK_CHAR)
            for rule in spacing:
                if rule.regex.search(spacing_outside):
                    problems.append(f"{where}: 空白の書き方が違う ({rule.raw})")
    return problems


def changes_markdown(section: ChangelogSection, lang: str) -> str:
    # リリースノートの「このバージョンの変更点」の中身 (#### の見出しと項目)。
    blocks = []
    for heading, items in section.langs.get(lang, []):
        blocks.append(f"#### {heading}\n\n" + "\n".join(f"- {item}" for item in items))
    return "\n\n".join(blocks)


def vencord_item_problems(section: ChangelogSection, lock: dict, previous_lock: Optional[dict]) -> list[str]:
    # 同梱の Vencord の tag が前のリリースと違えば、その節の ja と en の両方に、Vencord と新しい tag を書いた項目が
    # 必要 (更新した理由は変更履歴の項目に書く)。前のリリースがなければ (最初のリリース) チェックしない。
    if previous_lock is None:
        return []
    tag, before = lock.get("tag", ""), previous_lock.get("tag", "")
    if not tag or tag == before:
        return []
    problems = []
    for lang in dist_common.FILL_LANGS:
        if not any("Vencord" in item and tag in item for item in section.items(lang)):
            problems.append(f"CHANGELOG.md {section.version} {lang}: 同梱の Vencord が {before} から {tag} に変わったのに、"
                            f"Vencord と {tag} を書いた項目がない")
    return problems


# ---- 文を作るのに使うもの ----------------------------------------------------------------


class Materials:
    # 文を作るのに使う、リポジトリの中の約束 (実際の実行ではリポジトリのファイルから、セルフテストではテスト用の
    # データから作る)。

    def __init__(self, blocks: list, guide_texts: dict, offer_name: str, template: str, lock: dict, changelog: str,
                 previous_lock: Optional[dict] = None):
        self.blocks = blocks                # install_guide の組の並び
        self.guide_texts = guide_texts      # 言語 -> 手順書の Markdown (rules の target。ないものは入れない)
        self.offer_name = offer_name        # ソースコードの入手先.txt の名前 (source-offer の行)
        self.template = template            # release_notes_template.md の中身
        self.lock = lock                    # vencord.lock.json の中身
        self.changelog = changelog          # CHANGELOG.md の中身
        self.previous_lock = previous_lock  # 前のリリースの vencord.lock.json (なければ None)


def real_materials() -> Materials:
    entries, _ = dist_common.parse_top_level(dist_common.TOP_LEVEL_FILES.read_text(encoding="utf-8"))
    offers = [name for kind, name in entries if kind == "source-offer"]
    if len(offers) != 1:
        raise DistError(f"top_level_files.txt: source-offer の行は 1 つのはずが {len(offers)} 行ある")
    if not CHANGELOG_PATH.is_file():
        raise DistError(f"リポジトリのルートに {CHANGELOG_PATH.name} がない")
    blocks = install_guide.load_rules()
    guide_texts = {}
    for block in blocks:
        path = dist_common.REPO_ROOT / block.target
        if path.is_file():
            guide_texts[install_guide.guide_lang(block)] = path.read_bytes().decode("utf-8")
    return Materials(
        blocks,
        guide_texts,
        offers[0],
        TEMPLATE_PATH.read_text(encoding="utf-8"),
        dist_common.load_json(dist_common.VENCORD_LOCK_PATH),
        CHANGELOG_PATH.read_text(encoding="utf-8"),
    )


def guide_blocks_by_lang(materials: Materials, problems: list[str]) -> dict:
    # 言語 -> (zip の中の手順書の名前, rules の組)。ja と en で 1 つずつ。言語は rules の組の ini の名前で決める。
    found: dict = {}
    for block in materials.blocks:
        name = block.zip_name
        lang = install_guide.guide_lang(block)
        if lang not in dist_common.FILL_LANGS:
            problems.append(f"rules の target {block.target}: ini の名前 {block.ini} から言語 (ja か en) が決まらない")
            continue
        if lang in found:
            problems.append(f"{lang} の手順書が 2 つある: {found[lang][0]} と {name}")
            continue
        found[lang] = (name, block)
    for lang in dist_common.FILL_LANGS:
        if lang not in found:
            problems.append(f"{lang} の手順書がない (リリースノートには両方の言語が必要)")
    return found


def read_zip(path: Path) -> dict[str, bytes]:
    with zipfile.ZipFile(path) as archive:
        return {info.filename: archive.read(info) for info in archive.infolist() if not info.filename.endswith("/")}


def section_text(sections: dict, heading: str, guide: str, role: str, block, problems: list[str]) -> str:
    # role の節の本文 (前後の空行を除く)。ない、空、placeholder がある、{{ がある、``` があるときは問題にする。
    # 節は -rc のときでもリリースノートにそのまま載るので、placeholder は -rc か正式版かによらず問題にする。
    body = sections.get(heading)
    if body is None:
        problems.append(f"{guide}: {role} の節 ({heading}) がない")
        return ""
    body = body.strip("\n")
    if not body.strip():
        problems.append(f"{guide}: {role} の節が空")
    if block.placeholder and block.placeholder in body:
        problems.append(f"{guide}: {role} の節に placeholder が残っている")
    if dist_common.FILL_OPEN in body:
        problems.append(f"{guide}: {role} の節に差し込んでいない {dist_common.FILL_OPEN} がある")
    if FENCE in body:
        problems.append(f"{guide}: {role} の節に {FENCE} がある")
    return body


def classify_assets(assets_dir: Path, label: str, problems: list[str]) -> dict:
    # 添付するファイルが 3 つ (配布用の zip、pdb の zip、ソースをまとめた zip) だけかを確認し、種類 -> 名前 を返す。
    # 足りないもの、ほかのファイル、フォルダーは問題にする。添付を 3 つに限るのは、リリースのページで、どれを
    # ダウンロードすればよいかを名前を全部見て確かめなくて済むようにするため。
    names = {
        "zip": dist_common.RELEASE_ZIP_FORMAT.format(label=label),
        "pdb": dist_common.RELEASE_PDB_ZIP_FORMAT.format(label=label),
        "sources": dist_common.RELEASE_SOURCES_ZIP_FORMAT.format(label=label),
    }
    for path in sorted(assets_dir.iterdir()):
        if path.is_dir():
            problems.append(f"添付: フォルダー {path.name} がある (添付できるのはファイルだけ)")
        elif path.name not in names.values():
            problems.append(f"添付: {path.name} は添付するファイルでない (添付するのは {', '.join(names.values())} の 3 つだけ)")
    for name in names.values():
        if not (assets_dir / name).is_file():
            problems.append(f"添付: {name} がない")
    pdb_name = names["pdb"]
    pdb_path = assets_dir / pdb_name
    if pdb_path.is_file():
        try:
            with zipfile.ZipFile(pdb_path) as archive:
                members = archive.namelist()
            if members != [dist_common.RELEASE_PDB_MEMBER]:
                problems.append(f"添付: {pdb_name} の中身は [{dist_common.RELEASE_PDB_MEMBER}] のはずが {members}")
        except zipfile.BadZipFile as exc:
            problems.append(f"添付: {pdb_name}: {exc}")
    return names


def sources_zip_problems(path: Path, offer_text: str) -> list[str]:
    # ソースをまとめた zip (fetch_sources.py が作る) に、配布用の zip のソースコードの入手先.txt のコンポーネントの行の
    # ソースが全部あるかを確認し、問題の文の並びを返す。zip の中は、最上位のフォルダー (zip の名前から .zip を除いたもの)
    # の下に、次のものだけがあるはず。
    #   - 専用のアーカイブを入れない行 (dist_common.SOURCE_PARTS_WITHOUT_TARBALL) のほかの行ごとに、アーカイブが 1 つ
    #     (名前は dist_common.RELEASE_SOURCE_MEMBER_FORMAT。{part} は行の最初の語、{ext} は上流のアーカイブの拡張子)
    #   - 一覧 (dist_common.RELEASE_SOURCE_LIST)。ソースコードの入手先.txt のコンポーネントの行が、全部同じ文字列である
    name = path.name
    top = path.name[: -len(".zip")] + "/"
    list_name = top + dist_common.RELEASE_SOURCE_LIST
    try:
        with zipfile.ZipFile(path) as archive:
            names = archive.namelist()
            listing = archive.read(list_name).decode("utf-8") if list_name in names else None
    except (OSError, zipfile.BadZipFile, UnicodeDecodeError) as exc:
        return [f"添付: {name}: {exc}"]
    problems: list[str] = []
    outside = [n for n in names if not n.startswith(top)]
    if outside:
        problems.append(f"添付: {name} の最上位のフォルダー {top} の外に項目がある: {outside[:3]}")
    members = [n[len(top):] for n in names if n.startswith(top)]
    entries = fetch_sources.offer_entries(offer_text)
    if not entries:
        problems.append("配布用の zip のソースコードの入手先.txt にコンポーネントの行がない")
    matched: set[str] = set()
    for part, _ in entries:
        if part in dist_common.SOURCE_PARTS_WITHOUT_TARBALL:
            continue
        slug = dist_common.source_part_slug(part)
        wanted = {dist_common.RELEASE_SOURCE_MEMBER_FORMAT.format(part=slug, ext=suffix[1:])
                  for suffix in fetch_sources.ARCHIVE_SUFFIXES}
        found = [member for member in members if member in wanted]
        if not found:
            problems.append(f"添付: {name} に {part} の行のソースのアーカイブ ({slug}.<拡張子>) がない")
        elif len(found) > 1:
            problems.append(f"添付: {name} に {part} の行のソースのアーカイブが {len(found)} つある: {found}")
        matched.update(found)
    extra = [member for member in members if member not in matched and member != dist_common.RELEASE_SOURCE_LIST]
    if extra:
        problems.append(f"添付: {name} に、ソースコードの入手先.txt のどの行にも当たらない項目がある: {extra}")
    if listing is None:
        problems.append(f"添付: {name} に {dist_common.RELEASE_SOURCE_LIST} がない")
    else:
        listed = set(listing.split("\n"))
        missing = [part for part, line in entries if line not in listed]
        if missing:
            problems.append(f"添付: {name} の {dist_common.RELEASE_SOURCE_LIST} に、ソースコードの入手先.txt の行がない: {missing}")
    return problems


def internal_word_problems(text: str) -> list[str]:
    # 作った文に、内部の番号 (AC-<数字> の形。手順書の lint と同じ) と報告先の語がないかを確認する。
    problems = [f"リリースノートに内部の番号 {m} がある" for m in sorted(set(INTERNAL_NUMBER_RE.findall(text)))]
    for raw, pattern in REPORT_TO_PATTERNS:
        if pattern.search(text):
            problems.append(f"リリースノートに不具合の報告先 ({raw}) がある")
    return problems


def notes_markdown(body: str, label: str, target: str) -> str:
    # 手順書 (Markdown) の節の本文を、リリースノートに載せる形にする。リリースノートも Markdown なので、本文はそのまま
    # 載せる。手順書の中のリンクは、リリースのページからは辿れないので、このバージョンのタグの GitHub の URL にする
    # (同じファイルの見出しへのリンクは、手順書の URL にアンカーを付ける。相対リンクは、リポジトリの中のパスにする)。
    # ### の見出しは、テンプレートの ### の見出しの下に入るので、1 段下げて #### にする。
    def link(text: str, link_target: str) -> str:
        if link_target.startswith("https://"):
            return f"[{text}]({link_target})"
        if link_target.startswith("#"):
            return f"[{text}]({dist_common.guide_url_for(label, '', target)}{link_target})"
        path = posixpath.normpath(posixpath.join(posixpath.dirname(target), link_target))
        return f"[{text}]({dist_common.guide_url_for(label, '', path)})"

    lines = []
    for line in body.split("\n"):
        if line.startswith("### "):
            line = "#" + line
        lines.append(install_guide.render_inline(line, code=lambda content: f"`{content}`", link=link))
    return "\n".join(lines)


def heading_anchor(markdown: str, heading: str) -> str:
    # 手順書の ## の見出し heading の、GitHub のアンカー (見つからなければ空)
    doc = install_guide.parse_markdown(markdown)
    for anchor, line in doc.anchors.items():
        if line.kind == "section" and install_guide.plain_text(line.text) == heading:
            return anchor
    return ""


def apply_flags(template: str, flags: dict, problems: list[str]) -> str:
    # {{#フラグ}}...{{/フラグ}} を、フラグが真なら中身に、偽なら空にする。知らないフラグは問題にする (残したブロックは、
    # render で {{ の残りとしても問題になる)。
    def replace(match: re.Match) -> str:
        name = match.group(1)
        if name not in flags:
            problems.append(f"テンプレート: 知らないフラグ {{{{#{name}}}}} (使えるフラグ: {', '.join(sorted(flags))})")
            return match.group(0)
        return match.group(2) if flags[name] else ""
    return FLAG_BLOCK_RE.sub(replace, template)


def render(template: str, values: dict, problems: list[str]) -> str:
    # テンプレートの {{キー}} を埋める。知らないキーと、残った {{ は問題にする。
    unknown = sorted({key for key in dist_common.FILL_TOKEN_RE.findall(template) if key not in values})
    if unknown:
        problems.append(f"テンプレート: 知らないキー {unknown}")
    text = dist_common.FILL_TOKEN_RE.sub(lambda m: values.get(m.group(1), m.group(0)), template)
    if dist_common.FILL_OPEN in dist_common.FILL_TOKEN_RE.sub("", template):
        problems.append(f"テンプレート: 差し込みの書式でない {dist_common.FILL_OPEN} がある")
    return MAX_BLANK_RUN_RE.sub("\n\n", text).strip("\n") + "\n"


def compose(label: str, assets_dir: Path, materials: Materials, expect_commit: Optional[str] = None) -> tuple[str, list[str]]:
    # リリースノートを作り、(文, 問題の文の並び) を返す。問題が 1 つでもあれば、呼び出し元は文を使わない。
    # 問題にするもの:
    #   - 変更履歴: 形が違う (parse_changelog)、label のバージョンの節がない、項目の書き方が違う (item_problems)、
    #     同梱の Vencord を更新したのに項目がない (vencord_item_problems)
    #   - 手順書が ja と en で 1 つずつそろわない。zip に手順書がない。手順書の Markdown (guide/*.md) がない。互換性の
    #     節がない、空、placeholder や {{ がある (section_text)。更新の節がない
    #   - cordmux-build.txt の build_id が label のバージョンで始まらない、--expect-commit の短いコミットで終わらない
    #   - zip のソースコードの入手先.txt に、このバージョンのソースの URL (dist_common.source_url_for) がない
    #   - 添付するファイルが 3 つ (配布用の zip、pdb の zip、ソースをまとめた zip) でない (classify_assets)
    #   - ソースをまとめた zip に、zip のソースコードの入手先.txt の行のソースがそろっていない (sources_zip_problems)
    #   - テンプレートの知らないフラグとキー。作った文に内部の番号、報告先の語、{{ が残る
    #   - 正式版 (is_prerelease が偽) では、dist_common.formal_release_problems の問題も
    problems: list[str] = []
    prerelease = is_prerelease(label)
    version = version_of(label)

    changelog, changelog_problems = parse_changelog(materials.changelog)
    problems += changelog_problems
    section = changelog.get(version)
    if section is None:
        problems.append(f"CHANGELOG.md に '## {version}' の節がない (リリースノートはこのバージョンの変更点から始まる)")
    else:
        problems += item_problems(section)
        problems += vencord_item_problems(section, materials.lock, materials.previous_lock)

    assets = classify_assets(assets_dir, label, problems)
    guides = guide_blocks_by_lang(materials, problems)
    zip_path = zip_path_for(assets_dir, label)
    try:
        entries = read_zip(zip_path)
    except (OSError, zipfile.BadZipFile) as exc:
        problems.append(f"{zip_path.name}: {exc}")
        return "", problems
    top = zip_path.stem + "/"

    mark_names = dist_common.build_mark_names()
    raw_mark = entries.get(top + BUILD_MARK_IN_ZIP)
    if raw_mark is None:
        problems.append(f"{zip_path.name}: {BUILD_MARK_IN_ZIP} がない")
    else:
        pairs, parse_problems = dist_common.parse_key_values(raw_mark.decode("utf-8"), allow_blank_lines=False)
        problems += [f"{mark_names['file_name']}: {p}" for p in parse_problems]
        build_id = dict(pairs).get(mark_names["key_build_id"], "")
        if not build_id.startswith(version + "+"):
            problems.append(f"{mark_names['file_name']} の build_id {build_id!r} が label のバージョン {version!r} で始まらない")
        if expect_commit and not build_id.endswith("+" + expect_commit[: build_info.SHORT_COMMIT_LENGTH]):
            problems.append(f"{mark_names['file_name']} の build_id {build_id!r} が --expect-commit {expect_commit} の短いコミットで終わらない")

    values = {
        "zip_asset": assets["zip"],
        "pdb_asset": assets["pdb"],
        "sources_asset": assets["sources"],
        "source_offer": materials.offer_name,
    }
    guide_texts: dict[str, str] = {}
    for lang in dist_common.FILL_LANGS:
        values[f"{lang}_changes"] = changes_markdown(section, lang) if section is not None else ""
        if lang not in guides:
            continue
        name, block = guides[lang]
        raw = entries.get(top + name)
        if raw is None:
            problems.append(f"{zip_path.name}: {name} がない")
            continue
        guide_texts[name] = raw.decode("utf-8")
        markdown = materials.guide_texts.get(lang)
        if markdown is None:
            problems.append(f"{block.target} がない (互換性の節と更新の見出しを取る手順書)")
            continue
        sections = install_guide.sections_by_heading(markdown)
        maintenance_heading = block.roles["maintenance"]
        update_heading = block.roles["update"]
        maintenance_body = section_text(sections, maintenance_heading, block.target, "maintenance", block, problems)
        if update_heading not in sections:
            problems.append(f"{block.target}: 更新の節 ({update_heading}) がない (今 cordmux を使っている人に、更新のしかたを案内する文に使う)")
        guide_url = dist_common.guide_url_for(label, "", block.target)
        values.update({
            f"{lang}_guide": name,
            f"{lang}_guide_url": guide_url,
            f"{lang}_maintenance_heading": maintenance_heading,
            f"{lang}_maintenance": notes_markdown(maintenance_body, label, block.target),
            f"{lang}_update_heading": update_heading,
            f"{lang}_update_url": guide_url + "#" + heading_anchor(markdown, update_heading),
        })

    # このバージョンのソースの URL は文に載せず、zip の中のソースコードの入手先.txt にあることだけを確認する。タグのときの
    # URL はリリースのページそのものなので、文に載せても同じページを指すだけになるため。
    offer = entries.get(top + materials.offer_name)
    if offer is None:
        problems.append(f"{zip_path.name}: {materials.offer_name} がない")
    else:
        try:
            url = dist_common.source_url_for(label, expect_commit or "")
            if url not in offer.decode("utf-8"):
                problems.append(f"{materials.offer_name} にソースの URL {url} がない")
        except DistError as exc:
            problems.append(str(exc))
        sources_path = assets_dir / assets["sources"]
        if sources_path.is_file():
            problems += sources_zip_problems(sources_path, offer.decode("utf-8"))

    if not prerelease:
        placeholders = [block.placeholder for block in materials.blocks]
        problems += dist_common.formal_release_problems(guide_texts, placeholders)

    template = apply_flags(materials.template, {"prerelease": prerelease}, problems)
    notes = render(template, values, problems)
    if dist_common.FILL_OPEN in notes:
        problems.append(f"リリースノートに {dist_common.FILL_OPEN} が残っている")
    problems += internal_word_problems(notes)
    return notes, problems


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="CHANGELOG.md と zip から、リリースノート (日本語と英語) を作る")
    parser.add_argument("--selftest", action="store_true", help="このツールのセルフテストを実行する")
    parser.add_argument("--label", help="タグから v を除いた文字列。例: 0.1.0、0.1.0-rc1")
    parser.add_argument("--assets-dir", help="リリースに添付するファイルを全部置いたフォルダー (cordmux-<label>.zip を含む)")
    parser.add_argument("--out", help="リリースノート (Markdown) を書くパス")
    parser.add_argument("--pdb", help="先に cordmux-<label>-pdb.zip にして --assets-dir に置く cordmux.pdb")
    parser.add_argument("--expect-commit", help="zip の cordmux-build.txt の build_id の短いコミットと一致するはずの 40 桁のコミット (タグのコミット)")
    return parser


def validate(args: argparse.Namespace) -> Optional[str]:
    # 引数を確認し、問題があれば理由の文を返す。
    if args.selftest:
        return None
    missing = [f"--{name.replace('_', '-')}" for name in ("label", "assets_dir", "out") if not getattr(args, name)]
    if missing:
        return "引数が足りない: " + ", ".join(missing)
    if not label_ok(args.label):
        return f"--label にはタグから v を除いた文字列 (0.1.0 か 0.1.0-rc1) を指定する: {args.label!r}"
    if args.expect_commit and not re.fullmatch(r"[0-9a-f]{40}", args.expect_commit):
        return f"--expect-commit は 40 桁の小文字の 16 進数で指定する: {args.expect_commit!r}"
    return None


def run(args: argparse.Namespace) -> dict:
    result = {"ok": False, "out": args.out, "title": title_for(args.label), "prerelease": is_prerelease(args.label)}
    assets_dir = Path(args.assets_dir)
    try:
        if args.pdb:
            pack_pdb(Path(args.pdb), assets_dir, args.label)
        materials = real_materials()
        materials.previous_lock = previous_lock_for(args.label, require_own_tag=bool(args.expect_commit))
        notes, problems = compose(args.label, assets_dir, materials, args.expect_commit)
    except (DistError, ValueError, OSError) as exc:
        result["problems"] = [str(exc)]
        return result
    result["problems"] = problems
    if not problems:
        dist_common.write_text_lf(Path(args.out), notes)
        result["ok"] = True
    return result


# ---- セルフテスト (テスト用の zip、rules、添付するファイル、変更履歴で実行する) ------------------------------------

LB = "【"
RB = "】"
SELFTEST_COMMIT = "0123456789abcdef0123456789abcdef01234567"
# 内部の番号の形のテストデータ。
SAMPLE_AC_ID = "AC-56"
SAMPLE_RULE_ID = "R-55"
SAMPLE_LETTER_RULE_ID = "V10-08"
JA_NEW, JA_CHANGES, JA_FIXES = CHANGE_HEADINGS["ja"]


def selftest_rules_text(langs: tuple = ("ja", "en")) -> str:
    # ja と en の 2 組 (見出しは ASCII のテスト用の文字列。形は実際の rules と同じ)。
    marks = {"ja": "(ja mark)", "en": "(to be written after testing)"}
    blocks = []
    for lang in langs:
        blocks.append("\n".join([
            f"target: sample/selftest-{lang}.txt",
            f"ini: {lang}-XX.ini",
            f"zip_name: guide-{lang}.txt",
            "title: T",
            f"fixture: test/lint/fixtures/selftest-{lang}",
            "heading: Intro",
            "heading: Steps",
            "heading: Update",
            "heading: Upkeep",
            f"procedure: {LB}Steps{RB} {LB}Steps{RB}",
            f"role: {LB}Upkeep{RB} maintenance",
            f"role: {LB}Update{RB} update",
            f"placeholder: {marks[lang]}",
        ]))
    return "\n\n".join(blocks) + "\n"


SELFTEST_TOP_LEVEL = "\n".join([
    "source-offer offer.txt",
    "required-dir licenses",
    "fill-values ja offer.txt",
    "",
])


def selftest_changelog(version: str = "0.1.0", ja: Optional[list] = None, en: Optional[list] = None,
                       older: str = "0.0.9") -> str:
    # テスト用の変更履歴 (新しい節と、形を見るだけの古い節)。ja と en は [(見出し, [項目])]。
    ja = ja if ja is not None else [(JA_NEW, ["Added x" + JA_PERIOD]), (JA_FIXES, ["Fixed y" + JA_PERIOD])]
    en = en if en is not None else [("New Features", ["Added x"]), ("Bug Fixes", ["Fixed an issue where y failed"])]
    lines = ["# Changelog", "", f"## {version}", ""]
    for lang, groups in (("ja", ja), ("en", en)):
        lines += [f"### {lang}", ""]
        for heading, items in groups:
            lines += [f"#### {heading}", ""] + [f"- {item}" for item in items] + [""]
    lines += [f"## {older}", "", "### ja", "", f"#### {JA_CHANGES}", "", "- Changed z" + JA_PERIOD, "",
              "### en", "", "#### Changes", "", "- Changed z", ""]
    return "\n".join(lines)


def selftest_guide(maintenance: str = "Sample maintenance text.", steps: str = "1. Do it.", update: bool = True) -> str:
    # テスト用の手順書 (Markdown)。zip の中にも同じ文字列を置く (zip の中の手順書は、存在と placeholder だけを見るため)。
    update_lines = ["## Update", "", "1. Replace it.", ""] if update else []
    return "\n".join([
        "# T", "", "## Intro", "", "Hello.", "", "## Steps", "", steps, "", *update_lines,
        "## Upkeep", "", maintenance, "",
    ])


# テスト用のソースコードの入手先.txt のコンポーネントの行 (専用のアーカイブを入れない行と、入れる行が 1 つずつ)。
SELFTEST_OFFER_LINES = ("- cordmux same tag (Apache-2.0): https://example.invalid/cordmux",
                        "- FFmpeg n7.1.1 (GPL-3.0-or-later): https://example.invalid/ffmpeg")


def selftest_sources(listing_lines: tuple = SELFTEST_OFFER_LINES) -> dict:
    # テスト用のソースをまとめた zip の中身 (最上位のフォルダーの下の名前 -> 中身)。
    listing = "list\n\n" + "".join(f"{line}\n  file\n" for line in listing_lines)
    return {"ffmpeg.tar.gz": b"src", dist_common.RELEASE_SOURCE_LIST: listing.encode("utf-8")}


def selftest_world(root: Path, label: str, build_id: Optional[str] = None, guides: Optional[dict] = None,
                   offer: Optional[str] = None, extra_assets: tuple = (), skip_assets: tuple = (),
                   drop_zip_members: tuple = (), sources: Optional[dict] = None) -> Path:
    # 添付するファイルを置いたフォルダーを作る (zip、pdb の zip、ソースをまとめた zip)。sources はソースをまとめた zip の
    # 最上位のフォルダーの下の名前 -> 中身 (/ で始まる名前は、最上位のフォルダーの外に置く)。
    assets = root / "assets"
    assets.mkdir()
    build_id = build_id if build_id is not None else f"{version_of(label)}+{SELFTEST_COMMIT[:build_info.SHORT_COMMIT_LENGTH]}"
    guides = guides if guides is not None else {lang: selftest_guide() for lang in ("ja", "en")}
    top = f"cordmux-{label}/"
    default_offer = "".join([f"Source: {dist_common.source_url_for(label, SELFTEST_COMMIT)}\n",
                             *(line + "\n" for line in SELFTEST_OFFER_LINES)])
    members = {
        top + "guide-ja.txt": guides["ja"],
        top + "guide-en.txt": guides["en"],
        top + "offer.txt": offer if offer is not None else default_offer,
        top + BUILD_MARK_IN_ZIP: dist_common.build_mark_text(build_id, "0" * 40, "v1.0.0"),
    }
    zip_name = dist_common.RELEASE_ZIP_FORMAT.format(label=label)
    if zip_name not in skip_assets:
        with zipfile.ZipFile(assets / zip_name, "w") as archive:
            for name, text in members.items():
                if name[len(top):] not in drop_zip_members:
                    archive.writestr(name, text.encode("utf-8"))
    pdb = root / dist_common.RELEASE_PDB_MEMBER
    pdb.write_bytes(b"pdb")
    if dist_common.RELEASE_PDB_ZIP_FORMAT.format(label=label) not in skip_assets:
        pack_pdb(pdb, assets, label)
    sources_name = dist_common.RELEASE_SOURCES_ZIP_FORMAT.format(label=label)
    if sources_name not in skip_assets:
        sources_top = sources_name[: -len(".zip")] + "/"
        with zipfile.ZipFile(assets / sources_name, "w") as archive:
            for name, data in (sources if sources is not None else selftest_sources()).items():
                archive.writestr(name[1:] if name.startswith("/") else sources_top + name, data)
    for name in extra_assets:
        (assets / name).write_bytes(b"x")
    return assets


def selftest_materials(lock: Optional[dict] = None, top_level: str = SELFTEST_TOP_LEVEL, changelog: Optional[str] = None,
                       previous_lock: Optional[dict] = None, template: Optional[str] = None,
                       guides: Optional[dict] = None, langs: tuple = ("ja", "en")) -> Materials:
    entries, _ = dist_common.parse_top_level(top_level)
    offer = [name for kind, name in entries if kind == "source-offer"][0]
    guide_texts = guides if guides is not None else {lang: selftest_guide() for lang in ("ja", "en")}
    return Materials(install_guide.parse_rules(selftest_rules_text(langs), "selftest-rules"), dict(guide_texts), offer,
                     template if template is not None else TEMPLATE_PATH.read_text(encoding="utf-8"),
                     lock if lock is not None else {"tag": "v1.15.9"},
                     changelog if changelog is not None else selftest_changelog(), previous_lock)


def selftest() -> int:
    failures: list[str] = []
    parser = build_parser()

    def expect(argv: list[str], want_error: bool, label: str) -> None:
        error = validate(parser.parse_args(argv))
        if (error is not None) != want_error:
            failures.append(f"{label}: {'受け入れない' if want_error else '受け入れる'}はずが {error!r}")

    good = ["--label", "0.1.0-rc1", "--assets-dir", "assets", "--out", "_release/notes.md"]
    expect(good, False, "正しい引数")
    expect(good + ["--pdb", "cordmux.pdb", "--expect-commit", SELFTEST_COMMIT], False, "全部の引数")
    expect(["--assets-dir", "assets", "--out", "_release/notes.md"], True, "--label がない")
    expect(["--label", "0.1.0", "--out", "_release/notes.md"], True, "--assets-dir がない")
    expect(["--label", "0.1.0", "--assets-dir", "assets"], True, "--out がない")
    expect(["--label", "0.1.0+abc1234", "--assets-dir", "assets", "--out", "_release/notes.md"], True, "label にビルド ID")
    expect(["--label", "v0.1.0", "--assets-dir", "assets", "--out", "_release/notes.md"], True, "label の先頭に v")
    expect(good + ["--expect-commit", "abc"], True, "短い --expect-commit")

    # 正式版とプレリリースの判定と、名前の規則
    for label, want in (("0.1.0", False), ("0.1.0-rc1", True), ("1.2.3-beta.2", True)):
        if is_prerelease(label) != want:
            failures.append(f"is_prerelease({label!r}) は {want} のはず")
    if version_of("0.1.0-rc1") != "0.1.0":
        failures.append(f"version_of: {version_of('0.1.0-rc1')!r}")
    if title_for("0.1.0-rc1") != "cordmux v0.1.0-rc1":
        failures.append(f"タイトル: {title_for('0.1.0-rc1')!r}")
    if zip_path_for(Path("assets"), "0.1.0-rc1").name != "cordmux-0.1.0-rc1.zip":
        failures.append(f"zip の名前: {zip_path_for(Path('assets'), '0.1.0-rc1').name!r}")
    if dist_common.RELEASE_SOURCES_ZIP_FORMAT.format(label="0.1.0-rc1") != "cordmux-0.1.0-rc1-sources.zip":
        failures.append(f"ソースをまとめた zip の名前: {dist_common.RELEASE_SOURCES_ZIP_FORMAT.format(label='0.1.0-rc1')!r}")

    # 前のリリースの選び方
    tags = ["v0.1.0-rc1", "v0.1.0", "v0.1.1", "v0.2.0-rc1", "v0.2.0-rc2", "not-a-tag"]
    for label, want in (("0.2.0-rc3", "v0.1.1"), ("0.2.0", "v0.1.1"), ("0.1.0", None), ("0.1.0-rc2", None),
                        ("0.1.1", "v0.1.0"), ("0.3.0", "v0.2.0-rc2")):
        got = previous_release_tag(label, tags)
        if got != want:
            failures.append(f"previous_release_tag({label!r}) は {want!r} のはずが {got!r}")
    if previous_release_tag("0.2.0", ["v0.1.0-rc2", "v0.1.0-rc10"]) != "v0.1.0-rc10":
        failures.append("previous_release_tag: rc10 が rc2 より新しいことになっていない")

    # 実際の rules、テンプレート、CHANGELOG.md、手順書: どの組も maintenance、update の role と placeholder を持ち、
    # 言語 (ja と en) が 1 つずつ決まり、手順書の Markdown がある。変更履歴は全部の節が形と項目のチェックに通る (CI の
    # セルフテストで、書いたその日に失敗にする)。
    try:
        real = real_materials()
        langs = []
        for block in real.blocks:
            for role in ("maintenance", "update"):
                if role not in block.roles:
                    failures.append(f"rules {block.target}: role {role} がない")
            if not block.placeholder:
                failures.append(f"rules {block.target}: placeholder がない")
            langs.append(install_guide.guide_lang(block))
        if sorted(langs) != sorted(dist_common.FILL_LANGS):
            failures.append(f"rules の組の言語は {dist_common.FILL_LANGS} に 1 つずつのはずが {langs}")
        for lang in dist_common.FILL_LANGS:
            if lang not in real.guide_texts:
                failures.append(f"実際の手順書: {lang} の Markdown がない")
        template_keys = set(dist_common.FILL_TOKEN_RE.findall(real.template))
        if internal_word_problems(real.template):
            failures.append(f"テンプレート: {internal_word_problems(real.template)}")
        for key in ("ja_changes", "en_changes", "ja_update_heading", "en_update_heading", "ja_maintenance",
                    "en_maintenance", "sources_asset", "ja_guide_url", "en_guide_url", "ja_update_url",
                    "en_update_url"):
            if key not in template_keys:
                failures.append(f"テンプレートのキー: {sorted(template_keys)} に {key} がない")
        flags = [m.group(1) for m in FLAG_BLOCK_RE.finditer(real.template)]
        if flags != ["prerelease", "prerelease"]:
            failures.append(f"テンプレート: prerelease のブロックは言語ごとに 1 つのはずが {flags}")
        sections, shape = parse_changelog(real.changelog)
        failures += [f"実際の CHANGELOG.md: {p}" for p in shape]
        if not sections:
            failures.append("実際の CHANGELOG.md: バージョンの節がない")
        for section in sections.values():
            failures += [f"実際の CHANGELOG.md: {p}" for p in item_problems(section)]
    except (ValueError, OSError, DistError) as exc:
        failures.append(f"実際のファイル: {exc}")
    rc_sentences = [m.group(2).strip() for m in FLAG_BLOCK_RE.finditer(TEMPLATE_PATH.read_text(encoding="utf-8"))]

    def case(label: str, what: str, want_ok: bool, lock: Optional[dict] = None, expect_commit: Optional[str] = None,
             check=None, top_level: str = SELFTEST_TOP_LEVEL, reason: str = "", changelog: Optional[str] = None,
             previous_lock: Optional[dict] = None, template: Optional[str] = None, langs: tuple = ("ja", "en"),
             **world) -> None:
        # reason は、失敗するはずのデータで、問題の文のどれかに含まれるはずの文字列 (別の理由で失敗したデータを、
        # 期待どおりに数えないため)。
        with tempfile.TemporaryDirectory(prefix="cordmux-notes-selftest-") as tmp:
            md_guides = world.pop("md_guides", None)
            try:
                assets = selftest_world(Path(tmp), label, **world)
                materials = selftest_materials(lock, top_level, changelog, previous_lock, template,
                                               md_guides if md_guides is not None else world.get("guides"), langs)
                notes, problems = compose(label, assets, materials, expect_commit)
            except (DistError, ValueError, OSError) as exc:
                notes, problems = "", [f"例外: {exc}"]
        if (not problems) != want_ok:
            failures.append(f"{what}: {'成功' if want_ok else '失敗'}のはずが {problems}")
        elif not want_ok and not any(reason in problem for problem in problems):
            failures.append(f"{what}: {reason!r} とは別の理由で失敗した: {problems}")
        elif check is not None and want_ok:
            check(notes)

    rc, formal = "0.1.0-rc1", "0.1.0"

    def good_notes(notes: str) -> None:
        guide_url = "https://github.com/eoforgu0/cordmux/blob/v0.1.0-rc1/sample/selftest-ja.txt"
        for piece in ("cordmux-0.1.0-rc1.zip", "cordmux-0.1.0-rc1-pdb.zip", "`cordmux-0.1.0-rc1-sources.zip`",
                      "Sample maintenance text.", "offer.txt", "guide-ja.txt", f"({guide_url})", f"({guide_url}#update)",
                      f"#### {JA_NEW}\n\n- Added x" + JA_PERIOD, "#### Bug Fixes\n\n- Fixed an issue where y failed",
                      "Update", *rc_sentences):
            if piece not in notes:
                failures.append(f"正しいリリースノート: {piece!r} がない")
        if "Changed z" in notes:
            failures.append("正しいリリースノート: 古いバージョンの項目が入っている")
        if dist_common.FILL_OPEN in notes or "\n\n\n" in notes or FENCE in notes:
            failures.append("正しいリリースノート: 差し込みの書式か、続いた空行か、``` が残っている")
        # 並び: 変更点、互換性、添付するファイル (どれも ja の部分が先に出るので、ja の部分で見る)
        positions = [notes.find(piece) for piece in ("- Fixed y" + JA_PERIOD,
                                                     "Sample maintenance text.", "cordmux-0.1.0-rc1-pdb.zip")]
        if positions != sorted(positions):
            failures.append(f"正しいリリースノート: 部分の並びが違う {positions}")

    def formal_notes(notes: str) -> None:
        for sentence in rc_sentences:
            if sentence in notes:
                failures.append(f"正式版のリリースノート: リリース候補の文 {sentence!r} が入っている")

    case(rc, "正しい -rc", True, check=good_notes)

    # 互換性の節のリンクと ### の見出し: リンクはこのバージョンのタグの GitHub の URL にし、### は #### にする
    def linked_notes(notes: str) -> None:
        base = "https://github.com/eoforgu0/cordmux/blob/v0.1.0-rc1/"
        for piece in (f"[Update]({base}sample/selftest-ja.txt#update)", f"[licenses]({base}dist/licenses)",
                      "[site](https://example.com/)", "#### Sub", "`C:\\x`"):
            if piece not in notes:
                failures.append(f"互換性の節のリンク: {piece!r} がない")
    linked = "\n".join(["See [Update](#update), [licenses](../dist/licenses) and [site](https://example.com/).", "",
                         "### Sub", "", "Path: `C:\\x`."])
    case(rc, "互換性の節のリンクと見出し", True, check=linked_notes,
         guides={"ja": selftest_guide(maintenance=linked), "en": selftest_guide()})
    case(formal, "正しい正式版", True, check=formal_notes)
    marked = {lang: selftest_guide(steps="(ja mark)") for lang in ("ja", "en")}
    case(rc, "手順に placeholder がある -rc", True, guides=marked)
    case(formal, "手順に placeholder がある正式版", False, reason='placeholder (ja mark) が残っている', guides=marked)
    case(rc, "互換性の節に placeholder がある", False, reason='maintenance の節に placeholder が残っている',
         guides={"ja": selftest_guide(maintenance="(ja mark)"), "en": selftest_guide()})
    case(rc, "互換性の節が空", False, reason='maintenance の節が空',
         guides={"ja": selftest_guide(), "en": selftest_guide(maintenance="")})
    case(rc, "更新の節がない", False, reason='更新の節 (Update) がない',
         guides={"ja": selftest_guide(), "en": selftest_guide(update=False)})
    case(rc, "差し込みの書式が残っている", False, reason='差し込んでいない',
         guides={"ja": selftest_guide(maintenance="{{source_url}}"), "en": selftest_guide()})
    case(rc, "内部の番号がある", False, reason=f"内部の番号 {SAMPLE_AC_ID}",
         guides={"ja": selftest_guide(maintenance=f"See {SAMPLE_AC_ID}."), "en": selftest_guide()})
    case(rc, "手順書の部分に報告先がある", False, reason='不具合の報告先',
         guides={"ja": selftest_guide(), "en": selftest_guide(maintenance="Use GitHub Issues.")})
    case(rc, "知らない添付するファイル", False, reason='notes.txt は添付するファイルでない', extra_assets=("notes.txt",))
    case(rc, "前の形のソースのアーカイブを添付した", False, reason='cordmux-0.1.0-rc1-src-ffmpeg.tar.gz は添付するファイルでない',
         extra_assets=("cordmux-0.1.0-rc1-src-ffmpeg.tar.gz",))
    case(rc, "pdb の zip がない", False, reason='-pdb.zip がない', skip_assets=(dist_common.RELEASE_PDB_ZIP_FORMAT.format(label=rc),))
    sources_name = dist_common.RELEASE_SOURCES_ZIP_FORMAT.format(label=rc)
    case(rc, "ソースをまとめた zip がない", False, reason=f'{sources_name} がない', skip_assets=(sources_name,))
    no_ffmpeg = {k: v for k, v in selftest_sources().items() if k != "ffmpeg.tar.gz"}
    case(rc, "ソースをまとめた zip に行のアーカイブがない", False, reason="に FFmpeg の行のソースのアーカイブ (ffmpeg.<拡張子>) がない",
         sources=no_ffmpeg)
    case(rc, "ソースをまとめた zip に同じ行のアーカイブが 2 つ", False, reason="2 つある",
         sources={**selftest_sources(), "ffmpeg.zip": b"src"})
    case(rc, "ソースをまとめた zip に行に当たらない項目", False, reason="どの行にも当たらない項目がある: ['x264.tar.gz']",
         sources={**selftest_sources(), "x264.tar.gz": b"src"})
    case(rc, "ソースをまとめた zip の最上位のフォルダーの外に項目", False, reason="の外に項目がある",
         sources={**selftest_sources(), "/sources/ffmpeg.tar.gz": b"src"})
    no_list = {k: v for k, v in selftest_sources().items() if k != dist_common.RELEASE_SOURCE_LIST}
    case(rc, "ソースをまとめた zip に一覧がない", False, reason=f"{dist_common.RELEASE_SOURCE_LIST} がない", sources=no_list)
    case(rc, "一覧に行がない", False, reason="ソースコードの入手先.txt の行がない: ['FFmpeg']",
         sources=selftest_sources(SELFTEST_OFFER_LINES[:1]))
    case(rc, "一覧の行が配布用の zip の行と違う", False, reason="ソースコードの入手先.txt の行がない: ['FFmpeg']",
         sources=selftest_sources((SELFTEST_OFFER_LINES[0], SELFTEST_OFFER_LINES[1].replace("n7.1.1", "n7.1.0"))))
    case(rc, "配布用の zip にコンポーネントの行がない", False, reason="コンポーネントの行がない",
         offer=f"Source: {dist_common.source_url_for(rc, SELFTEST_COMMIT)}\n")
    case(rc, "zip がない", False, reason='cordmux-0.1.0-rc1.zip', skip_assets=(dist_common.RELEASE_ZIP_FORMAT.format(label=rc),))
    case(rc, "ソースコードの入手先.txt にソースの URL がない", False, reason='にソースの URL', offer="Source: somewhere\n")
    case(rc, "zip に英語の手順書がない", False, reason='guide-en.txt がない', drop_zip_members=("guide-en.txt",))
    case(rc, "build_id のバージョンが label と違う", False, reason="label のバージョン '0.1.0' で始まらない", build_id="0.2.0+0123456")
    case(rc, "cordmux-build.txt がない", False, reason="cordmux-build.txt が", drop_zip_members=(BUILD_MARK_IN_ZIP,))
    case(rc, "コミットがタグのコミットと違う", False, reason="--expect-commit " + "f" * 40 + " の短いコミットで終わらない", expect_commit="f" * 40)
    case(rc, "コミットがタグのコミットと同じ", True, expect_commit=SELFTEST_COMMIT)
    case(rc, "英語の手順書のセットがない", False, reason='en の手順書がない', langs=("ja",))
    case(rc, "手順書の Markdown がない", False, reason='sample/selftest-en.txt がない', md_guides={"ja": selftest_guide()})
    case(rc, "テンプレートの知らないフラグ", False, reason='知らないフラグ',
         template="{{#nightly}}x{{/nightly}}\n{{ja_changes}}{{en_changes}}\n")

    # 変更履歴の形
    good_log = selftest_changelog()
    case(rc, "変更履歴にバージョンの節がない", False, reason="'## 0.1.0' の節がない",
         changelog=selftest_changelog(version="0.2.0"))
    case(rc, "変更履歴の節の名前に -rc", False, reason="x.y.z の形でない",
         changelog=good_log.replace("## 0.1.0\n", "## 0.1.0-rc1\n"))
    case(rc, "変更履歴に同じバージョンが 2 つ", False, reason="の節が 2 つある",
         changelog=good_log + "\n## 0.1.0\n\n### ja\n\n#### " + JA_NEW + "\n\n- Added w" + JA_PERIOD + "\n")
    case(rc, "変更履歴のバージョンの順が違う", False, reason="新しい順に並べる",
         changelog=selftest_changelog(older="0.2.0"))
    case(rc, "変更履歴に英語の節がない", False, reason="'### en' の節がない",
         changelog=selftest_changelog(en=[]).replace("### en\n\n## 0.0.9", "## 0.0.9"))
    case(rc, "変更履歴に空の見出し", False, reason="の見出し 'New Features' が空",
         changelog=selftest_changelog(en=[("New Features", []), ("Bug Fixes", ["Fixed an issue where y failed"])]))
    case(rc, "変更履歴の知らない見出し", False, reason="知らない見出し",
         changelog=selftest_changelog(en=[("Improvements", ["Improved x"])]))
    case(rc, "変更履歴の見出しの順が違う", False, reason="の順が違う",
         changelog=selftest_changelog(en=[("Bug Fixes", ["Fixed x"]), ("New Features", ["Added y"])]))
    case(rc, "変更履歴の項目が 2 行", False, reason="1 行の「- 」の項目でもない",
         changelog=good_log.replace("- Added x\n", "- Added x\n  and more\n"))
    case(rc, "日本語の項目に「。」がない", False, reason="「。」で終わっていない",
         changelog=selftest_changelog(ja=[(JA_NEW, ["Added x"])]))
    case(rc, "英語の項目に「.」がある", False, reason="末尾に「.」がある",
         changelog=selftest_changelog(en=[("New Features", ["Added x."])]))
    case(rc, "英語の項目が過去形の動詞で始まらない", False, reason="過去形の動詞",
         changelog=selftest_changelog(en=[("New Features", ["New x is here"])]))
    case(rc, "英語の項目に we がある", False, reason="we、our、us がある",
         changelog=selftest_changelog(en=[("Changes", ["Changed x so we can do y"])]))
    for what, item, reason in (("作業の番号", f"Fixed x ({SAMPLE_RULE_ID})", "internal id"), ("英字を含む作業の番号", f"Fixed x ({SAMPLE_LETTER_RULE_ID})", "internal id"),
                               ("AC-<数字> の番号", f"Fixed {SAMPLE_AC_ID}", "内部の語か番号"), ("リポジトリにない文書の行", "Fixed x (05:2604)", "internal doc"),
                               ("コミット", "Fixed x in 6de9882", "commit"), ("関数の名前", "Fixed decide_dock() x", "コードの名前"),
                               ("Issues", "Fixed x, see GitHub Issues", "不具合の報告先")):
        case(rc, f"変更履歴に{what}", False, reason=reason,
             changelog=selftest_changelog(en=[("Bug Fixes", [item])]))
    case(rc, "変更履歴に作業の中だけで通じる語", False, reason="internal word",
         changelog=selftest_changelog(ja=[(JA_FIXES, ["Fixed " + "決めた" + "こと 1" + JA_PERIOD])]))
    case(rc, "変更履歴が古いファイルの名前を引用する", True,
         changelog=selftest_changelog(en=[("Changes", ['Renamed "name_screen.mov" to "Name Screen Share.mov"'])]))
    case(rc, "変更履歴に -rc の label のバージョンの節がある", True, changelog=good_log)

    # 同梱の Vencord の tag が前のリリースと違う
    lock_new, lock_old = {"tag": "v1.15.9"}, {"tag": "v1.15.1"}
    vencord_ja = (JA_FIXES, ["Updated Vencord v1.15.9" + JA_PERIOD])
    vencord_en = ("Bug Fixes", ["Updated the bundled Vencord to v1.15.9 to fix an error"])
    case(rc, "Vencord が変わったのに項目がない", False, reason="を書いた項目がない", lock=lock_new, previous_lock=lock_old)
    case(rc, "Vencord が変わり、両方の言語に項目がある", True, lock=lock_new, previous_lock=lock_old,
         changelog=selftest_changelog(ja=[vencord_ja], en=[vencord_en]))
    case(rc, "Vencord が変わり、日本語の項目だけがある", False, reason="en: 同梱の Vencord が",
         lock=lock_new, previous_lock=lock_old,
         changelog=selftest_changelog(ja=[vencord_ja]))
    case(rc, "Vencord の項目に新しい tag がない", False, reason="を書いた項目がない", lock=lock_new, previous_lock=lock_old,
         changelog=selftest_changelog(ja=[(JA_FIXES, ["Updated Vencord" + JA_PERIOD])], en=[("Bug Fixes", ["Updated Vencord"])]))
    case(rc, "Vencord が変わっていない", True, lock=lock_new, previous_lock={"tag": "v1.15.9"})
    case(rc, "前のリリースがない", True, lock=lock_new, previous_lock=None)

    # --pdb の名前が違えば作らない (デバッガーは DLL の中に書かれた名前で探す)
    with tempfile.TemporaryDirectory(prefix="cordmux-notes-selftest-") as tmp:
        other = Path(tmp) / "other.pdb"
        other.write_bytes(b"pdb")
        try:
            pack_pdb(other, Path(tmp), rc)
            failures.append("別の名前の pdb で pack_pdb: エラーになるはずが通った")
        except DistError:
            pass

    if failures:
        for failure in failures:
            print(f"selftest 失敗: {failure}", file=sys.stderr)
        return 1
    print("selftest OK: make_release_notes (引数、プレリリースの判定、前のリリース、変更履歴、名前、正しいデータと壊したデータでのリリースノート)")
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
    print(json.dumps(result, ensure_ascii=False))
    return 0 if result.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
