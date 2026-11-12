// SPDX-License-Identifier: Apache-2.0

// ws/log_sink.h の text_reveals_secret_or_port の単体テスト。トークンとポート番号を含まない文字列では検出せず、
// わざと含めた文字列では検出することを確認する。Listener のログの全文にトークンとポート番号が含まれないことは、
// tests/test_ws_listener.cpp が同じ関数で確認する。

#include "doctest.h"

#include "ws/log_sink.h"

using cordmux::ws::text_reveals_secret_or_port;

TEST_CASE("トークンとポート番号を含まないログには false を返す")
{
	const std::string secret = "a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f9";
	const std::string log = "hello: トークンが一致した。接続を確立した\nopen: 接続を受け付けた\n";
	CHECK_FALSE(text_reveals_secret_or_port(log, secret, 4470));
}

TEST_CASE("トークンを含むログには true を返す")
{
	const std::string secret = "a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f9";
	const std::string leaking_log = "hello: トークン " + secret + " を受け取った";
	CHECK(text_reveals_secret_or_port(leaking_log, secret, 4470));
}

TEST_CASE("ポート番号を含むログには true を返す")
{
	const std::string secret = "deadbeef";
	const std::string leaking_log = "try_open: ポート 47531 で待ち受けを開いた";
	CHECK(text_reveals_secret_or_port(leaking_log, secret, 47531));
}

TEST_CASE("port が 0 のときは、ポート番号を調べない")
{
	// 0 はまだポートを開いていないことを示す値で、ポート番号ではない。ログに 0 という数字があっても検出しない。
	CHECK_FALSE(text_reveals_secret_or_port("エラーコード 0 が返った", "secret", 0));
}

TEST_CASE("secret_hex が空のときは、トークンを調べない (空の文字列はどこにでも一致するため)")
{
	CHECK_FALSE(text_reveals_secret_or_port("何でもない記録", "", 4470));
}
