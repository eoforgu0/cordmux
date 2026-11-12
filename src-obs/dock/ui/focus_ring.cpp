// SPDX-License-Identifier: Apache-2.0

// focus_ring.h の実装。

#include "dock/ui/focus_ring.h"

#include <QAbstractButton>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QCheckBox>
#include <QEvent>
#include <QKeyEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QStyleOptionButton>

#include <algorithm>

namespace cordmux::dock::ui {

namespace {

// 最後の入力の監視 (アプリケーション全体で 1 つ)。リングが 1 つでもある間だけ、アプリケーションのイベントフィルター
// として登録する。
class InputModality : public QObject {
public:
	static InputModality *instance;
	static std::vector<FocusRing *> rings;
	static bool keyboard;

	static bool released;

	static void attach(FocusRing *ring)
	{
		rings.push_back(ring);
		if (instance == nullptr && !released && QCoreApplication::instance() != nullptr) {
			instance = new InputModality();
			QCoreApplication::instance()->installEventFilter(instance);
		}
	}

	static void detach(FocusRing *ring)
	{
		rings.erase(std::remove(rings.begin(), rings.end(), ring), rings.end());
		if (rings.empty())
			remove_filter();
	}

	// リングが残っていてもイベントフィルターを外す (モジュールをアンロードする前に呼ぶ)。
	static void remove_filter()
	{
		if (instance == nullptr)
			return;
		if (QCoreApplication::instance() != nullptr)
			QCoreApplication::instance()->removeEventFilter(instance);
		delete instance;
		instance = nullptr;
	}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		switch (event->type()) {
		case QEvent::KeyPress: {
			// 修飾キー (Shift、Ctrl、Alt) だけでは切り替えない。Ctrl を押しながらのクリックで、リングを表示
			// しないため。
			const int key = static_cast<QKeyEvent *>(event)->key();
			if (key != Qt::Key_Shift && key != Qt::Key_Control && key != Qt::Key_Alt &&
			    key != Qt::Key_Meta && key != Qt::Key_AltGr)
				set_keyboard(true);
			break;
		}
		case QEvent::MouseButtonPress:
		case QEvent::MouseButtonDblClick:
		case QEvent::TouchBegin:
		case QEvent::TabletPress:
			set_keyboard(false);
			break;
		default:
			break;
		}
		return QObject::eventFilter(watched, event);
	}

private:
	static void set_keyboard(bool on)
	{
		if (keyboard == on)
			return;
		keyboard = on;
		// コピーに対して繰り返す。update_ring の中でリングが削除されて rings が変わっても、繰り返しを
		// 壊さないため。
		const std::vector<FocusRing *> copy = rings;
		for (FocusRing *r : copy)
			r->update_ring();
	}
};

InputModality *InputModality::instance = nullptr;
std::vector<FocusRing *> InputModality::rings;
bool InputModality::keyboard = false;
bool InputModality::released = false;

// cordmux.dll がアンロードされるとき (DLL の静的なオブジェクトの破棄。モジュールのアンロードの最後) に、イベント
// フィルターが残っていれば外す。release_focus_input_watch を呼び忘れたときも、アンロードされたコードを Qt が
// 呼ばないようにするため。アプリケーションが先に破棄されていれば QCoreApplication::instance() は nullptr なので、
// 何もしない。
struct FilterGuard {
	~FilterGuard() { InputModality::remove_filter(); }
};
FilterGuard g_filter_guard;

constexpr const char *kObjectName = "cordmuxFocusRing";

} // namespace

void release_focus_input_watch()
{
	InputModality::released = true;
	InputModality::remove_filter();
	InputModality::keyboard = false;
	// 残っているリングは隠す (キーボードの監視がないので、もう表示を切り替えられない)。
	const std::vector<FocusRing *> copy = InputModality::rings;
	for (FocusRing *r : copy)
		r->hide();
}

