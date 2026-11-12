// SPDX-License-Identifier: Apache-2.0

// type_scale.h の実装。

#include "dock/ui/type_scale.h"

#include <QWidget>

#include "dock/ui/set_class.h"

namespace cordmux::dock::ui {

namespace {

constexpr const char *kFontPart = "font";

} // namespace

QString font_size_css(double base_pt, double scale)
{
	const double pt = (base_pt > 0.0 ? base_pt : 10.0) * scale;
	return QStringLiteral("font-size: %1pt;").arg(QString::number(pt, 'f', 2));
}

void apply_type(QWidget *target, TypeLevel level, double base_pt, double scale)
{
	if (target == nullptr)
		return;
	switch (level) {
	case TypeLevel::FirstLineHeading:
		set_own_style(target, kFontPart, font_size_css(base_pt, scale) + QStringLiteral(" font-weight: bold;"));
		return;
	case TypeLevel::FirstLineSummary:
		set_own_style(target, kFontPart, font_size_css(base_pt, scale) + QStringLiteral(" font-weight: normal;"));
		return;
	case TypeLevel::GroupHeading:
		set_own_style(target, kFontPart,
			      (scale > 1.0 ? font_size_css(base_pt, scale) + QStringLiteral(" ") : QString()) +
				      QStringLiteral("font-weight: bold;"));
		return;
	case TypeLevel::Heading:
		// 太さだけ。大きさはテーマの本文のまま。
		set_own_style(target, kFontPart, QStringLiteral("font-weight: bold;"));
		return;
	case TypeLevel::Body:
		set_own_style(target, kFontPart, QString());
		return;
	}
}

void apply_scale(QWidget *target, double base_pt, double scale)
{
	if (target == nullptr)
		return;
	set_own_style(target, kFontPart, scale > 1.0 ? font_size_css(base_pt, scale) : QString());
}

} // namespace cordmux::dock::ui
