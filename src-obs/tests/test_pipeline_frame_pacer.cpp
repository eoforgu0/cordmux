// SPDX-License-Identifier: Apache-2.0

// pipeline/frame_pacer.h の単体テスト。画面共有のフレームを「送信側のタイムスタンプ + 表示の緩衝 D」の時刻まで
// 保持する待ち行列の、渡す時刻、遅れて届いたフレーム、時刻が来たフレームが複数あるときの扱い、D の変更、時計の飛び、
// 上限、統計を確認する。

#include "doctest.h"

#include "pipeline/frame_pacer.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using cordmux::pipeline::frame_pacer_capacity;
using cordmux::pipeline::FramePacer;
using cordmux::pipeline::kFramePacerMaxAheadUs;
using cordmux::pipeline::FramePacerStats;
using cordmux::pipeline::PacedTake;

namespace {

// フレーム 1 つの代わりにする要素 (通し番号だけを持つ)。
struct Frame {
	int serial = 0;
};

constexpr std::size_t kBigBytes = 1000000000;

// 上限を気にしないテスト用の待ち行列。
FramePacer<Frame> make_pacer(std::int64_t delay_us)
{
	return FramePacer<Frame>(delay_us, 100, kBigBytes);
}

} // namespace

TEST_CASE("フレームは、生まれてからの時間が D になる時刻まで待ってから取り出せる")
{
	FramePacer<Frame> pacer = make_pacer(200000); // D = 200 ms
	// 受信側の今は 1000000。フレームは送信側で 50 ms 前に生まれたので、取り出せる時刻はその 150 ms 後。
	CHECK(pacer.push(1000000, 50000, 100, Frame{1}) == 0);
	CHECK(pacer.size() == 1);
	CHECK(pacer.next_release_us().value() == 1150000);

	// 取り出せる時刻の 1 マイクロ秒前には、まだ取り出せず、待ち行列も変わらない。
	const PacedTake<Frame> early = pacer.take(1149999);
	CHECK_FALSE(early.item.has_value());
	CHECK(early.skipped == 0);
	CHECK(pacer.size() == 1);

	const PacedTake<Frame> due = pacer.take(1150000);
	REQUIRE(due.item.has_value());
	CHECK(due.item->serial == 1);
	CHECK_FALSE(due.late);
	CHECK(due.skipped == 0);
	CHECK(pacer.empty());
	CHECK_FALSE(pacer.next_release_us().has_value());
}

TEST_CASE("D より前に生まれていたフレーム (遅れて届いたフレーム) は、積んだ時点で時刻を過ぎていて、すぐ取り出せる")
{
	FramePacer<Frame> pacer = make_pacer(100000);
	// 生まれてから 100 ms (D と同じ) と 250 ms (D より長い)。
	pacer.push(5000000, 100000, 100, Frame{1});
	CHECK(pacer.next_release_us().value() <= 5000000);

	const PacedTake<Frame> first = pacer.take(5000000);
	REQUIRE(first.item.has_value());
	CHECK(first.item->serial == 1);
	CHECK(first.late);

	pacer.push(5100000, 250000, 100, Frame{2});
	const PacedTake<Frame> second = pacer.take(5100000);
	REQUIRE(second.item.has_value());
	CHECK(second.item->serial == 2);
	CHECK(second.late);
}

TEST_CASE("時刻が来たフレームが複数あるときは、最も新しい 1 つだけを取り出し、古いフレームは捨てて数える")
{
	FramePacer<Frame> pacer = make_pacer(100000);
	// 1 つずつ 16.7 ms 離れて届き、取り出す側が 50 ms 遅れて 3 つ同時に時刻が来た。
	pacer.push(1000000, 40000, 100, Frame{1}); // 時刻 1060000
	pacer.push(1016700, 40000, 100, Frame{2}); // 時刻 1076700
	pacer.push(1033400, 40000, 100, Frame{3}); // 時刻 1093400
	pacer.push(1050100, 40000, 100, Frame{4}); // 時刻 1110100 (まだ来ない)

	const PacedTake<Frame> taken = pacer.take(1100000);
	REQUIRE(taken.item.has_value());
	CHECK(taken.item->serial == 3);
	CHECK(taken.skipped == 2);
	CHECK_FALSE(taken.late);
	// 時刻が来ていない新しいフレームは残る。
	CHECK(pacer.size() == 1);
	CHECK(pacer.next_release_us().value() == 1110100);
	const PacedTake<Frame> last = pacer.take(1110100);
	REQUIRE(last.item.has_value());
	CHECK(last.item->serial == 4);
}

TEST_CASE("古い時刻のフレームが、新しいフレームの後に渡されることはない")
{
	FramePacer<Frame> pacer = make_pacer(100000);
	// 時計の飛びなどで、後から積んだフレーム 2 のほうが先に時刻が来る並びになる。
	pacer.push(1000000, 0, 100, Frame{1});       // 時刻 1100000
	pacer.push(1010000, 500000, 100, Frame{2});  // 時刻 610000 (過去)
	CHECK(pacer.next_release_us().value() == 610000);

	const PacedTake<Frame> taken = pacer.take(1010000);
	REQUIRE(taken.item.has_value());
	CHECK(taken.item->serial == 2);
	// フレーム 1 は時刻が来ていないが、フレーム 2 より古いので捨てる。
	CHECK(taken.skipped == 1);
	CHECK(pacer.empty());
}

TEST_CASE("タイムスタンプが今より先にある (描く予定の時刻のまま送られた) フレームは、タイムスタンプ + D に渡す")
{
	FramePacer<Frame> pacer = make_pacer(100000);
	// 描く予定の時刻が 14 ms 先のフレームと、受信を始めた直後の 160 ms 先のフレーム。
	pacer.push(2000000, -14000, 100, Frame{1});
	CHECK(pacer.next_release_us().value() == 2000000 + 14000 + 100000);
	CHECK_FALSE(pacer.take(2113999).item.has_value());
	const PacedTake<Frame> first = pacer.take(2114000);
	REQUIRE(first.item.has_value());
	CHECK_FALSE(first.late);
	// 統計でも、遅れて届いたフレームとしては数えない (時刻が来るまで待って渡した)。
	const FramePacerStats stats = pacer.take_stats();
	CHECK(stats.late == 0);
	CHECK(stats.on_time == 1);
	CHECK(stats.max_ahead_us == 14000);
	CHECK(stats.ahead_capped == 0);
	pacer.push(3000000, -160000, 100, Frame{2});
	CHECK(pacer.next_release_us().value() == 3000000 + 160000 + 100000);
}

TEST_CASE("送信側の時計が大きく進んでいて age が負でも、待つ長さは積んだ時点から最長でも D + kFramePacerMaxAheadUs である")
{
	FramePacer<Frame> pacer = make_pacer(100000);
	// 送信側の時計が 5 秒進んでいる (age = -5 秒)。そのまま使うと 5.1 秒待つ。
	pacer.push(2000000, -5000000, 100, Frame{1});
	CHECK(pacer.next_release_us().value() == 2000000 + kFramePacerMaxAheadUs + 100000);
	CHECK_FALSE(pacer.take(2000000 + kFramePacerMaxAheadUs + 99999).item.has_value());
	const PacedTake<Frame> taken = pacer.take(2000000 + kFramePacerMaxAheadUs + 100000);
	REQUIRE(taken.item.has_value());
	CHECK_FALSE(taken.late);
	// 頭打ちにしたことと、先の長さの最大は統計に出る (受信側のログで確かめるため)。
	const FramePacerStats stats = pacer.take_stats();
	CHECK(stats.ahead_capped == 1);
	CHECK(stats.max_ahead_us == 5000000);
}

