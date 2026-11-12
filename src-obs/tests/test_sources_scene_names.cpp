// SPDX-License-Identifier: Apache-2.0

// sources/scene_names.h の純粋関数 (名前の作り方) のテスト。
// テストの中のテンプレートは ini の文字列と同じ形にしたもので、ini は読まない。

#include "doctest.h"

#include "sources/scene_names.h"

#include <string>

using cordmux::sources::fill_template;

TEST_CASE("fill_template は %1 から %9 を 1 回の走査で置き換え、人の名前の中の %2 は置き換えない")
{
	CHECK(fill_template("%1 (cordmux 画面共有 %2)", {"たろう", "3"}) == "たろう (cordmux 画面共有 3)");
	// 人の名前に %2 の形の文字列が入っていても、2 回目の置き換えをしない。
	CHECK(fill_template("%1 (cordmux 画面共有 %2)", {"a%2b", "1"}) == "a%2b (cordmux 画面共有 1)");
	CHECK(fill_template("%1 (cordmux 画面共有 %2)", {"%1", "1"}) == "%1 (cordmux 画面共有 1)");
	// 引数のない %n と、後ろが 1 から 9 でない % はそのまま (ini が崩れていてもクラッシュしない)。
	CHECK(fill_template("cordmux 画面共有 %1 (空き) %3 100%", {"4"}) == "cordmux 画面共有 4 (空き) %3 100%");
	CHECK(fill_template("%", {"x"}) == "%");
	CHECK(fill_template("%0 %1", {"x"}) == "%0 x");
	// 区切りの文字はコードで追加しない (テンプレートの文字だけで名前を作る)。
	CHECK(fill_template("%1%2", {"a", "b"}) == "ab");
}
