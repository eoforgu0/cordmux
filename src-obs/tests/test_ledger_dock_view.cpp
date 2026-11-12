// SPDX-License-Identifier: Apache-2.0

// ledger/dock_view.h の単体テスト。すべての型の operator== が、すべてのフィールドを比べることを確認する。
// フィールドを 1 つずつ変えて「違う」と判定されるかを見る。operator== にフィールドを追加し忘れると、その
// フィールドだけが変わった View を「同じ」とみなし、画面が古いまま残る。日時の引数 (time_arg() など) の形と
// 読み戻しも確認する。Qt も libobs も使わない。

#include "doctest.h"

#include "ledger/dock_view.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

using cordmux::ledger::ActionId;
using cordmux::ledger::Label;
using cordmux::ledger::TextRef;
using namespace cordmux::ledger::dockview;

namespace {

TextRef text(const std::string &id)
{
	TextRef t;
	t.id = id;
	return t;
}

// base から 1 つのフィールドだけを変えた値が、base と違うと判定されることを確認する。
template <class T> void check_each_field_counts(const T &base, const std::vector<std::function<void(T &)>> &mutations)
{
	CHECK(base == base);
	CHECK_FALSE(base != base);
	int index = 0;
	for (const auto &mutate : mutations) {
		CAPTURE(index);
		T changed = base;
		mutate(changed);
		CHECK_FALSE(changed == base);
		CHECK(changed != base);
		++index;
	}
}

ViewButton button(const std::string &id)
{
	ViewButton b;
	b.label = text(id);
	return b;
}

} // namespace

TEST_CASE("TextRef と Label の等値比較は、キー・引数・リテラルの文字列をすべて比べる")
{
	TextRef a = text("test.x");
	a.args = {"1"};
	check_each_field_counts<TextRef>(a, {
		[](TextRef &t) { t.id = "test.y"; },
		[](TextRef &t) { t.args = {"2"}; },
		[](TextRef &t) { t.args.push_back("3"); },
	});

	const Label by_text = Label::from_text(a);
	const Label by_literal = Label::from_literal("name");
	CHECK(by_text != by_literal);
	check_each_field_counts<Label>(by_text, {
		[](Label &l) { l.text->args = {"9"}; },
		[](Label &l) { l.text.reset(); },
		[](Label &l) { l.literal = "x"; },
	});
	check_each_field_counts<Label>(by_literal, {
		[](Label &l) { l.literal = "other"; },
		[](Label &l) { l.text = text("test.z"); },
	});
}

TEST_CASE("ViewButton・StageItem・LocationLine・BodyLocation の等値比較は、すべてのフィールドを比べる")
{
	check_each_field_counts<ViewButton>(button("a"), {
		[](ViewButton &b) { b.id = ActionId::ToggleRecording; },
		[](ViewButton &b) { b.label = text("b"); },
		[](ViewButton &b) { b.args.user_id = "<user_id>"; },
		[](ViewButton &b) { b.disabled_reason = text("why"); },
		[](ViewButton &b) { b.primary = true; },
	});
	StageItem item{text("n")};
	check_each_field_counts<StageItem>(item, {
		[](StageItem &s) { s.text = text("n2"); },
	});
	LocationLine line{"C:/x", true};
	check_each_field_counts<LocationLine>(line, {
		[](LocationLine &l) { l.path = "C:/y"; },
		[](LocationLine &l) { l.open_folder = false; },
		[](LocationLine &l) { l.kind = LocationKind::File; },
	});
	BodyLocation body_line{0, LocationLine{"C:/x", true}};
	check_each_field_counts<BodyLocation>(body_line, {
		[](BodyLocation &b) { b.after = 1; },
		[](BodyLocation &b) { b.line.path = "C:/y"; },
	});
}

