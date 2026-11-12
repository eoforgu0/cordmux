// SPDX-License-Identifier: Apache-2.0

// 合成のソース cordmux が、キャンバスのどこに誰を描くかを決める純粋関数。キャンバスの幅と高さ、自分の枠を出して
// いるか、載っている画面共有の一覧から、タイルの位置 (整数のピクセル) を決める。libobs にも Qt にも依存しない。
// 枠が 2 つ以上のときは、人数が変わってもキャンバスの 4 等分の決まった位置に置く。ほかのソフトとの連携の保存先所を
// この 4 等分に合わせている配信者の配置を、人数で崩さないためである。誰をどの位置に置くかは ledger/slot_cells.h の
// plan_slot_cells が決め、ドックの行の枠の位置も同じ関数で決める。描画とソースの登録はここでは扱わない。

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "config/allocation.h"
#include "ledger/natural_order.h"
#include "ledger/slot_cells.h"

namespace cordmux::sources {

// 4 等分の位置の数 (左上、右上、左下、右下)。
inline constexpr int kCompositeCellCount = ledger::kSlotCellCount;

// 整数のピクセルの矩形。x と y は左上、w と h は大きさ。
struct PixelRect {
	int x = 0;
	int y = 0;
	int w = 0;
	int h = 0;
};

// 載っている画面共有 1 つ分。name は並べ替えにだけ使い、ログには書かない。
struct CompositeShare {
	int slot = 0;         // 枠の番号 (1 から 4)
	std::string name;     // 表示名。並びは自然順 (ledger/natural_order.h)
	std::string user_id;  // 同じ名前のときの並びの決め手
	// cordmux がこの人の声を聞けるか。偽なら、話していなくても名前ラベルを暗くしない (話しても光らないので、話していない
	// 人と見分けられない)。配置には使わない。
	bool hears_voice = true;
	// 載せた順の番号 (config::SlotAssignment::added_order)。並び順が「載せた順」のときの決め手。
	std::int64_t added_order = 0;
	// メインアカウントの画面共有か。自分の枠を先頭に置く設定 (self_slot_first) の間は、名前順と載せた順で、先頭 (自分の枠の次)
	// に置く (ledger/slot_cells.h)。
	bool is_main = false;
};

struct CompositeLayoutRequest {
	// キャンバスの大きさ (OBS の映像の基本解像度)。どちらかが 0 以下なら何も置かない。
	int canvas_w = 0;
	int canvas_h = 0;

	// 自分の枠を cordmux 経由で出しているか。出しているときだけ真にする (cordmux を通さずに OBS に出している映像は
	// 数えない)。
	bool self_shown = false;

	// 載っている画面共有。並びは問わない (この関数が slot_order の順に並べる)。
	std::vector<CompositeShare> shares;

	// 並び順 (設定の slot_order)。
	config::SlotOrder slot_order = config::SlotOrder::Name;

	// 自分の枠とメインアカウントの画面共有を、並び順によらず先頭に置くか (設定の self_slot_first)。偽なら、ほかの人と同じ
	// 並び順で置く。名前順では自分の枠を self_name で、載せた順では self_added_mark で比べる (ledger/slot_cells.h の
	// SlotCellSelf)。
	bool self_first = true;
	std::int64_t self_added_mark = 0;

	// 合わせて 1 つのときに全面に置くか。偽なら、1 つでも 4 等分の左上に置く。
	bool full_when_single = true;

	// タイルの間のすき間 (ピクセル。設定の tile_gap_px)。負の値は 0 として読む。
	int gap_px = 0;

