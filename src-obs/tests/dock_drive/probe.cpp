// SPDX-License-Identifier: Apache-2.0

// probe.h の実装。

#include "probe.h"

#include <QApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QMenu>
#include <QStyleHints>
#include <QWidget>
#include <QWindow>

#include <memory>

#include "dock/dock_widget.h"
#include "dock/people_list.h"
#include "dock/person_row.h"
#include "input.h"
#include "obs_dock.h"
#include "query.h"
#include "script.h"
#include "sim_poll.h"

namespace cordmux::dock_drive {

namespace {

const QString kGroup = QStringLiteral("probe");

// 枠に載っている (チェックが入っていて押せる) 最初の行の人。
std::string first_slotted(SimPoll &sim)
{
	for (const ledger::dockview::PersonRow &row : sim.view().people) {
		if (row.checkable && row.checked && row.check_enabled)
			return row.user_id;
	}
	return std::string();
}

} // namespace

void run_probe(const dock::ui::DockStyle &style, Results &results)
{
	SimPoll sim(ui_preview::fixture_by_id("people-8"), style);
	sim.dock()->setGeometry(100, 100, 420, 700);
	sim.dock()->show();
	Input input;
	input.activate(sim.dock());
	sim.start();
	Script s;
	std::string target;
	std::size_t actions_before = 0;
	bool menu_seen = false;

	s.then(800, [&] {
		target = first_slotted(sim);
		results.expect_true(kGroup, QStringLiteral("テストデータ people-8 に、枠に載っていて押せる行がある"), !target.empty());
		const QRect r = check_hit_rect_global(sim.people(), target);
		results.expect_true(kGroup, QStringLiteral("行のチェックの押せる範囲を取得できる"), r.isValid());
		input.move_to(r.center());
	});
	s.then(50, [&] {
		results.expect_true(kGroup, QStringLiteral("QCursor::pos が、動かした位置を返す"), QCursor::pos() == input.pos(),
				    QStringLiteral("cursor=%1,%2").arg(QCursor::pos().x()).arg(QCursor::pos().y()));
		QWidget *under = Input::widget_at(input.pos());
		results.expect_true(kGroup, QStringLiteral("ポインターの下のウィジェットが、その人の行にある"), user_of(under) == target,
				    QStringLiteral("under=%1 part=%2")
					    .arg(under != nullptr ? QString::fromLatin1(under->metaObject()->className()) : QString(),
						 part_of(under)));
		results.expect_true(kGroup, QStringLiteral("スクロール領域の上で、並びの保留が始まる"), sim.widget()->layout_held());
	});
	s.then(700, [&] {
		actions_before = sim.actions().size();
		input.click();
	});
	s.then(50, [&] {
		const bool got = sim.actions().size() == actions_before + 1;
		results.expect_true(kGroup, QStringLiteral("クリックで操作が 1 つ届く"), got);
		if (got) {
			const ActionRecord &a = sim.actions().back();
			results.expect_true(kGroup, QStringLiteral("届いた操作が、押した人の SetSlot"),
					    a.id == ledger::ActionId::SetSlot && a.args.user_id == target);
		}
		input.move_to(QPoint(1800, 900));
	});
	s.then(50, [&] { results.expect_true(kGroup, QStringLiteral("ドックの外へ出ると、並びの保留が終わる"), !sim.widget()->layout_held()); });
	// キーボード: フォーカスをその人のチェックに置く (Tab で移す代わりに、ウィジェットに直接置く。Tab の順は
	// keyboard のシナリオで確認する)。
	s.then(600, [&] {
		dock::PersonRowWidget *row = sim.people() != nullptr ? sim.people()->row_for(target) : nullptr;
		if (row != nullptr && row->check() != nullptr)
			row->check()->setFocus(Qt::TabFocusReason);
		actions_before = sim.actions().size();
		QWindow *fw = QGuiApplication::focusWindow();
		QWidget *fwid = QApplication::focusWidget();
		results.info(kGroup, QStringLiteral("Space を押す前のフォーカス"),
			     QStringLiteral("window=%1 widget=%2 active=%3")
				     .arg(fw != nullptr ? fw->objectName() + QStringLiteral("/") + fw->title() : QStringLiteral("(none)"),
					  fwid != nullptr ? fwid->objectName() : QStringLiteral("(none)"))
				     .arg(sim.dock()->isActiveWindow() ? 1 : 0));
		input.key(Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
	});
	s.then(50, [&] {
		const bool got = sim.actions().size() == actions_before + 1 && sim.actions().back().args.user_id == target;
		results.expect_true(kGroup, QStringLiteral("Space で、フォーカスのある人の操作が届く"), got);
	});
	// 右クリックで行のメニューが開き、Esc で閉じる。
	s.then_blocking(600, [&] {
		const QRect r = check_hit_rect_global(sim.people(), target);
		input.move_to(r.center());
		menu_seen = false;
		input.press(Qt::RightButton);
		input.release(Qt::RightButton); // メニューが exec で開くと、ここは閉じるまで戻らない
	});
	s.then(300, [&] {
		QWidget *popup = QApplication::activePopupWidget();
		menu_seen = qobject_cast<QMenu *>(popup) != nullptr;
		results.expect_true(kGroup, QStringLiteral("右クリックで行のメニューが開く"), menu_seen,
				    popup != nullptr ? popup->objectName() : QStringLiteral("(none)"));
		input.key(Qt::Key_Escape);
	});
	s.then(200, [&] {
		results.expect_true(kGroup, QStringLiteral("Esc でメニューが閉じる"), QApplication::activePopupWidget() == nullptr);
		dock::PersonRowWidget *row = sim.people() != nullptr ? sim.people()->row_for(target) : nullptr;
		if (row != nullptr && row->check() != nullptr)
			row->check()->setFocus(Qt::TabFocusReason);
		input.move_to(QPoint(1800, 900));
		// Menu キーのキーイベントだけを送る (Windows のプラットフォームプラグインが、コンテキストメニューの
		// イベントを作る前の段階)。
		input.key(Qt::Key_Menu);
	});
	s.then(300, [&] {
		const bool opened = QApplication::activePopupWidget() != nullptr;
		results.info(kGroup, QStringLiteral("Menu キーのキーイベントだけでメニューが開くか"),
			     opened ? QStringLiteral("yes") : QStringLiteral("no"),
			     QStringLiteral("開かなければ、Windows のプラットフォームプラグインの WM_CONTEXTMENU の処理を再現する必要がある (input.cpp)"));
		if (opened)
			input.key(Qt::Key_Escape);
	});
	s.then_blocking(200, [&] {
		QWidget *fw = QApplication::focusWidget();
		results.info(kGroup, QStringLiteral("コンテキストメニューのキーを押す前のフォーカス"),
			     fw != nullptr ? fw->objectName() + QStringLiteral(" policy=") + QString::number(fw->contextMenuPolicy())
					   : QStringLiteral("(none)"));
		input.context_menu_key();
	});
	s.then(300, [&] {
		QWidget *popup = QApplication::activePopupWidget();
		results.expect_true(kGroup, QStringLiteral("キーボードからのコンテキストメニューで行のメニューが開く"),
				    qobject_cast<QMenu *>(popup) != nullptr,
				    QStringLiteral("accepted=%1").arg(input.last_context_accepted_ ? 1 : 0));
		if (popup != nullptr)
			input.key(Qt::Key_Escape);
	});
	// 押せないチェック (枠がすべて使用中で、枠に載っていない人) の上を右クリックしても、その人の行のメニューが開く
	// ことを確認する。行のどこを右クリックしても行のメニューを開き、押せないのはチェックだけで、[...] とメニューは
	// 使える。
	auto disabled_user = std::make_shared<std::string>();
	s.then(200, [&, disabled_user] {
		for (const ledger::dockview::PersonRow &row : sim.view().people) {
			if (row.checkable && !row.check_enabled && disabled_user->empty())
				*disabled_user = row.user_id;
		}
		dock::PersonRowWidget *row = sim.people() != nullptr ? sim.people()->row_for(*disabled_user) : nullptr;
		if (row != nullptr && row->check() != nullptr)
			input.move_to(row->check()->mapToGlobal(row->check()->rect().center()));
	});
	s.then_blocking(700, [&] {
		input.press(Qt::RightButton);
		input.release(Qt::RightButton);
	});
	s.then(300, [&, disabled_user] {
		QWidget *popup = QApplication::activePopupWidget();
		results.expect_true(kGroup, QStringLiteral("押せないチェックの上の右クリックでも行のメニューが開く"),
				    qobject_cast<QMenu *>(popup) != nullptr && popup->property("userId").toString().toStdString() == *disabled_user,
				    QStringLiteral("人 %1").arg(QString::fromStdString(*disabled_user).right(2)));
		if (popup != nullptr)
			input.key(Qt::Key_Escape);
	});
	s.then(200, [&] {
		results.info(kGroup, QStringLiteral("ダブルクリックの時間 (ms)"),
			     QString::number(QGuiApplication::styleHints()->mouseDoubleClickInterval()));
	});
	const bool finished = s.run(60000);
	results.expect_true(kGroup, QStringLiteral("手順が時間内に終わった"), finished);
	sim.stop();
}

} // namespace cordmux::dock_drive
