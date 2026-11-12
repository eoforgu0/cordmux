// SPDX-License-Identifier: Apache-2.0

// 名前ラベルの描画 (decor_paint.cpp と composite_label.cpp) が共有する関数。文字と画像の扱いを 1 か所にまとめて、
// 2 つの描画で文字の大きさ、フォント、アバターの切り抜き、名前の省略の判定が食い違わないようにする。libobs に
// 依存しない。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QPainterPath>
#include <QRectF>
#include <QString>

#include "config/label_placement.h"

namespace cordmux::sources::paint_common {

// 0xAARRGGBB の色。
QColor color_from_argb(std::uint32_t value);

// 乗算済みアルファの透明な画像。描画する面の解像度は 96 dpi に固定する。文字はポイントで指定するので、面の dpi が
// 違うと同じ入力から違う大きさの文字が出る (1 pt = 96 / 72 ピクセル)。幅と高さは最低 1 にする。
QImage make_surface(int width, int height);

// 文字を測るための 96 dpi の面 (1 x 1。描画しない)。QFontMetricsF を作るときに渡す。面を通さずに測ると、Qt は
// その PC の画面の dpi でポイントをピクセルに修正するので、OBS の中では画面の拡大率によって文字の大きさが変わる。
const QImage &measure_surface();

// 名前ラベルのフォント。family が空なら kDefaultLabelFontFamilies。em_px はピクセルでの文字の大きさ。ヒンティングは
// 無効にする (有効だと、描画する仕組みのバージョンによって形がずれ、同じ入力から同じピクセルを返せなくなる)。
QFont make_label_font(const std::string &family, int weight, bool italic, double em_px);

// フォントの設定が空のときに使うフォントを QFont に設定する (ダミーの番号が使う)。
void apply_default_family(QFont &font);

// アバターの形 (rect に収まる円、角丸の四角、四角) の輪郭。角丸の角の半径は、rect の短い辺の 1/4。
QPainterPath avatar_shape_path(const QRectF &rect, config::AvatarShape shape);

// アバターを一辺 diameter の形 (avatar_shape_path) で切り抜く。縦横比は保ったまま短い辺を合わせ、中央を切り取る。
// source が空か diameter が 0 以下なら空の画像を返す。
QImage clip_avatar_to_shape(const QImage &source, int diameter, config::AvatarShape shape);

// 見えている文字 (省略記号を除いた部分)。
QString visible_part(const QString &drawn);

// 書記素 (見た目の 1 文字。結合した絵文字も 1 文字) の数。
int grapheme_count(const QString &text);

// 末尾を省略した名前の見えている先頭が、ほかの人の名前の先頭にも一致するか。
bool prefix_collides(const QString &visible_head, const std::vector<std::string> &others);

// 同じ名前の人がほかにも見えているか。
bool same_name_visible(const std::string &name, const std::vector<std::string> &others);

// 文字の縦の位置 (ベースライン)。フォントが「国」を持てば、その文字列面の矩形の中心を center_y に合わせる。持たない
// フォントでは、大文字の高さの中心を合わせる。
double baseline_for(const QFont &font, const QFontMetricsF &metrics, double center_y);

} // namespace cordmux::sources::paint_common