TEST_CASE("age が非常に大きい (受信側の時計が大きく進んだ) フレームは、すぐ渡す側に倒れる")
{
	FramePacer<Frame> pacer = make_pacer(100000);
	pacer.push(2000000, INT64_C(3600) * 1000000, 100, Frame{1}); // 生まれてから 1 時間
	const PacedTake<Frame> taken = pacer.take(2000000);
	REQUIRE(taken.item.has_value());
	CHECK(taken.late);
}

TEST_CASE("D を縮めると、積んであるフレームの時刻も決め直され、時刻が来たものがすぐ取り出せる")
{
	FramePacer<Frame> pacer = make_pacer(500000);
	pacer.push(1000000, 0, 100, Frame{1});     // D = 500 ms なら 1500000
	pacer.push(1016700, 0, 100, Frame{2});     // 1516700
	pacer.push(1033400, 0, 100, Frame{3});     // 1533400
	CHECK_FALSE(pacer.take(1040000).item.has_value());

	pacer.configure(100000, 100, kBigBytes); // D = 100 ms: 1100000、1116700、1133400
	CHECK(pacer.delay_us() == 100000);
	CHECK(pacer.next_release_us().value() == 1100000);
	const PacedTake<Frame> taken = pacer.take(1120000);
	REQUIRE(taken.item.has_value());
	CHECK(taken.item->serial == 2);
	CHECK(taken.skipped == 1);
	CHECK(pacer.size() == 1);
}

TEST_CASE("D を伸ばすと、積んであるフレームもその分だけ待つ")
{
	FramePacer<Frame> pacer = make_pacer(100000);
	pacer.push(1000000, 0, 100, Frame{1}); // 1100000
	pacer.configure(300000, 100, kBigBytes);
	CHECK_FALSE(pacer.take(1100000).item.has_value());
	CHECK(pacer.next_release_us().value() == 1300000);
	CHECK(pacer.take(1300000).item.has_value());
}

TEST_CASE("D を 0 にすると、積んであるフレームはすぐ取り出せ、最も新しい 1 つだけが渡る")
{
	FramePacer<Frame> pacer = make_pacer(300000);
	pacer.push(1000000, 20000, 100, Frame{1});
	pacer.push(1016700, 20000, 100, Frame{2});
	pacer.push(1033400, 20000, 100, Frame{3});
	pacer.configure(0, 100, kBigBytes);

	const PacedTake<Frame> taken = pacer.take(1040000);
	REQUIRE(taken.item.has_value());
	CHECK(taken.item->serial == 3);
	CHECK(taken.skipped == 2);
	CHECK(pacer.empty());
}

TEST_CASE("負の D は 0 として扱い、上限が 0 でも 1 として扱う")
{
	FramePacer<Frame> pacer(-5, 0, 0);
	CHECK(pacer.delay_us() == 0);
	// 上限が 1 なので、2 つ積むと古いほうが捨てられる。積んだフレーム自身は捨てない。
	CHECK(pacer.push(1000, 0, 1, Frame{1}) == 0);
	CHECK(pacer.push(2000, 0, 1, Frame{2}) == 1);
	CHECK(pacer.size() == 1);
	const PacedTake<Frame> taken = pacer.take(2000);
	REQUIRE(taken.item.has_value());
	CHECK(taken.item->serial == 2);
}

TEST_CASE("数の上限を超えたら、古いほうから捨てる")
{
	FramePacer<Frame> pacer(1000000, 3, kBigBytes);
	CHECK(pacer.push(1000000, 0, 100, Frame{1}) == 0);
	CHECK(pacer.push(1016700, 0, 100, Frame{2}) == 0);
	CHECK(pacer.push(1033400, 0, 100, Frame{3}) == 0);
	CHECK(pacer.push(1050100, 0, 100, Frame{4}) == 1);
	CHECK(pacer.push(1066800, 0, 100, Frame{5}) == 1);
	CHECK(pacer.size() == 3);
	// 残っているのは 3、4、5。D が過ぎた後に取り出すと、最も新しい 5 が渡る。
	const PacedTake<Frame> taken = pacer.take(3000000);
	REQUIRE(taken.item.has_value());
	CHECK(taken.item->serial == 5);
	CHECK(taken.skipped == 2);
}

TEST_CASE("バイト数の上限を超えたら、古いほうから捨てる")
{
	FramePacer<Frame> pacer(1000000, 100, 2500); // 1000 バイトのフレームを 2 つまで
	CHECK(pacer.push(1000000, 0, 1000, Frame{1}) == 0);
	CHECK(pacer.push(1016700, 0, 1000, Frame{2}) == 0);
	CHECK(pacer.bytes() == 2000);
	CHECK(pacer.push(1033400, 0, 1000, Frame{3}) == 1);
	CHECK(pacer.bytes() == 2000);
	CHECK(pacer.size() == 2);
	// 1 つで上限を超える大きなフレームでも、積んだフレーム自身は残る。
	CHECK(pacer.push(1050100, 0, 9000, Frame{4}) == 2);
	CHECK(pacer.size() == 1);
	CHECK(pacer.bytes() == 9000);
}

TEST_CASE("取り出したときと捨てたときにバイト数が減る")
{
	FramePacer<Frame> pacer = make_pacer(100000);
	pacer.push(1000000, 0, 300, Frame{1});
	pacer.push(1016700, 0, 500, Frame{2});
	CHECK(pacer.bytes() == 800);
	pacer.take(2000000);
	CHECK(pacer.bytes() == 0);
	pacer.push(3000000, 0, 700, Frame{3});
	CHECK(pacer.discard_all() == 1);
	CHECK(pacer.bytes() == 0);
}

TEST_CASE("統計は、時刻どおりに渡した数、遅れて届いてすぐ渡した数、捨てた数、待ち行列の長さの最大を数え、取り出すと 0 に戻る")
{
	FramePacer<Frame> pacer(100000, 4, kBigBytes);
	// 時刻どおり 1 つ。
	pacer.push(1000000, 20000, 100, Frame{1});
	pacer.take(1080000);
	// 遅れて届いて、すぐ渡したもの 1 つ。
	pacer.push(2000000, 200000, 100, Frame{2});
	pacer.take(2000000);
	// 同時に時刻が来た 3 つのうち、2 つを捨てて 1 つを渡す (時刻どおり)。
	pacer.push(3000000, 0, 100, Frame{3});
	pacer.push(3016700, 0, 100, Frame{4});
	pacer.push(3033400, 0, 100, Frame{5});
	pacer.take(3200000);
	// 上限 (4) を超えて 1 つ捨てる。
	for (int i = 0; i < 5; ++i)
		pacer.push(4000000 + i * 16700, 0, 100, Frame{10 + i});

	const FramePacerStats stats = pacer.take_stats();
	CHECK(stats.on_time == 2);
	CHECK(stats.late == 1);
	CHECK(stats.dropped == 3);
	CHECK(stats.max_queued == 4);

	// 取り出した後は 0 から数え直す。待ち行列の長さの最大は、現在の長さから数え直す。
	const FramePacerStats again = pacer.take_stats();
	CHECK(again.on_time == 0);
	CHECK(again.late == 0);
	CHECK(again.dropped == 0);
	CHECK(again.max_queued == 4);
}

