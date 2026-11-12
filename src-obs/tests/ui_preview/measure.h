// SPDX-License-Identifier: Apache-2.0

// 描画した製品のウィジェットを調べ、結果を表 (tsv) の行にする。調べるのは、ドックの 1 行目とボタンが見えているか、
// 名前の省略、はみ出した文字、押せるものの大きさと間隔 (WCAG 2.5.8)、テキストと背景のコントラスト、文字の上下の
// 見切れ。押せる範囲は製品の focus_hit_rect_of (dock/ui/focus_ring.h) で求める。ウィジェットが FocusHitRect を継承
// していればその矩形、QPushButton は SE_PushButtonBevel、QCheckBox は SE_CheckBoxClickRect で、製品が hitButton に
// 使う矩形とフォーカスリングが囲む矩形も、この関数で求める (dock/person_row.h のコメント)。コントラスト比は製品の
// dock/ui/theme_read.h の contrast_ratio を使わず、ここで WCAG 2.x の式を別に書く。製品の式に誤りがあると、製品の式で
// 計算しても見つからないためである。

#pragma once

#include <QColor>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QStringList>

#include <vector>

class QMenu;
class QWidget;

namespace cordmux::ui_preview {

struct DockHost;

// 押せるもの 1 つ (DockFacts の m7)。
struct TargetFact {
	QString kind;  // 種類 (行のチェックボックス row-check、行の [...] row-menu、録画のボタン rec、ドックの [...]
	               // dock-menu、メッセージのレベル 1 のボタン notice-button、カードのボタン card-button、1 行目の
	               // ボタン first-line-button、空の一覧のボタン empty-button、メニューの項目 menu-item、
	               // そのほか other:<型>)
	QString label; // 人の名前か、ボタンのテキスト
	QRect rect;    // 押せる矩形 (画像の座標)
	QRect qt_rect; // Qt の既定の押せる矩形 (QCheckBox は SE_CheckBoxClickRect。ほかは rect と同じ)
	bool visible = true; // スクロール領域の外に隠れていない (矩形のすべてが見えている)
	bool enabled = true;
};

// WCAG 2.5.8 (Target Size (Minimum)) の判定。
struct Wcag258 {
	bool size_ok = false;    // 24 x 24 の正方形が入る
	bool spacing_ok = false; // 入らないときは、直径 24 px の円が、ほかの押せるものにも、ほかの小さいものの円にも
	                         // 交わらない
	QString conflict;        // 交わった相手
};
std::vector<Wcag258> judge_258(const std::vector<TargetFact> &targets);

// 名前 1 つ (DockFacts の m4。行どうしを見分けられるかも、この値で数える)。
struct NameFact {
	QString user_id;
	QString full;
	QString shown;
	QStringList line2_full;
	QStringList line2_shown;
	int full_graphemes = 0;
	int shown_graphemes = 0; // 省略記号を除いた文字の数
	bool elided = false;
	bool middle = false; // 真ん中を省略した (見えているテキストの真ん中に省略記号がある)
	QString a11y_name;
	QString a11y_desc;
};

// 見えているテキスト 1 つの色。
struct ContrastFact {
	QString object_name;
	QString cls;
	QString text;
	QColor fg;
	QColor bg; // 画像の上で、そのウィジェットの見えている矩形にいちばん多い色
	double ratio = 0;
	bool enabled = true;
	// 画像に描画された文字の色。矩形の中で、背景との比がいちばん大きいピクセル (文字の線の中心)。パレットの色と違う
	// 描画 (押せない名前を、不透明度を下げて描画するなど) でも、目に入る色で数えるため。
	QColor drawn_fg;
	double drawn_ratio = 0;
};

// ドックを数えた結果。metrics.tsv の m1 から m8 の列は、この値から書く。m1 は 1 行目がスクロールの位置 (先頭・
// 中ほど・最後) のどれでもすべて見えているか、m2 はメッセージのレベル 1 のボタンについて同じこと、m3 はスクロールせずに
// (先頭で) すべて見える人の数、m4 は名前の見えている文字数と真ん中の省略 (names.tsv)、m5 はウィジェットの外へ
// はみ出した文字 (省略の仕方を決めていない QLabel とボタン)、m6 は横のスクロールが必要か、m7 は押せるものの大きさと
// 間隔 (targets.tsv)、m8 は 1 行目の高さ。
struct DockFacts {
	int first_line_height = 0;
	bool first_line_visible[3] = {false, false, false}; // スクロールの位置 (先頭・中ほど・最後) ごと
	int l1_buttons = 0;
	bool l1_buttons_visible[3] = {false, false, false};
	int people = 0;
	int visible_people = 0; // 先頭ですべて見える人の数
	std::vector<NameFact> names;
	QStringList clipped;
	bool h_overflow = false;
	bool v_scrollbar = false;
	std::vector<TargetFact> targets;
	int min_width = 0;
	int rec_button_width = 0;
	QColor rec_button_bg; // 録画のボタンの背景 (押された見た目かどうかを、状態の間で比べる)
	QString rec_button_text;
	QString first_line_text;
	QString first_line_full;
	bool first_line_elided = false;
	bool first_line_first_segment = true; // 最初の区切りまでが残ったか (区切りは " - ")
	bool first_line_has_segments = false; // 全文に区切りがあるか (なければ上の値は意味を持たない)
	QString first_line_more;              // 残りの件数の QLabel のテキスト (「(他1件)」。なければ空)
	// 1 行目の View (render_sets.py の first_line_checks が読む)。kind は notice・summary・user-choice・
	// working、tier は 1・2・3 (メッセージのときだけ。ほかは -)、notice_key は見出しを表示しているメッセージのキー
	// (カードの noticeKey と同じ)、forms は長い順のすべての候補 (伝えなら見出しの全文と短い形)、
	// shown_form は QLabel が表示している候補の番号 (ElideLabel)。
	QString first_line_kind = QStringLiteral("-");
	QString first_line_tier = QStringLiteral("-");
	QString first_line_notice_key;
	QStringList first_line_forms;
	int first_line_shown_form = -1;
	QString l1_body_place = QStringLiteral("-");
	int notices = 0;
};

// ドックを現在の大きさで数える (スクロールの位置を動かして戻す)。image は現在の画像 (コントラストと録画のボタンの背景に
// 使う)。
DockFacts measure_dock(DockHost &host, const QImage &image);

// root の中の押せるもの (見えているものだけ)。origin は、画像の上での root の左上。
std::vector<TargetFact> collect_targets(QWidget *root, const QPoint &origin);
// 開いたメニューの項目 (QMenu::actionGeometry)。
std::vector<TargetFact> menu_targets(QMenu *menu, const QPoint &origin);

// root の中の見えているテキストの色と背景 (image は root を origin に置いた画像)。
std::vector<ContrastFact> measure_contrast(QWidget *root, const QImage &image, const QPoint &origin);

// 文字の上下の見切れ。見えている QLabel の上端と下端の 1 ピクセルの行に、文字のピクセル (QLabel の背景との比が 1.5 を
// 超えるピクセル) が乗っている数。乗っていれば、QLabel の高さが文字に足りず、上か下が切れているおそれがある。
struct EdgeFact {
	QString object_name;
	QString text;
	int top = 0;
	int bottom = 0;
	// 端に乗っているだけか、切れているおそれがあるか。QLabel の中身の高さが、Qt が測ったフォントの高さ
	// (QFontMetrics::height。日本語の文字の代替フォントは含まない) と、文字が描画される高さ (tightBoundingRect) の
	// どちらかより低ければ、切れているおそれがある。
	int label_h = 0;
	int font_h = 0;
	int ink_h = 0;
	bool suspect = false;
};
std::vector<EdgeFact> glyph_edges(QWidget *root, const QImage &image, const QPoint &origin);
extern const char *const kEdgesHeader;
QString edges_rows(const QString &key, const std::vector<EdgeFact> &facts);

// WCAG 2.x のコントラスト比 (製品の式とは別に書いた式。ファイルの先頭のコメント)。
double wcag_contrast(const QColor &a, const QColor &b);

// 画像の中の矩形で、いちばん多い色。
QColor dominant_color(const QImage &image, const QRect &rect);

// 文字の数 (書記素で数える。結合した絵文字と結合文字は 1 文字)。
int grapheme_count(const QString &text);

// ウィジェットの見えている矩形 (root の座標。祖先の矩形で切り取ったもの)。
QRect visible_rect_in(QWidget *w, QWidget *root);

// tsv の 1 つのセル (タブと改行を、見える文字に置き換える)。
QString tsv_cell(QString text);

// 表の見出しと行。key は画像の名前 (場面と大きさ)。
extern const char *const kTargetsHeader;
QString targets_rows(const QString &key, const std::vector<TargetFact> &targets, const std::vector<Wcag258> &judged);
extern const char *const kNamesHeader;
QString names_rows(const QString &key, const std::vector<NameFact> &names);
extern const char *const kContrastHeader;
QString contrast_rows(const QString &key, const std::vector<ContrastFact> &facts);

} // namespace cordmux::ui_preview
