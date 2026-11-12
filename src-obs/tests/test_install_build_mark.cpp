// SPDX-License-Identifier: Apache-2.0

// install/build_mark.h (マーカーのファイル cordmux-build.txt の読み書きと、data/vencord の判定) の単体テスト。

#include "doctest.h"

#include "install/build_mark.h"

#include <optional>
#include <string>
#include <vector>

using cordmux::install::BuildMark;
using cordmux::install::build_mark_to_text;
using cordmux::install::classify_shown_bundle;
using cordmux::install::kBuildMarkFileName;
using cordmux::install::parse_build_mark;
using cordmux::install::ShownBundleVerdict;

namespace {

const BuildMark kNew{"0.2.0+abc1234", "0123456789abcdef0123456789abcdef01234567", "v1.15.9"};
const BuildMark kOld{"0.1.0+fedcba9", "0123456789abcdef0123456789abcdef01234567", "v1.15.9"};

} // namespace

// ---- 読み書き ----

TEST_CASE("マーカーを書くと 3 行の「キー: 値」になり、キーは決めた順で、各行の末尾は LF になる")
{
	CHECK(build_mark_to_text(kNew) == "build_id: 0.2.0+abc1234\nvencord_commit: "
					  "0123456789abcdef0123456789abcdef01234567\nvencord_tag: v1.15.9\n");
	CHECK(std::string(kBuildMarkFileName) == "cordmux-build.txt");
}

TEST_CASE("書いたマーカーを読むと、同じマーカーに戻る")
{
	for (const BuildMark &mark : {kNew, kOld, BuildMark{"0.1.0+dev", "x", "v1.0.0"}}) {
		CAPTURE(mark.build_id);
		const auto read_back = parse_build_mark(build_mark_to_text(mark));
		REQUIRE(read_back.has_value());
		CHECK(*read_back == mark);
	}
}

TEST_CASE("マーカーは、CRLF、空白の数、キーの順、知らないキー、空の行、BOM があっても読める")
{
	const std::vector<std::string> texts = {
		"build_id: 0.2.0+abc1234\r\nvencord_commit: 0123456789abcdef0123456789abcdef01234567\r\nvencord_tag: v1.15.9\r\n",
		"build_id:0.2.0+abc1234\nvencord_commit:   0123456789abcdef0123456789abcdef01234567\nvencord_tag:v1.15.9",
		"vencord_tag: v1.15.9\nvencord_commit: 0123456789abcdef0123456789abcdef01234567\nbuild_id: 0.2.0+abc1234\n",
		"\n# note\nextra_key: 1\nbuild_id: 0.2.0+abc1234 \t\n\nvencord_commit: 0123456789abcdef0123456789abcdef01234567\n"
		"vencord_tag: v1.15.9\n",
		"\xEF\xBB\xBF" "build_id: 0.2.0+abc1234\nvencord_commit: 0123456789abcdef0123456789abcdef01234567\n"
		"vencord_tag: v1.15.9\n",
		"  build_id : 0.2.0+abc1234\n\tvencord_commit\t:\t0123456789abcdef0123456789abcdef01234567\n"
		" vencord_tag : v1.15.9\n",
	};
	for (const std::string &text : texts) {
		CAPTURE(text);
		const auto mark = parse_build_mark(text);
		REQUIRE(mark.has_value());
		CHECK(*mark == kNew);
	}
}

TEST_CASE("キーがない、値が空、同じキーが 2 回ある、キーの大文字と小文字が違う、ファイルが空のときは nullopt を返す")
{
	const std::vector<std::string> texts = {
		"",
		"\n\n",
		"not a mark file",
		"build_id: 0.2.0+abc1234\nvencord_tag: v1.15.9\n",                          // vencord_commit がない
		"vencord_commit: 0123456789abcdef0123456789abcdef01234567\nvencord_tag: v1.15.9\n", // build_id がない
		"build_id: 0.2.0+abc1234\nvencord_commit: x\n",                             // vencord_tag がない
		"build_id:\nvencord_commit: x\nvencord_tag: v1.15.9\n",                     // 値が空
		"build_id: 0.2.0+abc1234\nvencord_commit:   \nvencord_tag: v1.15.9\n",      // 値が空白だけ
		"build_id: 0.2.0+abc1234\nvencord_commit: x\nvencord_tag:\n",               // vencord_tag の値が空
		"build_id: 0.2.0+abc1234\nbuild_id: 0.2.0+abc1234\nvencord_commit: x\nvencord_tag: v1.15.9\n", // 同じキーが 2 回
		"build_id: 0.1.0+a\nvencord_commit: x\nvencord_tag: v1.15.9\nbuild_id: 0.2.0+b\n", // 同じキーが 2 回 (値が違う)
		"build_id: 0.2.0+abc1234\nvencord_commit: x\nvencord_tag: v1.0.0\nvencord_tag: v1.0.1\n", // 同じキーが 2 回
		"Build_Id: 0.2.0+abc1234\nvencord_commit: x\nvencord_tag: v1.15.9\n",       // 大文字と小文字が違う
	};
	for (const std::string &text : texts) {
		CAPTURE(text);
		CHECK_FALSE(parse_build_mark(text).has_value());
	}
}

