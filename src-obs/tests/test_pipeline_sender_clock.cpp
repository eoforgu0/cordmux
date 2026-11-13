// SPDX-License-Identifier: Apache-2.0

// pipeline/sender_clock.h (SenderClockOffset と sender_to_obs_us) の単体テスト。送信側の時計と OBS の時計の差を、
// 届くまでの遅れを含まない値に近づけて決めることを確認する。

#include "doctest.h"

#include "pipeline/sender_clock.h"

#include <cstdint>
#include <optional>

using cordmux::pipeline::sender_to_obs_us;
using cordmux::pipeline::SenderClockOffset;

namespace {

// 本当の差 (OBS の時計 - 送信側の時計)。送信側の時計はエポックに近い値、OBS の時計は起動からの値。
constexpr std::int64_t kTrueOffset = 3'600'000'000 - 1'700'000'000'000'000;

// 送信側の時計で sender_us に読んで送ったメッセージが、delay_us 後に届いたときの OBS の時計。
std::int64_t received_at(std::int64_t sender_us, std::int64_t delay_us)
{
	return sender_us + kTrueOffset + delay_us;
}

} // namespace

TEST_CASE("最初の組で差が決まり、それより遅れて届いた組では差を変えない")
{
	SenderClockOffset clock;
	CHECK_FALSE(clock.offset_us().has_value());
	const std::int64_t s0 = 1'700'000'000'000'000;
	CHECK(clock.note(s0, received_at(s0, 800)));
	CHECK(clock.offset_us() == kTrueOffset + 800);
	CHECK_FALSE(clock.note(s0 + 5'000'000, received_at(s0 + 5'000'000, 30'000)));
	CHECK(clock.offset_us() == kTrueOffset + 800);
	CHECK(clock.last_excess_us() == 30'000 - 800);
	CHECK(clock.samples() == 2);
}

TEST_CASE("差は届くまでの遅れの最も小さかった組の値になり、下がる向きにだけ動く")
{
	SenderClockOffset clock;
	const std::int64_t s0 = 1'700'000'000'000'000;
	// hello は JavaScript が忙しい時に届いて 12 ms 遅れ、その後の pong は 0.3 ms から 5 ms で届く。
	const std::int64_t delays[] = {12'000, 4'000, 5'000, 300, 900, 2'000};
	std::int64_t s = s0;
	for (const std::int64_t delay : delays) {
		clock.note(s, received_at(s, delay));
		s += 5'000'000;
	}
	CHECK(clock.offset_us() == kTrueOffset + 300);
}

TEST_CASE("reset の後は差を持ち越さない")
{
	SenderClockOffset clock;
	clock.note(1000, 5000);
	clock.reset();
	CHECK_FALSE(clock.offset_us().has_value());
	CHECK(clock.samples() == 0);
	CHECK(clock.note(2000, 2100));
	CHECK(clock.offset_us() == 100);
}

TEST_CASE("10 時間続けても、どちらの時計も同じ単調な時計で進むので、差は最初の数回の後は動かない")
{
	// 5 秒ごとの pong を 10 時間分 (7200 回)。届くまでの遅れは 0.2 ms から 3 ms。
	SenderClockOffset clock;
	std::int64_t s = 1'700'000'000'000'000;
	std::uint64_t changes = 0;
	std::uint64_t seed = 7;
	for (int i = 0; i < 7200; ++i) {
		seed = seed * 6364136223846793005ull + 1442695040888963407ull;
		const std::int64_t delay = 200 + std::int64_t((seed >> 33) % 2800);
		if (clock.note(s, received_at(s, delay)))
			++changes;
		s += 5'000'000;
	}
	REQUIRE(clock.offset_us().has_value());
	CHECK(*clock.offset_us() - kTrueOffset >= 200);
	CHECK(*clock.offset_us() - kTrueOffset < 210);
	CHECK(changes < 20);
}

TEST_CASE("送信側の時計が OBS の時計と同じなら、組がなくても差は 0 で、最小の組は届くまでの遅れになる")
{
	SenderClockOffset clock;
	clock.use_shared_clock();
	CHECK(clock.shared_clock());
	CHECK(clock.offset_us() == std::optional<std::int64_t>(0));
	CHECK_FALSE(clock.min_sample_us().has_value());
	// 送信側の読み s を、delay 後に受け取る。差は 0 のまま、最小値だけが下がる。
	const std::int64_t s = 264'017'591'900;
	CHECK(clock.note(s, s + 900));
	CHECK(clock.note(s + 5'000'000, s + 5'000'000 + 250));
	CHECK_FALSE(clock.note(s + 10'000'000, s + 10'000'000 + 3'000));
	CHECK(clock.offset_us() == std::optional<std::int64_t>(0));
	CHECK(clock.min_sample_us() == std::optional<std::int64_t>(250));
	CHECK(clock.last_excess_us() == 3'000 - 250);
	CHECK(clock.samples() == 3);
	// 送信側の時計が先に進んでいる (前提が崩れた) と、最小値が負になる。差は 0 のまま。
	CHECK(clock.note(s + 15'000'000, s + 15'000'000 - 4'000));
	// 送信側の誤差の上限 (1000 + 100 + 1 us) の内なら、前提は崩れていない。
	CHECK(cordmux::pipeline::shared_clock_verdict(-1101, 1) == cordmux::pipeline::SharedClockVerdict::Holds);
	// 重い負荷で、1 分のどの組も数百 ms 遅れても、送信側が遅れているとはみなさない。
	CHECK(cordmux::pipeline::shared_clock_verdict(400'000, 12) == cordmux::pipeline::SharedClockVerdict::Holds);
	CHECK(*clock.min_sample_us() < -cordmux::pipeline::kSharedClockToleranceUs);
	CHECK(clock.offset_us() == std::optional<std::int64_t>(0));
}

TEST_CASE("同じ時計という前提の判定: 先に進むのはすぐ、遅れるのは組が 12 個そろってから")
{
	using cordmux::pipeline::kSharedClockBehindSamples;
	using cordmux::pipeline::kSharedClockBehindUs;
	using cordmux::pipeline::kSharedClockToleranceUs;
	using cordmux::pipeline::shared_clock_verdict;
	using cordmux::pipeline::SharedClockVerdict;
	CHECK(shared_clock_verdict(300, 1) == SharedClockVerdict::Holds);
	CHECK(shared_clock_verdict(-kSharedClockToleranceUs, 1) == SharedClockVerdict::Holds);
	CHECK(shared_clock_verdict(-kSharedClockToleranceUs - 1, 1) == SharedClockVerdict::SenderAhead);
	// 遅れの向きは、最初の数組 (hello が忙しい時に届いて遅れる) では決めない。
	CHECK(shared_clock_verdict(kSharedClockBehindUs + 1, kSharedClockBehindSamples - 1) == SharedClockVerdict::Holds);
	CHECK(shared_clock_verdict(kSharedClockBehindUs, kSharedClockBehindSamples) == SharedClockVerdict::Holds);
	CHECK(shared_clock_verdict(kSharedClockBehindUs + 1, kSharedClockBehindSamples) == SharedClockVerdict::SenderBehind);
}

TEST_CASE("同じ時計をやめると、差はこれまでの組の最小値になる")
{
	SenderClockOffset clock;
	clock.use_shared_clock();
	clock.note(1000, 9000);
	clock.note(2000, 9500);
	CHECK(clock.offset_us() == std::optional<std::int64_t>(0));
	clock.leave_shared_clock();
	CHECK_FALSE(clock.shared_clock());
	CHECK(clock.offset_us() == std::optional<std::int64_t>(7500));
}

TEST_CASE("reset で、同じ時計の扱いも解く")
{
	SenderClockOffset clock;
	clock.use_shared_clock();
	clock.note(1000, 1100);
	clock.reset();
	CHECK_FALSE(clock.shared_clock());
	CHECK_FALSE(clock.offset_us().has_value());
	CHECK(clock.note(2000, 2100));
	CHECK(clock.offset_us() == std::optional<std::int64_t>(100));
}

TEST_CASE("差が決まっていれば送信側のタイムスタンプに差を足し、決まっていなければ OBS の今を返す")
{
	CHECK(sender_to_obs_us(1'700'000'000'000'000, std::optional<std::int64_t>(kTrueOffset), 5) ==
	      1'700'000'000'000'000 + kTrueOffset);
	CHECK(sender_to_obs_us(1'700'000'000'000'000, std::nullopt, 5) == 5);
}
