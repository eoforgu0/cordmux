// SPDX-License-Identifier: Apache-2.0

// 表示タブの判断 (ledger/look_tab.h) と View (decide_dock() の DockViewV2::look_tab) の単体テスト。入力欄を押せるか、
// 外から届いた値で入力欄を書き換えるか、変えた値を渡す間隔、ダミー映像の切り替えの押せる条件、先頭の選択肢の文字列。

#include "doctest.h"

#include "config/settings.h"
#include "ledger/look_tab.h"
#include "ledger/rules.h"
#include "ledger/text_ids.h"
#include "ledger_dock_test_helpers.h"

#include <cstdint>
#include <string>

using namespace ledger_dock_test;
using cordmux::config::RosterAnchor;
using cordmux::config::RosterDirection;
using cordmux::config::Settings;
using cordmux::ledger::look_control_enabled;
using cordmux::ledger::look_field_follows;
using cordmux::ledger::look_reflect_wait_ms;
using cordmux::ledger::look_roster_direction_allowed;
using cordmux::ledger::look_self_slot_first_enabled;
using cordmux::ledger::LookControl;
namespace keys = cordmux::ledger::text::keys;

TEST_CASE("渡す間隔: 最後に渡してから間隔がたつまでの残りを返し、たっていれば 0 を返す")
{
	CHECK(look_reflect_wait_ms(1000, 1000, 100) == 100);
	CHECK(look_reflect_wait_ms(1000, 1040, 100) == 60);
	CHECK(look_reflect_wait_ms(1000, 1099, 100) == 1);
	CHECK(look_reflect_wait_ms(1000, 1100, 100) == 0);
	CHECK(look_reflect_wait_ms(1000, 5000, 100) == 0);
	// 時刻が前に戻ったときは、待たせ続けない。
	CHECK(look_reflect_wait_ms(1000, 500, 100) == 0);
	// 間隔が 0 以下なら、いつでも渡す。
	CHECK(look_reflect_wait_ms(1000, 1000, 0) == 0);
	CHECK(look_reflect_wait_ms(1000, 1000, -5) == 0);
}

TEST_CASE("渡す間隔と保存までの時間は、動かしている間に渡す回数を減らし、保存は 1 秒後にまとめる値である")
{
	CHECK(cordmux::ledger::kLookReflectIntervalMs == 100);
	CHECK(cordmux::ledger::kLookSaveDelayMs == 1000);
	CHECK(cordmux::ledger::kLookSaveDelayMs > cordmux::ledger::kLookReflectIntervalMs);
}

TEST_CASE("外から届いた値は、操作している最中の入力欄と、渡していない変更がある間は書き換えない")
{
	CHECK(look_field_follows(false, false, true));
	CHECK_FALSE(look_field_follows(true, false, true));
	CHECK_FALSE(look_field_follows(false, true, true));
	CHECK_FALSE(look_field_follows(true, true, true));
	// 値が同じなら書き換えない。
	CHECK_FALSE(look_field_follows(false, false, false));
}

TEST_CASE("名前のフォント、太さ、サイズ、色と輪郭は、名前を表示するときだけ押せる")
{
	Settings s;
	s.label_show_name = true;
	for (const LookControl c : {LookControl::NameFont, LookControl::NameWeight, LookControl::NameSize,
				    LookControl::NameColor, LookControl::OutlineShow, LookControl::OutlineColor,
				    LookControl::OutlineSize})
		CHECK(look_control_enabled(c, s));
	s.label_show_name = false;
	for (const LookControl c : {LookControl::NameFont, LookControl::NameWeight, LookControl::NameSize,
				    LookControl::NameColor, LookControl::OutlineShow, LookControl::OutlineColor,
				    LookControl::OutlineSize})
		CHECK_FALSE(look_control_enabled(c, s));
}

