// SPDX-License-Identifier: Apache-2.0

// decide_dock() の、参加者タブの一覧 (DockViewV2::people_list) と上部のメインとサブ (DockViewV2::accounts) の単体テスト。

#include "doctest.h"

#include "ledger/notice_tracker.h"
#include "ledger/rules.h"
#include "ledger/text_ids.h"
#include "ledger_dock_test_helpers.h"
#include "sources/composite_input.h"

#include <string>
#include <vector>

using namespace ledger_dock_test;
using dockview::AccountCallState;
using dockview::MicState;
using dockview::PersonRow;
using dockview::SlotPosition;
using dockview::SpeakerState;
namespace keys = cordmux::ledger::text::keys;

namespace {

// サブアカウントを使う構成で、メインアカウント (main) とサブアカウント (sub) が同じ通話に参加している状態。
Observation with_main_and_sub()
{
	Observation obs = healthy();
	PersonFact main = present("500", "main");
	PersonFact sub = present("600", "sub");
	sub.is_receiver = true;
	main.is_self_frame = true;
	obs.people.push_back(main);
	obs.people.push_back(sub);
	obs.choices.broadcaster_user_id = "500";
	obs.call.roster_count = 4;
	return obs;
}

std::vector<std::string> ids_of(const std::vector<PersonRow> &rows)
{
	std::vector<std::string> out;
	for (const PersonRow &row : rows)
		out.push_back(row.user_id);
	return out;
}

const PersonRow *find_row(const dockview::PeopleListView &list, const std::string &user_id)
{
	if (list.pinned.has_value() && list.pinned->user_id == user_id)
		return &*list.pinned;
	for (const std::vector<PersonRow> *rows : {&list.sharing, &list.in_call}) {
		for (const PersonRow &row : *rows) {
			if (row.user_id == user_id)
				return &row;
		}
	}
	return nullptr;
}

} // namespace

TEST_CASE("一覧は、メインの行を先頭に固定し、画面共有中と通話中の 2 組に分け、サブアカウントの行を持たない")
{
	Observation obs = with_main_and_sub();
	obs.people.push_back(showing("700", "Bob2", 2));
	obs.people.push_back(showing("800", "bob10", 3));
	obs.people.push_back(present("900", "carol"));
	const dockview::PeopleListView list = decide_dock(obs).people_list;

	REQUIRE(list.pinned.has_value());
	CHECK(list.pinned->user_id == "500");
	CHECK(list.pinned->is_main);
	// 組の中は表示名の自然順。
	CHECK(ids_of(list.sharing) == std::vector<std::string>{"700", "800", "100"});
	CHECK(ids_of(list.in_call) == std::vector<std::string>{"900", "200"});
	CHECK(find_row(list, "600") == nullptr);

	CHECK(list.sharing_heading.id == keys::kDockGroupSharingHeading);
	CHECK(list.sharing_heading.args == std::vector<std::string>{"3"});
	CHECK(list.in_call_heading.id == keys::kDockGroupInCallHeading);
	CHECK(list.in_call_heading.args == std::vector<std::string>{"2"});
	CHECK(list.slots_used == 3);
	CHECK(list.slots_total == 4);
	CHECK(list.slots_label.id == keys::kDockSlotsUsedStatus);
	CHECK(list.slots_label.args == std::vector<std::string>{"3", "4"});
}

TEST_CASE("メインアカウントだけの構成では、Vesktop のアカウントの行を先頭に固定する")
{
	Observation obs = healthy();
	PersonFact own = present("600", "me");
	own.is_receiver = true;
	own.is_self_frame = true;
	obs.people.push_back(own);
	dockview::PeopleListView list = decide_dock(obs).people_list;
	REQUIRE(list.pinned.has_value());
	CHECK(list.pinned->user_id == "600");
	CHECK(list.pinned->is_main);

	// メインアカウントの userId が Vesktop のアカウントと同じときも同じ。
	obs.choices.broadcaster_user_id = "600";
	list = decide_dock(obs).people_list;
	REQUIRE(list.pinned.has_value());
	CHECK(list.pinned->user_id == "600");
	CHECK_FALSE(decide_dock(obs).accounts.sub.has_value());
}

