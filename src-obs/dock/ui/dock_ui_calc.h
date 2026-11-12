// SPDX-License-Identifier: Apache-2.0

// ドックのウィジェットが使う計算のうち、Qt に依存しない部分。単体テスト (test_dock_ui_calc.cpp) から直接呼び出せる。
// コントラスト比と白か黒の選び方、メッセージのレベル 1 のパネルの枠線、テキストを省くときと折り返すときの切れ目、
// 平常の要約の候補の選び方、ダイアログの主文の折り返しと高さ、ダイアログが入力を止める範囲の判定、
// [フォルダーを開く] で開くフォルダーの選び方、音声タブのメーターの色の境目を扱う。
// テキストは UTF-16 の文字列 (Qt の QString と同じ単位) で受け取り、添字も UTF-16 の単位で数える。Qt の側
// (elide_label.cpp など) は QString::utf16() をそのまま渡し、返った添字で QString を切る。

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "pipeline/audio_meter.h"

namespace cordmux::dock::ui::calc {

// ---- 色 ----

struct Rgb {
	int r = 0;
	int g = 0;
	int b = 0;
};

// WCAG 2.x で、テキストに必要なコントラスト比 (1.4.3) と、テキスト以外に必要なコントラスト比 (1.4.11)。
inline constexpr double kTextContrastMin = 4.5;
inline constexpr double kNonTextContrastMin = 3.0;

namespace detail {

inline double channel(int v8)
{
	const double v = static_cast<double>(v8) / 255.0;
	return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

} // namespace detail

// WCAG 2.x の相対輝度。
inline double relative_luminance(Rgb c)
{
	return 0.2126 * detail::channel(c.r) + 0.7152 * detail::channel(c.g) + 0.0722 * detail::channel(c.b);
}

// コントラスト比 (1 から 21)。引数の順によらない。
inline double contrast_ratio(Rgb a, Rgb b)
{
	const double la = relative_luminance(a);
	const double lb = relative_luminance(b);
	const double hi = la > lb ? la : lb;
	const double lo = la > lb ? lb : la;
	return (hi + 0.05) / (lo + 0.05);
}

// a から b へ percent % だけ寄せた色 (percent は 0 から 100 に収める)。
inline Rgb mix_rgb(Rgb a, Rgb b, int percent)
{
	const int p = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
	const auto one = [p](int x, int y) { return (x * (100 - p) + y * p + 50) / 100; };
	return Rgb{one(a.r, b.r), one(a.g, b.g), one(a.b, b.b)};
}

// 表示タブの区切りの罫線 (大きい分類の間) の最小のコントラスト比 (背景との比)。3:1 (WCAG 2.x の非テキストのコントラスト) に
// 届かせる。中くらいの分類の間には罫線を引かない (中見出しの太字と余白で分ける)。
inline constexpr double kStrongRuleContrastMin = 3.0;

// 区切りの罫線の色。背景 ground から本文の色 text の方へ 5% ずつ寄せ、背景との比が minimum_contrast に届いた最初の色を返す
// (テーマの背景と本文の色から作るので、明るいテーマでも暗いテーマでも背景との比が保たれる)。100% 寄せても届かないときは text を返す。
inline Rgb rule_color(Rgb ground, Rgb text, double minimum_contrast)
{
	for (int percent = 5; percent <= 100; percent += 5) {
		const Rgb mixed = mix_rgb(ground, text, percent);
		if (contrast_ratio(mixed, ground) >= minimum_contrast)
			return mixed;
	}
	return text;
}

// surface の上に描くテキストとアイコンの色。白と黒のうち、surface とのコントラスト比が高い方を返し、同じなら白を返す。
// 白とのコントラスト比と黒とのコントラスト比の積は 21 なので、選んだ方の比はどの色の上でも 21 の平方根 (約 4.58)
// 以上になる。テーマの色を変えずに、テキストに必要な 4.5:1 を保てる。
inline Rgb black_or_white_on(Rgb surface)
{
	const Rgb white{255, 255, 255};
	const Rgb black{0, 0, 0};
	return contrast_ratio(white, surface) >= contrast_ratio(black, surface) ? white : black;
}

// 弱いテキストの色 muted (OBS のテーマのクラス text-muted の色) を、背景の色 ground の上で使ってよいか。
// コントラスト比が 4.5:1 に届かなければ false を返す。呼び出し元は、そのときは本文の色で描く。
inline bool muted_text_allowed(Rgb muted, Rgb ground)
{
	return contrast_ratio(muted, ground) >= kTextContrastMin;
}

// ground の上でテキストとして読める色。color が ground とのコントラスト比 4.5:1 (kTextContrastMin) に届かないときは、
// 本文の色 text の方へ 10% ずつ寄せ、届いた最初の色を返す。届いていれば color のまま返す。100% 寄せた色 (text) でも届かない
// ときは text を返す (テーマの色の組み合わせとして、それ以上読みやすくできない)。注意の色など、テーマが決めた色の
// 色合いをなるべく残しながら、読めることを守るために使う。
inline Rgb readable_on(Rgb color, Rgb ground, Rgb text)
{
	for (int percent = 0; percent <= 100; percent += 10) {
		const Rgb mixed = mix_rgb(color, text, percent);
		if (contrast_ratio(mixed, ground) >= kTextContrastMin)
			return mixed;
	}
	return text;
}

// メッセージのレベル 1 のパネルの外周の枠線の太さ (px)。setting_px が 0 以上ならその値を返す。負なら、パネルの色と
// 背景の色のコントラスト比が threshold に届かないときだけ auto_px を返し、届けば 0 を返す (DockStyle::l1_border_px)。
inline int l1_border_px(Rgb surface, Rgb ground, int setting_px, int auto_px, double threshold)
{
	if (setting_px >= 0)
		return setting_px;
	return contrast_ratio(surface, ground) < threshold ? auto_px : 0;
}

// ---- 文字の種類の判定 (UTF-16) ----

namespace detail {

// 1 つのコードポイントと、その UTF-16 の単位の数。
struct CodePoint {
	char32_t value = 0;
	int units = 1;
};

inline CodePoint code_point_at(const std::u16string &s, std::size_t i)
{
	const char16_t c = s[i];
	if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size()) {
		const char16_t d = s[i + 1];
		if (d >= 0xDC00 && d <= 0xDFFF) {
			const char32_t v = 0x10000 + ((static_cast<char32_t>(c) - 0xD800) << 10) +
					   (static_cast<char32_t>(d) - 0xDC00);
			return CodePoint{v, 2};
		}
	}
	return CodePoint{static_cast<char32_t>(c), 1};
}

// 数字 (ASCII と全角)。Unicode の数字 (正規表現の \d) のうち、ドックのテキストで使う 2 種類だけを扱う。
inline bool is_digit(char32_t c)
{
	return (c >= U'0' && c <= U'9') || (c >= 0xFF10 && c <= 0xFF19);
}

// 空白 (Unicode の White_Space の文字。正規表現の \s と同じ範囲)。
inline bool is_space(char32_t c)
{
	switch (c) {
	case U' ':
	case U'\t':
	case U'\n':
	case U'\r':
	case U'\f':
	case U'\v':
	case 0x0085:
	case 0x00A0:
	case 0x1680:
	case 0x2028:
	case 0x2029:
	case 0x202F:
	case 0x205F:
	case 0x3000:
		return true;
	default:
		return c >= 0x2000 && c <= 0x200A;
	}
}

inline bool is_ascii_letter(char32_t c)
{
	return (c >= U'A' && c <= U'Z') || (c >= U'a' && c <= U'z');
}

// 漢字 (Unicode の Han の主な範囲)。数の後の単位の語 (「分」「人」「本中」) を判定するために使う。
inline bool is_han(char32_t c)
{
	return (c >= 0x2E80 && c <= 0x2FD5) || c == 0x3005 || c == 0x3007 || (c >= 0x3021 && c <= 0x3029) ||
	       (c >= 0x3038 && c <= 0x303B) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) ||
	       (c >= 0xF900 && c <= 0xFAFF) || (c >= 0x20000 && c <= 0x3134F);
}

inline bool starts_with_ascii(const std::u16string &s, std::size_t i, const char *word)
{
	for (std::size_t k = 0; word[k] != '\0'; ++k) {
		if (i + k >= s.size() || s[i + k] != static_cast<char16_t>(word[k]))
			return false;
	}
	return true;
}

} // namespace detail