TEST_CASE("輪郭の色とサイズは、名前と輪郭を表示するときだけ押せ、輪郭の [表示] は名前を表示すれば押せる")
{
	Settings s;
	CHECK(s.label_show_outline);
	s.label_show_name = true;
	s.label_show_outline = false;
	CHECK(look_control_enabled(LookControl::OutlineShow, s));
	CHECK_FALSE(look_control_enabled(LookControl::OutlineColor, s));
	CHECK_FALSE(look_control_enabled(LookControl::OutlineSize, s));
	s.label_show_outline = true;
	CHECK(look_control_enabled(LookControl::OutlineColor, s));
	CHECK(look_control_enabled(LookControl::OutlineSize, s));
	// 値は残る (押せなくするだけ)。
	s.label_show_outline = false;
	CHECK(s.label_outline_ratio == cordmux::config::kDefaultLabelOutlineRatio);
}

TEST_CASE("背景は、名前かアバターを表示するときだけ押せ、色と不透明度は背景を表示するときだけ押せる")
{
	Settings s;
	s.label_show_name = false;
	s.label_show_avatar = false;
	s.label_show_plate = true;
	CHECK_FALSE(look_control_enabled(LookControl::PlateShow, s));
	CHECK_FALSE(look_control_enabled(LookControl::PlateColor, s));
	CHECK_FALSE(look_control_enabled(LookControl::PlateOpacity, s));

	s.label_show_avatar = true;
	CHECK(look_control_enabled(LookControl::PlateShow, s));
	CHECK(look_control_enabled(LookControl::PlateColor, s));
	CHECK(look_control_enabled(LookControl::PlateOpacity, s));

	s.label_show_plate = false;
	CHECK(look_control_enabled(LookControl::PlateShow, s));
	CHECK_FALSE(look_control_enabled(LookControl::PlateColor, s));
	CHECK_FALSE(look_control_enabled(LookControl::PlateOpacity, s));
}

TEST_CASE("枠の背景の色と不透明度は、枠の背景を表示するときだけ押せる (名前ラベルの表示には依らない)")
{
	Settings s;
	CHECK(s.slot_background_show);
	s.slot_background_show = false;
	s.label_show_name = false;
	s.label_show_avatar = false;
	CHECK_FALSE(look_control_enabled(LookControl::SlotBackgroundColor, s));
	CHECK_FALSE(look_control_enabled(LookControl::SlotBackgroundOpacity, s));

	s.slot_background_show = true;
	CHECK(look_control_enabled(LookControl::SlotBackgroundColor, s));
	CHECK(look_control_enabled(LookControl::SlotBackgroundOpacity, s));
	// 名前ラベルの背景の [表示] には影響されない。
	s.label_show_plate = false;
	CHECK(look_control_enabled(LookControl::SlotBackgroundColor, s));
	CHECK(look_control_enabled(LookControl::SlotBackgroundOpacity, s));
	// 値は残る (押せなくするだけ)。
	s.slot_background_show = false;
	CHECK(s.slot_background_opacity_pct == 100);
}

TEST_CASE("アバターのサイズは、アバターを表示するときだけ押せる")
{
	Settings s;
	s.label_show_avatar = false;
	CHECK_FALSE(look_control_enabled(LookControl::AvatarSize, s));
	s.label_show_avatar = true;
	CHECK(look_control_enabled(LookControl::AvatarSize, s));
}

TEST_CASE("光の形は、光を表示するときだけ押せる")
{
	Settings s;
	s.label_show_speaking = true;
	for (const LookControl c : {LookControl::GlowTargets, LookControl::GlowColor, LookControl::GlowThickness,
				    LookControl::GlowBlur})
		CHECK(look_control_enabled(c, s));
	s.label_show_speaking = false;
	for (const LookControl c : {LookControl::GlowTargets, LookControl::GlowColor, LookControl::GlowThickness,
				    LookControl::GlowBlur})
		CHECK_FALSE(look_control_enabled(c, s));
}

TEST_CASE("光の時間の 3 つは、光を表示するか、話していない人を暗くするときに押せる")
{
	Settings s;
	const LookControl timings[] = {LookControl::TimingFadeIn, LookControl::TimingHold, LookControl::TimingFadeOut};
	s.label_show_speaking = false;
	s.label_dim_pct = 0;
	for (const LookControl c : timings)
		CHECK_FALSE(look_control_enabled(c, s));
	// 光を切っても、暗くする動きに適用されるので押せる。
	s.label_dim_pct = 30;
	for (const LookControl c : timings)
		CHECK(look_control_enabled(c, s));
	s.label_dim_pct = 0;
	s.label_show_speaking = true;
	for (const LookControl c : timings)
		CHECK(look_control_enabled(c, s));
}

