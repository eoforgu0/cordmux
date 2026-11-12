// SPDX-License-Identifier: Apache-2.0

// 音声タブの中身 (audio_tab.h) を実装する。

#include "dock/audio_tab.h"

#include <QApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>

#include "dock/layout_hold.h"
#include "dock/ui/dock_ui_calc.h"
#include "dock/ui/elide_label.h"
#include "dock/ui/text_resolve.h"
#include "dock/ui/wrap_label.h"
#include "ledger/audio_tab.h"
#include "ledger/text_ids.h"

namespace cordmux::dock {

namespace {

namespace keys = ledger::text::keys;

// 名前の列の最小の幅 (em)。名前は 1 行で省き、全文はツールチップに入れる。ドックの最小幅を決める値で、名前が短いときの
// 幅でもある。
constexpr double kNameEm = 5.5;
// 名前の欄が、つまみを最小のまま広がる上限の幅 (em)。16 文字ほどの名前が、ドックの幅 26 em で省かれずに出る長さにする。
// つまみが kSliderComfortEm まで広がったあとは、この幅を超えて全員の名前の全文の幅まで広がる (calc::name_column_px)。
constexpr double kNameMaxEm = 9.0;
// つまみのゆとりのある幅 (em)。0 から 200% を約 80 px で操作できる。名前の欄がさらに広がる前に、つまみをここまで広げる。
constexpr double kSliderComfortEm = 6.0;
// つまみの最小の幅 (em)。
constexpr double kSliderMinEm = 3.0;
// メーターの棒の高さ (em)。
constexpr double kMeterBarEm = 0.3;

QString value_text(int pct)
{
	ledger::TextRef ref;
	ref.id = keys::kDockRowVolumeValueLabel;
	ref.args.push_back(std::to_string(pct));
	return ui::resolve(ref);
}

// 1 行のラベル label に、text を欠けずに表示できる幅 (px)。QLabel が推奨の大きさを決めるのと同じ足し方にする (Qt の
// qlabel.cpp の sizeForWidth): ラベル自身のフォントでの文字の幅に、contentsMargins (テーマの padding と枠の幅、または
// setContentsMargins で入れた余白)、margin の 2 倍、字下げを足す。字下げは indent が負でラベルに枠の幅があるとき「x」の幅
// になり、文字を左か右に寄せたときに幅に入る。文字の幅は小数で測って切り上げる (文字の位置は小数で決まるので、四捨五入した
// 幅では右端の 1 px が欠けることがある)。フォントは、呼ぶ前にラベルを polish して、テーマのものにしておく。
// QLabel::sizeHint を使わないのは、固定の大きさを入れた後のラベルでは、その大きさより小さい値を返さないためである。
int label_text_px(const QLabel *label, const QString &text)
{
	const QFontMetricsF fm(label->font());
	const QMargins margins = label->contentsMargins();
	int extra = margins.left() + margins.right() + 2 * label->margin();
	int indent = label->indent();
	if (indent < 0 && label->frameWidth() > 0)
		indent = QFontMetrics(label->font()).horizontalAdvance(QLatin1Char('x')) - 2 * label->margin();
	if (indent > 0 && (label->alignment() & (Qt::AlignLeft | Qt::AlignRight)) != 0)
		extra += indent;
	return static_cast<int>(std::ceil(fm.horizontalAdvance(text))) + extra;
}

// 別々のテキスト (それぞれが ini の 1 つの文字列) を改行で並べる。空の行と、まったく同じ行は並べない。
QString join_lines(const std::vector<QString> &lines)
{
	QString out;
	std::vector<QString> seen;
	for (const QString &line : lines) {
		if (line.isEmpty() || std::find(seen.begin(), seen.end(), line) != seen.end())
			continue;
		seen.push_back(line);
		if (!out.isEmpty())
			out += QLatin1Char('\n');
		out += line;
	}
	return out;
}

} // namespace

// ---- AudioRowWidget ----

AudioRowWidget::AudioRowWidget(const std::string &user_id, const AudioEnv &env, QWidget *parent)
	: QWidget(parent),
	  user_id_(user_id),
	  env_(env)
{
	setObjectName(QStringLiteral("cordmuxAudioRow"));
	setProperty("userId", QString::fromStdString(user_id_));
	grid_ = new QGridLayout(this);

	name_ = new ui::ElideLabel(this);
	name_->setObjectName(QStringLiteral("cordmuxAudioName"));
	name_->setProperty("userId", QString::fromStdString(user_id_));
	grid_->addWidget(name_, 0, 0, Qt::AlignVCenter);

	build_line(voice_, AudioKind::Voice, 0);
	build_line(screen_, AudioKind::Screen, 1);
	grid_->setColumnStretch(2, 1);

	set_env(env);
}

void AudioRowWidget::build_line(Line &l, AudioKind kind, int grid_row)
{
	const bool voice = kind == AudioKind::Voice;
	const QString prefix = voice ? QStringLiteral("cordmuxAudioVoice") : QStringLiteral("cordmuxAudioScreen");
	const QString user = QString::fromStdString(user_id_);

	l.kind = new QLabel(ui::resolve_key(voice ? keys::kDockAudioVoiceKindLabel : keys::kDockAudioScreenKindLabel), this);
	l.kind->setObjectName(prefix + QStringLiteral("Kind"));
	l.kind->setTextFormat(Qt::PlainText);

	// つまみとメーターを置く場所。文を置く間は隠すが、場所は空けたままにする (列の幅を動かさない)。
	l.controls = new QWidget(this);
	l.controls->setObjectName(prefix + QStringLiteral("Place"));
	l.controls->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	QSizePolicy keep_controls = l.controls->sizePolicy();
	keep_controls.setRetainSizeWhenHidden(true);
	l.controls->setSizePolicy(keep_controls);
	auto *controls_layout = new QVBoxLayout(l.controls);
	controls_layout->setContentsMargins(0, 0, 0, 0);
	l.slider = new ui::VolumeSlider(l.controls);
	l.slider->setObjectName(prefix + QStringLiteral("Slider"));
	l.slider->setProperty("userId", user);
	l.slider->setToolTip(ui::resolve_key(keys::kDockAudioResetTip));
	l.slider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	l.meter = new ui::LevelMeter(l.controls);
	l.meter->setObjectName(prefix + QStringLiteral("Meter"));
	l.meter->setProperty("userId", user);
	controls_layout->addWidget(l.slider);
	controls_layout->addWidget(l.meter);
	// 文は、つまみの場所とパーセントの場所にまたがって置く (狭いドックでも、文が省かれにくい)。
	l.note = new ui::ElideLabel(this);
	l.note->setObjectName(prefix + QStringLiteral("Note"));
	l.note->setProperty("userId", user);
	l.note->hide();

	l.value = new QLabel(this);
	l.value->setObjectName(prefix + QStringLiteral("Value"));
	l.value->setTextFormat(Qt::PlainText);
	l.value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	// 文を置く行は、パーセントを隠しても場所を空ける (ミュートのボタンの位置を動かさない)。
	QSizePolicy keep_value = l.value->sizePolicy();
	keep_value.setRetainSizeWhenHidden(true);
	l.value->setSizePolicy(keep_value);

	l.mute = new MuteButton(voice ? ui::AudioMark::Voice : ui::AudioMark::Screen, this);
	l.mute->setObjectName(prefix + QStringLiteral("Mute"));
	l.mute->setProperty("userId", user);
	QSizePolicy keep_mute = l.mute->sizePolicy();
	keep_mute.setRetainSizeWhenHidden(true);
	l.mute->setSizePolicy(keep_mute);

	// 見出しとパーセントのフォントかスタイルが変わったら、幅を測り直す (eventFilter)。
	l.kind->installEventFilter(this);
	l.value->installEventFilter(this);

	grid_->addWidget(l.kind, grid_row, 1, Qt::AlignVCenter);
	grid_->addWidget(l.controls, grid_row, 2);
	grid_->addWidget(l.note, grid_row, 2, 1, 2, Qt::AlignVCenter);
	grid_->addWidget(l.value, grid_row, 3, Qt::AlignVCenter);
	grid_->addWidget(l.mute, grid_row, 4, Qt::AlignVCenter);

	// 動いた直後や現れた直後に、狙っていないつまみとボタンを押さない。
	new PressGuard(l.slider);
	new PressGuard(l.mute);

	ui::VolumeSlider *slider = l.slider;
	QLabel *value = l.value;
	QObject::connect(slider, &QSlider::valueChanged, this, [value](int v) { value->setText(value_text(v)); });
	slider->set_send([this, kind](int pct) {
		if (!on_action || departed_)
			return;
		ledger::ActionArgs args;
		args.user_id = user_id_;
		args.volume_pct = pct;
		on_action(kind == AudioKind::Voice ? ledger::ActionId::SetVoiceVolume : ledger::ActionId::SetScreenAudioVolume,
			  args);
	});
	QObject::connect(l.mute, &QAbstractButton::clicked, this, [this, kind] {
		if (!on_action || departed_ || !has_view_)
			return;
		const bool voice_line = kind == AudioKind::Voice;
		ledger::ActionArgs args;
		args.user_id = user_id_;
		args.on = !(voice_line ? view_.voice.muted : view_.screen.muted);
		on_action(voice_line ? ledger::ActionId::SetVoiceMute : ledger::ActionId::SetScreenAudioMute, args);
	});
	l.value->setText(value_text(l.slider->value()));
}

void AudioRowWidget::set_env(const AudioEnv &env)
{
	env_ = env;
	relayout();
	if (has_view_)
		apply_texts();
	update();
}

void AudioRowWidget::relayout()
{
	// 測る間に起きる FontChange と StyleChange (polish が送る) では、測り直しを積まない (この中で測る)。
	const bool was_in_relayout = in_relayout_;
	in_relayout_ = true;
	// 先に行と中のウィジェットをすべて polish し、テーマのスタイルシートのフォントと余白を当ててから測る。作った直後の行は
	// まだ polish されておらず、フォントは QApplication の既定のフォント (日本語の Windows では Yu Gothic UI の 9 pt) の
	// ままである。行はドックを表示した後に作られることが多く (通話に参加している人の行)、その後に測り直す機会がないので、
	// polish の前に測ると、テーマのフォント (Yami なら Open Sans の 10 pt) で描くパーセントと見出しが欠ける (100% が
	// 「00%」に見えた)。
	ensurePolished();
	const ui::DockMetrics &m = env_.row.metrics;
	const QFontMetrics fm = fontMetrics();
	const int small = m.small_px;
	const int large = m.large_px;
	const int ring = m.focus_ring_px + m.focus_ring_gap_px;
	const int slider_h = std::max(fm.height(), 12);
	const int bar_h = std::max(3, static_cast<int>(std::lround(m.em_px * kMeterBarEm)));
	const int controls_h = slider_h + ring + bar_h;
	const int line_h = std::max(env_.mute_side, controls_h);
	const int name_min = static_cast<int>(std::lround(m.em_px * kNameEm));
	const int slider_min = static_cast<int>(std::lround(m.em_px * kSliderMinEm));
	// 見出しの左の空き (列の間のすき間との差)。幅は、見出しのラベル自身のフォントと余白で、2 つの見出しの長い方に合わせる。
	for (Line *l : {&voice_, &screen_})
		l->kind->setContentsMargins(large - small, 0, 0, 0);
	const int kind_w = std::max(label_text_px(voice_.kind, ui::resolve_key(keys::kDockAudioVoiceKindLabel)),
				    label_text_px(screen_.kind, ui::resolve_key(keys::kDockAudioScreenKindLabel)));
	// パーセントの幅も、ラベル自身のフォントと余白で測る。上限の 200% が欠けない幅に 2 つの行でそろえる。
	const int value_w =
		std::max(label_text_px(voice_.value, value_text(200)), label_text_px(screen_.value, value_text(200)));

	// 先頭の人の上と、次の人との間に、フォーカスリングの太さとすき間を空ける。行の下の細い線は、その下の 1 px に描く。
	grid_->setContentsMargins(0, ring, 0, ring + 1);
	grid_->setHorizontalSpacing(small);
	grid_->setVerticalSpacing(ring);

	// 名前のラベルの横のサイズポリシーは、既定の Ignored のままにしない。Ignored のウィジェットはレイアウトの推奨の幅が 0 になり、
	// 最小の幅を持っていても列の幅が最小のまま広がらない。Preferred にして、推奨の幅と最大の幅を、行の幅から計算した名前の欄の幅
	// (apply_name_width) にする。最小の幅は name_min のままで、ドックの最小幅は変わらない。
	name_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
	name_->setMinimumWidth(name_min);
	name_->setFixedHeight(line_h);
	name_min_ = name_min;
	name_cap_ = static_cast<int>(std::lround(m.em_px * kNameMaxEm));
	slider_min_ = slider_min;
	slider_comfort_ = static_cast<int>(std::lround(m.em_px * kSliderComfortEm));
	// 名前の欄とつまみの場所以外の幅: 見出し、パーセント、ミュートのボタンと、5 列の間のすき間 4 つ。
	others_w_ = kind_w + value_w + env_.mute_side + 4 * small;
	for (Line *l : {&voice_, &screen_}) {
		l->kind->setFixedSize(kind_w, line_h);
		l->controls->setFixedHeight(line_h);
		l->controls->setMinimumWidth(slider_min);
		l->note->setFixedHeight(line_h);
		if (auto *lay = qobject_cast<QVBoxLayout *>(l->controls->layout())) {
			lay->setSpacing(ring);
			lay->setContentsMargins(0, (line_h - controls_h) / 2, 0, line_h - controls_h - (line_h - controls_h) / 2);
		}
		l->slider->setFixedHeight(slider_h);
		l->slider->setMinimumWidth(slider_min);
		l->meter->set_bar_height(bar_h);
		l->meter->set_colors(env_.meter);
		l->value->setFixedSize(value_w, line_h);
		l->mute->set_env(env_.row, env_.mute_side);
	}
	apply_name_width();
	updateGeometry();
	in_relayout_ = was_in_relayout;
}

void AudioRowWidget::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);
	if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
		request_relayout();
}

