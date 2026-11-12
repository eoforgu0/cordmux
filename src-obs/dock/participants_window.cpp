// SPDX-License-Identifier: Apache-2.0

// 参加者の設定ウィンドウ (participants_window.h) を実装する。

#include "dock/participants_window.h"

#include <obs-module.h>

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QStyle>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStyleOptionHeader>
#include <QVBoxLayout>

#include <algorithm>
#include <set>
#include <utility>

#include "dock/settings_window.h"
#include "dock/ui/dock_metrics.h"
#include "dock/ui/elide_label.h"
#include "dock/ui/focus_ring.h"
#include "dock/ui/text_resolve.h"
#include "dock/ui/theme_read.h"
#include "dock/ui/type_scale.h"
#include "dock/ui/wrap_label.h"
#include "ledger/natural_order.h"
#include "ledger/text_ids.h"

namespace cordmux::dock {

namespace keys = ledger::text::keys;

namespace {

QString tr_key(const char *key)
{
	return ui::resolve_key(key);
}

// 覚えている並べ替え (OBS を終了するまで。ウィンドウを閉じて開き直しても同じ。設定のファイルには保存しない)。
struct RememberedSort {
	ParticipantsWindow::Column column = ParticipantsWindow::ColLastSeen;
	bool ascending = false; // 既定は最終参加日時の新しい順
};

RememberedSort &remembered_sort()
{
	static RememberedSort sort;
	return sort;
}

// 列の見出し。押すと並べ替える。見出しの背景と文字は Qt の標準の表の見出し (QStyle::CE_HeaderSection と CE_HeaderLabel) で描く。
// OBS のテーマは QHeaderView::section に見出しの色と余白を決めているので、見えない QHeaderView を 1 つ持ち、そのウィジェットを
// 渡して描く (スタイルシートは、渡したウィジェットの型で規則を選ぶ)。これで、押せることがテーマ (Yami、Yami Light、Yami Classic
// ほか) に付いてくる。並べ替えの表示 (QStyle::PE_IndicatorHeaderArrow) は、スタイルシートのスタイルが、矢印の規則のないテーマで
// 見出しの上端に潰れた線として描くので使わず、本文の色の三角で描く (エクスプローラーと同じく、昇順は上向き、降順は下向き)。
// 表の見出しと違い、グリッドの列の上に置くため QHeaderView そのものは使えない (列の幅を行のウィジェットと共有できず、キーボードでも
// 押せない)。キーボードでは Tab で届き、Space と Enter で押せる。
//
// 文字は、どの列も見出しの中央に置く。Qt の表の見出し (QHeaderView) の既定 (Qt::AlignCenter) で、OBS 自身の表 (ソースの
// [フィルタ] など) の見出しも同じ形である。並べ替えの表示は、文字の右に間を空けて置き、文字と組にして、灰色の部分 (テーマの
// QHeaderView::section の枠線の内側) の中央に置く。並べていない間は文字だけを中央に置く。左右の余白は同じになる。
// 見出しの幅の下限は「文字 + 間 + 並べ替えの表示 + 左右の余白 (padding_)」で、並べていない見出しも並べ替えの表示の分を含める
// (並べ替えを変えても、見出しの幅も列の幅も動かさない)。OBS のテーマは見出しの左右の余白を 0 にしているので (Yami の
// QHeaderView::section の padding: 3px 0px)、cordmux が余白を持つ。余白がないと、列の幅が見出しの幅のときに、字が灰色の部分の
// 端に付く。
class SortHeader : public QAbstractButton {
public:
	explicit SortHeader(const QString &text, QWidget *parent = nullptr)
		: QAbstractButton(parent), proxy_(new QHeaderView(Qt::Horizontal, this))
	{
		setText(text);
		setFocusPolicy(Qt::StrongFocus);
		setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
		proxy_->hide();
	}

	void set_indicator(QStyleOptionHeader::SortIndicator indicator)
	{
		if (indicator_ == indicator)
			return;
		indicator_ = indicator;
		update();
	}

	QStyleOptionHeader::SortIndicator indicator() const { return indicator_; }

	// 灰色の部分の内側の、左右それぞれの余白 (px)。ParticipantsWindow::apply_spacing が em から決める。
	void set_padding(int px)
	{
		px = std::max(0, px);
		if (padding_ == px)
			return;
		padding_ = px;
		updateGeometry();
		update();
	}

	QSize sizeHint() const override
	{
		proxy_->ensurePolished();
		QStyleOptionHeader option;
		init_option(option);
		const QSize text = fontMetrics().size(Qt::TextSingleLine, this->text());
		// 文字、間、並べ替えの表示 (並べていなくても含める) と、左右の余白。テーマの枠線と上下の余白は sizeFromContents が足す。
		// 文字の幅は、送りの幅と、字の形が送りの外にはみ出す分の広い方にする。
		const std::pair<int, int> ink = ink_span();
		const int text_w = std::max(text.width(), ink.second) - std::min(0, ink.first);
		const QSize contents(2 * padding_ + text_w + indicator_gap() + indicator_size(), text.height());
		return style()->sizeFromContents(QStyle::CT_HeaderSection, &option, contents, proxy_).expandedTo(QSize(1, 1));
	}

