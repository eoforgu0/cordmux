// SPDX-License-Identifier: Apache-2.0

// measure.h の実装。

#include "measure.h"

#include <QAbstractButton>
#include <QAction>
#include <QCheckBox>
#include <QFontMetrics>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QStyleOptionButton>
#include <QTextBoundaryFinder>

#include <algorithm>
#include <cmath>
#include <map>

#include "dock/dock_widget.h"
#include "dock/first_line.h"
#include "dock/ui/elide_label.h"
#include "dock/ui/focus_ring.h"
#include "dock/ui/record_button_base.h"
#include "dock/ui/text_resolve.h"
#include "obs_dock.h"
#include "render.h"

namespace cordmux::ui_preview {

namespace {

// 祖先の objectName を root までたどる (押せるものの種類を決めるため)。
bool has_ancestor_named(QWidget *w, QWidget *root, const QString &name)
{
	for (QWidget *p = w->parentWidget(); p != nullptr; p = p->parentWidget()) {
		if (p->objectName() == name)
			return true;
		if (p == root)
			break;
	}
	return false;
}

QString kind_of(QWidget *w, QWidget *root)
{
	const QString on = w->objectName();
	if (on == QLatin1String("cordmuxRowCheck"))
		return QStringLiteral("row-check");
	if (on == QLatin1String("cordmuxRowMenuButton"))
		return QStringLiteral("row-menu");
	if (on == QLatin1String("cordmuxRecordButton"))
		return QStringLiteral("rec");
	if (on == QLatin1String("cordmuxDockMenuButton"))
		return QStringLiteral("dock-menu");
	if (on == QLatin1String("cordmuxEmptyButton"))
		return QStringLiteral("empty-button");
	if (has_ancestor_named(w, root, QStringLiteral("cordmuxFirstLine")))
		return QStringLiteral("first-line-button");
	if (has_ancestor_named(w, root, QStringLiteral("cordmuxNoticeCard")))
		return QStringLiteral("card-button");
	return QStringLiteral("other:") + QString::fromLatin1(w->metaObject()->className());
}

// 押せるものの名前 (テキストか、テキストを持たなければアクセシブル名)。行の中のものは、人の userId も付ける。
QString label_of(QAbstractButton *b)
{
	QString label = b->text();
	if (label.isEmpty())
		label = b->accessibleName();
	return label;
}

double channel(double c)
{
	c /= 255.0;
	return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double luminance(const QColor &c)
{
	return 0.2126 * channel(c.red()) + 0.7152 * channel(c.green()) + 0.0722 * channel(c.blue());
}

double dist_to_rect(const QPointF &c, const QRectF &r)
{
	const double dx = std::max({r.left() - c.x(), 0.0, c.x() - r.right()});
	const double dy = std::max({r.top() - c.y(), 0.0, c.y() - r.bottom()});
	return std::sqrt(dx * dx + dy * dy);
}

QRectF rectf(const QRect &r)
{
	return QRectF(r.x(), r.y(), r.width(), r.height());
}

QString rect_text(const QRect &r)
{
	return QStringLiteral("%1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
}

QString yn(bool b)
{
	return b ? QStringLiteral("yes") : QStringLiteral("NO");
}

} // namespace

QString tsv_cell(QString text)
{
	text.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
	text.replace(QLatin1Char('\t'), QStringLiteral("\\t"));
	text.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
	text.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
	return text;
}

double wcag_contrast(const QColor &a, const QColor &b)
{
	const double la = luminance(a);
	const double lb = luminance(b);
	return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

QColor dominant_color(const QImage &image, const QRect &rect)
{
	// rect はウィジェットの座標 (論理 px)。拡大率のある画像 (--dpr) では、ピクセルに修正して読む。
	const qreal dpr = image.devicePixelRatio();
	const QRect px(qRound(rect.x() * dpr), qRound(rect.y() * dpr), qRound(rect.width() * dpr),
		       qRound(rect.height() * dpr));
	const QRect clip = px.intersected(image.rect());
	std::map<QRgb, int> counts;
	for (int y = clip.top(); y <= clip.bottom(); ++y) {
		for (int x = clip.left(); x <= clip.right(); ++x)
			++counts[image.pixel(x, y) & 0x00FFFFFF];
	}
	QRgb best = 0;
	int best_n = -1;
	for (const auto &[rgb, n] : counts) {
		if (n > best_n) {
			best = rgb;
			best_n = n;
		}
	}
	return QColor(best);
}

int grapheme_count(const QString &text)
{
	QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
	int n = 0;
	while (finder.toNextBoundary() != -1)
		++n;
	return n;
}

QRect visible_rect_in(QWidget *w, QWidget *root)
{
	QRect r(w->mapTo(root, QPoint(0, 0)), w->size());
	for (QWidget *p = w->parentWidget(); p != nullptr; p = p->parentWidget()) {
		r &= QRect(p->mapTo(root, QPoint(0, 0)), p->size());
		if (p == root)
			break;
	}
	return r;
}

std::vector<Wcag258> judge_258(const std::vector<TargetFact> &targets)
{
	// 隠れたものは、交わる相手にも数えない (見えていないものは押せない)。
	std::vector<Wcag258> out(targets.size());
	for (std::size_t i = 0; i < targets.size(); ++i) {
		const QRect &r = targets[i].rect;
		out[i].size_ok = r.width() >= 24 && r.height() >= 24;
		if (out[i].size_ok) {
			out[i].spacing_ok = true;
			continue;
		}
		const QPointF c = rectf(r).center();
		bool ok = true;
		for (std::size_t j = 0; j < targets.size() && ok; ++j) {
			if (j == i || !targets[j].visible)
				continue;
			const QRect &o = targets[j].rect;
			if (dist_to_rect(c, rectf(o)) < 12.0) {
				ok = false;
				out[i].conflict = targets[j].kind + QStringLiteral(":") + targets[j].label;
				break;
			}
			if (o.width() < 24 || o.height() < 24) {
				const QPointF oc = rectf(o).center();
				if (std::hypot(c.x() - oc.x(), c.y() - oc.y()) < 24.0) {
					ok = false;
					out[i].conflict = QStringLiteral("circle: ") + targets[j].kind + QStringLiteral(":") +
							  targets[j].label;
				}
			}
		}
		out[i].spacing_ok = ok;
	}
	return out;
}

std::vector<TargetFact> collect_targets(QWidget *root, const QPoint &origin)
{
	std::vector<TargetFact> out;
	for (QAbstractButton *b : root->findChildren<QAbstractButton *>()) {
		if (!b->isVisibleTo(root) || b->window() != root->window())
			continue;
		TargetFact t;
		t.kind = kind_of(b, root);
		t.label = label_of(b);
		const QString uid = b->parentWidget() != nullptr ? b->parentWidget()->property("userId").toString() : QString();
		if (!uid.isEmpty())
			t.label += QStringLiteral(" [") + uid.right(2) + QStringLiteral("]");
		const QRect hit = dock::ui::focus_hit_rect_of(b);
		t.rect = QRect(b->mapTo(root, hit.topLeft()) + origin, hit.size());
		t.qt_rect = t.rect;
		if (auto *check = qobject_cast<QCheckBox *>(b)) {
			QStyleOptionButton option;
			option.initFrom(check);
			option.text = check->text();
			const QRect q = check->style()->subElementRect(QStyle::SE_CheckBoxClickRect, &option, check);
			if (q.isValid())
				t.qt_rect = QRect(b->mapTo(root, q.topLeft()) + origin, q.size());
		}
		const QRect shown = visible_rect_in(b, root).translated(origin);
		t.visible = shown.contains(t.rect.intersected(QRect(b->mapTo(root, QPoint(0, 0)) + origin, b->size())));
		t.enabled = b->isEnabled();
		out.push_back(t);
	}
	return out;
}

std::vector<TargetFact> menu_targets(QMenu *menu, const QPoint &origin)
{
	std::vector<TargetFact> out;
	for (QAction *a : menu->actions()) {
		if (a->isSeparator() || !a->isVisible())
			continue;
		TargetFact t;
		t.kind = QStringLiteral("menu-item");
		t.label = a->text();
		t.rect = menu->actionGeometry(a).translated(origin);
		t.qt_rect = t.rect;
		t.enabled = a->isEnabled();
		out.push_back(t);
	}
	return out;
}

std::vector<ContrastFact> measure_contrast(QWidget *root, const QImage &image, const QPoint &origin)
{
	std::vector<ContrastFact> out;
	QList<QWidget *> all{root};
	all += root->findChildren<QWidget *>();
	for (QWidget *w : all) {
		if (!w->isVisibleTo(root) || w->window() != root->window())
			continue;
		QString text;
		QPalette::ColorRole role = QPalette::WindowText;
		if (auto *label = qobject_cast<QLabel *>(w)) {
			text = label->text();
		} else if (auto *button = qobject_cast<QPushButton *>(w)) {
			text = button->text();
			role = QPalette::ButtonText;
		} else if (auto *check = qobject_cast<QCheckBox *>(w)) {
			text = check->text();
		}
		if (text.isEmpty())
			continue;
		// 見えている部分が半分に満たないウィジェットは数えない (スクロールで半分隠れた行が、隣の色を
		// 拾わないように)。
		const QRect full(w->mapTo(root, QPoint(0, 0)), w->size());
		const QRect shown = visible_rect_in(w, root);
		if (shown.width() * shown.height() * 2 < full.width() * full.height())
			continue;
		// 製品が、押せないが押せる見た目で描画するウィジェット (停止の処理中の録画のボタン) は、描画に使った
		// カラーグループで読む (dock/ui/record_button_base.cpp の paintEvent が Active で描画する条件と同じ)。
		bool painted_enabled = w->isEnabled();
		if (auto *rb = dynamic_cast<dock::ui::RecordButtonBase *>(w))
			painted_enabled = painted_enabled || (rb->pressed_look_while_disabled() && rb->isChecked());
		ContrastFact f;
		f.object_name = w->objectName();
		f.cls = QString::fromLatin1(w->metaObject()->className());
		f.text = text.left(30);
		f.fg = w->palette().color(painted_enabled ? QPalette::Active : QPalette::Disabled, role);
		f.bg = dominant_color(image, shown.translated(origin));
		f.ratio = wcag_contrast(f.fg, f.bg);
		f.enabled = w->isEnabled();
		{
			const qreal dpr = image.devicePixelRatio();
			const QRect r = shown.translated(origin);
			const QRect px = QRect(qRound(r.x() * dpr), qRound(r.y() * dpr), qRound(r.width() * dpr),
					       qRound(r.height() * dpr))
						 .intersected(image.rect());
			double best = 0;
			QColor best_c = f.bg;
			for (int y = px.top(); y <= px.bottom(); ++y) {
				for (int x = px.left(); x <= px.right(); ++x) {
					const QColor c(image.pixel(x, y));
					const double ratio = wcag_contrast(c, f.bg);
					if (ratio > best) {
						best = ratio;
						best_c = c;
					}
				}
			}
			f.drawn_fg = best_c;
			f.drawn_ratio = best;
		}
		out.push_back(f);
	}
	return out;
}

DockFacts measure_dock(DockHost &host, const QImage &image)
{
	DockFacts f;
	QWidget *frame = host.dock.get();
	dock::DockWidget *dw = host.widget;
	auto *first = dynamic_cast<dock::FirstLineBar *>(dw->findChild<QWidget *>(QStringLiteral("cordmuxFirstLine")));
	auto *scroll = dw->findChild<QScrollArea *>(QStringLiteral("cordmuxScroll"));
	auto *rec = dw->findChild<QPushButton *>(QStringLiteral("cordmuxRecordButton"));

	f.min_width = dw->computed_min_width();
	if (first != nullptr) {
		f.first_line_height = first->height();
		f.first_line_text = first->shown_text();
		f.first_line_full = first->full_text();
		f.first_line_elided = f.first_line_text != f.first_line_full;
		const QString head = f.first_line_full.section(QStringLiteral(" - "), 0, 0);
		f.first_line_has_segments = f.first_line_full.contains(QStringLiteral(" - "));
		f.first_line_first_segment = f.first_line_text.startsWith(head);
		// 残りの件数「(他N件)」は、1 行目のテキストとは別の QLabel (dock/first_line.cpp の more_。件数は
		// 省略しない)。見えているテキストの後ろに付けて書く。
		for (QLabel *l : first->findChildren<QLabel *>()) {
			if (auto *e = dynamic_cast<dock::ui::ElideLabel *>(l)) {
				f.first_line_shown_form = e->shown_form();
				continue;
			}
			if (!l->isVisibleTo(first) || l->text().isEmpty())
				continue;
			f.first_line_more = l->text();
		}
	}
	// 1 行目の View (描画した View。製品の判断を通したもの)。候補のテキストは、製品と同じ関数 (ui::resolve) で
	// 作る。
	{
		using ledger::dockview::FirstLineKind;
		using ledger::dockview::Tier;
		const ledger::dockview::FirstLine &line = host.view.first_line;
		switch (line.kind) {
		case FirstLineKind::Notice:
			f.first_line_kind = QStringLiteral("notice");
			break;
		case FirstLineKind::Summary:
			f.first_line_kind = QStringLiteral("summary");
			break;
		case FirstLineKind::UserChoice:
			f.first_line_kind = QStringLiteral("user-choice");
			break;
		case FirstLineKind::Working:
			f.first_line_kind = QStringLiteral("working");
			break;
		}
		if (line.tier.has_value())
			f.first_line_tier = *line.tier == Tier::L1   ? QStringLiteral("1")
					    : *line.tier == Tier::L2 ? QStringLiteral("2")
								     : QStringLiteral("3");
		f.first_line_notice_key = QString::fromStdString(line.notice_key);
		for (const ledger::TextRef &ref : line.forms)
			f.first_line_forms << dock::ui::resolve(ref);
	}
	if (scroll != nullptr) {
		f.v_scrollbar = scroll->verticalScrollBar()->isVisible();
		f.h_overflow = scroll->widget() != nullptr &&
			       scroll->widget()->minimumSizeHint().width() > scroll->viewport()->width();
	}
	if (rec != nullptr) {
		f.rec_button_width = rec->width();
		f.rec_button_text = rec->text();
		// 背景は、文字のない端の近くで読む (文字のピクセルを拾わないため)。ボタンの左端から 3 px 内側の、幅
		// 3 px の縦の範囲。
		const QRect r(rec->mapTo(frame, QPoint(0, 0)), rec->size());
		f.rec_button_bg = dominant_color(image, QRect(r.left() + 3, r.top() + 3, 3, std::max(1, r.height() - 6)));
	}
	for (QWidget *c : dw->findChildren<QWidget *>()) {
		const QString on = c->objectName();
		if (on == QLatin1String("cordmuxNoticeCard") && c->isVisibleTo(dw) && !c->property("l1Moved").toBool())
			++f.notices;
	}

	// m1、m2: スクロールの位置ごと (先頭・中ほど・最後)。
	auto fully_shown = [&](QWidget *w) {
		if (w == nullptr || !w->isVisibleTo(frame))
			return false;
		const QRect r(w->mapTo(frame, QPoint(0, 0)), w->size());
		return visible_rect_in(w, frame) == r && frame->rect().contains(r);
	};
	std::vector<QWidget *> rows;
	for (QWidget *c : dw->findChildren<QWidget *>(QStringLiteral("cordmuxRow"))) {
		if (c->isVisibleTo(dw))
			rows.push_back(c);
	}
	f.people = static_cast<int>(rows.size());
	for (int pos = 0; pos < 3; ++pos) {
		dw->set_scroll_position(pos);
		settle();
		f.first_line_visible[pos] = fully_shown(first);
		if (pos == 0) {
			int n = 0;
			for (QWidget *r : rows)
				n += fully_shown(r) ? 1 : 0;
			f.visible_people = n;
		}
	}
	dw->set_scroll_position(0);
	settle();

	// m4: 名前。
	for (QWidget *r : rows) {
		NameFact n;
		n.user_id = r->property("userId").toString();
		auto *name = dynamic_cast<dock::ui::ElideLabel *>(r->findChild<QLabel *>(QStringLiteral("cordmuxRowName")));
		if (name != nullptr) {
			n.full = name->full_text();
			n.shown = name->text();
		}
		for (QLabel *l2 : r->findChildren<QLabel *>(QStringLiteral("cordmuxRowLine2"))) {
			if (!l2->isVisibleTo(r))
				continue;
			n.line2_shown << l2->text();
			n.line2_full << (l2->toolTip().isEmpty() ? l2->text() : l2->toolTip());
		}
		n.full_graphemes = grapheme_count(n.full);
		n.elided = n.shown != n.full;
		QString without = n.shown;
		const int ell = without.indexOf(QChar(0x2026));
		without.remove(QChar(0x2026));
		n.shown_graphemes = n.elided ? grapheme_count(without) : n.full_graphemes;
		n.middle = n.elided && ell > 0 && ell < n.shown.size() - 1;
		QWidget *check = r->findChild<QWidget *>(QStringLiteral("cordmuxRowCheck"));
		QWidget *who = check != nullptr ? check : name;
		if (who != nullptr) {
			n.a11y_name = who->accessibleName();
			n.a11y_desc = who->accessibleDescription();
		}
		f.names.push_back(n);
	}

	// m5: はみ出した文字。省略の仕方を決めてある QLabel (ElideLabel) と、折り返す QLabel は形で見る。
	for (QLabel *label : dw->findChildren<QLabel *>()) {
		if (!label->isVisibleTo(dw) || label->text().isEmpty() || label->textFormat() == Qt::RichText)
			continue;
		if (dynamic_cast<dock::ui::ElideLabel *>(label) != nullptr)
			continue;
		if (label->wordWrap()) {
			if (label->heightForWidth(label->width()) > label->height() + 1)
				f.clipped << QStringLiteral("wrap-cut: %1").arg(label->text().left(24));
			continue;
		}
		const int need = label->fontMetrics().horizontalAdvance(label->text());
		if (need > label->contentsRect().width() + 1)
			f.clipped << QStringLiteral("text-cut: %1").arg(label->text().left(24));
	}
	for (QPushButton *b : dw->findChildren<QPushButton *>()) {
		if (!b->isVisibleTo(dw) || b->text().isEmpty())
			continue;
		if (b->sizeHint().width() > b->width() + 1)
			f.clipped << QStringLiteral("button-cut: %1").arg(b->text());
	}

	// m7: 押せるもの (ドックの画像の座標)。
	f.targets = collect_targets(frame, QPoint(0, 0));
	return f;
}

std::vector<EdgeFact> glyph_edges(QWidget *root, const QImage &image, const QPoint &origin)
{
	std::vector<EdgeFact> out;
	const qreal dpr = image.devicePixelRatio();
	for (QLabel *label : root->findChildren<QLabel *>()) {
		if (!label->isVisibleTo(root) || label->window() != root->window() || label->text().isEmpty() ||
		    label->textFormat() == Qt::RichText)
			continue;
		const QRect full(label->mapTo(root, QPoint(0, 0)), label->size());
		if (visible_rect_in(label, root) != full)
			continue; // スクロールで切れている QLabel は数えない (切れているのはスクロールのため)
		const QRect r = full.translated(origin);
		const QColor bg = dominant_color(image, r);
		const QRect px(qRound(r.x() * dpr), qRound(r.y() * dpr), qRound(r.width() * dpr), qRound(r.height() * dpr));
		const QRect clip = px.intersected(image.rect());
		if (clip.height() < 3)
			continue;
		auto count_row = [&](int y) {
			int n = 0;
			for (int x = clip.left(); x <= clip.right(); ++x) {
				if (wcag_contrast(QColor(image.pixel(x, y)), bg) > 1.5)
					++n;
			}
			return n;
		};
		EdgeFact e;
		e.object_name = label->objectName();
		e.text = label->text().left(30);
		e.top = count_row(clip.top());
		e.bottom = count_row(clip.bottom());
		if (e.top > 0 || e.bottom > 0) {
			const QFontMetrics fm = label->fontMetrics();
			e.label_h = label->contentsRect().height();
			e.font_h = fm.height();
			e.ink_h = label->wordWrap() ? 0 : fm.tightBoundingRect(label->text()).height();
			e.suspect = e.label_h < e.font_h || e.label_h < e.ink_h;
			out.push_back(e);
		}
	}
	return out;
}

const char *const kEdgesHeader =
	"shot\tobject_name\ttext\ttop_pixels\tbottom_pixels\tlabel_h\tfont_h\tink_h\tclip_suspected\n";

QString edges_rows(const QString &key, const std::vector<EdgeFact> &facts)
{
	QString out;
	for (const EdgeFact &e : facts) {
		const QStringList cells{tsv_cell(key),
					tsv_cell(e.object_name),
					tsv_cell(e.text),
					QString::number(e.top),
					QString::number(e.bottom),
					QString::number(e.label_h),
					QString::number(e.font_h),
					QString::number(e.ink_h),
					e.suspect ? QStringLiteral("yes") : QStringLiteral("no")};
		out += cells.join(QLatin1Char('\t')) + QLatin1Char('\n');
	}
	return out;
}

const char *const kTargetsHeader =
	"shot\tkind\tlabel\trect\tqt_rect\tvisible\tenabled\tsize_ok_24\tspacing_ok\tconflict\n";

QString targets_rows(const QString &key, const std::vector<TargetFact> &targets, const std::vector<Wcag258> &judged)
{
	QString out;
	for (std::size_t i = 0; i < targets.size(); ++i) {
		const TargetFact &t = targets[i];
		const QStringList cells{tsv_cell(key),
					t.kind,
					tsv_cell(t.label),
					rect_text(t.rect),
					rect_text(t.qt_rect),
					t.visible ? QStringLiteral("yes") : QStringLiteral("hidden"),
					t.enabled ? QStringLiteral("yes") : QStringLiteral("no"),
					yn(judged[i].size_ok),
					yn(judged[i].spacing_ok),
					tsv_cell(judged[i].conflict)};
		out += cells.join(QLatin1Char('\t')) + QLatin1Char('\n');
	}
	return out;
}

const char *const kNamesHeader = "shot\tuser\tfull\tshown\tfull_graphemes\tshown_graphemes\telided\telide_at\tline2_shown\t"
				 "line2_full\ta11y_name\ta11y_desc\n";

QString names_rows(const QString &key, const std::vector<NameFact> &names)
{
	QString out;
	for (const NameFact &n : names) {
		const QStringList cells{tsv_cell(key),
					n.user_id.right(2),
					tsv_cell(n.full),
					tsv_cell(n.shown),
					QString::number(n.full_graphemes),
					QString::number(n.shown_graphemes),
					n.elided ? QStringLiteral("yes") : QStringLiteral("no"),
					n.elided ? (n.middle ? QStringLiteral("middle") : QStringLiteral("end")) : QStringLiteral("-"),
					tsv_cell(n.line2_shown.join(QStringLiteral(" | "))),
					tsv_cell(n.line2_full.join(QStringLiteral(" | "))),
					tsv_cell(n.a11y_name),
					tsv_cell(n.a11y_desc)};
		out += cells.join(QLatin1Char('\t')) + QLatin1Char('\n');
	}
	return out;
}

const char *const kContrastHeader = "shot\tobject_name\tclass\ttext\tenabled\tfg\tbg\tratio\tdrawn_fg\tdrawn_ratio\n";

QString contrast_rows(const QString &key, const std::vector<ContrastFact> &facts)
{
	QString out;
	for (const ContrastFact &c : facts) {
		const QStringList cells{tsv_cell(key),
					tsv_cell(c.object_name),
					c.cls,
					tsv_cell(c.text),
					c.enabled ? QStringLiteral("yes") : QStringLiteral("no"),
					c.fg.name(),
					c.bg.name(),
					QString::number(c.ratio, 'f', 2),
					c.drawn_fg.name(),
					QString::number(c.drawn_ratio, 'f', 2)};
		out += cells.join(QLatin1Char('\t')) + QLatin1Char('\n');
	}
	return out;
}

} // namespace cordmux::ui_preview
