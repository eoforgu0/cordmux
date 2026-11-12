// SPDX-License-Identifier: Apache-2.0

// 表示タブの判断 (look_tab.h) を実装する。

#include "ledger/look_tab.h"

#include "ledger/text_ids.h"

namespace cordmux::ledger {

int look_reflect_wait_ms(std::int64_t last_reflect_ms, std::int64_t now_ms, int interval_ms)
{
	if (now_ms < last_reflect_ms || interval_ms <= 0)
		return 0;
	const std::int64_t elapsed = now_ms - last_reflect_ms;
	return elapsed >= interval_ms ? 0 : static_cast<int>(interval_ms - elapsed);
}

bool look_field_follows(bool editing, bool edit_pending, bool differs)
{
	return differs && !editing && !edit_pending;
}

bool look_control_enabled(LookControl control, const config::Settings &s)
{
	const bool name = s.label_show_name;
	const bool glow = s.label_show_speaking;
	// 時間の 3 つは、光のフェードと、話していない人を暗くする動きの両方に適用される。どちらも動作しないときだけ押せなくする。
	const bool timing = s.label_show_speaking || s.label_dim_pct > 0;
	switch (control) {
	case LookControl::NameFont:
	case LookControl::NameWeight:
	case LookControl::NameSize:
	case LookControl::NameColor:
	case LookControl::OutlineShow:
		return name;
	case LookControl::OutlineColor:
	case LookControl::OutlineSize:
		return name && s.label_show_outline;
	case LookControl::AvatarSize:
	case LookControl::AvatarShape:
		return s.label_show_avatar;
	case LookControl::PlateShow:
		return name || s.label_show_avatar;
	case LookControl::PlateColor:
	case LookControl::PlateOpacity:
		return (name || s.label_show_avatar) && s.label_show_plate;
	case LookControl::SlotBackgroundColor:
	case LookControl::SlotBackgroundOpacity:
		return s.slot_background_show;
	case LookControl::GlowTargets:
	case LookControl::GlowColor:
	case LookControl::GlowThickness:
	case LookControl::GlowBlur:
		return glow;
	case LookControl::TimingFadeIn:
	case LookControl::TimingHold:
	case LookControl::TimingFadeOut:
		return timing;
	case LookControl::SelfThreshold:
		return !s.self_speaking_source_uuid.empty() || !s.self_speaking_source_name.empty();
	case LookControl::SlotLabelHideWhenSingle:
	case LookControl::SlotLabelCorner:
	case LookControl::SlotLabelAvatarSide:
		return s.label_show_in_slot;
	case LookControl::RosterAnchor:
	case LookControl::RosterOffset:
	case LookControl::RosterDirection:
	case LookControl::RosterGap:
	case LookControl::RosterAvatarSide:
	case LookControl::RosterShowBots:
		return s.roster_show;
	case LookControl::RosterNameWidth:
		return s.roster_show && name;
	}
	return true;
}

bool look_self_slot_first_enabled(const config::Settings &settings, bool has_main_account)
{
	return has_main_account || config::self_frame_video_selected(settings);
}

bool look_roster_direction_allowed(config::RosterAnchor anchor, config::RosterDirection direction)
{
	return config::roster_direction_for(anchor, direction) == direction;
}

const char *look_self_frame_first_option_key(bool has_main_account)
{
	return has_main_account ? text::keys::kLookSelfFrameDiscordOption : text::keys::kLookNoneOption;
}

} // namespace cordmux::ledger
