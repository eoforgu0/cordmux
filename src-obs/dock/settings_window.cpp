// SPDX-License-Identifier: Apache-2.0

// 設定ウィンドウ (settings_window.h) を実装する。

#include "dock/settings_window.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QScreen>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpacerItem>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStyle>
#include <QStyleOption>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "dock/settings_fields.h"
#include "dock/ui/dock_metrics.h"
#include "dock/ui/focus_ring.h"
#include "dock/ui/form_parts.h"
#include "dock/ui/open_location.h"
#include "dock/ui/set_class.h"
#include "dock/ui/spin_box_ignore_wheel.h"
#include "dock/ui/text_resolve.h"
#include "dock/ui/theme_read.h"
#include "dock/ui/type_scale.h"
#include "dock/ui/wrap_label.h"
#include "dock/windows_calc.h"
#include "ledger/text_ids.h"
#include "sources/decor_paint.h"

namespace cordmux::dock {

namespace keys = ledger::text::keys;

struct SettingsWindow::Field {
	QWidget *widget = nullptr;
	std::function<void()> load;  // edited_ からウィジェットへ
	std::function<void()> store; // ウィジェットから edited_ へ
};

namespace {

using Page = ledger::SettingsPage;

QString tr_key(const char *key)
{
	return ui::resolve_key(key);
}

QString tr_ref(const ledger::TextRef &ref)
{
	return ui::resolve(ref);
}

// ページの名前のキーと、プレビューツールがページを指定するときの名前。
struct PageInfo {
	Page page;
	const char *title_key;
	const char *id;
};

const PageInfo kPages[] = {
	{Page::Recording, keys::kSettingsRecordingPage, "recording"},
	{Page::Receive, keys::kSettingsIncomingVideoPage, "receive"},
	{Page::Advanced, keys::kSettingsAdvancedPage, "advanced"},
	{Page::About, keys::kSettingsAboutPage, "about"},
};

int page_index(Page page)
{
	for (int i = 0; i < static_cast<int>(std::size(kPages)); ++i) {
		if (kPages[i].page == page)
			return i;
	}
	return 0;
}

using ui::add_note_row;
using ui::make_form;
using ui::set_combo_data;
using ui::wrap_label;

// 入力欄のラベル (ini のキー) を付けて行を足す。
QLabel *add_row(QFormLayout *form, const char *label_key, QWidget *field, QWidget *buddy = nullptr)
{
	return ui::add_form_row(form, tr_key(label_key), field, buddy);
}

// 選んでコピーできる 1 行 (場所とバージョン)。
QLabel *selectable_label(const QString &text)
{
	auto *label = new QLabel(text);
	label->setTextFormat(Qt::PlainText);
	label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
	return label;
}

// 保存先の入力欄に表示する場所。OS の区切り文字にそろえる (Pipeline は「/」を足した場所を返すため)。
QString display_path(const std::string &path)
{
	return QDir::toNativeSeparators(QString::fromStdString(path));
}

// 同じ場所を指す文字か (区切りの種類、末尾の区切り、大文字小文字は区別しない。Windows のパスのため)。
bool same_place(const QString &a, const QString &b)
{
	if (a.isEmpty() || b.isEmpty())
		return false;
	const QString left = QDir::cleanPath(QDir::fromNativeSeparators(a.trimmed()));
	const QString right = QDir::cleanPath(QDir::fromNativeSeparators(b.trimmed()));
	return left.compare(right, Qt::CaseInsensitive) == 0;
}

// 場所の入力欄 ([録画] のページの保存先)。
// - プレースホルダー (入力欄を空にして入力している間だけ見える、既定の場所) が入力欄に入らないときは、真ん中を省いて先頭 (ドライブ) と
//   末尾 (cordmux のフォルダー) を残す。Qt の QLineEdit はプレースホルダーの末尾を省く (Qt 6.8.3 の qlineedit.cpp の
//   paintEvent: fm.elidedText(placeholderText, Qt::ElideRight, lineRect.width())) ので、末尾の cordmux が消えて、どこに
//   書くかが読めない。省いたテキストを入力欄の幅に合わせてプレースホルダーに入れ直す (幅、フォントの大きさ、テーマが
//   変わるたびに)。全文はツールチップに入れる (呼び出し元が入れる)。
// - ウィンドウの初めの幅は中身から決める。入力欄の sizeHint の幅を、プレースホルダーと入力したテキストの長い方が全部
//   入る幅にする (上限は 40 em で、ダイアログの本文の 1 行と同じ長さ。それより長い場所は真ん中を省く)。OBS の録画の
//   保存先は既定で利用者の「ビデオ」(C:\Users\<名前>\Videos。OBS 31 の frontend/utility/platform-windows.cpp:59-67) なので、
//   通常のプレースホルダーは 24 字以上あり、QLineEdit の既定の幅 (x の 17 字分) に入らない。ウィンドウの初めの幅に
//   決まった数を足す方法は使わない。場所の長さは利用者名で変わり、どの数でも足りない人が残る。
// - 入力したテキストは省かない。入力中のテキストが省略記号に替わると修正できない (OBS の入力欄も省かない)。
// - [参照...] と [フォルダーを開く] は入力欄の右に並べる (OBS の [設定] の録画の場所の入力欄と同じ並び)。
class PathLineEdit : public QLineEdit {
public:
	void set_full_placeholder(const QString &text)
	{
		full_placeholder_ = text;
		updateGeometry();
		refit_placeholder();
	}

	QSize sizeHint() const override
	{
		QSize hint = QLineEdit::sizeHint();
		const QFontMetrics fm = fontMetrics();
		// QLineEdit::sizeHint のテキストの幅の分 (x の 17 字) を、プレースホルダーと入力したテキストの長い方に置き換える。
		const int base_text = fm.horizontalAdvance(QLatin1Char('x')) * 17;
		const int em = std::max(1, QFontInfo(font()).pixelSize());
		const int longest = std::max(fm.horizontalAdvance(full_placeholder_), fm.horizontalAdvance(text())) + 1;
		const int wanted = std::min(longest, 40 * em);
		if (wanted > base_text)
			hint.setWidth(hint.width() + wanted - base_text);
		return hint;
	}

protected:
	void resizeEvent(QResizeEvent *event) override
	{
		QLineEdit::resizeEvent(event);
		refit_placeholder();
	}

	void changeEvent(QEvent *event) override
	{
		QLineEdit::changeEvent(event);
		if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
			updateGeometry();
			refit_placeholder();
		}
	}

private:
	void refit_placeholder()
	{
		// プレースホルダーを描く幅は、Qt と同じ方法で求める (Qt 6.8.3 の qlineedit.cpp の paintEvent: SE_LineEditContents
		// からテキストの余白を引き、左右に QLineEditPrivate::horizontalMargin = 2 を残す)。
		QStyleOptionFrame option;
		initStyleOption(&option);
		const QRect contents =
			style()->subElementRect(QStyle::SE_LineEditContents, &option, this).marginsRemoved(textMargins());
		const int width = contents.width() - 2 * 2;
		const QString shown = width > 0 ? fontMetrics().elidedText(full_placeholder_, Qt::ElideMiddle, width)
						: full_placeholder_;
		if (placeholderText() != shown)
			setPlaceholderText(shown);
	}

	QString full_placeholder_;
};

ui::SpinBoxIgnoreWheel *make_spin(int lo, int hi, const char *suffix_key)
{
	auto *spin = new ui::SpinBoxIgnoreWheel();
	spin->setRange(lo, hi);
	if (suffix_key != nullptr)
		spin->setSuffix(tr_key(suffix_key));
	return spin;
}

QString page_group_title(const char *key)
{
	return tr_key(key);
}

// 「cordmux について」の項目のアイコン。OBS の [設定] のアイコンのセットに中身の近いものがないので、同じ線の太さと
// 大きさで cordmux が描く。円の輪郭と「i」を、テーマの本文の色の 1 色で描く。
// 線の太さは px / 8 (16 px で 2 px)。OBS 31 の themes/Dark/settings/video.svg は、16 のグリッドで外形の線が 2 (1.0 から
// 3.0) である。px / 12 では、拡大するとほかの 5 つのアイコンより細く薄く見える。i の縦線と点も同じ太さにする (点の
// 直径 = 線の太さ)。縦線は 0.49-0.70 にする (丸い端を足して点と 1 px 前後空き、円の内側に触れない。16 px で数えた)。
QIcon draw_about_icon(int px, const QColor &color)
{
	QIcon icon;
	for (const qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
		const int side = std::max(1, static_cast<int>(std::lround(px * dpr)));
		QPixmap pixmap(side, side);
		pixmap.fill(Qt::transparent);
		pixmap.setDevicePixelRatio(dpr);
		QPainter painter(&pixmap);
		painter.setRenderHint(QPainter::Antialiasing, true);
		const qreal stroke = std::max<qreal>(1.0, px / 8.0);
		QPen pen(color, stroke);
		pen.setCapStyle(Qt::RoundCap);
		painter.setPen(pen);
		painter.setBrush(Qt::NoBrush);
		const qreal inset = stroke / 2.0 + px * 0.06;
		painter.drawEllipse(QRectF(inset, inset, px - 2 * inset, px - 2 * inset));
		const qreal cx = px / 2.0;
		painter.drawLine(QPointF(cx, px * 0.49), QPointF(cx, px * 0.70));
		painter.setPen(Qt::NoPen);
		painter.setBrush(color);
		painter.drawEllipse(QPointF(cx, px * 0.30), stroke * 0.5, stroke * 0.5);
		painter.end();
		icon.addPixmap(pixmap);
	}
	return icon;
}

// アイコンがない項目にも同じ幅の空きを確保し、アイコンの有無でテキストの先頭がずれないようにする。
QIcon blank_icon(int px)
{
	QPixmap pixmap(std::max(1, px), std::max(1, px));
	pixmap.fill(Qt::transparent);
	return QIcon(pixmap);
}

bool restart_required_for(const std::string &json_key)
{
	for (std::size_t i = 0; i < settings_field_count(); ++i) {
		const SettingsFieldRow &row = settings_field_at(i);
		if (json_key == row.json_key)
			return row.restart_required;
	}
	return false;
}

QString version_value_text(const ledger::dockview::VersionRow &row)
{
	return ui::resolve_label(row.value);
}

} // namespace

QString obs_frontend_text(const char *key)
{
	const char *text = obs_frontend_get_locale_string(key);
	return QString::fromUtf8(text != nullptr ? text : key);
}

