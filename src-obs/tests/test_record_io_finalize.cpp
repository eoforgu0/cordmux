// SPDX-License-Identifier: Apache-2.0

// record/record_finalize.h (録画ファイルの作成) のテスト。SegmentWriter が書いたセグメントから録画ログと計画を作り、実際の
// ffmpeg で作成して、ffprobe で読んだ長さ、フレーム数、サンプル数を計画と整数で比べる。
// 環境変数 CORDMUX_FFMPEG_EXE と CORDMUX_FFPROBE_EXE が実在するファイルを指すときだけ実行し、指さなければ
// doctest::skip でスキップする。CI は 2 つの環境変数を設定し、スキップしたテストが 1 件でもあれば失敗にする。
// 指定するファイルと設定の仕方は、docs/testing.md の「C++ の単体テスト」にある。

#include "doctest.h"

#include "config/settings.h"
#include "record/record_args.h"
#include "record/record_avcc.h"
#include "record/record_branch.h"
#include "record/record_finalize.h"
#include "record/record_ledger.h"
#include "record/record_segment.h"
#include "record/record_writer.h"
#include "record/recorder.h"
#include "test_record_io_shared.h"

#include <string>
#include <vector>

using cordmux::record::annex_b_to_avcc;
using cordmux::record::build_avcc_from_annex_b;
using cordmux::record::CloseReason;
using cordmux::record::FinalizeBranch;
using cordmux::record::FinalizeCheck;
using cordmux::record::FinalizeOptions;
using cordmux::record::FinalizePlan;
using cordmux::record::FinalizeProgress;
using cordmux::record::FinalizeRun;
using cordmux::record::FinalizeState;
using cordmux::record::join_path;
using cordmux::record::kBlackDirName;
using cordmux::record::kSegmentsDirName;
using cordmux::record::LedgerBlackRow;
using cordmux::record::LedgerDocument;
using cordmux::record::LedgerFinalizeRow;
using cordmux::record::LedgerPersonRow;
using cordmux::record::LedgerSegmentRow;
using cordmux::record::make_real_record_platform_ops;
using cordmux::record::MediaKind;
using cordmux::record::RecordPlatformOps;
using cordmux::record::run_finalize;
using cordmux::record::SegmentAudioSpec;
using cordmux::record::SegmentVideoSpec;
using cordmux::record::SegmentWriteStats;
using cordmux::record::SegmentWriter;
using cordmux::record::StreamKind;
using cordmux::record::to_hex;
using cordmux::record::TrackKind;
using cordmux::tests::record_io::ffmpeg_exe;
using cordmux::tests::record_io::ffprobe_exe;
using cordmux::tests::record_io::have_real_ffmpeg;
using cordmux::tests::record_io::make_audio_chunk;
using cordmux::tests::record_io::probe_audio;
using cordmux::tests::record_io::probe_video;
using cordmux::tests::record_io::ProbeStream;
using cordmux::tests::record_io::read_h264_fixture;
using cordmux::tests::record_io::split_access_units;
using cordmux::tests::record_io::TempDir;
using cordmux::tests::record_io::unit_is_keyframe;

namespace {

// テストデータは 10 フレーム。10/1 fps とみなすので、10 フレームでちょうど 1 秒になる (test_record_io_writer.cpp と
// 同じ)。
constexpr int kFpsNum = 10;
constexpr int kFpsDen = 1;
constexpr int kVideoTimescale = 60000;
constexpr int kSampleRate = 48000;
constexpr std::int64_t kFrameTicks = 6000; // 60000 / 10

// セッションのフォルダーを作り、セグメントを書いて、録画ログを組み立てる。フォルダーの構成は製品と同じにする。
// セッションのフォルダーの名前は空白を含む「2000-01-01 12-00-00」の形、録画の中間ファイルは data/ の下、録画ファイルは
// セッションのフォルダーの直下である。
class SessionFixture {
public:
	explicit SessionFixture(const char *tag) : dir_(tag)
	{
		session_ = dir_.child("2000-01-01 12-00-00");
		materials_ = join_path(session_, cordmux::record::kMaterialsDirName);
		std::error_code ec;
		std::filesystem::create_directories(
			std::filesystem::u8path(join_path(materials_, kSegmentsDirName)), ec);
		std::filesystem::create_directories(std::filesystem::u8path(join_path(materials_, kBlackDirName)),
						    ec);
		units_ = split_access_units(read_h264_fixture());
		REQUIRE(units_.size() == 10);
		const auto built = build_avcc_from_annex_b(units_.front().data(), units_.front().size());
		REQUIRE(built.has_value());
		avcc_ = *built;

		doc_.session.schema = cordmux::record::kLedgerSchema;
		doc_.session.session_id = "2000-01-01 12-00-00";
		LedgerPersonRow person;
		person.user_id = "u1";
		person.display_name = "てすと";
		person.kinds.push_back(TrackKind::ScreenVideo);
		person.kinds.push_back(TrackKind::ScreenAudio);
		doc_.people.push_back(person);
	}

