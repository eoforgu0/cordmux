// SPDX-License-Identifier: Apache-2.0

// pipeline/stream_router.h の単体テスト。stream_id から枠への経路の作り方、
// 音声と映像を出してよいかの判定を確認する。誰を枠に入れるかの決め方 (手動、自動、「自動で載せる」) と、
// 使う声の置き換えのテストはファイルの後半に、OBS を起動し直した後 (割り当て表は空から始まる) のテストは
// 後ろのほうにある。自分の枠を使っている間の人数の上限と、描く物が 4 つを超えないことのテストと、ドックと Pipeline のスレッドの
// 間の競合のテストは末尾にある。

#include "doctest.h"

#include "pipeline/pipeline.h"
#include "pipeline/stream_router.h"
#include "sources/composite_input.h"
#include "sources/self_frame_plan.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using cordmux::config::AllocationTable;
using cordmux::config::SlotKind;
using cordmux::pipeline::AssignPolicy;
using cordmux::pipeline::needs_screen_stream_recreate;
using cordmux::pipeline::Route;
using cordmux::pipeline::RouteChange;
using cordmux::pipeline::StreamRouter;
using cordmux::ws::ConfigureMessage;
using cordmux::ws::StreamConfig;
using cordmux::ws::StreamKind;

namespace {

// 誰を枠に入れるかの決め方。前半のテストはすべて自動で実行する。どれも「configure に載った人を上限まで枠に入れる」
// 動作を確認するもので、これは自動の動作である。手動の動作は、ファイルの後半のテストで確認する。
const AssignPolicy kAuto{true, {}};

// 手動 (既定)。「自動で載せる」を付けた人は、テストごとに指定する。
AssignPolicy manual_with(std::set<std::string> always_on = {})
{
	AssignPolicy policy;
	policy.automatic = false;
	policy.always_on = std::move(always_on);
	return policy;
}

StreamConfig make_stream(uint32_t stream_id, const std::string &user_id, StreamKind kind, bool audio = false)
{
	StreamConfig c;
	c.stream_id = stream_id;
	c.user_id = user_id;
	c.kind = kind;
	c.audio = audio;
	return c;
}

} // namespace

TEST_CASE("configure に新しく載った画面の stream_id は、空いている枠に割り当てられる")
{
	AllocationTable table;
	StreamRouter router;

	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "alice", StreamKind::Screen));

	auto changes = router.apply_configure(msg, table, kAuto);
	REQUIRE(changes.size() == 1);
	CHECK(changes[0].stream_id == 1);
	CHECK_FALSE(changes[0].before.has_value());
	REQUIRE(changes[0].after.has_value());
	CHECK(changes[0].after->kind == SlotKind::Screen);
	CHECK(changes[0].after->slot == 1);
	CHECK(changes[0].after->user_id == "alice");

	const auto route = router.route(1);
	REQUIRE(route.has_value());
	CHECK(route->slot == 1);
	CHECK(router.output_allowed(1, table));
}

TEST_CASE("configure に新しく載った声の stream_id も、空いている声の枠に割り当てられる")
{
	AllocationTable table;
	StreamRouter router;

	ConfigureMessage msg;
	msg.streams.push_back(make_stream(10, "taro", StreamKind::Voice));

	auto changes = router.apply_configure(msg, table, kAuto);
	REQUIRE(changes.size() == 1);
	REQUIRE(changes[0].after.has_value());
	CHECK(changes[0].after->kind == SlotKind::Voice);
	CHECK(changes[0].after->slot == 1);
	CHECK(router.output_allowed(10, table));
}

TEST_CASE("configure から消えた stream_id は経路が消えるが、枠の割り当ては残る")
{
	AllocationTable table;
	StreamRouter router;

	ConfigureMessage msg1;
	msg1.streams.push_back(make_stream(1, "alice", StreamKind::Screen));
	router.apply_configure(msg1, table, kAuto);
	REQUIRE(router.route(1).has_value());

	// 2 回目の configure に stream_id=1 が載らない。
	ConfigureMessage msg2;
	auto changes = router.apply_configure(msg2, table, kAuto);
	REQUIRE(changes.size() == 1);
	CHECK(changes[0].stream_id == 1);
	REQUIRE(changes[0].before.has_value());
	CHECK(changes[0].before->user_id == "alice");
	CHECK_FALSE(changes[0].after.has_value());
	CHECK(std::string(changes[0].reason) == "removed");

	CHECK_FALSE(router.route(1).has_value());
	// 割り当て表は変わらない (枠は、載っている人が画面共有をやめても解放しない)。
	CHECK(table.has_assignment(SlotKind::Screen, 1));
	CHECK(table.assignment(SlotKind::Screen, 1)->owner_user_id == "alice");
}

TEST_CASE("同じ人が別の stream_id で来たとき (再接続) は同じ枠に入り、枠は止まらない")
{
	AllocationTable table;
	StreamRouter router;

	ConfigureMessage msg1;
	msg1.streams.push_back(make_stream(1, "alice", StreamKind::Screen));
	router.apply_configure(msg1, table, kAuto);

	// stream_id が 1 から 2 に変わったが、user_id は同じ alice のまま (再接続)。
	ConfigureMessage msg2;
	msg2.streams.push_back(make_stream(2, "alice", StreamKind::Screen));
	auto changes = router.apply_configure(msg2, table, kAuto);

	// 古い経路の削除と新しい経路の追加の 2 件。
	REQUIRE(changes.size() == 2);

	const auto new_route = router.route(2);
	REQUIRE(new_route.has_value());
	CHECK(new_route->slot == 1); // 同じ枠に入る (載っている人が同じなので AlreadyOwned)
	CHECK_FALSE(router.route(1).has_value());

	// 同じ人の再接続は別の人の映像ではないので、枠を止めない。
	CHECK(router.output_allowed(2, table));
	CHECK(table.is_output_allowed(SlotKind::Screen, 1));
}

TEST_CASE("画面の枠が埋まっていると、5 人目は載らず、空けた枠には次の configure で載る (取り上げない)")
{
	AllocationTable table;
	StreamRouter router;

	ConfigureMessage msg1;
	for (int i = 1; i <= 4; ++i)
		msg1.streams.push_back(make_stream(uint32_t(i), "p" + std::to_string(i), StreamKind::Screen));
	msg1.streams.push_back(make_stream(5, "bob", StreamKind::Screen));
	router.apply_configure(msg1, table, kAuto);
	// 枠はすべて埋まっていて、5 人目の bob は載らない。ほかの人の枠は取り上げない。
	CHECK_FALSE(router.route(5).has_value());
	for (int i = 1; i <= 4; ++i)
		CHECK(table.assignment(SlotKind::Screen, i)->owner_user_id == "p" + std::to_string(i));

	// p1 が画面共有をやめて枠を空け (pipeline/slot_release.h)、p1 のストリームが configure から消える。
	table.release(SlotKind::Screen, 1);
	ConfigureMessage msg2;
	for (int i = 2; i <= 4; ++i)
		msg2.streams.push_back(make_stream(uint32_t(i), "p" + std::to_string(i), StreamKind::Screen));
	msg2.streams.push_back(make_stream(5, "bob", StreamKind::Screen));
	router.apply_configure(msg2, table, kAuto);
	const auto route = router.route(5);
	REQUIRE(route.has_value());
	CHECK(route->slot == 1);
	CHECK(table.assignment(SlotKind::Screen, 1)->owner_user_id == "bob");
	// 空けた枠に載った人は、前の人の枠を引き継いだのではないので止めない。
	CHECK(router.output_allowed(5, table));
}

TEST_CASE("声の枠は 9 人目以降にも割り当てられ、30 人でも全員の経路ができる")
{
	AllocationTable table;
	StreamRouter router;

	ConfigureMessage msg1;
	for (int i = 1; i <= 30; ++i)
		msg1.streams.push_back(make_stream(uint32_t(i), "voice" + std::to_string(i), StreamKind::Voice));
	router.apply_configure(msg1, table, kAuto);
	for (int i = 1; i <= 30; ++i) {
		const auto route = router.route(uint32_t(i));
		REQUIRE(route.has_value());
		CHECK(route->slot == i);
		CHECK(router.output_allowed(uint32_t(i), table));
	}

	// 1 人が通話から抜けて (枠を空ける)、新しい人が来ると、空いた枠に入る。ほかの人の枠は動かない。
	table.release(SlotKind::Voice, 1);
	ConfigureMessage msg2;
	for (int i = 2; i <= 30; ++i)
		msg2.streams.push_back(make_stream(uint32_t(i), "voice" + std::to_string(i), StreamKind::Voice));
	msg2.streams.push_back(make_stream(99, "newguy", StreamKind::Voice));
	router.apply_configure(msg2, table, kAuto);
	REQUIRE(router.route(99).has_value());
	CHECK(router.route(99)->slot == 1);
	CHECK(router.route(30)->slot == 30);
}

TEST_CASE("シーンコレクションの切り替えの最中は、経路があっても音声を出さない")
{
	AllocationTable table;
	StreamRouter router;

	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "alice", StreamKind::Screen));
	router.apply_configure(msg, table, kAuto);
	CHECK(router.output_allowed(1, table));

	table.begin_scene_collection_change();
	CHECK(table.is_frozen());
	// table の is_output_allowed は凍結を見ないが、StreamRouter::output_allowed は凍結の間は出さない。
	CHECK(table.is_output_allowed(SlotKind::Screen, 1));
	CHECK_FALSE(router.output_allowed(1, table));

	table.apply_scene_collection_changed();
	CHECK(router.output_allowed(1, table));
}

TEST_CASE("経路がない stream_id と、枠から外した経路 (枠 0) の映像は出さない")
{
	AllocationTable table;
	StreamRouter router;
	CHECK_FALSE(router.video_allowed(999, table));

	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "alice", StreamKind::Screen));
	router.apply_configure(msg, table, kAuto);
	REQUIRE(router.video_allowed(1, table));
	table.release(SlotKind::Screen, 1);
	CHECK(router.retarget_user("alice", SlotKind::Screen, 0) == 1);
	CHECK_FALSE(router.video_allowed(1, table));
	CHECK_FALSE(router.output_allowed(1, table));
}

TEST_CASE("割り当てられないストリームは経路を持たず、RouteChange も返らない")
{
	AllocationTable table;
	StreamRouter router;

	ConfigureMessage msg;
	for (int i = 1; i <= 4; ++i)
		msg.streams.push_back(make_stream(uint32_t(i), "p" + std::to_string(i), StreamKind::Screen));
	router.apply_configure(msg, table, kAuto);

	ConfigureMessage msg2 = msg;
	msg2.streams.push_back(make_stream(5, "p5", StreamKind::Screen));
	auto changes = router.apply_configure(msg2, table, kAuto);

	// stream_id=5 は前の呼び出しでも経路を持っていなかったので、RouteChange は返らない (割り当てられていないことは
	// route() と output_allowed() で分かる)。p1 から p4 は AlreadyOwned のまま変わらないので、変わった経路はない。
	CHECK(changes.empty());
	CHECK_FALSE(router.route(5).has_value());
	CHECK_FALSE(router.output_allowed(5, table));
}

TEST_CASE("clear は経路をすべて忘れ、割り当て表には触らない")
{
	AllocationTable table;
	StreamRouter router;

	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "alice", StreamKind::Screen));
	router.apply_configure(msg, table, kAuto);
	REQUIRE(router.route(1).has_value());

	router.clear();
	CHECK_FALSE(router.route(1).has_value());
	CHECK(table.has_assignment(SlotKind::Screen, 1)); // 割り当て表はそのまま
}

TEST_CASE("経路がない stream_id の output_allowed は false")
{
	AllocationTable table;
	StreamRouter router;
	CHECK_FALSE(router.route(999).has_value());
	CHECK_FALSE(router.output_allowed(999, table));
}

// ---------------------------------------------------------------------------
// stream_id は画面と声をまたいだ通し番号である。
// ---------------------------------------------------------------------------

TEST_CASE("同じ stream_id が画面と声の両方に載った configure は、全体が捨てられる")
{
	AllocationTable table;
	StreamRouter router;

	// 画面の stream_id=1 と声の stream_id=1 が同じ configure に載っている。受信側はこれを 2 つの経路に
	// 分けられない (音声のメッセージのヘッダーに種類がない) ので、ws/ で configure 全体を捨てる
	// (どちらに渡すか決められないときは、どちらにも渡さない)。
	const auto j = cordmux::ws::try_parse_json(
		R"({"type":"configure","streams":[
		      {"stream_id":1,"user_id":"A","kind":"screen"},
		      {"stream_id":1,"user_id":"B","kind":"voice"}]})");
	REQUIRE(j.has_value());
	CHECK_FALSE(cordmux::ws::parse_configure(*j).has_value());

	// 捨てられた configure は router まで届かないので、経路は 1 つもできない。
	CHECK_FALSE(router.route(1).has_value());
	CHECK_FALSE(router.output_allowed(1, table));
	CHECK(router.route_ids().empty());
}

TEST_CASE("stream_id が別なら、同じ人の画面と声の両方の経路ができる")
{
	AllocationTable table;
	StreamRouter router;

	// 送信側は画面と声をまたいだ通し番号を振る。A の画面共有が 1、B の声が 2、
	// A の声が 3 (src-vencord/index.ts の nextStreamId)。
	const auto j = cordmux::ws::try_parse_json(
		R"({"type":"configure","streams":[
		      {"stream_id":1,"user_id":"A","kind":"screen"},
		      {"stream_id":2,"user_id":"B","kind":"voice"},
		      {"stream_id":3,"user_id":"A","kind":"voice"}]})");
	REQUIRE(j.has_value());
	const auto msg = cordmux::ws::parse_configure(*j);
	REQUIRE(msg.has_value());

	const auto changes = router.apply_configure(*msg, table, kAuto);
	CHECK(changes.size() == 3);

	const auto screen_route = router.route(1);
	REQUIRE(screen_route.has_value());
	CHECK(screen_route->kind == SlotKind::Screen);
	CHECK(screen_route->user_id == "A");
	CHECK(screen_route->slot == 1);

	const auto voice_route = router.route(2);
	REQUIRE(voice_route.has_value());
	CHECK(voice_route->kind == SlotKind::Voice);
	CHECK(voice_route->user_id == "B");

	// 音声のメッセージは、stream_id の経路の kind で画面の音か声かが決まる (画面共有の音は
	// 声の枠に行かない)。id=1 は画面、id=2 と 3 は声。
	CHECK(router.route(3)->kind == SlotKind::Voice);
	CHECK(router.route(3)->user_id == "A");

	CHECK(router.output_allowed(1, table));
	CHECK(router.output_allowed(2, table));
	CHECK(router.route_ids() == std::set<uint32_t>{1, 2, 3});
}

