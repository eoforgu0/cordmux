// SPDX-License-Identifier: Apache-2.0

// obs_theme.h の実装。移植の元にした関数の名前を、関数ごとに書いておく (OBS 31.1.1 の frontend/OBSApp_Themes.cpp)。

#include "obs_theme.h"

#include <QColor>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMetaEnum>
#include <QVariant>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>

namespace cordmux::ui_preview {

namespace {

// ---- トークン (libobs の cf-lexer が変数のブロックで区切るのと同じ区切り) ----
//
// cf-lexer の規則のうち、テーマの変数のブロックに適用されるものだけを移植した。
//   - 英字か `_` で始まるトークンは、英数字と `_` が続く限り 1 つ (cf_lexer_nexttoken は `_` を英字として扱う)。
//   - 数字で始まるトークンは、空白と `.` 以外の記号が来るまで 1 つ (`12px` `0.4` `1A3278` が 1 つになる)。
//   - `'...'` と `"..."` は、引用符を含めて 1 つ。
//   - `/* */` と `//` のコメントは空白と同じ。空白は区切りにだけ使い、トークンとしては返さない (cf_next_token)。
//   - それ以外の記号は 1 文字で 1 つ。
struct Token {
	QString text;
	bool is_number = false;
	bool is_name = false;
	bool is_string = false;
};

std::vector<Token> tokenize(const QString &source)
{
	std::vector<Token> out;
	const qsizetype n = source.size();
	qsizetype i = 0;
	const auto is_alpha = [](QChar c) {
		return (c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) ||
		       c == QLatin1Char('_');
	};
	const auto is_digit = [](QChar c) { return c >= QLatin1Char('0') && c <= QLatin1Char('9'); };
	const auto is_space = [](QChar c) {
		return c == QLatin1Char(' ') || c == QLatin1Char('\t') || c == QLatin1Char('\r') || c == QLatin1Char('\n');
	};
	while (i < n) {
		const QChar c = source.at(i);
		if (is_space(c)) {
			++i;
			continue;
		}
		if (c == QLatin1Char('/') && i + 1 < n && source.at(i + 1) == QLatin1Char('*')) {
			const qsizetype end = source.indexOf(QStringLiteral("*/"), i + 2);
			i = end < 0 ? n : end + 2;
			continue;
		}
		if (c == QLatin1Char('/') && i + 1 < n && source.at(i + 1) == QLatin1Char('/')) {
			while (i < n && source.at(i) != QLatin1Char('\n'))
				++i;
			continue;
		}
		Token token;
		const qsizetype start = i;
		if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
			++i;
			while (i < n && source.at(i) != c) {
				if (source.at(i) == QLatin1Char('\\') && i + 1 < n)
					++i;
				++i;
			}
			if (i < n)
				++i;
			token.is_string = true;
		} else if (is_alpha(c)) {
			while (i < n && (is_alpha(source.at(i)) || is_digit(source.at(i))))
				++i;
			token.is_name = true;
		} else if (is_digit(c) || (c == QLatin1Char('.') && i + 1 < n && is_digit(source.at(i + 1)))) {
			while (i < n && (is_alpha(source.at(i)) || is_digit(source.at(i)) || source.at(i) == QLatin1Char('.')))
				++i;
			token.is_number = true;
		} else {
			++i;
		}
		token.text = source.mid(start, i - start);
		out.push_back(token);
	}
	return out;
}

class TokenStream {
public:
	explicit TokenStream(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

	bool valid() const { return pos_ < tokens_.size(); }
	const Token &cur() const { return tokens_[pos_]; }
	bool is(const char *text) const { return valid() && cur().text == QLatin1String(text); }
	bool next()
	{
		if (pos_ < tokens_.size())
			++pos_;
		return valid();
	}
	// cf_next_token_should_be(p, text, goto_token, nullptr) の移植。次のトークンが text なら true を返す。
	// 違えば goto_token (あれば) まで進めて false を返す。
	bool next_should_be(const char *text, const char *goto_token)
	{
		if (!next())
			return false;
		if (is(text))
			return true;
		if (goto_token != nullptr)
			go_to(goto_token);
		return false;
	}
	// cf_go_to_token の移植。現在のトークンの次から探し、text の上で止まる。
	bool go_to(const char *text)
	{
		while (next()) {
			if (is(text))
				return true;
		}
		return false;
	}

private:
	std::vector<Token> tokens_;
	std::size_t pos_ = 0;
};

// cf_literal_to_str の移植。引用符を外す (中の `\x` はそのまま)。
QString literal_to_str(const QString &text)
{
	if (text.size() >= 2 && (text.front() == QLatin1Char('"') || text.front() == QLatin1Char('\'')) &&
	    text.back() == text.front())
		return text.mid(1, text.size() - 2);
	return text;
}

// ---- OBSThemeVariable の移植 ----

enum class VarType { Color, Size, Number, String, Alias, Calc, Min, Max };

struct Var {
	VarType type = VarType::Color; // QHash::value() が返す既定値と同じ (値で初期化した列挙は 0 の Color になる)
	QString name;
	QVariant value;
	QString suffix;
	bool editable = false;
};

using VarMap = QHash<QString, Var>;

struct ThemeMeta {
	QString id;
	QString name;
	QString extends;
	QString author;
	bool dark = false;
	bool is_base = false;
	QString location;
	QStringList dependencies;
	QString parent;
};

// ParseThemeMeta
std::optional<ThemeMeta> parse_theme_meta(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return std::nullopt;
	const QString data = QString::fromUtf8(file.readAll());
	TokenStream ts(tokenize(data));
	if (!ts.valid())
		return std::nullopt;
	// `@` を探し、その次が OBSThemeMeta になるところまで進める。
	bool found = false;
	while (ts.valid()) {
		if (ts.is("@")) {
			if (!ts.next())
				return std::nullopt;
			if (ts.is("OBSThemeMeta")) {
				found = true;
				break;
			}
			continue;
		}
		ts.next();
	}
	if (!found || !ts.next() || !ts.is("{"))
		return std::nullopt;

	ThemeMeta meta;
	for (;;) {
		if (!ts.next())
			return std::nullopt;
		if (!ts.cur().is_name)
			break;
		const QString name = ts.cur().text;
		if (!ts.next_should_be(":", ";"))
			continue;
		if (!ts.next())
			return std::nullopt;
		if (!ts.cur().is_string) {
			// cf_token_is_type(.., ";") と同じく、`;` まで進めてから次のキーと値に移る。
			ts.go_to(";");
			continue;
		}
		const QString str = literal_to_str(ts.cur().text);
		if (name == QLatin1String("dark"))
			meta.dark = str == QLatin1String("true");
		else if (name == QLatin1String("extends"))
			meta.extends = str;
		else if (name == QLatin1String("author"))
			meta.author = str;
		else if (name == QLatin1String("id"))
			meta.id = str;
		else if (name == QLatin1String("name"))
			meta.name = str;
		if (!ts.go_to(";"))
			return std::nullopt;
	}

	const QFileInfo info(path);
	meta.is_base = info.suffix() == QLatin1String("obt");
	if (meta.id.isEmpty() || meta.name.isEmpty() || (!meta.is_base && meta.extends.isEmpty()))
		return std::nullopt;
	meta.location = info.absoluteFilePath();
	return meta;
}

int anonymous_counter = 0;

bool parse_var_name(TokenStream &ts, QString *value)
{
	if (!ts.next_should_be("(", ";"))
		return false;
	if (!ts.next_should_be("-", ";"))
		return false;
	if (!ts.next_should_be("-", ";"))
		return false;
	if (!ts.next())
		return false;
	*value = ts.cur().text;
	if (!ts.next_should_be(")", ";"))
		return false;
	return !value->isEmpty();
}

// ParseColor の移植。`#` の次のトークンを 16 進数として、`rgb(r, g, b)` は 10 進数として読む (strtol と同じく、
// 読めない文字が来たらそこで止める)。
long strtol_prefix(const QString &text, int base)
{
	const QByteArray bytes = text.toLatin1();
	return std::strtol(bytes.constData(), nullptr, base);
}

std::optional<QColor> parse_color(TokenStream &ts)
{
	std::uint32_t color = 0;
	if (ts.is("#")) {
		if (!ts.next())
			return std::nullopt;
		color = static_cast<std::uint32_t>(strtol_prefix(ts.cur().text, 16));
	} else if (ts.is("rgb")) {
		if (!ts.next_should_be("(", ";") || !ts.next())
			return std::nullopt;
		color |= static_cast<std::uint32_t>(strtol_prefix(ts.cur().text, 10)) << 16;
		if (!ts.next_should_be(",", ";") || !ts.next())
			return std::nullopt;
		color |= static_cast<std::uint32_t>(strtol_prefix(ts.cur().text, 10)) << 8;
		if (!ts.next_should_be(",", ";") || !ts.next())
			return std::nullopt;
		color |= static_cast<std::uint32_t>(strtol_prefix(ts.cur().text, 10));
		if (!ts.next_should_be(")", ";"))
			return std::nullopt;
	} else if (ts.is("bikeshed")) {
		// OBS は乱数の色にする。プレビューは同じ入力から同じ画像を作るので、灰色に固定する (どのテーマも
		// 使っていない)。
		color = 0x404040;
	}
	return QColor(static_cast<QRgb>(color));
}

// ParseMath
bool parse_math(TokenStream &ts, QStringList *values, std::vector<Var> *vars)
{
	if (!ts.next_should_be("(", ";"))
		return false;
	if (!ts.next())
		return false;
	while (!ts.is(")")) {
		if (ts.is(";"))
			break;
		if (ts.is("calc") || ts.is("max") || ts.is("min")) {
			Var var;
			QStringList sub;
			var.name = QStringLiteral("__unnamed_%1").arg(++anonymous_counter);
			var.type = ts.is("calc") ? VarType::Calc : (ts.is("max") ? VarType::Max : VarType::Min);
			if (!parse_math(ts, &sub, vars))
				return false;
			var.value = sub;
			values->push_back(var.name);
			vars->push_back(std::move(var));
		} else if (ts.is("var")) {
			QString value;
			if (!parse_var_name(ts, &value))
				return false;
			values->push_back(value);
		} else {
			values->push_back(ts.cur().text);
		}
		if (!ts.next())
			return false;
	}
	return !values->isEmpty();
}

// os_strtod と同じく先頭から読めるだけ読む。
double leading_double(const QString &text)
{
	const QByteArray bytes = text.toLatin1();
	return std::strtod(bytes.constData(), nullptr);
}

// ParseThemeVariables
std::vector<Var> parse_theme_variables(const QString &data)
{
	std::vector<Var> vars;
	TokenStream ts(tokenize(data));
	if (!ts.valid())
		return vars;

	bool found = false;
	while (ts.valid()) {
		if (ts.is("@")) {
			if (!ts.next())
				return vars;
			if (ts.is("OBSThemeVars")) {
				found = true;
				break;
			}
			continue;
		}
		ts.next();
	}
	if (!found || !ts.next() || !ts.is("{"))
		return vars;

	for (;;) {
		if (!ts.next())
			return vars;
		if (!ts.is("-"))
			return vars;
		if (!ts.next_should_be("-", ";"))
			continue;
		if (!ts.next())
			return vars;
		if (!ts.cur().is_name)
			break;

		const QString key = ts.cur().text;
		Var var;
		var.name = key;
		const QString os_prefix = QStringLiteral("os_win_");
		if (key.startsWith(os_prefix) && key.size() > os_prefix.size())
			var.name = key.sliced(os_prefix.size());

		if (!ts.next_should_be(":", ";"))
			continue;
		if (!ts.next())
			return vars;
		if (key.startsWith(QLatin1String("obs")))
			continue;

		if (ts.cur().is_number) {
			const QString text = ts.cur().text;
			var.value = leading_double(text);
			var.type = VarType::Number;
			for (qsizetype i = 0; i < text.size(); ++i) {
				const QChar c = text.at(i);
				if (!c.isDigit() && !c.isSpace() && c != QLatin1Char('.')) {
					var.suffix = text.sliced(i);
					var.type = VarType::Size;
					break;
				}
			}
		} else if (ts.is("rgb") || ts.is("#") || ts.is("bikeshed")) {
			const std::optional<QColor> color = parse_color(ts);
			if (!color.has_value() || !color->isValid())
				continue;
			var.value = *color;
			var.type = VarType::Color;
		} else if (ts.is("var")) {
			QString value;
			if (!parse_var_name(ts, &value))
				continue;
			var.value = value;
			var.type = VarType::Alias;
		} else if (ts.is("calc") || ts.is("max") || ts.is("min")) {
			QStringList values;
			var.type = ts.is("calc") ? VarType::Calc : (ts.is("max") ? VarType::Max : VarType::Min);
			if (!parse_math(ts, &values, &vars))
				continue;
			var.value = values;
		} else {
			var.type = VarType::String;
			var.value = literal_to_str(ts.cur().text);
		}

		if (!ts.next())
			return vars;
		if (ts.is("!") && ts.next_should_be("editable", nullptr)) {
			if (var.type != VarType::Calc && var.type != VarType::Max && var.type != VarType::Min &&
			    var.type != VarType::Alias)
				var.editable = true;
		}
		vars.push_back(var);
		if (!ts.is(";") && !ts.go_to(";"))
			return vars;
	}
	return vars;
}

// ResolveVariable
bool resolve_variable(const VarMap &vars, Var *var)
{
	if (var->type != VarType::Alias)
		return true;
	QString key = var->value.toString();
	while (vars.value(key).type == VarType::Alias) {
		key = vars.value(key).value.toString();
		if (!vars.contains(key))
			return false;
	}
	*var = vars.value(key);
	return true;
}

QString eval_math(const VarMap &vars, const Var &var, VarType type, int recursion = 0);

// ParseMathVariable
Var parse_math_variable(const VarMap &vars, const QString &value)
{
	Var var;
	const QByteArray utf8 = value.toUtf8();
	if (!utf8.isEmpty() && std::isdigit(static_cast<unsigned char>(utf8.at(0)))) {
		var.type = VarType::Number;
		var.value = std::strtod(utf8.constData(), nullptr);
		for (qsizetype i = 0; i < utf8.size(); ++i) {
			const char c = utf8.at(i);
			if (c != 0 && !std::isdigit(static_cast<unsigned char>(c)) && c != '.') {
				var.suffix = QString::fromUtf8(utf8.constData() + i, utf8.size() - i);
				var.type = VarType::Size;
				break;
			}
		}
	} else {
		var.type = VarType::Alias;
		var.value = value;
		resolve_variable(vars, &var);
		if (var.type == VarType::Calc || var.type == VarType::Max || var.type == VarType::Min) {
			const QString val = eval_math(vars, var, var.type, 1);
			var = parse_math_variable(vars, val);
		}
		if (var.type != VarType::Number && var.type != VarType::Size)
			throw std::invalid_argument("Operand not of numeric type");
	}
	return var;
}

// EvalMath
QString eval_math(const VarMap &vars, const Var &var, VarType type, int recursion)
{
	const QString invalid = QStringLiteral("'Invalid expression'");
	if (recursion >= 10)
		return invalid;
	if (type != VarType::Calc && type != VarType::Max && type != VarType::Min)
		return invalid;
	const QStringList args = var.value.toStringList();
	if (args.size() < 2)
		return invalid;
	const QString &opt = args[1];
	if (type == VarType::Calc && opt != QLatin1String("*") && opt != QLatin1String("+") && opt != QLatin1String("-") &&
	    opt != QLatin1String("/"))
		return invalid;
	if ((type == VarType::Max || type == VarType::Min) && opt != QLatin1String(","))
		return invalid;
	if (args.size() != 3)
		return invalid;

	Var val1;
	Var val2;
	try {
		val1 = parse_math_variable(vars, args[0]);
		val2 = parse_math_variable(vars, args[2]);
	} catch (...) {
		return invalid;
	}
	if (!val1.suffix.isEmpty() && !val2.suffix.isEmpty() && val1.suffix != val2.suffix)
		return invalid;

	const double d1 = val1.value.toDouble();
	const double d2 = val2.value.toDouble();
	if (!std::isfinite(d1) || !std::isfinite(d2))
		return invalid;

	double val = std::numeric_limits<double>::quiet_NaN();
	if (type == VarType::Calc) {
		if (opt == QLatin1String("+"))
			val = d1 + d2;
		else if (opt == QLatin1String("-"))
			val = d1 - d2;
		else if (opt == QLatin1String("*"))
			val = d1 * d2;
		else if (opt == QLatin1String("/"))
			val = d1 / d2;
		if (!std::isnormal(val))
			return invalid;
	} else if (type == VarType::Max) {
		val = d1 > d2 ? d1 : d2;
	} else {
		val = d1 < d2 ? d1 : d2;
	}

	const bool is_integer = std::ceil(static_cast<long double>(val)) == val;
	QString result = QString::number(val, 'f', is_integer ? 0 : -1);
	if (!val1.suffix.isEmpty())
		result += val1.suffix;
	else if (!val2.suffix.isEmpty())
		result += val2.suffix;
	return result;
}

// PrepareQSS の 1 つの変数の分 (置き換える文字列)。解決できなければ nullopt。
std::optional<QString> var_replacement(const VarMap &vars, const Var &var_in)
{
	Var var = var_in;
	if (!resolve_variable(vars, &var))
		return std::nullopt;
	switch (var.type) {
	case VarType::Color:
		return var.value.value<QColor>().name(QColor::HexRgb);
	case VarType::Calc:
	case VarType::Max:
	case VarType::Min:
		return eval_math(vars, var, var.type);
	case VarType::Size:
	case VarType::Number: {
		const double val = var.value.toDouble();
		const bool is_integer = std::ceil(static_cast<long double>(val)) == val;
		QString out = QString::number(val, 'f', is_integer ? 0 : -1);
		if (!var.suffix.isEmpty())
			out += var.suffix;
		return out;
	}
	case VarType::String:
	case VarType::Alias:
		break;
	}
	return var.value.toString();
}

// FindEndOfOBSMetadata
qsizetype find_end_of_obs_metadata(const QString &content)
{
	qsizetype end = 0;
	for (const char *section : {"OBSThemeMeta", "OBSThemeVars", "OBSTheme"}) {
		const qsizetype idx = content.indexOf(QLatin1String(section), 0);
		if (idx > end)
			end = content.indexOf(QLatin1Char('}'), idx) + 1;
	}
	return end;
}

// PreparePalette
QPalette prepare_palette(const VarMap &vars, const QPalette &default_palette)
{
	QHash<QString, QPalette::ColorRole> role_map;
	QHash<QString, QPalette::ColorGroup> group_map;
	{
		const QMetaEnum roles = QMetaEnum::fromType<QPalette::ColorRole>();
		for (int i = 0; i < roles.keyCount(); ++i)
			role_map[QString::fromLatin1(roles.key(i)).toLower()] =
				static_cast<QPalette::ColorRole>(roles.value(i));
		const QMetaEnum groups = QMetaEnum::fromType<QPalette::ColorGroup>();
		for (int i = 0; i < groups.keyCount(); ++i)
			group_map[QString::fromLatin1(groups.key(i)).toLower()] =
				static_cast<QPalette::ColorGroup>(groups.value(i));
	}

	// 適用する順。OBS は vars (QHash) の並びのまま適用する。Qt 6 の QHash の並びは起動ごとに変わる (シードを
	// 毎回乱数で選ぶ) ので、同じ役割に palette_<役割> と palette_<役割>_<カラーグループ> の両方があると (Yami の
	// palette_text と palette_text_disabled、palette_text_inactive)、どちらが勝つかが OBS の起動ごとに変わる。
	// プレビューは同じ入力から同じ画像を作るので、役割全体、カラーグループごとの順に決めて適用する (カラーグループ
	// ごとの値が勝つ。テーマを書いた人の意図に沿う側で、OBS の起動で起こりうる結果の 1 つ)。現在の場面の画像には、
	// この違いは動作しない (違いが出るのは --dump-theme の .palette.txt だけ)。
	QStringList names = vars.keys();
	std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
		const qsizetype ua = a.count(QLatin1Char('_'));
		const qsizetype ub = b.count(QLatin1Char('_'));
		return ua != ub ? ua < ub : a < b;
	});
	QPalette pal(default_palette);
	for (const QString &name : names) {
		const Var var_in = vars.value(name);
		if (!var_in.name.startsWith(QLatin1String("palette_")))
			continue;
		const qsizetype underscores = var_in.name.count(QLatin1Char('_'));
		if (underscores < 1 || underscores > 2)
			continue;
		Var var = var_in;
		if (!resolve_variable(vars, &var) || var.type != VarType::Color)
			continue;
		QPalette::ColorRole role = QPalette::NoRole;
		QPalette::ColorGroup group = QPalette::All;
		const QStringList parts = var_in.name.split(QLatin1Char('_'));
		if (parts.size() >= 2) {
			const QString key = parts[1].toLower();
			if (!role_map.contains(key))
				continue;
			role = role_map[key];
		}
		if (parts.size() == 3) {
			const QString key = parts[2].toLower();
			if (!group_map.contains(key))
				continue;
			group = group_map[key];
		}
		const QColor color(var.value.value<QColor>().name(QColor::HexRgb));
		pal.setColor(group, role, color);
	}
	return pal;
}

