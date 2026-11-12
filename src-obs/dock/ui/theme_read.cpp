// SPDX-License-Identifier: Apache-2.0

// theme_read.h の実装。

#include "dock/ui/theme_read.h"

#include <QDialog>
#include <QFile>
#include <QFrame>
#include <QLabel>
#include <QListWidget>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QSize>
#include <QString>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QVariant>
#include <QWidget>

#include <obs-frontend-api.h>

#include <memory>

namespace cordmux::dock::ui {

namespace {

QColor window_text_of(QWidget *w)
{
	w->ensurePolished();
	return w->palette().color(QPalette::WindowText);
}

QColor window_of(QWidget *w)
{
	w->ensurePolished();
	return w->palette().color(QPalette::Window);
}

// 見えないラベルをコンテナーの子として作る (OBSDock QLabel のような、子孫のセレクターが適用されるように)。
template <class W> std::unique_ptr<W> probe(QWidget *container, const char *classes)
{
	auto w = std::make_unique<W>(container);
	w->setAttribute(Qt::WA_DontShowOnScreen, true);
	w->hide();
	if (classes != nullptr)
		w->setProperty("class", QString::fromLatin1(classes));
	return w;
}

// OBS のシーンのドックの一覧 (OBS 31.1.1 の frontend/forms/OBSBasic.ui の SceneTree。QListWidget で、枠なし、項目の
// 間隔 0) と同じ形の見えない一覧。1 つ目の項目のテキストの左端を、Qt が項目を描くときと同じ計算で求める。
// テキストの位置は、テーマの項目の矩形 (::item の margin、border、padding) を除いたテキストの矩形
// (SE_ItemViewItemText) の左に、Qt がテキストを描くときに加える余白 (PM_FocusFrameHMargin + 1。Qt の
// qcommonstyle.cpp の viewItemDrawText) を加えた位置である。
class ListProbe : public QListWidget {
public:
	explicit ListProbe(QWidget *parent) : QListWidget(parent)
	{
		setAttribute(Qt::WA_DontShowOnScreen, true);
		hide();
		setFrameShape(QFrame::NoFrame);
		setSpacing(0);
		addItem(QStringLiteral("x"));
	}