	QSize minimumSizeHint() const override { return sizeHint(); }

protected:
	void paintEvent(QPaintEvent *) override
	{
		proxy_->ensurePolished();
		QPainter painter(this);
		QStyleOptionHeader option;
		init_option(option);
		style()->drawControl(QStyle::CE_HeaderSection, &option, &painter, proxy_);
		// 文字 (と並べ替えの表示) の組を、灰色の部分の中央に置く。中央は、字の形 (送りの幅ではなく、実際に描く部分) の左端から、
		// 並べ替えの表示の右端 (並べていなければ字の形の右端) までで決める。字の左右の空き (サイドベアリング) は字ごとに違い、
		// 送りの幅で中央に置くと、見た目の左右の余白が 1-2 px ずれる。
		const QRect box = label_box();
		const int advance = fontMetrics().horizontalAdvance(text());
		const std::pair<int, int> ink = ink_span();
		const bool sorted = indicator_ != QStyleOptionHeader::None;
		const int group_right = sorted ? advance + indicator_gap() + indicator_size() : ink.second;
		const int group_w = group_right - ink.first;
		// origin は文字の送りの左端 (字の形の左端は origin + ink.first)。
		const int origin = box.left() + std::max(0, (box.width() - group_w) / 2) - ink.first;
		QStyleOptionHeader label = option;
		// 送りの幅より少し広い矩形に左揃えで描く (矩形が文字より狭いと、Qt が末尾を省略する)。
		label.rect = QRect(origin, box.top(), advance + 2, box.height());
		label.textAlignment = Qt::AlignLeft | Qt::AlignVCenter;
		style()->drawControl(QStyle::CE_HeaderLabel, &label, &painter, proxy_);
		if (sorted)
			paint_indicator(painter, origin + advance + indicator_gap(), box);
		// 押せることが分かるように、マウスが載っている間は少し明るくする (テーマの見出しの規則に、ホバーの見た目がない)。
		if (underMouse() && isEnabled()) {
			QColor veil = palette().color(QPalette::ButtonText);
			veil.setAlpha(isDown() ? 40 : 24);
			painter.fillRect(rect(), veil);
		}
	}

	void keyPressEvent(QKeyEvent *event) override
	{
		// QAbstractButton は Space だけ押す。Enter でも押せるようにする (標準の押しボタンと同じ)。
		if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
			click();
			return;
		}
		QAbstractButton::keyPressEvent(event);
	}

	void enterEvent(QEnterEvent *event) override
	{
		QAbstractButton::enterEvent(event);
		update();
	}

	void leaveEvent(QEvent *event) override
	{
		QAbstractButton::leaveEvent(event);
		update();
	}

private:
	// 並べ替えの表示 (三角) の大きさと、文字との間。文字の高さに合わせる (フォントの大きさに追従する)。
	int indicator_size() const { return std::max(6, fontMetrics().height() / 2); }
	int indicator_gap() const { return std::max(3, fontMetrics().height() / 4); }

	// 文字を実際に描いたときの、字の形の左端と右端 (送りの左端からの px。右端は含まない)。字の左右の空きは字ごとに違い、
	// フォントの寸法 (QFontMetrics::tightBoundingRect) と、描いたときの薄い縁の画素も合わないので、同じフォントで描いて確かめる。
	// 縁の画素は、不透明度が 1/4 以上のものを字の形に数える。文字とフォントが変わったときだけ描き直す。
	std::pair<int, int> ink_span() const
	{
		const QString key = text() + QLatin1Char('\n') + font().toString();
		if (key == ink_key_)
			return ink_span_;
		const QFontMetrics fm = fontMetrics();
		const int advance = fm.horizontalAdvance(text());
		const int margin = fm.height();
		QImage image(advance + 2 * margin, fm.height(), QImage::Format_ARGB32_Premultiplied);
		image.fill(Qt::transparent);
		{
			QPainter painter(&image);
			painter.setFont(font());
			painter.setPen(Qt::black);
			painter.drawText(margin, fm.ascent(), text());
		}
		int left = -1;
		int right = -1;
		for (int x = 0; x < image.width(); ++x) {
			for (int y = 0; y < image.height(); ++y) {
				if (qAlpha(image.pixel(x, y)) >= 64) {
					if (left < 0)
						left = x;
					right = x;
					break;
				}
			}
		}
		ink_span_ = left < 0 ? std::pair<int, int>(0, advance) : std::pair<int, int>(left - margin, right - margin + 1);
		ink_key_ = key;
		return ink_span_;
	}

	// 灰色の部分 (テーマの QHeaderView::section の枠線と上下の余白の内側。Yami では左右の枠線が背景の色なので、灰色はその内側)。
	// 並べ替えの表示の場所を引かないように、並べていない形で求める。
	QRect label_box() const
	{
		QStyleOptionHeader option;
		init_option(option);
		option.sortIndicator = QStyleOptionHeader::None;
		const QRect box = style()->subElementRect(QStyle::SE_HeaderLabel, &option, proxy_);
		return box.isValid() ? box : rect();
	}

	// 並べ替えの表示を、左端 x から描く (縦は box の中央)。
	void paint_indicator(QPainter &painter, int x, const QRect &box) const
	{
		const int size = indicator_size();
		const int y = box.center().y() - size / 4;
		QPolygon triangle;
		// QHeaderView の対応 (昇順は SortDown) に従い、昇順は上向き、降順は下向きに描く。
		if (indicator_ == QStyleOptionHeader::SortDown)
			triangle << QPoint(x, y + size / 2) << QPoint(x + size, y + size / 2) << QPoint(x + size / 2, y - size / 4);
		else
			triangle << QPoint(x, y) << QPoint(x + size, y) << QPoint(x + size / 2, y + size / 2 + size / 4);
		painter.save();
		painter.setRenderHint(QPainter::Antialiasing, true);
		painter.setPen(Qt::NoPen);
		painter.setBrush(palette().color(QPalette::ButtonText));
		painter.drawPolygon(triangle);
		painter.restore();
	}

	void init_option(QStyleOptionHeader &option) const
	{
		option.initFrom(proxy_);
		option.state |= QStyle::State_Horizontal | QStyle::State_Enabled;
		if (isDown())
			option.state |= QStyle::State_Sunken;
		option.rect = rect();
		option.section = 0;
		option.text = text();
		option.textAlignment = Qt::AlignHCenter | Qt::AlignVCenter;
		option.orientation = Qt::Horizontal;
		option.position = QStyleOptionHeader::OnlyOneSection;
		option.selectedPosition = QStyleOptionHeader::NotAdjacent;
		option.sortIndicator = indicator_;
	}

