// SPDX-License-Identifier: Apache-2.0

// 名前ラベルの調整ツールを、画面と同じ経路で動かして確認する (--drive)。描き直しのスレッド、重ねる仕事のスレッド、
// 16 ms ごとのタイマー、実際の時計をそのまま使い、決めた手順を利用者の操作と同じ入口 (スライダー、数の欄、
// コンボボックス、チェックボックスとボタンへのマウスの押下と離し) から数十 ms の間隔で入れる。ウィンドウは
// プレビュー用のプラットフォームプラグインの上に作るので、OS のウィンドウは開かない。

#pragma once

#include <cstdint>
#include <functional>

#include <QJsonObject>
#include <QString>

namespace cordmux::label_tuner {

class TunerWindow;

// 手順を入れ終えてから、描き終えるのを待つ時間の上限 (ms)。
inline constexpr int kDriveSettleLimitMs = 5000;

// window を動かし、イベントループが終わったときの終了コードを返す。確認が全部通れば 0、通らなければ 1。
// clock は window と同じ時計。write_shot は、終わりのウィンドウを path (PNG) と同じ名前の .json に書き、extra を
// 足す関数。結果は dir の drive.json と drive.png に書く。
int run_drive(TunerWindow &window, const QString &dir, const std::function<std::uint64_t()> &clock,
	      const std::function<bool(const QString &path, const QJsonObject &extra)> &write_shot);

} // namespace cordmux::label_tuner
