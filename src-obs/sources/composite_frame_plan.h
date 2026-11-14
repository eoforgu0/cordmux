// SPDX-License-Identifier: Apache-2.0

// 合成のソースが 1 フレームに描くもの (配置、タイルごとの映像の矩形、映像を描くか) を決める純粋関数と、その結果を動画の
// フレームごとに 1 回だけ作って覚えておく入れ物、合成のソースが実際に 1 人の映像だけを全面に描いたかの判定と、フィルタ
// cordmux_solo_filter の ON と OFF を番組の描画の中と外で決める印。描くのは sources/composite_source.cpp。libobs にも Qt にも
// 依存しない。

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "config/allocation.h"
#include "sources/composite_input.h"
#include "sources/composite_layout.h"

namespace cordmux::sources {

// 映像の大きさ (ピクセル)。どちらかが 0 以下なら、映像がない (最初のフレームが届くまでの子と、空けた後の子、自分の枠の
// 映像を選んでいないとき)。
struct ContentSize {
	int w = 0;
	int h = 0;
};

// 1 フレーム分の、配置の入力のほかに読む値。どれも、そのフレームで最初に読んだときの値である。
struct CompositeFrameFacts {
	// キャンバスの大きさ (OBS の映像の基本の解像度)。
	int canvas_w = 0;
	int canvas_h = 0;
	// 子 (枠 1 から config::kScreenTileCount) の映像の大きさ。添字は枠の番号 - 1。子がなければ 0。
	std::array<ContentSize, config::kScreenTileCount> tile_sizes{};
	// 自分の枠に映す映像のソースの大きさ。選んでいないか、ソースが消えたときは 0。
	ContentSize self_size;
};

// 1 フレームに描くもの。sizes、fits、has_video は layout.tiles と同じ並び。
struct CompositeFramePlan {
	CompositeLayout layout;
	// タイルの映像の大きさ (縦横比と拡大の倍率の元) と、映像を描く矩形 (fit_inside の結果)。
	std::vector<ContentSize> sizes;
	std::vector<PixelRect> fits;
	// 映像を描くか (矩形の幅と高さがどちらも 0 より大きいとき)。偽のタイルには映像も名前ラベルの枠の物も描かない。
	std::vector<bool> has_video;
};

// 合成のソースが 1 人の映像だけをキャンバスの全面に描いたか。合成のソースが描き終えたときに、実際に描いたもので決める。
// 次のすべてが成り立つときだけ真。
//   - タイルが 1 つで、その矩形がキャンバス全体である。1 つを 4 等分の 1 つに置いているときは、全面ではないので偽。判定は
//     矩形で行い、縦横比を保って縮めた映像の矩形 (fit) では見ない (映像の縦横比がキャンバスと違っても、全面に 1 人を
//     映していることは変わらない)。
//   - そのタイルの映像を描いた (drawn)。映像が届くまでと、空けた後は、何も描かないので偽。
//   - 描いた映像がダミーのフレームでない (dummy。子のテクスチャに入っているフレームの元。sources/frame_origin.h)。
inline bool composite_shows_one_full(const std::vector<CompositeTile> &tiles, const std::vector<bool> &drawn,
				     const std::vector<bool> &dummy, int canvas_w, int canvas_h)
{
	if (canvas_w <= 0 || canvas_h <= 0)
		return false;
	if (tiles.size() != 1 || drawn.empty() || !drawn.front())
		return false;
	if (dummy.empty() || dummy.front())
		return false;
	const PixelRect &rect = tiles.front().rect;
	return rect.x == 0 && rect.y == 0 && rect.w == canvas_w && rect.h == canvas_h;
}

// 配置の入力と、そのフレームで読んだ値から、1 フレームに描くものを決める。配置は plan_composite_layout で決め、全画面で
// 映す人の子 (focus_extra_slot) に映像があるか (focus_extra_ready) は、facts のその子の大きさで決める。タイルごとの映像の
// 矩形は、タイルの矩形に映像の大きさを fit_inside で収めたもの。キャンバスの大きさが 0 以下なら、何も描かない。
inline CompositeFramePlan plan_composite_frame(const CompositeInput &input, const CompositeFrameFacts &facts)
{
	const auto tile_size = [&facts](int slot) {
		if (slot < 1 || slot > config::kScreenTileCount)
			return ContentSize{};
		return facts.tile_sizes[static_cast<std::size_t>(slot - 1)];
	};

	CompositeLayoutRequest request = make_layout_request(input, facts.canvas_w, facts.canvas_h);
	if (request.focus_extra_slot >= 1 && request.focus_extra_slot <= config::kScreenTileCount) {
		const ContentSize extra = tile_size(request.focus_extra_slot);
		request.focus_extra_ready = extra.w > 0 && extra.h > 0;
	}

	CompositeFramePlan plan;
	plan.layout = plan_composite_layout(request);
	plan.sizes.reserve(plan.layout.tiles.size());
	plan.fits.reserve(plan.layout.tiles.size());
	plan.has_video.reserve(plan.layout.tiles.size());
	for (const CompositeTile &tile : plan.layout.tiles) {
		const ContentSize size = tile.self ? facts.self_size : tile_size(tile.slot);
		const PixelRect fit = fit_inside(tile.rect, size.w, size.h);
		plan.sizes.push_back(size);
		plan.fits.push_back(fit);
		plan.has_video.push_back(fit.w > 0 && fit.h > 0);
	}
	return plan;
}

// タイル 1 つの映像を描くか (draw) と、描く矩形 (fit) と、映像の大きさから矩形への拡大の倍率。合成のソースの描画は、この値の
// とおりに子か自分の枠の映像を描く。描かないタイル (映像の大きさが 0、範囲の外) は draw が偽で、ほかの値は 0。
struct CompositeTileDraw {
	bool draw = false;
	PixelRect fit;
	float scale_x = 0.0f;
	float scale_y = 0.0f;
};

inline CompositeTileDraw composite_tile_draw(const CompositeFramePlan &plan, std::size_t index)
{
	CompositeTileDraw out;
	if (index >= plan.has_video.size() || index >= plan.fits.size() || index >= plan.sizes.size())
		return out;
	const ContentSize &size = plan.sizes[index];
	if (!plan.has_video[index] || size.w <= 0 || size.h <= 0)
		return out;
	out.draw = true;
	out.fit = plan.fits[index];
	out.scale_x = static_cast<float>(out.fit.w) / static_cast<float>(size.w);
	out.scale_y = static_cast<float>(out.fit.h) / static_cast<float>(size.h);
	return out;
}

// ---- フィルタの ON と OFF (番組の描画の中と外) ----
//
// 番組の描画 (メインのキャンバスのミックスの、出力の描画。libobs の render_main_texture の obs_view_render) の間だけ、描画の
// スレッドのスレッドごとの印 (ProgramPassMark) を立てる。立てるのは描画の前のコールバック、降ろすのは描画の後のコールバックと、
// 次の周の tick のコールバック (描画の途中でコールバックが外れても、印を残さない)。libobs は描画のスレッドの 1 周ごとに、tick の
// コールバックの後、ミックスごとに render_main_texture を呼び、メインのキャンバスのミックスは常に先頭である (obs.c の
// obs_init_video の「Reset main canvas mix first so it remains first in the rendering order」)。そこで、tick の後の最初の描画の前の
// コールバックをメインのミックスと見なす。
// 番組の描画の中で合成のソースが 1 人の映像だけを全面に描いたら、印に残す。フィルタは:
//   - 番組の描画の中では、このフレームの番組の描画で、合成のソースが自分より先に 1 人を全面に描いていれば ON。まだ描かれて
//     いない、隠れている、トランジションで描かれていないなら OFF。合成のソースは全面に不透明な背景を敷くので、合成のソースより
//     下に置いたソースは、合成のソースが描かれる限り見えない (フィルタを付けたソースは合成のソースより上に置く)。
//   - 番組の描画の外 (プレビュー、プロジェクター、スクリーンショット、obs-websocket) では、直前の番組の描画の値 (降ろすときに
//     出した値) を使う。
struct ProgramPassMark {
	bool in_pass = false;
	bool drew_solo = false;
	// この周で、描画の前のコールバックが既に呼ばれたか (2 回目からはメインでないミックス)。
	bool first_mix_done = false;
};

// 描画のスレッドの 1 周の始め (tick のコールバック)。印を全部降ろす。
inline void program_pass_tick(ProgramPassMark &mark)
{
	mark = ProgramPassMark{};
}

// 描画の前のコールバック (ミックスごと)。その周で最初のものだけを番組の描画として印を立てる。
inline void program_pass_begin(ProgramPassMark &mark)
{
	mark.in_pass = !mark.first_mix_done;
	mark.first_mix_done = true;
	mark.drew_solo = false;
}

// 合成のソースが描き終えたとき。solo は composite_shows_one_full の結果。番組の描画の外では何も残さない。
inline void program_pass_note_composite(ProgramPassMark &mark, bool solo)
{
	if (mark.in_pass && solo)
		mark.drew_solo = true;
}

// 番組の描画の後。番組の描画だったら真を返し、*published に出す値 (このフレームの番組の描画で、合成のソースが 1 人を全面に
// 描いたか) を入れる。印は降ろす。
inline bool program_pass_end(ProgramPassMark &mark, bool *published)
{
	const bool was_pass = mark.in_pass;
	if (was_pass && published)
		*published = mark.drew_solo;
	mark.in_pass = false;
	mark.drew_solo = false;
	return was_pass;
}
// (first_mix_done は降ろさない。同じ周のほかのミックスの描画を番組の描画と見なさないため。)

// フィルタが付けたソースを描くか。last_published は直前の番組の描画で出した値。
inline bool solo_filter_on(const ProgramPassMark &mark, bool last_published)
{
	return mark.in_pass ? mark.drew_solo : last_published;
}

// ---- 合成のソースの描画どうしで、フレームごとの値を共にする ----

// 今の描画が、フレームごとの値をほかの描画と共にするか (Shared) と、その描画だけに作るか (Local)。共にするのは、OBS の描画の
// スレッドで、そのフレームの合成のソースの tick が済んだ後 (composite_tick_time がそのフレームの時刻) だけである。
//   - tick のコールバックの中の描画 (OBS の [スクリーンショット] の ScreenshotObj。frontend/utility/ScreenshotObj.cpp の
//     ScreenshotTick) は、ソースの tick より前に呼ばれ、フレームの時刻は同じである (obs-video.c の tick_sources)。子の大きさは
//     ソースの tick (async_tick) で変わるので、そこで作った値を覚えると、同じフレームの出力の描画が古い大きさで描く。
//   - 描画のスレッドの外の描画 (obs-websocket の GetSourceScreenshot など) は、描画の周とは別の時刻に動く。
// Local の値も、同じスレッドの同じフレームの間は共にする。
enum class CompositeFrameScope { Shared, Local };

inline CompositeFrameScope composite_frame_scope(bool graphics_thread, std::uint64_t frame_time,
						 std::uint64_t composite_tick_time)
{
	return graphics_thread && composite_tick_time == frame_time ? CompositeFrameScope::Shared
								     : CompositeFrameScope::Local;
}

// 動画のフレームごとに 1 回だけ値を作って覚えておく入れ物。key はそのフレームの時刻 (OBS の obs_get_video_frame_time) で、
// 同じ key で呼ぶと最初に作った値を返す。同じフレームの合成のソースの描画 (出力、プレビュー、プロジェクター、合成のソースが
// 複数あるときのほかの合成のソース) が同じ配置と大きさで描くためである。make はロックの外で呼ぶ (make の中で取るロックとの
// 順を作らない)。2 つのスレッドが同じ key で同時に作ったときは、先に入れたほうの値を両方に返す。どのスレッドから呼んでもよい。
template <typename T> class CompositeFrameMemo {
public:
	template <typename Make> std::shared_ptr<const T> get(std::uint64_t key, Make make)
	{
		{
			std::lock_guard<std::mutex> lock(mu_);
			if (value_ && key_ == key)
				return value_;
		}
		std::shared_ptr<const T> made = std::make_shared<const T>(make());
		std::shared_ptr<const T> old;
		{
			std::lock_guard<std::mutex> lock(mu_);
			if (value_ && key_ == key)
				return value_;
			old = std::move(value_);
			key_ = key;
			value_ = made;
		}
		// 前のフレームの値は、ロックを放してから捨てる。
		old.reset();
		return made;
	}

private:
	// key_ と value_ を守る葉のロック。持ったまま何も呼ばない。
	std::mutex mu_;
	std::uint64_t key_ = 0;
	std::shared_ptr<const T> value_;
};

} // namespace cordmux::sources
