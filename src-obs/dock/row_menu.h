// SPDX-License-Identifier: Apache-2.0

// 参加者の行の [...] のメニュー。項目は「自動で枠に載せる」(チェックの付く項目)、「名前を変更...」、「使う声」(サブメニュー)。
// 音量は音声タブ (audio_tab.h) で変える。
// 行の [...]、右クリック、Menu キー、Shift+F10 のどれでも、PeopleList が同じメニューを開く。

#pragma once

#include <functional>
#include <string>

#include "ledger/actions.h"
#include "ledger/dock_view.h"

class QColor;
class QMenu;
class QWidget;

namespace cordmux::dock {

// 押された操作を DockPoll に渡すコールバック。dock_widget.h ほかにも同じ別名があり、どれを include しても同じ型になる。
using ActionCallback = std::function<void(ledger::ActionId, const ledger::ActionArgs &)>;

// 行のメニューを作る (開かない)。objectName は cordmuxRowMenu、「使う声」のサブメニューは cordmuxRowVoiceMenu。
// 項目には動的なプロパティ userId を付け、使う声の項目には voiceUserId も付ける (テストとプレビューツールが探す)。
// 項目を押すと、メニューを作った時点の row の人と引数で on_action を呼び出す。押せるかは ledger::apply() が
// もう一度決める。「名前を変更...」は on_rename に row.user_id を渡すだけで、ダイアログは呼び出し元が開く。
// 現在の値はチェックで示す。「使う声」のサブメニューは「本人の声」と通話に参加している人から 1 つを選ぶグループにし、
// 「本人の声」と人の名前の間に区切り線を入れる。
// 項目のテキストは View の label にする。押せない「使う声」の候補は、名前に短い理由を括弧で添えた ini の 1 つの
// 文字列 (cordmux.dock.row_voice_taken.menu) になる。理由が候補の全員で同じときは、「使う声」の項目そのものを
// 押せなくし、テキストを cordmux.dock.row_voice_from_lent.menu にする。語順が言語で変わるので、名前と理由をコードで
// 連結しない。押せない項目の長い理由 (disabled_reason) は、項目のテキストにすると項目が文になるので、ツールチップに
// 表示する。
QMenu *make_row_menu(QWidget *parent, const ledger::dockview::PersonRow &row, ActionCallback on_action,
		     std::function<void(const std::string &)> on_rename);

// 枠が埋まっているときにチェックを押して開く「入れ替える相手」のメニューを作る (開かない)。objectName は cordmuxReplaceMenu。
// 先頭に押せない見出しの項目を置き、枠に載っている人を位置の順に並べる。項目のアイコンは枠の位置を示す画面のアイコンで、color で
// 描く。項目を押すと on_action を ReplaceSlot で呼ぶ。1 回の操作で入れ替わるので、途中の並びが配信に出ない。
QMenu *make_replace_menu(QWidget *parent, const ledger::dockview::PersonRow &row, const QColor &color, int icon_px,
			 ActionCallback on_action);

// 開いているメニューの項目を、新しい View に合わせて押せなくする。押す直前の項目を動かさないように、項目を
// 作り直さず、押せなくするだけにする。row が nullptr (その人が通話から抜けた) なら、全部の項目を押せなくする。
// 「使う声」の項目は、その声の人が row の選択肢から消えたら押せなくする。
void refresh_row_menu(QMenu *menu, const ledger::dockview::PersonRow *row);

} // namespace cordmux::dock
