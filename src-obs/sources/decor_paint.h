// SPDX-License-Identifier: Apache-2.0

// 名前ラベルの描画で共有する型と、フォントの候補と、ダミーの背景の画像。libobs に依存しない。名前ラベルの画像そのものは
// 合成のソースが composite_label.h の関数で描く。

#pragma once

#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include <QFont>
#include <QFontInfo>
#include <QImage>
#include <QString>

#include "config/label_placement.h"

namespace cordmux::sources {

// 名前ラベルを置く角。上下と左右の組み合わせで、映像の矩形の大きさが変わっても、角からの余白で位置が決まる。
// 既定値は左下。
enum class DecorCorner { TopLeft, TopRight, BottomLeft, BottomRight };

// 設定の名前ラベルの隅 (全員一律の label_corner) を、描画の隅に変換する。config/ と sources/ の型を分けてあるので、
// 変換はこの 1 か所だけにまとめる。
inline DecorCorner decor_corner_of(config::LabelCorner corner)
{
	switch (corner) {
	case config::LabelCorner::TopLeft:
		return DecorCorner::TopLeft;
	case config::LabelCorner::TopRight:
		return DecorCorner::TopRight;
	case config::LabelCorner::BottomRight:
		return DecorCorner::BottomRight;
	case config::LabelCorner::BottomLeft:
		break;
	}
	return DecorCorner::BottomLeft;
}

// フォントの設定が空のときに使うフォントの候補。先にあるものから順に探し、その PC にある最初のものを使う
// (UD デジタル 教科書体 NK-B は Windows の任意の機能の日本語の補助フォントに入るので、ない PC がある。最後の
// Yu Gothic UI は Windows の UI の日本語のゴシック体)。UD デジタル 教科書体 NK-B は、英語の名前
// (UD Digi Kyokasho NK-B) で並ぶ PC があるので、その名前も入れる。名前ラベルとダミーの番号が使う
// (decor_paint.cpp の apply_default_family)。設定の既定値は空のままにする (設定ファイルに OS ごとのフォントの
// 名前を書かない)。
inline constexpr const char *kDefaultLabelFontFamilies[] = {"UD デジタル 教科書体 NK-B", "UD Digi Kyokasho NK-B",
							    "Yu Gothic UI"};

// フォントの設定が空のときに実際に描くフォントの名前。kDefaultLabelFontFamilies のうち、その PC にある最初のもの
// (どれもなければ最後のもの)。設定の画面とテストが「(既定) <名前>」に出す。
inline QString default_label_font_family()
{
	// QFontInfo は、その名前のフォントがあればその名前を、なければ代わりのフォントの名前を返す。
	for (const char *name : kDefaultLabelFontFamilies) {
		const QString family = QString::fromUtf8(name);
		if (QFontInfo(QFont(family)).family().compare(family, Qt::CaseInsensitive) == 0)
			return family;
	}
	return QString::fromUtf8(kDefaultLabelFontFamilies[std::size(kDefaultLabelFontFamilies) - 1]);
}

// 1 つの名前ラベルに描画する内容。
struct LabelContent {
	std::string name; // 描画する名前 (UTF-8)。読めなければ空にする (ID などの代わりの文字列を入れない)
	QImage avatar;    // 空なら描画しない (代わりの画像を作らない)
	DecorCorner corner = DecorCorner::BottomLeft; // 参加者ごとの角
	// 同じシーンに見えているほかの人の名前の全文。名前を省略するときに、ほかの人と見分けられるように、真ん中を
	// 省略するか、アバターを残すかを決めるのに使う。
	std::vector<std::string> other_visible_names;
};

// ---- ダミーの背景の画像 ----

// ダミーの枠の背景。落ち着いた単色 (dummy_tiles.h の dummy_tile_color) に、枠の番号を大きく (高さの 0.42 倍の
// 太字、少し透けた白で) 描画した画像。動く線は重ねない (フレームごとに dummy_tiles.h の move_dummy_line が
// 重ねる)。枠ごと・大きさごとに 1 回だけ描画する。返す形式は Format_ARGB32_Premultiplied の不透明な画像で、
// リトルエンディアンの環境では、ピクセルの並びが BGRA になる (dummy_tiles.h の dummy_nv12_from_bgra が 1 回だけ NV12 にする)。
QImage paint_dummy_background(int slot, int width, int height);

} // namespace cordmux::sources
