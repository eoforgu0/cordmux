// SPDX-License-Identifier: Apache-2.0

// 合成のソース cordmux の名前ラベルと、話している人を示すグローの見た目の値。libobs にも Qt にも依存しないので、
// 設定から見た目の値を作る処理 (decor_params.h の composite_label_look_from_settings) をテストできる。
// 描画する関数は composite_label.h にある。色は 0xAARRGGBB で、長さは断りがなければ名前の大きさ (em) に対する倍率。

#pragma once

#include <cstdint>
#include <string>

#include "config/label_placement.h"

namespace cordmux::sources {

// 名前ラベルの見た目。
struct CompositeLabelLook {
	// ---- 文字 ----
	std::string font_family;      // フォントの名前 (UTF-8)。空なら kDefaultLabelFontFamilies の候補
	int font_weight = 700;
	bool font_italic = false;
	std::uint32_t text_argb = 0xFFFFFFFFu;    // 文字の色
	std::uint32_t outline_argb = 0xFF000000u; // 縁取りの色
	// 縁取りの線の太さ。線は文字の輪郭を中心に引き、その上に文字を重ねるので、見える縁はこの半分になる。
	double outline_ratio = 0.1;
	// false なら縁取りを描かない (太さ 0 と同じ見た目で、縁取りの分の余白も取らない)。
	bool show_outline = true;
	bool show_name = true;

	// ---- アバター ----
	bool show_avatar = true;
	// アバターを置く側。右にすると、名前ラベルの中の並びと背景の内側の余白が左右入れ替わる (アバターが右端、名前がその左)。
	// 設定では、画面共有している参加者 (label_avatar_side) と並び (roster_avatar_side) で別の値なので、呼び出し元が
	// 描く相手に合わせて入れる (composite_label_look_from_settings は前者を入れる)。
	config::AvatarSide avatar_side = config::AvatarSide::Left;
	double avatar_ratio = 1.5;    // アバターの直径 (丸でないときは一辺)
	config::AvatarShape avatar_shape = config::AvatarShape::Circle; // アバターの形。頭文字の形と縁取りも同じ形にする
	double avatar_gap_em = 0.38;  // アバターと名前の間
	// アバターの画像がないときに描く、名前の頭文字の背景の色。人によって変えない色にしてあるのは、参加者ごとに色を決める
	// には人を区別する値 (ユーザー ID) が必要で、名前から決めると、名前を変えたときに色も変わるため。白い頭文字との
	// コントラスト比は約 7:1。
	std::uint32_t initial_bg_argb = 0xFF4A5A6Au;
	double initial_em_ratio = 0.44; // 頭文字の大きさ (アバターの直径に対する倍率)

	// ---- 背景 ----
	bool show_plate = true;
	std::uint32_t plate_argb = 0x80000000u; // 黒、不透明度 50%
	double line_em = 1.2;          // 文字の行の高さ。背景がなくても、名前ラベルの高さの下限になる
	double plate_pad_v_em = 0.15;  // 背景の上下の余白 (行の高さ 1.2 em と合わせて 1.5 em になる)
	double plate_pad_right_em = 0.5;
	double plate_pad_left_em = 0.5;  // アバターがないときの左の余白。あるときは plate_pad_v_em と同じ
	double plate_radius_ratio = 0.25; // 角の丸み (背景の高さに対する倍率)

	// ---- 長い名前の扱い ----
	double min_name_em = 6.0;             // 名前に取る最小の幅。これが入らない幅では、アバターを省く
	bool drop_avatar_when_narrow = true;
	int min_visible_graphemes = 2;        // 省略した名前で見える文字 (書記素) がこれより少なければ描画しない
	// 名前の幅の上限 (名前の大きさに対する倍率)。0 なら上限なし (セルの幅だけで決まる)。画面共有していない参加者の並びの
	// 名前ラベルにだけ、呼び出し元が設定の roster_name_max_em を入れる (composite_label_look_from_settings は 0 のまま)。
	double max_name_em = 0.0;

	// ---- 全体 ----
	double opacity = 1.0; // 名前ラベル全体の不透明度。グローと名前ラベルをまとめた全体に 1 度だけ掛かるように描く

	// ---- 話している人を示すグロー ----
	std::uint32_t glow_argb = 0xFF57F287u;
	bool glow_frame = false;   // 枠を光らせる (paint_frame_glow)
	bool glow_avatar = true;
	bool glow_name = true;
	double glow_blur_ratio = 0.10;   // ぼかしの大きさ (標準偏差はその半分)
	double glow_spread_ratio = 0.10; // 広がり

	// ---- 描画する側が読む値 (paint_composite_label は読まない) ----
	double margin_ratio = 0.005;   // 枠の隅からの余白 (配信の画面の高さに対する割合)
	bool hide_when_single = true;  // 合成が全面に 1 枚だけ置いているとき、名前ラベルを描かない
	// 画面共有の枠 (と自分の枠) に名前ラベルを描くか。偽なら枠には名前ラベルも名前ラベルの光も描かず、その人を画面共有していない
	// 参加者の並びに描く。枠の光は描く (composite_label_plan.h の composite_tiles_labeled と composite_tiles_framed)。
	bool show_in_slot = true;
};

// 見た目の値が同じか。値が変わったときだけ、描き直す鍵の version を進めるために使う。
inline bool operator==(const CompositeLabelLook &a, const CompositeLabelLook &b)
{
	return a.font_family == b.font_family && a.font_weight == b.font_weight && a.font_italic == b.font_italic &&
	       a.text_argb == b.text_argb && a.outline_argb == b.outline_argb && a.outline_ratio == b.outline_ratio &&
	       a.show_outline == b.show_outline &&
	       a.show_name == b.show_name && a.show_avatar == b.show_avatar && a.avatar_side == b.avatar_side &&
	       a.avatar_ratio == b.avatar_ratio && a.avatar_shape == b.avatar_shape &&
	       a.avatar_gap_em == b.avatar_gap_em && a.initial_bg_argb == b.initial_bg_argb &&
	       a.initial_em_ratio == b.initial_em_ratio && a.show_plate == b.show_plate &&
	       a.plate_argb == b.plate_argb && a.line_em == b.line_em && a.plate_pad_v_em == b.plate_pad_v_em &&
	       a.plate_pad_right_em == b.plate_pad_right_em && a.plate_pad_left_em == b.plate_pad_left_em &&
	       a.plate_radius_ratio == b.plate_radius_ratio && a.min_name_em == b.min_name_em &&
	       a.drop_avatar_when_narrow == b.drop_avatar_when_narrow &&
	       a.min_visible_graphemes == b.min_visible_graphemes && a.max_name_em == b.max_name_em &&
	       a.opacity == b.opacity &&
	       a.glow_argb == b.glow_argb && a.glow_frame == b.glow_frame && a.glow_avatar == b.glow_avatar &&
	       a.glow_name == b.glow_name && a.glow_blur_ratio == b.glow_blur_ratio &&
	       a.glow_spread_ratio == b.glow_spread_ratio && a.margin_ratio == b.margin_ratio &&
	       a.hide_when_single == b.hide_when_single && a.show_in_slot == b.show_in_slot;
}

inline bool operator!=(const CompositeLabelLook &a, const CompositeLabelLook &b)
{
	return !(a == b);
}

} // namespace cordmux::sources