TEST_CASE("すべて捨てると、捨てた数を返して統計の捨てた数に足す")
{
	FramePacer<Frame> pacer = make_pacer(100000);
	pacer.push(1000000, 0, 100, Frame{1});
	pacer.push(1016700, 0, 100, Frame{2});
	CHECK(pacer.discard_all() == 2);
	CHECK(pacer.empty());
	CHECK(pacer.discard_all() == 0);
	CHECK(pacer.take_stats().dropped == 2);
}

TEST_CASE("次の時刻は、積んである全フレームのうち最も早いもの")
{
	FramePacer<Frame> pacer = make_pacer(100000);
	CHECK_FALSE(pacer.next_release_us().has_value());
	pacer.push(1000000, 0, 100, Frame{1});   // 1100000
	pacer.push(1016700, 80000, 100, Frame{2}); // 1036700
	CHECK(pacer.next_release_us().value() == 1036700);
}

TEST_CASE("待ち行列の上限は、D の間に届くフレームの数に余裕を足した数になる")
{
	// 60 fps で D = 1000 ms なら 60 フレームと余裕の 6 フレーム。
	CHECK(frame_pacer_capacity(1000000, 16667, 6) == 66);
	// D が 0 なら余裕の分だけ。
	CHECK(frame_pacer_capacity(0, 16667, 6) == 6);
	// フレームの間隔が不明でも、余裕の分は持つ。
	CHECK(frame_pacer_capacity(500000, 0, 6) == 6);
	// 余裕も 0 のときは 1 以上にする。
	CHECK(frame_pacer_capacity(0, 16667, 0) == 1);
	// 割り切れないときは切り上げる (6 フレーム分の 100002 us を超えると 7)。
	CHECK(frame_pacer_capacity(100000, 16667, 0) == 6);
	CHECK(frame_pacer_capacity(100003, 16667, 0) == 7);
}

// ---------------------------------------------------------------------------
// 刻みの格子 (set_tick_grid)
// ---------------------------------------------------------------------------

namespace {

constexpr std::int64_t kTick = 16667; // 60 fps の刻みの間隔 (マイクロ秒)

// 擬似乱数 (固定の種で、毎回同じ列になる)。0 以上 bound 未満を返す。
struct Lcg {
	std::uint64_t state = 12345;
	std::int64_t next(std::int64_t bound)
	{
		state = state * 6364136223846793005ULL + 1442695040888963407ULL;
		return std::int64_t((state >> 33) % std::uint64_t(bound));
	}
};

// phase を通る格子で、at_us 以後の最初の刻み (ちょうど刻みの上ならその刻み)。
std::int64_t first_tick_at_or_after(std::int64_t phase_us, std::int64_t interval_us, std::int64_t at_us)
{
	const std::int64_t k = (at_us - phase_us) / interval_us;
	std::int64_t tick = phase_us + k * interval_us;
	while (tick < at_us)
		tick += interval_us;
	while (tick - interval_us >= at_us)
		tick -= interval_us;
	return tick;
}

struct ShownFrame {
	std::int64_t tick_us = 0; // 表示される刻み
	int serial = 0;
};

struct SimResult {
	std::vector<ShownFrame> shown;
	std::size_t skipped = 0;
};

// 送信側が frame_interval_us の等間隔で count フレームを作り (最初の時刻は first_sent_us)、受信側は 20 ms 後に積む。
// 取り出すスレッドは、次の時刻 (next_release_us) の後 0 以上 late_max_us 以下の遅れで起きて take する。表示される刻みは、
// 取り出した時刻以後の最初の刻みである。grid_interval_us が 0 以下なら、格子なしの待ち行列で試す (表示される刻みの
// 計算には frame_interval_us の格子を使う)。
SimResult simulate(std::int64_t grid_phase_us, std::int64_t grid_interval_us, std::int64_t frame_interval_us,
		   std::int64_t first_sent_us, int count, std::int64_t delay_us, std::int64_t late_max_us)
{
	constexpr std::int64_t kArrivalUs = 20000;
	FramePacer<Frame> pacer = make_pacer(delay_us);
	pacer.set_tick_grid(grid_phase_us, grid_interval_us);
	const std::int64_t shown_interval_us = grid_interval_us > 0 ? grid_interval_us : frame_interval_us;
	Lcg rng;
	SimResult result;
	std::int64_t late = late_max_us > 0 ? rng.next(late_max_us + 1) : 0;

	// until_us までに起きる分を取り出す。
	auto drain = [&](std::int64_t until_us) {
		for (;;) {
			const auto release = pacer.next_release_us();
			if (!release || *release + late > until_us)
				return;
			const std::int64_t now = *release + late;
			PacedTake<Frame> taken = pacer.take(now);
			late = late_max_us > 0 ? rng.next(late_max_us + 1) : 0;
			if (!taken.item)
				continue;
			result.skipped += taken.skipped;
			// 起きた時刻 now 以後の最初の刻みに表示される。
			result.shown.push_back(
				{first_tick_at_or_after(grid_phase_us, shown_interval_us, now), taken.item->serial});
		}
	};

	for (int n = 0; n < count; ++n) {
		const std::int64_t sent = first_sent_us + n * frame_interval_us;
		drain(sent + kArrivalUs);
		pacer.push(sent + kArrivalUs, kArrivalUs, 100, Frame{n});
	}
	drain(INT64_MAX / 2);
	return result;
}

} // namespace

TEST_CASE("格子があるときの渡す時刻は、表示の時刻 r 以後の最初の刻みの半刻み前")
{
	FramePacer<Frame> pacer = make_pacer(100000);
	pacer.set_tick_grid(5000, 16000); // 刻み: ... 5000, 21000, 37000, ...
	// r = 1000000 + 100000 = 1100000。(1100000 - 5000) / 16000 = 68.4 なので次の刻みは 5000 + 69 * 16000 = 1109000。
	pacer.push(1000000, 0, 100, Frame{1});
	CHECK(pacer.next_release_us().value() == 1109000 - 8000);
}

TEST_CASE("r がちょうど刻みの上のとき、その刻みの半刻み前に渡す")
{
	FramePacer<Frame> pacer = make_pacer(100000);
	pacer.set_tick_grid(5000, 16000);
	// r = 5000 + 68 * 16000 = 1093000 になるタイムスタンプ。
	pacer.push(993000, 0, 100, Frame{1});
	CHECK(pacer.next_release_us().value() == 1093000 - 8000);
	CHECK_FALSE(pacer.take(1084999).item.has_value());
	CHECK(pacer.take(1085000).item.has_value());
}

TEST_CASE("r が刻みの 1 us 後のときは、次の刻みを選ぶ")
{
	FramePacer<Frame> pacer = make_pacer(100000);
	pacer.set_tick_grid(5000, 16000);
	pacer.push(993001, 0, 100, Frame{1}); // r = 1093001
	CHECK(pacer.next_release_us().value() == 1109000 - 8000);
}

