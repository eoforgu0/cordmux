# SPDX-License-Identifier: Apache-2.0

# プレビューツール (ui_preview.exe) で、セットごとの画像をすべて描画し、チェックの結果を <出力先>/summary.tsv と
# summary.md にまとめる。どのセットもウィンドウを表示しない。セットの中身は sets() の前のコメントにある。
#   python render_sets.py --exe <ui_preview.exe> --out <出力先> [--sets base,classic,...] [--lowprio <lowprio.ps1>]
#   python render_sets.py --out <出力先> --summary-only [--summary-dir <フォルダー>]   描画せずに集計だけをやり直す
#   python render_sets.py --selftest   1 行目のチェックのセルフテスト (描画しない)
# --before は、前回の出力先と比べる (main() の help)。画像を読むチェックは Pillow を使う。

import argparse
import csv
import glob
import os
import re
import subprocess
import sys
import tempfile
import time

THEMES = ["yami", "yami-light"]
LANGS = ["ja-JP", "en-US"]
CLASSIC = ["--themes", "yami-classic", "--font-size", "10", "--density", "-2"]
DIALOGS = ["uninstall-confirm", "switch-confirm", "uninstall-done", "uninstall-unconfirmed", "uninstall-stopped",
           "uninstall-loose", "export-failed-write", "export-failed-zip", "export-failed-secret",
           "settings-save-failed", "rename",
           # 途中で止まったときのダイアログの違い (dialog:<ID>:<違い> は場面 dialog-<ID>-<違い> を使う)。
           "uninstall-stopped:loose", "uninstall-stopped:groups",
           # 手順書と違う置き方の確認のダイアログ (最後の文が uninstall_files_after.text)。
           "uninstall-confirm:loose",
           # 製品で起きる、途中で止まったときのダイアログ (段階 (1) で止まった形と、段階 (3) で止まって元に戻せなかった
           # 設定だけが残る形)。上の uninstall-stopped・:loose・:groups は、段階をまたいで残りを混ぜた合成の場面。
           "uninstall-stopped:settings", "uninstall-stopped:not-restored"]
DOCK_SIZES = ["--widths-em", "15,20,26,32,45", "--sizes", "230x165,300x600,420x230,600x600", "--at-min-width"]
GRID_SCENES = "people-8,people-statuses,status-first-day,record-recording,record-starting-1.5s,record-stopping-1.5s,rows-name-lengths,notice-all-video-stopped"
RD4_SCENES = "status-connected-in-call,notice-all-video-stopped,notice-many,notice-composite-absent,notice-record-not-reached"
RD3_SCENES = "status-connected-in-call,status-dropped-no-reply,status-feature-off,notice-all-video-stopped,notice-l1-two"
C2_SCENES = "people-statuses,people-stages,glance-n1"
C5_SCENES = ("glance-l1,notice-record-stopped-space,status-dropped-no-reply,notice-no-video,notice-all-video-stopped,"
             "notice-l1-two,notice-record-dir-unwritable-usual")
# 画面共有のメッセージの場面ごとに期待する、影響の文 (止まった画像の文、空の枠の文、録画の文)。表にない場面
# (notice-all-video-stopped、ほかの notice-no-video) は、枠に載っていて映像が止まった人だけなので、止まった
# 画像の文だけ。
# 製品は、録画の対象だけの人には枠の文を表示せず、枠に載っていて録画もしている人には録画の文も表示する。最初の映像が
# 届かない人の枠は空で、映像が止まった人と混ざれば、両方の文を 1 回ずつ表示する。
SHARE_NOTICE_EXPECT = {
    "notice-no-video-recording-only": (False, False, True),
    "notice-no-video-both": (True, False, True),
    "notice-no-video-slot-recording": (True, False, True),
    "notice-no-video-waiting": (False, True, False),
    "notice-no-video-mixed-waiting": (True, True, False),
    "notice-no-video-cant-show-waiting": (False, True, False),
}


def _share_expect(shot):
    """画像の名前 (場面の名前 + "-" + 大きさ) から SHARE_NOTICE_EXPECT の行を返す。いちばん長い場面の名前を優先する
    (notice-no-video-cant-show-waiting は notice-no-video-cant-show より先)。"""
    best = max((k for k in SHARE_NOTICE_EXPECT if shot.startswith(k + "-")), key=len, default=None)
    return SHARE_NOTICE_EXPECT[best] if best else (True, False, False)


# 画面共有のメッセージの場面ごとに期待する、次の操作の文 (Vesktop で映像を確認する文、画面共有の始め直しの文)。
# 表にない場面は、Vesktop で確認する文だけ。製品は、エンコードの段階が unavailable の人には始め直しの文を
# 表示する。ほかの人の映像が届いている間はメッセージにしない。
SHARE_NEXT_EXPECT = {
    "notice-no-video-cant-show": (False, True),
    "notice-no-video-mixed-stages": (True, True),
}


def _next_expect(shot):
    """画像の名前から SHARE_NEXT_EXPECT の行を返す (いちばん長い場面の名前を優先する。notice-no-video-cant-show-waiting
    は notice-no-video-cant-show の行になる)。"""
    best = max((k for k in SHARE_NEXT_EXPECT if shot.startswith(k + "-")), key=len, default=None)
    return SHARE_NEXT_EXPECT[best] if best else (True, False)


# 520x206 は、折りたたんだメッセージのレベル 1 のボタンが 1 行目に並ぶ幅の例 (classic のセットの ja で 1 行になる)。
C5_SIZES = "430x206,520x206,267x206,200x206,230x165,420x230"
# 「見出しの全文とボタンが 1 行目に入るか」を画像から読むときの、幅の余裕 (px)。文字のピクセルの右端と、フォントの
# 送りの幅 (製品が比べる幅) は、最後の文字の右の空きの分だけ 1-3 px 違う。入ると読んだ幅にこれだけの余裕があるとき
# だけ「入るのに 1 行目に並べていない」を失敗にし、余裕が足りない幅は参考にする (どちらとも言えない幅で失敗にしない)。
FIT_SLACK_PX = 4
# 状態タブで描く、メッセージのカードの場面 (画面共有の映像が届かないメッセージ、録画ファイルの作成の失敗、1 人だけ映像が
# 止まった場面 glance-n1 = メッセージにならない)。カードは状態タブだけにある。
STATUS_CARD_SCENES = "notice-all-video-stopped,notice-no-video,notice-record-finalize-failed,glance-n1"
# 停止したカードの、手順書と違う置き方と 3 つのグループの場面。430 で描画する (ドックのすべての場面を描画するセットには
# 430 がない)。
STOPPED_CARD_SCENES = ("status-uninstall-stopped-loose,status-uninstall-stopped-groups,"
                       # 製品で起きる、途中で止まった形。
                       "status-uninstall-stopped-settings,status-uninstall-stopped-not-restored")
# r17: 背の高いウィンドウを、よく使われる画面 (解像度@表示の倍率) で描画する。1366x768 の 125% と 1920x1080 の 175% は、
# 作業領域が 574 と 577 (論理 px) で最も低い。1920x1080 の 150% は 680。3840x2160 の 200% は、配置が既定の画面
# (1920x1040) と同じで、描画だけが 2 倍 (文字のピクセルの確認)。
TALL_SCREENS = ["1366x768@125", "1920x1080@150", "1920x1080@175", "3840x2160@200"]
TALL_FACES = ["dialog:uninstall-done", "dialog:uninstall-loose", "dialog:uninstall-stopped:groups",
              "dialog:uninstall-confirm", "dialog:uninstall-confirm:loose", "settings:all", "participants"]
# フォントサイズ 14 では 100% でも本文をスクロールすることがあるダイアログ (停止したダイアログは 681 px など)。最も
# 低い 2 つの画面で描画する。
TALL_F14_FACES = ["dialog:uninstall-loose", "dialog:uninstall-stopped", "dialog:uninstall-stopped:groups",
                  "dialog:uninstall-unconfirmed"]
# 空き容量が尽きて停止したカードと、その録画のセッションの録画ファイルの作成の失敗のカードが、同じドックに並ぶ場面。
M1_SCENE = "notice-l1-space-finalize-failed"
# --only は、どれかの文字列を名前に含む場面を選ぶ (faces.cpp の id_selected) ので、R07_SCENE は -recording と -idle も
# 選ぶ。
R07_SCENE = "status-dropped-vesktop-stopped"
INST_STOPPED_SCENES = "status-uninstall-stopped-settings,status-uninstall-stopped-not-restored"
# 最小幅での 2 件目のメッセージのレベル 1 の場所の QLabel と、1 行目の残りの件数の QLabel「(+1 more)」を見る場面。
DOCK459_SCENES = M1_SCENE + ",notice-l1-two"
# en で長いテキストが表示される場面 (ini のキー = 場面)。
#   vencord_elsewhere.headline = status-vencord-elsewhere (-writes-off も)、settings_unsaved.headline = added-settings-unsaved、
#   participants_unsaved = added-seen-unsaved、participants_unread.text = added-seen-unread、settings_unread.text =
#   added-settings-unread、record_not_reached = notice-record-not-reached、composite_absent.text = notice-composite-absent、
#   disconnected_auth.text = status-dropped-secret、session_not_saved.text = status-session-file-not-saved、space_low.text =
#   notice-record-space-low、no_free_slot(_short).text = people-8 など (行の押せない理由)、
#   row_not_receiving_recording.tip = notice-no-video-recording-only (ツールチップ。テキストは widgets.tsv の tooltip の
#   列だけにある)。
T1B_DOCK_SCENES = ("status-vencord-elsewhere,added-settings-unsaved,added-seen-unsaved,added-seen-unread,"
                   "added-settings-unread,notice-record-not-reached,notice-composite-absent,notice-double-audio,status-dropped-secret,"
                   "status-session-file-not-saved,notice-record-space-low,people-8,notice-no-video-recording-only")
# r19: 1 行目の短い形 (src-obs/ledger/rules_dock.cpp の kShortHeadlines) ごとに、その見出しが 1 行目に表示される
# 場面。場面は、描画した metrics.tsv の first_line_forms から選ぶ (短い形を ini のキーの名前から推測しない)。ほかの
# 場面の 1 行目に表示されない 2 つ (contact_self と、名前のある main_account_not_in_call) と、短い形を持つメッセージの
# レベル 1 が 2 件並ぶ形は、fixtures.cpp の notice-short- で始まる場面にある。
#   vesktop_not_added = status-vencord-writes-off、vesktop_add_failed = added-vesktop-add-failed、vencord_elsewhere =
#   status-vencord-elsewhere、vencord_overwritten = status-vencord-overwritten、version_differs = notice-version-restart、
#   files_mixed = notice-version-mixed、vesktop_not_connected = status-vesktop-not-linking、ports_in_use =
#   status-ports-exhausted、contact_roster = notice-contacts、contact_self = notice-short-contact-self、composite_absent =
#   notice-composite-absent、double_audio = notice-double-audio、main_account_not_in_call =
#   notice-broadcaster-not-in-call (名前のない形) と notice-short-main-account-named、no_video = notice-no-video-both (2 人) と
#   notice-no-video-recording-only (1 人 = _one)、finalize_space_short = notice-record-finalize-short、finalize_pending =
#   notice-record-finalize-pending、finalize_failed = notice-record-finalize-failed (録画中の形も)、disconnected =
#   status-dropped-no-reply、record_stopped_space = notice-record-stopped-space。
# メッセージが 2 件: notice-l1-two、notice-l1-space-finalize-failed (M1_SCENE)、notice-short-video-finalize。
R19_SCENES = ("status-vencord-writes-off,added-vesktop-add-failed,added-vesktop-remove-failed-not-restorable,"
              "status-vencord-elsewhere,status-vencord-overwritten,"
              "notice-version-restart,notice-version-mixed,status-vesktop-not-linking,status-ports-exhausted,"
              "notice-contacts,notice-short-contact-self,notice-composite-absent,notice-double-audio,"
              "notice-broadcaster-not-in-call,notice-short-main-account-named,notice-no-video-both,"
              "notice-no-video-recording-only,notice-record-finalize-short,notice-record-finalize-pending,"
              "notice-record-finalize-failed,status-dropped-no-reply,notice-record-stopped-space,notice-l1-two,"
              + M1_SCENE + ",notice-short-video-finalize")
C7_SCENES = ["settings-uninstall-ready", "settings-uninstall-streaming", "settings-uninstall-obs-recording",
             "settings-uninstall-recording",
             # リプレイバッファの動作中と、仮想カメラの動作中。
             "settings-uninstall-replay-buffer", "settings-uninstall-virtual-camera",
             "settings-uninstall-checking", "settings-default"]


def all_faces(common):
    """base と classic が描画する、すべての描画の対象 (common はテーマと [外観] の引数)。"""
    jobs = [["--face", "dock"] + DOCK_SIZES + common,
            ["--face", "menu:dock"] + common,
            ["--face", "menu:row"] + common,
            ["--face", "menu:row-voice"] + common,
            ["--face", "settings:all"] + common,
            ["--face", "settings:all:restart"] + common,
            ["--face", "settings:about:busy"] + common,
            ["--face", "settings:about", "--only", "diag-known"] + common,
            ["--face", "settings:about", "--only", "diag-export-done"] + common,
            ["--face", "settings:about", "--only", "diag-export-done-usual"] + common,
            ["--face", "settings:recording", "--only", "settings-record-dir-usual"] + common,
            ["--face", "settings:recording", "--only", "settings-record-dir-chosen"] + common,
            ["--face", "settings:about", "--only", "diag-unknown"] + common,
            ["--face", "participants"] + common,
            ["--face", "participants", "--only", "participants-current"] + common,
            ["--face", "participants", "--only", "participants-sort-name"] + common,
            ["--face", "participants", "--only", "participants-sort-last-seen-asc"] + common,
            ["--face", "participants", "--only", "participants-sort-rename-desc"] + common,
            ["--face", "focus"] + common,
            ["--face", "timeline"] + common,
            ["--face", "theme-switch"] + common,
            ["--face", "probe:menu-check"] + common]
    jobs += [["--face", "dialog:" + d] + common for d in DIALOGS]
    return jobs


# セット (sets() が返す辞書のキー)。特に書いていなければ、ja-JP と en-US を描画する。
#   base     Yami と Yami Light で、すべての描画の対象とすべての場面 (ドックは 15、20、26、32、45 em と、230x165、
#            300x600、420x230、600x600、計算した最小幅)。出力先は face-<描画の対象>/<テーマ>/<言語> の形
#   classic  Yami Classic、フォントサイズ 10、[密度] は [クラシック]。base と同じ描画の対象と場面
#   obs32    OBS 32.2.2 のテーマ (Yami と Yami Light。--obs 32)。ドックのすべての場面、設定ウィンドウ、ドックの
#            メニュー、一部のダイアログ
#   grid     [密度] 4 つ x フォントサイズ 8、10、14 (Yami)。押せる範囲と最小幅を見る場面とメニュー。拡大率 150%
#            (フォントサイズ 10 の [通常] と [クラシック]) も
#   win14    設定ウィンドウと参加者の設定ウィンドウを、フォントサイズ 14 で
#   rd3      1 行目のフォントの倍率 1.25 (既定値の 1.5 は base)
#   rd4      7 つのテーマ x メッセージのレベルのアイコン 2 種類 x メッセージのレベル 1 のパネルの枠線 3 種類。7 つのテーマでの、
#            メッセージのレベル 1 の詳細のボタンのフォーカスリングも (rd4-focus)
#   rm6      文字の上下の見切れを見る場面を、フォントサイズ 8、10、14 x 拡大率 100%、150% で
#   layout   OBS の既定のレイアウトで、シーンのドックの右に置いた cordmux のテキストの左端を、[密度] 4 つで
#   r5       行の状態、メッセージのレベル 1 の折りたたみ、下のバー、停止したカード、[ツール] からのアンインストールを
#            (メッセージのカードを見る場面は、カードのある状態タブで描画する)
#   r17      背の高いウィンドウを 4 つの画面 (--screen) で。メッセージのレベル 1 が 2 件並ぶ場面と、アンインストールが途中で
#            止まったときのダイアログとカードも
#   r18      配信中に Vesktop が終了した場面、製品で起きる途中で止まったアンインストール、場所と件数の QLabel、en の長い
#            テキスト
#   r19      1 行目の短い形のすべて (メッセージのレベル 1 が 2 件の形と、折りたたんだ形も)
#   worst    配信者のいちばん悪い条件 (30 人の各タブ、235x300、メッセージの重なり、映像途切れの行) と、状態タブの全場面。
#            Yami Classic と Yami で、ja と en
def sets():
    out = {}
    tl = ["--themes", ",".join(THEMES), "--langs", ",".join(LANGS)]
    out["base"] = all_faces(tl)
    out["classic"] = all_faces(CLASSIC + ["--langs", ",".join(LANGS)])
    obs32 = tl + ["--obs", "32"]
    out["obs32"] = [["--face", "dock", "--widths-em", "20,26", "--sizes", "230x165,420x230"] + obs32,
                    ["--face", "settings:all"] + obs32,
                    ["--face", "menu:dock"] + obs32,
                    ["--face", "dialog:uninstall-confirm"] + obs32,
                    ["--face", "dialog:uninstall-stopped"] + obs32,
                    ["--face", "dialog:uninstall-stopped:loose"] + obs32,
                    ["--face", "dialog:uninstall-stopped:groups"] + obs32]
    grid = []
    for font in ["8", "10", "14"]:
        for density in ["-2", "-3", "-4", "-5"]:
            ap = ["--themes", "yami", "--langs", ",".join(LANGS), "--font-size", font, "--density", density]
            grid.append(["--face", "dock", "--only", GRID_SCENES, "--widths-em", "26", "--sizes", "230x165,420x230",
                         "--at-min-width"] + ap)
            grid.append(["--face", "menu:dock", "--only", "status-connected-in-call"] + ap)
            grid.append(["--face", "menu:row", "--only", "people-8"] + ap)
    for density in ["-4", "-2"]:
        ap = ["--themes", "yami", "--langs", ",".join(LANGS), "--font-size", "10", "--density", density, "--dpr", "1.5"]
        grid.append(["--face", "dock", "--only", GRID_SCENES, "--widths-em", "26", "--sizes", "230x165,420x230",
                     "--at-min-width"] + ap)
        grid.append(["--face", "menu:row", "--only", "people-8"] + ap)
    out["grid"] = grid
    out["win14"] = [["--face", "settings:all", "--font-size", "14"] + tl,
                    ["--face", "settings:all:restart", "--font-size", "14"] + tl,
                    ["--face", "participants", "--only", "participants-current", "--font-size", "14"] + tl,
                    ["--face", "participants", "--only", "participants-sort-name", "--font-size", "14"] + tl,
                    ["--face", "settings:recording", "--only", "settings-record-dir-chosen", "--font-size", "14"] + tl,
                    ["--face", "settings:about", "--only", "diag-export-done-usual", "--font-size", "14"] + tl,
                    ["--face", "settings:recording", "--only", "settings-record-dir-usual", "--font-size", "14"] + tl,
                    ["--face", "settings:all", "--font-size", "14"] + CLASSIC[:2] + ["--langs", ",".join(LANGS),
                                                                             "--density", "-2"]]
    for face, scene in [("settings:about", "diag-export-done-usual"), ("settings:recording", "settings-record-dir-usual")]:
        out["win14"].append(["--face", face, "--only", scene, "--font-size", "14"] + CLASSIC[:2] +
                            ["--langs", ",".join(LANGS), "--density", "-2"])
    out["rd3"] = [["--face", "dock", "--only", RD3_SCENES, "--widths-em", "none", "--sizes", "230x165,420x230",
                   "--first-line-scale", "1.25"] + tl]
    rd4 = []
    themes7 = "yami,yami-grey,yami-classic,yami-acri,yami-rachni,yami-light,system"
    for icons in ["theme", "windows"]:
        for border in ["-1", "1", "2"]:
            rd4.append(["--face", "dock", "--only", RD4_SCENES, "--widths-em", "26", "--themes", themes7,
                        "--langs", ",".join(LANGS), "--tier-icons", icons, "--l1-border", border,
                        "--subdir", "rd4-%s-b%s" % (icons, border.replace("-", "auto"))])
    # 詳細のボタンのフォーカスリングを、7 つのテーマのメッセージのレベル 1 のパネルの上で描画する。
    rd4.append(["--face", "focus", "--only", "notice-all-video-stopped,notice-many", "--themes", themes7,
                "--langs", ",".join(LANGS), "--subdir", "rd4-focus"])
    out["rd4"] = rd4
    rm6 = []
    for font in ["8", "10", "14"]:
        for dpr in ["1", "1.5"]:
            rm6.append(["--face", "dock", "--only", "rows-glyphs,rows-name-lengths,notice-all-video-stopped",
                        "--widths-em", "26,45", "--themes", "yami", "--langs", ",".join(LANGS),
                        "--font-size", font, "--dpr", dpr])
    out["rm6"] = rm6
    # OBS のシーンのドックの右に置いた cordmux のテキストの左端を、[密度] 4 つで並べる ([クラシック] は Yami Classic、
    # ほかは Yami。フォントサイズ 10、ja)。
    layout = [["--face", "probe:obs-layout", "--only", "beside-scenes", "--themes", "yami-classic", "--font-size", "10",
               "--density", "-2", "--langs", "ja-JP"]]
    for density in ["-3", "-4", "-5"]:
        layout.append(["--face", "probe:obs-layout", "--only", "beside-scenes", "--themes", "yami", "--font-size", "10",
                       "--density", density, "--langs", "ja-JP"])
    out["layout"] = layout
    # r5: メッセージのレベル 1 の場面 (6 つの大きさ)、下のバー (230、267、430)。どれも base と classic で、ja と en。
    # 行の状態のテキストは、Yami と Yami Light x [通常] と [クラシック] x 230、267、430 (classic でも同じ場面を
    # 描画する)。
    r5 = []
    for name, look in [("base", tl), ("classic", CLASSIC + ["--langs", ",".join(LANGS)])]:
        r5.append(["--face", "dock", "--only", C5_SCENES, "--widths-em", "none", "--sizes", C5_SIZES] + look +
                  ["--subdir", name + "-c5-open"])
        r5.append(["--face", "dock", "--only", "record-idle,record-recording", "--widths-em", "none",
                   "--sizes", "230x165,267x165,430x165"] + look + ["--subdir", name + "-c6"])
        # 停止したカードの手順書と違う置き方と 3 つのグループを 430 で描画する (26 em は、base と classic のドックのすべての
        # 場面が描画する)。高さは、カードの全体がスクロール領域に入る 600 (グループの間のチェックは、見える範囲の
        # 行だけを数える)。
        r5.append(["--face", "dock", "--tab", "status", "--only", STOPPED_CARD_SCENES, "--widths-em", "none",
                   "--sizes", "430x600"] + look + ["--subdir", name + "-stopped"])
        # 画面共有のメッセージと録画ファイルの作成の失敗のカードを、状態タブで描画する (430 は文の行数、267 は狭い幅)。
        r5.append(["--face", "dock", "--tab", "status", "--only", STATUS_CARD_SCENES, "--widths-em", "none",
                   "--sizes", "430x600,267x600"] + look + ["--subdir", name + "-status-cards"])
        # [ツール] の「cordmuxをアンインストール...」の後の設定ウィンドウ (押せる場面 1 つと、押せない場面)。
        for scene in C7_SCENES:
            r5.append(["--face", "settings:about:uninstall", "--only", scene] + look + ["--subdir", name + "-c7"])
    # 行の状態のテキストは、それを表示するいちばん狭い幅 (--at-status-floor。画像の名前の大きさは floor。状態の
    # テキストの QLabel は下限の 5.75 em) でも描画する (その幅でも短い形を省略せずに表示するかを見る)。
    for density in ["-4", "-2"]:
        r5.append(["--face", "dock", "--only", C2_SCENES, "--widths-em", "none", "--sizes", "230x600,267x600,430x600",
                   "--at-status-floor", "--font-size", "10", "--density", density] + tl +
                  ["--subdir", "c2-d" + density[1:]])
    r5.append(["--face", "dock", "--only", C2_SCENES, "--widths-em", "none", "--sizes", "230x600,267x600,430x600",
               "--at-status-floor"] + CLASSIC + ["--langs", ",".join(LANGS), "--subdir", "classic-c2"])
    out["r5"] = r5
    # r17: (1) 背の高いウィンドウを 4 つの画面で、classic と base の ja と en で描画する。出力先は
    # tall-<セット>-<解像度>-<倍率>。フォントサイズ 14 (Yami) は、最も低い 2 つの画面で描画する (tall-f14-...)。
    # (2) メッセージのレベル 1 が 2 件並ぶ場面を、20 em と 26 em で (m1-<セット>)。高さは、2 件の全体がスクロール領域に入る
    # 900 と、普通の 600。(3) アンインストールが途中で止まったときのダイアログとカード (製品で起きる段階 (1)・(3) の
    # 形と、合成の場面) を 20 em と 26 em で (uninstall-stopped-<セット>)。
    r17 = []
    looks = [("classic", CLASSIC + ["--langs", ",".join(LANGS)]), ("base", tl)]
    for screen in TALL_SCREENS:
        tag = screen.replace("@", "-")
        for name, look in looks:
            for face in TALL_FACES:
                r17.append(["--face", face, "--screen", screen] + look + ["--subdir", "tall-%s-%s" % (name, tag)])
    # チェックのセルフテスト (スクロールを止めたウィンドウでチェックが失敗するか。faces.cpp の face_probe_screen_fit)。
    # 最も低い画面で描画する。
    for name, look in looks:
        r17.append(["--face", "probe:screen-fit", "--screen", TALL_SCREENS[0]] + look +
                   ["--subdir", "tall-%s-%s" % (name, TALL_SCREENS[0].replace("@", "-"))])
    for screen in TALL_SCREENS[:1] + TALL_SCREENS[2:3]:
        tag = screen.replace("@", "-")
        for face in TALL_F14_FACES:
            r17.append(["--face", face, "--screen", screen, "--themes", "yami", "--langs", ",".join(LANGS),
                        "--font-size", "14", "--subdir", "tall-f14-%s" % tag])
    for name, look in looks:
        for height in ["600", "900"]:
            r17.append(["--face", "dock", "--only", M1_SCENE, "--widths-em", "20,26", "--height", height] + look +
                       ["--subdir", "m1-%s-h%s" % (name, height)])
        r17.append(["--face", "dock", "--only", "status-uninstall-stopped", "--widths-em", "20,26", "--height", "900"] +
                   look + ["--subdir", "uninstall-stopped-%s" % name])
        # 状態タブ (カードは状態タブだけにある)。
        r17.append(["--face", "dock", "--tab", "status", "--only", M1_SCENE, "--widths-em", "20,26", "--height", "900"] +
                   look + ["--subdir", "m1-status-%s" % name])
        r17.append(["--face", "dock", "--tab", "status", "--only", "status-uninstall-stopped", "--widths-em", "20,26",
                    "--height", "900"] + look + ["--subdir", "uninstall-stopped-status-%s" % name])
        for d in ["uninstall-stopped", "uninstall-stopped:groups", "uninstall-stopped:settings",
                  "uninstall-stopped:not-restored"]:
            r17.append(["--face", "dialog:" + d] + look + ["--subdir", "uninstall-stopped-%s" % name])
    out["r17"] = r17
    # r18: どれも classic と base で描画する。出力先は <中身>-<セット>。
    # (1) 配信中に Vesktop が終了した場面 (メッセージのレベル 1 の切断のカード。録画中の形と、配信していないときの
    #     VesktopNotRunning の平常の 1 行になる形も) を、ja と en、20 em と 26 em で (vesktop-stopped-<セット>)。
    # (2) 製品で起きる、途中で止まったアンインストール (段階 (1) の -settings、段階 (3) の -not-restored) のカードを
    #     20 em と 26 em (高さ 900) で、ダイアログを既定の画面で、ja と en で (inst-<セット>)。
    # (3) メッセージのレベル 1 が 2 件並ぶ場面を、20 em、26 em と計算した最小幅で、ja と en で (dock459-<セット>)。
    # (4) en の長いテキストを、表示される場所 (ドックの見出しと本文を 15 em、20 em、26 em と最小幅で。設定ウィンドウの
    #     すべてのページ) で、en だけ (t1b-<セット>)。ツールチップと、Windows のフォルダーを選ぶダイアログのタイトルは
    #     描画しない。
    r18 = []
    looks_en = [("classic", CLASSIC + ["--langs", "en-US"]),
                ("base", ["--themes", ",".join(THEMES), "--langs", "en-US"])]
    for name, look in looks:
        r18.append(["--face", "dock", "--only", R07_SCENE, "--widths-em", "20,26"] + look + ["--subdir", "vesktop-stopped-%s" % name])
        # 状態タブ (切断のカードと、停止したアンインストールのカードは状態タブだけにある)。
        r18.append(["--face", "dock", "--tab", "status", "--only", R07_SCENE, "--widths-em", "20,26"] + look +
                   ["--subdir", "vesktop-stopped-status-%s" % name])
        r18.append(["--face", "dock", "--tab", "status", "--only", INST_STOPPED_SCENES, "--widths-em", "20,26",
                    "--height", "900"] + look + ["--subdir", "inst-status-%s" % name])
        r18.append(["--face", "dock", "--only", INST_STOPPED_SCENES, "--widths-em", "20,26", "--height", "900"] + look +
                   ["--subdir", "inst-%s" % name])
        for d in ["uninstall-stopped:settings", "uninstall-stopped:not-restored"]:
            r18.append(["--face", "dialog:" + d] + look + ["--subdir", "inst-%s" % name])
        r18.append(["--face", "dock", "--only", DOCK459_SCENES, "--widths-em", "20,26", "--at-min-width"] + look +
                   ["--subdir", "dock459-%s" % name])
    for name, look in looks_en:
        r18.append(["--face", "dock", "--only", T1B_DOCK_SCENES, "--widths-em", "15,20,26", "--at-min-width"] + look +
                   ["--subdir", "t1b-%s" % name])
        r18.append(["--face", "settings:all"] + look + ["--subdir", "t1b-%s" % name])
    out["r18"] = r18
    # r19: 1 行目の短い形のすべてを、20 em と 26 em、ja と en、classic と base で描画する (short-<セット>。状態タブでも
    # 描画する = short-status-<セット>)。
    # メッセージのレベル 1 が 2 件の 3 つの場面を含む。チェックは first_line_checks (r19 も短い形のチェック (1) に入れる) と、short_form_coverage
    # (ほかのセットの 1 行目に表示された短い形が、r19 にすべてあるか)。
    r19 = []
    for name, look in looks:
        r19.append(["--face", "dock", "--only", R19_SCENES, "--widths-em", "20,26"] + look + ["--subdir", "short-%s" % name])
        # 状態タブ (1 行目が短い形のとき、カードの本文の先頭が見出しの全文かを見る)。
        # 高さは、2 件のカードと表の全体がスクロール領域に入る 1200 (入らないと、見えない行のピクセルを読んで、コントラストの
        # チェックが背景と同じ色の文字を拾う)。
        r19.append(["--face", "dock", "--tab", "status", "--only", R19_SCENES, "--widths-em", "20,26", "--height", "1200"] +
                   look + ["--subdir", "short-status-%s" % name])
    out["r19"] = r19
    out["worst"] = worst_jobs()
    return out


