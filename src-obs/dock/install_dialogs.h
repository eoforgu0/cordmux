// SPDX-License-Identifier: Apache-2.0

// インストールとアンインストールのダイアログの中身を、ui::MessageSpec にする。何をどの順に表示するかは
// dock/install_dialog_plan.h が決め、ここではテキストを取得して MessageSpec にするだけである。ダイアログそのもの (形、
// 親ウィンドウ、フォーカス、音を鳴らさないこと) は dock/ui/message_dialog.* が持つ。どのダイアログも、操作を止めるのは
// 親ウィンドウだけである (MessageBlocking::OwnerOnly。理由は install_dialogs.cpp の spec_of)。
// どの関数もダイアログを開かないので、プレビューツールの `--face dialog:<id>` とドックを操作するテストも、
// 同じ関数で描画できる。[OK] と [キャンセル] は、OBS の [設定] と同じテキストにするため、OBS 自身のテキストを
// obs_frontend_get_locale_string で取得する (ledger/text_ids.h の obs_keys)。

#pragma once

#include <string>

#include "dock/install_flow.h"
#include "dock/ui/message_dialog.h"
#include "ledger/dock_view.h"

namespace cordmux::dock {

// ダイアログの id。プレビューツールの `--face dialog:<id>` と、ダイアログの動的プロパティ dialogId の値に使う。
inline constexpr const char *kDialogIdUninstallConfirm = "uninstall-confirm";
inline constexpr const char *kDialogIdSwitchConfirm = "switch-confirm";
inline constexpr const char *kDialogIdUninstallDone = "uninstall-done";
inline constexpr const char *kDialogIdUninstallUnconfirmed = "uninstall-unconfirmed";
inline constexpr const char *kDialogIdUninstallStopped = "uninstall-stopped";
inline constexpr const char *kDialogIdUninstallLoose = "uninstall-loose";

// アンインストールの確認のダイアログ。ボタンは [アンインストール] (0 番) と [キャンセル] (1 番。既定のボタンで、
// Esc でも選ばれる)。loose は、手順書と違う置き方 (DLL のフォルダーが plugins\<名前> でない) か (最後の文を選ぶ。install_dialog_plan.h の
// uninstall_confirm_plan)。
ui::MessageSpec uninstall_confirm_spec(bool loose = false);

// ほかの Vencord から、cordmux に同梱の Vencord に切り替える確認のダイアログ。[Vesktopに追加] を押したときに、
// Vesktop の設定がほかの Vencord を指しているときだけ開く。current_vencord_dir は、押した時点の Vesktop の設定の値
// (UTF-8)。ボタンは [切り替え] (0 番) と [キャンセル] (1 番。既定のボタンで、Esc でも選ばれる)。
ui::MessageSpec switch_confirm_spec(const std::string &current_vencord_dir);

// アンインストールの結果のダイアログ。v は ledger の decide_dock が返す DockViewV2::uninstall_result。ボタンは
// [OK] だけ (0 番。既定のボタンで、Esc でも選ばれる)。[フォルダーを開く] は、フォルダーの場所の行ごとに、
// その行の右にある。終わったときの「削除するフォルダー:」の行だけは、[フォルダーの場所を開く] にする
// (dock/install_dialog_plan.h の DialogBlock::folder_location)。loose は、手順書と違う置き方で終わったときに表示する、
// 利用者に削除してもらうファイル (nullptr なら、zip の cordmux フォルダーと同じ名前だという 1 文にする)。[ファイルの場所を開く] と
// [フォルダーの場所を開く] で呼ぶ関数 (MessageSpec::reveal_file) は、呼び出し元が設定する。
ui::MessageSpec uninstall_result_spec(const ledger::dockview::UninstallResultView &v,
				      const LooseInstallFiles *loose = nullptr);

// 結果のダイアログの id (上の 4 つのどれか)。
const char *uninstall_result_dialog_id(const ledger::dockview::UninstallResultView &v);

} // namespace cordmux::dock
