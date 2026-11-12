// SPDX-License-Identifier: Apache-2.0

// install/removal_table.h (アンインストールで消す対象の表) の形の単体テスト。
// 表の形だけを確認する。各行の中身 (探し方の処理と、消す順序の実行) は、アンインストールのテストが確認する。

#include "doctest.h"

#include "install/removal_table.h"

#include <cstddef>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace cordmux::install;

TEST_CASE("表の行の数は kRemovalItemCount と一致し、RemovalItem のどの値も 1 回ずつ現れる")
{
	CHECK(kRemovalTableRows == kRemovalItemCount);

	std::vector<int> seen(kRemovalItemCount, 0);
	for (const RemovalRow &row : kRemovalTable) {
		const auto index = static_cast<std::size_t>(row.item);
		REQUIRE(index < kRemovalItemCount);
		++seen[index];
	}
	for (std::size_t i = 0; i < kRemovalItemCount; ++i) {
		CAPTURE(i);
		CHECK(seen[i] == 1);
	}
}

TEST_CASE("表の、誰が消すかと段階が食い違わない")
{
	for (const RemovalRow &row : kRemovalTable) {
		CAPTURE(static_cast<int>(row.item));
		switch (row.remover) {
		case Remover::Product:
			// Product の行は段階 (1) から (3) のどれか (段階 (0) は表の行を持たず、段階 (4) は場所を
			// 表示するだけ)。
			CHECK((row.step == UninstallStep::SettingsAndSecret || row.step == UninstallStep::SourcesAndScenes ||
			       row.step == UninstallStep::RestoreVencordDir));
			break;
		case Remover::User:
			// 利用者が消す行は、段階 (4) で場所を表示する。
			CHECK(row.step == UninstallStep::ShowFolder);
			break;
		case Remover::Nobody:
			CHECK(row.step == UninstallStep::NotRemoved);
			break;
		}
		CHECK(row.step != UninstallStep::MarkUninstalling);
	}
}

TEST_CASE("表は Python のツールが文字列として読める形になっている")
{
	// ヘッダーを文字列として読み、Python のツールと同じ規則で行を数える。この形が崩れると Python のツールが
	// 行を数え違えるので、C++ の側でも同じ規則で先に失敗させる。
	const std::string path = std::string(CORDMUX_TEST_FIXTURES_DIR) + "/../../install/removal_table.h";
	std::ifstream in(path, std::ios::binary);
	REQUIRE(in.good());
	std::stringstream buffer;
	buffer << in.rdbuf();
	const std::string text = buffer.str();

	const std::regex row_re(
		R"(^\s*\{RemovalItem::(\w+), Remover::(\w+), Locate::(\w+), UninstallStep::(\w+)\},\s*(//.*)?$)");
	const std::regex count_re(R"(kRemovalItemCount = (\d+);)");

	// 目印は行頭で見る (ヘッダーのコメントにも同じ文字列が出てくるので、文字列を探すだけでは、コメントの行に
	// 一致する。Python のツールも同じ見方をする)。
	const auto starts_with = [](const std::string &s, const char *prefix) { return s.rfind(prefix, 0) == 0; };
	bool inside = false;
	bool saw_begin = false;
	bool saw_end = false;
	std::size_t rows = 0;
	std::istringstream lines(text);
	std::string line;
	while (std::getline(lines, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (starts_with(line, "// removal-table-begin")) {
			inside = true;
			saw_begin = true;
			continue;
		}
		if (starts_with(line, "// removal-table-end")) {
			saw_end = true;
			break;
		}
		if (inside && std::regex_match(line, row_re))
			++rows;
	}
	CHECK(saw_begin);
	CHECK(saw_end);
	CHECK(rows == kRemovalItemCount);

	std::smatch m;
	REQUIRE(std::regex_search(text, m, count_re));
	CHECK(std::stoul(m[1].str()) == kRemovalItemCount);
}

TEST_CASE("RemovalItem の列挙子の数が kRemovalItemCount と一致する")
{
	// 行の数と kRemovalItemCount を比べるだけでは、RemovalItem に列挙子を追加して、表にも kRemovalItemCount にも
	// 追加し忘れたときに成功してしまう (追加した列挙子の値は kRemovalItemCount 以上なので、表のどの行にも
	// 現れず、上の「1 回ずつ現れる」も数え漏らす)。ヘッダーを文字列として読み、`enum class RemovalItem {` から
	// `};` までの、1 行に 1 つの列挙子を数える (ヘッダーの「Python が読む形」)。
	const std::string path = std::string(CORDMUX_TEST_FIXTURES_DIR) + "/../../install/removal_table.h";
	std::ifstream in(path, std::ios::binary);
	REQUIRE(in.good());

	const std::regex enumerator_re(R"(^\s*([A-Za-z_]\w*)\s*(=\s*\w+\s*)?,\s*(//.*)?$)");
	bool inside = false;
	bool saw_end = false;
	std::vector<std::string> names;
	std::size_t explicit_values = 0;
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (!inside) {
			if (line.rfind("enum class RemovalItem {", 0) == 0)
				inside = true;
			continue;
		}
		if (line.rfind("};", 0) == 0) {
			saw_end = true;
			break;
		}
		std::smatch m;
		if (std::regex_match(line, m, enumerator_re)) {
			names.push_back(m[1].str());
			if (m[2].matched)
				++explicit_values;
		}
	}
	REQUIRE(saw_end);
	CHECK(names.size() == kRemovalItemCount);
	// 値を明示しない (0 から 1 ずつ増える。上の「1 回ずつ現れる」は、列挙子の値を 0 から
	// kRemovalItemCount - 1 までの添字として使う)。
	CHECK(explicit_values == 0);
}
