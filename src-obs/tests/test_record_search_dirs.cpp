// SPDX-License-Identifier: Apache-2.0

// record/record_search_dirs.h の単体テスト。録画ファイルの作成の済んでいないセッションが残る保存先の一覧の、更新の規則と
// ファイルの読み書きを確認する。Recorder を通したテストは test_record_io_phases.cpp にある。

#include "doctest.h"

#include "record/record_search_dirs.h"

#include <string>
#include <vector>

using cordmux::record::format_pending_dirs;
using cordmux::record::parse_pending_dirs;
using cordmux::record::SearchDirResult;
using cordmux::record::update_pending_dirs;

namespace {

SearchDirResult result_of(const std::string &dir, bool reachable, bool holds)
{
	SearchDirResult result;
	result.base_dir = dir;
	result.reachable = reachable;
	result.holds_unfinished = holds;
	return result;
}

} // namespace

TEST_CASE("録画ファイルの作成の済んでいないセッションがある保存先は一覧に加え、探してそれがなかった保存先だけを外す")
{
	const std::vector<std::string> remembered = {"D:/old", "E:/usb", "F:/nas"};
	const std::vector<SearchDirResult> results = {
		result_of("D:/old", true, false),  // アクセスできて探せた。録画ファイルの作成の済んでいないセッションはない -> 外す
		result_of("F:/nas", false, false), // 一覧を読めなかった (アクセスできなかった) -> 残す
		result_of("C:/now", true, true),   // 現在の保存先に、録画ファイルの作成の済んでいないセッションがある -> 加える
		result_of("C:/clean", true, false) // 一覧になく、録画ファイルの作成の済んでいないセッションもない -> 加えない
	};
	// E:/usb は探さなかった (起動のときにアクセスできなかった) -> 残す。
	CHECK(update_pending_dirs(remembered, results) == std::vector<std::string>{"E:/usb", "F:/nas", "C:/now"});

	// 録画ファイルの作成の済んでいないセッションが残る保存先は、アクセスできても外さない。書き方の違う同じ場所は
	// 1 つにまとめる。
	CHECK(update_pending_dirs({"D:\\Old\\"}, {result_of("d:/old", true, true)}) == std::vector<std::string>{"D:\\Old\\"});
	// 何も変わらなければ同じ並びを返す (呼び出し元は、変わったときだけ書く)。
	CHECK(update_pending_dirs(remembered, {}) == remembered);
}

TEST_CASE("一覧のファイルの中身を書いて読み、形が違えば空にする")
{
	const std::vector<std::string> dirs = {"D:/rec", "E:/録画"};
	CHECK(parse_pending_dirs(format_pending_dirs(dirs)) == dirs);
	CHECK(parse_pending_dirs("").empty());
	CHECK(parse_pending_dirs("[\"D:/rec\"]").empty());
	CHECK(parse_pending_dirs("{\"dirs\": \"D:/rec\"}").empty());
	// 文字列でない項目、空の項目、重なった項目は除く。
	CHECK(parse_pending_dirs("{\"dirs\": [\"D:/rec\", 3, \"\", \"d:/REC/\"]}") == std::vector<std::string>{"D:/rec"});
}
