// SPDX-License-Identifier: Apache-2.0

// install/uninstall.h のファイルの段階 (1) と (3) の単体テスト。テスト用のファイルの関数 (メモリの中の
// フォルダー) で、消した回数、書いた中身、結果に入る残りを確認する。

#include "doctest.h"

#include "install/uninstall.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "platform/bundle_check.h"

using namespace cordmux::install;
using cordmux::platform::DirEntry;

namespace {

const char *const kCfg = "C:/obs/plugin_config/cordmux";
const char *const kBackup = "C:/obs/plugin_config/cordmux/vencord_dir_backup.json";
const char *const kLocal = "C:/local/cordmux";
const char *const kVesktop = "C:/roaming/Vesktop";
const char *const kState = "C:/roaming/Vesktop/state.json";
const char *const kBundled = "C:/plugins/cordmux/data/vencord";
const char *const kVencordSettings = "C:/roaming/Vesktop/settings/settings.json";

std::string parent_of(const std::string &path)
{
	const std::size_t cut = path.find_last_of('/');
	return cut == std::string::npos ? std::string() : path.substr(0, cut);
}

// メモリの中のフォルダー。場所は / 区切りの文字列のままキーにする (テストの中の場所は全部この形で書く)。
struct FakeFs {
	std::map<std::string, std::string> files;
	std::set<std::string> dirs;
	std::set<std::string> undeletable;
	std::set<std::string> unlistable;
	std::set<std::string> unwritable;
	bool have_state_path = true;
	std::string local_dir = kLocal;
	bool local_used_by_other = false;
	std::optional<bool> vesktop_running = false; // Vesktop が起動しているか (nullopt は調べられない)

	int deletes = 0;
	int vesktop_checks = 0;                   // Vesktop を確認した回数
	std::size_t writes_at_vesktop_check = 0; // 最後に Vesktop を確認した時点で書いた数
	int dir_removes = 0;
	std::vector<std::string> writes;          // 書いた場所
	std::map<std::string, std::string> written; // 書いた中身

	void add_dir(const std::string &dir)
	{
		std::string d = dir;
		while (!d.empty() && dirs.insert(d).second) {
			const std::string p = parent_of(d);
			if (p == d)
				break;
			d = p;
		}
	}
	void add_file(const std::string &path, const std::string &text)
	{
		add_dir(parent_of(path));
		files[path] = text;
	}
	bool has_children(const std::string &dir) const
	{
		for (const auto &f : files)
			if (parent_of(f.first) == dir)
				return true;
		for (const auto &d : dirs)
			if (parent_of(d) == dir)
				return true;
		return false;
	}

	UninstallFileOps ops()
	{
		UninstallFileOps o;
		o.delete_file = [this](const std::string &path) {
			if (dirs.count(path) != 0 || undeletable.count(path) != 0)
				return false;
			if (files.erase(path) != 0)
				++deletes;
			return true;
		};
		o.remove_directory_if_empty = [this](const std::string &dir) {
			if (dirs.count(dir) == 0)
				return true;
			if (has_children(dir) || undeletable.count(dir) != 0)
				return false;
			dirs.erase(dir);
			++dir_removes;
			return true;
		};
		o.list_directory = [this](const std::string &dir, std::vector<DirEntry> &out) {
			if (dirs.count(dir) == 0 || unlistable.count(dir) != 0)
				return false;
			std::vector<DirEntry> entries;
			for (const auto &f : files)
				if (parent_of(f.first) == dir)
					entries.push_back(DirEntry{f.first.substr(dir.size() + 1), false});
			for (const auto &d : dirs)
				if (parent_of(d) == dir)
					entries.push_back(DirEntry{d.substr(dir.size() + 1), true});
			out = entries;
			return true;
		};
		o.read_file_bytes = [this](const std::string &path, std::string &out, std::size_t max) {
			const auto it = files.find(path);
			if (it == files.end() || it->second.size() > max)
				return false;
			out = it->second;
			return true;
		};
		o.write_file_atomic = [this](const std::string &path, const std::string &text) {
			if (unwritable.count(path) != 0)
				return false;
			files[path] = text;
			writes.push_back(path);
			written[path] = text;
			return true;
		};
		o.vesktop_state_json_path = [this](std::string &out) {
			if (!have_state_path)
				return false;
			out = kState;
			return true;
		};
		o.vesktop_settings_json_path = [](std::string &out) {
			out = kVencordSettings;
			return true;
		};
		o.local_data_dir = [this] { return local_dir; };
		o.local_data_used_by_other = [this] { return local_used_by_other; };
		o.vesktop_running = [this] {
			++vesktop_checks;
			writes_at_vesktop_check = writes.size();
			return vesktop_running;
		};
		return o;
	}
};

UninstallContext context()
{
	UninstallContext c;
	c.module_config_dir = kCfg;
	c.backup_json_path = kBackup;
	c.max_json_bytes = 1 << 20;
	c.bundled_vencord_dir = kBundled;
	c.allow_state_json_write = true;
	return c;
}

// インストールした後の、普通の保存先の形 (cordmux が書く設定のファイル、バックアップ、トークンのファイル)。
// plugin_entry は、バックアップが持つ Vencord の設定の cordmux の項のバックアップ (既定は分からない)。
void seed_installed(FakeFs &fs, const PluginEntryBackup &plugin_entry = PluginEntryBackup{})
{
	Backup backup{std::string("C:/mine/dist"), kBundled, 123, "C:/plugins/cordmux"};
	backup.plugin_entry = plugin_entry;
	fs.add_file(std::string(kCfg) + "/settings.json", "{}");
	fs.add_file(std::string(kCfg) + "/seen_people.json", "[]");
	fs.add_file(std::string(kCfg) + "/dock_shown_once", "shown\n");
	fs.add_file(kBackup, backup_to_json(backup));
	fs.add_file(std::string(kLocal) + "/session.json", "{\"port\":1}");
	fs.add_dir(kVesktop);
	fs.add_file(kState, std::string("{\"vencordDir\":\"") + kBundled + "\",\"firstLaunch\":false}");
}

bool has_leftover(const UninstallStepResult &r, RemovalItem item, const std::string &where)
{
	for (const UninstallLeftover &l : r.leftovers)
		if (l.item == item && l.where == where)
			return true;
	return false;
}

// 計画を返すだけのテスト用の plan_restore (install/ の実際の処理によらず、段階の流れだけを確認する)。
std::function<RestorePlan(const std::string &, const Backup &)> plan_returning(RestoreAction action,
									       RefuseReason reason = RefuseReason::BrokenJson,
									       int *calls = nullptr)
{
	return [action, reason, calls](const std::string &, const Backup &) {
		if (calls != nullptr)
			++*calls;
		RestorePlan plan;
		plan.action = action;
		plan.refuse_reason = reason;
		plan.new_text = "{\"vencordDir\":\"C:/mine/dist\",\"firstLaunch\":false}";
		return plan;
	};
}

} // namespace

// ---- 段階 (1) 設定とトークン ----

TEST_CASE("段階 (1) は、設定のフォルダーにあるバックアップ以外のファイルと、トークンの保存先を全部消す")
{
	FakeFs fs;
	seed_installed(fs);
	fs.add_file(std::string(kCfg) + "/sub/note.txt", "x");

	const UninstallStepResult r = remove_settings_and_secret(fs.ops(), context());
	CHECK(r.leftovers.empty());
	// settings / seen_people / dock_shown_once / sub/note.txt / session.json の 5 つ。
	CHECK(fs.deletes == 5);
	// sub とトークンの保存先の 2 つ。設定のフォルダーはバックアップが残るので、段階 (3) まで消さない。
	CHECK(fs.dir_removes == 2);
	CHECK(fs.files.count(kBackup) == 1);
	CHECK(fs.dirs.count(kCfg) == 1);
	CHECK(fs.dirs.count(kLocal) == 0);
	CHECK(fs.files.count(kState) == 1); // Vesktop の設定には触らない
}

