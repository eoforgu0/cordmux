// SPDX-License-Identifier: Apache-2.0

// sources/composite_paint_handoff.h (名前ラベルと光の画像を描画のスレッドの外で描くための、仕事の受け渡し) のテスト。
// 描く関数は QPainter の代わりに整数の計算を差し替える。同じ鍵の仕事は渡さないこと、古い未着手の仕事を捨てること、
// 鍵が変わったら描き終えた画像を捨てること、止めるときに描いている最中の仕事を待つこと、保存先を足して空けられること、
// 捨てる画像を受け渡しのロックの外で壊すことを確認する。

#include "doctest.h"

#include "sources/composite_paint_handoff.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using cordmux::sources::PaintHandoff;

namespace {

// 鍵は整数、入力は文字列、出力は入力に "!" を付けた文字列。
using TestHandoff = PaintHandoff<int, std::string, std::string>;

std::string echo(const std::string &input)
{
	return input + "!";
}

} // namespace

TEST_CASE("同じ鍵の仕事は、待っている間も描いた後も受け付けない")
{
	TestHandoff h(2, echo, false);
	int made = 0;
	auto make = [&made] { ++made; return std::string("a"); };
	CHECK(h.submit(0, 1, make));
	CHECK_FALSE(h.submit(0, 1, make)); // 待っている
	CHECK(made == 1);                  // 入力は受け付けたときだけ作る
	CHECK(h.run_one());
	CHECK_FALSE(h.submit(0, 1, make)); // 描き終えて受け取り待ち
	const auto done = h.take(0, 1);
	REQUIRE(done.has_value());
	CHECK(done->output == "a!");
	CHECK(h.submit(0, 1, make)); // 受け取った後は、必要ならもう一度渡せる
	CHECK(made == 2);
}

TEST_CASE("新しい仕事が来たら、古い未着手の仕事は捨てる")
{
	TestHandoff h(1, echo, false);
	CHECK(h.submit(0, 1, [] { return std::string("old"); }));
	CHECK(h.submit(0, 2, [] { return std::string("new"); }));
	CHECK(h.run_one());
	CHECK_FALSE(h.run_one()); // 古い仕事は描かれない
	const auto done = h.take(0, 2);
	REQUIRE(done.has_value());
	CHECK(done->output == "new!");
	CHECK_FALSE(h.take(0, 1).has_value()); // 古い鍵の画像は、できていない
	const auto counts = h.take_counts();
	CHECK(counts.submitted == 2);
	CHECK(counts.finished == 1);
	CHECK(counts.dropped == 1);
}

TEST_CASE("鍵が欲しいものと違う画像は、受け取らずに捨てる")
{
	TestHandoff h(1, echo, false);
	CHECK(h.submit(0, 1, [] { return std::string("a"); }));
	CHECK(h.run_one());
	CHECK_FALSE(h.take(0, 2).has_value()); // 欲しい鍵が変わった
	CHECK_FALSE(h.take(0, 1).has_value()); // 捨てたので、前の鍵でも受け取れない
	CHECK(h.take_counts().dropped == 1);
	// 捨てた鍵の仕事は、また渡せる
	CHECK(h.submit(0, 1, [] { return std::string("a"); }));
}

TEST_CASE("保存先ごとに独立していて、古い順に描く")
{
	TestHandoff h(3, echo, false);
	CHECK(h.submit(2, 7, [] { return std::string("x"); }));
	CHECK(h.submit(0, 7, [] { return std::string("y"); })); // 同じ鍵でも保存先が違えば別の仕事
	CHECK(h.run_one());
	CHECK(h.take(2, 7).has_value()); // 先に渡した保存先 2 が先に描かれる
	CHECK_FALSE(h.take(0, 7).has_value());
	CHECK(h.run_one());
	CHECK(h.take(0, 7).has_value());
	CHECK_FALSE(h.run_one());
	CHECK_FALSE(h.submit(3, 1, [] { return std::string(); })); // 範囲の外
}

TEST_CASE("描く関数が例外を投げても、止まらず捨てた数に入る")
{
	TestHandoff h(1, [](const std::string &) -> std::string { throw std::runtime_error("x"); }, false);
	CHECK(h.submit(0, 1, [] { return std::string("a"); }));
	CHECK(h.run_one());
	CHECK_FALSE(h.take(0, 1).has_value());
	const auto counts = h.take_counts();
	CHECK(counts.finished == 0);
	CHECK(counts.dropped == 1);
}

TEST_CASE("計数は読むと 0 に戻り、描いた時間の最大を持つ")
{
	TestHandoff h(1, [](const std::string &s) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); return s; }, false);
	h.submit(0, 1, [] { return std::string("a"); });
	h.run_one();
	const auto first = h.take_counts();
	CHECK(first.max_paint_ns >= 4000000);
	const auto second = h.take_counts();
	CHECK(second.submitted == 0);
	CHECK(second.max_paint_ns == 0);
}

