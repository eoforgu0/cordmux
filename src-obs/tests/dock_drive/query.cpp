// SPDX-License-Identifier: Apache-2.0

// query.h の実装。

#include "query.h"

#include <QScrollArea>
#include <QScrollBar>
#include <QWidget>

#include <algorithm>
#include <utility>

#include "dock/dock_widget.h"
#include "dock/first_line.h"
#include "dock/notice_list.h"
#include "dock/people_list.h"
#include "dock/person_row.h"
#include "dock/ui/text_resolve.h"

namespace cordmux::dock_drive {

std::string user_of(const QWidget *w)
{
	for (const QWidget *p = w; p != nullptr; p = p->parentWidget()) {
		const QVariant v = p->property("userId");
		if (v.isValid() && !v.toString().isEmpty())
			return v.toString().toStdString();
	}
	return std::string();
}

QString part_of(const QWidget *w)
{
	for (const QWidget *p = w; p != nullptr; p = p->parentWidget()) {
		const QString name = p->objectName();
		if (name == QLatin1String("cordmuxRowCheck"))
			return QStringLiteral("check");
		if (name == QLatin1String("cordmuxRowMenuButton"))
			return QStringLiteral("dots");
		if (name == QLatin1String("cordmuxRow"))
			return QStringLiteral("row");
	}
	return QString();
}

QRect global_rect(const QWidget *w)
{
	if (w == nullptr)
		return QRect();
	return QRect(w->mapToGlobal(QPoint(0, 0)), w->size());
}

QRect check_hit_rect_global(dock::PeopleList *list, const std::string &user_id)
{
	if (list == nullptr)
		return QRect();
	dock::PersonRowWidget *row = list->row_for(user_id);
	if (row == nullptr || row->check() == nullptr || !row->check()->isVisible())
		return QRect();
	const QRect r = row->check()->focus_hit_rect();
	return QRect(row->check()->mapToGlobal(r.topLeft()), r.size());
}

QRect dots_rect_global(dock::PeopleList *list, const std::string &user_id)
{
	if (list == nullptr)
		return QRect();
	dock::PersonRowWidget *row = list->row_for(user_id);
	if (row == nullptr || row->dots() == nullptr || !row->dots()->isVisible())
		return QRect();
	return global_rect(row->dots());
}

QString first_line_full_text(dock::DockWidget *widget)
{
	auto *first = dynamic_cast<dock::FirstLineBar *>(widget->findChild<QWidget *>(QStringLiteral("cordmuxFirstLine")));
	return first != nullptr ? first->full_text() : QString();
}

QString first_line_expected(const ledger::dockview::DockViewV2 &view)
{
	if (view.first_line.forms.empty())
		return QString();
	return dock::ui::resolve(view.first_line.forms.front());
}

QStringList shown_notice_keys(dock::DockWidget *widget)
{
	// 画面の y の順 (上から順) に並べる。
	std::vector<std::pair<int, QString>> ordered;
	for (QWidget *c : widget->findChildren<QWidget *>()) {
		auto *card = dynamic_cast<dock::NoticeCard *>(c);
		if (card == nullptr || !card->isVisibleTo(widget))
			continue;
		ordered.push_back({card->mapTo(widget, QPoint(0, 0)).y(), QString::fromStdString(card->notice().key)});
	}
	std::stable_sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
	QStringList keys;
	for (const auto &[y, key] : ordered) {
		(void)y;
		keys << key;
	}
	return keys;
}

QStringList expected_notice_keys(const ledger::dockview::DockViewV2 &view)
{
	QStringList keys;
	for (const ledger::dockview::Notice &n : view.notices)
		keys << QString::fromStdString(n.key);
	return keys;
}

std::vector<ledger::dockview::PersonRow> listed_rows(const ledger::dockview::DockViewV2 &view)
{
	std::vector<ledger::dockview::PersonRow> rows;
	if (view.people_list.pinned.has_value())
		rows.push_back(*view.people_list.pinned);
	rows.insert(rows.end(), view.people_list.sharing.begin(), view.people_list.sharing.end());
	rows.insert(rows.end(), view.people_list.in_call.begin(), view.people_list.in_call.end());
	return rows;
}

int scroll_value(dock::DockWidget *widget)
{
	auto *scroll = widget->findChild<QScrollArea *>(QStringLiteral("cordmuxScroll"));
	return scroll != nullptr ? scroll->verticalScrollBar()->value() : 0;
}

} // namespace cordmux::dock_drive