// [start, end) の範囲 (UTF-16 の添字)。
struct Span {
	int start = 0;
	int end = 0;
};

// 数・時刻・人数の語の範囲。テキストを省くときも折り返すときも、この範囲の途中では切らない。途中で省いた
// 「表示中 (3...」や、行で分かれた「(2」と「人)」は、人数や時刻を読み違えさせるため。
// 次の 2 つの正規表現が一致する範囲を、手で実装した。
//   (1) \d[\d:.,]*%?(\s?[AP]M)?(\s(?:[A-Za-z]+|\p{Han}{1,2})|\p{Han}{1,2})?
//       数字の続き (21:32、1,024、42%) と、その後の AM/PM と、単位の語 (英字の 1 語か、漢字 1 字か 2 字。
//       「1 person」「25 minutes」「12 分」「8 本中」)。漢字の単位は、空白がなくても含める (ja の ini の「%1人」
//       「%2分間」「%2個中」から作る「2人」「30分」「2分間」)。正規表現と違い、「%」の後の語は単位に含めない。
//       「21:32 に」の「に」のような仮名も含めない。
//   (2) \([^()]*\d[^()]*\)
//       数字を含む括弧の組 (「(1 人)」「(4 people)」「(+1 more)」)。
// 2 つの範囲は重なってよい (使う側はすべての範囲を見て切れ目を動かす)。
// 折り返す側 (WrapLabel の KeepNumbers) は (1) と (2) を分けて使う (number_word_spans と number_bracket_spans)。
// (2) の括弧の組を分けずに扱うのは、組が 1 行に入り、分けなくても行が増えないときだけにする。行が増えると、
// ドックのカードの高さが変わるため。省く側 (safe_cut) は両方を守る。

// (1) の範囲だけ。数と単位の語。
inline std::vector<Span> number_word_spans(const std::u16string &text)
{
	using namespace detail;
	std::vector<Span> spans;
	const std::size_t n = text.size();

	// 左から順に、重ならないように探す (QRegularExpression::globalMatch と同じ)。
	std::size_t i = 0;
	while (i < n) {
		const CodePoint first = code_point_at(text, i);
		if (!is_digit(first.value)) {
			i += static_cast<std::size_t>(first.units);
			continue;
		}
		const std::size_t start = i;
		std::size_t j = i + static_cast<std::size_t>(first.units);
		// [\d:.,]*
		while (j < n) {
			const CodePoint c = code_point_at(text, j);
			if (is_digit(c.value) || c.value == U':' || c.value == U'.' || c.value == U',')
				j += static_cast<std::size_t>(c.units);
			else
				break;
		}
		// %?。割合は「%」が単位なので、後ろの語は単位に含めない。en の "0% volume" の volume を含めると、そこで
		// 折り返せずに行が 1 つ増える。
		bool percent = false;
		if (j < n && text[j] == u'%') {
			++j;
			percent = true;
		}
		// (\s?[AP]M)?
		{
			std::size_t k = j;
			if (k < n && is_space(code_point_at(text, k).value))
				k += static_cast<std::size_t>(code_point_at(text, k).units);
			if (starts_with_ascii(text, k, "AM") || starts_with_ascii(text, k, "PM"))
				j = k + 2;
		}
		// 単位の語 (\s(?:[A-Za-z]+|\p{Han}{1,2})|\p{Han}{1,2})?
		if (percent) {
			// 割合の後ろの語は含めない (上の %? のコメント)。
		} else if (j < n && is_han(code_point_at(text, j).value)) {
			// 空白のない単位の語 (「2人」「30分」「2分間」)。
			std::size_t m = j + static_cast<std::size_t>(code_point_at(text, j).units);
			if (m < n) {
				const CodePoint c2 = code_point_at(text, m);
				if (is_han(c2.value))
					m += static_cast<std::size_t>(c2.units);
			}
			j = m;
		} else if (j < n && is_space(code_point_at(text, j).value)) {
			const std::size_t k = j + static_cast<std::size_t>(code_point_at(text, j).units);
			if (k < n) {
				const CodePoint c = code_point_at(text, k);
				if (is_ascii_letter(c.value)) {
					std::size_t m = k;
					while (m < n && is_ascii_letter(code_point_at(text, m).value))
						++m;
					j = m;
				} else if (is_han(c.value)) {
					std::size_t m = k + static_cast<std::size_t>(c.units);
					if (m < n) {
						const CodePoint c2 = code_point_at(text, m);
						if (is_han(c2.value))
							m += static_cast<std::size_t>(c2.units);
					}
					j = m;
				}
			}
		}
		spans.push_back(Span{static_cast<int>(start), static_cast<int>(j)});
		i = j;
	}
	return spans;
}

