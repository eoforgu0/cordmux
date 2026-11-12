// SPDX-License-Identifier: Apache-2.0

// 録画の H.264 の処理。Annex B を走査し、ストリームの中の SPS と PPS から avcC を組み立て、サンプルを AVCC (4 バイトの
// 長さを前に付けた形) に詰め替える。ワイヤー形式の映像は Annex B で、avcC は送られてこない。
// avcC を muxer に組み立てさせないのは、録画ログの avcc_hex とファイルに入るバイト列が 1 バイトも違わないことを
// 保証するためである (作成方法は、この値がすべてのセグメントで一致するかで決まる)。
// libobs、Qt、avformat に依存しない。

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "record/record_types.h"

namespace cordmux::record {

// H.264 の NAL の型 (下位 5 ビット)。
inline constexpr std::uint8_t kNalTypeIdr = 5;
inline constexpr std::uint8_t kNalTypeSps = 7;
inline constexpr std::uint8_t kNalTypePps = 8;

inline std::uint8_t nal_type_of(std::uint8_t first_byte)
{
	return static_cast<std::uint8_t>(first_byte & 0x1F);
}

// 1 つのアクセスユニットの中の NAL の位置 (開始コードを除いた、データの先頭からの位置と長さ)。
struct AnnexBUnits {
	std::vector<std::pair<std::size_t, std::size_t>> units;
	bool has_idr = false;
	std::optional<std::size_t> sps_index; // units の添字 (最初に見つけたもの)
	std::optional<std::size_t> pps_index;
};

// Annex B を走査する。開始コードのないデータでは units が空になる。
// 走査は decode/h264_decoder.cpp の H264Decoder::contains_idr と同じ方法である (3 バイトの開始コード 00 00 01 を探し、
// 4 バイトの開始コードは末尾の 3 バイトで見つける)。contains_idr は IDR があるかだけを返す private な関数で、ここでは
// NAL の位置と長さが要るので、同じ走査をここにも書く。
inline AnnexBUnits scan_annex_b(const std::uint8_t *data, std::size_t len)
{
	AnnexBUnits out;
	if (data == nullptr || len < 4)
		return out;

	// 末尾の 0 は、before_start_code が真のとき (すぐ後ろに次の開始コードがあるとき) だけ除く。
	auto push_unit = [&out, data](std::size_t begin, std::size_t end, bool before_start_code) {
		// 4 バイトの開始コード (00 00 00 01) の 1 バイト目は、直前の NAL の末尾に見える。
		// 除いてよいのはそこだけである。NAL の最後のバイトには必ず rbsp_stop_one_bit があるので、ここで 0 を
		// 除いても NAL の中身は削れない。除かないと、同じ SPS と PPS から組み立てた avcC が、エンコーダの
		// 開始コードの選び方で変わってしまう。アクセスユニットの最後の NAL の後ろには開始コードがなく、
		// そこにある 0 (cabac_zero_word や trailing_zero_8bits) は受け取ったデータの一部なので除かない。
		// 除くと、受け取ったアクセスユニットとファイルに入るアクセスユニットがバイト単位で一致しなくなる
		// (再エンコードしていないことを、この一致で確認する)。
		if (before_start_code) {
			while (end > begin && data[end - 1] == 0x00)
				--end;
		}
		if (end <= begin)
			return;
		const std::uint8_t type = nal_type_of(data[begin]);
		if (type == kNalTypeIdr)
			out.has_idr = true;
		if (type == kNalTypeSps && !out.sps_index.has_value())
			out.sps_index = out.units.size();
		if (type == kNalTypePps && !out.pps_index.has_value())
			out.pps_index = out.units.size();
		out.units.emplace_back(begin, end - begin);
	};

	std::size_t i = 0;
	std::size_t begin = 0;
	bool in_nal = false;
	while (i + 3 <= len) {
		if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01) {
			if (in_nal)
				push_unit(begin, i, true);
			begin = i + 3;
			in_nal = true;
			i = begin;
			continue;
		}
		++i;
	}
	if (in_nal)
		push_unit(begin, len, false); // アクセスユニットの最後の NAL。末尾の 0 も残す
	return out;
}

// IDR を含むアクセスユニットに、SPS と PPS も入っているか。偽なら、そのアクセスユニットからセグメントを開いても avcC を
// 組み立てられない (送信側は、IDR に SPS と PPS がないときに前に付けて送る)。
inline bool idr_has_parameter_sets(const AnnexBUnits &units)
{
	return units.has_idr && units.sps_index.has_value() && units.pps_index.has_value();
}

