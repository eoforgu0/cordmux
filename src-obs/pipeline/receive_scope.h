// SPDX-License-Identifier: Apache-2.0

// 設定の receive_scope (config::ReceiveScope) から決まる 2 つの判断。送信側に伝える、視聴する人の範囲と、録画の入力に
// 通してよい画面のストリームである。どちらも「枠に載っている人」を基準にする純粋関数で、libobs にも Qt にも依存しない。
// 枠に載っている人は、配信の配置に出る人 (割り当て表で画面の枠を持つ人) である。その人が画面共有をやめると枠は
// 空くので (pipeline/slot_release.h)、載っている人に数えなくなる。

#pragma once

#include <set>
#include <string>

#include "config/allocation.h"
#include "config/settings.h"
#include "ws/control_messages.h"

namespace cordmux::pipeline {

// 送信側に送る、視聴する人の範囲。Everyone なら画面共有している全員 (user_ids は持たない)。Slots なら、割り当て表の
// 画面の枠を持つ人だけで、userId の昇順に重複なく並べる (同じ範囲なら同じ並びになり、前に送った値と比べられる)。
// 画面共有しているかは見ない。画面共有していない人を入れても、送信側は視聴するものがないので何もしない。
// cordmux を無効にしている間 (feature_enabled が偽) は、受け取る範囲にかかわらず、誰も入れない (Slots で空の範囲)。
// 無効の間は映像も音声も送らないので、誰の画面にも視聴者として出ない。focus_user_id は全画面で映す人で、空でなければ
// 枠に載っていなくても入れる (枠に載せていない人を全画面にしたときに、そこから受信を始める)。
inline ws::ReceiveScopeMessage watch_scope_of(config::ReceiveScope scope, const config::AllocationTable &table,
					      bool feature_enabled, const std::string &focus_user_id = std::string())
{
	ws::ReceiveScopeMessage out;
	if (!feature_enabled) {
		out.slots_only = true;
		return out;
	}
	if (scope != config::ReceiveScope::Slots)
		return out;
	out.slots_only = true;
	std::set<std::string> owners;
	for (int slot = 1; slot <= config::kScreenSlotCount; ++slot) {
		const config::SlotAssignment *a = table.assignment(config::SlotKind::Screen, slot);
		if (a != nullptr && !a->owner_user_id.empty())
			owners.insert(a->owner_user_id);
	}
	if (!focus_user_id.empty())
		owners.insert(focus_user_id);
	out.user_ids.assign(owners.begin(), owners.end());
	return out;
}

// 録画の入力 (映像と、画面共有の音) に通してよいストリームか。Everyone ならすべて通す。Slots なら、画面のストリームは
// その人が画面の枠を持つときと、全画面で映す人 (focus_user_id) のときだけ通す。声のストリームは、受け取る範囲が映像の範囲
// なので、どちらでも通す。
inline bool record_stream_allowed(config::ReceiveScope scope, const config::AllocationTable &table, bool screen,
				  const std::string &user_id, const std::string &focus_user_id = std::string())
{
	if (scope != config::ReceiveScope::Slots || !screen)
		return true;
	if (!focus_user_id.empty() && user_id == focus_user_id)
		return true;
	return table.slot_of_owner(config::SlotKind::Screen, user_id) != 0;
}

} // namespace cordmux::pipeline
