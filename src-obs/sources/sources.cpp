// SPDX-License-Identifier: Apache-2.0

// sources.h の register_source_types の実装。

#include "sources/sources.h"

#include "sources/composite_source.h"

namespace cordmux::sources {

void register_source_types()
{
	// obs_register_source (実体は obs_register_source_s) は struct の中身を libobs の中の一覧にコピーするので、
	// ローカル変数を渡した後に破棄してよい。
	// 合成のソースと、その中で描く子、子の表示の測定のフィルタ。子と測定のフィルタは「ソースを追加」の一覧に
	// 出ない非公開の種類 (composite_source.h)。
	obs_source_info composite_info = make_composite_source_info();
	obs_register_source(&composite_info);

	obs_source_info tile_info = make_tile_source_info();
	obs_register_source(&tile_info);

	obs_source_info tile_probe_info = make_tile_probe_filter_info();
	obs_register_source(&tile_probe_info);
}

} // namespace cordmux::sources
