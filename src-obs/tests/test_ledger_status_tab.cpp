// SPDX-License-Identifier: Apache-2.0

// 状態タブの判断 (ledger/status_tab.h) の単体テスト。映像途切れの累計の増え方、配信の開始とリセット、抜けた人の行、履歴の
// 出来事の種類と上限と順序、最後の録画、状態タブの View の組み立て。

#include "doctest.h"

#include "ledger/status_tab.h"

#include "ledger/natural_order.h"
#include "ledger/rules.h"
#include "ledger/text_ids.h"
#include "ledger_dock_test_helpers.h"

#include <string>
#include <vector>

using namespace ledger_dock_test;
namespace keys = cordmux::ledger::text::keys;

namespace {

// 映像途切れが出ている人の行を 1 つ持つ View。
dockview::DockViewV2 view_with_stalled(const std::vector<std::string> &stalled_ids)
{
	dockview::DockViewV2 v;
	for (const std::string &id : stalled_ids) {
		dockview::PersonRow row;
		row.user_id = id;
		row.name = Label::from_literal("name-" + id);
		dockview::RowStatus status;
		status.state = dockview::RowState::NoVideo;
		status.text.id = keys::kDockRowNotReceivingStatus;
		row.status = status;
		v.people.push_back(row);
	}
	return v;
}

StatusInput input_at(std::int64_t sec, bool streaming = false)
{
	StatusInput in;
	in.now_us = kNow + sec * kSec;
	in.utc_offset_sec = 9 * 3600;
	in.streaming = streaming;
	return in;
}

const dockview::StatusReceiveRow *row_in(const dockview::StatusTabView &v, const std::string &id)
{
	for (const dockview::StatusReceiveRow &r : v.rows) {
		if (r.user_id == id)
			return &r;
	}
	return nullptr;
}

StatusResult settle(const StatusState &prev, const dockview::DockViewV2 &stable, const Observation &obs,
		    const StatusInput &in)
{
	return settle_status(prev, stable, obs, in);
}

} // namespace

TEST_CASE("映像途切れの判定は、参加者の行に「映像が届いていません」が出る条件と同じである")
{
	// 枠に載っていて、最後のフレームから 5 秒たっている人は、行が「映像が届いていません」になる。
	Observation obs = healthy();
	obs.people[0].last_video_frame_at_us = kNow - 5 * kSec;
	dockview::DockViewV2 v = decide_dock(obs);
	REQUIRE(row_of(v, "100") != nullptr);
	REQUIRE(row_of(v, "100")->status.has_value());
	CHECK(row_of(v, "100")->status->text.id == keys::kDockRowNotReceivingStatus);
	const std::vector<StalledPerson> stalled = stalled_people(v);
	REQUIRE(stalled.size() == 1);
	CHECK(stalled[0].user_id == "100");
	CHECK(stalled[0].name.literal == "taro");

	// フレームが届いている間は入らない。
	obs.people[0].last_video_frame_at_us = kNow - kSec / 10;
	CHECK(stalled_people(decide_dock(obs)).empty());

	// 「映像を待っています」(まだ 1 枚も届いていない) は途切れではない。
	obs.people[0].last_video_frame_at_us.reset();
	v = decide_dock(obs);
	REQUIRE(row_of(v, "100")->status.has_value());
	CHECK(row_of(v, "100")->status->state == dockview::RowState::Waiting);
	CHECK(stalled_people(v).empty());
}

TEST_CASE("同じ NoVideo でも、自分の枠の映像が見つからない行は映像途切れに数えない")
{
	dockview::DockViewV2 v = view_with_stalled({"1"});
	v.people[0].status->text.id = keys::kDockRowSelfVideoMissingStatus;
	CHECK(stalled_people(v).empty());
}

