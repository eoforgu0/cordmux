// SPDX-License-Identifier: Apache-2.0

// ドックの上部の、メインとサブの固定の行。メインの行は名前、通話の名前、[変更]。サブの行は名前、通話の名前、[移動]、[退出] で、
// サブアカウントを使う構成でだけ置く。ボタンは常に同じ位置に置き、使えないときは押せない表示にしてツールチップで理由を出す。
// 判断はせず、ledger の AccountsView を描くだけである。

#pragma once

#include <functional>

#include <QWidget>

#include "dock/dock_look.h"
#include "ledger/dock_view.h"

class QGridLayout;
class QLabel;
class QPushButton;

namespace cordmux::dock {

namespace ui {
class ElideLabel;
} // namespace ui

class AccountsBar : public QWidget {
public:
	explicit AccountsBar(QWidget *parent = nullptr);

	void set_look(const DockLook &look);
	void set_accounts(const ledger::dockview::AccountsView &view);

	// ボタンが押されたときに呼び出す。anchor は押されたボタンで、[変更] のメニューの位置に使う。
	void set_on_press(std::function<void(const ledger::dockview::ViewButton &, QWidget *anchor)> cb)
	{
		on_press_ = std::move(cb);
	}

	// 状態によらず必要な幅。確認のボタン (押した場所の隣の確認) の幅 confirm_px も含める。
	int minimum_bar_width_px(int confirm_px) const;
	// [移動] と [退出] を合わせた矩形 (このウィジェットの座標)。[退出] の確認のボタンを置く場所である。
	QRect leave_place() const;
	// 確認のボタンを置く間、サブの行の通話の名前と [移動] と [退出] を隠す (場所は残す)。
	void set_leave_hidden(bool hidden);

	QPushButton *change_button() const { return change_; }
	QPushButton *move_button() const { return move_; }
	QPushButton *leave_button() const { return leave_; }

private:
	void apply_button(QPushButton *button, const ledger::dockview::ViewButton &view);
	void apply_row(ui::ElideLabel *name, ui::ElideLabel *call, const ledger::dockview::AccountRow &row);

	DockLook look_;
	ledger::dockview::AccountsView view_;
	bool has_view_ = false;
	bool leave_hidden_ = false;
	QGridLayout *grid_ = nullptr;
	QLabel *main_key_ = nullptr;
	ui::ElideLabel *main_name_ = nullptr;
	ui::ElideLabel *main_call_ = nullptr;
	QPushButton *change_ = nullptr;
	QLabel *sub_key_ = nullptr;
	ui::ElideLabel *sub_name_ = nullptr;
	ui::ElideLabel *sub_call_ = nullptr;
	QWidget *sub_buttons_ = nullptr;
	QPushButton *move_ = nullptr;
	QPushButton *leave_ = nullptr;
	std::function<void(const ledger::dockview::ViewButton &, QWidget *)> on_press_;
};

} // namespace cordmux::dock
