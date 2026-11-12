// SPDX-License-Identifier: Apache-2.0

// record/record_writer (セグメントを書く SegmentWriter と、読み出しの関数) のテスト。
// 書いている途中のセグメントが読めること、mov の avcC が録画ログの avcc_hex と 1 バイトも違わないこと、
// 音声だけのセグメントもフラグメントに分けて書くこと、生の PCM で後の書き込みが勝つことを確認する。

#include "doctest.h"

#include "record/record_avcc.h"
#include "record/record_writer.h"
#include "test_record_io_shared.h"

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244) // FFmpeg のヘッダーの暗黙の型変換の警告
#endif
extern "C" {
#include <libavformat/avformat.h>
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

using cordmux::record::annex_b_to_avcc;
using cordmux::record::build_avcc_from_annex_b;
using cordmux::record::kVideoTimescale;
using cordmux::record::OutputProbe;
using cordmux::record::probe_output;
using cordmux::record::RawFile;
using cordmux::record::read_segment_pcm;
using cordmux::record::record_avformat_major;
using cordmux::record::SegmentAudioSpec;
using cordmux::record::SegmentPcmInfo;
using cordmux::record::SegmentVideoSpec;
using cordmux::record::SegmentWriteStats;
using cordmux::record::SegmentWriter;
using cordmux::record::to_hex;
using cordmux::tests::record_io::make_audio_chunk;
using cordmux::tests::record_io::read_h264_fixture;
using cordmux::tests::record_io::split_access_units;
using cordmux::tests::record_io::TempDir;
using cordmux::tests::record_io::unit_is_keyframe;

namespace {

// テストデータは 30 fps で作ってあるが、SegmentWriter に渡す fps は呼び出し元が決める。10/1 にすると 10 フレームが
// ちょうど 1 秒になり、黒埋め用のフレーム (1 秒分) と同じ形のファイルを、1 つのテストデータから作れる。
constexpr int kFpsNum = 10;
constexpr int kFpsDen = 1;
constexpr std::int64_t kFramePeriodUs = 100000;

// mov に入った extradata を読み出す (録画ログの avcc_hex と 1 バイトも違わないことを確認するため)。
std::vector<std::uint8_t> read_extradata(const std::string &path)
{
	std::vector<std::uint8_t> out;
	AVFormatContext *ctx = nullptr;
	if (avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) < 0)
		return out;
	if (avformat_find_stream_info(ctx, nullptr) >= 0) {
		for (unsigned i = 0; i < ctx->nb_streams; ++i) {
			const AVCodecParameters *par = ctx->streams[i]->codecpar;
			if (par->codec_type == AVMEDIA_TYPE_VIDEO && par->extradata_size > 0) {
				out.assign(par->extradata, par->extradata + par->extradata_size);
				break;
			}
		}
	}
	avformat_close_input(&ctx);
	return out;
}

struct VideoMaterial {
	std::vector<std::vector<std::uint8_t>> units;
	std::vector<std::uint8_t> avcc;
};

VideoMaterial load_material()
{
	VideoMaterial material;
	material.units = split_access_units(read_h264_fixture());
	REQUIRE(material.units.size() == 10);
	REQUIRE(unit_is_keyframe(material.units[0]));
	const auto built = build_avcc_from_annex_b(material.units[0].data(), material.units[0].size());
	REQUIRE(built.has_value());
	material.avcc = *built;
	return material;
}

SegmentVideoSpec spec_for(const VideoMaterial &material)
{
	SegmentVideoSpec spec;
	spec.avcc = material.avcc;
	spec.width = 64;
	spec.height = 64;
	spec.fps_num = kFpsNum;
	spec.fps_den = kFpsDen;
	return spec;
}

} // namespace

TEST_CASE("リンクした avformat のメジャーバージョンが obs-deps と同じ 61 である")
{
	// obs-deps 以外の FFmpeg をリンクしていないことを確認する。
	CHECK(record_avformat_major() == 61);
}