TEST_CASE("行の枠の位置は配信の位置と同じ規則で決まる")
{
	Observation obs = with_main_and_sub();
	obs.people[1] = showing("200", "hanako", 2);
	obs.people.push_back(showing("700", "kenji", 4));
	dockview::PeopleListView list = decide_dock(obs).people_list;
	CHECK(find_row(list, "200")->position == SlotPosition::TopLeft); // hanako
	CHECK(find_row(list, "700")->position == SlotPosition::TopRight); // kenji
	CHECK(find_row(list, "100")->position == SlotPosition::BottomLeft); // taro
	CHECK(find_row(list, "500")->position == SlotPosition::None);

	// 載せた順: 番号の順。
	obs.choices.slot_order = cordmux::config::SlotOrder::Added;
	for (PersonFact &person : obs.people) {
		if (person.user_id == "700")
			person.slot_added_order = 1;
		else if (person.user_id == "100")
			person.slot_added_order = 2;
		else if (person.user_id == "200")
			person.slot_added_order = 3;
	}
	list = decide_dock(obs).people_list;
	CHECK(find_row(list, "700")->position == SlotPosition::TopLeft);
	CHECK(find_row(list, "100")->position == SlotPosition::TopRight);
	CHECK(find_row(list, "200")->position == SlotPosition::BottomLeft);

	// 自分の枠は左上で、ほかの人は右上から並ぶ (名前順: hanako、kenji、taro)。
	obs.choices.slot_order = cordmux::config::SlotOrder::Name;
	obs.choices.self_frame_shown = true;
	list = decide_dock(obs).people_list;
	CHECK(find_row(list, "500")->position == SlotPosition::TopLeft);
	CHECK(find_row(list, "200")->position == SlotPosition::TopRight);
	CHECK(find_row(list, "700")->position == SlotPosition::BottomLeft);
	CHECK(find_row(list, "100")->position == SlotPosition::BottomRight);

	// 枠を持っているが映っていない人は位置なしで、その人を除いて詰める (名前順: kenji、taro)。
	obs.choices.self_frame_shown = false;
	for (PersonFact &person : obs.people) {
		if (person.user_id == "200")
			person.slot_visible = false;
	}
	list = decide_dock(obs).people_list;
	CHECK(find_row(list, "200")->position == SlotPosition::None);
	CHECK(find_row(list, "700")->position == SlotPosition::TopLeft);
	CHECK(find_row(list, "100")->position == SlotPosition::TopRight);

	// 1 人だけなら全面。
	Observation solo = healthy();
	CHECK(find_row(decide_dock(solo).people_list, "100")->position == SlotPosition::Full);

	// ダミー映像の間は誰の位置も表示しない。
	solo.choices.dummy = FeatureSwitch::On;
	CHECK(find_row(decide_dock(solo).people_list, "100")->position == SlotPosition::None);
}

TEST_CASE("自分の枠を先頭に置かないときの行の位置は、自分の枠の人が通話にいなくても、配信の配置と同じ名前で決める")
{
	// サブアカウントの構成で、メインアカウント (自分の枠の人) が通話にいない (PersonFact がない)。自分の枠の名前は、配信の配置と
	// 同じ関数 (dock/build_observation.h の self_frame_identity) で決めた値が Choices に入る。
	Observation obs = healthy();
	obs.people = {showing("200", "hanako", 2), showing("700", "kenji", 4)};
	obs.choices.broadcaster_user_id = "500";
	obs.choices.self_frame_shown = true;
	obs.choices.self_slot_first = false;
	obs.choices.self_frame_user_id = "500";
	obs.choices.self_frame_name = "zed";
	const dockview::PeopleListView list = decide_dock(obs).people_list;
	// 名前順: hanako、kenji、自分の枠 (zed)。名前が空として比べると、自分の枠が先頭になり、2 人の位置が 1 つずつ後ろにずれる。
	CHECK(find_row(list, "200")->position == SlotPosition::TopLeft);
	CHECK(find_row(list, "700")->position == SlotPosition::TopRight);

	// 配信の配置 (合成のソースの全画面にできるタイル) も同じ位置。
	cordmux::sources::CompositeInput input;
	input.self_shown = true;
	input.self_first = false;
	input.self_user_id = obs.choices.self_frame_user_id;
	input.self_name = obs.choices.self_frame_name;
	input.full_when_single = true;
	for (const PersonFact &p : obs.people) {
		cordmux::sources::CompositeShare share;
		share.slot = *p.slot;
		share.name = p.display_name.value_or(std::string());
		share.user_id = p.user_id;
		input.shares.push_back(share);
	}
	const std::vector<cordmux::sources::CompositeFocusTile> tiles = cordmux::sources::composite_focus_tiles(input);
	REQUIRE(tiles.size() == 3);
	CHECK(tiles[0].user_id == "200");
	CHECK(tiles[0].cell == 0);
	CHECK(tiles[1].user_id == "700");
	CHECK(tiles[1].cell == 1);
	CHECK(tiles[2].user_id == "500");
	CHECK(tiles[2].cell == 2);
}