TEST_CASE("格子の時刻が r より後でも、r 以後の最初の刻みを選ぶ")
{
	FramePacer<Frame> pacer = make_pacer(0);
	pacer.set_tick_grid(1000000, 16000);
	// 刻み: 1000000 - 62 * 16000 = 8000、1000000 - 63 * 16000 = -8000。r = 100 以後の最初は 8000。
	pacer.push(100, 0, 100, Frame{1});
	CHECK(pacer.next_release_us().value() == 8000 - 8000);
	// r がちょうど刻みの上 (格子の時刻より前の側の境界)。
	pacer.take(INT64_MAX / 2);
	pacer.push(8000, 0, 100, Frame{2});
	CHECK(pacer.next_release_us().value() == 8000 - 8000);
}

TEST_CASE("格子の間隔が 0 以下なら格子はなく、渡す時刻は r のまま")
{
	FramePacer<Frame> pacer = make_pacer(100000);
	pacer.set_tick_grid(5000, 0);
	pacer.push(1000000, 0, 100, Frame{1});
	CHECK(pacer.tick_interval_us() == 0);
	CHECK(pacer.next_release_us().value() == 1100000);
	pacer.set_tick_grid(5000, -16000);
	CHECK(pacer.tick_interval_us() == 0);
	CHECK(pacer.next_release_us().value() == 1100000);
}

TEST_CASE("格子を途中で変えると、積んであるフレームの渡す時刻も新しい格子で決め直される")
{
	FramePacer<Frame> pacer = make_pacer(100000);
	pacer.push(1000000, 0, 100, Frame{1});
	CHECK(pacer.next_release_us().value() == 1100000);
	pacer.set_tick_grid(5000, 16000);
	CHECK(pacer.next_release_us().value() == 1109000 - 8000);
	pacer.set_tick_grid(0, 33333); // 30 fps の格子。r = 1100000 以後の最初は 33333 * 34 = 1133322
	CHECK(pacer.next_release_us().value() == 1133322 - 16666);
	pacer.set_tick_grid(0, 0);
	CHECK(pacer.next_release_us().value() == 1100000);
}

TEST_CASE("60 fps のフレームを 60 fps の格子に、起きる時刻を半刻み未満揺らしても、各刻みにちょうど 1 フレームずつ渡る")
{
	// 送信側の位相 (表示の時刻 r から格子の刻みまでの距離) を、刻みのすぐ手前、ちょうど上、すぐ後ろ、中ほどなどに振る。
	// 起きる時刻の遅れは 0 から (半刻み - 1 ms) まで。
	const std::int64_t offsets[] = {0, 1, 500, 1000, 4000, 8333, 8334, 12000, 15000, 16000, 16666};
	const std::int64_t delay = 100000;
	for (const std::int64_t offset : offsets) {
		CAPTURE(offset);
		const SimResult result = simulate(0, kTick, kTick, offset, 600, delay, kTick / 2 - 1000);
		REQUIRE(result.shown.size() == 600);
		CHECK(result.skipped == 0);
		for (std::size_t i = 0; i < result.shown.size(); ++i) {
			// 各フレームは、自分の r 以後の最初の刻みに出る。
			const std::int64_t r = offset + std::int64_t(i) * kTick + delay;
			CHECK(result.shown[i].serial == int(i));
			CHECK(result.shown[i].tick_us == first_tick_at_or_after(0, kTick, r));
			if (i > 0)
				CHECK(result.shown[i].tick_us - result.shown[i - 1].tick_us == kTick);
		}
	}
}

TEST_CASE("格子がなく、渡す時刻が刻みのすぐ手前にあると、起きる時刻の揺れで刻みが空いたり重なったりする (格子で直る不具合の再現)")
{
	// r が刻みの 1 ms 手前。起きる時刻の遅れが 1 ms を超えるフレームは次の刻みに回り、同じ刻みに 2 フレームが入って
	// (古いほうは表示されない)、その前の刻みは空く (前のフレームがもう 1 回出る)。
	const std::int64_t delay = 100000;
	const std::int64_t first_sent = kTick - 1000 - delay % kTick;
	const SimResult broken = simulate(0, 0, kTick, first_sent, 600, delay, 3000);
	std::size_t gaps = 0;
	std::size_t doubles = 0;
	for (std::size_t i = 1; i < broken.shown.size(); ++i) {
		const std::int64_t step = broken.shown[i].tick_us - broken.shown[i - 1].tick_us;
		if (step > kTick)
			++gaps;
		else if (step == 0)
			++doubles;
	}
	CHECK(gaps > 0);
	CHECK(doubles > 0);

	// 同じ送信側と同じ揺れでも、格子に合わせれば乱れない。
	const SimResult fixed = simulate(0, kTick, kTick, first_sent, 600, delay, 3000);
	REQUIRE(fixed.shown.size() == 600);
	CHECK(fixed.skipped == 0);
	for (std::size_t i = 1; i < fixed.shown.size(); ++i)
		CHECK(fixed.shown[i].tick_us - fixed.shown[i - 1].tick_us == kTick);
}

TEST_CASE("60 fps のフレームを 30 fps の格子に渡すと、2 フレームに 1 つが渡り、1 つが捨てられる")
{
	const std::int64_t grid = 2 * kTick;
	const std::int64_t offsets[] = {0, 1000, 8333, 16666, 20000};
	for (const std::int64_t offset : offsets) {
		CAPTURE(offset);
		const SimResult result = simulate(0, grid, kTick, offset, 600, 100000, kTick / 2 - 1000);
		// 各刻みにちょうど 1 フレーム (刻みの間隔が一定で、同じ刻みに 2 回出ない)。
		REQUIRE(result.shown.size() >= 299);
		REQUIRE(result.shown.size() <= 301);
		for (std::size_t i = 1; i < result.shown.size(); ++i)
			CHECK(result.shown[i].tick_us - result.shown[i - 1].tick_us == grid);
		// 渡した数 + 捨てた数 = 積んだ数。捨てたのは約半分。
		CHECK(result.shown.size() + result.skipped == 600);
		CHECK(result.skipped >= 299);
		CHECK(result.skipped <= 301);
		// 同じ刻みに入るフレームのうち、新しいほうが渡る。
		for (std::size_t i = 1; i < result.shown.size(); ++i)
			CHECK(result.shown[i].serial > result.shown[i - 1].serial);
	}
}

TEST_CASE("格子がないときは今までの渡し方のまま (D の時刻に渡し、古いフレームは捨てる)")
{
	FramePacer<Frame> pacer = make_pacer(100000);
	pacer.push(1000000, 0, 100, Frame{1});
	pacer.push(1016667, 0, 100, Frame{2});
	CHECK(pacer.next_release_us().value() == 1100000);
	CHECK_FALSE(pacer.take(1099999).item.has_value());
	PacedTake<Frame> taken = pacer.take(1116667);
	REQUIRE(taken.item.has_value());
	CHECK(taken.item->serial == 2);
	CHECK(taken.skipped == 1);
}

