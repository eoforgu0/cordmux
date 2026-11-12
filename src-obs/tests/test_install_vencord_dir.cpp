// SPDX-License-Identifier: Apache-2.0

// install/vencord_dir.h の単体テスト。
// state.json の例は、Vesktop の state.json と同じキー (firstLaunch、windowBounds (入れ子のオブジェクト)、
// maximized、minimized、vencordDir) を持つ。

#include "doctest.h"

#include "install/vencord_dir.h"

// バックアップを書く側と読む側をつなぐテストだけが、この 2 つを使う (書いたバックアップを読む側にそのまま渡し、
// ledger::restart_needed() まで通ることを確認する。ファイルの末尾の節)。比べ方が Vesktop の設定を調べる側と同じ
// 関数であることも、bundle_check.h の same_dir_path を直接呼び出して確認する。
#include "ledger/observation.h"
#include "platform/bundle_check.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <nlohmann/json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using cordmux::install::advance_written_at;
using cordmux::install::Backup;
using cordmux::install::backup_from_json;
using cordmux::install::backup_to_json;
using cordmux::install::plan_restore;
using cordmux::install::plan_restore_plugin_entry;
using cordmux::install::plan_write;
using cordmux::install::PluginEntryBackup;
using cordmux::install::PluginEntryState;
using cordmux::install::RefuseReason;
using cordmux::install::RestoreAction;
using cordmux::install::RestorePlan;
using cordmux::install::snapshot_plugin_entry;
using cordmux::install::WriteAction;
using cordmux::install::WritePlan;
using cordmux::install::WriteScene;

namespace {

// Vesktop の state.json と同じキーの形 (windowBounds は入れ子のオブジェクト)。パスの区切りはバックスラッシュ
// (JSON の文字列では \\ で 1 文字)。
constexpr const char *kStateCompact =
	R"({"firstLaunch":false,"windowBounds":{"x":100,"y":50,"width":1280,"height":800},)"
	R"("maximized":false,"minimized":false,"vencordDir":"C:\\Users\\alice\\Vencord\\dist"})";

constexpr const char *kStatePretty =
	"{\n"
	"  \"firstLaunch\": false,\n"
	"  \"windowBounds\": {\n"
	"    \"x\": 100,\n"
	"    \"y\": 50,\n"
	"    \"width\": 1280,\n"
	"    \"height\": 800\n"
	"  },\n"
	"  \"maximized\": false,\n"
	"  \"minimized\": false,\n"
	"  \"vencordDir\": \"C:\\\\Users\\\\alice\\\\Vencord\\\\dist\"\n"
	"}\n";

// vencordDir のキーがない state.json (利用者が一度も Vencord Location を設定していない)。
constexpr const char *kStateNoKey =
	R"({"firstLaunch":true,"windowBounds":{"x":0,"y":0,"width":800,"height":600},)"
	R"("maximized":false,"minimized":false})";

const std::string kTargetDir = R"(C:\ProgramData\obs-studio\plugins\cordmux\data\vencord)";
const std::string kDllDir = R"(C:\ProgramData\obs-studio\plugins\cordmux)";
constexpr std::int64_t kNowUs = 946'684'800'000'000; // 2000-01-01 00:00:00 (UTC)

constexpr WriteScene kAllScenes[] = {WriteScene::Pressed, WriteScene::FixRequested, WriteScene::Startup};

// Windows のパス (バックスラッシュを含む) を、手で書く JSON の文字列に入れるためのエスケープ
// (JSON は 1 つのバックスラッシュを \\ の 2 文字で表す)。
std::string json_escape_backslashes(const std::string &s)
{
	std::string out;
	out.reserve(s.size());
	for (char c : s) {
		if (c == '\\')
			out += '\\';
		out += c;
	}
	return out;
}

// ほかのキーを持つ state.json に、vencordDir を 1 つ入れた全文 (値が nullopt ならキーごとない)。
std::string state_with(const std::optional<std::string> &vencord_dir)
{
	std::string out = R"({"firstLaunch":false,"maximized":false)";
	if (vencord_dir.has_value())
		out += R"(,"vencordDir":")" + json_escape_backslashes(*vencord_dir) + "\"";
	out += "}";
	return out;
}

std::optional<std::string> vencord_dir_of(const std::string &json_text)
{
	const nlohmann::json j = nlohmann::json::parse(json_text);
	const auto it = j.find("vencordDir");
	if (it == j.end())
		return std::nullopt;
	return it->get<std::string>();
}

// "vencordDir" を除いた (キー, 値の文字列) の並び。元の文字列と書き戻した文字列でこれを比べ、ほかのキーの値と
// 順番が変わっていないことを 1 回で確認する。
std::vector<std::pair<std::string, std::string>> keys_excluding_vencord_dir(const std::string &json_text)
{
	const nlohmann::ordered_json j = nlohmann::ordered_json::parse(json_text);
	std::vector<std::pair<std::string, std::string>> out;
	for (auto it = j.begin(); it != j.end(); ++it) {
		if (it.key() == "vencordDir")
			continue;
		out.emplace_back(it.key(), it.value().dump());
	}
	return out;
}

// 一時フォルダーにテスト用の state.json を作り、RAII で消す。plan_write はファイルを読み書きしないが、ここでは
// 実際にディスクに書いたファイルを読み直して渡す。リポジトリの外 (std::filesystem::temp_directory_path) に置く。
class TempStateFile {
public:
	explicit TempStateFile(const std::string &contents)
	{
		static std::atomic<int> counter{0};
		dir_ = std::filesystem::temp_directory_path() /
		       ("cordmux_install_test_" + std::to_string(++counter) + "_" +
			std::to_string(reinterpret_cast<std::uintptr_t>(this)));
		std::filesystem::create_directories(dir_);
		path_ = dir_ / "state.json";
		std::ofstream out(path_, std::ios::binary);
		out << contents;
	}

	~TempStateFile()
	{
		std::error_code ec;
		std::filesystem::remove_all(dir_, ec);
	}

	TempStateFile(const TempStateFile &) = delete;
	TempStateFile &operator=(const TempStateFile &) = delete;

	std::string read() const
	{
		std::ifstream in(path_, std::ios::binary);
		std::ostringstream buffer;
		buffer << in.rdbuf();
		return buffer.str();
	}

	void write(const std::string &contents)
	{
		std::ofstream out(path_, std::ios::binary | std::ios::trunc);
		out << contents;
	}

	const std::filesystem::path &path() const { return path_; }

private:
	std::filesystem::path dir_;
	std::filesystem::path path_;
};

// ---- 場面のテストに使う値 (install/vencord_dir.h の plan_write のコメントの [E] [T] [C]) ----
//
// 利用者の元の値 kPrev、cordmux が前に書いた古い場所 kOldWritten (移す前の展開したフォルダー)、
// 現在の書きたい場所 kTargetDir、利用者かほかのツールが後で選んだ別の場所 kForeign。
const std::string kPrev = R"(C:\Users\alice\Vencord\dist)";
const std::string kOldWritten = R"(D:\old-place\cordmux\data\vencord)";
const std::string kOldDllDir = R"(D:\old-place\cordmux)";
const std::string kForeign = R"(C:\someone-else\vencord\dist)";
constexpr std::int64_t kWrittenAtUs = kNowUs - 5'000'000;

Backup backup_of(std::optional<std::string> previous, std::string written)
{
	Backup b;
	b.previous = std::move(previous);
	b.written = std::move(written);
	b.written_at_us = kWrittenAtUs;
	b.dll_dir = kOldDllDir;
	return b;
}

// 同じ場所を別の書き方にする (区切りを / に、英字を小文字にし、末尾に区切りを足す)。
// same_dir_path が同じ場所とみなす書き方の例 (platform/bundle_check.h の normalize_dir_path)。
std::string respell(const std::string &path)
{
	std::string out;
	for (char c : path) {
		if (c == '\\')
			out += '/';
		else if (c >= 'A' && c <= 'Z')
			out += static_cast<char>(c - 'A' + 'a');
		else
			out += c;
	}
	out += '/';
	return out;
}

// 書いた結果を確認する共通の関数。state.json の vencordDir は target で、ほかのキーは元のまま、バックアップの
// written、written_at、dll_dir は進み、previous は expected_previous になる。
void check_written(const WritePlan &plan, const std::string &state_before,
		   const std::optional<std::string> &expected_previous)
{
	REQUIRE(plan.action == WriteAction::Write);
	REQUIRE(plan.new_backup.has_value());
	CHECK(vencord_dir_of(plan.new_text) == std::optional<std::string>(kTargetDir));
	CHECK(keys_excluding_vencord_dir(plan.new_text) == keys_excluding_vencord_dir(state_before));
	CHECK(plan.new_backup->previous == expected_previous);
	CHECK(plan.new_backup->written == kTargetDir);
	CHECK(plan.new_backup->written_at_us == kNowUs);
	CHECK(plan.new_backup->dll_dir == kDllDir);
}

void check_refused(const WritePlan &plan, RefuseReason reason)
{
	CHECK(plan.action == WriteAction::Refuse);
	CHECK(plan.refuse_reason == reason);
	CHECK(plan.new_text.empty());
	CHECK_FALSE(plan.new_backup.has_value());
}

} // namespace

