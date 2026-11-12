// SPDX-License-Identifier: Apache-2.0

// DockPoll の処理のうち、設定 (settings.json) とこれまでに見た人の一覧 (seen_people.json) の読み書きと、
// 設定ウィンドウと参加者の設定ウィンドウを扱うものの状態 (DockPollSettingsState) と関数 (DockPollSettingsPart) を
// 宣言する。実装は dock_poll_settings.cpp にある。dock_poll.h を include しない (dock_poll.h がこのヘッダーを
// include する)。Qt (QPointer) を含む。

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <QPointer>
#include <QWidget>

#include "config/settings.h"
#include "ledger/dock_view.h"
#include "ledger/observation.h"

namespace cordmux::dock {

class DockPoll;

// 設定と参加者の設定に関わる処理の状態。DockPoll がメンバー settings_state_ として持つ。
struct DockPollSettingsState {
	// この起動で使っている設定。再起動が必要な項目について、設定ウィンドウの値と比べるのに使う。
	// DockPoll のコンストラクターが設定する。
	config::Settings effective_at_start;
	// 開いている設定ウィンドウ (なければ空)。テーマが変わったときに伝える先でもある。
	QPointer<QWidget> window;
	// 開いている参加者の設定ウィンドウ (なければ空)。
	QPointer<QWidget> participants_window;

	// 最後のポーリングで受け取った「バージョン情報」のページと、参加者の設定ウィンドウの行 (refresh_windows が
	// 入れる)。ウィンドウを開いたときに、次のポーリングを待たずに中身を表示するために持つ。
	std::optional<ledger::dockview::AboutPageView> last_about;
	std::vector<ledger::dockview::ParticipantRow> last_participants;

	DockPollSettingsState() = default;
	// 残っているウィンドウを破棄する。ウィンドウのコールバックは DockPoll を参照で持つので、DockPoll より
	// 長く残さない。普通は OBS のメインウィンドウと一緒に先に破棄されているので、何もしない。
	~DockPollSettingsState();
	DockPollSettingsState(const DockPollSettingsState &) = delete;
	DockPollSettingsState &operator=(const DockPollSettingsState &) = delete;
};

// 設定と参加者の設定に関わる処理。static の関数だけを持ち、第 1 引数に DockPoll を取る。
// DockPoll はこの型を friend にしている。
struct DockPollSettingsPart {
	// 参加者の設定ウィンドウを開く。開いていれば前面に出す。
	static void open_participants(DockPoll &self);
	// OBS のテーマが変わったときに呼ぶ。開いているウィンドウに伝える。
	static void on_theme_changed(DockPoll &self);
	// 設定ウィンドウか参加者の設定ウィンドウが閉じた後に呼ぶ。すぐに tick() を 1 回呼び、閉じたことを画面に
	// 反映する。
	static void on_window_closed(DockPoll &self);

	// 設定ウィンドウ (「cordmuxの設定」) を page のページで開く。開いていれば前面に出し、そのページに移る。
	// start_export が true なら、開いた後に「バージョン情報」のページで、診断情報のエクスポートを始める。
	// ドックのメニューの [診断情報をエクスポート...] も、直接エクスポートせずにこの関数を使う。エクスポートの
	// 結果を、設定ウィンドウのボタンの下に表示するためである。
	static void open_settings(DockPoll &self, ledger::SettingsPage page, bool start_export);
	// 開いているウィンドウ (設定ウィンドウの「バージョン情報」と、参加者の設定ウィンドウ) を、ドックと同じ
	// View で書き換える。ウィンドウとドックが同じ値を表示するためである。
	static void refresh_windows(DockPoll &self, const ledger::dockview::DockViewV2 &view);

	// 設定ウィンドウの [適用] と [OK] から呼ぶ (SettingsWindowInput::on_apply)。ウィンドウの値を clamp_settings で
	// 範囲に収めてから反映する。保存できたら true を返す。
	static bool apply_from_window(DockPoll &self, const config::Settings &settings);
	// OBS の [ツール] メニューの「cordmuxをアンインストール...」から呼ぶ。設定ウィンドウを「バージョン情報」の
	// ページで開き (open_settings と同じ)、ウィンドウを前面に出した後に SettingsWindow::start_uninstall を呼ぶ。
	// アンインストールできる状態なら確認のダイアログを開き、できない状態なら、ページのいちばん下と、できない理由を
	// 表示する。この関数を呼んだだけでは何も削除しない。
	static void open_settings_to_uninstall(DockPoll &self);
};

} // namespace cordmux::dock
