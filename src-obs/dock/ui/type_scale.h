// SPDX-License-Identifier: Apache-2.0

// フォントの大きさの倍率と太さ (apply_type、apply_scale)。フォントの大きさとフォント名はコードに書かない。倍率を掛けるテキスト
// (ドックの 1 行目、ダイアログの主文、表示タブの大見出し) だけ、元の大きさ (OBS の本文。base_point_size) に倍率を掛けた pt を
// そのウィジェット自身のスタイルシートに書く (QSS のフォントの大きさは pt と px だけで `em` は使えず、setFont は OBS のテーマの
// `QWidget { font-size }` に負ける)。テキストの見た目は、1.5 em (ダイアログの主文)、1.1 em の太字 (表示タブの大見出し)、1 em の
// 太字 (見出しと 1 行目のメッセージ)、1 em (本文と 1 行目の平常の状態)、1 em の弱いテキスト (補足と説明の文) の 5 種類までにする。
// 斜体 (日本語では文字を傾けるだけで読みにくい) と下線 (押せるものと紛れる) は使わない。弱いテキストの色は theme_read の
// muted_or_text で決める。

#pragma once

#include <QString>

class QWidget;

namespace cordmux::dock::ui {

enum class TypeLevel {
	FirstLineHeading, // ドックの 1 行目のメッセージの見出し: 倍率を掛け、太字にする
	FirstLineSummary, // 1 行目の平常の要約と、利用者が選んだ状態 (無効、ダミー) の行: 倍率を掛け、普通の太さにする
	GroupHeading,     // 表示タブの大見出し: 倍率 (DockStyle::group_heading_scale) を掛け、太字にする
	Heading,          // カードの見出しと、表示タブの中見出しと、状態タブの見出し: 1 em の太字
	Body,             // 本文: 1 em (ウィジェット自身のスタイルシートを書かない)
};

// 倍率を掛けたフォントの大きさの宣言 ("font-size: 15.00pt;")。base_pt は base_point_size() で読んだ値 (0 以下なら
// 10 pt とみなす)。
QString font_size_css(double base_pt, double scale);

// target にテキストの見た目を適用する。scale は FirstLine* と GroupHeading のときだけ使う (DockStyle::first_line_scale と
// group_heading_scale。1 以下なら本文の大きさのまま)。Body は
// 大きさと太さの宣言を消し、テーマのフォントに戻す。宣言は set_own_style の "font" の部分に書く (ほかの関数が書く
// "color" の部分を消さないため)。target が nullptr なら何もしない。
void apply_type(QWidget *target, TypeLevel level, double base_pt, double scale);

// 倍率だけを適用する (ダイアログの主文など)。太さは変えない。scale が 1 以下なら宣言を消す。
void apply_scale(QWidget *target, double base_pt, double scale);

} // namespace cordmux::dock::ui
