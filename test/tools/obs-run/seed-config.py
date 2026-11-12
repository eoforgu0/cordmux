# ポータブル OBS のテスト用の設定を作る。利用者の実際の OBS には触れない。
# 使い方: python seed-config.py <OBS のフォルダー (中に bin/ data/ がある。OBS 33 以上のポータブル版)> <録画の出力先フォルダー>
#   --step <run-window.mjs の --step の名前>
#   (--appdata <APPDATA の差し替え先> | --real-profile)   どちらか 1 つだけ
#   [--shape test|user]
#   [--dock-install elsewhere|missing|ours|absent]
#   [--vencord-dist <_work/vencord-dist に相当する場所>] [--assign-mode manual|auto]
#   [--decoder software|hardware] [--allow-state-json-write]
#   [--record-dir <cordmux の録画の保存先>] [--record-space-check-interval-s <秒>]
#   [--record-space-warn-minutes <分>]
#   [--vencord-check-disconnected-s <秒>] [--vencord-check-connected-s <秒>]
#   [--vencord-state absent|custom] [--custom-vencord-dir <自作の Vencord の場所>]
#   [--config-dir <OBS の設定の保存先> --no-portable-mode-file --vm]
#   [--replay-buffer] [--label-em-ratio-pct <1.0 から 8.5、0.1 刻み>] (--shape test だけ)
#   [--allow-outside-work] (OBS のフォルダーを repo の _work の外、出力先を _work と build_x64 の外でも受ける。
#                           指定がなければ、外は終了コード 5 で何も書かずに止まる。work_path_problems を参照)
#   [--dry-run]
#   python seed-config.py --selftest
#
# 「プラグインのファイルを置く」と「偽の state.json を作る」は別の仕事である。
#   - プラグインのファイル (data/vencord と data/vencord-src) は、--dock-install が ours / missing なら必ず置く
#     (--appdata の有無と関係ない)。
#   - 偽の state.json は --appdata を渡したときだけ作る。4 アカウントの通話の実行 (call-*) は
#     --appdata を渡さないので、実際の state.json を書くのは製品の [Discord 側につなぐ] だけになる。
#   - settings.json へ書くキーは assign_mode / allow_state_json_write / decoder の 3 つと、
#     録画の 3 つ (下を参照)。
#
# テスト用の送信側だけの実行では、録画の保存先を必ず作業用のフォルダーの下へ差し替える。
# 差し替えないと、cordmux の既定 (OBS の録画の保存先) に従って、利用者の実際の録画フォルダーへ
# 1 時間で 30 GB を書くことになる。
# そのため record_dir は常に書く (--record-dir がなければ <録画の出力先フォルダー>/recordings)。
# さらに、その場所が `_work` の下でなければ、このツールは何も書かずに終了コード 3 で止まる。
# 3 にしたのは argparse と分けるため。引数の綴りを間違えたときの argparse も 2 で終わるので、2 のままでは
# 呼び出し側から「守りに掛かった」のか「呼び方を間違えた」のかが見分けられない。空き容量の監視の 2 キー
# (record_space_check_interval_s / record_space_warn_minutes) は渡されたときだけ書く
# (短い実行でも警告を起こせるようにするため。既定の周期 5 分・残り 30 分は
# 短い実行では一度も発火しない)。
#
# 利用者の実際の state.json を実機の実行で書かない守りは、製品の既定ではなくこのツールの既定にある
# (製品の allow_state_json_write の既定は true)。このツールの規則:
#   - `--step` を必ず受け取り、`--appdata` と `--real-profile` のどちらか 1 つだけを必ず受け取る
#     (APPDATA を差し替える実行かどうかを呼び出し行に明示させる。両方も、どちらもなしも受け付けない)。
#   - `--real-profile` の実行は、call-* の step 以外で必ず allow_state_json_write=false を書く。
#   - `--allow-state-json-write` を受け付けるのは call-* だけ。call-* は `--real-profile` だけ
#     (実際の保存先で実行する)。install-roundtrip は `--appdata` だけ。
#   - `--appdata` の実行は true を書く。偽の state.json にしか届かない (run-window.mjs が子プロセスの
#     APPDATA を差し替える) ので書いてよく、製品の既定と同じ値にする
#     (assign_mode / decoder と同じ理由。実行スクリプトが製品の既定を変えて実行すると、実機で見たものが
#     利用者の見るものと違ってしまう)。
#   - 守りに掛かったら何も書かずに終了コード 4 で止まる (3 は録画の保存先の守り、2 は
#     argparse。呼び出し側が 3 つを見分けられるように分けた)。5 は OBS のフォルダーと出力先の
#     保存先の守り (work_path_problems。同じく見分けられるように分けた)。
#   - 書いた値は末尾の CORDMUX_SEED_CONFIG_JSON: の行に出す。`--dry-run` は 1 バイトも書かずに
#     同じ行 (書くはずの値) を出す (run-window.mjs の --dry-run がこれを実行計画と一緒に出す)。
#
# test のシーンコレクションは空の Main シーンだけを持つ。cordmux のソース (合成のソース cordmux) は
# 製品がいつも置き、実行スクリプト (run-window.mjs) が実行時に Main へ配線するので、ここでは何も置かない。
#
# `--shape user` は導入の往復 (--step install-roundtrip) で使う「利用者らしい設定」を作る。
# コレクション 2 つ (Stream / Chat)・利用者のソース (色・テキスト・メディア)・
# フィルター (色補正)・音量 (0.5)・プロファイル 1 つ (Stream。x264・30fps・単純出力)。
# cordmux のものは 1 つも置かない (cordmux のシーンも settings.json も置かない。導入前の
# 利用者と同じにしないと、導入と撤去の差分が成り立たない)。cordmux の settings.json を書くのは
# 周期の 2 キーを渡した場合と、`--real-profile` の場合 (allow_state_json_write=false を
# 必ず書く。往復は `--appdata` だけなので、導入と撤去の差分には影響しない) だけで、そのときも
# allow_state_json_write と record_dir は必ず書く。偽の APPDATA には Vesktop の state.json・Vesktop 自身の settings.json・
# Vencord の settings/settings.json と settings/quickCss.css (目印入り) を置く。
# `--vencord-state custom` は自作の Vencord の場所 (`--custom-vencord-dir`。4 ファイルの
# 目印を置く) を vencordDir に書く。名前の綴り (Stream / Chat 等) は、文字列リテラルに日本語を書かない
# 方針に合わせて ASCII にした。
#
# `--layout programdata` (VM だけ) は、ポータブルでない OBS の設定の保存先
# (`--config-dir`) へ書き、portable_mode.txt を置かない。`--config-dir` は `--vm` と一緒でなければ
# 受け付けず、保存先が既にあって空でなければ何も書かずに止まる (VM でない PC では利用者の実際の
# %APPDATA%\obs-studio に当たるため。まっさらな VM ではない)。
#
# 作るもの (--shape test):
#   <obs>/portable_mode.txt                       ポータブル判定 (--portable と二重にしておく)
#   <obs>/config/obs-studio/global.ini, user.ini  初回ウィザードと更新確認と終了確認を止める
#   <obs>/config/obs-studio/basic/profiles/test/   1080p60、詳細出力、NVENC H.264、mkv、音声 48kHz
#   <obs>/config/obs-studio/basic/scenes/test.json Main シーン (Desktop Audio と Mic を作らせない)
#   <obs>/config/obs-studio/plugin_config/obs-websocket/config.json  4460 番、認証なし (127.0.0.1 だけ)
#   <obs>/config/obs-studio/plugin_config/cordmux/settings.json      cordmux の設定 (assign_mode /
#                                                  allow_state_json_write / decoder と record_dir)
#   <録画の出力先フォルダー>/recordings            cordmux の録画の保存先
#
# 作るもの (--appdata を渡したときだけ):
#   <appdata>/Vesktop/state.json                  偽の Vesktop の state.json (--dock-install の
#                                                  値に応じて vencordDir を作る)
#   <obs>/plugins/cordmux/data/vencord             プラグインのファイルを Vesktop に見せる場所 (--dock-install
#                                                  ours のときだけ 4 ファイルを置く。missing は
#                                                  フォルダーだけ作り 4 ファイルは置かない)
#   <obs>/plugins/cordmux/data/vencord-src         プラグインのファイルの原本 (--dock-install ours/missing の
#                                                  ときだけ 4 ファイルを置く。ハッシュ照合の
#                                                  基準。src-obs/platform/bundle_check.h)
# 利用者の実際の %APPDATA%\Vesktop には一切触れない (--appdata は run-window.mjs が
# _work/appdata を渡す)。
import argparse
import json
import os
import re
import shutil
import sys
import uuid
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]

# src-obs/platform/bundle_check.h の kVencordBundleFiles と同じ 4 つ (並びも同じ)。
# 照合するのはこの 4 ファイルだけで package.json は照合しない (bundle_check.h を参照)。
VENCORD_BUNDLE_FILES = [
    "vencordDesktopMain.js",
    "vencordDesktopPreload.js",
    "vencordDesktopRenderer.js",
    "vencordDesktopRenderer.css",
]

# 照合には使わないが、dist にあれば一緒に置くファイル。
# 4 ファイルだけの形で「有効」と見なされるかは、置いてある package.json を一時的に退かして確認する。
# 退かすには先に置いてある必要がある。
VENCORD_OPTIONAL_FILES = ["package.json"]

DOCK_INSTALL_VALUES = ["elsewhere", "missing", "ours", "absent"]

# 製品が plugin_config/cordmux に保存し、起動し直しても戻す状態のファイル。実行の始めに消して、前の実行の人と枠を持ち越さない。
#   seen_people.json       これまでに見た人の一覧。残ると前の実行の人がドックの行に出る
#   record_pending_dirs.json  録画の保存先の探索の候補。残ると前の実行の保存先を拾う
# 消さないもの: vencord_dir_backup.json (Vesktop の vencordDir のバックアップ。実行の途中で止まったとき、cleanup.ps1 が戻すのに使う)。
STALE_STATE_FILES = ["seen_people.json", "record_pending_dirs.json"]

# デコードの種別 (src-obs/config/settings.h の decoder)。software と hardware の 2 通りで測り、
# 切り替えは OBS の起動し直しで行う (decoder はモジュールの読み込み時に 1 回読むだけ)。
DECODER_VALUES = ["software", "hardware"]

# 誰を枠に入れるか (src-obs/config/settings.h の assign_mode)。既定は manual で、
# 製品の既定と同じにしてある (実行スクリプトが製品の既定を変えて実行すると、実機で見たものが
# 利用者の見るものと違ってしまう)。--assign-mode auto を渡した実行だけが自動になる。
ASSIGN_MODE_VALUES = ["manual", "auto"]

# 実際の保存先で実行する step (run-window-plan.mjs の CALL_STEP_VALUES と同じ 6 つ)。
CALL_STEPS = ["call-gate-a", "call-gate-b", "call-gate-c", "call-gate-d", "call-1", "call-hold"]
# 導入の往復。--appdata だけ・--shape user だけ。
ROUNDTRIP_STEP = "install-roundtrip"

SHAPE_VALUES = ["test", "user"]
VENCORD_STATE_VALUES = ["absent", "custom"]

# 終了コード (上の説明を参照)。
EXIT_RECORD_DIR_GUARD = 3
EXIT_STATE_JSON_GUARD = 4
# OBS のフォルダーと出力先の保存先の守り (下の work_path_problems を参照)。3 と 4 と同じく
# 呼び出し側が「どの守りに掛かったか」を見分けられるよう、別の値にした。
EXIT_WORK_PATH_GUARD = 5

# 製品の周期のキーの範囲 (src-obs/config/settings.h の vencord_check_*_s。1 から 86400)。
VENCORD_CHECK_MIN_S = 1
VENCORD_CHECK_MAX_S = 86400

# cordmux のシーンの印 (非公開の設定のキー。src-obs/sources/scene_names.h の kSceneRoleKey)。利用者らしい設定に cordmux のものがないことの確認にだけ使う。
SCENE_ROLE_KEY = "cordmux_role"

