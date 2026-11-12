// SPDX-License-Identifier: Apache-2.0

// platform/platform.h の is_discord_client_exe_name (Vesktop と Discord のデスクトップクライアントの実行ファイルの
// 名前を見分ける関数) の単体テスト。OS にはアクセスしない関数なので、名前の文字列だけを渡す。

#include "doctest.h"

#include "platform/platform.h"

#include <string>

using cordmux::platform::is_discord_client_exe_name;

TEST_CASE("is_discord_client_exe_name は Vesktop と、Discord の安定版、PTB、Canary の実行ファイルの名前を、大文字と小文字を区別せずに見分ける")
{
	CHECK(is_discord_client_exe_name("Vesktop.exe"));
	CHECK(is_discord_client_exe_name("Discord.exe"));
	CHECK(is_discord_client_exe_name("DiscordPTB.exe"));
	CHECK(is_discord_client_exe_name("DiscordCanary.exe"));
	CHECK(is_discord_client_exe_name("vesktop.EXE"));
	CHECK(is_discord_client_exe_name("DISCORDCANARY.EXE"));
}

TEST_CASE("is_discord_client_exe_name は、ほかの実行ファイルの名前、拡張子のない名前、空の名前、UTF-8 として読めない名前に false を返す")
{
	CHECK_FALSE(is_discord_client_exe_name(""));
	CHECK_FALSE(is_discord_client_exe_name("Vesktop"));
	CHECK_FALSE(is_discord_client_exe_name("Discord.exe.bak"));
	CHECK_FALSE(is_discord_client_exe_name("obs64.exe"));
	CHECK_FALSE(is_discord_client_exe_name("chrome.exe"));
	CHECK_FALSE(is_discord_client_exe_name(std::string("Vesktop\xff.exe")));
}
