#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# 導入と撤去の往復 (run-window.mjs --step install-roundtrip) で、ファイルのスナップショットを撮ることと、
# 撮った 2 つを比べること。
#
# 撮ることと比べることは Python の 1 ファイルにまとめ、--selftest を付けている。消す対象の表は
# C++ のヘッダー (src-obs/install/removal_table.h) を正とし、このツールがヘッダーを文字列として読む。
# 導入の差分が「製品の行」と「揺れ」だけであること (installed)、撤去の後は揺れ以外の差が 0 件であること
# (uninstalled) を確認する。表に行を足してこのツールに足し忘れると、チェックも失敗する。
# 名前は dist/README.md の契約に合わせる。
#
# 使い方:
#   python install_snapshot.py snapshot --out <撮ったもの.json> --root <名前>=<場所> ... [--exclude <名前>=<相対>]
#   python install_snapshot.py compare --mode calibration|installed|uninstalled --base <A.json> --other <B.json>
#          [--wobble <較正の結果.json>] [--placed <置いた物.json>] [--installed <導入した後のコピー.json>] [--table <removal_table.h>]
#          [--scene-names <scene_names.h>] [--out <結果.json>]
#   python install_snapshot.py table [--table <removal_table.h>]
#   python install_snapshot.py extract --zip <zip> --dest <_work の下の場所> [--strip-single-top]
#   python install_snapshot.py --selftest
#
# extract は往復の zip (OBS と cordmux) を展開する (run-window.mjs の rt-extract)。Python の zipfile で
# 読むのは、名前の UTF-8 フラグ (はじめに.txt などの日本語の名前) を、zip を作るツール (test/tools/dist-zip) と
# 同じ読み方で扱うため。PowerShell と tar の展開は、バージョンによって名前の読み方が違う。展開先は `_work`
# の下だけで、`..` や絶対パスを含む名前は断る。
#
# 結果は末尾の 1 行 `CORDMUX_SNAPSHOT_JSON:<JSON>` (seed-config.py / cleanup.ps1 と同じ形)。
# 終了コード: 0 = 合格、1 = 不合格 (差分が許されない・撮れていない)、2 = ツールの誤り (引数・表が読めない等)。
#
# 撮るもの: ルートごとにファイルとフォルダーの一覧。ファイルは大きさと SHA-256 を持つ。
# JSON (*.json / *.json.bak) と ini (*.ini。utf-8-sig) は、キーの単位の中身も持つ (state.json も JSON)。
# JSON の配列は、要素が全部 "name" を持つ辞書なら名前で引く ({名前})。シーンコレクションの
# sources / items / filters / scene_order が並び替わっても差に出ない。
#
# 比べ方:
#   - calibration: cordmux なしで 2 回起動した前後 (A, B) の差を「揺れ」とする。差に利用者のソース
#     (コレクションの sources) が入ったら不合格にする。揺れのキーは、ファイルのパスの数字の並びを # に
#     まとめて持つ (起動のたびに名前の違うログのファイルが増えるため。固定の一覧ではなく、実測した
#     パスの形で持つ)。
#   - installed: 導入前 (A) と導入して閉じた後 (B) の差から揺れを引き、残りを表の行に
#     割り当てる。どの行 (製品・人・消さない) にも一致しない差があれば失敗する。製品の行にも置いた物にも
#     1 件も一致しなければ「入っていない」として不合格にする (撮れていない場合を合格にしない)。
#   - uninstalled: 導入前 (A) と、[外す] -> 置いた物を消す -> cordmux なしで起動した後 (B) の
#     差から揺れを引く。消さない行 (Nobody) だけを許し、製品の行と人の行に一致する差は「残っている」
#     として不合格にする。行ごとの結果を残す。
#
# 表 (src-obs/install/removal_table.h) は文字列として読む。目印の行は行頭で見る
# (`// removal-table-begin` / `// removal-table-end`。後ろにコメントが続く)。行の数を
# `kRemovalItemCount = <数>;` と突き合わせ、知らない Locate の行があればツールの誤り (2) にする。
# 探し方 (Locate) ごとに 1 つの見方を持つ (下の LOCATE_VIEWS)。表に行と探し方を足したら、
# ここにも見方を足さないと不合格になる。
#
# 見方の決め方 (このファイルで決めたもの):
#   (1) ObsProfileDockState の見方は global.ini / user.ini の [BasicWindow] の DockState だけにした。
#       テスト用の送信側だけでの実行で残るものを実測してから広げる。
#   (2) ObsLogDir の見方は OBS の設定の保存先の logs/ の下。crashes/ は入れない。落ちたログは
#       残っていれば気付くべきものなので、揺れか違反として目に入る側に置く。
#   (3) 数字のまとめ方はパス (ファイルの名前) にだけ使い、JSON のキーには使わない (枠の番号
#       "cordmux 画面 1" と "2" を同じものと見なさないため)。
#   (4) シーンコレクションのバックアップ (basic/scenes/*.json.bak) の差は、どの比べ方でも汚れにも違反にも
#       数えず、別の集計 (backupChurn) に数だけ残す。OBS は保存のたびに前の中身を .bak へコピーする
#       (src-obs/.deps/obs-studio-31.1.1/frontend/widgets/OBSBasic_SceneCollections.cpp:956。終了時にも保存する =
#       frontend/widgets/OBSBasic.cpp:1690) ので、.bak は本体より保存 1 回ぶん遅れたコピーである。
#       初期設定の中身 (prev_ver: 0) と OBS が保存し直した中身 (libobs/obs.c:2432) の差が、2 回目の較正にだけ
#       現れうる。本体の .json を比べれば、.bak が遅れて持つ差は 1 回前の保存の時点で本体に出ている。
#       1 回の起動で何回保存するかは測っていない。
#   (5) 1 つの差が 2 つの行に一致するときは、LOCATE_ORDER の順に先に一致した行に数える
#       (行ごとの数を「その行の物が残った数」として読めるようにするため)。
#   (6) 撮れていなければ比べないルート (REQUIRED_ROOTS) は obs_config・obs_program・appdata の 3 つ。appdata は
#       往復では必ずテスト用の APPDATA (seed-config.py --shape user が Vesktop の state.json を置く) で、ないまま
#       比べると state.json の vencordDir の行が何も見ずに成功する。localappdata は cordmux が導入の後に
#       作るので導入前にはなく、必須にしない。
#   (7) ObsPluginManagerFile の見方は、OBS の設定の保存先の plugin_manager/modules.json の全部のキー (ファイルごとの
#       差も含む)。OBS 32.2.2 の往復では、このファイルのキーだけが導入の比べと撤去の比べの違反になった
#       (OBS 32.x のプラグインの管理が読み込んだモジュールを覚え、DLL を消して起動し直しても
#       cordmux の項目が残る。31.1.1 の往復ではこのファイルがない)。同じフォルダーのほかのファイルと
#       フォルダーそのものは入れない。実測に出ていないので、出たら揺れか違反として目に入る側に置く。
#   (8) 利用者のシーンに置いた cordmux のソースの項目には、OBS が表示・非表示の割り当て
#       (hotkeys の libobs.show_scene_item.<id> と libobs.hide_scene_item.<id>) を書く。<id> が同じシーンの
#       cordmux の種類の項目の id であるものは CordmuxSources の行に数える (SourceTypeId の見方)。ほかの項目の id なら
#       表の外の差のままにする。シーンの settings.id_counter は OBS の項目の番号のカウンターで、項目を足すと増え、
#       消しても戻らない (製品が戻すこともできないので、表の行にしない)。導入した後のコピーでそのシーンに
#       cordmux の種類の項目が置かれているときだけ、増えた差を「戻らないカウンター」(unrestoredCounter) として
#       不合格にせず、名前と理由を出力に残す。減った差と、cordmux の項目のないシーンの差は不合格のままにする。
#       撤去の比べは --installed (なければ --other の名前の -uninstalled を -installed に替えた隣) のコピーで見る。
#       コピーがなければカウンターは許さない (不合格のまま)。
import argparse
import copy
import hashlib
import json
import os
import re
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent.parent
DEFAULT_TABLE = REPO_ROOT / "src-obs" / "install" / "removal_table.h"
DEFAULT_SCENE_NAMES = REPO_ROOT / "src-obs" / "sources" / "scene_names.h"