// (2) の範囲だけ。数字を含む括弧の組。
inline std::vector<Span> number_bracket_spans(const std::u16string &text)
{
	using namespace detail;
	std::vector<Span> spans;
	const std::size_t n = text.size();
	std::size_t i = 0;
	while (i < n) {
		if (text[i] != u'(') {
			++i;
			continue;
		}
		std::size_t j = i + 1;
		bool digit = false;
		while (j < n && text[j] != u'(' && text[j] != u')') {
			const CodePoint c = code_point_at(text, j);
			if (is_digit(c.value))
				digit = true;
			j += static_cast<std::size_t>(c.units);
		}
		if (j < n && text[j] == u')' && digit) {
			spans.push_back(Span{static_cast<int>(i), static_cast<int>(j + 1)});
			i = j + 1;
		} else {
			++i;
		}
	}
	return spans;
}

// 角かっこで囲んだボタンの名前 (「[Vesktopに追加]」「[Add to Vesktop]」) の範囲。ドックの文は、押すボタンや選ぶ項目の名前を
// 角かっこで囲んで書く。折り返す側 (WrapLabel の KeepNumbers) は、この範囲が 1 行に入り、折り返さなくても行が増えないときは
// 途中で折り返さない。
// 「[Vesktopに追」と「加]」のように行で分かれると、画面のボタンの名前と照らし合わせにくい。中に角かっこを含む組と、
// 閉じていない「[」は範囲にしない。省く側 (safe_cut) では使わない (省くときは、ボタンの名前の途中でも省略記号が付くので、
// 名前を読み違えない)。
inline std::vector<Span> button_name_spans(const std::u16string &text)
{
	std::vector<Span> spans;
	const std::size_t n = text.size();
	std::size_t i = 0;
	while (i < n) {
		if (text[i] != u'[') {
			++i;
			continue;
		}
		std::size_t j = i + 1;
		while (j < n && text[j] != u'[' && text[j] != u']')
			++j;
		if (j < n && text[j] == u']' && j > i + 1) {
			spans.push_back(Span{static_cast<int>(i), static_cast<int>(j + 1)});
			i = j + 1;
		} else {
			i = j;
		}
	}
	return spans;
}

// (1) と (2) の両方。省く側 (safe_cut) が使う。
inline std::vector<Span> number_spans(const std::u16string &text)
{
	std::vector<Span> spans = number_word_spans(text);
	const std::vector<Span> brackets = number_bracket_spans(text);
	spans.insert(spans.end(), brackets.begin(), brackets.end());
	return spans;
}

// 英語 (en) の短縮形の語の範囲。英字にはさまれたアポストロフィ (「'」か U+2019) を持つ語 (haven't、aren't、
// isn't、can't、won't、doesn't) で、所有格の cordmux's も同じ形として扱う。省く側 (safe_cut) は、この語の途中で
// 切らずに語ごと落とす。
// "The participant settings haven't been saved" を "The participant settings have..." と省くと、否定の n't が落ちて
// 逆の意味に読めるため。数の語の途中で省かないのと同じ種類の読み違えである。テキストを "have not been saved" に
// 変えても "have..." と切れうるので、テキストではなく省く側で修正する。英字の語をすべて語ごと落とす方法は使わない。
// Qt の 1 文字ずつの省略が en のすべてのラベルで変わり、意味が逆にならない語まで多く落とすため。
// 折り返す側では使わない。Qt の行分割は、アポストロフィの前後で折り返さないため。
inline std::vector<Span> contraction_spans(const std::u16string &text)
{
	using namespace detail;
	std::vector<Span> spans;
	const std::size_t n = text.size();
	const auto letter = [&text, n](std::size_t k) {
		return k < n && is_ascii_letter(static_cast<char32_t>(text[k]));
	};
	std::size_t i = 0;
	while (i < n) {
		if (!letter(i)) {
			++i;
			continue;
		}
		const std::size_t start = i;
		bool apostrophe = false;
		std::size_t j = i;
		while (j < n) {
			if (letter(j)) {
				++j;
			} else if ((text[j] == u'\'' || text[j] == 0x2019) && letter(j + 1)) {
				// 英字にはさまれたアポストロフィだけを語に含める (語の前後の引用符は含めない)。
				apostrophe = true;
				++j;
			} else {
				break;
			}
		}
		if (apostrophe)
			spans.push_back(Span{static_cast<int>(start), static_cast<int>(j)});
		i = j;
	}
	return spans;
}

namespace detail {

inline bool is_hiragana(char32_t c)
{
	return (c >= 0x3041 && c <= 0x3096) || (c >= 0x309D && c <= 0x309F);
}

// 片仮名 (長音の「ー」と半角の片仮名を含む)。
inline bool is_katakana(char32_t c)
{
	return (c >= 0x30A1 && c <= 0x30FF) || (c >= 0xFF66 && c <= 0xFF9F);
}

inline bool starts_with_u16(const std::u16string &s, std::size_t i, const char16_t *word)
{
	for (std::size_t k = 0; word[k] != u'\0'; ++k) {
		if (i + k >= s.size() || s[i + k] != word[k])
			return false;
	}
	return true;
}

// 述語の始まりを区切る 1 文字の助詞 (ja_negated_predicate_spans のコメント)。「で」は含めない。「追加できません」の
// 「で」を助詞とみなすと、述語の途中で区切ってしまうため。
inline bool is_boundary_particle(char16_t c)
{
	switch (c) {
	case u'を':
	case u'が':
	case u'は':
	case u'に':
	case u'へ':
	case u'と':
	case u'も':
	case u'や':
	case u'の':
		return true;
	default:
		return false;
	}
}

} // namespace detail