SettingsWindow::SettingsWindow(QWidget *parent, SettingsWindowInput input) : QDialog(parent), input_(std::move(input))
{
	setObjectName(QStringLiteral("cordmuxSettingsWindow"));
	setWindowTitle(tr_key(keys::kSettingsWindowTitle));
	// モーダルにしない (開いている間も、配信中に OBS とドックを操作できるように)。タイトルバーに [?] を表示しない
	// (Qt の QDialog は、Windows では既定で [?] を付ける)。
	setModal(false);
	setWindowFlag(Qt::WindowContextHelpButtonHint, false);

	// 閉じた後にフォーカスを戻す先の初めの値は、作った時点のフォーカス (ドックのメニューから開いたなら [...])。開き直した
	// ときは、呼び出し元が set_return_focus で渡し直す (dock_poll_settings.cpp の open_settings)。
	set_return_focus(QApplication::focusWidget());

	applied_ = input_.current;
	edited_ = input_.current;

	build();
	load_fields();
	update_about(input_.about);
	set_export_busy(input_.export_busy);
	set_page(input_.page);
	update_restart_notes();
	update_apply_enabled();

	// 大きさと位置。前に閉じたときの値があればそれにし、なければ中身から決めた大きさで親ウィンドウの中央に置く
	// (QDialog は初めて表示するときに親の中央に置く)。最小の大きさも中身から決め、どのページも横に切れず、項目の一覧の
	// 4 つが全部見える大きさにする。測る前に polish して、テーマのフォントの大きさを適用する。
	ensurePolished();
	// 測る前に、ページの中のレイアウトがキャッシュした大きさを捨てる。レイアウトは polish (テーマのフォントの適用) の前に
	// 測った大きさを保存していて、polish した後のウィジェットの sizeHint が反映されない。ウィンドウを表示する前の子の
	// ウィジェットは隠れている扱いで、updateGeometry が親のレイアウトに届かない (Qt の
	// QWidgetPrivate::updateGeometry_helper は、隠れたウィジェットからは親のレイアウトを無効にしない)。
	for (int i = 0; i < stack_->count(); ++i) {
		QWidget *box = stack_->widget(i);
		for (QWidget *child : box->findChildren<QWidget *>())
			child->updateGeometry();
		for (QLayout *child : box->findChildren<QLayout *>())
			child->invalidate();
		if (box->layout() != nullptr)
			box->layout()->invalidate();
	}
	setMinimumSize(minimum_window_size());
	bool restored = false;
	if (!input_.geometry.empty())
		restored = restoreGeometry(QByteArray::fromBase64(QByteArray::fromStdString(input_.geometry)));
	if (!restored)
		resize(initial_size());
}

QScrollArea *SettingsWindow::page_scroll(int index) const
{
	// ページは「スクロール領域 + 再起動を求める 1 文」のコンテナー (wrap_page)。
	QWidget *box = stack_->widget(index);
	return box != nullptr ? box->findChild<QScrollArea *>(QString(), Qt::FindDirectChildrenOnly) : nullptr;
}

QWidget *SettingsWindow::page_content(int index) const
{
	// 中身はスクロール領域の中にある。
	QScrollArea *scroll = page_scroll(index);
	return scroll != nullptr ? scroll->widget() : nullptr;
}

QSize SettingsWindow::minimum_window_size() const
{
	ensurePolished();
	int page_min_w = 0;
	for (int i = 0; i < stack_->count(); ++i) {
		if (QWidget *content = page_content(i))
			page_min_w = std::max(page_min_w, content->minimumSizeHint().width());
	}
	const QMargins m = layout()->contentsMargins();
	const int spacing = std::max(0, layout()->spacing());
	const int scrollbar = style()->pixelMetric(QStyle::PM_ScrollBarExtent);
	const int list_h = page_list_->sizeHintForRow(0) * page_list_->count() + 2 * page_list_->frameWidth();
	return QSize(m.left() + page_list_->width() + spacing + page_min_w + scrollbar + m.right(),
		     m.top() + list_h + spacing + buttons_->sizeHint().height() + m.bottom());
}

QSize SettingsWindow::initial_size() const
{
	// 初めて開くときの大きさは中身から決め、最も大きいページの中身がスクロールせずに入る大きさにする。QScrollArea の
	// sizeHint は中身の大きさを見ない (テキストの高さの 36 x 24 倍で止まる) ので、ページの中身の sizeHint を直接見る。
	// 画面の 85% を超えるときは画面に合わせ、残りは縦のスクロールに任せる。
	ensurePolished(); // テーマのフォントの大きさと余白を適用してから測る
	const int scrollbar = style()->pixelMetric(QStyle::PM_ScrollBarExtent);
	int content_w = 0;
	for (int i = 0; i < stack_->count(); ++i) {
		if (QWidget *content = page_content(i))
			content_w = std::max(content_w, content->sizeHint().width());
	}
	// 再起動を求める 1 文を表示することがあるページ (再起動が必要な入力欄を持つページ) では、1 文とその上の間の高さも
	// 足して高さを数える。1 文はスクロール領域の外の下に表示するので、表示した分だけスクロール領域が低くなる。足さないと、
	// 入力欄を変えた瞬間に詳細設定のページに縦のスクロールバーが出て、変えた入力欄の幅が縮み、最後の入力欄と枠の下が
	// 切れる。1 文の場所を空けておくのではなく、初めの高さの計算に入れるだけにする (1 文を表示していない間は、ページの
	// 下に余りがあるだけ)。どのページかは入力欄の restartRequired から求める (今は詳細設定だけ)。1 文を表示するときに
	// ウィンドウを伸ばすと、ウィンドウの大きさが利用者の操作で変わってしまう。
	int content_h = 0;
	for (int i = 0; i < stack_->count(); ++i) {
		QWidget *content = page_content(i);
		if (content == nullptr)
			continue;
		// ページの高さは、ウィンドウに置く幅 (content_w) で折り返した高さにする。折り返すラベルは幅で高さが変わり、sizeHint
		// の高さはラベルごとの仮の幅で測った値なので、置く幅と違うと高すぎるか低すぎる (エクスポートの場所のラベルは、
		// 仮の幅では 3-4 行の高さになり、ウィンドウが 18-54 px 高くなる)。
		const int page_hfw = content->hasHeightForWidth() ? content->heightForWidth(content_w) : -1;
		int page_h = page_hfw > 0 ? page_hfw : content->sizeHint().height();
		const auto note = restart_notes_.find(kPages[i].page);
		const bool can_restart = std::any_of(fields_.begin(), fields_.end(), [content](const Field *field) {
			return field->widget->property("restartRequired").toBool() && content->isAncestorOf(field->widget);
		});
		if (note != restart_notes_.end() && can_restart) {
			QLabel *label = note->second;
			const int note_w = content_w + scrollbar;
			const int note_h = label->hasHeightForWidth() ? label->heightForWidth(note_w) : label->sizeHint().height();
			const QLayout *box = stack_->widget(i)->layout();
			page_h += std::max(0, note_h) + (box != nullptr ? std::max(0, box->spacing()) : 0);
		}
		content_h = std::max(content_h, page_h);
	}
	const QMargins m = layout()->contentsMargins();
	const int spacing = std::max(0, layout()->spacing());
	QSize want(m.left() + page_list_->width() + spacing + content_w + scrollbar + m.right(),
		   m.top() + content_h + spacing + buttons_->sizeHint().height() + m.bottom());
	const QScreen *screen = parentWidget() != nullptr ? parentWidget()->screen() : this->screen();
	if (screen != nullptr) {
		const QSize avail = screen->availableGeometry().size() * 0.85;
		want = want.boundedTo(avail);
	}
	return want.expandedTo(minimumSizeHint());
}

SettingsWindow::~SettingsWindow()
{
	for (Field *field : fields_)
		delete field;
}

void SettingsWindow::build()
{
	auto *root = new QVBoxLayout(this);

	auto *body = new QHBoxLayout();
	page_list_ = new QListWidget(this);
	page_list_->setObjectName(QStringLiteral("cordmuxSettingsPageList"));
	page_list_->setAccessibleName(tr_key(keys::kSettingsWindowTitle));
	for (const PageInfo &info : kPages)
		page_list_->addItem(new QListWidgetItem(tr_key(info.title_key)));
	page_list_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
	body->addWidget(page_list_);

	stack_ = new QStackedWidget(this);
	stack_->addWidget(wrap_page(build_recording_page(), Page::Recording));
	stack_->addWidget(wrap_page(build_receive_page(), Page::Receive));
	stack_->addWidget(wrap_page(build_advanced_page(), Page::Advanced));
	stack_->addWidget(wrap_page(build_about_page(), Page::About));
	body->addWidget(stack_, 1);
	root->addLayout(body, 1);

	connect(page_list_, &QListWidget::currentRowChanged, this, [this](int row) {
		if (row >= 0)
			show_stack_page(row);
	});
	// UI Automation (スクリーンリーダーや音声での操作) でページを選ぶと、Qt は選択だけを変え、現在の行を変えない。OBS の
	// [設定] と同じく、選択が変わったらそのページを開く (現在の行を選択に合わせると、上の接続がページを替える)。
	connect(page_list_, &QListWidget::itemSelectionChanged, this, [this]() {
		const QList<QListWidgetItem *> selected = page_list_->selectedItems();
		if (selected.isEmpty())
			return;
		const int row = page_list_->row(selected.front());
		if (row != page_list_->currentRow())
			page_list_->setCurrentRow(row);
	});

	// [OK]、[キャンセル]、[適用]。並びは QDialogButtonBox の Windows の並びにし、テキストは OBS の [設定] と同じにする
	// ため、OBS の ini から読む。
	buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, this);
	buttons_->button(QDialogButtonBox::Ok)->setText(obs_frontend_text(ledger::text::obs_keys::kOk));
	// [OK] はプライマリのボタンにする (OBS のクラス button-primary)。
	ui::set_class(buttons_->button(QDialogButtonBox::Ok), QStringLiteral("button-primary"));
	buttons_->button(QDialogButtonBox::Cancel)->setText(obs_frontend_text(ledger::text::obs_keys::kCancel));
	apply_button_ = buttons_->button(QDialogButtonBox::Apply);
	apply_button_->setText(obs_frontend_text(ledger::text::obs_keys::kApply));
	connect(buttons_->button(QDialogButtonBox::Ok), &QPushButton::clicked, this, [this]() {
		// 反映していない変更があれば反映し、保存できたときだけ閉じる (保存できなければウィンドウを閉じない)。
		// アンインストールの後は、反映せずに閉じる。書いても残らず、入力欄のページは操作できなくしてあり、[適用] も
		// 押せない (update_lock)。操作できなくする前に変えた値 (ダミーのオンとオフなど) を、[OK] だけが反映する形に
		// しない。そのまま apply() を呼ぶと、操作できなくした入力欄の値が、アンインストールの後に反映される。[OK] も
		// 押せなくすると、OBS の [設定] の [OK] がいつも押せるのとそろわない。
		if (about_.settings_locked || !dirty() || apply())
			accept();
	});
	connect(buttons_->button(QDialogButtonBox::Cancel), &QPushButton::clicked, this, [this]() { reject(); });
	connect(apply_button_, &QPushButton::clicked, this, [this]() { apply(); });
	root->addWidget(buttons_);

	// フォーカスリング (cordmux のウィンドウでは、フォーカスを受けるウィジェットのすべてに付ける)。
	ring_ = new ui::FocusRing(this);
	refresh_focus_ring();
	refresh_notes();

	refresh_page_icons();
}

void SettingsWindow::refresh_focus_ring()
{
	// 色と太さは、テーマと [フォントサイズ] から決める (確認のダイアログと「名前を変更」のダイアログと同じ読み方)。
	if (ring_ == nullptr)
		return;
	const ui::ThemeRead theme = ui::read_theme(this, nullptr);
	const ui::DockMetrics m = ui::read_dock_metrics(this, theme, ui::DockStyle{});
	ring_->set_color(theme.text);
	ring_->set_metrics(m.focus_ring_px, m.focus_ring_gap_px);
	ring_reach_px_ = m.focus_ring_px + m.focus_ring_gap_px;
}