	// 全画面で映す人の user_id (空 = 全画面でない)。この人のタイルを全画面でないときの配置で描くときは、そのタイル 1 つだけを
	// キャンバスの全面に描き、ほかのタイルを描かない (full_when_single に依らない)。この人を描かないとき (dropped の人、
	// shares にいない人) は、全画面でないときと同じに描く。
	std::string focus_user_id;
	// 自分の枠の人の user_id。全画面で映す人が自分の枠の人かを、この値で見分ける。
	std::string self_user_id;
	// 自分の枠の人の、配信に出している名前 (自分の枠を並び順で置くときの、名前順の決め手)。
	std::string self_name;
	// 全画面で映す人を、全画面でないときの配置で描かないとき (枠に載せていない人、描けずに dropped になった人) に、その人の
	// 映像を持つ子の番号 (枠に載せていない人は kFocusOnlySlot、dropped の人はその人の枠)。0 なら、そのような子はない。
	// focus_extra_ready (その子に映像があるか) が真のときだけ、その子を全面に描く。映像が届くまでは、全画面でないときと
	// 同じに描く (受信を始めた直後に、空の画面を出さない)。
	int focus_extra_slot = 0;
	bool focus_extra_ready = false;
};

// 置くタイル 1 つ分。
struct CompositeTile {
	bool self = false; // 自分の枠か
	int slot = 0;      // 画面共有の枠の番号。自分の枠は 0
	PixelRect rect;
};

struct CompositeLayout {
	// 左上、右上、左下、右下の順 (全面のときは 1 つ)。
	std::vector<CompositeTile> tiles;

	// 置く場所が足りずに描かなかった画面共有。黙って消さないために呼び出し元へ返す。全画面で 1 人を映している間も、
	// 全画面でないときの配置で描かない人だけを入れる (全画面で隠れたタイルの人は入れない)。
	std::vector<CompositeShare> dropped;

