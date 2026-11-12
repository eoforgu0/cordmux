// SPDX-License-Identifier: Apache-2.0

// tuner_window.h の実装。

#include "tuner_window.h"

#include <QAbstractScrollArea>
#include <QCheckBox>
#include <QComboBox>
#include <QColor>
#include <QColorDialog>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "composite_scene.h"
#include "preview_views.h"
#include "sources/decor_paint.h"

namespace cordmux::label_tuner {

namespace sources = cordmux::sources;
namespace decor_preview = cordmux::decor_preview;

namespace {

constexpr int kCanvasWidth = kPreviewCanvasWidth;
constexpr int kCanvasHeight = kPreviewCanvasHeight;

// 等倍と 2 倍で見る範囲の、名前ラベルと光の外側の余白 (キャンバスのピクセル)。
constexpr int kCropPad = 24;

// 重ねる仕事 1 つを分ける帯の数 (同時に重ねるスレッドの数)。
constexpr int kComposeBands = 4;

// 等倍と 2 倍の表示を名前ラベルに合わせてスクロールするときの、表示の端との間 (キャンバスのピクセル)。
constexpr double kScrollMargin = 6.0;

// 名前ラベルを置いていないときに見る、枠の隅の範囲。
constexpr int kEmptyCropWidth = 360;
constexpr int kEmptyCropHeight = 200;

// 背景の選び方のコンボボックスの並び。先頭は見本と同じ使い分けで、1 から 4 はどの枠も同じ 1 種類、最後は画像のファイル。
const char *const kBackgroundTitles[] = {
	"枠ごとに違う 4 種類 (見本と同じ)",
	"暗いゲーム画面",
	"明るい空",
	"細かい縞",
	"下側が明るい景色",
	"画像のファイル",
};
constexpr int kPictureChoice = 5;

int decimals_of(int divisor)
{
	int decimals = 0;
	for (int d = divisor; d > 1; d /= 10)
		++decimals;
	return decimals;
}

// 枠の位置の名前。1 人で全面のときは「全面」。
QString tile_title(const decor_preview::ScenePlan &plan, std::size_t tile)
{
	if (plan.layout.tiles.size() == 1)
		return QStringLiteral("全面");
	static const char *const names[] = {"左上", "右上", "左下", "右下"};
	return tile < 4 ? QString::fromUtf8(names[tile]) : QString();
}

// つまみの製品の既定値の文字。つまみの名前の行の右に、薄い色で出す。
QString default_text(const KnobDef &def)
{
	const config::Settings defaults;
	switch (def.kind) {
	case KnobKind::Number: {
		QString text = QString::number(def.get_number(defaults), 'f', decimals_of(def.divisor));
		if (def.unit[0] != '\0')
			text += QStringLiteral(" ") + QString::fromUtf8(def.unit);
		return text;
	}
	case KnobKind::Toggle:
		return def.get_toggle(defaults) ? QStringLiteral("オン") : QStringLiteral("オフ");
	case KnobKind::Color:
		return QString::fromStdString(def.get_text(defaults)).toUpper();
	case KnobKind::Font:
		return sources::default_label_font_family();
	case KnobKind::Choice:
		return QString::fromUtf8(def.options[static_cast<std::size_t>(def.get_choice(defaults))]);
	}
	return QString();
}

// 1 行にまとめたチェックボックス (KnobDef::row が同じ並び) の既定値の文字。オンのものの名前を並べる。
QString default_row_text(const std::vector<const KnobDef *> &defs)
{
	const config::Settings defaults;
	QStringList on;
	for (const KnobDef *def : defs)
		if (def->get_toggle(defaults))
			on.append(QString::fromUtf8(def->title));
	return on.isEmpty() ? QStringLiteral("なし") : on.join(QStringLiteral("、"));
}

// つまみの名前の行。左に名前 (チェックボックスのつまみはチェックボックス)、右に既定値の文字 default_value。
void add_title_row(QVBoxLayout *layout, QWidget *parent, QWidget *title, const QString &default_value)
{
	auto *row = new QHBoxLayout();
	row->addWidget(title, 1);
	auto *hint = new QLabel(QStringLiteral("既定 %1").arg(default_value), parent);
	QPalette palette = hint->palette();
	palette.setColor(QPalette::WindowText, palette.color(QPalette::Disabled, QPalette::WindowText));
	hint->setPalette(palette);
	row->addWidget(hint);
	layout->addLayout(row);
}

// 等倍と 2 倍の表示の 1 つの軸で、見える範囲の始まり (キャンバスのピクセル)。port は見える長さ、名前ラベルは
// [label_begin, label_end)、edge は名前ラベルの側の枠の隅のピクセル、at_start は隅がこの軸の始まりの側 (左か上) か。
//   - 名前ラベルが入りきらなければ、隅の側の端をそろえる (左下の名前ラベルなら左端と下端)。
//   - 入れば名前ラベルを先に入れ、残りで枠の境 (隣の枠の 1 ピクセルを含む) をできるだけ入れる。
// kScrollMargin は、表示の端と名前ラベルや枠の境の間に空けるピクセル。
double scroll_start(double port, double label_begin, double label_end, double edge, bool at_start)
{
	const double length = label_end - label_begin;
	if (length >= port)
		return at_start ? label_begin - kScrollMargin : label_end + kScrollMargin - port;
	const double room = std::min(kScrollMargin, (port - length) / 2.0);
	if (at_start)
		return std::max(std::min(edge - kScrollMargin, label_begin - room), label_end + room - port);
	return std::min(std::max(edge + 1 + kScrollMargin, label_end + room) - port, label_begin - room);
}

QIcon swatch_icon(const QString &hex)
{
	QPixmap pixmap(28, 16);
	pixmap.fill(QColor(hex));
	QPainter painter(&pixmap);
	painter.setPen(QColor(96, 96, 96));
	painter.drawRect(0, 0, pixmap.width() - 1, pixmap.height() - 1);
	return QIcon(pixmap);
}

} // namespace

TunerWindow::TunerWindow(const Options &options, const config::Settings &settings, const ToolState &state,
			 std::function<std::uint64_t()> clock)
	: options_(options),
	  settings_(settings),
	  state_(state),
	  clock_(std::move(clock)),
	  renderer_(!options.shot),
	  full_worker_(!options.shot, kComposeBands),
	  crop_worker_(!options.shot, kComposeBands)
{
	setWindowTitle(QStringLiteral("名前ラベルの調整 (cordmux の合成のソース)"));
	background_mode_ = state_.background;
	background_kind_ = state_.background_kind;
	if (background_mode_ == BackgroundMode::Picture) {
		picture_ = QImage(state_.picture_path).convertToFormat(QImage::Format_ARGB32_Premultiplied);
		if (picture_.isNull()) {
			note_ = QStringLiteral("背景の画像を読めません: %1").arg(state_.picture_path);
			background_mode_ = BackgroundMode::Mixed;
		}
	}

	auto *root = new QVBoxLayout(this);
	// 3 つの列は、境をドラッグして幅を変えられる (表示の拡大率が大きい画面でも、見たい列を広げられるように)。
	auto *main_row = new QSplitter(Qt::Horizontal);
	main_row->setChildrenCollapsible(false);
	root->addWidget(main_row, 1);

	// 左の列は、上から全体を縮めた表示 (16:9 の高さ)、等倍、2 倍。その右に場面、右の端につまみを並べる。
	auto *left_column = new QWidget();
	auto *left = new QVBoxLayout(left_column);
	left->setContentsMargins(0, 0, 0, 0);
	main_row->addWidget(left_column);
	fit_ = new FitView();
	fit_->setToolTip(QStringLiteral("配信の画面 (1920 x 1080) 全体を縮めた表示です。クリックした枠の名前ラベルを、"
					"下の等倍と 2 倍で見ます。"));
	fit_->set_click_handler([this](const QPoint &at) {
		const decor_preview::ScenePlan &plan = renderer_.plan();
		for (std::size_t i = 0; i < plan.layout.tiles.size(); ++i) {
			const sources::PixelRect &r = plan.layout.tiles[i].rect;
			if (QRect(r.x, r.y, r.w, r.h).contains(at)) {
				view_combo_->setCurrentIndex(static_cast<int>(i));
				return;
			}
		}
	});
	left->addWidget(fit_);

	const auto add_pixel_view = [left](const QString &title, PixelView *view, QScrollArea **scroll) {
		auto *group = new QGroupBox(title);
		auto *layout = new QVBoxLayout(group);
		*scroll = new QScrollArea();
		(*scroll)->setWidget(view);
		(*scroll)->setWidgetResizable(false);
		(*scroll)->setAlignment(Qt::AlignLeft | Qt::AlignTop);
		layout->addWidget(*scroll);
		left->addWidget(group, 1);
	};
	one_ = new PixelView(1);
	two_ = new PixelView(2);
	add_pixel_view(QStringLiteral("等倍 (画像の 1 ピクセルを画面の 1 ピクセルで表示)"), one_, &one_scroll_);
	add_pixel_view(QStringLiteral("2 倍 (補間せずに拡大)"), two_, &two_scroll_);

	main_row->addWidget(build_scene_panel());
	main_row->addWidget(build_knob_panel());
	main_row->setStretchFactor(0, 1);
	main_row->setStretchFactor(1, 0);
	main_row->setStretchFactor(2, 0);
	main_row->setSizes({600, 540, 440});

	auto *bar = new QHBoxLayout();
	status_ = new QLabel();
	status_->setWordWrap(true);
	bar->addWidget(status_, 1);
	reset_button_ = new QPushButton(QStringLiteral("既定に戻す"));
	reset_button_->setToolTip(
		QStringLiteral("つまみを全部、製品の既定値に戻します。場面 (人と背景) は変えません。"));
	connect(reset_button_, &QPushButton::clicked, this, [this]() { reset_to_defaults(); });
	bar->addWidget(reset_button_);
	save_button_ = new QPushButton(QStringLiteral("保存"));
	if (options_.save_path.isEmpty()) {
		save_button_->setEnabled(false);
		save_button_->setToolTip(QStringLiteral("起動の引数 --save <パス> で保存先を渡すと押せます。"));
	} else {
		save_button_->setToolTip(QStringLiteral("つまみの値を %1 に書きます。").arg(options_.save_path));
	}
	connect(save_button_, &QPushButton::clicked, this, [this]() { save(); });
	bar->addWidget(save_button_);
	root->addLayout(bar);

	if (!options_.load_note.isEmpty())
		note_ = note_.isEmpty() ? options_.load_note : options_.load_note + QStringLiteral("  ") + note_;

	load_knob_widgets();
	renderer_.set_request(make_request());
	rebuild_view_choices();
	update_seat_rows();

	if (!options_.shot) {
		timer_ = new QTimer(this);
		timer_->setTimerType(Qt::PreciseTimer);
		connect(timer_, &QTimer::timeout, this, [this]() { tick(); });
		timer_->start(16);
	}
}

TunerWindow::~TunerWindow() = default;

QWidget *TunerWindow::build_scene_panel()
{
	auto *panel = new QWidget();
	auto *layout = new QVBoxLayout(panel);
	layout->setContentsMargins(0, 0, 0, 0);

	auto *people_row = new QHBoxLayout();
	people_row->addWidget(new QLabel(QStringLiteral("人数")));
	people_spin_ = new QSpinBox();
	people_spin_->setRange(1, sources::kCompositeCellCount);
	people_spin_->setValue(state_.people);
	people_spin_->setToolTip(
		QStringLiteral("製品の配置のとおりに並びます。2 人以上は 4 等分で、人 1 (自分の枠) が左上、"
			       "ほかの人は名前の順です。1 人は全面です。"));
	connect(people_spin_, &QSpinBox::valueChanged, this, [this](int value) {
		state_.people = value;
		update_seat_rows();
		on_scene_changed();
	});
	people_row->addWidget(people_spin_);
	people_row->addStretch(1);
	layout->addLayout(people_row);

	const std::vector<decor_preview::ScenePerson> &people = decor_preview::scene_people();
	for (int seat = 0; seat < sources::kCompositeCellCount; ++seat) {
		SeatRow &row = rows_[static_cast<std::size_t>(seat)];
		SeatState &seat_state = state_.seats[static_cast<std::size_t>(seat)];
		row.box = new QGroupBox(seat == 0 ? QStringLiteral("人 1 (自分の枠)")
						  : QStringLiteral("人 %1").arg(seat + 1));
		auto *grid = new QGridLayout(row.box);
		row.person = new WheelSafeComboBox();
		row.person->setFocusPolicy(Qt::StrongFocus);
		for (const decor_preview::ScenePerson &person : people) {
			QString title = QString::fromUtf8(person.name);
			if (!person.has_avatar)
				title += QStringLiteral(" (アバターの画像なし)");
			row.person->addItem(title);
		}
		row.person->setCurrentIndex(seat_state.person);
		connect(row.person, &QComboBox::currentIndexChanged, this, [this, seat](int index) {
			if (index < 0)
				return;
			state_.seats[static_cast<std::size_t>(seat)].person = index;
			on_scene_changed();
		});
		row.info = new QLabel();
		grid->addWidget(row.person, 0, 0, 1, 4);
		grid->addWidget(row.info, 3, 0, 1, 4);

		row.speak = new QCheckBox(QStringLiteral("話している"));
		connect(row.speak, &QCheckBox::toggled, this, [this, seat](bool on) {
			if (!loading_)
				tracks_[static_cast<std::size_t>(seat)].set_toggle(on, clock_(), timing());
		});
		row.hold = new QPushButton(QStringLiteral("プッシュ・トゥ・トーク"));
		connect(row.hold, &QPushButton::pressed, this,
			[this, seat]() { tracks_[static_cast<std::size_t>(seat)].set_hold(true, clock_(), timing()); });
		connect(row.hold, &QPushButton::released, this, [this, seat]() {
			tracks_[static_cast<std::size_t>(seat)].set_hold(false, clock_(), timing());
		});
		row.cycle = new QCheckBox(QStringLiteral("2 秒話して 1 秒黙る"));
		row.cycle->setToolTip(
			QStringLiteral("話し始めて 2 秒で止まり、1 秒後にまた話し始める、を繰り返します。"));
		connect(row.cycle, &QCheckBox::toggled, this, [this, seat](bool on) {
			if (!loading_)
				tracks_[static_cast<std::size_t>(seat)].set_cycle(on, clock_(), timing());
		});
		grid->addWidget(row.speak, 1, 0);
		grid->addWidget(row.hold, 1, 1, 1, 3);
		grid->addWidget(row.cycle, 2, 0, 1, 4, Qt::AlignLeft);
		layout->addWidget(row.box);
	}

	// 画面共有していない参加者。参加者ごとの話し方は席と同じ。cordmux がその人の声を聞けるかは、最後の 1 人だけ切り替える
	// (席の人は、自分の枠の人も、いつも聞ける人にする)。
	auto *roster_group = new QGroupBox(QStringLiteral("画面共有していない参加者"));
	auto *roster_box = new QVBoxLayout(roster_group);
	auto *roster_count_row = new QHBoxLayout();
	roster_count_row->addWidget(new QLabel(QStringLiteral("人数")));
	roster_spin_ = new QSpinBox();
	roster_spin_->setRange(0, kRosterRows);
	roster_spin_->setValue(state_.roster);
	roster_spin_->setToolTip(QStringLiteral("通話に参加していて、映像の枠を持たない人です。名前の順に並びます。"));
	connect(roster_spin_, &QSpinBox::valueChanged, this, [this](int value) {
		state_.roster = value;
		update_seat_rows();
		on_scene_changed();
	});
	roster_count_row->addWidget(roster_spin_);
	roster_unheard_ = new QCheckBox(QStringLiteral("最後の 1 人は声を聞けない"));
	roster_unheard_->setToolTip(QStringLiteral("声の経路を持てない人です。話しても光らず、"
						   "暗くもしません。名前の順に並べた最後の人に当てます。"));
	roster_unheard_->setChecked(state_.roster_last_unheard);
	connect(roster_unheard_, &QCheckBox::toggled, this, [this](bool on) {
		if (loading_)
			return;
		state_.roster_last_unheard = on;
		on_scene_changed();
	});
	roster_count_row->addWidget(roster_unheard_);
	roster_count_row->addStretch(1);
	roster_box->addLayout(roster_count_row);
	for (int index = 0; index < kRosterRows; ++index) {
		RosterRow &row = roster_rows_[static_cast<std::size_t>(index)];
		RosterRowState &row_state = state_.roster_rows[static_cast<std::size_t>(index)];
		const std::size_t track = static_cast<std::size_t>(sources::kCompositeCellCount + index);
		row.box = new QGroupBox(QStringLiteral("参加者 %1").arg(index + 1));
		auto *grid = new QGridLayout(row.box);
		row.person = new WheelSafeComboBox();
		row.person->setFocusPolicy(Qt::StrongFocus);
		for (const decor_preview::ScenePerson &person : people) {
			QString title = QString::fromUtf8(person.name);
			if (!person.has_avatar)
				title += QStringLiteral(" (アバターの画像なし)");
			row.person->addItem(title);
		}
		row.person->setCurrentIndex(row_state.person);
		connect(row.person, &QComboBox::currentIndexChanged, this, [this, index](int item) {
			if (item < 0)
				return;
			state_.roster_rows[static_cast<std::size_t>(index)].person = item;
			on_scene_changed();
		});
		row.info = new QLabel();
		grid->addWidget(row.person, 0, 0, 1, 2);
		grid->addWidget(row.info, 3, 0, 1, 4);

		row.speak = new QCheckBox(QStringLiteral("話している"));
		connect(row.speak, &QCheckBox::toggled, this, [this, track](bool on) {
			if (!loading_)
				tracks_[track].set_toggle(on, clock_(), timing());
		});
		row.hold = new QPushButton(QStringLiteral("プッシュ・トゥ・トーク"));
		connect(row.hold, &QPushButton::pressed, this,
			[this, track]() { tracks_[track].set_hold(true, clock_(), timing()); });
		connect(row.hold, &QPushButton::released, this,
			[this, track]() { tracks_[track].set_hold(false, clock_(), timing()); });
		row.cycle = new QCheckBox(QStringLiteral("2 秒話して 1 秒黙る"));
		row.cycle->setToolTip(
			QStringLiteral("話し始めて 2 秒で止まり、1 秒後にまた話し始める、を繰り返します。"));
		connect(row.cycle, &QCheckBox::toggled, this, [this, track](bool on) {
			if (!loading_)
				tracks_[track].set_cycle(on, clock_(), timing());
		});
		grid->addWidget(row.speak, 1, 0);
		grid->addWidget(row.hold, 1, 1, 1, 3);
		grid->addWidget(row.cycle, 2, 0, 1, 4, Qt::AlignLeft);
		roster_box->addWidget(row.box);
	}
	layout->addWidget(roster_group);

	auto *view_row = new QHBoxLayout();
	view_row->addWidget(new QLabel(QStringLiteral("等倍と 2 倍で見る名前ラベル")));
	view_combo_ = new WheelSafeComboBox();
	view_combo_->setFocusPolicy(Qt::StrongFocus);
	connect(view_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
		if (loading_ || index < 0)
			return;
		// 先頭はタイル、その後ろが画面共有していない参加者。
		const int tiles = static_cast<int>(renderer_.plan().layout.tiles.size());
		if (index < tiles) {
			state_.view_tile = index;
			state_.view_roster = -1;
		} else {
			state_.view_roster = index - tiles;
		}
		dirty_ = true;
	});
	view_row->addWidget(view_combo_, 1);
	layout->addLayout(view_row);