QWidget *SettingsWindow::wrap_page(QWidget *content, Page page)
{
	// 右の中身は縦のスクロール領域 1 つにし、横にはスクロールしない。再起動を求める 1 文は、スクロール領域の外の下
	// (ボタンの並びの上) に置く。スクロール領域の中の下に置くと、中身がウィンドウより高いページでは 1 文が見える範囲の
	// 外に出て、入力欄を変えても気付けない。OBS の [設定] も同じ位置に置いている (OBS 31 の
	// frontend/forms/OBSBasicSettings.ui の audioMsg と videoMsg は、ページのスクロール領域の後の項目)。1 文を表示したときに
	// スクロール領域を下までスクロールすると、利用者が見ていた入力欄が見えなくなる。
	// ページの objectName (cordmuxSettingsPage_<page>) は、スクロール領域と 1 文を持つコンテナーに付ける (ページの
	// ウィジェットは全部この下にある。テストはこの下で、再起動を求める 1 文を探す)。
	auto *box = new QWidget();
	box->setObjectName(QStringLiteral("cordmuxSettingsPage_") + QString::fromLatin1(kPages[page_index(page)].id));
	auto *v = new QVBoxLayout(box);
	v->setContentsMargins(0, 0, 0, 0);
	auto *scroll = new QScrollArea();
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	// スクロール領域はフォーカスを受けない。タブストップは、押す、選ぶ、入力することのできるウィジェットだけにする。
	// スクロール領域にフォーカスが来ても押すものがなく、フォーカスリングがページの全体を囲む (リングはウィジェットを示す
	// もので、ページを示すものではない)。PageUp と PageDown は、ボタン、チェックボックス、1 行の入力欄にフォーカスが
	// あれば、スクロール領域まで届いてスクロールする (QAbstractScrollArea が、子のウィジェットが受けなかったキーを受ける)。
	// 数の入力欄とコンボボックスでは入力欄がキーを受けて値を変え、スクロール領域は動かない (スクロール領域がタブストップ
	// の形でも同じ)。フォーカスを受けないようにして失うのは、値を変えずに PageUp と PageDown でスクロールできる
	// タブストップ 1 つだけである。Tab で進めば、移った先の入力欄は見える所までスクロールし (focusNextPrevChild)、マウスでの
	// スクロールも残る。スクロール領域の外から Tab で入ったウィジェットを見える所までスクロールするのは focusNextPrevChild
	// である (スクロール領域がタブストップでなくなると、Qt の QScrollArea::focusNextPrevChild のスクロールは、スクロール
	// 領域の中から移るときにしか通らない)。
	// Qt の既定 (StrongFocus) のままにすると、Tab の一巡のタブストップが 1 つ増え、そこではスクロールのほかにすることが
	// なく、リングはページの全体を囲む。OBS 31 の [設定] の 12 のスクロール領域は既定のまま (OBSBasicSettings.ui は
	// focusPolicy を変えていない) だが、この点は OBS と違う形にする。
	scroll->setFocusPolicy(Qt::NoFocus);
	scroll->setWidget(content);
	v->addWidget(scroll, 1);
	if (page != Page::About) {
		v->addWidget(make_restart_note(page));
		locked_pages_.push_back(content);
	}
	return box;
}

QLabel *SettingsWindow::make_restart_note(Page page)
{
	// 再起動を求める 1 文。色は付けない。危険の色はメッセージのレベル 1 のパネルだけに使い、この文は失敗ではなく、反映する
	// 前に伝える補足である。OBS と同じ text-danger にすると、メッセージのレベル 1 と同じ色のテキストがウィンドウに表示される。
	auto *note = wrap_label(tr_key(keys::kSettingsRestartNeededText));
	note->setObjectName(QStringLiteral("cordmuxRestartNote"));
	note->setVisible(false);
	restart_notes_[page] = note;
	return note;
}

void SettingsWindow::register_field(QWidget *widget, const std::string &keys_joined, std::function<void()> load,
				    std::function<void()> store)
{
	bool restart = false;
	for (const std::string &key : windows_calc::split_field_keys(keys_joined))
		restart = restart || restart_required_for(key);
	widget->setObjectName(QStringLiteral("cordmuxField_") + QString::fromStdString(keys_joined));
	widget->setProperty("restartRequired", restart);
	auto *field = new Field;
	field->widget = widget;
	field->load = std::move(load);
	field->store = std::move(store);
	fields_.push_back(field);
}

// ---- 3. 録画 ----

QWidget *SettingsWindow::build_recording_page()
{
	auto *page = new QWidget();
	auto *v = new QVBoxLayout(page);
	auto *form = make_form();

	// 保存先。テキストを入力できる (空にすると既定 = OBS の録画の保存先の中の cordmux に戻るので、戻す専用のボタンは
	// 置かない。OBS の録画の保存先の入力欄もテキストを入力できる)。入力欄の文字は、録画ごとのフォルダー (開始日時の名前) を
	// 作る実際の場所である。選んでいないときも、既定の場所をふつうの文字で表示する (D:\hoge と入れたら D:\hoge\<開始日時>
	// に、表示している場所のとおりに保存する)。書けない場所を入力しても、録画を始める時点で場所を解決し、メッセージの
	// レベル 1 で伝えるので、入力途中のテキストが、メッセージもログも出さずに使われることはない。
	auto *row = new QWidget();
	auto *h = new QHBoxLayout(row);
	h->setContentsMargins(0, 0, 0, 0);
	// [フォルダーを開く] は入力欄の隣に置き、場所の行は追加しない。入力欄そのものが場所を表示し、入力欄が空のときは
	// プレースホルダーが実際に使う場所を表示するので、場所の行を追加すると、同じ場所を 2 回表示することになる。開くのは、
	// 現在の入力欄の値で録画を始めたら書く場所。並びは OBS の録画の保存先の入力欄と同じく、[参照...] を入力欄のすぐ右に
	// 置き、その右に [フォルダーを開く] を置く。
	record_dir_edit_ = new PathLineEdit();
	auto *browse = new QPushButton(tr_key(keys::kSettingsBrowseButton));
	record_open_folder_ = new QPushButton(tr_key(keys::kSettingsOpenFolderButton));
	h->addWidget(record_dir_edit_, 1);
	h->addWidget(browse);
	h->addWidget(record_open_folder_);
	add_row(form, keys::kSettingsSaveFolderLabel, row, record_dir_edit_);
	// 入力欄には、選んでいないときも、録画ごとのフォルダーを作る実際の場所を、ふつうの文字で表示する (設定に保存するのは
	// 空のまま)。入力欄の文字がその時点の既定の場所と同じか空なら、選んでいないものとして扱う。
	register_field(
		record_dir_edit_, "record_dir",
		[this]() {
			record_default_path_ = input_.record_dir_now ? input_.record_dir_now(std::string()) : std::string();
			record_dir_edit_->setText(edited_.record_dir.empty()
							  ? display_path(record_default_path_)
							  : QString::fromStdString(edited_.record_dir));
		},
		[this]() {
			const QString text = record_dir_edit_->text().trimmed();
			const bool is_default = text.isEmpty() || same_place(text, QString::fromStdString(record_default_path_));
			edited_.record_dir = is_default ? std::string() : text.toStdString();
		});
	connect(record_dir_edit_, &QLineEdit::textChanged, this, [this](const QString &) { on_field_changed(); });
	connect(record_dir_edit_, &QLineEdit::editingFinished, this, [this]() { update_record_location(); });
	connect(browse, &QPushButton::clicked, this, [this]() { choose_record_dir(); });
	connect(record_open_folder_, &QPushButton::clicked, this, [this]() {
		// 入力途中のテキストでも、押した時点の入力欄の値で書く場所を解決してから開く (editingFinished は押す前に来るが、
		// 来ない場合 (入力欄にフォーカスがないままテキストが入った場合) でも同じ場所を開く)。
		update_record_location();
		// 開き方は、cordmux の [フォルダーを開く] に共通の関数 (dock/ui/open_location.h) に任せる。まだない場所と、もうない
		// 場所は、存在する中で最も近い親を開く。ここで独自に親をたどると、開き方を決める所が 2 か所になって食い違う。
		ui::open_folder(QString::fromUtf8(record_location_path_.c_str()));
	});
	add_description(form, tr_key(keys::kSettingsSaveFolderText));

	// 録画ファイルに書く音声の形式。保存するファイルの設定なので、[保存先] の下に置く。
	auto *pcm = new QComboBox();
	pcm->addItem(tr_key(keys::kSettingsAudioFormatFloat32Option),
		     static_cast<int>(config::RecordPcmFormat::Float32));
	pcm->addItem(tr_key(keys::kSettingsAudioFormatInt24Option), static_cast<int>(config::RecordPcmFormat::Int24));
	pcm->setToolTip(tr_key(keys::kSettingsAudioFormatTip));
	add_row(form, keys::kSettingsAudioFormatLabel, pcm);
	register_field(
		pcm, "record_output_pcm",
		[this, pcm]() { set_combo_data(pcm, static_cast<int>(edited_.record_output_pcm)); },
		[this, pcm]() {
			edited_.record_output_pcm = static_cast<config::RecordPcmFormat>(pcm->currentData().toInt());
		});
	connect(pcm, &QComboBox::currentIndexChanged, this, [this](int) { on_field_changed(); });

	auto *warn = make_spin(config::kRecordSpaceWarnMinMinutes, config::kRecordSpaceWarnMaxMinutes,
			       keys::kSettingsUnitMinutesLabel);
	warn->setToolTip(tr_key(keys::kSettingsRemainingTimeWarningTip));
	add_row(form, keys::kSettingsRemainingTimeWarningLabel, warn);
	register_field(
		warn, "record_space_warn_minutes", [this, warn]() { warn->setValue(edited_.record_space_warn_minutes); },
		[this, warn]() { edited_.record_space_warn_minutes = warn->value(); });
	connect(warn, &QSpinBox::valueChanged, this, [this](int) { on_field_changed(); });

	// 配信の開始で録画も始め、配信の終了で録画も終える。文言は OBS の設定の「配信時に自動的に録画する」に合わせる。
	// 録画の状態は起動のたびにオフに戻るが、この設定は戻らない。
	auto *with_stream = new QCheckBox(tr_key(keys::kSettingsRecordWithStreamCheck));
	with_stream->setToolTip(tr_key(keys::kSettingsRecordWithStreamTip));
	form->addRow(QString(), with_stream);
	register_field(
		with_stream, "record_with_stream", [this, with_stream]() { with_stream->setChecked(edited_.record_with_stream); },
		[this, with_stream]() { edited_.record_with_stream = with_stream->isChecked(); });
	connect(with_stream, &QCheckBox::toggled, this, [this](bool) { on_field_changed(); });

	v->addLayout(form);
	v->addStretch(1);
	return page;
}

void SettingsWindow::choose_record_dir()
{
	// 開く場所は、今入力欄にある場所か、なければ今書く場所。ダイアログのタイトルは「録画の保存先」(ページの名前 + 入力欄の
	// 名前) にする。
	QString start = record_dir_edit_->text().trimmed();
	if (start.isEmpty())
		start = QString::fromStdString(record_location_path_);
	// フォルダーを選ぶダイアログは Windows の標準のダイアログのままにし、親はこのウィンドウにする (OBS 自身のダイアログも
	// 同じ静的な関数を使う)。Qt 6.8.3 のソースでは、QDialog::exec は OS のダイアログを使うとき Qt のウィジェットの
	// ダイアログを表示せず (qdialog.cpp の QDialogPrivate::setVisible が setNativeDialogVisible で返る)、Qt のモーダルの
	// 仕組みではウィンドウを止めない。OS のダイアログは IFileDialog::Show(親) で開き (qwindowsdialoghelpers.cpp の exec)、
	// Windows が止めるのは親のこのウィンドウだけなので、OBS のメインウィンドウとドックは止まらないはずである。実際の
	// OBS で、開いている間にメインウィンドウとドックを操作できるかは確認していない (ドックを操作するテストは OS の
	// ダイアログを持たず、Qt のウィジェットのダイアログを ApplicationModal で開くので測れない)。Qt のウィジェットの
	// ダイアログ (DontUseNativeDialog) にして色のダイアログと同じ run_owner_only_dialog で開くと、最近使った場所と
	// クイック アクセスのある、Windows の標準のダイアログが使えなくなる。
	const QString chosen = QFileDialog::getExistingDirectory(this, tr_key(keys::kDialogSaveFolderTitle), start);
	if (chosen.isEmpty())
		return; // ダイアログを閉じた (何も変えない)
	// 設定のファイルには OS の区切り文字で入れる (settings.h の record_dir は「UTF-8 の絶対パス」)。
	record_dir_edit_->setText(QDir::toNativeSeparators(chosen));
	update_record_location();
}