TEST_CASE("行は [メイン] [自動] のタグ、マイクとスピーカーの状態を持つ")
{
	Observation obs = with_main_and_sub();
	obs.seen.people.push_back(SeenPerson{"100", "taro", 0, true});
	obs.people[0].voice_arriving = true;
	obs.people[0].audio.has_screen_audio = true;
	dockview::PeopleListView list = decide_dock(obs).people_list;
	const PersonRow *taro = find_row(list, "100");
	CHECK(taro->auto_add);
	CHECK_FALSE(taro->is_main);
	CHECK(taro->mic == MicState::Normal);
	CHECK(taro->speaker == SpeakerState::Normal);

	const PersonRow *hanako = find_row(list, "200");
	CHECK_FALSE(hanako->auto_add);
	CHECK(hanako->mic == MicState::NoVoice);
	CHECK(hanako->speaker == SpeakerState::Hidden);

	obs.people[0].audio.voice_muted = true;
	obs.people[0].audio.screen_muted = true;
	list = decide_dock(obs).people_list;
	CHECK(find_row(list, "100")->mic == MicState::Muted);
	CHECK(find_row(list, "100")->speaker == SpeakerState::Muted);

	// ミュートの人は、声がまだ届いていなくてもミュートと表示する (外せるように)。
	obs.people[0].voice_arriving = false;
	CHECK(find_row(decide_dock(obs).people_list, "100")->mic == MicState::Muted);
}

TEST_CASE("枠が埋まっているとき、載っていない人のチェックは入れ替えになり、候補は枠に載っている人の位置の順")
{
	Observation obs = with_main_and_sub();
	obs.people[0] = showing("100", "z", 1);
	obs.people.push_back(showing("701", "a", 2));
	obs.people.push_back(showing("702", "b", 3));
	obs.people.push_back(showing("703", "c", 4));
	for (PersonFact &person : obs.people) {
		if (person.user_id == "200")
			person.sharing = true; // 画面共有している人だけが載せられる
	}
	const dockview::PeopleListView list = decide_dock(obs).people_list;
	const PersonRow *hanako = find_row(list, "200");
	CHECK_FALSE(hanako->check_enabled);
	CHECK(hanako->check_replaces);
	REQUIRE(hanako->check_action.disabled_reason.has_value());
	CHECK(hanako->check_action.disabled_reason->id == keys::kDockRowCheckReplaceTip);
	REQUIRE(hanako->replace_choices.size() == 4);
	CHECK(hanako->replace_choices[0].position == SlotPosition::TopLeft);
	CHECK(hanako->replace_choices[0].action.id == ActionId::ReplaceSlot);
	CHECK(hanako->replace_choices[0].action.args.user_id == "200");
	CHECK(hanako->replace_choices[0].action.args.replace_user_id == "701");
	CHECK(hanako->replace_choices[0].action.args.slot == 2);
	CHECK_FALSE(hanako->replace_choices[0].action.disabled_reason.has_value());
	CHECK(hanako->replace_choices[3].action.args.replace_user_id == "100");

	// 載っている人のチェックは入れ替えにならない。
	CHECK_FALSE(find_row(list, "701")->check_replaces);
	CHECK(find_row(list, "701")->replace_choices.empty());

	// 画面共有していない人のチェックは、入れ替えにもならない (載せられない)。
	Observation not_sharing = obs;
	for (PersonFact &person : not_sharing.people) {
		if (person.user_id == "200")
			person.sharing = false;
	}
	const dockview::PeopleListView idle_list = decide_dock(not_sharing).people_list;
	const PersonRow *idle = find_row(idle_list, "200");
	CHECK_FALSE(idle->check_enabled);
	CHECK_FALSE(idle->check_replaces);
	REQUIRE(idle->check_action.disabled_reason.has_value());
	CHECK(idle->check_action.disabled_reason->id == keys::kDockRowCheckNotSharingTip);

	// 空きがあれば入れ替えにならない。
	Observation free = with_main_and_sub();
	for (PersonFact &person : free.people) {
		if (person.user_id == "200")
			person.sharing = true;
	}
	const dockview::PeopleListView open_list = decide_dock(free).people_list;
	const PersonRow *open = find_row(open_list, "200");
	CHECK(open->check_enabled);
	CHECK_FALSE(open->check_replaces);
}

TEST_CASE("自分の枠を出していて 3 人に達しているときも、チェックは入れ替えになり、枠の数に自分の枠を数える")
{
	Observation obs = with_main_and_sub();
	obs.choices.self_frame_selected = true;
	obs.choices.self_frame_bound = true;
	obs.choices.self_frame_on = true;
	obs.choices.self_frame_shown = true;
	obs.people.push_back(showing("701", "a", 2));
	obs.people.push_back(showing("702", "b", 3));
	for (PersonFact &person : obs.people) {
		if (person.user_id == "200")
			person.sharing = true; // 画面共有している人だけが載せられる
	}
	const dockview::PeopleListView list = decide_dock(obs).people_list;
	CHECK(list.slots_used == 4);
	const PersonRow *hanako = find_row(list, "200");
	CHECK(hanako->check_replaces);
	REQUIRE(hanako->check_action.disabled_reason.has_value());
	CHECK(hanako->check_action.disabled_reason->id == keys::kDockRowCheckReplaceTip);
	// 自分の枠の付け外しのチェックは入れ替えにならない。
	CHECK_FALSE(find_row(list, "500")->check_replaces);
}

