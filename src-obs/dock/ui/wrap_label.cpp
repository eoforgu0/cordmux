// SPDX-License-Identifier: Apache-2.0

// wrap_label.h の実装。

#include "dock/ui/wrap_label.h"

#include <QAction>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QStyle>
#include <QStyleOptionFrame>
#include <QTextBoundaryFinder>
#include <QTextLayout>
#include <QTextOption>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "dock/ui/dock_ui_calc.h"

namespace cordmux::dock::ui {

namespace {

int grapheme_count(const QString &text)
{
	QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
	int count = 0;
	while (finder.toNextBoundary() > 0)
		++count;
	return count;
}

struct LastLine {
	int lines = 0;
	int graphemes = 0;
	double natural_width = 0.0;
};

LastLine last_line_of(const QString &text, const QFont &font, int width)
{
	LastLine out;
	QTextLayout layout(text, font);
	QTextOption option;
	option.setWrapMode(QTextOption::WordWrap);
	layout.setTextOption(option);
	layout.beginLayout();
	int last_start = 0;
	int last_length = 0;
	for (;;) {
		QTextLine line = layout.createLine();
		if (!line.isValid())
			break;
		line.setLineWidth(width);
		++out.lines;
		last_start = line.textStart();
		last_length = line.textLength();
		out.natural_width = line.naturalTextWidth();
	}
	layout.endLayout();
	out.graphemes = grapheme_count(text.mid(last_start, last_length).trimmed());
	return out;
}

bool fits(const LastLine &l, int width, int min_graphemes, double min_ratio)
{
	if (l.lines < 2)
		return true;
	if (l.graphemes < min_graphemes)
		return false;
	return min_ratio <= 0.0 || l.natural_width >= min_ratio * width;
}

// text の from から始まる 1 行 (幅 width、Qt の折り返し mode) に入る文字数 (UTF-16 の単位)。少なくとも 1 つの
// 書記素クラスターを入れる (幅が 1 文字より狭くても、行を進めるため)。
int fit_length(const QString &text, int from, const QFont &font, int width, QTextOption::WrapMode mode)
{
	const QString rest = text.mid(from);
	QTextLayout layout(rest, font);
	QTextOption option;
	option.setWrapMode(mode);
	layout.setTextOption(option);
	layout.beginLayout();
	int len = static_cast<int>(rest.size());
	QTextLine line = layout.createLine();
	if (line.isValid()) {
		line.setLineWidth(width);
		len = line.textLength();
	}
	layout.endLayout();
	if (len <= 0) {
		QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, rest);
		const qsizetype next = finder.toNextBoundary();
		len = next > 0 ? static_cast<int>(next) : 1;
	}
	return len;
}

std::u16string to_u16(const QString &s)
{
	return std::u16string(reinterpret_cast<const char16_t *>(s.utf16()), static_cast<std::size_t>(s.size()));
}

constexpr QChar kLineSeparator = QChar(QChar::LineSeparator);
constexpr QChar kParagraphSeparator = QChar(QChar::ParagraphSeparator);

// 枠の幅を測る四角の一辺 (frame_margins)。枠の幅は四角の大きさによらないので、枠より十分に大きければよい。
constexpr int kFrameProbeSide = 1000;

} // namespace

bool last_line_fits(const QString &text, const QFont &font, int width, int min_graphemes, double min_ratio)
{
	if (width <= 0 || text.isEmpty())
		return true;
	return fits(last_line_of(text, font, width), width, min_graphemes, min_ratio);
}

int fitting_wrap_width(const QString &text, const QFont &font, int width, int min_graphemes, double min_ratio,
		       double floor_ratio)
{
	if (width <= 0 || text.isEmpty())
		return width;
	const int floor_w = std::max(1, static_cast<int>(std::ceil(width * std::clamp(floor_ratio, 0.0, 1.0))));
	int count_only = -1;
	for (int w = width; w >= floor_w; --w) {
		const LastLine l = last_line_of(text, font, w);
		if (fits(l, w, min_graphemes, min_ratio))
			return w;
		if (count_only < 0 && fits(l, w, min_graphemes, 0.0))
			count_only = w;
	}
	return count_only >= 0 ? count_only : width;
}

