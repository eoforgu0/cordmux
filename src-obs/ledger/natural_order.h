// SPDX-License-Identifier: Apache-2.0

// 名前を自然順で比べる natural_less。文字列の中の数字の並びを数として比べ、「たろう2」を「たろう10」より前に
// 並べる (Windows のエクスプローラーと同じ考え方)。ドックの参加者の一覧、使う声のメニュー、
// 参加者の設定ウィンドウの並びに使う。libobs にも Qt にも OS にも依存しない。

#pragma once

#include <cstddef>
#include <string>

namespace cordmux::ledger {

namespace natural_detail {

inline bool is_digit(char c)
{
	return c >= '0' && c <= '9';
}

inline unsigned char fold(char c)
{
	const unsigned char u = static_cast<unsigned char>(c);
	if (u >= 'A' && u <= 'Z')
		return static_cast<unsigned char>(u - 'A' + 'a');
	return u;
}

// a と b を単位ごとに比べ、a が前なら負、同じなら 0、後なら正を返す。英字の大文字と小文字の違いと、数字の並びの
// 先頭の 0 の違いでは決めない (最後に natural_less が元の文字列で決める)。
inline int compare_units(const std::string &a, const std::string &b)
{
	std::size_t i = 0;
	std::size_t j = 0;
	while (i < a.size() && j < b.size()) {
		if (is_digit(a[i]) && is_digit(b[j])) {
			std::size_t ie = i;
			while (ie < a.size() && is_digit(a[ie]))
				++ie;
			std::size_t je = j;
			while (je < b.size() && is_digit(b[je]))
				++je;
			std::size_t is = i;
			while (is + 1 < ie && a[is] == '0')
				++is;
			std::size_t js = j;
			while (js + 1 < je && b[js] == '0')
				++js;
			const std::size_t alen = ie - is;
			const std::size_t blen = je - js;
			if (alen != blen)
				return alen < blen ? -1 : 1;
			for (std::size_t k = 0; k < alen; ++k) {
				if (a[is + k] != b[js + k])
					return a[is + k] < b[js + k] ? -1 : 1;
			}
			i = ie;
			j = je;
			continue;
		}
		const unsigned char ca = fold(a[i]);
		const unsigned char cb = fold(b[j]);
		if (ca != cb)
			return ca < cb ? -1 : 1;
		++i;
		++j;
	}
	if (i < a.size())
		return 1;
	if (j < b.size())
		return -1;
	return 0;
}

} // namespace natural_detail

// a が自然順で b より前なら true を返す。厳密な弱順序なので std::sort にそのまま渡せる。
// 比べ方 (上から順に、決まるまで):
//   (1) 文字列を「数字の並び」と「それ以外の 1 バイト」の単位に分け、前から 1 単位ずつ比べる。
//   (2) 両方が数字の並びなら、先頭の 0 を除いた桁数で比べ、桁数が同じなら数字の並びで比べる (数の大小と同じ順)。
//       値が同じで先頭の 0 の数だけが違うときは、ここでは決めずに次の単位へ進む。
//   (3) それ以外の単位は、ASCII の英字の大文字と小文字を同じとみなし、UTF-8 のバイトの順で比べる。
//   (4) すべての単位が同じなら、元の文字列のバイトの順で比べる。違う文字列を同じとみなさないので、並びが実行ごとに
//       変わらない。
// 数とみなすのは ASCII の 0-9 だけで、全角の数字はほかの文字と同じに比べる。
// 日本語の名前は UTF-8 のバイトの順 (Unicode のコードポイントの順) に並ぶので、ひらがな、カタカナ、漢字の順に
// まとまる。かなと漢字を読みの順には並べない。読みは文字から決まらない (同じ漢字に読みが複数ある)。
// Qt の QCollator は使わない。表示の規則は Qt に依存せず、ドックの側で並べ直すと並びを決める所が 2 か所になる。
// そのため、QCollator の日本語の照合順とは並びが違うことがある。OS の StrCmpLogicalW も、テストを OS に依存させない
// ために使わない。
inline bool natural_less(const std::string &a, const std::string &b)
{
	const int c = natural_detail::compare_units(a, b);
	if (c != 0)
		return c < 0;
	return a < b;
}

} // namespace cordmux::ledger
