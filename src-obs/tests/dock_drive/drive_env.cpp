// SPDX-License-Identifier: Apache-2.0

// drive_env.h の実装。

#include "drive_env.h"

#include <QApplication>
#include <QFileInfo>
#include <QWidget>

#include "locale_stub.h"
#include "obs_names.h"
#include "obs_theme.h"
#include "render.h"

namespace cordmux::dock_drive {

namespace {

std::unique_ptr<OBSBasic> g_main_window;

ui_preview::ObsPaths paths_of(const EnvOptions &options)
{
	ui_preview::ObsPaths paths;
	paths.themes_dir = options.obs_source + QStringLiteral("/frontend/data/themes");
	paths.fonts_dir = options.obs_source + QStringLiteral("/frontend/forms/fonts");
	return paths;
}

} // namespace

QString theme_id_of(const QString &name)
{
	if (name == QLatin1String("yami"))
		return QStringLiteral("com.obsproject.Yami.Original");
	if (name == QLatin1String("yami-light"))
		return QStringLiteral("com.obsproject.Yami.Light");
	if (name == QLatin1String("yami-classic"))
		return QStringLiteral("com.obsproject.Yami.Classic");
	if (name == QLatin1String("system"))
		return QStringLiteral("com.obsproject.System");
	return name;
}

bool reapply_theme(QApplication &app, const EnvOptions &options, QString *error)
{
	ui_preview::ComposedTheme theme;
	ui_preview::Appearance appearance;
	appearance.font_scale = options.font_scale;
	appearance.density = options.density;
	return ui_preview::apply_obs_theme(app, paths_of(options), theme_id_of(options.theme), /*platform_defaults=*/true,
					   &theme, error, appearance);
}

bool setup_env(QApplication &app, const EnvOptions &options, QString *error)
{
	const ui_preview::ObsPaths paths = paths_of(options);
	if (!QFileInfo(paths.themes_dir + QStringLiteral("/Yami.obt")).exists()) {
		*error = QStringLiteral("OBS themes not found under %1 (pass --obs-source)").arg(paths.themes_dir);
		return false;
	}
	if (!ui_preview::load_obs_fonts(paths, error))
		return false;
	if (!reapply_theme(app, options, error))
		return false;
	if (!ui_preview::load_locale(options.locale_dir, options.lang, error))
		return false;
	if (!ui_preview::load_frontend_locale(options.obs_source, options.lang, error))
		return false;
	// OBS のメインウィンドウと同じ型名 (OBSBasic) にする。OBS のテーマは、この型名のウィジェットに画面共有の
	// アイコン (desktopCapIcon) を設定する。
	g_main_window = std::make_unique<OBSBasic>();
	g_main_window->setGeometry(0, 0, 1920, 1040);
	g_main_window->ensurePolished();
	// メインウィンドウを表示する (プラットフォームプラグインが OS のウィンドウを作らないので、画面には何も
	// 表示されない)。フローティングのドックの後ろに OBS のメインウィンドウがある形にし、ドックの外へ動かした
	// ポインターがメインウィンドウの上に来て、cordmux のウィンドウの親ウィンドウをたどった先がメインウィンドウに
	// なるようにする。
	// cordmux のダイアログが親ウィンドウの入力だけを止め、ほかのウィンドウ (OBS のメインウィンドウなど) は
	// 操作できることを確認するのに必要である。
	g_main_window->show();
	ui_preview::set_preview_main_window(g_main_window.get());
	return true;
}

QWidget *main_window()
{
	return g_main_window.get();
}

void teardown_env()
{
	ui_preview::set_preview_main_window(nullptr);
	g_main_window.reset();
}

} // namespace cordmux::dock_drive