TEST_CASE("configure から消えた stream_id は route_ids からも消える")
{
	AllocationTable table;
	StreamRouter router;

	ConfigureMessage msg1;
	msg1.streams.push_back(make_stream(1, "A", StreamKind::Screen));
	msg1.streams.push_back(make_stream(2, "B", StreamKind::Voice));
	router.apply_configure(msg1, table, kAuto);
	CHECK(router.route_ids() == std::set<uint32_t>{1, 2});

	ConfigureMessage msg2;
	msg2.streams.push_back(make_stream(2, "B", StreamKind::Voice));
	router.apply_configure(msg2, table, kAuto);
	CHECK(router.route_ids() == std::set<uint32_t>{2});

	router.clear();
	CHECK(router.route_ids().empty());
}

TEST_CASE("新しく追加された経路、消えた経路、種類が変わった経路では needs_screen_stream_recreate が false を返す")
{
	Route screen_a{SlotKind::Screen, 1, "A"};
	Route voice_b{SlotKind::Voice, 1, "B"};

	CHECK_FALSE(needs_screen_stream_recreate(RouteChange{1, std::nullopt, screen_a, "assigned"}));
	CHECK_FALSE(needs_screen_stream_recreate(RouteChange{1, screen_a, std::nullopt, "removed"}));
	CHECK_FALSE(needs_screen_stream_recreate(RouteChange{1, voice_b, screen_a, "changed"}));
	CHECK_FALSE(needs_screen_stream_recreate(RouteChange{1, screen_a, voice_b, "changed"}));
}

TEST_CASE("同じ stream_id が画面のまま別の人か別の枠に替わったら、"
	  "needs_screen_stream_recreate が true を返す")
{
	Route a_slot1{SlotKind::Screen, 1, "A"};
	Route b_slot1{SlotKind::Screen, 1, "B"};
	Route a_slot2{SlotKind::Screen, 2, "A"};

	// 同じ stream_id=1 が、別の人 (B) の画面共有に使われた (Vesktop だけを再起動したとき)。
	CHECK(needs_screen_stream_recreate(RouteChange{1, a_slot1, b_slot1, "changed"}));
	// 同じ人でも枠が変わった (通常は起きないが、念のため true にする)。
	CHECK(needs_screen_stream_recreate(RouteChange{1, a_slot1, a_slot2, "changed"}));
	// 人も枠も同じなら作り直さない。
	CHECK_FALSE(needs_screen_stream_recreate(RouteChange{1, a_slot1, a_slot1, "changed"}));
}

TEST_CASE("retarget_user は、その人のその種類の経路だけを新しい枠に付け替える")
{
	AllocationTable table;
	StreamRouter router;

	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "alice", StreamKind::Screen));
	msg.streams.push_back(make_stream(2, "bob", StreamKind::Screen));
	msg.streams.push_back(make_stream(3, "alice", StreamKind::Voice));
	router.apply_configure(msg, table, kAuto);

	REQUIRE(router.route(1).has_value());
	CHECK(router.route(1)->slot == 1);
	CHECK(router.route(2)->slot == 2);

	// 画面の枠だけを付け替える (声の経路はそのまま)。
	CHECK(router.retarget_user("alice", SlotKind::Screen, 3) == 1);
	CHECK(router.route(1)->slot == 3);
	CHECK(router.route(2)->slot == 2); // 別の人は動かない
	CHECK(router.route(3)->slot == 1); // 声の経路はそのまま

	// 同じ枠を指しているなら何も変わらない。
	CHECK(router.retarget_user("alice", SlotKind::Screen, 3) == 0);
	// 知らない人と空の userId では何も起きない。
	CHECK(router.retarget_user("carol", SlotKind::Screen, 4) == 0);
	CHECK(router.retarget_user("", SlotKind::Screen, 4) == 0);
}

TEST_CASE("枠から外した人の経路は枠 0 を指して出力が止まり、空いた枠に入れたほかの人の枠には出ない")
{
	AllocationTable table;
	StreamRouter router;

	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "alice", StreamKind::Screen));
	msg.streams.push_back(make_stream(2, "bob", StreamKind::Screen));
	router.apply_configure(msg, table, kAuto);
	CHECK(router.output_allowed(1, table));

	// 利用者が alice を枠 1 から外した (Pipeline::release_slot と同じく、表と経路を一緒に変える)。
	table.release(SlotKind::Screen, 1);
	CHECK(router.retarget_user("alice", SlotKind::Screen, 0) == 1);
	CHECK(router.route(1)->slot == 0);
	CHECK_FALSE(router.output_allowed(1, table));

	// 空いた枠 1 に bob を入れ直しても、alice の映像はそこに出ない。
	table.release(SlotKind::Screen, 2);
	auto r = table.try_assign(SlotKind::Screen, "bob");
	REQUIRE(r.slot == 1);
	CHECK(router.retarget_user("bob", SlotKind::Screen, r.slot) == 1);
	CHECK_FALSE(router.output_allowed(1, table)); // alice の経路は枠 0 のまま
	CHECK(router.output_allowed(2, table));       // bob の経路が枠 1 を指す
	CHECK(router.route(2)->slot == 1);
}

// ---------------------------------------------------------------------------
// 誰を枠に入れるかの決め方
//
// ここから下のテストは、決め方によって枠に入る人が変わることだけを確認する。経路の作り方 (種類の分け方) は、上のテストで確認している。
// ---------------------------------------------------------------------------

TEST_CASE("自動では、画面共有を始めた人が上限まで枠に載る")
{
	AllocationTable table;
	StreamRouter router;

	ConfigureMessage msg;
	for (uint32_t i = 1; i <= 5; ++i)
		msg.streams.push_back(make_stream(i, "p" + std::to_string(i), StreamKind::Screen));

	// 5 人とも通話に参加しているので取り上げは起きない (4 つの枠が埋まり、5 人目は入れない)。
	router.apply_configure(msg, table, kAuto);

	for (int slot = 1; slot <= 4; ++slot) {
		REQUIRE(table.has_assignment(SlotKind::Screen, slot));
		CHECK(table.assignment(SlotKind::Screen, slot)->owner_user_id == "p" + std::to_string(slot));
	}
	CHECK(router.route(5) == std::nullopt); // 5 人目は経路を持たない
}

TEST_CASE("手動では、画面共有を始めただけでは画面の枠に入らないが、声の枠には入る")
{
	AllocationTable table;
	StreamRouter router;

	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "alice", StreamKind::Screen));
	msg.streams.push_back(make_stream(2, "bob", StreamKind::Screen));
	msg.streams.push_back(make_stream(3, "alice", StreamKind::Voice));
	msg.streams.push_back(make_stream(4, "bob", StreamKind::Voice));

	const auto changes = router.apply_configure(msg, table, manual_with());

	// 画面: 1 人も入らない (経路も持たないので、出さない映像をデコードしない)。
	CHECK_FALSE(table.has_assignment(SlotKind::Screen, 1));
	CHECK(router.route(1) == std::nullopt);
	CHECK(router.route(2) == std::nullopt);

	// 声: 上限まで自動で入る (載せていない人の声を配信から消さない)。
	REQUIRE(table.has_assignment(SlotKind::Voice, 1));
	REQUIRE(table.has_assignment(SlotKind::Voice, 2));
	REQUIRE(router.route(3).has_value());
	CHECK(router.route(3)->kind == SlotKind::Voice);
	CHECK(router.output_allowed(3, table));
	CHECK(router.output_allowed(4, table));

	// 経路ができたのは声の 2 つだけ。
	CHECK(changes.size() == 2);
}

TEST_CASE("手動では利用者が入れた人だけが載り、外した人は configure が来ても戻らない")
{
	AllocationTable table;
	StreamRouter router;

	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "alice", StreamKind::Screen));
	msg.streams.push_back(make_stream(2, "bob", StreamKind::Screen));
	router.apply_configure(msg, table, manual_with());
	CHECK(router.route(1) == std::nullopt);

	// 利用者が alice を枠に載せた (ドックの操作で Pipeline::assign_slot が通る形)。
	const auto assigned = table.try_assign(SlotKind::Screen, "alice");
	REQUIRE(assigned.slot == 1);

	// 次の configure で alice の経路ができる (自分の枠がある人)。bob は入らないまま。
	router.apply_configure(msg, table, manual_with());
	REQUIRE(router.route(1).has_value());
	CHECK(router.route(1)->slot == 1);
	CHECK(router.output_allowed(1, table));
	CHECK(router.route(2) == std::nullopt);

	// 利用者が alice を枠から外した。次の configure で枠に戻らない。
	table.release(SlotKind::Screen, 1);
	const auto changes = router.apply_configure(msg, table, manual_with());
	CHECK_FALSE(table.has_assignment(SlotKind::Screen, 1));
	CHECK(router.route(1) == std::nullopt);
	REQUIRE(changes.size() == 1);
	CHECK(std::string(changes[0].reason) == "not_chosen");
}

TEST_CASE("手動では、「自動で載せる」を付けた人が画面共有を始めたら空いている枠に入る")
{
	AllocationTable table;
	StreamRouter router;

	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "alice", StreamKind::Screen));
	msg.streams.push_back(make_stream(2, "bob", StreamKind::Screen));

	router.apply_configure(msg, table, manual_with({"bob"}));

	// bob (「自動で載せる」を付けた人) だけが入る。alice は利用者が枠に載せるまで入らない。
	REQUIRE(table.has_assignment(SlotKind::Screen, 1));
	CHECK(table.assignment(SlotKind::Screen, 1)->owner_user_id == "bob");
	CHECK(router.route(1) == std::nullopt);
	REQUIRE(router.route(2).has_value());
	CHECK(router.route(2)->slot == 1);
}

TEST_CASE("手動では、「自動で載せる」を付けた人を枠から外すと、同じ画面共有の間は戻らず、画面共有を始め直すと戻る")
{
	AllocationTable table;
	StreamRouter router;

	ConfigureMessage sharing;
	sharing.streams.push_back(make_stream(1, "bob", StreamKind::Screen));
	const AssignPolicy policy = manual_with({"bob"});

	router.apply_configure(sharing, table, policy);
	REQUIRE(table.slot_of_owner(SlotKind::Screen, "bob") == 1);

	// 利用者が外した。画面共有は続いているので、同じ configure が何度来ても戻らない
	// (画面共有を始めたときだけ入れるため。stream_router.h の AssignPolicy)。
	table.release(SlotKind::Screen, 1);
	router.apply_configure(sharing, table, policy);
	router.apply_configure(sharing, table, policy);
	CHECK(table.slot_of_owner(SlotKind::Screen, "bob") == 0);

	// bob が画面共有をやめ、もう一度始めたら入る。
	ConfigureMessage stopped;
	stopped.streams.push_back(make_stream(2, "bob", StreamKind::Voice));
	router.apply_configure(stopped, table, policy);
	CHECK(table.slot_of_owner(SlotKind::Screen, "bob") == 0);

	router.apply_configure(sharing, table, policy);
	CHECK(table.slot_of_owner(SlotKind::Screen, "bob") == 1);
}

TEST_CASE("空いている枠がなければ、手動でも自動でも、ほかの人の枠を取り上げない")
{
	AllocationTable table;
	StreamRouter router;

	// 4 つの枠を、別の人で埋める。
	for (int slot = 1; slot <= 4; ++slot) {
		const auto r = table.try_assign(SlotKind::Screen, "old" + std::to_string(slot));
		REQUIRE(r.slot == slot);
	}

	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "bob", StreamKind::Screen));
	router.apply_configure(msg, table, manual_with({"bob"}));

	CHECK(table.slot_of_owner(SlotKind::Screen, "bob") == 0);
	CHECK(router.route(1) == std::nullopt);
	for (int slot = 1; slot <= 4; ++slot)
		CHECK(table.assignment(SlotKind::Screen, slot)->owner_user_id == "old" + std::to_string(slot));

	// 自動でも同じで、枠が空くまで載らない。
	router.apply_configure(msg, table, kAuto);
	CHECK(table.slot_of_owner(SlotKind::Screen, "bob") == 0);
	for (int slot = 1; slot <= 4; ++slot)
		CHECK(table.assignment(SlotKind::Screen, slot)->owner_user_id == "old" + std::to_string(slot));
}

TEST_CASE("自動に切り替えると上限まで載り、手動に戻しても載っている人は枠に残る")
{
	AllocationTable table;
	StreamRouter router;

	ConfigureMessage msg;
	for (uint32_t i = 1; i <= 4; ++i)
		msg.streams.push_back(make_stream(i, "p" + std::to_string(i), StreamKind::Screen));

	// 手動 (既定): 利用者が自分で入れた p2 の 1 人だけが載る。
	const auto chosen = table.try_assign(SlotKind::Screen, "p2");
	REQUIRE(chosen.slot == 1);
	router.apply_configure(msg, table, manual_with());
	CHECK(table.slot_of_owner(SlotKind::Screen, "p1") == 0);
	CHECK(table.slot_of_owner(SlotKind::Screen, "p2") == 1);

	// 自動に切り替えた (このとき Pipeline::set_assign_policy は直前の configure を適用し直す)。
	// 画面共有している人が上限まで載る。
	router.apply_configure(msg, table, kAuto);
	for (int slot = 1; slot <= 4; ++slot)
		CHECK(table.has_assignment(SlotKind::Screen, slot));
	CHECK(table.slot_of_owner(SlotKind::Screen, "p2") == 1); // 自分の枠があるので番号は変わらない

	// 手動に戻す。載っている人は誰も外さない。自動の間もドックはこの 4 人の行にチェックを付けて表示していたので、
	// 手動の「チェックを付けた人を載せる」と合う。
	std::map<int, std::string> before;
	for (int slot = 1; slot <= 4; ++slot)
		before[slot] = table.assignment(SlotKind::Screen, slot)->owner_user_id;
	router.apply_configure(msg, table, manual_with());
	for (int slot = 1; slot <= 4; ++slot) {
		REQUIRE(table.has_assignment(SlotKind::Screen, slot));
		CHECK(table.assignment(SlotKind::Screen, slot)->owner_user_id == before[slot]); // 番号も変わらない
	}
	for (uint32_t i = 1; i <= 4; ++i)
		CHECK(router.route(i).has_value()); // 映像の経路も残る (配信から消えない)

	// 手動に戻した後に画面共有を始めた人は載らない (チェックを付けていない人)。
	ConfigureMessage later = msg;
	later.streams.push_back(make_stream(5, "p5", StreamKind::Screen));
	router.apply_configure(later, table, manual_with());
	CHECK(table.slot_of_owner(SlotKind::Screen, "p5") == 0);
	CHECK(router.route(5) == std::nullopt);
}