	auto *background_row = new QHBoxLayout();
	background_row->addWidget(new QLabel(QStringLiteral("見本の映像")));
	background_combo_ = new WheelSafeComboBox();
	background_combo_->setFocusPolicy(Qt::StrongFocus);
	for (const char *title : kBackgroundTitles)
		background_combo_->addItem(QString::fromUtf8(title));
	background_combo_->setCurrentIndex(background_mode_ == BackgroundMode::Mixed  ? 0
					   : background_mode_ == BackgroundMode::Kind ? 1 + background_kind_
										      : kPictureChoice);
	connect(background_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
		if (loading_ || index < 0)
			return;
		if (index == kPictureChoice) {
			if (picture_.isNull()) {
				open_picture();
				return;
			}
			background_mode_ = BackgroundMode::Picture;
		} else if (index == 0) {
			background_mode_ = BackgroundMode::Mixed;
		} else {
			background_mode_ = BackgroundMode::Kind;
			background_kind_ = index - 1;
		}
		background_dirty_ = true;
	});
	background_row->addWidget(background_combo_, 1);
	auto *open = new QPushButton(QStringLiteral("画像を開く..."));
	open->setToolTip(QStringLiteral("画面共有のスクリーンショットなどを、どの枠にも背景として置きます (製品が映像を"
					"置くのと同じく、縦横比を保って枠に収めます)。"));
	connect(open, &QPushButton::clicked, this, [this]() { open_picture(); });
	background_row->addWidget(open);
	layout->addLayout(background_row);
	picture_label_ = new QLabel();
	picture_label_->setWordWrap(true);
	if (!picture_.isNull())
		picture_label_->setText(QFileInfo(state_.picture_path).fileName());
	layout->addWidget(picture_label_);
	layout->addStretch(1);
	// 場面の行が多いので、縦にスクロールできるようにする。
	auto *scroll = new QScrollArea();
	scroll->setWidget(panel);
	scroll->setWidgetResizable(true);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setMinimumWidth(panel->sizeHint().width() + scroll->verticalScrollBar()->sizeHint().width() + 8);
	scene_scroll_ = scroll;
	return scroll;
}

