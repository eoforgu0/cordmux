// SPDX-License-Identifier: Apache-2.0

// ledger/notice_tracker.h の単体テスト。時刻を進めながら、表示するまでの待ち (Debounced)、消すまでの待ち
// (Debounced と ClearOnly)、待たないメッセージ (Immediate)、表示した順の並び (同じメッセージのレベルの中では古い順で、
// 新しいものが下)、メッセージのレベルの順、1 行目の決め直し、表示と消去の記録 (diff)、人の行の状態の待ちと記録、
// 枠がすべて使用中になったか空いたかの通知を確認する。時計は読まない (now_us を渡す)。

#include "doctest.h"

#include "ledger/notice_tracker.h"
#include "ledger/text_ids.h"
#include "ledger_dock_test_helpers.h"

#include <string>
#include <vector>

using namespace ledger_dock_test;

namespace {

dockview::Notice notice(const std::string &key, dockview::Tier tier, dockview::Settle settle)
{
	dockview::Notice n;
	n.key = key;
	n.rule_name = key;
	n.tier = tier;
	n.log_level = rule_log_level(tier);
	n.settle = settle;
	n.headline.id = "test." + key;
	return n;
}

dockview::DockViewV2 view_with(const std::vector<dockview::Notice> &notices)
{
	dockview::DockViewV2 v;
	v.fallback_line.kind = dockview::FirstLineKind::Summary;
	v.fallback_line.forms = {TextRef{"test.summary", {}}};
	v.notices = notices;
	return v;
}

std::vector<std::string> keys_of(const dockview::DockViewV2 &v)
{
	std::vector<std::string> out;
	for (const dockview::Notice &n : v.notices)
		out.push_back(n.key);
	return out;
}

const NoticeTrackerConfig kCfg{2 * kSec, 1 * kSec};

} // namespace

TEST_CASE("Debounced のメッセージは show_delay の間続けて含まれてから表示し、途中で消えたら何も残さない")
{
	const dockview::Notice n = notice("Link", dockview::Tier::L2, dockview::Settle::Debounced);
	NoticeTrackerState state;
	StabilizeResult r = stabilize(state, view_with({n}), kNow, kCfg);
	CHECK(r.view.notices.empty());
	CHECK(r.diff.appeared.empty());
	r = stabilize(r.next, view_with({n}), kNow + 1999 * 1000, kCfg);
	CHECK(r.view.notices.empty());
	r = stabilize(r.next, view_with({n}), kNow + 2 * kSec, kCfg);
	REQUIRE(r.view.notices.size() == 1);
	CHECK(r.view.notices[0].appeared_at_us == kNow + 2 * kSec);
	REQUIRE(r.diff.appeared.size() == 1);
	CHECK(r.diff.appeared[0].rule_name == "Link");
	CHECK(r.diff.appeared[0].tier == dockview::Tier::L2);
	CHECK(r.diff.appeared[0].log_level == dockview::LogLevel::Warning);

	// 表示する前に消えたら、次に含まれたときは、待ちを数え直す。
	NoticeTrackerState s2;
	StabilizeResult a = stabilize(s2, view_with({n}), kNow, kCfg);
	a = stabilize(a.next, view_with({}), kNow + kSec, kCfg);
	CHECK(a.next.items.empty());
	CHECK(a.diff.disappeared.empty());
	a = stabilize(a.next, view_with({n}), kNow + 2 * kSec, kCfg);
	CHECK(a.view.notices.empty());
}