RESULT_PREFIX = "CORDMUX_SNAPSHOT_JSON:"
SNAPSHOT_VERSION = 1

# removal_table.h の「Python が読む形」は test/lint/removal_table.py が正 (目印・行の正規表現・数の読み方を
# 2 か所に持たない = 2 か所に持たない)。ここはそれを import し、探し方ごとの見方 (LOCATE_VIEWS) の照合だけを足す。
sys.dont_write_bytecode = True
sys.path.insert(0, str(REPO_ROOT / "test" / "lint"))
import removal_table  # noqa: E402

TABLE_BEGIN = removal_table.TABLE_BEGIN
TABLE_END = removal_table.TABLE_END
# scene_names.h から型 id を読む (同じ綴りを 2 か所で決めない)。
CONST_RE = re.compile(r"inline\s+constexpr\s+const\s+char\s*\*\s*(k\w+)\s*=\s*\"([^\"]*)\"\s*;")
SOURCE_TYPE_CONSTS = ("kCompositeSourceTypeId",)
SCENE_ITEM_HOTKEY_RE = re.compile(r"^libobs\.(?:show|hide)_scene_item\.(\d+)$")

# 表の外で不合格にしないカウンター (is_unrestored_id_counter)。比べの出力に行として残す。
UNRESTORED_COUNTER_NAME = "obs-scene-id-counter"
UNRESTORED_COUNTER_REASON = "OBS keeps raising the scene item id counter and never lowers it after an item is removed"

JSON_SUFFIXES = (".json", ".json.bak")
INI_SUFFIXES = (".ini",)
DIGITS_RE = re.compile(r"\d+")

# 根の名前 (run-window.mjs が渡す)。
ROOT_OBS_CONFIG = "obs_config"
ROOT_OBS_PROGRAM = "obs_program"
ROOT_APPDATA = "appdata"
ROOT_LOCALAPPDATA = "localappdata"
ROOT_PROGRAMDATA_PLUGINS = "programdata_plugins"
ROOT_RECORD = "record"
# 撮れていないと比べられない根 (無ければ撮ったことにしない)。
REQUIRED_ROOTS = (ROOT_OBS_CONFIG, ROOT_OBS_PROGRAM, ROOT_APPDATA)


class ToolError(Exception):
    # ツールの誤り (終了コード 2)。
    pass


# ---- 表を文字列として読む ----------------------------------------------------------------


def parse_removal_table(text):
    # removal_table.h の中身 -> (行の並び, kRemovalItemCount)。形が崩れていれば ToolError。
    # 読み方は test/lint/removal_table.py の parse_removal_table (上の import の注記)。
    try:
        rows, count = removal_table.parse_removal_table(text)
    except removal_table.TableError as e:
        raise ToolError(str(e))
    rows = [{k: row[k] for k in ("item", "remover", "locate", "step")} for row in rows]
    for row in rows:
        if row["locate"] not in LOCATE_VIEWS:
            raise ToolError("no view for Locate::" + row["locate"])
        if row["remover"] not in ("Product", "User", "Nobody"):
            raise ToolError("unknown Remover::" + row["remover"])
    return rows, count


def parse_scene_names(text):
    consts = dict(CONST_RE.findall(text))
    missing = [c for c in SOURCE_TYPE_CONSTS if c not in consts]
    if missing:
        raise ToolError("scene_names.h lacks " + ", ".join(missing))
    return {
        "sourceTypes": sorted(consts[c] for c in SOURCE_TYPE_CONSTS),
    }


# ---- 撮る ------------------------------------------------------------------------


def seg_key(segments):
    return json.dumps(segments, ensure_ascii=False)


def flatten_json(value, segments, out):
    if isinstance(value, dict):
        if not value:
            out[seg_key(segments)] = "{}"
        for k in sorted(value):
            flatten_json(value[k], segments + [k], out)
    elif isinstance(value, list):
        if not value:
            out[seg_key(segments)] = "[]"
            return
        named = all(isinstance(x, dict) and isinstance(x.get("name"), str) for x in value)
        seen = {}
        for i, x in enumerate(value):
            if named:
                n = x["name"]
                seen[n] = seen.get(n, 0) + 1
                label = "{" + n + ("#%d" % seen[n] if seen[n] > 1 else "") + "}"
            else:
                label = "[%d]" % i
            flatten_json(x, segments + [label], out)
    else:
        out[seg_key(segments)] = json.dumps(value, ensure_ascii=False, sort_keys=True)


def flatten_ini(text):
    out = {}
    section = ""
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith(";") or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line
            continue
        if "=" in line:
            k, v = line.split("=", 1)
            out[seg_key([section, k.strip()])] = v.strip()
    return out


def content_of(path_str, data):
    lower = path_str.lower()
    try:
        if lower.endswith(JSON_SUFFIXES):
            out = {}
            flatten_json(json.loads(data.decode("utf-8-sig")), [], out)
            return "json", out
        if lower.endswith(INI_SUFFIXES):
            return "ini", flatten_ini(data.decode("utf-8-sig"))
    except (ValueError, UnicodeDecodeError):
        return "unparsed", None
    return "binary", None


def take_snapshot(roots, excludes):
    # roots: {名前: 場所}。excludes: {名前: [相対の場所]}。
    snap = {"version": SNAPSHOT_VERSION, "roots": {}, "entries": {}}
    for name, base in sorted(roots.items()):
        base_path = Path(base)
        exists = base_path.is_dir()
        snap["roots"][name] = {"path": str(base_path), "exists": exists}
        if not exists:
            continue
        skip = [e.strip("/").lower() for e in excludes.get(name, [])]
        for dirpath, dirnames, filenames in os.walk(base_path):
            rel_dir = Path(dirpath).relative_to(base_path).as_posix()
            rel_dir = "" if rel_dir == "." else rel_dir
            kept = []
            for d in sorted(dirnames):
                rel = (rel_dir + "/" + d) if rel_dir else d
                if rel.lower() in skip:
                    continue
                kept.append(d)
                snap["entries"][name + ":" + rel] = {"type": "dir"}
            dirnames[:] = kept
            for f in sorted(filenames):
                rel = (rel_dir + "/" + f) if rel_dir else f
                if rel.lower() in skip:
                    continue
                full = Path(dirpath) / f
                try:
                    data = full.read_bytes()
                except OSError as e:
                    snap["entries"][name + ":" + rel] = {"type": "file", "error": str(e)}
                    continue
                fmt, flat = content_of(rel, data)
                entry = {"type": "file", "size": len(data), "sha256": hashlib.sha256(data).hexdigest(), "format": fmt}
                if flat is not None:
                    entry["flat"] = flat
                snap["entries"][name + ":" + rel] = entry
    return snap


