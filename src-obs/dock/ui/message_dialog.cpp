// SPDX-License-Identifier: Apache-2.0

// message_dialog.h の実装。

#include "dock/ui/message_dialog.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDockWidget>
#include <QEventLoop>
#include <QFontInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QStyle>
#include <QTextDocument>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>

#include "dock/ui/dock_metrics.h"
#include "dock/ui/dock_ui_calc.h"
#include "dock/ui/focus_ring.h"
#include "dock/ui/open_location.h"
#include "dock/ui/theme_read.h"
#include "dock/ui/type_scale.h"
#include "dock/ui/wrap_label.h"

namespace cordmux::dock::ui {

namespace {

constexpr const char *kObjectName = "cordmuxMessageDialog";
// 本文 (主文とボタンの間) のコンテナーと、画面に入らないときにそれを入れるスクロール領域の名前 (テストと
// プレビューツールが探す)。
constexpr const char *kBodyObjectName = "cordmuxMessageBody";
constexpr const char *kScrollObjectName = "cordmuxMessageScroll";
// 箇条書きの記号 (半角のハイフン)。
constexpr const char *kBulletMark = "-";
// ダイアログの枠 (タイトルバーと上下の枠) の高さを測れないときに使う高さ (論理 px)。Qt の QDialog::adjustPosition が、
// 枠を測れないときにダイアログを画面に収める計算に使うタイトルバーの高さと同じ値である
// (https://github.com/qt/qtbase/blob/v6.8.3/src/widgets/dialogs/qdialog.cpp の adjustPosition の extrah)。
// Windows 10 の拡大率 100% では、タイトルバーと上下の枠の和は約 39 px。
constexpr int kFrameFallbackPx = 40;
// スクロールするときに見せる本文の最小の行数。スクロールバー (上下の矢印とつまみ) を操作できる高さで、見出しと
// その最初の箇条書きが一緒に見える。1 行にすると、スクロールバーのつまみが押せないほど小さくなる。
constexpr int kMinBodyLines = 3;

// 本文をスクロールする QScrollArea。ドックのスクロール領域 (dock_widget.cpp の ScrollBox) と同じく、フォーカス
// リングを、見える範囲 (viewport) の内側の 4 辺に表示する。押せる範囲を見える範囲から太さ + 空きの分だけ内側に
// 寄せ、リングの外周を見える範囲の外周に重ねる。本文のコンテナーは上・下・左に太さ + 空き、右に 0.5 em の余白を
// 持つので、いちばん上か下までスクロールしたとき、リングはテキストにかからない (途中では、スクロール領域の端で
// 切れて見えるテキストに重なる。ドックと同じ)。
// スクロールする間、ダイアログは右へ 0.5 em とスクロールバーの幅だけ広がる (45 em の上限を超えることがある)。
// 上限は 1 行の長さのためのもので、列の幅はスクロールしないときと同じに保つ。テキストを折り返し直すと高さが
// 変わり、高さの調整を繰り返すことになるため。
// リングをスクロール領域の外に表示する方法は使わない。上の辺が主文のラベルの下端に、下の辺がボタンに
// 1 px まで近づくため。
class MessageScroll : public QScrollArea, public FocusHitRect {
public:
	explicit MessageScroll(QWidget *parent) : QScrollArea(parent) {}
	void set_ring_reach(int reach) { reach_ = std::max(0, reach); }
	QRect focus_hit_rect() const override
	{
		return viewport()->geometry().adjusted(reach_, reach_, -reach_, -reach_);
	}

private:
	int reach_ = 0;
};

class MessageDialog : public QDialog {
public:
	explicit MessageDialog(QWidget *parent) : QDialog(parent) {}

	int choice = -1;
	int escape_index = -1;

	// 画面の高さに合わせるために使うレイアウトとウィジェット (make_message_dialog が設定する)。
	QVBoxLayout *root = nullptr;     // ダイアログのルート (本文の行とボタンの行)
	QHBoxLayout *body_row = nullptr; // アイコンとテキストの列
	QVBoxLayout *texts = nullptr;    // テキストの列 (主文、主文と本文の間、本文のコンテナー)
	QWidget *main_label = nullptr;   // 主文
	QWidget *content = nullptr;      // 本文のコンテナー (本文のないダイアログでは nullptr)
	QVBoxLayout *flow = nullptr;     // 本文のコンテナーのレイアウト
	QSpacerItem *main_gap = nullptr; // 主文と本文の間 (テキストの列の中)
	int reach = 0;                   // フォーカスリングの太さ + 空き
	int gap_px = 0;                  // 大きい余白 (0.5 em)
	int min_body_px = 0;             // スクロールするときに見せる本文の最小の高さ

	void finish(int index, MessageButtonRole role)
	{
		choice = index;
		done(role == MessageButtonRole::Accept ? QDialog::Accepted : QDialog::Rejected);
	}

	// Esc とタイトルバーの [x] は、Esc のボタンを押したのと同じにする。
	void reject() override
	{
		choice = escape_index;
		QDialog::reject();
	}

	bool scrolls() const { return scroll_ != nullptr; }

	// 作業領域の高さを固定する (テストとプレビューツール。set_message_dialog_work_area)。0 以下なら固定をやめ、
	// 画面から読む。
	void set_work_area(int available_px, int frame_px)
	{
		work_area_px_ = std::max(0, available_px);
		work_frame_px_ = frame_px;
		if (work_area_px_ > 0)
			fit_to_screen();
	}

	// ダイアログの高さを作業領域に合わせる (dock_ui_calc.h の dialog_height_fit)。スクロールする形になったら
	// true を返す。
	bool fit_height(int available_px, int frame_px)
	{
		if (content == nullptr || root == nullptr)
			return false;
		read_stops();
		root->activate();
		// 現在の形から、本来の高さ (スクロールしない形の高さ) と、スクロール領域に置き換わる本文の高さを
		// 求める。本文の高さには、スクロールするときに主文との間とボタンとの間から本文のコンテナーの余白へ
		// 移す分 (リングの分) を含める (移してもダイアログの高さは変わらない)。
		const Steal s = steal();
		const int dialog_h = root->totalSizeHint().height();
		int body_px = 0;
		int natural_px = 0;
		if (scroll_ != nullptr) {
			body_px = content->sizeHint().height(); // コンテナーの余白 (上と下のリングの分) を含む
			natural_px = dialog_h - scroll_->minimumHeight() + body_px;
		} else {
			body_px = content->sizeHint().height() + s.top + s.bottom;
			natural_px = dialog_h;
		}
		const calc::DialogHeightFit fit =
			calc::dialog_height_fit(natural_px, body_px, available_px, frame_px, min_body_px);
		if (fit.scroll)
			scroll_on(fit.body_px, s);
		else
			scroll_off();
		root->activate();
		return fit.scroll;
	}

	// ダイアログを表示する画面の作業領域と、ダイアログの枠の高さで合わせる。
	void fit_to_screen()
	{
		if (work_area_px_ > 0) {
			fit_height(work_area_px_, work_frame_px_);
			return;
		}
		QWindow *handle = windowHandle();
		QScreen *s = handle != nullptr ? handle->screen() : screen();
		if (s == nullptr)
			return;
		int frame = 0;
		if (handle != nullptr) {
			const QMargins fm = handle->frameMargins();
			frame = fm.top() + fm.bottom();
		}
		fit_height(s->availableGeometry().height(), frame > 0 ? frame : kFrameFallbackPx);
	}

protected:
	// 表示する直前 (QDialog::showEvent が親の中央に置き、画面の中へ寄せる前) に高さを合わせる。この時点で
	// ウィンドウの枠は作られている (QWidget::setVisible は create の後に showEvent を送る) ので、枠の高さを測れる。
	void showEvent(QShowEvent *event) override
	{
		if (!event->spontaneous()) {
			watch_screen();
			fit_to_screen();
		}
		QDialog::showEvent(event);
	}