TEST_CASE("FirstLine と Notice の等値比較は、すべてのフィールドを比べる")
{
	check_each_field_counts<FirstLine>(FirstLine{}, {
		[](FirstLine &f) { f.kind = FirstLineKind::Notice; },
		[](FirstLine &f) { f.tier = Tier::L1; },
		[](FirstLine &f) { f.forms.push_back(text("long")); },
		[](FirstLine &f) { f.more_count = text("more"); },
		[](FirstLine &f) { f.button = button("enable"); },
		[](FirstLine &f) { f.notice_key = "Rule:1"; },
	});
	check_each_field_counts<Notice>(Notice{}, {
		[](Notice &n) { n.key = "k"; },
		[](Notice &n) { n.rule_name = "Rule"; },
		[](Notice &n) { n.log_subject = "slot 1"; },
		[](Notice &n) { n.tier = Tier::L1; },
		[](Notice &n) { n.log_level = LogLevel::Error; },
		[](Notice &n) { n.settle = Settle::Debounced; },
		[](Notice &n) { n.headline = text("h"); },
		[](Notice &n) { n.headline_short = text("hs"); },
		[](Notice &n) { n.body.push_back(text("b")); },
		[](Notice &n) { n.body_locations.push_back(BodyLocation{0, LocationLine{"C:/x", true}}); },
		[](Notice &n) { n.body_group_starts.push_back(1); },
		[](Notice &n) { n.location = LocationLine{"C:/x", true}; },
		[](Notice &n) { n.line2 = text("l2"); },
		[](Notice &n) { n.stages.push_back(StageItem{text("n")}); },
		[](Notice &n) { n.tip = text("t"); },
		[](Notice &n) { n.buttons.push_back(button("b")); },
		[](Notice &n) { n.appeared_at_us = 1; },
	});
}

TEST_CASE("PersonRow と、その中の型の等値比較は、すべてのフィールドを比べる")
{
	check_each_field_counts<RowStatus>(RowStatus{}, {
		[](RowStatus &s) { s.state = RowState::NoVideo; },
		[](RowStatus &s) { s.text = text("t"); },
		[](RowStatus &s) { s.settle = Settle::ClearOnly; },
		[](RowStatus &s) { s.tip = text("tip"); },
	});
	check_each_field_counts<VoiceChoice>(VoiceChoice{}, {
		[](VoiceChoice &v) { v.label = Label::from_literal("x"); },
		[](VoiceChoice &v) { v.checked = true; },
		[](VoiceChoice &v) { v.action = button("a"); },
	});
	check_each_field_counts<RowMenuView>(RowMenuView{}, {
		[](RowMenuView &m) { m.auto_add_only = true; },
		[](RowMenuView &m) { m.always_on = true; },
		[](RowMenuView &m) { m.always_on_toggle = button("t"); },
		[](RowMenuView &m) { m.focus = true; },
		[](RowMenuView &m) { m.focus_toggle = button("f"); },
		[](RowMenuView &m) { m.rename = button("r"); },
		[](RowMenuView &m) { m.name_override = "n"; },
		[](RowMenuView &m) { m.voices.push_back(VoiceChoice{}); },
		[](RowMenuView &m) { m.voices_disabled_label = text("vl"); },
		[](RowMenuView &m) { m.voices_disabled_reason = text("vr"); },
	});
	check_each_field_counts<PersonRow>(PersonRow{}, {
		[](PersonRow &r) { r.user_id = "<user_id>"; },
		[](PersonRow &r) { r.name = Label::from_literal("n"); },
		[](PersonRow &r) { r.checkable = false; },
		[](PersonRow &r) { r.checked = true; },
		[](PersonRow &r) { r.slot = 1; },
		[](PersonRow &r) { r.check_enabled = false; },
		[](PersonRow &r) { r.check_action = button("c"); },
		[](PersonRow &r) { r.sharing = true; },
		[](PersonRow &r) { r.status = RowStatus{}; },
		[](PersonRow &r) { r.quality = text("q"); },
		[](PersonRow &r) { r.resolution = text("r"); },
		[](PersonRow &r) { r.line2_parts.push_back(text("p")); },
		[](PersonRow &r) { r.same_discord_name_exists = true; },
		[](PersonRow &r) { r.same_visible_name_exists = true; },
		[](PersonRow &r) { r.username = std::string("u"); },
		[](PersonRow &r) { r.menu.always_on = true; },
		[](PersonRow &r) { r.refused_note = text("no"); },
		[](PersonRow &r) { r.position = SlotPosition::Full; },
		[](PersonRow &r) { r.is_main = true; },
		[](PersonRow &r) { r.auto_add = true; },
		[](PersonRow &r) { r.mic = MicState::Muted; },
		[](PersonRow &r) { r.speaker = SpeakerState::Muted; },
		[](PersonRow &r) { r.check_replaces = true; },
		[](PersonRow &r) { r.replace_choices.push_back(ReplaceChoice{}); },
		[](PersonRow &r) { r.off = true; },
	});
}