TEST_CASE("自動で 3 人が載った後に手動に戻すと、3 人とも枠に残り、空いている枠に次の人は載らない")
{
	AllocationTable table;
	StreamRouter router;

	// 自動: 3 人が画面共有して 3 つの枠に載る (4 つ目の枠は空き)。
	ConfigureMessage msg;
	for (uint32_t i = 1; i <= 3; ++i)
		msg.streams.push_back(make_stream(i, "p" + std::to_string(i), StreamKind::Screen));
	router.apply_configure(msg, table, kAuto);
	for (int slot = 1; slot <= 3; ++slot)
		CHECK(table.has_assignment(SlotKind::Screen, slot));
	CHECK_FALSE(table.has_assignment(SlotKind::Screen, 4));

	// 手動に戻す。
	router.apply_configure(msg, table, manual_with());
	for (uint32_t i = 1; i <= 3; ++i) {
		CHECK(table.slot_of_owner(SlotKind::Screen, "p" + std::to_string(i)) != 0);
		CHECK(router.route(i).has_value());
	}
	// ドックの行のチェックは、枠があるかで付く (ledger/rules_dock.cpp の row.checked)。
	// スナップショットに 3 行が残っていれば、3 人の行のチェックも付いたまま。
	int screen_rows = 0;
	for (const auto &row : cordmux::pipeline::collect_slot_snapshots(table)) {
		if (row.kind != SlotKind::Screen)
			continue;
		++screen_rows;
	}
	CHECK(screen_rows == 3);

	// 空いている枠が 1 つあっても、手動に戻した後に画面共有を始めた p4 は載らない。
	ConfigureMessage later = msg;
	later.streams.push_back(make_stream(4, "p4", StreamKind::Screen));
	router.apply_configure(later, table, manual_with());
	CHECK(table.slot_of_owner(SlotKind::Screen, "p4") == 0);
	CHECK(router.route(4) == std::nullopt);
	CHECK_FALSE(table.has_assignment(SlotKind::Screen, 4));
}

// ---------------------------------------------------------------------------
// 使う声の置き換え (適用の仕方は stream_router.h の AssignPolicy の voice_links の (1) から (4))
// ---------------------------------------------------------------------------

namespace {

// 自動の決め方に使う声の組を加えたもの (声の枠には、決め方に関係なく同じに適用される)。
AssignPolicy auto_with_links(std::map<std::string, std::string> links)
{
	AssignPolicy policy = kAuto;
	policy.voice_links = std::move(links);
	return policy;
}

// 枠に載っている人 (owner) と使う声に選んだ人 (source) が、どちらも声を送っている configure。owner が枠 1、
// source が枠 2 を持つ順に並べる。
ConfigureMessage owner_and_source_voices()
{
	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "owner", StreamKind::Voice));
	msg.streams.push_back(make_stream(2, "source", StreamKind::Voice));
	return msg;
}

const RouteChange *change_of(const std::vector<RouteChange> &changes, uint32_t stream_id)
{
	for (const RouteChange &change : changes) {
		if (change.stream_id == stream_id)
			return &change;
	}
	return nullptr;
}

} // namespace

TEST_CASE("使う声に選んだ人の声は枠に載っている人の声の枠に入り、載っている人自身の声は経路を持たない")
{
	AllocationTable table;
	StreamRouter router;
	const ConfigureMessage msg = owner_and_source_voices();

	// 置き換える前: それぞれ自分の枠 (owner = 1、source = 2)。
	router.apply_configure(msg, table, kAuto);
	REQUIRE(router.route(1).has_value());
	REQUIRE(router.route(2).has_value());
	CHECK(router.route(1)->slot == 1);
	CHECK(router.route(2)->slot == 2);

	// 決め方が変わった (ドックで使う声を選んだ) ので、同じ configure を適用し直す
	// (Pipeline::set_assign_policy が行う)。
	const auto changes = router.apply_configure(msg, table, auto_with_links({{"owner", "source"}}));

	// source のストリームは owner の枠 (1) を指す。経路の user_id は source のまま (stream_router.h の Route)。
	const RouteChange *moved = change_of(changes, 2);
	REQUIRE(moved != nullptr);
	CHECK(std::string(moved->reason) == "changed");
	REQUIRE(router.route(2).has_value());
	CHECK(router.route(2)->kind == SlotKind::Voice);
	CHECK(router.route(2)->slot == 1);
	CHECK(router.route(2)->user_id == "source");
	CHECK(router.output_allowed(2, table));

	// owner 自身の声は経路を失う (reason は "voice_replaced")。
	const RouteChange *dropped = change_of(changes, 1);
	REQUIRE(dropped != nullptr);
	CHECK(std::string(dropped->reason) == "voice_replaced");
	CHECK_FALSE(dropped->after.has_value());
	CHECK_FALSE(router.route(1).has_value());

	// 割り当て表: 枠 1 に載っている人は owner のまま (名前とミキサーの音量は owner のもの) で、声は source。
	CHECK(table.assignment(SlotKind::Voice, 1)->owner_user_id == "owner");
	CHECK(table.assignment(SlotKind::Voice, 1)->voice_user_id == "source");
	// source が前から持っていた枠 2 は、割り当てを残したまま無音になる (枠は解放しない)。
	CHECK(table.assignment(SlotKind::Voice, 2)->owner_user_id == "source");
	CHECK(changes.size() == 2);

	// もう一度同じ configure が来ても何も変わらない (置き換えたまま安定する)。
	CHECK(router.apply_configure(msg, table, auto_with_links({{"owner", "source"}})).empty());
}

TEST_CASE("使う声を本人の声に戻すと、枠に載っている人の声の枠は本人の声に、使う声に選んだ人の声はその人の枠に戻る")
{
	AllocationTable table;
	StreamRouter router;
	const ConfigureMessage msg = owner_and_source_voices();
	router.apply_configure(msg, table, kAuto);
	router.apply_configure(msg, table, auto_with_links({{"owner", "source"}}));
	REQUIRE(table.assignment(SlotKind::Voice, 1)->voice_user_id == "source");

	const auto changes = router.apply_configure(msg, table, kAuto);

	REQUIRE(router.route(1).has_value());
	CHECK(router.route(1)->slot == 1);
	CHECK(router.route(1)->user_id == "owner");
	REQUIRE(router.route(2).has_value());
	CHECK(router.route(2)->slot == 2);
	CHECK(table.assignment(SlotKind::Voice, 1)->voice_user_id == "owner");
	CHECK(table.assignment(SlotKind::Voice, 2)->voice_user_id == "source");
	REQUIRE(change_of(changes, 1) != nullptr);
	CHECK_FALSE(change_of(changes, 1)->before.has_value()); // 経路を取り戻した
	REQUIRE(change_of(changes, 2) != nullptr);
	CHECK(std::string(change_of(changes, 2)->reason) == "changed");
}

TEST_CASE("使う声に選んだ人が通話に参加していなくても、枠に載っている人自身の声は出さない")
{
	AllocationTable table;
	StreamRouter router;

	ConfigureMessage only_owner;
	only_owner.streams.push_back(make_stream(1, "owner", StreamKind::Voice));
	router.apply_configure(only_owner, table, kAuto);
	REQUIRE(router.route(1).has_value());

	const auto changes =
		router.apply_configure(only_owner, table, auto_with_links({{"owner", "source"}}));
	REQUIRE(changes.size() == 1);
	CHECK(std::string(changes[0].reason) == "voice_replaced");
	CHECK_FALSE(router.route(1).has_value());
	// 枠の voice_user_id は組のとおり (source) にそろえる。owner の声はこの枠から出ていない。
	CHECK(table.assignment(SlotKind::Voice, 1)->owner_user_id == "owner");
	CHECK(table.assignment(SlotKind::Voice, 1)->voice_user_id == "source");

	// 初めて来たストリーム (経路を一度も持っていない owner の声) は、経路がないまま変わらないので、
	// RouteChange を返さない。
	AllocationTable fresh;
	StreamRouter fresh_router;
	CHECK(fresh_router.apply_configure(only_owner, fresh, auto_with_links({{"owner", "source"}}))
		      .empty());
	CHECK_FALSE(fresh_router.route(1).has_value());
}

TEST_CASE("枠に載っている人の声が configure から消えても、router は声の枠を変えず、使う声に選んだ人の声が出続ける")
{
	AllocationTable table;
	StreamRouter router;

	const AssignPolicy linked = auto_with_links({{"owner", "source"}});
	router.apply_configure(owner_and_source_voices(), table, kAuto);
	router.apply_configure(owner_and_source_voices(), table, linked);
	REQUIRE(router.route(2)->slot == 1);

	// owner のストリームが configure から消え、newcomer が来た。枠を空けるのは pipeline/slot_release.h で、router は
	// 割り当てを変えない。newcomer は空いている枠 (3) に入る。
	ConfigureMessage after;
	after.streams.push_back(make_stream(9, "newcomer", StreamKind::Voice));
	after.streams.push_back(make_stream(2, "source", StreamKind::Voice));
	router.apply_configure(after, table, linked);

	CHECK(table.assignment(SlotKind::Voice, 1)->owner_user_id == "owner");
	CHECK(table.assignment(SlotKind::Voice, 1)->voice_user_id == "source");
	REQUIRE(router.route(2).has_value());
	CHECK(router.route(2)->slot == 1);
	CHECK(router.output_allowed(2, table));
	REQUIRE(router.route(9).has_value());
	CHECK(router.route(9)->slot == 3);
}

TEST_CASE("1 人を 2 人の使う声にする組と、連鎖する組は経路に使わない")
{
	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "owner1", StreamKind::Voice));
	msg.streams.push_back(make_stream(2, "owner2", StreamKind::Voice));
	msg.streams.push_back(make_stream(3, "source", StreamKind::Voice));

	// 重なり (owner1 と owner2 がどちらも source を指す): どちらも置き換えない。
	{
		AllocationTable table;
		StreamRouter router;
		router.apply_configure(msg, table,
				       auto_with_links({{"owner1", "source"}, {"owner2", "source"}}));
		for (uint32_t id = 1; id <= 3; ++id) {
			REQUIRE(router.route(id).has_value());
			CHECK(router.route(id)->slot == int(id)); // 3 人とも自分の枠
		}
		for (int slot = 1; slot <= 3; ++slot) {
			const auto *a = table.assignment(SlotKind::Voice, slot);
			CHECK(a->voice_user_id == a->owner_user_id);
		}
	}

	// 連鎖 (owner1 が使う声に選んだ owner2 が、さらに source を使う声に選んでいる): どちらの組も使わない。
	{
		AllocationTable table;
		StreamRouter router;
		router.apply_configure(msg, table,
				       auto_with_links({{"owner1", "owner2"}, {"owner2", "source"}}));
		for (uint32_t id = 1; id <= 3; ++id) {
			REQUIRE(router.route(id).has_value());
			CHECK(router.route(id)->slot == int(id));
		}
	}
}

TEST_CASE("使う声の組は、画面のストリームには適用されない")
{
	AllocationTable table;
	StreamRouter router;

	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "owner", StreamKind::Screen));
	msg.streams.push_back(make_stream(2, "source", StreamKind::Screen));
	router.apply_configure(msg, table, auto_with_links({{"owner", "source"}}));

	REQUIRE(router.route(1).has_value());
	REQUIRE(router.route(2).has_value());
	CHECK(router.route(1)->slot == 1);
	CHECK(router.route(2)->slot == 2);
	CHECK(table.all_screen_slots_consistent());
}

// ---------------------------------------------------------------------------
// メインアカウントの声 (stream_router.h の AssignPolicy の main_account_user_id と route_main_account_voice)。
// 経路を作らない形 (route_main_account_voice が偽。合成のソースがないとき) から確認する。
// ---------------------------------------------------------------------------

namespace {

AssignPolicy auto_with_main_account(std::string main_account, std::map<std::string, std::string> links = {})
{
	AssignPolicy policy = kAuto;
	policy.main_account_user_id = std::move(main_account);
	policy.voice_links = std::move(links);
	return policy;
}

} // namespace

TEST_CASE("サブアカウントの構成では、メインアカウントの声に声の枠の経路を作らず、画面とほかの人の声は今までどおり")
{
	AllocationTable table;
	StreamRouter router;
	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "main", StreamKind::Voice));
	msg.streams.push_back(make_stream(2, "friend", StreamKind::Voice));
	msg.streams.push_back(make_stream(3, "main", StreamKind::Screen));
	router.apply_configure(msg, table, auto_with_main_account("main"));

	CHECK_FALSE(router.route(1).has_value());
	CHECK(table.slot_of_owner(SlotKind::Voice, "main") == 0);
	REQUIRE(router.route(2).has_value());
	CHECK(router.route(2)->slot == 1);
	REQUIRE(router.route(3).has_value());
	CHECK(router.route(3)->kind == SlotKind::Screen);
}

TEST_CASE("メインアカウントのユーザーIDを後から入れると、その声の経路が消え、自分の声で持っていた声の枠が空く")
{
	AllocationTable table;
	StreamRouter router;
	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "main", StreamKind::Voice));
	msg.streams.push_back(make_stream(2, "friend", StreamKind::Voice));
	router.apply_configure(msg, table, kAuto);
	REQUIRE(router.route(1).has_value());
	REQUIRE(table.slot_of_owner(SlotKind::Voice, "main") == 1);

	const auto changes = router.apply_configure(msg, table, auto_with_main_account("main"));

	const RouteChange *dropped = change_of(changes, 1);
	REQUIRE(dropped != nullptr);
	CHECK(std::string(dropped->reason) == "main_account_voice");
	CHECK_FALSE(router.route(1).has_value());
	CHECK_FALSE(table.has_assignment(SlotKind::Voice, 1));
	REQUIRE(router.route(2).has_value());
	CHECK(router.route(2)->slot == 2);
}

TEST_CASE("メインアカウントの声は、使う声に選ばれていても声の枠に入らない")
{
	AllocationTable table;
	StreamRouter router;
	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "friend", StreamKind::Voice));
	msg.streams.push_back(make_stream(2, "main", StreamKind::Voice));
	router.apply_configure(msg, table,
			       auto_with_main_account("main", {{"friend", "main"}}));

	// friend 自身の声は使う声で置き換わり、置き換える先のメインアカウントの声も入らない。
	CHECK_FALSE(router.route(1).has_value());
	CHECK_FALSE(router.route(2).has_value());
}

