// SPDX-License-Identifier: Apache-2.0

// input.h の実装。プレビュー用のプラットフォームプラグインが持たない Windows の動作 (ウィンドウの前後、アクティブの
// 移り方、OS のカーソルの位置、キーボードからのコンテキストメニュー) を、ここで再現する。

#include "input.h"

#include <QApplication>
#include <QList>
#include <QPointer>
#include <QGuiApplication>
#include <QWidget>
#include <QWindow>

#include <qpa/qwindowsysteminterface.h>

#include <cstdio>

#include "crash_trace.h"
#include <QtGui/private/qguiapplication_p.h>

namespace cordmux::dock_drive {

namespace {

using Sync = QWindowSystemInterface::SynchronousDelivery;

QPointF local_of(QWindow *window, const QPoint &global)
{
	return QPointF(window->mapFromGlobal(global));
}

// ウィンドウの前後 (Windows の Z オーダーの再現)。プレビュー用のプラットフォームプラグインはウィンドウの前後を
// 持たず、QGuiApplication::topLevelAt はウィンドウを作った順で探す (後に作ったウィンドウが前)。Windows では、
// 表示したウィンドウとアクティブにしたウィンドウが前に来る (SetForegroundWindow とクリック)。ここでは、表示したときと
// アクティブにしたときに前へ移し、押す相手をこの順で探す。
QList<QPointer<QWindow>> &z_order()
{
	static QList<QPointer<QWindow>> list;
	return list;
}

// x が w の所有するウィンドウか (x の transientParent をたどると w がある)。
bool owned_by(QWindow *x, QWindow *w)
{
	for (QWindow *p = x->transientParent(); p != nullptr; p = p->transientParent()) {
		if (p == w)
			return true;
	}
	return false;
}

void bring_front(QWindow *w)
{
	if (w == nullptr)
		return;
	QList<QPointer<QWindow>> &z = z_order();
	for (qsizetype i = z.size() - 1; i >= 0; --i) {
		if (z[i].isNull() || z[i].data() == w)
			z.removeAt(i);
	}
	// w が所有するウィンドウ (フローティングのドック、設定ウィンドウなど) は、Windows では常に w より前にある。w を
	// 前に出すときは、所有するウィンドウを前後の順のまま、w のさらに前に置く。
	QList<QPointer<QWindow>> owned;
	for (qsizetype i = 0; i < z.size();) {
		if (owned_by(z[i].data(), w)) {
			owned.append(z[i]);
			z.removeAt(i);
		} else {
			++i;
		}
	}
	z.prepend(w);
	for (qsizetype i = owned.size() - 1; i >= 0; --i)
		z.prepend(owned[i]);
}

void activate_native(QWindow *w)
{
	if (w == nullptr)
		return;
	bring_front(w);
	QWindowSystemInterface::handleFocusWindowChanged<Sync>(w, Qt::ActiveWindowFocusReason);
}

// ウィンドウが隠れたときのアクティブの移り方を Windows に合わせる。プレビュー用のプラットフォームプラグインは、
// ウィンドウを作るたびにそのウィンドウをアクティブにし (tests/ui_preview/preview_platform.cpp の
// createPlatformWindow)、隠れたウィンドウの代わりをアクティブにしない。Windows では次のようになる。
//   - メニュー (Qt の popup) はアクティブを取らない (Windows のプラットフォームプラグインは、popup を
//     SW_SHOWNOACTIVATE で表示する) ので、閉じても前のウィンドウがアクティブのまま。
//   - ウィンドウ (QDialog) を閉じると、OS が親ウィンドウをアクティブにする。
// このままだと、メニューやダイアログを閉じた後にアクティブなウィンドウがなくなり、製品が閉じた後に戻すフォーカス
// (dock/people_list.cpp の open_row_menu の最後と、dock/ui/message_dialog.cpp) とキーが届かない。そこで、隠れた時点
// (製品がフォーカスを戻すより前) にアクティブを戻す。
class HideWatcher : public QObject {
public:
	QPointer<QWindow> *last_active = nullptr;

protected:
	bool eventFilter(QObject *o, QEvent *e) override
	{
		if ((e->type() != QEvent::Hide && e->type() != QEvent::Show) || !o->isWidgetType())
			return false;
		auto *w = static_cast<QWidget *>(o);
		if (!w->isWindow())
			return false;
		if (e->type() == QEvent::Show) {
			// 表示したウィンドウは前に来る (メニューも)。
			if (w->windowHandle() != nullptr)
				bring_front(w->windowHandle());
			return false;
		}
		QWindow *target = nullptr;
		if (w->windowType() == Qt::Popup) {
			if (QApplication::activePopupWidget() == nullptr && last_active != nullptr && !last_active->isNull())
				target = last_active->data();
		} else if (w->parentWidget() != nullptr) {
			target = w->parentWidget()->window()->windowHandle();
		}
		if (target == nullptr || !target->isVisible())
			return false;
		QWindow *fw = QGuiApplication::focusWindow();
		if (fw == nullptr || fw == w->windowHandle() || !fw->isVisible()) {
			activate_native(target);
			if (last_active != nullptr && target->type() != Qt::Popup)
				*last_active = target;
		}
		return false;
	}
};

} // namespace

QWindow *Input::window_at(const QPoint &global)
{
	// 開いているメニューがあれば、ボタンを押すイベントはすべてメニューへ送る (Qt の popup の動作。外を押すと
	// メニューが閉じる)。
	if (QWidget *popup = QApplication::activePopupWidget()) {
		if (QWindow *w = popup->windowHandle())
			return w;
	}
	for (const QPointer<QWindow> &w : z_order()) {
		if (!w.isNull() && w->isVisible() && w->geometry().contains(global))
			return w.data();
	}
	return QGuiApplication::topLevelAt(global);
}

QWidget *Input::widget_at(const QPoint &global)
{
	if (QWidget *popup = QApplication::activePopupWidget()) {
		if (popup->rect().contains(popup->mapFromGlobal(global))) {
			QWidget *child = popup->childAt(popup->mapFromGlobal(global));
			return child != nullptr ? child : popup;
		}
		return popup;
	}
	QWindow *w = window_at(global);
	for (QWidget *top : QApplication::topLevelWidgets()) {
		if (top->windowHandle() == w && w != nullptr) {
			QWidget *child = top->childAt(top->mapFromGlobal(global));
			return child != nullptr ? child : top;
		}
	}
	return QApplication::widgetAt(global);
}

void Input::send_mouse(QWindow *window, QEvent::Type type, Qt::MouseButton button)
{
	if (window == nullptr)
		return;
	QWindowSystemInterface::handleMouseEvent<Sync>(window, local_of(window, pos_), QPointF(pos_), buttons_, button,
							type, QGuiApplication::keyboardModifiers());
}

void Input::move_to(const QPoint &global)
{
	{
		char note[96];
		std::snprintf(note, sizeof(note), "move %d,%d", global.x(), global.y());
		crash_note(note);
	}
	pos_ = global;
	// ボタンを押している間は、押したウィンドウがイベントを受け続ける (Qt の暗黙のマウスグラブ)。
	QWindow *w = buttons_ != Qt::NoButton && !grab_.isNull() ? grab_.data() : window_at(global);
	// OS のカーソルの位置を再現する。Windows のプラットフォームプラグインの QCursor::pos は GetCursorPos で OS の
	// カーソルを読むので、ウィンドウから出たことの通知 (WM_MOUSELEAVE) を受けた時点で、もう新しい位置を返す。
	// プレビュー用のプラットフォームプラグインはカーソルを持たず、QCursor::pos は最後に届いたポインターの
	// イベントの位置 (QGuiApplicationPrivate::lastCursorPosition) を返す。製品の並びの保留 (dock/layout_hold.cpp)
	// は Enter と Leave のイベントの中で QCursor::pos を読むので、Enter と Leave を送る間だけ新しい位置にしておく。
	// 移動のイベントを送る前には前の位置に戻す (同じ位置だと、Qt は動いていないとみなして移動のイベントを捨てる。
	// qguiapplication_p.h のコメント)。どの Qt のウィンドウの上でもない所へ動かしたとき (移動のイベントがない) も、
	// 最後は新しい位置にしておく。
	const QPointF before = QGuiApplicationPrivate::lastCursorPosition;
	if (w != under_.data()) {
		QGuiApplicationPrivate::lastCursorPosition = QPointF(global);
		if (!under_.isNull())
			QWindowSystemInterface::handleLeaveEvent<Sync>(under_.data());
		if (w != nullptr)
			QWindowSystemInterface::handleEnterEvent<Sync>(w, local_of(w, global), QPointF(global));
		under_ = w;
		QGuiApplicationPrivate::lastCursorPosition = before;
	}
	send_mouse(w, QEvent::MouseMove, Qt::NoButton);
	QGuiApplicationPrivate::lastCursorPosition = QPointF(global);
}

void Input::press(Qt::MouseButton button)
{
	{
		char note[96];
		std::snprintf(note, sizeof(note), "press %d at %d,%d", static_cast<int>(button), pos_.x(), pos_.y());
		crash_note(note);
	}
	QWindow *w = window_at(pos_);
	// 押したウィンドウを前に出し、フォーカスのあるウィンドウにする (Windows が、ボタンを押したウィンドウを
	// アクティブにするのと同じ。WM_MOUSEACTIVATE。メニューは除く)。プレビュー用のプラットフォームプラグインは OS の
	// ウィンドウのアクティブを持たないので、ここでその通知を Qt へ送る。モーダルのウィンドウに止められている
	// ウィンドウはアクティブにしない (Windows は止められたウィンドウを EnableWindow(FALSE) にしていて、押しても
	// アクティブにならない。Qt もボタンを押すイベントを捨てる)。
	if (w != nullptr && QApplication::activePopupWidget() == nullptr && QGuiApplication::focusWindow() != w &&
	    !(w->flags() & Qt::WindowDoesNotAcceptFocus) && !QGuiApplicationPrivate::instance()->isWindowBlocked(w)) {
		activate_native(w);
		last_active_ = w;
	}
	buttons_ |= button;
	grab_ = w;
	++presses_;
	send_mouse(w, QEvent::MouseButtonPress, button);
}

void Input::release(Qt::MouseButton button)
{
	{
		char note[96];
		std::snprintf(note, sizeof(note), "release %d at %d,%d", static_cast<int>(button), pos_.x(), pos_.y());
		crash_note(note);
	}
	QWindow *w = !grab_.isNull() ? grab_.data() : window_at(pos_);
	buttons_ &= ~button;
	send_mouse(w, QEvent::MouseButtonRelease, button);
	if (buttons_ == Qt::NoButton)
		grab_.clear();
}

void Input::activate_window(QWidget *window)
{
	if (window == nullptr)
		return;
	QWidget *top = window->window();
	if (top->windowHandle() == nullptr)
		top->winId(); // Qt のウィンドウを作る (プラットフォームプラグインは OS のウィンドウを作らない)
	activate_native(top->windowHandle());
}

void Input::activate(QWidget *window)
{
	if (window == nullptr)
		return;
	activate_window(window);
	last_active_ = window->window()->windowHandle();
}

void Input::click(Qt::MouseButton button)
{
	press(button);
	release(button);
}

void Input::wheel(int delta)
{
	QWindow *w = window_at(pos_);
	if (w == nullptr)
		return;
	QWindowSystemInterface::handleWheelEvent(w, local_of(w, pos_), QPointF(pos_), QPoint(), QPoint(0, delta));
}

Input::Input()
{
	auto *watcher = new HideWatcher();
	watcher->last_active = &last_active_;
	watcher_ = watcher;
	qApp->installEventFilter(watcher);
}

Input::~Input()
{
	qApp->removeEventFilter(watcher_);
	delete watcher_;
}

QWindow *Input::key_window()
{
	if (QWidget *popup = QApplication::activePopupWidget()) {
		if (popup->windowHandle() != nullptr)
			return popup->windowHandle();
	}
	QWindow *fw = QGuiApplication::focusWindow();
	if (fw != nullptr && fw->isVisible() && fw->type() != Qt::Popup) {
		last_active_ = fw;
		return fw;
	}
	// Windows ではメニュー (Qt の popup) はアクティブを取らないので、メニューを閉じても前のウィンドウが
	// アクティブのまま。プレビュー用のプラットフォームプラグインはウィンドウを作るたびにアクティブにする
	// (tests/ui_preview/preview_platform.cpp の createPlatformWindow) ので、メニューを閉じるとアクティブな
	// ウィンドウがなくなる。Windows に合わせて、最後にアクティブだったウィンドウに戻す。
	if (!last_active_.isNull() && last_active_->isVisible()) {
		activate_native(last_active_.data());
		return last_active_.data();
	}
	return nullptr;
}

void Input::key(int qt_key, Qt::KeyboardModifiers modifiers, const QString &text)
{
	{
		char note[96];
		std::snprintf(note, sizeof(note), "key 0x%x mods 0x%x", qt_key, static_cast<unsigned>(modifiers));
		crash_note(note);
	}
	QWindow *w = key_window();
	if (w == nullptr)
		return;
	QWindowSystemInterface::handleKeyEvent<Sync>(w, QEvent::KeyPress, qt_key, modifiers, text);
	QWindowSystemInterface::handleKeyEvent<Sync>(w, QEvent::KeyRelease, qt_key, modifiers, text);
}

void Input::context_menu_key(Qt::KeyboardModifiers modifiers)
{
	// Windows のプラットフォームプラグイン (qwindowscontext.cpp の WM_CONTEXTMENU) と同じにする。Menu キーと
	// Shift+F10 では、OS が WM_CONTEXTMENU を位置なし (-1) で送り、Qt がキーボードからのコンテキストメニューの
	// イベントにする。プレビュー用のプラットフォームプラグインは OS のキーの変換を持たないので、最後の
	// handleContextMenuEvent (キーボード版) だけをここで行う。位置はフォーカスのあるウィジェットの中央にする
	// (QWidgetWindow は、キーボードからのイベントをフォーカスのあるウィジェットへ送り、位置はそのウィジェットが
	// 改めて決める)。
	QWindow *w = key_window();
	if (w == nullptr || w->type() == Qt::Popup)
		return;
	QPoint global = w->geometry().center();
	if (QWidget *focused = QApplication::focusWidget())
		global = focused->mapToGlobal(focused->rect().center());
	last_context_accepted_ =
		QWindowSystemInterface::handleContextMenuEvent<Sync>(w, false, w->mapFromGlobal(global), global, modifiers);
}

} // namespace cordmux::dock_drive
