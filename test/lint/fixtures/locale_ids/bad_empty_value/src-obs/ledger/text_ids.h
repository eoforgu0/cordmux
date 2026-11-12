// NG 例のコード。誤り (値が空のキー) は ini の側にだけある。

#pragma once

namespace fixture {

inline constexpr const char *kHeadline = "cordmux.dock.summary_connected.status";
inline constexpr const char *kButton = "cordmux.dock.export_diagnostics.button";

// 連結してキーを作るときの、先頭部分だけの文字列リテラル。キーの形 (cordmux の後に 2 つ以上の部分) に完全には
// 一致しないので、キーとして集めない。
inline const char *contact_prefix()
{
	return "cordmux.contact.";
}

} // namespace fixture
