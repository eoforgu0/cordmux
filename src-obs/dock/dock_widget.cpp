// SPDX-License-Identifier: Apache-2.0

// ドックのウィジェット (dock_widget.h の実装)。構成は dock_widget.h の DockWidget のコメントにある。

#include "dock/dock_widget.h"

#include <obs-frontend-api.h>

#include <QAccessible>
#include <QApplication>
#include <QEvent>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStringList>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

#include "dock/accounts_bar.h"
#include "dock/audio_tab.h"
#include "dock/bottom_bar.h"
#include "dock/dock_menus.h"
#include "dock/dock_tabs.h"
#include "dock/first_line.h"
#include "dock/inline_confirm.h"
#include "dock/layout_hold.h"
#include "dock/look_tab.h"
#include "dock/notice_list.h"
#include "dock/status_tab.h"
#include "dock/rename_dialog.h"
#include "dock/ui/dock_metrics.h"
#include "dock/ui/focus_ring.h"
#include "dock/ui/text_resolve.h"
#include "ledger/rules.h"
#include "ledger/text_ids.h"

#include "dock/people_list.h"

namespace cordmux::dock {

namespace {

// Yami の `OBSDock > QWidget` が付ける左右の枠線 (1 px ずつ)。最小幅に足す。
constexpr int kDockFrameBorderPx = 2;

// 下のバーとタブの中身の間の区切り線。OBS のドックの下のツールバーと同じ線を、テーマから読んだ色
// (DockLook::bar_rule) で自分で描く。テーマがツールバーに線を引かないときは、Qt の標準の区切り線にする。
//
// QFrame::HLine を使わないのは、Yami の `* [frameShape="4"] { border: 1px solid var(--bg_base) }` で、線がドックの
// 背景の色で塗られ、Yami と Light では見えず、Classic では背景と違う 2 本の線になるためである。中身のない QToolBar を
// 置いてテーマに線を引かせると、アクセシビリティのツリーに名前のない「ツール バー」が 1 つ増え、ナレーターが空の
// ツールバーを読む。下のバーそのものを QToolBar にすると、録画のボタンをバーの幅いっぱいに伸ばす形と、狭いときに
// [...] を 2 行目に折り返す形を QToolBar のレイアウトが持たず、[...] が » の奥に隠れる。
class BarRule : public QFrame {
public:
	explicit BarRule(QWidget *parent) : QFrame(parent)
	{
		setObjectName(QStringLiteral("cordmuxBarRule"));
		set_rule(QColor(), 1);
	}

	void set_rule(const QColor &color, int thickness)
	{
		if (set_once_ && color == color_ && std::max(1, thickness) == thickness_)
			return;
		set_once_ = true;
		color_ = color;
		thickness_ = std::max(1, thickness);
		if (color_.isValid()) {
			setFrameShape(QFrame::NoFrame);
			setFixedHeight(thickness_);
		} else {
			setFrameShape(QFrame::HLine);
			setFrameShadow(QFrame::Sunken);
			setMinimumHeight(0);
			setMaximumHeight(QWIDGETSIZE_MAX);
		}
		// 枠の形はテーマのセレクター (frameShape) に使われるので、変えたら unpolish と polish をやり直す
		// (ui::set_class と同じ理由)。
		style()->unpolish(this);
		style()->polish(this);
		updateGeometry();
		update();
	}

