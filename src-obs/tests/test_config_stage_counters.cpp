// SPDX-License-Identifier: Apache-2.0

// config/stage_counters.h (処理の段階ごとの件数) のテスト。

#include "doctest.h"

#include "config/stage_counters.h"

using cordmux::config::MediaKind;
using cordmux::config::rate_between;
using cordmux::config::StageCounterAccumulator;
using cordmux::config::StageCounters;
using cordmux::config::StreamStageCounters;

TEST_CASE("累積は増えるだけで、snapshot は副作用のないコピーを返す")
{
	StageCounterAccumulator acc;
	acc.add_received(10);
	acc.add_decoded(9);
	acc.add_handed_to_obs(8);
	acc.add_dropped(1);

	StageCounters s1 = acc.snapshot();
	CHECK(s1.received == 10);
	CHECK(s1.decoded == 9);
	CHECK(s1.handed_to_obs == 8);
	CHECK(s1.dropped == 1);

	// もう一度読んでも同じ値 (snapshot 自体は状態を変えない)。
	StageCounters s2 = acc.snapshot();
	CHECK(s2.received == 10);

	acc.add_received(1);
	StageCounters s3 = acc.snapshot();
	CHECK(s3.received == 11);
	// 既に取った snapshot は影響を受けない (コピーである)。
	CHECK(s1.received == 10);
}

TEST_CASE("add_* は引数を省くと 1 件を足す")
{
	StageCounterAccumulator acc;
	acc.add_received();
	acc.add_received();
	acc.add_decoded();
	CHECK(acc.snapshot().received == 2);
	CHECK(acc.snapshot().decoded == 1);
	CHECK(acc.snapshot().handed_to_obs == 0);
	CHECK(acc.snapshot().dropped == 0);
}

TEST_CASE("stream_id ごとに別々に数え、合計も返す")
{
	StreamStageCounters counters;
	counters.stream(1, MediaKind::Video).add_received(100);
	counters.stream(1, MediaKind::Video).add_decoded(98);
	counters.stream(2, MediaKind::Video).add_received(50);
	counters.stream(2, MediaKind::Video).add_dropped(2);

	auto per_stream = counters.per_stream_snapshot(MediaKind::Video);
	REQUIRE(per_stream.size() == 2);
	CHECK(per_stream.at(1).received == 100);
	CHECK(per_stream.at(1).decoded == 98);
	CHECK(per_stream.at(2).received == 50);
	CHECK(per_stream.at(2).dropped == 2);

	StageCounters total = counters.total_snapshot(MediaKind::Video);
	CHECK(total.received == 150);
	CHECK(total.decoded == 98);
	CHECK(total.dropped == 2);
	CHECK(total.handed_to_obs == 0);
}

TEST_CASE("stream() はない stream_id の項目を件数 0 で新しく作る")
{
	StreamStageCounters counters;
	auto snap = counters.stream(42, MediaKind::Video).snapshot();
	CHECK(snap.received == 0);
	CHECK(counters.per_stream_snapshot(MediaKind::Video).size() == 1);
}

TEST_CASE("同じ stream_id でも、映像と音声は別々に数える")
{
	StreamStageCounters counters;
	// 画面共有の stream_id=1。映像は毎秒 30 前後、音声は毎秒 100 (10 ms ごと) 届く想定。
	counters.stream(1, MediaKind::Video).add_received(30);
	counters.stream(1, MediaKind::Video).add_decoded(30);
	counters.stream(1, MediaKind::Video).add_handed_to_obs(30);
	counters.stream(1, MediaKind::Audio).add_received(100);
	counters.stream(1, MediaKind::Audio).add_handed_to_obs(100);

	const auto video = counters.per_stream_snapshot(MediaKind::Video);
	const auto audio = counters.per_stream_snapshot(MediaKind::Audio);
	REQUIRE(video.count(1) == 1);
	REQUIRE(audio.count(1) == 1);
	CHECK(video.at(1).received == 30);
	CHECK(audio.at(1).received == 100); // 映像の受信数に音声の件数が混ざっていない

	CHECK(counters.total_snapshot(MediaKind::Video).received == 30);
	CHECK(counters.total_snapshot(MediaKind::Audio).received == 100);

	// 音声しかない stream_id (声) は、映像の側の一覧に出てこない。
	counters.stream(2, MediaKind::Audio).add_received(100);
	CHECK(counters.per_stream_snapshot(MediaKind::Video).count(2) == 0);
	CHECK(counters.per_stream_snapshot(MediaKind::Audio).count(2) == 1);
}

