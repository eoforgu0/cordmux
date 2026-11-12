// SPDX-License-Identifier: Apache-2.0

// ドックの上部のメインとサブの行 (accounts_bar.h) を実装する。

#include "dock/accounts_bar.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

#include <algorithm>

#include "dock/layout_hold.h"
#include "dock/ui/dock_ui_calc.h"
#include "dock/ui/elide_label.h"
#include "dock/ui/set_class.h"
#include "dock/ui/text_resolve.h"
#include "dock/ui/theme_read.h"
#include "ledger/text_ids.h"

namespace cordmux::dock {

namespace {

namespace keys = ledger::text::keys;

QPushButton *make_button(const QString &text, const QString &object_name, QWidget *parent)
{
	auto *b = new QPushButton(text, parent);
	b->setObjectName(object_name);
	b->setAutoDefault(false);
	// クリックではフォーカスを取らない。OBS が前面のまま Space を押して、同じ操作が働くのを防ぐ。
	b->setFocusPolicy(Qt::TabFocus);
	QSizePolicy policy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	policy.setRetainSizeWhenHidden(true);
	b->setSizePolicy(policy);
	new PressGuard(b);
	return b;
}

ui::ElideLabel *make_text(const QString &object_name, QWidget *parent)
{
	auto *label = new ui::ElideLabel(parent);
	label->setObjectName(object_name);
	return label;
}

} // namespace

AccountsBar::AccountsBar(QWidget *parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("cordmuxAccounts"));
	grid_ = new QGridLayout(this);
	main_key_ = new QLabel(ui::resolve_key(keys::kDockAccountMainLabel), this);
	sub_key_ = new QLabel(ui::resolve_key(keys::kDockAccountSubLabel), this);
	main_name_ = make_text(QStringLiteral("cordmuxAccountMainName"), this);
	main_call_ = make_text(QStringLiteral("cordmuxAccountMainCall"), this);
	sub_name_ = make_text(QStringLiteral("cordmuxAccountSubName"), this);
	sub_call_ = make_text(QStringLiteral("cordmuxAccountSubCall"), this);
	change_ = make_button(ui::resolve_key(keys::kDockAccountChangeButton), QStringLiteral("cordmuxAccountChange"), this);
	sub_buttons_ = new QWidget(this);
	auto *sub_layout = new QHBoxLayout(sub_buttons_);
	sub_layout->setContentsMargins(0, 0, 0, 0);
	move_ = make_button(ui::resolve_key(keys::kDockAccountMoveButton), QStringLiteral("cordmuxAccountMove"), sub_buttons_);
	leave_ = make_button(ui::resolve_key(keys::kDockAccountLeaveButton), QStringLiteral("cordmuxAccountLeave"), sub_buttons_);
	sub_layout->addWidget(move_);
	sub_layout->addWidget(leave_);

	grid_->addWidget(main_key_, 0, 0);
	grid_->addWidget(main_name_, 0, 1);
	grid_->addWidget(main_call_, 0, 2);
	grid_->addWidget(change_, 0, 3, Qt::AlignRight);
	grid_->addWidget(sub_key_, 1, 0);
	grid_->addWidget(sub_name_, 1, 1);
	grid_->addWidget(sub_call_, 1, 2);
	grid_->addWidget(sub_buttons_, 1, 3, Qt::AlignRight);
	// 名前と通話の名前は、どちらが長いかが決まっていないので、同じ幅にする。
	grid_->setColumnStretch(1, 1);
	grid_->setColumnStretch(2, 1);

	for (QPushButton *button : {change_, move_, leave_}) {
		QObject::connect(button, &QPushButton::clicked, this, [this, button] {
			if (!has_view_ || !on_press_)
				return;
			if (button == change_)
				on_press_(view_.main.change, button);
			else if (view_.sub.has_value())
				on_press_(button == move_ ? view_.sub->move : view_.sub->leave, button);
		});
	}
	for (QWidget *w : {static_cast<QWidget *>(sub_key_), static_cast<QWidget *>(sub_name_),
			   static_cast<QWidget *>(sub_call_), sub_buttons_})
		w->hide();
}