// ---- 書く (バックアップがない最初の 1 回) ----

TEST_CASE("バックアップがなく、キーがあれば、前の値を previous に保存する")
{
	WritePlan plan = plan_write(kStateCompact, kTargetDir, std::nullopt, kNowUs, kDllDir, WriteScene::Pressed);
	REQUIRE(plan.action == WriteAction::Write);
	REQUIRE(plan.new_backup.has_value());
	CHECK(plan.new_backup->previous == R"(C:\Users\alice\Vencord\dist)");
	CHECK(plan.new_backup->written == kTargetDir);
	CHECK(plan.new_backup->written_at_us == kNowUs);
	CHECK(plan.new_backup->dll_dir == kDllDir);

	const nlohmann::json rewritten = nlohmann::json::parse(plan.new_text);
	CHECK(rewritten.at("vencordDir").get<std::string>() == kTargetDir);
}

TEST_CASE("バックアップがなく、キーもなければ、previous を nullopt にする")
{
	WritePlan plan = plan_write(kStateNoKey, kTargetDir, std::nullopt, kNowUs, kDllDir, WriteScene::Pressed);
	REQUIRE(plan.action == WriteAction::Write);
	REQUIRE(plan.new_backup.has_value());
	CHECK_FALSE(plan.new_backup->previous.has_value());

	const nlohmann::json rewritten = nlohmann::json::parse(plan.new_text);
	CHECK(rewritten.at("vencordDir").get<std::string>() == kTargetDir);
}

// ---- もう cordmux の場所を指している ----

TEST_CASE("state.json がもう target_dir と一致していれば何もしない")
{
	const std::string state_json = R"({"other":1,"vencordDir":")" + json_escape_backslashes(kTargetDir) + R"("})";

	WritePlan plan = plan_write(state_json, kTargetDir, std::nullopt, kNowUs, kDllDir, WriteScene::Pressed);
	CHECK(plan.action == WriteAction::AlreadyOurs);
	CHECK_FALSE(plan.new_backup.has_value()); // current がなかったので、バックアップもないまま
}

TEST_CASE("もう一致していて current があれば、バックアップをそのまま返す")
{
	Backup current;
	current.previous = R"(C:\old\vencord)";
	current.written = kTargetDir;
	current.written_at_us = kNowUs - 1000;
	current.dll_dir = kDllDir;

	const std::string state_json = R"({"vencordDir":")" + json_escape_backslashes(kTargetDir) + R"("})";

	WritePlan plan = plan_write(state_json, kTargetDir, current, kNowUs, kDllDir, WriteScene::Pressed);
	CHECK(plan.action == WriteAction::AlreadyOurs);
	REQUIRE(plan.new_backup.has_value());
	CHECK(plan.new_backup->previous == current.previous);
	CHECK(plan.new_backup->written == current.written);
	CHECK(plan.new_backup->written_at_us == current.written_at_us); // 更新していない (時刻もそのまま)
	CHECK(plan.new_backup->dll_dir == current.dll_dir);
}

// ---- 壊れた JSON は拒む ----

TEST_CASE("壊れた JSON (構文エラー) は拒む")
{
	WritePlan plan = plan_write("{not json", kTargetDir, std::nullopt, kNowUs, kDllDir, WriteScene::Pressed);
	CHECK(plan.action == WriteAction::Refuse);
	CHECK(plan.refuse_reason == RefuseReason::BrokenJson);
	CHECK(plan.new_text.empty());
	CHECK_FALSE(plan.new_backup.has_value());
}

TEST_CASE("空文字列も壊れた JSON として拒む")
{
	WritePlan plan = plan_write("", kTargetDir, std::nullopt, kNowUs, kDllDir, WriteScene::Pressed);
	CHECK(plan.action == WriteAction::Refuse);
	CHECK(plan.refuse_reason == RefuseReason::BrokenJson);
}

TEST_CASE("オブジェクトでない JSON (配列) は拒む")
{
	WritePlan plan = plan_write("[1, 2, 3]", kTargetDir, std::nullopt, kNowUs, kDllDir, WriteScene::Pressed);
	CHECK(plan.action == WriteAction::Refuse);
	CHECK(plan.refuse_reason == RefuseReason::NotObject);
}

TEST_CASE("オブジェクトでない JSON (数値だけ) も拒む")
{
	WritePlan plan = plan_write("42", kTargetDir, std::nullopt, kNowUs, kDllDir, WriteScene::Pressed);
	CHECK(plan.action == WriteAction::Refuse);
	CHECK(plan.refuse_reason == RefuseReason::NotObject);
}

TEST_CASE("vencordDir の値が文字列でなければ拒む (数値、null、オブジェクト)")
{
	{
		WritePlan plan =
			plan_write(R"({"vencordDir":1})", kTargetDir, std::nullopt, kNowUs, kDllDir, WriteScene::Pressed);
		CHECK(plan.action == WriteAction::Refuse);
		CHECK(plan.refuse_reason == RefuseReason::VencordDirNotString);
	}
	{
		WritePlan plan =
			plan_write(R"({"vencordDir":null})", kTargetDir, std::nullopt, kNowUs, kDllDir, WriteScene::Pressed);
		CHECK(plan.action == WriteAction::Refuse);
		CHECK(plan.refuse_reason == RefuseReason::VencordDirNotString);
	}
	{
		WritePlan plan = plan_write(R"({"vencordDir":{"a":1}})", kTargetDir, std::nullopt, kNowUs, kDllDir,
					    WriteScene::Pressed);
		CHECK(plan.action == WriteAction::Refuse);
		CHECK(plan.refuse_reason == RefuseReason::VencordDirNotString);
	}
}

// ---- ほかのキーを変えない ----

TEST_CASE("ほかのキーは、値も順番も変えない")
{
	const auto before = keys_excluding_vencord_dir(kStateCompact);
	WritePlan plan = plan_write(kStateCompact, kTargetDir, std::nullopt, kNowUs, kDllDir, WriteScene::Pressed);
	REQUIRE(plan.action == WriteAction::Write);
	const auto after = keys_excluding_vencord_dir(plan.new_text);
	CHECK(before == after);
}

TEST_CASE("キーがなかったときは、ほかのキーはそのままで vencordDir だけが増える")
{
	const auto before = keys_excluding_vencord_dir(kStateNoKey);
	WritePlan plan = plan_write(kStateNoKey, kTargetDir, std::nullopt, kNowUs, kDllDir, WriteScene::Pressed);
	REQUIRE(plan.action == WriteAction::Write);
	const auto after = keys_excluding_vencord_dir(plan.new_text);
	CHECK(before == after);

	const nlohmann::ordered_json rewritten = nlohmann::ordered_json::parse(plan.new_text);
	REQUIRE(rewritten.contains("vencordDir"));
	CHECK(rewritten.at("vencordDir").get<std::string>() == kTargetDir);
}

// ---- 整形は元に近い形にする ----

TEST_CASE("整形: 1 行の JSON は 1 行のまま書き戻す")
{
	WritePlan plan = plan_write(kStateCompact, kTargetDir, std::nullopt, kNowUs, kDllDir, WriteScene::Pressed);
	REQUIRE(plan.action == WriteAction::Write);
	CHECK(plan.new_text.find('\n') == std::string::npos);
}

TEST_CASE("整形: 2 文字の字下げの JSON は、字下げと末尾の改行を保って書き戻す")
{
	WritePlan plan = plan_write(kStatePretty, kTargetDir, std::nullopt, kNowUs, kDllDir, WriteScene::Pressed);
	REQUIRE(plan.action == WriteAction::Write);
	CHECK(plan.new_text.find("\n  \"firstLaunch\"") != std::string::npos);
	CHECK(plan.new_text.find("\n    \"x\"") != std::string::npos); // 入れ子は 2 段ぶん
	REQUIRE_FALSE(plan.new_text.empty());
	CHECK(plan.new_text.back() == '\n'); // 元の末尾の改行を保つ

	// 書き戻した後も JSON として妥当で、値は正しい。
	const nlohmann::json rewritten = nlohmann::json::parse(plan.new_text);
	CHECK(rewritten.at("vencordDir").get<std::string>() == kTargetDir);
	CHECK(rewritten.at("windowBounds").at("width").get<int>() == 1280);
}

TEST_CASE("整形: 末尾に改行のない入力は改行を足さない")
{
	// kStateCompact は定義上、末尾に改行を持たない。
	WritePlan plan = plan_write(kStateCompact, kTargetDir, std::nullopt, kNowUs, kDllDir, WriteScene::Pressed);
	REQUIRE(plan.action == WriteAction::Write);
	CHECK_FALSE(plan.new_text.empty());
	CHECK(plan.new_text.back() != '\n');
}