TEST_CASE("累計: 出ていない から 出ている に変わるたびに回数が 1 増え、出ている間の時間が合計に足される")
{
	StallTally t;
	const auto one = std::vector<StalledPerson>{StalledPerson{"1", Label::from_literal("a")}};
	const std::vector<StalledPerson> none;

	t = step_stall_tally(t, none, 0, false);
	CHECK(t.entries.empty());

	// 1 回目: 10 秒から 14 秒。続いている間は、今までの分を足して見せる。
	t = step_stall_tally(t, one, 10 * kSec, false);
	REQUIRE(t.entries.size() == 1);
	CHECK(t.entries[0].count == 1);
	CHECK(stall_total_us(t.entries[0], 10 * kSec) == 0);
	t = step_stall_tally(t, one, 12 * kSec, false);
	CHECK(t.entries[0].count == 1);
	CHECK(stall_total_us(t.entries[0], 12 * kSec) == 2 * kSec);
	t = step_stall_tally(t, none, 14 * kSec, false);
	CHECK(t.entries[0].count == 1);
	CHECK(stall_total_us(t.entries[0], 20 * kSec) == 4 * kSec);

	// 2 回目: 30 秒から 31 秒。
	t = step_stall_tally(t, one, 30 * kSec, false);
	CHECK(t.entries[0].count == 2);
	t = step_stall_tally(t, none, 31 * kSec, false);
	CHECK(t.entries[0].count == 2);
	CHECK(stall_total_us(t.entries[0], 40 * kSec) == 5 * kSec);
}

TEST_CASE("累計: 途切れたまま行から消えた人は、その時刻で途切れを閉じ、回数と合計を残す")
{
	StallTally t;
	t = step_stall_tally(t, {StalledPerson{"1", Label::from_literal("a")}}, 10 * kSec, false);
	t = step_stall_tally(t, {}, 13 * kSec, false);
	REQUIRE(t.entries.size() == 1);
	CHECK(t.entries[0].count == 1);
	CHECK_FALSE(t.entries[0].since_us.has_value());
	CHECK(stall_total_us(t.entries[0], 100 * kSec) == 3 * kSec);
}

TEST_CASE("累計: リセットで消え、リセットの時点で途切れていた人は、その途切れが終わるまで数えない")
{
	StallTally t;
	const std::vector<StalledPerson> a{StalledPerson{"1", Label::from_literal("a")}};
	const std::vector<StalledPerson> b{StalledPerson{"2", Label::from_literal("b")}};
	t = step_stall_tally(t, a, 10 * kSec, false);
	t = step_stall_tally(t, {}, 12 * kSec, false);
	t = step_stall_tally(t, b, 20 * kSec, false);
	REQUIRE(t.entries.size() == 2);

	t = reset_stall_tally(t);
	// 途切れが終わっている 1 は消え、続いている 2 は数えない印を付けて残る。
	REQUIRE(t.entries.size() == 1);
	CHECK(t.entries[0].user_id == "2");
	CHECK(t.entries[0].count == 0);
	CHECK(t.entries[0].ignoring);

	// リセットの直後に「1 回」と出さない。続いている間は何も足さない。
	t = step_stall_tally(t, b, 25 * kSec, false);
	CHECK(t.entries[0].count == 0);
	CHECK(stall_total_us(t.entries[0], 25 * kSec) == 0);
	// 途切れが終わっても足さず、行は消える。
	t = step_stall_tally(t, {}, 30 * kSec, false);
	CHECK(t.entries.empty());
	// その次の途切れからは数える。
	t = step_stall_tally(t, b, 40 * kSec, false);
	REQUIRE(t.entries.size() == 1);
	CHECK(t.entries[0].count == 1);
}

TEST_CASE("累計: reset を真にして進めると、先にゼロに戻してから進める")
{
	StallTally t;
	const std::vector<StalledPerson> a{StalledPerson{"1", Label::from_literal("a")}};
	t = step_stall_tally(t, a, 10 * kSec, false);
	t = step_stall_tally(t, {}, 12 * kSec, false);
	REQUIRE(t.entries[0].count == 1);
	t = step_stall_tally(t, a, 20 * kSec, true);
	// 戻した後に出た途切れは、新しい 1 回として数える。
	REQUIRE(t.entries.size() == 1);
	CHECK(t.entries[0].count == 1);
	CHECK(stall_total_us(t.entries[0], 20 * kSec) == 0);
}

