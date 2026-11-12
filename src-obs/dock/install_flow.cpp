// SPDX-License-Identifier: Apache-2.0

// ドックのインストールに関わる判断の純粋関数 (install_flow.h の実装)。

#include "dock/install_flow.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "ledger/actions.h"

namespace cordmux::dock {

bool other_vencord_dialog_needed(const std::optional<std::string> &vencord_dir, const std::string &bundled_dir,
				 const std::optional<std::string> &last_written)
{
	// vencordDir のキーがないとき (Vesktop の既定の Vencord を使っているとき) に、ほかの Vencord を使っていると
	// 表示するのは事実と違う。
	if (!vencord_dir.has_value())
		return false;
	if (!bundled_dir.empty() && platform::same_dir_path(*vencord_dir, bundled_dir))
		return false;
	// cordmux が最後に書いた場所 (フォルダーを移す前の cordmux の場所) も、ほかの Vencord ではない
	// (install_flow.h の other_vencord_dialog_needed)。
	if (last_written.has_value() && platform::same_dir_path(*vencord_dir, *last_written))
		return false;
	return true;
}

std::vector<std::string> run_copy_steps(const std::vector<install::CopyStep> &steps, const CopyFileFn &copy,
					const std::string &nothing_to_copy_name)
{
	std::vector<std::string> failed;
	if (steps.empty()) {
		failed.push_back(nothing_to_copy_name);
		return failed;
	}
	for (const install::CopyStep &step : steps) {
		const bool ok = copy && copy(step.from, step.to);
		if (!ok)
			failed.push_back(step.name);
	}
	return failed;
}

bool probe_after_reinstall(bool copied, bool pressed)
{
	return copied || pressed;
}

std::optional<bool> hashes_match_of(platform::BundleState state)
{
	switch (state) {
	case platform::BundleState::Matches:
		return true;
	case platform::BundleState::Mismatch:
		return false;
	case platform::BundleState::Unknown:
		break;
	}
	return std::nullopt;
}

std::vector<std::string> mismatched_file_names(const platform::BundleComparison &comparison)
{
	const auto contains = [](const std::vector<std::string> &list, const std::string &name) {
		return std::find(list.begin(), list.end(), name) != list.end();
	};

	std::vector<std::string> out;
	for (std::size_t i = 0; i < platform::kVencordBundleFileCount; ++i) {
		const std::string name = platform::kVencordBundleFiles[i];
		if (contains(comparison.missing, name) || contains(comparison.differing, name))
			out.push_back(name);
	}
	// 照合する 4 つのファイル以外の名前が来ても捨てず、後ろに付ける。
	for (const std::vector<std::string> *list : {&comparison.missing, &comparison.differing}) {
		for (const std::string &name : *list) {
			if (!name.empty() && !contains(out, name))
				out.push_back(name);
		}
	}
	return out;
}

std::optional<ledger::VesktopRemoveFailure> remove_failure_of(install::VesktopRemoveOutcome outcome)
{
	switch (outcome) {
	case install::VesktopRemoveOutcome::Restored:
	case install::VesktopRemoveOutcome::AlreadyRestored:
	case install::VesktopRemoveOutcome::VesktopRunning:
	case install::VesktopRemoveOutcome::VesktopUnknown:
		return std::nullopt;
	case install::VesktopRemoveOutcome::StatePathUnknown:
	case install::VesktopRemoveOutcome::StateUnreadable:
		return ledger::VesktopRemoveFailure::StateUnreadable;
	case install::VesktopRemoveOutcome::NoBackup:
	case install::VesktopRemoveOutcome::BackupInvalid:
	case install::VesktopRemoveOutcome::Refused:
		return ledger::VesktopRemoveFailure::NotRestorable;
	case install::VesktopRemoveOutcome::WritesDisabled:
		return ledger::VesktopRemoveFailure::WritesDisabled;
	case install::VesktopRemoveOutcome::StateWriteFailed:
		return ledger::VesktopRemoveFailure::StateWriteFailed;
	}
	return ledger::VesktopRemoveFailure::NotRestorable;
}

install::UninstallContext uninstall_context_of(const DockPollConfig &cfg, const std::string &module_config_dir,
					       const std::string &record_dir)
{
	install::UninstallContext context;
	context.module_config_dir = module_config_dir;
	context.backup_json_path = cfg.backup_json_path;
	context.record_dir = record_dir;
	context.max_json_bytes = cfg.max_json_bytes;
	context.stop_listening = cfg.stop_listening;
	context.bundled_vencord_dir = cfg.bundled_vencord_dir;
	context.allow_state_json_write = cfg.allow_state_json_write;
	return context;
}

std::optional<ledger::UninstallLeftoverKind> leftover_kind_of(install::RemovalItem item)
{
	switch (item) {
	case install::RemovalItem::CordmuxSources:
		return ledger::UninstallLeftoverKind::Sources;
	case install::RemovalItem::ModuleConfigFiles:
		return ledger::UninstallLeftoverKind::SettingsFiles;
	case install::RemovalItem::LocalDataFolder:
		return ledger::UninstallLeftoverKind::LocalDataFolder;
	case install::RemovalItem::VencordDirValue:
		return ledger::UninstallLeftoverKind::VencordDir;
	case install::RemovalItem::ExtractedFolder:
	case install::RemovalItem::Recordings:
	case install::RemovalItem::ObsDockLayout:
	case install::RemovalItem::ObsLogs:
	case install::RemovalItem::ObsPluginManagerRecord:
		break;
	}
	return std::nullopt;
}

ledger::UninstallFacts uninstall_facts_of(ledger::UninstallPhase phase,
					  const std::optional<install::UninstallReport> &report)
{
	ledger::UninstallFacts out;
	out.phase = phase;
	if (!report.has_value())
		return out;

	out.folder_to_delete = report->folder_to_delete;
	out.record_dir = report->record_dir;
	out.settings_dir = report->settings_dir;

	for (const install::UninstallLeftover &left : report->leftovers) {
		ledger::UninstallLeftoverFact fact;
		fact.kind = leftover_kind_of(left.item).value_or(ledger::UninstallLeftoverKind::SettingsFiles);
		fact.where = left.where;
		fact.is_file = left.is_file;
		out.leftovers.push_back(std::move(fact));
	}
	for (const std::string &name : report->collections_not_opened) {
		ledger::UninstallLeftoverFact fact;
		fact.kind = ledger::UninstallLeftoverKind::CollectionNotOpened;
		fact.where = name;
		out.leftovers.push_back(std::move(fact));
	}
	for (const std::string &name : report->collections_duplicate_names) {
		ledger::UninstallLeftoverFact fact;
		fact.kind = ledger::UninstallLeftoverKind::CollectionNameDuplicate;
		fact.where = name;
		out.leftovers.push_back(std::move(fact));
	}
	if (report->collection_folder_unreadable) {
		ledger::UninstallLeftoverFact fact;
		fact.kind = ledger::UninstallLeftoverKind::CollectionFolderUnreadable;
		out.leftovers.push_back(std::move(fact));
	}
	return out;
}

ledger::UninstallPhase phase_after(const install::UninstallReport &report)
{
	return report.completed ? ledger::UninstallPhase::Done : ledger::UninstallPhase::Stopped;
}

bool uninstall_can_start(const ledger::Observation &obs)
{
	return ledger::apply(obs, ledger::ActionId::Uninstall).can_run();
}

namespace {

bool is_separator(char c)
{
	return c == '/' || c == '\\';
}

char ascii_lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// ASCII の大文字と小文字を区別せずに比べて、等しいか (同梱のファイルの名前は ASCII だけ)。
bool same_file_name(const std::string &a, const std::string &b)
{
	if (a.size() != b.size())
		return false;
	for (std::size_t i = 0; i < a.size(); ++i) {
		if (ascii_lower(a[i]) != ascii_lower(b[i]))
			return false;
	}
	return true;
}

// ASCII の大文字と小文字を区別しない、名前の順。
bool file_name_less(const std::string &a, const std::string &b)
{
	return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
					    [](char x, char y) { return ascii_lower(x) < ascii_lower(y); });
}

// 末尾の区切り文字を除いてから、最後の要素を除いた親のパスを返す。最後の要素が expected_last でなければ空を返す。
std::string parent_if_last_is(const std::string &path, const std::string &expected_last)
{
	std::size_t end = path.size();
	while (end > 0 && is_separator(path[end - 1]))
		--end;
	std::size_t cut = end;
	while (cut > 0 && !is_separator(path[cut - 1]))
		--cut;
	if (cut == 0 || !same_file_name(path.substr(cut, end - cut), expected_last))
		return std::string();
	std::size_t parent_end = cut;
	while (parent_end > 0 && is_separator(path[parent_end - 1]))
		--parent_end;
	// 親がドライブのルート ("C:") や、ルートの区切り文字だけのときは、削除するフォルダーとして表示できないので
	// 空にする。
	if (parent_end == 0 || path[parent_end - 1] == ':')
		return std::string();
	return path.substr(0, parent_end);
}

} // namespace

std::optional<LooseInstallFiles> loose_install_files_of(const std::string &module_dir,
							const std::string &module_file_name,
							const std::vector<std::string> &runtime_names,
							const std::string &bundled_vencord_dir)
{
	if (module_dir.empty())
		return std::nullopt;
	LooseInstallFiles out;
	out.files_dir = module_dir;
	const auto add = [&out](const std::string &name) {
		if (name.empty())
			return;
		for (const std::string &have : out.file_names)
			if (same_file_name(have, name))
				return;
		out.file_names.push_back(name);
	};
	add(module_file_name);
	for (const std::string &name : runtime_names)
		add(name);
	if (out.file_names.empty())
		return std::nullopt;
	std::stable_sort(out.file_names.begin(), out.file_names.end(), file_name_less);
	out.data_dir = parent_if_last_is(bundled_vencord_dir, "vencord");
	return out;
}

} // namespace cordmux::dock