	const std::string &session_dir() const { return session_; }
	const std::string &materials_dir() const { return materials_; }
	LedgerDocument &doc() { return doc_; }

	// 黒埋め用のフレームのファイル (1 秒分) を 1 つ置く。
	void add_black_material()
	{
		const std::string file = cordmux::record::black_file_name("u1", "1-1");
		write_video_file(file, 10);
		LedgerBlackRow black;
		black.user_id = "u1";
		black.encoder_instance_id = "1-1";
		black.file = file;
		black.avcc_hex = to_hex(avcc_);
		black.width = 64;
		black.height = 64;
		black.fps_num = kFpsNum;
		black.fps_den = kFpsDen;
		black.frame_count = 10;
		black.duration_ticks = 10 * kFrameTicks;
		doc_.blacks.push_back(black);
	}

	// 映像のセグメントを 1 つ追加する (frames は 10 の倍数)。
	void add_video_segment(std::uint32_t stream_id, std::int64_t first_pts_us, std::int64_t first_steady_us,
			       int frames)
	{
		const std::string file =
			cordmux::record::segment_file_name("u1", TrackKind::ScreenVideo, ++seq_);
		write_video_file(file, frames);
		LedgerSegmentRow row;
		row.user_id = "u1";
		row.kind = StreamKind::Screen;
		row.stream_id = stream_id;
		row.encoder_instance_id = "1-1";
		row.file = file;
		row.media = MediaKind::Video;
		row.avcc_hex = to_hex(avcc_);
		row.width = 64;
		row.height = 64;
		row.fps_num = kFpsNum;
		row.fps_den = kFpsDen;
		row.timescale = kVideoTimescale;
		row.first_pts_us = first_pts_us;
		row.first_pts_steady_us = first_steady_us;
		row.last_pts_us = first_pts_us + (frames - 1) * 100000;
		row.sample_count = frames;
		row.duration_ticks = frames * kFrameTicks;
		row.starts_with_idr = true;
		row.close_reason = CloseReason::RecordingToggled;
		doc_.segments.push_back(row);
	}

	// フレームごとの時刻 (セグメントの先頭からのマイクロ秒) を指定して、映像のセグメントを 1 つ追加する。フレームの
	// 間隔がそろわないセグメント (画面が動かない間に送信側がフレームを間引いたとき) を作るのに使う。行の
	// フレーム数と長さは、SegmentWriter が数えた値にする (製品と同じ)。
	void add_video_segment_at(std::uint32_t stream_id, std::int64_t first_pts_us, std::int64_t first_steady_us,
				  const std::vector<std::int64_t> &pts_us, int fps_num = kFpsNum)
	{
		const std::string file =
			cordmux::record::segment_file_name("u1", TrackKind::ScreenVideo, ++seq_);
		const SegmentWriteStats stats = write_video_file_at(file, pts_us, fps_num);
		LedgerSegmentRow row;
		row.user_id = "u1";
		row.kind = StreamKind::Screen;
		row.stream_id = stream_id;
		row.encoder_instance_id = "1-1";
		row.file = file;
		row.media = MediaKind::Video;
		row.avcc_hex = to_hex(avcc_);
		row.width = 64;
		row.height = 64;
		row.fps_num = fps_num;
		row.fps_den = kFpsDen;
		row.timescale = kVideoTimescale;
		row.first_pts_us = first_pts_us;
		row.first_pts_steady_us = first_steady_us;
		row.last_pts_us = first_pts_us + pts_us.back();
		row.sample_count = stats.sample_count;
		row.duration_ticks = stats.duration_ticks;
		row.starts_with_idr = true;
		row.close_reason = CloseReason::RecordingToggled;
		doc_.segments.push_back(row);
	}

	// 音声のセグメントを 1 つ追加する (samples は 1 ch あたり)。
	void add_audio_segment(std::uint32_t stream_id, std::int64_t first_pts_us, std::int64_t first_steady_us,
			       std::int64_t samples)
	{
		const std::string file =
			cordmux::record::segment_file_name("u1", TrackKind::ScreenAudio, ++seq_);
		write_audio_file(file, samples);
		LedgerSegmentRow row;
		row.user_id = "u1";
		row.kind = StreamKind::Screen;
		row.stream_id = stream_id;
		row.file = file;
		row.media = MediaKind::Audio;
		row.timescale = kSampleRate;
		row.first_pts_us = first_pts_us;
		row.first_pts_steady_us = first_steady_us;
		row.sample_count = samples;
		row.duration_ticks = samples;
		row.close_reason = CloseReason::RecordingToggled;
		doc_.segments.push_back(row);
	}

