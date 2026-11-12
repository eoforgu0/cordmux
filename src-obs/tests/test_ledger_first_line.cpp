// SPDX-License-Identifier: Apache-2.0

// ledger/notice_tracker.h の settle_first_line の単体テスト。1 行目の中身の優先順、状態タブの件数、既読の規則を確認する。
// 時計は読まないので、時刻は引数で進める。

#include "doctest.h"

#include "ledger/notice_tracker.h"
#include "ledger/rules.h"
#include "ledger/text_ids.h"
#include "ledger_dock_test_helpers.h"

#include <string>
#include <utility>
#include <vector>

using namespace ledger_dock_test;
namespace keys = cordmux::ledger::text::keys;

namespace {

dockview::Notice notice(const std::string &key, dockview::Tier tier)
{
	dockview::Notice n;
	n.key = key;
	n.rule_name = key;
	n.tier = tier;
	n.headline.id = "test." + key;
	return n;
}

// 平常の要約だけの View に、メッセージを足す。
dockview::DockViewV2 view_with(const std::vector<dockview::Notice> &notices)
{
	dockview::DockViewV2 view;
	view.fallback_line.kind = dockview::FirstLineKind::Summary;
	view.fallback_line.forms = {TextRef{"test.summary", {}}};
	view.read_line = view.fallback_line;
	view.notices = notices;
	return view;
}

FirstLineInput input_at(std::int64_t now_us, bool streaming = false, bool status_tab_open = false)
{
	FirstLineInput input;
	input.now_us = now_us;
	input.streaming = streaming;
	input.status_tab_open = status_tab_open;
	return input;
}

} // namespace

TEST_CASE("1 行目は、レベル 1、未読のレベル 2、それ以外の順に選ぶ")
{
	const dockview::Notice l1 = notice("L1", dockview::Tier::L1);
	const dockview::Notice l2 = notice("L2", dockview::Tier::L2);
	const dockview::Notice l3 = notice("L3", dockview::Tier::L3);

	FirstLineResult r = settle_first_line({}, view_with({l2, l1, l3}), input_at(kNow));
	CHECK(r.first_line.kind == dockview::FirstLineKind::Notice);
	CHECK(r.first_line.notice_key == "L1");

	r = settle_first_line({}, view_with({l3, l2}), input_at(kNow));
	CHECK(r.first_line.notice_key == "L2");

	// レベル 3 は 1 行目に出さない。
	r = settle_first_line({}, view_with({l3}), input_at(kNow));
	CHECK(r.first_line.kind == dockview::FirstLineKind::Summary);
}

TEST_CASE("無効は未読のレベル 2 より上、ダミー映像は未読のレベル 2 より下で、どちらも要約より上に出る")
{
	Observation off = healthy();
	off.choices.feature = FeatureSwitch::Off;
	FirstLineResult r = settle_first_line({}, decide_dock(off), input_at(kNow));
	CHECK(r.first_line.kind == dockview::FirstLineKind::UserChoice);
	REQUIRE(r.first_line.button.has_value());
	CHECK(r.first_line.button->id == ActionId::ToggleFeature);

	Observation dummy = healthy();
	dummy.choices.dummy = FeatureSwitch::On;
	r = settle_first_line({}, decide_dock(dummy), input_at(kNow));
	CHECK(r.first_line.kind == dockview::FirstLineKind::UserChoice);
	REQUIRE_FALSE(r.first_line.forms.empty());
	CHECK(r.first_line.forms[0].id == keys::kDockSummaryPlaceholdersStatus);

	// 無効とダミー映像が同時なら無効を出す。
	dummy.choices.feature = FeatureSwitch::Off;
	r = settle_first_line({}, decide_dock(dummy), input_at(kNow));
	REQUIRE_FALSE(r.first_line.forms.empty());
	CHECK(r.first_line.forms[0].id == keys::kDockSummaryDisabledStatus);

	// ダミー映像のとき、未読のレベル 2 があれば、そちらを出す。
	dummy.choices.feature = FeatureSwitch::On;
	dummy.ops.settings_unsaved = true;
	r = settle_first_line({}, decide_dock(dummy), input_at(kNow));
	CHECK(r.first_line.kind == dockview::FirstLineKind::Notice);
	CHECK(r.first_line.notice_key == "SettingsUnsaved");
}

