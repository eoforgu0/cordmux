// SPDX-License-Identifier: Apache-2.0

// プレビューの画像と、OBS の実際の画面のスクリーンショットを比べる関数。プレビューが OBS の画面にどこまで似ているかを
// 数値で見るために使う。スクリーンショットは、OBS のフローティングのドックのウィンドウを撮ったものを前提にする。
// 左右と下に 8 px の見えない枠線 (黒) が、上に 31 px のタイトルバーがあり、中身の OBSDock はその内側にある。その内側を
// 切り出し、同じ大きさで描画したプレビューとピクセルごとに比べる。

#pragma once

#include <QImage>
#include <QRect>
#include <QString>

namespace cordmux::ui_preview {

// スクリーンショットのウィンドウの枠線とタイトルバーの大きさ (px。実際のスクリーンショットで測った値)。
inline constexpr int kShotBorder = 8;
inline constexpr int kShotTitle = 31;

// スクリーンショットから中身を切り出す。
QImage crop_client(const QImage &shot);

struct Comparison {
	double mean_abs = 0.0;      // ピクセルごと・チャンネルごとの差の平均 (0 から 255)
	double over16_ratio = 0.0;  // どれかのチャンネルで 16 を超えて違うピクセルの割合
	int width = 0;
	int height = 0;
};

// 2 枚の画像を比べる (大きさが違えば重なる所だけ)。mask の外は数えない (空ならすべてを数える)。
Comparison compare_images(const QImage &a, const QImage &b, const QRect &mask = QRect());

// 左にスクリーンショット、真ん中にプレビュー、右に差 (差の大きい所ほど明るい) を並べた 1 枚の画像。
QImage side_by_side(const QImage &real, const QImage &render);

} // namespace cordmux::ui_preview