QWidget *TunerWindow::build_knob_panel()
{
	auto *scroll = new QScrollArea();
	auto *inner = new QWidget();
	auto *layout = new QVBoxLayout(inner);
	QGroupBox *group = nullptr;
	QVBoxLayout *group_layout = nullptr;
	QString group_title;
	QHBoxLayout *row_layout = nullptr;
	const std::vector<KnobDef> &defs = knob_defs();
	for (std::size_t i = 0; i < defs.size(); ++i) {
		const KnobDef &def = defs[i];
		if (!group || group_title != QString::fromUtf8(def.group)) {
			group_title = QString::fromUtf8(def.group);
			group = new QGroupBox(group_title);
			group_layout = new QVBoxLayout(group);
			layout->addWidget(group);
			row_layout = nullptr;
		}
		if (def.row != nullptr && row_layout == nullptr) {
			// 同じ名前の行が続く間のチェックボックスを集めて、行の名前と既定値の行、チェックボックスの行を作る。
			std::vector<const KnobDef *> members;
			for (std::size_t j = i; j < defs.size() && defs[j].row != nullptr &&
						std::string(defs[j].row) == def.row && std::string(defs[j].group) == def.group;
			     ++j)
				members.push_back(&defs[j]);
			add_title_row(group_layout, group, new QLabel(QString::fromUtf8(def.row), group),
				      default_row_text(members));
			row_layout = new QHBoxLayout();
			group_layout->addLayout(row_layout);
		}
		add_knob(def, group, group_layout, def.row != nullptr ? row_layout : nullptr);
		const bool row_continues = def.row != nullptr && i + 1 < defs.size() && defs[i + 1].row != nullptr &&
					   std::string(defs[i + 1].row) == def.row &&
					   std::string(defs[i + 1].group) == def.group;
		if (def.row != nullptr && !row_continues) {
			row_layout->addStretch(1);
			row_layout = nullptr;
		}
	}
	layout->addStretch(1);
	scroll->setWidget(inner);
	scroll->setWidgetResizable(true);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scroll->setMinimumWidth(320);
	knob_scroll_ = scroll;
	return scroll;
}

void TunerWindow::add_knob(const KnobDef &def, QWidget *parent, QVBoxLayout *layout, QHBoxLayout *row_layout)
{
	KnobWidgets knob;
	knob.def = &def;
	const QString title = QString::fromUtf8(def.title);
	switch (def.kind) {
	case KnobKind::Toggle: {
		knob.check = new QCheckBox(title, parent);
		connect(knob.check, &QCheckBox::toggled, this, [this, &def](bool on) {
			if (loading_)
				return;
			def.set_toggle(settings_, on);
			on_scene_changed();
		});
		if (row_layout != nullptr)
			row_layout->addWidget(knob.check);
		else
			add_title_row(layout, parent, knob.check, default_text(def));
		break;
	}
	case KnobKind::Number: {
		knob.label = new QLabel(title, parent);
		add_title_row(layout, parent, knob.label, default_text(def));
		auto *row = new QHBoxLayout();
		knob.slider = new WheelSafeSlider(Qt::Horizontal, parent);
		knob.slider->setFocusPolicy(Qt::StrongFocus);
		knob.slider->setRange(static_cast<int>(std::lround(def.min * def.divisor)),
				      static_cast<int>(std::lround(def.max * def.divisor)));
		knob.spin = new WheelSafeSpinBox(parent);
		knob.spin->setFocusPolicy(Qt::StrongFocus);
		knob.spin->setDecimals(decimals_of(def.divisor));
		knob.spin->setRange(def.min, def.max);
		knob.spin->setSingleStep(static_cast<double>(def.step) / def.divisor);
		knob.slider->setSingleStep(def.step);
		knob.slider->setPageStep(def.step);
		if (def.unit[0] != '\0')
			knob.spin->setSuffix(QStringLiteral(" ") + QString::fromUtf8(def.unit));
		knob.spin->setMinimumWidth(96);
		const std::size_t index = knobs_.size();
		connect(knob.slider, &QSlider::valueChanged, this, [this, index](int step) {
			if (loading_)
				return;
			KnobWidgets &k = knobs_[index];
			const double value = snap_to_step(*k.def, static_cast<double>(step) / k.def->divisor);
			k.def->set_number(settings_, value);
			const QSignalBlocker block(k.spin);
			k.spin->setValue(value);
			// 刻みが 1 段より粗いつまみ (5 % 刻みの暗さ) は、スライダーも刻みの位置に合わせる。
			const int snapped = static_cast<int>(std::lround(value * k.def->divisor));
			if (snapped != step) {
				const QSignalBlocker block_slider(k.slider);
				k.slider->setValue(snapped);
			}
			on_scene_changed();
		});
		connect(knob.spin, &QDoubleSpinBox::valueChanged, this, [this, index](double typed) {
			if (loading_)
				return;
			KnobWidgets &k = knobs_[index];
			const double value = snap_to_step(*k.def, typed);
			k.def->set_number(settings_, value);
			const QSignalBlocker block(k.slider);
			k.slider->setValue(static_cast<int>(std::lround(value * k.def->divisor)));
			on_scene_changed();
		});
		row->addWidget(knob.slider, 1);
		row->addWidget(knob.spin);
		layout->addLayout(row);
		break;
	}
	case KnobKind::Color: {
		knob.label = new QLabel(title, parent);
		add_title_row(layout, parent, knob.label, default_text(def));
		auto *row = new QHBoxLayout();
		knob.swatch = new QPushButton(parent);
		knob.swatch->setToolTip(QStringLiteral("色を選ぶ"));
		knob.hex = new QLineEdit(parent);
		knob.hex->setValidator(new QRegularExpressionValidator(
			QRegularExpression(QStringLiteral("#[0-9A-Fa-f]{6}")), knob.hex));
		knob.hex->setMaximumWidth(110);
		const std::size_t index = knobs_.size();
		connect(knob.swatch, &QPushButton::clicked, this, [this, index, title]() {
			KnobWidgets &k = knobs_[index];
			const QColor chosen = QColorDialog::getColor(QColor(k.hex->text()), this, title);
			if (chosen.isValid())
				set_color(k, chosen.name(QColor::HexRgb));
		});
		connect(knob.hex, &QLineEdit::editingFinished, this, [this, index]() {
			KnobWidgets &k = knobs_[index];
			if (k.hex->hasAcceptableInput())
				set_color(k, k.hex->text());
			else
				k.hex->setText(QString::fromStdString(k.def->get_text(settings_)));
		});
		row->addWidget(knob.swatch);
		row->addWidget(knob.hex);
		row->addStretch(1);
		layout->addLayout(row);
		break;
	}
	case KnobKind::Font: {
		knob.label = new QLabel(title, parent);
		add_title_row(layout, parent, knob.label, default_text(def));
		knob.combo = new WheelSafeComboBox(parent);
		knob.combo->setFocusPolicy(Qt::StrongFocus);
		// 先頭は設定が空のとき (製品の既定のフォント)。
		knob.combo->addItem(
			QStringLiteral("(既定) %1").arg(sources::default_label_font_family()),
			QString());
		for (const QString &family : QFontDatabase::families())
			knob.combo->addItem(family, family);
		connect(knob.combo, &QComboBox::currentIndexChanged, this,
			[this, &def, index = knobs_.size()](int item) {
				if (loading_ || item < 0)
					return;
				def.set_text(settings_, knobs_[index].combo->itemData(item).toString().toStdString());
				on_scene_changed();
			});
		layout->addWidget(knob.combo);
		break;
	}
	case KnobKind::Choice: {
		knob.label = new QLabel(title, parent);
		add_title_row(layout, parent, knob.label, default_text(def));
		knob.combo = new WheelSafeComboBox(parent);
		knob.combo->setFocusPolicy(Qt::StrongFocus);
		for (const char *option : def.options)
			knob.combo->addItem(QString::fromUtf8(option));
		connect(knob.combo, &QComboBox::currentIndexChanged, this, [this, &def](int item) {
			if (loading_ || item < 0)
				return;
			def.set_choice(settings_, item);
			// 配置を変えると、合わない並びは合う並びに変わるので、選択肢の表示を修正する。
			update_choice_states();
			on_scene_changed();
		});
		layout->addWidget(knob.combo);
		break;
	}
	}
	knobs_.push_back(knob);
}