TEST_CASE("段階 (1) は、バックアップがなければ設定のフォルダーそのものも消す")
{
	FakeFs fs;
	fs.add_file(std::string(kCfg) + "/settings.json", "{}");
	const UninstallStepResult r = remove_settings_and_secret(fs.ops(), context());
	CHECK(r.leftovers.empty());
	CHECK(fs.dirs.count(kCfg) == 0);
}

TEST_CASE("段階 (1) で、もうないフォルダーは残りにしない (やり直しても同じ結果になる)")
{
	FakeFs fs;
	const UninstallStepResult r = remove_settings_and_secret(fs.ops(), context());
	CHECK(r.leftovers.empty());
	CHECK(fs.deletes == 0);
}

TEST_CASE("段階 (1) で消せないファイルは結果に入り、ほかのファイルは消える")
{
	FakeFs fs;
	seed_installed(fs);
	const std::string stuck = std::string(kCfg) + "/settings.json";
	fs.undeletable.insert(stuck);

	const UninstallStepResult r = remove_settings_and_secret(fs.ops(), context());
	REQUIRE(r.leftovers.size() == 1);
	CHECK(has_leftover(r, RemovalItem::ModuleConfigFiles, stuck));
	CHECK(fs.files.count(std::string(kCfg) + "/seen_people.json") == 0);
	CHECK(fs.dirs.count(kLocal) == 0); // トークンの行はそのまま進む
}

TEST_CASE("段階 (1) で、読めず中身が残っているフォルダーは、フォルダーとして結果に入る")
{
	FakeFs fs;
	seed_installed(fs);
	fs.unlistable.insert(kLocal);
	const UninstallStepResult r = remove_settings_and_secret(fs.ops(), context());
	REQUIRE(r.leftovers.size() == 1);
	CHECK(has_leftover(r, RemovalItem::LocalDataFolder, kLocal));
}

TEST_CASE("段階 (1) で、ほかの OBS が使っているトークンの保存先は、消さずに結果に入れる")
{
	FakeFs fs;
	seed_installed(fs);
	fs.local_used_by_other = true;
	const UninstallStepResult r = remove_settings_and_secret(fs.ops(), context());
	REQUIRE(r.leftovers.size() == 1);
	CHECK(has_leftover(r, RemovalItem::LocalDataFolder, kLocal));
	CHECK(fs.files.count(std::string(kLocal) + "/session.json") == 1);
}

TEST_CASE("段階 (1) で、トークンの保存先が分からないときは、場所なしで結果に入れる")
{
	FakeFs fs;
	seed_installed(fs);
	fs.local_dir.clear();
	const UninstallStepResult r = remove_settings_and_secret(fs.ops(), context());
	REQUIRE(r.leftovers.size() == 1);
	CHECK(has_leftover(r, RemovalItem::LocalDataFolder, ""));
}

TEST_CASE("段階 (1) でファイルの関数がないと、成功とせずに 2 つの行を結果に入れる")
{
	const UninstallStepResult r = remove_settings_and_secret(UninstallFileOps{}, context());
	CHECK(has_leftover(r, RemovalItem::ModuleConfigFiles, kCfg));
	CHECK(has_leftover(r, RemovalItem::LocalDataFolder, ""));
}

TEST_CASE("段階 (1) で、上限より深いフォルダーは、中へ入らずに結果に入れる")
{
	FakeFs fs;
	std::string deep = kCfg;
	for (int i = 0; i <= kMaxFolderDepth + 1; ++i)
		deep += "/d";
	fs.add_file(deep + "/x.txt", "x");
	const UninstallStepResult r = remove_settings_and_secret(fs.ops(), context());
	std::string limit_path = kCfg;
	for (int i = 0; i <= kMaxFolderDepth; ++i)
		limit_path += "/d";
	CHECK(has_leftover(r, RemovalItem::ModuleConfigFiles, limit_path));
	CHECK(fs.files.count(deep + "/x.txt") == 1);
}

// シンボリックリンクとジャンクションは中へ入らず、リンクそのものだけを消す (platform/platform.h の
// DirEntry::is_link の約束。消すのは cordmux のファイルだけにする)。テスト用の一覧の関数は、リンクの場所を渡すと
// 指す先の中身を返す (実際の list_directory と同じふるまい)。
namespace {

struct LinkedFs {
	FakeFs fs;
	std::string dir_link = std::string(kCfg) + "/junction";
	std::string file_link = std::string(kCfg) + "/file-link";
	std::set<std::string> links;
	std::set<std::string> stuck_links;
	std::vector<std::string> listed;
	std::vector<std::string> removed_links;
	std::vector<std::string> deleted_through_link;

	LinkedFs()
	{
		fs.add_file(std::string(kCfg) + "/settings.json", "{}");
		links = {dir_link, file_link};
	}

	UninstallFileOps ops()
	{
		const UninstallFileOps base = fs.ops();
		UninstallFileOps o = base;
		o.list_directory = [this, base](const std::string &dir, std::vector<DirEntry> &out) {
			listed.push_back(dir);
			if (dir == dir_link) {
				out = {DirEntry{"important.txt", false, false}};
				return true;
			}
			if (!base.list_directory(dir, out))
				return false;
			if (dir == kCfg) {
				if (links.count(dir_link) != 0)
					out.push_back(DirEntry{"junction", true, true});
				if (links.count(file_link) != 0)
					out.push_back(DirEntry{"file-link", false, true});
			}
			return true;
		};
		o.remove_directory_if_empty = [this, base](const std::string &dir) {
			if (links.count(dir) != 0) {
				if (stuck_links.count(dir) != 0)
					return false;
				links.erase(dir);
				removed_links.push_back(dir);
				return true;
			}
			return base.remove_directory_if_empty(dir);
		};
		o.delete_file = [this, base](const std::string &path) {
			if (path.rfind(dir_link + "/", 0) == 0) {
				deleted_through_link.push_back(path);
				return true;
			}
			if (links.count(path) != 0) {
				if (stuck_links.count(path) != 0)
					return false;
				links.erase(path);
				removed_links.push_back(path);
				return true;
			}
			return base.delete_file(path);
		};
		return o;
	}
};

} // namespace

TEST_CASE("段階 (1) で、リンクはリンクとして消し、中へは入らない")
{
	LinkedFs lfs;
	UninstallContext c = context();
	const UninstallStepResult r = remove_settings_and_secret(lfs.ops(), c);

	CHECK(r.leftovers.empty());
	CHECK(std::find(lfs.listed.begin(), lfs.listed.end(), lfs.dir_link) == lfs.listed.end());
	CHECK(lfs.deleted_through_link.empty());
	// フォルダーを指すリンクは remove_directory_if_empty で、ファイルを指すリンクは delete_file で消える。
	CHECK(std::find(lfs.removed_links.begin(), lfs.removed_links.end(), lfs.dir_link) != lfs.removed_links.end());
	CHECK(std::find(lfs.removed_links.begin(), lfs.removed_links.end(), lfs.file_link) != lfs.removed_links.end());
	CHECK(lfs.links.empty());
	CHECK(lfs.fs.files.count(std::string(kCfg) + "/settings.json") == 0);
}