// ja の打ち消しの述語 (「追加できませんでした」「更新できません」「届いていない」) の範囲。省く側 (safe_cut) は述語の
// 途中で切らず、述語の前の助詞の後まで戻す (「Vesktopにcordmuxを追加できま...」を「Vesktopにcordmuxを...」にする)。
// ja は打ち消しが文の末尾に来るので、述語の途中で省くと打ち消しが落ちて、肯定に読める (「Vesktopにcordmuxが
// 追加されて...」は「追加されている」と読める)。en の短縮形の語 (contraction_spans) と同じ考えだが、ja には語の
// 切れ目を示す文字がないので、文節の切れ目 (助詞の後) まで戻す。ドックの 1 行目の見出しには、問題を先に言う短い形
// (Notice::headline_short) がある。この範囲は、短い形でも省かれるとき (フォントが大きいとき、幅が狭いとき) の
// ガードである。
// 判定の規則:
//   - 打ち消しの語尾: 「ません」(後ろの「でした」も述語に含める)、「ない」「なかった」(後ろの「です」「でした」も)。
//     語尾の後ろが文字列の終わり、平仮名でない文字 (「届いていない人」の連体形の後の漢字、記号、空白、括弧)、
//     「でした」「です」のときだけ述語の終わりとみなす (名前の中の「ないとう」の「ない」を取らないため)。
//   - 述語の始まり: 語尾から前へ、漢字・片仮名・平仮名をさかのぼり、1 文字の助詞 (を・が・は・に・へ・と・も・や・の)
//     か 2 文字の助詞 (から・まで・より) の後ろ、またはそれ以外の文字 (英字・数字・空白・記号・「、」) の後ろで止める。
//     文の先頭までさかのぼったら先頭 (0) にする (safe_cut は 0 を返し、使う側は Qt の省略のままになる)。
// 打ち消しが落ちる省略をすべて直前の助詞まで戻す方法は使わない。述語の前で切れた形 (「参加者の設...」) まで削り、
// 意味が逆にならない文字まで減らすため。逆に読めるのは、述語の途中で切れたときだけである。
// 「Vesktopは起動していますが、接続されていません」が前の節 (肯定) の中で切れる形は、この範囲では防げない
// (逆の意味にはならないが、肯定に読める)。この見出しには短い形を持たせて防ぐ。
inline std::vector<Span> ja_negated_predicate_spans(const std::u16string &text)
{
	using namespace detail;
	std::vector<Span> spans;
	const std::size_t n = text.size();
	const auto hira_at = [&text, n](std::size_t k) { return k < n && is_hiragana(static_cast<char32_t>(text[k])); };
	std::size_t i = 0;
	while (i < n) {
		// core は打ち消しの語尾の終わり、end は後ろの「でした」「です」まで含めた述語の終わり。
		std::size_t core = 0;
		if (starts_with_u16(text, i, u"ません")) {
			core = i + 3;
		} else if (starts_with_u16(text, i, u"なかった") && i > 0 && hira_at(i - 1)) {
			core = i + 4;
		} else if (starts_with_u16(text, i, u"ない") && i > 0 && hira_at(i - 1)) {
			core = i + 2;
		}
		std::size_t end = core;
		if (core != 0) {
			if (starts_with_u16(text, core, u"でした"))
				end = core + 3;
			else if (starts_with_u16(text, core, u"です"))
				end = core + 2;
		}
		// 語尾の後ろが平仮名なら、述語の終わりではない (「ないとう」「なかったら」)。
		if (core == 0 || hira_at(end)) {
			++i;
			continue;
		}
		std::size_t start = i;
		while (start > 0) {
			const std::size_t k = start - 1;
			const char16_t c = text[k];
			// 次の文字が平仮名でないか。2 文字の助詞と「か」は、次の文字が平仮名なら語の中の文字とみなす。
			const bool next_not_hira = start < n && !is_hiragana(static_cast<char32_t>(text[start]));
			if (next_not_hira && k >= 1 &&
			    (starts_with_u16(text, k - 1, u"から") || starts_with_u16(text, k - 1, u"まで") ||
			     starts_with_u16(text, k - 1, u"より")))
				break;
			if (is_boundary_particle(c))
				break;
			// 「か」は、次の文字が平仮名でないときだけ助詞とみなす (「誰の映像か確認できません」の「か」)。
			// 「分からない」の「か」と「から」は送り仮名で、次の文字が平仮名になる。「か」で止めないと、
			// 「表示を止めています (誰の映像か確認できません)」の省略が「(誰の...」まで削られる。
			if (c == u'か' && next_not_hira)
				break;
			const auto v = static_cast<char32_t>(c);
			if (!(is_hiragana(v) || is_katakana(v) || is_han(v)) && !(c >= 0xD800 && c <= 0xDFFF))
				break;
			--start;
		}
		// 述語の始まりから打ち消しの語尾までを 1 つの範囲にし、後ろの「でした」「です」を別の範囲にする。
		// 「読み込めません...」(「でした」だけが落ちた形) は打ち消しが残るので、そこまでは戻さない
		// (「でした」の途中なら「ません」の後ろへ戻す)。「ません」は「ませ」の後ろで切れることも許す。
		// UI のテキストで「ませ」で始まる語尾は打ち消しだけなので、「空いている枠がありませ...」
		// 「追加できませ...」は打ち消しと読める。許さないと「空いている枠が...」まで削られる。
		// 「ない」「なかった」は、「な」だけでは打ち消しと決まらないので許さない。
		const std::size_t protect_end = starts_with_u16(text, i, u"ません") ? core - 1 : core;
		if (start < protect_end)
			spans.push_back(Span{static_cast<int>(start), static_cast<int>(protect_end)});
		if (core < end)
			spans.push_back(Span{static_cast<int>(core), static_cast<int>(end)});
		i = end;
	}
	return spans;
}

// テキストの末尾を省くときの切れ目を修正する。cut は残す文字数 (UTF-16 の単位)。cut が数の語か en の短縮形の語
// (contraction_spans) の途中なら、その語の前まで戻す (語ごと落とす)。ja の打ち消しの述語 (ja_negated_predicate_spans)
// の途中なら、述語の前 (助詞の後) まで戻す。戻した後の末尾の空白と読点 (「、」と全角の「，」) も落とす
// (「...ますが、」の「、」と省略記号が並ばないように)。cut が文字列の長さ以上なら長さを返す。0 を返したときは、
// 語を守ると何も残らないので、呼び出し元は Qt の省略をそのまま使う。
inline int safe_cut(const std::u16string &text, int cut)
{
	if (cut <= 0)
		return 0;
	if (static_cast<std::size_t>(cut) >= text.size())
		return static_cast<int>(text.size());
	std::vector<Span> spans = number_spans(text);
	const std::vector<Span> contractions = contraction_spans(text);
	spans.insert(spans.end(), contractions.begin(), contractions.end());
	const std::vector<Span> negations = ja_negated_predicate_spans(text);
	spans.insert(spans.end(), negations.begin(), negations.end());
	bool moved = true;
	while (moved) {
		moved = false;
		for (const Span &s : spans) {
			if (s.start < cut && cut < s.end) {
				cut = s.start;
				moved = true;
			}
		}
	}
	// サロゲートペアの間で切らない。
	if (cut > 0 && static_cast<std::size_t>(cut) < text.size() && text[static_cast<std::size_t>(cut)] >= 0xDC00 &&
	    text[static_cast<std::size_t>(cut)] <= 0xDFFF)
		--cut;
	while (cut > 0) {
		const char16_t last = text[static_cast<std::size_t>(cut) - 1];
		if (!detail::is_space(static_cast<char32_t>(last)) && last != u'、' && last != u'，')
			break;
		--cut;
	}
	return cut;
}

