// SPDX-License-Identifier: Apache-2.0

// OBS のテーマのクラス (動的なプロパティ class) を付け替える関数 (set_class、set_style_property) と、ウィジェット
// 自身のスタイルシートを部分ごとに書いて 1 つにつなぐ関数 (set_own_style)。

#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStyle>
#include <QVariant>
#include <QWidget>

#include <algorithm>

namespace cordmux::dock::ui {

// ウィジェットに OBS のテーマのクラスを設定する (空の文字列なら外す)。値が同じなら何もしない (不要な polish で
// 描き直さないため)。セレクターに使う動的なプロパティを後から変えたときは、ウィジェットを unpolish と polish し
// 修正しないと、スタイルシートが計算し直されない (Qt の Style Sheets のドキュメントの注意。OBS 自身も同じ処理を
// している)。クラスを後から変える所は、すべてこの関数を使う。
inline void set_class(QWidget *w, const QString &classes)
{
	if (w == nullptr)
		return;
	const QVariant now = w->property("class");
	if (now.isValid() && now.toString() == classes)
		return;
	if (!now.isValid() && classes.isEmpty())
		return;
	w->style()->unpolish(w);
	w->setProperty("class", classes.isEmpty() ? QVariant() : QVariant(classes));
	w->style()->polish(w);
	w->update();
}

// 任意の動的なプロパティを変えて polish し直す (セレクターに使うプロパティだけに使う)。
inline void set_style_property(QWidget *w, const char *name, const QVariant &value)
{
	if (w == nullptr)
		return;
	if (w->property(name) == value)
		return;
	w->style()->unpolish(w);
	w->setProperty(name, value);
	w->style()->polish(w);
	w->update();
}

namespace detail {

// 部分を保存する動的なプロパティの名前の接頭辞。
inline constexpr const char *kOwnStylePrefix = "_cordmux_style_";

} // namespace detail

// ウィジェット自身のスタイルシートの 1 つの部分を書く (css が空なら、その部分を消す)。part は "font"、"color" など。
// css は宣言の並び ("font-size: 15.00pt; font-weight: bold;") で、セレクターは付けない (ウィジェットそのものに
// 適用する)。
// cordmux がウィジェット自身のスタイルシートに書く値 (type_scale のフォントの大きさと、surface_frame のメッセージの
// レベル 1 のパネルの上のテキストの色) は、別々の関数から同じウィジェットに書かれる。setStyleSheet は前の値を
// 丸ごと置き換えるので、部分ごとに名前を付けて動的なプロパティに保存し、すべての部分をつないで 1 つの
// スタイルシートにする (片方の関数が、もう片方の値を消さないようにするため)。
// 書く値は、計算した後の数と色だけにする。OBS のテーマの `var(` と `calc(` は OBS がテーマを読み込むときに展開する
// もので、ウィジェットのスタイルシートでは展開されない。QSS の `em` は行の高さの倍数で、cordmux の 1 em とは違う。
inline void set_own_style(QWidget *w, const char *part, const QString &css)
{
	if (w == nullptr)
		return;
	const QByteArray name = QByteArray(detail::kOwnStylePrefix) + part;
	const QVariant now = w->property(name.constData());
	if (now.isValid() ? now.toString() == css : css.isEmpty())
		return;
	w->setProperty(name.constData(), css.isEmpty() ? QVariant() : QVariant(css));
	// 部分の名前の順に並べてつなぐ (書いた順によらず、同じ部分の組み合わせなら同じスタイルシートになる)。
	QList<QByteArray> names;
	for (const QByteArray &n : w->dynamicPropertyNames()) {
		if (n.startsWith(detail::kOwnStylePrefix))
			names.push_back(n);
	}
	std::sort(names.begin(), names.end());
	QString sheet;
	for (const QByteArray &n : names) {
		const QString value = w->property(n.constData()).toString();
		if (value.isEmpty())
			continue;
		if (!sheet.isEmpty())
			sheet += QLatin1Char(' ');
		sheet += value;
	}
	w->setStyleSheet(sheet);
}

} // namespace cordmux::dock::ui