TEST_CASE("10 fps で 10 フレームを書いたセグメントは、ちょうど 1 秒の長さで閉じる")
{
	const TempDir dir("writer");
	const VideoMaterial material = load_material();
	const std::string file = dir.child("segment.mov");

	SegmentWriter writer;
	REQUIRE(writer.open_video(file, spec_for(material)));
	CHECK(writer.timescale() == kVideoTimescale);
	for (std::size_t i = 0; i < material.units.size(); ++i) {
		const std::vector<std::uint8_t> sample =
			annex_b_to_avcc(material.units[i].data(), material.units[i].size());
		REQUIRE(writer.write_video(sample.data(), sample.size(),
					   static_cast<std::int64_t>(i) * kFramePeriodUs,
					   unit_is_keyframe(material.units[i])));
	}
	SegmentWriteStats stats;
	REQUIRE(writer.close(&stats));

	CHECK(stats.sample_count == 10);
	// 10 フレーム x (60000 / 10) = 60000 で、タイムスケール 60000 の 1.000 秒。長さは整数で比べる。
	CHECK(stats.duration_ticks == 60000);
	CHECK(stats.timescale == kVideoTimescale);
	CHECK(stats.bytes > 0);
	CHECK(stats.nonmonotonic_pushed == 0);

	const OutputProbe probe = probe_output(file, kVideoTimescale);
	CHECK(probe.ok);
	CHECK(probe.has_video);
	CHECK_FALSE(probe.has_audio);
	CHECK(probe.frames == 10);
	CHECK(probe.duration_ticks == 60000);
}

TEST_CASE("mov に入った avcC が、録画ログに書く avcc_hex と 1 バイトも違わない")
{
	const TempDir dir("avcc");
	const VideoMaterial material = load_material();
	const std::string file = dir.child("segment.mov");

	SegmentWriter writer;
	REQUIRE(writer.open_video(file, spec_for(material)));
	const std::vector<std::uint8_t> sample =
		annex_b_to_avcc(material.units[0].data(), material.units[0].size());
	REQUIRE(writer.write_video(sample.data(), sample.size(), 0, true));
	REQUIRE(writer.close(nullptr));

	const std::vector<std::uint8_t> written = read_extradata(file);
	REQUIRE_FALSE(written.empty());
	// 録画ファイルの作成は録画ログの avcc_hex を比べて Remux で並べられるかを決めるので、mov の中の avcC と 1 バイトも違って
	// はならない。avcC を muxer に作らせずに自分で作るのはそのためである。
	CHECK(to_hex(written) == to_hex(material.avcc));
}

TEST_CASE("閉じる前のセグメントのファイルも、書いた所までの大半を読める")
{
	const TempDir dir("truncated");
	const VideoMaterial material = load_material();
	const std::string file = dir.child("segment.mov");
	const std::string copy = dir.child("segment_copy.mov");

	SegmentWriter writer;
	REQUIRE(writer.open_video(file, spec_for(material)));
	// 10 フレームの並びを 6 回書く (60 フレーム = 6 秒)。キーフレームは 6 フレームごとに来る。
	std::int64_t pts_us = 0;
	for (int round = 0; round < 6; ++round) {
		for (std::size_t i = 0; i < material.units.size(); ++i) {
			const std::vector<std::uint8_t> sample =
				annex_b_to_avcc(material.units[i].data(), material.units[i].size());
			REQUIRE(writer.write_video(sample.data(), sample.size(), pts_us,
						   unit_is_keyframe(material.units[i])));
			pts_us += kFramePeriodUs;
		}
	}

	// 閉じる前にコピーを取り、av_write_trailer を通らないファイル (OBS か PC が止まったセッションと同じ形) を作る。
	// プロセスを強制終了しなくても、同じ形のファイルを確実に作れる。
	std::error_code ec;
	std::filesystem::copy_file(std::filesystem::u8path(file), std::filesystem::u8path(copy),
				   std::filesystem::copy_options::overwrite_existing, ec);
	REQUIRE_FALSE(static_cast<bool>(ec));
	writer.abort();

	const OutputProbe probe = probe_output(copy, kVideoTimescale);
	CHECK(probe.ok);
	CHECK(probe.has_video);
	// フラグメントに分けて書いているので、最後の 1 フラグメントが欠けても大半を読める。
	CHECK(probe.frames > 30);
	CHECK(probe.frames <= 60);
}