bool AudioRowWidget::eventFilter(QObject *watched, QEvent *event)
{
	// 測るラベル (見出しとパーセント) だけのフォントやスタイルが変わったときも測り直す (行のフォントは変わらないことがある)。
	if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
		request_relayout();
	return QWidget::eventFilter(watched, event);
}

void AudioRowWidget::request_relayout()
{
	if (in_relayout_ || relayout_queued_)
		return;
	// テーマを変えると、行と中のラベルに別々に FontChange と StyleChange が届き、届く順は決まっていない。全部が届いてから
	// 1 回だけ測り直すため、イベントループに積む。測り直した後、名前の欄の幅も全行でそろえ直す (AudioTab の on_remeasured)。
	relayout_queued_ = true;
	QTimer::singleShot(0, this, [this]() {
		relayout_queued_ = false;
		relayout();
		if (on_remeasured)
			on_remeasured();
	});
}

int AudioRowWidget::wanted_name_px() const
{
	const QMargins margins = name_->contentsMargins();
	return name_->fontMetrics().horizontalAdvance(name_->full_text()) + margins.left() + margins.right() + 2 * name_->margin();
}

void AudioRowWidget::set_wanted_name_px(int px)
{
	if (wanted_px_ == px)
		return;
	wanted_px_ = px;
	apply_name_width();
}

