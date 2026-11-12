// SPDX-License-Identifier: Apache-2.0

// 名前ラベルの調整ツールの、つまみごとの見本 (--sweep)。1 つのプロセスの中で、つまみを 1 つずつ下の端、既定、上の端へ
// 利用者の操作と同じ経路 (スライダー、数の欄、チェックボックス、色の欄、コンボボックス) で動かし、そのたびにウィンドウを
// 画像に書き出す。続けて動かすので、つまみを変えたのに描き直さない (古い画が残る) 誤りが、その場で描いた画像との違い
// (TunerWindow::fresh_mismatch) に出る。時間のつまみは、話し始めと止まりの後の決まった時刻で書き出す。

#pragma once

#include <cstdint>
#include <functional>

#include <QJsonObject>
#include <QString>

namespace cordmux::label_tuner {

class TunerWindow;

// 現在のウィンドウを path (PNG) と同じ名前の .json に書き、extra を JSON に足す関数。書けたら true。
using ShotWriter = std::function<bool(const QString &path, const QJsonObject &extra)>;

// *virtual_now は window の時刻 (ns) で、進めながら実行する。refresh は描き直しを終えてウィンドウの表示を整える関数。
// 書き出す先は dir の <番号>_<キー>_<段>.png。どの段もその場で描いた画像と同じなら 0、違う段があれば 1、書けなければ
// 2 を返す。
int run_knob_sweep(TunerWindow &window, std::uint64_t *virtual_now, const QString &dir,
		   const std::function<void()> &refresh, const ShotWriter &write_shot);

} // namespace cordmux::label_tuner