# 利用者らしい設定 (--shape user) の名前 (綴りは上の説明を参照)。
USER_PROFILE_NAME = "Stream"
USER_COLLECTION_MAIN = "Stream"
USER_COLLECTION_SECOND = "Chat"
USER_SOURCE_VOLUME = 0.5
# 目印 (Vencord の設定と quickCss に入れる。前後のハッシュで「壊れていない」を見る)。
USER_SETTINGS_MARKER = "cordmux-roundtrip-marker"

# OBS の言語と見た目のキーの綴りと範囲。OBS 31.1.1 のソース (src-obs/.deps/obs-studio-31.1.1/frontend) で
# 確認した。言語は OBSApp.cpp 595 行の config_get_string(userConfig, "General", "Language")、見た目は
# OBSApp.cpp 302-303 行の既定 (FontScale 10・Density 1) と settings/OBSBasicSettings_Appearance.cpp 117-123 行の書き込み
# (Appearance の Theme・FontScale・Density)。フォントサイズの範囲は forms/OBSBasicSettings.ui の appearanceFontScale
# (8 から 12)。密度の id は QButtonGroup が自動で振る -2 から -5 (ボタン 4 つ) と既定の 1 (OBSApp_Themes.cpp の
# getPaddingForDensityId が -2..-5 を読み、ほかは既定の余白)。OBS 31 から言語と見た目は user.ini に移った
# (global.ini は古いバージョンのために同じ内容を書く。2 つに同じ中身を書く)。
OBS_FONT_SCALE_MIN = 8
OBS_FONT_SCALE_MAX = 12
OBS_FONT_SCALE_DEFAULT = 10
OBS_DENSITY_VALUES = [1, -2, -3, -4, -5]
OBS_DENSITY_DEFAULT = 1
# [ドック] の [フルハイト ドック] (--full-height-docks on|off)。user.ini の [BasicWindow] SideDocks で、OBS は起動のときに読んで
# ドックの領域の角を決める (widgets/OBSBasic.cpp の config_get_bool(App()->GetUserConfig(), "BasicWindow", "SideDocks"))。
# OBS 33.0.1 の既定は true (OBSApp.cpp の config_set_default_bool。31.1.1 には既定の行がなく false)。on は左右の領域が主ウィンドウの
# 上端から下端まで、off は上下の領域が主ウィンドウの幅いっぱいになる。README の画像で、右にドッキングした cordmux のドックを
# コントロールのドックの上に置くために使う (test/tools/obs-run/readme-shot.ps1)。
OBS_FULL_HEIGHT_DOCKS_VALUES = ["on", "off"]
# HDR。色の形式と色空間の値は settings/OBSBasicSettings.cpp 998-1017 行の addItem の data。
# NVENC の H.264 は 10 ビットをエンコードできず (plugins/obs-nvenc/nvenc.c の init_encoder: 10bitUnsupported、
# 8 ビットで 2100 は 8bitUnsupportedHdr)、録画が始まらないので、--hdr は録画と配信のエンコーダも HEVC にする。
OBS_HDR_COLOR_FORMAT = "P010"
OBS_HDR_COLOR_SPACE = "2100PQ"
OBS_HDR_ENCODER = "obs_nvenc_hevc_tex"
OBS_SDR_ENCODER = "obs_nvenc_h264_tex"
# --replay-buffer は、test 用のプロファイル (テスト用の OBS だけ) の [AdvOut] に、リプレイバッファを有効にする 3 キーを書く。
# キーの綴りは OBS 31.1.1 の frontend/utility/AdvancedOutput.cpp (RecType=Standard の枝で
# config_get_bool(..., "AdvOut", "RecRB") が真なら replay_buffer の出力を作り、RecRBTime と RecRBSize を max_time_sec と
# max_size_mb に渡す) と settings/OBSBasicSettings.cpp の SaveCheckBox / SaveSpinBox (AdvOut の RecRB・RecRBTime・RecRBSize)。
# 書くのは「使える」ようにするまでで、動かすのは実行スクリプトが obs-websocket で呼ぶ StartReplayBuffer / StopReplayBuffer
# (保存しなければファイルを書かない)。確認したいのは「リプレイバッファの動作中はアンインストールできません」の
# 止める理由だけなので、溜める長さと量は小さくする (10 秒・256 MB)。OBS の既定 (20 秒・512 MB) では、
# 溜める分のメモリが増えるだけで確認できる内容は同じである。--shape user には許さない
# (導入の往復の差分に影響する設定を変えてしまう)。
REPLAY_BUFFER_SECONDS = 10
REPLAY_BUFFER_MEGABYTES = 256

# 名前の大きさ (settings.json の label_em_ratio_pct。配信の画面の高さに対する %)。範囲と刻みは src-obs/config/settings.h の
# kLabelEmRatioMinPct と kLabelEmRatioMaxPct と同じで、製品は範囲の外と 0.1 刻みでない値を読まずに設定全体を既定に戻すので、
# 書く前に断る。README の画像のように、縮めて見せる画面で名前を読めるようにするときに使う。--shape test だけ。
LABEL_EM_RATIO_PCT_MIN = 1.0
LABEL_EM_RATIO_PCT_MAX = 8.5


def label_em_ratio_problem(value):
    # 範囲の外か 0.1 刻みでなければ理由を返す。よければ None。
    if not LABEL_EM_RATIO_PCT_MIN <= value <= LABEL_EM_RATIO_PCT_MAX:
        return "--label-em-ratio-pct must be %.1f to %.1f" % (LABEL_EM_RATIO_PCT_MIN, LABEL_EM_RATIO_PCT_MAX)
    if abs(value * 10 - round(value * 10)) > 1e-6:
        return "--label-em-ratio-pct must be a multiple of 0.1"
    return None


def parse_args(argv):
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument("obs_dir")
    parser.add_argument("out_dir")
    parser.add_argument("--step", required=True,
                        help="the --step of run-window.mjs (the allow_state_json_write rule reads it)")
    parser.add_argument("--appdata", default=None,
                        help="where APPDATA is swapped to (a fake Vesktop state.json is written there)")
    parser.add_argument("--real-profile", action="store_true",
                        help="APPDATA is not swapped (call-* only; no state.json is written)")
    parser.add_argument("--shape", choices=SHAPE_VALUES, default="test")
    parser.add_argument("--dock-install", choices=DOCK_INSTALL_VALUES, default="elsewhere")
    parser.add_argument("--vencord-dist", default=None,
                        help="a Vencord dist with the 4 files (_work/vencord-dist)")
    parser.add_argument("--assign-mode", choices=ASSIGN_MODE_VALUES, default="manual",
                        help="assign_mode of settings.json (default manual, as the product)")
    parser.add_argument("--decoder", choices=DECODER_VALUES, default="software",
                        help="decoder of settings.json (default software, as the product)")
    parser.add_argument("--display-buffer-ms", type=int, default=None,
                        help="display_buffer_ms of settings.json (0 to 1000; omitted = not written, the product default)")
    parser.add_argument("--label-plate", action="store_true",
                        help="write label_show_plate=true and label_plate_opacity_pct=75 (the premise of plate_measure.py; omitted = the product defaults)")
    parser.add_argument("--allow-state-json-write", action="store_true",
                        help="call-* only: let the product write the real state.json")
    # リハーサル (run-window.mjs --step call-1 --rehearsal): call-* を差し替えた APPDATA (--appdata) で実行する。
    # 実際の state.json には触れない (書くのは偽の state.json だけ = --appdata の規則で true)。
    parser.add_argument("--rehearsal", action="store_true",
                        help="call-* with --appdata (the call rehearsed with the test sender; no real state.json)")
    # 既定は <out_dir>/recordings で、_work の外へは外せない。
    parser.add_argument("--record-dir", default=None,
                        help="record_dir of settings.json (default <out_dir>/recordings; must be under _work)")
    parser.add_argument("--record-space-check-interval-s", type=int, default=None,
                        help="free space check period in seconds (written only when given)")
    parser.add_argument("--record-space-warn-minutes", type=int, default=None,
                        help="remaining time warning in minutes (written only when given)")
    parser.add_argument("--vencord-check-disconnected-s", type=int, default=None,
                        help="Vencord check period while disconnected, seconds (written only when given)")
    parser.add_argument("--vencord-check-connected-s", type=int, default=None,
                        help="Vencord check period while connected, seconds (written only when given)")
    parser.add_argument("--vencord-state", choices=VENCORD_STATE_VALUES, default="absent",
                        help="vencordDir of the --shape user state.json (absent = the default Vencord)")
    parser.add_argument("--custom-vencord-dir", default=None,
                        help="the self-built Vencord folder for --vencord-state custom (4 marker files)")
    parser.add_argument("--config-dir", default=None,
                        help="OBS settings folder (default <obs>/config/obs-studio; changed only in the VM)")
    parser.add_argument("--no-portable-mode-file", action="store_true",
                        help="do not write portable_mode.txt (VM --layout programdata only)")
    parser.add_argument("--vm", action="store_true",
                        help="running inside the VM (needed by --config-dir / --no-portable-mode-file)")
    parser.add_argument("--dry-run", action="store_true",
                        help="write nothing; print what would be written on the CORDMUX_SEED_CONFIG_JSON: line")
    # OBS 自身の見た目と言語 (言語は ja / en、テーマは Yami / Yami Light / System、フォントサイズ、密度)。
    # どれも渡したときだけ書く (渡さなければ OBS の既定)。キーの綴りと範囲は上の OBS_* を参照。
    parser.add_argument("--language", default=None,
                        help="OBS UI language (user.ini [General] Language, e.g. ja-JP / en-US; must exist in <obs>/data/obs-studio/locale)")
    parser.add_argument("--theme", default=None,
                        help="OBS theme id (user.ini [Appearance] Theme, e.g. com.obsproject.Yami.Light; must exist in <obs>/data/obs-studio/themes)")
    parser.add_argument("--font-scale", type=int, default=None,
                        help="OBS font size (user.ini [Appearance] FontScale, %d..%d, OBS default %d)"
                        % (OBS_FONT_SCALE_MIN, OBS_FONT_SCALE_MAX, OBS_FONT_SCALE_DEFAULT))
    parser.add_argument("--density", type=int, default=None,
                        help="OBS density id (user.ini [Appearance] Density, one of %s, OBS default %d)"
                        % (",".join(str(v) for v in OBS_DENSITY_VALUES), OBS_DENSITY_DEFAULT))
    parser.add_argument("--hdr", action="store_true",
                        help="test profile in HDR: [Video] ColorFormat=P010 ColorSpace=2100PQ and the NVENC HEVC encoder")
    parser.add_argument("--full-height-docks", choices=OBS_FULL_HEIGHT_DOCKS_VALUES, default=None,
                        help="OBS Docks > Full-Height Docks (user.ini [BasicWindow] SideDocks; omitted = the OBS default, on in OBS 33)")
    parser.add_argument("--replay-buffer", action="store_true",
                        help="test profile only: enable the replay buffer ([AdvOut] RecRB=true RecRBTime=%d RecRBSize=%d)"
                        % (REPLAY_BUFFER_SECONDS, REPLAY_BUFFER_MEGABYTES))
    parser.add_argument("--label-em-ratio-pct", type=float, default=None,
                        help="test profile only: write label_em_ratio_pct of settings.json (the name size in percent of the canvas height, "
                        "%.1f to %.1f in steps of 0.1; omitted = the product default)" % (LABEL_EM_RATIO_PCT_MIN, LABEL_EM_RATIO_PCT_MAX))
    parser.add_argument("--self-speaking-source-name", default=None,
                        help="test profile only: write self_speaking_source_name of settings.json (the OBS audio input that decides whether "
                        "the own slot's person is speaking; the run creates the input over obs-websocket). The threshold is not written")
    parser.add_argument(ALLOW_OUTSIDE_WORK_FLAG, dest="allow_outside_work", action="store_true",
                        help="accept an OBS folder outside <repo>/_work and an output folder outside <repo>/_work and "
                        "<repo>/build_x64 (run-window.mjs passes it when it was given the same flag)")
    return parser.parse_args(argv)


# 録画の保存先が作業用のフォルダーの下でなければ、このツールは何もしない。
WORK_DIR_MARKER = "_work"