	void end_at(std::int64_t ended_at_steady_us)
	{
		doc_.session.ended_at_steady_us = ended_at_steady_us;
		doc_.session.stopped_reason = cordmux::record::StopReason::TurnedOff;
	}

private:
	void write_video_file(const std::string &relative, int frames)
	{
		SegmentVideoSpec spec;
		spec.avcc = avcc_;
		spec.width = 64;
		spec.height = 64;
		spec.fps_num = kFpsNum;
		spec.fps_den = kFpsDen;
		SegmentWriter writer;
		REQUIRE(writer.open_video(join_path(materials_, relative), spec));
		for (int i = 0; i < frames; ++i) {
			const std::vector<std::uint8_t> &unit = units_[static_cast<std::size_t>(i % 10)];
			const std::vector<std::uint8_t> sample = annex_b_to_avcc(unit.data(), unit.size());
			REQUIRE(writer.write_video(sample.data(), sample.size(),
						   static_cast<std::int64_t>(i) * 100000, unit_is_keyframe(unit)));
		}
		SegmentWriteStats stats;
		REQUIRE(writer.close(&stats));
		REQUIRE(stats.sample_count == frames);
	}

	SegmentWriteStats write_video_file_at(const std::string &relative, const std::vector<std::int64_t> &pts_us,
					      int fps_num)
	{
		SegmentVideoSpec spec;
		spec.avcc = avcc_;
		spec.width = 64;
		spec.height = 64;
		spec.fps_num = fps_num;
		spec.fps_den = kFpsDen;
		SegmentWriter writer;
		REQUIRE(writer.open_video(join_path(materials_, relative), spec));
		for (std::size_t i = 0; i < pts_us.size(); ++i) {
			const std::vector<std::uint8_t> &unit = units_[i % 10];
			const std::vector<std::uint8_t> sample = annex_b_to_avcc(unit.data(), unit.size());
			REQUIRE(writer.write_video(sample.data(), sample.size(), pts_us[i], unit_is_keyframe(unit)));
		}
		SegmentWriteStats stats;
		REQUIRE(writer.close(&stats));
		REQUIRE(stats.sample_count == static_cast<std::int64_t>(pts_us.size()));
		return stats;
	}

	void write_audio_file(const std::string &relative, std::int64_t samples)
	{
		SegmentAudioSpec spec;
		SegmentWriter writer;
		REQUIRE(writer.open_audio(join_path(materials_, relative), spec));
		const std::vector<std::uint8_t> chunk = make_audio_chunk(480, 0.5f);
		for (std::int64_t written = 0; written < samples; written += 480)
			REQUIRE(writer.write_audio(chunk.data(), chunk.size(), 480));
		REQUIRE(writer.close(nullptr));
	}

	TempDir dir_;
	std::string session_;
	std::string materials_;
	std::vector<std::vector<std::uint8_t>> units_;
	std::vector<std::uint8_t> avcc_;
	LedgerDocument doc_;
	std::int64_t seq_ = 0;
};

// 実際の ffmpeg を指す RecordPlatformOps。
RecordPlatformOps real_ops_with_ffmpeg(std::shared_ptr<std::string> log)
{
	RecordPlatformOps ops = make_real_record_platform_ops(log);
	ops.ffmpeg_path = []() { return ffmpeg_exe(); };
	return ops;
}

struct FinalizeOutcome {
	FinalizeProgress progress;
	int max_permille = -1; // on_progress で受け取った、作成中の録画ファイルの割合 (0 から 1000) の最大値
	std::vector<LedgerFinalizeRow> rows;
	std::string ledger;
};

FinalizeOutcome finalize(SessionFixture &fixture, cordmux::config::RecordPcmFormat pcm)
{
	FinalizeOutcome outcome;
	auto log = std::make_shared<std::string>();
	FinalizeRun run;
	run.ops = real_ops_with_ffmpeg(log);
	run.options.ffmpeg_path = ffmpeg_exe();
	run.options.session_dir = fixture.session_dir();
	run.options.materials_dir = fixture.materials_dir();
	run.options.output_pcm = pcm;
	run.doc = &fixture.doc();
	run.append_line = [&outcome](const std::string &line) { outcome.ledger += line + "\n"; };
	run.set_child_log = [log](const std::string &path) { *log = path; };
	run.on_progress = [&outcome](const FinalizeProgress &p) {
		if (p.current_permille && *p.current_permille > outcome.max_permille)
			outcome.max_permille = *p.current_permille;
	};
	outcome.progress = run_finalize(run);
	if (outcome.progress.state == FinalizeState::Done) {
		// 実際の FFmpeg の `-progress` の出力を読めて、割合が終わりまで進んだ。最後のフレームの長さだけ
		// 録画ログの長さに届かないことがあるので、9 割で確認する。
		CHECK(outcome.max_permille >= 900);
		// 録画ファイルの作成が成功したので、一時ファイルは削除されている。
		std::error_code ec;
		for (const std::filesystem::directory_entry &entry :
		     std::filesystem::directory_iterator(std::filesystem::u8path(fixture.materials_dir()), ec)) {
			const std::string leaf = entry.path().filename().u8string();
			CHECK_FALSE(leaf.rfind("concat_", 0) == 0);
			CHECK_FALSE(leaf.rfind("progress_", 0) == 0);
			CHECK_FALSE(leaf.rfind("audio_", 0) == 0);
		}
	}
	const LedgerDocument parsed = cordmux::record::parse_ledger(outcome.ledger);
	outcome.rows = parsed.finalizes;
	if (outcome.progress.state != FinalizeState::Done) {
		std::string text;
		cordmux::record::read_whole_file(*log, &text);
		MESSAGE("child log (" << *log << "): " << text);
	}
	return outcome;
}

// 録画ログから作った計画と、実際の ffprobe が読んだ値を整数で比べる。
void check_against_ffprobe(SessionFixture &fixture, const std::string &tag)
{
	FinalizeOptions options;
	options.ffmpeg_path = ffmpeg_exe();
	options.session_dir = fixture.session_dir();
	options.materials_dir = fixture.materials_dir();
	const FinalizePlan plan =
		cordmux::record::build_finalize_plan(fixture.doc(), "u1", StreamKind::Screen, options);
	CHECK(plan.branch == FinalizeBranch::Remux);
	// 録画ファイルは、セッションのフォルダーの直下に「名前 種類 日時」の形の名前で作られる (録画の中間ファイルは
	// data/ の下)。
	CHECK(plan.output_file == join_path(fixture.session_dir(), "てすと Screen Share 2000-01-01 12-00-00.mov"));

	const std::string probe_log = join_path(fixture.session_dir(), "probe_" + tag + ".log");
	const ProbeStream video = probe_video(plan.output_file, probe_log);
	REQUIRE(video.present);
	CHECK(video.time_base_den == kVideoTimescale);
	CHECK(video.frames == plan.expected_frames);
	CHECK(video.duration_ts == plan.expected_duration_ticks);

	const ProbeStream audio = probe_audio(plan.output_file, probe_log);
	REQUIRE(audio.present);
	CHECK(audio.sample_rate == kSampleRate);
	CHECK(audio.time_base_den == kSampleRate);
	CHECK(audio.sample_count == plan.pcm_total_samples);
}

} // namespace