// SPS と PPS から avcC を組み立てる。組み立てられなければ値を持たない。
// 渡された NAL の型 (SPS は 7、PPS は 8) を確認する。SPS と PPS を取り違えた avcC は、録画ログにもファイルにも同じ値で
// 入るので、avcc_hex を比べても見つからないためである。
inline std::optional<std::vector<std::uint8_t>> build_avcc(const std::uint8_t *sps, std::size_t sps_len,
							   const std::uint8_t *pps, std::size_t pps_len)
{
	// SPS には、NAL ヘッダーの 1 バイトと、profile_idc、constraint、level_idc の 3 バイトが要る。
	if (sps == nullptr || pps == nullptr || sps_len < 4 || pps_len < 1)
		return std::nullopt;
	if (sps_len > 0xFFFF || pps_len > 0xFFFF)
		return std::nullopt;
	// NAL の型を確認する (上のコメント)。
	if (nal_type_of(sps[0]) != kNalTypeSps || nal_type_of(pps[0]) != kNalTypePps)
		return std::nullopt;

	std::vector<std::uint8_t> out;
	out.reserve(11 + sps_len + pps_len);
	out.push_back(0x01);   // configurationVersion
	out.push_back(sps[1]); // AVCProfileIndication
	out.push_back(sps[2]); // profile_compatibility
	out.push_back(sps[3]); // AVCLevelIndication
	out.push_back(0xFF);   // 予約 6 ビット + lengthSizeMinusOne = 3 (長さは 4 バイト)
	out.push_back(0xE1);   // 予約 3 ビット + numOfSequenceParameterSets = 1
	out.push_back(static_cast<std::uint8_t>((sps_len >> 8) & 0xFF));
	out.push_back(static_cast<std::uint8_t>(sps_len & 0xFF));
	out.insert(out.end(), sps, sps + sps_len);
	out.push_back(0x01); // numOfPictureParameterSets = 1
	out.push_back(static_cast<std::uint8_t>((pps_len >> 8) & 0xFF));
	out.push_back(static_cast<std::uint8_t>(pps_len & 0xFF));
	out.insert(out.end(), pps, pps + pps_len);
	return out;
}

// Annex B のデータから avcC を組み立てる (セグメントを開くフレームのデータを、そのまま渡せる)。
inline std::optional<std::vector<std::uint8_t>> build_avcc_from_annex_b(const std::uint8_t *data, std::size_t len)
{
	const AnnexBUnits scanned = scan_annex_b(data, len);
	if (!scanned.sps_index.has_value() || !scanned.pps_index.has_value())
		return std::nullopt;
	const auto &sps = scanned.units[*scanned.sps_index];
	const auto &pps = scanned.units[*scanned.pps_index];
	return build_avcc(data + sps.first, sps.second, data + pps.first, pps.second);
}

// SPS が示す表示の解像度 (クロップした後の値)。
struct SpsDimensions {
	int width = 0;
	int height = 0;
};

namespace detail {

// RBSP を 1 ビットずつ読む (指数ゴロム符号も読む)。データの終わりを超えて読むと failed() が真になる。
class SpsBitReader {
public:
	explicit SpsBitReader(std::vector<std::uint8_t> rbsp) : bytes_(std::move(rbsp)) {}

	bool failed() const { return failed_; }

	std::uint32_t bit()
	{
		if (position_ >= bytes_.size() * 8) {
			failed_ = true;
			return 0;
		}
		const std::uint8_t byte = bytes_[position_ / 8];
		const std::uint32_t value = (byte >> (7 - (position_ % 8))) & 1u;
		++position_;
		return value;
	}

	std::uint32_t bits(int count)
	{
		std::uint32_t value = 0;
		for (int i = 0; i < count; ++i)
			value = (value << 1) | bit();
		return value;
	}

	// ue(v)。32 ビットを超える値は H.264 の SPS には現れないので壊れた入力として扱う。
	std::uint32_t ue()
	{
		int zeros = 0;
		while (bit() == 0) {
			if (failed_ || ++zeros > 31) {
				failed_ = true;
				return 0;
			}
		}
		if (zeros == 0)
			return 0;
		return ((1u << zeros) - 1u) + bits(zeros);
	}