	QSize sizeHint() const override
	{
		return color_.isValid() ? QSize(QFrame::sizeHint().width(), thickness_) : QFrame::sizeHint();
	}

protected:
	void paintEvent(QPaintEvent *event) override
	{
		if (!color_.isValid()) {
			QFrame::paintEvent(event);
			return;
		}
		QPainter p(this);
		p.fillRect(rect(), color_);
	}

private:
	QColor color_;
	int thickness_ = 1;
	bool set_once_ = false;
};

// OBS のドックの下のツールバー (`OBSDock QToolBar`) の上の枠線の色と太さを、テーマから読む。ツールバーと同じ
// ウィジェット (QToolBar) を container の子として表示せずに作って polish し、画像に描画して、いちばん上の行を読む。
// テーマの枠線の色はパレットに入らないので、描画したピクセルで読む。ツールバーの背景は透明 (Yami の
// `QToolBar { background-color: transparent }`) なので、いちばん上の行が不透明で、中ほどの行と違えば、
// それが枠線である。読めなければ無効な色を返す。テーマのファイルの `--border_color` を読まないのは、テーマのファイルの
// 場所と変数の名前に依存するためである。
QColor read_bar_rule(QWidget *container, int *thickness)
{
	*thickness = 1;
	QToolBar probe(container);
	probe.setAttribute(Qt::WA_DontShowOnScreen, true);
	probe.setMovable(false);
	probe.hide();
	probe.ensurePolished();
	probe.resize(32, 16);
	QImage image(probe.size(), QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	probe.render(&image, QPoint(), QRegion(), QWidget::RenderFlags());
	const int x = image.width() / 2;
	const QColor top = image.pixelColor(x, 0);
	const QColor inside = image.pixelColor(x, image.height() / 2);
	if (top.alpha() < 255 || top == inside)
		return QColor();
	int rows = 1;
	while (rows < 4 && rows < image.height() && image.pixelColor(x, rows) == top)
		++rows;
	*thickness = rows;
	return top;
}

// テーマのボタンの背景 (QPushButton の background-color は、polish した後のパレットの Button に入る)。
QColor read_button_ground(QWidget *container)
{
	QPushButton probe(container);
	probe.setAttribute(Qt::WA_DontShowOnScreen, true);
	probe.hide();
	probe.ensurePolished();
	return probe.palette().color(QPalette::Active, QPalette::Button);
}

// 「名前を変更」のダイアログに表示する、Discord での名前。ダイアログの文は Discord から来た名前を含むので、
// 書式なしで表示する (make_wrap_label と同じ)。
QString discord_name_of(const ledger::dockview::PersonRow &row)
{
	// cordmux での名前がなければ、表示されている名前が Discord での名前である。
	if (row.menu.name_override.empty())
		return ui::resolve_label(row.name);
	// cordmux での名前があるときは、2 行目の「Discord での名前: %1」の %1 の値が、Discord での名前である。
	for (const ledger::TextRef &part : row.line2_parts) {
		if ((part.id == ledger::text::keys::kDockRowDiscordNameStatus ||
		     part.id == ledger::text::keys::kDockRowDiscordNameUsernameStatus) &&
		    !part.args.empty())
			return QString::fromStdString(part.args.front());
	}
	return QString();
}

// フォーカスに合わせたスクロールの判断とスクロールは、dock/ui/focus_ring.h の関数 (ui::focus_mark_rect、
// ui::focus_mark_shown、ui::reveal_focus_mark、ui::scroll_follows_focus) を使う。設定ウィンドウと、確認と結果の
// ダイアログのスクロールも同じ関数を使い、ドックとウィンドウで同じ判断にする。ドックに関わる理由 (関数のコメントに
// ないもの):
// - スクロールする理由の表 (ui::scroll_follows_focus) で、ActiveWindowFocusReason を除く理由。ドックのウィンドウが
//   アクティブでないときに行を押すと、Windows はそのクリックでウィンドウをアクティブにし、Qt はウィンドウの中で最後に
//   フォーカスのあったウィジェットにフォーカスを戻す (ActiveWindowFocusReason)。そのウィジェットが見えない所にあると、
//   クリックが届く前に一覧がスクロールされ、クリックは、押した瞬間にポインターの下にいた人ではなく、スクロールして来た
//   人に適用される。押したウィジェットにフォーカスが移る (MouseFocusReason) ときも、押してから離すまでの間に一覧が動き、
//   離す点が押したウィジェットから外れることがある。ここで守りたいのは、キーボードで移したフォーカスが
//   見えることである。ActiveWindowFocusReason でもスクロールし、クリックを無視する処理だけで防ぐと、利用者のクリックが
//   毎回 1 回無視され、押し直しが必要になる。Alt+Tab で戻って Space を押す人のために ActiveWindowFocusReason でも
//   スクロールすると、クリックで戻った人の誤操作の損のほうが大きい (Tab を 1 回押せばスクロールする)。
// - 判断 (ui::reveal_focus_mark) に QScrollArea::ensureWidgetVisible を使わない理由。ensureWidgetVisible は
//   ウィジェットの矩形が見えていればスクロールしないので、Tab で移った行のチェックや [...] が、一覧の見える
//   範囲の上か下の端から、フォーカスリングの太さとすき間以内にあると、リングの 1 辺が 1 から 3 px 欠けたままになる。
//   ensureWidgetVisible の後にリングの分だけ足してスクロールすると、2 回動き、判断がウィンドウと 2 通りになる。
//   ドックだけ縦だけで判断すると、ウィンドウと判断が分かれる (ドックは横にスクロールせず、最小幅 (update_min_size) で
//   中身が横にはみ出さないので、横の判断が動作することはない)。

// スクロール領域の中身のウィジェットがフォーカスを得たイベント (FocusIn。理由を持つ) を見る、イベントフィルター。
// QApplication::focusChanged は理由を持たないので、アプリ全体のイベントフィルターで FocusIn を見る (LayoutHold が
// クリックを見るのと同じ)。このオブジェクトが破棄されるときに外す。
class FocusFollow : public QObject {
public:
	FocusFollow(QObject *parent, std::function<void(QWidget *, Qt::FocusReason)> on_focus_in)
		: QObject(parent), on_focus_in_(std::move(on_focus_in))
	{
		if (qApp != nullptr)
			qApp->installEventFilter(this);
	}
	~FocusFollow() override
	{
		if (qApp != nullptr)
			qApp->removeEventFilter(this);
	}
	FocusFollow(const FocusFollow &) = delete;
	FocusFollow &operator=(const FocusFollow &) = delete;

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (event->type() == QEvent::FocusIn && watched->isWidgetType() && on_focus_in_)
			on_focus_in_(static_cast<QWidget *>(watched), static_cast<QFocusEvent *>(event)->reason());
		return QObject::eventFilter(watched, event);
	}

private:
	std::function<void(QWidget *, Qt::FocusReason)> on_focus_in_;
};

// タブの番号。DockTabBar と QStackedWidget の並びで、設定 dock_tab の値と対応する。
int tab_index(config::DockTab tab)
{
	switch (tab) {
	case config::DockTab::People:
		return 0;
	case config::DockTab::Audio:
		return 1;
	case config::DockTab::Display:
		return 2;
	case config::DockTab::Status:
		return 3;
	}
	return 0;
}

config::DockTab tab_of_index(int index)
{
	switch (index) {
	case 1:
		return config::DockTab::Audio;
	case 2:
		return config::DockTab::Display;
	case 3:
		return config::DockTab::Status;
	default:
		break;
	}
	return config::DockTab::People;
}

} // namespace

DockWidget::DockWidget(QWidget *parent) : QWidget(parent)
{
	root_ = new QVBoxLayout(this);
	root_->setContentsMargins(0, 0, 0, 0);
	root_->setSpacing(0);

	// 1. 1 行目。
	first_line_ = new FirstLineBar(this);
	root_->addWidget(first_line_);

	// 2. メインとサブの行。
	accounts_ = new AccountsBar(this);
	root_->addWidget(accounts_);

	// 3. タブのバーとタブの中身。
	tab_bar_ = new DockTabBar(this);
	tab_bar_->addTab(ui::resolve_key(ledger::text::keys::kDockTabPeopleLabel));
	tab_bar_->addTab(ui::resolve_key(ledger::text::keys::kDockTabAudioLabel));
	tab_bar_->addTab(ui::resolve_key(ledger::text::keys::kDockTabDisplayLabel));
	tab_bar_->addTab(ui::resolve_key(ledger::text::keys::kDockTabStatusLabel));
	{
		// 件数は 2 桁まで見込む。レベル 1 と 2 のメッセージが同時に 100 件出ることはない。
		ledger::TextRef widest;
		widest.id = ledger::text::keys::kDockTabStatusCountLabel;
		widest.args.push_back("99");
		tab_bar_->set_widest_text(ui::resolve(widest));
	}
	root_->addWidget(tab_bar_);
	pages_ = new QStackedWidget(this);
	pages_->setObjectName(QStringLiteral("cordmuxTabPages"));
	root_->addWidget(pages_, 1);

	people_page_ = new DockPage(QStringLiteral("cordmuxScroll"), pages_);
	people_header_ = new PeopleHeader(people_page_);
	people_page_->set_header(people_header_);
	people_content_ = new QWidget();
	people_content_layout_ = new QVBoxLayout(people_content_);
	people_ = new PeopleList(people_content_);
	people_content_layout_->addWidget(people_);
	people_content_layout_->addStretch(1);
	people_page_->set_content(people_content_);
	pages_->addWidget(people_page_);

	audio_page_ = new DockPage(QStringLiteral("cordmuxAudioScroll"), pages_);
	audio_content_ = new QWidget();
	audio_content_layout_ = new QVBoxLayout(audio_content_);
	audio_ = new AudioTab(audio_content_);
	audio_content_layout_->addWidget(audio_);
	audio_content_layout_->addStretch(1);
	audio_page_->set_content(audio_content_);
	// Tab の順を表示の順にする。スクロール領域の次に、一覧の先頭の人のつまみに移る。
	QWidget::setTabOrder(audio_page_->scroll(), audio_->list_widget());
	pages_->addWidget(audio_page_);
	// 表示タブ。値を変えたらすぐ配信の画面に渡し、保存は DockPoll が変更の少し後にまとめる。
	display_page_ = new DockPage(QStringLiteral("cordmuxDisplayScroll"), pages_);
	look_content_ = new QWidget();
	look_content_layout_ = new QVBoxLayout(look_content_);
	look_tab_ = new LookTab(look_content_);
	look_content_layout_->addWidget(look_tab_);
	look_content_layout_->addStretch(1);
	display_page_->set_content(look_content_);
	pages_->addWidget(display_page_);

	status_page_ = new DockPage(QStringLiteral("cordmuxStatusScroll"), pages_);
	status_content_ = new QWidget();
	status_content_layout_ = new QVBoxLayout(status_content_);
	status_ = new StatusTab(status_content_);
	status_content_layout_->addWidget(status_);
	status_content_layout_->addStretch(1);
	status_page_->set_content(status_content_);
	pages_->addWidget(status_page_);

	// 4. 下のバーと、タブの中身との間の区切り線 (BarRule)。
	separator_ = new BarRule(this);
	root_->addWidget(separator_);
	bottom_ = new BottomBar(this);
	root_->addWidget(bottom_);

	confirm_ = new InlineConfirm(this);

	hold_ = new LayoutHold(this, people_page_->scroll()->viewport());
	hold_->set_on_changed([this](bool held) { on_hold_changed(held); });
	ring_ = new ui::FocusRing(this);

	first_line_->set_on_button([this](const ledger::dockview::ViewButton &b) { press(b); });
	QObject::connect(first_line_, &QAbstractButton::clicked, this, [this] { set_tab(config::DockTab::Status); });
	accounts_->set_on_press([this](const ledger::dockview::ViewButton &b, QWidget *anchor) {
		if (b.id == ledger::ActionId::SetMainAccount)
			open_main_account_menu(anchor);
		else
			press(b);
	});
	QObject::connect(tab_bar_, &QTabBar::currentChanged, this, [this](int index) { on_tab_index_changed(index); });
	status_->set_on_press([this](const ledger::dockview::ViewButton &b) { press(b); });
	bottom_->set_on_record([this](const ledger::dockview::ViewButton &b) { press(b); });
	bottom_->set_on_menu([this]() { open_dock_menu(); });
	people_->set_action_callback([this](ledger::ActionId id, const ledger::ActionArgs &args) {
		if (action_cb_)
			action_cb_(id, args);
	});
	people_->set_rename_callback([this](const std::string &user_id) { rename(user_id); });
	people_->set_menu_open_callback([this](bool open) { hold_->set_menu_open(open); });
	audio_->set_action_callback([this](ledger::ActionId id, const ledger::ActionArgs &args) {
		if (action_cb_)
			action_cb_(id, args);
	});
	look_tab_->set_action_callback([this](ledger::ActionId id, const ledger::ActionArgs &args) { press_action(id, args); });
	people_->setup_notices()->set_on_press([this](const ledger::dockview::ViewButton &b) { press(b); });

	// フォーカスがタブの中身のウィジェットに移ったら、ウィジェットとフォーカスリングが見える所までスクロールする
	// (ui::reveal_focus_mark)。利用者の操作への応答なので、並びの保留の対象にしない。スクロールするのは、キーボードとプログラムが
	// 移したフォーカスだけである (ui::scroll_follows_focus と上のコメント)。スクロール領域の中で Tab で移るときは、この後に Qt の
	// QScrollArea::focusNextPrevChild が ensureWidgetVisible を呼ぶが、ウィジェットの全体が見えているので、何もせずに戻る。
	focus_follow_ = new FocusFollow(this, [this](QWidget *now, Qt::FocusReason reason) {
		QScrollArea *scroll = current_scroll();
		if (now == nullptr || scroll == nullptr || scroll->widget() == nullptr ||
		    !scroll->widget()->isAncestorOf(now) || !ui::scroll_follows_focus(reason))
			return;
		// フォーカスを置くための、Tab で止まらないコンテナー (一覧そのものとメッセージの並び。破棄されるウィジェットから
		// フォーカスを移す先) にはスクロールしない。見せるものではなく、コンテナーが見える範囲より高いと Qt は真ん中に
		// スクロールし、並びの保留の間に一覧が跳ぶ。次の Tab で行のウィジェットに移ったときにスクロールする。
		if ((now->focusPolicy() & Qt::TabFocus) == 0)
			return;
		ui::reveal_focus_mark(scroll, now, look_.metrics.focus_ring_px + look_.metrics.focus_ring_gap_px);
	});

	read_look();
	apply_look();
}

DockWidget::~DockWidget()
{
	// 子のウィジェットを破棄する間にフォーカスが動いても、破棄されかけのスクロール領域にスクロールしないように、
	// イベントフィルターを先に外す。
	delete focus_follow_;
	focus_follow_ = nullptr;
}

void DockWidget::set_action_callback(ActionCallback callback)
{
	action_cb_ = std::move(callback);
}

void DockWidget::set_rename_callback(std::function<void(const std::string &user_id)> callback)
{
	rename_cb_ = std::move(callback);
}

void DockWidget::set_open_folder_callback(
	std::function<void(const std::string &path_utf8, ledger::dockview::LocationKind kind)> callback)
{
	people_->setup_notices()->set_folder_opener(callback);
	status_->set_folder_opener(std::move(callback));
}

bool DockWidget::layout_held() const
{
	return hold_ != nullptr && hold_->held();
}

void DockWidget::set_layout_hold_idle_ms(int ms)
{
	hold_->set_idle_ms(ms);
}

void DockWidget::set_look_values(const config::Settings &settings)
{
	look_tab_->set_values(settings);
	status_->set_buffer_value(settings.display_buffer_ms);
}

void DockWidget::set_look_changed_callback(std::function<void(const config::Settings &)> callback)
{
	if (!callback) {
		look_tab_->set_on_changed(nullptr);
		return;
	}
	// 映像のバッファは状態タブの入力欄が持つ。表示タブが持つ値 (最後に設定から読んだ値) で、状態タブで変えた値を戻さない。
	look_tab_->set_on_changed([this, callback = std::move(callback)](const config::Settings &tab) {
		config::Settings merged = tab;
		merged.display_buffer_ms = status_->buffer_ms();
		callback(merged);
	});
}

void DockWidget::set_buffer_changed_callback(std::function<void(int)> callback)
{
	status_->set_on_buffer_changed(std::move(callback));
}

void DockWidget::set_look_source_providers(std::function<std::vector<sources::SelfSourceCandidate>()> frame,
					   std::function<std::vector<sources::SelfSourceCandidate>()> speaking)
{
	look_tab_->set_source_providers(std::move(frame), std::move(speaking));
}

void DockWidget::set_canvas_size(int width, int height)
{
	look_tab_->set_canvas_size(width, height);
}

void DockWidget::flush_look()
{
	look_tab_->flush();
	status_->flush();
}

void DockWidget::set_confirm_inputs(bool streaming, bool confirm_while_streaming)
{
	streaming_ = streaming;
	confirm_while_streaming_ = confirm_while_streaming;
}

void DockWidget::read_look()
{
	ensurePolished();
	auto *main_window = static_cast<QWidget *>(obs_frontend_get_main_window());
	look_.theme = ui::read_theme(this, main_window);
	look_.metrics = ui::read_dock_metrics(this, look_.theme, look_.style);
	look_.base_pt = ui::base_point_size(this);
	look_.l1 = ui::l1_surface(look_.theme, look_.theme.base_bg, look_.style);
	look_.dpr = devicePixelRatioF();
	// カードの背景に frame-notice の背景色を使うか (DockLook::card_frame_ok)。ボタンの背景とカードの背景のコントラスト比と、
	// ボタンの背景とドックの背景のコントラスト比 (OBS 自身のボタンが、ドックの上でどのくらい見えるか) を比べる。
	// カードの上でそれより下がるなら、カードがボタンを見えにくくしているので使わない。OBS 31 の Yami の 7 つの色の
	// 組み合わせでは、Classic だけが当てはまる。比の下限を数で決めない (WCAG の 3:1 など) のは、OBS のボタンは
	// ドックの上で、多くのテーマが 1.1 から 1.4 で、ほとんどのテーマで下限を下回るためである。
	const QColor button = read_button_ground(this);
	look_.card_frame_ok = true;
	if (look_.theme.has_notice && button.isValid() && look_.theme.notice_bg.isValid() && look_.theme.base_bg.isValid())
		look_.card_frame_ok = ui::contrast_ratio(button, look_.theme.notice_bg) >=
				      ui::contrast_ratio(button, look_.theme.base_bg);
	int rule_px = 1;
	look_.bar_rule = read_bar_rule(this, &rule_px);
	static_cast<BarRule *>(separator_)->set_rule(look_.bar_rule, rule_px);
	look_ready_ = true;
}

void DockWidget::apply_look()
{
	// 外側の左右の余白。cordmux のテキストの左端を、OBS の「シーン」ドックにある一覧のテキストの左端にそろえる
	// (テーマと [密度] で変わる。読めないテーマでは大)。1 行目、メインとサブの行、タブの中身、下のバーが同じ値を使う。
	// 大 (0.5 em) のままにすると、4 つの [密度] のうち [通常] で 4 px、[快適] で 5 px ずれ、許容する 0.25 em を超える。
	const int edge = look_.metrics.edge_text_px;
	const int small = look_.metrics.small_px;
	// タブの中身の先頭のウィジェットのフォーカスリング (リングの太さ + すき間) の外側の 1 px を、背景に残す。リングは
	// スクロール領域の見える範囲で切られるので、余白がリングより狭いと、リングの上の辺が切れる。
	const int ring_top = look_.metrics.focus_ring_px + look_.metrics.focus_ring_gap_px + 1;
	for (QVBoxLayout *layout : {people_content_layout_, audio_content_layout_, look_content_layout_, status_content_layout_}) {
		layout->setContentsMargins(edge, std::max(small, ring_top), edge, small);
		layout->setSpacing(small);
	}
	for (DockPage *page : {people_page_, audio_page_, display_page_, status_page_})
		page->scroll()->set_ring_reach(look_.metrics.focus_ring_px + look_.metrics.focus_ring_gap_px);

	tab_bar_->set_side_padding(small);
	tab_bar_->set_ring_reach(look_.metrics.focus_ring_px + look_.metrics.focus_ring_gap_px);
	first_line_->set_look(look_);
	accounts_->set_look(look_);
	people_header_->set_look(look_);
	status_->set_look(look_);
	bottom_->set_look(look_);
	confirm_->set_look(look_);
	people_->set_theme(look_.theme, look_.style);
	people_->set_metrics(look_.metrics);
	people_->setup_notices()->set_look(look_);
	audio_->set_look(look_);
	look_tab_->set_look(look_);
	ring_->set_color(look_.theme.text);
	ring_->set_metrics(look_.metrics.focus_ring_px, look_.metrics.focus_ring_gap_px);

	if (has_view_)
		apply(!hold_->held());
	update_min_size();
}

void DockWidget::set_style(const ui::DockStyle &style)
{
	look_.style = style;
	read_look();
	apply_look();
}

void DockWidget::on_theme_changed()
{
	// テーマのスタイルを、ドックのすべてのウィジェットに、親から子の順に適用し直す。OBS がテーマを変えた後の
	// ドックが、そのテーマで作り直したドックと違うことがあるためである (Light から Classic に戻すと、カードの標準の
	// 枠の線が Light の色のまま残り、Classic から Light に変えると、メッセージのラベルの高さが 2 px 小さいままになる。
	// スタイルの余白が適用されていない)。親から子の順に polish し直すと前者が、polish した後に StyleChange を送ると
	// (QFrame が枠線と余白を測り直し、QLabel が大きさのキャッシュを捨てる) 後者が直り、作り直したドックと
	// 1 ピクセルも違わなくなる (プレビューツールの --face theme-switch で確認した)。原因は、Qt が全体を polish し
	// 修正する順が親子の順でないためと考えているが、確認していない (Qt のソースで、スタイルシートを変えたときの
	// polish の順を読めば決まる)。findChildren は、親を子より先に返す。
	//
	// テーマが変わったときにドックのウィジェットを作り直さないのは、フォーカス、並びの保留、直後のクリックを
	// 無視する処理の状態を失うためである。カードの枠と色だけを適用し直すと、ラベルの余白の残りが直らず、
	// どのウィジェットに残るかを数え上げることになる。
	auto refresh = [](QWidget *w) {
		w->style()->unpolish(w);
		w->style()->polish(w);
		// スタイルの余白と枠線は、StyleChange で測り直される (QFrame::changeEvent の updateFrameWidth、
		// QLabel の大きさのキャッシュ)。
		QEvent changed(QEvent::StyleChange);
		QApplication::sendEvent(w, &changed);
		w->updateGeometry();
	};
	// 適用し直すことで起きる StyleChange、FontChange、PaletteChange (changeEvent) で、もう一度読み直さない
	// (読み直しが読み直しを呼び続ける)。
	refreshing_ = true;
	refresh(this);
	for (QWidget *w : findChildren<QWidget *>())
		refresh(w);
	refreshing_ = false;
	read_look();
	apply_look();
}

void DockWidget::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	// 初めて表示されたときに 1 回読み直す。テーマは `OBSDock > QWidget` の形で背景を指定するので、OBS のドック
	// (OBSDock) に入って表示される前に読んだ背景は違う (メッセージのレベル 1 のパネルの枠線を描くかの比は、
	// この背景で決まる)。表示された直後は、テーマのスタイルがまだウィジェットに適用し終わっていないことがあるので、
	// イベントループを 1 周待ってから読む。
	if (!shown_once_) {
		shown_once_ = true;
		QTimer::singleShot(0, this, [this]() {
			read_look();
			apply_look();
		});
	}
	report_levels_wanted();
}