TEST_CASE("音声タブの View の等値比較は、すべてのフィールドを比べる")
{
	check_each_field_counts<AudioLine>(AudioLine{}, {
		[](AudioLine &l) { l.control = true; },
		[](AudioLine &l) { l.note = text("n"); },
		[](AudioLine &l) { l.mute_shown = true; },
		[](AudioLine &l) { l.mute_enabled = true; },
		[](AudioLine &l) { l.muted = true; },
		[](AudioLine &l) { l.volume_pct = 150; },
	});
	check_each_field_counts<AudioRowView>(AudioRowView{}, {
		[](AudioRowView &r) { r.user_id = "<user_id>"; },
		[](AudioRowView &r) { r.name = Label::from_literal("n"); },
		[](AudioRowView &r) { r.is_main = true; },
		[](AudioRowView &r) { r.off = true; },
		[](AudioRowView &r) { r.voice.muted = true; },
		[](AudioRowView &r) { r.screen.volume_pct = 20; },
	});
	check_each_field_counts<AudioTabView>(AudioTabView{}, {
		[](AudioTabView &v) { v.rows.push_back(AudioRowView{}); },
		[](AudioTabView &v) { v.notice = text("n"); },
	});
}

TEST_CASE("一覧の上の行・空の一覧・録画のボタン・メニューの等値比較は、すべてのフィールドを比べる")
{
	check_each_field_counts<ListReason>(ListReason{}, {
		[](ListReason &r) { r.text = text("t"); },
		[](ListReason &r) { r.short_text = text("s"); },
	});
	check_each_field_counts<EmptyList>(EmptyList{}, {
		[](EmptyList &e) { e.kind = EmptyKind::Alone; },
		[](EmptyList &e) { e.text = text("t"); },
		[](EmptyList &e) { e.button = button("b"); },
	});
	check_each_field_counts<RecordButtonView>(RecordButtonView{}, {
		[](RecordButtonView &r) { r.state = RecordButtonState::Stopping; },
		[](RecordButtonView &r) { r.label = text("l"); },
		[](RecordButtonView &r) { r.name = text("n"); },
		[](RecordButtonView &r) { r.pressed_look = true; },
		[](RecordButtonView &r) { r.action = button("a"); },
		[](RecordButtonView &r) { r.width_labels.push_back(text("w")); },
	});
	check_each_field_counts<DockMenuView>(DockMenuView{}, {
		[](DockMenuView &m) { m.open_settings = button("s"); },
		[](DockMenuView &m) { m.open_participants = button("p"); },
		[](DockMenuView &m) { m.export_bundle = button("e"); },
		[](DockMenuView &m) { m.feature_toggle = button("f"); },
	});
}