void AudioRowWidget::apply_name_width()
{
	const int available = width() - others_w_;
	const int w = ui::calc::name_column_px(available, wanted_px_, name_min_, name_cap_, slider_min_, slider_comfort_);
	name_->set_preferred_width(w);
	if (name_->maximumWidth() != w)
		name_->setMaximumWidth(w);
}

void AudioRowWidget::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	apply_name_width();
}

ui::VolumeSlider *AudioRowWidget::slider(AudioKind kind) const
{
	return line(kind).slider;
}
MuteButton *AudioRowWidget::mute(AudioKind kind) const
{
	return line(kind).mute;
}
ui::LevelMeter *AudioRowWidget::meter(AudioKind kind) const
{
	return line(kind).meter;
}
QLabel *AudioRowWidget::value_label(AudioKind kind) const
{
	return line(kind).value;
}
QLabel *AudioRowWidget::kind_label(AudioKind kind) const
{
	return line(kind).kind;
}
ui::ElideLabel *AudioRowWidget::note_label(AudioKind kind) const
{
	return line(kind).note;
}

bool AudioRowWidget::showing_note(AudioKind kind) const
{
	return line(kind).note_on;
}

void AudioRowWidget::apply(const ledger::dockview::AudioRowView &view)
{
	if (!has_view_) {
		// 最初の値は送らない。
		voice_.slider->set_initial(view.voice.volume_pct);
		screen_.slider->set_initial(view.screen.volume_pct);
		voice_.value->setText(value_text(voice_.slider->value()));
		screen_.value->setText(value_text(screen_.slider->value()));
	}
	view_ = view;
	has_view_ = true;
	const QString name = ui::resolve_label(view.name);
	if (name_->full_text() != name)
		name_->set_full_text(name);
	apply_line(AudioKind::Voice, view.voice);
	apply_line(AudioKind::Screen, view.screen);
	const bool enabled = !view.off && !departed_;
	if (!enabled)
		move_focus_off(this);
	setEnabled(enabled);
	apply_texts();
}