TEST_CASE("整形: 末尾の改行だけを持つ 1 行の JSON は、1 行のまま書き戻す (改行の種類も保つ)")
{
	// 1 行と末尾の改行は、2 行目がある形ではない。ここを 2 文字の字下げと読むと、cordmux のものではない
	// Vesktop の設定ファイルを、字下げした形で書き戻すことになる。
	for (const std::string tail : {std::string("\n"), std::string("\r\n")}) {
		CAPTURE(tail);
		const std::string input = std::string(kStateCompact) + tail;
		WritePlan plan = plan_write(input, kTargetDir, std::nullopt, kNowUs, kDllDir, WriteScene::Pressed);
		REQUIRE(plan.action == WriteAction::Write);
		REQUIRE(plan.new_text.size() > tail.size());
		// 本文には改行が 1 つも増えていない (末尾のぶんだけが残る)。
		const std::string body = plan.new_text.substr(0, plan.new_text.size() - tail.size());
		CHECK(body.find('\n') == std::string::npos);
		CHECK(plan.new_text.substr(plan.new_text.size() - tail.size()) == tail);

		const nlohmann::json rewritten = nlohmann::json::parse(plan.new_text);
		CHECK(rewritten.at("vencordDir").get<std::string>() == kTargetDir);
		CHECK(rewritten.at("windowBounds").at("width").get<int>() == 1280);
	}
}

// ---- バックアップとの比較 (フォルダーを移した後の書き直しと、元の値に戻した後の書き込み) ----

TEST_CASE("cordmux が最後に書いた値のままなら、DLL を移した後でも書き直し、previous を引き継ぐ")
{
	const std::string original_value = R"(C:\Users\alice\Vencord\dist)";
	const std::string old_target = R"(C:\old-place\cordmux\data\vencord)";
	const std::string new_target = R"(C:\new-place\cordmux\data\vencord)"; // DLL を移した後の値

	Backup current;
	current.previous = original_value;
	current.written = old_target;
	current.written_at_us = kNowUs - 5000;
	current.dll_dir = R"(C:\old-place\cordmux)";

	const std::string state_json =
		R"({"vencordDir":")" + json_escape_backslashes(old_target) + R"("})"; // まだ古い場所のまま

	WritePlan plan = plan_write(state_json, new_target, current, kNowUs, R"(C:\new-place\cordmux)",
				    WriteScene::Pressed);
	REQUIRE(plan.action == WriteAction::Write);
	REQUIRE(plan.new_backup.has_value());
	CHECK(plan.new_backup->previous == original_value); // current.previous を引き継ぐ (current.written ではない)
	CHECK(plan.new_backup->written == new_target);
	CHECK(plan.new_backup->written_at_us == kNowUs);
}

TEST_CASE("元の値に戻した後にもう一度押すと、previous を変えずに書く")
{
	const std::string original_value = R"(C:\Users\alice\Vencord\dist)";
	const std::string old_target = R"(C:\ProgramData\obs-studio\plugins\cordmux\data\vencord)";

	Backup current;
	current.previous = original_value;
	current.written = old_target;
	current.written_at_us = kNowUs - 9000;
	current.dll_dir = kDllDir;

	// vencordDir が元の値に戻った後の state.json (current->written ではなく current->previous と一致する)。
	const std::string state_json = R"({"vencordDir":")" + json_escape_backslashes(original_value) + R"("})";

	WritePlan plan = plan_write(state_json, old_target, current, kNowUs, kDllDir, WriteScene::Pressed);
	REQUIRE(plan.action == WriteAction::Write);
	REQUIRE(plan.new_backup.has_value());
	CHECK(plan.new_backup->previous == original_value); // 変わらない
	CHECK(plan.new_backup->written == old_target);
	CHECK(plan.new_backup->written_at_us == kNowUs);
}

TEST_CASE("[Vesktopから削除] で戻した後に [Vesktopに追加] を押しても、バックアップは cordmux の場所で上書きされない")
{
	// [Vesktopから削除] (install/uninstall.h の remove_from_vesktop) は、plan_restore で state.json を previous に
	// 戻し、バックアップは消さない。その後の [Vesktopに追加] は分岐 4 (元の値に戻っている) になり、previous と
	// plugin_entry を引き継ぐ。分岐 2 (バックアップがない) や分岐 5 のように現在の値を previous にすると、後の
	// アンインストールが元の値ではなく、ここで書いた値へ戻す。previous がある場合と、もともとキーがなかった場合の両方。
	PluginEntryBackup entry;
	entry.state = PluginEntryState::Present;
	entry.value_json = R"({"enabled":true,"token":"user-own"})";
	for (const std::optional<std::string> &previous : {std::optional<std::string>(kPrev), std::optional<std::string>()}) {
		CAPTURE(previous.value_or("(no key)"));
		Backup installed = backup_of(previous, kTargetDir);
		installed.plugin_entry = entry;

		// 追加した後の state.json を、削除で戻す。
		const RestorePlan restore = plan_restore(state_with(kTargetDir), installed);
		REQUIRE(restore.action == RestoreAction::Write);
		CHECK(vencord_dir_of(restore.new_text) == previous);

		// もう一度追加する。押したときの場面 (Pressed。キーがなかったとき) でも、ほかの Vencord から切り替える確認で
		// [切り替え] を選んだ場面 (FixRequested。previous がほかの Vencord のとき) でも、Vencord の設定の現在の項が
		// cordmux の項 (追加した後の値) でも、バックアップは前のまま引き継ぐ。
		PluginEntryBackup now;
		now.state = PluginEntryState::Present;
		now.value_json = R"({"enabled":true})";
		for (const WriteScene scene : {WriteScene::Pressed, WriteScene::FixRequested}) {
			CAPTURE(static_cast<int>(scene));
			const WritePlan again = plan_write(restore.new_text, kTargetDir, installed, kNowUs, kDllDir, scene, now);
			REQUIRE(again.action == WriteAction::Write);
			REQUIRE(again.new_backup.has_value());
			CHECK(again.new_backup->previous == previous);
			CHECK(again.new_backup->written == kTargetDir);
			CHECK(again.new_backup->plugin_entry.state == PluginEntryState::Present);
			CHECK(again.new_backup->plugin_entry.value_json == entry.value_json);
			CHECK(vencord_dir_of(again.new_text) == std::optional<std::string>(kTargetDir));

			// 後のアンインストールは、cordmux を追加する前の値へ戻す (cordmux の場所へ戻さない)。
			const RestorePlan later = plan_restore(again.new_text, *again.new_backup);
			REQUIRE(later.action == RestoreAction::Write);
			CHECK(vencord_dir_of(later.new_text) == previous);
		}
		// 起動時の書き直しは、元の値に戻った state.json を書き直さない (分岐 4。[Vesktopから削除] を取り消さない)。
		const WritePlan startup =
			plan_write(restore.new_text, kTargetDir, installed, kNowUs, kDllDir, WriteScene::Startup, now);
		CHECK(startup.action == WriteAction::Refuse);
		CHECK(startup.refuse_reason == RefuseReason::SceneNotAllowed);
	}
}

TEST_CASE("元の値に戻した後に書くとき、もともと vencordDir がなかったなら、previous は nullopt のまま")
{
	Backup current;
	current.previous = std::nullopt; // もともとキーがなかった
	current.written = kTargetDir;
	current.written_at_us = kNowUs - 9000;
	current.dll_dir = kDllDir;

	// previous がなかったので、元に戻すときはキーごと消す。
	WritePlan plan = plan_write(kStateNoKey, kTargetDir, current, kNowUs, kDllDir, WriteScene::Pressed);
	REQUIRE(plan.action == WriteAction::Write);
	REQUIRE(plan.new_backup.has_value());
	CHECK_FALSE(plan.new_backup->previous.has_value());
}

TEST_CASE("それ以外の値に変わっていれば ForeignChange で拒む")
{
	Backup current;
	current.previous = R"(C:\original\vencord)";
	current.written = R"(C:\ProgramData\obs-studio\plugins\cordmux\data\vencord)";
	current.written_at_us = kNowUs - 5000;
	current.dll_dir = kDllDir;

	// current.written でも current.previous でもない、別の値。
	const std::string state_json = R"({"vencordDir":"C:\\someone-else\\vencord"})";

	WritePlan plan = plan_write(state_json, current.written, current, kNowUs, kDllDir, WriteScene::Pressed);
	CHECK(plan.action == WriteAction::Refuse);
	CHECK(plan.refuse_reason == RefuseReason::ForeignChange);
	CHECK(plan.new_text.empty());
	CHECK_FALSE(plan.new_backup.has_value());
}

// ---- 3 つの場面と 5 つの分岐 (install/vencord_dir.h の plan_write のコメントの表) ----

TEST_CASE("分岐 1: もう書きたい場所を指していれば、どの場面でも AlreadyOurs で、バックアップを変えない")
{
	const Backup current = backup_of(kPrev, kOldWritten);
	for (const WriteScene scene : kAllScenes) {
		CAPTURE(static_cast<int>(scene));
		for (const std::optional<Backup> &c : {std::optional<Backup>(current), std::optional<Backup>()}) {
			const WritePlan plan = plan_write(state_with(kTargetDir), kTargetDir, c, kNowUs, kDllDir, scene);
			CHECK(plan.action == WriteAction::AlreadyOurs);
			CHECK(plan.new_text.empty());
			CHECK(plan.new_backup.has_value() == c.has_value());
			if (plan.new_backup.has_value())
				CHECK(plan.new_backup->written_at_us == kWrittenAtUs); // 時刻も進めない
		}
	}
}

