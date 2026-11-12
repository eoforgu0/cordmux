// SPDX-License-Identifier: Apache-2.0

// 名前ラベルの調整ツール (label_tuner.exe) のエントリポイント。使い方は tests/label_tuner/README.md。画面では Qt の
// 既定のプラットフォームプラグイン (Windows では windows) でウィンドウを開く。--shot、--sweep、--drive のときは
// ウィンドウを開かず、プレビュー用のプラットフォームプラグイン cordmuxdecorpreview (tests/decor_preview/decor_qpa.h。
// OS のウィンドウを作らない) の上でウィンドウを動かし、画像と JSON に書き出して終わる。

#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScreen>
#include <QStringList>
#include <QStyle>
#include <QStyleFactory>
#include <QTimer>
#include <QtPlugin>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "decor_qpa.h"
#include "tuner_knobs.h"
#include "tuner_state.h"
#include "tuner_drive.h"
#include "tuner_sweep.h"
#include "tuner_window.h"

Q_IMPORT_PLUGIN(CordmuxDecorPreviewIntegrationPlugin)

namespace label_tuner = cordmux::label_tuner;
namespace config = cordmux::config;

namespace {

// --shot の時刻の起点 (ns)。時刻 0 は「話していない」を表すので、起動からの 0 ms をこの時刻にする。
constexpr std::uint64_t kShotBaseNs = 1'000'000'000ULL;

// 文字が切れているウィジェットが見つかったか (--shot と --sweep は、あれば終了コードを 1 にする)。
bool g_clipped = false;

struct Args {
	QString save_path;
	QString load_path;
	QString shot_path;
	QString canvas_path;
	QString sweep_dir;
	QString drive_dir;
	QString state_text;
	bool help = false;
};

void print_usage()
{
	std::printf(
		"使い方: label_tuner.exe [--save <JSON>] [--load <JSON>] [--state <場面>]\n"
		"        label_tuner.exe --shot <PNG> [--canvas <PNG>] [--load <JSON>] [--state <場面>]\n"
		"  --save    [保存] で書く先。起動のときにあれば読み込む\n"
		"  --load    起動のときに読み込むつまみの値 (--save のファイルより先に使う)\n"
		"  --state   人数、人、話す時刻、背景などの指定 (src-obs/tests/label_tuner/README.md の「--state」)\n"
		"  --shot    ウィンドウを開かずに、ウィンドウを画像に描いて書き出して終わる。同じ名前の .json も書く\n"
		"  --canvas  --shot のとき、重ねたキャンバス (1920 x 1080) も書き出す\n"
		"        label_tuner.exe --sweep <フォルダー>\n"
		"  --sweep   つまみを 1 つずつ下の端、既定、上の端に動かしたウィンドウの画像を、フォルダーに書き出す\n"
		"        label_tuner.exe --drive <フォルダー>\n"
		"  --drive   描き直しのスレッドとタイマーと実際の時計で、決めた操作を入れて確認し、結果をフォルダーに書く\n");
}

bool parse_args(const QStringList &args, Args *out, QString *error)
{
	for (int i = 1; i < args.size(); ++i) {
		const QString a = args[i];
		if (a == QStringLiteral("--help") || a == QStringLiteral("-h")) {
			out->help = true;
			continue;
		}
		QString *target = nullptr;
		if (a == QStringLiteral("--save"))
			target = &out->save_path;
		else if (a == QStringLiteral("--load"))
			target = &out->load_path;
		else if (a == QStringLiteral("--shot"))
			target = &out->shot_path;
		else if (a == QStringLiteral("--canvas"))
			target = &out->canvas_path;
		else if (a == QStringLiteral("--state"))
			target = &out->state_text;
		else if (a == QStringLiteral("--sweep"))
			target = &out->sweep_dir;
		else if (a == QStringLiteral("--drive"))
			target = &out->drive_dir;
		if (!target) {
			*error = QStringLiteral("知らない引数です: %1").arg(a);
			return false;
		}
		if (i + 1 >= args.size()) {
			*error = QStringLiteral("%1 には値が要ります").arg(a);
			return false;
		}
		*target = args[++i];
	}
	if (!out->canvas_path.isEmpty() && out->shot_path.isEmpty()) {
		*error = QStringLiteral("--canvas は --shot と一緒に使います");
		return false;
	}
	if (!out->sweep_dir.isEmpty() && (!out->shot_path.isEmpty() || !out->load_path.isEmpty() ||
					  !out->save_path.isEmpty() || !out->state_text.isEmpty())) {
		*error = QStringLiteral("--sweep はほかの引数と一緒には使えません");
		return false;
	}
	if (!out->drive_dir.isEmpty() &&
	    (!out->shot_path.isEmpty() || !out->load_path.isEmpty() || !out->save_path.isEmpty() ||
	     !out->state_text.isEmpty() || !out->sweep_dir.isEmpty())) {
		*error = QStringLiteral("--drive はほかの引数と一緒には使えません");
		return false;
	}
	return true;
}

// つまみの値を読む。読めなければ false を返し、error に理由を書く (settings は製品の既定値になる)。刻みに合わない
// 数は刻みに合わせ、合わせたキーを snapped に入れる。
bool load_settings(const QString &path, config::Settings *settings, QString *error, QStringList *snapped)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		*error = QStringLiteral("読めません: %1").arg(path);
		return false;
	}
	const std::string text = file.readAll().toStdString();
	std::string reason;
	if (!label_tuner::load_knobs(text, settings, &reason)) {
		*error =
			QStringLiteral("値の形が違うので読めません: %1 (%2)").arg(path, QString::fromStdString(reason));
		return false;
	}
	for (const label_tuner::KnobDef &knob : label_tuner::knob_defs()) {
		if (knob.kind != label_tuner::KnobKind::Number)
			continue;
		const double value = knob.get_number(*settings);
		const double on_step = label_tuner::snap_to_step(knob, value);
		if (on_step != value) {
			knob.set_number(*settings, on_step);
			snapped->append(QStringLiteral("%1 %2 -> %3")
						.arg(QString::fromLatin1(knob.key))
						.arg(value)
						.arg(on_step));
		}
	}
	return true;
}

