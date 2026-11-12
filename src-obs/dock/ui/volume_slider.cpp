// SPDX-License-Identifier: Apache-2.0

// 音量のつまみ (volume_slider.h) を実装する。

#include "dock/ui/volume_slider.h"

#include <QSignalBlocker>
#include <QTimer>

namespace cordmux::dock::ui {

VolumeSlider::VolumeSlider(QWidget *parent) : NoWheelSlider(Qt::Horizontal, parent)
{
	setRange(config::kPersonVolumeMinPct, config::kPersonVolumeMaxPct);
	setSingleStep(1);
	setPageStep(slider_page_step(config::kPersonVolumeMaxPct - config::kPersonVolumeMinPct));
	set_reset_value(kResetPct);
	// ダブルクリックで戻した値は、間引かずにすぐ送る。
	set_on_reset([this] { flush(); });

	timer_ = new QTimer(this);
	timer_->setSingleShot(true);
	timer_->setInterval(kSendIntervalMs);
	QObject::connect(timer_, &QTimer::timeout, this, [this] {
		if (pending_ != sent_) {
			send_now();
			timer_->start();
		}
	});
	QObject::connect(this, &QSlider::valueChanged, this, [this](int v) {
		pending_ = v;
		if (!timer_->isActive()) {
			send_now();
			timer_->start();
		}
	});
	QObject::connect(this, &QSlider::sliderReleased, this, [this] { flush(); });
}

void VolumeSlider::flush()
{
	timer_->stop();
	send_now();
}

void VolumeSlider::set_initial(int pct)
{
	const QSignalBlocker block(this);
	setValue(pct);
	pending_ = sent_ = value();
}

bool VolumeSlider::sync(int pct)
{
	const auto now = std::chrono::steady_clock::now();
	if (isSliderDown() || timer_->isActive() || pending_ != sent_)
		return false;
	if (sent_at_.time_since_epoch().count() != 0 &&
	    std::chrono::duration_cast<std::chrono::milliseconds>(now - sent_at_).count() < kSyncHoldMs && pct != sent_)
		return false;
	if (value() == pct)
		return false;
	const QSignalBlocker block(this);
	setValue(pct);
	pending_ = sent_ = value();
	return true;
}

void VolumeSlider::send_now()
{
	if (pending_ == sent_)
		return;
	sent_ = pending_;
	sent_at_ = std::chrono::steady_clock::now();
	if (send_)
		send_(sent_);
}

} // namespace cordmux::dock::ui
