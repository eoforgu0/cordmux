// SPDX-License-Identifier: Apache-2.0

// フォルダーをエクスプローラーで開く ([フォルダーを開く])。cordmux の [フォルダーを開く] は、確認と結果の
// ダイアログ (message_dialog.cpp)、ドックのカード (dock_poll.cpp の open_location_of)、設定ウィンドウ
// (settings_window.cpp) のどれもが、この関数を使う。Qt だけを使うので、プレビューツールとテストからも
// 呼び出せる。UI スレッドから呼び出す。
// Windows の API を使わない (Qt の QDesktopServices で開く) ので、Windows の API を閉じ込める platform/ には
// 置かない。

#pragma once

#include <QString>

namespace cordmux::dock::ui {

// path を開くときに開くフォルダー。決まらなければ空を返す。OS の操作をしないので、テストで確認できる。
//   - 存在するフォルダーは、そのフォルダーを開く。存在するファイルは、そのファイルがあるフォルダーを開く
//     (ファイルは選択しない)。
//   - 存在しない (利用者が先に削除した、cordmux が録画の前にフォルダーを作らない) ときは、存在するいちばん近い
//     親を開く。ダイアログとカードはパスより長く残るので、押しても何も起きない形にしない。
//   - 空のパスと相対パス、ルートまでさかのぼっても存在するフォルダーがないときは空を返す。
// ファイルを選択した状態で開く [ファイルの場所を開く] と、フォルダーを親の中で選択した状態で開く [フォルダーの
// 場所を開く] (結果のダイアログの「削除するフォルダー:」の行だけ。message_dialog.h の
// MessageItem::folder_location) は、別の関数 (platform::open_file_location) を使う。
// 存在するいちばん近い親を決める処理は、ここ (dock_ui_calc.h の nearest_folder) と
// platform::resolve_file_location の 2 か所にある (開くものが違い、こちらはフォルダーそのものを開き、あちらは親を
// 開いてパスを選択する)。どちらも同じ規則を、test_dock_ui_calc.cpp と test_platform_file_location.cpp が確認する。
QString folder_to_open(const QString &path);

// folder_to_open が返したフォルダーをエクスプローラーで開く。OS に渡せたら true を返す。開くフォルダーが
// 決まらないときは false を返し、利用者には何も表示しない (ダイアログの上に別のメッセージを重ねないため)。
bool open_folder(const QString &path);

} // namespace cordmux::dock::ui
