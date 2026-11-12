// SPDX-License-Identifier: Apache-2.0

// プレビューツール (ui_preview.exe) のエントリポイント。使い方は README.md。実行中の画面にウィンドウを出さないように、
// Qt のプラットフォームプラグインは必ず cordmuxpreview (preview_platform.h) か Qt 同梱の minimal にし、環境変数で別の
// プラグインが指定されていても上書きする。どちらでもなければ、何も描画せずに終了する。どちらのプラグインも、Qt の
// ウィンドウに OS のウィンドウを作らない。既定は cordmuxpreview で、minimal との違いは preview_platform.h にある。
// --backend minimal は、その違いが画像にどれだけ動作するかを確認するために残している。引数の名前を
// --platform にしないのは、Qt 自身が -platform と --platform を読んでプラグインを差し替えるためである。

#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGlyphRun>
#include <QGuiApplication>
#include <QLabel>
#include <QRawFont>
#include <QRect>
#include <QScreen>
#include <QStringList>
#include <QTextLayout>
#include <QTextStream>
#include <QtPlugin>

#include <qpa/qwindowsysteminterface.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "faces.h"
#include "fixtures.h"
#include "locale_stub.h"
#include "obs_dock.h"
#include "obs_theme.h"
#include "preview_platform.h"
#include "render.h"

#include "ledger/rules.h"

Q_IMPORT_PLUGIN(CordmuxPreviewIntegrationPlugin)

using namespace cordmux::ui_preview;

