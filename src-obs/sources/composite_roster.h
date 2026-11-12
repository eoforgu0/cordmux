// SPDX-License-Identifier: Apache-2.0

// 合成のソース cordmux が、画面共有していない参加者 (通話に参加しているが映像の枠を持たない人) の名前ラベルを、キャンバスの
// どこに並べるかを決める純粋関数。参加者ごとの名前ラベルの大きさ (box の幅と高さ) から、参加者ごとの位置を決める。名前ラベルを
// 描くことはここでは扱わない (sources/composite_label.h の paint_composite_label が、名前の幅の上限と余白 0 のセルで
// 描いて、大きさを返す)。映像の上に重ねて描くので、映像の枠は縮めない。libobs にも Qt にも依存しない。

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "config/label_placement.h"
#include "sources/composite_layout.h"

namespace cordmux::sources {

// 並びの設定。設定の roster_show、roster_anchor、roster_direction、roster_avatar_side、roster_gap_ratio、roster_name_max_em、
// roster_offset_x_px、roster_offset_y_px と同じ意味。
struct CompositeRosterParams {
	bool show = true;
	config::RosterAnchor anchor = config::RosterAnchor::BottomRight;
	config::RosterDirection direction = config::RosterDirection::Column;
	config::AvatarSide avatar_side = config::AvatarSide::Right; // 並びの名前ラベルの中のアバターの位置と、縦並びの列の中の揃え
	double gap_ratio = 0.25; // 名前の大きさに対する倍率 (config::kDefaultRosterGapRatio と同じ値)
	double name_max_em = 12.0; // 名前の幅の上限。名前の大きさに対する倍率 (config::kDefaultRosterNameMaxEm と同じ値)
	int offset_x_px = 0; // 配置と余白で決まる位置に足すずれ (キャンバスのピクセル。右が正)
	int offset_y_px = 0; // 同じ (下が正)
};

// 名前ラベルを出す人 1 人。name は表示名、user_id は同じ名前のときの並びの決め手。hears_voice は、cordmux がこの人の声を
// 聞けるか (偽なら、話していなくても名前ラベルを暗くしない。CompositeShare::hears_voice と同じ)。ドックは、通話の参加者のうち
// 名前ラベルを出す人を全員渡す。合成のソースは、名前ラベル付きで描いている枠がある人をその枠に、ない人を並びに描く
// (composite_label_plan.h の composite_roster_shows)。
struct CompositeRosterPerson {
	std::string name;
	std::string user_id;
	bool hears_voice = true;
};

// 並べる順の、人の添字の並び。画面共有の枠と同じ比べ方 (composite_layout.h の share_less。表示名の自然順で、同じ名前なら
// user_id) で、同じ値の人は渡された順のまま。
inline std::vector<std::size_t> composite_roster_order(const std::vector<CompositeRosterPerson> &people)
{
	std::vector<CompositeShare> shares;
	shares.reserve(people.size());
	for (const CompositeRosterPerson &person : people)
		shares.push_back(CompositeShare{0, person.name, person.user_id});
	std::vector<std::size_t> order(people.size());
	for (std::size_t i = 0; i < order.size(); ++i)
		order[i] = i;
	std::stable_sort(order.begin(), order.end(), [&shares](std::size_t a, std::size_t b) {
		return composite_detail::share_less(shares[a], shares[b]);
	});
	return order;
}

// 合成のソースが描く人の添字の並び。composite_roster_order の順で、user_id が空の人と、同じ user_id の 2 人目以降を除く。
// 合成のソースは名前ラベルの画像を user_id ごとに持つので、同じ人を 2 か所に描かない。
inline std::vector<std::size_t> composite_roster_drawn(const std::vector<CompositeRosterPerson> &people)
{
	std::vector<std::size_t> out;
	std::vector<std::string> seen;
	for (const std::size_t index : composite_roster_order(people)) {
		const std::string &id = people[index].user_id;
		if (id.empty() || std::find(seen.begin(), seen.end(), id) != seen.end())
			continue;
		seen.push_back(id);
		out.push_back(index);
	}
	return out;
}

// 参加者ごとの名前ラベルを描くときの、名前の幅の上限 (ピクセル) = キャンバスの幅の半分 - 余白 x 2。4 等分のときの画面共有の
// 人の名前ラベルと同じ上限である。paint_composite_label に、この幅のセルと余白 0 を渡す。0 以下にならない。
inline int composite_roster_cap_width(int canvas_w, double margin_px)
{
	const double cap = static_cast<double>(std::max(0, canvas_w) / 2) - std::max(0.0, margin_px) * 2.0;
	return cap > 0.0 ? static_cast<int>(std::floor(cap)) : 0;
}

// 間隔 g = roster_gap_ratio x 名前の大きさ (ピクセル)。四捨五入して整数にする。
inline int composite_roster_gap_px(double gap_ratio, double name_px)
{
	if (!(gap_ratio > 0.0) || !(name_px > 0.0))
		return 0;
	return static_cast<int>(std::lround(gap_ratio * name_px));
}

// 名前ラベルの矩形 (背景があれば背景の矩形) の大きさ。
struct CompositeRosterBox {
	int w = 0;
	int h = 0;
};

struct CompositeRosterRequest {
	int canvas_w = 0;
	int canvas_h = 0;
	int margin_px = 0; // キャンバスの端からの距離
	int gap_px = 0;    // 人と人の間と、行 (列) と行 (列) の間
	config::RosterAnchor anchor = config::RosterAnchor::BottomRight;
	config::RosterDirection direction = config::RosterDirection::Column;
	config::AvatarSide avatar_side = config::AvatarSide::Right; // 縦並びの、列の中の揃え
	// 配置と余白で決まる位置に足すずれ (キャンバスのピクセル。右と下が正。設定の roster_offset_x_px と roster_offset_y_px)。
	// 誰を置けるか (折り返しと、入りきらない人) には使わない。
	int offset_x_px = 0;
	int offset_y_px = 0;
	// 並べる順の名前ラベルの大きさ。幅か高さが 0 以下のもの (描く物がない人) は、並べず、描かなかった人にも数えない。
	std::vector<CompositeRosterBox> boxes;
};

// 置いた人 1 人。index は CompositeRosterRequest::boxes の添字、x と y は名前ラベルの矩形の左上 (キャンバスの座標)。
struct CompositeRosterPlaced {
	std::size_t index = 0;
	int x = 0;
	int y = 0;
};

struct CompositeRosterPlan {
	// 置いた人。boxes の順。
	std::vector<CompositeRosterPlaced> placed;
	// 入りきらずに置かなかった人の数。黙って消さないために、呼び出し元へ返す。
	int not_placed = 0;
};

namespace roster_detail {

// 並べる 1 人。
struct Item {
	std::size_t index = 0;
	int w = 0;
	int h = 0;
};

// 主軸の長さ (横並びなら幅、縦並びなら高さ) と、副軸の長さ。
inline int main_of(const Item &item, bool row)
{
	return row ? item.w : item.h;
}

inline int cross_of(const Item &item, bool row)
{
	return row ? item.h : item.w;
}

// 行 (縦並びでは列) 1 つ分。members は items の添字の範囲 [begin, end)、main_len は人と人の間を含む主軸の長さ、
// cross_len は副軸のいちばん長い人の長さ。
struct Line {
	std::size_t begin = 0;
	std::size_t end = 0;
	int main_len = 0;
	int cross_len = 0;
};

// 2 つの整数の差の半分 (切り捨て)。負の差は 0 にする (はみ出す物は端に合わせる)。
inline int half_of(int free_space)
{
	return free_space > 0 ? free_space / 2 : 0;
}

} // namespace roster_detail

// 参加者ごとの名前ラベルの位置を決める。規則:
//   - 順に置き、主軸の長さ (横並びなら幅、縦並びなら高さ) がキャンバスの長さ - 余白 x 2 を超えるなら、次の行 (列) へ
//     折り返す。行 (列) を足していくと、全体のまとまりは配置の端に寄るので、キャンバスの中央の側へ伸びる。行 (列) の並びと
//     行の中の順は、どちらも読む順 (上から下、左から右)。
//   - 横並びの揃えは、左の配置が左揃え、中央が中央揃え、右が右揃え。
//   - 縦並びの列の中の揃えはアバターの位置 (左なら左揃え、右なら右揃え)。列の幅はその列でいちばん広い人の幅。縦の揃えは、
//     上の配置が上揃え、下の配置が下揃え、左右の中央の配置が中央揃え。
//   - 行 (列) を足すと副軸の長さがキャンバスの長さ - 余白 x 2 を超えるときは、そこで止めて、残りは置かない。置かなかった
//     人の数を not_placed に返す。
//   - 配置に合わない並びは、配置に合う並びに修正する (config::roster_direction_for)。
//   - 上で決めた全員の位置に、ずれ (offset_x_px、offset_y_px) をそのまま足す。どの人を置くか、どこで折り返すかは、ずらす
//     前の位置で決める (ずらしても並びの形は変わらない)。ずらしてキャンバスの外へはみ出しても、戻さない。
//   - 画面共有の人の名前ラベルなど、ほかの物は避けない。重なっても置く位置は変えない (位置が予測できるようにするため)。
inline CompositeRosterPlan plan_composite_roster(const CompositeRosterRequest &request)
{
	using namespace roster_detail;
	CompositeRosterPlan plan;
	if (request.canvas_w <= 0 || request.canvas_h <= 0)
		return plan;

	std::vector<Item> items;
	for (std::size_t i = 0; i < request.boxes.size(); ++i)
		if (request.boxes[i].w > 0 && request.boxes[i].h > 0)
			items.push_back(Item{i, request.boxes[i].w, request.boxes[i].h});
	if (items.empty())
		return plan;

	const int margin = std::max(0, request.margin_px);
	const int gap = std::max(0, request.gap_px);
	const bool row = config::roster_direction_for(request.anchor, request.direction) == config::RosterDirection::Row;
	const int avail_w = request.canvas_w - margin * 2;
	const int avail_h = request.canvas_h - margin * 2;
	const int avail_main = row ? avail_w : avail_h;
	const int avail_cross = row ? avail_h : avail_w;
	if (avail_main <= 0 || avail_cross <= 0) {
		plan.not_placed = static_cast<int>(items.size());
		return plan;
	}

	// 行 (列) に分ける。1 人でも入りきらない物 (主軸の長さが avail_main を超える) は、その人だけで 1 行にして置く。
	std::vector<Line> lines;
	int cross_total = 0; // 置けた行 (列) の副軸の長さの合計 (間隔を含む)
	std::size_t consumed = 0;
	while (consumed < items.size()) {
		Line line;
		line.begin = consumed;
		line.end = consumed;
		for (std::size_t i = consumed; i < items.size(); ++i) {
			const int added = main_of(items[i], row) + (i > consumed ? gap : 0);
			if (i > consumed && line.main_len + added > avail_main)
				break;
			line.main_len += added;
			line.cross_len = std::max(line.cross_len, cross_of(items[i], row));
			line.end = i + 1;
		}
		const int next_cross = cross_total + (lines.empty() ? 0 : gap) + line.cross_len;
		if (next_cross > avail_cross)
			break;
		cross_total = next_cross;
		lines.push_back(line);
		consumed = line.end;
	}
	plan.not_placed = static_cast<int>(items.size() - consumed);
	if (lines.empty())
		return plan;

	// まとまりの副軸の始まり。上と左の配置は余白、下と右の配置は余白から測った端にまとまりを寄せる。中央の配置は中央に置く。
	using Anchor = config::RosterAnchor;
	const bool top = request.anchor == Anchor::TopLeft || request.anchor == Anchor::TopCenter ||
			 request.anchor == Anchor::TopRight;
	const bool bottom = request.anchor == Anchor::BottomLeft || request.anchor == Anchor::BottomCenter ||
			    request.anchor == Anchor::BottomRight;
	const bool left = request.anchor == Anchor::TopLeft || request.anchor == Anchor::MiddleLeft ||
			  request.anchor == Anchor::BottomLeft;
	const bool right = request.anchor == Anchor::TopRight || request.anchor == Anchor::MiddleRight ||
			   request.anchor == Anchor::BottomRight;

	// 横並びでは行が縦に重なり、まとまりの縦の位置は配置の上下で決まる。縦並びでは列が横に並び、まとまりの横の位置は配置の左右で決まる。
	int cross_origin = 0;
	if (row)
		cross_origin = top ? margin : (bottom ? request.canvas_h - margin - cross_total
						      : margin + half_of(avail_cross - cross_total));
	else
		cross_origin = left ? margin : (right ? request.canvas_w - margin - cross_total
						      : margin + half_of(avail_cross - cross_total));


	int cross_at = cross_origin;
	for (const Line &line : lines) {
		// 行 (列) の中の、主軸の始まり。
		int main_at = 0;
		if (row) {
			if (left)
				main_at = margin;
			else if (right)
				main_at = request.canvas_w - margin - line.main_len;
			else
				main_at = margin + half_of(avail_main - line.main_len);
		} else {
			if (top)
				main_at = margin;
			else if (bottom)
				main_at = request.canvas_h - margin - line.main_len;
			else
				main_at = margin + half_of(avail_main - line.main_len);
		}
		for (std::size_t i = line.begin; i < line.end; ++i) {
			const Item &item = items[i];
			CompositeRosterPlaced out;
			out.index = item.index;
			if (row) {
				out.x = main_at;
				out.y = cross_at;
			} else {
				// 列の中の揃えはアバターの位置。
				const int room = line.cross_len - item.w;
				out.x = cross_at + (request.avatar_side == config::AvatarSide::Right ? room : 0);
				out.y = main_at;
			}
			out.x += request.offset_x_px;
			out.y += request.offset_y_px;
			plan.placed.push_back(out);
			main_at += main_of(item, row) + gap;
		}
		cross_at += line.cross_len + gap;
	}
	return plan;
}

} // namespace cordmux::sources
