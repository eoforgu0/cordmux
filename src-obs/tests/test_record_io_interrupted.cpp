// SPDX-License-Identifier: Apache-2.0

// 中断したセッション (録画か録画ファイルの作成の途中で OBS か PC が止まったセッション) のテスト。録画ログが NotStarted か
// Running で終わり、読めるデータがあるセッションを作成待ちに数えること、ほかのプロセスがロックファイルを持つ
// セッションと自分が録画中のセッションを数えないこと、読めるデータがないセッションには何も書かないこと、途中で
// 切れたセグメントを読める所まで修復して録画ファイルを作成することを確認する。修復して録画ファイルを作成するテストは実際の FFmpeg を使い、
// 環境変数がなければスキップする。ロックファイルは、platform/ の実際の関数 (hold_lock_file と lock_file_held) で開く。

#include "doctest.h"

#include "config/settings.h"
#include "platform/platform.h"
#include "record/record_args.h"
#include "record/record_ledger.h"
#include "record/record_segment.h"
#include "record/record_writer.h"
#include "record/recorder.h"
#include "test_record_io_shared.h"

#include <algorithm>
#include <chrono>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/locking.h>
#endif
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using cordmux::record::annex_b_to_avcc;
using cordmux::record::build_avcc_from_annex_b;
using cordmux::record::CloseReason;
using cordmux::record::FileHandle;
using cordmux::record::FinalizeBranch;
using cordmux::record::FinalizeCheck;
using cordmux::record::FinalizeState;
using cordmux::record::join_path;
using cordmux::record::kInvalidFile;
using cordmux::record::kLedgerFileName;
using cordmux::record::kMaterialsDirName;
using cordmux::record::kSegmentsDirName;
using cordmux::record::kSessionLockFileName;
using cordmux::record::LedgerDocument;
using cordmux::record::LedgerFinalizeRow;
using cordmux::record::LedgerPersonRow;
using cordmux::record::LedgerSegmentRow;
using cordmux::record::LedgerSessionRow;
using cordmux::record::make_real_record_platform_ops;
using cordmux::record::MediaKind;
using cordmux::record::parse_ledger;
using cordmux::record::read_whole_file;
using cordmux::record::Recorder;
using cordmux::record::RecordingSnapshot;
using cordmux::record::RecordPhase;
using cordmux::record::RecordPlatformOps;
using cordmux::record::repaired_segment_file;
using cordmux::record::SegmentAudioSpec;
using cordmux::record::SegmentVideoSpec;
using cordmux::record::SegmentWriter;
using cordmux::record::StopReason;
using cordmux::record::StreamKind;
using cordmux::record::to_hex;
using cordmux::record::to_jsonl;
using cordmux::record::TrackKind;
using cordmux::tests::record_io::ffmpeg_exe;
using cordmux::tests::record_io::have_real_ffmpeg;
using cordmux::tests::record_io::make_audio_chunk;
using cordmux::tests::record_io::probe_audio;
using cordmux::tests::record_io::probe_video;
using cordmux::tests::record_io::ProbeStream;
using cordmux::tests::record_io::read_h264_fixture;
using cordmux::tests::record_io::run_tool;
using cordmux::tests::record_io::split_access_units;
using cordmux::tests::record_io::TempDir;
using cordmux::tests::record_io::unit_is_keyframe;
using cordmux::tests::record_io::ffprobe_exe;

namespace {

constexpr const char *kSessionName = "2000-01-01 21-00-00";

template <typename Pred> bool wait_until(Recorder &recorder, Pred pred)
{
	for (int i = 0; i < 1000; ++i) {
		if (pred(recorder.snapshot()))
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return pred(recorder.snapshot());
}

std::string session_dir_of(const std::string &base, const std::string &name = kSessionName)
{
	return join_path(base, name);
}

std::string materials_of(const std::string &session)
{
	return join_path(session, kMaterialsDirName);
}

void write_text(const std::string &path, const std::string &text)
{
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::u8path(path).parent_path(), ec);
	std::ofstream file(std::filesystem::u8path(path), std::ios::binary);
	file << text;
}

std::string read_text(const std::string &path)
{
	std::string text;
	read_whole_file(path, &text);
	return text;
}

bool exists(const std::string &path)
{
	std::error_code ec;
	return std::filesystem::exists(std::filesystem::u8path(path), ec);
}

// 中断したセッションの録画ログ (session の行、人の行、セグメントの行)。`state` は、session の行に書く録画ファイルの作成の状態。
// `closed_voice` が真なら、閉じた (長さを示す) 声のセグメントの行を 1 つ持つ。セグメントのファイルは置かない
// (集計は閉じた行の長さを信じる)。
std::string interrupted_ledger(FinalizeState state, bool closed_voice)
{
	LedgerSessionRow session;
	session.session_id = kSessionName;
	session.started_at_steady_us = 1'000'000;
	session.finalize_state = state;
	if (state == FinalizeState::Running) {
		session.ended_at_steady_us = 61'000'000;
		session.stopped_reason = StopReason::TurnedOff;
	}
	std::string text = to_jsonl(session) + "\n";
	LedgerPersonRow person;
	person.user_id = "u1";
	person.display_name = "hana";
	person.kinds.push_back(TrackKind::Voice);
	text += to_jsonl(person) + "\n";
	if (closed_voice) {
		LedgerSegmentRow voice;
		voice.user_id = "u1";
		voice.kind = StreamKind::Voice;
		voice.media = MediaKind::Audio;
		voice.file = "segments/u1_voice_000001.mov";
		voice.timescale = 48000;
		voice.sample_count = 48000;
		voice.duration_ticks = 48000;
		voice.first_pts_steady_us = 1'000'000;
		voice.close_reason = CloseReason::RecordingToggled;
		text += to_jsonl(voice) + "\n";
	}
	return text;
}

// 閉じていない声のセグメントの行 (セグメントを開いた時点で Recorder が書くもの)。
LedgerSegmentRow open_voice_row(const std::string &file)
{
	LedgerSegmentRow row;
	row.user_id = "u1";
	row.kind = StreamKind::Voice;
	row.media = MediaKind::Audio;
	row.file = file;
	row.timescale = 48000;
	row.first_pts_us = 0;
	row.first_pts_steady_us = 1'000'000;
	row.truncated = true;
	row.close_reason = CloseReason::NotClosed;
	return row;
}

std::string write_session(const std::string &base, const std::string &name, const std::string &text)
{
	const std::string ledger = join_path(materials_of(session_dir_of(base, name)), kLedgerFileName);
	write_text(ledger, text);
	return ledger;
}

std::vector<cordmux::pipeline::StreamFormat> voice_formats()
{
	cordmux::pipeline::StreamFormat voice;
	voice.stream_id = 2;
	voice.user_id = "u1";
	voice.screen = false;
	return {voice};
}

void feed_voice(Recorder &recorder, int chunks)
{
	const std::vector<std::uint8_t> chunk = make_audio_chunk(480, 0.25f);
	for (int i = 0; i < chunks; ++i) {
		cordmux::ws::FrameHeader header;
		header.type = cordmux::ws::FrameType::Audio;
		header.stream_id = 2;
		header.timestamp_us = static_cast<std::uint64_t>(i) * 10000;
		header.flags = 0;
		const std::int64_t now = cordmux::platform::now_steady_us();
		recorder.on_audio(header, chunk.data(), chunk.size(), now, now);
	}
}

} // namespace