	// スクロール領域の中のウィジェットにフォーカスが移ったら、リングまで見える位置へスクロールする (ドックと
	// 設定ウィンドウと同じ判定)。Tab でスクロール領域の外 ([OK] など) から中のウィジェットへ入ったときは、Qt の
	// QScrollArea::focusNextPrevChild (スクロール領域の中で移るときだけ呼ばれる) がスクロールしないので、
	// イベントで見る。
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (event->type() == QEvent::FocusIn && scroll_ != nullptr && watched->isWidgetType()) {
			auto *w = static_cast<QWidget *>(watched);
			if (content != nullptr && content->isAncestorOf(w) &&
			    scroll_follows_focus(static_cast<QFocusEvent *>(event)->reason()))
				reveal_focus_mark(scroll_, w, reach);
		}
		return QDialog::eventFilter(watched, event);
	}

private:
	// スクロールするときに、主文との間、アイコンとの間、ボタンとの間から、本文のコンテナーの余白へ移す量 (リングの
	// 太さ + 空きまで)。本文のテキストの位置は、スクロールしない形と同じ所に残る (スクロール領域がリングの分だけ
	// 外へ広がり、コンテナーの余白が同じ分だけ内へ寄せる)。
	struct Steal {
		int left = 0;
		int top = 0;
		int bottom = 0;
	};

	Steal steal() const
	{
		Steal s;
		const int gap_top = main_gap != nullptr ? main_gap->sizeHint().height() + (scroll_ != nullptr ? top_taken_ : 0)
							: 0;
		s.left = std::min(reach, std::max(0, body_spacing_ >= 0 ? body_spacing_ : body_row->spacing()));
		s.top = std::min(reach, std::max(0, gap_top));
		s.bottom = std::min(reach, std::max(0, root_spacing_ >= 0 ? root_spacing_ : root->spacing()));
		return s;
	}

	// タブストップの並び (スクロール領域を除く) を、最初に合わせる前に 1 回だけ読む。スクロール領域を出し入れ
	// しても、ほかのタブストップの順は変えない。
	void read_stops()
	{
		if (stops_read_)
			return;
		stops_read_ = true;
		for (QWidget *w = nextInFocusChain(); w != nullptr && w != this; w = w->nextInFocusChain()) {
			if ((w->focusPolicy() & Qt::TabFocus) != Qt::TabFocus || !isAncestorOf(w))
				continue;
			stops_.emplace_back(w);
			if (content != nullptr && content->isAncestorOf(w))
				w->installEventFilter(this);
		}
	}

	// タブストップの順を設定し直す。スクロールするときは、スクロール領域を本文の最初のタブストップの直前に
	// 入れる (本文にタブストップがなければ、一巡の最後。つまり最初のタブストップの直前)。
	void order_stops()
	{
		std::vector<QWidget *> order;
		bool placed = false;
		for (const QPointer<QWidget> &p : stops_) {
			if (p.isNull())
				continue;
			if (!placed && scroll_ != nullptr && content->isAncestorOf(p.data())) {
				order.push_back(scroll_);
				placed = true;
			}
			order.push_back(p.data());
		}
		if (scroll_ != nullptr && !placed)
			order.push_back(scroll_);
		for (std::size_t i = 1; i < order.size(); ++i)
			QWidget::setTabOrder(order[i - 1], order[i]);
	}

	void scroll_on(int body_px, const Steal &s)
	{
		if (scroll_ == nullptr) {
			const QPointer<QWidget> had_focus = focusWidget();
			body_spacing_ = body_row->spacing();
			root_spacing_ = root->spacing();
			top_taken_ = s.top;
			auto *scroll = new MessageScroll(this);
			scroll->setObjectName(QString::fromLatin1(kScrollObjectName));
			scroll->setFrameShape(QFrame::NoFrame);
			scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
			scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
			scroll->setWidgetResizable(true);
			// スクロールできる間はタブストップにする (フォーカスがあれば、QAbstractScrollArea が上下の
			// 矢印キーと PageUp・PageDown でスクロールする)。アクセシブル名はダイアログのタイトルにする
			// (ドックのスクロール領域と同じ)。ドックのスクロール領域はいつもタブストップにしているが、
			// ダイアログのスクロール領域は画面に入らないときだけ作るので、入るダイアログ (ほとんどの
			// 場合) の Tab の順を変えないほうを選ぶ。
			scroll->setFocusPolicy(Qt::StrongFocus);
			scroll->setAccessibleName(windowTitle());
			scroll->set_ring_reach(reach);
			delete texts->replaceWidget(content, scroll);
			scroll->setWidget(content);
			// 背景はダイアログの背景のままにする。QScrollArea::setWidget は中身の背景を塗る設定にし、
			// 見える範囲も Qt の既定で背景を塗るので、どちらの autoFillBackground も、表示する (polish
			// される) 前に false にする。テーマの QSS に QAbstractScrollArea の規則 (Yami の
			// border-radius) があると、polish のときに QStyleSheetStyle が見える範囲を、背景を塗る設定の
			// まま覚えて Base の色で塗るため (Qt 6.8.3 の qstylesheetstyle.cpp の polish と PE_Widget。
			// https://github.com/qt/qtbase/blob/v6.8.3/src/widgets/styles/qstylesheetstyle.cpp)。
			// 表示した後に false にすると、Yami でスクロール領域がダイアログより暗く、Yami Light で
			// 灰色に見える。ドックのスクロール領域 (dock_widget.cpp) も、表示する前に false にしている。
			content->setAutoFillBackground(false);
			scroll->viewport()->setAutoFillBackground(false);
			// 表示中のダイアログ (showEvent の中と、画面を移った後) に加えた子のウィジェットは、
			// レイアウトが次のイベントループの周回で表示するまで隠れたままで、レイアウトの計算に入らない
			// (ダイアログが主文とボタンだけの高さに縮む)。ここで表示する。
			content->show();
			scroll->show();
			// リングの分を、外側の間隔からコンテナーの余白へ移す (本文のテキストは動かない)。右は、本文と
			// スクロールバーの間 (大きい余白 0.5 em。リングの分より狭くしない) と、スクロールバーの分だけ
			// ダイアログが広がる。行の右のボタンがスクロールバーに接して見えないようにするため。右も
			// リングの分だけにすると、ボタンとスクロールバーの間が 3 px になり、スクロールバーがボタンの
			// 枠線に見える。
			set_flow_margins(QMargins(s.left, s.top, std::max(reach, gap_px), s.bottom));
			body_row->setSpacing(body_spacing_ - s.left);
			if (main_gap != nullptr)
				main_gap->changeSize(0, std::max(0, main_gap->sizeHint().height() - s.top), QSizePolicy::Minimum,
						     QSizePolicy::Fixed);
			root->setSpacing(root_spacing_ - s.bottom);
			// 主文は、アイコンとの間が狭まった分だけ右へ戻す (主文のテキストも動かない)。
			texts->removeWidget(main_label);
			main_row_ = new QHBoxLayout();
			main_row_->setContentsMargins(0, 0, 0, 0);
			main_row_->setSpacing(0);
			main_row_->addSpacing(s.left);
			main_row_->addWidget(main_label);
			// 列の余り (スクロールバーとリングの分) は、主文の右に置く (QBoxLayout は、伸びない
			// ウィジェットの間に余りを配るため)。
			main_row_->addStretch(1);
			texts->insertLayout(0, main_row_);
			scroll_ = scroll;
			order_stops();
			if (!had_focus.isNull() && had_focus->isVisibleTo(this))
				had_focus->setFocus(Qt::OtherFocusReason);
		}
		const int bar = scroll_->verticalScrollBar()->sizeHint().width();
		scroll_->setFixedSize(content->sizeHint().width() + bar, body_px);
		texts->invalidate();
	}