TEST_CASE("段階 (1) で消せないリンクは結果に入り、中へは入らない")
{
	LinkedFs lfs;
	lfs.stuck_links.insert(lfs.dir_link);
	const UninstallStepResult r = remove_settings_and_secret(lfs.ops(), context());

	CHECK(has_leftover(r, RemovalItem::ModuleConfigFiles, lfs.dir_link));
	CHECK(std::find(lfs.listed.begin(), lfs.listed.end(), lfs.dir_link) == lfs.listed.end());
	CHECK(lfs.deleted_through_link.empty());
	CHECK(lfs.links.count(lfs.file_link) == 0); // 残りのリンクは消し続ける
}

// ---- 段階 (3) vencordDir を元に戻す ----

TEST_CASE("段階 (3) で計画が Write なら、計画の文字列を write_file_atomic で書き、バックアップと設定のフォルダーを消す")
{
	FakeFs fs;
	seed_installed(fs);
	remove_settings_and_secret(fs.ops(), context());
	UninstallFileOps ops = fs.ops();
	int calls = 0;
	ops.plan_restore = plan_returning(RestoreAction::Write, RefuseReason::BrokenJson, &calls);

	const UninstallStepResult r = restore_vencord_dir(ops, context());
	CHECK(r.leftovers.empty());
	CHECK(calls == 1);
	REQUIRE(fs.writes == std::vector<std::string>{kState});
	CHECK(fs.written[kState] == "{\"vencordDir\":\"C:/mine/dist\",\"firstLaunch\":false}");
	CHECK(fs.files.count(kBackup) == 0);
	CHECK(fs.dirs.count(kCfg) == 0);
}

TEST_CASE("段階 (3) で state.json の書き込みが許されていなければ、何も書かず、バックアップを残す")
{
	FakeFs fs;
	seed_installed(fs);
	UninstallFileOps ops = fs.ops();
	ops.plan_restore = plan_returning(RestoreAction::Write);
	UninstallContext ctx = context();
	ctx.allow_state_json_write = false;

	const UninstallStepResult r = restore_vencord_dir(ops, ctx);
	CHECK(has_leftover(r, RemovalItem::VencordDirValue, kState));
	CHECK(fs.writes.empty());
	CHECK(fs.files.count(kBackup) == 1);
}

TEST_CASE("UninstallContext の既定では state.json を書かない")
{
	CHECK_FALSE(UninstallContext{}.allow_state_json_write);
}

TEST_CASE("段階 (3) で書き込みに失敗すると結果に入れ、バックアップを残す")
{
	FakeFs fs;
	seed_installed(fs);
	fs.unwritable.insert(kState);
	UninstallFileOps ops = fs.ops();
	ops.plan_restore = plan_returning(RestoreAction::Write);
	const UninstallStepResult r = restore_vencord_dir(ops, context());
	CHECK(has_leftover(r, RemovalItem::VencordDirValue, kState));
	CHECK(fs.files.count(kBackup) == 1);
}

TEST_CASE("段階 (3) で計画が AlreadyRestored なら、何も書かず、バックアップを消す")
{
	FakeFs fs;
	seed_installed(fs);
	remove_settings_and_secret(fs.ops(), context());
	UninstallFileOps ops = fs.ops();
	ops.plan_restore = plan_returning(RestoreAction::AlreadyRestored);
	const UninstallStepResult r = restore_vencord_dir(ops, context());
	CHECK(r.leftovers.empty());
	CHECK(fs.writes.empty());
	CHECK(fs.files.count(kBackup) == 0);
}

TEST_CASE("段階 (3) で、インストールの後に利用者が選んだ値は、data/vencord を指していなければ残す")
{
	SUBCASE("ほかの場所を指す利用者の値は残りにしない")
	{
		FakeFs fs;
		seed_installed(fs);
		remove_settings_and_secret(fs.ops(), context());
		fs.files[kState] = "{\"vencordDir\":\"C:/other/dist\"}";
		UninstallFileOps ops = fs.ops();
		ops.plan_restore = plan_returning(RestoreAction::Refuse, RefuseReason::ForeignChange);
		const UninstallStepResult r = restore_vencord_dir(ops, context());
		CHECK(r.leftovers.empty());
		CHECK(fs.writes.empty());
		CHECK(fs.files.count(kBackup) == 0);
	}
	SUBCASE("data/vencord を指す値は結果に入れる (フォルダーを消すと公式の Vencord がダウンロードされる)")
	{
		FakeFs fs;
		seed_installed(fs);
		fs.files[kState] = "{\"vencordDir\":\"C:\\\\plugins\\\\cordmux\\\\data\\\\vencord\\\\\"}";
		UninstallFileOps ops = fs.ops();
		ops.plan_restore = plan_returning(RestoreAction::Refuse, RefuseReason::ForeignChange);
		const UninstallStepResult r = restore_vencord_dir(ops, context());
		CHECK(has_leftover(r, RemovalItem::VencordDirValue, kState));
		CHECK(fs.files.count(kBackup) == 1);
	}
}

TEST_CASE("段階 (3) で state.json が壊れていれば結果に入れ、バックアップを残す")
{
	FakeFs fs;
	seed_installed(fs);
	UninstallFileOps ops = fs.ops();
	ops.plan_restore = plan_returning(RestoreAction::Refuse, RefuseReason::BrokenJson);
	const UninstallStepResult r = restore_vencord_dir(ops, context());
	CHECK(has_leftover(r, RemovalItem::VencordDirValue, kState));
	CHECK(fs.files.count(kBackup) == 1);
}

TEST_CASE("段階 (3) でバックアップが壊れていれば、計画を立てずに結果に入れる")
{
	FakeFs fs;
	seed_installed(fs);
	fs.files[kBackup] = "not json";
	UninstallFileOps ops = fs.ops();
	int calls = 0;
	ops.plan_restore = plan_returning(RestoreAction::Write, RefuseReason::BrokenJson, &calls);
	const UninstallStepResult r = restore_vencord_dir(ops, context());
	CHECK(has_leftover(r, RemovalItem::VencordDirValue, kState));
	CHECK(calls == 0);
	CHECK(fs.files.count(kBackup) == 1);
}

TEST_CASE("段階 (3) でバックアップがないときは、data/vencord を指す値だけを結果に入れる")
{
	SUBCASE("ほかの場所を指すときは、戻すものがない")
	{
		FakeFs fs;
		fs.add_dir(kVesktop);
		fs.files[kState] = "{\"vencordDir\":\"C:/other/dist\"}";
		const UninstallStepResult r = restore_vencord_dir(fs.ops(), context());
		CHECK(r.leftovers.empty());
	}
	SUBCASE("キーがないときは、戻すものがない")
	{
		FakeFs fs;
		fs.add_dir(kCfg);
		fs.add_dir(kVesktop);
		fs.files[kState] = "{}";
		const UninstallStepResult r = restore_vencord_dir(fs.ops(), context());
		CHECK(r.leftovers.empty());
		CHECK(fs.dirs.count(kCfg) == 0); // 空の設定のフォルダーは消す
	}
	SUBCASE("data/vencord を指すときは、結果に入れる (バックアップを失った)")
	{
		FakeFs fs;
		fs.add_dir(kVesktop);
		fs.files[kState] = std::string("{\"vencordDir\":\"") + kBundled + "\"}";
		const UninstallStepResult r = restore_vencord_dir(fs.ops(), context());
		CHECK(has_leftover(r, RemovalItem::VencordDirValue, kState));
	}
}