TEST_CASE("録画ログが NotStarted か Running で終わり、読めるデータがある中断したセッションは、作成待ちに数える")
{
	const TempDir dir("int_count");
	write_session(dir.path(), "2000-01-01 21-00-00", interrupted_ledger(FinalizeState::NotStarted, true));
	write_session(dir.path(), "2000-01-01 22-00-00", interrupted_ledger(FinalizeState::Running, true));
	write_session(dir.path(), "2000-01-01 23-00-00", interrupted_ledger(FinalizeState::Done, true));

	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	Recorder recorder(settings, make_real_record_platform_ops(log), dir.path());
	const RecordingSnapshot now = recorder.snapshot();
	CHECK(now.pending_present); // 作成待ちのメッセージと再開のボタンを表示する条件 (ledger/actions.cpp)
	CHECK(now.pending_session_count == 2);
	// 中断したセッションは、失敗したセッションではない (失敗のメッセージに、その場所を表示しない)。
	CHECK(now.failed_session_dir.empty());
	// 数えるだけで、自動では作成しない。録画ログは 1 バイトも変わらない。
	CHECK(now.finalize.state == FinalizeState::NotStarted);
	CHECK(read_text(join_path(materials_of(session_dir_of(dir.path(), "2000-01-01 21-00-00")), kLedgerFileName)) ==
	      interrupted_ledger(FinalizeState::NotStarted, true));
	recorder.stop(0);
}

TEST_CASE("保存先の直下にある、録画ごとのフォルダーの名前の形でないフォルダーは、録画ログがあっても読まず数えない")
{
	const TempDir dir("int_name_shape");
	// 日時の名前のフォルダーは今までどおり数える (番号付きも)。
	write_session(dir.path(), "2000-01-01 21-00-00", interrupted_ledger(FinalizeState::NotStarted, true));
	write_session(dir.path(), "2000-01-01 21-00-00 (2)", interrupted_ledger(FinalizeState::Running, true));
	// D:\ のような保存先に並ぶ別のフォルダー。録画ログと同じ形のファイルを持っていても、録画ではないので読まない。
	write_session(dir.path(), "obs-call", interrupted_ledger(FinalizeState::NotStarted, true));
	write_session(dir.path(), "Program Files", interrupted_ledger(FinalizeState::Running, true));
	write_session(dir.path(), "2000-01-01_21-00-00", interrupted_ledger(FinalizeState::NotStarted, true));

	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	Recorder recorder(settings, make_real_record_platform_ops(log), dir.path());
	CHECK(recorder.snapshot().pending_session_count == 2);
	recorder.stop(0);
}

TEST_CASE("ほかのハンドルがロックファイルを持つセッションは録画ログの値に関係なく数えず、ハンドルを閉じると数える")
{
	const TempDir dir("int_lock_held");
	write_session(dir.path(), "2000-01-01 21-00-00", interrupted_ledger(FinalizeState::NotStarted, true));
	// 作成待ち (Pending) のセッションも、ほかの OBS が作成している間 (ロックファイルを持つ) は数えない。
	write_session(dir.path(), "2000-01-01 22-00-00", interrupted_ledger(FinalizeState::Pending, true));
	const std::string lock_a = join_path(materials_of(session_dir_of(dir.path(), "2000-01-01 21-00-00")),
					     kSessionLockFileName);
	const std::string lock_b = join_path(materials_of(session_dir_of(dir.path(), "2000-01-01 22-00-00")),
					     kSessionLockFileName);

	bool held = true;
	const FileHandle other_a = cordmux::platform::hold_lock_file(lock_a, &held);
	REQUIRE(other_a != kInvalidFile);
	CHECK_FALSE(held);
	const FileHandle other_b = cordmux::platform::hold_lock_file(lock_b, &held);
	REQUIRE(other_b != kInvalidFile);
	// 持たれているロックファイルは、同じプロセスの別のハンドルからも持てない (共有を許さずに開くため)。
	CHECK(cordmux::platform::hold_lock_file(lock_a, &held) == kInvalidFile);
	CHECK(held);
	CHECK(cordmux::platform::lock_file_held(lock_a));

	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	{
		Recorder recorder(settings, make_real_record_platform_ops(log), dir.path());
		CHECK_FALSE(recorder.snapshot().pending_present);
		CHECK(recorder.snapshot().pending_session_count == 0);
		// resume_pending を呼んでも、ほかのプロセスが持つセッションは積まない (録画ログに録画ファイルの作成の行が
		// 追加されない)。
		recorder.resume_pending();
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		CHECK(recorder.snapshot().finalize.state == FinalizeState::NotStarted);
		recorder.stop(0);
	}
	CHECK(read_text(join_path(materials_of(session_dir_of(dir.path(), "2000-01-01 21-00-00")), kLedgerFileName)) ==
	      interrupted_ledger(FinalizeState::NotStarted, true));

	// 持っていたハンドルを閉じると (クラッシュしたプロセスのハンドルを OS が閉じるのと同じ)、ロックファイルも
	// 消える。
	cordmux::platform::close(other_a);
	cordmux::platform::close(other_b);
	CHECK_FALSE(exists(lock_a));
	CHECK_FALSE(exists(lock_b));
	CHECK_FALSE(cordmux::platform::lock_file_held(lock_a));
	Recorder after(settings, make_real_record_platform_ops(log), dir.path());
	CHECK(after.snapshot().pending_session_count == 2);
	after.stop(0);
}

