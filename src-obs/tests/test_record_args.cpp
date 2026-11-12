// SPDX-License-Identifier: Apache-2.0

// record/record_args.h の単体テスト。録画ファイルの作成の計画と FFmpeg の引数を確認する。実際の FFmpeg を起動するテストは
// test_record_io_*.cpp にあり、ここでは引数の文字列までを確認する。

#include "doctest.h"

#include "record/record_args.h"

#include <algorithm>
#include <string>
#include <vector>

using cordmux::record::build_concat_list;
using cordmux::record::build_filter_script;
using cordmux::record::build_finalize_plan;
using cordmux::record::finalize_output_matches;
using cordmux::record::FinalizeBranch;
using cordmux::record::FinalizeItem;
using cordmux::record::FinalizeOptions;
using cordmux::record::FinalizePlan;
using cordmux::record::LedgerBlackRow;
using cordmux::record::LedgerDocument;
using cordmux::record::LedgerPersonRow;
using cordmux::record::LedgerSegmentRow;
using cordmux::record::MediaKind;
using cordmux::record::pcm_input_format_name;
using cordmux::record::repaired_segment_file;
using cordmux::record::StreamKind;

namespace {

const char *const kSessionDir = "C:/rec/2000-01-01 00-00-00";

FinalizeOptions options()
{
	FinalizeOptions opts;
	opts.ffmpeg_path = "C:/obs/ffmpeg.exe";
	opts.session_dir = kSessionDir;
	return opts;
}

LedgerSegmentRow video_segment(const char *file, std::int64_t first_pts_us, std::int64_t first_steady_us,
			       std::int64_t frames, const char *avcc)
{
	LedgerSegmentRow row;
	row.user_id = "1111";
	row.kind = StreamKind::Screen;
	row.media = MediaKind::Video;
	row.file = file;
	row.avcc_hex = avcc;
	row.width = 1280;
	row.height = 720;
	row.fps_num = 60;
	row.fps_den = 1;
	row.timescale = 60000;
	row.first_pts_us = first_pts_us;
	row.first_pts_steady_us = first_steady_us;
	row.sample_count = frames;
	row.duration_ticks = frames * 1000;
	row.bytes = frames * 1000;
	row.starts_with_idr = true;
	return row;
}

// 画面共有の音声のセグメント (48 kHz)。
LedgerSegmentRow audio_segment(const char *file, std::int64_t first_pts_us, std::int64_t first_steady_us,
			       std::int64_t samples)
{
	LedgerSegmentRow row;
	row.user_id = "1111";
	row.kind = StreamKind::Screen;
	row.media = MediaKind::Audio;
	row.file = file;
	row.timescale = 48000;
	row.first_pts_us = first_pts_us;
	row.first_pts_steady_us = first_steady_us;
	row.sample_count = samples;
	row.duration_ticks = samples;
	return row;
}

LedgerBlackRow black_material()
{
	LedgerBlackRow row;
	row.user_id = "1111";
	row.encoder_instance_id = "e1";
	row.file = "black/1111_e1.mov";
	row.avcc_hex = "aa";
	row.width = 1280;
	row.height = 720;
	row.fps_num = 60;
	row.fps_den = 1;
	row.frame_count = 60;
	row.duration_ticks = 60000;
	return row;
}

// セグメント A (2 秒) -> 欠落 3 秒 -> セグメント B (2 秒)。同じ PtsDomain に入る (タイムスタンプの差と steady_clock の
// 差が合う)。
LedgerDocument two_segments_with_gap(const char *avcc_a = "aa", const char *avcc_b = "aa")
{
	LedgerDocument doc;
	doc.session.session_id = "2000-01-01 00-00-00";
	LedgerPersonRow person;
	person.user_id = "1111";
	person.display_name = "taro";
	doc.people.push_back(person);
	doc.segments.push_back(
		video_segment("segments/1111_screen_video_000001.mov", 1'000'000, 100'000'000, 120, avcc_a));
	doc.segments.push_back(
		video_segment("segments/1111_screen_video_000002.mov", 6'000'000, 105'000'000, 120, avcc_b));
	doc.blacks.push_back(black_material());
	return doc;
}

std::size_t index_of(const std::vector<std::string> &args, const std::string &value)
{
	const auto it = std::find(args.begin(), args.end(), value);
	REQUIRE(it != args.end());
	return static_cast<std::size_t>(it - args.begin());
}

bool contains(const std::vector<std::string> &args, const std::string &value)
{
	return std::find(args.begin(), args.end(), value) != args.end();
}

} // namespace