namespace {

// 今使っているプラットフォームプラグインが cordmuxpreview か (minimal でなければ cordmuxpreview。main で確認している)。
bool g_preview_platform = true;

// 描画する画面 (--screen)。背の高いウィンドウが画面に収まるかを、よく使われる画面の形で描画して確認するために使う。
// Windows の表示の倍率の下で、Qt 6 のウィジェットが見る画面 (論理 px) を作る。
//   - 画面の大きさ = 解像度 / 倍率 (Qt の QHighDpi::fromNativePixels と同じく丸める。1366x768 の 125% は 1093x614)。
//   - 作業領域 = 画面の高さ - タスクバー 40 (Windows 10 の既定の、下にあるタスクバーは 100% で 40 px で、倍率と同じ
//     割合で太くなるので、論理 px では 40 のまま。1366x768 の 125% で 574、1920x1080 の 175% で 577)。
//   - 画像の拡大率 = 倍率 (--dpr と同じ。ウィジェットの大きさと配置は論理 px のまま、描画だけを倍率のピクセルで行う)。
// ウィンドウの枠 (タイトルバーと上下の枠) の高さは、cordmuxpreview では測れない (QPlatformWindow の frameMargins は
// 常に 0)。そのため製品のダイアログは、測れないときの 40 px (dock/ui/message_dialog.cpp の kFrameFallbackPx) を使う。
// Windows 10 の実際の枠は約 39 px なので、1 px 厳しい側になる。
// 画面の値は、minimal でも使う Qt の handleScreenGeometryChange で変える。cordmuxpreview に画面の値を変える関数を
// 追加しないのは、同じプラグインを使う dock_drive の動作に影響する経路が増えるためである。ウィンドウごとに
// set_message_dialog_work_area で作業領域を指定しないのは、製品が画面から作業領域を読む処理を通らず、設定ウィンドウと
// 参加者の設定ウィンドウの最初の大きさ (作業領域の 85%) にも適用されないためである。変わるのは QScreen が持つ値だけで、
// cordmuxpreview の PreviewScreen::geometry() は 1920x1080 を返したままになる。製品とプレビューツールは QScreen の
// 関数 (availableGeometry など) だけを読むので、これで足りる。
struct ScreenChoice {
	bool given = false;
	int physical_w = 1920;
	int physical_h = 1080;
	int scale_pct = 100;
	QString tag() const
	{
		return given ? QStringLiteral("%1x%2@%3").arg(physical_w).arg(physical_h).arg(scale_pct) : QString();
	}
	QRect geometry() const
	{
		return QRect(0, 0, qRound(physical_w * 100.0 / scale_pct), qRound(physical_h * 100.0 / scale_pct));
	}
	QRect available() const
	{
		const QRect g = geometry();
		return QRect(0, 0, g.width(), qRound(physical_h * 100.0 / scale_pct - kTaskbarPx));
	}
	static constexpr int kTaskbarPx = 40;
};

// "1366x768@125" -> ScreenChoice。形が違えば false。
bool parse_screen(const QString &v, ScreenChoice *out)
{
	const QStringList at = v.split(QLatin1Char('@'));
	if (at.size() != 2)
		return false;
	const QStringList wh = at[0].split(QLatin1Char('x'));
	if (wh.size() != 2)
		return false;
	bool ok_w = false, ok_h = false, ok_s = false;
	out->physical_w = wh[0].toInt(&ok_w);
	out->physical_h = wh[1].toInt(&ok_h);
	out->scale_pct = at[1].toInt(&ok_s);
	if (!ok_w || !ok_h || !ok_s || out->scale_pct < 100 || out->scale_pct > 400 || out->physical_w < 640 ||
	    out->physical_h < 480)
		return false;
	out->given = true;
	return out->available().height() > 0;
}

struct Options {
	QString out_dir;
	QStringList themes{QStringLiteral("yami"), QStringLiteral("yami-light")};
	QStringList langs{QStringLiteral("ja-JP"), QStringLiteral("en-US")};
	QString only;             // この文字列を名前に含む場面だけを描画する
	QString obs_source = QStringLiteral(CORDMUX_UI_PREVIEW_OBS_SOURCE_DIR);
	QString locale_dir = QStringLiteral(CORDMUX_UI_PREVIEW_LOCALE_DIR);
	bool dump_theme = false;
	bool probe_fonts = false;
	bool list = false;
	// 描画の対象 (faces.h の FaceOptions::face)。既定はドック。
	QString face = QStringLiteral("dock");
	QList<double> widths_em{15.0, 20.0, 26.0, 32.0, 45.0};
	QList<QSize> sizes;
	int height = 600;
	cordmux::dock::ui::DockStyle style;
	// OBS の [外観] の [フォントサイズ] と [密度] (既定値は OBS と同じ 10 と [通常])。
	Appearance appearance;
	// 画像の拡大率 (1.5 は表示の倍率 150%)。
	double dpr = 1.0;
	bool dpr_given = false;
	// 描画する画面 (--screen 1366x768@125。解像度と表示の倍率)。既定は 1920x1080 の 100% (cordmuxpreview の画面)。
	ScreenChoice screen;
	// 出力先のフォルダーの名前に付ける OBS のバージョン (--obs 32 のとき "obs32"。既定の 31.1.1 では空)。
	QString obs_tag;
	QString font_source; // フォントのフォルダー (--obs-install のとき。空なら obs_source)
	bool at_min_width = false;
	bool at_status_floor = false;
	std::optional<cordmux::config::DockTab> tab;
};

// [密度] の名前か OBS の番号 (user.ini の [Appearance] Density) -> OBS の番号。分からなければ 0。
int density_id_of(const QString &v)
{
	if (v == QLatin1String("classic"))
		return -2;
	if (v == QLatin1String("compact"))
		return -3;
	if (v == QLatin1String("normal"))
		return -4;
	if (v == QLatin1String("comfortable"))
		return -5;
	bool ok = false;
	const int id = v.toInt(&ok);
	return ok ? id : 0;
}

// テーマの短い名前 -> OBS のテーマの id。
QString theme_id_of(const QString &name)
{
	if (name == QLatin1String("yami"))
		return QStringLiteral("com.obsproject.Yami.Original"); // OBS 31 の既定 (DEFAULT_THEME)
	if (name == QLatin1String("yami-light"))
		return QStringLiteral("com.obsproject.Yami.Light");
	if (name == QLatin1String("yami-classic"))
		return QStringLiteral("com.obsproject.Yami.Classic");
	if (name == QLatin1String("system"))
		return QStringLiteral("com.obsproject.System");
	return name; // id をそのまま渡したとき
}

bool parse_args(const QStringList &args, Options *opt, QString *error)
{
	for (int i = 1; i < args.size(); ++i) {
		const QString &a = args[i];
		const auto value = [&](QString *out) {
			if (i + 1 >= args.size()) {
				*error = QStringLiteral("%1 needs a value").arg(a);
				return false;
			}
			*out = args[++i];
			return true;
		};
		QString v;
		if (a == QLatin1String("--out")) {
			if (!value(&opt->out_dir))
				return false;
		} else if (a == QLatin1String("--themes")) {
			if (!value(&v))
				return false;
			opt->themes = v.split(QLatin1Char(','), Qt::SkipEmptyParts);
		} else if (a == QLatin1String("--langs")) {
			if (!value(&v))
				return false;
			opt->langs = v.split(QLatin1Char(','), Qt::SkipEmptyParts);
		} else if (a == QLatin1String("--face")) {
			if (!value(&opt->face))
				return false;
		} else if (a == QLatin1String("--widths-em")) {
			if (!value(&v))
				return false;
			// none なら em の幅では描画しない (--sizes と --at-min-width の大きさだけで描画する)。
			opt->widths_em.clear();
			for (const QString &w : v.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
				if (w != QLatin1String("none"))
					opt->widths_em.push_back(w.toDouble());
			}
		} else if (a == QLatin1String("--at-min-width")) {
			// 製品が計算した最小幅でも描画する (大きさの名前は min)。
			opt->at_min_width = true;
		} else if (a == QLatin1String("--at-status-floor")) {
			// 行の状態のテキストを表示する、いちばん狭い幅でも描画する (状態のテキストは下限の 5.75 em。
			// 大きさの名前は floor)。
			opt->at_status_floor = true;
		} else if (a == QLatin1String("--tab")) {
			// ドックで開くタブ (faces.h の FaceOptions::tab)。
			QString tab;
			if (!value(&tab))
				return false;
			if (tab == QLatin1String("people"))
				opt->tab = cordmux::config::DockTab::People;
			else if (tab == QLatin1String("audio"))
				opt->tab = cordmux::config::DockTab::Audio;
			else if (tab == QLatin1String("display"))
				opt->tab = cordmux::config::DockTab::Display;
			else if (tab == QLatin1String("status"))
				opt->tab = cordmux::config::DockTab::Status;
			else
				return false;
		} else if (a == QLatin1String("--sizes")) {
			// px で指定する大きさ (例 230x165,420x230)。
			if (!value(&v))
				return false;
			for (const QString &size : v.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
				const QStringList wh = size.split(QLatin1Char('x'));
				if (wh.size() != 2) {
					*error = QStringLiteral("--sizes takes WxH,WxH: %1").arg(size);
					return false;
				}
				opt->sizes.push_back(QSize(wh[0].toInt(), wh[1].toInt()));
			}
		} else if (a == QLatin1String("--height")) {
			if (!value(&v))
				return false;
			opt->height = v.toInt();
		} else if (a == QLatin1String("--first-line-scale")) {
			// 1 行目のフォントの倍率 (DockStyle::first_line_scale。例 1.25、1.5)。既定値は
			// dock/ui/dock_style.h にある。
			if (!value(&v))
				return false;
			opt->style.first_line_scale = v.toDouble();
		} else if (a == QLatin1String("--heading-scale")) {
			// 表示タブの大見出しの倍率 (DockStyle::group_heading_scale。例 1、1.1、1.25)。
			if (!value(&v))
				return false;
			opt->style.group_heading_scale = v.toDouble();
		} else if (a == QLatin1String("--heading-space")) {
			// 見出しの段の余白 (em)。5 つをカンマで並べる: 大見出しの罫線の上、罫線から大見出し、大見出しの下、中見出しの上、
			// 中見出しの下 (DockStyle の group_rule_above_em から sub_heading_below_em。既定 1,1,0.5,2.25,0.25)。
			if (!value(&v))
				return false;
			const QStringList parts = v.split(QLatin1Char(','));
			if (parts.size() != 5) {
				*error = QStringLiteral("--heading-space takes 5 values in em: %1").arg(v);
				return false;
			}
			opt->style.group_rule_above_em = parts[0].toDouble();
			opt->style.group_heading_above_em = parts[1].toDouble();
			opt->style.group_heading_below_em = parts[2].toDouble();
			opt->style.sub_heading_above_em = parts[3].toDouble();
			opt->style.sub_heading_below_em = parts[4].toDouble();
		} else if (a == QLatin1String("--status-heading-above")) {
			// 状態タブの見出しの上の余白 (em。DockStyle::status_heading_above_em)。
			if (!value(&v))
				return false;
			opt->style.status_heading_above_em = v.toDouble();
		} else if (a == QLatin1String("--note-below")) {
			// 説明の文の下の余白 (em。DockStyle::note_below_em)。
			if (!value(&v))
				return false;
			opt->style.note_below_em = v.toDouble();
		} else if (a == QLatin1String("--section-gap")) {
			// 意味の違う部分の間の余白 (em。DockStyle::section_gap_em)。
			if (!value(&v))
				return false;
			opt->style.section_gap_em = v.toDouble();
		} else if (a == QLatin1String("--l1-border")) {
			// メッセージのレベル 1 のパネルの枠線の太さ (DockStyle::l1_border_px。-1 はコントラスト比で決め、
			// 0、1、2 はその太さに固定する)。
			if (!value(&v))
				return false;
			opt->style.l1_border_px = v.toInt();
		} else if (a == QLatin1String("--tier-icons")) {
			// メッセージのレベルのアイコン (DockStyle::tier_icon_set。theme は cordmux が描く 1 色のアイコン、
			// windows は Windows の標準の色付きのアイコン)。
			if (!value(&v))
				return false;
			opt->style.tier_icon_set = v == QLatin1String("windows") ? cordmux::dock::ui::TierIconSet::WindowsStandard
										  : cordmux::dock::ui::TierIconSet::ThemeMono;
		} else if (a == QLatin1String("--dialog-main-scale")) {
			// ダイアログの主文の倍率 (DockStyle::dialog_main_scale。0 なら OBS のテーマのクラス
			// text-large のまま)。
			if (!value(&v))
				return false;
			opt->style.dialog_main_scale = v.toDouble();
			if (opt->style.dialog_main_scale < 0.0 || opt->style.dialog_main_scale > 4.0) {
				*error = QStringLiteral("--dialog-main-scale takes 0 (theme) or a scale up to 4.0");
				return false;
			}
		} else if (a == QLatin1String("--only")) {
			if (!value(&opt->only))
				return false;
		} else if (a == QLatin1String("--obs-source")) {
			if (!value(&opt->obs_source))
				return false;
		} else if (a == QLatin1String("--obs-install") || a == QLatin1String("--obs")) {
			// --obs-install DIR は、OBS をインストールした形のフォルダー (data/obs-studio/themes と
			// locale を持つ) のテーマと ini を読む。--obs 32 は、CORDMUX_UI_PREVIEW_OBS32_DIR
			// (ui_preview.cmake) の OBS 32.2.2 を指定するのと同じ。インストールした形にはフォントの
			// ファイルがない (OBS の実行ファイルの中にある) ので、フォントは既定の OBS のソース (31.1.1) の
			// Open Sans を使う。
			if (!value(&v))
				return false;
			if (a == QLatin1String("--obs")) {
				if (v == QLatin1String("31"))
					continue;
				if (v != QLatin1String("32")) {
					*error = QStringLiteral("--obs takes 31 or 32");
					return false;
				}
				v = QStringLiteral(CORDMUX_UI_PREVIEW_OBS32_DIR);
				opt->obs_tag = QStringLiteral("obs32");
			} else {
				opt->obs_tag = QStringLiteral("obs-") + QFileInfo(v).fileName();
			}
			opt->font_source = opt->obs_source;
			opt->obs_source = v;
		} else if (a == QLatin1String("--font-size")) {
			// OBS の [外観] の [フォントサイズ] (8、10、14 など)。
			if (!value(&v))
				return false;
			opt->appearance.font_scale = v.toInt();
			if (opt->appearance.font_scale <= 0) {
				*error = QStringLiteral("--font-size takes a positive number");
				return false;
			}
		} else if (a == QLatin1String("--density")) {
			// OBS の [外観] の [密度]: classic / compact / normal / comfortable か OBS の番号 (-2 から -5)。
			if (!value(&v))
				return false;
			const int id = density_id_of(v);
			if (id < -5 || id > -2) {
				*error = QStringLiteral("--density takes classic|compact|normal|comfortable or -2..-5: %1").arg(v);
				return false;
			}
			opt->appearance.density = id;
		} else if (a == QLatin1String("--dpr")) {
			if (!value(&v))
				return false;
			opt->dpr = v.toDouble();
			opt->dpr_given = true;
			if (opt->dpr < 1.0 || opt->dpr > 4.0) {
				*error = QStringLiteral("--dpr takes 1.0 to 4.0");
				return false;
			}
		} else if (a == QLatin1String("--screen")) {
			// 描画する画面 (解像度@表示の倍率の百分率。例 1366x768@125)。上の ScreenChoice の
			// コメントにある。
			if (!value(&v))
				return false;
			if (!parse_screen(v, &opt->screen)) {
				*error = QStringLiteral("--screen takes WxH@PERCENT (e.g. 1366x768@125, 100..400): %1").arg(v);
				return false;
			}
		} else if (a == QLatin1String("--locale-dir") || a == QLatin1String("--ini-dir")) {
			// --ini-dir は、読み込む cordmux の ini のフォルダーを変える (--locale-dir も同じ意味で
			// 受け付ける)。
			if (!value(&opt->locale_dir))
				return false;
		} else if (a == QLatin1String("--dump-theme")) {
			opt->dump_theme = true;
		} else if (a == QLatin1String("--probe-fonts")) {
			opt->probe_fonts = true;
		} else if (a == QLatin1String("--list")) {
			opt->list = true;
		} else if (a == QLatin1String("--backend")) {
			++i; // main() が QApplication を作る前に読んでいる
		} else {
			*error = QStringLiteral("unknown argument: %1").arg(a);
			return false;
		}
	}
	if (opt->out_dir.isEmpty() && !opt->list && !opt->probe_fonts) {
		*error = QStringLiteral("--out DIR is required");
		return false;
	}
	if (opt->screen.given) {
		// 表示の倍率は --screen が決める (画像の拡大率は表示の倍率と同じ)。--dpr に別の値を指定すると、画面と
		// 画像の倍率が食い違うので、エラーにする。
		const double scale = opt->screen.scale_pct / 100.0;
		if (opt->dpr_given && qAbs(opt->dpr - scale) > 1e-9) {
			*error = QStringLiteral("--screen sets the scale (%1); drop --dpr or give the same value").arg(scale);
			return false;
		}
		opt->dpr = scale;
	}
	return true;
}

void print(const QString &line)
{
	std::fputs(line.toUtf8().constData(), stdout);
	std::fputs("\n", stdout);
	std::fflush(stdout);
}

bool write_text(const QString &path, const QString &text)
{
	QDir().mkpath(QFileInfo(path).absolutePath());
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return false;
	file.write(text.toUtf8());
	return true;
}

// フォントの確認 (--probe-fonts)。製品と同じ QSS のフォントでテキストを組んだとき、どのフォントのグリフが使われたかを
// 出力する。
void probe_fonts()
{
	QLabel label;
	label.ensurePolished();
	const QFont font = label.font();
	print(QStringLiteral("widget font: families=%1 pointSizeF=%2 pixelSize=%3")
		      .arg(font.families().join(QStringLiteral(" | ")))
		      .arg(font.pointSizeF())
		      .arg(font.pixelSize()));
	const QFontMetrics fm(font);
	print(QStringLiteral("metrics: height=%1 ascent=%2 descent=%3 leading=%4 maxWidth=%5 averageCharWidth=%6")
		      .arg(fm.height())
		      .arg(fm.ascent())
		      .arg(fm.descent())
		      .arg(fm.leading())
		      .arg(fm.maxWidth())
		      .arg(fm.averageCharWidth()));
	const QStringList samples{QStringLiteral("Discord 側 つながっていません"), QStringLiteral("参加者"),
				  QStringLiteral("機能の ON/OFF を切り替える"), QStringLiteral("Vesktop を起動し直してください"),
				  QStringLiteral("さくら🌸 Mika ✨")};
	for (const QString &text : samples) {
		QTextLayout layout(text, font);
		layout.beginLayout();
		QTextLine line = layout.createLine();
		layout.endLayout();
		QStringList parts;
		for (const QGlyphRun &run : layout.glyphRuns()) {
			const QRawFont raw = run.rawFont();
			parts << QStringLiteral("%1 (%2px, %3 glyphs)")
					 .arg(raw.familyName())
					 .arg(raw.pixelSize())
					 .arg(run.glyphIndexes().size());
		}
		print(QStringLiteral("  \"%1\" width=%2 -> %3").arg(text).arg(line.naturalTextWidth()).arg(parts.join(QStringLiteral(", "))));
	}
}

// テーマの QSS とパレットを書き出す (--dump-theme)。OBS と同じ QSS とパレットになっているかを確認するために使う。
int dump_themes(QApplication &app, const Options &opt, const ObsPaths &paths)
{
	for (const QString &theme_name : opt.themes) {
		ComposedTheme theme;
		QString error;
		if (!apply_obs_theme(app, paths, theme_id_of(theme_name), g_preview_platform, &theme, &error)) {
			print(QStringLiteral("theme %1: %2").arg(theme_name, error));
			return 1;
		}
		const QString base = opt.out_dir + QStringLiteral("/theme/") + theme_name;
		write_text(base + QStringLiteral(".qss"), theme.stylesheet);
		QStringList vars;
		for (const auto &[name, value] : theme.resolved)
			vars << name + QStringLiteral(" = ") + value;
		write_text(base + QStringLiteral(".vars.txt"), vars.join(QLatin1Char(10)) + QLatin1Char(10));
		QStringList pal;
		for (int g = 0; g < 3; ++g) {
			for (int r = 0; r < QPalette::NColorRoles; ++r) {
				pal << QStringLiteral("%1 %2 %3").arg(g).arg(r).arg(
					theme.palette.color(QPalette::ColorGroup(g), QPalette::ColorRole(r)).name(QColor::HexArgb));
			}
		}
		write_text(base + QStringLiteral(".palette.txt"), pal.join(QLatin1Char(10)) + QLatin1Char(10));
	}
	return 0;
}

} // namespace