# 状態タブの全場面。--only は名前に含む場面を選ぶので、status-recording は status-recording-button にも一致する。
STATUS_TAB_SCENES = ("status-empty,status-messages,status-finalize,status-recording,status-last-recording,status-table,"
                     "status-table-30,status-no-composite,status-history-60")


def worst_jobs():
    """worst のセットの描画。30 人、235x300、メッセージの重なり、映像途切れの行、状態タブの全場面。"""
    jobs = []
    for name, look in [("classic", CLASSIC + ["--langs", ",".join(LANGS)]),
                       ("base", ["--themes", "yami", "--langs", ",".join(LANGS)])]:
        # 30 人。参加者タブ、音声タブ、状態タブ。
        for tab, scene in [("people", "people-30"), ("audio", "audio-tab-30"), ("status", "status-table-30")]:
            jobs.append(["--face", "dock", "--tab", tab, "--only", scene, "--widths-em", "26", "--height", "640"] + look +
                        ["--subdir", "worst-%s-30-%s" % (name, tab)])
        # 235x300 (最小の幅より狭い)。参加者タブ、音声タブ、状態タブ。
        for tab, scenes in [("people", "people-statuses,people-30"), ("audio", "audio-tab-30"),
                            ("status", "status-messages,notice-tiers-overlap")]:
            jobs.append(["--face", "dock", "--tab", tab, "--only", scenes, "--widths-em", "none", "--sizes", "235x300"] +
                        look + ["--subdir", "worst-%s-235-%s" % (name, tab)])
        # メッセージの重なり (1 行目は 1 つ、状態タブにすべて)。
        jobs.append(["--face", "dock", "--tab", "status", "--only", "notice-tiers-overlap,notice-many,notice-l1-two",
                     "--widths-em", "26", "--height", "700"] + look + ["--subdir", "worst-%s-overlap" % name])
        jobs.append(["--face", "dock", "--only", "notice-tiers-overlap,glance-share-row", "--widths-em", "26",
                     "--height", "700"] + look + ["--subdir", "worst-%s-people" % name])
        # 状態タブの全場面。
        jobs.append(["--face", "dock", "--tab", "status", "--only", STATUS_TAB_SCENES, "--widths-em", "26",
                     "--height", "700"] + look + ["--subdir", "worst-%s-status" % name])
    return jobs


# ui_preview.exe を 1 回実行し、出力を log に追記して終了コードを返す。--exe の実行ファイルを直接呼ぶ
# (run_ui_preview.py はビルドのフォルダーが固定なので使わない)。lowprio を渡すと、PowerShell の lowprio.ps1 を通して
# 実行する (PowerShell が --out を自分の引数と取り違えるので、.cmd を経由する)。渡さなければ、Windows の「通常以下」の
# 優先度で実行する。
def run(exe, args, log, lowprio):
    if lowprio:
        def q(a):
            return '"' + a + '"' if any(c in a for c in " ,;") else a
        fd, cmd = tempfile.mkstemp(suffix=".cmd", dir=os.path.dirname(log))
        os.close(fd)
        with open(cmd, "w", encoding="utf-8") as f:
            f.write("@echo off\r\nchcp 65001 >nul\r\n")
            f.write(" ".join([q(exe)] + [q(a) for a in args]) + ' >> "%s" 2>&1\r\n' % log)
            f.write("exit /b %ERRORLEVEL%\r\n")
        r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", lowprio,
                            "cmd.exe", "/c", cmd])
        os.remove(cmd)
        return r.returncode
    with open(log, "a", encoding="utf-8") as f:
        r = subprocess.run([exe] + args, stdout=f, stderr=subprocess.STDOUT,
                           creationflags=getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0))
    return r.returncode


def read_tsv(path):
    with open(path, encoding="utf-8") as f:
        return list(csv.DictReader(f, delimiter="\t", quoting=csv.QUOTE_NONE))


# 計算した最小幅の上限 (px。フォントサイズ 10 の値)。これを超えたら、ドックが広くなりすぎたことに気づく。中身がフォントに
# 比例して広がるので、フォントサイズが 10 より大きい出力先 (出力先の名前の f<フォントサイズ>) は、比例して上げる。
MIN_WIDTH_LIMIT_PX = 300


def min_width_limit_px(scope):
    """出力先の相対パスのフォントサイズ (f<フォントサイズ>。なければ 10) から、計算した最小幅の上限 (px) を返す。"""
    font_pt = 10.0
    for part in scope.replace("\\", "/").split("/"):
        for piece in part.split("-"):
            if piece.startswith("f") and piece[1:].isdigit():
                font_pt = float(piece[1:])
    return int(round(MIN_WIDTH_LIMIT_PX * max(font_pt, 10.0) / 10.0))
_REQUEST_PX = re.compile(r"-(\d+)x(\d+)$")
_REQUEST_EM = re.compile(r"-(\d+(?:\.\d+)?)em$")


def unreached_sizes(rows):
    """metrics.tsv の行から、求めた大きさに届かなかった場面の名前を返す。

    求めた幅がドックの計算した最小幅より狭く、幅が最小幅に止まった場面は、届かないのが正しい動きなので数えない。
    高さも、求めた高さより大きいだけなら、ドックの最小の高さに止まったものとして数えない。ほかの理由で届かないもの
    (幅が最小幅でも求めた幅でもない、高さが求めた高さより小さい、求めた大きさを名前から読めない) は数える。
    求めた幅は場面の名前 (-<幅>x<高さ> か -<数>em) から読む。em の px は、届いた行の幅と em の比から求める。
    """
    em_px = next((int(x["width"]) / float(x["width_em"]) for x in rows
                  if x["size_reached"] == "ok" and float(x["width_em"] or 0) > 0), None)
    out = []
    for x in rows:
        if x["size_reached"] == "ok":
            continue
        width, height, min_width = int(x["width"]), int(x["height"]), int(x["min_width"] or 0)
        px = _REQUEST_PX.search(x["scene"])
        em = _REQUEST_EM.search(x["scene"])
        if px:
            want_w, want_h = int(px.group(1)), int(px.group(2))
        elif em and em_px:
            want_w, want_h = float(em.group(1)) * em_px, None
        else:
            out.append(x["scene"])
            continue
        width_ok = width == want_w or (want_w < min_width and width == min_width)
        height_ok = want_h is None or height >= want_h
        if not (width_ok and height_ok):
            out.append(x["scene"])
    return out


def window_overflow(rows):
    """ウィンドウの widgets.tsv の行から、表示されているウィジェットのうち、スクロール領域 (なければウィンドウ) の右端を
    超えるものの短い説明の一覧を返す。

    スクロール領域の右端を超え、かつ左端がその領域の中から始まるウィジェット (スクロール領域の中身) を数える。
    ウィンドウの右端を超えるウィジェットも数える。スクロールバー (QScrollBar) は数えない。
    """
    if not rows:
        return []
    win = rows[0]
    win_right = int(win["x"]) + int(win["w"])
    scrolls = [r for r in rows if r["class"] == "QScrollArea" and r["visible"] == "1"]
    scroll_right = max((int(r["x"]) + int(r["w"]) for r in scrolls), default=win_right)
    bad = []
    for r in rows[1:]:
        if r["visible"] != "1" or r["class"] == "QScrollBar":
            continue
        try:
            left = int(r["x"])
            right = left + int(r["w"])
        except ValueError:
            continue
        if (right > scroll_right + 1 and left < scroll_right) or right > win_right:
            bad.append("%s %s %d..%d" % (r["class"], (r["text"] or r["object_name"])[:24], left, right))
    return bad


_LINE2_KEYS = {"voice": "cordmux.dock.row_voice.status", "voice_absent": "cordmux.dock.row_voice_absent.status",
               "discord": "cordmux.dock.row_discord_name.status", "username": "cordmux.dock.row_username.status"}
_LINE2_TEMPLATES = {}


def line2_templates(lang):
    """2 行目の部分のテキストの形 (ini の値。%1 が値) を言語ごとに読み、{kind: (前の文字列, 後ろの文字列)} を返す。"""
    if lang not in _LINE2_TEMPLATES:
        ini = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "data", "locale", lang + ".ini")
        values = {}
        try:
            with open(ini, encoding="utf-8") as f:
                for line in f:
                    k, sep, v = line.rstrip("\n").partition("=")
                    if sep:
                        values[k] = v.strip().strip('"')
        except OSError:
            pass
        out = {}
        for kind, key in _LINE2_KEYS.items():
            if key in values and "%1" in values[key]:
                head, _, tail = values[key].partition("%1")
                out[kind] = (head, tail)
        _LINE2_TEMPLATES[lang] = out
    return _LINE2_TEMPLATES[lang]


def _part_of(text, templates):
    """2 行目の 1 つの部分の全文から (種類, 値) を返す。どの形にも合わなければ None。"""
    # 後ろの文字列が長い形 (通話に参加していない人の声) から先に試す。
    for kind in sorted(templates, key=lambda k: -len(templates[k][1])):
        head, tail = templates[kind]
        if text.startswith(head) and text.endswith(tail) and len(text) > len(head) + len(tail):
            return (kind, text[len(head):len(text) - len(tail)] if tail else text[len(head):])
    return None


def _fits(shown, full):
    """表示されているテキスト shown が、全文 full を省略した形か (同じか、省略記号の前と後ろが全文の先頭と末尾に
    一致する)。"""
    ell = chr(0x2026)  # Qt の省略記号 (U+2026)
    if shown == full:
        return True
    if ell in shown:
        head, _, tail = shown.partition(ell)
        tail = tail.replace(ell, "")
        return full.startswith(head) and full.endswith(tail) and len(head) + len(tail) < len(full)
    return False


def line2_checks(rows, lang):
    """names.tsv の行のうち、2 行目に 2 つ以上の事実がある行を、製品が 2 行目を縮める順でチェックする。

    すべての部分の全文は、アクセシブルな説明 (a11y_desc。枠に載せる + 2 行目のすべて + 理由) から ini のテキストの形で
    拾い、表示されている部分 (line2_shown) を並びの順に当てはめる。(チェックした行の数, ユーザー名の末尾が消えた行,
    事実の先頭が消えた行, 順が崩れた行) を返す。行は「画像の名前:人」。
      - ユーザー名の末尾: ユーザー名の部分があるのに表示されていない、または表示されている末尾 (省略記号の後。
        省略していなければすべて) が空か、値の末尾でないか、同じ画像のほかの人のユーザー名の末尾にも一致する (見分けに
        使えない)。
      - 事実の先頭: Discord での名前の部分を除く部分が表示されていない、または先頭 (「声: 」と値の 1 文字目) が
        表示されていない。
      - 順: Discord での名前の部分が表示されているのに、ほかの部分が縮められている (先に省くのは Discord での名前)。
    """
    templates = line2_templates(lang)
    checked = 0
    lost_tail, lost_head, order = [], [], []
    # 同じ画像の中のすべてのユーザー名 (表示されている末尾が、ほかの人のユーザー名の末尾にも一致するかを見る)。
    usernames = {}
    for x in rows:
        desc = x.get("a11y_desc", "").replace("\\n", "\n").split("\n")
        for p in (_part_of(d, templates) for d in desc):
            if p is not None and p[0] == "username":
                usernames.setdefault(x["shot"], []).append((x["user"], p[1]))
    for x in rows:
        desc = x.get("a11y_desc", "").replace("\\n", "\n").split("\n")
        parts = [p for p in (_part_of(d, templates) for d in desc) if p is not None]
        if len(parts) < 2:
            continue
        checked += 1
        who = "%s:%s" % (x["shot"], x["user"])
        shown = [s for s in x.get("line2_shown", "").split(" | ") if s]
        fulls = [d for d in desc if _part_of(d, templates) is not None]
        # 表示されている部分を、並びの順に全文へ当てはめる (省いた部分は飛ばす)。
        assigned = {}
        i = 0
        for s in shown:
            while i < len(parts):
                kind, value = parts[i]
                if _fits(s, fulls[i]) or (kind == "username" and _fits(s, value)):
                    assigned[i] = s
                    i += 1
                    break
                i += 1
        shortened_other = False
        for idx, (kind, value) in enumerate(parts):
            s = assigned.get(idx)
            if kind == "username":
                if s is None:
                    tail = ""
                elif chr(0x2026) in s:
                    tail = s.rpartition(chr(0x2026))[2]
                else:
                    tail = value  # 省略していない (前の文字列のある全文か、値だけの形)
                others = [v for u, v in usernames.get(x["shot"], []) if u != x["user"] and v != value]
                if not tail or not value.endswith(tail) or any(v.endswith(tail) for v in others):
                    lost_tail.append(who)
            if kind != "discord":
                head = templates[kind][0]
                if s is None or not (s.startswith(head + value[:1]) or (kind == "username" and s.startswith(value[:1]))):
                    lost_head.append(who)
                if s is not None and s != fulls[idx]:
                    shortened_other = True
        if shortened_other and any(k == "discord" and i in assigned for i, (k, _) in enumerate(parts)):
            order.append(who)
    return checked, sorted(set(lost_tail)), sorted(set(lost_head)), sorted(set(order))


_DIGITS = set("0123456789" + "".join(chr(0xFF10 + i) for i in range(10)))
_NUM_JOIN = set(":：.%％")


def _unescape(cell):
    """tsv_cell (measure.cpp) が書いた \\\\、\\t、\\n を元の文字に戻す。"""
    out, i = [], 0
    while i < len(cell):
        c = cell[i]
        if c == "\\" and i + 1 < len(cell):
            n = cell[i + 1]
            out.append({"\\": "\\", "t": "\t", "n": "\n", "r": "\r"}.get(n, "\\" + n))
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def _is_unit_start(c):
    """数の直後に来て、単位の語の先頭になりうる文字 (漢字、カタカナ、「つ」)。ひらがなの助詞 (「13:41」/「に」の
    「に」) は単位ではないので数えない (数えると、正しい折り返しまで拾ってしまう)。"""
    o = ord(c)
    return 0x30A0 <= o <= 0x30FF or 0x3400 <= o <= 0x9FFF or 0xF900 <= o <= 0xFAFF or 0xFF66 <= o <= 0xFF9D or c == "つ"


def number_breaks(breaks, spaces=""):
    """wraps.tsv の breaks (「前の行の末尾 3 文字 / 次の行の先頭 3 文字」を「 ; 」でつないだもの) から、数と単位の語が
    分かれた境を拾う。

    (strict, en) を返す。strict は、数と単位の語を行で分けない規則に反する境 (数の途中、時刻の「:」の前後、数と日本語の
    単位の文字の間)。en は、数で終わる行の次の行が英字で始まる境 (「2」/「people」。規則の例にないので、数えずに書く
    だけで、人が見て決める)。spaces は wraps.tsv の break_spaces (境ごとに空白をはさんだかの 1/0 を「 ; 」でつないだ
    もの。列のない表では空)。空白をはさんだ数字と数字の境 (セッションのフォルダーの名前「2000-01-01 」/「12-00-00」) は
    別の 2 つの語の間で、数の途中ではないので strict に数えない (製品の calc::number_word_spans も、空白をはさむ数を
    1 つの語にしない)。en と、数字と単位の文字の間は、空白をはさんでも数える。
    """
    strict, en = [], []
    flags = _unescape(spaces).split(" ; ") if spaces else []
    for i, b in enumerate(_unescape(breaks).split(" ; ")):
        if not b:
            continue
        a, sep, c = b.partition(" / ")
        if not sep or not a or not c:
            continue
        spaced = i < len(flags) and flags[i] == "1"
        x, y = a[-1], c[0]
        if x in _DIGITS and y in _DIGITS and spaced:
            continue
        if x in _DIGITS and (y in _DIGITS or y in _NUM_JOIN or _is_unit_start(y)):
            strict.append(b)
        elif x in _NUM_JOIN and len(a) >= 2 and a[-2] in _DIGITS and y in _DIGITS:
            strict.append(b)
        elif x in _DIGITS and y.isascii() and y.isalpha():
            en.append(b)
    return strict, en


def wrap_blank_px(row, png_cache):
    """折り返す QLabel の下の空き (QLabel の下端と、文字が描画された最後の行の差。px) を、画像のピクセルで数える。
    数えられなければ None を返す。

    QLabel の矩形の中で、背景の色 (QLabel の右下の内側) と違うピクセルのある最後の行を探す。QLabel が親で切られている
    (スクロール領域の外にある) ときは数えない。
    """
    try:
        from PIL import Image, ImageChops
    except ImportError:
        return None
    path = row["_png"]
    if path not in png_cache:
        try:
            png_cache.clear()
            png_cache[path] = Image.open(path).convert("RGB")
        except OSError:
            png_cache[path] = None
    img = png_cache[path]
    if img is None:
        return None
    # ウィジェットの矩形は論理 px、画像は拡大率 (出力先の名前の -x<百分率>) を掛けた px。空きも論理 px で返す。
    s = row.get("_scale", 1.0)
    x, y = int(round(int(row["x"]) * s)), int(round(int(row["y"]) * s))
    w, h = int(round(int(row["w"]) * s)), int(round(int(row["h"]) * s))
    if w < 4 or h < 4 or x < 0 or y < 0 or x + w > img.width or y + h > img.height:
        return None
    crop = img.crop((x, y, x + w, y + h))
    bg = crop.getpixel((w - 2, h - 2))
    diff = ImageChops.difference(crop, Image.new("RGB", crop.size, bg)).convert("L").point(lambda v: 255 if v > 40 else 0)
    box = diff.getbbox()
    if box is None:
        return None
    return int(round((h - box[3]) / s))