TEST_CASE("分岐 2 (バックアップがない): Pressed と FixRequested では書き、Startup では書かない")
{
	for (const std::optional<std::string> &existing : {std::optional<std::string>(kPrev), std::optional<std::string>()}) {
		CAPTURE(existing.has_value()); // false ならキーがない
		const std::string state = state_with(existing);

		// 同じ入力で、Pressed と FixRequested では書く (previous = 現在の値)。
		check_written(plan_write(state, kTargetDir, std::nullopt, kNowUs, kDllDir, WriteScene::Pressed), state,
			      existing);
		check_written(plan_write(state, kTargetDir, std::nullopt, kNowUs, kDllDir, WriteScene::FixRequested),
			      state, existing);

		// Startup では書かない (利用者が押していないのに書かない)。
		check_refused(plan_write(state, kTargetDir, std::nullopt, kNowUs, kDllDir, WriteScene::Startup),
			      RefuseReason::SceneNotAllowed);
	}
}

TEST_CASE("分岐 3 (cordmux が最後に書いた値のまま): 3 つの場面のどれでも書き、previous を引き継ぐ")
{
	// 展開したフォルダーを移した後。state.json はまだ古い場所 (cordmux が書いた値) を指している。
	for (const std::optional<std::string> &previous :
	     {std::optional<std::string>(kPrev), std::optional<std::string>()}) {
		CAPTURE(previous.has_value()); // false ならキーがなかった
		const Backup current = backup_of(previous, kOldWritten);
		const std::string state = state_with(kOldWritten);
		for (const WriteScene scene : kAllScenes) {
			CAPTURE(static_cast<int>(scene));
			// FixRequested でも、previous を cordmux の古い場所にしない (install/vencord_dir.h の
			// plan_write のコメント)。
			check_written(plan_write(state, kTargetDir, current, kNowUs, kDllDir, scene), state, previous);
		}
	}
}

TEST_CASE("分岐 4 (元の値に戻っている): Pressed と FixRequested では書き、Startup では書かない")
{
	// アンインストールの後や、手で戻した後。Startup で書くと、利用者が戻した値を、利用者が操作していないのに
	// 書き戻すことになる。
	for (const std::optional<std::string> &previous :
	     {std::optional<std::string>(kPrev), std::optional<std::string>()}) {
		CAPTURE(previous.has_value()); // false ならキーがなかった
		const Backup current = backup_of(previous, kOldWritten);
		const std::string state = state_with(previous); // 現在の値 == バックアップの previous

		check_written(plan_write(state, kTargetDir, current, kNowUs, kDllDir, WriteScene::Pressed), state,
			      previous);
		check_written(plan_write(state, kTargetDir, current, kNowUs, kDllDir, WriteScene::FixRequested), state,
			      previous);
		check_refused(plan_write(state, kTargetDir, current, kNowUs, kDllDir, WriteScene::Startup),
			      RefuseReason::SceneNotAllowed);
	}
}

TEST_CASE("分岐 5 (利用者かほかのツールの値): FixRequested のときだけ書き、previous は現在の値になる")
{
	const Backup current = backup_of(kPrev, kOldWritten);
	const std::string state = state_with(kForeign);

	check_refused(plan_write(state, kTargetDir, current, kNowUs, kDllDir, WriteScene::Pressed),
		      RefuseReason::ForeignChange);
	check_refused(plan_write(state, kTargetDir, current, kNowUs, kDllDir, WriteScene::Startup),
		      RefuseReason::ForeignChange);
	check_written(plan_write(state, kTargetDir, current, kNowUs, kDllDir, WriteScene::FixRequested), state,
		      kForeign);
}

TEST_CASE("分岐 5: インストールの後にキーが消されていたら、Pressed と FixRequested で書き、previous は nullopt になる")
{
	// バックアップの previous は利用者の値だったが、今はキーがない (Vesktop の [Reset Vesktop] で設定ごと消えた
	// ときなど)。キーがないことは previous (場所) と一致しないので、分岐 5 になる。Pressed で拒むと、ドックが
	// 勧める [Vesktopに追加] が動作しない。
	const Backup current = backup_of(kPrev, kOldWritten);
	const std::string state = state_with(std::nullopt);

	check_written(plan_write(state, kTargetDir, current, kNowUs, kDllDir, WriteScene::Pressed), state,
		      std::nullopt);
	check_refused(plan_write(state, kTargetDir, current, kNowUs, kDllDir, WriteScene::Startup),
		      RefuseReason::ForeignChange);
	check_written(plan_write(state, kTargetDir, current, kNowUs, kDllDir, WriteScene::FixRequested), state,
		      std::nullopt);
}

TEST_CASE("5 つの分岐を総当たりすると、Startup で Write になるのは分岐 3 だけである")
{
	struct Case {
		const char *label;
		std::optional<std::string> existing;
		std::optional<Backup> current;
		bool expect_write;
	};
	// label は分岐の番号 (install/vencord_dir.h の plan_write のコメントの 1-5) と、現在の値のキーがないかどうか。
	const std::vector<Case> cases = {
		{"branch 1", kTargetDir, backup_of(kPrev, kOldWritten), false},
		{"branch 2", kPrev, std::nullopt, false},
		{"branch 2, no key", std::nullopt, std::nullopt, false},
		{"branch 3", kOldWritten, backup_of(kPrev, kOldWritten), true},
		{"branch 4", kPrev, backup_of(kPrev, kOldWritten), false},
		{"branch 4, no key", std::nullopt, backup_of(std::nullopt, kOldWritten), false},
		{"branch 5", kForeign, backup_of(kPrev, kOldWritten), false},
		{"branch 5, no key", std::nullopt, backup_of(kPrev, kOldWritten), false},
	};
	for (const Case &c : cases) {
		CAPTURE(c.label);
		const WritePlan plan =
			plan_write(state_with(c.existing), kTargetDir, c.current, kNowUs, kDllDir, WriteScene::Startup);
		CHECK((plan.action == WriteAction::Write) == c.expect_write);
	}
}

TEST_CASE("フォルダーを移した後の起動時の書き直しは、previous とほかのキーを保ち、次の起動では何もしない")
{
	// フォルダーを移した後の 2 回の起動を再現する。
	const Backup current = backup_of(kPrev, kOldWritten);
	const std::string before = std::string(R"({"firstLaunch":false,"windowBounds":{"x":1,"y":2},)") +
				   R"("vencordDir":")" + json_escape_backslashes(kOldWritten) + R"(","minimized":false})";

	const WritePlan first = plan_write(before, kTargetDir, current, kNowUs, kDllDir, WriteScene::Startup);
	check_written(first, before, kPrev);

	// 書いた state.json とバックアップで次に起動したら、もう書かない (分岐 1)。
	const WritePlan second =
		plan_write(first.new_text, kTargetDir, first.new_backup, kNowUs + 1'000'000, kDllDir, WriteScene::Startup);
	CHECK(second.action == WriteAction::AlreadyOurs);
	REQUIRE(second.new_backup.has_value());
	CHECK(second.new_backup->written_at_us == kNowUs);
}

TEST_CASE("読めない state.json はどの場面でもその理由で拒む")
{
	const Backup current = backup_of(kPrev, kOldWritten);
	for (const WriteScene scene : kAllScenes) {
		CAPTURE(static_cast<int>(scene));
		check_refused(plan_write("{not json", kTargetDir, current, kNowUs, kDllDir, scene), RefuseReason::BrokenJson);
		check_refused(plan_write("[]", kTargetDir, current, kNowUs, kDllDir, scene), RefuseReason::NotObject);
		check_refused(plan_write(R"({"vencordDir":3})", kTargetDir, current, kNowUs, kDllDir, scene),
			      RefuseReason::VencordDirNotString);
	}
}

// ---- 比べ方 (Vesktop の設定を調べる側と同じ same_dir_path) ----

