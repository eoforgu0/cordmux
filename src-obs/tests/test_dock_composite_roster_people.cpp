// SPDX-License-Identifier: Apache-2.0

// dock/build_observation.h の、合成のソースの配置の入力に入れる値を決める関数の単体テスト。名前ラベルを出す人
// (composite_roster_people) と、その人を枠と画面共有していない参加者の並びのどちらに描くか (合成のソースと同じ
// sources/composite_label_plan.h の composite_tiles_labeled と composite_roster_shown)、名前ラベルと並びに渡すアバター
// (decor_avatar)、最後のフレームを残している枠があるか (pipeline/pipeline.h の link_lost_frame_held)、自分の枠の人の声を
// 聞けるか (self_frame_hears_voice)、自分の枠の人 (self_frame_user_id) を確認する。テストの人の名前は架空の名前で、userId は意味のない短い文字列にする。

#include "doctest.h"

#include "dock/build_observation.h"
#include "sources/composite_input.h"
#include "sources/composite_label_plan.h"
#include "sources/composite_layout.h"
#include "sources/screen_end.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

using cordmux::config::SlotKind;
using cordmux::dock::composite_roster_people;
using cordmux::dock::self_frame_hears_voice;
using cordmux::dock::self_frame_user_id;
using cordmux::ledger::SeenPerson;
using cordmux::pipeline::PipelineSnapshot;
using cordmux::pipeline::SlotSnapshot;
using cordmux::sources::composite_roster_shown;
using cordmux::sources::CompositeInput;
using cordmux::sources::CompositeRosterPerson;

namespace {

// 通話の参加者 (facts の roster) と、表示名と Bot の印 (profiles) を持つスナップショット。names の空の名前は、表示名を
// 読めなかった人 (キーごと省かれた人)。
PipelineSnapshot snapshot_with(const std::vector<std::pair<std::string, std::string>> &names,
			       const std::set<std::string> &bots = {})
{
	PipelineSnapshot snapshot;
	cordmux::pipeline::UserIdSet roster;
	cordmux::ws::ProfilesMessage message;
	for (const auto &[user_id, name] : names) {
		roster.user_ids.push_back(user_id);
		cordmux::ws::ProfileEntry entry;
		entry.user_id = user_id;
		if (!name.empty())
			entry.display_name = name;
		entry.bot = bots.count(user_id) != 0;
		message.profiles.push_back(entry);
	}
	roster.sent_count = static_cast<int>(roster.user_ids.size());
	snapshot.facts.received = true;
	snapshot.facts.roster = roster;
	snapshot.profiles = cordmux::pipeline::make_profiles_snapshot(message, 1);
	return snapshot;
}

SlotSnapshot voice_slot(int slot, const std::string &owner, bool present = true)
{
	SlotSnapshot row;
	row.kind = SlotKind::Voice;
	row.slot = slot;
	row.owner_user_id = owner;
	row.voice_user_id = owner;
	row.output_allowed = true;
	row.voice_present = present;
	return row;
}

std::vector<std::string> ids_of(const std::vector<CompositeRosterPerson> &people)
{
	std::vector<std::string> out;
	for (const CompositeRosterPerson &person : people)
		out.push_back(person.user_id);
	return out;
}

const CompositeRosterPerson *find(const std::vector<CompositeRosterPerson> &people, const std::string &user_id)
{
	for (const CompositeRosterPerson &person : people) {
		if (person.user_id == user_id)
			return &person;
	}
	return nullptr;
}

// 画面共有の枠 (枠の番号と人) と自分の枠から、合成のソースの配置の入力を作る (dock_poll_layout.cpp の push_composite_input と
// 同じ make_composite_input)。
CompositeInput input_with(const std::vector<std::pair<int, std::string>> &shares, bool self_shown,
			  const std::string &self_user_id = "", const std::string &focus_user_id = "")
{
	std::vector<int> slots;
	std::vector<cordmux::sources::SlotPerson> people;
	for (const auto &[slot, user_id] : shares) {
		slots.push_back(slot);
		cordmux::sources::SlotPerson person;
		person.slot = slot;
		person.name = user_id;
		person.user_id = user_id;
		people.push_back(person);
	}
	CompositeInput input = cordmux::sources::make_composite_input(slots, people, self_shown, true, 0);
	input.self_user_id = self_user_id;
	input.focus_user_id = focus_user_id;
	return input;
}

// 合成のソースが描くときの判断 (sources/composite_label_plan.h の composite_tiles_labeled、composite_labeled_tile_users、
// composite_roster_shows。CompositeDecor::draw と同じ関数) を通して、名前ラベルを枠に描く人と、並びに描く人を返す。配置は
// 1920x1080 のキャンバスで plan_composite_layout が決める。video はタイルごとの、映像を描いたか (空なら全部描いた)。
struct Drawn {
	std::vector<std::string> tiles;  // 名前ラベル付きで描く枠の人 (タイルの順)
	std::vector<std::string> roster; // 並びに描く人 (渡した順)
};

Drawn drawn_of(const std::vector<CompositeRosterPerson> &people, const CompositeInput &input, bool hide_when_single,
	       std::vector<bool> video = {}, bool focus_extra_ready = false, bool show_in_slot = true)
{
	cordmux::sources::CompositeLabelLook look;
	look.hide_when_single = hide_when_single;
	look.show_in_slot = show_in_slot;
	cordmux::sources::CompositeLayoutRequest request = cordmux::sources::make_layout_request(input, 1920, 1080);
	request.focus_extra_ready = focus_extra_ready;
	const cordmux::sources::CompositeLayout layout = cordmux::sources::plan_composite_layout(request);
	if (video.empty())
		video.assign(layout.tiles.size(), true);
	const std::vector<bool> labeled = cordmux::sources::composite_tiles_labeled(look, 1920, 1080, layout.tiles, video);
	Drawn out;
	out.tiles = cordmux::sources::composite_labeled_tile_users(labeled,
								    cordmux::sources::composite_tile_user_ids(input, layout.tiles));
	out.roster = ids_of(composite_roster_shown(people, out.tiles));
	return out;
}

// 名前ラベルを出す人 (people) の全員が、枠か並びのどちらか 1 か所にだけ出るか。
bool each_once(const std::vector<CompositeRosterPerson> &people, const Drawn &drawn)
{
	for (const CompositeRosterPerson &person : people) {
		const auto in = [&person](const std::vector<std::string> &ids) {
			return static_cast<int>(std::count(ids.begin(), ids.end(), person.user_id));
		};
		if (in(drawn.tiles) + in(drawn.roster) != 1)
			return false;
	}
	return true;
}

} // namespace