namespace {

// テキストを幅 width で折り返す (mode は KeepNumbers か PathSeparators。spans は KeepNumbers で守る語)。
QStringList break_lines(const QString &text, const std::u16string &u16, const QFont &font, int width, WrapBreaks mode,
			const std::vector<calc::Span> &spans)
{
	// KeepNumbers は語の切れ目で折り返し、1 つの語が幅に入らないときだけ語の中で折り返す。Qt の WordWrap は入らない
	// 語を右へはみ出させ、ラベルの最小の幅が長い語の幅になって、ドックの最小の幅を広げるため。
	const QTextOption::WrapMode qt_mode = mode == WrapBreaks::PathSeparators ? QTextOption::WrapAnywhere
									      : QTextOption::WrapAtWordBoundaryOrAnywhere;
	const int n = static_cast<int>(text.size());
	QStringList lines;
	int pos = 0;
	while (pos < n) {
		const int len = fit_length(text, pos, font, width, qt_mode);
		if (pos + len >= n) {
			lines << text.mid(pos);
			break;
		}
		int brk = pos + len;
		if (mode == WrapBreaks::KeepNumbers)
			brk = calc::keep_numbers_break(spans, pos, brk);
		else
			brk = calc::path_break(u16, pos, brk);
		lines << text.mid(pos, brk - pos);
		pos = brk;
	}
	return lines;
}

} // namespace

QStringList wrap_lines(const QString &text, const QFont &font, int width, WrapBreaks mode)
{
	if (mode == WrapBreaks::Qt || text.isEmpty() || width <= 0)
		return QStringList{text};
	const std::u16string u16 = to_u16(text);
	if (mode != WrapBreaks::KeepNumbers)
		return break_lines(text, u16, font, width, mode, {});
	// 数と単位の語 (「9分」「14 minutes」「13:41」) はいつも守る。数字を含む括弧の組 (「(2人)」) を 1 語として
	// 守るのは、括弧が 1 行に入り、守っても行が増えないときだけにする。1 行に入らない長い括弧 (「(録画できるのは
	// 残り約9分)」) を語として守ると、括弧ごと次の行へ送った後、行の先頭からの語として Qt の折り返しに戻り、
	// その間に行が 1 つ増える。
	// 括弧の組を「中が数の語と空白だけ」のものに限る方法は使わない。「(他2件)」「(+1 more)」のように、数の語の外の
	// 文字を含む短い括弧が守られなくなるため。幅と行の数で決めれば、文字列の形の一覧を持たずに済む。
	std::vector<calc::Span> words = calc::number_word_spans(u16);
	const QFontMetricsF metrics(font);
	// 角かっこで囲んだボタンの名前 (「[Vesktopに追加]」) も、1 行に入り、守っても行が増えないときは守る (ボタンの名前が行で
	// 分かれると、画面のボタンと照らし合わせにくい)。行が増えるときは守らない。カードの高さを変えないためで、数字を含む括弧の組と
	// 同じ扱いである。1 行に入らない名前は範囲にしない (守ると、名前を次の行へ送った後も入らず、行の先頭から語の中で折り返す)。
	std::vector<calc::Span> with_buttons = words;
	for (const calc::Span &s : calc::button_name_spans(u16)) {
		if (metrics.horizontalAdvance(text.mid(s.start, s.end - s.start)) <= width)
			with_buttons.push_back(s);
	}
	if (with_buttons.size() != words.size() &&
	    break_lines(text, u16, font, width, mode, with_buttons).size() <=
		    break_lines(text, u16, font, width, mode, words).size())
		words = std::move(with_buttons);
	std::vector<calc::Span> with_brackets = words;
	for (const calc::Span &s : calc::number_bracket_spans(u16)) {
		if (metrics.horizontalAdvance(text.mid(s.start, s.end - s.start)) <= width)
			with_brackets.push_back(s);
	}
	const QStringList kept = break_lines(text, u16, font, width, mode, with_brackets);
	if (with_brackets.size() == words.size())
		return kept;
	const QStringList words_only = break_lines(text, u16, font, width, mode, words);
	return kept.size() > words_only.size() ? words_only : kept;
}