TEST_CASE("Debounced と ClearOnly のメッセージは clear_delay の間続けて消えてから消し、Immediate のメッセージはすぐ消す")
{
	for (const dockview::Settle settle : {dockview::Settle::Debounced, dockview::Settle::ClearOnly}) {
		CAPTURE(static_cast<int>(settle));
		const dockview::Notice n = notice("NoVideo", dockview::Tier::L1, settle);
		NoticeTrackerState state;
		StabilizeResult r = stabilize(state, view_with({n}), kNow, kCfg);
		r = stabilize(r.next, view_with({n}), kNow + 3 * kSec, kCfg);
		REQUIRE(r.view.notices.size() == 1);
		const std::int64_t appeared = r.view.notices[0].appeared_at_us;
		// 消えてから 1 秒たつまでは表示し続ける (表示した時刻も変えない)。
		r = stabilize(r.next, view_with({}), kNow + 4 * kSec, kCfg);
		REQUIRE(r.view.notices.size() == 1);
		CHECK(r.view.notices[0].appeared_at_us == appeared);
		CHECK(r.diff.disappeared.empty());
		// 待ちの間に戻れば、消えなかったことになる (表示した時刻も同じまま)。
		StabilizeResult back = stabilize(r.next, view_with({n}), kNow + 4 * kSec + 500 * 1000, kCfg);
		REQUIRE(back.view.notices.size() == 1);
		CHECK(back.view.notices[0].appeared_at_us == appeared);
		CHECK(back.diff.appeared.empty());
		// 続けて消えていたら消す。
		r = stabilize(r.next, view_with({}), kNow + 5 * kSec, kCfg);
		CHECK(r.view.notices.empty());
		REQUIRE(r.diff.disappeared.size() == 1);
		CHECK(r.diff.disappeared[0].rule_name == "NoVideo");
	}
	// ClearOnly は表示する前に待たない (規則のしきい値そのものが、表示するまでの待ちになっている)。
	{
		NoticeTrackerState state;
		const StabilizeResult r =
			stabilize(state, view_with({notice("NoVideo", dockview::Tier::L1, dockview::Settle::ClearOnly)}), kNow, kCfg);
		CHECK(r.view.notices.size() == 1);
	}
	// Immediate はすぐ表示して、すぐ消す。
	{
		const dockview::Notice n = notice("Frames", dockview::Tier::L2, dockview::Settle::Immediate);
		NoticeTrackerState state;
		StabilizeResult r = stabilize(state, view_with({n}), kNow, kCfg);
		CHECK(r.view.notices.size() == 1);
		CHECK(r.diff.appeared.size() == 1);
		r = stabilize(r.next, view_with({}), kNow + 1, kCfg);
		CHECK(r.view.notices.empty());
		CHECK(r.diff.disappeared.size() == 1);
	}
}

TEST_CASE("メッセージはメッセージのレベルの順に並べ、同じメッセージのレベルの中では表示した順に並べて、"
	  "表示中のメッセージの位置を変えない")
{
	const dockview::Notice a = notice("A", dockview::Tier::L2, dockview::Settle::Immediate);
	const dockview::Notice b = notice("B", dockview::Tier::L2, dockview::Settle::Immediate);
	const dockview::Notice c = notice("C", dockview::Tier::L1, dockview::Settle::Immediate);
	const dockview::Notice d = notice("D", dockview::Tier::L3, dockview::Settle::Immediate);
	NoticeTrackerState state;
	// B を先に表示する。
	StabilizeResult r = stabilize(state, view_with({b}), kNow, kCfg);
	// decide_dock() の並びでは A が B より前でも、後から表示した A は B の下になる (新しいものが下)。
	r = stabilize(r.next, view_with({a, b, d}), kNow + kSec, kCfg);
	CHECK(keys_of(r.view) == std::vector<std::string>{"B", "A", "D"});
	// メッセージのレベル 1 は 2 より上 (メッセージのレベルの順)。
	r = stabilize(r.next, view_with({a, b, c, d}), kNow + 2 * kSec, kCfg);
	CHECK(keys_of(r.view) == std::vector<std::string>{"C", "B", "A", "D"});
	// 同じ時刻に表示したものは、decide_dock() の並び (規則の表の順) のまま。
	NoticeTrackerState fresh;
	const StabilizeResult same = stabilize(fresh, view_with({a, b}), kNow, kCfg);
	CHECK(keys_of(same.view) == std::vector<std::string>{"A", "B"});
}

