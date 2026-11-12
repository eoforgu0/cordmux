// SPDX-License-Identifier: Apache-2.0

// 合成のソース cordmux が 1 フレームに、映像の上に置く物の計画を決める純粋関数 (plan_composite_draws)。置く物の
// 種類、位置、明るさ、切り抜きの矩形と、置く順を返す。GPU で置く側 (sources/composite_decor.cpp) と CPU で重ねる側
// (sources/composite_draw_cpu.h。テストとプレビューツールが使う) は、同じ計画をそのとおりに置くだけにして、置く物の
// 判断を 2 か所に書かない。libobs にも Qt にも依存しないので、テスト (align_tests) がヘッダーを直接読む。

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sources/composite_label_plan.h"
#include "sources/composite_layout.h"

namespace cordmux::sources {

// 置く物の種類。上から下の順に、計画の中でもこの順に並ぶ (下にあるものほど後に置き、上に重なる)。
//   FrameGlow     枠の内側の光の画像
//   LabelGlow     アバターと名前の光の画像
//   Label         名前ラベルの画像 (背景、アバター、縁取り、文字)
//   RosterGlow    画面共有していない参加者のアバターと名前の光の画像
//   RosterLabel   画面共有していない参加者の名前ラベルの画像
// 画面共有していない参加者の 2 つは、映像の上に重ねるので、画面共有の枠の物の後に置く。
enum class CompositeDrawKind { FrameGlow, LabelGlow, Label, RosterGlow, RosterLabel };

// 描いてある画像 1 つの、セルの左上から見た位置と大きさ。present が偽なら画像はない。
struct CompositeDrawImage {
	bool present = false;
	int x = 0;
	int y = 0;
	int w = 0;
	int h = 0;
};

// タイル 1 つ分の入力。
struct CompositeDrawTile {
	PixelRect cell;          // タイルの矩形 (キャンバスのピクセル)
	bool has_video = false;  // このタイルに映像を描いた。描いていないタイルには何も置かない
	double level = 0.0;      // 話している人を示す明るさ (0 から 1。composite_speaking_level の値)
	// 名前ラベルとその光の画像。label_current は、画像が現在の鍵 (人、見た目、大きさ) で描いたものか。
	// label_owner は画像を描いた人、owner はいまこの枠に描く人 (どちらも枠に載っている人の user_id)。
	CompositeDrawImage label;
	CompositeDrawImage glow;
	bool label_current = false;
	std::string label_owner;
	std::string owner;
	// 名前ラベルの色に掛ける倍率 (composite_label_dim_factor の値。1 なら暗くしない)。
	double label_dim = 1.0;
	// 枠の内側の光の画像 (セルと同じ大きさで描く)。frame_current は、現在の鍵で描いたものか。
	CompositeDrawImage frame;
	bool frame_current = false;
};

// 画面共有していない参加者 1 人分。画像の位置は、タイルと違い、キャンバスの左上から見た位置である (並びは枠に属さず、
// 位置は sources/composite_roster.h の plan_composite_roster が決める)。置くかどうかは呼び出し元が決めて、置く画像
// だけを渡す。
struct CompositeDrawRosterEntry {
	CompositeDrawImage label;
	CompositeDrawImage glow;
	double level = 0.0; // 話している人を示す明るさ (0 から 1)
	double dim = 1.0;   // 名前ラベルの色に掛ける倍率
};

struct CompositeDrawRequest {
	int canvas_w = 0;
	int canvas_h = 0;
	// 画面共有の枠の名前ラベルと光を何も置かない (composite_hides_label の値と、名前の大きさが 0 のとき)。画面共有して
	// いない参加者には適用されない (並びは、自分の設定で出すかを決める)。
	bool hide = false;
	CompositeSpeakingParts parts;   // 話している人を示す光のうち、描くもの
	std::vector<CompositeDrawTile> tiles;
	std::vector<CompositeDrawRosterEntry> roster; // 画面共有していない参加者。並べる順
};

// 置く物 1 つ。画像は、image の位置に等倍で置き、そのうち clip の中だけを描く。
//   - 画像の 4 つのチャンネルすべてに level を掛け、乗算済みアルファの「元 + 下 x (1 - 元のアルファ)」で重ねる。
//   - 名前ラベル (Label と RosterLabel) は、そのあとで 3 つの色のチャンネルだけに dim を掛ける (アルファは変えない)。
//     光と枠の光の dim はいつも 1。
//   - clip は image の中にあり、キャンバスの中にある。光 (LabelGlow) はキャンバスの外だけを切り、セルの境では
//     切らない。名前ラベルと枠の光はセルの外を切る。画面共有していない参加者の物 (RosterGlow と RosterLabel) は、
//     キャンバスの外だけを切る。
struct CompositeDraw {
	CompositeDrawKind kind = CompositeDrawKind::Label;
	// FrameGlow、LabelGlow、Label では CompositeDrawRequest::tiles の、RosterGlow と RosterLabel では
	// CompositeDrawRequest::roster の何番目の画像か。
	std::size_t tile = 0;
	PixelRect image;
	PixelRect clip;
	double level = 1.0;
	double dim = 1.0;
};

namespace draw_plan_detail {

// 2 つの矩形の重なり。重ならなければ大きさ 0 の矩形。
inline PixelRect intersect(const PixelRect &a, const PixelRect &b)
{
	const int left = std::max(a.x, b.x);
	const int top = std::max(a.y, b.y);
	const int right = std::min(a.x + a.w, b.x + b.w);
	const int bottom = std::min(a.y + a.h, b.y + b.h);
	PixelRect out;
	if (right <= left || bottom <= top)
		return out;
	out.x = left;
	out.y = top;
	out.w = right - left;
	out.h = bottom - top;
	return out;
}

// セルの左上から見た画像を、キャンバスのピクセルの矩形にする。
inline PixelRect on_canvas(const PixelRect &cell, const CompositeDrawImage &image)
{
	return PixelRect{cell.x + image.x, cell.y + image.y, image.w, image.h};
}

// 名前ラベルの画像を置くか。現在の鍵の画像なら置く。新しい画像を描き終えていない間は、描く人が同じで、前の画像が
// 現在のセルに収まるときだけ前の画像を置く (composite_keeps_previous_label)。違う人の名前は見せない。
inline bool shows_label(const CompositeDrawTile &tile)
{
	if (!tile.label.present)
		return false;
	if (tile.label_current)
		return true;
	return composite_keeps_previous_label(true, tile.label_owner, tile.owner, tile.label.x, tile.label.y,
					      tile.label.w, tile.label.h, tile.cell.w, tile.cell.h);
}

} // namespace draw_plan_detail

// 1 フレームに置く物を、置く順に返す。順は、全部のタイルの枠の光、全部のタイルの名前ラベルの光、全部の
// タイルの名前ラベルで、それぞれの中はタイルの順。光をタイルごとでなく全部の名前ラベルの下にまとめるのは、
// 隣のタイルへ広がった光が、隣の名前ラベルの上に重ならないようにするためである。
//
// 映像を描いていないタイル、hide が真のときは、タイルの物を何も置かない。明るさが 0 なら、光は置かない。名前ラベルの
// 光は、名前ラベルを置くときだけ置く (名前ラベルの画像と同じ鍵で描いた光なので、名前ラベルを置かずに光だけを置かない)。
// 枠の光は、現在の鍵の画像か、セルと同じ大きさの前の画像だけを置く (composite_keeps_previous_frame_glow)。
//
// 画面共有していない参加者の物は、タイルの物の後に、全部の光、全部の名前ラベルの順で置く。hide には依らない。画像は
// 渡された物をそのまま置き、切り抜きはキャンバスだけである。明るさが 0 なら、光は置かない。
inline std::vector<CompositeDraw> plan_composite_draws(const CompositeDrawRequest &request)
{
	std::vector<CompositeDraw> draws;
	if (request.canvas_w <= 0 || request.canvas_h <= 0)
		return draws;
	const PixelRect canvas{0, 0, request.canvas_w, request.canvas_h};

	std::vector<CompositeDraw> frames;
	std::vector<CompositeDraw> glows;
	std::vector<CompositeDraw> labels;
	for (std::size_t index = 0; !request.hide && index < request.tiles.size(); ++index) {
		const CompositeDrawTile &tile = request.tiles[index];
		if (!tile.has_video || tile.cell.w <= 0 || tile.cell.h <= 0)
			continue;
		const PixelRect cell_clip = draw_plan_detail::intersect(tile.cell, canvas);
		const bool lit = tile.level > 0.0;
		const double level = std::min(tile.level, 1.0);

		if (lit && request.parts.frame_glow && tile.frame.present &&
		    (tile.frame_current ||
		     composite_keeps_previous_frame_glow(true, tile.frame.w, tile.frame.h, tile.cell.w, tile.cell.h))) {
			const PixelRect image = draw_plan_detail::on_canvas(tile.cell, tile.frame);
			const PixelRect clip = draw_plan_detail::intersect(image, cell_clip);
			if (clip.w > 0) {
				CompositeDraw draw;
				draw.kind = CompositeDrawKind::FrameGlow;
				draw.tile = index;
				draw.image = image;
				draw.clip = clip;
				draw.level = level;
				frames.push_back(draw);
			}
		}

		if (!draw_plan_detail::shows_label(tile))
			continue;
		if (lit && request.parts.label_glow && tile.glow.present) {
			const PixelRect image = draw_plan_detail::on_canvas(tile.cell, tile.glow);
			const PixelRect clip = draw_plan_detail::intersect(image, canvas);
			if (clip.w > 0) {
				CompositeDraw draw;
				draw.kind = CompositeDrawKind::LabelGlow;
				draw.tile = index;
				draw.image = image;
				draw.clip = clip;
				draw.level = level;
				glows.push_back(draw);
			}
		}
		const PixelRect image = draw_plan_detail::on_canvas(tile.cell, tile.label);
		const PixelRect clip = draw_plan_detail::intersect(image, cell_clip);
		if (clip.w > 0) {
			CompositeDraw draw;
			draw.kind = CompositeDrawKind::Label;
			draw.tile = index;
			draw.image = image;
			draw.clip = clip;
			draw.level = 1.0;
			draw.dim = std::clamp(tile.label_dim, 0.0, 1.0);
			labels.push_back(draw);
		}
	}

	std::vector<CompositeDraw> roster_glows;
	std::vector<CompositeDraw> roster_labels;
	for (std::size_t index = 0; index < request.roster.size(); ++index) {
		const CompositeDrawRosterEntry &entry = request.roster[index];
		if (!entry.label.present)
			continue;
		const auto image_of = [](const CompositeDrawImage &image) {
			return PixelRect{image.x, image.y, image.w, image.h};
		};
		if (entry.level > 0.0 && request.parts.label_glow && entry.glow.present) {
			const PixelRect image = image_of(entry.glow);
			const PixelRect clip = draw_plan_detail::intersect(image, canvas);
			if (clip.w > 0) {
				CompositeDraw draw;
				draw.kind = CompositeDrawKind::RosterGlow;
				draw.tile = index;
				draw.image = image;
				draw.clip = clip;
				draw.level = std::min(entry.level, 1.0);
				roster_glows.push_back(draw);
			}
		}
		const PixelRect image = image_of(entry.label);
		const PixelRect clip = draw_plan_detail::intersect(image, canvas);
		if (clip.w > 0) {
			CompositeDraw draw;
			draw.kind = CompositeDrawKind::RosterLabel;
			draw.tile = index;
			draw.image = image;
			draw.clip = clip;
			draw.level = 1.0;
			draw.dim = std::clamp(entry.dim, 0.0, 1.0);
			roster_labels.push_back(draw);
		}
	}
	draws.insert(draws.end(), frames.begin(), frames.end());
	draws.insert(draws.end(), glows.begin(), glows.end());
	draws.insert(draws.end(), labels.begin(), labels.end());
	draws.insert(draws.end(), roster_glows.begin(), roster_glows.end());
	draws.insert(draws.end(), roster_labels.begin(), roster_labels.end());
	return draws;
}

} // namespace cordmux::sources
