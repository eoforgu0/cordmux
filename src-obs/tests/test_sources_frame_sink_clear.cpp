// SPDX-License-Identifier: Apache-2.0

// sources/frame_sink_clear.h (合成のソースの子を空ける時機) のテスト。すぐ空ける要求も次の tick で空けること、保ってから
// 空ける予約は期限を過ぎた tick で空けること、すぐ空ける要求を後から来た保つ予約で遅らせないことを確認する。

#include "doctest.h"

#include "sources/frame_sink_clear.h"

#include <cstdint>

using cordmux::sources::frame_sink_clear_due;
using cordmux::sources::frame_sink_hold_deadline;
using cordmux::sources::kClearAtNextTick;
using cordmux::sources::kNoClear;

TEST_CASE("すぐ空ける要求は、次の tick で (時刻によらず) 空ける")
{
	CHECK(frame_sink_clear_due(kClearAtNextTick, false, 1));
	CHECK(frame_sink_clear_due(kClearAtNextTick, false, 5'000'000'000ULL));
}

TEST_CASE("予約がないときと、既に空いているときは空けない")
{
	CHECK_FALSE(frame_sink_clear_due(kNoClear, false, 5'000'000'000ULL));
	CHECK_FALSE(frame_sink_clear_due(kClearAtNextTick, true, 5'000'000'000ULL));
}

TEST_CASE("保ってから空ける予約は、期限を過ぎた tick で空ける")
{
	const std::uint64_t deadline = frame_sink_hold_deadline(kNoClear, 1'000'000'000ULL, 2'000'000'000ULL);
	CHECK(deadline == 3'000'000'000ULL);
	CHECK_FALSE(frame_sink_clear_due(deadline, false, 2'999'999'999ULL));
	CHECK(frame_sink_clear_due(deadline, false, 3'000'000'000ULL));
}

TEST_CASE("すぐ空ける要求の後に保つ予約が来ても、次の tick で空ける。保つ予約どうしは後から来たほうの期限にする")
{
	CHECK(frame_sink_hold_deadline(kClearAtNextTick, 1'000'000'000ULL, 2'000'000'000ULL) == kClearAtNextTick);
	CHECK(frame_sink_hold_deadline(3'000'000'000ULL, 2'000'000'000ULL, 2'000'000'000ULL) == 4'000'000'000ULL);
}

TEST_CASE("保つ予約の期限は、予約なしの値とすぐ空ける要求の値にならない")
{
	CHECK(frame_sink_hold_deadline(kNoClear, 0, 0) > kClearAtNextTick);
}
