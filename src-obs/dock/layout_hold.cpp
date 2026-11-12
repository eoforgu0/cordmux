// SPDX-License-Identifier: Apache-2.0

// 押す直前に一覧を動かさないための LayoutHold と PressGuard (layout_hold.h) を実装する。

#include "dock/layout_hold.h"

#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QStyleHints>
#include <QTimer>

#include "dock/ui/dock_ui_calc.h"

namespace cordmux::dock {

namespace {

// ガードを付けたウィジェットから、そのガードを探すための動的なプロパティの名前。
constexpr const char *kGuardProperty = "_cordmux_press_guard";

} // namespace

LayoutHold::LayoutHold(QWidget *root, QWidget *viewport) : QObject(root), root_(root), viewport_(viewport)
{
	// マウスのボタンを押すイベントは押された子のウィジェットに届き、親には伝わらないことがあるので、アプリケーション
	// 全体のイベントフィルターで受け取り、ドックの中のウィジェットへのイベントだけを数える。プラグインがアンロードされた後に
	// 残さないように、イベントフィルターはデストラクターで外す。
	if (qApp != nullptr)
		qApp->installEventFilter(this);
	idle_timer_ = new QTimer(this);
	idle_timer_->setSingleShot(true);
	QObject::connect(idle_timer_, &QTimer::timeout, this, [this] {
		moving_ = false;
		update_state();
	});
}

LayoutHold::~LayoutHold()
{
	if (qApp != nullptr)
		qApp->removeEventFilter(this);
}

void LayoutHold::set_viewport(QWidget *viewport)
{
	viewport_ = viewport;
	moving_ = false;
	idle_timer_->stop();
	reevaluate();
}

void LayoutHold::set_idle_ms(int ms)
{
	idle_ms_ = ms;
}

void LayoutHold::note_pointer_moved()
{
	if (idle_ms_ <= 0) {
		moving_ = false;
		return;
	}
	moving_ = true;
	idle_timer_->start(idle_ms_);
}

void LayoutHold::set_menu_open(bool open)
{
	menus_open_ += open ? 1 : -1;
	if (menus_open_ < 0)
		menus_open_ = 0;
	// メニューを閉じた時点のポインターの位置で判定し直す。閉じた直後に押される行は、ポインターがスクロール領域の
	// 上にある間の並びの保留が引き続き守る。
	pointer_over_ = pointer_over_viewport();
	update_state();
}

void LayoutHold::reevaluate()
{
	pointer_over_ = pointer_over_viewport();
	if (pressed_ && QGuiApplication::mouseButtons() == Qt::NoButton)
		pressed_ = false;
	update_state();
}

bool LayoutHold::pointer_over_viewport() const
{
	if (viewport_.isNull() || !viewport_->isVisible())
		return false;
	const QPoint local = viewport_->mapFromGlobal(QCursor::pos());
	return viewport_->rect().contains(local);
}

bool LayoutHold::inside_viewport(const QWidget *w) const
{
	return w == viewport_.data() || viewport_->isAncestorOf(w);
}

bool LayoutHold::inside_root(QObject *watched) const
{
	if (root_.isNull() || !watched->isWidgetType())
		return false;
	auto *w = static_cast<QWidget *>(watched);
	return w == root_.data() || root_->isAncestorOf(w);
}

bool LayoutHold::eventFilter(QObject *watched, QEvent *event)
{
	switch (event->type()) {
	case QEvent::MouseButtonPress:
	case QEvent::MouseButtonDblClick:
		if (inside_root(watched)) {
			pressed_ = true;
			update_state();
		}
		break;
	case QEvent::MouseButtonRelease:
		if (pressed_ && QGuiApplication::mouseButtons() == Qt::NoButton) {
			pressed_ = false;
			pointer_over_ = pointer_over_viewport();
			update_state();
		}
		break;
	case QEvent::MouseMove:
	case QEvent::Wheel:
		// マウスの移動は、Qt がマウスの追跡をしていないウィジェットの分もアプリケーションのイベントフィルターに渡す
		// (Qt 6.8.3 の qapplication.cpp の notify の MouseMove の節)。行のウィジェットに追跡を付けて回らずに済む。
		if (watched->isWidgetType() && !viewport_.isNull() && inside_viewport(static_cast<QWidget *>(watched))) {
			pointer_over_ = pointer_over_viewport();
			if (pointer_over_)
				note_pointer_moved();
			update_state();
		}
		break;
	case QEvent::Enter:
	case QEvent::Leave:
		// Enter と Leave だけで判定すると、子のウィジェットに入ったときに届くイベントがウィジェットの実装で変わり、
		// 判定がずれる。そのため、イベントのたびにポインターの位置を測り直す。
		if (watched == viewport_.data()) {
			pointer_over_ = pointer_over_viewport();
			if (pointer_over_)
				note_pointer_moved();
			update_state();
		}
		break;
	default:
		break;
	}
	return QObject::eventFilter(watched, event);
}

void LayoutHold::update_state()
{
	const bool now = ui::calc::layout_held(pointer_over_, moving_, pressed_, menus_open_ > 0);
	if (now == held_)
		return;
	held_ = now;
	if (on_changed_)
		on_changed_(held_);
}

PressGuard::PressGuard(QWidget *target) : QObject(target)
{
	target->setProperty(kGuardProperty, QVariant::fromValue(static_cast<QObject *>(this)));
	target->installEventFilter(this);
	arm();
}

void PressGuard::arm()
{
	since_.start();
	started_ = true;
}

bool PressGuard::armed() const
{
	if (!started_)
		return false;
	const int interval = QGuiApplication::styleHints() != nullptr
				     ? QGuiApplication::styleHints()->mouseDoubleClickInterval()
				     : 500;
	return since_.elapsed() < interval;
}

void PressGuard::arm_widget(QWidget *target)
{
	if (target == nullptr)
		return;
	// ドックのクラスは Q_OBJECT を使わない (シグナルにはラムダを接続する) ので、qobject_cast は使えない。
	auto *guard = dynamic_cast<PressGuard *>(target->property(kGuardProperty).value<QObject *>());
	if (guard != nullptr)
		guard->arm();
}

bool PressGuard::eventFilter(QObject *watched, QEvent *event)
{
	if ((event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick) && armed())
		return true; // マウスのボタンを押すイベントを捨てる。キーの入力はここに来ない
	return QObject::eventFilter(watched, event);
}

} // namespace cordmux::dock