TEST_CASE("段階 (3) で state.json がなければ、Vesktop は既定の設定で起動するので、バックアップを消す")
{
	FakeFs fs;
	seed_installed(fs);
	remove_settings_and_secret(fs.ops(), context());
	fs.files.erase(kState);
	UninstallFileOps ops = fs.ops();
	int calls = 0;
	ops.plan_restore = plan_returning(RestoreAction::Write, RefuseReason::BrokenJson, &calls);
	const UninstallStepResult r = restore_vencord_dir(ops, context());
	CHECK(r.leftovers.empty());
	CHECK(calls == 0);
	CHECK(fs.files.count(kBackup) == 0);
}

TEST_CASE("段階 (3) で state.json の場所が分からなければ、場所なしで結果に入れる")
{
	FakeFs fs;
	seed_installed(fs);
	fs.have_state_path = false;
	UninstallFileOps ops = fs.ops();
	ops.plan_restore = plan_returning(RestoreAction::Write);
	const UninstallStepResult r = restore_vencord_dir(ops, context());
	CHECK(has_leftover(r, RemovalItem::VencordDirValue, ""));
	CHECK(fs.files.count(kBackup) == 1);
}

TEST_CASE("段階 (3) で元に戻した後にバックアップを消せなければ、設定のファイルとして結果に入れる")
{
	FakeFs fs;
	seed_installed(fs);
	fs.undeletable.insert(kBackup);
	UninstallFileOps ops = fs.ops();
	ops.plan_restore = plan_returning(RestoreAction::Write);
	const UninstallStepResult r = restore_vencord_dir(ops, context());
	CHECK(has_leftover(r, RemovalItem::ModuleConfigFiles, kBackup));
	CHECK(fs.writes.size() == 1);
}

TEST_CASE("段階 (1) の後に設定のフォルダーへ書かれたファイルがあれば、段階 (3) はフォルダーを結果に入れる")
{
	FakeFs fs;
	seed_installed(fs);
	remove_settings_and_secret(fs.ops(), context());
	// 段階 (1) の後に、アンインストール中のフラグを見ない処理が書いた。
	fs.add_file(std::string(kCfg) + "/settings.json", "{}");
	UninstallFileOps ops = fs.ops();
	ops.plan_restore = plan_returning(RestoreAction::AlreadyRestored);
	const UninstallStepResult r = restore_vencord_dir(ops, context());
	CHECK(has_leftover(r, RemovalItem::ModuleConfigFiles, kCfg));
	CHECK(fs.files.count(kBackup) == 0);
}

// ---- 段階 (1) と (3) を続けて実行する ----

TEST_CASE("段階 (1) と (3) を実際の plan_restore で続けて実行すると、元の値に戻り、cordmux のものは何も残らない")
{
	FakeFs fs;
	seed_installed(fs);
	UninstallFileOps ops = fs.ops();
	ops.plan_restore = &plan_restore;

	CHECK(remove_settings_and_secret(ops, context()).leftovers.empty());
	CHECK(restore_vencord_dir(ops, context()).leftovers.empty());

	const std::optional<std::string> restored = cordmux::platform::parse_vencord_dir(fs.files[kState]);
	REQUIRE(restored.has_value());
	CHECK(*restored == "C:/mine/dist");
	CHECK(fs.dirs.count(kCfg) == 0);
	CHECK(fs.dirs.count(kLocal) == 0);
	for (const auto &f : fs.files)
		CHECK(f.first == kState); // 残っているのは Vesktop の設定だけ
}

TEST_CASE("段階 (1) で止まってやり直しても、バックアップが残っているので元に戻せる")
{
	FakeFs fs;
	seed_installed(fs);
	const std::string stuck = std::string(kCfg) + "/seen_people.json";
	fs.undeletable.insert(stuck);
	UninstallFileOps ops = fs.ops();
	ops.plan_restore = plan_returning(RestoreAction::Write);

	// 1 回目は段階 (1) が残りを返す (run_uninstall はここで止まり、段階 (3) を呼び出さない)。
	CHECK_FALSE(remove_settings_and_secret(ops, context()).leftovers.empty());
	CHECK(fs.files.count(kBackup) == 1);

	// 2 回目は消せるようになったので最後まで進み、バックアップから元に戻せる。
	fs.undeletable.clear();
	CHECK(remove_settings_and_secret(ops, context()).leftovers.empty());
	CHECK(restore_vencord_dir(ops, context()).leftovers.empty());
	CHECK(fs.writes == std::vector<std::string>{kState});
	CHECK(fs.dirs.count(kCfg) == 0);
}

// ---- 止まったときの結果 (UninstallStepResult::settings_dir と UninstallLeftover::is_file) ----

namespace {

const UninstallLeftover *leftover_at(const UninstallStepResult &r, RemovalItem item, const std::string &where)
{
	for (const UninstallLeftover &l : r.leftovers)
		if (l.item == item && l.where == where)
			return &l;
	return nullptr;
}

} // namespace

TEST_CASE("段階 (3) でバックアップを残して止まると設定のフォルダーを返し、バックアップがなければ返さない")
{
	SUBCASE("書き込みが許されていないときは、バックアップが残るので、フォルダーを返す")
	{
		FakeFs fs;
		seed_installed(fs);
		UninstallFileOps ops = fs.ops();
		ops.plan_restore = plan_returning(RestoreAction::Write);
		UninstallContext ctx = context();
		ctx.allow_state_json_write = false;
		const UninstallStepResult r = restore_vencord_dir(ops, ctx);
		CHECK(has_leftover(r, RemovalItem::VencordDirValue, kState));
		CHECK(fs.files.count(kBackup) == 1);
		CHECK(r.settings_dir == kCfg);
	}
	SUBCASE("書き込みの失敗、拒まれた計画、壊れたバックアップ、分からない state.json の場所のどれでも、フォルダーを返す")
	{
		for (int variant = 0; variant < 4; ++variant) {
			CAPTURE(variant);
			FakeFs fs;
			seed_installed(fs);
			UninstallFileOps ops = fs.ops();
			ops.plan_restore = plan_returning(RestoreAction::Write);
			if (variant == 0)
				fs.unwritable.insert(kState);
			if (variant == 1)
				ops.plan_restore = plan_returning(RestoreAction::Refuse, RefuseReason::BrokenJson);
			if (variant == 2)
				fs.files[kBackup] = "not json";
			if (variant == 3)
				fs.have_state_path = false;
			const UninstallStepResult r = restore_vencord_dir(ops, context());
			REQUIRE(r.leftovers.size() == 1);
			CHECK(r.leftovers[0].item == RemovalItem::VencordDirValue);
			CHECK(fs.files.count(kBackup) == 1);
			CHECK(r.settings_dir == kCfg);
		}
	}
	SUBCASE("バックアップがあるか分からない (フォルダーが読めず、空でない) ときは、フォルダーを返す")
	{
		FakeFs fs;
		seed_installed(fs);
		fs.unlistable.insert(kCfg);
		UninstallFileOps ops = fs.ops();
		ops.plan_restore = plan_returning(RestoreAction::Write);
		const UninstallStepResult r = restore_vencord_dir(ops, context());
		CHECK(has_leftover(r, RemovalItem::VencordDirValue, kState));
		CHECK(fs.dirs.count(kCfg) == 1);
		CHECK(r.settings_dir == kCfg);
	}
	SUBCASE("フォルダーの関数がなくて分からない (渡し忘れ) ときは、結果に入れるが、フォルダーは返さない")
	{
		FakeFs fs;
		seed_installed(fs);
		UninstallFileOps ops = fs.ops();
		ops.list_directory = nullptr;
		ops.remove_directory_if_empty = nullptr;
		ops.plan_restore = plan_returning(RestoreAction::Write);
		const UninstallStepResult r = restore_vencord_dir(ops, context());
		CHECK(has_leftover(r, RemovalItem::VencordDirValue, kState));
		CHECK(r.settings_dir.empty());
	}
	SUBCASE("バックアップがなく、値が data/vencord を指すときは、結果に入れるが、フォルダーは返さない")
	{
		FakeFs fs;
		fs.add_dir(kVesktop);
		fs.files[kState] = std::string("{\"vencordDir\":\"") + kBundled + "\"}";
		const UninstallStepResult r = restore_vencord_dir(fs.ops(), context());
		CHECK(has_leftover(r, RemovalItem::VencordDirValue, kState));
		CHECK(r.settings_dir.empty());
	}
	SUBCASE("UninstallContext にバックアップの場所がないときは、結果に入れるが、フォルダーは返さない")
	{
		FakeFs fs;
		seed_installed(fs);
		UninstallContext ctx = context();
		ctx.backup_json_path.clear();
		const UninstallStepResult r = restore_vencord_dir(fs.ops(), ctx);
		CHECK(has_leftover(r, RemovalItem::VencordDirValue, kState));
		CHECK(r.settings_dir.empty());
	}
	SUBCASE("元に戻したときは、後でバックアップの削除に失敗しても、フォルダーは返さない (バックアップはもう必要ない)")
	{
		FakeFs fs;
		seed_installed(fs);
		UninstallFileOps ops = fs.ops();
		ops.plan_restore = plan_returning(RestoreAction::Write);
		CHECK(restore_vencord_dir(ops, context()).settings_dir.empty());

		FakeFs stuck;
		seed_installed(stuck);
		stuck.undeletable.insert(kBackup);
		UninstallFileOps stuck_ops = stuck.ops();
		stuck_ops.plan_restore = plan_returning(RestoreAction::Write);
		const UninstallStepResult r = restore_vencord_dir(stuck_ops, context());
		CHECK(has_leftover(r, RemovalItem::ModuleConfigFiles, kBackup));
		CHECK(r.settings_dir.empty());
	}
}

