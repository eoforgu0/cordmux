// SPDX-License-Identifier: Apache-2.0

// ドックの参加者の一覧 (people_list.h) を実装する。

#include "dock/people_list.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QCheckBox>
#include <QCursor>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPalette>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <set>

#include "dock/name_check.h"
#include "dock/notice_list.h"
#include "dock/ui/elide_label.h"
#include "dock/ui/set_class.h"
#include "dock/ui/text_resolve.h"
#include "dock/ui/wrap_label.h"
#include "ledger/text_ids.h"

namespace cordmux::dock {

namespace {

namespace keys = ledger::text::keys;

constexpr char16_t kEllipsis = 0x2026;

// 一覧の行どうしの間 (小さい余白と、フォーカスリング + 背景の 1 px の大きい方。name_check.h の row_gap_px)。
int list_row_gap_px(const ui::DockMetrics &m)
{
	return rows::row_gap_px(m.small_px, m.focus_ring_px, m.focus_ring_gap_px);
}

std::u16string to_u16(const QString &s)
{
	return std::u16string(reinterpret_cast<const char16_t *>(s.utf16()), static_cast<std::size_t>(s.size()));
}

// 押せないラベルの不透明度。押せるときのテキストの色 a を背景 b の上に不透明度 alpha で描いた色が、押せないときの
// テキストの色 d に最も近くなる alpha を返す (3 つの色の成分の最小二乗)。テーマが 2 つの色を分けていなければ 1 (何もしない)。
double fit_alpha(const QColor &a, const QColor &d, const QColor &b)
{
	if (!a.isValid() || !d.isValid() || !b.isValid())
		return 1.0;
	const double ar[3] = {a.redF(), a.greenF(), a.blueF()};
	const double dr[3] = {d.redF(), d.greenF(), d.blueF()};
	const double br[3] = {b.redF(), b.greenF(), b.blueF()};
	double num = 0.0;
	double den = 0.0;
	for (int i = 0; i < 3; ++i) {
		num += (dr[i] - br[i]) * (ar[i] - br[i]);
		den += (ar[i] - br[i]) * (ar[i] - br[i]);
	}
	if (den < 1e-6)
		return 1.0;
	return std::clamp(num / den, 0.2, 1.0);
}

} // namespace

PeopleHeader::PeopleHeader(QWidget *parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("cordmuxPeopleHeader"));
	auto *layout = new QHBoxLayout(this);
	slots_ = new QLabel(this);
	slots_->setObjectName(QStringLiteral("cordmuxSlotsUsed"));
	slots_->setTextFormat(Qt::PlainText);
	layout->addWidget(slots_);
	layout->addStretch(1);
}

void PeopleHeader::set_look(const DockLook &look)
{
	const int edge = look.metrics.edge_text_px;
	const int small = look.metrics.small_px;
	if (QLayout *l = layout())
		l->setContentsMargins(edge, small, edge, small);
	// 行の高さは、チェックボックスを置いたときと同じに保つ (行の下の一覧の位置を動かさない)。
	QCheckBox probe(QStringLiteral("M"), this);
	probe.setFont(slots_->font());
	probe.ensurePolished();
	slots_->setMinimumHeight(probe.sizeHint().height());
}

void PeopleHeader::set_slots(const QString &text)
{
	if (slots_->text() != text)
		slots_->setText(text);
}

PeopleList::PeopleList(QWidget *parent) : QWidget(parent)
{
	outer_ = new QVBoxLayout(this);
	outer_->setContentsMargins(0, 0, 0, 0);
	outer_->setSpacing(metrics_.small_px);

	// 一覧そのもの。行が消えるときにフォーカスを移す先になる。Tab では止まらず (行の順を「チェック -> [...] -> 次の行の
	// チェック」に保つため)、行の外の空いた所を押したときと、フォーカスの移し先としてだけフォーカスを受ける。
	list_ = new QWidget(this);
	list_->setObjectName(QStringLiteral("cordmuxPeopleList"));
	list_->setFocusPolicy(Qt::ClickFocus);
	list_->setAccessibleName(ui::resolve_key(keys::kDockPeopleListA11y));
	list_layout_ = new QVBoxLayout(list_);
	list_layout_->setContentsMargins(0, 0, 0, 0);
	list_layout_->setSpacing(list_row_gap_px(metrics_));
	list_->hide();
	outer_->addWidget(list_);

	// 2 組の見出し。行がない間は隠す。
	sharing_heading_ = new QLabel(list_);
	sharing_heading_->setObjectName(QStringLiteral("cordmuxGroupSharing"));
	in_call_heading_ = new QLabel(list_);
	in_call_heading_->setObjectName(QStringLiteral("cordmuxGroupInCall"));
	for (QLabel *heading : {sharing_heading_, in_call_heading_}) {
		heading->setTextFormat(Qt::PlainText);
		ui::set_class(heading, QStringLiteral("text-muted"));
		heading->hide();
	}

	// 最初の設定のメッセージ。一覧に行がない間だけ、一覧の場所にも置く。
	setup_ = new NoticeList(this);
	setup_->hide();
	outer_->addWidget(setup_);

	// 空の一覧。理由か条件の 1 文と、次にできることがあればボタンを置く (ボタンはテキストの幅のまま左寄せ)。
	empty_ = new QWidget(this);
	empty_->setObjectName(QStringLiteral("cordmuxEmptyState"));
	auto *ev = new QVBoxLayout(empty_);
	ev->setContentsMargins(0, 0, 0, 0);
	ev->setSpacing(metrics_.large_px);
	// 文はドックのカードのテキストと同じ折り返し方 (ui::WrapLabel の KeepNumbers) にする。数と単位の語を行で分けず、
	// 最後の行が 2 字以下なら、行の数を増やさない範囲で行の長さをならす。同じスクロール領域の中のカードと形をそろえる
	// ためである。最後の行の値は、カード (notice_list.cpp の card_label) と同じ既定のままにする。横の sizePolicy は
	// Ignored にし、テキストの全文の幅を最小幅にしない。Qt の折り返しのままだと、日本語で「...表示されま」「す。」の
	// ように最後の行が短くなる。
	empty_text_ = new ui::WrapLabel(QString(), empty_);
	empty_text_->set_breaks(ui::WrapBreaks::KeepNumbers);
	empty_text_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	empty_text_->setObjectName(QStringLiteral("cordmuxEmptyText"));
	ev->addWidget(empty_text_);
	empty_button_ = new QPushButton(empty_);
	empty_button_->setObjectName(QStringLiteral("cordmuxEmptyButton"));
	empty_button_->hide();
	ev->addWidget(empty_button_, 0, Qt::AlignLeft);
	empty_->hide();
	outer_->addWidget(empty_);
	QObject::connect(empty_button_, &QPushButton::clicked, this, [this] {
		if (empty_action_ && on_action_)
			on_action_(empty_action_->id, empty_action_->args);
	});
}

