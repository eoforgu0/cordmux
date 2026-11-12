// SPDX-License-Identifier: Apache-2.0

// ドックのタブ (dock_tabs.h) を実装する。

#include "dock/dock_tabs.h"

#include <QFontMetrics>
#include <QFrame>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>

#include "dock/ui/text_resolve.h"
#include "ledger/text_ids.h"

namespace cordmux::dock {

DockTabBar::DockTabBar(QWidget *parent) : QTabBar(parent)
{
	setObjectName(QStringLiteral("cordmuxTabs"));
	setExpanding(true);
	setDrawBase(false);
	setUsesScrollButtons(false);
	setElideMode(Qt::ElideNone);
	setDocumentMode(true);
}

void DockTabBar::set_widest_text(const QString &text)
{
	if (widest_text_ == text)
		return;
	widest_text_ = text;
	updateGeometry();
}

QSize DockTabBar::tabSizeHint(int index) const
{
	QSize hint = QTabBar::tabSizeHint(index);
	// タブの枠の分 (Qt が足した幅) をそのまま使い、テキストの幅だけをいちばん長いテキストに替える。
	const QFontMetrics fm = fontMetrics();
	int text_px = fm.horizontalAdvance(widest_text_);
	for (int i = 0; i < count(); ++i)
		text_px = std::max(text_px, fm.horizontalAdvance(tabText(i)));
	hint.setWidth(hint.width() - fm.horizontalAdvance(tabText(index)) + text_px);
	return hint;
}

QSize DockTabBar::minimumTabSizeHint(int index) const
{
	return tabSizeHint(index);
}

void DockTabBar::set_side_padding(int px)
{
	setStyleSheet(QStringLiteral("QTabBar::tab { min-width: 0px; padding-left: %1px; padding-right: %1px; }").arg(px));
}

int DockTabBar::minimum_bar_width_px() const
{
	return count() > 0 ? count() * tabSizeHint(0).width() : 0;
}

void DockTabBar::wheelEvent(QWheelEvent *event)
{
	event->ignore();
}

ScrollBox::ScrollBox(QWidget *parent) : QScrollArea(parent)
{
	setWidgetResizable(true);
	setFrameShape(QFrame::NoFrame);
	setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
	// スクロール領域はタブストップにする。タブの中は、押せるウィジェットのないカードとテキストのラベルだけになることがある。
	// タブストップでないと、キーボードで操作する人は、そのテキストを下までスクロールできない。スクロール領域にフォーカスが
	// あれば、上下の矢印キーと PageUp、PageDown でスクロールする。
	setFocusPolicy(Qt::StrongFocus);
	// タブストップで読み上げる名前は、ドックのタイトル (cordmux.dock.window.title) にする。Qt 6.8.3 は名前のないスクロール領域に
	// 空の名前を返し、役割は Client なので、Windows の読み上げは、Tab で止まったときに「グループ」とだけ言う
	// (qaccessiblewidget.cpp、qaccessiblewidgetfactory.cpp、qwindowsuiautils.cpp)。参加者の設定ウィンドウと設定ウィンドウも、
	// スクロール領域をウィンドウのタイトルで名付けている。
	setAccessibleName(ui::resolve_key(ledger::text::keys::kDockWindowTitle));
	// 最小の高さを、Qt の既定 (スクロールバーの推奨の長さ) にしない。既定のままだと、OBS の下のドックを押し上げる。
	setMinimumHeight(1);
	viewport()->setAutoFillBackground(false);
}

void ScrollBox::set_ring_reach(int reach)
{
	reach_ = std::max(0, reach);
}

QRect ScrollBox::focus_hit_rect() const
{
	return rect().adjusted(reach_, reach_, -reach_, -reach_);
}

DockPage::DockPage(const QString &scroll_name, QWidget *parent) : QWidget(parent)
{
	layout_ = new QVBoxLayout(this);
	layout_->setContentsMargins(0, 0, 0, 0);
	layout_->setSpacing(0);
	scroll_ = new ScrollBox(this);
	scroll_->setObjectName(scroll_name);
	layout_->addWidget(scroll_, 1);
}

void DockPage::set_header(QWidget *widget)
{
	header_ = widget;
	layout_->insertWidget(0, widget);
}

void DockPage::set_content(QWidget *widget)
{
	widget->setAutoFillBackground(false);
	scroll_->setWidget(widget);
}

} // namespace cordmux::dock
