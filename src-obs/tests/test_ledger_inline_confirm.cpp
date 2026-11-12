// SPDX-License-Identifier: Apache-2.0

// ledger/inline_confirm.h の、押した場所の隣で確認するかの判断の単体テスト。

#include "doctest.h"

#include "ledger/inline_confirm.h"
#include "ledger/text_ids.h"

#include <string>

using cordmux::ledger::ActionId;
using cordmux::ledger::confirm_needed;
using cordmux::ledger::ConfirmFacts;
using cordmux::ledger::ConfirmPlace;
namespace keys = cordmux::ledger::text::keys;

namespace {

ConfirmFacts facts_of(bool streaming, bool setting)
{
	ConfirmFacts facts;
	facts.streaming = streaming;
	facts.confirm_while_streaming = setting;
	return facts;
}

} // namespace

TEST_CASE("[退出] は、配信中で設定がオンのときだけ確認する")
{
	const auto plan = confirm_needed(ActionId::LeaveCall, facts_of(true, true));
	REQUIRE(plan.has_value());
	CHECK(plan->place == ConfirmPlace::LeaveButtons);
	CHECK(std::string(plan->confirm_key) == keys::kDockAccountLeaveButton);
	CHECK(std::string(plan->cancel_key) == keys::kDockConfirmStayInCallButton);
	// 配信していないとき (録画中を含む) と、設定がオフのときは確認しない。
	CHECK_FALSE(confirm_needed(ActionId::LeaveCall, facts_of(false, true)).has_value());
	CHECK_FALSE(confirm_needed(ActionId::LeaveCall, facts_of(true, false)).has_value());
}

TEST_CASE("[退出] 以外は、配信中で設定がオンでも確認しない")
{
	for (const ActionId id : {ActionId::ToggleRecording, ActionId::ToggleFeature, ActionId::ToggleDummy}) {
		for (const auto &facts : {facts_of(true, true), facts_of(false, true), facts_of(true, false)})
			CHECK_FALSE(confirm_needed(id, facts).has_value());
	}
}

TEST_CASE("戻せる操作には確認を付けない")
{
	for (const ActionId id : {ActionId::SetSlot, ActionId::SetVoiceMute, ActionId::SetScreenAudioMute, ActionId::MoveToMainCall,
				  ActionId::ReplaceSlot, ActionId::SetMainAccount})
		CHECK_FALSE(confirm_needed(id, facts_of(true, true)).has_value());
}