TEST_CASE("実際の ffmpeg で録画ファイルを作成すると、音声が先に始まったセッションは映像の先頭が黒で埋まる" *
	  doctest::skip(!have_real_ffmpeg()))
{
	SessionFixture fixture("fin_head");
	fixture.add_black_material();
	// 音声は原点から、映像は 1 秒遅れて始まる (映像の先頭に 1 秒の欠落がある)。
	fixture.add_audio_segment(1, 0, 1000000, 96000);
	fixture.add_video_segment(1, 1000000, 2000000, 10);
	fixture.end_at(3000000);

	const FinalizeOutcome outcome = finalize(fixture, cordmux::config::RecordPcmFormat::Float32);
	REQUIRE(outcome.rows.size() == 1);
	CHECK(outcome.rows.front().exit_code == 0);
	CHECK(outcome.rows.front().check == FinalizeCheck::Matched);
	CHECK(outcome.rows.front().branch == FinalizeBranch::Remux);
	// 欠落を 1 つ、黒埋め用のフレームで埋めたことが録画ログに残る。
	REQUIRE(outcome.rows.front().gaps.size() == 1);
	CHECK(outcome.rows.front().gaps.front().start_us == 0);
	CHECK(outcome.rows.front().gaps.front().repeat_count == 1);
	CHECK(outcome.progress.state == FinalizeState::Done);
	check_against_ffprobe(fixture, "head");
}

TEST_CASE("実際の ffmpeg で録画ファイルを作成すると、映像の末尾の欠落は、端数を outpoint でフレーム単位に切った黒で埋まる" *
	  doctest::skip(!have_real_ffmpeg()))
{
	SessionFixture fixture("fin_tail");
	fixture.add_black_material();
	fixture.add_video_segment(1, 0, 1000000, 10);
	fixture.add_audio_segment(1, 0, 1000000, 48000);
	// 末尾の 1.5 秒の欠落は、黒埋め用のフレーム 1 つ分 (1 秒) と、端数の 5 フレーム (outpoint の行が出る) で
	// 埋まる。
	fixture.end_at(3500000);

	const FinalizeOutcome outcome = finalize(fixture, cordmux::config::RecordPcmFormat::Float32);
	REQUIRE(outcome.rows.size() == 1);
	CHECK(outcome.rows.front().exit_code == 0);
	CHECK(outcome.rows.front().check == FinalizeCheck::Matched);
	REQUIRE(outcome.rows.front().gaps.size() == 1);
	CHECK(outcome.rows.front().gaps.front().repeat_count == 1);
	CHECK(outcome.rows.front().gaps.front().remainder_ticks == 5 * kFrameTicks);
	check_against_ffprobe(fixture, "tail");
}