void DockWidget::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);
	switch (event->type()) {
	case QEvent::StyleChange:
	case QEvent::FontChange:
	case QEvent::PaletteChange:
	case QEvent::ParentChange:
		// テーマの変更。同じイベントの処理の中で何度も来るので、1 回にまとめて読み直す。OBS のドック
		// (OBSDock) に入った時点 (ParentChange) でも読み直す。テーマは `OBSDock > QWidget` の形で背景を
		// 指定するので、入る前に読んだ背景は違う。
		if (!rereading_ && !refreshing_ && look_ready_) {
			rereading_ = true;
			QTimer::singleShot(0, this, [this]() {
				rereading_ = false;
				on_theme_changed();
			});
		}
		break;
	default:
		break;
	}
}

void DockWidget::hideEvent(QHideEvent *event)
{
	QWidget::hideEvent(event);
	report_levels_wanted();
}

QScrollArea *DockWidget::current_scroll() const
{
	auto *page = dynamic_cast<DockPage *>(pages_->currentWidget());
	return page != nullptr ? page->scroll() : nullptr;
}

config::DockTab DockWidget::tab() const
{
	return tab_of_index(tab_bar_->currentIndex());
}

void DockWidget::set_tab(config::DockTab tab)
{
	tab_bar_->setCurrentIndex(tab_index(tab));
}

