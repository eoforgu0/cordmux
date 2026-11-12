// SPDX-License-Identifier: Apache-2.0

// 接続の認証に使うトークン。32 バイトの乱数の 16 進表記を作る関数と、定数時間で比べる関数。
// トークンは起動のたびに作り直す。
//
// 定数時間で比べても、タイミング攻撃を防げる相手はいない。応答の時間を測れるのは同じ PC で動くプログラムで、
// そのプログラムはトークンのファイル (session.json) を直接読めるためである。費用がほとんどないので、定数時間で
// 比べている。

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <string_view>

namespace cordmux::ws {

inline constexpr size_t kSecretBytes = 32;
inline constexpr size_t kSecretHexChars = kSecretBytes * 2;

// 既定の乱数のバイト。Microsoft のドキュメントは、MSVC の std::random_device が暗号に使える乱数を返すとしている。
// ほかの標準ライブラリの実装では確認していない。
inline uint8_t default_random_byte()
{
	static thread_local std::random_device rd;
	return uint8_t(rd() & 0xFF);
}

// 32 バイトの乱数を 16 進 (小文字、64 文字) にして返す。テストでは byte_source を差し替える。
inline std::string generate_secret_hex(const std::function<uint8_t()> &byte_source = default_random_byte)
{
	static const char *kHex = "0123456789abcdef";
	std::string out;
	out.resize(kSecretHexChars);
	for (size_t i = 0; i < kSecretBytes; i++) {
		const uint8_t b = byte_source();
		out[i * 2] = kHex[b >> 4];
		out[i * 2 + 1] = kHex[b & 0x0F];
	}
	return out;
}

// a と b が等しければ true を返す。長さが違っても、常に長いほうの長さだけ比べてから返す。一致したかどうかに加えて、
// 最初に違った位置も処理の時間に表れない。
inline bool constant_time_equals(std::string_view a, std::string_view b)
{
	const size_t n = a.size() > b.size() ? a.size() : b.size();
	unsigned char diff = (a.size() == b.size()) ? 0 : 1;
	for (size_t i = 0; i < n; i++) {
		const unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
		const unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
		diff = static_cast<unsigned char>(diff | (ca ^ cb));
	}
	return diff == 0;
}

} // namespace cordmux::ws