# ---- 差を取る --------------------------------------------------------------------


def split_entry(entry_key):
    root, _, rel = entry_key.partition(":")
    return root, rel


def diff_snapshots(a, b):
    items = []
    keys = sorted(set(a["entries"]) | set(b["entries"]))
    for k in keys:
        ea = a["entries"].get(k)
        eb = b["entries"].get(k)
        root, rel = split_entry(k)
        if ea is None or eb is None:
            present = eb if ea is None else ea
            items.append({"root": root, "path": rel, "type": present["type"],
                          "change": "added" if ea is None else "removed", "key": None})
            continue
        if ea["type"] != eb["type"]:
            items.append({"root": root, "path": rel, "type": eb["type"], "change": "changed", "key": None})
            continue
        if ea["type"] == "dir" or ea.get("sha256") == eb.get("sha256"):
            continue
        fa, fb = ea.get("flat"), eb.get("flat")
        if fa is None or fb is None:
            items.append({"root": root, "path": rel, "type": "file", "change": "changed", "key": None})
            continue
        for fk in sorted(set(fa) | set(fb)):
            va, vb = fa.get(fk), fb.get(fk)
            if va == vb:
                continue
            change = "added" if va is None else ("removed" if vb is None else "changed")
            items.append({"root": root, "path": rel, "type": "file", "change": change, "key": fk,
                          "before": va, "after": vb})
    return items


def wobble_key(item):
    return "%s:%s|%s|%s" % (item["root"], DIGITS_RE.sub("#", item["path"]), item["key"] or "", item["change"])


# ---- 表の行へ割り当てる (Locate ごとの見方) ------------------------------------


def is_collection(item):
    return (item["root"] == ROOT_OBS_CONFIG and item["path"].startswith("basic/scenes/")
            and item["path"].lower().endswith(JSON_SUFFIXES))


def is_collection_backup(item):
    # シーンコレクションのバックアップ (*.json.bak)。見方の決め方の (4)。
    return is_collection(item) and item["path"].lower().endswith(".json.bak")


def key_segments(item):
    return json.loads(item["key"]) if item["key"] else []


def flat_of(snap, item):
    entry = snap["entries"].get(item["root"] + ":" + item["path"])
    return (entry or {}).get("flat") or {}


def source_value(flat, source_label, *rest):
    raw = flat.get(seg_key(["sources", source_label] + list(rest)))
    if raw is None:
        return None
    try:
        return json.loads(raw)
    except ValueError:
        return raw


def lookup_both(ctx, item, source_label, *rest):
    # 変わった側 (後) を先に見て、無ければ前を見る (消えたものは前にしかない)。
    for snap in (ctx["other"], ctx["base"]):
        v = source_value(flat_of(snap, item), source_label, *rest)
        if v is not None:
            return v
    return None


def uuid_to_label(ctx, item, uuid_value):
    for snap in (ctx["other"], ctx["base"]):
        for k, v in flat_of(snap, item).items():
            segs = json.loads(k)
            if len(segs) == 3 and segs[0] == "sources" and segs[2] == "uuid" and json.loads(v) == uuid_value:
                return segs[1]
    return None


def scene_item_source_label(ctx, item, scene_label, item_id):
    # シーン scene_label の項目のうち id が item_id のものが指すソースの名前。無ければ None。
    for snap in (ctx["other"], ctx["base"]):
        flat = flat_of(snap, item)
        for k, v in flat.items():
            segs = json.loads(k)
            if (len(segs) == 6 and segs[0] == "sources" and segs[1] == scene_label and segs[2:4] == ["settings", "items"]
                    and segs[5] == "id" and json.loads(v) == item_id):
                uuid_value = flat.get(seg_key(segs[:5] + ["source_uuid"]))
                return uuid_to_label(ctx, item, json.loads(uuid_value)) if uuid_value is not None else None
    return None


def scene_has_cordmux_item(snap, item, scene_label, names):
    # コピー snap の同じファイルで、シーン scene_label に cordmux の種類のソースを指す項目があるか。
    flat = flat_of(snap, item)
    uuid_labels = {}
    for k, v in flat.items():
        segs = json.loads(k)
        if len(segs) == 3 and segs[0] == "sources" and segs[2] == "uuid":
            uuid_labels[json.loads(v)] = segs[1]
    for k, v in flat.items():
        segs = json.loads(k)
        if (len(segs) == 6 and segs[0] == "sources" and segs[1] == scene_label and segs[2:4] == ["settings", "items"]
                and segs[5] == "source_uuid"):
            label = uuid_labels.get(json.loads(v))
            raw = flat.get(seg_key(["sources", label, "id"])) if label is not None else None
            if raw is not None and json.loads(raw) in names["sourceTypes"]:
                return True
    return False


def is_unrestored_id_counter(ctx, item):
    # シーンの settings.id_counter が増えた差。OBS の項目の番号のカウンターで、項目を足すと増え、消しても戻らない
    # (cordmux の物ではなく、製品が戻すこともできない)。導入した後のコピーでそのシーンに cordmux の種類の項目が
    # 置かれているときだけ許す。減った差と、cordmux の項目のないシーンの差は許さない。
    if not is_collection(item) or item["change"] != "changed":
        return False
    segs = key_segments(item)
    if len(segs) != 4 or segs[0] != "sources" or segs[2:] != ["settings", "id_counter"]:
        return False
    try:
        before, after = int(json.loads(item["before"])), int(json.loads(item["after"]))
    except (TypeError, ValueError):
        return False
    installed = ctx.get("installed")
    return after > before and installed is not None and scene_has_cordmux_item(installed, item, segs[1], ctx["names"])


def view_source_type_id(ctx, item):
    if not is_collection(item):
        return False
    segs = key_segments(item)
    if len(segs) < 2 or segs[0] != "sources":
        return False
    if lookup_both(ctx, item, segs[1], "id") in ctx["names"]["sourceTypes"]:
        return True
    # 利用者のシーンに置いた cordmux のソースの項目 (sources{S}.settings.items{I})。
    if len(segs) >= 5 and segs[2] == "settings" and segs[3] == "items":
        uuid_value = lookup_both(ctx, item, segs[1], "settings", "items", segs[4], "source_uuid")
        label = uuid_to_label(ctx, item, uuid_value) if uuid_value is not None else None
        return label is not None and lookup_both(ctx, item, label, "id") in ctx["names"]["sourceTypes"]
    # 置いた項目に OBS が書く表示・非表示の割り当て (sources{S}.hotkeys{libobs.show_scene_item.<id>} と hide)。
    # <id> が同じシーンの cordmux の種類の項目の id であるものだけ。ほかの項目の id なら表の外のまま。
    if len(segs) == 4 and segs[2] == "hotkeys":
        m = SCENE_ITEM_HOTKEY_RE.match(segs[3])
        if m is None:
            return False
        label = scene_item_source_label(ctx, item, segs[1], int(m.group(1)))
        return label is not None and lookup_both(ctx, item, label, "id") in ctx["names"]["sourceTypes"]
    return False


def under(path, prefix):
    return path == prefix or path.startswith(prefix + "/")


def view_module_config_dir(ctx, item):
    return item["root"] == ROOT_OBS_CONFIG and under(item["path"], "plugin_config/cordmux")


def view_local_data_dir(ctx, item):
    return item["root"] == ROOT_LOCALAPPDATA and under(item["path"], "cordmux")


def view_state_json_key(ctx, item):
    return (item["root"] == ROOT_APPDATA and item["path"] == "Vesktop/state.json"
            and key_segments(item) == ["vencordDir"])


