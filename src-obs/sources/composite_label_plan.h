// SPDX-License-Identifier: Apache-2.0

// 合成のソース cordmux が名前ラベルと話している人の光を描くときの判断を決める純粋関数。名前の大きさと余白、
// 描き直す鍵、光の明るさ、全面 1 枚のときに隠すか、名前ラベルを出す人を枠と画面共有していない参加者の並びのどちらに描くかを
// 決める。libobs にも Qt にも依存しないので、テスト (align_tests) が
// ヘッダーを直接読む。描く処理は sources/composite_decor.cpp にある。

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sources/composite_label_look.h"
#include "sources/composite_layout.h"
#include "sources/composite_roster.h"
#include "sources/label_border.h"

namespace cordmux::sources {

// 名前の大きさ (文字の大きさ。ピクセル) = キャンバスの高さ x 設定の label_em_ratio_pct / 100。キャンバスの高さに
// 対する割合なので、解像度を変えても配信の画面の上での大きさは変わらない。0 以下の入力は 0 を返す (描かない)。
inline double composite_name_px(int canvas_h, double em_ratio)
{
	if (canvas_h <= 0 || !(em_ratio > 0.0))
		return 0.0;
	return static_cast<double>(canvas_h) * em_ratio;
}

// 枠の隅からの余白 (ピクセル) = キャンバスの高さ x 設定の label_margin_pct / 100。名前の大きさに連動させない。
inline double composite_margin_px(int canvas_h, double margin_ratio)
{
	if (canvas_h <= 0 || !(margin_ratio > 0.0))
		return 0.0;
	return static_cast<double>(canvas_h) * margin_ratio;
}

// 描き直すかを比べるための丸め (1/64 ピクセル)。浮動小数点数の誤差で描き直さないため。
inline long long composite_quantize(double value)
{
	return std::llround(value * 64.0);
}

// 名前ラベルの画像と、アバターと名前の光の画像を描き直す鍵。この鍵が前に描いたときと同じなら、画像は同じ内容に
// なるので描き直さない。
//   version       人の情報 (名前、アバター、角、ほかの名前)、枠の描く人、見た目の値のどれかが
//                 変わるたびに進む表の version
//   look_version  合成のソースの見た目の値が変わるたびに進む version
//   owner         枠の描く人。ダミー表示の開始と停止では version が変わらずに替わるので、鍵に入れる
//   cell_w/h      セルの大きさ。文字の折り返しと画像の切り取りが変わる
//   name_q        名前の大きさ (composite_quantize の値)
//   margin_q      余白 (同じ)
//   roster_names  画面共有していない参加者 (並び) の名前。ほかの人と見分けるための名前に足すので、省略の仕方が
//                 変わる。並びは表の version に入らないので、鍵に持つ
struct CompositeLabelKey {
	std::uint64_t version = 0;
	std::uint64_t look_version = 0;
	std::string owner;
	int cell_w = 0;
	int cell_h = 0;
	long long name_q = 0;
	long long margin_q = 0;
	std::vector<std::string> roster_names;
};

inline bool operator==(const CompositeLabelKey &a, const CompositeLabelKey &b)
{
	return a.version == b.version && a.look_version == b.look_version && a.owner == b.owner && a.cell_w == b.cell_w &&
	       a.cell_h == b.cell_h && a.name_q == b.name_q && a.margin_q == b.margin_q &&
	       a.roster_names == b.roster_names;
}

inline bool operator!=(const CompositeLabelKey &a, const CompositeLabelKey &b)
{
	return !(a == b);
}

// 枠の光の画像を描き直す鍵。枠の光は人にも名前にも依らず、セルの大きさ、色、ぼかし、広がりだけで決まる。名前の
// 大きさが変わると、ぼかしと広がり (名前の大きさに対する倍率) が変わるので、その値を丸めて入れる。
struct CompositeFrameGlowKey {
	int cell_w = 0;
	int cell_h = 0;
	std::uint32_t argb = 0;
	long long blur_q = 0;
	long long spread_q = 0;
};

inline bool operator==(const CompositeFrameGlowKey &a, const CompositeFrameGlowKey &b)
{
	return a.cell_w == b.cell_w && a.cell_h == b.cell_h && a.argb == b.argb && a.blur_q == b.blur_q &&
	       a.spread_q == b.spread_q;
}

inline bool operator!=(const CompositeFrameGlowKey &a, const CompositeFrameGlowKey &b)
{
	return !(a == b);
}

inline CompositeLabelKey make_composite_label_key(std::uint64_t version, std::uint64_t look_version,
						  const std::string &owner, const PixelRect &cell, double name_px,
						  double margin_px)
{
	CompositeLabelKey key;
	key.version = version;
	key.look_version = look_version;
	key.owner = owner;
	key.cell_w = cell.w;
	key.cell_h = cell.h;
	key.name_q = composite_quantize(name_px);
	key.margin_q = composite_quantize(margin_px);
	return key;
}

inline CompositeFrameGlowKey make_composite_frame_glow_key(const PixelRect &cell, const CompositeLabelLook &look,
							   double name_px)
{
	CompositeFrameGlowKey key;
	key.cell_w = cell.w;
	key.cell_h = cell.h;
	key.argb = look.glow_argb;
	key.blur_q = composite_quantize(look.glow_blur_ratio * name_px);
	key.spread_q = composite_quantize(look.glow_spread_ratio * name_px);
	return key;
}

// 画像を描き直すか。まだ描いていない (painted がない) か、鍵が前と違えば真。
template <typename Key>
bool composite_needs_repaint(bool painted_valid, const Key &painted, const Key &now)
{
	return !painted_valid || !(painted == now);
}

// 新しい名前ラベルの画像がまだできていない間、前に置いた名前ラベルの画像をそのまま描くか。描く人が替わった
// (shown_owner と wanted_owner が違う) ときは、違う人の名前を見せないので描かない。見た目や大きさだけが変わった
// ときは、前の画像を描いて、消えてちらつくのを避ける。ただし前の画像 (セルの左上から見た位置 x、y と大きさ w、h) が
// 現在のセルからはみ出すときは、セルの大きさが変わって位置がずれた名前ラベルがセルの境で切れて見えるので、描かない。
// 画像がないとき (shown_valid が偽) は描くものがない。
inline bool composite_keeps_previous_label(bool shown_valid, const std::string &shown_owner,
					   const std::string &wanted_owner, int x, int y, int w, int h, int cell_w,
					   int cell_h)
{
	if (!shown_valid || shown_owner != wanted_owner)
		return false;
	return x >= 0 && y >= 0 && w > 0 && h > 0 && x + w <= cell_w && y + h <= cell_h;
}

// 新しい枠の光の画像ができるまで、前の枠の光の画像を描くか。枠の光は人に依らないので、色やぼかしだけが変わった
// ときは前の画像を描く。セルの大きさが違うと、光が枠の縁に合わず、隣にはみ出すので描かない。
inline bool composite_keeps_previous_frame_glow(bool shown_valid, int image_w, int image_h, int cell_w, int cell_h)
{
	return shown_valid && image_w == cell_w && image_h == cell_h;
}

// 合成が全面に 1 枚だけ置いているときに、名前ラベルと光を描かないか。設定の label_hide_when_single が真で、タイルが
// 1 つで、それがキャンバス全体を覆っているときだけ真になる。全面にしない設定 (full_when_single が偽) で 1 枚を
// 左上の 4 等分に置いているときは、キャンバス全体を覆わないので、隠さない。全画面 (一時的に 1 人を全面に描く) の
// 1 枚も、同じ条件で隠す。
inline bool composite_hides_label(const CompositeLabelLook &look, int canvas_w, int canvas_h,
				  const std::vector<CompositeTile> &tiles)
{
	if (!look.hide_when_single || tiles.size() != 1)
		return false;
	const PixelRect &rect = tiles.front().rect;
	return rect.x == 0 && rect.y == 0 && rect.w == canvas_w && rect.h == canvas_h;
}

// タイルごとの、枠の光を描けるか (tiles と同じ並び)。描けるのは、映像を描いたタイル (has_video が真。まだ映像がない枠と空けた
// 後の枠には何も重ねない) で、大きさがあり、名前ラベルと光を隠す全面の 1 枚 (composite_hides_label) でないときである。
// 名前ラベルを枠に描かない設定 (CompositeLabelLook::show_in_slot が偽) でも、枠は残るので枠の光は描く。
inline std::vector<bool> composite_tiles_framed(const CompositeLabelLook &look, int canvas_w, int canvas_h,
						const std::vector<CompositeTile> &tiles, const std::vector<bool> &has_video)
{
	const bool hide = composite_hides_label(look, canvas_w, canvas_h, tiles);
	std::vector<bool> out;
	out.reserve(tiles.size());
	for (std::size_t i = 0; i < tiles.size(); ++i) {
		const PixelRect &cell = tiles[i].rect;
		out.push_back(!hide && i < has_video.size() && has_video[i] && cell.w > 0 && cell.h > 0);
	}
	return out;
}

// タイルごとの、名前ラベルを描くか (tiles と同じ並び)。枠の光を描けるタイル (composite_tiles_framed) のうち、名前ラベルを枠に
// 描く設定 (CompositeLabelLook::show_in_slot) のときだけ描く。名前ラベルの光 (アバターと名前の光) も、名前ラベルと一緒に
// 描く。合成のソースは、この値でタイルに名前ラベルを描くかを決め、同じ値で、名前ラベルを出す人をタイルと並びのどちらに描くかを
// 決める (composite_labeled_tile_users と composite_roster_shows)。2 か所で別々に決めると、同じ人が枠と並びの両方に出たり、
// どちらにも出なかったりする。
inline std::vector<bool> composite_tiles_labeled(const CompositeLabelLook &look, int canvas_w, int canvas_h,
						 const std::vector<CompositeTile> &tiles,
						 const std::vector<bool> &has_video)
{
	std::vector<bool> out = composite_tiles_framed(look, canvas_w, canvas_h, tiles, has_video);
	if (!look.show_in_slot)
		std::fill(out.begin(), out.end(), false);
	return out;
}

// 名前ラベル付きで描いている枠の人の user_id。tile_labeled は composite_tiles_labeled の値、tile_user_ids は同じ並びの
// タイルに描く人 (composite_input.h の composite_tile_user_ids)。空の user_id は入れない。
inline std::vector<std::string> composite_labeled_tile_users(const std::vector<bool> &tile_labeled,
							     const std::vector<std::string> &tile_user_ids)
{
	std::vector<std::string> out;
	for (std::size_t i = 0; i < tile_labeled.size() && i < tile_user_ids.size(); ++i) {
		if (tile_labeled[i] && !tile_user_ids[i].empty())
			out.push_back(tile_user_ids[i]);
	}
	return out;
}

// 名前ラベルを出す人 person (ドックが渡した、通話の参加者のうち名前ラベルを出す人。dock/build_observation.h の
// composite_roster_people) を、画面共有していない参加者の並びに描くか。いま名前ラベル付きで描いている枠がある人
// (labeled_users) は、その枠に名前が出ているので並びに描かず、それ以外の人は並びに描く。どの人も、枠か並びのどちらか
// 1 か所にだけ出る。並びに描くのは次のような人である: 画面共有していない人、枠が足りずに描けない人、枠に載せていない画面共有の
// 人、全画面の間に描かれていない枠の人、[1人のときは非表示] で名前ラベルを隠した全面の 1 枚の人、名前ラベルを枠に描かない設定
// (CompositeLabelLook::show_in_slot が偽) のときの枠の人、自分の枠を出していないか、
// 名前ラベルなしで描いている自分の枠の人、映像がまだ届いていない枠の人。
inline bool composite_roster_shows(const CompositeRosterPerson &person, const std::vector<std::string> &labeled_users)
{
	return std::find(labeled_users.begin(), labeled_users.end(), person.user_id) == labeled_users.end();
}

// 名前ラベルを出す人のうち、並びに描く人 (composite_roster_shows) だけを、渡された順のまま返す。
inline std::vector<CompositeRosterPerson> composite_roster_shown(const std::vector<CompositeRosterPerson> &roster,
								 const std::vector<std::string> &labeled_users)
{
	std::vector<CompositeRosterPerson> out;
	for (const CompositeRosterPerson &person : roster) {
		if (composite_roster_shows(person, labeled_users))
			out.push_back(person);
	}
	return out;
}

// 並びの名前ラベル 1 人が、ほかの人と見分けるために使う名前 (CompositeRosterLabelKey::others)。visible_names は、いま
// キャンバスに見えている名前の全部 (名前ラベル付きで描いている枠の人と、並びに描く人) で、そこから本人の名前を 1 つだけ除き、
// 並べ替えて返す。並べ替えるのは、人が枠と並びの間を移っても (全画面の始まりと終わり)、見えている名前の組が同じなら同じ値に
// して、名前ラベルの画像を描き直さないためである (描き直しを待つ間、その人が並びから一瞬消える)。見分け方は名前の順には
// 依らない。
inline std::vector<std::string> composite_roster_others(std::vector<std::string> visible_names, const std::string &own)
{
	const auto self = std::find(visible_names.begin(), visible_names.end(), own);
	if (self != visible_names.end())
		visible_names.erase(self);
	std::sort(visible_names.begin(), visible_names.end());
	return visible_names;
}

// 話している人を示す光のうち、描くもの。frame_glow は枠の内側の光、label_glow はアバターと名前の光。
struct CompositeSpeakingParts {
	bool frame_glow = false;
	bool label_glow = false;
};

inline CompositeSpeakingParts composite_speaking_parts(const CompositeLabelLook &look)
{
	CompositeSpeakingParts parts;
	parts.frame_glow = look.glow_frame;
	parts.label_glow = look.glow_avatar || look.glow_name;
	return parts;
}

// 時刻 now_ns に、話している人を示すものを描く明るさ (0 なら描かない、1 なら最大)。
//   show_speaking  設定の label_show_speaking。偽ならいつも 0
//   muted, volume  OBS のミキサーの、合成のソースの行のミュートと音量 (0 以上の倍率)。ミュートか音量が 0 のとき
//                  は声が配信に出ていないので、光らせない
// 音量が NaN のときは、読めていないので光らせない側に倒す (声が出ていない可能性があるのに光ると、聞こえない声の
// 人が話しているように見える)。
inline double composite_speaking_level(const SpeakingTimes &times, std::uint64_t now_ns, const BorderTiming &timing,
				       bool show_speaking, bool muted, float volume)
{
	if (!show_speaking || muted || !(volume > 0.0f))
		return 0.0;
	return speaking_level(times, now_ns, timing);
}

// 話していない人の名前ラベルを暗くするための、時刻 now_ns の明るさ (0 から 1)。composite_speaking_level と同じ値だが、
// 設定の「光」のオンとオフには依らない (光をオフにしても暗さは動作する。この関数に show_speaking は要らない)。muted と
// volume の扱いは composite_speaking_level と同じで、声が配信に出ていない人は話していない人として扱う。
inline double composite_label_dim_level(const SpeakingTimes &times, std::uint64_t now_ns, const BorderTiming &timing,
					bool muted, float volume)
{
	return composite_speaking_level(times, now_ns, timing, true, muted, volume);
}

// 名前ラベルの色 (乗算済みアルファの RGB。アルファは変えない) に掛ける倍率 f。
//     f = 1 - (dim_pct / 100) x (1 - level)
// level は光と同じ明るさ (話していれば 1、黙って保持時間とフェードアウトが過ぎれば 0)。話していれば f は 1、黙って
// 十分たてば 1 - dim_pct / 100 になる。次のときは、いつも 1 (暗くしない)。
//   - dim_pct が 0 以下 (暗くしない設定)
//   - hears_voice が偽 (cordmux がその人の声を聞けない。自分の声の経路がないときの自分の枠の人。話しても
//     光らないので、話していない人と見分けられず、暗くすると配信で不当に暗く見える)
//   - mixer_audible が偽 (OBS のミキサーで合成のソースがミュートか音量 0。この間は光らせず、配信で誰が話しているかを
//     示さないので、暗さも掛けない。このとき呼び出し元は composite_label_dim_level にミュートと音量を渡さず、false と 1 を渡す)
//   - level が NaN (読めていないときは、話している可能性があるので暗くしない)
// dim_pct は 0 から 100 に収め、level は 0 から 1 に収める。
inline double composite_label_dim_factor(double level, int dim_pct, bool hears_voice, bool mixer_audible = true)
{
	if (!hears_voice || !mixer_audible || dim_pct <= 0 || std::isnan(level))
		return 1.0;
	const double dim = static_cast<double>(std::min(dim_pct, 100)) / 100.0;
	const double lit = std::min(std::max(level, 0.0), 1.0);
	return 1.0 - dim * (1.0 - lit);
}

// 合成のソースが 1 人の名前ラベルに掛ける、時刻 now_ns の倍率 f (composite_label_dim_factor)。muted と volume は OBS の
// ミキサーの合成のソースの行のミュートと音量で、ミュートか音量が 0 以下 (NaN を含む) なら暗くしない。明るさは
// composite_label_dim_level にミュートと音量を渡さずに求める (ミキサーの扱いは mixer_audible の 1 か所で決める)。
inline double composite_label_dim_at(const SpeakingTimes &times, std::uint64_t now_ns, const BorderTiming &timing,
				     int dim_pct, bool hears_voice, bool muted, float volume)
{
	const bool mixer_audible = !muted && volume > 0.0f;
	return composite_label_dim_factor(composite_label_dim_level(times, now_ns, timing, false, 1.0f), dim_pct,
					  hears_voice, mixer_audible);
}

// 画面共有していない参加者 1 人の、名前ラベルの画像と光の画像を描き直す鍵。この鍵が前に描いたときと同じなら描き直さない。
//   user_id         描く人。画像は参加者ごとに持つ
//   person_version  人の情報 (名前とアバター。decor_registry.h の set_roster_person_decor) の version
//   look_version    合成のソースの見た目の値の version
//   avatar_side     並びの名前ラベルのアバターの位置 (設定の roster_avatar_side。見た目の値とは別に持つ)
//   name_max_em     並びの名前の幅の上限 (設定の roster_name_max_em。見た目の値とは別に持つ)
//   name_q         名前の大きさ (composite_quantize の値)
//   cell_w/h        描くセルの大きさ (名前の幅の上限と、キャンバスの高さ)
//   others          ほかの人と見分けるための名前 (composite_roster_others。見えている名前から本人を除き並べ替えた
//                   もの)。省略の仕方が変わる
struct CompositeRosterLabelKey {
	std::string user_id;
	std::uint64_t person_version = 0;
	std::uint64_t look_version = 0;
	config::AvatarSide avatar_side = config::AvatarSide::Right;
	double name_max_em = 0.0; // 名前の幅の上限 (並びの設定の roster_name_max_em)
	long long name_q = 0;
	int cell_w = 0;
	int cell_h = 0;
	std::vector<std::string> others;
};

inline bool operator==(const CompositeRosterLabelKey &a, const CompositeRosterLabelKey &b)
{
	return a.user_id == b.user_id && a.person_version == b.person_version && a.look_version == b.look_version &&
	       a.avatar_side == b.avatar_side && a.name_max_em == b.name_max_em && a.name_q == b.name_q &&
	       a.cell_w == b.cell_w && a.cell_h == b.cell_h && a.others == b.others;
}

inline bool operator!=(const CompositeRosterLabelKey &a, const CompositeRosterLabelKey &b)
{
	return !(a == b);
}

// 前に画像を持っていた人 (cached) のうち、いま描く人 (drawn) にいない人。この人たちのテクスチャを捨て、描き直しの保存先を
// 空ける。順は cached の順。
inline std::vector<std::string> composite_roster_departed(const std::vector<std::string> &cached,
							  const std::vector<std::string> &drawn)
{
	std::vector<std::string> out;
	for (const std::string &id : cached) {
		if (std::find(drawn.begin(), drawn.end(), id) == drawn.end())
			out.push_back(id);
	}
	return out;
}

} // namespace cordmux::sources