TEST_CASE("だれも開いていない古いロックファイルは使用中と判定せず、作成した後にロックファイルは残らない")
{
	const TempDir dir("int_stale_lock");
	const std::string ledger =
		write_session(dir.path(), kSessionName, interrupted_ledger(FinalizeState::NotStarted, true));
	const std::string lock = join_path(materials_of(session_dir_of(dir.path())), kSessionLockFileName);
	write_text(lock, ""); // だれも開いていないロックファイル (電源が落ちたときの残り)
	CHECK_FALSE(cordmux::platform::lock_file_held(lock));

	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	{
		Recorder recorder(settings, make_real_record_platform_ops(log), dir.path());
		REQUIRE(recorder.snapshot().pending_session_count == 1);
		// 数えるだけでは、ロックファイルに触らない (集計は読むだけ)。
		CHECK(exists(lock));
		// セグメントのファイルを置いていないセッションなので、録画ファイルの作成は失敗で終わる (同梱の FFmpeg もない)。
		// 失敗しても、録画ファイルの作成を終えたらロックファイルを閉じて、ファイルも消える。
		recorder.resume_pending();
		REQUIRE(wait_until(recorder, [](const RecordingSnapshot &s) {
			return s.finalize.state == FinalizeState::Failed && s.pending_present;
		}));
		recorder.stop(5000);
	}
	CHECK_FALSE(exists(lock));
	// 録画ログは Failed で終わり、止めた理由が記録されていなかったセッションであることを示す
	// (StopReason::Interrupted)。
	const LedgerDocument doc = parse_ledger(read_text(ledger));
	CHECK(doc.session.finalize_state == FinalizeState::Failed);
	CHECK(doc.session.stopped_reason == StopReason::Interrupted);
	// 失敗したセッションとして集計に残る (セグメントのファイルは削除していない)。
	Recorder again(settings, make_real_record_platform_ops(log), dir.path());
	CHECK(again.snapshot().pending_session_count == 1);
	CHECK(again.snapshot().failed_session_dir == session_dir_of(dir.path()));
	again.stop(0);
}

TEST_CASE("録画中のセッションは自分の Recorder も同じ保存先を見るもう 1 つの Recorder も数えず、録画ログには開いたセグメントの行がある")
{
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	// ロックファイルの関数を持たない RecordPlatformOps でも、自分のセッションは除く (ロックファイルを作れない
	// 保存先と同じ状態)。
	for (const bool with_lock : {true, false}) {
		CAPTURE(with_lock);
		// ループの 1 回ごとに別の保存先にする (前の録画ファイルの作成の失敗が、次の集計に入らないように)。
		const TempDir dir(with_lock ? "int_own_lock" : "int_own_nolock");
		RecordPlatformOps ops = make_real_record_platform_ops(log);
		if (!with_lock)
			ops.hold_lock = nullptr;
		Recorder recorder(settings, ops, dir.path());
		recorder.on_connected();
		recorder.on_configure(voice_formats(), {{"u1", "hana"}});
		recorder.set_enabled(true);
		REQUIRE(wait_until(recorder, [](const RecordingSnapshot &s) { return s.phase == RecordPhase::Recording; }));
		feed_voice(recorder, 30);
		REQUIRE(wait_until(recorder, [](const RecordingSnapshot &s) { return s.frames_written >= 30; }));
		const std::string session = recorder.snapshot().session_dir;
		REQUIRE_FALSE(session.empty());
		const std::string materials = materials_of(session);

		// 開いているセグメントの行が録画ログにある (閉じていないことを示す値が付いている)。
		const LedgerDocument during = parse_ledger(read_text(join_path(materials, kLedgerFileName)));
		REQUIRE(during.segments.size() == 1);
		CHECK(during.segments.front().truncated);
		CHECK(during.segments.front().close_reason == CloseReason::NotClosed);
		CHECK(during.session.finalize_state == FinalizeState::NotStarted);
		CHECK(exists(join_path(materials, kSessionLockFileName)) == with_lock);

		// 録画中に数え直しても (resume_pending は数え直す)、自分のセッションは数えず、積まない。
		recorder.resume_pending();
		CHECK_FALSE(recorder.snapshot().pending_present);
		CHECK(recorder.snapshot().phase == RecordPhase::Recording);
		if (with_lock) {
			// 同じ保存先を見るもう 1 つの Recorder (もう 1 つの OBS) は、ロックファイルを見て数えない。
			Recorder other(settings, make_real_record_platform_ops(log), dir.path());
			CHECK_FALSE(other.snapshot().pending_present);
			other.stop(0);
		}

		recorder.set_enabled(false);
		REQUIRE(wait_until(recorder, [](const RecordingSnapshot &s) {
			return s.finalize.state == FinalizeState::Failed || s.finalize.state == FinalizeState::Done;
		}));
		recorder.stop(5000);
		// 閉じたときの行が後から勝つ。録画ファイルの作成を終えたら、ロックファイルは残らない。
		const LedgerDocument after = parse_ledger(read_text(join_path(materials, kLedgerFileName)));
		REQUIRE(after.segments.size() == 1);
		CHECK_FALSE(after.segments.front().truncated);
		CHECK(after.segments.front().close_reason == CloseReason::RecordingToggled);
		CHECK(after.segments.front().sample_count > 0);
		CHECK_FALSE(exists(join_path(materials, kSessionLockFileName)));
	}
}

