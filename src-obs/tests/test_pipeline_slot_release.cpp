// SPDX-License-Identifier: Apache-2.0

// pipeline/slot_release.h (画面共有をやめた人と通話から抜けた人の枠を空ける判断) の単体テスト。pipeline.cpp が facts を
// 受け取るたびと configure を適用するたびに呼ぶ判断を、割り当て表と StreamRouter と組み合わせて確認する。枠を空けた後の載り方 (手動、自動、
// 「自動で載せる」、チェックを外した人) は StreamRouter の規則のままで、ここでは空けた後の動きも通して確認する。

#include "doctest.h"

#include "pipeline/slot_release.h"
#include "pipeline/stream_router.h"

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using cordmux::config::AllocationTable;
using cordmux::config::SlotKind;
using cordmux::pipeline::AssignPolicy;
using cordmux::pipeline::FactsSnapshot;
using cordmux::pipeline::SlotToRelease;
using cordmux::pipeline::screen_slots_to_release;
using cordmux::pipeline::voice_slots_to_release;
using cordmux::pipeline::StreamRouter;
using cordmux::ws::ConfigureMessage;
using cordmux::ws::StreamConfig;
using cordmux::ws::StreamKind;

namespace {

StreamConfig make_stream(uint32_t stream_id, const std::string &user_id, StreamKind kind)
{
	StreamConfig c;
	c.stream_id = stream_id;
	c.user_id = user_id;
	c.kind = kind;
	return c;
}

// 通話の参加者 (roster) と画面共有している人 (sharing) がそろった facts。
FactsSnapshot facts_of(std::vector<std::string> roster, std::vector<std::string> sharing)
{
	FactsSnapshot facts;
	facts.received = true;
	facts.sharing_valid = true;
	facts.sharing.user_ids = std::move(sharing);
	facts.sharing.sent_count = static_cast<int>(facts.sharing.user_ids.size());
	cordmux::pipeline::UserIdSet set;
	set.user_ids = std::move(roster);
	set.sent_count = static_cast<int>(set.user_ids.size());
	facts.roster = std::move(set);
	return facts;
}

// 直前の configure に載っている声のストリームの人。
std::set<std::string> live_voice_of(const ConfigureMessage &msg)
{
	std::set<std::string> out;
	for (const StreamConfig &s : msg.streams) {
		if (s.kind == StreamKind::Voice)
			out.insert(s.user_id);
	}
	return out;
}

// 判断のとおりに枠を空ける (Pipeline::Impl::release_departed_locked と同じ手順。画面の枠は経路を枠 0 へ付け替え、
// 声の枠は経路を触らない)。live は直前の configure の声のストリームの人、screens は画面の枠も見るか。
std::vector<SlotToRelease> release_departed(AllocationTable &table, StreamRouter &router, const FactsSnapshot &facts,
					    const std::map<std::string, std::string> &links = {},
					    const std::set<std::string> &live = {}, bool screens = true)
{
	std::vector<SlotToRelease> departed;
	if (screens)
		departed = screen_slots_to_release(table, facts);
	const std::vector<SlotToRelease> voices = voice_slots_to_release(table, facts, links, live);
	departed.insert(departed.end(), voices.begin(), voices.end());
	for (const SlotToRelease &r : departed) {
		table.release(r.kind, r.slot);
		if (r.kind == SlotKind::Screen)
			router.retarget_user(r.owner, r.kind, 0);
	}
	return departed;
}

// 両方を見る小さな関数 (判断だけを調べるテスト用)。
std::vector<SlotToRelease> slots_to_release(const AllocationTable &table, const FactsSnapshot &facts,
					    const std::map<std::string, std::string> &links,
					    const std::set<std::string> &live = {})
{
	std::vector<SlotToRelease> out = screen_slots_to_release(table, facts);
	const std::vector<SlotToRelease> voices = voice_slots_to_release(table, facts, links, live);
	out.insert(out.end(), voices.begin(), voices.end());
	return out;
}

AssignPolicy manual(std::set<std::string> always_on = {})
{
	AssignPolicy policy;
	policy.always_on = std::move(always_on);
	return policy;
}

AssignPolicy automatic()
{
	AssignPolicy policy;
	policy.automatic = true;
	return policy;
}

ConfigureMessage screens(const std::vector<std::string> &users)
{
	ConfigureMessage msg;
	uint32_t id = 1;
	for (const std::string &user : users)
		msg.streams.push_back(make_stream(id++, user, StreamKind::Screen));
	return msg;
}

} // namespace

