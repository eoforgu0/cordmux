// SPDX-License-Identifier: Apache-2.0

// 押した場所の隣の確認。元のボタンの場所を 2 つに分けて [確定] とやめる側のボタン ([通話を続ける] など) を置く。ダイアログにしないのは、フォーカスを奪うと
// 配信中の OBS の操作を妨げるためである。行の高さと位置は変えない。確認するかの判断は ledger/inline_confirm.h にある。

#pragma once

#include <functional>

#include <QPointer>
#include <QWidget>

#include "dock/dock_look.h"

class QPushButton;
class QTimer;

namespace cordmux::dock {

// 操作がないまま確認のボタンを出しておく時間 (ms)。これを過ぎたら元のボタンに戻す。押さなかったことは何も起こさないので、戻しても
// 安全である。
inline constexpr int kInlineConfirmTimeoutMs = 5000;

// 親は DockWidget で、ほかのウィジェットの上に重ねて置く。元のボタンを隠すのは呼び出す側である (on_end で戻す)。
// [確定] とやめる側のボタンは、出た直後のダブルクリックの時間だけクリックを受け付けない (PressGuard) ので、元のボタンを続けて
// 押しても確定しない。クリックではフォーカスを取らない。キーボードで押したときはやめる側のボタンにフォーカスを置く。
class InlineConfirm : public QWidget {
public:
	explicit InlineConfirm(QWidget *parent);

	void set_look(const DockLook &look);

	// place は親の座標の、元のボタンの場所。confirm_label は [確定] のテキスト、cancel_label はやめる側のボタンのテキスト
	// ([通話を続ける] など。押すと何も起こさず確認を閉じる)。on_confirm は [確定] を押したとき、on_end は
	// どの終わり方でも (確定の前に) 呼ぶ。動作中に呼んだときは、前の確認を取り消してから始める。focus_to_cancel はキーボードで
	// 押したときに true を渡す。ポインターの位置は QCursor から読む (dock/ui/dock_ui_calc.h の confirm_layout)。
	void start(const QRect &place, const QString &confirm_label, const QString &cancel_label, bool focus_to_cancel, std::function<void()> on_confirm,
		   std::function<void()> on_end);
	// 確認を取り消して元に戻す。動作していなければ何もしない。
	void cancel();
	bool active() const { return active_; }

	QPushButton *confirm_button() const { return confirm_; }
	QPushButton *cancel_button() const { return cancel_; }

	// [確定] とやめる側のボタンを並べるのに必要な幅。最小幅の計算に使う。
	int pair_width_px(const QString &confirm_label, const QString &cancel_label) const;

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	void finish();

	DockLook look_;
	QPushButton *confirm_ = nullptr;
	QPushButton *cancel_ = nullptr;
	QTimer *timer_ = nullptr;
	bool active_ = false;
	QPointer<QWidget> focus_back_;
	std::function<void()> on_confirm_;
	std::function<void()> on_end_;
};

} // namespace cordmux::dock
