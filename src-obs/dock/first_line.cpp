// SPDX-License-Identifier: Apache-2.0

// ドックの 1 行目 (first_line.h) を実装する。

#include "dock/first_line.h"

#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QResizeEvent>
#include <QStringList>
#include <QTimer>

#include <algorithm>

#include "dock/layout_hold.h"
#include "dock/ui/dock_ui_calc.h"
#include "dock/ui/elide_label.h"
#include "dock/ui/set_class.h"
#include "dock/ui/status_icons.h"
#include "dock/ui/surface_frame.h"
#include "dock/ui/text_resolve.h"
#include "dock/ui/type_scale.h"
#include "ledger/text_ids.h"

namespace cordmux::dock {

namespace {

ui::TierMark tier_mark_of(ledger::dockview::Tier tier)
{
	switch (tier) {
	case ledger::dockview::Tier::L1:
		return ui::TierMark::L1;
	case ledger::dockview::Tier::L2:
		return ui::TierMark::L2;
	case ledger::dockview::Tier::L3:
		return ui::TierMark::L3;
	}
	return ui::TierMark::L3;
}

} // namespace

FirstLineBar::FirstLineBar(QWidget *parent) : QAbstractButton(parent)
{
	setObjectName(QStringLiteral("cordmuxFirstLine"));
	// クリックではフォーカスを取らない。OBS が前面のまま Space を押して、同じ操作が働くのを防ぐ。
	setFocusPolicy(Qt::TabFocus);
	setAttribute(Qt::WA_Hover, true);
	setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
	box_ = new QHBoxLayout(this);
	box_->setContentsMargins(0, 0, 0, 0);
	mark_ = new ui::MarkLabel(this);
	mark_->hide();
	box_->addWidget(mark_, 0, Qt::AlignVCenter);
	text_ = new ui::ElideLabel(this);
	// テキストのラベルは自分のツールチップを持たない。1 行目のウィジェットが、全文と押したときの動作をまとめて 1 つの
	// ツールチップにする。省いたときの全文は、アクセシブル名に残る。
	text_->set_own_tool_tip(false);
	box_->addWidget(text_, 1, Qt::AlignVCenter);
	more_ = new QLabel(this);
	more_->setTextFormat(Qt::PlainText);
	more_->hide();
	box_->addWidget(more_, 0, Qt::AlignVCenter);
	button_ = new QPushButton(this);
	// ボタンはテキストの幅のままにする。
	button_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
	button_->setFocusPolicy(Qt::TabFocus);
	button_->setAutoDefault(false);
	button_->hide();
	new PressGuard(button_);
	box_->addWidget(button_, 0, Qt::AlignRight | Qt::AlignVCenter);
	QObject::connect(button_, &QPushButton::clicked, this, [this]() {
		if (button_view_.has_value() && on_button_)
			on_button_(*button_view_);
	});
	go_ = new ui::MarkLabel(this);
	box_->addWidget(go_, 0, Qt::AlignVCenter);
	// アイコンとテキストのラベルはマウスのイベントを受けない。1 行目のどこを押しても状態タブを開き、ツールチップをいつも
	// 1 行目のウィジェットのものにするためである。
	for (QWidget *w : {static_cast<QWidget *>(mark_), static_cast<QWidget *>(text_), static_cast<QWidget *>(more_),
			   static_cast<QWidget *>(go_)})
		w->setAttribute(Qt::WA_TransparentForMouseEvents, true);
}

void FirstLineBar::read_fonts()
{
	// 1 行目のテキストに使う 2 つのフォントを、見えないラベルで読む。テーマのフォントに倍率を掛けた後の値がほしいので、
	// QFont を計算せず、テキストのラベルと同じ処理を通す。
	QLabel probe(this);
	probe.hide();
	ui::apply_type(&probe, ui::TypeLevel::FirstLineSummary, look_.base_pt, look_.style.first_line_scale);
	probe.ensurePolished();
	large_font_ = probe.font();
	ui::apply_type(&probe, ui::TypeLevel::Body, look_.base_pt, look_.style.first_line_scale);
	probe.ensurePolished();
	body_font_ = probe.font();
}

void FirstLineBar::set_look(const DockLook &look)
{
	look_ = look;
	box_->setSpacing(look_.metrics.small_px);
	// 左右は外周の余白にそろえ、メッセージのレベル 1 の枠線の内側にテキストを収める。
	const int edge = std::max(look_.metrics.edge_text_px, look_.style.l1_border_auto_px);
	box_->setContentsMargins(edge, 0, edge, 0);
	read_fonts();
	apply_type();
	apply_colors();

	// 高さは状態で変えない。1 行目の倍率のテキスト 1 行 (日本語、英字、下に伸びる文字を含めて Qt のラベルに測らせる)、ボタンの
	// 高さ、アイコンの大きさのうち最も大きいものに、上下の余白を足した高さを、いつも確保する。
	auto *probe = new QLabel(QStringLiteral("あAgy"), this);
	probe->setTextFormat(Qt::PlainText);
	probe->hide();
	ui::apply_type(probe, ui::TypeLevel::FirstLineHeading, look_.base_pt, look_.style.first_line_scale);
	probe->ensurePolished();
	const int text_h = probe->sizeHint().height();
	delete probe;
	button_->ensurePolished();
	const int content = std::max({text_h, button_->sizeHint().height(), look_.metrics.icon_px});
	setFixedHeight(content + 2 * std::max(look_.metrics.small_px, look_.style.l1_border_auto_px));
	refit_later();
}

void FirstLineBar::apply_type()
{
	const bool heading = has_line_ && line_.kind == ledger::dockview::FirstLineKind::Notice;
	if (text_small_) {
		ui::apply_type(text_, ui::TypeLevel::Body, look_.base_pt, look_.style.first_line_scale);
		ui::apply_type(more_, ui::TypeLevel::Body, look_.base_pt, look_.style.first_line_scale);
		return;
	}
	const ui::TypeLevel level = heading ? ui::TypeLevel::FirstLineHeading : ui::TypeLevel::FirstLineSummary;
	ui::apply_type(text_, level, look_.base_pt, look_.style.first_line_scale);
	ui::apply_type(more_, level, look_.base_pt, look_.style.first_line_scale);
}

void FirstLineBar::apply_colors()
{
	if (on_surface_) {
		ui::paint_on_surface(text_, look_.l1.foreground);
		ui::paint_on_surface(more_, look_.l1.foreground);
	} else {
		ui::clear_surface_paint(text_);
		ui::clear_surface_paint(more_);
	}
	const QColor color = on_surface_ ? look_.l1.foreground : look_.theme.text;
	const int px = look_.metrics.icon_px;
	// メッセージのレベルのアイコンは、メッセージの見出しのときだけ表示する。
	const bool show_mark = has_line_ && line_.kind == ledger::dockview::FirstLineKind::Notice && line_.tier.has_value();
	if (show_mark) {
		const ui::TierMark mark = tier_mark_of(*line_.tier);
		const ui::TierIconSet set = look_.style.tier_icon_set;
		mark_->set_painter(px, [mark, px, color, set](qreal dpr) { return ui::tier_pixmap(mark, px, color, dpr, set); });
	}
	mark_->setVisible(show_mark);
	go_->set_painter(px, [px, color](qreal dpr) { return ui::chevron_pixmap(ui::Chevron::Right, px, color, dpr); });
	update();
}

void FirstLineBar::set_text_small(bool on)
{
	if (text_small_ == on)
		return;
	text_small_ = on;
	apply_type();
}

void FirstLineBar::set_line(const ledger::dockview::FirstLine &line, bool on_surface)
{
	const bool kind_changed = !has_line_ || line.kind != line_.kind;
	const bool changed = !has_line_ || line != line_ || on_surface != on_surface_;
	if (!changed)
		return;
	line_ = line;
	has_line_ = true;
	on_surface_ = on_surface;
	if (kind_changed) {
		text_small_ = false;
		apply_type();
	}
	apply_colors();
	// プレビューツールとドックを操作するテストが読む (メッセージのレベル 1 の背景と枠線)。
	setProperty("l1Surface", on_surface_);
	setProperty("l1BorderPx", on_surface_ ? look_.l1.border_px : 0);

	QStringList forms;
	for (const ledger::TextRef &ref : line_.forms)
		forms << ui::resolve(ref);
	text_->set_forms(forms);

	if (line_.more_count.has_value()) {
		more_->setText(ui::resolve(*line_.more_count));
		more_->show();
	} else {
		more_->clear();
		more_->hide();
	}

	button_view_ = line_.button;
	const bool had_button = !button_->isHidden();
	if (button_view_.has_value()) {
		button_->setEnabled(!button_view_->disabled_reason.has_value());
		button_->setToolTip(button_view_->disabled_reason.has_value() ? ui::resolve(*button_view_->disabled_reason)
									      : QString());
		ui::set_class(button_, button_view_->primary ? QStringLiteral("button-primary") : QString());
		button_->show();
		if (!had_button)
			PressGuard::arm_widget(button_); // 現れた直後のクリックを捨てる
	} else {
		button_->hide();
	}
	go_->setVisible(!button_view_.has_value());
	refit_later();
}

int FirstLineBar::text_width() const
{
	return text_->isVisible() ? text_->width() : 0;
}

QString FirstLineBar::shown_text() const
{
	return text_->isVisible() ? text_->text() : QString();
}

QFont FirstLineBar::text_font() const
{
	text_->ensurePolished();
	return text_->font();
}

QString FirstLineBar::full_text() const
{
	return text_->full_text();
}

int FirstLineBar::button_width_for(const QString &label) const
{
	// QPushButton::sizeHint をテキストを替えて読む。sizeHint はテキストごとにキャッシュを持たないので、見えないボタンで測る。
	QPushButton probe(const_cast<FirstLineBar *>(this));
	probe.hide();
	probe.setText(label);
	probe.ensurePolished();
	return probe.sizeHint().width();
}

int FirstLineBar::minimum_line_width_px() const
{
	const QString text = ui::resolve_key(ledger::text::keys::kDockSummaryDisabledStatus);
	const QMargins m = box_->contentsMargins();
	return m.left() + m.right() + QFontMetrics(body_font_).horizontalAdvance(text) + box_->spacing() +
	       button_width_for(ui::resolve_key(ledger::text::keys::kDockEnableShortButton));
}

QRect FirstLineBar::focus_hit_rect() const
{
	// リングはドックの矩形で切って描くので、幅いっぱいの押せる範囲のままだと左右の辺が消える。リングの太さとすき間の分だけ
	// 内側に寄せる。
	const int reach = look_.metrics.focus_ring_px + look_.metrics.focus_ring_gap_px;
	return rect().adjusted(reach, reach, -reach, -reach);
}

void FirstLineBar::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	if (on_surface_ && look_.l1.fill.isValid()) {
		p.fillRect(rect(), look_.l1.fill);
		if (look_.l1.border_px > 0 && look_.l1.border.isValid()) {
			const qreal half = look_.l1.border_px / 2.0;
			p.setPen(QPen(look_.l1.border, look_.l1.border_px));
			p.setBrush(Qt::NoBrush);
			p.drawRect(QRectF(rect()).adjusted(half, half, -half, -half));
		}
	}
	// ポインターの下と押している間は、背景のテキストの色を薄く重ねて、押せることを示す。
	if (isDown() || underMouse()) {
		QColor over = on_surface_ && look_.l1.foreground.isValid() ? look_.l1.foreground : look_.theme.text;
		over.setAlpha(isDown() ? 64 : 32);
		p.fillRect(rect(), over);
	}
}

