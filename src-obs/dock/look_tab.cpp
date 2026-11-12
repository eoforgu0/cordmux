// SPDX-License-Identifier: Apache-2.0

// 表示タブの中身 (look_tab.h) を実装する。

#include "dock/look_tab.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardItemModel>
#include <QStyle>
#include <QStyleOptionButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>

#include "dock/settings_fields.h"
#include "dock/ui/dock_ui_calc.h"
#include "dock/ui/form_parts.h"
#include "dock/ui/message_dialog.h"
#include "dock/ui/no_wheel.h"
#include "dock/ui/set_class.h"
#include "dock/ui/slider_spin.h"
#include "dock/ui/text_resolve.h"
#include "dock/ui/theme_read.h"
#include "dock/ui/type_scale.h"
#include "dock/ui/wrap_label.h"
#include "dock/windows_calc.h"
#include "ledger/text_ids.h"
#include "sources/decor_paint.h"

namespace cordmux::dock {

namespace keys = ledger::text::keys;

namespace {

using S = config::Settings;

QString tr_key(const char *key)
{
	return ui::resolve_key(key);
}

QString tr_ref(const ledger::TextRef &ref)
{
	return ui::resolve(ref);
}

std::int64_t now_ms()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// 自分の枠に映す映像の欄の、OBS のソースかシーンの選択肢の文字列。シーンには「シーン: 」を付けて、同じ名前の欄で見分ける。
QString source_option_text(const sources::SelfSourceCandidate &choice)
{
	if (!choice.is_scene)
		return QString::fromStdString(choice.name);
	ledger::TextRef ref;
	ref.id = keys::kLookSelfFrameSceneOption;
	ref.args = {choice.name};
	return tr_ref(ref);
}

// 小数の入力欄が表示する値を、設定の刻みに合わせた値にする。per_unit は 1 を刻みで割った数 (刻み 0.05 なら 20)。
double snapped_real(double value, double lo, double hi, double per_unit)
{
	return std::clamp(std::round(value * per_unit) / per_unit, lo, hi);
}

// 整数の入力欄の値を、lo から step ごとの刻みに合わせた値にする。
int snapped_int(int value, int lo, int hi, int step)
{
	return std::clamp(lo + (value - lo + step / 2) / step * step, lo, hi);
}

// 設定の値と入力欄の表示が、入力欄の桁で同じか。
bool same_as_shown(double stored, double shown, int decimals)
{
	const double scale = std::pow(10.0, decimals);
	return std::fabs(std::round(stored * scale) - std::round(shown * scale)) < 0.5;
}

// ソースを選ぶコンボボックス。ソースの名前は利用者が付けるので、長い名前で入力欄の幅を広げない (文字数で幅を決め、入りきらない
// 項目は省略する)。コンボボックスを開く前に、ソースの候補を集め直す。
class SourceCombo : public ui::NoWheelCombo {
public:
	std::function<void()> before_popup;

	void showPopup() override
	{
		if (before_popup)
			before_popup();
		ui::NoWheelCombo::showPopup();
	}
};

// 色の名前 (#RRGGBB) を書いたラベル。最小の幅は、色の名前が全部見える幅にする (狭い幅では、ラベルと並べずに下へ移る)。
class ColorSwatch : public QLabel {
public:
	QSize minimumSizeHint() const override
	{
		const QSize hint = QLabel::minimumSizeHint();
		return QSize(fontMetrics().horizontalAdvance(QStringLiteral("#MMMMMM")) + 8, hint.height());
	}
	QSize sizeHint() const override { return minimumSizeHint(); }
};

// 選択肢の幅を文字数で決めるコンボボックス。選択肢のうちいちばん長いテキストで最小の幅が決まると、ドックの最小の幅を超える。
template <class Combo> Combo *narrow_combo(Combo *combo)
{
	combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	combo->setMinimumContentsLength(8);
	return combo;
}

} // namespace

// 表示タブの分類の間の罫線。線の上に lead_px の空きを持ち、色と太さはテーマから決める (set_rule)。
class LookRule : public QWidget {
public:
	LookRule()
	{
		setObjectName(QStringLiteral("cordmuxLookRuleStrong"));
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		set_rule(QColor(), 1, 0);
	}

	QColor color() const { return color_; }

	void set_rule(const QColor &color, int thickness, int lead_px)
	{
		color_ = color;
		thickness_ = std::max(1, thickness);
		lead_px_ = std::max(0, lead_px);
		setFixedHeight(lead_px_ + thickness_);
		update();
	}

	QSize sizeHint() const override { return QSize(0, lead_px_ + thickness_); }

protected:
	void paintEvent(QPaintEvent *) override
	{
		if (!color_.isValid())
			return;
		QPainter p(this);
		p.fillRect(QRect(0, lead_px_, width(), thickness_), color_);
	}

private:
	QColor color_;
	int thickness_ = 1;
	int lead_px_ = 0;
};

// 2 つの欄 (オフセットの X と Y) の組。幅が足りれば 1 行に first、second の順に並べ、足りなければ 2 行に分ける。最小の幅は
// 1 つの欄の幅なので、ドックの最小の幅でも横にはみ出さない (1 行の幅を最小にすると、[クラシック] の密度の最小の幅のドックで
// 表示タブが横にはみ出した)。2 行のときの高さは heightForWidth で QFormLayout に伝える。
class AxisPair : public QWidget {
public:
	AxisPair(QWidget *first, QWidget *second) : first_(first), second_(second)
	{
		first_->setParent(this);
		second_->setParent(this);
		// 縦は Fixed にしない。Fixed だと、レイアウトが高さを sizeHint (1 行) にして、2 行目が切れる。
		QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
		policy.setHeightForWidth(true);
		setSizePolicy(policy);
	}

	// gap は 1 行のときの 2 つの欄の間、row_gap は 2 行のときの行の間。
	void set_gaps(int gap, int row_gap)
	{
		gap_ = std::max(0, gap);
		row_gap_ = std::max(0, row_gap);
		updateGeometry();
		place();
	}

	QSize sizeHint() const override
	{
		return QSize(one_row_width(), row_height());
	}
	QSize minimumSizeHint() const override
	{
		return QSize(std::max(first_->minimumSizeHint().width(), second_->minimumSizeHint().width()), row_height());
	}
	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int width) const override
	{
		return width >= one_row_width() ? row_height() : 2 * row_height() + row_gap_;
	}

protected:
	void resizeEvent(QResizeEvent *event) override
	{
		QWidget::resizeEvent(event);
		place();
	}

private:
	int one_row_width() const
	{
		return first_->minimumSizeHint().width() + gap_ + second_->minimumSizeHint().width();
	}
	int row_height() const
	{
		return std::max(first_->sizeHint().height(), second_->sizeHint().height());
	}
	void place()
	{
		const int w = width();
		const int h = row_height();
		if (w >= one_row_width()) {
			// 余った幅は 2 つの欄に同じだけ配る。
			const int first_w = (w - gap_) / 2;
			first_->setGeometry(0, 0, first_w, h);
			second_->setGeometry(first_w + gap_, 0, w - first_w - gap_, h);
		} else {
			first_->setGeometry(0, 0, w, h);
			second_->setGeometry(0, h + row_gap_, w, h);
		}
	}

	QWidget *first_;
	QWidget *second_;
	int gap_ = 8;
	int row_gap_ = 4;
};

namespace {

// 項目の全文をツールチップにする (幅に入らず省略したとき、全文を読めるように)。
void tip_items(QComboBox *combo)
{
	for (int i = 0; i < combo->count(); ++i)
		combo->setItemData(i, combo->itemText(i), Qt::ToolTipRole);
}

} // namespace

LookTab::LookTab(QWidget *parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("cordmuxLookTab"));
	reflect_timer_ = new QTimer(this);
	reflect_timer_->setSingleShot(true);
	QObject::connect(reflect_timer_, &QTimer::timeout, this, [this]() {
		if (dirty_)
			reflect_now();
	});
	build();
	ui::focus_by_tab_only(this);
	loading_ = true;
	for (const Field &field : fields_)
		field.load();
	loading_ = false;
	update_dependents();
}

LookTab::~LookTab() = default;

