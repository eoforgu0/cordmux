// SPDX-License-Identifier: Apache-2.0

// dock/self_frame_check.h の単体テスト。ドックが自分の行のチェックを Pipeline に渡し、Pipeline の答えを受ける所
// (DockPoll::push_assign_policy と carry_self_frame_check) の判断を、製品の関数のまま確かめる。Pipeline の代わりに、Pipeline が
// 割り当て表と同じロック (Impl::mu) の中で呼ぶ関数 (pipeline/pipeline.h の set_assign_policy_in と move_self_frame_in) を呼ぶ
// FakePipeline を間に挟み、Pipeline のスレッドが configure を適用することを決まった順に割り込ませる。
// Pipeline の側の判断そのもの (上限、付け替え) は test_pipeline_stream_router.cpp が確かめる。

#include "doctest.h"

#include "config/allocation.h"
#include "config/settings.h"
#include "dock/self_frame_check.h"
#include "pipeline/pipeline.h"
#include "pipeline/stream_router.h"
#include "sources/self_frame_plan.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using cordmux::config::AllocationTable;
using cordmux::config::SlotKind;
using cordmux::dock::assign_policy_pushed;
using cordmux::dock::plan_self_frame_carry;
using cordmux::dock::refuse_self_frame_step;
using cordmux::dock::self_frame_carry_facts;
using cordmux::dock::self_frame_in_use_of;
using cordmux::dock::self_frame_push_due;
using cordmux::dock::settle_self_frame_carry;
using cordmux::dock::SelfFrameCarryFacts;
using cordmux::dock::SelfFrameCarryPlan;
using cordmux::dock::SelfFrameCheck;
using cordmux::dock::SelfFrameLog;
using cordmux::dock::SelfFrameLogLine;
using cordmux::dock::SelfFrameMoveKind;
using cordmux::dock::SelfFrameStep;
using cordmux::pipeline::AssignPolicy;
using cordmux::pipeline::AssignPolicyUpdate;
using cordmux::pipeline::FactsSnapshot;
using cordmux::pipeline::Pipeline;
using cordmux::pipeline::PipelineSnapshot;
using cordmux::pipeline::StreamRouter;
using cordmux::ws::ConfigureMessage;
using cordmux::ws::StreamConfig;
using cordmux::ws::StreamKind;
using Outcome = Pipeline::SlotChange::Outcome;

namespace {

// メインアカウント (自分の枠の人) と、Vesktop のアカウント (サブアカウント)。
const std::string kMain = "main";
const std::string kSub = "sub";

// users の画面のストリーム (stream_id は 1 から順に振る)。
ConfigureMessage screens_of(const std::vector<std::string> &users)
{
	ConfigureMessage msg;
	std::uint32_t id = 1;
	for (const std::string &user : users) {
		StreamConfig c;
		c.stream_id = id++;
		c.user_id = user;
		c.kind = StreamKind::Screen;
		msg.streams.push_back(c);
	}
	return msg;
}

// Pipeline の代わり。割り当て表、経路、決め方、facts を持つ。自分の枠に関わる 2 つの呼び出しは、Pipeline と同じく、割り当て表と
// 同じロックの中で呼ぶ製品の関数 (pipeline.h の set_assign_policy_in と move_self_frame_in。自分の枠の人は self_frame_owner_of) を
// 呼び、変えたときは、画面共有している人を載せ直してから (Impl::admit_sharers_from_facts_locked が呼ぶ StreamRouter::admit_sharers)、
// 直前の configure を適用し直す (pipeline.cpp の set_assign_policy と move_self_frame)。受け取る範囲は既定の Slots、選び方は
// 「自動」で、「自動で載せる」の人と使う声はない。ログ、ミキサー、視聴する人の範囲など、割り当て表と決め方を変えない後処理は
// 持たない。
struct FakePipeline {
	AllocationTable table;
	StreamRouter router;
	AssignPolicy policy;
	FactsSnapshot facts;
	ConfigureMessage last;

	explicit FakePipeline(bool selected, bool in_use)
	{
		policy.automatic = true;
		policy.main_account_user_id = kMain;
		policy.receiver_user_id = kSub;
		policy.self_frame_selected = selected;
		policy.self_frame_in_use = in_use;
		facts.received = true;
		facts.sharing_valid = true;
		facts.self_user_id = kSub;
	}

