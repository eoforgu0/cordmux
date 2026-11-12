// SPDX-License-Identifier: Apache-2.0

// Recorder の段階、保存先、接続が切れていた期間、作成待ちの集計と再開、録画ファイルの作成の済んでいないセッションが残る
// 保存先の一覧のテスト。ほとんどのテストは実際の FFmpeg を起動しない。テストの実行ファイルの隣に同梱の FFmpeg が
// ないので、録画ファイルの作成は起動できずに失敗の行を録画ログに追加する。確認するのは、録画ログに書かれた名前と引数である。
// 実際の FFmpeg で録画ファイルを作成するテストは 1 つだけで、環境変数がなければスキップする。

#include "doctest.h"

#include "config/settings.h"
#include "record/record_ledger.h"
#include "platform/platform.h"
#include "record/record_search_dirs.h"
#include "record/record_segment.h"
#include "record/record_writer.h"
#include "record/recorder.h"
#include "test_record_io_shared.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#endif
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using cordmux::record::FileHandle;
using cordmux::record::join_path;
using cordmux::record::kLedgerFileName;
using cordmux::record::kMaterialsDirName;
using cordmux::record::LedgerDocument;
using cordmux::record::LedgerFinalizeRow;
using cordmux::record::LedgerPersonRow;
using cordmux::record::LedgerSegmentRow;
using cordmux::record::LedgerSessionRow;
using cordmux::record::list_subdirectories;
using cordmux::record::make_real_record_platform_ops;
using cordmux::record::MediaKind;
using cordmux::record::parse_ledger;
using cordmux::record::read_whole_file;
using cordmux::record::Recorder;
using cordmux::record::RecordingSnapshot;
using cordmux::record::RecordPhase;
using cordmux::record::RecordPlatformOps;
using cordmux::record::RecordSessionValues;
using cordmux::record::StreamKind;
using cordmux::tests::record_io::ffmpeg_exe;
using cordmux::tests::record_io::have_real_ffmpeg;
using cordmux::tests::record_io::make_audio_chunk;
using cordmux::tests::record_io::TempDir;

namespace {

// 条件が真になるまで待つ (上限 5 秒)。
template <typename Pred> bool wait_until(Recorder &recorder, Pred pred)
{
	for (int i = 0; i < 500; ++i) {
		if (pred(recorder.snapshot()))
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return pred(recorder.snapshot());
}

std::vector<cordmux::pipeline::StreamFormat> voice_formats()
{
	cordmux::pipeline::StreamFormat voice;
	voice.stream_id = 2;
	voice.user_id = "u1";
	voice.screen = false;
	return {voice};
}

cordmux::ws::FrameHeader audio_header(std::uint32_t stream_id, std::int64_t pts_us)
{
	cordmux::ws::FrameHeader header;
	header.type = cordmux::ws::FrameType::Audio;
	header.stream_id = stream_id;
	header.timestamp_us = static_cast<std::uint64_t>(pts_us);
	header.flags = 0;
	return header;
}

void feed_voice(Recorder &recorder, int chunks)
{
	const std::vector<std::uint8_t> chunk = make_audio_chunk(480, 0.25f);
	for (int i = 0; i < chunks; ++i) {
		const std::int64_t now = cordmux::platform::now_steady_us();
		recorder.on_audio(audio_header(2, i * 10000), chunk.data(), chunk.size(), now, now);
	}
}

std::string only_child(const std::string &dir)
{
	const std::vector<std::string> names = list_subdirectories(dir);
	REQUIRE(names.size() == 1);
	return join_path(dir, names.front());
}

LedgerDocument read_ledger_at(const std::string &path)
{
	std::string text;
	REQUIRE(read_whole_file(path, &text));
	return parse_ledger(text);
}

void write_text(const std::string &path, const std::string &text)
{
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::u8path(path).parent_path(), ec);
	std::ofstream file(std::filesystem::u8path(path), std::ios::binary);
	file << text;
}

// 作成待ち (Pending) の録画ログを 1 つ作る (声だけ。セグメントのファイルは置かない)。
std::string pending_ledger_text(const std::string &session_id, const std::string &name)
{
	LedgerSessionRow session;
	session.session_id = session_id;
	session.started_at_steady_us = 100'000'000;
	session.ended_at_steady_us = 101'000'000;
	session.finalize_state = cordmux::record::FinalizeState::Pending;
	LedgerPersonRow person;
	person.user_id = "u1";
	person.display_name = name;
	person.kinds.push_back(cordmux::record::TrackKind::Voice);
	LedgerSegmentRow voice;
	voice.user_id = "u1";
	voice.kind = StreamKind::Voice;
	voice.media = MediaKind::Audio;
	voice.file = "segments/u1_voice_000001.mov";
	voice.timescale = 48000;
	voice.sample_count = 48000;
	voice.duration_ticks = 48000;
	voice.first_pts_steady_us = 100'000'000;
	return cordmux::record::to_jsonl(session) + "\n" + cordmux::record::to_jsonl(person) + "\n" +
	       cordmux::record::to_jsonl(voice) + "\n";
}

// 録画ログへの追記を止めたり通したりできる、実際の RecordPlatformOps (止める処理が長引く状態を作るため)。
// `hold` が真の間、追記は `let_through` が残っていれば 1 つ減らして通り、なければ止まる。`blocked_total` は止まり
// 始めた回数で、書き込むスレッドが止めた所にいることを、時間に頼らずに知るために使う。
struct HoldableOps {
	std::atomic<bool> hold{false};
	std::atomic<int> let_through{0};
	std::atomic<int> blocked_total{0};
	RecordPlatformOps build(std::shared_ptr<std::string> log)
	{
		RecordPlatformOps ops = make_real_record_platform_ops(log);
		auto real_append = ops.append;
		ops.append = [this, real_append](FileHandle file, const void *data, std::size_t len) {
			if (hold.load() && !take_pass()) {
				++blocked_total;
				// 止まった所で待つ。`hold` が false になるか、通してよい数が増えたら通る。
				while (hold.load() && !take_pass())
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			return real_append(file, data, len);
		};
		return ops;
	}
	// 通してよい数を 1 つ使う (残っていなければ false)。
	bool take_pass()
	{
		int left = let_through.load();
		while (left > 0) {
			if (let_through.compare_exchange_weak(left, left - 1))
				return true;
		}
		return false;
	}
	// 止まり始めた回数が `count` に届くまで待つ (上限 5 秒)。
	bool wait_blocked(int count) const
	{
		for (int i = 0; i < 500; ++i) {
			if (blocked_total.load() >= count)
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return blocked_total.load() >= count;
	}
};

// 段階が Starting と Stopping を抜けるまで待つ。
bool wait_settled(Recorder &recorder)
{
	return wait_until(recorder, [](const RecordingSnapshot &s) {
		return s.phase != RecordPhase::Starting && s.phase != RecordPhase::Stopping;
	});
}

// データが実際に書かれるまで待つ (上限 5 秒)。
bool wait_frames_written(Recorder &recorder, std::int64_t more_than)
{
	return wait_until(recorder, [more_than](const RecordingSnapshot &s) { return s.frames_written > more_than; });
}

} // namespace

TEST_CASE("段階は Starting、Recording、Stopping、Idle の順に変わり、Stopping の間の ON は受け付けない")
{
	const TempDir dir("phases");
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	HoldableOps holdable;
	Recorder recorder(settings, holdable.build(log), dir.path());
	CHECK(recorder.snapshot().phase == RecordPhase::Idle);

	recorder.set_enabled(true);
	{
		const RecordingSnapshot now = recorder.snapshot();
		CHECK((now.phase == RecordPhase::Starting || now.phase == RecordPhase::Recording));
		CHECK(now.phase_since_wall_us > 0);
	}
	REQUIRE(wait_until(recorder, [](const RecordingSnapshot &s) { return s.phase == RecordPhase::Recording; }));
	CHECK(*recorder.snapshot().save_dir_writable);
	CHECK(recorder.snapshot().enabled);

	// 録画ログの最後の行の書き込みを止めておく (止める処理が長引く状態)。
	holdable.hold = true;
	const auto before = std::chrono::steady_clock::now();
	recorder.request_disable();
	const auto took = std::chrono::steady_clock::now() - before;
	// UI のスレッドで待たない。書き込むスレッドがまだ止まっていなくても、すぐ戻る。
	CHECK(took < std::chrono::milliseconds(500));
	{
		const RecordingSnapshot now = recorder.snapshot();
		CHECK(now.phase == RecordPhase::Stopping);
		CHECK_FALSE(now.enabled);
	}
	// 止めている最中の ON は受け付けない (ドックもボタンを押せなくする)。
	recorder.set_enabled(true);
	CHECK(recorder.snapshot().phase == RecordPhase::Stopping);
	CHECK_FALSE(recorder.snapshot().enabled);

	holdable.hold = false;
	REQUIRE(wait_until(recorder, [](const RecordingSnapshot &s) { return s.phase == RecordPhase::Idle; }));

	// 止め終わったら、もう一度始められる。
	recorder.set_enabled(true);
	REQUIRE(wait_until(recorder, [](const RecordingSnapshot &s) { return s.phase == RecordPhase::Recording; }));
	recorder.set_enabled(false);
	CHECK(recorder.snapshot().phase == RecordPhase::Idle);
	CHECK_FALSE(recorder.writer_running());
	recorder.stop(2000);
}

TEST_CASE("保存先に書けないときはセッションを始めずに録画を OFF に戻し、別の場所には移さない")
{
	cordmux::config::Settings settings;
	std::mutex mu;
	std::vector<std::string> asked;
	RecordPlatformOps ops = make_real_record_platform_ops(std::make_shared<std::string>());
	ops.make_directories = [&mu, &asked](const std::string &path) {
		std::lock_guard<std::mutex> lock(mu);
		asked.push_back(path);
		return false; // 書けない
	};
	ops.open_file = [](const std::string &) -> FileHandle { return cordmux::record::kInvalidFile; };
	const std::string base = "C:/cordmux-test-unwritable";
	Recorder recorder(settings, std::move(ops), base);

	recorder.set_enabled(true);
	REQUIRE(wait_until(recorder,
			   [](const RecordingSnapshot &s) { return s.phase == RecordPhase::StartFailed; }));
	const RecordingSnapshot failed = recorder.snapshot();
	CHECK_FALSE(failed.enabled); // ボタンを押された表示のまま残さない
	REQUIRE(failed.save_dir_writable.has_value());
	CHECK_FALSE(*failed.save_dir_writable);
	CHECK(failed.unwritable_dir == base); // 書けなかった場所を示す
	{
		// 試したのは選ばれた保存先とその下だけである (ビデオや AppData を試していない)。セッションのフォルダーを
		// make_new_directory で 1 階層だけ作るので、その親の保存先を先に作る (record_segment.h の ensure_session_open)。
		std::lock_guard<std::mutex> lock(mu);
		REQUIRE_FALSE(asked.empty());
		for (const std::string &path : asked)
			CHECK((path == base || path.rfind(base + "/", 0) == 0));
	}
	// 後始末 (自分で止まったセッションと同じ手順)。書き込むスレッドを止めても、StartFailed は残る。
	recorder.set_enabled(false);
	CHECK_FALSE(recorder.writer_running());
	CHECK(recorder.snapshot().phase == RecordPhase::StartFailed);
	recorder.stop(0);
}

TEST_CASE("保存先と PCM の形はセッションを始めた時点の値を使い、後から変えても前のセッションに及ばない")
{
	const TempDir first("values_first");
	const TempDir second("values_second");
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	{
		Recorder recorder(settings, make_real_record_platform_ops(log), first.path());
		RecordSessionValues values;
		values.base_dir = second.path(); // セッションを始める時点で解決した保存先
		values.output_pcm = cordmux::config::RecordPcmFormat::Int24;
		recorder.set_session_values(values);
		recorder.on_connected();
		recorder.on_configure(voice_formats(), {{"u1", "taro"}});
		recorder.set_enabled(true);
		REQUIRE(wait_until(recorder,
				   [](const RecordingSnapshot &s) { return s.phase == RecordPhase::Recording; }));
		feed_voice(recorder, 50);
		// セッションの途中で値を変える (次のセッションの値で、このセッションの録画ファイルの作成には及ばない)。
		values.output_pcm = cordmux::config::RecordPcmFormat::Float32;
		values.base_dir = first.path();
		recorder.set_session_values(values);
		recorder.set_enabled(false);
		recorder.stop(5000);
	}
	// セッションは 2 つ目の保存先に作られ、構築のときの場所には何もない。
	std::error_code ec;
	CHECK(std::filesystem::is_empty(std::filesystem::u8path(first.path()), ec)); // 保存先そのものの下には何も作らない
	const std::string session = only_child(second.path());
	const LedgerDocument doc = read_ledger_at(join_path(join_path(session, kMaterialsDirName), kLedgerFileName));
	REQUIRE_FALSE(doc.finalizes.empty());
	const LedgerFinalizeRow &row = doc.finalizes.front();
	CHECK(std::find(row.args.begin(), row.args.end(), "s24le") != row.args.end());
	// 録画ファイルの名前はセッションのフォルダーからの相対パスで、「名前 種類 日時」の形 (種類の語は既定)。
	CHECK(row.output_file == "taro Voice " + doc.session.session_id + ".mov");
	// セッションのフォルダーの直下には、録画の中間ファイルのフォルダーだけがある (録画ファイルの作成は失敗したので、
	// 録画ファイルはない)。
	const std::vector<std::string> inside = list_subdirectories(session);
	REQUIRE(inside.size() == 1);
	CHECK(inside.front() == kMaterialsDirName);
}

TEST_CASE("録画中に接続が切れていた期間を gaps に入れ、次のセッションを始めたら空にする")
{
	const TempDir dir("gaps");
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	Recorder recorder(settings, make_real_record_platform_ops(log), dir.path());
	recorder.on_connected();
	// 録画していない間の切断は数えない。
	recorder.on_disconnected(cordmux::ws::DisconnectReason::Unknown);
	recorder.on_connected();
	CHECK(recorder.snapshot().gaps.empty());

	recorder.set_enabled(true);
	REQUIRE(wait_until(recorder, [](const RecordingSnapshot &s) { return s.phase == RecordPhase::Recording; }));
	recorder.on_disconnected(cordmux::ws::DisconnectReason::Unknown);
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	recorder.on_connected();
	{
		const RecordingSnapshot now = recorder.snapshot();
		REQUIRE(now.gaps.size() == 1);
		CHECK(now.gaps.front().start_wall_us > 0);
		CHECK(now.gaps.front().duration_us >= 20000);
	}
	// 切れたままセッションが終わったら、終わりまでを 1 件にする。
	recorder.on_disconnected(cordmux::ws::DisconnectReason::Unknown);
	recorder.set_enabled(false);
	CHECK(recorder.snapshot().gaps.size() == 2);
	// 次のセッションを始めたら空にする。
	recorder.on_connected();
	recorder.set_enabled(true);
	REQUIRE(wait_until(recorder, [](const RecordingSnapshot &s) { return s.phase == RecordPhase::Recording; }));
	CHECK(recorder.snapshot().gaps.empty());
	recorder.stop(2000);
}

TEST_CASE("録画 I/O: 作成待ちのセッションを再開すると、中間ファイルのフォルダーの録画ログに録画ファイルの作成の行が足され、録画ファイルの名前は現在の形になる")
{
	const TempDir current("current_base");
	// 作成待ちの回 (帳簿は中間ファイルのフォルダーの中)。
	const std::string new_session = join_path(current.path(), "2000-01-01 12-00-00");
	const std::string new_ledger = join_path(join_path(new_session, kMaterialsDirName), kLedgerFileName);
	write_text(new_ledger, pending_ledger_text("2000-01-01 12-00-00", "hana"));

	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	{
		Recorder recorder(settings, make_real_record_platform_ops(log), current.path());
		CHECK(recorder.snapshot().pending_session_count == 1);
		recorder.resume_pending();
		recorder.stop(10000);
	}

	// 帳簿は同じ場所に足され、名前は既定の書式の形。
	const LedgerDocument new_doc = read_ledger_at(new_ledger);
	REQUIRE_FALSE(new_doc.finalizes.empty());
	CHECK(new_doc.finalizes.front().output_file == "hana Voice 2000-01-01 12-00-00.mov");
	// 一時ファイル (生 PCM) は中間ファイルのフォルダーで作られて消えている。
	std::error_code ec;
	CHECK_FALSE(std::filesystem::exists(
		std::filesystem::u8path(join_path(join_path(new_session, kMaterialsDirName), "audio_u1_voice.pcm")), ec));
}

namespace {

// 書き込むスレッドが終わるまで待つ (上限 5 秒。writer_running はどのスレッドから呼んでもよい)。
bool wait_writer_folded(Recorder &recorder)
{
	for (int i = 0; i < 500; ++i) {
		if (!recorder.writer_running())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return !recorder.writer_running();
}

} // namespace

TEST_CASE("Starting の間に止めると、セッションを開き終えても Recording にならず、書き込むスレッドが終わったら Idle になる")
{
	// 書き込むスレッドがセッションのフォルダーと録画ログを開いている最中に、ドックの機能を OFF にした状態
	// (DockPoll::set_feature(false) から set_recording(false))。保存先が遅い場所だと、開くのに時間がかかる。
	const TempDir dir("off_while_starting");
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	HoldableOps holdable;
	holdable.hold = true;
	Recorder recorder(settings, holdable.build(log), dir.path());
	recorder.on_connected();
	recorder.on_configure(voice_formats(), {{"u1", "taro"}});

	recorder.set_enabled(true);
	// 書き込むスレッドは録画ログの最初の行で止まっている (段階はまだ Starting)。
	REQUIRE(holdable.wait_blocked(1));
	CHECK(recorder.snapshot().phase == RecordPhase::Starting);
	recorder.request_disable();
	CHECK(recorder.snapshot().phase == RecordPhase::Stopping);

	holdable.hold = false;
	REQUIRE(wait_settled(recorder));
	{
		// 止める処理が済んだら Idle になる (画面が録画中と表示し続けない)。
		const RecordingSnapshot now = recorder.snapshot();
		CHECK(now.phase == RecordPhase::Idle);
		CHECK_FALSE(now.enabled);
	}
	CHECK(wait_writer_folded(recorder));

	// もう一度始められ、データが書かれる。
	recorder.set_enabled(true);
	REQUIRE(wait_until(recorder, [](const RecordingSnapshot &s) { return s.phase == RecordPhase::Recording; }));
	const std::int64_t before = recorder.snapshot().frames_written;
	feed_voice(recorder, 20);
	CHECK(wait_frames_written(recorder, before));
	recorder.set_enabled(false);
	recorder.stop(0);
}

TEST_CASE("止める処理の途中で ON にしても UI のスレッドは書き込むスレッドを待たず、次のセッションにはデータが書かれる")
{
	const TempDir dir("on_while_folding");
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	HoldableOps holdable;
	holdable.hold = true;
	Recorder recorder(settings, holdable.build(log), dir.path());
	recorder.on_connected();
	recorder.on_configure(voice_formats(), {{"u1", "taro"}});

	recorder.set_enabled(true);
	REQUIRE(holdable.wait_blocked(1)); // セッションの最初の行で止まっている
	recorder.request_disable();
	// セッションの最初の行だけ通す。書き込むスレッドはセッションを開き終え、続く OFF (セッションの最後の行) で
	// 止まる。
	holdable.let_through = 1;
	REQUIRE(holdable.wait_blocked(2));

	// 止める処理のスレッドは、まだ書き込むスレッドの join を待っている。この間に ON にする。
	std::thread release([&holdable]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
		holdable.hold = false;
	});
	const auto started = std::chrono::steady_clock::now();
	recorder.set_enabled(true);
	const auto took = std::chrono::steady_clock::now() - started;
	release.join();
	// UI のスレッドで、書き込むスレッドの join を待たない。
	CHECK(took < std::chrono::milliseconds(500));

