// SPDX-License-Identifier: Apache-2.0

// config/allocation.h (枠の割り当て表) のテスト。

#include "doctest.h"

#include "config/allocation.h"

#include <map>
#include <set>
#include <string>

using cordmux::config::AllocationTable;
using cordmux::config::AssignOutcome;
using cordmux::config::SlotKind;

TEST_CASE("空の表では、画面共有の枠の空き枠に割り当てる")
{
	AllocationTable table;
	auto r = table.try_assign(SlotKind::Screen, "u1");
	CHECK(r.outcome == AssignOutcome::Assigned);
	CHECK(r.slot == 1);
	CHECK(table.has_assignment(SlotKind::Screen, 1));
	CHECK(table.assignment(SlotKind::Screen, 1)->owner_user_id == "u1");
	// 既定では、声も同じ人になる。
	CHECK(table.assignment(SlotKind::Screen, 1)->voice_user_id == "u1");
	CHECK(table.is_output_allowed(SlotKind::Screen, 1));
}

TEST_CASE("空き枠は番号が小さい順に埋まる")
{
	AllocationTable table;
	table.try_assign(SlotKind::Screen, "u1");
	table.try_assign(SlotKind::Screen, "u2");
	CHECK(table.assignment(SlotKind::Screen, 1)->owner_user_id == "u1");
	CHECK(table.assignment(SlotKind::Screen, 2)->owner_user_id == "u2");
}

TEST_CASE("同じ人をもう一度割り当てても、枠の番号は変わらない")
{
	AllocationTable table;
	table.try_assign(SlotKind::Screen, "u1");
	table.try_assign(SlotKind::Screen, "u2");

	auto r = table.try_assign(SlotKind::Screen, "u1");
	CHECK(r.outcome == AssignOutcome::AlreadyOwned);
	CHECK(r.slot == 1);
}

TEST_CASE("画面共有の枠は 4 つまで、声の枠は上限なく割り当てられる")
{
	AllocationTable table;
	for (int i = 1; i <= 4; ++i)
		CHECK(table.try_assign(SlotKind::Screen, "u" + std::to_string(i)).outcome == AssignOutcome::Assigned);
	auto over = table.try_assign(SlotKind::Screen, "u5");
	CHECK(over.outcome == AssignOutcome::Insufficient);

	// 9 人目以降も声の枠に入る。番号は 1 から順に増える。
	for (int i = 1; i <= 30; ++i) {
		const auto r = table.try_assign(SlotKind::Voice, "v" + std::to_string(i));
		CHECK(r.outcome == AssignOutcome::Assigned);
		CHECK(r.slot == i);
	}
	CHECK(table.slot_extent(SlotKind::Voice) == 30);
	CHECK(table.assignment(SlotKind::Voice, 30)->owner_user_id == "v30");
	CHECK(table.try_assign(SlotKind::Voice, "v9").outcome == AssignOutcome::AlreadyOwned);
}

TEST_CASE("声の枠は、空けた枠から順に埋まり、末尾の空きは削られる")
{
	AllocationTable table;
	for (int i = 1; i <= 10; ++i)
		table.try_assign(SlotKind::Voice, "v" + std::to_string(i));
	table.release(SlotKind::Voice, 3);
	table.release(SlotKind::Voice, 5);
	// 空いた枠のうち、番号の小さい枠から入る。
	CHECK(table.try_assign(SlotKind::Voice, "new1").slot == 3);
	CHECK(table.try_assign(SlotKind::Voice, "new2").slot == 5);
	CHECK(table.try_assign(SlotKind::Voice, "new3").slot == 11);
	// 末尾から空けると、枠の数が減る。途中の空きは残る。
	table.release(SlotKind::Voice, 11);
	table.release(SlotKind::Voice, 10);
	CHECK(table.slot_extent(SlotKind::Voice) == 9);
	table.release(SlotKind::Voice, 4);
	CHECK(table.slot_extent(SlotKind::Voice) == 9);
	CHECK(table.first_free_slot(SlotKind::Voice) == 4);
}

TEST_CASE("assigned_count は割り当てのある枠を種類ごとに数え、途中の空きは数えない")
{
	AllocationTable table;
	CHECK(table.assigned_count(SlotKind::Screen) == 0);
	CHECK(table.assigned_count(SlotKind::Voice) == 0);
	table.try_assign(SlotKind::Screen, "a");
	table.try_assign(SlotKind::Screen, "b");
	table.try_assign(SlotKind::Screen, "c");
	table.try_assign(SlotKind::Voice, "a");
	table.release(SlotKind::Screen, 2);
	CHECK(table.assigned_count(SlotKind::Screen) == 2);
	CHECK(table.assigned_count(SlotKind::Voice) == 1);
}