PeopleList::~PeopleList()
{
	for (const QPointer<QWidget> &w : tracked_ancestors_) {
		if (w)
			w->removeEventFilter(this);
	}
	for (const QMetaObject::Connection &c : scroll_connections_)
		QObject::disconnect(c);
}

// ---- 環境 (テーマと余白や幅の値) ----

RowEnv PeopleList::make_env()
{
	RowEnv e;
	e.metrics = metrics_;
	e.theme = theme_;
	e.style = style_;
	e.status_keep_em = status_keep_em_;
	const QColor ground = theme_.base_bg.isValid() ? theme_.base_bg : palette().color(QPalette::Window);
	e.muted_line2 = ui::muted_ok(theme_, ground);
	e.enabled_text = theme_.text.isValid() ? theme_.text : palette().color(QPalette::Active, QPalette::WindowText);
	{
		// 押せるテキストと押せないテキストの色を、クラスを付けないラベルで読む (テーマのパレット)。
		QLabel probe(this);
		probe.hide();
		probe.ensurePolished();
		const QColor active = probe.palette().color(QPalette::Active, QPalette::WindowText);
		const QColor disabled = probe.palette().color(QPalette::Disabled, QPalette::WindowText);
		e.disabled_alpha = fit_alpha(active, disabled, ground);
	}
	{
		// Vesktop のアカウントの行の名前を、チェックのある行の名前と同じ左の位置にそろえる。
		RowCheck probe(this);
		probe.hide();
		probe.ensurePolished();
		probe.set_line1_height(std::max(probe.fontMetrics().height(), metrics_.icon_px));
		e.text_indent_px = probe.text_left();
	}
	return e;
}

void PeopleList::ensure_env()
{
	if (env_ready_)
		return;
	if (!has_theme_)
		theme_ = ui::read_theme(this, nullptr);
	if (!has_metrics_)
		metrics_ = ui::read_dock_metrics(this, theme_, style_);
	outer_->setSpacing(metrics_.small_px);
	list_layout_->setSpacing(list_row_gap_px(metrics_));
	empty_->layout()->setSpacing(metrics_.large_px);
	// 見出しの上は、前の組の最後の行との間を、意味の違う部分の間 (section_gap_px。1 em) にして、組の区切りが分かるようにする
	// (行どうしの間を引いた残りを見出しの上に空ける)。見出しの文字色は補足の色で、読めないテーマでは本文の色にする。
	for (QLabel *heading : {sharing_heading_, in_call_heading_}) {
		heading->setContentsMargins(0, std::max(0, metrics_.section_gap_px - list_row_gap_px(metrics_)), 0, 0);
		// 組の見出しは、テーマによって背景が本体の背景 (base_bg) にもウィンドウの背景 (window_bg。Rachni の一覧の背景) にもなる。
		// どちらの上でも読めるときだけ補足の色にする。
		const bool muted = ui::muted_ok(theme_, theme_.base_bg) && ui::muted_ok(theme_, theme_.window_bg);
		ui::set_class(heading, muted ? QStringLiteral("text-muted") : QString());
	}
	update_empty_gap();
	env_ = make_env();
	env_ready_ = true;
	min_row_width_ = -1;
	for (auto &[id, row] : rows_)
		row->set_env(env_);
}

void PeopleList::set_metrics(const ui::DockMetrics &metrics)
{
	metrics_ = metrics;
	has_metrics_ = true;
	env_ready_ = false;
	ensure_env();
	schedule_refit();
}