	REQUIRE(wait_settled(recorder));
	RecordingSnapshot now = recorder.snapshot();
	if (now.phase == RecordPhase::Idle) {
		// 止める処理の途中の ON は受け付けない。止め終わってから ON にし直す。
		CHECK_FALSE(now.enabled);
		REQUIRE(wait_writer_folded(recorder));
		recorder.set_enabled(true);
		REQUIRE(wait_until(recorder,
				   [](const RecordingSnapshot &s) { return s.phase == RecordPhase::Recording; }));
		now = recorder.snapshot();
	}
	// Recording なら ON/OFF のフラグも true で、データが実際に書かれる (何も録画されないセッションを作らない)。
	CHECK(now.phase == RecordPhase::Recording);
	CHECK(now.enabled);
	feed_voice(recorder, 20);
	CHECK(wait_frames_written(recorder, now.frames_written));
	recorder.set_enabled(false);
	recorder.stop(0);
}

TEST_CASE("始められなかったセッションを request_disable で片付けても StartFailed は残り、保存先が直れば次の ON で始められる")
{
	const TempDir dir("start_failed_product_path");
	cordmux::config::Settings settings;
	std::atomic<bool> fail{true};
	RecordPlatformOps ops = make_real_record_platform_ops(std::make_shared<std::string>());
	auto real_make = ops.make_directories;
	ops.make_directories = [&fail, real_make](const std::string &path) {
		return fail.load() ? false : real_make(path);
	};
	Recorder recorder(settings, std::move(ops), dir.path());

	recorder.set_enabled(true);
	REQUIRE(wait_until(recorder,
			   [](const RecordingSnapshot &s) { return s.phase == RecordPhase::StartFailed; }));

	// Pipeline::poll_recording_self_stop と同じ後始末 (待たない request_disable)。
	const auto started = std::chrono::steady_clock::now();
	recorder.request_disable();
	CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(500));
	CHECK(wait_writer_folded(recorder));
	{
		const RecordingSnapshot now = recorder.snapshot();
		CHECK(now.phase == RecordPhase::StartFailed); // 書き込むスレッドを止めても、StartFailed は残る
		CHECK_FALSE(now.enabled);
		CHECK(now.unwritable_dir == dir.path());
	}

