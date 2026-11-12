// SPDX-License-Identifier: Apache-2.0

// 参加者の行の [...] のメニュー (row_menu.h) を実装する。

#include "dock/row_menu.h"

#include <QAction>
#include <QActionGroup>
#include <QMenu>
#include <QString>
#include <QVariant>
#include <QWidget>

#include "dock/ui/status_icons.h"
#include "dock/ui/text_resolve.h"
#include "ledger/text_ids.h"

namespace cordmux::dock {

namespace {

namespace keys = ledger::text::keys;

// 項目のテキスト。View のボタンのテキストを使い、View がテキストを持たないときだけ、その項目の ini のキーから読む。
QString item_text(const ledger::TextRef &label, const char *fallback_key)
{
	if (!label.id.empty())
		return ui::resolve(label);
	return ui::resolve_key(fallback_key);
}

void disable_all(QMenu *menu)
{
	for (QAction *action : menu->actions()) {
		if (action->isSeparator())
			continue;
		if (QMenu *sub = action->menu())
			disable_all(sub);
		action->setEnabled(false);
	}
}

} // namespace

QMenu *make_row_menu(QWidget *parent, const ledger::dockview::PersonRow &row, ActionCallback on_action,
		     std::function<void(const std::string &)> on_rename)
{
	const ledger::dockview::RowMenuView &m = row.menu;
	const QString user_id = QString::fromStdString(row.user_id);
	auto *menu = new QMenu(parent);
	menu->setObjectName(QStringLiteral("cordmuxRowMenu"));
	menu->setProperty("userId", user_id);
	// 押せない項目の長い理由と、項目の説明をツールチップに表示する。
	menu->setToolTipsVisible(true);

	// 「自動で載せる」(チェックの付く項目)。
	QAction *auto_add = menu->addAction(item_text(m.always_on_toggle.label, keys::kDockRowAutoAddMenu));
	auto_add->setObjectName(QStringLiteral("cordmuxRowAutoAdd"));
	auto_add->setProperty("userId", user_id);
	auto_add->setCheckable(true);
	auto_add->setChecked(m.always_on);
	if (m.always_on_toggle.disabled_reason) {
		auto_add->setEnabled(false);
		auto_add->setToolTip(ui::resolve(*m.always_on_toggle.disabled_reason));
	} else {
		auto_add->setToolTip(ui::resolve_key(keys::kDockRowAutoAddTip));
	}
	{
		const ledger::ActionId id = m.always_on_toggle.id;
		const ledger::ActionArgs args = m.always_on_toggle.args;
		QObject::connect(auto_add, &QAction::triggered, menu, [on_action, id, args] {
			if (on_action)
				on_action(id, args);
		});
	}

	// 「全画面で表示」(チェックの付く項目)。この行の人を全画面で表示している間はチェックが付き、押すと元の配置に戻す。
	// 押せない行でも項目は出し、押せなくする。押せないときは、[自動で載せる] と同じく理由をツールチップに表示する。
	QAction *focus = menu->addAction(item_text(m.focus_toggle.label, keys::kDockRowFullScreenMenu));
	focus->setObjectName(QStringLiteral("cordmuxRowFullScreen"));
	focus->setProperty("userId", user_id);
	focus->setCheckable(true);
	focus->setChecked(m.focus);
	focus->setEnabled(!m.focus_toggle.disabled_reason.has_value());
	if (m.focus_toggle.disabled_reason)
		focus->setToolTip(ui::resolve(*m.focus_toggle.disabled_reason));
	{
		const ledger::ActionId id = m.focus_toggle.id;
		const ledger::ActionArgs args = m.focus_toggle.args;
		QObject::connect(focus, &QAction::triggered, menu, [on_action, id, args] {
			if (on_action)
				on_action(id, args);
		});
	}

	// 受信側に届かない人 (サブアカウントがない構成の自分の行) には、[自動で載せる] と [全画面で表示] だけを出す。
	if (m.auto_add_only)
		return menu;

	// 「名前を変更...」。ダイアログを開くので「...」を付ける。ダイアログ (rename_dialog.h) は呼び出し元が開く。
	QAction *rename = menu->addAction(item_text(m.rename.label, keys::kDockRowRenameMenu));
	rename->setObjectName(QStringLiteral("cordmuxRowRename"));
	rename->setProperty("userId", user_id);
	if (m.rename.disabled_reason) {
		rename->setEnabled(false);
		rename->setToolTip(ui::resolve(*m.rename.disabled_reason));
	}
	{
		const std::string uid = row.user_id;
		QObject::connect(rename, &QAction::triggered, menu, [on_rename, uid] {
			if (on_rename)
				on_rename(uid);
		});
	}

	// 「使う声」のサブメニュー。1 つだけを選べるグループにする。
	QMenu *voices = menu->addMenu(ui::resolve_key(keys::kDockRowVoiceFromMenu));
	voices->setObjectName(QStringLiteral("cordmuxRowVoiceMenu"));
	voices->setProperty("userId", user_id);
	voices->setToolTipsVisible(true);
	voices->menuAction()->setToolTip(ui::resolve_key(keys::kDockRowVoiceFromTip));
	// 理由が候補の全員で同じとき (この行の人の声をほかの行で使っていて、現在の値が本人の声のとき) は、「使う声」の項目
	// そのものを押せなくする。テキストは短い理由を括弧で 1 回だけ添えたもの (cordmux.dock.row_voice_from_lent.menu)
	// にし、長い理由をツールチップにする。開けない項目なので、サブメニューの項目は作らない。候補を名前だけの押せない
	// 項目にすると押せない理由がどこにも表示されず、候補ごとに括弧を付けると同じ長い文が人数分並ぶ。
	// サブメニューの矢印は残す。押せないサブメニューの項目を矢印ごと薄く描くのは Qt と Windows のメニューの標準の形で、
	// どの行のメニューでも同じ位置に同じ形の「使う声」が来る。
	if (m.voices_disabled_label && m.voices_disabled_reason) {
		QAction *item = voices->menuAction();
		item->setText(ui::resolve(*m.voices_disabled_label));
		// Qt のメニューの項目は、スクリーンリーダーに名前 (項目のテキスト) だけを渡し、説明を持たない (Qt 6.8.3 の
		// qaccessiblemenu.cpp の QAccessibleMenuItem::text は Name と Accelerator だけを返す)。短い理由は項目の
		// テキストの括弧にあるので、名前として読まれる。
		item->setToolTip(ui::resolve(*m.voices_disabled_reason));
		item->setEnabled(false);
		return menu;
	}
	auto *group = new QActionGroup(voices);
	group->setExclusive(true);
	for (std::size_t i = 0; i < m.voices.size(); ++i) {
		const ledger::dockview::VoiceChoice &choice = m.voices[i];
		// 先頭の「本人の声」と、通話に参加している人の名前の間に区切り線を入れる。
		if (i == 1)
			voices->addSeparator();
		// 項目のテキストは View の label のままにする。押せない項目の label は、名前に短い理由を括弧で添えた ini の
		// 1 つの文字列 (「bob (既に「alice」の行で使っています)」) で、ledger が作る。長い理由 (disabled_reason) は
		// 項目のテキストにすると項目が文になるので、ツールチップに表示する。参加者の設定ウィンドウも同じ分け方に
		// している。
		QAction *action = voices->addAction(ui::resolve_label(choice.label));
		if (choice.action.disabled_reason)
			action->setToolTip(ui::resolve(*choice.action.disabled_reason));
		action->setObjectName(QStringLiteral("cordmuxRowVoice"));
		action->setProperty("userId", user_id);
		action->setProperty("voiceUserId", QString::fromStdString(choice.action.args.voice_user_id));
		action->setCheckable(true);
		action->setChecked(choice.checked);
		action->setEnabled(!choice.action.disabled_reason.has_value());
		group->addAction(action);
		const bool already = choice.checked;
		const ledger::ActionId id = choice.action.id;
		const ledger::ActionArgs args = choice.action.args;
		QObject::connect(action, &QAction::triggered, menu, [on_action, id, args, already] {
			// 現在の値をもう 1 度選んでも何も変えない。
			if (!already && on_action)
				on_action(id, args);
		});
	}
	if (m.voices.empty())
		voices->menuAction()->setEnabled(false);
	return menu;
}

QMenu *make_replace_menu(QWidget *parent, const ledger::dockview::PersonRow &row, const QColor &color, int icon_px,
			 ActionCallback on_action)
{
	auto *menu = new QMenu(parent);
	menu->setObjectName(QStringLiteral("cordmuxReplaceMenu"));
	menu->setProperty("userId", QString::fromStdString(row.user_id));
	QAction *heading = menu->addAction(ui::resolve_key(keys::kDockReplaceWithMenu));
	heading->setEnabled(false);
	menu->addSeparator();
	for (const ledger::dockview::ReplaceChoice &choice : row.replace_choices) {
		QAction *action = menu->addAction(ui::resolve_label(choice.name));
		action->setProperty("userId", QString::fromStdString(choice.action.args.replace_user_id));
		ui::SlotMark mark = ui::SlotMark::None;
		switch (choice.position) {
		case ledger::dockview::SlotPosition::None:
			break;
		case ledger::dockview::SlotPosition::TopLeft:
			mark = ui::SlotMark::TopLeft;
			break;
		case ledger::dockview::SlotPosition::TopRight:
			mark = ui::SlotMark::TopRight;
			break;
		case ledger::dockview::SlotPosition::BottomLeft:
			mark = ui::SlotMark::BottomLeft;
			break;
		case ledger::dockview::SlotPosition::BottomRight:
			mark = ui::SlotMark::BottomRight;
			break;
		case ledger::dockview::SlotPosition::Full:
			mark = ui::SlotMark::Full;
			break;
		}
		QIcon icon;
		for (const qreal dpr : {1.0, 1.25, 1.5, 2.0})
			icon.addPixmap(ui::slot_mark_pixmap(mark, icon_px, color, dpr));
		action->setIcon(icon);
		if (choice.action.disabled_reason.has_value()) {
			action->setEnabled(false);
			action->setToolTip(ui::resolve(*choice.action.disabled_reason));
		}
		const ledger::ActionId id = choice.action.id;
		const ledger::ActionArgs args = choice.action.args;
		QObject::connect(action, &QAction::triggered, menu, [on_action, id, args] {
			if (on_action)
				on_action(id, args);
		});
	}
	menu->setToolTipsVisible(true);
	return menu;
}

void refresh_row_menu(QMenu *menu, const ledger::dockview::PersonRow *row)
{
	if (menu == nullptr)
		return;
	if (row == nullptr) {
		disable_all(menu);
		return;
	}
	// 開いている間にその人を描かなくなったら (画面共有をやめたときなど)、[全画面で表示] を押せなくする。押せるようには
	// 戻さない (「使う声」の項目と同じ)。チェックと押した後の向きは、開いたときのまま変えない。
	for (QAction *action : menu->actions()) {
		if (action->objectName() == QLatin1String("cordmuxRowFullScreen") &&
		    row->menu.focus_toggle.disabled_reason.has_value())
			action->setEnabled(false);
	}
	QMenu *voices = menu->findChild<QMenu *>(QStringLiteral("cordmuxRowVoiceMenu"));
	if (voices == nullptr)
		return;
	for (QAction *action : voices->actions()) {
		if (action->isSeparator())
			continue;
		const std::string voice = action->property("voiceUserId").toString().toStdString();
		bool present = false;
		for (const ledger::dockview::VoiceChoice &choice : row->menu.voices) {
			if (choice.action.args.voice_user_id == voice) {
				present = true;
				break;
			}
		}
		if (!present)
			action->setEnabled(false);
	}
}

} // namespace cordmux::dock
