// SPDX-License-Identifier: Apache-2.0

// bundle/zip_writer.h の単体テスト。書いた zip を、このテストで読み戻して確認する。展開するツールを使わずに開ける形か
// を確認するには、シグネチャと位置と大きさを読み直すしかないためである。CRC は、公表されている値 (CRC-32/ISO-HDLC の
// check の値) と比べる。

#include "doctest.h"

#include "bundle/zip_writer.h"

#include <cstdint>
#include <string>
#include <vector>

using cordmux::bundle::build_zip_stored;
using cordmux::bundle::crc32;
using cordmux::bundle::valid_zip_name;
using cordmux::bundle::ZipDateTime;
using cordmux::bundle::ZipEntry;

namespace {

std::uint16_t read_u16(const std::string &data, std::size_t at)
{
	REQUIRE(at + 2 <= data.size());
	return static_cast<std::uint16_t>(static_cast<unsigned char>(data[at]) |
					  (static_cast<unsigned char>(data[at + 1]) << 8));
}

std::uint32_t read_u32(const std::string &data, std::size_t at)
{
	REQUIRE(at + 4 <= data.size());
	return static_cast<std::uint32_t>(static_cast<unsigned char>(data[at])) |
	       (static_cast<std::uint32_t>(static_cast<unsigned char>(data[at + 1])) << 8) |
	       (static_cast<std::uint32_t>(static_cast<unsigned char>(data[at + 2])) << 16) |
	       (static_cast<std::uint32_t>(static_cast<unsigned char>(data[at + 3])) << 24);
}

// central directory から読み戻した 1 つのファイル。
struct ReadEntry {
	std::string name;
	std::string content;
	std::uint32_t crc = 0;
	std::uint16_t dos_date = 0;
	std::uint16_t dos_time = 0;
};

// end of central directory record (EOCD) から central directory をたどり、ファイルごとに local file header を通して
// 中身を取り出す。
std::vector<ReadEntry> read_zip(const std::string &zip)
{
	REQUIRE(zip.size() >= 22);
	const std::size_t eocd = zip.size() - 22;
	REQUIRE(read_u32(zip, eocd) == 0x06054B50u);
	const std::uint16_t count = read_u16(zip, eocd + 10);
	CHECK(read_u16(zip, eocd + 8) == count); // このディスクの数と、全体の数は同じ
	const std::uint32_t central_size = read_u32(zip, eocd + 12);
	const std::uint32_t central_offset = read_u32(zip, eocd + 16);
	CHECK(central_offset + central_size == eocd);

	std::vector<ReadEntry> out;
	std::size_t cursor = central_offset;
	for (std::uint16_t i = 0; i < count; ++i) {
		REQUIRE(read_u32(zip, cursor) == 0x02014B50u);
		CHECK(read_u16(zip, cursor + 10) == 0); // 圧縮の方式 0 (圧縮なし)
		ReadEntry entry;
		entry.dos_time = read_u16(zip, cursor + 12);
		entry.dos_date = read_u16(zip, cursor + 14);
		entry.crc = read_u32(zip, cursor + 16);
		const std::uint32_t csize = read_u32(zip, cursor + 20);
		const std::uint32_t usize = read_u32(zip, cursor + 24);
		CHECK(csize == usize); // 圧縮しないので、2 つの大きさは同じ
		const std::uint16_t name_len = read_u16(zip, cursor + 28);
		const std::uint16_t extra_len = read_u16(zip, cursor + 30);
		const std::uint16_t comment_len = read_u16(zip, cursor + 32);
		const std::uint32_t local_offset = read_u32(zip, cursor + 42);
		entry.name = zip.substr(cursor + 46, name_len);

		// local file header の値も同じか (シグネチャ、名前、大きさ、CRC)。
		REQUIRE(read_u32(zip, local_offset) == 0x04034B50u);
		CHECK(read_u16(zip, local_offset + 8) == 0);
		CHECK(read_u32(zip, local_offset + 14) == entry.crc);
		CHECK(read_u32(zip, local_offset + 18) == usize);
		const std::uint16_t local_name_len = read_u16(zip, local_offset + 26);
		const std::uint16_t local_extra_len = read_u16(zip, local_offset + 28);
		CHECK(local_name_len == name_len);
		CHECK(zip.substr(local_offset + 30, local_name_len) == entry.name);
		entry.content = zip.substr(local_offset + 30 + local_name_len + local_extra_len, usize);

		out.push_back(entry);
		cursor += 46 + name_len + extra_len + comment_len;
	}
	CHECK(cursor == central_offset + central_size);
	return out;
}

ZipDateTime sample_stamp()
{
	ZipDateTime stamp;
	stamp.year = 2000;
	stamp.month = 9;
	stamp.day = 16;
	stamp.hour = 13;
	stamp.minute = 45;
	stamp.second = 30;
	return stamp;
}

} // namespace

TEST_CASE("crc32 は、公表されている check の値と同じ値を返す")
{
	CHECK(crc32("123456789") == 0xCBF43926u); // CRC-32/ISO-HDLC の check 値
	CHECK(crc32("") == 0u);
	CHECK(crc32("a") == 0xE8B7BE43u);
	// 1 バイト違えば値が変わる (表の引き方を間違えていないことを確認する)。
	CHECK(crc32("cordmux") != crc32("cordmuy"));
}

