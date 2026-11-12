// SPDX-License-Identifier: Apache-2.0

// install/backup_first_write.h (バックアップを先に書き、state.json を書けなかったらバックアップを元に戻す手順) の
// 単体テスト。ファイルは偽の表で、場所は利用者のプロファイルに似せない。

#include "doctest.h"

#include "install/backup_first_write.h"
#include "install/vencord_dir.h"
#include "platform/bundle_check.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using cordmux::install::BackupFirstOps;
using cordmux::install::BackupFirstOutcome;
using cordmux::install::BackupFirstResult;
using cordmux::install::write_backup_then_state;

namespace {

const std::string kBackupPath = "fake-obs/plugin_config/cordmux/vencord_dir_backup.json";
const std::string kStatePath = "fake-profile/Vesktop/state.json";
const std::string kOldBundle = "C:/old-place/cordmux/data/vencord";
const std::string kNewBundle = "D:/new-place/cordmux/data/vencord";
const std::string kUserOriginal = "C:/someone/Vencord/dist";

struct Files {
	std::map<std::string, std::string> files;
	std::vector<std::string> writes; // 書こうとした場所 (順番どおり。失敗したものも入る)
	std::vector<std::string> deletes;
	std::set<std::string> fail_writes;
	std::size_t writes_before_fail_limit = 0; // 0 でなければ、この回数目以降の書き込みを失敗させる
};

BackupFirstOps ops_for(Files &fs, bool with_delete = true)
{
	BackupFirstOps ops;
	ops.read_file_bytes = [&fs](const std::string &path, std::string &out, std::size_t max_bytes) {
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
		if (fs.writes_before_fail_limit != 0 && fs.writes.size() >= fs.writes_before_fail_limit)
			return false;
		fs.files[path] = text;
		return true;
	};
	if (with_delete) {
		ops.delete_file = [&fs](const std::string &path) {
			fs.deletes.push_back(path);
			return fs.files.erase(path) != 0;
		};
		ops.file_known_absent = [&fs](const std::string &path) { return fs.files.count(path) == 0; };
	}
	return ops;
}

BackupFirstResult run(const BackupFirstOps &ops)
{
	return write_backup_then_state(ops, 1u << 20, kBackupPath, "NEW-BACKUP", kStatePath, "NEW-STATE");
}

} // namespace

TEST_CASE("バックアップと state.json の両方を書けたら、その順に書く")
{
	Files fs;
	fs.files[kBackupPath] = "OLD-BACKUP";
	fs.files[kStatePath] = "OLD-STATE";
	const BackupFirstResult r = run(ops_for(fs));
	CHECK(r.outcome == BackupFirstOutcome::Wrote);
	CHECK(fs.files[kBackupPath] == "NEW-BACKUP");
	CHECK(fs.files[kStatePath] == "NEW-STATE");
	REQUIRE(fs.writes.size() == 2);
	CHECK(fs.writes[0] == kBackupPath);
	CHECK(fs.writes[1] == kStatePath);
	CHECK(fs.deletes.empty());
}

TEST_CASE("バックアップを書けなければ、state.json に触らない")
{
	Files fs;
	fs.files[kBackupPath] = "OLD-BACKUP";
	fs.files[kStatePath] = "OLD-STATE";
	fs.fail_writes.insert(kBackupPath);
	const BackupFirstResult r = run(ops_for(fs));
	CHECK(r.outcome == BackupFirstOutcome::BackupWriteFailed);
	REQUIRE(fs.writes.size() == 1);
	CHECK(fs.writes[0] == kBackupPath);
	CHECK(fs.files[kBackupPath] == "OLD-BACKUP");
	CHECK(fs.files[kStatePath] == "OLD-STATE");
}

TEST_CASE("state.json を書けず、前のバックアップがあれば、前の中身にそのまま戻す")
{
	Files fs;
	fs.files[kBackupPath] = "OLD-BACKUP";
	fs.files[kStatePath] = "OLD-STATE";
	fs.fail_writes.insert(kStatePath);
	const BackupFirstResult r = run(ops_for(fs));
	CHECK(r.outcome == BackupFirstOutcome::StateWriteFailed);
	CHECK(r.backup_restored);
	CHECK(fs.files[kBackupPath] == "OLD-BACKUP");
	CHECK(fs.files[kStatePath] == "OLD-STATE");
	REQUIRE(fs.writes.size() == 3);
	CHECK(fs.writes[0] == kBackupPath);
	CHECK(fs.writes[1] == kStatePath);
	CHECK(fs.writes[2] == kBackupPath);
	CHECK(fs.deletes.empty());
}

