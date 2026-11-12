// SPDX-License-Identifier: Apache-2.0

// platform/platform.h の、インストールとアンインストールで使うファイルの操作の単体テスト。置き換える関数
// (write_file_atomic、write_file_bytes、copy_file_replace) が、置き換えで失敗しても元のファイルを元の中身のまま残し、
// 一時ファイルを残さないことを確認する。書いている途中の失敗は起こせないので、置き換える先を削除を共有しない形で開いて
// おき、一時ファイルを書き終えた後の置き換えを OS に拒否させる。削除の関数がもうないことを成功とすること、一覧が名前の
// 順であること、session.json のフォルダーが場所を調べるだけでは作られないことも確認する。場所には ASCII でない文字を
// 含める。共有、読み取り専用の属性、環境変数の差し替え、シンボリックリンク、OS のバージョンを確認するテストは、
// Windows でだけ実行する (_WIN32 で囲み、テストスイート windows に入れる)。

#include "doctest.h"

#include "platform/platform.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <aclapi.h>
#endif

using cordmux::platform::copy_file_replace;
using cordmux::platform::delete_file;
using cordmux::platform::DirEntry;
using cordmux::platform::ffmpeg_path;
using cordmux::platform::list_directory;
using cordmux::platform::local_data_dir;
using cordmux::platform::module_dir;
using cordmux::platform::os_version_text;
using cordmux::platform::read_file_bytes;
using cordmux::platform::remove_directory_if_empty;
using cordmux::platform::write_file_atomic;
using cordmux::platform::write_file_bytes;

namespace {

constexpr std::size_t kMaxBytes = 8u * 1024u * 1024u;

// tests/test_platform_file_io.cpp と同じ 2 文字 (タイ文字とハングル。どちらも日本語のコードページで表せず、幅が決まって
// いる)。
constexpr const char *kNonAsciiPart = u8"\u0e01\ud55c";

// 一時ファイルの名前に必ず入る文字列 (platform/platform.h の、インストールで使うファイルの操作のコメントで決めた形)。
constexpr const char *kTempMarker = ".cordmux-tmp-";

class TempDir {
public:
	TempDir()
	{
		static std::atomic<int> counter{0};
		const std::string name = "cordmux_install_ops_test_" + std::to_string(++counter) + "_" +
					 std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "_" +
					 std::string(kNonAsciiPart);
		dir_ = std::filesystem::temp_directory_path() / std::filesystem::u8path(name);
		std::filesystem::create_directories(dir_);
	}

	~TempDir()
	{
		// 読み取り専用にしたファイルが残っていても削除できるように、先に書き込みを許す。
		std::error_code ec;
		for (auto it = std::filesystem::recursive_directory_iterator(dir_, ec);
		     !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
			std::error_code perm_ec;
			if (!it->is_symlink(perm_ec))
				std::filesystem::permissions(it->path(), std::filesystem::perms::owner_write,
							     std::filesystem::perm_options::add, perm_ec);
		}
		std::filesystem::remove_all(dir_, ec);
	}

	TempDir(const TempDir &) = delete;
	TempDir &operator=(const TempDir &) = delete;

	// このフォルダーの下の場所を、UTF-8 の文字列で返す (platform/platform.h の関数が受け取る形)。
	std::string utf8_path(const std::string &relative) const
	{
		return (dir_ / std::filesystem::u8path(relative)).u8string();
	}

	std::filesystem::path fs_path(const std::string &relative) const
	{
		return std::filesystem::u8path(utf8_path(relative));
	}