TEST_CASE("手動で載せた人が画面共有をやめると枠が空き、再開しても載らない。選び直すと載る")
{
	AllocationTable table;
	StreamRouter router;
	table.try_assign(SlotKind::Screen, "A");
	table.try_assign(SlotKind::Screen, "B");
	router.apply_configure(screens({"A", "B"}), table, manual());
	REQUIRE(router.route(1).has_value());

	// A が共有をやめた (roster にはまだいる)。
	const auto departed = release_departed(table, router, facts_of({"A", "B"}, {"B"}));
	REQUIRE(departed.size() == 1);
	CHECK(departed[0].owner == "A");
	CHECK(departed[0].slot == 1);
	CHECK_FALSE(table.has_assignment(SlotKind::Screen, 1));
	CHECK(table.has_assignment(SlotKind::Screen, 2));
	// 空けただけで、外した人の一覧には入れない。
	CHECK_FALSE(router.is_excluded("A"));

	// A が共有を再開して configure に戻っても、手動なので載らない。
	router.apply_configure(screens({"B"}), table, manual());
	router.apply_configure(screens({"A", "B"}), table, manual());
	CHECK(table.slot_of_owner(SlotKind::Screen, "A") == 0);
	CHECK_FALSE(router.route(1).has_value());

	// 利用者が押し直すと載り、次の configure で経路ができる。
	REQUIRE(table.try_assign(SlotKind::Screen, "A").slot == 1);
	router.apply_configure(screens({"A", "B"}), table, manual());
	REQUIRE(router.route(1).has_value());
	CHECK(router.route(1)->slot == 1);
}

TEST_CASE("自動のとき、共有をやめた人の枠が空き、5 人目が載る。再開した人は空き枠があれば載る")
{
	AllocationTable table;
	StreamRouter router;
	router.apply_configure(screens({"A", "B", "C", "D", "E"}), table, automatic());
	CHECK(table.slot_of_owner(SlotKind::Screen, "E") == 0); // 枠は 4 つ

	// B が共有をやめた。
	const auto departed = release_departed(table, router, facts_of({"A", "B", "C", "D", "E"}, {"A", "C", "D", "E"}));
	REQUIRE(departed.size() == 1);
	CHECK(departed[0].owner == "B");
	router.apply_configure(screens({"A", "C", "D", "E"}), table, automatic());
	CHECK(table.slot_of_owner(SlotKind::Screen, "E") == 2); // 空いた枠に 5 人目が載る
	CHECK(table.slot_of_owner(SlotKind::Screen, "A") == 1);

	// A が共有をやめ、B が再開した。空いた枠 1 に載る。
	release_departed(table, router, facts_of({"A", "B", "C", "D", "E"}, {"B", "C", "D", "E"}));
	router.apply_configure(screens({"B", "C", "D", "E"}), table, automatic());
	CHECK(table.slot_of_owner(SlotKind::Screen, "B") == 1);
	CHECK(table.slot_of_owner(SlotKind::Screen, "A") == 0);
}

TEST_CASE("[自動で枠に載せる] の人は、共有をやめると枠が空き、再開すると載る")
{
	AllocationTable table;
	StreamRouter router;
	const AssignPolicy policy = manual({"A"});
	router.apply_configure(screens({"A"}), table, policy);
	REQUIRE(table.slot_of_owner(SlotKind::Screen, "A") == 1);

	release_departed(table, router, facts_of({"A"}, {}));
	CHECK(table.slot_of_owner(SlotKind::Screen, "A") == 0);
	router.apply_configure(ConfigureMessage{}, table, policy); // 共有をやめた configure
	router.apply_configure(screens({"A"}), table, policy);     // 再開
	CHECK(table.slot_of_owner(SlotKind::Screen, "A") == 1);
}

TEST_CASE("チェックを外した人が共有をやめて始め直すと、自動では載る")
{
	AllocationTable table;
	StreamRouter router;
	router.apply_configure(screens({"A", "B"}), table, automatic());
	// 利用者が A のチェックを外した (Pipeline::release_slot と同じ)。
	table.release(SlotKind::Screen, 1);
	router.exclude_user("A");
	router.retarget_user("A", SlotKind::Screen, 0);
	router.apply_configure(screens({"A", "B"}), table, automatic());
	CHECK(table.slot_of_owner(SlotKind::Screen, "A") == 0);

	// A が共有をやめ (facts の sharing から消える)、外した人の一覧から消える。始め直すと載る。
	router.forget_stopped_sharing({"B"});
	CHECK_FALSE(router.is_excluded("A"));
	router.apply_configure(screens({"B"}), table, automatic());
	router.apply_configure(screens({"B", "A"}), table, automatic());
	CHECK(table.slot_of_owner(SlotKind::Screen, "A") != 0);
}

