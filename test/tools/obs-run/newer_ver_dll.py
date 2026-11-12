#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# 製品の cordmux.dll より新しい libobs を要求する場合の確認用に、テスト用の DLL を作るスクリプト。
# 製品の DLL のコピーのうち、書き出された関数 obs_module_ver() が返すバージョンの数だけを新しくする
# (ほかのバイトは 1 つも変えない)。
#
# 作り直さずにコピーのバイトを書き換える。理由は次のとおり。
#   (1) 「obs_module_ver() だけが新しい DLL」を満たすのは、製品の DLL と 1 か所しか違わないコピーだけである
#       (別の小さなモジュールを作ると、名前・読み込む DLL・ログがすべて違ってしまう)。
#   (2) 製品のソース (src-obs/) を変えずに済む (OBS_DECLARE_MODULE の obs_module_ver は
#       libobs のヘッダーの LIBOBS_API_VER を返すだけで、ビルドの定義では差し替えられない)。
#   (3) CPU だけで数秒の処理なので、実機の実行の前にいつでも作り直せる。
# 関数の中身は MSVC の最適化で `mov eax, imm32; ret` (B8 xx xx xx xx C3) になる
# (src-obs/build_x64/RelWithDebInfo/cordmux.dll で実測: B8 01 00 01 1F C3 = 31.1.1)。
# 増分リンクの飛び先 (x86 の相対ジャンプ命令) は辿る。形が違えば書き換えずに止まる (黙って別のバイトを
# 変えない)。バージョンの数は libobs の MAKE_SEMANTIC_VERSION (major << 24 | minor << 16 | patch)。
#
# 使い方:
#   python newer_ver_dll.py --in <cordmux.dll> --out <コピーの保存先>/cordmux.dll [--ver 34.0.0]
#          [--expect-ver <いま返しているバージョン。既定は src-obs/buildspec.json の obs-studio のバージョン>]
#   python newer_ver_dll.py --in <cordmux.dll> --check-only     (書かずに、見つけたバージョンを出すだけ)
#   python newer_ver_dll.py --selftest                          (手で作った PE で試す。何も書かない)
# 結果は末尾の 1 行 `CORDMUX_AC07_DLL_JSON:<JSON>`。終了コード 0 = できた、1 = 形が違う・バージョンが違う、
# 2 = 引数の誤り。
import argparse
import json
import struct
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
BUILDSPEC = REPO_ROOT / "src-obs" / "buildspec.json"
RESULT_PREFIX = "CORDMUX_AC07_DLL_JSON:"
EXPORT_NAME = "obs_module_ver"
DEFAULT_NEW_VER = "34.0.0"
MAX_THUNK_HOPS = 4


class PatchError(Exception):
    # 形が違う・バージョンが違う (終了コード 1)。
    pass


def semantic_version(text):
    parts = [int(p) for p in text.split(".")]
    if len(parts) != 3 or any(p < 0 or p > 255 for p in parts[:2]) or parts[2] < 0 or parts[2] > 65535:
        raise ValueError("version must be major.minor.patch: " + text)
    return (parts[0] << 24) | (parts[1] << 16) | parts[2]


def version_text(value):
    return "%d.%d.%d" % (value >> 24, (value >> 16) & 0xFF, value & 0xFFFF)


def sections_of(data):
    if data[:2] != b"MZ":
        raise PatchError("not an MZ image")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise PatchError("PE signature not found")
    count = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic != 0x20B:
        raise PatchError("not a PE32+ (64-bit) image")
    export_rva, _ = struct.unpack_from("<II", data, opt + 112)
    sections = []
    table = opt + opt_size
    for i in range(count):
        _, vsize, va, rsize, rptr = struct.unpack_from("<8sIIII", data, table + 40 * i)
        sections.append((va, max(vsize, rsize), rptr, rsize))
    return export_rva, sections


def rva_to_offset(sections, rva):
    for va, size, rptr, rsize in sections:
        if va <= rva < va + size and rva - va < rsize:
            return rva - va + rptr
    raise PatchError("RVA 0x%x is not in any section" % rva)