TEST_CASE("段階 (1) で止まっても、設定のフォルダーは返さない (返すのは段階 (3) だけ)")
{
	FakeFs fs;
	seed_installed(fs);
	fs.undeletable.insert(std::string(kCfg) + "/settings.json");
	const UninstallStepResult r = remove_settings_and_secret(fs.ops(), context());
	CHECK_FALSE(r.leftovers.empty());
	CHECK(fs.files.count(kBackup) == 1);
	CHECK(r.settings_dir.empty());
}

TEST_CASE("段階 (1) と (3) の残りには、段階が見たとおりに、ファイルかフォルダーかが入る")
{
	SUBCASE("段階 (1) で消せないファイルはファイルになる")
	{
		FakeFs fs;
		seed_installed(fs);
		const std::string stuck = std::string(kCfg) + "/settings.json";
		fs.undeletable.insert(stuck);
		const UninstallStepResult r = remove_settings_and_secret(fs.ops(), context());
		const UninstallLeftover *l = leftover_at(r, RemovalItem::ModuleConfigFiles, stuck);
		REQUIRE(l != nullptr);
		CHECK(l->is_file);
	}
	SUBCASE("段階 (1) で、拡張子のないファイルもファイルになる (名前から推測しない)")
	{
		FakeFs fs;
		seed_installed(fs);
		const std::string stuck = std::string(kCfg) + "/dock_shown_once";
		fs.undeletable.insert(stuck);
		const UninstallStepResult r = remove_settings_and_secret(fs.ops(), context());
		const UninstallLeftover *l = leftover_at(r, RemovalItem::ModuleConfigFiles, stuck);
		REQUIRE(l != nullptr);
		CHECK(l->is_file);
	}
	SUBCASE("段階 (1) で、名前にドットを含むフォルダーもフォルダーになる")
	{
		FakeFs fs;
		seed_installed(fs);
		const std::string dotted = std::string(kCfg) + "/old.json";
		fs.add_file(dotted + "/inner.txt", "x");
		fs.undeletable.insert(dotted);
		const UninstallStepResult r = remove_settings_and_secret(fs.ops(), context());
		const UninstallLeftover *l = leftover_at(r, RemovalItem::ModuleConfigFiles, dotted);
		REQUIRE(l != nullptr);
		CHECK_FALSE(l->is_file);
	}
	SUBCASE("段階 (1) で、読めないフォルダー、使われているフォルダー、深さの上限を超えたフォルダーはフォルダーになる")
	{
		FakeFs fs;
		seed_installed(fs);
		fs.unlistable.insert(kLocal);
		UninstallStepResult r = remove_settings_and_secret(fs.ops(), context());
		const UninstallLeftover *l = leftover_at(r, RemovalItem::LocalDataFolder, kLocal);
		REQUIRE(l != nullptr);
		CHECK_FALSE(l->is_file);

		FakeFs used;
		seed_installed(used);
		used.local_used_by_other = true;
		r = remove_settings_and_secret(used.ops(), context());
		l = leftover_at(r, RemovalItem::LocalDataFolder, kLocal);
		REQUIRE(l != nullptr);
		CHECK_FALSE(l->is_file);

		FakeFs deep_fs;
		std::string deep = kCfg;
		for (int i = 0; i <= kMaxFolderDepth + 1; ++i)
			deep += "/d";
		deep_fs.add_file(deep + "/x.txt", "x");
		r = remove_settings_and_secret(deep_fs.ops(), context());
		std::string limit_path = kCfg;
		for (int i = 0; i <= kMaxFolderDepth; ++i)
			limit_path += "/d";
		l = leftover_at(r, RemovalItem::ModuleConfigFiles, limit_path);
		REQUIRE(l != nullptr);
		CHECK_FALSE(l->is_file);
	}
	SUBCASE("段階 (1) で消せないリンクは、ファイルを指せばファイル、フォルダーを指せばフォルダーになる")
	{
		LinkedFs lfs;
		lfs.stuck_links = {lfs.dir_link, lfs.file_link};
		const UninstallStepResult r = remove_settings_and_secret(lfs.ops(), context());
		const UninstallLeftover *dir = leftover_at(r, RemovalItem::ModuleConfigFiles, lfs.dir_link);
		const UninstallLeftover *file = leftover_at(r, RemovalItem::ModuleConfigFiles, lfs.file_link);
		REQUIRE(dir != nullptr);
		REQUIRE(file != nullptr);
		CHECK_FALSE(dir->is_file);
		CHECK(file->is_file);
	}
	SUBCASE("段階 (3) で、state.json とバックアップはファイル、設定のフォルダーはフォルダーになる")
	{
		FakeFs fs;
		seed_installed(fs);
		fs.unwritable.insert(kState);
		UninstallFileOps ops = fs.ops();
		ops.plan_restore = plan_returning(RestoreAction::Write);
		UninstallStepResult r = restore_vencord_dir(ops, context());
		const UninstallLeftover *l = leftover_at(r, RemovalItem::VencordDirValue, kState);
		REQUIRE(l != nullptr);
		CHECK(l->is_file);

		FakeFs stuck;
		seed_installed(stuck);
		stuck.undeletable.insert(kBackup);
		UninstallFileOps stuck_ops = stuck.ops();
		stuck_ops.plan_restore = plan_returning(RestoreAction::Write);
		r = restore_vencord_dir(stuck_ops, context());
		l = leftover_at(r, RemovalItem::ModuleConfigFiles, kBackup);
		REQUIRE(l != nullptr);
		CHECK(l->is_file);

		FakeFs late;
		seed_installed(late);
		remove_settings_and_secret(late.ops(), context());
		late.add_file(std::string(kCfg) + "/settings.json", "{}");
		UninstallFileOps late_ops = late.ops();
		late_ops.plan_restore = plan_returning(RestoreAction::AlreadyRestored);
		r = restore_vencord_dir(late_ops, context());
		l = leftover_at(r, RemovalItem::ModuleConfigFiles, kCfg);
		REQUIRE(l != nullptr);
		CHECK_FALSE(l->is_file);
	}
}

