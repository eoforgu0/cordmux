// SPDX-License-Identifier: Apache-2.0

// SHA-256 を計算する。同梱の Vencord のファイル (data/vencord) が、コピー元 (data/vencord-src) と同じかを確認するのに
// 使う (platform/bundle_check.h)。OS にもライブラリにも依存しない。実装は FIPS 180-4 の 6.2 の手順のままで、読んで
// 確認できるように最適化はしない。

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace cordmux::platform {

namespace detail {

// FIPS 180-4 の 4.2.2 の定数 (最初の 64 個の素数の立方根の、小数部の先頭 32 ビット)。
inline constexpr std::uint32_t kSha256K[64] = {
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
	0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
	0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
	0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
	0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
	0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
	0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
	0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

inline std::uint32_t sha256_rotr(std::uint32_t x, int n)
{
	return (x >> n) | (x << (32 - n));
}

} // namespace detail

// データを少しずつ渡して、最後に 1 つのハッシュを返す (ファイル全体をメモリに読み込まずに計算するため)。同じ入力
// なら、どこで区切って渡しても同じ値になる (単体テストが確認する)。
//
// OS の暗号の API を使わずに自分で実装している。OS の API を使うと Windows でしか動かず、照合の関数を単体テストで
// 確認できなくなる。依存しているライブラリ (websocketpp、asio、nlohmann/json) には SHA-256 の実装がない
// (websocketpp にあるのは、ハンドシェイク用の SHA-1 である)。正しさは、公表されているテストベクターで確認する
// (tests/test_platform_bundle_check.cpp)。照合するのは cordmux が配布したファイルで、一致しなければ
// インストールし直すよう勧めるだけなので、衝突を作ろうとする攻撃への強さは求めない。
class Sha256 {
public:
	void update(const void *data, std::size_t len)
	{
		const auto *bytes = static_cast<const unsigned char *>(data);
		for (std::size_t i = 0; i < len; ++i) {
			buffer_[buffer_len_++] = bytes[i];
			if (buffer_len_ == 64) {
				compress(buffer_);
				buffer_len_ = 0;
			}
		}
		total_bytes_ += len;
	}

	void update(std::string_view data) { update(data.data(), data.size()); }

	// ハッシュを 16 進の 64 文字 (小文字) で返す。呼んだ後のオブジェクトは使い回さない。
	std::string final_hex()
	{
		const std::uint64_t total_bits = total_bytes_ * 8;

		// FIPS 180-4 の 5.1.1 のパディング。0x80 を 1 バイト追加し、長さの 8 バイトを入れる位置まで 0 を追加する。
		const unsigned char one = 0x80;
		update(&one, 1);
		const unsigned char zero = 0x00;
		while (buffer_len_ != 56)
			update(&zero, 1);

		unsigned char length_be[8];
		for (int i = 0; i < 8; ++i)
			length_be[i] = static_cast<unsigned char>((total_bits >> (56 - 8 * i)) & 0xFF);
		update(length_be, 8);

		static const char kHexDigits[] = "0123456789abcdef";
		std::string out;
		out.reserve(64);
		for (const std::uint32_t word : state_) {
			for (int shift = 24; shift >= 0; shift -= 8) {
				const unsigned value = (word >> shift) & 0xFFu;
				out.push_back(kHexDigits[value >> 4]);
				out.push_back(kHexDigits[value & 0x0Fu]);
			}
		}
		return out;
	}

private:
	void compress(const unsigned char block[64])
	{
		std::uint32_t w[64];
		for (int i = 0; i < 16; ++i) {
			w[i] = (std::uint32_t(block[i * 4]) << 24) | (std::uint32_t(block[i * 4 + 1]) << 16) |
			       (std::uint32_t(block[i * 4 + 2]) << 8) | std::uint32_t(block[i * 4 + 3]);
		}
		for (int i = 16; i < 64; ++i) {
			const std::uint32_t s0 = detail::sha256_rotr(w[i - 15], 7) ^ detail::sha256_rotr(w[i - 15], 18) ^
						  (w[i - 15] >> 3);
			const std::uint32_t s1 = detail::sha256_rotr(w[i - 2], 17) ^ detail::sha256_rotr(w[i - 2], 19) ^
						  (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}

		std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
		std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

		for (int i = 0; i < 64; ++i) {
			const std::uint32_t s1 =
				detail::sha256_rotr(e, 6) ^ detail::sha256_rotr(e, 11) ^ detail::sha256_rotr(e, 25);
			const std::uint32_t ch = (e & f) ^ ((~e) & g);
			const std::uint32_t temp1 = h + s1 + ch + detail::kSha256K[i] + w[i];
			const std::uint32_t s0 =
				detail::sha256_rotr(a, 2) ^ detail::sha256_rotr(a, 13) ^ detail::sha256_rotr(a, 22);
			const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
			const std::uint32_t temp2 = s0 + maj;

			h = g;
			g = f;
			f = e;
			e = d + temp1;
			d = c;
			c = b;
			b = a;
			a = temp1 + temp2;
		}

		state_[0] += a;
		state_[1] += b;
		state_[2] += c;
		state_[3] += d;
		state_[4] += e;
		state_[5] += f;
		state_[6] += g;
		state_[7] += h;
	}

	// FIPS 180-4 の 5.3.3 の初期値 (最初の 8 個の素数の平方根の、小数部の先頭 32 ビット)。
	std::uint32_t state_[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
				    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
	unsigned char buffer_[64] = {};
	std::size_t buffer_len_ = 0;
	std::uint64_t total_bytes_ = 0;
};

// data の SHA-256 を、16 進の 64 文字 (小文字) で返す。
inline std::string sha256_hex(std::string_view data)
{
	Sha256 hasher;
	hasher.update(data);
	return hasher.final_hex();
}

} // namespace cordmux::platform
