// SPDX-License-Identifier: Apache-2.0

// record/record_segment.h の、録画が OFF の間に I/O の関数を呼ばないための判定の単体テスト。録画が OFF のまま 1000 回
// 呼んでも make_directories、open_file、append、start_child が 1 度も呼ばれず、ON にすると呼ばれることを確認する。
// 判定 (recording_gate) を常に true を返す関数に差し替えると、OFF のままでも open_file が呼ばれることも確認する。
// 判定を外しても失敗しないテストでは、OFF の間に何も動かないことを確認したことにならないためである。

#include "doctest.h"

#include "record/record_segment.h"

#include <string>

using cordmux::record::append_ledger_line;
using cordmux::record::close_session;
using cordmux::record::ensure_session_open;
using cordmux::record::FileHandle;
using cordmux::record::RecordPlatformOps;
using cordmux::record::SessionState;

namespace {

// テスト用の RecordPlatformOps。呼ばれた回数だけを数える (実際のファイルには 1 つも触れない)。
struct OpsCounter {
	int make_directories = 0;
	int open_file = 0;
	int append = 0;
	int close = 0;
	int free_space_bytes = 0;
	int start_child = 0;
	int wait_child = 0;
	int kill_child = 0;
	int now_steady_us = 0;
	int now_wall_us = 0;
	int ffmpeg_path = 0;
	std::size_t appended_bytes = 0;

	int io_total() const { return make_directories + open_file + append + start_child; }
};

RecordPlatformOps fake_ops(OpsCounter *counter)
{
	RecordPlatformOps ops;
	ops.make_directories = [counter](const std::string &) {
		++counter->make_directories;
		return true;
	};
	ops.open_file = [counter](const std::string &) -> FileHandle {
		++counter->open_file;
		return FileHandle(1);
	};
	ops.append = [counter](FileHandle, const void *, std::size_t len) {
		++counter->append;
		counter->appended_bytes += len;
		return true;
	};
	ops.close = [counter](FileHandle) { ++counter->close; };
	ops.free_space_bytes = [counter](const std::string &) -> std::optional<std::uint64_t> {
		++counter->free_space_bytes;
		return std::uint64_t(1);
	};
	ops.start_child = [counter](const std::string &, const std::vector<std::string> &) {
		++counter->start_child;
		return cordmux::record::ChildHandle(1);
	};
	ops.wait_child = [counter](cordmux::record::ChildHandle, int) {
		++counter->wait_child;
		return cordmux::record::ChildWaitResult{};
	};
	ops.kill_child = [counter](cordmux::record::ChildHandle) { ++counter->kill_child; };
	ops.now_steady_us = [counter]() -> std::int64_t {
		++counter->now_steady_us;
		return 1000;
	};
	ops.now_wall_us = [counter]() -> std::int64_t {
		++counter->now_wall_us;
		return 946'684'800'000'000;
	};
	ops.ffmpeg_path = [counter]() -> std::string {
		++counter->ffmpeg_path;
		return std::string();
	};
	return ops;
}

constexpr int kFrames = 1000;
const char *const kBaseDir = "C:/record-root";

} // namespace

TEST_CASE("録画が OFF のまま 1000 回呼んでも、I/O の関数は 1 度も呼ばれない")
{
	OpsCounter counter;
	const RecordPlatformOps ops = fake_ops(&counter);
	SessionState state;

	for (int i = 0; i < kFrames; ++i) {
		CHECK_FALSE(ensure_session_open(state, ops, kBaseDir, false, 0));
		CHECK_FALSE(append_ledger_line(state, ops, "{}", false));
	}

	CHECK(counter.make_directories == 0);
	CHECK(counter.open_file == 0);
	CHECK(counter.append == 0);
	CHECK(counter.start_child == 0);
	CHECK(counter.io_total() == 0);
	// 時計も読まない (読むこと自体が、OFF の間に何かを動かすことになる)。
	CHECK(counter.now_wall_us == 0);
	CHECK(counter.now_steady_us == 0);
	CHECK_FALSE(state.open);
}

