// SPDX-License-Identifier: Apache-2.0

// 合成のソース cordmux の枠の背景 (映像を描かない所の色) の値と、描くかどうかと画素の値を決める純粋関数。libobs にも
// Qt にも依存しないので、設定から背景の値を作る処理 (decor_params.h の composite_background_from_settings) と、描く判断を
// テストできる。描く関数は composite_decor.h の CompositeDecor::draw_background にある。
// 色は 0xAARRGGBB。

#pragma once

#include <cstdint>

namespace cordmux::sources {

// 枠の背景。キャンバス全体に、ほかの何よりも下に敷く 1 枚の矩形の色。映像は不透明なので、結果として映像のない所
// (空いた枠、枠の間隔、映像の縦横の比が枠と違うときの余り、誰も載っていないときのキャンバス全体) にだけ見える。
struct CompositeBackground {
	bool show = true;                 // 既定は描く (設定の slot_background_show の既定値)
	std::uint32_t argb = 0xFF000000u; // 黒、不透明度 100%
};

inline bool operator==(const CompositeBackground &a, const CompositeBackground &b)
{
	return a.show == b.show && a.argb == b.argb;
}

inline bool operator!=(const CompositeBackground &a, const CompositeBackground &b)
{
	return !(a == b);
}

// 描くか。表示を切っているときと、不透明度が 0 のときは、何も描かない (描画の処理も呼ばない)。
inline bool composite_background_drawn(const CompositeBackground &background)
{
	return background.show && (background.argb >> 24) != 0;
}

// 乗算済みアルファの画素の値 (0xAARRGGBB。リトルエンディアンの環境では B、G、R、A の順に並び、GS_BGRA のテクスチャに
// そのまま渡せる)。名前ラベルの背景は QPainter が乗算済みアルファの QImage に描くので、その式 (Qt の qt_div_255。
// 色にアルファを掛けて 255 で割り、四捨五入) と同じ式にして、同じ色と不透明度なら同じ画素の値になるようにする。
inline std::uint32_t composite_background_premultiplied(std::uint32_t argb)
{
	const std::uint32_t alpha = argb >> 24;
	const auto channel = [alpha](std::uint32_t value) {
		const std::uint32_t x = value * alpha;
		return (x + (x >> 8) + 0x80u) >> 8;
	};
	const std::uint32_t r = channel((argb >> 16) & 0xFFu);
	const std::uint32_t g = channel((argb >> 8) & 0xFFu);
	const std::uint32_t b = channel(argb & 0xFFu);
	return (alpha << 24) | (r << 16) | (g << 8) | b;
}

} // namespace cordmux::sources