def find_export(data, name):
    export_rva, sections = sections_of(data)
    if export_rva == 0:
        raise PatchError("no export directory")
    base = rva_to_offset(sections, export_rva)
    # IMAGE_EXPORT_DIRECTORY (40 バイト): 特性・時刻・バージョン 2 つ・名前・序数の基点・関数の数・名前の数・3 つの表。
    _, _, _, _, _, _, nfun, nnames, afun, anames, aord = struct.unpack_from("<IIHHIIIIIII", data, base)
    for i in range(nnames):
        name_rva = struct.unpack_from("<I", data, rva_to_offset(sections, anames) + 4 * i)[0]
        off = rva_to_offset(sections, name_rva)
        end = data.index(b"\0", off)
        if data[off:end].decode("ascii", "replace") == name:
            ordinal = struct.unpack_from("<H", data, rva_to_offset(sections, aord) + 2 * i)[0]
            fun_rva = struct.unpack_from("<I", data, rva_to_offset(sections, afun) + 4 * ordinal)[0]
            return fun_rva, sections
    raise PatchError("export not found: " + name)


def locate_immediate(data):
    # obs_module_ver の `mov eax, imm32; ret` の imm32 のファイル上の位置と、現在の値。
    rva, sections = find_export(data, EXPORT_NAME)
    for _ in range(MAX_THUNK_HOPS + 1):
        off = rva_to_offset(sections, rva)
        if data[off] == 0xE9:  # 増分リンクの飛び先 (jmp rel32)
            rel = struct.unpack_from("<i", data, off + 1)[0]
            rva = rva + 5 + rel
            continue
        if data[off] == 0xB8 and data[off + 5] == 0xC3:
            return off + 1, struct.unpack_from("<I", data, off + 1)[0]
        raise PatchError("unexpected code at %s: %s" % (EXPORT_NAME, data[off:off + 8].hex()))
    raise PatchError("too many jump thunks before " + EXPORT_NAME)


def patch_bytes(data, new_ver, expect_ver):
    pos, current = locate_immediate(data)
    if expect_ver is not None and current != expect_ver:
        raise PatchError("%s returns %s, expected %s" % (EXPORT_NAME, version_text(current), version_text(expect_ver)))
    out = bytearray(data)
    struct.pack_into("<I", out, pos, new_ver)
    return bytes(out), current


def buildspec_obs_version():
    try:
        spec = json.loads(BUILDSPEC.read_text(encoding="utf-8"))
        return spec["dependencies"]["obs-studio"]["version"]
    except (OSError, KeyError, ValueError):
        return None


def emit(result):
    print(RESULT_PREFIX + json.dumps(result, ensure_ascii=False))


def main(argv):
    if argv and argv[0] == "--selftest":
        return selftest()
    parser = argparse.ArgumentParser()
    parser.add_argument("--in", dest="src", required=True)
    parser.add_argument("--out", default=None)
    parser.add_argument("--ver", default=DEFAULT_NEW_VER)
    parser.add_argument("--expect-ver", default=None)
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args(argv)
    try:
        new_ver = semantic_version(args.ver)
        expect_text = args.expect_ver or buildspec_obs_version()
        expect_ver = semantic_version(expect_text) if expect_text else None
    except ValueError as e:
        emit({"ok": False, "error": str(e)})
        return 2
    src = Path(args.src)
    try:
        data = src.read_bytes()
        if args.check_only:
            _, current = locate_immediate(data)
            emit({"ok": True, "in": str(src), "current": version_text(current)})
            return 0
        if not args.out:
            emit({"ok": False, "error": "--out is required unless --check-only"})
            return 2
        out_path = Path(args.out)
        if out_path.resolve() == src.resolve():
            emit({"ok": False, "error": "--out must differ from --in (the input is never modified)"})
            return 2
        patched, current = patch_bytes(data, new_ver, expect_ver)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_bytes(patched)
        emit({"ok": True, "in": str(src), "out": str(out_path), "from": version_text(current),
              "to": version_text(new_ver), "changedBytes": sum(1 for a, b in zip(data, patched) if a != b)})
        return 0
    except (OSError, PatchError) as e:
        emit({"ok": False, "in": str(src), "error": str(e)})
        return 1


