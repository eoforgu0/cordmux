// SPDX-License-Identifier: Apache-2.0

// sources/ 全体で使う名前 (ソースの種類の ID、枠の番号) と、ダミー表示の名前を作る関数。
// 名前は ini の文字列から作り、コードで区切りの文字を追加しない。
// fill_template は libobs に依存しないので、テストから直接呼び出せる。

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace cordmux::sources {

inline constexpr int kScreenSlotCount = 4;

// 枠に載せていない人を全画面で映すときの子の番号と、子の数 (config/allocation.h の kFocusOnlySlot と kScreenTileCount と同じ値。
// 同じ値であることは tests/test_sources_composite_focus.cpp で確認する)。
// 画面の枠の子 (1 から kScreenSlotCount) の後ろに 1 つ置き、全画面のときだけ描く。
inline constexpr int kFocusOnlySlot = kScreenSlotCount + 1;
inline constexpr int kScreenTileCount = kScreenSlotCount + 1;

// 自分の枠を指す枠の番号。画面共有の枠の番号は 1 から始まるので、0 は使われていない (自分の枠には番号がない)。
inline constexpr int kSelfSlot = 0;

// ソースの種類の ID (obs_register_source の id) と kSlotSettingKey は利用者には見えない識別子で、変えない。
// 保存済みのシーンコレクションと、アンインストールで削除するものの一覧 (install/removal_table.h) が、この値で
// cordmux のソースを探すためである。
// 合成のソース (利用者がシーンに置く 1 つのソース)。その中で描く、画面の枠ごとの非公開の子 (kTileSourceTypeId)
// と、子が表示に選んだフレームを数えるフィルタ (kTileProbeFilterTypeId) は、保存されない非公開のソースである。
inline constexpr const char *kCompositeSourceTypeId = "cordmux_composite";
inline constexpr const char *kTileSourceTypeId = "cordmux_tile";
inline constexpr const char *kTileProbeFilterTypeId = "cordmux_tile_probe";

// 合成のソースの中で描く子と、子の測定のフィルタの名前 (非公開で、利用者に見えない)。製品名そのままにする。合成のソースの
// 種類の名前は ini (cordmux.source.composite.label) にある。
inline constexpr const char *kCompositeDisplayName = "cordmux";

// 枠の番号を持つ obs_data のキー。子のソースは create のときに settings から自分の枠の番号を読む。
inline constexpr const char *kSlotSettingKey = "slot";

// ---- 純粋関数 (libobs に依存しない) ----

// tmpl の中の %1 から %9 を args の値に、1 回の走査で置き換える。人の名前に "%2" が含まれていても、
// 置き換えた後の文字列をもう一度置き換えない。args にない番号の %n と、後ろが 1 から 9 でない % は
// そのまま残す (ini の値が崩れていてもクラッシュしない)。
inline std::string fill_template(std::string_view tmpl, const std::vector<std::string> &args)
{
	std::string out;
	out.reserve(tmpl.size() + 16);
	for (std::size_t i = 0; i < tmpl.size(); ++i) {
		const char c = tmpl[i];
		if (c == '%' && i + 1 < tmpl.size() && tmpl[i + 1] >= '1' && tmpl[i + 1] <= '9') {
			const std::size_t index = std::size_t(tmpl[i + 1] - '1');
			if (index < args.size()) {
				out += args[index];
				++i;
				continue;
			}
		}
		out += c;
	}
	return out;
}

// ---- 名前 (OBS の現在の言語の文字列を読むので libobs が必要。scene_names.cpp) ----

// ダミーの名前ラベルに描画する名前のテンプレート (cordmux.source.placeholder.label。%1 が枠の番号)。
std::string dummy_label_format();
// dummy_label_format() の %1 を slot にした名前。
std::string dummy_display_name(int slot);

} // namespace cordmux::sources