TEST_CASE("状態タブ: 配信していない間も途切れを数え、配信の開始でゼロに戻す")
{
	// 途切れが出ては消える人を、2 回繰り返す。
	StatusState state;
	const auto stalled = view_with_stalled({"100"});
	const auto fine = view_with_stalled({});
	const Observation obs = healthy();
	state = settle(state, stalled, obs, input_at(0)).next;
	state = settle(state, fine, obs, input_at(3)).next;
	state = settle(state, stalled, obs, input_at(10)).next;
	StatusResult r = settle(state, stalled, obs, input_at(14));
	state = r.next;
	const dockview::StatusReceiveRow *row = row_in(r.view, "100");
	REQUIRE(row != nullptr);
	CHECK(row->gap_count == 2);
	CHECK(row->gap_total_us == 3 * kSec + 4 * kSec);

	// 配信が始まった周期にゼロに戻る。途切れが続いているので、その途切れは数えない。
	r = settle(state, stalled, obs, input_at(20, true));
	state = r.next;
	CHECK(r.view.rows.empty());
	// 配信中に出る次の途切れから数える。
	state = settle(state, fine, obs, input_at(21, true)).next;
	r = settle(state, stalled, obs, input_at(30, true));
	REQUIRE(row_in(r.view, "100") != nullptr);
	CHECK(row_in(r.view, "100")->gap_count == 1);

	// 配信が続いている間は戻さない。
	r = settle(r.next, fine, obs, input_at(33, true));
	REQUIRE(row_in(r.view, "100") != nullptr);
	CHECK(row_in(r.view, "100")->gap_count == 1);
	CHECK(row_in(r.view, "100")->gap_total_us == 3 * kSec);
}

TEST_CASE("状態タブ: 通話を抜けた人の行は、累計が 0 でなければ残り、リセットで消える。並びは名前の自然順で値では変わらない")
{
	StatusState state;
	const Observation obs = healthy();
	dockview::DockViewV2 v = view_with_stalled({"10", "2"});
	v.people[0].name = Label::from_literal("hanako10");
	v.people[1].name = Label::from_literal("hanako2");
	state = settle(state, v, obs, input_at(0)).next;
	state = settle(state, view_with_stalled({}), obs, input_at(2)).next;
	// 2 人とも行から消えた (通話を抜けた)。累計は残り、名前は覚えている。
	StatusResult r = settle(state, dockview::DockViewV2{}, obs, input_at(5));
	state = r.next;
	REQUIRE(r.view.rows.size() == 2);
	CHECK(r.view.rows[0].user_id == "2");
	CHECK(r.view.rows[1].user_id == "10");
	CHECK(r.view.rows[0].name.literal == "hanako2");
	CHECK(r.view.rows[0].gap_count == 1);
	CHECK(r.view.rows[0].gap_total_us == 2 * kSec);

	// 値が変わっても並びは変わらない (10 の方が回数が多くなっても 2 が先)。
	state = settle(state, view_with_stalled({"10"}), obs, input_at(10)).next;
	state = settle(state, view_with_stalled({}), obs, input_at(11)).next;
	r = settle(state, view_with_stalled({"10"}), obs, input_at(20));
	state = r.next;
	REQUIRE(r.view.rows.size() == 2);
	CHECK(r.view.rows[0].user_id == "2");
	CHECK(r.view.rows[1].user_id == "10");
	CHECK(r.view.rows[1].gap_count == 3);

	// リセットで消える (続いている 10 は残るが、まだ数えない)。
	state = reset_status(state);
	r = settle(state, dockview::DockViewV2{}, obs, input_at(25));
	CHECK(r.view.rows.empty());
}