	QHeaderView *proxy_ = nullptr;
	int padding_ = 6; // apply_spacing が em から決め直す
	mutable QString ink_key_;            // ink_span を求めた文字とフォント
	mutable std::pair<int, int> ink_span_; // その結果
	QStyleOptionHeader::SortIndicator indicator_ = QStyleOptionHeader::None;
};

// 自然順で比べる (名前の列と同じ比べ方)。
int natural_compare(const std::string &a, const std::string &b)
{
	if (ledger::natural_less(a, b))
		return -1;
	if (ledger::natural_less(b, a))
		return 1;
	return 0;
}

} // namespace

struct ParticipantsWindow::Row {
	ledger::dockview::ParticipantRow data;
	ui::ElideLabel *name = nullptr;
	QCheckBox *auto_add = nullptr;
	QLineEdit *rename = nullptr;
	QComboBox *voice = nullptr;
	QCheckBox *roster_hide = nullptr;
	QLabel *last_seen = nullptr;
	// 並べ替えに使う文字 (名前と、選んでいる使う声の表示)。fill_row が書き換えるときに作る。
	std::string name_key;
	std::string voice_key;

	std::vector<QWidget *> widgets() const { return {name, auto_add, rename, voice, roster_hide, last_seen}; }
};

ParticipantsWindow::ParticipantsWindow(QWidget *parent, ActionCallback on_action)
	: QDialog(parent), on_action_(std::move(on_action))
{
	setObjectName(QStringLiteral("cordmuxParticipantsWindow"));
	setWindowTitle(tr_key(keys::kParticipantsWindowTitle));
	setModal(false);
	setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	// 閉じた後にフォーカスを戻す先の初めの値は、作った時点のフォーカス (設定ウィンドウと同じ)。開き直したときは、
	// 呼び出し元が渡し直す。
	set_return_focus(QApplication::focusWidget());

	auto *root = new QVBoxLayout(this);

	// 行は縦のスクロール領域 1 つの中に置く。横にはスクロールしない (列はウィンドウの幅まで伸びる)。
	scroll_ = new QScrollArea(this);
	scroll_->setWidgetResizable(true);
	scroll_->setFrameShape(QFrame::NoFrame);
	scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scroll_->setFocusPolicy(Qt::StrongFocus); // フォーカスのある行が消えたときの移し先 (update_rows)
	scroll_->setAccessibleName(tr_key(keys::kParticipantsWindowTitle));
	content_ = new QWidget();
	grid_ = new QGridLayout(content_);
	scroll_->setWidget(content_);
	root->addWidget(scroll_, 1);

	// 列の見出し。見出しのツールチップは列の説明にする。
	const char *header_keys[ColumnCount] = {keys::kParticipantsColNameLabel,        keys::kParticipantsColAutoAddLabel,
						keys::kParticipantsColNameInCordmuxLabel, keys::kParticipantsColVoiceFromLabel,
						keys::kParticipantsColRosterHideLabel,          keys::kParticipantsColLastSeenLabel};
	const char *header_tips[ColumnCount] = {nullptr,
						keys::kParticipantsColAutoAddTip,
						keys::kParticipantsNameInCordmuxTip,
						keys::kParticipantsColVoiceFromTip,
						keys::kParticipantsColRosterHideTip,
						nullptr};
	for (int c = 0; c < ColumnCount; ++c) {
		// 見出しの文字は、どの列も中央に置く (SortHeader)。
		auto *header = new SortHeader(tr_key(header_keys[c]));
		header->setObjectName(QStringLiteral("cordmuxParticipantHeader%1").arg(c));
		if (header_tips[c] != nullptr)
			header->setToolTip(tr_key(header_tips[c]));
		const Column column = static_cast<Column>(c);
		connect(header, &QAbstractButton::clicked, this, [this, column]() { on_header_clicked(column); });
		column_headers_.push_back(header);
	}
	sort_column_ = remembered_sort().column;
	sort_ascending_ = remembered_sort().ascending;
	refresh_sort_headers();
	// 利用者が行から離れたとき (見出しやほかのウィンドウに移ったとき) に、待たせていた並べ直しをする。
	connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *) {
		if (pending_resort_)
			reorder(false);
	});

	// まとまりの見出し (同じ形の 2 つ)。
	current_heading_ = new QLabel(tr_key(keys::kParticipantsCurrentHeading));
	current_heading_->setTextFormat(Qt::PlainText);
	// 通話に参加している人が 0 人でも見出しを表示し、その下に、ここに何が表示されるかを 1 文で表示する。理由は書かない
	// (Vesktop と接続されていない間も同じテキストが正しいため)。見出しごと隠すと、過去の参加者だけのウィンドウに見え、
	// 通話に参加している人の行がどこに表示されるかが分からない。空の 1 文は、最後の行が短くなりすぎないように折り返す
	// WrapLabel にする (設定ウィンドウの補足と同じ)。
	current_empty_ = new ui::WrapLabel(tr_key(keys::kParticipantsEmptyCurrentEmpty));
	past_heading_ = new QLabel(tr_key(keys::kParticipantsPastHeading));
	past_heading_->setTextFormat(Qt::PlainText);
	past_empty_ = new ui::WrapLabel(tr_key(keys::kParticipantsEmptyPastEmpty));
	apply_heading_type();
	apply_spacing();

	// 余った幅は名前の列だけに与える。cordmux での名前の列にも余りを分けると、initial_size が名前の全文のために足した
	// 幅が 2 列に半分ずつ分かれ、20 em より短い名前まで省かれる。cordmux での名前の入力欄は QLineEdit の既定の幅にする
	// (テキストが長ければ入力欄の中でスクロールする)。名前の列の最小の幅を全文の幅にすると、ウィンドウを縮めたときに
	// 4 em の下限が動作しなくなる。
	grid_->setColumnStretch(ColName, 1);

	// ボタンは [閉じる] だけ。テキストは OBS の ini から読む。
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	QPushButton *close = buttons->button(QDialogButtonBox::Close);
	close->setText(obs_frontend_text(ledger::text::obs_keys::kClose));
	close->setDefault(true);
	connect(close, &QPushButton::clicked, this, [this]() { reject(); });
	root->addWidget(buttons);

	// フォーカスリング。cordmux のウィンドウでは、フォーカスを受けるウィジェットのすべてに付ける。行のウィジェットは
	// スクロール領域の中にあるので、リングはスクロール領域の見えている範囲で切って描かれる (dock/ui/focus_ring.h)。
	ring_ = new ui::FocusRing(this);
	refresh_focus_ring();

	place_rows();
}

