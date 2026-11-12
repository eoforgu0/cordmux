// SPDX-License-Identifier: Apache-2.0

// install/uninstall.h の、段階の順序と止め方 (run_uninstall) と、表から段階ごとの対象を取り出す純粋関数の
// 単体テスト。
// プロセスに 1 つのアンインストール中のフラグは下ろせないので、ここでは立てない (UninstallMark の別の実体で
// 確認する。同じ実行ファイルのほかのテストが is_uninstalling() を読んでも、値が変わらないように)。

#include "doctest.h"

#include "install/uninstall.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace cordmux::install;

namespace {

UninstallStepResult clean()
{
	return UninstallStepResult{};
}

UninstallStepResult left(RemovalItem item, const std::string &where)
{
	UninstallStepResult result;
	result.leftovers.push_back(UninstallLeftover{item, where});
	return result;
}

// 呼び出された順を記録する、テスト用の段階の関数。
struct Recorder {
	std::vector<std::string> calls;
	UninstallStepResult step1 = clean();
	UninstallStepResult step2 = clean();
	UninstallStepResult step3 = clean();

	UninstallOps ops()
	{
		UninstallOps out;
		out.mark_and_stop_listening = [this] { calls.push_back("0"); };
		out.remove_settings_and_secret = [this] {
			calls.push_back("1");
			return step1;
		};
		out.remove_sources_and_scenes = [this] {
			calls.push_back("2");
			return step2;
		};
		out.restore_vencord_dir = [this] {
			calls.push_back("3");
			return step3;
		};
		out.folder_to_delete = [this] {
			calls.push_back("folder");
			return std::string("C:/plugins/cordmux");
		};
		out.record_dir = [this] {
			calls.push_back("record");
			return std::string("C:/rec");
		};
		return out;
	}
};

} // namespace

TEST_CASE("段階ごとの Product の行の種類が、表の順で返る")
{
	CHECK(product_items_for_step(UninstallStep::SettingsAndSecret) ==
	      std::vector<RemovalItem>{RemovalItem::ModuleConfigFiles, RemovalItem::LocalDataFolder});
	CHECK(product_items_for_step(UninstallStep::SourcesAndScenes) ==
	      std::vector<RemovalItem>{RemovalItem::CordmuxSources});
	CHECK(product_items_for_step(UninstallStep::RestoreVencordDir) ==
	      std::vector<RemovalItem>{RemovalItem::VencordDirValue});
	// 段階 (0) は表の行を持たず、段階 (4) は利用者が消す行だけで、消さない行はどの段階にも現れない。
	CHECK(product_items_for_step(UninstallStep::MarkUninstalling).empty());
	CHECK(product_items_for_step(UninstallStep::ShowFolder).empty());
	CHECK(product_items_for_step(UninstallStep::NotRemoved).empty());
}

TEST_CASE("表のどの Product の行も、ちょうど 1 つの段階で扱われる (追加した行が飛ばされない)")
{
	std::size_t product_rows = 0;
	for (const RemovalRow &row : kRemovalTable) {
		if (row.remover != Remover::Product)
			continue;
		++product_rows;
		int seen = 0;
		for (UninstallStep step :
		     {UninstallStep::SettingsAndSecret, UninstallStep::SourcesAndScenes, UninstallStep::RestoreVencordDir}) {
			for (RemovalItem item : product_items_for_step(step))
				seen += item == row.item ? 1 : 0;
		}
		CAPTURE(static_cast<int>(row.item));
		CHECK(seen == 1);
	}
	CHECK(product_rows == 4);
}

TEST_CASE("locate_of は表の Locate の列を返す")
{
	for (const RemovalRow &row : kRemovalTable) {
		CAPTURE(static_cast<int>(row.item));
		REQUIRE(locate_of(row.item).has_value());
		CHECK(*locate_of(row.item) == row.locate);
	}
}

TEST_CASE("段階 (0) から (4) が順に実行され、結果は completed になる")
{
	Recorder rec;
	const UninstallReport report = run_uninstall(rec.ops());
	CHECK(rec.calls == std::vector<std::string>{"0", "1", "2", "3", "folder", "record"});
	CHECK(report.completed);
	CHECK(report.reached == UninstallStep::ShowFolder);
	CHECK(report.leftovers.empty());
	CHECK(report.folder_to_delete == "C:/plugins/cordmux");
	CHECK(report.record_dir == "C:/rec");
}

