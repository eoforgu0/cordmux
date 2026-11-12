// SPDX-License-Identifier: Apache-2.0

// 名前ラベルの描画で共有する物 (sources/decor_paint.h と sources/decor_paint_common.h) のテスト。ダミー表示の背景の
// 画像と、既定のフォントの候補を確認する。フォントと dpi は固定してある。

#include "doctest.h"

#include <QColor>
#include <QFont>
#include <QImage>
#include <QString>
#include <QStringList>

#include <string>

#include "sources/decor_paint.h"
#include "sources/decor_paint_common.h"
#include "sources/dummy_tiles.h"

TEST_CASE("ダミー表示の背景の画像は、枠の色の上に番号を大きく描画する")
{
	const QImage background = cordmux::sources::paint_dummy_background(2, 320, 180);
	REQUIRE(background.width() == 320);
	REQUIRE(background.height() == 180);
	CHECK(background.format() == QImage::Format_ARGB32_Premultiplied);
	// 角は背景の色 (画面共有の枠 2)。
	CHECK(background.pixel(2, 2) == QColor::fromRgba(cordmux::sources::dummy_tile_color(2)).rgba());
	// 真ん中のあたりに、番号の文字の白っぽいピクセルがある。
	int bright = 0;
	for (int y = 40; y < 140; ++y) {
		for (int x = 110; x < 210; ++x) {
			if (qRed(background.pixel(x, y)) > 200 && qGreen(background.pixel(x, y)) > 200)
				++bright;
		}
	}
	CHECK(bright > 100);
	CHECK(cordmux::sources::paint_dummy_background(1, 0, 180).isNull());
}

TEST_CASE("既定のフォントの候補は UD デジタル 教科書体 NK-B、その英語の名前、Yu Gothic UI の順で、実際に描くフォントは候補のどれか")
{
	namespace sources = cordmux::sources;
	const QStringList wanted = {QString::fromUtf8("UD デジタル 教科書体 NK-B"), QStringLiteral("UD Digi Kyokasho NK-B"),
				    QStringLiteral("Yu Gothic UI")};
	QStringList candidates;
	for (const char *name : sources::kDefaultLabelFontFamilies)
		candidates.append(QString::fromUtf8(name));
	CHECK(candidates == wanted);

	CHECK(candidates.contains(sources::default_label_font_family()));

	// フォントの名前を指定しないとき、描くフォントの候補は並びのとおりになる。名前を指定したときは、その名前だけ。
	const QFont by_default = sources::paint_common::make_label_font(std::string(), 700, false, 24.0);
	CHECK(by_default.families() == wanted);
	const QFont by_name = sources::paint_common::make_label_font("Meiryo UI", 700, false, 24.0);
	CHECK(by_name.family() == QStringLiteral("Meiryo UI"));
}