void ParticipantsWindow::refresh_focus_ring()
{
	// 色と太さは、テーマと [フォントサイズ] から決める (設定ウィンドウと確認のダイアログと同じ読み方)。
	if (ring_ == nullptr)
		return;
	const ui::ThemeRead theme = ui::read_theme(this, nullptr);
	const ui::DockMetrics m = ui::read_dock_metrics(this, theme, ui::DockStyle{});
	ring_->set_color(theme.text);
	ring_->set_metrics(m.focus_ring_px, m.focus_ring_gap_px);
}

void ParticipantsWindow::set_return_focus(QWidget *opener)
{
	if (opener != nullptr && opener->window() == this)
		return;
	return_focus_ = opener;
}

ParticipantsWindow::~ParticipantsWindow()
{
	for (auto &entry : rows_)
		delete entry.second;
}

void ParticipantsWindow::apply_heading_type()
{
	// 見出しは本文と同じ大きさの太字にする (dock/ui/type_scale.h の TypeLevel::Heading)。
	const double base_pt = ui::base_point_size(this);
	for (QLabel *heading : {current_heading_, past_heading_})
		ui::apply_type(heading, ui::TypeLevel::Heading, base_pt, 1.0);
}

void ParticipantsWindow::apply_spacing()
{
	// 列の間は、まとまりの間隔 (0.75 em。DockMetrics::group_gap_px) にする。列はそれぞれ別のまとまりで、Qt の既定 (6 px)
	// では、英語の見出し "Add Automatically" と "Name in cordmux" が 1 つの句に読める。OBS の [オーディオの詳細プロパティ]
	// も、同じ形のグリッドの列の間を広く取っている (OBS 31 の frontend/components/OBSAdvAudioCtrl.cpp:252 の
	// setHorizontalSpacing(15))。OBS と同じ 15 px に固定すると [フォントサイズ] に追従せず、大きい余白 (0.5 em = 7 px)
	// では既定とほぼ同じなので見出しが分かれない。
	const ui::DockMetrics m =
		ui::compute_dock_metrics(ui::base_point_size(this), logicalDpiX(), 16, ui::DockStyle{});
	grid_->setHorizontalSpacing(m.group_gap_px);
	// 列の見出しの、灰色の部分の内側の左右の余白は、大きい余白 (0.5 em) にする。
	for (QAbstractButton *header : column_headers_) {
		if (auto *sort = dynamic_cast<SortHeader *>(header))
			sort->set_padding(m.large_px);
	}
	// 「過去の参加者」の見出しは、上の現在の参加者の行とは別のまとまりなので、前の行との間を、意味の違う部分の間 (1 em) に
	// する。グリッドの行の間の分を引いた残りを見出しの上に空ける。
	const int row_gap = std::max(0, grid_->verticalSpacing());
	past_heading_->setContentsMargins(0, std::max(0, m.section_gap_px - row_gap), 0, 0);
}

void ParticipantsWindow::refresh_theme()
{
	apply_heading_type();
	apply_spacing();
	refresh_focus_ring();
}

ParticipantsWindow::Row *ParticipantsWindow::make_row(const ledger::dockview::ParticipantRow &row_data)
{
	auto *row = new Row;
	row->name = new ui::ElideLabel();
	row->name->setObjectName(QStringLiteral("cordmuxParticipantRow"));
	// 名前に残す最小の幅 (4 em。ドックの一覧の行と同じ下限)。狭いウィンドウでも、名前が省略記号だけにならない。
	const double em = ui::base_point_size(this) * logicalDpiX() / 72.0;
	row->name->setMinimumWidth(static_cast<int>(em * 4.0 + 0.5));
	row->auto_add = new QCheckBox();
	row->auto_add->setObjectName(QStringLiteral("cordmuxParticipantAutoAdd"));
	row->rename = new QLineEdit();
	row->voice = new QComboBox();
	row->roster_hide = new QCheckBox();
	row->roster_hide->setObjectName(QStringLiteral("cordmuxParticipantRosterHide"));
	row->last_seen = new QLabel();
	row->last_seen->setTextFormat(Qt::PlainText);
	row->last_seen->setTextInteractionFlags(Qt::TextSelectableByMouse);

	const std::string user_id = row_data.user_id;
	for (QWidget *w : row->widgets())
		w->setProperty("userId", QString::fromStdString(user_id));

	// 自動で載せる。テキストのないチェックボックスなので、アクセシブル名に人の名前、accessibleDescription に列の名前を
	// 入れる (fill_row)。
	QObject::connect(row->auto_add, &QCheckBox::clicked, this, [this, user_id](bool) {
		auto it = rows_.find(user_id);
		if (it == rows_.end() || !on_action_)
			return;
		const ledger::dockview::ViewButton &toggle = it->second->data.always_on_toggle;
		on_action_(toggle.id, toggle.args);
	});

	// cordmux での名前。入力を終えたら (Enter を押すかフォーカスが外れたら)、すぐに反映する。空にすると、cordmux での
	// 名前を消し、Discord での名前に戻す。プレースホルダーは Discord での名前。
	QObject::connect(row->rename, &QLineEdit::editingFinished, this, [this, user_id]() {
		auto it = rows_.find(user_id);
		if (it == rows_.end() || !on_action_)
			return;
		Row &r = *it->second;
		const std::string text = r.rename->text().trimmed().toStdString();
		if (text == r.data.name_override)
			return;
		ledger::ActionArgs args = r.data.rename.args;
		args.user_id = user_id;
		args.name = text;
		on_action_(r.data.rename.id, args);
	});

	// 使う声。選び直したら、すぐに反映する。
	QObject::connect(row->voice, &QComboBox::activated, this, [this, user_id](int index) {
		auto it = rows_.find(user_id);
		if (it == rows_.end() || !on_action_)
			return;
		Row &r = *it->second;
		const int choice = r.voice->itemData(index).toInt();
		if (choice < 0 || choice >= static_cast<int>(r.data.voices.size()))
			return;
		const ledger::dockview::VoiceChoice &voice = r.data.voices[static_cast<std::size_t>(choice)];
		if (voice.checked || voice.action.disabled_reason.has_value())
			return;
		on_action_(voice.action.id, voice.action.args);
	});

	// 通話のみ時ラベル非表示 (チェックを付けた人を、画面共有していない参加者の並びに出さない)。テキストのないチェックボックスなので、アクセシブル名と
	// accessibleDescription は「自動で載せる」と同じ形にする (fill_row)。
	QObject::connect(row->roster_hide, &QCheckBox::clicked, this, [this, user_id](bool) {
		auto it = rows_.find(user_id);
		if (it == rows_.end() || !on_action_)
			return;
		const ledger::dockview::ViewButton &toggle = it->second->data.roster_toggle;
		on_action_(toggle.id, toggle.args);
	});

	fill_row(*row, row_data, true);
	return row;
}