	void scroll_off()
	{
		if (scroll_ == nullptr)
			return;
		const bool focus_on_box = focusWidget() == scroll_;
		const QPointer<QWidget> had_focus = focusWidget();
		MessageScroll *scroll = scroll_;
		scroll_ = nullptr;
		QWidget *taken = scroll->takeWidget();
		set_flow_margins(QMargins());
		delete texts->replaceWidget(scroll, taken);
		taken->show();
		body_row->setSpacing(body_spacing_);
		root->setSpacing(root_spacing_);
		if (main_gap != nullptr)
			main_gap->changeSize(0, main_gap->sizeHint().height() + top_taken_, QSizePolicy::Minimum,
					     QSizePolicy::Fixed);
		top_taken_ = 0;
		main_row_->removeWidget(main_label);
		texts->removeItem(main_row_);
		delete main_row_;
		main_row_ = nullptr;
		texts->insertWidget(0, main_label);
		delete scroll;
		body_spacing_ = -1;
		root_spacing_ = -1;
		order_stops();
		// スクロール領域にフォーカスがあったら、本文の最初のタブストップか、なければ既定のボタンへ移す
		// (スクロール領域は削除した)。
		if (focus_on_box) {
			QWidget *next = nullptr;
			for (const QPointer<QWidget> &p : stops_) {
				if (!p.isNull() && content->isAncestorOf(p.data())) {
					next = p.data();
					break;
				}
			}
			if (next == nullptr) {
				for (QPushButton *b : findChildren<QPushButton *>()) {
					if (b->isDefault()) {
						next = b;
						break;
					}
				}
			}
			if (next != nullptr)
				next->setFocus(Qt::OtherFocusReason);
		} else if (!had_focus.isNull() && had_focus->isVisibleTo(this)) {
			had_focus->setFocus(Qt::OtherFocusReason);
		}
	}

	// コンテナーの余白を変え、親のレイアウトの QWidgetItem が保存しているコンテナーのサイズ (sizeHint) をすぐに
	// 捨てる。QLayout::setContentsMargins はレイアウトを無効にするだけで、親のレイアウトのキャッシュは次の
	// イベントループの周回まで古いまま残り、その場で測ると余白を変える前の高さが返るため (スクロールを外した直後の
	// ダイアログが 6 px 高くなる)。
	void set_flow_margins(const QMargins &margins)
	{
		flow->setContentsMargins(margins);
		flow->activate();
		content->updateGeometry();
	}

	// ダイアログが別の画面へ移ったとき (QWindow::screenChanged) と、画面の作業領域が変わったとき
	// (QScreen::availableGeometryChanged。タスクバーの大きさ、解像度、拡大率の変更) に合わせ直す。どちらも Qt の
	// シグナルで通知されるので、同じ 1 つの関数で合わせ直せる。ダイアログの位置は動かさない (利用者がダイアログを
	// 動かしている最中に動かすと、引っ張り合いになるため)。
	void watch_screen()
	{
		QWindow *handle = windowHandle();
		if (handle == nullptr || screen_hooked_)
			return;
		screen_hooked_ = true;
		connect(handle, &QWindow::screenChanged, this, [this](QScreen *) {
			hook_available();
			if (isVisible())
				fit_to_screen();
		});
		hook_available();
	}

	void hook_available()
	{
		QObject::disconnect(available_hook_);
		QWindow *handle = windowHandle();
		QScreen *s = handle != nullptr ? handle->screen() : nullptr;
		if (s == nullptr)
			return;
		available_hook_ = connect(s, &QScreen::availableGeometryChanged, this, [this](const QRect &) {
			if (isVisible())
				fit_to_screen();
		});
	}

