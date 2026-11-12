// SPDX-License-Identifier: Apache-2.0

// sources/ のソースの種類を OBS に登録する。

#pragma once

namespace cordmux::sources {

// 合成のソース (と、その子と測定のフィルタ) を obs_register_source で登録する。
// obs_module_load から 1 回だけ呼び出す (plugin-main.cpp)。
void register_source_types();

} // namespace cordmux::sources
