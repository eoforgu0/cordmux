// SPDX-License-Identifier: Apache-2.0

// 名前ラベルの中のアバターの位置と、画面共有していない参加者の並べ方の選択肢。settings.json から読む値の定義で、
// 設定 (settings.h) と、名前ラベルを描く側 (sources/) の両方が使う。選択肢だけの小さなヘッダーにしてあるのは、描く側が
// JSON のライブラリを含めずに、この選択肢を使えるようにするため。標準ライブラリにも依存しない。

#pragma once

namespace cordmux::config {

// 名前ラベルの中の、アバターを置く側。値の書き方は "left" と "right"。右にすると、名前ラベルの中の並びが左右入れ替わる
// (アバターが右端、名前がその左)。画面共有している参加者の名前ラベル (label_avatar_side) と、画面共有していない参加者の
// 並び (roster_avatar_side。縦並びの列の中の揃えも右になる) で、別々に選ぶ。
enum class AvatarSide { Left, Right };

// 名前ラベルのアバターの形。値の書き方は "circle" (丸)、"rounded" (角丸)、"square" (四角)。画面共有している参加者の名前ラベルと、
// 画面共有していない参加者の並びで同じ形にする (label_avatar_shape の 1 つ)。角丸の角の半径は、アバターの直径の 1/4。
enum class AvatarShape { Circle, Rounded, Square };

// 画面共有している参加者の名前ラベルを、映像の枠のどの隅に置くか。全員一律の 1 つの値 (参加者ごとには持たない)。
// 値の書き方は "tl" "tr" "bl" "br" (上下が t b、左右が l r)。
enum class LabelCorner { TopLeft, TopRight, BottomLeft, BottomRight };

// 画面共有していない参加者を寄せる位置 (キャンバスの 8 か所。中央は選べない)。値の書き方は "tl" "tc" "tr" "ml" "mr" "bl"
// "bc" "br" (上中下が t m b、左中右が l c r)。
enum class RosterAnchor {
	TopLeft,
	TopCenter,
	TopRight,
	MiddleLeft,
	MiddleRight,
	BottomLeft,
	BottomCenter,
	BottomRight
};

// 画面共有していない参加者を並べる向き。Row は横に並べて入りきらなければ次の行へ折り返す横並び、Column は縦に並べて
// 入りきらなければ次の列へ移る縦並び。値の書き方は "row" と "column"。
enum class RosterDirection { Row, Column };

// 位置に合う向き。上と下の中央は横並びだけ、左と右の中央は縦並びだけ、四隅はどちらでもよい (非常識な組を除くため)。
// 合わない組は、位置に合う向きに修正する。
inline RosterDirection roster_direction_for(RosterAnchor anchor, RosterDirection direction)
{
	switch (anchor) {
	case RosterAnchor::TopCenter:
	case RosterAnchor::BottomCenter:
		return RosterDirection::Row;
	case RosterAnchor::MiddleLeft:
	case RosterAnchor::MiddleRight:
		return RosterDirection::Column;
	default:
		return direction;
	}
}

} // namespace cordmux::config