TEST_CASE("1 行目は待ちを通した後のメッセージから決め直し、メッセージのレベル 1 か 2 の先に表示した見出しと残りの件数にする")
{
	const dockview::Notice l1a = notice("L1a", dockview::Tier::L1, dockview::Settle::Immediate);
	const dockview::Notice l1b = notice("L1b", dockview::Tier::L1, dockview::Settle::Immediate);
	const dockview::Notice l2 = notice("L2", dockview::Tier::L2, dockview::Settle::Immediate);
	const dockview::Notice l3 = notice("L3", dockview::Tier::L3, dockview::Settle::Immediate);
	const dockview::Notice waiting = notice("Wait", dockview::Tier::L1, dockview::Settle::Debounced);

	NoticeTrackerState state;
	// メッセージのレベル 3 だけなら、1 行目は平常の行 (メッセージのレベル 3 は 1 行目に表示しない)。
	StabilizeResult r = stabilize(state, view_with({l3}), kNow, kCfg);
	CHECK(r.view.first_line.kind == dockview::FirstLineKind::Summary);
	// 待っている間のメッセージのレベル 1 は、1 行目に表示しない。
	r = stabilize(r.next, view_with({l3, waiting}), kNow + kSec, kCfg);
	CHECK(r.view.first_line.kind == dockview::FirstLineKind::Summary);
	// メッセージのレベル 2 を表示したら、その見出しにする。
	r = stabilize(r.next, view_with({l2, l3, waiting}), kNow + 2 * kSec, kCfg);
	CHECK(r.view.first_line.kind == dockview::FirstLineKind::Notice);
	REQUIRE(r.view.first_line.tier.has_value());
	CHECK(*r.view.first_line.tier == dockview::Tier::L2);
	CHECK(r.view.first_line.notice_key == "L2");
	CHECK_FALSE(r.view.first_line.more_count.has_value());
	// メッセージのレベル 1 を 2 件表示したら、先に表示したほうの見出しと「(他1件)」にする。
	r = stabilize(r.next, view_with({l1b, l2, l3}), kNow + 3 * kSec, kCfg);
	r = stabilize(r.next, view_with({l1a, l1b, l2, l3}), kNow + 4 * kSec, kCfg);
	REQUIRE(r.view.first_line.tier.has_value());
	CHECK(*r.view.first_line.tier == dockview::Tier::L1);
	CHECK(r.view.first_line.notice_key == "L1b");
	REQUIRE(r.view.first_line.forms.size() == 1);
	CHECK(r.view.first_line.forms[0].id == "test.L1b");
	REQUIRE(r.view.first_line.more_count.has_value());
	CHECK(r.view.first_line.more_count->id == cordmux::ledger::text::keys::kDockMoreNoticesStatus);
	CHECK(r.view.first_line.more_count->args == std::vector<std::string>{"1"});
}

