// SPDX-License-Identifier: Apache-2.0

// 設定から合成のソースの名前ラベルとグローの見た目を作る関数 (sources/decor_params.h の
// composite_label_look_from_settings) のテスト。

#include "doctest.h"

#include "config/settings.h"
#include "sources/composite_label_look.h"
#include "sources/decor_params.h"

using cordmux::config::Settings;
using cordmux::sources::CompositeLabelLook;
using cordmux::sources::composite_label_look_from_settings;

TEST_CASE("既定の設定から作った合成のソースの名前ラベルの見た目は、見た目の値の既定値と同じになる")
{
	const CompositeLabelLook from_settings = composite_label_look_from_settings(Settings{});
	const CompositeLabelLook defaults;
	CHECK(from_settings.font_family == defaults.font_family);
	CHECK(from_settings.font_weight == defaults.font_weight);
	CHECK(from_settings.text_argb == defaults.text_argb);
	CHECK(from_settings.outline_argb == defaults.outline_argb);
	CHECK(from_settings.outline_ratio == doctest::Approx(defaults.outline_ratio));
	CHECK(from_settings.show_name == defaults.show_name);
	CHECK(from_settings.show_avatar == defaults.show_avatar);
	CHECK(from_settings.avatar_ratio == doctest::Approx(defaults.avatar_ratio));
	CHECK(from_settings.show_plate == defaults.show_plate);
	CHECK(from_settings.plate_argb == defaults.plate_argb);
	CHECK(from_settings.opacity == doctest::Approx(defaults.opacity));
	CHECK(from_settings.glow_argb == defaults.glow_argb);
	CHECK(from_settings.glow_frame == defaults.glow_frame);
	CHECK(from_settings.glow_avatar == defaults.glow_avatar);
	CHECK(from_settings.glow_name == defaults.glow_name);
	CHECK(from_settings.glow_blur_ratio == doctest::Approx(defaults.glow_blur_ratio));
	CHECK(from_settings.glow_spread_ratio == doctest::Approx(defaults.glow_spread_ratio));
	CHECK(from_settings.margin_ratio == doctest::Approx(defaults.margin_ratio).epsilon(0.01));
	CHECK(from_settings.hide_when_single == defaults.hide_when_single);
}

TEST_CASE("変えた設定は、合成のソースの名前ラベルの見た目の対応する値に入る")
{
	Settings s;
	s.label_font_family = "Meiryo UI";
	s.label_font_weight = 350;
	s.label_text_color = "#102030";
	s.label_outline_color = "#A0B0C0";
	s.label_outline_ratio = 0.2;
	s.label_show_outline = false;
	s.label_show_name = false;
	s.label_show_avatar = false;
	s.label_avatar_ratio = 2.5;
	s.label_show_plate = true;
	s.label_plate_color = "#112233";
	s.label_plate_opacity_pct = 40;
	s.label_opacity_pct = 60;
	s.label_border_color = "#FF8800";
	s.label_glow_frame = false;
	s.label_glow_avatar = false;
	s.label_glow_name = false;
	s.label_glow_blur_ratio = 1.5;
	s.label_glow_spread_ratio = 0.3;
	s.label_margin_pct = 5.0;
	s.label_hide_when_single = false;

	const CompositeLabelLook look = composite_label_look_from_settings(s);
	CHECK(look.font_family == "Meiryo UI");
	CHECK(look.font_weight == 350);
	CHECK(look.text_argb == 0xFF102030u);
	CHECK(look.outline_argb == 0xFFA0B0C0u);
	CHECK(look.outline_ratio == doctest::Approx(0.2));
	CHECK_FALSE(look.show_outline);
	CHECK(composite_label_look_from_settings(Settings{}).show_outline);
	CHECK_FALSE(look.show_name);
	CHECK_FALSE(look.show_avatar);
	CHECK(look.avatar_ratio == doctest::Approx(2.5));
	CHECK(look.show_plate);
	// 背景の不透明度 40% は 255 * 0.4 = 102。
	CHECK(look.plate_argb == 0x66112233u);
	CHECK(look.opacity == doctest::Approx(0.6));
	CHECK(look.glow_argb == 0xFFFF8800u);
	CHECK_FALSE(look.glow_frame);
	CHECK_FALSE(look.glow_avatar);
	CHECK_FALSE(look.glow_name);
	CHECK(look.glow_blur_ratio == doctest::Approx(1.5));
	CHECK(look.glow_spread_ratio == doctest::Approx(0.3));
	CHECK(look.margin_ratio == doctest::Approx(0.05));
	CHECK_FALSE(look.hide_when_single);
}

