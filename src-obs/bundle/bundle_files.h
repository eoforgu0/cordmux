// SPDX-License-Identifier: Apache-2.0

// 診断情報の zip に入れる 5 つのファイルを作り、トークンを伏せる関数。トークンは 2 段階で守る。1 段目で中身から
// トークンと秘密の値を伏せ (RedactFn)、2 段目で、作り終えた全文にトークンが残っていないかを確認する (RevealFn)。
// どちらも呼び出し元が渡す関数で、トークンを知っているのは plugin-main.cpp だけである。トークンのほかに、Discord の
// ユーザーIDと Windows の利用者のフォルダーも zip に残さない。libobs にも Qt にも依存
// しないので、伏せる処理をやめると 2 段目で検出されることを、単体テストで確認できる
// (tests/test_bundle_files.cpp)。ファイルに書くのは bundle/export_bundle.cpp である。

#pragma once

#include <cctype>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bundle/bundle_manifest.h"
#include "bundle/zip_writer.h"
#include "platform/platform.h"

namespace cordmux::bundle {

// 伏せる関数 (1 段目)。受け取った文字列から、トークンを伏せたものを返す。
using RedactFn = std::function<std::string(const std::string &)>;

// 確認する関数 (2 段目)。作り終えた全文にトークンが残っていれば true を返す。ポート番号は調べない。診断情報は
// 利用者が自分で選んで渡すもので、ポート番号はトークンがなければ何にも使えないうえ、開いたポート番号は診断に必要な
// 情報として manifest.json に入れているためである。
using RevealFn = std::function<bool(std::string_view)>;

// zip に入れる 6 つのファイルの名前。zip の中にフォルダーは作らない。ログは、出来事の行 (plugin.log と discord.log。
// 通話の全体を残す) と、数秒ごとの統計の行 (*-stats.log。直近の行だけ) を分けて入れる。
inline constexpr const char *kManifestName = "manifest.json";
inline constexpr const char *kPluginLogName = "plugin.log";
inline constexpr const char *kPluginStatsLogName = "plugin-stats.log";
inline constexpr const char *kDiscordLogName = "discord.log";
inline constexpr const char *kDiscordStatsLogName = "discord-stats.log";
inline constexpr const char *kSettingsName = "settings.json";

// zip に入れるデータ。
struct BundleMaterials {
	ManifestInput manifest;

	// cordmux.dll のログのリングバッファーと、送信側から届いたログのリングバッファーの行。どれも古い順。*_log は出来事の
	// 行、*_stats_log は統計の行である。*_dropped は、上限を超えて捨てた行の数である。
	std::vector<std::string> plugin_log;
	std::uint64_t plugin_log_dropped = 0;
	std::vector<std::string> plugin_stats_log;
	std::uint64_t plugin_stats_log_dropped = 0;
	std::vector<std::string> discord_log;
	std::uint64_t discord_log_dropped = 0;
	std::vector<std::string> discord_stats_log;
	std::uint64_t discord_stats_log_dropped = 0;

	// 現在の設定 (settings.json の中身)。settings.json にトークンは入っていない。
	std::string settings_json;