TEST_CASE("実際の ffmpeg で録画ファイルを作成すると、音声が映像より短いセッションは、足りない分が無音で埋まる" *
	  doctest::skip(!have_real_ffmpeg()))
{
	SessionFixture fixture("fin_audio");
	fixture.add_black_material();
	fixture.add_video_segment(1, 0, 1000000, 20); // 2 秒
	fixture.add_audio_segment(1, 0, 1000000, 48000); // 1 秒だけ
	fixture.end_at(3000000);

	const FinalizeOutcome outcome = finalize(fixture, cordmux::config::RecordPcmFormat::Float32);
	REQUIRE(outcome.rows.size() == 1);
	CHECK(outcome.rows.front().exit_code == 0);
	CHECK(outcome.rows.front().check == FinalizeCheck::Matched);
	// 音声も映像と同じ長さにする (-shortest を付けないので、長さをそろえるのは計画を作る側である)。
	CHECK(outcome.rows.front().gaps.empty());
	check_against_ffprobe(fixture, "audio");
}

TEST_CASE("実際の ffmpeg で録画ファイルを作成すると、stream_id とタイムスタンプの基準が変わったセグメントは、受信側の単調な時計で置かれる" *
	  doctest::skip(!have_real_ffmpeg()))
{
	SessionFixture fixture("fin_reconnect");
	fixture.add_black_material();
	fixture.add_video_segment(1, 0, 1000000, 10);
	// 再接続で stream_id が変わり、あわせて「単調な時計 - タイムスタンプ」が 2 秒食い違う。タイムスタンプの差は
	// 0 なのに実際は 1 秒空いているので、基準が違うとみなして単調な時計で置く。送信側のタイムスタンプは
	// 単調な時計で進むので (docs/wire_format.md の「timestamp_us」)、再接続しただけなら基準は変わらず、
	// タイムスタンプで置く。ここは、Vesktop を起動し直して送信側の時計の原点が変わった場合に当たる。
	fixture.add_video_segment(7, 0, 3000000, 10);
	fixture.add_audio_segment(1, 0, 1000000, 48000);
	fixture.end_at(4000000);

	const FinalizeOutcome outcome = finalize(fixture, cordmux::config::RecordPcmFormat::Float32);
	REQUIRE(outcome.rows.size() == 1);
	CHECK(outcome.rows.front().exit_code == 0);
	CHECK(outcome.rows.front().check == FinalizeCheck::Matched);
	REQUIRE(outcome.rows.front().gaps.size() == 1);
	CHECK(outcome.rows.front().gaps.front().start_us == 1000000);
	check_against_ffprobe(fixture, "reconnect");
}

TEST_CASE("実際の ffmpeg で録画ファイルの PCM を in24 にして作成しても、長さが計画と一致する" *
	  doctest::skip(!have_real_ffmpeg()))
{
	SessionFixture fixture("fin_in24");
	fixture.add_black_material();
	fixture.add_video_segment(1, 0, 1000000, 10);
	fixture.add_audio_segment(1, 0, 1000000, 48000);
	fixture.end_at(2000000);

	const FinalizeOutcome outcome = finalize(fixture, cordmux::config::RecordPcmFormat::Int24);
	REQUIRE(outcome.rows.size() == 1);
	CHECK(outcome.rows.front().exit_code == 0);
	CHECK(outcome.rows.front().check == FinalizeCheck::Matched);
	check_against_ffprobe(fixture, "in24");
}

TEST_CASE("実際の ffmpeg で録画ファイルを作成すると、声だけの録画ファイルは映像を持たず、サンプルの数が計画と一致する" *
	  doctest::skip(!have_real_ffmpeg()))
{
	SessionFixture fixture("fin_voice");
	LedgerSegmentRow row;
	fixture.add_audio_segment(3, 0, 1000000, 48000);
	fixture.doc().segments.back().kind = StreamKind::Voice;
	fixture.end_at(2000000);
	(void)row;

	const FinalizeOutcome outcome = finalize(fixture, cordmux::config::RecordPcmFormat::Float32);
	REQUIRE(outcome.rows.size() == 1);
	CHECK(outcome.rows.front().kind == StreamKind::Voice);
	CHECK(outcome.rows.front().exit_code == 0);
	CHECK(outcome.rows.front().check == FinalizeCheck::Matched);

	FinalizeOptions options;
	options.session_dir = fixture.session_dir();
	options.materials_dir = fixture.materials_dir();
	const FinalizePlan plan =
		cordmux::record::build_finalize_plan(fixture.doc(), "u1", StreamKind::Voice, options);
	CHECK_FALSE(plan.has_video);
	CHECK(plan.expected_frames == 0);
	const std::string probe_log = join_path(fixture.session_dir(), "probe_voice.log");
	const ProbeStream audio = probe_audio(plan.output_file, probe_log);
	REQUIRE(audio.present);
	CHECK(audio.sample_count == plan.pcm_total_samples);
	const ProbeStream video = probe_video(plan.output_file, probe_log);
	CHECK_FALSE(video.present);
}