void SettingsWindow::update_record_location()
{
	if (record_dir_edit_ == nullptr)
		return;
	// 現在の既定の場所。選んでいない (入力欄が空か、前の既定の場所か、現在の既定の場所のまま) 間は、入力欄の文字を現在の既定の
	// 場所に合わせる (OBS の録画の保存先を変えたとき。入力欄が空になったときは、既定に戻る)。
	std::string default_now;
	if (input_.record_dir_now)
		default_now = input_.record_dir_now(std::string());
	const QString text = record_dir_edit_->text().trimmed();
	const bool follows_default = text.isEmpty() || same_place(text, QString::fromStdString(record_default_path_)) ||
				     same_place(text, QString::fromStdString(default_now));
	record_default_path_ = default_now;
	if (follows_default && !default_now.empty() && record_dir_edit_->text() != display_path(default_now))
		record_dir_edit_->setText(display_path(default_now)); // textChanged から edited_ が更新される
	std::string resolved = default_now;
	if (!follows_default && input_.record_dir_now)
		resolved = input_.record_dir_now(edited_.record_dir);
	record_location_path_ = resolved;
	// 書く場所が分からないとき (問い合わせる関数がないか、空を返したとき) だけ押せなくする。隠すと入力欄の幅が変わり、
	// 行が動く。
	record_open_folder_->setEnabled(!resolved.empty());
	// プレースホルダーは、入力欄を空にして入力している間だけ見える。入力が終わると既定の場所の文字に戻る。入力欄に入らない
	// プレースホルダーは、真ん中を省く (PathLineEdit)。全文は入力欄のツールチップで、現在の入力欄の値で録画を始めたら
	// 書く場所にする。accessibleDescription もツールチップから届く (Qt の QAccessibleWidget は、説明が空なら
	// ツールチップを使う)。
	static_cast<PathLineEdit *>(record_dir_edit_)->set_full_placeholder(display_path(default_now));
	record_dir_edit_->setToolTip(display_path(resolved));
}

// ---- 4. 映像の受信 ----

QWidget *SettingsWindow::build_receive_page()
{
	auto *page = new QWidget();
	auto *v = new QVBoxLayout(page);

	// 「受信する映像」の組。受け取る映像の上限と、受信する人。
	auto *received_group = new QGroupBox(page_group_title(keys::kSettingsHeadingReceivedVideoHeading));
	auto *form = make_form();

	auto *long_edge = new QComboBox();
	add_row(form, keys::kSettingsMaxResolutionLabel, long_edge);
	register_field(
		long_edge, "limit_max_long_edge",
		[this, long_edge]() {
			// 一覧にない値 (settings.json を手で書いた値) は、その値の項目を追加して表示する (近い値に丸めない)。
			long_edge->clear();
			for (const int value :
			     windows_calc::with_value(windows_calc::long_edge_choices(), edited_.limit_max_long_edge)) {
				if (value == config::kLimitMaxLongEdgeNone) {
					long_edge->addItem(tr_key(keys::kSettingsMaxResolutionNoneOption), value);
					continue;
				}
				ledger::TextRef px;
				px.id = keys::kSettingsMaxResolutionPxOption;
				px.args = {std::to_string(value)};
				long_edge->addItem(tr_ref(px), value);
			}
			set_combo_data(long_edge, edited_.limit_max_long_edge);
		},
		[this, long_edge]() { edited_.limit_max_long_edge = long_edge->currentData().toInt(); });
	connect(long_edge, &QComboBox::currentIndexChanged, this, [this](int) { on_field_changed(); });

	auto *fps = new QComboBox();
	fps->addItem(tr_key(keys::kSettingsMaxFrameRateOriginalOption), 1);
	fps->addItem(tr_key(keys::kSettingsMaxFrameRateHalfOption), 2);
	add_row(form, keys::kSettingsMaxFrameRateLabel, fps);
	register_field(
		fps, "limit_fps_divisor", [this, fps]() { set_combo_data(fps, edited_.limit_fps_divisor); },
		[this, fps]() { edited_.limit_fps_divisor = fps->currentData().toInt(); });
	connect(fps, &QComboBox::currentIndexChanged, this, [this](int) { on_field_changed(); });

	add_description(form, tr_key(keys::kSettingsLimitsApplyText));

	// 受信する人。「枠に載せた人だけ」にすると、Vesktop は枠に載せた人の画面共有だけを視聴し、画面共有の
	// 録画も枠に載せた人の分だけになる。
	auto *scope = new QComboBox();
	scope->addItem(tr_key(keys::kSettingsReceiveScopeSlotsOption), static_cast<int>(config::ReceiveScope::Slots));
	scope->addItem(tr_key(keys::kSettingsReceiveScopeEveryoneOption), static_cast<int>(config::ReceiveScope::Everyone));
	scope->setToolTip(tr_key(keys::kSettingsReceiveScopeTip));
	add_row(form, keys::kSettingsReceiveScopeLabel, scope);
	register_field(
		scope, "receive_scope", [this, scope]() { set_combo_data(scope, static_cast<int>(edited_.receive_scope)); },
		[this, scope]() { edited_.receive_scope = static_cast<config::ReceiveScope>(scope->currentData().toInt()); });
	connect(scope, &QComboBox::currentIndexChanged, this, [this](int) { on_field_changed(); });

	received_group->setLayout(form);
	v->addWidget(received_group);

	// 「Vesktopでのエンコード」の組。送信側のエンコードの設定。続いている画面共有には適用されず、次に始まる画面共有から
	// 反映される (説明に書く)。
	auto *encoding_group = new QGroupBox(page_group_title(keys::kSettingsHeadingVesktopEncodingHeading));
	form = make_form();
	auto *encoder = new QComboBox();
	encoder->addItem(tr_key(keys::kSettingsVideoEncoderHardwareOption),
			 static_cast<int>(config::EncoderPreference::Hardware));
	encoder->addItem(tr_key(keys::kSettingsVideoEncoderSoftwareOption),
			 static_cast<int>(config::EncoderPreference::Software));
	encoder->setToolTip(tr_key(keys::kSettingsVideoEncoderTip));
	add_row(form, keys::kSettingsVideoEncoderLabel, encoder);
	register_field(
		encoder, "encoder_preference",
		[this, encoder]() { set_combo_data(encoder, static_cast<int>(edited_.encoder_preference)); },
		[this, encoder]() {
			edited_.encoder_preference = static_cast<config::EncoderPreference>(encoder->currentData().toInt());
		});
	connect(encoder, &QComboBox::currentIndexChanged, this, [this](int) { on_field_changed(); });

	auto *priority = new QComboBox();
	priority->addItem(tr_key(keys::kSettingsEncodePriorityLatencyOption),
			  static_cast<int>(config::EncodePriority::Speed));
	priority->addItem(tr_key(keys::kSettingsEncodePriorityQualityOption),
			  static_cast<int>(config::EncodePriority::Quality));
	priority->setToolTip(tr_key(keys::kSettingsEncodePriorityTip));
	add_row(form, keys::kSettingsEncodePriorityLabel, priority);
	register_field(
		priority, "encode_priority",
		[this, priority]() { set_combo_data(priority, static_cast<int>(edited_.encode_priority)); },
		[this, priority]() {
			edited_.encode_priority = static_cast<config::EncodePriority>(priority->currentData().toInt());
		});
	connect(priority, &QComboBox::currentIndexChanged, this, [this](int) { on_field_changed(); });

	encoding_group->setLayout(form);
	v->addWidget(encoding_group);
	v->addStretch(1);
	return page;
}

// ---- 5. 詳細設定 ----

QWidget *SettingsWindow::build_advanced_page()
{
	auto *page = new QWidget();
	auto *v = new QVBoxLayout(page);

	// 映像と音声のまとまり。受け取った映像と声の形とタイミングに関わる値 (デコード、音声のずれと遅延)。
	{
		auto *group = new QGroupBox(page_group_title(keys::kSettingsHeadingVideoAudioHeading));
		auto *form = make_form();

		auto *decoder = new QComboBox();
		decoder->addItem(tr_key(keys::kSettingsVideoDecodingSoftwareOption),
				 static_cast<int>(config::DecoderChoice::Software));
		decoder->addItem(tr_key(keys::kSettingsVideoDecodingHardwareOption),
				 static_cast<int>(config::DecoderChoice::D3D11VA));
		add_row(form, keys::kSettingsVideoDecodingLabel, decoder);
		register_field(
			decoder, "decoder", [this, decoder]() { set_combo_data(decoder, static_cast<int>(edited_.decoder)); },
			[this, decoder]() {
				edited_.decoder = static_cast<config::DecoderChoice>(decoder->currentData().toInt());
			});
		connect(decoder, &QComboBox::currentIndexChanged, this, [this](int) { on_field_changed(); });

		auto *offset = make_spin(config::kScreenAudioOffsetMinMs, config::kScreenAudioOffsetMaxMs,
					 keys::kSettingsUnitMsLabel);
		offset->setToolTip(tr_key(keys::kSettingsScreenAudioOffsetTip));
		add_row(form, keys::kSettingsScreenAudioOffsetLabel, offset);
		register_field(
			offset, "screen_audio_offset_ms",
			[this, offset]() { offset->setValue(edited_.screen_audio_offset_ms); },
			[this, offset]() { edited_.screen_audio_offset_ms = offset->value(); });
		connect(offset, &QSpinBox::valueChanged, this, [this](int) { on_field_changed(); });

		group->setLayout(form);
		v->addWidget(group);
	}

	// ドックのまとまり。配信中の確認。メッセージと一覧の並びの時間、ドックの更新の周期は、設定ファイルでだけ変える。
	{
		auto *group = new QGroupBox(page_group_title(keys::kSettingsHeadingDockHeading));
		auto *form = make_form();

		// 配信中に、cordmux を無効にするときと [退出] とダミー映像を表示するときに、押した場所の隣で確認する。
		auto *confirm = new QCheckBox(tr_key(keys::kSettingsConfirmWhileStreamingCheck));
		confirm->setToolTip(tr_key(keys::kSettingsConfirmWhileStreamingTip));
		form->addRow(QString(), confirm);
		register_field(
			confirm, "confirm_while_streaming",
			[this, confirm]() { confirm->setChecked(edited_.confirm_while_streaming); },
			[this, confirm]() { edited_.confirm_while_streaming = confirm->isChecked(); });
		connect(confirm, &QCheckBox::toggled, this, [this](bool) { on_field_changed(); });

		group->setLayout(form);
		v->addWidget(group);
	}

	v->addStretch(1);
	return page;
}

// ---- 6. cordmux について ----

