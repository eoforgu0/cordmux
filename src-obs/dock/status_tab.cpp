// SPDX-License-Identifier: Apache-2.0

// 状態タブの中身 (status_tab.h) を実装する。

#include "dock/status_tab.h"

#include <QDoubleSpinBox>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <set>

#include "dock/layout_hold.h"
#include "dock/ui/elide_label.h"
#include "dock/ui/form_parts.h"
#include "dock/ui/no_wheel.h"
#include "dock/ui/set_class.h"
#include "dock/ui/slider_spin.h"
#include "dock/ui/text_resolve.h"
#include "dock/ui/theme_read.h"
#include "dock/ui/type_scale.h"
#include "dock/ui/wrap_label.h"
#include "dock/windows_calc.h"
#include "ledger/look_tab.h"
#include "ledger/status_tab.h"
#include "ledger/text_ids.h"

namespace cordmux::dock {

namespace keys = ledger::text::keys;

namespace {

QString tr_key(const char *key)
{
	return ui::resolve_key(key);
}

QString tr_ref(const char *id, std::vector<std::string> args = {})
{
	ledger::TextRef ref;
	ref.id = id;
	ref.args = std::move(args);
	return ui::resolve(ref);
}

// 表の列。名前、途切れ、合計、遅延、最大。
constexpr int kColumns = 5;

// 秒で表示する合計 (四捨五入)。
std::uint64_t total_seconds(std::int64_t us)
{
	return us <= 0 ? 0 : static_cast<std::uint64_t>((us + 500'000) / 1'000'000);
}

void set_if_changed(QLabel *label, const QString &text)
{
	if (label->text() != text)
		label->setText(text);
}

} // namespace

StatusTab::StatusTab(QWidget *parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("cordmuxStatusTab"));
	reflect_timer_ = new QTimer(this);
	reflect_timer_->setSingleShot(true);
	QObject::connect(reflect_timer_, &QTimer::timeout, this, [this]() {
		if (dirty_)
			reflect_now();
	});
	build();
	ui::focus_by_tab_only(this);
}

StatusTab::~StatusTab() = default;

void StatusTab::build()
{
	root_ = new QVBoxLayout(this);
	root_->setContentsMargins(0, 0, 0, 0);

	const auto add_heading = [this](const char *key) {
		auto *label = new QLabel(tr_key(key));
		label->setTextFormat(Qt::PlainText);
		headings_.push_back(label);
		root_->addWidget(label);
		return label;
	};

	// ---- 現在のメッセージ ----
	notices_heading_ = add_heading(keys::kStatusNoticesHeading);
	notices_ = new NoticeList(this);
	notices_->hide();
	root_->addWidget(notices_);
	notices_none_ = new QLabel(tr_key(keys::kStatusNoticesNoneStatus));
	notices_none_->setObjectName(QStringLiteral("cordmuxStatusNoticesNone"));
	notices_none_->setTextFormat(Qt::PlainText);
	root_->addWidget(notices_none_);

	// ---- cordmuxの録画 ----
	recording_heading_ = add_heading(keys::kStatusRecordingHeading);
	recording_now_ = new QLabel(tr_key(keys::kStatusRecordingNowText));
	recording_now_->setObjectName(QStringLiteral("cordmuxStatusRecordingNow"));
	recording_now_->setTextFormat(Qt::PlainText);
	root_->addWidget(recording_now_);
	recording_last_ = new ui::WrapLabel();
	recording_last_->setObjectName(QStringLiteral("cordmuxStatusRecordingLast"));
	root_->addWidget(recording_last_);
	open_folder_ = new QPushButton(tr_key(keys::kDialogOpenFolderButton));
	open_folder_->setObjectName(QStringLiteral("cordmuxStatusOpenFolder"));
	open_folder_->setAutoDefault(false);
	new PressGuard(open_folder_);
	root_->addWidget(open_folder_, 0, Qt::AlignLeft);
	QObject::connect(open_folder_, &QPushButton::clicked, this, [this]() {
		if (opener_ && !open_folder_path_.empty())
			opener_(open_folder_path_, ledger::dockview::LocationKind::Folder);
	});
	recording_none_ = new QLabel(tr_key(keys::kStatusRecordingNoneStatus));
	recording_none_->setObjectName(QStringLiteral("cordmuxStatusRecordingNone"));
	recording_none_->setTextFormat(Qt::PlainText);
	root_->addWidget(recording_none_);
	recording_now_->hide();
	recording_last_->hide();
	open_folder_->hide();

	// ---- 映像の受信 ----
	receive_heading_ = add_heading(keys::kStatusReceiveHeading);
	table_ = new QWidget(this);
	table_->setObjectName(QStringLiteral("cordmuxStatusTable"));
	grid_ = new QGridLayout(table_);
	grid_->setContentsMargins(0, 0, 0, 0);
	grid_->setColumnStretch(0, 1);
	const char *header_keys[kColumns] = {keys::kStatusColNameLabel, keys::kStatusColGapsLabel, keys::kStatusColTotalLabel,
					     keys::kStatusColLateLabel, keys::kStatusColMaxLabel};
	for (int c = 0; c < kColumns; ++c) {
		auto *header = new QLabel(tr_key(header_keys[c]), table_);
		header->setTextFormat(Qt::PlainText);
		header->setObjectName(QStringLiteral("cordmuxStatusHeader%1").arg(c));
		grid_->addWidget(header, 0, c, c == 0 ? Qt::Alignment() : Qt::Alignment(Qt::AlignRight));
		headers_.push_back(header);
	}
	root_->addWidget(table_);
	receive_empty_ = new ui::WrapLabel(tr_key(keys::kStatusReceiveEmptyEmpty));
	receive_empty_->setObjectName(QStringLiteral("cordmuxStatusReceiveEmpty"));
	root_->addWidget(receive_empty_);
	receive_no_composite_ = new ui::WrapLabel(tr_key(keys::kStatusReceiveNoCompositeText));
	receive_no_composite_->setObjectName(QStringLiteral("cordmuxStatusNoComposite"));
	root_->addWidget(receive_no_composite_);
	receive_note_ = new ui::WrapLabel(tr_key(keys::kStatusReceiveNoteText));
	receive_note_->setObjectName(QStringLiteral("cordmuxStatusReceiveNote"));
	// 説明の文は入れ物に入れ、下の余白を入れ物に持たせる (apply_look_to_widgets)。
	receive_note_box_ = ui::note_box(receive_note_);
	root_->addWidget(receive_note_box_);
	reset_ = new QPushButton(tr_key(keys::kStatusResetButton));
	reset_->setObjectName(QStringLiteral("cordmuxStatusReset"));
	reset_->setAutoDefault(false);
	new PressGuard(reset_);
	root_->addWidget(reset_, 0, Qt::AlignLeft);
	QObject::connect(reset_, &QPushButton::clicked, this, [this]() {
		if (on_press_ && !reset_view_.disabled_reason.has_value())
			on_press_(reset_view_);
	});

	// [映像のバッファ]。範囲と刻みは設定のまま。
	buffer_form_ = ui::make_form();
	auto *form = buffer_form_;
	form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	buffer_ = ui::SliderSpin::integer(config::kDisplayBufferMinMs, config::kDisplayBufferMaxMs, 10,
					  tr_key(keys::kSettingsUnitMsLabel));
	// つまみのダブルクリックで、設定の既定値に戻す。
	buffer_->set_default_value(config::Settings{}.display_buffer_ms);
	buffer_->setObjectName(QStringLiteral("cordmuxStatus_display_buffer_ms"));
	buffer_->spin()->setObjectName(QStringLiteral("cordmuxStatus_display_buffer_ms_spin"));
	if (buffer_->slider() != nullptr)
		buffer_->slider()->setObjectName(QStringLiteral("cordmuxStatus_display_buffer_ms_slider"));
	buffer_->spin()->setToolTip(tr_key(keys::kStatusVideoBufferTip));
	buffer_->spin()->setAccessibleName(tr_key(keys::kStatusVideoBufferLabel));
	if (buffer_->slider() != nullptr)
		buffer_->slider()->setAccessibleName(tr_key(keys::kStatusVideoBufferLabel));
	buffer_label_ = ui::add_form_row(form, tr_key(keys::kStatusVideoBufferLabel), buffer_);
	buffer_->set_on_changed([this]() { on_buffer_input_changed(); });
	root_->addLayout(form);

	// ---- Vesktop ----
	// Vesktop の設定が同梱の Vencord を指していて、Vesktop が起動していない間だけ表示する
	// (dockview::StatusVesktopView)。映像の受信の後、履歴の前に置く。配信中に見る表とバッファより下にし、毎回は
	// 使わない操作で上の組を押し下げない。履歴より上にするのは、履歴が最大 60 行まで伸び、その下では見つけにくいため
	// である。押せない理由 (この起動で Vesktop の設定を書かないとき) は、メッセージのカードのボタンと同じく、ボタンの
	// 下に文で表示する (ツールチップだけにしない)。
	vesktop_heading_ = add_heading(keys::kStatusVesktopHeading);
	vesktop_heading_->setObjectName(QStringLiteral("cordmuxStatusVesktopHeading"));
	vesktop_text_ = new ui::WrapLabel(tr_key(keys::kStatusVesktopAddedText));
	vesktop_text_->setObjectName(QStringLiteral("cordmuxStatusVesktopText"));
	root_->addWidget(vesktop_text_);
	vesktop_remove_ = new QPushButton(tr_key(keys::kStatusRemoveFromVesktopButton));
	vesktop_remove_->setObjectName(QStringLiteral("cordmuxStatusRemoveFromVesktop"));
	vesktop_remove_->setAutoDefault(false);
	new PressGuard(vesktop_remove_);
	root_->addWidget(vesktop_remove_, 0, Qt::AlignLeft);
	QObject::connect(vesktop_remove_, &QPushButton::clicked, this, [this]() {
		if (on_press_ && shown_vesktop_.has_value() && !shown_vesktop_->remove.disabled_reason.has_value())
			on_press_(shown_vesktop_->remove);
	});
	vesktop_reason_ = new ui::WrapLabel();
	vesktop_reason_->setObjectName(QStringLiteral("cordmuxStatusVesktopReason"));
	// 折り返し方は、メッセージのカードのボタンの下の押せない理由と同じにする (notice_list.cpp の NoticeButtons)。
	vesktop_reason_->set_breaks(ui::WrapBreaks::KeepNumbers);
	root_->addWidget(vesktop_reason_);
	vesktop_heading_->hide();
	vesktop_text_->hide();
	vesktop_remove_->hide();
	vesktop_reason_->hide();

	// ---- 履歴 ----
	history_heading_ = add_heading(keys::kStatusHistoryHeading);
	history_box_ = new QWidget(this);
	history_box_->setObjectName(QStringLiteral("cordmuxStatusHistory"));
	history_grid_ = new QGridLayout(history_box_);
	history_grid_->setContentsMargins(0, 0, 0, 0);
	history_grid_->setColumnStretch(1, 1);
	root_->addWidget(history_box_);
	history_none_ = new QLabel(tr_key(keys::kStatusHistoryNoneStatus));
	history_none_->setObjectName(QStringLiteral("cordmuxStatusHistoryNone"));
	history_none_->setTextFormat(Qt::PlainText);
	root_->addWidget(history_none_);
	history_box_->hide();

	receive_empty_->hide();
	receive_no_composite_->hide();
	table_->hide();
}

void StatusTab::style_muted(QLabel *label)
{
	// 薄い色のテキストが背景の上で読めるテーマだけ使う。読めないテーマは本文の色のまま。
	ui::set_class(label, ui::muted_ok(look_.theme, look_.theme.base_bg) ? QStringLiteral("text-muted") : QString());
}

void StatusTab::set_look(const DockLook &look)
{
	look_ = look;
	has_look_ = true;
	apply_look_to_widgets();
	notices_->set_look(look_);
}

void StatusTab::apply_look_to_widgets()
{
	const ui::DockMetrics &m = look_.metrics;
	const int small = m.small_px;
	// 縦の並びの間は small なので、各部分の余白は、決めた余白から small を引いた残りにする (DockStyle の見出しの段のコメント)。
	const auto less_small = [small](int px) { return std::max(0, px - small); };
	// 見出しは、表示タブの中見出しと同じ見た目 (本文の色の太字) で、下も同じ 0.25 em にする。上は状態タブだけの 1 em
	// (DockStyle::status_heading_above_em。表示タブより狭くする理由はそのコメント)。このタブには見出しの段が 1 つしかないので、
	// 大見出しの大きさと罫線は使わない。
	bool first = true;
	for (QLabel *heading : headings_) {
		ui::apply_type(heading, ui::TypeLevel::Heading, look_.base_pt, 1.0);
		heading->setContentsMargins(0, first ? 0 : less_small(m.status_heading_above_px), 0,
					    less_small(m.sub_heading_below_px));
		first = false;
	}
	// 表の下の説明の文 (途切れと合計、遅延率と最大が何の値か) は、下に note_below を空けて、[途切れの累計をリセット] と分ける。
	ui::set_note_box_margins(receive_note_box_, 0, less_small(m.note_below_px));
	// [映像のバッファ] は、表と [途切れの累計をリセット] とは意味の違う設定なので、上に section_gap を空ける。
	if (buffer_form_ != nullptr)
		buffer_form_->setContentsMargins(0, less_small(m.section_gap_px), 0, 0);
	root_->setSpacing(small);
	grid_->setHorizontalSpacing(look_.metrics.large_px);
	grid_->setVerticalSpacing(small);
	history_grid_->setHorizontalSpacing(look_.metrics.large_px);
	history_grid_->setVerticalSpacing(small);
	for (QLabel *w : {notices_none_, recording_none_, history_none_, static_cast<QLabel *>(receive_empty_),
			  static_cast<QLabel *>(receive_no_composite_), static_cast<QLabel *>(receive_note_),
			  static_cast<QLabel *>(recording_last_)})
		style_muted(w);
	for (QLabel *header : headers_)
		style_muted(header);
	for (QLabel *w : history_when_)
		style_muted(w);
	for (Row &row : rows_) {
		for (QLabel *cell : {row.gaps, row.total, row.late, row.max})
			cell->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		// 名前の列の最小の幅。ドックを最小の幅にしても、表が横にはみ出さない幅にする (dock_drive の status の
		// 「最小の幅 ... でも、横にはみ出さない」で確認する)。
		row.name->setMinimumWidth(static_cast<int>(look_.metrics.em_px * 2.5));
	}
	for (ui::WrapLabel *label :
	     {recording_last_, receive_empty_, receive_no_composite_, receive_note_, vesktop_text_, vesktop_reason_})
		label->setMaximumWidth(look_.metrics.paragraph_max_px);
	for (ui::WrapLabel *label : history_text_)
		label->setMaximumWidth(look_.metrics.paragraph_max_px);
}

void StatusTab::retire(QWidget *w)
{
	// 表示中のウィジェットを、イベントの最中に削除しない (NoticeList の retire_widget と同じ理由)。
	if (w == nullptr)
		return;
	w->hide();
	w->deleteLater();
}

void StatusTab::arm_guards()
{
	PressGuard::arm_widget(reset_);
	PressGuard::arm_widget(open_folder_);
	PressGuard::arm_widget(vesktop_remove_);
}

void StatusTab::set_on_press(NoticeButtonCallback cb)
{
	on_press_ = cb;
	notices_->set_on_press(std::move(cb));
}

void StatusTab::set_folder_opener(FolderOpener opener)
{
	opener_ = opener;
	notices_->set_folder_opener(std::move(opener));
}

void StatusTab::set_busy(const std::set<ledger::ActionId> &busy)
{
	notices_->set_busy(busy);
}

bool StatusTab::update(const std::vector<ledger::dockview::Notice> &notices, const ledger::dockview::StatusTabView &view,
		       bool allow_moves)
{
	bool pending = false;
	if (notices_->update(notices, allow_moves))
		pending = true;
	notices_none_->setVisible(notices_->empty());

	// 押せるかはボタンの View が決める。位置は動かない。
	if (!has_shown_ || view.reset != reset_view_) {
		reset_view_ = view.reset;
		const QString text = ui::resolve(view.reset.label);
		if (reset_->text() != text)
			reset_->setText(text);
		const QString reason = view.reset.disabled_reason.has_value() ? ui::resolve(*view.reset.disabled_reason) : QString();
		reset_->setEnabled(!view.reset.disabled_reason.has_value());
		reset_->setToolTip(reason);
	}

	// 録画の節。3 つの状態は高さが違うので、状態が替わる変化は位置を動かす変化として扱う。同じ状態の中の文の更新は、すぐ行う。
	{
		const ledger::dockview::StatusRecordingView &rec = view.recording;
		const bool structural = !has_shown_ || rec.section != shown_.recording.section ||
					rec.folder.empty() != shown_.recording.folder.empty();
		if (!structural || allow_moves) {
			apply_recording(rec);
			shown_.recording = rec;
			// 現れたり動いたりしたボタンだけ、直後のクリックを捨てるガードをかけ直す (周期ごとにかけ直すと、押せなくなる)。
			if (structural)
				arm_guards();
		} else {
			pending = true;
		}
	}

	apply_receive(view, allow_moves, &pending);

	// 「Vesktop」の組。組が現れるか消えるとき、押せない理由の文が現れるか消えるときは高さが変わるので、位置を動かす
	// 変化として扱う。同じ高さのままの変化 (理由の文の中身) は、すぐ行う。
	{
		const auto has_reason = [](const std::optional<ledger::dockview::StatusVesktopView> &v) {
			return v.has_value() && v->remove.disabled_reason.has_value();
		};
		const bool structural = !has_shown_ || view.vesktop.has_value() != shown_vesktop_.has_value() ||
					has_reason(view.vesktop) != has_reason(shown_vesktop_);
		if (!structural || allow_moves) {
			apply_vesktop(view.vesktop);
			if (structural)
				arm_guards();
		} else {
			pending = true;
		}
	}

	apply_history(view.history);
	has_shown_ = true;
	moves_pending_ = pending;
	return pending;
}

void StatusTab::apply_recording(const ledger::dockview::StatusRecordingView &view)
{
	using Section = ledger::dockview::StatusRecordingSection;
	recording_now_->setVisible(view.section == Section::Recording);
	recording_none_->setVisible(view.section == Section::None);
	recording_last_->setVisible(view.section == Section::Last);
	const bool has_folder = view.section == Section::Last && !view.folder.empty();
	open_folder_->setVisible(has_folder);
	open_folder_path_ = view.folder;
	if (view.section == Section::Last) {
		const QString text = ui::resolve(view.last);
		if (recording_last_->source_text() != text)
			recording_last_->set_text(text);
	}
	if (has_folder)
		open_folder_->setAccessibleDescription(QString::fromStdString(view.folder));
}

QLabel *StatusTab::vesktop_reason_label() const
{
	return vesktop_reason_;
}

void StatusTab::apply_vesktop(const std::optional<ledger::dockview::StatusVesktopView> &view)
{
	shown_vesktop_ = view;
	const bool shown = view.has_value();
	vesktop_heading_->setVisible(shown);
	vesktop_text_->setVisible(shown);
	vesktop_remove_->setVisible(shown);
	if (!shown) {
		vesktop_reason_->hide();
		return;
	}
	const QString text = ui::resolve(view->text);
	if (vesktop_text_->source_text() != text)
		vesktop_text_->set_text(text);
	const QString label = ui::resolve(view->remove.label);
	if (vesktop_remove_->text() != label)
		vesktop_remove_->setText(label);
	const bool enabled = !view->remove.disabled_reason.has_value();
	vesktop_remove_->setEnabled(enabled);
	// 押せない理由は、ボタンの下の文と、ツールチップとアクセシブルな説明に入れる (メッセージのカードのボタンと同じ。
	// notice_list.cpp の NoticeButtons::apply_views)。
	const QString reason = enabled ? QString() : ui::resolve(*view->remove.disabled_reason);
	vesktop_remove_->setToolTip(reason);
	vesktop_remove_->setAccessibleDescription(reason);
	if (vesktop_reason_->source_text() != reason)
		vesktop_reason_->set_text(reason);
	vesktop_reason_->setVisible(!enabled);
}

void StatusTab::apply_receive(const ledger::dockview::StatusTabView &view, bool allow_moves, bool *pending)
{
	std::vector<std::string> order;
	order.reserve(view.rows.size());
	for (const ledger::dockview::StatusReceiveRow &row : view.rows)
		order.push_back(row.user_id);
	std::vector<std::string> shown_order;
	for (const Row &row : rows_)
		shown_order.push_back(row.user_id);

	const bool structural = !has_shown_ || view.composite_present != shown_.composite_present || order != shown_order;
	if (structural && !allow_moves) {
		// 位置が動く変化は保留が解けてから行う。今ある行の値だけを書き換える。
		*pending = true;
		for (const ledger::dockview::StatusReceiveRow &item : view.rows) {
			for (Row &row : rows_) {
				if (row.user_id == item.user_id) {
					data_[item.user_id] = item;
					fill_row(row, item);
				}
			}
		}
		return;
	}

	if (structural) {
		std::map<std::string, ledger::dockview::StatusReceiveRow> next_data;
		for (const ledger::dockview::StatusReceiveRow &item : view.rows)
			next_data[item.user_id] = item;
		data_ = std::move(next_data);
		relayout_rows(order);
		shown_.composite_present = view.composite_present;
		table_->setVisible(view.composite_present && !rows_.empty());
		receive_empty_->setVisible(view.composite_present && rows_.empty());
		receive_no_composite_->setVisible(!view.composite_present);
		// 説明の文とリセットは表についてのものなので、表がないときは出さない。
		receive_note_->setVisible(view.composite_present && !rows_.empty());
		receive_note_box_->setVisible(view.composite_present && !rows_.empty());
		reset_->setVisible(view.composite_present && !rows_.empty());
		arm_guards();
	} else {
		for (const ledger::dockview::StatusReceiveRow &item : view.rows)
			data_[item.user_id] = item;
	}
	for (Row &row : rows_) {
		const auto it = data_.find(row.user_id);
		if (it != data_.end())
			fill_row(row, it->second);
	}
}

StatusTab::Row StatusTab::make_row(const std::string &user_id)
{
	Row row;
	row.user_id = user_id;
	row.name = new ui::ElideLabel(table_);
	row.name->setObjectName(QStringLiteral("cordmuxStatusName"));
	row.name->setMinimumWidth(has_look_ ? static_cast<int>(look_.metrics.em_px * 2.5) : 24);
	const auto number_label = [this](const char *name) {
		auto *label = new QLabel(table_);
		label->setObjectName(QString::fromLatin1(name));
		label->setTextFormat(Qt::PlainText);
		label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		return label;
	};
	row.gaps = number_label("cordmuxStatusGaps");
	row.total = number_label("cordmuxStatusTotal");
	row.late = number_label("cordmuxStatusLate");
	row.max = number_label("cordmuxStatusMax");
	return row;
}

void StatusTab::relayout_rows(const std::vector<std::string> &order)
{
	// 見出し以外のセルをレイアウトから外し、View の順に置き直す。残る行のウィジェットは作り直さない。
	std::vector<Row> next;
	std::set<std::string> wanted(order.begin(), order.end());
	for (const std::string &user_id : order) {
		auto it = std::find_if(rows_.begin(), rows_.end(), [&](const Row &r) { return r.user_id == user_id; });
		next.push_back(it != rows_.end() ? *it : make_row(user_id));
	}
	for (const Row &row : rows_) {
		if (wanted.count(row.user_id) == 0) {
			for (QWidget *w : {static_cast<QWidget *>(row.name), static_cast<QWidget *>(row.gaps),
					   static_cast<QWidget *>(row.total), static_cast<QWidget *>(row.late),
					   static_cast<QWidget *>(row.max)}) {
				grid_->removeWidget(w);
				retire(w);
			}
		}
	}
	for (const Row &row : next) {
		for (QWidget *w : {static_cast<QWidget *>(row.name), static_cast<QWidget *>(row.gaps),
				   static_cast<QWidget *>(row.total), static_cast<QWidget *>(row.late),
				   static_cast<QWidget *>(row.max)})
			grid_->removeWidget(w);
	}
	rows_ = std::move(next);
	int grid_row = 1;
	for (const Row &row : rows_) {
		grid_->addWidget(row.name, grid_row, 0);
		grid_->addWidget(row.gaps, grid_row, 1, Qt::AlignRight);
		grid_->addWidget(row.total, grid_row, 2, Qt::AlignRight);
		grid_->addWidget(row.late, grid_row, 3, Qt::AlignRight);
		grid_->addWidget(row.max, grid_row, 4, Qt::AlignRight);
		for (QWidget *w : {static_cast<QWidget *>(row.name), static_cast<QWidget *>(row.gaps),
				   static_cast<QWidget *>(row.total), static_cast<QWidget *>(row.late),
				   static_cast<QWidget *>(row.max)})
			w->show();
		++grid_row;
	}
}

void StatusTab::fill_row(Row &row, const ledger::dockview::StatusReceiveRow &item)
{
	const QString name = ui::resolve_label(item.name);
	if (row.name->full_text() != name)
		row.name->set_full_text(name);

	const bool has_data = pipeline::frame_count(item.streams) != 0;
	const windows_calc::ArrivalCells cells = windows_calc::arrival_cells(item.streams, buffer_ms());
	const QString gaps = tr_ref(keys::kStatusGapsStatus, {windows_calc::group_digits(item.gap_count)});
	const QString total = tr_ref(keys::kStatusTotalStatus, {windows_calc::group_digits(total_seconds(item.gap_total_us))});
	const QString dash = QStringLiteral("-");
	const QString late = has_data ? tr_ref(keys::kStatusLateStatus, {windows_calc::percent_text(cells.late_pct)}) : dash;
	const QString max =
		has_data ? tr_ref(keys::kStatusMaxStatus, {windows_calc::group_digits(static_cast<std::uint64_t>(cells.max_ms))})
			 : dash;
	set_if_changed(row.gaps, gaps);
	set_if_changed(row.total, total);
	set_if_changed(row.late, late);
	set_if_changed(row.max, max);

	// 行のツールチップ。直近 1 分の詳しい値。
	QString tip;
	if (has_data) {
		QStringList lines;
		lines << tr_key(keys::kStatusTipPeriodStatus);
		lines << tr_ref(keys::kStatusTipLateStatus,
				{windows_calc::group_digits(cells.late), windows_calc::group_digits(cells.frames),
				 windows_calc::percent_text(cells.late_pct)});
		lines << tr_ref(keys::kStatusTipStutterStatus, {windows_calc::group_digits(cells.stutters)});
		lines << tr_ref(keys::kStatusTipMeanStatus, {windows_calc::group_digits(static_cast<std::uint64_t>(cells.mean_ms))});
		lines << tr_ref(keys::kStatusTipMaxStatus, {windows_calc::group_digits(static_cast<std::uint64_t>(cells.max_ms))});
		tip = lines.join(QLatin1Char('\n'));
	}
	row.name->set_base_tool_tip(tip);
	for (QLabel *cell : {row.gaps, row.total, row.late, row.max}) {
		if (cell->toolTip() != tip)
			cell->setToolTip(tip);
	}
	// スクリーンリーダー向けに、セルの名前を「列の見出し 値」にする。
	const QString names[4] = {headers_[1]->text(), headers_[2]->text(), headers_[3]->text(), headers_[4]->text()};
	QLabel *cell_labels[4] = {row.gaps, row.total, row.late, row.max};
	for (int i = 0; i < 4; ++i) {
		const QString accessible = name + QLatin1Char(' ') + names[i] + QLatin1Char(' ') + cell_labels[i]->text();
		if (cell_labels[i]->accessibleName() != accessible)
			cell_labels[i]->setAccessibleName(accessible);
	}
}

void StatusTab::refill_rows()
{
	for (Row &row : rows_) {
		const auto it = data_.find(row.user_id);
		if (it != data_.end())
			fill_row(row, it->second);
	}
}

void StatusTab::apply_history(const std::vector<ledger::dockview::StatusHistoryLine> &lines)
{
	// 変わっていなければ何も作り直さない。
	if (lines == history_shown_ && history_none_shown_ == lines.empty() && has_shown_)
		return;
	history_shown_ = lines;
	history_none_shown_ = lines.empty();
	history_none_->setVisible(lines.empty());
	history_box_->setVisible(!lines.empty());
	// 行のウィジェットは足りない分だけ足し、書き換えるのは文だけにする (最大 60 行)。新しい行は先頭に来るので、全部の文を
	// 1 つずつずらして書き込む。
	while (history_text_.size() < lines.size()) {
		const int row = static_cast<int>(history_text_.size());
		auto *when = new QLabel(history_box_);
		when->setTextFormat(Qt::PlainText);
		when->setObjectName(QStringLiteral("cordmuxStatusHistoryWhen"));
		auto *text = new ui::WrapLabel(QString(), history_box_);
		text->setObjectName(QStringLiteral("cordmuxStatusHistoryText"));
		text->setMaximumWidth(has_look_ ? look_.metrics.paragraph_max_px : 533);
		history_grid_->addWidget(when, row, 0, Qt::AlignTop);
		history_grid_->addWidget(text, row, 1);
		history_when_.push_back(when);
		history_text_.push_back(text);
		style_muted(when);
	}
	for (std::size_t i = 0; i < history_text_.size(); ++i) {
		const bool used = i < lines.size();
		history_when_[i]->setVisible(used);
		history_text_[i]->setVisible(used);
		if (!used)
			continue;
		const ledger::dockview::StatusHistoryLine &line = lines[i];
		ledger::TextRef text = line.text;
		if (line.subject.has_value())
			text.args.insert(text.args.begin(), ui::resolve(*line.subject).toStdString());
		set_if_changed(history_when_[i], tr_ref(keys::kStatusHistoryWhenStatus, {line.when}));
		const QString resolved = ui::resolve(text);
		if (history_text_[i]->source_text() != resolved)
			history_text_[i]->set_text(resolved);
	}
}

QWidget *StatusTab::cell(int row, int column) const
{
	if (column < 0 || column >= kColumns)
		return nullptr;
	if (row == 0)
		return headers_[static_cast<std::size_t>(column)];
	if (row < 1 || row > static_cast<int>(rows_.size()))
		return nullptr;
	const Row &r = rows_[static_cast<std::size_t>(row - 1)];
	switch (column) {
	case 0:
		return r.name;
	case 1:
		return r.gaps;
	case 2:
		return r.total;
	case 3:
		return r.late;
	default:
		return r.max;
	}
}

// ---- 映像のバッファ ----

int StatusTab::buffer_ms() const
{
	return static_cast<int>(std::lround(buffer_->value()));
}

void StatusTab::set_buffer_value(int ms)
{
	const bool differs = ms != buffer_ms();
	if (!ledger::look_field_follows(buffer_->editing(), dirty_, differs))
		return;
	loading_ = true;
	buffer_->set_value(ms);
	loading_ = false;
	refill_rows();
}

void StatusTab::on_buffer_input_changed()
{
	if (loading_)
		return;
	// 表の遅延と最大を、入力欄の値ですぐ計算し直す。
	refill_rows();
	request_reflect();
}

void StatusTab::request_reflect()
{
	// 入力が止まってから渡す。動いている間はタイマーをかけ直す。
	dirty_ = true;
	reflect_timer_->start(ledger::kStatusBufferIdleMs);
}

void StatusTab::reflect_now()
{
	dirty_ = false;
	if (on_buffer_changed_)
		on_buffer_changed_(buffer_ms());
}

void StatusTab::flush()
{
	reflect_timer_->stop();
	if (dirty_)
		reflect_now();
}

} // namespace cordmux::dock