TEST_CASE("録画を ON にすると、I/O の関数が呼ばれる")
{
	OpsCounter counter;
	const RecordPlatformOps ops = fake_ops(&counter);
	SessionState state;

	for (int i = 0; i < kFrames; ++i) {
		CHECK(ensure_session_open(state, ops, kBaseDir, true, 9 * 3600));
		CHECK(append_ledger_line(state, ops, "{}", true));
	}

	// セッションのフォルダー、中間ファイルのフォルダー、segments/、black/ の 4 つを 1 回ずつ作り、録画ログを
	// 1 回だけ開く (中間ファイルは、セッションのフォルダーの下の 1 つのフォルダーにまとめる)。
	CHECK(counter.make_directories == 4);
	CHECK(counter.open_file == 1);
	CHECK(counter.append == kFrames);
	CHECK(counter.start_child == 0); // 録画ファイルの作成は I/O の部分が行う。純粋関数の部分からは 1 度も起動しない
	CHECK(state.open);
	CHECK(state.session_id == "2000-01-01 09-00-00");
	CHECK(state.session_dir == "C:/record-root/2000-01-01 09-00-00");
	CHECK(state.materials_dir == "C:/record-root/2000-01-01 09-00-00/data");
	// 行末の改行は append_ledger_line が付ける (録画ログは JSONL)。
	CHECK(counter.appended_bytes == std::size_t(kFrames) * 3);
}

TEST_CASE("判定を常に true を返す関数に差し替えると、OFF のままでも open_file が呼ばれる")
{
	OpsCounter counter;
	const RecordPlatformOps ops = fake_ops(&counter);
	SessionState state;

	// OFF の分岐を外した判定 (何を渡しても true を返す)。
	const auto identity_gate = [](bool) { return true; };

	for (int i = 0; i < kFrames; ++i) {
		CHECK(ensure_session_open(state, ops, kBaseDir, false, 0, identity_gate));
		CHECK(append_ledger_line(state, ops, "{}", false, identity_gate));
	}

	CHECK(counter.open_file != 0);
	CHECK(counter.make_directories != 0);
	CHECK(counter.append != 0);
	CHECK(counter.io_total() != 0);
}

TEST_CASE("必要な関数が 1 つでも欠けていれば、セッションを開かない")
{
	OpsCounter counter;
	RecordPlatformOps ops = fake_ops(&counter);
	ops.open_file = nullptr;
	SessionState state;

	CHECK_FALSE(ensure_session_open(state, ops, kBaseDir, true, 0));
	CHECK_FALSE(state.open);
}

TEST_CASE("録画ログを開けなければ、開いたことにしない")
{
	OpsCounter counter;
	RecordPlatformOps ops = fake_ops(&counter);
	ops.open_file = [&counter](const std::string &) -> FileHandle {
		++counter.open_file;
		return cordmux::record::kInvalidFile;
	};
	SessionState state;

	CHECK_FALSE(ensure_session_open(state, ops, kBaseDir, true, 0));
	CHECK_FALSE(state.open);
	CHECK_FALSE(append_ledger_line(state, ops, "{}", true));
	CHECK(counter.append == 0);
}

TEST_CASE("閉じる処理は録画が ON かの判定を通さないので、OFF にした後にも閉じられる")
{
	OpsCounter counter;
	const RecordPlatformOps ops = fake_ops(&counter);
	SessionState state;

	CHECK(ensure_session_open(state, ops, kBaseDir, true, 0));
	close_session(state, ops);
	CHECK(counter.close == 1);
	CHECK_FALSE(state.open);
	CHECK(state.ledger == cordmux::record::kInvalidFile);

	// 2 回閉じても、close は 1 回しか呼ばれない。
	close_session(state, ops);
	CHECK(counter.close == 1);
}
