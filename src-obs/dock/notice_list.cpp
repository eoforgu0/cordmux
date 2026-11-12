// SPDX-License-Identifier: Apache-2.0

// ドックのスクロール領域のメッセージ (notice_list.h) を実装する。

#include "dock/notice_list.h"

#include <QApplication>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QResizeEvent>
#include <QStringList>
#include <QStyle>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>

#include "dock/layout_hold.h"
#include "dock/ui/elide_label.h"
#include "dock/ui/set_class.h"
#include "dock/ui/status_icons.h"
#include "dock/ui/text_resolve.h"
#include "dock/ui/type_scale.h"
#include "dock/ui/wrap_label.h"
#include "ledger/text_ids.h"

namespace cordmux::dock {

namespace {

// カードの左端のバーの太さ (px)。
constexpr int kBarPx = 3;

ui::TierMark tier_mark_of(ledger::dockview::Tier tier)
{
	switch (tier) {
	case ledger::dockview::Tier::L1:
		return ui::TierMark::L1;
	case ledger::dockview::Tier::L2:
		return ui::TierMark::L2;
	case ledger::dockview::Tier::L3:
		return ui::TierMark::L3;
	}
	return ui::TierMark::L3;
}

// 表示しているウィジェットをレイアウトから外して隠し、削除はイベントループの次の周回で行う。並びの保留が解けて、
// ためておいた変化を反映するのは、ポインターがスクロール領域を出るイベントの最中のことがある。Qt
// (QApplicationPrivate::dispatchEnterLeave) は 1 回の移動で「入るウィジェット」の一覧を先に作ってからイベントを順に送る
// ので、その最中に一覧のウィジェットをその場で delete すると、Qt が削除済みのウィジェットにイベントを送り、OBS ごと
// クラッシュする。隠しただけのウィジェットにイベントが届いても何も起きない (NoticeList::update がメッセージのカードを削除する
// ときも同じ形)。並びの保留が解けたときの反映だけを次の周回に送る方法は使わない。表示中のウィジェットをイベントの
// 最中に削除する所がほかにもあれば、同じクラッシュが起きる。削除の方法を 1 つに決めれば、どこから呼ばれても安全である。
// フォーカスがこのウィジェット (かその子) にあれば、先に focus_to に移す。隠すと Qt が次の押せるウィジェットに
// フォーカスを移し、続けて押した Space が別のボタンを押すためである。
void retire_widget(QWidget *w, QLayout *layout, QWidget *focus_to)
{
	if (w == nullptr)
		return;
	QWidget *focus = QApplication::focusWidget();
	if (focus_to != nullptr && focus != nullptr && (focus == w || w->isAncestorOf(focus)))
		focus_to->setFocus(Qt::OtherFocusReason);
	if (layout != nullptr)
		layout->removeWidget(w);
	w->hide();
	w->deleteLater();
}

int tier_number(ledger::dockview::Tier tier)
{
	switch (tier) {
	case ledger::dockview::Tier::L1:
		return 1;
	case ledger::dockview::Tier::L2:
		return 2;
	case ledger::dockview::Tier::L3:
		return 3;
	}
	return 3;
}

// ボタンを幅に合わせて折り返すレイアウト (Qt のサンプルの FlowLayout と同じ考え方)。ボタンはテキストの幅のまま伸ばさず、
// 左寄せにする。伸ばすと、押す範囲が文と重なって見え、文とボタンの区別が弱まる。
class FlowLayout : public QLayout {
public:
	FlowLayout(int spacing, QWidget *parent) : QLayout(parent), spacing_(spacing) { setContentsMargins(0, 0, 0, 0); }
	~FlowLayout() override
	{
		while (QLayoutItem *item = takeAt(0))
			delete item;
	}
	void set_spacing_px(int px)
	{
		spacing_ = px;
		invalidate();
	}
	void addItem(QLayoutItem *item) override { items_.push_back(item); }
	int count() const override { return static_cast<int>(items_.size()); }
	QLayoutItem *itemAt(int index) const override
	{
		return index >= 0 && index < count() ? items_[static_cast<std::size_t>(index)] : nullptr;
	}
	QLayoutItem *takeAt(int index) override
	{
		if (index < 0 || index >= count())
			return nullptr;
		QLayoutItem *item = items_[static_cast<std::size_t>(index)];
		items_.erase(items_.begin() + index);
		return item;
	}
	Qt::Orientations expandingDirections() const override { return {}; }
	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int width) const override { return do_layout(QRect(0, 0, width, 0), true); }
	void setGeometry(const QRect &rect) override
	{
		QLayout::setGeometry(rect);
		do_layout(rect, false);
	}
	QSize sizeHint() const override { return minimumSize(); }
	QSize minimumSize() const override
	{
		QSize size;
		for (QLayoutItem *item : items_) {
			if (!item->isEmpty())
				size = size.expandedTo(item->minimumSize());
		}
		return size;
	}

private:
	int do_layout(const QRect &rect, bool test) const
	{
		int x = rect.x();
		int y = rect.y();
		int line = 0;
		for (QLayoutItem *item : items_) {
			if (item->isEmpty())
				continue;
			const QSize hint = item->sizeHint();
			int next = x + hint.width() + spacing_;
			if (next - spacing_ > rect.right() + 1 && line > 0) {
				x = rect.x();
				y += line + spacing_;
				next = x + hint.width() + spacing_;
				line = 0;
			}
			if (!test)
				item->setGeometry(QRect(QPoint(x, y), hint));
			x = next;
			line = std::max(line, hint.height());
		}
		return y + line - rect.y();
	}
	std::vector<QLayoutItem *> items_;
	int spacing_;
};

// カードのテキストのラベル (見出し、段落、準備の段階の行、2 行目、場所の行)。折り返す位置はラベルが決める
// (ui::WrapLabel::set_breaks)。文は KeepNumbers にし、「(2人)」「13:41」のような数と単位の語を行で分けず、最後の行が
// 2 字以下なら、行の数を増やさない範囲で行の長さをならす。行の数を増やさないのは、カードの高さを変えず、一覧を下に
// 押さないためである。場所の行は PathSeparators (location_label)。高さは、WrapLabel が置かれる幅 (最大の幅で切った幅)
// で測る (ui::CappedWrapLabel と同じ)。Qt の折り返しのままだと、「(2」「人)」のように数と単位が分かれる。ini の
// テキストに折り返してはいけない位置の目印を書く方法は使わない。テキストを書く側は幅を知らず、ini を読む全部の所に
// 目印が残る。
ui::WrapLabel *card_label(QWidget *parent, ui::WrapBreaks breaks)
{
	auto *label = new ui::WrapLabel(QString(), parent);
	label->set_breaks(breaks);
	return label;
}

// テキストが変わったときだけ入れ替える (折り返す位置を決め直さない)。比べるのは、改行を入れる前のテキスト。
void set_text_if_changed(ui::WrapLabel *label, const QString &text)
{
	if (label->source_text() != text)
		label->set_text(text);
}

// 場所の行のラベル。省かずに全文を表示し、幅が足りなければ区切りの直後で折り返す (1 つの区切りの間が幅に入らない
// ときだけ文字の間で折り返す)。マウスとキーボードで選んでコピーできる (Tab で来て Shift+矢印と Ctrl+C。コピーする
// テキストに改行は入らない。WrapLabel の動作)。全文が見えているので、ツールチップに同じテキストを置かない。
// アクセシブル名は全文 (WrapLabel が元のテキストを入れる)。結果のダイアログを閉じた後も場所が残るのはドックだけである。
// 本文の後の場所の行 (Notice::location) と、段落の間の場所の行 (Notice::body_locations) は同じ実装。読み取り専用の
// 入力欄にすると、20 em の幅では末尾の (削除する) フォルダーの名前が見えない。
ui::WrapLabel *location_label(QWidget *parent)
{
	ui::WrapLabel *label = card_label(parent, ui::WrapBreaks::PathSeparators);
	label->setObjectName(QStringLiteral("cordmuxNoticeLocation"));
	label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
	return label;
}

// 場所の行を開くボタン。場所の行のすぐ下に、テキストの幅のまま左寄せで置く (NoticeBody のコンストラクターのコメント)。
QPushButton *open_folder_button(QWidget *parent)
{
	auto *b = new QPushButton(parent);
	b->setObjectName(QStringLiteral("cordmuxNoticeOpenFolder"));
	b->setAutoDefault(false);
	b->setFocusPolicy(Qt::TabFocus); // クリックではフォーカスを取らない
	b->setText(ui::resolve_key(ledger::text::keys::kDialogOpenFolderButton));
	new PressGuard(b); // 現れた直後のクリックを捨てる
	return b;
}

// 場所の行に開くボタンを置くかを決め、テキストを合わせる。フォルダーの行には、View の open_folder が true のときに
// [フォルダーを開く] を置く。ファイルの行 (途中で止まったアンインストールのカードにある、Vesktop の設定のファイル) には、
// いつも [ファイルの場所を開く] を置く (エクスプローラーでそのファイルを選んで開く)。dock_view.h の LocationLine の
// 約束で、File の行は open_folder を使わず、読む側が種類を見る。結果のダイアログの [ファイルの場所を開く] と同じテキストと
// 開き方にする。objectName も種類で分ける (プレビューツールとテストが数える)。アクセシブル名は見えているテキストのまま、
// accessibleDescription は開く場所にする。
bool configure_open_button(QPushButton *b, const ledger::dockview::LocationLine &line)
{
	const bool file = line.kind == ledger::dockview::LocationKind::File;
	const bool wanted = !line.path.empty() && (file || line.open_folder);
	if (!wanted)
		return false;
	const QString text = ui::resolve_key(file ? ledger::text::keys::kDialogOpenFileLocationButton
						 : ledger::text::keys::kDialogOpenFolderButton);
	if (b->text() != text)
		b->setText(text);
	const QString name = file ? QStringLiteral("cordmuxNoticeOpenFileLocation") : QStringLiteral("cordmuxNoticeOpenFolder");
	if (b->objectName() != name)
		b->setObjectName(name);
	b->setAccessibleDescription(QString::fromStdString(line.path));
	return true;
}

} // namespace