TEST_CASE("既定の設定から作った枠の背景は、不透明な黒で描く")
{
	using cordmux::sources::CompositeBackground;
	using cordmux::sources::composite_background_drawn;
	using cordmux::sources::composite_background_from_settings;
	const CompositeBackground from_settings = composite_background_from_settings(Settings{});
	const CompositeBackground defaults;
	CHECK(from_settings.show);
	CHECK(from_settings == defaults);
	CHECK(from_settings.argb == 0xFF000000u);
	CHECK(composite_background_drawn(from_settings));
}

TEST_CASE("変えた設定は、枠の背景の表示、色、不透明度に入り、色と不透明度は名前ラベルの背景と同じ作り方になる")
{
	using cordmux::sources::composite_background_from_settings;
	Settings s;
	s.slot_background_show = true;
	s.slot_background_color = "#112233";
	s.slot_background_opacity_pct = 40;
	const auto background = composite_background_from_settings(s);
	CHECK(background.show);
	// 不透明度 40% は 255 * 0.4 = 102 (0x66)。
	CHECK(background.argb == 0x66112233u);
	// 同じ色と不透明度の名前ラベルの背景と、同じ 0xAARRGGBB になる。
	s.label_plate_color = "#112233";
	s.label_plate_opacity_pct = 40;
	CHECK(background.argb == composite_label_look_from_settings(s).plate_argb);
	// 範囲の端。
	s.slot_background_opacity_pct = 0;
	CHECK((composite_background_from_settings(s).argb >> 24) == 0u);
	s.slot_background_opacity_pct = 100;
	CHECK((composite_background_from_settings(s).argb >> 24) == 0xFFu);
	// 表示を切った設定は、描かない値になる。
	s.slot_background_show = false;
	CHECK_FALSE(composite_background_from_settings(s).show);
}

TEST_CASE("枠の背景は、表示していて不透明度が 0 でないときだけ描く")
{
	using cordmux::sources::CompositeBackground;
	using cordmux::sources::composite_background_drawn;
	CompositeBackground b;
	b.argb = 0x80102030u;
	b.show = false;
	CHECK_FALSE(composite_background_drawn(b));
	b.show = true;
	CHECK(composite_background_drawn(b));
	b.argb = 0x01102030u;
	CHECK(composite_background_drawn(b));
	b.argb = 0x00102030u;
	CHECK_FALSE(composite_background_drawn(b));
	b.show = false;
	CHECK_FALSE(composite_background_drawn(b));
}

TEST_CASE("枠の背景の画素は、乗算済みアルファの 0xAARRGGBB にする (Qt の乗算済みアルファの式と同じ)")
{
	using cordmux::sources::composite_background_premultiplied;
	// 不透明は色のまま、完全な透明は 0。
	CHECK(composite_background_premultiplied(0xFF102030u) == 0xFF102030u);
	CHECK(composite_background_premultiplied(0xFFFFFFFFu) == 0xFFFFFFFFu);
	CHECK(composite_background_premultiplied(0x00FFFFFFu) == 0x00000000u);
	// 50% (0x80) の白は、各チャンネルが 0x80。
	CHECK(composite_background_premultiplied(0x80FFFFFFu) == 0x80808080u);
	// 黒は、不透明度によらず色が 0。
	CHECK(composite_background_premultiplied(0x66000000u) == 0x66000000u);
	// 40% (0x66) の #112233: 色 x 102 / 255 を四捨五入 (7、14、20)。
	CHECK(composite_background_premultiplied(0x66112233u) == 0x66070E14u);
	// どの不透明度でも、色のチャンネルはアルファを超えない (乗算済みアルファの約束)。
	for (std::uint32_t alpha = 0; alpha <= 255; ++alpha) {
		const std::uint32_t p = composite_background_premultiplied((alpha << 24) | 0x00FFFFFFu);
		CHECK(((p >> 16) & 0xFFu) <= alpha);
		CHECK(((p >> 8) & 0xFFu) <= alpha);
		CHECK((p & 0xFFu) <= alpha);
		CHECK((p >> 24) == alpha);
	}
}

