// SPDX-License-Identifier: Apache-2.0

// 合成のソース cordmux の名前ラベルと、話している人を示すグローを QPainter で描画する。libobs に依存しない。
// 描画する関数は、値を受け取って QImage を返すだけにして、OBS の型は現れない。合成のソースはキャンバスのピクセルで
// 描画するので、倍率は 1 で、大きさと位置はすべてピクセルで受け渡す。見た目の値は composite_label_look.h にある。
// 画像をキャンバスに置く順と切り抜きは sources/composite_draw_plan.h が決め、CPU で重ねる関数は
// sources/composite_draw_cpu.h にある。テスト (tests/decor/) は、同じ入力から描画した PNG を完全一致で比べる。

#pragma once

#include <cstdint>

#include <QImage>
#include <QString>

#include "sources/composite_label_look.h"
#include "sources/decor_paint.h"

namespace cordmux::sources {

// paint_composite_label の結果。画像はすべて乗算済みアルファ (QImage::Format_ARGB32_Premultiplied) で、透明でない
// ピクセルを囲む大きさに切り詰めてある。位置はセルの左上から見たピクセル。描画するものがなければ、画像は空で、
// 位置は 0。
//
// 2 つの画像は、グロー、名前ラベルの順に、どちらも「元の色 + 下の色 x (1 - 元のアルファ)」で重ねる。グローには
// 呼び出し元が時間の明るさ level を 4 つのチャンネルすべてに掛ける。こう重ねると、level がいくつでも、グローと
// 名前ラベルを 1 枚にまとめてから名前ラベル全体の不透明度を 1 度だけ掛けたものと同じ値になる (グローの画像の
// 作り方は paint_composite_label のコメント)。
struct CompositeLabel {
	// 名前ラベル (背景、アバターとその縁取り、縁取りの文字)。名前ラベル全体の不透明度を掛けてある。セルが小さすぎ
	// なければ、セルの中に収まる。収まらない部分は、置くときにセルで切り抜く。
	QImage label;
	int label_x = 0;
	int label_y = 0;
	// アバターと名前のグロー。最大の明るさ (話している間の明るさ) で描いてあり、名前ラベルの下になる部分を抜いて
	// ある。セルの外へ広がることがあり、ここでは切り取らない (置くときにキャンバスの外だけを切る)。グローの対象
	// (glow_avatar と glow_name) がないか、広がりもぼかしも 0 なら空。
	QImage glow;
	int glow_x = 0;
	int glow_y = 0;
	// 名前ラベルの矩形 (背景があれば背景の矩形)。背景がなければ、縁取りを含むアバターと文字を並べた範囲。
	int box_x = 0;
	int box_y = 0;
	int box_w = 0;
	int box_h = 0;
	// アバター (または頭文字) を囲む正方形。縁取りは含まない。描画しなければ大きさは 0。
	int avatar_x = 0;
	int avatar_y = 0;
	int avatar_d = 0;
	bool elided = false;         // 名前を省略した
	bool elided_middle = false;  // ほかの名前と見分けられないので、真ん中を省略した
	bool avatar_dropped = false; // 名前の幅を確保するために、アバターを省いた
	bool name_hidden = false;    // 見える文字が少なすぎるので、名前を描画しなかった
	bool initial_drawn = false;  // アバターの画像がないので、名前の頭文字を描いた
	QString name_drawn;          // 実際に描画した名前 (省略したときは省略記号を含む)
	double name_room_px = 0.0;   // 名前に使えた幅
	QString font_family_used;    // 文字の描画に実際に使われたフォント
};

// セルの隅 (content.corner) から margin_px 離して、名前ラベルとグローを描画する。
//
// name_px は文字の大きさ (ピクセル)、cell_w と cell_h はセルの大きさ。余白は、セルの端から名前ラベルの矩形 (box)
// までの距離である。名前ラベルの幅は「セルの幅 - 余白 x 2」を超えない。超えるときは、名前を省略するか、アバターを
// 省く (ほかの名前と見分けがつかないときは、アバターを残して真ん中を省略する)。見える文字が min_visible_graphemes
// より少なくなる名前は描画しない。名前もアバターも描画しないとき (name_px や cell が 0 以下、名前が空でアバターも
// ない、不透明度が 0) は、画像が空になる。字形が矩形の外へはみ出してセルの外に出るときは、セルの中へ寄せる。
//
// look.avatar_side が右のときは、名前ラベルの中の並びを左右入れ替える (アバターが右端、名前がその左)。背景の内側の余白も
// 入れ替わり、アバターのある側が plate_pad_v_em、名前のある側が plate_pad_right_em になる。光の形は同じ配置から作るので、
// 入れ替えた形になる。名前の省略とアバターを省く判断は、左のときと同じ (幅だけで決まる)。
//
// 縁取りは、文字の輪郭とアバターの輪郭 (頭文字も) を中心にした同じ太さの線を引き、その上に文字とアバターを重ねる
// (見える縁は、どちらも線の太さの半分)。余白、背景の内側の余白、アバターと名前の間は、どれも見える縁 (縁取りの
// 外側) から測る。縁取りを太くすると名前ラベルが大きくなり、アバターと名前の間が詰まったり、縁取りが背景の外へ
// 出たりしない。アバターの画像がなくて show_avatar が真なら、名前の頭文字 1 文字 (書記素) をアバターの形で描く。画像が
// 取れなくても配信に影響させず、警告も出さないため。
//
// グローの形は、見える縁 (縁取りの外側。縁取りが 0 なら字形とアバターの形) を、広がり (glow_spread_ratio x
// name_px) だけ円の範囲で膨らませ (glow_filter.h の dilate_disk。どの向きにも同じ距離)、標準偏差
// glow_blur_ratio x name_px / 2 のぼかしをかけたものである。文字とアバターは同じ処理を通るので、同じ値なら同じ
// 太さとぼけ方になる。対象は glow_avatar と glow_name で選ぶ。背景を表示するときは、どちらかが真なら、背景を含む
// 名前ラベル全体の外側に付ける。
//
// グローの画像には、グローの色と次の倍率を掛けてある。o は名前ラベル全体の不透明度、c は名前ラベルの見える形
// (背景、縁取りを含むアバター、縁取りを含む文字) が覆う割合、a は不透明度を掛ける前の名前ラベルのアルファ。
//     o x (1 - c) / (1 - o x a)
// (1 - c) は、名前ラベルの下になる所のグローを抜く (半透明の背景やアバターの透明な所から、グローが透けない)。
// 1 / (1 - o x a) は、上に重ねる名前ラベルが「1 - o x a」だけ下を残すことを打ち消し、2 枚を重ねた値を、1 枚に
// まとめてから o を 1 度だけ掛けた値に一致させる。見える形の中 (c = 1) では 0 になり、不透明な部分の縁 (a = c) で
// o = 1 なら 1 になる (グローを抜かずに重ねたのと同じ)。広がりもぼかしも 0 なら、グローは見える形そのものに
// なって見えないので、描かない。同じ入力からは必ず同じピクセルを返す。
//
// 描画の時間の目安 (Release ビルド、1 枠、ぼかしと広がりが既定の 0.1): 名前の大きさ 27 px (1080p の 2.5%) で約 5 ms、
// 92 px (同じく 8.5%) で約 20 ms。ぼかしの計算は標準偏差に比例して増え、92 px でぼかし 0.75 と広がり 0.5 なら約 80 ms、
// どちらも範囲の上の端 (3.0 と 1.0) なら約 0.5 秒になる。内容と大きさが変わったときだけ呼び、描画のたびには呼ばない。
CompositeLabel paint_composite_label(const LabelContent &content, const CompositeLabelLook &look, double name_px,
				     double margin_px, int cell_w, int cell_h);

// 枠 (セル) の内側に向かうグロー。CSS の box-shadow の inset 0 0 blur spread と同じ形で、セルの外側が光る元になり、
// 広がり spread_px の分だけ内側へ入り、ぼかし blur_px (標準偏差はその半分) で内側へ薄れる。最大の明るさで描いた
// cell_w x cell_h の画像 (乗算済みアルファ)。argb のアルファも掛ける。大きさが 0 以下なら空の画像を返す。1920 x 1080 で
// 約 25 ms (Release ビルド)。縦横の値を erf で求めるので、ぼかしの大きさには依らない。
QImage paint_frame_glow(int cell_w, int cell_h, std::uint32_t argb, double blur_px, double spread_px);

} // namespace cordmux::sources