def record_dir_is_inside_work(path):
    """録画の保存先が作業用のフォルダ (_work) の下かどうか。区切りは両方受ける。"""
    parts = os.path.abspath(path).replace("\\", "/").split("/")
    return WORK_DIR_MARKER in parts


# OBS のフォルダー (obs_dir) は repo の _work の下、出力先 (out_dir) は repo の _work か
# build_x64 の下だけを受ける。確認しないと、書き違え 1 つ (例: 利用者の OBS のフォルダー) で、その OBS の設定
# (config/obs-studio)・portable_mode.txt・プラグインのファイルを書き換えてしまう。録画の保存先の守り (上) はパスのどこかに
# _work という名前があるかだけを見るので、この 2 つの守りにはならない。実行スクリプト (run-window.mjs の workPathProblems) と
# 同じ規則で、同じフラグ (--allow-outside-work) で外せる (実行スクリプトは自分がフラグを受けた場合だけ渡す)。
# 下 (直下でなく下) だけを受け、ルートそのものは断る。文字列の上で比べる (ジャンクションは解かない)。
# --config-dir (VM の --layout programdata) と --appdata は、実行スクリプトが決まった場所を渡すので見ない。
# 断りは終了コード 3 (録画の保存先の守り) と分ける。混ぜると、呼び出し側が「どの守りか」を見分けられない。
ALLOW_OUTSIDE_WORK_FLAG = "--allow-outside-work"
OBS_DIR_ROOTS = (("_work",),)
OUT_DIR_ROOTS = (("_work",), ("build_x64",))


def path_is_under(path, root):
    # path が root の下 (root そのものは含まない) か。どちらも abspath で絶対にし、normcase で大文字小文字と区切りをそろえる。
    full = os.path.normcase(os.path.abspath(path))
    root_full = os.path.normcase(os.path.abspath(root)).rstrip(os.sep)
    return full.startswith(root_full + os.sep) and len(full) > len(root_full) + 1


def work_path_problems(obs_dir, out_dir, repo_root):
    # 純関数。守りに合わない理由の並び (空なら合う)。
    problems = []
    for flag, value, roots in (("obs_dir", obs_dir, OBS_DIR_ROOTS), ("out_dir", out_dir, OUT_DIR_ROOTS)):
        root_paths = [os.path.join(str(repo_root), *parts) for parts in roots]
        if not any(path_is_under(value, r) for r in root_paths):
            problems.append("%s must be under %s (got %s; pass %s to accept it)"
                            % (flag, " or ".join(root_paths), os.path.abspath(value), ALLOW_OUTSIDE_WORK_FLAG))
    return problems


def decide_state_json_write(step, real_profile, has_appdata, allow_flag, rehearsal=False):
    # state.json を書く規則。戻り値は (書く値, 守りの理由)。守りに掛かったら値は None。
    # rehearsal (リハーサル): call-* を --appdata で実行する唯一の形。--real-profile とは両立しない。
    is_call = step in CALL_STEPS
    if real_profile == has_appdata:
        return None, "pass exactly one of --appdata and --real-profile"
    if allow_flag and not is_call:
        return None, "--allow-state-json-write is accepted only for call-* steps"
    if rehearsal and (not is_call or real_profile):
        return None, "--rehearsal is accepted only for call-* steps with --appdata"
    if is_call and not real_profile and not rehearsal:
        return None, "call-* steps run only with --real-profile (or --rehearsal with --appdata)"
    if step == ROUNDTRIP_STEP and real_profile:
        return None, "install-roundtrip runs only with --appdata"
    if real_profile:
        # call-* 以外は必ず false。call-* はフラグがあるときだけ true。
        return bool(is_call and allow_flag), None
    # --appdata の実行: 偽の state.json にしか届かないので、製品の既定と同じ true。
    return True, None


def bundled_vencord_dirs(obs_dir):
    """プラグインのファイルの data/vencord (Vesktop に見せる場所) と data/vencord-src (原本) の絶対パス。
    src-obs/plugin-main.cpp の join_dir(module_data_dir(), "vencord"/"vencord-src") と
    同じ場所 (module_data_dir() = obs_get_module_data_path() は、この実行スクリプトが filesToPlace で
    コピーする先そのものである <obs>/plugins/cordmux/data)。"""
    data_dir = os.path.join(obs_dir, "plugins", "cordmux", "data")
    return os.path.join(data_dir, "vencord"), os.path.join(data_dir, "vencord-src")


def dist_has_bundle_files(dist_dir):
    if not dist_dir or not os.path.isdir(dist_dir):
        return False
    return all(os.path.isfile(os.path.join(dist_dir, name)) for name in VENCORD_BUNDLE_FILES)


def reset_dir(path):
    """前回の --dock-install が残した中身を消す (obs_dir は実行ごとに使い回されるため、
    ここで消さないと例えば前回 ours で置いた 4 ファイルが missing の実行にも残ってしまう)。"""
    if os.path.isdir(path):
        shutil.rmtree(path)
    elif os.path.exists(path):
        os.remove(path)


def copy_bundle_files(dist_dir, dest_dir):
    """照合する 4 ファイルと、dist にあれば package.json もコピーする。
    戻り値は実際にコピーした任意ファイルの名前の一覧 (呼び出し側が結果 JSON に残す)。"""
    os.makedirs(dest_dir, exist_ok=True)
    for name in VENCORD_BUNDLE_FILES:
        shutil.copyfile(os.path.join(dist_dir, name), os.path.join(dest_dir, name))
    copied_optional = []
    for name in VENCORD_OPTIONAL_FILES:
        src = os.path.join(dist_dir, name)
        if os.path.isfile(src):
            shutil.copyfile(src, os.path.join(dest_dir, name))
            copied_optional.append(name)
    return copied_optional


def seed_vencord_bundle(obs_dir, dock_install, dist_dir):
    """--dock-install ours/missing 用に、プラグインのファイルの data/vencord (見せる場所) と
    data/vencord-src (原本) を作り直す。

    elsewhere/absent は同梱かどうかの判定そのものへ進まない (state.json の vencordDir が
    同梱の場所を指していない時点で dir_state_of() が PointsElsewhere/NotConfigured を返し、
    ハッシュ照合を見ない。src-obs/dock/build_observation.cpp の dir_state_of)。この 2 つの
    ときは vencord/vencord-src ディレクトリを空にするだけで、4 ファイルは置かない。

    ours は原本 (vencord-src) と見せる場所 (vencord) の両方に同じ 4 ファイルを置く
    (ハッシュが一致 = Matches)。missing は原本だけに置き、見せる場所は「フォルダはあるが
    4 ファイルはない」状態にする (Force Update で上書きされて消えている、を模す =
    Mismatch/missing)。フォルダー自体は必ず作る (作らないと、見せる場所が丸ごと存在しない状態になる)。

    _work/vencord-dist がない、または 4 ファイルが揃っていなければ、ours も missing も
    原本を用意できない (原本が読めないと bundle_check.h は「違う」と言わずに Unknown を
    返す) ので、結果として同じ見え方 (Unknown) になる。呼び出し側 (run-window.mjs) の結果 JSON に
    残す note はここで作る。"""
    vencord_dir, vencord_src_dir = bundled_vencord_dirs(obs_dir)
    reset_dir(vencord_dir)
    reset_dir(vencord_src_dir)

    if dock_install not in ("missing", "ours"):
        return {"attempted": False, "distFound": None, "note": None}

    if not dist_has_bundle_files(dist_dir):
        note = "vencord-dist not found"
        print(note + ":", dist_dir)
        return {"attempted": True, "distFound": False, "note": note, "optionalFiles": []}

    optional = copy_bundle_files(dist_dir, vencord_src_dir)
    if dock_install == "ours":
        optional = copy_bundle_files(dist_dir, vencord_dir)
    else:
        # missing はここで vencord_dir を「空フォルダ」として作る (4 ファイルは置かない)。
        os.makedirs(vencord_dir, exist_ok=True)
        optional = []

    return {"attempted": True, "distFound": True, "note": None, "optionalFiles": optional}


def vesktop_state(obs_dir, dock_install, appdata_dir):
    """偽の Vesktop state.json の中身を作る (--dock-install の値に応じて vencordDir を
    変える)。ほかのキー (maximized/minimized) は実際の state.json に実在するキーをそのまま真似た
    もので、cordmux はこの 2 つを読まない (os_probe.h は vencordDir しか見ない)。"""
    vencord_dir, _ = bundled_vencord_dirs(obs_dir)
    state = {"maximized": False, "minimized": False}
    if dock_install == "elsewhere":
        # 同梱の場所とは別の、appdata の配下だけに存在する架空の場所 (実在しなくてよい。
        # cordmux はこの文字列を、プラグインのファイルの vencord/vencord-src とだけ突き合わせ、vencordDir が
        # 指す先そのものを読みには行かない = src-obs/platform/os_probe.h probe_once)。
        elsewhere_dir = os.path.join(appdata_dir, "elsewhere-vencord", "dist")
        state["vencordDir"] = elsewhere_dir.replace("\\", "/")
    elif dock_install in ("missing", "ours"):
        state["vencordDir"] = vencord_dir.replace("\\", "/")
    # dock_install == "absent": vencordDir キー自体を持たせない (state.json は読めたが
    # キーがまだない = ledger::VencordDirState::NotConfigured)。
    return state


# ---- 書く中身を先に全部組み立てる (--dry-run が 1 バイトも書かずに同じ値を出せるように) ----


def json_text(value, indent=None):
    return json.dumps(value, ensure_ascii=False, indent=indent)


def scene_source(name, items=None):
    # シーン 1 つぶんのソースの JSON (OBS のシーンコレクションの sources の 1 要素)。
    return {
        "id": "scene", "versioned_id": "scene", "name": name, "uuid": str(uuid.uuid4()),
        "enabled": True, "muted": False, "volume": 1.0, "balance": 0.5, "monitoring_type": 0,
        "mixers": 0, "sync": 0, "flags": 0, "push-to-mute": False, "push-to-mute-delay": 0,
        "push-to-talk": False, "push-to-talk-delay": 0, "hotkeys": {}, "deinterlace_mode": 0,
        "deinterlace_field_order": 0, "private_settings": {}, "prev_ver": 0,
        "settings": {"id_counter": len(items or []), "custom_size": False, "items": items or []},
    }


def collection(name, scenes, other_sources=None):
    # シーンコレクションの JSON。scenes は scene_source() の並び (最初が出ているシーン)。
    first = scenes[0]["name"]
    return {
        "name": name,
        "current_scene": first,
        "current_program_scene": first,
        "current_transition": "Cut",
        "transition_duration": 0,
        "transitions": [],
        "scene_order": [{"name": s["name"]} for s in scenes],
        "groups": [],
        "modules": {},
        "sources": list(scenes) + list(other_sources or []),
    }


def item(source, item_id, x=0.0, y=0.0):
    # シーンの中の項目 (sources[].settings.items の 1 要素)。
    return {
        "name": source["name"], "source_uuid": source["uuid"], "visible": True, "locked": False,
        "rot": 0.0, "pos": {"x": x, "y": y}, "scale": {"x": 1.0, "y": 1.0}, "align": 5,
        "bounds_type": 0, "bounds_align": 0, "bounds": {"x": 0.0, "y": 0.0},
        "crop_left": 0, "crop_top": 0, "crop_right": 0, "crop_bottom": 0, "id": item_id,
        "group_item_backup": False, "scale_filter": "disable", "blend_method": "default",
        "blend_type": "normal", "show_transition": {"duration": 0}, "hide_transition": {"duration": 0},
        "private_settings": {},
    }


def input_source(source_id, versioned_id, name, settings, volume=1.0, filters=None, mixers=0):
    # 利用者のソース 1 つ (シーンではないもの)。
    source = {
        "id": source_id, "versioned_id": versioned_id, "name": name, "uuid": str(uuid.uuid4()),
        "enabled": True, "muted": False, "volume": volume, "balance": 0.5, "monitoring_type": 0,
        "mixers": mixers, "sync": 0, "flags": 0, "push-to-mute": False, "push-to-mute-delay": 0,
        "push-to-talk": False, "push-to-talk-delay": 0, "hotkeys": {}, "deinterlace_mode": 0,
        "deinterlace_field_order": 0, "private_settings": {}, "prev_ver": 0, "settings": settings,
    }
    if filters:
        source["filters"] = filters
    return source


