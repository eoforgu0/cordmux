// SPDX-License-Identifier: Apache-2.0

// 押した場所の隣の確認 (inline_confirm.h) を実装する。

#include "dock/inline_confirm.h"

#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QPushButton>
#include <QTimer>

#include <algorithm>

#include "dock/layout_hold.h"
#include "dock/ui/dock_ui_calc.h"
#include "dock/ui/text_resolve.h"
#include "ledger/text_ids.h"

namespace cordmux::dock {

InlineConfirm::InlineConfirm(QWidget *parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("cordmuxInlineConfirm"));
	confirm_ = new QPushButton(this);
	confirm_->setObjectName(QStringLiteral("cordmuxConfirmButton"));
	cancel_ = new QPushButton(this);
	cancel_->setObjectName(QStringLiteral("cordmuxConfirmCancel"));
	for (QPushButton *b : {confirm_, cancel_}) {
		b->setAutoDefault(false);
		b->setFocusPolicy(Qt::TabFocus);
		new PressGuard(b);
		b->installEventFilter(this);
	}
	timer_ = new QTimer(this);
	timer_->setSingleShot(true);
	timer_->setInterval(kInlineConfirmTimeoutMs);
	QObject::connect(timer_, &QTimer::timeout, this, [this] { cancel(); });
	QObject::connect(cancel_, &QPushButton::clicked, this, [this] { cancel(); });
	QObject::connect(confirm_, &QPushButton::clicked, this, [this] {
		std::function<void()> run = on_confirm_;
		finish();
		if (run)
			run();
	});
	hide();
}

void InlineConfirm::set_look(const DockLook &look)
{
	look_ = look;
}

int InlineConfirm::pair_width_px(const QString &confirm_label, const QString &cancel_label) const
{
	QPushButton probe(const_cast<InlineConfirm *>(this));
	probe.hide();
	probe.setText(confirm_label);
	probe.ensurePolished();
	const int confirm_w = probe.sizeHint().width();
	probe.setText(cancel_label);
	return ui::calc::confirm_pair_width(confirm_w, probe.sizeHint().width(), look_.metrics.small_px);
}

void InlineConfirm::start(const QRect &place, const QString &confirm_label, const QString &cancel_label,
			  bool focus_to_cancel, std::function<void()> on_confirm, std::function<void()> on_end)
{
	cancel();
	confirm_->setText(confirm_label);
	cancel_->setText(cancel_label);
	confirm_->ensurePolished();
	cancel_->ensurePolished();
	const int gap = look_.metrics.small_px;
	QWidget *host = parentWidget();
	const QPoint pointer = host->mapFromGlobal(QCursor::pos());
	const bool pointer_in_row = pointer.y() >= place.top() && pointer.y() <= place.bottom();
	const ui::calc::ConfirmLayout layout = ui::calc::confirm_layout(
		place.left(), place.width(), confirm_->sizeHint().width(), cancel_->sizeHint().width(), gap,
		pointer_in_row, pointer.x());
	setGeometry(layout.left, place.top(), layout.width, place.height());
	QPushButton *left = layout.cancel_on_left ? cancel_ : confirm_;
	QPushButton *right = layout.cancel_on_left ? confirm_ : cancel_;
	const int left_width = layout.cancel_on_left ? layout.cancel_width : layout.confirm_width;
	left->setGeometry(0, 0, left_width, place.height());
	right->setGeometry(left_width + gap, 0, layout.width - left_width - gap, place.height());
	QWidget::setTabOrder(left, right);
	on_confirm_ = std::move(on_confirm);
	on_end_ = std::move(on_end);
	active_ = true;
	focus_back_ = QApplication::focusWidget();
	PressGuard::arm_widget(confirm_);
	PressGuard::arm_widget(cancel_);
	raise();
	show();
	if (focus_to_cancel)
		cancel_->setFocus(Qt::TabFocusReason);
	timer_->start();
}

void InlineConfirm::finish()
{
	if (!active_)
		return;
	active_ = false;
	timer_->stop();
	const bool had_focus = isAncestorOf(QApplication::focusWidget());
	hide();
	std::function<void()> end = std::move(on_end_);
	on_end_ = nullptr;
	on_confirm_ = nullptr;
	if (end)
		end();
	// キーボードで操作していたら、フォーカスを元のウィジェットに戻す。隠したウィジェットのフォーカスを Qt が次へ移すと、続けて
	// 押した Space が別の操作になるためである。
	if (had_focus && !focus_back_.isNull() && focus_back_->isVisible() && focus_back_->isEnabled())
		focus_back_->setFocus(Qt::OtherFocusReason);
	focus_back_.clear();
}

void InlineConfirm::cancel()
{
	finish();
}

bool InlineConfirm::eventFilter(QObject *watched, QEvent *event)
{
	// ボタンの上でポインターが動いている間とキーの入力の間は、迷っている最中なので、時間を数え直す。
	if (active_ && (event->type() == QEvent::Enter || event->type() == QEvent::HoverMove ||
			event->type() == QEvent::MouseMove || event->type() == QEvent::KeyPress))
		timer_->start();
	return QWidget::eventFilter(watched, event);
}

} // namespace cordmux::dock
