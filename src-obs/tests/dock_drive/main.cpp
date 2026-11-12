// SPDX-License-Identifier: Apache-2.0

// ドックを操作するテスト (dock_drive.exe) のエントリポイント。プラットフォームプラグインを確認し、
// 引数を読み、シナリオを順に実行する。使い方は README.md にある。
// 画面を 1 つも開かない。Qt のプラットフォームプラグインは必ずプレビュー用の
// cordmuxpreview (tests/ui_preview/preview_platform.cpp) にし、環境変数でほかのプラグインが指定されていても上書きする。
// それでもほかのプラグインで起動したときは、何もせずに終わる。cordmuxpreview は OS のウィンドウを作らず、入力も
// Qt に渡すだけで、OS の入力には何も送らない (SendInput も SetCursorPos も使わない)。アクセシビリティのイベントは
// テストが受け取るので、Windows に届かず、音も出ない。

#include <QApplication>
#include <QByteArray>
#include <QChildEvent>
#include <QDir>
#include <QGuiApplication>
#include <QStringList>
#include <QStyleHints>
#include <QtPlugin>

#include <qpa/qwindowsysteminterface.h>

#include <cstdio>

#include "diag/diag_log.h"
#include "drive_env.h"
#include "mistap.h"
#include "preview_platform.h"
#include "probe.h"
#include "scenarios.h"
#include "crash_trace.h"
#include "report.h"

Q_IMPORT_PLUGIN(CordmuxPreviewIntegrationPlugin)

using namespace cordmux::dock_drive;

namespace {

struct Options {
	EnvOptions env;
	QString out_dir;
	QStringList scenarios{QStringLiteral("probe")};
	QList<int> seeds{1, 2, 3, 4, 5};
	double minutes = 10.0;
	int dblclick_ms = 500;
	cordmux::dock::ui::DockStyle style;
};

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
		// --dir と --out は同じ。PowerShell のスクリプトを通して実行すると、PowerShell が
		// --out を自分の -OutVariable か -OutBuffer と取り違えることがあるので、そのときは --dir を使う。
		if (a == QLatin1String("--out") || a == QLatin1String("--dir")) {
			if (!value(&opt->out_dir))
				return false;
		} else if (a == QLatin1String("--scenario")) {
			if (!value(&v))
				return false;
			opt->scenarios = v.split(QLatin1Char(','), Qt::SkipEmptyParts);
		} else if (a == QLatin1String("--seeds")) {
			if (!value(&v))
				return false;
			opt->seeds.clear();
			for (const QString &s : v.split(QLatin1Char(','), Qt::SkipEmptyParts))
				opt->seeds.push_back(s.toInt());
		} else if (a == QLatin1String("--minutes")) {
			if (!value(&v))
				return false;
			opt->minutes = v.toDouble();
		} else if (a == QLatin1String("--theme")) {
			if (!value(&opt->env.theme))
				return false;
		} else if (a == QLatin1String("--lang")) {
			if (!value(&opt->env.lang))
				return false;
		} else if (a == QLatin1String("--font-scale")) {
			if (!value(&v))
				return false;
			opt->env.font_scale = v.toInt();
		} else if (a == QLatin1String("--density")) {
			if (!value(&v))
				return false;
			opt->env.density = v.toInt();
		} else if (a == QLatin1String("--dblclick-ms")) {
			// クリックを捨てるガードの時間 (ダブルクリックの時間) を変える。0 にするとガードが止まるので、
			// 押し間違いを数えられることを確認できる (mistap のセルフテスト。README.md にある)。
			if (!value(&v))
				return false;
			opt->dblclick_ms = v.toInt();
		} else if (a == QLatin1String("--obs-source")) {
			if (!value(&opt->env.obs_source))
				return false;
		} else if (a == QLatin1String("--ini-dir")) {
			if (!value(&opt->env.locale_dir))
				return false;
		} else {
			*error = QStringLiteral("unknown argument: %1").arg(a);
			return false;
		}
	}
	if (opt->out_dir.isEmpty()) {
		*error = QStringLiteral("--dir DIR is required");
		return false;
	}
	return true;
}

