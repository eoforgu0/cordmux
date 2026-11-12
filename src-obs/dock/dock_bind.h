// SPDX-License-Identifier: Apache-2.0

// ドックを OBS に追加し、初めての起動のときだけ、ドックをフローティングで表示する。

#pragma once

#include <string>

class QWidget;

namespace cordmux::dock {

// cordmux の設定フォルダー (obs_module_config_path) にある name のファイルのパスを返す。設定フォルダーがなければ作る。
// ファイルは作らない。パスを取得できなければ空文字列を返す。
std::string module_config_file_path(const char *name);

// cordmux の設定フォルダー (obs_module_config_path) のパスを返す。末尾に区切り文字は付けない。取得できなければ
// 空文字列を返す。アンインストールで消すフォルダーを渡すために使うので、module_config_file_path と違ってフォルダーを
// 作らない。
std::string module_config_dir();

// ドックを OBS に追加する。追加できたら true を返す。追加した後は、widget の寿命を OBS が持つ。
// obs_frontend_add_dock_by_id は widget を OBSDock (QDockWidget の派生クラス) の中身にし、OBS のメインウィンドウの
// 子にする。widget はメインウィンドウと一緒に、obs_module_unload より前に破棄される。そのため呼び出し元は widget を
// QPointer で持ち、obs_module_unload の中では触らない。
// https://github.com/obsproject/obs-studio/blob/31.1.1/frontend/OBSStudioAPI.cpp
bool add_dock(QWidget *widget);

// OBS_FRONTEND_EVENT_FINISHED_LOADING から呼ぶ。OBS は初めての起動で、追加したドックを隠したままにする。
// そのため初めての起動のときだけ、ドックを setVisible(true) と raise() でフローティングとして表示する。
// 初めての起動かどうかは、設定フォルダーのマーカーのファイルで判断する。OBS が保存するドックの表示状態からは、
// 初めての起動と、利用者がドックを閉じた後の起動を区別できないためである。2 回目からの起動では何もしないので、
// 利用者が閉じたドックは閉じたままになる。
//
// 表示は QTimer::singleShot(0) でイベントループに 1 度戻ってから行い、OBS がレイアウトを復元する処理と
// 競合しないようにする。FINISHED_LOADING のコールバックの中で直接表示して動くかは確認していない。
void show_dock_once_after_loading(QWidget *widget);

} // namespace cordmux::dock