def filter_source(source_id, versioned_id, name, settings):
    return {
        "id": source_id, "versioned_id": versioned_id, "name": name, "uuid": str(uuid.uuid4()),
        "enabled": True, "muted": False, "volume": 1.0, "balance": 0.5, "monitoring_type": 0,
        "mixers": 0, "sync": 0, "flags": 0, "push-to-mute": False, "push-to-mute-delay": 0,
        "push-to-talk": False, "push-to-talk-delay": 0, "hotkeys": {}, "deinterlace_mode": 0,
        "deinterlace_field_order": 0, "private_settings": {}, "prev_ver": 0, "settings": settings,
    }


def user_collections():
    # --shape user の 2 つのコレクション (上の説明を参照)。
    background = input_source("color_source", "color_source_v3", "Background",
                               {"color": 4281545523, "width": 1920, "height": 1080})
    title = input_source("text_gdiplus", "text_gdiplus_v3", "Title", {"text": "Hello"},
                         filters=[filter_source("color_filter", "color_filter_v2", "Color Correction",
                                                {"opacity": 0.9})])
    music = input_source("ffmpeg_source", "ffmpeg_source", "Music",
                         {"is_local_file": True, "local_file": "", "looping": False},
                         volume=USER_SOURCE_VOLUME, mixers=255)
    opening = scene_source("Opening", items=[item(background, 1), item(title, 2, 100.0, 100.0)])
    game = scene_source("Game", items=[item(title, 1, 40.0, 40.0), item(music, 2)])
    main = collection(USER_COLLECTION_MAIN, [opening, game], [background, title, music])

    backdrop = input_source("color_source", "color_source_v3", "Backdrop",
                            {"color": 4278190335, "width": 1920, "height": 1080})
    chat_scene = scene_source("Chat", items=[item(backdrop, 1)])
    second = collection(USER_COLLECTION_SECOND, [chat_scene], [backdrop])
    return main, second


def appearance_ini_lines(look):
    # 渡された物だけを [General] と [Appearance] に足す文字列 (look は obs_look_from_args の戻り値)。
    general = ""
    if look.get("language"):
        general += "Language=" + look["language"] + "\n"
    appearance = ""
    if look.get("theme"):
        appearance += "Theme=" + look["theme"] + "\n"
    if look.get("fontScale") is not None:
        appearance += "FontScale=" + str(look["fontScale"]) + "\n"
    if look.get("density") is not None:
        appearance += "Density=" + str(look["density"]) + "\n"
    return general, ("\n[Appearance]\n" + appearance) if appearance else ""


def global_ini_text(profile, collection_name, look=None):
    general, appearance = appearance_ini_lines(look or {})
    # [ドック] の [フルハイト ドック] (渡したときだけ。上の OBS_FULL_HEIGHT_DOCKS_VALUES を参照)。
    full_height_docks = (look or {}).get("fullHeightDocks")
    side_docks = "" if full_height_docks is None else "SideDocks=" + ("true" if full_height_docks else "false") + "\n"
    return (
        "[General]\n"
        + general
        + "FirstRun=true\n"
        "LicenseAccepted=true\n"
        "ConfirmOnExit=false\n"
        "EnableAutoUpdates=false\n"
        "WarnBeforeStartingStream=false\n"
        "WarnBeforeStoppingStream=false\n"
        "WarnBeforeStoppingRecord=false\n"
        "\n"
        "[Basic]\n"
        "Profile=" + profile + "\n"
        "ProfileDir=" + profile + "\n"
        "SceneCollection=" + collection_name + "\n"
        "SceneCollectionFile=" + collection_name + "\n"
        "\n"
        "[BasicWindow]\n"
        "ShowListboxToolbars=true\n"
        + side_docks
        + "\n"
        "[Video]\n"
        "Renderer=Direct3D 11\n"
        + appearance
    )


def poc_basic_ini(out_dir, hdr=False, replay_buffer=False):
    # hdr (--hdr): 色の形式と色空間を HDR に、エンコーダーを HEVC にする (上の OBS_HDR_* を参照)。
    # replay_buffer (--replay-buffer): [AdvOut] にリプレイバッファの 3 キーを書く (上の REPLAY_BUFFER_* を参照)。
    replay = ("RecRB=true\n" "RecRBTime=" + str(REPLAY_BUFFER_SECONDS) + "\n"
              "RecRBSize=" + str(REPLAY_BUFFER_MEGABYTES) + "\n") if replay_buffer else ""
    color_format = OBS_HDR_COLOR_FORMAT if hdr else "NV12"
    color_space = OBS_HDR_COLOR_SPACE if hdr else "709"
    encoder = OBS_HDR_ENCODER if hdr else OBS_SDR_ENCODER
    return (
        "[General]\n"
        "Name=test\n"
        "\n"
        "[Video]\n"
        "BaseCX=1920\n"
        "BaseCY=1080\n"
        "OutputCX=1920\n"
        "OutputCY=1080\n"
        "FPSType=0\n"
        "FPSCommon=60\n"
        "ScaleType=bicubic\n"
        "ColorFormat=" + color_format + "\n"
        "ColorSpace=" + color_space + "\n"
        "ColorRange=Partial\n"
        "\n"
        "[Audio]\n"
        "SampleRate=48000\n"
        "ChannelSetup=Stereo\n"
        "MonitoringDeviceId=disabled\n"
        "MonitoringDeviceName=Disabled\n"
        "\n"
        "[Output]\n"
        "Mode=Advanced\n"
        "\n"
        "[AdvOut]\n"
        "RecType=Standard\n"
        "RecFilePath=" + out_dir.replace("\\", "/") + "\n"
        "RecFormat2=mkv\n"
        "RecEncoder=" + encoder + "\n"
        "RecAudioEncoder=ffmpeg_aac\n"
        "RecTracks=1\n"
        "RecUseRescale=false\n"
        "TrackIndex=1\n"
        "Encoder=" + encoder + "\n"
        "AudioEncoder=ffmpeg_aac\n"
        "ApplyServiceSettings=false\n"
        "FFOutputToFile=true\n"
        + replay
        + "\n"
        "[Stream1]\n"
        "IgnoreRecommended=true\n"
    )


def user_basic_ini(record_dir):
    # --shape user のプロファイル (単純出力・x264・30fps。録画の保存先は作業用のフォルダの下)。
    return (
        "[General]\n"
        "Name=" + USER_PROFILE_NAME + "\n"
        "\n"
        "[Video]\n"
        "BaseCX=1920\n"
        "BaseCY=1080\n"
        "OutputCX=1280\n"
        "OutputCY=720\n"
        "FPSType=0\n"
        "FPSCommon=30\n"
        "\n"
        "[Audio]\n"
        "SampleRate=48000\n"
        "ChannelSetup=Stereo\n"
        "MonitoringDeviceId=disabled\n"
        "MonitoringDeviceName=Disabled\n"
        "\n"
        "[Output]\n"
        "Mode=Simple\n"
        "\n"
        "[SimpleOutput]\n"
        "FilePath=" + record_dir.replace("\\", "/") + "\n"
        "RecFormat2=mkv\n"
        "StreamEncoder=x264\n"
        "RecQuality=Stream\n"
        "VBitrate=4000\n"
        "ABitrate=160\n"
        "\n"
        "[Stream1]\n"
        "IgnoreRecommended=true\n"
    )


def websocket_config_text():
    return json_text({"alerts_enabled": False, "auth_required": False, "first_load": False,
                      "server_enabled": True, "server_password": "", "server_port": 4460})


def service_json_text(key):
    # 配信先はローカルの受け口 (ffmpeg -listen 1)。配信の出力に乗るかの確認用。
    return json_text({"type": "rtmp_custom", "settings": {"server": "rtmp://127.0.0.1:1935/live", "key": key,
                      "use_auth": False, "bwtest": False}})


def cordmux_settings(args, record_dir, state_json_write):
    # cordmux の settings.json。--shape test は必ず書き、--shape user は周期のキーを渡したときだけ
    # 書く (上の説明を参照)。書くときは allow_state_json_write と record_dir を必ず入れる。
    # 省けるのは書く値が製品の既定 (true) と同じときだけ。--real-profile の実行は
    # shape によらず必ず書く (省くと製品の既定 true が働き、call-* 以外でも実際の state.json を書ける)。
    periods = {}
    if args.vencord_check_disconnected_s is not None:
        periods["vencord_check_disconnected_s"] = int(args.vencord_check_disconnected_s)
    if args.vencord_check_connected_s is not None:
        periods["vencord_check_connected_s"] = int(args.vencord_check_connected_s)
    if args.shape == "user" and not periods and state_json_write:
        return None
    settings = {"allow_state_json_write": bool(state_json_write),
                # 必ず差し替える。守りを通っているので、
                # ここに入る値は必ず作業用のフォルダ (_work) の下である。
                "record_dir": record_dir.replace("\\", "/")}
    if args.shape == "test":
        settings = {"assign_mode": args.assign_mode,
                    "allow_state_json_write": bool(state_json_write),
                    "decoder": args.decoder,
                    "record_dir": record_dir.replace("\\", "/")}
        # 表示の緩衝 (src-obs/config/settings.h の display_buffer_ms)。渡されたときだけ書く。範囲は製品の clamp と同じ。
        if args.display_buffer_ms is not None:
            if not 0 <= args.display_buffer_ms <= 1000:
                raise SystemExit("--display-buffer-ms must be 0 to 1000")
            settings["display_buffer_ms"] = int(args.display_buffer_ms)
        # 名前ラベルの板 (plate_measure.py で測る)。既定の板は不透明度 50% (label_show_plate は既定で true) なので、
        # plate_measure.py の前提 (黒の 75%) で出す。渡されたときだけ書く。
        if args.label_plate:
            settings["label_show_plate"] = True
            settings["label_plate_opacity_pct"] = 75
        # 自分の枠の音声ソースの名前 (hold の --self-mic。しきい値は書かない = 製品の既定の値で測る)。
        if args.self_speaking_source_name is not None:
            settings["self_speaking_source_name"] = args.self_speaking_source_name
        # 名前の大きさ (hold の --label-em-ratio-pct)。範囲と刻みは build_plan で確認済み。
        if args.label_em_ratio_pct is not None:
            settings["label_em_ratio_pct"] = round(args.label_em_ratio_pct, 1)
        if args.record_space_check_interval_s is not None:
            settings["record_space_check_interval_s"] = int(args.record_space_check_interval_s)
        if args.record_space_warn_minutes is not None:
            settings["record_space_warn_minutes"] = int(args.record_space_warn_minutes)
    settings.update(periods)
    return settings


def user_vesktop_files(appdata_dir, vencord_state, custom_dir):
    # --shape user の偽の APPDATA の中身。{絶対パス: 中身}。
    vesktop = os.path.join(appdata_dir, "Vesktop")
    state = {"firstLaunch": False, "maximized": False, "minimized": False,
             "windowBounds": {"x": 120, "y": 80, "width": 1280, "height": 720}}
    files = {}
    if vencord_state == "custom":
        custom_abs = os.path.normpath(os.path.abspath(custom_dir))
        state["vencordDir"] = custom_abs
        for name in VENCORD_BUNDLE_FILES:
            files[os.path.join(custom_abs, name)] = "/* custom Vencord build: " + USER_SETTINGS_MARKER + " */\n"
    files[os.path.join(vesktop, "state.json")] = json_text(state, indent=2)
    files[os.path.join(vesktop, "settings.json")] = json_text(
        {"discordBranch": "stable", "minimizeToTray": True, "arRPC": False}, indent=2)
    files[os.path.join(vesktop, "settings", "settings.json")] = json_text(
        {"autoUpdate": False, "notifyAboutUpdates": False, "useQuickCss": True,
         "plugins": {"NoTrack": {"enabled": True}, "Settings": {"enabled": True}},
         "notes": USER_SETTINGS_MARKER}, indent=2)
    files[os.path.join(vesktop, "settings", "quickCss.css")] = "/* " + USER_SETTINGS_MARKER + " */\nbody { }\n"
    return files