// ---- NoticeBody ----

NoticeBody::NoticeBody(QWidget *parent) : QWidget(parent)
{
	layout_ = new QVBoxLayout(this);
	layout_->setContentsMargins(0, 0, 0, 0);
	heading_ = card_label(this, ui::WrapBreaks::KeepNumbers);
	heading_->hide();
	layout_->addWidget(heading_);
	flow_ = new QWidget(this);
	flow_layout_ = new QVBoxLayout(flow_);
	flow_layout_->setContentsMargins(0, 0, 0, 0);
	flow_->hide();
	layout_->addWidget(flow_);
	stages_ = new QWidget(this);
	auto *stages_layout = new QVBoxLayout(stages_);
	stages_layout->setContentsMargins(0, 0, 0, 0);
	stages_->hide();
	// 本文の後の場所の行 (location_label のコメント)。
	location_ = location_label(this);
	location_->hide();
	// [フォルダーを開く]。場所の行のすぐ下に、テキストの幅のまま左寄せで置く (場所の行と同じまとまりなので、間は小さい
	// 余白)。アンインストールを終えた後の次の操作は「OBS を終了してから削除」で、結果のダイアログはもう閉じているので、
	// OBS を閉じる前にエクスプローラーで開いておけるボタンがドックに必要になる。
	// ダイアログでは場所の行の右に置くが、ドックのカードの列は 15-26 em (200 px 前後) しかなく、右に置くと場所の行が
	// 90 px 前後になって、削除するフォルダーの名前がさらに見えなくなる。ドックのカードのボタンは文の下に左寄せなので、
	// その形に合わせる (どの場所を開くかは、真上の行と accessibleDescription の場所で分かる)。幅で右と下を切り替えると、
	// 境目の幅でスクロールバーが出入りしたときに形が行き来する。カードの操作のボタンの並びに入れると、View の ActionId が
	// 必要になり、場所と結び付かないボタンの並びに混ざる。
	open_ = open_folder_button(this);
	open_->hide();
	QObject::connect(open_, &QPushButton::clicked, this, [this]() { open_path(open_path_, open_kind_); });
	line2_ = card_label(this, ui::WrapBreaks::KeepNumbers);
	line2_->hide();
	layout_->addWidget(stages_);
	layout_->addWidget(location_);
	layout_->addWidget(open_, 0, Qt::AlignLeft);
	layout_->addWidget(line2_);
}