TEST_CASE("しきい値は、自分の枠の音声ソースを選んだときだけ押せる")
{
	Settings s;
	CHECK_FALSE(look_control_enabled(LookControl::SelfThreshold, s));
	s.self_speaking_source_name = "mic";
	CHECK(look_control_enabled(LookControl::SelfThreshold, s));
	s.self_speaking_source_name.clear();
	s.self_speaking_source_uuid = "11111111-2222-3333-4444-555555555555";
	CHECK(look_control_enabled(LookControl::SelfThreshold, s));
}

TEST_CASE("画面共有していない参加者の並びの入力欄は、並びを表示するときだけ押せる")
{
	Settings s;
	const LookControl rows[] = {LookControl::RosterAnchor, LookControl::RosterOffset, LookControl::RosterDirection,
				    LookControl::RosterGap, LookControl::RosterAvatarSide, LookControl::RosterShowBots};
	s.roster_show = true;
	for (const LookControl c : rows)
		CHECK(look_control_enabled(c, s));
	s.roster_show = false;
	for (const LookControl c : rows)
		CHECK_FALSE(look_control_enabled(c, s));
}

TEST_CASE("アバターのサイズと形は、アバターを表示するときだけ押せる")
{
	Settings s;
	s.label_show_avatar = true;
	CHECK(look_control_enabled(LookControl::AvatarSize, s));
	CHECK(look_control_enabled(LookControl::AvatarShape, s));
	s.label_show_avatar = false;
	CHECK_FALSE(look_control_enabled(LookControl::AvatarSize, s));
	CHECK_FALSE(look_control_enabled(LookControl::AvatarShape, s));
}

TEST_CASE("画面共有していない参加者の名前の最大幅は、並びと名前の両方を表示するときだけ押せる")
{
	Settings s;
	s.roster_show = true;
	s.label_show_name = true;
	CHECK(look_control_enabled(LookControl::RosterNameWidth, s));
	s.label_show_name = false;
	CHECK_FALSE(look_control_enabled(LookControl::RosterNameWidth, s));
	s.label_show_name = true;
	s.roster_show = false;
	CHECK_FALSE(look_control_enabled(LookControl::RosterNameWidth, s));
}

TEST_CASE("並びの向きは、配置に合うものだけを選べる")
{
	CHECK(look_roster_direction_allowed(RosterAnchor::TopCenter, RosterDirection::Row));
	CHECK_FALSE(look_roster_direction_allowed(RosterAnchor::TopCenter, RosterDirection::Column));
	CHECK(look_roster_direction_allowed(RosterAnchor::BottomCenter, RosterDirection::Row));
	CHECK_FALSE(look_roster_direction_allowed(RosterAnchor::BottomCenter, RosterDirection::Column));
	CHECK(look_roster_direction_allowed(RosterAnchor::MiddleLeft, RosterDirection::Column));
	CHECK_FALSE(look_roster_direction_allowed(RosterAnchor::MiddleLeft, RosterDirection::Row));
	CHECK(look_roster_direction_allowed(RosterAnchor::MiddleRight, RosterDirection::Column));
	CHECK_FALSE(look_roster_direction_allowed(RosterAnchor::MiddleRight, RosterDirection::Row));
	// 四隅はどちらでもよい。
	for (const RosterAnchor corner : {RosterAnchor::TopLeft, RosterAnchor::TopRight, RosterAnchor::BottomLeft,
					  RosterAnchor::BottomRight}) {
		CHECK(look_roster_direction_allowed(corner, RosterDirection::Row));
		CHECK(look_roster_direction_allowed(corner, RosterDirection::Column));
	}
}

TEST_CASE("自分の枠に映す映像の先頭の選択肢は、メインアカウントがあるときだけ「Discordの画面共有」にする")
{
	CHECK(std::string(cordmux::ledger::look_self_frame_first_option_key(true)) == keys::kLookSelfFrameDiscordOption);
	CHECK(std::string(cordmux::ledger::look_self_frame_first_option_key(false)) == keys::kLookNoneOption);
}