TEST_CASE("ダイアログの View の等値比較は、すべてのフィールドを比べる")
{
	check_each_field_counts<ParticipantRow>(ParticipantRow{}, {
		[](ParticipantRow &p) { p.user_id = "<user_id>"; },
		[](ParticipantRow &p) { p.name = Label::from_literal("n"); },
		[](ParticipantRow &p) { p.in_call = true; },
		[](ParticipantRow &p) { p.last_seen_at_us = 5; },
		[](ParticipantRow &p) { p.always_on = true; },
		[](ParticipantRow &p) { p.always_on_toggle = button("t"); },
		[](ParticipantRow &p) { p.name_override = "o"; },
		[](ParticipantRow &p) { p.rename = button("r"); },
		[](ParticipantRow &p) { p.voices.push_back(VoiceChoice{}); },
		[](ParticipantRow &p) { p.roster_shown = false; },
		[](ParticipantRow &p) { p.roster_toggle = button("s"); },
	});
	check_each_field_counts<VersionRow>(VersionRow{}, {
		[](VersionRow &v) { v.label = text("l"); },
		[](VersionRow &v) { v.value = Label::from_literal("1.0"); },
	});
	check_each_field_counts<ExportResultView>(ExportResultView{}, {
		[](ExportResultView &e) { e.text = text("t"); },
		[](ExportResultView &e) { e.location = LocationLine{"C:/a.zip", true}; },
	});
	check_each_field_counts<AboutPageView>(AboutPageView{}, {
		[](AboutPageView &a) { a.versions.push_back(VersionRow{}); },
		[](AboutPageView &a) { a.vesktop_rewrote_at_startup = text("w"); },
		[](AboutPageView &a) { a.export_result = ExportResultView{}; },
		[](AboutPageView &a) { a.export_button = button("e"); },
		[](AboutPageView &a) { a.uninstall_button = button("u"); },
		[](AboutPageView &a) { a.settings_locked = true; },
	});
	check_each_field_counts<LeftoverGroup>(LeftoverGroup{}, {
		[](LeftoverGroup &g) { g.heading = text("h"); },
		[](LeftoverGroup &g) { g.items.push_back(Label::from_literal("i")); },
		[](LeftoverGroup &g) { g.item_locations.push_back({LocationLine{"C:/x", true}}); },
		[](LeftoverGroup &g) { g.next = text("next"); },
	});
	check_each_field_counts<UninstallResultView>(UninstallResultView{}, {
		[](UninstallResultView &u) { u.kind = UninstallResultKind::Stopped; },
		[](UninstallResultView &u) { u.loose = true; },
		[](UninstallResultView &u) { u.main = text("m"); },
		[](UninstallResultView &u) { u.notes.push_back(text("n")); },
		[](UninstallResultView &u) { u.folder_to_delete = LocationLine{"C:/x", true}; },
		[](UninstallResultView &u) { u.record_dir = LocationLine{"C:/r", true}; },
		[](UninstallResultView &u) { u.groups.push_back(LeftoverGroup{}); },
	});
}

TEST_CASE("ReplaceChoice のすべてのフィールドを比べる")
{
	const ReplaceChoice base;
	check_each_field_counts<ReplaceChoice>(base, {
		[](ReplaceChoice &c) { c.name = Label::from_literal("x"); },
		[](ReplaceChoice &c) { c.position = SlotPosition::TopLeft; },
		[](ReplaceChoice &c) { c.action.primary = true; },
	});
}

TEST_CASE("AccountRow と SubAccountView のすべてのフィールドを比べる")
{
	const SubAccountView base;
	check_each_field_counts<SubAccountView>(base, {
		[](SubAccountView &v) { v.row.state = AccountCallState::NoCall; },
		[](SubAccountView &v) { v.row.call_warn = true; },
		[](SubAccountView &v) { v.move.primary = true; },
		[](SubAccountView &v) { v.leave.primary = true; },
	});
}