TEST_CASE("Remux で録画ファイルを作成するときは、セグメントと黒を concat で並べ、生の PCM と一緒に -c copy で 1 つのファイルにする")
{
	const LedgerDocument doc = two_segments_with_gap();
	const FinalizePlan plan = build_finalize_plan(doc, "1111", StreamKind::Screen, options());

	CHECK(plan.branch == FinalizeBranch::Remux);
	CHECK(plan.has_video);
	// 録画ファイルの名前は、人の名前、種類の語、セッションの日時 (語は既定の値で、en-US.ini と同じ文字列)。
	CHECK(plan.output_file == std::string(kSessionDir) + "/taro Screen Share 2000-01-01 00-00-00.mov");
	CHECK(plan.expected_timescale == 60000);
	// 120 フレーム + 黒 180 フレーム + 120 フレーム = 7 秒。
	CHECK(plan.expected_frames == 420);
	CHECK(plan.expected_duration_ticks == 420000);
	REQUIRE(plan.items.size() == 5);
	CHECK(plan.items[0].file == "segments/1111_screen_video_000001.mov");
	CHECK(plan.items[1].black);
	CHECK(plan.items[3].black);
	CHECK(plan.items[4].file == "segments/1111_screen_video_000002.mov");
	REQUIRE(plan.gaps.size() == 1);
	CHECK(plan.gaps[0].start_us == 2'000'000);
	CHECK(plan.gaps[0].duration_us == 3'000'000);
	CHECK(plan.gaps[0].repeat_count == 3);
	CHECK(plan.gaps[0].black_encoder_instance_id == "e1");

	const std::vector<std::string> &args = plan.args;
	CHECK(contains(args, "-xerror"));         // 必ず付ける
	CHECK_FALSE(contains(args, "-shortest")); // 付けない
	CHECK_FALSE(contains(args, "-t"));
	CHECK(index_of(args, "-f") < index_of(args, "-map"));
	// -map の後に -i を加えない。
	CHECK(index_of(args, "-map") > index_of(args, "-i"));
	for (std::size_t i = index_of(args, "-map"); i < args.size(); ++i)
		CHECK(args[i] != "-i");

	const std::size_t concat_at = index_of(args, "concat");
	CHECK(args[concat_at - 1] == "-f");
	CHECK(args[concat_at + 1] == "-safe");
	CHECK(args[concat_at + 2] == "0");
	CHECK(args[concat_at + 3] == "-i");
	CHECK(args[concat_at + 4] == plan.concat_path);

	const std::size_t pcm_at = index_of(args, plan.pcm_path);
	CHECK(args[pcm_at - 1] == "-i");
	CHECK(args[pcm_at - 2] == "2");
	CHECK(args[pcm_at - 3] == "-ac");
	CHECK(args[pcm_at - 4] == "48000");
	CHECK(args[pcm_at - 5] == "-ar");
	CHECK(args[pcm_at - 6] == "f32le");
	CHECK(args[pcm_at - 7] == "-f");

	CHECK(contains(args, "0:v:0"));
	CHECK(contains(args, "1:a:0"));
	CHECK(args[args.size() - 3] == "-c");
	CHECK(args[args.size() - 2] == "copy");
	CHECK(args.back() == plan.output_file);
}

TEST_CASE("concat の本文は file の行だけで、duration を 1 行も書かない")
{
	const LedgerDocument doc = two_segments_with_gap();
	const FinalizePlan plan = build_finalize_plan(doc, "1111", StreamKind::Screen, options());
	const std::string list = plan.concat_list;

	CHECK(list.find("ffconcat version 1.0") == 0);
	CHECK(list.find("duration") == std::string::npos);
	CHECK(list.find("file 'segments/1111_screen_video_000001.mov'") != std::string::npos);
	CHECK(list.find("file 'black/1111_e1.mov'") != std::string::npos);
	// 黒 3 つ + セグメント 2 つ = 5 行。
	std::size_t lines = 0;
	for (std::size_t at = list.find("file "); at != std::string::npos; at = list.find("file ", at + 1))
		++lines;
	CHECK(lines == 5);
	CHECK(list.find("outpoint") == std::string::npos); // 端数のない欠落
	CHECK(build_concat_list(plan) == list);
}

TEST_CASE("端数の outpoint はフレームの真ん中に置き、どちらに丸めても同じフレーム数になる")
{
	LedgerDocument doc = two_segments_with_gap();
	// 欠落を 2.5 秒にする (黒埋め用のフレームのファイル 2 つ + 30 フレーム)。
	doc.segments[1].first_pts_us = 5'500'000;
	doc.segments[1].first_pts_steady_us = 104'500'000;

	const FinalizePlan plan = build_finalize_plan(doc, "1111", StreamKind::Screen, options());
	REQUIRE(plan.gaps.size() == 1);
	CHECK(plan.gaps[0].repeat_count == 2);
	CHECK(plan.gaps[0].remainder_ticks == 30000);
	// 30 フレームの終わりは 0.500000 秒で、その半フレーム手前が outpoint。
	CHECK(plan.concat_list.find("outpoint 0.491666") != std::string::npos);
	CHECK(plan.expected_frames == 120 + 150 + 120);
}

TEST_CASE("PtsDomain をまたいだセグメントは、受信側の steady_clock の時刻で置く")
{
	LedgerDocument doc = two_segments_with_gap();
	// 「steady_clock の時刻 - タイムスタンプ」が 5.5 秒食い違う (Vesktop を起動し直して、送信側の時計の原点が変わった
	// 形)。タイムスタンプの差は経過時間を表さないので、PtsDomain の間は steady_clock の時刻で置く (record_args.h の
	// build_session_timeline)。送信側の時計は steady_clock と同じ単調な時計で進むので、再接続しただけでは PtsDomain は
	// 変わらない。
	doc.segments[1].first_pts_us = 500'000;
	doc.segments[1].first_pts_steady_us = 105'000'000;

	const FinalizePlan plan = build_finalize_plan(doc, "1111", StreamKind::Screen, options());
	REQUIRE(plan.gaps.size() == 1);
	CHECK(plan.gaps[0].start_us == 2'000'000);
	CHECK(plan.gaps[0].duration_us == 3'000'000); // steady_clock の 5 秒 - セグメント A の 2 秒
}

