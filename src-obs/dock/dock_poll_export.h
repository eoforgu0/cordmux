// SPDX-License-Identifier: Apache-2.0

// DockPoll の処理のうち、診断情報のエクスポートの状態 (DockPollExportState) と関数 (DockPollExportPart) を宣言する。
// 実装は dock_poll_export.cpp にある。DockPoll::export_bundle_now の宣言は dock_poll.h にある。dock_poll.h を
// include しない (dock_poll.h がこのヘッダーを include する)。Qt (QObject、QPointer) を含む。

#pragma once

#include <QObject>
#include <QPointer>
#include <QWidget>

#include <memory>
#include <string>
#include <thread>

#include "bundle/export_bundle.h"

namespace cordmux::dock {

class DockPoll;

// 診断情報のエクスポートの状態。DockPoll がメンバー export_state_ として持つ。ワーカースレッドは 1 つだけで、
// この構造体が持ち、破棄するときに join する。トークンを消す関数は plugin-main.cpp の関数なので、DLL がアンロードされた
// 後に実行されてはならない (obs_module_unload は先に DockPoll を破棄する)。結果は receiver (UI スレッドの QObject) に
// QueuedConnection で返す。receiver が先に破棄されたら、結果は捨てられる。
struct DockPollExportState {
	enum class Phase { Idle, Building, Asking, Writing };

	Phase phase = Phase::Idle;
	// エラーのダイアログと保存のダイアログの親 (エクスポートを始めた設定ウィンドウ。閉じていれば空)。
	QPointer<QWidget> owner;
	// 保存のダイアログに表示する既定のファイル名 (cordmux-diagnostics-<日時>.zip)。
	std::string zip_name;
	// ワーカースレッド (zip を作るか、書き込む)。1 つだけ。
	std::thread worker;
	// ワーカースレッドの結果を UI スレッドで受け取る QObject (QueuedConnection の呼び出しの宛先)。
	std::unique_ptr<QObject> receiver;

	DockPollExportState();
	~DockPollExportState();
	DockPollExportState(const DockPollExportState &) = delete;
	DockPollExportState &operator=(const DockPollExportState &) = delete;
};

// 診断情報のエクスポートの処理。static の関数だけを持ち、第 1 引数に DockPoll を取る。DockPoll はこの型を
// friend にしている。
//
// 流れ:
//   1. どこから始めても、設定ウィンドウを「バージョン情報」のページで開き、そのページの
//      [診断情報をエクスポート...] から始めたのと同じにする (DockPollSettingsPart::open_settings の start_export)。
//      ボタンは押した時点で「エクスポート中...」になる (設定ウィンドウが表示する)。
//   2. zip に入れるデータを UI スレッドで集め、トークンを消す処理、トークンが残っていないかの確認、zip の作成は
//      ワーカースレッドで行う (bundle::prepare_bundle)。データを UI スレッドで集めるのは、DockPoll の状態と Pipeline の
//      スナップショットを読むためである。残りは UI スレッドを止めないように、ワーカースレッドで行う。
//   3. トークンが残っていたか、zip を作れなかったら、保存のダイアログを開かずにエラーのダイアログを表示する。
//   4. 作れたら、設定ウィンドウを親にした保存のダイアログで保存先を選んでもらい、ワーカースレッドで書き込む
//      (bundle::write_bundle)。キャンセルされたら何も表示しない。
//   5. 書き込めたら DockPoll の ops_.export_result に置き、すぐに tick() を 1 回呼んで、ボタンの下に結果と場所の
//      行を表示する。結果は設定ウィンドウを閉じるまで表示する。書き込めなかったら、エラーのダイアログを表示する。
struct DockPollExportPart {
	// エクスポートの処理中か。設定ウィンドウを開くときに、ボタンを「エクスポート中...」で表示するために使う。
	static bool busy(const DockPoll &self);
	// エクスポートを始める。owner はエラーのダイアログと保存のダイアログの親 (設定ウィンドウ)。
	// 処理中なら何もしない。
	static void begin(DockPoll &self, QWidget *owner);
	// ワーカースレッドの結果を UI スレッドで受け取る (DockPollExportState::receiver への QueuedConnection の
	// 呼び出しから呼ばれる)。
	static void on_prepared(DockPoll &self, const bundle::PreparedBundle &prepared);
	static void on_written(DockPoll &self, const bundle::ExportResult &result);
};

} // namespace cordmux::dock
