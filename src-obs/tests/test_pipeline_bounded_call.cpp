// SPDX-License-Identifier: Apache-2.0

// pipeline/bounded_call.h の単体テスト。タイムアウトまでに返った答えはそのまま返ること、タイムアウトしたときは
// 値を持たないこと、待つのをやめたスレッドを後で join できることを確認する。

#include "doctest.h"

#include "pipeline/bounded_call.h"

#include <atomic>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <thread>

using cordmux::pipeline::BoundedCall;

TEST_CASE("タイムアウトまでに返った答えは、true も false もそのまま返る")
{
	BoundedCall call;
	const std::optional<bool> yes = call.ask([]() { return true; }, 5000);
	REQUIRE(yes.has_value());
	CHECK(*yes);

	const std::optional<bool> no = call.ask([]() { return false; }, 5000);
	REQUIRE(no.has_value());
	CHECK_FALSE(*no);

	CHECK(call.pending_count() == 0);
}

TEST_CASE("タイムアウトした問い合わせは値を持たずに戻り、そのスレッドは後で join できる")
{
	std::atomic<bool> release{false};
	std::atomic<int> finished{0};
	{
		BoundedCall call;
		const std::optional<bool> answer = call.ask(
			[&release, &finished]() {
				while (!release.load())
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				++finished;
				return true; // タイムアウトの後に true を返しても、呼び出し元はもう受け取らない
			},
			30);
		// タイムアウトしたので値を持たない (false ではない)。
		CHECK_FALSE(answer.has_value());
		// 待つのをやめたスレッドは保持している。
		CHECK(call.pending_count() == 1);

		release.store(true);
		call.join_pending();
		CHECK(call.pending_count() == 0);
	}
	CHECK(finished.load() == 1);
}

TEST_CASE("タイムアウトが 0 以下なら、待たずに値のない答えを返す")
{
	std::atomic<bool> release{false};
	BoundedCall call;
	const std::optional<bool> answer = call.ask(
		[&release]() {
			while (!release.load())
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			return true;
		},
		0);
	CHECK_FALSE(answer.has_value());
	CHECK(call.pending_count() == 1);
	release.store(true);
	// join_pending を呼ばなくても、デストラクターが join する。
}

TEST_CASE("例外を投げた問い合わせは false を返す")
{
	BoundedCall call;
	const std::optional<bool> answer = call.ask([]() -> bool { throw std::runtime_error("boom"); }, 5000);
	REQUIRE(answer.has_value());
	CHECK_FALSE(*answer);
}