TEST_CASE("画面共有の枠に載せられる人数は、自分の枠を使っている間 (映像を OBS から選び、チェックが付いている) だけ 1 つ少ない")
{
	using cordmux::config::kScreenSlotCount;
	using cordmux::config::screen_slot_capacity;
	using cordmux::config::self_frame_in_use;
	CHECK(screen_slot_capacity(false) == kScreenSlotCount);
	CHECK(screen_slot_capacity(true) == kScreenSlotCount - 1);
	CHECK(self_frame_in_use(true, true));
	CHECK_FALSE(self_frame_in_use(true, false));
	CHECK_FALSE(self_frame_in_use(false, true)); // 「Discordの画面共有」か「(なし)」の間に残したチェックは数えない
	CHECK_FALSE(self_frame_in_use(false, false));
}

// 画面共有の枠 (4 つ) をすべて埋めて Insufficient を作る (声の枠には上限がなく、Insufficient にならない)。
TEST_CASE("空き枠がなければ Insufficient を返し、割り当てを変えない (ほかの人の枠を取り上げない)")
{
	AllocationTable table;
	table.try_assign(SlotKind::Screen, "p1");
	table.try_assign(SlotKind::Screen, "p2");
	table.try_assign(SlotKind::Screen, "p3");
	table.try_assign(SlotKind::Screen, "p4");

	auto r = table.try_assign(SlotKind::Screen, "new-guy");
	CHECK(r.outcome == AssignOutcome::Insufficient);
	CHECK(r.slot == 0);
	for (int slot = 1; slot <= 4; ++slot)
		CHECK(table.assignment(SlotKind::Screen, slot)->owner_user_id == "p" + std::to_string(slot));
}

TEST_CASE("使う声を置き換えても、owner は変わらない")
{
	AllocationTable table;
	table.try_assign(SlotKind::Voice, "taro");
	table.set_voice_source(SlotKind::Voice, 1, "taro-alt-account");

	const auto *a = table.assignment(SlotKind::Voice, 1);
	REQUIRE(a != nullptr);
	CHECK(a->owner_user_id == "taro"); // 表示名とミキサーの行は、載っている人のまま
	CHECK(a->voice_user_id == "taro-alt-account");

	// 同じ人の枠かどうかは owner_user_id で判定する (置き換えた後も同じ枠に残る)。
	auto r = table.try_assign(SlotKind::Voice, "taro");
	CHECK(r.outcome == AssignOutcome::AlreadyOwned);
	CHECK(r.slot == 1);
}

TEST_CASE("rewrite は枠の人を別の人に書き直し、出力は割り当てがある間は出る")
{
	AllocationTable table;
	table.try_assign(SlotKind::Screen, "taro");
	CHECK(table.is_output_allowed(SlotKind::Screen, 1));

	table.rewrite(SlotKind::Screen, 1, "jiro", "jiro");
	CHECK(table.is_output_allowed(SlotKind::Screen, 1));
	CHECK(table.assignment(SlotKind::Screen, 1)->owner_user_id == "jiro");
}

TEST_CASE("割り当てのない枠は出力できない")
{
	AllocationTable table;
	CHECK_FALSE(table.is_output_allowed(SlotKind::Screen, 1));
	CHECK(table.assignment(SlotKind::Screen, 1) == nullptr);
}

TEST_CASE("範囲の外の枠の番号には false か nullptr を返す")
{
	AllocationTable table;
	CHECK_FALSE(table.has_assignment(SlotKind::Screen, 0));
	CHECK_FALSE(table.has_assignment(SlotKind::Screen, 5));
	CHECK_FALSE(table.has_assignment(SlotKind::Voice, 1)); // 声の枠は使っている番号までしかない
	CHECK(table.assignment(SlotKind::Voice, -1) == nullptr);
	CHECK_FALSE(table.is_output_allowed(SlotKind::Screen, 100));
}

TEST_CASE("シーンコレクションを切り替える間は凍結し、割り当てと出してよいかの判定は変えない")
{
	AllocationTable table;
	table.try_assign(SlotKind::Voice, "taro");
	table.try_assign(SlotKind::Screen, "kenji");

	CHECK_FALSE(table.is_frozen());
	table.begin_scene_collection_change();
	CHECK(table.is_frozen());

	// 凍結している間も、割り当てはそのまま読める (受信は続く。凍結を見て出さないのは stream_router.h)。
	CHECK(table.assignment(SlotKind::Voice, 1)->owner_user_id == "taro");
	CHECK(table.is_output_allowed(SlotKind::Voice, 1));
	CHECK(table.is_video_allowed(SlotKind::Screen, 1));
	CHECK(table.is_output_allowed(SlotKind::Screen, 1));

	table.apply_scene_collection_changed();
	CHECK_FALSE(table.is_frozen());
	// 切り替えた後も、どの枠も止めない。
	CHECK(table.assignment(SlotKind::Voice, 1)->owner_user_id == "taro");
	CHECK(table.is_output_allowed(SlotKind::Voice, 1));
	CHECK(table.is_video_allowed(SlotKind::Screen, 1));
	CHECK(table.is_output_allowed(SlotKind::Screen, 1));
}