TEST_CASE("遅れて届いたかどうかは格子に依らず、刻みに合わせる前の r で決める")
{
	FramePacer<Frame> pacer = make_pacer(100000);
	pacer.set_tick_grid(0, 16667);
	// r = 1100000 は、まだ来ていない (now = 1099000)。半刻み前に渡す時刻は過ぎていても、遅れて届いたのではない。
	pacer.push(1099000, 99000, 100, Frame{1});
	PacedTake<Frame> taken = pacer.take(1099000);
	REQUIRE(taken.item.has_value());
	CHECK_FALSE(taken.late);
	CHECK(pacer.take_stats().on_time == 1);
}

// ---------------------------------------------------------------------------
// 上限を超えたときに早く渡す (release_early_on_overflow)
// ---------------------------------------------------------------------------

namespace {

// pipeline.cpp の configure_frame_pacer と同じ設定の値 (60 fps とみなすフレームの間隔、余裕の 6 フレーム、
// 1 ストリームあたり 256 MiB)。
constexpr std::int64_t kPipelineIntervalUs = 16667;
constexpr std::size_t kPipelineHeadroomFrames = 6;
constexpr std::size_t kPipelineMaxBytes = 256ull * 1024 * 1024;

// デコードしたフレーム 1 つの大きさ (I420 か NV12 で 1 ピクセル 1.5 バイト)。
constexpr std::size_t kBytes4k = 3840 * 2160 * 3 / 2;    // 12441600。256 MiB に 21 フレーム
constexpr std::size_t kBytes1440p = 2560 * 1440 * 3 / 2; // 5529600。48 フレーム
constexpr std::size_t kBytes1080p = 1920 * 1080 * 3 / 2; // 3110400。86 フレーム

// pipeline.cpp の configure_frame_pacer と同じ形で、D に合わせて上限を設定する。release_early が false なら、
// 直す前の pipeline.cpp と同じ (上限を超えたら捨てる)。
void configure_like_pipeline(FramePacer<Frame> &pacer, std::int64_t delay_us, bool release_early)
{
	pacer.configure(delay_us,
			frame_pacer_capacity(delay_us + kFramePacerMaxAheadUs, kPipelineIntervalUs, kPipelineHeadroomFrames),
			kPipelineMaxBytes, release_early);
}

FramePacer<Frame> make_pipeline_pacer(std::int64_t delay_us, bool release_early)
{
	FramePacer<Frame> pacer;
	configure_like_pipeline(pacer, delay_us, release_early);
	return pacer;
}

// 積むフレーム 1 つ。lead_us はタイムスタンプが積んだ時点の今より先にある長さ (age_us = -lead_us)。
struct LeadFrame {
	std::int64_t lead_us = 0;
	std::size_t bytes = 0;
};

struct LeadSimResult {
	std::size_t dropped_on_push = 0;   // push が捨てた数
	std::size_t skipped = 0;           // take が、新しいフレームの時刻が来て渡さずに捨てた数
	std::size_t max_bytes = 0;         // 待ち行列のバイト数の最大
	std::vector<int> shown;            // 渡したフレームの通し番号 (渡した順)
	std::vector<std::int64_t> wait_us; // 渡したフレームの、積んでから渡すまでの長さ (shown と同じ順)
	std::vector<std::int64_t> at_us;   // 渡した時刻 (shown と同じ順)
	std::vector<std::int64_t> tick_us; // 表示される刻み (格子がなければ渡した時刻。shown と同じ順)
};

// 待ち行列の模型の条件。
struct SimOptions {
	std::int64_t grid_phase_us = 0;    // 刻みの格子 (set_tick_grid)。間隔が 0 なら格子なし
	std::int64_t grid_interval_us = 0;
	std::int64_t take_lag_max_us = 0; // 取り出すスレッドが時刻から遅れて起きる長さの最大
};

// 時刻 release_us に起きるはずの取り出しの遅れ (0 以上 max_us 以下)。同じ時刻には同じ遅れを返すので、比べる 2 つの
// 待ち行列で、同じ時刻の取り出しは同じだけ遅れる。
std::int64_t take_lag_us(std::int64_t release_us, std::int64_t max_us)
{
	if (max_us <= 0)
		return 0;
	const std::uint64_t mixed = std::uint64_t(release_us) * 0x9E3779B97F4A7C15ULL;
	return std::int64_t((mixed >> 33) % std::uint64_t(max_us + 1));
}

// 60 fps (kTick ごと) でフレームを積み、取り出すスレッドは次の時刻から take_lag_us だけ遅れて起きて take する
// (時刻がもう過ぎていれば、その今に take する)。before_push(n, pacer) は n 番目のフレームを積む直前に呼ぶ (D の変更
// などを差し込む)。
template <typename BeforePush>
LeadSimResult simulate_leads(FramePacer<Frame> &pacer, const std::vector<LeadFrame> &frames, BeforePush before_push,
			     const SimOptions &options = SimOptions{})
{
	constexpr std::int64_t kStartUs = 1000000;
	LeadSimResult result;
	std::vector<std::int64_t> pushed_at(frames.size(), 0);
	std::int64_t clock_us = kStartUs;
	if (options.grid_interval_us > 0)
		pacer.set_tick_grid(options.grid_phase_us, options.grid_interval_us);
	auto drain = [&](std::int64_t until_us) {
		for (;;) {
			const auto release = pacer.next_release_us();
			if (!release)
				return;
			const std::int64_t wake_us = *release + take_lag_us(*release, options.take_lag_max_us);
			if (wake_us > until_us)
				return;
			if (wake_us > clock_us)
				clock_us = wake_us;
			PacedTake<Frame> taken = pacer.take(clock_us);
			if (!taken.item)
				continue;
			result.skipped += taken.skipped;
			result.shown.push_back(taken.item->serial);
			result.wait_us.push_back(clock_us - pushed_at[std::size_t(taken.item->serial)]);
			result.at_us.push_back(clock_us);
			result.tick_us.push_back(options.grid_interval_us > 0
							 ? first_tick_at_or_after(options.grid_phase_us,
										  options.grid_interval_us, clock_us)
							 : clock_us);
		}
	};
	for (std::size_t n = 0; n < frames.size(); ++n) {
		const std::int64_t now = kStartUs + std::int64_t(n) * kTick;
		drain(now);
		clock_us = now;
		before_push(n, pacer);
		pushed_at[n] = now;
		result.dropped_on_push += pacer.push(now, -frames[n].lead_us, frames[n].bytes, Frame{int(n)});
		if (pacer.bytes() > result.max_bytes)
			result.max_bytes = pacer.bytes();
	}
	drain(INT64_MAX / 2);
	return result;
}

LeadSimResult simulate_leads(FramePacer<Frame> &pacer, const std::vector<LeadFrame> &frames,
			     const SimOptions &options = SimOptions{})
{
	return simulate_leads(pacer, frames, [](std::size_t, FramePacer<Frame> &) {}, options);
}

// 比べる試験で回す模型の条件: 遅れなし、刻みの格子、取り出しの遅れ (最大 5 ms。半刻み 8.3 ms より短い)、両方。
const SimOptions kSimVariants[] = {
	{0, 0, 0},
	{5000, 16667, 0},
	{0, 0, 5000},
	{5000, 16667, 5000},
};

std::vector<LeadFrame> same_frames(std::size_t count, std::int64_t lead_us, std::size_t bytes)
{
	return std::vector<LeadFrame>(count, LeadFrame{lead_us, bytes});
}

std::vector<LeadFrame> joined(std::vector<LeadFrame> first, const std::vector<LeadFrame> &second)
{
	first.insert(first.end(), second.begin(), second.end());
	return first;
}

// 渡したフレームの並びが逆にならないこと (通し番号が増え、渡した時刻が戻らない)。
void check_order(const LeadSimResult &result)
{
	for (std::size_t i = 1; i < result.shown.size(); ++i) {
		CHECK(result.shown[i] > result.shown[i - 1]);
		CHECK(result.at_us[i] >= result.at_us[i - 1]);
	}
}

} // namespace