void PeopleList::set_theme(const ui::ThemeRead &theme, const ui::DockStyle &style)
{
	theme_ = theme;
	style_ = style;
	has_theme_ = true;
	env_ready_ = false;
	ensure_env();
	schedule_refit();
}

void PeopleList::set_status_keep_em(double em)
{
	if (em <= 0.0 || status_keep_em_ == em)
		return;
	status_keep_em_ = em;
	env_ready_ = false;
	ensure_env();
	schedule_refit();
}

void PeopleList::set_available_width_em(double em)
{
	if (available_em_ == em)
		return;
	available_em_ = em;
	schedule_refit();
}

double PeopleList::width_em() const
{
	if (available_em_ > 0.0)
		return available_em_;
	return metrics_.em_px > 0.0 ? list_->width() / metrics_.em_px : 0.0;
}

int PeopleList::minimum_row_width_px() const
{
	if (min_row_width_ >= 0)
		return min_row_width_;
	auto *self = const_cast<PeopleList *>(this);
	self->ensure_env();
	// 行と同じ形のウィジェットで測る (人がいなくても同じ値になる)。
	RowCheck check(self);
	check.hide();
	check.set_env(env_);
	check.ensurePolished();
	DotsButton dots(self);
	dots.hide();
	dots.set_env(env_);
	dots.ensurePolished();
	check.set_avatar_shown(false);
	min_row_width_ = rows::min_row_width(
				 check.text_left(), metrics_.name_floor_px, metrics_.icon_px, dots.sizeHint().width(),
				 metrics_.small_px, metrics_.large_px,
				 rows::dots_gap_px(metrics_.large_px, metrics_.focus_ring_px, metrics_.focus_ring_gap_px)) +
			 rows::audio_buttons_px(mute_button_side(dots.sizeHint(), metrics_.icon_px), metrics_.small_px);
	return min_row_width_;
}

int PeopleList::status_text_min_width_px() const
{
	// 状態のテキストに残す幅は、行が名前とテキストの幅を分けるときと同じ値 (env の status_keep_em) にする。プレビュー
	// ツールが変える値も、この値を通る。
	const int keep = static_cast<int>(status_keep_em_ * metrics_.em_px + 0.5);
	return rows::status_text_min_width(minimum_row_width_px(), keep, metrics_.large_px);
}

// ---- 書き換え ----

void PeopleList::set_action_callback(ActionCallback cb)
{
	on_action_ = std::move(cb);
}

void PeopleList::set_rename_callback(std::function<void(const std::string &user_id)> cb)
{
	on_rename_ = std::move(cb);
}

void PeopleList::set_menu_open_callback(std::function<void(bool open)> cb)
{
	on_menu_open_ = std::move(cb);
}

void PeopleList::flatten_latest()
{
	const ledger::dockview::PeopleListView &shown = latest_.list;
	latest_.rows.clear();
	if (shown.pinned.has_value())
		latest_.rows.push_back(*shown.pinned);
	latest_.rows.insert(latest_.rows.end(), shown.sharing.begin(), shown.sharing.end());
	latest_.rows.insert(latest_.rows.end(), shown.in_call.begin(), shown.in_call.end());
}

const ledger::dockview::PersonRow *PeopleList::latest_row(const std::string &user_id) const
{
	for (const ledger::dockview::PersonRow &r : latest_.rows) {
		if (r.user_id == user_id)
			return &r;
	}
	return nullptr;
}

void PeopleList::update(const ledger::dockview::PeopleListView &list,
			const std::optional<ledger::dockview::ListReason> &reason,
			const std::optional<ledger::dockview::EmptyList> &empty)
{
	latest_.list = list;
	latest_.reason = reason;
	latest_.empty = empty;
	has_latest_ = true;
	flatten_latest();
	// 行のメニューが開いている間は、DockWidget が並びの保留を伝えるのを待たずに、このウィジェットが自分で、
	// 位置が動く変化を後に回す。メニューの入れ子のイベントループの中で、メニューを開いた行を削除しないためである。
	if (held_ || open_menu_)
		apply_in_place();
	else
		apply_structure();
	// 開いている行のメニューでは、その人が抜けたときと、使う声の人が抜けたときに、その項目を押せなくする。
	if (open_menu_)
		refresh_row_menu(open_menu_, latest_row(open_menu_user_));
}

void PeopleList::set_speaking(const std::vector<std::string> &user_ids)
{
	for (auto &[id, row] : rows_)
		row->set_speaking(std::find(user_ids.begin(), user_ids.end(), id) != user_ids.end());
}

void PeopleList::set_avatars(const std::map<std::string, AvatarBytes> &avatars)
{
	if (avatars == avatars_)
		return;
	// バイト列のポインターが変わった人だけデコードする。
	std::map<std::string, QPixmap> images;
	for (const auto &[id, bytes] : avatars) {
		const auto before = avatars_.find(id);
		if (before != avatars_.end() && before->second == bytes) {
			const auto kept = avatar_images_.find(id);
			if (kept != avatar_images_.end())
				images[id] = kept->second;
			continue;
		}
		QPixmap image;
		if (bytes && !bytes->empty() &&
		    image.loadFromData(bytes->data(), static_cast<uint>(bytes->size())))
			images[id] = image;
	}
	avatars_ = avatars;
	avatar_images_ = std::move(images);
	for (auto &[id, row] : rows_)
		row->set_avatar(avatar_for(id));
}