TEST_CASE("メインアカウントが使う声でほかの人の声を運んでいる声の枠は、空けない")
{
	AllocationTable table;
	StreamRouter router;
	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "main", StreamKind::Voice));
	msg.streams.push_back(make_stream(2, "friend", StreamKind::Voice));
	router.apply_configure(msg, table,
			       auto_with_main_account("main", {{"main", "friend"}}));

	// friend の声はメインアカウントの声の枠に入り、その枠は残る。
	REQUIRE(router.route(2).has_value());
	const int slot = router.route(2)->slot;
	REQUIRE(slot != 0);
	REQUIRE(table.assignment(SlotKind::Voice, slot) != nullptr);
	CHECK(table.assignment(SlotKind::Voice, slot)->owner_user_id == "main");
	CHECK(table.assignment(SlotKind::Voice, slot)->voice_user_id == "friend");
	CHECK_FALSE(router.route(1).has_value());
}

TEST_CASE("メインアカウントのユーザーIDが空なら、だれの声も除かない")
{
	AllocationTable table;
	StreamRouter router;
	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "main", StreamKind::Voice));
	router.apply_configure(msg, table, auto_with_main_account(""));
	REQUIRE(router.route(1).has_value());
	CHECK(router.route(1)->slot == 1);
}

namespace {

// 合成のソースがあるときの決め方 (メインアカウントの声にも経路を作る)。
AssignPolicy routed_main_account(std::string main_account)
{
	AssignPolicy policy = auto_with_main_account(std::move(main_account));
	policy.route_main_account_voice = true;
	return policy;
}

} // namespace

TEST_CASE("合成のソースがあるときは、メインアカウントの声にもほかの人と同じく声の枠の経路を作る")
{
	AllocationTable table;
	StreamRouter router;
	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "main", StreamKind::Voice));
	msg.streams.push_back(make_stream(2, "friend", StreamKind::Voice));
	const auto changes = router.apply_configure(msg, table, routed_main_account("main"));

	REQUIRE(router.route(1).has_value());
	CHECK(router.route(1)->kind == SlotKind::Voice);
	CHECK(router.route(1)->user_id == "main");
	CHECK(table.slot_of_owner(SlotKind::Voice, "main") == router.route(1)->slot);
	CHECK(router.output_allowed(1, table));
	const RouteChange *added = change_of(changes, 1);
	REQUIRE(added != nullptr);
	CHECK(std::string(added->reason) == "assigned");
	REQUIRE(router.route(2).has_value());
	CHECK(router.route(2)->slot != router.route(1)->slot);
}

TEST_CASE("合成のソースがなくなると、メインアカウントの声の経路が消えて自分の声の枠が空き、戻ると経路ができる")
{
	AllocationTable table;
	StreamRouter router;
	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "main", StreamKind::Voice));
	msg.streams.push_back(make_stream(2, "friend", StreamKind::Voice));
	router.apply_configure(msg, table, routed_main_account("main"));
	REQUIRE(router.route(1).has_value());

	const auto removed = router.apply_configure(msg, table, auto_with_main_account("main"));
	const RouteChange *dropped = change_of(removed, 1);
	REQUIRE(dropped != nullptr);
	CHECK(std::string(dropped->reason) == "main_account_voice");
	CHECK_FALSE(router.route(1).has_value());
	CHECK(table.slot_of_owner(SlotKind::Voice, "main") == 0);
	REQUIRE(router.route(2).has_value());

	router.apply_configure(msg, table, routed_main_account("main"));
	REQUIRE(router.route(1).has_value());
	CHECK(table.slot_of_owner(SlotKind::Voice, "main") != 0);
}

TEST_CASE("メインアカウントの声も声の枠を 1 つ使い、ほかの人の声は人数によらず全員が経路を持つ")
{
	AllocationTable table;
	StreamRouter router;
	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "main", StreamKind::Voice));
	for (int i = 1; i <= 29; ++i) {
		const std::string user = "friend" + std::to_string(i);
		msg.streams.push_back(make_stream(static_cast<uint32_t>(10 + i), user, StreamKind::Voice));
	}
	router.apply_configure(msg, table, routed_main_account("main"));

	REQUIRE(router.route(1).has_value());
	int routed_friends = 0;
	for (int i = 1; i <= 29; ++i) {
		if (router.route(static_cast<uint32_t>(10 + i)).has_value())
			++routed_friends;
	}
	CHECK(routed_friends == 29);
	for (int slot = 1; slot <= 30; ++slot)
		CHECK(table.has_assignment(SlotKind::Voice, slot));
}

// ---------------------------------------------------------------------------
// OBS を起動し直した後 (割り当て表は保存しないので、空の表から始まる)
// ---------------------------------------------------------------------------

TEST_CASE("OBS を起動し直した後は、手動なら前に選んだ人も、画面共有を始め直しても載らず、選び直すと載る")
{
	// 起動し直した後の表は空である。前の起動で bob と alice を載せていても、覚えていない。
	AllocationTable table;
	StreamRouter router;

	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "carol", StreamKind::Screen));
	msg.streams.push_back(make_stream(2, "alice", StreamKind::Screen));
	router.apply_configure(msg, table, manual_with());
	CHECK_FALSE(router.route(1).has_value());
	CHECK_FALSE(router.route(2).has_value());
	for (int slot = 1; slot <= 4; ++slot)
		CHECK_FALSE(table.has_assignment(SlotKind::Screen, slot));

	// 利用者が alice を選ぶと、次の configure で経路ができる。
	REQUIRE(table.try_assign(SlotKind::Screen, "alice").slot == 1);
	router.apply_configure(msg, table, manual_with());
	REQUIRE(router.route(2).has_value());
	CHECK(router.route(2)->slot == 1);
	CHECK_FALSE(router.route(1).has_value());
}

// ---------------------------------------------------------------------------
// 自動の間にチェックを外した人
// ---------------------------------------------------------------------------

namespace {

ConfigureMessage screens(const std::vector<std::string> &users)
{
	ConfigureMessage msg;
	uint32_t id = 1;
	for (const auto &u : users)
		msg.streams.push_back(make_stream(id++, u, StreamKind::Screen));
	return msg;
}

// 利用者がドックで user をチェックを外した (Pipeline::release_slot と同じく、表と経路と外した人の一覧を一緒に変える)。
void uncheck(AllocationTable &table, StreamRouter &router, const std::string &user)
{
	const int slot = table.slot_of_owner(SlotKind::Screen, user);
	REQUIRE(slot != 0);
	table.release(SlotKind::Screen, slot);
	router.exclude_user(user);
	router.retarget_user(user, SlotKind::Screen, 0);
}

} // namespace

TEST_CASE("自動でチェックを外した人は、まだ画面共有をしていても次の configure で枠に戻らない")
{
	AllocationTable table;
	StreamRouter router;
	router.apply_configure(screens({"alice", "bob"}), table, kAuto);
	uncheck(table, router, "alice");

	// 外していないほかの人 (carol) は、空いた枠に今までどおり入る。
	router.apply_configure(screens({"alice", "bob", "carol"}), table, kAuto);
	CHECK(table.slot_of_owner(SlotKind::Screen, "alice") == 0);
	CHECK(router.route(1) == std::nullopt);
	CHECK(table.slot_of_owner(SlotKind::Screen, "carol") == 1);
	CHECK(router.route(3).has_value());
	CHECK(router.is_excluded("alice"));
}

TEST_CASE("外した人は、facts の sharing にいない状態を経て始め直すと枠に戻る")
{
	AllocationTable table;
	StreamRouter router;
	router.apply_configure(screens({"alice", "bob"}), table, kAuto);
	uncheck(table, router, "alice");

	// alice が共有をやめた (sharing に bob だけ)。
	router.forget_stopped_sharing({"bob"});
	CHECK_FALSE(router.is_excluded("alice"));

	router.apply_configure(screens({"bob", "alice"}), table, kAuto);
	CHECK(table.slot_of_owner(SlotKind::Screen, "alice") != 0);
	CHECK(router.route(2).has_value());
}

TEST_CASE("繋ぎ直しの順 (ストリームのない configure、sharing に外した人がいる facts、ストリームのある configure) でも戻らない")
{
	AllocationTable table;
	StreamRouter router;
	router.apply_configure(screens({"alice", "bob"}), table, kAuto);
	uncheck(table, router, "alice");

	router.clear(); // 接続が切れた
	router.apply_configure(screens({}), table, kAuto);
	CHECK(router.is_excluded("alice"));
	router.forget_stopped_sharing({"alice", "bob"}); // sharing は繋ぎ直しても変わらない
	CHECK(router.is_excluded("alice"));
	router.apply_configure(screens({"alice", "bob"}), table, kAuto);
	CHECK(table.slot_of_owner(SlotKind::Screen, "alice") == 0);
	CHECK(router.route(1) == std::nullopt);
}

TEST_CASE("ストリームのない configure だけでは、外した人の一覧を消さない")
{
	AllocationTable table;
	StreamRouter router;
	router.apply_configure(screens({"alice"}), table, kAuto);
	uncheck(table, router, "alice");
	router.apply_configure(screens({}), table, kAuto);
	CHECK(router.is_excluded("alice"));
}

TEST_CASE("外した人は、またチェックを付けると次の configure で枠に載る")
{
	AllocationTable table;
	StreamRouter router;
	router.apply_configure(screens({"alice"}), table, kAuto);
	uncheck(table, router, "alice");
	router.apply_configure(screens({"alice"}), table, kAuto);
	CHECK(table.slot_of_owner(SlotKind::Screen, "alice") == 0);

	router.include_user("alice");
	router.apply_configure(screens({"alice"}), table, kAuto);
	CHECK(table.slot_of_owner(SlotKind::Screen, "alice") != 0);
	CHECK(router.route(1).has_value());
}

TEST_CASE("選び方を切り替えて外した人の一覧を消すと、その人は枠に載る")
{
	AllocationTable table;
	StreamRouter router;
	router.apply_configure(screens({"alice"}), table, kAuto);
	uncheck(table, router, "alice");

	router.clear_excluded();
	CHECK_FALSE(router.is_excluded("alice"));
	router.apply_configure(screens({"alice"}), table, kAuto);
	CHECK(table.slot_of_owner(SlotKind::Screen, "alice") != 0);
}

TEST_CASE("手動で「自動で載せる」の人を外すと、繋ぎ直した後の configure でも戻らず、始め直すと戻る")
{
	AllocationTable table;
	StreamRouter router;
	const AssignPolicy policy = manual_with({"alice"});
	router.apply_configure(screens({"alice"}), table, policy);
	REQUIRE(table.slot_of_owner(SlotKind::Screen, "alice") != 0);
	uncheck(table, router, "alice");

	router.clear(); // 接続が切れた (screen_sharers_ も忘れる)
	router.apply_configure(screens({"alice"}), table, policy);
	CHECK(table.slot_of_owner(SlotKind::Screen, "alice") == 0);

	router.forget_stopped_sharing({}); // 共有をやめた
	router.apply_configure(screens({}), table, policy);
	router.apply_configure(screens({"alice"}), table, policy);
	CHECK(table.slot_of_owner(SlotKind::Screen, "alice") != 0);
}

TEST_CASE("外した人の一覧は、forget_stopped_sharing を呼んだときだけ、sharing にいない人から消える")
{
	// sharing_valid が false の facts では Pipeline が呼ばない (pipeline.cpp の handle_facts)ので、一覧は残る。
	// 呼ばれたときだけ消えることを確認する。
	StreamRouter router;
	router.exclude_user("alice");
	CHECK(router.is_excluded("alice"));
	router.forget_stopped_sharing({"alice"});
	CHECK(router.is_excluded("alice"));
	router.forget_stopped_sharing({});
	CHECK_FALSE(router.is_excluded("alice"));
}

// ---------------------------------------------------------------------------
// 手動で外した人の始め直しと、載せた人が抜けたとき
// ---------------------------------------------------------------------------

TEST_CASE("手動で外した人は、画面共有をやめて別の stream_id で始め直しても枠に戻らず、載せたままの人は同じ枠で続く")
{
	AllocationTable table;
	StreamRouter router;

	// 利用者が alice と bob を載せ、その後 alice を外した。
	for (const char *user : {"alice", "bob"}) {
		const auto assigned = table.try_assign(SlotKind::Screen, user);
		REQUIRE(assigned.slot != 0);
	}
	ConfigureMessage first;
	first.streams.push_back(make_stream(1, "alice", StreamKind::Screen));
	first.streams.push_back(make_stream(2, "bob", StreamKind::Screen));
	router.apply_configure(first, table, manual_with());
	table.release(SlotKind::Screen, table.slot_of_owner(SlotKind::Screen, "alice"));
	router.exclude_user("alice");
	router.retarget_user("alice", SlotKind::Screen, 0);

	// 2 人とも画面共有をやめる (facts の sharing から消える)。
	router.apply_configure(ConfigureMessage{}, table, manual_with());
	router.forget_stopped_sharing({});
	CHECK_FALSE(router.is_excluded("alice"));

	// 2 人とも別の stream_id で始め直す。alice は利用者が外したままなので、手動の間は枠に戻らない。
	ConfigureMessage again;
	again.streams.push_back(make_stream(11, "alice", StreamKind::Screen));
	again.streams.push_back(make_stream(12, "bob", StreamKind::Screen));
	router.apply_configure(again, table, manual_with());
	CHECK(table.slot_of_owner(SlotKind::Screen, "alice") == 0);
	CHECK(router.route(11) == std::nullopt);

	// 載せたままの bob は、始め直しても自分の枠の経路ができる。
	REQUIRE(router.route(12).has_value());
	CHECK(router.route(12)->user_id == "bob");
	CHECK(router.route(12)->slot == table.slot_of_owner(SlotKind::Screen, "bob"));
}

TEST_CASE("手動で 3 人を載せているとき、1 人が抜けると経路はその人の分だけ消え、全員が抜けると経路は全部消えて、割り当ては残る")
{
	AllocationTable table;
	StreamRouter router;
	for (const char *user : {"alice", "bob", "carol"}) {
		const auto assigned = table.try_assign(SlotKind::Screen, user);
		REQUIRE(assigned.slot != 0);
	}
	ConfigureMessage all;
	all.streams.push_back(make_stream(1, "alice", StreamKind::Screen));
	all.streams.push_back(make_stream(2, "bob", StreamKind::Screen));
	all.streams.push_back(make_stream(3, "carol", StreamKind::Screen));
	router.apply_configure(all, table, manual_with());
	REQUIRE(router.route_ids().size() == 3);

	// bob が通話を抜けた。bob の映像だけを空けて、残りの 2 人は動かさない。割り当ては貼り付いたまま残る。
	ConfigureMessage without_bob;
	without_bob.streams.push_back(make_stream(1, "alice", StreamKind::Screen));
	without_bob.streams.push_back(make_stream(3, "carol", StreamKind::Screen));
	const auto left = router.apply_configure(without_bob, table, manual_with());
	CHECK(cordmux::pipeline::screen_slots_share_ended(left, {1, 3}) == std::set<int>{2});
	CHECK(router.route(1).has_value());
	CHECK(router.route(3).has_value());
	CHECK(table.slot_of_owner(SlotKind::Screen, "bob") == 2);

	// 1 アカウントの形で、配信者が Vesktop で通話を抜けた。configure のストリームが全部なくなる。
	const auto gone = router.apply_configure(ConfigureMessage{}, table, manual_with());
	CHECK(cordmux::pipeline::screen_slots_share_ended(gone, {}) == std::set<int>{1, 3});
	CHECK(router.route_ids().empty());
	for (const char *user : {"alice", "bob", "carol"})
		CHECK(table.slot_of_owner(SlotKind::Screen, user) != 0);
}