TEST_CASE("状態タブ: 直近 1 分の列を持つ行に累計を付け、列がなく累計だけの人の行も足す")
{
	Observation obs = healthy();
	cordmux::pipeline::ArrivalWindow w;
	w.delays_ms = {10, 20, 300};
	w.mean_ms = 110.0;
	w.max_ms = 300.0;
	obs.receive.arrival = {ArrivalFact{"100", {w}}, ArrivalFact{"200", {}}};
	dockview::DockViewV2 raw = decide_dock(obs);
	REQUIRE(raw.status_tab.composite_present);
	REQUIRE(raw.status_tab.rows.size() == 1);
	CHECK(raw.status_tab.rows[0].user_id == "100");
	CHECK(raw.status_tab.rows[0].name.literal == "taro");
	CHECK(raw.status_tab.rows[0].streams.size() == 1);
	CHECK(raw.status_tab.reset.id == ActionId::ResetStallTally);
	CHECK(raw.status_tab.reset.label.id == keys::kStatusResetButton);
	CHECK_FALSE(raw.status_tab.reset.disabled_reason.has_value());

	StatusState state;
	dockview::DockViewV2 stable = raw;
	stable.people.clear();
	StatusResult r = settle(state, view_with_stalled({"300"}), obs, input_at(0));
	state = r.next;
	r = settle(state, view_with_stalled({}), obs, input_at(4));
	state = r.next;
	stable.status_tab = raw.status_tab;
	r = settle(state, stable, obs, input_at(10));
	REQUIRE(r.view.rows.size() == 2);
	// 名前の自然順: hanako-ではなく name-300 と taro。
	const dockview::StatusReceiveRow *taro = row_in(r.view, "100");
	const dockview::StatusReceiveRow *gone = row_in(r.view, "300");
	REQUIRE(taro != nullptr);
	REQUIRE(gone != nullptr);
	CHECK(taro->gap_count == 0);
	CHECK(taro->streams.size() == 1);
	CHECK(gone->gap_count == 1);
	CHECK(gone->gap_total_us == 4 * kSec);
	CHECK(gone->streams.empty());
	CHECK(cordmux::ledger::natural_less(gone->name.literal, taro->name.literal));
	CHECK(r.view.rows[0].user_id == "300");
}

TEST_CASE("状態タブ: 直近 1 分の列は 2 秒ごとに書き換え、初めて現れた人は待たずに出し、途切れの累計は周期ごとに書き換える")
{
	const auto streams_of = [](std::uint16_t delay) {
		cordmux::pipeline::ArrivalWindow w;
		w.delays_ms = {delay};
		w.mean_ms = delay;
		w.max_ms = delay;
		return std::vector<cordmux::pipeline::ArrivalWindow>{w};
	};
	const auto stable_with = [&](const std::vector<std::string> &stalled, std::uint16_t a, std::optional<std::uint16_t> b) {
		dockview::DockViewV2 v = view_with_stalled(stalled);
		dockview::StatusReceiveRow row_a;
		row_a.user_id = "a";
		row_a.name = Label::from_literal("a");
		row_a.streams = streams_of(a);
		v.status_tab.composite_present = true;
		v.status_tab.rows.push_back(row_a);
		if (b.has_value()) {
			dockview::StatusReceiveRow row_b;
			row_b.user_id = "b";
			row_b.name = Label::from_literal("b");
			row_b.streams = streams_of(*b);
			v.status_tab.rows.push_back(row_b);
		}
		return v;
	};
	const Observation obs;
	const auto in_ms = [](std::int64_t ms) {
		StatusInput in;
		in.now_us = kNow + ms * 1000;
		return in;
	};
	const auto delay_of = [](const dockview::StatusTabView &v, const std::string &id) {
		const dockview::StatusReceiveRow *row = row_in(v, id);
		REQUIRE(row != nullptr);
		REQUIRE(!row->streams.empty());
		return row->streams[0].delays_ms[0];
	};

	StatusResult r = settle_status(StatusState{}, stable_with({}, 10, std::nullopt), obs, in_ms(0));
	CHECK(delay_of(r.view, "a") == 10);

	// 2 秒に満たない間は前の値のまま。
	r = settle_status(r.next, stable_with({}, 20, std::nullopt), obs, in_ms(500));
	CHECK(delay_of(r.view, "a") == 10);
	r = settle_status(r.next, stable_with({}, 30, std::nullopt), obs, in_ms(1999));
	CHECK(delay_of(r.view, "a") == 10);

	// その間に初めて現れた人の列は待たずに出す。次に書き換えるまでは、その値のまま。
	r = settle_status(r.next, stable_with({}, 30, std::uint16_t{70}), obs, in_ms(1999));
	CHECK(delay_of(r.view, "a") == 10);
	CHECK(delay_of(r.view, "b") == 70);
	r = settle_status(r.next, stable_with({}, 30, std::uint16_t{80}), obs, in_ms(1999));
	CHECK(delay_of(r.view, "b") == 70);

	// 途切れの累計は、列を書き換えない周期でも増える。
	r = settle_status(r.next, stable_with({"a"}, 30, std::uint16_t{80}), obs, in_ms(1999));
	REQUIRE(row_in(r.view, "a") != nullptr);
	CHECK(row_in(r.view, "a")->gap_count == 1);
	CHECK(delay_of(r.view, "a") == 10);

	// 2 秒たつと、全員の列を書き換える。
	r = settle_status(r.next, stable_with({}, 40, std::uint16_t{90}), obs, in_ms(2000));
	CHECK(delay_of(r.view, "a") == 40);
	CHECK(delay_of(r.view, "b") == 90);
}

