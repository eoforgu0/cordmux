// SPDX-License-Identifier: Apache-2.0

// pipeline/arrival_stats.h の単体テスト。画面のストリームの届くまでの遅れの分布 (分位点、範囲の入れ替わり、
// 上限を超えた分、D ごとの間に合わない率、間に合わなかったフレームのまとまりの数) を確認する。

#include "doctest.h"

#include "pipeline/arrival_stats.h"

#include <cstdint>
#include <vector>

using cordmux::pipeline::ArrivalDelayStats;
using cordmux::pipeline::ArrivalPeriod;
using cordmux::pipeline::ArrivalSummary;
using cordmux::pipeline::ArrivalWindow;
using cordmux::pipeline::kArrivalFrameCapacity;
using cordmux::pipeline::kArrivalMaxDelayMs;
using cordmux::pipeline::kArrivalWindowSeconds;

namespace {

constexpr std::int64_t kMs = 1000;

} // namespace

TEST_CASE("何も足していないときの要約は 0")
{
	ArrivalDelayStats stats;
	const ArrivalSummary s = stats.summary(100);
	CHECK(s.frames == 0);
	CHECK(s.window_seconds == 0);
	CHECK(s.p50_ms == 0.0);
	CHECK(s.max_ms == 0.0);
	CHECK(stats.late_fraction_at(100, 50) == 0.0);
}

TEST_CASE("分位点は 1 ms 刻みの度数の上端で、最大を超えない")
{
	ArrivalDelayStats stats;
	// 1 ms から 100 ms まで 1 つずつ (0.5 ms ずつずらして、度数の中ほどに入れる)。
	for (int ms = 1; ms <= 100; ++ms)
		stats.note(10, ms * kMs + 500, 1000 * kMs);
	const ArrivalSummary s = stats.summary(10);
	CHECK(s.frames == 100);
	CHECK(s.p50_ms == 51.0); // 50 番目は 50.5 ms で、度数 50 の上端
	CHECK(s.p95_ms == 96.0);
	CHECK(s.p99_ms == 100.0);
	CHECK(s.max_ms == doctest::Approx(100.5));
	// 1 つしかないときは、上端が最大を超えないよう最大で返る。
	ArrivalDelayStats one;
	one.note(10, 17300, 1000 * kMs);
	CHECK(one.summary(10).p50_ms == doctest::Approx(17.3));
}

TEST_CASE("負の遅れは 0 ms として数える")
{
	ArrivalDelayStats stats;
	stats.note(10, -5000, 100 * kMs);
	const ArrivalSummary s = stats.summary(10);
	CHECK(s.frames == 1);
	CHECK(s.max_ms == 0.0);
}

TEST_CASE("上限以上の遅れは 1 つの度数にまとまり、最大は実際の値で返る")
{
	ArrivalDelayStats stats;
	for (int i = 0; i < 98; ++i)
		stats.note(10, 20 * kMs, 100 * kMs);
	stats.note(10, 1500 * kMs, 100 * kMs);
	stats.note(10, 4000 * kMs, 100 * kMs);
	const ArrivalSummary s = stats.summary(10);
	CHECK(s.frames == 100);
	CHECK(s.p50_ms == 21.0);
	CHECK(s.p99_ms == doctest::Approx(4000.0)); // 99 番目は上限以上の度数で、最大で返る
	CHECK(s.max_ms == doctest::Approx(4000.0));
	// D の上限 (1000 ms) にしても、上限以上の 2 フレームは間に合わない。
	CHECK(stats.late_frames_at(10, kArrivalMaxDelayMs) == 2);
	// 範囲の外の D は端に収める。
	CHECK(stats.late_frames_at(10, 5000) == 2);
}

TEST_CASE("D を X にしたら間に合わないフレームの率は、遅れが X ms 以上のフレームの割合")
{
	ArrivalDelayStats stats;
	for (int i = 0; i < 90; ++i)
		stats.note(10, 20 * kMs, 100 * kMs);
	for (int i = 0; i < 5; ++i)
		stats.note(10, 120 * kMs, 100 * kMs);
	for (int i = 0; i < 5; ++i)
		stats.note(10, 220 * kMs, 100 * kMs);
	CHECK(stats.late_fraction_at(10, 10) == doctest::Approx(1.0));
	CHECK(stats.late_fraction_at(10, 21) == doctest::Approx(0.10));
	CHECK(stats.late_fraction_at(10, 120) == doctest::Approx(0.10)); // ちょうど 120 ms のフレームは間に合わない
	CHECK(stats.late_fraction_at(10, 121) == doctest::Approx(0.05));
	CHECK(stats.late_fraction_at(10, 221) == doctest::Approx(0.0));
	CHECK(stats.late_frames_at(10, 121) == 5);
	// D を 0 にすると全部が間に合わない。
	CHECK(stats.late_fraction_at(10, 0) == doctest::Approx(1.0));
}