namespace {

// 自分の枠に映すソースが結び付いている間の決め方。automatic で自動と手動を選ぶ。
AssignPolicy self_frame_selected_policy(bool automatic, std::string main_account)
{
	AssignPolicy policy;
	policy.automatic = automatic;
	policy.main_account_user_id = std::move(main_account);
	policy.self_frame_selected = true;
	return policy;
}

ConfigureMessage main_and_friend_screens()
{
	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "main", StreamKind::Screen));
	msg.streams.push_back(make_stream(2, "friend", StreamKind::Screen));
	return msg;
}

} // namespace

TEST_CASE("自分の枠のソースが結び付いている間は、自動でもメインアカウントの画面に経路を作らず、枠に入れない")
{
	AllocationTable table;
	StreamRouter router;
	const auto changes = router.apply_configure(main_and_friend_screens(), table,
						    self_frame_selected_policy(true, "main"));

	CHECK_FALSE(router.route(1).has_value());
	CHECK(table.slot_of_owner(SlotKind::Screen, "main") == 0);
	REQUIRE(router.route(2).has_value());
	CHECK(router.route(2)->slot == 1);
	CHECK(change_of(changes, 1) == nullptr); // 前の経路がないので、変化も返さない
}

TEST_CASE("自分の枠のソースが結び付いている間は、手動でチェックを付けた人でも、メインアカウントの画面は枠に入らない")
{
	AllocationTable table;
	StreamRouter router;
	AssignPolicy policy = self_frame_selected_policy(false, "main");
	policy.always_on = {"main", "friend"};
	router.apply_configure(main_and_friend_screens(), table, policy);

	CHECK_FALSE(router.route(1).has_value());
	CHECK(table.slot_of_owner(SlotKind::Screen, "main") == 0);
	// ほかの人は今までどおり、「自動で載せる」で入る。
	CHECK(router.route(2).has_value());
}

TEST_CASE("結び付いた時点でメインアカウントが画面の枠を持っていれば、枠が空き、経路が消える")
{
	AllocationTable table;
	StreamRouter router;
	const ConfigureMessage msg = main_and_friend_screens();
	router.apply_configure(msg, table, manual_with({"main", "friend"}));
	REQUIRE(router.route(1).has_value());
	REQUIRE(table.slot_of_owner(SlotKind::Screen, "main") != 0);

	const auto changes = router.apply_configure(msg, table, self_frame_selected_policy(false, "main"));

	const RouteChange *dropped = change_of(changes, 1);
	REQUIRE(dropped != nullptr);
	CHECK(std::string(dropped->reason) == "self_frame_selected");
	CHECK_FALSE(router.route(1).has_value());
	CHECK(table.slot_of_owner(SlotKind::Screen, "main") == 0);
	CHECK(router.route(2).has_value()); // ほかの人は動かさない
}

TEST_CASE("メインアカウントが画面共有をやめていても、結び付いた時点で持っていた画面の枠は空く")
{
	AllocationTable table;
	StreamRouter router;
	router.apply_configure(main_and_friend_screens(), table, manual_with({"main"}));
	REQUIRE(table.slot_of_owner(SlotKind::Screen, "main") != 0);

	ConfigureMessage only_friend;
	only_friend.streams.push_back(make_stream(2, "friend", StreamKind::Screen));
	router.apply_configure(only_friend, table, self_frame_selected_policy(false, "main"));

	CHECK(table.slot_of_owner(SlotKind::Screen, "main") == 0);
}

TEST_CASE("結び付きが外れたら、自動ではメインアカウントの画面が載り、手動では載せ直すまで載らない")
{
	const ConfigureMessage msg = main_and_friend_screens();
	{
		AllocationTable table;
		StreamRouter router;
		router.apply_configure(msg, table, self_frame_selected_policy(true, "main"));
		REQUIRE_FALSE(router.route(1).has_value());

		AssignPolicy unbound = self_frame_selected_policy(true, "main");
		unbound.self_frame_selected = false;
		router.apply_configure(msg, table, unbound);
		CHECK(router.route(1).has_value());
	}
	{
		AllocationTable table;
		StreamRouter router;
		router.apply_configure(msg, table, self_frame_selected_policy(false, "main"));

		AssignPolicy unbound = self_frame_selected_policy(false, "main");
		unbound.self_frame_selected = false;
		router.apply_configure(msg, table, unbound);
		CHECK_FALSE(router.route(1).has_value()); // 手動は、利用者が載せ直すまで載らない
		CHECK(table.slot_of_owner(SlotKind::Screen, "main") == 0);
	}
}

TEST_CASE("自分の枠のソースが結び付いていても、メインアカウントの声の扱いは変わらない")
{
	AllocationTable table;
	StreamRouter router;
	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "main", StreamKind::Voice));
	msg.streams.push_back(make_stream(2, "main", StreamKind::Screen));
	AssignPolicy policy = self_frame_selected_policy(true, "main");
	policy.route_main_account_voice = true;
	router.apply_configure(msg, table, policy);

	REQUIRE(router.route(1).has_value());
	CHECK(router.route(1)->kind == SlotKind::Voice);
	CHECK_FALSE(router.route(2).has_value());

	// 声の経路を作らない側も、画面の理由に置き換わらず "main_account_voice" のまま。
	policy.route_main_account_voice = false;
	const auto changes = router.apply_configure(msg, table, policy);
	const RouteChange *dropped = change_of(changes, 1);
	REQUIRE(dropped != nullptr);
	CHECK(std::string(dropped->reason) == "main_account_voice");
}

TEST_CASE("メインアカウントのユーザーIDが空なら、結び付いていても画面の枠に何もしない")
{
	AllocationTable table;
	StreamRouter router;
	router.apply_configure(main_and_friend_screens(), table, self_frame_selected_policy(true, ""));

	CHECK(router.route(1).has_value());
	CHECK(router.route(2).has_value());
}

// ---------------------------------------------------------------------------
// admit_sharers: 受け取る範囲が枠に載せた人だけのときの、映像が届く前の割り当て
// ---------------------------------------------------------------------------

TEST_CASE("admit_sharers: 自動のときは、画面共有している人を、外した人を除いて順に枠に載せる")
{
	AllocationTable table;
	StreamRouter router;
	router.exclude_user("2");

	const auto admitted = router.admit_sharers({"1", "2", "3"}, table, kAuto);
	CHECK(admitted == std::vector<std::string>{"1", "3"});
	CHECK(table.slot_of_owner(SlotKind::Screen, "1") != 0);
	CHECK(table.slot_of_owner(SlotKind::Screen, "2") == 0);
	CHECK(table.slot_of_owner(SlotKind::Screen, "3") != 0);

	// 同じ人をもう一度渡しても、新しく載せない。
	CHECK(router.admit_sharers({"1", "3"}, table, kAuto).empty());
}

TEST_CASE("admit_sharers: 枠が足りなければ載せない。空のユーザー ID は無視する")
{
	AllocationTable table;
	StreamRouter router;
	const std::vector<std::string> sharers = {"", "1", "2", "3", "4", "5"};
	const auto admitted = router.admit_sharers(sharers, table, kAuto);
	CHECK(admitted.size() == 4);
	CHECK(table.slot_of_owner(SlotKind::Screen, "5") == 0);
}

TEST_CASE("admit_sharers: 手動のときは、「自動で載せる」の人が画面共有を始めたときだけ、空いている枠に載せる")
{
	AllocationTable table;
	StreamRouter router;
	const AssignPolicy policy = manual_with({"1"});

	// 「自動で載せる」でない人は載せない。
	auto admitted = router.admit_sharers({"2"}, table, policy);
	CHECK(admitted.empty());
	CHECK(table.slot_of_owner(SlotKind::Screen, "2") == 0);

	// 「自動で載せる」の人が新しく画面共有を始めたら載せる。
	admitted = router.admit_sharers({"2", "1"}, table, policy);
	CHECK(admitted == std::vector<std::string>{"1"});

	// 利用者が外した後は、画面共有を続けている間は載せない (外した人の一覧)。
	table.release(SlotKind::Screen, table.slot_of_owner(SlotKind::Screen, "1"));
	router.exclude_user("1");
	CHECK(router.admit_sharers({"1"}, table, policy).empty());

	// 画面共有をやめて (facts から消えて)、始め直せば載せる。
	router.forget_stopped_sharing({});
	CHECK(router.admit_sharers({}, table, policy).empty());
	CHECK(router.admit_sharers({"1"}, table, policy) == std::vector<std::string>{"1"});
}

TEST_CASE("admit_sharers: 手動で、始めたのではない (前から画面共有している) 人は、枠が空いても載せない")
{
	AllocationTable table;
	StreamRouter router;
	// 枠 4 つを手動で埋めた人が画面共有している間に、「自動で載せる」の別の人も画面共有している。
	for (const char *id : {"2", "3", "4", "5"})
		table.try_assign(SlotKind::Screen, id);
	const AssignPolicy policy = manual_with({"1"});
	CHECK(router.admit_sharers({"1", "2", "3", "4", "5"}, table, policy).empty());
	// 1 つ空いても、1 は前から画面共有しているので、載せない。
	table.release(SlotKind::Screen, table.slot_of_owner(SlotKind::Screen, "5"));
	CHECK(router.admit_sharers({"1", "2", "3", "4"}, table, policy).empty());
}

TEST_CASE("admit_sharers: 自分の枠で出す人 (メインアカウント) は載せない")
{
	AllocationTable table;
	StreamRouter router;
	AssignPolicy policy = kAuto;
	policy.main_account_user_id = "9";
	policy.self_frame_selected = true;
	CHECK(router.admit_sharers({"9", "1"}, table, policy) == std::vector<std::string>{"1"});
	policy.self_frame_selected = false;
	CHECK(router.admit_sharers({"9", "1"}, table, policy) == std::vector<std::string>{"9"});
}

TEST_CASE("admit_sharers: 枠が埋まっていれば載せず、空けた枠には載せる (ほかの人の枠は取り上げない)")
{
	AllocationTable table;
	StreamRouter router;
	for (const char *id : {"1", "2", "3", "4"})
		table.try_assign(SlotKind::Screen, id);
	// 4 つとも埋まっていると、新しく共有を始めた人は載らない。
	CHECK(router.admit_sharers({"1", "2", "3", "4", "5"}, table, kAuto).empty());
	CHECK(table.slot_of_owner(SlotKind::Screen, "5") == 0);
	// 共有をやめた 1 の枠が空くと、5 が載る。載せた人の映像は、前の人の枠を引き継いだのではないので止めない。
	table.release(SlotKind::Screen, 1);
	const auto admitted = router.admit_sharers({"2", "3", "4", "5"}, table, kAuto);
	REQUIRE(admitted == std::vector<std::string>{"5"});
	CHECK(table.slot_of_owner(SlotKind::Screen, "5") == 1);
	CHECK(table.is_video_allowed(SlotKind::Screen, 1));
}

TEST_CASE("replace_slot_in は枠の人を別の人に替え、枠の番号は変えず、経路も 1 回で付け替える")
{
	using cordmux::pipeline::replace_slot_in;
	using Outcome = cordmux::pipeline::Pipeline::SlotChange::Outcome;
	AllocationTable table;
	StreamRouter router;
	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "alice", StreamKind::Screen));
	msg.streams.push_back(make_stream(2, "bob", StreamKind::Screen));
	msg.streams.push_back(make_stream(3, "carol", StreamKind::Screen));
	router.apply_configure(msg, table, kAuto);
	const std::int64_t bob_order = table.assignment(SlotKind::Screen, 2)->added_order;

	int out_n = -1;
	int in_n = -1;
	// carol が枠 3 に載っていると、替えられない。
	const auto none = replace_slot_in(table, router, "bob", "carol", &out_n, &in_n);
	CHECK(none.outcome == Outcome::AlreadyOwned);
	CHECK(table.slot_of_owner(SlotKind::Screen, "bob") == 2);

	// carol を枠から外してから、bob と替える。
	table.release(SlotKind::Screen, 3);
	router.retarget_user("carol", SlotKind::Screen, 0);
	const auto change = replace_slot_in(table, router, "bob", "carol", &out_n, &in_n);
	CHECK(change.outcome == Outcome::Replaced);
	CHECK(change.slot == 2);
	CHECK(out_n == 1);
	CHECK(in_n == 1);
	CHECK(change.retargeted == 1);

	// 枠 2 は carol のものになり、bob は枠にいない。alice の枠は変わらない。
	CHECK(table.slot_of_owner(SlotKind::Screen, "carol") == 2);
	CHECK(table.slot_of_owner(SlotKind::Screen, "bob") == 0);
	CHECK(table.slot_of_owner(SlotKind::Screen, "alice") == 1);
	// 空きの枠は増えない (枠の数は変わらず、空いているのは外した carol の元の枠 3 だけ)。
	CHECK(table.first_free_slot(SlotKind::Screen) == 3);
	// 載せた順は出る人の番号を引き継ぐ。
	const auto *assignment = table.assignment(SlotKind::Screen, 2);
	REQUIRE(assignment != nullptr);
	CHECK(assignment->owner_user_id == "carol");
	CHECK(assignment->voice_user_id == "carol");
	CHECK(assignment->added_order == bob_order);

	// 経路: bob の経路は枠 0 になって出力が止まり、carol の経路は枠 2 を指す。
	CHECK(router.route(2)->slot == 0);
	CHECK_FALSE(router.output_allowed(2, table));
	CHECK(router.route(3)->slot == 2);
	CHECK(router.output_allowed(3, table));
	// 外した bob は、画面共有を始め直すまで、次の configure でも枠に戻らない。
	CHECK(router.is_excluded("bob"));
	CHECK_FALSE(router.is_excluded("carol"));
}