// ---- 折り返す位置を選ぶ (WrapLabel) ----

// 行が line_start から始まり、Qt が brk (次の行の先頭) で折り返そうとするとき、brk が spans のどれかの範囲の途中なら、
// その範囲の先頭まで折り返す位置を戻す (語ごと次の行へ送る)。範囲が重なっていれば (「(2人)」の括弧と「2人」)、
// いちばん外側の範囲の先頭へ戻す。brk を含む範囲のどれかが行の先頭から始まる (戻すと行が空になるか「(」だけが残る)
// なら、その語は 1 行に入らないので、Qt の位置のまま返す。
inline int keep_numbers_break(const std::vector<Span> &spans, int line_start, int brk)
{
	const int original = brk;
	for (;;) {
		int target = brk;
		for (const Span &s : spans) {
			if (s.start < brk && brk < s.end) {
				if (s.start <= line_start)
					return original;
				if (s.start < target)
					target = s.start;
			}
		}
		if (target == brk)
			return brk;
		brk = target;
	}
}

// パスの区切り文字 (Windows の「\」と「/」)。
inline bool is_path_separator(char16_t c)
{
	return c == u'\\' || c == u'/';
}

// 場所の行を折り返す位置を選ぶ。行が line_start から始まり、幅に入るのが fit_end の手前まで (Qt が文字の間で
// 折り返すときの位置) のとき、次の行の先頭を返す。(line_start, fit_end] の中で、次の段階を順に調べ、最初に
// 見つかった段階のいちばん右の位置にする。
//   1. パスの区切り「\」「/」の直後 (フォルダーの名前を分けない)。なければ、fit_end が区切りの直前 (名前はすべて入り、
//      続く区切りだけが入らない) なら fit_end。
//   2. 空白の直後 (名前の中の語の切れ目。「2000-01-01 12-34-56」の日付と時刻の間)。fit_end が空白の直前なら、その
//      空白の直後 (行の末尾の空白は見えないため)。
//   3. 「-」「_」「.」の直後 (語の中のつなぎ)。ただし数と数の間の「.」(「1.5」) は数の語の中なので除く。
//   4. 文字の間 (fit_end)。ただし数字の並びの途中なら、並びの先頭まで戻す (並びが行の先頭から始まるなら戻さない)。
// 段階を分けるのは、右から探すときに、大きな切れ目を優先するためである。空白と「-」を同じ段階にすると「12-34-」と
// 「56」に折れて、最後の行が 2 文字になる。区切りを、空白や「-」「_」「.」と同じ段階にすると、区切りで折り返せる
// 幅でも、フォルダーの名前の途中で折れる。段階 4 で数字の並びを分けないのは、区切りもつなぎも
// ない名前で、数の途中で折れないようにするため (「abc12」と「3」にしない)。
// ドックの最小の幅を広げて名前を 1 行に入れる方法は使わない。ドックの幅は利用者が決めるもので、名前の長さは
// 保存先と日時で変わり、広げる量を決められないため。場所の行には、最後の行の文字数の規則を適用しない
// (末尾の「cordmux」1 語の行はパスの形のままにする。wrap_label.h)。
// 全角の「－」「＿」「．」は段階 3 に含めない。日本語の文字はもともと文字の間で折り返すので、段階 4 で折れる。
inline int path_break(const std::u16string &text, int line_start, int fit_end)
{
	const auto at = [&](int k) { return text[static_cast<std::size_t>(k)]; };
	const auto digit_at = [&](int k) {
		return k >= 0 && k < static_cast<int>(text.size()) && detail::is_digit(static_cast<char32_t>(at(k)));
	};
	// 段階 1: パスの区切り。
	for (int k = fit_end; k > line_start; --k) {
		if (is_path_separator(at(k - 1)))
			return k;
	}
	// 区切りの直前まで入る (名前がすべて入り、続く区切りだけが入らない) なら、その位置で折り返す (区切りは次の行の
	// 先頭になる)。段階 2 と 3 より先に調べるのは、「obs_videos_01」と「\cordmux\...」に分けられる幅で、
	// 「obs_videos_」と「01\cordmux\...」に分けないようにするため。
	if (fit_end < static_cast<int>(text.size()) && is_path_separator(at(fit_end)))
		return fit_end;
	// 段階 2: 空白。行の末尾の空白は見えないので、空白の直前まで入れば空白の後で折り返す (「2000-01-01」まで入り、
	// 空白が入らない幅で、段階 3 の「2000-01-」と「01 12-34-56」にしないため)。
	if (fit_end < static_cast<int>(text.size()) && detail::is_space(static_cast<char32_t>(at(fit_end))))
		return fit_end + 1;
	for (int k = fit_end; k > line_start; --k) {
		if (detail::is_space(static_cast<char32_t>(at(k - 1))))
			return k;
	}
	// 段階 3: 語の中のつなぎ。
	for (int k = fit_end; k > line_start; --k) {
		const char16_t c = at(k - 1);
		if (c == u'.' && digit_at(k - 2) && digit_at(k))
			continue;
		if (c == u'-' || c == u'_' || c == u'.')
			return k;
	}
	// 段階 4: 文字の間。数字の並びの途中なら、並びの先頭へ戻す。
	int brk = fit_end;
	while (brk > line_start && digit_at(brk - 1) && digit_at(brk))
		--brk;
	return brk > line_start ? brk : fit_end;
}

// ---- 長い順の候補から、幅に入るものを選ぶ ----

// 平常の要約のように、区切り (「 - 」) を含むテキストを幅に収める。区切りや文の断片をコードで連結すると、翻訳する
// 人が言語ごとに語順と区切りを決められないので、長い形と短い形をそれぞれ完全な文字列として ini に持つ
// (ledger/dock_view.h の FirstLine::forms)。長い順に並べた候補のうち、幅に入る最初のものを選ぶ。どれも入らない
// ときだけ、最後 (いちばん短い) の候補の末尾を省く。後ろの区切りから順に丸ごと落とすのと同じ見え方になる。
struct FormFit {
	int index = -1;     // 表示する候補 (-1 は候補がないときだけ)
	bool elide = false; // その候補も入らない (末尾を省く)
};

