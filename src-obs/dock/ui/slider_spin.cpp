// SPDX-License-Identifier: Apache-2.0

// つまみと数の入力欄を並べた入力欄 (slider_spin.h) を実装する。

#include "dock/ui/slider_spin.h"

#include <QHBoxLayout>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QTimer>

#include <algorithm>
#include <cmath>

#include "dock/ui/no_wheel.h"

namespace cordmux::dock::ui {

namespace {

// つまみに残す最小の幅 (1 em の倍数)。
constexpr int kSliderMinEm = 6;

// 最小の大きさを、いちばん長い値が収まる大きさにする数の入力欄。Qt の既定の最小の大きさは、値の文字を切ることがある。
class FullTextDoubleSpin : public NoWheelDoubleSpin {
public:
	// Qt の見積もりは、テーマの余白と単位の文字を数えきれず、右端の文字が切れるので、1 em 足す。
	QSize sizeHint() const override
	{
		QSize hint = NoWheelDoubleSpin::sizeHint();
		hint.setWidth(hint.width() + fontMetrics().horizontalAdvance(QLatin1Char('M')));
		return hint;
	}
	QSize minimumSizeHint() const override { return sizeHint(); }
};

} // namespace

SliderSpin *SliderSpin::integer(int lo, int hi, int step, const QString &suffix, QWidget *parent)
{
	return new SliderSpin(lo, hi, 0, std::max(1, step), suffix, parent);
}

SliderSpin *SliderSpin::real(double lo, double hi, int decimals, double step, const QString &suffix, QWidget *parent)
{
	return new SliderSpin(lo, hi, decimals, step, suffix, parent);
}

SliderSpin::SliderSpin(double lo, double hi, int decimals, double step, const QString &suffix, QWidget *parent)
	: QWidget(parent), lo_(lo), hi_(hi), step_(step)
{
	auto *h = new QHBoxLayout(this);
	h->setContentsMargins(0, 0, 0, 0);

	spin_ = new FullTextDoubleSpin();
	spin_->setRange(lo, hi);
	spin_->setDecimals(decimals);
	spin_->setSingleStep(step);
	spin_->setSuffix(suffix);
	// 打っている途中の値は渡さない。確定は Enter、フォーカスが外れたとき、打つ手が止まって kTypingSettleMs たったときに行う。
	spin_->setKeyboardTracking(false);
	typing_timer_ = new QTimer(this);
	typing_timer_->setSingleShot(true);
	typing_timer_->setInterval(kTypingSettleMs);
	QObject::connect(typing_timer_, &QTimer::timeout, this, [this]() { spin_->interpretText(); });
	// lineEdit() は protected なので、子から探す。
	QLineEdit *edit = spin_->findChild<QLineEdit *>();
	QObject::connect(edit, &QLineEdit::textEdited, this, [this]() { typing_timer_->start(); });
	QObject::connect(edit, &QLineEdit::returnPressed, this, [this]() {
		typing_timer_->stop();
		// Enter で入力を終える。フォーカスを残すと、次に押した Space や矢印のキーが入力欄に届く。
		spin_->clearFocus();
	});
	setFocusProxy(spin_);

	const double steps = std::round((hi - lo) / step);
	if (steps >= 1 && steps <= kMaxSliderSteps) {
		slider_ = new NoWheelSlider(Qt::Horizontal);
		slider_->setRange(0, static_cast<int>(steps));
		slider_->setSingleStep(1);
		slider_->setPageStep(slider_page_step(static_cast<int>(steps)));
		// テーマのつまみは高さを持たないので、数の入力欄と同じ高さにする。
		slider_->setFixedHeight(spin_->sizeHint().height());
		h->addWidget(slider_, 1);
		QObject::connect(slider_, &QSlider::valueChanged, this, [this](int pos) {
			const double value = std::clamp(lo_ + pos * step_, lo_, hi_);
			{
				const QSignalBlocker blocker(spin_);
				spin_->setValue(value);
			}
			changed_by_user();
		});
	}
	h->addWidget(spin_, slider_ != nullptr ? 0 : 1);

	QObject::connect(spin_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
		if (slider_ != nullptr) {
			const QSignalBlocker blocker(slider_);
			slider_->setValue(slider_pos_of(value));
		}
		changed_by_user();
	});
	// 入力した値が刻みに合わないときは、入力を終えたところで合う値に修正して表示する (設定には合う値だけを書く)。
	QObject::connect(spin_, &QAbstractSpinBox::editingFinished, this, [this]() {
		const double fixed = snapped(spin_->value());
		if (std::fabs(fixed - spin_->value()) > step_ * 1e-6)
			spin_->setValue(fixed);
	});
}

QSize SliderSpin::minimumSizeHint() const
{
	const int em = std::max(1, fontMetrics().horizontalAdvance(QLatin1Char('M')));
	const QSize spin = spin_->sizeHint();
	const int slider = slider_ != nullptr ? em * kSliderMinEm + layout()->spacing() : 0;
	return QSize(spin.width() + slider, spin.height());
}

double SliderSpin::snapped(double value) const
{
	if (step_ <= 0)
		return std::clamp(value, lo_, hi_);
	return std::clamp(lo_ + std::round((value - lo_) / step_) * step_, lo_, hi_);
}

int SliderSpin::slider_pos_of(double value) const
{
	return static_cast<int>(std::lround((std::clamp(value, lo_, hi_) - lo_) / step_));
}

double SliderSpin::value() const
{
	return spin_->value();
}

void SliderSpin::set_default_value(double value)
{
	if (slider_ == nullptr)
		return;
	const double fixed = snapped(value);
	slider_->set_reset_value(slider_pos_of(fixed));
	// つまみの位置が既定値と同じでも、数の入力欄の値が刻みの間 (設定ファイルに手で書いた値) なら、既定値にそろえる。
	slider_->set_on_reset([this, fixed]() {
		if (std::fabs(spin_->value() - fixed) > step_ * 1e-6)
			spin_->setValue(fixed);
	});
}

void SliderSpin::set_value(double value)
{
	const QSignalBlocker blocker_spin(spin_);
	spin_->setValue(value);
	if (slider_ != nullptr) {
		const QSignalBlocker blocker_slider(slider_);
		slider_->setValue(slider_pos_of(spin_->value()));
	}
}

bool SliderSpin::editing() const
{
	return spin_->hasFocus() || (slider_ != nullptr && (slider_->hasFocus() || slider_->isSliderDown()));
}

void SliderSpin::changed_by_user()
{
	if (on_changed_)
		on_changed_();
}

} // namespace cordmux::dock::ui