void ParticipantsWindow::fill_row(Row &row, const ledger::dockview::ParticipantRow &row_data, bool first)
{
	// 変わった所だけを書き換える。過去の参加者は 200 人まで並ぶことがあるので、周期ごとに全員の全部のウィジェットの
	// テキストを書き直さない。
	const ledger::dockview::ParticipantRow before = row.data;
	row.data = row_data;
	const bool name_changed = first || row_data.name != before.name;
	const QString person = ui::resolve_label(row_data.name);

	// 名前 (Discord での名前。読めなければ userId)。省いたら、全文をツールチップとアクセシブル名に入れる。行の
	// ウィジェットのアクセシブル名は人の名前、accessibleDescription は列の名前にして、誰の何の値かを伝える。
	if (name_changed) {
		row.name->set_full_text(person);
		for (QWidget *w : {static_cast<QWidget *>(row.auto_add), static_cast<QWidget *>(row.rename),
				   static_cast<QWidget *>(row.voice), static_cast<QWidget *>(row.roster_hide)})
			w->setAccessibleName(person);
		row.rename->setPlaceholderText(person); // プレースホルダーは Discord での名前
		row.name_key = person.toStdString();
	}
	if (first) {
		row.auto_add->setAccessibleDescription(tr_key(keys::kParticipantsColAutoAddLabel));
		row.rename->setAccessibleDescription(tr_key(keys::kParticipantsColNameInCordmuxLabel));
		row.voice->setAccessibleDescription(tr_key(keys::kParticipantsColVoiceFromLabel));
		row.voice->setToolTip(tr_key(keys::kParticipantsColVoiceFromTip));
		row.roster_hide->setAccessibleDescription(tr_key(keys::kParticipantsColRosterHideLabel));
		row.last_seen->setAccessibleDescription(tr_key(keys::kParticipantsColLastSeenLabel));
	}

	// 自動で載せる。押せないとき (選び方が「自動」の間) は押せない見た目にし、理由をツールチップに入れる。
	if (row.auto_add->isChecked() != row_data.always_on) {
		const QSignalBlocker block(row.auto_add);
		row.auto_add->setChecked(row_data.always_on);
	}
	if (first || row_data.always_on_toggle != before.always_on_toggle) {
		const bool blocked = row_data.always_on_toggle.disabled_reason.has_value();
		row.auto_add->setEnabled(!blocked);
		row.auto_add->setToolTip(blocked ? ui::resolve(*row_data.always_on_toggle.disabled_reason)
						 : tr_key(keys::kParticipantsColAutoAddTip));
	}

	// cordmux での名前。入力している間 (フォーカスがある間) は、入力途中のテキストを消さないように書き換えない。
	// 入力できないときは、理由をツールチップに入れる。
	if (!row.rename->hasFocus() && (first || row_data.name_override != before.name_override))
		row.rename->setText(QString::fromStdString(row_data.name_override));
	if (first || row_data.rename != before.rename) {
		const bool blocked = row_data.rename.disabled_reason.has_value();
		row.rename->setEnabled(!blocked);
		row.rename->setToolTip(blocked ? ui::resolve(*row_data.rename.disabled_reason)
					       : tr_key(keys::kParticipantsNameInCordmuxTip));
	}

	// 使う声。一覧を開いている間は、選んでいる最中に項目が動かないように書き換えない。
	if (first || row_data.voices != before.voices) {
		// 並べ替えの文字は、一覧の状態に関係なく、選んでいる声の表示にする。
		row.voice_key.clear();
		for (const ledger::dockview::VoiceChoice &v : row_data.voices) {
			if (v.checked)
				row.voice_key = ui::resolve_label(v.label).toStdString();
		}
		const bool open = row.voice->view() != nullptr && row.voice->view()->isVisible();
		if (!open) {
			const QSignalBlocker block(row.voice);
			row.voice->clear();
			int current = -1;
			for (std::size_t i = 0; i < row_data.voices.size(); ++i) {
				const ledger::dockview::VoiceChoice &v = row_data.voices[i];
				// 先頭は「本人の声」で、区切り線の後に通話に参加している人を並べる。
				if (i == 1)
					row.voice->insertSeparator(row.voice->count());
				row.voice->addItem(ui::resolve_label(v.label), static_cast<int>(i));
				const int index = row.voice->count() - 1;
				if (v.checked)
					current = index;
				if (v.action.disabled_reason.has_value()) {
					// 選べない項目は選べない見た目にし、理由 (cordmux.participants.voice_taken.text) をその項目の
					// ツールチップに入れる。
					if (auto *model = qobject_cast<QStandardItemModel *>(row.voice->model())) {
						if (QStandardItem *item = model->item(index))
							item->setEnabled(false);
					}
					row.voice->setItemData(index, ui::resolve(*v.action.disabled_reason), Qt::ToolTipRole);
				}
			}
			if (current >= 0)
				row.voice->setCurrentIndex(current);
		}
	}

	// 通話のみ時ラベル非表示。チェックは「並びに出さない」の向きで、保存の値 (roster_shown) の逆である。押せないとき
	// (メインアカウントの行など) は、並びに出ない人なのでチェックが付いた形にし、押せない見た目にして理由をツールチップに入れる。
	const bool blocked = row_data.roster_toggle.disabled_reason.has_value();
	const bool hidden = blocked || !row_data.roster_shown;
	if (row.roster_hide->isChecked() != hidden) {
		const QSignalBlocker block(row.roster_hide);
		row.roster_hide->setChecked(hidden);
	}
	if (first || row_data.roster_toggle != before.roster_toggle) {
		row.roster_hide->setEnabled(!blocked);
		row.roster_hide->setToolTip(blocked ? ui::resolve(*row_data.roster_toggle.disabled_reason)
						    : tr_key(keys::kParticipantsColRosterHideTip));
	}

	// 最終参加日時 (過去の参加者は最後に通話にいた日時、現在の参加者はこの通話に参加した日時)。表示の形は QLocale で決める。
	if (first || row_data.last_seen_at_us != before.last_seen_at_us) {
		if (row_data.last_seen_at_us.has_value())
			row.last_seen->setText(ui::format_time_us(*row_data.last_seen_at_us, true));
		else
			row.last_seen->clear();
	}
}