bool DockWidget::status_tab_open() const
{
	return isVisible() && tab() == config::DockTab::Status;
}

void DockWidget::on_tab_index_changed(int index)
{
	// 表示タブを離れるときは、渡していない値を渡す。開くときは、OBS のソースの候補を集め直す。
	if (pages_->currentIndex() == tab_index(config::DockTab::Display) && index != pages_->currentIndex())
		look_tab_->flush();
	if (pages_->currentIndex() == tab_index(config::DockTab::Status) && index != pages_->currentIndex())
		status_->flush();
	pages_->setCurrentIndex(index);
	if (index == tab_index(config::DockTab::Display))
		look_tab_->refresh_sources();
	if (QScrollArea *scroll = current_scroll())
		hold_->set_viewport(scroll->viewport());
	report_levels_wanted();
	if (on_tab_changed_)
		on_tab_changed_(tab_of_index(index));
}

void DockWidget::set_status_tab_label(const ledger::TextRef &label)
{
	if (label.id.empty())
		return;
	const QString text = ui::resolve(label);
	const int index = tab_index(config::DockTab::Status);
	if (tab_bar_->tabText(index) != text)
		tab_bar_->setTabText(index, text);
}

bool DockWidget::levels_wanted() const
{
	return levels_wanted_;
}

