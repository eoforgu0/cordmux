// SPDX-License-Identifier: Apache-2.0

// probes.h の実装。

#include "probes.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QKeyEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QLayout>
#include <QListWidget>
#include <QMainWindow>
#include <QMenuBar>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSlider>
#include <QStatusBar>
#include <QStyle>
#include <QStyleHints>
#include <QStyleOptionButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

// GetSystemMetrics (probe:obs-layout が、実行した PC のタイトルバーの高さを読んで出力に書くだけ)。windows.h の min と
// max のマクロを無効にする。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "dock/dock_widget.h"
#include "dock/first_line.h"
#include "dock/ui/dock_metrics.h"
#include "dock/ui/focus_ring.h"
#include "fixtures.h"
#include "ledger/notice_tracker.h"
#include "ledger/rules.h"
#include "ledger/text_ids.h"
#include "measure.h"
#include "obs_dock.h"
#include "obs_names.h"

namespace cordmux::ui_preview {

namespace {

QString rect_text(const QRect &r)
{
	return QStringLiteral("%1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
}

QString theme_id_of(const QString &name)
{
	if (name == QLatin1String("yami"))
		return QStringLiteral("com.obsproject.Yami.Original");
	if (name == QLatin1String("yami-light"))
		return QStringLiteral("com.obsproject.Yami.Light");
	if (name == QLatin1String("yami-classic"))
		return QStringLiteral("com.obsproject.Yami.Classic");
	if (name == QLatin1String("yami-grey"))
		return QStringLiteral("com.obsproject.Yami.Grey");
	if (name == QLatin1String("yami-acri"))
		return QStringLiteral("com.obsproject.Yami.Acri");
	if (name == QLatin1String("yami-rachni"))
		return QStringLiteral("com.obsproject.Yami.Rachni");
	if (name == QLatin1String("system"))
		return QStringLiteral("com.obsproject.System");
	return name;
}

void save_png_file(const QImage &image, const QString &path)
{
	QDir().mkpath(QFileInfo(path).absolutePath());
	image.save(path, "PNG");
}

QStringList selected_scenes(const FaceOptions &opt, const QStringList &defaults)
{
	return opt.only.isEmpty() ? defaults : opt.only.split(QLatin1Char(','), Qt::SkipEmptyParts);
}

// 描画する幅 (--widths-em に 26 em があれば 26 em、なければ最初の値。指定がなければ 26 em)。
int first_width_px(const FaceOptions &opt, QWidget *w)
{
	const double e = opt.widths_em.isEmpty() ? 26.0 : (opt.widths_em.contains(26.0) ? 26.0 : opt.widths_em.front());
	return static_cast<int>(std::lround(e * em_px_of(w)));
}

QImage grab_dock_now(DockHost &host)
{
	settle();
	return grab_widget(host.dock.get());
}

// ---- focus ----

// フォーカスを置く相手 (種類ごとに、見えているものの最初の 1 つ)。
struct FocusTarget {
	QString kind;
	QWidget *widget = nullptr;
};

std::vector<FocusTarget> focus_targets(DockHost &host)
{
	std::vector<FocusTarget> out;
	QWidget *root = host.widget;
	auto first_named = [&](const QString &name) -> QAbstractButton * {
		for (QAbstractButton *b : root->findChildren<QAbstractButton *>(name)) {
			if (b->isVisibleTo(root) && b->isEnabled())
				return b;
		}
		return nullptr;
	};
	auto first_under = [&](const QString &parent_name) -> QAbstractButton * {
		for (QWidget *p : root->findChildren<QWidget *>(parent_name)) {
			if (!p->isVisibleTo(root))
				continue;
			for (QAbstractButton *b : p->findChildren<QAbstractButton *>()) {
				if (b->isVisibleTo(root) && b->isEnabled())
					return b;
			}
		}
		return nullptr;
	};
	// つまみ (音声タブ、表示タブ、状態タブを --tab で開いたとき)。
	auto first_slider = [&]() -> QWidget * {
		for (QSlider *s : root->findChildren<QSlider *>()) {
			if (s->isVisibleTo(root) && s->isEnabled() && s->focusPolicy() != Qt::NoFocus)
				return s;
		}
		return nullptr;
	};
	const std::pair<QString, QWidget *> all[] = {
		{QStringLiteral("slider"), first_slider()},
		{QStringLiteral("row-check"), first_named(QStringLiteral("cordmuxRowCheck"))},
		{QStringLiteral("row-menu"), first_named(QStringLiteral("cordmuxRowMenuButton"))},
		{QStringLiteral("card-button"), first_under(QStringLiteral("cordmuxNoticeCard"))},
		{QStringLiteral("rec"), first_named(QStringLiteral("cordmuxRecordButton"))},
		{QStringLiteral("dock-menu"), first_named(QStringLiteral("cordmuxDockMenuButton"))},
	};
	for (const auto &[kind, widget] : all) {
		if (widget != nullptr)
			out.push_back({kind, widget});
	}
	return out;
}

} // namespace

int face_focus(const FaceOptions &opt, Writer &w)
{
	// フォーカスリングを画像で確認する。キーボードで操作したときに表示されるか、押せる範囲を囲むか、隣の色との
	// 比を数える。マウスで操作したときに表示されないことと、ダイアログを閉じた後にフォーカスが戻ることは、
	// ドックを操作するテスト (dock_drive) が確認する。surface_gap と surface_border の列は、1 行目の背景の中のボタンがないので常に - になる。
	w.extra_name = QStringLiteral("focus.tsv");
	w.extra = QStringLiteral("shot\ttarget\tlabel\tfocused\tring_visible\tring_rect\thit_rect\tring_encloses_hit\t"
				 "sides_drawn(TRBL)\tring_colors\tmin_ratio_outer\tmin_ratio_inner\touter_px\touter_px_below_3\t"
				 "min_ratio_outer_px\tsides_below_3\tsurface_gap\tsurface_border\n");
	for (const QString &scene : selected_scenes(opt, {QStringLiteral("people-4"), QStringLiteral("notice-many"),
							   QStringLiteral("notice-all-video-stopped")})) {
		const Fixture fixture = fixture_by_id(scene.toStdString());
		if (fixture.id.empty()) {
			print_line(QStringLiteral("focus: no scene %1").arg(scene));
			++w.failures;
			continue;
		}
		DockHost host = build_dock(fixture, opt.style);
		if (opt.tab.has_value()) {
			host.widget->set_tab(*opt.tab);
			host.widget->set_audio_levels(fixture.levels);
		}
		render_dock(host, first_width_px(opt, host.widget), opt.height, 600);
		host.dock->activateWindow();

		auto shoot = [&](const QString &kind, QWidget *button) {
			// キーボードの入力があったことを、製品の監視 (dock/ui/focus_ring.cpp の InputModality) に
			// 伝える。どのウィジェットも使わないキー (Qt::Key_F13) を送り、その後に、Tab キーで
			// 移したときと同じ理由でフォーカスを置く。
			QKeyEvent press(QEvent::KeyPress, Qt::Key_F13, Qt::NoModifier);
			QApplication::sendEvent(host.widget, &press);
			QKeyEvent release(QEvent::KeyRelease, Qt::Key_F13, Qt::NoModifier);
			QApplication::sendEvent(host.widget, &release);
			button->setFocus(Qt::TabFocusReason);
			const QImage image = grab_dock_now(host);
			const QString name = scene + QStringLiteral("-") + kind;
			QString widgets;
			int index = 0;
			dump_widgets(host.dock.get(), QPoint(0, 0), &index, &widgets);
			w.shot(name, image, widgets);

			QWidget *ring = nullptr;
			for (QWidget *c : host.dock->findChildren<QWidget *>(QStringLiteral("cordmuxFocusRing"))) {
				if (c->isVisibleTo(host.dock.get()))
					ring = c;
			}
			const QRect hit_local = dock::ui::focus_hit_rect_of(button);
			const QRect hit(button->mapTo(host.dock.get(), hit_local.topLeft()), hit_local.size());
			// フォーカスリングは、ウィジェット cordmuxFocusRing の外周に描画される
			// (dock/ui/focus_ring.cpp。押せる範囲を、太さと間隔の分だけ広げた矩形)。辺ごとに、外周の 1 列
			// (リングの色)、その 1 px 外 (外の背景)、押せる範囲の 1 px 外 (内側の背景) を読み、リングの色が
			// 外か内のどちらかと見分けられる (比が 1.5 を超える) 辺を、描画されたと数える。
			// 辺が欠けていれば、リングが切れている。
			QRect ring_rect;
			QString sides;
			QStringList side_colors;
			double min_outer = 0;
			double min_inner = 0;
			int outer_px = 0;
			int outer_px_low = 0;
			double min_outer_px = 0;
			QString low_sides;
			QString surface_gap = QStringLiteral("-");
			QString surface_border = QStringLiteral("-");
			if (ring != nullptr) {
				ring_rect = QRect(ring->mapTo(host.dock.get(), QPoint(0, 0)), ring->size());
				const QRect rr = ring_rect;
				struct Side {
					QChar tag;
					QRect line;
					QRect out;
					QRect in;
				};
				const int w2 = std::max(1, rr.width() - 6);
				const int h2 = std::max(1, rr.height() - 6);
				const Side all[] = {
					{QLatin1Char('T'), QRect(rr.left() + 3, rr.top(), w2, 1), QRect(rr.left() + 3, rr.top() - 1, w2, 1),
					 QRect(rr.left() + 3, hit.top() - 1, w2, 1)},
					{QLatin1Char('R'), QRect(rr.right(), rr.top() + 3, 1, h2), QRect(rr.right() + 1, rr.top() + 3, 1, h2),
					 QRect(hit.right() + 1, rr.top() + 3, 1, h2)},
					{QLatin1Char('B'), QRect(rr.left() + 3, rr.bottom(), w2, 1), QRect(rr.left() + 3, rr.bottom() + 1, w2, 1),
					 QRect(rr.left() + 3, hit.bottom() + 1, w2, 1)},
					{QLatin1Char('L'), QRect(rr.left(), rr.top() + 3, 1, h2), QRect(rr.left() - 1, rr.top() + 3, 1, h2),
					 QRect(hit.left() - 1, rr.top() + 3, 1, h2)},
				};
				for (const Side &s : all) {
					const QColor c = dominant_color(image, s.line);
					const QColor o = dominant_color(image, s.out);
					const QColor i = dominant_color(image, s.in);
					const double ro = wcag_contrast(c, o);
					const double ri = wcag_contrast(c, i);
					const bool drawn = ro > 1.5 || ri > 1.5;
					sides += drawn ? s.tag : QLatin1Char('-');
					if (drawn) {
						min_outer = min_outer == 0 ? ro : std::min(min_outer, ro);
						min_inner = min_inner == 0 ? ri : std::min(min_inner, ri);
						side_colors << c.name();
						// リングの外側のすべてのピクセル。辺の 1 列のピクセルごとに、そのすぐ
						// 外のピクセルとの比を読む。いちばん多い色どうしの比
						// (上の min_ratio_outer) では、辺の一部だけが隣のウィジェット
						// (画面共有のアイコン) に接する所を拾えないためである。画像の
						// 外のピクセル (ドックの端で切れた所) は数えない。
						for (int k = 0; k < std::max(s.line.width(), s.line.height()); ++k) {
							const QPoint p = s.line.width() > 1 ? QPoint(s.line.left() + k, s.line.top())
											   : QPoint(s.line.left(), s.line.top() + k);
							const QPoint q = s.line.width() > 1 ? QPoint(s.out.left() + k, s.out.top())
											   : QPoint(s.out.left(), s.out.top() + k);
							if (!image.rect().contains(p) || !image.rect().contains(q))
								continue;
							const double r = wcag_contrast(image.pixelColor(p), image.pixelColor(q));
							++outer_px;
							min_outer_px = min_outer_px == 0 ? r : std::min(min_outer_px, r);
							if (r < 3.0) {
								++outer_px_low;
								if (!low_sides.contains(s.tag))
									low_sides += s.tag;
							}
						}
					}
				}
				side_colors.removeDuplicates();
			}
			const QStringList cells{name,
						kind,
						tsv_cell(qobject_cast<QAbstractButton *>(button) == nullptr ||
								 qobject_cast<QAbstractButton *>(button)->text().isEmpty()
							 ? button->accessibleName()
							 : qobject_cast<QAbstractButton *>(button)->text()),
						button->hasFocus() ? QStringLiteral("yes") : QStringLiteral("NO"),
						ring != nullptr ? QStringLiteral("yes") : QStringLiteral("NO"),
						ring != nullptr ? rect_text(ring_rect) : QString(),
						rect_text(hit),
						ring != nullptr && ring_rect.contains(hit) ? QStringLiteral("yes") : QStringLiteral("NO"),
						sides,
						side_colors.join(QLatin1Char(',')),
						QString::number(min_outer, 'f', 2),
						QString::number(min_inner, 'f', 2),
						QString::number(outer_px),
						QString::number(outer_px_low),
						QString::number(min_outer_px, 'f', 2),
						low_sides,
						surface_gap,
						surface_border};
			w.extra += cells.join(QLatin1Char('\t')) + QLatin1Char('\n');
			button->clearFocus();
			settle();
		};

		for (const FocusTarget &t : focus_targets(host))
			shoot(t.kind, t.widget);
	}
	return 0;
}

int face_timeline(const FaceOptions &opt, Writer &w)
{
	w.extra_name = QStringLiteral("timeline.tsv");
	w.extra = QStringLiteral("step\tt_s\tnote\tpress_hold\theld\traw_notices\tview_notices\tshown_cards\tfirst_line\n");
	const Fixture base = fixture_by_id("status-connected-in-call");
	ledger::NoticeTrackerConfig cfg;
	cfg.show_delay_us = static_cast<std::int64_t>(base.settings.notice_show_delay_ms) * 1000;
	cfg.clear_delay_us = static_cast<std::int64_t>(base.settings.notice_clear_delay_ms) * 1000;
	const std::vector<TimelineStep> steps = timeline_steps();
	if (steps.empty())
		return 0;
	DockHost host = build_dock(base, opt.style);
	const int width = first_width_px(opt, host.widget);
	render_dock(host, width, opt.height, 600);
	auto *scroll = host.widget->findChild<QScrollArea *>(QStringLiteral("cordmuxScroll"));
	QWidget *viewport = scroll != nullptr ? scroll->viewport() : host.widget;
	const QPoint press_at(viewport->width() / 2, std::max(1, viewport->height() - 4));
	ledger::NoticeTrackerState state;
	const std::int64_t t0 = steps.front().obs.now_us;
	bool pressed = false;
	for (const TimelineStep &step : steps) {
		if (pressed && !step.press_hold) {
			// ボタンを離す (LayoutHold は、離した後に並びの保留を解き、ためていた変化を反映する)。
			QMouseEvent release(QEvent::MouseButtonRelease, QPointF(press_at), QPointF(viewport->mapToGlobal(press_at)),
					    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
			QApplication::sendEvent(viewport, &release);
			pressed = false;
			settle();
		}
		if (step.press_hold && !pressed) {
			// スクロール領域の中でマウスのボタンを押す (並びの保留を始める操作の 1 つ。
			// dock/layout_hold.cpp)。
			QMouseEvent press(QEvent::MouseButtonPress, QPointF(press_at), QPointF(viewport->mapToGlobal(press_at)),
					  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
			QApplication::sendEvent(viewport, &press);
			pressed = true;
			settle();
		}
		const ledger::dockview::DockViewV2 raw = ledger::decide_dock(step.obs);
		const ledger::StabilizeResult r = ledger::stabilize(state, raw, step.obs.now_us, cfg);
		state = r.next;
		host.widget->render(r.view);
		settle();
		const QImage image = grab_dock_now(host);
		const QString name = QString::fromStdString(step.id);
		QString widgets;
		int index = 0;
		dump_widgets(host.dock.get(), QPoint(0, 0), &index, &widgets);
		w.shot(name, image, widgets);
		w.index_html += QStringLiteral("<p>%1</p>\n").arg(QString::fromStdString(step.note).toHtmlEscaped());

		QStringList raw_keys;
		for (const auto &n : raw.notices)
			raw_keys << QString::fromStdString(n.key);
		QStringList view_keys;
		for (const auto &n : r.view.notices)
			view_keys << QString::fromStdString(n.key);
		// 描画されているカードと行の並び (上から)。
		std::vector<std::pair<int, QString>> cards;
		for (QWidget *c : host.widget->findChildren<QWidget *>()) {
			const QString on = c->objectName();
			if (on == QLatin1String("cordmuxNoticeCard") && c->isVisibleTo(host.widget))
				cards.push_back({c->mapTo(host.widget, QPoint(0, 0)).y(), c->property("noticeKey").toString()});
		}
		std::sort(cards.begin(), cards.end());
		QStringList shown;
		for (const auto &[y, key] : cards)
			shown << key;
		auto *first = dynamic_cast<dock::FirstLineBar *>(host.widget->findChild<QWidget *>(QStringLiteral("cordmuxFirstLine")));
		const QStringList cells{name,
					QString::number(static_cast<double>(step.obs.now_us - t0) / 1e6, 'f', 1),
					tsv_cell(QString::fromStdString(step.note)),
					step.press_hold ? QStringLiteral("yes") : QStringLiteral("no"),
					host.widget->layout_held() ? QStringLiteral("yes") : QStringLiteral("no"),
					raw_keys.join(QLatin1Char(',')),
					view_keys.join(QLatin1Char(',')),
					shown.join(QLatin1Char(',')),
					tsv_cell(first != nullptr ? first->shown_text() : QString())};
		w.extra += cells.join(QLatin1Char('\t')) + QLatin1Char('\n');
	}
	if (pressed) {
		QMouseEvent release(QEvent::MouseButtonRelease, QPointF(press_at), QPointF(viewport->mapToGlobal(press_at)),
				    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
		QApplication::sendEvent(viewport, &release);
		settle();
	}
	return 0;
}

int face_theme_switch(QApplication &app, const ObsPaths &paths, const FaceOptions &opt, const QString &theme_name,
		      Writer &w)
{
	// テーマ・フォントサイズ・密度を 2 回ずつ行き来させ、製品に OBS の THEME_CHANGED と同じ
	// 通知 (DockWidget::on_theme_changed。製品では DockPoll::on_theme_changed から呼ぶ) を送る。毎回、同じ設定で
	// 作り直したドックと比べ、1 行目の文字の大きさの倍率・余白・最小幅・画像のピクセルが同じかを数える
	// (スタイルの二重の適用や、古い値の残りを見つける)。
	w.extra_name = QStringLiteral("theme-switch.tsv");
	w.extra = QStringLiteral("step\ttheme\tfont\tdensity\tbase_pt\tfirst_pt\tratio\tratio_expected\tspacing_px\t"
				 "spacing_fresh\tmin_width\tmin_width_fresh\tsize\tdiff_pixels\tsame_as_fresh\n");
	const QString other = theme_name == QLatin1String("yami-light") ? QStringLiteral("yami") : QStringLiteral("yami-light");
	const Appearance a0 = opt.appearance;
	Appearance big = a0;
	big.font_scale = a0.font_scale == 14 ? 10 : 14;
	Appearance dense = a0;
	dense.density = a0.density == -2 ? -4 : -2;
	struct State {
		QString theme;
		Appearance ap;
	};
	const std::vector<State> states{{other, a0}, {theme_name, a0}, {other, a0}, {theme_name, a0},
					{theme_name, big}, {theme_name, a0}, {theme_name, big}, {theme_name, a0},
					{theme_name, dense}, {theme_name, a0}, {theme_name, dense}, {theme_name, a0}};
	const QString scene = opt.only.isEmpty() ? QStringLiteral("notice-many") : opt.only.section(QLatin1Char(','), 0, 0);
	const Fixture fixture = fixture_by_id(scene.toStdString());
	DockHost host = build_dock(fixture, opt.style);
	const QSize size(420, 600);
	int step = 0;
	for (const State &s : states) {
		++step;
		ComposedTheme composed;
		QString error;
		if (!apply_obs_theme(app, paths, theme_id_of(s.theme), opt.platform_defaults, &composed, &error, s.ap)) {
			print_line(QStringLiteral("theme-switch %1: %2").arg(s.theme, error));
			++w.failures;
			break;
		}
		host.widget->on_theme_changed();
		settle();
		render_dock(host, size.width(), size.height(), 600);
		const QImage switched = grab_dock_now(host);
		DockHost fresh = build_dock(fixture, opt.style);
		render_dock(fresh, size.width(), size.height(), 600);
		const QImage made = grab_dock_now(fresh);

		auto first_of = [](DockHost &h) {
			return dynamic_cast<dock::FirstLineBar *>(h.widget->findChild<QWidget *>(QStringLiteral("cordmuxFirstLine")));
		};
		dock::FirstLineBar *f1 = first_of(host);
		dock::FirstLineBar *f2 = first_of(fresh);
		const double base_pt = dock::ui::base_point_size(host.widget);
		const double first_pt = f1 != nullptr ? f1->text_font().pointSizeF() : 0;
		const int spacing = f1 != nullptr && f1->layout() != nullptr ? f1->layout()->spacing() : -1;
		const int spacing_fresh = f2 != nullptr && f2->layout() != nullptr ? f2->layout()->spacing() : -1;
		int diff = 0;
		if (switched.size() == made.size()) {
			for (int y = 0; y < switched.height(); ++y) {
				for (int x = 0; x < switched.width(); ++x)
					diff += switched.pixel(x, y) != made.pixel(x, y) ? 1 : 0;
			}
		} else {
			diff = -1;
		}
		const double ratio = base_pt > 0 ? first_pt / base_pt : 0;
		const bool same = diff == 0 && spacing == spacing_fresh &&
				  host.widget->computed_min_width() == fresh.widget->computed_min_width() &&
				  std::abs(ratio - opt.style.first_line_scale) < 0.01;
		const QString name = QStringLiteral("s%1-%2-f%3-d%4").arg(step, 2, 10, QLatin1Char('0')).arg(s.theme).arg(s.ap.font_scale).arg(s.ap.density);
		QString widgets;
		int index = 0;
		dump_widgets(host.dock.get(), QPoint(0, 0), &index, &widgets);
		w.shot(name, switched, widgets);
		const QStringList cells{name,
					s.theme,
					QString::number(s.ap.font_scale),
					QString::number(s.ap.density),
					QString::number(base_pt, 'f', 2),
					QString::number(first_pt, 'f', 2),
					QString::number(ratio, 'f', 3),
					QString::number(opt.style.first_line_scale, 'f', 3),
					QString::number(spacing),
					QString::number(spacing_fresh),
					QString::number(host.widget->computed_min_width()),
					QString::number(fresh.widget->computed_min_width()),
					QStringLiteral("%1x%2").arg(switched.width()).arg(switched.height()),
					QString::number(diff),
					same ? QStringLiteral("yes") : QStringLiteral("NO")};
		w.extra += cells.join(QLatin1Char('\t')) + QLatin1Char('\n');
		if (!same)
			print_line(QStringLiteral("theme-switch %1: differs from a fresh dock (diff %2)").arg(name).arg(diff));
	}
	// 元の設定に戻す (同じ実行の次の言語の画像を、指定されたテーマで描画するため)。
	ComposedTheme composed;
	QString error;
	apply_obs_theme(app, paths, theme_id_of(theme_name), opt.platform_defaults, &composed, &error, a0);
	host.widget->on_theme_changed();
	settle();
	return 0;
}

int face_probe_menu_check(QApplication &app, const ComposedTheme &theme, Writer &w)
{
	// メニューのチェックマークが見えるかを確認する。製品のウィジェットを使わない、ただの QMenu のチェックできる
	// 項目を、(a) QSS なし (基底の windowsvista のスタイルとパレットだけ) と (b) OBS のテーマの QSS で描画し、
	// 項目の左の余白に描画されたピクセルの数を、チェックのない項目と比べる。(a) でも描画されなければ
	// プレビューのプラグインの不足で、(b) でだけ描画されなければ、テーマの QSS の下でチェックマークが消える
	// (製品で対処が必要)。
	w.extra_name = QStringLiteral("menu-check.tsv");
	w.extra = QStringLiteral("variant\titem\tchecked\tgutter_rect\tgutter_bg\tgutter_pixels_differing\n");
	const QString keep = app.styleSheet();
	for (int variant = 0; variant < 2; ++variant) {
		const QString vname = variant == 0 ? QStringLiteral("no-qss") : QStringLiteral("theme-qss");
		app.setStyleSheet(variant == 0 ? QString() : theme.stylesheet);
		settle();
		QMenu menu;
		menu.setAttribute(Qt::WA_DontShowOnScreen, true);
		QAction *on = menu.addAction(QStringLiteral("check=1"));
		on->setCheckable(true);
		on->setChecked(true);
		QAction *off = menu.addAction(QStringLiteral("check=0"));
		off->setCheckable(true);
		off->setChecked(false);
		QAction *plain = menu.addAction(QStringLiteral("plain"));
		menu.popup(QPoint(100, 100));
		settle();
		const QImage image = grab_widget(&menu);
		QString widgets;
		int index = 0;
		dump_widgets(&menu, QPoint(0, 0), &index, &widgets);
		w.shot(QStringLiteral("menu-check-") + vname, image, widgets);
		for (QAction *a : {on, off, plain}) {
			const QRect r = menu.actionGeometry(a);
			// 項目の左の余白 (テキストの始まりより左)。Qt のスタイルがチェックマークを描画する所
			// (左から 1.5 em 前後) を含めて、項目の高さのすべて。
			const QRect gutter(r.left(), r.top(), std::min(r.width(), 28), r.height());
			const QColor bg = dominant_color(image, gutter);
			int differing = 0;
			for (int y = gutter.top(); y <= gutter.bottom(); ++y) {
				for (int x = gutter.left(); x <= gutter.right(); ++x)
					differing += wcag_contrast(QColor(image.pixel(x, y)), bg) > 1.5 ? 1 : 0;
			}
			const QStringList cells{vname,
						a->text(),
						a->isCheckable() ? (a->isChecked() ? QStringLiteral("1") : QStringLiteral("0"))
								 : QStringLiteral("-"),
						rect_text(gutter),
						bg.name(),
						QString::number(differing)};
			w.extra += cells.join(QLatin1Char('\t')) + QLatin1Char('\n');
		}
		{
			// チェックの付いた項目と付いていない項目の余白を、ピクセルごとに比べる (テキストは同じ
			// 長さなので、違うのはチェックマークだけのはず)。
			const QRect a = menu.actionGeometry(on);
			const QRect b = menu.actionGeometry(off);
			const int gw = std::min(a.width(), 28);
			int diff = 0;
			for (int y = 0; y < std::min(a.height(), b.height()); ++y) {
				for (int x = 0; x < gw; ++x)
					diff += image.pixel(a.left() + x, a.top() + y) != image.pixel(b.left() + x, b.top() + y) ? 1 : 0;
			}
			w.extra += QStringList{vname, QStringLiteral("check=1 vs check=0"), QStringLiteral("1-vs-0"),
					       rect_text(QRect(a.left(), a.top(), gw, a.height())), QString(), QString::number(diff)}
					   .join(QLatin1Char('\t')) +
				   QLatin1Char('\n');
		}
		menu.close();
		settle();
		// 比べる基準。同じ基底のスタイルで、QCheckBox (チェックの付いたものと付いていないもの) の
		// チェックマークの範囲が違うピクセルの数。QSS なしでもここが 0 なら、このプラグインでは基底の
		// スタイル (windowsvista) が OS のコントロールを描画できておらず、メニューのチェックマークがないのも
		// 同じ理由と読む (実際の OBS の [ドック] のメニューのチェックと並べて見る)。
		QCheckBox box_on(QStringLiteral("on"));
		QCheckBox box_off(QStringLiteral("off"));
		box_on.setChecked(true);
		box_on.setAttribute(Qt::WA_DontShowOnScreen, true);
		box_off.setAttribute(Qt::WA_DontShowOnScreen, true);
		box_on.show();
		box_off.show();
		box_off.resize(box_on.size());
		settle();
		const QImage on_img = grab_widget(&box_on);
		const QImage off_img = grab_widget(&box_off);
		QStyleOptionButton option;
		option.initFrom(&box_on);
		const QRect ind = box_on.style()->subElementRect(QStyle::SE_CheckBoxIndicator, &option, &box_on);
		int box_diff = 0;
		for (int y = ind.top(); y <= ind.bottom() && y < on_img.height() && y < off_img.height(); ++y) {
			for (int x = ind.left(); x <= ind.right() && x < on_img.width() && x < off_img.width(); ++x)
				box_diff += on_img.pixel(x, y) != off_img.pixel(x, y) ? 1 : 0;
		}
		save_png_file(on_img, w.dir + QStringLiteral("/checkbox-on-") + vname + QStringLiteral(".png"));
		w.extra += QStringList{vname, QStringLiteral("QCheckBox"), QStringLiteral("1-vs-0"), rect_text(ind), QString(),
				       QString::number(box_diff)}
				   .join(QLatin1Char('\t')) +
			   QLatin1Char('\n');
		box_on.hide();
		box_off.hide();
	}
	app.setStyleSheet(keep);
	settle();
	return 0;
}

int face_probe_obs_layout(const FaceOptions &opt, Writer &w)
{
	// OBS の既定のレイアウトで、下に並ぶドックが何 px になるか (プレビューで描画するドックの大きさの根拠にする)。
	// OBS 31.1.1 の OBSBasic::on_resetDocks_triggered (frontend/widgets/OBSBasic_Docks.cpp) と同じく、
	// メインウィンドウの下のドックの領域に 5 つのドック (シーン・ソース・音声ミキサー・シーントランジション・
	// コントロール) を横に並べ、縦はメインウィンドウの高さの 22.5%、横は 22.5% / 22.5% / 残り / 5% / 21% を
	// resizeDocks に渡す。区切り (QMainWindow::separator) とドックのタイトルバーは、プラグインに適用した OBS の
	// テーマの QSS のまま描画する。メインウィンドウは 1920 x 1080 の画面で最大化した大きさ (Windows 10 の既定の
	// 下のタスクバーは 40 px)。高さは、作業領域の高さからタイトルバー (SM_CYCAPTION。96 dpi で 23 px) を引いた値
	// (最大化したウィンドウは、外枠が画面の外に出る)。ここは計算の根拠を Qt のレイアウトで確認するだけで、実際の
	// OBS では測らない。ほかのドックの中身は何もしない QWidget にする (実際のドックの中身の最小の幅は、求めた幅より
	// 小さいと見込んでいる。確認していない)。cordmux のドックは、製品の DockWidget (平常の場面) を OBSDock に
	// 入れて、(1) シーンのドックとタブで重ねた形と、(2) コントロールのドックの右に分けて追加した形の 2 つで、中身の
	// 大きさを読む。
	w.extra_name = QStringLiteral("obs-layout.tsv");
	w.extra = QStringLiteral("case\tdock\ttitle\tdock_rect\tcontent_rect\ttitle_bar_h\trequested_w\trequested_h\n");
	const int caption_h = 23;          // SM_CYCAPTION (96 dpi の Windows 10 の既定)
	// 作業領域は画面から読む (既定の画面なら 1920 x 1040 = 1080 - タスクバー 40。--screen で替えた画面なら、その
	// 作業領域)。
	const QRect work = QGuiApplication::primaryScreen()->availableGeometry();
	const int work_h = work.height();
	const QSize main_size(work.width(), work_h - caption_h);
	print_line(QStringLiteral("obs-layout: main window %1x%2 (caption %3, this PC SM_CYCAPTION=%4)")
			   .arg(main_size.width())
			   .arg(main_size.height())
			   .arg(caption_h)
			   .arg(GetSystemMetrics(SM_CYCAPTION)));
	// (3) beside-scenes: シーンのドックに OBS と同じ中身 (OBSBasic.ui の scenesFrame と SceneTree。外側の余白
	// 1・0・1・1、枠線なし、項目の間隔 0) を入れ、cordmux のドックをその右に分けて追加し、2 つのドックのテキストの
	// 左端を並べて数える (cordmux のドックのテキストの左端が、シーンのドックと 0.25 em 以内でそろっているかを
	// 見る)。SceneTree は QListWidget を継承するだけで、Yami のテーマに SceneTree の型の名前で選ぶ規則はないので、
	// QListWidget のまま置く。
	QString edges = QStringLiteral("dock\tpart\tink_left_in_content\tband\n");
	for (int layout_case = 0; layout_case < 4; ++layout_case) {
		const QString case_name = layout_case == 0 ? QStringLiteral("default")
				       : layout_case == 1  ? QStringLiteral("tab-with-scenes")
				       : layout_case == 2  ? QStringLiteral("right-of-controls")
							   : QStringLiteral("beside-scenes");
		if (!opt.only.isEmpty() && !opt.only.split(QLatin1Char(',')).contains(case_name))
			continue;
		auto main_window = std::make_unique<OBSBasic>();
		main_window->setAttribute(Qt::WA_DontShowOnScreen, true);
		main_window->setCentralWidget(new QWidget);
		main_window->menuBar()->addMenu(QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.File")));
		main_window->statusBar()->showMessage(QStringLiteral(" "));
		main_window->setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
		main_window->setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);
		struct Named {
			const char *object_name;
			const char *title_key;
		};
		const Named kDocks[] = {{"scenesDock", "Basic.Main.Scenes"},
					{"sourcesDock", "Basic.Main.Sources"},
					{"mixerDock", "Mixer"},
					{"transitionsDock", "Basic.SceneTransitions"},
					{"controlsDock", "Basic.Main.Controls"}};
		QList<QDockWidget *> docks;
		for (const Named &n : kDocks) {
			auto *dock = new OBSDock(main_window.get());
			dock->setObjectName(QString::fromLatin1(n.object_name));
			dock->setWindowTitle(QString::fromUtf8(obs_frontend_get_locale_string(n.title_key)));
			if (layout_case == 3 && docks.isEmpty()) {
				auto *contents = new QWidget;
				auto *outer = new QVBoxLayout(contents);
				outer->setSpacing(0);
				outer->setContentsMargins(1, 0, 1, 1);
				auto *frame = new QFrame;
				frame->setObjectName(QStringLiteral("scenesFrame"));
				auto *inner = new QVBoxLayout(frame);
				inner->setSpacing(0);
				inner->setContentsMargins(0, 0, 0, 0);
				auto *list = new QListWidget;
				list->setObjectName(QStringLiteral("scenes"));
				list->setFrameShape(QFrame::NoFrame);
				list->setFrameShadow(QFrame::Plain);
				list->setSpacing(0);
				list->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
				const QString scene = QString::fromUtf8(obs_frontend_get_locale_string("Basic.Scene"));
				list->addItem(scene);
				list->addItem(scene + QStringLiteral(" 2"));
				inner->addWidget(list);
				outer->addWidget(frame);
				dock->setWidget(contents);
			} else {
				dock->setWidget(new QWidget);
			}
			if (docks.isEmpty())
				main_window->addDockWidget(Qt::BottomDockWidgetArea, dock);
			else
				main_window->splitDockWidget(docks.back(), dock, Qt::Horizontal);
			docks.push_back(dock);
		}
		// 先に OBS の既定のレイアウトにする (利用者は、既定のレイアウトに cordmux のドックを追加する)。
		main_window->resize(main_size);
		main_window->show();
		settle();
		// on_resetDocks_triggered と同じ計算 (整数の割り算も同じ)。
		const int cx = main_window->width();
		const int cy_all = main_window->height();
		const int cx22_5 = cx * 225 / 1000;
		const int cx5 = cx * 5 / 100;
		const int cx21 = cx * 21 / 100;
		const int cy = cy_all * 225 / 1000;
		const int mixer = cx - (cx22_5 * 2 + cx5 + cx21);
		const QList<QDockWidget *> base = docks.mid(0, 5);
		const QList<int> sizes{cx22_5, cx22_5, mixer, cx5, cx21};
		main_window->resizeDocks(base, {cy, cy, cy, cy, cy}, Qt::Vertical);
		main_window->resizeDocks(base, sizes, Qt::Horizontal);
		settle();
		DockHost host;
		if (layout_case != 0) {
			host = build_dock(fixture_by_id("glance-dock-closed"), opt.style);
			// メインウィンドウが所有する (OBS もメインウィンドウの子にする)。
			OBSDock *cordmux_dock = host.dock.release();
			// フローティングのウィンドウとして一度表示しているので、隠してフローティングを
			// 解いてから、メインウィンドウの下のドックの領域に入れる (addDockWidget が親を
			// 替える)。メインウィンドウの子として描画されるように、画面に表示しないための
			// 属性はメインウィンドウの側にだけ残す。
			cordmux_dock->hide();
			cordmux_dock->setFloating(false);
			cordmux_dock->setAttribute(Qt::WA_DontShowOnScreen, false);
			settle();
			main_window->addDockWidget(Qt::BottomDockWidgetArea, cordmux_dock);
			if (layout_case == 1)
				main_window->tabifyDockWidget(docks[0], cordmux_dock);
			else if (layout_case == 2)
				main_window->splitDockWidget(docks[4], cordmux_dock, Qt::Horizontal);
			else
				main_window->splitDockWidget(docks[0], cordmux_dock, Qt::Horizontal);
			cordmux_dock->show();
			settle();
			if (layout_case == 1)
				cordmux_dock->raise();
			if (layout_case == 3) {
				// 並べて比べるため、シーンのドックと同じ幅にする。
				main_window->resizeDocks({docks[0], cordmux_dock}, {cx22_5, cx22_5}, Qt::Horizontal);
			}
			settle();
			cordmux_dock->widget()->update();
			settle();
			docks.push_back(cordmux_dock);
		}
		for (int i = 0; i < docks.size(); ++i) {
			QDockWidget *d = docks[i];
			const QRect dock_rect(d->mapTo(main_window.get(), QPoint(0, 0)), d->size());
			const QRect content(d->widget()->mapTo(d, QPoint(0, 0)), d->widget()->size());
			w.extra += QStringList{case_name,
					       d->objectName(),
					       tsv_cell(d->windowTitle()),
					       rect_text(dock_rect),
					       rect_text(content),
					       QString::number(content.y()),
					       i < 5 ? QString::number(sizes[i]) : QStringLiteral("-"),
					       i < 5 ? QString::number(cy) : QStringLiteral("-")}
					   .join(QLatin1Char('\t')) +
				   QLatin1Char('\n');
		}
		QString widgets;
		int index = 0;
		dump_widgets(main_window.get(), QPoint(0, 0), &index, &widgets);
		const QImage image = grab_widget(main_window.get());
		w.shot(QStringLiteral("obs-layout-") + case_name, image, widgets);
		if (layout_case == 3) {
			// テキストの左端 (中身の左端からの px)。ウィジェットの矩形の中で、矩形にいちばん多い色との
			// 比が 1.5 を超えるピクセルの、いちばん左の列 (フォーカスリングと文字の見切れを数えるときと
			// 同じしきい値)。
			const auto ink_left = [&](QWidget *part, const QRect &band_local) -> int {
				const QRect band(part->mapTo(main_window.get(), band_local.topLeft()), band_local.size());
				const QColor bg = dominant_color(image, band);
				for (int x = band.left(); x <= band.right(); ++x) {
					for (int y = band.top(); y <= band.bottom(); ++y) {
						if (wcag_contrast(QColor(image.pixel(x, y)), bg) > 1.5)
							return x;
					}
				}
				return -1;
			};
			QWidget *scenes_content = docks[0]->widget();
			const int scenes_left = scenes_content->mapTo(main_window.get(), QPoint(0, 0)).x();
			if (auto *list = scenes_content->findChild<QListWidget *>(QStringLiteral("scenes"))) {
				const QRect item = list->visualItemRect(list->item(0));
				const QRect band(list->viewport()->mapTo(list, item.topLeft()), item.size());
				const int x = ink_left(list, band);
				edges += QStringList{QStringLiteral("scenesDock"), QStringLiteral("scene item 1 text"),
						     QString::number(x < 0 ? -1 : x - scenes_left), rect_text(band)}
						 .join(QLatin1Char('\t')) +
					 QLatin1Char('\n');
			}
			QWidget *cordmux_content = docks.back()->widget();
			const int cordmux_left = cordmux_content->mapTo(main_window.get(), QPoint(0, 0)).x();
			for (const char *name : {"cordmuxFirstLine", "cordmuxRowCheck", "cordmuxRecordButton"}) {
				QWidget *part = nullptr;
				for (QWidget *c : cordmux_content->findChildren<QWidget *>(QString::fromLatin1(name))) {
					if (c->isVisibleTo(cordmux_content)) {
						part = c;
						break;
					}
				}
				if (part == nullptr)
					continue;
				const int x = ink_left(part, part->rect());
				const int widget_x = part->mapTo(main_window.get(), QPoint(0, 0)).x() - cordmux_left;
				edges += QStringList{QStringLiteral("cordmux"),
						     QStringLiteral("%1 (widget x=%2)").arg(QString::fromLatin1(name)).arg(widget_x),
						     QString::number(x < 0 ? -1 : x - cordmux_left),
						     rect_text(QRect(part->mapTo(main_window.get(), QPoint(0, 0)), part->size()))}
						 .join(QLatin1Char('\t')) +
					 QLatin1Char('\n');
			}
			// 2 つのドックの中身だけを並べた画像 (見比べるため)。
			const QRect a(scenes_content->mapTo(main_window.get(), QPoint(0, 0)), scenes_content->size());
			const QRect b(cordmux_content->mapTo(main_window.get(), QPoint(0, 0)), cordmux_content->size());
			const QRect both = a.united(b);
			save_png_file(image.copy(both), w.dir + QStringLiteral("/beside-scenes-contents.png"));
			write_text_file(w.dir + QStringLiteral("/beside-scenes-edges.tsv"), edges);
		}
		main_window->hide();
		settle();
	}
	return 0;
}

} // namespace cordmux::ui_preview