// ---- 名前ラベルを出す人 (composite_roster_people) ----

TEST_CASE("名前ラベルを出す人には、通話の参加者を名前と一緒に、画面共有しているかに依らず全員入れる")
{
	const PipelineSnapshot snapshot = snapshot_with({{"a", "あおい"}, {"b", "ken"}, {"c", "sakura"}});
	const std::vector<CompositeRosterPerson> people =
		composite_roster_people(snapshot, cordmux::config::Settings{}, {}, "");
	CHECK(ids_of(people) == std::vector<std::string>{"a", "b", "c"});
	REQUIRE(find(people, "a") != nullptr);
	CHECK(find(people, "a")->name == "あおい");
	CHECK(find(people, "a")->hears_voice);
}

TEST_CASE("サブアカウントの構成では、受け取りに使うアカウントを外し、メインアカウントはほかの人と同じに入れる")
{
	PipelineSnapshot snapshot = snapshot_with({{"main", "めいん"}, {"sub", "うけとり"}, {"a", "あおい"}});
	snapshot.facts.self_user_id = "sub";
	cordmux::config::Settings settings;
	settings.broadcaster_user_id = "main";
	const std::vector<CompositeRosterPerson> people = composite_roster_people(snapshot, settings, {}, "main");
	CHECK(ids_of(people) == std::vector<std::string>{"main", "a"});
}

TEST_CASE("メインアカウントだけの構成では、Vesktop のアカウントを外さない (配信する本人で、自分の枠の人)")
{
	PipelineSnapshot snapshot = snapshot_with({{"me", "わたし"}, {"a", "あおい"}});
	snapshot.facts.self_user_id = "me";
	cordmux::config::Settings settings; // メインアカウントの欄は空
	const std::vector<CompositeRosterPerson> people = composite_roster_people(snapshot, settings, {}, "me");
	CHECK(ids_of(people) == std::vector<std::string>{"me", "a"});
	// Discord が本人の声を返さないので、声を聞けない (光らせず、暗くもしない。自分の枠と同じ規則)。
	CHECK_FALSE(find(people, "me")->hears_voice);
	CHECK(find(people, "a")->hears_voice);
	// 自分の声の経路があれば聞ける (自分の枠と同じ self_frame_hears_voice)。
	snapshot.slots.push_back(voice_slot(1, "me"));
	const std::vector<CompositeRosterPerson> heard = composite_roster_people(snapshot, settings, {}, "me");
	REQUIRE(find(heard, "me") != nullptr);
	CHECK(find(heard, "me")->hears_voice);
	// メインアカウントの欄に Vesktop のアカウントと同じ人を入れた形も、メインアカウントだけの構成と同じに扱う。
	settings.broadcaster_user_id = "me";
	CHECK(ids_of(composite_roster_people(snapshot, settings, {}, "me")) == std::vector<std::string>{"me", "a"});
}

