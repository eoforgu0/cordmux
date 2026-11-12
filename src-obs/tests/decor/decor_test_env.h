// SPDX-License-Identifier: Apache-2.0

// decor_tests の中で共有する値 (テスト用のフォントの名前、失敗したときの画像を置くフォルダー、期待する画像を
// 作り直す実行かどうか)。値は main.cpp が決める。

#pragma once

#include <QString>

namespace cordmux::tests {

// 同梱のフォントを読み込んだ結果のフォントの名前。読み込めなかったときは空文字列になるが、そのときは main が
// テストを実行する前に失敗で終わるので、テストの中では空にならない。
QString decor_test_font_family();

// テストが失敗したときに 3 つの画像 (期待、実際、差分) を置くフォルダー (実行ファイルのフォルダーの下)。
QString decor_failure_dir();

// 期待する画像を作り直す実行かどうか。main がオプション --cordmux-snapshot-update を見て決める。環境変数に
// しないのは、消し忘れると、比べるはずの実行が何も伝えずに画像を作り直す実行に変わるためである。
// 作り直した実行は成功で終わらない (main が 0 以外を返す)。
bool decor_snapshot_update();

} // namespace cordmux::tests