void LookTab::register_field(QWidget *widget, const std::string &keys_joined, std::function<void()> load,
			     std::function<void()> store, std::function<bool()> editing)
{
	widget->setObjectName(QStringLiteral("cordmuxLook_") + QString::fromStdString(keys_joined));
	Field field;
	field.widget = widget;
	field.load = std::move(load);
	field.store = std::move(store);
	field.editing = std::move(editing);
	fields_.push_back(std::move(field));
}

QString LookTab::field_in_group_name(const char *label_key, const char *group_key) const
{
	ledger::TextRef name;
	name.id = keys::kLookFieldInGroupA11y;
	name.args = {tr_key(label_key).toStdString(), tr_key(group_key).toStdString()};
	return tr_ref(name);
}

void LookTab::build()
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	form_ = ui::make_form();
	// ラベルは左寄せにする。幅が足りない行はラベルが入力欄の上に移るので、右寄せのままだと、行によってラベルの位置が変わる。
	form_->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	root->addLayout(form_);

	const char *group_key = nullptr;
	// 同じ名前の入力欄 (表示、色、サイズなど) のアクセシブル名をまとまりの見出しで聞き分けるために、入力欄を集める。
	struct Named {
		QWidget *widget;
		const char *label_key;
		const char *group_key;
	};
	std::vector<Named> named;

	// 組の見出し (大見出し: 「枠」「名前ラベル」「話している人の強調」「ダミー映像」) と、組の中のまとまりの見出し (中見出し)。
	// 組の間にははっきりした罫線を引く (行の幅いっぱい)。中見出しの前には罫線を引かず、太字と上の余白で分ける
	// (apply_type_levels)。余白の値は DockStyle の見出しの段の値。
	auto add_group = [&](const char *heading_key) {
		if (!group_headings_.empty()) {
			auto *rule = new LookRule();
			form_->addRow(rule);
			strong_rules_.push_back(rule);
		}
		auto *label = new QLabel(tr_key(heading_key));
		label->setTextFormat(Qt::PlainText);
		form_->addRow(label);
		group_headings_.push_back(label);
		group_key = heading_key;
	};
	auto add_sub = [&](const char *heading_key) {
		// 大見出しのすぐ後 (間に行がない) か。フォームの最後の行が、最後に置いた大見出しかで決める (入力欄の行を置く関数
		// ごとに印を消さなくてよいように)。
		const int last_row = form_->rowCount() - 1;
		QLayoutItem *last = last_row >= 0 ? form_->itemAt(last_row, QFormLayout::SpanningRole) : nullptr;
		const bool right_after_group =
			!group_headings_.empty() && last != nullptr && last->widget() == group_headings_.back();
		auto *label = new QLabel(tr_key(heading_key));
		label->setTextFormat(Qt::PlainText);
		form_->addRow(label);
		sub_headings_.emplace_back(label, right_after_group);
		group_key = heading_key;
	};
	// 見出しのない、意味の違う部分の間の余白 (高さは apply_type_levels で決める)。
	auto add_section_gap = [&]() {
		auto *gap = new QWidget();
		gap->setObjectName(QStringLiteral("cordmuxLookSectionGap"));
		gap->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		form_->addRow(gap);
		section_gaps_.push_back(gap);
	};
	auto depend = [this](ledger::LookControl control, std::vector<QWidget *> widgets) {
		dependents_.push_back({control, std::move(widgets)});
	};

	// 選択肢を持つ入力欄。選択肢の順は、設定の列挙の値の順と同じにする。
	auto add_choice = [&](const char *label_key, const char *json_key, std::vector<const char *> option_keys,
			      std::function<int(const S &)> get, std::function<void(S &, int)> set,
			      const char *tip_key = nullptr) {
		auto *combo = narrow_combo(new ui::NoWheelCombo());
		for (std::size_t i = 0; i < option_keys.size(); ++i)
			combo->addItem(tr_key(option_keys[i]), static_cast<int>(i));
		tip_items(combo);
		if (tip_key != nullptr)
			combo->setToolTip(tr_key(tip_key));
		QLabel *label = ui::add_form_row(form_, tr_key(label_key), combo);
		register_field(
			combo, json_key, [this, combo, get]() { ui::set_combo_data(combo, get(edited_)); },
			[this, combo, set]() { set(edited_, combo->currentData().toInt()); },
			[combo]() { return combo->hasFocus() || (combo->view() != nullptr && combo->view()->isVisible()); });
		QObject::connect(combo, &QComboBox::currentIndexChanged, this, [this](int) { on_field_changed(); });
		named.push_back({combo, label_key, group_key});
		return std::pair<QLabel *, QComboBox *>{label, combo};
	};

	auto add_check = [&](const char *text_key, const char *json_key, bool S::*member, const char *tip_key = nullptr) {
		auto *check = new QCheckBox(tr_key(text_key));
		if (tip_key != nullptr)
			check->setToolTip(tr_key(tip_key));
		form_->addRow(check);
		register_field(
			check, json_key, [this, check, member]() { check->setChecked(edited_.*member); },
			[this, check, member]() { edited_.*member = check->isChecked(); },
			[check]() { return check->hasFocus(); });
		QObject::connect(check, &QCheckBox::toggled, this, [this](bool) { on_field_changed(); });
		named.push_back({check, text_key, group_key});
		return check;
	};

	// 整数の入力欄 (つまみと数の入力欄)。step を 1 より大きくしたときは、入力した値を刻みに合う値にそろえる (SliderSpin)。
	// つまみのダブルクリックで、その設定の既定値 (config::Settings の既定値) に戻す。
	auto add_int = [&](const char *label_key, const char *json_key, int S::*member, int lo, int hi, int step,
			   const char *unit_key, const char *tip_key = nullptr) {
		auto *input = ui::SliderSpin::integer(lo, hi, step, unit_key != nullptr ? tr_key(unit_key) : QString());
		input->set_default_value(static_cast<double>(S{}.*member));
		if (tip_key != nullptr)
			input->spin()->setToolTip(tr_key(tip_key));
		QLabel *label = ui::add_form_row(form_, tr_key(label_key), input);
		register_field(
			input, json_key, [this, input, member]() { input->set_value(edited_.*member); },
			[this, input, member, lo, hi, step]() {
				edited_.*member = snapped_int(static_cast<int>(std::lround(input->value())), lo, hi, step);
			},
			[input]() { return input->editing(); });
		input->set_on_changed([this]() { on_field_changed(); });
		named.push_back({input, label_key, group_key});
		return std::pair<QLabel *, QWidget *>{label, input};
	};

	// 小数の入力欄。設定の値が入力欄の桁で表示と同じなら書き換えない (別の入力欄を変えたときに、設定を手で書いた桁の多い値を
	// 丸めて書き換えないため)。
	auto add_real = [&](const char *label_key, const char *json_key, double S::*member, double lo, double hi,
			    int decimals, double step, const char *unit_key, const char *tip_key = nullptr) {
		auto *input = ui::SliderSpin::real(lo, hi, decimals, step, unit_key != nullptr ? tr_key(unit_key) : QString());
		input->set_default_value(S{}.*member);
		if (tip_key != nullptr)
			input->spin()->setToolTip(tr_key(tip_key));
		QLabel *label = ui::add_form_row(form_, tr_key(label_key), input);
		const double per_unit = std::round(1.0 / step);
		register_field(
			input, json_key, [this, input, member]() { input->set_value(edited_.*member); },
			[this, input, member, lo, hi, decimals, per_unit]() {
				if (!same_as_shown(edited_.*member, input->value(), decimals))
					edited_.*member = snapped_real(input->value(), lo, hi, per_unit);
			},
			[input]() { return input->editing(); });
		input->set_on_changed([this]() { on_field_changed(); });
		named.push_back({input, label_key, group_key});
		return std::pair<QLabel *, QWidget *>{label, input};
	};

	// 色の入力欄 (色の名前を書いたラベルと [色を選択])。入力欄の objectName は、押して値を選ぶボタンに付ける。
	auto add_color = [&](const char *label_key, const char *json_key, std::string S::*member) {
		auto *row = new QWidget();
		auto *h = new QHBoxLayout(row);
		h->setContentsMargins(0, 0, 0, 0);
		auto *swatch = new ColorSwatch();
		auto *pick = new QPushButton(tr_key(keys::kLookSelectColorButton));
		h->addWidget(swatch, 1);
		h->addWidget(pick);
		QLabel *label = ui::add_form_row(form_, tr_key(label_key), row, pick);
		ledger::TextRef name;
		name.id = keys::kLookSelectColorForA11y;
		name.args = {tr_key(group_key).toStdString()};
		pick->setAccessibleName(tr_ref(name));
		register_field(
			pick, json_key, [this, swatch, member]() { update_color_swatch(swatch, edited_.*member); }, []() {},
			[pick]() { return pick->hasFocus(); });
		swatches_.push_back({swatch, member});
		// ダイアログのタイトルは入力欄の名前にする (色の欄が複数あるので、まとまりの見出しを添える)。
		const QString title = field_in_group_name(label_key, group_key);
		QObject::connect(pick, &QPushButton::clicked, this, [this, swatch, member, title]() {
			choose_color(member, swatch, title);
		});
		return std::pair<QLabel *, QWidget *>{label, row};
	};

	// ---- 枠 ----
	add_group(keys::kLookHeadingSlotsHeading);
	add_choice(
		keys::kLookSlotAssignmentLabel, "assign_mode",
		{keys::kLookSlotAssignmentManualOption, keys::kLookSlotAssignmentAutoOption},
		[](const S &s) { return static_cast<int>(s.assign_mode); },
		[](S &s, int i) { s.assign_mode = static_cast<config::AssignMode>(i); }, keys::kLookSlotAssignmentTip);
	add_choice(
		keys::kLookSlotOrderLabel, "slot_order",
		{keys::kLookSlotOrderNameOption, keys::kLookSlotOrderAddedOption},
		[](const S &s) { return static_cast<int>(s.slot_order); },
		[](S &s, int i) { s.slot_order = static_cast<config::SlotOrder>(i); });
	add_int(keys::kLookSlotSpacingLabel, "tile_gap_px", &S::tile_gap_px, config::kTileGapMinPx, config::kTileGapMaxPx, 1,
		keys::kSettingsUnitPxLabel);

	// 自分の枠に映す映像。先頭の選択肢は、メインアカウントがあるときは「Discordの画面共有」、なければ「(なし)」。
	{
		auto *combo = narrow_combo(new SourceCombo());
		frame_combo_ = combo;
		combo->setToolTip(tr_key(keys::kLookSelfFrameSourceTip));
		combo->before_popup = [this]() { refresh_sources(); };
		ui::add_form_row(form_, tr_key(keys::kLookSelfFrameSourceLabel), combo);
		register_field(
			combo, "self_frame_source_uuid+self_frame_source_name",
			[this, combo]() {
				fill_source_combo(combo, frame_choices_, edited_.self_frame_source_uuid,
						  edited_.self_frame_source_name, true);
			},
			[this, combo]() {
				const int index = combo->currentIndex();
				if (index <= 0) {
					edited_.self_frame_source_uuid.clear();
					edited_.self_frame_source_name.clear();
					return;
				}
				edited_.self_frame_source_uuid = combo->itemData(index, Qt::UserRole).toString().toStdString();
				edited_.self_frame_source_name =
					combo->itemData(index, Qt::UserRole + 1).toString().toStdString();
			},
			[combo]() { return combo->hasFocus() || (combo->view() != nullptr && combo->view()->isVisible()); });
		QObject::connect(combo, &QComboBox::currentIndexChanged, this, [this](int) { on_field_changed(); });
	}
	// 自分の枠に映す映像を、並び順によらず先頭 (左上) に置くか。左の列に名前を置かず、チェックボックスの横に語を置く
	// (ほかのチェックボックスの行と同じ)。映像が「(なし)」の間は押せない (update_self_first_enabled)。
	self_first_check_ =
		add_check(keys::kLookSelfSlotFirstCheck, "self_slot_first", &S::self_slot_first, keys::kLookSelfSlotFirstTip);

	// 背景。映像を描かない所 (空いた枠、枠の間隔、映像の縦横の比が枠と違うときの余り) の色。[表示] を切ると、色と不透明度を
	// 押せなくする。見出しは名前ラベルの背景と同じなので、読み上げの名前と色のダイアログのタイトルに添えるまとまりの名前は
	// 「枠の背景」にする (「表示 (枠の背景)」。名前ラベルの側は「表示 (背景)」のまま)。
	add_sub(keys::kLookHeadingBackgroundHeading);
	group_key = keys::kLookSlotBackgroundGroupA11y;
	add_check(keys::kLookShowCheck, "slot_background_show", &S::slot_background_show);
	const auto slot_bg_color = add_color(keys::kLookColorLabel, "slot_background_color", &S::slot_background_color);
	const auto slot_bg_opacity =
		add_int(keys::kLookOpacityLabel, "slot_background_opacity_pct", &S::slot_background_opacity_pct,
			config::kSlotBackgroundOpacityMinPct, config::kSlotBackgroundOpacityMaxPct, 1, keys::kSettingsUnitPercentLabel);
	depend(ledger::LookControl::SlotBackgroundColor, {slot_bg_color.first, slot_bg_color.second});
	depend(ledger::LookControl::SlotBackgroundOpacity, {slot_bg_opacity.first, slot_bg_opacity.second});

	// ---- 名前ラベル ----
	add_group(keys::kLookHeadingLabelsHeading);

	// 名前。[表示] を切ると、名前のテキストにだけ反映される入力欄を押せなくする (隠さない)。
	add_sub(keys::kLookHeadingNameHeading);
	add_check(keys::kLookShowCheck, "label_show_name", &S::label_show_name);

	// フォント。ページの中にフォントの一覧を置き、OS のフォントのダイアログは開かない。そのダイアログには大きさの入力欄が
	// 必ずあり、そこで選んだ大きさは反映されないためである。斜体は選ばせない。フォントの設定が空のとき (既定) は、配信の
	// 画面に実際に描くフォント (sources/decor_paint.h の kDefaultLabelFontFamilies のうち、その PC にある最初のもの。
	// default_label_font_family) を表示する。設定は空のままにし、利用者が選んだときだけ書く (既定を 2 か所で決めない)。
	// 手で入力できる形にしない。入力できる形だと、入りきらない名前の末尾だけが見え (カーソルが末尾にあるため)、ほかのコンボボックスと
	// 見え方が揃わない。一覧を開いて文字を打てば、頭の文字で探せる。
	font_family_ = narrow_combo(new ui::NoWheelFontCombo());
	font_family_->setEditable(false);
	QLabel *family_label = ui::add_form_row(form_, tr_key(keys::kLookFontLabel), font_family_);
	register_field(
		font_family_, "label_font_family",
		[this]() {
			const QString family = edited_.label_font_family.empty()
						       ? sources::default_label_font_family()
						       : QString::fromStdString(edited_.label_font_family);
			font_family_->setCurrentFont(QFont(family));
		},
		[]() {}, [this]() { return font_family_->hasFocus() || font_family_->view()->isVisible(); });
	QObject::connect(font_family_, &QFontComboBox::currentFontChanged, this, [this](const QFont &font) {
		if (loading_)
			return;
		edited_.label_font_family = font.family().toStdString();
		// 太さはフォントごとに選択肢が違うので、新しいフォントの中で現在の値にいちばん近いものを選ぶ。
		edited_.label_font_weight = fill_font_weights(edited_.label_font_weight, true);
		on_field_changed();
	});

	// 太さ。選んでいるフォントが持つ太さを、フォントが付けている名前で並べる (OBS のテキストソースのフォントのダイアログも、
	// フォントが持つスタイルの名前から選ばせる)。値は label_font_weight (数) に書く。選択肢は、フォントの入力欄の値から作る
	// ので、フォントの入力欄より後ろに登録する (値の読み込みは登録の順)。
	font_weight_ = narrow_combo(new ui::NoWheelCombo());
	QLabel *weight_label = ui::add_form_row(form_, tr_key(keys::kLookFontWeightLabel), font_weight_);
	register_field(
		font_weight_, "label_font_weight", [this]() { fill_font_weights(edited_.label_font_weight, false); }, []() {},
		[this]() { return font_weight_->hasFocus() || font_weight_->view()->isVisible(); });
	QObject::connect(font_weight_, &QComboBox::currentIndexChanged, this, [this](int index) {
		if (loading_ || index < 0)
			return;
		edited_.label_font_weight = font_weight_->itemData(index).toInt();
		on_field_changed();
	});
	named.push_back({font_weight_, keys::kLookFontWeightLabel, group_key});

	// 大きさ。配信の画面の高さに対する割合の入力欄 1 つで、単位の「%」は入力欄の後ろに付ける。
	const auto size_row = add_real(keys::kLookFontSizeLabel, "label_em_ratio_pct", &S::label_em_ratio_pct,
				       config::kLabelEmRatioMinPct, config::kLabelEmRatioMaxPct, 1, 0.1, keys::kSettingsUnitPercentLabel);
	font_size_ = static_cast<ui::SliderSpin *>(size_row.second);
	const auto text_color = add_color(keys::kLookColorLabel, "label_text_color", &S::label_text_color);
	depend(ledger::LookControl::NameFont, {family_label, font_family_});
	depend(ledger::LookControl::NameWeight, {weight_label, font_weight_});
	depend(ledger::LookControl::NameSize, {size_row.first, size_row.second});
	depend(ledger::LookControl::NameColor, {text_color.first, text_color.second});

	// 輪郭。名前の文字だけに付くので、名前を表示しないときは押せなくする。輪郭を表示しないときは、色とサイズを押せなくする。
	add_sub(keys::kLookHeadingOutlineHeading);
	QCheckBox *outline_show = add_check(keys::kLookShowCheck, "label_show_outline", &S::label_show_outline);
	const auto outline_color = add_color(keys::kLookColorLabel, "label_outline_color", &S::label_outline_color);
	const auto outline_size = add_real(keys::kLookSizeLabel, "label_outline_ratio", &S::label_outline_ratio,
					   config::kLabelOutlineRatioMin, config::kLabelOutlineRatioMax, 2, 0.01,
					   keys::kLookUnitTimesLabel, keys::kLookRatioOfFontSizeTip);
	depend(ledger::LookControl::OutlineShow, {outline_show});
	depend(ledger::LookControl::OutlineColor, {outline_color.first, outline_color.second});
	depend(ledger::LookControl::OutlineSize, {outline_size.first, outline_size.second});

	// アバター。
	add_sub(keys::kLookHeadingAvatarHeading);
	add_check(keys::kLookShowCheck, "label_show_avatar", &S::label_show_avatar);
	const auto avatar_size = add_real(keys::kLookSizeLabel, "label_avatar_ratio", &S::label_avatar_ratio,
					  config::kLabelAvatarRatioMin, config::kLabelAvatarRatioMax, 2, 0.01,
					  keys::kLookUnitTimesLabel, keys::kLookRatioOfFontSizeTip);
	// 形。画面共有している参加者と、していない参加者の並びの両方に適用する。
	const auto avatar_shape = add_choice(
		keys::kLookAvatarShapeLabel, "label_avatar_shape",
		{keys::kLookShapeCircleOption, keys::kLookShapeRoundedOption, keys::kLookShapeSquareOption},
		[](const S &s) { return static_cast<int>(s.label_avatar_shape); },
		[](S &s, int i) { s.label_avatar_shape = static_cast<config::AvatarShape>(i); });
	depend(ledger::LookControl::AvatarSize, {avatar_size.first, avatar_size.second});
	depend(ledger::LookControl::AvatarShape, {avatar_shape.first, avatar_shape.second});

	// 背景。名前もアバターも表示しないときは背景を描かないので、押せなくする。
	add_sub(keys::kLookHeadingBackgroundHeading);
	QCheckBox *plate_show = add_check(keys::kLookShowCheck, "label_show_plate", &S::label_show_plate);
	const auto plate_color = add_color(keys::kLookColorLabel, "label_plate_color", &S::label_plate_color);
	const auto plate_opacity = add_int(keys::kLookOpacityLabel, "label_plate_opacity_pct", &S::label_plate_opacity_pct,
					   config::kLabelPlateOpacityMinPct, config::kLabelPlateOpacityMaxPct, 1,
					   keys::kSettingsUnitPercentLabel);
	depend(ledger::LookControl::PlateShow, {plate_show});
	depend(ledger::LookControl::PlateColor, {plate_color.first, plate_color.second});
	depend(ledger::LookControl::PlateOpacity, {plate_opacity.first, plate_opacity.second});

	// 名前ラベル全体。
	add_sub(keys::kLookHeadingWholeHeading);
	add_real(keys::kLookMarginLabel, "label_margin_pct", &S::label_margin_pct, config::kLabelMarginMinPct,
		 config::kLabelMarginMaxPct, 1, 0.1, keys::kSettingsUnitPercentLabel, keys::kLookRatioOfCanvasHeightTip);
	add_int(keys::kLookOpacityLabel, "label_opacity_pct", &S::label_opacity_pct, config::kLabelOpacityMinPct,
		config::kLabelOpacityMaxPct, 1, keys::kSettingsUnitPercentLabel);

	// 画面共有している参加者。上から [表示]、[1人のときは非表示]、[位置]、[アバターの位置] ([1人のときは非表示] は
	// [表示] と意味が近いので、そのすぐ下に置く)。[表示] を外すと、ほかの 3 つを押せなくする (画面共有していない参加者の
	// [表示] と同じ形)。位置は全員一律で、各人の枠の同じ隅に置く。
	add_sub(keys::kLookHeadingSharingHeading);
	add_check(keys::kLookShowCheck, "label_show_in_slot", &S::label_show_in_slot, keys::kLookSlotLabelShowTip);
	QCheckBox *hide_when_single = add_check(keys::kLookHideWhenSingleCheck, "label_hide_when_single",
						&S::label_hide_when_single, keys::kLookHideWhenSingleTip);
	const auto corner = add_choice(
		keys::kLookNameLabelPositionLabel, "label_corner",
		{keys::kLookPositionTopLeftOption, keys::kLookPositionTopRightOption, keys::kLookPositionBottomLeftOption,
		 keys::kLookPositionBottomRightOption},
		[](const S &s) { return static_cast<int>(s.label_corner); },
		[](S &s, int i) { s.label_corner = static_cast<config::LabelCorner>(i); }, keys::kLookNameLabelPositionTip);
	const auto avatar_side = add_choice(
		keys::kLookAvatarPositionLabel, "label_avatar_side", {keys::kLookSideLeftOption, keys::kLookSideRightOption},
		[](const S &s) { return static_cast<int>(s.label_avatar_side); },
		[](S &s, int i) { s.label_avatar_side = static_cast<config::AvatarSide>(i); }, keys::kLookAvatarSideTip);
	depend(ledger::LookControl::SlotLabelHideWhenSingle, {hide_when_single});
	depend(ledger::LookControl::SlotLabelCorner, {corner.first, corner.second});
	depend(ledger::LookControl::SlotLabelAvatarSide, {avatar_side.first, avatar_side.second});

	// 画面共有していない参加者。並びは、配置に合うものだけを選べる (中央上と中央下は横並び、左中央と右中央は縦並びだけ)。
	// 配置を変えたら、合う並びに修正する。
	add_sub(keys::kLookHeadingNotSharingHeading);
	add_check(keys::kLookShowCheck, "roster_show", &S::roster_show);
	const auto anchor = add_choice(
		keys::kLookAnchorLabel, "roster_anchor",
		{keys::kLookPositionTopLeftOption, keys::kLookPositionTopCenterOption, keys::kLookPositionTopRightOption,
		 keys::kLookPositionMiddleLeftOption, keys::kLookPositionMiddleRightOption,
		 keys::kLookPositionBottomLeftOption, keys::kLookPositionBottomCenterOption,
		 keys::kLookPositionBottomRightOption},
		[](const S &s) { return static_cast<int>(s.roster_anchor); },
		[](S &s, int i) {
			s.roster_anchor = static_cast<config::RosterAnchor>(i);
			s.roster_direction = config::roster_direction_for(s.roster_anchor, s.roster_direction);
		});
	// オフセット。配置と余白で決まる並びの位置からずらす量 (キャンバスのピクセル。右と下が正)。横 (X) と縦 (Y) の 2 つの欄を
	// 1 行に、X、Y の順に置く (OBS の変換の編集のダイアログの「位置」と同じ並び)。1 行に入らない幅では 2 行に分ける (AxisPair)。
	// 欄は表示タブのほかの数の欄と同じ SliderSpin で、範囲が広くつまみの段数が上限を超えるので、つまみを置かず数の入力欄だけに
	// なる。読み上げの名前は「X (オフセット)」の形にする。
	QLabel *offset_label = nullptr;
	QWidget *offset_row = nullptr;
	{
		auto add_axis = [&](const char *axis_key, const char *json_key, int S::*member) {
			auto *axis = new QWidget();
			auto *axis_layout = new QHBoxLayout(axis);
			axis_layout->setContentsMargins(0, 0, 0, 0);
			offset_axis_layouts_.push_back(axis_layout);
			auto *caption = new QLabel(tr_key(axis_key));
			caption->setTextFormat(Qt::PlainText);
			auto *input = ui::SliderSpin::integer(config::kRosterOffsetMinPx, config::kRosterOffsetMaxPx, 1,
							      tr_key(keys::kSettingsUnitPxLabel));
			input->spin()->setToolTip(tr_key(keys::kLookRosterOffsetTip));
			caption->setBuddy(input->spin());
			const QString name = field_in_group_name(axis_key, keys::kLookOffsetLabel);
			input->setAccessibleName(name);
			input->spin()->setAccessibleName(name);
			register_field(
				input, json_key, [this, input, member]() { input->set_value(edited_.*member); },
				[this, input, member]() {
					const int v = static_cast<int>(std::lround(input->value()));
					edited_.*member = std::clamp(v, config::kRosterOffsetMinPx, config::kRosterOffsetMaxPx);
				},
				[input]() { return input->editing(); });
			input->set_on_changed([this]() { on_field_changed(); });
			axis_layout->addWidget(caption);
			axis_layout->addWidget(input, 1);
			return std::pair<QWidget *, ui::SliderSpin *>{axis, input};
		};
		const auto offset_x = add_axis(keys::kLookOffsetXLabel, "roster_offset_x_px", &S::roster_offset_x_px);
		const auto offset_y = add_axis(keys::kLookOffsetYLabel, "roster_offset_y_px", &S::roster_offset_y_px);
		offset_pair_ = new AxisPair(offset_x.first, offset_y.first);
		offset_row = offset_pair_;
		offset_label = ui::add_form_row(form_, tr_key(keys::kLookOffsetLabel), offset_row, offset_x.second->spin());
	}
	const auto direction = add_choice(
		keys::kLookDirectionLabel, "roster_direction", {keys::kLookDirectionRowOption, keys::kLookDirectionColumnOption},
		[](const S &s) { return static_cast<int>(s.roster_direction); },
		[](S &s, int i) {
			s.roster_direction = config::roster_direction_for(s.roster_anchor, static_cast<config::RosterDirection>(i));
		});
	roster_direction_ = direction.second;
	const auto roster_gap = add_real(keys::kLookGapLabel, "roster_gap_ratio", &S::roster_gap_ratio,
					 config::kRosterGapRatioMin, config::kRosterGapRatioMax, 2, 0.05,
					 keys::kLookUnitTimesLabel, keys::kLookRatioOfFontSizeTip);
	// 名前の最大幅。長すぎる名前が、キャンバスの幅を取らないようにする。名前を表示しないときは押せなくする。
	const auto roster_name_width = add_real(keys::kLookNameMaxWidthLabel, "roster_name_max_em", &S::roster_name_max_em,
						config::kRosterNameMaxEmMin, config::kRosterNameMaxEmMax, 1, 0.5,
						keys::kLookUnitTimesLabel, keys::kLookNameMaxWidthTip);
	const auto roster_side = add_choice(
		keys::kLookAvatarPositionLabel, "roster_avatar_side", {keys::kLookSideLeftOption, keys::kLookSideRightOption},
		[](const S &s) { return static_cast<int>(s.roster_avatar_side); },
		[](S &s, int i) { s.roster_avatar_side = static_cast<config::AvatarSide>(i); }, keys::kLookRosterAvatarSideTip);
	QCheckBox *show_bots = add_check(keys::kLookShowBotsCheck, "roster_show_bots", &S::roster_show_bots);
	depend(ledger::LookControl::RosterAnchor, {anchor.first, anchor.second});
	depend(ledger::LookControl::RosterOffset, {offset_label, offset_row});
	depend(ledger::LookControl::RosterDirection, {direction.first, direction.second});
	depend(ledger::LookControl::RosterGap, {roster_gap.first, roster_gap.second});
	depend(ledger::LookControl::RosterNameWidth, {roster_name_width.first, roster_name_width.second});
	depend(ledger::LookControl::RosterAvatarSide, {roster_side.first, roster_side.second});
	depend(ledger::LookControl::RosterShowBots, {show_bots});

	// ---- 話している人の強調 ----
	add_group(keys::kLookHeadingSpeakingHeading);
	// [光] を切ると、光の形を決める入力欄を押せなくする。時間の 3 つは、光のフェードと、話していない人を暗くする動きの両方に
	// 適用されるので、どちらも動作しないときだけ押せなくする。
	add_check(keys::kLookGlowCheck, "label_show_speaking", &S::label_show_speaking);
	auto *targets = new QWidget();
	auto *targets_layout = new QHBoxLayout(targets);
	targets_layout->setContentsMargins(0, 0, 0, 0);
	auto make_target = [&](const char *text_key, const char *json_key, bool S::*member) {
		auto *check = new QCheckBox(tr_key(text_key));
		register_field(
			check, json_key, [this, check, member]() { check->setChecked(edited_.*member); },
			[this, check, member]() { edited_.*member = check->isChecked(); }, [check]() { return check->hasFocus(); });
		QObject::connect(check, &QCheckBox::toggled, this, [this](bool) { on_field_changed(); });
		named.push_back({check, text_key, group_key});
		targets_layout->addWidget(check);
		return check;
	};
	QCheckBox *target_frame = make_target(keys::kLookGlowTargetFrameCheck, "label_glow_frame", &S::label_glow_frame);
	make_target(keys::kLookGlowTargetAvatarCheck, "label_glow_avatar", &S::label_glow_avatar);
	make_target(keys::kLookGlowTargetNameCheck, "label_glow_name", &S::label_glow_name);
	targets_layout->addStretch(1);
	QLabel *targets_label = ui::add_form_row(form_, tr_key(keys::kLookGlowTargetsLabel), targets, target_frame);
	const auto glow_color = add_color(keys::kLookColorLabel, "label_border_color", &S::label_border_color);
	const auto glow_thickness = add_real(keys::kLookGlowThicknessLabel, "label_glow_spread_ratio", &S::label_glow_spread_ratio,
					     config::kLabelGlowSpreadRatioMin, config::kLabelGlowSpreadRatioMax, 2, 0.01,
					     keys::kLookUnitTimesLabel, keys::kLookRatioOfFontSizeTip);
	const auto glow_blur = add_real(keys::kLookGlowBlurLabel, "label_glow_blur_ratio", &S::label_glow_blur_ratio,
					config::kLabelGlowBlurRatioMin, config::kLabelGlowBlurRatioMax, 2, 0.01,
					keys::kLookUnitTimesLabel, keys::kLookRatioOfFontSizeTip);
	depend(ledger::LookControl::GlowTargets, {targets_label, targets});
	depend(ledger::LookControl::GlowColor, {glow_color.first, glow_color.second});
	depend(ledger::LookControl::GlowThickness, {glow_thickness.first, glow_thickness.second});
	depend(ledger::LookControl::GlowBlur, {glow_blur.first, glow_blur.second});
	add_int(keys::kLookDimLabel, "label_dim_pct", &S::label_dim_pct, config::kLabelDimMinPct, config::kLabelDimMaxPct,
		config::kLabelDimStepPct, keys::kSettingsUnitPercentLabel, keys::kLookDimTip);
	const auto fade_in = add_int(keys::kLookFadeInLabel, "label_border_rise_ms", &S::label_border_rise_ms,
				     config::kLabelBorderTimingMinMs, config::kLabelBorderTimingMaxMs, 1, keys::kSettingsUnitMsLabel);
	const auto hold = add_int(keys::kLookHoldLabel, "label_border_hold_ms", &S::label_border_hold_ms,
				  config::kLabelBorderTimingMinMs, config::kLabelBorderTimingMaxMs, 1, keys::kSettingsUnitMsLabel,
				  keys::kLookHoldTip);
	const auto fade_out = add_int(keys::kLookFadeOutLabel, "label_border_fade_ms", &S::label_border_fade_ms,
				      config::kLabelBorderTimingMinMs, config::kLabelBorderTimingMaxMs, 1,
				      keys::kSettingsUnitMsLabel);
	depend(ledger::LookControl::TimingFadeIn, {fade_in.first, fade_in.second});
	depend(ledger::LookControl::TimingHold, {hold.first, hold.second});
	depend(ledger::LookControl::TimingFadeOut, {fade_out.first, fade_out.second});

	// 自分の枠の音声ソースとしきい値。光り方 (上の入力欄) とは別の、光る条件なので、意味の違う部分の間の余白で分ける
	// (中見出しと同じ上の余白。罫線は引かない)。
	add_section_gap();
	{
		auto *combo = narrow_combo(new SourceCombo());
		speaking_combo_ = combo;
		combo->setToolTip(tr_key(keys::kLookSelfSourceTip));
		combo->before_popup = [this]() { refresh_sources(); };
		ui::add_form_row(form_, tr_key(keys::kLookSelfSourceLabel), combo);
		register_field(
			combo, "self_speaking_source_uuid+self_speaking_source_name",
			[this, combo]() {
				fill_source_combo(combo, speaking_choices_, edited_.self_speaking_source_uuid,
						  edited_.self_speaking_source_name, false);
			},
			[this, combo]() {
				const int index = combo->currentIndex();
				if (index <= 0) {
					edited_.self_speaking_source_uuid.clear();
					edited_.self_speaking_source_name.clear();
					return;
				}
				edited_.self_speaking_source_uuid = combo->itemData(index, Qt::UserRole).toString().toStdString();
				edited_.self_speaking_source_name =
					combo->itemData(index, Qt::UserRole + 1).toString().toStdString();
			},
			[combo]() { return combo->hasFocus() || (combo->view() != nullptr && combo->view()->isVisible()); });
		QObject::connect(combo, &QComboBox::currentIndexChanged, this, [this](int) { on_field_changed(); });
		named.push_back({combo, keys::kLookSelfSourceLabel, group_key});
	}
	const auto threshold = add_int(keys::kLookSelfThresholdLabel, "self_speaking_threshold_db",
				       &S::self_speaking_threshold_db, config::kSelfSpeakingThresholdMinDb,
				       config::kSelfSpeakingThresholdMaxDb, 1, keys::kLookUnitDbLabel, keys::kLookSelfThresholdTip);
	depend(ledger::LookControl::SelfThreshold, {threshold.first, threshold.second});

	// ---- ダミー映像 ----
	// 表示を確認するための、めったに使わない操作なので、表示タブのいちばん下に置く。設定の値ではなく、ActionId::ToggleDummy で
	// 切り替える。
	add_group(keys::kLookHeadingDummyHeading);
	// チェックのテキストは、ほかの組 (名前、アバターなど) と同じく [表示] にする。何を表示するかは見出しで分かる。
	// アクセシブル名は、見出しを読まなくても分かる「ダミー映像を表示」にする (実機のテストのツールもこの名前で探す)。
	// オンの間は、相手の画面共有も声も配信に出ないことを、押す前に伝える。
	dummy_check_ = new QCheckBox(tr_key(keys::kLookShowCheck));
	dummy_check_->setAccessibleName(tr_key(keys::kLookShowPlaceholdersCheck));
	dummy_check_->setObjectName(QStringLiteral("cordmuxLook_dummy_on"));
	form_->addRow(dummy_check_);
	dummy_note_ = ui::wrap_label(tr_key(keys::kLookShowPlaceholdersDescText));
	dummy_note_->setObjectName(QStringLiteral("cordmuxLookDummyNote"));
	dummy_check_->setAccessibleDescription(tr_key(keys::kLookShowPlaceholdersDescText));
	// 説明の文は入れ物に入れ、字下げと下の余白を入れ物に持たせる (apply_type_levels)。
	dummy_note_box_ = ui::note_box(dummy_note_);
	form_->addRow(dummy_note_box_);
	QObject::connect(dummy_check_, &QCheckBox::toggled, this, [this](bool on) {
		if (loading_ || !on_action_)
			return;
		ledger::ActionArgs args = view_.dummy_toggle.args;
		args.on = on;
		on_action_(ledger::ActionId::ToggleDummy, args);
	});

	// 同じ名前の入力欄は、まとまりの見出しを添えたアクセシブル名にする (「表示 (名前)」のように)。
	std::map<QString, int> same_label;
	for (const Named &n : named)
		++same_label[tr_key(n.label_key)];
	for (const Named &n : named) {
		if (same_label[tr_key(n.label_key)] <= 1)
			continue;
		const QString name = field_in_group_name(n.label_key, n.group_key);
		n.widget->setAccessibleName(name);
		if (auto *input = dynamic_cast<ui::SliderSpin *>(n.widget)) {
			input->spin()->setAccessibleName(name);
			if (input->slider() != nullptr)
				input->slider()->setAccessibleName(name);
		}
	}
	// つまみと数の入力欄の objectName とアクセシブル名 (名前が同じ入力欄を除く)。
	for (const Field &field : fields_) {
		auto *input = dynamic_cast<ui::SliderSpin *>(field.widget);
		if (input == nullptr)
			continue;
		input->spin()->setObjectName(field.widget->objectName() + QStringLiteral("_spin"));
		if (input->slider() != nullptr)
			input->slider()->setObjectName(field.widget->objectName() + QStringLiteral("_slider"));
	}
	for (const Named &n : named) {
		auto *input = dynamic_cast<ui::SliderSpin *>(n.widget);
		if (input == nullptr || !input->spin()->accessibleName().isEmpty())
			continue;
		input->spin()->setAccessibleName(tr_key(n.label_key));
		if (input->slider() != nullptr)
			input->slider()->setAccessibleName(tr_key(n.label_key));
	}
}