void AudioRowWidget::apply_line(AudioKind kind, const ledger::dockview::AudioLine &spec)
{
	Line &l = line(kind);
	if (!spec.control) {
		const QString note = ui::resolve(spec.note);
		if (l.note->full_text() != note)
			l.note->set_full_text(note);
	}
	// つまみに入っていたフォーカスを、隠す前に一覧に移す (Qt は次のウィジェットに移し、続けて押したキーが別の人に働く)。
	if (!spec.control)
		move_focus_off(l.controls);
	l.note_on = !spec.control;
	l.controls->setVisible(spec.control);
	l.note->setVisible(!spec.control);
	l.value->setVisible(spec.control);
	if (l.slider->sync(spec.volume_pct))
		l.value->setText(value_text(l.slider->value()));
	if (!spec.mute_shown)
		move_focus_off(l.mute);
	l.mute->setVisible(spec.mute_shown);
	l.mute->set_muted(spec.muted);
	l.mute->setEnabled(spec.mute_enabled && !departed_);
}

void AudioRowWidget::apply_texts()
{
	const QString name = ui::resolve_label(view_.name);
	for (const AudioKind kind : {AudioKind::Voice, AudioKind::Screen}) {
		Line &l = line(kind);
		const bool voice = kind == AudioKind::Voice;
		ledger::TextRef slider_name;
		slider_name.id = voice ? keys::kDockRowVoiceVolumeA11y : keys::kDockRowScreenVolumeA11y;
		slider_name.args.push_back(name.toStdString());
		l.slider->setAccessibleName(ui::resolve(slider_name));

		ledger::TextRef mute_name;
		mute_name.id = voice ? keys::kDockRowVoiceMuteA11y : keys::kDockRowScreenMuteA11y;
		mute_name.args.push_back(name.toStdString());
		l.mute->setAccessibleName(ui::resolve(mute_name));

		const ledger::dockview::AudioLine &spec = voice ? view_.voice : view_.screen;
		QString action;
		QString note;
		if (voice) {
			action = ui::resolve_key(spec.muted ? keys::kDockRowVoiceMuteOffA11y : keys::kDockRowVoiceMuteOnA11y);
			if (view_.is_main)
				note = ui::resolve_key(keys::kDockRowMainVoiceMuteTip);
		} else {
			action = ui::resolve_key(spec.muted ? keys::kDockRowScreenMuteOffA11y : keys::kDockRowScreenMuteOnA11y);
			if (spec.muted)
				note = ui::resolve_key(keys::kDockRowScreenMuteStartTip);
		}
		const QString tip = voice && !spec.control ? ui::resolve_key(keys::kDockRowNoVoiceTip) : join_lines({action, note});
		if (l.mute->toolTip() != tip)
			l.mute->setToolTip(tip);
		if (l.mute->accessibleDescription() != note)
			l.mute->setAccessibleDescription(note);
	}
}

