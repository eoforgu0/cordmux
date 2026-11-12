// SPDX-License-Identifier: Apache-2.0

// platform/platform.h の counts_as_same_user と is_process_of_current_user (Vesktop を探すときに、OBS と同じ Windows の
// 利用者のプロセスだけを数える決め方) の単体テスト。決め方は SID のバイト列とセッションの値だけで確認し、自分の
// プロセスの確認は Windows でだけ実行する (_WIN32 で囲み、テストスイート windows に入れる)。

#include "doctest.h"

#include "platform/platform.h"

#include <cstdint>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using cordmux::platform::counts_as_same_user;

namespace {
const std::vector<uint8_t> kSidA = {1, 5, 0, 0, 0, 0, 0, 5, 21, 0, 0, 0, 1, 2, 3, 4};
const std::vector<uint8_t> kSidB = {1, 5, 0, 0, 0, 0, 0, 5, 21, 0, 0, 0, 1, 2, 3, 5};
const std::vector<uint8_t> kUnreadable;
} // namespace

TEST_CASE("counts_as_same_user は、2 つの SID を読めれば SID だけで比べ、セッションは見ない")
{
	CHECK(counts_as_same_user(kSidA, kSidA, false));
	CHECK(counts_as_same_user(kSidA, std::vector<uint8_t>(kSidA), true));
	CHECK_FALSE(counts_as_same_user(kSidA, kSidB, true)); // 同じセッションで動く別の利用者
	CHECK_FALSE(counts_as_same_user(kSidA, std::vector<uint8_t>(kSidA.begin(), kSidA.end() - 1), true));
}

TEST_CASE("counts_as_same_user は、相手の SID を読めなければ、同じセッションのときだけ数える")
{
	// 管理者として起動した同じ利用者のプロセスは、トークンを読めないことがある。
	CHECK(counts_as_same_user(kSidA, kUnreadable, true));
	CHECK_FALSE(counts_as_same_user(kSidA, kUnreadable, false));
}

TEST_CASE("counts_as_same_user は、自分の SID を読めなければ、利用者で分けずに数える")
{
	CHECK(counts_as_same_user(kUnreadable, kSidA, false));
	CHECK(counts_as_same_user(kUnreadable, kUnreadable, false));
}

#ifdef _WIN32
TEST_CASE("is_process_of_current_user は、このプロセス自身を同じ利用者のものと判定する" * doctest::test_suite("windows"))
{
	CHECK(cordmux::platform::is_process_of_current_user(uint32_t(GetCurrentProcessId())));
}
#endif // _WIN32
