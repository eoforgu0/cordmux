// SPDX-License-Identifier: Apache-2.0

#include "dock/self_frame_check.h"

#include <algorithm>

#include "config/allocation.h"
#include "dock/build_observation.h"

namespace cordmux::dock {

namespace {

// 何も変えない結果 (ここから書き換える)。
SelfFrameStep unchanged(const SelfFrameCheck &check, bool settings_on)
{
	SelfFrameStep out;
	out.next = check;
	out.settings_on = settings_on;
	return out;
}

// step の後の値に、Pipeline が断ったことを当てる。ログは前の行の後に足す。
void apply_refusal(SelfFrameStep &step)
{
	sources::SelfFrameInput in;
	in.state = step.next.state;
	in.event = sources::SelfFrameEvent::Refused;
	const sources::SelfFrameResult result = sources::plan_self_frame(in);
	step.next.state = result.state;
	step.next.facts.self_frame_on = result.state.on;
	step.next.facts.self_frame_shown = result.shown;
	if (step.settings_on != result.state.on) {
		step.settings_on = result.state.on;
		step.write_settings = true;
	}
	step.logs.push_back(SelfFrameLogLine{SelfFrameLog::TurnedOffRefused, 0});
}

} // namespace

bool self_frame_in_use_of(const SelfFrameCheck &check, bool selected)
{
	return config::self_frame_in_use(selected, check.state.on);
}

bool self_frame_push_due(const SelfFrameCheck &check, bool selected)
{
	return self_frame_in_use_of(check, selected) != check.pushed_in_use;
}

SelfFrameStep refuse_self_frame_step(const SelfFrameCheck &check, bool settings_on)
{
	SelfFrameStep out = unchanged(check, settings_on);
	apply_refusal(out);
	return out;
}

SelfFrameStep assign_policy_pushed(const SelfFrameCheck &check, bool settings_on, bool requested, bool accepted)
{
	SelfFrameStep out = unchanged(check, settings_on);
	out.next.pushed_in_use = accepted;
	if (requested && !accepted)
		apply_refusal(out);
	return out;
}

SelfFrameCarryFacts self_frame_carry_facts(const config::Settings &settings, const pipeline::PipelineSnapshot &snapshot)
{
	SelfFrameCarryFacts out;
	out.owner = self_frame_user_id(settings, snapshot);
	const bool has_sub_account = !settings.broadcaster_user_id.empty() &&
				     settings.broadcaster_user_id != snapshot.facts.self_user_id.value_or(std::string());
	out.carries = !out.owner.empty() && has_sub_account;
	if (!out.carries)
		return out;
	for (const pipeline::SlotSnapshot &row : snapshot.slots) {
		if (row.kind == config::SlotKind::Screen && row.owner_user_id == out.owner) {
			out.owner_slot = row.slot;
			out.owner_order = row.added_order;
		}
	}
	if (snapshot.facts.sharing_valid) {
		const std::vector<std::string> &sharing = snapshot.facts.sharing.user_ids;
		out.owner_not_sharing = std::find(sharing.begin(), sharing.end(), out.owner) == sharing.end();
	}
	return out;
}

SelfFrameCarryPlan plan_self_frame_carry(const SelfFrameCheck &check, bool settings_on, const SelfFrameCarryFacts &facts,
					 bool now_selected)
{
	SelfFrameCarryPlan out;
	out.owner = facts.owner;
	out.before = unchanged(check, settings_on);
	if (!facts.carries)
		return out;

	if (now_selected) {
		if (facts.owner_slot == 0)
			return out;
		out.move = SelfFrameMoveKind::ToSelfFrame;
		out.before.next.state.on = true;
		out.before.next.state.excluded = false;
		out.before.settings_on = true;
		// 載せた順の位置は、メインアカウントの行にチェックを付けた時のまま引き継ぐ (映像の種類を替えても並びの位置を変えない)。
		// 自分の枠を、メインアカウントの枠の番号の人がいた位置 (1 つ前の番号の人の後、1 つ後の番号の人の前) に置く。
		out.before.next.mark = sources::self_order_mark_from_share(facts.owner_order);
		out.before.logs.push_back(SelfFrameLogLine{SelfFrameLog::TurnedOnVideoChanged, 0});
		return out;
	}

	if (!check.state.on)
		return out;
	if (facts.owner_not_sharing) {
		out.before.logs.push_back(SelfFrameLogLine{SelfFrameLog::KeptNotSharing, 0});
		return out;
	}
	out.move = SelfFrameMoveKind::ToScreenShare;
	if (check.mark.decided)
		out.after_order = check.mark.mark;
	return out;
}

SelfFrameStep settle_self_frame_carry(const SelfFrameCarryPlan &plan, const pipeline::Pipeline::SelfFrameMove &moved)
{
	using Outcome = pipeline::Pipeline::SlotChange::Outcome;
	SelfFrameStep out = unchanged(plan.before.next, plan.before.settings_on);
	switch (plan.move) {
	case SelfFrameMoveKind::None:
		// Pipeline を呼んでいないので、答えはない。
		return out;
	case SelfFrameMoveKind::ToSelfFrame:
		out.next.pushed_in_use = moved.self_frame_in_use;
		if (moved.change.outcome == Outcome::Released) {
			out.logs.push_back(SelfFrameLogLine{SelfFrameLog::RemovedFromSlot, moved.change.slot});
			out.released_slot = moved.change.slot;
		} else {
			// スナップショットの後に、メインアカウントが画面共有をやめて枠が空いていた。
			out.logs.push_back(SelfFrameLogLine{SelfFrameLog::NotInSlot, 0});
		}
		if (!moved.self_frame_in_use)
			apply_refusal(out);
		return out;
	case SelfFrameMoveKind::ToScreenShare:
		out.next.pushed_in_use = moved.self_frame_in_use;
		if (moved.change.outcome == Outcome::Assigned || moved.change.outcome == Outcome::AlreadyOwned) {
			out.next.state.on = false;
			out.settings_on = false;
			out.logs.push_back(SelfFrameLogLine{SelfFrameLog::TurnedOffVideoChanged, 0});
		} else {
			out.logs.push_back(SelfFrameLogLine{SelfFrameLog::KeptNoFreeSlot, 0});
		}
		return out;
	}
	return out;
}

} // namespace cordmux::dock
