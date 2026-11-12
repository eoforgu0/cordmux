// SPDX-License-Identifier: Apache-2.0

// 押した場所の隣で確認する操作の判断。全体に影響する操作だけを確認し、戻せる操作 (チェック、ミュート) には確認を付けない。
// フォーカスを奪わないようにダイアログにせず、ボタンの場所に [確定] と、やめる側のボタンを置く (dock/inline_confirm.h)。
// やめる側のボタンは、押した結果が文言から分かるようにする ([通話を続ける])。[キャンセル] だと、通話がどうなるかを
// 推測することになる。
// libobs にも Qt にも依存しない純粋関数である。

#pragma once

#include <optional>

#include "ledger/actions.h"
#include "ledger/text_ids.h"

namespace cordmux::ledger {

// 確認のボタンを置く場所。LeaveButtons はサブの行の [移動] と [退出] の場所。
enum class ConfirmPlace { LeaveButtons };

struct ConfirmPlan {
	ConfirmPlace place = ConfirmPlace::LeaveButtons;
	// [確定] のボタンの ini のキー。ボタンのテキストは動作の名前にする。「OK」だと、何が起きるかを読み直すことになる。
	const char *confirm_key = nullptr;
	// やめる側のボタンの ini のキー。押すと確認を閉じて、操作を実行せずに、そのままにする。
	const char *cancel_key = nullptr;
};

struct ConfirmFacts {
	bool streaming = false;               // OBS が配信中か
	bool confirm_while_streaming = true;  // 設定 confirm_while_streaming
};

// 押された操作を確認するかを決める。確認しないときは持たない値を返す。
//   - [退出] は全員の映像と声が止まるので、配信中で設定 (confirm_while_streaming) がオンのときだけ確認する。配信していない
//     ときは確認しない。
//   - ほかの操作は、配信中でも確認しない (押したらすぐ実行する)。録画の終了、ダミー映像の表示、[無効にする] も確認しない。
//     確認は 1 回の操作を 2 回に増やすので、戻せない操作か、戻すのに手間がかかる操作にだけ付ける。
inline std::optional<ConfirmPlan> confirm_needed(ActionId id, const ConfirmFacts &facts)
{
	if (id == ActionId::LeaveCall && facts.streaming && facts.confirm_while_streaming)
		return ConfirmPlan{ConfirmPlace::LeaveButtons, text::keys::kDockAccountLeaveButton,
				   text::keys::kDockConfirmStayInCallButton};
	return std::nullopt;
}

} // namespace cordmux::ledger
