// OK 例のコード。2 つの ini のキーと過不足なく一致する。
#pragma once

namespace fixture {

// C++14 の桁区切りの ' は、文字リテラルの始まりではない。コードとして読み飛ばさないと、この行より下の
// 文字列リテラルからキーを 1 つも集められない。
inline constexpr unsigned long long kHoldNs = 2'000'000'000ULL;

inline constexpr const char *kHeadline = "cordmux.dock.summary_connected.status";
inline constexpr const char *kButton = "cordmux.dock.export_diagnostics.button";

// 元のキーだけを参照する。`<キー>_one` は、元のキーが参照されていれば参照されたとみなす。
inline constexpr const char *kCallCount = "cordmux.dock.summary_call.status";

// 元のキーと `_one` のキーの両方を持つ形 (ledger/plural.h の CountedId と同じ持ち方)。
inline constexpr const char *kRemovedOther = "cordmux.dock.past_removed.headline";
inline constexpr const char *kRemovedOne = "cordmux.dock.past_removed.headline_one";

// ja の `_one` の値が元のキーと違う例。元のキーだけを参照する。
inline constexpr const char *kUncheckedNext = "cordmux.dialog.unchecked_next.text";

// 連結してキーを作るときの、先頭部分だけの文字列リテラル。キーの形 (cordmux の後に 2 つ以上の部分) に完全には
// 一致しないので、キーとして集めない。
inline const char *contact_prefix()
{
	return "cordmux.contact.";
}

} // namespace fixture
