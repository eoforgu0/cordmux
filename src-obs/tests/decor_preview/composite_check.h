// SPDX-License-Identifier: Apache-2.0

// OBS の中で合成のソースを撮った画像とピクセルで比べるための、CPU の参照の画像を描く (--composite-check)。設定は
// settings.json を製品と同じ関数で読み、配置、名前ラベル、光、話していない人の暗さ、画面共有していない参加者の並び、
// 置く物の計画と CPU で重ねる処理も製品の関数をそのまま使う (別の式で描き直さない)。OBS の画像ソースに入れる背景の PNG (--composite-check-background) と、アバターの
// PNG (--composite-check-avatar) も書く。要求の JSON の形と出力は tests/decor_preview/README.md の
// 「OBS の中の画像と比べる参照」にある。

#pragma once

#include <QStringList>

namespace cordmux::decor_preview {

// args は QGuiApplication::arguments()。--composite-check の要求の JSON と --out を読み、参照の画像と JSON を書いて、
// 終了コード (成功は 0、引数・要求・設定の誤りは 2、書き出せなければ 1) を返す。
int run_composite_check(const QStringList &args);

// args は QGuiApplication::arguments()。--composite-check-background <種類> <幅>x<高さ> <PNG> を読み、見本の背景
// (composite_scene.h の make_scene_background) を PNG に書いて、終了コード (run_composite_check と同じ) を返す。
int run_composite_check_background(const QStringList &args);

// args は QGuiApplication::arguments()。--composite-check-avatar <色相> <PNG> を読み、見本のアバター
// (composite_scene.h の make_scene_avatar) を PNG に書いて、終了コード (run_composite_check と同じ) を返す。
int run_composite_check_avatar(const QStringList &args);

} // namespace cordmux::decor_preview
