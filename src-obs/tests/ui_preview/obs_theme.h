// SPDX-License-Identifier: Apache-2.0

// OBS のテーマ (.obt と .ovt) を、OBS 31.1.1 が作るのと同じ QSS とパレットにする。OBS 31.1.1 の
// frontend/OBSApp_Themes.cpp の ParseThemeMeta、FindThemes (継承の順)、ParseThemeVariables、ParseMath、
// ResolveVariable、ParseMathVariable、EvalMath、FindEndOfOBSMetadata、PrepareQSS、PreparePalette、SetTheme を移植した。
// 数値の書き方 (QString::number(v, 'f', 整数なら 0 桁、そうでなければ -1)) も同じ Qt の関数を同じ引数で呼ぶので、
// `1.6px` が `1.600000px` になるところまで OBS と同じになる。libobs の文字列句解析 (cf-parser) はそのまま移植せず、テーマの
// 変数のブロックが使う形 (`--名前: 値;`。値は数値・#色・rgb()・var()・calc()/max()/min()・文字列) で同じ結果になる
// 範囲の、小さな文字列句解析を書いた (obs_theme.cpp の Tokenizer)。

#pragma once

#include <QPalette>
#include <QString>
#include <QStringList>

#include <utility>
#include <vector>

namespace cordmux::ui_preview {

struct ComposedTheme {
	QString id;
	QString name;
	bool dark = false;
	QStringList files;   // 読んだ順 (基底のテーマから)
	QString stylesheet;  // PrepareQSS の結果 (OBS が setStyleSheet へ渡すもの)
	QPalette palette;    // PreparePalette の結果 (OBS が setPalette へ渡すもの)
	// 変数ごとの解決後の値 (名前の順)。確認のために書き出すだけで、描画には使わない。
	std::vector<std::pair<QString, QString>> resolved;
};

// OBS の [外観] の 2 つの値 (user.ini の [Appearance])。既定値は OBS 31.1.1 の OBSApp::InitUserConfigDefaults と
// 同じ (FontScale=10、Density=1)。
struct Appearance {
	int font_scale = 10;
	int density = 1;
};

// themes_dir の中から theme_id のテーマを探し、基底のテーマから順に重ねて、QSS とパレットを作る。default_palette は
// OBS の defaultPalette (SetTheme の前のアプリのパレット) に当たるもの。見つからないときと読めないときは false を
// 返し、error に理由を入れる。
bool compose_obs_theme(const QString &themes_dir, const QString &theme_id, const Appearance &appearance,
		       const QPalette &default_palette, ComposedTheme *out, QString *error);

// OBS の getPaddingForDensityId と同じく、Density の番号から obsPadding の値を返す。
double padding_for_density(int density);

} // namespace cordmux::ui_preview