	// Pipeline のスレッドが configure を適用する (自動で載せる経路)。画面のストリームを送ってきた人は、facts の sharing にもいる。
	void configure(const ConfigureMessage &msg)
	{
		last = msg;
		facts.sharing.user_ids.clear();
		for (const StreamConfig &c : msg.streams)
			facts.sharing.user_ids.push_back(c.user_id);
		facts.sharing.sent_count = static_cast<int>(facts.sharing.user_ids.size());
		router.apply_configure(msg, table, policy);
	}

	bool set_assign_policy(bool selected, bool in_use)
	{
		const AssignPolicyUpdate update = cordmux::pipeline::set_assign_policy_in(
			table, router, policy, facts, cordmux::config::AssignMode::Auto, {}, {}, kMain, selected,
			in_use);
		if (update.changed)
			readmit_and_replay();
		return update.self_frame_in_use;
	}

	Pipeline::SelfFrameMove move_self_frame(const std::string &user_id, bool to_self_frame,
					       std::optional<std::int64_t> after_order)
	{
		const Pipeline::SelfFrameMove moved = cordmux::pipeline::move_self_frame_in(
			table, router, policy,
			cordmux::pipeline::self_frame_owner_of(policy.main_account_user_id, facts), user_id,
			to_self_frame, after_order);
		readmit_and_replay();
		return moved;
	}

	void readmit_and_replay()
	{
		router.admit_sharers(facts.sharing.user_ids, table, policy);
		router.apply_configure(last, table, policy);
	}

	// ドックが取るスナップショット (画面の枠、facts の sharing、Vesktop のアカウント)。
	PipelineSnapshot snapshot(const std::vector<std::string> &sharing) const
	{
		PipelineSnapshot out;
		for (int slot = 1; slot <= cordmux::config::kScreenSlotCount; ++slot) {
			const cordmux::config::SlotAssignment *a = table.assignment(SlotKind::Screen, slot);
			if (a == nullptr)
				continue;
			cordmux::pipeline::SlotSnapshot row;
			row.kind = SlotKind::Screen;
			row.slot = slot;
			row.owner_user_id = a->owner_user_id;
			row.added_order = a->added_order;
			out.slots.push_back(row);
		}
		out.facts.received = true;
		out.facts.sharing_valid = true;
		out.facts.sharing.user_ids = sharing;
		out.facts.sharing.sent_count = static_cast<int>(sharing.size());
		out.facts.self_user_id = kSub;
		return out;
	}

	int slot_of(const std::string &user_id) const { return table.slot_of_owner(SlotKind::Screen, user_id); }
	int screen_count() const { return table.assigned_count(SlotKind::Screen); }
};

// メインアカウントを使うサブアカウントの構成の設定。selected は、自分の枠に映す映像を OBS から選んでいるか。
cordmux::config::Settings settings_with(bool selected)
{
	cordmux::config::Settings s;
	s.broadcaster_user_id = kMain;
	if (selected) {
		s.self_frame_source_uuid = "uuid-of-a-source";
		s.self_frame_source_name = "Window Capture";
	}
	return s;
}

// layout_step が自分の枠を付けて、配置の入力に入れた後の値 (DockPoll::step_self_frame の結果)。
SelfFrameCheck shown_check(bool pushed_in_use)
{
	SelfFrameCheck check;
	check.state.on = true;
	check.state.prev = cordmux::sources::SelfShare::On;
	check.pushed_in_use = pushed_in_use;
	check.facts.self_frame_bound = true;
	check.facts.self_frame_on = true;
	check.facts.self_frame_shown = true;
	check.facts.self_frame_share = cordmux::ledger::SelfShareState::On;
	return check;
}

bool logs_are(const SelfFrameStep &step, const std::vector<SelfFrameLog> &expected)
{
	if (step.logs.size() != expected.size())
		return false;
	for (std::size_t i = 0; i < expected.size(); ++i) {
		if (step.logs[i].what != expected[i])
			return false;
	}
	return true;
}

// 描く物 (Discord の人の枠と、配置の入力に入れた自分の枠) の数。
int drawn(const FakePipeline &pipeline, const SelfFrameCheck &check)
{
	return pipeline.screen_count() + (check.facts.self_frame_shown ? 1 : 0);
}

} // namespace

