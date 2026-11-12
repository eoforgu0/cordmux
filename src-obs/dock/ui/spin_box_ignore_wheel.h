// SPDX-License-Identifier: Apache-2.0

// スピンボックス (QSpinBox、QDoubleSpinBox) の値が、マウスのホイールで変わらないようにする (IgnoreWheelSpin)。
// コンボボックス (QComboBox) には何もしなくてよい。OBS はアプリケーション全体の QStyle で
// SH_ComboBox_AllowWheelScrolling を 0 にしている (OBS 31.1.1 の frontend/utility/OBSProxyStyle.cpp)。そのため
// cordmux は、どのウィジェットにも独自の QStyle を設定しない (設定すると、OBS のこの設定が動作しなくなる)。

#pragma once

#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QWheelEvent>

namespace cordmux::dock::ui {

// OBS の SpinBoxIgnoreScroll (OBS 31.1.1 の shared/properties-view/spinbox-ignorewheel.cpp) と同じ実装である。
// フォーカスはクリックと Tab でだけ受け取り (Qt::StrongFocus)、フォーカスがないときはホイールのイベントを親へ渡す
// (スクロールの途中で値が変わらないように)。フォーカスのあるスピンボックスでは、ホイールで値を変えられる (OBS と
// 同じ)。OBS のウィジェットはプラグインから使えないので、同じ数行を cordmux が持つ。
template <class Base> class IgnoreWheelSpin : public Base {
public:
	explicit IgnoreWheelSpin(QWidget *parent = nullptr) : Base(parent) { this->setFocusPolicy(Qt::StrongFocus); }

protected:
	void wheelEvent(QWheelEvent *event) override
	{
		if (!this->hasFocus())
			event->ignore();
		else
			Base::wheelEvent(event);
	}
};

using SpinBoxIgnoreWheel = IgnoreWheelSpin<QSpinBox>;
using DoubleSpinBoxIgnoreWheel = IgnoreWheelSpin<QDoubleSpinBox>;

} // namespace cordmux::dock::ui