TEST_CASE("接続が切れても、切れる前の参加者の行を切れる前の組のまま残し、off にして状態を持たない")
{
	Observation connected = with_main_and_sub();
	connected.people[0].last_video_frame_at_us = kNow - 5 * kSec; // 行の状態がある人
	std::vector<PersonFact> before = connected.people;

	Observation obs = connected;
	obs.link.state = LinkState::Disconnected;
	obs.link.connected_at_us = kNow - 60 * kSec;
	obs.call = Call{};
	// 接続が切れると、枠に載っている人だけが people に残り、画面共有の事実は消える。
	PersonFact taro = showing("100", "taro", 1);
	taro.sharing = false;
	taro.sharing_known = false;
	taro.in_roster = false;
	taro.slot_visible = false;
	obs.people = {taro};
	obs.last_known_people = before;

	const dockview::DockViewV2 view = decide_dock(obs);
	const dockview::PeopleListView &list = view.people_list;
	CHECK(find_row(list, "600") == nullptr);
	REQUIRE(list.pinned.has_value());
	CHECK(list.pinned->user_id == "500");
	CHECK(list.pinned->off);
	CHECK(ids_of(list.sharing) == std::vector<std::string>{"100"});
	CHECK(ids_of(list.in_call) == std::vector<std::string>{"200"});
	for (const PersonRow &row : list.sharing) {
		CHECK(row.off);
		CHECK_FALSE(row.check_enabled);
		CHECK_FALSE(row.status.has_value());
		CHECK_FALSE(row.quality.has_value());
		CHECK_FALSE(row.check_replaces);
	}

	// 現在のドックの一覧 (people) は変わらない。
	CHECK(view.people.size() == 1);

	// 接続している間は off にしない。
	const dockview::PeopleListView live = decide_dock(connected).people_list;
	CHECK_FALSE(find_row(live, "100")->off);
	CHECK(find_row(live, "100")->status.has_value());
}

TEST_CASE("接続が切れた行のチェックは現在の枠に合わせる (枠を空にした後は外れる)")
{
	Observation connected = with_main_and_sub();
	std::vector<PersonFact> before = connected.people;
	const dockview::PersonRow *row_before = find_row(decide_dock(connected).people_list, "100");
	REQUIRE(row_before != nullptr);
	REQUIRE(row_before->checked);

	Observation obs = connected;
	obs.link.state = LinkState::Disconnected;
	obs.link.connected_at_us = kNow - 60 * kSec;
	obs.call = Call{};
	obs.last_known_people = before;

	// 枠を空にするまでの間は、枠に載っている人が people に残るのでチェックが付いたまま。
	PersonFact taro = showing("100", "taro", 1);
	taro.sharing = false;
	taro.sharing_known = false;
	taro.in_roster = false;
	obs.people = {taro};
	const dockview::PeopleListView held = decide_dock(obs).people_list;
	REQUIRE(find_row(held, "100") != nullptr);
	CHECK(find_row(held, "100")->checked);
	CHECK(held.slots_used == 1);

	// 枠を空にした後は、覚えている行が切れる前の枠の値を持っていてもチェックを付けない。
	obs.people.clear();
	const dockview::PeopleListView emptied = decide_dock(obs).people_list;
	REQUIRE(find_row(emptied, "100") != nullptr);
	CHECK_FALSE(find_row(emptied, "100")->checked);
	CHECK_FALSE(find_row(emptied, "100")->slot.has_value());
	CHECK(emptied.slots_used == 0);
}

TEST_CASE("アンインストールを始めた後は一覧に行を作らない")
{
	Observation obs = with_main_and_sub();
	obs.uninstall.phase = UninstallPhase::Running;
	const dockview::PeopleListView list = decide_dock(obs).people_list;
	CHECK_FALSE(list.pinned.has_value());
	CHECK(list.sharing.empty());
	CHECK(list.in_call.empty());
}

