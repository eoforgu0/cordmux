// SPDX-License-Identifier: Apache-2.0

// decode/h264_decoder.h (H264Decoder) と decode/ffmpeg_link_versions.h の単体テスト。
// 壊れた入力を渡すテストでは、avcodec が標準エラー出力にメッセージ ("No start code is found." など) を
// 書く。avcodec の既定のふるまいで、テストの失敗ではない。

#include "doctest.h"

#include "decode/ffmpeg_link_versions.h"
#include "decode/h264_decoder.h"
#include "fixtures.h"

#include <fstream>
#include <string>
#include <vector>

using cordmux::decode::DecodedFrame;
using cordmux::decode::DecoderConfig;
using cordmux::decode::H264Decoder;
using cordmux::decode::PixelFormat;
using cordmux::tests::Rng;

namespace {

#ifndef CORDMUX_TEST_FIXTURES_DIR
#define CORDMUX_TEST_FIXTURES_DIR "fixtures"
#endif

// テストデータ (fixtures/h264_64x64_10f.h264) は、ffmpeg 8.0 の次のコマンドで作った。
//   ffmpeg -y -f lavfi -i testsrc2=size=64x64:rate=30 -frames:v 10 -an \
//     -c:v libx264 -pix_fmt yuv420p -profile:v baseline -g 5 -bf 0 \
//     -x264-params repeat-headers=1:aud=1 -f h264 h264_64x64_10f.h264
// -g 5 で、5 フレームに 1 回キーフレームが入る (このファイルではフレーム 0 と 5 が IDR。ffprobe で
// 確認した)。repeat-headers=1 で、キーフレームごとに SPS と PPS が入る (decode/h264_decoder.h が
// 前提にする形)。aud=1 で、フレームの先頭に AUD (NAL type 9) が付く。このテストは AUD の位置で
// アクセスユニットに分ける。
std::vector<uint8_t> read_fixture(const char *name)
{
	std::string path = std::string(CORDMUX_TEST_FIXTURES_DIR) + "/" + name;
	std::ifstream f(path, std::ios::binary);
	REQUIRE(f.good());
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Annex B のバイト列を、AUD (NAL type 9) の開始位置でアクセスユニットに分ける。
// WebSocket の 1 メッセージが 1 アクセスユニットであること (docs/wire_format.md の「映像のペイロード」) を、
// 1 つのファイルから再現するためのテスト用の関数。decode/ はこの分け方に依存しない (push() は、受け取った
// バイト列をそのまま 1 アクセスユニットとして扱う)。
std::vector<std::vector<uint8_t>> split_into_access_units(const std::vector<uint8_t> &bytes)
{
	std::vector<size_t> boundaries;
	size_t i = 0;
	while (i + 3 <= bytes.size()) {
		if (bytes[i] == 0x00 && bytes[i + 1] == 0x00 && bytes[i + 2] == 0x01) {
			size_t header = i + 3;
			if (header < bytes.size() && (bytes[header] & 0x1F) == 9) // AUD
				boundaries.push_back(i);
			i = header;
			continue;
		}
		++i;
	}
	std::vector<std::vector<uint8_t>> units;
	for (size_t k = 0; k < boundaries.size(); ++k) {
		size_t begin = boundaries[k];
		size_t end = (k + 1 < boundaries.size()) ? boundaries[k + 1] : bytes.size();
		units.emplace_back(bytes.begin() + static_cast<long>(begin), bytes.begin() + static_cast<long>(end));
	}
	return units;
}

std::vector<std::vector<uint8_t>> load_access_units()
{
	auto units = split_into_access_units(read_fixture("h264_64x64_10f.h264"));
	REQUIRE(units.size() == 10);
	return units;
}

} // namespace

TEST_CASE("10 フレームの Annex B をデコードすると、フレームの数と解像度が一致する")
{
	auto units = load_access_units();
	H264Decoder decoder;
	std::vector<DecodedFrame> frames;

	for (size_t i = 0; i < units.size(); ++i) {
		bool need_key = decoder.push(units[i].data(), units[i].size(), int64_t(i) * 33333, &frames);
		// キーフレーム (フレーム 0) から順に渡しているので、一度もキーフレームを要求しない。
		CHECK_FALSE(need_key);
	}
	decoder.flush(&frames);

	REQUIRE(frames.size() == 10);
	for (const auto &f : frames) {
		CHECK(f.width == 64);
		CHECK(f.height == 64);
		CHECK(f.format == PixelFormat::I420);
		CHECK_FALSE(f.plane[0].empty());
		CHECK_FALSE(f.plane[1].empty());
		CHECK_FALSE(f.plane[2].empty());
	}
	CHECK(decoder.stats().received == 10);
	CHECK(decoder.stats().decoded == 10);
	CHECK(decoder.stats().failed == 0);
	CHECK_FALSE(decoder.stats().hw_fallback);
}

TEST_CASE("キーフレームより後から渡すと、次のキーフレームまでキーフレームを要求し続ける")
{
	auto units = load_access_units();
	H264Decoder decoder;
	std::vector<DecodedFrame> frames;

	// フレーム 0 (キーフレーム) を渡さず、フレーム 1 (P フレーム) から渡す。
	for (size_t i = 1; i < units.size(); ++i) {
		bool need_key = decoder.push(units[i].data(), units[i].size(), int64_t(i) * 33333, &frames);
		if (i < 5) {
			// フレーム 5 (2 つ目のキーフレーム) より前は、デコードに必要な情報がまだない。
			CHECK(need_key);
		} else {
			// フレーム 5 でキーフレームを受け取り、以後は要求しない。
			CHECK_FALSE(need_key);
		}
	}
	decoder.flush(&frames);

	// フレーム 5 から 9 の 5 つだけがデコードできる。
	CHECK(frames.size() == 5);
	CHECK(decoder.stats().received == 9);
	CHECK(decoder.stats().decoded == 5);
	CHECK(decoder.stats().failed == 4); // フレーム 1 から 4 (デコーダーに渡さずに捨てたもの)
}

TEST_CASE("キーフレームより前に乱数のバイト列を渡しても、クラッシュせずに数え続ける")
{
	H264Decoder decoder;
	std::vector<DecodedFrame> frames;
	Rng rng(20260914);

	const int iterations = 300;
	for (int i = 0; i < iterations; ++i) {
		size_t len = 1 + (rng.next() % 400);
		std::vector<uint8_t> garbage(len);
		for (auto &b : garbage)
			b = uint8_t(rng.next() & 0xFF);
		// デコーダーを一度も開いていない (キーフレームをまだ 1 つも受け取っていない) 状態で、
		// 乱数のバイト列を繰り返し渡す。
		decoder.push(garbage.data(), garbage.size(), int64_t(i), &frames);
	}

	// クラッシュせずにここまで来ることが主な目的。統計の整合性も確認する。
	CHECK(decoder.stats().received == uint64_t(iterations));
	CHECK(decoder.stats().decoded <= decoder.stats().received);
	CHECK(decoder.stats().failed <= decoder.stats().received);
}

TEST_CASE("デコードの途中に乱数のバイト列を挟んでもクラッシュせず、次のキーフレームからまたデコードできる")
{
	auto units = load_access_units();
	H264Decoder decoder;
	std::vector<DecodedFrame> frames;

	// まず正しいキーフレームを渡して、デコードできる状態にする。
	CHECK_FALSE(decoder.push(units[0].data(), units[0].size(), 0, &frames));

	Rng rng(9);
	const int iterations = 300;
	for (int i = 0; i < iterations; ++i) {
		size_t len = 1 + (rng.next() % 400);
		std::vector<uint8_t> garbage(len);
		for (auto &b : garbage)
			b = uint8_t(rng.next() & 0xFF);
		decoder.push(garbage.data(), garbage.size(), int64_t(i) + 1, &frames);
	}

	// 乱数のバイト列の後も、2 つ目のキーフレーム (フレーム 5) から先は正しくデコードできる
	// (壊れた入力を挟んでも、中の状態が壊れたままにならない)。
	for (size_t i = 5; i < units.size(); ++i)
		decoder.push(units[i].data(), units[i].size(), int64_t(i) * 33333, &frames);
	decoder.flush(&frames);

	bool recovered = false;
	for (const auto &f : frames) {
		if (f.width == 64 && f.height == 64)
			recovered = true;
	}
	CHECK(recovered);
	CHECK(decoder.stats().received == uint64_t(1 + iterations + 5));
}

TEST_CASE("このビルドが対応しない hw_type を指定すると、ソフトウェアデコードに切り替えて hw_fallback を true にする")
{
	auto units = load_access_units();
	DecoderConfig cfg;
	// VideoToolbox は macOS 専用で、Windows 向けの FFmpeg には入っていない
	// (GPU やドライバーに触れずに、対応していない種類を再現できる)。
	cfg.hw_type = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
	H264Decoder decoder(cfg);
	std::vector<DecodedFrame> frames;

	bool need_key = decoder.push(units[0].data(), units[0].size(), 0, &frames);
	decoder.flush(&frames);

	CHECK_FALSE(need_key);
	CHECK(decoder.stats().hw_fallback);
	REQUIRE(frames.size() == 1);
	CHECK(frames[0].format == PixelFormat::I420); // ソフトウェアデコードに切り替わっている
	CHECK(decoder.stats().failed == 0);
}

TEST_CASE("リンクした FFmpeg のバージョンが obs-deps のもの (avcodec 61、avutil 59、swresample 5) と一致する")
{
	auto v = cordmux::decode::ffmpeg_link_versions();
	CHECK(v.avcodec_major == 61);
	CHECK(v.avutil_major == 59);
	CHECK(v.swresample_major == 5);
}