TEST_CASE("「通話のみ時ラベル非表示」を付けた人を外す。付けていない行の人は入れる")
{
	const PipelineSnapshot snapshot = snapshot_with({{"a", "あおい"}, {"b", "ken"}});
	SeenPerson hidden;
	hidden.user_id = "a";
	hidden.roster_hidden = true;
	SeenPerson shown;
	shown.user_id = "b";
	shown.always_on = true;
	const std::vector<CompositeRosterPerson> people =
		composite_roster_people(snapshot, cordmux::config::Settings{}, {hidden, shown}, "");
	CHECK(ids_of(people) == std::vector<std::string>{"b"});
}

TEST_CASE("ほかの人の行の「使う声」に選ばれている人を外す。選んだ人は入れる")
{
	// 選んだ人 (a) の声の組は a で、b の声が a の名前ラベルを光らせる (pipeline/composite_audio_plan.h の mixer_key_for。
	// test_pipeline_composite_audio_plan.cpp の「使う声は、枠に載っている人の組に入る」)。並びの a も同じ組の発言で光る。
	const PipelineSnapshot snapshot = snapshot_with({{"a", "あおい"}, {"b", "ken"}, {"c", "sakura"}});
	SeenPerson borrower;
	borrower.user_id = "a";
	borrower.voice_source_user_id = "b";
	const std::vector<CompositeRosterPerson> people =
		composite_roster_people(snapshot, cordmux::config::Settings{}, {borrower}, "");
	CHECK(ids_of(people) == std::vector<std::string>{"a", "c"});

	// 衝突して使えない組 (2 人が同じ声を選んだ。config::usable_voice_links が落とす) では、声の人を外さない。
	SeenPerson second;
	second.user_id = "c";
	second.voice_source_user_id = "b";
	const std::vector<CompositeRosterPerson> clashed =
		composite_roster_people(snapshot, cordmux::config::Settings{}, {borrower, second}, "");
	CHECK(ids_of(clashed) == std::vector<std::string>{"a", "b", "c"});
}

TEST_CASE("Bot のアカウントは、roster_show_bots が偽 (既定) なら外し、真なら入れる")
{
	const PipelineSnapshot snapshot = snapshot_with({{"a", "あおい"}, {"bot", "ろぼ"}}, {"bot"});
	cordmux::config::Settings settings;
	CHECK_FALSE(settings.roster_show_bots);
	CHECK(ids_of(composite_roster_people(snapshot, settings, {}, "")) == std::vector<std::string>{"a"});
	settings.roster_show_bots = true;
	CHECK(ids_of(composite_roster_people(snapshot, settings, {}, "")) == std::vector<std::string>{"a", "bot"});
}

TEST_CASE("名前は cordmux での名前を先に使い、名前を取得できない人は入れない")
{
	const PipelineSnapshot snapshot = snapshot_with({{"a", "あおい"}, {"b", ""}, {"c", ""}});
	SeenPerson renamed;
	renamed.user_id = "a";
	renamed.name_override = "あおちゃん";
	SeenPerson renamed_c;
	renamed_c.user_id = "c";
	renamed_c.name_override = "しー";
	const std::vector<CompositeRosterPerson> people =
		composite_roster_people(snapshot, cordmux::config::Settings{}, {renamed, renamed_c}, "");
	// b は表示名を読めず、cordmux での名前もないので入れない (userId を名前の代わりに描かない)。
	CHECK(ids_of(people) == std::vector<std::string>{"a", "c"});
	CHECK(find(people, "a")->name == "あおちゃん");
	CHECK(find(people, "c")->name == "しー");
}

TEST_CASE("roster_show が偽か、通話の参加者をまだ読めていないときは、誰も入れない")
{
	PipelineSnapshot snapshot = snapshot_with({{"a", "あおい"}});
	cordmux::config::Settings settings;
	settings.roster_show = false;
	CHECK(composite_roster_people(snapshot, settings, {}, "").empty());

	snapshot.facts.roster.reset(); // 送信側がまだ読めていない (キーごと省かれた)
	CHECK(composite_roster_people(snapshot, cordmux::config::Settings{}, {}, "").empty());
}