TEST_CASE("通話を抜けた人の画面共有の枠と声の枠が空き、同じ声の枠に新しい人が入る")
{
	AllocationTable table;
	StreamRouter router;
	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "A", StreamKind::Screen));
	msg.streams.push_back(make_stream(2, "A", StreamKind::Voice));
	msg.streams.push_back(make_stream(3, "B", StreamKind::Voice));
	router.apply_configure(msg, table, automatic());
	REQUIRE(table.slot_of_owner(SlotKind::Voice, "A") == 1);
	REQUIRE(table.slot_of_owner(SlotKind::Voice, "B") == 2);

	// A が通話を抜けた。
	const auto departed = release_departed(table, router, facts_of({"B", "C"}, {}));
	REQUIRE(departed.size() == 2);
	CHECK(table.slot_of_owner(SlotKind::Screen, "A") == 0);
	CHECK(table.slot_of_owner(SlotKind::Voice, "A") == 0);
	CHECK(table.slot_of_owner(SlotKind::Voice, "B") == 2); // 残る人は動かない

	ConfigureMessage next;
	next.streams.push_back(make_stream(3, "B", StreamKind::Voice));
	next.streams.push_back(make_stream(4, "C", StreamKind::Voice));
	router.apply_configure(next, table, automatic());
	CHECK(table.slot_of_owner(SlotKind::Voice, "C") == 1);
}

TEST_CASE("roster や sharing が分からない間は、枠を外さない")
{
	AllocationTable table;
	table.try_assign(SlotKind::Screen, "A");
	table.try_assign(SlotKind::Voice, "A");

	// facts がまだ届いていない。
	CHECK(slots_to_release(table, FactsSnapshot{}, {}).empty());

	// roster がない (送信側が読めていない)。sharing が空でも、やめたとは分からない。
	FactsSnapshot no_roster = facts_of({}, {});
	no_roster.roster.reset();
	CHECK(slots_to_release(table, no_roster, {}).empty());

	// sharing のキーがない facts。画面共有の枠は外さない。声の枠は roster で判断できるので外す。
	FactsSnapshot no_sharing = facts_of({"B"}, {});
	no_sharing.sharing_valid = false;
	const auto departed = slots_to_release(table, no_sharing, {});
	REQUIRE(departed.size() == 1);
	CHECK(departed[0].kind == SlotKind::Voice);
}

TEST_CASE("Vesktop のアカウントが通話を抜けると (roster が空)、全員の枠が空き、戻っても手動の人は自動では載らない")
{
	AllocationTable table;
	StreamRouter router;
	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "A", StreamKind::Screen));
	msg.streams.push_back(make_stream(2, "B", StreamKind::Screen));
	table.try_assign(SlotKind::Screen, "A"); // 手動
	table.try_assign(SlotKind::Screen, "C"); // 手動 (共有していない人)
	table.try_assign(SlotKind::Voice, "A");
	table.try_assign(SlotKind::Voice, "D");

	// roster が空で、sharing も空 (どの画面共有も見えない)。声のストリームがなければ、全員の枠が空く。
	const auto departed = release_departed(table, router, facts_of({}, {}));
	CHECK(departed.size() == 4);
	CHECK(table.slot_of_owner(SlotKind::Screen, "A") == 0);
	CHECK(table.slot_of_owner(SlotKind::Voice, "A") == 0);

	// 戻って共有を続けていても、手動で載せた人は自動では載らない。
	router.apply_configure(msg, table, manual());
	CHECK(table.slot_of_owner(SlotKind::Screen, "A") == 0);
	CHECK(table.slot_of_owner(SlotKind::Screen, "B") == 0);
}