TEST_CASE("状態タブ: 合成のソースがないときは、直近 1 分の行を作らず、reset と composite_present を持つ")
{
	Observation obs = healthy();
	obs.choices.composite_present = false;
	cordmux::pipeline::ArrivalWindow w;
	w.delays_ms = {1};
	obs.receive.arrival = {ArrivalFact{"100", {w}}};
	const dockview::DockViewV2 v = decide_dock(obs);
	CHECK_FALSE(v.status_tab.composite_present);
	CHECK(v.status_tab.rows.empty());
	CHECK(v.status_tab.reset.id == ActionId::ResetStallTally);
}

TEST_CASE("リセットの操作は、いつでも押せて、押すと ResetStallTally の仕事を返す")
{
	const Observation obs = healthy();
	const Intent intent = apply(obs, ActionId::ResetStallTally, ActionArgs{});
	CHECK(intent.can_run());
	CHECK(intent.kind == IntentKind::ResetStallTally);
}

// ---- 履歴 ----

namespace {

HistorySnapshot snapshot_with(std::vector<std::string> notice_keys, LinkState link, bool recording,
			      std::vector<std::pair<std::string, std::string>> slots = {})
{
	HistorySnapshot s;
	for (const std::string &k : notice_keys) {
		HistoryNotice n;
		n.key = k;
		n.headline.id = "headline." + k;
		s.notices.push_back(n);
	}
	s.link = link;
	s.recording = recording;
	for (const auto &[id, name] : slots)
		s.slots.push_back(HistorySlot{id, Label::from_literal(name)});
	return s;
}

} // namespace

TEST_CASE("履歴: メッセージの発生と解消を、全レベルで、見出しを添えて返す")
{
	const HistorySnapshot a = snapshot_with({"A", "B"}, LinkState::Connected, false);
	const HistorySnapshot b = snapshot_with({"B", "C"}, LinkState::Connected, false);
	const std::vector<HistoryEvent> events = history_events(a, b);
	REQUIRE(events.size() == 2);
	CHECK(events[0].text.id == keys::kStatusHistoryNoticeClearedStatus);
	REQUIRE(events[0].subject.has_value());
	CHECK(events[0].subject->id == "headline.A");
	CHECK(events[1].text.id == keys::kStatusHistoryNoticeAppearedStatus);
	CHECK(events[1].subject->id == "headline.C");
	// 変わらなければ何も返さない。
	CHECK(history_events(b, b).empty());
}

TEST_CASE("履歴: 最初の周期は、あるメッセージだけを発生として返し、接続・枠・録画は返さない")
{
	const HistorySnapshot first = snapshot_with({"A"}, LinkState::Connected, true, {{"1", "taro"}});
	const std::vector<HistoryEvent> events = history_events(std::nullopt, first);
	REQUIRE(events.size() == 1);
	CHECK(events[0].text.id == keys::kStatusHistoryNoticeAppearedStatus);
}