// たまっているレイアウトの要求と再描画を処理する (ウィンドウを開かないプラグインでも、レイアウトはイベントの順に進む)。
void settle()
{
	for (int i = 0; i < 4; ++i) {
		QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
		QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
	}
}

struct SpeakEvent {
	int ms = 0;
	int seat = 0; // roster が真なら、画面共有していない参加者の行 (0 から)
	bool cycle = false;
	bool on = false;
	bool roster = false;
};

// 話す時刻を、時刻の順 (同じ時刻なら止まりを先) に並べる。
std::vector<SpeakEvent> speak_events(const label_tuner::ToolState &state)
{
	std::vector<SpeakEvent> events;
	for (int seat = 0; seat < static_cast<int>(state.seats.size()); ++seat) {
		const label_tuner::SeatState &s = state.seats[static_cast<std::size_t>(seat)];
		for (const label_tuner::SpeakSpan &span : s.speak) {
			events.push_back({span.from_ms, seat, false, true});
			if (span.to_ms >= 0)
				events.push_back({span.to_ms, seat, false, false});
		}
		if (s.cycle_from_ms >= 0)
			events.push_back({s.cycle_from_ms, seat, true, true});
	}
	for (int row = 0; row < static_cast<int>(state.roster_rows.size()); ++row) {
		const label_tuner::RosterRowState &r = state.roster_rows[static_cast<std::size_t>(row)];
		for (const label_tuner::SpeakSpan &span : r.speak) {
			events.push_back({span.from_ms, row, false, true, true});
			if (span.to_ms >= 0)
				events.push_back({span.to_ms, row, false, false, true});
		}
		if (r.cycle_from_ms >= 0)
			events.push_back({r.cycle_from_ms, row, true, true, true});
	}
	std::stable_sort(events.begin(), events.end(), [](const SpeakEvent &a, const SpeakEvent &b) {
		if (a.ms != b.ms)
			return a.ms < b.ms;
		return !a.on && b.on;
	});
	return events;
}

