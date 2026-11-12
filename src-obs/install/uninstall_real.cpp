// SPDX-License-Identifier: Apache-2.0

// install/uninstall.h の、実際の platform/ と OBS の関数をまとめる側。OBS の frontend API と sources/ を使うので、
// 製品のビルドにだけ入る (align_tests には入れない。install/install.cmake)。ここでするのは、実際の関数をつなぐ
// ことと、ログに 1 行ずつ書くことだけで、順序、止め方、何を消すかは uninstall.cpp の純粋関数が決める
// (テストはそちらを確認する)。シーンコレクションの扱いは OBS 31.1.1 の frontend の実装に合わせてある (関数ごとの
// コメントにある)。

#include "install/uninstall.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>

#include "diag/diag_log.h"
#include "platform/bundle_check.h"
#include "platform/platform.h"
#include "sources/cordmux_removal.h"

namespace cordmux::install {

namespace {

std::string take_bstr(char *text)
{
	std::string out = text != nullptr ? text : "";
	bfree(text);
	return out;
}

// obs_frontend_get_scene_collections の戻り値は、1 回の確保で配列と文字列を持つ (bfree 1 回で解放する。
// OBS 31.1.1 の frontend/api/obs-frontend-api.cpp の convert_string_list)。
std::vector<std::string> take_bstr_list(char **list)
{
	std::vector<std::string> out;
	if (list == nullptr)
		return out;
	for (char **it = list; *it != nullptr; ++it)
		out.emplace_back(*it);
	bfree(list);
	return out;
}

std::string current_collection_name()
{
	return take_bstr(obs_frontend_get_current_scene_collection());
}

std::vector<std::string> list_collection_names()
{
	return take_bstr_list(obs_frontend_get_scene_collections());
}

// 表示名でシーンコレクションを開き、開けたかを、呼び出した後の現在の表示名で確認する。
// obs_frontend_set_current_scene_collection は、表示名でメニューの項目を探して押す。UI のスレッドから呼び出すと、
// 切り替えは同期して終わり、切り替える前に現在のシーンコレクションを保存する (OBS 31.1.1 の
// frontend/OBSStudioAPI.cpp と、frontend/widgets/OBSBasic_SceneCollections.cpp の ChangeSceneCollection と
// ActivateSceneCollection)。
// ChangeSceneCollection は、メニューにある名前が OBS の持つシーンコレクションの一覧にないと例外を投げる。
// メニューは一覧から作り直されるので、実際には起きない見込みで、try/catch は念のためのガードにすぎない。
// obs_frontend_set_current_scene_collection は extern "C" の関数で、製品は /EHsc でビルドするので、コンパイラは
// この try/catch を省いてよく、例外を受け止められる保証はない。
bool open_collection(const std::string &name)
{
	if (name.empty())
		return false;
	// 念のためのガード (上のコメント。受け止められる保証はない)。
	try {
		obs_frontend_set_current_scene_collection(name.c_str());
	} catch (...) {
		diag::logf(LOG_WARNING, "Uninstall: switching the scene collection failed; it's treated as not opened");
		return false;
	}
	return current_collection_name() == name;
}

// シーンコレクションのフォルダーの一覧が読めなければ nullopt。
std::optional<std::vector<CollectionFile>> read_collection_files(const std::string &dir)
{
	std::vector<platform::DirEntry> entries;
	if (!platform::list_directory(dir, entries))
		return std::nullopt;
	std::vector<CollectionFile> files;
	for (const platform::DirEntry &entry : entries) {
		if (entry.is_directory)
			continue;
		CollectionFile file;
		file.file_name = entry.name;
		if (!collection_display_name(file))
			continue; // .json でない (OBS も読まない)
		const std::string path = platform::join_path(dir, entry.name);
		// OBS が一覧を作るときと同じ読み方 (壊れていれば .bak を読む)。
		obs_data_t *data = obs_data_create_from_json_file_safe(path.c_str(), "bak");
		if (data != nullptr) {
			const char *name = obs_data_get_string(data, "name");
			file.name_in_file = name != nullptr ? name : "";
			obs_data_release(data);
		}
		files.push_back(std::move(file));
	}
	return files;
}

// 同じ表示名のシーンコレクションは、OBS の一覧に 1 つしか載らない (RefreshSceneCollectionCache の try_emplace)。
// 2 つ目を見つけるには、シーンコレクションのフォルダーを自分で読むしかない。フォルダーは global.ini の Locations の
// SceneCollections (なければ既定の値) の下の obs-studio/basic/scenes である (OBS 31.1.1 の frontend/OBSApp.cpp の
// InitGlobalLocationDefaults と InitGlobalConfig、frontend/widgets/OBSBasic_SceneCollections.cpp の
// SceneCollectionPath)。ポータブル版の OBS は既定の値だけを使い、ほかの OBS も設定の値の場所がなければ既定の値を
// 使う (InitGlobalConfig) ので、設定の値のフォルダーが読めなければ、既定の値のフォルダーを読む。どちらも読めなければ、
// 重複を調べるのをあきらめて nullopt を返す (ログにも 1 行書く。シーンコレクションを開くことは止めず、結果の
// 「確認できなかったもの」に入れる)。
// OBS には、シーンコレクションのファイルの場所を返す関数がない (OBS 31.1.1 と 33.0.0 の obs-frontend-api.h)。
// global.ini の [Locations] SceneCollections を obs_frontend_get_app_config と config_get_string で読み、その下の
// obs-studio/basic/scenes を見る組み立ては、OBS 33.0.0 でも同じである (frontend/OBSApp.cpp の InitGlobalConfig が
// 読む SceneCollections の値を userScenesLocation に入れ、frontend/widgets/OBSBasic_SceneCollections.cpp の
// RefreshSceneCollectionCache が userScenesLocation の下の obs-studio/basic/scenes を読む)。
std::optional<std::vector<CollectionFile>> list_collection_files()
{
	config_t *app = obs_frontend_get_app_config();
	if (app == nullptr) {
		diag::logf(LOG_WARNING,
			   "Uninstall: OBS settings unavailable, so scene collection names weren't checked for "
			   "duplicates");
		return std::nullopt;
	}
	std::vector<std::string> bases;
	const char *configured = config_get_string(app, "Locations", "SceneCollections");
	if (configured != nullptr && *configured != '\0')
		bases.emplace_back(configured);
	const char *fallback = config_get_default_string(app, "Locations", "SceneCollections");
	if (fallback != nullptr && *fallback != '\0' && (bases.empty() || !platform::same_dir_path(bases.front(), fallback)))
		bases.emplace_back(fallback);

	for (const std::string &base : bases) {
		if (auto files = read_collection_files(platform::join_path(base, "obs-studio/basic/scenes")))
			return std::move(*files);
	}
	diag::logf(LOG_WARNING,
		   "Uninstall: couldn't read the scene collection folder, so names weren't checked for duplicates");
	return std::nullopt;
}

CollectionWalkOps make_real_walk_ops()
{
	CollectionWalkOps ops;
	ops.current_collection = &current_collection_name;
	ops.list_collections = &list_collection_names;
	ops.open_collection = &open_collection;
	ops.remove_here = &sources::remove_cordmux_objects;
	ops.list_collection_files = &list_collection_files;
	ops.save = [] { obs_frontend_save(); };
	return ops;
}

// トークンの保存先を、今ほかの OBS が使っているか (UninstallFileOps::local_data_used_by_other)。session.json を
// 書いたのが自分か、もう動いていない OBS なら、使っていない。
//
// read_session_file は、トークンの保存先がなければ作る (platform/platform_win.cpp の session_file_path_w)。
// 段階 (1) はこの確認の後にその保存先を消すので、順番を入れ替えない。
bool local_data_used_by_other()
{
	platform::SessionInfo info;
	if (!platform::read_session_file(info))
		return false;
	const platform::ProcessIdentity self = platform::current_process_identity();
	if (info.writer_pid == self.pid && info.writer_started_at == self.started_at)
		return false;
	return platform::is_process_alive(info.writer_pid, info.writer_started_at);
}

// DLL の名前から拡張子を除いたもの (OBS がフォルダーの名前として探す文字列)。
std::string module_name()
{
	const char *file = obs_get_module_file_name(obs_current_module());
	std::string name = file != nullptr ? file : "";
	const std::size_t dot = name.rfind('.');
	if (dot != std::string::npos)
		name.resize(dot);
	return name;
}

// 残ったものをログに書くときの語句。英語で書き、列挙子の名前は書かない。
const char *removal_item_log_name(RemovalItem item)
{
	switch (item) {
	case RemovalItem::CordmuxSources:
		return "cordmux sources";
	case RemovalItem::ModuleConfigFiles:
		return "cordmux settings files";
	case RemovalItem::LocalDataFolder:
		return "the connection details folder";
	case RemovalItem::VencordDirValue:
		return "Vesktop's Vencord location";
	case RemovalItem::ExtractedFolder:
		return "the cordmux plugin folder";
	case RemovalItem::Recordings:
		return "recordings";
	case RemovalItem::ObsDockLayout:
		return "the dock layout saved by OBS";
	case RemovalItem::ObsLogs:
		return "OBS logs";
	case RemovalItem::ObsPluginManagerRecord:
		return "the OBS plugin manager record";
	}
	return "an item";
}

// 段階の結果をログに書く。場所の文字列は利用者の名前を含むので書かない。
void log_step(int number, const UninstallStepResult &result)
{
	diag::logf(result.leftovers.empty() ? LOG_INFO : LOG_WARNING,
		   "Uninstall step %d done (left=%d collections_not_opened=%d duplicate_collection_names=%d)", number,
		   static_cast<int>(result.leftovers.size()), static_cast<int>(result.collections_not_opened.size()),
		   static_cast<int>(result.collections_duplicate_names.size()));
	for (const UninstallLeftover &left : result.leftovers)
		diag::logf(LOG_WARNING,
			   "Uninstall step %d couldn't remove %s", number, removal_item_log_name(left.item));
}

} // namespace

std::string real_folder_to_delete()
{
	return extracted_folder_from_module_dir(platform::module_dir(), module_name()).value_or(std::string());
}

UninstallFileOps make_real_uninstall_file_ops()
{
	UninstallFileOps ops;
	ops.delete_file = &platform::delete_file;
	ops.remove_directory_if_empty = &platform::remove_directory_if_empty;
	ops.list_directory = &platform::list_directory;
	ops.read_file_bytes = &platform::read_file_bytes;
	ops.write_file_atomic = &platform::write_file_atomic;
	ops.vesktop_state_json_path = &platform::vesktop_state_json_path;
	ops.vesktop_settings_json_path = &platform::vesktop_settings_json_path;
	ops.local_data_dir = &platform::local_data_dir;
	ops.local_data_used_by_other = &local_data_used_by_other;
	ops.plan_restore = &plan_restore;
	ops.vesktop_running = &platform::vesktop_running_now;
	return ops;
}

UninstallOps make_real_uninstall_ops(const UninstallContext &context)
{
	const auto file_ops = std::make_shared<const UninstallFileOps>(make_real_uninstall_file_ops());

	UninstallOps ops;
	ops.mark_and_stop_listening = [stop = context.stop_listening]() {
		mark_uninstalling();
		if (stop) {
			stop();
			diag::logf(LOG_INFO, "Uninstall step 0 done (stopped waiting for Vesktop)");
		} else {
			diag::logf(LOG_WARNING,
				   "Uninstall step 0: couldn't stop waiting for Vesktop (no way to close it was "
				   "given)");
		}
	};
	ops.remove_settings_and_secret = [file_ops, context]() {
		UninstallStepResult result = remove_settings_and_secret(*file_ops, context);
		log_step(1, result);
		return result;
	};
	ops.remove_sources_and_scenes = []() {
		UninstallStepResult result =
			walk_collections(make_real_walk_ops(), product_items_for_step(UninstallStep::SourcesAndScenes));
		log_step(2, result);
		return result;
	};
	ops.restore_vencord_dir = [file_ops, context]() {
		UninstallStepResult result = restore_vencord_dir(*file_ops, context);
		log_step(3, result);
		return result;
	};
	ops.folder_to_delete = []() {
		const std::string folder = real_folder_to_delete();
		// 段階の番号は書かない。途中で止まったときにもこの関数を呼び出すので、段階 (4) まで進んでいないのに
		// "step 4" と書くことになる。ログレベルは LOG_INFO にする。手順書と違う置き方は失敗ではなく、画面も
		// メッセージを表示せずに、消すものの場所と名前を表示する普通の結果だからである。ログはここで 1 回だけ書く
		// (確認のダイアログの前に real_folder_to_delete を呼び出す所では書かない)。
		if (folder.empty())
			diag::logf(LOG_INFO,
				   "Uninstall: no single folder to delete (cordmux isn't installed in a plugins/<name> folder)");
		return folder;
	};
	// あって空でないときだけ表示する (record_dir_if_has_files)。見るのは結果を作るこの 1 回だけである
	// (アンインストールは録画を止めた後に行うので、この後に中身は増えない)。
	ops.record_dir = [dir = context.record_dir]() { return record_dir_if_has_files(dir, &platform::list_directory); };
	return ops;
}

} // namespace cordmux::install