// ---- push_assign_policy: Pipeline の答えを受ける ----

TEST_CASE("Pipeline が自分の枠を使い始めることを受け付けたら、受け付けた値を覚え、チェックも設定も変えない")
{
	FakePipeline pipeline(true, false);
	pipeline.configure(screens_of({"a", "b", "c"}));
	const SelfFrameCheck check = shown_check(false);
	REQUIRE(self_frame_push_due(check, true));

	const bool requested = self_frame_in_use_of(check, true);
	REQUIRE(requested);
	const SelfFrameStep step =
		assign_policy_pushed(check, true, requested, pipeline.set_assign_policy(true, requested));
	CHECK(step.next.pushed_in_use);
	CHECK(step.next.state.on);
	CHECK(step.next.facts.self_frame_shown);
	CHECK(step.settings_on);
	CHECK_FALSE(step.write_settings);
	CHECK(step.released_slot == 0);
	CHECK(step.logs.empty());
	CHECK_FALSE(self_frame_push_due(step.next, true));
	CHECK(drawn(pipeline, step.next) == 4);
}

TEST_CASE("競合: チェックを付けた後に自動で 4 人目が載っていれば、Pipeline が断り、チェックと、この周期の配置の入力から自分の枠を外す")
{
	// ドックは、Discord の人が 3 人のスナップショットで自分の枠を付け (チェックの操作、または共有の始まり)、layout_step が配置の
	// 入力にも入れた。settings.json にはもう付けたことを書いてある。
	FakePipeline pipeline(true, false);
	pipeline.configure(screens_of({"a", "b", "c"}));
	SelfFrameCheck check = shown_check(false);
	check.state.excluded = false;
	check.mark.decided = true;
	check.mark.mark = 3;

	// Pipeline に渡す前に、Pipeline のスレッドが configure を適用し、自動で d を 4 人目に載せた。
	pipeline.configure(screens_of({"a", "b", "c", "d"}));
	REQUIRE(pipeline.screen_count() == 4);

	const bool requested = self_frame_in_use_of(check, true);
	const bool accepted = pipeline.set_assign_policy(true, requested);
	CHECK_FALSE(accepted);
	const SelfFrameStep step = assign_policy_pushed(check, true, requested, accepted);

	// チェックを外す。excluded にはしない (自動なら、枠が空けば付く)。直前の共有の値は変えない。
	CHECK_FALSE(step.next.state.on);
	CHECK_FALSE(step.next.state.excluded);
	CHECK(step.next.state.prev == cordmux::sources::SelfShare::On);
	CHECK_FALSE(step.next.pushed_in_use);
	// この周期の配置の入力からも外す (入れたままだと、この周期だけ 5 つになる)。結び付きと共有の値は変えない。
	CHECK_FALSE(step.next.facts.self_frame_on);
	CHECK_FALSE(step.next.facts.self_frame_shown);
	CHECK(step.next.facts.self_frame_bound);
	CHECK(step.next.facts.self_frame_share == cordmux::ledger::SelfShareState::On);
	CHECK(drawn(pipeline, step.next) == 4);
	// settings.json に外したことを今書く。ログは 1 行。
	CHECK_FALSE(step.settings_on);
	CHECK(step.write_settings);
	CHECK(logs_are(step, {SelfFrameLog::TurnedOffRefused}));
	CHECK(step.released_slot == 0);
	// 載せた順の印は、このステップでは変えない (チェックが外れたので、次に配置の入力を作るときに決め直す)。
	CHECK(step.next.mark.decided);
	// Pipeline が受け付けた値 (偽) と、外した後の使っているか (偽) がそろうので、周期ごとに渡し直さない。
	CHECK_FALSE(self_frame_push_due(step.next, true));

	// d が画面共有をやめて枠が空くと、次に付けたときは受け付ける。
	pipeline.table.release(SlotKind::Screen, pipeline.slot_of("d"));
	pipeline.configure(screens_of({"a", "b", "c"}));
	SelfFrameCheck again = step.next;
	again.state.on = true;
	again.facts.self_frame_on = true;
	again.facts.self_frame_shown = true;
	REQUIRE(self_frame_push_due(again, true));
	const SelfFrameStep accepted_step =
		assign_policy_pushed(again, true, true, pipeline.set_assign_policy(true, true));
	CHECK(accepted_step.next.pushed_in_use);
	CHECK(accepted_step.next.state.on);
	CHECK(accepted_step.logs.empty());
	// d が画面共有を始め直しても、自分の枠の分の枠には載らない。
	pipeline.configure(screens_of({"a", "b", "c", "d"}));
	CHECK(pipeline.slot_of("d") == 0);
	CHECK(drawn(pipeline, accepted_step.next) == 4);
}

