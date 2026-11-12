// SPDX-License-Identifier: Apache-2.0

// 行のメニューの「名前を変更...」で開く「名前を変更」のダイアログ。
// 入力欄 (cordmux での名前) 1 つと、その補足の 1 文、[OK] と [キャンセル] を持つ。
// 構成は確認のダイアログ (dock/ui/message_dialog.h) にそろえる。QMessageBox は表示するときに Windows の音を鳴らすことが
// あり、QInputDialog は入力欄の下に補足の 1 文を置けないので、どちらも使わない。

#pragma once

#include <optional>
#include <string>

#include <QString>

class QDialog;
class QWidget;

namespace cordmux::dock {

// ダイアログを作り、開かずに返す。プレビューツールとドックを操作するテストが、開かずに中身を調べるために使う。
// 親は owner のウィンドウ。入力を止める範囲は Qt の既定のままで、開くときの範囲は run_rename_dialog が決める。
// discord_name は、入力欄が空のときにプレースホルダーとして表示する Discord での名前 (読めなければ空)。
// current_override は現在の cordmux での名前 (なければ空)。
QDialog *make_rename_dialog(QWidget *owner, const QString &discord_name, const std::string &current_override);

// 入力欄のテキストから前後の空白を除いたもの。dialog が nullptr なら空。
QString rename_dialog_text(const QDialog *dialog);

// ダイアログを開き、閉じるまで待つ。[OK] なら入力したテキスト (空なら cordmux での名前を消す)、
// [キャンセル] と Esc なら値を持たない。開いている間に入力を止めるのは owner のドックだけ。
// 開いている間に OBS の終了でドックが削除されると、値を持たずに返る。呼び出し元は、返った後に自分がまだあるかを
// 確認してから自分に触る。
std::optional<std::string> run_rename_dialog(QWidget *owner, const QString &discord_name,
					     const std::string &current_override);

} // namespace cordmux::dock
