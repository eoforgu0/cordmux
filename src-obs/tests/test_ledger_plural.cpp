// SPDX-License-Identifier: Apache-2.0

// ledger/plural.h の単体テスト。数がちょうど 1 のときだけ `_one` のキーを選ぶことと、text_ids.h の CountedId が
// どれも `<キー>` と `<キー>_one` の組になっていることを確認する。ini の側の組は test/lint/locale_ids.py が
// チェックする。

#include "doctest.h"

#include "ledger/plural.h"
#include "ledger/text_ids.h"

#include <string>

using namespace cordmux::ledger::text;

namespace {

// テスト用のキーは `cordmux.` で始めない。test/lint/locale_ids.py は `cordmux.` で始まる文字列を ini のキーと
// 突き合わせ、ini にないキーをエラーにするためである。
constexpr CountedId kSample = {"sample.status", "sample.status_one"};

// text_ids.h の text::keys にある CountedId。CountedId を追加したら、ここにも追加する。
const CountedId kAllCounted[] = {
	keys::kDockSummaryCallStatus,
	keys::kDockSummaryNoneDisplayedStatus,
	keys::kDockSummaryCallShortStatus,
	keys::kDockStageCallJoinedStatus,
	keys::kDockRecordingOnlyHeadline,
	keys::kDockNoVideoHeadline,
	keys::kDockSpaceLowHeadline,
	keys::kDockFinalizeProgressStatus,
	keys::kDockFinalizeProgressFileStatus,
	keys::kDockFinalizeStalledStatus,
	keys::kDockRecordGapHeadline,
	keys::kDockPastRemovedHeadline,
};

} // namespace

TEST_CASE("pick は数がちょうど 1 のときだけ _one のキーを選ぶ")
{
	CHECK(std::string(pick(kSample, 1)) == "sample.status_one");
	CHECK(std::string(pick(kSample, 2)) == "sample.status");
	CHECK(std::string(pick(kSample, 12345)) == "sample.status");
}

TEST_CASE("英語では 0 も複数形なので、pick は 0 のときに元のキーを選ぶ")
{
	CHECK(std::string(pick(kSample, 0)) == "sample.status");
}

TEST_CASE("pick は負の数のときも元のキーを選ぶ")
{
	CHECK(std::string(pick(kSample, -1)) == "sample.status");
	CHECK(std::string(pick(kSample, -2)) == "sample.status");
}

TEST_CASE("pick はコンパイル時にも評価できる")
{
	static_assert(pick(kSample, 1) == kSample.one);
	static_assert(pick(kSample, 0) == kSample.other);
	CHECK(true);
}

TEST_CASE("text_ids.h の CountedId はどれも <キー> と <キー>_one の組になっている")
{
	for (const CountedId &id : kAllCounted) {
		CAPTURE(std::string(id.other));
		CHECK(std::string(id.one) == std::string(id.other) + "_one");
		CHECK(std::string(id.other).rfind("cordmux.", 0) == 0);
	}
}