TEST_CASE("残りを返した段階で止まり、その段階の残りだけを結果に入れる")
{
	SUBCASE("段階 (1) で止まる")
	{
		Recorder rec;
		rec.step1 = left(RemovalItem::ModuleConfigFiles, "C:/cfg/settings.json");
		const UninstallReport report = run_uninstall(rec.ops());
		// 止まったときもプラグインのフォルダーを求める (結果のダイアログで、削除しないでほしいフォルダーとして
		// 表示する)。録画の場所は、終わったときだけ求める (ここでは呼び出さない)。
		CHECK(rec.calls == std::vector<std::string>{"0", "1", "folder"});
		CHECK_FALSE(report.completed);
		CHECK(report.reached == UninstallStep::MarkUninstalling);
		REQUIRE(report.leftovers.size() == 1);
		CHECK(report.leftovers[0].item == RemovalItem::ModuleConfigFiles);
		CHECK(report.leftovers[0].where == "C:/cfg/settings.json");
		CHECK(report.folder_to_delete == "C:/plugins/cordmux");
		CHECK(report.record_dir.empty());
	}
	SUBCASE("段階 (2) で止まる")
	{
		Recorder rec;
		rec.step2 = left(RemovalItem::CordmuxSources, "Sources A");
		const UninstallReport report = run_uninstall(rec.ops());
		CHECK(rec.calls == std::vector<std::string>{"0", "1", "2", "folder"});
		CHECK(report.reached == UninstallStep::SettingsAndSecret);
		REQUIRE(report.leftovers.size() == 1);
		CHECK(report.leftovers[0].where == "Sources A");
	}
	SUBCASE("段階 (3) で止まる")
	{
		Recorder rec;
		rec.step3 = left(RemovalItem::VencordDirValue, "C:/Vesktop/state.json");
		const UninstallReport report = run_uninstall(rec.ops());
		CHECK(rec.calls == std::vector<std::string>{"0", "1", "2", "3", "folder"});
		CHECK(report.reached == UninstallStep::SourcesAndScenes);
		CHECK_FALSE(report.completed);
		// 段階 (3) で止まったときも、結果のダイアログの注意が指すフォルダーの場所を持つ。
		CHECK(report.folder_to_delete == "C:/plugins/cordmux");
		CHECK(report.record_dir.empty());
	}
}

TEST_CASE("開けなかったシーンコレクションと重複した表示名は結果に入るが、アンインストールは止まらない")
{
	Recorder rec;
	rec.step2.collections_not_opened = {"Broken"};
	rec.step2.collections_duplicate_names = {"Twin"};
	const UninstallReport report = run_uninstall(rec.ops());
	CHECK(report.completed);
	CHECK(report.collections_not_opened == std::vector<std::string>{"Broken"});
	CHECK(report.collections_duplicate_names == std::vector<std::string>{"Twin"});
	CHECK(report.leftovers.empty());
}

TEST_CASE("止まった段階が返したシーンコレクションの名前も、結果に入る")
{
	Recorder rec;
	rec.step2 = left(RemovalItem::CordmuxSources, "A");
	rec.step2.collections_not_opened = {"Broken"};
	const UninstallReport report = run_uninstall(rec.ops());
	CHECK_FALSE(report.completed);
	CHECK(report.collections_not_opened == std::vector<std::string>{"Broken"});
}

TEST_CASE("段階の関数がないと、その段階の行を残りとして結果に入れて止まる")
{
	Recorder rec;
	UninstallOps ops = rec.ops();
	ops.remove_sources_and_scenes = nullptr;
	const UninstallReport report = run_uninstall(ops);
	CHECK(rec.calls == std::vector<std::string>{"0", "1", "folder"});
	CHECK_FALSE(report.completed);
	CHECK(report.reached == UninstallStep::SettingsAndSecret);
	REQUIRE(report.leftovers.size() == 1);
	CHECK(report.leftovers[0].item == RemovalItem::CordmuxSources);
	for (const UninstallLeftover &l : report.leftovers)
		CHECK(l.where.empty());
}