void LookTab::set_look(const DockLook &look)
{
	look_ = look;
	apply_type_levels();
	for (const auto &[swatch, member] : swatches_)
		update_color_swatch(swatch, edited_.*member);
}

void LookTab::apply_type_levels()
{
	const ui::DockMetrics &m = look_.metrics;
	const int small = m.small_px;
	// フォームの行の間は small なので、各部分の余白は、決めた余白から行の間を引いた残りにする (DockStyle の見出しの段のコメント)。
	const auto less_small = [small](int px) { return std::max(0, px - small); };
	// 組の間ははっきりした罫線 (前の行の下から罫線まで group_rule_above、罫線から大見出しまで group_heading_above)。色は背景から
	// 本文の色の方へ寄せて作る。大見出しは倍率を掛けた太字、中見出しは本文の大きさの太字で、どちらも本文の色 (補足の色に
	// しない)。
	const ui::calc::Rgb ground = ui::to_rgb(look_.theme.base_bg.isValid() ? look_.theme.base_bg : QColor(Qt::black));
	const ui::calc::Rgb text = ui::to_rgb(look_.theme.text.isValid() ? look_.theme.text : QColor(Qt::white));
	const ui::calc::Rgb strong = ui::calc::rule_color(ground, text, ui::calc::kStrongRuleContrastMin);
	const auto to_color = [](ui::calc::Rgb c) { return QColor(c.r, c.g, c.b); };
	for (LookRule *rule : strong_rules_)
		rule->set_rule(to_color(strong), 1, less_small(m.group_rule_above_px));
	bool first = true;
	for (QLabel *label : group_headings_) {
		ui::apply_type(label, ui::TypeLevel::GroupHeading, look_.base_pt, look_.style.group_heading_scale);
		label->setContentsMargins(0, first ? 0 : less_small(m.group_heading_above_px), 0,
					  less_small(m.group_heading_below_px));
		first = false;
	}
	for (const auto &[label, after_group] : sub_headings_) {
		ui::apply_type(label, ui::TypeLevel::Heading, look_.base_pt, 1.0);
		// 大見出しのすぐ後の中見出しは、大見出しの下の余白だけを空ける (2 つの見出しの間を広げると、大見出しが中身から離れる)。
		label->setContentsMargins(0, after_group ? 0 : less_small(m.sub_heading_above_px), 0,
					  less_small(m.sub_heading_below_px));
	}
	// 見出しのない切れ目は、前の行と次の行の間を section_gap にする (前後の行の間の 2 つ分を引く)。
	for (QWidget *gap : section_gaps_)
		gap->setFixedHeight(std::max(0, m.section_gap_px - 2 * small));
	// 説明の文: 補足の色 (背景との比が 4.5:1 に届かないテーマでは本文の色)、説明する [表示] の文字の頭にそろえ、下に note_below。
	if (dummy_note_ != nullptr && dummy_check_ != nullptr) {
		ui::set_class(dummy_note_, ui::muted_ok(look_.theme, look_.theme.base_bg) ? QStringLiteral("text-muted") : QString());
		QStyleOptionButton option;
		option.initFrom(dummy_check_);
		option.text = dummy_check_->text();
		const QRect contents = dummy_check_->style()->subElementRect(QStyle::SE_CheckBoxContents, &option, dummy_check_);
		ui::set_note_box_margins(dummy_note_box_, contents.left(), less_small(m.note_below_px));
	}
	if (form_ != nullptr) {
		form_->setVerticalSpacing(small);
		form_->setHorizontalSpacing(look_.metrics.large_px);
	}
	// オフセットの行: 「X」と欄の間は小、X の欄と「Y」の間は大 (フォームのラベルと入力欄の間と同じ)。2 行に分けたときの行の
	// 間は、フォームの行の間と同じ小。
	for (QHBoxLayout *axis_layout : offset_axis_layouts_)
		axis_layout->setSpacing(small);
	if (offset_pair_ != nullptr)
		offset_pair_->set_gaps(look_.metrics.large_px, small);
}