TEST_CASE("読めるデータが 1 つもない中断したセッションは数えず、セッションのフォルダーも録画ログも変えない")
{
	const TempDir dir("int_empty");
	// (1) session の行だけ (録画を始めた直後に止まった)。
	const std::string only_session = write_session(dir.path(), "2000-01-01 21-00-00",
						       interrupted_ledger(FinalizeState::NotStarted, false));
	// (2) 閉じていないセグメントの行はあるが、セグメントのファイルがない。
	std::string missing_text = interrupted_ledger(FinalizeState::NotStarted, false) +
				   to_jsonl(open_voice_row("segments/u1_voice_000001.mov")) + "\n";
	const std::string missing = write_session(dir.path(), "2000-01-01 22-00-00", missing_text);
	// (3) セグメントのファイルはあるが、ヘッダーだけでサンプルが 1 つもない (SegmentWriter を開いてすぐ止まった
	//     状態)。
	std::string header_text = interrupted_ledger(FinalizeState::Running, false) +
				  to_jsonl(open_voice_row("segments/u1_voice_000001.mov")) + "\n";
	const std::string header_only = write_session(dir.path(), "2000-01-01 23-00-00", header_text);
	const std::string header_file =
		join_path(materials_of(session_dir_of(dir.path(), "2000-01-01 23-00-00")), "segments/u1_voice_000001.mov");
	{
		std::error_code ec;
		std::filesystem::create_directories(std::filesystem::u8path(header_file).parent_path(), ec);
		SegmentWriter writer;
		REQUIRE(writer.open_audio(header_file, SegmentAudioSpec()));
		writer.abort();
	}
	REQUIRE(exists(header_file));
	// (4) 0 で埋まったセグメントのファイル (電源が落ちて中身が書かれなかった状態)。
	std::string zero_text = interrupted_ledger(FinalizeState::NotStarted, false) +
				to_jsonl(open_voice_row("segments/u1_voice_000001.mov")) + "\n";
	const std::string zeros = write_session(dir.path(), "2000-01-02 00-00-00", zero_text);
	write_text(join_path(materials_of(session_dir_of(dir.path(), "2000-01-02 00-00-00")),
			     "segments/u1_voice_000001.mov"),
		   std::string(4096, '\0'));

	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	Recorder recorder(settings, make_real_record_platform_ops(log), dir.path());
	CHECK_FALSE(recorder.snapshot().pending_present);
	CHECK(recorder.snapshot().pending_session_count == 0);
	recorder.resume_pending(); // 再開のボタンを押せない状態だが、呼ばれても何も積まない
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	CHECK(recorder.snapshot().finalize.state == FinalizeState::NotStarted);
	recorder.stop(0);

	// 録画ログは 1 バイトも変わらず、フォルダーもファイルも残っている (利用者のフォルダーを削除しない)。
	CHECK(read_text(only_session) == interrupted_ledger(FinalizeState::NotStarted, false));
	CHECK(read_text(missing) == missing_text);
	CHECK(read_text(header_only) == header_text);
	CHECK(read_text(zeros) == zero_text);
	CHECK(exists(header_file));
	for (const char *name : {"2000-01-01 21-00-00", "2000-01-01 22-00-00", "2000-01-01 23-00-00",
				 "2000-01-02 00-00-00"})
		CHECK_FALSE(exists(join_path(materials_of(session_dir_of(dir.path(), name)), kSessionLockFileName)));
}

namespace {

// 実際の ffprobe で、セグメントにあるパケットの位置と大きさを並べる。切る位置と、読めるはずの数を決める基準にする
// (製品の読み出しとは別のツールで数える)。
struct PacketSpan {
	std::int64_t pos = 0;
	std::int64_t size = 0;
};

std::vector<PacketSpan> probe_packets(const std::string &file, const std::string &log_path)
{
	std::vector<PacketSpan> out;
	std::string text;
	const int code = run_tool(ffprobe_exe(),
				  {"-v", "error", "-select_streams", "0", "-show_entries", "packet=pos,size", "-of",
				   "default=noprint_wrappers=1", file},
				  log_path, &text);
	REQUIRE(code == 0);
	PacketSpan current;
	bool have_pos = false;
	bool have_size = false;
	std::size_t at = 0;
	while (at < text.size()) {
		std::size_t end = text.find('\n', at);
		if (end == std::string::npos)
			end = text.size();
		std::string line = text.substr(at, end - at);
		while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
			line.pop_back();
		if (line.rfind("pos=", 0) == 0) {
			current.pos = std::stoll(line.substr(4));
			have_pos = true;
		} else if (line.rfind("size=", 0) == 0) {
			current.size = std::stoll(line.substr(5));
			have_size = true;
		}
		if (have_pos && have_size) {
			out.push_back(current);
			have_pos = false;
			have_size = false;
		}
		at = end + 1;
	}
	return out;
}

// 切る位置は最後のパケットの真ん中。読めるはずのものは、切る位置より前で終わるパケットである。
struct CutPlan {
	std::int64_t cut = 0;
	std::int64_t readable_packets = 0;
	std::int64_t readable_bytes_end = 0; // 読めるはずの最後のパケットが終わる位置
	std::int64_t readable_audio_samples = 0;
};

CutPlan plan_cut(const std::vector<PacketSpan> &packets, int audio_block)
{
	REQUIRE(packets.size() >= 2);
	CutPlan plan;
	const PacketSpan &last = packets.back();
	plan.cut = last.pos + last.size / 2;
	for (const PacketSpan &p : packets) {
		if (p.pos + p.size <= plan.cut) {
			++plan.readable_packets;
			plan.readable_bytes_end = std::max(plan.readable_bytes_end, p.pos + p.size);
			if (audio_block > 0)
				plan.readable_audio_samples += p.size / audio_block;
		}
	}
	return plan;
}

void truncate_file(const std::string &path, std::int64_t size)
{
	std::error_code ec;
	std::filesystem::resize_file(std::filesystem::u8path(path), static_cast<std::uintmax_t>(size), ec);
	REQUIRE_FALSE(ec);
}

} // namespace

