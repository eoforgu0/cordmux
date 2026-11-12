// SPDX-License-Identifier: Apache-2.0

// mistap のシナリオ。テストデータ people-8 の状態 (人の出入り、名前の変更、画面共有の開始と終了、メッセージの表示と消去) を
// 乱数で変えながら、ポインターの移動、クリック、メニューの開閉、キーの入力を重ね、押し間違いを数える
// (1 回 10 分、シード 5 つ)。数えるもの (すべてのシードで 0 が合格) と数え方は、mistap.cpp の Mistap クラスのコメントに
// ある。

#pragma once

#include <QString>

#include "dock/ui/dock_style.h"
#include "report.h"

namespace cordmux::dock_drive {

struct MistapOptions {
	int seed = 1;
	double minutes = 10.0;
	QString out_dir; // シードごとの記録 (操作と、見つけた押し間違いの行) を書くフォルダー
	dock::ui::DockStyle style;
};

// 1 つのシードで実行し、結果を results に加える。
void run_mistap(const MistapOptions &options, Results &results);

} // namespace cordmux::dock_drive