void DockWidget::set_on_levels_wanted(std::function<void(bool)> callback)
{
	on_levels_wanted_ = std::move(callback);
	report_levels_wanted();
}

void DockWidget::report_levels_wanted()
{
	const bool wanted = isVisible() && (tab() == config::DockTab::People || tab() == config::DockTab::Audio);
	// 音声タブを離れたら、メーターを 0 に戻す。次に開いたときに古い棒が一瞬残らない。
	if (tab() != config::DockTab::Audio)
		audio_->clear_levels();
	if (wanted == levels_wanted_)
		return;
	levels_wanted_ = wanted;
	if (on_levels_wanted_)
		on_levels_wanted_(wanted);
	// 周期を止めたら、光とメーターを消しておく。次に見えたときに古い光と棒が一瞬残らない。
	if (!wanted) {
		people_->set_speaking({});
		audio_->clear_levels();
	}
}

void DockWidget::set_speaking(const std::vector<std::string> &user_ids)
{
	people_->set_speaking(user_ids);
}

void DockWidget::set_audio_levels(const std::vector<pipeline::AudioTrackLevel> &tracks)
{
	if (tab() != config::DockTab::Audio)
		return;
	audio_->set_levels(tracks);
}

void DockWidget::set_avatars(const std::map<std::string, std::shared_ptr<const std::vector<std::uint8_t>>> &avatars)
{
	people_->set_avatars(avatars);
}