	std::string utf8_root() const { return dir_.u8string(); }
	const std::filesystem::path &root() const { return dir_; }

private:
	std::filesystem::path dir_;
};

// テストの側で書く (テストする関数を使わずに、前提の状態を作る)。
void put_file(const std::filesystem::path &path, const std::string &body)
{
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	f.write(body.data(), std::streamsize(body.size()));
}

std::string get_file(const std::filesystem::path &path)
{
	std::ifstream f(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// フォルダーの中の名前 (UTF-8)。テストする list_directory を使わずに数える (確認する関数自身を、確認する側に使わない)。
std::vector<std::string> names_in(const std::filesystem::path &dir)
{
	std::vector<std::string> names;
	for (const auto &entry : std::filesystem::directory_iterator(dir))
		names.push_back(entry.path().filename().u8string());
	std::sort(names.begin(), names.end());
	return names;
}

bool has_temp_leftover(const std::filesystem::path &dir)
{
	for (const std::string &name : names_in(dir)) {
		if (name.find(kTempMarker) != std::string::npos)
			return true;
	}
	return false;
}

// 0 のバイトと改行を含む、size バイトの中身。
std::string binary_body(std::size_t size)
{
	std::string body(size, '\0');
	for (std::size_t i = 0; i < size; i++)
		body[i] = char((i * 131u + 7u) & 0xffu);
	return body;
}

} // namespace

// ---------------------------------------------------------------------------
// 一時ファイルと置き換えて書く
// ---------------------------------------------------------------------------

TEST_CASE("write_file_atomic は、ファイルがなければ作り、あれば置き換えて、一時ファイルを残さない")
{
	const TempDir dir;
	const std::string name = std::string("state_") + kNonAsciiPart + ".json";
	const std::string path = dir.utf8_path(name);

	REQUIRE(write_file_atomic(path, "{\"vencordDir\":\"aaaaaaaaaaaaaaaaaaaaaaaa\"}"));
	CHECK(get_file(dir.fs_path(name)) == "{\"vencordDir\":\"aaaaaaaaaaaaaaaaaaaaaaaa\"}");

	REQUIRE(write_file_atomic(path, "{\"vencordDir\":\"b\"}"));
	CHECK(get_file(dir.fs_path(name)) == "{\"vencordDir\":\"b\"}");

	// フォルダーに残るのは、置き換えたファイルの 1 つだけ。
	CHECK(names_in(dir.root()) == std::vector<std::string>{name});
}

TEST_CASE("write_file_atomic で書いた中身は 1 バイトも変わらず、空の中身も書ける")
{
	const TempDir dir;
	std::string body = "a\nb\r\nc";
	body.push_back('\0');
	body += "d";

	REQUIRE(write_file_atomic(dir.utf8_path("bytes.bin"), body));
	std::string read_back;
	REQUIRE(read_file_bytes(dir.utf8_path("bytes.bin"), read_back, kMaxBytes));
	CHECK(read_back == body);

	REQUIRE(write_file_atomic(dir.utf8_path("bytes.bin"), std::string()));
	CHECK(std::filesystem::file_size(dir.fs_path("bytes.bin")) == 0);
	CHECK_FALSE(has_temp_leftover(dir.root()));
}

TEST_CASE("write_file_atomic は、フォルダーがない場所には書かず、フォルダーを渡されても置き換えない")
{
	const TempDir dir;
	CHECK_FALSE(write_file_atomic(dir.utf8_path("no-such-dir/inner.txt"), "x"));
	CHECK_FALSE(std::filesystem::exists(dir.fs_path("no-such-dir")));

	std::filesystem::create_directories(dir.fs_path("folder"));
	put_file(dir.fs_path("folder/keep.txt"), "keep");
	CHECK_FALSE(write_file_atomic(dir.utf8_path("folder"), "x"));
	CHECK(std::filesystem::is_directory(dir.fs_path("folder")));
	CHECK(get_file(dir.fs_path("folder/keep.txt")) == "keep");
	CHECK_FALSE(has_temp_leftover(dir.root()));

	CHECK_FALSE(write_file_atomic(std::string(), "x"));
}

TEST_CASE("write_file_bytes も write_file_atomic と同じく、一時ファイルを残さずに置き換える")
{
	const TempDir dir;
	REQUIRE(write_file_bytes(dir.utf8_path("settings.json"), "{\"a\":1}"));
	REQUIRE(write_file_bytes(dir.utf8_path("settings.json"), "{}"));
	CHECK(get_file(dir.fs_path("settings.json")) == "{}");
	CHECK(names_in(dir.root()) == std::vector<std::string>{"settings.json"});
}

// ---------------------------------------------------------------------------
// コピー
// ---------------------------------------------------------------------------

TEST_CASE("copy_file_replace は、コピー先がなければ作り、あれば置き換えて、中身がバイト単位で一致する")
{
	const TempDir dir;
	std::filesystem::create_directories(dir.fs_path("vencord-src"));
	std::filesystem::create_directories(dir.fs_path("vencord"));

	// コピーの読み込みの単位 (1 MiB) をまたぐ大きさにする (区切りで欠けないことを確認する)。
	const std::string body = binary_body(1024u * 1024u + 12345u);
	const std::string name = std::string("vencordDesktopRenderer_") + kNonAsciiPart + ".js";
	put_file(dir.fs_path("vencord-src/" + name), body);

	REQUIRE(copy_file_replace(dir.utf8_path("vencord-src/" + name), dir.utf8_path("vencord/" + name)));
	CHECK(get_file(dir.fs_path("vencord/" + name)) == body);

	// data/vencord が書き換えられた後にコピーし直すと、コピー元の中身に戻る (インストールし直すときの形)。
	put_file(dir.fs_path("vencord/" + name), "overwritten by someone else");
	REQUIRE(copy_file_replace(dir.utf8_path("vencord-src/" + name), dir.utf8_path("vencord/" + name)));
	CHECK(get_file(dir.fs_path("vencord/" + name)) == body);

	// コピー元は変わらず、どちらのフォルダーにも一時ファイルが残らない。
	CHECK(get_file(dir.fs_path("vencord-src/" + name)) == body);
	CHECK(names_in(dir.fs_path("vencord")) == std::vector<std::string>{name});
	CHECK(names_in(dir.fs_path("vencord-src")) == std::vector<std::string>{name});
}

TEST_CASE("copy_file_replace は、長さ 0 のファイルもコピーできる")
{
	const TempDir dir;
	put_file(dir.fs_path("empty.txt"), std::string());
	put_file(dir.fs_path("to.txt"), "old");
	REQUIRE(copy_file_replace(dir.utf8_path("empty.txt"), dir.utf8_path("to.txt")));
	CHECK(std::filesystem::file_size(dir.fs_path("to.txt")) == 0);
}

TEST_CASE("コピー元がないとき、コピー先のフォルダーがないとき、フォルダーを渡されたときは、copy_file_replace が失敗し、コピー先は元のまま残る")
{
	const TempDir dir;
	put_file(dir.fs_path("to.txt"), "original");

	CHECK_FALSE(copy_file_replace(dir.utf8_path("absent.txt"), dir.utf8_path("to.txt")));
	CHECK(get_file(dir.fs_path("to.txt")) == "original");

	put_file(dir.fs_path("from.txt"), "new");
	CHECK_FALSE(copy_file_replace(dir.utf8_path("from.txt"), dir.utf8_path("no-such-dir/to.txt")));
	CHECK_FALSE(std::filesystem::exists(dir.fs_path("no-such-dir")));

	std::filesystem::create_directories(dir.fs_path("folder"));
	CHECK_FALSE(copy_file_replace(dir.utf8_path("folder"), dir.utf8_path("to.txt")));
	CHECK_FALSE(copy_file_replace(dir.utf8_path("from.txt"), dir.utf8_path("folder")));
	CHECK(std::filesystem::is_directory(dir.fs_path("folder")));
	CHECK(get_file(dir.fs_path("to.txt")) == "original");
	CHECK_FALSE(has_temp_leftover(dir.root()));
}

TEST_CASE("同じ場所にコピーしても、copy_file_replace は中身を変えない")
{
	const TempDir dir;
	put_file(dir.fs_path("same.txt"), "same");
	CHECK(copy_file_replace(dir.utf8_path("same.txt"), dir.utf8_path("same.txt")));
	CHECK(get_file(dir.fs_path("same.txt")) == "same");
	CHECK_FALSE(has_temp_leftover(dir.root()));
}

// ---------------------------------------------------------------------------
// 削除
// ---------------------------------------------------------------------------

TEST_CASE("delete_file は、削除できたときも、もうないときも true を返し、フォルダーは削除しない")
{
	const TempDir dir;
	const std::string name = std::string("seen_people_") + kNonAsciiPart + ".json";
	put_file(dir.fs_path(name), "[]");

	CHECK(delete_file(dir.utf8_path(name)));
	CHECK_FALSE(std::filesystem::exists(dir.fs_path(name)));
	CHECK(delete_file(dir.utf8_path(name)));                  // やり直しても同じ結果
	CHECK(delete_file(dir.utf8_path("no-such-dir/inner.txt"))); // フォルダーごとない

	std::filesystem::create_directories(dir.fs_path("folder"));
	put_file(dir.fs_path("folder/keep.txt"), "keep");
	CHECK_FALSE(delete_file(dir.utf8_path("folder")));
	CHECK(get_file(dir.fs_path("folder/keep.txt")) == "keep");

	CHECK_FALSE(delete_file(std::string()));
}

TEST_CASE("remove_directory_if_empty は、空のフォルダーを削除し、もうなければ true を返し、中身があれば削除しない")
{
	const TempDir dir;
	std::filesystem::create_directories(dir.fs_path("empty"));
	CHECK(remove_directory_if_empty(dir.utf8_path("empty")));
	CHECK_FALSE(std::filesystem::exists(dir.fs_path("empty")));
	CHECK(remove_directory_if_empty(dir.utf8_path("empty")));

	std::filesystem::create_directories(dir.fs_path("full"));
	put_file(dir.fs_path("full/keep.txt"), "keep");
	CHECK_FALSE(remove_directory_if_empty(dir.utf8_path("full")));
	CHECK(get_file(dir.fs_path("full/keep.txt")) == "keep");

	put_file(dir.fs_path("file.txt"), "x");
	CHECK_FALSE(remove_directory_if_empty(dir.utf8_path("file.txt")));
	CHECK(std::filesystem::exists(dir.fs_path("file.txt")));
}

// ---------------------------------------------------------------------------
// 一覧
// ---------------------------------------------------------------------------

TEST_CASE("list_directory は 1 階層の中身を名前の順に返し、. と .. を含めない")
{
	const TempDir dir;
	const std::string non_ascii = std::string("z_") + kNonAsciiPart + ".txt";
	put_file(dir.fs_path("b.txt"), "b");
	put_file(dir.fs_path("a.txt"), "a");
	put_file(dir.fs_path(non_ascii), "z");
	std::filesystem::create_directories(dir.fs_path("sub/inner"));
	put_file(dir.fs_path("sub/inner/deep.txt"), "deep");

	std::vector<DirEntry> entries{DirEntry{"stale", false, false}};
	REQUIRE(list_directory(dir.utf8_root(), entries));
	REQUIRE(entries.size() == 4);
	CHECK(entries[0].name == "a.txt");
	CHECK(entries[1].name == "b.txt");
	CHECK(entries[2].name == "sub");
	CHECK(entries[3].name == non_ascii);
	CHECK_FALSE(entries[0].is_directory);
	CHECK(entries[2].is_directory);
	for (const DirEntry &entry : entries)
		CHECK_FALSE(entry.is_link);

	// 返った名前をそのまま delete_file に渡せる (ASCII でない文字も変わらない)。
	CHECK(delete_file(dir.utf8_path(entries[3].name)));
	CHECK_FALSE(std::filesystem::exists(dir.fs_path(non_ascii)));
}

TEST_CASE("list_directory は空のフォルダーに空の一覧を返し、ない場所とファイルには false を返して out を変えない")
{
	const TempDir dir;
	std::filesystem::create_directories(dir.fs_path("empty"));
	std::vector<DirEntry> entries{DirEntry{"stale", false, false}};
	REQUIRE(list_directory(dir.utf8_path("empty"), entries));
	CHECK(entries.empty());

	entries = {DirEntry{"untouched", true, false}};
	CHECK_FALSE(list_directory(dir.utf8_path("absent"), entries));
	put_file(dir.fs_path("file.txt"), "x");
	CHECK_FALSE(list_directory(dir.utf8_path("file.txt"), entries));
	REQUIRE(entries.size() == 1);
	CHECK(entries[0].name == "untouched");
}

TEST_CASE("list_directory は、末尾に区切りがある場所と / で区切った場所も、同じフォルダーとして読む")
{
	const TempDir dir;
	put_file(dir.fs_path("one.txt"), "1");
	std::vector<DirEntry> a;
	std::vector<DirEntry> b;
	REQUIRE(list_directory(dir.utf8_root() + "\\", a));
	std::string slashed = dir.utf8_root();
	std::replace(slashed.begin(), slashed.end(), '\\', '/');
	REQUIRE(list_directory(slashed, b));
	REQUIRE(a.size() == 1);
	REQUIRE(b.size() == 1);
	CHECK(a[0].name == "one.txt");
	CHECK(b[0].name == "one.txt");
}

// ---------------------------------------------------------------------------
// cordmux.dll のフォルダー
// ---------------------------------------------------------------------------

TEST_CASE("module_dir は、存在するフォルダーを末尾の区切りなしで返し、ffmpeg_path と同じフォルダーを指す")
{
	const std::string dir = module_dir();
	REQUIRE_FALSE(dir.empty());
	CHECK(dir.back() != '\\');
	CHECK(dir.back() != '/');
	CHECK(std::filesystem::is_directory(std::filesystem::u8path(dir)));

	// このテストでは、module_dir はテストの実行ファイルのフォルダーを返す。同じフォルダーに ffmpeg.exe を置いた環境では、
	// ffmpeg_path がそのフォルダーの直下を返す (調べ方が 1 か所であることを確認する)。
	const std::string ffmpeg = ffmpeg_path();
	if (!ffmpeg.empty())
		CHECK(ffmpeg.rfind(dir + "\\", 0) == 0);
}

// ---------------------------------------------------------------------------
// Windows でだけ実行するテスト
// ---------------------------------------------------------------------------

#ifdef _WIN32

namespace {

// 置き換える先を、削除を共有しない形で開いておく (ほかのツールがファイルを開いている状態)。読み取りと書き込みは共有
// するが削除は共有しないので、名前の変更による置き換えは OS に拒否される。一時ファイルを書き終えた後の置き換えで失敗
// させて、書いている途中で失敗したときと同じく、置き換える先の中身が元のままであることを確認する。
class HeldOpen {
public:
	explicit HeldOpen(const std::filesystem::path &path)
	{
		handle_ = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
				      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	}
	~HeldOpen()
	{
		if (handle_ != INVALID_HANDLE_VALUE)
			CloseHandle(handle_);
	}
	HeldOpen(const HeldOpen &) = delete;
	HeldOpen &operator=(const HeldOpen &) = delete;
	bool ok() const { return handle_ != INVALID_HANDLE_VALUE; }

private:
	HANDLE handle_ = INVALID_HANDLE_VALUE;
};

// 環境変数を差し替え、デストラクターで元に戻す (session.json のフォルダーを一時フォルダーに向けるため。
// platform/platform_win.cpp は環境変数を先に読む = profile_dir_w)。
class EnvOverride {
public:
	EnvOverride(const wchar_t *name, const std::wstring &value) : name_(name)
	{
		wchar_t buf[32768];
		const DWORD len = GetEnvironmentVariableW(name, buf, DWORD(std::size(buf)));
		if (len > 0 && len < std::size(buf)) {
			had_ = true;
			old_.assign(buf, len);
		}
		SetEnvironmentVariableW(name, value.c_str());
	}
	~EnvOverride() { SetEnvironmentVariableW(name_, had_ ? old_.c_str() : nullptr); }
	EnvOverride(const EnvOverride &) = delete;
	EnvOverride &operator=(const EnvOverride &) = delete;

private:
	const wchar_t *name_;
	bool had_ = false;
	std::wstring old_;
};

// 読み取り専用の属性を付け、デストラクターで外す (一時フォルダーを削除できるように戻す)。
class ReadOnlyMark {
public:
	explicit ReadOnlyMark(const std::filesystem::path &path) : path_(path.wstring())
	{
		SetFileAttributesW(path_.c_str(), FILE_ATTRIBUTE_READONLY);
	}
	~ReadOnlyMark() { SetFileAttributesW(path_.c_str(), FILE_ATTRIBUTE_NORMAL); }
	ReadOnlyMark(const ReadOnlyMark &) = delete;
	ReadOnlyMark &operator=(const ReadOnlyMark &) = delete;

private:
	std::wstring path_;
};

// ファイルの権限が、現在の利用者だけを許し、親から継承しない 1 つの ACE だけかを確認する (session.json の権限が、
// 置き換えた後にも残っているか)。
bool has_owner_only_acl(const std::filesystem::path &path)
{
	PACL dacl = nullptr;
	PSECURITY_DESCRIPTOR sd = nullptr;
	if (GetNamedSecurityInfoW(path.wstring().c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
				  &dacl, nullptr, &sd) != ERROR_SUCCESS)
		return false;

	bool ok = false;
	SECURITY_DESCRIPTOR_CONTROL control = 0;
	DWORD revision = 0;
	ACL_SIZE_INFORMATION size_info{};
	HANDLE token = nullptr;
	if (dacl != nullptr && GetSecurityDescriptorControl(sd, &control, &revision) &&
	    (control & SE_DACL_PROTECTED) != 0 &&
	    GetAclInformation(dacl, &size_info, sizeof(size_info), AclSizeInformation) && size_info.AceCount == 1 &&
	    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
		DWORD needed = 0;
		GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
		std::vector<std::uint8_t> buf(needed);
		void *ace = nullptr;
		if (needed > 0 && GetTokenInformation(token, TokenUser, buf.data(), needed, &needed) &&
		    GetAce(dacl, 0, &ace)) {
			const auto *allowed = static_cast<const ACCESS_ALLOWED_ACE *>(ace);
			const auto *user = reinterpret_cast<const TOKEN_USER *>(buf.data());
			ok = allowed->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
			     EqualSid(const_cast<DWORD *>(&allowed->SidStart), user->User.Sid);
		}
		CloseHandle(token);
	}
	LocalFree(sd);
	return ok;
}

bool starts_with(const std::string &text, const std::string &head)
{
	return text.size() >= head.size() && text.compare(0, head.size(), head) == 0;
}

} // namespace

TEST_CASE("置き換えを OS に拒否されても、write_file_atomic は元のファイルを元の中身のまま残す" *
	  doctest::test_suite("windows"))
{
	const TempDir dir;
	const std::string name = std::string("state_") + kNonAsciiPart + ".json";
	const std::string original = "{\"vencordDir\":\"C:\\\\original\"}";
	put_file(dir.fs_path(name), original);

	{
		const HeldOpen held(dir.fs_path(name));
		REQUIRE(held.ok());
		CHECK_FALSE(write_file_atomic(dir.utf8_path(name), "{\"vencordDir\":\"C:\\\\new\"}"));
		// 開かれている間も中身は元のまま (その場で切り詰めて書く実装なら、ここで 0 バイトになる)。
		CHECK(get_file(dir.fs_path(name)) == original);
	}

	CHECK(get_file(dir.fs_path(name)) == original);
	CHECK(names_in(dir.root()) == std::vector<std::string>{name}); // 一時ファイルを残さない

	// 閉じれば書ける (失敗の影響が後に残らない)。
	REQUIRE(write_file_atomic(dir.utf8_path(name), "{}"));
	CHECK(get_file(dir.fs_path(name)) == "{}");
}

TEST_CASE("置き換えを OS に拒否されても、write_file_bytes は元のファイルを元の中身のまま残す" *
	  doctest::test_suite("windows"))
{
	const TempDir dir;
	put_file(dir.fs_path("vencord_dir_backup.json"), "{\"written\":\"keep\"}");
	{
		const HeldOpen held(dir.fs_path("vencord_dir_backup.json"));
		REQUIRE(held.ok());
		CHECK_FALSE(write_file_bytes(dir.utf8_path("vencord_dir_backup.json"), "{\"written\":\"new\"}"));
	}
	CHECK(get_file(dir.fs_path("vencord_dir_backup.json")) == "{\"written\":\"keep\"}");
	CHECK_FALSE(has_temp_leftover(dir.root()));
}

TEST_CASE("置き換えを OS に拒否されても、copy_file_replace はコピー先を元の中身のまま残す" *
	  doctest::test_suite("windows"))
{
	const TempDir dir;
	put_file(dir.fs_path("from.js"), binary_body(4096));
	put_file(dir.fs_path("to.js"), "shown");
	{
		const HeldOpen held(dir.fs_path("to.js"));
		REQUIRE(held.ok());
		CHECK_FALSE(copy_file_replace(dir.utf8_path("from.js"), dir.utf8_path("to.js")));
	}
	CHECK(get_file(dir.fs_path("to.js")) == "shown");
	CHECK_FALSE(has_temp_leftover(dir.root()));
}

TEST_CASE("copy_file_replace は、コピー元の読み取り専用の属性をコピーしない (次の置き換えが拒否されない)" *
	  doctest::test_suite("windows"))
{
	const TempDir dir;
	put_file(dir.fs_path("from.css"), "body{}");
	const ReadOnlyMark mark(dir.fs_path("from.css"));
	const DWORD from_attributes = GetFileAttributesW(dir.fs_path("from.css").wstring().c_str());
	REQUIRE((from_attributes & FILE_ATTRIBUTE_READONLY) != 0);

	REQUIRE(copy_file_replace(dir.utf8_path("from.css"), dir.utf8_path("to.css")));
	const DWORD to_attributes = GetFileAttributesW(dir.fs_path("to.css").wstring().c_str());
	CHECK((to_attributes & FILE_ATTRIBUTE_READONLY) == 0);

	// 2 回目も置き換えられる。
	CHECK(copy_file_replace(dir.utf8_path("from.css"), dir.utf8_path("to.css")));
	CHECK(get_file(dir.fs_path("to.css")) == "body{}");
}

TEST_CASE("読み取り専用の属性が付いたファイルは、delete_file が削除せずに false を返す" *
	  doctest::test_suite("windows"))
{
	const TempDir dir;
	put_file(dir.fs_path("locked.json"), "{}");
	const ReadOnlyMark mark(dir.fs_path("locked.json"));
	REQUIRE((GetFileAttributesW(dir.fs_path("locked.json").wstring().c_str()) & FILE_ATTRIBUTE_READONLY) != 0);
	CHECK_FALSE(delete_file(dir.utf8_path("locked.json")));
	CHECK(std::filesystem::exists(dir.fs_path("locked.json")));
}

TEST_CASE("シンボリックリンクは list_directory で is_link が true になり、リンクを削除しても指している先は残る" *
	  doctest::test_suite("windows"))
{
	const TempDir dir;
	std::filesystem::create_directories(dir.fs_path("outside"));
	put_file(dir.fs_path("outside/user_file.txt"), "user");
	std::filesystem::create_directories(dir.fs_path("config"));

	std::error_code ec;
	std::filesystem::create_directory_symlink(dir.fs_path("outside"), dir.fs_path("config/link"), ec);
	if (ec) {
		// シンボリックリンクを作るには、開発者モードか管理者の権限が必要である。ない環境では確認できない。
		MESSAGE("記号の繋がりを作れない環境なので飛ばす: " << ec.message());
		return;
	}

	std::vector<DirEntry> entries;
	REQUIRE(list_directory(dir.utf8_path("config"), entries));
	REQUIRE(entries.size() == 1);
	CHECK(entries[0].name == "link");
	CHECK(entries[0].is_directory);
	CHECK(entries[0].is_link);

	// リンクを削除する関数 (フォルダーを指すリンクは remove_directory_if_empty) で削除すると、指している先の中身には
	// 触らない。
	CHECK(remove_directory_if_empty(dir.utf8_path("config/link")));
	CHECK_FALSE(std::filesystem::exists(dir.fs_path("config/link")));
	CHECK(get_file(dir.fs_path("outside/user_file.txt")) == "user");
}

TEST_CASE("user_profile_dir は環境変数の USERPROFILE を先に読む" * doctest::test_suite("windows"))
{
	const TempDir dir;
	const EnvOverride env(L"USERPROFILE", dir.root().wstring());
	CHECK(cordmux::platform::user_profile_dir() == dir.utf8_root());
}

TEST_CASE("local_data_dir は場所を返すだけでフォルダーを作らず、write_session_file がそのフォルダーを作る" *
	  doctest::test_suite("windows"))
{
	const TempDir dir;
	const EnvOverride env(L"LOCALAPPDATA", dir.root().wstring());

	const std::string data_dir = local_data_dir();
	REQUIRE(data_dir == dir.utf8_root() + "\\cordmux");
	CHECK_FALSE(std::filesystem::exists(dir.fs_path("cordmux")));
	CHECK(local_data_dir() == data_dir); // 2 回呼んでも作らない
	CHECK_FALSE(std::filesystem::exists(dir.fs_path("cordmux")));

	// ここから先は、実際の session.json に触らないことを確認してから書く (差し替えが動作していない環境で、利用者の
	// トークンを上書きしない)。
	REQUIRE(starts_with(data_dir, dir.utf8_root()));

	cordmux::platform::SessionInfo info;
	info.port = 45678;
	info.secret_hex = std::string(64, 'a');
	info.writer_pid = 1234;
	info.writer_started_at = 5678;
	REQUIRE(cordmux::platform::write_session_file(info));
	CHECK(names_in(dir.fs_path("cordmux")) == std::vector<std::string>{"session.json"});
	// 権限は一時ファイルに付けてから置き換える。置き換えた後のファイルに権限が残っていること。
	CHECK(has_owner_only_acl(dir.fs_path("cordmux/session.json")));

	// 書き直しても一時ファイルを残さず、権限も残り、読める。
	info.port = 45679;
	REQUIRE(cordmux::platform::write_session_file(info));
	CHECK(names_in(dir.fs_path("cordmux")) == std::vector<std::string>{"session.json"});
	CHECK(has_owner_only_acl(dir.fs_path("cordmux/session.json")));
	cordmux::platform::SessionInfo read_back;
	REQUIRE(cordmux::platform::read_session_file(read_back));
	CHECK(read_back.port == 45679);
	CHECK(read_back.secret_hex == info.secret_hex);

	// アンインストールと同じく、中身を削除してからフォルダーを削除すると、場所を調べ直してもフォルダーは作られない。
	CHECK(delete_file(data_dir + "\\session.json"));
	CHECK(remove_directory_if_empty(data_dir));
	CHECK(local_data_dir() == data_dir);
	CHECK_FALSE(std::filesystem::exists(dir.fs_path("cordmux")));
}

TEST_CASE("delete_session_file は session.json だけを削除し、もうなければ成功とし、フォルダーを作らない" *
	  doctest::test_suite("windows"))
{
	const TempDir dir;
	const EnvOverride env(L"LOCALAPPDATA", dir.root().wstring());
	const std::string data_dir = local_data_dir();
	REQUIRE(starts_with(data_dir, dir.utf8_root()));

	// フォルダーがないときは、何もせず成功とし、フォルダーを作らない。
	CHECK(cordmux::platform::delete_session_file());
	CHECK_FALSE(std::filesystem::exists(dir.fs_path("cordmux")));

	cordmux::platform::SessionInfo info;
	info.port = 45678;
	info.secret_hex = std::string(64, 'a');
	info.writer_pid = 1234;
	info.writer_started_at = 5678;
	REQUIRE(cordmux::platform::write_session_file(info));
	std::filesystem::create_directories(dir.fs_path("cordmux"));
	put_file(dir.fs_path("cordmux/other.txt"), "keep");

	CHECK(cordmux::platform::delete_session_file());
	CHECK(names_in(dir.fs_path("cordmux")) == std::vector<std::string>{"other.txt"});
	CHECK(cordmux::platform::delete_session_file()); // もうない
}

TEST_CASE("session.json の数のフィールドが壊れていても、read_session_file は例外を投げずに false を返す" *
	  doctest::test_suite("windows"))
{
	// session.json は OBS がプラグインを読み込む途中で読む。壊れた中身 (書きかけやディスクのエラー) で std::stoll が
	// 例外を投げると OBS がクラッシュするので、どの形でも false を返すことを確認する。
	const TempDir dir;
	const EnvOverride env(L"LOCALAPPDATA", dir.root().wstring());
	const std::string data_dir = local_data_dir();
	// 実際の session.json に触らないことを確認してから書く (上のテストと同じ)。
	REQUIRE(starts_with(data_dir, dir.utf8_root()));
	std::filesystem::create_directories(dir.fs_path("cordmux"));

	const std::string secret = "\"secret\":\"" + std::string(64, 'a') + "\"";
	struct Case {
		const char *what;
		std::string text;
	};
	const Case cases[] = {
		{"64 ビットに収まらない桁", "{\"port\":99999999999999999999999," + secret + ",\"pid\":1,\"started_at\":2}"},
		{"符号だけ", "{\"port\":-," + secret + ",\"pid\":1,\"started_at\":2}"},
		{"符号が 2 つ", "{\"port\":--5," + secret + ",\"pid\":1,\"started_at\":2}"},
		{"数の途中に符号", "{\"port\":12-3," + secret + ",\"pid\":1,\"started_at\":2}"},
		{"番号が 16 ビットの外", "{\"port\":70000," + secret + ",\"pid\":1,\"started_at\":2}"},
		{"負の番号", "{\"port\":-1," + secret + ",\"pid\":1,\"started_at\":2}"},
		{"pid が 32 ビットの外", "{\"port\":45678," + secret + ",\"pid\":4294967296,\"started_at\":2}"},
	};
	for (const Case &c : cases) {
		CAPTURE(c.what);
		{
			std::ofstream file(dir.fs_path("cordmux/session.json"), std::ios::binary | std::ios::trunc);
			file << c.text;
		}
		cordmux::platform::SessionInfo info;
		bool read = true;
		CHECK_NOTHROW(read = cordmux::platform::read_session_file(info));
		CHECK_FALSE(read);
	}

	// 正しい中身は読める (範囲の端の値も)。
	{
		std::ofstream file(dir.fs_path("cordmux/session.json"), std::ios::binary | std::ios::trunc);
		file << "{\"port\":65535," + secret + ",\"pid\":4294967295,\"started_at\":-5}";
	}
	cordmux::platform::SessionInfo info;
	REQUIRE(cordmux::platform::read_session_file(info));
	CHECK(info.port == 65535);
	CHECK(info.writer_pid == 4294967295u);
	CHECK(info.writer_started_at == -5);
}

TEST_CASE("os_version_text は、4 つの数をピリオドでつないだ形で返す" * doctest::test_suite("windows"))
{
	const std::string text = os_version_text();
	REQUIRE_FALSE(text.empty());

	std::vector<std::string> parts(1);
	for (const char c : text) {
		if (c == '.') {
			parts.emplace_back();
			continue;
		}
		REQUIRE(c >= '0');
		REQUIRE(c <= '9');
		parts.back().push_back(c);
	}
	REQUIRE(parts.size() == 4);
	for (const std::string &part : parts)
		CHECK_FALSE(part.empty());
	CHECK(std::stoi(parts[0]) >= 10);    // cordmux が動く Windows は 10 以上
	CHECK(std::stoi(parts[2]) >= 10240); // Windows 10 の最初のビルドより前はない
}

#endif // _WIN32
