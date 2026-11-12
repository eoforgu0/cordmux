// SPDX-License-Identifier: Apache-2.0

// 製品のドックのウィジェットを、objectName と動的プロパティで探して読む関数。読むだけで、ウィジェットを書き換えない。

#pragma once

#include <QPoint>
#include <QRect>
#include <QString>
#include <QStringList>

#include <string>
#include <vector>

#include "ledger/dock_view.h"

class QWidget;

namespace cordmux::dock {
class DockWidget;
class PeopleList;
} // namespace cordmux::dock

namespace cordmux::dock_drive {

// w かその祖先が持つ userId (行のウィジェットはすべて持つ)。なければ空を返す。
std::string user_of(const QWidget *w);
// w が行の中のどのウィジェットか ("check"、"dots"、"row"、"" のどれか)。祖先をたどって調べる。
QString part_of(const QWidget *w);

// ウィジェットの全体の矩形 (スクリーン座標)。w が nullptr なら空を返す。
QRect global_rect(const QWidget *w);

// 行のチェックの押せる範囲 (製品の RowCheck::focus_hit_rect と同じ矩形で、hitButton が使う範囲) のスクリーン座標。行が
// ないか、チェックが表示されていなければ空を返す。
QRect check_hit_rect_global(dock::PeopleList *list, const std::string &user_id);
// 行の [...] のスクリーン座標の矩形。行がないか、[...] が表示されていなければ空を返す。
QRect dots_rect_global(dock::PeopleList *list, const std::string &user_id);

// 1 行目の全文 (FirstLineBar::full_text)。
QString first_line_full_text(dock::DockWidget *widget);
// View の 1 行目に表示するはずの全文 (候補の最初の 1 つ)。FirstLineBar と同じ関数で ini を引く。
QString first_line_expected(const ledger::dockview::DockViewV2 &view);

// 状態タブに表示しているメッセージのカードと行のキー (上から順)。状態タブを開いていないときは空。
QStringList shown_notice_keys(dock::DockWidget *widget);
// View のメッセージのキー。状態タブにはすべてのメッセージを置く。
QStringList expected_notice_keys(const ledger::dockview::DockViewV2 &view);

// 参加者タブに表示する行を、並びの順に返す (メインの行、画面共有中、通話中)。
std::vector<ledger::dockview::PersonRow> listed_rows(const ledger::dockview::DockViewV2 &view);

// スクロール領域の縦の位置 (利用者がスクロールした量)。行が動いたかを調べるときに、スクロールした分を除くために使う。
int scroll_value(dock::DockWidget *widget);

} // namespace cordmux::dock_drive
