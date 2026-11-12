// SPDX-License-Identifier: Apache-2.0

// 合成のソース cordmux の名前ラベルと光の場面を、製品の配置 (sources/composite_layout.h)、描画
// (sources/composite_label.h)、置く物の計画 (sources/composite_draw_plan.h) のとおりに作り、CPU で重ねる
// (sources/composite_draw_cpu.h)。見本 (composite_sample.cpp) と、名前ラベルの調整ツール (tests/label_tuner) が使う。
// 計画 (plan_scene)、タイルごとの描画 (paint_scene_label と paint_scene_frame)、重ね (draw_scene) を分けてあるので、
// 調整ツールは重い描画だけを別のスレッドで行える。人の名前とアバターは見本のために作った架空のもの。

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <QImage>

#include "config/settings.h"
#include "sources/composite_label.h"
#include "sources/composite_label_look.h"
#include "sources/composite_label_plan.h"
#include "sources/composite_layout.h"
#include "sources/composite_roster.h"
#include "sources/decor_params.h"
#include "sources/decor_paint.h"
#include "sources/label_border.h"

namespace cordmux::decor_preview {

// 場面に出せる人。id は引数で人を指す名前、name は表示名 (UTF-8)。
struct ScenePerson {
	const char *id;
	const char *name;
	bool has_avatar;
	int hue; // アバターの地の色相
};

// 見本の人の一覧。先頭の kSamplePeopleCount 人は見本 (--composite-sample) の 4 人で、見本で確認したい見え方
// (画像のアバター、頭文字の円、長い名前の省略) が全部出る。残りは調整ツールで選べる人で、アバターの画像がない人と、
// 長い日本語の名前、長い英語の名前がある (画面共有していない参加者を 8 人まで出せるように、全部で 12 人)。
const std::vector<ScenePerson> &scene_people();
inline constexpr int kSamplePeopleCount = 4;

// 背景の種類 (make_scene_background の kind)。
inline constexpr int kDarkGame = 0;   // 暗いゲーム画面
inline constexpr int kBrightSky = 1;  // 明るい空と雲
inline constexpr int kStripes = 2;    // 細かい縞
inline constexpr int kSnowyField = 3; // 緑の景色と、下側の明るい雪
inline constexpr int kBackgroundKindCount = 4;

// 映像の代わりの背景 (w x h)。kind は上の 4 種類で、範囲の外は 4 で割った余りで読む。
QImage make_scene_background(int kind, int w, int h);

// 架空のアバター (色相 hue の地に、顔と肩の形。128 x 128)。
QImage make_scene_avatar(int hue);

// 場面の席。席 0 は自分の枠、席 1 から 3 は画面共有の枠の番号 1 から 3 で、製品の配置のとおり、自分の枠は左上に、
// 画面共有の枠は表示名の順に残りの位置へ並ぶ。person は scene_people() の添字。
// 名前ラベルを置く隅は席ごとには持たず、設定の label_corner を全員に使う。
struct SceneSeat {
	int person = 0;
};

// 画面共有していない参加者 1 人。person は scene_people() の添字。tracked は、cordmux がその人の声を聞けるか (偽の人は
// 暗くしない。画面共有の枠の人は、自分の枠の人も含めて、いつも聞ける人として暗くする)。
struct SceneRosterMember {
	int person = 0;
	bool tracked = true;
};

// 画面共有していない参加者の最大の人数 (声の枠の数と同じ)。
inline constexpr int kMaxRosterMembers = 8;

struct SceneRequest {
	int canvas_w = 1920;
	int canvas_h = 1080;
	config::Settings settings;
	std::vector<SceneSeat> seats; // 1 から 4 席
	std::vector<SceneRosterMember> roster; // 0 から 8 人。並べる順は plan_scene が決める
	// 並びの最後の 1 人 (並べた順の最後) を、声を聞けない人 (声の枠が埋まっている人) にする。
	bool roster_last_unheard = false;
};

// 場面の計画。値はどれも、合成のソースが設定から作るのと同じ関数で作る。タイルの添字は layout.tiles の並び (左上、
// 右上、左下、右下。1 人なら全面の 1 つ)。
struct ScenePlan {
	int canvas_w = 0;
	int canvas_h = 0;
	sources::CompositeLabelLook look;
	sources::DecorLabelParams params;
	double name_px = 0.0;
	double margin_px = 0.0;
	sources::CompositeLayout layout;
	bool hide = false; // 名前ラベルと光を何も置かない (composite_hides_label か、名前の大きさが 0)
	sources::CompositeSpeakingParts parts;
	// タイルごとの席の番号、人 (scene_people() の添字)、描く人を表す文字列 (計画の label_owner と owner)、描く内容。
	std::vector<int> tile_seat;
	std::vector<int> tile_person;
	std::vector<std::string> tile_owner;
	std::vector<sources::LabelContent> tile_content;
	// タイルごとの、名前ラベルとその光を描くか (製品の sources::composite_tiles_labeled)。偽のタイルには、枠の光だけを置く
	// (名前ラベルを枠に描かない設定 label_show_in_slot)。
	std::vector<bool> tile_labeled;
	// 名前ラベルを描くときの見た目。光を描かない形のときは、光の対象を外してある (合成のソースと同じく、光の画像を
	// 作らない)。
	sources::CompositeLabelLook paint_look;
	// 画面共有していない参加者の名前ラベルを描くときの見た目。paint_look と同じで、アバターの位置だけ並びの設定
	// (roster_avatar_side) にする。
	sources::CompositeLabelLook roster_paint_look;
	// 話していない人の暗さ (%)。タイルごとの、cordmux がその人の声を聞けるか (このツールでは全員聞ける人にする)。
	int dim_pct = 0;
	std::vector<bool> tile_hears_voice;