void AudioRowWidget::set_departed(bool departed)
{
	if (departed_ == departed)
		return;
	departed_ = departed;
	if (departed)
		move_focus_off(this);
	setEnabled(!departed_ && !(has_view_ && view_.off));
	if (has_view_) {
		voice_.mute->setEnabled(view_.voice.mute_enabled && !departed_);
		screen_.mute->setEnabled(view_.screen.mute_enabled && !departed_);
	}
}

void AudioRowWidget::move_focus_off(QWidget *inside)
{
	// ウィンドウがアクティブでないときも、ウィンドウの中でフォーカスを持つウィジェット (アクティブに戻ったときの戻り先) を見る。
	for (QWidget *focus : {QApplication::focusWidget(), window() != nullptr ? window()->focusWidget() : nullptr}) {
		if (focus != nullptr && (focus == inside || inside->isAncestorOf(focus))) {
			if (QWidget *list = parentWidget())
				list->setFocus(Qt::OtherFocusReason);
			return;
		}
	}
}

void AudioRowWidget::set_level(AudioKind kind, const pipeline::AudioTrackLevel *track)
{
	Line &l = line(kind);
	if (track == nullptr || l.note_on || !isEnabled()) {
		l.meter->set_reading(0.0, false);
		return;
	}
	const ledger::MeterReading reading = ledger::meter_reading(track->peak_before, track->peak_after, track->silenced, l.slider->value());
	l.meter->set_reading(static_cast<double>(reading.position), reading.gray);
}

