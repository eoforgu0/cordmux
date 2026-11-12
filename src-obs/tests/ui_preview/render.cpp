// SPDX-License-Identifier: Apache-2.0

// render.h の実装。

#include "render.h"

#include <obs-module.h>

#include <QAbstractScrollArea>
#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFontDatabase>
#include <QHeaderView>
#include <QLayout>
#include <QMenu>
#include <QPainter>
#include <QProxyStyle>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyleFactory>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>

#include <algorithm>
#include <functional>

#include "dock/dock_widget.h"
#include "dock/look_tab.h"
#include "ledger/text_ids.h"
#include "sources/self_speaking_judge.h"
#include "locale_stub.h"
#include "obs_dock.h"

namespace cordmux::ui_preview {

namespace {

// OBSProxyStyle (OBS 31.1.1 の frontend/utility/OBSProxyStyle.cpp) と同じオーバーライドを持つスタイル。
// Windows では基底を指定せずに作るので、Qt が選ぶ既定のスタイル (Windows 10 では windowsvista) が基底になる
// (base が nullptr のときの QProxyStyle と同じ)。minimal は既定のスタイルを知らないので、そのときは名前を指定して
// 同じスタイルを基底にする。
class ObsLikeProxyStyle : public QProxyStyle {
public:
	explicit ObsLikeProxyStyle(QStyle *base) : QProxyStyle(base) {}

