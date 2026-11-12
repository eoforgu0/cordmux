// SPDX-License-Identifier: Apache-2.0

// decide_dock() (ledger/rules_dock.cpp) と stabilize() (ledger/notice_tracker.h) のテストが共に使う Observation と
// 関数。test_ledger_tiers.cpp・test_ledger_dock_rules.cpp・test_ledger_notice_tracker.cpp などが、同じ
// 「何も問題がなく、接続している状態」から 1 つずつ変えてテストする。各テストに同じものをコピーすると、片方だけ
// 修正したときにテストの前提が食い違う。userId は、実在の Discord の ID ではない短い数字にする。

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ledger/dock_view.h"
#include "ledger/observation.h"
#include "ledger/rules.h"

namespace ledger_dock_test {

using namespace cordmux::ledger;

inline constexpr std::int64_t kSec = 1'000'000;
// テストの「今」。2000-01-10 12:00:00 UTC (healthy() の時差 +9 時間では 21:00)。今日かどうかと日付を見る
// テストがあるので、時刻に意味がある。
inline constexpr std::int64_t kNow = 947'505'600LL * kSec;

// slot の枠に載っていて、画面共有していて、フレームが届いている人。
inline PersonFact showing(const std::string &user_id, const std::string &name, int slot)
{
	PersonFact p;
	p.user_id = user_id;
	p.display_name = name;
	p.discord_name = name;
	p.slot = slot;
	p.slot_visible = true;
	p.in_roster = true;
	p.sharing = true;
	p.sharing_known = true;
	p.sharing_since_us = kNow - 60 * kSec;
	p.last_video_frame_at_us = kNow - kSec / 10;
	return p;
}

// 通話に参加しているが、枠に載っていない人。
inline PersonFact present(const std::string &user_id, const std::string &name)
{
	PersonFact p;
	p.user_id = user_id;
	p.display_name = name;
	p.discord_name = name;
	p.in_roster = true;
	p.sharing_known = true;
	return p;
}

// 何も問題がなく、接続していて、2 人の通話で 1 人を表示している状態。各テストはここから 1 つずつ変える。
inline Observation healthy()
{
	Observation obs;
	obs.now_us = kNow;
	obs.utc_offset_sec = 9 * 3600;
	obs.choices.feature = FeatureSwitch::On;
	obs.choices.recording = FeatureSwitch::Off;
	obs.choices.dummy = FeatureSwitch::Off;
	obs.choices.composite_checked = true;
	obs.choices.composite_present = true;
	obs.choices.discord_audio_source = DiscordAudioSource::NotFound;
	// cordmux のソースが配信に出ていて、鳴る (配信中の普通の状態)。
	obs.choices.composite_active = true;
	obs.choices.composite_audible = true;
	obs.listener.state = ListenerState::Listening;
	obs.install.process = VesktopProcess::Running;
	obs.install.dir = VencordDirState::Matches;
	obs.link.state = LinkState::Connected;
	obs.link.connected_at_us = kNow - 60 * kSec;
	obs.call.presence = Presence::InCall;
	obs.call.roster_count = 2;
	obs.versions.plugin = "1.0.0+abc";
	obs.versions.discord_side = "1.0.0+abc";
	obs.versions.vesktop = "1.5.3";
	obs.versions.bundled_vencord = "1.0.0+abc";
	obs.uninstall.obs_streaming = false;
	obs.uninstall.obs_recording = false;
	obs.obs_outputs.streaming = false;
	obs.obs_outputs.recording = false;
	obs.recording.phase = RecordPhase::Idle;
	obs.people.push_back(showing("100", "taro", 1));
	obs.people.push_back(present("200", "hanako"));
	return obs;
}

// 接続していない状態。primary は Connected より下の行 (インストールと Vesktop の状態の行) になる。
inline Observation not_connected()
{
	Observation obs = healthy();
	obs.link.state = LinkState::NeverConnected;
	obs.link.connected_at_us.reset();
	obs.call.presence = Presence::Unknown;
	obs.call.roster_count.reset();
	return obs;
}

inline std::vector<dockview::Notice> notices_of(const dockview::DockViewV2 &view, const std::string &rule_name)
{
	std::vector<dockview::Notice> out;
	for (const dockview::Notice &n : view.notices) {
		if (n.rule_name == rule_name)
			out.push_back(n);
	}
	return out;
}

inline const dockview::PersonRow *row_of(const dockview::DockViewV2 &view, const std::string &user_id)
{
	for (const dockview::PersonRow &row : view.people) {
		if (row.user_id == user_id)
			return &row;
	}
	return nullptr;
}

// View の中のすべての TextRef を集める (テキストのキーと %N に入れる値を確認するテストが使う)。
inline void collect_refs(const dockview::DockViewV2 &v, std::vector<TextRef> *out)
{
	const auto add = [out](const TextRef &t) {
		if (!t.id.empty())
			out->push_back(t);
	};
	const auto add_label = [&add](const Label &l) {
		if (l.text.has_value())
			add(*l.text);
	};
	const auto add_button = [&add](const dockview::ViewButton &b) {
		add(b.label);
		if (b.disabled_reason.has_value())
			add(*b.disabled_reason);
	};
	const auto add_line = [&](const dockview::FirstLine &f) {
		for (const TextRef &t : f.forms)
			add(t);
		if (f.more_count.has_value())
			add(*f.more_count);
		if (f.button.has_value())
			add_button(*f.button);
	};
	add_line(v.first_line);
	add_line(v.fallback_line);
	for (const dockview::Notice &n : v.notices) {
		add(n.headline);
		for (const TextRef &t : n.body)
			add(t);
		if (n.line2.has_value())
			add(*n.line2);
		if (n.tip.has_value())
			add(*n.tip);
		for (const dockview::StageItem &s : n.stages)
			add(s.text);
		for (const dockview::ViewButton &b : n.buttons)
			add_button(b);
	}
	if (v.list_reason.has_value()) {
		add(v.list_reason->text);
		if (v.list_reason->short_text.has_value())
			add(*v.list_reason->short_text);
	}
	if (v.empty.has_value()) {
		add(v.empty->text);
		if (v.empty->button.has_value())
			add_button(*v.empty->button);
	}
	const auto add_row = [&](const dockview::PersonRow &r) {
		add_label(r.name);
		add_button(r.check_action);
		if (r.status.has_value())
			add(r.status->text);
		if (r.quality.has_value())
			add(*r.quality);
		if (r.resolution.has_value())
			add(*r.resolution);
		for (const TextRef &t : r.line2_parts)
			add(t);
		add_button(r.menu.always_on_toggle);
		add_button(r.menu.focus_toggle);
		add_button(r.menu.rename);
		for (const dockview::VoiceChoice &c : r.menu.voices) {
			add_label(c.label);
			add_button(c.action);
		}
		if (r.refused_note.has_value())
			add(*r.refused_note);
		for (const dockview::ReplaceChoice &c : r.replace_choices) {
			add_label(c.name);
			add_button(c.action);
		}
	};
	for (const dockview::PersonRow &r : v.people)
		add_row(r);
	if (v.people_list.pinned.has_value())
		add_row(*v.people_list.pinned);
	for (const dockview::PersonRow &r : v.people_list.sharing)
		add_row(r);
	for (const dockview::PersonRow &r : v.people_list.in_call)
		add_row(r);
	add(v.people_list.slots_label);
	add(v.people_list.sharing_heading);
	add(v.people_list.in_call_heading);
	const auto add_account = [&](const dockview::AccountRow &row) {
		add_label(row.name);
		add_label(row.call);
	};
	add_account(v.accounts.main.row);
	add_button(v.accounts.main.change);
	for (const dockview::MainAccountChoice &c : v.accounts.main.choices) {
		add_label(c.label);
		add_button(c.action);
	}
	add_label(v.accounts.main.no_main.label);
	add_button(v.accounts.main.no_main.action);
	if (v.accounts.sub.has_value()) {
		add_account(v.accounts.sub->row);
		add_button(v.accounts.sub->move);
		add_button(v.accounts.sub->leave);
	}
	add(v.record.label);
	add(v.record.name);
	add_button(v.record.action);
	for (const TextRef &t : v.record.width_labels)
		add(t);
	add_button(v.look_tab.dummy_toggle);
	add_button(v.status_tab.reset);
	add(v.status_tab.recording.last);
	for (const dockview::StatusHistoryLine &line : v.status_tab.history) {
		add(line.text);
		if (line.subject.has_value())
			add(*line.subject);
	}
	add_button(v.menu.open_settings);
	add_button(v.menu.open_participants);
	add_button(v.menu.export_bundle);
	add_button(v.menu.feature_toggle);
	for (const dockview::ParticipantRow &p : v.participants) {
		add_label(p.name);
		add_button(p.always_on_toggle);
		add_button(p.rename);
		add_button(p.roster_toggle);
		for (const dockview::VoiceChoice &c : p.voices) {
			add_label(c.label);
			add_button(c.action);
		}
	}
	for (const dockview::VersionRow &row : v.about.versions) {
		add(row.label);
		add_label(row.value);
	}
	if (v.about.vesktop_rewrote_at_startup.has_value())
		add(*v.about.vesktop_rewrote_at_startup);
	if (v.about.export_result.has_value())
		add(v.about.export_result->text);
	add_button(v.about.export_button);
	add_button(v.about.uninstall_button);
	if (v.uninstall_result.has_value()) {
		add(v.uninstall_result->main);
		for (const TextRef &t : v.uninstall_result->notes)
			add(t);
		for (const dockview::LeftoverGroup &g : v.uninstall_result->groups) {
			add(g.heading);
			for (const Label &l : g.items)
				add_label(l);
		}
	}
}

} // namespace ledger_dock_test
