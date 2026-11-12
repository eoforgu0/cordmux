// SPDX-License-Identifier: Apache-2.0

// cordmux の録画のボタンの基底クラス (RecordButtonBase)。録画の実際の状態を、ボタンの押された見た目で表示する。
// 状態ごとのテキストと押せるかどうかは、使う側 (dock/bottom_bar.cpp) が View の RecordButtonView から
// 決める。

#pragma once

#include <QPushButton>
#include <QRect>
#include <QStringList>

namespace cordmux::dock::ui {

// 押された見た目 (テーマの QPushButton:checked。Yami では primary の色で塗る) で、録画の実際の状態を表示する。
// OBS の録画のボタンの見た目 (state-active) は、OBS のウィジェットの objectName のセレクターに付いていて、
// プラグインからは使えないので、どのテーマにもある QPushButton:checked を使う。押したときではなく、録画の処理が
// 録画中になったと返したときに見た目を変える。押しただけで変えると、開始に失敗したときや停止の処理の間に、実際と
// 違う状態を表示するため。そのためボタンは checkable にするが、押しただけでは押された見た目にしない
// (nextCheckState で何もしない)。checked を変えるのは set_recording_look だけである。
// 停止の処理中は、押せない状態 (setEnabled(false) なので、キーボードとスクリーンリーダーにも押せないと伝わる)
// のまま、押された見た目で描く。OBS 自身の録画のボタンも、停止の処理中は押された見た目のままテキストだけを変える。
// OBS のテーマは :disabled の背景を :checked より後に書いているので、setEnabled(false) にしただけでは押された
// 見た目が消える。そこで、描くときだけ、QStyleOption の state に State_Enabled を付けてスタイルに渡す。
// どの状態のテキストでもボタンの幅を変えない。4 つのテキスト (cordmux.dock.record_start.button、
// record_stop.button、record_starting.button、record_stopping.button) を set_width_labels で受け取り、いちばん
// 広いものの幅を最小の幅にする。フォントやテーマが変わったら計算し直す。
// objectName (cordmuxRecordButton) は使う側が付ける。OBS の recordButton の objectName は使わない (OBS の中に
// 同じ名前のウィジェットが 2 つできるため)。
class RecordButtonBase : public QPushButton {
public:
	explicit RecordButtonBase(QWidget *parent = nullptr);

	// 録画の処理が録画中と返したときだけ true にする。
	void set_recording_look(bool on);
	bool recording_look() const { return isChecked(); }

	// 押せない間も、押された見た目を保つ (停止の処理中)。
	void set_pressed_look_while_disabled(bool on);
	bool pressed_look_while_disabled() const { return pressed_look_; }

	// 幅を決めるテキスト (どの状態でも同じ幅にするため)。
	void set_width_labels(const QStringList &labels);
	int widest_label_width() const { return widest_; }

	// 見えるテキストを、ボタンのテキスト (text()。読み上げソフトが読む名前になる) と別にする。空ならテキストをそのまま
	// 描く。録画中の経過時間のように毎秒変わる部分を、名前に入れずに見せるために使う。setText で変えると、Qt は
	// そのたびに名前が変わったことを読み上げソフトに伝えるので、フォーカスがボタンにある間、経過時間を読み続ける。
	void set_display_text(const QString &text);
	QString display_text() const { return display_text_.isEmpty() ? text() : display_text_; }

	// Qt がクリックを受ける矩形 (フォーカスリングが囲む範囲)。
	QRect hit_rect() const;

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	// 押しただけでは checked を変えない。
	void nextCheckState() override;
	void paintEvent(QPaintEvent *event) override;
	void changeEvent(QEvent *event) override;

private:
	void recompute_width();
	int width_for(const QString &label) const;

	QStringList width_labels_;
	int widest_ = 0;
	bool pressed_look_ = false;
	QString display_text_;
};

} // namespace cordmux::dock::ui