void TunerWindow::set_color(KnobWidgets &knob, const QString &hex)
{
	const QString upper = hex.toUpper();
	knob.def->set_text(settings_, upper.toStdString());
	{
		const QSignalBlocker block(knob.hex);
		knob.hex->setText(upper);
	}
	knob.swatch->setIcon(swatch_icon(upper));
	on_scene_changed();
}

void TunerWindow::load_knob_widgets()
{
	loading_ = true;
	for (KnobWidgets &knob : knobs_) {
		const KnobDef &def = *knob.def;
		switch (def.kind) {
		case KnobKind::Toggle:
			knob.check->setChecked(def.get_toggle(settings_));
			break;
		case KnobKind::Number: {
			const double value = def.get_number(settings_);
			knob.slider->setValue(static_cast<int>(std::lround(value * def.divisor)));
			knob.spin->setValue(value);
			break;
		}
		case KnobKind::Color: {
			const QString hex = QString::fromStdString(def.get_text(settings_)).toUpper();
			knob.hex->setText(hex);
			knob.swatch->setIcon(swatch_icon(hex));
			break;
		}
		case KnobKind::Font: {
			const QString family = QString::fromStdString(def.get_text(settings_));
			int item = knob.combo->findData(family);
			if (item < 0) {
				// 一覧にないフォントの名前 (インストールされていないフォント) も、そのまま持つ。
				knob.combo->addItem(family + QStringLiteral(" (インストールされていません)"), family);
				item = knob.combo->count() - 1;
			}
			knob.combo->setCurrentIndex(item);
			break;
		}
		case KnobKind::Choice:
			break; // 下の update_choice_states が入れる
		}
	}
	update_choice_states();
	loading_ = false;
	refresh_hints();
}

void TunerWindow::update_choice_states()
{
	const bool was_loading = loading_;
	loading_ = true;
	for (KnobWidgets &knob : knobs_) {
		if (knob.def->kind != KnobKind::Choice)
			continue;
		knob.combo->setCurrentIndex(knob.def->get_choice(settings_));
		// 現在の配置に合わない選択肢は、押せなくする。
		auto *model = qobject_cast<QStandardItemModel *>(knob.combo->model());
		if (model == nullptr)
			continue;
		for (int item = 0; item < knob.combo->count(); ++item) {
			const bool allowed = !knob.def->choice_allowed || knob.def->choice_allowed(settings_, item);
			if (QStandardItem *entry = model->item(item))
				entry->setEnabled(allowed);
		}
	}
	loading_ = was_loading;
}

decor_preview::SceneRequest TunerWindow::make_request() const
{
	decor_preview::SceneRequest request;
	request.canvas_w = kCanvasWidth;
	request.canvas_h = kCanvasHeight;
	request.settings = settings_;
	for (int seat = 0; seat < state_.people; ++seat) {
		const SeatState &s = state_.seats[static_cast<std::size_t>(seat)];
		request.seats.push_back({s.person});
	}
	for (int row = 0; row < state_.roster; ++row)
		request.roster.push_back({state_.roster_rows[static_cast<std::size_t>(row)].person, true});
	request.roster_last_unheard = state_.roster_last_unheard;
	return request;
}

sources::BorderTiming TunerWindow::timing() const
{
	// 合成のソースと同じく、設定から作った値 (sources::DecorLabelParams) の時間を使う。
	const sources::DecorLabelParams params = sources::decor_label_params_from_settings(settings_);
	sources::BorderTiming timing;
	timing.rise_ms = params.border_rise_ms;
	timing.hold_ms = params.border_hold_ms;
	timing.fade_ms = params.border_fade_ms;
	return timing;
}

void TunerWindow::on_scene_changed()
{
	if (loading_)
		return;
	std::string error;
	if (!knobs_within_range(settings_, &error))
		note_ = QStringLiteral("つまみの値の誤り: %1").arg(QString::fromStdString(error));
	renderer_.set_request(make_request());
	rebuild_view_choices();
	update_seat_rows();
	refresh_hints();
	dirty_ = true;
}

void TunerWindow::refresh_hints()
{
	for (KnobWidgets &knob : knobs_) {
		const QString hint = QString::fromStdString(knob_hint(*knob.def, settings_));
		for (QWidget *widget : {static_cast<QWidget *>(knob.label), static_cast<QWidget *>(knob.slider),
					static_cast<QWidget *>(knob.spin)})
			if (widget != nullptr)
				widget->setToolTip(hint);
	}
}

void TunerWindow::rebuild_view_choices()
{
	const decor_preview::ScenePlan &plan = renderer_.plan();
	loading_ = true;
	view_combo_->clear();
	for (std::size_t i = 0; i < plan.layout.tiles.size(); ++i) {
		const int person = plan.tile_person[i];
		view_combo_->addItem(QStringLiteral("%1: %2").arg(
			tile_title(plan, i),
			QString::fromUtf8(decor_preview::scene_people()[static_cast<std::size_t>(person)].name)));
	}
	const int tiles = view_combo_->count();
	for (std::size_t r = 0; r < plan.roster_person.size(); ++r) {
		view_combo_->addItem(QStringLiteral("画面共有していない参加者 %1: %2")
					     .arg(r + 1)
					     .arg(QString::fromUtf8(
						     decor_preview::scene_people()[static_cast<std::size_t>(plan.roster_person[r])]
							     .name)));
	}
	// 並びの人を見ていて、その人がいなくなったら、タイルに戻す。
	if (state_.view_roster >= static_cast<int>(plan.roster_person.size()))
		state_.view_roster = -1;
	state_.view_tile = std::clamp(state_.view_tile, 0, std::max(0, tiles - 1));
	view_combo_->setCurrentIndex(state_.view_roster >= 0 ? tiles + state_.view_roster : state_.view_tile);
	loading_ = false;
}

void TunerWindow::update_seat_rows()
{
	for (int seat = 0; seat < sources::kCompositeCellCount; ++seat)
		rows_[static_cast<std::size_t>(seat)].box->setEnabled(seat < state_.people);
	for (int row = 0; row < kRosterRows; ++row)
		roster_rows_[static_cast<std::size_t>(row)].box->setEnabled(row < state_.roster);
}

void TunerWindow::set_roster_speaking(int row, bool cycle, bool on, std::uint64_t at_ns)
{
	if (row < 0 || row >= kRosterRows)
		return;
	RosterRow &widgets = roster_rows_[static_cast<std::size_t>(row)];
	SpeakTrack &track = tracks_[static_cast<std::size_t>(sources::kCompositeCellCount + row)];
	loading_ = true;
	if (cycle) {
		track.set_cycle(on, at_ns, timing());
		widgets.cycle->setChecked(on);
	} else {
		track.set_toggle(on, at_ns, timing());
		widgets.speak->setChecked(on);
	}
	loading_ = false;
}

void TunerWindow::set_speaking(int seat, bool cycle, bool on, std::uint64_t at_ns)
{
	if (seat < 0 || seat >= sources::kCompositeCellCount)
		return;
	SeatRow &row = rows_[static_cast<std::size_t>(seat)];
	SpeakTrack &track = tracks_[static_cast<std::size_t>(seat)];
	loading_ = true;
	if (cycle) {
		track.set_cycle(on, at_ns, timing());
		row.cycle->setChecked(on);
	} else {
		track.set_toggle(on, at_ns, timing());
		row.speak->setChecked(on);
	}
	loading_ = false;
}