def view_module_folder(ctx, item):
    for placed in ctx["placed"]:
        if item["root"] == placed.get("root") and under(item["path"], placed.get("rel", "").strip("/")):
            return True
    return False


def view_record_dir(ctx, item):
    return item["root"] == ROOT_RECORD


def view_obs_profile_dock_state(ctx, item):
    return (item["root"] == ROOT_OBS_CONFIG and item["path"] in ("global.ini", "user.ini")
            and key_segments(item) == ["[BasicWindow]", "DockState"])


def view_obs_log_dir(ctx, item):
    return item["root"] == ROOT_OBS_CONFIG and under(item["path"], "logs")


def view_obs_plugin_manager_file(ctx, item):
    # 見方の決め方の (7)。キーは見ない (ファイルごとの差も、どのキーの差もこの行に当てる)。
    return item["root"] == ROOT_OBS_CONFIG and item["path"] == "plugin_manager/modules.json"


LOCATE_VIEWS = {
    "SourceTypeId": view_source_type_id,
    "ModuleConfigDir": view_module_config_dir,
    "LocalDataDir": view_local_data_dir,
    "StateJsonKey": view_state_json_key,
    "ModuleFolder": view_module_folder,
    "RecordDir": view_record_dir,
    "ObsProfileDockState": view_obs_profile_dock_state,
    "ObsLogDir": view_obs_log_dir,
    "ObsPluginManagerFile": view_obs_plugin_manager_file,
}
# 1 つの差が 2 つの行に一致するとき、先に見る行。LOCATE_VIEWS に見方を足したらここにも足す (割り当てはこの並びだけを回る)。
LOCATE_ORDER = ["SourceTypeId", "ModuleConfigDir", "LocalDataDir",
                "StateJsonKey", "ModuleFolder", "RecordDir", "ObsProfileDockState", "ObsLogDir",
                "ObsPluginManagerFile"]


def attribute(ctx, item):
    for locate in LOCATE_ORDER:
        for row in ctx["rows"]:
            if row["locate"] == locate and LOCATE_VIEWS[locate](ctx, item):
                return row
    return None


def is_user_source_item(item):
    # 較正の汚れ: コレクションの sources か groups に一致する差。
    if not is_collection(item) or is_collection_backup(item):
        return False
    if item["key"] is None:
        return True
    segs = key_segments(item)
    return bool(segs) and segs[0] in ("sources", "groups")


def snapshots_complete(*snaps):
    missing = []
    for snap in snaps:
        for name in REQUIRED_ROOTS:
            if not snap.get("roots", {}).get(name, {}).get("exists"):
                missing.append(name)
    return missing


def compare(mode, base, other, wobble_keys, placed, rows, names, installed=None):
    diffs = diff_snapshots(base, other)
    missing = snapshots_complete(base, other)
    result = {"mode": mode, "ok": False, "diffCount": len(diffs), "missingRoots": missing}
    if mode == "calibration":
        wobble = sorted({wobble_key(d) for d in diffs})
        contaminated = [d for d in diffs if is_user_source_item(d)]
        result.update({"wobble": wobble, "wobbleCount": len(wobble), "contaminated": contaminated[:50],
                       "contaminatedCount": len(contaminated),
                       "backupChurnCount": sum(1 for d in diffs if is_collection_backup(d))})
        result["ok"] = not missing and not contaminated
        result["reason"] = ("snapshot incomplete" if missing else
                            "calibration differences touch user sources" if contaminated else None)
        return result

    # 導入した後のコピー。installed の比べでは other そのもの。uninstalled の比べでは呼び出し元が渡したもの (無ければ None)。
    ctx = {"base": base, "other": other, "placed": placed, "rows": rows, "names": names,
           "installed": other if mode == "installed" else installed}
    wobble_set = set(wobble_keys)
    row_results = {r["item"]: {"item": r["item"], "remover": r["remover"], "locate": r["locate"], "step": r["step"],
                               "count": 0, "samples": []} for r in rows}
    wobbled, violations, backup_churn, unrestored = [], [], [], []
    for d in diffs:
        if is_collection_backup(d):
            backup_churn.append(d)
            continue
        row = attribute(ctx, d)
        if row is not None:
            rr = row_results[row["item"]]
            rr["count"] += 1
            if len(rr["samples"]) < 20:
                rr["samples"].append(d)
            if mode == "uninstalled" and row["remover"] != "Nobody":
                violations.append(dict(d, row=row["item"], reason="left after uninstall"))
            continue
        if wobble_key(d) in wobble_set:
            wobbled.append(d)
            continue
        if is_unrestored_id_counter(ctx, d):
            unrestored.append(dict(d, name=UNRESTORED_COUNTER_NAME, reason=UNRESTORED_COUNTER_REASON))
            continue
        violations.append(dict(d, row=None, reason="not in the removal table and not wobble"))

    result.update({"rows": list(row_results.values()), "wobbledCount": len(wobbled),
                   "backupChurnCount": len(backup_churn), "backupChurn": backup_churn[:20],
                   "unrestoredCounterCount": len(unrestored), "unrestoredCounter": unrestored[:20],
                   "installedSnapshotGiven": ctx["installed"] is not None,
                   "violations": violations[:100], "violationCount": len(violations)})
    reason = None
    if missing:
        reason = "snapshot incomplete"
    elif mode == "installed":
        product_hits = sum(r["count"] for r in row_results.values() if r["remover"] == "Product")
        user_hits = sum(r["count"] for r in row_results.values() if r["remover"] == "User")
        if product_hits == 0 or user_hits == 0:
            reason = "nothing looks installed (no difference on the product rows or the placed files)"
    if reason is None and violations:
        reason = "differences outside the removal table and the wobble" if mode == "installed" else "left after uninstall or outside the wobble"
    result["reason"] = reason
    result["ok"] = reason is None
    return result


# ---- 入口 ------------------------------------------------------------------------


