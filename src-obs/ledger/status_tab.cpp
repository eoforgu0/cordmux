// SPDX-License-Identifier: Apache-2.0

// 状態タブの判断 (status_tab.h) を実装する。

#include "ledger/status_tab.h"

#include <algorithm>
#include <map>
#include <set>

#include "ledger/natural_order.h"
#include "ledger/text_ids.h"

namespace cordmux::ledger {

namespace {

namespace keys = text::keys;

constexpr std::int64_t kUsPerSec = 1'000'000;
constexpr std::int64_t kSecPerDay = 86'400;

TextRef ref(const char *id, std::vector<std::string> args = {})
{
	TextRef r;
	r.id = id;
	r.args = std::move(args);
	return r;
}

// 時差を足した時刻の日付を、1970-01-01 からの日数で返す。
std::int64_t local_day(std::int64_t epoch_us, int utc_offset_sec)
{
	std::int64_t seconds = epoch_us / kUsPerSec;
	if (epoch_us % kUsPerSec < 0)
		--seconds;
	seconds += utc_offset_sec;
	std::int64_t day = seconds / kSecPerDay;
	if (seconds % kSecPerDay < 0)
		--day;
	return day;
}

bool is_stall_status(const dockview::RowStatus &status)
{
	return status.state == dockview::RowState::NoVideo && status.text.id == keys::kDockRowNotReceivingStatus;
}

const StallEntry *find_entry(const StallTally &tally, const std::string &user_id)
{
	for (const StallEntry &e : tally.entries) {
		if (e.user_id == user_id)
			return &e;
	}
	return nullptr;
}

} // namespace

std::string when_arg_of(std::int64_t epoch_us, std::int64_t now_us, int utc_offset_sec)
{
	if (local_day(epoch_us, utc_offset_sec) == local_day(now_us, utc_offset_sec))
		return dockview::time_arg(epoch_us);
	return dockview::date_time_arg(epoch_us);
}

// ---- 映像途切れの累計 ----

std::vector<StalledPerson> stalled_people(const dockview::DockViewV2 &stable)
{
	std::vector<StalledPerson> out;
	for (const dockview::PersonRow &row : stable.people) {
		if (row.user_id.empty() || !row.status.has_value() || !is_stall_status(*row.status))
			continue;
		out.push_back(StalledPerson{row.user_id, row.name});
	}
	return out;
}

StallTally reset_stall_tally(const StallTally &prev)
{
	StallTally next;
	for (const StallEntry &e : prev.entries) {
		// 途切れが続いている人だけ残す。その途切れが終わるまでは数えない。
		if (!e.since_us.has_value() && !e.ignoring)
			continue;
		StallEntry kept;
		kept.user_id = e.user_id;
		kept.name = e.name;
		kept.ignoring = true;
		next.entries.push_back(std::move(kept));
	}
	return next;
}

StallTally step_stall_tally(const StallTally &prev, const std::vector<StalledPerson> &stalled, std::int64_t now_us,
			    bool reset)
{
	StallTally next = reset ? reset_stall_tally(prev) : prev;
	std::map<std::string, const StalledPerson *> now_stalled;
	for (const StalledPerson &p : stalled)
		now_stalled.emplace(p.user_id, &p);

	for (StallEntry &e : next.entries) {
		const auto it = now_stalled.find(e.user_id);
		if (it != now_stalled.end()) {
			e.name = it->second->name;
			if (!e.ignoring && !e.since_us.has_value()) {
				++e.count;
				e.since_us = now_us;
			}
		} else {
			e.ignoring = false;
			if (e.since_us.has_value()) {
				e.total_us += std::max<std::int64_t>(0, now_us - *e.since_us);
				e.since_us.reset();
			}
		}
	}
	for (const StalledPerson &p : stalled) {
		if (find_entry(next, p.user_id) != nullptr)
			continue;
		StallEntry e;
		e.user_id = p.user_id;
		e.name = p.name;
		e.count = 1;
		e.since_us = now_us;
		next.entries.push_back(std::move(e));
	}
	next.entries.erase(std::remove_if(next.entries.begin(), next.entries.end(),
					  [](const StallEntry &e) {
						  return e.count == 0 && e.total_us == 0 && !e.since_us.has_value() && !e.ignoring;
					  }),
			   next.entries.end());
	return next;
}

std::int64_t stall_total_us(const StallEntry &entry, std::int64_t now_us)
{
	std::int64_t total = entry.total_us;
	if (entry.since_us.has_value())
		total += std::max<std::int64_t>(0, now_us - *entry.since_us);
	return total;
}

// ---- 履歴 ----

HistorySnapshot history_snapshot_of(const dockview::DockViewV2 &stable, const Observation &obs)
{
	HistorySnapshot snap;
	for (const dockview::Notice &n : stable.notices)
		snap.notices.push_back(HistoryNotice{n.key, n.headline});
	snap.link = obs.link.state;
	snap.recording = obs.recording.phase == RecordPhase::Recording;
	for (const dockview::PersonRow &row : stable.people) {
		if (row.slot.has_value())
			snap.slots.push_back(HistorySlot{row.user_id, row.name});
		for (const PersonFact &person : obs.people) {
			if (person.user_id == row.user_id && person.receive_restarts > 0)
				snap.restarts.push_back(HistoryRestart{row.user_id, row.name, person.receive_restarts});
		}
	}
	return snap;
}

std::vector<HistoryEvent> history_events(const std::optional<HistorySnapshot> &prev, const HistorySnapshot &cur)
{
	std::vector<HistoryEvent> out;
	const auto has_notice = [](const HistorySnapshot &s, const std::string &key) {
		return std::any_of(s.notices.begin(), s.notices.end(), [&](const HistoryNotice &n) { return n.key == key; });
	};
	const auto has_slot = [](const HistorySnapshot &s, const std::string &user_id) {
		return std::any_of(s.slots.begin(), s.slots.end(), [&](const HistorySlot &p) { return p.user_id == user_id; });
	};

	if (prev.has_value()) {
		for (const HistoryNotice &n : prev->notices) {
			if (!has_notice(cur, n.key))
				out.push_back(HistoryEvent{ref(keys::kStatusHistoryNoticeClearedStatus), n.headline});
		}
	}
	for (const HistoryNotice &n : cur.notices) {
		if (!prev.has_value() || !has_notice(*prev, n.key))
			out.push_back(HistoryEvent{ref(keys::kStatusHistoryNoticeAppearedStatus), n.headline});
	}
	if (!prev.has_value())
		return out;

	if (prev->link != LinkState::Connected && cur.link == LinkState::Connected)
		out.push_back(HistoryEvent{ref(keys::kStatusHistoryConnectedStatus), std::nullopt});
	else if (prev->link == LinkState::Connected && cur.link != LinkState::Connected)
		out.push_back(HistoryEvent{ref(keys::kStatusHistoryDisconnectedStatus), std::nullopt});

	for (const HistorySlot &p : prev->slots) {
		if (!has_slot(cur, p.user_id))
			out.push_back(HistoryEvent{ref(keys::kStatusHistorySlotRemovedStatus, {p.name.literal}), std::nullopt});
	}
	for (const HistorySlot &p : cur.slots) {
		if (!has_slot(*prev, p.user_id))
			out.push_back(HistoryEvent{ref(keys::kStatusHistorySlotAddedStatus, {p.name.literal}), std::nullopt});
	}

	for (const HistoryRestart &r : cur.restarts) {
		std::uint64_t before = 0;
		for (const HistoryRestart &p : prev->restarts) {
			if (p.user_id == r.user_id)
				before = p.count;
		}
		if (r.count > before)
			out.push_back(
				HistoryEvent{ref(keys::kStatusHistoryReceiveRestartedStatus, {r.name.literal}), std::nullopt});
	}

	if (!prev->recording && cur.recording)
		out.push_back(HistoryEvent{ref(keys::kStatusHistoryRecordingStartedStatus), std::nullopt});
	else if (prev->recording && !cur.recording)
		out.push_back(HistoryEvent{ref(keys::kStatusHistoryRecordingStoppedStatus), std::nullopt});
	return out;
}

// ---- 最後の録画 ----

RecordingMemory step_recording_memory(const RecordingMemory &prev, const Observation &obs, std::int64_t now_us)
{
	RecordingMemory next = prev;
	const bool recording = obs.recording.phase == RecordPhase::Recording;
	if (recording) {
		if (!next.active) {
			next.active = true;
			next.start_us = obs.recording.enabled_since_us.value_or(now_us);
		}
		next.folder = obs.recording.session_dir;
	} else if (next.active) {
		next.active = false;
		next.has_last = true;
		next.last_start_us = next.start_us;
		next.last_duration_us = std::max<std::int64_t>(0, now_us - next.start_us);
		next.last_folder = next.folder;
	}
	return next;
}

dockview::StatusRecordingView recording_section_of(const RecordingMemory &memory, std::int64_t now_us,
						     int utc_offset_sec)
{
	dockview::StatusRecordingView view;
	if (memory.active) {
		view.section = dockview::StatusRecordingSection::Recording;
		return view;
	}
	if (!memory.has_last)
		return view;
	view.section = dockview::StatusRecordingSection::Last;
	view.folder = memory.last_folder;
	const std::string when = when_arg_of(memory.last_start_us, now_us, utc_offset_sec);
	const std::int64_t minutes = memory.last_duration_us / (60 * kUsPerSec);
	if (minutes < 1)
		view.last = ref(keys::kStatusRecordingLastShortStatus, {when});
	else if (minutes < 60)
		view.last = ref(keys::kStatusRecordingLastMinutesStatus, {when, std::to_string(minutes)});
	else
		view.last = ref(keys::kStatusRecordingLastHoursStatus,
				{when, std::to_string(minutes / 60), std::to_string(minutes % 60)});
	return view;
}

// ---- 状態タブ全体 ----

StatusResult settle_status(const StatusState &prev, const dockview::DockViewV2 &stable, const Observation &obs,
			   const StatusInput &input)
{
	StatusResult result;
	StatusState &next = result.next;
	const bool stream_started = input.streaming && !prev.streaming;
	next.streaming = input.streaming;
	next.tally = step_stall_tally(prev.tally, stalled_people(stable), input.now_us, stream_started);
	next.recording = step_recording_memory(prev.recording, obs, input.now_us);

	// 履歴。新しい出来事を先頭に入れ、上限を超えた古い行を捨てる。
	const HistorySnapshot snap = history_snapshot_of(stable, obs);
	next.history = prev.history;
	for (const HistoryEvent &e : history_events(prev.prev, snap)) {
		dockview::StatusHistoryLine line;
		line.when = when_arg_of(input.now_us, input.now_us, input.utc_offset_sec);
		line.text = e.text;
		line.subject = e.subject;
		next.history.insert(next.history.begin(), std::move(line));
	}
	if (next.history.size() > dockview::kStatusHistoryMax)
		next.history.resize(dockview::kStatusHistoryMax);
	next.prev = snap;

	// 表の行。直近 1 分にフレームが届いた人 (decide_dock が作った行) に累計を付け、累計だけがある人の行を足す。
	dockview::StatusTabView &view = result.view;
	view = stable.status_tab;
	// 直近 1 分の列は kStatusReceiveRefreshUs ごとに書き換える。初めて現れた人の列は、待たずに出す。
	const bool refresh = !prev.receive_shown_at_us.has_value() ||
			     input.now_us - *prev.receive_shown_at_us >= kStatusReceiveRefreshUs ||
			     input.now_us < *prev.receive_shown_at_us;
	if (refresh) {
		next.receive_shown_at_us = input.now_us;
		for (const dockview::StatusReceiveRow &row : view.rows)
			next.receive_shown[row.user_id] = row.streams;
	} else {
		next.receive_shown_at_us = prev.receive_shown_at_us;
		next.receive_shown = prev.receive_shown;
		for (dockview::StatusReceiveRow &row : view.rows) {
			const auto [it, inserted] = next.receive_shown.try_emplace(row.user_id, row.streams);
			if (!inserted)
				row.streams = it->second;
		}
	}
	std::set<std::string> listed;
	for (dockview::StatusReceiveRow &row : view.rows) {
		listed.insert(row.user_id);
		if (const StallEntry *e = find_entry(next.tally, row.user_id)) {
			row.gap_count = e->count;
			row.gap_total_us = stall_total_us(*e, input.now_us);
		}
	}
	for (const StallEntry &e : next.tally.entries) {
		const std::int64_t total = stall_total_us(e, input.now_us);
		if ((e.count == 0 && total == 0) || listed.count(e.user_id) != 0)
			continue;
		dockview::StatusReceiveRow row;
		row.user_id = e.user_id;
		row.name = e.name;
		row.gap_count = e.count;
		row.gap_total_us = total;
		view.rows.push_back(std::move(row));
	}
	std::sort(view.rows.begin(), view.rows.end(),
		  [](const dockview::StatusReceiveRow &a, const dockview::StatusReceiveRow &b) {
			  if (a.name.literal != b.name.literal)
				  return natural_less(a.name.literal, b.name.literal);
			  return a.user_id < b.user_id;
		  });

	view.recording = recording_section_of(next.recording, input.now_us, input.utc_offset_sec);
	view.history = next.history;
	return result;
}

StatusState reset_status(const StatusState &prev)
{
	StatusState next = prev;
	next.tally = reset_stall_tally(prev.tally);
	return next;
}

} // namespace cordmux::ledger