TEST_CASE("実際の ffmpeg で録画ファイルを作成すると、avcC が食い違う人は ProRes に再エンコードされて 1 つの録画ファイルになる" *
	  doctest::skip(!have_real_ffmpeg()))
{
	SessionFixture fixture("fin_prores");
	fixture.add_black_material();
	fixture.add_video_segment(1, 0, 1000000, 10);
	fixture.add_video_segment(1, 2000000, 3000000, 10);
	fixture.add_audio_segment(1, 0, 1000000, 48000);
	// エンコーダが作り直されて avcC が 1 バイト変わった状態にする。avcC が違うセグメントを concat でつなぐと、
	// 警告も出ず終了コードも 0 のまま、中身の食い違ったファイルができる (実測)。そのため avcC が違えば ProRes で
	// 録画ファイルを作成する。
	fixture.doc().segments[1].avcc_hex += "00";
	fixture.end_at(4000000);

	const FinalizeOutcome outcome = finalize(fixture, cordmux::config::RecordPcmFormat::Float32);
	REQUIRE(outcome.rows.size() == 1);
	CHECK(outcome.rows.front().branch == FinalizeBranch::ProRes);

	// ProRes への再エンコードを実際の ffmpeg で最後まで実行し、録画ファイルが計画と一致することを確認する。
	// フィルターのスクリプトの pad は `pad=64:64:...` の形で書く (FFmpeg は `pad=64x64` を受け付けない)。
	CHECK(outcome.rows.front().exit_code == 0);
	CHECK(outcome.rows.front().check == FinalizeCheck::Matched);
	CHECK(outcome.progress.state == FinalizeState::Done);
	CHECK(outcome.progress.any_reencode);

	// 成功したときも、セグメントのファイルは削除しない。
	std::error_code ec;
	for (const LedgerSegmentRow &row : fixture.doc().segments) {
		CHECK(std::filesystem::exists(
			std::filesystem::u8path(join_path(fixture.materials_dir(), row.file)), ec));
	}
	FinalizeOptions options;
	options.session_dir = fixture.session_dir();
	options.materials_dir = fixture.materials_dir();
	const FinalizePlan plan =
		cordmux::record::build_finalize_plan(fixture.doc(), "u1", StreamKind::Screen, options);
	CHECK(plan.branch == FinalizeBranch::ProRes);
	// フィルターのスクリプトは、セグメント 2 つと欠落 1 つの 3 つを並べて concat する。
	CHECK(plan.filter_script.find("concat=n=3:v=1:a=0,setpts=N/(10)/TB[vout]") != std::string::npos);

	// 録画ファイルを ffprobe で数え直す。check_against_ffprobe は Remux で作成した録画ファイルしか確認しないので、
	// ここでは同じ計画の整数と直接比べる。
	const std::string probe_log = join_path(fixture.session_dir(), "probe_prores.log");
	const ProbeStream video = probe_video(plan.output_file, probe_log);
	REQUIRE(video.present);
	CHECK(video.frames == plan.expected_frames);
	// 録画ファイルのタイムスケールは録画ログのタイムスケールにそろい、録画ログの行は録画ファイルと同じ
	// タイムスケールと長さを示す。-video_track_timescale を付けないと、mov の muxer が fps から別の
	// タイムスケールを選ぶ。
	CHECK(video.time_base_den == plan.expected_timescale);
	CHECK(outcome.rows.front().output_timescale == video.time_base_den);
	CHECK(outcome.rows.front().output_duration_ticks == video.duration_ts);
	const ProbeStream audio = probe_audio(plan.output_file, probe_log);
	REQUIRE(audio.present);
	CHECK(audio.sample_count == plan.pcm_total_samples);
}

namespace {

// 24 フレームのセグメントで、各フレームの時刻。10 fps の間隔で、12 フレーム目の後に 1 度だけ 200 ms 空く。
std::vector<std::int64_t> uneven_pts_24()
{
	std::vector<std::int64_t> pts;
	for (int i = 0; i < 24; ++i)
		pts.push_back(static_cast<std::int64_t>(i) * 100000 + (i >= 12 ? 100000 : 0));
	return pts;
}

} // namespace