TEST_CASE("is_video_allowed は声の枠、割り当てのない枠、範囲の外の枠、外した枠で false を返す")
{
	AllocationTable table;
	CHECK_FALSE(table.is_video_allowed(SlotKind::Screen, 1)); // 割り当てがない
	CHECK_FALSE(table.is_video_allowed(SlotKind::Screen, 0));
	CHECK_FALSE(table.is_video_allowed(SlotKind::Screen, 5));
	CHECK_FALSE(table.is_video_allowed(SlotKind::Voice, 9));

	// 声の枠は映像を運ばないので、割り当てがあって音声を出せても、映像の判定は false になる。
	table.try_assign(SlotKind::Voice, "taro");
	CHECK(table.is_output_allowed(SlotKind::Voice, 1));
	CHECK_FALSE(table.is_video_allowed(SlotKind::Voice, 1));

	table.try_assign(SlotKind::Screen, "taro");
	CHECK(table.is_video_allowed(SlotKind::Screen, 1));
	table.release(SlotKind::Screen, 1);
	CHECK_FALSE(table.is_video_allowed(SlotKind::Screen, 1));
}

TEST_CASE("is_consistent は、画面共有の枠の owner と voice が同じなら true を返す")
{
	AllocationTable table;
	table.try_assign(SlotKind::Screen, "taro");
	CHECK(table.is_consistent(SlotKind::Screen, 1));
	CHECK(table.all_screen_slots_consistent());
}

TEST_CASE("is_consistent は、画面共有の枠で voice だけを置き換えると false を返す")
{
	// 画面共有の枠は声を運ばないので、set_voice_source を呼ぶこと自体が呼び出し元の取り違えである。
	// is_consistent がそれを検出できることを確認する (この関数は修正しない。allocation.h のコメント)。
	AllocationTable table;
	table.try_assign(SlotKind::Screen, "taro");
	table.set_voice_source(SlotKind::Screen, 1, "jiro");
	CHECK_FALSE(table.is_consistent(SlotKind::Screen, 1));
	CHECK_FALSE(table.all_screen_slots_consistent());
}

TEST_CASE("is_consistent は、割り当てのない画面共有の枠で true を返す")
{
	AllocationTable table;
	CHECK(table.is_consistent(SlotKind::Screen, 1));
	CHECK(table.all_screen_slots_consistent());
}

TEST_CASE("is_consistent は、声の枠では owner と voice が違っても true を返す")
{
	AllocationTable table;
	table.try_assign(SlotKind::Voice, "taro");
	table.set_voice_source(SlotKind::Voice, 1, "jiro");
	CHECK(table.is_consistent(SlotKind::Voice, 1));
}

TEST_CASE("all_screen_slots_consistent は、1 つの枠でも約束が崩れていれば false を返す")
{
	AllocationTable table;
	table.try_assign(SlotKind::Screen, "taro");
	table.try_assign(SlotKind::Screen, "jiro");
	table.set_voice_source(SlotKind::Screen, 2, "saburo");
	CHECK(table.is_consistent(SlotKind::Screen, 1));
	CHECK_FALSE(table.is_consistent(SlotKind::Screen, 2));
	CHECK_FALSE(table.all_screen_slots_consistent());
}

TEST_CASE("begin を呼ばずに apply だけを呼んでも、何もしない")
{
	AllocationTable table;
	table.try_assign(SlotKind::Screen, "taro");
	table.apply_scene_collection_changed();
	CHECK(table.is_output_allowed(SlotKind::Screen, 1));
	CHECK_FALSE(table.is_frozen());
}