TEST_CASE("音声だけのセグメントもフラグメントに分けて書くので、閉じる前のファイルを読める")
{
	const TempDir dir("audio");
	const std::string file = dir.child("voice.mov");
	const std::string copy = dir.child("voice_copy.mov");

	SegmentWriter writer;
	SegmentAudioSpec spec;
	REQUIRE(writer.open_audio(file, spec));
	CHECK(writer.timescale() == 48000);
	// 10 ms のチャンクを 300 個書く (3 秒)。
	const std::vector<std::uint8_t> chunk = make_audio_chunk(480, 0.25f);
	for (int i = 0; i < 300; ++i)
		REQUIRE(writer.write_audio(chunk.data(), chunk.size(), 480));

	std::error_code ec;
	std::filesystem::copy_file(std::filesystem::u8path(file), std::filesystem::u8path(copy),
				   std::filesystem::copy_options::overwrite_existing, ec);
	REQUIRE_FALSE(static_cast<bool>(ec));

	SegmentWriteStats stats;
	REQUIRE(writer.close(&stats));
	CHECK(stats.sample_count == 144000);
	CHECK(stats.duration_ticks == 144000);

	// フラグメントが 1 つしかないと、ここで 0 サンプルになる。
	const OutputProbe partial = probe_output(copy, 48000);
	CHECK(partial.ok);
	CHECK(partial.has_audio);
	CHECK(partial.audio_samples > 48000);
}

TEST_CASE("セグメントから読み出した PCM が、書いた PCM と 1 バイトも違わない")
{
	const TempDir dir("pcm");
	const std::string file = dir.child("audio.mov");

	std::vector<std::uint8_t> sent;
	{
		SegmentWriter writer;
		SegmentAudioSpec spec;
		REQUIRE(writer.open_audio(file, spec));
		for (int i = 0; i < 5; ++i) {
			const std::vector<std::uint8_t> chunk =
				make_audio_chunk(480, 0.1f * static_cast<float>(i + 1));
			sent.insert(sent.end(), chunk.begin(), chunk.end());
			REQUIRE(writer.write_audio(chunk.data(), chunk.size(), 480));
		}
		REQUIRE(writer.close(nullptr));
	}

	std::vector<std::uint8_t> back;
	SegmentPcmInfo info;
	REQUIRE(read_segment_pcm(
		file,
		[&back](const void *data, std::size_t len) {
			const std::uint8_t *bytes = static_cast<const std::uint8_t *>(data);
			back.insert(back.end(), bytes, bytes + len);
			return true;
		},
		&info));
	CHECK(info.sample_rate == 48000);
	CHECK(info.channels == 2);
	CHECK(info.samples == 2400);
	REQUIRE(back.size() == sent.size());
	CHECK(std::memcmp(back.data(), sent.data(), sent.size()) == 0);
}

TEST_CASE("生の PCM のファイルは 0 で埋まり、範囲が重なると後の書き込みが勝つ")
{
	const TempDir dir("raw");
	const std::string file = dir.child("audio.pcm");

	const std::vector<std::uint8_t> first(16, 0x11);
	const std::vector<std::uint8_t> second(16, 0x22);
	{
		RawFile raw;
		REQUIRE(raw.create_zeroed(file, 64));
		REQUIRE(raw.write_at(8, first.data(), first.size()));
		// 範囲が重なる書き込み。音声が早く届いて原点を置き直したセッションで起きる。
		REQUIRE(raw.write_at(16, second.data(), second.size()));
		REQUIRE(raw.close());
	}

	std::string body;
	REQUIRE(cordmux::record::read_whole_file(file, &body));
	REQUIRE(body.size() == 64);
	CHECK(static_cast<std::uint8_t>(body[0]) == 0x00);
	CHECK(static_cast<std::uint8_t>(body[8]) == 0x11);
	CHECK(static_cast<std::uint8_t>(body[15]) == 0x11);
	CHECK(static_cast<std::uint8_t>(body[16]) == 0x22); // 後の書き込みが勝つ
	CHECK(static_cast<std::uint8_t>(body[31]) == 0x22);
	CHECK(static_cast<std::uint8_t>(body[32]) == 0x00);
}

TEST_CASE("RawFile::open_append で開いたファイルは、切り詰めずに末尾に追記できる")
{
	const TempDir dir("append");
	const std::string file = dir.child("ledger.jsonl");

	{
		RawFile raw;
		REQUIRE(raw.create_zeroed(file, 0));
		REQUIRE(raw.write_at(0, "one\n", 4));
		REQUIRE(raw.close());
	}
	{
		RawFile raw;
		REQUIRE(raw.open_append(file));
		REQUIRE(raw.append("two\n", 4));
		REQUIRE(raw.close());
	}

	std::string body;
	REQUIRE(cordmux::record::read_whole_file(file, &body));
	CHECK(body == "one\ntwo\n");
}