int main(int argc, char **argv)
{
	// プラットフォームプラグインを選ぶ (QApplication を作る前に決める必要があるので、この引数だけを先に読む)。
	QByteArray platform = QByteArray(kPreviewPlatformName);
	for (int i = 1; i + 1 < argc; ++i) {
		if (std::strcmp(argv[i], "--backend") == 0)
			platform = QByteArray(argv[i + 1]);
	}
	if (platform != QByteArray(kPreviewPlatformName) && platform != QByteArray("minimal")) {
		std::fprintf(stderr, "ui_preview: --backend must be %s or minimal\n", kPreviewPlatformName);
		return 2;
	}
	g_preview_platform = platform == QByteArray(kPreviewPlatformName);
	// ウィンドウを表示しない (冒頭のコメント)。minimal には enable_fonts を付けて、フォントを使えるようにする
	// (tests/decor/main.cpp と同じ。理由はそこのコメントにある)。
	qputenv("QT_QPA_PLATFORM", g_preview_platform ? platform : QByteArray("minimal:enable_fonts"));
	// 100% の拡大率で描画する (実行する PC の表示の倍率に左右されないように)。
	qputenv("QT_ENABLE_HIGHDPI_SCALING", QByteArray("0"));
	QApplication::setAttribute(Qt::AA_Use96Dpi);
	// OBS の OBSApp のコンストラクターと同じ (OBS の frontend/OBSApp.cpp)。
	QApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);

	QApplication app(argc, argv);
	if (QGuiApplication::platformName() != QString::fromLatin1(g_preview_platform ? platform : QByteArray("minimal"))) {
		std::fprintf(stderr, "ui_preview: refusing to run on platform %s\n",
			     QGuiApplication::platformName().toUtf8().constData());
		return 3;
	}
	Options opt;
	QString error;
	if (!parse_args(app.arguments(), &opt, &error)) {
		std::fprintf(stderr, "ui_preview: %s\n", error.toUtf8().constData());
		return 2;
	}
	if (!g_preview_platform || opt.screen.given) {
		// minimal の画面は 240 x 320 で、ダイアログの幅の上限 (QMessageBox) やメニューが表示される位置が
		// 変わるので、cordmuxpreview と同じ大きさ (1920 x 1080、下にタスクバー 40 px) に広げる。
		// --screen があれば、どちらのプラグインでもその画面にする (ウィジェットを 1 つも作る前に変える)。
		QWindowSystemInterface::handleScreenGeometryChange(app.primaryScreen(), opt.screen.geometry(),
								   opt.screen.available());
		QWindowSystemInterface::flushWindowSystemEvents();
		if (opt.screen.given)
			print(QStringLiteral("screen %1: logical %2x%3, work area %4x%5, dpr %6")
				      .arg(opt.screen.tag())
				      .arg(app.primaryScreen()->geometry().width())
				      .arg(app.primaryScreen()->geometry().height())
				      .arg(app.primaryScreen()->availableGeometry().width())
				      .arg(app.primaryScreen()->availableGeometry().height())
				      .arg(opt.dpr));
	}

	// 網羅のチェック (fixtures.h の uncovered_rules)。どの場面でも成り立たない表の行が 1 つでもあれば終了する。
	const std::vector<std::string> missing = uncovered_rules(all_fixtures());
	if (!missing.empty()) {
		for (const std::string &rule : missing)
			std::fprintf(stderr, "ui_preview: no fixture reaches %s\n", rule.c_str());
		return 4;
	}

	if (opt.list) {
		// 場面ごとに、成り立った表の行 (primary と notices の番号。番号は ledger/rules.h の列挙の順) を
		// 出力する。
		for (const Fixture &f : all_fixtures()) {
			const cordmux::ledger::Observation obs = observation_for(f);
			const cordmux::ledger::PrimaryRuleId primary = cordmux::ledger::primary_rule_of(obs);
			int primary_index = -1;
			for (std::size_t i = 0; i < cordmux::ledger::primary_rule_count(); ++i) {
				if (cordmux::ledger::primary_rule_id_at(i) == primary)
					primary_index = static_cast<int>(i);
			}
			QStringList notices;
			for (std::size_t i = 0; i < cordmux::ledger::notice_rule_count(); ++i) {
				if (cordmux::ledger::notice_rule_fires(cordmux::ledger::notice_rule_id_at(i), obs))
					notices << QString::number(i);
			}
			print(QStringList{QString::fromStdString(f.id), QStringLiteral("primary=") + QString::number(primary_index),
					  QStringLiteral("notices=") +
						  (notices.isEmpty() ? QStringLiteral("-") : notices.join(QLatin1Char(','))),
					  QString::fromStdString(f.note)}
				      .join(QLatin1Char('\t')));
		}
		return 0;
	}

	ObsPaths paths;
	paths.themes_dir = opt.obs_source + QStringLiteral("/frontend/data/themes");
	paths.fonts_dir = opt.obs_source + QStringLiteral("/frontend/forms/fonts");
	if (!opt.font_source.isEmpty()) {
		// OBS をインストールした形のフォルダー (--obs-install と --obs 32)。
		paths.themes_dir = opt.obs_source + QStringLiteral("/data/obs-studio/themes");
		paths.fonts_dir = opt.font_source + QStringLiteral("/frontend/forms/fonts");
	}
	if (!QFileInfo(paths.themes_dir + QStringLiteral("/Yami.obt")).exists()) {
		std::fprintf(stderr, "ui_preview: OBS themes not found under %s (pass --obs-source)\n",
			     paths.themes_dir.toUtf8().constData());
		return 2;
	}
	if (!load_obs_fonts(paths, &error)) {
		std::fprintf(stderr, "ui_preview: %s\n", error.toUtf8().constData());
		return 2;
	}

	if (opt.probe_fonts) {
		ComposedTheme theme;
		if (!apply_obs_theme(app, paths, theme_id_of(QStringLiteral("yami")), g_preview_platform, &theme, &error)) {
			std::fprintf(stderr, "ui_preview: %s\n", error.toUtf8().constData());
			return 2;
		}
		probe_fonts();
		return 0;
	}

	if (opt.dump_theme)
		return dump_themes(app, opt, paths);

	// 描画の対象を描画する (faces.h。既定はドック)。
	FaceOptions f;
	f.face = opt.face;
	f.out_dir = opt.out_dir;
	f.themes = opt.themes;
	f.langs = opt.langs;
	f.widths_em = opt.widths_em;
	f.sizes = opt.sizes;
	f.height = opt.height;
	f.only = opt.only;
	f.locale_dir = opt.locale_dir;
	f.obs_source = opt.obs_source;
	f.platform_defaults = g_preview_platform;
	f.style = opt.style;
	f.appearance = opt.appearance;
	f.dpr = opt.dpr;
	f.screen_tag = opt.screen.tag();
	f.obs_tag = opt.obs_tag;
	f.at_min_width = opt.at_min_width;
	f.at_status_floor = opt.at_status_floor;
	f.tab = opt.tab;
	return run_face(app, paths, f);
}