TEST_CASE("1 行目の候補は見出しの全文、短い形の順で、待ちを通しても同じで、残りの件数は別に持つ")
{
	dockview::Notice first = notice("L1a", dockview::Tier::L1, dockview::Settle::Immediate);
	first.headline.args = {"13:41"};
	first.headline_short = TextRef{"test.L1a.short", {}};
	const dockview::Notice second = notice("L1b", dockview::Tier::L1, dockview::Settle::Immediate);
	dockview::Notice l2 = notice("L2", dockview::Tier::L2, dockview::Settle::Immediate);
	l2.headline_short = TextRef{"test.L2.short", {}};

	// 待ちを通す前の 1 行目: 全文、短い形の順。短い形は %N の値を持たないまま (全文の時刻をコピーしない)。
	dockview::FirstLine line = first_line_of({first, second}, dockview::FirstLine{});
	REQUIRE(line.forms.size() == 2);
	CHECK(line.forms[0].id == "test.L1a");
	CHECK(line.forms[0].args == std::vector<std::string>{"13:41"});
	CHECK(line.forms[1].id == "test.L1a.short");
	CHECK(line.forms[1].args.empty());
	// 残りの件数は候補の並びに混ぜない (どの候補を表示しても落とさない)。
	REQUIRE(line.more_count.has_value());
	CHECK(line.more_count->args == std::vector<std::string>{"1"});

	// 待ちを通した後も同じ (stabilize() は Notice の短い形をそのまま渡す)。
	NoticeTrackerState state;
	StabilizeResult r = stabilize(state, view_with({first, second, l2}), kNow, kCfg);
	REQUIRE(r.view.first_line.forms.size() == 2);
	CHECK(r.view.first_line.forms[0].id == "test.L1a");
	CHECK(r.view.first_line.forms[1].id == "test.L1a.short");
	CHECK(r.view.first_line.notice_key == "L1a");

	// メッセージのレベル 1 が消えると、メッセージのレベル 2 の見出しと短い形になる (残りの件数はない)。
	r = stabilize(r.next, view_with({l2}), kNow + 10 * kSec, kCfg);
	REQUIRE(r.view.first_line.forms.size() == 2);
	CHECK(r.view.first_line.forms[0].id == "test.L2");
	CHECK(r.view.first_line.forms[1].id == "test.L2.short");
	CHECK_FALSE(r.view.first_line.more_count.has_value());

	// 短い形のないメッセージは全文 1 つ (省略は、Qt の末尾の省略と safe_cut のガードに任せる)。
	line = first_line_of({second}, dockview::FirstLine{});
	REQUIRE(line.forms.size() == 1);
	CHECK(line.forms[0].id == "test.L1b");
}

TEST_CASE("人の行の状態は種類ごとに待ちを通して表示と消去を記録し、ログの対象は枠の番号で示す")
{
	dockview::DockViewV2 v = view_with({});
	dockview::PersonRow row;
	row.user_id = "100";
	row.slot = 2;
	dockview::RowStatus noviddeo;
	noviddeo.state = dockview::RowState::NoVideo;
	noviddeo.text.id = "cordmux.dock.row_not_receiving.status";
	noviddeo.settle = dockview::Settle::ClearOnly;
	row.status = noviddeo;
	v.people = {row};

	NoticeTrackerState state;
	StabilizeResult r = stabilize(state, v, kNow, kCfg);
	REQUIRE(r.view.people.size() == 1);
	REQUIRE(r.view.people[0].status.has_value());
	CHECK(r.view.people[0].status->state == dockview::RowState::NoVideo);
	REQUIRE(r.diff.appeared.size() == 1);
	CHECK(r.diff.appeared[0].is_row);
	CHECK(r.diff.appeared[0].rule_name == "RowStatus.NoVideo");
	CHECK(r.diff.appeared[0].log_subject == "slot 2");
	CHECK(r.diff.appeared[0].key.find("100") != std::string::npos); // キーは周期をまたいで見分けるためだけに使う
	CHECK(r.diff.appeared[0].tier == dockview::Tier::L3);

	// 映像が戻った (decide_dock() の結果から状態が消えた): 消すまでの待ちの間は、状態を表示し続ける。
	dockview::DockViewV2 ok = v;
	ok.people[0].status.reset();
	r = stabilize(r.next, ok, kNow + 500 * 1000, kCfg);
	REQUIRE(r.view.people[0].status.has_value());
	CHECK(r.diff.disappeared.empty());
	r = stabilize(r.next, ok, kNow + 1600 * 1000, kCfg);
	CHECK_FALSE(r.view.people[0].status.has_value());
	REQUIRE(r.diff.disappeared.size() == 1);
	CHECK(r.diff.disappeared[0].rule_name == "RowStatus.NoVideo");

	// 状態が替わったら、前の状態が消えて新しい状態を表示する (それぞれを記録する)。Immediate の状態は待たない。
	dockview::DockViewV2 waiting = v;
	waiting.people[0].status->state = dockview::RowState::Waiting;
	waiting.people[0].status->settle = dockview::Settle::Immediate;
	dockview::DockViewV2 not_sharing = v;
	not_sharing.people[0].status->state = dockview::RowState::NotSharing;
	not_sharing.people[0].status->settle = dockview::Settle::Immediate;
	NoticeTrackerState s2;
	StabilizeResult w = stabilize(s2, waiting, kNow, kCfg);
	w = stabilize(w.next, not_sharing, kNow + kSec, kCfg);
	REQUIRE(w.view.people[0].status.has_value());
	CHECK(w.view.people[0].status->state == dockview::RowState::NotSharing);
	REQUIRE(w.diff.appeared.size() == 1);
	CHECK(w.diff.appeared[0].rule_name == "RowStatus.NotSharing");
	REQUIRE(w.diff.disappeared.size() == 1);
	CHECK(w.diff.disappeared[0].rule_name == "RowStatus.Waiting");

	// 行ごといなくなった人の状態は、待たずに消す。
	const StabilizeResult gone = stabilize(w.next, view_with({}), kNow + 2 * kSec, kCfg);
	REQUIRE(gone.diff.disappeared.size() == 1);
	CHECK(gone.diff.disappeared[0].rule_name == "RowStatus.NotSharing");

	// 行の状態の名前。
	CHECK(std::string(row_state_log_name(dockview::RowState::NotSharing)) == "NotSharing");
}