TEST_CASE("履歴: Vesktop との接続と切断")
{
	const HistorySnapshot never = snapshot_with({}, LinkState::NeverConnected, false);
	const HistorySnapshot up = snapshot_with({}, LinkState::Connected, false);
	const HistorySnapshot down = snapshot_with({}, LinkState::Disconnected, false);
	std::vector<HistoryEvent> e = history_events(never, up);
	REQUIRE(e.size() == 1);
	CHECK(e[0].text.id == keys::kStatusHistoryConnectedStatus);
	e = history_events(up, down);
	REQUIRE(e.size() == 1);
	CHECK(e[0].text.id == keys::kStatusHistoryDisconnectedStatus);
	// 一度も接続していない間の変化は出来事ではない。
	CHECK(history_events(snapshot_with({}, LinkState::Unknown, false), never).empty());
	CHECK(history_events(down, snapshot_with({}, LinkState::Unknown, false)).empty());
}

TEST_CASE("履歴: 枠に載せた人と外した人。枠の中身が同じなら何も返さない")
{
	const HistorySnapshot a = snapshot_with({}, LinkState::Connected, false, {{"1", "taro"}, {"2", "hanako"}});
	const HistorySnapshot b = snapshot_with({}, LinkState::Connected, false, {{"2", "hanako"}, {"3", "kenji"}});
	const std::vector<HistoryEvent> e = history_events(a, b);
	REQUIRE(e.size() == 2);
	CHECK(e[0].text.id == keys::kStatusHistorySlotRemovedStatus);
	CHECK(e[0].text.args == std::vector<std::string>{"taro"});
	CHECK(e[1].text.id == keys::kStatusHistorySlotAddedStatus);
	CHECK(e[1].text.args == std::vector<std::string>{"kenji"});
	// 順番が替わっただけでは返さない。
	const HistorySnapshot c = snapshot_with({}, LinkState::Connected, false, {{"3", "kenji"}, {"2", "hanako"}});
	CHECK(history_events(b, c).empty());
}

TEST_CASE("履歴: 受信をやり直した回数が増えた人に、1 行ずつ「受信をやり直しました」を書く。最初の周期と、増えないときは書かない")
{
	HistorySnapshot a = snapshot_with({}, LinkState::Connected, false, {{"1", "taro"}});
	HistorySnapshot b = a;
	b.restarts.push_back(HistoryRestart{"1", Label::from_literal("taro"), 1});
	std::vector<HistoryEvent> e = history_events(a, b);
	REQUIRE(e.size() == 1);
	CHECK(e[0].text.id == keys::kStatusHistoryReceiveRestartedStatus);
	CHECK(e[0].text.args == std::vector<std::string>{"taro"});
	CHECK(history_events(b, b).empty());
	CHECK(history_events(std::nullopt, b).empty());

	HistorySnapshot c = b;
	c.restarts[0].count = 2;
	e = history_events(b, c);
	REQUIRE(e.size() == 1);
	CHECK(e[0].text.id == keys::kStatusHistoryReceiveRestartedStatus);
}

TEST_CASE("履歴: history_snapshot_of は、一覧に行があり、受信をやり直した回数が 1 以上の人だけを入れる")
{
	dockview::DockViewV2 stable;
	dockview::PersonRow row;
	row.user_id = "1";
	row.name = Label::from_literal("taro");
	row.slot = 1;
	stable.people.push_back(row);
	Observation obs;
	PersonFact taro;
	taro.user_id = "1";
	taro.receive_restarts = 3;
	PersonFact gone;
	gone.user_id = "2";
	gone.receive_restarts = 1;
	obs.people = {taro, gone};
	const HistorySnapshot snap = history_snapshot_of(stable, obs);
	REQUIRE(snap.restarts.size() == 1);
	CHECK(snap.restarts[0].user_id == "1");
	CHECK(snap.restarts[0].count == 3);
}

TEST_CASE("履歴: 録画の開始と終了")
{
	const HistorySnapshot idle = snapshot_with({}, LinkState::Connected, false);
	const HistorySnapshot rec = snapshot_with({}, LinkState::Connected, true);
	std::vector<HistoryEvent> e = history_events(idle, rec);
	REQUIRE(e.size() == 1);
	CHECK(e[0].text.id == keys::kStatusHistoryRecordingStartedStatus);
	e = history_events(rec, idle);
	REQUIRE(e.size() == 1);
	CHECK(e[0].text.id == keys::kStatusHistoryRecordingStoppedStatus);
}