void NoticeBody::set_look(const DockLook &look, bool body_muted)
{
	look_ = look;
	body_muted_ = body_muted;
	layout_->setSpacing(look_.metrics.small_px);
	flow_layout_->setSpacing(look_.metrics.small_px);
	if (auto *sl = stages_->layout())
		sl->setSpacing(look_.metrics.small_px);
	arrange_flow();
	ui::apply_type(heading_, ui::TypeLevel::Heading, look_.base_pt, look_.style.first_line_scale);
	paint();
}

void NoticeBody::paint()
{
	// 見出しは本文の色の太字、影響と次の操作と 2 行目 (録画ファイルの作成の進み具合) は薄い色 (text-muted)。薄い色がパネルの上で
	// 読めないテーマは、本文の色のままにする。場所の行と準備の段階は、読み取って使う値なので本文の色にする。
	const QString muted = body_muted_ ? QStringLiteral("text-muted") : QString();
	ui::set_class(line2_, muted);
	for (ui::WrapLabel *p : paragraphs_) {
		ui::set_class(p, muted);
		// 段落は 40 em で折り返す。
		p->setMaximumWidth(look_.metrics.paragraph_max_px);
	}
	heading_->setMaximumWidth(look_.metrics.paragraph_max_px);
	line2_->setMaximumWidth(look_.metrics.paragraph_max_px);
}

void NoticeBody::rebuild_paragraphs(int count)
{
	while (static_cast<int>(paragraphs_.size()) > count) {
		retire_widget(paragraphs_.back(), flow_layout_, nullptr); // その場で delete しない
		paragraphs_.pop_back();
	}
	while (static_cast<int>(paragraphs_.size()) < count) {
		// 並べるのは arrange_flow (見出しの後、準備の段階の前のコンテナー flow_ の中)。
		paragraphs_.push_back(card_label(flow_, ui::WrapBreaks::KeepNumbers));
	}
	arranged_gap_ = -1; // 並べ直す
}

void NoticeBody::rebuild_inline_locations()
{
	// 描かない行 (after が段落の数以上の行) は、dock_view.h の BodyLocation の約束どおり落とす。
	std::vector<const ledger::dockview::BodyLocation *> wanted;
	for (const ledger::dockview::BodyLocation &loc : notice_.body_locations) {
		if (loc.after < notice_.body.size() && !loc.line.path.empty())
			wanted.push_back(&loc);
	}
	while (inline_.size() > wanted.size()) {
		InlineLocation &last = inline_.back();
		retire_widget(last.label, flow_layout_, this); // その場で delete しない
		retire_widget(last.open, flow_layout_, this);
		inline_.pop_back();
		arranged_gap_ = -1;
	}
	while (inline_.size() < wanted.size()) {
		InlineLocation loc;
		loc.label = location_label(flow_);
		loc.open = open_folder_button(flow_);
		const std::size_t index = inline_.size();
		QObject::connect(loc.open, &QPushButton::clicked, this, [this, index]() {
			if (index < inline_.size())
				open_path(inline_[index].path, inline_[index].kind);
		});
		inline_.push_back(loc);
		arranged_gap_ = -1;
	}
	for (std::size_t i = 0; i < wanted.size(); ++i) {
		InlineLocation &loc = inline_[i];
		const ledger::dockview::LocationLine &line = wanted[i]->line;
		set_text_if_changed(loc.label, QString::fromStdString(line.path));
		loc.path = line.path;
		loc.kind = line.kind;
		loc.open_wanted = configure_open_button(loc.open, line);
		if (loc.after != wanted[i]->after) {
			loc.after = wanted[i]->after;
			arranged_gap_ = -1;
		}
	}
}

