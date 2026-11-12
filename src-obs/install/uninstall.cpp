// SPDX-License-Identifier: Apache-2.0

// install/uninstall.h の、libobs を含まない側の実装。
// 実際の platform/ と OBS の関数をまとめる部分は uninstall_real.cpp (製品のビルドにだけ入る) にある。ここにある
// ものは、どれも注入された関数を通してだけファイルと OBS に触る (テストが、テスト用の関数で確認する)。

#include "install/uninstall.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <utility>

#include "platform/bundle_check.h"
#include "record/record_segment.h"

namespace cordmux::install {

namespace {

UninstallMark &process_mark()
{
	static UninstallMark mark;
	return mark;
}

bool step_has_leftovers(const UninstallStepResult &result)
{
	return !result.leftovers.empty();
}

void append_unique(std::vector<std::string> &out, const std::string &value)
{
	if (std::find(out.begin(), out.end(), value) == out.end())
		out.push_back(value);
}

// where がファイルかフォルダーか (UninstallLeftover::is_file)。場所が空のときと、シーンコレクションの表示名の
// ときは kFolder にする (読む側は場所の行を置かない)。
constexpr bool kFile = true;
constexpr bool kFolder = false;

void add_leftover(UninstallStepResult &result, RemovalItem item, const std::string &where, bool is_file)
{
	for (const UninstallLeftover &existing : result.leftovers) {
		if (existing.item == item && existing.where == where)
			return;
	}
	result.leftovers.push_back(UninstallLeftover{item, where, is_file});
}

// 段階の関数がないときの結果。その段階の Product の行を、場所なしで結果に入れる (run_uninstall のコメント)。
UninstallStepResult missing_step_result(UninstallStep step)
{
	UninstallStepResult result;
	for (RemovalItem item : product_items_for_step(step))
		add_leftover(result, item, std::string(), kFolder);
	return result;
}

bool equals_ignoring_ascii_case(std::string_view a, std::string_view b)
{
	if (a.size() != b.size())
		return false;
	for (std::size_t i = 0; i < a.size(); ++i) {
		const auto ca = static_cast<unsigned char>(a[i]);
		const auto cb = static_cast<unsigned char>(b[i]);
		if (std::tolower(ca) != std::tolower(cb))
			return false;
	}
	return true;
}

bool is_separator(char c)
{
	return c == '/' || c == '\\';
}

// 場所を、親のフォルダーと最後の名前に分ける。区切りがなければ nullopt。
std::optional<std::pair<std::string, std::string>> split_parent(std::string_view path)
{
	std::size_t end = path.size();
	while (end > 0 && is_separator(path[end - 1]))
		--end;
	if (end == 0)
		return std::nullopt;
	std::size_t cut = end;
	while (cut > 0 && !is_separator(path[cut - 1]))
		--cut;
	if (cut == 0)
		return std::nullopt;
	std::string parent(path.substr(0, cut - 1));
	std::string name(path.substr(cut, end - cut));
	if (parent.empty() || name.empty())
		return std::nullopt;
	return std::make_pair(std::move(parent), std::move(name));
}

// ---- 段階 (1) フォルダーの中身を消す ----

// dir の中身を消し、keep_path だけは残す。残したものがなければ dir 自身も消す。
// 消せなかったものは、item の行として結果に入れる。
void clear_folder(const UninstallFileOps &ops, const std::string &dir, const std::string &keep_path,
		  RemovalItem item, int depth, UninstallStepResult &result)
{
	if (dir.empty()) {
		add_leftover(result, item, std::string(), kFolder);
		return;
	}
	if (!ops.list_directory || !ops.delete_file || !ops.remove_directory_if_empty) {
		add_leftover(result, item, dir, kFolder);
		return;
	}

	std::vector<platform::DirEntry> entries;
	if (!ops.list_directory(dir, entries)) {
		// 一覧が読めない。もうない (か空) なら消したものとして扱い、中身が残っていれば結果に入れる。
		// この関数は cordmux 自身の保存先にしか使わない (空なら消してよいフォルダーである)。
		if (!ops.remove_directory_if_empty(dir))
			add_leftover(result, item, dir, kFolder);
		return;
	}

	const std::size_t leftovers_before = result.leftovers.size();
	bool kept = false;
	for (const platform::DirEntry &entry : entries) {
		const std::string path = platform::join_path(dir, entry.name);
		if (!keep_path.empty() && platform::same_dir_path(path, keep_path)) {
			kept = true;
			continue;
		}
		// シンボリックリンクとジャンクションは、中へ入らずにリンクそのものだけを消す (platform/platform.h の
		// DirEntry::is_link の約束)。フォルダーを指すリンクは is_directory も true なので、先に見ないと、
		// 指す先 (cordmux の保存先の外にある利用者のファイル) を 1 つずつ消してしまう (消すのは cordmux の
		// ファイルだけにする)。どちらの関数も、指す先には触らない。消せなければ、リンクの場所を結果に入れる。
		if (entry.is_link) {
			const bool removed =
				entry.is_directory ? ops.remove_directory_if_empty(path) : ops.delete_file(path);
			if (!removed)
				add_leftover(result, item, path, entry.is_directory ? kFolder : kFile);
			continue;
		}
		if (entry.is_directory) {
			if (depth + 1 > kMaxFolderDepth) {
				add_leftover(result, item, path, kFolder);
				continue;
			}
			clear_folder(ops, path, std::string(), item, depth + 1, result);
			continue;
		}
		if (!ops.delete_file(path))
			add_leftover(result, item, path, kFile);
	}

	if (kept)
		return;
	if (!ops.remove_directory_if_empty(dir) && result.leftovers.size() == leftovers_before) {
		// 中身は全部消せたのにフォルダーが消えない (途中でほかの処理が書いたなど)。フォルダーを結果に入れる。
		add_leftover(result, item, dir, kFolder);
	}
}

// ---- 段階 (3) バックアップと state.json ----

enum class Presence {
	Absent,
	Present,
	Unknown,
};

// path の親のフォルダーを読んで、path があるかを見る (何も消さない)。
Presence file_presence(const UninstallFileOps &ops, const std::string &path)
{
	const auto parts = split_parent(path);
	if (!parts || !ops.list_directory)
		return Presence::Unknown;
	std::vector<platform::DirEntry> entries;
	if (!ops.list_directory(parts->first, entries))
		return Presence::Unknown;
	for (const platform::DirEntry &entry : entries) {
		if (!entry.is_directory && equals_ignoring_ascii_case(entry.name, parts->second))
			return Presence::Present;
	}
	return Presence::Absent;
}

// バックアップがあるか。バックアップは cordmux 自身の保存先にあるので、一覧が読めないときは、空のフォルダーの
// 削除を試して、もうないことを確認してよい。
Presence backup_presence(const UninstallFileOps &ops, const std::string &backup_path)
{
	const Presence presence = file_presence(ops, backup_path);
	if (presence != Presence::Unknown)
		return presence;
	const auto parts = split_parent(backup_path);
	if (parts && ops.remove_directory_if_empty && ops.remove_directory_if_empty(parts->first))
		return Presence::Absent;
	return Presence::Unknown;
}

// state.json の本文の vencordDir が data/vencord を指しているか (比べ方は、Vesktop の設定を調べる側と同じ)。
bool points_at_bundled(const std::string &state_text, const UninstallContext &context)
{
	if (context.bundled_vencord_dir.empty())
		return false;
	const std::optional<std::string> value = platform::parse_vencord_dir(state_text);
	return value.has_value() && platform::same_dir_path(*value, context.bundled_vencord_dir);
}

// 元に戻し終えた (か、戻すものがなかった) ので、バックアップを消し、設定のフォルダーを消す
// (restore_vencord_dir のコメント)。
void finish_backup(const UninstallFileOps &ops, const UninstallContext &context, bool backup_exists,
		   UninstallStepResult &result)
{
	if (backup_exists) {
		if (!ops.delete_file || !ops.delete_file(context.backup_json_path)) {
			add_leftover(result, RemovalItem::ModuleConfigFiles, context.backup_json_path, kFile);
			return;
		}
	}
	if (context.module_config_dir.empty())
		return;
	if (!ops.remove_directory_if_empty || !ops.remove_directory_if_empty(context.module_config_dir))
		add_leftover(result, RemovalItem::ModuleConfigFiles, context.module_config_dir, kFolder);
}

// Vesktop が起動していないと分かったか (ops.vesktop_running が false を返した)。関数がないときと、起動しているか
// 調べられないときは偽 (書かない側にする。ledger/actions.cpp の vesktop_busy_reason)。
bool vesktop_stopped(const UninstallFileOps &ops)
{
	const std::optional<bool> running = ops.vesktop_running ? ops.vesktop_running() : std::nullopt;
	return running.has_value() && !*running;
}

// plugin_entry には、バックアップを読めて形が正しかったときだけ、そのバックアップ (Backup::plugin_entry) を入れる。
// 入れなければ、呼び出し元は Vencord の設定に触らない。vesktop_checked には、state.json を書く直前に Vesktop が
// 起動していないことを確認したときに true を入れる (呼び出し元は、Vencord の設定を書く前の確認を繰り返さない)。
void restore_state_json_key(const UninstallFileOps &ops, const UninstallContext &context, RemovalItem item,
			    UninstallStepResult &result, PluginEntryBackup &plugin_entry, bool &vesktop_checked,
			    std::string &recorded_state_path)
{
	std::string state_path;
	bool have_state_path = ops.vesktop_state_json_path && ops.vesktop_state_json_path(state_path);
	if (!have_state_path)
		state_path.clear();

	// 元に戻せなかったときの結果。state.json はファイルの行にする。バックアップが残っている (ある、またはあるか
	// 分からない = フォルダーがあり中身がある) ときは、バックアップを残した設定のフォルダーも返す
	// (restore_vencord_dir のコメント)。バックアップの場所が分からないときと、バックアップがないときは返さない
	// (ないフォルダーや、バックアップを持たないフォルダーを、削除しないでほしい場所として表示しない)。
	const auto not_restored = [&result, &context, &state_path, item](bool backup_kept) {
		add_leftover(result, item, state_path, kFile);
		if (backup_kept)
			result.settings_dir = context.module_config_dir;
	};

	if (context.backup_json_path.empty()) {
		not_restored(/*backup_kept=*/false);
		return;
	}

	const Presence backup = backup_presence(ops, context.backup_json_path);
	if (backup == Presence::Unknown) {
		// バックアップがあるかどうかも分からない (cordmux が書いたかどうかが分からない)。2 つの関数があれば、
		// 一覧が読めず、空のフォルダーの削除も失敗した = フォルダーがあり中身がある (バックアップがそこに
		// ありうる) ので、設定のフォルダーを返す。関数がない (渡し忘れた) ときは、何も見ていないので返さない。
		not_restored(/*backup_kept=*/static_cast<bool>(ops.list_directory) &&
			     static_cast<bool>(ops.remove_directory_if_empty));
		return;
	}

	if (backup == Presence::Absent) {
		// cordmux は書いていない (か、バックアップを失った)。現在の値が data/vencord を指しているときだけ、元に
		// 戻せないものとして結果に入れる。state.json が読めないときは、書いた証拠がないので数えない。
		std::string state_text;
		if (have_state_path && ops.read_file_bytes &&
		    ops.read_file_bytes(state_path, state_text, context.max_json_bytes) &&
		    points_at_bundled(state_text, context)) {
			not_restored(/*backup_kept=*/false);
			return;
		}
		finish_backup(ops, context, /*backup_exists=*/false, result);
		return;
	}

	// バックアップがある。読めて形が正しいときだけ元に戻す。
	std::string backup_text;
	Backup backup_value;
	if (!ops.read_file_bytes || !ops.read_file_bytes(context.backup_json_path, backup_text, context.max_json_bytes) ||
	    !backup_from_json(backup_text, &backup_value)) {
		not_restored(/*backup_kept=*/true);
		return;
	}
	plugin_entry = backup_value.plugin_entry;
	// 戻す state.json は、バックアップに残した、書いた場所を先に使う (現在の環境変数から決めた場所が違っていても、
	// 書いた場所を戻す)。古い版のバックアップにはないので、そのときは現在の場所を使う。
	if (!backup_value.state_path.empty()) {
		state_path = backup_value.state_path;
		recorded_state_path = backup_value.state_path;
		have_state_path = true;
	}
	if (!have_state_path) {
		not_restored(/*backup_kept=*/true);
		return;
	}

	std::string state_text;
	if (!ops.read_file_bytes(state_path, state_text, context.max_json_bytes)) {
		// state.json がなければ、Vesktop は既定の設定で起動するので、戻すものがない。
		if (file_presence(ops, state_path) == Presence::Absent) {
			finish_backup(ops, context, /*backup_exists=*/true, result);
			return;
		}
		not_restored(/*backup_kept=*/true);
		return;
	}

	const RestorePlan plan = ops.plan_restore ? ops.plan_restore(state_text, backup_value) : RestorePlan{};
	switch (plan.action) {
	case RestoreAction::Write:
		// 書く直前に、Vesktop が起動していないことを確認する ([Vesktopに追加] と [Vesktopから削除] と同じ)。押す前の
		// 確認のダイアログの前と後で Vesktop の状態を見ているが、その後に起動されることがある。起動中の Vesktop は
		// state.json をメモリの値で書き直し、戻した値が消える。起動していたら (調べられなくても) 書かず、書けなかった
		// ときと同じく、元に戻せなかった設定として結果に入れて止める (バックアップは残す)。
		if (!context.allow_state_json_write || !ops.write_file_atomic) {
			not_restored(/*backup_kept=*/true);
			return;
		}
		vesktop_checked = true;
		if (!vesktop_stopped(ops) || !ops.write_file_atomic(state_path, plan.new_text)) {
			not_restored(/*backup_kept=*/true);
			return;
		}
		break;
	case RestoreAction::AlreadyRestored:
		break;
	case RestoreAction::Refuse:
		// インストールの後に利用者が選んだ値は残す。ただし data/vencord を指していれば、フォルダーを消すと
		// Vesktop が公式の Vencord をダウンロードするので、結果に入れる。
		if (plan.refuse_reason != RefuseReason::ForeignChange || points_at_bundled(state_text, context)) {
			not_restored(/*backup_kept=*/true);
			return;
		}
		break;
	}
	finish_backup(ops, context, /*backup_exists=*/true, result);
}

// Vencord の設定の cordmux のプラグインの項を、バックアップの値に合わせて戻す (restore_vencord_dir のコメント)。
// 失敗は黙って何もしない。vesktop_checked が偽なら、書く前に Vesktop が起動していないことを確認し、起動していれば
// (調べられなくても) 書かない (Vesktop は Vencord の設定もメモリの値で書き直す)。
void restore_vencord_plugin_entry(const UninstallFileOps &ops, const UninstallContext &context,
				  const PluginEntryBackup &plugin_entry, bool vesktop_checked,
				  const std::string &recorded_state_path)
{
	if (!context.allow_state_json_write || !ops.vesktop_settings_json_path || !ops.read_file_bytes ||
	    !ops.write_file_atomic)
		return;
	if (plugin_entry.state == PluginEntryState::Unknown)
		return; // 触らない (確認のためにプロセスの一覧を読まない)
	// バックアップに書いた場所があれば、その state.json と同じフォルダーの Vencord の設定を戻す。
	std::string path;
	if (!recorded_state_path.empty())
		path = platform::vesktop_settings_json_beside(recorded_state_path);
	if (path.empty() && !ops.vesktop_settings_json_path(path))
		return;
	std::string text;
	if (!ops.read_file_bytes(path, text, context.max_json_bytes))
		return;
	const std::optional<std::string> next = plan_restore_plugin_entry(text, kVencordPluginName, plugin_entry);
	if (!next.has_value())
		return;
	if (!vesktop_checked && !vesktop_stopped(ops))
		return;
	ops.write_file_atomic(path, *next);
}

} // namespace

// ---- アンインストール中のフラグ ----

void UninstallMark::mark()
{
	marked_.store(true);
	// フラグを立てた後にロックを 1 回取る (install/uninstall.h の write_unless_uninstalling のコメント)。
	// フラグを見る前に確認を通り過ぎた書き込みが実行中なら、それが終わるまでここで待つ。以後の書き込みは、
	// 必ずフラグを見る。
	std::lock_guard<std::mutex> lock(write_mu_);
}

bool UninstallMark::write_unless_marked(const std::function<bool()> &write)
{
	std::lock_guard<std::mutex> lock(write_mu_);
	if (marked_.load() || !write)
		return false;
	return write();
}

void mark_uninstalling()
{
	process_mark().mark();
}

bool is_uninstalling()
{
	return process_mark().is_marked();
}

bool write_unless_uninstalling(const std::function<bool()> &write)
{
	return process_mark().write_unless_marked(write);
}

// ---- 表から段階ごとの対象を取り出す ----

std::vector<RemovalItem> product_items_for_step(UninstallStep step)
{
	std::vector<RemovalItem> out;
	for (const RemovalRow &row : kRemovalTable) {
		if (row.remover == Remover::Product && row.step == step)
			out.push_back(row.item);
	}
	return out;
}

std::optional<Locate> locate_of(RemovalItem item)
{
	for (const RemovalRow &row : kRemovalTable) {
		if (row.item == item)
			return row.locate;
	}
	return std::nullopt;
}

// ---- ファイルの段階 ----

UninstallStepResult remove_settings_and_secret(const UninstallFileOps &ops, const UninstallContext &context)
{
	UninstallStepResult result;
	for (RemovalItem item : product_items_for_step(UninstallStep::SettingsAndSecret)) {
		const std::optional<Locate> locate = locate_of(item);
		if (!locate) {
			add_leftover(result, item, std::string(), kFolder);
			continue;
		}
		switch (*locate) {
		case Locate::ModuleConfigDir:
			clear_folder(ops, context.module_config_dir, context.backup_json_path, item, 0, result);
			break;
		case Locate::LocalDataDir: {
			const std::string dir = ops.local_data_dir ? ops.local_data_dir() : std::string();
			if (dir.empty()) {
				add_leftover(result, item, std::string(), kFolder);
				break;
			}
			if (ops.local_data_used_by_other && ops.local_data_used_by_other()) {
				add_leftover(result, item, dir, kFolder);
				break;
			}
			clear_folder(ops, dir, std::string(), item, 0, result);
			break;
		}
		default:
			// この段階で扱わない探し方 (表を変えたのに処理を追加していない)。消さずに結果に入れる。
			add_leftover(result, item, std::string(), kFolder);
			break;
		}
	}
	return result;
}

UninstallStepResult restore_vencord_dir(const UninstallFileOps &ops, const UninstallContext &context)
{
	UninstallStepResult result;
	PluginEntryBackup plugin_entry; // バックアップを読めなかったときは分からないのまま (Vencord の設定に触らない)
	bool vesktop_checked = false;   // state.json を書く直前に Vesktop を確認したか
	std::string recorded_state_path; // バックアップに残した、書いた state.json の場所 (なければ空)
	for (RemovalItem item : product_items_for_step(UninstallStep::RestoreVencordDir)) {
		const std::optional<Locate> locate = locate_of(item);
		if (locate && *locate == Locate::StateJsonKey)
			restore_state_json_key(ops, context, item, result, plugin_entry, vesktop_checked, recorded_state_path);
		else
			add_leftover(result, item, std::string(), kFolder);
	}
	if (!step_has_leftovers(result))
		restore_vencord_plugin_entry(ops, context, plugin_entry, vesktop_checked, recorded_state_path);
	return result;
}

// ---- [Vesktopから削除] ----

VesktopRemoveOutcome remove_from_vesktop(const UninstallFileOps &ops, const UninstallContext &context)
{
	// バックアップ。cordmux を追加する前の値はここにしかない (段階 (3) と同じ)。
	std::string backup_text;
	if (context.backup_json_path.empty() || !ops.read_file_bytes ||
	    !ops.read_file_bytes(context.backup_json_path, backup_text, context.max_json_bytes))
		return VesktopRemoveOutcome::NoBackup;
	Backup backup;
	if (!backup_from_json(backup_text, &backup))
		return VesktopRemoveOutcome::BackupInvalid;

	// 戻す state.json は、バックアップに残した場所を先に使う (restore_state_json_key と同じ)。
	std::string state_path = backup.state_path;
	if (state_path.empty() && (!ops.vesktop_state_json_path || !ops.vesktop_state_json_path(state_path)))
		return VesktopRemoveOutcome::StatePathUnknown;
	std::string state_text;
	if (!ops.read_file_bytes(state_path, state_text, context.max_json_bytes))
		return VesktopRemoveOutcome::StateUnreadable;

	// 計画は段階 (3) と同じ plan_restore で立てる (関数がなければ実際の関数を使う。段階 (3) は関数がないと
	// 戻せなかったものとして扱うが、ここは引数を省いた呼び出しを、書かない側に倒す理由がない)。
	const RestorePlan plan = ops.plan_restore ? ops.plan_restore(state_text, backup) : plan_restore(state_text, backup);
	if (plan.action == RestoreAction::Refuse)
		return VesktopRemoveOutcome::Refused;
	if (!context.allow_state_json_write)
		return VesktopRemoveOutcome::WritesDisabled;

	// 書く直前に、Vesktop が起動していないことを確認する (uninstall.h の remove_from_vesktop のコメント)。
	const std::optional<bool> running = ops.vesktop_running ? ops.vesktop_running() : std::nullopt;
	if (!running.has_value())
		return VesktopRemoveOutcome::VesktopUnknown;
	if (*running)
		return VesktopRemoveOutcome::VesktopRunning;

	if (plan.action == RestoreAction::Write &&
	    (!ops.write_file_atomic || !ops.write_file_atomic(state_path, plan.new_text)))
		return VesktopRemoveOutcome::StateWriteFailed;
	restore_vencord_plugin_entry(ops, context, backup.plugin_entry, /*vesktop_checked=*/true, backup.state_path);
	return plan.action == RestoreAction::Write ? VesktopRemoveOutcome::Restored
						   : VesktopRemoveOutcome::AlreadyRestored;
}

// ---- 段階 (4) の場所 ----

std::optional<std::string> extracted_folder_from_module_dir(std::string_view module_dir, std::string_view module_name)
{
	if (module_name.empty())
		return std::nullopt;

	// 末尾から 2 つの名前を取る: <module_name> / plugins。
	std::size_t end = module_dir.size();
	std::string_view names[2];
	std::size_t folder_end = 0;
	for (int i = 0; i < 2; ++i) {
		while (end > 0 && is_separator(module_dir[end - 1]))
			--end;
		if (end == 0)
			return std::nullopt;
		std::size_t start = end;
		while (start > 0 && !is_separator(module_dir[start - 1]))
			--start;
		names[i] = module_dir.substr(start, end - start);
		if (i == 0)
			folder_end = end;
		end = start;
	}
	if (!equals_ignoring_ascii_case(names[0], module_name) || !equals_ignoring_ascii_case(names[1], "plugins"))
		return std::nullopt;
	// plugins の上に少なくとも 1 階層あること (ルートそのものを返さない)。
	if (end == 0)
		return std::nullopt;
	return std::string(module_dir.substr(0, folder_end));
}

// ---- 段階 (2) シーンコレクションを順に開く ----

CollectionVisitPlan plan_collection_visits(const std::vector<std::string> &listed, const std::string &current)
{
	CollectionVisitPlan plan;
	plan.original = current;
	if (current.empty()) {
		for (const std::string &name : listed)
			append_unique(plan.not_visited, name);
		return plan;
	}
	for (const std::string &name : listed) {
		if (name == current)
			continue;
		append_unique(plan.others, name);
	}
	return plan;
}

std::optional<std::string> collection_display_name(const CollectionFile &file)
{
	static constexpr std::string_view kExtension = ".json";
	const std::string &name = file.file_name;
	if (name.size() <= kExtension.size())
		return std::nullopt;
	if (name.compare(name.size() - kExtension.size(), kExtension.size(), kExtension) != 0)
		return std::nullopt;
	if (!file.name_in_file.empty())
		return file.name_in_file;
	return name.substr(0, name.size() - kExtension.size());
}

std::vector<std::string> duplicate_collection_names(const std::vector<CollectionFile> &files)
{
	std::vector<std::string> seen;
	std::vector<std::string> duplicates;
	for (const CollectionFile &file : files) {
		const std::optional<std::string> name = collection_display_name(file);
		if (!name)
			continue;
		if (std::find(seen.begin(), seen.end(), *name) != seen.end())
			append_unique(duplicates, *name);
		else
			seen.push_back(*name);
	}
	return duplicates;
}

UninstallStepResult summarize_collection_walk(const std::vector<CollectionVisit> &visits,
					      const std::vector<RemovalItem> &items,
					      const std::vector<std::string> &not_opened,
					      const std::vector<std::string> &duplicate_names, bool folder_unreadable)
{
	UninstallStepResult result;
	result.collection_folder_unreadable = folder_unreadable;
	for (const CollectionVisit &visit : visits) {
		for (RemovalItem item : items) {
			bool reported = false;
			std::size_t remaining = 0;
			for (const RemovalCount &count : visit.remaining) {
				if (count.item == item) {
					reported = true;
					remaining += count.remaining;
				}
			}
			if (!reported || remaining > 0)
				add_leftover(result, item, visit.name, kFolder);
		}
	}
	for (const std::string &name : not_opened)
		append_unique(result.collections_not_opened, name);
	for (const std::string &name : duplicate_names)
		append_unique(result.collections_duplicate_names, name);
	return result;
}

UninstallStepResult walk_collections(const CollectionWalkOps &ops, const std::vector<RemovalItem> &items)
{
	const auto remove_here = [&ops, &items]() {
		return ops.remove_here ? ops.remove_here(items) : std::vector<RemovalCount>{};
	};

	const std::string original = ops.current_collection ? ops.current_collection() : std::string();
	const std::vector<std::string> listed = ops.list_collections ? ops.list_collections() : std::vector<std::string>{};
	const CollectionVisitPlan plan = plan_collection_visits(listed, original);
	// フォルダーを読めなかったときは、同じ名前があるか確認できなかったものとして結果に入れる (関数がないときは、
	// 確認する手段がないので同じ扱いにする)。
	const std::optional<std::vector<CollectionFile>> files =
		ops.list_collection_files ? ops.list_collection_files() : std::nullopt;
	const std::vector<std::string> duplicates = files ? duplicate_collection_names(*files) : std::vector<std::string>{};

	std::vector<CollectionVisit> visits;
	std::vector<std::string> not_opened = plan.not_visited;

	// 現在のシーンコレクションで先に消す (ここで消した状態は、次のシーンコレクションを開くときに OBS が保存する。
	// OBS 31.1.1 の ActivateSceneCollection は、切り替える前に SaveProjectNow を呼び出す)。
	std::vector<RemovalCount> first = remove_here();

	if (!plan.others.empty()) {
		for (const std::string &name : plan.others) {
			if (!ops.open_collection || !ops.open_collection(name)) {
				append_unique(not_opened, name);
				continue;
			}
			visits.push_back(CollectionVisit{name, remove_here()});
		}
		// 元のシーンコレクションに戻り、もう一度消して数え直す (切り替えたときのイベントで作り直されたものを
		// 拾う)。
		if (ops.open_collection && ops.open_collection(plan.original)) {
			first = remove_here();
		} else {
			// 戻れなかった。元のシーンコレクションは最初に消した結果のままにし、戻れなかったことを結果に
			// 入れる (利用者は今、別のシーンコレクションを開いている)。
			append_unique(not_opened, plan.original);
		}
	}
	visits.insert(visits.begin(), CollectionVisit{plan.original, std::move(first)});

	if (ops.save)
		ops.save();
	return summarize_collection_walk(visits, items, not_opened, duplicates, !files.has_value());
}

std::string record_dir_if_has_files(
	const std::string &dir, const std::function<bool(const std::string &, std::vector<platform::DirEntry> &)> &list)
{
	if (dir.empty() || !list)
		return std::string();
	std::vector<platform::DirEntry> entries;
	if (!list(dir, entries))
		return std::string();
	// 録画ごとのフォルダー (開始日時の名前) が 1 つでもあるときだけ表示する。利用者が選んだ保存先は、録画のほかのファイルも
	// 入るフォルダー (D:\ など) でありうるので、空でないだけでは、録画の場所と言えない。
	for (const platform::DirEntry &entry : entries)
		if (entry.is_directory && !entry.is_link && record::is_session_dir_name(entry.name))
			return dir;
	return std::string();
}

// ---- 順序と止め方 ----

UninstallReport run_uninstall(const UninstallOps &ops)
{
	UninstallReport report;

	// 段階 (0) でフラグを立て、接続の待ち受けを止める。関数がなければ何もせず、Product の行を全部結果に入れる
	// (フラグを立てずに先へ進まない)。
	if (!ops.mark_and_stop_listening) {
		for (UninstallStep step :
		     {UninstallStep::SettingsAndSecret, UninstallStep::SourcesAndScenes, UninstallStep::RestoreVencordDir}) {
			const UninstallStepResult missing = missing_step_result(step);
			report.leftovers.insert(report.leftovers.end(), missing.leftovers.begin(), missing.leftovers.end());
		}
		return report;
	}
	ops.mark_and_stop_listening();
	report.reached = UninstallStep::MarkUninstalling;

	struct StepCall {
		UninstallStep step;
		const std::function<UninstallStepResult()> *op;
	};
	const StepCall steps[] = {
		{UninstallStep::SettingsAndSecret, &ops.remove_settings_and_secret},
		{UninstallStep::SourcesAndScenes, &ops.remove_sources_and_scenes},
		{UninstallStep::RestoreVencordDir, &ops.restore_vencord_dir},
	};
	for (const StepCall &call : steps) {
		const UninstallStepResult result = *call.op ? (*call.op)() : missing_step_result(call.step);
		for (const std::string &name : result.collections_not_opened)
			append_unique(report.collections_not_opened, name);
		for (const std::string &name : result.collections_duplicate_names)
			append_unique(report.collections_duplicate_names, name);
		if (result.collection_folder_unreadable)
			report.collection_folder_unreadable = true;
		if (step_has_leftovers(result)) {
			report.leftovers = result.leftovers;
			// バックアップを残した設定のフォルダー (止まった段階の結果だけをコピーする。
			// UninstallStepResult::settings_dir)。
			report.settings_dir = result.settings_dir;
			// 止まったときの結果のダイアログで、削除しないでほしいフォルダーとして表示する場所
			// (UninstallReport::folder_to_delete のコメント)。
			report.folder_to_delete = ops.folder_to_delete ? ops.folder_to_delete() : std::string();
			return report;
		}
		report.reached = call.step;
	}

	// 段階 (4) で場所を表示する (利用者が消す行 = 表の ShowFolder)。
	report.folder_to_delete = ops.folder_to_delete ? ops.folder_to_delete() : std::string();
	report.record_dir = ops.record_dir ? ops.record_dir() : std::string();
	report.reached = UninstallStep::ShowFolder;
	report.completed = true;
	return report;
}

} // namespace cordmux::install
