// SPDX-License-Identifier: Apache-2.0

// pipeline/latency_stats.h の単体テスト。受信側の遅れの足し上げと、平均と 95 パーセンタイルの取り出し、
// OBS が表示に選んだフレームの遅れ (shown_latency_us と ShownFrameWindow) を確認する。

#include "doctest.h"

#include "pipeline/latency_stats.h"

using cordmux::pipeline::LatencySummary;
using cordmux::pipeline::LatencyWindow;
using cordmux::pipeline::ShownFrameWindow;
using cordmux::pipeline::shown_latency_us;

TEST_CASE("何も足していなければ、数も値も 0")
{
	LatencyWindow w;
	const LatencySummary s = w.take();
	CHECK(s.count == 0);
	CHECK(s.avg_ms == doctest::Approx(0.0));
	CHECK(s.p95_ms == doctest::Approx(0.0));
	CHECK(s.max_ms == doctest::Approx(0.0));
}

TEST_CASE("平均は足した値の合計を数で割った値で、ms で返る")
{
	LatencyWindow w;
	w.add(10'000);
	w.add(20'000);
	w.add(60'000);
	const LatencySummary s = w.take();
	CHECK(s.count == 3);
	CHECK(s.avg_ms == doctest::Approx(30.0));
	CHECK(s.max_ms == doctest::Approx(60.0));
}

TEST_CASE("95 パーセンタイルは、1 ms 刻みのビンの上端で近似する")
{
	LatencyWindow w;
	// 1 ms から 100 ms を 1 つずつ。95 番目は 95 ms (ビンの上端で 96 ms)。
	for (int ms = 1; ms <= 100; ++ms)
		w.add(std::int64_t(ms) * 1000 + 100);
	const LatencySummary s = w.take();
	CHECK(s.count == 100);
	CHECK(s.p95_ms >= 95.0);
	CHECK(s.p95_ms <= 96.0 + 1e-9);
}

TEST_CASE("少数の外れ値は 95 パーセンタイルに出ず、平均には出る")
{
	LatencyWindow w;
	for (int i = 0; i < 99; ++i)
		w.add(5'000);
	w.add(500'000);
	const LatencySummary s = w.take();
	CHECK(s.p95_ms == doctest::Approx(6.0));
	CHECK(s.avg_ms == doctest::Approx((99 * 5.0 + 500.0) / 100.0));
	CHECK(s.max_ms == doctest::Approx(500.0));
}

TEST_CASE("95 パーセンタイルは区間の最大を超えない")
{
	LatencyWindow w;
	w.add(5'200); // ビンの上端は 6 ms だが、最大は 5.2 ms
	const LatencySummary s = w.take();
	CHECK(s.p95_ms == doctest::Approx(5.2));
}

TEST_CASE("take のたびに空に戻り、次の区間は前の区間を含まない")
{
	LatencyWindow w;
	w.add(100'000);
	w.add(100'000);
	CHECK(w.count() == 2);
	const LatencySummary first = w.take();
	CHECK(first.count == 2);
	CHECK(w.count() == 0);

	w.add(10'000);
	const LatencySummary second = w.take();
	CHECK(second.count == 1);
	CHECK(second.avg_ms == doctest::Approx(10.0));
	CHECK(second.p95_ms == doctest::Approx(10.0));
	CHECK(second.max_ms == doctest::Approx(10.0));
}

TEST_CASE("ビンの範囲を超えた値は最大の値で数え、落ちない")
{
	LatencyWindow w;
	w.add(5'000'000); // 5 秒
	w.add(3'000'000);
	const LatencySummary s = w.take();
	CHECK(s.count == 2);
	CHECK(s.p95_ms == doctest::Approx(5000.0));
	CHECK(s.avg_ms == doctest::Approx(4000.0));
}

TEST_CASE("負の値 (時計の食い違い) は、合計には実際の値を入れ、ビンには 0 ms として数える")
{
	LatencyWindow w;
	w.add(-4'000);
	w.add(8'000);
	const LatencySummary s = w.take();
	CHECK(s.count == 2);
	CHECK(s.avg_ms == doctest::Approx(2.0));
	CHECK(s.max_ms == doctest::Approx(8.0));
	CHECK(s.p95_ms >= 0.0);
	CHECK(s.p95_ms <= 8.0 + 1e-9);
}

TEST_CASE("表示の遅れは、渡したタイムスタンプから補正を引いて送信側のタイムスタンプに戻し、システム時刻との差を取る")
{
	// 送信側のタイムスタンプ 1000000 us に、整列の補正 3000 us と音のずれの補正 40000 us を足して渡した。
	const std::uint64_t timestamp_ns = (1'000'000ULL + 3'000ULL + 40'000ULL) * 1000ULL;
	CHECK(shown_latency_us(1'200'000, timestamp_ns, 43'000) == 200'000);
	// 補正を引かないと、足した分だけ小さく出る。
	CHECK(shown_latency_us(1'200'000, timestamp_ns, 0) == 200'000 - 43'000);
}

TEST_CASE("補正が負 (整列がタイムスタンプより前に置いた) でも、引いて戻る")
{
	const std::uint64_t timestamp_ns = (1'000'000ULL - 2'500ULL) * 1000ULL;
	CHECK(shown_latency_us(1'050'000, timestamp_ns, -2'500) == 50'000);
}

TEST_CASE("同じフレームを何回表示しても 1 回しか数えない")
{
	ShownFrameWindow w;
	const std::uint64_t ts = 5'000'000ULL * 1000ULL;
	CHECK(w.note(ts, 5'100'000, 0));
	// 60 fps の描画で 30 fps のフレームなら、同じフレームがもう 1 回選ばれる。
	CHECK_FALSE(w.note(ts, 5'116'667, 0));
	CHECK_FALSE(w.note(ts, 5'133'333, 0));
	const LatencySummary s = w.take();
	CHECK(s.count == 1);
	// 数えたのは最初の表示の 100 ms で、あとの表示の遅れは入らない。
	CHECK(s.avg_ms == doctest::Approx(100.0));
}

TEST_CASE("表示するフレームが替わるたびに数える")
{
	ShownFrameWindow w;
	CHECK(w.note(1'000'000ULL * 1000ULL, 1'010'000, 0));
	CHECK(w.note(1'033'333ULL * 1000ULL, 1'050'000, 0));
	CHECK(w.note(1'066'667ULL * 1000ULL, 1'090'000, 0));
	const LatencySummary s = w.take();
	CHECK(s.count == 3);
	CHECK(s.avg_ms == doctest::Approx((10.0 + 16.667 + 23.333) / 3.0).epsilon(0.001));
}

TEST_CASE("取り出しをまたいでも、最後に数えたフレームは数え直さない")
{
	ShownFrameWindow w;
	const std::uint64_t ts = 7'000'000ULL * 1000ULL;
	CHECK(w.note(ts, 7'020'000, 0));
	CHECK(w.take().count == 1);
	CHECK_FALSE(w.note(ts, 7'036'000, 0));
	CHECK(w.take().count == 0);
	CHECK(w.note(ts + 33'333'000ULL, 7'060'000, 0));
	CHECK(w.take().count == 1);
}

TEST_CASE("補正を引いた値が数えられる")
{
	ShownFrameWindow w;
	// 渡したタイムスタンプは送信側のタイムスタンプより 30 ms 先 (補正 30'000 us)。システム時刻との差は実際の 80 ms になる。
	CHECK(w.note((2'000'000ULL + 30'000ULL) * 1000ULL, 2'080'000, 30'000));
	const LatencySummary s = w.take();
	CHECK(s.avg_ms == doctest::Approx(80.0));
}
