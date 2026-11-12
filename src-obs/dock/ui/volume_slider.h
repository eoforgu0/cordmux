// SPDX-License-Identifier: Apache-2.0

// 音量のつまみ (VolumeSlider)。0 から 200% を 1% 刻みで動かし、動かした値を操作として送る側へ間引いて渡す。
// 外から届く値 (次の周期の View) で、動かしている最中のつまみを書き換えない。ホイール、フォーカス、キーの扱いと、ダブル
// クリックで既定値 (100%) に戻す動きは、ドックのつまみの共通の部品 (no_wheel.h の NoWheelSlider) のとおり。

#pragma once

#include "config/person_audio.h"
#include "dock/ui/no_wheel.h"

#include <chrono>
#include <functional>

class QTimer;

namespace cordmux::dock::ui {

class VolumeSlider : public NoWheelSlider {
public:
	explicit VolumeSlider(QWidget *parent = nullptr);

	// 動かした値を送る関数。最初の値はすぐ、その後は最後の値を kSendIntervalMs ごとにまとめて送る。つまみを離したときと flush() は、
	// まだ送っていない最後の値を必ず送る。ダブルクリックで 100% に戻したときも、すぐ送る。キーで動かした値も同じに送る。
	void set_send(std::function<void(int)> send) { send_ = std::move(send); }
	// まだ送っていない最後の値を送る。
	void flush();
	// 最初の値を入れる。送らず、valueChanged も出さない。
	void set_initial(int pct);
	// 次の周期の View の値を反映する。動かしている最中と、送っていない値がある間と、最後に送ってから kSyncHoldMs の間は、送った値
	// 以外の値で書き換えない (送った値が View に戻るまで、古い値でつまみが戻らないようにする)。値を書き換えたときは true を返す。
	// valueChanged は出さない。
	bool sync(int pct);

	// 動かす間に操作を送る間合い (ms)。動かすたびに全部送ると、ドックの処理が値ごとに走る。
	static constexpr int kSendIntervalMs = 50;
	// 最後に送ってから、次の周期の値でつまみを書き換えない時間 (ms)。
	static constexpr int kSyncHoldMs = 1500;
	// ダブルクリックで戻す値 (%)。参加者ごとの音量の既定値。
	static constexpr int kResetPct = config::kPersonVolumeDefaultPct;

private:
	void send_now();

	std::function<void(int)> send_;
	QTimer *timer_ = nullptr;
	int pending_ = kResetPct;
	int sent_ = kResetPct;
	std::chrono::steady_clock::time_point sent_at_{};
};

} // namespace cordmux::dock::ui