namespace {

// 映像のセグメントで、各フレームの時刻。10 fps の間隔で、12 フレーム目の後に 1 度だけ 200 ms 空ける (画面が動かない
// 間に送信側がフレームを間引いたときの、フレームの間隔がそろわないセッション)。
std::int64_t frame_pts_us(int i)
{
	return static_cast<std::int64_t>(i) * 100000 + (i >= 12 ? 100000 : 0);
}

// 中断したセッションの録画の中間ファイルと録画ログ。製品の SegmentWriter で書いたセグメントを閉じずに破棄し、
// フラグメントの途中で切った状態にする。
struct CrashImage {
	std::string session;
	std::string materials;
	std::string ledger_path;
	std::string probe_log;
	std::string video_file = "segments/u1_screen_video_000001.mov";
	std::string voice_file = "segments/u1_voice_000002.mov";
	CutPlan video_cut;
	CutPlan voice_cut;
};

CrashImage build_crash_image(const std::string &base)
{
	CrashImage image;
	image.session = session_dir_of(base);
	image.materials = materials_of(image.session);
	const std::string &materials = image.materials;
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::u8path(join_path(materials, kSegmentsDirName)), ec);

	// ---- 製品の SegmentWriter でセグメントを書き、閉じずに破棄する (OBS がクラッシュした状態。最後の完全な
	// フラグメントまでがディスクにある) ----
	const std::vector<std::vector<std::uint8_t>> units = split_access_units(read_h264_fixture());
	REQUIRE(units.size() == 10);
	const auto avcc = build_avcc_from_annex_b(units.front().data(), units.front().size());
	REQUIRE(avcc.has_value());
	const std::string &video_file = image.video_file;
	const std::string &voice_file = image.voice_file;
	{
		SegmentVideoSpec spec;
		spec.avcc = *avcc;
		spec.width = 64;
		spec.height = 64;
		spec.fps_num = 10;
		spec.fps_den = 1;
		SegmentWriter writer;
		REQUIRE(writer.open_video(join_path(materials, video_file), spec));
		for (int i = 0; i < 30; ++i) {
			const std::vector<std::uint8_t> &unit = units[static_cast<std::size_t>(i % 10)];
			const std::vector<std::uint8_t> sample = annex_b_to_avcc(unit.data(), unit.size());
			REQUIRE(writer.write_video(sample.data(), sample.size(), frame_pts_us(i),
						   unit_is_keyframe(unit)));
		}
		writer.abort();
	}
	{
		SegmentWriter writer;
		REQUIRE(writer.open_audio(join_path(materials, voice_file), SegmentAudioSpec()));
		const std::vector<std::uint8_t> chunk = make_audio_chunk(480, 0.5f);
		for (int i = 0; i < 300; ++i) // 3 秒
			REQUIRE(writer.write_audio(chunk.data(), chunk.size(), 480));
		writer.abort();
	}

	// ---- ffprobe でパケットの並びを読み、最後のパケットの真ん中で切る (電源が落ちた状態) ----
	image.probe_log = join_path(base, "probe.log");
	const std::string &probe_log = image.probe_log;
	const CutPlan video_cut = plan_cut(probe_packets(join_path(materials, video_file), probe_log), 0);
	const CutPlan voice_cut = plan_cut(probe_packets(join_path(materials, voice_file), probe_log), 8);
	REQUIRE(video_cut.readable_packets > 0);
	REQUIRE(voice_cut.readable_audio_samples > 0);
	truncate_file(join_path(materials, video_file), video_cut.cut);
	truncate_file(join_path(materials, voice_file), voice_cut.cut);
	image.video_cut = video_cut;
	image.voice_cut = voice_cut;

	// ---- 中断したセッションの録画ログ (Recorder が書く形) ----
	// session の行は NotStarted で、セグメントは開いた時点の行だけがあり、最後の行は途中で切れている。
	LedgerSessionRow row_session;
	row_session.session_id = kSessionName;
	row_session.started_at_steady_us = 1'000'000;
	LedgerPersonRow person;
	person.user_id = "u1";
	person.display_name = "hana";
	person.kinds = {TrackKind::ScreenVideo, TrackKind::Voice};
	LedgerSegmentRow video;
	video.user_id = "u1";
	video.kind = StreamKind::Screen;
	video.stream_id = 1;
	video.encoder_instance_id = "1-1";
	video.file = video_file;
	video.media = MediaKind::Video;
	video.avcc_hex = to_hex(*avcc);
	video.width = 64;
	video.height = 64;
	video.fps_num = 10;
	video.fps_den = 1;
	video.timescale = 60000;
	video.first_pts_us = 0;
	video.first_pts_steady_us = 1'000'000;
	video.starts_with_idr = true;
	video.truncated = true;
	video.close_reason = CloseReason::NotClosed;
	LedgerSegmentRow voice = open_voice_row(voice_file);

	// 黒埋め用のフレーム (EncoderCreated)。最初の通常のフレームが届いた時点で閉じて行を書くので、中断した
	// セッションにも普通はある。
	const std::string black_file = cordmux::record::black_file_name("u1", "1-1");
	std::filesystem::create_directories(std::filesystem::u8path(join_path(materials, "black")), ec);
	{
		SegmentVideoSpec spec;
		spec.avcc = *avcc;
		spec.width = 64;
		spec.height = 64;
		spec.fps_num = 10;
		spec.fps_den = 1;
		SegmentWriter writer;
		REQUIRE(writer.open_video(join_path(materials, black_file), spec));
		for (int i = 0; i < 10; ++i) {
			const std::vector<std::uint8_t> &unit = units[static_cast<std::size_t>(i)];
			const std::vector<std::uint8_t> sample = annex_b_to_avcc(unit.data(), unit.size());
			REQUIRE(writer.write_video(sample.data(), sample.size(), static_cast<std::int64_t>(i) * 100000,
						   unit_is_keyframe(unit)));
		}
		REQUIRE(writer.close(nullptr));
	}
	cordmux::record::LedgerBlackRow black;
	black.user_id = "u1";
	black.encoder_instance_id = "1-1";
	black.file = black_file;
	black.avcc_hex = to_hex(*avcc);
	black.width = 64;
	black.height = 64;
	black.fps_num = 10;
	black.fps_den = 1;
	black.frame_count = 10;
	black.duration_ticks = 60000;

