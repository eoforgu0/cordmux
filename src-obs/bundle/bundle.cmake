# SPDX-License-Identifier: Apache-2.0

# 診断情報 (src-obs/bundle) をターゲットに追加する関数 cordmux_add_bundle(<target>) を定義する。src-obs/CMakeLists.txt が
# include して呼び出す。コンパイルするのは、Qt を使う export_bundle.cpp だけである。bundle_manifest.h、zip_writer.h、
# bundle_files.h はヘッダーだけで、src-obs/tests の単体テストは、このファイルを使わずにそのままインクルードする。
# Qt6::Widgets は src-obs/CMakeLists.txt がリンクし、QT_NO_KEYWORDS は src-obs/dock/dock.cmake がターゲット全体に定義する。

set(CORDMUX_BUNDLE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(cordmux_add_bundle target)
  target_sources(
    ${target}
    PRIVATE
      "${CORDMUX_BUNDLE_DIR}/zip_writer.h"
      "${CORDMUX_BUNDLE_DIR}/bundle_manifest.h"
      "${CORDMUX_BUNDLE_DIR}/bundle_files.h"
      "${CORDMUX_BUNDLE_DIR}/export_bundle.h"
      "${CORDMUX_BUNDLE_DIR}/export_bundle.cpp"
  )
endfunction()