void AudioRowWidget::arm_guards()
{
	for (Line *l : {&voice_, &screen_}) {
		PressGuard::arm_widget(l->slider);
		PressGuard::arm_widget(l->mute);
	}
}

void AudioRowWidget::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	if (env_.rule.isValid())
		p.fillRect(QRect(0, height() - 1, width(), 1), env_.rule);
}

// ---- AudioTab ----

AudioTab::AudioTab(QWidget *parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("cordmuxAudioTab"));
	outer_ = new QVBoxLayout(this);
	outer_->setContentsMargins(0, 0, 0, 0);
	outer_->setSpacing(0);

	// 一覧そのもの。行が消えるときにフォーカスを移す先になる。Tab では止まらず、行の外の空いた所を押したときと、
	// フォーカスの移し先としてだけフォーカスを受ける。
	list_ = new QWidget(this);
	list_->setObjectName(QStringLiteral("cordmuxAudioList"));
	list_->setFocusPolicy(Qt::ClickFocus);
	list_->setAccessibleName(ui::resolve_key(keys::kDockTabAudioLabel));
	list_layout_ = new QVBoxLayout(list_);
	list_layout_->setContentsMargins(0, 0, 0, 0);
	list_layout_->setSpacing(0);
	list_->hide();
	outer_->addWidget(list_);

	// 行がないときの文。ドックのカードのテキストと同じ折り返し方にする。
	notice_ = new QWidget(this);
	notice_->setObjectName(QStringLiteral("cordmuxAudioNotice"));
	auto *notice_layout = new QVBoxLayout(notice_);
	notice_layout->setContentsMargins(0, 0, 0, 0);
	notice_text_ = new ui::WrapLabel(QString(), notice_);
	notice_text_->set_breaks(ui::WrapBreaks::KeepNumbers);
	notice_text_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	notice_text_->setObjectName(QStringLiteral("cordmuxAudioNoticeText"));
	notice_layout->addWidget(notice_text_);
	notice_->hide();
	outer_->addWidget(notice_);
}

AudioEnv AudioTab::make_env()
{
	AudioEnv e;
	e.row.metrics = look_.metrics;
	e.row.theme = look_.theme;
	e.row.style = look_.style;
	const QColor ground = look_.theme.base_bg.isValid() ? look_.theme.base_bg : palette().color(QPalette::Window);
	e.row.muted_line2 = ui::muted_ok(look_.theme, ground);
	e.row.enabled_text =
		look_.theme.text.isValid() ? look_.theme.text : palette().color(QPalette::Active, QPalette::WindowText);
	e.meter = ui::meter_colors(look_.theme, ground);
	{
		// ミュートのボタンの一辺は、参加者タブの行と同じく [...] の高さにそろえる。
		DotsButton probe(this);
		probe.hide();
		probe.set_env(e.row);
		probe.ensurePolished();
		e.mute_side = mute_button_side(probe.sizeHint(), look_.metrics.icon_px);
	}
	e.rule = look_.theme.text;
	e.rule.setAlpha(40);
	return e;
}

