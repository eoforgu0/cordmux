// SPDX-License-Identifier: Apache-2.0

// 1 つの zip を作る関数 (build_zip_stored)。作れる形は、圧縮なし (method 0)、暗号なし、data descriptor なし、zip64 なし
// の 1 つだけで (PKWARE APPNOTE 6.3.x の最小の形)、Windows の標準の展開、7-Zip、Python の zipfile のどれでも開ける。
// 中身は診断情報のログで数百 KiB なので、圧縮の効果より、zlib に依存することの費用のほうが大きい。CRC-32 も自分で
// 計算する。時刻は引数で受け取り、時計を読まないので、同じ入力には必ず同じバイト列を返す (単体テストが zip の中身を
// 1 バイトずつ比べられる)。libobs にも Qt にも依存しない。

#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace cordmux::bundle {

// zip に入れる 1 つのファイル。name は、フォルダーを含まない ASCII の名前 (valid_zip_name)。
struct ZipEntry {
	std::string name;
	std::string content;
};

// zip の中のファイルに書く日時 (MS-DOS の形にする)。範囲の外の値は 1980-01-01 00:00:00 にする (日時を書けないことで、
// zip の作成を失敗させない。書き出した時刻は manifest.json にもあるので、ここで変えても失われる情報はない)。
struct ZipDateTime {
	int year = 1980;
	int month = 1;
	int day = 1;
	int hour = 0;
	int minute = 0;
	int second = 0;
};

// zip 全体の大きさの上限 (バイト)。中身はログのリングバッファーの上限から数百 KiB のはずなので、桁を間違えたデータで
// 巨大なメモリを確保しないための値である (目安の値で、測って決めたものではない)。zip64 に対応しないので、どのみち
// 4 GiB は書けない。
inline constexpr std::size_t kZipMaxTotalBytes = 256u * 1024u * 1024u;

// 入れられるファイルの数の上限。診断情報のファイルは 5 つで、数千に増えることはない。end of central directory
// record はファイルの数を 16 ビットで持つので、どのみち 65535 を超える数は書けない。
inline constexpr std::size_t kZipMaxEntries = 4096;

// zip に入れられる名前か。ASCII の英数字と "."、"-"、"_" だけで、100 文字まで。zip の中にフォルダーを作らないので、
// 区切りも許さない。先頭の "." を許さないのは、展開したときに隠しファイルにしないためである。
inline bool valid_zip_name(std::string_view name)
{
	if (name.empty() || name.size() > 100)
		return false;
	if (name.front() == '.')
		return false;
	for (const char c : name) {
		const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
				c == '.' || c == '-' || c == '_';
		if (!ok)
			return false;
	}
	return true;
}

namespace detail {

// CRC-32 (IEEE 802.3。反転した多項式 0xEDB88320) の表。zlib に依存しないので、自分で持つ。表は最初に 1 度だけ作る。
inline const std::uint32_t *crc32_table()
{
	static const std::vector<std::uint32_t> table = [] {
		std::vector<std::uint32_t> out(256);
		for (std::uint32_t i = 0; i < 256; ++i) {
			std::uint32_t c = i;
			for (int bit = 0; bit < 8; ++bit)
				c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
			out[i] = c;
		}
		return out;
	}();
	return table.data();
}

inline void put_u16(std::string *out, std::uint16_t value)
{
	out->push_back(static_cast<char>(value & 0xFFu));
	out->push_back(static_cast<char>((value >> 8) & 0xFFu));
}

inline void put_u32(std::string *out, std::uint32_t value)
{
	out->push_back(static_cast<char>(value & 0xFFu));
	out->push_back(static_cast<char>((value >> 8) & 0xFFu));
	out->push_back(static_cast<char>((value >> 16) & 0xFFu));
	out->push_back(static_cast<char>((value >> 24) & 0xFFu));
}

// MS-DOS の日付と時刻 (2 秒単位)。範囲の外の値は 1980-01-01 00:00:00 にする (ZipDateTime のコメント)。
inline void dos_date_time(const ZipDateTime &stamp, std::uint16_t *out_date, std::uint16_t *out_time)
{
	const bool in_range = stamp.year >= 1980 && stamp.year <= 2107 && stamp.month >= 1 && stamp.month <= 12 &&
			      stamp.day >= 1 && stamp.day <= 31 && stamp.hour >= 0 && stamp.hour <= 23 &&
			      stamp.minute >= 0 && stamp.minute <= 59 && stamp.second >= 0 && stamp.second <= 59;
	if (!in_range) {
		*out_date = static_cast<std::uint16_t>((0u << 9) | (1u << 5) | 1u); // 1980-01-01
		*out_time = 0;
		return;
	}
	*out_date = static_cast<std::uint16_t>(((stamp.year - 1980) << 9) | (stamp.month << 5) | stamp.day);
	*out_time = static_cast<std::uint16_t>((stamp.hour << 11) | (stamp.minute << 5) | (stamp.second / 2));
}

} // namespace detail