TEST_CASE("stabilize() を通すと、一覧の行にも待ちを通した後の行の状態が入る")
{
	Observation obs = with_main_and_sub();
	// 「映像を待っています...」(待ちなし) の人と、選んだ映像が見つからない自分の行 (Debounced)。
	obs.people[0].last_video_frame_at_us.reset();
	obs.choices.self_frame_selected = true;
	obs.choices.self_frame_on = true;
	const dockview::DockViewV2 raw = decide_dock(obs);
	const StabilizeResult r = stabilize({}, raw, kNow, NoticeTrackerConfig{});
	const PersonRow *waiting = find_row(r.view.people_list, "100");
	REQUIRE(waiting->status.has_value());
	CHECK(waiting->status->state == dockview::RowState::Waiting);
	const PersonRow *main = find_row(r.view.people_list, "500");
	CHECK_FALSE(main->status.has_value());

	NoticeTrackerConfig cfg;
	const StabilizeResult later = stabilize(r.next, raw, kNow + cfg.show_delay_us, cfg);
	main = find_row(later.view.people_list, "500");
	REQUIRE(main->status.has_value());
	CHECK(main->status->state == dockview::RowState::NoVideo);
}

TEST_CASE("サブアカウントを使う構成の上部は、メインとサブの 2 行になり、同じ通話なら注意の色にしない")
{
	Observation obs = with_main_and_sub();
	obs.call.name = "room";
	const dockview::AccountsView accounts = decide_dock(obs).accounts;
	CHECK(accounts.main.row.name.literal == "main");
	CHECK(accounts.main.row.state == AccountCallState::SameCall);
	CHECK(accounts.main.row.call.literal == "room");
	CHECK_FALSE(accounts.main.row.call_warn);
	REQUIRE(accounts.sub.has_value());
	CHECK(accounts.sub->row.name.literal == "sub");
	CHECK(accounts.sub->row.state == AccountCallState::SameCall);
	CHECK(accounts.sub->row.call.literal == "room");
	CHECK_FALSE(accounts.sub->row.call_warn);

	// 同じ通話に参加しているので [移動] は押せず、[退出] は押せる。
	REQUIRE(accounts.sub->move.disabled_reason.has_value());
	CHECK(accounts.sub->move.disabled_reason->id == keys::kDockAccountSameCallTip);
	CHECK(accounts.sub->move.id == ActionId::MoveToMainCall);
	CHECK(accounts.sub->move.label.id == keys::kDockAccountMoveButton);
	CHECK_FALSE(accounts.sub->leave.disabled_reason.has_value());
	CHECK(accounts.sub->leave.id == ActionId::LeaveCall);
	CHECK(accounts.sub->leave.label.id == keys::kDockAccountLeaveButton);
	CHECK_FALSE(accounts.main.change.disabled_reason.has_value());
	CHECK(accounts.main.change.label.id == keys::kDockAccountChangeButton);
}

TEST_CASE("通話の名前が分からないときは「-」、サブが別の通話に参加しているときは注意の色にする")
{
	Observation obs = with_main_and_sub();
	dockview::AccountsView accounts = decide_dock(obs).accounts;
	CHECK(accounts.main.row.call.literal == "-");
	CHECK(accounts.sub->row.call.literal == "-");

	// メインアカウントが別の通話に参加している。
	obs.people.erase(obs.people.begin() + 2); // メインアカウントの行
	obs.call.main_presence = Presence::InCall;
	obs.call.main_name = "other";
	obs.call.name = "room";
	obs.call.main_call_joinable = true;
	accounts = decide_dock(obs).accounts;
	CHECK(accounts.main.row.state == AccountCallState::OtherCall);
	CHECK(accounts.main.row.call.literal == "other");
	CHECK_FALSE(accounts.main.row.call_warn);
	CHECK(accounts.sub->row.state == AccountCallState::OtherCall);
	CHECK(accounts.sub->row.call.literal == "room");
	CHECK(accounts.sub->row.call_warn);
	CHECK_FALSE(accounts.sub->move.disabled_reason.has_value());

	// 入れない通話。
	obs.call.main_call_joinable = false;
	accounts = decide_dock(obs).accounts;
	REQUIRE(accounts.sub->move.disabled_reason.has_value());
	CHECK(accounts.sub->move.disabled_reason->id == keys::kDockAccountCannotJoinTip);

	// メインアカウントの通話が分からない。「通話なし」と断定せず、名前の「-」を保つ。サーバーの通話は、Vesktop の
	// アカウントから見えないところにいることがあり、どの通話にもいないこととは区別できない。
	obs.call.main_presence = Presence::Unknown;
	accounts = decide_dock(obs).accounts;
	CHECK(accounts.main.row.state == AccountCallState::Unknown);
	CHECK(accounts.main.row.call.literal == "-");
	REQUIRE(accounts.sub->move.disabled_reason.has_value());
	CHECK(accounts.sub->move.disabled_reason->id == keys::kDockAccountMainCallUnknownTip);
}

