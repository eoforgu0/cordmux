// SPDX-License-Identifier: Apache-2.0

// 同梱の Vencord のファイルが変わっていないかと、Vesktop の設定 (state.json) が指す場所を調べる関数。Vencord の
// 更新 (Force Update) が data/vencord のファイルを上書きすることは防げないので、コピー元 (data/vencord-src) と毎回
// 比べて見つける。ファイルを読み、ハッシュを計算するのは呼び出し元 (platform/os_probe.h) で、ここは結果を比べる
// だけである。同じ入力には必ず同じ結果を返し、ファイルにも OS にもアクセスしない。

#pragma once

// nlohmann/json は外部のライブラリなので、そのヘッダーが出す警告は表示しない。
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <nlohmann/json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cordmux::platform {

// 比べる 4 つのファイル。Vesktop が vencordDir を有効なフォルダーとみなす条件は、この 4 つのファイルがあることだけで
// ある (Vesktop v1.6.9 の src/main/utils/vencordLoader.ts の isValidVencordInstall。見るのは FILES_TO_DOWNLOAD の
// 4 つ)。package.json は、この条件には入らない。Vesktop が公式の Vencord をダウンロードするときにも同じフォルダーに
// 置く (downloadVencordFiles が "{}" を書く) ので、同梱のフォルダーにも置いておく。中身は比べない (置いてあるかは、
// 配布用の zip を作るときに確認する)。
inline constexpr const char *kVencordBundleFiles[] = {
	"vencordDesktopMain.js",
	"vencordDesktopPreload.js",
	"vencordDesktopRenderer.js",
	"vencordDesktopRenderer.css",
};
inline constexpr std::size_t kVencordBundleFileCount = 4;

// 1 つのファイルを読んだ結果。found が false なら読めなかった (ないか、権限がない)。
struct FileDigest {
	std::string name;       // ファイル名 (kVencordBundleFiles の 1 つ)
	bool found = false;     // 読めたか
	std::string sha256_hex; // found が true のときだけ意味を持つ
};

// 比べた結果。Unknown は、まだ確認していないときと、確認できなかったときの両方を含む (コピー元が読めないときは、
// 違うとは言わない)。ドックは Unknown のときに、ファイルが違うとは表示しない (dock/build_observation.cpp)。
enum class BundleState {
	Unknown,  // コピー元が読めないので比べられなかった
	Matches,  // 4 つのファイルがどれもコピー元と同じ
	Mismatch, // 1 つ以上が欠けている、または中身が違う
};

struct BundleComparison {
	BundleState state = BundleState::Unknown;
	std::vector<std::string> missing;   // data/vencord から読めなかったファイルの名前
	std::vector<std::string> differing; // 中身がコピー元と違うファイルの名前
};

// コピー元 (data/vencord-src) と data/vencord を読んだ結果を比べる。original に 4 つのファイルがそろっていない (配布した
// コピー元が壊れているか、読めない) ときは Unknown を返す。data/vencord で読めなかったファイルと中身が違うファイルは、
// どのファイルかを表示できるように、名前の一覧で返す。
inline BundleComparison compare_bundle(const std::vector<FileDigest> &original, const std::vector<FileDigest> &shown)
{
	const auto find = [](const std::vector<FileDigest> &list, const std::string &name) -> const FileDigest * {
		for (const auto &entry : list) {
			if (entry.name == name)
				return &entry;
		}
		return nullptr;
	};

	BundleComparison out;
	for (std::size_t i = 0; i < kVencordBundleFileCount; ++i) {
		const std::string name = kVencordBundleFiles[i];
		const FileDigest *src = find(original, name);
		if (!src || !src->found) {
			out.state = BundleState::Unknown;
			out.missing.clear();
			out.differing.clear();
			return out;
		}
		const FileDigest *dst = find(shown, name);
		if (!dst || !dst->found) {
			out.missing.push_back(name);
			continue;
		}
		if (dst->sha256_hex != src->sha256_hex)
			out.differing.push_back(name);
	}
	out.state = (out.missing.empty() && out.differing.empty()) ? BundleState::Matches : BundleState::Mismatch;
	return out;
}

// state.json から vencordDir を読む。キーがないとき、文字列でないとき、JSON として読めないときは nullopt を返す。
// キーがあって空なら空の文字列を返し、キーがないことと区別する。読むだけで、書き戻すのは install/ の役目である。
inline std::optional<std::string> parse_vencord_dir(std::string_view state_json)
{
	const nlohmann::json j = nlohmann::json::parse(state_json, nullptr, /*allow_exceptions=*/false);
	if (j.is_discarded() || !j.is_object())
		return std::nullopt;
	const auto it = j.find("vencordDir");
	if (it == j.end() || !it->is_string())
		return std::nullopt;
	return it->get<std::string>();
}

// state.json を vencordDir の読み書きの対象にできる形か。JSON のオブジェクトで、vencordDir があるなら文字列であること
// (install/ の plan_write が書いてよいとする形と同じ)。壊れていて確認できないものを「vencordDir がない」と
// 同じに扱わないために、読めたかの判定に使う。
inline bool state_json_usable(std::string_view state_json)
{
	const nlohmann::json j = nlohmann::json::parse(state_json, nullptr, /*allow_exceptions=*/false);
	if (j.is_discarded() || !j.is_object())
		return false;
	const auto it = j.find("vencordDir");
	return it == j.end() || it->is_string();
}

// バックアップ (vencord_dir_backup.json。キーは previous、written、written_at、dll_dir) のうち、ドックの表示に使う 2 つ
// だけを読んだ値。書くのは install/ の役目で、ここは読むだけである。
//
// install/vencord_dir.h の backup_from_json() とは読み方が違う。あちらは書き直してよいかを決めるために 4 つのキーが
// そろっていることを求め (1 つでも欠ければバックアップとして使わない)、こちらは表示できる分だけをキーごとに読む
// (片方が壊れていても、もう片方は使う)。キーの名前は install/ と同じでなければならず、
// tests/test_install_vencord_dir.cpp が、backup_to_json() の出力をこの関数で読んで確認する。
//
// written_at はエポックからのマイクロ秒として読む。Vesktop の起動の時刻と比べる値なので、ledger::Observation の時刻と
// 同じ単位にする。
struct VencordDirBackup {
	std::optional<std::string> written;        // cordmux が最後に書いた vencordDir
	std::optional<std::int64_t> written_at_us; // cordmux が最後に Vesktop の設定を書いた時刻
};

// バックアップを読む。JSON として読めなければ nullopt を返す (壊れたバックアップを、書いていないことと混同しない)。
// キーごとに読むので、片方が壊れていても、もう片方は使える。
inline std::optional<VencordDirBackup> parse_vencord_dir_backup(std::string_view json)
{
	const nlohmann::json j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
	if (j.is_discarded() || !j.is_object())
		return std::nullopt;

	VencordDirBackup out;
	const auto written = j.find("written");
	if (written != j.end() && written->is_string())
		out.written = written->get<std::string>();
	const auto written_at = j.find("written_at");
	if (written_at != j.end() && written_at->is_number_integer())
		out.written_at_us = written_at->get<std::int64_t>();
	return out;
}

// 場所を比べるための形にする。区切りを / にそろえ、末尾の区切りを取り除き、英字を小文字にする。文字列のまま比べると、
// 同じ場所を指す `C:\x\y` と `C:/x/y/` が別の場所になり、ドックが、Vesktop の設定が別の場所を指していると誤って表示
// する。大文字と小文字を区別しないのは、現在の対象が Windows の場所だからである (ほかの OS に広げるときは見直す)。
// 短い名前、ジャンクション、相対の場所は解決しない (解決するには OS に問い合わせる必要がある)。
inline std::string normalize_dir_path(std::string_view path)
{
	std::string out;
	out.reserve(path.size());
	for (const char c : path)
		out.push_back(c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	while (out.size() > 1 && out.back() == '/')
		out.pop_back();
	return out;
}

// 2 つの場所が同じ場所を指しているか (normalize_dir_path の形にして比べるだけ)。
inline bool same_dir_path(std::string_view a, std::string_view b)
{
	return normalize_dir_path(a) == normalize_dir_path(b);
}

// フォルダーとファイル名をつなぐ (区切りは / を使う。Windows の API も / を受け取る)。
inline std::string join_path(std::string_view dir, std::string_view name)
{
	std::string out(dir);
	while (!out.empty() && (out.back() == '/' || out.back() == '\\'))
		out.pop_back();
	out.push_back('/');
	out.append(name);
	return out;
}

} // namespace cordmux::platform