// data の CRC-32 (zip のヘッダーに入れる値)。"123456789" には 0xCBF43926 (CRC カタログの CRC-32/ISO-HDLC の check の
// 値)、空の入力には 0 を返す。
inline std::uint32_t crc32(std::string_view data)
{
	const std::uint32_t *table = detail::crc32_table();
	std::uint32_t crc = 0xFFFFFFFFu;
	for (const char ch : data) {
		const std::uint8_t byte = static_cast<std::uint8_t>(ch);
		crc = table[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
	}
	return crc ^ 0xFFFFFFFFu;
}

// entries を、圧縮しない 1 つの zip にして *out に入れる。
//
// false を返すのは次の場合だけで、そのとき *out は空になる (壊れた zip を渡さない)。
//   - 入れるファイルが 1 つもないか、kZipMaxEntries より多い
//   - 名前が valid_zip_name を満たさないか、同じ名前が 2 つある
//   - 全体が max_total_bytes を超える
// 理由は *error に、人が読む 1 行で入れる。
//
// max_total_bytes を引数にしているのは、上限を超えたときの動作を、256 MiB の文字列を作らずに単体テストで確認する
// ためである (cordmux.dll は既定の値で呼ぶ)。
inline bool build_zip_stored(const std::vector<ZipEntry> &entries, const ZipDateTime &stamp, std::string *out,
			     std::string *error, std::size_t max_total_bytes = kZipMaxTotalBytes)
{
	if (out)
		out->clear();
	if (error)
		error->clear();

	if (entries.empty()) {
		if (error)
			*error = "入れるものが 1 つもない";
		return false;
	}
	if (entries.size() > kZipMaxEntries) {
		if (error)
			*error = "入れるものが多すぎる";
		return false;
	}

	std::uint16_t dos_date = 0;
	std::uint16_t dos_time = 0;
	detail::dos_date_time(stamp, &dos_date, &dos_time);

	std::set<std::string> seen_names;
	std::size_t total = 0;
	for (const ZipEntry &entry : entries) {
		if (!valid_zip_name(entry.name)) {
			if (error)
				*error = "zip に入れられない名前: " + entry.name;
			return false;
		}
		if (!seen_names.insert(entry.name).second) {
			if (error)
				*error = "同じ名前が 2 つある: " + entry.name;
			return false;
		}
		// local file header の 30 バイト、central directory header の 46 バイト、2 回の名前、中身。
		total += 30 + 46 + entry.name.size() * 2 + entry.content.size();
		if (total > max_total_bytes) {
			if (error)
				*error = "材料が大きすぎる";
			return false;
		}
	}

	std::string zip;
	zip.reserve(total + 22);

	struct CentralRecord {
		std::uint32_t crc = 0;
		std::uint32_t size = 0;
		std::uint32_t offset = 0;
	};
	std::vector<CentralRecord> central;
	central.reserve(entries.size());

	for (const ZipEntry &entry : entries) {
		CentralRecord record;
		record.crc = crc32(entry.content);
		record.size = static_cast<std::uint32_t>(entry.content.size());
		record.offset = static_cast<std::uint32_t>(zip.size());

		detail::put_u32(&zip, 0x04034B50u);                                 // local file header のシグネチャ
		detail::put_u16(&zip, 20);                                          // 展開に必要なバージョン (2.0)
		detail::put_u16(&zip, 0);                                           // フラグ (暗号も data descriptor もなし)
		detail::put_u16(&zip, 0);                                           // 圧縮の方式 0 (圧縮なし)
		detail::put_u16(&zip, dos_time);
		detail::put_u16(&zip, dos_date);
		detail::put_u32(&zip, record.crc);
		detail::put_u32(&zip, record.size); // 圧縮しないので、圧縮後と圧縮前の大きさは同じ
		detail::put_u32(&zip, record.size);
		detail::put_u16(&zip, static_cast<std::uint16_t>(entry.name.size()));
		detail::put_u16(&zip, 0); // extra field は持たない
		zip += entry.name;
		zip += entry.content;

		central.push_back(record);
	}

	const std::uint32_t central_offset = static_cast<std::uint32_t>(zip.size());
	for (std::size_t i = 0; i < entries.size(); ++i) {
		const ZipEntry &entry = entries[i];
		const CentralRecord &record = central[i];

		detail::put_u32(&zip, 0x02014B50u); // central directory header のシグネチャ
		detail::put_u16(&zip, 20);          // 作ったバージョン
		detail::put_u16(&zip, 20);          // 展開に必要なバージョン
		detail::put_u16(&zip, 0);
		detail::put_u16(&zip, 0);
		detail::put_u16(&zip, dos_time);
		detail::put_u16(&zip, dos_date);
		detail::put_u32(&zip, record.crc);
		detail::put_u32(&zip, record.size);
		detail::put_u32(&zip, record.size);
		detail::put_u16(&zip, static_cast<std::uint16_t>(entry.name.size()));
		detail::put_u16(&zip, 0); // extra field の長さ
		detail::put_u16(&zip, 0); // file comment の長さ
		detail::put_u16(&zip, 0); // 開始ディスクの番号 (disk number start)
		detail::put_u16(&zip, 0); // 内部のファイル属性 (internal file attributes)
		detail::put_u32(&zip, 0); // 外部のファイル属性 (external file attributes)
		detail::put_u32(&zip, record.offset);
		zip += entry.name;
	}

	const std::uint32_t central_size = static_cast<std::uint32_t>(zip.size()) - central_offset;
	const std::uint16_t count = static_cast<std::uint16_t>(entries.size());

	detail::put_u32(&zip, 0x06054B50u); // end of central directory record のシグネチャ
	detail::put_u16(&zip, 0);
	detail::put_u16(&zip, 0);
	detail::put_u16(&zip, count);
	detail::put_u16(&zip, count);
	detail::put_u32(&zip, central_size);
	detail::put_u32(&zip, central_offset);
	detail::put_u16(&zip, 0); // zip file comment の長さ

	if (out)
		*out = std::move(zip);
	return true;
}

} // namespace cordmux::bundle