void TunerWindow::rebuild_background_if_needed()
{
	const decor_preview::ScenePlan &plan = renderer_.plan();
	std::vector<QRect> cells;
	for (const sources::CompositeTile &tile : plan.layout.tiles)
		cells.push_back(QRect(tile.rect.x, tile.rect.y, tile.rect.w, tile.rect.h));
	if (!background_dirty_ && cells == background_cells_ && !background_.isNull())
		return;
	background_cells_ = cells;
	background_dirty_ = false;
	background_ = QImage(kCanvasWidth, kCanvasHeight, QImage::Format_ARGB32_Premultiplied);
	background_.fill(Qt::black);
	QPainter painter(&background_);
	for (std::size_t i = 0; i < plan.layout.tiles.size(); ++i) {
		const sources::PixelRect &cell = plan.layout.tiles[i].rect;
		if (background_mode_ == BackgroundMode::Picture && !picture_.isNull()) {
			// 製品が画面共有の映像を枠に置くのと同じく、縦横比を保って収め、余りは黒にする。
			const sources::PixelRect fit = sources::fit_inside(cell, picture_.width(), picture_.height());
			painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
			painter.drawImage(QRect(fit.x, fit.y, fit.w, fit.h), picture_);
			continue;
		}
		const int kind =
			background_mode_ == BackgroundMode::Kind
				? background_kind_
				: static_cast<int>(std::min<std::size_t>(i, decor_preview::kBackgroundKindCount - 1));
		painter.drawImage(cell.x, cell.y, decor_preview::make_scene_background(kind, cell.w, cell.h));
	}
	dirty_ = true;
}

void TunerWindow::tick()
{
	const std::uint64_t now = clock_();
	const sources::BorderTiming border_timing = timing();
	for (SpeakTrack &track : tracks_)
		track.advance(now, border_timing);

	bool changed = renderer_.poll();
	if (options_.shot) {
		renderer_.paint_pending_now();
		changed = renderer_.poll() || changed;
	}
	rebuild_background_if_needed();

	const decor_preview::ScenePlan &plan = renderer_.plan();
	std::vector<double> levels(plan.layout.tiles.size(), 0.0);
	std::vector<double> dims(plan.layout.tiles.size(), 1.0);
	for (std::size_t i = 0; i < plan.layout.tiles.size(); ++i) {
		const auto seat = static_cast<std::size_t>(plan.tile_seat[i]);
		levels[i] = tracks_[seat].level(now, border_timing, plan.params.show_speaking);
		// 暗さは光の表示のオンとオフには依らず、cordmux がその人の声を聞けるときだけ動作する。
		dims[i] = sources::composite_label_dim_factor(tracks_[seat].dim_level(now, border_timing), plan.dim_pct,
							      plan.tile_hears_voice[i]);
	}
	std::vector<double> roster_levels(plan.roster_member.size(), 0.0);
	std::vector<double> roster_dims(plan.roster_member.size(), 1.0);
	for (std::size_t r = 0; r < plan.roster_member.size(); ++r) {
		const auto track = static_cast<std::size_t>(plan.roster_track[r]);
		roster_levels[r] = tracks_[track].level(now, border_timing, plan.params.show_speaking);
		roster_dims[r] = sources::composite_label_dim_factor(tracks_[track].dim_level(now, border_timing),
								     plan.dim_pct, plan.roster_hears_voice[r]);
	}
	const bool scene_changed = changed || dirty_ || levels != levels_ || dims != dims_ ||
				   roster_levels != roster_levels_ || roster_dims != roster_dims_;
	// 等倍と 2 倍で見せる範囲は、置いている名前ラベルの位置で決まる。範囲か表示の大きさが変わったら、名前ラベルが
	// 見えるようにスクロールし直す。見えている所が変わったら (手でスクロールしたときも)、その所だけを重ね直す。
	const ViewTarget target = view_target();
	if (target.extent != crop_) {
		crop_ = target.extent;
		one_->set_extent(target.extent);
		two_->set_extent(target.extent);
	}
	ScrollGoal goal;
	goal.extent = target.extent;
	goal.label = target.label;
	goal.cell_corner = target.cell_corner;
	goal.corner = target.corner;
	goal.one_port = one_scroll_->viewport()->size();
	goal.two_port = two_scroll_->viewport()->size();
	goal.dpr = devicePixelRatioF();
	if (goal != scroll_goal_) {
		scroll_goal_ = goal;
		scroll_pending_ = true;
	}
	if (scroll_pending_)
		apply_scroll_goal();
	const QRect region = visible_crop_region();
	std::vector<decor_preview::SceneTileImages> tiles;
	std::vector<decor_preview::SceneRosterImages> roster;
	if (scene_changed || region != crop_region_) {
		tiles = renderer_.shown_images(levels, dims);
		roster = renderer_.shown_roster(roster_levels, roster_dims);
	}
	if (scene_changed) {
		levels_ = levels;
		dims_ = dims;
		roster_levels_ = roster_levels;
		roster_dims_ = roster_dims;
		dirty_ = false;
		++compose_serial_;
		request_full_compose(tiles, roster);
	}
	if (scene_changed || region != crop_region_)
		request_crop_compose(tiles, roster, region);
	if (scene_changed) {
		for (std::size_t i = 0; i < plan.layout.tiles.size(); ++i) {
			const auto seat = static_cast<std::size_t>(plan.tile_seat[i]);
			rows_[seat].info->setText(QStringLiteral("%1の枠 光 %2 暗さ %3 %")
							  .arg(tile_title(plan, i))
							  .arg(levels_[i], 0, 'f', 2)
							  .arg(std::lround((1.0 - dims_[i]) * 100.0)));
		}
		for (int seat = state_.people; seat < sources::kCompositeCellCount; ++seat)
			rows_[static_cast<std::size_t>(seat)].info->setText(QStringLiteral("(人数の外)"));
		for (int row = 0; row < state_.roster; ++row)
			roster_rows_[static_cast<std::size_t>(row)].info->setText(QStringLiteral("(表示しない設定)"));
		for (std::size_t r = 0; r < plan.roster_member.size(); ++r) {
			// 名前ラベルを隠す全面の 1 枚の席の人 (roster_member が -1) は、並びの行を持たない。
			if (plan.roster_member[r] < 0)
				continue;
			const auto row = static_cast<std::size_t>(plan.roster_member[r]);
			roster_rows_[row].info->setText(
				QStringLiteral("並びの %1 番目 光 %2 暗さ %3 %%4")
					.arg(r + 1)
					.arg(roster_levels_[r], 0, 'f', 2)
					.arg(std::lround((1.0 - roster_dims_[r]) * 100.0))
					.arg(plan.roster_hears_voice[r] ? QString() : QStringLiteral(" (声を聞けない)")));
		}
		for (int row = state_.roster; row < kRosterRows; ++row)
			roster_rows_[static_cast<std::size_t>(row)].info->setText(QStringLiteral("(人数の外)"));
	}
	if (options_.shot) {
		full_worker_.run_pending();
		crop_worker_.run_pending();
	}
	take_composed();
	update_status();
	if (tick_observer_) {
		TickInfo info;
		info.now_ns = now;
		for (std::size_t i = 0; i < plan.layout.tiles.size(); ++i) {
			const auto seat = static_cast<std::size_t>(plan.tile_seat[i]);
			info.seat_level[seat] = levels[i];
			info.seat_shown[seat] = true;
		}
		tick_observer_(info);
	}
}

bool TunerWindow::all_current() const
{
	return renderer_.all_current() && !dirty_ && !scroll_pending_ && last_shown_serial_full_ == compose_serial_ &&
	       last_shown_serial_crop_ == compose_serial_ && crop_region_shown_ == crop_region_;
}

void TunerWindow::apply_scroll_goal()
{
	const ScrollGoal &goal = scroll_goal_;
	bool done = true;
	for (const int scale : {1, 2}) {
		const PixelView *view = scale == 1 ? one_ : two_;
		QScrollArea *scroll = scale == 1 ? one_scroll_ : two_scroll_;
		const QSize port = scroll->viewport()->size();
		const double per_logical = goal.dpr / scale; // 論理ピクセル 1 つに入るキャンバスのピクセル
		int values[2] = {0, 0};
		if (!goal.extent.isEmpty() && !goal.label.isEmpty()) {
			const bool left = goal.corner == sources::DecorCorner::TopLeft ||
					  goal.corner == sources::DecorCorner::BottomLeft;
			const bool top = goal.corner == sources::DecorCorner::TopLeft ||
					 goal.corner == sources::DecorCorner::TopRight;
			const double sx = scroll_start(port.width() * per_logical, goal.label.left(),
						       goal.label.right() + 1, goal.cell_corner.x(), left);
			const double sy = scroll_start(port.height() * per_logical, goal.label.top(),
						       goal.label.bottom() + 1, goal.cell_corner.y(), top);
			values[0] = static_cast<int>(std::lround((sx - goal.extent.x()) / per_logical));
			values[1] = static_cast<int>(std::lround((sy - goal.extent.y()) / per_logical));
		}
		// スクロールエリアがウィジェットの新しい大きさを知った後でないと、範囲が古いまま値が切り詰められる。範囲が
		// ウィジェットの大きさと合うまで、次の tick でもう一度試す。
		const int max_x = std::max(0, view->width() - port.width());
		const int max_y = std::max(0, view->height() - port.height());
		QScrollBar *bars[2] = {scroll->horizontalScrollBar(), scroll->verticalScrollBar()};
		const int maxes[2] = {max_x, max_y};
		for (int axis = 0; axis < 2; ++axis) {
			const int want = std::clamp(values[axis], 0, maxes[axis]);
			if (bars[axis]->maximum() != maxes[axis])
				done = false;
			bars[axis]->setValue(want);
			if (bars[axis]->value() != want)
				done = false;
		}
	}
	scroll_pending_ = !done;
}