def wraps_checks(set_dir, add, set_name):
    """wraps.tsv (faces.cpp の wrap_rows) のチェック。折り返す QLabel の行と高さと、場所の QLabel を見る。"""
    png_cache = {}
    for wt in sorted(glob.glob(os.path.join(set_dir, "**", "wraps.tsv"), recursive=True)):
        scope = os.path.relpath(os.path.dirname(wt), set_dir)
        rows = read_tsv(wt)
        scale = 1.0
        for piece in os.path.basename(os.path.dirname(os.path.dirname(wt))).split("-"):
            if piece.startswith("x") and piece[1:].isdigit():
                scale = int(piece[1:]) / 100.0
        for r in rows:
            r["_png"] = os.path.join(os.path.dirname(wt), r["shot"] + ".png")
            r["_scale"] = scale
        wrapped = [r for r in rows if r.get("wrap", "yes") == "yes"]
        # 数と単位の語の分かれ。en の「数 / 英字」は書くだけ。
        strict, en = [], []
        for r in wrapped:
            s, e = number_breaks(r["breaks"], r.get("break_spaces") or "")
            strict += ["%s %s" % (r["shot"], b) for b in s]
            en += ["%s %s" % (r["shot"], b) for b in e]
        add(set_name, "number and unit split across lines", scope, len(wrapped), len(strict), "; ".join(strict)[:300])
        add(set_name, "en digit at line end then a word (record only)", scope, len(wrapped), 0,
            "%d: %s" % (len(en), "; ".join(en)[:280]) if en else "")
        # 最後の行が 2 文字以下 (カードとウィンドウ)。ドックでは notice_key のある QLabel (カードの見出しと段落) と
        # スクロール領域の中の文、ウィンドウではすべての折り返す QLabel。
        short = [r for r in wrapped if int(r["lines"] or 0) >= 2 and 0 < int(r["last_graphemes"] or 0) <= 2]
        add(set_name, "last line of 2 graphemes or fewer", scope, len(wrapped), len(short),
            "; ".join(sorted({"%s %s/%s" % (r["shot"], _unescape(r["breaks"]).split(" ; ")[-1], _unescape(r["last_line"]))
                              for r in short}))[:300])
        # 改行の位置を自分で決める QLabel (数と単位を分けない QLabel) が、Qt の折り返し方より行を増やした (製品は、行の
        # 数を増やさない範囲で改行の位置を決める)。場所の QLabel は除く (区切りで折り返すので、行が増えるのは
        # 規則のうち)。
        more = [r for r in wrapped if r["path"] != "yes" and r.get("qt_lines") and
                int(r["lines"] or 0) > int(r["qt_lines"])]
        add(set_name, "managed breaks add lines over Qt's", scope, len(wrapped), len(more),
            "; ".join("%s %s %s>%s" % (r["shot"], _unescape(r["text"]).replace(" ", "/")[:24], r["lines"], r["qt_lines"])
                      for r in more)[:300])
        # QLabel がテキストより高い (下に空きがある)。親で切られている QLabel と、高い行の中で縦の中央に置いた 1 行の
        # QLabel は除く。ピクセルで数え、形 (h と組んだテキストの高さ) は詳細に書く。1 行の QLabel が上に寄って高い行
        # (隣のボタンの高さ) に置かれたものは、別に数える (テキストが、隣のボタンのテキストと縦にそろわない)。
        blank, one_line = [], []
        for r in wrapped:
            h, vis, lp = int(r["h"]), int(r["visible_h"] or 0), max(1, int(r["line_px"] or 1))
            if vis < h or (r["valign"] == "vcenter" and int(r["lines"] or 0) <= 1):
                continue
            px = wrap_blank_px(r, png_cache)
            if px is not None and px >= 0.6 * lp:
                item = "%s %s h=%d text_h=%s hfw=%s blank=%d" % (r["shot"], _unescape(r["text"])[:16], h, r["text_h"],
                                                                r["hfw"], px)
                (one_line if int(r["lines"] or 0) <= 1 and h < 2 * lp else blank).append(item)
        add(set_name, "wrapped label taller than its text", scope, len(wrapped), len(blank), "; ".join(blank)[:300])
        add(set_name, "1-line label top-aligned in a taller row", scope, len(wrapped), len(one_line),
            "; ".join(one_line)[:300])
        # QLabel がテキストより低い (下の行が切れる)。
        cut = [r for r in wrapped if int(r["text_h"] or 0) > int(r["h"]) + 1]
        add(set_name, "wrapped label shorter than its text", scope, len(wrapped), len(cut),
            "; ".join("%s %s h=%s text_h=%s" % (r["shot"], _unescape(r["text"])[:16], r["h"], r["text_h"]) for r in cut)[:300])
        # 場所の QLabel の全文が見えて、コピーできる (省略していない、右も下も切れていない、親で切られていない、コピー
        # すると全文になる)。読み取り専用の 1 行の入力欄 (QLineEdit) は、fields.tsv でチェックする。
        paths = [r for r in rows if r["path"] == "yes"]
        bad = []
        for r in paths:
            why = []
            if r["elided"] == "yes":
                why.append("elided")
            if int(r["text_h"] or 0) > int(r["h"]) + 1:
                why.append("cut below")
            if r.get("wrap") == "no" and int(r.get("text_px") or 0) > int(r["wrap_w"]):
                why.append("cut right")
            # スクロール領域の見える範囲の外にある行 (スクロールすれば見える) は数えない。inner_h の列がない表では
            # visible_h を使う。
            if int(r.get("inner_h") or r["visible_h"] or 0) < int(r["h"]):
                why.append("clipped by parent")
            if r["copy_clean"] == "no":
                why.append("copy not the path")
            if why:
                bad.append("%s %s (%s)" % (r["shot"], _unescape(r["text"])[-24:], ",".join(why)))
        add(set_name, "location label full text not visible", scope, len(paths), len(bad), "; ".join(bad)[:300])


_INI = {}
LS = chr(0x2028)  # 行区切り文字 (WrapLabel が改行の位置に入れる)


def ini_values(lang):
    """cordmux の ini (言語ごと) のキーから値への辞書 (引用符を外し、\\" を " に戻したもの)。"""
    if lang not in _INI:
        ini = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "data", "locale", lang + ".ini")
        values = {}
        try:
            with open(ini, encoding="utf-8") as f:
                for line in f:
                    k, sep, v = line.rstrip("\n").partition("=")
                    if sep and not k.startswith("#"):
                        v = v.strip()
                        if len(v) >= 2 and v[0] == '"' and v[-1] == '"':
                            v = v[1:-1]
                        values[k.strip()] = v.replace('\\"', '"')
        except OSError:
            pass
        _INI[lang] = values
    return _INI[lang]


def _lang_of(path):
    """出力先の .../<テーマ>/<言語>/... から言語を返す。"""
    for part in reversed(os.path.normpath(path).split(os.sep)):
        if part in LANGS:
            return part
    return "en-US"


def _widgets(path):
    """widgets.tsv の行 (テキストの列は _unescape したもの、数の列は int)。"""
    out = []
    for x in read_tsv(path):
        try:
            for k in ("x", "y", "w", "h"):
                x[k] = int(x[k])
        except (ValueError, KeyError):
            continue
        for k in ("text", "full_text", "tooltip", "a11y_name", "a11y_desc"):
            # 改行の位置を自分で決める QLabel (WrapLabel) は、テキストに行区切り文字 (U+2028) を入れる。比べるときは
            # 除く。
            x[k] = _unescape(x.get(k) or "").replace(LS, "")
        out.append(x)
    return out


_PATH_RE = re.compile(r"^[A-Za-z]:\\")


def location_rows(allw, ini):
    """表示されている場所の行 (テキストがドライブの名前で始まる QLabel) を上から並べ、対になるボタンの種類を添えて
    [(場所, 種類)] で返す。

    種類は ini の cordmux.dialog.<種類>.button の <種類> (open_folder、open_file_location、open_folder_location) で、
    対になるボタンがなければ None。対になるボタンは、QLabel の右にあって縦の範囲が QLabel と重なるボタン (ダイアログの
    形。テーマでボタンが QLabel より高く、縦の中央でそろう)、なければ QLabel の下端から 8 px までに上端があるボタン
    (カードの形。QLabel の直下)。どちらも近いほう。ボタンのテキストは ini から引く (言語によらない)。
    """
    kinds = {ini.get("cordmux.dialog.%s.button" % k): k for k in ("open_folder", "open_file_location",
                                                                   "open_folder_location")}
    vis = [x for x in allw if x["visible"] == "1"]
    buttons = [x for x in vis if x["class"].endswith("Button") and x["full_text"] in kinds]
    out = []
    for x in sorted((x for x in vis if not x["class"].endswith("Button") and _PATH_RE.match(x["full_text"])),
                    key=lambda x: (x["y"], x["x"])):
        right = [b for b in buttons if b["x"] >= x["x"] + x["w"] and b["y"] < x["y"] + x["h"] and b["y"] + b["h"] > x["y"]]
        below = [b for b in buttons if x["y"] + x["h"] - 2 <= b["y"] <= x["y"] + x["h"] + 8 and b["x"] < x["x"] + x["w"]]
        best = (min(right, key=lambda b: b["x"]) if right else
                min(below, key=lambda b: b["y"], default=None))
        out.append((x["full_text"], kinds[best["full_text"]] if best else None))
    return out


def _inside(a, b, slack=0):
    """ウィジェット a の矩形が b の中にあるか。"""
    return (a["x"] >= b["x"] - slack and a["y"] >= b["y"] - slack and a["x"] + a["w"] <= b["x"] + b["w"] + slack and
            a["y"] + a["h"] <= b["y"] + b["h"] + slack)


def _band_rows(band):
    """beside-scenes-edges.tsv の帯 ("<x>,<y> <幅>x<高さ>") の縦の範囲 (上端, 下端) を返す。"""
    m = re.match(r"^-?\d+,(-?\d+) \d+x(\d+)$", band)
    return (int(m.group(1)), int(m.group(1)) + int(m.group(2))) if m else (0, 0)


def is_name_placeholder(scope, field):
    """fields.tsv の行が、参加者の設定ウィンドウの [cordmuxでの名前] のプレースホルダーか (scope は出力先の相対パス)。"""
    return "face-participants" in scope.replace(os.sep, "/").split("/")[0:3] and field["what"] == "placeholder"


def _on_status_tab(rows):
    """画像が状態タブのものか (状態タブのスクロール領域 cordmuxStatusScroll が見えているか)。メッセージのカードは状態タブ
    だけにあり、参加者タブにはカードがない (1 行目の見出しだけ)。カードの中身を見るチェックは、状態タブの画像だけを数える。"""
    return any(x["object_name"] == "cordmuxStatusScroll" and x["visible"] == "1" for x in rows)


def _sentences(text, lang):
    """文の数 (ja は「。」、en は「. 」と最後の「.」で数える。ツールチップを 2 文以内にする規則のチェックに使う)。"""
    t = text.strip()
    if not t:
        return 0
    if lang == "ja-JP":
        return max(1, t.count("。") + (0 if t.endswith("。") else 1))
    return max(1, t.count(". ") + 1)


def _size_of(shot):
    """画像の名前の末尾の大きさ (例 notice-many-267x600 なら 267x600、-20em なら 20em)。"""
    return shot.rsplit("-", 1)[1] if "-" in shot else ""


def _scale_of(path):
    """出力先の名前の -x<百分率> の拡大率 (なければ 1)。"""
    for part in os.path.normpath(path).split(os.sep):
        for piece in part.split("-"):
            if piece.startswith("x") and piece[1:].isdigit():
                return int(piece[1:]) / 100.0
    return 1.0


def _lum(p):
    def ch(c):
        c = c / 255.0
        return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4
    return 0.2126 * ch(p[0]) + 0.7152 * ch(p[1]) + 0.0722 * ch(p[2])


def _ratio(a, b):
    la, lb = _lum(a), _lum(b)
    return (max(la, lb) + 0.05) / (min(la, lb) + 0.05)


def _mark_ratio(png, w, scale):
    """ウィジェット w の内側 (枠線の 3 px を除く) の背景の色と、背景との比がいちばん大きい色の、コントラスト比。
    数えられなければ None。"""
    try:
        from PIL import Image
        import collections
        im = Image.open(png).convert("RGB")
    except (ImportError, OSError):
        return None
    x0, y0 = int((w["x"] + 3) * scale), int((w["y"] + 3) * scale)
    x1, y1 = int((w["x"] + w["w"] - 3) * scale), int((w["y"] + w["h"] - 3) * scale)
    if x1 <= x0 or y1 <= y0 or x1 > im.width or y1 > im.height:
        return None
    counts = collections.Counter(im.crop((x0, y0, x1, y1)).getdata())
    bg = counts.most_common(1)[0][0]
    # 1 ピクセルだけの色 (枠線のにじみ) は、アイコンの色に数えない。
    return max((_ratio(c, bg) for c, n in counts.items() if n >= 2), default=1.0)


_PNG_CACHE = {}


def _ink_rows(png, w, card, scale):
    """ウィジェット w の矩形 (card の内側に切る) の中で、card の背景 (card の矩形でいちばん多い色) と違うピクセルがある
    行の上端と下端 (画像の上の論理 px)。なければ None。違うピクセルは、RGB のどれかが 24 を超えて離れたもの (にじみの
    薄いピクセルは数えない)。"""
    try:
        from PIL import Image
        import collections
    except ImportError:
        return None
    if png not in _PNG_CACHE:
        try:
            _PNG_CACHE.clear()
            _PNG_CACHE[png] = Image.open(png).convert("RGB")
        except OSError:
            return None
    im = _PNG_CACHE[png]
    cx0, cy0 = int(card["x"] * scale), int(card["y"] * scale)
    cx1, cy1 = int((card["x"] + card["w"]) * scale), int((card["y"] + card["h"]) * scale)
    key = (png, cx0, cy0, cx1, cy1)
    if key not in _PNG_CACHE:
        # 枠線 (カードの外周の線) を除くため、内側 3 px で背景の色を読む。
        crop = im.crop((cx0 + 3, cy0 + 3, max(cx0 + 4, min(im.width, cx1) - 3), max(cy0 + 4, min(im.height, cy1) - 3)))
        _PNG_CACHE[key] = collections.Counter(crop.getdata()).most_common(1)[0][0]
    bg = _PNG_CACHE[key]
    x0 = max(int(w["x"] * scale), cx0 + 2)
    x1 = min(int((w["x"] + w["w"]) * scale), cx1 - 2)
    y0 = max(int(w["y"] * scale), cy0 + 2)
    y1 = min(int((w["y"] + w["h"]) * scale), cy1 - 2)
    # 画像の外 (スクロール領域の下に隠れた所) は数えない。
    x0, y0 = max(x0, 0), max(y0, 0)
    x1, y1 = min(x1, im.width), min(y1, im.height)
    if x1 <= x0 or y1 <= y0:
        return None
    px = im.load()
    top = bottom = None
    for y in range(y0, y1):
        for x in range(x0, x1):
            p = px[x, y]
            if abs(p[0] - bg[0]) > 24 or abs(p[1] - bg[1]) > 24 or abs(p[2] - bg[2]) > 24:
                if top is None:
                    top = y
                bottom = y
                break
    if top is None:
        return None
    return (int(top / scale), int(bottom / scale))


def _ink_right(png, w, area, scale):
    """ウィジェット w の矩形の中で、area の背景 (area の内側でいちばん多い色) と違うピクセルがある列のいちばん右 (画像の
    上の論理 px)。なければ None。違うピクセルのしきい値は _ink_rows と同じ (RGB のどれかが 24 を超えて離れたもの)。"""
    try:
        from PIL import Image
        import collections
    except ImportError:
        return None
    if png not in _PNG_CACHE:
        try:
            _PNG_CACHE.clear()
            _PNG_CACHE[png] = Image.open(png).convert("RGB")
        except OSError:
            return None
    im = _PNG_CACHE[png]
    ax0, ay0 = int(area["x"] * scale), int(area["y"] * scale)
    ax1, ay1 = min(im.width, int((area["x"] + area["w"]) * scale)), min(im.height, int((area["y"] + area["h"]) * scale))
    if ax1 <= ax0 or ay1 <= ay0:
        return None
    bg = collections.Counter(im.crop((ax0, ay0, ax1, ay1)).getdata()).most_common(1)[0][0]
    x0, y0 = max(int(w["x"] * scale), 0), max(int(w["y"] * scale), 0)
    x1, y1 = min(int((w["x"] + w["w"]) * scale), im.width), min(int((w["y"] + w["h"]) * scale), im.height)
    px = im.load()
    for x in range(x1 - 1, x0 - 1, -1):
        for y in range(y0, y1):
            p = px[x, y]
            if abs(p[0] - bg[0]) > 24 or abs(p[1] - bg[1]) > 24 or abs(p[2] - bg[2]) > 24:
                return int(x / scale)
    return None


def _box_and_margin_colors(png, box, scale):
    """スクロール領域の左半分と、その左のダイアログの余白 (領域の縦の中央から 40 px) の、いちばん多い色の組。
    読めなければ None。"""
    try:
        from PIL import Image
        import collections
    except ImportError:
        return None
    try:
        im = Image.open(png).convert("RGB")
    except OSError:
        return None

    def common(x0, y0, x1, y1):
        crop = im.crop((int(x0 * scale), int(y0 * scale), int(x1 * scale), int(y1 * scale)))
        return collections.Counter(crop.getdata()).most_common(1)[0][0] if crop.width and crop.height else None

    mid = box["y"] + box["h"] // 2
    inside = common(box["x"], box["y"], box["x"] + box["w"] // 2, box["y"] + box["h"])
    margin = common(0, mid, box["x"] - 2, mid + 40)
    return (inside, margin) if inside and margin else None


def _fit_rows(wd_dir):
    """出力先の screen-fit.tsv の行 ({画像の名前: 行})。なければ空。"""
    path = os.path.join(wd_dir, "screen-fit.tsv")
    return {r["shot"]: r for r in read_tsv(path)} if os.path.exists(path) else {}


def r17_checks(set_dir, add, set_name):
    """ウィンドウの高さと、アンインストールと録画ファイルの作成の失敗のメッセージのチェック。

    (1) ウィンドウが画面の作業領域に収まるか (faces.cpp の screen_fit_row が書く screen-fit.tsv)。ウィンドウのすべての
        出力先でチェックする (既定の画面 1920x1040 の base と classic も、r17 の 4 つの画面も)。失敗は 3 つで、
        ウィンドウが作業領域より大きい、本文の外のボタンが作業領域の外かウィンドウの中で切れる、スクロールしても本文の
        行に届かない。
    (2) 空き容量が尽きて停止したカード (レベル 1) と、録画ファイルの作成の失敗のカード (レベル 2) が、同じドックに並び、作成の失敗の
        カードに次の操作と [作成を再開] がある (停止したカードは後で録画ファイルを作成する方法を伝えず、同じドックの録画ファイルの作成の失敗の
        カードに任せる)。2 件の見出しがスクロール領域の中で見えるかは参考 (折りたたんだメッセージのレベル 1 は、見出しだけを
        表示する)。
    (3) 製品で起きる、途中で止まったアンインストール (ダイアログとカードの両方)。段階 (1) で止まった形では、
        「削除できなかったもの」の次の操作が「もう一度アンインストールしてください。」で、手で削除する操作がない。
        段階 (3) で止まった形 (元に戻せなかった設定だけ) では、注意「この設定を元に戻すまで、次のフォルダーを削除
        しないでください。もう一度アンインストールするときに必要です。」の後に、プラグインのフォルダー、plugin_config の
        cordmux の順で 2 つの場所の行 (どちらも [フォルダーを開く]) があり、削除できなかったもののグループはない。
        ファイル 1 つの場所の行 (段階 (1) の settings.json、段階 (3) の state.json) は [ファイルの場所を開く]、
        フォルダーの行は [フォルダーを開く]。場所の行と対になるボタンは location_rows が決める。
    """
    for f in sorted(glob.glob(os.path.join(set_dir, "**", "screen-fit-selfcheck.tsv"), recursive=True)):
        r = read_tsv(f)
        add(set_name, "screen fit selfcheck: the count turns red without scrolling", os.path.relpath(os.path.dirname(f), set_dir),
            sum(1 for x in r if x["result"] != "skipped"), sum(1 for x in r if x["result"] == "FAIL"),
            "; ".join("%s %s h=%s out=%s unreach=%s" % (x["case"], x["result"], x["window_h"], x["buttons_out"],
                                                       x["rows_unreachable"]) for x in r)[:300])
    fits = sorted(glob.glob(os.path.join(set_dir, "**", "screen-fit.tsv"), recursive=True))
    for f in fits:
        scope = os.path.relpath(os.path.dirname(f), set_dir)
        r = read_tsv(f)
        over = [x for x in r if x["fits"] != "yes"]
        add(set_name, "screen fit: window larger than the work area", scope, len(r), len(over),
            "; ".join("%s %sx%s+%s on %sx%s" % (x["shot"], x["window_w"], x["window_h"], x["frame"], x["avail_w"],
                                                x["avail_h"]) for x in over)[:300])
        add(set_name, "screen fit: dialog buttons outside the work area", scope,
            sum(int(x["buttons"]) for x in r), sum(int(x["buttons_out"]) for x in r),
            "; ".join("%s %s" % (x["shot"], x["unreachable"]) for x in r if x["buttons_out"] != "0")[:300])
        add(set_name, "screen fit: body rows not reachable", scope,
            sum(int(x["rows"]) for x in r), sum(int(x["rows_unreachable"]) for x in r),
            "; ".join("%s %s" % (x["shot"], x["unreachable"]) for x in r if x["rows_unreachable"] != "0")[:300])
        scrolled = [x for x in r if x["scrolls"] == "yes"]
        # 本文をスクロールする確認・結果のダイアログで、本文の背景がダイアログの背景と同じか (スクロール領域に、背景の
        # 色と枠線を付けない)。スクロール領域 (cordmuxMessageScroll) の左半分でいちばん多い色と、その左のダイアログの
        # 余白 (領域の縦の中央の 40 px) でいちばん多い色を比べる。RGB のどれかが 4 を超えて違えば失敗。
        box_bad, box_seen = [], 0
        for x in scrolled:
            png = os.path.join(os.path.dirname(f), x["shot"] + ".png")
            wt = os.path.join(os.path.dirname(f), x["shot"] + ".widgets.tsv")
            if not (os.path.exists(png) and os.path.exists(wt)):
                continue
            ws = _widgets(wt)
            box = next((w for w in ws if w["object_name"] == "cordmuxMessageScroll"), None)
            if box is None or box["x"] < 8:
                continue
            got = _box_and_margin_colors(png, box, _scale_of(os.path.dirname(f)))
            if got is None:
                continue
            box_seen += 1
            inside, margin = got
            if any(abs(a - b) > 4 for a, b in zip(inside, margin)):
                box_bad.append("%s box #%02x%02x%02x / window #%02x%02x%02x" % ((x["shot"],) + inside + margin))
        if box_seen:
            add(set_name, "screen fit: scrolling body painted unlike the dialog", scope, box_seen, len(box_bad),
                "; ".join(box_bad)[:300])
        if scrolled:
            add(set_name, "screen fit: windows that scroll their body (record only)", scope, len(r), 0,
                "; ".join("%s %sx%s view %s/%s tall %s" % (x["shot"], x["window_w"], x["window_h"], x["view_h"],
                                                          x["content_h"], x["rows_taller_than_view"])
                          for x in scrolled)[:300])
    for wd_dir in sorted({os.path.dirname(p) for p in glob.glob(os.path.join(set_dir, "**", "face-dock", "*", "*",
                                                                                  M1_SCENE + "-*.widgets.tsv"),
                                                                     recursive=True)}):
        scope = os.path.relpath(wd_dir, set_dir)
        ini = ini_values(_lang_of(wd_dir))
        stop_head = ini.get("cordmux.dock.record_stopped_space.headline", "\0")
        stop_short = ini.get("cordmux.dock.record_stopped_space_short.headline", "\0")
        fail_head = ini.get("cordmux.dock.finalize_failed.headline", "\0")
        fail_next = ini.get("cordmux.dock.finalize_failed_next.text", "\0")
        resume = ini.get("cordmux.dock.resume_finalizing.button", "\0")
        wraps = {}
        if os.path.exists(os.path.join(wd_dir, "wraps.tsv")):
            for w in read_tsv(os.path.join(wd_dir, "wraps.tsv")):
                wraps.setdefault(w["shot"], []).append(_unescape(w["text"]).replace(LS, ""))
        seen, bad, notes = 0, [], []
        for wd in sorted(glob.glob(os.path.join(wd_dir, M1_SCENE + "-*.widgets.tsv"))):
            shot = os.path.basename(wd)[:-len(".widgets.tsv")]
            allw = _widgets(wd)
            # 2 件のカードは状態タブにあるので、状態タブの画像だけを見る。参加者タブの画像には、カードがない。
            if not _on_status_tab(allw):
                continue
            seen += 1
            allw = [x for x in allw if x["visible"] == "1"]
            keys = [p.split("=", 1)[1] for x in allw for p in x["props"].split(";") if p.startswith("noticeKey=")]
            texts = [x["full_text"] for x in allw] + [x["a11y_name"] for x in allw] + wraps.get(shot, [])
            why = []
            if not any(t in (stop_head, stop_short) for t in texts):
                why.append("no stopped headline")
            if not any(t == fail_head for t in texts):
                why.append("no finalize-failed headline")
            if not any(t == fail_next for t in texts):
                why.append("no finalize-failed next step")
            if not any(x["class"].endswith("Button") and x["full_text"] == resume for x in allw):
                why.append("no resume button")
            if why:
                bad.append("%s %s" % (shot, ",".join(why)))
            vis = allw
            view = next((x for x in vis if x["object_name"] == "cordmuxStatusScroll"), None)
            # 見える = スクロール領域の見える範囲の下端より上 (上端のメッセージのレベル 1 はスクロール領域の外の上にあり、
            # スクロール領域は先頭の位置で描画する)。
            shown = [h for h in (stop_head, stop_short, fail_head)
                     if any(x["full_text"] == h and (view is None or x["y"] + x["h"] <= view["y"] + view["h"])
                            for x in vis)]
            notes.append("%s keys=%s shown=%d" % (shot.rsplit("-", 1)[1], ",".join(keys) or "-", len(shown)))
        if seen:
            add(set_name, "stopped card and finalize-failed card shown together", scope, seen, len(bad),
                ("; ".join(bad) + " | " + "; ".join(notes))[:300])
    for wd_dir in sorted({os.path.dirname(p) for p in
                          glob.glob(os.path.join(set_dir, "**", "*uninstall-stopped-settings*.widgets.tsv"), recursive=True) +
                          glob.glob(os.path.join(set_dir, "**", "*uninstall-stopped-not-restored*.widgets.tsv"),
                                    recursive=True)}):
        scope = os.path.relpath(wd_dir, set_dir)
        ini = ini_values(_lang_of(wd_dir))
        again = ini.get("cordmux.dialog.not_deleted_uninstall_again_next.text", "\0")
        by_hand = ini.get("cordmux.dialog.not_deleted_next.text", "\0")
        wraps = {}
        if os.path.exists(os.path.join(wd_dir, "wraps.tsv")):
            for w in read_tsv(os.path.join(wd_dir, "wraps.tsv")):
                wraps.setdefault(w["shot"], []).append(_unescape(w["text"]).replace(LS, ""))
        seen, bad = 0, []
        for wd in sorted(glob.glob(os.path.join(wd_dir, "*uninstall-stopped-*.widgets.tsv"))):
            shot = os.path.basename(wd)[:-len(".widgets.tsv")]
            if shot.startswith("bottom-") or not ("-settings" in shot or "-not-restored" in shot):
                continue
            allw = _widgets(wd)
            # ドックの画像は、カードのある状態タブのものだけを見る (参加者タブの画像にはカードがない)。ダイアログの画像は
            # そのまま見る。
            if "face-dock" in wd_dir.replace(os.sep, "/") and not _on_status_tab(allw):
                continue
            seen += 1
            texts = [x["full_text"] for x in allw if x["visible"] == "1"] + wraps.get(shot, [])
            why = []
            if any(by_hand in t for t in texts):
                why.append("says delete by hand")
            if "-settings" in shot and not any(t == again or t.endswith(again) for t in texts):
                why.append("no uninstall-again step")
            rows = location_rows(allw, ini)
            if "-not-restored" in shot:
                if not any(t == ini.get("cordmux.dialog.dont_delete_folders.text", "\0") for t in texts):
                    why.append("no dont-delete-folders caution")
                if any(t == ini.get("cordmux.dialog.not_deleted.text", "\0") for t in texts):
                    why.append("has a not-deleted group")
                order = [p for p, _ in rows]
                plug = next((i for i, p in enumerate(order) if p.lower().endswith("\\plugins\\cordmux")), None)
                conf = next((i for i, p in enumerate(order) if p.lower().endswith("\\plugin_config\\cordmux")), None)
                if plug is None or conf is None:
                    why.append("caution rows %s" % ("/".join(p.rsplit("\\", 2)[-2] for p in order) or "none"))
                elif plug > conf:
                    why.append("plugin_config before plugins")
            for path, button in rows:
                want = "open_file_location" if path.lower().endswith(".json") else "open_folder"
                if button != want:
                    why.append("%s -> %s" % (path.rsplit("\\", 1)[-1], button or "no button"))
            if why:
                bad.append("%s %s" % (shot, ",".join(why)))
        if seen:
            add(set_name, "stopped uninstall as the product makes it (stage 1 / stage 3)", scope, seen, len(bad),
                "; ".join(bad)[:300])


def _guide_quotes():
    """手順書 (guide/ja.md と guide/en.md) が引用する切断のメッセージ (ja は「「...」のようなメッセージ」、en は
    'a notice that starts with "..."') を、言語ごとに返す。読めなければ空。"""
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "guide")
    out = {}
    for lang, name, pat in [("ja-JP", "ja.md", r"「(Vesktopとの接続が[^」]*)」のようなメッセージ"),
                            ("en-US", "en.md", r'notice that starts with\s+"(Disconnected from Vesktop[^"]*)"')]:
        try:
            with open(os.path.join(root, name), encoding="utf-8") as f:
                m = re.search(pat, f.read())
        except OSError:
            m = None
        if m:
            out[lang] = m.group(1)
    return out