TEST_CASE("release は枠を空きに戻す")
{
	AllocationTable table;
	table.try_assign(SlotKind::Screen, "taro");
	CHECK(table.slot_of_owner(SlotKind::Screen, "taro") == 1);
	CHECK(table.is_output_allowed(SlotKind::Screen, 1));

	table.release(SlotKind::Screen, 1);
	CHECK_FALSE(table.has_assignment(SlotKind::Screen, 1));
	CHECK_FALSE(table.is_output_allowed(SlotKind::Screen, 1));
	CHECK(table.slot_of_owner(SlotKind::Screen, "taro") == 0);
	CHECK(table.assignment(SlotKind::Screen, 1) == nullptr);

	// 空きに戻ったので、次の人が同じ枠に入れる。
	auto r = table.try_assign(SlotKind::Screen, "jiro");
	CHECK(r.outcome == AssignOutcome::Assigned);
	CHECK(r.slot == 1);
	CHECK(table.is_output_allowed(SlotKind::Screen, 1));
}

TEST_CASE("release は範囲の外と空き枠には何もしない")
{
	AllocationTable table;
	table.try_assign(SlotKind::Screen, "taro");
	table.release(SlotKind::Screen, 0);
	table.release(SlotKind::Screen, 5);
	table.release(SlotKind::Screen, 2); // もともと空き
	CHECK(table.has_assignment(SlotKind::Screen, 1)); // 1 はそのまま
	CHECK(table.slot_of_owner(SlotKind::Screen, "") == 0);
}

TEST_CASE("release は種類ごとに適用され、画面共有の枠から外しても声の枠は残る")
{
	AllocationTable table;
	table.try_assign(SlotKind::Screen, "taro");
	table.try_assign(SlotKind::Voice, "taro");

	table.release(SlotKind::Screen, 1);
	CHECK_FALSE(table.has_assignment(SlotKind::Screen, 1));
	CHECK(table.has_assignment(SlotKind::Voice, 1));
	CHECK(table.slot_of_owner(SlotKind::Voice, "taro") == 1);
}

TEST_CASE("使う声の組は 1 対 1 で、連鎖と重なりはすべて落とす")
{
	using cordmux::config::usable_voice_links;
	using Links = std::map<std::string, std::string>;

	// 普通の 1 組と、互いに関わらない 2 組目は、そのまま採る。
	CHECK(usable_voice_links(Links{{"owner1", "source1"}}) == Links{{"owner1", "source1"}});
	CHECK(usable_voice_links(Links{{"owner1", "source1"}, {"owner2", "source2"}}) ==
	      Links{{"owner1", "source1"}, {"owner2", "source2"}});

	// 空の値と、本人を指す組は、置き換えていないのと同じ (空の上書きと同じ約束)。
	CHECK(usable_voice_links(Links{{"owner1", ""}, {"owner2", "owner2"}, {"", "source1"}}).empty());

	// 1 人を 2 人の使う声にしている組は、どちらも採らない (どちらかを勝たせない)。関係のない組は残る。
	CHECK(usable_voice_links(Links{{"owner1", "source1"}, {"owner2", "source1"}, {"owner3", "source3"}}) ==
	      Links{{"owner3", "source3"}});

	// 連鎖 (owner1 の使う声の source1 が、さらに source2 を使う声に持つ) では、両方の組を落とす。
	CHECK(usable_voice_links(Links{{"owner1", "source1"}, {"source1", "source2"}}).empty());

	// 互いを使う声にする 2 人も、連鎖である。
	CHECK(usable_voice_links(Links{{"owner1", "owner2"}, {"owner2", "owner1"}}).empty());
}

namespace {

// 画面共有の枠 1 から 4 に a、b、c、d を載せた表。
AllocationTable four_slots_filled()
{
	AllocationTable table;
	table.try_assign(SlotKind::Screen, "a");
	table.try_assign(SlotKind::Screen, "b");
	table.try_assign(SlotKind::Screen, "c");
	table.try_assign(SlotKind::Screen, "d");
	return table;
}

} // namespace

TEST_CASE("枠を空けると、次の人は空いた枠に入り、ほかの人の枠は動かない")
{
	AllocationTable table = four_slots_filled();
	table.release(SlotKind::Screen, 2);
	auto r = table.try_assign(SlotKind::Screen, "e");
	CHECK(r.outcome == AssignOutcome::Assigned);
	CHECK(r.slot == 2);
	CHECK(table.assignment(SlotKind::Screen, 1)->owner_user_id == "a");
	CHECK(table.assignment(SlotKind::Screen, 3)->owner_user_id == "c");
	CHECK(table.assignment(SlotKind::Screen, 4)->owner_user_id == "d");
	// 載せた順の番号は、新しく載せた人が最後になる。
	CHECK(table.assignment(SlotKind::Screen, 2)->added_order > table.assignment(SlotKind::Screen, 4)->added_order);
	// 空きがなくなれば、ほかの人の枠は取り上げない。
	CHECK(table.try_assign(SlotKind::Screen, "f").outcome == AssignOutcome::Insufficient);
}