def validate_period(value, flag):
    if value is None:
        return None
    if value < VENCORD_CHECK_MIN_S or value > VENCORD_CHECK_MAX_S:
        return flag + " must be " + str(VENCORD_CHECK_MIN_S) + ".." + str(VENCORD_CHECK_MAX_S)
    return None


THEME_ID_PATTERN = re.compile(r"""@OBSThemeMeta\s*\{[^}]*?\bid:\s*'([^']+)'""", re.S)
LANGUAGE_PATTERN = re.compile(r"^[a-z]{2,3}-[A-Z]{2}$")


def obs_theme_ids(obs_dir):
    # <obs>/data/obs-studio/themes の .obt と .ovt の @OBSThemeMeta の id (OBS が Theme の値と照らす物)。
    # 保存先が無ければ None (確認できない)。
    themes_dir = os.path.join(obs_dir, "data", "obs-studio", "themes")
    if not os.path.isdir(themes_dir):
        return None
    ids = []
    for name in sorted(os.listdir(themes_dir)):
        if not (name.endswith(".obt") or name.endswith(".ovt")):
            continue
        with open(os.path.join(themes_dir, name), encoding="utf-8", errors="replace") as f:
            m = THEME_ID_PATTERN.search(f.read())
        if m:
            ids.append(m.group(1))
    return ids


def obs_look_from_args(args, obs_dir):
    # 6 つの値を確認して {language, theme, fontScale, density, hdr, fullHeightDocks} にする。戻り値は (look, 誤りの文字列 or None)。
    # 言語とテーマは、その OBS が持っているものだけを受ける (OBS は知らない値を黙って既定に落とすので、綴りを誤ると
    # 実機で既定の見た目を撮ってしまい、撮った後まで気づけない)。確認する保存先がなければ断る。
    # フルハイト ドックの on / off は argparse の choices で確認済み (渡さなければ None = 書かない)。
    look = {"language": None, "theme": None, "fontScale": None, "density": None, "hdr": bool(args.hdr),
            "fullHeightDocks": None if args.full_height_docks is None else args.full_height_docks == "on"}
    if args.language is not None:
        if not LANGUAGE_PATTERN.match(args.language):
            return None, "--language must look like ja-JP (got " + args.language + ")"
        ini = os.path.join(obs_dir, "data", "obs-studio", "locale", args.language + ".ini")
        if not os.path.isfile(ini):
            return None, "--language " + args.language + ": no " + ini + " in this OBS"
        look["language"] = args.language
    if args.theme is not None:
        ids = obs_theme_ids(obs_dir)
        if ids is None:
            return None, "--theme: no data/obs-studio/themes in " + obs_dir + " (cannot check the id)"
        if args.theme not in ids:
            return None, "--theme " + args.theme + " is not a theme of this OBS (" + ", ".join(ids) + ")"
        look["theme"] = args.theme
    if args.font_scale is not None:
        if not (OBS_FONT_SCALE_MIN <= args.font_scale <= OBS_FONT_SCALE_MAX):
            return None, "--font-scale must be %d..%d" % (OBS_FONT_SCALE_MIN, OBS_FONT_SCALE_MAX)
        look["fontScale"] = args.font_scale
    if args.density is not None:
        if args.density not in OBS_DENSITY_VALUES:
            return None, "--density must be one of " + ",".join(str(v) for v in OBS_DENSITY_VALUES)
        look["density"] = args.density
    if args.hdr and args.shape != "test":
        return None, "--hdr is accepted only with --shape test (the user shape records with x264)"
    return look, None


def build_plan(args):
    # 書く中身を全部組み立てる。戻り値は (plan, 守りの理由, 終了コード)。
    obs_dir = os.path.abspath(args.obs_dir)
    out_dir = os.path.abspath(args.out_dir)
    record_dir = os.path.abspath(args.record_dir) if args.record_dir else os.path.join(out_dir, "recordings")

    # OBS のフォルダーと出力先の保存先の守り。ほかの守りと同じく、設定を 1 バイトも書く前に見る。
    if not args.allow_outside_work:
        problems = work_path_problems(obs_dir, out_dir, REPO_ROOT)
        if problems:
            return None, "; ".join(problems), EXIT_WORK_PATH_GUARD

    # 録画の保存先の守り。設定を 1 バイトも書く前に見る (途中まで書いた状態で
    # 止まると、次の起動で古い record_dir のまま OBS が立ち上がりうる)。
    if not record_dir_is_inside_work(record_dir):
        return None, ("record dir is not under a " + WORK_DIR_MARKER + " folder: " + record_dir
                      + " (milestone 6 decision 45)"), EXIT_RECORD_DIR_GUARD

    state_json_write, guard = decide_state_json_write(args.step, bool(args.real_profile), args.appdata is not None,
                                                      bool(args.allow_state_json_write), bool(args.rehearsal))
    if guard is not None:
        return None, guard, EXIT_STATE_JSON_GUARD
    if args.step == ROUNDTRIP_STEP and args.shape != "user":
        return None, "install-roundtrip uses --shape user", EXIT_STATE_JSON_GUARD
    for value, flag in ((args.vencord_check_disconnected_s, "--vencord-check-disconnected-s"),
                        (args.vencord_check_connected_s, "--vencord-check-connected-s")):
        problem = validate_period(value, flag)
        if problem:
            return None, problem, 2
    if args.vencord_state == "custom" and (args.shape != "user" or not args.custom_vencord_dir):
        return None, "--vencord-state custom needs --shape user and --custom-vencord-dir", 2
    look, look_error = obs_look_from_args(args, obs_dir)
    if look_error is not None:
        return None, look_error, 2
    if args.replay_buffer and args.shape != "test":
        return None, "--replay-buffer is accepted only with --shape test (the test OBS profile)", 2
    if args.label_em_ratio_pct is not None:
        if args.shape != "test":
            return None, "--label-em-ratio-pct is accepted only with --shape test (the test OBS profile)", 2
        problem = label_em_ratio_problem(args.label_em_ratio_pct)
        if problem:
            return None, problem, 2
    if args.self_speaking_source_name is not None:
        name = args.self_speaking_source_name
        if args.shape != "test":
            return None, "--self-speaking-source-name is accepted only with --shape test (the test OBS profile)", 2
        if not name.strip() or name != name.strip() or len(name) > 64 or any(ord(c) < 32 or ord(c) == 127 for c in name):
            return None, "--self-speaking-source-name must be 1 to 64 characters without control characters or edge spaces", 2
    if (args.config_dir or args.no_portable_mode_file) and not args.vm:
        return None, "--config-dir / --no-portable-mode-file are accepted only with --vm", EXIT_STATE_JSON_GUARD

    cfg = os.path.abspath(args.config_dir) if args.config_dir else os.path.join(obs_dir, "config", "obs-studio")
    if args.config_dir and os.path.isdir(cfg) and os.listdir(cfg):
        # まっさらな VM でだけ使う保存先。既に中身があれば (VM でない PC なら利用者の実際のもの) 何も書かない。
        return None, "config dir already has content, refusing to write: " + cfg, EXIT_STATE_JSON_GUARD

    files = {}
    if not args.no_portable_mode_file:
        files[os.path.join(obs_dir, "portable_mode.txt")] = "portable\n"
    files[os.path.join(cfg, "plugin_config", "obs-websocket", "config.json")] = websocket_config_text()

    scenes_dir = os.path.join(cfg, "basic", "scenes")
    if args.shape == "test":
        ini = global_ini_text("test", "test", look)
        prof = os.path.join(cfg, "basic", "profiles", "test")
        files[os.path.join(prof, "basic.ini")] = poc_basic_ini(out_dir, hdr=look["hdr"], replay_buffer=bool(args.replay_buffer))
        files[os.path.join(prof, "recordEncoder.json")] = json_text(
            {"bitrate": 12000, "rate_control": "CBR", "preset2": "p4", "tune": "hq", "profile": "high",
             "lookahead": False, "psycho_aq": True, "bf": 2, "keyint_sec": 2})
        files[os.path.join(prof, "streamEncoder.json")] = json_text(
            {"bitrate": 12000, "rate_control": "CBR", "preset2": "p4", "profile": "high", "bf": 2, "keyint_sec": 2})
        files[os.path.join(prof, "service.json")] = service_json_text("test")
        # 空の Main シーンだけを持つシーンコレクションを作る (name はコレクション名 = ファイル名の
        # もとになる名前)。--step second-obs が SetCurrentSceneCollection の行き先に使う
        # "test-empty" もここで同じ形で作る。実行スクリプト (test/tools/obs-run/run-window-plan.mjs) の
        # SECOND_OBS_MAIN_SCENE_COLLECTION / SECOND_OBS_EMPTY_SCENE_COLLECTION と名前を揃えること)。
        test = collection("test", [scene_source("Main")])
        files[os.path.join(scenes_dir, "test.json")] = json_text(test, indent=1)
        # --step second-obs が切り替える先の「空のもの」。global.ini の SceneCollection は "test" の
        # ままなので OBS はこちらを既定では開かない (obs-websocket の SetCurrentSceneCollection でだけ使う)。
        files[os.path.join(scenes_dir, "test-empty.json")] = json_text(collection("test-empty", [scene_source("Main")]), indent=1)
    else:
        ini = global_ini_text(USER_PROFILE_NAME, USER_COLLECTION_MAIN, look)
        prof = os.path.join(cfg, "basic", "profiles", USER_PROFILE_NAME)
        files[os.path.join(prof, "basic.ini")] = user_basic_ini(record_dir)
        files[os.path.join(prof, "service.json")] = service_json_text("user-like")
        main, second = user_collections()
        files[os.path.join(scenes_dir, USER_COLLECTION_MAIN + ".json")] = json_text(main, indent=1)
        files[os.path.join(scenes_dir, USER_COLLECTION_SECOND + ".json")] = json_text(second, indent=1)
    for name in ("global.ini", "user.ini"):
        files[os.path.join(cfg, name)] = ini

    # cordmux 自身の設定 (obs_module_config_path("settings.json") = plugin_config/cordmux/)。
    # 書くのは決まったキーだけにする。ないキーは既定値になる
    # (config/settings.h の「無ければ既定値」) ので、実行スクリプトが製品の既定のコピーを持たずに
    # 済む (コピーすると、既定が変わったときに実機の実行だけが古い値で動く)。record_dir だけは例外で、
    # 既定に任せると利用者の実際の録画フォルダーへ書くことになる。
    settings = cordmux_settings(args, record_dir, state_json_write)
    if settings is not None:
        files[os.path.join(cfg, "plugin_config", "cordmux", "settings.json")] = json_text(settings, indent=2)

    # 前の実行の保存状態 (STALE_STATE_FILES)。test の形だけ消す (user の形は導入の往復で、cordmux のものを 1 つも置かない形)。
    stale_paths = []
    if args.shape == "test":
        stale_paths = [os.path.join(cfg, "plugin_config", "cordmux", name) for name in STALE_STATE_FILES]
    state = None
    if args.appdata:
        appdata_dir = os.path.abspath(args.appdata)
        if args.shape == "user":
            for path, text in user_vesktop_files(appdata_dir, args.vencord_state, args.custom_vencord_dir).items():
                files[path] = text
            state = json.loads(files[os.path.join(appdata_dir, "Vesktop", "state.json")])
        else:
            state = vesktop_state(obs_dir, args.dock_install, appdata_dir)
            files[os.path.join(appdata_dir, "Vesktop", "state.json")] = json_text(state)

    plan = {
        "obsDir": obs_dir, "outDir": out_dir, "configDir": cfg, "step": args.step, "shape": args.shape,
        "dockInstall": args.dock_install, "assignMode": args.assign_mode, "decoder": args.decoder,
        "realProfile": bool(args.real_profile),
        # 書いた OBS の見た目と言語 (渡さなかった物は null = OBS の既定)。
        "obsLook": look,
        # test 用のプロファイルにリプレイバッファを書いたか (--replay-buffer)。
        "replayBuffer": bool(args.replay_buffer),
        # 書いた値 (settings.json を書かない実行は書いていない = null と、製品の既定)。
        "allowStateJsonWrite": bool(state_json_write) if settings is not None else None,
        "allowStateJsonWriteRule": state_json_write,
        "recordDir": record_dir.replace("\\", "/"),
        "settings": settings,
        "stateJson": state,
        "files": sorted(p.replace("\\", "/") for p in files),
        # 実行の始めに消す保存状態 (STALE_STATE_FILES)。実際に消した物は seed の結果の removedStateFiles に出る。
        "staleStateFiles": sorted(p.replace("\\", "/") for p in stale_paths),
        "_stale_paths": stale_paths,
        "record_dir_abs": record_dir,
        "_files": files,
    }
    return plan, None, 0