TEST_CASE("段階 (0) の関数がないと、何も実行せず、Product の行を全部結果に入れる")
{
	Recorder rec;
	UninstallOps ops = rec.ops();
	ops.mark_and_stop_listening = nullptr;
	const UninstallReport report = run_uninstall(ops);
	CHECK(rec.calls.empty());
	CHECK_FALSE(report.completed);
	CHECK(report.leftovers.size() == 4);
}

TEST_CASE("UninstallOps の関数がすべて空でもクラッシュしない")
{
	const UninstallReport report = run_uninstall(UninstallOps{});
	CHECK_FALSE(report.completed);
	CHECK(report.folder_to_delete.empty());
}

TEST_CASE("段階 (4) の関数がなくても完了し、フォルダーは空になる")
{
	Recorder rec;
	UninstallOps ops = rec.ops();
	ops.folder_to_delete = nullptr;
	const UninstallReport report = run_uninstall(ops);
	CHECK(report.completed);
	CHECK(report.folder_to_delete.empty());
	CHECK(report.record_dir == "C:/rec");
}

TEST_CASE("段階 (4) の関数がないまま途中で止まると、フォルダーは空のまま (手順書と違う置き方か、関数の渡し忘れ)")
{
	Recorder rec;
	rec.step3 = left(RemovalItem::VencordDirValue, "C:/Vesktop/state.json");
	UninstallOps ops = rec.ops();
	ops.folder_to_delete = nullptr;
	const UninstallReport report = run_uninstall(ops);
	CHECK_FALSE(report.completed);
	CHECK(report.folder_to_delete.empty());
}

TEST_CASE("録画の保存先は、フォルダーがあり中に 1 つ以上のものがあるときだけ表示する")
{
	using cordmux::platform::DirEntry;
	std::vector<std::string> listed;
	const auto list_of = [&listed](bool found, std::vector<DirEntry> entries) {
		return [&listed, found, entries](const std::string &dir, std::vector<DirEntry> &out) {
			listed.push_back(dir);
			if (!found)
				return false;
			out = entries;
			return true;
		};
	};
	DirEntry session;
	session.name = "2000-01-01 20-00-00";
	session.is_directory = true;

	// ない (一度も録画していない利用者。保存先のフォルダーは録画のセッションを始めるときにだけ作る) ので、
	// 表示しない。
	CHECK(record_dir_if_has_files("C:/Videos/cordmux", list_of(false, {})).empty());
	// あるが空 (録画を手で消した) なので、表示しない。
	CHECK(record_dir_if_has_files("C:/Videos/cordmux", list_of(true, {})).empty());
	// あり、中に 1 つ以上あるので、そのまま表示する。
	CHECK(record_dir_if_has_files("C:/Videos/cordmux", list_of(true, {session})) == "C:/Videos/cordmux");
	CHECK(listed == std::vector<std::string>{"C:/Videos/cordmux", "C:/Videos/cordmux", "C:/Videos/cordmux"});
	// 場所が空 (保存先を決められなかった) か、関数がないときは、見ずに表示しない。
	listed.clear();
	CHECK(record_dir_if_has_files("", list_of(true, {session})).empty());
	CHECK(listed.empty());
	CHECK(record_dir_if_has_files("C:/Videos/cordmux", nullptr).empty());

	// 利用者が選んだ保存先は、録画のほかのファイルも入る場所でありうる。開始日時の名前のフォルダーがなければ、
	// 空でなくても録画の場所として表示しない (フォルダーでも、リンクでも、名前の形が違っても数えない)。
	DirEntry other_folder;
	other_folder.name = "Windows";
	other_folder.is_directory = true;
	DirEntry stray_file;
	stray_file.name = "2000-01-01 20-00-00";
	DirEntry linked;
	linked.name = "2000-01-01 20-00-00";
	linked.is_directory = true;
	linked.is_link = true;
	CHECK(record_dir_if_has_files("D:/", list_of(true, {other_folder, stray_file, linked})).empty());
	CHECK(record_dir_if_has_files("D:/", list_of(true, {other_folder, session})) == "D:/");
}