void ParticipantsWindow::update_rows(const std::vector<ledger::dockview::ParticipantRow> &rows)
{
	// 消える行。フォーカスがあれば、先にスクロール領域に移す。
	std::map<std::string, const ledger::dockview::ParticipantRow *> next;
	for (const ledger::dockview::ParticipantRow &r : rows)
		next[r.user_id] = &r;
	for (auto it = rows_.begin(); it != rows_.end();) {
		if (next.count(it->first) > 0) {
			++it;
			continue;
		}
		Row *gone = it->second;
		const QWidget *focus = QApplication::focusWidget();
		for (QWidget *w : gone->widgets()) {
			if (w == focus)
				scroll_->setFocus(Qt::OtherFocusReason);
		}
		for (QWidget *w : gone->widgets()) {
			grid_->removeWidget(w);
			w->deleteLater();
		}
		delete gone;
		it = rows_.erase(it);
	}

	// 追加する行と書き換える行。
	for (const ledger::dockview::ParticipantRow &r : rows) {
		auto it = rows_.find(r.user_id);
		if (it == rows_.end())
			rows_[r.user_id] = make_row(r);
		else
			fill_row(*it->second, r, false);
	}

	reorder(false);
}

std::vector<std::string> ParticipantsWindow::sorted_ids(bool in_call) const
{
	std::vector<const Row *> list;
	for (const auto &entry : rows_) {
		if (entry.second->data.in_call == in_call)
			list.push_back(entry.second);
	}
	const Column column = sort_column_;
	const bool ascending = sort_ascending_;
	// 列ごとの比べ方。名前と cordmux での名前と使う声は自然順 (使う声は表示している文字)、チェックの列はチェックありが先、
	// 最終参加日時は時刻。同じ値の行は、名前の自然順 (向きに関係なく昇順)。
	const auto by_column = [column](const Row &a, const Row &b) {
		switch (column) {
		case ColName:
			return natural_compare(a.name_key, b.name_key);
		case ColAutoAdd:
			return static_cast<int>(b.data.always_on) - static_cast<int>(a.data.always_on);
		case ColRename:
			return natural_compare(a.data.name_override, b.data.name_override);
		case ColVoice:
			return natural_compare(a.voice_key, b.voice_key);
		case ColRosterHide: {
			const auto hidden = [](const Row &r) {
				return r.data.roster_toggle.disabled_reason.has_value() || !r.data.roster_shown;
			};
			return static_cast<int>(hidden(b)) - static_cast<int>(hidden(a));
		}
		case ColLastSeen: {
			const std::int64_t at = a.data.last_seen_at_us.value_or(0);
			const std::int64_t bt = b.data.last_seen_at_us.value_or(0);
			return at < bt ? -1 : (at > bt ? 1 : 0);
		}
		default:
			return 0;
		}
	};
	std::sort(list.begin(), list.end(), [&](const Row *a, const Row *b) {
		int c = by_column(*a, *b);
		if (c != 0)
			return ascending ? c < 0 : c > 0;
		c = natural_compare(a->name_key, b->name_key);
		if (c != 0)
			return c < 0;
		return a->data.user_id < b->data.user_id;
	});
	std::vector<std::string> ids;
	for (const Row *row : list)
		ids.push_back(row->data.user_id);
	return ids;
}

bool ParticipantsWindow::user_is_engaged() const
{
	// 利用者が操作している行: 行のウィジェットにフォーカスがあるか、使う声の一覧を開いている。
	// フォーカスはウィジェットごとに見る (ウィンドウが前面でない間も、フォーカスを持つウィジェットは変わらない)。
	for (const auto &entry : rows_) {
		for (const QWidget *w : entry.second->widgets()) {
			if (w->hasFocus())
				return true;
		}
		const QAbstractItemView *view = entry.second->voice->view();
		if (view != nullptr && view->isVisible())
			return true;
	}
	return false;
}

void ParticipantsWindow::reorder(bool force)
{
	std::vector<std::string> current = sorted_ids(true);
	std::vector<std::string> past = sorted_ids(false);
	const bool hold = !force && user_is_engaged();
	bool pending = false;
	if (hold) {
		// 現在の並びを保つ。組が変わった行 (通話に参加した・抜けた) と新しい行は、組の最後に、並べ替えの順で足す。
		const auto keep = [this](bool in_call, const std::vector<std::string> &wanted) {
			std::vector<std::string> out;
			std::set<std::string> used;
			for (const std::string &id : order_) {
				const auto it = rows_.find(id);
				if (it == rows_.end() || it->second->data.in_call != in_call)
					continue;
				out.push_back(id);
				used.insert(id);
			}
			for (const std::string &id : wanted) {
				if (used.count(id) == 0)
					out.push_back(id);
			}
			return out;
		};
		std::vector<std::string> kept_current = keep(true, current);
		std::vector<std::string> kept_past = keep(false, past);
		pending = kept_current != current || kept_past != past;
		current = std::move(kept_current);
		past = std::move(kept_past);
	}
	pending_resort_ = pending;

	std::vector<std::string> order = std::move(current);
	std::vector<bool> in_call(order.size(), true);
	for (std::string &id : past)
		order.push_back(std::move(id));
	in_call.resize(order.size(), false);
	if (order != order_ || in_call != order_in_call_) {
		order_ = std::move(order);
		order_in_call_ = std::move(in_call);
		place_rows();
	}
}