TEST_CASE("同じ userId が通話の参加者に 2 回あっても、1 人だけ入れる")
{
	PipelineSnapshot snapshot = snapshot_with({{"a", "あおい"}});
	snapshot.facts.roster->user_ids.push_back("a");
	snapshot.facts.roster->user_ids.push_back("");
	CHECK(ids_of(composite_roster_people(snapshot, cordmux::config::Settings{}, {}, "")) ==
	      std::vector<std::string>{"a"});
}

TEST_CASE("並びの人の声は、声の枠に上限がないので、声の枠が何個埋まっていても聞ける")
{
	PipelineSnapshot snapshot = snapshot_with({{"a", "あおい"}, {"b", "ken"}});
	for (int slot = 1; slot <= 30; ++slot)
		snapshot.slots.push_back(voice_slot(slot, slot == 1 ? std::string("a") : "v" + std::to_string(slot)));
	const std::vector<CompositeRosterPerson> people =
		composite_roster_people(snapshot, cordmux::config::Settings{}, {}, "");
	REQUIRE(find(people, "a") != nullptr);
	REQUIRE(find(people, "b") != nullptr);
	CHECK(find(people, "a")->hears_voice);
	CHECK(find(people, "b")->hears_voice);
}

// ---- 名前ラベルを出す人を、枠と並びのどちらに描くか (合成のソースと同じ判断) ----

TEST_CASE("名前ラベル付きで描く枠がある人は枠に、ない人 (画面共有していない人、枠に載せていない画面共有の人) は並びに出す")
{
	// a と b が枠に載っている (2 枚)。c は通話だけ、d は画面共有しているが枠に載せていない (枠を持たない)。
	const PipelineSnapshot snapshot = snapshot_with({{"a", "あおい"}, {"b", "ken"}, {"c", "sakura"}, {"d", "daichi"}});
	const std::vector<CompositeRosterPerson> people =
		composite_roster_people(snapshot, cordmux::config::Settings{}, {}, "");
	const Drawn drawn = drawn_of(people, input_with({{1, "a"}, {2, "b"}}, false), true);
	CHECK(drawn.tiles == std::vector<std::string>{"a", "b"});
	CHECK(drawn.roster == std::vector<std::string>{"c", "d"});
	CHECK(each_once(people, drawn));
}

TEST_CASE("名前ラベルを枠に描かない設定では、枠の人を全員並びに出し、並びも出さない設定ならどこにも出さない")
{
	// 自分の枠 (me) と a、b が枠に載っている。c は通話だけ。
	const PipelineSnapshot snapshot = snapshot_with({{"me", "zz"}, {"a", "あおい"}, {"b", "ken"}, {"c", "sakura"}});
	const std::vector<CompositeRosterPerson> people =
		composite_roster_people(snapshot, cordmux::config::Settings{}, {}, "me");
	const CompositeInput input = input_with({{1, "a"}, {2, "b"}}, true, "me");
	const Drawn off = drawn_of(people, input, true, {}, false, false);
	CHECK(off.tiles.empty());
	CHECK(off.roster == std::vector<std::string>{"me", "a", "b", "c"});
	CHECK(each_once(people, off));
	// 枠に描く (既定) なら、前と同じ。
	const Drawn on = drawn_of(people, input, true);
	CHECK(on.tiles.size() == 3);
	CHECK(on.roster == std::vector<std::string>{"c"});
	// 並びも出さない設定 (roster_show が偽) では、名前ラベルを出す人がいないので、枠にも並びにも出ない。
	cordmux::config::Settings no_roster;
	no_roster.roster_show = false;
	const std::vector<CompositeRosterPerson> none = composite_roster_people(snapshot, no_roster, {}, "me");
	const Drawn nowhere = drawn_of(none, input, true, {}, false, false);
	CHECK(nowhere.tiles.empty());
	CHECK(nowhere.roster.empty());
}

