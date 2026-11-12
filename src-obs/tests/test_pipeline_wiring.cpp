// SPDX-License-Identifier: Apache-2.0

// pipeline/pipeline.h の、起動と送信側に送る値の判断 (純粋関数) と、platform/os_probe.h の
// probe_interval_ms_from_seconds の単体テスト。pipeline.cpp と plugin-main.cpp は libobs に依存してこのテストに
// 入らないが、ここで確認する判断は、Vesktop の設定が書き換わるか、送信側がエンコードを止めるか、といった利用者に
// 見える結果を直接決める。

#include "doctest.h"

#include "install/vencord_dir.h"
#include "pipeline/pipeline.h"
#include "platform/bundle_check.h"
#include "platform/os_probe.h"
#include "ws/control_messages.h"

#include <climits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using cordmux::pipeline::any_cordmux_source;
using cordmux::pipeline::classify_settings_file;
using cordmux::pipeline::decide_media;
using cordmux::pipeline::LayoutPresence;
using cordmux::pipeline::limits_from_settings;
using cordmux::pipeline::media_inputs_of;
using cordmux::pipeline::MediaInputs;
using cordmux::pipeline::run_startup_vencord_dir_rewrite;
using cordmux::pipeline::SettingsFileState;
using cordmux::pipeline::StartupRewriteInputs;
using cordmux::pipeline::StartupRewriteOps;
using cordmux::pipeline::StartupRewriteOutcome;
using cordmux::pipeline::StartupRewriteResult;