TEST_CASE("DockViewV2 の等値比較は、すべてのフィールドを比べる")
{
	check_each_field_counts<DockViewV2>(DockViewV2{}, {
		[](DockViewV2 &v) { v.first_line.notice_key = "k"; },
		[](DockViewV2 &v) { v.fallback_line.kind = FirstLineKind::Working; },
		[](DockViewV2 &v) { v.read_line.kind = FirstLineKind::Working; },
		[](DockViewV2 &v) { v.people_list.pinned = PersonRow{}; },
		[](DockViewV2 &v) { v.people_list.sharing.push_back(PersonRow{}); },
		[](DockViewV2 &v) { v.people_list.in_call.push_back(PersonRow{}); },
		[](DockViewV2 &v) { v.people_list.slots_used = 3; },
		[](DockViewV2 &v) { v.people_list.slots_total = 3; },
		[](DockViewV2 &v) { v.people_list.slots_label.id = "x"; },
		[](DockViewV2 &v) { v.people_list.sharing_heading.id = "x"; },
		[](DockViewV2 &v) { v.people_list.in_call_heading.id = "x"; },
		[](DockViewV2 &v) { v.audio_tab.rows.push_back(AudioRowView{}); },
		[](DockViewV2 &v) { v.audio_tab.notice = TextRef{"x", {}}; },
		[](DockViewV2 &v) { v.accounts.main.row.state = AccountCallState::NoCall; },
		[](DockViewV2 &v) { v.accounts.main.row.name = Label::from_literal("x"); },
		[](DockViewV2 &v) { v.accounts.main.row.call = Label::from_literal("x"); },
		[](DockViewV2 &v) { v.accounts.main.row.call_warn = true; },
		[](DockViewV2 &v) { v.accounts.main.change.primary = true; },
		[](DockViewV2 &v) { v.accounts.main.choices.push_back(MainAccountChoice{}); },
		[](DockViewV2 &v) { v.accounts.main.no_main.checked = true; },
		[](DockViewV2 &v) { v.accounts.main.no_main.in_call = true; },
		[](DockViewV2 &v) { v.accounts.main.no_main.label = Label::from_literal("x"); },
		[](DockViewV2 &v) { v.accounts.main.no_main.action.primary = true; },
		[](DockViewV2 &v) { v.accounts.sub = SubAccountView{}; },
		[](DockViewV2 &v) { v.notices.push_back(Notice{}); },
		[](DockViewV2 &v) { v.list_reason = ListReason{}; },
		[](DockViewV2 &v) { v.empty = EmptyList{}; },
		[](DockViewV2 &v) { v.people.push_back(PersonRow{}); },
		[](DockViewV2 &v) { v.record.pressed_look = true; },
		[](DockViewV2 &v) { v.menu.feature_toggle = button("f"); },
		[](DockViewV2 &v) { v.participants.push_back(ParticipantRow{}); },
		[](DockViewV2 &v) { v.about.settings_locked = true; },
		[](DockViewV2 &v) { v.status_tab.composite_present = true; },
		[](DockViewV2 &v) { v.uninstall_result = UninstallResultView{}; },
		[](DockViewV2 &v) { v.list_full = true; },
	});
}

TEST_CASE("状態タブの View の等値比較は、すべてのフィールドを比べる")
{
	check_each_field_counts<StatusTabView>(StatusTabView{}, {
		[](StatusTabView &v) { v.composite_present = true; },
		[](StatusTabView &v) { v.rows.push_back(StatusReceiveRow{}); },
		[](StatusTabView &v) { v.reset = button("reset"); },
		[](StatusTabView &v) { v.recording.section = StatusRecordingSection::Recording; },
		[](StatusTabView &v) { v.history.push_back(StatusHistoryLine{}); },
	});
	check_each_field_counts<StatusReceiveRow>(StatusReceiveRow{}, {
		[](StatusReceiveRow &r) { r.user_id = "u"; },
		[](StatusReceiveRow &r) { r.name = Label::from_literal("n"); },
		[](StatusReceiveRow &r) { r.gap_count = 1; },
		[](StatusReceiveRow &r) { r.gap_total_us = 1; },
		[](StatusReceiveRow &r) { r.streams.emplace_back(); },
	});
	check_each_field_counts<StatusRecordingView>(StatusRecordingView{}, {
		[](StatusRecordingView &r) { r.section = StatusRecordingSection::Last; },
		[](StatusRecordingView &r) { r.last = text("last"); },
		[](StatusRecordingView &r) { r.folder = "C:/x"; },
	});
	check_each_field_counts<StatusHistoryLine>(StatusHistoryLine{}, {
		[](StatusHistoryLine &l) { l.when = "w"; },
		[](StatusHistoryLine &l) { l.text = text("t"); },
		[](StatusHistoryLine &l) { l.subject = text("s"); },
	});
	cordmux::pipeline::ArrivalWindow a;
	a.delays_ms = {1, 2};
	cordmux::pipeline::ArrivalWindow b = a;
	CHECK(a == b);
	b.delays_ms[1] = 3;
	CHECK(a != b);
	b = a;
	b.mean_ms = 1.0;
	CHECK(a != b);
	b = a;
	b.max_ms = 2.0;
	CHECK(a != b);
}