TEST_CASE("直す前の動き: 4K のフレームで先の長さが大きいと、上限を超えて時刻の来ていないフレームばかりを捨てる")
{
	// 4K60 で D = 100 ms、400 ms 先のフレームが続く (受信を始めた直後)。待ち行列は約 30 フレーム、約 373 MB に
	// なろうとするが、256 MiB (21 フレーム) で古いほうから捨てる。どのフレームも時刻 (積んでから 500 ms) が来る前に
	// 押し出され、渡るのは最後に残った 21 フレームだけで、その間映像は止まる。
	FramePacer<Frame> pacer = make_pipeline_pacer(100000, false);
	const LeadSimResult result = simulate_leads(pacer, same_frames(300, 400000, kBytes4k));
	CHECK(result.dropped_on_push == 279);
	CHECK(result.shown.size() == 21);
}

TEST_CASE("4K のフレームで先の長さが大きくても、捨てずに古いほうから早く渡し、映像は途切れない")
{
	FramePacer<Frame> pacer = make_pipeline_pacer(100000, true);
	const LeadSimResult result = simulate_leads(pacer, same_frames(300, 400000, kBytes4k));
	CHECK(result.dropped_on_push == 0);
	CHECK(result.skipped == 0);
	REQUIRE(result.shown.size() == 300);
	check_order(result);
	// 待ち行列が持つのは上限 + 早く渡すフレーム 1 つまで。
	CHECK(result.max_bytes <= kPipelineMaxBytes + kBytes4k);
	// n 番目のフレームは、n + 21 番目を積んだとき (上限の 21 フレームを超えたとき) に早く渡す。最後の 21 フレームは
	// 後に積むフレームがないので、時刻 (積んでから 500 ms) に渡す。
	for (std::size_t i = 0; i < result.shown.size(); ++i) {
		CAPTURE(i);
		CHECK(result.shown[i] == int(i));
		CHECK(result.wait_us[i] == (i + 21 < 300 ? 21 * kTick : 500000));
	}
	const FramePacerStats stats = pacer.take_stats();
	CHECK(stats.early == 279);
	CHECK(stats.early_max_us == 500000 - 21 * kTick);
	CHECK(stats.on_time == 21);
	CHECK(stats.late == 0);
	CHECK(stats.dropped == 0);
	CHECK(stats.ahead_capped == 0);
}

TEST_CASE("上限に届かない場面では、渡す時刻は直す前と 1 us も変わらない")
{
	// 直す前 (上限を超えたら捨てる) の待ち行列が 1 つも捨てない場面。4K60 で D = 100 ms なら、先の長さ 250 ms までは
	// 待ち行列が 21 フレームに収まる。上限を決めない待ち行列とも比べる。
	struct Scene {
		std::int64_t delay_us;
		std::size_t bytes;
		std::int64_t lead_us;
	};
	const Scene scenes[] = {
		{100000, kBytes1080p, 0},       {100000, kBytes1080p, 14000},  {100000, kBytes1080p, 160000},
		{100000, kBytes1080p, 400000},  {100000, kBytes1080p, 499999}, {100000, kBytes1080p, 500000},
		{100000, kBytes1080p, 600000},  {1000000, kBytes1080p, 0},     {1000000, kBytes1080p, 300000},
		{100000, kBytes1440p, 500000},  {100000, kBytes4k, 0},         {100000, kBytes4k, 100000},
		{100000, kBytes4k, 150000},     {100000, kBytes4k, 200000},    {100000, kBytes4k, 250000},
		{0, kBytes4k, 340000},          {300000, kBytes4k, 0},
	};
	// 刻みの格子を通すときと、取り出しが遅れるときも比べる。取り出しが遅れると、待ち行列に残る長さが延びて、上限の
	// ちょうど手前の場面 (4K で先 250 ms) は直す前の形でも上限を超える。その場面は「上限に届かない場面」ではないので
	// 比べず (下の上限を超える場面の試験の主張が当てはまる)、数だけ確かめる。
	std::size_t compared = 0;
	std::size_t over_limit = 0;
	for (const SimOptions &options : kSimVariants) {
		CAPTURE(options.grid_interval_us);
		CAPTURE(options.take_lag_max_us);
		for (const Scene &scene : scenes) {
			CAPTURE(scene.delay_us);
			CAPTURE(scene.bytes);
			CAPTURE(scene.lead_us);
			FramePacer<Frame> fixed = make_pipeline_pacer(scene.delay_us, true);
			FramePacer<Frame> before = make_pipeline_pacer(scene.delay_us, false);
			FramePacer<Frame> unbounded(scene.delay_us, 1000, kBigBytes);
			const std::vector<LeadFrame> frames = same_frames(120, scene.lead_us, scene.bytes);
			const LeadSimResult a = simulate_leads(fixed, frames, options);
			const LeadSimResult b = simulate_leads(before, frames, options);
			const LeadSimResult c = simulate_leads(unbounded, frames, options);
			if (b.dropped_on_push > 0) {
				// 取り出しが遅れるときだけ起きる。
				CHECK(options.take_lag_max_us > 0);
				MESSAGE("取り出しの遅れで上限を超えた: D " << scene.delay_us << " 大きさ " << scene.bytes << " 先 "
									    << scene.lead_us << " 格子 " << options.grid_interval_us);
				++over_limit;
				continue;
			}
			++compared;
			CHECK(a.dropped_on_push == 0);
			CHECK(a.shown == b.shown);
			CHECK(a.at_us == b.at_us);
			CHECK(a.shown == c.shown);
			CHECK(a.at_us == c.at_us);
			CHECK(fixed.take_stats().early == 0);
		}
	}
	MESSAGE("上限に届かない場面: 比べた " << compared << " / 取り出しの遅れで上限を超えた " << over_limit);
	CHECK(compared + over_limit == 4 * (sizeof(scenes) / sizeof(scenes[0])));
	CHECK(over_limit <= 4);
}

