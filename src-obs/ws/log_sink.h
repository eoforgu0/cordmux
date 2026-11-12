// SPDX-License-Identifier: Apache-2.0

// ws/ がログの行を渡す出力先の型と、文字列にトークンかポート番号が含まれているかを調べる関数。
// ws/ のコードは、トークンとポート番号をログの行に入れない。text_reveals_secret_or_port は、plugin-main.cpp が
// ログの行と診断情報をチェックするのに使い、tests/test_ws_listener.cpp が Listener のログの全文を確認するのに使う。

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace cordmux::ws {

// 1 行のログを受け取る関数。ws/ がログを書くのは、この関数だけである。
using LogSink = std::function<void(std::string_view line)>;

inline void null_log_sink(std::string_view) {}

// text の中に secret_hex (トークン) か port (ポート番号の 10 進表記) が含まれていれば true を返す。
// secret_hex が空のときはトークンを調べず、port が 0 のときはポート番号を調べない。空の文字列はどこにでも
// 一致し、0 はまだポートを開いていないことを示す値なので、調べると誤って検出する。
inline bool text_reveals_secret_or_port(std::string_view text, std::string_view secret_hex, uint16_t port)
{
	if (!secret_hex.empty() && text.find(secret_hex) != std::string_view::npos)
		return true;
	if (port != 0) {
		const std::string port_str = std::to_string(port);
		if (text.find(port_str) != std::string_view::npos)
			return true;
	}
	return false;
}

} // namespace cordmux::ws
