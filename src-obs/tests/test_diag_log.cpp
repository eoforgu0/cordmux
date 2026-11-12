// SPDX-License-Identifier: Apache-2.0

// diag/diag_log.h の単体テスト。libobs を使わず、テストの出力先を登録して、渡された行を確認する。

#include "doctest.h"

#include "diag/diag_log.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// テストの出力先が受け取った行。出力先は関数ポインタで状態を持てないので、ここに置く。
std::vector<std::pair<int, std::string>> g_captured;
std::vector<cordmux::diag::LineKind> g_kinds;

void capture_sink(int level, cordmux::diag::LineKind kind, std::string_view line)
{
	g_captured.emplace_back(level, std::string(line));
	g_kinds.push_back(kind);
}

// テストの終わりに出力先の登録を必ず解除する (ほかのテストに残さない)。
struct SinkScope {
	SinkScope()
	{
		g_captured.clear();
		g_kinds.clear();
	}
	~SinkScope() { cordmux::diag::set_log_sink(nullptr); }
};

} // namespace

TEST_CASE("出力先を登録する前と解除した後は、logf の行を出力先に渡さない")
{
	SinkScope scope;
	cordmux::diag::set_log_sink(nullptr);
	CHECK(cordmux::diag::log_sink() == nullptr);

	cordmux::diag::logf(1, "%s", "before");
	CHECK(g_captured.empty());

	cordmux::diag::set_log_sink(&capture_sink);
	cordmux::diag::logf(2, "%s", "during");
	REQUIRE(g_captured.size() == 1);

	cordmux::diag::set_log_sink(nullptr);
	cordmux::diag::logf(3, "%s", "after");
	CHECK(g_captured.size() == 1);
}

TEST_CASE("logf はログレベルと組み立てた 1 行をそのまま出力先に渡す")
{
	SinkScope scope;
	cordmux::diag::set_log_sink(&capture_sink);
	CHECK(cordmux::diag::log_sink() == &capture_sink);

	cordmux::diag::logf(7, "slot %d of %s (%.1f)", 3, "screen", 1.25);
	REQUIRE(g_captured.size() == 1);
	CHECK(g_captured[0].first == 7);
	CHECK(g_captured[0].second == "slot 3 of screen (1.2)");

	// 先頭の [cordmux] は付けない (付けるのは plugin-main.cpp の出力先)。
	CHECK(g_captured[0].second.find("[cordmux]") == std::string::npos);
}

TEST_CASE("バッファーに収まらない行は、末尾を切って出力先に渡す")
{
	SinkScope scope;
	cordmux::diag::set_log_sink(&capture_sink);

	const std::string huge(cordmux::diag::kMaxLineBytes * 2, 'x');
	cordmux::diag::logf(1, "%s", huge.c_str());
	REQUIRE(g_captured.size() == 1);
	CHECK(g_captured[0].second.size() == cordmux::diag::kMaxLineBytes - 1);
	CHECK(g_captured[0].second.find_first_not_of('x') == std::string::npos);
}

TEST_CASE("書式が nullptr のときは、出力先に何も渡さない")
{
	SinkScope scope;
	cordmux::diag::set_log_sink(&capture_sink);

	cordmux::diag::logf(1, nullptr);
	CHECK(g_captured.empty());
}

TEST_CASE("logf は出来事の行として渡し、種類を付けた logf は統計の行として渡す")
{
	SinkScope scope;
	cordmux::diag::set_log_sink(&capture_sink);

	cordmux::diag::logf(1, "event %d", 1);
	cordmux::diag::logf(cordmux::diag::LineKind::Stats, 2, "stats %d", 2);
	cordmux::diag::logf(cordmux::diag::LineKind::Event, 3, "event %d", 3);
	REQUIRE(g_captured.size() == 3);
	CHECK(g_kinds[0] == cordmux::diag::LineKind::Event);
	CHECK(g_kinds[1] == cordmux::diag::LineKind::Stats);
	CHECK(g_kinds[2] == cordmux::diag::LineKind::Event);
	CHECK(g_captured[1].first == 2);
	CHECK(g_captured[1].second == "stats 2");
}