TEST_CASE("枠が足りずに描けない人 (dropped) は並びに出す")
{
	// 自分の枠と 4 人の画面共有。名前順の最後の 1 人 (e) は描けない。
	const PipelineSnapshot snapshot =
		snapshot_with({{"me", "zz"}, {"a", "aoi"}, {"b", "ben"}, {"c", "chika"}, {"e", "emi"}});
	const std::vector<CompositeRosterPerson> people =
		composite_roster_people(snapshot, cordmux::config::Settings{}, {}, "me");
	CompositeInput input = input_with({{1, "a"}, {2, "b"}, {3, "c"}, {4, "e"}}, true, "me");
	for (cordmux::sources::CompositeShare &share : input.shares)
		share.name = *snapshot.profiles.display_name_of(share.user_id);
	const Drawn drawn = drawn_of(people, input, true);
	CHECK(drawn.roster == std::vector<std::string>{"e"});
	CHECK(each_once(people, drawn));
}

TEST_CASE("1 枚を全面に置いて [1人のときは非表示] が名前ラベルを隠すとき、その人をほかの人と同じ並びに出す")
{
	const PipelineSnapshot snapshot = snapshot_with({{"a", "あおい"}, {"b", "ken"}, {"c", "sakura"}});
	const std::vector<CompositeRosterPerson> people =
		composite_roster_people(snapshot, cordmux::config::Settings{}, {}, "");
	const CompositeInput single = input_with({{1, "b"}}, false);
	const Drawn hidden = drawn_of(people, single, true);
	CHECK(hidden.tiles.empty());
	CHECK(hidden.roster == std::vector<std::string>{"a", "b", "c"});
	// 設定が偽なら、枠に名前ラベルを出すので並びには出さない。
	const Drawn shown = drawn_of(people, single, false);
	CHECK(shown.tiles == std::vector<std::string>{"b"});
	CHECK(shown.roster == std::vector<std::string>{"a", "c"});
	// 2 枚のときは全面でないので、名前ラベルを隠さない。
	const Drawn two = drawn_of(people, input_with({{1, "b"}, {2, "c"}}, false), true);
	CHECK(two.tiles == std::vector<std::string>{"b", "c"});
	CHECK(two.roster == std::vector<std::string>{"a"});
	CHECK(each_once(people, hidden));
	CHECK(each_once(people, shown));
	CHECK(each_once(people, two));
}

TEST_CASE("一時的な全画面の間は、描かれていない枠の人を並びに出し、全画面の 1 人の名前ラベルを隠すときはその人も並びに出す")
{
	const PipelineSnapshot snapshot = snapshot_with({{"a", "あおい"}, {"b", "ken"}, {"c", "sakura"}, {"d", "daichi"}});
	const std::vector<CompositeRosterPerson> people =
		composite_roster_people(snapshot, cordmux::config::Settings{}, {}, "");
	// a、b、c が枠に載っていて、b を全画面にしている。a と c の枠は描かれていない。
	const CompositeInput focused = input_with({{1, "a"}, {2, "b"}, {3, "c"}}, false, "", "b");
	const Drawn labeled = drawn_of(people, focused, false);
	CHECK(labeled.tiles == std::vector<std::string>{"b"});
	CHECK(labeled.roster == std::vector<std::string>{"a", "c", "d"});
	// [1人のときは非表示] が全画面の 1 人の名前ラベルを隠すときは、その人も並びに出す。
	const Drawn hidden = drawn_of(people, focused, true);
	CHECK(hidden.tiles.empty());
	CHECK(hidden.roster == std::vector<std::string>{"a", "b", "c", "d"});
	CHECK(each_once(people, labeled));
	CHECK(each_once(people, hidden));
}

TEST_CASE("枠に載せていない人の全画面は、映像が届いて全面に描く間はその枠に、届くまでは並びに出す")
{
	const PipelineSnapshot snapshot = snapshot_with({{"a", "あおい"}, {"x", "xeno"}});
	const std::vector<CompositeRosterPerson> people =
		composite_roster_people(snapshot, cordmux::config::Settings{}, {}, "");
	CompositeInput input = input_with({{1, "a"}}, false, "", "x");
	input.focus_extra_slot = cordmux::config::kFocusOnlySlot;
	// 映像が届くまでは、全画面でないときの配置 (a の 1 枚) のまま。x はどの枠にも描かれていない。
	const Drawn waiting = drawn_of(people, input, false);
	CHECK(waiting.tiles == std::vector<std::string>{"a"});
	CHECK(waiting.roster == std::vector<std::string>{"x"});
	// 映像が届くと、x を全画面だけで映す子に全面で描く。
	const Drawn ready = drawn_of(people, input, false, {}, true);
	CHECK(ready.tiles == std::vector<std::string>{"x"});
	CHECK(ready.roster == std::vector<std::string>{"a"});
	CHECK(each_once(people, waiting));
	CHECK(each_once(people, ready));
}

