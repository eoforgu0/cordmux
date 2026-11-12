// SPDX-License-Identifier: Apache-2.0

// 合成のソース cordmux の名前ラベルと、話している人を示すグローの見本 (--composite-sample) と、つまみごとの見本の
// 一覧 (--composite-sweep)。製品の配置 (sources/composite_layout.h) で自分の枠と 3 人を 2 x 2 に並べ、背景の画像の上に、
// 製品の描画 (sources/composite_label.h) と置く物の計画 (sources/composite_draw_plan.h) のとおりに CPU で重ねた
// (sources/composite_draw_cpu.h) PNG を書き出す。場面を作る部分は composite_scene.h にある。使い方は
// tests/decor_preview/README.md。

#pragma once

#include <QStringList>

namespace cordmux::decor_preview {

// args は QGuiApplication::arguments() (先頭は実行ファイルの名前)。--composite-sample のほかの引数を読み、PNG を書き
// 出して、終了コード (成功は 0、引数の誤りは 2、書き出せなければ 1) を返す。引数は README.md の「合成のソースの見本」に
// ある。
int run_composite_sample(const QStringList &args);

// args は QGuiApplication::arguments()。--composite-sweep の引数 (--out DIR と、1 つだけ描くときの --knob NAME) を読み、
// つまみごとに 1 枚の一覧の PNG を書き出して、終了コード (run_composite_sample と同じ) を返す。引数とつまみの名前は
// README.md の「つまみごとの見本の一覧」にある。
int run_composite_sweep(const QStringList &args);

} // namespace cordmux::decor_preview
