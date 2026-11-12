// SPDX-License-Identifier: Apache-2.0

// ドックの下のバー (bottom_bar.h) を実装する。

#include "dock/bottom_bar.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>

#include "dock/layout_hold.h"
#include "dock/ui/elide_label.h"
#include "dock/ui/set_class.h"
#include "dock/ui/status_icons.h"
#include "dock/ui/text_resolve.h"
#include "ledger/text_ids.h"

namespace cordmux::dock {

namespace {

const char *record_state_name(ledger::dockview::RecordButtonState s)
{
	switch (s) {
	case ledger::dockview::RecordButtonState::Idle:
		return "Idle";
	case ledger::dockview::RecordButtonState::Starting:
		return "Starting";
	case ledger::dockview::RecordButtonState::Recording:
		return "Recording";
	case ledger::dockview::RecordButtonState::Stopping:
		return "Stopping";
	case ledger::dockview::RecordButtonState::Failed:
		return "Failed";
	}
	return "Idle";
}

} // namespace

RecordButton::RecordButton(QWidget *parent) : ui::RecordButtonBase(parent)
{
	setObjectName(QStringLiteral("cordmuxRecordButton"));
	setAutoDefault(false);
}

BottomBar::BottomBar(QWidget *parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("cordmuxBottomBar"));
	v_ = new QVBoxLayout(this);
	row1_ = new QHBoxLayout();
	row1_->setContentsMargins(0, 0, 0, 0);
	record_ = new RecordButton(this);
	QSizePolicy record_policy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	record_policy.setRetainSizeWhenHidden(true);
	record_->setSizePolicy(record_policy);
	// クリックではフォーカスを取らない。OBS が前面のまま Space を押して、同じ操作が働くのを防ぐ。
	record_->setFocusPolicy(Qt::TabFocus);
	// accessibleDescription は、状態に合わせて set_record が設定する。
	row1_->addWidget(record_, 1);
	menu_ = new QPushButton(this);
	menu_->setObjectName(QStringLiteral("cordmuxDockMenuButton"));
	menu_->setAutoDefault(false);
	menu_->setFocusPolicy(Qt::TabFocus);
	ui::set_class(menu_, QStringLiteral("btn-tool icon-dots-vert"));
	const QString more = ui::resolve_key(ledger::text::keys::kDockMoreActionsA11y);
	menu_->setToolTip(more);
	menu_->setAccessibleName(more);
	row1_->addWidget(menu_, 0);
	v_->addLayout(row1_);

	QObject::connect(record_, &QPushButton::clicked, this, [this]() {
		if (has_view_ && on_record_)
			on_record_(view_.action);
	});
	QObject::connect(menu_, &QPushButton::clicked, this, [this]() {
		if (on_menu_)
			on_menu_();
	});
}

void BottomBar::set_look(const DockLook &look)
{
	look_ = look;
	has_look_ = true;
	// 左右の余白はドックの外周の余白 (edge_text_px)、上下は小さい余白にする。
	const int edge = look_.metrics.edge_text_px;
	const int small = look_.metrics.small_px;
	v_->setContentsMargins(edge, small, edge, small);
	v_->setSpacing(small);
	// 録画のボタンと [...] の間は、どちらかにフォーカスがあるときに、フォーカスリング (空き + 線) の外に背景の色が
	// 1 px 以上残る幅にする。大きい余白を使い、小さいフォントで大きい余白がリング + 1 px を下回るときだけリング + 1 px
	// にする (行の共有のアイコンと [...] の間を決める name_check.h の dots_gap_px と同じ)。小さい余白 (10 pt で 3 px) の
	// ままだと 3 px のリングで使い切り、リングが隣のボタンの背景に接して、コントラスト比が足りなくなる。リングの色を
	// 隣の背景に合わせて変えることはしない。フォーカスリングの形と色は、どこでも同じにする。
	row1_->setSpacing(ring_neighbor_gap_px(look_.metrics.large_px));
	apply_menu_icon();
}

void BottomBar::apply_menu_icon()
{
	// テーマが [...] のアイコンを持たないときは、同じ意味の cordmux のアイコンを本文の色で描く。
	// テーマが持つときは何もしない。アイコンは、テーマの qproperty-icon が polish のときに設定する。ここで setIcon を
	// 呼ぶと、空のアイコンでもテーマのアイコンを上書きして消す。前に cordmux のアイコンを入れていたら、polish し直して
	// テーマのアイコンに戻す。
	if (!look_.theme.has_dots) {
		menu_->setIcon(ui::fallback_dots_icon(look_.theme, look_.metrics.icon_px, look_.theme.text));
		menu_->setIconSize(QSize(look_.metrics.icon_px, look_.metrics.icon_px));
		fallback_icon_ = true;
	} else if (fallback_icon_) {
		menu_->setIcon(QIcon());
		menu_->style()->unpolish(menu_);
		menu_->style()->polish(menu_);
		fallback_icon_ = false;
	}
}