	int styleHint(StyleHint hint, const QStyleOption *option, const QWidget *widget,
		      QStyleHintReturn *return_data) const override
	{
		if (hint == SH_ComboBox_AllowWheelScrolling)
			return 0;
		return QProxyStyle::styleHint(hint, option, widget, return_data);
	}
};

// たまっているレイアウトの要求と再描画を処理する (ウィンドウを表示しないプラグインでも、レイアウトはイベントの順に
// 進む)。
void settle_impl()
{
	for (int i = 0; i < 4; ++i) {
		QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
		QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
	}
}

// どのウィジェットにもフォーカスがない状態にする。ウィンドウを表示すると、Qt は最初にフォーカスを受け取れる
// ウィジェットにフォーカスを置くので、そのままだとそのウィジェットがフォーカスのある色で描画される。利用者が
// 開いたばかりのドックではどのウィジェットにもフォーカスがないので、プレビューもこの状態を基準にする
// (フォーカスのある状態は --face focus で描画する)。
void clear_focus()
{
	if (QWidget *focused = QApplication::focusWidget())
		focused->clearFocus();
}

double g_grab_dpr = 1.0;

QImage grab_opaque(QWidget *widget)
{
	// 不透明な画像に描画する (QWidget::grab は透明な背景に描画するので、ClearType の文字のにじみ方が変わる)。
	// OBS のウィンドウのバッキングストアも不透明なので、こちらに合わせる。
	QImage image(QSize(qRound(widget->width() * g_grab_dpr), qRound(widget->height() * g_grab_dpr)),
		     QImage::Format_RGB32);
	image.setDevicePixelRatio(g_grab_dpr);
	image.fill(QColor(255, 0, 255)); // 描画されない所があれば目立つ色
	widget->render(&image);
	return image;
}

} // namespace

bool load_obs_fonts(const ObsPaths &paths, QString *error)
{
	static bool loaded = false;
	if (loaded)
		return true;
	// obs-main.cpp の run_program と同じく、Open Sans の 3 つのフォントをアプリのフォントとして追加する。
	for (const char *name : {"OpenSans-Regular.ttf", "OpenSans-Bold.ttf", "OpenSans-Italic.ttf"}) {
		const QString path = paths.fonts_dir + QStringLiteral("/") + QString::fromLatin1(name);
		if (QFontDatabase::addApplicationFont(path) < 0) {
			if (error != nullptr)
				*error = QStringLiteral("cannot load %1").arg(path);
			return false;
		}
	}
	loaded = true;
	return true;
}

QPalette windows_light_base_palette()
{
	// Windows 10 の既定の配色の色 (GetSysColor)。プレビューが PC の設定で変わらないように、毎回は読まずに
	// 既定の配色の値に固定する。
	const QColor btn_face(0xF0, 0xF0, 0xF0);    // COLOR_BTNFACE
	const QColor window_text(0x00, 0x00, 0x00); // COLOR_WINDOWTEXT
	const QColor btn_highlight(0xFF, 0xFF, 0xFF); // COLOR_BTNHIGHLIGHT
	const QColor btn_shadow(0xA0, 0xA0, 0xA0);  // COLOR_BTNSHADOW
	const QColor window(0xFF, 0xFF, 0xFF);      // COLOR_WINDOW
	const QColor btn_text(0x00, 0x00, 0x00);    // COLOR_BTNTEXT
	const QColor light_3d(0xE3, 0xE3, 0xE3);    // COLOR_3DLIGHT
	const QColor dk_shadow_3d(0x69, 0x69, 0x69); // COLOR_3DDKSHADOW
	const QColor highlight_text(0xFF, 0xFF, 0xFF); // COLOR_HIGHLIGHTTEXT
	// アクセントカラー (Windows の既定の青)。OBS は Highlight、Link、LinkVisited を上書きし、Accent の役割は
	// windowsvista のスタイルも Yami の QSS も使わないので、画像には反映されない。
	const QColor accent(0x00, 0x78, 0xD7);
	const QColor accent_dark = accent.darker(120);
	const QColor accent_darker = accent_dark.darker(120);
	const QColor accent_darkest = accent_darker.darker(120);

	// QWindowsTheme の standardPalette() (QStyle::standardPalette のコピー)。
	const QColor background(0xd4, 0xd0, 0xc8);
	QPalette result(Qt::black, background, background.lighter(), background.darker(), QColor(Qt::gray), Qt::black,
			Qt::white);
	result.setBrush(QPalette::Disabled, QPalette::WindowText, QBrush(background.darker()));
	result.setBrush(QPalette::Disabled, QPalette::Text, QBrush(background.darker()));
	result.setBrush(QPalette::Disabled, QPalette::ButtonText, QBrush(background.darker()));
	result.setBrush(QPalette::Disabled, QPalette::Base, QBrush(background));

	// populateLightSystemBasePalette
	QColor placeholder = window_text;
	placeholder.setAlpha(128);
	result.setColor(QPalette::Highlight, accent);
	result.setColor(QPalette::WindowText, window_text);
	result.setColor(QPalette::Button, btn_face);
	result.setColor(QPalette::Light, btn_highlight);
	result.setColor(QPalette::Dark, btn_shadow);
	result.setColor(QPalette::Mid, result.button().color().darker(150));
	result.setColor(QPalette::Text, window_text);
	result.setColor(QPalette::PlaceholderText, placeholder);
	result.setColor(QPalette::BrightText, btn_highlight);
	result.setColor(QPalette::Base, window);
	result.setColor(QPalette::Window, btn_face);
	result.setColor(QPalette::ButtonText, btn_text);
	result.setColor(QPalette::Midlight, light_3d);
	result.setColor(QPalette::Shadow, dk_shadow_3d);
	result.setColor(QPalette::HighlightedText, highlight_text);
	result.setColor(QPalette::Accent, accent_dark);
	result.setColor(QPalette::Link, accent_darker);
	result.setColor(QPalette::LinkVisited, accent_darkest);
	result.setColor(QPalette::Inactive, QPalette::Button, result.button().color());
	result.setColor(QPalette::Inactive, QPalette::Window, result.window().color());
	result.setColor(QPalette::Inactive, QPalette::Light, result.light().color());
	result.setColor(QPalette::Inactive, QPalette::Dark, result.dark().color());
	if (result.midlight() == result.button())
		result.setColor(QPalette::Midlight, result.button().color().lighter(110));

	// QWindowsTheme::systemPalette の残り (Inactive と Disabled のカラーグループ)。
	if (result.window() != result.base()) {
		result.setColor(QPalette::Inactive, QPalette::Highlight, result.color(QPalette::Inactive, QPalette::Window));
		result.setColor(QPalette::Inactive, QPalette::HighlightedText,
				result.color(QPalette::Inactive, QPalette::Text));
		result.setColor(QPalette::Inactive, QPalette::Accent, result.color(QPalette::Inactive, QPalette::Window));
	}
	const QColor wt = result.windowText().color();
	const QColor bt = result.button().color();
	const QColor disabled((wt.red() + bt.red()) / 2, (wt.green() + bt.green()) / 2, (wt.blue() + bt.blue()) / 2);
	result.setColorGroup(QPalette::Disabled, result.windowText(), result.button(), result.light(), result.dark(),
			     result.mid(), result.text(), result.brightText(), result.base(), result.window());
	result.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
	result.setColor(QPalette::Disabled, QPalette::Text, disabled);
	result.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
	result.setColor(QPalette::Disabled, QPalette::Highlight, result.color(QPalette::Highlight));
	result.setColor(QPalette::Disabled, QPalette::HighlightedText, result.color(QPalette::HighlightedText));
	result.setColor(QPalette::Disabled, QPalette::Accent, disabled);
	result.setColor(QPalette::Disabled, QPalette::Base, result.window().color());

	// windowsvista のスタイルが polish(QPalette&) で追加する、交互の行の色 (Yami は QSS で上書きする)。
	result.setColor(QPalette::AlternateBase, result.base().color().darker(104));
	return result;
}

bool apply_obs_theme(QApplication &app, const ObsPaths &paths, const QString &theme_id, bool platform_defaults,
		     ComposedTheme *theme, QString *error, const Appearance &appearance)
{
	// OBSApp::InitTheme と同じく、defaultPalette = palette()、setStyle(new OBSProxyStyle())、「theme:」の検索パスの
	// 順に行う。InitTheme は起動のときに 1 回だけ実行されるので、ここも最初の 1 回だけ行う。
	static bool initialized = false;
	static QPalette default_palette;
	if (!initialized) {
		if (platform_defaults) {
			default_palette = app.palette();
			app.setStyle(new ObsLikeProxyStyle(nullptr));
		} else {
			QStyle *base = QStyleFactory::create(QStringLiteral("windowsvista"));
			if (base == nullptr) {
				if (error != nullptr)
					*error = QStringLiteral("style windowsvista is not available (styles/qmodernwindowsstyle.dll)");
				return false;
			}
			default_palette = windows_light_base_palette();
			app.setStyle(new ObsLikeProxyStyle(base));
		}
		initialized = true;
	}
	QDir::setSearchPaths(QStringLiteral("theme"), QStringList{paths.themes_dir});

	ComposedTheme composed;
	if (!compose_obs_theme(paths.themes_dir, theme_id, appearance, default_palette, &composed, error))
		return false;

	// OBSApp::SetTheme と同じく、setStyleSheet("")、テーマの合成、setPalette、setStyleSheet の順に行う。
	app.setStyleSheet(QString());
	app.setPalette(composed.palette);
	app.setStyleSheet(composed.stylesheet);
	// obs_frontend_is_theme_dark が返す値は、OBS と同じく、適用したテーマの meta の dark にする。
	set_preview_theme_dark(composed.dark);
	if (theme != nullptr)
		*theme = composed;
	return true;
}

void settle()
{
	settle_impl();
}

void set_grab_dpr(double dpr)
{
	g_grab_dpr = dpr;
}

QImage grab_widget(QWidget *widget)
{
	settle_impl();
	return grab_opaque(widget);
}

DockHost build_dock(const Fixture &fixture, const dock::ui::DockStyle &style)
{
	DockHost host;
	host.dock = std::make_unique<OBSDock>();
	// obs_frontend_add_dock_by_id と OBSBasic::AddDockWidget が、ドックのコンテナーに設定するもの。
	host.dock->setObjectName(QStringLiteral("cordmux"));
	host.dock->setWindowTitle(QString::fromUtf8(obs_module_text(ledger::text::keys::kDockWindowTitle)));
	host.dock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
			       QDockWidget::DockWidgetFloatable);
	// 画面に表示しない (プレビューのプラグインではもともと表示されないが、別のプラグインで実行しても表示されない
	// ように)。
	host.dock->setAttribute(Qt::WA_DontShowOnScreen, true);

