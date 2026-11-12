// SPDX-License-Identifier: Apache-2.0

// 録画ファイルの作成の進み具合と、進み具合が止まったかの判定のテスト。実際の FFmpeg は起動せず、テスト用の FFmpeg を使う。
// テスト用の FFmpeg は RecordPlatformOps の子プロセスの関数を差し替えたもので、ポーリング (200 ms) のたびにテスト用の
// 時計を 200 ms 進め、`-progress` に渡されたファイルに FFmpeg と同じ形の行 (`out_time_us=...` と
// `progress=continue`) を追記する。割合が進むときだけ増えることと、`out_time_us` が進まなくなったら決めた時間で
// 止まったと判定し、その間は割合が増えないことを確認する。

#include "doctest.h"

#include "config/settings.h"
#include "record/record_args.h"
#include "record/record_finalize.h"
#include "record/record_ledger.h"
#include "record/record_segment.h"
#include "record/record_types.h"
#include "test_record_io_shared.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

using cordmux::record::ChildWait;
using cordmux::record::ChildWaitResult;
using cordmux::record::FileHandle;
using cordmux::record::FinalizeProgress;
using cordmux::record::FinalizeRun;
using cordmux::record::FinalizeState;
using cordmux::record::join_path;
using cordmux::record::LedgerDocument;
using cordmux::record::LedgerPersonRow;
using cordmux::record::LedgerSegmentRow;
using cordmux::record::MediaKind;
using cordmux::record::parse_progress_out_time_us;
using cordmux::record::RecordPlatformOps;
using cordmux::record::run_finalize;
using cordmux::record::StreamKind;
using cordmux::tests::record_io::TempDir;

namespace {

constexpr std::int64_t kPollUs = 200000; // ポーリング 1 回分 (kFinalizePollMs)

// テスト用の FFmpeg。out_time_at は、poll 回目 (1 から数える) のポーリングで書く out_time_us の値を返し、値なしなら
// 書かない。exit_at 回目のポーリングで終了する (0 なら自分では終了しない)。
struct FakeFfmpeg {
	std::function<std::optional<std::int64_t>(int poll)> out_time_at;
	int exit_at = 0;
	int exit_code = 0;

	// 状態 (テストが読む)。
	std::int64_t steady_us = 1'000'000;
	int polls = 0;
	std::string progress_path;
	std::vector<std::string> args;

	RecordPlatformOps build(const std::string &dir)
	{
		RecordPlatformOps ops;
		ops.make_directories = [](const std::string &) { return true; };
		ops.open_file = [](const std::string &) -> FileHandle { return 1; };
		ops.append = [](FileHandle, const void *, std::size_t) { return true; };
		ops.close = [](FileHandle) {};
		ops.free_space_bytes = [](const std::string &) { return std::optional<std::uint64_t>(); };
		ops.start_child = [this](const std::string &, const std::vector<std::string> &given) {
			args = given;
			const auto it = std::find(given.begin(), given.end(), "-progress");
			if (it != given.end() && it + 1 != given.end())
				progress_path = *(it + 1);
			return static_cast<cordmux::record::ChildHandle>(7);
		};
		ops.wait_child = [this](cordmux::record::ChildHandle, int) {
			++polls;
			steady_us += kPollUs;
			if (out_time_at && !progress_path.empty()) {
				const std::optional<std::int64_t> out = out_time_at(polls);
				if (out) {
					std::ofstream file(std::filesystem::u8path(progress_path),
							   std::ios::binary | std::ios::app);
					file << "frame=0\nout_time_us=" << *out << "\nprogress=continue\n";
				}
			}
			ChildWaitResult result;
			if (exit_at > 0 && polls >= exit_at) {
				result.outcome = ChildWait::Exited;
				result.exit_code = exit_code;
			} else {
				result.outcome = ChildWait::Timeout;
			}
			return result;
		};
		ops.kill_child = [](cordmux::record::ChildHandle) {};
		ops.now_steady_us = [this]() { return steady_us; };
		ops.now_wall_us = [this]() { return steady_us + 946'684'800'000'000; };
		ops.ffmpeg_path = [dir]() { return join_path(dir, "fake-ffmpeg.exe"); };
		return ops;
	}
};

// 声の録画ファイル 1 つ (長さ 1 秒) を作る録画ログ。セグメントのファイルは置かない。読めないセグメントは無音のまま
// 残るので、生の PCM は 0 を書くだけで、すぐ作れる。
LedgerDocument one_voice_second()
{
	LedgerDocument doc;
	doc.session.session_id = "2000-01-01 12-00-00";
	doc.session.started_at_steady_us = 100'000'000;
	doc.session.ended_at_steady_us = 101'000'000;
	LedgerPersonRow person;
	person.user_id = "u1";
	person.display_name = "taro";
	doc.people.push_back(person);
	LedgerSegmentRow voice;
	voice.user_id = "u1";
	voice.kind = StreamKind::Voice;
	voice.media = MediaKind::Audio;
	voice.file = "segments/u1_voice_000001.mov";
	voice.timescale = 48000;
	voice.sample_count = 48000;
	voice.duration_ticks = 48000;
	voice.first_pts_us = 0;
	voice.first_pts_steady_us = 100'000'000;
	doc.segments.push_back(voice);
	return doc;
}

struct Seen {
	std::vector<FinalizeProgress> all;
};

FinalizeRun make_run(const RecordPlatformOps &ops, const TempDir &dir, const LedgerDocument &doc, Seen &seen,
		     int stall_s, std::function<bool()> should_stop = nullptr)
{
	FinalizeRun run;
	run.ops = ops;
	run.options.ffmpeg_path = ops.ffmpeg_path();
	run.options.session_dir = dir.path();
	run.options.materials_dir = dir.child("data");
	run.options.finalize_stall_s = stall_s;
	run.doc = &doc;
	run.append_line = [](const std::string &) {};
	run.on_progress = [&seen](const FinalizeProgress &progress) { seen.all.push_back(progress); };
	run.should_stop = std::move(should_stop);
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::u8path(dir.child("data")), ec);
	return run;
}

} // namespace