void AccountsBar::set_look(const DockLook &look)
{
	look_ = look;
	const int edge = look_.metrics.edge_text_px;
	for (QLabel *key : {main_key_, sub_key_})
		ui::set_class(key, ui::muted_ok(look_.theme, look_.theme.base_bg) ? QStringLiteral("text-muted") : QString());
	grid_->setContentsMargins(edge, 0, edge, look_.metrics.small_px);
	grid_->setHorizontalSpacing(look_.metrics.large_px);
	grid_->setVerticalSpacing(look_.metrics.small_px);
	if (auto *sub_layout = sub_buttons_->layout())
		sub_layout->setSpacing(look_.metrics.small_px);
	// テーマのボタンの左右の余白のままだと、2 字のボタン 2 つで名前と通話の名前の幅の半分を使う。この行では名前が読めることを
	// 優先して、ボタンの左右の余白を大きい余白まで詰める。上下の外側の余白を外し、この 2 行がタブの中身の高さを取りすぎないように
	// する (235 x 300 のドックで一覧が見えなくなる)。
	const QString padding = QStringLiteral("padding-left: %1px; padding-right: %1px; margin-top: 0px; margin-bottom: 0px;")
					.arg(look_.metrics.large_px);
	for (QPushButton *button : {change_, move_, leave_})
		ui::set_own_style(button, "padding", padding);
	if (has_view_)
		apply_row(sub_name_, sub_call_, view_.sub.has_value() ? view_.sub->row : ledger::dockview::AccountRow{});
}

void AccountsBar::apply_button(QPushButton *button, const ledger::dockview::ViewButton &view)
{
	const bool enabled = !view.disabled_reason.has_value();
	if (button->isEnabled() != enabled)
		button->setEnabled(enabled);
	const QString tip = enabled ? QString() : ui::resolve(*view.disabled_reason);
	if (button->toolTip() != tip)
		button->setToolTip(tip);
}

void AccountsBar::apply_row(ui::ElideLabel *name, ui::ElideLabel *call, const ledger::dockview::AccountRow &row)
{
	name->set_full_text(ui::resolve_label(row.name));
	call->set_full_text(ui::resolve_label(row.call));
	// 注意の色は、背景の上で 4.5:1 に届かないテーマ (Classic の背景では 3.66:1) があるので、本文の色の方へ寄せて読める色にする。
	const QColor warn_theme = look_.theme.warning.isValid() ? look_.theme.warning : look_.theme.text;
	const ui::calc::Rgb warn_rgb = ui::calc::readable_on(ui::to_rgb(warn_theme), ui::to_rgb(look_.theme.base_bg),
							      ui::to_rgb(look_.theme.text));
	const QColor warn(warn_rgb.r, warn_rgb.g, warn_rgb.b);
	ui::set_own_style(call, "color", row.call_warn ? QStringLiteral("color: %1;").arg(warn.name(QColor::HexRgb)) : QString());
}

void AccountsBar::set_accounts(const ledger::dockview::AccountsView &view)
{
	if (has_view_ && view == view_)
		return;
	view_ = view;
	has_view_ = true;
	apply_row(main_name_, main_call_, view_.main.row);
	apply_button(change_, view_.main.change);
	const bool sub = view_.sub.has_value();
	for (QWidget *w : {static_cast<QWidget *>(sub_key_), static_cast<QWidget *>(sub_name_), sub_buttons_})
		w->setVisible(sub);
	sub_call_->setVisible(sub && !leave_hidden_);
	if (!sub)
		return;
	apply_row(sub_name_, sub_call_, view_.sub->row);
	apply_button(move_, view_.sub->move);
	apply_button(leave_, view_.sub->leave);
}

int AccountsBar::minimum_bar_width_px(int confirm_px) const
{
	const int edge = look_.metrics.edge_text_px;
	const int gap = look_.metrics.large_px;
	const int key = std::max(main_key_->sizeHint().width(), sub_key_->sizeHint().width());
	sub_buttons_->ensurePolished();
	const int pair = move_->sizeHint().width() + look_.metrics.small_px + leave_->sizeHint().width();
	const int buttons = std::max(pair, change_->sizeHint().width());
	// 名前と通話の名前に 2 em ずつを残す。確認のボタンは通話の名前の場所も使うので、その分だけ広いときに足りる。
	const int floor_px = static_cast<int>(2.0 * look_.metrics.em_px + 0.5);
	const int with_names = key + gap + floor_px + gap + floor_px + gap + buttons;
	const int with_confirm = key + gap + floor_px + gap + std::max(confirm_px, buttons);
	return 2 * edge + std::max(with_names, with_confirm);
}

QRect AccountsBar::leave_place() const
{
	return sub_buttons_->geometry();
}

void AccountsBar::set_leave_hidden(bool hidden)
{
	leave_hidden_ = hidden;
	// 通話の名前の列は、メインの行のラベルが幅を持つので、ここを隠しても列の幅は変わらない。
	sub_call_->setVisible(!hidden && view_.sub.has_value());
	// コンテナーは隠さず、中のボタンだけを隠す。コンテナーごと隠すと、確認の間にボタンの列の幅が変わる。
	move_->setVisible(!hidden);
	leave_->setVisible(!hidden);
}

} // namespace cordmux::dock