// 製品の diag::logf の出力先。状態を変えない区間にログの行がないことを確認するため、行を数えるだけにする。
std::int64_t g_log_lines = 0;
void count_log_line(int, cordmux::diag::LineKind, std::string_view)
{
	++g_log_lines;
}

} // namespace

std::int64_t drive_log_lines()
{
	return g_log_lines;
}

namespace {

// クラッシュする直前の記録 (crash_trace.h): Enter と Leave のイベントを受けたウィジェットと、破棄されたか外された子の
// アドレスを残す。アドレスは型を読む前に残す。受けたウィジェットが既に破棄されていれば、
// 型を読むところでクラッシュするので、そのアドレスが前に破棄された子のアドレスと同じかで分ける。
class NoteFilter : public QObject {
public:
	bool eventFilter(QObject *o, QEvent *e) override
	{
		const QEvent::Type t = e->type();
		if (t == QEvent::Enter || t == QEvent::Leave || t == QEvent::DeferredDelete) {
			char note[160];
			std::snprintf(note, sizeof(note), "%s %p", t == QEvent::Enter ? "enter" : t == QEvent::Leave ? "leave" : "deferred-delete",
				      static_cast<void *>(o));
			cordmux::dock_drive::crash_note(note);
			std::snprintf(note, sizeof(note), "  = %s '%s'", o->metaObject()->className(), o->objectName().toUtf8().constData());
			cordmux::dock_drive::crash_note(note);
		} else if (t == QEvent::ChildRemoved) {
			auto *ce = static_cast<QChildEvent *>(e);
			if (ce->child() != nullptr && ce->child()->isWidgetType()) {
				char note[160];
				std::snprintf(note, sizeof(note), "child-removed %p from %p", static_cast<void *>(ce->child()), static_cast<void *>(o));
				cordmux::dock_drive::crash_note(note);
			}
		}
		return false;
	}
};

} // namespace