	MessageScroll *scroll_ = nullptr;
	QHBoxLayout *main_row_ = nullptr;
	int body_spacing_ = -1; // スクロールする前の、アイコンとテキストの列の間隔 (スクロールする間だけ)
	int root_spacing_ = -1; // スクロールする前の、本文とボタンの間隔 (スクロールする間だけ)
	int top_taken_ = 0;     // 主文との間からコンテナーの上の余白へ移した量 (スクロールする間だけ)
	std::vector<QPointer<QWidget>> stops_;
	bool stops_read_ = false;
	bool screen_hooked_ = false;
	QMetaObject::Connection available_hook_;
	int work_area_px_ = 0;
	int work_frame_px_ = 0;
};

QLabel *plain_label(const QString &text)
{
	auto *label = new QLabel(text);
	label->setTextFormat(Qt::PlainText);
	return label;
}

// 折り返すラベル。幅は、ダイアログがすべてのラベルで同じ列を選んで決める (列を決めるまでは、ラベルが自分で幅を
// 狭めない。決めた後に、その列で最後の行の規則に合わないラベルだけが、自分で狭める)。
WrapLabel *wrap_label(const QString &text)
{
	auto *label = new WrapLabel(text);
	label->set_auto_narrow(false);
	return label;
}

// 本文のブロックの種類。ブロックの間の余白を決めるのに使う (近いものはまとまって見えるので、同じまとまりの中は
// 狭く、まとまりの間は広くする)。BulletLocation は、直前の箇条書きの項目の場所の行 (MessageItem::under_bullet)。
enum class Block { None, Main, Paragraph, Bullet, LocationHeading, Location, BulletLocation, Copy };

// 場所の行。選択してコピーできるラベルにする。列に入らない長さなら、区切りの「\」の後で折り返す (省かないので、
// 全文が見え、コピーすると元の文字列になる)。どの長さでも同じ形 (枠のないラベル) なので、ダイアログの中のパスの
// 行の左端がそろう。最後の行の規則は適用しない (パスの末尾のフォルダー名 1 つの行は、パスの形のままにする)。
// 列に入らないときだけ読み取り専用の入力欄にする方法は使わない。右が切れて末尾のフォルダー名が見えず、隣の
// ラベルの行と左端と形がずれるため。
QWidget *path_widget(QWidget *dialog, const QString &path, int column)
{
	auto *label = new WrapLabel(QString(), dialog);
	label->set_auto_narrow(false);
	label->set_breaks(WrapBreaks::PathSeparators);
	label->set_text(path);
	label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
	// 行の右のボタンより低い 1 行のパスは、ボタンのテキストと同じ高さに並べる (縦の中央。QLabel の既定と同じ)。
	// WrapLabel の既定の上寄せでは、テキストがボタンのテキストより約 4 px 上に出る。折り返したパスは、ラベルの
	// 高さがテキストの高さなので、上寄せと同じに見える。
	label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	label->ensurePolished();
	label->setFixedWidth(std::max(1, column));
	return label;
}

QPushButton *open_folder_button(const QString &text, const QString &path)
{
	auto *button = new QPushButton(text);
	button->setAutoDefault(false);
	// アクセシブル名は見えているテキストのままにし、説明に開くパスを入れる。同じ名前のボタンが 2 つか 3 つ並ぶ
	// ダイアログ (結果のダイアログ、切り替えの確認) で、どのパスを開くかを聞き分けられるようにするため。
	button->setAccessibleDescription(path);
	// 開き方は、ドックのカードと設定ウィンドウと同じ関数を使う (存在しないパスは、存在する親を開く。
	// open_location.h)。
	QObject::connect(button, &QPushButton::clicked, button, [path]() { open_folder(path); });
	return button;
}

// [ファイルの場所を開く] と [フォルダーの場所を開く]。アクセシブルな説明は、[フォルダーを開く] と同じく開くパス。
// 開き方 (エクスプローラーで親を開き、そのファイルやフォルダーを選択する) は、呼び出し元が渡す関数に任せる
// (Windows の API は platform/ に閉じ込めるため)。
QPushButton *reveal_file_button(const QString &text, const QString &path,
				const std::function<void(const QString &)> &reveal)
{
	auto *button = new QPushButton(text);
	button->setAutoDefault(false);
	button->setAccessibleDescription(path);
	QObject::connect(button, &QPushButton::clicked, button, [path, reveal]() { reveal(path); });
	return button;
}

// 主文 (1.5 em の見出し) が列に入らずに 2 行になるとき、DockStyle::dialog_main_break_after の文字の直後で折り返す
// (見出しは語の切れ目で折り返す)。折り返したら、1 行目と 2 行目の広い方の幅を返す (呼び出し元は、列をこれより
// 狭めない)。折り返さなければ 0 を返す。折り返す位置には U+2028 (LINE SEPARATOR) を入れる。Qt のラベルと
// QTextLayout はこれを改行として扱い、段落を分けない。アクセシブル名は、折り返す前のテキストにする (呼び出し元が
// 設定する)。
// 次の方法は使わない。主文を短くして 1 行に入れる (どのアプリケーションの Vencord かが主文から消える)。
// ダイアログを広げる (幅の上限 45 em を超える)。補足にも適用する (Windows と OBS のダイアログの本文は文字の間で
// 折り返すので、それと違う折り返しになり、行も増える)。
int break_main_at_phrase(WrapLabel *main, const QString &text, int column, const DockStyle &style)
{
	if (style.dialog_main_break_after == nullptr || style.dialog_main_break_after[0] == 0 || text.isEmpty())
		return 0;
	const QString marks = QString::fromUtf16(style.dialog_main_break_after);
	const QFontMetrics fm = main->fontMetrics();
	const int room = column - main->text_indent();
	const int total = fm.horizontalAdvance(text);
	std::vector<int> widths;
	std::vector<int> positions;
	for (int i = 0; i + 1 < text.size(); ++i) {
		if (marks.contains(text.at(i))) {
			positions.push_back(i + 1);
			widths.push_back(fm.horizontalAdvance(text.left(i + 1)));
		}
	}
	const int min_second = static_cast<int>(std::ceil(room * std::max(0.0, style.min_last_line_ratio)));
	const int k = calc::phrase_break_choice(widths, total, room, min_second);
	if (k < 0)
		return 0;
	const QString first = text.left(positions[static_cast<std::size_t>(k)]);
	const QString second = text.mid(positions[static_cast<std::size_t>(k)]);
	main->set_text(first + QChar(QChar::LineSeparator) + second);
	return std::max(fm.horizontalAdvance(first), fm.horizontalAdvance(second)) + main->text_indent();
}

const char *blocking_name(MessageBlocking blocking)
{
	return blocking == MessageBlocking::OwnerOnly ? "ownerOnly" : "ownerAndParents";
}

// Qt のイベントの種類を、捨てるかどうかの表 (dock_ui_calc.h の owner_hold_action) の種類に置き換える。
calc::HoldEvent hold_event_of(QEvent::Type type)
{
	switch (type) {
	case QEvent::MouseButtonPress:
	case QEvent::MouseButtonDblClick:
	case QEvent::NonClientAreaMouseButtonPress:
	case QEvent::NonClientAreaMouseButtonDblClick:
		return calc::HoldEvent::PointerPress;
	case QEvent::MouseButtonRelease:
	case QEvent::MouseMove:
	case QEvent::NonClientAreaMouseButtonRelease:
	case QEvent::NonClientAreaMouseMove:
		return calc::HoldEvent::PointerOther;
	case QEvent::Wheel:
		return calc::HoldEvent::Wheel;
	case QEvent::KeyPress:
		return calc::HoldEvent::KeyPress;
	case QEvent::KeyRelease:
	case QEvent::ShortcutOverride:
	case QEvent::Shortcut:
		return calc::HoldEvent::KeyOther;
	case QEvent::ContextMenu:
		return calc::HoldEvent::ContextMenu;
	case QEvent::ToolTip:
	case QEvent::WhatsThis:
	case QEvent::QueryWhatsThis:
		return calc::HoldEvent::Hint;
	case QEvent::Enter:
	case QEvent::HoverEnter:
	case QEvent::HoverMove:
		return calc::HoldEvent::HoverIn;
	case QEvent::TouchBegin:
	case QEvent::TabletPress:
		return calc::HoldEvent::TouchBegin;
	case QEvent::TouchUpdate:
	case QEvent::TouchEnd:
	case QEvent::TouchCancel:
	case QEvent::TabletMove:
	case QEvent::TabletRelease:
		return calc::HoldEvent::TouchOther;
	case QEvent::DragEnter:
	case QEvent::DragMove:
	case QEvent::Drop:
		return calc::HoldEvent::Drag;
	case QEvent::Close:
		return calc::HoldEvent::Close;
	case QEvent::WindowActivate:
		return calc::HoldEvent::Activate;
	default:
		return calc::HoldEvent::Other;
	}
}

// イベントの届け先のウィジェット (ウィジェットでなければ、親をたどって最初のウィジェット。QShortcut と QAction の
// イベント)。
QWidget *widget_of(QObject *o)
{
	for (; o != nullptr; o = o->parent()) {
		if (o->isWidgetType())
			return static_cast<QWidget *>(o);
	}
	return nullptr;
}

// ウィジェット w が、止める範囲 region の中にあるか (region そのものか、同じウィンドウの中の子孫)。
// QWidget::isAncestorOf はウィンドウの境で止まるので、範囲の子のウィンドウ (止めているダイアログそのものと、範囲の
// ウィンドウから開いたほかのウィンドウ) は範囲に入らない。
bool in_region(const QWidget *region, const QWidget *w)
{
	return region != nullptr && w != nullptr && (w == region || region->isAncestorOf(w));
}

// 操作を始めた範囲だけを止める (MessageBlocking::OwnerOnly)。ダイアログが開いている間だけアプリケーションの
// イベントフィルターとして登録され、止める範囲 (owner_hold_region) の中のウィジェットに届く入力を、
// owner_hold_action の表に従って捨てる。止めている組み合わせは一覧に持ち、message_dialog_holding で引ける。
class OwnerHold : public QObject {
public:
	static std::vector<OwnerHold *> holds;

	OwnerHold(QWidget *region, QDialog *dialog) : owner_(region), dialog_(dialog)
	{
		holds.push_back(this);
		if (QCoreApplication::instance() != nullptr)
			QCoreApplication::instance()->installEventFilter(this);
	}

	~OwnerHold() override
	{
		if (QCoreApplication::instance() != nullptr)
			QCoreApplication::instance()->removeEventFilter(this);
		holds.erase(std::remove(holds.begin(), holds.end(), this), holds.end());
	}

	OwnerHold(const OwnerHold &) = delete;
	OwnerHold &operator=(const OwnerHold &) = delete;

	const QWidget *region() const { return owner_.data(); }
	QDialog *dialog() const { return dialog_.data(); }

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		const calc::HoldEvent kind = hold_event_of(event->type());
		if (kind == calc::HoldEvent::Other || owner_.isNull() || dialog_.isNull() || !dialog_->isVisible())
			return false;
		const bool in_owner = in_region(owner_.data(), widget_of(watched));
		switch (calc::owner_hold_action(kind, in_owner, event->spontaneous())) {
		case calc::HoldAction::Pass:
			return false;
		case calc::HoldAction::Swallow:
			event->ignore();
			return true;
		case calc::HoldAction::SwallowAndRaise:
			// 閉じるイベントは、受け入れた状態のままだと閉じてしまう (QCloseEvent は受け入れた状態で
			// 作られる)。ignore() してから捨てる。
			event->ignore();
			bring_front();
			return true;
		case calc::HoldAction::PassAndRaise:
			// 止める範囲が前面に来た (タイトルバーを押した、タスクバーやほかのウィンドウから前面に
			// 出された)。アクティブにする処理が済んでから、ダイアログをその上に出す。
			QMetaObject::invokeMethod(this, [this]() { bring_front(); }, Qt::QueuedConnection);
			return false;
		}
		return false;
	}

private:
	void bring_front()
	{
		if (dialog_.isNull() || !dialog_->isVisible())
			return;
		dialog_->raise();
		dialog_->activateWindow();
	}

