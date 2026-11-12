// SPDX-License-Identifier: Apache-2.0

// pipeline/log_ring.h の単体テスト。ログの行のリングバッファーの上限と捨て方、行の先頭に付ける時刻の形を
// 確認する。

#include "doctest.h"

#include "pipeline/log_ring.h"

#include <string>

using cordmux::pipeline::log_clock_prefix;
using cordmux::pipeline::LogRing;

// 時刻の形は OBS のログの行の先頭と同じで、test/tools/notice-count/notice_count.py の TIME_PREFIX_RE
// (^\d{2}:\d{2}:\d{2}\.\d{3}: ) で読めること。
TEST_CASE("行の先頭の時刻は、OBS のログと同じ 0 埋めの HH:MM:SS.mmm: の形になる")
{
	CHECK(log_clock_prefix(0, 0, 0, 0) == "00:00:00.000: ");
	CHECK(log_clock_prefix(9, 5, 7, 42) == "09:05:07.042: ");
	CHECK(log_clock_prefix(23, 59, 59, 999) == "23:59:59.999: ");
	// うるう秒 (tm_sec は 60 まで) はそのまま。範囲の外は端の値にそろえ、幅を変えない。
	CHECK(log_clock_prefix(23, 59, 60, 0) == "23:59:60.000: ");
	CHECK(log_clock_prefix(-1, 75, -3, 1500) == "00:59:00.999: ");
}

TEST_CASE("上限までは全部の行が古い順に残る")
{
	LogRing ring(3);
	CHECK(ring.capacity() == 3);
	CHECK(ring.empty());

	ring.push("1 行目");
	ring.push("2 行目");
	ring.push("3 行目");

	const auto lines = ring.snapshot();
	REQUIRE(lines.size() == 3);
	CHECK(lines[0] == "1 行目");
	CHECK(lines[2] == "3 行目");
	CHECK(ring.dropped() == 0);
}

TEST_CASE("上限を超えたら古い行から捨てて数え、新しい行は残す")
{
	LogRing ring(3);
	for (int i = 1; i <= 5; ++i)
		ring.push(std::to_string(i));

	const auto lines = ring.snapshot();
	REQUIRE(lines.size() == 3);
	CHECK(lines[0] == "3"); // 1 と 2 が捨てられた
	CHECK(lines[2] == "5");
	CHECK(ring.dropped() == 2);
	CHECK(ring.size() == 3);
}

TEST_CASE("上限が 0 なら 1 行も持たず、積んだ行を全部捨てた数に入れる")
{
	LogRing ring(0);
	ring.push("消える");
	ring.push("これも消える");
	CHECK(ring.size() == 0);
	CHECK(ring.snapshot().empty());
	CHECK(ring.dropped() == 2);
}

TEST_CASE("既定のコンストラクターは上限 0 で、行を 1 つも持たない")
{
	LogRing ring;
	CHECK(ring.capacity() == 0);
	ring.push("消える");
	CHECK(ring.snapshot().empty());
	CHECK(ring.dropped() == 1);
}

TEST_CASE("clear で行を捨てても、捨てた行の総数は減らない")
{
	LogRing ring(2);
	ring.push("1");
	ring.push("2");
	ring.push("3"); // ここで 1 行捨てられる
	CHECK(ring.dropped() == 1);

	ring.clear();
	CHECK(ring.empty());
	CHECK(ring.dropped() == 1);

	ring.push("4");
	CHECK(ring.snapshot().size() == 1);
	CHECK(ring.dropped() == 1);
}

TEST_CASE("前に取ったコピーは、後から積んだ行で変わらない")
{
	LogRing ring(2);
	ring.push("1");
	const auto first = ring.snapshot();
	ring.push("2");
	ring.push("3");
	CHECK(first.size() == 1);
	CHECK(first[0] == "1");
	CHECK(ring.snapshot().size() == 2);
}

TEST_CASE("同じ行を何度積んでも、行の数は上限を超えない")
{
	LogRing ring(200); // settings の既定と同じ行の数
	for (int i = 0; i < 1000; ++i)
		ring.push("同じ行");
	CHECK(ring.size() == 200);
	CHECK(ring.dropped() == 800);
}