QPixmap PeopleList::avatar_for(const std::string &user_id)
{
	const auto found = avatar_images_.find(user_id);
	return found == avatar_images_.end() ? QPixmap() : found->second;
}

void PeopleList::set_layout_hold(bool held)
{
	if (held_ == held)
		return;
	held_ = held;
	if (!held_ && !open_menu_ && structure_differs())
		apply_structure();
}

bool PeopleList::has_pending_layout() const
{
	return structure_differs();
}

bool PeopleList::structure_differs() const
{
	if (!has_latest_)
		return false;
	if (latest_.rows.size() != shown_order_.size())
		return true;
	// 組の間の移動は、行の順が同じでも見出しの位置が変わる。
	const std::vector<QWidget *> wanted = wanted_widgets();
	if (static_cast<int>(wanted.size()) != list_layout_->count())
		return true;
	for (std::size_t i = 0; i < wanted.size(); ++i) {
		QLayoutItem *item = list_layout_->itemAt(static_cast<int>(i));
		if (wanted[i] == nullptr || item == nullptr || item->widget() != wanted[i])
			return true;
	}
	for (std::size_t i = 0; i < latest_.rows.size(); ++i) {
		const ledger::dockview::PersonRow &v = latest_.rows[i];
		if (v.user_id != shown_order_[i])
			return true;
		const auto it = rows_.find(v.user_id);
		if (it == rows_.end() || it->second->shape_differs(v))
			return true;
	}
	if (latest_.empty.has_value() != empty_shown_)
		return true;
	if (latest_.empty && empty_shown_ && latest_.empty->button.has_value() != empty_action_.has_value())
		return true;
	return false;
}

PersonRowWidget *PeopleList::make_row(const std::string &user_id)
{
	auto *row = new PersonRowWidget(user_id, env_, list_);
	row->set_avatar(avatar_for(user_id));
	row->on_check_clicked = [this, user_id] {
		PersonRowWidget *r = row_for(user_id);
		if (r == nullptr || r->departed() || !on_action_)
			return;
		// 枠が埋まっている間は、チェックを変えずに入れ替える相手を選ぶ。
		if (r->view().check_replaces) {
			open_replace_menu(user_id, r->check());
			return;
		}
		// 押した時点でその行が持つ人に対して操作する。押せるかは apply() がもう一度決める。
		const ledger::dockview::ViewButton &b = r->view().check_action;
		on_action_(b.id, b.args);
	};
	row->on_audio_action = [this](ledger::ActionId id, const ledger::ActionArgs &args) {
		// ミュートのボタン。押した時点でその行が持つ人に対して操作する。押せるかは apply() がもう一度決める。
		if (on_action_)
			on_action_(id, args);
	};
	row->on_menu_requested = [this, user_id](QWidget *anchor, const QPoint &pos, bool keyboard) {
		open_row_menu(user_id, anchor, pos, keyboard);
	};
	row->on_geometry_changed = [this] { schedule_refit(); };
	row->installEventFilter(this);
	rows_[user_id] = row;
	return row;
}

void PeopleList::move_focus_off(QWidget *row)
{
	if (row == nullptr)
		return;
	// ウィンドウがアクティブでないとき (OBS の外のアプリを使っている間) も、ウィンドウの中でフォーカスを持つウィジェット
	// (アクティブに戻ったときのフォーカスの戻り先) を見る。見ないと、戻ったときに、抜けた人の行にフォーカスが戻る。
	for (QWidget *focus : {QApplication::focusWidget(), window()->focusWidget()}) {
		if (focus != nullptr && (focus == row || row->isAncestorOf(focus))) {
			list_->setFocus(Qt::OtherFocusReason);
			return;
		}
	}
}

void PeopleList::remove_row(const std::string &user_id)
{
	const auto it = rows_.find(user_id);
	if (it == rows_.end())
		return;
	PersonRowWidget *row = it->second;
	// フォーカスのある行を消す前に、フォーカスを一覧そのものに移す。Qt は隠したウィジェットのフォーカスを次の
	// ウィジェットに移すので、そのままだと、続けて押した Space が別の人に対して働く。
	move_focus_off(row);
	row->removeEventFilter(this);
	row->hide();
	list_layout_->removeWidget(row);
	row->deleteLater();
	rows_.erase(it);
	positions_.erase(user_id);
}

std::vector<QWidget *> PeopleList::wanted_widgets() const
{
	// 並びは、メインの行、「画面共有中」の見出しと行、「通話中」の見出しと行。行がまだないときは nullptr を入れる。
	const ledger::dockview::PeopleListView &shown = latest_.list;
	std::vector<QWidget *> out;
	if (shown.pinned.has_value())
		out.push_back(row_for(shown.pinned->user_id));
	const bool any_people = !latest_.list.sharing.empty() || !latest_.list.in_call.empty();
	if (any_people)
		out.push_back(sharing_heading_);
	for (const ledger::dockview::PersonRow &row : shown.sharing)
		out.push_back(row_for(row.user_id));
	if (any_people)
		out.push_back(in_call_heading_);
	for (const ledger::dockview::PersonRow &row : shown.in_call)
		out.push_back(row_for(row.user_id));
	return out;
}

