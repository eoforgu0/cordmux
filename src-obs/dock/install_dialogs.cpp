// SPDX-License-Identifier: Apache-2.0

// インストールとアンインストールのダイアログの中身を、ui::MessageSpec にする (install_dialogs.h の実装)。

#include "dock/install_dialogs.h"

#include <obs-frontend-api.h>

#include <QString>

#include "dock/install_dialog_plan.h"
#include "dock/ui/text_resolve.h"
#include "ledger/text_ids.h"

namespace cordmux::dock {

using ledger::dockview::UninstallResultKind;
using ledger::dockview::UninstallResultView;

namespace {

// OBS 自身の ini のテキスト ([OK] と [キャンセル])。取得できなければ、キーをそのまま返す (obs_module_text と同じ動作)。
// プレビューツールでは、tests/ui_preview/stub/obs-frontend-api.h と
// tests/ui_preview/locale_stub.cpp が、同じ関数を OBS の ini から提供する。
QString obs_text(const char *key)
{
	const char *text = obs_frontend_get_locale_string(key);
	return QString::fromUtf8(text != nullptr ? text : key);
}

QString path_text(const std::string &utf8)
{
	return QString::fromUtf8(utf8.c_str());
}

bool label_is_empty(const ledger::Label &label)
{
	return !label.is_text() && label.literal.empty();
}

ui::MessageItem item_shape_of(const DialogBlock &block)
{
	switch (block.kind) {
	case DialogBlock::Kind::Paragraph:
		return ui::MessageItem::paragraph(ui::resolve_label(block.text));
	case DialogBlock::Kind::Bullet:
		return ui::MessageItem::bullet(ui::resolve_label(block.text));
	case DialogBlock::Kind::Location:
		break;
	}
	const QString heading = label_is_empty(block.text) ? QString() : ui::resolve_label(block.text);
	// 親フォルダーの中で選択して見せる、フォルダーの場所の行。[フォルダーの場所を開く] は、そのテキストと開く関数が
	// MessageSpec にあるときだけ、dock/ui が置く。
	if (block.folder_location)
		return ui::MessageItem::folder_location_in_parent(heading, path_text(block.path), block.under_bullet);
	// ファイルの場所の行。[ファイルの場所を開く] は、そのテキストと開く関数が MessageSpec にあるときだけ、
	// dock/ui が置く。
	if (block.file)
		return ui::MessageItem::file_location(heading, path_text(block.path), block.under_bullet);
	return ui::MessageItem::location(heading, path_text(block.path), block.open_folder, block.under_bullet);
}

ui::MessageItem item_of(const DialogBlock &block)
{
	ui::MessageItem item = item_shape_of(block);
	// 2 つ目からのグループの始まりの前は、まとまりの間の余白にする (DialogBlock::group_start)。どの種類の要素にも、
	// 同じようにコピーする。
	item.group_start = block.group_start;
	return item;
}

ui::MessageButton button_of(const DialogButton &button)
{
	ui::MessageButton b;
	switch (button.kind) {
	case DialogButton::Kind::Confirm:
		b.text = ui::resolve(button.text);
		b.role = ui::MessageButtonRole::Accept;
		break;
	case DialogButton::Kind::Cancel:
		b.text = obs_text(ledger::text::obs_keys::kCancel);
		b.role = ui::MessageButtonRole::Reject;
		break;
	case DialogButton::Kind::Ok:
		b.text = obs_text(ledger::text::obs_keys::kOk);
		b.role = ui::MessageButtonRole::Accept;
		break;
	}
	b.is_default = button.is_default;
	b.is_escape = button.is_escape;
	return b;
}

ui::MessageSpec spec_of(const DialogPlan &plan)
{
	ui::MessageSpec s;
	// 操作を止めるのは、親ウィンドウだけにする (OwnerOnly)。設定ウィンドウから開いたダイアログも、ドックから開いた
	// ダイアログ (切り替えの確認と、ドックから始めたアンインストール) も同じである。Qt の WindowModal は親の祖先の
	// ウィンドウをすべて止めるので、設定ウィンドウからならメインウィンドウとフローティングのドックまで、
	// フローティングのドックからならメインウィンドウまで止める。配信の間にメインウィンドウを止めないために、
	// OwnerOnly にする ([Vesktopに追加] は配信中も押せるので、切り替えの確認は配信中にも開く)。
	// ドックがメインウィンドウに入っているときは、親がメインウィンドウそのものなので、どちらでもメインウィンドウは
	// 止まる (違いが出るのは、フローティングのドックと設定ウィンドウのときだけ)。
	// ダイアログの中身と同じ所で形を決めるのは、製品、プレビューツール、ドックを操作するテストが、
	// 同じ形を見るためである。ドックから開いたダイアログを WindowModal にすると、フローティングのドックから
	// 開いたときにメインウィンドウが止まり、メインウィンドウとドックは配信中も使える、という約束と食い違う。
	// 配信中は [Vesktopに追加] を押せなくする方法では、配信中でないときに、フローティングのドックのダイアログが
	// メインウィンドウを止めたままになる。呼び出し元で親ごとに選ぶと、同じダイアログが呼ぶ所によって違う形になり、
	// プレビューツールとテストが製品の形を見ない。
	s.blocking = ui::MessageBlocking::OwnerOnly;
	s.title = ui::resolve(plan.title);
	s.icon = plan.icon == DialogIcon::Warning ? ui::MessageIcon::Warning : ui::MessageIcon::Information;
	s.main = ui::resolve(plan.main);
	for (const DialogBlock &block : plan.blocks)
		s.items.push_back(item_of(block));
	s.open_folder_text = ui::resolve(plan.open_folder_text);
	// [ファイルの場所を開く] のテキスト。押したときに呼ぶ関数 (reveal_file) は、製品の呼び出し元
	// (dock_poll_install.cpp) が platform::open_file_location を設定する。Windows の API を platform/ の外で
	// 使わないためで、platform_win.cpp を持たないプレビューツールとテストは、自分の関数を設定する。
	if (!plan.open_file_location_text.id.empty())
		s.open_file_location_text = ui::resolve(plan.open_file_location_text);
	// [フォルダーの場所を開く] のテキスト。押したときに呼ぶ関数は、同じ reveal_file である。
	if (!plan.open_folder_location_text.id.empty())
		s.open_folder_location_text = ui::resolve(plan.open_folder_location_text);
	for (const DialogButton &button : plan.buttons)
		s.buttons.push_back(button_of(button));
	return s;
}

} // namespace

ui::MessageSpec uninstall_confirm_spec(bool loose)
{
	return spec_of(uninstall_confirm_plan(loose));
}

ui::MessageSpec switch_confirm_spec(const std::string &current_vencord_dir)
{
	return spec_of(switch_confirm_plan(current_vencord_dir));
}

ui::MessageSpec uninstall_result_spec(const UninstallResultView &v, const LooseInstallFiles *loose)
{
	return spec_of(uninstall_result_plan(v, loose));
}

const char *uninstall_result_dialog_id(const UninstallResultView &v)
{
	switch (v.kind) {
	case UninstallResultKind::Stopped:
		return kDialogIdUninstallStopped;
	case UninstallResultKind::DoneUnconfirmed:
		return kDialogIdUninstallUnconfirmed;
	case UninstallResultKind::Done:
		break;
	}
	return uninstall_result_is_loose(v) ? kDialogIdUninstallLoose : kDialogIdUninstallDone;
}

} // namespace cordmux::dock