TEST_CASE("Pipeline が断ったとき、settings.json がもう外したことになっていれば、書き直さない")
{
	// 例えば、付けた値を書く前に断られた。今の値と同じなので書かない。
	const SelfFrameStep step = refuse_self_frame_step(shown_check(true), false);
	CHECK_FALSE(step.next.state.on);
	CHECK_FALSE(step.settings_on);
	CHECK_FALSE(step.write_settings);
	CHECK(logs_are(step, {SelfFrameLog::TurnedOffRefused}));
	// 受け付けた値は、ここでは変えない (断った呼び出しの戻り値を覚えるのは assign_policy_pushed と settle_self_frame_carry)。
	CHECK(step.next.pushed_in_use);
}

TEST_CASE("使うのをやめる値を渡して偽が返るのは、断られたのではないので、チェックを外さない")
{
	// 映像を OBS から選んでいない間は、チェックが付いていても使っていない。
	SelfFrameCheck check = shown_check(true);
	const bool requested = self_frame_in_use_of(check, false);
	REQUIRE_FALSE(requested);
	REQUIRE(self_frame_push_due(check, false));
	FakePipeline pipeline(false, true);
	pipeline.configure(screens_of({"a", "b", "c", "d"}));
	const SelfFrameStep step = assign_policy_pushed(check, true, requested, pipeline.set_assign_policy(false, requested));
	CHECK_FALSE(step.next.pushed_in_use);
	CHECK(step.next.state.on);
	CHECK(step.next.facts.self_frame_shown);
	CHECK(step.settings_on);
	CHECK_FALSE(step.write_settings);
	CHECK(step.logs.empty());
	CHECK_FALSE(self_frame_push_due(step.next, false));
}

// ---- carry_self_frame_check: 映す映像を替えたときの引き継ぎ ----

TEST_CASE("映像を OBS のソースに替えると、メインアカウントの枠を空けて自分の枠を使い始め、並びの位置を引き継ぐ")
{
	// 映像は「Discordの画面共有」で、メインアカウントが画面共有して 2 番目に載っている。e は自動で待っている。
	FakePipeline pipeline(false, false);
	const ConfigureMessage msg = screens_of({"a", kMain, "b", "c", "e"});
	pipeline.configure(msg);
	REQUIRE(pipeline.screen_count() == 4);
	const int main_slot = pipeline.slot_of(kMain);
	REQUIRE(main_slot != 0);
	const std::int64_t main_order = pipeline.table.assignment(SlotKind::Screen, main_slot)->added_order;

	const SelfFrameCarryFacts facts =
		self_frame_carry_facts(settings_with(true), pipeline.snapshot({"a", kMain, "b", "c", "e"}));
	CHECK(facts.owner == kMain);
	CHECK(facts.carries);
	CHECK(facts.owner_slot == main_slot);
	CHECK(facts.owner_order == main_order);
	CHECK_FALSE(facts.owner_not_sharing);

	SelfFrameCheck check; // 「Discordの画面共有」の間は、自分の枠の状態を動かさない
	const SelfFrameCarryPlan plan = plan_self_frame_carry(check, false, facts, true);
	REQUIRE(plan.move == SelfFrameMoveKind::ToSelfFrame);
	CHECK(plan.owner == kMain);
	// Pipeline を呼ぶ前に、チェックを自分の枠に移したことを当てる (settings.json は、設定を替えた後処理が書く)。
	CHECK(plan.before.next.state.on);
	CHECK(plan.before.settings_on);
	CHECK_FALSE(plan.before.write_settings);
	CHECK(logs_are(plan.before, {SelfFrameLog::TurnedOnVideoChanged}));
	// 自分の枠を、メインアカウントの枠がいた位置に置く印。
	CHECK(plan.before.next.mark.decided);
	CHECK(plan.before.next.mark.mark == main_order - 1);

	const Pipeline::SelfFrameMove moved = pipeline.move_self_frame(plan.owner, true, plan.after_order);
	const SelfFrameStep step = settle_self_frame_carry(plan, moved);
	CHECK(step.next.pushed_in_use);
	CHECK(step.next.state.on);
	CHECK(step.next.mark.mark == main_order - 1);
	CHECK(step.settings_on);
	CHECK_FALSE(step.write_settings);
	// 空けたメインアカウントの枠は、ドックが片付ける (画面と名前ラベルを空けて、並べ直す)。
	CHECK(step.released_slot == main_slot);
	REQUIRE(step.logs.size() == 1);
	CHECK(step.logs[0].what == SelfFrameLog::RemovedFromSlot);
	CHECK(step.logs[0].slot == main_slot);
	// 待っていた e は、自分の枠の分の枠に載らない。
	CHECK(pipeline.slot_of(kMain) == 0);
	CHECK(pipeline.slot_of("e") == 0);
	CHECK(pipeline.screen_count() == 3);
	CHECK(self_frame_push_due(step.next, true) == false);
}

