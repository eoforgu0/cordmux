// SPDX-License-Identifier: Apache-2.0

// 録画ファイルの作成の済んでいない録画のセッションが残る保存先を、一覧として設定のフォルダーのファイルに保存する。
// settings.json には現在の保存先しかないので、利用者が保存先を変えて OBS を起動し直すと、この一覧がなければ
// 前の保存先に残る作成待ちのセッションを探せない。Recorder (recorder.cpp) は起動のたびに一覧の保存先も探す。
// 一覧の更新の規則は update_pending_dirs、ファイルの読み書きは pending_dirs_file_store にある。
// libobs にも Qt にも依存しない。

#pragma once

// nlohmann/json は外部のライブラリなので、そのヘッダーが出す警告は抑える。
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <nlohmann/json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "platform/bundle_check.h"
#include "platform/platform.h"
#include "record/record_writer.h"

namespace cordmux::record {

// 一覧のファイルの名前。ファイルは設定のフォルダー (obs_module_config_path) に置き、settings.json とは分ける。
// settings.json はドックが自分のコピーから丸ごと書き直すので、録画の側も書くと、片方が書いた値をもう片方が
// 消すことがある。設定ウィンドウが、録画の側の変更を利用者の未保存の変更と判定することにもなる。
// アンインストールは設定のフォルダーを中身ごと削除するので、削除するファイルの一覧に追加する必要はない。
inline constexpr const char *kPendingDirsFileName = "record_pending_dirs.json";

// 保存先にアクセスできるかを同時に問い合わせる数の上限。一覧には普通は数個の保存先しかないが、壊れたファイルから
// 数百のスレッドを起動しないように上限を置く。
inline constexpr int kMaxPendingDirProbes = 16;

// 起動のときにアクセスできなかった保存先を、もう一度問い合わせるまでの待ち時間 (問い合わせ直すのは 1 度だけ)。
// ログインの直後にネットワークの場所が再接続するまでと、外付けのドライブがスリープから復帰するまで (どちらも
// 普通は数十秒) より長く、利用者が起動の直後に最初の録画ファイルを作成するより前に終わる長さにした。目安の値で、
// 測って決めたものではない。テストは Recorder::set_pending_dir_reprobe_delay_ms で短くする。
inline constexpr int kPendingDirReprobeDelayMs = 60 * 1000;

// 一覧のファイルを読み書きする関数のセット。`load` は現在のファイルの中身を読み、ファイルがなければ空の一覧を、
// 読めなければ値なしを返す。`save` は一覧を書き、書けなければ false を返す。
// Recorder は書く直前に `load` で読み直し、その中身に自分の変更を加えてから `save` する。前に読んだコピーから
// 書くと、同じ設定のフォルダーを使うもう 1 つの OBS が追加した保存先を消すためである。
struct PendingDirsStore {
	std::function<std::optional<std::vector<std::string>>()> load;
	std::function<bool(const std::vector<std::string> &)> save;
};

// 探した保存先 1 つの結果。recorder.cpp の集計が作る。
struct SearchDirResult {
	std::string base_dir;          // セッションのフォルダーの親フォルダー (探す保存先の値そのもの)
	bool reachable = false;        // アクセスできて探せた (`cordmux` の中を読めたか、なくても空き容量を読めた)
	bool holds_unfinished = false; // 録画ファイルの作成の済んでいないセッションがある (意味は update_pending_dirs のコメント)
};

// 一覧のファイルの本文 (`{"dirs": ["D:/rec", ...]}`) を読む。形が違えば空の一覧を返す。一覧が壊れていても、
// 現在の保存先の集計はそのまま続く。
inline std::vector<std::string> parse_pending_dirs(const std::string &text)
{
	std::vector<std::string> dirs;
	const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
	if (j.is_discarded() || !j.is_object())
		return dirs;
	const auto it = j.find("dirs");
	if (it == j.end() || !it->is_array())
		return dirs;
	for (const nlohmann::json &item : *it) {
		if (!item.is_string())
			continue;
		const std::string dir = item.get<std::string>();
		if (dir.empty())
			continue;
		bool seen = false;
		for (const std::string &existing : dirs)
			seen = seen || platform::same_dir_path(existing, dir);
		if (!seen)
			dirs.push_back(dir);
	}
	return dirs;
}

// 保存先の一覧から、ファイルに書く本文 (`{"dirs": [...]}`) を作る。末尾には改行を付ける。
inline std::string format_pending_dirs(const std::vector<std::string> &dirs)
{
	nlohmann::json j = nlohmann::json::object();
	j["dirs"] = dirs;
	return j.dump(2) + "\n";
}

// 探した結果で一覧を更新する。録画ファイルの作成の済んでいないセッションがある保存先は追加し、アクセスできて探せて、作成の
// 済んでいないセッションが 1 つもなかった保存先だけを外す。アクセスできなかった保存先 (外した外付けのドライブ、
// 接続していないネットワークの場所) と、探さなかった保存先は残す。並びは前の一覧の順を保ち、追加する保存先は末尾に
// 置く。
// 「録画ファイルの作成の済んでいないセッション」は、作成待ちに数えるセッション (Pending・Failed・中断したセッション) の
// ほかに、録画中か作成中のセッション (このプロセスのものも、ほかの OBS のものも) と、録画ログを読めなかった
// セッションを含む。録画中か作成中のセッションは数えないが、録画ファイルの作成の途中で OBS が止まると、その保存先に
// 中断したセッションが残る。録画ログを読めなかったセッションは、録画ファイルの作成が済んだかどうかが分からない。
// 探したすべての保存先 (現在の保存先も含めて) に同じ規則を使う。現在の保存先も一覧に入れておかないと、
// 次にその保存先を変えたときに、そこに残るセッションを探せなくなる。
// すべてのドライブを探すと、アクセスできないネットワークの場所で起動が止まるので、一覧に載った保存先だけを探す。
inline std::vector<std::string> update_pending_dirs(const std::vector<std::string> &remembered,
						     const std::vector<SearchDirResult> &results)
{
	std::vector<std::string> out;
	for (const std::string &dir : remembered) {
		bool drop = false;
		for (const SearchDirResult &result : results) {
			if (platform::same_dir_path(result.base_dir, dir) && result.reachable && !result.holds_unfinished)
				drop = true;
		}
		bool held = false;
		for (const SearchDirResult &result : results) {
			if (platform::same_dir_path(result.base_dir, dir) && result.holds_unfinished)
				held = true;
		}
		if (!drop || held)
			out.push_back(dir);
	}
	for (const SearchDirResult &result : results) {
		if (!result.holds_unfinished || result.base_dir.empty())
			continue;
		bool present = false;
		for (const std::string &dir : out)
			present = present || platform::same_dir_path(dir, result.base_dir);
		if (!present)
			out.push_back(result.base_dir);
	}
	return out;
}

// セッションを始めた保存先を一覧に追加する。既にあれば同じ一覧を返す (並びは変えず、追加する保存先は末尾に置く)。
// Recorder はセッションを始めるたびに呼ぶ。集計のときだけ一覧を更新すると、保存先 A で録画している間に保存先を
// B に変え、録画ファイルの作成の前に OBS か PC が止まったとき、次の起動では B だけを探し、A に残った中断したセッションを
// 数えられない。
inline std::vector<std::string> add_pending_dir(std::vector<std::string> remembered, const std::string &dir)
{
	if (dir.empty())
		return remembered;
	for (const std::string &existing : remembered) {
		if (platform::same_dir_path(existing, dir))
			return remembered;
	}
	remembered.push_back(dir);
	return remembered;
}

// 一覧のファイル (`store_path`) を読み書きする PendingDirsStore を作る。pipeline.cpp とテストが同じ関数を使う。
// `write_gate` は書き込みを包む関数で、空なら包まない。製品では install::write_unless_uninstalling を渡し、
// アンインストールが始まった後は書かない。
inline PendingDirsStore pending_dirs_file_store(
	const std::string &store_path,
	std::function<bool(const std::function<bool()> &)> write_gate = std::function<bool(const std::function<bool()> &)>())
{
	PendingDirsStore store;
	store.load = [store_path]() -> std::optional<std::vector<std::string>> {
		std::string text;
		if (read_whole_file(store_path, &text))
			return parse_pending_dirs(text);
		// 読めなかった。OS がファイルはないと答えたときだけ空の一覧を返し、ほかは分からないので値なしを返す。
		// 値なしを受けると、recorder.cpp の change_pending_dirs_locked は書かずに戻り、起動のとき
		// (remember_pending_dirs) は一覧の保存先を 1 つも問い合わせない
		// (空の一覧と同じ。現在の保存先だけを探す)。
		std::error_code ec;
		const std::filesystem::file_status status = std::filesystem::status(std::filesystem::u8path(store_path), ec);
		if (status.type() == std::filesystem::file_type::not_found)
			return std::vector<std::string>();
		return std::nullopt;
	};
	store.save = [store_path, write_gate](const std::vector<std::string> &dirs) {
		const auto write = [&store_path, &dirs]() {
			// 設定のフォルダーがなければ書かない。アンインストールが削除したフォルダーを
			// 作り直さないためである。
			std::error_code ec;
			const std::filesystem::path store = std::filesystem::u8path(store_path);
			if (!std::filesystem::is_directory(store.parent_path(), ec))
				return false;
			return platform::write_file_atomic(store_path, format_pending_dirs(dirs));
		};
		return write_gate ? write_gate(write) : write();
	};
	return store;
}

} // namespace cordmux::record