void ParticipantsWindow::set_sort(Column column, bool ascending)
{
	sort_column_ = column;
	sort_ascending_ = ascending;
	remembered_sort() = RememberedSort{column, ascending};
	refresh_sort_headers();
	reorder(true);
}

ParticipantsWindow::Column ParticipantsWindow::sort_column() const
{
	return sort_column_;
}

bool ParticipantsWindow::sort_ascending() const
{
	return sort_ascending_;
}

void ParticipantsWindow::reset_remembered_sort()
{
	remembered_sort() = RememberedSort{};
}

QAbstractButton *ParticipantsWindow::column_header(Column column) const
{
	return column_headers_.at(static_cast<std::size_t>(column));
}

void ParticipantsWindow::on_header_clicked(Column column)
{
	// 同じ見出しをもう一度押すと逆の順にする (エクスプローラーと同じ)。別の見出しは、最初の向きで並べる。最初の向きは、
	// 最終参加日時だけ新しい順 (エクスプローラーの日付と同じ)、ほかは昇順 (チェックの列はチェックありが先)。
	if (column == sort_column_)
		set_sort(column, !sort_ascending_);
	else
		set_sort(column, column != ColLastSeen);
}

void ParticipantsWindow::refresh_sort_headers()
{
	// 並べている列に、向きの表示を付ける (QHeaderView と同じ対応: 昇順は SortDown)。スクリーンリーダーには、アクセシブル
	// な説明で向きを伝える (並べていない列は、説明が空なのでツールチップの列の説明が届く)。
	for (int c = 0; c < ColumnCount; ++c) {
		auto *header = static_cast<SortHeader *>(column_headers_[static_cast<std::size_t>(c)]);
		if (c != sort_column_) {
			header->set_indicator(QStyleOptionHeader::None);
			header->setAccessibleDescription(QString());
			continue;
		}
		header->set_indicator(sort_ascending_ ? QStyleOptionHeader::SortDown : QStyleOptionHeader::SortUp);
		header->setAccessibleDescription(tr_key(sort_ascending_ ? keys::kParticipantsSortedAscendingA11y
									: keys::kParticipantsSortedDescendingA11y));
	}
}

void ParticipantsWindow::place_rows()
{
	// グリッドから全部を外し (ウィジェットは削除しない)、現在の並びで置き直す。
	for (QAbstractButton *header : column_headers_)
		grid_->removeWidget(header);
	for (QLabel *label : {current_heading_, current_empty_, past_heading_, past_empty_})
		grid_->removeWidget(label);
	for (auto &entry : rows_) {
		for (QWidget *w : entry.second->widgets())
			grid_->removeWidget(w);
	}

	// 「自動で載せる」と「通話のみ時ラベル非表示」の列は、テキストのないチェックボックスを見出しの下の中央に置く。見出しの左端の下に小さな
	// 四角が寄ると、隣の列の見出しとつながって読める。見出しの文字は、どの列も中央に置く (SortHeader)。
	const auto column_alignment = [](int c) {
		return c == ColAutoAdd || c == ColRosterHide ? Qt::Alignment(Qt::AlignHCenter) : Qt::Alignment();
	};
	// 見出しは列の幅いっぱいに置く (表の見出しの形。文字の位置は見出しの中で決める)。
	int r = 0;
	for (int c = 0; c < ColumnCount; ++c)
		grid_->addWidget(column_headers_[static_cast<std::size_t>(c)], r, c);
	++r;

	bool any_current = false;
	bool any_past = false;
	for (const bool in_call : order_in_call_) {
		any_current = any_current || in_call;
		any_past = any_past || !in_call;
	}

	// 現在の参加者。誰も通話に参加していなければ、空の状態の 1 文を表示する。
	grid_->addWidget(current_heading_, r++, 0, 1, ColumnCount);
	current_empty_->setVisible(!any_current);
	if (!any_current)
		grid_->addWidget(current_empty_, r++, 0, 1, ColumnCount);
	for (std::size_t i = 0; i < order_.size(); ++i) {
		if (!order_in_call_[i])
			continue;
		Row *row = rows_[order_[i]];
		const std::vector<QWidget *> ws = row->widgets();
		for (int c = 0; c < ColumnCount; ++c)
			grid_->addWidget(ws[static_cast<std::size_t>(c)], r, c, column_alignment(c));
		row->last_seen->setVisible(true);
		++r;
	}

	// 過去の参加者。いなければ、空の状態の 1 文を表示する。
	grid_->addWidget(past_heading_, r++, 0, 1, ColumnCount);
	past_empty_->setVisible(!any_past);
	if (!any_past)
		grid_->addWidget(past_empty_, r++, 0, 1, ColumnCount);
	for (std::size_t i = 0; i < order_.size(); ++i) {
		if (order_in_call_[i])
			continue;
		Row *row = rows_[order_[i]];
		const std::vector<QWidget *> ws = row->widgets();
		for (int c = 0; c < ColumnCount; ++c)
			grid_->addWidget(ws[static_cast<std::size_t>(c)], r, c, column_alignment(c));
		row->last_seen->setVisible(true);
		++r;
	}
	grid_->setRowStretch(r, 1);

	// 最小の大きさも中身から決める。全部の列が横に切れず (名前は 4 em の下限)、見出しと 1 行と [閉じる] が見える大きさにする。
	setMinimumSize(minimum_window_size());

	// Tab の順を見た目の並び (見出しの行、その下は行ごとに左から右) にそろえる。
	QWidget *prev = nullptr;
	for (QAbstractButton *header : column_headers_) {
		if (prev != nullptr)
			QWidget::setTabOrder(prev, header);
		prev = header;
	}
	for (std::size_t i = 0; i < order_.size(); ++i) {
		for (QWidget *w : rows_[order_[i]]->widgets()) {
			if (w->focusPolicy() == Qt::NoFocus)
				continue;
			if (prev != nullptr)
				QWidget::setTabOrder(prev, w);
			prev = w;
		}
	}
}