# ---- セルフテスト: 最小の PE32+ を手で作る (ファイルは書かない) --------------------------


def build_fake_pe(code_at_export, extra_code=b""):
    # .text 1 つに書き出しの表と関数を置いた最小の PE32+。code_at_export は書き出しの先のバイト。
    file_align = 0x200
    sect_rva = 0x1000
    text = bytearray(0x400)
    # 関数本体 (0x000) と追加のコード (0x40)。
    text[0:len(code_at_export)] = code_at_export
    text[0x40:0x40 + len(extra_code)] = extra_code
    # 書き出しの表 (0x100): 名前 1 つ。
    exp = 0x100
    names = exp + 40
    funcs = names + 4
    ords = funcs + 4
    name_str = ords + 4
    dll_name = name_str + len(EXPORT_NAME) + 1
    struct.pack_into("<IIHHIIIIIII", text, exp, 0, 0, 0, 0, sect_rva + dll_name, 1, 1, 1,
                     sect_rva + funcs, sect_rva + names, sect_rva + ords)
    struct.pack_into("<I", text, names, sect_rva + name_str)
    struct.pack_into("<I", text, funcs, sect_rva + 0)
    struct.pack_into("<H", text, ords, 0)
    text[name_str:name_str + len(EXPORT_NAME)] = EXPORT_NAME.encode("ascii")
    text[dll_name:dll_name + 7] = b"x.dll\0\0"

    headers = bytearray(file_align)
    headers[0:2] = b"MZ"
    pe = 0x80
    struct.pack_into("<I", headers, 0x3C, pe)
    headers[pe:pe + 4] = b"PE\0\0"
    opt_size = 240
    struct.pack_into("<HHIIIHH", headers, pe + 4, 0x8664, 1, 0, 0, 0, opt_size, 0x2022)
    opt = pe + 24
    struct.pack_into("<H", headers, opt, 0x20B)
    struct.pack_into("<II", headers, opt + 112, sect_rva + exp, 40)
    sect = opt + opt_size
    struct.pack_into("<8sIIIIIIHHI", headers, sect, b".text\0\0\0", len(text), sect_rva, len(text), file_align,
                     0, 0, 0, 0, 0x60000020)
    return bytes(headers) + bytes(text)


def selftest():
    failures = []

    def check(cond, label):
        if not cond:
            failures.append(label)
        print(("ok   " if cond else "FAIL ") + label)

    v3111 = semantic_version("31.1.1")
    v33 = semantic_version("33.0.0")
    direct = build_fake_pe(b"\xB8" + struct.pack("<I", v3111) + b"\xC3")
    patched, current = patch_bytes(direct, v33, v3111)
    check(current == v3111 and locate_immediate(patched)[1] == v33, "direct mov/ret is patched to 33.0.0")
    check(sum(1 for a, b in zip(direct, patched) if a != b) <= 4 and len(direct) == len(patched),
          "only the immediate bytes change")
    try:
        patch_bytes(direct, v33, semantic_version("32.2.2"))
        check(False, "a different current version is refused")
    except PatchError:
        check(True, "a different current version is refused")
    # 増分リンクの飛び先: 0x000 に jmp rel32 で 0x40 へ。
    thunk = build_fake_pe(b"\xE9" + struct.pack("<i", 0x40 - 5), b"\xB8" + struct.pack("<I", v3111) + b"\xC3")
    patched, _ = patch_bytes(thunk, v33, v3111)
    check(locate_immediate(patched)[1] == v33, "an incremental-link thunk is followed")
    odd = build_fake_pe(b"\x48\x8B\x05\x00\x00\x00\x00\xC3")
    try:
        patch_bytes(odd, v33, None)
        check(False, "unexpected code is refused")
    except PatchError:
        check(True, "unexpected code is refused")
    check(version_text(semantic_version("32.2.2")) == "32.2.2", "version round trip")
    print("selftest: %d failure(s)" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