TEST_CASE("範囲の外に出た秒の度数は引かれる")
{
	ArrivalDelayStats stats;
	for (int i = 0; i < 10; ++i)
		stats.note(100, 500 * kMs, 100 * kMs);
	for (int i = 0; i < 10; ++i)
		stats.note(130, 20 * kMs, 100 * kMs);

	// 秒 100 は、秒 159 までは範囲の中にいる (範囲は 60 秒)。
	ArrivalSummary s = stats.summary(159);
	CHECK(s.frames == 20);
	CHECK(s.max_ms == doctest::Approx(500.0));
	CHECK(s.window_seconds == kArrivalWindowSeconds);

	s = stats.summary(160);
	CHECK(s.frames == 10);
	CHECK(s.max_ms == doctest::Approx(20.0));
	CHECK(stats.late_fraction_at(160, 100) == doctest::Approx(0.0));

	// 全部が範囲を出たら空になる。
	s = stats.summary(1000);
	CHECK(s.frames == 0);
	// 空になった後に足し直せる。
	stats.note(1000, 30 * kMs, 100 * kMs);
	CHECK(stats.summary(1000).frames == 1);
}

TEST_CASE("起動直後の範囲の秒数は、溜めた長さ")
{
	ArrivalDelayStats stats;
	stats.note(50, 20 * kMs, 100 * kMs);
	CHECK(stats.summary(50).window_seconds == 1);
	CHECK(stats.summary(59).window_seconds == 10);
	stats.note(59, 20 * kMs, 100 * kMs);
	CHECK(stats.summary(109).window_seconds == kArrivalWindowSeconds);
}

TEST_CASE("時計が戻っても、数えた分を壊さない")
{
	ArrivalDelayStats stats;
	stats.note(100, 20 * kMs, 100 * kMs);
	stats.note(99, 30 * kMs, 100 * kMs);
	CHECK(stats.summary(100).frames == 2);
}

TEST_CASE("間に合わなかったフレームは遅れが D 以上のフレームで、続いたものは 1 つのまとまりと数える")
{
	ArrivalDelayStats stats;
	const std::int64_t d = 100 * kMs;
	// 間に合う 3、間に合わない 2 (まとまり 1)、間に合う 1、間に合わない 1 (まとまり 2)、間に合わない 3 続き (まとまり 2 のまま)。
	stats.note(10, 50 * kMs, d);
	stats.note(10, 60 * kMs, d);
	stats.note(10, 99 * kMs + 999, d); // D 未満
	stats.note(10, 100 * kMs, d);      // ちょうど D は間に合わない
	stats.note(10, 150 * kMs, d);
	stats.note(10, 40 * kMs, d);
	stats.note(10, 300 * kMs, d);
	stats.note(10, 310 * kMs, d);
	stats.note(10, 320 * kMs, d);
	stats.note(10, 320 * kMs, d);
	const ArrivalPeriod p = stats.take_period();
	CHECK(p.frames == 10);
	CHECK(p.late == 6);
	CHECK(p.runs == 2);
}

TEST_CASE("周期の数は取り出すと 0 に戻り、まとまりは周期をまたいでも 1 つと数える")
{
	ArrivalDelayStats stats;
	const std::int64_t d = 100 * kMs;
	stats.note(10, 200 * kMs, d);
	CHECK(stats.take_period().runs == 1);
	stats.note(11, 210 * kMs, d); // 前の周期から続いている
	ArrivalPeriod p = stats.take_period();
	CHECK(p.frames == 1);
	CHECK(p.late == 1);
	CHECK(p.runs == 0);
	stats.note(12, 10 * kMs, d);
	stats.note(12, 210 * kMs, d);
	p = stats.take_period();
	CHECK(p.runs == 1);
	CHECK(stats.take_period().frames == 0);
}

