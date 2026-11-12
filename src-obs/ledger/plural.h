// SPDX-License-Identifier: Apache-2.0

// 英語の形が数で変わるテキストのキーの組 (CountedId) と、数からキーを選ぶ pick()。
// キーの名前の付け方は docs/ui_guidelines.md の「ini のキーの書式」にある。libobs にも Qt にも依存しない。

#pragma once

namespace cordmux::ledger::text {

// 英語の形が数で変わるテキストのキーの組。other は 0 と 2 以上 (と負の数) のときのキー、one は数がちょうど 1 のときの
// キー (other の後ろに `_one` を付けた名前)。2 つのテキストは同じ番号の %N を同じ数だけ持つ
// (test/lint/locale_ids.py がチェックする)。
//
// 2 つのキーを文字列で持ち、`_one` をその場で連結しない。連結すると、`_one` の行がないキーでも 1 のときに
// `<キー>_one` を作れてしまい、ini にないキーがそのまま画面に表示される。表示の規則は ini を読まないので、ないキーに
// 気付いて元のキーに戻すこともできない。組の型からしか選べないようにすれば、組を持つキーだけが数で選ばれる。
struct CountedId {
	const char *other;
	const char *one;
};

// 数がちょうど 1 のときだけ one を返す。0 は英語でも複数の形 ("0 files") なので other を返す。
constexpr const char *pick(const CountedId &id, long long count)
{
	return count == 1 ? id.one : id.other;
}

} // namespace cordmux::ledger::text