TEST_CASE("値の中の ':' は値の一部として残す (区切りは最初の ':' だけ)")
{
	const auto mark = parse_build_mark("build_id: 1.0.0+a:b\nvencord_commit: c:d\nvencord_tag: v1:2\n");
	REQUIRE(mark.has_value());
	CHECK(mark->build_id == "1.0.0+a:b");
	CHECK(mark->vencord_commit == "c:d");
	CHECK(mark->vencord_tag == "v1:2");
}

TEST_CASE("印: vencord_tag は読み書きされるが、比べない")
{
	const auto read_back = parse_build_mark(build_mark_to_text(kNew));
	REQUIRE(read_back.has_value());
	CHECK(read_back->vencord_tag == "v1.15.9");

	// tag は比べない: 更新と Force Update の見分けは build_id と vencord_commit だけ。
	BuildMark other_tag = kNew;
	other_tag.vencord_tag = "v9.9.9";
	CHECK(other_tag == kNew);
	CHECK(classify_shown_bundle(other_tag, kNew, true) == ShownBundleVerdict::Matches);
}

// ---- 判定 (ヘッダーの表の 7 行) ----

TEST_CASE("data/vencord-src のマーカーが読めないか比べられなければ、判定は Unknown になる")
{
	for (const std::optional<BuildMark> &shown : {std::optional<BuildMark>(kNew), std::optional<BuildMark>()}) {
		for (const std::optional<bool> &hashes : {std::optional<bool>(true), std::optional<bool>(false),
							   std::optional<bool>()}) {
			CHECK(classify_shown_bundle(std::nullopt, shown, hashes) == ShownBundleVerdict::Unknown);
		}
		CHECK(classify_shown_bundle(kNew, shown, std::nullopt) == ShownBundleVerdict::Unknown);
	}
	CHECK(classify_shown_bundle(kNew, kOld, std::nullopt) == ShownBundleVerdict::Unknown);
}

TEST_CASE("data/vencord のマーカーが cordmux の別のバージョンなら、判定は StaleOwnBundle になる")
{
	// 展開が途中で止まり、data/vencord に前のバージョンのファイルが残った (警告を表示せずにコピーする)。
	CHECK(classify_shown_bundle(kNew, kOld, false) == ShownBundleVerdict::StaleOwnBundle);
	// 4 つのファイルが同じでも、マーカーが違えばコピーする (コピーすれば、次から Matches になる)。
	CHECK(classify_shown_bundle(kNew, kOld, true) == ShownBundleVerdict::StaleOwnBundle);
	// マーカーの vencord_commit だけが違っても、別のバージョンである。
	CHECK(classify_shown_bundle(kNew, BuildMark{kNew.build_id, "ffff", "v1.15.9"}, false) ==
	      ShownBundleVerdict::StaleOwnBundle);
}

TEST_CASE("マーカーが同じでハッシュが違うか、マーカーがなくハッシュが違えば、判定は Overwritten になる")
{
	// Force Update はマーカーのファイルを知らないので、マーカーは残ったまま、4 つのファイルだけが変わる。
	CHECK(classify_shown_bundle(kNew, kNew, false) == ShownBundleVerdict::Overwritten);
	// data/vencord ごと作り直された (マーカーも消えた)。
	CHECK(classify_shown_bundle(kNew, std::nullopt, false) == ShownBundleVerdict::Overwritten);
}

TEST_CASE("マーカーが同じかなく、ハッシュが一致すれば、判定は Matches になる")
{
	CHECK(classify_shown_bundle(kNew, kNew, true) == ShownBundleVerdict::Matches);
	CHECK(classify_shown_bundle(kNew, std::nullopt, true) == ShownBundleVerdict::Matches);
}

TEST_CASE("読んだマーカーから判定までを通すと、StaleOwnBundle、Overwritten、Matches の 3 通りになる")
{
	const auto original = parse_build_mark(build_mark_to_text(kNew));
	const auto stale = parse_build_mark(build_mark_to_text(kOld));
	const auto missing = parse_build_mark(""); // data/vencord にマーカーがない
	REQUIRE(original.has_value());
	REQUIRE(stale.has_value());
	REQUIRE_FALSE(missing.has_value());

	CHECK(classify_shown_bundle(original, stale, false) == ShownBundleVerdict::StaleOwnBundle);
	CHECK(classify_shown_bundle(original, original, false) == ShownBundleVerdict::Overwritten);
	CHECK(classify_shown_bundle(original, missing, false) == ShownBundleVerdict::Overwritten);
	CHECK(classify_shown_bundle(original, original, true) == ShownBundleVerdict::Matches);
}