# en の長いテキストのうち、ドックの 1 行の QLabel (見出し) に表示されるもの。省略されたか (QLabel のテキストが全文と
# 違う) を、参考として書く。
T1B_HEADLINE_KEYS = ["cordmux.dock.vencord_elsewhere.headline", "cordmux.dock.settings_unsaved.headline",
                     "cordmux.dock.participants_unsaved.headline", "cordmux.dock.record_not_reached.headline"]


def r18_checks(set_dir, add, set_name):
    """配信中に Vesktop が終了した場面と、en の長い見出しのチェック。

    (1) 配信中に Vesktop が終了した場面 (status-dropped-vesktop-stopped と -recording) では、メッセージのレベル 1 の切断の
        カード (noticeKey が ConnectedThenDropped、tier=1) の見出しが disconnected.headline (時刻 13:41) で、本文に
        disconnected_vesktop_not_running.text があり、理由ごとの本文 (disconnected.text) がない。-recording には録画の
        1 文 (disconnected_recording.text) もある。配信していない -idle は、1 行目が「Vesktopが起動していません」で、
        切断のカードがない。手順書の引用 (ja は見出しのテキストそのもの、en は見出しの先頭) が、描画した見出しと
        一致するかも見る (guide の 2 つの手順書から読む)。
    (2) en の見出し (T1B_HEADLINE_KEYS) が、描画した場所で省略されたか (参考。1 行の QLabel は省略するのが規則)。
    """
    quotes = _guide_quotes()
    for wd_dir in sorted({os.path.dirname(p) for p in glob.glob(os.path.join(set_dir, "**", "face-dock", "*", "*",
                                                                                  R07_SCENE + "*.widgets.tsv"),
                                                                     recursive=True)}):
        scope = os.path.relpath(wd_dir, set_dir)
        lang = _lang_of(wd_dir)
        ini = ini_values(lang)
        head = ini.get("cordmux.dock.disconnected.headline", "\0").replace("%1", "13:41")
        stopped = ini.get("cordmux.dock.disconnected_vesktop_not_running.text", "\0")
        reason = ini.get("cordmux.dock.disconnected.text", "\0")
        rec = ini.get("cordmux.dock.disconnected_recording.text", "\0")
        summary = ini.get("cordmux.dock.summary_vesktop_not_running.status", "\0")
        quote = quotes.get(lang)
        wraps = {}
        if os.path.exists(os.path.join(wd_dir, "wraps.tsv")):
            for w in read_tsv(os.path.join(wd_dir, "wraps.tsv")):
                wraps.setdefault(w["shot"], []).append(_unescape(w["text"]).replace(LS, ""))
        seen, bad, notes = 0, [], []
        for wd in sorted(glob.glob(os.path.join(wd_dir, R07_SCENE + "*.widgets.tsv"))):
            shot = os.path.basename(wd)[:-len(".widgets.tsv")]
            allw = _widgets(wd)
            # 切断のカードは状態タブにあるので、状態タブの画像だけを見る。参加者タブの画像には 1 行目の見出しだけがある。
            if not _on_status_tab(allw):
                continue
            seen += 1
            vis = [x for x in allw if x["visible"] == "1"]
            texts = [x["full_text"] for x in vis] + wraps.get(shot, [])
            # 状態タブのカードを探す (props の noticeKey と tier がカードに付く)。
            cards = [x for x in allw if "noticeKey=ConnectedThenDropped" in x["props"]]
            first = next((x for x in vis if x["object_name"] == "cordmuxFirstLine"), None)
            why = []
            if "-idle" in shot:
                if cards:
                    why.append("disconnect card outside streaming")
                if not any(t == summary for t in texts):
                    why.append("no not-running summary")
            else:
                if not cards:
                    why.append("no disconnect card")
                elif not any("tier=1" in c["props"] for c in cards):
                    why.append("card not tier 1")
                if not any(t == head for t in texts):
                    why.append("no headline at 13:41")
                if not any(stopped in t for t in texts):
                    why.append("no not-running text")
                if any(reason in t for t in texts):
                    why.append("reason text shown")
                if "-recording" in shot and not any(rec in t for t in texts):
                    why.append("no recording sentence")
                if quote is None:
                    why.append("guide quote not found")
                elif lang == "ja-JP" and quote != head:
                    why.append("guide quote %s != %s" % (quote, head))
                elif lang != "ja-JP" and not head.startswith(quote):
                    why.append("headline does not start with the guide quote")
            if why:
                bad.append("%s %s" % (shot, ",".join(why)))
            if first is not None:
                notes.append("%s first=%s" % (shot.replace(R07_SCENE, "") or shot, (first["a11y_name"] or first["full_text"])[:40]))
        if seen:
            add(set_name, "Vesktop stopped while streaming (disconnect card and guide quote)", scope, seen, len(bad),
                ("; ".join(bad) + " | " + "; ".join(notes))[:300])
    for wd_dir in sorted({os.path.dirname(p) for p in glob.glob(os.path.join(set_dir, "**", "face-dock", "*", "en-US",
                                                                                  "*.widgets.tsv"), recursive=True)}):
        if "t1b-" not in wd_dir:
            continue
        ini = ini_values("en-US")
        heads = {ini.get(k): k.split(".")[2] for k in T1B_HEADLINE_KEYS if ini.get(k)}
        # 1 行目の QLabel は、省略したテキストそのものを持つ (full_text も省略したテキスト) ので、省略記号 (U+2026) で
        # 終わる QLabel のテキストが見出しの先頭と一致すれば、その見出しを省略した QLabel と見なす。省略した位置が短縮形
        # (haven't など) の中なら、詳細にそのことを書く (「have」の後で省略すると否定が消え、逆の意味に読める)。
        seen, notes, contractions = 0, [], 0
        for wd in sorted(glob.glob(os.path.join(wd_dir, "*.widgets.tsv"))):
            shot = os.path.basename(wd)[:-len(".widgets.tsv")]
            for x in _widgets(wd):
                if x["visible"] != "1" or not x["class"].endswith("Label"):
                    continue
                t = x["text"]
                if x["full_text"] in heads and t == x["full_text"]:
                    seen += 1
                    continue
                if not t.endswith("\u2026"):
                    continue
                prefix = t[:-1].rstrip()
                for h, key in heads.items():
                    if prefix and h.startswith(prefix):
                        seen += 1
                        rest = h[len(prefix):].split(" ")[0]
                        word = (prefix.split(" ")[-1] if not h[len(prefix):].startswith(" ") else "") + rest
                        cut = bool(rest) and "'" in word
                        contractions += cut
                        notes.append("%s %s: %s%s" % (shot, key, t[-20:], " (cut inside a contraction)" if cut else ""))
                        break
        if seen:
            add(set_name, "en headlines elided where drawn (record only)", os.path.relpath(wd_dir, set_dir), seen, 0,
                ("%d elided, %d inside a contraction: " % (len(notes), contractions) + "; ".join(
                    n for n in notes if "contraction" in n) + " | " + "; ".join(notes))[:300] if notes else "")


# ---- 1 行目の短い形と省略 (短い形と、ja の打ち消しを省略で落とさないガード) ----
# metrics.tsv の first_line_forms の候補の区切り (faces.cpp の kFormsSeparator と同じ文字)。
FORMS_SEP = "\x1f"
ELLIPSIS = "\u2026"
# ja の打ち消しの述語の先頭を決める助詞と区切り (省略するときは、打ち消しの述語の直前の助詞の後まで戻す、という規則)。
# チェックは製品の実装 (src-obs/dock/ui/dock_ui_calc.h の ja_negated_predicate_spans) をコピーせず、規則から書いた
# (コピーすると、製品の誤りをチェックも同じように持つ)。「で」は入れない (「追加できません」の「で」は述語の中)。
_JA_PARTICLES_1 = set("をがはにへともやの")
_JA_PARTICLES_2 = ("から", "まで", "より")


def _is_kana_or_kanji(c):
    o = ord(c)
    return 0x3041 <= o <= 0x309F or 0x30A0 <= o <= 0x30FF or 0x3400 <= o <= 0x9FFF or 0xF900 <= o <= 0xFAFF or c == "\u3005"


def _is_hiragana(c):
    return 0x3041 <= ord(c) <= 0x309F


def _ja_negations(text):
    """text の中の打ち消しの語 (「ません」と、ひらがなの後の「ない」。「なかった」も含む) の (始め, 終わり) の一覧。
    「少ない」(漢字の後) は打ち消しではないので取らない。"""
    out = []
    for m in re.finditer("ません", text):
        out.append((m.start(), m.end()))
    for m in re.finditer("な(?:い|かった)", text):
        if m.start() > 0 and _is_hiragana(text[m.start() - 1]):
            out.append((m.start(), m.end()))
    return sorted(out)


def _ja_predicate_start(text, end):
    """text[:end] の末尾の述語の先頭 (後ろから見て、最初の助詞か、かなと漢字以外の文字の直後)。なければ 0。"""
    i = end
    while i > 0:
        c = text[i - 1]
        if text[max(0, i - 2):i] in _JA_PARTICLES_2:
            return i
        if c in _JA_PARTICLES_1:
            return i
        if c == "か" and (i >= len(text) or not _is_hiragana(text[i])):
            return i
        if not _is_kana_or_kanji(c):
            return i
        i -= 1
    return 0


def ja_negation_cut(full, prefix):
    """省略した ja の 1 行目 (prefix は、表示されているテキストから省略記号を除いたもの) が、full の打ち消しを落として
    肯定に読めるか。

    理由の文字列を返す (なければ空)。
      - "inside a negated predicate": 切れ目が打ち消しの述語の中 (述語の先頭より後ろ、打ち消しの語の終わりより前)。
        ただし、表示されているテキストが「ませ」で終わる (打ち消しが読める) ものは除く。
      - "affirmative clause before a dropped negation": 表示されているテキストが肯定の節 (「ます」「です」「ました」) で
        終わり、省いた後ろが逆接の「が」で始まって打ち消しを持つ (「Vesktopは起動しています...」で、「が、接続されて
        いません」が落ちる形)。括弧の補足 (「録画が止まりました (...足りません)」) は事実を補うだけで、逆に読めないので
        数えない。
    """
    cut = len(prefix)
    if not full.startswith(prefix) or cut >= len(full):
        return ""
    for start, end in _ja_negations(full):
        if end <= cut:
            continue
        head = _ja_predicate_start(full, start)
        if head < cut < end and not prefix.endswith("ませ"):
            return "inside a negated predicate"
    tail = prefix.rstrip(" \u3000\u3001")
    if tail.endswith(("ます", "です", "ました")) and full[len(tail):].lstrip().startswith("が") and \
            any(s >= cut for s, _ in _ja_negations(full)):
        return "affirmative clause before a dropped negation"
    return ""


def en_contraction_cut(full, prefix):
    """省略した en の 1 行目の切れ目が、短縮形 (haven't、can't など) の語の中か。"""
    cut = len(prefix)
    if not full.startswith(prefix) or cut == 0 or cut >= len(full):
        return False
    if full[cut - 1] == " " or full[cut] == " ":
        return False
    left = prefix.split(" ")[-1]
    right = full[cut:].split(" ")[0]
    word = left + right
    return "'" in word or "\u2019" in word


def number_cut(full, prefix):
    """省略した 1 行目の切れ目が、数の途中か、数と単位の間か (「12.」/「3 GB」、「13:」/「41」、「3」/「人」)。"""
    cut = len(prefix)
    if not full.startswith(prefix) or cut == 0 or cut >= len(full):
        return False
    strict, _ = number_breaks(full[max(0, cut - 3):cut] + " / " + full[cut:cut + 3])
    return bool(strict)


def _norm(text):
    """QLabel のテキストを比べるための形 (行区切り文字と空白を除く)。"""
    return re.sub(r"\s+", "", (text or "").replace(LS, "").replace("\u2029", ""))


def c14_body_start(widgets, notice_key, full):
    """1 行目に表示した見出しのメッセージの本文の先頭 (メッセージのレベル 1 の本文 cordmuxL1Body か、noticeKey の一致する
    カード。カードは、メッセージのレベル 2 のカードと、スクロール領域の先頭に移したメッセージのレベル 1 のカード) で、最初の
    テキストの QLabel が見出しの全文か。

    widgets は _widgets() の並び (ウィジェットのツリーを前から順にたどったもの。depth で子を決める)。notice_key が空
    なら (列のない表) どのカードでもよい。(全文で始まる所があるか, 見た所のテキストの一覧) を返す。
    """
    starts = []
    for i, x in enumerate(widgets):
        if x["visible"] != "1":
            continue
        is_body = x["object_name"] == "cordmuxL1Body"
        is_card = x["object_name"] == "cordmuxNoticeCard" and (
            not notice_key or ("noticeKey=%s;" % notice_key) in (x.get("props", "") + ";"))
        if not (is_body or is_card):
            continue
        depth = int(x["depth"])
        first = None
        for y in widgets[i + 1:]:
            if int(y["depth"]) <= depth:
                break
            if y["visible"] == "1" and y["class"].endswith("Label") and _norm(y["text"]):
                first = y
                break
        starts.append(first["text"] if first is not None else "")
    return any(_norm(s) == _norm(full) for s in starts), starts


def _first_line_kind(row, widgets):
    """1 行目がメッセージの見出しか。metrics.tsv に first_line_kind の列がない表では、1 行目のメッセージのレベルのアイコン
    (cordmuxFirstLine の直下のテキストのない QLabel) が表示されているかで読む (アイコンはメッセージの見出しのときだけ表示
    される。src-obs/dock/first_line.h)。"""
    if row.get("first_line_kind"):
        return row["first_line_kind"]
    for i, x in enumerate(widgets):
        if x["object_name"] == "cordmuxFirstLine":
            depth = int(x["depth"])
            for y in widgets[i + 1:]:
                if int(y["depth"]) <= depth:
                    break
                if int(y["depth"]) == depth + 1 and y["class"] == "QLabel" and not y["text"]:
                    return "notice" if y["visible"] == "1" else "summary"
            break
    return "-"


def short_form_complete(shown, forms):
    """1 行目のテキストが、候補のどれかを省略せずに表示しているか (全文か、短い形のすべて)。"""
    return shown in forms


def first_line_cut_reasons(lang, full, shown):
    """省略した 1 行目 (省略記号で終わる) の、切れ方の問題の理由の一覧を返す。"""
    prefix = shown[:-1].rstrip() if shown.endswith(ELLIPSIS) else shown
    out = []
    if lang == "ja-JP":
        why = ja_negation_cut(full, prefix)
        if why:
            out.append("ja " + why)
    else:
        if en_contraction_cut(full, prefix):
            out.append("en contraction")
    if number_cut(full, prefix):
        out.append("number")
    return out


# 1 行目の短い形のチェック (1) をするセット (フォントサイズ 10 の classic と base、同じ見た目で描画する r19)。
SHORT_FORM_SETS = ("base", "classic", "r19")

# チェック (1) の判定の行から除いて参考の行に書く、受け入れた例外の一覧。1 つの例外は、言語、メッセージのキー
# (metrics.tsv の first_line_notice_key)、短い形のテキスト、幅 (em。列挙した値だけ)、省略した形の先頭が、すべて一致する
# 画像だけに適用する。どれか 1 つでも違う画像 (短い形のテキストを変えた、省略の位置が変わった、別の幅で省略された) は、
# 判定の行で失敗のまま。適用をやめる条件 (reopen_prefix で始まる別のテキストが ini に追加された) を満たすと、例外を
# 適用せずに失敗に戻し、詳細にそのキーを書く。例外を列挙するのは、判定の行を「新しく崩れたものがなければ 0」と
# 読めるようにするためである (毎回失敗する行があると、本当に崩れたものが埋もれる)。判定の行から除く条件を規則 (例:
# tier 1 の en の 26 em を除く) にしないのは、同じ規則で、別のメッセージの崩れも隠れるためである。例外の画像を判定の行の対象
# (value) から抜かないのは、前回の出力と比べたときに対象が減ったように見えるためである (value は例外の画像も数え、
# bad からだけ抜く)。
FIRST_LINE_EXCEPTIONS = (
    {
        "name": "record_stopped_space en 26 em",
        "lang": "en-US",
        "notice_key": "RecordStoppedSpaceExhausted",
        # 例外のメッセージ自身の ini のキー (適用をやめる条件の照合から除く。全文の見出しも同じ先頭で始まるため)。
        "ini_keys": ("cordmux.dock.record_stopped_space.headline", "cordmux.dock.record_stopped_space_short.headline"),
        "short_form": "The cordmux recording stopped",
        # 受け入れた幅 (ドックの最初の幅の 26 em。classic と base。32 em 以上ではすべて入るので、受け入れない)。
        "widths_em": (26.0,),
        # 省略した形の先頭 (これより短く省略された画像は受け入れていない)。適用をやめる条件の先頭も同じ文字列。
        "shown_prefix": "The cordmux recording st",
        "reopen_prefix": "The cordmux recording st",
        "reason": "no wording fits 277 px that says 'cordmux recording', does not start with cordmux and puts the "
                  "problem first; the elided form is not misread, and the card body starts with the full headline",
        "reopen": "another string beginning \"The cordmux recording st\" is added (e.g. started)",
    },
)


def _exception_reopened(exc):
    """例外の適用をやめる条件。例外のメッセージ自身のキーを除く ini のテキストに、reopen_prefix で始まるものがあれば、その
    キーを返す (なければ None)。見出しのキーだけでなく、すべてのテキストを見る (1 行目には要約や処理中のテキストも表示
    される。余計に失敗になるのは、人が読めば済むほうである)。"""
    for k, v in sorted(ini_values(exc["lang"]).items()):
        if k not in exc["ini_keys"] and v.startswith(exc["reopen_prefix"]):
            return k
    return None


def first_line_exception(lang, notice_key, em, shown, forms):
    """判定の行で失敗になる画像に、列挙した例外を適用するか。(例外, 適用しない理由) を返す。言語、メッセージのキー、幅の
    どれかが合わなければ (None, None) (例外と関係のない失敗)。合っても、テキストか省略か適用をやめる条件で外れれば
    (例外, 理由) で、呼び出し元は失敗のまま詳細に理由を書く。適用するなら (例外, None)。"""
    for exc in FIRST_LINE_EXCEPTIONS:
        if lang != exc["lang"] or notice_key != exc["notice_key"]:
            continue
        if not any(abs(em - w) < 1e-6 for w in exc["widths_em"]):
            continue
        if exc["short_form"] not in forms:
            return exc, "short form is not %r" % exc["short_form"]
        prefix = shown[:-1].rstrip() if shown.endswith(ELLIPSIS) else None
        if prefix is None or not prefix.startswith(exc["shown_prefix"]) or not exc["short_form"].startswith(prefix):
            return exc, "shown %r is not the accepted cut" % shown
        key = _exception_reopened(exc)
        if key:
            return exc, "reopened by %s" % key
        return exc, None
    return None, None