void NoticeBody::arrange_flow()
{
	// まとまりの始まりの前の空き。空きの前後に入るレイアウトの spacing (小さい余白) 2 つ分と合わせて、まとまりの間隔
	// (0.75 em) になる高さにする。まとまりの間隔が小さい余白 2 つ分より狭い [密度] では 0 にする (それでも、まとまりの間は
	// 小さい余白 2 つ分で、まとまりの中の小さい余白より広い)。
	const int gap = std::max(0, look_.metrics.group_gap_px - 2 * look_.metrics.small_px);
	// まとまりの境の前がボタン ([フォルダーを開く] か [ファイルの場所を開く]) のときに足す空き。段落のラベルはテキストの
	// 下に空き (テーマのラベルの下の余白 + 文字のディセント) を持つが、ボタンは枠線まで描くので持たない。まとまりの中の
	// 段落の間は「上のラベルの下の空き + 小さい余白 + 下のラベルの上の空き」に見えるので、この空きを足さないと、ボタンの
	// 後のまとまりの境だけ、その空きの分だけ見える間が狭くなる。テーマの `.frame-notice QLabel { padding: 4px 0 }` は
	// ラベルの上下に 4 px ずつ足すので、足さないと、Yami と Yami Light の 20 em では、ボタンの後の境 (16 px) が、まとまりの
	// 中の段落の間 (17-19 px) より狭くなる。テーマの余白を消す方法は、cordmux のラベルだけが OBS のテーマの見た目から
	// 外れるので使わない。
	int after_button = 0;
	// どのまとまりの境にも足す空き (row_pad)。段落の間の場所の行のボタンが、1 行のラベルより高い分の半分にする。ボタンの
	// 背景がカードの背景とほとんど違わないテーマ (Yami Classic) では、ボタンのテキストだけが見え、テキストの下にこの分の
	// 空きが見える。まとまりの中の「ボタン -> 次の段落」の見える間がその分広がり、段落の後のまとまりの境と同じに見える。
	// ダイアログ (dock/ui/message_dialog.cpp のまとまりの間) と同じ考え方で、どの境にも足して、見える間の差を、ウィジェットの
	// 間の差 (まとまりの間隔 - 小さい余白) のまま保つ。場所の行にボタンが 1 つもないカードでは 0 (まとまりの中にボタンが
	// なく、広がる間もない)。
	// ボタンの後の境は after_button と row_pad の大きい方にする (ダイアログと同じ形)。OBS の既定の Yami では、ボタンの後の
	// 境と段落の後の境の見える間がそろい (英語、26 em で 25 px と 25 px)、Yami Classic では 20 px と 16 px になる。どちらの
	// テーマでも、まとまりの間はまとまりの中より広い。ボタンの後の境を after_button + row_pad にすると、2 つの境の差が
	// 広がる (Yami で 30 px と 25 px)。段落の後の境だけに足すと、row_pad がラベルの下の空きより大きいテーマで、ボタンの
	// 後の境が段落の後の境より狭くなる。
	int row_pad = 0;
	if (!paragraphs_.empty()) {
		ui::WrapLabel *p = paragraphs_.front();
		p->ensurePolished();
		const int below_text = std::max(0, p->rect().bottom() - p->contentsRect().bottom()) + p->margin();
		after_button = below_text + QFontMetrics(p->font()).descent();
		// 1 行のラベルの高さは、テキストの高さとラベルの上下の余白から求める (現在のラベルの高さは、2 行に折り返した
		// ラベルでは大きくなるので使わない)。
		const int one_line_h = QFontMetrics(p->font()).height() + (p->rect().height() - p->contentsRect().height()) +
				       2 * p->margin();
		for (const InlineLocation &loc : inline_) {
			if (!loc.open_wanted)
				continue;
			loc.open->ensurePolished();
			row_pad = std::max(row_pad, (loc.open->sizeHint().height() - one_line_h) / 2);
		}
	}
	std::vector<std::size_t> afters;
	std::vector<bool> opens;
	for (const InlineLocation &loc : inline_) {
		afters.push_back(loc.after);
		opens.push_back(loc.open_wanted);
	}
	if (gap == arranged_gap_ && after_button == arranged_after_button_ && row_pad == arranged_row_pad_ &&
	    notice_.body_group_starts == arranged_starts_ && afters == arranged_afters_ && opens == arranged_opens_)
		return;
	arranged_gap_ = gap;
	arranged_after_button_ = after_button;
	arranged_row_pad_ = row_pad;
	arranged_starts_ = notice_.body_group_starts;
	arranged_afters_ = afters;
	arranged_opens_ = opens;
	// ウィジェットは flow_ の子のまま、レイアウトの項目だけを作り直す (ウィジェットを削除しないので、フォーカスも残る)。
	while (QLayoutItem *item = flow_layout_->takeAt(0))
		delete item;
	for (std::size_t i = 0; i < paragraphs_.size(); ++i) {
		const bool group_start = i > 0 && std::find(notice_.body_group_starts.begin(),
							   notice_.body_group_starts.end(), i) != notice_.body_group_starts.end();
		if (group_start) {
			// 境の直前に見えるウィジェットは、前の段落の後の場所の行のうち最後のもののボタン (表示していれば) か、その
			// ラベルか、前の段落。
			bool button_before = false;
			for (const InlineLocation &loc : inline_) {
				if (loc.after == i - 1)
					button_before = loc.open_wanted;
			}
			flow_layout_->addSpacing(gap + (button_before ? std::max(after_button, row_pad) : row_pad));
		}
		flow_layout_->addWidget(paragraphs_[i]);
		for (const InlineLocation &loc : inline_) {
			if (loc.after != i)
				continue;
			flow_layout_->addWidget(loc.label);
			flow_layout_->addWidget(loc.open, 0, Qt::AlignLeft);
		}
	}
}

void NoticeBody::open_path(const std::string &path, ledger::dockview::LocationKind kind) const
{
	if (!path.empty() && opener_)
		opener_(path, kind);
}

void NoticeBody::set_notice(const ledger::dockview::Notice &notice, bool heading_visible)
{
	const bool same = has_notice_ && notice == notice_;
	notice_ = notice;
	has_notice_ = true;
	if (!same) {
		set_text_if_changed(heading_, ui::resolve(notice_.headline));
		rebuild_paragraphs(static_cast<int>(notice_.body.size()));
		for (std::size_t i = 0; i < notice_.body.size(); ++i)
			set_text_if_changed(paragraphs_[i], ui::resolve(notice_.body[i]));
		rebuild_inline_locations();
		arrange_flow();

		// 準備の段階。1 つの段階を 1 行にし、テキストは ini の 1 つの文字列の全文にする (名前と値をコードで連結しない)。
		QLayout *sl = stages_->layout();
		while (QLayoutItem *item = sl->takeAt(0)) {
			retire_widget(item->widget(), nullptr, nullptr); // その場で delete しない
			delete item;
		}
		for (const ledger::dockview::StageItem &stage : notice_.stages) {
			ui::WrapLabel *l = card_label(stages_, ui::WrapBreaks::KeepNumbers);
			l->set_text(ui::resolve(stage.text));
			sl->addWidget(l);
		}
		stages_->setVisible(!notice_.stages.empty());

		// 場所の行。省かず、区切りの後で折り返し、選んでコピーできる (location_label)。表示と非表示は
		// apply_parts_visible で決める。
		set_text_if_changed(location_, notice_.location.has_value() ? QString::fromStdString(notice_.location->path)
									     : QString());
		open_wanted_ = notice_.location.has_value() && configure_open_button(open_, *notice_.location);
		if (open_wanted_) {
			open_path_ = notice_.location->path;
			open_kind_ = notice_.location->kind;
		} else {
			open_path_.clear();
		}

		set_text_if_changed(line2_, notice_.line2.has_value() ? ui::resolve(*notice_.line2) : QString());
		apply_parts_visible();
		paint();
	}
	set_heading_visible(heading_visible);
}

