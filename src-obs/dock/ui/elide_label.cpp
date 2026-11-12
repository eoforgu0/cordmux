// SPDX-License-Identifier: Apache-2.0

// elide_label.h の実装。

#include "dock/ui/elide_label.h"

#include <QEvent>
#include <QFontMetrics>
#include <QResizeEvent>
#include <QSizePolicy>

#include <algorithm>
#include <string>
#include <vector>

#include "dock/ui/dock_ui_calc.h"

namespace cordmux::dock::ui {

namespace {

constexpr char16_t kEllipsis = 0x2026;

std::u16string to_u16(const QString &s)
{
	return std::u16string(reinterpret_cast<const char16_t *>(s.utf16()), static_cast<std::size_t>(s.size()));
}

// 省いたときのツールチップ。全文を 1 行目に置き、使う側のツールチップの行を続ける。使う側のツールチップの 1 行目が
// 全文と同じなら、同じテキストを 2 回並べないように、そのまま使う (参加者の行の状態のテキストは、ツールチップに
// 状態のテキストと「枠に何が映っているか」の 2 行を持つ)。
// 省いたときにツールチップを全文だけにすると、省いていないときに表示する説明の 1 文が、省いたときだけ消える。
// 1 行目が全文で始まるかどうかで比べる方法は使わない。全文の後に続きがある別のテキストを、同じものと取り違えるため。
QString elided_tool_tip(const QString &full, const QString &base)
{
	if (base.isEmpty())
		return full;
	const qsizetype newline = base.indexOf(QLatin1Char('\n'));
	const QString first = newline < 0 ? base : base.left(newline);
	if (first == full)
		return base;
	return full + QLatin1Char('\n') + base;
}

} // namespace

QString elide_keep_numbers(const QFontMetrics &fm, const QString &text, int width)
{
	const QString elided = fm.elidedText(text, Qt::ElideRight, width > 0 ? width : 0);
	if (elided == text || !elided.endsWith(QChar(kEllipsis)))
		return elided;
	const int cut = static_cast<int>(elided.size()) - 1;
	const int safe = calc::safe_cut(to_u16(text), cut);
	if (safe <= 0)
		return elided;
	return text.left(safe) + QChar(kEllipsis);
}

ElideLabel::ElideLabel(QWidget *parent) : QLabel(parent)
{
	setTextFormat(Qt::PlainText);
	setWordWrap(false);
	setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
}

QString ElideLabel::full_text() const
{
	return use_forms_ ? (forms_.isEmpty() ? QString() : forms_.front()) : full_;
}

void ElideLabel::set_full_text(const QString &text)
{
	if (!use_forms_ && full_ == text)
		return;
	use_forms_ = false;
	forms_.clear();
	full_ = text;
	updateGeometry();
	refit();
}

void ElideLabel::set_forms(const QStringList &forms)
{
	if (use_forms_ && forms_ == forms)
		return;
	use_forms_ = true;
	forms_ = forms;
	full_ = forms.isEmpty() ? QString() : forms.front();
	updateGeometry();
	refit();
}

void ElideLabel::set_elide_mode(Qt::TextElideMode mode)
{
	if (mode_ == mode)
		return;
	mode_ = mode;
	refit();
}

void ElideLabel::set_keep_numbers(bool keep)
{
	if (keep_numbers_ == keep)
		return;
	keep_numbers_ = keep;
	refit();
}

void ElideLabel::set_base_tool_tip(const QString &tip)
{
	base_tip_ = tip;
	refit();
}

void ElideLabel::set_base_accessible_name(const QString &name)
{
	base_name_ = name;
	refit();
}

void ElideLabel::set_own_tool_tip(bool on)
{
	if (own_tip_ == on)
		return;
	own_tip_ = on;
	refit();
}

void ElideLabel::set_preferred_width(int px)
{
	if (preferred_w_ == px)
		return;
	preferred_w_ = px;
	updateGeometry();
}

QSize ElideLabel::sizeHint() const
{
	// 省かない全文の幅 (set_preferred_width で指定していれば、その幅)。サイズポリシーが Ignored なら最小の幅にはならず、
	// 広いときに取りたい幅として使われる。
	const QSize base = QLabel::sizeHint();
	if (preferred_w_ >= 0)
		return QSize(preferred_w_, base.height());
	const QMargins m = contentsMargins();
	const int w = fontMetrics().horizontalAdvance(full_text()) + m.left() + m.right() + 2 * margin();
	return QSize(w, base.height());
}

QSize ElideLabel::minimumSizeHint() const
{
	// 横は省略記号だけの幅、縦は 1 行の高さ。
	const QSize base = QLabel::minimumSizeHint();
	const QMargins m = contentsMargins();
	const int w = fontMetrics().horizontalAdvance(QString(QChar(kEllipsis))) + m.left() + m.right() + 2 * margin();
	return QSize(w, base.height());
}

int ElideLabel::available_width() const
{
	return contentsRect().width() - 2 * margin();
}

QString ElideLabel::elide_one(const QString &text, int width) const
{
	if (mode_ == Qt::ElideRight && keep_numbers_)
		return elide_keep_numbers(fontMetrics(), text, width);
	return fontMetrics().elidedText(text, mode_, width > 0 ? width : 0);
}

void ElideLabel::refit()
{
	if (fitting_)
		return;
	fitting_ = true;
	const QFontMetrics fm = fontMetrics();
	const int avail = available_width();
	QString shown;
	form_index_ = 0;
	if (use_forms_ && !forms_.isEmpty()) {
		std::vector<int> widths;
		widths.reserve(static_cast<std::size_t>(forms_.size()));
		for (const QString &f : forms_)
			widths.push_back(fm.horizontalAdvance(f));
		const calc::FormFit fit = calc::fit_forms(widths, avail);
		form_index_ = fit.index;
		// どれも入らないときは、守る語の途中で切らずに省ける候補を後ろから選ぶ (calc::elide_form_index)。
		// 数の語を守らないラベル (set_keep_numbers(false)) と、末尾以外を省くラベルは、最後の候補を省く。
		if (fit.elide && mode_ == Qt::ElideRight && keep_numbers_ && forms_.size() > 1) {
			std::vector<std::u16string> texts;
			std::vector<int> cuts;
			texts.reserve(static_cast<std::size_t>(forms_.size()));
			cuts.reserve(static_cast<std::size_t>(forms_.size()));
			for (const QString &f : forms_) {
				const QString e = fm.elidedText(f, Qt::ElideRight, avail > 0 ? avail : 0);
				const bool cut_here = e != f && e.endsWith(QChar(kEllipsis));
				cuts.push_back(cut_here ? static_cast<int>(e.size()) - 1 : (e == f ? static_cast<int>(f.size()) : 0));
				texts.push_back(to_u16(f));
			}
			form_index_ = calc::elide_form_index(texts, cuts);
		}
		const QString &form = forms_.at(form_index_);
		shown = fit.elide ? elide_one(form, avail) : form;
	} else {
		shown = fm.horizontalAdvance(full_) <= avail ? full_ : elide_one(full_, avail);
	}
	elided_ = shown != full_text();
	if (text() != shown)
		setText(shown);
	const QString tip = !own_tip_ ? QString() : (elided_ ? elided_tool_tip(full_text(), base_tip_) : base_tip_);
	if (toolTip() != tip)
		setToolTip(tip);
	const QString name = elided_ ? full_text() : base_name_;
	if (accessibleName() != name)
		setAccessibleName(name);
	fitting_ = false;
}

void ElideLabel::resizeEvent(QResizeEvent *event)
{
	QLabel::resizeEvent(event);
	if (event->size().width() != event->oldSize().width())
		refit();
}

void ElideLabel::changeEvent(QEvent *event)
{
	QLabel::changeEvent(event);
	switch (event->type()) {
	case QEvent::FontChange:
	case QEvent::StyleChange:
		updateGeometry();
		refit();
		break;
	default:
		break;
	}
}

CappedWrapLabel::CappedWrapLabel(QWidget *parent) : QLabel(parent)
{
	setTextFormat(Qt::PlainText);
	setWordWrap(true);
	setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
}

int CappedWrapLabel::heightForWidth(int width) const
{
	return QLabel::heightForWidth(std::min(width, maximumWidth()));
}

QLabel *make_wrap_label(const QString &text, QWidget *parent)
{
	auto *label = new CappedWrapLabel(parent);
	label->setText(text);
	return label;
}

} // namespace cordmux::dock::ui
