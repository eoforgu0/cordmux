// SPDX-License-Identifier: Apache-2.0

// decor_tests の中で共有する、画像を期待する画像と比べる関数とテスト用のアバター。期待する画像は
// src-obs/tests/snapshots の PNG で、失敗したときは、期待、実際、差分の 3 つの画像を書き出す。期待する画像を作り直す
// 実行かどうかは decor_test_env.h の decor_snapshot_update が決める。

#pragma once

#include <QImage>
#include <QRect>

namespace cordmux::tests {

// テスト用のアバター。ピクセルをテストの中で作るので、画像のファイルを別に持たなくてよい。
QImage test_avatar();

// 画像が、snapshots の name.png と 1 ピクセルも違わず一致するか。期待する画像を作り直す実行では、画像を保存して
// 保存できたかを返す。
bool snapshot_matches(const QImage &actual, const char *name);

// 描画された範囲 (透明でないピクセルを囲む最小の矩形)。何も描画されていなければ空の矩形。位置のテストに使う。
QRect ink_bounds(const QImage &image);

} // namespace cordmux::tests