TEST_CASE("FFmpeg の -progress の出力から、最後の out_time_us を読む")
{
	CHECK_FALSE(parse_progress_out_time_us("").has_value());
	CHECK(*parse_progress_out_time_us("out_time_us=1000\nprogress=continue\n") == 1000);
	// 最後の値が勝つ。N/A は数えない。途中で切れた最後の行 (改行がない) も数えない。
	CHECK(*parse_progress_out_time_us("out_time_us=1000\nout_time_us=2000\nout_time_us=N/A\n") == 2000);
	CHECK(*parse_progress_out_time_us("out_time_us=1000\nout_time_us=20") == 1000);
	// 末尾だけを読んで先頭の行が途中で切れていても、その行はキーとして一致しない。
	CHECK(*parse_progress_out_time_us("e_us=99\nout_time_us=5\r\n") == 5);
}

TEST_CASE("-progress の出力の 64 ビットに収まらない数字の並びは、例外を投げずにその行を数えない")
{
	// 壊れた出力で 30 桁の数字が来ても、例外を投げない。録画ファイルの作成のスレッドの外へ例外が抜けると、OBS が
	// クラッシュする。
	const std::string huge = "out_time_us=123456789012345678901234567890\n";
	CHECK_NOTHROW(parse_progress_out_time_us(huge));
	CHECK_FALSE(parse_progress_out_time_us(huge).has_value());
	// 前の正しい行は残る (数えないのは壊れた行だけ)。
	CHECK(*parse_progress_out_time_us("out_time_us=1000\n" + huge) == 1000);
	const std::string negative_huge = "out_time_us=-123456789012345678901234567890\n";
	CHECK_NOTHROW(parse_progress_out_time_us(negative_huge));
	CHECK_FALSE(parse_progress_out_time_us(negative_huge).has_value());
	// 64 ビットの最大値は、そのまま読む。
	CHECK(*parse_progress_out_time_us("out_time_us=9223372036854775807\n") == INT64_MAX);
}

TEST_CASE("遅いが進むテスト用の FFmpeg では、割合は進むときだけ増え、止まったとは判定しない")
{
	const TempDir dir("progress_slow");
	FakeFfmpeg fake;
	// ポーリングのたびに 0.1 秒ずつ進む (録画ログの長さ 1 秒を 10 回で書き終える)。12 回目で終了する。
	fake.out_time_at = [](int poll) { return std::optional<std::int64_t>(std::min(poll, 10) * 100000); };
	fake.exit_at = 12;
	const RecordPlatformOps ops = fake.build(dir.path());
	const LedgerDocument doc = one_voice_second();
	Seen seen;
	const FinalizeRun run = make_run(ops, dir, doc, seen, 1); // 止まったと判定するまで 1 秒 (ポーリング 5 回)

	const FinalizeProgress last = run_finalize(run);
	// 引数に -progress の出力先が入っている (data/ の中)。
	REQUIRE_FALSE(fake.progress_path.empty());
	CHECK(fake.progress_path.find(dir.child("data")) == 0);

	int previous = -1;
	int increases = 0;
	bool any_stalled = false;
	for (const FinalizeProgress &p : seen.all) {
		any_stalled = any_stalled || p.stalled;
		if (p.current_index != 1 || !p.current_permille)
			continue;
		CHECK(*p.current_permille >= previous); // 戻らない
		if (*p.current_permille > previous)
			++increases;
		previous = *p.current_permille;
	}
	// 音声の段階の 0 と、FFmpeg の段階の 10 回の報告 (550, 600, ..., 1000) で、11 回増えた。
	CHECK(increases == 11);
	CHECK(previous == 1000);
	CHECK_FALSE(any_stalled);
	// 終わった後は、作成中の録画ファイルはない。
	CHECK(last.current_index == 0);
	CHECK_FALSE(last.current_permille.has_value());
	CHECK(last.files_total == 1);
}