TEST_CASE("表示タブの View: ダミー映像の切り替えは、押した後の値を持ち、いつでも押せる")
{
	Observation obs = healthy();
	obs.choices.dummy = FeatureSwitch::Off;
	dockview::LookTabView v = decide_dock(obs).look_tab;
	CHECK_FALSE(v.dummy_on);
	CHECK(v.dummy_toggle.id == ActionId::ToggleDummy);
	CHECK(v.dummy_toggle.args.on);
	CHECK_FALSE(v.dummy_toggle.disabled_reason.has_value());
	CHECK(v.dummy_toggle.label.id == keys::kLookShowPlaceholdersCheck);

	obs.choices.dummy = FeatureSwitch::On;
	v = decide_dock(obs).look_tab;
	CHECK(v.dummy_on);
	CHECK_FALSE(v.dummy_toggle.args.on);
	CHECK_FALSE(v.dummy_toggle.disabled_reason.has_value());
}

TEST_CASE("表示タブの View: アンインストールを始めた後は、ダミー映像を切り替えられない")
{
	Observation obs = healthy();
	obs.uninstall.phase = UninstallPhase::Running;
	const dockview::LookTabView v = decide_dock(obs).look_tab;
	REQUIRE(v.dummy_toggle.disabled_reason.has_value());
	CHECK(v.dummy_toggle.disabled_reason->id == keys::kSettingsUninstallStartedText);
}

TEST_CASE("表示タブの View: メインアカウントの有無を持つ")
{
	Observation obs = healthy();
	obs.choices.broadcaster_user_id.clear();
	CHECK_FALSE(decide_dock(obs).look_tab.has_main_account);
	obs.choices.broadcaster_user_id = "500";
	CHECK(decide_dock(obs).look_tab.has_main_account);
}

TEST_CASE("ダミー映像を表示している間、1 行目は「ダミー映像を表示中」になる")
{
	Observation obs = healthy();
	obs.choices.dummy = FeatureSwitch::On;
	const dockview::DockViewV2 v = decide_dock(obs);
	REQUIRE_FALSE(v.fallback_line.forms.empty());
	CHECK(v.fallback_line.forms.back().id == keys::kDockSummaryPlaceholdersShortStatus);
	obs.choices.dummy = FeatureSwitch::Off;
	CHECK(decide_dock(obs).fallback_line.forms.back().id != keys::kDockSummaryPlaceholdersShortStatus);
}

TEST_CASE("[先頭に配置] は、自分の枠に映す映像が「(なし)」の間 (OBS の映像を選んでおらず、メインアカウントもない) だけ押せない")
{
	Settings s;
	CHECK_FALSE(look_self_slot_first_enabled(s, false));
	// 「Discordの画面共有」(メインアカウントがある)。
	CHECK(look_self_slot_first_enabled(s, true));
	// OBS のソースかシーン。UUID と名前のどちらかがあれば選んでいる。
	s.self_frame_source_name = "ゲーム画面";
	CHECK(look_self_slot_first_enabled(s, false));
	s.self_frame_source_name.clear();
	s.self_frame_source_uuid = "u-1";
	CHECK(look_self_slot_first_enabled(s, false));
	// メインアカウントの有無は、渡された値で決める (表示タブの設定の写しは古いことがある)。
	Settings stale;
	stale.broadcaster_user_id = "123";
	CHECK_FALSE(look_self_slot_first_enabled(stale, false));
}

TEST_CASE("「画面共有している参加者」の [表示] を外すと、[1人のときは非表示]、[位置]、[アバターの位置] が押せない")
{
	Settings s;
	for (const LookControl c :
	     {LookControl::SlotLabelHideWhenSingle, LookControl::SlotLabelCorner, LookControl::SlotLabelAvatarSide}) {
		CHECK(look_control_enabled(c, s));
	}
	s.label_show_in_slot = false;
	for (const LookControl c :
	     {LookControl::SlotLabelHideWhenSingle, LookControl::SlotLabelCorner, LookControl::SlotLabelAvatarSide}) {
		CHECK_FALSE(look_control_enabled(c, s));
	}
	// 名前ラベルの見た目 (名前、アバターなど) は、並びの名前ラベルにも使うので押せるまま。
	CHECK(look_control_enabled(LookControl::NameFont, s));
	CHECK(look_control_enabled(LookControl::AvatarSize, s));
}
