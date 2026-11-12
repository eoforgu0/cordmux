// SPDX-License-Identifier: Apache-2.0

// 変更の後、少し待ってから 1 回だけ保存する (deferred_save.h) を実装する。

#include "dock/deferred_save.h"

#include <QTimer>

namespace cordmux::dock {

DeferredSave::DeferredSave(int delay_ms, std::function<void()> save, QObject *parent)
	: QObject(parent), save_(std::move(save))
{
	timer_ = new QTimer(this);
	timer_->setSingleShot(true);
	timer_->setInterval(delay_ms);
	QObject::connect(timer_, &QTimer::timeout, this, [this]() { flush(); });
}

DeferredSave::~DeferredSave() = default;

void DeferredSave::touch()
{
	pending_ = true;
	timer_->start();
}

void DeferredSave::flush()
{
	timer_->stop();
	if (!pending_)
		return;
	pending_ = false;
	if (save_)
		save_();
}

} // namespace cordmux::dock