	// cordmux を動かしている PC の Windows の利用者のプロファイルのフォルダー (C:\Users\<名前>。OS から読んだ値)。ファイルの中のこの
	// 文字列は %USERPROFILE% に置き換える。空なら置き換えない (名前の推測では置き換えない)。
	std::string user_profile_dir;
};

namespace detail {

// 識別子を語に分ける。camelCase の境目と、"_"、"-"、"."、空白で分け、小文字にそろえる。数字は語の一部として扱う
// ("token2" は 1 語)。
inline std::vector<std::string> split_identifier_words(std::string_view name)
{
	std::vector<std::string> words;
	std::string current;
	for (std::size_t i = 0; i < name.size(); ++i) {
		const unsigned char c = static_cast<unsigned char>(name[i]);
		if (c == '_' || c == '-' || c == '.' || c == ' ') {
			if (!current.empty())
				words.push_back(current);
			current.clear();
			continue;
		}
		// 小文字か数字の後に大文字が来たら、語の境目とする ("accessToken" は access と token)。
		if (std::isupper(c) && !current.empty()) {
			const unsigned char prev = static_cast<unsigned char>(name[i - 1]);
			if (std::islower(prev) || std::isdigit(prev)) {
				words.push_back(current);
				current.clear();
			}
		}
		current.push_back(static_cast<char>(std::tolower(c)));
	}
	if (!current.empty())
		words.push_back(current);
	return words;
}

// 秘密の値を持つキーに使われる語。語として完全に一致したときだけ秘密とするので、"codec"、"encoder"、"keyframe" は
// 1 語のままで一致しない。部分一致にすると、こうしたキーの値まで伏せてしまう。
inline bool is_secret_word(const std::string &word)
{
	static const char *const words[] = {"secret", "token",   "password",      "passwd", "passphrase",
					    "key",    "keys",    "credential",    "credentials",
					    "auth",   "cookie",  "authorization", "code"};
	for (const char *candidate : words) {
		if (word == candidate)
			return true;
	}
	return false;
}

// key が秘密の値を持つキーか。語に分けてから、どれかの語が完全に一致するかで決める。語に分けずにキー全体で比べると、
// accessToken のようなキーを見逃す。
inline bool is_secret_key(std::string_view key)
{
	for (const std::string &word : split_identifier_words(key)) {
		if (is_secret_word(word))
			return true;
	}
	return false;
}

} // namespace detail

// 伏せた値の代わりに入れる文字列 (読んだ人が、ここに値があったと分かるようにする)。
inline constexpr const char *kRedactedMark = "[redacted]";

// text の中の `"<キー>": "<値>"` の形のうち、キーが秘密の値を持つもの (detail::is_secret_key) の値を伏せる。JSON として
// 読まないのは、ログの行の中にある JSON の一部にも同じように使うためである (ログの行は自由な文字列で、行全体が
// JSON とは限らない)。
inline std::string redact_secret_keys(const std::string &text)
{
	std::string out;
	out.reserve(text.size());

	std::size_t i = 0;
	while (i < text.size()) {
		if (text[i] != '"') {
			out.push_back(text[i]);
			++i;
			continue;
		}
		// キーらしい文字列の終わりを探す (キーにはエスケープの文字がない前提で、閉じる引用符までとする)。
		const std::size_t key_start = i + 1;
		std::size_t key_end = key_start;
		while (key_end < text.size() && text[key_end] != '"' && text[key_end] != '\n')
			++key_end;
		if (key_end >= text.size() || text[key_end] != '"') {
			out.push_back(text[i]);
			++i;
			continue;
		}
		const std::string key = text.substr(key_start, key_end - key_start);

		// 閉じる引用符の後の ":" と空白を読み飛ばして、値の始まりを見る。
		std::size_t cursor = key_end + 1;
		while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '\t'))
			++cursor;
		const bool has_colon = cursor < text.size() && text[cursor] == ':';
		if (has_colon) {
			++cursor;
			while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '\t'))
				++cursor;
		}

		if (!has_colon || cursor >= text.size() || text[cursor] != '"' || !detail::is_secret_key(key)) {
			out.append(text, i, key_end + 1 - i);
			i = key_end + 1;
			continue;
		}

		// 値の文字列の終わり (エスケープされた文字を飛ばす)。
		std::size_t value_end = cursor + 1;
		while (value_end < text.size() && text[value_end] != '"') {
			if (text[value_end] == '\\' && value_end + 1 < text.size())
				++value_end;
			++value_end;
		}
		if (value_end >= text.size()) {
			out.append(text, i, key_end + 1 - i);
			i = key_end + 1;
			continue;
		}

		out.append(text, i, cursor + 1 - i); // キーと ":" と、値の開く引用符まで
		out += kRedactedMark;
		out.push_back('"');
		i = value_end + 1;
	}
	return out;
}