TEST_CASE("no_route は経路のないメッセージを stream_id ごとの表に追加せず、1 つの件数にまとめる")
{
	StreamStageCounters counters;
	counters.no_route(MediaKind::Video).add_received(5);
	counters.no_route(MediaKind::Video).add_dropped(5);
	counters.no_route(MediaKind::Audio).add_received(7);
	counters.no_route(MediaKind::Audio).add_dropped(7);

	// stream_id ごとの一覧は空のまま (壊れた送信側が送る番号で、表が大きくならない)。
	CHECK(counters.per_stream_snapshot(MediaKind::Video).empty());
	CHECK(counters.per_stream_snapshot(MediaKind::Audio).empty());

	CHECK(counters.no_route_snapshot(MediaKind::Video).dropped == 5);
	CHECK(counters.no_route_snapshot(MediaKind::Audio).dropped == 7);
	// 合計には入る (届いたメッセージを数え落とさない)。
	CHECK(counters.total_snapshot(MediaKind::Video).received == 5);
	CHECK(counters.total_snapshot(MediaKind::Audio).received == 7);
}

TEST_CASE("retain は configure から消えた stream_id の項目を削除するが、合計は減らない")
{
	StreamStageCounters counters;
	counters.stream(1, MediaKind::Video).add_received(30);
	counters.stream(2, MediaKind::Video).add_received(20);
	counters.stream(2, MediaKind::Audio).add_received(100);

	counters.retain({1});

	const auto video = counters.per_stream_snapshot(MediaKind::Video);
	CHECK(video.size() == 1);
	CHECK(video.count(1) == 1);
	// 音声の側の項目も、同じ stream_id ごと削除される。
	CHECK(counters.per_stream_snapshot(MediaKind::Audio).empty());

	// 削除した項目の値は合計に残る (10 秒ごとにログに書く値が巻き戻らない)。
	CHECK(counters.total_snapshot(MediaKind::Video).received == 50);
	CHECK(counters.total_snapshot(MediaKind::Audio).received == 100);

	// 同じ番号で作り直しても、合計は足し込まれ続ける。
	counters.stream(1, MediaKind::Video).add_received(5);
	CHECK(counters.total_snapshot(MediaKind::Video).received == 55);

	counters.retain({});
	CHECK(counters.per_stream_snapshot(MediaKind::Video).empty());
	CHECK(counters.total_snapshot(MediaKind::Video).received == 55);
}

TEST_CASE("rate_between は 1 秒で 60 フレームを受信したら 60 Hz を返す")
{
	StageCounters prev;
	StageCounters cur;
	cur.received = 60;
	cur.decoded = 58;
	cur.handed_to_obs = 58;
	cur.dropped = 2;

	auto rates = rate_between(prev, cur, 1000000); // 1,000,000 us = 1 秒
	CHECK(rates.received_hz == doctest::Approx(60.0));
	CHECK(rates.decoded_hz == doctest::Approx(58.0));
	CHECK(rates.handed_to_obs_hz == doctest::Approx(58.0));
	CHECK(rates.dropped_hz == doctest::Approx(2.0));
}

TEST_CASE("rate_between は経過時間が 0 以下なら、0 で割らずにすべての段階で 0 を返す")
{
	StageCounters prev;
	StageCounters cur;
	cur.received = 60;

	CHECK(rate_between(prev, cur, 0).received_hz == 0.0);
	CHECK(rate_between(prev, cur, -1000).received_hz == 0.0);
}

TEST_CASE("rate_between は件数が巻き戻った段階だけ 0 を返す")
{
	StageCounters prev;
	prev.received = 100;
	prev.decoded = 10;
	StageCounters cur;
	cur.received = 50; // 巻き戻った (誤り)
	cur.decoded = 20;  // こちらは正しく増えている

	auto rates = rate_between(prev, cur, 1000000);
	CHECK(rates.received_hz == 0.0);
	CHECK(rates.decoded_hz == doctest::Approx(10.0));
}

TEST_CASE("rate_between は経過時間が半分なら 2 倍の値を返す")
{
	StageCounters prev;
	StageCounters cur;
	cur.received = 30;

	auto rates = rate_between(prev, cur, 500000); // 0.5 秒
	CHECK(rates.received_hz == doctest::Approx(60.0));
}
