// SPDX-License-Identifier: Apache-2.0

// platform/platform.h の、ファイルの場所を開く関数 (resolve_file_location と open_file_location) の単体テスト。
// [ファイルの場所を開く] が、どこを開いて何を選ぶかを確認する。open_file_location は、開かずに戻る場合だけを確認する
// (開ける場所を渡すと、エクスプローラーのウィンドウが開くため。選んだ状態で開くことは、このテストでは確認しない)。
// 場所には ASCII でない文字を含める (tests/test_platform_file_io.cpp と同じ 2 文字)。Windows の場所の形 (ドライブのルートと
// `\`) を確認するので、_WIN32 で囲み、テストスイート windows に入れる。

#include "doctest.h"

#include "platform/platform.h"

#ifdef _WIN32

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

using cordmux::platform::FileLocationTarget;
using cordmux::platform::open_file_location;
using cordmux::platform::resolve_file_location;

namespace {

// tests/test_platform_file_io.cpp と同じ 2 文字 (タイ文字とハングル)。
constexpr const char *kNonAsciiPart = u8"\u0e01\ud55c";

class LocationDir {
public:
	LocationDir()
	{
		static std::atomic<int> counter{0};
		const std::string name = "cordmux_file_location_test_" + std::to_string(++counter) + "_" +
					 std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "_" +
					 std::string(kNonAsciiPart);
		// 一時フォルダーの場所を、GetFullPathNameW を通した形 (区切りが `\` で、末尾の区切りがない) にそろえておく
		// (結果と文字列で比べるため)。
		dir_ = std::filesystem::absolute(std::filesystem::temp_directory_path() / std::filesystem::u8path(name));
		std::filesystem::create_directories(dir_);
	}

	~LocationDir()
	{
		std::error_code ec;
		std::filesystem::remove_all(dir_, ec);
	}

	LocationDir(const LocationDir &) = delete;
	LocationDir &operator=(const LocationDir &) = delete;

	std::string path(const std::string &relative) const
	{
		return (dir_ / std::filesystem::u8path(relative)).u8string();
	}

	std::string root() const { return dir_.u8string(); }

private:
	std::filesystem::path dir_;
};

void put_file(const std::string &utf8_path)
{
	std::ofstream f(std::filesystem::u8path(utf8_path), std::ios::binary | std::ios::trunc);
	f << "x";
}

} // namespace

TEST_SUITE("windows")
{

TEST_CASE("resolve_file_location は、存在するファイルには、親のフォルダーを開いてそのファイルを選ぶ")
{
	LocationDir dir;
	const std::string file = dir.path(std::string(kNonAsciiPart) + ".mkv");
	put_file(file);

	FileLocationTarget t;
	REQUIRE(resolve_file_location(file, t));
	CHECK(t.folder == dir.root());
	CHECK(t.item == file);
}

TEST_CASE("resolve_file_location は、存在するフォルダーには、親のフォルダーを開いてそのフォルダーを選ぶ (末尾の区切りは無視する)")
{
	LocationDir dir;
	const std::string sub = dir.path("sub");
	std::filesystem::create_directories(std::filesystem::u8path(sub));

	FileLocationTarget t;
	REQUIRE(resolve_file_location(sub, t));
	CHECK(t.folder == dir.root());
	CHECK(t.item == sub);

	FileLocationTarget t2;
	REQUIRE(resolve_file_location(sub + "\\", t2));
	CHECK(t2.folder == dir.root());
	CHECK(t2.item == sub);
}

TEST_CASE("resolve_file_location は、もうないファイルの親のフォルダーを開き、何も選ばない")
{
	LocationDir dir;
	const std::string file = dir.path("gone.mkv");
	put_file(file);
	std::filesystem::remove(std::filesystem::u8path(file));

	FileLocationTarget t;
	REQUIRE(resolve_file_location(file, t));
	CHECK(t.folder == dir.root());
	CHECK(t.item.empty());
}

TEST_CASE("途中のフォルダーもないとき、resolve_file_location は、存在するフォルダーのうち、いちばん近い親を開く")
{
	LocationDir dir;
	std::filesystem::create_directories(std::filesystem::u8path(dir.path("kept")));
	const std::string deep = dir.path("kept\\gone1\\gone2\\file.json");

	FileLocationTarget t;
	REQUIRE(resolve_file_location(deep, t));
	CHECK(t.folder == dir.path("kept"));
	CHECK(t.item.empty());
}

TEST_CASE("resolve_file_location は、`/` と `..` を正規化してから、親のフォルダーをたどる")
{
	LocationDir dir;
	const std::string file = dir.path("a.mkv");
	put_file(file);
	std::filesystem::create_directories(std::filesystem::u8path(dir.path("other")));

	std::string slashed = dir.root() + "/other/../a.mkv";
	FileLocationTarget t;
	REQUIRE(resolve_file_location(slashed, t));
	CHECK(t.folder == dir.root());
	CHECK(t.item == file); // `..` の 1 つ上 (other) を親と読まない
}

TEST_CASE("resolve_file_location は、ドライブのルートにはルートを開き、何も選ばない")
{
	LocationDir dir;
	const std::string drive = std::filesystem::u8path(dir.root()).root_name().u8string(); // 例 "C:"
	REQUIRE(drive.size() == 2);

	FileLocationTarget t;
	REQUIRE(resolve_file_location(drive + "\\", t));
	CHECK(t.folder == drive + "\\"); // `C:` (そのドライブの作業フォルダー) にしない
	CHECK(t.item.empty());

	// ルートの直下のファイルの親も `C:\` の形になる (ないファイルなので、親であるルートを開く)。
	FileLocationTarget t2;
	REQUIRE(resolve_file_location(drive + "\\cordmux_file_location_test_no_such_file.bin", t2));
	CHECK(t2.folder == drive + "\\");
	CHECK(t2.item.empty());
}

TEST_CASE("空の場所、相対の場所、ドライブだけが付いた相対の場所には、resolve_file_location が false を返して out を変えない")
{
	FileLocationTarget t;
	t.folder = "keep-folder";
	t.item = "keep-item";
	CHECK_FALSE(resolve_file_location("", t));
	CHECK_FALSE(resolve_file_location("a\\b.mkv", t));
	CHECK_FALSE(resolve_file_location("C:foo\\b.mkv", t));
	CHECK_FALSE(resolve_file_location("C:", t));
	CHECK(t.folder == "keep-folder");
	CHECK(t.item == "keep-item");
}

TEST_CASE("開く場所が決まらなければ、open_file_location は何も開かずに false を返す")
{
	// 開ける場所は渡さない (エクスプローラーのウィンドウが開くため)。
	CHECK_FALSE(open_file_location(""));
	CHECK_FALSE(open_file_location("relative\\file.mkv"));
	CHECK_FALSE(open_file_location("C:foo"));
}

} // TEST_SUITE("windows") の終わり

#endif // _WIN32