TEST_CASE("1 行目の順は、レベル 1、無効、未読のレベル 2、ダミー、要約")
{
	Observation off = healthy();
	off.choices.feature = FeatureSwitch::Off;
	off.ops.settings_unsaved = true; // 未読のレベル 2

	// 無効は、未読のレベル 2 より優先する。
	dockview::DockViewV2 view = decide_dock(off);
	REQUIRE_FALSE(notices_of(view, "SettingsUnsaved").empty());
	FirstLineResult r = settle_first_line({}, view, input_at(kNow));
	CHECK(r.first_line.kind == dockview::FirstLineKind::UserChoice);
	REQUIRE_FALSE(r.first_line.forms.empty());
	CHECK(r.first_line.forms[0].id == keys::kDockSummaryDisabledStatus);
	// 状態タブの件数には数える。
	CHECK(r.status_count >= 1);

	// レベル 1 は無効より優先する。
	view.notices.push_back(notice("Big", dockview::Tier::L1));
	r = settle_first_line({}, view, input_at(kNow));
	CHECK(r.first_line.kind == dockview::FirstLineKind::Notice);
	CHECK(r.first_line.notice_key == "Big");

	// 無効の間は、状態タブを開いても未読のレベル 2 を既読にしない。有効に戻すと 1 行目に出る。
	view = decide_dock(off);
	r = settle_first_line({}, view, input_at(kNow, false, true));
	CHECK(r.next.read.empty());
	Observation on = off;
	on.choices.feature = FeatureSwitch::On;
	r = settle_first_line(r.next, decide_dock(on), input_at(kNow + kSec));
	CHECK(r.first_line.kind == dockview::FirstLineKind::Notice);
	CHECK(r.first_line.notice_key == "SettingsUnsaved");
}

TEST_CASE("全画面の行は、未読のレベル 2 の次、ダミー映像の前に出て、名前と [元に戻す] を持つ")
{
	Observation focus = healthy();
	focus.choices.focus_user_id = "100";
	focus.choices.focus_name = "taro";
	focus.choices.focus_candidates = {"100"};

	// 全画面の行。色なし (UserChoice) で、右端に [元に戻す] (SetFocus の戻す向き)。無効の行の印は立てない。
	FirstLineResult r = settle_first_line({}, decide_dock(focus), input_at(kNow));
	CHECK(r.first_line.kind == dockview::FirstLineKind::UserChoice);
	CHECK_FALSE(r.first_line.disabled);
	REQUIRE(r.first_line.forms.size() == 1);
	CHECK(r.first_line.forms[0].id == keys::kDockSummaryFullScreenStatus);
	CHECK(r.first_line.forms[0].args == std::vector<std::string>{"taro"});
	REQUIRE(r.first_line.button.has_value());
	CHECK(r.first_line.button->id == ActionId::SetFocus);
	CHECK(r.first_line.button->args.user_id.empty());
	CHECK(r.first_line.button->label.id == keys::kDockFullScreenRestoreButton);
	CHECK_FALSE(r.first_line.button->disabled_reason.has_value());
	CHECK(apply(focus, r.first_line.button->id, r.first_line.button->args).can_run());

	// ダミー映像より優先する。
	Observation both = focus;
	both.choices.dummy = FeatureSwitch::On;
	r = settle_first_line({}, decide_dock(both), input_at(kNow));
	REQUIRE_FALSE(r.first_line.forms.empty());
	CHECK(r.first_line.forms[0].id == keys::kDockSummaryFullScreenStatus);

	// 未読のレベル 2 があれば、そちらを出す。状態タブを開いて既読にすると、全画面の行に戻る。
	Observation unread = focus;
	unread.ops.settings_unsaved = true;
	r = settle_first_line({}, decide_dock(unread), input_at(kNow));
	CHECK(r.first_line.kind == dockview::FirstLineKind::Notice);
	CHECK(r.first_line.notice_key == "SettingsUnsaved");
	r = settle_first_line(r.next, decide_dock(unread), input_at(kNow + kSec, false, true));
	REQUIRE_FALSE(r.first_line.forms.empty());
	CHECK(r.first_line.forms[0].id == keys::kDockSummaryFullScreenStatus);

	// レベル 1 は全画面より優先する。
	dockview::DockViewV2 view = decide_dock(focus);
	view.notices.push_back(notice("Big", dockview::Tier::L1));
	r = settle_first_line({}, view, input_at(kNow));
	CHECK(r.first_line.kind == dockview::FirstLineKind::Notice);
	CHECK(r.first_line.notice_key == "Big");

	// 名前が分からないときは、行と同じ名前にする。
	Observation unnamed = focus;
	unnamed.choices.focus_name.clear();
	REQUIRE(unnamed.people[0].user_id == "100");
	unnamed.people[0].display_name = "Taro";
	r = settle_first_line({}, decide_dock(unnamed), input_at(kNow));
	REQUIRE_FALSE(r.first_line.forms.empty());
	CHECK(r.first_line.forms[0].args == std::vector<std::string>{"Taro"});
}