WrapLabel::WrapLabel(const QString &text, QWidget *parent) : QLabel(text, parent), source_(text)
{
	setTextFormat(Qt::PlainText);
	setWordWrap(true);
	// 箇条書きの記号を 1 行目にそろえるため、テキストは上に寄せる (ラベルがテキストより高くなっても、記号と
	// テキストがずれない)。
	setAlignment(Qt::AlignLeft | Qt::AlignTop);
	// Qt の自動の字下げを使わない。QLabel は、字下げが負 (既定) で枠の幅が 0 でないとき (テーマがラベルに余白を
	// 付けると、Yami の .frame-notice QLabel { padding: ... } のように枠の幅になる)、サイズの計算では「x」の幅を、
	// 描くときはその半分を、配置の向きの辺 (左と、上寄せなら上) から空ける (Qt の qlabel.cpp の sizeForWidth と
	// paintEvent)。上寄せのラベルでは計算と描画が食い違い、ラベルがテキストより高くなり、2 行のテキストに 3 行分の
	// 高さを取ることもある。字下げを 0 にすると、テキストはテーマの余白の内側 (contentsRect) に置かれ、計算と描画が
	// 一致する。
	setIndent(0);
}

void WrapLabel::set_text(const QString &text)
{
	source_ = text;
	if (managed()) {
		setProperty("sourceText", source_);
		setAccessibleName(source_);
		refresh_breaks();
	} else {
		setText(text);
	}
	update_margins();
	updateGeometry();
}

QString WrapLabel::source_text() const
{
	return managed() ? source_ : text();
}

void WrapLabel::set_last_line_rule(int min_graphemes, double min_ratio, double floor_ratio)
{
	min_graphemes_ = min_graphemes;
	min_ratio_ = min_ratio;
	floor_ratio_ = floor_ratio;
	cache_w_ = -1;
	lines_cache_w_ = -1;
	update_margins();
	refresh_breaks();
	updateGeometry();
}

void WrapLabel::set_last_line_rule(const DockStyle &style)
{
	set_last_line_rule(style.min_last_line_graphemes, style.min_last_line_ratio, style.wrap_narrow_floor_ratio);
}

void WrapLabel::set_auto_narrow(bool on)
{
	auto_narrow_ = on;
	update_margins();
	updateGeometry();
}

void WrapLabel::set_breaks(WrapBreaks mode)
{
	if (mode == breaks_)
		return;
	if (!managed())
		source_ = text();
	breaks_ = mode;
	lines_cache_w_ = -1;
	if (managed()) {
		setProperty("sourceText", source_);
		setAccessibleName(source_);
		refresh_breaks();
	} else {
		setProperty("sourceText", QVariant());
		setText(source_);
	}
	update_margins();
	updateGeometry();
}

void WrapLabel::set_bullet(const QString &mark, int gap_px)
{
	mark_ = mark;
	gap_px_ = std::max(0, gap_px);
	update_margins();
	refresh_breaks();
	updateGeometry();
	update();
}

int WrapLabel::wrap_width_for(int w) const
{
	const int avail = std::max(1, w - frame_.left() - frame_.right() - indent_);
	if (!auto_narrow_ || managed())
		return avail;
	const QString font_key = font().key();
	const int inset = indent_ + frame_.left() + frame_.right();
	if (w == cache_w_ && inset == cache_indent_ && text() == cache_text_ && font_key == cache_font_)
		return cache_result_;
	cache_result_ = fitting_wrap_width(text(), font(), avail, min_graphemes_, min_ratio_, floor_ratio_);
	cache_w_ = w;
	cache_indent_ = inset;
	cache_text_ = text();
	cache_font_ = font_key;
	return cache_result_;
}