	QPointer<QWidget> owner_;
	QPointer<QDialog> dialog_;
};

std::vector<OwnerHold *> OwnerHold::holds;

// 操作を始めた範囲だけを止めてダイアログを表示し、閉じるまで待つ (run_owner_only_dialog と、run_message_dialog の
// OwnerOnly の処理の本体)。フォーカスを戻すのは、呼び出し元の 2 つの関数がそれぞれ 1 回だけ行う (戻す途中の
// ウィジェットに、フォーカスのイベントを送らないため)。
int hold_and_wait(QDialog *dialog, QWidget *owner)
{
	if (dialog == nullptr)
		return -1;
	QPointer<QDialog> guard = dialog;
	QWidget *region = owner_hold_region(owner);
	if (region == nullptr) {
		// 操作を始めたウィジェットがないので、止める範囲を決められない。Qt の既定 (exec) で待つ。
		const int result = dialog->exec();
		return guard.isNull() ? -1 : result;
	}
	// 親のウィンドウの子のウィンドウにする (いつも親の上に表示する)。親が別のウィンドウなら、呼び出し元の
	// 指定を残す。
	if (dialog->parentWidget() == nullptr)
		dialog->setParent(owner->window(), dialog->windowFlags() | Qt::Dialog);
	dialog->setWindowModality(Qt::NonModal);
	dialog->setProperty("messageBlocking", QString::fromLatin1(blocking_name(MessageBlocking::OwnerOnly)));
	// exec と open はモーダルにするので使わず、モーダルでないまま表示して、自分のイベントループで待つ。親が削除
	// されるとダイアログも削除されるので、削除されたときもループを抜ける。
	OwnerHold hold(region, dialog);
	QEventLoop loop;
	QObject::connect(dialog, &QDialog::finished, &loop, &QEventLoop::quit);
	QObject::connect(dialog, &QObject::destroyed, &loop, &QEventLoop::quit);
	dialog->show();
	dialog->raise();
	dialog->activateWindow();
	QEventLoop *running = &loop;
	if (!guard.isNull() && guard->isVisible())
		running->exec();
	const int result = guard.isNull() ? -1 : guard->result();
	if (!guard.isNull())
		guard->hide();
	return result;
}

} // namespace

