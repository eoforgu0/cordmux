// SPDX-License-Identifier: Apache-2.0

// platform/platform.h の、Vesktop の設定の場所の決め方の単体テスト。Vesktop v1.6.9 の src/main/constants.ts の DATA_DIR と
// 同じ順で決めること (環境変数 VENCORD_USER_DATA_DIR があればそこ、なければ RoamingAppData の既知のフォルダーの下の Vesktop)、
// 環境変数 APPDATA を読まないこと、ポータブル版の Vesktop の実行ファイルを、Vesktop と同じ規則で見分けることを確認する。
// 環境変数の差し替えと実際のファイルを使うので、Windows でだけ実行する (テストスイート windows)。ファイルは、テストを
// 実行したフォルダーの下の一時のフォルダーに作り、終わったら消す。

#include "doctest.h"

#include "platform/platform.h"

#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {

// 環境変数を差し替え、デストラクターで元に戻す。value が nullptr なら、その環境変数を消す。
class EnvSwap {
public:
	EnvSwap(const wchar_t *name, const wchar_t *value) : name_(name)
	{
		wchar_t buf[32768];
		const DWORD len = GetEnvironmentVariableW(name, buf, DWORD(sizeof(buf) / sizeof(buf[0])));
		if (len > 0 && len < sizeof(buf) / sizeof(buf[0])) {
			had_ = true;
			old_.assign(buf, len);
		}
		SetEnvironmentVariableW(name, value);
	}
	~EnvSwap() { SetEnvironmentVariableW(name_, had_ ? old_.c_str() : nullptr); }
	EnvSwap(const EnvSwap &) = delete;
	EnvSwap &operator=(const EnvSwap &) = delete;

private:
	const wchar_t *name_;
	bool had_ = false;
	std::wstring old_;
};

// 実行したフォルダーの下に一時のフォルダーを作り、終わったら消す。
class WorkDir {
public:
	WorkDir()
	{
		dir_ = std::filesystem::current_path() / L"vesktop_dir_test_tmp";
		std::error_code ec;
		std::filesystem::remove_all(dir_, ec);
		std::filesystem::create_directories(dir_);
	}
	~WorkDir()
	{
		std::error_code ec;
		std::filesystem::remove_all(dir_, ec);
	}
	WorkDir(const WorkDir &) = delete;
	WorkDir &operator=(const WorkDir &) = delete;

	const std::filesystem::path &path() const { return dir_; }
	void touch(const std::wstring &name) const { std::ofstream(dir_ / name).put('x'); }

private:
	std::filesystem::path dir_;
};