// ---- 段階 (3) Vencord の設定の cordmux の項 ----

namespace {

const char *const kVencordSettingsBefore = R"({
    "plugins": {
        "Alpha": {
            "enabled": true
        },
        "cordmux": {
            "enabled": true
        }
    }
}
)";
const char *const kVencordSettingsAfter = R"({
    "plugins": {
        "Alpha": {
            "enabled": true
        }
    }
}
)";

// 入れる前に項がなかったバックアップ。
PluginEntryBackup entry_absent()
{
	PluginEntryBackup e;
	e.state = PluginEntryState::Absent;
	return e;
}

// 入れる前に項があったバックアップ (利用者が前から使っている別のバージョンの cordmux の項)。
const char *const kOtherEntry = R"({"enabled":true,"port":8123,"instanceId":"abc","token":"secret"})";
PluginEntryBackup entry_present()
{
	PluginEntryBackup e;
	e.state = PluginEntryState::Present;
	e.value_json = kOtherEntry;
	return e;
}

// 実際の plan_restore を使う段階 (3) の関数。
UninstallFileOps with_real_plan(FakeFs &fs)
{
	UninstallFileOps ops = fs.ops();
	ops.plan_restore = &plan_restore;
	return ops;
}

} // namespace

TEST_CASE("段階 (3) は、元に戻し終えた後に、前に項がなかったなら、Vencord の設定の cordmux の項だけを消す")
{
	FakeFs fs;
	seed_installed(fs, entry_absent());
	remove_settings_and_secret(fs.ops(), context()); // 段階 (1) を済ませた状態から始める
	fs.add_file(kVencordSettings, kVencordSettingsBefore);

	const UninstallStepResult r = restore_vencord_dir(with_real_plan(fs), context());
	CHECK(r.leftovers.empty());
	CHECK(fs.files[kVencordSettings] == kVencordSettingsAfter);
	CHECK(fs.written.count(kVencordSettings) == 1);
}

TEST_CASE("段階 (3) で Vencord の設定のファイルがない、項がない、壊れているときは、何も書かず、残りにも数えない")
{
	{
		FakeFs fs; // ファイルがない
		seed_installed(fs, entry_absent());
		remove_settings_and_secret(fs.ops(), context()); // 段階 (1) を済ませた状態から始める
		const UninstallStepResult r = restore_vencord_dir(with_real_plan(fs), context());
		CHECK(r.leftovers.empty());
		CHECK(fs.files.count(kVencordSettings) == 0);
		CHECK(fs.written.count(kVencordSettings) == 0);
	}
	for (const char *text : {R"({"plugins":{"Alpha":{}}})", R"({"plugins":{"cordmux":)", "[]"}) {
		FakeFs fs;
		seed_installed(fs, entry_absent());
		remove_settings_and_secret(fs.ops(), context()); // 段階 (1) を済ませた状態から始める
		fs.add_file(kVencordSettings, text);
		const UninstallStepResult r = restore_vencord_dir(with_real_plan(fs), context());
		CHECK(r.leftovers.empty());
		CHECK(fs.files[kVencordSettings] == text);
		CHECK(fs.written.count(kVencordSettings) == 0);
	}
}

TEST_CASE("段階 (3) で Vencord の設定を書けなくても、残りに数えない")
{
	FakeFs fs;
	seed_installed(fs, entry_absent());
	remove_settings_and_secret(fs.ops(), context()); // 段階 (1) を済ませた状態から始める
	fs.add_file(kVencordSettings, kVencordSettingsBefore);
	fs.unwritable.insert(kVencordSettings);
	const UninstallStepResult r = restore_vencord_dir(with_real_plan(fs), context());
	CHECK(r.leftovers.empty());
	CHECK(fs.files[kVencordSettings] == kVencordSettingsBefore);
}

TEST_CASE("段階 (3) は、元に戻せずに止まるときと、設定の変更を許していないときは、Vencord の設定に触らない")
{
	{
		FakeFs fs;
		seed_installed(fs, entry_absent());
		remove_settings_and_secret(fs.ops(), context()); // 段階 (1) を済ませた状態から始める
		fs.add_file(kVencordSettings, kVencordSettingsBefore);
		fs.unwritable.insert(kState); // state.json を書けず、段階が止まる
		UninstallFileOps ops = fs.ops();
		ops.plan_restore = plan_returning(RestoreAction::Write);
		const UninstallStepResult r = restore_vencord_dir(ops, context());
		CHECK_FALSE(r.leftovers.empty());
		CHECK(fs.files[kVencordSettings] == kVencordSettingsBefore);
	}
	{
		FakeFs fs;
		seed_installed(fs, entry_absent());
		remove_settings_and_secret(fs.ops(), context()); // 段階 (1) を済ませた状態から始める
		fs.add_file(kVencordSettings, kVencordSettingsBefore);
		UninstallContext c = context();
		c.allow_state_json_write = false;
		restore_vencord_dir(with_real_plan(fs), c);
		CHECK(fs.files[kVencordSettings] == kVencordSettingsBefore);
	}
}

TEST_CASE("段階 (3) は、前から項があったなら、cordmux の後で値が変わっていても、前の値に戻す")
{
	FakeFs fs;
	seed_installed(fs, entry_present());
	remove_settings_and_secret(fs.ops(), context()); // 段階 (1) を済ませた状態から始める
	fs.add_file(kVencordSettings, kVencordSettingsBefore); // cordmux の項は、Vencord が補った {"enabled": true}

	const UninstallStepResult r = restore_vencord_dir(with_real_plan(fs), context());
	CHECK(r.leftovers.empty());
	CHECK(fs.files[kVencordSettings] ==
	      "{\n"
	      "    \"plugins\": {\n"
	      "        \"Alpha\": {\n"
	      "            \"enabled\": true\n"
	      "        },\n"
	      "        \"cordmux\": {\n"
	      "            \"enabled\": true,\n"
	      "            \"port\": 8123,\n"
	      "            \"instanceId\": \"abc\",\n"
	      "            \"token\": \"secret\"\n"
	      "        }\n"
	      "    }\n"
	      "}\n");
	CHECK(fs.written.count(kVencordSettings) == 1);
}

TEST_CASE("段階 (3) は、バックアップが分からない (古いバックアップで項目がない、取れなかった) ときは、Vencord の設定に触らない")
{
	{
		FakeFs fs; // plugin_entry のキーがない古いバックアップ
		seed_installed(fs);
		fs.files[kBackup] = R"({"previous":"C:/mine/dist","written":"C:/plugins/cordmux/data/vencord",)"
				    R"("written_at":123,"dll_dir":"C:/plugins/cordmux"})";
		remove_settings_and_secret(fs.ops(), context()); // 段階 (1) を済ませた状態から始める
		fs.add_file(kVencordSettings, kVencordSettingsBefore);
		const UninstallStepResult r = restore_vencord_dir(with_real_plan(fs), context());
		CHECK(r.leftovers.empty());
		CHECK(fs.files[kState] == R"({"vencordDir":"C:/mine/dist","firstLaunch":false})"); // vencordDir は戻る
		CHECK(fs.files[kVencordSettings] == kVencordSettingsBefore);
		CHECK(fs.written.count(kVencordSettings) == 0);
	}
	{
		FakeFs fs; // バックアップが「分からない」で書かれたバックアップ
		seed_installed(fs, PluginEntryBackup{});
		remove_settings_and_secret(fs.ops(), context());
		fs.add_file(kVencordSettings, kVencordSettingsBefore);
		restore_vencord_dir(with_real_plan(fs), context());
		CHECK(fs.files[kVencordSettings] == kVencordSettingsBefore);
	}
	{
		FakeFs fs; // バックアップがない
		seed_installed(fs);
		fs.files.erase(kBackup);
		remove_settings_and_secret(fs.ops(), context());
		fs.add_file(kVencordSettings, kVencordSettingsBefore);
		restore_vencord_dir(with_real_plan(fs), context());
		CHECK(fs.files[kVencordSettings] == kVencordSettingsBefore);
	}
	{
		FakeFs fs; // 壊れたバックアップ (段階は止まる)
		seed_installed(fs, entry_absent());
		fs.files[kBackup] = "{broken";
		remove_settings_and_secret(fs.ops(), context());
		fs.add_file(kVencordSettings, kVencordSettingsBefore);
		restore_vencord_dir(with_real_plan(fs), context());
		CHECK(fs.files[kVencordSettings] == kVencordSettingsBefore);
	}
}