void LookTab::set_canvas_size(int width, int height)
{
	if (width == canvas_width_ && height == canvas_height_)
		return;
	canvas_width_ = width;
	canvas_height_ = height;
	update_font_size_tip();
}

void LookTab::update_font_size_tip()
{
	if (font_size_ == nullptr)
		return;
	// ツールチップに、現在の配信の画面の px に直した値を表示する (cordmux.look.font_size.tip)。画面の大きさが分からなければ
	// 表示しない (%1x%2 と %3 を埋められない。0x0 と 0 px を表示すると、事実と違う数を見せることになる)。
	const int px = windows_calc::label_font_px(canvas_height_, edited_.label_em_ratio_pct);
	if (px <= 0 || canvas_width_ <= 0) {
		font_size_->spin()->setToolTip(QString());
		return;
	}
	ledger::TextRef tip;
	tip.id = keys::kLookFontSizeTip;
	tip.args = {std::to_string(canvas_width_), std::to_string(canvas_height_), std::to_string(px)};
	font_size_->spin()->setToolTip(tr_ref(tip));
}

void LookTab::update_color_swatch(QLabel *swatch, const std::string &hex)
{
	if (swatch == nullptr)
		return;
	// OBS の色のプロパティと同じ見せ方にし、色の名前を、その色を背景にしたラベルに書く (OBS 31 の
	// properties-view.cpp:764-797)。テキストの色は、白と黒のうち背景とのコントラスト比が高い方にする。
	const QColor color(QString::fromStdString(hex));
	swatch->setFrameStyle(QFrame::Sunken | QFrame::Panel);
	swatch->setAlignment(Qt::AlignCenter);
	swatch->setText(color.isValid() ? color.name(QColor::HexRgb).toUpper() : QString::fromStdString(hex));
	if (color.isValid()) {
		const QColor fg = ui::black_or_white_on(color);
		swatch->setStyleSheet(QStringLiteral("background-color: %1; color: %2;")
					      .arg(color.name(QColor::HexRgb), fg.name(QColor::HexRgb)));
	}
}