QStringList WrapLabel::lines_for(int w) const
{
	if (!managed())
		return QStringList{text()};
	const QString font_key = font().key();
	const int inset = indent_ + frame_.left() + frame_.right();
	if (w == lines_cache_w_ && inset == lines_cache_indent_ && source_ == lines_cache_text_ &&
	    font_key == lines_cache_font_)
		return lines_cache_;
	const int avail = std::max(1, w - inset);
	const QFont f = font();
	// 最後の行が規則に合うか (KeepNumbers だけ。場所の行には適用しない)。
	const auto last_ok = [&](const QStringList &lines, int width, double ratio) {
		if (lines.size() < 2)
			return true;
		const QString last = lines.back().trimmed();
		if (grapheme_count(last) < min_graphemes_)
			return false;
		return ratio <= 0.0 || QFontMetricsF(f).horizontalAdvance(last) >= ratio * width;
	};
	QStringList out;
	// 段落 (元のテキストの改行) ごとに折り返す。段落の境は改行のまま残す (行の区切りと分け、つなぐと元のテキストに
	// 戻るようにする)。
	const QStringList paragraphs = source_.split(QLatin1Char('\n'));
	for (qsizetype p = 0; p < paragraphs.size(); ++p) {
		const QString &para = paragraphs[p];
		QStringList lines = wrap_lines(para, f, avail, breaks_);
		if (breaks_ == WrapBreaks::KeepNumbers && !last_ok(lines, avail, min_ratio_)) {
			// 行の数を増やさない範囲で、折り返す幅を狭めて最後の行の長さをそろえる。両方の規則に合う幅が
			// なければ、文字数だけに合う幅にする。それもなければ、現在の折り返しのままにする (カードの高さを
			// 変えないため)。
			const int floor_w =
				std::max(1, static_cast<int>(std::ceil(avail * std::clamp(floor_ratio_, 0.0, 1.0))));
			QStringList both;
			QStringList count_only;
			for (int ww = avail - 1; ww >= floor_w && both.isEmpty(); --ww) {
				const QStringList narrower = wrap_lines(para, f, ww, breaks_);
				if (narrower.size() != lines.size())
					continue;
				if (last_ok(narrower, ww, min_ratio_))
					both = narrower;
				else if (count_only.isEmpty() && last_ok(narrower, ww, 0.0))
					count_only = narrower;
			}
			if (!both.isEmpty())
				lines = both;
			else if (!count_only.isEmpty() && !last_ok(lines, avail, 0.0))
				lines = count_only;
		}
		if (p + 1 < paragraphs.size() && !lines.isEmpty())
			lines.back() += QLatin1Char('\n');
		out << lines;
	}
	lines_cache_w_ = w;
	lines_cache_indent_ = inset;
	lines_cache_text_ = source_;
	lines_cache_font_ = font_key;
	lines_cache_ = out;
	return out;
}

QMargins WrapLabel::frame_margins() const
{
	// QFrame が枠の幅を決めるのと同じ方法 (Qt の qframe.cpp の updateStyledFrameWidths) で測る。ただし、測る四角は
	// ラベルの大きさと現在の余白によらない四角にする。Qt が測る frameRect() は現在の contentsMargins から求まり、この
	// ラベルは contentsMargins に字下げと右の余白を加えている。frameRect() で測ると、高さが 0 のときに、余白を変える
	// たびに測った枠の幅が 0 とテーマの余白とで入れ替わる。表示しているラベルの余白を変えると、Qt はその場で
	// resizeEvent を送るので、resizeEvent の update_margins が終わらず、スタックオーバーフローで落ちる。
	QStyleOptionFrame opt;
	initStyleOption(&opt);
	opt.rect = QRect(0, 0, kFrameProbeSide, kFrameProbeSide);
	const QRect cr = style()->subElementRect(QStyle::SE_ShapedFrameContents, &opt, this);
	return QMargins(std::max(0, cr.left() - opt.rect.left()), std::max(0, cr.top() - opt.rect.top()),
			std::max(0, opt.rect.right() - cr.right()), std::max(0, opt.rect.bottom() - cr.bottom()));
}