namespace {

// ---- 起動時の書き直しのテスト用のファイル (パスは利用者のプロファイルに似せない) ----

const std::string kStatePath = "fake-profile/Vesktop/state.json";
const std::string kBackupPath = "fake-obs/plugin_config/cordmux/vencord_dir_backup.json";
// 前に書いた場所 (移す前の cordmux のフォルダー) と、現在の場所 (移した後のフォルダー)。
const std::string kOldBundle = "C:/old-place/cordmux/data/vencord";
const std::string kNewBundle = "D:/new-place/cordmux/data/vencord";
const std::string kOldDllDir = "C:/old-place/cordmux";
const std::string kNewDllDir = "D:/new-place/cordmux";
// 利用者がもともと使っていた別の Vencord の場所 (バックアップの previous)。
const std::string kUserOriginal = "C:/someone/Vencord/dist";
// cordmux を入れた後に、利用者かほかのツールが選んだ別の場所。
const std::string kForeign = "E:/other-tool/vencord";

std::string state_with(const std::string &dir)
{
	return std::string(R"({"firstLaunch":false,"vencordDir":")") + dir + R"(","maximized":true})";
}

std::string state_without_key()
{
	return R"({"firstLaunch":false,"maximized":true})";
}

cordmux::install::Backup old_backup(std::optional<std::string> previous = kUserOriginal)
{
	cordmux::install::Backup b;
	b.previous = std::move(previous);
	b.written = kOldBundle;
	b.written_at_us = 1000;
	b.dll_dir = kOldDllDir;
	return b;
}

struct FakeFiles {
	std::map<std::string, std::string> files;
	std::vector<std::string> writes; // 書こうとした場所 (順番どおり。失敗したものも入る)
	std::set<std::string> fail_writes;
	bool state_path_resolvable = true;
	int reads = 0;
	// Vesktop が起動しているか (nullopt は調べられない)。既定は起動していない。
	std::optional<bool> vesktop_running = false;
	int vesktop_checks = 0;
	std::size_t writes_at_check = 0; // Vesktop を確認した時点で、書こうとした数
};

StartupRewriteOps ops_for(FakeFiles &fs)
{
	StartupRewriteOps ops;
	ops.vesktop_state_json_path = [&fs](std::string &out) {
		if (!fs.state_path_resolvable)
			return false;
		out = kStatePath;
		return true;
	};
	ops.read_file_bytes = [&fs](const std::string &path, std::string &out, std::size_t max_bytes) {
		++fs.reads;
		const auto it = fs.files.find(path);
		if (it == fs.files.end() || it->second.size() > max_bytes)
			return false;
		out = it->second;
		return true;
	};
	ops.write_file_atomic = [&fs](const std::string &path, const std::string &text) {
		fs.writes.push_back(path);
		if (fs.fail_writes.count(path) != 0)
			return false;
		fs.files[path] = text;
		return true;
	};
	ops.vesktop_running = [&fs]() {
		++fs.vesktop_checks;
		fs.writes_at_check = fs.writes.size();
		return fs.vesktop_running;
	};
	return ops;
}

StartupRewriteInputs moved_inputs()
{
	StartupRewriteInputs in;
	in.uninstalling = false;
	in.feature_enabled = true;
	in.allow_state_json_write = true;
	in.target_dir = kNewBundle;
	in.backup_json_path = kBackupPath;
	in.dll_dir = kNewDllDir;
	in.max_json_bytes = 1u << 20;
	in.now_us = 5000;
	return in;
}

// cordmux のフォルダーを移した後の起動。state.json は前に書いた場所を指し、バックアップもその場所を覚えている。
FakeFiles moved_files()
{
	FakeFiles fs;
	fs.files[kStatePath] = state_with(kOldBundle);
	fs.files[kBackupPath] = cordmux::install::backup_to_json(old_backup());
	return fs;
}

std::optional<cordmux::install::Backup> backup_in(const FakeFiles &fs)
{
	const auto it = fs.files.find(kBackupPath);
	if (it == fs.files.end())
		return std::nullopt;
	cordmux::install::Backup b;
	if (!cordmux::install::backup_from_json(it->second, &b))
		return std::nullopt;
	return b;
}

} // namespace

// ---------------------------------------------------------------------------
// 起動時の書き直し
// ---------------------------------------------------------------------------

TEST_CASE("起動時の書き直しは、値が前に書いた場所のままでフォルダーが移っていれば書く")
{
	FakeFiles fs = moved_files();
	const StartupRewriteResult r = run_startup_vencord_dir_rewrite(moved_inputs(), ops_for(fs));
	CHECK(r.outcome == StartupRewriteOutcome::Wrote);

	// state.json は現在の場所を指し、ほかのキーは残る。
	const std::optional<std::string> dir = cordmux::platform::parse_vencord_dir(fs.files[kStatePath]);
	REQUIRE(dir.has_value());
	CHECK(*dir == kNewBundle);
	CHECK(fs.files[kStatePath].find("\"maximized\":true") != std::string::npos);

	// バックアップは written、written_at、dll_dir を新しくし、previous は利用者の元の値のまま。
	const auto b = backup_in(fs);
	REQUIRE(b.has_value());
	CHECK(b->previous == std::optional<std::string>(kUserOriginal));
	CHECK(b->written == kNewBundle);
	CHECK(b->written_at_us == 5000);
	CHECK(b->dll_dir == kNewDllDir);

	// バックアップを先に書き、次に state.json を書く (dock/dock_poll.cpp の write_vencord_dir と同じ順)。
	REQUIRE(fs.writes.size() == 2);
	CHECK(fs.writes[0] == kBackupPath);
	CHECK(fs.writes[1] == kStatePath);
	// Vesktop が起動していないことを、書く直前に 1 回だけ確認した。
	CHECK(fs.vesktop_checks == 1);
	CHECK(fs.writes_at_check == 0);
}

TEST_CASE("起動時の書き直しは、Vesktop が起動していれば何も書かずに待ち、起動していないと分かってから書く")
{
	// 起動中の Vesktop は state.json をメモリの値で書き直すので、起動中に書いた値は消える。起動していたら何も書かずに
	// VesktopRunning を返し、ドックが Vesktop の終了を見たときに、同じ入力でもう一度呼ぶ (dock_poll_install.cpp)。
	FakeFiles fs = moved_files();
	const std::string state_before = fs.files[kStatePath];
	const std::string backup_before = fs.files[kBackupPath];
	fs.vesktop_running = true;
	StartupRewriteResult r = run_startup_vencord_dir_rewrite(moved_inputs(), ops_for(fs));
	CHECK(r.outcome == StartupRewriteOutcome::VesktopRunning);
	CHECK(cordmux::pipeline::startup_rewrite_waits_for_vesktop(r.outcome));
	CHECK(fs.writes.empty());
	CHECK(fs.files[kStatePath] == state_before);
	CHECK(fs.files[kBackupPath] == backup_before);

	// 起動しているかを調べられないときも、書かずに待つ。
	fs.vesktop_running = std::nullopt;
	r = run_startup_vencord_dir_rewrite(moved_inputs(), ops_for(fs));
	CHECK(r.outcome == StartupRewriteOutcome::VesktopUnknown);
	CHECK(cordmux::pipeline::startup_rewrite_waits_for_vesktop(r.outcome));
	CHECK(fs.writes.empty());

	// 確認する関数を渡し忘れたときも、調べられないものとして書かない。
	StartupRewriteOps no_check = ops_for(fs);
	no_check.vesktop_running = nullptr;
	CHECK(run_startup_vencord_dir_rewrite(moved_inputs(), no_check).outcome == StartupRewriteOutcome::VesktopUnknown);
	CHECK(fs.writes.empty());

	// Vesktop が終了した後にもう一度呼ぶと、書く。
	fs.vesktop_running = false;
	r = run_startup_vencord_dir_rewrite(moved_inputs(), ops_for(fs));
	CHECK(r.outcome == StartupRewriteOutcome::Wrote);
	CHECK_FALSE(cordmux::pipeline::startup_rewrite_waits_for_vesktop(r.outcome));
	CHECK(cordmux::platform::parse_vencord_dir(fs.files[kStatePath]) == std::optional<std::string>(kNewBundle));
}

TEST_CASE("起動時の書き直しで、書く必要がないときは Vesktop を確認しない (待たずに終わる)")
{
	// もう同梱の場所を指している、バックアップがない、設定で書き込みを止めている、のどれも、Vesktop の状態によらず
	// 書かない。待つ結果 (VesktopRunning) を返すと、ドックが Vesktop の終了のたびに書き直しを試し続ける。
	FakeFiles ours = moved_files();
	ours.files[kStatePath] = state_with(kNewBundle);
	ours.vesktop_running = true;
	CHECK(run_startup_vencord_dir_rewrite(moved_inputs(), ops_for(ours)).outcome == StartupRewriteOutcome::AlreadyOurs);
	CHECK(ours.vesktop_checks == 0);

	FakeFiles none = moved_files();
	none.files.erase(kBackupPath);
	none.vesktop_running = true;
	CHECK(run_startup_vencord_dir_rewrite(moved_inputs(), ops_for(none)).outcome == StartupRewriteOutcome::NotThisScene);
	CHECK(none.vesktop_checks == 0);

	FakeFiles off = moved_files();
	off.vesktop_running = true;
	StartupRewriteInputs in = moved_inputs();
	in.allow_state_json_write = false;
	CHECK(run_startup_vencord_dir_rewrite(in, ops_for(off)).outcome == StartupRewriteOutcome::BlockedBySetting);
	CHECK(off.vesktop_checks == 0);
}

TEST_CASE("起動時の書き直しは、大文字と小文字や末尾の区切りだけが違う値も、前に書いた場所として書き直す")
{
	// 前に書いた値と文字は違うが同じ場所 (大文字と小文字、末尾の区切りだけが違う) も、前に書いた場所として扱う。
	FakeFiles fs = moved_files();
	fs.files[kStatePath] = state_with("c:/OLD-PLACE/cordmux/data/vencord/");
	const StartupRewriteResult r = run_startup_vencord_dir_rewrite(moved_inputs(), ops_for(fs));
	CHECK(r.outcome == StartupRewriteOutcome::Wrote);
	CHECK(cordmux::platform::parse_vencord_dir(fs.files[kStatePath]) == std::optional<std::string>(kNewBundle));
}

TEST_CASE("起動時の書き直しは、バックアップがなければ書かず、壊れたバックアップもないものとして扱う")
{
	SUBCASE("バックアップのファイルがない")
	{
		FakeFiles fs;
		fs.files[kStatePath] = state_with(kUserOriginal);
		const StartupRewriteResult r = run_startup_vencord_dir_rewrite(moved_inputs(), ops_for(fs));
		CHECK(r.outcome == StartupRewriteOutcome::NotThisScene);
		CHECK(fs.writes.empty());
	}
	SUBCASE("バックアップのファイルも vencordDir のキーもない")
	{
		FakeFiles fs;
		fs.files[kStatePath] = state_without_key();
		const StartupRewriteResult r = run_startup_vencord_dir_rewrite(moved_inputs(), ops_for(fs));
		CHECK(r.outcome == StartupRewriteOutcome::NotThisScene);
		CHECK(fs.writes.empty());
	}
	SUBCASE("壊れたバックアップのファイルはないものとして扱う")
	{
		FakeFiles fs = moved_files();
		fs.files[kBackupPath] = "{ this is not the backup";
		const StartupRewriteResult r = run_startup_vencord_dir_rewrite(moved_inputs(), ops_for(fs));
		CHECK(r.outcome == StartupRewriteOutcome::NotThisScene);
		CHECK(fs.writes.empty());
		CHECK(fs.files[kStatePath] == state_with(kOldBundle));
	}
}

TEST_CASE("起動時の書き直しは、利用者の元の値に戻っていれば書かない")
{
	SUBCASE("元の値がほかの場所")
	{
		FakeFiles fs = moved_files();
		fs.files[kStatePath] = state_with(kUserOriginal);
		const StartupRewriteResult r = run_startup_vencord_dir_rewrite(moved_inputs(), ops_for(fs));
		CHECK(r.outcome == StartupRewriteOutcome::NotThisScene);
		CHECK(fs.writes.empty());
	}
	SUBCASE("元はキーがなかった")
	{
		FakeFiles fs;
		fs.files[kStatePath] = state_without_key();
		fs.files[kBackupPath] = cordmux::install::backup_to_json(old_backup(std::nullopt));
		const StartupRewriteResult r = run_startup_vencord_dir_rewrite(moved_inputs(), ops_for(fs));
		CHECK(r.outcome == StartupRewriteOutcome::NotThisScene);
		CHECK(fs.writes.empty());
	}
}

TEST_CASE("state.json がもう同梱の場所を指していれば、起動時の書き直しは何もしない")
{
	FakeFiles fs = moved_files();
	fs.files[kStatePath] = state_with(kNewBundle);
	const StartupRewriteResult r = run_startup_vencord_dir_rewrite(moved_inputs(), ops_for(fs));
	CHECK(r.outcome == StartupRewriteOutcome::AlreadyOurs);
	CHECK(fs.writes.empty());
}

TEST_CASE("利用者かほかのツールが変えた値は、起動時の書き直しで変えない")
{
	FakeFiles fs = moved_files();
	fs.files[kStatePath] = state_with(kForeign);
	const StartupRewriteResult r = run_startup_vencord_dir_rewrite(moved_inputs(), ops_for(fs));
	CHECK(r.outcome == StartupRewriteOutcome::Refused);
	CHECK(r.refuse_reason == cordmux::install::RefuseReason::ForeignChange);
	CHECK(fs.writes.empty());
	CHECK(fs.files[kStatePath] == state_with(kForeign));
}

TEST_CASE("壊れた state.json は、理由を返して書かない")
{
	FakeFiles fs = moved_files();
	fs.files[kStatePath] = "{\"vencordDir\": ";
	const StartupRewriteResult r = run_startup_vencord_dir_rewrite(moved_inputs(), ops_for(fs));
	CHECK(r.outcome == StartupRewriteOutcome::Refused);
	CHECK(r.refuse_reason == cordmux::install::RefuseReason::BrokenJson);
	CHECK(fs.writes.empty());
}

TEST_CASE("アンインストールの最中は、何も読まずに起動時の書き直しをしない")
{
	FakeFiles fs = moved_files();
	StartupRewriteInputs in = moved_inputs();
	in.uninstalling = true;
	const StartupRewriteResult r = run_startup_vencord_dir_rewrite(in, ops_for(fs));
	CHECK(r.outcome == StartupRewriteOutcome::SkippedUninstalling);
	CHECK(fs.reads == 0);
	CHECK(fs.writes.empty());
}

TEST_CASE("cordmux が無効なら、起動時の書き直しをしない")
{
	FakeFiles fs = moved_files();
	StartupRewriteInputs in = moved_inputs();
	in.feature_enabled = false;
	const StartupRewriteResult r = run_startup_vencord_dir_rewrite(in, ops_for(fs));
	CHECK(r.outcome == StartupRewriteOutcome::SkippedFeatureOff);
	CHECK(fs.reads == 0);
	CHECK(fs.writes.empty());
}

TEST_CASE("allow_state_json_write が false なら、書く計算はしても何も書かない")
{
	FakeFiles fs = moved_files();
	StartupRewriteInputs in = moved_inputs();
	in.allow_state_json_write = false;
	const StartupRewriteResult r = run_startup_vencord_dir_rewrite(in, ops_for(fs));
	CHECK(r.outcome == StartupRewriteOutcome::BlockedBySetting);
	CHECK(fs.writes.empty());
	CHECK(fs.files[kStatePath] == state_with(kOldBundle));
}

TEST_CASE("state.json の場所が分からないか読めないときは、書かずに戻る")
{
	SUBCASE("場所が分からない")
	{
		FakeFiles fs = moved_files();
		fs.state_path_resolvable = false;
		const StartupRewriteResult r = run_startup_vencord_dir_rewrite(moved_inputs(), ops_for(fs));
		CHECK(r.outcome == StartupRewriteOutcome::StateJsonUnread);
		CHECK(fs.writes.empty());
	}
	SUBCASE("ファイルがない (Vesktop を一度も起動していない)")
	{
		FakeFiles fs = moved_files();
		fs.files.erase(kStatePath);
		const StartupRewriteResult r = run_startup_vencord_dir_rewrite(moved_inputs(), ops_for(fs));
		CHECK(r.outcome == StartupRewriteOutcome::StateJsonUnread);
		CHECK(fs.writes.empty());
	}
	SUBCASE("ファイルが上限より大きい")
	{
		FakeFiles fs = moved_files();
		StartupRewriteInputs in = moved_inputs();
		in.max_json_bytes = 8;
		const StartupRewriteResult r = run_startup_vencord_dir_rewrite(in, ops_for(fs));
		CHECK(r.outcome == StartupRewriteOutcome::StateJsonUnread);
		CHECK(fs.writes.empty());
	}
}

TEST_CASE("書きたい場所かバックアップの場所が分からなければ、何も読まない")
{
	SUBCASE("書きたい場所が空")
	{
		FakeFiles fs = moved_files();
		StartupRewriteInputs in = moved_inputs();
		in.target_dir.clear();
		CHECK(run_startup_vencord_dir_rewrite(in, ops_for(fs)).outcome == StartupRewriteOutcome::NoTarget);
		CHECK(fs.reads == 0);
	}
	SUBCASE("バックアップの場所が空")
	{
		FakeFiles fs = moved_files();
		StartupRewriteInputs in = moved_inputs();
		in.backup_json_path.clear();
		CHECK(run_startup_vencord_dir_rewrite(in, ops_for(fs)).outcome == StartupRewriteOutcome::NoTarget);
		CHECK(fs.reads == 0);
	}
}

TEST_CASE("バックアップを書けなければ、state.json に触らない")
{
	FakeFiles fs = moved_files();
	fs.fail_writes.insert(kBackupPath);
	const StartupRewriteResult r = run_startup_vencord_dir_rewrite(moved_inputs(), ops_for(fs));
	CHECK(r.outcome == StartupRewriteOutcome::BackupWriteFailed);
	REQUIRE(fs.writes.size() == 1);
	CHECK(fs.writes[0] == kBackupPath);
	CHECK(fs.files[kStatePath] == state_with(kOldBundle));
}

TEST_CASE("state.json を書けなければ、バックアップを書く前の中身に戻す")
{
	FakeFiles fs = moved_files();
	const std::string backup_before = fs.files[kBackupPath];
	fs.fail_writes.insert(kStatePath);
	const StartupRewriteResult r = run_startup_vencord_dir_rewrite(moved_inputs(), ops_for(fs));
	CHECK(r.outcome == StartupRewriteOutcome::StateWriteFailed);
	CHECK(r.backup_restored);
	CHECK(fs.files[kBackupPath] == backup_before);
	CHECK(fs.files[kStatePath] == state_with(kOldBundle));
	REQUIRE(fs.writes.size() == 3);
	CHECK(fs.writes[0] == kBackupPath);
	CHECK(fs.writes[1] == kStatePath);
	CHECK(fs.writes[2] == kBackupPath);

	// 戻したので、次の起動は同じ書き直しをもう一度試す (利用者かほかのツールが変えた値に見えない)。
	fs.fail_writes.clear();
	fs.writes.clear();
	const StartupRewriteResult again = run_startup_vencord_dir_rewrite(moved_inputs(), ops_for(fs));
	CHECK(again.outcome == StartupRewriteOutcome::Wrote);
}

TEST_CASE("起動時の書き直しの結果と拒んだ理由には、それぞれ別のログの語句がある")
{
	const StartupRewriteOutcome outcomes[] = {
		StartupRewriteOutcome::SkippedUninstalling, StartupRewriteOutcome::SkippedFeatureOff,
		StartupRewriteOutcome::NoTarget,            StartupRewriteOutcome::StateJsonUnread,
		StartupRewriteOutcome::AlreadyOurs,         StartupRewriteOutcome::NotThisScene,
		StartupRewriteOutcome::Refused,             StartupRewriteOutcome::BlockedBySetting,
		StartupRewriteOutcome::BackupWriteFailed,   StartupRewriteOutcome::StateWriteFailed,
		StartupRewriteOutcome::Wrote,
	};
	std::set<std::string> names;
	for (const StartupRewriteOutcome outcome : outcomes) {
		const std::string name = cordmux::pipeline::startup_rewrite_outcome_name(outcome);
		CHECK(name != "unknown");
		names.insert(name);
	}
	CHECK(names.size() == sizeof(outcomes) / sizeof(outcomes[0]));

	const cordmux::install::RefuseReason reasons[] = {
		cordmux::install::RefuseReason::BrokenJson,          cordmux::install::RefuseReason::NotObject,
		cordmux::install::RefuseReason::VencordDirNotString, cordmux::install::RefuseReason::ForeignChange,
		cordmux::install::RefuseReason::SceneNotAllowed,
	};
	std::set<std::string> reason_names;
	for (const cordmux::install::RefuseReason reason : reasons) {
		const std::string name = cordmux::pipeline::refuse_reason_name(reason);
		CHECK(name != "unknown");
		reason_names.insert(name);
	}
	CHECK(reason_names.size() == sizeof(reasons) / sizeof(reasons[0]));
}

// ---------------------------------------------------------------------------
// media
// ---------------------------------------------------------------------------

TEST_CASE("cordmux が無効なら、ほかの値にかかわらず media は false")
{
	for (int bits = 0; bits < 8; ++bits) {
		MediaInputs in;
		in.feature_enabled = false;
		in.loading_finished = (bits & 1) != 0;
		in.cordmux_sources_present = (bits & 2) != 0;
		in.recording_on = (bits & 4) != 0;
		CHECK_FALSE(decide_media(in));
	}
}

TEST_CASE("cordmux のソースがあるか録画をしていれば media は true で、どちらもなければ false")
{
	MediaInputs in;
	in.feature_enabled = true;
	in.loading_finished = true;

	in.cordmux_sources_present = false;
	in.recording_on = false;
	CHECK_FALSE(decide_media(in)); // ソースも録画もないので、送信側は読み出しもエンコードもしない

	in.cordmux_sources_present = true;
	CHECK(decide_media(in));

	in.cordmux_sources_present = false;
	in.recording_on = true;
	CHECK(decide_media(in)); // 録画は OBS のソースを通らないので、ソースがなくても true

	in.cordmux_sources_present = true;
	CHECK(decide_media(in));
}

TEST_CASE("シーンコレクションを読み終えるまで、media は false")
{
	MediaInputs in;
	in.feature_enabled = true;
	in.loading_finished = false;
	in.cordmux_sources_present = true; // 読み込みの前の値は信じない
	in.recording_on = false;
	CHECK_FALSE(decide_media(in));
}

TEST_CASE("any_cordmux_source は、合成のソースがあるときだけ true を返す")
{
	CHECK_FALSE(any_cordmux_source(LayoutPresence{}));
	LayoutPresence composite;
	composite.composite = true;
	CHECK(any_cordmux_source(composite));
}

TEST_CASE("合成のソースが現れたら media は true になり、消えたら false に戻る")
{
	// 録画をしていない、cordmux が有効な、シーンコレクションを読み終えた後の 1 秒ごとの調べ直し
	// (pipeline.cpp の refresh_layout_presence) の順に、調べた値を渡す。
	LayoutPresence presence;
	CHECK_FALSE(decide_media(media_inputs_of(true, true, presence, false)));

	presence.composite = true; // 利用者が OBS の [+] で合成のソースを追加した
	CHECK(decide_media(media_inputs_of(true, true, presence, false)));

	presence.composite = false; // 合成のソースを削除した
	CHECK_FALSE(decide_media(media_inputs_of(true, true, presence, false)));

	// 録画をしている間は、合成のソースがなくても読み出させる。
	CHECK(decide_media(media_inputs_of(true, true, presence, true)));
}

TEST_CASE("合成のソースの有無をまだ調べていない間は、読み込みが終わっていても media は false")
{
	const std::optional<LayoutPresence> unread;
	const MediaInputs in = media_inputs_of(true, true, unread, false);
	CHECK_FALSE(in.loading_finished);
	CHECK_FALSE(in.cordmux_sources_present);
	CHECK_FALSE(decide_media(in));
	// 調べた値があっても、シーンコレクションを読み終える前は false (decide_media の約束)。
	LayoutPresence composite;
	composite.composite = true;
	CHECK_FALSE(decide_media(media_inputs_of(true, false, composite, false)));
	// cordmux を無効にしていれば、合成のソースがあっても false。
	CHECK_FALSE(decide_media(media_inputs_of(false, true, composite, false)));
}

// ---------------------------------------------------------------------------
// limits
// ---------------------------------------------------------------------------

TEST_CASE("既定の設定では、limits は空のオブジェクトになる")
{
	const cordmux::ws::LimitsMessage m = limits_from_settings(0, 1);
	CHECK_FALSE(m.max_long_edge.has_value());
	CHECK_FALSE(m.fps_divisor.has_value());
	CHECK(m == cordmux::ws::LimitsMessage{});
}

TEST_CASE("limits のキーは、制限があるときだけ入る")
{
	const cordmux::ws::LimitsMessage edge = limits_from_settings(1280, 1);
	CHECK(edge.max_long_edge == std::optional<std::uint32_t>(1280));
	CHECK_FALSE(edge.fps_divisor.has_value());

	const cordmux::ws::LimitsMessage half = limits_from_settings(0, 2);
	CHECK_FALSE(half.max_long_edge.has_value());
	CHECK(half.fps_divisor == std::optional<int>(2));

	const cordmux::ws::LimitsMessage both = limits_from_settings(640, 2);
	CHECK(both.max_long_edge == std::optional<std::uint32_t>(640));
	CHECK(both.fps_divisor == std::optional<int>(2));
}

TEST_CASE("設定の範囲の外の値からも、ワイヤー形式の範囲の外の limits は作らない")
{
	const int edges[] = {-1, 0, 16, 1920, 7680, INT_MAX};
	const int fps[] = {-1, 0, 1, 2, 3};
	for (const int e : edges) {
		for (const int f : fps)
			CHECK(cordmux::ws::limits_in_range(limits_from_settings(e, f)));
	}
	CHECK(limits_from_settings(-1, 3) == cordmux::ws::LimitsMessage{});
}

TEST_CASE("エンコードの設定は、既定値ではキーを省き、既定と違うときだけ limits に入る")
{
	using cordmux::config::EncodePriority;
	using cordmux::config::EncoderPreference;
	// 既定 (ハードウェア、速さ) は、何も送らないときの {} と同じ形になる。
	CHECK(limits_from_settings(0, 1, EncoderPreference::Hardware, EncodePriority::Speed) ==
	      cordmux::ws::LimitsMessage{});

	const cordmux::ws::LimitsMessage software = limits_from_settings(0, 1, EncoderPreference::Software, EncodePriority::Speed);
	CHECK(software.encoder_preference == std::optional<std::string>("software"));
	CHECK_FALSE(software.latency_mode.has_value());

	const cordmux::ws::LimitsMessage quality = limits_from_settings(0, 1, EncoderPreference::Hardware, EncodePriority::Quality);
	CHECK_FALSE(quality.encoder_preference.has_value());
	CHECK(quality.latency_mode == std::optional<std::string>("quality"));

	// 画質の上限と同時に変えても、4 つのキーが別々に入る。
	const cordmux::ws::LimitsMessage all = limits_from_settings(1280, 2, EncoderPreference::Software, EncodePriority::Quality);
	CHECK(all.max_long_edge == std::optional<std::uint32_t>(1280));
	CHECK(all.fps_divisor == std::optional<int>(2));
	CHECK(all.encoder_preference == std::optional<std::string>("software"));
	CHECK(all.latency_mode == std::optional<std::string>("quality"));
	CHECK(cordmux::ws::limits_in_range(all));

	// エンコードの設定だけが違っても、前に送った値と比べて違うと分かる (set_limits が送り直す条件)。
	CHECK(software != cordmux::ws::LimitsMessage{});
	CHECK(quality != software);
}

// ---------------------------------------------------------------------------
// settings.json の読み込み
// ---------------------------------------------------------------------------

TEST_CASE("settings.json がないことと、あるのに読めないことを区別する")
{
	const std::string json = "{\"feature_enabled\":true}";
	CHECK(classify_settings_file(true, json, false, false) == SettingsFileState::Read);
	CHECK(classify_settings_file(true, json, true, true) == SettingsFileState::Read);
	// フォルダーごとない (初めての起動)。
	CHECK(classify_settings_file(false, "", false, false) == SettingsFileState::Absent);
	// フォルダーはあるが、ファイルがない。
	CHECK(classify_settings_file(false, "", true, false) == SettingsFileState::Absent);
	// あるのに読めない (開けない、大きすぎる)。書かない。
	CHECK(classify_settings_file(false, "", true, true) == SettingsFileState::Unreadable);
}

// 0 バイトか空白だけの settings.json は、ないのと同じに扱う (利用者の指定が入っていない。pipeline.h の
// classify_settings_file)。「読めない」にすると、以後の起動でドックが設定を書かず、利用者が手で消すまで設定が
// 保存されない。
TEST_CASE("空か空白だけの settings.json は、ないものとして扱う")
{
	CHECK(classify_settings_file(true, "", true, true) == SettingsFileState::Absent);
	CHECK(classify_settings_file(true, " \r\n\t\n", true, true) == SettingsFileState::Absent);
	// 空白以外の文字があれば、読めたものとして parse_settings に渡す (壊れていれば parse_settings が拒む)。
	CHECK(classify_settings_file(true, "{", true, true) == SettingsFileState::Read);
	CHECK(classify_settings_file(true, " x ", true, true) == SettingsFileState::Read);
	// 読めなかったときは中身を見ない (読み込みの関数が out を変えないので、空とは限らない)。
	CHECK(classify_settings_file(false, "{}", true, true) == SettingsFileState::Unreadable);
}

// settings.json があるのに読めなかったときは、Vesktop の設定を書かない (設定で書いた false を、既定値の
// true で上書きしない。pipeline.h の effective_allow_state_json_write)。
TEST_CASE("settings.json を読めなかったときは、state.json を書かない")
{
	using cordmux::pipeline::effective_allow_state_json_write;
	CHECK(effective_allow_state_json_write(true, false));
	CHECK_FALSE(effective_allow_state_json_write(false, false));
	CHECK_FALSE(effective_allow_state_json_write(true, true));
	CHECK_FALSE(effective_allow_state_json_write(false, true));
}

// ---------------------------------------------------------------------------
// OsProbe の確認の間隔 (platform/os_probe.h)
// ---------------------------------------------------------------------------

TEST_CASE("probe_interval_ms_from_seconds は、設定の秒をミリ秒にする")
{
	using cordmux::platform::probe_interval_ms_from_seconds;
	CHECK(probe_interval_ms_from_seconds(30, 1) == 30000);
	CHECK(probe_interval_ms_from_seconds(300, 1) == 300000);
	CHECK(probe_interval_ms_from_seconds(1, 7) == 1000);
	CHECK(probe_interval_ms_from_seconds(86400, 1) == 86400000);
}

TEST_CASE("probe_interval_ms_from_seconds は、0 以下なら既定の間隔を返し、大きすぎる値を int に収まる値にする")
{
	using cordmux::platform::probe_interval_ms_from_seconds;
	CHECK(probe_interval_ms_from_seconds(0, 30000) == 30000);
	CHECK(probe_interval_ms_from_seconds(-5, 300000) == 300000);
	const int huge = probe_interval_ms_from_seconds(INT_MAX, 1);
	CHECK(huge > 0);
	CHECK(huge % 1000 == 0);
	CHECK(huge == (INT_MAX / 1000) * 1000);
}

TEST_CASE("起動時の書き直しは、書いた state.json の場所をバックアップに残す")
{
	FakeFiles fs = moved_files();
	REQUIRE(run_startup_vencord_dir_rewrite(moved_inputs(), ops_for(fs)).outcome == StartupRewriteOutcome::Wrote);
	const auto b = backup_in(fs);
	REQUIRE(b.has_value());
	CHECK(b->state_path == kStatePath);
}

TEST_CASE("起動時の書き直しは、バックアップに書いた場所があれば、現在の場所ではなくそこを書き直す")
{
	const std::string recorded = "fake-other/Vesktop/state.json";
	FakeFiles fs;
	cordmux::install::Backup backup = old_backup();
	backup.state_path = recorded;
	fs.files[kBackupPath] = cordmux::install::backup_to_json(backup);
	fs.files[recorded] = state_with(kOldBundle);
	fs.files[kStatePath] = state_with(kForeign); // 現在の場所の state.json は別の Vesktop の設定 (触らない)
	const StartupRewriteResult r = run_startup_vencord_dir_rewrite(moved_inputs(), ops_for(fs));
	CHECK(r.outcome == StartupRewriteOutcome::Wrote);
	CHECK(cordmux::platform::parse_vencord_dir(fs.files[recorded]) == std::optional<std::string>(kNewBundle));
	CHECK(fs.files[kStatePath] == state_with(kForeign));
	const auto b = backup_in(fs);
	REQUIRE(b.has_value());
	CHECK(b->state_path == recorded);
}
