// SPDX-License-Identifier: Apache-2.0

// decor_tests の main。doctest を実行する前に、ウィンドウを開かない Qt のプラットフォームプラグインで
// QGuiApplication を作り、同梱のフォントを読み込む (PC に入っているフォントで描画が変わらないように)。
// テストは test_decor_paint.cpp にある。

#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QString>
#include <QStringList>

#include <cstdio>
#include <cstring>
#include <vector>

#include "decor/decor_test_env.h"

namespace {

QString g_font_family;
bool g_snapshot_update = false;

// 期待する画像を作り直すときだけ渡すオプション。doctest は知らないオプションを無視するが、doctest の
// オプションと取り違えないように、名前に cordmux を付ける。
constexpr const char *kSnapshotUpdateFlag = "--cordmux-snapshot-update";

} // namespace

namespace cordmux::tests {

QString decor_test_font_family()
{
	return g_font_family;
}

QString decor_failure_dir()
{
	return QCoreApplication::applicationDirPath() + QStringLiteral("/snapshot_failures");
}

bool decor_snapshot_update()
{
	return g_snapshot_update;
}

} // namespace cordmux::tests

int main(int argc, char **argv)
{
	// ウィンドウを 1 つも開かず、フォントだけを使う。obs-deps 2025-07-11 の Qt 6.8.3 には、offscreen の
	// プラットフォームプラグインが同梱されていない (plugins/platforms にあるのは windows、direct2d、minimal の
	// 3 つ)。offscreen を指定すると、プラグインを初期化できないというダイアログが出て、誰かが閉じるまで止まる。
	// minimal に enable_fonts を付ければ addApplicationFont が成功し、描画したピクセルは windows で描画した
	// ものと 1 バイトも違わない (両方で描画した PNG を比べて確認した)。
	qputenv("QT_QPA_PLATFORM", QByteArray("minimal:enable_fonts"));
	// 画面の拡大率を使わない (PC ごとに描画したピクセルが変わらないように)。
	qputenv("QT_ENABLE_HIGHDPI_SCALING", QByteArray("0"));
	QGuiApplication::setAttribute(Qt::AA_Use96Dpi);

	QGuiApplication app(argc, argv);

	const QString font_path = QStringLiteral(CORDMUX_DECOR_FONT_DIR) + QStringLiteral("/OpenSans-Regular.ttf");
	int font_id = QFontDatabase::addApplicationFont(font_path);
	QStringList families = font_id >= 0 ? QFontDatabase::applicationFontFamilies(font_id) : QStringList();
	if (families.isEmpty()) {
		// ここで止めないと、フォントがないまま描画した画像を期待する画像として保存してしまい、その後の
		// テストが何も伝えずに成功する。
		std::fprintf(stderr, "decor_tests: cannot load the bundled typeface: %s\n",
			     font_path.toUtf8().constData());
		return 1;
	}
	g_font_family = families.first();

	// 作り直しの指定は、実行のたびにオプションで渡す (QGuiApplication が自分のオプションを取り除いた後の
	// 残りを見る)。doctest には、このオプションを除いた残りを渡す。
	std::vector<char *> forwarded;
	forwarded.reserve(static_cast<std::size_t>(argc));
	for (int i = 0; i < argc; ++i) {
		if (std::strcmp(argv[i], kSnapshotUpdateFlag) == 0) {
			g_snapshot_update = true;
			continue;
		}
		forwarded.push_back(argv[i]);
	}

	doctest::Context context;
	context.applyCommandLine(static_cast<int>(forwarded.size()), forwarded.data());
	int result = context.run();

	// 作り直した実行は成功で終わらせない。比べていないので、この実行の成功には意味がなく、作り直した後に
	// オプションなしで 1 回実行するのを忘れると、何も確認しないままテストが成功したことになる。
	if (g_snapshot_update) {
		std::fprintf(stderr, "decor_tests: snapshots were rewritten; run again without %s\n",
			     kSnapshotUpdateFlag);
		return result == 0 ? 2 : result;
	}
	return result;
}