void TunerWindow::request_full_compose(const std::vector<decor_preview::SceneTileImages> &tiles,
				       const std::vector<decor_preview::SceneRosterImages> &roster)
{
	ComposeJob job;
	job.serial = compose_serial_;
	job.background = background_;
	job.plan = renderer_.plan_ptr();
	job.tiles = tiles;
	job.roster = roster;
	job.region = background_.rect();
	full_worker_.submit(std::move(job));
}

void TunerWindow::request_crop_compose(const std::vector<decor_preview::SceneTileImages> &tiles,
				       const std::vector<decor_preview::SceneRosterImages> &roster, const QRect &region)
{
	crop_region_ = region;
	if (region.isEmpty()) {
		// 見えている所がない (タイルがないか、表示が隠れている) ときは、重ねない。
		last_shown_serial_crop_ = compose_serial_;
		crop_region_shown_ = region;
		one_->set_patch(QRect(), QImage());
		two_->set_patch(QRect(), QImage());
		return;
	}
	ComposeJob job;
	job.serial = compose_serial_;
	job.background = background_;
	job.plan = renderer_.plan_ptr();
	job.tiles = tiles;
	job.roster = roster;
	job.region = region;
	crop_worker_.submit(std::move(job));
}

QRect TunerWindow::visible_crop_region() const
{
	const auto visible = [](const PixelView *view, const QScrollArea *scroll) {
		const QRect port(view->mapFrom(scroll->viewport(), QPoint(0, 0)), scroll->viewport()->size());
		return view->visible_canvas_rect(port);
	};
	return visible(one_, one_scroll_) | visible(two_, two_scroll_);
}

bool TunerWindow::take_composed()
{
	bool taken = false;
	std::optional<ComposeResult> full = full_worker_.take();
	if (full && full->image.isNull())
		full.reset();
	if (full) {
		canvas_ = full->image;
		full_compose_ms_ = full->compose_ms;
		last_shown_serial_full_ = full->serial;
		fit_->set_canvas(canvas_);
		taken = true;
	}
	std::optional<ComposeResult> crop = crop_worker_.take();
	if (crop && crop->image.isNull())
		crop.reset();
	if (crop) {
		crop_compose_ms_ = crop->compose_ms;
		last_shown_serial_crop_ = crop->serial;
		crop_region_shown_ = crop->region;
		one_->set_patch(crop->region, crop->image);
		two_->set_patch(crop->region, crop->image);
		taken = true;
	}
	return taken;
}

TunerWindow::ViewTarget TunerWindow::view_target() const
{
	ViewTarget target;
	const decor_preview::ScenePlan &plan = renderer_.plan();
	if (background_.isNull())
		return target;
	if (state_.view_roster >= 0 && state_.view_roster < static_cast<int>(plan.roster_person.size())) {
		// 画面共有していない参加者を見る。枠はないので、名前ラベルの範囲だけを見せる。隅は、配置に近い側にする
		// (右の配置は右上か右下)。
		using Anchor = config::RosterAnchor;
		const auto r = static_cast<std::size_t>(state_.view_roster);
		const Anchor anchor = plan.roster_params.anchor;
		const bool left = anchor != Anchor::TopRight && anchor != Anchor::MiddleRight && anchor != Anchor::BottomRight;
		const bool top = anchor != Anchor::BottomLeft && anchor != Anchor::BottomCenter &&
				 anchor != Anchor::BottomRight;
		QRect area = renderer_.roster_label_area(r);
		const QRect label = renderer_.roster_label_rect(r);
		if (area.isEmpty()) {
			area = QRect(left ? 0 : kCanvasWidth - kEmptyCropWidth, top ? 0 : kCanvasHeight - kEmptyCropHeight,
				     kEmptyCropWidth, kEmptyCropHeight);
		}
		const QRect reference = label.isEmpty() ? area : label;
		const QPoint corner_point(left ? reference.left() : reference.right(), top ? reference.top() : reference.bottom());
		// 切り抜きは、並びのまとまり全体 (全員の名前ラベルと光) の外接矩形をフォントサイズ 1 つ分広げた範囲で、キャンバスで切る。
		// 表示より大きければ、選んだ人が見える位置にスクロールする。
		QRect block = area;
		for (std::size_t other = 0; other < plan.roster_person.size(); ++other)
			block |= renderer_.roster_label_area(other);
		const int pad = std::max(1, static_cast<int>(std::lround(plan.name_px)));
		target.valid = true;
		target.extent = block.adjusted(-pad, -pad, pad, pad).intersected(background_.rect());
		target.label = label;
		target.cell_corner = corner_point;
		target.corner = top ? (left ? sources::DecorCorner::TopLeft : sources::DecorCorner::TopRight)
				    : (left ? sources::DecorCorner::BottomLeft : sources::DecorCorner::BottomRight);
		return target;
	}
	if (plan.layout.tiles.empty())
		return target;
	const std::size_t tile = static_cast<std::size_t>(
		std::clamp(state_.view_tile, 0, static_cast<int>(plan.layout.tiles.size()) - 1));
	const sources::PixelRect &cell = plan.layout.tiles[tile].rect;
	const sources::DecorCorner corner = plan.tile_content[tile].corner;
	const bool left = corner == sources::DecorCorner::TopLeft || corner == sources::DecorCorner::BottomLeft;
	const bool top = corner == sources::DecorCorner::TopLeft || corner == sources::DecorCorner::TopRight;
	// 名前ラベルと光の範囲に、名前ラベルの側の枠の隅を含める。枠の境 (光が隣の枠へ出るか、枠の線) も見えるように
	// するためである。
	const QPoint corner_point(left ? cell.x : cell.x + cell.w - 1, top ? cell.y : cell.y + cell.h - 1);
	QRect area = renderer_.label_area(tile);
	if (area.isEmpty()) {
		area = QRect(left ? cell.x : cell.x + cell.w - kEmptyCropWidth,
			     top ? cell.y : cell.y + cell.h - kEmptyCropHeight, kEmptyCropWidth, kEmptyCropHeight);
	}
	area |= QRect(corner_point, QSize(1, 1));
	target.valid = true;
	target.extent = area.adjusted(-kCropPad, -kCropPad, kCropPad, kCropPad).intersected(background_.rect());
	target.label = renderer_.label_rect(tile);
	target.cell_corner = corner_point;
	target.corner = corner;
	return target;
}

void TunerWindow::update_status()
{
	const int pending = renderer_.pending_count();
	QString text;
	if (pending > 0) {
		text = QStringLiteral("描き直し中 (残り %1 枚)。描き終えるまで、前の値で描いた画像を表示しています。")
			       .arg(pending);
	} else {
		text = QStringLiteral("表示は、現在のつまみの値で描いた画像です (直近の描き直し %1 ms、重ね %2 ms)。")
			       .arg(renderer_.last_paint_ms(), 0, 'f', 0)
			       .arg(full_compose_ms_, 0, 'f', 0);
	}
	if (!note_.isEmpty())
		text += QStringLiteral("  ") + note_;
	if (status_->text() != text)
		status_->setText(text);
}

void TunerWindow::open_picture()
{
	const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("背景の画像を開く"), QString(),
							  QStringLiteral("画像 (*.png *.jpg *.jpeg *.bmp)"));
	loading_ = true;
	if (!path.isEmpty()) {
		const QImage image = QImage(path).convertToFormat(QImage::Format_ARGB32_Premultiplied);
		if (image.isNull()) {
			note_ = QStringLiteral("背景の画像を読めません: %1").arg(path);
		} else {
			picture_ = image;
			state_.picture_path = path;
			picture_label_->setText(QFileInfo(path).fileName());
			background_mode_ = BackgroundMode::Picture;
			background_dirty_ = true;
		}
	}
	background_combo_->setCurrentIndex(background_mode_ == BackgroundMode::Mixed  ? 0
					   : background_mode_ == BackgroundMode::Kind ? 1 + background_kind_
										      : kPictureChoice);
	loading_ = false;
}

void TunerWindow::save()
{
	if (options_.save_path.isEmpty())
		return;
	std::string existing;
	QFile current(options_.save_path);
	if (current.exists()) {
		if (!current.open(QIODevice::ReadOnly)) {
			note_ = QStringLiteral("保存できません (保存先を読めません): %1").arg(options_.save_path);
			return;
		}
		existing = current.readAll().toStdString();
		current.close();
	}
	std::string text;
	std::string error;
	if (!merge_knobs_into(existing, settings_, &text, &error)) {
		note_ = QStringLiteral("保存しませんでした: %1 (%2)")
				.arg(QString::fromStdString(error), options_.save_path);
		return;
	}
	QSaveFile out(options_.save_path);
	if (!out.open(QIODevice::WriteOnly) ||
	    out.write(text.data(), static_cast<qint64>(text.size())) != static_cast<qint64>(text.size()) ||
	    !out.commit()) {
		note_ = QStringLiteral("保存できません: %1").arg(options_.save_path);
		return;
	}
	note_ = QStringLiteral("保存しました: %1").arg(options_.save_path);
}

void TunerWindow::reset_to_defaults()
{
	settings_ = config::Settings{};
	load_knob_widgets();
	note_ = QStringLiteral("つまみを製品の既定値に戻しました。");
	on_scene_changed();
}