void NoticeBody::apply_parts_visible()
{
	// 隠すウィジェット (場所の行と [フォルダーを開く]) がフォーカスを持っていれば、先にこのウィジェットに移す。隠すと
	// Qt が次の押せるウィジェットにフォーカスを移し、続けて押した Space が別のボタンを押すためである。
	QWidget *focus = QApplication::focusWidget();
	const bool open_shown = open_wanted_;
	const bool location_shown = notice_.location.has_value();
	const bool flow_shown = !paragraphs_.empty() || !inline_.empty();
	if (focus != nullptr && ((focus == open_ && !open_shown) || (focus == location_ && !location_shown) ||
				 (!flow_shown && flow_->isAncestorOf(focus))))
		setFocus(Qt::OtherFocusReason);
	for (const InlineLocation &loc : inline_) {
		if (focus == loc.open && !loc.open_wanted)
			setFocus(Qt::OtherFocusReason);
		const bool was_hidden = loc.open->isHidden();
		loc.open->setVisible(loc.open_wanted);
		if (loc.open_wanted && was_hidden)
			PressGuard::arm_widget(loc.open); // 現れた直後のクリックを捨てる
	}
	// 空のコンテナーを表示したままにすると、レイアウトの spacing (小さい余白) が 1 つ余計に入る。
	flow_->setVisible(flow_shown);
	stages_->setVisible(!notice_.stages.empty());
	location_->setVisible(location_shown);
	const bool open_was_hidden = open_->isHidden();
	open_->setVisible(open_shown);
	if (open_shown && open_was_hidden)
		PressGuard::arm_widget(open_); // 現れた直後のクリックを捨てる
	line2_->setVisible(notice_.line2.has_value());
}

void NoticeBody::set_heading_visible(bool visible)
{
	heading_->setVisible(visible);
}

QString NoticeBody::description() const
{
	QStringList parts;
	// 改行を含まない元のテキストを使う (スクリーンリーダーとツールチップに折り返しを持ち込まない)。折りたたんでいても
	// 全部を入れ、見えているかではなく View が持つかで決める。段落の間の場所の行は、その段落の直後に入れる (表示の順)。
	for (std::size_t i = 0; i < paragraphs_.size(); ++i) {
		parts << paragraphs_[i]->source_text();
		for (const InlineLocation &loc : inline_) {
			if (loc.after == i)
				parts << loc.label->source_text();
		}
	}
	if (notice_.location.has_value())
		parts << location_->source_text();
	if (!line2_->source_text().isEmpty())
		parts << line2_->source_text();
	return parts.join(QLatin1Char('\n'));
}

int NoticeBody::open_button_width_px() const
{
	int w = open_wanted_ ? open_->sizeHint().width() : 0;
	for (const InlineLocation &loc : inline_) {
		if (loc.open_wanted)
			w = std::max(w, loc.open->sizeHint().width());
	}
	return w;
}

std::vector<QWidget *> NoticeBody::focus_order() const
{
	// 表示の順。段落の間の場所の行 (ラベルとその下のボタン) を段落の順に並べ、その後に本文の後の場所の行と
	// [フォルダーを開く] を置く (arrange_flow とコンストラクターの並び)。
	std::vector<QWidget *> order;
	for (std::size_t i = 0; i < paragraphs_.size(); ++i) {
		for (const InlineLocation &loc : inline_) {
			if (loc.after != i)
				continue;
			order.push_back(loc.label);
			order.push_back(loc.open);
		}
	}
	order.push_back(location_);
	order.push_back(open_);
	return order;
}

void NoticeBody::arm_guards()
{
	if (!open_->isHidden())
		PressGuard::arm_widget(open_);
	for (const InlineLocation &loc : inline_) {
		if (!loc.open->isHidden())
			PressGuard::arm_widget(loc.open);
	}
}

// ---- NoticeButtons ----

NoticeButtons::NoticeButtons(QWidget *parent) : QWidget(parent)
{
	auto *v = new QVBoxLayout(this);
	v->setContentsMargins(0, 0, 0, 0);
	row_ = new QWidget(this);
	new FlowLayout(3, row_);
	v->addWidget(row_);
	// 押せない理由の 1 文。カードの段落と同じラベル (KeepNumbers) にし、数と単位の語を行で分けず、最後の行の長さを
	// ならす。カードのテキストのラベルを 1 つの実装にそろえないと、折り返し方が分かれ、テーマの字下げでテキストの左端が
	// 段落とずれることがある。
	reason_ = card_label(this, ui::WrapBreaks::KeepNumbers);
	reason_->hide();
	v->addWidget(reason_);
	hide();
}

void NoticeButtons::set_look(const DockLook &look)
{
	look_ = look;
	static_cast<FlowLayout *>(row_->layout())->set_spacing_px(look_.metrics.small_px);
	layout()->setSpacing(look_.metrics.small_px);
	// 段落と同じく 40 em で折り返す。
	reason_->setMaximumWidth(look_.metrics.paragraph_max_px);
}