// 画面で開いたとき、--state の speak と cycle を入れる。ウィンドウを出した時刻を 0 として、各時刻に実際の時計で
// 切り替える (--shot は描く時刻より前の切り替えをまとめて入れるので、ここは使わない)。
void schedule_speaking(label_tuner::TunerWindow &window, const label_tuner::ToolState &state,
		       const std::function<std::uint64_t()> &clock)
{
	for (const SpeakEvent &event : speak_events(state))
		QTimer::singleShot(event.ms, &window, [&window, clock, event]() {
			if (event.roster)
				window.set_roster_speaking(event.seat, event.cycle, event.on, clock());
			else
				window.set_speaking(event.seat, event.cycle, event.on, clock());
		});
}

// 描き直しを終え、等倍と 2 倍の表示の大きさとスクロールが決まった後の表示にする。スクロールエリアは、中の
// ウィジェットの大きさが変わった後のイベントで範囲を更新するので、1 回の tick では決まらないことがある。決まるまで
// (多くても 10 回) tick とイベントの処理を繰り返す。
void refresh(label_tuner::TunerWindow &window)
{
	for (int i = 0; i < 10; ++i) {
		window.tick();
		settle();
		if (i >= 1 && window.all_current())
			break;
	}
}

// 現在のウィンドウを path (PNG) に描き、同じ名前の .json に説明と extra を書く。canvas_path が空でなければ、重ねた
// キャンバスも書く。
bool write_shot(label_tuner::TunerWindow &window, const QString &path, const QJsonObject &extra,
		const QString &canvas_path)
{
	if (!window.all_current()) {
		std::fprintf(stderr, "label_tuner: 描き直しが終わっていません: %s\n", path.toUtf8().constData());
		return false;
	}
	const double dpr = window.devicePixelRatioF();
	QImage image(QSize(static_cast<int>(std::lround(window.width() * dpr)),
			   static_cast<int>(std::lround(window.height() * dpr))),
		     QImage::Format_RGB32);
	image.setDevicePixelRatio(dpr);
	image.fill(QColor(255, 0, 255)); // 描かれない所があれば目立つ色
	window.render(&image);
	if (!QDir().mkpath(QFileInfo(path).absolutePath()) || !image.save(path)) {
		std::fprintf(stderr, "label_tuner: 書き出せません: %s\n", path.toUtf8().constData());
		return false;
	}
	QJsonObject root = window.describe(dpr);
	root[QStringLiteral("window")] = QJsonArray{window.width(), window.height()};
	for (auto it = extra.begin(); it != extra.end(); ++it)
		root[it.key()] = it.value();
	const QString json_path = QFileInfo(path).path() + QStringLiteral("/") + QFileInfo(path).completeBaseName() +
				  QStringLiteral(".json");
	QFile json(json_path);
	if (!json.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
	    json.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0) {
		std::fprintf(stderr, "label_tuner: 書き出せません: %s\n", json_path.toUtf8().constData());
		return false;
	}
	json.close();
	if (!canvas_path.isEmpty() && !window.canvas().convertToFormat(QImage::Format_RGB32).save(canvas_path)) {
		std::fprintf(stderr, "label_tuner: 書き出せません: %s\n", canvas_path.toUtf8().constData());
		return false;
	}
	const QJsonArray clipped = root.value(QStringLiteral("clipped_widgets")).toArray();
	if (!clipped.isEmpty()) {
		g_clipped = true;
		std::fprintf(stderr, "label_tuner: 文字が切れているウィジェットが %d 個あります: %s\n",
			     static_cast<int>(clipped.size()), path.toUtf8().constData());
	}
	std::printf("%s\n", QDir::toNativeSeparators(path).toUtf8().constData());
	return true;
}

