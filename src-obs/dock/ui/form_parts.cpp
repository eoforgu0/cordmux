// SPDX-License-Identifier: Apache-2.0

// 入力欄を並べる関数 (form_parts.h) を実装する。

#include "dock/ui/form_parts.h"

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QSizePolicy>
#include <QSpacerItem>
#include <QVBoxLayout>

#include <algorithm>

#include "dock/ui/wrap_label.h"

namespace cordmux::dock::ui {

QFormLayout *make_form()
{
	auto *form = new QFormLayout();
	form->setRowWrapPolicy(QFormLayout::WrapLongRows);
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	form->setLabelAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
	return form;
}

QLabel *add_form_row(QFormLayout *form, const QString &label_text, QWidget *field, QWidget *buddy)
{
	auto *label = new QLabel(label_text);
	label->setTextFormat(Qt::PlainText);
	label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
	label->setBuddy(buddy != nullptr ? buddy : field);
	form->addRow(label, field);
	return label;
}

void add_note_row(QFormLayout *form, QWidget *note)
{
	const int row = form->rowCount();
	form->setItem(row, QFormLayout::LabelRole, new QSpacerItem(0, 0));
	form->setWidget(row, QFormLayout::FieldRole, note);
	form->invalidate();
}

WrapLabel *wrap_label(const QString &text)
{
	return new WrapLabel(text);
}

QWidget *note_box(QWidget *note)
{
	auto *box = new QWidget();
	auto *v = new QVBoxLayout(box);
	v->setContentsMargins(0, 0, 0, 0);
	v->setSpacing(0);
	v->addWidget(note);
	return box;
}

void set_note_box_margins(QWidget *box, int left_px, int bottom_px)
{
	if (box != nullptr && box->layout() != nullptr)
		box->layout()->setContentsMargins(std::max(0, left_px), 0, 0, std::max(0, bottom_px));
}

void set_combo_data(QComboBox *combo, const QVariant &value)
{
	const int index = combo->findData(value);
	if (index >= 0)
		combo->setCurrentIndex(index);
}

} // namespace cordmux::dock::ui