TEST_CASE("既読のレベル 2 は、読んだときより段階が進んだら未読に戻す")
{
	dockview::Notice low = notice("Space", dockview::Tier::L2);
	low.read_stage = 0;
	// 状態タブを開いて既読にする。
	FirstLineResult r = settle_first_line({}, view_with({low}), input_at(kNow, false, true));
	REQUIRE(r.next.read.size() == 1);
	CHECK(r.next.read[0].stage == 0);
	r = settle_first_line(r.next, view_with({low}), input_at(kNow + kSec));
	CHECK(r.first_line.kind == dockview::FirstLineKind::Summary);

	// 段階が進むと未読に戻り、1 行目に出る。キーは同じなので、メッセージは 1 つのまま。
	dockview::Notice urgent = low;
	urgent.read_stage = 1;
	r = settle_first_line(r.next, view_with({urgent}), input_at(kNow + 2 * kSec));
	CHECK(r.first_line.kind == dockview::FirstLineKind::Notice);
	CHECK(r.first_line.notice_key == "Space");
	CHECK(r.next.read.empty());

	// 状態タブを開くと、新しい段階で既読になる。同じ段階の間は、また出さない。
	r = settle_first_line(r.next, view_with({urgent}), input_at(kNow + 3 * kSec, false, true));
	REQUIRE(r.next.read.size() == 1);
	CHECK(r.next.read[0].stage == 1);
	r = settle_first_line(r.next, view_with({urgent}), input_at(kNow + 4 * kSec));
	CHECK(r.first_line.kind == dockview::FirstLineKind::Summary);
}

TEST_CASE("空き容量の警告は、残り 5 分以下になると段階が 1 になる")
{
	Observation obs = healthy();
	obs.choices.recording = FeatureSwitch::On;
	obs.recording.phase = RecordPhase::Recording;
	obs.recording.space_warn_seconds = 30 * 60;
	for (const auto &[remaining, stage] : {std::pair<std::int64_t, int>{25 * 60, 0}, {kSpaceUrgentSeconds + 1, 0},
					       {kSpaceUrgentSeconds, 1}, {60, 1}}) {
		obs.recording.remaining_seconds = remaining;
		const std::vector<dockview::Notice> n = notices_of(decide_dock(obs), "RecordSpaceLow");
		REQUIRE(n.size() == 1);
		CHECK(n[0].tier == dockview::Tier::L2);
		CHECK(n[0].read_stage == stage);
		CHECK(n[0].key == "RecordSpaceLow");
	}
}