	std::int32_t se()
	{
		const std::uint32_t code = ue();
		const std::int32_t magnitude = static_cast<std::int32_t>((code + 1) / 2);
		return (code % 2 == 1) ? magnitude : -magnitude;
	}

private:
	std::vector<std::uint8_t> bytes_;
	std::size_t position_ = 0;
	bool failed_ = false;
};

// 00 00 03 の 03 (エミュレーション防止バイト) を除いて RBSP に戻す。
inline std::vector<std::uint8_t> sps_to_rbsp(const std::uint8_t *data, std::size_t len)
{
	std::vector<std::uint8_t> out;
	out.reserve(len);
	int zeros = 0;
	for (std::size_t i = 0; i < len; ++i) {
		if (zeros >= 2 && data[i] == 0x03) {
			zeros = 0;
			continue;
		}
		out.push_back(data[i]);
		zeros = data[i] == 0x00 ? zeros + 1 : 0;
	}
	return out;
}

// scaling_list() を読み飛ばす (値は使わないが、長さが可変なので中身をたどる)。
inline void skip_scaling_list(SpsBitReader &reader, int size)
{
	std::int32_t last = 8;
	std::int32_t next = 8;
	for (int j = 0; j < size && !reader.failed(); ++j) {
		if (next != 0)
			next = (last + reader.se() + 256) % 256;
		last = next == 0 ? last : next;
	}
}

} // namespace detail

// SPS (NAL ヘッダーの 1 バイト目から) を読み、表示の解像度を返す。読めなければ値を持たない。
// 黒埋め用のフレームの解像度は、configure のコピーではなくここから取る。エンコーダを作った直後に作る黒埋め用の
// フレームは、新しい解像度の configure より先に届くので、コピーはまだ前の解像度を示している (コピーを使うと、
// 1280x720 の黒埋め用のフレームの行が 1920x1080 になる)。読めないときは、呼び出し元がコピーの解像度を使う。
inline std::optional<SpsDimensions> parse_sps_dimensions(const std::uint8_t *sps, std::size_t len)
{
	if (sps == nullptr || len < 4 || nal_type_of(sps[0]) != kNalTypeSps)
		return std::nullopt;
	detail::SpsBitReader reader(detail::sps_to_rbsp(sps + 1, len - 1));

	const std::uint32_t profile_idc = reader.bits(8);
	reader.bits(8); // constraint_set フラグと予約
	reader.bits(8); // level_idc
	reader.ue();    // seq_parameter_set_id (読み飛ばす)
	std::uint32_t chroma_format_idc = 1; // ないときの既定は 4:2:0
	bool separate_colour_plane = false;
	switch (profile_idc) {
	case 100: case 110: case 122: case 244: case 44: case 83:
	case 86: case 118: case 128: case 138: case 139: case 134: case 135:
		chroma_format_idc = reader.ue();
		if (chroma_format_idc == 3)
			separate_colour_plane = reader.bit() != 0;
		reader.ue();  // bit_depth_luma_minus8 (読み飛ばす)
		reader.ue();  // bit_depth_chroma_minus8 (読み飛ばす)
		reader.bit(); // qpprime_y_zero_transform_bypass_flag (読み飛ばす)
		if (reader.bit() != 0) { // seq_scaling_matrix_present_flag (真なら行列を読み飛ばす)
			const int lists = chroma_format_idc == 3 ? 12 : 8;
			for (int i = 0; i < lists && !reader.failed(); ++i) {
				if (reader.bit() != 0)
					detail::skip_scaling_list(reader, i < 6 ? 16 : 64);
			}
		}
		break;
	default:
		break;
	}

	reader.ue(); // log2_max_frame_num_minus4 (読み飛ばす)
	const std::uint32_t poc_type = reader.ue();
	if (poc_type == 0) {
		reader.ue(); // log2_max_pic_order_cnt_lsb_minus4 (読み飛ばす)
	} else if (poc_type == 1) {
		reader.bit(); // delta_pic_order_always_zero_flag (読み飛ばす)
		reader.se();  // offset_for_non_ref_pic (読み飛ばす)
		reader.se();  // offset_for_top_to_bottom_field (読み飛ばす)
		const std::uint32_t cycle = reader.ue();
		for (std::uint32_t i = 0; i < cycle && !reader.failed(); ++i)
			reader.se();
	}
	reader.ue();  // max_num_ref_frames (読み飛ばす)
	reader.bit(); // gaps_in_frame_num_value_allowed_flag (読み飛ばす)
	const std::uint32_t width_in_mbs = reader.ue() + 1;
	const std::uint32_t height_in_map_units = reader.ue() + 1;
	const std::uint32_t frame_mbs_only = reader.bit();
	if (frame_mbs_only == 0)
		reader.bit(); // mb_adaptive_frame_field_flag (読み飛ばす)
	reader.bit();         // direct_8x8_inference_flag (読み飛ばす)
	std::uint32_t crop_left = 0;
	std::uint32_t crop_right = 0;
	std::uint32_t crop_top = 0;
	std::uint32_t crop_bottom = 0;
	if (reader.bit() != 0) { // frame_cropping_flag (真ならクロップの量を読む)
		crop_left = reader.ue();
		crop_right = reader.ue();
		crop_top = reader.ue();
		crop_bottom = reader.ue();
	}
	if (reader.failed())
		return std::nullopt;

	// クロップの単位 (H.264 の 7.4.2.1.1)。
	const std::uint32_t chroma_array_type = separate_colour_plane ? 0 : chroma_format_idc;
	const std::uint32_t sub_width = (chroma_array_type == 1 || chroma_array_type == 2) ? 2 : 1;
	const std::uint32_t sub_height = chroma_array_type == 1 ? 2 : 1;
	const std::uint32_t crop_unit_x = chroma_array_type == 0 ? 1 : sub_width;
	const std::uint32_t crop_unit_y = (chroma_array_type == 0 ? 1 : sub_height) * (2 - frame_mbs_only);

	const std::int64_t width = static_cast<std::int64_t>(width_in_mbs) * 16 -
				   static_cast<std::int64_t>(crop_left + crop_right) * crop_unit_x;
	const std::int64_t height = static_cast<std::int64_t>(2 - frame_mbs_only) * height_in_map_units * 16 -
				    static_cast<std::int64_t>(crop_top + crop_bottom) * crop_unit_y;
	if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
		return std::nullopt;
	SpsDimensions out;
	out.width = static_cast<int>(width);
	out.height = static_cast<int>(height);
	return out;
}