TEST_CASE("D を途中で変えると、変えた後のフレームから新しい D で間に合うかを決める")
{
	ArrivalDelayStats stats;
	stats.note(10, 150 * kMs, 100 * kMs); // 間に合わない
	stats.note(10, 150 * kMs, 200 * kMs); // 間に合う
	const ArrivalPeriod p = stats.take_period();
	CHECK(p.late == 1);
	// 度数は D に依らず、後から別の D で率を出せる。
	CHECK(stats.late_frames_at(10, 100) == 2);
	CHECK(stats.late_frames_at(10, 200) == 0);
}

TEST_CASE("範囲の遅れの列は届いた順で、平均と最大は実際の値")
{
	ArrivalDelayStats stats;
	stats.note(10, 20 * kMs + 900, 100 * kMs);
	stats.note(10, 5 * kMs, 100 * kMs);
	stats.note(11, 130 * kMs, 100 * kMs);
	stats.note(11, 1500 * kMs, 100 * kMs);
	const ArrivalWindow w = stats.window(11);
	REQUIRE(w.delays_ms.size() == 4);
	CHECK(w.delays_ms[0] == 20); // ms に切り捨てる
	CHECK(w.delays_ms[1] == 5);
	CHECK(w.delays_ms[2] == 130);
	CHECK(w.delays_ms[3] == 1500); // 上限で丸めない
	CHECK(w.max_ms == doctest::Approx(1500.0));
	CHECK(w.mean_ms == doctest::Approx((20.9 + 5.0 + 130.0 + 1500.0) / 4.0));
	// 何もないストリームは空の列
	ArrivalDelayStats none;
	const ArrivalWindow e = none.window(11);
	CHECK(e.delays_ms.empty());
	CHECK(e.mean_ms == 0.0);
	CHECK(e.max_ms == 0.0);
}

TEST_CASE("列は 60 秒の範囲に出入りし、度数の要約と同じフレームを持つ")
{
	ArrivalDelayStats stats;
	for (int i = 0; i < 10; ++i)
		stats.note(100, 500 * kMs, 100 * kMs);
	for (int i = 0; i < 7; ++i)
		stats.note(130, 20 * kMs, 100 * kMs);
	ArrivalWindow w = stats.window(159); // 秒 100 は 159 まで範囲の中
	CHECK(w.delays_ms.size() == 17);
	CHECK(stats.summary(159).frames == 17);
	w = stats.window(160);
	CHECK(w.delays_ms.size() == 7);
	CHECK(w.max_ms == doctest::Approx(20.0));
	CHECK(stats.summary(160).frames == 7);
	w = stats.window(1000);
	CHECK(w.delays_ms.empty());
	// 空になった後も足し直せる。
	stats.note(1000, 30 * kMs, 100 * kMs);
	CHECK(stats.window(1000).delays_ms.size() == 1);
}

TEST_CASE("列を読んでも、周期の数も度数も消えない")
{
	ArrivalDelayStats stats;
	const std::int64_t d = 100 * kMs;
	stats.note(10, 50 * kMs, d);
	stats.note(10, 150 * kMs, d);
	stats.note(10, 160 * kMs, d);
	const ArrivalSummary before = stats.summary(10);
	for (int i = 0; i < 3; ++i)
		(void)stats.window(10);
	const ArrivalSummary after = stats.summary(10);
	CHECK(after.frames == before.frames);
	CHECK(after.p50_ms == before.p50_ms);
	CHECK(after.p95_ms == before.p95_ms);
	CHECK(after.max_ms == before.max_ms);
	CHECK(stats.late_frames_at(10, 100) == 2);
	const ArrivalPeriod p = stats.take_period();
	CHECK(p.frames == 3);
	CHECK(p.late == 2);
	CHECK(p.runs == 1);
}

TEST_CASE("列が上限を超えたら古いフレームから外れ、時計が戻っても壊れない")
{
	ArrivalDelayStats stats;
	for (std::size_t i = 0; i < kArrivalFrameCapacity + 5; ++i)
		stats.note(10, static_cast<std::int64_t>(i % 100) * kMs, 100 * kMs);
	ArrivalWindow w = stats.window(10);
	CHECK(w.delays_ms.size() == kArrivalFrameCapacity);
	CHECK(w.delays_ms.front() == 5); // 先頭の 5 フレーム (0 から 4 ms) が外れた
	stats.note(9, 7 * kMs, 100 * kMs);
	CHECK(stats.window(10).delays_ms.size() == kArrivalFrameCapacity);
}