TEST_CASE("枠がすべて使用中になったときと空いたときに、1 回だけ伝える")
{
	dockview::DockViewV2 full = view_with({});
	full.list_full = true;
	// 最初の周期 (前の周期がない) は、空いていても埋まっていても、変わったとみなさない。
	NoticeTrackerState state;
	StabilizeResult r = stabilize(state, view_with({}), kNow - kSec, kCfg);
	CHECK_FALSE(r.diff.list_full_changed_to.has_value());
	r = stabilize(r.next, full, kNow, kCfg);
	REQUIRE(r.diff.list_full_changed_to.has_value());
	CHECK(*r.diff.list_full_changed_to);
	r = stabilize(r.next, full, kNow + kSec, kCfg);
	CHECK_FALSE(r.diff.list_full_changed_to.has_value());
	r = stabilize(r.next, view_with({}), kNow + 2 * kSec, kCfg);
	REQUIRE(r.diff.list_full_changed_to.has_value());
	CHECK_FALSE(*r.diff.list_full_changed_to);
}

TEST_CASE("起動した直後に既に枠がすべて使用中でも、スクリーンリーダーに通知しない")
{
	dockview::DockViewV2 full = view_with({});
	full.list_full = true;
	StabilizeResult r = stabilize(NoticeTrackerState{}, full, kNow, kCfg);
	CHECK_FALSE(r.diff.list_full_changed_to.has_value());
	REQUIRE(r.next.list_full.has_value());
	CHECK(*r.next.list_full);
	r = stabilize(r.next, view_with({}), kNow + kSec, kCfg);
	REQUIRE(r.diff.list_full_changed_to.has_value());
	CHECK_FALSE(*r.diff.list_full_changed_to);
}

