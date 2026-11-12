// SPDX-License-Identifier: Apache-2.0

// 合成のソース cordmux が、全部のタイルの映像の上に、名前ラベルと話している人の光と、画面共有していない参加者の並びを
// 置く (CompositeDecor)。合成のソース 1 つにつき 1 つ持つ。何をどの順にどこへ置くかは sources/composite_draw_plan.h の
// 計画が決め、ここでは計画のとおりに置くだけである (CPU で同じ計画を重ねる sources/composite_draw_cpu.h と同じ値になる)。
// 描き直す鍵などの判断は sources/composite_label_plan.h、並びの位置は sources/composite_roster.h の純粋関数にあり、
// 画像の描画は sources/composite_label.h にある。
//
// 画像は CPU (QPainter) で描き、テクスチャにして GPU で置く。描き直しは重いので、内容・見た目・大きさが変わったときだけ
// 描き直す。描き直しとテクスチャを作るのは、CompositeDecor ごとに 1 つの描き直しのスレッド (composite_paint_handoff.h)
// で、描画のスレッドは仕事を渡し、できたテクスチャを受け取って置くだけである (理由は composite_decor.cpp の make_image)。
//
// 呼び出しは、stop_painter と take_paint_stats を除き、グラフィックスのコンテキストの中 (video_render と、release を
// 呼ぶ前に obs_enter_graphics した後) で行う。内部のロックは、グラフィックスのコンテキストの内側で取る 1 つだけで、
// 持ったままほかの cordmux のロックを取らない (描き直しのスレッドとの受け渡しのロックだけは、持ったまま取る。そちらは
// 葉のロックで、さらにロックを取らない)。表のロック (read_composite_decor と read_composite_roster) は、内部のロックを
// 放してから取る。描き直しのスレッドは、受け渡しのロックを持たずにグラフィックスのコンテキストに入る。

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <obs-module.h>

#include "sources/composite_layout.h"
#include "sources/composite_paint_handoff.h"
#include "sources/composite_roster.h"

namespace cordmux::sources {

// 10 秒ごとの Stats の行に出す、この周期の値。
//   paint          描き直しのスレッドの仕事の数。max_paint_ns は QPainter で描いた時間の最大 (テクスチャを作る時間と、
//                  グラフィックスのコンテキストを待つ時間は含まない)
//   uploads        描画のスレッドが受け取って置き換えた仕事の数 (帯の数ではない)
//   max_upload_ns  テクスチャを作るために、描き直しのスレッドがグラフィックスのコンテキストに 1 回 (帯 1 つ) 入っていた
//                  時間の最大。入るのは 1 フレームに 1 回までなので、描画のスレッドを 1 フレームに待たせうる時間の上限
//   max_swap_ns    描画のスレッドが 1 つの仕事のテクスチャを受け取り、前のテクスチャを破棄するのにかかった時間の最大
struct CompositeDecorPaintStats {
	PaintHandoffCounts paint;
	std::uint64_t uploads = 0;
	std::int64_t max_upload_ns = 0;
	std::int64_t max_swap_ns = 0;
};

class CompositeDecor {
public:
	CompositeDecor();
	~CompositeDecor();
	CompositeDecor(const CompositeDecor &) = delete;
	CompositeDecor &operator=(const CompositeDecor &) = delete;

	// 1 回の描画で、全部のタイルの映像を描いた後に 1 回呼ぶ。表から値を読み、描き直しの仕事を渡し、できたテクスチャを
	// 受け取ってから、計画のとおりに置く。tiles は plan_composite_layout が決めたタイル全部、has_video と hears_voice は
	// tiles と同じ並びで、そのタイルに映像を描いたか (描いていないタイルには何も置かない) と、cordmux がその人の声を
	// 聞けるか (composite_input.h の composite_tile_hears_voice)。tile_user_ids も同じ並びで、タイルに描く人
	// (composite_tile_user_ids)。roster はドックが渡した、通話の参加者のうち名前ラベルを出す人で、並びは問わない。その人のうち、
	// 名前ラベル付きで描くタイルがない人を、画面共有していない参加者の並びに描く (composite_label_plan.h の
	// composite_tiles_labeled と composite_roster_shows)。
	// muted と volume は OBS のミキサーの行のミュートと音量 (obs_source_muted と obs_source_get_volume の値。呼び出し元が
	// 読む)。
	void draw(int canvas_w, int canvas_h, const std::vector<CompositeTile> &tiles, const std::vector<bool> &has_video,
		  const std::vector<bool> &hears_voice, const std::vector<std::string> &tile_user_ids,
		  const std::vector<CompositeRosterPerson> &roster, bool muted, float volume);

	// 枠の背景 (映像を描かない所の色) を、キャンバス全体に 1 枚の矩形で置く。1 回の描画で、タイルの映像を描く前に 1 回呼ぶ
	// (ほかの何よりも下に敷くため)。値は表から読む (read_composite_background)。表示を切っているときと不透明度が 0 の
	// ときは、何も置かない。色は、名前ラベルの背景の画像と同じ効果と混ぜ方で置くので、HDR の出力でも名前ラベルの背景と同じ
	// 見え方になる。
	void draw_background(int canvas_w, int canvas_h);

	// テクスチャをすべて破棄する (描き終えて受け取っていないものも)。呼び出し元が stop_painter で描き直しのスレッドを
	// 止めてから、obs_enter_graphics の中で呼ぶこと。
	void release();

	// 描き直しのスレッドを止めて待つ。待っている仕事は捨て、描いている最中の仕事は描き終えるまで待つ (1 つの描画なので、
	// 既定の見た目なら長くても 20 ms ほど、ぼかしと広がりが範囲の上の端なら 0.5 秒ほど。composite_label.h の
	// paint_composite_label の時間の目安)。テクスチャを帯ごとに作っている最中なら、帯の間の待ちをやめ、残りの帯は作らない。
	// obs_enter_graphics の外から呼ぶこと。描き直しのスレッドはテクスチャを作るときにグラフィックスのコンテキストに入るので、
	// 中から呼ぶと、止める側が待ち、描き直しのスレッドがコンテキストを待って、どちらも進まない。何度呼んでもよい。
	void stop_painter();

	// この周期の値を読んで 0 に戻す。どのスレッドから呼んでもよい。
	CompositeDecorPaintStats take_paint_stats();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
	mutable std::mutex mu_;
};

// 明るさを掛けて置く効果 (OBS の効果の文字列から作る) を破棄する。obs_module_unload から呼ぶ (何度呼んでもよい。
// 中で obs_enter_graphics する)。
void release_composite_decor_shared();

} // namespace cordmux::sources