TEST_CASE("参加者の一覧をまだ読めていない間は、サブが別の通話に参加しているとして注意の色にしない")
{
	Observation obs = with_main_and_sub();
	obs.people.erase(obs.people.begin() + 2); // メインアカウントの行
	obs.call.presence = Presence::InCall;
	obs.call.name = "room";
	obs.call.main_presence = Presence::InCall;
	obs.call.main_name = "other";
	obs.call.main_call_joinable = true;

	// 一覧を読めた後は、メインアカウントがいなければ別の通話で、注意の色になる (これまでどおり)。
	obs.call.roster_count = 3;
	dockview::AccountsView accounts = decide_dock(obs).accounts;
	CHECK(accounts.sub->row.state == AccountCallState::OtherCall);
	CHECK(accounts.sub->row.call_warn);

	// 一覧をまだ読めていない間は、メインアカウントがいるかを決められないので、注意の色にしない。通話の名前は出す。
	obs.call.roster_count.reset();
	accounts = decide_dock(obs).accounts;
	CHECK(accounts.sub->row.state == AccountCallState::SameCall);
	CHECK_FALSE(accounts.sub->row.call_warn);
	CHECK(accounts.sub->row.call.literal == "room");
}

TEST_CASE("接続していないときは「未接続」、通話にいないときは「通話なし」になり、ボタンは同じ位置に押せないまま残る")
{
	Observation obs = with_main_and_sub();
	obs.call.presence = Presence::NotInCall;
	obs.call.roster_count = 0;
	dockview::AccountsView accounts = decide_dock(obs).accounts;
	CHECK(accounts.sub->row.state == AccountCallState::NoCall);
	REQUIRE(accounts.sub->row.call.text.has_value());
	CHECK(accounts.sub->row.call.text->id == keys::kDockAccountNoCallStatus);
	REQUIRE(accounts.sub->leave.disabled_reason.has_value());
	CHECK(accounts.sub->leave.disabled_reason->id == keys::kDockAccountSubNoCallTip);

	obs.link.state = LinkState::Disconnected;
	obs.call = Call{};
	accounts = decide_dock(obs).accounts;
	CHECK(accounts.main.row.state == AccountCallState::NotConnected);
	REQUIRE(accounts.main.row.call.text.has_value());
	CHECK(accounts.main.row.call.text->id == keys::kDockAccountNotConnectedStatus);
	CHECK(accounts.sub->row.state == AccountCallState::NotConnected);
	REQUIRE(accounts.sub->move.disabled_reason.has_value());
	CHECK(accounts.sub->move.disabled_reason->id == keys::kDockAccountNotConnectedTip);
	REQUIRE(accounts.sub->leave.disabled_reason.has_value());
	CHECK(accounts.sub->leave.disabled_reason->id == keys::kDockAccountNotConnectedTip);
	// [変更] は接続していなくても押せる。
	CHECK_FALSE(accounts.main.change.disabled_reason.has_value());
}

TEST_CASE("メインアカウントだけの構成の上部は 1 行で、Vesktop のアカウントを表す")
{
	Observation obs = healthy();
	PersonFact own = present("600", "me");
	own.is_receiver = true;
	obs.people.push_back(own);
	obs.call.name = "room";
	const dockview::AccountsView accounts = decide_dock(obs).accounts;
	CHECK_FALSE(accounts.sub.has_value());
	CHECK(accounts.main.row.name.literal == "me");
	CHECK(accounts.main.row.state == AccountCallState::SameCall);
	CHECK(accounts.main.row.call.literal == "room");
	CHECK(accounts.main.no_main.checked);
}

TEST_CASE("[変更] のメニューは通話中の人、過去の参加者の順で、Vesktop のアカウントを並べない")
{
	Observation obs = with_main_and_sub();
	obs.seen.people.push_back(SeenPerson{"900", "zoe"});
	obs.seen.people.push_back(SeenPerson{"901", "amy"});
	obs.seen.people.push_back(SeenPerson{"100", "taro"});
	obs.seen.people.push_back(SeenPerson{"600", "sub"});
	const dockview::AccountsView accounts = decide_dock(obs).accounts;
	std::vector<std::string> ids;
	for (const dockview::MainAccountChoice &choice : accounts.main.choices)
		ids.push_back(choice.action.args.user_id);
	CHECK(ids == std::vector<std::string>{"200", "500", "100", "901", "900"});
	CHECK(accounts.main.choices[0].in_call);
	CHECK_FALSE(accounts.main.choices[3].in_call);

	// 現在のメインアカウントはチェックで示し、押せない理由を持たない。
	CHECK(accounts.main.choices[1].checked);
	CHECK_FALSE(accounts.main.choices[1].action.disabled_reason.has_value());
	CHECK_FALSE(accounts.main.choices[0].checked);

	// 「メインアカウントを使わない」は押せて、値は空。
	REQUIRE(accounts.main.no_main.label.text.has_value());
	CHECK(accounts.main.no_main.label.text->id == keys::kDockAccountNoMainMenu);
	CHECK(accounts.main.no_main.action.args.user_id.empty());
	CHECK_FALSE(accounts.main.no_main.action.disabled_reason.has_value());
	CHECK_FALSE(accounts.main.no_main.checked);
}