TEST_CASE("既定の値は、メッセージのレベルが Tier::L3、待ちが Immediate、録画のボタンが Idle、1 行目が平常の要約である")
{
	const Notice n{};
	CHECK(n.tier == Tier::L3);
	CHECK(n.log_level == LogLevel::Info);
	CHECK(n.settle == Settle::Immediate);
	CHECK(n.appeared_at_us == 0);
	const DockViewV2 v{};
	CHECK(v.first_line.kind == FirstLineKind::Summary);
	CHECK(v.first_line.forms.empty());
	CHECK(v.record.state == RecordButtonState::Idle);
	CHECK_FALSE(v.list_full);
	const PersonRow r{};
	CHECK(r.checkable);
	CHECK(r.check_enabled);
	const ViewButton b{};
	CHECK(b.id == ActionId::ExportBundle);
	CHECK_FALSE(b.disabled_reason.has_value());
}

TEST_CASE("日時の引数は、マーカーと種類の 1 字と 10 進のマイクロ秒で、読み戻せる")
{
	const std::string t = time_arg(1700000000123456);
	const std::string d = date_time_arg(1700000000123456);
	CHECK(t == std::string(1, kTimeArgMark) + "T1700000000123456");
	CHECK(d == std::string(1, kTimeArgMark) + "D1700000000123456");
	CHECK(t[0] == static_cast<char>(0x1F));

	TimeArgKind kind = TimeArgKind::DateTime;
	std::int64_t us = 0;
	REQUIRE(parse_time_arg(t, &kind, &us));
	CHECK(kind == TimeArgKind::Time);
	CHECK(us == 1700000000123456);
	REQUIRE(parse_time_arg(d, &kind, &us));
	CHECK(kind == TimeArgKind::DateTime);
	CHECK(us == 1700000000123456);

	// 端の値 (0、負、int64 の端) も読み戻せる。
	for (const std::int64_t v :
	     {std::int64_t{0}, std::int64_t{-1}, std::numeric_limits<std::int64_t>::max(),
	      std::numeric_limits<std::int64_t>::min()}) {
		CAPTURE(v);
		REQUIRE(parse_time_arg(time_arg(v), &kind, &us));
		CHECK(us == v);
	}
	CHECK(parse_time_arg(time_arg(5), nullptr, nullptr));
}

TEST_CASE("日時のマーカーがないか形が違う引数は、日時として読まない (文字列のまま表示する)")
{
	const std::string mark(1, kTimeArgMark);
	for (const std::string &bad : {std::string("T123"), std::string("123"), std::string(""), mark, mark + "T",
				       mark + "X123", mark + "t123", mark + "T12a", mark + "T 12", mark + "T+12",
				       mark + "T99999999999999999999", mark + "D", std::string("cordmux")}) {
		CAPTURE(bad);
		CHECK_FALSE(parse_time_arg(bad, nullptr, nullptr));
	}
}
