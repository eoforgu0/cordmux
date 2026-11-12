// SPDX-License-Identifier: Apache-2.0

// ドックのタブ。タブのバー (DockTabBar) と、タブの中身の 1 ページ (DockPage)。ページは、スクロールしない行 (なくてもよい) と、
// 縦に 1 つのスクロール領域を持つ。新しいタブの中身は、DockPage::set_content にウィジェットを渡して作る。

#pragma once

#include <QScrollArea>
#include <QTabBar>
#include <QWidget>

#include "dock/ui/focus_ring.h"

class QVBoxLayout;
class QWheelEvent;

namespace cordmux::dock {

// タブのバー。すべてのタブを同じ幅にする。状態タブの件数でタブの幅が変わると、タブの位置が動くためである。幅は、いちばん長い
// テキスト (set_widest_text) で測る。テーマの見た目と、タブのアクセシビリティは QTabBar のまま使う。ホイールではタブを切り替えない
// (スクロールの操作で値を変えない)。クリックではフォーカスを取らない (QTabBar の既定の Qt::TabFocus)。
class DockTabBar : public QTabBar, public ui::FocusHitRect {
public:
	explicit DockTabBar(QWidget *parent = nullptr);
	// リングの太さ + すき間。バーはドックの幅いっぱいなので、押せる範囲をこの分だけ内側に寄せる (リングの左右の辺が切れないように)。
	void set_ring_reach(int reach) { reach_ = reach; }
	QRect focus_hit_rect() const override { return rect().adjusted(reach_, reach_, -reach_, -reach_); }

	// 幅を測るテキスト (件数を含む状態タブの最も長い形)。
	void set_widest_text(const QString &text);
	// タブの左右の余白 (px)。テーマの既定の余白と最小の幅のままだと、4 つのタブだけでドックの最小幅が 26 em を超える。
	void set_side_padding(int px);
	// 状態によらずバーに必要な幅。
	int minimum_bar_width_px() const;

protected:
	QSize tabSizeHint(int index) const override;
	QSize minimumTabSizeHint(int index) const override;
	void wheelEvent(QWheelEvent *event) override;

private:
	QString widest_text_;
	int reach_ = 0;
};

// タブの中身のスクロール領域。タブストップに残すので、フォーカスリングの押せる範囲を、矩形からリングの太さとすき間の分だけ
// 内側に寄せる。リングは範囲 (ドック) の矩形で切って描くので、矩形のままだと左右の辺が消え、上下の 2 本が区切り線と見分けが
// 付かなくなる。スクロールバーの幅は常に確保する (Qt::ScrollBarAlwaysOn)。バーが出たときに行の右端のボタンが動かないようにする
// ためである。
class ScrollBox : public QScrollArea, public ui::FocusHitRect {
public:
	explicit ScrollBox(QWidget *parent);
	// リングの太さ + すき間。
	void set_ring_reach(int reach);
	QRect focus_hit_rect() const override;

private:
	int reach_ = 0;
};

// タブの中身の 1 ページ。
//   - 親は DockWidget のページの並び (QStackedWidget) で、DockWidget が add_page で足す。
//   - スクロールはページの中の縦に 1 つだけで、横のスクロールと入れ子のスクロールを作らない。
//   - ページの中のつまみ、コンボボックス、スピンボックスはホイールを受け取らず、スクロールに渡す。ウィジェットの側で wheelEvent を
//     ignore する (dock/ui/spin_box_ignore_wheel.h の形。フォーカスがあっても渡すように、hasFocus を見ない形にする)。
//   - フォーカスがスクロール領域の中へ移ると、DockWidget がフォーカスリングの見える所までスクロールする。
class DockPage : public QWidget {
public:
	// scroll_name はスクロール領域の objectName。読み上げの名前はドックのタイトルにする。
	DockPage(const QString &scroll_name, QWidget *parent = nullptr);

	// スクロールしない行を置く。呼ばなければ、スクロール領域がページの全部になる。widget はページの子になる。
	void set_header(QWidget *widget);
	// スクロールする中身を置く。widget はスクロール領域の子になり、幅はスクロール領域に合わせて変わる。
	void set_content(QWidget *widget);

	ScrollBox *scroll() const { return scroll_; }
	QWidget *header() const { return header_; }

private:
	QVBoxLayout *layout_ = nullptr;
	QWidget *header_ = nullptr;
	ScrollBox *scroll_ = nullptr;
};

} // namespace cordmux::dock
