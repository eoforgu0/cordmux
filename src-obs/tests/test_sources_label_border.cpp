// SPDX-License-Identifier: Apache-2.0

// sources/label_border.h (発言中の光を表示するタイミング) のテスト。

#include "doctest.h"

#include "sources/label_border.h"

#include <cstdint>

using cordmux::sources::BorderTiming;
using cordmux::sources::merge_speaking;
using cordmux::sources::speaking_level;
using cordmux::sources::speaking_visible_until;
using cordmux::sources::SpeakingTimes;

namespace {

constexpr std::uint64_t kMs = 1'000'000ULL;
constexpr std::uint64_t kT0 = 10'000 * kMs; // 時刻の起点 (0 は始まっていないことを示すので避ける)

} // namespace

TEST_CASE("既定のタイミングは、0.15 秒で表示し、声が止まってから 0.2 秒保ち、0.15 秒で消える")
{
	const BorderTiming timing;
	CHECK(timing.rise_ms == 150);
	CHECK(timing.hold_ms == 200);
	CHECK(timing.fade_ms == 150);
	const SpeakingTimes t{kT0, kT0 + 1'000 * kMs};
	CHECK(speaking_level(t, kT0 + 75 * kMs, timing) == doctest::Approx(0.5));
	CHECK(speaking_level(t, kT0 + 1'199 * kMs, timing) == doctest::Approx(1.0));
	CHECK(speaking_level(t, kT0 + 1'275 * kMs, timing) == doctest::Approx(0.5));
	CHECK(speaking_visible_until(t, timing) == kT0 + 1'350 * kMs);
}

TEST_CASE("枠線は 0.2 秒で表示され、声が止まってから 0.5 秒保ち、0.4 秒で消える")
{
	const BorderTiming timing{200, 500, 400};

	SUBCASE("始まっていなければ 0 で、始まる前 (声の遅延を足した時刻の前) も 0")
	{
		CHECK(speaking_level(SpeakingTimes{}, kT0, timing) == doctest::Approx(0.0));
		CHECK(speaking_level(SpeakingTimes{kT0, 0}, kT0 - 1, timing) == doctest::Approx(0.0));
	}

	SUBCASE("表示し始めは 0 で、0.1 秒で半分、0.2 秒で最大の明るさになる")
	{
		const SpeakingTimes t{kT0, 0};
		CHECK(speaking_level(t, kT0, timing) == doctest::Approx(0.0));
		CHECK(speaking_level(t, kT0 + 100 * kMs, timing) == doctest::Approx(0.5));
		CHECK(speaking_level(t, kT0 + 200 * kMs, timing) == doctest::Approx(1.0));
		CHECK(speaking_level(t, kT0 + 60'000 * kMs, timing) == doctest::Approx(1.0));
	}

	SUBCASE("止まってから 0.5 秒は保ち、その後 0.4 秒で 0 になる")
	{
		const SpeakingTimes t{kT0, kT0 + 1'000 * kMs};
		CHECK(speaking_level(t, kT0 + 1'000 * kMs, timing) == doctest::Approx(1.0));
		CHECK(speaking_level(t, kT0 + 1'499 * kMs, timing) == doctest::Approx(1.0));
		CHECK(speaking_level(t, kT0 + 1'700 * kMs, timing) == doctest::Approx(0.5));
		CHECK(speaking_level(t, kT0 + 1'900 * kMs, timing) == doctest::Approx(0.0));
		CHECK(speaking_visible_until(t, timing) == kT0 + 1'900 * kMs);
	}

	SUBCASE("最大の明るさになる前に止まったら、そのときの明るさのまま保ってから消える")
	{
		const SpeakingTimes t{kT0, kT0 + 100 * kMs};
		CHECK(speaking_level(t, kT0 + 300 * kMs, timing) == doctest::Approx(0.5));
		CHECK(speaking_level(t, kT0 + 800 * kMs, timing) == doctest::Approx(0.25));
	}

	SUBCASE("rise が 0 なら、次のフレームで最大の明るさにする")
	{
		BorderTiming instant = timing;
		instant.rise_ms = 0;
		CHECK(speaking_level(SpeakingTimes{kT0, 0}, kT0, instant) == doctest::Approx(1.0));
	}
}

TEST_CASE("merge_speaking は届いた始まりと止まりを重ね、消えかけの線を 0 に戻さずに現在の明るさから上げ直す")
{
	const BorderTiming timing{200, 500, 400};

	SUBCASE("初めての話し始め")
	{
		const SpeakingTimes t = merge_speaking(SpeakingTimes{}, kT0, 0, timing);
		CHECK(t.start_ns == kT0);
		CHECK(t.stop_ns == 0);
	}

	SUBCASE("止まりは、始まりを渡さなくても前の始まりを引き継ぐ")
	{
		const SpeakingTimes t = merge_speaking(SpeakingTimes{kT0, 0}, 0, kT0 + 2'000 * kMs, timing);
		CHECK(t.start_ns == kT0);
		CHECK(t.stop_ns == kT0 + 2'000 * kMs);
	}

	SUBCASE("保っている間に話し始めたら、最大の明るさのまま続く")
	{
		const SpeakingTimes stopped{kT0, kT0 + 1'000 * kMs};
		const SpeakingTimes t = merge_speaking(stopped, kT0 + 1'200 * kMs, 0, timing);
		CHECK(t.stop_ns == 0);
		CHECK(speaking_level(t, kT0 + 1'200 * kMs, timing) == doctest::Approx(1.0));
		CHECK(speaking_level(t, kT0 + 5'000 * kMs, timing) == doctest::Approx(1.0));
	}

	SUBCASE("消えかけの間に話し始めたら、その時刻の明るさから立ち上がりの速さで上がる (跳ねない)")
	{
		// 止まってから 0.6 秒は、消え始めて 0.1 秒で明るさ 0.75。そこから 0.05 秒で最大の明るさに戻る。
		const SpeakingTimes stopped{kT0, kT0 + 1'000 * kMs};
		const std::uint64_t again = kT0 + 1'600 * kMs;
		const double before = speaking_level(stopped, again, timing);
		CHECK(before == doctest::Approx(0.75));
		const SpeakingTimes t = merge_speaking(stopped, again, 0, timing);
		CHECK(t.stop_ns == 0);
		CHECK(speaking_level(t, again, timing) == doctest::Approx(before));
		CHECK(speaking_level(t, again + 25 * kMs, timing) == doctest::Approx(0.875));
		CHECK(speaking_level(t, again + 50 * kMs, timing) == doctest::Approx(1.0));
		// 1 ms ごとに見て、明るさは下がらず、1 ms で立ち上がりの速さ (1 / 200) より大きくは変わらない。
		double last = before;
		for (std::uint64_t ms = 1; ms <= 100; ++ms) {
			const double now = speaking_level(t, again + ms * kMs, timing);
			CHECK(now >= last - 1e-9);
			CHECK(now - last <= 1.0 / 200.0 + 1e-9);
			last = now;
		}
	}

	SUBCASE("話している最中に話し始めが重ねて届いても、明るさは変わらない")
	{
		const SpeakingTimes talking{kT0, 0};
		const SpeakingTimes rising = merge_speaking(talking, kT0 + 50 * kMs, 0, timing);
		CHECK(speaking_level(rising, kT0 + 50 * kMs, timing) == doctest::Approx(0.25));
		const SpeakingTimes full = merge_speaking(talking, kT0 + 900 * kMs, 0, timing);
		CHECK(speaking_level(full, kT0 + 900 * kMs, timing) == doctest::Approx(1.0));
	}

	SUBCASE("消えた後に話し始めたら、新しく表示し始める")
	{
		const SpeakingTimes stopped{kT0, kT0 + 1'000 * kMs};
		const SpeakingTimes t = merge_speaking(stopped, kT0 + 1'901 * kMs, 0, timing);
		CHECK(t.start_ns == kT0 + 1'901 * kMs);
	}

	SUBCASE("短い発言を細かく繰り返しても、消えてから表示し直すまでの 1 周期は 0.9 秒より短くならない")
	{
		// 0.1 秒話して 0.3 秒黙る、を繰り返す。保っている間に次が始まるので、線は消えない。
		SpeakingTimes t;
		std::uint64_t now = kT0;
		int off_count = 0;
		bool was_on = false;
		for (int i = 0; i < 20; ++i) {
			t = merge_speaking(t, now, 0, timing);
			t = merge_speaking(t, 0, now + 100 * kMs, timing);
			for (std::uint64_t s = now; s < now + 400 * kMs; s += 10 * kMs) {
				const bool on = speaking_level(t, s, timing) > 0.0;
				if (was_on && !on)
					++off_count;
				was_on = on;
			}
			now += 400 * kMs;
		}
		CHECK(off_count == 0);
	}
}
