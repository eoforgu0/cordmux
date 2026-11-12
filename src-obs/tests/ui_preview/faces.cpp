// SPDX-License-Identifier: Apache-2.0

// faces.h の実装。

#include "faces.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QGroupBox>
#include <QHelpEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QSet>
#include <QSlider>
#include <QSpinBox>
#include <QStyle>
#include <QStyleHints>
#include <QStyleOptionFrame>
#include <QTextBoundaryFinder>
#include <QTextEdit>
#include <QTextLayout>
#include <QTextOption>
#include <QTextStream>
#include <QThread>
#include <QtMath>
#include <QToolTip>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>

#include "dock/dock_menus.h"
#include "dock/dock_widget.h"
#include "dock/first_line.h"
#include "dock/rename_dialog.h"
#include "dock/ui/dock_metrics.h"
#include "dock/ui/elide_label.h"
#include "dock/ui/focus_ring.h"
#include "dock/ui/message_dialog.h"
#include "dock/ui/theme_read.h"
#include "ledger/text_ids.h"
#include "locale_stub.h"
#include "measure.h"
#include "obs_dock.h"
#include "obs_names.h"
#include "probes.h"

#if CORDMUX_UI_PREVIEW_HAS_ROWS
#include "dock/people_list.h"
#endif
#if CORDMUX_UI_PREVIEW_HAS_WINDOWS
#include "dock/participants_window.h"
#include "dock/settings_window.h"
#endif
#if CORDMUX_UI_PREVIEW_HAS_INSTALL_UI
#include "dock/install_dialogs.h"
#include "dock/install_runtime_files.h"
#endif