TEST_CASE("アンインストールを始めた後は、空いている枠が変わってもスクリーンリーダーに通知しない")
{
	// 4 人を載せて、枠がすべて使用中。
	Observation obs = healthy();
	obs.people.clear();
	for (int i = 1; i <= 4; ++i)
		obs.people.push_back(showing(std::to_string(100 + i), "p" + std::to_string(i), i));
	obs.call.roster_count = 4;
	StabilizeResult r = stabilize(NoticeTrackerState{}, decide_dock(obs), kNow - 2 * kSec, kCfg);
	r = stabilize(r.next, decide_dock(obs), kNow - kSec, kCfg);
	REQUIRE(r.next.list_full.has_value());
	REQUIRE(*r.next.list_full);
	// アンインストールを始め、消す段階で枠と人がなくなる (空いている枠ができる)。
	Observation removing = obs;
	removing.uninstall.phase = UninstallPhase::Running;
	removing.people.clear();
	removing.link.state = LinkState::Disconnected;
	const dockview::DockViewV2 raw = decide_dock(removing);
	REQUIRE_FALSE(raw.list_full);
	r = stabilize(r.next, raw, kNow, kCfg);
	CHECK_FALSE(r.diff.list_full_changed_to.has_value());
	REQUIRE(r.next.list_full.has_value());
	CHECK(*r.next.list_full); // 前の値を保つ
	Observation done = removing;
	done.uninstall.phase = UninstallPhase::Done;
	r = stabilize(r.next, decide_dock(done), kNow + kSec, kCfg);
	CHECK_FALSE(r.diff.list_full_changed_to.has_value());
}

TEST_CASE("decide_dock と stabilize を続けて実行すると、Vesktop が接続してこないメッセージは 2 秒後に表示される")
{
	Observation obs = not_connected();
	NoticeTrackerState state;
	StabilizeResult r = stabilize(state, decide_dock(obs), kNow, kCfg);
	CHECK(notices_of(r.view, "VesktopRunningNotLinked").empty());
	// 待っている間の 1 行目は「確認中...」(事実と違うことを表示しない)。
	REQUIRE_FALSE(r.view.first_line.forms.empty());
	CHECK(r.view.first_line.forms[0].id == cordmux::ledger::text::keys::kDockSummaryCheckingStatus);
	obs.now_us = kNow + 2 * kSec;
	r = stabilize(r.next, decide_dock(obs), obs.now_us, kCfg);
	CHECK(notices_of(r.view, "VesktopRunningNotLinked").size() == 1);
	CHECK(r.view.first_line.kind == dockview::FirstLineKind::Notice);
}

TEST_CASE("メッセージの並びがメッセージのレベルの順でなくても、1 行目はメッセージのレベル 1 を選ぶ")
{
	dockview::FirstLine fallback;
	fallback.kind = dockview::FirstLineKind::Summary;
	const std::vector<dockview::Notice> raw = {notice("L2", dockview::Tier::L2, dockview::Settle::Immediate),
						   notice("L1a", dockview::Tier::L1, dockview::Settle::Immediate),
						   notice("L3", dockview::Tier::L3, dockview::Settle::Immediate),
						   notice("L1b", dockview::Tier::L1, dockview::Settle::Immediate)};
	const dockview::FirstLine line = first_line_of(raw, fallback);
	CHECK(line.notice_key == "L1a");
	REQUIRE(line.more_count.has_value());
	CHECK(line.more_count->args == std::vector<std::string>{"1"});

	// decide_dock() の待ちを通す前の 1 行目も同じ (メッセージのレベル 2 の後に、メッセージのレベル 1 が並ぶ形)。
	Observation obs = not_connected();
	obs.install.dir = VencordDirState::PointsElsewhere; // InstallMismatch (メッセージのレベル 2)
	obs.link.state = LinkState::Connected;              // 下の映像のメッセージを作るために、接続したままにする
	obs.call.presence = Presence::InCall;
	obs.call.roster_count = 2;
	obs.recording.stopped_by_space_exhausted = true; // RecordStoppedSpaceExhausted (メッセージのレベル 1)
	const dockview::DockViewV2 v = decide_dock(obs);
	REQUIRE(v.first_line.tier.has_value());
	CHECK(*v.first_line.tier == dockview::Tier::L1);
	CHECK(v.first_line.notice_key == "RecordStoppedSpaceExhausted");
}