TEST_CASE("映像がまだ届いていない枠の人は、名前ラベルを描かないので並びに出す")
{
	const PipelineSnapshot snapshot = snapshot_with({{"a", "あおい"}, {"b", "ken"}});
	const std::vector<CompositeRosterPerson> people =
		composite_roster_people(snapshot, cordmux::config::Settings{}, {}, "");
	const Drawn drawn = drawn_of(people, input_with({{1, "a"}, {2, "b"}}, false), true, {true, false});
	CHECK(drawn.tiles == std::vector<std::string>{"a"});
	CHECK(drawn.roster == std::vector<std::string>{"b"});
}

TEST_CASE("メインアカウント: 自分の枠に名前ラベル付きで描く間は枠に、自分の枠がないか名前ラベルなしで描く間は並びに出す")
{
	PipelineSnapshot snapshot = snapshot_with({{"main", "めいん"}, {"sub", "うけとり"}, {"a", "あおい"}, {"b", "ken"}});
	snapshot.facts.self_user_id = "sub";
	cordmux::config::Settings settings;
	settings.broadcaster_user_id = "main";
	const std::vector<CompositeRosterPerson> people = composite_roster_people(snapshot, settings, {}, "main");
	CHECK(ids_of(people) == std::vector<std::string>{"main", "a", "b"}); // サブアカウントは外す

	// 自分の枠と b の画面共有 (2 枚)。自分の枠に名前ラベルが出ているので、並びには出さない。
	const Drawn framed = drawn_of(people, input_with({{1, "b"}}, true, "main"), true);
	CHECK(framed.tiles == std::vector<std::string>{"main", "b"});
	CHECK(framed.roster == std::vector<std::string>{"a"});
	// 自分の枠を出していない: ほかの通話だけの人と同じく並びに出す。
	const Drawn no_frame = drawn_of(people, input_with({{1, "a"}, {2, "b"}}, false, "main"), true);
	CHECK(no_frame.roster == std::vector<std::string>{"main"});
	// 自分の枠だけ (全面の 1 枚) で [1人のときは非表示] が真: 名前ラベルを隠し、並びに出す。偽なら枠に出す。
	const CompositeInput self_only = input_with({}, true, "main");
	CHECK(drawn_of(people, self_only, true).roster == std::vector<std::string>{"main", "a", "b"});
	CHECK(drawn_of(people, self_only, false).tiles == std::vector<std::string>{"main"});
	// 自分の枠の映像がまだない (名前ラベルなしで置いている) ときも並びに出す。
	CHECK(drawn_of(people, input_with({{1, "b"}}, true, "main"), true, {false, true}).roster ==
	      std::vector<std::string>{"main", "a"});
	// 全画面で b を映している間は、自分の枠は描かれないので並びに出す。
	CHECK(drawn_of(people, input_with({{1, "b"}}, true, "main", "b"), false).roster ==
	      std::vector<std::string>{"main", "a"});
	CHECK(each_once(people, framed));
	CHECK(each_once(people, no_frame));

	// 「通話のみ時ラベル非表示」を付ければ、名前ラベルを出す人から外れ、並びには出ない (枠の名前ラベルは変わらない)。
	SeenPerson hidden;
	hidden.user_id = "main";
	hidden.roster_hidden = true;
	const std::vector<CompositeRosterPerson> without = composite_roster_people(snapshot, settings, {hidden}, "main");
	CHECK(drawn_of(without, input_with({{1, "a"}, {2, "b"}}, false, "main"), true).roster.empty());
	CHECK(drawn_of(without, self_only, true).roster == std::vector<std::string>{"a", "b"});
}

TEST_CASE("Bot、サブアカウント、「通話のみ時ラベル非表示」の人、使う声の人は、枠に描かれていないときも並びに出さない")
{
	PipelineSnapshot snapshot = snapshot_with(
		{{"main", "めいん"}, {"sub", "うけとり"}, {"bot", "ろぼ"}, {"h", "hide"}, {"v", "voice"}, {"a", "あおい"}},
		{"bot"});
	snapshot.facts.self_user_id = "sub";
	cordmux::config::Settings settings;
	settings.broadcaster_user_id = "main";
	SeenPerson hidden;
	hidden.user_id = "h";
	hidden.roster_hidden = true;
	SeenPerson borrower;
	borrower.user_id = "a";
	borrower.voice_source_user_id = "v";
	const std::vector<CompositeRosterPerson> people =
		composite_roster_people(snapshot, settings, {hidden, borrower}, "main");
	// 全画面で a を映している (ほかの枠は描かれない) ときも、外した人は並びに出ない。
	const Drawn drawn = drawn_of(people, input_with({{1, "a"}, {2, "bot"}, {3, "h"}}, false, "", "a"), false);
	CHECK(drawn.tiles == std::vector<std::string>{"a"});
	CHECK(drawn.roster == std::vector<std::string>{"main"});
	CHECK(each_once(people, drawn));
}

