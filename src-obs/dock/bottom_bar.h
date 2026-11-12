// SPDX-License-Identifier: Apache-2.0

// ドックの下のバー。左に cordmux の録画のボタン、右端にドックのメニューを開く [...] だけを置き、スクロールしない。
// 録画のボタンはバーの幅いっぱいに伸ばす (OBS の [コントロール] のドックの録画のボタンと同じ形)。高さは状態でも幅でも変えない。
// バーとタブの中身の間の区切りは DockWidget が置く。
// [...] は OBS のクラス btn-tool icon-dots-vert を使い、アイコンはテーマのもの (なければ cordmux の 3 つの点) にする。

#pragma once

#include <functional>

#include <QPointer>
#include <QWidget>

#include "dock/dock_look.h"
#include "dock/ui/record_button_base.h"
#include "ledger/dock_view.h"

class QHBoxLayout;
class QPushButton;
class QVBoxLayout;

namespace cordmux::dock {

// 録画のボタン (objectName は cordmuxRecordButton。動的なプロパティ recordState に状態の名前を入れる)。
class RecordButton : public ui::RecordButtonBase {
public:
	explicit RecordButton(QWidget *parent = nullptr);
};

class BottomBar : public QWidget {
public:
	explicit BottomBar(QWidget *parent = nullptr);
	void set_look(const DockLook &look);
	// 録画のボタンに View を反映する。5 つの状態 (RecordButtonState) を表示し、押された見た目にするのは録画の側が
	// 保存先に書けたと返したとき (pressed_look) だけにする。幅は全部の状態のテキストのうち最も広いものに合わせ、
	// 状態で変えない。押せないときもボタンを消さずに押せない見た目にし、理由はツールチップと accessibleDescription に
	// 入れる。ボタンの下に文を出すと、バーの高さが変わってボタンが動く。
	void set_record(const ledger::dockview::RecordButtonView &view);
	void set_on_record(std::function<void(const ledger::dockview::ViewButton &)> cb) { on_record_ = std::move(cb); }
	// [...] が押されたときに呼び出す (メニューは DockWidget が開く)。
	void set_on_menu(std::function<void()> cb) { on_menu_ = std::move(cb); }

	// バーの最小の幅 (外周の余白、録画のボタンの最も広いテキストの幅、[...] の幅)。
	int minimum_bar_width() const;
	// 確認のボタンを置く間、録画のボタンを隠す (場所は残す)。
	void set_record_hidden(bool hidden);

	RecordButton *record_button() const { return record_; }
	QPushButton *menu_button() const { return menu_; }

private:
	void apply_menu_icon();
	// 隣のウィジェットとの間。フォーカスリング (空き + 線) の外に背景の色が 1 px 以上残るように、base_px と
	// リング + 1 px の大きい方を返す。
	int ring_neighbor_gap_px(int base_px) const;

	DockLook look_;
	bool has_look_ = false;
	QVBoxLayout *v_ = nullptr;
	QHBoxLayout *row1_ = nullptr;
	RecordButton *record_ = nullptr;
	QPushButton *menu_ = nullptr;
	ledger::dockview::RecordButtonView view_;
	bool has_view_ = false;
	bool fallback_icon_ = false; // [...] に cordmux のアイコンを入れている (テーマがアイコンを持たない)
	QPointer<QWidget> focus_moved_to_; // 押せなくしたときに Qt がフォーカスを移した先
	std::function<void(const ledger::dockview::ViewButton &)> on_record_;
	std::function<void()> on_menu_;
};

} // namespace cordmux::dock