TEST_CASE("実際の ffmpeg で録画ファイルを作成すると、フレームの間隔がそろわない人も ProRes に再エンコードされて Matched になる" *
	  doctest::skip(!have_real_ffmpeg()))
{
	SessionFixture fixture("fin_prores_uneven");
	// 黒埋め用のフレームがない人 (EncoderCreated のフレームが届かなかった) は、ProRes で録画ファイルを作成する。
	fixture.add_video_segment_at(1, 0, 1000000, uneven_pts_24());
	fixture.add_audio_segment(1, 0, 1000000, 120000); // 2.5 秒
	fixture.end_at(3500000);
	const LedgerSegmentRow &row = fixture.doc().segments.front();
	REQUIRE(row.sample_count == 24);
	REQUIRE(row.duration_ticks == 150000); // 最後のフレーム (2.4 秒) + 1 フレーム = 2.5 秒

	FinalizeOptions options;
	options.session_dir = fixture.session_dir();
	options.materials_dir = fixture.materials_dir();
	const FinalizePlan plan =
		cordmux::record::build_finalize_plan(fixture.doc(), "u1", StreamKind::Screen, options);
	REQUIRE(plan.branch == FinalizeBranch::ProRes);
	CHECK(plan.expected_frames == 25); // fps のフィルターは 2.5 秒を 10 fps の 25 フレームにする

	const FinalizeOutcome outcome = finalize(fixture, cordmux::config::RecordPcmFormat::Float32);
	REQUIRE(outcome.rows.size() == 1);
	CHECK(outcome.rows.front().branch == FinalizeBranch::ProRes);
	CHECK(outcome.rows.front().exit_code == 0);
	CHECK(outcome.rows.front().check == FinalizeCheck::Matched);
	CHECK(outcome.progress.state == FinalizeState::Done);

	// 録画ファイルのフレーム数は、fps のフィルターの数え方どおりになる。
	const ProbeStream video = probe_video(plan.output_file, join_path(fixture.session_dir(), "probe_uneven.log"));
	REQUIRE(video.present);
	CHECK(video.frames == 25);
	const ProbeStream audio = probe_audio(plan.output_file, join_path(fixture.session_dir(), "probe_uneven.log"));
	REQUIRE(audio.present);
	CHECK(audio.sample_count == plan.pcm_total_samples);
}

TEST_CASE("実際の ffmpeg で作成しても、フレームが欠けた ProRes の録画ファイルは DurationMismatch で失敗する" *
	  doctest::skip(!have_real_ffmpeg()))
{
	SessionFixture fixture("fin_prores_short");
	fixture.add_video_segment_at(1, 0, 1000000, uneven_pts_24());
	fixture.add_audio_segment(1, 0, 1000000, 192000); // 4 秒
	// 録画ログは 4 秒、40 フレームを示すが、セグメントのファイルには 2.5 秒分しかない (セグメントが途中で切れた
	// 状態)。
	// fps のフィルターはファイルにある長さの分 (25 フレーム) しか出力せず、期待の 40 フレームに、許容の幅
	// (frame_tolerance = 1) を超えて足りない。
	LedgerSegmentRow &row = fixture.doc().segments.front();
	row.sample_count = 40;
	row.duration_ticks = 240000;
	fixture.end_at(5000000);

	FinalizeOptions options;
	options.session_dir = fixture.session_dir();
	options.materials_dir = fixture.materials_dir();
	const FinalizePlan plan =
		cordmux::record::build_finalize_plan(fixture.doc(), "u1", StreamKind::Screen, options);
	REQUIRE(plan.branch == FinalizeBranch::ProRes);
	REQUIRE(plan.expected_frames == 40);
	REQUIRE(plan.frame_tolerance == 1);

	const FinalizeOutcome outcome = finalize(fixture, cordmux::config::RecordPcmFormat::Float32);
	REQUIRE(outcome.rows.size() == 1);
	CHECK(outcome.rows.front().exit_code == 0); // FFmpeg は終了コード 0 のまま、短い録画ファイルを書く
	CHECK(outcome.rows.front().check == FinalizeCheck::DurationMismatch);
	CHECK(outcome.progress.state == FinalizeState::Failed);
	const ProbeStream video = probe_video(plan.output_file, join_path(fixture.session_dir(), "probe_short.log"));
	REQUIRE(video.present);
	CHECK(video.frames < plan.expected_frames - plan.frame_tolerance);
}

TEST_CASE("同梱の FFmpeg の場所が空なら録画ファイルの作成は失敗になり、セグメントのファイルは削除しない" *
	  doctest::skip(!have_real_ffmpeg()))
{
	SessionFixture fixture("fin_fail");
	fixture.add_black_material();
	fixture.add_video_segment(1, 0, 1000000, 10);
	fixture.add_audio_segment(1, 0, 1000000, 48000);
	fixture.end_at(2000000);

	auto log = std::make_shared<std::string>();
	std::string ledger;
	FinalizeRun run;
	run.ops = make_real_record_platform_ops(log);
	run.ops.ffmpeg_path = []() { return std::string(); }; // 同梱の FFmpeg が見つからない状態
	run.options.session_dir = fixture.session_dir();
	run.options.materials_dir = fixture.materials_dir();
	run.doc = &fixture.doc();
	run.append_line = [&ledger](const std::string &line) { ledger += line + "\n"; };
	const FinalizeProgress progress = run_finalize(run);

	CHECK(progress.state == FinalizeState::Failed);
	CHECK(progress.files_failed == 1);
	CHECK(progress.ffmpeg_unavailable);
	// セグメントのファイルは削除しない。
	std::error_code ec;
	for (const LedgerSegmentRow &row : fixture.doc().segments) {
		CHECK(std::filesystem::exists(
			std::filesystem::u8path(join_path(fixture.materials_dir(), row.file)), ec));
	}
}

