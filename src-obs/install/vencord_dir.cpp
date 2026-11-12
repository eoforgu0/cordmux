// SPDX-License-Identifier: Apache-2.0

// install/vencord_dir.h の実装。

#include "install/vencord_dir.h"

// 場所の比べ方は、Vesktop の設定を調べる側と同じ関数を使う (vencord_dir.h の plan_write のコメント)。
// 純粋関数だけのヘッダーで、Windows の名前も libobs も持たない。
#include "platform/bundle_check.h"

// nlohmann/json は外部のライブラリなので、そのヘッダーが出す警告は抑える (config/settings.h と
// ws/control_messages.h と同じ)。
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <nlohmann/json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <utility>

namespace cordmux::install {

namespace detail {

// state.json の元の字下げの幅を推定する (元に近い形で書き戻すため)。
// 改行が 1 つもないか、最初の改行より後に中身がなければ 1 行の形とみなし、-1 (nlohmann::json::dump() の
// 字下げなし) を返す。2 行目があれば、その先頭の半角空白の数を 1 段の幅とみなす。
// タブで字下げしているなど、空白の数で読めない形は、既定の 2 にする。
inline int detect_indent(const std::string &text)
{
	const auto newline_pos = text.find('\n');
	if (newline_pos == std::string::npos)
		return -1; // 1 行 (dump() の既定のまま)

	// 最初の改行より後に中身がなければ、これも 1 行の形である (`{"a":1}\n` のように、末尾の改行だけを持つ
	// state.json)。ここを見ないと、下の空白の数が 0 になって既定の 2 になり、1 行だった state.json を
	// 字下げして書き戻してしまう。
	std::size_t rest = newline_pos + 1;
	while (rest < text.size() && (text[rest] == '\n' || text[rest] == '\r'))
		++rest;
	if (rest >= text.size())
		return -1;

	std::size_t i = newline_pos + 1;
	int spaces = 0;
	while (i < text.size() && text[i] == ' ') {
		++spaces;
		++i;
	}
	if (spaces == 0)
		return 2; // 改行はあるが、空白の字下げが読めない (タブなど)
	return spaces;
}

// 入力の末尾の改行 (なし、"\n"、"\r\n") をそのまま返す。書き戻す全文の末尾に付け直すためだけのもので、
// JSON の意味には関わらない。
inline std::string trailing_newline(const std::string &text)
{
	if (text.size() >= 2 && text[text.size() - 2] == '\r' && text[text.size() - 1] == '\n')
		return "\r\n";
	if (!text.empty() && text.back() == '\n')
		return "\n";
	return {};
}

// 書き換えた JSON を、元の整形に近い形で文字列にする (plan_write と plan_restore が同じ形で書き戻すため)。
inline std::string render_like(const nlohmann::ordered_json &j, const std::string &original_text)
{
	std::string text = j.dump(detect_indent(original_text));
	text += trailing_newline(original_text);
	return text;
}

// state.json を読んだ結果。ok が false なら reason が理由で、ほかのフィールドは使わない。
struct ParsedState {
	bool ok = false;
	RefuseReason reason = RefuseReason::BrokenJson;
	nlohmann::ordered_json json;
	std::optional<std::string> vencord_dir; // キーがなければ nullopt
};

inline ParsedState parse_state(const std::string &state_json_text)
{
	ParsedState out;
	out.json = nlohmann::ordered_json::parse(state_json_text, nullptr, /*allow_exceptions=*/false);
	if (out.json.is_discarded()) {
		out.reason = RefuseReason::BrokenJson;
		return out;
	}
	if (!out.json.is_object()) {
		out.reason = RefuseReason::NotObject;
		return out;
	}
	const auto it = out.json.find("vencordDir");
	if (it != out.json.end()) {
		if (!it->is_string()) {
			out.reason = RefuseReason::VencordDirNotString;
			return out;
		}
		out.vencord_dir = it->get<std::string>();
	}
	out.ok = true;
	return out;
}

// キーの有無まで含めて同じ値か (vencord_dir.h の plan_write のコメントの比べ方)。どちらもキーがなければ同じ、
// 片方だけなければ違う、どちらもあれば same_dir_path で比べる。
inline bool same_optional_dir(const std::optional<std::string> &a, const std::optional<std::string> &b)
{
	if (!a.has_value() || !b.has_value())
		return a.has_value() == b.has_value();
	return platform::same_dir_path(*a, *b);
}

// vencord_dir.h の plan_write のコメントの 5 つの分岐。
enum class Branch {
	AlreadyTarget,   // 分岐 1: 現在の値が target_dir と同じ場所
	NoBackup,        // 分岐 2: バックアップがない
	LastWritten,     // 分岐 3: 現在の値がバックアップの written と同じ場所
	BackToPrevious,  // 分岐 4: 現在の値がバックアップの previous と同じ
	Foreign,         // 分岐 5: それ以外
};

inline Branch classify(const std::optional<std::string> &existing, const std::string &target_dir,
		       const std::optional<Backup> &current)
{
	if (existing.has_value() && platform::same_dir_path(*existing, target_dir))
		return Branch::AlreadyTarget;
	if (!current.has_value())
		return Branch::NoBackup;
	if (existing.has_value() && platform::same_dir_path(*existing, current->written))
		return Branch::LastWritten;
	if (same_optional_dir(existing, current->previous))
		return Branch::BackToPrevious;
	return Branch::Foreign;
}

} // namespace detail

std::string backup_to_json(const Backup &backup)
{
	nlohmann::ordered_json j;
	if (backup.previous.has_value())
		j["previous"] = *backup.previous;
	else
		j["previous"] = nullptr; // キーがなかったことを JSON の null で表す
	j["written"] = backup.written;
	// キーは written_at (単位はエポックからのマイクロ秒)。platform/bundle_check.h の
	// parse_vencord_dir_backup() が同じキーで読む。
	j["written_at"] = backup.written_at_us;
	j["dll_dir"] = backup.dll_dir;
	// 古い版のバックアップにはないので、空ならキーごと書かない (読むほうは、キーがなければ空と読む)。
	if (!backup.state_path.empty())
		j["state_path"] = backup.state_path;
	// 分からないときはキーごと書かない (読むほうは、キーがないことを分からないと読む)。
	switch (backup.plugin_entry.state) {
	case PluginEntryState::Absent:
		j["plugin_entry"] = nullptr;
		break;
	case PluginEntryState::Present: {
		nlohmann::ordered_json value =
			nlohmann::ordered_json::parse(backup.plugin_entry.value_json, nullptr, /*allow_exceptions=*/false);
		if (!value.is_discarded() && value.is_object())
			j["plugin_entry"] = std::move(value);
		break;
	}
	case PluginEntryState::Unknown:
		break;
	}
	return j.dump();
}

bool backup_from_json(std::string_view json_text, Backup *out)
{
	// 項の中の順番を保つため、ordered_json で読む。
	const nlohmann::ordered_json j = nlohmann::ordered_json::parse(json_text, nullptr, /*allow_exceptions=*/false);
	if (j.is_discarded() || !j.is_object())
		return false;

	const auto previous_it = j.find("previous");
	if (previous_it == j.end())
		return false;
	std::optional<std::string> previous;
	if (previous_it->is_string()) {
		previous = previous_it->get<std::string>();
	} else if (!previous_it->is_null()) {
		return false; // null でも文字列でもない値は形が違う
	}

	const auto written_it = j.find("written");
	if (written_it == j.end() || !written_it->is_string())
		return false;

	const auto written_at_it = j.find("written_at");
	if (written_at_it == j.end() || !written_at_it->is_number_integer())
		return false;

	const auto dll_dir_it = j.find("dll_dir");
	if (dll_dir_it == j.end() || !dll_dir_it->is_string())
		return false;

	// plugin_entry は、キーがない、型が違うときも失敗にせず、分からないとする。
	PluginEntryBackup plugin_entry;
	const auto entry_it = j.find("plugin_entry");
	if (entry_it != j.end()) {
		if (entry_it->is_null()) {
			plugin_entry.state = PluginEntryState::Absent;
		} else if (entry_it->is_object()) {
			plugin_entry.state = PluginEntryState::Present;
			plugin_entry.value_json = entry_it->dump();
		}
	}

	// state_path は、キーがない、型が違うときも失敗にせず、空 (現在の場所を使う) とする。
	std::string state_path;
	const auto state_path_it = j.find("state_path");
	if (state_path_it != j.end() && state_path_it->is_string())
		state_path = state_path_it->get<std::string>();

	if (out) {
		out->state_path = std::move(state_path);
		out->plugin_entry = std::move(plugin_entry);
		out->previous = std::move(previous);
		out->written = written_it->get<std::string>();
		out->written_at_us = written_at_it->get<std::int64_t>();
		out->dll_dir = dll_dir_it->get<std::string>();
	}
	return true;
}

WritePlan plan_write(const std::string &state_json_text, const std::string &target_dir,
		     const std::optional<Backup> &current, std::int64_t now_us, const std::string &dll_dir,
		     WriteScene scene, const PluginEntryBackup &plugin_entry_now)
{
	WritePlan plan;

	detail::ParsedState state = detail::parse_state(state_json_text);
	if (!state.ok) {
		plan.action = WriteAction::Refuse;
		plan.refuse_reason = state.reason;
		return plan;
	}

	const auto refuse = [&plan](RefuseReason reason) {
		plan.action = WriteAction::Refuse;
		plan.refuse_reason = reason;
		return plan;
	};

	// 書くときの次の previous (vencord_dir.h の plan_write のコメントの、分岐ごとの規則)。
	std::optional<std::string> next_previous;
	PluginEntryBackup next_plugin_entry; // previous と同じ規則 (previous を現在の値にするときだけ現在の項にする)
	switch (detail::classify(state.vencord_dir, target_dir, current)) {
	case detail::Branch::AlreadyTarget:
		// 1. もう target_dir と同じ場所なので、どの場面でも何もしない。
		plan.action = WriteAction::AlreadyOurs;
		plan.new_backup = current;
		return plan;
	case detail::Branch::NoBackup:
		// 2. 一度も書いていないので、現在の値がそのまま元の値になる。
		//    起動時は書かない (利用者が押していないのに書かない)。
		if (scene == WriteScene::Startup)
			return refuse(RefuseReason::SceneNotAllowed);
		next_previous = state.vencord_dir;
		next_plugin_entry = plugin_entry_now;
		break;
	case detail::Branch::LastWritten:
		// 3. cordmux が最後に書いた値のまま (cordmux のフォルダーを移した後の書き直しもここ)。
		//    previous は引き継ぐ (ここで書き換えると、アンインストールで元の値に戻せなくなる)。
		//    起動時が書くのはこの分岐だけ (written == target_dir なら分岐 1 になる)。
		next_previous = current->previous;
		next_plugin_entry = current->plugin_entry;
		break;
	case detail::Branch::BackToPrevious:
		// 4. 元の値に戻っている (手で戻したときなど)。previous は変えない。
		//    起動時は書かない (アンインストールや手で戻した値を書き戻さない)。
		if (scene == WriteScene::Startup)
			return refuse(RefuseReason::SceneNotAllowed);
		next_previous = current->previous;
		next_plugin_entry = current->plugin_entry;
		break;
	case detail::Branch::Foreign:
		// 5. それ以外 (利用者かほかのツールが書き換えた) なので、書き直さない。
		//    ほかの Vencord からの切り替えを利用者が選んだとき (FixRequested) と、現在の値にキーがないときの
		//    Pressed だけ書き、previous は現在の値にする。
		if (scene == WriteScene::Startup || (scene == WriteScene::Pressed && state.vencord_dir.has_value()))
			return refuse(RefuseReason::ForeignChange);
		next_previous = state.vencord_dir;
		next_plugin_entry = plugin_entry_now;
		break;
	}

	state.json["vencordDir"] = target_dir;

	plan.action = WriteAction::Write;
	plan.new_text = detail::render_like(state.json, state_json_text);

	Backup next_backup;
	next_backup.previous = std::move(next_previous);
	next_backup.written = target_dir;
	next_backup.written_at_us = now_us;
	next_backup.dll_dir = dll_dir;
	next_backup.plugin_entry = std::move(next_plugin_entry);
	plan.new_backup = std::move(next_backup);

	return plan;
}

Backup advance_written_at(const Backup &current, std::int64_t now_us)
{
	Backup next = current;
	next.written_at_us = now_us;
	return next;
}

RestorePlan plan_restore(const std::string &state_json_text, const Backup &backup)
{
	RestorePlan plan;

	detail::ParsedState state = detail::parse_state(state_json_text);
	if (!state.ok) {
		plan.action = RestoreAction::Refuse;
		plan.refuse_reason = state.reason;
		return plan;
	}

	// もう元の値になっている (キーがなかったなら、もうキーがない)。
	if (detail::same_optional_dir(state.vencord_dir, backup.previous)) {
		plan.action = RestoreAction::AlreadyRestored;
		return plan;
	}

	// cordmux が書いた値が残っているときだけ戻す (理由は vencord_dir.h の plan_restore のコメントにある)。
	if (!state.vencord_dir.has_value() || !platform::same_dir_path(*state.vencord_dir, backup.written)) {
		plan.action = RestoreAction::Refuse;
		plan.refuse_reason = RefuseReason::ForeignChange;
		return plan;
	}

	if (backup.previous.has_value())
		state.json["vencordDir"] = *backup.previous;
	else
		state.json.erase("vencordDir"); // キーがなかったなら、キーごと消す

	plan.action = RestoreAction::Write;
	plan.new_text = detail::render_like(state.json, state_json_text);
	return plan;
}

std::optional<std::string> plan_remove_plugin_entry(const std::string &settings_json_text,
						    const std::string &plugin_name)
{
	nlohmann::ordered_json json = nlohmann::ordered_json::parse(settings_json_text, nullptr, /*allow_exceptions=*/false);
	if (json.is_discarded() || !json.is_object())
		return std::nullopt;
	const auto plugins_it = json.find("plugins");
	if (plugins_it == json.end() || !plugins_it->is_object())
		return std::nullopt;
	if (plugins_it->erase(plugin_name) == 0)
		return std::nullopt;
	return detail::render_like(json, settings_json_text);
}

PluginEntryBackup snapshot_plugin_entry(const std::string &settings_json_text, const std::string &plugin_name)
{
	PluginEntryBackup out;
	const nlohmann::ordered_json json =
		nlohmann::ordered_json::parse(settings_json_text, nullptr, /*allow_exceptions=*/false);
	if (json.is_discarded() || !json.is_object())
		return out;
	const auto plugins_it = json.find("plugins");
	if (plugins_it == json.end()) {
		out.state = PluginEntryState::Absent;
		return out;
	}
	if (!plugins_it->is_object())
		return out;
	const auto entry_it = plugins_it->find(plugin_name);
	if (entry_it == plugins_it->end()) {
		out.state = PluginEntryState::Absent;
		return out;
	}
	if (!entry_it->is_object())
		return out;
	out.state = PluginEntryState::Present;
	out.value_json = entry_it->dump();
	return out;
}

std::optional<std::string> plan_restore_plugin_entry(const std::string &settings_json_text,
						     const std::string &plugin_name, const PluginEntryBackup &entry)
{
	switch (entry.state) {
	case PluginEntryState::Unknown:
		return std::nullopt;
	case PluginEntryState::Absent:
		return plan_remove_plugin_entry(settings_json_text, plugin_name);
	case PluginEntryState::Present:
		break;
	}

	const nlohmann::ordered_json value =
		nlohmann::ordered_json::parse(entry.value_json, nullptr, /*allow_exceptions=*/false);
	if (value.is_discarded() || !value.is_object())
		return std::nullopt;
	nlohmann::ordered_json json = nlohmann::ordered_json::parse(settings_json_text, nullptr, /*allow_exceptions=*/false);
	if (json.is_discarded() || !json.is_object())
		return std::nullopt;
	if (json.find("plugins") == json.end())
		json["plugins"] = nlohmann::ordered_json::object();
	nlohmann::ordered_json &plugins = json["plugins"];
	if (!plugins.is_object())
		return std::nullopt;
	const auto it = plugins.find(plugin_name);
	if (it != plugins.end() && *it == value)
		return std::nullopt; // もうバックアップの値になっている
	plugins[plugin_name] = value;
	return detail::render_like(json, settings_json_text);
}

} // namespace cordmux::install