	image.ledger_path = join_path(materials, kLedgerFileName);
	write_text(image.ledger_path, to_jsonl(row_session) + "\n" + to_jsonl(person) + "\n" + to_jsonl(black) + "\n" +
					to_jsonl(video) + "\n" + to_jsonl(voice) + "\n" +
					"{\"type\":\"segment\",\"user_id\":\"u");
	return image;
}

} // namespace

TEST_CASE("実際の ffmpeg で録画ファイルを作成すると、フラグメントの途中で切れたセグメントは読める所まで完成、Done になって集計から外れる" *
	  doctest::skip(!have_real_ffmpeg()))
{
	const TempDir dir("int_truncated");
	const CrashImage image = build_crash_image(dir.path());
	const std::string &session = image.session;
	const std::string &materials = image.materials;
	const std::string &ledger_path = image.ledger_path;
	const std::string &probe_log = image.probe_log;
	const std::string &video_file = image.video_file;
	const std::string &voice_file = image.voice_file;
	const CutPlan &video_cut = image.video_cut;
	const CutPlan &voice_cut = image.voice_cut;

	auto log = std::make_shared<std::string>();
	RecordPlatformOps ops = make_real_record_platform_ops(log);
	ops.ffmpeg_path = []() { return ffmpeg_exe(); };
	cordmux::config::Settings settings;
	{
		Recorder recorder(settings, ops, dir.path());
		REQUIRE(recorder.snapshot().pending_session_count == 1);
		recorder.resume_pending();
		REQUIRE(wait_until(recorder, [](const RecordingSnapshot &s) {
			return s.finalize.state != FinalizeState::NotStarted && s.finalize.state != FinalizeState::Running;
		}));
		const RecordingSnapshot done = recorder.snapshot();
		if (done.finalize.state != FinalizeState::Done) {
			MESSAGE("child log (" << *log << "): " << read_text(*log));
			MESSAGE("ledger: " << read_text(ledger_path));
			// 原因を調べるために、修復したセグメントと元のセグメントのパケットの並びを表示する。
			for (const std::string &file : {repaired_segment_file(video_file), video_file}) {
				std::string packets;
				run_tool(ffprobe_exe(),
					 {"-v", "error", "-show_entries", "packet=pts,dts,duration,pos,size,flags", "-of", "csv",
					  join_path(materials, file)},
					 probe_log, &packets);
				MESSAGE(file << " packets: " << packets);
			}
		}
		CHECK(done.finalize.state == FinalizeState::Done);
		CHECK(done.finalize.files_done == 2);
		CHECK(done.finalize.files_failed == 0);
		CHECK_FALSE(done.pending_present); // 完成したセッションは集計から外れる (再開のボタンが消える)
		recorder.stop(10000);
	}

	// ---- 録画ログ: 読めた長さと捨てた末尾、Matched の録画ファイルの作成の行、Done と interrupted ----
	const LedgerDocument doc = parse_ledger(read_text(ledger_path));
	CHECK(doc.session.finalize_state == FinalizeState::Done);
	CHECK(doc.session.stopped_reason == StopReason::Interrupted);
	const auto row_of = [&doc](const std::string &file) -> const LedgerSegmentRow * {
		for (const LedgerSegmentRow &row : doc.segments)
			if (row.file == file)
				return &row;
		return nullptr;
	};
	const LedgerSegmentRow *video_row = row_of(video_file);
	const LedgerSegmentRow *voice_row = row_of(voice_file);
	REQUIRE(video_row != nullptr);
	REQUIRE(voice_row != nullptr);
	CHECK(video_row->truncated);
	CHECK(video_row->sample_count == video_cut.readable_packets);
	// 長さは、読めた最後のフレームの時刻に、フレームの間隔を足した値 (10 fps なので、タイムスケール 60000 で
	// 100 ms = 6000)。
	CHECK(video_row->duration_ticks == frame_pts_us(static_cast<int>(video_cut.readable_packets) - 1) * 6 / 100 + 6000);
	CHECK(video_row->dropped_tail_bytes == video_cut.cut - video_cut.readable_bytes_end);
	CHECK(voice_row->sample_count == voice_cut.readable_audio_samples);
	CHECK(voice_row->dropped_tail_bytes == voice_cut.cut - voice_cut.readable_bytes_end);
	CHECK(voice_row->dropped_tail_bytes > 0);
	REQUIRE(doc.finalizes.size() == 2);
	for (const LedgerFinalizeRow &row : doc.finalizes) {
		CAPTURE(row.output_file);
		CHECK(row.exit_code == 0);
		CHECK(row.check == FinalizeCheck::Matched); // 長さの比較を通って Done になる
	}

	// ---- 録画ファイルを ffprobe で数える。映像は読めるフレーム数、声は読めるサンプルの数 ----
	for (const LedgerFinalizeRow &row : doc.finalizes) {
		const std::string output = join_path(session, row.output_file);
		if (row.kind == StreamKind::Screen) {
			// 修復したセグメントは、再エンコードしない Remux で並ぶ。フレームの間隔がそろわない
			// セッションでも、フレーム数は読めたフレーム数のままである。
			CHECK(row.branch == FinalizeBranch::Remux);
			const ProbeStream v = probe_video(output, probe_log);
			REQUIRE(v.present);
			CHECK(v.frames == video_cut.readable_packets);
		} else {
			const ProbeStream a = probe_audio(output, probe_log);
			REQUIRE(a.present);
			CHECK(a.sample_count == voice_cut.readable_audio_samples);
		}
	}

	// 途中で切れたセグメントのファイルは削除していない。ロックファイルは残らず、次の起動でも数えない。
	CHECK(exists(join_path(materials, video_file)));
	CHECK(exists(join_path(materials, voice_file)));
	CHECK_FALSE(exists(join_path(materials, kSessionLockFileName)));
	Recorder again(settings, ops, dir.path());
	CHECK_FALSE(again.snapshot().pending_present);
	again.stop(0);
}

