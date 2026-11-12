// SPDX-License-Identifier: Apache-2.0

// pipeline/dummy_interval.h の単体テスト。ダミーを表示していた時間帯の書き留め方と上限を確認する。

#include "doctest.h"

#include "pipeline/dummy_interval.h"

#include <vector>

using cordmux::pipeline::dummy_interval_open;
using cordmux::pipeline::DummyInterval;
using cordmux::pipeline::note_dummy_interval;

TEST_CASE("ダミーの表示を始めると時間帯が開き、やめると閉じる")
{
	std::vector<DummyInterval> intervals;
	CHECK_FALSE(dummy_interval_open(intervals));

	CHECK(note_dummy_interval(&intervals, true, 100));
	REQUIRE(intervals.size() == 1);
	CHECK(intervals[0].started_at_us == 100);
	CHECK(intervals[0].open());
	CHECK(dummy_interval_open(intervals));

	CHECK(note_dummy_interval(&intervals, false, 500));
	REQUIRE(intervals.size() == 1);
	CHECK(intervals[0].ended_at_us == 500);
	CHECK_FALSE(intervals[0].open());
	CHECK_FALSE(dummy_interval_open(intervals));
}

TEST_CASE("同じ値を続けて渡しても時間帯は増えず、始めた時刻も終えた時刻も変わらない")
{
	std::vector<DummyInterval> intervals;
	CHECK(note_dummy_interval(&intervals, true, 100));
	CHECK_FALSE(note_dummy_interval(&intervals, true, 200));
	CHECK(intervals.size() == 1);
	CHECK(intervals[0].started_at_us == 100); // 始めた時刻は上書きしない

	CHECK(note_dummy_interval(&intervals, false, 300));
	CHECK_FALSE(note_dummy_interval(&intervals, false, 400));
	CHECK(intervals.size() == 1);
	CHECK(intervals[0].ended_at_us == 300);
}

TEST_CASE("やめた後にまた始めると、新しい時間帯が後ろに追加される")
{
	std::vector<DummyInterval> intervals;
	note_dummy_interval(&intervals, true, 10);
	note_dummy_interval(&intervals, false, 20);
	note_dummy_interval(&intervals, true, 30);
	REQUIRE(intervals.size() == 2);
	CHECK(intervals[1].started_at_us == 30);
	CHECK(intervals[1].open());
}

TEST_CASE("上限を超えたら古い時間帯から捨てる")
{
	std::vector<DummyInterval> intervals;
	for (int i = 1; i <= 5; ++i) {
		note_dummy_interval(&intervals, true, i * 100, 3);
		note_dummy_interval(&intervals, false, i * 100 + 50, 3);
	}
	REQUIRE(intervals.size() == 3);
	CHECK(intervals[0].started_at_us == 300); // 100 と 200 に始めた時間帯が捨てられた
	CHECK(intervals[2].started_at_us == 500);
}

TEST_CASE("上限が 0 なら何も書き留めない")
{
	std::vector<DummyInterval> intervals;
	CHECK_FALSE(note_dummy_interval(&intervals, true, 100, 0));
	CHECK(intervals.empty());
	// 時間帯が開いていないので、やめる側も何も書かない。
	CHECK_FALSE(note_dummy_interval(&intervals, false, 200, 0));
	CHECK(intervals.empty());
}

TEST_CASE("一覧に nullptr を渡すと何もせずに false を返す")
{
	CHECK_FALSE(note_dummy_interval(nullptr, true, 100));
}