QDialog *make_message_dialog(QWidget *owner, const MessageSpec &spec, const DockStyle &style)
{
	QWidget *parent = owner != nullptr ? owner->window() : nullptr;
	auto *dialog = new MessageDialog(parent);
	dialog->setObjectName(QString::fromLatin1(kObjectName));
	dialog->setWindowTitle(spec.title);
	// 止める範囲。OwnerOnly では Qt のモーダルを使わず、run_message_dialog が止める範囲への入力だけを捨てる
	// (OwnerHold)。どちらの形でも親のウィンドウの子のウィンドウにする (いつも親の上に表示し、タスクバーに別の
	// ボタンを持たない)。
	const bool owner_only = spec.blocking == MessageBlocking::OwnerOnly && parent != nullptr;
	dialog->setWindowModality(owner_only ? Qt::NonModal : Qt::WindowModal);
	dialog->setProperty("messageBlocking",
			    QString::fromLatin1(blocking_name(owner_only ? MessageBlocking::OwnerOnly
									  : MessageBlocking::OwnerAndParents)));
	dialog->setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	dialog->setSizeGripEnabled(false);
	// ダイアログの ID (動的なプロパティ dialogId。テストとプレビューツールがダイアログを見分ける)。
	if (!spec.dialog_id.isEmpty())
		dialog->setProperty("dialogId", spec.dialog_id);
	dialog->ensurePolished();

	const ThemeRead theme = read_theme(dialog, nullptr);
	const DockMetrics m = read_dock_metrics(dialog, theme, style);

	auto *root = new QVBoxLayout(dialog);
	// cordmux が置く間隔は、大きい余白 (0.5 em) だけにする。ダイアログの外周の余白は、テーマと Qt の既定のまま
	// にする (OBS のダイアログと同じ)。
	root->setSpacing(m.large_px);
	auto *body = new QHBoxLayout();
	body->setSpacing(m.large_px);

	// 左上の標準のアイコン。警告か情報だけを使う。
	auto *icon = new QLabel();
	const QStyle::StandardPixmap sp = spec.icon == MessageIcon::Warning ? QStyle::SP_MessageBoxWarning
									     : QStyle::SP_MessageBoxInformation;
	const int icon_px = dialog->style()->pixelMetric(QStyle::PM_MessageBoxIconSize, nullptr, dialog);
	icon->setPixmap(dialog->style()->standardIcon(sp, nullptr, dialog)
				.pixmap(QSize(icon_px, icon_px), dialog->devicePixelRatioF()));
	icon->setFixedSize(icon_px, icon_px);
	body->addWidget(icon, 0, Qt::AlignTop);

	auto *texts = new QVBoxLayout();
	// ブロックの間隔は 1 つずつ加える (setSpacing では、まとまりの中と外を分けられない)。
	texts->setSpacing(0);
	// 本文 (主文とボタンの間のすべてのブロック) は 1 つのコンテナーに入れる。ダイアログが画面の作業領域に
	// 入らないときだけ、そのコンテナーをスクロール領域に入れ替える (MessageDialog::fit_height)。コンテナーは
	// 余白 0 で背景のないウィジェットなので、スクロールしないダイアログの見た目は、コンテナーのない形と同じになる。
	// flow は、次のブロックを加えるレイアウト (主文まではテキストの列、本文からはコンテナーのレイアウト)。
	QVBoxLayout *flow = texts;
	std::vector<WrapLabel *> wraps;
	QStringList description;
	Block prev = Block::None;
	// 場所の行は列の幅が決まってから作るので、レイアウトの場所だけを確保しておく。
	struct PendingLocation {
		QVBoxLayout *slot;
		QString path;
		bool open_folder;
		bool under_bullet;
		bool file;
		bool folder_location; // 親のフォルダーの中で選択して表示するフォルダー (MessageItem::folder_location)
		QPushButton *open = nullptr; // 行の右のボタン (列の幅を決める前に作る)
	};
	std::vector<PendingLocation> pending;
	// 次に加えるブロックが、2 つ目からのグループの始まりか (MessageItem::group_start と、MessageSpec::groups の
	// 2 つ目から)。次の gap_before が読んで false に戻す (場所の行の見出しがあれば、見出しの前で使う)。
	bool next_group_start = false;
	// グループの間の空き。高さは場所の行を置いた後に決める (下の「グループの間の見える間隔」)。after_location は、
	// 境の直前のブロックが場所の行のとき、その行の pending の番号 (ほかは -1)。
	struct GroupGap {
		QSpacerItem *spacer;
		int after_location;
	};
	std::vector<GroupGap> group_gaps;
	// 次のブロックの前の余白。見出しの段落とその直後の箇条書き、箇条書きどうし、場所の行の見出しとその場所の行、
	// 箇条書きとその項目の場所の行は小さい余白 (0.25 em)、ほかのブロックの間は大きい余白 (0.5 em) にする。すべてを
	// 同じ間隔にすると、2 つのグループが 1 つの列に見えるため。項目の場所の行は箇条書きの一部として数える (次の
	// 箇条書きとの間も小さい余白)。2 つ目からのグループの始まりの前は、まとまりの間隔 (0.75 em) にする。見える
	// 間隔をグループの中より広く保つための調整は、場所の行を置いた後に適用する (下の「グループの間の見える間隔」)。
	auto gap_before = [&](Block next) {
		const bool body_starts = prev == Block::Main;
		if (prev != Block::None) {
			const bool after_item = prev == Block::Bullet || prev == Block::BulletLocation;
			const bool same_group = (next == Block::Bullet && (prev == Block::Paragraph || after_item)) ||
						(next == Block::Location && prev == Block::LocationHeading) ||
						(next == Block::BulletLocation && after_item);
			if (next_group_start) {
				auto *spacer = new QSpacerItem(0, m.group_gap_px, QSizePolicy::Minimum, QSizePolicy::Fixed);
				flow->addSpacerItem(spacer);
				const bool after_location = prev == Block::Location || prev == Block::BulletLocation;
				group_gaps.push_back(
					GroupGap{spacer, after_location ? static_cast<int>(pending.size()) - 1 : -1});
			} else {
				flow->addSpacing(same_group ? m.small_px : m.large_px);
			}
		}
		// 主文の次のブロックから、本文のコンテナーに入れる。主文と本文の間はテキストの列に残す (スクロールする
		// ときも、主文の下の間隔はスクロールしない)。
		if (body_starts) {
			dialog->main_gap = texts->itemAt(texts->count() - 1)->spacerItem();
			auto *content = new QWidget();
			content->setObjectName(QString::fromLatin1(kBodyObjectName));
			auto *rest = new QVBoxLayout(content);
			rest->setContentsMargins(0, 0, 0, 0);
			rest->setSpacing(0);
			texts->addWidget(content);
			dialog->content = content;
			dialog->flow = rest;
			flow = rest;
		}
		next_group_start = false;
		prev = next;
	};

	// 主文は補足より大きいフォントにする。倍率は下で適用する (DockStyle::dialog_main_scale)。倍率が 0 のとき
	// だけ、OBS のテーマのクラス text-large に任せる。
	WrapLabel *main = wrap_label(spec.main);
	if (style.dialog_main_scale <= 0.0)
		main->setProperty("class", QStringLiteral("text-large"));
	gap_before(Block::Main);
	texts->addWidget(main);
	wraps.push_back(main);
	description << spec.main;

	auto add_paragraph = [&](const QString &text, Block kind) {
		WrapLabel *label = wrap_label(text);
		gap_before(kind);
		flow->addWidget(label);
		wraps.push_back(label);
		description << text;
	};
	// 箇条書き。記号はラベルの左の余白に描く (ラベルは 1 つで、読み上げるのはテキストだけ。記号を別のラベルに
	// しない)。折り返した行は記号の右にそろう。ダイアログのアクセシブルな説明にも記号を加えない (1 行ずつ並ぶので、
	// 箇条書きの形は行で伝わる。テキストをコードで連結しない)。
	auto add_bullet = [&](const QString &text) {
		WrapLabel *label = wrap_label(text);
		label->set_bullet(QString::fromLatin1(kBulletMark), m.large_px);
		gap_before(Block::Bullet);
		flow->addWidget(label);
		wraps.push_back(label);
		description << text;
	};
	auto add_location = [&](const QString &heading, const QString &path, bool open_folder, bool under_bullet,
				bool file, bool folder_location) {
		// 項目の場所の行は、直前が箇条書きのときだけ箇条書きの列にそろえる (見出しがあれば、項目のパスでは
		// ない)。
		const bool item_place = under_bullet && heading.isEmpty() &&
					(prev == Block::Bullet || prev == Block::BulletLocation);
		if (!heading.isEmpty())
			add_paragraph(heading, Block::LocationHeading);
		auto *slot = new QVBoxLayout();
		slot->setContentsMargins(0, 0, 0, 0);
		gap_before(item_place ? Block::BulletLocation : Block::Location);
		flow->addLayout(slot);
		pending.push_back(PendingLocation{slot, path, open_folder, item_place, file, folder_location});
		description << path;
	};

	if (!spec.supplement.isEmpty())
		add_paragraph(spec.supplement, Block::Paragraph);
	for (const MessageItem &item : spec.items) {
		next_group_start = item.group_start;
		switch (item.kind) {
		case MessageItem::Kind::Text:
			add_paragraph(item.text, Block::Paragraph);
			break;
		case MessageItem::Kind::Bullet:
			add_bullet(item.text);
			break;
		case MessageItem::Kind::Location:
			add_location(item.text, item.path, item.open_folder, item.under_bullet, item.file,
				     item.folder_location);
			break;
		}
	}
	for (const QString &b : spec.bullets)
		add_bullet(b);
	for (std::size_t gi = 0; gi < spec.groups.size(); ++gi) {
		const MessageGroup &g = spec.groups[gi];
		next_group_start = gi > 0;
		if (!g.heading.isEmpty())
			add_paragraph(g.heading, Block::Paragraph);
		for (const QString &item : g.items)
			add_bullet(item);
	}
	// 中身のないグループの始まりの指定を、後の場所の行へ持ち越さない。
	next_group_start = false;
	for (const MessageLocation &loc : spec.locations)
		add_location(loc.heading, loc.path, loc.open_folder, false, false, false);

	// コピーできる入力欄と [コピー]。
	if (!spec.copy_lines.isEmpty()) {
		auto *copy = new QPlainTextEdit(spec.copy_lines.join(QLatin1Char('\n')));
		copy->setReadOnly(true);
		copy->setLineWrapMode(QPlainTextEdit::NoWrap);
		const QFontMetrics fm(copy->font());
		const int frame = copy->frameWidth() * 2 + static_cast<int>(copy->document()->documentMargin() * 2);
		copy->setFixedHeight(fm.lineSpacing() * static_cast<int>(spec.copy_lines.size()) + frame + 2);
		gap_before(Block::Copy);
		flow->addWidget(copy);
		description << spec.copy_lines;
		if (!spec.copy_text.isEmpty()) {
			auto *copy_button = new QPushButton(spec.copy_text);
			copy_button->setAutoDefault(false);
			const QString all = spec.copy_lines.join(QLatin1Char('\n'));
			QObject::connect(copy_button, &QPushButton::clicked, copy_button, [all]() {
				if (QClipboard *cb = QGuiApplication::clipboard())
					cb->setText(all);
			});
			auto *row = new QHBoxLayout();
			row->addStretch(1);
			row->addWidget(copy_button);
			flow->addSpacing(m.small_px);
			flow->addLayout(row);
		}
	}

	body->addLayout(texts, 1);
	root->addLayout(body);

	// 右下のボタン。渡した順に並べる。
	auto *buttons = new QHBoxLayout();
	buttons->addStretch(1);
	QPushButton *default_button = nullptr;
	for (std::size_t i = 0; i < spec.buttons.size(); ++i) {
		const MessageButton &b = spec.buttons[i];
		auto *button = new QPushButton(b.text);
		const int index = static_cast<int>(i);
		if (b.is_default) {
			button->setDefault(true);
			default_button = button;
		}
		if (b.is_escape)
			dialog->escape_index = index;
		if (!b.open_path.isEmpty()) {
			const QString path = b.open_path;
			QObject::connect(button, &QPushButton::clicked, button, [path]() { open_folder(path); });
		} else {
			const MessageButtonRole role = b.role;
			QObject::connect(button, &QPushButton::clicked, dialog,
					 [dialog, index, role]() { dialog->finish(index, role); });
		}
		buttons->addWidget(button);
	}
	// is_escape のボタンがなければ、Reject の役のボタン ([キャンセル]) を Esc のボタンにする。
	if (dialog->escape_index < 0) {
		for (std::size_t i = 0; i < spec.buttons.size(); ++i) {
			if (spec.buttons[i].role == MessageButtonRole::Reject) {
				dialog->escape_index = static_cast<int>(i);
				break;
			}
		}
	}
	root->addLayout(buttons);

	// 主文と補足を、ダイアログのアクセシブルな説明に入れる。QAccessible::Alert のイベントは出さない。
	dialog->setAccessibleName(spec.title);
	dialog->setAccessibleDescription(description.join(QLatin1Char('\n')));

	// 主文の大きさ。DockStyle::dialog_main_scale が正ならその倍率 (既定は 1.5)。0 ならテーマのクラス text-large の
	// ままにし、テーマがそのクラスを持たず主文が補足と同じ大きさになるときだけ、cordmux が倍率を適用する。
	for (QLabel *label : dialog->findChildren<QLabel *>())
		label->ensurePolished();
	if (style.dialog_main_scale > 0.0) {
		apply_scale(main, base_point_size(dialog), style.dialog_main_scale);
	} else {
		auto *plain = plain_label(QString());
		plain->setParent(dialog);
		plain->hide();
		plain->ensurePolished();
		if (QFontInfo(main->font()).pointSizeF() <= QFontInfo(plain->font()).pointSizeF())
			apply_scale(main, base_point_size(dialog), style.dialog_main_fallback_scale);
		delete plain;
	}
	main->ensurePolished();
	// 箇条書きの記号の幅は、ラベルを polish した後のフォントで測り直す (set_bullet は、作ったときのフォントで
	// 測っている)。
	for (WrapLabel *w : wraps)
		w->set_bullet(w->text_indent() > 0 ? QString::fromLatin1(kBulletMark) : QString(), m.large_px);

	// 場所の行の右のボタンを先に作る (場所の行の全体、つまり箇条書きの字下げ + パス + 間隔 + ボタンを、列の幅の
	// 計算に入れるため)。
	int bullet_indent = 0;
	for (const WrapLabel *w : wraps)
		bullet_indent = std::max(bullet_indent, w->text_indent());
	for (PendingLocation &p : pending) {
		if (p.file) {
			if (!spec.open_file_location_text.isEmpty() && spec.reveal_file)
				p.open = reveal_file_button(spec.open_file_location_text, p.path, spec.reveal_file);
		} else if (p.folder_location) {
			// テキストか関数がなければボタンを置かない ([フォルダーを開く] で代えると、テキストと動作が
			// 食い違うため。message_dialog.h)。
			if (!spec.open_folder_location_text.isEmpty() && spec.reveal_file)
				p.open = reveal_file_button(spec.open_folder_location_text, p.path, spec.reveal_file);
		} else if (p.open_folder && !spec.open_folder_text.isEmpty()) {
			p.open = open_folder_button(spec.open_folder_text, p.path);
		}
		if (p.open != nullptr) {
			p.open->setParent(dialog);
			p.open->ensurePolished();
		}
	}
	const auto location_room = [&](const PendingLocation &p, int col) {
		int room = col - (p.under_bullet ? bullet_indent : 0);
		if (p.open != nullptr)
			room -= p.open->sizeHint().width() + m.large_px;
		return room;
	};

	// 折り返す幅は、いちばん長い 1 行と 40 em の小さい方にする (Qt に任せると 20 字前後で折り返す)。場所の行も
	// 1 行の長さに数える (パスは区切りで折り返すが、40 em までは折り返さずに 1 行で見せる。数えないと、短い本文の
	// ダイアログで、長いパスが列の幅で何行にも折れる)。
	int column = 0;
	for (const WrapLabel *w : wraps)
		column = std::max(column, w->fontMetrics().horizontalAdvance(w->text()) + w->text_indent());
	{
		const QFontMetrics body_fm = dialog->fontMetrics();
		for (const PendingLocation &p : pending)
			column = std::max(column, body_fm.horizontalAdvance(p.path) + (column - location_room(p, column)));
	}
	column = std::min(column, m.paragraph_max_px);
	// 主文が 2 行になるなら、語の切れ目で折り返す。折り返したら列を測り直し (主文は 2 つの行の広い方)、下の幅の
	// 調整でも列をその幅より狭めない (狭めると 1 行目が折れて 3 行になるため)。
	const int main_floor = break_main_at_phrase(main, spec.main, column, style);
	if (main_floor > 0) {
		main->setAccessibleName(spec.main);
		column = main_floor;
		for (const WrapLabel *w : wraps) {
			if (w != main)
				column = std::max(column, w->fontMetrics().horizontalAdvance(w->text()) + w->text_indent());
		}
		column = std::min(column, m.paragraph_max_px);
	}
	// 最後の行: すべてのラベルの最後の行が文字数と長さの規則に合う、いちばん広い列を、元の幅の 4 分の 3 まで
	// 狭めて探す (DockStyle の min_last_line_graphemes、min_last_line_ratio、wrap_narrow_floor_ratio)。ラベルごとに
	// 幅を変えると右端がそろわないので、ダイアログのすべてのラベルで 1 つの列にする。両方に合う列がなければ、
	// 文字数だけに合う列にする。
	const auto all_fit = [&](int w, double ratio) {
		for (const WrapLabel *part : wraps) {
			if (!last_line_fits(part->text(), part->font(), w - part->text_indent(),
					    style.min_last_line_graphemes, ratio))
				return false;
		}
		return true;
	};
	{
		const int floor_w = std::max(static_cast<int>(column * style.wrap_narrow_floor_ratio), main_floor);
		int chosen = -1;
		int count_only = -1;
		for (int w = column; w >= floor_w && w > 0; --w) {
			if (all_fit(w, style.min_last_line_ratio)) {
				chosen = w;
				break;
			}
			if (count_only < 0 && all_fit(w, 0.0))
				count_only = w;
		}
		if (chosen > 0)
			column = chosen;
		else if (count_only > 0)
			column = count_only;
	}
	// 1 つの列ですべてのラベルが規則に合わなかったとき (長い主文と長い段落がどちらも 2 行になるダイアログなど)、
	// 合わないラベルだけが、ラベルの幅はそのままで、テキストを折り返す幅を狭める (WrapLabel の自動の調整。合って
	// いるラベルでは狭める幅が 0 で、何も変わらない)。列の下限 (wrap_narrow_floor_ratio) を下げて共通の列を
	// 探し続ける方法は使わない。合っているラベルまで狭くなり、ダイアログが縦に伸びるため。
	for (WrapLabel *part : wraps) {
		part->setFixedWidth(std::max(1, column));
		part->set_last_line_rule(style);
		// 語の切れ目で折り返した主文は、折り返す位置を決めた後なので狭めない (狭めると 1 行目が折れて 3 行に
		// なることがある)。
		part->set_auto_narrow(!(part == main && main_floor > 0));
	}

	// 場所の行を置く。行の全体 (箇条書きの字下げ + パス + 間隔 + [フォルダーを開く]) を列の幅に収める。パスの
	// ラベルの幅はその残りで、入らなければラベルが区切りで折り返す。列の幅で測ったパスの右にボタンを加えると、
	// 長いパスでダイアログが幅の上限 (45 em) を超えて広がるため。
	// row_pad は、場所の行のボタンがパスのテキストより高い分の半分 (テキストは縦の中央に置くので、テキストの上と
	// 下に同じだけ空く) のうち、いちばん大きいもの。下の「グループの間の見える間隔」で使う。
	int row_pad = 0;
	for (const PendingLocation &p : pending) {
		const int indent = p.under_bullet ? bullet_indent : 0;
		if (indent > 0)
			p.slot->setContentsMargins(indent, 0, 0, 0);
		QPushButton *open = p.open;
		const int room = std::max(1, location_room(p, column));
		QWidget *path = path_widget(dialog, p.path, room);
		if (open != nullptr) {
			const int path_h = path->hasHeightForWidth() ? path->heightForWidth(room) : path->sizeHint().height();
			row_pad = std::max(row_pad, (open->sizeHint().height() - path_h) / 2);
			auto *row = new QHBoxLayout();
			row->setSpacing(m.large_px);
			// パスのラベルは、テキストの高さのまま縦の中央に置く (ボタンより低い 1 行のパスを、ボタンの
			// 高さまで伸ばさない)。伸ばすとラベルがテキストより高くなる
			// (tests/ui_preview/render_sets.py が検出する形)。
			row->addWidget(path, 1, Qt::AlignVCenter);
			row->addWidget(open, 0, Qt::AlignVCenter);
			p.slot->addLayout(row);
		} else {
			p.slot->addWidget(path);
		}
	}

	// グループの間の見える間隔: グループの境の見える間隔 (背景と違う色のピクセルの行の間) を、グループの中のどの
	// 見える間隔より広くする。ウィジェットの矩形の間隔 (まとまりの間隔 0.75 em と、大きい余白 0.5 em) だけでは、
	// 次の 2 つの場合に見える間隔が逆になる。
	//  (1) 場所の行のボタンの背景がダイアログの背景とほとんど変わらないテーマ (Yami Light、Yami Classic) では、
	//      場所の行のテキストだけが見え、テキストの上下にボタンの高さの余り (row_pad) が空いて見える。場所の行の
	//      前後のグループの中の間隔がその分広く見え、まとまりの間隔より広くなる。どの境にも row_pad を加え、
	//      見える間隔の差を、ウィジェットの間隔の差 (0.25 em) のまま保つ。
	//  (2) 境の直前がボタン ([フォルダーを開く]、[ファイルの場所を開く]) で、ボタンの背景が見えるテーマ (Yami)
	//      では、段落のラベルはテキストの下に空き (テーマのラベルの下の余白 + フォントのディセント = under_text)
	//      を持つが、ボタンは枠線まで見えるので、ボタンの後の境だけがその分狭く見える。ドックのカードの同じ調整
	//      (dock/notice_list.cpp の arrange_flow) と同じ量で、row_pad より大きいときはこちらを加える。
	// どちらのテーマかは判定しない (背景の差のしきい値を決める根拠がなく、WCAG 1.4.11 もボタンの背景の境を見える
	// ものとして求めない)。背景が見えても見えなくても、グループの間がグループの中より広くなる量にする。そのため、
	// ボタンの後の境と段落の後の境の見える間隔は、どちらかのテーマで少し違う。
	// まとまりの間隔を 1.25 em 前後に上げる方法は使わない。3 つの余白の値から外れ、場所の行のないダイアログの
	// グループの間まで広がるため。
	if (!group_gaps.empty()) {
		int under_text = 0;
		for (WrapLabel *w : wraps) {
			if (w == main)
				continue;
			w->ensurePolished();
			under_text = std::max(0, w->rect().bottom() - w->contentsRect().bottom()) + w->margin() +
				     QFontMetrics(w->font()).descent();
			break;
		}
		for (const GroupGap &g : group_gaps) {
			const bool button_before = g.after_location >= 0 &&
						   pending[static_cast<std::size_t>(g.after_location)].open != nullptr;
			g.spacer->changeSize(0, m.group_gap_px + (button_before ? std::max(under_text, row_pad) : row_pad),
					     QSizePolicy::Minimum, QSizePolicy::Fixed);
		}
		flow->invalidate();
		texts->invalidate();
	}

	// 大きさは中身から決める (利用者が大きさを変えるダイアログではない)。折り返すラベルの幅は 40 em までなので、
	// ダイアログの幅は 45 em の上限に収まる。
	root->setSizeConstraint(QLayout::SetFixedSize);

	// 画面の作業領域に入らないときに本文をスクロールするための設定 (MessageDialog::fit_height)。合わせるのは、
	// 表示する直前 (showEvent)、画面を移ったとき、作業領域が変わったとき。作っただけのダイアログ (プレビュー
	// ツールとテストが描くもの) は、set_message_dialog_work_area で作業領域を固定したときだけ合わせる。
	dialog->root = root;
	dialog->body_row = body;
	dialog->texts = texts;
	dialog->main_label = main;
	dialog->reach = m.focus_ring_px + m.focus_ring_gap_px;
	dialog->gap_px = m.large_px;
	dialog->min_body_px = std::max(kMinBodyLines * dialog->fontMetrics().lineSpacing(),
				       3 * dialog->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, dialog));

	// フォーカスリングを付け、フォーカスを既定のボタンに置く。
	auto *ring = new FocusRing(dialog);
	ring->set_color(theme.text);
	ring->set_metrics(m.focus_ring_px, m.focus_ring_gap_px);
	if (default_button != nullptr)
		default_button->setFocus(Qt::OtherFocusReason);
	return dialog;
}