TEST_CASE("競合: OBS のソースに替える間にメインアカウントの枠が空いて自動で 4 人目が載っていれば、Pipeline が断り、チェックを外す")
{
	FakePipeline pipeline(false, false);
	pipeline.configure(screens_of({"a", "b", "c", kMain, "e"}));
	REQUIRE(pipeline.slot_of(kMain) != 0);
	// ドックは、メインアカウントが載っているスナップショットで引き継ぎ方を決めた。
	const SelfFrameCarryFacts facts =
		self_frame_carry_facts(settings_with(true), pipeline.snapshot({"a", "b", "c", kMain, "e"}));
	const SelfFrameCarryPlan plan = plan_self_frame_carry(SelfFrameCheck{}, false, facts, true);
	REQUIRE(plan.move == SelfFrameMoveKind::ToSelfFrame);

	// Pipeline に渡す前に、メインアカウントが画面共有をやめて枠が空き、自動で待っていた e が載った。
	pipeline.table.release(SlotKind::Screen, pipeline.slot_of(kMain));
	pipeline.configure(screens_of({"a", "b", "c", "e"}));
	REQUIRE(pipeline.screen_count() == 4);

	const Pipeline::SelfFrameMove moved = pipeline.move_self_frame(plan.owner, true, plan.after_order);
	CHECK(moved.change.outcome == Outcome::NotAssigned);
	CHECK_FALSE(moved.self_frame_in_use);
	const SelfFrameStep step = settle_self_frame_carry(plan, moved);
	CHECK(logs_are(step, {SelfFrameLog::NotInSlot, SelfFrameLog::TurnedOffRefused}));
	CHECK(step.released_slot == 0);
	CHECK_FALSE(step.next.pushed_in_use);
	CHECK_FALSE(step.next.state.on);
	CHECK_FALSE(step.next.state.excluded);
	CHECK_FALSE(step.next.facts.self_frame_shown);
	// 前に当てた付けた値を、外した値で書き直す。
	CHECK_FALSE(step.settings_on);
	CHECK(step.write_settings);
	CHECK(drawn(pipeline, step.next) == 4);
}