	// 書けるようになったら (利用者が保存先を修正した後)、次の ON で始められる。
	fail = false;
	recorder.set_enabled(true);
	REQUIRE(wait_until(recorder, [](const RecordingSnapshot &s) { return s.phase == RecordPhase::Recording; }));
	{
		const RecordingSnapshot now = recorder.snapshot();
		CHECK(now.enabled);
		REQUIRE(now.save_dir_writable.has_value());
		CHECK(*now.save_dir_writable);
		CHECK(now.unwritable_dir.empty());
	}
	recorder.set_enabled(false);
	recorder.stop(0);
}

TEST_CASE("解決できなかった保存先 (空か相対パス) ではセッションを始めず、構築のときの場所にも移さない")
{
	const TempDir dir("unresolved_base");
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	// 相対パスは作業フォルダーの下に解決されるので、もし作られても、テストの後に必ず削除する。
	const std::string relative = "cordmux_record_io_relative_base_r4";
	struct RemoveRelative {
		std::string path;
		~RemoveRelative()
		{
			std::error_code ec;
			std::filesystem::remove_all(std::filesystem::u8path(path), ec);
		}
	} cleanup{relative};

	Recorder recorder(settings, make_real_record_platform_ops(log), dir.path());
	std::error_code ec;

	// Pipeline は、解決できなかったセッションの保存先を空で渡す (resolve_record_dir_choice が None のとき)。
	RecordSessionValues values;
	values.base_dir.clear();
	recorder.set_session_values(values);
	recorder.set_enabled(true);
	REQUIRE(wait_until(recorder,
			   [](const RecordingSnapshot &s) { return s.phase == RecordPhase::StartFailed; }));
	{
		const RecordingSnapshot now = recorder.snapshot();
		CHECK_FALSE(now.enabled);
		REQUIRE(now.save_dir_writable.has_value());
		CHECK_FALSE(*now.save_dir_writable);
		CHECK(now.unwritable_dir.empty()); // 保存先を解決できていないので、場所は示せない
	}
	CHECK(std::filesystem::is_empty(std::filesystem::u8path(dir.path()), ec)); // 保存先そのものの下には何も作らない
	recorder.request_disable();
	REQUIRE(wait_writer_folded(recorder));

	// 相対パス (settings.json を手で編集したとき) も同じ。作業フォルダーの下に作らない。
	values.base_dir = relative;
	recorder.set_session_values(values);
	recorder.set_enabled(true);
	REQUIRE(wait_until(recorder,
			   [](const RecordingSnapshot &s) { return s.phase == RecordPhase::StartFailed; }));
	CHECK_FALSE(recorder.snapshot().enabled);
	CHECK_FALSE(std::filesystem::exists(std::filesystem::u8path(relative), ec));
	CHECK(std::filesystem::is_empty(std::filesystem::u8path(dir.path()), ec)); // 保存先そのものの下には何も作らない
	recorder.request_disable();
	recorder.stop(0);
}

namespace {

// 録画ファイルの作成の状態を指定した録画ログを 1 つ作る。`with_output` が偽なら人とセグメントの行を持たない (録画ファイルを作成する
// 録画ファイルが 0 個なので、再開すると FFmpeg を起動せずに Done になる)。
std::string ledger_text_with_state(const std::string &session_id, cordmux::record::FinalizeState state,
				   bool with_output)
{
	std::string text = pending_ledger_text(session_id, "hana");
	if (!with_output)
		text = text.substr(0, text.find('\n') + 1); // 先頭の session の行だけ
	LedgerDocument doc = parse_ledger(text);
	doc.session.finalize_state = state;
	if (!with_output)
		return cordmux::record::to_jsonl(doc.session) + "\n";
	return cordmux::record::to_jsonl(doc.session) + "\n" + text.substr(text.find('\n') + 1);
}

// 現在の形のセッションを 1 つ作り、録画ログのパスを返す。
std::string write_session_ledger(const std::string &base, const std::string &session_id, const std::string &text)
{
	const std::string session = join_path(base, session_id);
	const std::string ledger = join_path(join_path(session, kMaterialsDirName), kLedgerFileName);
	write_text(ledger, text);
	return ledger;
}

// 録画ログに追加された録画ファイルの作成の行の数 (作成を試した回数)。parse_ledger は同じ録画ファイルの行を後の行で
// 置き換えるので、本文の行を数える。
int finalize_line_count(const std::string &ledger)
{
	std::string text;
	REQUIRE(read_whole_file(ledger, &text));
	int count = 0;
	for (std::size_t at = text.find("\"type\":\"finalize\""); at != std::string::npos;
	     at = text.find("\"type\":\"finalize\"", at + 1))
		++count;
	return count;
}

// 録画ファイルの作成が空き容量を確認する所 (record_finalize.cpp の、書き始める前の確認) で止められる、実際の
// RecordPlatformOps。作成中の状態を、時間に頼らずに作るため。
struct FinalizeHoldOps {
	std::atomic<bool> hold{true};
	std::atomic<int> entered{0};
	RecordPlatformOps build(std::shared_ptr<std::string> log)
	{
		RecordPlatformOps ops = make_real_record_platform_ops(log);
		auto real_free = ops.free_space_bytes;
		ops.free_space_bytes = [this, real_free](const std::string &path) {
			++entered;
			while (hold.load())
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			return real_free(path);
		};
		return ops;
	}
	bool wait_entered(int count) const
	{
		for (int i = 0; i < 500; ++i) {
			if (entered.load() >= count)
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return entered.load() >= count;
	}
};

} // namespace

TEST_CASE("録画ファイルの作成に失敗したセッション (Failed) と作成の途中で止まったセッション (Running) は作成待ちに数え、Done は数えない")
{
	using cordmux::record::FinalizeState;
	const TempDir dir("finalize_failed_count");
	write_session_ledger(dir.path(), "2000-01-01 10-00-00", ledger_text_with_state("a", FinalizeState::Failed, true));
	write_session_ledger(dir.path(), "2000-01-01 11-00-00", ledger_text_with_state("b", FinalizeState::Pending, true));
	write_session_ledger(dir.path(), "2000-01-01 12-00-00", ledger_text_with_state("c", FinalizeState::Done, true));
	write_session_ledger(dir.path(), "2000-01-01 13-00-00", ledger_text_with_state("d", FinalizeState::Running, true));

	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	Recorder recorder(settings, make_real_record_platform_ops(log), dir.path());
	const RecordingSnapshot now = recorder.snapshot();
	CHECK(now.pending_present);
	// a (Failed)、b (Pending)、d (Running で終わり、だれもロックファイルを持たず、閉じたセグメントがある中断した
	// セッション) を数える。c (Done) は数えない。
	CHECK(now.pending_session_count == 3);
	// 失敗が残っているセッションのフォルダー (失敗のメッセージを、録画ファイルの作成が成功するまで残す)。
	CHECK(now.failed_session_dir == join_path(dir.path(), "2000-01-01 10-00-00"));
	recorder.stop(0);
}

TEST_CASE("failed_session_dir は、失敗したセッションのうち最も新しいセッションのフォルダーを示し、なければ空になる")
{
	using cordmux::record::FinalizeState;
	const TempDir dir("finalize_failed_newest");
	write_session_ledger(dir.path(), "2000-01-01 09-00-00", ledger_text_with_state("a", FinalizeState::Failed, true));
	write_session_ledger(dir.path(), "2000-01-02 08-00-00", ledger_text_with_state("b", FinalizeState::Failed, true));
	write_session_ledger(dir.path(), "2000-01-03 07-00-00", ledger_text_with_state("c", FinalizeState::Pending, true));

	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	{
		Recorder recorder(settings, make_real_record_platform_ops(log), dir.path());
		const RecordingSnapshot now = recorder.snapshot();
		CHECK(now.pending_session_count == 3);
		// Pending のセッションの方が新しくても、入れるのは Failed のセッションだけ。
		CHECK(now.failed_session_dir ==
		      join_path(dir.path(), "2000-01-02 08-00-00"));
		recorder.stop(0);
	}

	const TempDir none("finalize_failed_none");
	write_session_ledger(none.path(), "2000-01-01 09-00-00", ledger_text_with_state("a", FinalizeState::Pending, true));
	Recorder pending_only(settings, make_real_record_platform_ops(log), none.path());
	CHECK(pending_only.snapshot().pending_present);
	CHECK(pending_only.snapshot().failed_session_dir.empty());
	pending_only.stop(0);
}

TEST_CASE("失敗したセッションを再開して録画ファイルを作成し終えたら、作成待ちの集計から外れる")
{
	using cordmux::record::FinalizeState;
	const TempDir dir("finalize_failed_done");
	const std::string ledger =
		write_session_ledger(dir.path(), "2000-01-01 10-00-00", ledger_text_with_state("a", FinalizeState::Failed, false));

	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	{
		Recorder recorder(settings, make_real_record_platform_ops(log), dir.path());
		REQUIRE(recorder.snapshot().pending_session_count == 1);
		recorder.resume_pending();
		REQUIRE(wait_until(recorder, [](const RecordingSnapshot &s) {
			return s.finalize.state == FinalizeState::Done && !s.pending_present;
		}));
		CHECK(recorder.snapshot().pending_session_count == 0);
		CHECK(recorder.snapshot().failed_session_dir.empty()); // 完成すれば失敗のメッセージは消える
		recorder.stop(10000);
	}
	// 録画ログの最後の session の行が Done になる (後の行が勝つ)。次の起動でも数えない。
	CHECK(read_ledger_at(ledger).session.finalize_state == FinalizeState::Done);
	Recorder again(settings, make_real_record_platform_ops(log), dir.path());
	CHECK_FALSE(again.snapshot().pending_present);
	again.stop(0);
}

TEST_CASE("resume_pending は呼ばれるたびに試し、また失敗したセッションは集計に戻り、作成中は数えず、続けて呼んでも 2 度積まない")
{
	using cordmux::record::FinalizeState;
	const TempDir dir("finalize_failed_retry");
	// 声のセグメントのファイルを置かないセッション。声の録画ファイルの作成が失敗し、セッションは Failed になる。
	const std::string ledger =
		write_session_ledger(dir.path(), "2000-01-01 10-00-00", ledger_text_with_state("a", FinalizeState::Failed, true));

	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	FinalizeHoldOps hold;
	{
		Recorder recorder(settings, hold.build(log), dir.path());
		REQUIRE(recorder.snapshot().pending_session_count == 1);

		// 1 回目: 録画ファイルの作成が空き容量を確認する所で止まっている間は、作成中である。
		recorder.resume_pending();
		REQUIRE(hold.wait_entered(1));
		{
			const RecordingSnapshot now = recorder.snapshot();
			CHECK(now.finalize.state == FinalizeState::Running);
			CHECK_FALSE(now.pending_present); // 作成中のセッションは数えない (再開のボタンを表示しない)
			CHECK(now.pending_session_count == 0);
			CHECK(now.failed_session_dir.empty()); // 再開して作成している間は、失敗のメッセージを表示しない
		}
		// 作成中に続けて呼んでも積まない (下で、録画ログの録画ファイルの作成の行が 1 回分だけであることを確認する)。
		recorder.resume_pending();
		hold.hold = false;
		REQUIRE(wait_until(recorder, [](const RecordingSnapshot &s) {
			return s.finalize.state == FinalizeState::Failed && s.pending_present;
		}));
		CHECK(recorder.snapshot().pending_session_count == 1);
		CHECK_FALSE(recorder.snapshot().failed_session_dir.empty()); // また失敗すれば、失敗のメッセージが戻る
		// 2 度積んでいれば、2 回目の録画ファイルの作成が終わるまでに作成の行が 2 つになる。待ってから数える。
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		{
			CHECK(finalize_line_count(ledger) == 1);
			CHECK(read_ledger_at(ledger).session.finalize_state == FinalizeState::Failed);
		}

		// 2 回目: 同じ失敗が続くセッションも、呼べばもう一度試す (試す回数に上限を設けない)。
		recorder.resume_pending();
		REQUIRE(wait_until(recorder, [](const RecordingSnapshot &s) {
			return s.finalize.state == FinalizeState::Failed && s.pending_present;
		}));
		recorder.stop(10000);
	}
	CHECK(finalize_line_count(ledger) == 2);
	CHECK(read_ledger_at(ledger).session.finalize_state == FinalizeState::Failed);
	// 録画ログは残っていて、次の起動でも作成待ちに数える。
	Recorder again(settings, make_real_record_platform_ops(log), dir.path());
	CHECK(again.snapshot().pending_session_count == 1);
	// 起動し直した後も、失敗したセッションのフォルダーがスナップショットにある (メッセージのレベル 1 で、失敗のメッセージが
	// 残る)。
	CHECK(again.snapshot().failed_session_dir ==
	      join_path(dir.path(), "2000-01-01 10-00-00"));
	again.stop(0);
}

TEST_CASE("前のセッションの録画ファイルの作成の最中に ON にしても新しいセッションを始められ、作成の進み具合は消えない")
{
	using cordmux::record::FinalizeState;
	const TempDir dir("on_while_finalizing");
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	FinalizeHoldOps hold;
	hold.hold = false;
	{
		Recorder recorder(settings, hold.build(log), dir.path());
		recorder.on_connected();
		recorder.on_configure(voice_formats(), {{"u1", "taro"}});
		recorder.set_enabled(true);
		REQUIRE(wait_until(recorder, [](const RecordingSnapshot &s) { return s.phase == RecordPhase::Recording; }));
		feed_voice(recorder, 10);
		REQUIRE(wait_frames_written(recorder, 0));

		// OFF にした後、録画ファイルの作成が空き容量を確認する所で止まっている間に、次の ON を押す。
		hold.hold = true;
		const int entered_before = hold.entered.load();
		recorder.request_disable();
		REQUIRE(hold.wait_entered(entered_before + 1));
		REQUIRE(wait_writer_folded(recorder));
		CHECK(recorder.snapshot().finalize.state == FinalizeState::Running);

		recorder.set_enabled(true);
		REQUIRE(wait_until(recorder, [](const RecordingSnapshot &s) { return s.phase == RecordPhase::Recording; }));
		const RecordingSnapshot now = recorder.snapshot();
		CHECK(now.enabled);
		// 前のセッションの録画ファイルの作成はまだ続いているので、作成の行の元になる状態を新しいセッションの始まりで消さない。
		CHECK(now.finalize.state == FinalizeState::Running);

		hold.hold = false;
		recorder.set_enabled(false);
		recorder.stop(10000);
	}
}

TEST_CASE("OBS を終了するとき、録画ファイルの作成を待つ上限を過ぎても終わらない作成は打ち切り、録画ログを残して次の起動の作成待ちにする")
{
	const TempDir dir("exit_wait");
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	FinalizeHoldOps hold;
	hold.hold = false;
	{
		Recorder recorder(settings, hold.build(log), dir.path());
		recorder.on_connected();
		recorder.on_configure(voice_formats(), {{"u1", "taro"}});
		recorder.set_enabled(true);
		REQUIRE(wait_until(recorder, [](const RecordingSnapshot &s) { return s.phase == RecordPhase::Recording; }));
		feed_voice(recorder, 10);
		REQUIRE(wait_frames_written(recorder, 0));

		hold.hold = true;
		const int entered_before = hold.entered.load();
		recorder.request_disable();
		REQUIRE(hold.wait_entered(entered_before + 1));
		REQUIRE(wait_writer_folded(recorder));

		// 録画ファイルの作成が止まったまま、待つ上限 (300 ms) まで待ってから打ち切る。止まっている処理は、後から放して終わらせる。
		std::thread release([&hold]() {
			std::this_thread::sleep_for(std::chrono::milliseconds(1000));
			hold.hold = false;
		});
		const auto started = std::chrono::steady_clock::now();
		recorder.stop(300);
		const auto took = std::chrono::steady_clock::now() - started;
		release.join();
		CHECK(took >= std::chrono::milliseconds(300));
	}
	// 録画ログとセグメントのファイルは残り、次の起動で作成待ちに数える。
	Recorder again(settings, make_real_record_platform_ops(log), dir.path());
	CHECK(again.snapshot().pending_session_count == 1);
	CHECK(again.snapshot().pending_present);
	again.stop(0);
}

// ---------------------------------------------------------------------------
// セッションのフォルダー名の衝突、前の保存先の作成待ち、削除を許さない保存先のロックファイル
// ---------------------------------------------------------------------------

TEST_CASE("セッションのフォルダーの名前が既にあれば次の名前にし、既にある録画ログは 1 バイトも変えない")
{
	using cordmux::record::FinalizeState;
	const TempDir dir("session_name_taken");
	// 同じ秒にもう 1 つの OBS が始めたセッション (時計が戻った後の同じ秒も、同じ名前になる)。
	const std::int64_t wall = 946684800000000;
	const std::string name = cordmux::record::session_dir_name(wall, 0);
	const std::string taken = write_session_ledger(dir.path(), name, ledger_text_with_state("x", FinalizeState::Done, true));
	std::string before;
	REQUIRE(read_whole_file(taken, &before));
	REQUIRE_FALSE(before.empty());

	auto log = std::make_shared<std::string>();
	RecordPlatformOps ops = make_real_record_platform_ops(log);
	ops.now_wall_us = [wall]() { return wall; };
	ops.utc_offset_sec = []() { return 0; };
	cordmux::config::Settings settings;
	{
		Recorder recorder(settings, ops, dir.path());
		recorder.on_connected();
		recorder.on_configure(voice_formats(), {{"u1", "taro"}});
		recorder.set_enabled(true);
		REQUIRE(wait_until(recorder,
				   [](const RecordingSnapshot &s) { return s.phase == RecordPhase::Recording; }));
		feed_voice(recorder, 50);
		recorder.set_enabled(false);
		recorder.stop(5000);
	}

	// 前からあるセッションの録画ログは 1 バイトも変わらず、新しいセッションは「 (2)」を付けた名前で、別の
	// フォルダーにある。
	std::string after;
	REQUIRE(read_whole_file(taken, &after));
	CHECK(after == before);
	const std::string root = dir.path();
	const std::vector<std::string> names = list_subdirectories(root);
	REQUIRE(names.size() == 2);
	CHECK(names[0] == name);
	CHECK(names[1] == name + " (2)");
	const LedgerDocument doc =
		read_ledger_at(join_path(join_path(join_path(root, name + " (2)"), kMaterialsDirName), kLedgerFileName));
	CHECK(doc.session.session_id == name + " (2)");
	CHECK_FALSE(doc.segments.empty());
}

TEST_CASE("ensure_session_open はセッションのフォルダーを make_new_directory で作って既にある名前を飛ばし、ほかの理由で作れなければ始めない")
{
	using cordmux::record::ensure_session_open;
	using cordmux::record::SessionState;
	const std::int64_t wall = 946684800000000;
	const std::string name = cordmux::record::session_dir_name(wall, 0);
	std::vector<std::string> tried;
	int exists_until = 2; // 最初の 2 つの名前は既にある
	bool other_failure = false;
	RecordPlatformOps ops;
	ops.make_directories = [](const std::string &) { return true; };
	ops.make_new_directory = [&](const std::string &path, bool *already_exists) {
		tried.push_back(path);
		if (other_failure) {
			*already_exists = false;
			return false;
		}
		if (static_cast<int>(tried.size()) <= exists_until) {
			*already_exists = true;
			return false;
		}
		return true;
	};
	ops.open_file = [](const std::string &) -> FileHandle { return 1; };
	ops.now_wall_us = [wall]() { return wall; };
	ops.now_steady_us = []() { return std::int64_t{5}; };

	SessionState state;
	REQUIRE(ensure_session_open(state, ops, "C:/rec", true, 0));
	CHECK(state.session_id == name + " (3)");
	REQUIRE(tried.size() == 3);
	CHECK(tried[0] == "C:/rec/" + name);
	CHECK(tried[1] == "C:/rec/" + name + " (2)");
	CHECK(tried[2] == "C:/rec/" + name + " (3)");

	// 同じ起動の同じ秒の次のセッションは、前に使った番号の次から試す。
	cordmux::record::close_session(state, ops);
	tried.clear();
	exists_until = 0;
	REQUIRE(ensure_session_open(state, ops, "C:/rec", true, 0));
	CHECK(state.session_id == name + " (4)");

	// 既にある以外の理由で作れなければ、名前を変えずに始めない (書けない保存先なので StartFailed)。
	cordmux::record::close_session(state, ops);
	tried.clear();
	other_failure = true;
	SessionState fresh;
	CHECK_FALSE(ensure_session_open(fresh, ops, "C:/rec", true, 0));
	CHECK(tried.size() == 1);
	CHECK(fresh.failed_root == "C:/rec");

	// 既にあると答え続ける壊れた関数でも、試し続けない (kMaxSessionNameTries 回でやめる)。
	other_failure = false;
	exists_until = 1000;
	tried.clear();
	SessionState stuck;
	CHECK_FALSE(ensure_session_open(stuck, ops, "C:/rec", true, 0));
	CHECK(tried.size() == static_cast<std::size_t>(cordmux::record::kMaxSessionNameTries));
}

namespace {

// 一覧のファイル (製品と同じ、record_search_dirs.h の pending_dirs_file_store) と、書いた回数。
struct CountingStore {
	explicit CountingStore(std::string file) : path(std::move(file)) {}
	std::string path;
	std::shared_ptr<std::atomic<int>> writes = std::make_shared<std::atomic<int>>(0);