void ParticipantsWindow::set_on_closed(std::function<void(const std::string &)> on_closed)
{
	on_closed_ = std::move(on_closed);
}

void ParticipantsWindow::restore_window_geometry(const std::string &geometry)
{
	bool restored = false;
	if (!geometry.empty())
		restored = restoreGeometry(QByteArray::fromBase64(QByteArray::fromStdString(geometry)));
	if (!restored)
		resize(initial_size());
}

void ParticipantsWindow::polish_rows() const
{
	// テーマのスタイルシート (フォントの大きさ、コンボボックスの余白と矢印) を、測る前に全部のウィジェットに適用する。
	// ウィンドウは作った時点で (見出しのフォントを読むときに) polish され、その後に追加した行のウィジェットは、
	// ウィンドウの ensurePolished では polish されない (Qt は 1 度 polish したウィジェットの子を 2 回目にはたどらない)。
	// polish する前のコンボボックスの幅でウィンドウの幅を決めると、表示したときにコンボボックスが広がり、余りを受ける
	// 名前の列が削られる (コンボボックスは、テーマの余白と矢印が付くと広がる)。
	ensurePolished();
	for (QWidget *w : content_->findChildren<QWidget *>())
		w->ensurePolished();
}

QSize ParticipantsWindow::minimum_window_size() const
{
	polish_rows();
	const QMargins m = layout()->contentsMargins();
	const int spacing = std::max(0, layout()->spacing());
	const int scrollbar = style()->pixelMetric(QStyle::PM_ScrollBarExtent);
	int buttons_h = 0;
	for (QDialogButtonBox *box : findChildren<QDialogButtonBox *>(QString(), Qt::FindDirectChildrenOnly))
		buttons_h = std::max(buttons_h, box->sizeHint().height());
	// 縦は、列の見出し、まとまりの見出し、1 人の行の 3 行分 (残りはスクロールする)。行の高さはコンボボックスの高さにする。
	int row_h = column_headers_.empty() ? 0 : column_headers_.front()->sizeHint().height();
	if (!rows_.empty())
		row_h = std::max(row_h, rows_.begin()->second->voice->sizeHint().height());
	const int grid_spacing = std::max(0, grid_->verticalSpacing());
	const QMargins gm = grid_->contentsMargins();
	const int rows_h = gm.top() + 3 * row_h + 2 * grid_spacing + gm.bottom();
	return QSize(m.left() + content_->minimumSizeHint().width() + scrollbar + m.right(),
		     m.top() + rows_h + spacing + buttons_h + m.bottom());
}

QSize ParticipantsWindow::initial_size() const
{
	// 初めて開くときの大きさは中身から決め、全部の列と行がスクロールせずに入る大きさにする (名前は全文の幅)。
	// QScrollArea の sizeHint は中身の大きさを見ないので、グリッドの中身の sizeHint を直接見る。画面の 85% を超えるときは
	// 画面に合わせ、残りは縦のスクロールと名前の省略に任せる。
	polish_rows();
	QSize content = content_->sizeHint();
	// 名前のラベル (ElideLabel) は、長い名前が 1 つあってもウィンドウの最小幅が広がらないように、横の sizePolicy を
	// Ignored にしてある。そのため、グリッドの sizeHint は名前の列を最小の幅 (4 em) で数える。ここで全文の幅との差を
	// 足す (上限は 20 em で、それより長い名前は省く)。
	const double em = ui::base_point_size(const_cast<ParticipantsWindow *>(this)) * logicalDpiX() / 72.0;
	int name_extra = 0;
	for (const auto &entry : rows_) {
		const ui::ElideLabel *name = entry.second->name;
		const int full = std::min(name->sizeHint().width(), static_cast<int>(em * 20.0 + 0.5));
		name_extra = std::max(name_extra, full - name->minimumWidth());
	}
	content.rwidth() += name_extra;
	const QMargins m = layout()->contentsMargins();
	const int spacing = std::max(0, layout()->spacing());
	const int scrollbar = style()->pixelMetric(QStyle::PM_ScrollBarExtent);
	int buttons_h = 0;
	for (QDialogButtonBox *box : findChildren<QDialogButtonBox *>(QString(), Qt::FindDirectChildrenOnly))
		buttons_h = std::max(buttons_h, box->sizeHint().height());
	QSize want(m.left() + content.width() + scrollbar + m.right(),
		   m.top() + content.height() + spacing + buttons_h + m.bottom());
	const QScreen *screen = parentWidget() != nullptr ? parentWidget()->screen() : this->screen();
	if (screen != nullptr)
		want = want.boundedTo(screen->availableGeometry().size() * 0.85);
	return want.expandedTo(minimumSizeHint());
}

void ParticipantsWindow::done(int result)
{
	const bool was_active = isActiveWindow();
	if (!closed_reported_) {
		closed_reported_ = true;
		if (on_closed_)
			on_closed_(saveGeometry().toBase64().toStdString());
	}
	QDialog::done(result);
	return_focus_after_close(was_active, return_focus_, parentWidget() != nullptr ? parentWidget()->window() : nullptr);
}

ParticipantsWindow *make_participants_window(QWidget *parent, const std::vector<ledger::dockview::ParticipantRow> &rows,
					     ActionCallback on_action)
{
	auto *window = new ParticipantsWindow(parent, std::move(on_action));
	window->update_rows(rows);
	window->restore_window_geometry(std::string());
	return window;
}

} // namespace cordmux::dock