TEST_CASE("名前が分からないアカウントは userId を出さず「-」にする")
{
	Observation obs = with_main_and_sub();
	obs.choices.broadcaster_user_id = "999";
	const dockview::AccountsView accounts = decide_dock(obs).accounts;
	CHECK(accounts.main.row.name.literal == "-");
}

TEST_CASE("アンインストールを始めた後は、上部のボタンを押せない")
{
	Observation obs = with_main_and_sub();
	obs.uninstall.phase = UninstallPhase::Running;
	const dockview::AccountsView accounts = decide_dock(obs).accounts;
	REQUIRE(accounts.main.change.disabled_reason.has_value());
	CHECK(accounts.main.change.disabled_reason->id == keys::kSettingsUninstallStartedText);
	REQUIRE(accounts.sub->move.disabled_reason.has_value());
	CHECK(accounts.sub->move.disabled_reason->id == keys::kSettingsUninstallStartedText);
}

TEST_CASE("録画中で始めた時刻が分かれば、録画のボタンに経過時間を出す")
{
	Observation obs = healthy();
	obs.recording.phase = cordmux::ledger::RecordPhase::Recording;
	obs.recording.enabled_since_us = obs.now_us - 754'000'000;
	const dockview::DockViewV2 view = decide_dock(obs);
	CHECK(view.record.label.id == keys::kDockRecordStopElapsedButton);
	REQUIRE(view.record.label.args.size() == 1);
	CHECK(view.record.label.args[0] == "12:34");
	// ボタンの名前 (読み上げソフトが読む名前) には経過時間を入れない。毎秒変わる名前は、フォーカスがある間読み続けられる。
	CHECK(view.record.name.id == keys::kDockRecordStopButton);
	CHECK(view.record.name.args.empty());
}

TEST_CASE("録画のボタンの名前は、経過時間を出さない状態では見えるテキストと同じ")
{
	Observation obs = healthy();
	const dockview::DockViewV2 idle = decide_dock(obs);
	CHECK(idle.record.name == idle.record.label);
	obs.recording.phase = cordmux::ledger::RecordPhase::Recording;
	obs.recording.enabled_since_us.reset(); // 始めた時刻が分からない
	const dockview::DockViewV2 recording = decide_dock(obs);
	CHECK(recording.record.label.id == keys::kDockRecordStopButton);
	CHECK(recording.record.name == recording.record.label);
}

TEST_CASE("録画のボタンの幅は、経過時間を含むいちばん広い形でも測る")
{
	const dockview::DockViewV2 view = decide_dock(healthy());
	bool found = false;
	for (const cordmux::ledger::TextRef &label : view.record.width_labels)
		found = found || label.id == keys::kDockRecordStopElapsedButton;
	CHECK(found);
}

TEST_CASE("最初の設定のメッセージは、一覧に行がない間だけ一覧の場所にも置く")
{
	dockview::DockViewV2 view;
	dockview::Notice setup;
	setup.key = "VencordDirNotConfigured";
	setup.rule_name = "VencordDirNotConfigured";
	dockview::Notice other;
	other.key = "RecordSpaceLow";
	other.rule_name = "RecordSpaceLow";
	view.notices = {setup, other};
	std::vector<dockview::Notice> shown = cordmux::ledger::setup_notices_of(view);
	REQUIRE(shown.size() == 1);
	CHECK(shown[0].key == "VencordDirNotConfigured");
	// メインの行だけなら、一覧は空とみなす。
	view.people_list.pinned = PersonRow{};
	CHECK(cordmux::ledger::setup_notices_of(view).size() == 1);
	view.people_list.in_call.push_back(PersonRow{});
	CHECK(cordmux::ledger::setup_notices_of(view).empty());
	view.people_list.in_call.clear();
	view.people_list.sharing.push_back(PersonRow{});
	CHECK(cordmux::ledger::setup_notices_of(view).empty());
}

TEST_CASE("通話にいない人は、声の枠だけでも画面共有の枠を持っていても、一覧にも音声タブにも出さない")
{
	Observation obs = with_main_and_sub();
	PersonFact ghost = present("300", "ghost");
	ghost.in_roster = false;
	obs.people.push_back(ghost);
	const dockview::DockViewV2 view = decide_dock(obs);
	CHECK(find_row(view.people_list, "300") == nullptr);
	for (const dockview::AudioRowView &row : view.audio_tab.rows)
		CHECK(row.user_id != "300");
	// 画面共有の枠を持つ人も、通話にいない (参加者の一覧にいない) 間は、一覧にも音声タブにも出さない。通話を抜けた人は、
	// 枠を空けられるので、枠を持ち続けて一覧に残る人はいない。
	PersonFact gone = showing("400", "gone", 2);
	gone.in_roster = false;
	obs.people.push_back(gone);
	const dockview::DockViewV2 with_gone = decide_dock(obs);
	CHECK(find_row(with_gone.people_list, "400") == nullptr);
	for (const dockview::AudioRowView &row : with_gone.audio_tab.rows)
		CHECK(row.user_id != "400");
}

