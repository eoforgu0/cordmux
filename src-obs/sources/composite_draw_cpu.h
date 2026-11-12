// SPDX-License-Identifier: Apache-2.0

// 合成のソースが置く物の計画 (sources/composite_draw_plan.h) を、CPU で QImage に重ねる。GPU で置く側
// (sources/composite_decor.cpp) と同じ画像を、同じ順、同じ切り抜き、同じ混ぜ方の式で重ねるので、テストと
// プレビューツールは、製品が描く値を GPU なしで確認できる。libobs に依存しない。
//
// GPU と同じ値になる理由: GPU の側は、画像を 8 ビットのテクスチャにして等倍で整数の位置に置くので、ピクセルの中心が
// テクセルの中心に重なり、読む値はテクセルの値そのもの (n / 255) になる。効果は 4 つのチャンネルに明るさ level を
// 掛けるだけで、混ぜ方は GS_BLEND_ONE と GS_BLEND_INVSRCALPHA (色もアルファも「元 + 下 x (1 - 元のアルファ)」)。
// ここでも同じ式を倍精度で計算し、最後に 8 ビットへ四捨五入する。違いは丸めの 1 段階だけである。どちらも sRGB の
// 変換をせず、値のまま重ねる。キャンバスが 8 ビットでない
// (HDR などの) 色の形式での値は確認していない。

#pragma once

#include <vector>

#include <QImage>

#include "sources/composite_draw_plan.h"

namespace cordmux::sources {

// タイル 1 つ分の画像。CompositeDrawTile の label、glow、frame と同じ画像 (paint_composite_label と
// paint_frame_glow が返した乗算済みアルファの画像) を入れる。
struct CompositeTileImages {
	QImage label;
	QImage glow;
	QImage frame;
};

// canvas に draws を順に重ねる。canvas は QImage::Format_ARGB32_Premultiplied で、キャンバスの大きさ
// (CompositeDrawRequest の canvas_w x canvas_h) であること。images[i] は draws の tile が i の物の画像で、足りない
// タイルと空の画像は置かない。画像の大きさは、計画を作ったときの CompositeDrawImage の大きさと同じであること。
// roster_images[i] は、画面共有していない参加者 (RosterGlow と RosterLabel) の、CompositeDrawRequest::roster の i 番目の
// 画像で、label と glow を使う。画面共有していない参加者がいなければ渡さなくてよい。
void draw_composite_plan(QImage *canvas, const std::vector<CompositeDraw> &draws,
			 const std::vector<CompositeTileImages> &images,
			 const std::vector<CompositeTileImages> &roster_images = {});

} // namespace cordmux::sources