	cordmux::record::PendingDirsStore store() const
	{
		const cordmux::record::PendingDirsStore inner = cordmux::record::pending_dirs_file_store(path);
		const std::shared_ptr<std::atomic<int>> count = writes;
		cordmux::record::PendingDirsStore out;
		out.load = inner.load;
		out.save = [inner, count](const std::vector<std::string> &dirs) {
			++*count;
			return inner.save(dirs);
		};
		return out;
	}
	std::vector<std::string> read() const
	{
		std::string text;
		read_whole_file(path, &text);
		return cordmux::record::parse_pending_dirs(text);
	}
	void write(const std::vector<std::string> &dirs) const { write_text(path, cordmux::record::format_pending_dirs(dirs)); }
	bool present() const
	{
		std::error_code ec;
		return std::filesystem::exists(std::filesystem::u8path(path), ec);
	}
};

// アクセスできるかを問い合わせた保存先 (問い合わせは、保存先ごとに別のスレッドで行う)。
struct AskLog {
	std::mutex mu;
	std::vector<std::string> asked;
	void add(const std::string &dir)
	{
		std::lock_guard<std::mutex> lock(mu);
		asked.push_back(dir);
	}
	std::vector<std::string> sorted()
	{
		std::lock_guard<std::mutex> lock(mu);
		std::vector<std::string> out = asked;
		std::sort(out.begin(), out.end());
		return out;
	}
};

// 条件が真になるまで待つ (上限 5 秒。Recorder のスナップショット以外の条件に使う)。
template <typename Pred> bool wait_for(Pred pred)
{
	for (int i = 0; i < 500; ++i) {
		if (pred())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return pred();
}

std::vector<std::string> sorted_of(std::vector<std::string> dirs)
{
	std::sort(dirs.begin(), dirs.end());
	return dirs;
}

} // namespace

TEST_CASE("前の保存先に残った失敗したセッションは保存先を変えて起動し直しても数えて作成でき、済めば一覧から外れ、アクセスできない保存先は一覧に残る")
{
	using cordmux::record::FinalizeState;
	const TempDir old_base("pending_dirs_old");
	const TempDir new_base("pending_dirs_new");
	const TempDir config("pending_dirs_config");
	const CountingStore store(join_path(config.path(), cordmux::record::kPendingDirsFileName));
	const std::string unreachable = "Z:/cordmux-test-unplugged-drive";
	const std::string failed_session = join_path(old_base.path(), "2000-01-01 10-00-00");
	write_session_ledger(old_base.path(), "2000-01-01 10-00-00", ledger_text_with_state("a", FinalizeState::Failed, false));
	store.write({unreachable});
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;

	// 1 回目の起動: 保存先は前の場所。失敗したセッションを数え、その保存先を一覧に追加する (アクセスできない一覧の
	// 保存先はそのまま)。
	{
		const auto asked = std::make_shared<AskLog>();
		Recorder recorder(settings, make_real_record_platform_ops(log), old_base.path());
		recorder.remember_pending_dirs(store.store(), [asked](const std::string &dir) {
			asked->add(dir);
			return false;
		});
		REQUIRE(wait_for([&asked]() { return asked->sorted().size() == 1; }));
		CHECK(asked->sorted() == std::vector<std::string>{unreachable});
		CHECK(recorder.snapshot().pending_session_count == 1);
		recorder.stop(0);
	}
	CHECK(*store.writes == 1);
	CHECK(store.read() == std::vector<std::string>{unreachable, old_base.path()});

	// 2 回目の起動: 保存先を変えた。一覧がなければ、前の保存先の失敗したセッションは集計から消える。
	{
		const auto asked = std::make_shared<AskLog>();
		Recorder recorder(settings, make_real_record_platform_ops(log), new_base.path());
		CHECK(recorder.snapshot().pending_session_count == 0);
		recorder.remember_pending_dirs(store.store(), [asked, unreachable](const std::string &dir) {
			asked->add(dir);
			return dir != unreachable;
		});
		REQUIRE(wait_until(recorder, [](const RecordingSnapshot &s) { return s.pending_session_count == 1; }));
		REQUIRE(wait_for([&asked]() { return asked->sorted().size() == 2; }));
		CHECK(asked->sorted() == sorted_of({unreachable, old_base.path()}));
		// 失敗のメッセージ (メッセージのレベル 1) は、録画ファイルの作成が成功するまで残る。一覧は変わっていないので書かない。
		CHECK(recorder.snapshot().failed_session_dir == failed_session);
		CHECK(*store.writes == 1);

		recorder.resume_pending();
		REQUIRE(wait_until(recorder, [](const RecordingSnapshot &s) {
			return s.finalize.state == FinalizeState::Done && !s.pending_present;
		}));
		recorder.stop(10000);
	}
	// 録画ファイルの作成が済んだので前の保存先は一覧から外れ、アクセスできない保存先は残る (アクセスできないうちは、済んだか
	// 分からない)。
	CHECK(read_ledger_at(join_path(join_path(failed_session, kMaterialsDirName), kLedgerFileName)).session.finalize_state ==
	      FinalizeState::Done);
	CHECK(store.read() == std::vector<std::string>{unreachable});

	// 3 回目の起動: アクセスできない保存先は探さずに残す。録画ファイルの作成の済んでいないセッションがない現在の保存先は、一覧に
	// 追加しない (書かない)。
	{
		const int writes_before = *store.writes;
		Recorder recorder(settings, make_real_record_platform_ops(log), new_base.path());
		recorder.remember_pending_dirs(store.store(), [](const std::string &) { return false; });
		CHECK_FALSE(recorder.snapshot().pending_present);
		recorder.stop(0);
		CHECK(*store.writes == writes_before);
	}
	CHECK(store.read() == std::vector<std::string>{unreachable});

	// 4 回目の起動: ほかの OBS が録画中のセッション (ロックファイルを持っている) は数えないが、その保存先には
	// 録画ファイルの作成の済んでいないセッションがあるので一覧に追加する (その OBS がクラッシュすれば、中断した
	// セッションがそこに残る)。
	{
		const std::string busy = write_session_ledger(old_base.path(), "2000-01-02 10-00-00",
							      ledger_text_with_state("b", FinalizeState::Running, true));
		const std::string lock =
			join_path(std::filesystem::u8path(busy).parent_path().u8string(), cordmux::record::kSessionLockFileName);
		bool held_elsewhere = false;
		const FileHandle other = cordmux::platform::hold_lock_file(lock, &held_elsewhere);
		REQUIRE(other != 0);
		// 保存先を前の場所に戻した起動 (現在の保存先として探す)。
		Recorder recorder(settings, make_real_record_platform_ops(log), old_base.path());
		recorder.remember_pending_dirs(store.store(), [](const std::string &) { return false; });
		CHECK_FALSE(recorder.snapshot().pending_present);
		recorder.stop(0);
		cordmux::platform::close(other);
	}
	CHECK(store.read() == std::vector<std::string>{unreachable, old_base.path()});
}

TEST_CASE("実際の ffmpeg で録画ファイルを作成すると、録画の途中で保存先を変えて中断したセッションも、一覧に追加しておいた保存先から数えて作成できる" *
	  doctest::skip(!have_real_ffmpeg()))
{
	using cordmux::record::FinalizeState;
	const TempDir first_base("dirs461_a");
	const TempDir second_base("dirs461_b");
	const TempDir config("dirs461_config");
	const CountingStore store(join_path(config.path(), cordmux::record::kPendingDirsFileName));
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;

	// クラッシュする OBS を再現する。クラッシュした後は録画ログにも一覧にも 1 行も書かず、子プロセスも起動しない。
	// ハンドルは閉じる (クラッシュしたプロセスのハンドルは OS が閉じ、ロックファイルも消える)。Recorder を止めずに
	// 残すとハンドルを持ち続け、次の Recorder の録画ファイルの作成が録画ログに追記できないので、止める処理は通し、書き込む
	// 関数だけを止める。
	const auto crashed = std::make_shared<std::atomic<bool>>(false);
	RecordPlatformOps ops = make_real_record_platform_ops(log);
	const auto append = ops.append;
	ops.append = [append, crashed](FileHandle file, const void *data, std::size_t len) {
		return !crashed->load() && append(file, data, len);
	};
	const auto start_child = ops.start_child;
	ops.start_child = [start_child, crashed](const std::string &program, const std::vector<std::string> &args) {
		return crashed->load() ? cordmux::record::kInvalidChild : start_child(program, args);
	};
	cordmux::record::PendingDirsStore crashable = store.store();
	const auto save = crashable.save;
	crashable.save = [save, crashed](const std::vector<std::string> &dirs) { return !crashed->load() && save(dirs); };

	std::string session_dir;
	{
		Recorder falling(settings, ops, first_base.path());
		falling.remember_pending_dirs(crashable, [](const std::string &) { return true; });
		// 録画ファイルの作成の済んでいないセッションがないときは、一覧のファイルを作らない。
		CHECK_FALSE(store.present());

		RecordSessionValues values;
		values.base_dir = first_base.path();
		falling.set_session_values(values);
		falling.on_connected();
		falling.on_configure(voice_formats(), {{"u1", "hana"}});
		falling.set_enabled(true);
		REQUIRE(wait_until(falling, [](const RecordingSnapshot &s) { return s.phase == RecordPhase::Recording; }));
		feed_voice(falling, 150);
		REQUIRE(wait_until(falling, [](const RecordingSnapshot &s) { return s.frames_written >= 150; }));
		session_dir = falling.snapshot().session_dir;
		// セッションを始めたときに、その保存先を一覧に追加した (録画中に集計はない)。
		CHECK(store.read() == std::vector<std::string>{first_base.path()});
		CHECK(*store.writes == 1);

		// 録画の途中で保存先を変える (設定ウィンドウでも、OBS の録画の保存先でも。どちらも集計し直さない)。
		values.base_dir = second_base.path();
		falling.set_session_values(values);

		// ここでクラッシュする。
		crashed->store(true);
		falling.stop(0);
	}
	// クラッシュした状態: 録画ログは録画の途中のまま (録画ファイルの作成の行も最後の行もない)。
	const LedgerDocument after_crash =
		read_ledger_at(join_path(join_path(session_dir, kMaterialsDirName), kLedgerFileName));
	REQUIRE(after_crash.session.finalize_state == FinalizeState::NotStarted);
	CHECK(store.read() == std::vector<std::string>{first_base.path()});

	// 起動し直す: 保存先は変えた後の場所。中断したセッションは一覧の保存先から集計に出て、作成できる。
	{
		RecordPlatformOps real = make_real_record_platform_ops(log);
		real.ffmpeg_path = []() { return ffmpeg_exe(); };
		Recorder again(settings, real, second_base.path());
		again.remember_pending_dirs(store.store(), [](const std::string &) { return true; });
		REQUIRE(wait_until(again, [](const RecordingSnapshot &s) { return s.pending_session_count == 1; }));
		again.resume_pending();
		REQUIRE(wait_until(again, [](const RecordingSnapshot &s) {
			return s.finalize.state != FinalizeState::NotStarted && s.finalize.state != FinalizeState::Running;
		}));
		CHECK(again.snapshot().finalize.state == FinalizeState::Done);
		CHECK_FALSE(again.snapshot().pending_present);
		again.stop(10000);
	}
	// 録画ファイルの作成が済んだので一覧から外れる (ファイルは空の一覧で残る)。
	CHECK(store.present());
	CHECK(store.read().empty());
}

TEST_CASE("一覧に応答しない保存先があっても、ほかの保存先を探して数え、起動は問い合わせを待たない")
{
	using cordmux::record::FinalizeState;
	const TempDir now_base("dirs_n2_now");
	const TempDir later_base("dirs_n2_later");
	const TempDir config("dirs_n2_config");
	const CountingStore store(join_path(config.path(), cordmux::record::kPendingDirsFileName));
	const std::string silent = "\\\\cordmux-test-sleeping-nas\\rec";
	write_session_ledger(later_base.path(), "2000-01-03 10-00-00",
			     ledger_text_with_state("c", FinalizeState::Failed, false));
	// 応答しない保存先が一覧の先頭にある。1 つのスレッドで順に問い合わせると、後ろの保存先が、その起動の間
	// 探されない。
	store.write({silent, later_base.path()});

	// 応答しない問い合わせ。テストの終わりに待ちを解く。解かなくても、10 秒でアクセスできないと答える。
	struct Gate {
		std::mutex mu;
		std::condition_variable cv;
		bool open = false;
	};
	const auto gate = std::make_shared<Gate>();
	const auto reachable = [gate, silent](const std::string &dir) {
		if (dir != silent)
			return true;
		std::unique_lock<std::mutex> lock(gate->mu);
		gate->cv.wait_for(lock, std::chrono::seconds(10), [&gate]() { return gate->open; });
		return false;
	};
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	Recorder recorder(settings, make_real_record_platform_ops(log), now_base.path());
	const auto before = std::chrono::steady_clock::now();
	recorder.remember_pending_dirs(store.store(), reachable);
	const auto waited =
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - before).count();
	CHECK(waited < 1000); // 起動は問い合わせを待たない