TEST_CASE("実際の ffmpeg で録画ファイルを作成するとき、修復したファイルを書けないセグメントは外さず、失敗として作成待ちに残る" *
	  doctest::skip(!have_real_ffmpeg()))
{
	const TempDir dir("int_unwritten");
	const CrashImage image = build_crash_image(dir.path());
	// 修復したファイルの場所に同じ名前のフォルダーを置き、修復したファイルを書けない状態にする (保存先に書けない、
	// 空き容量が尽きたときの代わり)。
	const std::string blocked = join_path(image.materials, repaired_segment_file(image.video_file));
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::u8path(blocked), ec);
	REQUIRE(exists(blocked));

	auto log = std::make_shared<std::string>();
	RecordPlatformOps ops = make_real_record_platform_ops(log);
	ops.ffmpeg_path = []() { return ffmpeg_exe(); };
	cordmux::config::Settings settings;
	{
		Recorder recorder(settings, ops, dir.path());
		REQUIRE(recorder.snapshot().pending_session_count == 1);
		recorder.resume_pending();
		REQUIRE(wait_until(recorder, [](const RecordingSnapshot &s) {
			return s.finalize.state == FinalizeState::Failed && s.pending_present;
		}));
		// 画面共有の録画ファイルの作成は失敗し、声の録画ファイルは完成する (1 つの失敗で、ほかの録画ファイルの
		// 録画ファイルの作成を止めない)。
		CHECK(recorder.snapshot().finalize.files_failed == 1);
		CHECK(recorder.snapshot().finalize.files_done == 1);
		CHECK(recorder.snapshot().failed_session_dir == image.session);
		recorder.stop(10000);
	}
	const LedgerDocument doc = parse_ledger(read_text(image.ledger_path));
	CHECK(doc.session.finalize_state == FinalizeState::Failed);
	// 映像のセグメントは外されていない (読める長さが 0 の行で上書きされていない)。
	bool video_dropped = false;
	for (const LedgerSegmentRow &row : doc.segments) {
		if (row.file == image.video_file && row.sample_count == 0 && row.dropped_tail_bytes > 0)
			video_dropped = true;
	}
	CHECK_FALSE(video_dropped);
	CHECK_FALSE(exists(join_path(image.materials, kSessionLockFileName)));

	// 原因を取り除いてもう一度再開すれば、完成する (失敗のメッセージは消せる)。
	std::filesystem::remove(std::filesystem::u8path(blocked), ec);
	REQUIRE_FALSE(exists(blocked));
	Recorder again(settings, ops, dir.path());
	REQUIRE(again.snapshot().pending_session_count == 1);
	again.resume_pending();
	REQUIRE(wait_until(again, [](const RecordingSnapshot &s) {
		return s.finalize.state != FinalizeState::NotStarted && s.finalize.state != FinalizeState::Running;
	}));
	CHECK(again.snapshot().finalize.state == FinalizeState::Done);
	CHECK_FALSE(again.snapshot().pending_present);
	again.stop(10000);
}

namespace {

// ほかのソフトが、読み取りを許さずにファイルを開いている状態を作る (共有を許さない開き方)。閉じるまで、同じ
// ファイルを読む処理 (FFmpeg の avio) は開けない。
class ExclusiveHold {
public:
	explicit ExclusiveHold(const std::string &path)
	{
#ifdef _WIN32
		_wsopen_s(&fd_, std::filesystem::u8path(path).wstring().c_str(), _O_RDONLY | _O_BINARY, _SH_DENYRW, 0);
#endif
	}
	~ExclusiveHold() { release(); }
	ExclusiveHold(const ExclusiveHold &) = delete;
	ExclusiveHold &operator=(const ExclusiveHold &) = delete;
	bool held() const { return fd_ >= 0; }
	void release()
	{
#ifdef _WIN32
		if (fd_ >= 0)
			_close(fd_);
#endif
		fd_ = -1;
	}

private:
	int fd_ = -1;
};

} // namespace

TEST_CASE("copy_readable_part は、読み出しの失敗 (開けない) と、中身の形で読めない (0 バイト) を区別する")
{
	const TempDir dir("int_io_failed");
	const std::string voice = join_path(dir.path(), "voice.mov");
	{
		SegmentWriter writer;
		REQUIRE(writer.open_audio(voice, SegmentAudioSpec()));
		const std::vector<std::uint8_t> chunk = make_audio_chunk(480, 0.5f);
		for (int i = 0; i < 10; ++i)
			REQUIRE(writer.write_audio(chunk.data(), chunk.size(), 480));
		REQUIRE(writer.close(nullptr));
	}
	const cordmux::record::ReadablePart readable = cordmux::record::copy_readable_part(voice, std::string());
	CHECK(readable.opened);
	CHECK_FALSE(readable.io_failed);
	CHECK(readable.samples == 4800);

	// 0 バイトで残ったセグメント (電源が落ちて中身が書かれなかった状態) は、中身の形で読めない。録画ファイルの作成から外して
	// よいセグメントである。
	const std::string empty = join_path(dir.path(), "empty.mov");
	write_text(empty, "");
	const cordmux::record::ReadablePart zero = cordmux::record::copy_readable_part(empty, std::string());
	CHECK_FALSE(zero.opened);
	CHECK_FALSE(zero.io_failed);
	CHECK(zero.samples == 0);

	// ないファイルと、ほかのソフトが読み取りを許さずに開いているファイルは、読み出しの失敗として扱う。
	CHECK(cordmux::record::copy_readable_part(join_path(dir.path(), "missing.mov"), std::string()).io_failed);
	{
		ExclusiveHold hold(voice);
		REQUIRE(hold.held());
		const cordmux::record::ReadablePart locked = cordmux::record::copy_readable_part(voice, std::string());
		CHECK_FALSE(locked.opened);
		CHECK(locked.io_failed);
		CHECK(locked.samples == 0);
	}
}

