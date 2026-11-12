// SPDX-License-Identifier: Apache-2.0

// つまみと数の入力欄を並べた入力欄 (SliderSpin)。ドックの表示タブで、配信の画面を見ながら値を動かすために使う。つまみを動かすと
// 数の入力欄が、数の入力欄を変えるとつまみが、同じ値になる。どちらもホイールを受け取らず、スクロールに渡す (no_wheel.h)。
// つまみの位置は値の刻み (step) の倍数に合わせる。つまみの段数が多すぎる値 (0 から 5000 ms など) は、つまみを置かず数の入力欄
// だけにする。つまみはドックのつまみの共通の部品 (no_wheel.h の NoWheelSlider) で、クリックと Tab でフォーカスを取り、キーで
// 動き、ダブルクリックで set_default_value の値に戻る。

#pragma once

#include <QString>
#include <QWidget>

#include <functional>

#include "dock/ui/no_wheel.h"

class QDoubleSpinBox;
class QSlider;
class QTimer;

namespace cordmux::dock::ui {

class SliderSpin : public QWidget {
public:
	// 整数の入力欄 (単位 suffix は数の後ろに付く)。
	static SliderSpin *integer(int lo, int hi, int step, const QString &suffix, QWidget *parent = nullptr);
	// 小数の入力欄。decimals は表示する桁で、刻み step の桁以上にする。
	static SliderSpin *real(double lo, double hi, int decimals, double step, const QString &suffix,
				QWidget *parent = nullptr);

	// つまみを置く段数の上限。これより段数が多い値は、つまみを置かない。
	static constexpr int kMaxSliderSteps = 1000;
	// 数を打つ手が止まってから、打った値を確定するまでの時間 (ms)。打っている途中の値 (300 と入れる途中の 3 と 30) は渡さない。
	// 確定は Enter、フォーカスが外れたとき、この時間のどれかが先に起きたときに行う。
	static constexpr int kTypingSettleMs = 500;

	double value() const;
	// 値を入れる。changed は呼ばない (外から届いた値を入れるとき)。
	void set_value(double value);
	// つまみのダブルクリックで戻す値 (その設定の既定値)。範囲の外なら範囲の端、刻みに合わなければ近い刻みに戻す。戻したときは、
	// 利用者が値を変えたときと同じく set_on_changed の関数を呼ぶ。つまみがなければ何もしない。
	void set_default_value(double value);
	// 利用者が値を変えたときに呼ぶ (つまみ、数の入力欄の矢印とキー、入力の確定)。
	void set_on_changed(std::function<void()> callback) { on_changed_ = std::move(callback); }
	// 利用者が操作している最中か (つまみを押している、つまみか数の入力欄にフォーカスがある)。
	bool editing() const;

	QDoubleSpinBox *spin() const { return spin_; }
	// 数の入力欄がいちばん長い値 (単位を含む) を省略せずに表示できる幅と、つまみに残す幅の和。ラベルの横に置けない幅では、
	// QFormLayout がこの入力欄をラベルの下へ移す。
	QSize minimumSizeHint() const override;
	// つまみがなければ nullptr。
	QSlider *slider() const { return slider_; }

private:
	SliderSpin(double lo, double hi, int decimals, double step, const QString &suffix, QWidget *parent);

	double snapped(double value) const;
	int slider_pos_of(double value) const;
	void changed_by_user();

	double lo_ = 0;
	double hi_ = 0;
	double step_ = 1;
	QDoubleSpinBox *spin_ = nullptr;
	NoWheelSlider *slider_ = nullptr;
	QTimer *typing_timer_ = nullptr;
	std::function<void()> on_changed_;
};

} // namespace cordmux::dock::ui