void NoticeButtons::set_buttons(const std::vector<ledger::dockview::ViewButton> &buttons)
{
	if (set_once_ && buttons == views_)
		return;
	set_once_ = true;
	const bool same_count = buttons.size() == buttons_.size();
	views_ = buttons;
	if (!same_count) {
		// その場で delete しない (retire_widget のコメント)。
		for (QPushButton *b : buttons_)
			retire_widget(b, row_->layout(), this);
		buttons_.clear();
		for (std::size_t i = 0; i < views_.size(); ++i) {
			auto *b = new QPushButton(row_);
			b->setAutoDefault(false);
			b->setFocusPolicy(Qt::TabFocus); // クリックではフォーカスを取らない
			new PressGuard(b); // 現れた直後のクリックを捨てる
			const std::size_t index = i;
			QObject::connect(b, &QPushButton::clicked, this, [this, index]() {
				if (index < views_.size() && on_press_)
					on_press_(views_[index]);
			});
			row_->layout()->addWidget(b);
			buttons_.push_back(b);
		}
	}
	apply_views();
}

void NoticeButtons::set_busy(const std::set<ledger::ActionId> &busy)
{
	if (busy == busy_)
		return;
	busy_ = busy;
	if (set_once_)
		apply_views();
}

void NoticeButtons::apply_views()
{
	QStringList reasons;
	for (std::size_t i = 0; i < views_.size(); ++i) {
		const ledger::dockview::ViewButton &view = views_[i];
		QPushButton *b = buttons_[i];
		// 処理中のテキスト。押した操作のボタンだけが持つ (ほかの操作はテキストを変えない)。
		const char *busy_key = nullptr;
		if (busy_.count(view.id) != 0) {
			if (view.id == ledger::ActionId::WriteVencordDir || view.id == ledger::ActionId::RepairVencordDir)
				busy_key = ledger::text::keys::kDockAddToVesktopBusyButton;
		}
		const QString text = busy_key != nullptr ? ui::resolve_key(busy_key) : ui::resolve(view.label);
		if (b->text() != text)
			b->setText(text);
		const bool enabled = !view.disabled_reason.has_value() && busy_key == nullptr;
		const bool had_focus = b->hasFocus();
		const bool was_enabled = b->isEnabled();
		b->setEnabled(enabled);
		if (was_enabled && !enabled && had_focus) {
			// 押せなくすると、Qt はフォーカスを次のウィジェットに移す。済んだら戻すために、移した先を保存する。
			focus_moved_to_ = QApplication::focusWidget();
			focus_owner_ = b;
		} else if (!was_enabled && enabled && focus_owner_.data() == b) {
			if (!focus_moved_to_.isNull() && QApplication::focusWidget() == focus_moved_to_.data())
				b->setFocus(Qt::OtherFocusReason);
			focus_moved_to_.clear();
			focus_owner_.clear();
		}
		if (busy_key != nullptr) {
			b->setToolTip(QString());
			b->setAccessibleDescription(QString());
			continue; // 処理中は押せない理由を伝えない (テキストが処理中であることを伝える)
		}
		const QString reason = enabled ? QString() : ui::resolve(*view.disabled_reason);
		b->setToolTip(reason);
		b->setAccessibleDescription(reason);
		if (!enabled)
			reasons << reason;
		// プライマリのボタン (OBS のクラス button-primary)。
		ui::set_class(b, view.primary ? QStringLiteral("button-primary") : QString());
	}
	// 押せない理由はボタンの下に 1 文で表示する。同じ理由が続くときは 1 回だけにする。
	reasons.removeDuplicates();
	set_text_if_changed(reason_, reasons.join(QLatin1Char('\n')));
	reason_->setVisible(!reasons.isEmpty());
	setVisible(!views_.empty());
}

void NoticeButtons::arm_guards()
{
	for (QPushButton *b : buttons_)
		PressGuard::arm_widget(b);
}

int NoticeButtons::widest_button_px() const
{
	int w = 0;
	for (QPushButton *b : buttons_)
		w = std::max(w, b->sizeHint().width());
	return w;
}

bool NoticeButtons::event(QEvent *event)
{
	// 子のボタンがツールチップを持たないと、Qt はツールチップのイベントを親に伝える (QApplication::notify)。ここで止める。
	if (event->type() == QEvent::ToolTip && toolTip().isEmpty()) {
		QToolTip::hideText();
		return true;
	}
	return QWidget::event(event);
}

// ---- NoticeCard ----

NoticeCard::NoticeCard(QWidget *parent) : QFrame(parent)
{
	setObjectName(QStringLiteral("cordmuxNoticeCard"));
	// 外側の縦のレイアウトには、アイコンとテキストの列の行と、その下に (狭いときだけ) 操作のボタンを置く。カードの
	// 外周の余白は外側のレイアウトが持つ。
	auto *outer = new QVBoxLayout(this);
	row_ = new QHBoxLayout();
	row_->setContentsMargins(0, 0, 0, 0);
	outer->addLayout(row_);
	mark_ = new ui::MarkLabel(this);
	row_->addWidget(mark_, 0, Qt::AlignTop);
	column_ = new QVBoxLayout();
	column_->setContentsMargins(0, 0, 0, 0);
	row_->addLayout(column_, 1);
	body_ = new NoticeBody(this);
	column_->addWidget(body_);
	buttons_ = new NoticeButtons(this);
	column_->addWidget(buttons_);
}

void NoticeCard::set_look(const DockLook &look)
{
	look_ = look;
	QLayout *h = layout();
	const int large = look_.metrics.large_px;
	const int small = look_.metrics.small_px;
	// テキストの列と、その下に置いたボタンの間は、テキストの列の中の本文とボタンの間と同じ小さい余白にする。
	h->setSpacing(small);
	row_->setSpacing(large);
	column_->setSpacing(small);
	apply_frame();
	if (body_ != nullptr)
		body_->set_look(look_, ui::muted_ok(look_.theme, ui::panel_fill(look_.theme, look_.card_frame_ok)));
	if (buttons_ != nullptr)
		buttons_->set_look(look_);
	apply_mark();
	place_buttons();
}