TEST_CASE("段階 (3) は、state.json を書く直前に Vesktop が起動していれば (調べられなくても)、書かずに止まり、バックアップを残す")
{
	const auto run_with = [](std::optional<bool> running, bool pass_check) {
		FakeFs fs;
		seed_installed(fs, entry_absent());
		remove_settings_and_secret(fs.ops(), context()); // 段階 (1) を済ませた状態から始める
		fs.add_file(kVencordSettings, kVencordSettingsBefore);
		const std::string state_before = fs.files[kState];
		fs.vesktop_running = running;
		UninstallFileOps ops = with_real_plan(fs);
		if (!pass_check)
			ops.vesktop_running = nullptr;

		const UninstallStepResult r = restore_vencord_dir(ops, context());
		// 書けなかったときと同じ止まり方 (元に戻せなかった設定として結果に入れ、バックアップを残した設定のフォルダーを返す)。
		CHECK(has_leftover(r, RemovalItem::VencordDirValue, kState));
		CHECK(r.settings_dir == kCfg);
		CHECK(fs.writes.empty());
		CHECK(fs.files[kState] == state_before);
		CHECK(fs.files[kVencordSettings] == kVencordSettingsBefore);
		CHECK(fs.files.count(kBackup) == 1);
		CHECK(fs.vesktop_checks == (pass_check ? 1 : 0));
	};
	SUBCASE("起動している") { run_with(true, true); }
	SUBCASE("起動しているか調べられない") { run_with(std::nullopt, true); }
	SUBCASE("確認する関数を渡し忘れた") { run_with(false, false); }
}

TEST_CASE("段階 (3) は、Vesktop を書く直前に 1 回だけ確認する")
{
	FakeFs fs;
	seed_installed(fs, entry_absent());
	remove_settings_and_secret(fs.ops(), context()); // 段階 (1) を済ませた状態から始める
	fs.add_file(kVencordSettings, kVencordSettingsBefore);

	const UninstallStepResult r = restore_vencord_dir(with_real_plan(fs), context());
	CHECK(r.leftovers.empty());
	CHECK(fs.writes == std::vector<std::string>{kState, kVencordSettings});
	// 確認は state.json を書く前の 1 回だけで、Vencord の設定を書く前に繰り返さない。
	CHECK(fs.vesktop_checks == 1);
	CHECK(fs.writes_at_vesktop_check == 0);
}

TEST_CASE("段階 (3) で state.json がもう戻っていれば、Vencord の設定を書く前に Vesktop を確認し、起動していれば書かない")
{
	for (const std::optional<bool> running :
	     {std::optional<bool>(false), std::optional<bool>(true), std::optional<bool>()}) {
		CAPTURE(running.has_value());
		FakeFs fs;
		seed_installed(fs, entry_absent());
		remove_settings_and_secret(fs.ops(), context()); // 段階 (1) を済ませた状態から始める
		fs.files[kState] = R"({"vencordDir":"C:/mine/dist","firstLaunch":false})"; // もう戻っている
		fs.add_file(kVencordSettings, kVencordSettingsBefore);
		fs.vesktop_running = running;

		const UninstallStepResult r = restore_vencord_dir(with_real_plan(fs), context());
		// Vencord の設定は Vesktop の動作に関わらないので、書かなくても段階は止めない。
		CHECK(r.leftovers.empty());
		CHECK(fs.written.count(kState) == 0);
		CHECK(fs.vesktop_checks == 1);
		const bool stopped = running.has_value() && !*running;
		CHECK(fs.files[kVencordSettings] == (stopped ? kVencordSettingsAfter : kVencordSettingsBefore));
	}
}

// ---- [Vesktopから削除] (remove_from_vesktop) ----

namespace {

// [Vesktopから削除] の関数。実際の plan_restore を使い、Vesktop は起動していない (running を変えれば起動中か、
// 調べられない)。checks には確認した回数を、writes_at_check には確認した時点で書いた数を入れる。
struct RemoveRun {
	std::optional<bool> running = false;
	int checks = 0;
	std::size_t writes_at_check = 0;
};

UninstallFileOps remove_ops(FakeFs &fs, RemoveRun &run)
{
	UninstallFileOps ops = with_real_plan(fs);
	ops.vesktop_running = [&fs, &run]() {
		++run.checks;
		run.writes_at_check = fs.writes.size();
		return run.running;
	};
	return ops;
}

} // namespace

TEST_CASE("[Vesktopから削除] は、Vesktop の設定を cordmux を追加する前に戻し、cordmux のファイルと設定は消さない")
{
	FakeFs fs;
	seed_installed(fs, entry_absent());
	fs.add_file(kVencordSettings, kVencordSettingsBefore);
	const std::map<std::string, std::string> cordmux_files_before = [&fs] {
		std::map<std::string, std::string> out;
		for (const auto &f : fs.files)
			if (f.first.rfind(kCfg, 0) == 0 || f.first.rfind(kLocal, 0) == 0)
				out.insert(f);
		return out;
	}();
	RemoveRun run;
	CHECK(remove_from_vesktop(remove_ops(fs, run), context()) == VesktopRemoveOutcome::Restored);

	// state.json はバックアップの previous に戻り、ほかのキーは残る。Vencord の設定の cordmux の項も戻る (項が
	// なかったので消える)。
	CHECK(fs.files[kState] == R"({"vencordDir":"C:/mine/dist","firstLaunch":false})");
	CHECK(fs.files[kVencordSettings] == kVencordSettingsAfter);
	// バックアップ、cordmux の設定、トークンは 1 つも消さず、書き換えもしない (段階 (3) と違い、finish_backup を
	// しない)。後の [Vesktopに追加] とアンインストールが、同じバックアップを使う。
	CHECK(fs.deletes == 0);
	CHECK(fs.dir_removes == 0);
	for (const auto &f : cordmux_files_before) {
		CAPTURE(f.first);
		REQUIRE(fs.files.count(f.first) == 1);
		CHECK(fs.files[f.first] == f.second);
	}
	// 書いたのは Vesktop の 2 つのファイルだけで、書く前に 1 回だけ Vesktop を確認した。
	CHECK(fs.writes == std::vector<std::string>{kState, kVencordSettings});
	CHECK(run.checks == 1);
	CHECK(run.writes_at_check == 0);

	// もう戻っているときは、state.json を書かずに、戻っていると返す。
	RemoveRun again;
	CHECK(remove_from_vesktop(remove_ops(fs, again), context()) == VesktopRemoveOutcome::AlreadyRestored);
	CHECK(fs.writes.size() == 2);
}

