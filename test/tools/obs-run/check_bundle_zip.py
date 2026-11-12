# 判定スクリプト: 診断情報の書き出しが作った zip の中身を確認する。
#
# 実行スクリプト (--dock-scenario export-bundle) が、zip が実際に開けて 6 つの中身
# (src-obs/bundle/bundle_files.h の kManifestName / kPluginLogName / kPluginStatsLogName /
# kDiscordLogName / kDiscordStatsLogName / kSettingsName) が揃っており、トークンが混ざって
# いないことを、人手で zip を開かずに判定するために使う。
#
# トークン (secret) の形は 64 桁の 16 進数 (session.json の secret と ws/handshake のトークンと同じ形。
# seed-config.py が書く形と同じ)。settings.json だけでなく 6 ファイルすべてに同じ正規表現を使う。
# export_bundle.cpp の bundle_reveals_secret は書き出す前に全文を見張っているので、通常はどの
# ファイルにも出ないが、settings.json 以外に紛れていないかも見ておくと、このチェック自体の
# 見落としに気付きやすい。
#
# 使い方: python check_bundle_zip.py <zip のパス> [--session-json <session.json のパス>]
#
# --session-json で製品が書いた session.json を渡せば、その中の secret そのものを探す (これが本来のチェック)。
# 渡さないとき (または読めないとき) は、64 桁の 16 進数の形で探す。
import json
import os
import re
import sys
import zipfile

EXPECTED_NAMES = ["manifest.json", "plugin.log", "plugin-stats.log", "discord.log", "discord-stats.log", "settings.json"]
SECRET_PATTERN = re.compile(r"\b[0-9a-fA-F]{64}\b")


def read_secret(session_json_path):
    """session.json の secret を返す。ない・読めない・形が違うときは None。"""
    if not session_json_path or not os.path.isfile(session_json_path):
        return None
    try:
        with open(session_json_path, encoding="utf-8") as f:
            value = json.load(f).get("secret")
    except (OSError, ValueError):
        return None
    if isinstance(value, str) and SECRET_PATTERN.fullmatch(value):
        return value
    return None


def main():
    argv = sys.argv[1:]
    session_json_path = None
    if "--session-json" in argv:
        i = argv.index("--session-json")
        session_json_path = argv[i + 1] if i + 1 < len(argv) else None
        del argv[i:i + 2]
    path = argv[0]
    out = {"file": path, "ok": False}
    secret = read_secret(session_json_path)
    out["secretSource"] = "session.json" if secret else "pattern"

    try:
        zf = zipfile.ZipFile(path)
    except (FileNotFoundError, zipfile.BadZipFile) as e:
        out["error"] = "cannot open zip: %s" % e
        print(json.dumps(out))
        sys.exit(1)

    with zf:
        names = zf.namelist()
        missing = [n for n in EXPECTED_NAMES if n not in names]
        out["names"] = names
        out["missing"] = missing

        secret_found_in = []
        for name in EXPECTED_NAMES:
            if name not in names:
                continue
            data = zf.read(name)
            text = data.decode("utf-8", errors="replace")
            if secret is not None:
                hit = secret in text
            else:
                hit = SECRET_PATTERN.search(text) is not None
            if hit:
                secret_found_in.append(name)
        out["secretFoundIn"] = secret_found_in
        out["settingsHasSecret"] = "settings.json" in secret_found_in
        out["ok"] = len(missing) == 0 and len(secret_found_in) == 0

    print(json.dumps(out))
    sys.exit(0 if out["ok"] else 1)


if __name__ == "__main__":
    main()