def load_json_file(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def emit(result, out_path=None):
    if out_path:
        Path(out_path).parent.mkdir(parents=True, exist_ok=True)
        with open(out_path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(result, f, ensure_ascii=False, indent=2)
    print(RESULT_PREFIX + json.dumps(result, ensure_ascii=False))


def parse_pairs(values, flag):
    out = {}
    for v in values or []:
        if "=" not in v:
            raise ToolError(flag + " expects NAME=VALUE: " + v)
        k, val = v.split("=", 1)
        out.setdefault(k, []).append(val)
    return out


def cmd_snapshot(args):
    roots = {k: v[-1] for k, v in parse_pairs(args.root, "--root").items()}
    excludes = parse_pairs(args.exclude, "--exclude")
    snap = take_snapshot(roots, excludes)
    missing = snapshots_complete(snap)
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(snap, f, ensure_ascii=False)
    result = {"ok": not missing, "out": args.out, "entryCount": len(snap["entries"]),
              "roots": snap["roots"], "missingRoots": missing,
              "reason": "required roots missing: " + ", ".join(missing) if missing else None}
    emit(result)
    return 0 if not missing else 1


def installed_snapshot(args):
    # uninstalled の比べで使う、導入した後のコピー。--installed で渡す。渡されなければ、--other の名前の
    # `-uninstalled.json` を `-installed.json` に替えた隣 (往復のスクリプトが付ける名前) を探す。どちらも無ければ None
    # (id_counter の差は既定どおり表の外の差になる)。
    if args.mode != "uninstalled":
        return None
    path = args.installed
    if not path:
        other = Path(args.other)
        if not other.name.endswith("-uninstalled.json"):
            return None
        path = str(other.with_name(other.name[:-len("-uninstalled.json")] + "-installed.json"))
        if not Path(path).is_file():
            return None
    return load_json_file(path)


def cmd_compare(args):
    rows, _ = parse_removal_table(Path(args.table).read_text(encoding="utf-8"))
    names = parse_scene_names(Path(args.scene_names).read_text(encoding="utf-8"))
    base = load_json_file(args.base)
    other = load_json_file(args.other)
    wobble = load_json_file(args.wobble).get("wobble", []) if args.wobble else []
    placed = load_json_file(args.placed).get("placed", []) if args.placed else []
    if args.mode != "calibration" and not args.wobble:
        raise ToolError("--wobble is required for " + args.mode)
    result = compare(args.mode, base, other, wobble, placed, rows, names, installed_snapshot(args))
    emit(result, args.out)
    return 0 if result["ok"] else 1


WORK_DIR_MARKER = "_work"


def safe_member_path(dest, name):
    # zip の中の名前を dest の下の場所にする。絶対の場所・ドライブ・.. を含む名前は断る。
    parts = [p for p in name.replace("\\", "/").split("/") if p not in ("", ".")]
    if not parts or any(p == ".." for p in parts) or ":" in parts[0] or name.startswith(("/", "\\")):
        raise ToolError("unsafe name in the zip: " + name)
    return dest.joinpath(*parts)


def extract_zip(zip_path, dest, strip_single_top):
    # zip を dest へ展開する (Python の zipfile。名前の UTF-8 フラグを読む。zip を作るツールと同じ読み方)。
    # strip_single_top: 全部が 1 つのフォルダの下にある zip は、そのフォルダの中身を dest へ置く (OBS の zip 用)。
    import shutil
    import zipfile
    if WORK_DIR_MARKER not in Path(dest).resolve().parts:
        raise ToolError("--dest must be under a " + WORK_DIR_MARKER + " folder: " + str(dest))
    dest = Path(dest)
    with zipfile.ZipFile(zip_path) as zf:
        infos = zf.infolist()
        tops = sorted({i.filename.replace("\\", "/").split("/")[0] for i in infos if i.filename})
        strip = None
        if strip_single_top and len(tops) == 1 and any("/" in i.filename.replace("\\", "/") for i in infos):
            strip = tops[0] + "/"
        dest.mkdir(parents=True, exist_ok=True)
        count = 0
        for info in infos:
            name = info.filename.replace("\\", "/")
            if strip:
                if not name.startswith(strip):
                    continue
                name = name[len(strip):]
                if not name:
                    continue
            target = safe_member_path(dest, name)
            if name.endswith("/"):
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with zf.open(info) as src, open(target, "wb") as out:
                shutil.copyfileobj(src, out)
            count += 1
    return {"ok": True, "files": count, "tops": tops[:20], "stripped": strip}


def cmd_extract(args):
    emit(extract_zip(args.zip, args.dest, args.strip_single_top))
    return 0


def cmd_table(args):
    rows, count = parse_removal_table(Path(args.table).read_text(encoding="utf-8"))
    emit({"ok": True, "count": count, "rows": rows})
    return 0


def main(argv):
    if argv and argv[0] == "--selftest":
        return selftest()
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="cmd", required=True)
    p_snap = sub.add_parser("snapshot")
    p_snap.add_argument("--out", required=True)
    p_snap.add_argument("--root", action="append", required=True)
    p_snap.add_argument("--exclude", action="append")
    p_cmp = sub.add_parser("compare")
    p_cmp.add_argument("--mode", choices=["calibration", "installed", "uninstalled"], required=True)
    p_cmp.add_argument("--base", required=True)
    p_cmp.add_argument("--other", required=True)
    p_cmp.add_argument("--wobble")
    p_cmp.add_argument("--placed")
    p_cmp.add_argument("--installed")
    p_cmp.add_argument("--table", default=str(DEFAULT_TABLE))
    p_cmp.add_argument("--scene-names", default=str(DEFAULT_SCENE_NAMES))
    p_cmp.add_argument("--out")
    p_tab = sub.add_parser("table")
    p_tab.add_argument("--table", default=str(DEFAULT_TABLE))
    p_ext = sub.add_parser("extract")
    p_ext.add_argument("--zip", required=True)
    p_ext.add_argument("--dest", required=True)
    p_ext.add_argument("--strip-single-top", action="store_true")
    args = parser.parse_args(argv)
    try:
        return {"snapshot": cmd_snapshot, "compare": cmd_compare, "table": cmd_table, "extract": cmd_extract}[args.cmd](args)
    except (ToolError, OSError, ValueError) as e:
        emit({"ok": False, "error": str(e)})
        return 2


# ---- セルフテスト (ファイルを書かず、手で作ったスナップショットで確認する) ---------------------


def _collection(sources, order):
    return {"name": "Stream", "current_scene": order[0], "scene_order": [{"name": n} for n in order], "sources": sources}


def _src(name, sid, uuid_value, **extra):
    s = {"name": name, "id": sid, "uuid": uuid_value, "settings": {}, "private_settings": {}}
    s.update(extra)
    return s


def _file_entry(path, obj=None, text=None, raw=b""):
    if obj is not None:
        data = json.dumps(obj).encode("utf-8")
    elif text is not None:
        data = text.encode("utf-8")
    else:
        data = raw
    fmt, flat = content_of(path, data)
    e = {"type": "file", "size": len(data), "sha256": hashlib.sha256(data).hexdigest(), "format": fmt}
    if flat is not None:
        e["flat"] = flat
    return e


def _snap(files, dirs=()):
    snap = {"version": SNAPSHOT_VERSION,
            "roots": {n: {"path": n, "exists": True} for n in (ROOT_OBS_CONFIG, ROOT_OBS_PROGRAM, ROOT_APPDATA, ROOT_LOCALAPPDATA)},
            "entries": {}}
    for d in dirs:
        snap["entries"][d] = {"type": "dir"}
    for key, entry in files.items():
        snap["entries"][key] = entry
    return snap


# セルフテストの一時の保存先。呼び出し元の TEMP (tempfile.gettempdir()) の下に作って消す。
# _work は実機の実行の保存先 (実行の準備が中身を保存して凍結し、実行スクリプトの後片付けが毎回消す場所) で、
# セルフテストがそこに書くと実行の前後の中身の比べに混じり、CI でもセルフテストを実行できない。
# seed-config・border_timing・harness-suspend のセルフテストも同じ形 (seed-config.py の selftest_place_problem)。
# build_x64 の決まった場所は使わない。CI と手元で保存先が違い、呼び出し元が TEMP で決められない。
# 呼び出し元が TEMP を替えなければ OS の既定の一時フォルダになる。


def selftest_place_problem(path, temp_dir, repo_root):
    # セルフテストの保存先が条件に合うか。合えば None、合わなければ理由。純関数 (セルフテストが実際の保存先と別の保存先の両方で確認する)。
    full = os.path.normcase(os.path.abspath(str(path)))
    temp_full = os.path.normcase(os.path.abspath(str(temp_dir)))
    work_full = os.path.normcase(os.path.abspath(os.path.join(str(repo_root), "_work")))
    if full != temp_full and not full.startswith(temp_full.rstrip(os.sep) + os.sep):
        return "not under the caller's TEMP (" + str(temp_dir) + ")"
    if full == work_full or full.startswith(work_full + os.sep):
        return "under _work (the live window's folder)"
    return None


def selftest():
    failures = []

    def check(cond, label):
        if not cond:
            failures.append(label)
        print(("ok   " if cond else "FAIL ") + label)

    names = parse_scene_names(DEFAULT_SCENE_NAMES.read_text(encoding="utf-8"))
    real_rows, real_count = parse_removal_table(DEFAULT_TABLE.read_text(encoding="utf-8"))
    check(real_count == len(real_rows) and real_count > 0, "the real removal table parses (%d rows)" % real_count)
    check(all(r["locate"] in LOCATE_VIEWS for r in real_rows), "every Locate of the real table has a view")

    # 表の形が崩れたらツールの誤り。
    table_text = DEFAULT_TABLE.read_text(encoding="utf-8")
    broken = [
        ("unknown Locate", table_text.replace("Locate::ObsLogDir, UninstallStep::NotRemoved}", "Locate::Nowhere, UninstallStep::NotRemoved}")),
        ("row count mismatch", table_text.replace("kRemovalItemCount = %d;" % real_count, "kRemovalItemCount = %d;" % (real_count + 1))),
        ("marker not at line start", table_text.replace("\n" + TABLE_BEGIN, "\n  x " + TABLE_BEGIN)),
    ]
    for label, text in broken:
        try:
            parse_removal_table(text)
            check(False, "broken table is refused: " + label)
        except ToolError:
            check(True, "broken table is refused: " + label)

    screen_type = names["sourceTypes"][0]
    user_bg = _src("Background", "color_source", "u-1", settings={"color": 1})
    user_title = _src("Title", "text_gdiplus", "u-2", settings={"text": "Hello"},
                      filters=[{"name": "Color Correction", "id": "color_filter", "settings": {"opacity": 0.9}}])
    user_scene = _src("Opening", "scene", "u-3", settings={"items": [{"name": "Background", "source_uuid": "u-1", "id": 1}],
                                                           "id_counter": 1})
    # cordmux の項目を置かないシーン (id_counter の差が cordmux の物かどうかの対照)。
    plain_scene = _src("Chat", "scene", "u-4", settings={"items": [{"name": "Title", "source_uuid": "u-2", "id": 1}],
                                                         "id_counter": 1})
    base_col = _collection([user_scene, plain_scene, user_bg, user_title], ["Opening", "Chat"])
    ini_a = "\ufeff[General]\nFirstRun=true\n\n[BasicWindow]\ngeometry=AAA\n"
    ini_b = "\ufeff[General]\nFirstRun=true\n\n[BasicWindow]\ngeometry=BBB\n"

    def base_files(ini_text, log_names, collection_obj):
        files = {
            ROOT_OBS_CONFIG + ":basic/scenes/Stream.json": _file_entry("Stream.json", obj=collection_obj),
            ROOT_OBS_CONFIG + ":user.ini": _file_entry("user.ini", text=ini_text),
            ROOT_APPDATA + ":Vesktop/state.json": _file_entry("state.json", obj={"maximized": False}),
            ROOT_APPDATA + ":Vesktop/settings/quickCss.css": _file_entry("quickCss.css", text="/* marker */"),
            ROOT_OBS_PROGRAM + ":bin/64bit/obs64.exe": _file_entry("obs64.exe", raw=b"MZ"),
        }
        for n in log_names:
            files[ROOT_OBS_CONFIG + ":logs/" + n] = _file_entry(n, text="log " + n)
        return files

    # OBS そのものが持つフォルダ (導入の前からある)。
    base_dirs = [ROOT_OBS_CONFIG + ":logs", ROOT_OBS_CONFIG + ":plugin_config", ROOT_OBS_PROGRAM + ":plugins",
                 ROOT_OBS_PROGRAM + ":data"]
    calib1 = _snap(base_files(ini_a, ["log 1.txt"], base_col), dirs=base_dirs)
    calib2 = _snap(base_files(ini_b, ["log 1.txt", "log 2.txt"], base_col), dirs=base_dirs)
    cal = compare("calibration", calib1, calib2, [], [], real_rows, names)
    check(cal["ok"] and cal["wobbleCount"] >= 2, "calibration with only log/geometry churn is green")
    wobble = cal["wobble"]

    # 較正に利用者のソースの差が入ったら不合格。
    dirty_col = copy.deepcopy(base_col)
    dirty_col["sources"][1]["settings"]["color"] = 2
    calib_dirty = _snap(base_files(ini_b, ["log 1.txt"], dirty_col), dirs=base_dirs)
    check(not compare("calibration", calib1, calib_dirty, [], [], real_rows, names)["ok"],
          "calibration touching a user source is red")

    # 見方の決め方の (4): コレクションのバックアップ (.json.bak) だけが保存 1 回ぶん遅れて変わった組は合格
    # (seed の prev_ver: 0 のコピーと、OBS が保存し直したコピー)。本体の .json の汚れは上のとおり不合格のまま。
    seed_col = copy.deepcopy(base_col)
    saved_col = copy.deepcopy(base_col)
    for src in saved_col["sources"]:
        src["prev_ver"] = 520159233
    calib1_bak = copy.deepcopy(calib1)
    calib1_bak["entries"][ROOT_OBS_CONFIG + ":basic/scenes/Stream.json.bak"] = _file_entry("Stream.json.bak", obj=seed_col)
    calib2_bak = copy.deepcopy(calib2)
    calib2_bak["entries"][ROOT_OBS_CONFIG + ":basic/scenes/Stream.json.bak"] = _file_entry("Stream.json.bak", obj=saved_col)
    cal_bak = compare("calibration", calib1_bak, calib2_bak, [], [], real_rows, names)
    check(cal_bak["ok"] and cal_bak["backupChurnCount"] > 0,
          "calibration where only the collection .bak lags one save is green (%s)" % (cal_bak.get("reason"),))

    # 導入して閉じた後 (installed)。
    inst_col = copy.deepcopy(base_col)
    # 利用者のシーン (Opening) に、cordmux のソース (合成のソース) を置いた形。シーンの項目も cordmux の行に数える。
    inst_col["sources"][0]["settings"]["items"] += [
        {"name": "cordmux", "source_uuid": "c-1", "id": 2},
    ]
    # OBS が置いた項目に書く物: 表示・非表示の割り当てと、項目の番号の数えの増え (消しても戻らない)。
    inst_col["sources"][0]["settings"]["id_counter"] = 2
    inst_col["sources"][0]["hotkeys"] = {"libobs.show_scene_item.2": [], "libobs.hide_scene_item.2": []}
    inst_col["sources"] += [
        _src("cordmux", screen_type, "c-1", settings={}),
    ]
    inst_files = base_files(ini_b, ["log 1.txt", "log 2.txt", "log 3.txt"], inst_col)
    inst_files[ROOT_OBS_CONFIG + ":plugin_config/cordmux/settings.json"] = _file_entry("settings.json", obj={"a": 1})
    inst_files[ROOT_LOCALAPPDATA + ":cordmux/session.json"] = _file_entry("session.json", obj={"port": 1})
    inst_files[ROOT_APPDATA + ":Vesktop/state.json"] = _file_entry("state.json", obj={"maximized": False, "vencordDir": "X"})
    inst_files[ROOT_OBS_PROGRAM + ":plugins/cordmux/cordmux.dll"] = _file_entry("cordmux.dll", raw=b"MZ-")
    inst_files[ROOT_OBS_PROGRAM + ":plugins/cordmux/data/locale/ja-JP.ini"] = _file_entry("ja-JP.ini", text="a=b\n")
    inst_files[ROOT_OBS_CONFIG + ":user.ini"] = _file_entry("user.ini", text=ini_b + "DockState=ZZZ\n")
    installed = _snap(inst_files, dirs=base_dirs + [ROOT_OBS_CONFIG + ":plugin_config/cordmux", ROOT_LOCALAPPDATA + ":cordmux",
                                                    ROOT_OBS_PROGRAM + ":plugins/cordmux",
                                                    ROOT_OBS_PROGRAM + ":plugins/cordmux/data",
                                                    ROOT_OBS_PROGRAM + ":plugins/cordmux/data/locale"])
    placed = [{"root": ROOT_OBS_PROGRAM, "rel": "plugins/cordmux"}]
    ac04 = compare("installed", calib1, installed, wobble, placed, real_rows, names)
    check(ac04["ok"], "installed: product rows + placed files + wobble is green (%s)" % (ac04.get("reason"),))
    ac04_nothing = compare("installed", calib1, calib2, wobble, placed, real_rows, names)
    check(not ac04_nothing["ok"], "installed: nothing installed is red")
    stray = copy.deepcopy(installed)
    stray["entries"][ROOT_APPDATA + ":Vesktop/settings/quickCss.css"] = _file_entry("quickCss.css", text="/* changed */")
    check(not compare("installed", calib1, stray, wobble, placed, real_rows, names)["ok"],
          "installed: a changed user setting file is red")

    # 置いた項目の hotkeys は CordmuxSources の行に数える。cordmux の項目でない id (Background の 1) の hotkeys は表の外で不合格。
    src_row = [r for r in ac04["rows"] if r["locate"] == "SourceTypeId"]
    check(ac04["violationCount"] == 0 and src_row and src_row[0]["count"] >= 4 + 2,
          "installed: hotkeys of the placed cordmux items are counted on the CordmuxSources row")
    check(ac04["unrestoredCounterCount"] == 1 and ac04["unrestoredCounter"][0]["name"] == UNRESTORED_COUNTER_NAME,
          "installed: the raised id_counter of the scene holding cordmux items is reported by name, not red")
    other_hot = copy.deepcopy(inst_col)
    other_hot["sources"][0]["hotkeys"]["libobs.show_scene_item.1"] = []
    stray_hot = copy.deepcopy(installed)
    stray_hot["entries"][ROOT_OBS_CONFIG + ":basic/scenes/Stream.json"] = _file_entry("Stream.json", obj=other_hot)
    res_hot = compare("installed", calib1, stray_hot, wobble, placed, real_rows, names)
    check(not res_hot["ok"] and res_hot["violationCount"] == 1, "installed: a hotkey of a non-cordmux item is red")
    # id_counter: cordmux の項目のないシーンで増えた差、減った差は失敗。
    plain_up = copy.deepcopy(inst_col)
    plain_up["sources"][1]["settings"]["id_counter"] = 2
    up = copy.deepcopy(installed)
    up["entries"][ROOT_OBS_CONFIG + ":basic/scenes/Stream.json"] = _file_entry("Stream.json", obj=plain_up)
    res_up = compare("installed", calib1, up, wobble, placed, real_rows, names)
    check(not res_up["ok"] and res_up["violationCount"] == 1, "installed: id_counter raised in a scene without cordmux items is red")
    down_col = copy.deepcopy(inst_col)
    down_col["sources"][0]["settings"]["id_counter"] = 0
    down = copy.deepcopy(installed)
    down["entries"][ROOT_OBS_CONFIG + ":basic/scenes/Stream.json"] = _file_entry("Stream.json", obj=down_col)
    res_down = compare("installed", calib1, down, wobble, placed, real_rows, names)
    check(not res_down["ok"] and res_down["violationCount"] == 1, "installed: id_counter lowered is red")

    # [外す] -> 置いた物を消す -> 起動した後 (uninstalled)。
    after_files = base_files(ini_b, ["log 1.txt", "log 2.txt", "log 4.txt"], base_col)
    after_files[ROOT_OBS_CONFIG + ":user.ini"] = _file_entry("user.ini", text=ini_b + "DockState=ZZZ\n")
    after_ok = _snap(after_files, dirs=base_dirs)
    ac69 = compare("uninstalled", calib1, after_ok, wobble, placed, real_rows, names)
    check(ac69["ok"], "uninstalled: wobble + Nobody rows only is green (%s)" % (ac69.get("reason"),))
    # uninstalled: 項目と hotkeys は消え、id_counter だけが増えたまま残る。導入した後のコピーを渡せば合格 (カウンターを名前付きで残す)、
    # 渡さなければ不合格、cordmux の項目のないシーンで増えたままなら渡しても不合格。
    left_counter = copy.deepcopy(base_col)
    left_counter["sources"][0]["settings"]["id_counter"] = 2
    after_counter_files = dict(after_files)
    after_counter_files[ROOT_OBS_CONFIG + ":basic/scenes/Stream.json"] = _file_entry("Stream.json", obj=left_counter)
    after_counter = _snap(after_counter_files, dirs=base_dirs)
    res_c = compare("uninstalled", calib1, after_counter, wobble, placed, real_rows, names, installed)
    check(res_c["ok"] and res_c["unrestoredCounterCount"] == 1 and res_c["unrestoredCounter"][0]["name"] == UNRESTORED_COUNTER_NAME,
          "uninstalled: id_counter left raised after the cordmux items are gone is reported by name, not red (%s)" % (res_c.get("reason"),))
    check(not compare("uninstalled", calib1, after_counter, wobble, placed, real_rows, names)["ok"],
          "uninstalled: id_counter left raised is red when no installed snapshot is given")
    plain_left = copy.deepcopy(left_counter)
    plain_left["sources"][1]["settings"]["id_counter"] = 2
    after_plain_files = dict(after_files)
    after_plain_files[ROOT_OBS_CONFIG + ":basic/scenes/Stream.json"] = _file_entry("Stream.json", obj=plain_left)
    check(not compare("uninstalled", calib1, _snap(after_plain_files, dirs=base_dirs), wobble, placed, real_rows, names, installed)["ok"],
          "uninstalled: id_counter raised in a scene without cordmux items is red")
    dock = [r for r in ac69["rows"] if r["locate"] == "ObsProfileDockState"]
    check(dock and dock[0]["count"] == 1, "uninstalled: the dock state is reported on its Nobody row")

    # 見方の決め方の (7): OBS 32.x のプラグインの管理のファイル (plugin_manager/modules.json) は、導入で cordmux の
    # 項目が増え、撤去の後も項目が残る。どちらも消さない行に一致して合格。
    pm_path = ROOT_OBS_CONFIG + ":plugin_manager/modules.json"
    pm_dir = ROOT_OBS_CONFIG + ":plugin_manager"
    pm_module = {"module_name": "cordmux", "display_name": "", "id": "", "version": "", "enabled": True,
                 "sources": [screen_type], "outputs": [], "encoders": [], "services": []}
    pm_left = dict(pm_module, sources=[])

    def with_pm(snap, modules):
        out = copy.deepcopy(snap)
        out["entries"][pm_dir] = {"type": "dir"}
        out["entries"][pm_path] = _file_entry("modules.json", obj=modules)
        return out

    calib1_pm = with_pm(calib1, [])
    ac04_pm = compare("installed", calib1_pm, with_pm(installed, [pm_module]), wobble, placed, real_rows, names)
    pm_rows = [r for r in ac04_pm["rows"] if r["locate"] == "ObsPluginManagerFile"]
    check(ac04_pm["ok"] and pm_rows and pm_rows[0]["count"] > 0,
          "installed: the OBS plugin manager record is on its Nobody row (%s)" % (ac04_pm.get("reason"),))
    ac69_pm = compare("uninstalled", calib1_pm, with_pm(after_ok, [pm_left]), wobble, placed, real_rows, names)
    pm_rows = [r for r in ac69_pm["rows"] if r["locate"] == "ObsPluginManagerFile"]
    check(ac69_pm["ok"] and pm_rows and pm_rows[0]["count"] > 0,
          "uninstalled: the OBS plugin manager record left after uninstall is on its Nobody row (%s)" % (ac69_pm.get("reason"),))
    # 割り当ては LOCATE_ORDER だけを回るので、見方があっても並びに無ければ一度も当たらない。
    check(sorted(LOCATE_ORDER) == sorted(LOCATE_VIEWS), "LOCATE_ORDER lists every view exactly once")

    lost_user = copy.deepcopy(after_ok)
    col = copy.deepcopy(base_col)
    col["sources"] = [s for s in col["sources"] if s["name"] != "Title"]
    lost_user["entries"][ROOT_OBS_CONFIG + ":basic/scenes/Stream.json"] = _file_entry("Stream.json", obj=col)
    check(not compare("uninstalled", calib1, lost_user, wobble, placed, real_rows, names)["ok"],
          "uninstalled: a user source removed is red")

    left_cordmux = copy.deepcopy(after_ok)
    col = copy.deepcopy(base_col)
    col["sources"].append(_src("cordmux 2", screen_type, "c-9", settings={}))
    left_cordmux["entries"][ROOT_OBS_CONFIG + ":basic/scenes/Stream.json"] = _file_entry("Stream.json", obj=col)
    res = compare("uninstalled", calib1, left_cordmux, wobble, placed, real_rows, names)
    row_a = [r for r in res["rows"] if r["locate"] == "SourceTypeId"]
    check(not res["ok"] and row_a and row_a[0]["count"] > 0, "uninstalled: a cordmux source left is red on its row")

    left_file = copy.deepcopy(after_ok)
    left_file["entries"][ROOT_OBS_PROGRAM + ":plugins/cordmux/cordmux.dll"] = _file_entry("cordmux.dll", raw=b"MZ-")
    check(not compare("uninstalled", calib1, left_file, wobble, placed, real_rows, names)["ok"],
          "uninstalled: a placed file left is red")

    missing_root = copy.deepcopy(after_ok)
    missing_root["roots"][ROOT_OBS_CONFIG]["exists"] = False
    check(not compare("uninstalled", calib1, missing_root, wobble, placed, real_rows, names)["ok"],
          "uninstalled: a snapshot without the OBS config is not green")
    missing_appdata = copy.deepcopy(after_ok)
    missing_appdata["roots"][ROOT_APPDATA]["exists"] = False
    check(not compare("uninstalled", calib1, missing_appdata, wobble, placed, real_rows, names)["ok"],
          "uninstalled: a snapshot without the fake APPDATA is not green (the vencordDir row would see nothing)")

    # 見方の決め方の (4): [外す] の後、バックアップ (.bak) だけが 1 回前の保存 (cordmux のソース入り) を持つ組は
    # 合格 (本体の .json に cordmux のソースが残った組は上のとおり不合格)。
    bak_left = copy.deepcopy(after_ok)
    bak_left["entries"][ROOT_OBS_CONFIG + ":basic/scenes/Stream.json.bak"] = _file_entry("Stream.json.bak", obj=inst_col)
    res_bak = compare("uninstalled", calib1, bak_left, wobble, placed, real_rows, names)
    check(res_bak["ok"] and res_bak["backupChurnCount"] > 0,
          "uninstalled: only the collection .bak still holding the previous save is green (%s)" % (res_bak.get("reason"),))

    # ini は utf-8-sig で読み、キーの単位で比べる。
    fmt, flat = content_of("user.ini", ini_a.encode("utf-8"))
    check(fmt == "ini" and flat.get(seg_key(["[General]", "FirstRun"])) == "true", "ini with a BOM parses key by key")

    # extract: 日本語の名前 (UTF-8 フラグ) と 1 つのフォルダーの除去、危ない名前の拒否。一時の保存先は
    # 呼び出し元の TEMP の下に作って消す (seed-config.py の selftest_place_problem と同じ)。
    import io
    import shutil
    import tempfile
    import zipfile
    temp_dir = tempfile.gettempdir()
    temp_root = Path(tempfile.mkdtemp(prefix="snapshot-selftest-"))
    # 展開先の守り (extract_zip) は名前が _work のフォルダの下を求めるので、作業フォルダの中に _work を作り、その中で実行する。
    tmp = temp_root / WORK_DIR_MARKER
    tmp.mkdir()
    print("selftest work: " + str(tmp))
    try:
        # (0) 保存先の守り。実際の保存先は TEMP の下で _work の外。前の保存先 (_work の下) と、名前が TEMP で始まる隣は断る。
        check(selftest_place_problem(tmp, temp_dir, REPO_ROOT) is None, "the work folder is under the caller's TEMP and outside _work")
        check(selftest_place_problem(REPO_ROOT / "_work" / "snapshot-selftest-x", temp_dir, REPO_ROOT) is not None,
              "the old place under _work is refused")
        check(selftest_place_problem(Path(temp_dir + "2") / "x", temp_dir, REPO_ROOT) is not None,
              "a sibling folder whose name starts with TEMP is not under TEMP")
        # 下の「_work の外は断る」の外の場所は作業フォルダの中の _work の隣。TEMP 自身が _work の下だと外を作れない。
        outside = temp_root / "outside"
        check(WORK_DIR_MARKER not in outside.resolve().parts,
              "the outside folder has no _work part (TEMP itself must not be under a _work folder)")
        jp_name = "cordmux-0.1.0/\u306f\u3058\u3081\u306b.txt"
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as zf:
            zf.writestr(jp_name, "hello")
            zf.writestr("cordmux-0.1.0/cordmux/cordmux.dll", b"MZ")
        zpath = tmp / "a.zip"
        zpath.write_bytes(buf.getvalue())
        r = extract_zip(zpath, tmp / "keep", False)
        check(r["files"] == 2 and (tmp / "keep" / "cordmux-0.1.0" / "\u306f\u3058\u3081\u306b.txt").is_file(),
              "extract keeps the UTF-8 Japanese file name")
        r = extract_zip(zpath, tmp / "strip", True)
        check((tmp / "strip" / "cordmux" / "cordmux.dll").is_file() and r["stripped"] == "cordmux-0.1.0/",
              "extract strips a single top folder when asked")
        evil = io.BytesIO()
        with zipfile.ZipFile(evil, "w") as zf:
            zf.writestr("../evil.txt", "x")
        epath = tmp / "evil.zip"
        epath.write_bytes(evil.getvalue())
        try:
            extract_zip(epath, tmp / "evil", False)
            check(False, "extract refuses .. in a name")
        except ToolError:
            check(not (tmp / "evil.txt").exists(), "extract refuses .. in a name")
        try:
            extract_zip(zpath, outside, False)
            check(False, "extract refuses a destination outside _work")
        except ToolError:
            check(not outside.exists(), "extract refuses a destination outside _work")
    finally:
        # 自分が mkdtemp で作った作業フォルダーだけを消す (seed-config・border_timing のセルフテストと同じ扱い)。
        shutil.rmtree(temp_root, ignore_errors=True)

    print("selftest: %d failure(s)" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