void DockWidget::render(const ledger::dockview::DockViewV2 &view)
{
	if (has_view_ && view == view_)
		return; // 値が変わっていないウィジェットには何も書かない
	view_ = view;
	has_view_ = true;
	apply(!hold_->held());
}

void DockWidget::apply(bool allow_moves)
{
	if (!has_view_)
		return;
	bool pending = false;
	// 1 行目は、いつでもすぐに書き換える。高さは一定なので、ほかのウィジェットは動かない。
	first_line_->set_line(view_.first_line, view_.first_line.tier == std::optional<ledger::dockview::Tier>(ledger::dockview::Tier::L1));
	accounts_->set_accounts(view_.accounts);

	if (status_->update(view_.notices, view_.status_tab, allow_moves))
		pending = true;
	people_->update(view_.people_list, view_.list_reason, view_.empty);
	audio_->update(view_.audio_tab);
	look_tab_->set_view(view_.look_tab);
	if (people_->setup_notices()->update(ledger::setup_notices_of(view_), allow_moves))
		pending = true;
	people_header_->set_slots(ui::resolve(view_.people_list.slots_label));
	bottom_->set_record(view_.record);
	if (confirm_->active() && !confirm_still_valid())
		confirm_->cancel();

	if (pending)
		pending_moves_ = true;
	if (allow_moves)
		pending_moves_ = false;
	// 下のバーの幅は録画のボタンのテキストで決まり、最初の View が届くまで分からない。ほかは状態によらないので、毎回は測らない。
	if (bottom_->minimum_bar_width() != last_bottom_min_)
		update_min_size();
}