void PeopleList::apply_headings()
{
	const QString sharing = ui::resolve(latest_.list.sharing_heading);
	if (sharing_heading_->text() != sharing)
		sharing_heading_->setText(sharing);
	const QString in_call = ui::resolve(latest_.list.in_call_heading);
	if (in_call_heading_->text() != in_call)
		in_call_heading_->setText(in_call);
}

void PeopleList::apply_structure()
{
	ensure_env();
	std::set<std::string> ids;
	for (const ledger::dockview::PersonRow &v : latest_.rows)
		ids.insert(v.user_id);
	const std::vector<std::string> before = shown_order_;
	for (const std::string &id : before) {
		if (ids.count(id) == 0)
			remove_row(id);
	}
	shown_order_.clear();
	for (std::size_t i = 0; i < latest_.rows.size(); ++i) {
		const ledger::dockview::PersonRow &v = latest_.rows[i];
		PersonRowWidget *row = row_for(v.user_id);
		if (row == nullptr)
			row = make_row(v.user_id);
		row->set_departed(false);
		row->apply(v, true);
		row->show();
		shown_order_.push_back(v.user_id);
	}
	// 行を作り直さずに並べ直す。見出しも同じ並びに入れる。
	const std::vector<QWidget *> wanted = wanted_widgets();
	for (QLabel *heading : {sharing_heading_, in_call_heading_}) {
		if (std::find(wanted.begin(), wanted.end(), heading) == wanted.end()) {
			list_layout_->removeWidget(heading);
			heading->hide();
		}
	}
	for (std::size_t i = 0; i < wanted.size(); ++i) {
		QWidget *w = wanted[i];
		if (list_layout_->indexOf(w) != static_cast<int>(i)) {
			list_layout_->removeWidget(w);
			list_layout_->insertWidget(static_cast<int>(i), w);
		}
		w->show();
	}
	apply_headings();
	list_->setVisible(!shown_order_.empty());
	apply_empty(latest_.empty);
	update_empty_gap();
	fix_tab_order();
	update_list_accessibility();
	schedule_refit();
}

void PeopleList::apply_in_place()
{
	ensure_env();
	// 並びの保留の間は、行の中のテキスト、アイコン、チェックだけをその場で書き換える。
	std::set<std::string> ids;
	for (const ledger::dockview::PersonRow &v : latest_.rows) {
		ids.insert(v.user_id);
		PersonRowWidget *row = row_for(v.user_id);
		if (row == nullptr)
			continue; // 新しく来た人の行は、並びの保留が解けてから追加する
		row->set_departed(false);
		row->apply(v, false);
	}
	for (const std::string &id : shown_order_) {
		if (ids.count(id) != 0)
			continue;
		PersonRowWidget *row = row_for(id);
		if (row == nullptr || row->departed())
			continue;
		// 通話から抜けた人の行は残し、押せなくする。フォーカスは先に一覧に移す。
		move_focus_off(row);
		row->set_departed(true);
	}
	// 人数の見出しは、行を動かすときに一緒に変える。行が動く前に人数だけが変わると、人数と行が合わない。
	if (!structure_differs())
		apply_headings();
	if (latest_.empty.has_value() == empty_shown_ && latest_.empty && empty_shown_ &&
	    latest_.empty->button.has_value() == empty_action_.has_value())
		update_empty_content(*latest_.empty);
	update_list_accessibility();
	schedule_refit();
}

void PeopleList::update_empty_gap()
{
	// 空の一覧の文が行 (Vesktop のアカウントの行) の下に付くときは、文が一覧のもう 1 行に見えないように、一覧と文の間を
	// 意味の違う部分の間 (section_gap_px。1 em) にする。一覧と文の間は outer_ の spacing (小さい余白) なので、それを引いた
	// 分を文の上に足す (行どうしの間 list_row_gap_px とは別の間)。一覧を表示していなければ空けない (文が一覧の場所の
	// 先頭になる)。
	const int top = list_->isHidden() ? 0 : std::max(0, metrics_.section_gap_px - metrics_.small_px);
	QLayout *layout = empty_->layout();
	if (layout->contentsMargins().top() != top)
		layout->setContentsMargins(0, top, 0, 0);
}

void PeopleList::update_empty_content(const ledger::dockview::EmptyList &empty)
{
	// 比べるのは改行を入れる前のテキスト (WrapLabel の text() は、折り返すと行の区切りを含む)。
	const QString text = ui::resolve(empty.text);
	if (empty_text_->source_text() != text)
		empty_text_->set_text(text);
	empty_action_ = empty.button;
	if (empty.button) {
		empty_button_->setText(ui::resolve(empty.button->label));
		empty_button_->setEnabled(!empty.button->disabled_reason.has_value());
		empty_button_->setToolTip(empty.button->disabled_reason ? ui::resolve(*empty.button->disabled_reason)
									: QString());
		empty_button_->show();
	} else {
		empty_button_->hide();
	}
}

void PeopleList::apply_empty(const std::optional<ledger::dockview::EmptyList> &empty)
{
	if (!empty) {
		empty_->hide();
		empty_shown_ = false;
		empty_action_.reset();
		return;
	}
	update_empty_content(*empty);
	empty_->show();
	empty_shown_ = true;
}

