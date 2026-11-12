// SPDX-License-Identifier: Apache-2.0

// ホイールを受け取らない入力欄 (NoWheelSpin、NoWheelDoubleSpin、NoWheelCombo、NoWheelFontCombo、NoWheelSlider)。ドックのタブの
// 中の入力欄に使う。ホイールのイベントは、フォーカスの有無によらず、いつも親に渡してタブをスクロールさせる。スクロールの操作で
// 値が変わると、配信の画面がそのまま変わるためである。設定ウィンドウの数の入力欄は、フォーカスがあるときだけホイールで値を
// 変えられる (spin_box_ignore_wheel.h)。ドックのタブの入力欄は、いつ見ても同じ動作にする。
// コンボボックスは OBS の QStyle (SH_ComboBox_AllowWheelScrolling が 0) でもホイールを受け取らないが、cordmux は OBS の QStyle
// を前提にせず、自分で受け取らない。つまみ (NoWheelSlider) は、フォーカスの取り方、キー、ダブルクリックで既定値に戻す動きも
// 持つ (ドックのつまみの共通の部品)。

#pragma once

#include <QAbstractButton>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QMouseEvent>
#include <QSlider>
#include <QSpinBox>
#include <QWheelEvent>

#include <algorithm>
#include <functional>
#include <optional>
#include <utility>

namespace cordmux::dock::ui {

// Base を継承して wheelEvent を無条件に ignore する。フォーカスはクリックと Tab で受け取る (Qt::StrongFocus)。
template <class Base> class NoWheel : public Base {
public:
	explicit NoWheel(QWidget *parent = nullptr) : Base(parent) { this->setFocusPolicy(Qt::StrongFocus); }

protected:
	void wheelEvent(QWheelEvent *event) override { event->ignore(); }
};

// ドックのタブの中にあるチェック、ボタン、コンボボックスを、Tab でだけフォーカスを受けるようにする (クリックではフォーカスを
// 取らない)。クリックでフォーカスを取ると、OBS が前面のまま Space や矢印のキーを押したときに、同じ操作が働いてしまう。
// 数の入力欄は、クリックで文字を打ち始めるので対象にしない。つまみも対象にしない (NoWheelSlider のコメント)。tab の中身を
// 作り終えた後に呼ぶ。
inline void focus_by_tab_only(QWidget *root)
{
	for (QAbstractButton *button : root->findChildren<QAbstractButton *>())
		button->setFocusPolicy(Qt::TabFocus);
	for (QComboBox *combo : root->findChildren<QComboBox *>())
		combo->setFocusPolicy(Qt::TabFocus);
}

using NoWheelSpin = NoWheel<QSpinBox>;
using NoWheelDoubleSpin = NoWheel<QDoubleSpinBox>;
using NoWheelCombo = NoWheel<QComboBox>;
using NoWheelFontCombo = NoWheel<QFontComboBox>;

// つまみの大きい刻み (PageUp と PageDown で動く段数)。10 段にし、範囲の段数の 4 分の 1 を超えない (少なくとも 1 段)。
// 4 回より少なく押して端から端まで動くと、Home と End と変わらないためである (0 から 90% を 5% 刻みにした暗さは 4 段 = 20%)。
inline int slider_page_step(int range_steps)
{
	return std::max(1, std::min(10, range_steps / 4));
}

// ドックのつまみ (QSlider は向きを引数に取るので、別のクラスにする)。ドックのつまみはすべてこのクラスにする
// (音量のつまみ volume_slider.h と、数の入力欄と並べたつまみ slider_spin.h)。
// - ホイールは受け取らず、スクロールに渡す (このファイルの先頭のコメント)。スクロールしている途中に、ポインターがたまたま
//   つまみの上を通っただけの操作を、値の操作と取り違えないためである。
// - クリックと Tab でフォーカスを取る (Qt::StrongFocus)。フォーカスがある間は、矢印のキーで 1 段 (右と上で増え、左と下で減る)、
//   PageUp と PageDown で大きい刻み (slider_page_step)、Home と End で両端に動く (Qt の QSlider の既定の動き。HTML の range と
//   Windows のトラックバーも同じ)。キーを受け取るのは、フォーカスが、利用者がそのつまみを選んだ結果だからである (ホイールの
//   理由は当てはまらない)。ボタンとチェックがクリックでフォーカスを取らない理由 (OBS が前面のまま押した Space が同じ操作を
//   繰り返す) も、Space で何もしないつまみには当てはまらない。
// - 左のボタンのダブルクリックで、set_reset_value で決めた位置 (その設定の既定値) に戻す。戻した後に on_reset を呼ぶ。
//   位置を決めていなければ、ダブルクリックは普通のクリックと同じに扱う。
class NoWheelSlider : public QSlider {
public:
	explicit NoWheelSlider(Qt::Orientation orientation, QWidget *parent = nullptr) : QSlider(orientation, parent)
	{
		setFocusPolicy(Qt::StrongFocus);
	}

	// ダブルクリックで戻す位置 (つまみの値。範囲の外なら範囲の端)。
	void set_reset_value(int value) { reset_value_ = value; }
	std::optional<int> reset_value() const { return reset_value_; }
	// ダブルクリックで戻した後に呼ぶ関数 (値が変わらなかったときも呼ぶ)。valueChanged の後に呼ぶ。
	void set_on_reset(std::function<void()> on_reset) { on_reset_ = std::move(on_reset); }

protected:
	void wheelEvent(QWheelEvent *event) override { event->ignore(); }

	void mouseDoubleClickEvent(QMouseEvent *event) override
	{
		if (event->button() != Qt::LeftButton || !reset_value_.has_value()) {
			QSlider::mouseDoubleClickEvent(event);
			return;
		}
		event->accept();
		setValue(*reset_value_);
		if (on_reset_)
			on_reset_();
	}

private:
	std::optional<int> reset_value_;
	std::function<void()> on_reset_;
};

} // namespace cordmux::dock::ui
