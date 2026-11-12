// SPDX-License-Identifier: Apache-2.0

// surface_frame.h の実装。

#include "dock/ui/surface_frame.h"

#include "dock/ui/set_class.h"

namespace cordmux::dock::ui {

namespace {

constexpr const char *kColorPart = "color";

} // namespace

void paint_on_surface(QWidget *w, const QColor &foreground)
{
	if (w == nullptr || !foreground.isValid())
		return;
	// 計算した後の色だけを書く (テーマの var() は、ウィジェットのスタイルシートでは展開されないため)。
	set_own_style(w, kColorPart, QStringLiteral("color: %1;").arg(foreground.name(QColor::HexRgb)));
}

void clear_surface_paint(QWidget *w)
{
	set_own_style(w, kColorPart, QString());
}

} // namespace cordmux::dock::ui