void LookTab::choose_color(ColorMember member, QLabel *swatch, const QString &title)
{
	// ダイアログのタイトルは入力欄の名前にする (OBS の色のプロパティと同じ)。同じ「色を選択」のタイトルのダイアログが複数の
	// 入力欄から開くと、どの入力欄の色を選んでいるかがダイアログの中で分からない。
	// 色のダイアログは、ドックの操作だけを止めて開く (dock/ui/message_dialog.h の run_owner_only_dialog)。開いている間も
	// OBS のメインウィンドウは操作できる (名前ラベルの色は配信の画面を見ながら選び、ダイアログは利用者が閉じるまで開いた
	// ままになるので、配信中にメインウィンドウを止めない)。QColorDialog::getColor は中で exec を呼び、メインウィンドウも
	// 止めるので使わない。
	const QColor initial(QString::fromStdString(edited_.*member));
	const QPointer<QWidget> before = QApplication::focusWidget();
	const QPointer<LookTab> self(this);
	QPointer<QColorDialog> dialog = new QColorDialog(initial, window());
	dialog->setObjectName(QStringLiteral("cordmuxColorDialog"));
	dialog->setWindowTitle(title);
	// 色を選んでいる間も、配信の画面に反映する (閉じてから結果を見ると、選び直しに時間がかかる)。閉じる前の色は覚えておいて、
	// キャンセルで戻す。
	const std::string value_before = edited_.*member;
	QObject::connect(dialog.data(), &QColorDialog::currentColorChanged, this, [this, member, swatch](const QColor &color) {
		if (!color.isValid())
			return;
		edited_.*member = color.name(QColor::HexRgb).toUpper().toStdString();
		update_color_swatch(swatch, edited_.*member);
		on_field_changed();
	});
	const int result = ui::run_owner_only_dialog(dialog.data(), this);
	// 開いている間に OBS の終了でドックが削除されたら、色のダイアログも一緒に削除されている。このタブのものには触らない
	// (message_dialog.h の約束)。
	if (self.isNull())
		return;
	QColor chosen;
	if (!dialog.isNull()) {
		QObject::disconnect(dialog.data(), nullptr, this, nullptr);
		if (result == QDialog::Accepted)
			chosen = dialog->selectedColor();
		delete dialog.data();
	}
	// 閉じたら、開く前のフォーカス ([色を選択]) に戻す (run_message_dialog と同じ)。
	if (!before.isNull() && before->isEnabled() && before->isVisible())
		before->setFocus(Qt::OtherFocusReason);
	if (!chosen.isValid()) {
		// キャンセルで閉じた。選んでいる間に反映した色を、開く前の色に戻す。
		if (edited_.*member != value_before) {
			edited_.*member = value_before;
			update_color_swatch(swatch, edited_.*member);
			on_field_changed();
		}
		return;
	}
	edited_.*member = chosen.name(QColor::HexRgb).toUpper().toStdString();
	update_color_swatch(swatch, edited_.*member);
	on_field_changed();
}

