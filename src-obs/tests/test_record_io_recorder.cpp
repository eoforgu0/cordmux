// SPDX-License-Identifier: Apache-2.0

// record/recorder (Recorder の入力、書き込むスレッド、録画ログ、キュー、空き容量) のテスト。
// 録画が OFF の間に RecordPlatformOps の関数を 1 度も呼ばないことと、OFF の判定 (`gate`) を外すとそれが崩れる
// ことを確認する。録画ログが、実際に読み直せる形で残ることも確認する。

#include "doctest.h"

#include "config/settings.h"
#include "record/record_ledger.h"
#include "record/record_segment.h"
#include "record/record_writer.h"
#include "record/recorder.h"
#include "test_record_io_shared.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using cordmux::record::annex_b_to_avcc;
using cordmux::record::ChildWait;
using cordmux::record::ChildWaitResult;
using cordmux::record::CloseReason;
using cordmux::record::FileHandle;
using cordmux::record::kInvalidFile;
using cordmux::record::kLedgerFileName;
using cordmux::record::LedgerDocument;
using cordmux::record::make_real_record_platform_ops;
using cordmux::record::MediaKind;
using cordmux::record::parse_ledger;
using cordmux::record::read_whole_file;
using cordmux::record::Recorder;
using cordmux::record::RecordingSnapshot;
using cordmux::record::RecordPlatformOps;
using cordmux::record::StreamKind;
using cordmux::tests::record_io::make_audio_chunk;
using cordmux::tests::record_io::read_h264_fixture;
using cordmux::tests::record_io::split_access_units;
using cordmux::tests::record_io::TempDir;
using cordmux::tests::record_io::unit_is_keyframe;

