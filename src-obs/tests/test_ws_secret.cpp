// SPDX-License-Identifier: Apache-2.0

// ws/secret.h の単体テスト。トークンの作り方と、定数時間の比較を確認する。

#include "doctest.h"

#include "ws/secret.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>

using cordmux::ws::constant_time_equals;
using cordmux::ws::generate_secret_hex;
using cordmux::ws::kSecretHexChars;

TEST_CASE("generate_secret_hex は小文字の 16 進で 64 文字のトークンを返し、呼ぶたびに値が変わる")
{
	const std::string a = generate_secret_hex();
	const std::string b = generate_secret_hex();

	CHECK(a.size() == kSecretHexChars);
	CHECK(b.size() == kSecretHexChars);
	CHECK(a != b); // 乱数なので、偶然に一致する確率は無視できる

	for (const char c : a)
		CHECK(std::isxdigit(static_cast<unsigned char>(c)));
	CHECK(a == [&] {
		std::string lower = a;
		std::transform(lower.begin(), lower.end(), lower.begin(),
		               [](unsigned char c) { return char(std::tolower(c)); });
		return lower;
	}());
}

TEST_CASE("generate_secret_hex は byte_source から 32 バイトを読み、その順に 16 進にする")
{
	int calls = 0;
	const std::string s = generate_secret_hex([&calls]() -> uint8_t { return uint8_t(calls++); });
	CHECK(calls == 32);
	CHECK(s == "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
}

TEST_CASE("constant_time_equals は内容が同じ文字列に true を返す")
{
	CHECK(constant_time_equals("", ""));
	CHECK(constant_time_equals("abc", "abc"));
	CHECK(constant_time_equals(std::string(64, 'f'), std::string(64, 'f')));
}

TEST_CASE("constant_time_equals は、長さが同じでも違っても、内容が違う文字列に false を返す")
{
	CHECK_FALSE(constant_time_equals("abc", "abd"));
	CHECK_FALSE(constant_time_equals("abc", "ab"));
	CHECK_FALSE(constant_time_equals("ab", "abc"));
	CHECK_FALSE(constant_time_equals("", "a"));
	CHECK_FALSE(constant_time_equals("a", ""));
}

TEST_CASE("constant_time_equals は最初の文字だけ違う場合も最後の文字だけ違う場合も false を返す")
{
	// 処理の時間は測らない (理由は ws/secret.h の先頭のコメント)。途中で比較を打ち切っていても、この 2 つの
	// 結果は false になる。打ち切っていないことは、実装を読んで確認する。
	const std::string a(1000, 'x');
	std::string b_first_diff = a;
	b_first_diff[0] = 'y';
	std::string b_last_diff = a;
	b_last_diff[999] = 'y';

	CHECK_FALSE(constant_time_equals(a, b_first_diff));
	CHECK_FALSE(constant_time_equals(a, b_last_diff));
}