void NoticeCard::resizeEvent(QResizeEvent *event)
{
	QFrame::resizeEvent(event);
	place_buttons();
}

void NoticeCard::place_buttons()
{
	if (buttons_ == nullptr)
		return;
	// テキストの列の幅 = カードの中の幅 - アイコン - アイコンとテキストの間。最も広いボタンがそこに入らなければ、ボタンを
	// アイコンの列の下 (カードの内側の左端) から始める。形が行き来しないように、下からテキストの列に戻すときだけ、小さい
	// 余白の分の余裕を求める (スクロールバーの出入りで幅が数 px 変わるため。DockWidget::layout_l1_body の不感帯と同じ
	// 考え方)。いつもアイコンの列の下から始めると、通常の幅でボタンがテキストの列とそろわず、ほかのカードと形が分かれる。
	// ボタンのテキストを省くと、押すものの名前が読めなくなる。
	const int widest = buttons_->widest_button_px();
	const QMargins m = layout()->contentsMargins();
	const int column = contentsRect().width() - m.left() - m.right() - mark_->sizeHint().width() - row_->spacing();
	bool under = false;
	if (widest > 0)
		under = buttons_under_mark_ ? widest > column - look_.metrics.small_px : widest > column;
	if (under == buttons_under_mark_)
		return;
	buttons_under_mark_ = under;
	auto *outer = static_cast<QVBoxLayout *>(layout());
	if (under) {
		column_->removeWidget(buttons_);
		outer->addWidget(buttons_);
	} else {
		outer->removeWidget(buttons_);
		column_->addWidget(buttons_);
	}
	setProperty("buttonsUnderMark", under);
	// ボタンが動いたので、クリックを捨てるガードをかけ直す。
	buttons_->arm_guards();
}

QSize NoticeCard::minimumSizeHint() const
{
	QSize size = QFrame::minimumSizeHint();
	if (buttons_ == nullptr || buttons_under_mark_ || body_ == nullptr)
		return size;
	const int widest = buttons_->widest_button_px();
	if (widest <= 0)
		return size;
	// テキストの列から始めている間も、狭くなればアイコンの列の下に移すので、最小はその形の幅 (ボタン 1 つと、
	// アイコン + テキストの列の最小の広い方)。
	const QMargins frame = contentsMargins();
	const QMargins m = layout()->contentsMargins();
	const int column = mark_->sizeHint().width() + row_->spacing() + body_->minimumSizeHint().width();
	const int narrow = frame.left() + frame.right() + m.left() + m.right() + std::max(widest, column);
	size.setWidth(std::min(size.width(), narrow));
	return size;
}

void NoticeCard::apply_frame()
{
	// 背景とバーは paintEvent で自分で描く。テーマの枠 (frame-notice と標準の枠) は使わない。枠線のないパネルにして、
	// カードの上でボタンが見分けにくくなるテーマでも、パネルの色をボタンから離す (ui::panel_fill)。
	ui::set_class(this, QString());
	setFrameShape(QFrame::NoFrame);
	// カードの内側の余白。左はバーの分を足す。
	const int large = look_.metrics.large_px;
	layout()->setContentsMargins(kBarPx + large, large, large, large);
	update();
}

QColor NoticeCard::bar_color() const
{
	switch (notice_.tier) {
	case ledger::dockview::Tier::L1:
		return look_.theme.has_danger && look_.theme.danger.isValid() ? look_.theme.danger : look_.theme.text;
	case ledger::dockview::Tier::L2:
		return look_.theme.warning.isValid() ? look_.theme.warning : look_.theme.text;
	case ledger::dockview::Tier::L3:
		break;
	}
	return QColor();
}

void NoticeCard::apply_mark()
{
	if (!has_notice_)
		return;
	const ui::TierMark mark = tier_mark_of(notice_.tier);
	const QColor color = look_.theme.text;
	const ui::TierIconSet set = look_.style.tier_icon_set;
	const int px = look_.metrics.icon_px;
	mark_->set_painter(px, [mark, px, color, set](qreal dpr) { return ui::tier_pixmap(mark, px, color, dpr, set); });
}

void NoticeCard::set_notice(const ledger::dockview::Notice &notice, bool heading_visible)
{
	const bool tier_changed = !has_notice_ || notice.tier != notice_.tier;
	notice_ = notice;
	has_notice_ = true;
	setProperty("noticeKey", QString::fromStdString(notice_.key));
	setProperty("tier", tier_number(notice_.tier));
	if (tier_changed)
		update();
	apply_mark();
	if (body_ != nullptr) {
		body_->set_notice(notice_, heading_visible);
		// アクセシブル名は見出し、accessibleDescription は影響と次の操作にする。
		setAccessibleName(ui::resolve(notice_.headline));
		QString desc = body_->description();
		// メッセージのレベル 3 のツールチップ (録画のためだけに受け取っている理由など)。accessibleDescription にも入れる。
		const QString tip = notice_.tip.has_value() ? ui::resolve(*notice_.tip) : QString();
		setToolTip(tip);
		if (!tip.isEmpty())
			desc = desc.isEmpty() ? tip : desc + QLatin1Char('\n') + tip;
		setAccessibleDescription(desc);
	}
	if (buttons_ != nullptr) {
		buttons_->set_buttons(notice_.buttons);
		place_buttons();
	}
}

void NoticeCard::set_heading_visible(bool visible)
{
	if (body_ != nullptr)
		body_->set_heading_visible(visible);
}

void NoticeCard::set_on_press(NoticeButtonCallback cb)
{
	if (buttons_ != nullptr)
		buttons_->set_on_press(std::move(cb));
}

void NoticeCard::set_folder_opener(const FolderOpener &opener)
{
	if (body_ != nullptr)
		body_->set_folder_opener(opener);
}

void NoticeCard::set_busy(const std::set<ledger::ActionId> &busy)
{
	if (buttons_ != nullptr)
		buttons_->set_busy(busy);
}

