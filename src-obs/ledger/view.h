// SPDX-License-Identifier: Apache-2.0

// 画面に表示するテキストの参照 (TextRef) と、名前や値の表示 (Label)。ドックの View (ledger/dock_view.h) と
// ダイアログが使う。ini のキーを引いて args を埋めるのは表示する側 (dock/ui/text_resolve.h) で、ini にないキーは
// libobs の obs_module_text と同じくキーがそのまま表示される。dock_view.h と分けてあるので、テキストを引く側は
// Observation と操作の型を読み込まずにこのヘッダーだけを使える。libobs にも Qt にも依存しない。

#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cordmux::ledger {

// ini のテキスト 1 つ。id は ini のキー (ledger/text_ids.h)、args は `%1` から順に埋める値。
// args の値は訳されずにそのまま埋まる。表示の規則が入れるのは、数、日時 (ledger/dock_view.h の time_arg など)、
// 人の名前と userId、OBS のソースとシーンコレクションの名前、ファイル名とフォルダーの場所で、どれも訳さない値である。
struct TextRef {
	std::string id;
	std::vector<std::string> args;
};

// 画面に表示する名前か値 1 つ。ini のテキスト (text) か、訳さない文字列 (literal) のどちらかを持つ。literal に
// 入れるのは、Discord の表示名 (読めないときは userId)、cordmux での名前、バージョン、シーンコレクションの名前
// などで、どれも名前やバージョンそのものなので訳さない。
struct Label {
	std::optional<TextRef> text;
	std::string literal;

	static Label from_text(TextRef t)
	{
		Label l;
		l.text = std::move(t);
		return l;
	}

	static Label from_literal(std::string s)
	{
		Label l;
		l.literal = std::move(s);
		return l;
	}

	bool is_text() const { return text.has_value(); }
};

// 等値比較。ドックは View を前の値と比べ、変わったウィジェットだけを書き換える。
inline bool operator==(const TextRef &a, const TextRef &b)
{
	return a.id == b.id && a.args == b.args;
}

inline bool operator!=(const TextRef &a, const TextRef &b)
{
	return !(a == b);
}

inline bool operator==(const Label &a, const Label &b)
{
	return a.text == b.text && a.literal == b.literal;
}

inline bool operator!=(const Label &a, const Label &b)
{
	return !(a == b);
}

} // namespace cordmux::ledger