TEST_CASE("履歴の元になる状態: 待ちを通した後のメッセージ全レベルと、枠に載った人と、接続と録画を取る。人の行の状態は入れない")
{
	Observation obs = healthy();
	obs.people[0].last_video_frame_at_us = kNow - 5 * kSec; // 行が「映像が届いていません」になる
	obs.recording.phase = RecordPhase::Recording;
	const dockview::DockViewV2 v = decide_dock(obs);
	const HistorySnapshot s = history_snapshot_of(v, obs);
	CHECK(s.link == LinkState::Connected);
	CHECK(s.recording);
	REQUIRE(s.slots.size() == 1);
	CHECK(s.slots[0].user_id == "100");
	for (const HistoryNotice &n : s.notices)
		CHECK(n.key.rfind("row:", 0) != 0);
}

TEST_CASE("履歴: 新しいものが先頭で、最大 60 行。上限を超えた古い行から捨てる")
{
	StatusState state;
	const Observation obs = healthy();
	dockview::DockViewV2 v;
	// 周期ごとにメッセージを 1 つずつ足して、70 回の発生を作る。
	for (int i = 0; i < 70; ++i) {
		dockview::Notice n;
		n.key = "K" + std::to_string(i);
		n.headline.id = "h" + std::to_string(i);
		v.notices.push_back(n);
		state = settle(state, v, obs, input_at(i)).next;
	}
	REQUIRE(state.history.size() == dockview::kStatusHistoryMax);
	// 先頭は最後の発生、末尾は 11 番目の発生。
	REQUIRE(state.history.front().subject.has_value());
	CHECK(state.history.front().subject->id == "h69");
	CHECK(state.history.back().subject->id == "h10");
	// 時刻は新しい順 (先頭ほど遅い)。
	CHECK(state.history.front().when != state.history.back().when);
}

TEST_CASE("履歴: 変化がない周期は行を足さない")
{
	StatusState state;
	const Observation obs = healthy();
	const dockview::DockViewV2 v = decide_dock(obs);
	state = settle(state, v, obs, input_at(0)).next;
	const std::size_t n = state.history.size();
	for (int i = 1; i < 20; ++i)
		state = settle(state, v, obs, input_at(i)).next;
	CHECK(state.history.size() == n);
}

TEST_CASE("履歴: 同じ周期の複数の出来事は、古い順に入れて、後のものが先頭に来る")
{
	StatusState state;
	Observation obs = healthy();
	obs.link.state = LinkState::NeverConnected;
	state = settle(state, decide_dock(obs), obs, input_at(0)).next;
	state.history.clear();
	obs.link.state = LinkState::Connected;
	obs.recording.phase = RecordPhase::Recording;
	const StatusResult r = settle(state, decide_dock(obs), obs, input_at(1));
	REQUIRE(r.view.history.size() >= 2);
	// 録画の開始が先頭、接続がその次。
	CHECK(r.view.history[0].text.id == keys::kStatusHistoryRecordingStartedStatus);
	CHECK(r.view.history[1].text.id == keys::kStatusHistoryConnectedStatus);
}

// ---- 最後の録画 ----

TEST_CASE("録画の節: 録画中、最後の録画、まだ録画していない、の 3 つ")
{
	RecordingMemory m;
	CHECK(recording_section_of(m, kNow, 9 * 3600).section == dockview::StatusRecordingSection::None);

	Observation obs = healthy();
	obs.recording.phase = RecordPhase::Recording;
	obs.recording.enabled_since_us = kNow - 12 * 60 * kSec;
	obs.recording.session_dir = "D:/rec/2000-01-10 20-48-00";
	m = step_recording_memory(m, obs, kNow);
	CHECK(m.active);
	CHECK(recording_section_of(m, kNow, 9 * 3600).section == dockview::StatusRecordingSection::Recording);

	// 止めると、始めた時刻と長さと場所を覚える。
	obs.recording.phase = RecordPhase::Stopping;
	obs.recording.enabled_since_us.reset();
	m = step_recording_memory(m, obs, kNow);
	CHECK_FALSE(m.active);
	const dockview::StatusRecordingView v = recording_section_of(m, kNow, 9 * 3600);
	CHECK(v.section == dockview::StatusRecordingSection::Last);
	CHECK(v.last.id == keys::kStatusRecordingLastMinutesStatus);
	REQUIRE(v.last.args.size() == 2);
	CHECK(v.last.args[0] == dockview::time_arg(kNow - 12 * 60 * kSec));
	CHECK(v.last.args[1] == "12");
	CHECK(v.folder == "D:/rec/2000-01-10 20-48-00");
}

