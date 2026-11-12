// SPDX-License-Identifier: Apache-2.0

// ドックの 1 行目。メッセージの見出しか平常の要約を 1 行で表示するボタンで、押すと状態タブを開く。左から、メッセージのレベルの
// アイコン (メッセージの見出しのときだけ)、テキスト、残りの件数 (cordmux.dock.more_notices.status)、右端に、状態タブへ移ることを
// 示す山形か、[cordmuxを有効にする] (cordmux が無効の間だけ) を並べる。本文と操作のボタンは状態タブに置き、ここには置かない。
// 高さは状態で変えず、折り返さない。メッセージが表示されても消えても、その下のウィジェットを動かさないためである。

#pragma once

#include <functional>
#include <optional>

#include <QAbstractButton>
#include <QFont>

#include "dock/dock_look.h"
#include "dock/ui/focus_ring.h"
#include "ledger/dock_view.h"

class QHBoxLayout;
class QLabel;
class QPushButton;

namespace cordmux::dock {

namespace ui {
class ElideLabel;
class MarkLabel;
} // namespace ui

class FirstLineBar : public QAbstractButton, public ui::FocusHitRect {
public:
	explicit FirstLineBar(QWidget *parent = nullptr);

	// テーマの値を読み込み直したときに呼び出す。
	void set_look(const DockLook &look);

	// 中身を書き換える。並びの保留の間も、いつでもすぐに書き換える (高さが変わらないので、ほかのウィジェットは
	// 動かない)。
	// テキストは ledger の FirstLine::forms (長い順の候補。区切りは ini の 1 つの文字列の中にある) を ElideLabel に渡す。
	// どの候補も入らないときだけ、最後の候補の末尾を省く (数、時刻、人数の語の途中で切らないため)。全文はツールチップと
	// アクセシブル名に残る。残りの件数は見出しとは別のラベルにし、省かない。見出しと件数は、区切りの文字や空白を挟んで
	// コードで連結せず、間はレイアウトの spacing で空ける。
	// on_surface が true ならメッセージのレベル 1 なので、1 行目の背景を危険の色で塗り、テキストとアイコンを白か黒で描く。
	// メッセージの見出しは太字、平常の要約、利用者が選んだ状態、処理中は普通の太さにする。
	// cordmux が無効の間は、テキストとボタンを幅によらず両方残す (dock/ui/dock_ui_calc.h の disabled_line_fit)。
	void set_line(const ledger::dockview::FirstLine &line, bool on_surface);

	// 1 行目のボタン ([cordmuxを有効にする]) が押されたときに呼び出す。状態タブを開く操作は QAbstractButton::clicked で受ける。
	void set_on_button(std::function<void(const ledger::dockview::ViewButton &)> cb) { on_button_ = std::move(cb); }

	// 1 行目のテキストに使える幅 (プレビューツールが数える)。
	int text_width() const;
	// 見えているテキストと全文 (プレビューツールが数える)。
	QString shown_text() const;
	QString full_text() const;

	QPushButton *button() const { return button_; }
	// 1 行目のテキストのフォント (倍率を適用した後)。
	QFont text_font() const;
	// 状態によらず 1 行目に必要な幅。無効の形の、本文の大きさのテキストと短いボタンが入る幅である。
	int minimum_line_width_px() const;

	QRect focus_hit_rect() const override;

protected:
	void paintEvent(QPaintEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void changeEvent(QEvent *event) override;

private:
	void apply_type();
	void apply_colors();
	void read_fonts();
	int button_width_for(const QString &label) const;
	void refit_later();
	void refit();
	void set_text_small(bool on);

	DockLook look_;
	ledger::dockview::FirstLine line_;
	bool on_surface_ = false;
	bool has_line_ = false;
	bool text_small_ = false;
	QHBoxLayout *box_ = nullptr;
	ui::MarkLabel *mark_ = nullptr;
	ui::ElideLabel *text_ = nullptr;
	QLabel *more_ = nullptr;
	QPushButton *button_ = nullptr;
	ui::MarkLabel *go_ = nullptr; // 右端の山形
	std::optional<ledger::dockview::ViewButton> button_view_;
	QFont large_font_;
	QFont body_font_;
	bool refit_pending_ = false;
	std::function<void(const ledger::dockview::ViewButton &)> on_button_;
};

} // namespace cordmux::dock