TEST_CASE("OBS のソースに替えたときメインアカウントが枠にいなければ、前から付いていたチェックが残り、枠が埋まっていれば次に渡すときに外れる")
{
	// 「Discordの画面共有」の間に、メインアカウントを枠に載せられずに残したチェック。Discord の人で枠が埋まっている。
	FakePipeline pipeline(false, false);
	pipeline.configure(screens_of({"a", "b", "c", "d"}));
	SelfFrameCheck check;
	check.state.on = true;
	const SelfFrameCarryFacts facts =
		self_frame_carry_facts(settings_with(true), pipeline.snapshot({"a", "b", "c", "d"}));
	CHECK(facts.owner_slot == 0);
	const SelfFrameCarryPlan plan = plan_self_frame_carry(check, true, facts, true);
	CHECK(plan.move == SelfFrameMoveKind::None);
	CHECK(plan.before.next.state.on);
	CHECK(plan.before.logs.empty());
	// Pipeline を呼ばないので、答えを受けても何も変わらない。
	const SelfFrameStep none = settle_self_frame_carry(plan, Pipeline::SelfFrameMove{});
	CHECK(none.next.state.on);
	CHECK(none.logs.empty());

	// 設定を替えた後処理が渡す (DockPoll::on_settings_changed の push_assign_policy)。Pipeline が断り、チェックが外れる。
	const bool requested = self_frame_in_use_of(plan.before.next, true);
	REQUIRE(requested);
	const SelfFrameStep step =
		assign_policy_pushed(plan.before.next, plan.before.settings_on, requested, pipeline.set_assign_policy(true, requested));
	CHECK_FALSE(step.next.state.on);
	CHECK(step.write_settings);
	CHECK(logs_are(step, {SelfFrameLog::TurnedOffRefused}));
	CHECK(pipeline.screen_count() == 4);
}

TEST_CASE("映像を「Discordの画面共有」に替えると、自分の枠をやめてメインアカウントを載せ、自動で待っている人がいても 4 つを超えない")
{
	// 自分の枠を使っていて、Discord の a、b、c が載り、e が自動で待っている。メインアカウントも画面共有している。
	FakePipeline pipeline(true, true);
	const ConfigureMessage msg = screens_of({"a", "b", "c", kMain, "e"});
	pipeline.configure(msg);
	REQUIRE(pipeline.screen_count() == 3);
	REQUIRE(pipeline.slot_of("e") == 0);
	SelfFrameCheck check = shown_check(true);
	// 自分の枠は a の後に並んでいた。
	check.mark.decided = true;
	check.mark.mark = pipeline.table.assignment(SlotKind::Screen, pipeline.slot_of("a"))->added_order;

	const SelfFrameCarryFacts facts =
		self_frame_carry_facts(settings_with(false), pipeline.snapshot({"a", "b", "c", kMain, "e"}));
	const SelfFrameCarryPlan plan = plan_self_frame_carry(check, true, facts, false);
	REQUIRE(plan.move == SelfFrameMoveKind::ToScreenShare);
	REQUIRE(plan.after_order.has_value());
	CHECK(*plan.after_order == check.mark.mark);
	CHECK(plan.before.logs.empty());

	const Pipeline::SelfFrameMove moved = pipeline.move_self_frame(plan.owner, false, plan.after_order);
	CHECK(moved.change.outcome == Outcome::Assigned);
	const SelfFrameStep step = settle_self_frame_carry(plan, moved);
	CHECK_FALSE(step.next.state.on);
	CHECK_FALSE(step.next.pushed_in_use);
	CHECK_FALSE(step.settings_on);
	CHECK_FALSE(step.write_settings); // 設定を替えた後処理が書く
	CHECK(logs_are(step, {SelfFrameLog::TurnedOffVideoChanged}));
	CHECK(step.released_slot == 0);
	// メインアカウントは自分の枠の位置 (a の後) に載り、e は載らない。
	const int main_slot = pipeline.slot_of(kMain);
	REQUIRE(main_slot != 0);
	CHECK(pipeline.table.assignment(SlotKind::Screen, main_slot)->added_order == check.mark.mark + 1);
	CHECK(pipeline.slot_of("e") == 0);
	CHECK(pipeline.screen_count() == 4);
	CHECK_FALSE(self_frame_push_due(step.next, false));
}

