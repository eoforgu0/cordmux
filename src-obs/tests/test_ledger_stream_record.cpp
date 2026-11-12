// SPDX-License-Identifier: Apache-2.0

// ledger/stream_record.h の単体テスト。配信の開始と終了のイベントで、cordmux の録画を始める、終える判断を確認する。

#include "doctest.h"

#include "ledger/stream_record.h"

using cordmux::ledger::record_elapsed_text;
using cordmux::ledger::record_follow_of;
using cordmux::ledger::RecordFollow;
using cordmux::ledger::StreamEvent;

TEST_CASE("設定がオフなら、配信の開始でも終了でも録画を変えない")
{
	CHECK(record_follow_of(false, StreamEvent::Started, false) == RecordFollow::None);
	CHECK(record_follow_of(false, StreamEvent::Started, true) == RecordFollow::None);
	CHECK(record_follow_of(false, StreamEvent::Stopped, false) == RecordFollow::None);
	CHECK(record_follow_of(false, StreamEvent::Stopped, true) == RecordFollow::None);
}

TEST_CASE("設定がオンで、配信が始まったときに録画していなければ録画を始める")
{
	CHECK(record_follow_of(true, StreamEvent::Started, false) == RecordFollow::Start);
}

TEST_CASE("設定がオンで、配信が始まったときにもう録画していれば何もしない")
{
	CHECK(record_follow_of(true, StreamEvent::Started, true) == RecordFollow::None);
}

TEST_CASE("設定がオンで、配信が終わったときに録画していれば、誰が始めた録画でも終える")
{
	CHECK(record_follow_of(true, StreamEvent::Stopped, true) == RecordFollow::Stop);
}

TEST_CASE("設定がオンで、配信が終わったときに録画していなければ何もしない")
{
	CHECK(record_follow_of(true, StreamEvent::Stopped, false) == RecordFollow::None);
}

TEST_CASE("録画の経過時間は 1 時間までは分と秒で、それより長ければ時間を付ける")
{
	CHECK(record_elapsed_text(0) == "00:00");
	CHECK(record_elapsed_text(754'000'000) == "12:34");
	CHECK(record_elapsed_text(3'599'999'999) == "59:59");
	CHECK(record_elapsed_text(3'600'000'000) == "1:00:00");
	CHECK(record_elapsed_text(3'754'000'000) == "1:02:34");
	CHECK(record_elapsed_text(36'000'000'000) == "10:00:00");
}

TEST_CASE("システム時刻が戻って経過時間が負になっても 0 として表示する")
{
	CHECK(record_elapsed_text(-5'000'000) == "00:00");
}