	host.widget = new dock::DockWidget();
	host.dock->setWidget(host.widget);
	host.widget->set_style(style);
	// ダイアログを開く操作は受け付けない (押しても何も起きない。「名前を変更」のダイアログは
	// --face dialog:rename で描画する)。
	host.widget->set_rename_callback([](const std::string &) {});

	// 表示タブ。設定の値と、OBS のソースの候補 (OBS の映像キャプチャーとシーン、既定の入力の名前) を渡す。
	host.widget->set_look_source_providers(
		[] {
			return std::vector<sources::SelfSourceCandidate>{{"preview-capture", "Video Capture Device", true},
									   {"preview-game", "Game Capture", true},
									   {"preview-scene", "Game Scene", true, true}};
		},
		[] {
			return std::vector<sources::SelfSourceCandidate>{{"preview-desktop", "Desktop Audio", true},
									   {"preview-mic", "Mic/Aux", true}};
		});
	host.widget->set_canvas_size(1920, 1080);
	host.widget->set_look_values(fixture.settings);
	host.widget->look_tab()->refresh_sources();

	// 大きさは描画する側が決め直すので、ここでは仮の大きさにする。
	host.dock->resize(420, 600);
	host.dock->show();
	settle_impl();
	// 場面の値は、ドックを表示し、初めての表示のときの読み直し (DockWidget::showEvent) が済んでから渡す。製品では、通話の
	// 参加者の行やメッセージは、OBS がドックを表示した後に届いた値で作られる。表示する前に渡すと、作った行がその読み直しで
	// 測り直され、表示した後に作られた行だけに起きる測り違い (テーマのスタイルを当てる前のフォントで測る) が画像に出ない。
	// 製品の DockPoll と同じく、decide_dock、stabilize、settle_first_line、render の順に通す (fixtures.h の view_for)。
	host.view = view_for(fixture);
	host.widget->set_status_tab_label(first_line_for(fixture, false).status_tab_label);
	host.widget->render(host.view);
	settle_impl();
	host.widget->refresh_layout();
	host.widget->set_speaking(fixture.speaking);
	host.widget->set_audio_levels(fixture.levels);
	clear_focus();
	settle_impl();
	return host;
}