TEST_CASE("[Vesktopから削除] は、書く直前に Vesktop が起動していれば (調べられなくても) 何も書かない")
{
	for (const std::optional<bool> running : {std::optional<bool>(true), std::optional<bool>()}) {
		FakeFs fs;
		seed_installed(fs, entry_absent());
		fs.add_file(kVencordSettings, kVencordSettingsBefore);
		const std::string state_before = fs.files[kState];
		RemoveRun run;
		run.running = running;
		const VesktopRemoveOutcome outcome = remove_from_vesktop(remove_ops(fs, run), context());
		CHECK(outcome == (running.has_value() ? VesktopRemoveOutcome::VesktopRunning
						      : VesktopRemoveOutcome::VesktopUnknown));
		CHECK(fs.writes.empty());
		CHECK(fs.files[kState] == state_before);
		CHECK(fs.files[kVencordSettings] == kVencordSettingsBefore);
		CHECK(run.checks == 1);
	}
	// 確認する関数を渡し忘れたときも、調べられないものとして書かない。
	FakeFs fs;
	seed_installed(fs);
	UninstallFileOps ops = with_real_plan(fs);
	ops.vesktop_running = nullptr;
	CHECK(remove_from_vesktop(ops, context()) == VesktopRemoveOutcome::VesktopUnknown);
	CHECK(fs.writes.empty());
}

TEST_CASE("[Vesktopから削除] で戻せないときは、理由を返して何も書かない")
{
	{
		FakeFs fs; // バックアップがない
		seed_installed(fs);
		fs.files.erase(kBackup);
		RemoveRun run;
		CHECK(remove_from_vesktop(remove_ops(fs, run), context()) == VesktopRemoveOutcome::NoBackup);
		CHECK(fs.writes.empty());
		CHECK(run.checks == 0);
	}
	{
		FakeFs fs; // 壊れたバックアップ
		seed_installed(fs);
		fs.files[kBackup] = "{broken";
		RemoveRun run;
		CHECK(remove_from_vesktop(remove_ops(fs, run), context()) == VesktopRemoveOutcome::BackupInvalid);
		CHECK(fs.writes.empty());
	}
	{
		FakeFs fs; // state.json の場所が分からない
		seed_installed(fs);
		fs.have_state_path = false;
		RemoveRun run;
		CHECK(remove_from_vesktop(remove_ops(fs, run), context()) == VesktopRemoveOutcome::StatePathUnknown);
		CHECK(fs.writes.empty());
	}
	{
		FakeFs fs; // state.json を読めない
		seed_installed(fs);
		fs.files.erase(kState);
		RemoveRun run;
		CHECK(remove_from_vesktop(remove_ops(fs, run), context()) == VesktopRemoveOutcome::StateUnreadable);
		CHECK(fs.writes.empty());
	}
	{
		FakeFs fs; // 現在の値が cordmux の書いた値でも元の値でもない (戻す先が分からない)
		seed_installed(fs);
		fs.files[kState] = R"({"vencordDir":"C:/someone/else","firstLaunch":false})";
		RemoveRun run;
		CHECK(remove_from_vesktop(remove_ops(fs, run), context()) == VesktopRemoveOutcome::Refused);
		CHECK(fs.writes.empty());
		CHECK(run.checks == 0);
	}
	{
		FakeFs fs; // この起動では Vesktop の設定を書かない
		seed_installed(fs);
		UninstallContext c = context();
		c.allow_state_json_write = false;
		RemoveRun run;
		CHECK(remove_from_vesktop(remove_ops(fs, run), c) == VesktopRemoveOutcome::WritesDisabled);
		CHECK(fs.writes.empty());
		CHECK(run.checks == 0);
	}
	{
		FakeFs fs; // state.json に書けない (Vencord の設定にも触らない)
		seed_installed(fs, entry_absent());
		fs.add_file(kVencordSettings, kVencordSettingsBefore);
		fs.unwritable.insert(kState);
		RemoveRun run;
		CHECK(remove_from_vesktop(remove_ops(fs, run), context()) == VesktopRemoveOutcome::StateWriteFailed);
		CHECK(fs.files[kVencordSettings] == kVencordSettingsBefore);
		CHECK(fs.files.count(kBackup) == 1);
	}
}

// ---- バックアップに書いた state.json の場所 (Backup::state_path) ----

namespace {

const char *const kOtherState = "D:/other/Vesktop/state.json";

// バックアップに、書いた state.json の場所 (現在の環境変数の場所とは別) を入れた、インストールした後の形。
void seed_installed_elsewhere(FakeFs &fs)
{
	Backup backup{std::string("C:/mine/dist"), kBundled, 123, "C:/plugins/cordmux"};
	backup.plugin_entry = entry_absent();
	backup.state_path = kOtherState;
	fs.add_file(kBackup, backup_to_json(backup));
	fs.add_file(kOtherState, std::string("{\"vencordDir\":\"") + kBundled + "\",\"firstLaunch\":false}");
	// 現在の場所の state.json は、別の Vesktop の設定 (触らない)。
	fs.add_file(kState, R"({"vencordDir":"C:/someone/else","firstLaunch":false})");
	fs.add_file(cordmux::platform::vesktop_settings_json_beside(kOtherState), kVencordSettingsBefore);
	fs.add_file(kVencordSettings, kVencordSettingsBefore);
}

} // namespace

TEST_CASE("[Vesktopから削除] は、バックアップに書いた場所の state.json と、同じフォルダーの Vencord の設定を戻し、現在の場所には触らない")
{
	FakeFs fs;
	seed_installed_elsewhere(fs);
	RemoveRun run;
	CHECK(remove_from_vesktop(remove_ops(fs, run), context()) == VesktopRemoveOutcome::Restored);

	CHECK(fs.files[kOtherState] == R"({"vencordDir":"C:/mine/dist","firstLaunch":false})");
	CHECK(fs.files[cordmux::platform::vesktop_settings_json_beside(kOtherState)] == kVencordSettingsAfter);
	CHECK(fs.files[kState] == R"({"vencordDir":"C:/someone/else","firstLaunch":false})");
	CHECK(fs.files[kVencordSettings] == kVencordSettingsBefore);
	CHECK(fs.writes.size() == 2);
	CHECK(fs.writes[0] == kOtherState);
}

TEST_CASE("段階 (3) は、バックアップに書いた場所の state.json を戻す。場所が今と違っても、現在の場所には触らない")
{
	FakeFs fs;
	seed_installed_elsewhere(fs);
	const UninstallStepResult r = restore_vencord_dir(with_real_plan(fs), context());
	CHECK(r.leftovers.empty());
	CHECK(fs.files[kOtherState] == R"({"vencordDir":"C:/mine/dist","firstLaunch":false})");
	CHECK(fs.files[cordmux::platform::vesktop_settings_json_beside(kOtherState)] == kVencordSettingsAfter);
	CHECK(fs.files[kState] == R"({"vencordDir":"C:/someone/else","firstLaunch":false})");
	CHECK(fs.files[kVencordSettings] == kVencordSettingsBefore);
}

TEST_CASE("段階 (3) は、バックアップに場所がなければ (古い版のバックアップ)、現在の場所を使う")
{
	FakeFs fs;
	seed_installed(fs, entry_absent());
	remove_settings_and_secret(fs.ops(), context()); // 段階 (1) を済ませた状態から始める
	fs.add_file(kVencordSettings, kVencordSettingsBefore);
	const UninstallStepResult r = restore_vencord_dir(with_real_plan(fs), context());
	CHECK(r.leftovers.empty());
	CHECK(fs.files[kState] == R"({"vencordDir":"C:/mine/dist","firstLaunch":false})");
}