TEST_CASE("replace_slot_in は、外す人が枠にいないときと空の userId では何も変えない")
{
	using cordmux::pipeline::replace_slot_in;
	using Outcome = cordmux::pipeline::Pipeline::SlotChange::Outcome;
	AllocationTable table;
	StreamRouter router;
	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "alice", StreamKind::Screen));
	router.apply_configure(msg, table, kAuto);

	CHECK(replace_slot_in(table, router, "zed", "bob", nullptr, nullptr).outcome == Outcome::NotAssigned);
	CHECK(replace_slot_in(table, router, "alice", "", nullptr, nullptr).outcome == Outcome::NotAssigned);
	CHECK(replace_slot_in(table, router, "alice", "alice", nullptr, nullptr).outcome == Outcome::AlreadyOwned);
	CHECK(table.slot_of_owner(SlotKind::Screen, "alice") == 1);
	CHECK_FALSE(router.is_excluded("alice"));
	CHECK(router.route(1)->slot == 1);
}

TEST_CASE("replace_slot_in で替わった人は、外した人の枠の番号にそのまま入る")
{
	using cordmux::pipeline::replace_slot_in;
	AllocationTable table;
	StreamRouter router;
	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "alice", StreamKind::Screen));
	msg.streams.push_back(make_stream(2, "bob", StreamKind::Screen));
	router.apply_configure(msg, table, kAuto);

	replace_slot_in(table, router, "alice", "dave", nullptr, nullptr);
	// 外した人の枠の番号に、替わった人が入る。
	CHECK(table.slot_of_owner(SlotKind::Screen, "dave") == 1);
	CHECK(table.slot_of_owner(SlotKind::Screen, "bob") == 2);
}

TEST_CASE("replace_slot_in は載せた順の番号を入る人が引き継ぎ、載せた順で並べるとほかの人は動かない")
{
	using cordmux::pipeline::replace_slot_in;
	AllocationTable table;
	StreamRouter router;
	ConfigureMessage msg;
	uint32_t id = 1;
	for (const char *user : {"A", "B", "C", "D"})
		msg.streams.push_back(make_stream(id++, user, StreamKind::Screen));
	router.apply_configure(msg, table, kAuto);

	// 載せた順の並び (番号の昇順に枠の人を並べる)。
	const auto owners_by_order = [&table]() {
		std::vector<std::pair<std::int64_t, std::string>> items;
		for (int slot = 1; slot <= cordmux::config::kScreenSlotCount; ++slot) {
			if (const auto *a = table.assignment(SlotKind::Screen, slot))
				items.emplace_back(a->added_order, a->owner_user_id);
		}
		std::sort(items.begin(), items.end());
		std::vector<std::string> out;
		for (const auto &item : items)
			out.push_back(item.second);
		return out;
	};
	REQUIRE(owners_by_order() == std::vector<std::string>{"A", "B", "C", "D"});

	int out_n = 0;
	int in_n = 0;
	replace_slot_in(table, router, "B", "E", &out_n, &in_n);
	CHECK(owners_by_order() == std::vector<std::string>{"A", "E", "C", "D"});
}

// ---- 枠に載せていない人の全画面 (AssignPolicy::focus_user_id) ----

TEST_CASE("手動で枠に載せていない人を全画面にすると、割り当て表に触らずに全画面だけで映す子への経路を作り、映像だけを出す")
{
	AllocationTable table;
	StreamRouter router;
	AssignPolicy policy = manual_with();
	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "alice", StreamKind::Screen, true));
	// 全画面にする前は、枠に載せていないので経路がない。
	CHECK(router.apply_configure(msg, table, policy).empty());
	CHECK_FALSE(router.route(1).has_value());

	policy.focus_user_id = "alice";
	const auto changes = router.apply_configure(msg, table, policy);
	REQUIRE(changes.size() == 1);
	CHECK(std::string(changes[0].reason) == "focus_only");
	REQUIRE(router.route(1).has_value());
	CHECK(router.route(1)->kind == SlotKind::Screen);
	CHECK(router.route(1)->slot == cordmux::config::kFocusOnlySlot);
	CHECK(table.slot_of_owner(SlotKind::Screen, "alice") == 0); // 割り当て表は変えない
	CHECK(router.video_allowed(1, table));
	CHECK_FALSE(router.output_allowed(1, table)); // 画面共有の音は出さない
	// シーンコレクションの切り替えの最中は、映像も出さない。
	table.begin_scene_collection_change();
	CHECK_FALSE(router.video_allowed(1, table));
	table.apply_scene_collection_changed();

	// 全画面をやめると、経路を消す。
	policy.focus_user_id.clear();
	const auto back = router.apply_configure(msg, table, policy);
	REQUIRE(back.size() == 1);
	CHECK(std::string(back[0].reason) == "not_chosen");
	CHECK_FALSE(router.route(1).has_value());
}

TEST_CASE("全画面にした人が画面の枠を持つなら、全画面だけで映す子への経路は作らず、枠への経路のまま")
{
	AllocationTable table;
	StreamRouter router;
	REQUIRE(table.try_assign(SlotKind::Screen, "alice").slot == 1);
	AssignPolicy policy = manual_with();
	policy.focus_user_id = "alice";
	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "alice", StreamKind::Screen));
	router.apply_configure(msg, table, policy);
	REQUIRE(router.route(1).has_value());
	CHECK(router.route(1)->slot == 1);
}

TEST_CASE("自動で枠が足りずに載らない人を全画面にすると、全画面だけで映す子への経路を作る")
{
	AllocationTable table;
	StreamRouter router;
	AssignPolicy policy = kAuto;
	ConfigureMessage msg;
	for (uint32_t id = 1; id <= 5; ++id)
		msg.streams.push_back(make_stream(id, "p" + std::to_string(id), StreamKind::Screen));
	router.apply_configure(msg, table, policy);
	CHECK_FALSE(router.route(5).has_value()); // 5 人目は枠が足りない

	policy.focus_user_id = "p5";
	router.apply_configure(msg, table, policy);
	REQUIRE(router.route(5).has_value());
	CHECK(router.route(5)->slot == cordmux::config::kFocusOnlySlot);
	for (uint32_t id = 1; id <= 4; ++id)
		CHECK(router.route(id)->slot == static_cast<int>(id)); // 枠の人は変わらない
}

TEST_CASE("自分の枠で出すメインアカウントは、全画面にしても全画面だけで映す子への経路を作らない")
{
	AllocationTable table;
	StreamRouter router;
	AssignPolicy policy = manual_with();
	policy.main_account_user_id = "main";
	policy.self_frame_selected = true;
	policy.focus_user_id = "main";
	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "main", StreamKind::Screen));
	router.apply_configure(msg, table, policy);
	CHECK_FALSE(router.route(1).has_value());
}

TEST_CASE("place_added_order_after は、枠の人の載せた順を指定の番号の人の後に置き、ほかの人どうしの順を変えない")
{
	using cordmux::pipeline::place_added_order_after;
	AllocationTable table;
	REQUIRE(table.try_assign(SlotKind::Screen, "a").slot == 1); // 番号 1
	REQUIRE(table.try_assign(SlotKind::Screen, "b").slot == 2); // 番号 2
	REQUIRE(table.try_assign(SlotKind::Screen, "c").slot == 3); // 番号 3
	REQUIRE(table.try_assign(SlotKind::Screen, "main").slot == 4); // 番号 4 (今の最後)
	const auto order_of = [&](int slot) { return table.assignment(SlotKind::Screen, slot)->added_order; };
	// main を a (番号 1) の後に置く: a、main、b、c の順になる。
	place_added_order_after(table, 4, 1);
	CHECK(order_of(1) < order_of(4));
	CHECK(order_of(4) < order_of(2));
	CHECK(order_of(2) < order_of(3));
	// 誰よりも前 (0 の後) に置く。
	place_added_order_after(table, 4, 0);
	CHECK(order_of(4) < order_of(1));
	CHECK(order_of(1) < order_of(2));
	CHECK(order_of(2) < order_of(3));
	// 番号は重ならない。
	std::set<std::int64_t> orders{order_of(1), order_of(2), order_of(3), order_of(4)};
	CHECK(orders.size() == 4);
	// 後から載せた人は、ずらした後の誰よりも後になる。
	table.release(SlotKind::Screen, 3);
	REQUIRE(table.try_assign(SlotKind::Screen, "d").slot == 3);
	CHECK(order_of(3) > order_of(2));
	CHECK(order_of(3) > order_of(4));
}

// ---------------------------------------------------------------------------
// 自分の枠を使っている間の人数の上限 (config::screen_slot_capacity)。手動で載せる操作 (ledger/actions.cpp の SetSlot) と同じく、
// 「自動」と [自動で枠に載せる] で載せる経路も、Discord の人を 3 人までにする。自分の枠と合わせて、描く物が 4 つを超えない
// (どの人も描かれずに落ちない) ことを、配信の配置と同じ関数で確かめる。
// ---------------------------------------------------------------------------

namespace {

// 自分の枠を使っている間の決め方 (メインアカウントは "main"。自分の枠に映す映像を OBS から選び、チェックが付いている)。
AssignPolicy in_use(AssignPolicy policy)
{
	policy.main_account_user_id = "main";
	policy.self_frame_selected = true;
	policy.self_frame_in_use = true;
	return policy;
}

// users の画面のストリーム (stream_id は 1 から順に振る)。
ConfigureMessage screens_of(const std::vector<std::string> &users)
{
	ConfigureMessage msg;
	uint32_t id = 1;
	for (const std::string &user : users)
		msg.streams.push_back(make_stream(id++, user, StreamKind::Screen));
	return msg;
}

// 画面の枠 (1 から 4) への経路を持つ stream_id の数。
int routed_to_slots(const StreamRouter &router, const ConfigureMessage &msg)
{
	int n = 0;
	for (const StreamConfig &stream : msg.streams) {
		const auto route = router.route(stream.stream_id);
		if (route && route->kind == SlotKind::Screen && route->slot >= 1 &&
		    route->slot <= cordmux::config::kScreenSlotCount)
			++n;
	}
	return n;
}

// 割り当て表の画面の枠と自分の枠を、配信の配置 (sources/composite_layout.h の plan_composite_layout) と全画面にできるタイル
// (sources/composite_input.h の composite_focus_tiles) に通し、描く物が 4 つ以下で、誰も落ちず、自分の枠を出していれば
// 自分の枠も全画面にできることを確かめる。並び順 2 つと、[先頭に配置] の有無の両方で見る。自分の枠の名前と載せた順の印は、
// 先頭に置かないときに最後に並ぶ値にする (落ちるなら自分の枠が落ちる並び)。
void check_drawn_within_four(const AllocationTable &table, bool self_shown)
{
	std::vector<int> visible;
	std::vector<cordmux::sources::SlotPerson> people;
	for (int slot = 1; slot <= cordmux::config::kScreenSlotCount; ++slot) {
		const cordmux::config::SlotAssignment *a = table.assignment(SlotKind::Screen, slot);
		if (a == nullptr)
			continue;
		visible.push_back(slot);
		cordmux::sources::SlotPerson person;
		person.slot = slot;
		person.name = a->owner_user_id;
		person.user_id = a->owner_user_id;
		person.added_order = a->added_order;
		people.push_back(person);
	}
	for (const bool first : {true, false}) {
		for (const cordmux::config::SlotOrder order : {cordmux::config::SlotOrder::Name, cordmux::config::SlotOrder::Added}) {
			CAPTURE(first);
			CAPTURE(static_cast<int>(order));
			cordmux::sources::CompositeInput input =
				cordmux::sources::make_composite_input(visible, people, self_shown, true, 0);
			input.slot_order = order;
			input.self_first = first;
			input.self_user_id = "main";
			input.self_name = "zzz";
			input.self_added_mark = 1000;
			const cordmux::sources::CompositeLayout layout =
				cordmux::sources::plan_composite_layout(cordmux::sources::make_layout_request(input, 1920, 1080));
			const std::size_t expected = visible.size() + (self_shown ? 1u : 0u);
			CHECK(expected <= 4u);
			CHECK(layout.tiles.size() == expected);
			CHECK(layout.dropped.empty());
			const std::vector<cordmux::sources::CompositeFocusTile> focus = cordmux::sources::composite_focus_tiles(input);
			CHECK(focus.size() == expected);
			// 自分の枠のタイルは、自分の枠の名前で見分ける (メインアカウントを画面共有の枠に載せると、その枠も "main" になる)。
			const bool self_focusable =
				std::any_of(focus.begin(), focus.end(), [](const cordmux::sources::CompositeFocusTile &tile) {
					return tile.user_id == "main" && tile.name == "zzz";
				});
			CHECK(self_focusable == self_shown);
		}
	}
}

} // namespace

TEST_CASE("自分の枠を使っている間は、自動で画面共有を始めた人が 3 人まで載り、4 人目は載らない")
{
	AllocationTable table;
	StreamRouter router;
	const ConfigureMessage msg = screens_of({"a", "b", "c", "d"});
	const auto changes = router.apply_configure(msg, table, in_use(kAuto));

	CHECK(table.assigned_count(SlotKind::Screen) == 3);
	CHECK(routed_to_slots(router, msg) == 3);
	CHECK_FALSE(router.route(4).has_value()); // 4 人目は経路を持たない (映像をデコードしない)
	CHECK(table.slot_of_owner(SlotKind::Screen, "d") == 0);
	CHECK(table.first_free_slot(SlotKind::Screen) == 4); // 4 つ目の枠は空いたまま (自分の枠の分)
	CHECK(change_of(changes, 4) == nullptr);
	check_drawn_within_four(table, true);
}

TEST_CASE("自分の枠を使っていなければ、自動で 4 人まで載る")
{
	AllocationTable table;
	StreamRouter router;
	AssignPolicy policy = in_use(kAuto);
	policy.self_frame_in_use = false; // 映像は OBS から選んでいるが、自分の行のチェックが外れている
	const ConfigureMessage msg = screens_of({"a", "b", "c", "d"});
	router.apply_configure(msg, table, policy);

	CHECK(table.assigned_count(SlotKind::Screen) == 4);
	CHECK(routed_to_slots(router, msg) == 4);
	check_drawn_within_four(table, false);
}

TEST_CASE("自分の枠を使っている間は、[自動で枠に載せる] の人も 3 人まで載り、4 人目は載らない")
{
	AllocationTable table;
	StreamRouter router;
	const ConfigureMessage msg = screens_of({"a", "b", "c", "d"});
	const auto changes = router.apply_configure(msg, table, in_use(manual_with({"a", "b", "c", "d"})));

	CHECK(table.assigned_count(SlotKind::Screen) == 3);
	CHECK(routed_to_slots(router, msg) == 3);
	CHECK_FALSE(router.route(4).has_value());
	CHECK(change_of(changes, 4) == nullptr);
	check_drawn_within_four(table, true);

	// 自分の枠を使っていなければ 4 人目も載る (始めたときに枠が空いている)。
	AllocationTable table4;
	StreamRouter router4;
	AssignPolicy policy = in_use(manual_with({"a", "b", "c", "d"}));
	policy.self_frame_in_use = false;
	router4.apply_configure(msg, table4, policy);
	CHECK(table4.assigned_count(SlotKind::Screen) == 4);
}