namespace {

// 条件が真になるまで待つ (上限 5 秒)。固定の時間だけ待つと、負荷の高いときに非同期の処理が間に合わない。
template <typename Pred> bool wait_until(Recorder &recorder, Pred pred)
{
	for (int i = 0; i < 500; ++i) {
		if (pred(recorder.snapshot()))
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return pred(recorder.snapshot());
}

// 呼び出しの回数だけを数えるテスト用の RecordPlatformOps。ファイルには 1 バイトも書かない。
struct CountingOps {
	std::atomic<int> make_directories{0};
	std::atomic<int> open_file{0};
	std::atomic<int> append{0};
	std::atomic<int> close{0};
	std::atomic<int> free_space{0};
	std::atomic<int> start_child{0};
	std::atomic<std::int64_t> steady{0};

	RecordPlatformOps build()
	{
		RecordPlatformOps ops;
		ops.make_directories = [this](const std::string &) {
			++make_directories;
			return true;
		};
		ops.open_file = [this](const std::string &) -> FileHandle {
			return static_cast<FileHandle>(++open_file);
		};
		ops.append = [this](FileHandle, const void *, std::size_t) {
			++append;
			return true;
		};
		ops.close = [this](FileHandle) { ++close; };
		ops.free_space_bytes = [this](const std::string &) -> std::optional<std::uint64_t> {
			++free_space;
			return std::nullopt;
		};
		ops.start_child = [this](const std::string &, const std::vector<std::string> &) {
			++start_child;
			return static_cast<cordmux::record::ChildHandle>(0);
		};
		ops.wait_child = [](cordmux::record::ChildHandle, int) {
			ChildWaitResult result;
			result.outcome = ChildWait::Failed;
			return result;
		};
		ops.kill_child = [](cordmux::record::ChildHandle) {};
		ops.now_steady_us = [this]() { return steady.fetch_add(1000); };
		ops.now_wall_us = []() { return static_cast<std::int64_t>(946684800) * 1000000; };
		ops.ffmpeg_path = []() { return std::string(); };
		return ops;
	}
};

// 常に録画中と判定する `gate`。OFF の判定を外したときにテストが失敗することを確認するためのもので、製品にはない。
bool always_open_gate(bool)
{
	return true;
}

struct Material {
	std::vector<std::vector<std::uint8_t>> units;
};

Material load_units()
{
	Material material;
	material.units = split_access_units(read_h264_fixture());
	REQUIRE(material.units.size() == 10);
	return material;
}

cordmux::ws::FrameHeader video_header(std::uint32_t stream_id, std::int64_t pts_us, bool keyframe, bool material)
{
	cordmux::ws::FrameHeader header;
	header.type = cordmux::ws::FrameType::Video;
	header.stream_id = stream_id;
	header.timestamp_us = static_cast<std::uint64_t>(pts_us);
	header.flags = static_cast<std::uint8_t>((keyframe ? cordmux::ws::kFlagKeyframe : 0) |
						 (material ? cordmux::ws::kFlagRecordingOnly : 0));
	return header;
}

cordmux::ws::FrameHeader audio_header(std::uint32_t stream_id, std::int64_t pts_us, bool material)
{
	cordmux::ws::FrameHeader header;
	header.type = cordmux::ws::FrameType::Audio;
	header.stream_id = stream_id;
	header.timestamp_us = static_cast<std::uint64_t>(pts_us);
	header.flags = material ? cordmux::ws::kFlagRecordingOnly : 0;
	return header;
}

std::vector<cordmux::pipeline::StreamFormat> screen_formats(int width, int height, double fps)
{
	cordmux::pipeline::StreamFormat screen;
	screen.stream_id = 1;
	screen.user_id = "u1";
	screen.screen = true;
	screen.width = width;
	screen.height = height;
	screen.fps = fps;
	cordmux::pipeline::StreamFormat voice;
	voice.stream_id = 2;
	voice.user_id = "u1";
	voice.screen = false;
	return {screen, voice};
}

// 保存先に 1 つでもファイルがあるか。録画が OFF の間にファイルが増えないことを、保存先の側からも確認する。
bool any_file_under(const std::string &dir)
{
	std::error_code ec;
	std::filesystem::recursive_directory_iterator it(std::filesystem::u8path(dir), ec);
	if (ec)
		return false;
	for (const std::filesystem::directory_entry &entry : it) {
		std::error_code file_ec;
		if (entry.is_regular_file(file_ec) && !file_ec)
			return true;
	}
	return false;
}

// このセッションのフォルダー (1 つだけできる) を探す。
std::string only_session_dir(const std::string &base)
{
	const std::string root = base;
	const std::vector<std::string> names = cordmux::record::list_subdirectories(root);
	REQUIRE(names.size() == 1);
	return cordmux::record::join_path(root, names.front());
}

// このセッションの data/ フォルダー (録画ログとセグメントがある)。
std::string only_materials_dir(const std::string &base)
{
	return cordmux::record::join_path(only_session_dir(base), cordmux::record::kMaterialsDirName);
}

LedgerDocument read_ledger(const std::string &session_dir)
{
	std::string text;
	REQUIRE(read_whole_file(cordmux::record::join_path(session_dir, kLedgerFileName), &text));
	REQUIRE_FALSE(text.empty());
	return parse_ledger(text);
}

// 受信側の単調な時計は、実際の時計を使う。録画ファイルの作成で黒で埋める長さは `ended_at_steady_us - first_pts_steady_us`
// で決まるので、送信側のタイムスタンプをそのまま単調な時計として渡すと、何日分もの黒で埋める計画になる。
inline std::int64_t steady_now()
{
	return cordmux::platform::now_steady_us();
}

// テストデータの 10 フレームを `rounds` 回、1 つのストリームに送る (pts は 10 fps の間隔)。
void feed_video(Recorder &recorder, const Material &material, std::uint32_t stream_id, int rounds,
		std::int64_t start_us)
{
	std::int64_t pts_us = start_us;
	for (int round = 0; round < rounds; ++round) {
		for (const std::vector<std::uint8_t> &unit : material.units) {
			const std::int64_t steady = steady_now();
			recorder.on_video(video_header(stream_id, pts_us, unit_is_keyframe(unit), false),
					  unit.data(), unit.size(), steady, steady);
			pts_us += 100000;
		}
	}
}

} // namespace

TEST_CASE("録画が OFF の間は、RecordPlatformOps の関数を 1 度も呼ばず、1 バイトも書かない")
{
	const TempDir dir("ac46");
	CountingOps counting;
	const Material material = load_units();
	cordmux::config::Settings settings;

	Recorder recorder(settings, counting.build(), dir.path());
	recorder.on_connected();
	recorder.on_configure(screen_formats(64, 64, 10.0), {{"u1", "ゆーざ"}});

	// 1000 個のメッセージを送る。映像、黒埋め用のフレーム、音声を混ぜる。
	const std::vector<std::uint8_t> chunk = make_audio_chunk(480, 0.5f);
	for (int i = 0; i < 1000; ++i) {
		const std::vector<std::uint8_t> &unit = material.units[static_cast<std::size_t>(i % 10)];
		recorder.on_video(video_header(1, i * 100000, unit_is_keyframe(unit), i % 100 == 0), unit.data(),
				  unit.size(), steady_now(), steady_now());
		recorder.on_audio(audio_header(2, i * 10000, false), chunk.data(), chunk.size(), steady_now(),
				  steady_now());
	}
	recorder.on_disconnected(cordmux::ws::DisconnectReason::PeerClosed);
	recorder.stop(0);

	CHECK(counting.make_directories.load() == 0);
	CHECK(counting.open_file.load() == 0);
	CHECK(counting.append.load() == 0);
	CHECK(counting.start_child.load() == 0);
	CHECK(counting.free_space.load() == 0);
	CHECK_FALSE(any_file_under(dir.path()));

	const RecordingSnapshot snapshot = recorder.snapshot();
	CHECK_FALSE(snapshot.enabled);
	CHECK(snapshot.segment_count == 0);
	CHECK(snapshot.open_segment_count == 0);
	CHECK(snapshot.frames_written == 0);
}

TEST_CASE("録画中かを判定する関数を常に真を返す関数に差し替えると、OFF に戻した後もファイルを開く")
{
	const Material material = load_units();
	cordmux::config::Settings settings;

	// 既定の `gate`: OFF に戻した後のフレームでは、1 度もファイルを開かない。
	int real_after = 0;
	{
		const TempDir dir("gate_real");
		CountingOps counting;
		Recorder recorder(settings, counting.build(), dir.path());
		recorder.on_connected();
		recorder.on_configure(screen_formats(64, 64, 10.0), {{"u1", "u"}});
		recorder.set_enabled(true);
		recorder.set_enabled(false);
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		const int before = counting.open_file.load();
		feed_video(recorder, material, 1, 2, 0);
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		recorder.stop(0);
		real_after = counting.open_file.load() - before;
	}

	// 常に録画中と判定する `gate`: 同じ手順でファイルを開いてしまう。
	int mutant_after = 0;
	{
		const TempDir dir("gate_mutant");
		CountingOps counting;
		Recorder recorder(settings, counting.build(), dir.path(), &always_open_gate);
		recorder.on_connected();
		recorder.on_configure(screen_formats(64, 64, 10.0), {{"u1", "u"}});
		recorder.set_enabled(true);
		recorder.set_enabled(false);
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		const int before = counting.open_file.load();
		feed_video(recorder, material, 1, 2, 0);
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		recorder.stop(0);
		mutant_after = counting.open_file.load() - before;
	}

	CHECK(real_after == 0);
	CHECK(mutant_after > 0);
}

TEST_CASE("ON にしてから OFF にすると、読み直せる録画ログとセグメントのファイルが残る")
{
	const TempDir dir("ledger");
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	const Material material = load_units();

	{
		Recorder recorder(settings, make_real_record_platform_ops(log), dir.path());
		recorder.on_connected();
		recorder.on_configure(screen_formats(64, 64, 10.0), {{"u1", "ゆーざ 1"}});
		recorder.set_enabled(true);
		feed_video(recorder, material, 1, 1, 0);
		const std::vector<std::uint8_t> chunk = make_audio_chunk(480, 0.25f);
		for (int i = 0; i < 100; ++i)
			recorder.on_audio(audio_header(2, i * 10000, false), chunk.data(), chunk.size(),
					  steady_now(), steady_now());
		recorder.set_enabled(false);
		recorder.stop(2000);
	}

	const std::string session = only_materials_dir(dir.path());
	const LedgerDocument doc = read_ledger(session);
	CHECK(doc.session.schema == cordmux::record::kLedgerSchema);
	CHECK(doc.session.session_id.size() == 19); // YYYY-MM-DD hh-mm-ss の形 (19 文字)
	CHECK_FALSE(doc.session.ffmpeg_version.empty());
	CHECK(doc.skipped_lines == 0);

	REQUIRE(doc.people.size() == 1);
	CHECK(doc.people.front().user_id == "u1");
	CHECK(doc.people.front().display_name == "ゆーざ 1");

	REQUIRE(doc.segments.size() == 2);
	const cordmux::record::LedgerSegmentRow *video = nullptr;
	const cordmux::record::LedgerSegmentRow *audio = nullptr;
	for (const cordmux::record::LedgerSegmentRow &row : doc.segments) {
		if (row.media == MediaKind::Video)
			video = &row;
		else
			audio = &row;
	}
	REQUIRE(video != nullptr);
	REQUIRE(audio != nullptr);
	CHECK(video->kind == StreamKind::Screen);
	CHECK(video->sample_count == 10);
	CHECK(video->duration_ticks == 60000);
	CHECK(video->timescale == 60000);
	CHECK(video->width == 64);
	CHECK(video->height == 64);
	CHECK(video->fps_num == 10);
	CHECK(video->fps_den == 1);
	CHECK(video->starts_with_idr);
	CHECK_FALSE(video->avcc_hex.empty());
	CHECK(video->bytes > 0);
	CHECK(video->close_reason == CloseReason::RecordingToggled);
	CHECK(audio->kind == StreamKind::Voice);
	CHECK(audio->sample_count == 48000);
	CHECK(audio->timescale == 48000);

	// セグメントのファイルが、実際にその名前で置かれている。
	std::error_code ec;
	CHECK(std::filesystem::exists(
		std::filesystem::u8path(cordmux::record::join_path(session, video->file)), ec));
	CHECK(std::filesystem::exists(
		std::filesystem::u8path(cordmux::record::join_path(session, audio->file)), ec));
}

TEST_CASE("録画中に名前を変えると、終了した時点の名前が録画ログに残り、終了した後の改名は動作しない")
{
	const TempDir dir("ledger");
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	const Material material = load_units();

	{
		Recorder recorder(settings, make_real_record_platform_ops(log), dir.path());
		recorder.on_connected();
		recorder.on_configure(screen_formats(64, 64, 10.0), {{"u1", "旧"}});
		recorder.set_enabled(true);
		feed_video(recorder, material, 1, 1, 0);
		// セグメントを開いた後の改名。セグメントは開いたままなので、人の行を足すのは終了のときだけになる。
		recorder.on_configure(screen_formats(64, 64, 10.0), {{"u1", "新"}});
		recorder.set_enabled(false);
		// 終了した後の改名。
		recorder.on_configure(screen_formats(64, 64, 10.0), {{"u1", "後"}});
		recorder.stop(2000);
	}

	const LedgerDocument doc = read_ledger(only_materials_dir(dir.path()));
	REQUIRE(doc.people.size() == 1);
	CHECK(doc.people.front().display_name == "新");
}

TEST_CASE("同じ Recorder で 2 回録画すると、2 回目のセッションの録画ログにも、前のセッションにいた人の行が入る")
{
	const TempDir dir("two_sessions");
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	const Material material = load_units();

	{
		Recorder recorder(settings, make_real_record_platform_ops(log), dir.path());
		recorder.on_connected();
		recorder.on_configure(screen_formats(64, 64, 10.0), {{"u1", "ゆーざ 1"}});
		recorder.set_enabled(true);
		feed_video(recorder, material, 1, 1, 0);
		recorder.set_enabled(false);
		// 同じ人の 2 回目のセッション。人の行の添字が前のセッションのまま残っていると、ここでセグメントを開いたときに、
		// 空になった人の一覧を古い添字で読む。
		recorder.set_enabled(true);
		feed_video(recorder, material, 1, 1, 1000000);
		recorder.set_enabled(false);
		recorder.stop(2000);
	}

	const std::string root = dir.path();
	const std::vector<std::string> names = cordmux::record::list_subdirectories(root);
	REQUIRE(names.size() == 2);
	for (const std::string &name : names) {
		const LedgerDocument doc = read_ledger(cordmux::record::join_path(
			cordmux::record::join_path(root, name), cordmux::record::kMaterialsDirName));
		REQUIRE(doc.people.size() == 1);
		CHECK(doc.people.front().user_id == "u1");
		CHECK(doc.people.front().display_name == "ゆーざ 1");
		CHECK(doc.segments.size() == 1);
	}
}

TEST_CASE("録画ログの session の行にプラグインのバージョンとダミーの時間帯が入り、セグメントの行に SPS と PPS を付けた回数が入る")
{
	const TempDir dir("facts");
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	const Material material = load_units();

	{
		Recorder recorder(settings, make_real_record_platform_ops(log), dir.path());
		cordmux::record::SessionFacts facts;
		facts.plugin_version = "0.9.1";
		facts.sps_pps_prepended = 3;
		cordmux::pipeline::DummyInterval interval;
		interval.started_at_us = 1000;
		interval.ended_at_us = 2000;
		facts.dummy_intervals.push_back(interval);
		// 録画が OFF の間に渡してよい (RecordPlatformOps の関数を呼ばない)。
		recorder.on_session_facts(facts);

		recorder.on_connected();
		recorder.on_configure(screen_formats(64, 64, 10.0), {{"u1", "u"}});
		recorder.set_enabled(true);
		feed_video(recorder, material, 1, 1, 0);
		recorder.set_enabled(false);
		recorder.stop(2000);
	}

	const LedgerDocument doc = read_ledger(only_materials_dir(dir.path()));
	CHECK(doc.session.plugin_version == "0.9.1");
	REQUIRE(doc.session.dummy_intervals.size() == 1);
	CHECK(doc.session.dummy_intervals.front().started_at_us == 1000);
	CHECK(doc.session.dummy_intervals.front().ended_at_us == 2000);
	// SPS と PPS を付けた回数は、セグメントを閉じた時点の累計が、セグメントの行に入る。
	for (const cordmux::record::LedgerSegmentRow &row : doc.segments)
		CHECK(row.sps_pps_prepended_count == 3);
}

TEST_CASE("OFF に戻すと、書き込むスレッドが残らない")
{
	const TempDir dir("writer_life");
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;

	Recorder recorder(settings, make_real_record_platform_ops(log), dir.path());
	CHECK_FALSE(recorder.writer_running()); // 一度も ON にしていない
	recorder.on_connected();
	recorder.on_configure(screen_formats(64, 64, 10.0), {{"u1", "u"}});
	recorder.set_enabled(true);
	CHECK(recorder.writer_running());
	recorder.set_enabled(false);
	CHECK_FALSE(recorder.writer_running()); // OFF ならスレッドは残らない
	// もう一度 ON にすれば、スレッドを起動し直す。
	recorder.set_enabled(true);
	CHECK(recorder.writer_running());
	recorder.stop(2000);
	CHECK_FALSE(recorder.writer_running());
}

TEST_CASE("空き容量が尽きると自分で止まり、その理由を見た呼び出し元が書き込むスレッドを止められる")
{
	const TempDir dir("space_out");
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	settings.record_space_check_interval_s = 1; // 監視の最短の間隔 (config/settings.h の下限)
	const Material material = load_units();

	// 空き容量は 0 と答える。録画ファイルの作成の子プロセスは起動しない (ここで確認するのは止まり方だけ)。
	RecordPlatformOps ops = make_real_record_platform_ops(log);
	ops.free_space_bytes = [](const std::string &) { return std::optional<std::uint64_t>(0); };
	ops.ffmpeg_path = []() { return std::string(); };

	Recorder recorder(settings, std::move(ops), dir.path());
	recorder.on_connected();
	recorder.on_configure(screen_formats(64, 64, 10.0), {{"u1", "u"}});
	recorder.set_enabled(true);
	// セグメントを開いて、書いたバイト数を増やす。開いているセグメントも書いた量に数えるので、閉じなくても監視は
	// 残り時間を求められる。
	feed_video(recorder, material, 1, 2, 0);

	RecordingSnapshot snapshot;
	for (int i = 0; i < 200; ++i) {
		snapshot = recorder.snapshot();
		if (snapshot.self_stopped_reason.has_value())
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
	}
	REQUIRE(snapshot.self_stopped_reason.has_value());
	CHECK(*snapshot.self_stopped_reason == cordmux::record::StopReason::SpaceExhausted);
	CHECK_FALSE(snapshot.enabled);
	// 書き込むスレッドは自分を join できないので、まだ動いている (join するのはスナップショットを定期的に読む側)。
	CHECK(recorder.writer_running());

	// スナップショットを見た側が set_enabled(false) を呼ぶと、スレッドが止まる。ON/OFF のフラグはもう
	// false なので、状態が変わらないときに何もしない形で戻ってはいけない。
	recorder.set_enabled(false);
	CHECK_FALSE(recorder.writer_running());

	// もう一度 ON にできる (押しても何も起きない状態にしない)。新しいセッションが始まると、止まった理由は消える。
	recorder.set_enabled(true);
	CHECK(recorder.writer_running());
	for (int i = 0; i < 200; ++i) {
		snapshot = recorder.snapshot();
		if (snapshot.enabled && !snapshot.self_stopped_reason.has_value())
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
	}
	CHECK(snapshot.enabled);
	CHECK_FALSE(snapshot.self_stopped_reason.has_value());
	recorder.stop(0);
	CHECK_FALSE(recorder.writer_running());
}

TEST_CASE("黒埋め用のフレームが作られたタイミングを、届いた順と時刻から推定して録画ログに書く")
{
	const TempDir dir("material_origin");
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	const Material material = load_units();

	// 画面共有のストリームを 3 つ (1 人に 1 つずつ)。声は使わない。
	std::vector<cordmux::pipeline::StreamFormat> formats;
	const char *users[] = {"u1", "u2", "u3"};
	for (int i = 0; i < 3; ++i) {
		cordmux::pipeline::StreamFormat screen;
		screen.stream_id = static_cast<std::uint32_t>(1 + i * 2);
		screen.user_id = users[i];
		screen.screen = true;
		screen.width = 64;
		screen.height = 64;
		screen.fps = 10.0;
		formats.push_back(screen);
	}

	{
		Recorder recorder(settings, make_real_record_platform_ops(log), dir.path());
		recorder.on_connected();
		recorder.on_configure(formats, {{"u1", "a"}, {"u2", "b"}, {"u3", "c"}});
		recorder.set_enabled(true);

		// セッションが開くのを待って、その単調な時計の原点を使う。タイミングの推定は、ON からどれだけ経って
		// 届いたかを見るので、届いた時刻はテストが決める。
		std::int64_t origin_us = 0;
		for (int i = 0; i < 200 && origin_us == 0; ++i) {
			origin_us = recorder.snapshot().started_at_steady_us;
			if (origin_us == 0)
				std::this_thread::sleep_for(std::chrono::milliseconds(25));
		}
		REQUIRE(origin_us != 0);

		const auto feed = [&](std::uint32_t stream_id, bool is_material, std::int64_t at_us,
				      std::int64_t pts_base_us) {
			std::int64_t pts_us = pts_base_us;
			for (const std::vector<std::uint8_t> &unit : material.units) {
				recorder.on_video(video_header(stream_id, pts_us, unit_is_keyframe(unit),
							       is_material),
						  unit.data(), unit.size(), at_us, at_us);
				pts_us += 100000;
			}
		};

		// RecordingTurnedOn: 録画を ON にした直後に届いた (別のインスタンスで作られたもの)。
		feed(1, true, origin_us + 100000, 0);
		// EncoderTeardown: 通常のフレームがもう届いているストリームに、ずっと後から届いた (エンコーダを
		// 閉じるときのもの)。
		feed(3, false, origin_us + 10 * 1000000, 0);
		feed(3, true, origin_us + 11 * 1000000, 2000000);
		// EncoderCreated: 通常のフレームより前に、ON からずっと経って届いた (エンコーダを作った直後のもの)。
		feed(5, true, origin_us + 10 * 1000000, 0);

		recorder.set_enabled(false);
		recorder.stop(2000);
	}

	const LedgerDocument doc = read_ledger(only_materials_dir(dir.path()));
	std::map<std::string, cordmux::record::MaterialOrigin> origin_of;
	for (const cordmux::record::LedgerBlackRow &row : doc.blacks)
		origin_of[row.user_id] = row.origin;
	REQUIRE(origin_of.size() == 3);
	CHECK(origin_of["u1"] == cordmux::record::MaterialOrigin::RecordingTurnedOn);
	CHECK(origin_of["u2"] == cordmux::record::MaterialOrigin::EncoderTeardown);
	CHECK(origin_of["u3"] == cordmux::record::MaterialOrigin::EncoderCreated);
}

TEST_CASE("保存先に書けたかどうかを、スナップショットの save_dir_writable で示す")
{
	cordmux::config::Settings settings;

	SUBCASE("開ける保存先なら真になる") {
		const TempDir dir("writable_ok");
		auto log = std::make_shared<std::string>();
		Recorder recorder(settings, make_real_record_platform_ops(log), dir.path());
		CHECK_FALSE(recorder.snapshot().save_dir_writable.has_value()); // まだ確認していない
		recorder.set_enabled(true);
		REQUIRE(wait_until(recorder, [](const RecordingSnapshot &s) { return s.save_dir_writable.has_value(); }));
		const RecordingSnapshot snapshot = recorder.snapshot();
		REQUIRE(snapshot.save_dir_writable.has_value());
		CHECK(*snapshot.save_dir_writable);
		recorder.stop(0);
	}

	SUBCASE("フォルダーを作れない保存先なら偽になる") {
		const TempDir dir("writable_ng");
		CountingOps counting;
		RecordPlatformOps ops = counting.build();
		ops.make_directories = [](const std::string &) { return false; };
		Recorder recorder(settings, std::move(ops), dir.path());
		recorder.set_enabled(true);
		REQUIRE(wait_until(recorder, [](const RecordingSnapshot &s) { return s.save_dir_writable.has_value(); }));
		const RecordingSnapshot snapshot = recorder.snapshot();
		REQUIRE(snapshot.save_dir_writable.has_value());
		CHECK_FALSE(*snapshot.save_dir_writable);
		recorder.stop(0);
	}
}

TEST_CASE("解像度が変わると、セグメントを分ける")
{
	const TempDir dir("cut");
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	const Material material = load_units();

	{
		Recorder recorder(settings, make_real_record_platform_ops(log), dir.path());
		recorder.on_connected();
		recorder.on_configure(screen_formats(64, 64, 10.0), {{"u1", "u"}});
		recorder.set_enabled(true);
		feed_video(recorder, material, 1, 1, 0);
		// 前の解像度と比べるのは録画の側だけである (stream_formats は configure のたびに丸ごと置き換わる)。
		recorder.on_configure(screen_formats(64, 48, 10.0), {{"u1", "u"}});
		feed_video(recorder, material, 1, 1, 1000000);
		recorder.set_enabled(false);
		recorder.stop(2000);
	}

	const LedgerDocument doc = read_ledger(only_materials_dir(dir.path()));
	std::vector<const cordmux::record::LedgerSegmentRow *> videos;
	for (const cordmux::record::LedgerSegmentRow &row : doc.segments) {
		if (row.media == MediaKind::Video)
			videos.push_back(&row);
	}
	REQUIRE(videos.size() == 2);
	CHECK(videos[0]->close_reason == CloseReason::FormatChanged);
	CHECK(videos[0]->height == 64);
	CHECK(videos[1]->height == 48);
	CHECK(videos[1]->close_reason == CloseReason::RecordingToggled);
}

TEST_CASE("録画専用のフラグが付いた映像は黒埋め用のフレームとして書き、フラグが付いた音声は捨てて数える")
{
	const TempDir dir("material");
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	const Material material = load_units();

	{
		Recorder recorder(settings, make_real_record_platform_ops(log), dir.path());
		recorder.on_connected();
		recorder.on_configure(screen_formats(64, 64, 10.0), {{"u1", "u"}});
		recorder.set_enabled(true);
		// 黒埋め用のフレームを 10 個送る。この録画ログで、black の行の `origin` は RecordingTurnedOn になる。
		// ON にした直後に届くためで、`origin` は届いた順と時刻からの推定である (推定の規則は別のテストが
		// 確認する)。
		std::int64_t pts_us = 0;
		for (const std::vector<std::uint8_t> &unit : material.units) {
			recorder.on_video(video_header(1, pts_us, unit_is_keyframe(unit), true), unit.data(),
					  unit.size(), steady_now(), steady_now());
			pts_us += 100000;
		}
		// 録画専用のフラグが付いた音声は、捨てて数える。
		const std::vector<std::uint8_t> chunk = make_audio_chunk(480, 0.5f);
		recorder.on_audio(audio_header(2, 0, true), chunk.data(), chunk.size(), steady_now(), steady_now());
		// 通常のフレームが届いたら、黒埋め用のフレームのファイルを閉じる。
		feed_video(recorder, material, 1, 1, 2000000);
		const RecordingSnapshot live = recorder.snapshot();
		(void)live;
		recorder.set_enabled(false);
		recorder.stop(2000);
	}

	const std::string session = only_materials_dir(dir.path());
	const LedgerDocument doc = read_ledger(session);
	REQUIRE(doc.blacks.size() == 1);
	CHECK(doc.blacks.front().user_id == "u1");
	CHECK(doc.blacks.front().frame_count == 10);
	CHECK(doc.blacks.front().width == 64);
	CHECK_FALSE(doc.blacks.front().avcc_hex.empty());
	std::error_code ec;
	CHECK(std::filesystem::exists(
		std::filesystem::u8path(cordmux::record::join_path(session, doc.blacks.front().file)), ec));
}

TEST_CASE("黒埋め用のフレームの解像度は、configure ではなくそのフレームの SPS から取る")
{
	// 解像度を変えた直後は、新しい解像度の configure より先に、EncoderCreated の黒埋め用のフレームが
	// 届くことがある。
	// そのとき、black の行に前の解像度を書かないことを確認する。
	const TempDir dir("material_size");
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	const Material material = load_units();

	{
		Recorder recorder(settings, make_real_record_platform_ops(log), dir.path());
		recorder.on_connected();
		// 保存している解像度は 1920x1080 のまま。届くフレームは 64x64 (tests/fixtures/ のテストデータの
		// SPS)。
		recorder.on_configure(screen_formats(1920, 1080, 10.0), {{"u1", "u"}});
		recorder.set_enabled(true);
		std::int64_t pts_us = 0;
		for (const std::vector<std::uint8_t> &unit : material.units) {
			recorder.on_video(video_header(1, pts_us, unit_is_keyframe(unit), true), unit.data(),
					  unit.size(), steady_now(), steady_now());
			pts_us += 100000;
		}
		recorder.set_enabled(false);
		recorder.stop(2000);
	}

	const std::string session = only_materials_dir(dir.path());
	const LedgerDocument doc = read_ledger(session);
	REQUIRE(doc.blacks.size() == 1);
	CHECK(doc.blacks.front().width == 64);
	CHECK(doc.blacks.front().height == 64);
	CHECK(doc.blacks.front().frame_count == 10);
}

TEST_CASE("キューが溢れたらデータを捨てて数え、録画は続く")
{
	const TempDir dir("overflow");
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	settings.record_queue_limit = 16;
	const Material material = load_units();

	// 書き込むスレッドを最初の処理で止めておく (キューが確実に溢れるようにするため)。
	std::mutex gate_mu;
	std::condition_variable gate_cv;
	bool released = false;
	RecordPlatformOps ops = make_real_record_platform_ops(log);
	const auto real_make_directories = ops.make_directories;
	ops.make_directories = [&](const std::string &path) {
		std::unique_lock<std::mutex> lock(gate_mu);
		gate_cv.wait(lock, [&]() { return released; });
		lock.unlock();
		return real_make_directories(path);
	};

	std::int64_t dropped = 0;
	{
		Recorder recorder(settings, ops, dir.path());
		recorder.on_connected();
		recorder.on_configure(screen_formats(64, 64, 10.0), {{"u1", "u"}});
		recorder.set_enabled(true);
		feed_video(recorder, material, 1, 50, 0); // 500 フレーム (上限は 16)
		{
			std::lock_guard<std::mutex> lock(gate_mu);
			released = true;
		}
		gate_cv.notify_all();
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		// 録画は続く。溢れた後のキーフレームから書き始める。
		feed_video(recorder, material, 1, 2, 60000000);
		dropped = recorder.snapshot().frames_dropped_overflow;
		recorder.set_enabled(false);
		recorder.stop(2000);
	}

	CHECK(dropped > 0);
	const LedgerDocument doc = read_ledger(only_materials_dir(dir.path()));
	CHECK_FALSE(doc.segments.empty());
}

TEST_CASE("set_enabled は、状態が変わらないときは何もしない")
{
	const TempDir dir("toggle");
	CountingOps counting;
	cordmux::config::Settings settings;
	Recorder recorder(settings, counting.build(), dir.path());
	for (int i = 0; i < 10; ++i)
		recorder.set_enabled(false); // ドックは 500 ms ごとに同じ値を渡す
	CHECK(counting.make_directories.load() == 0);
	CHECK(counting.open_file.load() == 0);
	recorder.stop(0);
}

TEST_CASE("解像度か fps が分からない間は、セグメントを開かない")
{
	const TempDir dir("unknown");
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	const Material material = load_units();

	{
		Recorder recorder(settings, make_real_record_platform_ops(log), dir.path());
		recorder.on_connected();
		// 0 は、まだ分からないことを示す (pipeline::StreamFormat の約束)。
		recorder.on_configure(screen_formats(0, 0, 0.0), {{"u1", "u"}});
		recorder.set_enabled(true);
		feed_video(recorder, material, 1, 1, 0);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		CHECK(recorder.snapshot().open_segment_count == 0);
		// 値が届いたら、次のキーフレームから始まる。
		recorder.on_configure(screen_formats(64, 64, 10.0), {{"u1", "u"}});
		feed_video(recorder, material, 1, 1, 1000000);
		recorder.set_enabled(false);
		recorder.stop(2000);
	}

	const LedgerDocument doc = read_ledger(only_materials_dir(dir.path()));
	int videos = 0;
	for (const cordmux::record::LedgerSegmentRow &row : doc.segments) {
		if (row.media == MediaKind::Video)
			++videos;
	}
	CHECK(videos == 1);
}

TEST_CASE("Vesktop と繋がる前に ON にしておくと、映像が届くまでセグメントを開かず、届いたら最初のキーフレームから書き始める")
{
	const TempDir dir("on_before_connect");
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	const Material material = load_units();

	{
		Recorder recorder(settings, make_real_record_platform_ops(log), dir.path());
		recorder.set_enabled(true);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		RecordingSnapshot before = recorder.snapshot();
		CHECK(before.enabled);
		CHECK(before.segment_count == 0);
		CHECK(before.open_segment_count == 0);
		CHECK(before.bytes_written == 0);

		recorder.on_connected();
		recorder.on_configure(screen_formats(64, 64, 10.0), {{"u1", "u"}});
		feed_video(recorder, material, 1, 1, 0);
		recorder.set_enabled(false);
		recorder.stop(2000);
	}

	const LedgerDocument doc = read_ledger(only_materials_dir(dir.path()));
	int videos = 0;
	for (const cordmux::record::LedgerSegmentRow &row : doc.segments) {
		if (row.media == MediaKind::Video) {
			++videos;
			CHECK(row.starts_with_idr);
		}
	}
	CHECK(videos == 1);
}

TEST_CASE("録画の入力を通ったフレームについて、最後の到着時刻をストリームごとに記録する")
{
	const TempDir dir("recordonly");
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	const Material material = load_units();

	Recorder recorder(settings, make_real_record_platform_ops(log), dir.path());
	recorder.on_connected();
	recorder.on_configure(screen_formats(64, 64, 10.0), {{"u1", "u"}});
	recorder.set_enabled(true);
	feed_video(recorder, material, 1, 1, 0);
	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	const std::map<std::uint32_t, cordmux::pipeline::RecordOnlyFrameClock> clocks =
		recorder.record_only_frames();
	recorder.set_enabled(false);
	recorder.stop(2000);

	REQUIRE(clocks.count(1) == 1);
	CHECK(clocks.at(1).user_id == "u1");
	CHECK(clocks.at(1).screen);
	CHECK(clocks.at(1).last_frame_steady_us > 0);
}

TEST_CASE("configure からストリームが消えたら、そのストリームの最後の到着時刻も消す")
{
	// 枠から外した人の画面は、外した時点で録画をやめる。最後の到着時刻を残すと、ドックがその人を、録画のためだけに
	// 受け取っている人として、映像が止まったとみなすしきい値の間だけ数える。
	const TempDir dir("recordonly_gone");
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	const Material material = load_units();

	Recorder recorder(settings, make_real_record_platform_ops(log), dir.path());
	recorder.on_connected();
	const std::vector<cordmux::pipeline::StreamFormat> both = screen_formats(64, 64, 10.0);
	recorder.on_configure(both, {{"u1", "u"}});
	recorder.set_enabled(true);
	feed_video(recorder, material, 1, 1, 0);
	const auto has_screen = [&recorder] { return recorder.record_only_frames().count(1) == 1; };
	for (int i = 0; i < 500 && !has_screen(); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	REQUIRE(has_screen());

	// 声のストリームだけを残す。
	recorder.on_configure({both[1]}, {{"u1", "u"}});
	for (int i = 0; i < 500 && has_screen(); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	CHECK_FALSE(has_screen());

	recorder.set_enabled(false);
	recorder.stop(2000);
}

TEST_CASE("声の時計がタイムスタンプより +-500 ppm ずれても、録画ログのサンプル数はタイムスタンプの長さに合い、欠けた分は置き直し、"
	  "前へ戻ったらセグメントを分ける")
{
	// 40 秒分の声 (480 サンプルのチャンク) を、音の時計がタイムスタンプの時計より d_ppm 速い (負なら遅い) 速さで送る。
	// 10 秒の時点から 1 秒分が欠け、20 秒の時点からタイムスタンプが 300 ms 前へ飛ぶ。速さの差を補正しないと、20 秒で
	// 10 ms (480 サンプル) の差になる。
	for (const double d_ppm : {500.0, -500.0}) {
		CAPTURE(d_ppm);
		const TempDir dir("drift");
		auto log = std::make_shared<std::string>();
		cordmux::config::Settings settings;
		settings.record_queue_limit = cordmux::config::kRecordQueueLimitMax;
		const double chunk_ts_us = 10000.0 / (1.0 + d_ppm * 1e-6); // 1 チャンクのタイムスタンプの時計での長さ
		const std::int64_t ts0 = 1'000'000;
		const int chunks = 4000;
		const int gap_first = 1000;
		const int gap_chunks = 100;
		const int jump_first = 2000;
		const std::int64_t jump_us = -300'000;
		auto pts_of = [&](int i) {
			// +-0.5 ms の揺れ (決まった並び)。
			const double jitter = double((i * 7919) % 1001 - 500);
			return std::llround(double(ts0) + double(i) * chunk_ts_us + jitter) + (i >= jump_first ? jump_us : 0);
		};
		{
			Recorder recorder(settings, make_real_record_platform_ops(log), dir.path());
			recorder.on_connected();
			recorder.on_configure(screen_formats(64, 64, 10.0), {{"u1", "ゆーざ 1"}});
			recorder.set_enabled(true);
			const std::vector<std::uint8_t> chunk = make_audio_chunk(480, 0.25f);
			for (int i = 0; i < chunks; ++i) {
				if (i >= gap_first && i < gap_first + gap_chunks)
					continue;
				recorder.on_audio(audio_header(2, pts_of(i), false), chunk.data(), chunk.size(), steady_now(),
						  steady_now());
			}
			recorder.set_enabled(false);
			recorder.stop(2000);
		}

		const LedgerDocument doc = read_ledger(only_materials_dir(dir.path()));
		std::vector<const cordmux::record::LedgerSegmentRow *> rows;
		for (const cordmux::record::LedgerSegmentRow &row : doc.segments) {
			if (row.media == MediaKind::Audio && row.stream_id == 2)
				rows.push_back(&row);
		}
		REQUIRE(rows.size() == 2);
		const cordmux::record::LedgerSegmentRow &first = *rows[0];
		const cordmux::record::LedgerSegmentRow &second = *rows[1];
		CHECK(first.close_reason == CloseReason::ClockJump);
		CHECK(second.close_reason == CloseReason::RecordingToggled);
		CHECK(first.first_pts_us == pts_of(0));
		CHECK(second.first_pts_us == pts_of(jump_first));
		// 欠けた 1 秒の後に 1 回だけ置き直し、その量は欠けた長さである。
		REQUIRE(first.pts_reanchor.size() == 1);
		CHECK(first.pts_reanchor.front().at_pts_us == pts_of(gap_first + gap_chunks));
		CHECK(std::llabs(first.pts_reanchor.front().delta_us - std::llround(double(gap_chunks) * chunk_ts_us)) <= 2000);
		CHECK(second.pts_reanchor.empty());
		// 書いたサンプル数 (0 のサンプルを含む) は、最初のチャンクから最後のチャンクの終わりまでのタイムスタンプの長さに、
		// 2 ms (96 サンプル) の内で合う。
		const double first_span_us = double(pts_of(jump_first - 1)) + chunk_ts_us - double(pts_of(0));
		const double second_span_us = double(pts_of(chunks - 1)) + chunk_ts_us - double(pts_of(jump_first));
		std::printf("\n音の時計 %+.0f ppm: sample_count %lld (タイムスタンプの長さ %.1f), %lld (%.1f)\n", d_ppm,
			    static_cast<long long>(first.sample_count), first_span_us * 0.048,
			    static_cast<long long>(second.sample_count), second_span_us * 0.048);
		CHECK(std::fabs(double(first.sample_count) - first_span_us * 0.048) <= 96.0);
		CHECK(std::fabs(double(second.sample_count) - second_span_us * 0.048) <= 96.0);
		CHECK(std::llabs(first.last_pts_us - std::llround(double(pts_of(jump_first - 1)) + chunk_ts_us)) <= 2000);
		CHECK(std::llabs(second.last_pts_us - std::llround(double(pts_of(chunks - 1)) + chunk_ts_us)) <= 2000);
	}
}