void AudioTab::ensure_env()
{
	if (env_ready_)
		return;
	if (!has_look_) {
		look_.theme = ui::read_theme(this, nullptr);
		look_.metrics = ui::read_dock_metrics(this, look_.theme, look_.style);
	}
	env_ = make_env();
	env_ready_ = true;
}

void AudioTab::set_look(const DockLook &look)
{
	look_ = look;
	has_look_ = true;
	env_ready_ = false;
	min_row_width_ = -1;
	ensure_env();
	for (auto &[id, row] : rows_)
		row->set_env(env_);
	fit_name_column();
	if (notice_text_ != nullptr)
		notice_text_->set_last_line_rule(look_.style);
}

int AudioTab::minimum_row_width_px() const
{
	if (min_row_width_ >= 0)
		return min_row_width_;
	auto *self = const_cast<AudioTab *>(this);
	self->ensure_env();
	// 行と同じ形のウィジェットで測る (人がいなくても同じ値になる)。
	AudioRowWidget probe(std::string(), env_, self);
	probe.hide();
	probe.ensurePolished();
	min_row_width_ = probe.minimumSizeHint().width();
	return min_row_width_;
}

void AudioTab::update(const ledger::dockview::AudioTabView &view)
{
	if (has_latest_ && view == latest_)
		return;
	latest_ = view;
	has_latest_ = true;
	if (held_)
		apply_in_place();
	else
		apply_structure();
}

bool AudioTab::structure_differs() const
{
	if (!has_latest_)
		return false;
	if (latest_.rows.size() != shown_order_.size())
		return true;
	for (std::size_t i = 0; i < latest_.rows.size(); ++i) {
		if (latest_.rows[i].user_id != shown_order_[i])
			return true;
	}
	return latest_.notice.has_value() != notice_visible_;
}

bool AudioTab::has_pending_layout() const
{
	return structure_differs();
}

void AudioTab::set_layout_hold(bool held)
{
	if (held_ == held)
		return;
	held_ = held;
	if (held_)
		return;
	// 解けたら、保留の間にたまった行の追加、削除、並べ替えと、名前の欄の幅の変更をまとめて反映する。
	if (structure_differs())
		apply_structure();
	else
		fit_name_column();
}

AudioRowWidget *AudioTab::row_for(const std::string &user_id) const
{
	const auto it = rows_.find(user_id);
	return it == rows_.end() ? nullptr : it->second;
}

bool AudioTab::notice_shown() const
{
	return notice_visible_;
}

QString AudioTab::notice_text() const
{
	return notice_text_->source_text();
}

AudioRowWidget *AudioTab::make_row(const std::string &user_id)
{
	auto *row = new AudioRowWidget(user_id, env_, list_);
	row->on_action = [this](ledger::ActionId id, const ledger::ActionArgs &args) {
		if (on_action_)
			on_action_(id, args);
	};
	// 行が後から測り直したとき (フォントかスタイルが変わった) は、名前の欄の幅を全行でそろえ直す。並びの保留の間は変えず、
	// 解けたとき (set_layout_hold) にそろえる。
	row->on_remeasured = [this]() {
		if (!held_)
			fit_name_column();
	};
	rows_[user_id] = row;
	return row;
}

void AudioTab::remove_row(const std::string &user_id)
{
	const auto it = rows_.find(user_id);
	if (it == rows_.end())
		return;
	AudioRowWidget *row = it->second;
	// フォーカスのある行を消す前に、フォーカスを一覧そのものに移す。
	row->set_departed(true);
	row->hide();
	list_layout_->removeWidget(row);
	row->deleteLater();
	rows_.erase(it);
}

void AudioTab::apply_notice()
{
	notice_visible_ = latest_.notice.has_value();
	if (notice_visible_) {
		const QString text = ui::resolve(*latest_.notice);
		if (notice_text_->source_text() != text)
			notice_text_->set_text(text);
	}
	notice_->setVisible(notice_visible_);
}