TEST_CASE("残りの件数はレベル 1 が 2 件以上のときだけ添える")
{
	FirstLineResult r = settle_first_line(
		{}, view_with({notice("A", dockview::Tier::L1), notice("B", dockview::Tier::L1), notice("C", dockview::Tier::L2)}),
		input_at(kNow));
	REQUIRE(r.first_line.more_count.has_value());
	CHECK(r.first_line.more_count->args == std::vector<std::string>{"1"});

	r = settle_first_line({}, view_with({notice("C", dockview::Tier::L2), notice("D", dockview::Tier::L2)}), input_at(kNow));
	CHECK_FALSE(r.first_line.more_count.has_value());
}

TEST_CASE("状態タブの件数はレベル 1 と 2 の合計で、既読も数え、0 のときは件数を付けない")
{
	FirstLineResult r = settle_first_line({}, view_with({notice("L3", dockview::Tier::L3)}), input_at(kNow));
	CHECK(r.status_count == 0);
	CHECK(r.status_tab_label.id == keys::kDockTabStatusLabel);
	CHECK(r.status_tab_label.args.empty());

	const dockview::DockViewV2 view =
		view_with({notice("L1", dockview::Tier::L1), notice("L2", dockview::Tier::L2), notice("L3", dockview::Tier::L3)});
	r = settle_first_line({}, view, input_at(kNow));
	CHECK(r.status_count == 2);
	CHECK(r.status_tab_label.id == keys::kDockTabStatusCountLabel);
	CHECK(r.status_tab_label.args == std::vector<std::string>{"2"});

	r = settle_first_line(r.next, view, input_at(kNow + kSec, false, true));
	CHECK(r.status_count == 2);
}

TEST_CASE("状態タブを開くと、その時点のレベル 2 は 1 行目から消え、レベル 1 は残る")
{
	const dockview::DockViewV2 only_l2 = view_with({notice("L2", dockview::Tier::L2)});
	FirstLineResult r = settle_first_line({}, only_l2, input_at(kNow));
	CHECK(r.first_line.notice_key == "L2");

	r = settle_first_line(r.next, only_l2, input_at(kNow + kSec, false, true));
	CHECK(r.first_line.kind == dockview::FirstLineKind::Summary);
	CHECK(r.status_count == 1);

	// タブを閉じても既読のまま。
	r = settle_first_line(r.next, only_l2, input_at(kNow + 2 * kSec));
	CHECK(r.first_line.kind == dockview::FirstLineKind::Summary);

	// レベル 1 は開いても残る。
	const dockview::DockViewV2 with_l1 = view_with({notice("L1", dockview::Tier::L1)});
	r = settle_first_line({}, with_l1, input_at(kNow, false, true));
	CHECK(r.first_line.notice_key == "L1");
	r = settle_first_line(r.next, with_l1, input_at(kNow + kSec));
	CHECK(r.first_line.notice_key == "L1");
}

TEST_CASE("タブを開いた後に新しく出たレベル 2 は、閉じていれば 1 行目に出て、開いたままなら出ない")
{
	const dockview::Notice a = notice("A", dockview::Tier::L2);
	const dockview::Notice b = notice("B", dockview::Tier::L2);
	FirstLineResult opened = settle_first_line({}, view_with({a}), input_at(kNow, false, true));

	FirstLineResult r = settle_first_line(opened.next, view_with({a, b}), input_at(kNow + kSec));
	CHECK(r.first_line.notice_key == "B");

	r = settle_first_line(opened.next, view_with({a, b}), input_at(kNow + kSec, false, true));
	CHECK(r.first_line.kind == dockview::FirstLineKind::Summary);
}