TEST_CASE("声の枠だけの人がいても、空の一覧の理由と最初の設定のメッセージは隠れない")
{
	Observation obs = healthy();
	obs.people.clear();
	obs.call.presence = Presence::NotInCall;
	obs.call.roster_count = 0;
	PersonFact ghost = present("300", "ghost");
	ghost.in_roster = false;
	obs.people.push_back(ghost);
	dockview::DockViewV2 view = decide_dock(obs);
	REQUIRE(view.empty.has_value());
	CHECK(view.empty->kind == dockview::EmptyKind::NotInCall);

	dockview::Notice setup;
	setup.key = "VencordDirNotConfigured";
	setup.rule_name = "VencordDirNotConfigured";
	view.notices = {setup};
	CHECK(cordmux::ledger::setup_notices_of(view).size() == 1);
}

TEST_CASE("接続が切れる前の声の枠だけの人も、切れた後に行にしない")
{
	Observation connected = with_main_and_sub();
	PersonFact ghost = present("300", "ghost");
	ghost.in_roster = false;
	connected.people.push_back(ghost);
	Observation obs = connected;
	obs.link.state = LinkState::Disconnected;
	obs.call = Call{};
	obs.people.clear();
	obs.last_known_people = connected.people;
	const dockview::PeopleListView list = decide_dock(obs).people_list;
	CHECK(find_row(list, "300") == nullptr);
	CHECK(find_row(list, "100") != nullptr);
}

TEST_CASE("Vesktop のアカウントは通話の外でも分かり、メインだけの構成の行に名前が出る")
{
	Observation obs = healthy();
	obs.people.clear();
	obs.call.presence = Presence::NotInCall;
	obs.call.roster_count = 0;
	obs.vesktop_user_id = "600";
	obs.vesktop_name = "me";
	const dockview::AccountsView accounts = decide_dock(obs).accounts;
	CHECK_FALSE(accounts.sub.has_value());
	CHECK(accounts.main.row.name.literal == "me");
	CHECK(accounts.main.row.state == AccountCallState::NoCall);
}

TEST_CASE("Vesktop のアカウントが通話の外でも、サブの行に名前が出て、[変更] の候補に並ばない")
{
	Observation obs = healthy();
	obs.people.clear();
	obs.call.presence = Presence::NotInCall;
	obs.call.roster_count = 0;
	obs.choices.broadcaster_user_id = "500";
	obs.vesktop_user_id = "600";
	obs.vesktop_name = "sub";
	obs.seen.people.push_back(SeenPerson{"600", "sub"});
	obs.seen.people.push_back(SeenPerson{"500", "main"});
	const dockview::AccountsView accounts = decide_dock(obs).accounts;
	REQUIRE(accounts.sub.has_value());
	CHECK(accounts.sub->row.name.literal == "sub");
	CHECK(accounts.sub->row.state == AccountCallState::NoCall);
	for (const dockview::MainAccountChoice &choice : accounts.main.choices)
		CHECK(choice.action.args.user_id != "600");

	// 通話に参加して行ができても、サブの行は消えない。
	PersonFact main = present("500", "main");
	PersonFact sub = present("600", "sub");
	sub.is_receiver = true;
	obs.people = {main, sub};
	obs.call.presence = Presence::InCall;
	obs.call.roster_count = 2;
	CHECK(decide_dock(obs).accounts.sub.has_value());
}

TEST_CASE("メインアカウントの画面共有は、自分の枠がなくても行の位置が左上で、配信の位置と同じ")
{
	Observation obs = healthy();
	obs.people.clear();
	obs.choices.broadcaster_user_id = "500";
	obs.choices.slot_order = cordmux::config::SlotOrder::Name;
	PersonFact main = showing("500", "zed", 3);
	PersonFact a = showing("100", "a", 1);
	PersonFact b = showing("200", "b", 2);
	main.is_self_frame = false;
	obs.people = {a, b, main};
	const dockview::PeopleListView list = decide_dock(obs).people_list;
	REQUIRE(list.pinned.has_value());
	CHECK(list.pinned->user_id == "500");
	CHECK(list.pinned->position == SlotPosition::TopLeft);
	CHECK(find_row(list, "100")->position == SlotPosition::TopRight);
	CHECK(find_row(list, "200")->position == SlotPosition::BottomLeft);
}
