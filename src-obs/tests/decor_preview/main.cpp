// SPDX-License-Identifier: Apache-2.0

// 名前ラベルのプレビューツール (decor_preview.exe) のエントリポイント。合成のソース (cordmux) の
// 名前ラベルと光を、製品と同じ関数で描いた画像 (PNG) と、OBS の中の描画と比べるための参照を書き出す。
// 使い方は tests/decor_preview/README.md。
// ウィンドウ (QWindow) は 1 つも作らない。Qt のプラットフォームプラグインは cordmuxdecorpreview (decor_qpa.h)、minimal、
// windows のどれかにし、環境変数で別のプラグインが指定されていても上書きする。どれでもなければ何も描画せずに終了する。

#include <QByteArray>
#include <QFont>
#include <QGuiApplication>
#include <QString>
#include <QStringList>
#include <QtPlugin>

#include <cstdio>
#include <cstring>
#include <string>

#include "composite_check.h"
#include "composite_sample.h"
#include "decor_qpa.h"

Q_IMPORT_PLUGIN(CordmuxDecorPreviewIntegrationPlugin)

using namespace cordmux::decor_preview;

namespace {

void print_usage()
{
	std::printf(
		"usage: decor_preview <mode> [options]\n"
		"  --backend cordmuxdecorpreview|minimal|windows  --system-font NAME\n"
		"  composite source sample: --composite-sample --out DIR [--name-pct P] [--glow none|frame,avatar,name]\n"
		"    [--speaker 0-3|none] [--corner tl|tr|bl|br] [--plate] [--no-avatar] [--no-slot-labels]\n"
		"    [--margin-pct M] [--tag T]\n"
		"    [--people ID,ID,ID,ID] [--tile-images TL.png,TR.png,BL.png,BR.png] ...\n"
		"  composite source knob sweep: --composite-sweep --out DIR [--knob NAME]\n"
		"  reference for the OBS capture check: --composite-check REQUEST.json --out DIR\n"
		"    --composite-check-background dark-game|bright-sky|stripes|snowy-field WxH PNG\n"
		"    --composite-check-avatar HUE PNG\n");
}

} // namespace

int main(int argc, char **argv)
{
	// プラットフォームプラグインと既定のフォントは QGuiApplication を作る前に決める必要があるので、この 2 つだけを
	// 先に読む。
	QByteArray platform = QByteArray(kDecorPlatformName);
	std::string system_font = "Yu Gothic UI";
	for (int i = 1; i + 1 < argc; ++i) {
		if (std::strcmp(argv[i], "--backend") == 0)
			platform = QByteArray(argv[i + 1]);
		if (std::strcmp(argv[i], "--system-font") == 0)
			system_font = argv[i + 1];
	}
	set_system_font_family(system_font.c_str());
	const bool own_platform = platform == QByteArray(kDecorPlatformName);
	const bool minimal_platform = platform == QByteArray("minimal");
	// windows は、OBS の中と同じ Qt の windows プラグインで描き、cordmuxdecorpreview の画像と比べるためだけに使う。
	// QWindow を作らないので、このプラグインでも画面には何も出ない。OBS と同じく、高 DPI の拡大と dpi は変えない
	// (名前ラベルは 96 dpi に固定した面に描くので、文字の大きさには反映されない)。
	const bool windows_platform = platform == QByteArray("windows");
	if (!own_platform && !minimal_platform && !windows_platform) {
		std::fprintf(stderr, "decor_preview: --backend must be %s, minimal, or windows\n", kDecorPlatformName);
		return 2;
	}
	if (windows_platform) {
		qputenv("QT_QPA_PLATFORM", platform);
	} else {
		qputenv("QT_QPA_PLATFORM", own_platform ? platform : QByteArray("minimal:enable_fonts"));
		qputenv("QT_ENABLE_HIGHDPI_SCALING", QByteArray("0"));
		QGuiApplication::setAttribute(Qt::AA_Use96Dpi);
	}

	QGuiApplication app(argc, argv);
	const QByteArray expected_platform = minimal_platform ? QByteArray("minimal") : platform;
	if (QGuiApplication::platformName() != QString::fromLatin1(expected_platform)) {
		std::fprintf(stderr, "decor_preview: refusing to run on platform %s\n",
			     QGuiApplication::platformName().toUtf8().constData());
		return 3;
	}
	if (minimal_platform) {
		// minimal には既定のフォントを返すテーマがない。違いをフォントデータベースだけに絞るため、同じ
		// フォントを設定する。
		QGuiApplication::setFont(QFont(QString::fromStdString(system_font), 9));
	}

	const QStringList global_args = app.arguments();
	// 動作は引数で選ぶ。見本、つまみごとの見本の一覧、OBS の中の描画と比べる参照は、それぞれ引数が別なので、
	// ここで分ける。
	if (global_args.contains(QStringLiteral("--help")) || global_args.contains(QStringLiteral("-h"))) {
		print_usage();
		return 0;
	}
	if (global_args.contains(QStringLiteral("--composite-check")))
		return run_composite_check(global_args);
	if (global_args.contains(QStringLiteral("--composite-check-background")))
		return run_composite_check_background(global_args);
	if (global_args.contains(QStringLiteral("--composite-check-avatar")))
		return run_composite_check_avatar(global_args);
	if (global_args.contains(QStringLiteral("--composite-sample")))
		return run_composite_sample(global_args);
	if (global_args.contains(QStringLiteral("--composite-sweep")))
		return run_composite_sweep(global_args);
	print_usage();
	return 2;
}