bool TunerWindow::set_knob_number(const std::string &key, double value, bool via_slider)
{
	for (KnobWidgets &knob : knobs_) {
		if (knob.def->key != key || knob.def->kind != KnobKind::Number)
			continue;
		if (via_slider)
			knob.slider->setValue(static_cast<int>(std::lround(value * knob.def->divisor)));
		else
			knob.spin->setValue(value);
		return true;
	}
	return false;
}

bool TunerWindow::set_knob_toggle(const std::string &key, bool on)
{
	for (KnobWidgets &knob : knobs_) {
		if (knob.def->key != key || knob.def->kind != KnobKind::Toggle)
			continue;
		knob.check->setChecked(on);
		return true;
	}
	return false;
}

bool TunerWindow::set_knob_text(const std::string &key, const QString &text)
{
	for (KnobWidgets &knob : knobs_) {
		if (knob.def->key != key)
			continue;
		switch (knob.def->kind) {
		case KnobKind::Color:
			// 色の欄に打ち込んで確定したのと同じ経路。
			knob.hex->setText(text);
			Q_EMIT knob.hex->editingFinished();
			return true;
		case KnobKind::Font: {
			const int item = knob.combo->findData(text);
			if (item < 0)
				return false;
			knob.combo->setCurrentIndex(item);
			return true;
		}
		default:
			return false;
		}
	}
	return false;
}

void TunerWindow::reset_knobs()
{
	reset_button_->click();
}

void TunerWindow::press_save()
{
	save_button_->click();
}

void TunerWindow::set_people(int count)
{
	people_spin_->setValue(count);
}

QPushButton *TunerWindow::hold_button(int seat) const
{
	return seat >= 0 && seat < sources::kCompositeCellCount ? rows_[static_cast<std::size_t>(seat)].hold : nullptr;
}

QCheckBox *TunerWindow::speak_box(int seat) const
{
	return seat >= 0 && seat < sources::kCompositeCellCount ? rows_[static_cast<std::size_t>(seat)].speak : nullptr;
}

QCheckBox *TunerWindow::cycle_box(int seat) const
{
	return seat >= 0 && seat < sources::kCompositeCellCount ? rows_[static_cast<std::size_t>(seat)].cycle : nullptr;
}

QCheckBox *TunerWindow::knob_check(const std::string &key) const
{
	for (const KnobWidgets &knob : knobs_)
		if (knob.def->key == key && knob.check)
			return knob.check;
	return nullptr;
}

void TunerWindow::set_background_choice(int index)
{
	if (index >= 0 && index < kPictureChoice)
		background_combo_->setCurrentIndex(index);
}

int TunerWindow::fit_paints() const
{
	return fit_->paint_count();
}

int TunerWindow::pixel_paints() const
{
	return one_->paint_count() + two_->paint_count();
}

void TunerWindow::set_view_tile(int tile)
{
	const int tiles = static_cast<int>(renderer_.plan().layout.tiles.size());
	view_combo_->setCurrentIndex(std::clamp(tile, 0, std::max(0, tiles - 1)));
}

void TunerWindow::set_view_roster(int row)
{
	const int tiles = static_cast<int>(renderer_.plan().layout.tiles.size());
	const int rows = static_cast<int>(renderer_.plan().roster_person.size());
	if (row < 0 || row >= rows) {
		set_view_tile(state_.view_tile);
		return;
	}
	view_combo_->setCurrentIndex(tiles + row);
}

void TunerWindow::set_roster_people(int count)
{
	roster_spin_->setValue(count);
}

void TunerWindow::set_roster_last_unheard(bool on)
{
	roster_unheard_->setChecked(on);
}

bool TunerWindow::set_knob_choice(const std::string &key, int index)
{
	for (KnobWidgets &knob : knobs_) {
		if (knob.def->key != key || knob.def->kind != KnobKind::Choice)
			continue;
		if (index < 0 || index >= knob.combo->count())
			return false;
		if (knob.def->choice_allowed && !knob.def->choice_allowed(settings_, index))
			return false;
		knob.combo->setCurrentIndex(index);
		return true;
	}
	return false;
}

void TunerWindow::focus_knob(const std::string &key)
{
	for (const KnobWidgets &knob : knobs_) {
		if (knob.def->key != key)
			continue;
		QWidget *widget = knob.check    ? static_cast<QWidget *>(knob.check)
				  : knob.slider ? static_cast<QWidget *>(knob.slider)
				  : knob.hex    ? static_cast<QWidget *>(knob.hex)
						: static_cast<QWidget *>(knob.combo);
		knob_scroll_->ensureWidgetVisible(widget, 20, 80);
		focus_key_ = key;
		return;
	}
}

long long TunerWindow::fresh_mismatch() const
{
	if (canvas_.isNull() || background_.isNull())
		return -1;
	// 見本 (decor_preview --composite-sample) と同じく、その場で描いて重ねる。
	const decor_preview::ScenePlan plan = decor_preview::plan_scene(make_request());
	std::vector<decor_preview::SceneTileImages> tiles;
	for (std::size_t i = 0; i < plan.layout.tiles.size(); ++i) {
		decor_preview::SceneTileImages images;
		images.label = decor_preview::paint_scene_label(plan, i);
		images.label_owner = plan.tile_owner[i];
		images.frame = decor_preview::paint_scene_frame(plan, i);
		images.level = i < levels_.size() ? levels_[i] : 0.0;
		images.dim = i < dims_.size() ? dims_[i] : 1.0;
		tiles.push_back(images);
	}
	std::vector<decor_preview::SceneRosterImages> roster;
	for (std::size_t r = 0; r < plan.roster_content.size(); ++r) {
		decor_preview::SceneRosterImages images;
		images.label = decor_preview::paint_scene_roster_label(plan, r);
		images.label_owner = plan.roster_owner[r];
		images.level = r < roster_levels_.size() ? roster_levels_[r] : 0.0;
		images.dim = r < roster_dims_.size() ? roster_dims_[r] : 1.0;
		roster.push_back(images);
	}
	QImage fresh = background_.copy();
	decor_preview::draw_scene(&fresh, plan, tiles, roster);
	if (fresh.size() != canvas_.size())
		return -1;
	long long differ = 0;
	for (int y = 0; y < fresh.height(); ++y) {
		const auto *a = reinterpret_cast<const QRgb *>(fresh.constScanLine(y));
		const auto *b = reinterpret_cast<const QRgb *>(canvas_.constScanLine(y));
		for (int x = 0; x < fresh.width(); ++x)
			if (a[x] != b[x])
				++differ;
	}
	return differ;
}

QJsonArray TunerWindow::clipped_widgets() const
{
	QJsonArray out;
	const auto scan = [&](const QWidget *widget, const QString &text, const QString &kind) {
		if (!widget->isVisibleTo(this) || widget->width() <= 0)
			return;
		if (widget->sizeHint().width() <= widget->width())
			return;
		// 見えているか。スクロールエリアの中の物は、その表示の範囲と重なるときだけ数える。
		const QRect rect(widget->mapTo(this, QPoint(0, 0)), widget->size());
		for (const QWidget *p = widget->parentWidget(); p != nullptr; p = p->parentWidget()) {
			const auto *area = qobject_cast<const QAbstractScrollArea *>(p);
			if (area == nullptr)
				continue;
			const QRect port(area->viewport()->mapTo(this, QPoint(0, 0)), area->viewport()->size());
			if (!port.intersects(rect))
				return;
		}
		QJsonObject o;
		o[QStringLiteral("class")] = kind;
		o[QStringLiteral("text")] = text;
		o[QStringLiteral("width")] = widget->width();
		o[QStringLiteral("hint")] = widget->sizeHint().width();
		out.append(o);
	};
	for (const QLabel *w : findChildren<QLabel *>())
		if (!w->wordWrap())
			scan(w, w->text(), QStringLiteral("QLabel"));
	for (const QCheckBox *w : findChildren<QCheckBox *>())
		scan(w, w->text(), QStringLiteral("QCheckBox"));
	for (const QPushButton *w : findChildren<QPushButton *>())
		scan(w, w->text(), QStringLiteral("QPushButton"));
	for (const QComboBox *w : findChildren<QComboBox *>())
		scan(w, w->currentText(), QStringLiteral("QComboBox"));
	return out;
}