TEST_CASE("同じ接続の音声と映像は送信側のタイムスタンプで置くので、到着の遅延が 140 ms 違っても音声と映像はずれない")
{
	// 録画を ON にした直後は、最初の IDR がエンコードと転送のために音声より遅れて届く。送信側のタイムスタンプでは
	// 映像は音声の 50 ms 後だが、受信側に届いたのは音声の 190 ms 後 (到着の遅延が音声より 140 ms 大きい)。到着の
	// 時刻で置くと、この 140 ms が、ファイル全体で音声が先に鳴るずれとして残る。
	LedgerDocument doc;
	LedgerPersonRow taro;
	taro.user_id = "1111";
	taro.display_name = "taro";
	doc.people.push_back(taro);
	LedgerPersonRow jiro;
	jiro.user_id = "2222";
	jiro.display_name = "jiro";
	doc.people.push_back(jiro);

	// 同じ接続の別の人の声が 1 秒先に始まっている (セッションの原点がこちらで決まり、位置が 0 でなくなる)。
	LedgerSegmentRow voice;
	voice.user_id = "2222";
	voice.kind = StreamKind::Voice;
	voice.media = MediaKind::Audio;
	voice.file = "segments/2222_voice_000001.mov";
	voice.timescale = 48000;
	voice.first_pts_us = 0;
	voice.first_pts_steady_us = 99'000'000;
	voice.sample_count = 3 * 48000;
	voice.duration_ticks = 3 * 48000;
	doc.segments.push_back(voice);

	const std::int64_t audio_pts = 1'000'000;
	const std::int64_t video_pts = 1'050'000;
	doc.segments.push_back(
		audio_segment("segments/1111_screen_audio_000001.mov", audio_pts, 100'000'000, 2 * 48000));
	doc.segments.push_back(video_segment("segments/1111_screen_video_000001.mov", video_pts,
					     100'000'000 + 50'000 + 140'000, 120, "aa"));
	doc.blacks.push_back(black_material());

	const FinalizePlan plan = build_finalize_plan(doc, "1111", StreamKind::Screen, options());
	REQUIRE(plan.audio_writes.size() == 1);
	REQUIRE(plan.gaps.size() == 1);
	REQUIRE(plan.gaps[0].start_us == 0); // 映像の前の黒の長さが、映像のセグメントの位置になる

	const std::int64_t audio_start_us = plan.audio_writes[0].offset_samples * 1'000'000 / 48000;
	const std::int64_t video_start_us = plan.gaps[0].duration_us;
	// 音声は声の 1 秒後、映像はそのさらに 50 ms 後 (どちらも送信側のタイムスタンプのとおり)。
	CHECK(audio_start_us == 1'000'000);
	CHECK(video_start_us == 1'050'000);
	// 録画ファイルでの音声と映像のずれ (録画ファイルでの位置の差 - 送信側のタイムスタンプの差) は 0 になる。
	CHECK((video_start_us - audio_start_us) - (video_pts - audio_pts) == 0);

	// 声の人も同じ基準で置かれる (セッションの原点そのもの)。
	const FinalizePlan voice_plan = build_finalize_plan(doc, "2222", StreamKind::Voice, options());
	REQUIRE(voice_plan.audio_writes.size() == 1);
	CHECK(voice_plan.audio_writes[0].offset_samples == 0);
}

