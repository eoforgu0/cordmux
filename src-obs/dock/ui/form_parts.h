// SPDX-License-Identifier: Apache-2.0

// 設定ウィンドウと、ドックの表示タブが共通で使う、入力欄を並べる関数 (make_form、add_form_row、add_note_row、wrap_label、
// note_box、set_combo_data)。

#pragma once

#include <QString>
#include <QVariant>

class QComboBox;
class QFormLayout;
class QLabel;
class QWidget;

namespace cordmux::dock::ui {

class WrapLabel;

// 入力欄を並べるフォーム。幅が足りないときはラベルの下に入力欄を置き (WrapLongRows)、どの入力欄も列の幅まで伸ばす
// (AllNonFixedFieldsGrow。OBS の [設定] の 48 の QFormLayout のうち 47 と同じ)。ラベルは右寄せで縦の中央にする
// (OBS 31 の frontend/forms/OBSBasicSettings.ui の 48 のうち 44 の labelAlignment と同じ)。既定の左寄せと上寄せでは、
// ラベルのテキストが入力欄のテキストより 2.5 px 上に表示される。ラベルの下に入力欄を置いた行では、Qt がラベルの幅を
// そのテキストの幅にする (qformlayout.cpp の setupHorizontalLayoutData: sideBySide でなければ layoutWidth = sizeHint)
// ので、右寄せにしても左端に置かれる。
QFormLayout *make_form();

// 入力欄のラベルを付けて行を足す。buddy に入力欄を設定する (Qt がラベルのテキストを入力欄のアクセシブル名にする)。
// buddy が nullptr なら field が buddy になる。
// ラベルの高さを行の高さまで伸ばす (ラベルのテキストは QLabel の既定で縦の中央なので、入力欄のテキストと同じ高さに
// 並ぶ)。QFormLayout は labelAlignment の縦の指定を使わず、縦に伸びないラベルの高さを、行の高さ、ラベルの sizeHint の
// 7/4、最大の高さのうち小さいものにする (Qt 6.8 の qformlayout.cpp の arrangeWidgets)。そのため、テキストの高さが
// 18 px のラベルは 31 px で止まり、36 px のコンボボックスよりテキストが 2.5 px 上に表示される ([密度] が -2 のように
// 入力欄が低いときは差が出ない)。ラベルの最小の高さを入力欄の sizeHint にそろえる方法は、テーマや [密度] が変わる
// たびに合わせ直しが必要になるので使わない。
QLabel *add_form_row(QFormLayout *form, const QString &label_text, QWidget *field, QWidget *buddy = nullptr);

// ラベルのない行 (入力欄の下の補足。入力欄の列にそろえる)。ラベルの位置に大きさ 0 の QSpacerItem を置く。
// ラベルのない行 (addRow(QString(), ラベル)) では、QFormLayout は行の高さを、折り返すラベルの heightForWidth と、ラベルの
// sizeHint の高さの大きい方にする (Qt 6.8.3 の qformlayout.cpp の setupHfwLayoutData。ラベルの位置に項目がある行は、
// 行の高さをラベルの高さに置き直してから入力欄の heightForWidth と比べるが、項目がない行は置き直さず、入力欄の sizeHint
// の高さとの大きい方が残る)。QLabel の折り返しの sizeHint は、テキストの長さから決めた仮の幅で測った高さなので、
// ウィンドウで置かれた幅が仮の幅より広いと、テキストの行より高いラベルになり、補足と次の入力欄の間が空く。ラベルの位置に
// ウィジェットではない項目を置けば、行の高さが 0 に置き直され、置かれた幅の heightForWidth になる。
// 空の QLabel を置くと、名前のない静的なテキストがアクセシビリティのツリーに 1 つ増える。SpanningRole にすると、補足が
// 入力欄の列から外れて左端から始まる。
void add_note_row(QFormLayout *form, QWidget *note);

// 折り返す文 (入力欄の下の補足など、常に表示している文)。最後の行が 2 字以下にならず、折り返す幅の 4 分の 1 より
// 短くならないように、折り返す幅を右の余白で狭める (dock/ui/wrap_label.h の WrapLabel。値は DockStyle の既定)。
// QLabel の setWordWrap だけでは、補足が「...保存しま」「す。」のように 2 字の行で終わる。テキストを変えるときは
// WrapLabel::set_text を使う (狭める幅を測り直す)。
WrapLabel *wrap_label(const QString &text);

// 説明の文 (項目に添える文) を入れる入れ物。説明の文の左の字下げと下の余白は、入れ物のレイアウトの余白に持たせる
// (set_note_box_margins)。WrapLabel は自分の内側の余白 (contentsMargins) を、折り返す幅を狭めるために自分で書き直すので、
// ラベルに余白を足しても、大きさが変わるたびに消える。入れ物は note を子にする。
QWidget *note_box(QWidget *note);
// note_box の入れ物の左の余白 (説明する項目の文字の頭にそろえる字下げ) と下の余白 (px)。
void set_note_box_margins(QWidget *box, int left_px, int bottom_px);

// データが value の項目を選ぶ。なければ何もしない。
void set_combo_data(QComboBox *combo, const QVariant &value);

} // namespace cordmux::dock::ui