int run_shot(label_tuner::TunerWindow &window, const label_tuner::ToolState &state, std::uint64_t *virtual_now,
	     const Args &args)
{
	window.resize(state.window_w, state.window_h);
	window.show();
	settle();
	// 描く時刻より後の切り替えは、まだ起きていないので入れない。
	for (const SpeakEvent &event : speak_events(state)) {
		if (event.ms > state.at_ms)
			break;
		*virtual_now = kShotBaseNs + static_cast<std::uint64_t>(event.ms) * 1'000'000ULL;
		if (event.roster)
			window.set_roster_speaking(event.seat, event.cycle, event.on, *virtual_now);
		else
			window.set_speaking(event.seat, event.cycle, event.on, *virtual_now);
	}
	*virtual_now = kShotBaseNs + static_cast<std::uint64_t>(state.at_ms) * 1'000'000ULL;
	if (!state.focus_key.isEmpty())
		window.focus_knob(state.focus_key.toStdString());
	if (state.press == QStringLiteral("save"))
		window.press_save();
	else if (state.press == QStringLiteral("reset"))
		window.reset_knobs();
	refresh(window);
	QJsonObject extra;
	extra[QStringLiteral("at_ms")] = state.at_ms;
	if (!write_shot(window, args.shot_path, extra, args.canvas_path))
		return 1;
	if (state.bench_frames <= 0)
		return g_clipped ? 1 : 0;

	// 1 フレームの時間を測る。時刻を 16 ms ずつ進めて tick し、ウィンドウ全体を画像に描く (画面では、変わったウィジェット
	// だけを描き直すので、これより軽い)。書き出した JSON に足す。
	std::vector<double> frames;
	double full_sum = 0.0;
	double crop_sum = 0.0;
	QImage image(QSize(static_cast<int>(std::lround(window.width() * window.devicePixelRatioF())),
			   static_cast<int>(std::lround(window.height() * window.devicePixelRatioF()))),
		     QImage::Format_RGB32);
	image.setDevicePixelRatio(window.devicePixelRatioF());
	for (int i = 0; i < state.bench_frames; ++i) {
		*virtual_now += 16'000'000ULL;
		QElapsedTimer timer;
		timer.start();
		window.tick();
		window.render(&image);
		frames.push_back(static_cast<double>(timer.nsecsElapsed()) / 1e6);
		full_sum += window.full_compose_ms();
		crop_sum += window.crop_compose_ms();
	}
	double sum = 0.0;
	for (const double ms : frames)
		sum += ms;
	const QString json_path = QFileInfo(args.shot_path).path() + QStringLiteral("/") +
				  QFileInfo(args.shot_path).completeBaseName() + QStringLiteral(".json");
	QFile json(json_path);
	if (!json.open(QIODevice::ReadWrite))
		return 1;
	QJsonObject root = QJsonDocument::fromJson(json.readAll()).object();
	QJsonObject bench;
	bench[QStringLiteral("frames")] = static_cast<int>(frames.size());
	bench[QStringLiteral("mean_ms")] = sum / static_cast<double>(frames.size());
	bench[QStringLiteral("max_ms")] = *std::max_element(frames.begin(), frames.end());
	// 画面では、重ねる仕事は画面のスレッドの外で行う。画面のスレッドの時間は、全体から重ねる時間を引いたもの。
	bench[QStringLiteral("full_compose_mean_ms")] = full_sum / static_cast<double>(frames.size());
	bench[QStringLiteral("crop_compose_mean_ms")] = crop_sum / static_cast<double>(frames.size());
	bench[QStringLiteral("gui_mean_ms")] = (sum - full_sum - crop_sum) / static_cast<double>(frames.size());
	root[QStringLiteral("bench")] = bench;
	json.resize(0);
	json.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	return g_clipped ? 1 : 0;
}

int run_sweep(label_tuner::TunerWindow &window, const label_tuner::ToolState &state, std::uint64_t *virtual_now,
	      const Args &args)
{
	window.resize(state.window_w, state.window_h);
	window.show();
	settle();
	const int result = label_tuner::run_knob_sweep(
		window, virtual_now, args.sweep_dir, [&window]() { refresh(window); },
		[&window](const QString &path, const QJsonObject &extra) {
			return write_shot(window, path, extra, QString());
		});
	return result == 0 && g_clipped ? 1 : result;
}

} // namespace