TEST_CASE("「Discordの画面共有」に替えても、メインアカウントが画面共有していないか、枠が空いていなければ、チェックを残す")
{
	SUBCASE("画面共有していない (Pipeline を呼ばない)")
	{
		FakePipeline pipeline(true, true);
		pipeline.configure(screens_of({"a", "b"}));
		const SelfFrameCarryFacts facts = self_frame_carry_facts(settings_with(false), pipeline.snapshot({"a", "b"}));
		CHECK(facts.owner_not_sharing);
		const SelfFrameCarryPlan plan = plan_self_frame_carry(shown_check(true), true, facts, false);
		CHECK(plan.move == SelfFrameMoveKind::None);
		CHECK(plan.before.next.state.on);
		CHECK(logs_are(plan.before, {SelfFrameLog::KeptNotSharing}));
	}
	SUBCASE("画面共有しているかが分からない間は、載せようとする")
	{
		FakePipeline pipeline(true, true);
		pipeline.configure(screens_of({"a", "b", kMain}));
		PipelineSnapshot snapshot = pipeline.snapshot({});
		snapshot.facts.sharing_valid = false;
		const SelfFrameCarryFacts facts = self_frame_carry_facts(settings_with(false), snapshot);
		CHECK_FALSE(facts.owner_not_sharing);
		CHECK(plan_self_frame_carry(shown_check(true), true, facts, false).move == SelfFrameMoveKind::ToScreenShare);
	}
	SUBCASE("空いている枠がない")
	{
		// 普通は自分の枠の分の枠が空いているが、Discord の人が 4 人いる形で、載らなかったときの扱いを確かめる。
		FakePipeline pipeline(true, true);
		for (const char *id : {"a", "b", "c", "d"})
			REQUIRE(pipeline.table.try_assign(SlotKind::Screen, id).outcome ==
				cordmux::config::AssignOutcome::Assigned);
		const SelfFrameCarryFacts facts =
			self_frame_carry_facts(settings_with(false), pipeline.snapshot({"a", "b", "c", "d", kMain}));
		const SelfFrameCarryPlan plan = plan_self_frame_carry(shown_check(true), true, facts, false);
		REQUIRE(plan.move == SelfFrameMoveKind::ToScreenShare);
		const Pipeline::SelfFrameMove moved = pipeline.move_self_frame(plan.owner, false, plan.after_order);
		CHECK(moved.change.outcome == Outcome::Insufficient);
		const SelfFrameStep step = settle_self_frame_carry(plan, moved);
		CHECK(step.next.state.on);
		CHECK(step.settings_on);
		CHECK(logs_are(step, {SelfFrameLog::KeptNoFreeSlot}));
		// Pipeline は自分の枠を使うのをやめた (映像はもう OBS から選んでいない)。チェックは残っても、使っていることにはならない。
		CHECK_FALSE(step.next.pushed_in_use);
		CHECK_FALSE(self_frame_push_due(step.next, false));
	}
	SUBCASE("自分の枠のチェックが付いていない")
	{
		FakePipeline pipeline(true, false);
		pipeline.configure(screens_of({"a", kMain}));
		const SelfFrameCarryFacts facts = self_frame_carry_facts(settings_with(false), pipeline.snapshot({"a", kMain}));
		const SelfFrameCarryPlan plan = plan_self_frame_carry(SelfFrameCheck{}, false, facts, false);
		CHECK(plan.move == SelfFrameMoveKind::None);
		CHECK(plan.before.logs.empty());
	}
}

TEST_CASE("サブアカウントがない構成では、映像を替えても引き継ぐものがない")
{
	// 自分の枠の人は Vesktop のアカウント。その人の Discord の画面共有は受信側に届かない。
	cordmux::config::Settings settings = settings_with(true);
	settings.broadcaster_user_id.clear();
	FakePipeline pipeline(false, false);
	pipeline.configure(screens_of({"a", "b"}));
	const SelfFrameCarryFacts facts = self_frame_carry_facts(settings, pipeline.snapshot({"a", "b", kSub}));
	CHECK(facts.owner == kSub);
	CHECK_FALSE(facts.carries);
	CHECK(plan_self_frame_carry(SelfFrameCheck{}, false, facts, true).move == SelfFrameMoveKind::None);
	CHECK(plan_self_frame_carry(shown_check(true), true, facts, false).move == SelfFrameMoveKind::None);

	// メインアカウントが Vesktop のアカウントと同じ (サブアカウントがない) ときも同じ。
	settings.broadcaster_user_id = kSub;
	const SelfFrameCarryFacts same = self_frame_carry_facts(settings, pipeline.snapshot({"a", "b", kSub}));
	CHECK_FALSE(same.carries);
}