TEST_CASE("比べ方: 同じ場所の別の書き方 (区切り、大文字と小文字、末尾の区切り) は、同じ場所として扱う")
{
	// 前提として、ここで使う書き方は、Vesktop の設定を調べる関数も同じ場所とみなす (画面と書き込みが同じ
	// 答えを出す)。
	REQUIRE(cordmux::platform::same_dir_path(respell(kTargetDir), kTargetDir));
	REQUIRE(cordmux::platform::same_dir_path(respell(kOldWritten), kOldWritten));
	REQUIRE(cordmux::platform::same_dir_path(respell(kPrev), kPrev));
	REQUIRE_FALSE(cordmux::platform::same_dir_path(kForeign, kPrev));

	const Backup current = backup_of(kPrev, kOldWritten);

	SUBCASE("分岐 1: 現在の値が書きたい場所の別の書き方なら、どの場面でも書かない")
	{
		for (const WriteScene scene : kAllScenes) {
			CAPTURE(static_cast<int>(scene));
			CHECK(plan_write(state_with(respell(kTargetDir)), kTargetDir, current, kNowUs, kDllDir, scene).action ==
			      WriteAction::AlreadyOurs);
		}
	}
	SUBCASE("分岐 3: 現在の値がバックアップの written の別の書き方なら、拒まずに書く")
	{
		const std::string state = state_with(respell(kOldWritten));
		check_written(plan_write(state, kTargetDir, current, kNowUs, kDllDir, WriteScene::Pressed), state, kPrev);
		check_written(plan_write(state, kTargetDir, current, kNowUs, kDllDir, WriteScene::Startup), state, kPrev);
	}
	SUBCASE("分岐 4: 現在の値が previous の別の書き方なら元の値として扱い、previous の文字列は変えない")
	{
		const std::string state = state_with(respell(kPrev));
		check_written(plan_write(state, kTargetDir, current, kNowUs, kDllDir, WriteScene::Pressed), state, kPrev);
		check_refused(plan_write(state, kTargetDir, current, kNowUs, kDllDir, WriteScene::Startup),
			      RefuseReason::SceneNotAllowed);
	}
	SUBCASE("Startup: バックアップの written が書きたい場所の別の書き方なら、書き直さない (分岐 1)")
	{
		// written == target_dir (同じ場所) のときは書かない。
		const Backup same_place = backup_of(kPrev, respell(kTargetDir));
		const WritePlan plan = plan_write(state_with(respell(kTargetDir)), kTargetDir, same_place, kNowUs, kDllDir,
						  WriteScene::Startup);
		CHECK(plan.action == WriteAction::AlreadyOurs);
	}
	SUBCASE("キーがないことは、空の値とも一致しない")
	{
		// キーがないので分岐 5 になり、Pressed でも書いて previous をキーがない状態にする。空の値と一致した
		// (分岐 4) なら、previous は空の文字列のまま残る。
		const Backup empty_previous = backup_of(std::string(), kOldWritten);
		const std::string no_key = state_with(std::nullopt);
		check_written(plan_write(no_key, kTargetDir, empty_previous, kNowUs, kDllDir, WriteScene::Pressed), no_key,
			      std::nullopt);
		const Backup no_previous = backup_of(std::nullopt, kOldWritten);
		check_refused(plan_write(state_with(std::string()), kTargetDir, no_previous, kNowUs, kDllDir,
					 WriteScene::Pressed),
			      RefuseReason::ForeignChange);
	}
}

// ---- written_at だけを進める ----

TEST_CASE("advance_written_at は written_at だけを進め、ほかのフィールドは変えない")
{
	for (const std::optional<std::string> &previous :
	     {std::optional<std::string>(kPrev), std::optional<std::string>(), std::optional<std::string>("")}) {
		Backup current = backup_of(previous, kTargetDir);
		const Backup next = advance_written_at(current, kNowUs);
		CHECK(next.written_at_us == kNowUs);
		CHECK(next.previous == current.previous);
		CHECK(next.written == current.written);
		CHECK(next.dll_dir == current.dll_dir);
		CHECK(current.written_at_us == kWrittenAtUs); // 渡したバックアップは変えない

		// JSON にしても written_at のほかは同じ文字列になる (バックアップのファイルの差分が、1 つのフィールド
		// だけになる)。
		Backup expected = current;
		expected.written_at_us = kNowUs;
		CHECK(backup_to_json(next) == backup_to_json(expected));
	}
}

TEST_CASE("advance_written_at で進めたバックアップでは、restart_needed() が true になる")
{
	// 前に書いたのは Vesktop の起動より前なので、restart_needed() は false。再インストールした後は true になる。
	const std::int64_t vesktop_started = kWrittenAtUs + 1'000'000;
	const Backup old_backup = backup_of(kPrev, kTargetDir);

	const auto restart_needed_with = [&](const Backup &b) {
		const auto read_back = cordmux::platform::parse_vencord_dir_backup(backup_to_json(b));
		REQUIRE(read_back.has_value());
		cordmux::ledger::Observation obs;
		obs.install.discord_side_written_at_us = read_back->written_at_us;
		obs.install.vesktop_started_at_us = vesktop_started;
		return cordmux::ledger::restart_needed(obs);
	};
	CHECK_FALSE(restart_needed_with(old_backup));
	CHECK(restart_needed_with(advance_written_at(old_backup, kNowUs)));
}

TEST_CASE("advance_written_at: 時計が戻っていても now_us をそのまま入れる")
{
	const Backup current = backup_of(kPrev, kTargetDir);
	CHECK(advance_written_at(current, kWrittenAtUs - 1).written_at_us == kWrittenAtUs - 1);
}

// ---- vencordDir を元に戻す ----

TEST_CASE("plan_restore: cordmux が書いた値のままなら previous の文字列に戻し、ほかのキーは変えない")
{
	const Backup backup = backup_of(kPrev, kTargetDir);
	const std::string state = state_with(kTargetDir);
	const RestorePlan plan = plan_restore(state, backup);
	REQUIRE(plan.action == RestoreAction::Write);
	CHECK(vencord_dir_of(plan.new_text) == std::optional<std::string>(kPrev));
	CHECK(keys_excluding_vencord_dir(plan.new_text) == keys_excluding_vencord_dir(state));
}

TEST_CASE("plan_restore: previous が nullopt ならキーごと消す (test/tools/obs-run/cleanup.ps1 と同じ)")
{
	const Backup backup = backup_of(std::nullopt, kTargetDir);
	const std::string state = state_with(kTargetDir);
	const RestorePlan plan = plan_restore(state, backup);
	REQUIRE(plan.action == RestoreAction::Write);
	const nlohmann::ordered_json j = nlohmann::ordered_json::parse(plan.new_text);
	CHECK_FALSE(j.contains("vencordDir"));
	CHECK(keys_excluding_vencord_dir(plan.new_text) == keys_excluding_vencord_dir(state));
}

TEST_CASE("plan_restore: 書いてから元に戻すと、元の state.json と 1 文字も違わない (1 行、字下げ、キーなし)")
{
	for (const std::string original : {std::string(kStateCompact), std::string(kStatePretty),
					    std::string(kStateNoKey), std::string(kStateCompact) + "\r\n"}) {
		CAPTURE(original);
		const WritePlan written =
			plan_write(original, kTargetDir, std::nullopt, kNowUs, kDllDir, WriteScene::Pressed);
		REQUIRE(written.action == WriteAction::Write);
		REQUIRE(written.new_backup.has_value());

		const RestorePlan restored = plan_restore(written.new_text, *written.new_backup);
		REQUIRE(restored.action == RestoreAction::Write);
		CHECK(restored.new_text == original);

		// 元に戻した後にもう一度戻そうとしても、何もしない (やり直しても同じ結果になる)。
		CHECK(plan_restore(restored.new_text, *written.new_backup).action == RestoreAction::AlreadyRestored);
	}
}

TEST_CASE("plan_restore: もう元の値なら何もしない (どちらもキーがないときと、同じ場所の別の書き方)")
{
	CHECK(plan_restore(state_with(kPrev), backup_of(kPrev, kTargetDir)).action == RestoreAction::AlreadyRestored);
	CHECK(plan_restore(state_with(respell(kPrev)), backup_of(kPrev, kTargetDir)).action ==
	      RestoreAction::AlreadyRestored);
	CHECK(plan_restore(state_with(std::nullopt), backup_of(std::nullopt, kTargetDir)).action ==
	      RestoreAction::AlreadyRestored);
}

TEST_CASE("plan_restore: cordmux が書いた値の別の書き方でも元に戻す (比べ方は same_dir_path)")
{
	const RestorePlan plan = plan_restore(state_with(respell(kTargetDir)), backup_of(kPrev, kTargetDir));
	REQUIRE(plan.action == RestoreAction::Write);
	CHECK(vencord_dir_of(plan.new_text) == std::optional<std::string>(kPrev)); // バックアップの文字列のまま戻す
}

TEST_CASE("plan_restore: インストールの後に利用者かほかのツールが変えた値は戻さない")
{
	// 別の場所へ変えた。
	{
		const RestorePlan plan = plan_restore(state_with(kForeign), backup_of(kPrev, kTargetDir));
		CHECK(plan.action == RestoreAction::Refuse);
		CHECK(plan.refuse_reason == RefuseReason::ForeignChange);
		CHECK(plan.new_text.empty());
	}
	// キーを消した (previous は場所だった)。
	{
		const RestorePlan plan = plan_restore(state_with(std::nullopt), backup_of(kPrev, kTargetDir));
		CHECK(plan.action == RestoreAction::Refuse);
		CHECK(plan.refuse_reason == RefuseReason::ForeignChange);
	}
	// previous は nullopt で、今は別の場所。
	{
		const RestorePlan plan = plan_restore(state_with(kForeign), backup_of(std::nullopt, kTargetDir));
		CHECK(plan.action == RestoreAction::Refuse);
		CHECK(plan.refuse_reason == RefuseReason::ForeignChange);
	}
}

TEST_CASE("plan_restore: 読めない state.json はその理由で拒む")
{
	const Backup backup = backup_of(kPrev, kTargetDir);
	const std::vector<std::pair<std::string, RefuseReason>> cases = {
		{"{not json", RefuseReason::BrokenJson},
		{"", RefuseReason::BrokenJson},
		{"[1]", RefuseReason::NotObject},
		{R"({"vencordDir":false})", RefuseReason::VencordDirNotString},
	};
	for (const auto &c : cases) {
		CAPTURE(c.first);
		const RestorePlan plan = plan_restore(c.first, backup);
		CHECK(plan.action == RestoreAction::Refuse);
		CHECK(plan.refuse_reason == c.second);
		CHECK(plan.new_text.empty());
	}
}