	// 全画面で 1 人を映しているか (tiles はその人のタイル 1 つで、キャンバスの全面)。
	bool focused = false;
};

namespace composite_detail {

// 画面共有の並び。表示名の自然順で、同じ名前なら user_id、それも同じなら枠の番号。実行のたびに並びが変わらない
// ようにする。
inline bool share_less(const CompositeShare &a, const CompositeShare &b)
{
	return ledger::slot_name_less(a.name, a.user_id, a.slot, b.name, b.user_id, b.slot);
}

// 自分の枠の置き方 (ledger/slot_cells.h の SlotCellSelf)。ドックの行の位置 (ledger/rules_dock.cpp の slot_positions) と同じ値を
// 渡す。
inline ledger::SlotCellSelf cell_self_of(bool shown, bool first, const std::string &name, const std::string &user_id,
					 std::int64_t added_mark)
{
	ledger::SlotCellSelf self;
	self.shown = shown;
	self.first = first;
	self.name = name;
	self.user_id = user_id;
	self.added_mark = added_mark;
	return self;
}

inline std::vector<ledger::SlotCellShare> cell_shares_of(const std::vector<CompositeShare> &shares)
{
	std::vector<ledger::SlotCellShare> out;
	out.reserve(shares.size());
	for (const CompositeShare &share : shares) {
		ledger::SlotCellShare cell;
		cell.slot = share.slot;
		cell.name = share.name;
		cell.user_id = share.user_id;
		cell.added_order = share.added_order;
		cell.is_main = share.is_main;
		out.push_back(std::move(cell));
	}
	return out;
}

// キャンバスを 4 等分した位置 cell (0 左上、1 右上、2 左下、3 右下) の矩形。幅と高さが奇数のときは、左の列と上の行を
// 切り捨てた大きさにし、残りを右の列と下の行に入れる。どちらに寄せても隙間なく覆える。境界の位置を整数の割り算
// (幅 / 2、高さ / 2) の結果そのものにでき、左上のタイルから右下のタイルまで同じ式で求められるので、この向きにした。
inline PixelRect cell_rect(int canvas_w, int canvas_h, int cell)
{
	const int left_w = canvas_w / 2;
	const int top_h = canvas_h / 2;
	const bool right = (cell % 2) == 1;
	const bool bottom = (cell / 2) == 1;
	PixelRect rect;
	rect.x = right ? left_w : 0;
	rect.y = bottom ? top_h : 0;
	rect.w = right ? canvas_w - left_w : left_w;
	rect.h = bottom ? canvas_h - top_h : top_h;
	return rect;
}

// タイルの間のすき間を作る。キャンバスの端に接していない辺だけを内側に寄せ、隣り合う 2 つのタイルのあいだが
// ちょうど gap になるようにする。奇数のすき間は、左または上のタイルの辺が小さいほう (gap / 2、切り捨て)、右または下の
// タイルの辺が残り (gap - gap / 2) を取る。キャンバスの外周にはすき間ができない。幅と高さは最低 1 にする。
inline PixelRect inset_for_gap(const PixelRect &rect, int canvas_w, int canvas_h, int gap)
{
	if (gap <= 0)
		return rect;
	const int far_side = gap / 2;
	const int near_side = gap - far_side;
	int left = rect.x;
	int top = rect.y;
	int right = rect.x + rect.w;
	int bottom = rect.y + rect.h;
	if (left > 0)
		left += near_side;
	if (top > 0)
		top += near_side;
	if (right < canvas_w)
		right -= far_side;
	if (bottom < canvas_h)
		bottom -= far_side;
	PixelRect out;
	out.x = left;
	out.y = top;
	out.w = std::max(1, right - left);
	out.h = std::max(1, bottom - top);
	return out;
}

} // namespace composite_detail

// 載っている画面共有を、タイルに描くもの (placed) と、置く場所が足りずに描かないもの (dropped) に分ける。分け方は
// ledger/slot_cells.h の plan_slot_cells のとおりで、置ける数 (4 等分の位置の数。自分の枠を出していればその 1 つを含む) まで
// を描き、残りを描かない。キャンバスの大きさには依らない。plan_composite_layout と同じ分け方である。描かない人は、名前ラベル付きで
// 描く枠がないので、画面共有していない参加者の並びに出る (composite_label_plan.h の composite_roster_shows)。
struct CompositeShareSplit {
	std::vector<CompositeShare> placed;  // 左上の位置からの順
	std::vector<CompositeShare> dropped; // 並び順の順
};

inline CompositeShareSplit split_composite_shares(const std::vector<CompositeShare> &shares, const ledger::SlotCellSelf &self,
						  config::SlotOrder order = config::SlotOrder::Name)
{
	const ledger::SlotCellPlan plan =
		ledger::plan_slot_cells(composite_detail::cell_shares_of(shares), self, order, ledger::kFullWhenSingle);
	CompositeShareSplit split;
	for (const ledger::SlotCell &cell : plan.cells) {
		if (!cell.self)
			split.placed.push_back(shares[static_cast<std::size_t>(cell.share)]);
	}
	for (const int index : plan.dropped)
		split.dropped.push_back(shares[static_cast<std::size_t>(index)]);
	return split;
}

// 自分の枠を先頭に置くとき (設定の self_slot_first の既定) の形。
inline CompositeShareSplit split_composite_shares(const std::vector<CompositeShare> &shares, bool self_shown,
						  config::SlotOrder order = config::SlotOrder::Name)
{
	ledger::SlotCellSelf self;
	self.shown = self_shown;
	return split_composite_shares(shares, self, order);
}

// タイルの位置を決める。規則:
//   - 合わせて 2 つ以上なら、常に 2 x 2 の決まった位置。3 つでも 2 つでも形を変えない (空いた位置は描かない)。
//   - 自分の枠は、先頭に置く設定 (self_first) なら左上。画面共有の位置と、先頭に置かない自分の枠の位置は、並び順で決まる
//     (ledger/slot_cells.h の plan_slot_cells)。
//   - 合わせて 1 つなら全面 (full_when_single が偽なら、その位置の 4 等分)。0 なら空。
//   - 自分の枠を出していて画面共有が 4 つあると 5 つになり、置けない。置けない画面共有を描かず、dropped に入れる。自分の枠を
//     先頭に置かないときは、自分の枠が後ろになって描かれないことがある (dropped には入れない)。
//   - 全画面で映す人 (focus_user_id) が上の規則で描かれる人なら、その人のタイルだけをキャンバスの全面に描く。どの人を
//     どの枠に載せているかと、dropped は変えない。
//   - 全画面で映す人が上の規則で描かれない人で、その人の映像を持つ子 (focus_extra_slot) に映像があれば
//     (focus_extra_ready)、その子を全面に描く。映像がまだなければ、全画面でないときと同じに描く。
inline CompositeLayout plan_composite_layout(const CompositeLayoutRequest &request)
{
	CompositeLayout layout;
	if (request.canvas_w <= 0 || request.canvas_h <= 0)
		return layout;

	const ledger::SlotCellPlan plan = ledger::plan_slot_cells(
		composite_detail::cell_shares_of(request.shares),
		composite_detail::cell_self_of(request.self_shown, request.self_first, request.self_name, request.self_user_id,
					       request.self_added_mark),
		request.slot_order, request.full_when_single);
	for (const int index : plan.dropped)
		layout.dropped.push_back(request.shares[static_cast<std::size_t>(index)]);

	if (!request.focus_user_id.empty()) {
		for (const ledger::SlotCell &cell : plan.cells) {
			const std::string &user_id =
				cell.self ? request.self_user_id : request.shares[static_cast<std::size_t>(cell.share)].user_id;
			if (user_id != request.focus_user_id)
				continue;
			CompositeTile tile;
			tile.self = cell.self;
			tile.slot = cell.self ? 0 : request.shares[static_cast<std::size_t>(cell.share)].slot;
			tile.rect.w = request.canvas_w;
			tile.rect.h = request.canvas_h;
			layout.tiles.push_back(tile);
			layout.focused = true;
			return layout;
		}
		if (request.focus_extra_slot >= 1 && request.focus_extra_ready) {
			CompositeTile tile;
			tile.slot = request.focus_extra_slot;
			tile.rect.w = request.canvas_w;
			tile.rect.h = request.canvas_h;
			layout.tiles.push_back(tile);
			layout.focused = true;
			return layout;
		}
	}

	const int gap = std::max(0, request.gap_px);
	for (const ledger::SlotCell &cell : plan.cells) {
		CompositeTile tile;
		tile.self = cell.self;
		tile.slot = cell.self ? 0 : request.shares[static_cast<std::size_t>(cell.share)].slot;
		if (plan.full) {
			tile.rect.x = 0;
			tile.rect.y = 0;
			tile.rect.w = request.canvas_w;
			tile.rect.h = request.canvas_h;
		} else {
			const PixelRect rect = composite_detail::cell_rect(request.canvas_w, request.canvas_h, cell.cell);
			tile.rect = composite_detail::inset_for_gap(rect, request.canvas_w, request.canvas_h, gap);
		}
		layout.tiles.push_back(tile);
	}
	return layout;
}

// 幅 content_w、高さ content_h の物を、縦横比を保ったまま rect の内側に収まる最大の大きさにして、中央に置いた矩形を
// 返す。中央の位置は、余りが奇数のとき左と上に寄せる (切り捨て)。大きさは四捨五入で、縮めた側が 0 にならないよう最低 1 にする。
// content_w か content_h が 0 以下 (物の大きさが分からない) なら、何も描かないよう、大きさ 0 の矩形を rect の左上に返す。
inline PixelRect fit_inside(const PixelRect &rect, int content_w, int content_h)
{
	PixelRect out;
	out.x = rect.x;
	out.y = rect.y;
	if (content_w <= 0 || content_h <= 0 || rect.w <= 0 || rect.h <= 0)
		return out;

	const std::int64_t rw = rect.w;
	const std::int64_t rh = rect.h;
	const std::int64_t cw = content_w;
	const std::int64_t ch = content_h;
	std::int64_t w = 0;
	std::int64_t h = 0;
	if (rw * ch <= rh * cw) {
		// 幅いっぱいに合わせる。
		w = rw;
		h = (2 * rw * ch + cw) / (2 * cw);
		h = std::min(std::max<std::int64_t>(1, h), rh);
	} else {
		// 高さいっぱいに合わせる。
		h = rh;
		w = (2 * rh * cw + ch) / (2 * ch);
		w = std::min(std::max<std::int64_t>(1, w), rw);
	}
	out.w = static_cast<int>(w);
	out.h = static_cast<int>(h);
	out.x = rect.x + static_cast<int>((rw - w) / 2);
	out.y = rect.y + static_cast<int>((rh - h) / 2);
	return out;
}

// 配置のログの 1 行 (改行なし)。形は
//   Composite layout canvas=<w>x<h> count=<n> <tile>=<x>,<y>,<w>,<h> ...
// で、<tile> は self か slot<N>、並びは tiles の順、count は描くタイルの数。人の ID と名前は書かない。
inline std::string composite_layout_log_line(int canvas_w, int canvas_h, const std::vector<CompositeTile> &tiles)
{
	std::string line = "Composite layout canvas=" + std::to_string(canvas_w) + "x" + std::to_string(canvas_h) +
			   " count=" + std::to_string(tiles.size());
	for (const CompositeTile &tile : tiles) {
		line += ' ';
		line += tile.self ? std::string("self") : "slot" + std::to_string(tile.slot);
		line += '=' + std::to_string(tile.rect.x) + ',' + std::to_string(tile.rect.y) + ',' +
			std::to_string(tile.rect.w) + ',' + std::to_string(tile.rect.h);
	}
	return line;
}

} // namespace cordmux::sources