int LookTab::fill_font_weights(int weight, bool pick_nearest)
{
	if (font_weight_ == nullptr || font_family_ == nullptr)
		return weight;
	// 太さごとに、フォントが付けている名前を 1 つ持つ (同じ太さの名前が複数あるときは最初の 1 つ)。斜体は選ばせない。
	// フォントが標準 (400) か太字 (700) を持たないときは、持たない方を足す (Qt が文字列を合成して描く)。
	const QString family = font_family_->currentFont().family();
	std::map<int, QString> items;
	for (const QString &style : QFontDatabase::styles(family)) {
		if (!QFontDatabase::italic(family, style))
			items.emplace(QFontDatabase::weight(family, style), style);
	}
	items.emplace(static_cast<int>(QFont::Normal), tr_key(keys::kLookFontWeightNormalOption));
	items.emplace(static_cast<int>(QFont::Bold), tr_key(keys::kLookFontWeightBoldOption));

	const QSignalBlocker blocker(font_weight_);
	font_weight_->clear();
	for (const auto &[w, name] : items)
		font_weight_->addItem(name, w);
	int index = font_weight_->findData(weight);
	if (index < 0 && pick_nearest) {
		int best = items.begin()->first;
		for (const auto &item : items) {
			if (std::abs(item.first - weight) < std::abs(best - weight))
				best = item.first;
		}
		weight = best;
		index = font_weight_->findData(weight);
	} else if (index < 0) {
		// 保存されている値がどの項目とも合わないときは、その数を項目にして選べるままにする (値は書き換えない)。
		int at = 0;
		while (at < font_weight_->count() && font_weight_->itemData(at).toInt() < weight)
			++at;
		font_weight_->insertItem(at, QString::number(weight), weight);
		index = at;
	}
	font_weight_->setCurrentIndex(index);
	tip_items(font_weight_);
	return weight;
}

