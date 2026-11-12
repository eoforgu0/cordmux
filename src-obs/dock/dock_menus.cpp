// SPDX-License-Identifier: Apache-2.0

// ドックのメニューを作る (dock_menus.h の実装)。

#include "dock/dock_menus.h"

#include <QAction>
#include <QActionGroup>
#include <QMenu>
#include <QString>

#include "dock/ui/text_resolve.h"

namespace cordmux::dock {

namespace {

// View の 1 つのボタンを、メニューの 1 つの項目にする。押せないボタンは項目も押せなくし、理由をツールチップに表示する。
QAction *add_item(QMenu *menu, const ledger::dockview::ViewButton &button, const ActionCallback &on_action)
{
	QAction *action = menu->addAction(ui::resolve(button.label));
	if (button.disabled_reason.has_value()) {
		action->setEnabled(false);
		action->setToolTip(ui::resolve(*button.disabled_reason));
	}
	const ledger::ActionId id = button.id;
	const ledger::ActionArgs args = button.args;
	QObject::connect(action, &QAction::triggered, menu, [on_action, id, args]() {
		if (on_action)
			on_action(id, args);
	});
	return action;
}

} // namespace

QMenu *make_dock_menu(QWidget *parent, const ledger::dockview::DockMenuView &view, ActionCallback on_action)
{
	auto *menu = new QMenu(parent);
	menu->setObjectName(QStringLiteral("cordmuxDockMenu"));
	// QMenu は既定では項目のツールチップを表示しないので、押せない理由を表示するために有効にする。
	menu->setToolTipsVisible(true);
	add_item(menu, view.open_settings, on_action);
	add_item(menu, view.open_participants, on_action);
	add_item(menu, view.export_bundle, on_action);
	menu->addSeparator();
	add_item(menu, view.feature_toggle, on_action);
	return menu;
}

QMenu *make_main_account_menu(QWidget *parent, const ledger::dockview::MainAccountView &view, ActionCallback on_action)
{
	auto *menu = new QMenu(parent);
	menu->setObjectName(QStringLiteral("cordmuxMainAccountMenu"));
	menu->setToolTipsVisible(true);
	auto *group = new QActionGroup(menu);
	group->setExclusive(true);
	const auto add = [&](const ledger::dockview::MainAccountChoice &choice) {
		QAction *action = menu->addAction(ui::resolve_label(choice.label));
		action->setCheckable(true);
		action->setChecked(choice.checked);
		action->setProperty("userId", QString::fromStdString(choice.action.args.user_id));
		group->addAction(action);
		if (choice.action.disabled_reason.has_value()) {
			action->setEnabled(false);
			action->setToolTip(ui::resolve(*choice.action.disabled_reason));
		}
		const bool already = choice.checked;
		const ledger::ActionId id = choice.action.id;
		const ledger::ActionArgs args = choice.action.args;
		QObject::connect(action, &QAction::triggered, menu, [on_action, id, args, already]() {
			if (!already && on_action)
				on_action(id, args);
		});
	};
	bool in_call_part = true;
	for (const ledger::dockview::MainAccountChoice &choice : view.choices) {
		if (in_call_part && !choice.in_call && !menu->isEmpty())
			menu->addSeparator();
		in_call_part = choice.in_call;
		add(choice);
	}
	if (!menu->isEmpty())
		menu->addSeparator();
	add(view.no_main);
	return menu;
}

} // namespace cordmux::dock