inline FormFit fit_forms(const std::vector<int> &form_widths, int available)
{
	FormFit fit;
	if (form_widths.empty())
		return fit;
	for (std::size_t k = 0; k < form_widths.size(); ++k) {
		if (form_widths[k] <= available) {
			fit.index = static_cast<int>(k);
			return fit;
		}
	}
	fit.index = static_cast<int>(form_widths.size()) - 1;
	fit.elide = true;
	return fit;
}

// どの候補も入らずに末尾を省くとき、省く候補を選ぶ (fit_forms の elide が true のときだけ使う)。qt_cuts[k] は、
// forms[k] を Qt が省いたときに残す文字数 (UTF-16 の単位。省略記号を除く。省けないときは 0)。最後の候補から前へ
// 調べ、safe_cut が 1 文字以上を残す (守る語の途中で切らずに省ける) 最初の候補の番号を返す。どれも残せなければ
// 最後の候補を返す (Qt の省略のままになる)。forms が空なら -1 を返す。
// 最後の候補だけを省くと、数で始まる短い形 (「%1人の映像が届いていません」) では、safe_cut が数の語の先頭 (0) まで
// 戻り、Qt の省略で「1...」と数と単位の間で切れた形になる。1 つ前の候補 (見出しの全文) を同じガードで省けば、
// 問題の内容が 1 行目に残る。
inline int elide_form_index(const std::vector<std::u16string> &forms, const std::vector<int> &qt_cuts)
{
	const int n = static_cast<int>(forms.size() < qt_cuts.size() ? forms.size() : qt_cuts.size());
	if (n <= 0)
		return forms.empty() ? -1 : static_cast<int>(forms.size()) - 1;
	for (int k = n - 1; k >= 0; --k) {
		const std::size_t i = static_cast<std::size_t>(k);
		if (qt_cuts[i] > 0 && safe_cut(forms[i], qt_cuts[i]) > 0)
			return k;
	}
	return static_cast<int>(forms.size()) - 1;
}

// ---- 1 行目の無効の形 ----

// 1 行目の「cordmuxは無効です」と [cordmuxを有効にする] の形。幅によらずテキストとボタンを残すので、大きいテキストと長いボタン、
// 大きいテキストと短いボタン、本文の大きさのテキストと短いボタン、の順に入るものを選ぶ。どれも入らないときは最後の形にする
// (ドックの最小幅がこの形を含むので、製品では起きない)。
struct DisabledLineFit {
	bool small_text = false;
	bool short_button = false;
};

inline DisabledLineFit disabled_line_fit(int line_px, int text_px, int button_px, int short_button_px, int gap_px)
{
	DisabledLineFit fit;
	if (text_px + gap_px + button_px <= line_px)
		return fit;
	fit.short_button = true;
	if (text_px + gap_px + short_button_px <= line_px)
		return fit;
	fit.small_text = true;
	return fit;
}

// ---- 押した場所の隣の確認 ----

// [確定] とやめる側のボタンの置き方。place は元のボタンの場所の左端と幅で、右端をそろえる。元の場所に 2 つのテキストが入る
// ときは、場所の幅を 2 つに分け、入らないときだけ左へ広げる (サブの行では、通話の名前の場所を使う)。
// やめる側のボタンは、[確定] より幅が狭くないとき (「通話を続ける」と「退出」)、必要な幅を 2 つのテキストの幅の和にし、
// やめる側を広い方 (半分以上) にする。同じ幅にそろえると、短い [確定] の分まで広げることになり、ドックの最小の幅が増える。
// やめる側が狭いときは、同じ幅で 2 つに分ける。
// やめる側のボタンはポインターの下に来る側にする。続けて押しても確定しないためである。ポインターが場所の外にあるとき
// (メニューから選んだとき、キーボードで押したとき) は右にする (Windows のボタンの並びと同じ)。
struct ConfirmLayout {
	int left = 0;
	int width = 0;
	bool cancel_on_left = false;
	int confirm_width = 0; // [確定] のボタンの幅
	int cancel_width = 0;  // やめる側のボタンの幅 (confirm_width + 間隔 + cancel_width = width)
};

// 2 つのボタンを並べるのに必要な幅 (ConfirmLayout の width の下限)。
inline int confirm_pair_width(int confirm_px, int cancel_px, int gap_px)
{
	if (cancel_px >= confirm_px)
		return confirm_px + cancel_px + gap_px;
	return 2 * confirm_px + gap_px;
}

inline ConfirmLayout confirm_layout(int place_left, int place_width, int confirm_px, int cancel_px, int gap_px,
				    bool pointer_known, int pointer_x)
{
	ConfirmLayout out;
	const int need = confirm_pair_width(confirm_px, cancel_px, gap_px);
	out.width = place_width > need ? place_width : need;
	out.left = place_left + place_width - out.width;
	const int inner = out.width - gap_px;
	if (cancel_px >= confirm_px) {
		const int half = (inner + 1) / 2;
		out.cancel_width = cancel_px > half ? cancel_px : half;
	} else {
		out.cancel_width = inner / 2;
	}
	out.confirm_width = inner - out.cancel_width;
	if (pointer_known && pointer_x >= out.left && pointer_x < out.left + out.width)
		out.cancel_on_left = pointer_x < out.left + out.width / 2;
	return out;
}

// ---- 並びの保留 ----

// 行が組の間を移る変化を後に回すか。ボタンを押している間とメニューを開いている間は保留を続ける。ポインターが一覧の上で
// 動いている間も保留し、止まって layout_hold_idle_ms がたったら moving が false になる。時間の上限を置くのは、止まった
// ポインターの下で新しい行がいつまでも出ないと、故障に見えるためである。
inline bool layout_held(bool pointer_over, bool moving, bool pressed, bool menu_open)
{
	return (pointer_over && moving) || pressed || menu_open;
}

// ---- フォントの大きさから余白を計算する ----

// em_px の em 倍を四捨五入した px。floor_px より小さくしない。
inline int px_from_em(double em_px, double em, int floor_px)
{
	const int v = static_cast<int>(std::lround(em_px * em));
	return v < floor_px ? floor_px : v;
}

// ---- ダイアログの主文を語の切れ目で 2 行に折り返す ----

