// SPDX-License-Identifier: Apache-2.0

// locale_stub.h の実装。ini の読み方は OBS 31.1.1 の libobs/util/text-lookup.c の lookup_addfiledata と同じ規則にする。
// CR は空白に置き換えてから読み (text_lookup_add)、行頭の `#` から行末までは読み飛ばす。キーは `=` か空白の手前まで、
// 値は `=` の直後から。値が `"` で始まれば、次の (直前が `\` でない) `"` か行末までが中身 (両端の `"` は除く) で、
// そうでなければ次の空白の手前まで。中身の `\n` `\t` `\r` `\"` を、この順に 1 回ずつ全体で置き換える (convert_string)。
// 同じキーが 2 回あれば、後のものを使う (HASH_REPLACE_STR)。libobs の文字列句解析をそのまま移植したのではなく、製品の
// ini が使う形 (キー="値") で同じ結果になる範囲で書いている。

#include "locale_stub.h"

#include <QFileInfo>

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QFile>
#include <QPointer>
#include <QWidget>

#include <string>
#include <unordered_map>

namespace cordmux::ui_preview {

namespace {

using Table = std::unordered_map<std::string, std::string>;

// cordmux の ini (obs_module_text)。
Table &table()
{
	static Table map;
	return map;
}

// OBS 自身の ini (obs_frontend_get_locale_string)。
Table &frontend_table()
{
	static Table map;
	return map;
}

// obs_frontend_get_main_window が返すメインウィンドウ (プレビューツールが作ったもの)。
QPointer<QWidget> &main_window_slot()
{
	static QPointer<QWidget> window;
	return window;
}

bool &theme_dark_slot()
{
	static bool dark = true;
	return dark;
}

bool is_space(char ch)
{
	return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

// dstr_replace と同じく、左から重ならないように全部置き換える。
void replace_all(std::string &text, const std::string &from, const std::string &to)
{
	std::size_t pos = 0;
	while ((pos = text.find(from, pos)) != std::string::npos) {
		text.replace(pos, from.size(), to);
		pos += to.size();
	}
}

std::string convert_string(std::string value)
{
	replace_all(value, "\\n", "\n");
	replace_all(value, "\\t", "\t");
	replace_all(value, "\\r", "\r");
	replace_all(value, "\\\"", "\"");
	return value;
}

void add_file_data(Table &into, std::string data)
{
	// text_lookup_add: CR を空白に置き換える。
	for (char &ch : data) {
		if (ch == '\r')
			ch = ' ';
	}
	// 先頭の BOM を除く (os_fread_utf8 も除く)。
	if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
	    static_cast<unsigned char>(data[1]) == 0xBB && static_cast<unsigned char>(data[2]) == 0xBF)
		data.erase(0, 3);

	std::size_t pos = 0;
	const std::size_t size = data.size();
	while (pos < size) {
		const std::size_t line_end_found = data.find('\n', pos);
		const std::size_t line_end = line_end_found == std::string::npos ? size : line_end_found;
		const std::string line = data.substr(pos, line_end - pos);
		pos = line_end + 1;

		if (line.empty() || line[0] == '#' || is_space(line[0]))
			continue;

		std::size_t i = 0;
		while (i < line.size() && line[i] != '=' && !is_space(line[i]) && line[i] != '#')
			++i;
		const std::string key = line.substr(0, i);
		if (key.empty() || i >= line.size() || line[i] != '=')
			continue;
		++i; // '='
		if (i >= line.size())
			continue;

		std::string value;
		if (line[i] == '"') {
			++i;
			bool was_backslash = false;
			std::size_t j = i;
			for (; j < line.size(); ++j) {
				if (!was_backslash) {
					if (line[j] == '\\')
						was_backslash = true;
					else if (line[j] == '"')
						break;
				} else {
					was_backslash = false;
				}
			}
			value = line.substr(i, j - i);
		} else {
			std::size_t j = i;
			while (j < line.size() && !is_space(line[j]))
				++j;
			value = line.substr(i, j - i);
		}
		into[key] = convert_string(value);
	}
}

bool add_file(Table &into, const QString &path, QString *error)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		if (error != nullptr)
			*error = QStringLiteral("cannot open %1").arg(path);
		return false;
	}
	const QByteArray bytes = file.readAll();
	add_file_data(into, std::string(bytes.constData(), static_cast<std::size_t>(bytes.size())));
	return true;
}

} // namespace

bool load_locale(const QString &locale_dir, const QString &lang, QString *error)
{
	table().clear();
	// OBS_MODULE_USE_DEFAULT_LOCALE(.., "en-US") と同じく、既定の言語を先に読み、選んだ言語を上に重ねる。
	if (!add_file(table(), locale_dir + QStringLiteral("/en-US.ini"), error))
		return false;
	if (lang != QStringLiteral("en-US")) {
		if (!add_file(table(), locale_dir + QStringLiteral("/") + lang + QStringLiteral(".ini"), error))
			return false;
	}
	if (table().empty()) {
		if (error != nullptr)
			*error = QStringLiteral("no keys in %1").arg(locale_dir);
		return false;
	}
	return true;
}

bool load_frontend_locale(const QString &obs_source_dir, const QString &lang, QString *error)
{
	frontend_table().clear();
	// OBS の App::InitLocale と同じ順に、en-US.ini を読み、選んだ言語の ini を上に重ねる (ja-JP.ini に OK が
	// ないので、ja でも en-US の「OK」が表示される。ledger/text_ids.h の obs_keys のコメント)。ソースの
	// フォルダーの形 (frontend/data/locale) になければ、インストールしたフォルダーの形 (data/obs-studio/locale。
	// --obs-install) を読む。
	QString dir = obs_source_dir + QStringLiteral("/frontend/data/locale");
	if (!QFileInfo::exists(dir + QStringLiteral("/en-US.ini")))
		dir = obs_source_dir + QStringLiteral("/data/obs-studio/locale");
	if (!add_file(frontend_table(), dir + QStringLiteral("/en-US.ini"), error))
		return false;
	if (lang != QStringLiteral("en-US")) {
		if (!add_file(frontend_table(), dir + QStringLiteral("/") + lang + QStringLiteral(".ini"), error))
			return false;
	}
	return true;
}

void set_preview_main_window(QWidget *window)
{
	main_window_slot() = window;
}

void set_preview_theme_dark(bool dark)
{
	theme_dark_slot() = dark;
}

} // namespace cordmux::ui_preview

// libobs の obs_module_text と同じく、見つからなければ渡された文字列をそのまま返す。返す文字列は次の load_locale まで
// 有効である (ドックは受け取った直後に QString にコピーするので、それで足りる)。
extern "C" const char *obs_module_text(const char *lookup_string)
{
	if (lookup_string == nullptr)
		return lookup_string;
	const auto &map = cordmux::ui_preview::table();
	const auto found = map.find(lookup_string);
	return found == map.end() ? lookup_string : found->second.c_str();
}

// OBS の obs_frontend_get_locale_string と同じく、見つからなければ渡された文字列をそのまま返す。
extern "C" const char *obs_frontend_get_locale_string(const char *string)
{
	if (string == nullptr)
		return string;
	const auto &map = cordmux::ui_preview::frontend_table();
	const auto found = map.find(string);
	return found == map.end() ? string : found->second.c_str();
}

extern "C" void *obs_frontend_get_main_window(void)
{
	return cordmux::ui_preview::main_window_slot().data();
}

extern "C" bool obs_frontend_is_theme_dark(void)
{
	return cordmux::ui_preview::theme_dark_slot();
}