QJsonObject TunerWindow::describe(double dpr) const
{
	QJsonObject root;
	root[QStringLiteral("clipped_widgets")] = clipped_widgets();
	root[QStringLiteral("dpr")] = dpr;
	root[QStringLiteral("all_current")] = all_current();
	root[QStringLiteral("settings")] =
		QJsonDocument::fromJson(QByteArray::fromStdString(knobs_to_json(settings_))).object();
	root[QStringLiteral("painted_settings")] =
		QJsonDocument::fromJson(QByteArray::fromStdString(knobs_to_json(renderer_.request().settings))).object();

	// 画面のつまみが表示している値。数のつまみはスライダーの位置から求めた値も書く。
	QJsonObject widgets;
	for (const KnobWidgets &knob : knobs_) {
		const QString key = QString::fromLatin1(knob.def->key);
		switch (knob.def->kind) {
		case KnobKind::Toggle:
			widgets[key] = knob.check->isChecked();
			break;
		case KnobKind::Number:
			widgets[key] = QJsonArray{knob.spin->value(),
						  static_cast<double>(knob.slider->value()) / knob.def->divisor,
						  knob.spin->text()};
			break;
		case KnobKind::Color:
			widgets[key] = knob.hex->text();
			break;
		case KnobKind::Font:
			widgets[key] = knob.combo->currentData().toString();
			break;
		case KnobKind::Choice:
			widgets[key] = QJsonArray{knob.combo->currentIndex(), knob.combo->currentText()};
			break;
		}
	}
	root[QStringLiteral("widgets")] = widgets;

	// 画面のつまみに付いているヒント (マウスを載せると出る文)。
	QJsonObject hints;
	for (const KnobWidgets &knob : knobs_) {
		const QWidget *widget = knob.slider ? static_cast<const QWidget *>(knob.slider) : knob.label;
		if (widget != nullptr && !widget->toolTip().isEmpty())
			hints[QString::fromLatin1(knob.def->key)] = widget->toolTip();
	}
	root[QStringLiteral("hints")] = hints;

	const decor_preview::ScenePlan &plan = renderer_.plan();
	root[QStringLiteral("name_px")] = plan.name_px;
	root[QStringLiteral("margin_px")] = plan.margin_px;
	root[QStringLiteral("hide")] = plan.hide;
	QJsonArray tiles;
	for (std::size_t i = 0; i < plan.layout.tiles.size(); ++i) {
		const sources::PixelRect &r = plan.layout.tiles[i].rect;
		QJsonObject t;
		t[QStringLiteral("rect")] = QJsonArray{r.x, r.y, r.w, r.h};
		t[QStringLiteral("seat")] = plan.tile_seat[i];
		t[QStringLiteral("person")] = QString::fromLatin1(
			decor_preview::scene_people()[static_cast<std::size_t>(plan.tile_person[i])].id);
		t[QStringLiteral("level")] = i < levels_.size() ? levels_[i] : 0.0;
		t[QStringLiteral("dim")] = i < dims_.size() ? dims_[i] : 1.0;
		t[QStringLiteral("hears_voice")] = plan.tile_hears_voice[i];
		const QRect area = renderer_.label_area(i);
		t[QStringLiteral("label_area")] = QJsonArray{area.x(), area.y(), area.width(), area.height()};
		if (const sources::CompositeLabel *label = renderer_.shown_label(i)) {
			t[QStringLiteral("name_drawn")] = label->name_drawn;
			t[QStringLiteral("elided")] = label->elided;
			t[QStringLiteral("avatar_dropped")] = label->avatar_dropped;
			t[QStringLiteral("name_hidden")] = label->name_hidden;
			t[QStringLiteral("initial_drawn")] = label->initial_drawn;
		}
		tiles.append(t);
	}
	root[QStringLiteral("tiles")] = tiles;

	// 画面共有していない参加者。並べた順に、人、明るさ、色の倍率、声を聞けるか、置いた名前ラベルの範囲、置けなかった人の数。
	QJsonArray roster;
	for (std::size_t r = 0; r < plan.roster_person.size(); ++r) {
		QJsonObject p;
		p[QStringLiteral("person")] = QString::fromLatin1(
			decor_preview::scene_people()[static_cast<std::size_t>(plan.roster_person[r])].id);
		p[QStringLiteral("row")] = plan.roster_member[r];
		p[QStringLiteral("level")] = r < roster_levels_.size() ? roster_levels_[r] : 0.0;
		p[QStringLiteral("dim")] = r < roster_dims_.size() ? roster_dims_[r] : 1.0;
		p[QStringLiteral("hears_voice")] = plan.roster_hears_voice[r];
		const QRect label = renderer_.roster_label_rect(r);
		p[QStringLiteral("label_rect")] = QJsonArray{label.x(), label.y(), label.width(), label.height()};
		const QRect area = renderer_.roster_label_area(r);
		p[QStringLiteral("label_area")] = QJsonArray{area.x(), area.y(), area.width(), area.height()};
		if (const sources::CompositeLabel *shown = renderer_.shown_roster_label(r)) {
			p[QStringLiteral("name_drawn")] = shown->name_drawn;
			p[QStringLiteral("elided")] = shown->elided;
			p[QStringLiteral("avatar_dropped")] = shown->avatar_dropped;
			p[QStringLiteral("name_hidden")] = shown->name_hidden;
			p[QStringLiteral("initial_drawn")] = shown->initial_drawn;
			p[QStringLiteral("box")] = QJsonArray{shown->box_w, shown->box_h};
		}
		roster.append(p);
	}
	root[QStringLiteral("roster")] = roster;
	root[QStringLiteral("roster_not_placed")] = renderer_.roster_not_placed();
	root[QStringLiteral("dim_pct")] = plan.dim_pct;

	// 等倍と 2 倍の表示の、ウィンドウの画像の中での位置 (画像のピクセル)。origin は切り抜きの左上を描いた位置、
	// visible はスクロールエリアの中で見えている範囲。名前ラベル (光を含めない) が見えている範囲に全部入るか
	// (label_inside)、表示より大きくて入りきらないとき (label_fits が偽) は、隅の側の角が入るか (label_corner_inside)
	// と、名前ラベルの側の枠の隅が入るか (cell_corner_inside) も書く。
	const ViewTarget target = view_target();
	const auto view_json = [this, dpr, &target](const PixelView *view, const QScrollArea *scroll, int scale) {
		const QPoint at = view->mapTo(this, QPoint(0, 0));
		const QPoint port = scroll->viewport()->mapTo(this, QPoint(0, 0));
		const QRect visible = QRect(at, view->size()).intersected(QRect(port, scroll->viewport()->size()));
		const auto device = [dpr](double v) {
			return static_cast<int>(std::lround(v * dpr));
		};
		const QRect visible_device(device(visible.x()), device(visible.y()),
					   device(visible.x() + visible.width()) - device(visible.x()),
					   device(visible.y() + visible.height()) - device(visible.y()));
		QJsonObject v;
		v[QStringLiteral("origin")] = QJsonArray{device(at.x()), device(at.y())};
		v[QStringLiteral("visible")] = QJsonArray{visible_device.x(), visible_device.y(),
							  visible_device.width(), visible_device.height()};
		if (target.valid && !target.label.isEmpty()) {
			// キャンバスのピクセルを、画像のピクセルの矩形にする。
			const auto on_image = [&](const QRect &canvas_rect) {
				return QRect(device(at.x()) + (canvas_rect.x() - crop_.x()) * scale,
					     device(at.y()) + (canvas_rect.y() - crop_.y()) * scale,
					     canvas_rect.width() * scale, canvas_rect.height() * scale);
			};
			const bool left = target.corner == sources::DecorCorner::TopLeft ||
					  target.corner == sources::DecorCorner::BottomLeft;
			const bool top = target.corner == sources::DecorCorner::TopLeft ||
					 target.corner == sources::DecorCorner::TopRight;
			const QRect label = on_image(target.label);
			const QPoint label_corner(left ? target.label.left() : target.label.right(),
						  top ? target.label.top() : target.label.bottom());
			const QSize port_device(device(scroll->viewport()->width()),
						device(scroll->viewport()->height()));
			v[QStringLiteral("label")] = QJsonArray{label.x(), label.y(), label.width(), label.height()};
			v[QStringLiteral("label_fits")] = label.width() <= port_device.width() &&
							  label.height() <= port_device.height();
			v[QStringLiteral("label_inside")] = visible_device.contains(label);
			v[QStringLiteral("label_corner_inside")] =
				visible_device.contains(on_image(QRect(label_corner, QSize(1, 1))));
			v[QStringLiteral("cell_corner_inside")] =
				visible_device.contains(on_image(QRect(target.cell_corner, QSize(1, 1))));
		}
		return v;
	};
	QJsonObject view;
	view[QStringLiteral("tile")] = state_.view_tile;
	view[QStringLiteral("roster")] = state_.view_roster;
	view[QStringLiteral("crop")] = QJsonArray{crop_.x(), crop_.y(), crop_.width(), crop_.height()};
	view[QStringLiteral("composed")] = QJsonArray{crop_region_shown_.x(), crop_region_shown_.y(),
						      crop_region_shown_.width(), crop_region_shown_.height()};
	view[QStringLiteral("one")] = view_json(one_, one_scroll_, 1);
	view[QStringLiteral("two")] = view_json(two_, two_scroll_, 2);
	view[QStringLiteral("corner")] = corner_title(target.corner);
	root[QStringLiteral("view")] = view;
	root[QStringLiteral("status")] = status_->text();

	// focus_knob で見えるようにしたつまみのウィジェットを囲む矩形 (画像のピクセル)。
	for (const KnobWidgets &knob : knobs_) {
		if (focus_key_.empty() || knob.def->key != focus_key_)
			continue;
		QRect area;
		for (const QWidget *widget :
		     {static_cast<const QWidget *>(knob.slider), static_cast<const QWidget *>(knob.spin),
		      static_cast<const QWidget *>(knob.check), static_cast<const QWidget *>(knob.combo),
		      static_cast<const QWidget *>(knob.hex), static_cast<const QWidget *>(knob.swatch)}) {
			if (widget)
				area |= QRect(widget->mapTo(this, QPoint(0, 0)), widget->size());
		}
		const auto device = [dpr](int v) {
			return static_cast<int>(std::lround(v * dpr));
		};
		root[QStringLiteral("focus_rect")] =
			QJsonArray{device(area.x()), device(area.y()), device(area.width()), device(area.height())};
	}
	return root;
}

} // namespace cordmux::label_tuner