def write_text(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def remove_stale_state(paths):
    """前の実行の保存状態のファイルを消す。あった物の絶対パスを返す。消せなければ例外のまま止める
    (残したまま実行すると、前の実行の人が枠に載ったまま始まる)。"""
    removed = []
    for path in paths:
        if os.path.isfile(path):
            os.remove(path)
            removed.append(path)
    return removed


def public_result(plan):
    return {k: v for k, v in plan.items() if not k.startswith("_") and k != "record_dir_abs"}


def main(argv):
    if argv and argv[0] == "--selftest":
        return selftest()
    args = parse_args(argv)
    plan, guard, code = build_plan(args)
    if guard is not None:
        print("seed-config refused: " + guard)
        print("CORDMUX_SEED_CONFIG_JSON:" + json.dumps({"ok": False, "exitCode": code, "reason": guard, "step": args.step}))
        return code

    result = public_result(plan)
    if args.dry_run:
        result["dryRun"] = True
        print("CORDMUX_SEED_CONFIG_JSON:" + json.dumps(result, ensure_ascii=False))
        return 0

    obs_dir = plan["obsDir"]
    os.makedirs(plan["configDir"], exist_ok=True)
    os.makedirs(plan["outDir"], exist_ok=True)
    os.makedirs(plan["record_dir_abs"], exist_ok=True)
    removed = remove_stale_state(plan["_stale_paths"])
    result["removedStateFiles"] = [p.replace("\\", "/") for p in removed]
    for path in removed:
        print("removed stale state:", path)
    for path, text in plan["_files"].items():
        write_text(path, text)
    print("seeded:", plan["configDir"])

    # プラグインのファイル (data/vencord と data/vencord-src) は --appdata の有無と関係なく置く
    # (2 つの仕事は別である)。--shape user (導入の往復) は zip が置くので触らない。
    if args.shape == "test":
        dist_dir = os.path.abspath(args.vencord_dist) if args.vencord_dist else None
        result["vencordDist"] = seed_vencord_bundle(obs_dir, args.dock_install, dist_dir)

    if args.appdata:
        result["appdataDir"] = os.path.abspath(args.appdata)
        result["stateJsonPath"] = os.path.join(os.path.abspath(args.appdata), "Vesktop", "state.json")
        print("seeded fake Vesktop files under:", result["appdataDir"])
    else:
        print("no --appdata: the real state.json is not touched (only the product writes it)")
        result["appdataDir"] = None
        result["stateJsonPath"] = None

    # run-window.mjs はこの 1 行を拾って結果 JSON (state.results) へ残す (cleanup.ps1 の
    # CORDMUX_CLEANUP_JSON: と同じ「末尾に 1 行、決まった接頭辞つきの JSON」という形)。
    print("CORDMUX_SEED_CONFIG_JSON:" + json.dumps(result, ensure_ascii=False))
    return 0


# ---- セルフテスト ------------------------------------------------------------
# 一時の保存先は呼び出し側の TEMP (tempfile.gettempdir()) の下に作って消す。録画の保存先の守りが
# 名前が `_work` のフォルダーの下を求めるので、TEMP の下の作業フォルダーの中に `_work` を作り、その中で実行する。
# _work は実機の実行の保存先 (実行の準備が中身を保存して凍結し、実行スクリプトの後片付けが毎回消す場所) で、
# セルフテストがそこに書くと実行の前後の中身の比べに混じり、並べて実行する別の実行の後片付けとも衝突する。
# build_x64 の決まった場所は使わない。CI と手元で保存先が違い、呼び出し側が TEMP で決められない。
# 呼び出し側が TEMP を替えなければ OS の既定の一時フォルダになる (CI の ubuntu は /tmp)。


def selftest_place_problem(path, temp_dir, repo_root):
    # セルフテストの保存先が守りに合うか。合えば None、合わなければ理由。純関数 (セルフテストが実際の保存先と前の保存先の両方に当てる)。
    full = os.path.normcase(os.path.abspath(path))
    temp_full = os.path.normcase(os.path.abspath(temp_dir))
    work_full = os.path.normcase(os.path.abspath(os.path.join(str(repo_root), "_work")))
    if full != temp_full and not full.startswith(temp_full.rstrip(os.sep) + os.sep):
        return "not under the caller's TEMP (" + temp_dir + ")"
    if full == work_full or full.startswith(work_full + os.sep):
        return "under _work (the live window's folder)"
    return None


def selftest():
    import tempfile
    temp_dir = tempfile.gettempdir()
    temp_root = tempfile.mkdtemp(prefix="seed-selftest-")
    # 録画の保存先の守り (record_dir_is_inside_work) を通る名前の中で実行する (上の説明を参照)。
    tmp = os.path.join(temp_root, WORK_DIR_MARKER)
    os.makedirs(tmp)
    print("selftest work: " + tmp)
    failures = []

    def check(cond, label):
        if not cond:
            failures.append(label)
        print(("ok   " if cond else "FAIL ") + label)

    try:
        # (0) 保存先の守り。実際の保存先は TEMP の下で _work の外、前の保存先 (_work の下) は断る。
        check(selftest_place_problem(tmp, temp_dir, REPO_ROOT) is None, "the work folder is under the caller's TEMP and outside _work")
        old_place = os.path.join(str(REPO_ROOT), "_work", "seed-selftest-x")
        check(selftest_place_problem(old_place, temp_dir, REPO_ROOT) is not None, "the old place under _work is refused")
        check(selftest_place_problem(os.path.join(temp_dir + "2", "x"), temp_dir, REPO_ROOT) is not None,
              "a sibling folder whose name starts with TEMP is not under TEMP")
        check(record_dir_is_inside_work(tmp), "the record dir guard sees the work folder as under _work")
        check(not record_dir_is_inside_work(os.path.join(temp_root, "outside-record-dir")),
              "the outside folder of check (11) has no _work part (TEMP itself must not be under a _work folder)")

        # (0b) OBS のフォルダーと出力先の保存先の守り。純関数に、実行スクリプトが渡す形と外の形を渡して確認する。
        repo = str(REPO_ROOT)
        rj = lambda *parts: os.path.join(repo, *parts)  # noqa: E731
        place_cases = [
            # (obs_dir, out_dir, 守りを通るか, 名前)
            (rj("_work", "obs-33.0.1"), rj("_work", "out", "live-a", "window3b"), True, "manual: obs and out under _work"),
            (rj("_work", "rt-live311", "obs"), rj("_work", "out", "live-a", "window3b"), True, "manual: the roundtrip OBS under _work/rt-<tag>"),
            (rj("_work", "obs-33.0.1"), rj("build_x64", "dry-run", "_work", "out-hold", "window3b"), True, "dry-run: out under build_x64"),
            (rj("build_x64", "obs"), rj("_work", "out"), False, "obs under build_x64 is refused (only out may be there)"),
            (rj("_work"), rj("_work", "out"), False, "obs = _work itself is refused"),
            (rj("_work2", "obs"), rj("_work", "out"), False, "a sibling _work2 is refused"),
            (rj("_work", "..", "obs"), rj("_work", "out"), False, "a .. that leaves _work is refused"),
            (rj("_work", "obs-33.0.1"), rj("build_x64"), False, "out = build_x64 itself is refused"),
            # repo の外 (repo の親の下。文字列だけで、作らない)。
            (rj("_work", "obs-33.0.1"), os.path.join(os.path.dirname(repo), "cordmux-outside", "out"), False, "out outside the repo is refused"),
            (os.path.join(os.path.dirname(repo), "cordmux-outside", "obs"), rj("_work", "out"), False, "obs outside the repo is refused"),
            (os.path.join(temp_root, "obs"), rj("_work", "out"), False, "obs under the caller's TEMP is refused"),
        ]
        if os.name == "nt":
            place_cases.append((rj("_work", "obs-33.0.1").upper(), rj("_work", "out").upper(), True,
                                "Windows: the same folders in another letter case pass"))
        for obs_case, out_case, want_ok, label in place_cases:
            got = work_path_problems(obs_case, out_case, REPO_ROOT)
            check((not got) == want_ok, "place guard: " + label + (" (" + "; ".join(got) + ")" if got and want_ok else ""))

        # セルフテストの保存先 (TEMP の下) は上の守りの外なので、ほかの main の呼び出しはフラグを付けて実行する (守り自体は (0b) と下で当てる)。
        def main_t(argv):
            return main(argv + [ALLOW_OUTSIDE_WORK_FLAG])

        def parse_args_t(argv):
            return parse_args(argv + [ALLOW_OUTSIDE_WORK_FLAG])

        # フラグがなければ、TEMP の下の OBS のフォルダーと出力先は終了コード 5 で断り、何も書かない。
        wp_obs = os.path.join(tmp, "wp-obs")
        wp_out = os.path.join(tmp, "wp-out")
        wp_ad = os.path.join(tmp, "wp-ad")
        code = main([wp_obs, wp_out, "--step", "dock-view", "--appdata", wp_ad])
        check(code == EXIT_WORK_PATH_GUARD and not any(os.path.exists(p) for p in (wp_obs, wp_out, wp_ad)),
              "obs and out outside the work places are refused with 5 before any write")
        code = main([wp_obs, wp_out, "--step", "dock-view", "--appdata", wp_ad, ALLOW_OUTSIDE_WORK_FLAG, "--dry-run"])
        check(code == 0 and not os.path.exists(wp_obs), "the override flag accepts them (dry-run writes nothing)")

        # (1) state.json を書く規則の表。
        table = [
            # 並び: step・実際の保存先か・偽の APPDATA か・フラグ -> (書く値, 守りに掛かるか)
            ("dock-view", False, True, False, (True, False)),
            ("dock-view", True, False, False, (False, False)),
            ("dock-view", True, False, True, (None, True)),
            ("dock-view", False, True, True, (None, True)),
            ("call-1", True, False, True, (True, False)),
            ("call-1", True, False, False, (False, False)),
            ("call-gate-a", False, True, True, (None, True)),
            ("call-hold", True, False, True, (True, False)),
            ("call-hold", False, True, True, (None, True)),
            ("install-roundtrip", False, True, False, (True, False)),
            ("install-roundtrip", True, False, False, (None, True)),
            ("record", False, False, False, (None, True)),
            ("record", True, True, False, (None, True)),
        ]
        for step, real, appdata, flag, (want, guarded) in table:
            value, guard = decide_state_json_write(step, real, appdata, flag)
            check((guard is not None) == guarded and (guarded or value == want),
                  "rule step=%s real=%s appdata=%s flag=%s -> %s" % (step, real, appdata, flag, want))
        # リハーサル (--rehearsal): call-1 を --appdata で実行する唯一の形。それ以外の step や --real-profile とは断る。
        rehearsal_table = [
            ("call-1", False, True, False, (True, False)),
            ("call-1", False, True, True, (True, False)),
            ("call-1", True, False, False, (None, True)),
            ("call-hold", False, True, True, (True, False)),
            ("dock-view", False, True, False, (None, True)),
        ]
        for step, real, appdata, flag, (want, guarded) in rehearsal_table:
            value, guard = decide_state_json_write(step, real, appdata, flag, True)
            check((guard is not None) == guarded and (guarded or value == want),
                  "rehearsal rule step=%s real=%s appdata=%s flag=%s -> %s" % (step, real, appdata, flag, want))

        def run(extra, obs_name):
            obs = os.path.join(tmp, obs_name)
            out = os.path.join(tmp, obs_name + "-out")
            return obs, out, main_t([obs, out] + extra)

        # (2) --real-profile の非 call の実行は false を書く。
        obs, out, code = run(["--step", "dock-view", "--real-profile"], "real-noncall")
        s = json.load(open(os.path.join(obs, "config", "obs-studio", "plugin_config", "cordmux", "settings.json"), encoding="utf-8"))
        check(code == 0 and s["allow_state_json_write"] is False, "real profile + dock-view writes false")
        check(not os.path.exists(os.path.join(tmp, "real-noncall", "Vesktop")), "real profile writes no state.json")

        # (3) 非 call の実行にフラグを渡すと何も書かずに 4。
        obs, out, code = run(["--step", "dock-view", "--appdata", os.path.join(tmp, "ad3"), "--allow-state-json-write"], "flag-noncall")
        check(code == EXIT_STATE_JSON_GUARD and not os.path.exists(obs), "flag on non-call step is refused before any write")

        # (4) --appdata の実行は true、test は Main シーンだけを持つ (cordmux のものは何も置かない)。
        obs, out, code = run(["--step", "select-four", "--appdata", os.path.join(tmp, "ad4")], "fake")
        cfg = os.path.join(obs, "config", "obs-studio")
        s = json.load(open(os.path.join(cfg, "plugin_config", "cordmux", "settings.json"), encoding="utf-8"))
        check(code == 0 and s["allow_state_json_write"] is True, "fake appdata writes true (product default)")
        test = json.load(open(os.path.join(cfg, "basic", "scenes", "test.json"), encoding="utf-8"))
        check([src["name"] for src in test["sources"]] == ["Main"] and all(not src["private_settings"] for src in test["sources"]),
              "test has only the Main scene and no cordmux mark")
        empty = json.load(open(os.path.join(cfg, "basic", "scenes", "test-empty.json"), encoding="utf-8"))
        check([src["name"] for src in empty["sources"]] == ["Main"], "test-empty has only the Main scene")
        # (5) --label-plate は板の 2 キーだけを足し、渡さなければ書かない (ほかのキーは変わらない)。
        check("label_show_plate" not in s and "label_plate_opacity_pct" not in s, "no label keys without --label-plate")
        obs5, out5, code5 = run(["--step", "select-four", "--appdata", os.path.join(tmp, "ad5p"), "--label-plate"], "plate")
        s5 = json.load(open(os.path.join(obs5, "config", "obs-studio", "plugin_config", "cordmux", "settings.json"), encoding="utf-8"))
        check(code5 == 0 and s5["label_show_plate"] is True and s5["label_plate_opacity_pct"] == 75, "--label-plate writes the plate keys")
        check({k: v for k, v in s5.items() if not k.startswith("label_") and k != "record_dir"} == {k: v for k, v in s.items() if k != "record_dir"},
              "--label-plate changes no other key")
        # (6) --shape user: cordmux のものを 1 つも置かず、Vesktop の目印を置く。
        ad6 = os.path.join(tmp, "ad6")
        obs, out, code = run(["--step", "install-roundtrip", "--appdata", ad6, "--shape", "user"], "user")
        cfg = os.path.join(obs, "config", "obs-studio")
        check(code == 0 and not os.path.exists(os.path.join(cfg, "plugin_config", "cordmux")), "user shape writes no cordmux settings")
        for name in (USER_COLLECTION_MAIN, USER_COLLECTION_SECOND):
            col = json.load(open(os.path.join(cfg, "basic", "scenes", name + ".json"), encoding="utf-8"))
            check(all(SCENE_ROLE_KEY not in src.get("private_settings", {}) for src in col["sources"]), "user collection %s has no cordmux scene" % name)
        state = json.load(open(os.path.join(ad6, "Vesktop", "state.json"), encoding="utf-8"))
        check("vencordDir" not in state, "user shape default has no vencordDir (default Vencord)")
        check(os.path.isfile(os.path.join(ad6, "Vesktop", "settings", "quickCss.css")), "user shape writes the quickCss marker")

        # (7) --shape user + custom: 自作の場所に 4 ファイル、state.json がそこを指す。
        ad7 = os.path.join(tmp, "ad7")
        custom = os.path.join(tmp, "custom-vencord", "dist")
        obs, out, code = run(["--step", "install-roundtrip", "--appdata", ad7, "--shape", "user",
                              "--vencord-state", "custom", "--custom-vencord-dir", custom], "user-custom")
        state = json.load(open(os.path.join(ad7, "Vesktop", "state.json"), encoding="utf-8"))
        check(code == 0 and os.path.normcase(state.get("vencordDir", "")) == os.path.normcase(os.path.normpath(custom)), "custom vencordDir is written")
        check(all(os.path.isfile(os.path.join(custom, n)) for n in VENCORD_BUNDLE_FILES), "custom dir has the 4 marker files")

        # (8) --shape user + 周期のキー: settings.json に周期と守りの 2 キーだけ。
        obs, out, code = run(["--step", "install-roundtrip", "--appdata", os.path.join(tmp, "ad8"), "--shape", "user",
                              "--vencord-check-disconnected-s", "5", "--vencord-check-connected-s", "5"], "user-periods")
        s = json.load(open(os.path.join(obs, "config", "obs-studio", "plugin_config", "cordmux", "settings.json"), encoding="utf-8"))
        check(code == 0 and sorted(s) == sorted(["allow_state_json_write", "record_dir", "vencord_check_disconnected_s", "vencord_check_connected_s"]),
              "user shape with periods writes only the period keys and the two guarded keys")

        # (8b) --real-profile は --shape user でも settings.json に false を書く
        # (省くと製品の既定 true が働く)。dry-run の出力にも false が出る。
        obs, out, code = run(["--step", "dock-view", "--real-profile", "--shape", "user"], "real-user")
        s_path = os.path.join(obs, "config", "obs-studio", "plugin_config", "cordmux", "settings.json")
        s = json.load(open(s_path, encoding="utf-8")) if os.path.isfile(s_path) else {}
        check(code == 0 and s.get("allow_state_json_write") is False, "real profile + user shape + dock-view writes false")
        obs, out, code = run(["--step", "call-1", "--real-profile", "--shape", "user"], "real-user-call")
        s_path = os.path.join(obs, "config", "obs-studio", "plugin_config", "cordmux", "settings.json")
        s = json.load(open(s_path, encoding="utf-8")) if os.path.isfile(s_path) else {}
        check(code == 0 and s.get("allow_state_json_write") is False, "real profile + user shape + call-1 without the flag writes false")
        plan, guard, code = build_plan(parse_args_t([os.path.join(tmp, "dry-ru"), os.path.join(tmp, "dry-ru-out"), "--step", "dock-view",
                                                   "--real-profile", "--shape", "user", "--dry-run"]))
        check(guard is None and public_result(plan)["allowStateJsonWrite"] is False and public_result(plan)["settings"] is not None,
              "dry-run of real profile + user shape shows false")

        # (8c) 前の実行の保存状態 (STALE_STATE_FILES) は実行の始めに消え、バックアップ (vencord_dir_backup.json) は残る。dry-run は消さない。
        obs9c = os.path.join(tmp, "stale")
        pdir = os.path.join(obs9c, "config", "obs-studio", "plugin_config", "cordmux")
        os.makedirs(pdir)
        for name in STALE_STATE_FILES + ["vencord_dir_backup.json"]:
            write_text(os.path.join(pdir, name), "{}")
        code = main_t([obs9c, obs9c + "-out", "--step", "dock-view", "--appdata", os.path.join(tmp, "ad9c"), "--dry-run"])
        check(code == 0 and all(os.path.isfile(os.path.join(pdir, n)) for n in STALE_STATE_FILES), "--dry-run keeps the stale state files")
        code = main_t([obs9c, obs9c + "-out", "--step", "dock-view", "--appdata", os.path.join(tmp, "ad9c")])
        check(code == 0 and not any(os.path.exists(os.path.join(pdir, n)) for n in STALE_STATE_FILES), "the stale state files are removed at seed")
        check(os.path.isfile(os.path.join(pdir, "vencord_dir_backup.json")) and os.path.isfile(os.path.join(pdir, "settings.json")),
              "the Vesktop vencordDir backup stays and settings.json is written")

        # (9) --dry-run は 1 バイトも書かない。
        obs, out, code = run(["--step", "dock-view", "--appdata", os.path.join(tmp, "ad9"), "--dry-run"], "dry")
        check(code == 0 and not os.path.exists(obs) and not os.path.exists(os.path.join(tmp, "ad9")), "--dry-run writes nothing")

        # (10) --config-dir は --vm が無ければ組めない。
        obs, out, code = run(["--step", "install-roundtrip", "--appdata", os.path.join(tmp, "ad10"), "--shape", "user",
                              "--config-dir", os.path.join(tmp, "cfg10")], "cfgdir")
        check(code == EXIT_STATE_JSON_GUARD and not os.path.exists(os.path.join(tmp, "cfg10")), "--config-dir without --vm is refused")

        # (11) 録画の保存先が _work の外なら 3 (守りの 1 つ)。外の場所は作業フォルダーの中の _work の隣にする。
        outside = os.path.join(temp_root, "outside-record-dir")
        code = main_t([os.path.join(tmp, "o11"), os.path.join(tmp, "o11-out"), "--step", "dock-view",
                     "--appdata", os.path.join(tmp, "ad11"), "--record-dir", outside])
        check(code == EXIT_RECORD_DIR_GUARD and not os.path.exists(outside), "record dir outside _work is refused")

        # (12) call-* を --appdata で呼ぶと 4。
        code = main_t([os.path.join(tmp, "o12"), os.path.join(tmp, "o12-out"), "--step", "call-1",
                     "--appdata", os.path.join(tmp, "ad12"), "--allow-state-json-write"])
        check(code == EXIT_STATE_JSON_GUARD and not os.path.exists(os.path.join(tmp, "o12")), "call-* with --appdata is refused")

        # (13) OBS の言語と見た目。テーマと言語はその OBS が持つ物だけを受ける (偽の OBS に 1 つずつ置く)。
        def fake_obs(name):
            obs_dir = os.path.join(tmp, name)
            write_text(os.path.join(obs_dir, "data", "obs-studio", "locale", "en-US.ini"), 'Language="English"\n')
            write_text(os.path.join(obs_dir, "data", "obs-studio", "themes", "Yami_Light.ovt"),
                       "@OBSThemeMeta {\n    name: 'Light';\n    id: 'com.obsproject.Yami.Light';\n    extends: 'com.obsproject.Yami';\n}\n")
            return obs_dir

        obs13 = fake_obs("look")
        code = main_t([obs13, obs13 + "-out", "--step", "hold", "--appdata", os.path.join(tmp, "ad13"), "--language", "en-US",
                     "--theme", "com.obsproject.Yami.Light", "--font-scale", "10", "--density", "-2"])
        cfg13 = os.path.join(obs13, "config", "obs-studio")
        user_ini = open(os.path.join(cfg13, "user.ini"), encoding="utf-8").read()
        check(code == 0 and "[General]\nLanguage=en-US\n" in user_ini, "--language goes to [General] Language")
        check("\n[Appearance]\nTheme=com.obsproject.Yami.Light\nFontScale=10\nDensity=-2\n" in user_ini,
              "--theme / --font-scale / --density go to [Appearance]")
        check(open(os.path.join(cfg13, "global.ini"), encoding="utf-8").read() == user_ini, "global.ini keeps the same text as user.ini")
        obs13b = fake_obs("look-default")
        code = main_t([obs13b, obs13b + "-out", "--step", "hold", "--appdata", os.path.join(tmp, "ad13b")])
        user_ini = open(os.path.join(obs13b, "config", "obs-studio", "user.ini"), encoding="utf-8").read()
        check(code == 0 and "Language=" not in user_ini and "[Appearance]" not in user_ini and "SideDocks=" not in user_ini,
              "nothing is written when the look flags are absent")
        for value, written in (("off", "false"), ("on", "true")):
            obs13c = fake_obs("look-docks-" + value)
            code = main_t([obs13c, obs13c + "-out", "--step", "hold", "--appdata", os.path.join(tmp, "ad13c-" + value),
                           "--full-height-docks", value])
            cfg13c = os.path.join(obs13c, "config", "obs-studio")
            user_ini = open(os.path.join(cfg13c, "user.ini"), encoding="utf-8").read()
            check(code == 0 and "\n[BasicWindow]\nShowListboxToolbars=true\nSideDocks=" + written + "\n\n" in user_ini
                  and open(os.path.join(cfg13c, "global.ini"), encoding="utf-8").read() == user_ini,
                  "--full-height-docks " + value + " goes to [BasicWindow] SideDocks=" + written + " of user.ini and global.ini")
        for extra, label in ((["--theme", "com.obsproject.Nope"], "unknown theme"), (["--language", "fr-FR"], "language the OBS lacks"),
                             (["--language", "english"], "language not shaped like ja-JP"), (["--font-scale", "13"], "font scale above 12"),
                             (["--density", "0"], "density id OBS does not use")):
            obs_bad = fake_obs("bad-" + label.replace(" ", "-"))
            code = main_t([obs_bad, obs_bad + "-out", "--step", "hold", "--appdata", os.path.join(tmp, "adbad")] + extra)
            check(code == 2 and not os.path.exists(os.path.join(obs_bad, "config")), label + " is refused before any write")
        code = main_t([os.path.join(tmp, "nothemes"), os.path.join(tmp, "nothemes-out"), "--step", "hold", "--appdata", os.path.join(tmp, "adnt"),
                     "--theme", "com.obsproject.Yami.Light"])
        check(code == 2, "--theme is refused when the OBS has no themes folder to check against")

        # (14) --hdr: 色の形式・色空間・エンコーダー (HEVC) が HDR になる。user shape では断る。
        obs14 = fake_obs("hdr")
        code = main_t([obs14, obs14 + "-out", "--step", "hold", "--appdata", os.path.join(tmp, "ad14"), "--hdr"])
        basic = open(os.path.join(obs14, "config", "obs-studio", "basic", "profiles", "test", "basic.ini"), encoding="utf-8").read()
        check(code == 0 and "ColorFormat=P010\nColorSpace=2100PQ\n" in basic, "--hdr writes P010 and 2100PQ")
        check("RecEncoder=" + OBS_HDR_ENCODER + "\n" in basic and "\nEncoder=" + OBS_HDR_ENCODER + "\n" in basic, "--hdr switches both encoders to HEVC")
        basic_sdr = poc_basic_ini(os.path.join(tmp, "sdr"))
        check("ColorFormat=NV12\nColorSpace=709\n" in basic_sdr and "RecEncoder=" + OBS_SDR_ENCODER + "\n" in basic_sdr, "without --hdr the profile is unchanged (NV12, 709, H.264)")
        code = main_t([os.path.join(tmp, "hdr-user"), os.path.join(tmp, "hdr-user-out"), "--step", "install-roundtrip",
                     "--appdata", os.path.join(tmp, "ad14u"), "--shape", "user", "--hdr"])
        check(code == 2, "--hdr with --shape user is refused")

        # (17) --replay-buffer: test 用の [AdvOut] にだけ 3 キーを書く (なければ書かない)。user shape では断る。
        obs17 = fake_obs("replay")
        code = main_t([obs17, obs17 + "-out", "--step", "hold", "--appdata", os.path.join(tmp, "ad17"), "--replay-buffer"])
        basic17 = open(os.path.join(obs17, "config", "obs-studio", "basic", "profiles", "test", "basic.ini"), encoding="utf-8").read()
        adv17 = basic17.split("[AdvOut]\n", 1)[-1].split("\n\n[", 1)[0]
        check(code == 0 and "\nRecRB=true\nRecRBTime=%d\nRecRBSize=%d\n" % (REPLAY_BUFFER_SECONDS, REPLAY_BUFFER_MEGABYTES) in adv17 + "\n"
              and basic17.count("RecRB=") == 1, "--replay-buffer writes RecRB / RecRBTime / RecRBSize in [AdvOut]")
        basic_plain = open(os.path.join(obs13b, "config", "obs-studio", "basic", "profiles", "test", "basic.ini"), encoding="utf-8").read()
        check("RecRB" not in basic_plain, "without --replay-buffer the profile has no replay buffer key")
        code = main_t([os.path.join(tmp, "rb-user"), os.path.join(tmp, "rb-user-out"), "--step", "install-roundtrip",
                     "--appdata", os.path.join(tmp, "ad17u"), "--shape", "user", "--replay-buffer"])
        check(code == 2 and not os.path.exists(os.path.join(tmp, "rb-user")), "--replay-buffer with --shape user is refused before any write")
        plan, guard, code = build_plan(parse_args_t([os.path.join(tmp, "rb-dry"), os.path.join(tmp, "rb-dry-out"), "--step", "hold",
                                                   "--appdata", os.path.join(tmp, "ad17d"), "--replay-buffer", "--dry-run"]))
        check(guard is None and public_result(plan)["replayBuffer"] is True, "dry-run shows replayBuffer true")

        # (18) hold の --self-mic: test の形の settings.json に自分の枠の音声ソースの名前だけを書く (しきい値は書かない)。
        #      プロファイルの監視の出力は disabled のまま (音を鳴らさない)。user shape と不正な名前は書く前に断る。
        obs18 = fake_obs("self-mic")
        code = main_t([obs18, obs18 + "-out", "--step", "hold", "--appdata", os.path.join(tmp, "ad18"),
                     "--self-speaking-source-name", "マイク"])
        cfg18 = os.path.join(obs18, "config", "obs-studio")
        settings18 = json.loads(open(os.path.join(cfg18, "plugin_config", "cordmux", "settings.json"), encoding="utf-8").read())
        check(code == 0 and settings18.get("self_speaking_source_name") == "マイク", "--self-speaking-source-name writes the name")
        check("self_speaking_threshold_db" not in settings18 and "self_speaking_source_uuid" not in settings18,
              "--self-speaking-source-name writes neither the threshold nor the uuid (the product default is measured)")
        basic18 = open(os.path.join(cfg18, "basic", "profiles", "test", "basic.ini"), encoding="utf-8").read()
        check("\n[Audio]\n" in basic18 and "\nMonitoringDeviceId=disabled\n" in basic18, "the test profile keeps the monitoring output disabled")
        settings_plain = json.loads(open(os.path.join(obs13b, "config", "obs-studio", "plugin_config", "cordmux", "settings.json"), encoding="utf-8").read())
        check("self_speaking_source_name" not in settings_plain, "without the flag settings.json has no self speaking key")
        for extra, label in ((["--self-speaking-source-name", " x"], "name with an edge space"), (["--self-speaking-source-name", ""], "empty name"),
                             (["--self-speaking-source-name", "x" * 65], "name over 64 characters"),
                             (["--self-speaking-source-name", "a\tb"], "name with a control character")):
            obs_bad = fake_obs("bad-self-" + label.replace(" ", "-"))
            code = main_t([obs_bad, obs_bad + "-out", "--step", "hold", "--appdata", os.path.join(tmp, "adbad18")] + extra)
            check(code == 2 and not os.path.exists(os.path.join(obs_bad, "config")), label + " is refused before any write")
        code = main_t([os.path.join(tmp, "self-user"), os.path.join(tmp, "self-user-out"), "--step", "install-roundtrip",
                     "--appdata", os.path.join(tmp, "ad18u"), "--shape", "user", "--self-speaking-source-name", "x"])
        check(code == 2 and not os.path.exists(os.path.join(tmp, "self-user")), "--self-speaking-source-name with --shape user is refused before any write")

        # (19) hold の --label-em-ratio-pct: test の形の settings.json に名前の大きさを 1 桁の小数で書く。範囲の外、0.1 刻みでない値、
        #      user の形は書く前に断る。渡さなければキーを書かない (製品の既定)。
        obs19 = fake_obs("label-em")
        code = main_t([obs19, obs19 + "-out", "--step", "hold", "--appdata", os.path.join(tmp, "ad19"), "--label-em-ratio-pct", "4"])
        settings19 = json.loads(open(os.path.join(obs19, "config", "obs-studio", "plugin_config", "cordmux", "settings.json"),
                                     encoding="utf-8").read())
        check(code == 0 and settings19.get("label_em_ratio_pct") == 4.0, "--label-em-ratio-pct writes label_em_ratio_pct")
        check("label_em_ratio_pct" not in settings_plain, "without the flag settings.json has no label_em_ratio_pct")
        for value, label in (("0.9", "below the minimum"), ("8.6", "above the maximum"), ("4.05", "not a multiple of 0.1")):
            obs_bad = fake_obs("bad-label-em-" + label.replace(" ", "-"))
            code = main_t([obs_bad, obs_bad + "-out", "--step", "hold", "--appdata", os.path.join(tmp, "adbad19"),
                           "--label-em-ratio-pct", value])
            check(code == 2 and not os.path.exists(os.path.join(obs_bad, "config")), "--label-em-ratio-pct " + label + " is refused before any write")
        for value in ("1.0", "8.5"):
            check(label_em_ratio_problem(float(value)) is None, "--label-em-ratio-pct " + value + " (an end of the range) is accepted")
        code = main_t([os.path.join(tmp, "label-user"), os.path.join(tmp, "label-user-out"), "--step", "install-roundtrip",
                     "--appdata", os.path.join(tmp, "ad19u"), "--shape", "user", "--label-em-ratio-pct", "4"])
        check(code == 2 and not os.path.exists(os.path.join(tmp, "label-user")), "--label-em-ratio-pct with --shape user is refused before any write")

        # (16) キーの綴りを OBS のソースと照合する (ソースが手元にあるときだけ。なければ飛ばしたと書く)。
        obs_src = REPO_ROOT / "src-obs" / ".deps" / "obs-studio-31.1.1" / "frontend"
        if obs_src.is_dir():
            app = (obs_src / "OBSApp.cpp").read_text(encoding="utf-8", errors="replace")
            look_cpp = (obs_src / "settings" / "OBSBasicSettings_Appearance.cpp").read_text(encoding="utf-8", errors="replace")
            settings_cpp = (obs_src / "settings" / "OBSBasicSettings.cpp").read_text(encoding="utf-8", errors="replace")
            check('config_get_string(userConfig, "General", "Language")' in app, "OBS source reads [General] Language from user.ini")
            check(all('config_set_%s(config, "Appearance", "%s"' % (kind, key) in look_cpp
                      for kind, key in (("string", "Theme"), ("int", "FontScale"), ("int", "Density"))),
                  "OBS source writes [Appearance] Theme / FontScale / Density")
            check('config_set_default_int(userConfig, "Appearance", "FontScale", %d)' % OBS_FONT_SCALE_DEFAULT in app
                  and 'config_set_default_int(userConfig, "Appearance", "Density", %d)' % OBS_DENSITY_DEFAULT in app,
                  "OBS source defaults match FontScale %d and Density %d" % (OBS_FONT_SCALE_DEFAULT, OBS_DENSITY_DEFAULT))
            check('"%s")' % OBS_HDR_COLOR_SPACE in settings_cpp and '"%s")' % OBS_HDR_COLOR_FORMAT in settings_cpp,
                  "OBS source lists %s and %s" % (OBS_HDR_COLOR_SPACE, OBS_HDR_COLOR_FORMAT))
            basic_cpp = (obs_src / "widgets" / "OBSBasic.cpp").read_text(encoding="utf-8", errors="replace")
            check('config_get_bool(App()->GetUserConfig(), "BasicWindow", "SideDocks")' in basic_cpp,
                  "OBS source reads [BasicWindow] SideDocks from user.ini")
        else:
            print("skip OBS source key check (no " + str(obs_src) + ")")
    finally:
        shutil.rmtree(temp_root, ignore_errors=True)
    check(not os.path.exists(temp_root), "the work folder is removed")

    print("selftest: %d failure(s)" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    # 標準出力と標準エラー出力を UTF-8 にする。出力の JSON には日本語の文字列が入り、Python 3.14 までの Windows では
    # コンソールの既定のコードページ (cp1252 など) で書き出せずに例外になるため。呼び出し元 (run-window.mjs) は
    # PYTHONIOENCODING=utf-8 を付けて、UTF-8 として読む。
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8")
    sys.exit(main(sys.argv[1:]))