TEST_CASE("修復中にサンプルの読み出しが途中で失敗したセグメントは、切れた末尾ではなく読み出しの失敗として扱う")
{
	const TempDir dir("int_io_mid_sample");
	const std::string voice = join_path(dir.path(), "voice.mov");
	constexpr int kChunks = 400;
	{
		SegmentWriter writer;
		REQUIRE(writer.open_audio(voice, SegmentAudioSpec()));
		const std::vector<std::uint8_t> chunk = make_audio_chunk(480, 0.5f);
		for (int i = 0; i < kChunks; ++i)
			REQUIRE(writer.write_audio(chunk.data(), chunk.size(), 480));
		REQUIRE(writer.close(nullptr));
	}
	std::error_code ec;
	const auto size = static_cast<long>(std::filesystem::file_size(std::filesystem::u8path(voice), ec));
	REQUIRE_FALSE(ec);
#ifdef _WIN32
	// 読んでいる途中で、別のハンドルがファイル全体の範囲をロックする (ReadFile が ERROR_LOCK_VIOLATION で失敗する。
	// OS の読み出しの失敗)。avio が先に読んだ所まではサンプルが出て、追加の読み込みの途中で失敗したサンプルは、短く
	// 読めた CORRUPT のサンプルになる。
	int fd = -1;
	_wsopen_s(&fd, std::filesystem::u8path(voice).wstring().c_str(), _O_RDONLY | _O_BINARY, _SH_DENYNO, 0);
	REQUIRE(fd >= 0);
	int calls = 0;
	bool locked = false;
	const auto lock_midway = [&calls, &locked, fd, size]() {
		// 打ち切りの要求は 64 サンプルごとに確認できる。2 回目 (64 サンプルを読んだ後) にロックする。
		if (++calls == 2) {
			_lseek(fd, 0, SEEK_SET);
			locked = _locking(fd, _LK_NBLCK, size) == 0;
		}
		return false;
	};
	const cordmux::record::ReadablePart part =
		cordmux::record::copy_readable_part(voice, join_path(dir.path(), "voice_repair.mov"), 0, 0, lock_midway);
	REQUIRE(locked);
	_lseek(fd, 0, SEEK_SET);
	_locking(fd, _LK_UNLCK, size);
	_close(fd);
	CHECK(part.opened);
	CHECK(part.samples > 0);
	CHECK(part.samples < kChunks * 480);
	// 切れた末尾として扱うと、後ろに読めるデータが残っていてもセグメントを短くして、セッションが Done になる。
	CHECK(part.io_failed);
#endif
}

TEST_CASE("実際の ffmpeg で録画ファイルを作成するとき、修復の時点で開けなかったセグメントは外さず、そのセグメントを使う録画ファイルの作成を失敗にして作成待ちに残す" *
	  doctest::skip(!have_real_ffmpeg()))
{
	const TempDir dir("int_unreadable");
	const CrashImage image = build_crash_image(dir.path());

	auto log = std::make_shared<std::string>();
	RecordPlatformOps ops = make_real_record_platform_ops(log);
	ops.ffmpeg_path = []() { return ffmpeg_exe(); };
	cordmux::config::Settings settings;
	{
		Recorder recorder(settings, ops, dir.path());
		REQUIRE(recorder.snapshot().pending_session_count == 1);
		// 再開した後の修復の間、映像のセグメントを、ほかのソフトが読み取りを許さずに開いている状態にする (声の
		// セグメントは読めるので、集計には数える)。
		ExclusiveHold hold(join_path(image.materials, image.video_file));
		REQUIRE(hold.held());
		recorder.resume_pending();
		REQUIRE(wait_until(recorder, [](const RecordingSnapshot &s) {
			return s.finalize.state == FinalizeState::Failed && s.pending_present;
		}));
		// 画面共有の録画ファイルの作成は失敗し、声の録画ファイルは完成する。セッションは失敗のメッセージを残したまま
		// 作成待ちに残る。
		// セグメントを外すと Done になり、ファイルが残っているのに集計から消える。
		CHECK(recorder.snapshot().finalize.files_failed == 1);
		CHECK(recorder.snapshot().finalize.files_done == 1);
		CHECK(recorder.snapshot().failed_session_dir == image.session);
		hold.release();
		recorder.stop(10000);
	}
	const LedgerDocument doc = parse_ledger(read_text(image.ledger_path));
	CHECK(doc.session.finalize_state == FinalizeState::Failed);
	// 映像のセグメントは外されていない (読める長さが 0 の行で上書きされていない)。
	bool video_dropped = false;
	for (const LedgerSegmentRow &row : doc.segments) {
		if (row.file == image.video_file && row.sample_count == 0 && row.dropped_tail_bytes > 0)
			video_dropped = true;
	}
	CHECK_FALSE(video_dropped);
	CHECK(exists(join_path(image.materials, image.video_file)));

	// ほかのソフトがファイルを閉じた後にもう一度再開すれば、完成する。
	Recorder again(settings, ops, dir.path());
	REQUIRE(again.snapshot().pending_session_count == 1);
	again.resume_pending();
	REQUIRE(wait_until(again, [](const RecordingSnapshot &s) {
		return s.finalize.state != FinalizeState::NotStarted && s.finalize.state != FinalizeState::Running;
	}));
	CHECK(again.snapshot().finalize.state == FinalizeState::Done);
	CHECK_FALSE(again.snapshot().pending_present);
	again.stop(10000);
}