void LookTab::fill_source_combo(QComboBox *combo, const std::vector<sources::SelfSourceCandidate> &choices,
				const std::string &uuid, const std::string &name, bool is_frame)
{
	// 選択肢のデータは、UUID (UserRole) と名前 (UserRole + 1)。名前が同じ別のソースがあるときは、cordmux もその名前の
	// ソースを使うので、そのソースを選んだ見た目にする。
	const QSignalBlocker blocker(combo);
	combo->clear();
	const QString first = is_frame ? tr_key(ledger::look_self_frame_first_option_key(view_.has_main_account))
				       : tr_key(keys::kLookNoneOption);
	combo->addItem(first, QString());
	combo->setItemData(0, QString(), Qt::UserRole + 1);
	for (const sources::SelfSourceCandidate &choice : choices) {
		combo->addItem(source_option_text(choice), QString::fromStdString(choice.uuid));
		combo->setItemData(combo->count() - 1, QString::fromStdString(choice.name), Qt::UserRole + 1);
		combo->setItemData(combo->count() - 1, source_option_text(choice), Qt::ToolTipRole);
	}
	int selected = 0;
	const int found = sources::find_self_source(choices, uuid, name);
	if (found >= 0) {
		selected = found + 1;
	} else if (!uuid.empty() || !name.empty()) {
		// 保存したソースが OBS に見つからないときは、その名前に見つからないことを添えた選択肢を足して選んだままにする。
		ledger::TextRef missing;
		missing.id = keys::kLookSelfSourceMissingOption;
		missing.args = {name};
		combo->addItem(tr_ref(missing), QString::fromStdString(uuid));
		combo->setItemData(combo->count() - 1, QString::fromStdString(name), Qt::UserRole + 1);
		combo->setItemData(combo->count() - 1, tr_ref(missing), Qt::ToolTipRole);
		selected = combo->count() - 1;
	}
	combo->setCurrentIndex(selected);
}

