// SPDX-License-Identifier: Apache-2.0

// 差し込む値 (TextRef の args) を画面のテキストにする前の計算 (plan_arg、short_clock_format)。Qt に依存しない
// ので、単体テスト (test_dock_ui_calc.cpp) から直接呼び出せる。
//   - 値の種類の判定: 日時のマーカーが付いた値 (ledger/dock_view.h の time_arg、date_time_arg、
//     date_arg) か、そのまま表示する文字列か。マーカーの読み方は dockview::parse_time_arg だけを使う (2 か所に
//     持たないため)。
//   - 画面に制御文字を表示しない。マーカーが壊れていたらマーカーと種類の文字を除き、どの値からも制御文字を除く。
//   - Qt の日時の書式から秒を除き、画面の言語が日本語なら 24 時間制にする (docs/ui_guidelines.md の「表記」)。
// 文字列は UTF-8 で受け取る (TextRef の args と同じ)。

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "ledger/dock_view.h"

namespace cordmux::dock::ui::calc {

// 差し込む値の種類。
enum class ArgKind {
	Plain,    // 文字列をそのまま表示する (text は制御文字を除いた文字列)
	Time,     // 時と分 (マーカーの種類 T)
	DateTime, // 日付と時刻 (マーカーの種類 D)
};

struct ArgPlan {
	ArgKind kind = ArgKind::Plain;
	std::int64_t epoch_us = 0; // Time、DateTime のときの Unix 時刻 (マイクロ秒)
	std::string text;          // Plain のときの文字列
};

namespace detail {

// UTF-8 の文字列から、from 以降の制御文字 (C0 = U+0000 から U+001F、DEL = U+007F、C1 = U+0080 から U+009F) を除く。
// U+0080 から U+009F は、UTF-8 で 0xC2 0x80 から 0xC2 0x9F の 2 バイトになる。ほかのバイトはそのまま通す
// (壊れた並びも修正しない)。
inline std::string without_controls(const std::string &s, std::size_t from)
{
	std::string out;
	out.reserve(s.size() > from ? s.size() - from : 0);
	for (std::size_t i = from; i < s.size(); ++i) {
		const auto c = static_cast<unsigned char>(s[i]);
		if (c < 0x20 || c == 0x7F)
			continue;
		if (c == 0xC2 && i + 1 < s.size()) {
			const auto d = static_cast<unsigned char>(s[i + 1]);
			if (d >= 0x80 && d <= 0x9F) {
				++i;
				continue;
			}
		}
		out += static_cast<char>(c);
	}
	return out;
}

inline bool is_ascii_alpha(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

} // namespace detail

// 差し込む値 1 つの種類を判定する。日時のマーカーが読めれば、その種類と時刻を返す。読めなければ、文字列 (先頭の
// マーカーと種類の文字 1 つと、すべての制御文字を除いた残り) を返す。壊れたマーカーの値をそのまま表示すると、
// 制御文字 U+001F が画面に表示されるため。
inline ArgPlan plan_arg(const std::string &arg)
{
	namespace dv = ledger::dockview;
	ArgPlan plan;
	dv::TimeArgKind kind = dv::TimeArgKind::Time;
	std::int64_t us = 0;
	if (dv::parse_time_arg(arg, &kind, &us)) {
		switch (kind) {
		case dv::TimeArgKind::Time:
			plan.kind = ArgKind::Time;
			break;
		case dv::TimeArgKind::DateTime:
			plan.kind = ArgKind::DateTime;
			break;
		}
		plan.epoch_us = us;
		return plan;
	}
	std::size_t from = 0;
	if (!arg.empty() && arg[0] == dv::kTimeArgMark) {
		from = 1;
		// 種類の文字 (T、D と、今後加える文字) は英字 1 文字。数の先頭の文字 (- や数字) は除かない。
		if (from < arg.size() && detail::is_ascii_alpha(arg[from]))
			++from;
	}
	plan.text = detail::without_controls(arg, from);
	return plan;
}

// Qt の日時の書式 (QLocale::timeFormat、dateTimeFormat が返すもの) から、秒 (s) と 1/1000 秒 (z) を、前の区切りごと
// 除く。force_24h が true なら、午前と午後の表示 (AP、ap、A、a など) を前後の空白ごと除き、時 (h) を 24 時間制の
// H にする。引用符 ('...') の中の文字は変えない。書式の記号は ASCII だけなので、ASCII でない文字 (日本語の「時」
// など) はそのまま通す。
// 例: "h:mm:ss AP" -> "h:mm AP"、24 時間制なら "H:mm"。"M/d/yyyy h:mm AP" -> 24 時間制で "M/d/yyyy H:mm"。
// 正規表現 "[:.]?s+" で除く方法は使わない。引用符の中の s も除いてしまい、1/1000 秒と午前と午後の表示を扱えない
// ため。
inline std::string short_clock_format(const std::string &format, bool force_24h)
{
	std::string out;
	out.reserve(format.size());
	// 区切りとして除いてよい文字 (秒の前の ":"、"."、","、空白)。
	auto drop_trailing_separators = [&out]() {
		while (!out.empty() && (out.back() == ':' || out.back() == '.' || out.back() == ' ' || out.back() == ','))
			out.pop_back();
	};
	bool drop_leading_space = false; // 先頭の午前と午後の表示を除いた後の空白を除く
	std::size_t i = 0;
	const std::size_t n = format.size();
	while (i < n) {
		const char c = format[i];
		if (c == '\'') {
			// 引用符の中は文字のまま。'' は引用符そのもの。
			std::size_t j = i + 1;
			while (j < n) {
				if (format[j] == '\'') {
					if (j + 1 < n && format[j + 1] == '\'') {
						j += 2;
						continue;
					}
					break;
				}
				++j;
			}
			const std::size_t end = j < n ? j + 1 : n;
			out.append(format, i, end - i);
			i = end;
			drop_leading_space = false;
			continue;
		}
		std::size_t run = 1;
		while (i + run < n && format[i + run] == c)
			++run;
		if (c == 's' || c == 'z') {
			drop_trailing_separators();
			i += run;
			continue;
		}
		if (force_24h && (c == 'a' || c == 'A')) {
			// AP、ap、Ap、aP は 2 文字、A、a は 1 文字。
			std::size_t len = 1;
			if (i + 1 < n && (format[i + 1] == 'p' || format[i + 1] == 'P'))
				len = 2;
			while (!out.empty() && out.back() == ' ')
				out.pop_back();
			drop_leading_space = out.empty();
			i += len;
			continue;
		}
		if (drop_leading_space && c == ' ') {
			i += run;
			continue;
		}
		drop_leading_space = false;
		if (force_24h && c == 'h') {
			out.append(run, 'H');
			i += run;
			continue;
		}
		out.append(format, i, run);
		i += run;
	}
	// 最後に残った空白 (除いた午前と午後の表示の前にあったもの)。
	while (!out.empty() && out.back() == ' ')
		out.pop_back();
	return out;
}

} // namespace cordmux::dock::ui::calc