TEST_CASE("全画面の始まりと終わりで、名前ラベルを出す人は毎回、枠か並びのどちらか 1 か所にだけ出る (消えず、二重に出ない)")
{
	PipelineSnapshot snapshot =
		snapshot_with({{"main", "めいん"}, {"a", "あおい"}, {"b", "ken"}, {"c", "sakura"}, {"d", "daichi"}});
	snapshot.facts.self_user_id = "main";
	const std::vector<CompositeRosterPerson> people =
		composite_roster_people(snapshot, cordmux::config::Settings{}, {}, "main");
	const std::vector<std::pair<int, std::string>> shares{{1, "a"}, {2, "b"}, {3, "c"}};
	// 全画面でない -> b を全画面 -> 自分の枠を全画面 -> 戻す。[1人のときは非表示] の両方で調べる。
	for (const bool hide : {false, true}) {
		CAPTURE(hide);
		for (const std::string &focus : {std::string(), std::string("b"), std::string("main"), std::string()}) {
			CAPTURE(focus);
			const Drawn drawn = drawn_of(people, input_with(shares, true, "main", focus), hide);
			CHECK(each_once(people, drawn));
		}
	}
}

TEST_CASE("並びの名前ラベルが見分けに使う名前は、見えている名前から本人を除いて並べ替え、人が枠と並びの間を移っても変わらない")
{
	using cordmux::sources::composite_roster_others;
	// 全画面の前: 枠に a、b、並びに c。b を全画面にした後: 枠に b、並びに a、c。c の見分けに使う名前は同じ。
	const std::vector<std::string> before{"a", "b", "c"};
	const std::vector<std::string> after{"b", "a", "c"};
	CHECK(composite_roster_others(before, "c") == composite_roster_others(after, "c"));
	CHECK(composite_roster_others(before, "c") == std::vector<std::string>{"a", "b"});
	// 同じ名前の人が 2 人いれば、本人の分を 1 つだけ除く。
	CHECK(composite_roster_others({"x", "x", "y"}, "x") == std::vector<std::string>{"x", "y"});
}

// ---- そのほか ----

TEST_CASE("自分の枠の人の声を聞けるのは、その人の声の枠がいま声の経路を持っているときだけ")
{
	std::vector<SlotSnapshot> slots;
	// メインアカウントだけの構成: 自分の声は返ってこないので、声の枠がない。
	CHECK_FALSE(self_frame_hears_voice(slots, "main"));
	slots.push_back(voice_slot(1, "main", false)); // 割り当ては残っているが、いま経路がない
	CHECK_FALSE(self_frame_hears_voice(slots, "main"));
	slots[0].voice_present = true;
	CHECK(self_frame_hears_voice(slots, "main"));
	// 自分の枠の人が分からなければ聞けない。ほかの人の声の枠は関係しない。
	CHECK_FALSE(self_frame_hears_voice(slots, ""));
	CHECK_FALSE(self_frame_hears_voice(slots, "other"));
}