TEST_CASE("UninstallMark のフラグは、一度立てたら下りない")
{
	UninstallMark mark;
	CHECK_FALSE(mark.is_marked());
	mark.mark();
	CHECK(mark.is_marked());
	mark.mark();
	CHECK(mark.is_marked());
}

TEST_CASE("フラグの確認を通り過ぎた書き込みは mark() が戻る前に終わり、その後の書き込みは断られる")
{
	// 設定のフォルダーに、ほかのスレッドが書くファイルが 1 つある (録画ファイルの作成の済んでいない録画の保存先の一覧)。その
	// 書き込みの最中に段階 (0) がフラグを立て、段階 (1) がフォルダーを空にする。その後にファイルが作り直されては
	// ならない。
	UninstallMark mark;
	std::mutex mu;
	std::condition_variable cv;
	bool writer_inside = false;
	bool release_writer = false;
	bool mark_returned = false;
	std::atomic<bool> file_exists{false};

	std::thread writer([&]() {
		const bool wrote = mark.write_unless_marked([&]() {
			std::unique_lock<std::mutex> lock(mu);
			writer_inside = true;
			cv.notify_all();
			cv.wait(lock, [&]() { return release_writer; });
			file_exists = true;
			return true;
		});
		CHECK(wrote);
	});
	{
		std::unique_lock<std::mutex> lock(mu);
		cv.wait(lock, [&]() { return writer_inside; });
	}
	std::thread uninstaller([&]() {
		mark.mark(); // 段階 (0)
		{
			std::lock_guard<std::mutex> lock(mu);
			mark_returned = true;
		}
		file_exists = false; // 段階 (1) が設定のフォルダーを空にする
	});
	// フラグを立てる側が mark() に入った (フラグが立った) のを見てから、まだ戻っていないことを確認する。
	for (int i = 0; i < 500 && !mark.is_marked(); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	REQUIRE(mark.is_marked());
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	{
		std::lock_guard<std::mutex> lock(mu);
		CHECK_FALSE(mark_returned); // フラグを立てる側は、実行中の書き込みが終わるまで待つ
		release_writer = true;
	}
	cv.notify_all();
	writer.join();
	uninstaller.join();
	CHECK(mark.is_marked());
	CHECK_FALSE(file_exists.load());

	// 後から来る一覧の変更 (録画ファイルの作成が済んだ) は断られ、ファイルを作り直さない。
	bool called = false;
	CHECK_FALSE(mark.write_unless_marked([&]() {
		called = true;
		file_exists = true;
		return true;
	}));
	CHECK_FALSE(called);
	CHECK_FALSE(file_exists.load());
}

TEST_CASE("展開したフォルダーは、plugins/<モジュール名> の形のときだけ、そのフォルダーになる")
{
	CHECK(extracted_folder_from_module_dir("C:/ProgramData/obs-studio/plugins/cordmux", "cordmux") ==
	      std::optional<std::string>("C:/ProgramData/obs-studio/plugins/cordmux"));
	// ポータブル版の OBS の plugins フォルダーも同じ形。
	CHECK(extracted_folder_from_module_dir("C:/obs-studio/plugins/cordmux", "cordmux") ==
	      std::optional<std::string>("C:/obs-studio/plugins/cordmux"));
	// 区切りは \ でもよく、末尾の区切りは無視する (返す値に末尾の区切りは付かない)。
	CHECK(extracted_folder_from_module_dir("C:\\ProgramData\\obs-studio\\plugins\\cordmux\\", "cordmux") ==
	      std::optional<std::string>("C:\\ProgramData\\obs-studio\\plugins\\cordmux"));
	CHECK(extracted_folder_from_module_dir("C:/ProgramData/obs-studio/plugins/cordmux//", "cordmux") ==
	      std::optional<std::string>("C:/ProgramData/obs-studio/plugins/cordmux"));
	// 大文字と小文字は区別しない (Windows の場所)。
	CHECK(extracted_folder_from_module_dir("D:/x/PLUGINS/CordMux", "cordmux") == std::optional<std::string>("D:/x/PLUGINS/CordMux"));
	// <名前>/bin/64bit の置き方では返さない。
	CHECK_FALSE(extracted_folder_from_module_dir("C:/ProgramData/obs-studio/plugins/cordmux/bin/64bit", "cordmux").has_value());
	CHECK_FALSE(extracted_folder_from_module_dir("C:\\ProgramData\\obs-studio\\plugins\\cordmux\\bin\\64bit\\", "cordmux").has_value());
	// OBS 自身の obs-plugins/64bit の形では、OBS のフォルダーを返さない。
	CHECK_FALSE(extracted_folder_from_module_dir("C:/obs/obs-plugins/64bit", "cordmux").has_value());
	// plugins の下の名前がモジュールの名前と違う。
	CHECK_FALSE(extracted_folder_from_module_dir("C:/obs/plugins/other", "cordmux").has_value());
	// 親の名前が plugins でない (plugins の名前が似ているだけのフォルダーも含む)。
	CHECK_FALSE(extracted_folder_from_module_dir("C:/obs/data/cordmux", "cordmux").has_value());
	CHECK_FALSE(extracted_folder_from_module_dir("C:/obs/my-plugins/cordmux", "cordmux").has_value());
	// 途中までの場所は返さない。
	CHECK_FALSE(extracted_folder_from_module_dir("plugins", "cordmux").has_value());
	CHECK_FALSE(extracted_folder_from_module_dir("C:/obs/plugins", "cordmux").has_value());
	// plugins がルートの直下のときは返さない (上に 1 階層が必要)。
	CHECK_FALSE(extracted_folder_from_module_dir("plugins/cordmux", "cordmux").has_value());
	CHECK_FALSE(extracted_folder_from_module_dir("", "cordmux").has_value());
	CHECK_FALSE(extracted_folder_from_module_dir("C:/obs/plugins/cordmux", "").has_value());
}

TEST_CASE("次のアンインストールのために残した設定のフォルダーは、止まった段階の結果からコピーされる")
{
	SUBCASE("段階 (3) がフォルダーを返して止まると、結果にフォルダーと残りの is_file が入る")
	{
		Recorder rec;
		rec.step3 = left(RemovalItem::VencordDirValue, "C:/Vesktop/state.json");
		rec.step3.leftovers[0].is_file = true;
		rec.step3.settings_dir = "C:/obs/plugin_config/cordmux";
		const UninstallReport report = run_uninstall(rec.ops());
		CHECK_FALSE(report.completed);
		CHECK(report.settings_dir == "C:/obs/plugin_config/cordmux");
		REQUIRE(report.leftovers.size() == 1);
		CHECK(report.leftovers[0].is_file);
	}
	SUBCASE("段階 (3) がフォルダーを返さずに止まると (バックアップがない)、空になる")
	{
		Recorder rec;
		rec.step3 = left(RemovalItem::VencordDirValue, "C:/Vesktop/state.json");
		CHECK(run_uninstall(rec.ops()).settings_dir.empty());
	}
	SUBCASE("フォルダーを返しても残りがなければ止まらず、完了した結果にフォルダーは入らない")
	{
		Recorder rec;
		rec.step3.settings_dir = "C:/obs/plugin_config/cordmux";
		const UninstallReport report = run_uninstall(rec.ops());
		CHECK(report.completed);
		CHECK(report.settings_dir.empty());
	}
	SUBCASE("段階 (2) で止まると、止まった段階の結果だけがコピーされる")
	{
		Recorder rec;
		rec.step1.settings_dir = "C:/obs/plugin_config/cordmux";
		rec.step2 = left(RemovalItem::CordmuxSources, "Sources A");
		CHECK(run_uninstall(rec.ops()).settings_dir.empty());
	}
}

TEST_CASE("シーンコレクションのフォルダーを読めなかったことも結果に入るが、アンインストールは止まらない")
{
	Recorder rec;
	rec.step2.collection_folder_unreadable = true;
	const UninstallReport report = run_uninstall(rec.ops());
	CHECK(report.completed);
	CHECK(report.collection_folder_unreadable);
	CHECK(report.leftovers.empty());

	Recorder normal;
	CHECK_FALSE(run_uninstall(normal.ops()).collection_folder_unreadable);
}