TEST_CASE("既読のレベル 2 が配信の中で消えて、同じ配信の中で再び出たら既読のまま、別の配信なら未読に戻す")
{
	const dockview::DockViewV2 shown = view_with({notice("L2", dockview::Tier::L2)});
	const dockview::DockViewV2 gone = view_with({});
	FirstLineResult r = settle_first_line({}, shown, input_at(kNow, true, true));
	CHECK(r.first_line.kind == dockview::FirstLineKind::Summary);

	// 同じ配信の中で消えて、5 分後に再び出る: 既読のまま。
	r = settle_first_line(r.next, gone, input_at(kNow + kSec, true));
	r = settle_first_line(r.next, shown, input_at(kNow + 300 * kSec, true));
	CHECK(r.first_line.kind == dockview::FirstLineKind::Summary);

	// 消えて、配信が終わり、次の配信で再び出る: 未読。
	r = settle_first_line(r.next, gone, input_at(kNow + 301 * kSec, true));
	r = settle_first_line(r.next, gone, input_at(kNow + 302 * kSec, false));
	r = settle_first_line(r.next, gone, input_at(kNow + 303 * kSec, true));
	r = settle_first_line(r.next, shown, input_at(kNow + 304 * kSec, true));
	CHECK(r.first_line.notice_key == "L2");
}

TEST_CASE("配信が始まった周期に再び出たメッセージも、配信をまたいだので未読に戻す")
{
	const dockview::DockViewV2 shown = view_with({notice("L2", dockview::Tier::L2)});
	const dockview::DockViewV2 gone = view_with({});
	FirstLineResult r = settle_first_line({}, shown, input_at(kNow, false, true));
	r = settle_first_line(r.next, gone, input_at(kNow + kSec));
	r = settle_first_line(r.next, shown, input_at(kNow + 2 * kSec, true));
	CHECK(r.first_line.notice_key == "L2");
}

TEST_CASE("配信の外では、消えて 60 秒以上たってから出たら未読に戻す")
{
	const dockview::DockViewV2 shown = view_with({notice("L2", dockview::Tier::L2)});
	const dockview::DockViewV2 gone = view_with({});
	const FirstLineResult opened = settle_first_line({}, shown, input_at(kNow, false, true));

	// 59 秒: 既読のまま。
	FirstLineResult r = settle_first_line(opened.next, gone, input_at(kNow + kSec));
	r = settle_first_line(r.next, shown, input_at(kNow + 60 * kSec));
	CHECK(r.first_line.kind == dockview::FirstLineKind::Summary);

	// 60 秒: 未読。
	r = settle_first_line(opened.next, gone, input_at(kNow + kSec));
	r = settle_first_line(r.next, shown, input_at(kNow + 61 * kSec));
	CHECK(r.first_line.notice_key == "L2");

	// 待つ間に周期が来ても同じ。
	r = settle_first_line(opened.next, gone, input_at(kNow + kSec));
	r = settle_first_line(r.next, gone, input_at(kNow + 61 * kSec));
	CHECK(r.next.read.empty());
	r = settle_first_line(r.next, shown, input_at(kNow + 62 * kSec));
	CHECK(r.first_line.notice_key == "L2");

	// 配信の中で消えて、配信が終わった 10 秒後に出る (消えてから 60 秒未満): 既読のまま。
	r = settle_first_line({}, shown, input_at(kNow, true, true));
	r = settle_first_line(r.next, gone, input_at(kNow + kSec, true));
	r = settle_first_line(r.next, gone, input_at(kNow + 2 * kSec, false));
	r = settle_first_line(r.next, shown, input_at(kNow + 12 * kSec, false));
	CHECK(r.first_line.kind == dockview::FirstLineKind::Summary);
}

TEST_CASE("既読のメッセージは、消えない間は配信をまたいでも既読のまま")
{
	const dockview::DockViewV2 shown = view_with({notice("L2", dockview::Tier::L2)});
	FirstLineResult r = settle_first_line({}, shown, input_at(kNow, false, true));
	r = settle_first_line(r.next, shown, input_at(kNow + kSec, true));
	r = settle_first_line(r.next, shown, input_at(kNow + 2 * kSec, false));
	r = settle_first_line(r.next, shown, input_at(kNow + 3 * kSec, true));
	CHECK(r.first_line.kind == dockview::FirstLineKind::Summary);
}