def first_line_checks(set_dir, add, set_name):
    """1 行目のチェック (ドックのすべての画像、ja と en)。

    (1) 短い形のあるメッセージ (1 行目がメッセージの見出しで、候補が 2 つ以上) は、26 em 以上 (フォントサイズ 10 の classic、
        base、r19) で件数の QLabel がなければ、1 行目に候補のどれかを省略せずに表示する (短い形が全文と同じテキストの
        メッセージを除く。判定の行)。20 em 以上のすべてを、件数の QLabel (「(他1件)」) の有無と、20-26 em か 26 em 以上かで
        分けた参考の行にも書く。判定の行で失敗になる画像のうち、FIRST_LINE_EXCEPTIONS に列挙したものは、例外ごとの参考の
        行へ移す。
    (2) 省略した 1 行目 (省略記号で終わる) が、ja の打ち消しを落として肯定に読めない、en の短縮形の語の中で切れない、
        数の途中で切れない。
    (3) 1 行目が見出しの全文でない (短い形か、省略した形) とき、そのメッセージの本文の先頭が見出しの全文 (状態タブを開いた画像)。
    どれも画像ごとに数え、0 でない数の詳細に、画像の名前とテキストを書く。
    """
    for m in sorted(glob.glob(os.path.join(set_dir, "**", "face-dock", "*", "*", "metrics.tsv"), recursive=True)):
        scope = os.path.relpath(os.path.dirname(m), set_dir)
        lang = _lang_of(m)
        rows = read_tsv(m)
        short = {}
        cut_seen, cut_bad = {"ja": 0, "en": 0, "num": 0}, {"ja": [], "en": [], "num": []}
        c14_seen, c14_bad = 0, []
        for r in rows:
            shot = r["scene"]
            shown = _unescape(r["first_line_text"])
            full = _unescape(r["first_line_full"])
            forms = [f for f in _unescape(r.get("first_line_forms") or "").split(FORMS_SEP) if f] or [full]
            wd = os.path.join(os.path.dirname(m), shot + ".widgets.tsv")
            widgets = _widgets(wd) if os.path.exists(wd) else []
            kind = _first_line_kind(r, widgets)
            more = _unescape(r.get("first_line_more") or "")
            # (1)
            if set_name in SHORT_FORM_SETS and kind == "notice" and len(forms) >= 2:
                em = float(r["width_em"] or 0)
                if em >= 20.0 - 1e-6:
                    band = "20-26 em" if em < 26.0 - 1e-6 else "26 em or wider"
                    key = (band, "with count label" if more else "no count label")
                    seen, bad = short.get(key, (0, []))
                    if not short_form_complete(shown, forms):
                        bad = bad + ["%s %s" % (shot, shown)]
                    short[key] = (seen + 1, bad)
                    # 判定の行: 26 em 以上で件数の QLabel がなく、短い形が全文と違うメッセージ。
                    if band == "26 em or wider" and not more and len(set(forms)) > 1:
                        seen, bad = short.get("judged", (0, []))
                        if not short_form_complete(shown, forms):
                            # 列挙した例外 (FIRST_LINE_EXCEPTIONS) を適用する画像は、参考の行へ移す。適用しない理由が
                            # あれば、失敗のまま書く。
                            exc, why = first_line_exception(lang, _unescape(r.get("first_line_notice_key") or ""), em,
                                                            shown, forms)
                            if exc is not None and why is None:
                                eseen, ebad = short.get(("exception", exc["name"]), (0, []))
                                short[("exception", exc["name"])] = (eseen + 1, ebad + ["%s %s" % (shot, shown)])
                            elif exc is not None:
                                bad = bad + ["%s %s (exception '%s' does not apply: %s)" % (shot, shown, exc["name"], why)]
                            else:
                                bad = bad + ["%s %s" % (shot, shown)]
                        short["judged"] = (seen + 1, bad)
            # (2)
            if shown.endswith(ELLIPSIS):
                prefix = shown[:-1].rstrip()
                idx = r.get("first_line_shown_form", "")
                src = forms[int(idx)] if idx.lstrip("-").isdigit() and 0 <= int(idx) < len(forms) else None
                if src is None or not src.startswith(prefix):
                    src = next((f for f in reversed(forms) if f.startswith(prefix)), full)
                reasons = first_line_cut_reasons(lang, src, shown)
                if lang == "ja-JP":
                    cut_seen["ja"] += 1
                else:
                    cut_seen["en"] += 1
                cut_seen["num"] += 1
                for why in reasons:
                    k = "num" if why == "number" else ("ja" if why.startswith("ja") else "en")
                    cut_bad[k].append("%s %s (%s)" % (shot, shown, why))
            # (3) メッセージのカードは状態タブにあるので、状態タブを開いて描画した画像だけを見る。
            on_status_tab = any(x["object_name"] == "cordmuxStatusScroll" and x["visible"] == "1" for x in widgets)
            if kind == "notice" and shown != forms[0] and widgets and on_status_tab:
                c14_seen += 1
                ok, starts = c14_body_start(widgets, _unescape(r.get("first_line_notice_key") or ""), forms[0])
                if not ok:
                    c14_bad.append("%s %s / starts: %s" % (shot, shown, " | ".join(s[:30] for s in starts) or "none"))
        # 行は、対象が 0 枚でも書く (出力先ごとの行の有無が描画のたびに変わらないようにする。前回の出力と (セット、
        # チェック、出力先) で照合するとき、省略がなくなった出力先の行が消えると、「減った」と「なくなった」を
        # 見分けられないため)。
        # 短い形のチェック (1) は、SHORT_FORM_SETS のセットだけ (フォントサイズ 10)。
        # - 判定の行: 26 em (ドックの最初の幅) 以上で件数の QLabel がなく、短い形が全文と違うメッセージは、候補のどれかを
        #   省略せずに表示する。短い形は、20 em の 1 行目に入る文を目指し、入る自然な文がなければ 26 em に入る
        #   文にしたので、すべての短い形に共通に言える下限が 26 em である。
        # - 参考の行 (数えるが 0 でない数にしない。値は対象の枚数、詳細は省略せずに表示できなかった画像): 20-26 em の幅
        #   (短い形ごとに、20 em に入る設計か 26 em に入る設計かが違い、表からは決められない)、件数の QLabel と並ぶ形
        #   (ja は 6-7 文字、en は 8-10 文字しか入らないので、ja の打ち消しと en の短縮形を省略で切らないガードに
        #   任せる)、短い形を全文と同じテキストにしたメッセージ (語順で問題を先頭に言えるので、省略しても問題の語が先頭に
        #   残る)。
        # 20 em 以上のすべてで判定しないのは、満たせないと分かっている行が毎回失敗し、短い形のテキストが伸びて 26 em に
        # 入らなくなった本当の崩れが埋もれるためである。26 em 以上の判定から外れた短い形を規則で除かないのは、下限を
        # 緩めると崩れを見逃すためである (外れはテキストの側の問題として残す)。受け入れた例外は、
        # FIRST_LINE_EXCEPTIONS に列挙した画像だけを判定の行の bad から抜き、例外ごとの参考の行に書く (判定の行の
        # value は、例外の画像も数える)。
        if set_name in SHORT_FORM_SETS:
            seen, bad = short.get("judged", (0, []))
            add(set_name, "first line short form (differs from the headline) shown whole at 26 em or wider, "
                "no count label (font 10)", scope, seen, len(bad), "; ".join(bad)[:400])
            # 列挙した例外ごとの参考の行 (値は適用した画像の数。0 の出力先でも書き、行の有無を描画のたびに変えない)。
            for exc in FIRST_LINE_EXCEPTIONS:
                eseen, ebad = short.get(("exception", exc["name"]), (0, []))
                add(set_name, "first line short form not whole, accepted exception '%s' (record only)" % exc["name"],
                    scope, eseen, 0,
                    ("; ".join(ebad) + " | reason: %s | reopen when: %s" % (exc["reason"], exc["reopen"]))[:400]
                    if ebad else "")
            for band in ("20-26 em", "26 em or wider"):
                for label in ("no count label", "with count label"):
                    seen, bad = short.get((band, label), (0, []))
                    add(set_name, "first line short form shown whole at %s, %s (font 10, record only)" % (band, label),
                        scope, seen, 0, ("not whole %d: " % len(bad) + "; ".join(bad))[:400] if bad else "")
        if lang == "ja-JP":
            add(set_name, "elided first line drops a ja negation", scope, cut_seen["ja"], len(cut_bad["ja"]),
                "; ".join(cut_bad["ja"])[:400])
        else:
            add(set_name, "elided first line cuts inside an en contraction", scope, cut_seen["en"],
                len(cut_bad["en"]), "; ".join(cut_bad["en"])[:400])
        add(set_name, "elided first line cuts inside a number", scope, cut_seen["num"], len(cut_bad["num"]),
            "; ".join(cut_bad["num"])[:400])
        add(set_name, "card body starts with the full headline when the first line is not it", scope, c14_seen,
            len(c14_bad), "; ".join(c14_bad)[:400])


def short_form_coverage(root, add):
    """r19 の場面が、ほかのセットの 1 行目に表示された短い形をすべて含むか (短い形を追加したのに、r19 に場面を追加し
    忘れると失敗)。短い形はメッセージの候補の 2 つ目以降のテキストで、人数などの数は # に置き換えて比べる。"""
    r19 = os.path.join(root, "r19")
    if not os.path.isdir(r19):
        return

    def shorts(where):
        out = {}
        for m in glob.glob(os.path.join(where, "**", "face-dock", "*", "*", "metrics.tsv"), recursive=True):
            lang = _lang_of(m)
            for r in read_tsv(m):
                if r.get("first_line_kind") != "notice":
                    continue
                for f in _unescape(r.get("first_line_forms") or "").split(FORMS_SEP)[1:]:
                    if f:
                        out.setdefault((lang, re.sub(r"[0-9][0-9.,]*", "#", f)), r["scene"])
        return out

    have = shorts(r19)
    want = {}
    for other in sorted(glob.glob(os.path.join(root, "*"))):
        if os.path.isdir(other) and os.path.basename(other) != "r19":
            for k, v in shorts(other).items():
                want.setdefault(k, (os.path.basename(other), v))
    missing = sorted(k for k in want if k not in have)
    add("r19", "first line short forms drawn in r19 (coverage of the other sets)", "face-dock", len(want), len(missing),
        "; ".join("%s %s (e.g. %s/%s)" % (k[0], k[1], want[k][0], want[k][1]) for k in missing)[:400] +
        (" | r19 has %d" % len(have)))


def status_tab_selftest(expect):
    """状態タブに合わせたチェックが、失敗を検出し、見る画像を切り替えているか。一時フォルダーに合成した widgets.tsv と
    wraps.tsv を書き、チェックの行を数える。

    (1) 求めた大きさに届かない場面: 求めた幅が最小幅より狭く最小幅に止まった場面は数えず、ほかの理由で届かない場面は数える。
    (2) 行の状態: 267 ではアイコンがなければ失敗、floor では状態のテキストがなければ失敗。
    (3) 状態タブの切り替え: 1 人だけ映像が止まった場面 (glance-n1) の画像は、状態タブにあるときだけ数え、本文にスピーカー
        ミュートの文があれば失敗にする。参加者タブの画像は数えない。
    (4) 入力欄: 参加者の設定ウィンドウの名前のプレースホルダーだけを対象から外す。
    """
    def metric(scene, width_em, width, height, reached, min_width=256):
        return {"scene": scene, "width_em": width_em, "width": str(width), "height": str(height),
                "size_reached": "ok" if reached else "NOT-REACHED", "min_width": str(min_width)}
    ok_row = metric("a-20em", "20.00", 267, 600, True)
    expect("unreached: em narrower than the min width", unreached_sizes(
        [ok_row, metric("a-15em", "15.00", 256, 600, False)]), [])
    expect("unreached: px narrower than the min width", unreached_sizes(
        [ok_row, metric("a-230x165", "17.25", 256, 176, False)]), [])
    expect("unreached: width is neither the request nor the min", unreached_sizes(
        [ok_row, metric("a-230x165", "17.25", 300, 176, False)]), ["a-230x165"])
    expect("unreached: request is not narrower than the min", unreached_sizes(
        [ok_row, metric("a-20em", "20.00", 256, 600, False)]), ["a-20em"])
    expect("unreached: height below the request", unreached_sizes(
        [ok_row, metric("a-420x230", "31.50", 420, 200, False)]), ["a-420x230"])
    expect("unreached: unreadable request", unreached_sizes([ok_row, metric("a-min", "19.20", 256, 600, False)]),
           ["a-min"])
    expect("min width limit: font 10", min_width_limit_px("face-dock/yami-classic-f10-d-2/en-US"), 300)
    expect("min width limit: no font in the name", min_width_limit_px("face-dock/yami/en-US"), 300)
    expect("min width limit: font 8 is not tightened", min_width_limit_px("face-dock/yami-f8-d-2/en-US"), 300)
    expect("min width limit: font 14", min_width_limit_px("face-dock/yami-f14-d-2/en-US"), 420)
    expect("name placeholder: participants", is_name_placeholder("face-participants/yami/ja-JP",
                                                                  {"what": "placeholder"}), True)
    expect("name placeholder: typed text", is_name_placeholder("face-participants/yami/ja-JP", {"what": "text"}), False)
    expect("name placeholder: save folder", is_name_placeholder("face-settings-recording/yami/ja-JP",
                                                                 {"what": "placeholder"}), False)

    wcols = ["index", "depth", "object_name", "class", "obs_class", "visible", "enabled", "x", "y", "w", "h", "text",
             "full_text", "tooltip", "a11y_name", "a11y_desc", "fg", "buddy", "checked", "props"]

    def widget(name, cls="QLabel", visible="1", x=0, y=0, w=100, h=20, text="", full=""):
        r = dict.fromkeys(wcols, "")
        r.update(object_name=name, visible=visible, enabled="1", x=str(x), y=str(y), w=str(w), h=str(h),
                 text=text, full_text=full or text, depth="1")
        r["class"] = cls
        return r

    def write(path, rows, cols):
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\t".join(cols) + "\n")
            for i, r in enumerate(rows):
                f.write("\t".join(str(dict(r, index=i).get(c, "")) for c in cols) + "\n")

    def row_widgets(mark, text_visible, text):
        # 行 1 つ (状態のアイコンの QLabel と、状態のテキストの QLabel)。
        out = [widget("cordmux", "OBSDock", w=267, h=600), widget("cordmuxRow", "QWidget", y=100, w=267, h=40)]
        if mark:
            out.append(widget("cordmuxRowStatus", x=130, y=110, w=14, h=14))
        out.append(widget("cordmuxRowStatusText", visible="1" if text_visible else "0", x=150, y=108, w=100, h=20,
                          text=text, full="映像が届いていません"))
        return out

    ini = ini_values("ja-JP")
    check_next = ini.get("cordmux.dock.no_video_next.text", "\0")
    no_video_rec = ini.get("cordmux.dock.no_video_recording.text", "\0")
    tmp = tempfile.mkdtemp(prefix="render_sets_selftest_status_")
    d = os.path.join(tmp, "r5", "face-dock", "yami", "ja-JP")
    try:
        os.makedirs(d)
        # (2) 行の状態。267 でアイコンがない行 (失敗) と、アイコンだけの行 (成功)。floor でテキストが隠れた行 (失敗) と、
        # テキストがある行 (成功)。
        for shot, mark, text_visible, text in [("people-statuses-267x600", False, False, "映像..."),
                                               ("people-stages-267x600", True, False, "映像..."),
                                               ("people-statuses-floor", True, False, "映像..."),
                                               ("people-stages-floor", True, True, "映像途切れ")]:
            write(os.path.join(d, shot + ".widgets.tsv"), row_widgets(mark, text_visible, text), wcols)
        # (3) glance-n1: 状態タブの画像 (本文に Vesktop で確認する文がある = 失敗) と、同じ本文を持つ参加者タブの画像
        # (数えない)。
        status_tab = [widget("cordmux", "OBSDock", w=267, h=600), widget("cordmuxStatusScroll", "QScrollArea", w=267, h=400)]
        people_tab = [widget("cordmux", "OBSDock", w=267, h=600), widget("cordmuxScroll", "QScrollArea", w=267, h=400)]
        write(os.path.join(d, "glance-n1-430x600.widgets.tsv"), status_tab, wcols)
        write(os.path.join(d, "glance-n1-267x600.widgets.tsv"), people_tab, wcols)
        # 画面共有のメッセージ (notice-no-video-both は止まった人も録画の対象もいる場面で、止まった画像の文と録画の文が
        # 要る)。状態タブの画像では録画の文しかない (失敗)。参加者タブの画像は数えない。
        write(os.path.join(d, "notice-no-video-both-430x600.widgets.tsv"), status_tab, wcols)
        write(os.path.join(d, "notice-no-video-both-267x600.widgets.tsv"), people_tab, wcols)
        wcols_w = ["shot", "object_name", "class", "notice_key", "path", "text", "x", "y", "w", "h", "lines"]
        write(os.path.join(d, "wraps.tsv"),
              [{"shot": "glance-n1-430x600", "text": check_next, "y": "50", "lines": "1"},
               {"shot": "glance-n1-267x600", "text": check_next, "y": "50", "lines": "1"},
               {"shot": "notice-no-video-both-430x600", "text": no_video_rec, "y": "60", "lines": "1"},
               {"shot": "notice-no-video-both-267x600", "text": no_video_rec, "y": "60", "lines": "1"}], wcols_w)
        # (4) 入力欄。参加者の設定ウィンドウは、プレースホルダーだけ外す。入れた名前は対象のまま。
        fd = os.path.join(tmp, "r5", "face-participants", "yami", "ja-JP")
        os.makedirs(fd)
        fcols = ["shot", "object_name", "what", "text", "text_px", "room_px", "fits", "tooltip_has_full", "elided"]
        write(os.path.join(fd, "fields.tsv"),
              [{"shot": "p", "what": "placeholder", "text": "long", "text_px": "291", "room_px": "119", "fits": "no",
                "tooltip_has_full": "no", "elided": "no"},
               {"shot": "p", "what": "text", "text": "long", "text_px": "291", "room_px": "119", "fits": "no",
                "tooltip_has_full": "no", "elided": "no"}], fcols)
        got = {}

        def collect(set_name, check, scope, value, bad, note=""):
            got.setdefault(check, []).append((int(value), int(bad)))
        r5_checks(os.path.join(tmp, "r5"), collect, "r5")
        fields_checks(os.path.join(tmp, "r5"), collect, "r5")
        expect("r5 icon at 267: judged rows", got.get("row state icon missing at 267"), [(4, 1)])
        expect("r5 text at floor: judged rows", got.get("row state text missing at the floor width"), [(2, 1)])
        expect("r5 glance-n1: only the status tab image is judged", got.get("glance-n1 has a no-video next step"),
               [(1, 1)])
        # 画面共有のメッセージは、状態タブの画像 1 枚だけを数え (参加者タブの同じ本文の画像は数えない)、止まった画像の文が
        # ないので失敗にする。
        for name in ("share notice text", "share notice consequence not chosen per person"):
            expect("r5 %s: status tab image judged, people tab image not" % name, got.get(name), [(1, 1)])
        # 状態タブの画像がない場面 (録画ファイルの作成の失敗) は、行を出さない (参加者タブの画像では見るものがない)。
        name = "finalize failed notice lacks headline, location or resume"
        expect("r5 %s: no row without a status tab image" % name, name in got, False)
        expect("fields: placeholder excluded, typed text judged", got.get("line edit full text not visible"), [(1, 1)])
    finally:
        for root, dirs, files in os.walk(tmp, topdown=False):
            for name in files:
                os.remove(os.path.join(root, name))
            for name in dirs:
                os.rmdir(os.path.join(root, name))
        os.rmdir(tmp)


