# SPDX-License-Identifier: Apache-2.0

# ドック (src-obs/dock) のソースをターゲットに追加する。ドックは、ledger が決めた表示を描く Qt のウィジェットと、
# それを OBS に追加し、状態をポーリングし、押された操作を実行する部分からなる。src-obs/CMakeLists.txt がこのファイルを
# include し、cordmux_add_dock(<target>) を 1 回呼ぶ (ledger/ledger.cmake と同じ形)。Qt6::Widgets の find_package と
# リンクは、src-obs/CMakeLists.txt が行う (ENABLE_QT のとき)。
# Qt を含まないソースだけを追加する cordmux_add_dock_observation() もあり、単体テストはこちらだけを呼ぶ。
# 表示の判断とその入力を作る処理を、Qt なしで確認するためである。

# include() は呼び出し元の CMAKE_CURRENT_SOURCE_DIR を変えないので、このファイルのディレクトリを先に変数に入れておく
# (ledger/ledger.cmake と同じ)。
set(CORDMUX_DOCK_DIR "${CMAKE_CURRENT_LIST_DIR}")

# ---- ドックの部分ごとの .cmake ----
# ドックのうち 4 つの部分は、それぞれ自分の .cmake でソースを追加する。どのファイルもなくてよい (OPTIONAL)。
# あるファイルは次の 2 つの関数を定義してよく、下の 2 つの関数が、定義されているかを確認してから呼ぶ。
#   cordmux_add_<名前>_observation(target)  Qt を含まないソースだけ (align_tests にも入る)
#   cordmux_add_<名前>(target)              Qt を含むソース (製品だけ)。_observation の側は追加しなくてよい
#                                           (cordmux_add_dock が先に cordmux_add_dock_observation を呼ぶので、
#                                           そこで入る。追加しても、同じファイルは 1 つのターゲットに 1 回だけ入る)
# <名前> は、ファイルの名前から .cmake を除いたものである。
#   ui/dock_ui.cmake  -> dock_ui      (ウィジェット: アイコン、フォントサイズ、メッセージのダイアログなど)
#   rows.cmake        -> rows         (参加者の一覧)
#   windows.cmake     -> windows      (設定ウィンドウと参加者の設定ウィンドウ)
#   install_ui.cmake  -> install_ui   (インストールとアンインストールのダイアログ)
# 関数の外でソースを追加しない (追加すると、align_tests に Qt のソースが入る)。新しくファイルを置いたら、
# configure をやり直す (ないファイルは、configure をやり直す条件に入らない)。
include("${CORDMUX_DOCK_DIR}/ui/dock_ui.cmake" OPTIONAL)
include("${CORDMUX_DOCK_DIR}/rows.cmake" OPTIONAL)
include("${CORDMUX_DOCK_DIR}/windows.cmake" OPTIONAL)
include("${CORDMUX_DOCK_DIR}/install_ui.cmake" OPTIONAL)

# Qt を含まないソースだけを追加する (src-obs/tests もこれを呼ぶ)。settings_fields.* (settings.json のキーごとに、
# 値を書き換える所を決めた表)、install_flow.* (インストールに関わる判断)、self_frame_check.* (自分の行のチェックを Pipeline に
# 渡し、Pipeline の答えを受ける所の判断) も、build_observation.* と同じく、判断を単体テストで確認するために Qt の側から分けて
# ある。install_flow.h が DockPollConfig を受け取るので、DockPollConfig は Qt を含まない dock_poll_config.h にある。
function(cordmux_add_dock_observation target)
  target_sources(
    ${target}
    PRIVATE
      "${CORDMUX_DOCK_DIR}/build_observation.h"
      "${CORDMUX_DOCK_DIR}/build_observation.cpp"
      "${CORDMUX_DOCK_DIR}/settings_fields.h"
      "${CORDMUX_DOCK_DIR}/settings_fields.cpp"
      "${CORDMUX_DOCK_DIR}/dock_poll_config.h"
      "${CORDMUX_DOCK_DIR}/install_flow.h"
      "${CORDMUX_DOCK_DIR}/install_flow.cpp"
      "${CORDMUX_DOCK_DIR}/self_frame_check.h"
      "${CORDMUX_DOCK_DIR}/self_frame_check.cpp"
  )

  # 部分ごとの .cmake の、Qt を含まない側 (このファイルの上の「ドックの部分ごとの .cmake」)。
  if(COMMAND cordmux_add_dock_ui_observation)
    cordmux_add_dock_ui_observation(${target})
  endif()
  if(COMMAND cordmux_add_rows_observation)
    cordmux_add_rows_observation(${target})
  endif()
  if(COMMAND cordmux_add_windows_observation)
    cordmux_add_windows_observation(${target})
  endif()
  if(COMMAND cordmux_add_install_ui_observation)
    cordmux_add_install_ui_observation(${target})
  endif()
endfunction()

