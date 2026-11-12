// SPDX-License-Identifier: Apache-2.0

// record_button_base.h の実装。

#include "dock/ui/record_button_base.h"

#include <QEvent>
#include <QFontMetrics>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStylePainter>

#include <algorithm>

namespace cordmux::dock::ui {

RecordButtonBase::RecordButtonBase(QWidget *parent) : QPushButton(parent)
{
	setCheckable(true);
	setChecked(false);
	setAutoDefault(false);
	setDefault(false);
}

void RecordButtonBase::nextCheckState()
{
	// 何もしない。checked は set_recording_look だけが変える。
}

void RecordButtonBase::set_recording_look(bool on)
{
	if (isChecked() == on)
		return;
	setChecked(on);
}

void RecordButtonBase::set_pressed_look_while_disabled(bool on)
{
	if (pressed_look_ == on)
		return;
	pressed_look_ = on;
	update();
}

void RecordButtonBase::set_display_text(const QString &text)
{
	if (display_text_ == text)
		return;
	display_text_ = text;
	update();
}

void RecordButtonBase::set_width_labels(const QStringList &labels)
{
	if (width_labels_ == labels)
		return;
	width_labels_ = labels;
	recompute_width();
}

int RecordButtonBase::width_for(const QString &label) const
{
	// QPushButton::sizeHint と同じ計算を、テキストだけを替えて行う (Qt の qpushbutton.cpp の sizeHint)。
	QStyleOptionButton option;
	initStyleOption(&option);
	option.text = label;
	const QFontMetrics fm = fontMetrics();
	const QSize text = fm.size(Qt::TextShowMnemonic, label.isEmpty() ? QStringLiteral("XXXX") : label);
	int w = text.width();
	int h = text.height();
	if (!icon().isNull()) {
		const int ih = option.iconSize.height();
		const int iw = option.iconSize.width() + 4;
		w += iw;
		h = std::max(h, ih);
	}
	option.rect.setSize(QSize(w, h));
	return style()->sizeFromContents(QStyle::CT_PushButton, &option, QSize(w, h), this).width();
}

void RecordButtonBase::recompute_width()
{
	int widest = 0;
	for (const QString &label : width_labels_)
		widest = std::max(widest, width_for(label));
	if (widest_ == widest)
		return;
	widest_ = widest;
	updateGeometry();
}

QSize RecordButtonBase::sizeHint() const
{
	QSize s = QPushButton::sizeHint();
	s.setWidth(std::max(s.width(), widest_));
	return s;
}

QSize RecordButtonBase::minimumSizeHint() const
{
	QSize s = QPushButton::minimumSizeHint();
	s.setWidth(std::max(s.width(), widest_));
	return s;
}

QRect RecordButtonBase::hit_rect() const
{
	QStyleOptionButton option;
	initStyleOption(&option);
	return style()->subElementRect(QStyle::SE_PushButtonBevel, &option, this);
}

void RecordButtonBase::paintEvent(QPaintEvent *)
{
	// QPushButton::paintEvent と同じ描き方。違うのは 2 つ。見えるテキストがあれば、テキストの代わりに描くこと
	// (set_display_text)。押された見た目を保つときだけ、State_Enabled を付けてスタイルに渡すこと (スタイルは
	// :disabled を QStyleOption の state から決め、ウィジェットの isEnabled() は見ない)。
	QStylePainter painter(this);
	QStyleOptionButton option;
	initStyleOption(&option);
	if (!display_text_.isEmpty())
		option.text = display_text_;
	if (pressed_look_ && !isEnabled() && isChecked()) {
		option.state |= QStyle::State_Enabled;
		option.palette.setCurrentColorGroup(QPalette::Active);
	}
	painter.drawControl(QStyle::CE_PushButton, option);
}

void RecordButtonBase::changeEvent(QEvent *event)
{
	QPushButton::changeEvent(event);
	switch (event->type()) {
	case QEvent::FontChange:
	case QEvent::StyleChange:
		recompute_width();
		break;
	default:
		break;
	}
}

} // namespace cordmux::dock::ui