namespace cordmux::ui_preview {

void print_line(const QString &line)
{
	std::fputs(line.toUtf8().constData(), stdout);
	std::fputs("\n", stdout);
	std::fflush(stdout);
}

bool write_text_file(const QString &path, const QString &text)
{
	QDir().mkpath(QFileInfo(path).absolutePath());
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return false;
	file.write(text.toUtf8());
	return true;
}

namespace {

bool save_png(const QImage &image, const QString &path)
{
	QDir().mkpath(QFileInfo(path).absolutePath());
	return image.save(path, "PNG");
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

// ---- widgets.tsv ----

// ウィジェットが表示しているテキスト (QLabel・ボタン・入力欄・コンボボックス・グループボックスのタイトル)。
QString shown_text(QWidget *w)
{
	if (auto *label = qobject_cast<QLabel *>(w))
		return label->text();
	if (auto *button = qobject_cast<QAbstractButton *>(w))
		return button->text();
	if (auto *edit = qobject_cast<QLineEdit *>(w))
		return edit->text().isEmpty() ? edit->placeholderText() : edit->text();
	if (auto *combo = qobject_cast<QComboBox *>(w))
		return combo->currentText();
	if (auto *box = qobject_cast<QGroupBox *>(w))
		return box->title();
	return QString();
}

QString full_text(QWidget *w)
{
	if (auto *elide = dynamic_cast<dock::ui::ElideLabel *>(w))
		return elide->full_text();
	return shown_text(w);
}

QString props_of(QObject *o)
{
	QStringList parts;
	for (const char *name : {"userId", "noticeKey", "tier", "recordState", "restartRequired", "dialogId", "l1Moved",
				 "l1BorderPx", "l1Surface"}) {
		const QVariant v = o->property(name);
		if (v.isValid())
			parts << QStringLiteral("%1=%2").arg(QString::fromLatin1(name), v.toString());
	}
	return parts.join(QLatin1Char(';'));
}

int depth_of(QWidget *w, QWidget *root)
{
	int d = 0;
	for (QWidget *p = w; p != nullptr && p != root; p = p->parentWidget())
		++d;
	return d;
}

QString rect_cell(const QRect &r)
{
	return r.isValid() ? QStringLiteral("%1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()) : QString();
}

// メニューの項目に Qt のツールチップのイベントを送り、Qt が実際に表示したテキストを読む (QMenu の toolTipsVisible が
// true のとき、明示したツールチップだけを表示するのか、項目のテキストから作った既定値も表示するのかを、実際の Qt で
// 確認するため)。メニューが開いていなければ空を返す。
QString shown_tip_of(QMenu *menu, QAction *a)
{
	if (!menu->isVisible())
		return QString();
	const QRect r = menu->actionGeometry(a);
	if (!r.isValid())
		return QString();
	// 先に目印のテキストのツールチップを表示しておき、イベントの後のテキストを読む。QMenu が項目のテキストを
	// 表示すればテキストが替わり、表示しなければ (hideText を呼ぶか、何もしなければ) 目印のまま残る。
	// ツールチップの表示と非表示のアニメーション (Qt のエフェクト) は無効にしておく (エフェクトの途中は
	// isVisible が false で、表示されたかを読めない)。
	QApplication::setEffectEnabled(Qt::UI_FadeTooltip, false);
	QApplication::setEffectEnabled(Qt::UI_AnimateTooltip, false);
	const QString sentinel = QStringLiteral("\x01ui_preview-sentinel");
	const QPoint p = r.center();
	QToolTip::showText(menu->mapToGlobal(p), sentinel, menu);
	QHelpEvent ev(QEvent::ToolTip, p, menu->mapToGlobal(p));
	QApplication::sendEvent(menu, &ev);
	const QString after = QToolTip::text();
	QToolTip::hideText();
	return after == sentinel ? QString() : after;
}

} // namespace

const char *const kWidgetsHeader = "index\tdepth\tobject_name\tclass\tobs_class\tvisible\tenabled\tx\ty\tw\th\ttext\tfull_text\t"
				   "tooltip\ta11y_name\ta11y_desc\tfg\tbuddy\tchecked\tprops\thit_rect\tshown_tip\n";

// screen-fit.tsv の見出し (列の中身は screen_fit_row の前のコメント)。
const char *const kScreenFitHeader =
	"shot\tscreen\tavail_w\tavail_h\tframe\twindow_w\twindow_h\tfits\tscrolls\tview_h\tcontent_h\tbuttons\tbuttons_out\t"
	"rows\trows_unreachable\trows_taller_than_view\tunreachable\n";

// root と、その子のすべてのウィジェットを書く。origin は、画像の上での root の左上 (メニューを重ねた画像のため)。
void dump_widgets(QWidget *root, const QPoint &origin, int *index, QString *out)
{
	QList<QWidget *> all{root};
	all += root->findChildren<QWidget *>();
	for (QWidget *w : all) {
		// 別のウィンドウ (開いているメニューなど) は、そのウィンドウをルートにして別に書く (同じ
		// ウィジェットを 2 回書かない)。
		if (w != root && w->window() != root->window())
			continue;
		const QPoint at = (w == root ? QPoint(0, 0) : w->mapTo(root, QPoint(0, 0))) + origin;
		const QColor fg = w->palette().color(w->foregroundRole());
		QString buddy;
		if (auto *label = qobject_cast<QLabel *>(w)) {
			if (label->buddy() != nullptr)
				buddy = label->buddy()->objectName().isEmpty()
						? QString::fromLatin1(label->buddy()->metaObject()->className())
						: label->buddy()->objectName();
		}
		QString checked;
		QString hit;
		if (auto *button = qobject_cast<QAbstractButton *>(w)) {
			if (button->isCheckable())
				checked = button->isChecked() ? QStringLiteral("1") : QStringLiteral("0");
			// 押せる範囲 (measure.h のコメント。画像の座標)。
			const QRect r = dock::ui::focus_hit_rect_of(button);
			hit = rect_cell(QRect(at + r.topLeft(), r.size()));
		}
		// セルは 1 つずつつなぐ (QString::arg を重ねると、差し込んだ文字列の中の %N が次の arg で
		// 置き換えられる)。
		const QStringList cells{QString::number(*index),
					QString::number(depth_of(w, root)),
					tsv_cell(w->objectName()),
					QString::fromLatin1(w->metaObject()->className()),
					tsv_cell(w->property("class").toString()),
					QString::number(w->isVisibleTo(root) || w == root ? 1 : 0),
					QString::number(w->isEnabled() ? 1 : 0),
					QString::number(at.x()),
					QString::number(at.y()),
					QString::number(w->width()),
					QString::number(w->height()),
					tsv_cell(shown_text(w)),
					tsv_cell(full_text(w)),
					tsv_cell(w->toolTip()),
					tsv_cell(w->accessibleName()),
					tsv_cell(w->accessibleDescription()),
					fg.name(QColor::HexRgb),
					tsv_cell(buddy),
					checked,
					tsv_cell(props_of(w)),
					hit,
					QString()};
		*out += cells.join(QLatin1Char('\t')) + QLatin1Char('\n');
		++*index;
		// メニューは項目も書く (項目はウィジェットではないので、上の一覧に出ない)。
		if (auto *menu = qobject_cast<QMenu *>(w)) {
			for (QAction *a : menu->actions()) {
				const QRect r = menu->actionGeometry(a);
				const QString text = a->isSeparator() ? QStringLiteral("-") : a->text();
				const QString item_checked =
					a->isCheckable() ? (a->isChecked() ? QStringLiteral("1") : QStringLiteral("0")) : QString();
				const QString tip = a->isSeparator() ? QString() : shown_tip_of(menu, a);
				const QStringList item{QString::number(*index),
						       QString::number(depth_of(w, root) + 1),
						       tsv_cell(a->objectName()),
						       a->menu() != nullptr ? QStringLiteral("QAction+submenu") : QStringLiteral("QAction"),
						       QString(),
						       QString::number(a->isVisible() ? 1 : 0),
						       QString::number(a->isEnabled() ? 1 : 0),
						       QString::number(at.x() + r.x()),
						       QString::number(at.y() + r.y()),
						       QString::number(r.width()),
						       QString::number(r.height()),
						       tsv_cell(text),
						       tsv_cell(text),
						       tsv_cell(a->toolTip()),
						       QString(),
						       QString(),
						       QString(),
						       QString(),
						       item_checked,
						       QString(),
						       a->isSeparator() ? QString() : rect_cell(QRect(at + r.topLeft(), r.size())),
						       tsv_cell(tip)};
				*out += item.join(QLatin1Char('\t')) + QLatin1Char('\n');
				++*index;
			}
		}
	}
}

// ---- 1 枚の画像の書き出し ----

void Writer::shot(const QString &name, const QImage &image, const QString &widgets)
{
	if (image.isNull()) {
		print_line(QStringLiteral("FAILED %1/%2").arg(dir, name));
		++failures;
		return;
	}
	save_png(image, dir + QStringLiteral("/") + name + QStringLiteral(".png"));
	write_text_file(dir + QStringLiteral("/") + name + QStringLiteral(".widgets.tsv"),
			QString::fromLatin1(kWidgetsHeader) + widgets);
	index_html += QStringLiteral("<figure><img src=\"%1.png\"><figcaption>%1</figcaption></figure>\n").arg(name);
}

namespace {

// 画像ごとの行の表 (1 列目が画像の名前) を書く。同じ出力先に、前の実行 (同じ描画の対象の別の場面。render_sets.py は
// 設定ウィンドウの「バージョン情報」を何回かに分けて描画する) が書いた表があれば、今回描画した画像の行だけを
// 入れ替え、ほかの画像の行は残す。見出しが違えば (表の形が変わった前のバージョン)、前の行を捨てる。
void write_shot_table(const QString &path, const QString &header, const QString &rows)
{
	QSet<QString> shots;
	for (const QString &line : rows.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
		shots.insert(line.section(QLatin1Char('\t'), 0, 0));
	QString kept;
	QFile old(path);
	if (old.open(QIODevice::ReadOnly)) {
		const QStringList lines = QString::fromUtf8(old.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
		old.close();
		if (!lines.isEmpty() && lines.front() + QLatin1Char('\n') == header) {
			for (qsizetype i = 1; i < lines.size(); ++i) {
				if (!shots.contains(lines.at(i).section(QLatin1Char('\t'), 0, 0)))
					kept += lines.at(i) + QLatin1Char('\n');
			}
		}
	}
	write_text_file(path, header + kept + rows);
}

} // namespace

void Writer::write_tables() const
{
	if (!metrics.isEmpty())
		write_text_file(dir + QStringLiteral("/metrics.tsv"), metrics);
	if (!targets.isEmpty())
		write_shot_table(dir + QStringLiteral("/targets.tsv"), QString::fromLatin1(kTargetsHeader), targets);
	if (!names.isEmpty())
		write_shot_table(dir + QStringLiteral("/names.tsv"), QString::fromLatin1(kNamesHeader), names);
	if (!contrast.isEmpty())
		write_shot_table(dir + QStringLiteral("/contrast.tsv"), QString::fromLatin1(kContrastHeader), contrast);
	if (!extra_name.isEmpty())
		write_text_file(dir + QStringLiteral("/") + extra_name, extra);
	if (!wraps.isEmpty())
		write_shot_table(dir + QStringLiteral("/wraps.tsv"), QString::fromLatin1(kWrapsHeader), wraps);
	if (!uninstall_entry.isEmpty())
		write_shot_table(dir + QStringLiteral("/uninstall-entry.tsv"),
				 QStringLiteral("shot\tbutton\tbutton_enabled\tconfirm_called\tbutton_in_view\tbelow_in_view\tbelow\n"),
				 uninstall_entry);
	if (!screen_fit.isEmpty())
		write_shot_table(dir + QStringLiteral("/screen-fit.tsv"), QString::fromLatin1(kScreenFitHeader), screen_fit);
	if (!fields.isEmpty())
		write_shot_table(dir + QStringLiteral("/fields.tsv"),
				 QStringLiteral("shot\tobject_name\twhat\ttext\ttext_px\troom_px\tfits\ttooltip_has_full\telided\n"),
				 fields);
	write_text_file(dir + QStringLiteral("/index.html"), index_html);
}

QImage grab_window(QWidget *window, QString *widgets)
{
	window->setAttribute(Qt::WA_DontShowOnScreen, true);
	window->show();
	settle();
	if (auto *focused = QApplication::focusWidget())
		focused->clearFocus();
	settle();
	const QImage image = grab_widget(window);
	int index = 0;
	dump_widgets(window, QPoint(0, 0), &index, widgets);
	window->hide();
	return image;
}

QImage grab_dock_with_menus(DockHost &host, const std::vector<QMenu *> &menus, QPushButton *anchor, QString *widgets,
			    std::vector<QPoint> *menu_origins)
{
	if (menus.empty())
		return QImage();
	QMenu *first = menus.front();
	first->setAttribute(Qt::WA_DontShowOnScreen, true);
	const QPoint pos = anchor != nullptr ? anchor->mapToGlobal(QPoint(0, anchor->height()))
					     : host.dock->mapToGlobal(QPoint(0, 0));
	first->popup(pos);
	settle();
	// サブメニューは、前のメニューの、そのサブメニューを持つ項目の右に開く (項目にマウスポインターを置いたときに
	// Qt が開く位置と同じ)。
	for (std::size_t i = 1; i < menus.size(); ++i) {
		QMenu *parent = menus[i - 1];
		QMenu *sub = menus[i];
		sub->setAttribute(Qt::WA_DontShowOnScreen, true);
		QAction *owner = nullptr;
		for (QAction *a : parent->actions()) {
			if (a->menu() == sub)
				owner = a;
		}
		if (owner != nullptr)
			parent->setActiveAction(owner);
		const QRect r = owner != nullptr ? parent->actionGeometry(owner) : QRect(0, 0, parent->width(), 0);
		if (!sub->isVisible())
			sub->popup(parent->mapToGlobal(r.topRight()));
		settle();
	}
	const QImage dock_image = grab_widget(host.dock.get());
	const qreal dpr = dock_image.devicePixelRatio();
	QRect canvas(QPoint(0, 0), host.dock->size());
	std::vector<QImage> images;
	std::vector<QRect> rects;
	for (QMenu *m : menus) {
		const QPoint at = host.dock->mapFromGlobal(m->pos());
		images.push_back(grab_widget(m));
		rects.push_back(QRect(at, m->size()));
		canvas = canvas.united(rects.back());
	}
	QImage image(QSize(qRound(canvas.width() * dpr), qRound(canvas.height() * dpr)), QImage::Format_RGB32);
	image.setDevicePixelRatio(dpr);
	image.fill(QColor(128, 128, 128));
	QPainter painter(&image);
	painter.drawImage(-canvas.topLeft(), dock_image);
	for (std::size_t i = 0; i < menus.size(); ++i)
		painter.drawImage(rects[i].topLeft() - canvas.topLeft(), images[i]);
	painter.end();
	int index = 0;
	dump_widgets(host.dock.get(), -canvas.topLeft(), &index, widgets);
	for (std::size_t i = 0; i < menus.size(); ++i) {
		dump_widgets(menus[i], rects[i].topLeft() - canvas.topLeft(), &index, widgets);
		if (menu_origins != nullptr)
			menu_origins->push_back(rects[i].topLeft() - canvas.topLeft());
	}
	for (auto it = menus.rbegin(); it != menus.rend(); ++it)
		(*it)->close();
	settle();
	return image;
}

double em_px_of(QWidget *w)
{
	return dock::ui::base_point_size(w) * w->logicalDpiY() / 72.0;
}

QString theme_dir_name(const QString &theme, const FaceOptions &o)
{
	QString name = theme;
	if (!o.obs_tag.isEmpty())
		name += QStringLiteral("-") + o.obs_tag;
	const Appearance def;
	if (o.appearance.font_scale != def.font_scale || o.appearance.density != def.density)
		name += QStringLiteral("-f%1-d%2").arg(o.appearance.font_scale).arg(o.appearance.density);
	if (o.dpr != 1.0)
		name += QStringLiteral("-x%1").arg(qRound(o.dpr * 100));
	return name;
}

namespace {

// 1 行の入力欄のテキストが入力欄に入るかの行 (fields.tsv。中身は下の定義のコメント)。
QString line_edit_rows(const QString &shot, QWidget *window);
// 折り返す QLabel の行と高さ (wraps.tsv の行。中身は下の定義のコメント)。
QString wrap_rows(const QString &shot, QWidget *window);

// ---- 描画の対象ごと ----

// 列の後ろに、measure.h の m1 から m8 の値が続く。
const char *kMetricsHeader = "scene\twidth_em\twidth\theight\tsize_reached\tmin_width\tfirst_line_h\tfirst_line_text\t"
			     "first_line_full\tfirst_line_elided\th_overflow\tv_scrollbar\tl1_body_place\tnotices\tpeople\t"
			     "still_scrolls\tm1_first_line_at_scroll_end\tm2_l1_buttons_at_scroll_end\t"
			     "m1_first_line_visible(top/mid/end)\tm2_l1_buttons_visible(top/mid/end)\tm3_people_visible_at_top\t"
			     "m4_min_shown_graphemes\tnames_elided\tnames_middle\tm5_clipped\tm7_targets_failing_2.5.8\t"
			     "m7_failing_kinds\trec_button_w\trec_button_bg\trec_button_text\tfirst_segment_kept\t"
			     "min_text_contrast\tglyph_edge_hits\tclipped_parts\tfirst_line_more\tglyph_clip_suspected\t"
			     "first_line_kind\tfirst_line_tier\tfirst_line_notice_key\tfirst_line_shown_form\tfirst_line_forms\n";

// metrics.tsv の first_line_forms の候補の区切り (U+001F。ini のテキストに入らない制御文字。render_sets.py が同じ文字で
// 分ける)。「 | 」のような見える文字は ini のテキストに入ることがあり、候補ごとに別の表にすると 1 行目の集計が 2 つの
// 表をつなぐことになるので、制御文字にする。
constexpr char16_t kFormsSeparator = 0x1F;

QString yn3(const bool v[3])
{
	QStringList parts;
	for (int i = 0; i < 3; ++i)
		parts << (v[i] ? QStringLiteral("yes") : QStringLiteral("NO"));
	return parts.join(QLatin1Char('/'));
}

bool id_selected(const QString &id, const QString &only)
{
	if (only.isEmpty())
		return true;
	// --only は , で区切って複数を渡せる (どれかの文字列を含む場面を選ぶ)。
	for (const QString &part : only.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
		if (id.contains(part))
			return true;
	}
	return false;
}

// 行の状態のテキスト (全文を持つ QLabel) が 1 つでも表示されているか。製品の判断 (rows::width_rules の status_text) は
// すべての行で同じなので、1 つで足りる。
bool row_status_text_shown(QWidget *root)
{
	for (QLabel *l : root->findChildren<QLabel *>(QStringLiteral("cordmuxRowStatusText"))) {
		auto *e = dynamic_cast<dock::ui::ElideLabel *>(l);
		if (e != nullptr && !e->full_text().isEmpty() && !e->isHidden())
			return true;
	}
	return false;
}

// 行の状態のテキストを表示する、いちばん狭いドックの幅 (--at-status-floor)。製品の境目 (status_text_min_width_px) は
// 行が使える幅で比べるので、ドックの幅から行の幅への換算 (余白とスクロールバー) をツールでまねず、製品の判断の結果
// (状態のテキストの QLabel が表示されているか) を、幅を変えて描画して読む。行の幅はドックの幅について減らないので、
// 二分探索で探す。lo は描画する最小の幅。hi で表示されていなければ (状態のテキストのない場面) 0 を返す。境目の px から
// 幅を計算しないのは、ドックの余白とスクロールバーの幅の計算をコピーすると、製品と同じ誤りを
// 持つおそれがあるためである。
int status_floor_width(DockHost &host, int lo, int height)
{
	constexpr int kHi = 900;
	render_dock(host, kHi, height, 600);
	if (!row_status_text_shown(host.widget))
		return 0;
	int hi = kHi;
	lo = std::max(1, lo);
	render_dock(host, lo, height, 600);
	if (row_status_text_shown(host.widget))
		return lo;
	while (hi - lo > 1) {
		const int mid = lo + (hi - lo) / 2;
		render_dock(host, mid, height, 600);
		if (row_status_text_shown(host.widget))
			hi = mid;
		else
			lo = mid;
	}
	return hi;
}

void face_dock(const FaceOptions &opt, Writer &w)
{
	w.metrics = QString::fromLatin1(kMetricsHeader);
	for (const Fixture &fixture : dock_fixtures()) {
		const QString id = QString::fromStdString(fixture.id);
		if (!id_selected(id, opt.only))
			continue;
		DockHost host = build_dock(fixture, opt.style);
		if (opt.tab.has_value()) {
			host.widget->set_tab(*opt.tab);
			host.widget->set_audio_levels(fixture.levels);
			// 状態タブを開くと、その時点のレベル 2 は既読になり、1 行目から消える (製品の DockPoll と同じ)。
			if (*opt.tab == cordmux::config::DockTab::Status) {
				host.view.first_line = first_line_for(fixture, true).first_line;
				host.widget->render(host.view);
			}
			settle();
		}
		w.index_html += QStringLiteral("<h2>%1</h2><p>%2</p>\n").arg(id, QString::fromStdString(fixture.note).toHtmlEscaped());
		const double em = em_px_of(host.widget);
		if (w.metrics.count(QLatin1Char('\n')) <= 1) {
			// テーマから読んだ色 (メッセージのレベル 1 のパネルに枠線を描画するかを決める元)。1 つの場面の
			// 分だけ書く。
			const dock::ui::ThemeRead t =
				dock::ui::read_theme(host.widget, static_cast<QWidget *>(obs_frontend_get_main_window()));
			const dock::ui::L1Surface l1 = dock::ui::l1_surface(t, t.base_bg, opt.style);
			print_line(QStringLiteral("theme read: base_bg=%1 text=%2 danger=%3 has_danger=%4 l1_border_px=%5 has_dots=%6 "
						  "has_desktop_cap=%7 em_px=%8")
					   .arg(t.base_bg.name(), t.text.name(), t.danger.name())
					   .arg(t.has_danger ? 1 : 0)
					   .arg(l1.border_px)
					   .arg(t.has_dots ? 1 : 0)
					   .arg(t.has_desktop_cap ? 1 : 0)
					   .arg(em, 0, 'f', 2));
		}
		struct Size {
			QString label;
			double em;
			int w;
			int h;
		};
		std::vector<Size> sizes;
		for (double e : opt.widths_em)
			sizes.push_back({QStringLiteral("%1em").arg(e), e, static_cast<int>(std::lround(e * em)), opt.height});
		for (const QSize &s : opt.sizes)
			sizes.push_back({QStringLiteral("%1x%2").arg(s.width()).arg(s.height()), s.width() / em, s.width(),
					 s.height()});
		if (opt.at_min_width) {
			// 製品が計算した最小幅。現在の大きさで計算した値なので、最初の大きさで 1 回描画してから読む。
			// 幅を狭くするとスクロールバーが表示され、その幅の分だけ最小幅が上がることがあるので、値が
			// 変わらなくなるまで描画し直して読む。
			render_dock(host, sizes.empty() ? 420 : sizes.front().w, opt.height, 600);
			int min_w = host.widget->computed_min_width();
			for (int i = 0; i < 4; ++i) {
				render_dock(host, min_w, opt.height, 600);
				const int again = host.widget->computed_min_width();
				if (again == min_w)
					break;
				min_w = again;
			}
			sizes.push_back({QStringLiteral("min"), min_w / em, min_w, opt.height});
		}
		if (opt.at_status_floor) {
			// 行の状態のテキストを表示する、いちばん狭い幅 (状態のテキストの QLabel は 5.75 em が下限)。
			// 探す範囲の下限は、製品が計算した最小幅。
			render_dock(host, sizes.empty() ? 420 : sizes.front().w, opt.height, 600);
			const int floor_w = status_floor_width(host, host.widget->computed_min_width(), opt.height);
			if (floor_w > 0) {
				sizes.push_back({QStringLiteral("floor"), floor_w / em, floor_w, opt.height});
				print_line(QStringLiteral("status floor: %1 %2 px").arg(id).arg(floor_w));
			}
		}
		for (const Size &s : sizes) {
			const DockShot shot = render_dock(host, s.w, s.h, 600);
			const QString name = id + QStringLiteral("-") + s.label;
			QString widgets;
			int index = 0;
			dump_widgets(host.dock.get(), QPoint(0, 0), &index, &widgets);
			w.shot(name, shot.image, widgets);

			const DockFacts f = measure_dock(host, shot.image);
			const std::vector<Wcag258> judged = judge_258(f.targets);
			w.targets += targets_rows(name, f.targets, judged);
			w.names += names_rows(name, f.names);
			// カードの場所の行 (読み取り専用の 1 行の入力欄) のテキストが、入力欄に入るか。
			w.fields += line_edit_rows(name, host.dock.get());
			w.wraps += wrap_rows(name, host.dock.get());
			const std::vector<ContrastFact> contrast = measure_contrast(host.dock.get(), shot.image, QPoint(0, 0));
			w.contrast += contrast_rows(name, contrast);
			const std::vector<EdgeFact> edges = glyph_edges(host.dock.get(), shot.image, QPoint(0, 0));
			if (!edges.empty()) {
				if (w.extra_name.isEmpty()) {
					w.extra_name = QStringLiteral("edges.tsv");
					w.extra = QString::fromLatin1(kEdgesHeader);
				}
				w.extra += edges_rows(name, edges);
			}

			int failing = 0;
			QStringList failing_kinds;
			for (std::size_t i = 0; i < f.targets.size(); ++i) {
				if (f.targets[i].visible && !judged[i].spacing_ok) {
					++failing;
					if (!failing_kinds.contains(f.targets[i].kind))
						failing_kinds << f.targets[i].kind;
				}
			}
			int min_shown = -1;
			int elided = 0;
			int middle = 0;
			for (const NameFact &n : f.names) {
				min_shown = min_shown < 0 ? n.shown_graphemes : std::min(min_shown, n.shown_graphemes);
				elided += n.elided ? 1 : 0;
				middle += n.middle ? 1 : 0;
			}
			double min_ratio = 0;
			for (const ContrastFact &c : contrast) {
				if (c.enabled)
					min_ratio = min_ratio == 0 ? c.ratio : std::min(min_ratio, c.ratio);
			}
			int edge_hits = 0;
			int clip_suspected = 0;
			for (const EdgeFact &e : edges) {
				edge_hits += e.top + e.bottom;
				clip_suspected += e.suspect ? 1 : 0;
			}

			const QStringList cells{
				name,
				QString::number(s.em, 'f', 2),
				QString::number(shot.width),
				QString::number(shot.height),
				shot.size_reached ? QStringLiteral("ok") : QStringLiteral("NOT-REACHED"),
				QString::number(f.min_width),
				QString::number(f.first_line_height),
				tsv_cell(f.first_line_text),
				tsv_cell(f.first_line_full),
				QString::number(f.first_line_elided ? 1 : 0),
				QString::number(f.h_overflow ? 1 : 0),
				QString::number(f.v_scrollbar ? 1 : 0),
				f.l1_body_place,
				QString::number(f.notices),
				QString::number(static_cast<int>(host.view.people.size())),
				QString::number(shot.still_scrolls ? 1 : 0),
				QString::number(f.first_line_visible[2] ? 1 : 0),
				f.l1_buttons == 0 ? QStringLiteral("-") : QString::number(f.l1_buttons_visible[2] ? 1 : 0),
				yn3(f.first_line_visible),
				f.l1_buttons == 0 ? QStringLiteral("-") : yn3(f.l1_buttons_visible),
				QStringLiteral("%1/%2").arg(f.visible_people).arg(f.people),
				QString::number(min_shown),
				QString::number(elided),
				QString::number(middle),
				QString::number(f.clipped.size()),
				QString::number(failing),
				failing_kinds.join(QLatin1Char(',')),
				QString::number(f.rec_button_width),
				f.rec_button_bg.name(),
				tsv_cell(f.rec_button_text),
				!f.first_line_has_segments ? QStringLiteral("-")
							   : (f.first_line_first_segment ? QStringLiteral("yes") : QStringLiteral("NO")),
				QString::number(min_ratio, 'f', 2),
				QString::number(edge_hits),
				tsv_cell(f.clipped.join(QStringLiteral("; "))),
				tsv_cell(f.first_line_more),
				QString::number(clip_suspected),
				f.first_line_kind,
				f.first_line_tier,
				tsv_cell(f.first_line_notice_key),
				QString::number(f.first_line_shown_form),
				tsv_cell(f.first_line_forms.join(QChar(kFormsSeparator)))};
			w.metrics += cells.join(QLatin1Char('\t')) + QLatin1Char('\n');
		}
	}
}

// ウィンドウとドックの 1 行の入力欄 (QLineEdit) ごとに、表示しているテキスト (テキストが空ならプレースホルダー) が、
// 入力欄の中のテキストの領域に入るか (fields.tsv の行)。保存先の入力欄のプレースホルダーは、空のまま使ったときに
// 書き込む場所なので、普通の長さの場所の末尾 (書き込み先のフォルダーの名前) が見えるかを数える。テキストの領域は
// Qt の QLineEdit と同じ求め方にする (SE_LineEditContents から textMargins を引き、さらに左右の 2 px の余白を引く。
// qlineedit_p.h の horizontalMargin)。入力欄がテキストを省略する形 (真ん中の省略) なら、表示しているテキストは省略した
// 後のもので (elided)、tooltip_has_full が全文の有無を表す。fits が no なら、テキストの右が入力欄で切れている
// (読み取り専用の入力欄はテキストを省略せず、選んでコピーできる形のまま、右が見えない)。
QString line_edit_rows(const QString &shot, QWidget *window)
{
	QString out;
	for (QLineEdit *edit : window->findChildren<QLineEdit *>()) {
		if (!edit->isVisibleTo(window) || qobject_cast<QSpinBox *>(edit->parentWidget()) != nullptr ||
		    qobject_cast<QComboBox *>(edit->parentWidget()) != nullptr)
			continue;
		const bool placeholder = edit->text().isEmpty();
		const QString text = placeholder ? edit->placeholderText() : edit->displayText();
		if (text.isEmpty())
			continue;
		QStyleOptionFrame option;
		option.initFrom(edit);
		option.rect = edit->rect();
		option.lineWidth = edit->hasFrame() ? edit->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, &option, edit) : 0;
		const QRect contents = edit->style()->subElementRect(QStyle::SE_LineEditContents, &option, edit)
					       .marginsRemoved(edit->textMargins());
		const int room = std::max(0, contents.width() - 2 * 2);
		const int text_px = edit->fontMetrics().horizontalAdvance(text);
		// 製品がプレースホルダーを省略して入れている (真ん中の省略。設定ウィンドウの保存先の入力欄) か。
		// 省略していれば、ツールチップが、省略したテキストの先頭と末尾を持つ全文かを見る (全文は製品の
		// 中にしかない)。
		const qsizetype ell = text.indexOf(QChar(0x2026));
		const bool elided = ell >= 0;
		const QString tip = edit->toolTip();
		const bool tip_full = elided ? tip.startsWith(text.left(ell)) && tip.endsWith(text.mid(ell + 1)) &&
						       tip.size() > text.size() - 1
					     : tip.contains(text);
		out += QStringList{shot,
				   tsv_cell(edit->objectName()),
				   placeholder ? QStringLiteral("placeholder") : QStringLiteral("text"),
				   tsv_cell(text),
				   QString::number(text_px),
				   QString::number(room),
				   text_px <= room ? QStringLiteral("yes") : QStringLiteral("no"),
				   tip_full ? QStringLiteral("yes") : QStringLiteral("no"),
				   elided ? QStringLiteral("yes") : QStringLiteral("no")}
			       .join(QLatin1Char('\t')) +
		       QLatin1Char('\n');
	}
	return out;
}

// テキストの書記素の数 (最後の行が 2 文字以下かを見るため。数え方は WrapLabel と同じく、前後の空白を除いて数える)。
int graphemes_of(const QString &text)
{
	QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
	int count = 0;
	while (finder.toNextBoundary() > 0)
		++count;
	return count;
}

// テキストがフォルダーかファイルの場所か (ドライブの「C:\」か、ネットワークの「\\」で始まる)。
bool looks_like_path(const QString &text)
{
	return (text.size() >= 3 && text.at(0).isLetter() && text.at(1) == QLatin1Char(':') && text.at(2) == QLatin1Char('\\')) ||
	       text.startsWith(QStringLiteral("\\\\"));
}

// コピーしたときに場所に混ざる、目に見えない文字 (改行の位置を示すのに使われることがあるもの)。
// allow_line_separator なら、行の区切り (U+2028) は数えない。改行の位置を自分で決める WrapLabel (動的な
// プロパティ sourceText を持つ) は、表示するテキストに U+2028 を入れ、コピーするテキストからは
// 除くためである (dock/ui/wrap_label.h のコメント)。[コピー] の動作は製品のテストが確認する。プレビューツールは、
// 実行した PC のクリップボードを使わない。
bool has_invisible_break(const QString &text, bool allow_line_separator)
{
	for (const QChar c : text) {
		const char16_t u = c.unicode();
		if (u == 0x2028 && allow_line_separator)
			continue;
		if (u == 0x200B || u == 0x2028 || u == 0x2029 || u == 0x00AD || u == 0x2060 || u == 0xFEFF)
			return true;
	}
	return false;
}

} // namespace

const char *const kWrapsHeader = "shot\tobject_name\tclass\tnotice_key\tpath\ttext\tx\ty\tw\th\tvisible_h\thfw\twrap_w\tline_px\t"
				 "lines\ttext_h\tvalign\tlast_line\tlast_graphemes\tbreaks\telided\tcopy_clean\twrap\ttext_px\tinner_h\tqt_lines\t"
				 "break_spaces\n";

namespace {

// ウィンドウとドックの折り返す QLabel (wordWrap) ごとに、Qt の折り返し方をまねて組んだ行と、QLabel の高さを書く
// (wraps.tsv の行)。集計は render_sets.py が行う (数と単位の語を行で分けない、最後の行を 2 文字以下にしない、QLabel が
// テキストより高くない、場所の全文が見えてコピーできる)。
//   - 行は、QLabel のテキストを、テキストの幅 (contentsRect から margin を引いた幅。WrapLabel は詰める幅と箇条書きの
//     字下げを余白で持つ) で QTextLayout の WordWrap に組んだもの (QLabel がテキストを描画する qt_format_text と同じ
//     折り返し方)。
//   - hfw は QLabel が自分で返す高さ (heightForWidth(width()))。text_h は組んだ行の高さの和。h と比べて、QLabel が
//     テキストより高い (下に空きがある) か、低い (下が切れる) かを見る。visible_h は、親 (スクロールの範囲など) で
//     切られた後の見える高さ。
//   - breaks は、行の境ごとの「前の行の末尾 3 文字 / 次の行の先頭 3 文字」を「 ; 」でつないだもの (数と単位が分かれた
//     かは、読む側で見る)。
//   - break_spaces は breaks と同じ並びで、境ごとに空白をはさんだか (前の行の末尾か次の行の先頭に空白があれば 1、
//     なければ 0) を「 ; 」でつないだもの。breaks は両側を trimmed() してから書くので空白が消え、空白をはさむ 2 つの数
//     (セッションのフォルダーの名前「2000-01-01 」/「12-00-00」) を、数の途中で分かれたと読み違えるためである。空白は、
//     行区切り文字 (U+2028・U+2029) を除いてから見る (WrapLabel は空白の後に U+2028 を入れる)。列は末尾に追加し、
//     前の列の並びを変えない (render_sets.py は列の名前で読むので、この列のない前の表も読める)。breaks のテキストに
//     空白を残さないのは、書式が変わり、render_sets.py が参考として書く en の「2 / people」のテキストも変わるため
//     である。
//   - path はテキストが場所か。elided はテキストに省略記号 (U+2026) があるか。copy_clean は、場所の QLabel の
//     テキストに目に見えない文字がなく、アクセシブル名があればそれと同じか (選んでコピーすると場所の全文になる)。
//     wrap は QLabel が折り返すか。text_px は、組んだ行のうちいちばん長い行のテキストの幅 (折り返さない
//     QLabel では、wrap_w を超えれば右が切れている)。
//   - 折り返さない QLabel は、テキストが場所のものだけを書く (場所の全文が見えるかを同じ表で数える)。
//   - inner_h は、スクロールの見える範囲より内側の親 (カードなど) だけで切り取った後の高さ (スクロールすれば見える
//     行は、切れていない)。
//   - qt_lines は、改行の位置を自分で決める QLabel (sourceText を持つ WrapLabel) の元のテキストを、Qt の折り返し方で
//     同じ幅に組んだ行の数。
// ウィンドウを隠した後に呼び出すので、見える範囲は親の矩形を重ねて自分で求める (visibleRegion は隠すと空になる)。
QString wrap_rows(const QString &shot, QWidget *window)
{
	QString out;
	for (QLabel *label : window->findChildren<QLabel *>()) {
		if (!label->isVisibleTo(window) || label->text().isEmpty() || label->window() != window->window())
			continue;
		// 折り返すすべての QLabel と、折り返さない QLabel のうちテキストが場所のもの (場所の全文が見えるかを
		// 同じ表で数える)。
		const bool wraps_text = label->wordWrap();
		if (!wraps_text && !looks_like_path(label->text()) && !looks_like_path(label->accessibleName()))
			continue;
		const Qt::TextFormat format = label->textFormat();
		if (format == Qt::RichText || format == Qt::MarkdownText ||
		    (format == Qt::AutoText && Qt::mightBeRichText(label->text())))
			continue;
		const QString text = label->text();
		const QPoint at = label->mapTo(window, QPoint(0, 0));
		QRect visible(at, label->size());
		// inner は、スクロールの見える範囲 (qt_scrollarea_viewport) より内側の親だけで切り取った
		// 矩形 (スクロールすれば見える行を、切れているとは数えない)。
		QRect inner = visible;
		bool past_viewport = false;
		for (QWidget *p = label->parentWidget(); p != nullptr && p != window; p = p->parentWidget()) {
			const QRect r(p->mapTo(window, QPoint(0, 0)), p->size());
			if (p->objectName() == QLatin1String("qt_scrollarea_viewport"))
				past_viewport = true;
			visible &= r;
			if (!past_viewport)
				inner &= r;
		}
		visible &= window->rect();
		if (!past_viewport)
			inner &= window->rect();
		const int wrap_w = std::max(1, label->contentsRect().width() - 2 * label->margin());
		QTextLayout layout(text, label->font());
		QTextOption option;
		option.setWrapMode(wraps_text ? QTextOption::WordWrap : QTextOption::NoWrap);
		layout.setTextOption(option);
		QStringList lines;
		double text_h = 0.0;
		double text_px = 0.0;
		layout.beginLayout();
		for (;;) {
			QTextLine line = layout.createLine();
			if (!line.isValid())
				break;
			line.setLineWidth(wraps_text ? wrap_w : 1000000);
			lines << text.mid(line.textStart(), line.textLength());
			text_h += line.height();
			text_px = std::max(text_px, line.naturalTextWidth());
		}
		layout.endLayout();
		QStringList breaks;
		QStringList break_spaces;
		const auto without_marks = [](QString line) {
			line.remove(QChar(QChar::LineSeparator));
			line.remove(QChar(QChar::ParagraphSeparator));
			return line;
		};
		for (qsizetype i = 0; i + 1 < lines.size(); ++i) {
			const QString a = lines.at(i).trimmed();
			const QString b = lines.at(i + 1).trimmed();
			breaks << a.right(3) + QStringLiteral(" / ") + b.left(3);
			const QString raw_a = without_marks(lines.at(i));
			const QString raw_b = without_marks(lines.at(i + 1));
			const bool spaced =
				(!raw_a.isEmpty() && raw_a.back().isSpace()) || (!raw_b.isEmpty() && raw_b.front().isSpace());
			break_spaces << (spaced ? QStringLiteral("1") : QStringLiteral("0"));
		}
		const QString last = lines.isEmpty() ? QString() : lines.last().trimmed();
		QString notice_key;
		for (QWidget *p = label; p != nullptr && p != window; p = p->parentWidget()) {
			const QVariant v = p->property("noticeKey");
			if (v.isValid()) {
				notice_key = v.toString();
				break;
			}
		}
		const bool path = looks_like_path(QString(text).remove(QChar(0x200B))) || looks_like_path(label->accessibleName());
		// コピーするテキスト。改行の位置を自分で決める QLabel では、元のテキスト (sourceText) と、
		// 行区切り文字を除いた表示のテキストが同じで、アクセシブル名も同じになる。
		const QVariant source = label->property("sourceText");
		const bool managed = source.isValid();
		const QString copied = managed ? QString(text).remove(QChar(QChar::LineSeparator)) : text;
		// 改行の位置を自分で決める QLabel が、Qt の折り返し方 (元のテキストを同じ幅で) より行を
		// 増やしていないか (行を増やすとカードの高さが変わる)。qt_lines は、元のテキストを Qt の折り返し方で
		// 組んだ行の数 (位置を自分で決めない QLabel では lines と同じ)。
		int qt_lines = lines.size();
		if (managed && wraps_text) {
			QTextLayout plain(source.toString(), label->font());
			plain.setTextOption(option);
			plain.beginLayout();
			qt_lines = 0;
			for (QTextLine line = plain.createLine(); line.isValid(); line = plain.createLine()) {
				line.setLineWidth(wrap_w);
				++qt_lines;
			}
			plain.endLayout();
		}
		const bool clean = !has_invisible_break(text, managed) && (!managed || copied == source.toString()) &&
				   (label->accessibleName().isEmpty() || label->accessibleName() == copied);
		const Qt::Alignment v = label->alignment() & Qt::AlignVertical_Mask;
		out += QStringList{shot,
				   tsv_cell(label->objectName()),
				   QString::fromLatin1(label->metaObject()->className()),
				   tsv_cell(notice_key),
				   path ? QStringLiteral("yes") : QStringLiteral("no"),
				   tsv_cell(text),
				   QString::number(at.x()),
				   QString::number(at.y()),
				   QString::number(label->width()),
				   QString::number(label->height()),
				   QString::number(visible.isValid() ? visible.height() : 0),
				   QString::number(label->heightForWidth(label->width())),
				   QString::number(wrap_w),
				   QString::number(label->fontMetrics().lineSpacing()),
				   QString::number(lines.size()),
				   QString::number(qCeil(text_h)),
				   v == Qt::AlignTop ? QStringLiteral("top")
						     : (v == Qt::AlignBottom ? QStringLiteral("bottom") : QStringLiteral("vcenter")),
				   tsv_cell(last),
				   QString::number(lines.size() < 2 ? 0 : graphemes_of(last)),
				   tsv_cell(breaks.join(QStringLiteral(" ; "))),
				   text.contains(QChar(0x2026)) ? QStringLiteral("yes") : QStringLiteral("no"),
				   path ? (clean ? QStringLiteral("yes") : QStringLiteral("no")) : QString(),
				   wraps_text ? QStringLiteral("yes") : QStringLiteral("no"),
				   QString::number(qCeil(text_px)),
				   QString::number(inner.isValid() ? inner.height() : 0),
				   QString::number(qt_lines),
				   break_spaces.join(QStringLiteral(" ; "))}
			       .join(QLatin1Char('\t')) +
		       QLatin1Char('\n');
	}
	return out;
}

// ウィンドウの描画に使う View (「バージョン情報」、参加者の設定ウィンドウ、アンインストールの結果) の元の場面。
QString window_scene(const FaceOptions &opt, const char *fallback)
{
	// ウィンドウの描画は場面を 1 つだけ使う (--only に , で複数を渡したときは最初の 1 つ)。
	return opt.only.isEmpty() ? QString::fromLatin1(fallback) : opt.only.section(QLatin1Char(','), 0, 0);
}

#if CORDMUX_UI_PREVIEW_HAS_WINDOWS
// 再起動が必要な設定の値を 1 つ変える (利用者が変えたのと同じく、ウィジェットの値を変えると、ウィンドウのシグナルで
// 再起動の 1 文が表示される)。現在のページに見えている入力欄があればそれを、なければウィンドウの中の最初の入力欄を
// 変える。変えた入力欄の objectName を返す。
QString change_restart_field(QWidget *window)
{
	QWidget *target = nullptr;
	for (QWidget *c : window->findChildren<QWidget *>()) {
		if (!c->property("restartRequired").toBool())
			continue;
		if (target == nullptr || (c->isVisibleTo(window) && !target->isVisibleTo(window)))
			target = c;
	}
	if (target == nullptr)
		return QString();
	if (auto *combo = qobject_cast<QComboBox *>(target))
		combo->setCurrentIndex((combo->currentIndex() + 1) % std::max(1, combo->count()));
	else if (auto *spin = qobject_cast<QSpinBox *>(target))
		spin->setValue(spin->value() == spin->maximum() ? spin->value() - 1 : spin->value() + 1);
	else if (auto *check = qobject_cast<QCheckBox *>(target))
		check->setChecked(!check->isChecked());
	settle();
	return target->objectName().isEmpty() ? QString::fromLatin1(target->metaObject()->className()) : target->objectName();
}
#endif

// [ツール] の「cordmuxをアンインストール...」を選んだ後のウィンドウ (uninstall-entry.tsv の行)。ページの
// [cordmuxをアンインストール...] (テキストで探す) と、その下の QLabel (押せない理由) が、スクロールの見える範囲に
// 入っているか。確認のダイアログを開く関数が呼ばれたか。
QString uninstall_entry_row(const QString &shot, QWidget *window, bool confirm_called)
{
	const QString label = QString::fromUtf8(obs_module_text(ledger::text::keys::kSettingsUninstallButton));
	QAbstractButton *button = nullptr;
	for (QAbstractButton *b : window->findChildren<QAbstractButton *>()) {
		if (b->isVisibleTo(window) && b->text() == label)
			button = b;
	}
	QString in_view = QStringLiteral("-");
	QString below_in_view = QStringLiteral("-");
	QStringList below_texts;
	QAbstractScrollArea *area = nullptr;
	for (QWidget *p = button != nullptr ? button->parentWidget() : nullptr; p != nullptr && area == nullptr;
	     p = p->parentWidget())
		area = qobject_cast<QAbstractScrollArea *>(p);
	auto *scroll = qobject_cast<QScrollArea *>(area);
	if (button != nullptr && scroll != nullptr && scroll->widget() != nullptr) {
		const QRect view(scroll->viewport()->mapTo(window, QPoint(0, 0)), scroll->viewport()->size());
		const QRect br(button->mapTo(window, QPoint(0, 0)), button->size());
		in_view = view.contains(br) ? QStringLiteral("yes") : QStringLiteral("NO");
		bool all_in = true;
		for (QLabel *l : scroll->widget()->findChildren<QLabel *>()) {
			const QRect lr(l->mapTo(window, QPoint(0, 0)), l->size());
			if (!l->isVisibleTo(window) || l->text().trimmed().isEmpty() || lr.top() < br.bottom())
				continue;
			below_texts << l->text().left(24);
			all_in = all_in && view.contains(lr);
		}
		below_in_view = below_texts.isEmpty() ? QStringLiteral("-") : (all_in ? QStringLiteral("yes") : QStringLiteral("NO"));
	}
	return QStringList{shot,
			   button != nullptr ? QStringLiteral("yes") : QStringLiteral("NO"),
			   button != nullptr && button->isEnabled() ? QStringLiteral("yes") : QStringLiteral("NO"),
			   confirm_called ? QStringLiteral("yes") : QStringLiteral("NO"),
			   in_view,
			   below_in_view,
			   tsv_cell(below_texts.join(QStringLiteral(" / ")))}
		       .join(QLatin1Char('\t')) +
	       QLatin1Char('\n');
}

// ---- ウィンドウが画面の作業領域に収まるか (screen-fit.tsv) ----
//
// ウィンドウ (確認・結果のダイアログ、設定ウィンドウ、参加者の設定ウィンドウ) を、ウィンドウの枠の上端を作業領域の
// 上端に置いた形 (Windows がウィンドウを置ける最善の位置) で数える。1 枚の画像に 1 行。
//   - frame: ウィンドウの枠 (タイトルバーと上下の枠) の高さ。QWindow::frameMargins の上と下の和で、測れなければ 40 px
//     (製品の dock/ui/message_dialog.cpp の kFrameFallbackPx と同じ値。プレビューのプラグインは枠を測れない)。
//   - fits: ウィンドウの高さ + frame <= 作業領域の高さ、かつ ウィンドウの幅 <= 作業領域の幅。
//   - buttons / buttons_out: 本文の外の押せるもの (ダイアログの [OK]・[キャンセル]・[適用] など)。下端 + frame が
//     作業領域の高さを超えるか、右端が作業領域の幅を超えるか、ウィンドウの中で切れていれば外と数える (押せない)。
//   - rows / rows_unreachable: 本文の行 (テキストの QLabel・押せるもの・入力欄・一覧。ウィジェットの中のウィジェットは
//     数えない)。スクロール領域の中の行は、中身の中にあり、いちばん下までスクロールしたときに画面の中の見える範囲まで
//     届くかを見る。領域の外の行は、ウィンドウの中で切れず、画面の中にあるかを見る。届かない行の名前を unreachable に
//     書く。
//   - rows_taller_than_view: スクロール領域の見える範囲 (画面の中の部分) より高い行 (参考の値。スクロールすれば上と
//     下を分けて読める)。
//   - scrolls / view_h / content_h: 縦にスクロールする領域 (一覧の QListWidget を除く QScrollArea) が見えていて
//     スクロールできるか、その見える範囲と中身の高さ (いちばん大きい領域)。ダイアログのスクロール領域
//     (cordmuxMessageScroll) もここに入る。
// ボタンが作業領域の外にあるときと、本文の行に届けないときを失敗にする (ダイアログの [OK] はいつも見え、スクロール
// するのは本文だけ、という製品の動作を確認する)。ウィンドウの高さだけでは足りない。設定ウィンドウは作業領域の 85% で
// 大きさが止まり、ページの中をスクロールするので、行に届くかが分からないためである。widgets.tsv を読む Python の
// ツールで集計しないのは、スクロールの中身の高さと範囲が表になく、見える範囲の外の行と届かない行を見分けられない
// ためである。
// 表の見出し kScreenFitHeader は、Writer::write_tables の前 (kWidgetsHeader の後) に置いた。

// 数えるウィジェットか (行の単位)。入力欄の中の QLineEdit のような、ウィジェットの中のウィジェットは、外の
// ウィジェットを数えたら数えない。
bool fit_counted(QWidget *w)
{
	if (qobject_cast<QScrollBar *>(w) != nullptr)
		return false;
	if (auto *label = qobject_cast<QLabel *>(w))
		return !label->text().trimmed().isEmpty() || !label->pixmap().isNull();
	return qobject_cast<QAbstractButton *>(w) != nullptr || qobject_cast<QLineEdit *>(w) != nullptr ||
	       qobject_cast<QComboBox *>(w) != nullptr || qobject_cast<QAbstractSpinBox *>(w) != nullptr ||
	       qobject_cast<QAbstractItemView *>(w) != nullptr || qobject_cast<QTextEdit *>(w) != nullptr ||
	       qobject_cast<QPlainTextEdit *>(w) != nullptr || qobject_cast<QSlider *>(w) != nullptr ||
	       qobject_cast<QProgressBar *>(w) != nullptr;
}

// ウィンドウの中の、見えていて縦にスクロールできる領域 (QScrollArea。一覧の QListWidget は QAbstractScrollArea なので
// 入らない)。
std::vector<QScrollArea *> fit_scrolls(QWidget *window)
{
	std::vector<QScrollArea *> out;
	for (QScrollArea *s : window->findChildren<QScrollArea *>()) {
		if (s->window() == window && s->isVisibleTo(window) && s->widget() != nullptr &&
		    s->verticalScrollBar()->maximum() > 0)
			out.push_back(s);
	}
	return out;
}

// ウィジェットの矩形 (ウィンドウの座標) を、stop までの祖先の矩形で切り取ったもの (stop は含めない)。
QRect fit_clipped(QWidget *w, QWidget *stop, QWidget *window)
{
	QRect r(w->mapTo(window, QPoint(0, 0)), w->size());
	for (QWidget *p = w->parentWidget(); p != nullptr && p != stop; p = p->parentWidget()) {
		r &= QRect(p == window ? QPoint(0, 0) : p->mapTo(window, QPoint(0, 0)), p->size());
		if (p == window)
			break;
	}
	return r;
}

QString fit_name(QWidget *w)
{
	QString text;
	if (auto *label = qobject_cast<QLabel *>(w))
		text = label->text();
	else if (auto *button = qobject_cast<QAbstractButton *>(w))
		text = button->text();
	text = text.simplified().left(20);
	const QString name = w->objectName().isEmpty() ? QString::fromLatin1(w->metaObject()->className()) : w->objectName();
	return text.isEmpty() ? name : name + QLatin1Char(' ') + text;
}

QString screen_fit_row(const QString &shot, QWidget *window, const FaceOptions &opt)
{
	const QScreen *screen = window->screen();
	const QRect avail = screen != nullptr ? screen->availableGeometry() : QRect();
	int frame = 0;
	if (QWindow *handle = window->windowHandle()) {
		const QMargins fm = handle->frameMargins();
		frame = fm.top() + fm.bottom();
	}
	if (frame <= 0)
		frame = 40; // dock/ui/message_dialog.cpp の kFrameFallbackPx
	const int limit_h = avail.height() - frame; // ウィンドウの中の座標で、画面の中に見える下の端 (この値を含まない)
	const int limit_w = avail.width();
	const bool fits = window->height() + frame <= avail.height() && window->width() <= avail.width();

	const std::vector<QScrollArea *> scrolls = fit_scrolls(window);
	int view_h = 0;
	int content_h = 0;
	for (QScrollArea *s : scrolls) {
		if (s->widget()->height() > content_h) {
			content_h = s->widget()->height();
			view_h = s->viewport()->height();
		}
	}

	int buttons = 0, buttons_out = 0, rows = 0, rows_bad = 0, rows_tall = 0;
	QStringList bad_names;
	QList<QWidget *> todo{window};
	while (!todo.isEmpty()) {
		QWidget *parent = todo.takeFirst();
		for (QObject *child : parent->children()) {
			auto *w = qobject_cast<QWidget *>(child);
			if (w == nullptr || w->isWindow() || !w->isVisibleTo(window))
				continue;
			if (!fit_counted(w)) {
				todo.push_back(w);
				continue;
			}
			// いちばん近いスクロール領域 (その中身の中にあるときだけ)。
			QScrollArea *area = nullptr;
			for (QWidget *p = w->parentWidget(); p != nullptr && p != window && area == nullptr; p = p->parentWidget()) {
				auto *s = qobject_cast<QScrollArea *>(p);
				if (s != nullptr && s->widget() != nullptr && s->widget()->isAncestorOf(w))
					area = s;
			}
			bool in_body = area != nullptr;
			for (QWidget *p = w->parentWidget(); p != nullptr && p != window && !in_body; p = p->parentWidget())
				in_body = p->objectName() == QLatin1String("cordmuxMessageBody");
			const bool is_button = qobject_cast<QAbstractButton *>(w) != nullptr && !in_body;
			bool ok = true;
			if (area != nullptr) {
				QWidget *content = area->widget();
				const QRect rc(w->mapTo(content, QPoint(0, 0)), w->size());
				const QRect clipped = fit_clipped(w, content, window);
				const QRect vr(area->viewport()->mapTo(window, QPoint(0, 0)), area->viewport()->size());
				const int visible_h = std::min(vr.bottom() + 1, limit_h) - vr.top();
				const int max_scroll = std::max(0, content->height() - vr.height());
				const int room_w = area->horizontalScrollBar()->maximum() > 0 ? content->width() : vr.width();
				ok = clipped.size() == w->size() && rc.top() >= 0 && rc.left() >= 0 && visible_h > 0 &&
				     rc.bottom() + 1 <= max_scroll + visible_h && rc.bottom() + 1 <= content->height() &&
				     rc.right() + 1 <= room_w && vr.right() + 1 <= limit_w;
				if (ok && rc.height() > visible_h)
					++rows_tall;
			} else {
				const QRect r(w->mapTo(window, QPoint(0, 0)), w->size());
				ok = fit_clipped(w, window, window) == r && r.bottom() + 1 <= limit_h && r.right() + 1 <= limit_w;
			}
			if (is_button) {
				++buttons;
				if (!ok) {
					++buttons_out;
					bad_names << QStringLiteral("button ") + fit_name(w);
				}
			} else {
				++rows;
				if (!ok) {
					++rows_bad;
					bad_names << fit_name(w);
				}
			}
		}
	}
	return QStringList{shot,
			   opt.screen_tag.isEmpty() ? QStringLiteral("1920x1080@100") : opt.screen_tag,
			   QString::number(avail.width()),
			   QString::number(avail.height()),
			   QString::number(frame),
			   QString::number(window->width()),
			   QString::number(window->height()),
			   fits ? QStringLiteral("yes") : QStringLiteral("NO"),
			   scrolls.empty() ? QStringLiteral("no") : QStringLiteral("yes"),
			   QString::number(view_h),
			   QString::number(content_h),
			   QString::number(buttons),
			   QString::number(buttons_out),
			   QString::number(rows),
			   QString::number(rows_bad),
			   QString::number(rows_tall),
			   tsv_cell(bad_names.mid(0, 12).join(QStringLiteral(" / ")))}
		       .join(QLatin1Char('\t')) +
	       QLatin1Char('\n');
}

// 画面を指定したとき (--screen)、ウィンドウの中にスクロールできる領域があれば、すべての領域をいちばん下までスクロール
// した画像も描画する (名前の先頭は bottom-。スクロールの下の端の行とボタンが同じ画像に見えるかを、目で確認するため)。
// 描画の後に、スクロールを先頭へ戻す。既定の画面では描画しない (既定の画面の画像のセットを変えないため)。
void shot_scrolled_bottom(const FaceOptions &opt, Writer &w, const QString &name, QWidget *window)
{
	if (opt.screen_tag.isEmpty() || fit_scrolls(window).empty())
		return;
	window->setAttribute(Qt::WA_DontShowOnScreen, true);
	window->show();
	settle();
	for (QScrollArea *s : fit_scrolls(window))
		s->verticalScrollBar()->setValue(s->verticalScrollBar()->maximum());
	if (auto *focused = QApplication::focusWidget())
		focused->clearFocus();
	settle();
	const QImage image = grab_widget(window);
	QString widgets;
	int index = 0;
	dump_widgets(window, QPoint(0, 0), &index, &widgets);
	window->hide();
	for (QScrollArea *s : fit_scrolls(window))
		s->verticalScrollBar()->setValue(0);
	w.shot(QStringLiteral("bottom-") + name, image, widgets);
}

bool face_settings(const FaceOptions &opt, const QString &page_name, const QString &variant, Writer &w)
{
#if CORDMUX_UI_PREVIEW_HAS_WINDOWS
	struct Page {
		const char *name;
		ledger::SettingsPage page;
	};
	static const Page kPages[] = {{"recording", ledger::SettingsPage::Recording},
				      {"receive", ledger::SettingsPage::Receive},
				      {"advanced", ledger::SettingsPage::Advanced},
				      {"about", ledger::SettingsPage::About}};
	for (const Page &p : kPages) {
		if (page_name != QLatin1String("all") && page_name != QLatin1String(p.name))
			continue;
		const QString scene = window_scene(opt, "settings-default");
		const Fixture fixture = fixture_by_id(scene.toStdString());
		const ledger::dockview::DockViewV2 view = view_for(fixture);
		dock::SettingsWindowInput in;
		in.current = fixture.settings;
		in.effective_at_start = fixture.settings;
		in.page = p.page;
		in.about = view.about;
		in.record_dir_now = [empty_dir = fixture.record_dir_default](const std::string &setting) {
			return setting.empty() ? empty_dir : setting;
		};
		in.export_busy = variant == QLatin1String("busy");
		// [ツール] の「cordmuxをアンインストール...」: 確認のダイアログを開く関数が呼ばれたかを
		// 数える (プレビューは確認のダイアログを開かない。押せる状態なら呼ばれ、押せない状態なら呼ばれない)。
		auto confirm_called = std::make_shared<bool>(false);
		in.on_uninstall = [confirm_called](QWidget *) { *confirm_called = true; };
		std::unique_ptr<QWidget> window(dock::make_settings_window(nullptr, in));
		QString name = QStringLiteral("settings-") + QString::fromLatin1(p.name);
		if (scene != QLatin1String("settings-default"))
			name += QStringLiteral("-") + scene;
		if (variant == QLatin1String("uninstall")) {
			// 製品の SettingsWindow::start_uninstall ([ツール] の項目が呼び出す関数) を、ウィンドウを
			// 表示した後に呼び出す。
			window->setAttribute(Qt::WA_DontShowOnScreen, true);
			window->show();
			settle();
			auto *sw = dynamic_cast<dock::SettingsWindow *>(window.get());
			if (sw != nullptr)
				sw->start_uninstall();
			settle();
			name += QStringLiteral("-tools-uninstall");
		} else if (variant == QLatin1String("restart")) {
			window->setAttribute(Qt::WA_DontShowOnScreen, true);
			window->show();
			settle();
			const QString changed = change_restart_field(window.get());
			print_line(QStringLiteral("settings %1: changed restart field %2").arg(QString::fromLatin1(p.name), changed));
			name += QStringLiteral("-restart");
		} else if (!variant.isEmpty()) {
			name += QStringLiteral("-") + variant;
		}
		QString widgets;
		const QImage image = grab_window(window.get(), &widgets);
		w.shot(name, image, widgets);
		w.screen_fit += screen_fit_row(name, window.get(), opt);
		w.fields += line_edit_rows(name, window.get());
		w.wraps += wrap_rows(name, window.get());
		if (variant == QLatin1String("uninstall"))
			w.uninstall_entry += uninstall_entry_row(name, window.get(), *confirm_called);
		// ページの一覧の項目の矩形を list-items.tsv に書く (画像の上で押された位置から、どのページの項目かを
		// 決めるため)。widgets.tsv には項目の行を追加しない (widgets.tsv を読むツールが受け取る行を
		// 変えないため)。
		if (w.extra_name.isEmpty()) {
			w.extra_name = QStringLiteral("list-items.tsv");
			w.extra = QStringLiteral("shot\tlist\trow\ttext\trect\tselected\n");
		}
		for (QListWidget *list : window->findChildren<QListWidget *>()) {
			if (!list->isVisibleTo(window.get()))
				continue;
			for (int row = 0; row < list->count(); ++row) {
				QListWidgetItem *item = list->item(row);
				const QRect r = list->visualItemRect(item);
				const QPoint at = list->viewport()->mapTo(window.get(), r.topLeft());
				w.extra += QStringList{name, tsv_cell(list->objectName()), QString::number(row), tsv_cell(item->text()),
						       rect_cell(QRect(at, r.size())), item->isSelected() ? QStringLiteral("1") : QStringLiteral("0")}
						   .join(QLatin1Char('\t')) +
					   QLatin1Char('\n');
			}
		}
		std::vector<TargetFact> targets = collect_targets(window.get(), QPoint(0, 0));
		w.targets += targets_rows(name, targets, judge_258(targets));
		w.contrast += contrast_rows(name, measure_contrast(window.get(), image, QPoint(0, 0)));
		shot_scrolled_bottom(opt, w, name, window.get());
	}
	return true;
#else
	(void)opt;
	(void)page_name;
	(void)variant;
	(void)w;
	return false;
#endif
}

bool face_participants(const FaceOptions &opt, Writer &w)
{
#if CORDMUX_UI_PREVIEW_HAS_WINDOWS
	const Fixture fixture = fixture_by_id(window_scene(opt, "seen-several").toStdString());
	const ledger::dockview::DockViewV2 view = view_for(fixture);
	// 覚えている並べ替えを既定に戻してから、場面の並べ替えを選ぶ (場面ごとに同じ初期状態から描く)。
	dock::ParticipantsWindow::reset_remembered_sort();
	std::unique_ptr<dock::ParticipantsWindow> window(dock::make_participants_window(nullptr, view.participants, nullptr));
	if (fixture.participants_sort_column >= 0)
		window->set_sort(static_cast<dock::ParticipantsWindow::Column>(fixture.participants_sort_column),
				 fixture.participants_sort_ascending);
	dock::ParticipantsWindow::reset_remembered_sort();
	QString widgets;
	const QImage image = grab_window(window.get(), &widgets);
	const QString name = QStringLiteral("participants-") + QString::fromStdString(fixture.id);
	w.shot(name, image, widgets);
	// 製品が許す最小の幅 (ウィンドウの minimumSizeHint の幅。高さは初めの大きさのまま) でも描く (--at-min-width。名前の後ろは
	// -min)。列の見出しの幅の下限が守られているかを見る。
	if (opt.at_min_width) {
		const QSize initial = window->size();
		window->resize(window->minimumSizeHint().width(), initial.height());
		QString min_widgets;
		const QImage min_image = grab_window(window.get(), &min_widgets);
		w.shot(name + QStringLiteral("-min"), min_image, min_widgets);
		window->resize(initial);
	}
	w.screen_fit += screen_fit_row(name, window.get(), opt);
	w.fields += line_edit_rows(name, window.get());
	w.wraps += wrap_rows(name, window.get());
	std::vector<TargetFact> targets = collect_targets(window.get(), QPoint(0, 0));
	w.targets += targets_rows(name, targets, judge_258(targets));
	w.contrast += contrast_rows(name, measure_contrast(window.get(), image, QPoint(0, 0)));
	shot_scrolled_bottom(opt, w, name, window.get());
	return true;
#else
	(void)opt;
	(void)w;
	return false;
#endif
}

#if CORDMUX_UI_PREVIEW_HAS_INSTALL_UI
// 手順書と違う置き方の完了のダイアログに渡す、利用者に削除してもらうファイル。製品は、実行中の DLL のフォルダーと名前と、
// 同梱の Vencord の場所から dock::loose_install_files_of で作る (dock/dock_poll_install.cpp)。プレビューツールは
// dock/install_flow.cpp をビルドしない (platform が必要) ので、同じ元のデータ (ビルドのときに CMake の一覧から書いた
// ヘッダーの名前と cordmux.dll) を、同じ並べ方 (名前の順。大文字と小文字を区別しない) で並べる。場所は架空の
// 手順書と違う置き方のフォルダー。名前をここにコピーしないのは、同梱するファイルを追加したときに、画像だけが古いまま
// 残るためである。
dock::LooseInstallFiles preview_loose_files()
{
	dock::LooseInstallFiles loose;
	loose.files_dir = "C:/obs-studio/plugins/cordmux/bin/64bit";
	loose.file_names.push_back("cordmux.dll");
	for (const char *name : dock::generated::kBundledRuntimeFiles)
		loose.file_names.push_back(name);
	std::stable_sort(loose.file_names.begin(), loose.file_names.end(), [](const std::string &a, const std::string &b) {
		return QString::fromStdString(a).compare(QString::fromStdString(b), Qt::CaseInsensitive) < 0;
	});
	loose.data_dir = "C:/obs-studio/plugins/cordmux/data";
	return loose;
}
#endif

// variant は結果のダイアログの場面の違い (dialog:<ダイアログ>:<違い> は場面 dialog-<ダイアログ>-<違い> を使い、画像の
// 名前も同じ)。同じダイアログの ID になる場面 (途中で止まったダイアログの手順書と違う置き方と、3 つのグループの形) を
// 選ぶために使う。確認のダイアログだけは違いが loose の 1 つで、手順書と違う置き方の確認のダイアログ (最後の文が
// uninstall_files_after.text) を描画する。
bool face_dialog(const FaceOptions &opt, const QString &id, const QString &variant, Writer &w)
{
	// 場面の違いを選べるのは、アンインストールのダイアログ (uninstall-) だけ。
	if (!variant.isEmpty() && !id.startsWith(QLatin1String("uninstall-")))
		return false;
	// ダイアログの親ウィンドウはドック (ドックから始めた操作の形)。
	DockHost host = build_dock(fixture_by_id("status-connected-in-call"), opt.style);
	host.dock->resize(420, 600);
	settle();
	if (id == QLatin1String("rename")) {
		std::unique_ptr<QDialog> dialog(
			dock::make_rename_dialog(host.widget, QStringLiteral("hanako"), std::string()));
		QString widgets;
		const QImage image = grab_window(dialog.get(), &widgets);
		w.shot(QStringLiteral("dialog-rename"), image, widgets);
		return true;
	}
	dock::ui::MessageSpec spec;
	bool found = false;
#if CORDMUX_UI_PREVIEW_HAS_INSTALL_UI
	if (id == QLatin1String(dock::kDialogIdUninstallConfirm)) {
		const bool loose = variant == QLatin1String("loose");
		spec = dock::uninstall_confirm_spec(loose);
		found = variant.isEmpty() || loose;
	} else if (id == QLatin1String(dock::kDialogIdSwitchConfirm)) {
		spec = dock::switch_confirm_spec(fixture_by_id("dialog-switch-to-cordmux").dialog_vencord);
		found = true;
	} else if (id.startsWith(QLatin1String("uninstall-")) && !variant.isEmpty()) {
		// 場面の違いを指定した形。その場面だけを使い、場面のダイアログの ID が id と同じときだけ描画する。
		const Fixture fixture =
			fixture_by_id((QStringLiteral("dialog-") + id + QLatin1Char('-') + variant).toStdString());
		const ledger::dockview::DockViewV2 view = view_for(fixture);
		if (!fixture.id.empty() && view.uninstall_result.has_value() &&
		    id == QLatin1String(dock::uninstall_result_dialog_id(*view.uninstall_result))) {
			spec = dock::uninstall_result_spec(*view.uninstall_result);
			found = true;
		}
	} else if (id.startsWith(QLatin1String("uninstall-"))) {
		// 結果のダイアログは、場面の View の uninstall_result から作る (どの形かは場面が決める)。
		for (const char *scene : {"dialog-uninstall-done", "dialog-uninstall-stopped", "dialog-uninstall-loose",
					  "status-uninstall-done", "status-uninstall-done-unchecked", "status-uninstall-stopped"}) {
			const ledger::dockview::DockViewV2 view = view_for(fixture_by_id(scene));
			if (!view.uninstall_result.has_value())
				continue;
			if (id == QLatin1String(dock::uninstall_result_dialog_id(*view.uninstall_result))) {
				// 手順書と違う置き方の完了のダイアログは、製品と同じく、削除するファイルの場所と
				// 名前と、データのフォルダーを表示する。
				const dock::LooseInstallFiles loose = preview_loose_files();
				spec = dock::uninstall_result_spec(*view.uninstall_result,
								   id == QLatin1String(dock::kDialogIdUninstallLoose) ? &loose
															  : nullptr);
				found = true;
				break;
			}
		}
	}
#endif
#if CORDMUX_UI_PREVIEW_HAS_WINDOWS
	if (id == QLatin1String("export-failed-write")) {
		spec = dock::export_failed_spec(bundle::ExportOutcome::WriteFailed);
		found = true;
	} else if (id == QLatin1String("export-failed-zip")) {
		spec = dock::export_failed_spec(bundle::ExportOutcome::ZipFailed);
		found = true;
	} else if (id == QLatin1String("export-failed-secret")) {
		spec = dock::export_failed_spec(bundle::ExportOutcome::SecretFound);
		found = true;
	} else if (id == QLatin1String("settings-save-failed")) {
		spec = dock::settings_save_failed_spec();
		found = true;
	}
#endif
	if (!found)
		return false;
	// [ファイルの場所を開く] は、開く関数 (reveal_file) があるときだけダイアログに表示される。製品では呼び出し元
	// (dock/dock_poll_install.cpp) が platform::open_file_location を渡す。プレビューツールは platform を
	// ビルドせず、エクスプローラーも開かないので、何もしない関数を渡して、製品と同じボタンの並びで描画する
	// (渡さないと、製品で表示されるボタンが画像に出ない)。
	if (!spec.open_file_location_text.isEmpty() && !spec.reveal_file)
		spec.reveal_file = [](const QString &) {};
	// 見た目の値 (--dialog-main-scale など) を、製品と同じ方法で渡す (make_message_dialog の 3 つ目の引数)。
	std::unique_ptr<QDialog> dialog(dock::ui::make_message_dialog(host.widget, spec, opt.style));
	dialog->setProperty("dialogId", id);
	QString widgets;
	const QImage image = grab_window(dialog.get(), &widgets);
	const QString name = QStringLiteral("dialog-") + id + (variant.isEmpty() ? QString() : QStringLiteral("-") + variant);
	w.shot(name, image, widgets);
	w.screen_fit += screen_fit_row(name, dialog.get(), opt);
	w.fields += line_edit_rows(name, dialog.get());
	w.wraps += wrap_rows(name, dialog.get());
	// 主文の行数 (主文の倍率ごとに、主文が何行に折り返されるか)。主文の QLabel は、テキストから行区切り文字
	// (U+2028。製品が語の境で折り返した主文。dock/ui/message_dialog.cpp の break_main_at_phrase) を除くと
	// spec.main と同じになる QLabel (製品の QLabel は objectName を持たず、折り返した主文はテキストに行区切り文字が
	// 入るので、そのままの一致では見つからない)。行数は、QLabel の高さをフォントの行の高さで割って丸めた値。
	// main_breaks は主文のテキストの行区切り文字の数 (製品が折り返したか)。
	{
		const QChar separator(QChar::LineSeparator);
		QLabel *main_label = nullptr;
		for (QLabel *label : dialog->findChildren<QLabel *>()) {
			QString plain = label->text();
			plain.remove(separator);
			if (plain == spec.main) {
				main_label = label;
				break;
			}
		}
		if (w.extra_name.isEmpty()) {
			w.extra_name = QStringLiteral("dialog-main.tsv");
			w.extra = QStringLiteral("dialog\tscale\tmain_pt\tmain_line_px\tmain_w\tmain_h\tmain_lines\tdialog_w\tdialog_h\t"
						 "main_breaks\n");
		}
		if (main_label != nullptr) {
			const QFontMetrics fm(main_label->font());
			const int lines = std::max(1, qRound(double(main_label->height()) / std::max(1, fm.lineSpacing())));
			w.extra += QStringList{name, QString::number(opt.style.dialog_main_scale, 'f', 2),
					       QString::number(QFontInfo(main_label->font()).pointSizeF(), 'f', 2),
					       QString::number(fm.lineSpacing()), QString::number(main_label->width()),
					       QString::number(main_label->height()), QString::number(lines),
					       QString::number(dialog->width()), QString::number(dialog->height()),
					       QString::number(main_label->text().count(separator))}
					   .join(QLatin1Char('\t')) +
				   QLatin1Char('\n');
		} else {
			w.extra += name + QStringLiteral("\t-\t-\t-\t-\t-\t-\t-\t-\t-\n");
		}
	}
	std::vector<TargetFact> targets = collect_targets(dialog.get(), QPoint(0, 0));
	w.targets += targets_rows(name, targets, judge_258(targets));
	w.contrast += contrast_rows(name, measure_contrast(dialog.get(), image, QPoint(0, 0)));
	shot_scrolled_bottom(opt, w, name, dialog.get());
	return true;
}

// 画面に収まるかの集計 (screen_fit_row) のセルフテスト (--face probe:screen-fit。screen-fit-selfcheck.tsv)。集計が
// 失敗を出せることを毎回確認する。失敗を出せない集計は、成功しても何も確認していないためである。いちばん背の高い
// 手順書と違う置き方の完了のダイアログ (箇条書きが 13 行) を、3 つの形で数える。
//   (a) 製品の形 (ダイアログが画面の作業領域を読む): 収まり、外のボタンも届かない行も 0。
//   (b) スクロールを止めた形 (set_message_dialog_work_area で大きな作業領域を指定し、スクロールしない形): ダイアログの
//       本来の高さが作業領域 - 枠 を超える画面なら、ボタンが外にあり、届かない行がある (失敗)。超えない画面では
//       数えない (skipped)。
//   (c) 指定を外した形: (a) と同じ結果になる (行き来してもダイアログが元に戻る)。
// テスト用に作ったダイアログではなく製品のダイアログで数えるのは、製品と同じウィジェットのツリーを通すためである。
bool face_probe_screen_fit(const FaceOptions &opt, Writer &w)
{
#if CORDMUX_UI_PREVIEW_HAS_INSTALL_UI
	DockHost host = build_dock(fixture_by_id("status-connected-in-call"), opt.style);
	host.dock->resize(420, 600);
	settle();
	const ledger::dockview::DockViewV2 view = view_for(fixture_by_id("dialog-uninstall-loose"));
	if (!view.uninstall_result.has_value())
		return false;
	const dock::LooseInstallFiles loose = preview_loose_files();
	dock::ui::MessageSpec spec = dock::uninstall_result_spec(*view.uninstall_result, &loose);
	if (!spec.open_file_location_text.isEmpty() && !spec.reveal_file)
		spec.reveal_file = [](const QString &) {};
	std::unique_ptr<QDialog> dialog(dock::ui::make_message_dialog(host.widget, spec, opt.style));
	const int avail_h = dialog->screen() != nullptr ? dialog->screen()->availableGeometry().height() : 0;
	const auto measure = [&](const QString &name) {
		QString widgets;
		grab_window(dialog.get(), &widgets);
		return screen_fit_row(name, dialog.get(), opt).split(QLatin1Char('\t'));
	};
	const int col_fits = 7, col_buttons_out = 12, col_rows_bad = 14, col_window_h = 6;
	QString rows = QStringLiteral("case\texpect\twindow_h\tfits\tbuttons_out\trows_unreachable\tresult\n");
	const auto add = [&](const QString &name, const QStringList &r, const QString &expect, bool ok) {
		rows += QStringList{name, expect, r.value(col_window_h), r.value(col_fits), r.value(col_buttons_out),
				    r.value(col_rows_bad), ok ? QStringLiteral("pass") : QStringLiteral("FAIL")}
				.join(QLatin1Char('\t')) +
			QLatin1Char('\n');
	};
	const QStringList a = measure(QStringLiteral("a-product"));
	add(QStringLiteral("a-product"), a, QStringLiteral("fits, 0 out, 0 unreachable"),
	    a.value(col_fits) == QLatin1String("yes") && a.value(col_buttons_out) == QLatin1String("0") &&
		    a.value(col_rows_bad) == QLatin1String("0"));
	dock::ui::set_message_dialog_work_area(dialog.get(), 100000, 40);
	const QStringList b = measure(QStringLiteral("b-no-scroll"));
	if (b.value(col_window_h).toInt() + 40 > avail_h)
		add(QStringLiteral("b-no-scroll"), b, QStringLiteral("red: buttons out and rows unreachable"),
		    b.value(col_fits) == QLatin1String("NO") && b.value(col_buttons_out).toInt() > 0 &&
			    b.value(col_rows_bad).toInt() > 0);
	else
		rows += QStringLiteral("b-no-scroll\tskipped (the natural height fits this screen)\t%1\t-\t-\t-\tskipped\n")
				.arg(b.value(col_window_h));
	dock::ui::set_message_dialog_work_area(dialog.get(), 0, 0);
	const QStringList c = measure(QStringLiteral("c-product-again"));
	add(QStringLiteral("c-product-again"), c, QStringLiteral("same as a"),
	    c.mid(1) == a.mid(1));
	w.extra_name = QStringLiteral("screen-fit-selfcheck.tsv");
	w.extra = rows;
	print_line(rows.trimmed());
	if (rows.contains(QStringLiteral("\tFAIL\n")))
		++w.failures;
	return true;
#else
	(void)opt;
	(void)w;
	return false;
#endif
}

// メニューを重ねた画像の、メニューの項目の押せる範囲を targets.tsv に書く (項目の高さを見るため)。
void menu_tables(Writer &w, const QString &name, DockHost &host, const std::vector<QMenu *> &menus,
		 const std::vector<QPoint> &origins, const QImage &image)
{
	std::vector<TargetFact> targets;
	for (std::size_t i = 0; i < menus.size() && i < origins.size(); ++i) {
		const std::vector<TargetFact> items = menu_targets(menus[i], origins[i]);
		targets.insert(targets.end(), items.begin(), items.end());
	}
	w.targets += targets_rows(name, targets, judge_258(targets));
	(void)host;
	(void)image;
}

bool face_menu(const FaceOptions &opt, const QString &which, Writer &w)
{
	if (which == QLatin1String("dock")) {
		// 場面の既定値: 平常、cordmux を無効にした状態、録画中 (最後の項目のテキストが変わる)、
		// アンインストールの後 (押せない項目と理由)。
		const QStringList scenes = opt.only.isEmpty()
						   ? QStringList{QStringLiteral("status-connected-in-call"),
								 QStringLiteral("status-feature-off"),
								 QStringLiteral("record-recording"),
								 QStringLiteral("status-uninstall-running")}
						   : opt.only.split(QLatin1Char(','), Qt::SkipEmptyParts);
		for (const QString &scene : scenes) {
			const Fixture fixture = fixture_by_id(scene.toStdString());
			if (fixture.id.empty()) {
				print_line(QStringLiteral("menu:dock: no scene %1").arg(scene));
				++w.failures;
				continue;
			}
			DockHost host = build_dock(fixture, opt.style);
			// ドックの大きさは、--sizes の最初の値か、既定値の 420 x 600。
			const QSize dock_size = opt.sizes.isEmpty() ? QSize(420, 600) : opt.sizes.front();
			render_dock(host, dock_size.width(), dock_size.height(), dock_size.height());
			auto *anchor = host.widget->findChild<QPushButton *>(QStringLiteral("cordmuxDockMenuButton"));
			QMenu *menu = dock::make_dock_menu(host.widget, host.view.menu, nullptr);
			QString widgets;
			std::vector<QPoint> origins;
			const QImage image = grab_dock_with_menus(host, {menu}, anchor, &widgets, &origins);
			const QString name = QStringLiteral("menu-dock-") + scene;
			w.shot(name, image, widgets);
			menu_tables(w, name, host, {menu}, origins, image);
			delete menu;
		}
		return true;
	}
#if CORDMUX_UI_PREVIEW_HAS_ROWS
	if (which == QLatin1String("row") || which == QLatin1String("row-voice")) {
		const bool voice = which == QLatin1String("row-voice");
		// 場面の既定値: 使う声を置き換えた行、「自動で載せる」が付いた行、選び方が「自動」の行。
		const QStringList scenes = opt.only.isEmpty()
						   ? (voice ? QStringList{QStringLiteral("people-voice-source")}
							    : QStringList{QStringLiteral("people-voice-source"),
									  QStringLiteral("people-always-on"),
									  QStringLiteral("people-assign-auto")})
						   : opt.only.split(QLatin1Char(','), Qt::SkipEmptyParts);
		for (const QString &scene : scenes) {
			const Fixture fixture = fixture_by_id(scene.toStdString());
			if (fixture.id.empty()) {
				print_line(QStringLiteral("menu:row: no scene %1").arg(scene));
				++w.failures;
				continue;
			}
			DockHost host = build_dock(fixture, opt.style);
			render_dock(host, 420, 600, 600);
			// ウィジェットの型は Q_OBJECT を持たないので、findChild の型では探せない。
			dock::PeopleList *list = nullptr;
			for (QWidget *c : host.widget->findChildren<QWidget *>()) {
				if (auto *p = dynamic_cast<dock::PeopleList *>(c))
					list = p;
			}
			if (list == nullptr || host.view.people.empty())
				continue;
			// 行ごとに 1 枚 (行のメニューの現在の値は参加者ごとに違う)。製品がメニューを開かない行
			// (make_menu_for が nullptr) は描画しない。
			for (const ledger::dockview::PersonRow &row : host.view.people) {
				QMenu *menu = list->make_menu_for(row.user_id, host.widget);
				if (menu == nullptr)
					continue;
				QWidget *anchor_widget = list->row_for(row.user_id);
				QPushButton *anchor =
					anchor_widget != nullptr
						? anchor_widget->findChild<QPushButton *>(QStringLiteral("cordmuxRowMenuButton"))
						: nullptr;
				std::vector<QMenu *> menus{menu};
				if (voice) {
					for (QAction *a : menu->actions()) {
						if (a->menu() != nullptr)
							menus.push_back(a->menu());
					}
					if (menus.size() == 1) {
						delete menu;
						continue;
					}
				}
				QString widgets;
				std::vector<QPoint> origins;
				const QImage image = grab_dock_with_menus(host, menus, anchor, &widgets, &origins);
				const QString name = QStringLiteral("menu-") + which + QStringLiteral("-") + scene +
						     QStringLiteral("-") + QString::fromStdString(row.user_id).right(2);
				w.shot(name, image, widgets);
				menu_tables(w, name, host, menus, origins, image);
				delete menu;
			}
		}
		return true;
	}
	if (which == QLatin1String("row-replace")) {
		// 枠が埋まっているときにチェックを押して開く「入れ替える相手」のメニュー。行ごとに 1 枚。
		const QStringList scenes = opt.only.isEmpty() ? QStringList{QStringLiteral("people-8")}
							      : opt.only.split(QLatin1Char(','), Qt::SkipEmptyParts);
		for (const QString &scene : scenes) {
			const Fixture fixture = fixture_by_id(scene.toStdString());
			if (fixture.id.empty()) {
				print_line(QStringLiteral("menu:row-replace: no scene %1").arg(scene));
				++w.failures;
				continue;
			}
			DockHost host = build_dock(fixture, opt.style);
			render_dock(host, 420, 600, 600);
			for (const ledger::dockview::PersonRow &row : host.view.people_list.sharing) {
				if (row.replace_choices.empty())
					continue;
				QMenu *menu = dock::make_replace_menu(host.widget, row, host.widget->palette().color(QPalette::WindowText),
								      16, nullptr);
				if (menu == nullptr)
					continue;
				// 製品はポインターの位置に開く。ここでは行の下に開く形で描く (行のボタンを基準にする)。
				QPushButton *anchor = nullptr;
				for (QPushButton *b : host.widget->findChildren<QPushButton *>(QStringLiteral("cordmuxRowMenuButton"))) {
					for (QWidget *c = b; c != nullptr && anchor == nullptr; c = c->parentWidget()) {
						const auto *person = dynamic_cast<dock::PersonRowWidget *>(c);
						if (person != nullptr && person->view().user_id == row.user_id)
							anchor = b;
					}
				}
				QString widgets;
				std::vector<QPoint> origins;
				const QImage image = grab_dock_with_menus(host, {menu}, anchor, &widgets, &origins);
				const QString name = QStringLiteral("menu-row-replace-") + scene + QStringLiteral("-") +
						     QString::fromStdString(row.user_id).right(2);
				w.shot(name, image, widgets);
				menu_tables(w, name, host, {menu}, origins, image);
				delete menu;
			}
		}
		return true;
	}
#endif
	if (which == QLatin1String("confirm")) {
		// 配信中に押した場所の隣に出る確認。[退出]、録画の終了、ダミー映像の表示の 3 つ。製品と同じにボタンを押して出す。
		struct ConfirmCase {
			const char *scene;
			const char *button;
			std::optional<cordmux::config::DockTab> tab;
			const char *name;
		};
		const ConfirmCase cases[] = {
			{"people-audio", "cordmuxAccountLeave", std::nullopt, "people-audio"},
		};
		for (const ConfirmCase &c : cases) {
			const Fixture fixture = fixture_by_id(c.scene);
			if (fixture.id.empty()) {
				print_line(QStringLiteral("menu:confirm: no scene %1").arg(QLatin1String(c.scene)));
				++w.failures;
				continue;
			}
			DockHost host = build_dock(fixture, opt.style);
			if (c.tab.has_value())
				host.widget->set_tab(*c.tab);
			render_dock(host, 346, 600, 600);
			host.widget->set_confirm_inputs(true, true);
			auto *button = host.widget->findChild<QAbstractButton *>(QLatin1String(c.button));
			if (button == nullptr) {
				print_line(QStringLiteral("menu:confirm: no button %1").arg(QLatin1String(c.button)));
				++w.failures;
				continue;
			}
			button->click();
			settle();
			w.shot(QStringLiteral("confirm-") + QLatin1String(c.name), grab_widget(host.dock.get()), QString());
		}
		return true;
	}
	return false;
}

} // namespace

QStringList face_names()
{
	return QStringList{QStringLiteral("dock"),
			   QStringLiteral("settings:<recording|receive|advanced|about|all>[:restart|:busy|:uninstall]"),
			   QStringLiteral("participants"),
			   QStringLiteral("dialog:<uninstall-confirm|switch-confirm|uninstall-done|uninstall-unconfirmed|"
					  "uninstall-stopped|uninstall-loose|export-failed-write|"
					  "export-failed-zip|export-failed-secret|settings-save-failed|rename>[:<variant>]"),
			   QStringLiteral("menu:dock"),
			   QStringLiteral("menu:row"),
			   QStringLiteral("menu:row-voice"),
			   QStringLiteral("menu:row-replace"),
			   QStringLiteral("menu:confirm"),
			   QStringLiteral("focus"),
			   QStringLiteral("timeline"),
			   QStringLiteral("theme-switch"),
			   QStringLiteral("probe:menu-check"),
			   QStringLiteral("probe:obs-layout"), QStringLiteral("probe:screen-fit")};
}

int run_face(QApplication &app, const ObsPaths &paths, const FaceOptions &opt)
{
	const QString kind = opt.face.section(QLatin1Char(':'), 0, 0);
	const QString arg = opt.face.section(QLatin1Char(':'), 1, 1);
	const QString variant = opt.face.section(QLatin1Char(':'), 2, 2);
	int failures = 0;
	bool absent = false;
	set_grab_dpr(opt.dpr);
	for (const QString &theme_name : opt.themes) {
		ComposedTheme theme;
		QString error;
		if (!apply_obs_theme(app, paths, theme_id_of(theme_name), opt.platform_defaults, &theme, &error,
				     opt.appearance)) {
			print_line(QStringLiteral("theme %1: %2").arg(theme_name, error));
			return 1;
		}
		// OBS のメインウィンドウと同じ型の名前のコンテナー (テーマが、画面共有のアイコン desktopCapIcon を
		// 設定する)。製品は obs_frontend_get_main_window で実際のメインウィンドウを読む。
		auto main_window = std::make_unique<OBSBasic>();
		main_window->setAttribute(Qt::WA_DontShowOnScreen, true);
		main_window->ensurePolished();
		set_preview_main_window(main_window.get());
		for (const QString &lang : opt.langs) {
			if (!load_locale(opt.locale_dir, lang, &error) || !load_frontend_locale(opt.obs_source, lang, &error)) {
				print_line(QStringLiteral("locale %1: %2").arg(lang, error));
				set_preview_main_window(nullptr);
				return 1;
			}
			Writer w;
			w.dir = opt.out_dir + QStringLiteral("/face-") + QString(opt.face).replace(QLatin1Char(':'), QLatin1Char('-')) +
				QStringLiteral("/") + theme_dir_name(theme_name, opt) + QStringLiteral("/") + lang;
			QDir().mkpath(w.dir);
			w.index_html = QStringLiteral("<!doctype html><meta charset=\"utf-8\"><title>cordmux %1 %2 %3</title>\n"
						      "<style>body{font-family:sans-serif;background:#888}figure{display:inline-block;"
						      "vertical-align:top;margin:4px}img{border:1px solid #000}</style>\n")
					       .arg(opt.face, theme_dir_name(theme_name, opt), lang);
			bool drawn = true;
			if (kind == QLatin1String("dock"))
				face_dock(opt, w);
			else if (kind == QLatin1String("settings"))
				drawn = face_settings(opt, arg.isEmpty() ? QStringLiteral("all") : arg, variant, w);
			else if (kind == QLatin1String("participants"))
				drawn = face_participants(opt, w);
			else if (kind == QLatin1String("dialog"))
				drawn = face_dialog(opt, arg, variant, w);
			else if (kind == QLatin1String("menu"))
				drawn = face_menu(opt, arg, w);
			else if (kind == QLatin1String("focus"))
				drawn = face_focus(opt, w) == 0;
			else if (kind == QLatin1String("timeline"))
				drawn = face_timeline(opt, w) == 0;
			else if (kind == QLatin1String("theme-switch"))
				drawn = face_theme_switch(app, paths, opt, theme_name, w) == 0;
			else if (kind == QLatin1String("probe") && arg == QLatin1String("menu-check"))
				drawn = face_probe_menu_check(app, theme, w) == 0;
			else if (kind == QLatin1String("probe") && arg == QLatin1String("obs-layout"))
				drawn = face_probe_obs_layout(opt, w) == 0;
			else if (kind == QLatin1String("probe") && arg == QLatin1String("screen-fit"))
				drawn = face_probe_screen_fit(opt, w);
			else {
				print_line(QStringLiteral("unknown face: %1").arg(opt.face));
				set_preview_main_window(nullptr);
				return 2;
			}
			if (!drawn) {
				// 描画する関数がビルドにない (ウィンドウを作るソースの .cmake がない、など)。
				// 描画できなかったとは数えない。
				print_line(QStringLiteral("face %1: このビルドにはない").arg(opt.face));
				absent = true;
				break;
			}
			w.write_tables();
			failures += w.failures;
			print_line(QStringLiteral("wrote %1").arg(w.dir));
		}
		set_preview_main_window(nullptr);
		if (absent)
			break;
	}
	return failures == 0 ? 0 : 1;
}

} // namespace cordmux::ui_preview