TEST_CASE("自分の枠を使っている間は、手動で 3 人を載せた後に [自動で枠に載せる] の人が始めても載らない")
{
	AllocationTable table;
	StreamRouter router;
	for (const char *id : {"a", "b", "c"})
		REQUIRE(table.try_assign(SlotKind::Screen, id).outcome == cordmux::config::AssignOutcome::Assigned);
	const AssignPolicy policy = in_use(manual_with({"d"}));
	const ConfigureMessage before = screens_of({"a", "b", "c"});
	router.apply_configure(before, table, policy);
	// d が画面共有を始めた。4 つ目の枠は空いているが、自分の枠の分なので載せない。
	const ConfigureMessage after = screens_of({"a", "b", "c", "d"});
	router.apply_configure(after, table, policy);
	CHECK(table.slot_of_owner(SlotKind::Screen, "d") == 0);
	CHECK_FALSE(router.route(4).has_value());
	check_drawn_within_four(table, true);
}

TEST_CASE("admit_sharers: 自分の枠を使っている間は、自動でも [自動で枠に載せる] でも 3 人まで載せる")
{
	// OBS を起動したとき、シーンの読み込みが終わる前に facts が届くと、ここで画面共有している人を載せる。自分の枠は
	// まだ結び付いていないが、チェックが付いていれば上限を当てる。
	{
		AllocationTable table;
		StreamRouter router;
		const auto admitted = router.admit_sharers({"a", "b", "c", "d", "e"}, table, in_use(kAuto));
		CHECK(admitted == std::vector<std::string>{"a", "b", "c"});
		CHECK(table.assigned_count(SlotKind::Screen) == 3);
		check_drawn_within_four(table, true);
	}
	{
		AllocationTable table;
		StreamRouter router;
		const auto admitted =
			router.admit_sharers({"a", "b", "c", "d"}, table, in_use(manual_with({"a", "b", "c", "d"})));
		CHECK(admitted == std::vector<std::string>{"a", "b", "c"});
		CHECK(table.slot_of_owner(SlotKind::Screen, "d") == 0);
		check_drawn_within_four(table, true);
	}
	{
		// 使っていなければ 4 人まで。
		AllocationTable table;
		StreamRouter router;
		AssignPolicy policy = in_use(kAuto);
		policy.self_frame_in_use = false;
		CHECK(router.admit_sharers({"a", "b", "c", "d", "e"}, table, policy).size() == 4);
	}
}

TEST_CASE("自分の枠を後から使い始めても、すでに枠に載っている 4 人は外さない")
{
	AllocationTable table;
	StreamRouter router;
	const ConfigureMessage msg = screens_of({"a", "b", "c", "d"});
	AssignPolicy policy = in_use(kAuto);
	policy.self_frame_in_use = false;
	router.apply_configure(msg, table, policy);
	REQUIRE(table.assigned_count(SlotKind::Screen) == 4);

	policy.self_frame_in_use = true;
	const auto changes = router.apply_configure(msg, table, policy);
	CHECK(changes.empty());
	CHECK(table.assigned_count(SlotKind::Screen) == 4);
	CHECK(routed_to_slots(router, msg) == 4);
	CHECK(router.admit_sharers({"a", "b", "c", "d"}, table, policy).empty());
	CHECK(table.assigned_count(SlotKind::Screen) == 4);
}

TEST_CASE("自分の枠を使うのをやめると、自動では待っていた人が空いた枠に載り、[自動で枠に載せる] の人は始め直すまで載らない")
{
	const ConfigureMessage msg = screens_of({"a", "b", "c", "d"});
	{
		AllocationTable table;
		StreamRouter router;
		AssignPolicy policy = in_use(kAuto);
		router.apply_configure(msg, table, policy);
		REQUIRE_FALSE(router.route(4).has_value());
		// 自分の行のチェックを外した (Pipeline::set_assign_policy は直前の configure を適用し直す)。
		policy.self_frame_in_use = false;
		router.apply_configure(msg, table, policy);
		REQUIRE(router.route(4).has_value());
		CHECK(router.route(4)->slot == 4);
		check_drawn_within_four(table, false);
	}
	{
		AllocationTable table;
		StreamRouter router;
		AssignPolicy policy = in_use(manual_with({"a", "b", "c", "d"}));
		router.apply_configure(msg, table, policy);
		REQUIRE_FALSE(router.route(4).has_value());
		policy.self_frame_in_use = false;
		router.apply_configure(msg, table, policy);
		// d は前から画面共有している (「始めた」ではない) ので、枠が空いても載らない (空いている枠がなかったときと同じ)。
		CHECK_FALSE(router.route(4).has_value());
		// 画面共有をやめて始め直すと載る。
		router.apply_configure(screens_of({"a", "b", "c"}), table, policy);
		router.apply_configure(msg, table, policy);
		REQUIRE(router.route(4).has_value());
		CHECK(router.route(4)->slot == 4);
	}
}

TEST_CASE("自分の枠を使っている間に上限で載らない人を全画面にすると、全画面だけで映す子への経路を作る")
{
	AllocationTable table;
	StreamRouter router;
	AssignPolicy policy = in_use(kAuto);
	const ConfigureMessage msg = screens_of({"a", "b", "c", "d"});
	router.apply_configure(msg, table, policy);
	REQUIRE_FALSE(router.route(4).has_value());

	policy.focus_user_id = "d";
	router.apply_configure(msg, table, policy);
	REQUIRE(router.route(4).has_value());
	CHECK(router.route(4)->slot == cordmux::config::kFocusOnlySlot);
	CHECK(table.slot_of_owner(SlotKind::Screen, "d") == 0); // 割り当て表は変えない
	CHECK(table.assigned_count(SlotKind::Screen) == 3);
}

TEST_CASE("自分の枠を使っている間は、どの経路を通っても、描く物が 4 つを超えない")
{
	// 自分の枠を使い続けたまま、枠に人を載せる経路を順に通す。各段のあとで、自分の枠と画面の枠を配信の配置に通し、
	// 描く物が 4 つ以下で、誰も落ちないことを確かめる。手動で載せる操作は、ドックが同じ上限で押せなくする (ledger の
	// テスト) ので、ここでは割り当て表に直接載せる形で、上限に達していない間だけ通す。
	using cordmux::pipeline::replace_slot_in;
	AllocationTable table;
	StreamRouter router;
	const AssignPolicy auto_policy = in_use(kAuto);
	const AssignPolicy manual_policy = in_use(manual_with({"f", "g"}));

	// (1) OBS の起動。シーンの読み込みの前に facts が届き、画面共有している 5 人を載せる。
	router.admit_sharers({"a", "b", "c", "d", "e"}, table, auto_policy);
	check_drawn_within_four(table, true);

	// (2) シーンの読み込みが終わり、保留していた configure を適用する (自動)。
	const ConfigureMessage five = screens_of({"a", "b", "c", "d", "e"});
	router.apply_configure(five, table, auto_policy);
	CHECK(routed_to_slots(router, five) == 3);
	check_drawn_within_four(table, true);

	// (3) Vesktop との接続が切れて繋ぎ直した (経路を忘れ、同じ configure がまた届く)。
	router.clear();
	router.apply_configure(five, table, auto_policy);
	CHECK(routed_to_slots(router, five) == 3);
	check_drawn_within_four(table, true);

	// (4) 枠の入れ替え (a を外して d を載せる)。人数は変わらない。
	int out_n = 0;
	int in_n = 0;
	replace_slot_in(table, router, "a", "d", &out_n, &in_n);
	CHECK(table.assigned_count(SlotKind::Screen) == 3);
	check_drawn_within_four(table, true);

	// (5) 1 人が画面共有をやめて枠が空き (pipeline/slot_release.h)、自動で待っていた人が載る。
	table.release(SlotKind::Screen, table.slot_of_owner(SlotKind::Screen, "b"));
	const ConfigureMessage without_b = screens_of({"a", "c", "d", "e"});
	router.apply_configure(without_b, table, auto_policy);
	CHECK(table.assigned_count(SlotKind::Screen) == 3);
	check_drawn_within_four(table, true);

	// (6) 手動に替えて、[自動で枠に載せる] の f と g が画面共有を始める (configure と facts の両方)。
	const ConfigureMessage with_fg = screens_of({"a", "c", "d", "e", "f", "g"});
	router.apply_configure(with_fg, table, manual_policy);
	router.admit_sharers({"a", "c", "d", "e", "f", "g"}, table, manual_policy);
	CHECK(table.slot_of_owner(SlotKind::Screen, "f") == 0);
	CHECK(table.slot_of_owner(SlotKind::Screen, "g") == 0);
	check_drawn_within_four(table, true);

	// (7) 手動で 1 人を外し、空いた枠に 1 人を載せる (ドックの SetSlot。上限に達していないので押せる)。
	table.release(SlotKind::Screen, table.slot_of_owner(SlotKind::Screen, "c"));
	REQUIRE(table.assigned_count(SlotKind::Screen) < cordmux::config::screen_slot_capacity(true));
	table.try_assign(SlotKind::Screen, "f");
	router.apply_configure(with_fg, table, manual_policy);
	CHECK(table.assigned_count(SlotKind::Screen) == 3);
	check_drawn_within_four(table, true);

	// (8) 自分の枠を使うのをやめた後は、自動で 4 人まで載り、自分の枠は描かない。
	AssignPolicy off = auto_policy;
	off.self_frame_in_use = false;
	router.apply_configure(with_fg, table, off);
	CHECK(table.assigned_count(SlotKind::Screen) == 4);
	check_drawn_within_four(table, false);

	// (9) ドックが (8) の前のスナップショット (Discord の人が 3 人) で自分の枠をまた付けると判断し、Pipeline に渡したときには
	// (8) で 4 人目が載っていた。Pipeline は使い始めることを断る (ドックはチェックを外す)。
	using cordmux::pipeline::accepted_self_frame_in_use;
	CHECK_FALSE(accepted_self_frame_in_use(table, false, true, "main"));
	check_drawn_within_four(table, false);

	// (10) 1 人 (c) が画面共有をやめて枠が空き、次の周期で付けると、割り込みがなければ受け付ける。その後は自動で待っている人
	// (g) が載らない。
	table.release(SlotKind::Screen, table.slot_of_owner(SlotKind::Screen, "c"));
	REQUIRE(accepted_self_frame_in_use(table, false, true, "main"));
	const ConfigureMessage without_c = screens_of({"a", "d", "e", "f", "g"});
	router.apply_configure(without_c, table, auto_policy);
	CHECK(table.slot_of_owner(SlotKind::Screen, "g") == 0);
	check_drawn_within_four(table, true);

	// (11) 手動に替えて 1 人 (f) を外した後、ドックが 2 人のスナップショットで g を載せると判断した。Pipeline に渡す前に、
	// [自動で枠に載せる] の h が画面共有を始めて 3 人目に載った。手動で載せる操作は断る (Insufficient)。
	using cordmux::pipeline::assign_slot_in;
	using cordmux::pipeline::release_slot_in;
	const AssignPolicy manual_h = in_use(manual_with({"h"}));
	router.apply_configure(without_c, table, manual_h);
	REQUIRE(release_slot_in(table, router, SlotKind::Screen, "f").outcome ==
		cordmux::pipeline::Pipeline::SlotChange::Outcome::Released);
	REQUIRE(screen_slot_room(table, manual_h)); // ドックの判断 (2 人なので押せる)
	const ConfigureMessage with_h = screens_of({"a", "d", "e", "f", "g", "h"});
	router.apply_configure(with_h, table, manual_h);
	REQUIRE(table.slot_of_owner(SlotKind::Screen, "h") != 0);
	CHECK(assign_slot_in(table, router, manual_h, SlotKind::Screen, "g").outcome ==
	      cordmux::pipeline::Pipeline::SlotChange::Outcome::Insufficient);
	CHECK(table.slot_of_owner(SlotKind::Screen, "g") == 0);
	CHECK(table.assigned_count(SlotKind::Screen) == 3);
	check_drawn_within_four(table, true);

	// (12) 自分の枠の映像を「Discordの画面共有」に替え、メインアカウントを 4 人目に載せる (1 回の呼び出し)。自分の枠は描かない。
	using cordmux::pipeline::move_self_frame_in;
	AssignPolicy moving = manual_h;
	CHECK(move_self_frame_in(table, router, moving, "main", "main", false).change.outcome ==
	      cordmux::pipeline::Pipeline::SlotChange::Outcome::Assigned);
	CHECK(table.assigned_count(SlotKind::Screen) == 4);
	check_drawn_within_four(table, false);

	// (13) OBS のソースに戻し、メインアカウントの枠を自分の枠に替える (1 回の呼び出し)。
	CHECK(move_self_frame_in(table, router, moving, "main", "main", true).self_frame_in_use);
	CHECK(table.assigned_count(SlotKind::Screen) == 3);
	check_drawn_within_four(table, true);
}

// ---------------------------------------------------------------------------
// ドックと Pipeline のスレッドの間の競合。ドックは、自分の枠を付けるか、4 人目を載せるかを、スナップショット (最大 1 周期前) の
// 人数で決める。Pipeline に渡す前に、Pipeline のスレッド (configure と facts) が自動で人を載せていることがある。上限の判断は、
// 割り当て表と同じロック (Pipeline の Impl::mu) の中で動く関数 (stream_router.h の accepted_self_frame_in_use、pipeline.h の
// assign_slot_in と move_self_frame_in) が持つので、ここでは決まった順で割り込ませて確かめる。
// ---------------------------------------------------------------------------

namespace {

using cordmux::pipeline::accepted_self_frame_in_use;
using cordmux::pipeline::assign_slot_in;
using cordmux::pipeline::move_self_frame_in;
using cordmux::pipeline::release_slot_in;
using Outcome = cordmux::pipeline::Pipeline::SlotChange::Outcome;

// ドックが見るスナップショットの、Discord の人で埋まっている画面の枠の数 (自分の枠の人 "main" を数えない。dock/dock_poll.cpp の
// discord_slotted_of と同じ数え方)。
int discord_slotted_in(const AllocationTable &table)
{
	int n = 0;
	for (int slot = 1; slot <= cordmux::config::kScreenSlotCount; ++slot) {
		const cordmux::config::SlotAssignment *a = table.assignment(SlotKind::Screen, slot);
		if (a != nullptr && a->owner_user_id != "main")
			++n;
	}
	return n;
}

// ドックの自分の枠の状態を、スナップショットの人数で 1 つ進める (dock/dock_poll.cpp の step_self_frame と set_self_frame)。
cordmux::sources::SelfFrameResult dock_step(const cordmux::sources::SelfFrameState &state,
					    cordmux::sources::SelfFrameEvent event, int discord_slotted)
{
	cordmux::sources::SelfFrameInput in;
	in.state = state;
	in.event = event;
	in.share = cordmux::sources::SelfShare::On;
	in.selected = true;
	in.bound = true;
	in.automatic = true;
	in.discord_slotted = discord_slotted;
	return cordmux::sources::plan_self_frame(in);
}

} // namespace

