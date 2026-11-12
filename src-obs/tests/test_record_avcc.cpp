// SPDX-License-Identifier: Apache-2.0

// record/record_avcc.h の単体テスト。Annex B の走査、avcC の組み立て、AVCC への詰め替えを確認する。
// テストデータは tests/fixtures/h264_64x64_10f.h264 (decode/ のテストと同じもの。作り方は test_decode.cpp の
// コメントにある)。実際のエンコーダが出した Annex B で確認する。作ったバイト列だけでは、AUD や SEI が混じる実際の
// 並びで走査が崩れても気付けないためである。

#include "doctest.h"

#include "record/record_avcc.h"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using cordmux::record::annex_b_to_avcc;
using cordmux::record::AnnexBUnits;
using cordmux::record::avcc_all_equal;
using cordmux::record::build_avcc;
using cordmux::record::build_avcc_from_annex_b;
using cordmux::record::idr_has_parameter_sets;
using cordmux::record::nal_type_of;
using cordmux::record::parse_sps_dimensions;
using cordmux::record::sps_dimensions_from_annex_b;
using cordmux::record::scan_annex_b;
using cordmux::record::to_hex;

namespace {

#ifndef CORDMUX_TEST_FIXTURES_DIR
#define CORDMUX_TEST_FIXTURES_DIR "fixtures"
#endif

std::vector<std::uint8_t> read_fixture(const char *name)
{
	const std::string path = std::string(CORDMUX_TEST_FIXTURES_DIR) + "/" + name;
	std::ifstream file(path, std::ios::binary);
	REQUIRE(file.good());
	return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("エンコーダが出した Annex B を、AUD と SEI が混じっていても正しく走査する")
{
	const std::vector<std::uint8_t> bytes = read_fixture("h264_64x64_10f.h264");
	const AnnexBUnits units = scan_annex_b(bytes.data(), bytes.size());

	CHECK(units.units.size() == 25); // ffprobe で数えた数 (AUD 10 + SPS 2 + PPS 2 + SEI 1 + スライス 10)
	CHECK(units.has_idr);
	REQUIRE(units.sps_index.has_value());
	REQUIRE(units.pps_index.has_value());
	CHECK(nal_type_of(bytes[units.units[*units.sps_index].first]) == 7);
	CHECK(nal_type_of(bytes[units.units[*units.pps_index].first]) == 8);
	CHECK(units.units[*units.sps_index].second == 23);
	CHECK(units.units[*units.pps_index].second == 5);
	CHECK(idr_has_parameter_sets(units));

	// 末尾の 0 (次の 4 バイトの開始コードの先頭) を中身に含めない。
	for (const auto &unit : units.units)
		CHECK(bytes[unit.first + unit.second - 1] != 0x00);
}

TEST_CASE("avcC は SPS の 3 バイトをコピーし、長さを前に付けた SPS と PPS を並べたものになる")
{
	const std::vector<std::uint8_t> bytes = read_fixture("h264_64x64_10f.h264");
	const auto avcc = build_avcc_from_annex_b(bytes.data(), bytes.size());
	REQUIRE(avcc.has_value());

	CHECK(avcc->size() == 39);
	CHECK((*avcc)[0] == 0x01);
	CHECK((*avcc)[4] == 0xFF); // lengthSizeMinusOne = 3
	CHECK((*avcc)[5] == 0xE1); // SPS は 1 つ
	// 録画ログに書くのは、このバイト列そのものである。
	CHECK(to_hex(*avcc) == "0142c00affe100176742c00ad90426c044000003000400000300f03c48992001000568cb83cb20");
}

TEST_CASE("渡された NAL の型が違えば、avcC を組み立てない")
{
	const std::vector<std::uint8_t> not_sps = {0x68, 0x01, 0x02, 0x03};
	const std::vector<std::uint8_t> pps = {0x68, 0xcb};
	CHECK_FALSE(build_avcc(not_sps.data(), not_sps.size(), pps.data(), pps.size()).has_value());

	const std::vector<std::uint8_t> sps = {0x67, 0x42, 0xc0, 0x0a};
	const std::vector<std::uint8_t> not_pps = {0x67, 0xcb};
	CHECK_FALSE(build_avcc(sps.data(), sps.size(), not_pps.data(), not_pps.size()).has_value());

	// SPS が 4 バイトに満たなければ組み立てられない (コピーする 3 バイトがない)。
	const std::vector<std::uint8_t> short_sps = {0x67, 0x42, 0xc0};
	CHECK_FALSE(build_avcc(short_sps.data(), short_sps.size(), pps.data(), pps.size()).has_value());

	CHECK(build_avcc(sps.data(), sps.size(), pps.data(), pps.size()).has_value());
}

TEST_CASE("AVCC に詰め替えると、各 NAL の前に 4 バイトの長さが付き、SPS と PPS も残る")
{
	const std::vector<std::uint8_t> bytes = read_fixture("h264_64x64_10f.h264");
	const AnnexBUnits units = scan_annex_b(bytes.data(), bytes.size());
	const std::vector<std::uint8_t> avcc = annex_b_to_avcc(bytes.data(), bytes.size());

	std::size_t expected = 0;
	for (const auto &unit : units.units)
		expected += 4 + unit.second;
	CHECK(avcc.size() == expected);

	// 先頭の長さが最初の NAL の長さと一致し、その後ろに中身がそのまま来る。
	const std::size_t first_len = units.units[0].second;
	CHECK(avcc[0] == 0x00);
	CHECK(avcc[1] == 0x00);
	CHECK(avcc[2] == static_cast<std::uint8_t>((first_len >> 8) & 0xFF));
	CHECK(avcc[3] == static_cast<std::uint8_t>(first_len & 0xFF));
	CHECK(avcc[4] == bytes[units.units[0].first]);

	// すべての NAL を順にたどれる (長さが正しいことの確認)。
	std::size_t at = 0;
	std::size_t count = 0;
	while (at + 4 <= avcc.size()) {
		const std::size_t len = (std::size_t(avcc[at]) << 24) | (std::size_t(avcc[at + 1]) << 16) |
					(std::size_t(avcc[at + 2]) << 8) | std::size_t(avcc[at + 3]);
		at += 4 + len;
		++count;
	}
	CHECK(at == avcc.size());
	CHECK(count == units.units.size());
}

TEST_CASE("3 バイトと 4 バイトの開始コードが混ざっていても、同じ位置で分ける")
{
	// 並びは 3 バイトの開始コード + SPS、4 バイトの開始コード + PPS、3 バイトの開始コード + IDR。
	const std::vector<std::uint8_t> bytes = {0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x0a, 0x00, 0x00,
						 0x00, 0x01, 0x68, 0xcb, 0x83, 0x00, 0x00, 0x01, 0x65, 0x88};
	const AnnexBUnits units = scan_annex_b(bytes.data(), bytes.size());
	REQUIRE(units.units.size() == 3);
	CHECK(units.units[0].second == 4); // SPS (末尾の 0 は次の開始コードのもの)
	CHECK(units.units[1].second == 3); // PPS
	CHECK(units.units[2].second == 2); // IDR
	CHECK(units.has_idr);
	CHECK(idr_has_parameter_sets(units));
}

TEST_CASE("アクセスユニットの最後の NAL の末尾の 0 は、受け取ったデータの一部なので除かない")
{
	// NAL が 1 つだけのアクセスユニットで、中身が 0x00 で終わる (H.264 の cabac_zero_word か trailing_zero_8bits)。
	const std::vector<std::uint8_t> bytes = {0x00, 0x00, 0x01, 0x65, 0x88, 0x00, 0x00};
	const AnnexBUnits units = scan_annex_b(bytes.data(), bytes.size());
	REQUIRE(units.units.size() == 1);
	CHECK(units.units[0].second == 4); // 65 88 00 00 (1 バイトも除かない)

	// 詰め替えても縮まない (受け取ったアクセスユニットと、ファイルに入るアクセスユニットがバイト単位で一致する)。
	const std::vector<std::uint8_t> sample = annex_b_to_avcc(bytes.data(), bytes.size());
	REQUIRE(sample.size() == 4 + 4);
	CHECK(sample[3] == 4);
	CHECK(sample[6] == 0x00);
	CHECK(sample[7] == 0x00);

	// 一方、次に開始コードが続く NAL の末尾の 0 は、4 バイトの開始コードの 1 バイト目なので除く。
	const std::vector<std::uint8_t> two = {0x00, 0x00, 0x01, 0x65, 0x88, 0x00,
					       0x00, 0x00, 0x01, 0x68, 0xcb};
	const AnnexBUnits pair = scan_annex_b(two.data(), two.size());
	REQUIRE(pair.units.size() == 2);
	CHECK(pair.units[0].second == 2); // 65 88
	CHECK(pair.units[1].second == 2); // 68 cb
}

TEST_CASE("開始コードがないデータからは、NAL を 1 つも取り出さない")
{
	const std::vector<std::uint8_t> bytes = {0x65, 0x88, 0x84, 0x1d, 0xf1};
	const AnnexBUnits units = scan_annex_b(bytes.data(), bytes.size());
	CHECK(units.units.empty());
	CHECK_FALSE(units.has_idr);
	CHECK(annex_b_to_avcc(bytes.data(), bytes.size()).empty());
	CHECK_FALSE(build_avcc_from_annex_b(bytes.data(), bytes.size()).has_value());
	CHECK(scan_annex_b(nullptr, 0).units.empty());
}

TEST_CASE("SPS と PPS の付いていない IDR からは avcC を組み立てられない")
{
	const std::vector<std::uint8_t> bytes = {0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x1d};
	const AnnexBUnits units = scan_annex_b(bytes.data(), bytes.size());
	CHECK(units.has_idr);
	CHECK_FALSE(idr_has_parameter_sets(units));
	CHECK_FALSE(build_avcc_from_annex_b(bytes.data(), bytes.size()).has_value());
}

TEST_CASE("16 進は小文字で書き、avcC の比較は 1 バイトでも違えば偽になる")
{
	const std::vector<std::uint8_t> bytes = {0x00, 0x0f, 0xa0, 0xff};
	CHECK(to_hex(bytes) == "000fa0ff");

	CHECK(avcc_all_equal({}));
	CHECK(avcc_all_equal({"0142c00a"}));
	CHECK(avcc_all_equal({"0142c00a", "0142c00a", "0142c00a"}));
	CHECK_FALSE(avcc_all_equal({"0142c00a", "0142c00b"}));
	CHECK_FALSE(avcc_all_equal({"0142c00a", ""}));
}

namespace {

std::vector<std::uint8_t> from_hex(const std::string &hex)
{
	std::vector<std::uint8_t> out;
	for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
		out.push_back(static_cast<std::uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
	return out;
}

} // namespace

TEST_CASE("SPS から表示の解像度を読む")
{
	// エンコーダが出した Annex B (64x64。クロップなし)。
	const std::vector<std::uint8_t> bytes = read_fixture("h264_64x64_10f.h264");
	const auto fixture = sps_dimensions_from_annex_b(bytes.data(), bytes.size());
	REQUIRE(fixture.has_value());
	CHECK(fixture->width == 64);
	CHECK(fixture->height == 64);

	// 同梱の FFmpeg の libx264 で作った SPS (High、scaling matrix あり、下を 8 行クロップする、エミュレーション防止
	// バイト 00 00 03 を含む)。1088 行から 8 行を引いて 1080。
	const std::vector<std::uint8_t> hd = from_hex("6764002aacd940780227e5c044000003000400000301e03c60c658");
	const auto full_hd = parse_sps_dimensions(hd.data(), hd.size());
	REQUIRE(full_hd.has_value());
	CHECK(full_hd->width == 1920);
	CHECK(full_hd->height == 1080);

	// 同じく Main の 640x360 (368 行から 8 行を引く)。
	const std::vector<std::uint8_t> sd = from_hex("674d401feca05017fcb80880000003008000003c078c18cb");
	const auto small = parse_sps_dimensions(sd.data(), sd.size());
	REQUIRE(small.has_value());
	CHECK(small->width == 640);
	CHECK(small->height == 360);

	// 途中で切れた SPS と、SPS ではない NAL は値を持たない (呼び出し元は configure の解像度を使う)。
	const std::vector<std::uint8_t> cut(hd.begin(), hd.begin() + 6);
	CHECK_FALSE(parse_sps_dimensions(cut.data(), cut.size()).has_value());
	const std::vector<std::uint8_t> pps = {0x68, 0xce, 0x3c, 0x80};
	CHECK_FALSE(parse_sps_dimensions(pps.data(), pps.size()).has_value());
}