TEST_CASE("state.json を書けず、前のバックアップがなければ、書いたバックアップを消す")
{
	Files fs;
	fs.files[kStatePath] = "OLD-STATE";
	fs.fail_writes.insert(kStatePath);
	const BackupFirstResult r = run(ops_for(fs));
	CHECK(r.outcome == BackupFirstOutcome::StateWriteFailed);
	CHECK(r.backup_restored);
	CHECK(fs.files.count(kBackupPath) == 0);
	REQUIRE(fs.deletes.size() == 1);
	CHECK(fs.deletes[0] == kBackupPath);
	CHECK(fs.files[kStatePath] == "OLD-STATE");
}

TEST_CASE("state.json を書けず、戻す書き込みも失敗したら、戻せなかったと返す")
{
	Files fs;
	fs.files[kBackupPath] = "OLD-BACKUP";
	fs.files[kStatePath] = "OLD-STATE";
	fs.fail_writes.insert(kStatePath);
	fs.writes_before_fail_limit = 3; // 3 回目 (戻す書き込み) から失敗させる
	const BackupFirstResult r = run(ops_for(fs));
	CHECK(r.outcome == BackupFirstOutcome::StateWriteFailed);
	CHECK_FALSE(r.backup_restored);
	CHECK(fs.files[kBackupPath] == "NEW-BACKUP");
}

TEST_CASE("書く前にバックアップを読めなかった理由が分からなければ、戻せなかったと返し、消さない")
{
	SUBCASE("ファイルはあるが上限より大きい")
	{
		Files fs;
		fs.files[kBackupPath] = std::string(64, 'x');
		fs.fail_writes.insert(kStatePath);
		const BackupFirstResult r =
			write_backup_then_state(ops_for(fs), 8, kBackupPath, "NEW", kStatePath, "NEW-STATE");
		CHECK(r.outcome == BackupFirstOutcome::StateWriteFailed);
		CHECK_FALSE(r.backup_restored);
		CHECK(fs.deletes.empty());
	}
	SUBCASE("ファイルがないと確認する関数を渡していない")
	{
		Files fs;
		fs.fail_writes.insert(kStatePath);
		const BackupFirstResult r = run(ops_for(fs, false));
		CHECK(r.outcome == BackupFirstOutcome::StateWriteFailed);
		CHECK_FALSE(r.backup_restored);
		CHECK(fs.deletes.empty());
	}
}

// ---------------------------------------------------------------------------
// [Vesktopに追加] の経路で、cordmux のフォルダーを移した後に state.json を書けなかった場面。
// ---------------------------------------------------------------------------

TEST_CASE("state.json を書けなかった押し直しで、利用者の元の値が cordmux の古い場所に置き換わらない")
{
	using namespace cordmux::install;

	// 前のバックアップ {previous = 利用者の元の値, written = 古い場所}。state.json は古い場所のまま (分岐 3)。
	Backup old_backup;
	old_backup.previous = kUserOriginal;
	old_backup.written = kOldBundle;
	old_backup.written_at_us = 1000;
	old_backup.dll_dir = "C:/old-place/cordmux";
	const std::string state_old = std::string(R"({"vencordDir":")") + kOldBundle + R"("})";

	Files fs;
	fs.files[kBackupPath] = backup_to_json(old_backup);
	fs.files[kStatePath] = state_old;

	const auto press = [&fs]() {
		std::optional<Backup> current;
		Backup parsed;
		if (backup_from_json(fs.files[kBackupPath], &parsed))
			current = parsed;
		return plan_write(fs.files[kStatePath], kNewBundle, current, 5000, "D:/new-place/cordmux",
				  WriteScene::Pressed);
	};

	// 1 回目: state.json を書けない。
	WritePlan first = press();
	REQUIRE(first.action == WriteAction::Write);
	REQUIRE(first.new_backup.has_value());
	CHECK(first.new_backup->previous == std::optional<std::string>(kUserOriginal));
	fs.fail_writes.insert(kStatePath);
	const BackupFirstResult r = write_backup_then_state(ops_for(fs), 1u << 20, kBackupPath,
							    backup_to_json(*first.new_backup), kStatePath, first.new_text);
	CHECK(r.outcome == BackupFirstOutcome::StateWriteFailed);
	CHECK(r.backup_restored);
	CHECK(fs.files[kBackupPath] == backup_to_json(old_backup));
	CHECK(fs.files[kStatePath] == state_old);

	// 2 回目: 書けるようにして押し直す。分岐 3 のままで、previous は利用者の元の値のまま。
	fs.fail_writes.clear();
	WritePlan second = press();
	REQUIRE(second.action == WriteAction::Write);
	REQUIRE(second.new_backup.has_value());
	CHECK(second.new_backup->previous == std::optional<std::string>(kUserOriginal));
	CHECK(second.new_backup->written == kNewBundle);
	const BackupFirstResult r2 = write_backup_then_state(ops_for(fs), 1u << 20, kBackupPath,
							     backup_to_json(*second.new_backup), kStatePath, second.new_text);
	CHECK(r2.outcome == BackupFirstOutcome::Wrote);
	CHECK(cordmux::platform::parse_vencord_dir(fs.files[kStatePath]) == std::optional<std::string>(kNewBundle));
}
