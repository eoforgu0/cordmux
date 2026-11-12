// NG 例: コードの文字列リテラルのキーが ini にない。
#pragma once

namespace fixture {

inline constexpr const char *kHeadline = "cordmux.dock.summary_connected.status";
inline constexpr const char *kButton = "cordmux.dock.export_diagnostics.button";
// このキーは、2 つの ini のどちらにもない。
inline constexpr const char *kExtra = "cordmux.dock.extra_missing_from_ini.status";

} // namespace fixture