TEST_CASE("範囲の端の設定でも、合成のソースの名前ラベルの見た目は不透明度を 0 から 1 に収める")
{
	Settings s;
	s.label_opacity_pct = 0;
	CHECK(composite_label_look_from_settings(s).opacity == 0.0);
	s.label_opacity_pct = 100;
	CHECK(composite_label_look_from_settings(s).opacity == 1.0);
	// 範囲の外の値 (読む処理は通さない) でも、不透明度は 0 から 1 のまま。
	s.label_opacity_pct = 250;
	CHECK(composite_label_look_from_settings(s).opacity == 1.0);
	s.label_opacity_pct = -3;
	CHECK(composite_label_look_from_settings(s).opacity == 0.0);
}

TEST_CASE("アバターの位置、暗さ、画面共有していない参加者の並びの設定は、見た目の値と並びの設定に入る")
{
	using cordmux::config::AvatarSide;
	using cordmux::config::RosterAnchor;
	using cordmux::config::RosterDirection;
	using cordmux::sources::composite_roster_params_from_settings;
	using cordmux::sources::decor_label_params_from_settings;

	// 既定は、画面共有している参加者の名前ラベルが左下でアバター左、暗さ 30、並びが表示で右下の縦並びでアバター右、間隔 0.25。
	const CompositeLabelLook defaults = composite_label_look_from_settings(Settings{});
	CHECK(defaults.avatar_side == AvatarSide::Left);
	CHECK(decor_label_params_from_settings(Settings{}).dim_pct == 30);
	const auto roster_defaults = composite_roster_params_from_settings(Settings{});
	CHECK(roster_defaults.show);
	CHECK(roster_defaults.anchor == RosterAnchor::BottomRight);
	CHECK(roster_defaults.direction == RosterDirection::Column);
	CHECK(roster_defaults.avatar_side == AvatarSide::Right);
	CHECK(roster_defaults.gap_ratio == doctest::Approx(0.25));
	CHECK(roster_defaults.name_max_em == doctest::Approx(12.0));
	// 画面共有している参加者の名前ラベルには、名前の最大幅を入れない (並びの名前ラベルにだけ入れる)。
	CHECK(composite_label_look_from_settings(Settings{}).max_name_em == 0.0);
	CHECK(defaults.avatar_shape == cordmux::config::AvatarShape::Circle);

	Settings s;
	s.label_avatar_side = AvatarSide::Right;
	s.label_dim_pct = 65;
	s.roster_show = false;
	s.roster_anchor = RosterAnchor::MiddleRight;
	s.roster_direction = RosterDirection::Column;
	s.roster_avatar_side = AvatarSide::Left;
	s.roster_gap_ratio = 1.25;
	s.roster_name_max_em = 7.5;
	s.roster_offset_x_px = -200;
	s.roster_offset_y_px = -100;
	s.label_avatar_shape = cordmux::config::AvatarShape::Square;
	CHECK(composite_label_look_from_settings(s).avatar_side == AvatarSide::Right);
	CHECK(composite_label_look_from_settings(s).avatar_shape == cordmux::config::AvatarShape::Square);
	CHECK(decor_label_params_from_settings(s).dim_pct == 65);
	const auto roster = composite_roster_params_from_settings(s);
	CHECK_FALSE(roster.show);
	CHECK(roster.anchor == RosterAnchor::MiddleRight);
	CHECK(roster.direction == RosterDirection::Column);
	CHECK(roster.avatar_side == AvatarSide::Left); // 画面共有している参加者の名前ラベルとは別の値
	CHECK(roster.gap_ratio == doctest::Approx(1.25));
	CHECK(roster.name_max_em == doctest::Approx(7.5));
	CHECK(roster.offset_x_px == -200); // キャンバスのピクセルのまま渡す
	CHECK(roster.offset_y_px == -100);

	// 設定を直接作ったときに、位置に合わない向きが入っていても、位置に合う向きにして渡す。
	s.roster_anchor = RosterAnchor::BottomCenter;
	s.roster_direction = RosterDirection::Column;
	CHECK(composite_roster_params_from_settings(s).direction == RosterDirection::Row);
}