TEST_CASE("通話を抜けて戻ると、[自動で枠に載せる] の人と「自動」は、共有していれば載る")
{
	AllocationTable table;
	StreamRouter router;
	router.apply_configure(screens({"A", "B"}), table, manual({"A"}));
	REQUIRE(table.slot_of_owner(SlotKind::Screen, "A") == 1);

	release_departed(table, router, facts_of({}, {}));
	CHECK(table.slot_of_owner(SlotKind::Screen, "A") == 0);
	router.apply_configure(ConfigureMessage{}, table, manual({"A"})); // 繋ぎ直しで、画面のストリームが 1 つもない
	router.apply_configure(screens({"A", "B"}), table, manual({"A"}));
	CHECK(table.slot_of_owner(SlotKind::Screen, "A") != 0); // [自動で枠に載せる]
	CHECK(table.slot_of_owner(SlotKind::Screen, "B") == 0);  // 手動

	AllocationTable table2;
	StreamRouter router2;
	router2.apply_configure(screens({"A", "B"}), table2, automatic());
	release_departed(table2, router2, facts_of({}, {}));
	CHECK(table2.slot_of_owner(SlotKind::Screen, "A") == 0);
	router2.apply_configure(screens({"A", "B"}), table2, automatic());
	CHECK(table2.slot_of_owner(SlotKind::Screen, "A") != 0); // 「自動」
	CHECK(table2.slot_of_owner(SlotKind::Screen, "B") != 0);
}

TEST_CASE("Vesktop との接続が切れても facts は届かず枠は残り、繋ぎ直して同じ人が共有していれば映り直す")
{
	AllocationTable table;
	StreamRouter router;
	table.try_assign(SlotKind::Screen, "A");
	router.apply_configure(screens({"A"}), table, manual());
	REQUIRE(router.route(1).has_value());

	// 切断 (Pipeline が経路を忘れる)。facts は届かないので、空ける判断はされない。
	router.clear();
	CHECK(table.slot_of_owner(SlotKind::Screen, "A") == 1);

	// 繋ぎ直した直後の configure は、画面のストリームが 1 つもない (送信側が切断で止めて作り直すため)。
	// configure からは枠を空けないので、全員が消えたように見えても、割り当ては変わらない。
	router.apply_configure(ConfigureMessage{}, table, manual());
	CHECK(table.slot_of_owner(SlotKind::Screen, "A") == 1);

	// 繋ぎ直した後の facts で A は共有を続けている。空けない。
	CHECK(slots_to_release(table, facts_of({"A"}, {"A"}), {}).empty());
	router.apply_configure(screens({"A"}), table, manual());
	REQUIRE(router.route(1).has_value());
	CHECK(router.route(1)->slot == 1);
	CHECK(router.output_allowed(1, table));
}

TEST_CASE("使う声の置き換えで声を運んでいる枠は、載っている人が抜けても、使う声に選んだ人が通話に参加している間は空けない")
{
	AllocationTable table;
	table.try_assign(SlotKind::Voice, "owner");
	table.set_voice_source(SlotKind::Voice, 1, "source");
	const std::map<std::string, std::string> links = {{"owner", "source"}};

	CHECK(slots_to_release(table, facts_of({"source", "other"}, {}), links).empty());
	// 使う声に選んだ人も抜けたら空ける。
	const auto departed = slots_to_release(table, facts_of({"other"}, {}), links);
	REQUIRE(departed.size() == 1);
	CHECK(departed[0].owner == "owner");
	CHECK(departed[0].voice == "source");
	// 置き換えていなければ、載っている人が抜けた時点で空ける。
	CHECK(slots_to_release(table, facts_of({"source", "other"}, {}), {}).size() == 1);
}

TEST_CASE("30 人の声の枠は、全員が通話に参加している間は空けず、一部が抜けると抜けた人の枠だけを空ける")
{
	AllocationTable table;
	std::vector<std::string> roster;
	for (int i = 1; i <= 30; ++i) {
		roster.push_back("v" + std::to_string(i));
		table.try_assign(SlotKind::Voice, roster.back());
	}
	CHECK(slots_to_release(table, facts_of(roster, {}), {}).empty());

	roster.erase(roster.begin() + 9); // v10 が抜けた
	const auto departed = slots_to_release(table, facts_of(roster, {}), {});
	REQUIRE(departed.size() == 1);
	CHECK(departed[0].owner == "v10");
	CHECK(departed[0].slot == 10);
}

TEST_CASE("声のストリームが生きている間は、roster からいったん消えて戻っても、声の枠と経路が残る")
{
	AllocationTable table;
	StreamRouter router;
	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "A", StreamKind::Voice));
	router.apply_configure(msg, table, automatic());
	REQUIRE(table.slot_of_owner(SlotKind::Voice, "A") == 1);
	REQUIRE(router.route(1).has_value());

	// roster から A が一瞬欠けた facts。声のストリームが生きているので空けない。
	CHECK(release_departed(table, router, facts_of({"B"}, {}), {}, live_voice_of(msg)).empty());
	CHECK(table.slot_of_owner(SlotKind::Voice, "A") == 1);
	REQUIRE(router.route(1).has_value());
	CHECK(router.route(1)->slot == 1);
	// roster に戻っても同じ。
	CHECK(release_departed(table, router, facts_of({"A", "B"}, {}), {}, live_voice_of(msg)).empty());
	CHECK(table.slot_of_owner(SlotKind::Voice, "A") == 1);
}