int message_dialog_choice(const QDialog *dialog)
{
	const auto *d = dynamic_cast<const MessageDialog *>(dialog);
	return d != nullptr ? d->choice : -1;
}

bool set_message_dialog_work_area(QDialog *dialog, int available_px, int frame_px)
{
	auto *d = dynamic_cast<MessageDialog *>(dialog);
	if (d == nullptr)
		return false;
	d->set_work_area(available_px, frame_px);
	return d->scrolls();
}

bool message_dialog_scrolls(const QDialog *dialog)
{
	const auto *d = dynamic_cast<const MessageDialog *>(dialog);
	return d != nullptr && d->scrolls();
}

int run_message_dialog(QWidget *owner, const MessageSpec &spec, const DockStyle &style)
{
	// 閉じた後にフォーカスを戻す先。ダイアログが開いている間に削除されることがあるので、QPointer で持つ。
	const QPointer<QWidget> before = QApplication::focusWidget();
	const QPointer<QWidget> fallback = spec.focus_fallback;
	QPointer<QDialog> dialog = make_message_dialog(owner, spec, style);
	if (dialog->windowModality() == Qt::NonModal && dialog->parentWidget() != nullptr) {
		// 操作を始めた範囲だけを止める (OwnerOnly)。色を選ぶダイアログなどの run_owner_only_dialog も、同じ
		// 本体 (hold_and_wait) を使う。
		hold_and_wait(dialog.data(), owner);
	} else {
		dialog->exec();
	}
	int choice = -1;
	if (!dialog.isNull()) {
		choice = message_dialog_choice(dialog.data());
		delete dialog.data();
	}
	if (!before.isNull() && before->isEnabled() && before->isVisible())
		before->setFocus(Qt::OtherFocusReason);
	else if (!fallback.isNull() && fallback->isEnabled() && fallback->isVisible())
		fallback->setFocus(Qt::OtherFocusReason);
	return choice;
}

