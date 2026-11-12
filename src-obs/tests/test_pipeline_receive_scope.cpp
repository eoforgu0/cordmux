// SPDX-License-Identifier: Apache-2.0

// pipeline/receive_scope.h の単体テスト。受け取る範囲 (設定の receive_scope) から決まる、送信側に伝える視聴する人の範囲と、
// 録画の入力に通してよい画面のストリームを確認する。

#include "doctest.h"

#include "pipeline/receive_scope.h"

#include <set>
#include <string>
#include <vector>

using cordmux::config::AllocationTable;
using cordmux::config::ReceiveScope;
using cordmux::config::SlotKind;
using cordmux::pipeline::record_stream_allowed;
using cordmux::pipeline::watch_scope_of;

TEST_CASE("受け取る範囲が全員のときは、枠に載っている人がいても、視聴する人を絞らない")
{
	AllocationTable table;
	table.try_assign(SlotKind::Screen, "1");
	const auto scope = watch_scope_of(ReceiveScope::Everyone, table, true);
	CHECK_FALSE(scope.slots_only);
	CHECK(scope.user_ids.empty());
}

TEST_CASE("枠に載せた人だけのときは、画面の枠に載っている人だけを、userId の昇順に重複なく入れる")
{
	AllocationTable table;
	CHECK(watch_scope_of(ReceiveScope::Slots, table, true).slots_only);
	CHECK(watch_scope_of(ReceiveScope::Slots, table, true).user_ids.empty()); // 誰も載せていなければ、誰も視聴しない

	const std::set<std::string> in_call = {"30", "10", "20", "99"};
	table.try_assign(SlotKind::Screen, "30");
	table.try_assign(SlotKind::Screen, "10");
	table.try_assign(SlotKind::Screen, "20");
	// 声の枠に載っている人は、視聴の範囲に関係しない。
	table.try_assign(SlotKind::Voice, "99");

	const auto scope = watch_scope_of(ReceiveScope::Slots, table, true);
	CHECK(scope.slots_only);
	CHECK(scope.user_ids == std::vector<std::string>{"10", "20", "30"});

	table.release(SlotKind::Screen, table.slot_of_owner(SlotKind::Screen, "20"));
	CHECK(watch_scope_of(ReceiveScope::Slots, table, true).user_ids == std::vector<std::string>{"10", "30"});
}

TEST_CASE("録画の入力: 全員のときはどのストリームも通し、枠に載せた人だけのときは、画面は枠に載っている人だけを通す")
{
	AllocationTable table;
	table.try_assign(SlotKind::Screen, "1");

	CHECK(record_stream_allowed(ReceiveScope::Everyone, table, /*screen=*/true, "2"));

	CHECK(record_stream_allowed(ReceiveScope::Slots, table, /*screen=*/true, "1"));
	CHECK_FALSE(record_stream_allowed(ReceiveScope::Slots, table, /*screen=*/true, "2"));
	// 声のストリームは、受け取る範囲が映像の範囲なので、枠に載っていなくても通す。
	CHECK(record_stream_allowed(ReceiveScope::Slots, table, /*screen=*/false, "2"));
}

TEST_CASE("cordmux を無効にしている間は、受け取る範囲にかかわらず、誰も視聴しない (空の範囲)")
{
	AllocationTable table;
	table.try_assign(SlotKind::Screen, "1");
	table.try_assign(SlotKind::Screen, "2");
	for (const ReceiveScope scope : {ReceiveScope::Everyone, ReceiveScope::Slots}) {
		const auto off = watch_scope_of(scope, table, /*feature_enabled=*/false);
		CHECK(off.slots_only);
		CHECK(off.user_ids.empty());
	}
	// 有効に戻すと、受け取る範囲どおりになる。
	CHECK_FALSE(watch_scope_of(ReceiveScope::Everyone, table, true).slots_only);
	CHECK(watch_scope_of(ReceiveScope::Slots, table, true).user_ids == std::vector<std::string>{"1", "2"});
}

TEST_CASE("全画面で映す人は、枠に載っていなくても視聴の範囲に入れ、その人の画面を録画の入力に通す")
{
	AllocationTable table;
	table.try_assign(SlotKind::Screen, "10");
	const auto scope = watch_scope_of(ReceiveScope::Slots, table, true, "50");
	CHECK(scope.slots_only);
	CHECK(scope.user_ids == std::vector<std::string>{"10", "50"});
	// 枠に載っている人を渡しても重ならない。
	CHECK(watch_scope_of(ReceiveScope::Slots, table, true, "10").user_ids == std::vector<std::string>{"10"});
	// 無効の間は、全画面で映す人も入れない。
	CHECK(watch_scope_of(ReceiveScope::Slots, table, false, "50").user_ids.empty());

	CHECK(record_stream_allowed(ReceiveScope::Slots, table, true, "50", "50"));
	CHECK_FALSE(record_stream_allowed(ReceiveScope::Slots, table, true, "60", "50"));
	CHECK(record_stream_allowed(ReceiveScope::Slots, table, true, "10", "50"));
}