int main(int argc, char **argv)
{
	// --shot はウィンドウを開かない。プラットフォームプラグインは QApplication を作る前に決める必要があるので、
	// この引数だけを先に読む。
	bool shot = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--shot") == 0 || std::strcmp(argv[i], "--sweep") == 0 ||
		    std::strcmp(argv[i], "--drive") == 0)
			shot = true;
	if (shot)
		qputenv("QT_QPA_PLATFORM", QByteArray(cordmux::decor_preview::kDecorPlatformName));

	QApplication app(argc, argv);
	if (shot &&
	    QGuiApplication::platformName() != QString::fromLatin1(cordmux::decor_preview::kDecorPlatformName)) {
		std::fprintf(stderr, "label_tuner: --shot、--sweep、--drive をプラットフォーム %s では実行しません\n",
			     QGuiApplication::platformName().toUtf8().constData());
		return 3;
	}
	// 見た目は、画面と --shot の画像で同じになるように、Qt に入っている Fusion と、その標準の色にそろえる (Windows の
	// スタイルは OS の設定とプラグインで変わる)。
	QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
	QApplication::setPalette(QApplication::style()->standardPalette());

	Args args;
	QString error;
	if (!parse_args(app.arguments(), &args, &error)) {
		std::fprintf(stderr, "label_tuner: %s\n", error.toUtf8().constData());
		print_usage();
		return 2;
	}
	if (args.help) {
		print_usage();
		return 0;
	}

	label_tuner::ToolState state = label_tuner::default_tool_state();
	if (!args.state_text.isEmpty() && !label_tuner::parse_state(args.state_text, &state, &error)) {
		std::fprintf(stderr, "label_tuner: %s\n", error.toUtf8().constData());
		return 2;
	}

	// 画面で開いたときは、画面共有していない参加者の並びと暗さが初めから見えるように、--state に roster がなければ 3 人に
	// する。--shot、--sweep、--drive の既定の場面は変えない (0 人)。
	if (!shot && !args.state_text.contains(QStringLiteral("roster=")))
		state.roster = 3;

	config::Settings settings;
	label_tuner::TunerWindow::Options options;
	options.save_path = args.save_path;
	// --drive は、画面と同じく描き直しのスレッドとタイマーと実際の時計で動かす。
	const bool drive = !args.drive_dir.isEmpty();
	options.shot = shot && !drive;
	const QString load_path = !args.load_path.isEmpty()                                          ? args.load_path
				  : (!args.save_path.isEmpty() && QFileInfo::exists(args.save_path)) ? args.save_path
												     : QString();
	if (!load_path.isEmpty()) {
		QStringList snapped;
		if (!load_settings(load_path, &settings, &error, &snapped)) {
			if (shot) {
				std::fprintf(stderr, "label_tuner: %s\n", error.toUtf8().constData());
				return 2;
			}
			options.load_note = error + QStringLiteral(" つまみは製品の既定値にしました。");
		} else {
			options.load_note = QStringLiteral("読み込みました: %1").arg(load_path);
		}
		if (!snapped.isEmpty()) {
			const QString text =
				QStringLiteral("刻みに合わせた値: %1").arg(snapped.join(QStringLiteral(", ")));
			options.load_note += QStringLiteral("  ") + text;
			std::fprintf(stderr, "label_tuner: %s\n", text.toUtf8().constData());
		}
	}

	std::uint64_t virtual_now = kShotBaseNs;
	const auto clock = options.shot ? std::function<std::uint64_t()>([&virtual_now]() { return virtual_now; })
					: std::function<std::uint64_t()>([]() {
						  return static_cast<std::uint64_t>(
							  std::chrono::duration_cast<std::chrono::nanoseconds>(
								  std::chrono::steady_clock::now().time_since_epoch())
								  .count());
					  });
	label_tuner::TunerWindow window(options, settings, state, clock);
	if (drive) {
		window.resize(state.window_w, state.window_h);
		window.show();
		settle();
		return label_tuner::run_drive(window, args.drive_dir, clock,
					      [&window](const QString &path, const QJsonObject &extra) {
						      return write_shot(window, path, extra, QString());
					      });
	}
	if (!args.sweep_dir.isEmpty())
		return run_sweep(window, state, &virtual_now, args);
	if (shot)
		return run_shot(window, state, &virtual_now, args);

	const QRect available = window.screen() ? window.screen()->availableGeometry() : QRect(0, 0, 1600, 1000);
	window.resize(std::min(1600, available.width() - 40), std::min(1000, available.height() - 60));
	window.show();
	schedule_speaking(window, state, clock);
	return app.exec();
}