TEST_CASE("plan_restore: FixRequested で書いた後に元に戻すと、切り替える前の Vencord の場所に戻る")
{
	// 利用者は別の Vencord (kForeign) を使っていた。[Vesktopに追加]、[切り替え]、アンインストールの順に操作すると、
	// kForeign に戻る。
	const Backup first = backup_of(kPrev, kOldWritten);
	const std::string state = state_with(kForeign);
	const WritePlan fixed = plan_write(state, kTargetDir, first, kNowUs, kDllDir, WriteScene::FixRequested);
	REQUIRE(fixed.action == WriteAction::Write);
	REQUIRE(fixed.new_backup.has_value());

	const RestorePlan restored = plan_restore(fixed.new_text, *fixed.new_backup);
	REQUIRE(restored.action == RestoreAction::Write);
	CHECK(vencord_dir_of(restored.new_text) == std::optional<std::string>(kForeign));
	CHECK(restored.new_text == state);
}

// ---- バックアップの JSON への変換と読み込み ----

TEST_CASE("バックアップを JSON にして読むと元に戻る (previous があるとき)")
{
	Backup original;
	original.previous = R"(C:\Users\alice\Vencord\dist)";
	original.written = kTargetDir;
	original.written_at_us = kNowUs;
	original.dll_dir = kDllDir;

	const std::string text = backup_to_json(original);
	Backup roundtrip;
	REQUIRE(backup_from_json(text, &roundtrip));
	CHECK(roundtrip.previous == original.previous);
	CHECK(roundtrip.written == original.written);
	CHECK(roundtrip.written_at_us == original.written_at_us);
	CHECK(roundtrip.dll_dir == original.dll_dir);
}

TEST_CASE("バックアップを JSON にして読むと元に戻る (previous が nullopt のとき。null と空の文字列を区別する)")
{
	Backup original;
	original.previous = std::nullopt;
	original.written = kTargetDir;
	original.written_at_us = kNowUs;
	original.dll_dir = kDllDir;

	const std::string text = backup_to_json(original);
	CHECK(text.find("null") != std::string::npos); // nullopt は JSON の null で表す

	Backup roundtrip;
	REQUIRE(backup_from_json(text, &roundtrip));
	CHECK_FALSE(roundtrip.previous.has_value());
}

TEST_CASE("バックアップを JSON にして読むと元に戻る (previous が空の文字列のとき。null とは別)")
{
	Backup original;
	original.previous = std::string{}; // キーはあったが値が空の文字列だった、という別の状態
	original.written = kTargetDir;
	original.written_at_us = kNowUs;
	original.dll_dir = kDllDir;

	const std::string text = backup_to_json(original);
	Backup roundtrip;
	REQUIRE(backup_from_json(text, &roundtrip));
	REQUIRE(roundtrip.previous.has_value());
	CHECK(roundtrip.previous->empty());
}

TEST_CASE("壊れたバックアップの JSON は false を返し、out を変えない")
{
	Backup sentinel;
	sentinel.previous = "sentinel";
	sentinel.written = "sentinel";
	sentinel.written_at_us = 999;
	sentinel.dll_dir = "sentinel";

	const std::vector<std::string> bad_inputs = {
		"{not json",
		"[1, 2, 3]",
		"42",
		R"({"written":"w","written_at":1,"dll_dir":"d"})",              // previous がない
		R"({"previous":null,"written_at":1,"dll_dir":"d"})",           // written がない
		R"({"previous":null,"written":"w","dll_dir":"d"})",               // written_at がない
		R"({"previous":null,"written":"w","written_at":1})",           // dll_dir がない
		R"({"previous":1,"written":"w","written_at":1,"dll_dir":"d"})", // previous が数値
		R"({"previous":null,"written":1,"written_at":1,"dll_dir":"d"})", // written が文字列でない
		R"({"previous":null,"written":"w","written_at":"1","dll_dir":"d"})", // written_at が文字列
		R"({"previous":null,"written":"w","written_at":1.5,"dll_dir":"d"})", // written_at が小数
		R"({"previous":null,"written":"w","written_at":1,"dll_dir":2})", // dll_dir が数値
	};
	for (const auto &bad : bad_inputs) {
		Backup out = sentinel;
		CAPTURE(bad);
		CHECK_FALSE(backup_from_json(bad, &out));
		CHECK(out.previous == sentinel.previous); // 書き換えられていない
		CHECK(out.written == sentinel.written);
		CHECK(out.written_at_us == sentinel.written_at_us);
		CHECK(out.dll_dir == sentinel.dll_dir);
	}
}

TEST_CASE("backup_from_json は out が nullptr でもクラッシュしない")
{
	CHECK(backup_from_json(R"({"previous":null,"written":"w","written_at":1,"dll_dir":"d"})", nullptr));
	CHECK_FALSE(backup_from_json("{not json", nullptr));
}

// ---- 書く側 (install/) と読む側 (platform/) のつなぎ目 ----
//
// バックアップを書くのは install/ で、それを読んで Observation にするのは platform::parse_vencord_dir_backup() で
// ある。どちらも同じキーで書き、読むことは、片方だけを見るテストでは確認できない (書く側のキーの名前を変えても、
// 書く側の往復のテストも、読む側の手で書いた JSON のテストも成功のままになる)。ここで 2 つを直接つないで、
// ledger::restart_needed() が読む値が実際に渡ることを確認する。
TEST_CASE("install/ が書いたバックアップの JSON を、platform/ がそのまま読める")
{
	Backup original;
	original.previous = R"(C:\Users\alice\Vencord\dist)";
	original.written = kTargetDir;
	original.written_at_us = kNowUs;
	original.dll_dir = kDllDir;

	const std::string text = backup_to_json(original);
	const auto read_back = cordmux::platform::parse_vencord_dir_backup(text);
	REQUIRE(read_back.has_value());
	REQUIRE(read_back->written.has_value());
	CHECK(*read_back->written == original.written);
	REQUIRE(read_back->written_at_us.has_value()); // ledger::restart_needed() が読む値
	CHECK(*read_back->written_at_us == original.written_at_us);
}

TEST_CASE("Vesktop の起動より後に書いたバックアップでは、restart_needed() が true になる")
{
	// [Vesktopに追加] を押した直後のバックアップをそのまま Observation に入れると、restart_needed() の条件
	// (書いた時刻 > Vesktop の起動時刻) が成り立つ。
	Backup fresh;
	fresh.previous = std::nullopt;
	fresh.written = kTargetDir;
	fresh.written_at_us = kNowUs;
	fresh.dll_dir = kDllDir;

	const auto read_back = cordmux::platform::parse_vencord_dir_backup(backup_to_json(fresh));
	REQUIRE(read_back.has_value());
	REQUIRE(read_back->written_at_us.has_value());

	cordmux::ledger::Observation obs;
	obs.install.discord_side_written_at_us = read_back->written_at_us;
	obs.install.vesktop_started_at_us = kNowUs - 1000; // ボタンを押す前から起動していた
	CHECK(cordmux::ledger::restart_needed(obs));
}

// ---- 一時フォルダーのテスト用の state.json で確認する ----

TEST_CASE("一時フォルダーの state.json に書き、書き戻した内容を読み直せる")
{
	TempStateFile fake_state(kStateCompact);

	const std::string before = fake_state.read();
	CHECK(before == kStateCompact);

	WritePlan plan = plan_write(before, kTargetDir, std::nullopt, kNowUs, kDllDir, WriteScene::Pressed);
	REQUIRE(plan.action == WriteAction::Write);

	fake_state.write(plan.new_text);
	const std::string after = fake_state.read();

	const nlohmann::json rewritten = nlohmann::json::parse(after);
	CHECK(rewritten.at("vencordDir").get<std::string>() == kTargetDir);
	CHECK(rewritten.at("firstLaunch").get<bool>() == false);
	CHECK(rewritten.at("windowBounds").at("width").get<int>() == 1280);

	// 2 回目 (もう一度押したとき) は、もう cordmux の値なので書かない。
	WritePlan second = plan_write(after, kTargetDir, plan.new_backup, kNowUs + 1000, kDllDir, WriteScene::Pressed);
	CHECK(second.action == WriteAction::AlreadyOurs);
}

TEST_CASE("一時フォルダーの、キーのない state.json に書く")
{
	TempStateFile fake_state(kStateNoKey);
	const std::string before = fake_state.read();

	WritePlan plan = plan_write(before, kTargetDir, std::nullopt, kNowUs, kDllDir, WriteScene::Pressed);
	REQUIRE(plan.action == WriteAction::Write);
	CHECK_FALSE(plan.new_backup->previous.has_value());

	fake_state.write(plan.new_text);
	const nlohmann::json rewritten = nlohmann::json::parse(fake_state.read());
	CHECK(rewritten.at("vencordDir").get<std::string>() == kTargetDir);
}