TEST_CASE("アバターは profiles の画像を使い、画像がまだ分からない間は前に渡した画像を使う")
{
	using cordmux::dock::decor_avatar;
	using cordmux::pipeline::AvatarImage;
	using cordmux::pipeline::ProfilesSnapshot;
	const std::vector<std::uint8_t> old_png{1, 2, 3};
	const std::vector<std::uint8_t> new_png{7, 8};

	// 接続が切れて profiles を捨てた後 (届いていない): 前の画像。前の画像がなければ空。
	ProfilesSnapshot profiles;
	CHECK(decor_avatar(profiles, "a", &old_png) == old_png);
	CHECK(decor_avatar(profiles, "a", nullptr).empty());

	// 接続し直して profiles が届いたが、画像はまだ届いていない (バージョンは分かっている): 前の画像。
	cordmux::ws::ProfilesMessage message;
	cordmux::ws::ProfileEntry with_avatar;
	with_avatar.user_id = "a";
	with_avatar.avatar_key = "k2";
	cordmux::ws::ProfileEntry no_avatar;
	no_avatar.user_id = "b";
	no_avatar.avatar_key = "";
	cordmux::ws::ProfileEntry unread;
	unread.user_id = "c"; // バージョンを読めていない
	message.profiles = {with_avatar, no_avatar, unread};
	profiles = cordmux::pipeline::make_profiles_snapshot(message, 1);
	CHECK(decor_avatar(profiles, "a", &old_png) == old_png);
	CHECK(decor_avatar(profiles, "c", &old_png) == old_png);
	CHECK(decor_avatar(profiles, "x", &old_png) == old_png); // profiles にいない人
	// アバターを設定していない人は、前の画像があっても空。
	CHECK(decor_avatar(profiles, "b", &old_png).empty());

	// 画像が届いたら、その画像。
	AvatarImage image;
	image.key = "k2";
	image.png = std::make_shared<const std::vector<std::uint8_t>>(new_png);
	profiles.avatars["a"] = image;
	CHECK(decor_avatar(profiles, "a", &old_png) == new_png);
	CHECK(decor_avatar(profiles, "a", nullptr) == new_png);
}

TEST_CASE("最後のフレームを残している枠があるのは、配置を保ち、映像が戻らず、まだ空けていない間だけ")
{
	using cordmux::pipeline::link_lost_frame_held;
	SlotSnapshot row;
	row.kind = SlotKind::Screen;
	row.slot = 1;
	row.owner_user_id = "a";
	std::vector<SlotSnapshot> slots{row};
	CHECK_FALSE(link_lost_frame_held(slots)); // 切れていない
	slots[0].held_after_link_lost = true;
	CHECK(link_lost_frame_held(slots));
	slots[0].share_present = true; // 映像が戻った
	CHECK_FALSE(link_lost_frame_held(slots));
	slots[0].share_present = false;
	slots[0].held_frame_cleared = true; // 最後のフレームを空けた
	CHECK_FALSE(link_lost_frame_held(slots));
}

TEST_CASE("自分の枠の人は、メインアカウントの欄があればその人、空なら Vesktop のアカウント")
{
	PipelineSnapshot snapshot;
	cordmux::config::Settings settings;
	CHECK(self_frame_user_id(settings, snapshot).empty());
	snapshot.facts.self_user_id = "vesktop";
	CHECK(self_frame_user_id(settings, snapshot) == "vesktop");
	settings.broadcaster_user_id = "main";
	CHECK(self_frame_user_id(settings, snapshot) == "main");
}

// ---- [1人のときは非表示] の人を、画面共有していない参加者の並びに描く ----

TEST_CASE("受信をやり直す間は枠の映像を空けないので、名前ラベルは枠に残り、画面共有していない参加者の並びへ移らない")
{
	// 受信のやり直し (pipeline/receive_restart.h) は、デコードの経路を作り直すだけで、合成のソースの子を空けない。子の大きさが
	// 0 にならないので、合成のソースは映像を描いたタイル (has_video が真) のまま扱い、名前ラベルを枠に描く。
	const cordmux::sources::ScreenEndAction action =
		cordmux::sources::screen_end_action(cordmux::sources::ScreenEndEvent::ReceiveRestarted);
	CHECK_FALSE(action.clear_now);
	CHECK_FALSE(action.hold_then_clear);
	CHECK_FALSE(action.relayout);
	CHECK_FALSE(action.clear_owner);

	const PipelineSnapshot snapshot = snapshot_with({{"a", "あおい"}, {"b", "ken"}, {"c", "sakura"}});
	const std::vector<CompositeRosterPerson> people =
		composite_roster_people(snapshot, cordmux::config::Settings{}, {}, "");
	const CompositeInput input = input_with({{1, "a"}, {2, "b"}}, false);
	// やり直しの間 (a の映像は止まっているが、最後のフレームが残っている)。
	const Drawn during = drawn_of(people, input, true, {true, true});
	CHECK(during.tiles == std::vector<std::string>{"a", "b"});
	CHECK(during.roster == std::vector<std::string>{"c"});
	CHECK(each_once(people, during));
	// 仮に枠を空けていたら (子の大きさが 0)、a は並びへ移っていた。やり直しではこの形にならない。
	const Drawn cleared = drawn_of(people, input, true, {false, true});
	CHECK(cleared.tiles == std::vector<std::string>{"b"});
	CHECK(cleared.roster == std::vector<std::string>{"a", "c"});
}