void PeopleList::fix_tab_order()
{
	// Tab の順を「チェック -> ミュートのボタン -> [...] -> 次の行のチェック」に保つ。Qt のフォーカスの順はウィジェットを作った順なので、
	// 後から来た人の行や並べ替えた行のために、一覧そのもの (作ったときから順の中の位置が決まっている) から順に
	// つなぎ直す。
	QWidget *prev = list_;
	for (const std::string &id : shown_order_) {
		PersonRowWidget *row = row_for(id);
		if (row == nullptr)
			continue;
		if (row->check() != nullptr) {
			QWidget::setTabOrder(prev, row->check());
			prev = row->check();
		}
		// 順は「チェック -> 声のミュート -> 画面共有の音のミュート -> [...]」。隠れているボタンは飛ばす。
		for (QWidget *mute : {static_cast<QWidget *>(row->voice_mute()), static_cast<QWidget *>(row->screen_mute())}) {
			if (!mute->isHidden()) {
				QWidget::setTabOrder(prev, mute);
				prev = mute;
			}
		}
		if (!row->dots()->isHidden()) {
			QWidget::setTabOrder(prev, row->dots());
			prev = row->dots();
		}
	}
}

void PeopleList::update_list_accessibility()
{
	// アクセシブル名は「参加者」にし、枠がすべて使用中の間は accessibleDescription に理由を入れる。説明は位置を
	// 動かさないので後に回さない。
	const QString name = ui::resolve_key(keys::kDockPeopleListA11y);
	if (list_->accessibleName() != name)
		list_->setAccessibleName(name);
	const QString desc = latest_.reason ? ui::resolve(latest_.reason->text) : QString();
	if (list_->accessibleDescription() != desc)
		list_->setAccessibleDescription(desc);
}

// ---- 幅による判定と名前の見分け ----

void PeopleList::schedule_refit()
{
	if (refit_scheduled_ || refitting_)
		return;
	refit_scheduled_ = true;
	QTimer::singleShot(0, this, [this] {
		refit_scheduled_ = false;
		refit();
	});
}

void PeopleList::refit_now()
{
	refit();
}

void PeopleList::refit()
{
	if (refitting_)
		return;
	refitting_ = true;
	if (!rows_.empty()) {
		// 行が使える幅で決め、全部の行に同じ結果を適用する。解像度は 1 行に収まるときだけ表示し、1 つの行でも収まら
		// なければ、全部の行で表示しない。収まるかは、ウィジェットを表示したり隠したりせずに sizeHint の幅で決める
		// (表示と非表示で幅が変わり、判定が行き来しないように。結果が同じならウィジェットは何も変わらない)。
		// 状態のテキストの境目は中身から計算した px、解像度の境目は kResolutionMinEm。行の幅は DockWidget が em で渡す
		// (行が使える幅 / 1 em) ので、px に戻す (丸めの差は 1 px 未満)。
		const int row_px = static_cast<int>(width_em() * metrics_.em_px + 0.5);
		const int res_px = static_cast<int>(rows::kResolutionMinEm * metrics_.em_px + 0.5);
		const rows::WidthRules rules = rows::width_rules(
			row_px, status_text_min_width_px(), res_px,
			rows::avatar_min_width(status_text_min_width_px(), rows::kRowAvatarPx, metrics_.small_px, metrics_.name_floor_px));
		bool allow = rules.resolution;
		for (auto &[id, row] : rows_)
			allow = allow && row->resolution_would_fit(rules);
		for (auto &[id, row] : rows_)
			row->set_width_rules(rules, allow);
		for (auto &[id, row] : rows_)
			row->activate_layouts();
		list_layout_->activate();
		elide_names();
	}
	refitting_ = false;
}

void PeopleList::elide_names()
{
	// 末尾を省いたときの見え方を全部の行で求め、見えているテキストがほかの人の名前の先頭にも一致する行だけ、真ん中を
	// 省く。
	std::vector<PersonRowWidget *> order;
	std::vector<rows::NameShown> shown;
	for (const std::string &id : shown_order_) {
		PersonRowWidget *row = row_for(id);
		if (row == nullptr)
			continue;
		const QString full = row->full_name();
		const QString end = row->name_font_metrics().elidedText(full, Qt::ElideRight, std::max(0, row->name_width()));
		rows::NameShown s;
		s.full = to_u16(full);
		s.elided = end != full;
		QString prefix = end;
		if (s.elided && prefix.endsWith(QChar(kEllipsis)))
			prefix.chop(1);
		s.visible_prefix = to_u16(s.elided ? prefix : full);
		order.push_back(row);
		shown.push_back(s);
	}
	const std::vector<bool> middle = rows::middle_elide_rows(shown);
	for (std::size_t i = 0; i < order.size(); ++i) {
		order[i]->set_name_mode(middle[i] ? Qt::ElideMiddle : Qt::ElideRight);
		order[i]->refresh_accessibility();
	}
}

// ---- 行のメニュー ----