void FirstLineBar::resizeEvent(QResizeEvent *event)
{
	QAbstractButton::resizeEvent(event);
	refit_later();
}

void FirstLineBar::changeEvent(QEvent *event)
{
	QAbstractButton::changeEvent(event);
	if (event->type() == QEvent::EnabledChange)
		update();
}

void FirstLineBar::refit_later()
{
	// 子のラベルの大きさは、この後のレイアウトの処理で決まるので、決まってから計算する。
	if (refit_pending_)
		return;
	refit_pending_ = true;
	QTimer::singleShot(0, this, [this]() {
		refit_pending_ = false;
		refit();
	});
}

void FirstLineBar::refit()
{
	if (!has_line_)
		return;
	// ボタンのある行 (無効の行と全画面の行) は、幅によらずテキストとボタンを両方残す。ボタンの短い形を持つのは無効の行の
	// [有効にする] だけで、全画面の行の [元に戻す] は同じテキストのまま、テキストのほうを小さくする (1 行目の倍率が本文と
	// 同じ 1 のときは大きさが変わらず、テキストを省くだけになる)。
	if (button_view_.has_value()) {
		const QString label = ui::resolve(button_view_->label);
		const QString short_label =
			line_.disabled ? ui::resolve_key(ledger::text::keys::kDockEnableShortButton) : label;
		const QMargins m = box_->contentsMargins();
		const QString text = text_->full_text();
		const ui::calc::DisabledLineFit fit = ui::calc::disabled_line_fit(
			width() - m.left() - m.right(), QFontMetrics(large_font_).horizontalAdvance(text), button_width_for(label),
			button_width_for(short_label), box_->spacing());
		set_text_small(fit.small_text);
		const QString want = fit.short_button ? short_label : label;
		if (button_->text() != want)
			button_->setText(want);
		box_->activate();
	} else {
		set_text_small(false);
	}
	text_->refit();

	// アクセシブル名は全文、説明とツールチップの最後の行は、押したときの動作にする。
	const QString full = text_->full_text();
	const QString what = ui::resolve_key(ledger::text::keys::kDockFirstLineA11y);
	setAccessibleName(full);
	setAccessibleDescription(what);
	QStringList tip;
	if (text_->is_elided())
		tip << full;
	tip << what;
	setToolTip(tip.join(QLatin1Char('\n')));
}

} // namespace cordmux::dock