TEST_CASE("上限を超える場面でも、直す前に渡ったフレームはすべて同じ時刻に渡り、ほかに早く渡すフレームが増えるだけ")
{
	struct Scene {
		std::string name;
		std::int64_t delay_us;
		std::vector<LeadFrame> frames;
		std::int64_t change_at; // この番号のフレームを積む前に D を change_to_us にする (負なら変えない)
		std::int64_t change_to_us;
	};
	const std::vector<Scene> scenes = {
		{"4K 先 260 ms", 100000, same_frames(300, 260000, kBytes4k), -1, 0},
		{"4K 先 500 ms", 100000, same_frames(300, 500000, kBytes4k), -1, 0},
		{"4K 時計の飛び 5 秒", 100000, same_frames(300, 5000000, kBytes4k), -1, 0},
		{"4K D 400 ms (D だけで上限を超える)", 400000, same_frames(300, 50000, kBytes4k), -1, 0},
		{"4K D 1000 ms 先 0", 1000000, same_frames(300, 0, kBytes4k), -1, 0},
		{"1080p D 1000 ms 先 500 ms", 1000000, same_frames(300, 500000, kBytes1080p), -1, 0},
		{"1080p から 4K", 100000, joined(same_frames(150, 400000, kBytes1080p), same_frames(150, 400000, kBytes4k)), -1, 0},
		{"1440p から 4K", 100000, joined(same_frames(150, 400000, kBytes1440p), same_frames(150, 400000, kBytes4k)), -1, 0},
		{"4K から 1080p", 100000, joined(same_frames(150, 400000, kBytes4k), same_frames(150, 400000, kBytes1080p)), -1, 0},
		{"4K 先 200 ms のまま D を 100 から 200 ms", 100000, same_frames(300, 200000, kBytes4k), 100, 200000},
		{"4K D を 100 から 400 ms", 100000, same_frames(300, 400000, kBytes4k), 100, 400000},
		{"4K D を 200 から 100 ms", 200000, same_frames(300, 400000, kBytes4k), 100, 100000},
		{"4K 先へ 300 ms 飛ぶ", 100000, joined(same_frames(100, 400000, kBytes4k), same_frames(100, 700000, kBytes4k)), -1, 0},
		{"4K 後ろへ 600 ms 飛ぶ", 100000, joined(same_frames(100, 400000, kBytes4k), same_frames(100, -200000, kBytes4k)), -1, 0},
	};
	// 刻みの格子を通すときと、取り出しが遅れるときも回す。取り出しが遅れるときは、早く渡すフレームのために起きた
	// 取り出しが、時刻の来た待つフレームを (遅れの範囲の内で) 先に渡すことがあるので、渡した時刻ではなく表示される
	// 刻みで比べる (遅れは半刻みより短いので、刻みは変わらない)。格子がなく遅れるときは表示される刻みがないので、
	// 渡した時刻が遅れの最大より大きくずれないことを確かめる。
	for (const SimOptions &options : kSimVariants) {
		CAPTURE(options.grid_interval_us);
		CAPTURE(options.take_lag_max_us);
		for (const Scene &scene : scenes) {
			CAPTURE(scene.name);
			auto change = [&](std::size_t n, FramePacer<Frame> &p, bool early) {
				if (scene.change_at >= 0 && n == std::size_t(scene.change_at))
					configure_like_pipeline(p, scene.change_to_us, early);
			};
			FramePacer<Frame> fixed = make_pipeline_pacer(scene.delay_us, true);
			FramePacer<Frame> before = make_pipeline_pacer(scene.delay_us, false);
			const LeadSimResult a = simulate_leads(
				fixed, scene.frames, [&](std::size_t n, FramePacer<Frame> &p) { change(n, p, true); }, options);
			const LeadSimResult b = simulate_leads(
				before, scene.frames, [&](std::size_t n, FramePacer<Frame> &p) { change(n, p, false); }, options);
			check_order(a);
			CHECK(a.shown.size() >= b.shown.size());
			CHECK(a.shown.size() + a.skipped + a.dropped_on_push == scene.frames.size());
			CHECK(a.max_bytes <= kPipelineMaxBytes + kBytes4k);
			// 直す前に渡ったフレームは、直した後も同じ時刻に渡る。
			std::size_t j = 0;
			for (std::size_t i = 0; i < b.shown.size(); ++i) {
				while (j < a.shown.size() && a.shown[j] < b.shown[i])
					++j;
				REQUIRE(j < a.shown.size());
				CHECK(a.shown[j] == b.shown[i]);
				if (options.take_lag_max_us == 0) {
					CHECK(a.at_us[j] == b.at_us[i]);
				} else if (options.grid_interval_us > 0) {
					CHECK(a.tick_us[j] == b.tick_us[i]);
				} else {
					CHECK(a.at_us[j] - b.at_us[i] <= options.take_lag_max_us);
					CHECK(b.at_us[i] - a.at_us[j] <= options.take_lag_max_us);
				}
			}
			MESSAGE(scene.name << " (格子 " << options.grid_interval_us << " 遅れ " << options.take_lag_max_us
					   << "): 直す前 渡す " << b.shown.size() << " 捨てる " << b.dropped_on_push << " 飛ばす "
					   << b.skipped << " / 直した後 渡す " << a.shown.size() << " 捨てる " << a.dropped_on_push
					   << " 飛ばす " << a.skipped);
		}
	}
}

TEST_CASE("D だけで上限を超えても (4K60 で D = 400 ms)、捨てずに早く渡し、映像は途切れない")
{
	// D の間に届く 24 フレームだけで 256 MiB (21 フレーム) を超える。直す前は、時刻の来ていないフレームばかりを
	// 捨てて映像が止まる。直した後は、どのフレームも積んでから 21 フレーム分で渡る (時刻より約 100 ms 早い)。
	FramePacer<Frame> pacer = make_pipeline_pacer(400000, true);
	const LeadSimResult result = simulate_leads(pacer, same_frames(300, 50000, kBytes4k));
	CHECK(result.dropped_on_push == 0);
	CHECK(result.skipped == 0);
	REQUIRE(result.shown.size() == 300);
	for (std::size_t i = 0; i + 21 < result.shown.size(); ++i)
		CHECK(result.wait_us[i] == 21 * kTick);
	CHECK(pacer.take_stats().early_max_us == 450000 - 21 * kTick);
}

TEST_CASE("数の上限を超えたら、捨てずに最も古いフレームを今の時刻で渡すフレームにする")
{
	FramePacer<Frame> pacer(1000000, 3, kBigBytes, true);
	CHECK(pacer.push(1000000, 0, 100, Frame{1}) == 0);
	CHECK(pacer.push(1016700, 0, 100, Frame{2}) == 0);
	CHECK(pacer.push(1033400, 0, 100, Frame{3}) == 0);
	CHECK(pacer.next_release_us().value() == 2000000);
	// 4 つ目で上限を超える。1 を今 (1050100) に渡すフレームにし、残りの 3 つは上限の内。
	CHECK(pacer.push(1050100, 0, 100, Frame{4}) == 0);
	CHECK(pacer.size() == 4);
	CHECK(pacer.bytes() == 400);
	CHECK(pacer.next_release_us().value() == 1050100);
	CHECK_FALSE(pacer.take(1050099).item.has_value());
	const PacedTake<Frame> taken = pacer.take(1050100);
	REQUIRE(taken.item.has_value());
	CHECK(taken.item->serial == 1);
	CHECK(taken.skipped == 0);
	CHECK_FALSE(taken.late);
	CHECK(pacer.next_release_us().value() == 2016700);
	const FramePacerStats stats = pacer.take_stats();
	CHECK(stats.early == 1);
	CHECK(stats.early_max_us == 2000000 - 1050100);
	CHECK(stats.on_time == 0);
	CHECK(stats.dropped == 0);
}