TEST_CASE("PtsDomain の間は steady_clock の時刻で置き、同じ PtsDomain の音声と映像はタイムスタンプでそろえる")
{
	// 前半と後半の PtsDomain で「steady_clock の時刻 - タイムスタンプ」が 14 秒食い違う (送信側の時計の原点が
	// 変わった形)。PtsDomain の間の欠落の長さは steady_clock で決め、それぞれの PtsDomain の中の音声と映像は
	// タイムスタンプの差でそろえる。
	LedgerDocument doc;
	LedgerPersonRow taro;
	taro.user_id = "1111";
	taro.display_name = "taro";
	doc.people.push_back(taro);
	doc.blacks.push_back(black_material());

	// 前半: 音声 1.00 秒、映像 1.05 秒 (映像の到着は 140 ms 余計に遅れた)。
	doc.segments.push_back(
		audio_segment("segments/1111_screen_audio_000001.mov", 1'000'000, 100'000'000, 2 * 48000));
	doc.segments.push_back(video_segment("segments/1111_screen_video_000001.mov", 1'050'000,
					     100'190'000, 120, "aa"));
	// 後半: steady_clock では前半の先頭から 5 秒後。タイムスタンプは 20 秒台へ飛んでいる。映像の到着は 100 ms
	// 余計に遅れた。
	doc.segments.push_back(
		audio_segment("segments/1111_screen_audio_000002.mov", 20'000'000, 105'000'000, 2 * 48000));
	doc.segments.push_back(video_segment("segments/1111_screen_video_000002.mov", 20'050'000,
					     105'150'000, 120, "aa"));

	const FinalizePlan plan = build_finalize_plan(doc, "1111", StreamKind::Screen, options());
	REQUIRE(plan.audio_writes.size() == 2);
	REQUIRE(plan.gaps.size() == 2);
	// 前半: 音声は原点、映像は 50 ms 後 (先頭の黒 3 フレーム)。
	CHECK(plan.audio_writes[0].offset_samples == 0);
	CHECK(plan.gaps[0].start_us == 0);
	CHECK(plan.gaps[0].duration_us == 50'000);
	// 後半: 音声は steady_clock の 5 秒後、映像はその 50 ms 後 (タイムスタンプの差)。
	CHECK(plan.audio_writes[1].offset_samples == 5 * 48000);
	CHECK(plan.gaps[1].start_us == 2'050'000);
	CHECK(plan.gaps[1].start_us + plan.gaps[1].duration_us == 5'050'000);
}

TEST_CASE("セッションが終わるまでの末尾の欠落も黒で埋める")
{
	LedgerDocument doc = two_segments_with_gap();
	doc.segments.pop_back(); // セグメント A だけ (2 秒で画面共有をやめた)
	doc.session.started_at_steady_us = 100'000'000;
	doc.session.ended_at_steady_us = 110'000'000; // このセッションは 10 秒

	const FinalizePlan plan = build_finalize_plan(doc, "1111", StreamKind::Screen, options());
	CHECK(plan.expected_frames == 120 + 480); // 2 秒 + 8 秒
	REQUIRE(plan.gaps.size() == 1);
	CHECK(plan.gaps[0].start_us == 2'000'000);
	CHECK(plan.gaps[0].duration_us == 8'000'000);
}

TEST_CASE("画面共有を止めて 5 分後に始め直した人は、止めていた間を黒と無音で埋めた 1 本になり、avcC が食い違えば ProRes になる")
{
	// 止める前 (2 秒) と始め直した後 (2 秒) は同じ PtsDomain で、始め直しは止めた時点の 300 秒後。
	auto build = [](const char *avcc_b) {
		LedgerDocument doc = two_segments_with_gap("aa", avcc_b);
		doc.segments.clear();
		doc.segments.push_back(video_segment("segments/1111_screen_video_000001.mov", 1'000'000, 100'000'000, 120, "aa"));
		doc.segments.push_back(audio_segment("segments/1111_screen_audio_000001.mov", 1'000'000, 100'000'000, 2 * 48000));
		doc.segments.push_back(
			video_segment("segments/1111_screen_video_000002.mov", 301'000'000, 400'000'000, 120, avcc_b));
		doc.segments.push_back(
			audio_segment("segments/1111_screen_audio_000002.mov", 301'000'000, 400'000'000, 2 * 48000));
		return doc;
	};

	const FinalizePlan same = build_finalize_plan(build("aa"), "1111", StreamKind::Screen, options());
	CHECK(same.branch == FinalizeBranch::Remux);
	// 2 秒 + 黒 298 秒 + 2 秒。
	CHECK(same.expected_frames == 120 + 298 * 60 + 120);
	REQUIRE(same.gaps.size() == 1);
	CHECK(same.gaps[0].start_us == 2'000'000);
	CHECK(same.gaps[0].duration_us == 298'000'000);
	CHECK(same.gaps[0].repeat_count == 298);
	// 音声は、止めていた間が生の PCM の無音のまま残り、始め直した後のぶんは 300 秒の位置に書く。
	REQUIRE(same.audio_writes.size() == 2);
	CHECK(same.audio_writes[0].offset_samples == 0);
	CHECK(same.audio_writes[1].offset_samples == 300 * 48000);
	CHECK(same.pcm_total_samples == 302 * 48000);

	const FinalizePlan differ = build_finalize_plan(build("bb"), "1111", StreamKind::Screen, options());
	CHECK(differ.branch == FinalizeBranch::ProRes);
	CHECK(differ.expected_frames == same.expected_frames);
	CHECK(differ.filter_script.find("d=298.000000") != std::string::npos);
}

TEST_CASE("avcC が食い違えば ProRes で作成し、黒は lavfi で作る")
{
	const LedgerDocument doc = two_segments_with_gap("aa", "bb");
	const FinalizePlan plan = build_finalize_plan(doc, "1111", StreamKind::Screen, options());

	CHECK(plan.branch == FinalizeBranch::ProRes);
	const std::vector<std::string> &args = plan.args;
	CHECK(contains(args, "-filter_complex_script"));
	CHECK(contains(args, "[vout]"));
	CHECK(contains(args, "prores_ks"));
	CHECK(contains(args, "yuv422p10le"));
	CHECK(contains(args, "apl0"));
	CHECK(contains(args, "3"));
	CHECK_FALSE(contains(args, "concat")); // concat demuxer は使わない
	CHECK_FALSE(contains(args, "-shortest"));
	// 入力はセグメント 2 つと生の PCM の 3 つ。音声は 3 つ目 (0 から数えて 2)。
	CHECK(contains(args, "2:a:0"));
	for (std::size_t i = index_of(args, "-map"); i < args.size(); ++i)
		CHECK(args[i] != "-i");

	const std::string script = plan.filter_script;
	CHECK(script.find("color=c=black:s=1280x720:r=60:d=3.000000") != std::string::npos);
	CHECK(script.find("scale=1280:720") != std::string::npos);
	// pad は幅と高さを別の引数で受け取る。"1280x720" を渡すと、実際の FFmpeg が Invalid chars のエラーで終了する
	// (終了コード -22) ので、書き方をここで確認する。
	CHECK(script.find("pad=1280:720:(ow-iw)/2:(oh-ih)/2") != std::string::npos);
	CHECK(script.find("pad=1280x720") == std::string::npos);
	CHECK(script.find("concat=n=3:v=1:a=0,setpts=N/(60)/TB[vout]") != std::string::npos); // セグメントと黒の 3 つ
	CHECK(build_filter_script(plan) == script);
}

TEST_CASE("ProRes で録画ファイルを作成するときは、映像の timescale を録画ログの timescale にそろえる (-video_track_timescale)")
{
	// 付けないと、mov の muxer が 60 fps に 15360 を選び、録画ログの output_timescale (60000) と実際のファイルが
	// 食い違う (秒では同じ長さ)。
	const LedgerDocument doc = two_segments_with_gap("aa", "bb");
	const FinalizePlan plan = build_finalize_plan(doc, "1111", StreamKind::Screen, options());
	REQUIRE(plan.branch == FinalizeBranch::ProRes);
	REQUIRE(plan.expected_timescale == 60000);
	const std::size_t at = index_of(plan.args, "-video_track_timescale");
	REQUIRE(at + 1 < plan.args.size());
	CHECK(plan.args[at + 1] == "60000");
	CHECK(at < index_of(plan.args, plan.output_file)); // 録画ファイルのパスの前に置く

	// Remux で録画ファイルを作成するとき (-c copy) は付けない (timescale はセグメントのものがそのまま残る)。
	const LedgerDocument same = two_segments_with_gap();
	const FinalizePlan remux = build_finalize_plan(same, "1111", StreamKind::Screen, options());
	REQUIRE(remux.branch == FinalizeBranch::Remux);
	CHECK_FALSE(contains(remux.args, "-video_track_timescale"));
}

TEST_CASE("録画ファイルの timescale が録画ログと違うときは、秒に修正して 1 フレーム以内の違いを許す")
{
	const LedgerDocument doc = two_segments_with_gap();
	const FinalizePlan plan = build_finalize_plan(doc, "1111", StreamKind::Screen, options());
	REQUIRE(plan.expected_timescale == 60000);
	REQUIRE(plan.expected_duration_ticks == 420000); // 7 秒 = 420 フレーム

	// 7 秒を timescale 15360 で数えると 107520 (1 フレーム = 256)。
	CHECK(finalize_output_matches(plan, 107520, 420, 15360));
	CHECK(finalize_output_matches(plan, 107520 + 256, 420, 15360));
	CHECK_FALSE(finalize_output_matches(plan, 107520 + 257, 420, 15360));
	CHECK_FALSE(finalize_output_matches(plan, 107520 - 257, 420, 15360));
	CHECK_FALSE(finalize_output_matches(plan, 107520, 419, 15360)); // フレーム数はちょうど一致させる
	// 同じ timescale を渡したときと、0 を渡したときは、timescale を修正せずに比べる。
	CHECK(finalize_output_matches(plan, 421000, 420, 60000));
	CHECK_FALSE(finalize_output_matches(plan, 421001, 420, 60000));
	CHECK(finalize_output_matches(plan, 421000, 420, 0));
}

TEST_CASE("ProRes で録画ファイルを作成するときは fps のフィルターが出すフレーム数と比べ、並び 1 つにつき 1 フレームのずれを許す")
{
	// フレームの間隔がそろわないセグメント (画面が動かない間にフレームが間引かれた): 60 fps を示し、24 サンプルで
	// 長さは 2.5 秒。fps のフィルターは長さの分の 150 フレームを出す (サンプル数の合計 24 と比べると、
	// 再開するたびに失敗する)。
	LedgerDocument doc = two_segments_with_gap("aa", "bb");
	doc.segments[0].sample_count = 24;
	doc.segments[0].duration_ticks = 150'000;
	const FinalizePlan plan = build_finalize_plan(doc, "1111", StreamKind::Screen, options());
	REQUIRE(plan.branch == FinalizeBranch::ProRes);
	// セグメント A は 2.5 秒の位置まで、黒はセグメント B の始まり (5 秒の位置) までの 2.5 秒 (150 フレーム)、
	// セグメント B は 120 フレーム。
	CHECK(plan.expected_frames == 150 + 150 + 120);
	CHECK(plan.expected_duration_ticks == 420'000); // フレーム数 x 1 フレームの長さ (420 x 1000)
	CHECK(plan.frame_tolerance == 3);               // セグメント 2 つ + 黒 1 つ
	CHECK(plan.pcm_total_samples == 420 * 800);     // 生の PCM も同じ長さ (48000 / 60)
	// 録画ファイルの映像の fps を計画の fps と指定する (スクリプトの最後の setpts を通るとフレームレートが不明に
	// なるため)。
	const std::size_t rate_at = index_of(plan.args, "-r");
	REQUIRE(rate_at + 1 < plan.args.size());
	CHECK(plan.args[rate_at + 1] == "60");
	CHECK(rate_at < index_of(plan.args, plan.output_file));

	CHECK(finalize_output_matches(plan, 420'000, 420, 60000));
	// 並び 1 つにつき 1 フレームまでのずれは許す (fps が混ざった人や、color の長さの丸め)。
	CHECK(finalize_output_matches(plan, 423'000, 423, 60000));
	CHECK(finalize_output_matches(plan, 417'000, 417, 60000));
	// 本当に欠けた録画ファイル (セグメント A のファイルが、録画ログの示す長さより 1 秒短い) は失敗のままにする。
	CHECK_FALSE(finalize_output_matches(plan, 360'000, 360, 60000));
	CHECK_FALSE(finalize_output_matches(plan, 416'000, 416, 60000));
	CHECK_FALSE(finalize_output_matches(plan, 0, 0, 60000)); // 1 フレームもない録画ファイルは許容に入らない
	// フレーム数が合っても、長さが外れていれば失敗する (許容は、許容のフレーム数 + 1 フレームまで)。
	CHECK(finalize_output_matches(plan, 424'000, 420, 60000));
	CHECK_FALSE(finalize_output_matches(plan, 424'001, 420, 60000));

	// セグメントごとの数は、計画の fps の時刻のうち最も近いものに丸める (半分は上に丸める。av_rescale_rnd の
	// NEAR_INF)。
	CHECK(cordmux::record::fps_filter_frames(150'000, 60000, 10, 1) == 25); // 24 フレーム + 200 ms の空き (10 fps)
	CHECK(cordmux::record::fps_filter_frames(147'000, 60000, 10, 1) == 25); // 24.5 フレーム
	CHECK(cordmux::record::fps_filter_frames(146'999, 60000, 10, 1) == 24);
	CHECK(cordmux::record::fps_filter_frames(0, 60000, 10, 1) == 0);

	// 短い並びばかりの人: 並びの数が期待のフレーム数に届いても、許容は期待より 1 少ない数まで。0 フレームの
	// 録画ファイルは通さない。
	LedgerDocument tiny;
	tiny.session.session_id = "2000-01-01 00-00-00";
	LedgerPersonRow tiny_person;
	tiny_person.user_id = "1111";
	tiny_person.display_name = "taro";
	tiny.people.push_back(tiny_person);
	tiny.segments.push_back(video_segment("segments/1111_screen_video_000001.mov", 0, 100'000'000, 1, "aa"));
	tiny.segments.push_back(video_segment("segments/1111_screen_video_000002.mov", 16'667, 100'016'667, 1, "bb"));
	const FinalizePlan tiny_plan = build_finalize_plan(tiny, "1111", StreamKind::Screen, options());
	REQUIRE(tiny_plan.branch == FinalizeBranch::ProRes);
	REQUIRE(tiny_plan.expected_frames == 2);
	CHECK(tiny_plan.frame_tolerance == 1);
	CHECK(finalize_output_matches(tiny_plan, 2000, 2, 60000));
	CHECK(finalize_output_matches(tiny_plan, 1000, 1, 60000));
	CHECK_FALSE(finalize_output_matches(tiny_plan, 0, 0, 60000));

	// Remux で録画ファイルを作成するときは、サンプル数の合計とちょうど一致することを求める (許容は 0)。
	LedgerDocument same = two_segments_with_gap();
	same.segments[0].sample_count = 24;
	same.segments[0].duration_ticks = 150'000;
	const FinalizePlan remux = build_finalize_plan(same, "1111", StreamKind::Screen, options());
	REQUIRE(remux.branch == FinalizeBranch::Remux);
	CHECK(remux.frame_tolerance == 0);
	CHECK(remux.expected_frames == 24 + 150 + 120);
	CHECK_FALSE(finalize_output_matches(remux, remux.expected_duration_ticks, remux.expected_frames + 1, 60000));
}

TEST_CASE("ProRes で録画ファイルを作成するとき、続いた黒の長さはフレーム数の合計から 1 度だけ timescale の単位に修正する")
{
	// 59 fps、黒埋め用のフレームは 30 フレーム (1 つで 30,508.47 単位)、黒は 2 時間 (424,800 フレーム。14,160 回の
	// 繰り返し)。1 つごとに丸めた 30,508 単位を足すと d=7199.888000 になり、color が 424,794 フレームしか出さずに、
	// 長さの比較で失敗する。
	LedgerDocument doc = two_segments_with_gap("aa", "bb");
	for (LedgerSegmentRow &row : doc.segments) {
		row.fps_num = 59;
		row.sample_count = 59;
		row.duration_ticks = 60000; // 1 秒
	}
	doc.segments[0].first_pts_us = 1'000'000;
	doc.segments[0].first_pts_steady_us = 100'000'000;
	doc.segments[1].first_pts_us = 1'000'000 + 7'201'000'000LL; // セグメント A の 1 秒 + 黒 2 時間
	doc.segments[1].first_pts_steady_us = 100'000'000 + 7'201'000'000LL;
	doc.blacks[0].fps_num = 59;
	doc.blacks[0].frame_count = 30;
	const FinalizePlan plan = build_finalize_plan(doc, "1111", StreamKind::Screen, options());
	REQUIRE(plan.branch == FinalizeBranch::ProRes);
	REQUIRE(plan.fps_num == 59);
	CHECK(plan.expected_frames == 59 + 424'800 + 59);
	const std::string &script = plan.filter_script;
	CHECK(script.find("color=c=black:s=1280x720:r=59:d=7200.000000,") != std::string::npos);
	CHECK(script.find("d=7199.888000") == std::string::npos);
	CHECK(script.find("concat=n=3:v=1:a=0") != std::string::npos); // 続いた黒は 1 つの color になる
}

TEST_CASE("声のファイルは映像を持たず、生の PCM だけをコピーする")
{
	LedgerDocument doc;
	LedgerPersonRow person;
	person.user_id = "1111";
	person.display_name = "taro";
	doc.people.push_back(person);
	LedgerSegmentRow voice;
	voice.user_id = "1111";
	voice.kind = StreamKind::Voice;
	voice.media = MediaKind::Audio;
	voice.file = "segments/1111_voice_000001.mov";
	voice.timescale = 48000;
	voice.sample_count = 48000;
	voice.duration_ticks = 48000;
	doc.segments.push_back(voice);
	doc.session.session_id = "2000-01-01 00-00-00";

	FinalizeOptions opts = options();
	opts.voice_kind_word = "声"; // ini から渡される語
	const FinalizePlan plan = build_finalize_plan(doc, "1111", StreamKind::Voice, opts);
	CHECK_FALSE(plan.has_video);
	CHECK(plan.branch == FinalizeBranch::Remux);
	CHECK(plan.sample_rate == 48000);
	CHECK(plan.output_file == std::string(kSessionDir) + "/taro 声 2000-01-01 00-00-00.mov");
	CHECK(contains(plan.args, "0:a:0"));
	CHECK_FALSE(contains(plan.args, "0:v:0"));
	CHECK_FALSE(contains(plan.args, "concat"));
	CHECK(plan.concat_list.find("file ") == std::string::npos);
	CHECK(plan.filter_script.empty());
}

TEST_CASE("音声は、全体の長さの無音に各セグメントを実際の位置で書き込む")
{
	LedgerDocument doc = two_segments_with_gap();
	// 画面共有の音声のセグメントを 2 つ加える (映像と同じ原点、同じ位置)。
	LedgerSegmentRow first;
	first.user_id = "1111";
	first.kind = StreamKind::Screen;
	first.media = MediaKind::Audio;
	first.file = "segments/1111_screen_audio_000001.mov";
	first.timescale = 48000;
	first.first_pts_us = 1'000'000;
	first.first_pts_steady_us = 100'000'000;
	first.sample_count = 96000; // 2 秒
	first.duration_ticks = 96000;
	doc.segments.push_back(first);
	LedgerSegmentRow second = first;
	second.file = "segments/1111_screen_audio_000002.mov";
	second.first_pts_us = 6'000'000;
	second.first_pts_steady_us = 105'000'000;
	doc.segments.push_back(second);

	const FinalizePlan plan = build_finalize_plan(doc, "1111", StreamKind::Screen, options());
	CHECK(plan.sample_rate == 48000);
	REQUIRE(plan.audio_writes.size() == 2);
	CHECK(plan.audio_writes[0].file == "segments/1111_screen_audio_000001.mov");
	CHECK(plan.audio_writes[0].offset_samples == 0);
	CHECK(plan.audio_writes[0].sample_count == 96000);
	// 2 つ目は 5 秒の位置 (セグメント 2 秒 + 欠落 3 秒)。
	CHECK(plan.audio_writes[1].offset_samples == 5 * 48000);
	// 生の PCM の長さは映像と同じ 7 秒 (-shortest を付けないので、長さをそろえる)。
	CHECK(plan.pcm_total_samples == 7 * 48000);
}

TEST_CASE("声のファイルの長さは音声で決まり、timescale はサンプリングレートになる")
{
	LedgerDocument doc;
	LedgerPersonRow person;
	person.user_id = "1111";
	person.display_name = "taro";
	doc.people.push_back(person);
	doc.session.started_at_steady_us = 100'000'000;
	doc.session.ended_at_steady_us = 130'000'000; // このセッションは 30 秒
	LedgerSegmentRow voice;
	voice.user_id = "1111";
	voice.kind = StreamKind::Voice;
	voice.media = MediaKind::Audio;
	voice.file = "segments/1111_voice_000001.mov";
	voice.timescale = 48000;
	voice.first_pts_us = 0;
	voice.first_pts_steady_us = 100'000'000;
	voice.sample_count = 10 * 48000;
	voice.duration_ticks = 10 * 48000;
	doc.segments.push_back(voice);

	const FinalizePlan plan = build_finalize_plan(doc, "1111", StreamKind::Voice, options());
	CHECK_FALSE(plan.has_video);
	CHECK(plan.expected_frames == 0);
	CHECK(plan.expected_timescale == 48000);
	CHECK(plan.pcm_total_samples == 30 * 48000); // 話していなかった 20 秒も 0 で埋める
	CHECK(plan.expected_duration_ticks == 30 * 48000);
	REQUIRE(plan.audio_writes.size() == 1);
	CHECK(plan.audio_writes[0].offset_samples == 0);
	CHECK(plan.audio_writes[0].sample_count == 10 * 48000);
	// 映像を持たない録画ファイルでも長さを比べられる (フレーム数は 0、長さはサンプル数)。
	CHECK(finalize_output_matches(plan, 30 * 48000, 0));
	CHECK_FALSE(finalize_output_matches(plan, 10 * 48000, 0));
}

TEST_CASE("録画ファイルの PCM の形は設定で切り替わる")
{
	CHECK(std::string(pcm_input_format_name(cordmux::config::RecordPcmFormat::Float32)) == "f32le");
	CHECK(std::string(pcm_input_format_name(cordmux::config::RecordPcmFormat::Int24)) == "s24le");

	FinalizeOptions opts = options();
	opts.output_pcm = cordmux::config::RecordPcmFormat::Int24;
	const LedgerDocument doc = two_segments_with_gap();
	const FinalizePlan plan = build_finalize_plan(doc, "1111", StreamKind::Screen, opts);
	CHECK(contains(plan.args, "s24le"));
	CHECK_FALSE(contains(plan.args, "f32le"));
}

TEST_CASE("途中で切れたセグメントは、書き直した後のファイルを指す")
{
	CHECK(repaired_segment_file("segments/a.mov") == "segments/a_repair.mov");
	CHECK(repaired_segment_file("segments/a") == "segments/a_repair.mov");

	LedgerDocument doc = two_segments_with_gap();
	doc.segments[1].truncated = true;

	FinalizeOptions opts = options();
	const FinalizePlan without = build_finalize_plan(doc, "1111", StreamKind::Screen, opts);
	CHECK(without.items.back().file == "segments/1111_screen_video_000002.mov");
	CHECK(without.branch == FinalizeBranch::ProRes); // 途中で切れたセグメントがあれば ProRes で録画ファイルを作成する

	opts.repair_truncated = true;
	const FinalizePlan with = build_finalize_plan(doc, "1111", StreamKind::Screen, opts);
	CHECK(with.items.back().file == "segments/1111_screen_video_000002_repair.mov");
	// 書き直し (読めるサンプルだけのコピー) は、FFmpeg の子プロセスではなく record_finalize.cpp が行う。実際の
	// 途中で切れたセグメントで確認するのは test_record_io_interrupted.cpp である。
}

TEST_CASE("録画ファイルの名前には、重なりを解いた後の表示名を使う")
{
	LedgerDocument doc = two_segments_with_gap();
	LedgerPersonRow other;
	other.user_id = "2222222222";
	other.display_name = "taro";
	doc.people.push_back(other);
	doc.people[0].user_id = "1111111111";
	for (LedgerSegmentRow &row : doc.segments)
		row.user_id = "1111111111";
	doc.blacks[0].user_id = "1111111111";

	const FinalizePlan plan = build_finalize_plan(doc, "1111111111", StreamKind::Screen, options());
	CHECK(plan.output_file == std::string(kSessionDir) + "/taro_111111 Screen Share 2000-01-01 00-00-00.mov");
}

TEST_CASE("中間ファイルのフォルダーがあれば一時ファイルはその下に、録画ファイルはセッションのフォルダーの直下に置く")
{
	const LedgerDocument doc = two_segments_with_gap();
	FinalizeOptions opts = options();
	opts.materials_dir = std::string(kSessionDir) + "/data";
	const FinalizePlan plan = build_finalize_plan(doc, "1111", StreamKind::Screen, opts);
	CHECK(plan.output_file == std::string(kSessionDir) + "/taro Screen Share 2000-01-01 00-00-00.mov");
	CHECK(plan.concat_path == std::string(kSessionDir) + "/data/concat_1111_screen.txt");
	CHECK(plan.pcm_path == std::string(kSessionDir) + "/data/audio_1111_screen.pcm");
	CHECK(plan.progress_path == std::string(kSessionDir) + "/data/progress_1111_screen.txt");
	// concat の一覧の行は、中間ファイルのフォルダーからの相対パス (録画ログの `file` のまま)。
	CHECK(plan.concat_list.find("file 'segments/1111_screen_video_000001.mov'") != std::string::npos);
	// 進み具合を、プログラムが読める形でファイルに書かせる。入力と録画ファイルの指定より前に置く。
	CHECK(plan.args[index_of(plan.args, "-progress") + 1] == plan.progress_path);
	CHECK(plan.args[index_of(plan.args, "-stats_period") + 1] == "1");
	CHECK(index_of(plan.args, "-progress") < index_of(plan.args, "-i"));
}

TEST_CASE("録画ログの 2 つの時刻が食い違っても、末尾の黒はセッションの長さを超えない")
{
	LedgerDocument doc = two_segments_with_gap();
	// 原点が 0 になる形: steady_clock の時刻を持たないセグメントの行が 1 つでもあると、原点が 0 になる。
	doc.segments.front().first_pts_steady_us = 0;
	// セッションの長さそのものは 8 秒 (session の行の 2 つの時刻は一貫している)。
	doc.session.started_at_steady_us = 100'000'000;
	doc.session.ended_at_steady_us = 108'000'000;

	const FinalizePlan plan = build_finalize_plan(doc, "1111", StreamKind::Screen, options());
	CHECK(plan.tail_gap_clamped);
	// 黒は 8 秒 = 480 フレームを超えない (上限がないと、105 秒分 = 6300 フレームの黒を作る)。
	std::int64_t black_frames = 0;
	for (const FinalizeItem &item : plan.items) {
		if (item.black)
			black_frames += item.frames;
	}
	CHECK(black_frames <= 480);
	// セグメントそのものは削らない (実際に書いたものは残す)。
	CHECK(plan.expected_frames >= 240);

	// 正常な録画ログでは、上限は何も変えない。
	LedgerDocument healthy = two_segments_with_gap();
	healthy.session.started_at_steady_us = 99'000'000;
	healthy.session.ended_at_steady_us = 108'000'000;
	const FinalizePlan ok = build_finalize_plan(healthy, "1111", StreamKind::Screen, options());
	CHECK_FALSE(ok.tail_gap_clamped);
	// セグメント A の先頭 (100 秒) からセッションの終わり (108 秒) までの 8 秒 = 480 フレーム。
	CHECK(ok.expected_frames == 480);
}

TEST_CASE("原点がセッションの開始より少し前にある正常な録画ログでは、末尾の黒を削らない")
{
	// セッションの開始は最初のセグメントを開くときの時刻なので、そのセグメントの到着より後になる。原点は送信側の
	// タイムスタンプから作るので、到着よりさらに前にある。ここでは原点 (100 秒) が開始より 20 ms 前にある。
	LedgerDocument doc = two_segments_with_gap();
	doc.session.started_at_steady_us = 100'020'000;
	doc.session.ended_at_steady_us = 110'000'000;

	const FinalizePlan plan = build_finalize_plan(doc, "1111", StreamKind::Screen, options());
	CHECK_FALSE(plan.tail_gap_clamped);
	CHECK(plan.expected_frames == 600); // 原点からセッションの終わりまでの 10 秒 = 600 フレーム

	// 開始より 1 秒を超えて前にある原点は、録画ログの 2 つの時刻の食い違いとして扱う。
	LedgerDocument far = two_segments_with_gap();
	far.session.started_at_steady_us = 101'500'000;
	far.session.ended_at_steady_us = 110'000'000;
	const FinalizePlan clamped = build_finalize_plan(far, "1111", StreamKind::Screen, options());
	CHECK(clamped.tail_gap_clamped);
}

TEST_CASE("録画ファイルの長さとフレーム数は整数で比べる")
{
	const LedgerDocument doc = two_segments_with_gap();
	const FinalizePlan plan = build_finalize_plan(doc, "1111", StreamKind::Screen, options());

	CHECK(finalize_output_matches(plan, 420000, 420));
	// コンテナーの丸めで、1 フレーム分までは動きうる。
	CHECK(finalize_output_matches(plan, 420000 + 1000, 420));
	CHECK_FALSE(finalize_output_matches(plan, 420000 + 1001, 420));
	// フレーム数はちょうど一致しなければならない (途中で切れたセグメントは、ここで見つかる)。
	CHECK_FALSE(finalize_output_matches(plan, 420000, 419));
	CHECK_FALSE(finalize_output_matches(plan, 300000, 300));
}