QSize WrapLabel::sizeHint() const
{
	QSize s = QLabel::sizeHint();
	s.rwidth() -= contentsMargins().right() - frame_.right();
	return s;
}

QSize WrapLabel::minimumSizeHint() const
{
	if (managed()) {
		// 折り返す位置をラベルが決める形は、どこでも折り返せる (パスは区切りがなければ文字の間、文は入らない
		// 語だけ語の中) ので、いちばん広い 1 文字の幅まで狭められる。表示しているテキスト (現在の幅の折り返し
		// 入り) から QLabel が測ると、現在の幅のいちばん長い行か長い語が最小の幅になり、ドックやダイアログを
		// 狭め直せない。
		const QFontMetrics fm = fontMetrics();
		int widest = 1;
		QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, source_);
		qsizetype at = 0;
		for (qsizetype next = finder.toNextBoundary(); next > 0; next = finder.toNextBoundary()) {
			widest = std::max(widest, fm.horizontalAdvance(source_.mid(at, next - at)));
			at = next;
		}
		const QMargins m = contentsMargins();
		return QSize(widest + m.left() + m.right() + 2 * margin(), fm.height() + m.top() + m.bottom() + 2 * margin());
	}
	QSize s = QLabel::minimumSizeHint();
	s.rwidth() -= contentsMargins().right() - frame_.right();
	return s;
}

int WrapLabel::heightForWidth(int w) const
{
	// レイアウトは、ラベルに最大の幅があっても、最大の幅で切らない幅のまま高さを問い合わせる
	// (QWidgetItem::heightForWidth)。ラベルは最大の幅で配置されるので、配置される幅で測る (広い所で 1 行の高さの
	// まま 2 行目が切れないように)。
	w = std::min(w, maximumWidth());
	const QMargins m = contentsMargins();
	if (managed()) {
		// ラベルが決めた折り返し入りのテキスト。今表示しているテキストと同じなら、QLabel に測らせる (選択
		// できるラベルの文書の高さと同じ処理)。違う幅を問い合わせられたら、その幅の折り返し入りのテキストを、
		// QLabel がプレーンテキストを測るのと同じ方法 (QFontMetrics::boundingRect) で測る (配置された後に
		// resizeEvent がテキストを替え、高さを問い合わせ直される)。
		const QStringList lines = lines_for(w);
		QString shown = lines.join(kLineSeparator);
		shown.replace(QStringLiteral("\n") + kLineSeparator, QStringLiteral("\n"));
		if (shown == text())
			return QLabel::heightForWidth(w);
		const int avail = std::max(1, w - m.left() - m.right() - 2 * margin());
		const QRect br = fontMetrics().boundingRect(0, 0, avail, 1000000,
							    Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, shown);
		return br.height() + m.top() + m.bottom() + 2 * margin();
	}
	// QLabel は、現在の左右の余白を引いた幅で折り返す。w のときに選ぶ折り返しの幅になるよう、渡す幅を合わせる。
	return QLabel::heightForWidth(wrap_width_for(w) + m.left() + m.right());
}

void WrapLabel::refresh_breaks()
{
	if (!managed())
		return;
	QString shown = lines_for(width()).join(kLineSeparator);
	// 段落の境 (改行) の後ろに、行の区切りを重ねない。
	shown.replace(QStringLiteral("\n") + kLineSeparator, QStringLiteral("\n"));
	if (shown != text())
		setText(shown);
}

void WrapLabel::update_margins()
{
	indent_ = mark_.isEmpty() ? 0 : fontMetrics().horizontalAdvance(mark_) + gap_px_;
	frame_ = frame_margins();
	const int right = auto_narrow_ && !managed()
				  ? std::max(0, (width() - frame_.left() - frame_.right() - indent_) - wrap_width_for(width()))
				  : 0;
	// QFrame は枠 (テーマの余白と枠線) を内側の余白 (contentsMargins) として持つ (Qt の qframe.cpp の
	// setFrameRect)。箇条書きの字下げと、狭めるための右の余白は、枠の上に加える (枠を消すと、テーマの余白がなくなる
	// ため)。
	const QMargins want(frame_.left() + indent_, frame_.top(), frame_.right() + right, frame_.bottom());
	if (contentsMargins() != want)
		setContentsMargins(want);
}