// cordmux.dll が使う伏せ方 (1 段目)。次の 2 つを順に行う。
//   (a) トークンの文字列を伏せる。トークンを守るのは、こちらである。
//   (b) 秘密の値を持つキーの値を伏せる (redact_secret_keys)。トークン以外の秘密の値に備える。
// secret_hex が空なら (a) は何もしない (待ち受けを始めていないときは、トークンがない)。
inline RedactFn make_redact(std::string secret_hex)
{
	return [secret = std::move(secret_hex)](const std::string &text) {
		std::string out = text;
		if (!secret.empty()) {
			std::string replaced;
			replaced.reserve(out.size());
			std::size_t from = 0;
			for (;;) {
				const std::size_t hit = out.find(secret, from);
				if (hit == std::string::npos) {
					replaced.append(out, from, std::string::npos);
					break;
				}
				replaced.append(out, from, hit - from);
				replaced += kRedactedMark;
				from = hit + secret.size();
			}
			out = std::move(replaced);
		}
		return redact_secret_keys(out);
	};
}

namespace detail {

inline bool is_ascii_digit(char c)
{
	return c >= '0' && c <= '9';
}

inline bool is_ascii_alnum(char c)
{
	return is_ascii_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// 大文字と小文字を区別せずに (ASCII だけ)、text の pos から word が続くか。Windows の場所は大文字と小文字を区別しない。
inline bool matches_ignoring_ascii_case(const std::string &text, std::size_t pos, const std::string &word)
{
	if (pos + word.size() > text.size())
		return false;
	for (std::size_t i = 0; i < word.size(); ++i) {
		const unsigned char a = static_cast<unsigned char>(text[pos + i]);
		const unsigned char b = static_cast<unsigned char>(word[i]);
		if (std::tolower(a) != std::tolower(b))
			return false;
	}
	return true;
}

// フォルダーの名前の続きになりうる文字 (英数字、"_"、ASCII でない文字)。プロファイルのフォルダーの次にこの文字が
// 続くときは、別の名前のフォルダーなので置き換えない。
inline bool continues_folder_name(char c)
{
	const unsigned char u = static_cast<unsigned char>(c);
	return u >= 0x80 || std::isalnum(u) || c == '_';
}

} // namespace detail

// Discord のユーザーID (17 桁から 20 桁の数) の代わりに入れる印の対応。現れた順に user-1、user-2 と付ける。同じ
// 対応を zip のファイル全部に使うので、ファイルをまたいで同じ人を同じ印で追える。
class UserIdAliases {
public:
	std::string alias_for(const std::string &id)
	{
		auto it = aliases_.find(id);
		if (it == aliases_.end())
			it = aliases_.emplace(id, "user-" + std::to_string(aliases_.size() + 1)).first;
		return it->second;
	}

private:
	std::map<std::string, std::string> aliases_;
};

// text の中の、前後が ASCII の英数字でない 17 桁から 20 桁の数を、aliases の印に置き換える。ユーザーIDは Discord の
// 識別子 (snowflake) で、この桁数に収まり、ログと設定の中では "="、引用符、"/"、空白、"," などで区切られて現れる。
// 英字につながった数字の並びは置き換えない。16 進のハッシュの中の数字だけの並びを置き換えると、ハッシュが読めなく
// なり、zip ファイルの照合に使えない。ログの時刻などが偶然この桁数になると、それも置き換わる (情報が減るだけで、害はない)。
inline std::string replace_discord_user_ids(const std::string &text, UserIdAliases &aliases)
{
	std::string out;
	out.reserve(text.size());
	std::size_t i = 0;
	while (i < text.size()) {
		if (!detail::is_ascii_digit(text[i])) {
			out.push_back(text[i]);
			++i;
			continue;
		}
		std::size_t end = i;
		while (end < text.size() && detail::is_ascii_digit(text[end]))
			++end;
		const std::size_t length = end - i;
		const bool glued_before = i > 0 && detail::is_ascii_alnum(text[i - 1]);
		const bool glued_after = end < text.size() && detail::is_ascii_alnum(text[end]);
		if (length >= 17 && length <= 20 && !glued_before && !glued_after)
			out += aliases.alias_for(text.substr(i, length));
		else
			out.append(text, i, length);
		i = end;
	}
	return out;
}

// text の中の user_profile_dir を %USERPROFILE% に置き換える。区切りが "\" の形、"/" の形、JSON の文字列の中の
// "\\" の形の 3 つを置き換える。user_profile_dir は OS から読んだ値で、名前から推測した場所は渡さない。空なら
// 何もしない。
inline std::string replace_user_profile_dir(const std::string &text, const std::string &user_profile_dir)
{
	std::string base = user_profile_dir;
	while (!base.empty() && (base.back() == '\\' || base.back() == '/'))
		base.pop_back();
	if (base.empty())
		return text;

	std::string backslash_form;
	std::string slash_form;
	std::string escaped_form;
	for (const char c : base) {
		const bool separator = c == '\\' || c == '/';
		backslash_form.push_back(separator ? '\\' : c);
		slash_form.push_back(separator ? '/' : c);
		escaped_form += separator ? std::string("\\\\") : std::string(1, c);
	}
	// 長い形を先に試す。
	const std::string forms[] = {escaped_form, backslash_form, slash_form};

	std::string out;
	out.reserve(text.size());
	std::size_t i = 0;
	while (i < text.size()) {
		bool replaced = false;
		for (const std::string &form : forms) {
			if (!detail::matches_ignoring_ascii_case(text, i, form))
				continue;
			const std::size_t next = i + form.size();
			if (next < text.size() && detail::continues_folder_name(text[next]))
				continue;
			out += platform::kUserProfileMark;
			i = next;
			replaced = true;
			break;
		}
		if (!replaced) {
			out.push_back(text[i]);
			++i;
		}
	}
	return out;
}

namespace detail {

// 1 つのログのリングバッファーの行を、1 つのファイルの中身にする。捨てた行があれば、そのことを示す 1 行を先頭に
// 書く (途中から始まるログを、そうと分からないまま渡さないため)。
inline std::string log_file_text(const std::vector<std::string> &lines, std::uint64_t dropped)
{
	std::string out;
	if (dropped > 0)
		out += "# dropped " + std::to_string(dropped) + " older lines (ring limit)\n";
	for (const std::string &line : lines) {
		out += line;
		out.push_back('\n');
	}
	return out;
}

} // namespace detail

// zip に入れる 6 つのファイルを作る。どのファイルの中身も redact を通す (1 段目)。redact が空なら何も伏せない
// (伏せる処理がないと 2 段目で検出されることを、単体テストがこの方法で確認する)。その後で、プロファイルのフォルダーと
// Discord のユーザーIDを置き換える。ユーザーIDの印の対応は、ファイルを並べた順に現れた順で、全部のファイルで共通にする。
inline std::vector<ZipEntry> build_bundle_entries(const BundleMaterials &materials, const RedactFn &redact)
{
	UserIdAliases aliases;
	const auto apply = [&redact, &aliases, &materials](std::string text) {
		if (redact)
			text = redact(text);
		text = replace_user_profile_dir(text, materials.user_profile_dir);
		return replace_discord_user_ids(text, aliases);
	};

	std::vector<ZipEntry> entries;
	entries.reserve(6);
	entries.push_back({kManifestName, apply(build_manifest_json(materials.manifest))});
	entries.push_back(
		{kPluginLogName, apply(detail::log_file_text(materials.plugin_log, materials.plugin_log_dropped))});
	entries.push_back({kPluginStatsLogName, apply(detail::log_file_text(materials.plugin_stats_log,
									     materials.plugin_stats_log_dropped))});
	entries.push_back(
		{kDiscordLogName, apply(detail::log_file_text(materials.discord_log, materials.discord_log_dropped))});
	entries.push_back({kDiscordStatsLogName, apply(detail::log_file_text(materials.discord_stats_log,
									      materials.discord_stats_log_dropped))});
	entries.push_back({kSettingsName, apply(materials.settings_json)});
	return entries;
}

// 2 段目の確認。作り終えた全文にトークンが残っていれば true を返す。true なら書き出してはいけない。reveals が空なら
// false を返す。
inline bool bundle_reveals_secret(const std::vector<ZipEntry> &entries, const RevealFn &reveals)
{
	if (!reveals)
		return false;
	std::string all;
	for (const ZipEntry &entry : entries) {
		all += entry.name;
		all.push_back('\n');
		all += entry.content;
		all.push_back('\n');
	}
	return reveals(all);
}

} // namespace cordmux::bundle