TEST_CASE("配信で既読のレベル 2 がレベル 1 に上がると 1 行目に出て、配信が終わるとまた既読に戻る")
{
	FirstLineResult r = settle_first_line({}, view_with({notice("Audio", dockview::Tier::L2)}), input_at(kNow, false, true));
	r = settle_first_line(r.next, view_with({notice("Audio", dockview::Tier::L1)}), input_at(kNow + kSec, true));
	CHECK(r.first_line.notice_key == "Audio");
	r = settle_first_line(r.next, view_with({notice("Audio", dockview::Tier::L2)}), input_at(kNow + 2 * kSec));
	CHECK(r.first_line.kind == dockview::FirstLineKind::Summary);
}

TEST_CASE("primary のメッセージを既読にすると、1 行目は「確認中...」にならず、接続していないことを伝える")
{
	Observation obs = not_connected(); // VesktopRunningNotLinked (待ちを通すメッセージ)
	dockview::DockViewV2 view = decide_dock(obs);
	REQUIRE(notices_of(view, "VesktopRunningNotLinked").size() == 1);
	FirstLineResult r = settle_first_line({}, view, input_at(kNow, false, true));
	REQUIRE_FALSE(r.first_line.forms.empty());
	CHECK(r.first_line.kind == dockview::FirstLineKind::Summary);
	CHECK(r.first_line.forms[0].id == keys::kDockStageVesktopDisconnectedStatus);

	// メッセージを表示するまでの待ちの間は「確認中...」にする。
	view.notices.clear();
	r = settle_first_line({}, view, input_at(kNow));
	REQUIRE_FALSE(r.first_line.forms.empty());
	CHECK(r.first_line.forms[0].id == keys::kDockSummaryCheckingStatus);
}

// ---- 録画だけの失敗のレベル 1 (Notice::read_on_status_tab) ----

namespace {

dockview::Notice record_only_l1(const std::string &key)
{
	dockview::Notice n = notice(key, dockview::Tier::L1);
	n.read_on_status_tab = true;
	return n;
}

} // namespace

TEST_CASE("既読にしてよい印のあるレベル 1 は、状態タブを開くと 1 行目から消え、件数と状態タブのカードには残る")
{
	const dockview::DockViewV2 view = view_with({record_only_l1("RecordStop")});
	FirstLineResult r = settle_first_line({}, view, input_at(kNow));
	REQUIRE(r.first_line.tier.has_value());
	CHECK(*r.first_line.tier == dockview::Tier::L1);
	CHECK(r.first_line.notice_key == "RecordStop");

	r = settle_first_line(r.next, view, input_at(kNow + kSec, false, true));
	// 1 行目はレベル 1 の見た目 (tier がレベル 1 だと、ドックは背景を危険の色にする) ではなく、平常の要約になる。
	CHECK(r.first_line.kind == dockview::FirstLineKind::Summary);
	CHECK_FALSE(r.first_line.tier.has_value());
	CHECK(r.status_count == 1);
	REQUIRE(view.notices.size() == 1); // メッセージそのものは残る (状態タブのカード)

	// タブを閉じても既読のまま。
	r = settle_first_line(r.next, view, input_at(kNow + 2 * kSec));
	CHECK(r.first_line.kind == dockview::FirstLineKind::Summary);
	CHECK_FALSE(r.first_line.tier.has_value());
}

TEST_CASE("既読にしてよい印のあるレベル 1 の既読は、ほかのレベル 1 を 1 行目から消さない")
{
	const dockview::DockViewV2 view = view_with({record_only_l1("RecordStop"), notice("Other", dockview::Tier::L1)});
	FirstLineResult r = settle_first_line({}, view, input_at(kNow, false, true));
	CHECK(r.first_line.notice_key == "Other");
	REQUIRE(r.first_line.tier.has_value());
	CHECK(*r.first_line.tier == dockview::Tier::L1);
	CHECK_FALSE(r.first_line.more_count.has_value()); // 既読の分は「ほか N 件」に数えない
	CHECK(r.status_count == 2);
}