TEST_CASE("facts が先、configure が後。ストリームが消えた時点で roster にもいなければ、最後に空く")
{
	AllocationTable table;
	StreamRouter router;
	ConfigureMessage first;
	first.streams.push_back(make_stream(1, "A", StreamKind::Voice));
	router.apply_configure(first, table, automatic());
	REQUIRE(table.slot_of_owner(SlotKind::Voice, "A") == 1);

	// facts (A は roster にいない) が先に届く。直前の configure にまだ A のストリームがあるので残る。
	const FactsSnapshot facts = facts_of({"B"}, {});
	CHECK(release_departed(table, router, facts, {}, live_voice_of(first)).empty());
	CHECK(table.slot_of_owner(SlotKind::Voice, "A") == 1);

	// A のストリームが消えた configure が届く。configure の側は声の枠だけを見て、空ける。
	ConfigureMessage next;
	next.streams.push_back(make_stream(2, "B", StreamKind::Voice));
	router.apply_configure(next, table, automatic());
	const auto departed = release_departed(table, router, facts, {}, live_voice_of(next), false);
	REQUIRE(departed.size() == 1);
	CHECK(departed[0].kind == SlotKind::Voice);
	CHECK(departed[0].owner == "A");
	CHECK(table.slot_of_owner(SlotKind::Voice, "A") == 0);
	CHECK(table.slot_of_owner(SlotKind::Voice, "B") != 0);
}

TEST_CASE("configure が先、facts が後。facts が届いた時点で空く")
{
	AllocationTable table;
	StreamRouter router;
	ConfigureMessage first;
	first.streams.push_back(make_stream(1, "A", StreamKind::Voice));
	first.streams.push_back(make_stream(2, "B", StreamKind::Voice));
	router.apply_configure(first, table, automatic());

	// A のストリームが消えた configure が先に届く。facts はまだ古い (A は roster にいる)。空けない。
	ConfigureMessage next;
	next.streams.push_back(make_stream(2, "B", StreamKind::Voice));
	router.apply_configure(next, table, automatic());
	const FactsSnapshot old_facts = facts_of({"A", "B"}, {});
	CHECK(release_departed(table, router, old_facts, {}, live_voice_of(next), false).empty());
	CHECK(table.slot_of_owner(SlotKind::Voice, "A") == 1);

	// A が roster にいない facts が後で届く。ストリームはないので空く。
	const auto departed = release_departed(table, router, facts_of({"B"}, {}), {}, live_voice_of(next));
	REQUIRE(departed.size() == 1);
	CHECK(departed[0].owner == "A");
	CHECK(table.slot_of_owner(SlotKind::Voice, "B") == 2);
}

TEST_CASE("configure だけでは、画面の枠を空けない (始めたばかりの人を外さない)")
{
	AllocationTable table;
	StreamRouter router;
	router.apply_configure(screens({"A"}), table, automatic());
	REQUIRE(table.slot_of_owner(SlotKind::Screen, "A") == 1);
	// facts の sharing に A がまだいない (configure が facts より先に届いた)。configure の側は画面の枠を見ない。
	CHECK(release_departed(table, router, facts_of({"A"}, {}), {}, {}, false).empty());
	CHECK(table.slot_of_owner(SlotKind::Screen, "A") == 1);
}

TEST_CASE("使う声の組。使う声のストリームが生きている間は、載っている人が roster にいなくても空けない")
{
	AllocationTable table;
	table.try_assign(SlotKind::Voice, "owner");
	table.set_voice_source(SlotKind::Voice, 1, "source");
	const std::map<std::string, std::string> links = {{"owner", "source"}};

	// どちらも roster にいなくても、使う声 (source) のストリームが生きていれば空けない。
	CHECK(slots_to_release(table, facts_of({"other"}, {}), links, {"source"}).empty());
	// owner のストリームだけが生きていても、その枠で声を出しているのは source なので、空ける。
	CHECK(slots_to_release(table, facts_of({"other"}, {}), links, {"owner"}).size() == 1);
	// どれもなければ空ける。
	CHECK(slots_to_release(table, facts_of({"other"}, {}), links, {}).size() == 1);
}
