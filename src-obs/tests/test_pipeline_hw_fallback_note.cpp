// SPDX-License-Identifier: Apache-2.0

// pipeline/hw_fallback_note.h の単体テスト。ソフトウェアデコードへの切り替えを、ストリームごとに 1 度だけ伝えることを確認する。

#include "doctest.h"

#include "pipeline/hw_fallback_note.h"

using cordmux::pipeline::HwFallbackNote;

TEST_CASE("切り替えていない間は伝えず、切り替えたと初めて見たときだけ伝える")
{
	HwFallbackNote note;
	CHECK_FALSE(note.observe(false));
	CHECK_FALSE(note.observe(false));
	CHECK(note.observe(true));
}

TEST_CASE("切り替えたままでも、2 回目以降は伝えない")
{
	HwFallbackNote note;
	CHECK(note.observe(true));
	CHECK_FALSE(note.observe(true));
	CHECK_FALSE(note.observe(true));
}

TEST_CASE("ストリームごとに別の記録を持つので、別のストリームの切り替えは別に伝える")
{
	HwFallbackNote first;
	HwFallbackNote second;
	CHECK(first.observe(true));
	CHECK(second.observe(true));
	CHECK_FALSE(first.observe(true));
}