void NoticeCard::arm_guards()
{
	if (buttons_ != nullptr)
		buttons_->arm_guards();
	if (body_ != nullptr)
		body_->arm_guards();
}

int NoticeCard::widest_button_px() const
{
	int w = buttons_ != nullptr ? buttons_->widest_button_px() : 0;
	if (body_ != nullptr)
		w = std::max(w, body_->open_button_width_px());
	return w;
}

void NoticeCard::paintEvent(QPaintEvent *event)
{
	QFrame::paintEvent(event);
	// パネルの背景と、左端のバー (レベル 3 はバーなし)。
	QPainter p(this);
	p.fillRect(rect(), ui::panel_fill(look_.theme, look_.card_frame_ok));
	const QColor bar = bar_color();
	if (bar.isValid())
		p.fillRect(QRect(0, 0, kBarPx, height()), bar);
}

// ---- NoticeList ----

NoticeList::NoticeList(QWidget *parent) : QWidget(parent)
{
	layout_ = new QVBoxLayout(this);
	layout_->setContentsMargins(0, 0, 0, 0);
	// 消えるカードにフォーカスがあったときに、フォーカスを受け取る先 (Tab では止まらない)。
	setFocusPolicy(Qt::ClickFocus);
}

void NoticeList::set_look(const DockLook &look)
{
	look_ = look;
	has_look_ = true;
	layout_->setSpacing(look_.metrics.small_px);
	for (auto &[key, card] : by_key_) {
		(void)key;
		if (!card.isNull())
			card->set_look(look_);
	}
}

NoticeCard *NoticeList::make_card(const ledger::dockview::Notice &)
{
	auto *card = new NoticeCard(this);
	card->set_look(look_);
	card->set_on_press(on_press_);
	card->set_folder_opener(opener_);
	card->set_busy(busy_);
	return card;
}

bool NoticeList::update(const std::vector<ledger::dockview::Notice> &notices, bool allow_moves)
{
	std::vector<const ledger::dockview::Notice *> wanted;
	for (const ledger::dockview::Notice &n : notices)
		wanted.push_back(&n);

	// その場で書き換えてよいもの (今あるウィジェットの中身) は、並びの保留の間も書き換える。
	std::vector<std::string> wanted_keys;
	for (const ledger::dockview::Notice *n : wanted) {
		wanted_keys.push_back(n->key);
		auto it = by_key_.find(n->key);
		if (it == by_key_.end() || it->second.isNull())
			continue;
		it->second->set_notice(*n, true);
	}

	const bool moves_needed = wanted_keys != order_;
	if (!moves_needed)
		return false;
	if (!allow_moves)
		return true; // 追加と削除と並べ替えは、並びの保留が解けた後に行う

	// 削除するウィジェットにフォーカスがあれば、削除する前にフォーカスをこのウィジェットに移す。Qt が次のウィジェットに
	// 移すと、続けて押した Space が別のボタンを押すためである。
	QWidget *focus = QApplication::focusWidget();
	std::map<std::string, QPointer<NoticeCard>> next;
	for (const ledger::dockview::Notice *n : wanted) {
		auto it = by_key_.find(n->key);
		if (it != by_key_.end() && !it->second.isNull()) {
			next[n->key] = it->second;
			by_key_.erase(it);
		}
	}
	for (auto &[key, card] : by_key_) {
		(void)key;
		if (card.isNull())
			continue;
		if (focus != nullptr && (card.data() == focus || card->isAncestorOf(focus)))
			setFocus(Qt::OtherFocusReason);
		layout_->removeWidget(card.data());
		card->hide();
		card->deleteLater(); // ボタンの clicked シグナルの処理の途中で削除しない
	}
	by_key_.clear();

	// View の順に並べ直す。前と同じ位置に留まらないウィジェットは動いたので、クリックを捨てるガードをかけ直す。
	std::vector<std::string> old_order = order_;
	while (QLayoutItem *item = layout_->takeAt(0))
		delete item;
	for (std::size_t i = 0; i < wanted.size(); ++i) {
		const ledger::dockview::Notice &n = *wanted[i];
		QPointer<NoticeCard> card;
		auto it = next.find(n.key);
		const bool created = it == next.end();
		if (!created)
			card = it->second;
		else
			card = make_card(n);
		card->set_notice(n, true);
		layout_->addWidget(card.data());
		card->show();
		const bool same_place = !created && i < old_order.size() && old_order[i] == n.key;
		if (!same_place)
			card->arm_guards();
		by_key_[n.key] = card;
	}
	order_ = wanted_keys;
	setVisible(!order_.empty());
	return false;
}

void NoticeList::set_on_press(NoticeButtonCallback cb)
{
	on_press_ = std::move(cb);
	for (auto &[key, card] : by_key_) {
		(void)key;
		if (!card.isNull())
			card->set_on_press(on_press_);
	}
}

void NoticeList::set_folder_opener(FolderOpener opener)
{
	opener_ = std::move(opener);
	for (auto &[key, card] : by_key_) {
		(void)key;
		if (!card.isNull())
			card->set_folder_opener(opener_);
	}
}

void NoticeList::set_busy(const std::set<ledger::ActionId> &busy)
{
	busy_ = busy;
	for (auto &[key, card] : by_key_) {
		(void)key;
		if (!card.isNull())
			card->set_busy(busy_);
	}
}

int NoticeList::widest_button_px() const
{
	int w = 0;
	for (const auto &[key, card] : by_key_) {
		(void)key;
		if (!card.isNull())
			w = std::max(w, card->widest_button_px());
	}
	return w;
}

std::vector<NoticeCard *> NoticeList::cards() const
{
	std::vector<NoticeCard *> out;
	for (const std::string &key : order_) {
		auto it = by_key_.find(key);
		if (it != by_key_.end() && !it->second.isNull())
			out.push_back(it->second.data());
	}
	return out;
}

} // namespace cordmux::dock