QRect focus_hit_rect_of(const QWidget *w)
{
	if (w == nullptr)
		return QRect();
	if (const auto *hit = dynamic_cast<const FocusHitRect *>(w))
		return hit->focus_hit_rect();
	if (const auto *button = qobject_cast<const QPushButton *>(w)) {
		QStyleOptionButton option;
		option.initFrom(button);
		option.text = button->text();
		option.icon = button->icon();
		option.iconSize = button->iconSize();
		if (button->isDefault())
			option.features |= QStyleOptionButton::DefaultButton;
		return button->style()->subElementRect(QStyle::SE_PushButtonBevel, &option, button);
	}
	if (const auto *check = qobject_cast<const QCheckBox *>(w)) {
		QStyleOptionButton option;
		option.initFrom(check);
		option.text = check->text();
		option.icon = check->icon();
		option.iconSize = check->iconSize();
		const QRect r = check->style()->subElementRect(QStyle::SE_CheckBoxClickRect, &option, check);
		return r.isValid() ? r : check->rect();
	}
	if (const auto *radio = qobject_cast<const QRadioButton *>(w)) {
		QStyleOptionButton option;
		option.initFrom(radio);
		option.text = radio->text();
		option.icon = radio->icon();
		option.iconSize = radio->iconSize();
		const QRect r = radio->style()->subElementRect(QStyle::SE_RadioButtonClickRect, &option, radio);
		return r.isValid() ? r : radio->rect();
	}
	return w->rect();
}

// ドック、設定ウィンドウ、確認と結果のダイアログが使う同じ判定 (focus_ring.h のコメント)。
QRect focus_mark_rect(const QScrollArea *scroll, const QWidget *w, int reach)
{
	const QWidget *content = scroll->widget();
	QRect r = w->rect();
	const QRect hit = focus_hit_rect_of(w);
	if (hit.isValid())
		r |= hit.adjusted(-reach, -reach, reach, reach);
	return QRect(w->mapTo(content, r.topLeft()), r.size()) & content->rect();
}

bool focus_mark_shown(const QScrollArea *scroll, const QWidget *w, int reach)
{
	const QWidget *viewport = scroll->viewport();
	if (!viewport->rect().contains(QRect(w->mapTo(viewport, QPoint(0, 0)), w->size())))
		return false;
	const QRect mark = focus_mark_rect(scroll, w, reach);
	const int top = scroll->widget()->mapTo(viewport, mark.topLeft()).y();
	return top >= 0 && top + mark.height() <= viewport->height();
}

void reveal_focus_mark(QScrollArea *scroll, const QWidget *w, int reach)
{
	if (scroll == nullptr || w == nullptr || scroll->widget() == nullptr || !scroll->widget()->isAncestorOf(w))
		return;
	if (focus_mark_shown(scroll, w, reach))
		return;
	constexpr int kMargin = 50;
	const QRect r = focus_mark_rect(scroll, w, reach);
	QScrollBar *bar = scroll->verticalScrollBar();
	const int view_h = scroll->viewport()->height();
	int value = bar->value();
	if (r.height() + 2 * kMargin > view_h || r.top() < value)
		value = r.top() - (r.height() + 2 * kMargin > view_h ? 0 : kMargin);
	else
		value = r.bottom() + 1 + kMargin - view_h;
	bar->setValue(std::clamp(value, bar->minimum(), bar->maximum()));
}

bool scroll_follows_focus(Qt::FocusReason reason)
{
	switch (reason) {
	case Qt::TabFocusReason:
	case Qt::BacktabFocusReason:
	case Qt::ShortcutFocusReason:
	case Qt::OtherFocusReason:
	case Qt::PopupFocusReason:
		return true;
	case Qt::MouseFocusReason:
	case Qt::ActiveWindowFocusReason:
	case Qt::MenuBarFocusReason:
	case Qt::NoFocusReason:
		return false;
	}
	return false;
}

FocusRing::FocusRing(QWidget *scope) : QWidget(scope), scope_(scope)
{
	setObjectName(QString::fromLatin1(kObjectName));
	setAttribute(Qt::WA_TransparentForMouseEvents, true);
	setAttribute(Qt::WA_NoSystemBackground, true);
	setFocusPolicy(Qt::NoFocus);
	hide();
	InputModality::attach(this);
	if (auto *app = qobject_cast<QApplication *>(QCoreApplication::instance())) {
		QObject::connect(app, &QApplication::focusChanged, this,
				 [this](QWidget *, QWidget *now) { retarget(now); });
	}
	retarget(QApplication::focusWidget());
}

FocusRing::~FocusRing()
{
	unwatch();
	InputModality::detach(this);
}

bool FocusRing::keyboard_mode()
{
	return InputModality::keyboard;
}