void WrapLabel::resizeEvent(QResizeEvent *e)
{
	update_margins();
	refresh_breaks();
	QLabel::resizeEvent(e);
}

void WrapLabel::changeEvent(QEvent *e)
{
	QLabel::changeEvent(e);
	// フォントの大きさが変わった (テーマ、フォントサイズ) ので、記号の幅と、狭める幅と、折り返す位置を測り直す。
	if (e->type() == QEvent::FontChange || e->type() == QEvent::StyleChange) {
		update_margins();
		refresh_breaks();
		updateGeometry();
	}
}

void WrapLabel::copy_selection() const
{
	// 折り返す位置に入れた行の区切りを除いてコピーする (元のテキストに U+2028 はない。選択が段落をまたげば改行に
	// する)。
	QString s = selectedText();
	s.remove(kLineSeparator);
	s.replace(kParagraphSeparator, QLatin1Char('\n'));
	if (QClipboard *cb = QGuiApplication::clipboard())
		cb->setText(s);
}

void WrapLabel::keyPressEvent(QKeyEvent *e)
{
	if (managed() && e->matches(QKeySequence::Copy) && hasSelectedText()) {
		copy_selection();
		e->accept();
		return;
	}
	QLabel::keyPressEvent(e);
}

void WrapLabel::contextMenuEvent(QContextMenuEvent *e)
{
	QLabel::contextMenuEvent(e);
	if (!managed())
		return;
	// QLabel の標準のメニュー (Qt のテキストのまま) の [コピー] を、行の区切りを除いてコピーする処理に差し替える。
	// メニューはラベルの子として作られ、popup で表示した後に戻る (Qt の qlabel.cpp の contextMenuEvent)。
	// [コピー] には Qt が objectName "edit-copy" を付ける (Qt の qwidgettextcontrol.cpp の
	// createStandardContextMenu)。
	for (QMenu *menu : findChildren<QMenu *>(Qt::FindDirectChildrenOnly)) {
		for (QAction *action : menu->actions()) {
			if (action->objectName() != QStringLiteral("edit-copy") ||
			    action->property("cordmuxCopyStripsBreaks").toBool())
				continue;
			QObject::disconnect(action, &QAction::triggered, nullptr, nullptr);
			connect(action, &QAction::triggered, this, [this]() { copy_selection(); });
			action->setProperty("cordmuxCopyStripsBreaks", true);
		}
	}
}

void WrapLabel::mousePressEvent(QMouseEvent *e)
{
	// 選択した所を押してドラッグすると、選択したテキスト (行の区切り入り) をほかへドロップできる (Qt のテキストの
	// ウィジェットの既定)。ドロップした先でパスに改行が入るので、選択した所を左ボタンで押したら、選択をやめる。
	if (managed() && e->button() == Qt::LeftButton && hasSelectedText())
		setSelection(0, 0);
	QLabel::mousePressEvent(e);
}

void WrapLabel::paintEvent(QPaintEvent *e)
{
	QLabel::paintEvent(e);
	if (mark_.isEmpty())
		return;
	// 記号はテキストと同じフォントと色で、1 行目の高さに描く (ラベルのテキストと同じ処理なので、テーマの色と、
	// setEnabled(false) のときの見た目に従う)。
	QPainter p(this);
	const QRect cr = contentsRect();
	const QRect mark_rect(cr.left() - indent_, cr.top(), std::max(0, indent_ - gap_px_), fontMetrics().height());
	style()->drawItemText(&p, mark_rect, Qt::AlignLeft | Qt::AlignTop, palette(), isEnabled(), mark_,
			      foregroundRole());
}

} // namespace cordmux::dock::ui