DockShot render_dock(DockHost &host, int width, int height, int min_height)
{
	DockShot shot;
	int h = height > 0 ? height : min_height;
	auto apply_size = [&](int w, int hh) {
		for (int i = 0; i < 2; ++i) {
			host.dock->resize(w, hh);
			settle_impl();
			host.widget->refresh_layout();
			settle_impl();
		}
	};
	apply_size(width, h);
	if (height <= 0) {
		// スクロールが不要になる高さまで伸ばす (一覧の全員と、すべてのメッセージを 1 枚に入れる)。
		constexpr int kMaxHeight = 6000;
		// 開いているタブのスクロール領域。
		const char *scroll_name = "cordmuxScroll";
		switch (host.widget->tab()) {
		case config::DockTab::People:
			break;
		case config::DockTab::Audio:
			scroll_name = "cordmuxAudioScroll";
			break;
		case config::DockTab::Display:
			scroll_name = "cordmuxDisplayScroll";
			break;
		case config::DockTab::Status:
			scroll_name = "cordmuxStatusScroll";
			break;
		}
		auto *scroll = host.widget->findChild<QScrollArea *>(QString::fromLatin1(scroll_name));
		for (int i = 0; i < 12 && scroll != nullptr; ++i) {
			const int hidden = scroll->verticalScrollBar()->maximum();
			if (hidden <= 0)
				break;
			h = std::min(kMaxHeight, h + hidden);
			apply_size(width, h);
			if (h >= kMaxHeight)
				break;
		}
		shot.still_scrolls = scroll != nullptr && scroll->verticalScrollBar()->maximum() > 0;
	}
	clear_focus();
	settle_impl();
	shot.image = grab_opaque(host.dock.get());
	shot.width = host.dock->width();
	shot.height = host.dock->height();
	shot.size_reached = shot.width == width && (height <= 0 || shot.height == height);
	return shot;
}

} // namespace cordmux::ui_preview
