// SPDX-License-Identifier: Apache-2.0

// settings.json のキーごとに値を書き換える所を決めた表と、設定ウィンドウで変えた値をコピーする関数。
// 設定ウィンドウが持つ値の一覧はこの表だけにする。キーを追加したら、この表にも行を追加する。
// tests/test_dock_settings_fields.cpp は、表のキーの集合が config::settings_to_json() の書くキーの集合と
// ちょうど一致することを確認するので、行を追加し忘れるとテストが失敗する。
// Qt にも libobs にも依存しないので、align_tests に入る。

#pragma once

#include <cstddef>
#include <optional>

#include "config/settings.h"
#include "ledger/observation.h"

namespace cordmux::dock {

// 値を書き換える所。
//   SettingsWindow  設定ウィンドウの入力欄。[OK] と [適用] で反映する
//   DockTab         ドックの表示タブと状態タブの入力欄。変えるとすぐ反映し、保存は変更の少し後にまとめる。設定ウィンドウは
//                   この行をコピーしない (ウィンドウが開いた時点の古い値で、表示タブの値を上書きしないため)
//   Action          ActionId を通る操作 (オンとオフの切り替えと、メインアカウントの変更)。押せるかは ledger::apply が決める
//   FileOnly        画面に表示せず、settings.json を手で書いたときだけ変わる値。利用者が選べる余地を残すために
//                   設定として持つ値と、製品が書くが利用者には見せない値 (ウィンドウの大きさと位置) が入る
enum class SettingsOwner {
	SettingsWindow,
	DockTab,
	Action,
	FileOnly,
};

struct SettingsFieldRow {
	const char *json_key; // config::settings_to_json() のキーの綴り
	SettingsOwner owner;
	// from のフィールドを to の同じフィールドにコピーする。呼び出すのは SettingsWindow の行だけだが、ほかの行にも
	// 持たせておくのは、値を書き換える所を変えるときに owner だけを修正すれば済むようにするため。
	void (*copy)(const config::Settings &from, config::Settings &to);
	// 設定ウィンドウのどのページに入力欄を置くか。owner == SettingsWindow の行だけが持つ (ほかの行は std::nullopt。
	// テストがそれを確認する)。
	std::optional<ledger::SettingsPage> page;
	// 値を変えても、OBS を再起動するまで反映されないか。true の行の値が起動のときの値と違う間、そのページの下に
	// 再起動を求める文を表示する (restart_pending_on_page)。
	bool restart_required;
};

// 表の行数と、index 番目 (0 から数える) の行。index が範囲の外なら最後の行を返す。表は settings_fields.cpp にある。
std::size_t settings_field_count();
const SettingsFieldRow &settings_field_at(std::size_t index);

// 設定ウィンドウから返った設定 (from_page) のうち、SettingsWindow の行のフィールドだけを current にコピーする。
// ほかのフィールドは current のままにする。ウィンドウが持たない値、表示タブの値、ActionId を通る操作の値、ドックの
// フラグを、ウィンドウが最後に描画したときの古い値で上書きしないためである。
void apply_settings_page_fields(const config::Settings &from_page, config::Settings &current);

// 表示タブから返った設定 (from_tab) のうち、DockTab の行のフィールドだけを current にコピーする。ほかのフィールドは
// current のままにする。
void apply_look_tab_fields(const config::Settings &from_tab, config::Settings &current);

// DockTab の行のうち、a と b で値が違うものがあるか (settings.json に書く値で比べる)。
bool look_tab_fields_differ(const config::Settings &a, const config::Settings &b);

// ウィンドウで編集中の設定 (edited) が、この起動で使っている設定 (effective_at_start) と、そのページの
// 再起動が必要な入力欄のどれかで違うか。違う間だけ、ページの下に再起動を求める文を表示する。
// 比べるのは settings.json に書く値である。フィールドごとの比べ方を表に持たせると、行を追加するたびに 2 つの
// 関数を書くことになる。page が About のように入力欄を持たないページなら false。
bool restart_pending_on_page(const config::Settings &effective_at_start, const config::Settings &edited,
			     ledger::SettingsPage page);

} // namespace cordmux::dock
