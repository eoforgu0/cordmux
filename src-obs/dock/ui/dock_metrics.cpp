// SPDX-License-Identifier: Apache-2.0

// dock_metrics.h のうち Qt に依存する部分。ウィジェットから本文のフォントの大きさを読む。

#include "dock/ui/dock_metrics.h"

#include <QFont>
#include <QWidget>

#include "dock/ui/theme_read.h"

namespace cordmux::dock::ui {

double base_point_size(QWidget *root)
{
	if (root == nullptr)
		return 10.0;
	root->ensurePolished();
	const QFont f = root->font();
	if (f.pointSizeF() > 0.0)
		return f.pointSizeF();
	// テーマがフォントの大きさを px で指定しているとき (QSS の font-size: 13px など) は pt に換算する。
	if (f.pixelSize() > 0) {
		const double dpi = root->logicalDpiY() > 0 ? root->logicalDpiY() : 96.0;
		return f.pixelSize() * 72.0 / dpi;
	}
	return 10.0;
}

DockMetrics read_dock_metrics(QWidget *root, const ThemeRead &theme, const DockStyle &style)
{
	const double dpi = root != nullptr && root->logicalDpiY() > 0 ? root->logicalDpiY() : 96.0;
	return compute_dock_metrics(base_point_size(root), dpi, theme.icon_px, style, theme.list_text_left_px);
}

} // namespace cordmux::dock::ui