bool ends_with(const std::string &text, const std::string &suffix)
{
	return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

TEST_CASE("Vesktop の設定の場所: VENCORD_USER_DATA_DIR があればその下を使う" * doctest::test_suite("windows"))
{
	const EnvSwap env(L"VENCORD_USER_DATA_DIR", L"C:\\fake\\data");
	std::string dir, state, settings;
	REQUIRE(cordmux::platform::vesktop_data_dir(dir));
	REQUIRE(cordmux::platform::vesktop_state_json_path(state));
	REQUIRE(cordmux::platform::vesktop_settings_json_path(settings));
	CHECK(dir == "C:\\fake\\data");
	CHECK(state == "C:\\fake\\data\\state.json");
	CHECK(settings == "C:\\fake\\data\\settings\\settings.json");
}

TEST_CASE("Vesktop の設定の場所: 末尾の区切りは除く。ドライブのルートは除かない" * doctest::test_suite("windows"))
{
	std::string dir;
	{
		const EnvSwap env(L"VENCORD_USER_DATA_DIR", L"C:\\fake\\data\\");
		REQUIRE(cordmux::platform::vesktop_data_dir(dir));
		CHECK(dir == "C:\\fake\\data");
	}
	{
		const EnvSwap env(L"VENCORD_USER_DATA_DIR", L"C:\\");
		REQUIRE(cordmux::platform::vesktop_data_dir(dir));
		CHECK(dir == "C:\\");
	}
}

TEST_CASE("Vesktop の設定の場所: 環境変数 APPDATA は読まない。なければ既知のフォルダーの下の Vesktop" *
	  doctest::test_suite("windows"))
{
	// APPDATA を実際の場所とは別の場所に向け、VENCORD_USER_DATA_DIR を消す。製品が APPDATA を読むと、ここに向く。
	const EnvSwap appdata(L"APPDATA", L"C:\\fake\\not-the-real-appdata");
	const EnvSwap data_dir(L"VENCORD_USER_DATA_DIR", nullptr);
	std::string dir, state, settings;
	REQUIRE(cordmux::platform::vesktop_data_dir(dir));
	REQUIRE(cordmux::platform::vesktop_state_json_path(state));
	REQUIRE(cordmux::platform::vesktop_settings_json_path(settings));
	CHECK(dir.find("not-the-real-appdata") == std::string::npos);
	CHECK(ends_with(dir, "\\Vesktop"));
	CHECK(state == dir + "\\state.json");
	CHECK(settings == dir + "\\settings\\settings.json");
}

TEST_CASE("Vesktop の設定の場所: 空の VENCORD_USER_DATA_DIR は未設定と同じ" * doctest::test_suite("windows"))
{
	const EnvSwap data_dir(L"VENCORD_USER_DATA_DIR", L"");
	std::string dir;
	REQUIRE(cordmux::platform::vesktop_data_dir(dir));
	CHECK(ends_with(dir, "\\Vesktop"));
}

TEST_CASE("Vesktop の設定の場所: 長い VENCORD_USER_DATA_DIR もそのまま使う (既知のフォルダーに落とさない)" *
	  doctest::test_suite("windows"))
{
	const std::wstring long_dir = L"C:\\fake\\" + std::wstring(300, L'a');
	const EnvSwap env(L"VENCORD_USER_DATA_DIR", long_dir.c_str());
	std::string dir;
	REQUIRE(cordmux::platform::vesktop_data_dir(dir));
	CHECK(dir == std::string("C:\\fake\\") + std::string(300, 'a'));
}

TEST_CASE("vesktop_settings_json_beside: state.json と同じフォルダーの settings/settings.json" * doctest::test_suite("windows"))
{
	CHECK(cordmux::platform::vesktop_settings_json_beside("C:\\a\\Vesktop\\state.json") ==
	      "C:\\a\\Vesktop\\settings\\settings.json");
	CHECK(cordmux::platform::vesktop_settings_json_beside("C:/a/Vesktop/state.json") ==
	      "C:/a/Vesktop\\settings\\settings.json");
	CHECK(cordmux::platform::vesktop_settings_json_beside("state.json").empty());
	CHECK(cordmux::platform::vesktop_settings_json_beside("").empty());
}

TEST_CASE("vesktop_exe_is_portable: 隣に Uninstall Vesktop.exe がなければポータブル版" * doctest::test_suite("windows"))
{
	const WorkDir work;
	const std::string exe = (work.path() / L"Vesktop.exe").u8string();

	// インストーラーで入れた Vesktop は、exe の隣に Uninstall Vesktop.exe がある。
	work.touch(L"Uninstall Vesktop.exe");
	CHECK_FALSE(cordmux::platform::vesktop_exe_is_portable(exe));

	// ない (ポータブル版)。
	std::filesystem::remove(work.path() / L"Uninstall Vesktop.exe");
	CHECK(cordmux::platform::vesktop_exe_is_portable(exe));

	// 開発用の electron.exe での起動は、隣に何がなくてもポータブル版とは言わない (Vesktop の PORTABLE と同じ)。
	CHECK_FALSE(cordmux::platform::vesktop_exe_is_portable((work.path() / L"electron.exe").u8string()));
	CHECK_FALSE(cordmux::platform::vesktop_exe_is_portable((work.path() / L"Electron.EXE").u8string()));

	// 場所の形でないものは、ポータブル版とは言わない。
	CHECK_FALSE(cordmux::platform::vesktop_exe_is_portable("Vesktop.exe"));
	CHECK_FALSE(cordmux::platform::vesktop_exe_is_portable(""));
}

#endif