// 終了コード: 0 はすべての確認が合格、1 は合格しない確認がある、2 は引数の誤り、3 はプラットフォームプラグインが違う、
// 4 は環境を用意できない。
int main(int argc, char **argv)
{
	// クラッシュしたら、その場所を標準エラー出力に書く (crash_trace.h)。
	cordmux::dock_drive::install_crash_trace();
	const QByteArray platform(cordmux::ui_preview::kPreviewPlatformName);
	// 画面を開かないプラットフォームプラグインだけを使う (ファイルの先頭のコメント)。
	// 表示の倍率 100% (96 dpi) で描く。
	qputenv("QT_QPA_PLATFORM", platform);
	qputenv("QT_ENABLE_HIGHDPI_SCALING", QByteArray("0"));
	QApplication::setAttribute(Qt::AA_Use96Dpi);
	QApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);

	QApplication app(argc, argv);
	NoteFilter note_filter;
	app.installEventFilter(&note_filter);
	if (QGuiApplication::platformName() != QString::fromLatin1(platform)) {
		std::fprintf(stderr, "dock_drive: refusing to run on platform %s\n",
			     QGuiApplication::platformName().toUtf8().constData());
		return 3;
	}
	// ダブルクリックの時間は Windows の既定 (GetDoubleClickTime の 500 ms) にする。製品のクリックを捨てる
	// ガードはこの値を読む。プレビュー用のプラットフォームプラグインのテーマはこの値を返さず、Qt の既定の
	// 400 ms になるので、Windows のプラットフォームプラグインが返す値にそろえる。実行する PC の設定は読まない。
	// 読むと PC ごとに結果が変わるためで、プレビュー用のプラットフォームプラグインも PC の設定を読まない。
	QGuiApplication::styleHints()->setMouseDoubleClickInterval(500);

	Options opt;
	opt.env.obs_source = QStringLiteral(CORDMUX_DOCK_DRIVE_OBS_SOURCE_DIR);
	opt.env.locale_dir = QStringLiteral(CORDMUX_DOCK_DRIVE_LOCALE_DIR);
	QString error;
	if (!parse_args(app.arguments(), &opt, &error)) {
		std::fprintf(stderr, "dock_drive: %s\n", error.toUtf8().constData());
		return 2;
	}
	QGuiApplication::styleHints()->setMouseDoubleClickInterval(opt.dblclick_ms);
	if (!setup_env(app, opt.env, &error)) {
		std::fprintf(stderr, "dock_drive: %s\n", error.toUtf8().constData());
		return 4;
	}
	cordmux::diag::set_log_sink(&count_log_line);

	Results results;
	say(QStringLiteral("dock_drive: theme=%1 lang=%2 font_scale=%3 density=%4")
		    .arg(opt.env.theme, opt.env.lang)
		    .arg(opt.env.font_scale)
		    .arg(opt.env.density));
	for (const QString &scenario : opt.scenarios) {
		if (scenario == QLatin1String("probe")) {
			run_probe(opt.style, results);
		} else if (scenario == QLatin1String("keyboard")) {
			run_keyboard(ScenarioOptions{opt.out_dir, opt.style}, results);
		} else if (scenario == QLatin1String("windows")) {
			run_windows(ScenarioOptions{opt.out_dir, opt.style}, results);
		} else if (scenario == QLatin1String("record")) {
			run_record(ScenarioOptions{opt.out_dir, opt.style}, results);
		} else if (scenario == QLatin1String("a11y")) {
			run_a11y(ScenarioOptions{opt.out_dir, opt.style}, results);
		} else if (scenario == QLatin1String("menus")) {
			run_menus(ScenarioOptions{opt.out_dir, opt.style}, results);
		} else if (scenario == QLatin1String("layout")) {
			run_layout(ScenarioOptions{opt.out_dir, opt.style}, results);
		} else if (scenario == QLatin1String("dialogs")) {
			run_dialogs(ScenarioOptions{opt.out_dir, opt.style}, results);
		} else if (scenario == QLatin1String("summary")) {
			run_summary(ScenarioOptions{opt.out_dir, opt.style}, results);
		} else if (scenario == QLatin1String("audio")) {
			run_audio(ScenarioOptions{opt.out_dir, opt.style}, results);
		} else if (scenario == QLatin1String("rowaudio")) {
			run_row_audio(ScenarioOptions{opt.out_dir, opt.style}, results);
		} else if (scenario == QLatin1String("selfframe")) {
			run_self_frame(ScenarioOptions{opt.out_dir, opt.style}, results);
		} else if (scenario == QLatin1String("framework")) {
			run_framework(ScenarioOptions{opt.out_dir, opt.style}, results);
		} else if (scenario == QLatin1String("tools")) {
			run_tools_uninstall(ScenarioOptions{opt.out_dir, opt.style}, results);
		} else if (scenario == QLatin1String("pages")) {
			run_settings_pages(ScenarioOptions{opt.out_dir, opt.style}, results);
		} else if (scenario == QLatin1String("look")) {
			run_look(ScenarioOptions{opt.out_dir, opt.style}, results);
		} else if (scenario == QLatin1String("status")) {
			run_status(ScenarioOptions{opt.out_dir, opt.style}, results);
		} else if (scenario == QLatin1String("positions")) {
			run_positions(ScenarioOptions{opt.out_dir, opt.style}, results);
		} else if (scenario == QLatin1String("dispatch")) {
			run_dispatch(ScenarioOptions{opt.out_dir, opt.style}, results);
		} else if (scenario == QLatin1String("unload")) {
			run_unload(ScenarioOptions{opt.out_dir, opt.style}, results);
		} else if (scenario == QLatin1String("mistap")) {
			for (int seed : opt.seeds) {
				MistapOptions m;
				m.seed = seed;
				m.minutes = opt.minutes;
				m.out_dir = opt.out_dir;
				m.style = opt.style;
				run_mistap(m, results);
			}
		} else {
			std::fprintf(stderr, "dock_drive: unknown scenario %s\n", scenario.toUtf8().constData());
			return 2;
		}
	}
	results.print();
	results.write_tsv(opt.out_dir + QStringLiteral("/results.tsv"));
	cordmux::diag::set_log_sink(nullptr);
	teardown_env();
	return results.any_fail() ? 1 : 0;
}