	int text_left()
	{
		ensurePolished();
		QListWidgetItem *it = item(0);
		if (it == nullptr)
			return -1;
		QStyleOptionViewItem opt;
		initViewItemOption(&opt);
		opt.rect = visualItemRect(it);
		opt.index = indexFromItem(it);
		opt.text = it->text();
		opt.features |= QStyleOptionViewItem::HasDisplay;
		const QRect text = style()->subElementRect(QStyle::SE_ItemViewItemText, &opt, this);
		if (!text.isValid())
			return -1;
		const int draw_margin = style()->pixelMetric(QStyle::PM_FocusFrameHMargin, &opt, this) + 1;
		// 見えない一覧はスクロール領域の配置 (viewport の位置) が決まっていないので、枠と余白から求める。
		const int viewport_left = contentsRect().left() + viewportMargins().left();
		return viewport_left + text.left() + draw_margin;
	}
};

QString settings_icon_name(SettingsIcon which)
{
	switch (which) {
	case SettingsIcon::General:
		return QStringLiteral("general");
	case SettingsIcon::Appearance:
		return QStringLiteral("appearance");
	case SettingsIcon::Stream:
		return QStringLiteral("stream");
	case SettingsIcon::Output:
		return QStringLiteral("output");
	case SettingsIcon::Audio:
		return QStringLiteral("audio");
	case SettingsIcon::Video:
		return QStringLiteral("video");
	case SettingsIcon::Hotkeys:
		return QStringLiteral("hotkeys");
	case SettingsIcon::Accessibility:
		return QStringLiteral("accessibility");
	case SettingsIcon::Advanced:
		return QStringLiteral("advanced");
	}
	return QStringLiteral("general");
}

QIcon load_icon(const QString &path)
{
	// SVG のアイコンはサイズの一覧を持たない (どの大きさでも描ける) ので、ファイルが存在し、画像を描けるかで
	// 確認する。
	if (!QFile::exists(path))
		return QIcon();
	QIcon icon(path);
	if (icon.isNull() || icon.pixmap(QSize(16, 16)).isNull())
		return QIcon();
	return icon;
}

} // namespace

calc::Rgb to_rgb(const QColor &c)
{
	const QColor rgb = c.toRgb();
	return calc::Rgb{rgb.red(), rgb.green(), rgb.blue()};
}

double contrast_ratio(const QColor &a, const QColor &b)
{
	return calc::contrast_ratio(to_rgb(a), to_rgb(b));
}

QColor black_or_white_on(const QColor &surface)
{
	const calc::Rgb c = calc::black_or_white_on(to_rgb(surface));
	return QColor(c.r, c.g, c.b);
}

ThemeRead read_theme(QWidget *container, QWidget *main_window)
{
	ThemeRead r;
	if (container == nullptr)
		return r;
	auto plain = probe<QLabel>(container, nullptr);
	auto muted = probe<QLabel>(container, "text-muted");
	auto danger = probe<QLabel>(container, "text-danger");
	auto warning = probe<QLabel>(container, "text-warning");
	auto success = probe<QLabel>(container, "text-success");
	auto plain_frame = probe<QFrame>(container, nullptr);
	auto notice = probe<QFrame>(container, "frame-notice");
	auto dots = probe<QPushButton>(container, "btn-tool icon-dots-vert");

	r.text = window_text_of(plain.get());
	const QColor muted_c = window_text_of(muted.get());
	const QColor danger_c = window_text_of(danger.get());
	r.has_muted = muted_c != r.text;
	r.has_danger = danger_c != r.text;
	r.muted = r.has_muted ? muted_c : r.text;
	r.danger = r.has_danger ? danger_c : QColor();
	const QColor warning_c = window_text_of(warning.get());
	const QColor success_c = window_text_of(success.get());
	r.warning = warning_c != r.text ? warning_c : QColor();
	r.success = success_c != r.text ? success_c : QColor();
	r.base_bg = window_of(container);
	const QColor frame_plain = window_of(plain_frame.get());
	const QColor notice_c = window_of(notice.get());
	r.has_notice = notice_c != frame_plain;
	r.notice_bg = r.has_notice ? notice_c : r.base_bg;
	dots->ensurePolished();
	r.dots = dots->icon();
	r.has_dots = !r.dots.isNull();
	r.icon_px = dots->iconSize().width() > 0 ? dots->iconSize().width() : 16;
	{
		ListProbe list(container);
		r.list_text_left_px = list.text_left();
	}

	// ダイアログの背景: QDialog の Window。コンテナーがダイアログそのものなら、その背景。
	if (qobject_cast<QDialog *>(container) != nullptr) {
		r.window_bg = r.base_bg;
	} else {
		QDialog dialog;
		dialog.setAttribute(Qt::WA_DontShowOnScreen, true);
		dialog.ensurePolished();
		r.window_bg = dialog.palette().color(QPalette::Window);
	}
	// 明暗は OBS が決めたものを読む (OBS 自身のアイコンの Dark と Light の選び方と同じ IsThemeDark)。背景と文字の
	// 明るさを比べて決めると、テーマがどちらかの色を別の意味で使ったときに OBS と食い違う。
	r.dark = obs_frontend_is_theme_dark();

	// 画面共有のアイコン: OBS のメインウィンドウの Q_PROPERTY (OBS 31.1.1 の frontend/widgets/OBSBasic.hpp)。
	// テーマが qproperty-desktopCapIcon で設定する。
	if (main_window != nullptr) {
		main_window->ensurePolished();
		const QVariant v = main_window->property("desktopCapIcon");
		if (v.isValid() && v.canConvert<QIcon>()) {
			r.desktop_cap = v.value<QIcon>();
			r.has_desktop_cap = !r.desktop_cap.isNull();
		}
	}
	return r;
}

bool muted_ok(const ThemeRead &theme, const QColor &ground)
{
	return theme.has_muted && calc::muted_text_allowed(to_rgb(theme.muted), to_rgb(ground));
}

QColor muted_or_text(const ThemeRead &theme, const QColor &ground)
{
	return muted_ok(theme, ground) ? theme.muted : theme.text;
}

L1Surface l1_surface(const ThemeRead &theme, const QColor &ground, const DockStyle &style)
{
	L1Surface s;
	if (theme.has_danger && theme.danger.isValid()) {
		s.fill = theme.danger;
		s.foreground = black_or_white_on(s.fill);
	} else {
		// 危険の色を持たないテーマ: 本文の色のパネルに、背景の色のテキスト (色を反転したバー)。
		s.fill = theme.text;
		s.foreground = ground;
		s.inverted = true;
	}
	s.border = theme.text;
	s.border_px = calc::l1_border_px(to_rgb(s.fill), to_rgb(ground), style.l1_border_px, style.l1_border_auto_px,
					 style.l1_border_contrast_min);
	return s;
}

QColor panel_fill(const ThemeRead &theme, bool card_frame_ok)
{
	if (theme.has_notice && card_frame_ok && theme.notice_bg.isValid())
		return theme.notice_bg;
	// ドックの背景から本文の色の方へ寄せる割合 (%)。背景と区別でき、弱いテキストの色が読める範囲の小さい値にする。
	constexpr int kPanelMixPercent = 7;
	const calc::Rgb mixed = calc::mix_rgb(to_rgb(theme.base_bg), to_rgb(theme.text), kPanelMixPercent);
	return QColor(mixed.r, mixed.g, mixed.b);
}

QIcon settings_icon(const ThemeRead &theme, SettingsIcon which)
{
	// OBS の設定のウィンドウのアイコンは、Yami が theme:Dark/settings/*.svg、Yami Light が
	// theme:Light/settings/*.svg、System が :settings/images/settings/*.svg を設定する。theme: の検索パスは
	// OBS が登録する (OBS 31.1.1 の frontend/OBSApp_Themes.cpp)。明暗で選ぶのは、OBS 自身のアイコンの選び方と
	// 同じである (frontend/components/SourceTree.cpp の UpdateNoSourcesMessage)。
	const QString name = settings_icon_name(which);
	const QString tone = theme.dark ? QStringLiteral("Dark") : QStringLiteral("Light");
	QIcon icon = load_icon(QStringLiteral("theme:%1/settings/%2.svg").arg(tone, name));
	if (icon.isNull())
		icon = load_icon(QStringLiteral(":settings/images/settings/%1.svg").arg(name));
	return icon;
}

} // namespace cordmux::dock::ui