// 主文 (1.5 em の見出し) が 1 行に入らないとき、どの候補の後で折り返すかを選ぶ。candidates は、折り返してよい文字
// (読点「、」など。DockStyle::dialog_main_break_after) の直後までの幅 (左から順)、total は主文全体の幅、column は
// 列の幅、min_second は 2 行目に必要な最小の幅 (最後の行の長さの下限)。1 行目 (候補までの文字) と 2 行目 (残り) が
// どちらも列に入り、2 行目が min_second 以上になる候補のうち、2 つの行の長さがいちばん近い (広い方の行がいちばん
// 短い) 候補の番号を返す。見出しは行の長さをそろえると読みやすいため (CSS の text-wrap: balance が見出しに
// 勧められるのと同じ考え)。どの候補も合わないときと、1 行に入るときは -1 を返す (Qt の折り返しのままになる)。
// 2 行目の幅は total から候補の幅を引いて見積もる (文字の詰めの差は 1 px 程度で、列の余りに収まる)。
inline int phrase_break_choice(const std::vector<int> &candidates, int total, int column, int min_second)
{
	if (column <= 0 || total <= column)
		return -1;
	int chosen = -1;
	int best_wide = 0;
	for (std::size_t k = 0; k < candidates.size(); ++k) {
		const int first = candidates[k];
		const int second = total - first;
		if (first > column || second > column || second < min_second)
			continue;
		const int wide = first > second ? first : second;
		if (chosen < 0 || wide < best_wide) {
			chosen = static_cast<int>(k);
			best_wide = wide;
		}
	}
	return chosen;
}

// ---- ダイアログの高さを画面の作業領域に収める ----
//
// 確認・結果・失敗のダイアログ (message_dialog) の高さは中身から決まり、利用者は大きさを変えられない。箇条書きの多い
// ダイアログ (手順書と違う置き方のアンインストールの結果など) は 600 px 前後の高さになり、作業領域の高さが小さい画面
// (1366x768 で拡大率 125% のとき、作業領域の高さは論理 px で約 574。1920x1080 で 175% のときは約 577) に入らず、
// 下の行とボタンが画面の外に出る。そこで、本来の高さが画面に入らないときだけ、主文とボタンの間の本文をダイアログの
// 中でスクロールする (アイコン、主文、ボタンはいつも見える)。
//
// natural_px はダイアログの中身の本来の高さ (タイトルバーと枠を除く)、body_px はそのうちスクロール領域に置き換わる
// 本文の高さ、available_px はダイアログを表示する画面の作業領域の高さ (QScreen::availableGeometry。タスクバーを除く)、
// frame_px はタイトルバーと枠の高さ (上と下の和)、min_body_px はスクロールするときに見せる本文の最小の高さ。どれも
// 論理 px (表示の倍率で割った値) で渡す。
// 戻り値の scroll が true なら、本文を戻り値の body_px の高さでスクロールする。そのときダイアログの中身の高さは
// natural_px - 引数の body_px + 戻り値の body_px になり、作業領域から枠を引いた高さに収まる (作業領域が小さすぎて
// min_body_px を下回るときだけ収まらない)。false なら本来の高さのままにする (見た目を変えない)。作業領域の高さが
// 分からない (0 以下の) ときと、スクロールする本文がないときも false になる。
// ダイアログ全体をスクロールする方法は使わない。主文とボタンが本文と一緒に流れ、[OK] が見えなくなるため。
struct DialogHeightFit {
	bool scroll = false;
	int body_px = 0; // scroll が true のときの、本文の見える高さ
};

inline DialogHeightFit dialog_height_fit(int natural_px, int body_px, int available_px, int frame_px, int min_body_px)
{
	DialogHeightFit fit;
	if (available_px <= 0 || natural_px <= 0 || body_px <= 0)
		return fit;
	const int room = available_px - (frame_px > 0 ? frame_px : 0);
	if (natural_px <= room)
		return fit;
	const int fixed = natural_px > body_px ? natural_px - body_px : 0;
	int floor_px = min_body_px > 1 ? min_body_px : 1;
	if (floor_px > body_px)
		floor_px = body_px;
	const int shown = room - fixed > floor_px ? room - fixed : floor_px;
	// スクロールしても低くならない (本文が最小の高さ以下) ならスクロールしない (見えない所ができるだけになる)。
	if (shown >= body_px)
		return fit;
	fit.scroll = true;
	fit.body_px = shown;
	return fit;
}

// ---- 操作を始めた範囲だけを止めるダイアログ ----
//
// 失敗などのダイアログを、モーダルでないウィンドウとして表示し、開いている間は、止める範囲 (設定ウィンドウなど)
// の中のウィジェットへの入力だけを捨てる。OBS のメインウィンドウとほかのドックは使えるままにする (配信中に OBS の
// 操作を止めないため)。止める範囲の見た目は変えない (setEnabled(false) にして灰色にしない)。入力を捨てたときは、
// ダイアログを前面に出す (Windows のオーナーウィンドウを持つウィンドウと同じ動作)。ただし音は鳴らさない。OBS が
// デスクトップの音声を取り込んでいると、その音が配信に入るため。
//
// Qt のイベントの種類を、Qt に依存しない形に置き換えたもの (message_dialog.cpp が置き換える)。
enum class HoldEvent {
	PointerPress, // 押す、ダブルクリックする (ウィンドウの枠の上も含む)
	PointerOther, // 離す、動かす
	Wheel,
	KeyPress,
	KeyOther,     // キーを離す、ショートカット (ShortcutOverride と Shortcut)
	ContextMenu,
	Hint,         // ツールチップと WhatsThis
	HoverIn,      // 入る、上で動く (出るイベントは通す。ホバーの見た目を戻すため)
	TouchBegin,
	TouchOther,
	Drag,         // ドラッグして入る、動かす、ドロップする
	Close,        // 閉じる (タイトルバーの [x]、Alt+F4)
	Activate,     // ウィンドウがアクティブになった
	Other,
};

enum class HoldAction {
	Pass,            // そのまま届ける
	Swallow,         // 捨てる
	SwallowAndRaise, // 捨てて、ダイアログを前面に出す
	PassAndRaise,    // 届けて、ダイアログを前面に出す (止める範囲が前面に来たら、ダイアログをその上に出す)
};

// in_owner は、イベントの届け先が止める範囲の中のウィジェットかどうか (ダイアログとその中は含まない)。spontaneous は、
// OS から来たイベント (利用者の操作) かどうか。コードからの close() (OBS の終了処理で閉じるとき) は止めない。
inline HoldAction owner_hold_action(HoldEvent event, bool in_owner, bool spontaneous)
{
	if (!in_owner)
		return HoldAction::Pass;
	switch (event) {
	case HoldEvent::PointerPress:
	case HoldEvent::KeyPress:
	case HoldEvent::ContextMenu:
	case HoldEvent::TouchBegin:
		return HoldAction::SwallowAndRaise;
	case HoldEvent::PointerOther:
	case HoldEvent::Wheel:
	case HoldEvent::KeyOther:
	case HoldEvent::Hint:
	case HoldEvent::HoverIn:
	case HoldEvent::TouchOther:
	case HoldEvent::Drag:
		return HoldAction::Swallow;
	case HoldEvent::Close:
		return spontaneous ? HoldAction::SwallowAndRaise : HoldAction::Pass;
	case HoldEvent::Activate:
		return HoldAction::PassAndRaise;
	case HoldEvent::Other:
		break;
	}
	return HoldAction::Pass;
}