def selftest():
    """1 行目のチェックが失敗を検出できるか (どのチェックも、失敗するテキストと失敗しないテキストの両方で試す)。すべてが
    期待どおりなら 0 を返す。"""
    fails = []

    def expect(name, got, want):
        if got != want:
            fails.append("%s: got %r want %r" % (name, got, want))

    E = ELLIPSIS
    # (2) ja の打ち消し: 打ち消しの述語の中で切れた形は失敗、助詞の後まで戻した形は成功。
    expect("ja add failed", first_line_cut_reasons("ja-JP", "Vesktopにcordmuxを追加できませんでした",
                                                       "Vesktopにcordmuxを追加できま" + E), ["ja inside a negated predicate"])
    expect("ja uninstall", first_line_cut_reasons("ja-JP", "アンインストールを完了できませんでした",
                                                      "アンインストールを完了でき" + E), ["ja inside a negated predicate"])
    expect("ja attributive", first_line_cut_reasons("ja-JP", "画面共有の映像が届いていない人がいます (1人)",
                                                        "画面共有の映像が届い" + E), ["ja inside a negated predicate"])
    expect("ja positive clause", first_line_cut_reasons("ja-JP", "Vesktopは起動していますが、接続されていません",
                                                            "Vesktopは起動しています" + E),
           ["ja affirmative clause before a dropped negation"])
    expect("ja parenthesis after affirmative", first_line_cut_reasons(
        "ja-JP", "cordmuxの録画が止まりました (保存先の空き容量が足りません)", "cordmuxの録画が止まりました" + E), [])
    expect("ja after particle", first_line_cut_reasons("ja-JP", "Vesktopにcordmuxを追加できませんでした",
                                                       "Vesktopにcordmuxを" + E), [])
    expect("ja before particle", first_line_cut_reasons("ja-JP", "メインアカウントがいません", "メインアカウント" + E),
           [])
    expect("ja inside a noun", first_line_cut_reasons("ja-JP", "参加者の設定を保存できていません", "参加者の設" + E), [])
    expect("ja mase kept", first_line_cut_reasons("ja-JP", "空いている枠がありません。", "空いている枠がありませ" + E), [])
    expect("ja deshita only", first_line_cut_reasons("ja-JP", "録画を作成できませんでした", "録画を作成できません" + E), [])
    expect("ja sukunai", first_line_cut_reasons("ja-JP", "空き容量が少ないです", "空き容量が少" + E), [])
    # (2) en の短縮形と数。
    expect("en contraction", first_line_cut_reasons("en-US", "The participant settings haven't been saved",
                                                    "The participant settings haven" + E), ["en contraction"])
    expect("en contraction apostrophe", first_line_cut_reasons("en-US", "Vesktop isn't set up", "Vesktop isn'" + E),
           ["en contraction"])
    expect("en word before", first_line_cut_reasons("en-US", "The participant settings haven't been saved",
                                                    "The participant settings" + E), [])
    expect("number inside", first_line_cut_reasons("ja-JP", "空き容量が約12.3 GB足りません", "空き容量が約12." + E),
           ["number"])
    expect("number time", first_line_cut_reasons("en-US", "Disconnected from Vesktop at 13:41", "Disconnected from Vesktop at 13:" + E),
           ["number"])
    expect("number unit", first_line_cut_reasons("ja-JP", "3人の映像が届いていません", "3" + E), ["number"])
    expect("number whole", first_line_cut_reasons("ja-JP", "空き容量が約12.3 GB足りません", "空き容量が約" + E), [])
    # (1) 短い形のすべてか。
    expect("short whole", short_form_complete("Vesktopと接続されていません",
                                              ["Vesktopは起動していますが、接続されていません", "Vesktopと接続されていません"]), True)
    expect("short elided", short_form_complete("Vesktopと" + E,
                                               ["Vesktopは起動していますが、接続されていません", "Vesktopと接続されていません"]), False)

    # (3) 本文の先頭が全文のウィジェットの並びと、全文がない (先頭が本文のテキストの) 並び。
    def w(i, depth, name, cls, visible, text, props=""):
        return {"index": str(i), "depth": str(depth), "object_name": name, "class": cls, "visible": visible,
                "text": text, "full_text": text, "props": props}
    full = "Vesktopとの接続が13:41に切れました"
    good = [w(0, 3, "cordmuxL1Body", "QWidget", "1", ""), w(1, 4, "", "QLabel", "1", "Vesktopとの接続が" + LS + "13:41に切れました"),
            w(2, 4, "", "QLabel", "1", "再接続するまで...")]
    bad = [w(0, 3, "cordmuxL1Body", "QWidget", "1", ""), w(1, 4, "", "QLabel", "1", "再接続するまで..."),
           w(2, 4, "", "QLabel", "1", full)]
    card_other = [w(0, 5, "cordmuxNoticeCard", "QFrame", "1", "", "noticeKey=Other;tier=2"),
                  w(1, 6, "", "QLabel", "1", full)]
    card_good = [w(0, 5, "cordmuxNoticeCard", "QFrame", "1", "", "noticeKey=ConnectedThenDropped;tier=1"),
                 w(1, 6, "", "QLabel", "1", ""), w(2, 6, "", "QLabel", "1", full)]
    expect("c14 good", c14_body_start(good, "ConnectedThenDropped", full)[0], True)
    expect("c14 headline not first", c14_body_start(bad, "ConnectedThenDropped", full)[0], False)
    expect("c14 other card", c14_body_start(card_other, "ConnectedThenDropped", full)[0], False)
    expect("c14 card", c14_body_start(card_good, "ConnectedThenDropped", full)[0], True)
    expect("c14 key prefix", c14_body_start(card_good, "Connected", full)[0], False)

    # 表を通したチェック (first_line_checks) が 0 でない行を出すか。一時フォルダーに、合成した metrics.tsv と
    # widgets.tsv を書く。
    tmp = tempfile.mkdtemp(prefix="render_sets_selftest_")
    try:
        d = os.path.join(tmp, "classic", "face-dock", "yami-classic-f10-d-2", "ja-JP")
        os.makedirs(d)
        cols = ["scene", "width_em", "first_line_text", "first_line_full", "first_line_more", "first_line_kind",
                "first_line_tier", "first_line_notice_key", "first_line_shown_form", "first_line_forms"]
        lines = ["\t".join(cols),
                 "\t".join(["s-20em", "20.00", "Vesktopと" + E, "Vesktopは起動していますが、接続されていません", "",
                            "notice", "2", "VesktopRunningNotLinked", "1",
                            "Vesktopは起動していますが、接続されていません" + FORMS_SEP + "Vesktopと接続されていません"]),
                 "\t".join(["t-26em", "26.00", "Vesktopにcordmuxを追加できま" + E, "Vesktopにcordmuxを追加できませんでした",
                            "(他1件)", "notice", "1", "VesktopAddFailed", "0", "Vesktopにcordmuxを追加できませんでした"]),
                 # 判定の行: 26 em で件数の QLabel がなく、短い形が全文と違うのに省略した形 (失敗 1)、同じ形ですべてを
                 # 表示した形 (成功)、短い形を全文と同じテキストにしたメッセージの省略 (判定の対象外)。widgets.tsv を
                 # 書かないので、本文の先頭のチェックには入らない。
                 "\t".join(["u-26em", "26.00", "Vesktopと" + E, "Vesktopは起動していますが、接続されていません", "",
                            "notice", "2", "VesktopRunningNotLinked", "1",
                            "Vesktopは起動していますが、接続されていません" + FORMS_SEP + "Vesktopと接続されていません"]),
                 "\t".join(["v-32em", "32.00", "Vesktopと接続されていません", "Vesktopは起動していますが、接続されていません", "",
                            "notice", "2", "VesktopRunningNotLinked", "1",
                            "Vesktopは起動していますが、接続されていません" + FORMS_SEP + "Vesktopと接続されていません"]),
                 "\t".join(["w-26em", "26.00", "録画ファイルの作成が終わっていないcordmuxの" + E, "録画ファイルの作成が終わっていないcordmuxの録画があります",
                            "", "notice", "1", "FinalizePending", "1",
                            "録画ファイルの作成が終わっていないcordmuxの録画があります" + FORMS_SEP + "録画ファイルの作成が終わっていないcordmuxの録画があります"])]
        with open(os.path.join(d, "metrics.tsv"), "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
        wcols = ["index", "depth", "object_name", "class", "obs_class", "visible", "enabled", "x", "y", "w", "h", "text",
                 "full_text", "tooltip", "a11y_name", "a11y_desc", "fg", "buddy", "checked", "props"]
        for shot in ("s-20em", "t-26em"):
            with open(os.path.join(d, shot + ".widgets.tsv"), "w", encoding="utf-8", newline="\n") as f:
                f.write("\t".join(wcols) + "\n")
                f.write("\t".join(["0", "3", "cordmuxStatusScroll", "QScrollArea", "", "1", "1", "0", "0", "9", "9", "", "",
                                   "", "", "", "", "", "", ""]) + "\n")
                f.write("\t".join(["1", "5", "cordmuxNoticeCard", "QFrame", "", "1", "1", "0", "0", "9", "9", "", "", "",
                                   "", "", "", "", "", "noticeKey=Other;tier=2"]) + "\n")
                f.write("\t".join(["2", "6", "", "QLabel", "", "1", "1", "0", "0", "9", "9", "本文", "本文", "", "", "",
                                   "", "", "", ""]) + "\n")
        got = []
        first_line_checks(os.path.join(tmp, "classic"), lambda s, c, sc, v, b, n="": got.append((c, int(b), v, n)),
                          "classic")
        by, val, note = {}, {}, {}
        for c, b, v, n in got:
            by[c] = by.get(c, 0) + b
            val[c] = v
            note[c] = n
        judged = ("first line short form (differs from the headline) shown whole at 26 em or wider, "
                  "no count label (font 10)")
        expect("table short 26 em judged red", (by.get(judged), val.get(judged)), (1, 2))
        expect("table short 26 em judged names the shot", "u-26em" in note.get(judged, ""), True)
        rec20 = "first line short form shown whole at 20-26 em, no count label (font 10, record only)"
        expect("table short 20 em record only", (by.get(rec20), val.get(rec20), note.get(rec20, "")[:13]),
               (0, 1, "not whole 1: "))
        rec26 = "first line short form shown whole at 26 em or wider, no count label (font 10, record only)"
        expect("table short 26 em record counts the same form", (by.get(rec26), val.get(rec26)), (0, 3))
        expect("table ja negation red", by.get("elided first line drops a ja negation"), 1)
        expect("table c14 red", by.get("card body starts with the full headline when the first line is not it"), 2)
        # 網羅 (short_form_coverage): classic の表の短い形 (「Vesktopと接続されていません」) が r19 の表になければ失敗、
        # あれば成功。classic の表は、判定の行のために追加した 3 行を除いた形に戻す (追加した行の短い形が、網羅の
        # チェックに混ざらないように)。
        with open(os.path.join(d, "metrics.tsv"), "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines[:3]) + "\n")
        r19d = os.path.join(tmp, "r19", "short-classic", "face-dock", "yami-classic-f10-d-2", "ja-JP")
        os.makedirs(r19d)
        for rows_r19, want in ((lines[:1] + lines[2:3], 1), (lines[:2], 0)):
            with open(os.path.join(r19d, "metrics.tsv"), "w", encoding="utf-8", newline="\n") as f:
                f.write("\n".join(rows_r19) + "\n")
            got = []
            short_form_coverage(tmp, lambda s, c, sc, v, b, n="": got.append(int(b)))
            expect("coverage %s" % ("red" if want else "green"), got, [want])
    finally:
        for p in glob.glob(os.path.join(tmp, "r19", "short-classic", "face-dock", "yami-classic-f10-d-2", "ja-JP",
                                        "*")):
            os.remove(p)
        for p in (os.path.join(tmp, "r19", "short-classic", "face-dock", "yami-classic-f10-d-2", "ja-JP"),
                  os.path.join(tmp, "r19", "short-classic", "face-dock", "yami-classic-f10-d-2"),
                  os.path.join(tmp, "r19", "short-classic", "face-dock"), os.path.join(tmp, "r19", "short-classic"),
                  os.path.join(tmp, "r19")):
            if os.path.isdir(p):
                os.rmdir(p)
        for p in glob.glob(os.path.join(tmp, "classic", "face-dock", "yami-classic-f10-d-2", "ja-JP", "*")):
            os.remove(p)
        for p in (os.path.join(tmp, "classic", "face-dock", "yami-classic-f10-d-2", "ja-JP"),
                  os.path.join(tmp, "classic", "face-dock", "yami-classic-f10-d-2"),
                  os.path.join(tmp, "classic", "face-dock"),
                  os.path.join(tmp, "classic"), tmp):
            os.rmdir(p)

    # 列挙した例外 (FIRST_LINE_EXCEPTIONS): 例外を適用する画像は判定の行で失敗にならずに参考の行に出て、列挙の外の失敗、
    # 幅の違い、短い形のテキストの違い、適用をやめる条件では失敗のまま。ini は合成したテキストに替える (実際の ini の
    # テキストに、テストを依存させない)。
    exc = FIRST_LINE_EXCEPTIONS[0]
    ex_full = "The cordmux recording stopped (not enough space in the save folder)"
    ex_forms = ex_full + FORMS_SEP + exc["short_form"]
    ini_en = {exc["ini_keys"][0]: ex_full, exc["ini_keys"][1]: exc["short_form"],
              "cordmux.dock.record_gap.headline": "The cordmux recording was interrupted"}
    saved_ini = _INI.get("en-US")
    tmp = tempfile.mkdtemp(prefix="render_sets_selftest_exc_")
    d = os.path.join(tmp, "base", "face-dock", "yami", "en-US")
    try:
        os.makedirs(d)
        cols = ["scene", "width_em", "first_line_text", "first_line_full", "first_line_more", "first_line_kind",
                "first_line_tier", "first_line_notice_key", "first_line_shown_form", "first_line_forms"]
        listed = "\t".join(["x-26em", "26.00", "The cordmux recording st" + E, ex_full, "", "notice", "1",
                            exc["notice_key"], "1", ex_forms])
        others = [
            # 列挙の外のメッセージの省略 (失敗のまま)。
            "\t".join(["y-26em", "26.00", "Not connected to Ves" + E, "Vesktop is running but isn't connected", "",
                       "notice", "2", "VesktopRunningNotLinked", "1",
                       "Vesktop is running but isn't connected" + FORMS_SEP + "Not connected to Vesktop"]),
            # 列挙したメッセージでも、列挙していない幅 (失敗のまま)。
            "\t".join(["z-32em", "32.00", "The cordmux recording st" + E, ex_full, "", "notice", "1",
                       exc["notice_key"], "1", ex_forms]),
            # 列挙したメッセージでも、短い形のテキストが違う (失敗のまま)。
            "\t".join(["q-26em", "26.00", "The cordmux recording ha" + E, ex_full, "", "notice", "1",
                       exc["notice_key"], "1", ex_full + FORMS_SEP + "The cordmux recording halted"]),
            # 列挙したメッセージでも、受け入れた形より短く省略した (失敗のまま)。
            "\t".join(["r-26em", "26.00", "The cordmux recor" + E, ex_full, "", "notice", "1",
                       exc["notice_key"], "1", ex_forms])]

        def run_exc(rows):
            with open(os.path.join(d, "metrics.tsv"), "w", encoding="utf-8", newline="\n") as f:
                f.write("\n".join(["\t".join(cols)] + rows) + "\n")
            got = {}
            first_line_checks(os.path.join(tmp, "base"),
                              lambda s, c, sc, v, b, n="": got.__setitem__(c, (int(b), v, n)), "base")
            return got

        judged = ("first line short form (differs from the headline) shown whole at 26 em or wider, "
                  "no count label (font 10)")
        rec_exc = "first line short form not whole, accepted exception '%s' (record only)" % exc["name"]
        _INI["en-US"] = ini_en
        got = run_exc([listed])
        expect("exception listed: judged green", got.get(judged, (None,))[:2], (0, 1))
        expect("exception listed: recorded", got.get(rec_exc, (None,))[:2], (0, 1))
        expect("exception listed: record names the shot and the reopen condition",
               ("x-26em" in got.get(rec_exc, (0, 0, ""))[2], "reopen when" in got.get(rec_exc, (0, 0, ""))[2]),
               (True, True))
        got = run_exc([listed] + others)
        expect("exception others: judged red 4 of 5", got.get(judged, (None,))[:2], (4, 5))
        note = got.get(judged, (0, 0, ""))[2]
        expect("exception others: judged names the red shots",
               [s in note for s in ("y-26em", "z-32em", "q-26em", "r-26em", "x-26em")], [True, True, True, True, False])
        expect("exception others: listed still recorded", got.get(rec_exc, (None,))[:2], (0, 1))
        # 適用をやめる条件: 同じ先頭で始まる別のテキストを ini に追加すると、列挙した画像も失敗に戻り、詳細にそのキーが
        # 出る。
        _INI["en-US"] = dict(ini_en, **{"cordmux.dock.record_started.headline": "The cordmux recording started"})
        got = run_exc([listed])
        expect("exception reopened: judged red", got.get(judged, (None,))[:2], (1, 1))
        expect("exception reopened: names the key", "record_started" in got.get(judged, (0, 0, ""))[2], True)
        expect("exception reopened: not recorded", got.get(rec_exc, (None,))[:2], (0, 0))
    finally:
        if saved_ini is None:
            _INI.pop("en-US", None)
        else:
            _INI["en-US"] = saved_ini
        for p in glob.glob(os.path.join(tmp, "base", "face-dock", "yami", "en-US", "*")):
            os.remove(p)
        for p in (os.path.join(tmp, "base", "face-dock", "yami", "en-US"), os.path.join(tmp, "base", "face-dock", "yami"),
                  os.path.join(tmp, "base", "face-dock"), os.path.join(tmp, "base"), tmp):
            if os.path.isdir(p):
                os.rmdir(p)
    status_tab_selftest(expect)
    for f in fails:
        print("SELFTEST FAIL " + f)
    print("selftest: %d checks failed" % len(fails))
    return 1 if fails else 0


def r5_checks(set_dir, add, set_name):
    """ドックの行、メッセージ、ボタンのチェック (保留の人の行、行の状態のテキスト、状態のツールチップ、メッセージのレベル 1 の
    折りたたみ、画面共有のメッセージ、下のバーなど)。

    どのセットでも、場面の画像があればチェックする (r5 は場面を絞って描画し、base と classic はすべての場面を描画する)。
    """
    for wd_dir in sorted({os.path.dirname(p) for p in glob.glob(os.path.join(set_dir, "**", "face-dock", "*", "*",
                                                                                  "*.widgets.tsv"), recursive=True)}):
        scope = os.path.relpath(wd_dir, set_dir)
        lang = _lang_of(wd_dir)
        ini = ini_values(lang)
        metrics = {x["scene"]: x for x in read_tsv(os.path.join(wd_dir, "metrics.tsv"))} \
            if os.path.exists(os.path.join(wd_dir, "metrics.tsv")) else {}
        # 求めた大きさに届いた行から、em の px を求める (届かなかった画像の幅は、求めた幅ではなく最小幅なので、em の
        # 計算に使えない)。
        em_px_dir = next((int(x["width"]) / float(x["width_em"]) for x in metrics.values()
                          if x["size_reached"] == "ok" and float(x["width_em"] or 0) > 0), 0)
        wraps = {}
        if os.path.exists(os.path.join(wd_dir, "wraps.tsv")):
            for r in read_tsv(os.path.join(wd_dir, "wraps.tsv")):
                wraps.setdefault(r["shot"], []).append(r)
        tips = {ini.get("cordmux.dock.%s.status" % k, "\0"): ini.get("cordmux.dock.%s.tip" % k, "\0")
                for k in ("row_waiting", "row_not_receiving", "row_not_sharing")}
        rec_start = ini.get("cordmux.dock.record_start.button", "\0")
        rec_stop = ini.get("cordmux.dock.record_stop.button", "\0")
        rec_tip = ini.get("cordmux.dock.record.a11y", "\0")
        no_video = ini.get("cordmux.dock.no_video.text", "\0")
        no_video_rec = ini.get("cordmux.dock.no_video_recording.text", "\0")
        no_video_wait = ini.get("cordmux.dock.no_video_waiting.text", "\0")
        d53_seen, d53_bad = 0, []
        # 載せていない人の行の画質のテキストと、最初のフレームの前の解像度のテキスト。
        reduced_res_full = ini.get("cordmux.dock.row_resolution_reduced.status", "\0")
        d51q_seen, d51q_bad, d51r_seen, d51r_bad = 0, [], 0, []
        # 行の状態のテキストの先頭。打ち消しの 4 つの全文から短い形への辞書 (en は全文と同じテキスト)、画質の 4 つ
        # (先頭に「下げ」/ "Lower")。
        short_of = {}
        for k in ("row_not_receiving", "row_not_sharing", "row_cant_show", "row_self_video_missing"):
            full = ini.get("cordmux.dock.%s.status" % k)
            short = ini.get("cordmux.dock.%s_short.status" % k)
            if full and short:
                short_of[full] = short
        quality_full = {ini.get("cordmux.dock.%s.status" % k, "\0") for k in
                        ("row_fps_reduced", "row_resolution_reduced", "row_both_reduced", "row_quality_reduced")}
        quality_head = "下げ" if lang == "ja-JP" else "Lower"
        cant_show_full = ini.get("cordmux.dock.row_cant_show.status", "\0")
        # 1 行目の「表示中」は、エンコードの段階が unavailable の人を数えない。ini の 1 行目の形から数を読む。
        summary_forms = []
        for k in ("summary_call.status", "summary_call.status_one", "summary_none_displayed.status",
                  "summary_none_displayed.status_one"):
            t = ini.get("cordmux.dock." + k)
            if t:
                pat = re.escape(t).replace(re.escape("%1"), r"(\d+)").replace(re.escape("%2"), r"(\d+)")
                summary_forms.append((k, re.compile("^" + pat + "$")))
        d40_seen, d40_bad, d40_notes = 0, [], set()
        # 画面共有のメッセージの次の操作を、映像が止まった人のエンコードの段階で選ぶ。
        check_next = ini.get("cordmux.dock.no_video_next.text", "\0")
        cant_next = ini.get("cordmux.dock.no_video_cant_show_next.text", "\0")
        r08_seen, r08_bad = 0, []
        # メッセージのレベル 1 の録画ファイルの作成の失敗: 見出し、場所の行 (録画のセッションのフォルダー)、[作成を再開]。
        failed_head = ini.get("cordmux.dock.finalize_failed.headline", "\0")
        failed_next = ini.get("cordmux.dock.finalize_failed_next.text", "\0")
        resume_button = ini.get("cordmux.dock.resume_finalizing.button", "\0")
        pending_head = ini.get("cordmux.dock.finalize_pending.headline", "\0")
        r01_seen, r01_bad, r01_notes = 0, [], set()
        # 録画の対象だけの人 (枠に載っていない) の行の状態のアイコンとテキスト。ツールチップは枠に触れない形。
        rec_tips = {ini.get("cordmux.dock.%s.status" % k, "\0"): ini.get("cordmux.dock.%s_recording.tip" % k, "\0")
                    for k in ("row_waiting", "row_not_receiving")}
        r09_seen, r09_bad, r09_notes = 0, [], set()
        d44_seen, d44_bad, d44_rows_seen, d44_rows_bad = 0, [], 0, []
        c2_seen, c2_noicon, c2_clip, c2_name, c2_short, c2_heads, c2_text230 = 0, [], [], [], [], set(), []
        c2_notext_floor, c2_floor_seen = [], 0
        d34_seen, d34_neg, d34_quality, d34_short, d34_floor, d34_notes = 0, [], [], [], [], []
        d34_floor_seen, d34_short_seen, d340_seen, d340_bad = 0, 0, 0, []
        c3_seen, c3_bad = 0, []
        c3_text_seen, c3_text_elided, c3_text_bad = 0, 0, []
        c5_text_bad, c5_text_seen, c5_lines = [], 0, []
        c6_seen, c6_bad = 0, []
        stop_seen, stop_bad, stop_locs, stop_gaps = [], [], [], []
        for wd in sorted(glob.glob(os.path.join(wd_dir, "*.widgets.tsv"))):
            shot = os.path.basename(wd)[:-len(".widgets.tsv")]
            size = _size_of(shot)
            rows = _widgets(wd)
            if not rows:
                continue
            dock = rows[0]
            vis = [x for x in rows if x["visible"] == "1"]
            people = [x for x in vis if x["object_name"] == "cordmuxRow"]
            status_tab = _on_status_tab(rows)

            def parts_of(row, name):
                return [x for x in vis if x["object_name"] == name and _inside(x, row, 1)]

            # people-stages は 5 人で、枠に映像が表示されているのは、段階を下げた 3 人 (half、down、soft。
            # fixtures.cpp の people-stages)。ren は段階が unavailable、kenji は画面共有していない。1 行目の全文
            # (cordmuxFirstLine のアクセシブル名。狭い幅で後ろを省略しても全文を持つ) の「表示中」が 3 で、場面に映像を
            # 表示できない ren の行があること (なければチェックが空のまま通る)。
            if shot.startswith("people-stages"):
                d40_seen += 1
                head = next((x["a11y_name"] for x in rows if x["object_name"] == "cordmuxFirstLine"), None)
                got = None
                for k, rx in summary_forms:
                    m = rx.match(head or "")
                    if m:
                        got = (int(m.group(1)), int(m.group(2)) if "none" not in k else 0)
                        break
                if got is None:
                    d40_bad.append("%s first line not a call summary: %s" % (shot, head))
                elif got != (5, 3):
                    d40_bad.append("%s in call %d displayed %d (want 5, 3)" % (shot, got[0], got[1]))
                else:
                    d40_notes.add(head)
                # スクロール領域の下に隠れた行も見る (小さな大きさでは、ren の行が見える範囲の外にある)。
                ren = [r for r in rows if r["object_name"] == "cordmuxRow" and
                       any(n["object_name"] == "cordmuxRowName" and n["full_text"] == "ren" and _inside(n, r, 1)
                           for n in rows)]
                said = " ".join(x["full_text"] + " " + x["tooltip"] + " " + x["a11y_name"] + " " + x["a11y_desc"]
                                for r in ren for x in rows if _inside(x, r, 1))
                if not ren or cant_show_full not in said:
                    d40_bad.append("%s no cant-show row for ren" % shot)
            # fixtures.cpp の people-unslotted-stages。(1) 枠に載っていない人の行にも、画質のテキスト (ren は
            # 「映像を表示できません」、sakura は「画質を下げています (解像度)」) が、状態のテキストか状態のアイコンの
            # ツールチップに表示される。(2) 解像度のテキストは、最初のフレームが届いてから表示される。フレームのない
            # Taro (枠に載っている)、ren、sakura の行の解像度の QLabel は空で、行の中のアクセシブルな説明と
            # ツールチップにも、その人の解像度がない。比べる相手の hanako (フレームが届いた) の行には、解像度の
            # QLabel のテキスト 960x540 がある (なければ、どれも表示しない実装でも通ってしまう)。スクロール領域の下の
            # 行も見る (rows のすべて)。
            if shot.startswith("people-unslotted-stages"):
                def row_of(name):
                    return next((r for r in rows if r["object_name"] == "cordmuxRow" and
                                 any(n["object_name"] == "cordmuxRowName" and n["full_text"] == name and _inside(n, r, 1)
                                     for n in rows)), None)

                def inner_of(row):
                    # 隠れた解像度の QLabel は、行の矩形の外に置かれることがある (狭い幅) ので、行のウィジェットは、
                    # 矩形の内側にあるか、同じ userId を props に持つもので拾う。
                    uid = next((p for p in row["props"].split(";") if p.startswith("userId=")), None)
                    return [x for x in rows if x is not row and
                            (_inside(x, row, 1) or (uid is not None and uid in x["props"].split(";")))]
                for who, want in (("ren", cant_show_full), ("さくら🌸", reduced_res_full)):
                    d51q_seen += 1
                    row = row_of(who)
                    if row is None:
                        d51q_bad.append("%s no row for %s" % (shot, who))
                    elif not any(x["object_name"] in ("cordmuxRowStatusText", "cordmuxRowStatus") and
                                 (want == x["full_text"] or want in x["tooltip"]) for x in inner_of(row)):
                        d51q_bad.append("%s %s lacks the quality text" % (shot, who))
                for who, size_text, framed in (("hanako", "960x540", True), ("Taro Yamada", "1600x900", False),
                                               ("ren", "1366x768", False), ("さくら🌸", "1152x648", False)):
                    d51r_seen += 1
                    row = row_of(who)
                    if row is None:
                        d51r_bad.append("%s no row for %s" % (shot, who))
                        continue
                    inner = inner_of(row)
                    label = "".join(x["full_text"] for x in inner if x["object_name"] == "cordmuxRowResolution")
                    said = " ".join(x["full_text"] + " " + x["tooltip"] + " " + x["a11y_name"] + " " + x["a11y_desc"]
                                    for x in inner)
                    if framed and size_text not in label:
                        d51r_bad.append("%s %s (frame arrived) lacks the resolution" % (shot, who))
                    elif not framed and (label.strip() or size_text in said):
                        d51r_bad.append("%s %s shows %s before the first frame" % (shot, who,
                                                                                  "a label" if label.strip() else "text"))
            # 行の状態のテキスト (製品が計算した境目の幅)。people-statuses、people-stages、glance-n1 の 230、267、430。
            if shot.startswith(("people-statuses", "people-stages", "glance-n1")) and size.split("x")[0] in \
                    ("230", "267", "430", "floor"):
                m = metrics.get(shot)
                em = (int(m["width"]) / float(m["width_em"]) if m and m["size_reached"] == "ok" and
                      float(m["width_em"] or 0) > 0 else em_px_dir)
                for row in people:
                    marks = parts_of(row, "cordmuxRowStatus")
                    # 状態を持つ行 (状態のテキストの QLabel に全文がある。狭くて QLabel を隠しても全文は残る)。
                    stated = any(x["object_name"] == "cordmuxRowStatusText" and x["full_text"].strip() and
                                 _inside(x, row, 1) for x in rows)
                    narrow_or_floor = size.startswith(("267", "floor"))
                    if not marks and not (stated and narrow_or_floor):
                        continue
                    c2_seen += 1
                    texts = parts_of(row, "cordmuxRowStatusText")
                    who = (parts_of(row, "cordmuxRowName") or [{"full_text": "?"}])[0]["full_text"][:10]
                    # 状態のテキストを表示し始める幅 (floor) より狭い 267 では、アイコンだけになるのが正しい。floor では、
                    # アイコンの右にテキストがある。
                    if size.startswith("267") and stated and not marks:
                        c2_noicon.append("%s %s" % (shot, who))
                    if size == "floor" and stated:
                        c2_floor_seen += 1
                        if not any(t["text"].strip() for t in texts):
                            c2_notext_floor.append("%s %s" % (shot, who))
                    if size.startswith("230") and any(t["text"].strip() for t in texts):
                        c2_text230.append("%s %s" % (shot, who))
                    for t in texts:
                        head = t["text"].split(chr(0x2026))[0]
                        if t["text"] != t["full_text"]:
                            # 短い形は省略記号なしで表示される。参考の先頭には、表示されているテキストのまま書く。
                            c2_heads.add(head + (chr(0x2026) if chr(0x2026) in t["text"] else ""))
                            if len(head) < 3:
                                c2_short.append("%s %s" % (shot, t["text"]))
                    # 表示されている状態のテキストごとに、先頭をチェックする。
                    for t in texts:
                        shown, full = t["text"], t["full_text"]
                        if not shown.strip() or not full:
                            continue
                        d34_seen += 1
                        cut = shown != full
                        head = shown.split(chr(0x2026))[0]
                        short = short_of.get(full)
                        tag = "%s %s [%s]" % (shot, who, shown)
                        # (a) 打ち消しの落ちた先頭: 全文が「ません」で終わり、表示されているテキストに「ません」も
                        #     短い形もない。
                        if lang == "ja-JP" and cut and full.endswith("ません") and "ません" not in shown and \
                                shown != short:
                            d34_neg.append(tag)
                        # (b) 画質の 4 つの先頭に「下げ」(en は "Lower")。
                        if full in quality_full and cut and quality_head not in head:
                            d34_quality.append(tag)
                        # (c) 打ち消しの 4 つ (短い形が全文と違う言語だけ): 全文の入らない QLabel では短い形を省略せずに
                        #     表示し、アクセシブル名に全文を入れる。下限の幅では必ず短い形。
                        if short is not None and short != full:
                            if cut:
                                d34_short_seen += 1
                                if shown != short:
                                    d34_short.append(tag + " not the short form")
                                elif full not in t["a11y_name"]:
                                    d34_short.append(tag + " a11y name lacks the full text")
                                d34_notes.append("%s %s=%dpx" % (size, shown, t["w"]))
                            if size == "floor":
                                d34_floor_seen += 1
                                if shown != short:
                                    d34_floor.append(tag)
                    # 映像を表示できない行には、解像度を表示しない。場面の人は古い 1280x720 / 30 FPS を持ったまま
                    # (表示の規則が、段階が unavailable の間に解像度を捨てるかの監視)。解像度の QLabel と、行の中の
                    # アクセシブル名、アクセシブルな説明、ツールチップを見る。
                    if any(t["full_text"] == cant_show_full for t in texts):
                        d340_seen += 1
                        inner = [x for x in rows if _inside(x, row, 1)]
                        res = [x for x in inner if x["object_name"] == "cordmuxRowResolution" and x["visible"] == "1" and
                               x["text"].strip()]
                        said = " ".join(x["a11y_desc"] + " " + x["tooltip"] + " " + x["a11y_name"] for x in inner)
                        if res or "1280" in said:
                            d340_bad.append("%s %s%s" % (shot, who, " label" if res else " text"))
                    for p in marks + parts_of(row, "cordmuxRowMenuButton") + parts_of(row, "cordmuxRowShare"):
                        if p["x"] + p["w"] > dock["w"] or p["x"] + p["w"] > row["x"] + row["w"] + 1:
                            c2_clip.append("%s %s %s" % (shot, who, p["object_name"]))
                    for n in parts_of(row, "cordmuxRowName"):
                        if em and n["w"] < 4 * em - 1:
                            c2_name.append("%s %s w=%d<4em=%.0f" % (shot, who, n["w"], 4 * em))
            # 状態のアイコンのツールチップ (状態のテキスト + 枠に何が表示されているかの 1 文。2 文以内)。アクセシブルな
            # 説明にも同じ 1 文がある。枠に載っていない録画の対象の人の行は、枠の 1 文の代わりに録画の 1 文
            # (rec_tips) を持つ。
            def has_second(full, text):
                return tips[full] in text or rec_tips.get(full, "\0") in text
            for row in people:
                for mark in parts_of(row, "cordmuxRowStatus"):
                    tip = mark["tooltip"]
                    full = next((t for t in tips if t != "\0" and t in tip), None)
                    if full is None:
                        continue
                    c3_seen += 1
                    descs = " ".join(x["a11y_desc"] for x in vis if _inside(x, row, 1))
                    why = []
                    if not has_second(full, tip):
                        why.append("tip lacks the slot sentence")
                    if _sentences(tip, lang) > 2:
                        why.append("tip has %d sentences" % _sentences(tip, lang))
                    if not has_second(full, descs):
                        why.append("a11y description lacks it")
                    if why:
                        c3_bad.append("%s %s (%s)" % (shot, full[:12], ",".join(why)))
                # 状態のテキストの QLabel のツールチップ。省略した QLabel でも、全文の後にアイコンと同じ 1 文がある
                # (状態のアイコンとテキストのツールチップは 2 文)。
                for label in parts_of(row, "cordmuxRowStatusText"):
                    full = next((t for t in tips if t != "\0" and t == label["full_text"]), None)
                    if full is None:
                        continue
                    c3_text_seen += 1
                    elided = label["text"] != label["full_text"]
                    if elided:
                        c3_text_elided += 1
                    tip = label["tooltip"]
                    why = []
                    if not has_second(full, tip):
                        why.append("tip lacks the slot sentence")
                    if elided and not tip.startswith(full):
                        why.append("tip lacks the full text")
                    if _sentences(tip, lang) > 2:
                        why.append("tip has %d sentences" % _sentences(tip, lang))
                    if why:
                        c3_text_bad.append("%s %s%s (%s)" % (shot, full[:12], " elided" if elided else "",
                                                             ",".join(why)))
            # 画面共有のメッセージの文 (430 で 2 行以下。録画の対象だけの場面は、枠の文がなく録画の文がある)。メッセージの
            # カードは状態タブだけにあるので、状態タブの画像だけを見る (参加者タブの画像には 1 行目の見出しだけがある)。
            if shot.startswith(("notice-all-video-stopped", "notice-no-video")) and status_tab:
                c5_text_seen += 1
                plain = lambda r: _unescape(r["text"]).replace(LS, "")
                bodies = [r for r in wraps.get(shot, []) if plain(r) in (no_video, no_video_wait, no_video_rec)]
                has_slot = any(plain(r) == no_video for r in bodies)
                has_rec = any(plain(r) == no_video_rec for r in bodies)
                # 場面ごとに期待する影響の文 (SHARE_NOTICE_EXPECT。枠に載っていて録画もしている人にも、録画の文を
                # 追加する。最初の映像が届かない人だけの場面には、止まった画像の文がない)。
                expect_slot, expect_wait, expect_rec = _share_expect(shot)
                if has_slot != expect_slot or has_rec != expect_rec:
                    c5_text_bad.append("%s slot=%s rec=%s" % (shot, has_slot, has_rec))
                # 影響の文は、映像が届かない参加者ごとに選ぶ。止まった人がいれば止まった画像の文、枠に載っていて最初の映像が
                # 届かない人がいれば空の枠の文、両方なら止まった画像の文が先で、どちらも 1 回。録画の文と次の操作は、
                # その後 (並びは wraps.tsv の y)。
                d53_seen += 1
                ys = {}
                for r in wraps.get(shot, []):
                    ys.setdefault(plain(r), []).append(int(r["y"] or 0))
                counts = (len(ys.get(no_video, [])), len(ys.get(no_video_wait, [])), len(ys.get(no_video_rec, [])))
                if no_video_wait == "\0":
                    d53_bad.append("%s no ini key no_video_waiting" % shot)
                elif counts != (int(expect_slot), int(expect_wait), int(expect_rec)):
                    d53_bad.append("%s slot=%d wait=%d rec=%d" % ((shot,) + counts))
                else:
                    order = [ys[t][0] for t in (no_video, no_video_wait, no_video_rec) if t in ys]
                    nexts = [y for t in (check_next, cant_next) for y in ys.get(t, [])]
                    if order != sorted(order) or (nexts and order and max(order) >= min(nexts)):
                        d53_bad.append("%s order" % shot)
                # 次の操作。エンコードの段階が unavailable の人がいれば、始め直しの文。unavailable でない人がいれば、
                # Vesktop で確認する文 (始め直しの文より先)。場面ごとの期待は SHARE_NEXT_EXPECT。
                d44_seen += 1
                ys = {}
                for r in wraps.get(shot, []):
                    if plain(r) in (check_next, cant_next):
                        ys.setdefault(plain(r), []).append(int(r["y"] or 0))
                want_check, want_cant = _next_expect(shot)
                got = (len(ys.get(check_next, [])), len(ys.get(cant_next, [])))
                if cant_next == "\0":
                    d44_bad.append("%s no ini key no_video_cant_show_next" % shot)
                elif got != (int(want_check), int(want_cant)):
                    d44_bad.append("%s check=%d cant=%d" % ((shot,) + got))
                elif want_cant and want_check and ys[check_next][0] >= ys[cant_next][0]:
                    d44_bad.append("%s cant-show next before the other next step" % shot)
                # 始め直しの文は「映像を表示できない人」に頼む文なので、その人の行が、目に見えるテキスト (状態の
                # テキストか状態のアイコンのツールチップ) で「映像を表示できません」と伝えていること (伝えなければ、
                # 誰に頼むかが画面から分からない)。
                if want_cant:
                    d44_rows_seen += 1
                    if not any(x["object_name"] in ("cordmuxRowStatusText", "cordmuxRowStatus") and
                               (cant_show_full in x["full_text"] or cant_show_full in x["tooltip"]) for x in rows):
                        d44_rows_bad.append(shot)
                if size.startswith("430") and bodies:
                    lines = sum(int(r["lines"] or 0) for r in bodies)
                    c5_lines.append("%s %d" % (shot, lines))
                    # 空の枠の文も枠についての 1 文なので、同じ 2 行の上限に入れる。
                    if any(int(r["lines"] or 0) > 2 for r in bodies if plain(r) in (no_video, no_video_wait)):
                        c5_text_bad.append("%s slot text over 2 lines at 430" % shot)
            # glance-n1: ren だけが止まり、kenji と Sakurazaka_Haru1 の映像は届いている。行の状態だけで分かるので、
            # 映像が届かないメッセージの文はない。本文を描画した行 (wraps.tsv) のない画像は数えない。
            # 状態タブの画像だけを見る (参加者タブにはカードの本文がなく、見ても何も見つからない)。
            if shot.startswith("glance-n1-") and wraps.get(shot) and status_tab:
                r08_seen += 1
                said = [_unescape(r["text"]).replace(LS, "") for r in wraps[shot]]
                if said.count(check_next) != 0:
                    r08_bad.append("%s check=%d" % (shot, said.count(check_next)))
            # notice-record-finalize-failed: メッセージのレベル 2 で、見出し、次の操作、録画のセッションの
            # フォルダーの場所の行、[作成を再開] (押せて、右で切れず、テキストを省略していない) がある。
            # 作成待ちのカードが同時に表示されるかは表示の規則が決めることなので、参考にだけ書く。
            # テキストの切れは、clipped text と edges.tsv のチェックがすべての場面で見る。
            if shot.startswith("notice-record-finalize-failed-") and status_tab:
                r01_seen += 1
                why = []
                if not any(failed_head in (x["full_text"], x["a11y_name"]) for x in vis):
                    why.append("no headline")
                if not any(_unescape(r["text"]).replace(LS, "") == failed_next for r in wraps.get(shot, [])) and \
                        not any(x["full_text"] == failed_next for x in vis):
                    why.append("no next step")
                locs = [x for x in vis if x["object_name"] == "cordmuxNoticeLocation"]
                if not any(x["full_text"].endswith("2000-01-01 12-00-00") for x in locs):
                    why.append("no session folder location")
                elif not any(x["text"].endswith("12-00-00") for x in locs):
                    r01_notes.add("%s location tail not shown" % size)
                buttons = [x for x in vis if x["class"].endswith("Button") and x["full_text"] == resume_button]
                if not buttons:
                    why.append("no resume button")
                elif not any(b["enabled"] == "1" for b in buttons):
                    why.append("resume button disabled")
                elif any(b["x"] + b["w"] > dock["w"] or b["text"] != b["full_text"] for b in buttons):
                    why.append("resume button cut")
                if any(pending_head in (x["full_text"], x["a11y_name"]) for x in vis):
                    r01_notes.add("pending card also shown")
                if why:
                    r01_bad.append("%s %s" % (shot, ",".join(why)))
            # 録画の対象だけの人 (枠に載っていない kenji) の映像が止まると、その行に状態のアイコンが表示され、状態の
            # テキストかアイコンのツールチップで「映像が届いていません」と伝え、ツールチップは枠に触れない録画の 1 文。
            # 同じ画像で枠の人の状態のテキストが表示されていれば、kenji の状態のテキストも表示される。比べる相手の
            # notice-recording-only (kenji は録画の対象で、映像が届いている) ではアイコンがない (これがないと、常に
            # 表示する実装でも通ってしまう)。
            # スクロール領域の下の行も見る (rows のすべて)。
            r09_on = shot.startswith(("notice-no-video-recording-only-", "notice-no-video-both-"))
            # 人の行は参加者タブにあるので、参加者タブの画像だけを見る (状態タブでは行が隠れている)。
            if (r09_on or shot.startswith("notice-recording-only-")) and not status_tab:
                r09_seen += 1
                kenji = next((r for r in rows if r["object_name"] == "cordmuxRow" and r["visible"] == "1" and
                              any(n["object_name"] == "cordmuxRowName" and n["full_text"] == "kenji" and
                                  _inside(n, r, 1) for n in rows)), None)
                if kenji is None:
                    r09_bad.append("%s no row for kenji" % shot)
                else:
                    uid = next((p for p in kenji["props"].split(";") if p.startswith("userId=")), None)
                    inner = [x for x in rows if x is not kenji and x["visible"] == "1" and
                             (_inside(x, kenji, 1) or (uid is not None and uid in x["props"].split(";")))]
                    marks = [x for x in inner if x["object_name"] == "cordmuxRowStatus"]
                    texts = [x for x in inner if x["object_name"] == "cordmuxRowStatusText" and x["text"].strip()]
                    if not r09_on:
                        if marks:
                            r09_bad.append("%s kenji has a status mark while his video arrives" % shot)
                    elif not marks:
                        r09_bad.append("%s kenji has no status mark" % shot)
                    else:
                        state = next((t for t in rec_tips if t != "\0" and
                                      any(t == x["full_text"] or t in x["tooltip"] for x in marks + texts)), None)
                        tip = " ".join(x["tooltip"] for x in marks)
                        others_text = any(x["object_name"] == "cordmuxRowStatusText" and x["visible"] == "1" and
                                          x["text"].strip() and not (_inside(x, kenji, 1) or
                                                                    (uid is not None and uid in x["props"].split(";")))
                                          for x in rows)
                        if state is None:
                            r09_bad.append("%s kenji's row does not say the state" % shot)
                        elif rec_tips[state] not in tip:
                            r09_bad.append("%s kenji's tip lacks the recording sentence" % shot)
                        elif tips.get(state, "\0") in tip:
                            r09_bad.append("%s kenji's tip talks about the slot" % shot)
                        elif others_text and not texts:
                            r09_bad.append("%s kenji's state text hidden while others show it" % shot)
                        r09_notes.add("%s text=%s" % (size, "yes" if texts else "no"))
            # 停止したアンインストールのカード。グループの間 (グループの見出しの前) が、グループの中の段落と場所の行の
            # 間より広い。場所の行 (cordmuxNoticeLocation) が、グループに残った場所の数だけある (参考)。
            if shot.startswith("status-uninstall-stopped") and status_tab:
                heads = {ini.get("cordmux.dialog.%s.text" % k, "\0") for k in ("not_deleted", "not_restored", "not_checked")}
                cards = [x for x in vis if x["object_name"] == "cordmuxNoticeCard"]
                png = os.path.join(wd_dir, shot + ".png")
                scale = _scale_of(wd_dir)
                for card in cards:
                    # 見た目の間隔で数える。ウィジェットの矩形どうしの間隔では、テーマが QLabel にだけ追加する上下の余白
                    # (Yami 系の .frame-notice QLabel) と、ボタンの背景と枠線を区別できない。テキストの QLabel と
                    # ボタンのすべてを行にし、行ごとに背景と違うピクセルの行 (文字のピクセル、ボタンの背景と枠線) の
                    # 上端と下端を読み、行と行の間をその差で数える。
                    parts = sorted([x for x in vis if _inside(x, card) and x is not card and
                                    (x["text"].strip() or x["class"].endswith("Button"))], key=lambda x: (x["y"], x["x"]))
                    # 同じ行のウィジェット (場所の QLabel と [フォルダーを開く]) は、1 つの行にまとめる。
                    lines = []
                    for x in parts:
                        if lines and x["y"] < lines[-1]["y"] + lines[-1]["h"]:
                            last = lines[-1]
                            x1 = max(last["x"] + last["w"], x["x"] + x["w"])
                            lines[-1] = dict(last, x=min(last["x"], x["x"]), w=x1 - min(last["x"], x["x"]),
                                             h=max(last["h"], x["y"] + x["h"] - last["y"]),
                                             heads=last["heads"] or x["full_text"] in heads)
                            continue
                        lines.append(dict(x, heads=x["full_text"] in heads))
                    # スクロール領域の見える範囲 (状態タブの cordmuxStatusScroll) に収まらない行は数えない (下が隠れた
                    # 行のピクセルでは、間隔を読み違える)。
                    view = next((x for x in vis if x["object_name"] == "cordmuxStatusScroll"), None)
                    ink = [_ink_rows(png, ln, card, scale) if view is None or _inside(ln, view) else None
                           for ln in lines]
                    gaps_group, gaps_in = [], []
                    for i in range(1, len(lines)):
                        if ink[i] is None or ink[i - 1] is None:
                            continue
                        gap = ink[i][0] - ink[i - 1][1] - 1
                        (gaps_group if lines[i]["heads"] and i > 1 else gaps_in).append(gap)
                    if gaps_group:
                        stop_seen.append(1)
                        if min(gaps_group) <= max(gaps_in or [0]):
                            stop_bad.append("%s group %s <= in %s" % (shot, min(gaps_group), max(gaps_in or [0])))
                        stop_gaps.append("%s %s/%s" % (shot, min(gaps_group), max(gaps_in or [0])))
                    locs = sum(1 for x in vis if x["object_name"] == "cordmuxNoticeLocation" and _inside(x, card))
                    stop_locs.append("%s %d" % (shot, locs))
            # 録画のボタンのテキストが省略されない。開始のボタンだけにツールチップ (画面共有と声を...) がある。
            for b in (x for x in vis if x["object_name"] == "cordmuxRecordButton"):
                c6_seen += 1
                why = []
                if b["text"] != b["full_text"] or chr(0x2026) in b["text"]:
                    why.append("elided")
                # 押せないときは、説明の後に理由の行が続く (押せないウィジェットのツールチップ)。先頭が説明か。
                if b["full_text"] == rec_start and not b["tooltip"].startswith(rec_tip):
                    why.append("start tip")
                if b["full_text"] == rec_stop and rec_tip in b["tooltip"]:
                    why.append("stop has the start tip")
                if why:
                    c6_bad.append("%s %s (%s)" % (shot, b["text"], ",".join(why)))
        if c2_seen:
            add(set_name, "row state icon missing at 267", scope, c2_seen, len(c2_noicon), "; ".join(c2_noicon)[:300])
            add(set_name, "row state text missing at the floor width", scope, c2_floor_seen, len(c2_notext_floor),
                "; ".join(c2_notext_floor)[:300])
            add(set_name, "row parts cut on the right", scope, c2_seen, len(c2_clip), "; ".join(c2_clip)[:300])
            # 境目は中身から計算するので、230 px でテキストが表示されるかは、フォントサイズと [密度] で変わる (参考。
            # [通常] と [クラシック] のフォントサイズ 10 では、230 px は境目より狭いので、r5 では 0 になる)。
            add(set_name, "row state text shown at 230 (record only)", scope, c2_seen, 0,
                "%d: %s" % (len(c2_text230), "; ".join(c2_text230)[:280]) if c2_text230 else "")
            add(set_name, "row name narrower than 4 em", scope, c2_seen, len(c2_name), "; ".join(c2_name)[:300])
            add(set_name, "shortened row state text head under 3 chars", scope, c2_seen, len(c2_short),
                "; ".join(c2_short)[:300])
            add(set_name, "shortened row state text heads (record only)", scope, len(c2_heads), 0,
                " | ".join(sorted(c2_heads))[:300])
        if d34_seen:
            add(set_name, "row state text head drops the negation", scope, d34_seen, len(d34_neg),
                "; ".join(d34_neg)[:300])
            add(set_name, "quality state text head lacks the lowering word", scope, d34_seen, len(d34_quality),
                "; ".join(d34_quality)[:300])
            add(set_name, "cut negative state not shown as the whole short form", scope, d34_short_seen,
                len(d34_short), ("; ".join(d34_short) + " | " + ", ".join(sorted(set(d34_notes))))[:300])
            add(set_name, "short form not shown at the 5.75 em floor", scope, d34_floor_seen, len(d34_floor),
                "; ".join(d34_floor)[:300])
        if d340_seen:
            add(set_name, "cant-show row shows a resolution", scope, d340_seen, len(d340_bad),
                "; ".join(d340_bad)[:300])
        if c3_seen:
            add(set_name, "row state tooltip and description", scope, c3_seen, len(c3_bad), "; ".join(c3_bad)[:300])
        if c3_text_seen:
            add(set_name, "row state text tooltip (elided too)", scope, c3_text_seen, len(c3_text_bad),
                ("elided %d; " % c3_text_elided + "; ".join(c3_text_bad))[:300])
        if d40_seen:
            add(set_name, "people-stages first line displayed count not 3 (unavailable counted)", scope, d40_seen,
                len(d40_bad), ("; ".join(d40_bad) + " | " + " / ".join(sorted(d40_notes)))[:300])
        if d44_seen:
            add(set_name, "share notice next step not chosen by stage and others' video", scope, d44_seen,
                len(d44_bad), "; ".join(d44_bad)[:300])
        if r08_seen:
            add(set_name, "glance-n1 has a no-video next step", scope, r08_seen, len(r08_bad),
                "; ".join(r08_bad)[:300])
        if r01_seen:
            add(set_name, "finalize failed notice lacks headline, location or resume", scope, r01_seen,
                len(r01_bad), ("; ".join(r01_bad) + " | " + ", ".join(sorted(r01_notes)))[:300])
        if r09_seen:
            add(set_name, "recording-only row lacks a status mark and text", scope, r09_seen, len(r09_bad),
                ("; ".join(r09_bad) + " | " + ", ".join(sorted(r09_notes)))[:300])
        if d44_rows_seen:
            add(set_name, "cant-show next step but no row says cant show", scope, d44_rows_seen, len(d44_rows_bad),
                "; ".join(d44_rows_bad)[:300])
        if d53_seen:
            add(set_name, "share notice consequence not chosen per person", scope, d53_seen, len(d53_bad),
                "; ".join(d53_bad)[:300])
        if d51q_seen:
            add(set_name, "unslotted row with a stage lacks the quality text", scope, d51q_seen, len(d51q_bad),
                "; ".join(d51q_bad)[:300])
        if d51r_seen:
            add(set_name, "resolution shown before the first frame", scope, d51r_seen, len(d51r_bad),
                "; ".join(d51r_bad)[:300])
        if c5_text_seen:
            add(set_name, "share notice text", scope, c5_text_seen, len(c5_text_bad),
                ("; ".join(c5_text_bad) + " | lines at 430: " + ", ".join(c5_lines))[:300])
        if stop_seen or stop_locs:
            add(set_name, "stopped card group gap not wider than in-group gaps", scope, len(stop_seen), len(stop_bad),
                ("; ".join(stop_bad) + " | group/in px: " + ", ".join(stop_gaps) + " | location rows: " +
                 ", ".join(stop_locs))[:400])
        if c6_seen:
            add(set_name, "record button text and tip", scope, c6_seen, len(c6_bad), "; ".join(c6_bad)[:300])
    # 途中で止まったときの結果のダイアログのグループの間。1 つ目のグループの最後の行の下端から、次のグループの見出しの
    # 上端までが、グループの中の行の間より広い。ドックのカードと同じく、見た目の間隔 (背景と違うピクセルの行) で数える。
    # 場面の違い (手順書と違う置き方、3 つのグループ = face-dialog-uninstall-stopped-<違い>) も同じように数える。3 つの
    # グループの場面は、2 つの境の両方をグループの間に数える。
    for wd in sorted(glob.glob(os.path.join(set_dir, "**", "face-dialog-uninstall-stopped*", "*", "*",
                                            "dialog-uninstall-stopped*.widgets.tsv"), recursive=True)):
        wd_dir = os.path.dirname(wd)
        scope = os.path.relpath(wd_dir, set_dir)
        # 本文をスクロールする形のダイアログ (r17 の低い画面) は数えない。スクロール領域の見える範囲の外の行は画像に
        # なく、見た目の間隔を読めない。グループの間は、スクロールしない形 (base と classic の 1920x1040) で数える。
        # 数えなかった画像は、r17 のチェックの note に出る。
        if _fit_rows(wd_dir).get(os.path.basename(wd)[:-len(".widgets.tsv")], {}).get("scrolls") == "yes":
            continue
        ini = ini_values(_lang_of(wd_dir))
        heads = {ini.get("cordmux.dialog.%s.text" % k, "\0") for k in ("not_deleted", "not_restored", "not_checked")}
        vis = [x for x in _widgets(wd) if x["visible"] == "1"]
        dlg = next((x for x in vis if x["depth"] == "0"), None)
        if dlg is None:
            continue
        labels = [x for x in vis if x is not dlg and x["class"] != "QPushButton" and x["text"].strip()]
        last_label = max((x["y"] + x["h"] for x in labels), default=0)
        parts = sorted([x for x in vis if x is not dlg and x["y"] < last_label and
                        (x["text"].strip() or x["class"].endswith("Button"))], key=lambda x: (x["y"], x["x"]))
        lines = []
        for x in parts:
            if lines and x["y"] < lines[-1]["y"] + lines[-1]["h"]:
                last = lines[-1]
                x1 = max(last["x"] + last["w"], x["x"] + x["w"])
                lines[-1] = dict(last, x=min(last["x"], x["x"]), w=x1 - min(last["x"], x["x"]),
                                 h=max(last["h"], x["y"] + x["h"] - last["y"]),
                                 heads=last["heads"] or x["full_text"] in heads)
                continue
            lines.append(dict(x, heads=x["full_text"] in heads))
        png = wd[:-len(".widgets.tsv")] + ".png"
        ink = [_ink_rows(png, ln, dlg, _scale_of(wd_dir)) for ln in lines]
        first_head = next((i for i, ln in enumerate(lines) if ln["heads"]), None)
        gaps_group, gaps_in = [], []
        for i in range(1, len(lines)):
            if first_head is None or i <= first_head or ink[i] is None or ink[i - 1] is None:
                continue
            gap = ink[i][0] - ink[i - 1][1] - 1
            (gaps_group if lines[i]["heads"] else gaps_in).append(gap)
        if gaps_group:
            bad = min(gaps_group) <= max(gaps_in or [0])
            add(set_name, "stopped dialog group gap not wider than in-group gaps", scope, 1, 1 if bad else 0,
                "group %s / in %s" % (",".join(str(g) for g in gaps_group), max(gaps_in or [0])))
    # [ツール] から始めたアンインストールの設定ウィンドウ。押せる場面 (settings-uninstall-ready) だけ確認のダイアログを
    # 開く関数が呼ばれ、どの場面でもページのボタンが見え、押せない場面ではボタンの下の理由も見える。
    for ue in sorted(glob.glob(os.path.join(set_dir, "**", "uninstall-entry.tsv"), recursive=True)):
        scope = os.path.relpath(os.path.dirname(ue), set_dir)
        r = read_tsv(ue)
        bad = []
        for x in r:
            ready = "settings-uninstall-ready" in x["shot"]
            why = []
            if x["button"] != "yes" or x["button_in_view"] != "yes":
                why.append("button not in view")
            if ready and x["confirm_called"] != "yes":
                why.append("confirm not opened")
            if not ready and x["confirm_called"] == "yes":
                why.append("confirm opened while blocked")
            if not ready and x["below_in_view"] != "yes":
                why.append("reason not in view")
            if why:
                bad.append("%s (%s)" % (x["shot"], ",".join(why)))
        add(set_name, "tools menu uninstall entry", scope, len(r), len(bad), "; ".join(bad)[:300])


def card_heights(root, before):
    """ドックのカード (props に noticeKey を持つウィジェット) の高さを、前回の出力 (before のフォルダー) の同じ画像と
    比べる (テキストの変更でカードの高さが変わっていないかを見る)。<root>/card-heights.tsv に書き、変わった数を返す。"""
    out = ["set\tscope\tshot\tnotice_key\tbefore_h\tafter_h"]
    changed = 0
    for wd in sorted(glob.glob(os.path.join(root, "*", "**", "face-dock", "*", "*", "*.widgets.tsv"), recursive=True)):
        rel = os.path.relpath(wd, root)
        old = os.path.join(before, rel)
        if not os.path.exists(old):
            continue

        def cards(path):
            got = {}
            for x in read_tsv(path):
                for part in (x.get("props") or "").split(";"):
                    if part.startswith("noticeKey=") and x["visible"] == "1":
                        got.setdefault(part[len("noticeKey="):], int(x["h"]))
            return got
        a, b = cards(old), cards(wd)
        for k in sorted(set(a) & set(b)):
            if a[k] != b[k]:
                changed += 1
                parts = rel.split(os.sep)
                out.append("\t".join([parts[0], os.path.dirname(rel), os.path.basename(wd)[:-len(".widgets.tsv")], k,
                                      str(a[k]), str(b[k])]))
    with open(os.path.join(root, "card-heights.tsv"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    print("card heights changed: %d (card-heights.tsv)" % changed)
    return changed


def fields_checks(set_dir, add, set_name):
    """fields.tsv のチェック (ウィンドウの 1 行の入力欄のテキストが、入力欄に入るか)。"""
    # ウィンドウの 1 行の入力欄のテキストが、入力欄に入るか (fields.tsv。保存先の入力欄のプレースホルダーなど)。
    for fl in sorted(glob.glob(os.path.join(set_dir, "**", "fields.tsv"), recursive=True)):
        scope = os.path.relpath(os.path.dirname(fl), set_dir)
        r = read_tsv(fl)
        # 参加者の設定ウィンドウの [cordmuxでの名前] のプレースホルダーは、左の [名前] の列と同じテキストなので、切れても
        # 情報は失われない。対象から外す (入れた名前 what=text は対象のまま。外した数は詳細に書く)。ほかの入力欄
        # (保存先のフォルダーなど) のプレースホルダーは、切れると情報が失われるので対象のまま。
        same_as_column = [x for x in r if is_name_placeholder(scope, x)]
        r = [x for x in r if x not in same_as_column]
        # 入力欄のテキストの末尾 (書き込み先のフォルダーの名前) が切れて見えない。全文がツールチップにあるかは、
        # 詳細に書く (製品は真ん中を省略して全文をツールチップに入れるが、ツールチップだけではプレースホルダーの
        # 末尾は見えない)。
        # 省略したテキスト (elided) も、全文が見えていないものに数える (普通の長さの場所が、すべて見えるかを見る)。
        narrow = [x for x in r if x["fits"] != "yes" or x.get("elided") == "yes"]
        add(set_name, "line edit full text not visible", scope, len(r), len(narrow),
            ("; ".join(sorted({"%s %s %s %s/%s elided=%s tip=%s" % (
                x["shot"], x["object_name"] or "-", x["what"], x["text_px"], x["room_px"], x.get("elided", "-"),
                x["tooltip_has_full"]) for x in narrow})) +
             (" | excluded name placeholders %d" % len(same_as_column) if same_as_column else ""))[:300])


def summarize(root, dest=None):
    """すべてのセットのチェックを実行し、結果を summary.tsv と summary.md に書く。

    dest を渡すと、結果を root ではなく dest に書く (別の出力先を現在のチェックで数え直すとき、その出力先に書かないため)。
    """
    rows = []
    dest = dest or root

    def add(set_name, check, scope, value, bad, note=""):
        rows.append({"set": set_name, "check": check, "scope": scope, "value": str(value), "bad": str(bad), "note": note})

    for set_dir in sorted(glob.glob(os.path.join(root, "*"))):
        if not os.path.isdir(set_dir):
            continue
        set_name = os.path.basename(set_dir)
        # ドックのチェック (metrics.tsv)
        for m in sorted(glob.glob(os.path.join(set_dir, "**", "face-dock", "*", "*", "metrics.tsv"), recursive=True)):
            scope = os.path.relpath(os.path.dirname(m), set_dir)
            r = read_tsv(m)
            heights = sorted({x["first_line_h"] for x in r})
            add(set_name, "first line height values", scope, ",".join(heights), 0 if len(heights) == 1 else 1)
            add(set_name, "first line hidden at some scroll", scope, len(r),
                sum(1 for x in r if "NO" in x["m1_first_line_visible(top/mid/end)"]))
            add(set_name, "clipped text", scope, len(r), sum(1 for x in r if x["m5_clipped"] not in ("0", "")),
                "; ".join(sorted({x["clipped_parts"] for x in r if x["clipped_parts"]}))[:300])
            add(set_name, "horizontal scroll", scope, len(r), sum(1 for x in r if x["h_overflow"] == "1"))
            add(set_name, "targets failing 2.5.8 per scene (metrics.tsv)", scope, len(r),
                sum(int(x["m7_targets_failing_2.5.8"] or 0) for x in r),
                ",".join(sorted({k for x in r for k in x["m7_failing_kinds"].split(",") if k})))
            unreached = unreached_sizes(r)
            add(set_name, "size not reached", scope, len(r), len(unreached), ",".join(unreached)[:300])
            # 計算した最小幅が 300 px を超えるか。ドックの最小幅は中身から計算する (現在は 256 から 294 px)。しきい値は、
            # 最小幅が大きくなりすぎたことに気づくための上限で、OBS の音声ミキサーの最小幅 (220 px) などの外の事実では
            # ない。230x165 は試しの大きさなので、最小幅より狭くても数えない。同じ幅の場面は名前の順に並べる (集合の
            # 並びのままだと、Python の文字列のハッシュの種で詳細の順が実行ごとに変わり、出力を比べたときに差が出る)。
            limit = min_width_limit_px(scope)
            wide = sorted({(x["scene"].rsplit("-", 1)[0], int(x["min_width"])) for x in r
                           if int(x["min_width"] or 0) > limit}, key=lambda t: (-t[1], t[0]))
            add(set_name, "computed min width over %d px" % MIN_WIDTH_LIMIT_PX, scope,
                len({x["scene"].rsplit("-", 1)[0] for x in r}),
                len({t[0] for t in wide}), "limit %d; max %s; %s" % (limit, max((int(x["min_width"] or 0) for x in r),
                                                                               default=0),
                                                                     ", ".join("%s %d" % t for t in wide[:8]))[:300])
            add(set_name, "labels with glyphs cut (suspected)", scope, len(r),
                sum(int(x.get("glyph_clip_suspected") or 0) for x in r))
            rec = {}
            for x in r:
                if x["scene"].startswith("record-"):
                    size = x["scene"].rsplit("-", 1)[1]
                    rec.setdefault(size, set()).add(x["rec_button_w"])
            add(set_name, "record button width varies by state", scope, len(rec),
                sum(1 for v in rec.values() if len(v) > 1))
            # 長い名前の人がいても、最小幅が変わらない (名前の QLabel は小さい最小幅を返す)。
            by_scene = {x["scene"]: x for x in r}
            raised = []
            for x in r:
                if not x["scene"].startswith("rows-name-lengths-"):
                    continue
                size = x["scene"][len("rows-name-lengths-"):]
                base = by_scene.get("status-connected-in-call-" + size)
                if base is not None and int(x["min_width"]) > int(base["min_width"]) and \
                        x["v_scrollbar"] == base["v_scrollbar"]:
                    raised.append(size)
            add(set_name, "long names raise the min width", scope, len(r), len(raised), ",".join(raised))
            low = [x for x in r if x["min_text_contrast"] and 0 < float(x["min_text_contrast"]) < 4.5]
            add(set_name, "enabled text contrast below 4.5", scope, len(r), len(low),
                ",".join(sorted({x["scene"].rsplit("-", 1)[0] for x in low}))[:300])
        # 押せるもの (targets.tsv)。cordmux のウィジェットとメニューの項目を分ける (メニューは参考)。
        for t in sorted(glob.glob(os.path.join(set_dir, "**", "targets.tsv"), recursive=True)):
            scope = os.path.relpath(os.path.dirname(t), set_dir)
            r = [x for x in read_tsv(t) if x["visible"] == "yes"]
            ours = [x for x in r if x["kind"] != "menu-item" and not x["kind"].startswith("other:")]
            add(set_name, "cordmux targets failing 2.5.8 (targets.tsv)", scope, len(ours),
                sum(1 for x in ours if x["spacing_ok"] != "yes"),
                ",".join(sorted({x["kind"] for x in ours if x["spacing_ok"] != "yes"})))
            items = [x for x in r if x["kind"] == "menu-item"]
            if items:
                heights = sorted({int(x["rect"].split("x")[-1]) for x in items})
                add(set_name, "menu item heights (record only)", scope, len(items), 0,
                    "min %d max %d" % (heights[0], heights[-1]))
            others = [x for x in r if x["kind"].startswith("other:")]
            if others:
                add(set_name, "other targets failing 2.5.8 (windows)", scope, len(others),
                    sum(1 for x in others if x["spacing_ok"] != "yes"),
                    ",".join(sorted({x["label"] for x in others if x["spacing_ok"] != "yes"}))[:300])
        # 一覧の上の 1 行 (cordmuxListReason) が、全文の数 (「3人まで」の 3) を表示していない。
        lost = []
        seen = 0
        for wd in sorted(glob.glob(os.path.join(set_dir, "**", "face-dock", "*", "*", "*.widgets.tsv"), recursive=True)):
            for x in read_tsv(wd):
                if x["object_name"] != "cordmuxListReason" or x["visible"] != "1":
                    continue
                seen += 1
                digits = "".join(c for c in x["full_text"] if c in _DIGITS)
                if digits and not all(c in x["text"] for c in digits):
                    lost.append("%s/%s" % (os.path.basename(os.path.dirname(os.path.dirname(wd))) +
                                           "/" + os.path.basename(os.path.dirname(wd)),
                                           os.path.basename(wd)[:-len(".widgets.tsv")]))
        if seen:
            add(set_name, "list top line hides its number", "face-dock", seen, len(lost), "; ".join(lost)[:300])
        # 名前の見分け (names.tsv。表示されている名前と 2 行目の組、アクセシブル名と説明の組が、どの行でも違う)。
        for n in sorted(glob.glob(os.path.join(set_dir, "**", "names.tsv"), recursive=True)):
            scope = os.path.relpath(os.path.dirname(n), set_dir)
            shots = {}
            for x in read_tsv(n):
                shots.setdefault(x["shot"], []).append(x)
            same_seen = []
            same_a11y = []
            for shot, xs in shots.items():
                seen_keys = [(x["shown"], x["line2_shown"]) for x in xs]
                a11y_keys = [(x["a11y_name"], x["a11y_desc"]) for x in xs]
                if len(set(seen_keys)) != len(seen_keys):
                    same_seen.append(shot)
                if len(set(a11y_keys)) != len(a11y_keys):
                    same_a11y.append(shot)
            add(set_name, "rows that look the same", scope, len(shots), len(same_seen), ",".join(same_seen)[:300])
            add(set_name, "rows with the same a11y name and description", scope, len(shots), len(same_a11y),
                ",".join(same_a11y)[:300])
            # 2 行目に 2 つ以上の事実が並ぶ行の縮め方。
            lang = os.path.basename(os.path.dirname(n))
            checked, lost_tail, lost_head, order = line2_checks(read_tsv(n), lang)
            add(set_name, "line 2 username tail lost", scope, checked, len(lost_tail), ",".join(lost_tail)[:300])
            add(set_name, "line 2 fact head lost", scope, checked, len(lost_head), ",".join(lost_head)[:300])
            add(set_name, "line 2 drop order broken", scope, checked, len(order), ",".join(order)[:300])
        # ウィンドウのウィジェットが、ウィンドウとスクロール領域の右端を超える (横にスクロールさせない)。
        for wd in sorted(glob.glob(os.path.join(set_dir, "**", "face-settings*", "*", "*", "*.widgets.tsv"),
                                   recursive=True) +
                         glob.glob(os.path.join(set_dir, "**", "face-participants", "*", "*", "*.widgets.tsv"),
                                   recursive=True)):
            scope = os.path.relpath(os.path.dirname(wd), set_dir)
            shot = os.path.basename(wd)[:-len(".widgets.tsv")]
            bad = window_overflow(read_tsv(wd))
            add(set_name, "window parts past the scroll area right edge", scope + "/" + shot, 1, len(bad),
                " / ".join(bad[:4])[:300])
        fields_checks(set_dir, add, set_name)
        # 折り返す QLabel と場所の QLabel (wraps.tsv)
        wraps_checks(set_dir, add, set_name)
        # ドックの行、メッセージ、ボタン
        r5_checks(set_dir, add, set_name)
        # ウィンドウが画面に収まるか (screen-fit.tsv)、メッセージのレベル 1 が 2 件並ぶ場面、途中で止まったアンインストール
        r17_checks(set_dir, add, set_name)
        # 配信中に Vesktop が終了した場面と、en の見出しの省略 (参考)
        r18_checks(set_dir, add, set_name)
        # 1 行目の短い形と省略 (短い形を省略せずに表示するか、省略が打ち消し・短縮形・数の途中で切れないか、本文の先頭)
        first_line_checks(set_dir, add, set_name)
        # シーンのドックと cordmux のテキストの左端 (beside-scenes-edges.tsv。差が 0.25 em 以内か)。em は、OBS の本文の
        # フォントサイズ (出力先の名前の f<フォントサイズ>。なければ 10 pt) を 96 dpi の px にしたもの。
        for e in sorted(glob.glob(os.path.join(set_dir, "**", "beside-scenes-edges.tsv"), recursive=True)):
            scope = os.path.relpath(os.path.dirname(e), set_dir)
            r = read_tsv(e)
            scenes = [int(x["ink_left_in_content"]) for x in r if x["dock"] == "scenesDock"]
            # 下のバーの録画のボタンに半分以上隠れた測定の対象 (OBS の既定のレイアウトのドックは低く、[密度] が狭いと参加者の行が
            # 見える範囲の外に出る) は、帯の中のピクセルが録画のボタンのものなので測らない。数は詳細に書く。
            bar = [_band_rows(x["band"]) for x in r if x["dock"] == "cordmux" and x["part"].startswith("cordmuxRecordButton")]

            def hidden_rows(band):
                top, bottom = _band_rows(band)
                return max([0] + [min(bottom, b[1]) - max(top, b[0]) for b in bar]) * 2 >= bottom - top > 0
            covered = [x for x in r if x["dock"] == "cordmux" and not x["part"].startswith("cordmuxRecordButton") and
                       hidden_rows(x["band"])]
            ours = [(x["part"].split(" ")[0], int(x["ink_left_in_content"])) for x in r
                    if x["dock"] == "cordmux" and not x["part"].startswith("cordmuxRecordButton") and x not in covered]
            if not scenes or not ours:
                continue
            font_pt = 10.0
            for part in scope.replace("\\", "/").split("/"):
                for piece in part.split("-"):
                    if piece.startswith("f") and piece[1:].isdigit():
                        font_pt = float(piece[1:])
            limit = 0.25 * font_pt * 96.0 / 72.0
            worst = max(abs(v - scenes[0]) for _, v in ours)
            add(set_name, "text left edge differs from the scenes dock by more than 0.25 em", scope, len(ours),
                1 if worst > limit else 0,
                ("scenes %d / %s (limit %.1f px)" % (scenes[0], ", ".join("%s %d" % p for p in ours), limit)) +
                (" | not measured, behind the bottom bar: %s" % ", ".join(x["part"].split(" ")[0] for x in covered)
                 if covered else ""))
        # 文字の上下の見切れ (edges.tsv。ドック以外の描画の対象も)
        for e in sorted(glob.glob(os.path.join(set_dir, "**", "edges.tsv"), recursive=True)):
            scope = os.path.relpath(os.path.dirname(e), set_dir)
            r = read_tsv(e)
            sus = [x for x in r if x.get("clip_suspected") == "yes"]
            add(set_name, "edges.tsv clip suspected", scope, len(r), len(sus),
                "; ".join(sorted({"%s %s (%s<%s/%s)" % (x["object_name"] or "-", x["text"][:14], x["label_h"],
                                                        x["font_h"], x["ink_h"]) for x in sus}))[:300])
        # フォーカスリング (focus.tsv)
        for f in sorted(glob.glob(os.path.join(set_dir, "**", "focus.tsv"), recursive=True)):
            scope = os.path.relpath(os.path.dirname(f), set_dir)
            r = read_tsv(f)
            add(set_name, "focus ring with a missing side", scope, len(r),
                sum(1 for x in r if x["sides_drawn(TRBL)"] != "TRBL"),
                ",".join(sorted({x["target"] for x in r if x["sides_drawn(TRBL)"] != "TRBL"})))
            # コントラスト比は、4 辺が描画されたフォーカスリングだけで数える (切れたリングの辺の読み取りは、切れ方で外と
            # 内の列が入れ替わり、当てにならない)。
            whole = [x for x in r if x["sides_drawn(TRBL)"] == "TRBL"]
            add(set_name, "whole focus ring contrast below 3", scope, len(whole),
                sum(1 for x in whole if min(float(x["min_ratio_outer"] or 0), float(x["min_ratio_inner"] or 0)) < 3.0))
            # フォーカスリングの外側のピクセルごとのコントラスト比 (辺の一部が隣のウィジェットに接する所)。列がない
            # 表もある。
            if whole and "outer_px_below_3" in whole[0]:
                low = [x for x in whole if int(x["outer_px_below_3"] or 0) > 0]
                add(set_name, "focus ring outer pixels below 3:1", scope, len(whole), len(low),
                    "; ".join("%s %s px=%s min=%s" % (x["shot"], x["sides_below_3"], x["outer_px_below_3"],
                                                      x["min_ratio_outer_px"]) for x in low)[:300])
            # メッセージのレベル 1 のパネルの中のフォーカスリング (詳細のボタンと、メッセージのレベル 1 のボタン)。リングの
            # 外周とパネルの外周の間が、パネルの枠線の太さより広い (リングが枠線に重ならず、間にパネルが 1 px
            # 以上残るので、リングと枠線が 1 本に見えない)。
            on_surface = [x for x in r if x.get("surface_gap", "-") not in ("-", "")]
            if on_surface:
                tight = [x for x in on_surface if int(x["surface_gap"]) <= int(x["surface_border"] or 0)]
                add(set_name, "focus ring on the tier 1 surface touches its edge", scope, len(on_surface), len(tight),
                    "; ".join("%s gap=%s border=%s" % (x["shot"], x["surface_gap"], x["surface_border"])
                              for x in (tight or on_surface))[:300])
        # テーマの切り替え (theme-switch.tsv)
        for f in sorted(glob.glob(os.path.join(set_dir, "**", "theme-switch.tsv"), recursive=True)):
            scope = os.path.relpath(os.path.dirname(f), set_dir)
            r = read_tsv(f)
            add(set_name, "theme switch differs from a fresh dock", scope, len(r),
                sum(1 for x in r if x["same_as_fresh"] != "yes"))
        # 時間を進める場面 (timeline.tsv)
        for f in sorted(glob.glob(os.path.join(set_dir, "**", "timeline.tsv"), recursive=True)):
            scope = os.path.relpath(os.path.dirname(f), set_dir)
            r = read_tsv(f)
            held = [x for x in r if x["press_hold"] == "yes"]
            moved_while_held = sum(1 for x in held if x["shown_cards"] != r[r.index(x) - 1]["shown_cards"])
            add(set_name, "cards moved while held", scope, len(held), moved_while_held)
        # メニューのチェックマーク (menu-check.tsv)
        for f in sorted(glob.glob(os.path.join(set_dir, "**", "menu-check.tsv"), recursive=True)):
            scope = os.path.relpath(os.path.dirname(f), set_dir)
            r = read_tsv(f)
            for x in r:
                if x["checked"] == "1-vs-0":
                    add(set_name, "menu-check %s %s pixels differing" % (x["variant"], x["item"]), scope,
                        x["gutter_pixels_differing"], 0)
        # 読み取った色 (contrast.tsv)。押せるテキストのコントラスト比が 4.5 未満
        for f in sorted(glob.glob(os.path.join(set_dir, "**", "contrast.tsv"), recursive=True)):
            scope = os.path.relpath(os.path.dirname(f), set_dir)
            allrows = read_tsv(f)
            # 描画された文字の色 (drawn_ratio) で数える。列のない表では、パレットの色のコントラスト比で数える。
            col = "drawn_ratio" if allrows and "drawn_ratio" in allrows[0] else "ratio"
            r = [x for x in allrows if x["enabled"] == "yes"]
            low = [x for x in r if float(x[col]) < 4.5]
            add(set_name, "contrast.tsv enabled text below 4.5 (drawn)", scope, len(r), len(low),
                "; ".join(sorted({"%s %s %s/%s" % (x["object_name"] or x["class"], x["text"][:12],
                                                   x.get("drawn_fg", x["fg"]), x["bg"]) for x in low}))[:400])
            dis = [x for x in allrows if x["enabled"] == "no"]
            if dis:
                worst = min(dis, key=lambda x: float(x[col]))
                add(set_name, "disabled text min contrast (record only)", scope, len(dis), 0,
                    "%s %s %s/%s %s" % (worst["object_name"] or worst["class"], worst["text"][:12],
                                        worst.get("drawn_fg", worst["fg"]), worst["bg"], worst[col]))
    short_form_coverage(root, add)
    os.makedirs(dest, exist_ok=True)
    with open(os.path.join(dest, "summary.tsv"), "w", encoding="utf-8", newline="\n") as f:
        w = csv.DictWriter(f, fieldnames=["set", "check", "scope", "value", "bad", "note"], delimiter="\t",
                           lineterminator="\n", quoting=csv.QUOTE_NONE, escapechar="\\")
        w.writeheader()
        for r in rows:
            w.writerow(r)
    bad = [r for r in rows if r["bad"] not in ("0", "")]
    with open(os.path.join(dest, "summary.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write("# ui_preview のセットごとのチェックの結果\n\n")
        f.write("チェックした行 %d、0 でない行 %d。すべての行は summary.tsv にある。\n\n" % (len(rows), len(bad)))
        f.write("| セット | チェック | 出力先 | 対象 | 0 でない数 | 詳細 |\n|---|---|---|---|---|---|\n")
        for r in bad:
            f.write("| %s | %s | %s | %s | %s | %s |\n" % (r["set"], r["check"], r["scope"].replace("\\", "/"),
                                                         r["value"], r["bad"], r["note"].replace("|", "/")))
    print("summary: %d rows, %d non-zero" % (len(rows), len(bad)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe")
    ap.add_argument("--out")
    ap.add_argument("--lowprio")
    ap.add_argument("--sets", default="base,classic,obs32,grid,win14,rd3,rd4,rm6,layout,r5,r17,r18,r19,worst")
    ap.add_argument("--summary-only", action="store_true")
    ap.add_argument("--summary-dir", help="結果 (summary.tsv と summary.md) を書くフォルダー (既定は --out。"
                                          "--summary-only で別の出力先を数え直すときに、そこへ書かないために使う)")
    ap.add_argument("--before", help="前回の出力先 (カードの高さを同じ画像と比べ、card-heights.tsv に書く)")
    ap.add_argument("--selftest", action="store_true", help="1 行目のチェックが失敗を検出できるかを確認して終了する")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.out:
        ap.error("--out is required")
    if a.summary_dir and a.before:
        ap.error("--before writes card-heights.tsv into --out; do not combine it with --summary-dir")
    os.makedirs(a.out, exist_ok=True)
    if not a.summary_only:
        all_sets = sets()
        results = []
        for name in a.sets.split(","):
            set_dir = os.path.join(a.out, name)
            os.makedirs(set_dir, exist_ok=True)
            log = os.path.join(set_dir, "run.log")
            for job in all_sets[name]:
                args = list(job)
                sub = None
                if "--subdir" in args:
                    i = args.index("--subdir")
                    sub = args[i + 1]
                    del args[i:i + 2]
                out_dir = os.path.join(set_dir, sub) if sub else set_dir
                t0 = time.time()
                code = run(a.exe, args + ["--out", out_dir], log, a.lowprio)
                results.append((name, " ".join(args), code, time.time() - t0))
                print("%s exit=%d %.1fs %s" % (name, code, time.time() - t0, " ".join(args)), flush=True)
        with open(os.path.join(a.out, "runs.tsv"), "a", encoding="utf-8", newline="\n") as f:
            for name, args, code, secs in results:
                f.write("%s\t%d\t%.1f\t%s\n" % (name, code, secs, args))
        failed = [r for r in results if r[2] != 0]
        print("jobs %d, failed %d" % (len(results), len(failed)))
    summarize(a.out, a.summary_dir)
    if a.before:
        card_heights(a.out, a.before)


if __name__ == "__main__":
    sys.exit(main())
