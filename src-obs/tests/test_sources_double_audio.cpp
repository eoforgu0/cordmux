// SPDX-License-Identifier: Apache-2.0

// sources/double_audio.h の純粋関数 (アプリケーション音声キャプチャの設定から実行ファイルの名前を取り出す関数) の
// テスト。設定の値は、OBS 31.1.1 の libobs/util/windows/window-helpers.c が保存する形 (':' を "#3A" に、'#' を
// "#22" に置き換えたもの) で書く。最後のテストは、取り出した名前を platform::is_discord_client_exe_name に渡して、
// 取り込む先が Vesktop か Discord かを見分けられることを確認する。

#include "doctest.h"

#include "platform/platform.h"
#include "sources/double_audio.h"

#include <string>

using cordmux::platform::is_discord_client_exe_name;
using cordmux::sources::window_setting_exe;

TEST_CASE("window_setting_exe はタイトル、クラス、実行ファイル名の 3 つ目のフィールドを返す")
{
	CHECK(window_setting_exe("Vesktop:Chrome_WidgetWin_1:Vesktop.exe") == "Vesktop.exe");
	CHECK(window_setting_exe("Discord:Chrome_WidgetWin_1:Discord.exe") == "Discord.exe");
	// 4 つ目からのフィールドは読まない (OBS も 3 つ目までしか読まない)。
	CHECK(window_setting_exe("Vesktop:Chrome_WidgetWin_1:Vesktop.exe:extra") == "Vesktop.exe");
}

TEST_CASE("window_setting_exe は、タイトルの中の #3A と #22 を区切りと読まず、実行ファイル名の中の #3A と #22 を元の文字に戻す")
{
	// タイトル "#general: alice" は "#22general#3A alice" の形で保存される。
	CHECK(window_setting_exe("#22general#3A alice:Chrome_WidgetWin_1:Vesktop.exe") == "Vesktop.exe");
	CHECK(window_setting_exe("t:c:a#3Ab#22c.exe") == "a:b#c.exe");
	// 元の "#3A" は "#223A" の形で保存される。"#3A" を先に戻すので、':' にはならない。
	CHECK(window_setting_exe("t:c:#223A.exe") == "#3A.exe");
	CHECK(window_setting_exe("t:c:#22#3A") == "#:");
}

TEST_CASE("window_setting_exe は、フィールドが 3 つに足りない値と空の値には空を返し、空のフィールドも 1 つと数える")
{
	CHECK(window_setting_exe("").empty());
	CHECK(window_setting_exe("Vesktop.exe").empty());
	CHECK(window_setting_exe("Vesktop:Chrome_WidgetWin_1").empty());
	CHECK(window_setting_exe(":").empty());
	// 空のフィールドも数えるので、タイトルとクラスが空でも実行ファイル名を読める。
	CHECK(window_setting_exe("::Vesktop.exe") == "Vesktop.exe");
	// 実行ファイル名のフィールドが空なら、空を返す。
	CHECK(window_setting_exe("Vesktop:Chrome_WidgetWin_1:").empty());
}

TEST_CASE("アプリケーション音声キャプチャの設定から、取り込む先が Vesktop か Discord かを読める")
{
	CHECK(is_discord_client_exe_name(window_setting_exe("#22general#3A alice:Chrome_WidgetWin_1:Vesktop.exe")));
	CHECK(is_discord_client_exe_name(window_setting_exe("Discord:Chrome_WidgetWin_1:DiscordPTB.exe")));
	CHECK_FALSE(is_discord_client_exe_name(window_setting_exe("Example Game:UnityWndClass:game.exe")));
	// まだ取り込む先を選んでいないソースの設定は空である。
	CHECK_FALSE(is_discord_client_exe_name(window_setting_exe("")));
}