TEST_CASE("build_zip_stored で書いた zip を読み戻すと、名前と中身と CRC が元のとおりに読める")
{
	std::vector<ZipEntry> entries = {
		{"manifest.json", "{\"a\":1}\n"},
		{"plugin.log", "1 行目\n2 行目\n"}, // 中身は ASCII でなくてよい (ASCII に限るのは名前だけ)
		{"verified.txt", ""},              // 空のファイルも入れられる
	};

	std::string zip;
	std::string error;
	REQUIRE(build_zip_stored(entries, sample_stamp(), &zip, &error));
	CHECK(error.empty());

	const std::vector<ReadEntry> read = read_zip(zip);
	REQUIRE(read.size() == 3);
	for (std::size_t i = 0; i < entries.size(); ++i) {
		CHECK(read[i].name == entries[i].name);
		CHECK(read[i].content == entries[i].content);
		CHECK(read[i].crc == crc32(entries[i].content));
	}
}

TEST_CASE("日時は MS-DOS の形で入り、範囲の外の日時は 1980-01-01 になる")
{
	std::string zip;
	REQUIRE(build_zip_stored({{"a.txt", "x"}}, sample_stamp(), &zip, nullptr));
	const std::vector<ReadEntry> read = read_zip(zip);
	REQUIRE(read.size() == 1);
	CHECK(read[0].dos_date == static_cast<std::uint16_t>(((2000 - 1980) << 9) | (9 << 5) | 16));
	CHECK(read[0].dos_time == static_cast<std::uint16_t>((13 << 11) | (45 << 5) | (30 / 2)));

	ZipDateTime broken;
	broken.year = 1200; // 1980 より前 (MS-DOS の形に入らない)
	std::string zip2;
	REQUIRE(build_zip_stored({{"a.txt", "x"}}, broken, &zip2, nullptr));
	const std::vector<ReadEntry> read2 = read_zip(zip2);
	REQUIRE(read2.size() == 1);
	CHECK(read2[0].dos_date == static_cast<std::uint16_t>((1 << 5) | 1));
	CHECK(read2[0].dos_time == 0);
}

TEST_CASE("フォルダーを含まない ASCII の名前でなければ、build_zip_stored は書かない")
{
	CHECK(valid_zip_name("manifest.json"));
	CHECK(valid_zip_name("plugin-2.log"));
	CHECK_FALSE(valid_zip_name(""));
	CHECK_FALSE(valid_zip_name(".hidden"));      // 先頭の "." は隠しファイルになる
	CHECK_FALSE(valid_zip_name("dir/name.txt")); // zip の中にフォルダーを作らない
	CHECK_FALSE(valid_zip_name("dir\\name.txt"));
	CHECK_FALSE(valid_zip_name("../escape.txt"));
	CHECK_FALSE(valid_zip_name("材料.txt")); // ASCII でない
	CHECK_FALSE(valid_zip_name("a b.txt"));

	std::string zip;
	std::string error;
	CHECK_FALSE(build_zip_stored({{"材料.txt", "x"}}, sample_stamp(), &zip, &error));
	CHECK(zip.empty());
	CHECK_FALSE(error.empty());
}

TEST_CASE("同じ名前が 2 つあれば、build_zip_stored は書かない (展開したときに、どちらが残るか決まらない)")
{
	std::string zip;
	std::string error;
	CHECK_FALSE(build_zip_stored({{"a.txt", "1"}, {"a.txt", "2"}}, sample_stamp(), &zip, &error));
	CHECK(zip.empty());
	CHECK_FALSE(error.empty());
}

TEST_CASE("入れるファイルが 1 つもなければ、build_zip_stored は書かない")
{
	std::string zip;
	std::string error;
	CHECK_FALSE(build_zip_stored({}, sample_stamp(), &zip, &error));
	CHECK(zip.empty());
	CHECK_FALSE(error.empty());
}

TEST_CASE("max_total_bytes を超えるときは、build_zip_stored は書かない")
{
	const std::string big(1024, 'x');
	std::string zip;
	std::string error;
	// 上限を小さくして確認する (256 MiB の文字列を作らずに、上限を超えたときの処理を通す)。
	CHECK_FALSE(build_zip_stored({{"big.log", big}}, sample_stamp(), &zip, &error, 512));
	CHECK(zip.empty());
	CHECK_FALSE(error.empty());
	// 上限の中なら同じデータを書ける (上で書けなかったのが、上限のためであることを確認する)。
	CHECK(build_zip_stored({{"big.log", big}}, sample_stamp(), &zip, &error, 8192));
	CHECK_FALSE(zip.empty());
}

TEST_CASE("同じデータと同じ日時なら、build_zip_stored は 1 バイトも違わない zip を返す (時計を読まない)")
{
	const std::vector<ZipEntry> entries = {{"a.txt", "1"}, {"b.txt", "2"}};
	std::string first;
	std::string second;
	REQUIRE(build_zip_stored(entries, sample_stamp(), &first, nullptr));
	REQUIRE(build_zip_stored(entries, sample_stamp(), &second, nullptr));
	CHECK(first == second);
}