QMenu *PeopleList::make_menu_for(const std::string &user_id, QWidget *parent)
{
	// 製品の処理 (open_row_menu) がメニューを開かない行 (Vesktop のアカウントの行と、抜けた人の行) では作らない。
	// プレビューツールが製品で起きない形を描かないように、開く条件をそろえる。
	PersonRowWidget *row = row_for(user_id);
	if (row == nullptr || row->departed() || row->view().off || !(row->view().checkable || row->view().menu.auto_add_only))
		return nullptr;
	return make_row_menu(parent, row->view(), on_action_, on_rename_);
}

void PeopleList::open_row_menu(const std::string &user_id, QWidget *anchor, const QPoint &global_pos, bool keyboard)
{
	PersonRowWidget *row = row_for(user_id);
	if (row == nullptr || row->departed() || row->view().off || !(row->view().checkable || row->view().menu.auto_add_only))
		return;
	QPoint at = global_pos;
	if (keyboard && anchor != nullptr)
		at = anchor->mapToGlobal(QPoint(0, anchor->height()));
	const ledger::dockview::PersonRow view = row->view();
	run_menu(user_id, at, [this, view](const ActionCallback &on_action, const std::function<void(const std::string &)> &on_rename) {
		return make_row_menu(this, view, on_action, on_rename);
	});
}

void PeopleList::open_replace_menu(const std::string &user_id, QWidget *anchor)
{
	PersonRowWidget *row = row_for(user_id);
	if (row == nullptr || row->departed() || row->view().replace_choices.empty())
		return;
	// ポインターで押したときはポインターの位置、キーボードで押したときは行の下に開く。
	const QPoint at = ui::FocusRing::keyboard_mode() && anchor != nullptr ? anchor->mapToGlobal(QPoint(0, anchor->height()))
									      : QCursor::pos();
	const ledger::dockview::PersonRow view = row->view();
	const int px = metrics_.icon_px;
	const QColor color = theme_.text;
	run_menu(user_id, at, [this, view, px, color](const ActionCallback &on_action, const std::function<void(const std::string &)> &) {
		return make_replace_menu(this, view, color, px, on_action);
	});
}

void PeopleList::run_menu(const std::string &user_id, const QPoint &at,
			  const std::function<QMenu *(const ActionCallback &, const std::function<void(const std::string &)> &)> &make_menu)
{
	const QPointer<QWidget> focus_before = QApplication::focusWidget();
	// 押した項目は、メニューが閉じてから実行する (「名前を変更」のダイアログなどを、メニューの入れ子のイベントループの
	// 中で開かない)。
	// UI Automation (スクリーンリーダーや音声での操作) で項目を実行すると、Qt はメニューを閉じない (DockWidget の
	// open_dock_menu と同じ)。閉じないと exec から戻らず、押した操作を実行しないので、押されたら閉じる。
	std::function<void()> pending;
	QMenu *menu = nullptr;
	menu = make_menu(
		[this, &pending, &menu](ledger::ActionId id, const ledger::ActionArgs &args) {
			// 音量のスライダーはメニューを開いたまま、動かすたびにすぐ渡す (閉じてから実行する対象ではない)。
			if (id == ledger::ActionId::SetVoiceVolume || id == ledger::ActionId::SetScreenAudioVolume) {
				if (on_action_)
					on_action_(id, args);
				return;
			}
			pending = [this, id, args] {
				if (on_action_)
					on_action_(id, args);
			};
			if (menu != nullptr)
				menu->hide();
		},
		[this, &pending, &menu](const std::string &uid) {
			pending = [this, uid] {
				if (on_rename_)
					on_rename_(uid);
			};
			if (menu != nullptr)
				menu->hide();
		});
	open_menu_ = menu;
	open_menu_user_ = user_id;
	// メニューが開いている間は、一覧の位置が動く変化を後に回す (ドック全体の並びの保留は layout_hold.h の LayoutHold)。
	if (on_menu_open_)
		on_menu_open_(true);
	menu->exec(at);
	open_menu_ = nullptr;
	open_menu_user_.clear();
	delete menu;
	if (on_menu_open_)
		on_menu_open_(false);
	// メニューが開いている間にためておいた並びの変化を、並びの保留の間でなければここで反映する。
	if (!held_ && structure_differs())
		apply_structure();
	// メニューを閉じたら、フォーカスを開く前のウィジェットに戻す。そのウィジェットが消えたか押せなくなっていたら
	// 戻さない。
	if (focus_before && focus_before->isVisible() && focus_before->isEnabled())
		focus_before->setFocus(Qt::PopupFocusReason);
	if (pending)
		pending();
}

// ---- 位置の監視 (クリックを捨てるガード) ----