QWidget *SettingsWindow::build_about_page()
{
	auto *page = new QWidget();
	auto *v = new QVBoxLayout(page);

	// バージョン (選んでコピーできる。[コピー] で全部を 1 回でコピーする)。中身は update_about が入れる。
	{
		auto *group = new QGroupBox(page_group_title(keys::kSettingsHeadingVersionsHeading));
		auto *gv = new QVBoxLayout(group);
		versions_box_ = new QWidget();
		gv->addWidget(versions_box_);
		auto *copy_row = new QHBoxLayout();
		auto *copy = new QPushButton(tr_key(keys::kSettingsCopyButton));
		copy_button_ = copy;
		copy_row->addWidget(copy);
		copy_row->addStretch(1);
		gv->addLayout(copy_row);
		connect(copy, &QPushButton::clicked, this, [this]() { copy_versions(); });
		v->addWidget(group);
	}

	// 診断情報。結果はボタンの下に表示し、設定ウィンドウを閉じるまで残す。
	{
		auto *group = new QGroupBox(page_group_title(keys::kSettingsHeadingDiagnosticsHeading));
		auto *gv = new QVBoxLayout(group);
		// 「起動時のVesktopの設定」の行。バージョンではない値なので、「バージョン」ではなく「診断情報」のまとまりに置く。
		// 中身は update_about が入れる。
		vesktop_box_ = new QWidget();
		vesktop_box_->setVisible(false);
		gv->addWidget(vesktop_box_);
		auto *row = new QHBoxLayout();
		export_button_ = new QPushButton(tr_key(keys::kSettingsExportDiagnosticsButton));
		row->addWidget(export_button_);
		row->addStretch(1);
		gv->addLayout(row);
		connect(export_button_, &QPushButton::clicked, this, [this]() { start_export(); });
		export_result_text_ = wrap_label(QString());
		export_result_text_->setVisible(false);
		gv->addWidget(export_result_text_);
		export_location_row_ = new QWidget();
		auto *lh = new QHBoxLayout(export_location_row_);
		lh->setContentsMargins(0, 0, 0, 0);
		// 場所は、区切り「\」の直後で折り返す、選べるラベルにする (全文が見え、選んでコピーでき、真ん中を省かない。
		// dock/ui/wrap_label.h の WrapLabel の PathSeparators で、確認と結果のダイアログの場所の行と同じ)。場所が長いときは
		// 行が増えるだけで、ページの幅を押し広げず (ラベルの最小の幅は 1 字の幅)、末尾の ZIP のファイル名も見える。
		// 読み取り専用の入力欄にすると、通常の長さの場所 (利用者名が 20 字) で末尾が切れる。1 行の選べる QLabel にすると、
		// 最小の幅が全文の幅になり、ページの右の枠とボタンが切れる。末尾から表示すると先頭のドライブと利用者のフォルダーが
		// 見えず、真ん中を省くと、見えているテキストとコピーするテキストが違う。
		export_location_ = new ui::WrapLabel();
		export_location_->set_breaks(ui::WrapBreaks::PathSeparators);
		export_location_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
		// 場所の長さでウィンドウの初めの幅を決めない (利用者名で長さが変わり、ラベルは現在の幅で折り返すので、どの幅でも
		// 全文が見える)。横の sizeHint を数えると、通常の長さの場所でウィンドウの初めの幅が 28 px 広がるが、広げても
		// 場所は 2 行のままである。
		export_location_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		// 1 行に収まる場所は、行の右のボタンより低い。WrapLabel の既定の上寄せでは、ラベルがボタンの高さまで伸び、
		// テキストがボタンのテキストより上に出て、下に空きができる。縦の中央に置くと、ボタンのテキストと高さがそろう
		// (折り返した場所は、ラベルの高さがテキストの高さなので変わらない。確認と結果のダイアログの場所の行 =
		// dock/ui/message_dialog.cpp の path_widget と同じ)。
		export_location_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
		export_open_button_ = new QPushButton(tr_key(keys::kSettingsOpenFolderButton));
		// ボタンは場所のラベルの縦の中央に置く (確認と結果のダイアログの場所の行 = dock/ui/message_dialog.cpp と同じ置き方)。
		lh->addWidget(export_location_, 1);
		lh->addWidget(export_open_button_, 0, Qt::AlignVCenter);
		export_location_row_->setVisible(false);
		gv->addWidget(export_location_row_);
		connect(export_open_button_, &QPushButton::clicked, this, [this]() {
			// ファイルの行は、エクスプローラーでそのファイルを選んで開く (前の診断情報の ZIP が並んでいても、今回のものが
			// 選ばれている)。関数がなければ (プレビューツール)、ファイルのあるフォルダーを開く ([録画] のページの
			// [フォルダーを開く] と同じ関数)。
			if (export_location_is_file_ && input_.reveal_file)
				input_.reveal_file(export_location_path_);
			else
				ui::open_folder(QString::fromUtf8(export_location_path_.c_str()));
		});
		v->addWidget(group);
	}

	v->addStretch(1);

	// アンインストールのまとまり。ページのいちばん下に置き、ほかのボタンと 1 em 以上離す (取り消せない操作は、ほかの
	// 操作から離して置く)。
	{
		const double em = ui::base_point_size(page) * page->logicalDpiY() / 72.0;
		v->addSpacing(static_cast<int>(std::lround(std::max(1.0, em) * 1.0)));
		auto *group = new QGroupBox(page_group_title(keys::kSettingsHeadingUninstallHeading));
		auto *gv = new QVBoxLayout(group);
		auto *row = new QHBoxLayout();
		uninstall_button_ = new QPushButton(tr_key(keys::kSettingsUninstallButton));
		// テキストはアンインストールの最中に「アンインストール中...」に変わるので、テストがテキストに依らずに探せる
		// objectName を付ける (理由のラベルも同じ)。
		uninstall_button_->setObjectName(QStringLiteral("cordmuxUninstallButton"));
		row->addWidget(uninstall_button_);
		row->addStretch(1);
		gv->addLayout(row);
		// 押せない理由は、ボタンのすぐ下に 1 文で表示する。
		uninstall_reason_ = wrap_label(QString());
		uninstall_reason_->setObjectName(QStringLiteral("cordmuxUninstallReason"));
		uninstall_reason_->setVisible(false);
		gv->addWidget(uninstall_reason_);
		connect(uninstall_button_, &QPushButton::clicked, this, [this]() { press_uninstall(focusWidget()); });
		v->addWidget(group);
	}
	return page;
}