void DockWidget::update_min_size()
{
	if (!look_ready_)
		return;
	const int edge = look_.metrics.edge_text_px;
	const int small = look_.metrics.small_px;
	last_bottom_min_ = bottom_->minimum_bar_width();
	const int bar = people_page_->scroll()->verticalScrollBar()->sizeHint().width();
	// 参加者の一覧の 1 行と音声タブの 1 人 (行のウィジェットが申告する最小の幅、タブの中の左右の余白、スクロールバー)。
	int row = 0;
	row = std::max(people_->minimum_row_width_px(), audio_->minimum_row_width_px());
	row += 2 * edge + bar;
	// 確認のボタンは [退出] と [通話を続ける] がいちばん長くなりうる場所である。録画のボタンの場所は十分に広い。
	const int confirm = confirm_->pair_width_px(ui::resolve_key(ledger::text::keys::kDockAccountLeaveButton),
						     ui::resolve_key(ledger::text::keys::kDockConfirmStayInCallButton));
	const int inside = std::max({last_bottom_min_, row, accounts_->minimum_bar_width_px(confirm),
				     first_line_->minimum_line_width_px(), tab_bar_->minimum_bar_width_px(),
				     people_header_->minimumSizeHint().width()});
	min_width_ = inside + kDockFrameBorderPx;
	setMinimumWidth(min_width_);

	// 最小の高さは、タブの中身の 1 行分までを見せる高さにする。
	int min_h = first_line_->height() + accounts_->minimumSizeHint().height() + tab_bar_->sizeHint().height();
	min_h += people_header_->minimumSizeHint().height() + look_.metrics.row_min_height_px + 2 * small;
	min_h += separator_->sizeHint().height() + bottom_->minimumSizeHint().height();
	setMinimumHeight(std::max(1, min_h));
}

void DockWidget::on_hold_changed(bool held)
{
	people_->set_layout_hold(held);
	audio_->set_layout_hold(held);
	if (!held && pending_moves_) {
		// 並びの保留が終わったら、ためておいた変化をまとめて反映する。
		apply(true);
	}
}

void DockWidget::open_dock_menu()
{
	if (!has_view_)
		return;
	QPushButton *anchor = bottom_->menu_button();
	// 押した項目は、メニューが閉じて、フォーカスが開く前のウィジェットに戻ってから実行する (行のメニューの
	// PeopleList::run_menu と同じ)。設定ウィンドウなどをメニューの中のイベントループで開くと、ウィンドウが、
	// 開いたウィジェットとして覚えるフォーカスが、メニューを閉じる途中のものになることがある。ウィンドウを
	// 閉じたときに戻す先は、開く前にフォーカスのあった、ドックの [...] である。
	std::function<void()> pending;
	QPointer<QMenu> menu;
	menu = make_dock_menu(this, view_.menu, [&pending, &menu, this](ledger::ActionId id, const ledger::ActionArgs &args) {
		pending = [this, id, args] { press_action(id, args); };
		// UI Automation (スクリーンリーダーや音声での操作) で項目を実行すると、Qt はメニューを閉じない
		// (QAccessibleMenuItem の press は QAction::trigger だけを呼ぶ)。閉じないと exec から戻らず、押した操作を
		// 実行しないので、ここで閉じる。マウスとキーボードで押したときは、Qt がもう閉じている。
		if (!menu.isNull())
			menu->hide();
	});
	// メニューが開いている間は並びの保留にする。閉じたら、フォーカスは開く前のウィジェットに戻る (Qt の QMenu が戻す)。
	hold_->set_menu_open(true);
	menu->exec(anchor->mapToGlobal(QPoint(0, anchor->height())));
	hold_->set_menu_open(false);
	if (!menu.isNull())
		menu->deleteLater();
	if (pending)
		pending();
}

void DockWidget::open_main_account_menu(QWidget *anchor)
{
	if (!has_view_)
		return;
	std::function<void()> pending;
	QPointer<QMenu> menu;
	menu = make_main_account_menu(this, view_.accounts.main,
				      [&pending, &menu, this](ledger::ActionId id, const ledger::ActionArgs &args) {
					      pending = [this, id, args] { press_action(id, args); };
					      if (!menu.isNull())
						      menu->hide();
				      });
	hold_->set_menu_open(true);
	menu->exec(anchor->mapToGlobal(QPoint(0, anchor->height())));
	hold_->set_menu_open(false);
	if (!menu.isNull())
		menu->deleteLater();
	if (pending)
		pending();
}

void DockWidget::press(const ledger::dockview::ViewButton &button)
{
	if (button.disabled_reason.has_value())
		return;
	press_action(button.id, button.args);
}

std::optional<ledger::ConfirmPlan> DockWidget::confirm_plan_for(ledger::ActionId id) const
{
	ledger::ConfirmFacts facts;
	facts.streaming = streaming_;
	facts.confirm_while_streaming = confirm_while_streaming_;
	return ledger::confirm_needed(id, facts);
}

void DockWidget::press_action(ledger::ActionId id, const ledger::ActionArgs &args)
{
	if (const std::optional<ledger::ConfirmPlan> plan = confirm_plan_for(id)) {
		start_confirm(*plan, id, args);
		return;
	}
	if (action_cb_)
		action_cb_(id, args);
}

void DockWidget::start_confirm(const ledger::ConfirmPlan &plan, ledger::ActionId id, const ledger::ActionArgs &args)
{
	QRect place;
	if (plan.place == ledger::ConfirmPlace::LeaveButtons) {
		const QRect in_bar = accounts_->leave_place();
		place = QRect(accounts_->mapTo(this, in_bar.topLeft()), in_bar.size());
	}
	confirm_plan_ = plan;
	confirm_action_ = id;
	const auto hide_target = [this](bool hidden) { accounts_->set_leave_hidden(hidden); };
	// 元のボタンを隠す前に始める。隠すと Qt がフォーカスを移すので、戻す先を先に覚えさせる。
	confirm_->start(
		place, ui::resolve_key(plan.confirm_key), ui::resolve_key(plan.cancel_key), ui::FocusRing::keyboard_mode(),
		[this, id, args] {
			if (action_cb_)
				action_cb_(id, args);
		},
		[this, hide_target] {
			confirm_plan_.reset();
			hide_target(false);
		});
	hide_target(true);
}