void PeopleList::track_ancestors()
{
	for (const QPointer<QWidget> &w : tracked_ancestors_) {
		if (w)
			w->removeEventFilter(this);
	}
	tracked_ancestors_.clear();
	for (const QMetaObject::Connection &c : scroll_connections_)
		QObject::disconnect(c);
	scroll_connections_.clear();
	// 祖先が動くか大きさを変えると、行はウィンドウの上で動くことがある。一覧の上にメッセージのカードが入ったとき
	// (スクロール領域の中身の中の動き) に加えて、ドックの上端の高さが変わってスクロール領域そのものが動いたときと、
	// スクロール領域の中身が自動でスクロールしたとき (フォーカスに合わせたスクロール、中身が縮んだときの詰め) も見る。
	// ウィンドウそのものとその上も見る (フローティングのドックをメインウィンドウに戻すと、ドックは親を変えずに
	// ウィンドウでなくなる)。ウィンドウが動いてもウィンドウの座標は変わらないので、ガードの対象にはならない。
	for (QWidget *w = this; w != nullptr; w = w->parentWidget()) {
		w->installEventFilter(this);
		tracked_ancestors_.emplace_back(w);
		// 利用者のスクロールは、スクロールバーの actionTriggered (ホイール、スクロールバー、キーのどれでも出る) で見分ける。
		if (auto *area = qobject_cast<QAbstractScrollArea *>(w)) {
			for (QScrollBar *bar : {area->verticalScrollBar(), area->horizontalScrollBar()}) {
				if (bar == nullptr)
					continue;
				scroll_connections_.push_back(QObject::connect(bar, &QAbstractSlider::actionTriggered, this,
										 [this](int) { note_user_scroll(); }));
				// 値が変わり終えたら false にする。QAbstractScrollArea は作るときに valueChanged を中身の移動に
				// 接続する (Qt 6.8.3 の qabstractscrollarea.cpp の init)。ここで接続するのはその後なので、この
				// シグナルは中身が動いた後に届く。
				scroll_connections_.push_back(QObject::connect(bar, &QAbstractSlider::valueChanged, this,
										 [this](int) { end_user_scroll(); }));
			}
		}
	}
}

void PeopleList::note_user_scroll()
{
	// actionTriggered はスクロールバーの値が変わる前に出て、中身の移動 (Move) は同じ呼び出しの中の valueChanged で
	// 続く。フラグは valueChanged の後 (end_user_scroll) で false にする。値が変わらない操作 (端でのホイール) は
	// valueChanged を出さないので、同じイベントループの周回の終わりでも false にする。周回の終わりだけで false にすると、
	// フラグが true の間に DockPoll の更新が並びを動かしたときに、その動きがガードの対象から外れる。
	if (user_scrolling_)
		return;
	user_scrolling_ = true;
	QTimer::singleShot(0, this, [this] { end_user_scroll(); });
}

void PeopleList::end_user_scroll()
{
	if (!user_scrolling_)
		return;
	user_scrolling_ = false;
	// false にした時点の矩形を基準にする (スクロールの最中に記録しなかった動きを、後でガードの対象にしない)。
	note_positions(false);
}

void PeopleList::note_positions(bool mark_moved)
{
	// ウィンドウの座標で比べる (押した点にいる人が替わる動きは、どの祖先が動いてもウィンドウの上の矩形に表れる)。
	QWidget *anchor = window();
	const bool mark = mark_moved && !user_scrolling_;
	const std::int64_t now = PersonRowWidget::now_ms();
	for (auto &[id, row] : rows_) {
		if (row->isHidden())
			continue;
		const QRect rect(row->mapTo(anchor, QPoint(0, 0)), row->size());
		const auto it = positions_.find(id);
		if (it == positions_.end()) {
			positions_[id] = rect;
			continue;
		}
		if (it->second != rect) {
			it->second = rect;
			if (mark)
				row->mark_changed(now);
		}
	}
}

bool PeopleList::eventFilter(QObject *watched, QEvent *event)
{
	switch (event->type()) {
	case QEvent::Move:
	case QEvent::Resize:
		note_positions(true);
		break;
	case QEvent::Show:
		// 現れた直後も、ダブルクリックの時間だけクリックを受け付けない。
		if (auto *row = dynamic_cast<PersonRowWidget *>(watched))
			row->mark_changed(PersonRowWidget::now_ms());
		break;
	case QEvent::ParentChange:
		// 祖先の親が変わったら (ドックの付け替えなど)、監視する祖先を選び直す。イベントを配っている最中に
		// イベントフィルターを外さないように、イベントループの次の周回で行う。
		if (watched != this && dynamic_cast<PersonRowWidget *>(watched) == nullptr)
			QTimer::singleShot(0, this, [this] {
				track_ancestors();
				note_positions(true);
			});
		break;
	default:
		break;
	}
	return QWidget::eventFilter(watched, event);
}

void PeopleList::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	schedule_refit();
}

void PeopleList::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);
	switch (event->type()) {
	case QEvent::StyleChange:
	case QEvent::FontChange:
	case QEvent::PaletteChange:
		// DockWidget がテーマを渡していない間は、自分でテーマの値を読み込み直す。
		if (!has_theme_ || !has_metrics_) {
			env_ready_ = false;
			if (!rows_.empty())
				ensure_env();
			schedule_refit();
		}
		break;
	case QEvent::ParentChange:
		track_ancestors();
		break;
	default:
		break;
	}
}

void PeopleList::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	track_ancestors();
	note_positions(false);
	schedule_refit();
}

// ---- プレビューツールとテストが使う関数 ----

QWidget *PeopleList::list_widget() const
{
	return list_;
}

PersonRowWidget *PeopleList::row_for(const std::string &user_id) const
{
	const auto it = rows_.find(user_id);
	return it == rows_.end() ? nullptr : it->second;
}

std::vector<std::string> PeopleList::shown_user_ids() const
{
	return shown_order_;
}

} // namespace cordmux::dock
