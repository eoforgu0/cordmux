// SPDX-License-Identifier: Apache-2.0

// pipeline/frame_pacer.h の単体テスト。画面共有のフレームを「送信側のタイムスタンプ + 表示の緩衝 D」の時刻まで
// 保持する待ち行列の、渡す時刻、遅れて届いたフレーム、時刻が来たフレームが複数あるときの扱い、D の変更、時計の飛び、
// 上限、統計を確認する。

#include "doctest.h"

#include "pipeline/frame_pacer.h"

#include <cstddef>
#include <cstdint>
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