TEST_CASE("追加、フォルダーを移した後の起動、アンインストールの順に state.json を書くと、元のバイト列に戻る")
{
	// インストール、フォルダーの移動、アンインストールを、1 つのファイルで通して確認する。
	TempStateFile fake_state(kStatePretty);
	const std::string original = fake_state.read();

	const std::string old_target = kOldWritten;
	const WritePlan pressed = plan_write(original, old_target, std::nullopt, kNowUs, kOldDllDir, WriteScene::Pressed);
	REQUIRE(pressed.action == WriteAction::Write);
	fake_state.write(pressed.new_text);

	const WritePlan startup = plan_write(fake_state.read(), kTargetDir, pressed.new_backup, kNowUs + 10, kDllDir,
					     WriteScene::Startup);
	REQUIRE(startup.action == WriteAction::Write);
	REQUIRE(startup.new_backup.has_value());
	CHECK(startup.new_backup->previous == kPrev); // 利用者の元の値のまま
	fake_state.write(startup.new_text);

	const RestorePlan restored = plan_restore(fake_state.read(), *startup.new_backup);
	REQUIRE(restored.action == RestoreAction::Write);
	fake_state.write(restored.new_text);
	CHECK(fake_state.read() == original);
}

TEST_CASE("plan_remove_plugin_entry は plugins の cordmux の項だけを消し、ほかのキーの値と順番と整形を変えない")
{
	const std::string text =
		"{\n"
		"    \"autoUpdate\": true,\n"
		"    \"plugins\": {\n"
		"        \"Alpha\": {\n"
		"            \"enabled\": true,\n"
		"            \"size\": 1.5\n"
		"        },\n"
		"        \"cordmux\": {\n"
		"            \"enabled\": true\n"
		"        },\n"
		"        \"Zeta\": {\n"
		"            \"enabled\": false\n"
		"        }\n"
		"    },\n"
		"    \"theme\": \"日本語\"\n"
		"}\n";
	const std::optional<std::string> next = cordmux::install::plan_remove_plugin_entry(text, "cordmux");
	REQUIRE(next.has_value());
	const std::string expected =
		"{\n"
		"    \"autoUpdate\": true,\n"
		"    \"plugins\": {\n"
		"        \"Alpha\": {\n"
		"            \"enabled\": true,\n"
		"            \"size\": 1.5\n"
		"        },\n"
		"        \"Zeta\": {\n"
		"            \"enabled\": false\n"
		"        }\n"
		"    },\n"
		"    \"theme\": \"日本語\"\n"
		"}\n";
	CHECK(*next == expected);
}

TEST_CASE("plan_remove_plugin_entry は、項がない、plugins がない、壊れた JSON、オブジェクトでない JSON では何も返さない")
{
	using cordmux::install::plan_remove_plugin_entry;
	CHECK_FALSE(plan_remove_plugin_entry("{\"plugins\":{\"Alpha\":{}}}", "cordmux").has_value());
	CHECK_FALSE(plan_remove_plugin_entry("{\"theme\":\"x\"}", "cordmux").has_value());
	CHECK_FALSE(plan_remove_plugin_entry("{\"plugins\":[1,2]}", "cordmux").has_value());
	CHECK_FALSE(plan_remove_plugin_entry("{\"plugins\":{\"cordmux\":", "cordmux").has_value());
	CHECK_FALSE(plan_remove_plugin_entry("", "cordmux").has_value());
	CHECK_FALSE(plan_remove_plugin_entry("[]", "cordmux").has_value());
}

// ---- Vencord の設定の cordmux の項のバックアップ (Backup::plugin_entry) ----

namespace {

// 利用者が前から使っている別のバージョンの cordmux の項 (接続のトークンなどを持つ)。
const std::string kOtherEntry = R"({"enabled":true,"port":8123,"instanceId":"abc","token":"secret"})";
// cordmux を入れた後に Vencord が補った項。
const std::string kAddedEntry = R"({"enabled":true})";

PluginEntryBackup present_entry(const std::string &value_json)
{
	PluginEntryBackup e;
	e.state = PluginEntryState::Present;
	e.value_json = value_json;
	return e;
}

PluginEntryBackup absent_entry()
{
	PluginEntryBackup e;
	e.state = PluginEntryState::Absent;
	return e;
}

std::string settings_with(const std::string &cordmux_entry)
{
	return "{\n"
	       "  \"autoUpdate\": true,\n"
	       "  \"plugins\": {\n"
	       "    \"Alpha\": {\"enabled\": true},\n"
	       "    \"cordmux\": " + cordmux_entry + ",\n"
	       "    \"Zeta\": {\"enabled\": false}\n"
	       "  },\n"
	       "  \"theme\": \"x\"\n"
	       "}\n";
}

Backup backup_with_entry(const PluginEntryBackup &entry)
{
	Backup b = backup_of(kPrev, kOldWritten);
	b.plugin_entry = entry;
	return b;
}

} // namespace

TEST_CASE("snapshot_plugin_entry は、項がオブジェクトなら値を、plugins か項がなければ「ない」を、形が違えば「分からない」を返す")
{
	const PluginEntryBackup present = snapshot_plugin_entry(settings_with(kOtherEntry), "cordmux");
	CHECK(present.state == PluginEntryState::Present);
	CHECK(present.value_json == kOtherEntry); // 順番もそのまま

	CHECK(snapshot_plugin_entry(R"({"plugins":{"Alpha":{}}})", "cordmux").state == PluginEntryState::Absent);
	CHECK(snapshot_plugin_entry(R"({"theme":"x"})", "cordmux").state == PluginEntryState::Absent);

	for (const char *text : {"", "{not json", "[]", R"({"plugins":[1]})", R"({"plugins":{"cordmux":null}})",
				 R"({"plugins":{"cordmux":"on"}})"}) {
		CAPTURE(text);
		CHECK(snapshot_plugin_entry(text, "cordmux").state == PluginEntryState::Unknown);
	}
}

TEST_CASE("バックアップ (1): 前から項があれば、cordmux の後で値が変わっていても、アンインストールで前の値に戻る")
{
	// 入れる前に取ったバックアップを、バックアップの JSON を通して持ち、Vencord が項を補った後の設定に当てる。
	const WritePlan plan = plan_write(kStateCompact, kTargetDir, std::nullopt, kNowUs, kDllDir, WriteScene::Pressed,
					  snapshot_plugin_entry(settings_with(kOtherEntry), "cordmux"));
	REQUIRE(plan.action == WriteAction::Write);
	Backup stored;
	REQUIRE(backup_from_json(backup_to_json(*plan.new_backup), &stored));

	const std::string after = settings_with(kAddedEntry);
	const std::optional<std::string> next = plan_restore_plugin_entry(after, "cordmux", stored.plugin_entry);
	REQUIRE(next.has_value());
	const nlohmann::ordered_json restored = nlohmann::ordered_json::parse(*next);
	CHECK(restored["plugins"]["cordmux"].dump() == kOtherEntry); // 項の中の順番もそのまま
	CHECK(restored["plugins"]["Alpha"].dump() == R"({"enabled":true})");
	CHECK(restored["plugins"]["Zeta"].dump() == R"({"enabled":false})");
	CHECK(restored["plugins"].items().begin().key() == "Alpha"); // 項の順番は変わらない
	CHECK(restored["theme"] == "x");
}

TEST_CASE("バックアップ (2): 前に項がなければ、アンインストールで項が消え、ほかの項と整形は変わらない")
{
	const WritePlan plan = plan_write(kStateCompact, kTargetDir, std::nullopt, kNowUs, kDllDir, WriteScene::Pressed,
					  snapshot_plugin_entry(R"({"plugins":{"Alpha":{}}})", "cordmux"));
	REQUIRE(plan.new_backup.has_value());
	CHECK(plan.new_backup->plugin_entry.state == PluginEntryState::Absent);

	const std::string after = settings_with(kAddedEntry);
	const std::optional<std::string> next = plan_restore_plugin_entry(after, "cordmux", plan.new_backup->plugin_entry);
	REQUIRE(next.has_value());
	CHECK(*next == cordmux::install::plan_remove_plugin_entry(after, "cordmux").value());
	CHECK(next->find("cordmux") == std::string::npos);
	CHECK(next->find("Zeta") != std::string::npos);
}