TEST_CASE("録画の節: 1 分に満たない録画と 1 時間以上の録画は別のテキストにし、今日でない日の録画には日付を付ける")
{
	RecordingMemory m;
	m.has_last = true;
	m.last_start_us = kNow - 100 * kSec;
	m.last_duration_us = 30 * kSec;
	CHECK(recording_section_of(m, kNow, 9 * 3600).last.id == keys::kStatusRecordingLastShortStatus);
	m.last_duration_us = 61 * 60 * kSec;
	const dockview::StatusRecordingView hours = recording_section_of(m, kNow, 9 * 3600);
	CHECK(hours.last.id == keys::kStatusRecordingLastHoursStatus);
	CHECK(hours.last.args[1] == "1");
	CHECK(hours.last.args[2] == "1");
	// 2 日前の録画。
	m.last_start_us = kNow - 2 * 24 * 3600 * kSec;
	const dockview::StatusRecordingView old = recording_section_of(m, kNow, 9 * 3600);
	CHECK(old.last.args[0] == dockview::date_time_arg(m.last_start_us));
	// 場所を覚えていなければ、ボタンを置かない。
	CHECK(old.folder.empty());
}

TEST_CASE("録画の節: 始めていない間は覚えず、2 回目の録画では最後の録画を置き換える")
{
	RecordingMemory m;
	Observation obs = healthy();
	obs.recording.phase = RecordPhase::Idle;
	obs.recording.session_dir = "D:/rec/previous-launch"; // 前の起動の録画のフォルダーは、この起動の録画ではない
	m = step_recording_memory(m, obs, kNow);
	CHECK_FALSE(m.has_last);
	CHECK(recording_section_of(m, kNow, 0).section == dockview::StatusRecordingSection::None);

	obs.recording.phase = RecordPhase::Recording;
	obs.recording.session_dir = "D:/rec/one";
	m = step_recording_memory(m, obs, kNow);
	obs.recording.phase = RecordPhase::Idle;
	m = step_recording_memory(m, obs, kNow + 5 * 60 * kSec);
	CHECK(m.last_folder == "D:/rec/one");
	CHECK(m.last_duration_us == 5 * 60 * kSec);

	obs.recording.phase = RecordPhase::Recording;
	obs.recording.session_dir = "D:/rec/two";
	m = step_recording_memory(m, obs, kNow + 10 * 60 * kSec);
	CHECK(m.active);
	// 録画中は、前の録画を覚えたまま。
	CHECK(m.last_folder == "D:/rec/one");
	obs.recording.phase = RecordPhase::Idle;
	m = step_recording_memory(m, obs, kNow + 20 * 60 * kSec);
	CHECK(m.last_folder == "D:/rec/two");
	CHECK(m.last_duration_us == 10 * 60 * kSec);
}

TEST_CASE("状態タブの View の録画の節と履歴は、settle_status の結果に入る")
{
	StatusState state;
	Observation obs = healthy();
	obs.recording.phase = RecordPhase::Recording;
	obs.recording.session_dir = "D:/rec/x";
	StatusResult r = settle(state, decide_dock(obs), obs, input_at(0));
	CHECK(r.view.recording.section == dockview::StatusRecordingSection::Recording);
	obs.recording.phase = RecordPhase::Idle;
	r = settle(r.next, decide_dock(obs), obs, input_at(90));
	CHECK(r.view.recording.section == dockview::StatusRecordingSection::Last);
	CHECK(r.view.recording.last.id == keys::kStatusRecordingLastMinutesStatus);
	CHECK(r.view.recording.folder == "D:/rec/x");
	CHECK(r.view.history.front().text.id == keys::kStatusHistoryRecordingStoppedStatus);
}