# ドックのウィジェットのうち、libobs をほとんど使わないもの。製品 (cordmux_add_dock) とプレビューツール
# (src-obs/tests/ui_preview/ui_preview.cmake) の両方が呼び、同じ一覧を 2 か所に書かないようにする。
# ここのソースが libobs から使ってよいのは、プレビューツールの stub/ が持つ関数 (obs-module.h の obs_module_text と、
# obs-frontend-api.h の obs_frontend_get_locale_string と obs_frontend_get_main_window) だけである。ほかの関数を使う
# ソースは、cordmux_add_dock の側に置く (dock_bind と dock_poll*)。参加者の一覧 (rows.cmake) は呼び出し元が一緒に追加する。
function(cordmux_add_dock_face target)
  target_sources(
    ${target}
    PRIVATE
      "${CORDMUX_DOCK_DIR}/dock_widget.h"
      "${CORDMUX_DOCK_DIR}/dock_widget.cpp"
      "${CORDMUX_DOCK_DIR}/dock_look.h"
      "${CORDMUX_DOCK_DIR}/first_line.h"
      "${CORDMUX_DOCK_DIR}/first_line.cpp"
      "${CORDMUX_DOCK_DIR}/notice_list.h"
      "${CORDMUX_DOCK_DIR}/notice_list.cpp"
      "${CORDMUX_DOCK_DIR}/bottom_bar.h"
      "${CORDMUX_DOCK_DIR}/bottom_bar.cpp"
      "${CORDMUX_DOCK_DIR}/accounts_bar.h"
      "${CORDMUX_DOCK_DIR}/accounts_bar.cpp"
      "${CORDMUX_DOCK_DIR}/dock_tabs.h"
      "${CORDMUX_DOCK_DIR}/dock_tabs.cpp"
      "${CORDMUX_DOCK_DIR}/inline_confirm.h"
      "${CORDMUX_DOCK_DIR}/inline_confirm.cpp"
      "${CORDMUX_DOCK_DIR}/dock_menus.h"
      "${CORDMUX_DOCK_DIR}/dock_menus.cpp"
      "${CORDMUX_DOCK_DIR}/rename_dialog.h"
      "${CORDMUX_DOCK_DIR}/rename_dialog.cpp"
      "${CORDMUX_DOCK_DIR}/layout_hold.h"
      "${CORDMUX_DOCK_DIR}/layout_hold.cpp"
      "${CORDMUX_DOCK_DIR}/deferred_save.h"
      "${CORDMUX_DOCK_DIR}/deferred_save.cpp"
      "${CORDMUX_DOCK_DIR}/look_tab.h"
      "${CORDMUX_DOCK_DIR}/look_tab.cpp"
      "${CORDMUX_DOCK_DIR}/status_tab.h"
      "${CORDMUX_DOCK_DIR}/status_tab.cpp"
  )
endfunction()

# 製品のターゲットに、Qt を含むソースもすべて追加する。
function(cordmux_add_dock target)
  cordmux_add_dock_observation(${target})

  # Qt の小文字のキーワード (signals、slots、emit、foreach) を、マクロとして定義させない。Qt のヘッダーを先に読むと、
  # pipeline/pipeline.h の PipelineSnapshot::slots が `slots` マクロに置き換えられて、コンパイルエラー (C2208) になる。
  # include の順によって壊れることがないように、マクロそのものをなくす。このリポジトリでは Q_OBJECT も signals: も
  # slots: も emit も使っていない (ドックは QObject::connect にラムダを渡す)。
  # target_compile_definitions は、呼んだ場所に関係なくターゲット全体に適用されるので、Qt を使うこの .cmake に置く。
  # src-obs/CMakeLists.txt には include の行だけを置く。
  target_compile_definitions(${target} PRIVATE QT_NO_KEYWORDS)

  cordmux_add_dock_face(${target})

  target_sources(
    ${target}
    PRIVATE
      "${CORDMUX_DOCK_DIR}/dock_bind.h"
      "${CORDMUX_DOCK_DIR}/dock_bind.cpp"
      "${CORDMUX_DOCK_DIR}/dock_poll.h"
      "${CORDMUX_DOCK_DIR}/dock_poll.cpp"
      # DockPoll の実装を分けたファイル (dock_poll.h)。どれも Qt を含む。
      "${CORDMUX_DOCK_DIR}/dock_poll_internal.h"
      "${CORDMUX_DOCK_DIR}/dock_poll_layout.h"
      "${CORDMUX_DOCK_DIR}/dock_poll_layout.cpp"
      "${CORDMUX_DOCK_DIR}/dock_poll_install.h"
      "${CORDMUX_DOCK_DIR}/dock_poll_install.cpp"
      "${CORDMUX_DOCK_DIR}/dock_poll_settings.h"
      "${CORDMUX_DOCK_DIR}/dock_poll_settings.cpp"
      "${CORDMUX_DOCK_DIR}/dock_poll_export.h"
      "${CORDMUX_DOCK_DIR}/dock_poll_export.cpp"
  )

  # 部分ごとの .cmake の、Qt を含む側。
  if(COMMAND cordmux_add_dock_ui)
    cordmux_add_dock_ui(${target})
  endif()
  if(COMMAND cordmux_add_rows)
    cordmux_add_rows(${target})
  endif()
  if(COMMAND cordmux_add_windows)
    cordmux_add_windows(${target})
  endif()
  if(COMMAND cordmux_add_install_ui)
    cordmux_add_install_ui(${target})
  endif()
endfunction()