void FocusRing::set_color(const QColor &color)
{
	if (color_ == color)
		return;
	color_ = color;
	update();
}

void FocusRing::set_metrics(int ring_px, int gap_px)
{
	ring_px_ = std::max(1, ring_px);
	gap_px_ = std::max(0, gap_px);
	place();
}

void FocusRing::retarget(QWidget *now)
{
	QWidget *next = nullptr;
	if (now != nullptr && now != this && !scope_.isNull() && scope_->isAncestorOf(now))
		next = now;
	if (next != target_.data()) {
		unwatch();
		target_ = next;
		if (next != nullptr)
			watch(next);
	}
	update_ring();
}

void FocusRing::watch(QWidget *target)
{
	// ウィジェットと、scope までの祖先の移動を監視する (スクロールで中身が動いたときも、リングを追従させるため)。
	for (QWidget *w = target; w != nullptr && w != scope_.data(); w = w->parentWidget()) {
		w->installEventFilter(this);
		watched_.emplace_back(w);
	}
}

void FocusRing::unwatch()
{
	for (const QPointer<QWidget> &w : watched_) {
		if (!w.isNull())
			w->removeEventFilter(this);
	}
	watched_.clear();
}

bool FocusRing::eventFilter(QObject *watched, QEvent *event)
{
	switch (event->type()) {
	case QEvent::Move:
	case QEvent::Resize:
	case QEvent::Show:
	case QEvent::Hide:
	case QEvent::EnabledChange:
		update_ring();
		break;
	case QEvent::ParentChange:
		// 祖先が変わったので、監視を登録し直す。
		if (!target_.isNull()) {
			QWidget *t = target_.data();
			unwatch();
			watch(t);
		}
		update_ring();
		break;
	default:
		break;
	}
	return QWidget::eventFilter(watched, event);
}

void FocusRing::update_ring()
{
	QWidget *t = target_.data();
	if (t == nullptr || scope_.isNull() || !keyboard_mode() || !t->isVisible() || !t->hasFocus()) {
		hide();
		return;
	}
	place();
}

void FocusRing::place()
{
	QWidget *t = target_.data();
	QWidget *scope = scope_.data();
	if (t == nullptr || scope == nullptr || !keyboard_mode())
		return;
	const QRect hit = focus_hit_rect_of(t);
	if (!hit.isValid()) {
		hide();
		return;
	}
	const int pad = ring_px_ + gap_px_;
	const QRect in_scope(t->mapTo(scope, hit.topLeft()), hit.size());
	const QRect ring = in_scope.adjusted(-pad, -pad, pad, pad);
	// スクロール領域の見える範囲 (QAbstractScrollArea の viewport) と scope だけで切り、スクロール領域の外に
	// はみ出したリングを描かない。行やカードのような途中の祖先では切らない。リングは押せる範囲の 3 px 外側を
	// 囲むので、押せる範囲が祖先の端に接するウィジェット (行のチェックと [...]) では、祖先で切ると上下と片側の
	// 辺が消え、縦の 1 本だけが残るため。リングが隣の行に 3 px 重なるのは Qt の QFocusFrame と同じである
	// (リングは透明で、クリックを受けない)。
	QRect visible = scope->rect();
	for (QWidget *w = t->parentWidget(); w != nullptr && w != scope; w = w->parentWidget()) {
		const auto *area = qobject_cast<const QAbstractScrollArea *>(w->parentWidget());
		if (area != nullptr && area->viewport() == w)
			visible &= QRect(w->mapTo(scope, QPoint(0, 0)), w->size());
	}
	const QRect shown = ring & visible;
	if (shown.isEmpty()) {
		hide();
		return;
	}
	setGeometry(ring);
	clip_ = shown.translated(-ring.topLeft());
	raise();
	show();
	update();
}

void FocusRing::paintEvent(QPaintEvent *)
{
	if (!color_.isValid())
		return;
	QPainter p(this);
	p.setClipRect(clip_);
	p.setRenderHint(QPainter::Antialiasing, false);
	QPen pen(color_, ring_px_);
	pen.setJoinStyle(Qt::MiterJoin);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	const qreal half = ring_px_ / 2.0;
	p.drawRect(QRectF(rect()).adjusted(half, half, -half, -half));
}

} // namespace cordmux::dock::ui