TEST_CASE("自分の枠を使い始めてよいかは、自分の枠の人の枠を数えずに、Discord の人が 4 人いるかで決める")
{
	AllocationTable four_discord;
	for (const char *id : {"a", "b", "c", "d"})
		REQUIRE(four_discord.try_assign(SlotKind::Screen, id).outcome == cordmux::config::AssignOutcome::Assigned);
	CHECK_FALSE(accepted_self_frame_in_use(four_discord, false, true, "main"));
	// 使っている間に渡し直しても断らない (押しのけない。普通は 4 人にならない)。やめる向きは断らない。
	CHECK(accepted_self_frame_in_use(four_discord, true, true, "main"));
	CHECK_FALSE(accepted_self_frame_in_use(four_discord, false, false, "main"));
	CHECK_FALSE(accepted_self_frame_in_use(four_discord, true, false, "main"));

	// メインアカウントの画面共有の枠は数えない (自分の枠に映す映像を OBS から選ぶと、apply_configure が空ける)。
	AllocationTable with_main;
	for (const char *id : {"a", "main", "b", "c"})
		REQUIRE(with_main.try_assign(SlotKind::Screen, id).outcome == cordmux::config::AssignOutcome::Assigned);
	CHECK(accepted_self_frame_in_use(with_main, false, true, "main"));
	// 自分の枠の人がほかの人 (メインアカウントだけの構成の Vesktop のアカウント) なら、メインアカウントの枠も数える。
	CHECK_FALSE(accepted_self_frame_in_use(with_main, false, true, "vesktop"));
	AllocationTable with_vesktop;
	for (const char *id : {"a", "vesktop", "b", "c"})
		REQUIRE(with_vesktop.try_assign(SlotKind::Screen, id).outcome == cordmux::config::AssignOutcome::Assigned);
	CHECK(accepted_self_frame_in_use(with_vesktop, false, true, "vesktop"));
	// 空いていれば受け付ける。
	AllocationTable three;
	for (const char *id : {"a", "b", "c"})
		REQUIRE(three.try_assign(SlotKind::Screen, id).outcome == cordmux::config::AssignOutcome::Assigned);
	CHECK(accepted_self_frame_in_use(three, false, true, "main"));
}

TEST_CASE("競合: ドックが自分の枠を付けると判断した後に、自動で 4 人目が載っていれば、Pipeline が断りチェックが外れる")
{
	using cordmux::sources::SelfFrameEvent;
	// 周期の処理 (自動で、共有の間に枠が空いた) と、ドックのチェックの操作 (SetSelfFrame) の 2 つの判断。
	for (const SelfFrameEvent event : {SelfFrameEvent::Poll, SelfFrameEvent::DockOn}) {
		CAPTURE(static_cast<int>(event));
		AllocationTable table;
		StreamRouter router;
		AssignPolicy policy = in_use(kAuto);
		policy.self_frame_in_use = false; // 映像は OBS から選んでいて、チェックはまだ付いていない
		const ConfigureMessage three = screens_of({"a", "b", "c"});
		router.apply_configure(three, table, policy);
		REQUIRE(table.assigned_count(SlotKind::Screen) == 3);

		// ドックが、Discord の人が 3 人のスナップショットで、自分の枠を付けると判断した。
		cordmux::sources::SelfFrameState state;
		state.prev = cordmux::sources::SelfShare::On;
		const cordmux::sources::SelfFrameResult decided = dock_step(state, event, discord_slotted_in(table));
		REQUIRE(decided.state.on);

		// Pipeline に渡す前に、Pipeline のスレッドが configure を適用し、自動で d を 4 人目に載せた。
		const ConfigureMessage four = screens_of({"a", "b", "c", "d"});
		router.apply_configure(four, table, policy);
		REQUIRE(table.assigned_count(SlotKind::Screen) == 4);

		// ドックが自分の枠を使い始めることを渡す。Pipeline は割り当て表と同じロックの中で断る。
		policy.self_frame_in_use = accepted_self_frame_in_use(table, policy.self_frame_in_use, true, "main");
		CHECK_FALSE(policy.self_frame_in_use);
		// ドックはチェックを外す。excluded にはしない。
		const cordmux::sources::SelfFrameResult refused = dock_step(decided.state, SelfFrameEvent::Refused, 4);
		CHECK_FALSE(refused.state.on);
		CHECK_FALSE(refused.shown);
		CHECK_FALSE(refused.state.excluded);
		check_drawn_within_four(table, refused.shown);

		// 次の周期は 4 人のスナップショットで見送る。d が画面共有をやめて枠が空くと、自動では付き、今度は受け付ける。
		const cordmux::sources::SelfFrameResult deferred =
			dock_step(refused.state, SelfFrameEvent::Poll, discord_slotted_in(table));
		CHECK(deferred.deferred);
		CHECK_FALSE(deferred.state.on);
		table.release(SlotKind::Screen, table.slot_of_owner(SlotKind::Screen, "d"));
		const cordmux::sources::SelfFrameResult again =
			dock_step(deferred.state, SelfFrameEvent::Poll, discord_slotted_in(table));
		REQUIRE(again.state.on);
		policy.self_frame_in_use = accepted_self_frame_in_use(table, policy.self_frame_in_use, true, "main");
		CHECK(policy.self_frame_in_use);
		// d が画面共有を始め直しても、自分の枠の分の枠には載らない。
		router.apply_configure(four, table, policy);
		CHECK(table.slot_of_owner(SlotKind::Screen, "d") == 0);
		check_drawn_within_four(table, again.shown);
	}
}

TEST_CASE("競合: 手動で 4 人目を載せると判断した後に、[自動で枠に載せる] の人が 3 人目に載っていれば、Pipeline が断る")
{
	AllocationTable table;
	StreamRouter router;
	const AssignPolicy policy = in_use(manual_with({"c"}));
	for (const char *id : {"a", "b"})
		REQUIRE(assign_slot_in(table, router, policy, SlotKind::Screen, id).outcome == Outcome::Assigned);
	router.apply_configure(screens_of({"a", "b", "d"}), table, policy);

	// ドックが、2 人のスナップショットで d を載せられると判断した (ledger の self_frame_limit_reached と同じ上限)。
	REQUIRE(screen_slot_room(table, policy));
	// Pipeline に渡す前に、[自動で枠に載せる] の c が画面共有を始め、3 人目に載った。
	const ConfigureMessage with_c = screens_of({"a", "b", "d", "c"});
	router.apply_configure(with_c, table, policy);
	REQUIRE(table.slot_of_owner(SlotKind::Screen, "c") != 0);

	// 手動で載せる操作は、割り当て表と同じロックの中で上限を見て断る (ドックの扱いは、空いている枠がないときと同じ)。
	const cordmux::pipeline::Pipeline::SlotChange refused = assign_slot_in(table, router, policy, SlotKind::Screen, "d");
	CHECK(refused.outcome == Outcome::Insufficient);
	CHECK(refused.slot == 0);
	CHECK(table.slot_of_owner(SlotKind::Screen, "d") == 0);
	CHECK(table.assigned_count(SlotKind::Screen) == 3);
	check_drawn_within_four(table, true);

	// もう載っている人は上限に関係なく AlreadyOwned。自分の枠を使っていなければ 4 人目を載せられる。
	CHECK(assign_slot_in(table, router, policy, SlotKind::Screen, "a").outcome == Outcome::AlreadyOwned);
	AssignPolicy off = policy;
	off.self_frame_in_use = false;
	const cordmux::pipeline::Pipeline::SlotChange assigned = assign_slot_in(table, router, off, SlotKind::Screen, "d");
	CHECK(assigned.outcome == Outcome::Assigned);
	CHECK(assigned.retargeted == 0); // d の経路はまだない (Pipeline::assign_slot は直前の configure を適用し直す)
	router.apply_configure(with_c, table, off);
	REQUIRE(router.route(3).has_value());
	CHECK(router.route(3)->slot == assigned.slot);
}

TEST_CASE("競合: 自分の枠の映像を OBS のソース -> Discordの画面共有 -> OBS のソースと替えても、自動で待っている人がいても 4 つを超えない")
{
	// メインアカウント (main) は画面共有していて、自分の枠に映す映像は OBS のソース。Discord の a、b、c が載り、e が自動で
	// 待っている。
	AllocationTable table;
	StreamRouter router;
	AssignPolicy policy = in_use(kAuto);
	const ConfigureMessage msg = screens_of({"a", "b", "c", "main", "e"});
	router.apply_configure(msg, table, policy);
	REQUIRE(table.assigned_count(SlotKind::Screen) == 3);
	REQUIRE(table.slot_of_owner(SlotKind::Screen, "e") == 0);
	check_drawn_within_four(table, true);
	// 自分の枠の載せた順の印 (ドックの self_order_mark_)。a の後、b の前に並んでいたことにする。
	const std::int64_t mark = table.assignment(SlotKind::Screen, table.slot_of_owner(SlotKind::Screen, "a"))->added_order;

	// ---- OBS のソース -> Discordの画面共有 ----
	{
		// 分けて渡す 2 つの順は、どちらもメインアカウントを載せ直せない。
		// (1) 自分の枠を使っている間に載せる: 上限 (3 人) で断られる。
		AllocationTable t1 = table;
		StreamRouter r1 = router;
		CHECK(assign_slot_in(t1, r1, policy, SlotKind::Screen, "main").outcome == Outcome::Insufficient);
		// (2) 先に使うのをやめる: その間に Pipeline のスレッドが configure を適用すると、e が 4 つ目の枠に載り、メインアカウントは
		// 載らない。
		AllocationTable t2 = table;
		StreamRouter r2 = router;
		AssignPolicy p2 = policy;
		p2.self_frame_in_use = false;
		r2.apply_configure(msg, t2, p2);
		CHECK(t2.slot_of_owner(SlotKind::Screen, "e") != 0);
		CHECK(assign_slot_in(t2, r2, p2, SlotKind::Screen, "main").outcome == Outcome::Insufficient);
	}
	// 1 回の呼び出し (Pipeline::move_self_frame) なら、自分の枠の分の枠にメインアカウントが載る。
	const cordmux::pipeline::Pipeline::SelfFrameMove to_slot = move_self_frame_in(table, router, policy, "main", "main", false, mark);
	CHECK(to_slot.change.outcome == Outcome::Assigned);
	CHECK_FALSE(to_slot.self_frame_in_use);
	CHECK_FALSE(policy.self_frame_in_use);
	CHECK_FALSE(policy.self_frame_selected);
	// 載せた順は、自分の枠の位置 (a の後) を引き継ぐ。
	const int main_slot = table.slot_of_owner(SlotKind::Screen, "main");
	REQUIRE(main_slot != 0);
	CHECK(table.assignment(SlotKind::Screen, main_slot)->added_order == mark + 1);
	// 呼び出しの後に Pipeline のスレッドが configure を適用しても、e は載らず、メインアカウントの画面に経路ができる。
	router.apply_configure(msg, table, policy);
	CHECK(table.slot_of_owner(SlotKind::Screen, "e") == 0);
	REQUIRE(router.route(4).has_value());
	CHECK(router.route(4)->slot == main_slot);
	check_drawn_within_four(table, false);

	// ---- Discordの画面共有 -> OBS のソース ----
	{
		// 分けて渡すと、枠を空けた後に Pipeline のスレッドが configure を適用し、e が空いた枠に載って、自分の枠を使い始める
		// ことが断られる (メインアカウントが配信から消える)。
		AllocationTable t3 = table;
		StreamRouter r3 = router;
		REQUIRE(release_slot_in(t3, r3, SlotKind::Screen, "main").outcome == Outcome::Released);
		r3.apply_configure(msg, t3, policy);
		CHECK(t3.slot_of_owner(SlotKind::Screen, "e") != 0);
		CHECK_FALSE(accepted_self_frame_in_use(t3, false, true, "main"));
	}
	// 1 回の呼び出しなら、メインアカウントの枠を空けて、自分の枠を使い始める。
	const cordmux::pipeline::Pipeline::SelfFrameMove to_self = move_self_frame_in(table, router, policy, "main", "main", true);
	CHECK(to_self.change.outcome == Outcome::Released);
	CHECK(to_self.change.slot == main_slot);
	CHECK(to_self.self_frame_in_use);
	CHECK(policy.self_frame_in_use);
	CHECK(policy.self_frame_selected);
	CHECK(router.is_excluded("main")); // 外した人として覚える (release_slot と同じ)
	// 呼び出しの後に configure を適用しても、e は自分の枠の分の枠に載らず、メインアカウントの画面の経路はなくなる。
	router.apply_configure(msg, table, policy);
	CHECK(table.slot_of_owner(SlotKind::Screen, "e") == 0);
	CHECK_FALSE(router.route(4).has_value());
	CHECK(table.assigned_count(SlotKind::Screen) == 3);
	check_drawn_within_four(table, true);

	// もう 1 度 Discordの画面共有に替えても、同じく載せ直せる。
	CHECK(move_self_frame_in(table, router, policy, "main", "main", false).change.outcome == Outcome::Assigned);
	router.apply_configure(msg, table, policy);
	CHECK(table.slot_of_owner(SlotKind::Screen, "e") == 0);
	check_drawn_within_four(table, false);
}

TEST_CASE("自分の枠に替えるとき、メインアカウントの枠がもう空いていて Discord の人が 4 人なら、使い始めることを断る")
{
	// ドックのスナップショットではメインアカウントが載っていたが、渡す前にメインアカウントが画面共有をやめて枠が空き
	// (pipeline/slot_release.h)、自動で待っていた e が載っていた。
	AllocationTable table;
	StreamRouter router;
	AssignPolicy policy = in_use(kAuto);
	policy.self_frame_selected = false;
	policy.self_frame_in_use = false;
	for (const char *id : {"a", "b", "c", "e"})
		REQUIRE(table.try_assign(SlotKind::Screen, id).outcome == cordmux::config::AssignOutcome::Assigned);
	const cordmux::pipeline::Pipeline::SelfFrameMove moved = move_self_frame_in(table, router, policy, "main", "main", true);
	CHECK(moved.change.outcome == Outcome::NotAssigned);
	CHECK_FALSE(moved.self_frame_in_use);
	CHECK_FALSE(policy.self_frame_in_use);
	CHECK(policy.self_frame_selected); // 映像の選択は替わる (メインアカウントの画面を画面の枠に入れない)
	check_drawn_within_four(table, false);
}