	REQUIRE(wait_until(recorder, [](const RecordingSnapshot &s) { return s.pending_session_count == 1; }));
	CHECK(recorder.snapshot().failed_session_dir ==
	      join_path(later_base.path(), "2000-01-03 10-00-00"));
	// 応答しない保存先は一覧に残る (済んだか分からない)。一覧は変わっていないので書かない。
	CHECK(store.read() == std::vector<std::string>{silent, later_base.path()});
	CHECK(*store.writes == 0);

	{
		std::lock_guard<std::mutex> lock(gate->mu);
		gate->open = true;
	}
	gate->cv.notify_all();
	recorder.stop(0);
}

TEST_CASE("同じ一覧のファイルを使う 2 つの OBS は、もう片方が追加した保存先を消さない")
{
	using cordmux::record::FinalizeState;
	const TempDir first_base("dirs_n5_a");
	const TempDir second_base("dirs_n5_b");
	const TempDir config("dirs_n5_config");
	const CountingStore store(join_path(config.path(), cordmux::record::kPendingDirsFileName));
	write_session_ledger(first_base.path(), "2000-01-04 10-00-00", ledger_text_with_state("d", FinalizeState::Failed, false));
	write_session_ledger(second_base.path(), "2000-01-04 11-00-00", ledger_text_with_state("e", FinalizeState::Failed, false));
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;

	// 1 つ目の OBS (保存先 A) が A を一覧に追加する。
	Recorder first(settings, make_real_record_platform_ops(log), first_base.path());
	first.remember_pending_dirs(store.store(), [](const std::string &) { return false; });
	CHECK(store.read() == std::vector<std::string>{first_base.path()});
	// 2 つ目の OBS (保存先 B。A にはアクセスできないとする) が B を追加する。
	Recorder second(settings, make_real_record_platform_ops(log), second_base.path());
	second.remember_pending_dirs(store.store(), [](const std::string &) { return false; });
	CHECK(store.read() == std::vector<std::string>{first_base.path(), second_base.path()});

	// 1 つ目の OBS が A のセッションの録画ファイルを作成し終えて A を外す。起動のときに読んだコピー (A だけ) から丸ごと書くと、
	// B を消してしまう。
	first.resume_pending();
	REQUIRE(wait_until(first, [](const RecordingSnapshot &s) {
		return s.finalize.state == FinalizeState::Done && !s.pending_present;
	}));
	// スナップショットの Done は録画ファイルの作成の後の集計より先に見えることがあるので、一覧のファイルが変わるまで待つ。
	CHECK(wait_for([&store, &second_base]() { return store.read() == std::vector<std::string>{second_base.path()}; }));
	CHECK(second.snapshot().pending_session_count == 1);
	second.stop(0);
	first.stop(10000);
	CHECK(store.read() == std::vector<std::string>{second_base.path()});
}

TEST_CASE("一覧のファイルを読み直せないときは書かず、ほかの OBS が追加した保存先を前に読んだコピーで消さない")
{
	using cordmux::record::FinalizeState;
	const TempDir first_base("dirs_n5r_a");
	const TempDir config("dirs_n5r_config");
	const CountingStore store(join_path(config.path(), cordmux::record::kPendingDirsFileName));
	const std::string other = "D:/cordmux-test-other-obs";
	write_session_ledger(first_base.path(), "2000-01-06 10-00-00", ledger_text_with_state("g", FinalizeState::Failed, false));
	const auto fail_load = std::make_shared<std::atomic<bool>>(false);
	cordmux::record::PendingDirsStore flaky = store.store();
	const auto load = flaky.load;
	flaky.load = [load, fail_load]() -> std::optional<std::vector<std::string>> {
		if (fail_load->load())
			return std::nullopt;
		return load();
	};
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	Recorder recorder(settings, make_real_record_platform_ops(log), first_base.path());
	recorder.remember_pending_dirs(flaky, [](const std::string &) { return false; });
	CHECK(store.read() == std::vector<std::string>{first_base.path()});
	// ほかの OBS が保存先を追加した後、こちらの読み直しが失敗する。
	store.write({first_base.path(), other});
	fail_load->store(true);
	recorder.resume_pending();
	REQUIRE(wait_until(recorder, [](const RecordingSnapshot &s) {
		return s.finalize.state == FinalizeState::Done && !s.pending_present;
	}));
	recorder.stop(10000);
	// 録画ファイルの作成が済んで A を外したいが、読めないので書かない (前に読んだコピーから書くと、ほかの OBS の保存先を消す)。
	CHECK(store.read() == sorted_of({first_base.path(), other}));
	CHECK(*store.writes == 1);

	// 読めるようになった次の集計で、A だけを外す。
	fail_load->store(false);
	Recorder again(settings, make_real_record_platform_ops(log), first_base.path());
	again.remember_pending_dirs(flaky, [](const std::string &) { return false; });
	again.stop(0);
	CHECK(store.read() == std::vector<std::string>{other});
}

namespace {

// ほかのソフトが、読み取りを許さずにファイルを開いている状態を作る (共有を許さない開き方)。閉じるまで、同じ
// ファイルを読む処理は開けない。
class DenyReadHold {
public:
	void take(const std::string &path)
	{
#ifdef _WIN32
		if (fd_ < 0)
			_wsopen_s(&fd_, std::filesystem::u8path(path).wstring().c_str(), _O_RDONLY | _O_BINARY, _SH_DENYRW, 0);
#endif
	}
	~DenyReadHold() { release(); }
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

TEST_CASE("ロックファイルを確認した後に録画ログを読み直せなかったセッションの保存先は、一覧から外さない")
{
	using cordmux::record::FinalizeState;
	const TempDir now_base("dirs_n6_now");
	const TempDir old_base("dirs_n6_old");
	const TempDir config("dirs_n6_config");
	const CountingStore store(join_path(config.path(), cordmux::record::kPendingDirsFileName));
	const std::string ledger = write_session_ledger(old_base.path(), "2000-01-05 10-00-00",
							ledger_text_with_state("f", FinalizeState::Failed, false));
	const std::string materials = std::filesystem::u8path(ledger).parent_path().u8string();
	store.write({old_base.path()});

	// ロックファイルを確認した時点で、ほかのソフトが録画ログを開く (ネットワークが一瞬切れたときと同じく、読めない
	// 状態)。最初の読み込みは通っている。
	const auto hold = std::make_shared<DenyReadHold>();
	const auto looked = std::make_shared<std::atomic<bool>>(false);
	auto log = std::make_shared<std::string>();
	RecordPlatformOps ops = make_real_record_platform_ops(log);
	const auto lock_held = ops.lock_held;
	ops.lock_held = [lock_held, hold, looked, ledger, materials](const std::string &path) {
		if (path.rfind(materials, 0) == 0) {
			hold->take(ledger);
			looked->store(true);
		}
		return lock_held(path);
	};
	cordmux::config::Settings settings;
	{
		Recorder recorder(settings, ops, now_base.path());
		recorder.remember_pending_dirs(store.store(), [](const std::string &) { return true; });
		REQUIRE(wait_for([&looked]() { return looked->load(); }));
		recorder.stop(0); // 問い合わせのスレッド (と、その集計) を待つ
		CHECK(hold->held());
		// 読めないセッションは数えないが、済んだか分からないので、その保存先は一覧に残る。アクセスできて
		// 録画ファイルの作成の済んでいないセッションがないとして外すと、現在の保存先でないその保存先の失敗したセッションが、
		// 次の起動から集計に出ない。
		CHECK_FALSE(recorder.snapshot().pending_present);
	}
	CHECK(store.read() == std::vector<std::string>{old_base.path()});
	CHECK(*store.writes == 0);
	hold->release();

	// 読めるようになれば、次の起動で数える。
	Recorder recorder(settings, make_real_record_platform_ops(log), now_base.path());
	recorder.remember_pending_dirs(store.store(), [](const std::string &) { return true; });
	REQUIRE(wait_until(recorder, [](const RecordingSnapshot &s) { return s.pending_session_count == 1; }));
	recorder.stop(0);
}

TEST_CASE("削除を許さない保存先 (読み取り専用の古いロックファイル) でも hold_lock_file は共有を許さずに持ち、閉じた後は使用中と判定しない")
{
	const TempDir dir("lock_readonly");
	const std::string lock = join_path(dir.path(), cordmux::record::kSessionLockFileName);
	write_text(lock, "");
	const std::filesystem::perms writes =
		std::filesystem::perms::owner_write | std::filesystem::perms::group_write | std::filesystem::perms::others_write;
	std::error_code ec;
	std::filesystem::permissions(std::filesystem::u8path(lock), writes, std::filesystem::perm_options::remove, ec);
	REQUIRE_FALSE(ec);

	// 閉じると削除される指定の開き方は、共有違反以外の理由で断られる。hold_lock_file は、そのときは削除の指定を
	// 外し、共有を許さない開き方のまま開き直す。
	bool held_elsewhere = true;
	const FileHandle first = cordmux::platform::hold_lock_file(lock, &held_elsewhere);
	REQUIRE(first != 0);
	CHECK_FALSE(held_elsewhere);
	// 持っている間の排他は保たれる (ほかのハンドルは共有違反で持てず、集計は使用中と判定する)。
	CHECK(cordmux::platform::lock_file_held(lock));
	bool second_elsewhere = false;
	CHECK(cordmux::platform::hold_lock_file(lock, &second_elsewhere) == 0);
	CHECK(second_elsewhere);
	// 閉じた後もロックファイルは残るが、電源が落ちたときの残りと同じく、使用中とは判定しない。
	cordmux::platform::close(first);
	CHECK(std::filesystem::exists(std::filesystem::u8path(lock), ec));
	CHECK_FALSE(cordmux::platform::lock_file_held(lock));

	// テストのフォルダーを削除できるように、書き込める状態に戻す。
	std::filesystem::permissions(std::filesystem::u8path(lock), writes, std::filesystem::perm_options::add, ec);
}

namespace {

// 途中で読めなくなるフォルダーの一覧 (ネットワークの場所が切れる、外付けのドライブを抜く)。実際のフォルダーでは
// 起こしにくいので、このクラスで起こす。実際の一覧をラップし、`fail_at` 回目の次に進む読み込みを失敗にする (実際の
// 一覧の項目の数を渡すと、終わりの代わりに失敗が返る。項目を全部読んだ後の読み込みが失敗する状態)。失敗の返し方は標準の
// directory_iterator と同じで、increment(ec) は ec に入れ、++ は filesystem_error を投げる (製品のコードが ++ を
// 使うと、テストの中で例外が投げられて失敗する)。
class FailingDirIterator {
public:
	FailingDirIterator() = default;
	FailingDirIterator(std::filesystem::directory_iterator inner, int fail_at) : inner_(std::move(inner)), fail_at_(fail_at)
	{
	}
	const std::filesystem::directory_entry &operator*() const { return *inner_; }
	bool operator!=(const FailingDirIterator &other) const { return inner_ != other.inner_; }
	FailingDirIterator &increment(std::error_code &ec)
	{
		ec.clear();
		if (++steps_ == fail_at_) {
			ec = std::make_error_code(std::errc::io_error);
			return *this;
		}
		inner_.increment(ec);
		return *this;
	}
	FailingDirIterator &operator++()
	{
		std::error_code ec;
		increment(ec);
		if (ec)
			throw std::filesystem::filesystem_error("directory_iterator::operator++", ec);
		return *this;
	}

private:
	std::filesystem::directory_iterator inner_;
	int fail_at_ = -1;
	int steps_ = 0;
};

// 保存先の `cordmux` にあるセッションのフォルダー数 (一覧の項目数)。
int session_dir_count(const std::string &base)
{
	int count = 0;
	for (const std::filesystem::directory_entry &entry :
	     std::filesystem::directory_iterator(std::filesystem::u8path(base))) {
		(void)entry;
		++count;
	}
	return count;
}

} // namespace

TEST_CASE("フォルダーの一覧を途中で読めなくなっても collect_subdirectory_names は投げず、読めた所までの名前を捨てて Failed を返す")
{
	using cordmux::record::collect_subdirectory_names;
	using cordmux::record::ListOutcome;
	const TempDir dir("list_n9");
	for (const char *name : {"2000-01-01 10-00-00", "2000-01-01 11-00-00", "2000-01-01 12-00-00", "2000-01-01 13-00-00"})
		std::filesystem::create_directories(std::filesystem::u8path(dir.child(name)));
	write_text(dir.child("note.txt"), "x"); // フォルダーでない項目は並べない
	const std::vector<std::string> all = {"2000-01-01 10-00-00", "2000-01-01 11-00-00", "2000-01-01 12-00-00",
					      "2000-01-01 13-00-00"};
	const auto open = [&dir]() { return std::filesystem::directory_iterator(std::filesystem::u8path(dir.path())); };

	// 失敗しない一覧は全部を並べる (製品の関数も同じ)。
	ListOutcome outcome = ListOutcome::Failed;
	CHECK(collect_subdirectory_names(FailingDirIterator(open(), -1), &outcome) == all);
	CHECK(outcome == ListOutcome::Listed);
	outcome = ListOutcome::Failed;
	CHECK(list_subdirectories(dir.path(), &outcome) == all);
	CHECK(outcome == ListOutcome::Listed);

	// 2 つ目の項目に進む読み込み、途中の読み込み、項目を全部読んだ後の読み込みが失敗する。どれも投げず、空の一覧と
	// Failed を返す。範囲 for (例外を投げる ++) を使うと filesystem_error が投げられ、受ける所のないスレッドでは
	// OBS が終了する。
	for (int fail_at : {1, 3, 5}) {
		CAPTURE(fail_at);
		std::vector<std::string> names = {"stale"};
		outcome = ListOutcome::Listed;
		CHECK_NOTHROW(names = collect_subdirectory_names(FailingDirIterator(open(), fail_at), &outcome));
		CHECK(outcome == ListOutcome::Failed);
		CHECK(names.empty());
	}
}

TEST_CASE("UTF-8 として読めないパスと、UTF-8 に変換できない名前の項目があっても、list_subdirectories は投げない")
{
	using cordmux::record::ListOutcome;
	ListOutcome outcome = ListOutcome::Listed;
	std::vector<std::string> names = {"stale"};
	CHECK_NOTHROW(names = list_subdirectories(std::string("C:/cordmux-test-\xff\xfe-bad"), &outcome));
	CHECK(outcome == ListOutcome::Failed);
	CHECK(names.empty());

#ifdef _WIN32
	// Windows のファイル名は、対になっていないサロゲートを含められる (ほかのソフトが作ることがある)。その項目は
	// 飛ばし、ほかのセッションのフォルダーは並べる (一覧の全体を読めないとはしない)。
	const TempDir dir("list_n9_name");
	std::filesystem::create_directories(std::filesystem::u8path(dir.child("2000-01-01 10-00-00")));
	std::wstring odd = L"odd-";
	odd.push_back(static_cast<wchar_t>(0xD800));
	std::error_code ec;
	std::filesystem::create_directory(std::filesystem::u8path(dir.path()) / std::filesystem::path(odd), ec);
	REQUIRE_FALSE(ec);
	outcome = ListOutcome::Failed;
	CHECK_NOTHROW(names = list_subdirectories(dir.path(), &outcome));
	CHECK(outcome == ListOutcome::Listed);
	CHECK(names == std::vector<std::string>{"2000-01-01 10-00-00"});
#endif
}

TEST_CASE("一覧の保存先のフォルダーを途中で読めなくなったら、アクセスできない保存先として一覧に残し、読めた所までのセッションを数えない")
{
	using cordmux::record::collect_subdirectory_names;
	using cordmux::record::FinalizeState;
	using cordmux::record::ListOutcome;
	const TempDir now_base("dirs_n9_now");
	const TempDir far_base("dirs_n9_far");
	const TempDir config("dirs_n9_config");
	const CountingStore store(join_path(config.path(), cordmux::record::kPendingDirsFileName));
	write_session_ledger(far_base.path(), "2000-01-07 10-00-00", ledger_text_with_state("h", FinalizeState::Failed, false));
	write_session_ledger(far_base.path(), "2000-01-07 11-00-00", ledger_text_with_state("i", FinalizeState::Done, false));
	write_session_ledger(far_base.path(), "2000-01-07 12-00-00", ledger_text_with_state("j", FinalizeState::Done, false));
	store.write({far_base.path()});
	const std::string far_root = far_base.path();
	// 項目を全部読んだ後の読み込みが失敗する (読めた所までの名前に、失敗したセッションが入る状態)。
	const int fail_at = session_dir_count(far_base.path());
	REQUIRE(fail_at == 3);

	const auto listed_far = std::make_shared<std::atomic<int>>(0);
	auto log = std::make_shared<std::string>();
	RecordPlatformOps ops = make_real_record_platform_ops(log);
	const auto real_list = ops.list_subdirectories;
	REQUIRE(real_list);
	ops.list_subdirectories = [real_list, far_root, fail_at, listed_far](const std::string &path, ListOutcome *outcome) {
		if (path != far_root)
			return real_list(path, outcome);
		++*listed_far;
		return collect_subdirectory_names(FailingDirIterator(std::filesystem::directory_iterator(std::filesystem::u8path(path)),
								     fail_at),
						  outcome);
	};
	cordmux::config::Settings settings;
	{
		Recorder recorder(settings, ops, now_base.path());
		recorder.remember_pending_dirs(store.store(), [](const std::string &) { return true; });
		REQUIRE(wait_for([&listed_far]() { return listed_far->load() >= 1; }));
		recorder.stop(0); // 問い合わせのスレッド (と、その集計) を待つ
		// 読めた所までの名前は数えない (失敗のメッセージを、起動によって表示したりしなかったりしない)。
		CHECK_FALSE(recorder.snapshot().pending_present);
		CHECK(recorder.snapshot().failed_session_dir.empty());
	}
	// アクセスできない保存先と同じく一覧に残す (読めなかったので、録画ファイルの作成の済んでいないセッションがないとは
	// 分からない)。
	// 書かない。
	CHECK(store.read() == std::vector<std::string>{far_base.path()});
	CHECK(*store.writes == 0);

	// 読めるようになれば、次の起動で数える (一覧に残した保存先から探す)。
	Recorder recorder(settings, make_real_record_platform_ops(log), now_base.path());
	recorder.remember_pending_dirs(store.store(), [](const std::string &) { return true; });
	REQUIRE(wait_until(recorder, [](const RecordingSnapshot &s) { return s.pending_session_count == 1; }));
	CHECK(recorder.snapshot().failed_session_dir == join_path(far_root, "2000-01-07 10-00-00"));
	recorder.stop(0);
	CHECK(store.read() == std::vector<std::string>{far_base.path()});
}

TEST_CASE("問い合わせのスレッドは、答えた後の集計で例外が投げられても外に出さず、その保存先を一覧に残す")
{
	using cordmux::record::FinalizeState;
	using cordmux::record::ListOutcome;
	const TempDir now_base("dirs_n9t_now");
	const TempDir far_base("dirs_n9t_far");
	const TempDir config("dirs_n9t_config");
	const CountingStore store(join_path(config.path(), cordmux::record::kPendingDirsFileName));
	write_session_ledger(far_base.path(), "2000-01-08 10-00-00", ledger_text_with_state("k", FinalizeState::Failed, false));
	store.write({far_base.path()});
	const std::string far_root = far_base.path();

	// 一覧の保存先のフォルダーを読む関数が、最初の 1 回だけ例外を投げる。list_subdirectories は投げない約束だが、
	// 問い合わせのスレッドの try はその約束に頼らない (後から追加した関数や、メモリーの確保の失敗も同じように
	// 投げうる)。
	const auto thrown = std::make_shared<std::atomic<int>>(0);
	auto log = std::make_shared<std::string>();
	RecordPlatformOps ops = make_real_record_platform_ops(log);
	const auto real_list = ops.list_subdirectories;
	ops.list_subdirectories = [real_list, far_root, thrown](const std::string &path, ListOutcome *outcome) {
		if (path == far_root && thrown->fetch_add(1) == 0)
			throw std::filesystem::filesystem_error("directory_iterator::operator++",
								std::make_error_code(std::errc::io_error));
		return real_list(path, outcome);
	};
	cordmux::config::Settings settings;
	Recorder recorder(settings, ops, now_base.path());
	recorder.remember_pending_dirs(store.store(), [](const std::string &) { return true; });
	// スレッドで例外を受けないと、ここでテストの実行ファイルごと終了する (std::terminate)。
	REQUIRE(wait_for([&thrown]() { return thrown->load() >= 1; }));
	CHECK_FALSE(recorder.snapshot().pending_present); // 例外を投げた集計は、スナップショットに何も入れていない
	// 例外の後も Recorder は使える (集計のロックは放されている。放されていなければ、ここで止まる)。
	recorder.stop(0);
	// 一覧は書いていない (例外は書く前で、後の集計でも変わらない)。その保存先は残る。
	CHECK(store.read() == std::vector<std::string>{far_base.path()});
	CHECK(*store.writes == 0);
}

// ---------------------------------------------------------------------------
// 起動のときにアクセスできなかった一覧の保存先への、1 度だけの問い合わせ直し (名前に「問い合わせ直し」を含む 5 件)。
// 日本語を含む doctest の -tc のパターンは、ANSI コードページが UTF-8 でない Windows (日本語の Windows は CP932) では
// どのテストにも一致しない。-sf="*test_record_io_phases.cpp" で、この 5 件を含むファイルのテストを実行できる。
// ---------------------------------------------------------------------------

namespace {

// 失敗したセッションが 1 つある一覧の保存先と、空の現在の保存先を組み合わせたもの (一覧のファイルには、
// その保存先だけがある)。
struct ReprobeScene {
	TempDir now_base;
	TempDir far_base;
	TempDir config;
	CountingStore store;
	std::string far_root;
	std::string failed_session;
	ReprobeScene(const std::string &tag, const std::string &session_name)
		: now_base((tag + "_now").c_str()),
		  far_base((tag + "_far").c_str()),
		  config((tag + "_config").c_str()),
		  store(join_path(config.path(), cordmux::record::kPendingDirsFileName)),
		  far_root(far_base.path()),
		  failed_session(join_path(far_root, session_name))
	{
		write_session_ledger(far_base.path(), session_name,
				     ledger_text_with_state(tag, cordmux::record::FinalizeState::Failed, false));
		store.write({far_base.path()});
	}
};

// 問い合わせ直しまでの待ち時間 (テストの中の値)。最初の 3 件は、1 回目の後にまだ数えていないことを、下の ProbeGate
// の順序で確認し、待ち時間に頼らない。最も短い 1 ms で実行すること自体が、待ち時間に頼っていないことを示す (0 以下は
// 製品の既定の 60 秒に戻る)。
constexpr int kTestReprobeDelayMs = 1;

// テストの中の待ち時間の上限 (ビルドと重なって遅い PC でも、正しい製品なら間に合う長さ)。間に合わなければ失敗にする。
constexpr std::chrono::seconds kReprobeTestLimit{30};

// テスト用の問い合わせを、テストが許した回数だけ通す。n 回目の問い合わせは、テストが n 回目まで許すまで待つ。
// 問い合わせ直しが、1 回目の後の確認より先に来ないことを、待ち時間ではなく順序で決める。
// - n 回目が enter を呼んだことは、n - 1 回目までの処理 (問い合わせ、集計、待ち) が済んだことと同じである (1 つの
//   スレッドが順に問い合わせる。recorder.cpp の remember_pending_dirs の body)。問い合わせはロックを持たずに呼ばれる
//   ので、ここで止めてもスナップショットは読める。
// - テストが途中で失敗して戻ったときは、ProbeGateOpener がすべて通す。待ちには上限 (kReprobeTestLimit) もあり、上限で
//   通ったときは timed_out に残してテストを失敗にする (起動が問い合わせを待つ場合も、ここで分かる)。
// 待ち時間を長くする方法と、確認の前に眠る方法は、どちらも時間の幅に頼る。製品には止めるための関数を追加しない。
// 問い合わせはテストが渡すものなので、テストの側で止められる。
class ProbeGate {
public:
	// 問い合わせの中から呼ぶ。何回目か (1 から) を返す。
	int enter()
	{
		std::unique_lock<std::mutex> lock(mu_);
		const int nth = ++entered_;
		cv_.notify_all();
		if (!cv_.wait_for(lock, kReprobeTestLimit, [this, nth]() { return opened_ >= nth; }))
			timed_out_ = true;
		return nth;
	}
	// n 回目までを通す。
	void open_through(int n)
	{
		{
			std::lock_guard<std::mutex> lock(mu_);
			opened_ = std::max(opened_, n);
		}
		cv_.notify_all();
	}
	void open_all() { open_through(1 << 30); }
	// n 回目の問い合わせが enter を呼ぶまで待つ (上限 kReprobeTestLimit)。
	bool wait_entered(int n)
	{
		std::unique_lock<std::mutex> lock(mu_);
		return cv_.wait_for(lock, kReprobeTestLimit, [this, n]() { return entered_ >= n; });
	}
	int entered()
	{
		std::lock_guard<std::mutex> lock(mu_);
		return entered_;
	}
	bool timed_out()
	{
		std::lock_guard<std::mutex> lock(mu_);
		return timed_out_;
	}

private:
	std::mutex mu_;
	std::condition_variable cv_;
	int entered_ = 0;
	int opened_ = 0;
	bool timed_out_ = false;
};

// スコープを抜けるときに、ProbeGate ですべての問い合わせを通す。Recorder より後に宣言する。先に破棄されて問い合わせを
// 通し、その後の Recorder の stop が問い合わせのスレッドを待てるようにするためである。逆の順だと、テストが途中で
// 失敗したときに上限まで止まる。
struct ProbeGateOpener {
	std::shared_ptr<ProbeGate> gate;
	~ProbeGateOpener() { gate->open_all(); }
};

// スナップショットの条件が真になるまで待つ (上限 kReprobeTestLimit。wait_until の 5 秒より長い)。
template <typename Pred> bool wait_snapshot_long(Recorder &recorder, Pred pred)
{
	const auto deadline = std::chrono::steady_clock::now() + kReprobeTestLimit;
	while (std::chrono::steady_clock::now() < deadline) {
		if (pred(recorder.snapshot()))
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return pred(recorder.snapshot());
}

} // namespace

TEST_CASE("起動のときの問い合わせに答えなかった保存先も、少し後の 1 度の問い合わせ直しで答えれば数える")
{
	const ReprobeScene scene("dirs_r19c_late", "2000-01-09 10-00-00");
	// 1 回目はアクセスできない (false を返すか、例外を投げる)。2 回目は答える (ログインの直後で、ネットワークの
	// 場所がまだ接続していない状態)。
	bool throw_first = false;
	SUBCASE("1 回目は偽を返す") { throw_first = false; }
	SUBCASE("1 回目は例外を投げる") { throw_first = true; }
	const auto gate = std::make_shared<ProbeGate>();
	const auto others = std::make_shared<std::atomic<int>>(0);
	const std::string far = scene.far_base.path();
	const auto reachable = [gate, others, far, throw_first](const std::string &dir) {
		if (dir != far) {
			++*others;
			return false;
		}
		if (gate->enter() == 1) {
			if (throw_first)
				throw std::runtime_error("cordmux-test: not ready yet");
			return false;
		}
		return true;
	};
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	Recorder recorder(settings, make_real_record_platform_ops(log), scene.now_base.path());
	const ProbeGateOpener opener{gate};
	recorder.set_pending_dir_reprobe_delay_ms(kTestReprobeDelayMs);
	recorder.remember_pending_dirs(scene.store.store(), reachable);
	// 起動は問い合わせも問い合わせ直しも待たない。問い合わせを止めたまま、ここに戻った (待てば上限まで戻らず、
	// timed_out が true になる)。
	CHECK_FALSE(gate->timed_out());

	gate->open_through(1);
	// 2 回目が enter を呼んだので、1 回目の処理は済み、問い合わせ直しの答えはまだである (順序で決まり、待ち時間に
	// 頼らない)。
	REQUIRE(gate->wait_entered(2));
	CHECK_FALSE(recorder.snapshot().pending_present); // 1 回目では集計に出ない
	gate->open_through(2);
	REQUIRE(wait_snapshot_long(recorder, [](const RecordingSnapshot &s) { return s.pending_session_count == 1; }));
	CHECK(recorder.snapshot().failed_session_dir == scene.failed_session);
	recorder.stop(0);
	CHECK(gate->entered() == 2);
	CHECK(others->load() == 0);
	CHECK_FALSE(gate->timed_out());
	// 一覧は変わらない (録画ファイルの作成の済んでいないセッションがある)。書かない。
	CHECK(scene.store.read() == std::vector<std::string>{far});
	CHECK(*scene.store.writes == 0);
}

TEST_CASE("起動のときの問い合わせに答えたがフォルダーを読めなかった保存先も、1 度の問い合わせ直しで読めれば数える")
{
	using cordmux::record::ListOutcome;
	const ReprobeScene scene("dirs_r19c_list", "2000-01-10 10-00-00");
	// 一覧の保存先のフォルダーを、最初の 1 回だけ読めなくする (答えた直後に切れた、途中で読めなくなった状態。
	// ListOutcome::Failed)。
	const auto listed_far = std::make_shared<std::atomic<int>>(0);
	auto log = std::make_shared<std::string>();
	RecordPlatformOps ops = make_real_record_platform_ops(log);
	const auto real_list = ops.list_subdirectories;
	REQUIRE(real_list);
	const std::string far_root = scene.far_root;
	ops.list_subdirectories = [real_list, far_root, listed_far](const std::string &path, ListOutcome *outcome) {
		if (path == far_root && listed_far->fetch_add(1) == 0) {
			if (outcome != nullptr)
				*outcome = ListOutcome::Failed;
			return std::vector<std::string>();
		}
		return real_list(path, outcome);
	};
	const auto gate = std::make_shared<ProbeGate>();
	cordmux::config::Settings settings;
	Recorder recorder(settings, ops, scene.now_base.path());
	const ProbeGateOpener opener{gate};
	recorder.set_pending_dir_reprobe_delay_ms(kTestReprobeDelayMs);
	recorder.remember_pending_dirs(scene.store.store(), [gate](const std::string &) {
		gate->enter();
		return true;
	});
	CHECK_FALSE(gate->timed_out()); // 起動は問い合わせを待たない
	gate->open_through(1);
	// 2 回目が enter を呼んだので、1 回目の答えに続く集計 (フォルダーを読めなかった) は済み、問い合わせ直しの後の
	// 集計はまだである。
	REQUIRE(gate->wait_entered(2));
	CHECK(listed_far->load() == 1);
	CHECK_FALSE(recorder.snapshot().pending_present);
	gate->open_through(2);
	REQUIRE(wait_snapshot_long(recorder, [](const RecordingSnapshot &s) { return s.pending_session_count == 1; }));
	CHECK(recorder.snapshot().failed_session_dir == scene.failed_session);
	CHECK(listed_far->load() == 2);
	recorder.stop(0);
	CHECK(gate->entered() == 2);
	CHECK_FALSE(gate->timed_out());
	CHECK(scene.store.read() == std::vector<std::string>{scene.far_base.path()});
	CHECK(*scene.store.writes == 0);
}

TEST_CASE("一覧にある現在の保存先を起動のときに読めなくても、1 度の問い合わせ直しで読めれば数える")
{
	using cordmux::record::FinalizeState;
	using cordmux::record::ListOutcome;
	const TempDir now_base("dirs_r19c_now");
	const TempDir config("dirs_r19c_now_config");
	const CountingStore store(join_path(config.path(), cordmux::record::kPendingDirsFileName));
	write_session_ledger(now_base.path(), "2000-01-11 10-00-00", ledger_text_with_state("m", FinalizeState::Failed, false));
	store.write({now_base.path()});
	const std::string now_root = now_base.path();
	// 外付けのドライブにある現在の保存先が、まだスリープから復帰していない状態。起動のときの 2 回の集計 (構築と、
	// 一覧を使い始めたとき) では読めない。
	const auto failing = std::make_shared<std::atomic<bool>>(true);
	auto log = std::make_shared<std::string>();
	RecordPlatformOps ops = make_real_record_platform_ops(log);
	const auto real_list = ops.list_subdirectories;
	REQUIRE(real_list);
	ops.list_subdirectories = [real_list, now_root, failing](const std::string &path, ListOutcome *outcome) {
		if (path == now_root && failing->load()) {
			if (outcome != nullptr)
				*outcome = ListOutcome::Failed;
			return std::vector<std::string>();
		}
		return real_list(path, outcome);
	};
	// 問い合わせ直しは、テストがフォルダーを読めるようにして ProbeGate で通すまで止める (時間の幅に頼らないため)。
	const auto gate = std::make_shared<ProbeGate>();
	cordmux::config::Settings settings;
	Recorder recorder(settings, ops, now_base.path());
	const ProbeGateOpener opener{gate};
	recorder.set_pending_dir_reprobe_delay_ms(kTestReprobeDelayMs);
	recorder.remember_pending_dirs(store.store(), [gate](const std::string &) {
		gate->enter();
		return true;
	});
	CHECK_FALSE(gate->timed_out()); // 起動は問い合わせを待たない
	CHECK_FALSE(recorder.snapshot().pending_present);
	failing->store(false);
	gate->open_through(1);
	REQUIRE(wait_snapshot_long(recorder, [](const RecordingSnapshot &s) { return s.pending_session_count == 1; }));
	CHECK(recorder.snapshot().failed_session_dir == join_path(now_root, "2000-01-11 10-00-00"));
	recorder.stop(0);
	// 既に探している保存先の 1 回目は起動のときの集計で、問い合わせは問い合わせ直しの 1 回だけである
	// (起動のときにも問い合わせていれば、2 回になる)。
	CHECK(gate->entered() == 1);
	CHECK_FALSE(gate->timed_out());
	CHECK(store.read() == std::vector<std::string>{now_base.path()});
	CHECK(*store.writes == 0);
}

TEST_CASE("問い合わせ直しでも答えない保存先は、そこで問い合わせをやめて一覧に残し、ほかに影響しない")
{
	const ReprobeScene scene("dirs_r19c_gone", "2000-01-12 10-00-00");
	const auto asked = std::make_shared<std::atomic<int>>(0);
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	Recorder recorder(settings, make_real_record_platform_ops(log), scene.now_base.path());
	const int delay_ms = 200;
	recorder.set_pending_dir_reprobe_delay_ms(delay_ms);
	recorder.remember_pending_dirs(scene.store.store(), [asked](const std::string &) {
		++*asked;
		return false;
	});
	REQUIRE(wait_for([&asked]() { return asked->load() >= 2; }));
	// 問い合わせ直しは 1 度だけで、待ち時間の 5 倍を過ぎても 3 回目は問い合わせない。
	std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms * 5));
	CHECK(asked->load() == 2);
	CHECK_FALSE(recorder.snapshot().pending_present);
	CHECK(recorder.snapshot().failed_session_dir.empty());
	// Recorder はそのまま使える (ロックは放されている。放されていなければ、ここで止まる)。アクセスできなかった
	// 保存先は探していないので、作成待ちは出ない。
	recorder.resume_pending();
	CHECK(recorder.snapshot().pending_session_count == 0);
	const auto before = std::chrono::steady_clock::now();
	recorder.stop(0);
	CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - before).count() < 1000);
	// アクセスできなかった保存先は一覧に残す (済んだか分からない)。書かない。
	CHECK(scene.store.read() == std::vector<std::string>{scene.far_base.path()});
	CHECK(*scene.store.writes == 0);
	CHECK(asked->load() == 2);
}

TEST_CASE("問い合わせ直しを待っている間に OBS を閉じても、閉じる処理は待ち時間の分を待たない")
{
	// 製品の待ち時間 (60 秒) のまま、1 回目の問い合わせが戻った直後 (スレッドが待ちに入る所) に止める。通知を
	// 取りこぼすと、その stop は待ち時間の分だけ止まる (OBS の終了が止まる)。待ちに入る所の前後に重なるように、
	// 20 回繰り返す。
	static_assert(cordmux::record::kPendingDirReprobeDelayMs >= 10000, "the product delay must be long enough to show a lost wakeup");
	const ReprobeScene scene("dirs_r19c_quit", "2000-01-13 10-00-00");
	auto log = std::make_shared<std::string>();
	cordmux::config::Settings settings;
	for (int round = 0; round < 20; ++round) {
		CAPTURE(round);
		const auto asked = std::make_shared<std::atomic<int>>(0);
		Recorder recorder(settings, make_real_record_platform_ops(log), scene.now_base.path());
		recorder.remember_pending_dirs(scene.store.store(), [asked](const std::string &) {
			++*asked;
			return false;
		});
		REQUIRE(wait_for([&asked]() { return asked->load() >= 1; }));
		// 止めるタイミングを少しずつずらす。
		if (round % 2 == 1)
			std::this_thread::sleep_for(std::chrono::milliseconds(round));
		const auto before = std::chrono::steady_clock::now();
		recorder.stop(0);
		CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - before).count() <
		      2000);
		CHECK(asked->load() == 1); // 止めた後は問い合わせ直さない
	}
	CHECK(scene.store.read() == std::vector<std::string>{scene.far_base.path()});
	CHECK(*scene.store.writes == 0);
}