QWidget *owner_hold_region(QWidget *owner)
{
	// 同じウィンドウの中で祖先をたどり、いちばん近い QDockWidget を範囲にする。フローティングのドックは
	// QDockWidget そのものがウィンドウなので、ウィンドウの境を見る前に型を見る。設定ウィンドウなどは、
	// ウィンドウの境で止まり、そのウィンドウが範囲になる。
	for (QWidget *w = owner; w != nullptr; w = w->parentWidget()) {
		if (qobject_cast<QDockWidget *>(w) != nullptr)
			return w;
		if (w->isWindow())
			return w;
	}
	return nullptr;
}

int run_owner_only_dialog(QDialog *dialog, QWidget *owner)
{
	// 閉じた後にフォーカスを戻す先。ダイアログが開いている間に削除されることがあるので、QPointer で持つ。
	const QPointer<QWidget> before = QApplication::focusWidget();
	const int result = hold_and_wait(dialog, owner);
	if (!before.isNull() && before->isEnabled() && before->isVisible())
		before->setFocus(Qt::OtherFocusReason);
	return result;
}

QDialog *message_dialog_holding(const QWidget *widget)
{
	if (widget == nullptr)
		return nullptr;
	// 後から開いたダイアログを先に見る (同じ範囲に 2 つ重なったときは、上のダイアログを返す)。
	for (auto it = OwnerHold::holds.rbegin(); it != OwnerHold::holds.rend(); ++it) {
		QDialog *d = (*it)->dialog();
		if (in_region((*it)->region(), widget) && d != nullptr && d->isVisible())
			return d;
	}
	return nullptr;
}

} // namespace cordmux::dock::ui