bool DockWidget::confirm_still_valid() const
{
	if (!confirm_plan_.has_value())
		return false;
	switch (confirm_action_) {
	case ledger::ActionId::LeaveCall:
		return view_.accounts.sub.has_value() && !view_.accounts.sub->leave.disabled_reason.has_value();
	default:
		break;
	}
	return false;
}

void DockWidget::rename(const std::string &user_id)
{
	if (rename_cb_) {
		rename_cb_(user_id);
		return;
	}
	// 押した時点のその人の行 (人の ID で探す)。通話から抜けていれば何もしない。
	const ledger::dockview::PersonRow *row = nullptr;
	const auto find_in = [&row, &user_id](const std::vector<ledger::dockview::PersonRow> &rows) {
		for (const ledger::dockview::PersonRow &p : rows) {
			if (p.user_id == user_id)
				row = &p;
		}
	};
	find_in(view_.people_list.sharing);
	find_in(view_.people_list.in_call);
	if (view_.people_list.pinned.has_value() && view_.people_list.pinned->user_id == user_id)
		row = &*view_.people_list.pinned;
	if (row == nullptr)
		return;
	const ledger::dockview::ViewButton action = row->menu.rename;
	const QString discord_name = discord_name_of(*row);
	const std::string current = row->menu.name_override;
	hold_->set_menu_open(true); // ダイアログが開いている間も一覧を動かさない
	// ダイアログはドックの操作だけを止める (OwnerOnly。rename_dialog.h) ので、開いている間も
	// OBS のメインウィンドウを操作でき、OBS の終了でこのドックが破棄されることがある。戻った後は、
	// 自分が残っているかを確認してから触る (message_dialog.h の run_message_dialog)。
	const QPointer<DockWidget> self(this);
	const std::optional<std::string> name = run_rename_dialog(this, discord_name, current);
	if (self.isNull())
		return;
	hold_->set_menu_open(false);
	if (!name.has_value() || action.disabled_reason.has_value())
		return;
	ledger::ActionArgs args = action.args;
	args.user_id = user_id;
	args.name = *name;
	if (action_cb_)
		action_cb_(action.id, args);
}

void DockWidget::set_busy_actions(const std::set<ledger::ActionId> &busy)
{
	status_->set_busy(busy);
	people_->setup_notices()->set_busy(busy);
}

void DockWidget::announce(const QString &text, bool assertive)
{
	if (text.isEmpty())
		return;
	// スクリーンリーダーの読み上げに渡す。フォーカスは動かさない。QAccessible の Alert のイベントは出さない。
	// Alert のイベントには Windows が音を鳴らす経路があり、デスクトップの音として配信に入るためである。
	QAccessibleAnnouncementEvent event(this, text);
	event.setPoliteness(assertive ? QAccessible::AnnouncementPoliteness::Assertive
				      : QAccessible::AnnouncementPoliteness::Polite);
	QAccessible::updateAccessibility(&event);
}

std::vector<Announcement> announcements_for(const ledger::NoticeDiff &diff,
					    const ledger::dockview::FirstLine &first_line, bool list_was_shown)
{
	std::vector<Announcement> out;
	QStringList appeared;
	for (const ledger::NoticeEvent &e : diff.appeared) {
		if (e.is_row)
			continue;
		const QString text = ui::resolve(e.headline);
		appeared << text;
		out.push_back(Announcement{text, e.tier == ledger::dockview::Tier::L1});
	}
	const bool l1_cleared = std::any_of(diff.disappeared.begin(), diff.disappeared.end(), [](const ledger::NoticeEvent &e) {
		return !e.is_row && e.tier == ledger::dockview::Tier::L1;
	});
	if (l1_cleared && !first_line.forms.empty()) {
		// 1 行目の全文は、いちばん長い形である (FirstLineBar の読み上げの名前と同じテキスト。残りの件数の
		// ラベルは足さない)。
		const QString now = ui::resolve(first_line.forms.front());
		if (!now.isEmpty() && !appeared.contains(now))
			out.push_back(Announcement{now, false});
	}
	if (diff.list_full_changed_to.has_value() && list_was_shown) {
		out.push_back(Announcement{*diff.list_full_changed_to
						   ? ui::resolve_key(ledger::text::keys::kDockNoFreeSlotText)
						   : ui::resolve_key(ledger::text::keys::kDockSlotFreeA11y),
					   false});
	}
	return out;
}

void DockWidget::set_scroll_position(int position)
{
	QScrollArea *scroll = current_scroll();
	if (scroll == nullptr)
		return;
	QScrollBar *bar = scroll->verticalScrollBar();
	const int max = bar->maximum();
	bar->setValue(position <= 0 ? 0 : position == 1 ? max / 2 : max);
}

void DockWidget::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	// 確認のボタンは元のボタンの場所に重ねて置いているので、大きさが変わると場所がずれる。取り消して元に戻す。
	confirm_->cancel();
	refresh_layout();
}

void DockWidget::refresh_layout()
{
	if (!look_ready_)
		return;
	// 一覧の行が使える幅 (スクロールバーを除く) で、表示するものを決める。
	const int row_w = people_page_->scroll()->viewport()->width() - 2 * look_.metrics.edge_text_px;
	if (look_.metrics.em_px > 0)
		people_->set_available_width_em(row_w / look_.metrics.em_px);
}

} // namespace cordmux::dock