TEST_CASE("進み具合が途中で止まるテスト用の FFmpeg では、決めた時間で止まったと判定し、その間に割合は増えない")
{
	const TempDir dir("progress_stall");
	FakeFfmpeg fake;
	// 3 回目までは進み、その後は同じ時刻を書き続ける (FFmpeg が応答しなくなった状態)。
	fake.out_time_at = [](int poll) { return std::optional<std::int64_t>(std::min(poll, 3) * 100000); };
	fake.exit_at = 0; // 自分では終了しない
	const RecordPlatformOps ops = fake.build(dir.path());
	const LedgerDocument doc = one_voice_second();
	Seen seen;
	// 止まったと判定するまで 2 秒 (ポーリング 10 回)。40 回ポーリングしたら打ち切る (OBS の終了で上限に達したときと
	// 同じ)。
	FinalizeRun run = make_run(ops, dir, doc, seen, 2, [&fake]() { return fake.polls >= 40; });
	int last_increase_poll = 0;
	int stalled_poll = 0;
	int previous = -1;
	bool increased_while_stalled = false;
	run.on_progress = [&](const FinalizeProgress &p) {
		seen.all.push_back(p);
		if (p.current_index != 1)
			return;
		if (p.current_permille && *p.current_permille > previous) {
			previous = *p.current_permille;
			last_increase_poll = fake.polls;
			increased_while_stalled = increased_while_stalled || (stalled_poll != 0);
		}
		if (p.stalled && stalled_poll == 0)
			stalled_poll = fake.polls;
	};

	const FinalizeProgress last = run_finalize(run);
	CHECK(last.state == FinalizeState::Pending); // 打ち切ったので Pending になり、次の起動で作成待ちに数える
	// 声だけの録画ファイルは、音声の段階と FFmpeg の段階が同じ量 (生の PCM) を扱うので、重みは半分ずつ。
	// 0.3 秒 / 1 秒まで進んだので、500 + 0.3 x 500。
	CHECK(previous == 650);
	CHECK(last_increase_poll == 3);
	// 最後に進んでから 2 秒 (ポーリング 10 回) たったところで、止まったと判定する。早すぎず、遅すぎない。
	CHECK(stalled_poll == 13);
	CHECK_FALSE(increased_while_stalled);
	// 止まったときの進み具合には、最後に進んだ時刻が残る (画面に、進んでいない時間を表示するのに使う)。
	bool found = false;
	for (const FinalizeProgress &p : seen.all) {
		if (p.stalled && p.current_index == 1) {
			CHECK(p.last_progress_steady_us == 1'000'000 + 3 * kPollUs);
			found = true;
			break;
		}
	}
	CHECK(found);
}

TEST_CASE("録画ファイルの作成に失敗した録画ファイルの -progress の出力は残し、生の PCM は削除する")
{
	// テスト用の FFmpeg は録画ファイルを作らないので、作成した後のチェックが通らず、録画ファイルの作成に失敗する。
	const TempDir dir("temp_kept");
	FakeFfmpeg fake;
	fake.out_time_at = [](int poll) { return std::optional<std::int64_t>(poll * 100000); };
	fake.exit_at = 2;
	const RecordPlatformOps ops = fake.build(dir.path());
	const LedgerDocument doc = one_voice_second();
	Seen seen;
	const FinalizeRun run = make_run(ops, dir, doc, seen, 120);
	const FinalizeProgress last = run_finalize(run);
	CHECK(last.files_failed == 1);
	std::error_code ec;
	// 録画ファイルの作成に失敗した録画ファイルの -progress の出力は、原因を調べるために残る。生の PCM はいつも削除する。
	CHECK(std::filesystem::exists(std::filesystem::u8path(fake.progress_path), ec));
	CHECK_FALSE(std::filesystem::exists(std::filesystem::u8path(dir.child("data/audio_u1_voice.pcm")), ec));
}