// Annex B のデータの中の、最初の SPS から解像度を読む。
inline std::optional<SpsDimensions> sps_dimensions_from_annex_b(const std::uint8_t *data, std::size_t len)
{
	const AnnexBUnits scanned = scan_annex_b(data, len);
	if (!scanned.sps_index.has_value())
		return std::nullopt;
	const auto &sps = scanned.units[*scanned.sps_index];
	return parse_sps_dimensions(data + sps.first, sps.second);
}

// サンプルを詰め替える (Annex B から、4 バイトの長さを前に付けた形へ)。SPS と PPS はサンプルの中に残す。4 バイトの長さを
// 前に付けた形は、MP4 と MOV の H.264 のサンプルの形 (ISO/IEC 14496-15) である。
inline std::vector<std::uint8_t> annex_b_to_avcc(const std::uint8_t *data, std::size_t len)
{
	std::vector<std::uint8_t> out;
	const AnnexBUnits scanned = scan_annex_b(data, len);
	if (scanned.units.empty())
		return out;

	std::size_t total = 0;
	for (const auto &unit : scanned.units)
		total += 4 + unit.second;
	out.reserve(total);

	for (const auto &unit : scanned.units) {
		const std::uint32_t size = static_cast<std::uint32_t>(unit.second);
		out.push_back(static_cast<std::uint8_t>((size >> 24) & 0xFF));
		out.push_back(static_cast<std::uint8_t>((size >> 16) & 0xFF));
		out.push_back(static_cast<std::uint8_t>((size >> 8) & 0xFF));
		out.push_back(static_cast<std::uint8_t>(size & 0xFF));
		out.insert(out.end(), data + unit.first, data + unit.first + unit.second);
	}
	return out;
}

// 録画ログの avcc_hex。16 進の文字は小文字に決めておく (判定スクリプトが、大文字と小文字の違いを気にせずに、
// そのまま比べられるように)。
inline std::string to_hex(const std::uint8_t *data, std::size_t len)
{
	static const char *const kDigits = "0123456789abcdef";
	std::string out;
	if (data == nullptr)
		return out;
	out.reserve(len * 2);
	for (std::size_t i = 0; i < len; ++i) {
		out.push_back(kDigits[(data[i] >> 4) & 0x0F]);
		out.push_back(kDigits[data[i] & 0x0F]);
	}
	return out;
}

inline std::string to_hex(const std::vector<std::uint8_t> &bytes)
{
	return to_hex(bytes.data(), bytes.size());
}

// 作成方法を決めるための比較。1 つでも違えば、その人は ProRes で録画ファイルを作成する。空の並び (比べるものが 1 つもない) は
// 真を返す。作成方法は record_branch.h の decide_branch が決め、黒埋め用のフレームがないことは
// any_material_missing で別に渡る。
inline bool avcc_all_equal(const std::vector<std::string> &hexes)
{
	for (std::size_t i = 1; i < hexes.size(); ++i) {
		if (hexes[i] != hexes[0])
			return false;
	}
	return true;
}

} // namespace cordmux::record