void SettingsWindow::update_about(const ledger::dockview::AboutPageView &about)
{
	const bool first = !about_shown_;
	about_shown_ = true;

	// バージョンの一覧 (変わったときだけ作り直す)。
	if (first || about.versions != about_.versions) {
		delete versions_box_->layout();
		for (QWidget *child : versions_box_->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
			delete child;
		auto *form = make_form();
		form->setContentsMargins(0, 0, 0, 0);
		for (const ledger::dockview::VersionRow &row : about.versions) {
			auto *value = selectable_label(version_value_text(row));
			// 読めない値は「-」と表示する。スクリーンリーダーでは記号のまま読ませず、「取得できません」と読ませる。
			if (!row.value.literal.empty() && row.value.literal == "-")
				value->setAccessibleName(tr_key(keys::kSettingsUnavailableA11y));
			auto *label = new QLabel(tr_ref(row.label));
			label->setTextFormat(Qt::PlainText);
			label->setBuddy(value);
			form->addRow(label, value);
		}
		versions_box_->setLayout(form);
	}

	// 「起動時のVesktopの設定」の行 (「診断情報」のまとまりの先頭。変わったときだけ作り直す)。
	if (first || about.vesktop_rewrote_at_startup != about_.vesktop_rewrote_at_startup) {
		delete vesktop_box_->layout();
		for (QWidget *child : vesktop_box_->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
			delete child;
		auto *v = new QVBoxLayout();
		v->setContentsMargins(0, 0, 0, 0);
		if (about.vesktop_rewrote_at_startup.has_value()) {
			// 「名前: 値」の 1 行にする (同じページの「確認した日: ...」と同じ形)。ラベルと値の 2 つのウィジェットを並べる
			// 形では、1 組だけの行には列がなく、英語で "Vesktop Settings at Startup Not changed" と 1 つの句に読める。
			ledger::TextRef line;
			line.id = keys::kSettingsStartupVesktopSettingsStatus;
			line.args = {tr_ref(*about.vesktop_rewrote_at_startup).toStdString()};
			auto *value = selectable_label(tr_ref(line));
			value->setWordWrap(true);
			v->addWidget(value);
		}
		vesktop_box_->setLayout(v);
		vesktop_box_->setVisible(about.vesktop_rewrote_at_startup.has_value());
	}

	// エクスポートの結果。ここに表示するのは成功だけで、失敗はダイアログで伝える。
	if (first || about.export_result != about_.export_result) {
		if (about.export_result.has_value()) {
			export_result_text_->set_text(tr_ref(about.export_result->text));
			export_result_text_->setVisible(true);
			const bool has_location = about.export_result->location.has_value();
			export_location_path_ = has_location ? about.export_result->location->path : std::string();
			// 場所の種類でボタンを選ぶ (ファイルの場所には [ファイルの場所を開く]、フォルダーには [フォルダーを開く])。
			// 種類は View が決める (dock_view.h の LocationLine)。エクスポートの結果は ZIP だとしてウィンドウが種類を決めると、
			// 場所の種類を決める所が View とウィンドウの 2 つになる。
			export_location_is_file_ =
				has_location && about.export_result->location->kind == ledger::dockview::LocationKind::File;
			export_open_button_->setText(tr_key(export_location_is_file_ ? keys::kDialogOpenFileLocationButton
										  : keys::kSettingsOpenFolderButton));
			export_location_->set_text(QString::fromStdString(export_location_path_));
			// アクセシブル名は場所 (WrapLabel が元のテキストを入れるので、改行は読まない)、accessibleDescription は上の
			// 結果の 1 文にする (ラベルにフォーカスが来たときに、何の場所かが分かる)。全文が見えるので、ツールチップは
			// 付けない。名前を結果の 1 文にすると、場所が読まれなくなる。
			export_location_->setAccessibleDescription(tr_ref(about.export_result->text));
			export_location_row_->setVisible(has_location);
		} else {
			export_result_text_->set_text(QString());
			export_result_text_->setVisible(false);
			export_location_path_.clear();
			export_location_row_->setVisible(false);
		}
	}

	// アンインストールの段階が前の周期から進んだか (ウィンドウに見える形で判断する。入力欄を操作できなくしたかと、
	// ボタンのテキスト)。下で、押せなくなったボタンのフォーカスの移し先を、アンインストールを始めた場合と、断られた場合や
	// 押せなくなった場合とで分ける。
	const bool uninstall_stage_moved = !first && (about.settings_locked != about_.settings_locked ||
						      about.uninstall_button.label != about_.uninstall_button.label);
	const ledger::TextRef uninstall_label_before = about_.uninstall_button.label;
	if (uninstall_press_.active && (about.settings_locked != uninstall_press_.locked_at_press ||
					about.uninstall_button.label != uninstall_press_.label_at_press))
		uninstall_press_.stage_moved = true;

	about_ = about;
	set_export_busy(export_busy_);

	// アンインストールのボタンと押せない理由。
	uninstall_button_->setText(tr_ref(about.uninstall_button.label));
	// 押せるかは、[ツール] の「cordmuxをアンインストール...」から来たとき (start_uninstall) と同じ判定で決める
	// (windows_calc.h の uninstall_entry)。
	const bool can_uninstall =
		windows_calc::uninstall_entry(about.uninstall_button) == windows_calc::UninstallEntry::Confirm;
	// フォーカスのあるボタンが押せなくなるとき (確認のダイアログで [アンインストール] を押した後と、アンインストール中
	// から済んだ後) は、フォーカスをこのページの最初のウィジェット ([コピー]) に移す (確認と結果のダイアログの
	// focus_fallback と同じ所)。Qt に任せると、フォーカスは Tab の順で次の [OK] に移り、結果のダイアログを閉じた後の
	// フォーカスも [OK] に戻る (Enter で設定ウィンドウが閉じてしまう)。ウィンドウがアクティブでなくても、ウィンドウの
	// 中のフォーカスの位置だけを変える。ボタンを押せるままにしてクリックを捨てると、押せないことが見た目と
	// スクリーンリーダーに伝わらない。
	// 移した後に見える範囲 (小さいウィンドウでは、いちばん下を見ている間 [コピー] は見える範囲の外にある) は、押せなく
	// なった理由で分ける。
	// - アンインストールを始めた (段階が進んだ): フォーカスは [コピー] に置く。アンインストールが済んだ周期 (ボタンの
	//   テキストが元に戻る) で [コピー] までスクロールする (下の reveal_copy_after_label_)。アンインストールの最中は
	//   スクロールしない (押したボタンのテキスト「アンインストール中...」が進み具合を示すので、押した所の近くを見せて
	//   おく)。結果のダイアログは開いたときのフォーカス ([コピー]) に戻す (dock/ui/message_dialog.cpp) ので、閉じた後の
	//   フォーカスは見える所にある。
	// - 断られた、または押せなくなった (段階が同じで理由が表示された): [コピー] が見える範囲の外なら項目の一覧に移す
	//   (settle_focus_after_refusal)。押していないときはスクロールしない (フォーカスの位置だけを修正し、押す直前の表示を
	//   動かさない)。押した後の周期は、下の uninstall_press_ の所で、いちばん下 (理由) までスクロールする。
	// どちらの場合も [コピー] までスクロールすると、断られたときに、ボタンのすぐ下の理由が小さいウィンドウで見えなくなる。
	bool refused_while_focused = false;
	if (!can_uninstall && focusWidget() == uninstall_button_ && copy_button_ != nullptr) {
		copy_button_->setFocus(Qt::OtherFocusReason);
		if (uninstall_stage_moved || uninstall_press_.stage_moved)
			reveal_copy_after_label_ = uninstall_press_.active ? uninstall_press_.label_at_press : uninstall_label_before;
		else
			refused_while_focused = true;
	}
	uninstall_button_->setEnabled(can_uninstall);
	if (about.uninstall_button.disabled_reason.has_value()) {
		uninstall_reason_->set_text(tr_ref(*about.uninstall_button.disabled_reason));
		uninstall_reason_->setVisible(true);
	} else {
		uninstall_reason_->set_text(QString());
		uninstall_reason_->setVisible(false);
	}
	update_lock();

	// 押した後の最初の周期 (押した関数から返った後。ページのボタンと [ツール] の両方)。押した後に段階が進まず
	// (アンインストールを始めておらず)、押せない理由が表示されていれば、押した関数が状態を取り直して断った場合である
	// (周期の間か確認のダイアログの間に、配信か録画を始めた場合。途中で止まった後に押し直した場合も同じで、入力欄は
	// 操作できないままなので、入力欄を操作できなくしたかでは見分けられない)。押せないときの処理 (start_uninstall の途中の
	// return) と同じ形にする。理由まで見えるようにいちばん下までスクロールし直し、フォーカスは押す前の位置が見えて
	// いればそこに、なければ [コピー] が見えていれば [コピー] に、どちらも見える範囲の外なら項目の一覧に置く
	// (settle_focus_after_refusal)。利用者は [ツール] から選んだだけで、ボタンにフォーカスを置いたのは cordmux の都合
	// である。確認のダイアログを [キャンセル] で閉じたときは理由が表示されないので、何もしない (フォーカスは確認の
	// ダイアログがボタンに戻す)。
	// スクロールし直さないと、理由の行が見える範囲の外に出る。フォーカスを [コピー] のままにすると、小さいウィンドウ
	// では見えない所にフォーカスがあり、フォーカスリングも見えない (WCAG 2.4.11)。段階を「入力欄を操作できなくしたか」
	// だけで見ると、途中で止まった後に押し直して断られた場合を、アンインストールを始めた場合と見分けられない。
	// ただし、ドックを閉じる途中 (dock_poll_install.cpp の after_paint が、周期を進めずにアンインストールする) で、
	// 途中で止まった後の押し直しが済んだときは、押している間にアンインストール中のテキストを見ないので、断られた
	// 場合と同じ形 (いちばん下までスクロールし、フォーカスは見える所) になる。
	if (uninstall_press_.active && uninstall_press_.returned) {
		const UninstallPress press = uninstall_press_;
		uninstall_press_ = UninstallPress{};
		if (!press.stage_moved && about.uninstall_button.disabled_reason.has_value())
			settle_focus_after_refusal(press.focus_before.data(), true);
	} else if (refused_while_focused) {
		settle_focus_after_refusal(nullptr, false);
	}

	// アンインストールが済んだ周期 (上の reveal_copy_after_label_ のコメント)。フォーカスが [コピー] のままのときだけ
	// スクロールする (利用者が別の所に動かしたフォーカスは追わない)。スクロールは、Tab でフォーカスが移ったウィジェットと
	// 同じ形にする (reveal_mark: ボタンとフォーカスリングが入るまで)。QScrollArea::ensureWidgetVisible は、ボタンの全体が
	// 見えていれば、リングの 1 辺が切れていてもスクロールしない。
	if (reveal_copy_after_label_.has_value() && about.uninstall_button.label == *reveal_copy_after_label_) {
		reveal_copy_after_label_.reset();
		QScrollArea *scroll = about_scroll();
		if (scroll != nullptr && copy_button_ != nullptr && focusWidget() == copy_button_) {
			QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
			reveal_mark(scroll, copy_button_);
		}
	}
}

QWidget *SettingsWindow::about_focus_fallback() const
{
	return copy_button_;
}

void SettingsWindow::press_uninstall(QWidget *focus_before)
{
	// 押せないボタンはクリックを受けないが、[ツール] から来たときと同じ判定をここでも通す (押せるかの元は View の 1 つ)。
	if (windows_calc::uninstall_entry(about_.uninstall_button) != windows_calc::UninstallEntry::Confirm)
		return;
	if (!input_.on_uninstall)
		return;
	// 押す前の段階とフォーカスを保存する (update_about の終わりで使う)。押した関数の中で周期が進む (アンインストールを
	// 始めた場合と、確認のダイアログの間) ので、呼び出す前に保存する。
	uninstall_press_ = UninstallPress{};
	uninstall_press_.active = true;
	uninstall_press_.locked_at_press = about_.settings_locked;
	uninstall_press_.label_at_press = about_.uninstall_button.label;
	uninstall_press_.focus_before = focus_before;
	// 確認のダイアログが開いている間はメインウィンドウとドックを操作できるので、このウィンドウは削除されることがある
	// (OBS を閉じたときなど。apply() のコメントと同じ)。返った後は、このウィンドウが残っているときだけ自分に触る。
	const QPointer<SettingsWindow> alive(this);
	input_.on_uninstall(this);
	if (alive.isNull())
		return;
	uninstall_press_.returned = true;
}

void SettingsWindow::start_uninstall()
{
	// ページの名前を読まなくても、取り消せない操作のボタン (ページのいちばん下に、ほかのボタンから 1 em 離して置いて
	// ある) が見えるようにする。押せても押せなくてもスクロールする。押せるときは確認のダイアログの後ろにボタンが見えるので、
	// 次からはこのページのボタンからも始められると分かる。押せないときは、確認のダイアログの代わりに、ボタンの下の
	// 理由が見えている。
	// 押せるときは、フォーカスをボタンに置いてから押す (マウスやキーボードで押したときと同じく、確認のダイアログを
	// 閉じた後のフォーカスがボタンに戻る)。押せないときはフォーカスをボタンに動かさず (見える範囲の外になったフォーカス
	// だけを置き直す。下の settle_focus_after_refusal)、スクリーンリーダーへの通知も出さない (理由はフォーカスの届かない
	// ラベルにある)。スクリーンリーダーで理由が聞こえるかは確認していない。押せないときも確認のダイアログを開き、
	// [アンインストール] を押した後で断ると、押しても始まらないダイアログを挟むことになる。押せないときに何もしないと、
	// 選んだのに何も起きない。スクロールせずにページを開くだけだと、中身がウィンドウより高いときにボタンも理由も
	// 見えない。
	set_page(Page::About);
	show_about_bottom();
	if (windows_calc::uninstall_entry(about_.uninstall_button) != windows_calc::UninstallEntry::Confirm) {
		// 別のページの入力欄にフォーカスがあった場合は、ページを替えたときにフォーカスを項目の一覧に移してある
		// (show_stack_page)。一覧はスクロール領域の外でいつも見えているので、下の置き直しはそのままにする。フォーカスが
		// 「cordmux について」の中にあった場合 (ページは替わらない) は、見える範囲の外になったフォーカスだけを置き直す
		// (スクロールは上で済んでいる)。
		settle_focus_after_refusal(nullptr, false);
		return;
	}
	QWidget *focus_before = focusWidget();
	uninstall_button_->setFocus(Qt::OtherFocusReason);
	// 押せる見た目でも、押した関数 (DockPollInstallPart::uninstall) は配信と録画の状態を取り直し、周期の間に始めていれば
	// 確認のダイアログを開かずに断る。断ったことは、次の周期の update_about で理由の行として追加され、中身がその分高く
	// なる。スクロールの位置は追加する前のいちばん下のままなので、理由の行が見える範囲の外に出る。押した後の最初の周期で、
	// 押せないときと同じ形 (いちばん下までスクロールし、フォーカスは押す前の位置か見える所) にそろえる (update_about の
	// 終わり)。
	press_uninstall(focus_before);
}

QScrollArea *SettingsWindow::about_scroll() const
{
	return page_scroll(page_index(Page::About));
}

bool SettingsWindow::shown_in_about_view(QWidget *w) const
{
	if (w == nullptr || !w->isVisible())
		return false;
	QScrollArea *scroll = about_scroll();
	QWidget *content = scroll != nullptr ? scroll->widget() : nullptr;
	if (content == nullptr || !content->isAncestorOf(w))
		return true;
	// スクロール領域の見えている範囲に、ウィジェットの全体とフォーカスリングの縦の範囲が入っているか (一部が切れた
	// ウィジェットはリングも切れる。ウィジェットの全体が見えていても、端から「太さ + 空き」以内なら、リングの 1 辺が
	// 切れる)。ドックを操作するテストの shown_in_viewport (ウィジェットの全体) よりリングの幅だけ厳しいので、ここで true
	// なら、テストの判定も true になる。
	return mark_shown_in(scroll, w);
}

void SettingsWindow::settle_focus_after_refusal(QWidget *preferred, bool scroll)
{
	// アンインストールを断られた場合と、押せない場合。いちばん下 (ボタンとその下の理由) を見せたまま、フォーカスは見える
	// 所に置く。順は、押す前の位置 (見えていれば。利用者が押す前にいた所)、[コピー] (見えていれば。そのページの最初の
	// ウィジェット)、項目の一覧 (スクロール領域の外で、いつも見えている。現在のページの「cordmux について」が選ばれている)。
	// 置き直すのは、cordmux が動かしたフォーカス (押せなくなったボタンから [コピー] へ) か、見える範囲の外のフォーカス
	// だけにする (利用者が自分で置いた見えるフォーカスと、ページを替えたときに一覧に移したフォーカス = show_stack_page は
	// 動かさない)。「見える」は、ウィジェットの全体とフォーカスリングの縦の範囲が見える範囲にあること
	// (shown_in_about_view)。利用者が置いたフォーカスでも、端に寄ってリングの 1 辺が切れていれば置き直す。リングが切れた
	// ままでは、フォーカスの位置が分からない。断られたときも [コピー] までスクロールすると、小さいウィンドウで理由が
	// 見えなくなる。フォーカスを [OK] などのボタンの並びに置くと、Enter でウィンドウが閉じてしまう。
	if (scroll)
		show_about_bottom();
	else
		QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
	QWidget *now = focusWidget();
	const bool moved_by_cordmux = now == nullptr || now == copy_button_ || now == uninstall_button_;
	if (!moved_by_cordmux && shown_in_about_view(now))
		return;
	if (preferred != nullptr && preferred != uninstall_button_ && preferred->isEnabled() && isAncestorOf(preferred) &&
	    shown_in_about_view(preferred)) {
		if (preferred != now)
			preferred->setFocus(Qt::OtherFocusReason);
		return;
	}
	if (copy_button_ != nullptr && copy_button_->isEnabled() && shown_in_about_view(copy_button_)) {
		if (copy_button_ != now)
			copy_button_->setFocus(Qt::OtherFocusReason);
		return;
	}
	page_list_->setFocus(Qt::OtherFocusReason);
}

void SettingsWindow::show_about_bottom()
{
	QScrollArea *scroll = about_scroll();
	if (scroll == nullptr)
		return;
	// 押せない理由を現在の周期で表示した直後は、レイアウトの要求がたまっていて、スクロールの範囲が古い。先にレイアウトを
	// やり直してから、いちばん下までスクロールする (範囲が古いと、理由の行の分だけ届かない)。
	QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
	QScrollBar *bar = scroll->verticalScrollBar();
	bar->setValue(bar->maximum());
}

void SettingsWindow::copy_versions()
{
	// 「バージョン」のまとまりの全部の行を、「名前 TAB 値」で 1 行ずつコピーする。区切りは表の形 (タブ) にし、画面の
	// テキストの区切りは使わない。「起動時のVesktopの設定」の行は「診断情報」のまとまりにあるのでコピーしない (ボタンの
	// あるまとまりに見えている行だけをコピーする)。
	QString text;
	for (const ledger::dockview::VersionRow &row : about_.versions) {
		text += tr_ref(row.label);
		text += QLatin1Char('\t');
		text += version_value_text(row);
		text += QLatin1Char('\n');
	}
	QGuiApplication::clipboard()->setText(text);
}

void SettingsWindow::start_export()
{
	if (export_busy_ || about_.export_button.disabled_reason.has_value())
		return;
	set_page(Page::About);
	set_export_busy(true);
	// 終わったら、フォーカスを [診断情報をエクスポート...] に戻す。ボタンを押せなくすると Qt がフォーカスを次の
	// ウィジェットに移し、押せるようにしても戻さない。ドックのメニューや、メッセージのレベル 1 のボタンから来たとき (ボタンに
	// フォーカスがなかったとき) も、ページのボタンが最後のフォーカスの位置になる。戻すのは、終わったときにウィンドウの
	// フォーカスがこの時点 (押せなくした後) と同じとき、つまり利用者が処理の間にフォーカスを動かしていないときだけにする
	// (ドックの録画のボタンの focus_moved_to_ と同じ考え方。bottom_bar.cpp)。ウィンドウがアクティブでなければ、ウィンドウの
	// 中のフォーカスが変わるだけで、ウィンドウを前面に出しはしない。押せなくせずにテキストだけを変えてクリックを捨てると、
	// 押せないことが見た目とスクリーンリーダーに伝わらない。
	export_return_focus_ = true;
	export_focus_mark_ = focusWidget();
	if (input_.on_export)
		input_.on_export(this);
	else
		set_export_busy(false);
}

void SettingsWindow::set_export_busy(bool busy)
{
	export_busy_ = busy;
	if (export_button_ == nullptr)
		return;
	// 押したら 0.1 秒以内に見た目を変える。処理の間は、ボタンのテキストを「エクスポート中...」にして押せなくする。
	if (busy)
		export_button_->setText(tr_key(keys::kSettingsExportDiagnosticsBusyButton));
	else if (!about_.export_button.label.id.empty())
		export_button_->setText(tr_ref(about_.export_button.label));
	else
		export_button_->setText(tr_key(keys::kSettingsExportDiagnosticsButton));
	export_button_->setEnabled(!busy && !about_.export_button.disabled_reason.has_value());
	if (about_.export_button.disabled_reason.has_value())
		export_button_->setToolTip(tr_ref(*about_.export_button.disabled_reason));
	else
		export_button_->setToolTip(QString());
	if (!busy && export_return_focus_) {
		export_return_focus_ = false;
		if (export_button_->isEnabled() && focusWidget() == export_focus_mark_.data())
			export_button_->setFocus(Qt::OtherFocusReason);
		export_focus_mark_.clear();
	}
}

void SettingsWindow::update_lock()
{
	// アンインストールの後は、入力欄を操作できなくする (書いても残らないため)。「cordmux について」のページは操作できる
	// ままにする (結果を読み直し、押し直すため)。
	for (QWidget *page : locked_pages_)
		page->setEnabled(!about_.settings_locked);
	update_apply_enabled();
}

// ---- 値の読み込みと書き込み ----

void SettingsWindow::load_fields()
{
	loading_ = true;
	for (Field *field : fields_)
		field->load();
	loading_ = false;
	update_record_location();
}

void SettingsWindow::on_field_changed()
{
	if (loading_)
		return;
	for (Field *field : fields_)
		field->store();
	update_restart_notes();
	update_apply_enabled();
}

void SettingsWindow::update_restart_notes()
{
	for (const auto &[page, note] : restart_notes_)
		note->setVisible(restart_pending_on_page(input_.effective_at_start, edited_, page));
}

bool SettingsWindow::dirty() const
{
	return save_failed_ || config::settings_to_json(edited_) != config::settings_to_json(applied_);
}

void SettingsWindow::update_apply_enabled()
{
	// [適用] は反映していない変更があるときだけ押せる (OBS の [設定] と同じ)。
	if (apply_button_ != nullptr)
		apply_button_->setEnabled(dirty() && !about_.settings_locked);
}

bool SettingsWindow::apply()
{
	config::Settings next = edited_;
	// ウィンドウから入る値の最後のガード。入力欄そのものが範囲を持つので、通常は何も変わらない。
	config::clamp_settings(next);
	bool saved = true;
	if (input_.on_apply)
		saved = input_.on_apply(next);
	// 保存できなくても、値は反映されている。この起動の間は新しい値を使い、次に起動したときに元の値に戻る。
	applied_ = next;
	save_failed_ = !saved;
	update_apply_enabled();
	if (!saved) {
		// 失敗のダイアログが入力を止めるのは、このウィンドウだけにする (OwnerOnly)。配信中に OBS のメインウィンドウと
		// ドック (フローティングのドックも) を止めないためである。ダイアログはこのウィンドウの子で、このウィンドウの上に
		// 表示し、タスクバーに別のボタンを持たず、このウィンドウを押すと前面に出る。Qt::WindowModal にすると、Qt は親の
		// 連なりの全部を止め、親は OBS のメインウィンドウなので、メインウィンドウとドックも止まる。設定ウィンドウを
		// 親なしにすると、OBS を押したときに設定ウィンドウが OBS の後ろに回り、タスクバーに別のボタンが出る。
		// ダイアログが開いている間はメインウィンドウとドックを操作できるので、このウィンドウは削除されることがある (OBS を
		// 閉じたときなど)。run_message_dialog から返った後は、このウィンドウのものに触らない (返すのはローカル変数の値
		// だけ。呼び出し元の [OK] と [適用] も、保存できなかったときは何もしない)。
		ui::MessageSpec spec = settings_save_failed_spec();
		spec.blocking = ui::MessageBlocking::OwnerOnly;
		ui::run_message_dialog(this, spec);
	}
	return saved;
}

void SettingsWindow::set_page(Page page)
{
	const int index = page_index(page);
	// 一覧の行が変われば、currentRowChanged から show_stack_page が呼ばれる。変わらなかった (同じ行の) ときのために、ここ
	// でも呼ぶ (同じページなら何もしない)。
	page_list_->setCurrentRow(index);
	show_stack_page(index);
}

void SettingsWindow::show_stack_page(int index)
{
	if (index == stack_->currentIndex())
		return;
	// ページを替えるとき、フォーカスが替える前のページの中にあれば、先に項目の一覧に移す。一覧で選んだときはフォーカスが
	// 一覧にあるので動かない (OBS の [設定] と同じ。OBS 31 の frontend/settings/OBSBasicSettings.cpp の
	// on_listWidget_itemSelectionChanged はページを替えるだけで、フォーカスに触らない)。ここで移すのは、フォーカスがページの
	// 中にあるまま cordmux がページを替えた場合 (ドックのメニューから別のページを開いた場合、[ツール] からの
	// アンインストール、エクスポート) と、スクリーンリーダーが一覧の行を選んだ場合である。一覧では現在のページの行が選ばれて
	// いるので、フォーカスリングが今いる所を示し、Tab 1 回でページの最初のウィジェットに入れる。フォーカスがページの外
	// (一覧、[OK] の並び) にあるときと、フォーカスがない (開く前の) ときは動かさない。
	// Qt に任せると、QStackedLayout::setCurrentIndex がフォーカスを新しいページの最初のウィジェットに移すので、ページに
	// よってはスクロール領域の外のウィジェットにフォーカスが移る。新しいページの最初の入力欄に移すと、利用者が選んで
	// いない入力欄にフォーカスが入り、一覧で選んだときと位置が食い違う。
	QWidget *focus = focusWidget();
	QWidget *current = stack_->currentWidget();
	if (focus != nullptr && current != nullptr && current->isAncestorOf(focus))
		page_list_->setFocus(Qt::OtherFocusReason);
	stack_->setCurrentIndex(index);
}

bool SettingsWindow::focusNextPrevChild(bool next)
{
	if (!QDialog::focusNextPrevChild(next))
		return false;
	// 移った先が現在のページのスクロール領域の中なら、見える所までスクロールする。スクロール領域がフォーカスを受けない
	// ので、一覧から Tab で入った場合と、[OK] の並びから Shift+Tab で戻った場合は、Qt の QScrollArea::focusNextPrevChild
	// (スクロール領域の中で移るときだけ通る) がスクロールせず、いちばん下までスクロールしたページでフォーカスが見える
	// 範囲の外に出る。
	// ウィジェットの全体とフォーカスリング (押せる範囲の外に「太さ + 空き」だけはみ出し、スクロール領域の見える範囲で
	// 切って描く。focus_ring.cpp の place) の縦の範囲が見えていなければ、全部が入るまでスクロールする (reveal_mark)。
	// ウィジェットが見えていても、リングの 1 辺が切れる (ウィジェットが、見える範囲の上か下の端から「太さ + 空き」以内に
	// ある) ときもスクロールする。リングの横は見ない (横にはスクロールせず、縦にスクロールしても横は直らない。横まで
	// 比べると、横にリングがはみ出すウィジェットで Tab のたびにスクロールする)。余白は Qt の ensureWidgetVisible の既定と
	// 同じ 50 px にする (スクロール領域の中で移る場合も同じ量で動く)。ウィジェットとリングが見える範囲より高ければ、
	// リングの上端に合わせる。
	// QScrollArea::ensureWidgetVisible を呼ぶと、入力欄と数の入力欄ではテキストカーソルの矩形だけを見える所に入れ、
	// 入力欄の枠の下が切れたまま止まる (ウィジェットの全体が見えていれば、リングが切れていても何もしない)。スクロール
	// 領域の中で移る場合も Qt はこの関数を呼ぶので、ここで全体を入れておけば、Qt の呼び出しは何もせずに返る。フォーカスが
	// 移るたびにスクロールする方法 (QApplication::focusChanged) は使わない。アンインストールを始めたときに [コピー] に
	// 移したフォーカスを済むまでスクロールしない処理 (reveal_copy_after_label_) と、断られたときに見える所だけに置く
	// 処理 (settle_focus_after_refusal) が崩れる。
	QWidget *focus = focusWidget();
	QScrollArea *scroll = page_scroll(stack_->currentIndex());
	QWidget *content = scroll != nullptr ? scroll->widget() : nullptr;
	if (focus == nullptr || content == nullptr || !content->isAncestorOf(focus))
		return true;
	reveal_mark(scroll, focus);
	return true;
}

// 判定とスクロールは、dock/ui/focus_ring.h の ui::focus_mark_shown と ui::reveal_focus_mark を呼ぶ。ドックと、確認と
// 結果のダイアログのスクロールも同じ関数を呼び、同じ判定にする。リングは、押せる範囲 (focus_hit_rect_of) を
// ring_reach_px_ だけ広げた矩形 (focus_ring.cpp の place)。中身の外にはみ出す分は、どうスクロールしても見える範囲に
// 入らないので除く (中身の端に接するウィジェットで、見えるはずのない分を待って「見えていない」と判断し続けない)。
// ウィジェットの全体は縦と横で判定し、リングは縦だけを足す (focusNextPrevChild のコメント)。
bool SettingsWindow::mark_shown_in(const QScrollArea *scroll, const QWidget *w) const
{
	return ui::focus_mark_shown(scroll, w, ring_reach_px_);
}

void SettingsWindow::reveal_mark(QScrollArea *scroll, const QWidget *w)
{
	ui::reveal_focus_mark(scroll, w, ring_reach_px_);
}

Page SettingsWindow::page() const
{
	const int index = stack_->currentIndex();
	return index >= 0 && index < static_cast<int>(std::size(kPages)) ? kPages[index].page : Page::Recording;
}

void SettingsWindow::refresh_page_icons()
{
	// 項目の一覧のアイコンは、OBS の [設定] のアイコンのセットを使う。「cordmux について」は cordmux が描く。
	const ui::ThemeRead theme = ui::read_theme(this, nullptr);
	const int px = theme.icon_px > 0 ? theme.icon_px : 16;
	page_list_->setIconSize(QSize(px, px));
	const ui::SettingsIcon icons[] = {ui::SettingsIcon::Output, ui::SettingsIcon::Video, ui::SettingsIcon::Advanced};
	for (int i = 0; i < static_cast<int>(std::size(kPages)); ++i) {
		QIcon icon;
		if (kPages[i].page == Page::About)
			icon = draw_about_icon(px, theme.text);
		else
			icon = ui::settings_icon(theme, icons[i]);
		if (icon.isNull())
			icon = blank_icon(px);
		page_list_->item(i)->setIcon(icon);
	}
	// 一覧の幅は、中身 (最も長い名前とアイコン) に合わせる。
	page_list_->setFixedWidth(page_list_->sizeHintForColumn(0) + 2 * page_list_->frameWidth() +
				  page_list_->style()->pixelMetric(QStyle::PM_ScrollBarExtent) / 2);
}

void SettingsWindow::refresh_theme()
{
	refresh_page_icons();
	refresh_focus_ring();
	refresh_notes();
}

void SettingsWindow::add_description(QFormLayout *form, const QString &text)
{
	ui::WrapLabel *note = wrap_label(text);
	add_note_row(form, note);
	notes_.push_back({form, note});
}

void SettingsWindow::refresh_notes()
{
	// 説明の文は補足の色にする (ドックの表示タブの説明の文と同じ。docs/ui_guidelines.md の「色とフォント」)。まとまりの面
	// (QGroupBox) の色はテーマから読めないので、ダイアログの背景とコンテナーの背景のどちらの上でも読めるときだけにする。
	// 上下の余白は、フォームの行の間 (テーマの値。Yami では 6 px) のままにする。行の間が説明の文の下の余白 (0.5 em。10 pt で
	// 7 px) とほぼ同じで、余白のための入れ物 (ui::note_box) を挟むと、ウィンドウの初めの大きさの見積もりが変わるため。
	const ui::ThemeRead theme = ui::read_theme(this, nullptr);
	const bool muted = ui::muted_ok(theme, theme.window_bg) && ui::muted_ok(theme, theme.base_bg);
	const QString classes = muted ? QStringLiteral("text-muted") : QString();
	for (const Note &n : notes_) {
		// まだ polish していないラベル (ウィンドウを作っている途中) は、クラスのプロパティだけを入れて、最初の polish で
		// 反映させる。ここで polish し直すと、まだ配置されていない幅で WrapLabel が折り返す幅を決め直し、ウィンドウの
		// 初めの大きさ (initial_size。中身の sizeHint から決める) が変わる。
		if (n.label->testAttribute(Qt::WA_WState_Polished))
			ui::set_class(n.label, classes);
		else
			n.label->setProperty("class", classes.isEmpty() ? QVariant() : QVariant(classes));
	}
}

bool SettingsWindow::event(QEvent *event)
{
	// ウィジェットのフォントの大きさや色が変わった (テーマを読み込み直したとき)。
	if (page_list_ != nullptr && (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange))
		refresh_page_icons();
	// 別のウィンドウ (OBS の [設定]) で録画の保存先を変えて戻ってきたとき、選んでいない保存先の表示を現在の場所に合わせる。
	// 入力欄を編集している最中 (フォーカスがある間) は、入力途中の文字を動かさない。
	if (event->type() == QEvent::WindowActivate && record_dir_edit_ != nullptr && !record_dir_edit_->hasFocus() &&
	    !loading_)
		update_record_location();
	return QDialog::event(event);
}

void SettingsWindow::set_return_focus(QWidget *opener)
{
	// ウィンドウの中のウィジェットは戻し先にしない (ウィンドウの中のボタンから開き直したときは、前の戻し先を保つ)。
	if (opener != nullptr && opener->window() == this)
		return;
	return_focus_ = opener;
}

void SettingsWindow::done(int result)
{
	const bool was_active = isActiveWindow();
	if (!closed_reported_) {
		closed_reported_ = true;
		if (input_.on_closed)
			input_.on_closed(saveGeometry().toBase64().toStdString());
	}
	QDialog::done(result);
	return_focus_after_close(was_active, return_focus_, parentWidget() != nullptr ? parentWidget()->window() : nullptr);
}

void return_focus_after_close(bool was_active, const QPointer<QWidget> &opener, const QPointer<QWidget> &owner)
{
	// 閉じたウィンドウがフォーカスを持っていたときだけ戻す (利用者が OBS の別の所に移っていたら奪わない)。開いた
	// ウィジェットが削除されたか、隠れたか、押せなければ何もしない (Windows が前面にした親ウィンドウにフォーカスが残る)。
	// 戻すのはイベントループの次の周回にする。Windows は親ウィンドウを持つウィンドウを隠すと、その場で親ウィンドウ
	// (OBS のメインウィンドウ) を前面にするので、その後に、開いたウィジェットのウィンドウ (フローティングのドックなら、
	// そのウィンドウ) を前面にする。これをしないと、ドックのメニューから開いた設定ウィンドウを Esc で閉じたときに、
	// フォーカスがドックの [...] ではなくメインウィンドウに移る。設定ウィンドウの親をドックのウィンドウにする方法は
	// 使わない。親はウィンドウの前後の関係と、失敗のダイアログが止めるウィンドウの範囲にも関わるので、フォーカスの戻し先の
	// ためだけに変えない (親はメインウィンドウのまま。dock_poll_settings.cpp)。
	// 次の周回までに別のウィンドウが前面に出ていたら (閉じたのと同じ周回で別のウィンドウを開いたら) 何もしない。前面に
	// 出してよいのは、アクティブなウィンドウがないとき、Windows が前面にした親ウィンドウ (owner) のとき、開いたウィジェット
	// のウィンドウのとき、閉じて隠れたウィンドウ (Qt がアクティブの移動をまだ受けていない) のときだけである (フォーカスを
	// 奪わない)。
	if (!was_active || opener.isNull())
		return;
	QTimer::singleShot(0, opener.data(), [opener, owner]() {
		QWidget *w = opener.data();
		if (w == nullptr || !w->isVisible() || !w->isEnabled())
			return;
		QWidget *active = QApplication::activeWindow();
		if (active != nullptr && active->isVisible() && active != owner.data() && active != w->window())
			return;
		w->window()->activateWindow();
		w->setFocus(Qt::OtherFocusReason);
	});
}

SettingsWindow *make_settings_window(QWidget *parent, const SettingsWindowInput &in)
{
	return new SettingsWindow(parent, in);
}

// ---- 失敗のダイアログ ----

ui::MessageSpec export_failed_spec(bundle::ExportOutcome outcome)
{
	ui::MessageSpec spec;
	spec.title = tr_key(keys::kDialogExportTitle);
	spec.icon = ui::MessageIcon::Warning;
	spec.main = tr_key(keys::kDialogExportFailedHeadline);
	// 書けなかったときだけ理由を伝える。zip を作れなかったときと、秘密の値が残っていたときは、利用者にできることが
	// ないので、次の操作だけを伝える (理由はログにだけ書く)。
	spec.supplement = outcome == bundle::ExportOutcome::WriteFailed ? tr_key(keys::kDialogExportUnwritableText)
									: tr_key(keys::kDialogExportRetryText);
	// ダイアログの id (dialogId)。テストとプレビューツールが、製品の処理で開いたダイアログを見分けるのに使う。綴りは
	// プレビューツールの --face dialog:<id> と同じ。
	switch (outcome) {
	case bundle::ExportOutcome::WriteFailed:
		spec.dialog_id = QStringLiteral("export-failed-write");
		break;
	case bundle::ExportOutcome::SecretFound:
		spec.dialog_id = QStringLiteral("export-failed-secret");
		break;
	default:
		spec.dialog_id = QStringLiteral("export-failed-zip");
		break;
	}
	ui::MessageButton ok;
	ok.text = obs_frontend_text(ledger::text::obs_keys::kOk);
	ok.role = ui::MessageButtonRole::Accept;
	ok.is_default = true;
	ok.is_escape = true;
	spec.buttons.push_back(ok);
	return spec;
}

ui::MessageSpec settings_save_failed_spec()
{
	ui::MessageSpec spec;
	spec.title = tr_key(keys::kDialogSettingsSaveTitle);
	spec.icon = ui::MessageIcon::Warning;
	spec.main = tr_key(keys::kDialogSettingsSaveFailedHeadline);
	spec.supplement = tr_key(keys::kDialogSettingsSaveFailedText);
	spec.dialog_id = QStringLiteral("settings-save-failed");
	ui::MessageButton ok;
	ok.text = obs_frontend_text(ledger::text::obs_keys::kOk);
	ok.role = ui::MessageButtonRole::Accept;
	ok.is_default = true;
	ok.is_escape = true;
	spec.buttons.push_back(ok);
	return spec;
}

} // namespace cordmux::dock
