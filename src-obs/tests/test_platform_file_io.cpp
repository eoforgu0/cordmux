// SPDX-License-Identifier: Apache-2.0

// platform/platform.h のファイルの読み書き (read_file_bytes と write_file_bytes) の単体テスト。
//
// 場所の文字列は UTF-8 で持つが、Windows で char の場所を取る関数 (fopen や std::fstream) は、それをアクティブ
// コードページの文字列として読む。そのため、ASCII でない文字を含む利用者のフォルダーでは、フォルダーは作れるのに
// ファイルを開けない。ここでは ASCII でない文字を含む場所を実際に作り、書いた中身をその場所からそのまま読めることを
// 確認する。場所に使う文字は u8 の文字列リテラルで書く (ソースは UTF-8 で、/utf-8 でコンパイルする)。

#include "doctest.h"

#include "platform/platform.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

using cordmux::platform::read_file_bytes;
using cordmux::platform::write_file_bytes;

namespace {

// 読み込みの上限。このテストのファイルはどれも数十バイトなので、上限で読めなくならない大きさにする。
constexpr std::size_t kMaxBytes = 1u * 1024u * 1024u;

// ASCII でない 2 文字を含む名前の一部 (tests/test_platform_record_ops.cpp の kNonAsciiPart と同じ 2 文字)。どちらも日本語の
// コードページ (932) で表せない文字なので、char の場所を取る関数で開くと必ず失敗する。タイ文字 (1 文字の幅) と
// ハングル (2 文字の幅) で、幅が決まっている文字だけを使う (曖昧幅の文字は、失敗したときに表示される場所の文字列で
// 桁がずれる)。ひらがなは 932 で表せるので、char の場所を取る関数に戻ってしまっても見つけられない。
constexpr const char *kNonAsciiPart = u8"ก한";

// リポジトリの外の一時フォルダーの下に、ASCII でない文字を含む名前のフォルダーを作り、デストラクターで削除する。
class TempDir {
public:
	TempDir()
	{
		static std::atomic<int> counter{0};
		const std::string name = "cordmux_file_io_test_" + std::to_string(++counter) + "_" +
					 std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "_" +
					 std::string(kNonAsciiPart);
		dir_ = std::filesystem::temp_directory_path() / std::filesystem::u8path(name);
		std::filesystem::create_directories(dir_);
	}

	~TempDir()
	{
		std::error_code ec;
		std::filesystem::remove_all(dir_, ec);
	}

	TempDir(const TempDir &) = delete;
	TempDir &operator=(const TempDir &) = delete;

	// このフォルダーの中の 1 つのファイルの場所を、UTF-8 の文字列で返す (platform/platform.h の関数が受け取る形)。
	std::string utf8_path(const std::string &file_name) const
	{
		const std::filesystem::path path = dir_ / std::filesystem::u8path(file_name);
		return path.u8string();
	}

	// 同じ場所を std::filesystem::path で返す (UTF-8 の文字列から作り直す)。
	std::filesystem::path fs_path(const std::string &file_name) const
	{
		return std::filesystem::u8path(utf8_path(file_name));
	}

private:
	std::filesystem::path dir_;
};

} // namespace

TEST_CASE("ASCII でない文字を含む場所に書いたファイルを、同じ場所から読める")
{
	const TempDir dir;
	const std::string name = std::string("marker_") + kNonAsciiPart + ".txt";
	const std::string path = dir.utf8_path(name);
	const std::string body = "shown\n";

	REQUIRE(write_file_bytes(path, body));

	// 指定した場所にファイルができている (別の名前のファイルができていない)。
	CHECK(std::filesystem::exists(dir.fs_path(name)));

	std::string read_back;
	REQUIRE(read_file_bytes(path, read_back, kMaxBytes));
	CHECK(read_back == body);
}

TEST_CASE("同じ場所に書き直すと、前の中身は残らない")
{
	const TempDir dir;
	const std::string path = dir.utf8_path(std::string("backup_") + kNonAsciiPart + ".json");

	REQUIRE(write_file_bytes(path, "{\"written\":\"aaaaaaaaaaaaaaaaaaaa\"}"));
	REQUIRE(write_file_bytes(path, "{\"written\":\"b\"}"));

	std::string read_back;
	REQUIRE(read_file_bytes(path, read_back, kMaxBytes));
	CHECK(read_back == "{\"written\":\"b\"}");
}

TEST_CASE("書いて読んだ中身は、改行と 0 のバイトも含めて 1 バイトも変わらない")
{
	const TempDir dir;
	const std::string path = dir.utf8_path("bytes.bin");

	// 改行を入れる (テキストモードで開いていると、Windows では 2 バイトに増える)。0 のバイトも入れる (途中で切れない
	// ことを確認する)。
	std::string body = "a\nb\r\nc";
	body.push_back('\0');
	body += "d";

	REQUIRE(write_file_bytes(path, body));
	CHECK(std::filesystem::file_size(dir.fs_path("bytes.bin")) == body.size());

	std::string read_back;
	REQUIRE(read_file_bytes(path, read_back, kMaxBytes));
	CHECK(read_back == body);
	CHECK(read_back.size() == body.size());
}

TEST_CASE("空の中身を書くと、長さ 0 のファイルができる")
{
	const TempDir dir;
	const std::string path = dir.utf8_path("empty.txt");

	REQUIRE(write_file_bytes(path, std::string()));
	CHECK(std::filesystem::exists(dir.fs_path("empty.txt")));

	std::string read_back = "not-empty";
	REQUIRE(read_file_bytes(path, read_back, kMaxBytes));
	CHECK(read_back.empty());
}

TEST_CASE("フォルダーがない場所には書けず、write_file_bytes が false を返す")
{
	const TempDir dir;
	const std::string path = dir.utf8_path("no-such-dir/inner.txt");

	CHECK_FALSE(write_file_bytes(path, "x"));
	CHECK_FALSE(std::filesystem::exists(dir.fs_path("no-such-dir/inner.txt")));
}

TEST_CASE("ないファイルは読めず、read_file_bytes が false を返して out を変えない")
{
	const TempDir dir;
	const std::string path = dir.utf8_path("absent.txt");

	std::string read_back = "untouched";
	CHECK_FALSE(read_file_bytes(path, read_back, kMaxBytes));
	CHECK(read_back == "untouched"); // 読めなかったときは out を変えない (platform/platform.h の read_file_bytes のコメント)
}

TEST_CASE("max_bytes より大きいファイルは、read_file_bytes が読まない")
{
	const TempDir dir;
	const std::string path = dir.utf8_path("big.txt");
	const std::string body(64, 'x');

	REQUIRE(write_file_bytes(path, body));

	std::string read_back;
	CHECK_FALSE(read_file_bytes(path, read_back, 63));
	CHECK(read_file_bytes(path, read_back, 64));
	CHECK(read_back == body);
}