// ---- [フォルダーを開く] で開くフォルダー (open_location.h の folder_to_open) ----
//
// パスは QDir::cleanPath の形の絶対パスで受け取る (区切りは '/'、`.` と `..` は解決済み、末尾の区切りはない。
// ドライブのルートだけは "C:/"、UNC は "//server/share/...")。相対パスは、呼び出し元 (Qt の側) が先に除く。
// 存在を調べる部分 (QFileInfo) と、親をたどる規則を分け、規則を単体テストで確認できるようにしている。親は文字列
// でたどる。QDir::cdUp は上のフォルダーが存在するときだけ動くので、2 階層以上が消えたパスで止まるため。

enum class PathKind { Missing, File, Folder };

// path の親のパス。ルートなら空を返す。ルートは、ドライブのルート "C:/"、"/"、UNC の共有のルート "//server/share"
// と、それより短い UNC の形である ("//server" はエクスプローラーで開けるフォルダーではないので、親にしない)。
inline std::u16string parent_path(const std::u16string &path)
{
	if (path.empty() || path == u"/")
		return std::u16string();
	if (path.size() == 3 && path[1] == u':' && path[2] == u'/')
		return std::u16string();
	if (path.size() >= 2 && path[0] == u'/' && path[1] == u'/') {
		// "//server/share" までがルート。先頭の "//" を含めて区切りが 3 つ以下なら、ルートかルートより短い。
		std::size_t seps = 0;
		for (std::size_t i = 2; i < path.size(); ++i)
			if (path[i] == u'/')
				++seps;
		if (seps <= 1)
			return std::u16string();
	}
	const std::size_t last = path.rfind(u'/');
	if (last == std::u16string::npos)
		return std::u16string();
	if (last == 0)
		return u"/";
	if (last == 2 && path[1] == u':')
		return path.substr(0, 3);
	return path.substr(0, last);
}

// 開くフォルダーを返す。path が存在するフォルダーならそのフォルダー、存在するファイルならそのファイルがある
// フォルダー (ファイルは選択しない)、存在しない (削除された、まだ作られていない) なら、存在するいちばん近い親の
// フォルダーにする。path が空のときと、ルートまでさかのぼっても存在するフォルダーがないときは空を返す。
// kind_of(パス) は PathKind を返す。
template <class KindOf> std::u16string nearest_folder(const std::u16string &path, const KindOf &kind_of)
{
	std::u16string now = path;
	while (!now.empty()) {
		switch (kind_of(now)) {
		case PathKind::Folder:
			return now;
		case PathKind::File:
			return parent_path(now);
		case PathKind::Missing:
			break;
		}
		now = parent_path(now);
	}
	return std::u16string();
}

// ---- 音声タブのメーター (level_meter.h) ----
//
// 音の大きさは 0 から 1 の位置 (pipeline/audio_meter.h の meter_position。-48 dB が 0、0 dB が 1) で受け取る。色の境目は
// 位置に変換した -20 dB (ここから黄) と -9 dB (ここから赤) で、メーターの全幅に対して固定である。値が小さいときに、黄と赤の
// 範囲を短いメーターの幅に押し込まない。

// 黄が始まる大きさと、赤が始まる大きさ (dBFS)。
inline constexpr float kMeterYellowFromDb = -20.0f;
inline constexpr float kMeterRedFromDb = -9.0f;

// 塗る範囲を、色ごとの右端の位置で表す。緑は 0 から green_end、黄は green_end から yellow_end、赤は yellow_end から red_end。
// 色が届かない範囲は、前の色の右端と同じ位置になる (幅 0)。
struct MeterSegments {
	double green_end = 0.0;
	double yellow_end = 0.0;
	double red_end = 0.0;
};

// 位置 position (範囲の外は端に収める) まで塗るときの、色ごとの右端。
inline MeterSegments meter_segments(double position)
{
	const double yellow_from = static_cast<double>(pipeline::meter_position(kMeterYellowFromDb));
	const double red_from = static_cast<double>(pipeline::meter_position(kMeterRedFromDb));
	const double p = std::isnan(position) ? 0.0 : (position < 0.0 ? 0.0 : (position > 1.0 ? 1.0 : position));
	MeterSegments s;
	s.green_end = p < yellow_from ? p : yellow_from;
	s.yellow_end = p < yellow_from ? s.green_end : (p < red_from ? p : red_from);
	s.red_end = p < red_from ? s.yellow_end : p;
	return s;
}

// 位置 position を、幅 width_px のメーターの塗りつぶす幅 (px、四捨五入) にする。メーターは、この値と灰色かどうかが変わった
// ときだけ塗り直す。
inline int meter_fill_px(double position, int width_px)
{
	if (width_px <= 0 || std::isnan(position) || position <= 0.0)
		return 0;
	if (position >= 1.0)
		return width_px;
	return static_cast<int>(std::lround(position * static_cast<double>(width_px)));
}

// 音声タブの 1 人の行で、名前の欄に割り当てる幅 (px) を決める。名前の欄とつまみの場所を合わせた幅 available を、3 段で配る。
//   1. 狭いうち: つまみは最小 (slider_min) のまま、名前の欄を name_min から min(wanted, name_cap) まで広げる。
//   2. 次に、つまみを slider_min から slider_comfort まで広げる (名前の欄は 1 の幅のまま)。
//   3. さらに余れば、名前の欄を wanted (全員の名前の全文の幅の最大) まで広げる。name_cap を超えてよい。残りはつまみが取る。
// どの行にも同じ引数を渡せば同じ幅になるので、つまみの位置がそろう。結果は name_min 以上で、available がつまみの最小を引いても
// name_min に足りないときは name_min を返す (ドックの最小幅を広げない)。
inline int name_column_px(int available, int wanted, int name_min, int name_cap, int slider_min, int slider_comfort)
{
	const int cap = std::max(name_min, name_cap);
	const int first = std::max(name_min, std::min(wanted, cap));
	const int tier1 = std::min(first, std::max(name_min, available - slider_min));
	const int full = std::max(first, wanted);
	const int tier3 = std::min(full, available - std::max(slider_comfort, slider_min));
	return std::max(tier1, tier3);
}

} // namespace cordmux::dock::ui::calc
