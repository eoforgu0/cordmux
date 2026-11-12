// SPDX-License-Identifier: Apache-2.0

// 枠に載っている人が画面共有をやめたか通話から抜けたときに、その人の枠を空ける判断。枠を持つのは、いま画面共有を
// している人 (画面共有の枠) と、いま通話に参加している人 (声の枠) だけにするためで、短時間で戻った人も、自動で載せる
// 設定の人と選び方が「自動」のときを除いて、押し直すまで載らない。画面の枠と声の枠は、別の関数で判断する。
//
// 画面の枠 (screen_slots_to_release)。facts の sharing (Discord 側で画面共有している人) だけで判断し、
// handle_facts だけが呼ぶ。configure の streams では判断しない。送信側は切断のときに画面のストリームをすべて止めて
// 作り直すので、繋ぎ直した直後の configure には画面のストリームが 1 つもなく、見ると、共有を続けている全員が
// 「やめた」ように見える (stream_router.h の exclude_user のコメントと同じ理由)。また configure が facts より先に
// 届くと、画面共有を始めたばかりの人が sharing にまだいないので、configure の側で空けると、その人を外してしまう。
//
// 声の枠 (voice_slots_to_release)。facts の roster (通話の参加者) で判断する。ただし、その枠で声を出している人の
// 声のストリームが直前の configure に載っている間は空けない。声の経路は configure でしか戻らないので、roster が
// 一瞬欠けただけで経路を外すと、その人の声が次の configure まで消え、配信の音が途切れるためである。facts と
// configure のどちらが先に届いても、ストリームが消えた時点で roster にもいなければ、最後に空く (handle_facts と
// handle_configure の両方が声の枠を判断する)。
//
// どちらも空けないとき (枠を持ち続ける。繋ぎ直して同じ人が共有していれば、そのまま映り直す)。
//   - facts がまだ届いていないか、roster がない (送信側が読めていない)。
//   - 画面の枠で、sharing のキーがない facts (sharing_valid が偽)。
// roster が空 (Vesktop のアカウントが通話にいない) のときは、共有している人も声の人もいないものとして、普通に
// 空ける (手動で載せた人は、戻っても自動では載らない。利用者が外した人を勝手に戻さないため)。Vesktop との接続が切れている間は facts が
// 新しく届かないので、この判断は呼ばれない。
// 純粋関数で、libobs にも Qt にも依存しない。

#pragma once

#include "config/allocation.h"
#include "pipeline/facts_state.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace cordmux::pipeline {

// 空ける枠 1 つ。owner は枠に載っている人、voice は声の枠で実際に声を出している人 (画面共有の枠では owner と同じ)。
struct SlotToRelease {
	config::SlotKind kind = config::SlotKind::Screen;
	int slot = 0;
	std::string owner;
	std::string voice;
};

// 画面の枠で空ける枠を返す。sharing にいない人の枠である。
inline std::vector<SlotToRelease> screen_slots_to_release(const config::AllocationTable &table,
							  const FactsSnapshot &facts)
{
	std::vector<SlotToRelease> out;
	if (!facts.received || !facts.roster.has_value() || !facts.sharing_valid)
		return out;
	const std::set<std::string> sharing(facts.sharing.user_ids.begin(), facts.sharing.user_ids.end());
	for (int slot = 1; slot <= table.slot_extent(config::SlotKind::Screen); ++slot) {
		const config::SlotAssignment *a = table.assignment(config::SlotKind::Screen, slot);
		if (a != nullptr && sharing.count(a->owner_user_id) == 0)
			out.push_back(SlotToRelease{config::SlotKind::Screen, slot, a->owner_user_id, a->voice_user_id});
	}
	return out;
}

// 声の枠で空ける枠を返す。roster にいない人の枠である。次のどれかに当てはまる間は空けない。
//   - 載っている人が roster にいる。
//   - 使う声に選んだ人 (voice_links。枠に載っている人 -> 使う声に選んだ人) が roster にいる。その声を運んでいる枠である。
//   - その枠で声を出している人 (voice_user_id) の声のストリームが、直前の configure に載っている
//     (live_voice_users。configure の streams のうち声の種類のものの user_id)。
inline std::vector<SlotToRelease> voice_slots_to_release(const config::AllocationTable &table,
							 const FactsSnapshot &facts,
							 const std::map<std::string, std::string> &voice_links,
							 const std::set<std::string> &live_voice_users)
{
	std::vector<SlotToRelease> out;
	if (!facts.received || !facts.roster.has_value())
		return out;
	const std::set<std::string> roster(facts.roster->user_ids.begin(), facts.roster->user_ids.end());
	const std::map<std::string, std::string> links = config::usable_voice_links(voice_links);
	for (int slot = 1; slot <= table.slot_extent(config::SlotKind::Voice); ++slot) {
		const config::SlotAssignment *a = table.assignment(config::SlotKind::Voice, slot);
		if (a == nullptr)
			continue;
		bool keep = roster.count(a->owner_user_id) != 0 || live_voice_users.count(a->voice_user_id) != 0;
		const auto linked = links.find(a->owner_user_id);
		if (!keep && linked != links.end())
			keep = roster.count(linked->second) != 0;
		if (!keep)
			out.push_back(SlotToRelease{config::SlotKind::Voice, slot, a->owner_user_id, a->voice_user_id});
	}
	return out;
}

} // namespace cordmux::pipeline