TEST_CASE("スレッドが描いた画像を受け取れる")
{
	TestHandoff h(1, echo);
	CHECK(h.submit(0, 1, [] { return std::string("a"); }));
	std::optional<TestHandoff::Done> done;
	for (int i = 0; i < 500 && !done; ++i) {
		done = h.take(0, 1);
		if (!done)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	REQUIRE(done.has_value());
	CHECK(done->output == "a!");
}

TEST_CASE("止めるときは、待っている仕事を捨て、描いている最中の仕事を待つ")
{
	std::mutex gate_mu;
	std::condition_variable gate_cv;
	bool started = false;
	bool release = false;
	std::atomic<bool> finished_paint{false};
	auto paint = [&](const std::string &s) {
		std::unique_lock<std::mutex> lock(gate_mu);
		started = true;
		gate_cv.notify_all();
		gate_cv.wait(lock, [&] { return release; });
		finished_paint = true;
		return s;
	};
	TestHandoff h(2, paint);
	CHECK(h.submit(0, 1, [] { return std::string("running"); }));
	{
		std::unique_lock<std::mutex> lock(gate_mu);
		REQUIRE(gate_cv.wait_for(lock, std::chrono::seconds(10), [&] { return started; }));
	}
	CHECK(h.submit(1, 1, [] { return std::string("waiting"); })); // 描いている最中に、別の保存先へ渡す

	std::atomic<bool> stop_returned{false};
	std::thread stopper([&] {
		h.stop();
		stop_returned = true;
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	CHECK_FALSE(stop_returned); // 描き終えるまで返らない
	{
		std::lock_guard<std::mutex> lock(gate_mu);
		release = true;
	}
	gate_cv.notify_all();
	stopper.join();
	CHECK(stop_returned);
	CHECK(finished_paint);
	CHECK(h.stopped());
	// 描き終えた画像は捨て、待っていた仕事は描かれず、止めた後は受け付けない
	CHECK_FALSE(h.take(0, 1).has_value());
	CHECK_FALSE(h.take(1, 1).has_value());
	CHECK_FALSE(h.submit(0, 2, [] { return std::string("late"); }));
	const auto counts = h.take_counts();
	CHECK(counts.finished == 0);
	CHECK(counts.dropped == 2);
	h.stop(); // 何度呼んでもよい
}

TEST_CASE("止めるのは、何も描いていなければすぐ終わる")
{
	TestHandoff h(1, echo);
	const auto started = std::chrono::steady_clock::now();
	h.stop();
	CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
}

namespace {

struct LockProbe;
using ProbeHandoff = PaintHandoff<int, int, LockProbe>;

// LockProbe が数える値。handoff は確認する相手、waiting はロックが空くのを待っているスレッド (テストの終わりに待つ)。
struct LockProbeState {
	ProbeHandoff *handoff = nullptr;
	int destroyed = 0;
	int destroyed_under_lock = 0;
	std::vector<std::thread> waiting;
};

// 壊れるときに、受け渡しのロックが空いているかを確認する出力。別のスレッドからロックを取る関数を呼び、2 秒以内に返れば
// 空いている。返らなければ、壊す側がロックを持っている (そのスレッドはロックが空いた後に終わるので、テストの終わりに待つ)。
struct LockProbe {
	LockProbeState *state = nullptr;

	LockProbe() = default;
	explicit LockProbe(LockProbeState *s) : state(s) {}
	LockProbe(LockProbe &&other) noexcept : state(std::exchange(other.state, nullptr)) {}
	LockProbe &operator=(LockProbe &&other) noexcept
	{
		if (this != &other) {
			finish();
			state = std::exchange(other.state, nullptr);
		}
		return *this;
	}
	LockProbe(const LockProbe &) = delete;
	LockProbe &operator=(const LockProbe &) = delete;
	~LockProbe() { finish(); }

	void finish()
	{
		if (!state)
			return;
		LockProbeState *s = std::exchange(state, nullptr);
		++s->destroyed;
		auto returned = std::make_shared<std::atomic<bool>>(false);
		std::thread probe([s, returned] {
			s->handoff->stopped();
			*returned = true;
		});
		for (int i = 0; i < 200 && !*returned; ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		if (*returned) {
			probe.join();
		} else {
			++s->destroyed_under_lock;
			s->waiting.push_back(std::move(probe));
		}
	}
};

} // namespace

TEST_CASE("保存先を足すと、新しい番号の保存先に仕事を渡せる")
{
	TestHandoff h(1, echo, false);
	CHECK_FALSE(h.submit(1, 1, [] { return std::string("a"); })); // まだない
	CHECK(h.add_slot() == 1);
	CHECK(h.add_slot() == 2);
	CHECK(h.submit(2, 1, [] { return std::string("b"); }));
	CHECK(h.run_one());
	const auto done = h.take(2, 1);
	REQUIRE(done.has_value());
	CHECK(done->output == "b!");
}

TEST_CASE("描いている間に保存先を足しても、描き終えた画像は元の保存先に置かれる")
{
	std::mutex gate_mu;
	std::condition_variable gate_cv;
	bool started = false;
	bool release = false;
	auto paint = [&](const std::string &s) {
		std::unique_lock<std::mutex> lock(gate_mu);
		started = true;
		gate_cv.notify_all();
		gate_cv.wait(lock, [&] { return release; });
		return s + "!";
	};
	TestHandoff h(1, paint);
	CHECK(h.submit(0, 1, [] { return std::string("a"); }));
	{
		std::unique_lock<std::mutex> lock(gate_mu);
		REQUIRE(gate_cv.wait_for(lock, std::chrono::seconds(10), [&] { return started; }));
	}
	// 保存先の表が作り直されるくらい足す。
	for (int i = 0; i < 100; ++i)
		h.add_slot();
	{
		std::lock_guard<std::mutex> lock(gate_mu);
		release = true;
	}
	gate_cv.notify_all();
	std::optional<TestHandoff::Done> done;
	for (int i = 0; i < 500 && !done; ++i) {
		done = h.take(0, 1);
		if (!done)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	REQUIRE(done.has_value());
	CHECK(done->output == "a!");
}

TEST_CASE("保存先を空けると、待っている仕事を捨て、描き終えた画像を受け取らずに返す")
{
	TestHandoff h(2, echo, false);
	CHECK(h.submit(0, 1, [] { return std::string("done"); }));
	CHECK(h.run_one());
	CHECK(h.submit(0, 2, [] { return std::string("waiting"); }));
	CHECK(h.submit(1, 1, [] { return std::string("other"); }));
	const auto cleared = h.clear_slot(0);
	REQUIRE(cleared.has_value());
	CHECK(cleared->output == "done!");
	CHECK_FALSE(h.clear_slot(0).has_value()); // もう何もない
	CHECK_FALSE(h.clear_slot(5).has_value()); // 範囲の外
	CHECK(h.run_one());                       // 残っているのは保存先 1 の仕事だけ
	CHECK_FALSE(h.run_one());
	CHECK(h.take(1, 1).has_value());
	const auto counts = h.take_counts();
	CHECK(counts.dropped == 2); // 待っていた仕事と、受け取らなかった画像
	// 空けた保存先は、また使える
	CHECK(h.submit(0, 1, [] { return std::string("again"); }));
}

TEST_CASE("描き終えて受け取っていない画像は、鍵によらず全部まとめて受け取れる")
{
	TestHandoff h(3, echo, false);
	CHECK(h.submit(0, 1, [] { return std::string("a"); }));
	CHECK(h.submit(2, 9, [] { return std::string("c"); }));
	CHECK(h.run_one());
	CHECK(h.run_one());
	h.stop();
	const auto drained = h.drain_done();
	REQUIRE(drained.size() == 2);
	CHECK(drained[0].output == "a!");
	CHECK(drained[1].output == "c!");
	CHECK(h.drain_done().empty());
	CHECK_FALSE(h.take(0, 1).has_value());
}

TEST_CASE("捨てる画像は、受け渡しのロックを放してから壊す")
{
	LockProbeState state;
	ProbeHandoff h(1, [&state](const int &) { return LockProbe(&state); }, false);
	state.handoff = &h;

	// 描き終えた画像を受け取る前に、同じ保存先の次の画像ができた。
	CHECK(h.submit(0, 1, [] { return 0; }));
	CHECK(h.run_one());
	CHECK(h.submit(0, 2, [] { return 0; }));
	CHECK(h.run_one());
	CHECK(state.destroyed == 1);
	// 欲しい鍵と違う画像を捨てる。
	CHECK_FALSE(h.take(0, 3).has_value());
	CHECK(state.destroyed == 2);
	// 保存先を空けたときと、まとめて受け取ったときは、呼び出し元が壊す。
	CHECK(h.submit(0, 4, [] { return 0; }));
	CHECK(h.run_one());
	h.clear_slot(0);
	CHECK(state.destroyed == 3);
	CHECK(h.submit(0, 5, [] { return 0; }));
	CHECK(h.run_one());
	h.drain_done();
	CHECK(state.destroyed == 4);
	CHECK(state.destroyed_under_lock == 0);
	for (std::thread &t : state.waiting)
		t.join();
}
