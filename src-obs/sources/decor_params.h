// SPDX-License-Identifier: Apache-2.0

// 名前ラベルと発言中の光の見た目の値と、設定からその値を作る純粋関数。見た目の値は cordmux 全体で 1 つで、
// cordmux の設定に置く。libobs にも Qt にも依存しないので、設定から見た目の値を作る処理をテストできる。表へ値を渡す
// 関数 (set_decor_label_params) は sources/decor_registry.h にある。

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "config/settings.h"
#include "sources/composite_background.h"
#include "sources/composite_label_look.h"
#include "sources/composite_roster.h"

namespace cordmux::sources {

// 名前ラベルの em の既定値 (配信の画面の高さに対する割合)。設定の label_em_ratio_pct の既定値と同じ。
inline constexpr double kDefaultLabelEmRatio = 0.025;

// 名前ラベルと発言中の光の見た目。色は 0xAARRGGBB。
struct DecorLabelParams {
	double em_ratio = kDefaultLabelEmRatio;                   // 設定の label_em_ratio_pct / 100
	bool show_name = true, show_avatar = true, show_speaking = true; // 名前、アバター、話している人の光を表示するか
	// 話していない人の名前ラベルを暗くする度合い (%。設定の label_dim_pct)。0 なら暗くしない。
	int dim_pct = 30;
	std::uint32_t plate_argb = 0x80000000u;                   // 背景の色と不透明度
	std::uint32_t text_argb = 0xFFFFFFFFu;                    // 文字の色
	// 話している人の光に使う明るい線の色。既定の色は相対輝度が 0.40 以上で、どの明るさの映像の上でも、暗い線か明るい線の
	// どちらかが 3:1 以上のコントラストになる。利用者が選んだ色には、この下限を適用しない。
	std::uint32_t border_light_argb = 0xFF57F287u;
	int border_rise_ms = 150, border_hold_ms = 200, border_fade_ms = 150; // label_border.h の BorderTiming
	int min_visible_graphemes = 2;                            // 省略した名前で見える文字 (書記素) の最小の数
	// フォントの名前。空なら decor_paint.h の kDefaultLabelFontFamilies を使う。フォントと太さは、設定の
	// ドックの [表示] タブで利用者が変えられる。
	std::string font_family;
	int font_weight = 700;
	// 斜体は利用者に選ばせないので、いつも false にする。
	bool font_italic = false;
};

namespace detail {

// `#RRGGBB` を 0xRRGGBB として読む。形が違えば fallback を返す。設定を読む config::detail::read_color が形を
// 確認してから値を入れるので、崩れた値はここに来ない。来ても既定の色で描画する。
inline std::uint32_t rgb_of_hex(const std::string &hex, std::uint32_t fallback)
{
	if (hex.size() != 7 || hex[0] != '#')
		return fallback;
	std::uint32_t value = 0;
	for (std::size_t i = 1; i < 7; ++i) {
		const char c = hex[i];
		std::uint32_t digit = 0;
		if (c >= '0' && c <= '9')
			digit = std::uint32_t(c - '0');
		else if (c >= 'a' && c <= 'f')
			digit = std::uint32_t(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			digit = std::uint32_t(c - 'A' + 10);
		else
			return fallback;
		value = (value << 4) | digit;
	}
	return value;
}

// 0 から 100 の % を、0 から 255 のアルファ値にする (四捨五入)。範囲の外の値は、範囲の端の値にする。
inline std::uint32_t alpha_of_pct(int pct)
{
	const int clamped = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
	return std::uint32_t(std::lround(clamped * 255.0 / 100.0));
}

} // namespace detail

// 設定の値から見た目の値を作る (起動したときと、設定を変えたたびに呼ぶ)。値は丸めない。フォントの太さは、
// フォントを選ぶダイアログが返した値 (350、950 など) のまま渡す (描画する側の decor_paint.cpp が 1 から 1000 に
// 収めて QFont に渡す)。割合は % を 1/100 にするだけである。文字の色は設定ファイルだけで変えられる値で、
// 設定ウィンドウには表示しない。不透明度は背景の色にだけかける。
inline DecorLabelParams decor_label_params_from_settings(const config::Settings &s)
{
	DecorLabelParams p;
	p.em_ratio = s.label_em_ratio_pct / 100.0;
	p.show_name = s.label_show_name;
	p.show_avatar = s.label_show_avatar;
	p.show_speaking = s.label_show_speaking;
	p.dim_pct = s.label_dim_pct;
	p.plate_argb = (detail::alpha_of_pct(s.label_plate_opacity_pct) << 24) |
		       detail::rgb_of_hex(s.label_plate_color, 0x000000u);
	p.text_argb = 0xFF000000u | detail::rgb_of_hex(s.label_text_color, 0xFFFFFFu);
	p.border_light_argb = 0xFF000000u | detail::rgb_of_hex(s.label_border_color, 0x57F287u);
	p.border_rise_ms = s.label_border_rise_ms;
	p.border_hold_ms = s.label_border_hold_ms;
	p.border_fade_ms = s.label_border_fade_ms;
	p.font_family = s.label_font_family;
	p.font_weight = s.label_font_weight;
	return p;
}

// 設定の値から、合成のソースの名前ラベルとグローの見た目を作る (decor_label_params_from_settings と同じ時期に
// 呼ぶ)。割合の % は 1/100 にするだけで、丸めない。文字の色と縁取りの色の不透明度は 100% で、不透明度は名前ラベル全体
// (label_opacity_pct) と背景 (label_plate_opacity_pct) の 2 つだけが持つ。グローの色は話している人の光の色
// (label_border_color) と同じ。
inline CompositeLabelLook composite_label_look_from_settings(const config::Settings &s)
{
	CompositeLabelLook look;
	look.font_family = s.label_font_family;
	look.font_weight = s.label_font_weight;
	look.text_argb = 0xFF000000u | detail::rgb_of_hex(s.label_text_color, 0xFFFFFFu);
	look.outline_argb = 0xFF000000u | detail::rgb_of_hex(s.label_outline_color, 0x000000u);
	look.outline_ratio = s.label_outline_ratio;
	look.show_outline = s.label_show_outline;
	look.show_name = s.label_show_name;
	look.show_avatar = s.label_show_avatar;
	look.avatar_side = s.label_avatar_side;
	look.avatar_shape = s.label_avatar_shape;
	look.avatar_ratio = s.label_avatar_ratio;
	look.show_plate = s.label_show_plate;
	look.plate_argb = (detail::alpha_of_pct(s.label_plate_opacity_pct) << 24) |
			  detail::rgb_of_hex(s.label_plate_color, 0x000000u);
	look.opacity = std::clamp(s.label_opacity_pct, 0, 100) / 100.0;
	look.glow_argb = 0xFF000000u | detail::rgb_of_hex(s.label_border_color, 0x57F287u);
	look.glow_frame = s.label_glow_frame;
	look.glow_avatar = s.label_glow_avatar;
	look.glow_name = s.label_glow_name;
	look.glow_blur_ratio = s.label_glow_blur_ratio;
	look.glow_spread_ratio = s.label_glow_spread_ratio;
	look.margin_ratio = s.label_margin_pct / 100.0;
	look.hide_when_single = s.label_hide_when_single;
	look.show_in_slot = s.label_show_in_slot;
	return look;
}

// 設定の値から、枠の背景の値を作る (composite_label_look_from_settings と同じ時期に呼ぶ)。色と不透明度は、名前ラベルの
// 背景 (plate_argb) と同じ作り方である。
inline CompositeBackground composite_background_from_settings(const config::Settings &s)
{
	CompositeBackground background;
	background.show = s.slot_background_show;
	background.argb = (detail::alpha_of_pct(s.slot_background_opacity_pct) << 24) |
			  detail::rgb_of_hex(s.slot_background_color, 0x000000u);
	return background;
}

// 設定の値から、画面共有していない参加者の並びの設定を作る。位置と合わない向きは、位置に合う向きにして渡す
// (設定を読むときに修正してあるが、設定を直接作る呼び出し元にも同じ規則を守らせる)。
inline CompositeRosterParams composite_roster_params_from_settings(const config::Settings &s)
{
	CompositeRosterParams params;
	params.show = s.roster_show;
	params.anchor = s.roster_anchor;
	params.direction = config::roster_direction_for(s.roster_anchor, s.roster_direction);
	params.avatar_side = s.roster_avatar_side;
	params.gap_ratio = s.roster_gap_ratio;
	params.name_max_em = s.roster_name_max_em;
	params.offset_x_px = s.roster_offset_x_px;
	params.offset_y_px = s.roster_offset_y_px;
	return params;
}

// 枠の名前ラベルと話している人の光に使う人の情報 (名前とアバターなど)。枠の名前ラベルに描く人 (owner) の情報だけを
// 使い、owner が空か、owner の情報がまだ届いていなければ nullptr を返す (描画しない)。前の人の情報や、枠に結び
// 付けた古い情報で代用しないので、owner が替わった最初のフレームから、前の人の名前は表示されない。people は
// userId から情報への表 (std::map と同じ find を持つ型)。
template <typename PeopleMap>
const typename PeopleMap::mapped_type *owner_material(const PeopleMap &people, const std::string &owner)
{
	if (owner.empty())
		return nullptr;
	const auto found = people.find(owner);
	return found == people.end() ? nullptr : &found->second;
}

} // namespace cordmux::sources