TEST_CASE("バックアップ (3)(4): バックアップが分からない (古いバックアップでキーがない、型が違う、取れなかった) ときは、設定に触らない")
{
	// (3) キーがない古いバックアップは、読めて、「分からない」になる。
	Backup old_backup;
	REQUIRE(backup_from_json(R"({"previous":null,"written":"w","written_at":1,"dll_dir":"d"})", &old_backup));
	CHECK(old_backup.plugin_entry.state == PluginEntryState::Unknown);
	// 型が違っても、バックアップ全体は失敗にしない (vencordDir を戻せなくなるため)。
	Backup odd;
	REQUIRE(backup_from_json(R"({"previous":null,"written":"w","written_at":1,"dll_dir":"d","plugin_entry":5})", &odd));
	CHECK(odd.plugin_entry.state == PluginEntryState::Unknown);
	Backup text_entry;
	REQUIRE(backup_from_json(R"({"previous":null,"written":"w","written_at":1,"dll_dir":"d","plugin_entry":"x"})",
				 &text_entry));
	CHECK(text_entry.plugin_entry.state == PluginEntryState::Unknown);

	// (4) 「分からない」のバックアップでは、どんな設定にも何も返さない。
	for (const std::string &text : {settings_with(kAddedEntry), std::string(R"({"plugins":{}})"), std::string("{not json")})
		CHECK_FALSE(plan_restore_plugin_entry(text, "cordmux", PluginEntryBackup{}).has_value());

	// 設定が読めなかったときのバックアップは「分からない」で、それを書いて読んでも「分からない」のまま。
	const WritePlan plan = plan_write(kStateCompact, kTargetDir, std::nullopt, kNowUs, kDllDir, WriteScene::Pressed,
					  snapshot_plugin_entry("{not json", "cordmux"));
	REQUIRE(plan.new_backup.has_value());
	CHECK(backup_to_json(*plan.new_backup).find("plugin_entry") == std::string::npos);
	Backup stored;
	REQUIRE(backup_from_json(backup_to_json(*plan.new_backup), &stored));
	CHECK(stored.plugin_entry.state == PluginEntryState::Unknown);
}

TEST_CASE("plan_write にバックアップを渡さなければ、新しく書くバックアップの値は「分からない」になる")
{
	const WritePlan plan = plan_write(kStateCompact, kTargetDir, std::nullopt, kNowUs, kDllDir, WriteScene::Pressed);
	REQUIRE(plan.new_backup.has_value());
	CHECK(plan.new_backup->plugin_entry.state == PluginEntryState::Unknown);
}

TEST_CASE("バックアップ (5): バックアップの JSON に書いて読むと、バックアップが元に戻る (3 つの状態と、項の中の順番)")
{
	for (const PluginEntryBackup &entry : {PluginEntryBackup{}, absent_entry(), present_entry(kOtherEntry)}) {
		CAPTURE(static_cast<int>(entry.state));
		const Backup original = backup_with_entry(entry);
		const std::string text = backup_to_json(original);
		Backup roundtrip;
		REQUIRE(backup_from_json(text, &roundtrip));
		CHECK(roundtrip.plugin_entry.state == entry.state);
		CHECK(roundtrip.plugin_entry.value_json == entry.value_json);
		CHECK(roundtrip.previous == original.previous); // ほかのキーは変わらない
		CHECK(roundtrip.written == original.written);
		// 読む側 (platform/) は、バックアップのキーがあっても今までのキーを読める。
		const auto read_back = cordmux::platform::parse_vencord_dir_backup(text);
		REQUIRE(read_back.has_value());
		CHECK(read_back->written == std::optional<std::string>(original.written));
	}
	CHECK(backup_to_json(backup_with_entry(absent_entry())).find(R"("plugin_entry":null)") != std::string::npos);
}

TEST_CASE("バックアップ (6): 分岐 3 と 4 では、どの場面でもバックアップを引き継ぎ、現在の項に置き換えない")
{
	const PluginEntryBackup inherited = present_entry(kOtherEntry);
	const PluginEntryBackup now = present_entry(kAddedEntry);
	const Backup current = backup_with_entry(inherited);
	for (const WriteScene scene : kAllScenes) {
		CAPTURE(static_cast<int>(scene));
		const WritePlan branch3 = plan_write(state_with(kOldWritten), kTargetDir, current, kNowUs, kDllDir, scene, now);
		REQUIRE(branch3.action == WriteAction::Write);
		CHECK(branch3.new_backup->plugin_entry.state == PluginEntryState::Present);
		CHECK(branch3.new_backup->plugin_entry.value_json == kOtherEntry);

		if (scene == WriteScene::Startup)
			continue; // 分岐 4 は起動時には書かない
		const WritePlan branch4 = plan_write(state_with(kPrev), kTargetDir, current, kNowUs, kDllDir, scene, now);
		REQUIRE(branch4.action == WriteAction::Write);
		CHECK(branch4.new_backup->plugin_entry.value_json == kOtherEntry);
	}
	// 引き継いだバックアップが「ない」でも「分からない」でも、現在の項にしない。
	for (const PluginEntryBackup &entry : {absent_entry(), PluginEntryBackup{}}) {
		const WritePlan plan = plan_write(state_with(kOldWritten), kTargetDir, backup_with_entry(entry), kNowUs, kDllDir,
						  WriteScene::Startup, now);
		REQUIRE(plan.action == WriteAction::Write);
		CHECK(plan.new_backup->plugin_entry.state == entry.state);
	}
}

TEST_CASE("バックアップ (7): 分岐 2 と、previous を現在の値にする分岐 5 (FixRequested、キーが消えた Pressed) では、バックアップが現在の項になる")
{
	const PluginEntryBackup now = present_entry(kOtherEntry);
	const Backup current = backup_with_entry(absent_entry()); // 前のバックアップは別の状態

	const WritePlan first = plan_write(state_with(kPrev), kTargetDir, std::nullopt, kNowUs, kDllDir,
					   WriteScene::Pressed, now);
	REQUIRE(first.action == WriteAction::Write);
	CHECK(first.new_backup->plugin_entry.value_json == kOtherEntry);

	const WritePlan fix = plan_write(state_with(kForeign), kTargetDir, current, kNowUs, kDllDir,
					 WriteScene::FixRequested, now);
	REQUIRE(fix.action == WriteAction::Write);
	CHECK(fix.new_backup->plugin_entry.state == PluginEntryState::Present);
	CHECK(fix.new_backup->plugin_entry.value_json == kOtherEntry);

	const WritePlan reset = plan_write(state_with(std::nullopt), kTargetDir, current, kNowUs, kDllDir,
					   WriteScene::Pressed, now);
	REQUIRE(reset.action == WriteAction::Write);
	CHECK(reset.new_backup->plugin_entry.value_json == kOtherEntry);

	// 書かない場合 (AlreadyOurs) は、バックアップをそのまま返す。
	const WritePlan same = plan_write(state_with(kTargetDir), kTargetDir, backup_with_entry(absent_entry()), kNowUs,
					  kDllDir, WriteScene::Pressed, now);
	REQUIRE(same.action == WriteAction::AlreadyOurs);
	CHECK(same.new_backup->plugin_entry.state == PluginEntryState::Absent);
}

TEST_CASE("plan_restore_plugin_entry は、項がなければ plugins の最後に足し、plugins もなければ作り、もう同じなら何も返さない")
{
	const PluginEntryBackup entry = present_entry(R"({"enabled":true,"token":"t"})");

	const std::optional<std::string> added =
		plan_restore_plugin_entry(R"({"plugins":{"Alpha":{"enabled":true}},"theme":"x"})", "cordmux", entry);
	REQUIRE(added.has_value());
	CHECK(*added == R"({"plugins":{"Alpha":{"enabled":true},"cordmux":{"enabled":true,"token":"t"}},"theme":"x"})");

	const std::optional<std::string> created = plan_restore_plugin_entry(R"({"theme":"x"})", "cordmux", entry);
	REQUIRE(created.has_value());
	CHECK(*created == R"({"theme":"x","plugins":{"cordmux":{"enabled":true,"token":"t"}}})");

	CHECK_FALSE(plan_restore_plugin_entry(*added, "cordmux", entry).has_value());
	CHECK_FALSE(plan_restore_plugin_entry(R"({"plugins":[1]})", "cordmux", entry).has_value());
	CHECK_FALSE(plan_restore_plugin_entry("{not json", "cordmux", entry).has_value());
	CHECK_FALSE(plan_restore_plugin_entry("[]", "cordmux", entry).has_value());
	CHECK_FALSE(plan_restore_plugin_entry(R"({"plugins":{}})", "cordmux", present_entry("{broken")).has_value());
}

TEST_CASE("バックアップの state_path: 書いた場所を JSON に残して読める")
{
	Backup original;
	original.previous = std::nullopt;
	original.written = kTargetDir;
	original.written_at_us = kNowUs;
	original.dll_dir = kDllDir;
	original.state_path = "D:/data/Vesktop/state.json";

	Backup roundtrip;
	REQUIRE(backup_from_json(backup_to_json(original), &roundtrip));
	CHECK(roundtrip.state_path == "D:/data/Vesktop/state.json");
}

TEST_CASE("バックアップの state_path: 古い版のバックアップ (キーなし) は空として読み、空ならキーを書かない")
{
	Backup original;
	original.previous = std::nullopt;
	original.written = kTargetDir;
	original.written_at_us = kNowUs;
	original.dll_dir = kDllDir;
	const std::string text = backup_to_json(original);
	CHECK(text.find("state_path") == std::string::npos);

	Backup roundtrip;
	roundtrip.state_path = "stale";
	REQUIRE(backup_from_json(text, &roundtrip));
	CHECK(roundtrip.state_path.empty());

	// 型が違う値も、失敗にせず空として読む (ないバックアップのために vencordDir まで戻せなくならない)。
	Backup wrong_type;
	REQUIRE(backup_from_json(R"({"previous":null,"written":"x","written_at":1,"dll_dir":"y","state_path":5})", &wrong_type));
	CHECK(wrong_type.state_path.empty());
}