	// 画面共有していない参加者。並べる順 (画面共有の枠と同じ比べ方) に並べてあり、出さない形 (roster_show が偽) では空。
	// roster_member は SceneRequest::roster の何番目か (名前ラベルを隠す全面の 1 枚の席の人は -1)、roster_track は話している
	// 明るさを読む行 (席の人はその席の番号、並びの人は kCompositeCellCount + roster_member)、roster_person は人
	// (scene_people() の添字)、roster_owner は描く人を表す文字列、roster_content は描く内容 (隅は左上。位置は
	// plan_composite_roster が決める)。
	sources::CompositeRosterParams roster_params;
	int roster_cap_w = 0; // 名前ラベルの幅の上限 (sources::composite_roster_cap_width)
	std::vector<int> roster_member;
	std::vector<int> roster_track;
	std::vector<int> roster_person;
	std::vector<std::string> roster_owner;
	std::vector<bool> roster_hears_voice;
	std::vector<sources::LabelContent> roster_content;
};

// 場面の計画を作る。seats が空か 4 席より多いときは、タイルのない計画を返す。
ScenePlan plan_scene(const SceneRequest &request);

// タイル tile の名前ラベルと光を描く (sources::paint_composite_label)。重いので、調整ツールは別のスレッドで呼ぶ。
// plan は呼んでいる間に変えないこと。
sources::CompositeLabel paint_scene_label(const ScenePlan &plan, std::size_t tile);

// タイル tile の枠の光を描く (sources::paint_frame_glow)。枠を光らせない形なら空の画像を返す。
QImage paint_scene_frame(const ScenePlan &plan, std::size_t tile);

// 画面共有していない参加者 r (roster_member などと同じ並び) の名前ラベルと光を描く。名前の幅の上限 roster_cap_w の幅の
// セルに、余白 0 で描き、名前ラベルの大きさ (box_w と box_h) を得る (位置は plan_composite_roster が決める)。
sources::CompositeLabel paint_scene_roster_label(const ScenePlan &plan, std::size_t r);

// タイル 1 つの、置く画像と明るさ。label_current と frame_current は、現在の計画で描いた画像か。古い画像を置くかは
// 合成のソースと同じ規則 (sources/composite_draw_plan.h) で決まる。label_owner は名前ラベルを描いた人。
struct SceneTileImages {
	sources::CompositeLabel label;
	bool label_current = true;
	std::string label_owner;
	QImage frame;
	bool frame_current = true;
	double level = 0.0; // 話している人を示す明るさ (0 から 1)
	double dim = 1.0;   // 名前ラベルの色に掛ける倍率 (sources::composite_label_dim_factor)
};

// 画面共有していない参加者 1 人の、置く画像と明るさ。label_current は、現在の計画で描いた画像か (偽のときは、描く人が
// 同じなら前の画像を置く)。
struct SceneRosterImages {
	sources::CompositeLabel label;
	bool label_current = true;
	std::string label_owner;
	double level = 0.0;
	double dim = 1.0;
};

// 画面共有していない参加者を置く位置 (キャンバスの座標)。roster は計画の並び (roster_member などと同じ並び) で、
// 置いた人の名前ラベルの矩形の左上と、置けなかった人の数を返す。画像がまだない人は、並べず、数えない。
// 画面共有の人の名前ラベルとは重なっても避けない。
struct SceneRosterLayout {
	std::vector<sources::CompositeRosterPlaced> placed; // index は roster の添字
	int not_placed = 0;
};
SceneRosterLayout layout_scene_roster(const ScenePlan &plan, const std::vector<SceneRosterImages> &roster);

// 背景 (映像の代わり) を描いた canvas に、計画のとおりに名前ラベルと光を重ねる。canvas は
// QImage::Format_ARGB32_Premultiplied で、計画のキャンバスの大きさであること。tiles は計画のタイルと同じ並び、
// roster は計画の画面共有していない参加者と同じ並び (なければ空)。
void draw_scene(QImage *canvas, const ScenePlan &plan, const std::vector<SceneTileImages> &tiles,
		const std::vector<SceneRosterImages> &roster = {});

// draw_scene のうち、キャンバスの一部だけを重ねる。target はキャンバスの (origin.x, origin.y) から始まる範囲の背景を
// 描いた画像 (QImage::Format_ARGB32_Premultiplied)。計画の置く物の切り抜きを target の範囲に狭め、target の左上が
// 原点になるようにずらして同じ関数で重ねるので、どのピクセルも draw_scene で重ねたキャンバスの同じピクセルと同じ値になる
// (調整ツールは、見えている範囲だけを重ねたり、範囲を分けて同時に重ねたりする)。
void draw_scene_region(QImage *target, int origin_x, int origin_y, const ScenePlan &plan,
		       const std::vector<SceneTileImages> &tiles, const std::vector<SceneRosterImages> &roster = {});

} // namespace cordmux::decor_preview