namespace {

// 60 fps のセグメントにある `count` 個のフレームの時刻 (マイクロ秒)。
std::vector<std::int64_t> pts_60fps(int count)
{
	std::vector<std::int64_t> pts;
	for (int i = 0; i < count; ++i)
		pts.push_back(static_cast<std::int64_t>(i) * 1000000 / 60);
	return pts;
}

} // namespace

TEST_CASE("実際の ffmpeg で録画ファイルを作成すると、60 fps のセグメントの後に 10 fps のセグメントがある人も ProRes で Matched になる" *
	  doctest::skip(!have_real_ffmpeg()))
{
	SessionFixture fixture("fin_prores_mixed_fps");
	// 画面共有の途中で fps と avcC が変わった人 (エンコーダが作り直された状態)。計画の fps は最初のセグメントの
	// 60 になる。
	std::vector<std::int64_t> p10;
	for (int i = 0; i < 10; ++i)
		p10.push_back(static_cast<std::int64_t>(i) * 100000);
	fixture.add_video_segment_at(1, 0, 1000000, pts_60fps(10), 60);
	fixture.add_video_segment_at(1, 166667, 1166667, p10, 10);
	fixture.doc().segments[1].avcc_hex += "00";
	fixture.end_at(2166667);

	FinalizeOptions options;
	options.session_dir = fixture.session_dir();
	options.materials_dir = fixture.materials_dir();
	const FinalizePlan plan =
		cordmux::record::build_finalize_plan(fixture.doc(), "u1", StreamKind::Screen, options);
	REQUIRE(plan.branch == FinalizeBranch::ProRes);
	CHECK(plan.expected_frames == 10 + 60); // 10 fps の 1 秒は、60 fps の 60 フレーム

	const FinalizeOutcome outcome = finalize(fixture, cordmux::config::RecordPcmFormat::Float32);
	REQUIRE(outcome.rows.size() == 1);
	CHECK(outcome.rows.front().check == FinalizeCheck::Matched);
	CHECK(outcome.progress.state == FinalizeState::Done);
	const ProbeStream video = probe_video(plan.output_file, join_path(fixture.session_dir(), "probe_mixed.log"));
	REQUIRE(video.present);
	CHECK(video.frames == 70);
}

TEST_CASE("実際の ffmpeg で録画ファイルを作成すると、1 フレームのセグメントと 1 フレーム分の黒が交互に続いても、ProRes の録画ファイルからフレームが落ちない" *
	  doctest::skip(!have_real_ffmpeg()))
{
	SessionFixture fixture("fin_prores_one_frame");
	// 60 fps の 1 フレームのセグメント 5 つを、1 フレーム分の空きを挟んで並べる (黒埋め用のフレームがないので、
	// 空きは color の 1 フレームになる)。concat のフィルターは、1 フレームだけの入力の終わりを始まりと同じ時刻と
	// 見積もる。
	// タイムスタンプを付け直さないと、一定の間隔で出力する処理が重なったフレームを落とし、5 つのセグメントが
	// 2 フレームになる。
	for (int k = 0; k < 5; ++k) {
		const std::int64_t at = static_cast<std::int64_t>(2 * k) * 1000000 / 60;
		fixture.add_video_segment_at(1, at, 1000000 + at, pts_60fps(1), 60);
	}
	fixture.end_at(1000000 + 9 * 1000000 / 60);

	FinalizeOptions options;
	options.session_dir = fixture.session_dir();
	options.materials_dir = fixture.materials_dir();
	const FinalizePlan plan =
		cordmux::record::build_finalize_plan(fixture.doc(), "u1", StreamKind::Screen, options);
	REQUIRE(plan.branch == FinalizeBranch::ProRes);
	REQUIRE(plan.expected_frames == 9); // セグメントの 5 フレームと黒の 4 フレーム
	CHECK(plan.frame_tolerance == 8); // 入力は 9 つだが、許容の幅は期待のフレーム数より 1 少ない数まで

	const FinalizeOutcome outcome = finalize(fixture, cordmux::config::RecordPcmFormat::Float32);
	REQUIRE(outcome.rows.size() == 1);
	CHECK(outcome.rows.front().check == FinalizeCheck::Matched);
	const ProbeStream video = probe_video(plan.output_file, join_path(fixture.session_dir(), "probe_one_frame.log"));
	REQUIRE(video.present);
	CHECK(video.frames == 9); // 許容の幅に頼らず、どのフレームも落ちない
}