void LookTab::set_source_providers(Candidates frame, Candidates speaking)
{
	frame_provider_ = std::move(frame);
	speaking_provider_ = std::move(speaking);
}

void LookTab::refresh_sources()
{
	// 開いている間は選択肢を作り直さない (開いている一覧が消える)。
	for (QComboBox *combo : {frame_combo_, speaking_combo_}) {
		if (combo != nullptr && combo->view() != nullptr && combo->view()->isVisible())
			return;
	}
	if (frame_provider_)
		frame_choices_ = frame_provider_();
	if (speaking_provider_)
		speaking_choices_ = speaking_provider_();
	loading_ = true;
	for (const Field &field : fields_) {
		if (field.widget == frame_combo_ || field.widget == speaking_combo_)
			field.load();
	}
	loading_ = false;
}

void LookTab::set_view(const ledger::dockview::LookTabView &view)
{
	const bool main_changed = view.has_main_account != view_.has_main_account;
	view_ = view;
	loading_ = true;
	{
		const QSignalBlocker blocker(dummy_check_);
		dummy_check_->setChecked(view.dummy_on);
	}
	loading_ = false;
	const bool allowed = !view.dummy_toggle.disabled_reason.has_value();
	dummy_check_->setEnabled(allowed);
	dummy_check_->setToolTip(allowed ? QString() : tr_ref(*view.dummy_toggle.disabled_reason));
	// 先頭の選択肢の文字列は、メインアカウントの有無で変わる (保存する値は同じ)。
	if (main_changed && frame_combo_ != nullptr && frame_combo_->count() > 0 && !(frame_combo_->view() && frame_combo_->view()->isVisible()))
		frame_combo_->setItemText(0, tr_key(ledger::look_self_frame_first_option_key(view.has_main_account)));
	if (main_changed)
		update_self_first_enabled();
}

void LookTab::set_values(const config::Settings &settings)
{
	// 変えた値をまだ渡していない間は、書き換えない (入力欄が古い値に戻る)。
	if (dirty_)
		return;
	if (!look_tab_fields_differ(edited_, settings))
		return;
	edited_ = settings;
	loading_ = true;
	for (const Field &field : fields_) {
		if (ledger::look_field_follows(field.editing ? field.editing() : false, dirty_, true))
			field.load();
		else if (field.store)
			field.store();
	}
	loading_ = false;
	update_dependents();
	update_font_size_tip();
}

void LookTab::on_field_changed()
{
	if (loading_)
		return;
	for (const Field &field : fields_)
		field.store();
	update_dependents();
	update_font_size_tip();
	request_reflect();
}

bool LookTab::control_enabled(ledger::LookControl control) const
{
	return ledger::look_control_enabled(control, edited_);
}

void LookTab::update_dependents()
{
	// 反映されない入力欄は、隠さずに操作できない見た目にする。ラベルも一緒に操作できない見た目にする (ラベルだけ濃いと、
	// 操作できるように見える)。
	for (const Dependent &d : dependents_) {
		const bool on = ledger::look_control_enabled(d.control, edited_);
		for (QWidget *w : d.widgets)
			w->setEnabled(on);
	}
	update_self_first_enabled();
	// 配置に合わない並びは、選べない項目にして、選ばれている並びを設定の値に合わせる。
	if (roster_direction_ != nullptr) {
		auto *model = qobject_cast<QStandardItemModel *>(roster_direction_->model());
		for (int i = 0; i < roster_direction_->count(); ++i) {
			const auto want = static_cast<config::RosterDirection>(roster_direction_->itemData(i).toInt());
			QStandardItem *item = model != nullptr ? model->item(i) : nullptr;
			if (item != nullptr)
				item->setEnabled(ledger::look_roster_direction_allowed(edited_.roster_anchor, want));
		}
		const QSignalBlocker blocker(roster_direction_);
		ui::set_combo_data(roster_direction_, static_cast<int>(edited_.roster_direction));
	}
}

void LookTab::update_self_first_enabled()
{
	if (self_first_check_ != nullptr)
		self_first_check_->setEnabled(ledger::look_self_slot_first_enabled(edited_, view_.has_main_account));
}

void LookTab::request_reflect()
{
	dirty_ = true;
	const int wait = ledger::look_reflect_wait_ms(last_reflect_ms_, now_ms(), ledger::kLookReflectIntervalMs);
	if (wait <= 0) {
		reflect_timer_->stop();
		reflect_now();
		return;
	}
	if (!reflect_timer_->isActive())
		reflect_timer_->start(wait);
}

void LookTab::reflect_now()
{
	dirty_ = false;
	last_reflect_ms_ = now_ms();
	if (on_changed_)
		on_changed_(edited_);
}

void LookTab::flush()
{
	reflect_timer_->stop();
	if (dirty_)
		reflect_now();
}

QWidget *LookTab::field(const QString &json_keys) const
{
	const QString prefix = QStringLiteral("cordmuxLook_");
	if (QWidget *exact = findChild<QWidget *>(prefix + json_keys))
		return exact;
	// 1 つの入力欄が 2 つ以上のキーを持つとき (objectName はキーを「+」でつないだ名前) も、キー 1 つで見つける。
	for (QWidget *w : findChildren<QWidget *>()) {
		if (!w->objectName().startsWith(prefix))
			continue;
		if (w->objectName().mid(prefix.size()).split(QLatin1Char('+')).contains(json_keys))
			return w;
	}
	return nullptr;
}

} // namespace cordmux::dock