void BottomBar::set_record(const ledger::dockview::RecordButtonView &view)
{
	if (has_view_ && view == view_)
		return;
	view_ = view;
	has_view_ = true;

	QStringList widths;
	for (const ledger::TextRef &ref : view_.width_labels)
		widths << ui::resolve(ref);
	record_->set_width_labels(widths);
	// ボタンのテキスト (読み上げソフトが読む名前) は経過時間を除いた名前にし、経過時間を含む見えるテキストは別に渡す
	// (RecordButtonView::name。名前を毎秒変えると、フォーカスがある間、経過時間を読み続ける)。
	const QString label = ui::resolve(view_.label);
	const QString name = ui::resolve(view_.name);
	if (record_->text() != name)
		record_->setText(name);
	record_->set_display_text(label == name ? QString() : label);
	ui::set_style_property(record_, "recordState", QString::fromLatin1(record_state_name(view_.state)));

	// 押された見た目にするのは、録画の側が保存先に書けたと返したときだけにする。押した時点で録画中の見た目にすると、
	// 開始に失敗したときにボタンが実際と違う状態を示す。終了の処理中は、押された見た目のまま押せなくする。
	record_->set_recording_look(view_.pressed_look);
	const bool enabled = !view_.action.disabled_reason.has_value();
	record_->set_pressed_look_while_disabled(view_.pressed_look && !enabled);

	const bool had_focus = record_->hasFocus();
	const bool was_enabled = record_->isEnabled();
	record_->setEnabled(enabled);
	if (was_enabled && !enabled && had_focus) {
		// 押せなくすると、Qt はフォーカスを次のウィジェットに移す。移した先を保存しておき、処理が済んだときに
		// フォーカスがその先のまま (利用者が動かしていない) なら、ボタンに戻す。
		focus_moved_to_ = QApplication::focusWidget();
	} else if (!was_enabled && enabled) {
		if (!focus_moved_to_.isNull() && QApplication::focusWidget() == focus_moved_to_.data())
			record_->setFocus(Qt::OtherFocusReason);
		focus_moved_to_.clear();
	}

	const QString reason = enabled ? QString() : ui::resolve(*view_.action.disabled_reason);
	// 押せない理由がボタンのテキストと同じとき (処理中のテキストが理由を兼ねるとき) は、ツールチップと
	// accessibleDescription で繰り返さない。
	const QString extra_reason = reason == label ? QString() : reason;
	// accessibleDescription は、どの状態でも cordmux.dock.record.a11y (「画面共有と声を、参加者ごとに別のファイルに
	// 録画します。」) にする。アクセシブル名 (見えているテキスト) は「cordmuxの録画を開始」のように何の録画かを言うが、
	// OBS の録画と何が違うか (参加者ごとに別のファイル) は言わないので、説明で言う。処理中のテキスト (開始処理中... と
	// 停止処理中...) は目的語を省いているが、説明で録画のボタンだと分かる。押せない理由は、ボタンのテキストと違う
	// ときだけ説明に追加する。
	// ツールチップは、開始のボタン ([cordmuxの録画を開始]。録画できなかった後も同じテキスト) にだけ同じ文を表示する。
	// 終了のボタンと処理中には表示しない。終了で何が起きるかは名前で分かり、表示すると配信中に何度も出て邪魔になる。
	// 状態ではなく表示しているテキストで判断するのは、処理中になってから 1 秒までは前のテキストのままだからである。
	// 押せない理由は、ツールチップの 2 行目に追加する。
	const QString what = ui::resolve_key(ledger::text::keys::kDockRecordA11y);
	QStringList tip;
	if (view_.label.id == ledger::text::keys::kDockRecordStartButton)
		tip << what;
	if (!extra_reason.isEmpty())
		tip << extra_reason;
	record_->setToolTip(tip.join(QLatin1Char('\n')));
	QStringList desc;
	desc << what;
	if (!extra_reason.isEmpty())
		desc << extra_reason;
	record_->setAccessibleDescription(desc.join(QLatin1Char('\n')));
}

void BottomBar::set_record_hidden(bool hidden)
{
	record_->setVisible(!hidden);
}

int BottomBar::minimum_bar_width() const
{
	const int edge = has_look_ ? look_.metrics.edge_text_px : ui::DockMetrics().edge_text_px;
	return record_->minimumSizeHint().width() + row1_->spacing() + menu_->sizeHint().width() + 2 * edge;
}

int BottomBar::ring_neighbor_gap_px(int base_px) const
{
	const ui::DockMetrics &m = look_.metrics;
	return std::max(base_px, m.focus_ring_px + m.focus_ring_gap_px + 1);
}

} // namespace cordmux::dock
