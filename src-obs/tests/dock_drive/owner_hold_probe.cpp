// SPDX-License-Identifier: Apache-2.0

// owner_hold_probe.h の実装。

#include "owner_hold_probe.h"

#include <QApplication>
#include <QPushButton>
#include <QTimer>
#include <QVariant>
#include <QWidget>
#include <QWindow>

#include <QtGui/private/qguiapplication_p.h>

#include "dock/dock_widget.h"
#include "drive_env.h"
#include "input.h"
#include "query.h"
#include "scen_common.h"
#include "sim_poll.h"

namespace cordmux::dock_drive {

bool qt_window_blocked(QWidget *w)
{
	if (w == nullptr || w->windowHandle() == nullptr)
		return false;
	return QGuiApplicationPrivate::instance()->isWindowBlocked(w->windowHandle());
}

QWidget *find_message_dialog(QWidget *owner)
{
	QWidget *found = nullptr;
	for (QWidget *w : QApplication::topLevelWidgets()) {
		if (w->isVisible() && w->objectName() == QLatin1String("cordmuxMessageDialog") &&
		    (owner == nullptr || w->parentWidget() == owner))
			found = w; // 後から開いたウィンドウが後ろに並ぶ (同じ親に 2 つ重なったときは上のウィンドウ)
	}
	return found;
}

QString blocking_name_of(const QWidget *dialog)
{
	return dialog != nullptr ? dialog->property("messageBlocking").toString() : QString();
}

MainWindowProbe::MainWindowProbe()
{
	QWidget *main = main_window();
	if (main == nullptr)
		return;
	button_ = new QPushButton(QStringLiteral("probe"), main);
	button_->setObjectName(QStringLiteral("driveMainWindowProbe"));
	button_->setGeometry(40, main->height() - 80, 160, 32);
	button_->show();
	button_->raise();
	QObject::connect(button_.data(), &QPushButton::clicked, button_.data(), [this] { ++clicks_; });
}

MainWindowProbe::~MainWindowProbe()
{
	if (!button_.isNull())
		delete button_.data();
}

bool MainWindowProbe::click_works(Input &input)
{
	if (button_.isNull())
		return false;
	const int before = clicks_;
	input.move_to(button_->mapToGlobal(button_->rect().center()));
	pump(100);
	input.click();
	pump(200);
	return clicks_ > before;
}

bool dock_row_click_works(Input &input, SimPoll &sim, QString *note)
{
	QRect target;
	for (const ledger::dockview::PersonRow &row : sim.view().people) {
		if (row.checkable && row.check_enabled) {
			target = check_hit_rect_global(sim.people(), row.user_id);
			break;
		}
	}
	if (target.isEmpty()) {
		// 押せる行がない (アンインストールの後など): ドックの [...] を押してメニューが開くかで確認する
		// (開いたら閉じる)。
		auto *menu_button = sim.widget()->findChild<QPushButton *>(QStringLiteral("cordmuxDockMenuButton"));
		if (menu_button == nullptr) {
			if (note != nullptr)
				*note = QStringLiteral("押せる行も [...] もない");
			return false;
		}
		input.move_to(menu_button->mapToGlobal(menu_button->rect().center()));
		pump(600);
		QTimer::singleShot(300, [] {
			if (QWidget *popup = QApplication::activePopupWidget())
				popup->close();
		});
		bool opened = false;
		QTimer::singleShot(150, [&opened] { opened = QApplication::activePopupWidget() != nullptr; });
		input.click(); // メニューが開くと、閉じるまで戻らない (上の 300 ms のタイマーが閉じる)
		pump(400);
		if (note != nullptr)
			*note = QStringLiteral("押せる行がないので [...] を押した: メニュー %1").arg(opened ? QStringLiteral("開いた") : QStringLiteral("開かない"));
		return opened;
	}
	const std::size_t before = sim.actions().size();
	input.move_to(target.center());
	pump(600); // クリックを捨てるガードの時間の外
	input.click();
	pump(200);
	const bool ran = sim.actions().size() > before;
	if (note != nullptr) {
		QWindow *w = Input::window_at(target.center());
		*note = QStringLiteral("押した位置のウィンドウ %1").arg(w != nullptr ? w->objectName() : QStringLiteral("(none)"));
	}
	if (ran) {
		// 押した分を戻す (同じ人をもう一度押す)。
		pump(600);
		input.click();
		pump(200);
	}
	return ran;
}

} // namespace cordmux::dock_drive