QString type_name(VarType type)
{
	switch (type) {
	case VarType::Color:
		return QStringLiteral("color");
	case VarType::Size:
		return QStringLiteral("size");
	case VarType::Number:
		return QStringLiteral("number");
	case VarType::String:
		return QStringLiteral("string");
	case VarType::Alias:
		return QStringLiteral("alias");
	case VarType::Calc:
		return QStringLiteral("calc");
	case VarType::Min:
		return QStringLiteral("min");
	case VarType::Max:
		return QStringLiteral("max");
	}
	return QString();
}

} // namespace

double padding_for_density(int density)
{
	// getPaddingForDensityId
	double padding = 4;
	if (density == -2)
		padding = 0.25;
	else if (density == -3)
		padding = 2;
	else if (density == -4)
		padding = 4;
	else if (density == -5)
		padding = 6;
	return padding;
}

bool compose_obs_theme(const QString &themes_dir, const QString &theme_id, const Appearance &appearance,
		       const QPalette &default_palette, ComposedTheme *out, QString *error)
{
	// FindThemes の移植。フォルダーの .obt と .ovt をすべて読み (.oha はハイコントラストのときだけ使うので
	// 読まない)、継承の順を作る。
	std::map<QString, ThemeMeta> themes;
	QDirIterator it(themes_dir, QStringList{QStringLiteral("*.obt"), QStringLiteral("*.ovt")}, QDir::Files);
	while (it.hasNext()) {
		std::optional<ThemeMeta> meta = parse_theme_meta(it.next());
		if (meta.has_value() && themes.find(meta->id) == themes.end())
			themes[meta->id] = *meta;
	}
	const auto found = themes.find(theme_id);
	if (found == themes.end()) {
		if (error != nullptr)
			*error = QStringLiteral("theme %1 not found in %2").arg(theme_id, themes_dir);
		return false;
	}
	ThemeMeta &theme = found->second;
	QString parent_id = theme.extends;
	while (!parent_id.isEmpty()) {
		const auto parent = themes.find(parent_id);
		if (parent == themes.end() || parent->second.id == theme.id ||
		    theme.dependencies.contains(parent->second.id)) {
			if (error != nullptr)
				*error = QStringLiteral("theme %1 has a broken ancestor %2").arg(theme.id, parent_id);
			return false;
		}
		theme.dependencies.push_front(parent->second.id);
		parent_id = parent->second.extends;
	}

	// SetTheme
	VarMap vars;
	QStringList theme_ids = theme.dependencies;
	theme_ids << theme.id;

	Var font_scale;
	font_scale.name = QStringLiteral("obsFontScale");
	font_scale.type = VarType::Number;
	font_scale.value = QVariant::fromValue(appearance.font_scale);
	Var padding;
	padding.name = QStringLiteral("obsPadding");
	padding.type = VarType::Number;
	padding.value = QVariant::fromValue(padding_for_density(appearance.density));
	vars[font_scale.name] = font_scale;
	vars[padding.name] = padding;

	QStringList contents;
	out->files.clear();
	for (const QString &id : theme_ids) {
		const ThemeMeta &cur = themes[id];
		QFile file(cur.location);
		if (!file.open(QIODevice::ReadOnly)) {
			if (error != nullptr)
				*error = QStringLiteral("cannot open %1").arg(cur.location);
			return false;
		}
		const QString content = QString::fromUtf8(file.readAll());
		for (Var &var : parse_theme_variables(content))
			vars[var.name] = var;
		contents.push_back(content);
		out->files << cur.location;
	}

	// PrepareQSS
	QString stylesheet;
	for (const QString &content : contents) {
		const qsizetype offset = find_end_of_obs_metadata(content);
		if (offset >= 0) {
			stylesheet += QStringLiteral("\n");
			stylesheet += content.sliced(offset);
		}
	}
	out->resolved.clear();
	for (auto var_it = vars.cbegin(); var_it != vars.cend(); ++var_it) {
		const std::optional<QString> replace = var_replacement(vars, var_it.value());
		if (!replace.has_value())
			continue;
		stylesheet.replace(QStringLiteral("var(--%1)").arg(var_it.value().name), *replace);
		if (!var_it.value().name.startsWith(QLatin1String("__unnamed_")))
			out->resolved.emplace_back(var_it.value().name,
						   *replace + QStringLiteral("    (") + type_name(var_it.value().type) +
							   QStringLiteral(")"));
	}
	std::sort(out->resolved.begin(), out->resolved.end());

	out->id = theme.id;
	out->name = theme.name;
	out->dark = theme.dark;
	out->stylesheet = stylesheet;
	out->palette = prepare_palette(vars, default_palette);
	return true;
}

} // namespace cordmux::ui_preview