void AudioTab::apply_structure()
{
	ensure_env();
	std::set<std::string> ids;
	for (const ledger::dockview::AudioRowView &v : latest_.rows)
		ids.insert(v.user_id);
	std::map<std::string, std::size_t> old_index;
	for (std::size_t i = 0; i < shown_order_.size(); ++i)
		old_index[shown_order_[i]] = i;
	const std::vector<std::string> before = shown_order_;
	for (const std::string &id : before) {
		if (ids.count(id) == 0)
			remove_row(id);
	}
	shown_order_.clear();
	for (std::size_t i = 0; i < latest_.rows.size(); ++i) {
		const ledger::dockview::AudioRowView &v = latest_.rows[i];
		AudioRowWidget *row = row_for(v.user_id);
		const bool created = row == nullptr;
		if (created)
			row = make_row(v.user_id);
		row->set_departed(false);
		row->apply(v);
		// 行を作り直さずに並べ直す。
		if (list_layout_->indexOf(row) != static_cast<int>(i)) {
			list_layout_->removeWidget(row);
			list_layout_->insertWidget(static_cast<int>(i), row);
		}
		row->show();
		const auto old = old_index.find(v.user_id);
		if (!created && old != old_index.end() && old->second != i)
			row->arm_guards();
		shown_order_.push_back(v.user_id);
	}
	list_->setVisible(!shown_order_.empty());
	apply_notice();
	fit_name_column();
	fix_tab_order();
}

void AudioTab::apply_in_place()
{
	ensure_env();
	std::set<std::string> ids;
	for (const ledger::dockview::AudioRowView &v : latest_.rows) {
		ids.insert(v.user_id);
		AudioRowWidget *row = row_for(v.user_id);
		if (row == nullptr)
			continue; // 新しく来た人の行は、並びの保留が解けてから追加する
		row->set_departed(false);
		row->apply(v);
	}
	for (const std::string &id : shown_order_) {
		if (ids.count(id) != 0)
			continue;
		if (AudioRowWidget *row = row_for(id))
			row->set_departed(true);
	}
	// 名前の欄の幅 (全行のつまみの位置) は、並びの保留の間は変えない。名前が変わっても、保留が解けるまで古い幅のままにする。
	// 文を表示している間の文の入れ替えは、場所を動かさないので後に回さない。
	if (latest_.notice.has_value() && notice_visible_) {
		const QString text = ui::resolve(*latest_.notice);
		if (notice_text_->source_text() != text)
			notice_text_->set_text(text);
	}
}

void AudioTab::fit_name_column()
{
	int wanted = 0;
	for (const auto &[id, row] : rows_)
		wanted = std::max(wanted, row->wanted_name_px());
	for (const auto &[id, row] : rows_)
		row->set_wanted_name_px(wanted);
}

void AudioTab::fix_tab_order()
{
	// Tab の順を「声のつまみ -> 声のミュート -> 画面共有のつまみ -> 画面共有のミュート -> 次の人」に保つ。
	QWidget *prev = list_;
	for (const std::string &id : shown_order_) {
		AudioRowWidget *row = row_for(id);
		if (row == nullptr)
			continue;
		for (const AudioKind kind : {AudioKind::Voice, AudioKind::Screen}) {
			QWidget::setTabOrder(prev, row->slider(kind));
			QWidget::setTabOrder(row->slider(kind), row->mute(kind));
			prev = row->mute(kind);
		}
	}
}

void AudioTab::set_levels(const std::vector<pipeline::AudioTrackLevel> &tracks)
{
	std::unordered_map<std::string, const pipeline::AudioTrackLevel *> voice;
	std::unordered_map<std::string, const pipeline::AudioTrackLevel *> screen;
	for (const pipeline::AudioTrackLevel &t : tracks)
		(t.key.kind == pipeline::MixerKind::Voice ? voice : screen)[t.key.user_id] = &t;
	for (auto &[id, row] : rows_) {
		const auto v = voice.find(id);
		const auto s = screen.find(id);
		row->set_level(AudioKind::Voice, v == voice.end() ? nullptr : v->second);
		row->set_level(AudioKind::Screen, s == screen.end() ? nullptr : s->second);
	}
}

void AudioTab::clear_levels()
{
	for (auto &[id, row] : rows_) {
		row->set_level(AudioKind::Voice, nullptr);
		row->set_level(AudioKind::Screen, nullptr);
	}
}

} // namespace cordmux::dock
