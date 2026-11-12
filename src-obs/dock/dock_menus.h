// SPDX-License-Identifier: Apache-2.0

// ドックのメニュー (下のバーの [...] で開くメニューと、上部の [変更] で開くメニュー) を作る。項目のテキストと押せるかどうかは
// ledger の View が決め、ここでは判断しない。

#pragma once

#include <functional>

#include "ledger/actions.h"
#include "ledger/dock_view.h"

class QMenu;
class QWidget;

namespace cordmux::dock {

using ActionCallback = std::function<void(ledger::ActionId, const ledger::ActionArgs &)>;

// メニューを作って返す。開かずに返すので、プレビューツールとドックを操作するテストも、開かずに描画できる。メニューは
// parent を親にして作るので、呼び出し元が削除しなければ parent と一緒に破棄される。View で押せないボタンは項目も
// 押せなくして、理由をツールチップに表示する。項目を押すと on_action を呼ぶ。押してよいかは、呼び出された側が
// ledger::apply() で判断し直す。UI スレッドから呼び出す。
QMenu *make_dock_menu(QWidget *parent, const ledger::dockview::DockMenuView &menu, ActionCallback on_action);

// 上部の [変更] のメニューを作って返す (開かない。親と所有者は make_dock_menu と同じ)。通話中の人、過去の参加者、
// 「メインアカウントを使わない」の順に並べ、まとまりの間に区切り線を入れる。現在の値はチェックで示す。現在の値をもう 1 度選んでも
// 何も変えない。ユーザー ID を入力させない。
QMenu *make_main_account_menu(QWidget *parent, const ledger::dockview::MainAccountView &view, ActionCallback on_action);

} // namespace cordmux::dock
