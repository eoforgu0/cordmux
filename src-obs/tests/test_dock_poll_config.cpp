// SPDX-License-Identifier: Apache-2.0

// dock/dock_poll_config.h の maintain_listener_due の単体テスト。待ち受けの世話を呼ぶ時刻の判断を、時間を待たずに、
// 時刻を渡して確認する。Qt にも libobs にも依存しない。

#include "doctest.h"

#include "dock/dock_poll_config.h"

using namespace cordmux::dock;

TEST_CASE("maintain_listener_due は、前に呼んだ時刻から間隔が過ぎたときだけ true を返す")
{
	const std::int64_t last = 10'000'000;
	CHECK_FALSE(maintain_listener_due(last, last));
	CHECK_FALSE(maintain_listener_due(last + kMaintainListenerIntervalUs - 1, last));
	CHECK(maintain_listener_due(last + kMaintainListenerIntervalUs, last));
	CHECK(maintain_listener_due(last + kMaintainListenerIntervalUs * 3, last));
}

TEST_CASE("maintain_listener_due は、システム時刻が前に戻ったときも true を返す")
{
	CHECK(maintain_listener_due(1'000'000, 10'000'000));
}

TEST_CASE("link_just_lost は、接続ありから接続なしに変わったときだけ true を返す")
{
	CHECK(link_just_lost(true, false));
	CHECK_FALSE(link_just_lost(true, true));
	CHECK_FALSE(link_just_lost(false, false));
	CHECK_FALSE(link_just_lost(false, true));
}

TEST_CASE("layout_step_interval_ms は、ドックの表示の更新の間隔によらず同じ値を返す")
{
	// 設定で選べる更新の間隔の下限、既定、上限。
	for (const int dock_poll_ms : {50, 500, 60000})
		CHECK(layout_step_interval_ms(dock_poll_ms) == kLayoutStepIntervalMs);
}

TEST_CASE("kLayoutStepIntervalMs は、更新の間隔の既定より短く、配信の絵として遅れが気にならない値である")
{
	CHECK(kLayoutStepIntervalMs > 0);
	CHECK(kLayoutStepIntervalMs <= 500);
}

TEST_CASE("vesktop_quit_check_due は、Vesktop の終了を待つ操作があり、Vesktop が起動していて接続していない間だけ、間隔ごとに true を返す")
{
	const std::int64_t last = 10'000'000;
	const std::int64_t later = last + kVesktopQuitCheckIntervalUs;
	CHECK(vesktop_quit_check_due(true, false, false, true, later, last));
	CHECK_FALSE(vesktop_quit_check_due(true, false, false, true, later - 1, last));
	// システム時刻が前に戻ったときも調べる。
	CHECK(vesktop_quit_check_due(true, false, false, true, last - 1, last));
	// 起動していない、接続している (切れたときに OsProbe に問い合わせ直させる)、アンインストールを始めた後、終了を待つ
	// 操作がない (cordmux が無効か、Vesktop の設定を読めていない) ときは調べない。
	CHECK_FALSE(vesktop_quit_check_due(false, false, false, true, later, last));
	CHECK_FALSE(vesktop_quit_check_due(true, true, false, true, later, last));
	CHECK_FALSE(vesktop_quit_check_due(true, false, true, true, later, last));
	CHECK_FALSE(vesktop_quit_check_due(true, false, false, false, later, last));
	// 間隔は OsProbe の接続していない間の間隔 (30 秒) より十分短い。
	CHECK(kVesktopQuitCheckIntervalUs <= 2'000'000);
}