TEST_CASE("既読にしてよい印のあるレベル 1 は、消えて別の配信で再び出たら未読に戻り、段階が進んでも未読に戻る")
{
	const dockview::DockViewV2 shown = view_with({record_only_l1("RecordStop")});
	const dockview::DockViewV2 gone = view_with({});
	FirstLineResult r = settle_first_line({}, shown, input_at(kNow, true, true));
	r = settle_first_line(r.next, gone, input_at(kNow + kSec, true));
	// 同じ配信の中で再び出ても既読のまま。
	r = settle_first_line(r.next, shown, input_at(kNow + 2 * kSec, true));
	CHECK(r.first_line.kind == dockview::FirstLineKind::Summary);
	// 消えている間に配信が終わって別の配信が始まったら、未読に戻る。
	r = settle_first_line(r.next, gone, input_at(kNow + 3 * kSec, true));
	r = settle_first_line(r.next, gone, input_at(kNow + 4 * kSec, false));
	r = settle_first_line(r.next, gone, input_at(kNow + 5 * kSec, true));
	r = settle_first_line(r.next, shown, input_at(kNow + 6 * kSec, true));
	CHECK(r.first_line.notice_key == "RecordStop");

	// 段階が進んだら未読に戻る (レベル 2 と同じ)。
	dockview::Notice staged = record_only_l1("RecordStop");
	r = settle_first_line({}, view_with({staged}), input_at(kNow, false, true));
	staged.read_stage = 1;
	r = settle_first_line(r.next, view_with({staged}), input_at(kNow + kSec));
	CHECK(r.first_line.notice_key == "RecordStop");
}

TEST_CASE("録画の停止と録画の開始の失敗のレベル 1 だけが、既読にしてよい印を持つ")
{
	Observation space = healthy();
	space.recording.stopped_by_space_exhausted = true;
	const std::vector<dockview::Notice> stopped = notices_of(decide_dock(space), "RecordStoppedSpaceExhausted");
	REQUIRE(stopped.size() == 1);
	CHECK(stopped[0].tier == dockview::Tier::L1);
	CHECK(stopped[0].read_on_status_tab);

	Observation failed = healthy();
	failed.recording.phase = RecordPhase::StartFailed;
	failed.recording.unwritable_dir = "D:/rec";
	const std::vector<dockview::Notice> start_failed = notices_of(decide_dock(failed), "RecordDirUnwritable");
	REQUIRE(start_failed.size() == 1);
	CHECK(start_failed[0].tier == dockview::Tier::L1);
	CHECK(start_failed[0].read_on_status_tab);

	// 始めようとしていない (書き込めなかった場所が残っているだけ) ときはレベル 2 で、印は付けない。
	Observation remembered = healthy();
	remembered.recording.phase = RecordPhase::Idle;
	remembered.recording.unwritable_dir = "D:/rec";
	const std::vector<dockview::Notice> level2 = notices_of(decide_dock(remembered), "RecordDirUnwritable");
	REQUIRE(level2.size() == 1);
	CHECK(level2[0].tier == dockview::Tier::L2);
	CHECK_FALSE(level2[0].read_on_status_tab);

	// ほかのレベル 1 (配信が損なわれるもの) は、印を持たない。
	Observation no_video = healthy();
	for (const dockview::Notice &n : decide_dock(no_video).notices)
		if (n.tier == dockview::Tier::L1)
			CHECK_FALSE(n.read_on_status_tab);

	// 実際の View でも、状態タブを開くと 1 行目から消える。
	FirstLineResult r = settle_first_line({}, decide_dock(space), input_at(kNow));
	CHECK(r.first_line.notice_key == "RecordStoppedSpaceExhausted");
	r = settle_first_line(r.next, decide_dock(space), input_at(kNow + kSec, false, true));
	CHECK_FALSE(r.first_line.tier.has_value());
	CHECK(r.status_count == 1);
}