TEST_CASE("早く渡すフレームを取り出す前に次を早く渡すと決めたら、前のほうは渡らないので捨てて数える")
{
	FramePacer<Frame> pacer(1000000, 3, kBigBytes, true);
	pacer.push(1000000, 0, 100, Frame{1});
	pacer.push(1016700, 0, 100, Frame{2});
	pacer.push(1033400, 0, 100, Frame{3});
	CHECK(pacer.push(1050100, 0, 100, Frame{4}) == 0); // 1 を早く渡す
	// 取り出すスレッドが遅れていて、1 がまだ残っている。5 を積むと 2 を早く渡すことになり、1 は捨てる (取り出しても
	// 時刻の来た新しいほう (2) だけが渡るため)。
	CHECK(pacer.push(1066800, 0, 100, Frame{5}) == 1);
	CHECK(pacer.size() == 4); // 早く渡す 2 と、待つ 3、4、5
	CHECK(pacer.bytes() == 400);
	const PacedTake<Frame> taken = pacer.take(1066800);
	REQUIRE(taken.item.has_value());
	CHECK(taken.item->serial == 2);
	CHECK(taken.skipped == 0);
	const FramePacerStats stats = pacer.take_stats();
	CHECK(stats.early == 1);
	CHECK(stats.dropped == 1);
}

TEST_CASE("取り出すスレッドが止まっていても、待ち行列は上限 + 1 フレームを超えない")
{
	FramePacer<Frame> pacer(1000000, 3, 1000, true); // 100 バイトのフレームを 3 つ、バイト数は 10 フレーム分
	std::size_t dropped = 0;
	for (int n = 0; n < 100; ++n) {
		dropped += pacer.push(1000000 + n * 16667, 0, 100, Frame{n});
		CHECK(pacer.size() <= 4);
		CHECK(pacer.bytes() <= 400);
	}
	// 早く渡すと決めたのは 0 から 96 で、最後の 96 だけが残る。
	CHECK(dropped == 96);
	const PacedTake<Frame> taken = pacer.take(1000000 + 99 * 16667);
	REQUIRE(taken.item.has_value());
	CHECK(taken.item->serial == 96);
	CHECK(pacer.size() == 3);
}

TEST_CASE("バイト数の上限を超えたら、捨てずに古いほうから早く渡すフレームにする (1 回の push で 2 つ以上でも)")
{
	FramePacer<Frame> pacer(1000000, 100, 2500, true); // 1000 バイトのフレームを 2 つまで
	CHECK(pacer.push(1000000, 0, 1000, Frame{1}) == 0);
	CHECK(pacer.push(1016700, 0, 1000, Frame{2}) == 0);
	CHECK(pacer.push(1033400, 0, 1000, Frame{3}) == 0); // 1 を早く渡す
	CHECK(pacer.bytes() == 3000);
	CHECK(pacer.next_release_us().value() == 1033400);
	// 1 つで上限を超える大きなフレーム。2 と 3 を順に早く渡すことにし、前に決めた 1 と 2 は捨てる。積んだフレーム
	// 自身は残る。
	CHECK(pacer.push(1050100, 0, 9000, Frame{4}) == 2);
	CHECK(pacer.size() == 2);
	CHECK(pacer.bytes() == 10000);
	const PacedTake<Frame> taken = pacer.take(1050100);
	REQUIRE(taken.item.has_value());
	CHECK(taken.item->serial == 3);
	CHECK(taken.skipped == 0);
	CHECK(pacer.size() == 1);
}

TEST_CASE("上限が 0 でも 1 として扱い、早く渡す指定でも積んだフレーム自身は残る")
{
	FramePacer<Frame> pacer(-5, 0, 0, true);
	CHECK(pacer.delay_us() == 0);
	CHECK(pacer.push(1000, 0, 1, Frame{1}) == 0);
	CHECK(pacer.push(2000, 0, 1, Frame{2}) == 0); // 1 を早く渡す
	CHECK(pacer.size() == 2);
	// D = 0 なので 2 の時刻も来ている。新しいほうだけを渡す。
	const PacedTake<Frame> taken = pacer.take(2000);
	REQUIRE(taken.item.has_value());
	CHECK(taken.item->serial == 2);
	CHECK(taken.skipped == 1);
}

TEST_CASE("早く渡すフレームの時刻は、格子にも D にもよらず、決めたときの今。D を縮めて元の時刻が早くなればそちら")
{
	FramePacer<Frame> pacer(1000000, 1, kBigBytes, true);
	pacer.set_tick_grid(5000, 16000);
	pacer.push(1000000, 0, 100, Frame{1});
	pacer.push(1016700, 0, 100, Frame{2}); // 1 を 1016700 に早く渡す
	CHECK(pacer.next_release_us().value() == 1016700);
	pacer.configure(2000000, 1, kBigBytes, true);
	CHECK(pacer.next_release_us().value() == 1016700);
	pacer.set_tick_grid(0, 33333);
	CHECK(pacer.next_release_us().value() == 1016700);
	// D = 0、格子なしなら、1 の元の時刻 1000000 のほうが早い。早めたことにはならないので、早く渡した数に数えない。
	pacer.set_tick_grid(0, 0);
	pacer.configure(0, 1, kBigBytes, true);
	CHECK(pacer.next_release_us().value() == 1000000);
	const PacedTake<Frame> first = pacer.take(1000000);
	REQUIRE(first.item.has_value());
	CHECK(first.item->serial == 1);
	const FramePacerStats stats = pacer.take_stats();
	CHECK(stats.early == 0);
	CHECK(stats.on_time == 1);
}

TEST_CASE("早く渡した数と長さは統計を取り出すと 0 に戻り、すべて捨てると早く渡すフレームも捨てる")
{
	FramePacer<Frame> pacer(1000000, 1, kBigBytes, true);
	pacer.push(1000000, 0, 100, Frame{1});
	pacer.push(1016700, 0, 100, Frame{2});
	REQUIRE(pacer.take(1016700).item.has_value());
	CHECK(pacer.take_stats().early == 1);
	const FramePacerStats again = pacer.take_stats();
	CHECK(again.early == 0);
	CHECK(again.early_max_us == 0);
	pacer.push(1033400, 0, 100, Frame{3}); // 2 を早く渡す
	CHECK(pacer.discard_all() == 2);
	CHECK(pacer.empty());
	CHECK(pacer.bytes() == 0);
	CHECK_FALSE(pacer.next_release_us().has_value());
}

TEST_CASE("ストリームごとの待ち行列は、それぞれの上限で早く渡す")
{
	FramePacer<Frame> big = make_pipeline_pacer(100000, true);
	FramePacer<Frame> small = make_pipeline_pacer(100000, true);
	const LeadSimResult a = simulate_leads(big, same_frames(200, 400000, kBytes4k));
	const LeadSimResult b = simulate_leads(small, same_frames(200, 400000, kBytes1080p));
	CHECK(a.dropped_on_push + b.dropped_on_push == 0);
	CHECK(a.shown.size() == 200);
	CHECK(b.shown.size() == 200);
	for (const std::int64_t wait : b.wait_us)
		CHECK(wait == 500000);
	CHECK(big.take_stats().early == 179);
	CHECK(small.take_stats().early == 0);
}
