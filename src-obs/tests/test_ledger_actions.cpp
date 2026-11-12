// SPDX-License-Identifier: Apache-2.0

// ledger/actions.h の単体テスト。押せる条件の表が ActionId を漏れなく持つことと、代表的な状態と引数の総当たりで
// apply() が仕事 (Intent) を返し、押せないときは理由を付けることを確認する。操作ごとの押せる条件も確認する。

#include "doctest.h"

#include "ledger/actions.h"
#include "ledger/rules.h"
#include "ledger/text_ids.h"

#include <string>
#include <vector>

using namespace cordmux::ledger;

namespace {

constexpr std::int64_t kNow = 1'000'000'000'000;
constexpr std::int64_t kSec = 1'000'000;

Observation healthy()
{
	Observation obs;
	obs.now_us = kNow;
	obs.choices.feature = FeatureSwitch::On;
	obs.choices.recording = FeatureSwitch::Off;
	obs.choices.composite_checked = true;
	obs.choices.composite_present = true;
	obs.choices.discord_audio_source = DiscordAudioSource::NotFound;
	obs.listener.state = ListenerState::Listening;
	obs.install.process = VesktopProcess::Running;
	obs.install.dir = VencordDirState::Matches;
	obs.link.state = LinkState::Connected;
	obs.link.connected_at_us = kNow - 60 * kSec;
	obs.call.presence = Presence::InCall;
	obs.call.roster_count = 4;
	obs.versions.plugin = "1.0.0";
	obs.versions.discord_side = "1.0.0";
	return obs;
}

// 総当たりに使う代表的な Observation。起動した直後、インストールの前と直後、問題が起きたとき、接続している
// ときを 1 つずつ並べる。
std::vector<Observation> representative_observations()
{
	std::vector<Observation> list;

	// 起動した直後 (すべて Unknown)。
	list.push_back(Observation{});

	// インストールした日 (state.json は読めたが、vencordDir のキーがまだない)。
	Observation day_one = healthy();
	day_one.install.dir = VencordDirState::NotConfigured;
	day_one.install.process = VesktopProcess::Running;
	day_one.link.state = LinkState::NeverConnected;
	day_one.link.connected_at_us.reset();
	day_one.choices.composite_present = false;
	list.push_back(day_one);

	// インストールする前 (Vesktop の設定をまだ書いていない)。
	Observation before = healthy();
	before.install.dir = VencordDirState::Unknown;
	before.install.process = VesktopProcess::NotRunning;
	before.link.state = LinkState::NeverConnected;
	before.link.connected_at_us.reset();
	before.choices.composite_present = false;
	list.push_back(before);

	// Vesktop の設定を書いた直後 (Vesktop を再起動してもらう場面)。
	Observation written = healthy();
	written.install.vesktop_started_at_us = kNow - 600 * kSec;
	written.install.discord_side_written_at_us = kNow - 300 * kSec;
	written.link.state = LinkState::NeverConnected;
	written.link.connected_at_us.reset();
	list.push_back(written);

	// 同梱の Vencord が上書きされたときと、Vesktop の設定が別の場所を指しているとき。
	Observation overwritten = healthy();
	overwritten.install.dir = VencordDirState::HashMismatch;
	list.push_back(overwritten);
	Observation elsewhere = healthy();
	elsewhere.install.dir = VencordDirState::PointsElsewhere;
	list.push_back(elsewhere);

	// 機能を OFF にしている。
	Observation off = healthy();
	off.choices.feature = FeatureSwitch::Off;
	list.push_back(off);

	// 過去の参加者に、名前を上書きしている行がある。上書きがある人とない人の両方を総当たりに通すために、
	// この状態を入れる (SetNameOverride は、やめる上書きがあるかで押せるかが変わる)。
	Observation with_override = healthy();
	SeenPerson seen;
	seen.user_id = "100";
	seen.display_name = "taro";
	seen.last_seen_at_us = kNow - 60 * kSec;
	seen.name_override = "hanako";
	with_override.seen.people.push_back(seen);
	SeenPerson plain;
	plain.user_id = "200";
	plain.display_name = "jiro";
	plain.last_seen_at_us = kNow - 120 * kSec;
	with_override.seen.people.push_back(plain);
	list.push_back(with_override);

	// アンインストールの 3 つの場合: 押せるとき (配信も録画もしておらず、Vesktop が起動していないと分かって
	// いる)、配信中、終わった後。終わった後は書き込む操作がどれも押せないので、総当たりの
	// 「押せないときは必ず理由が付く」を、その理由でも確認できる。
	Observation ready = healthy();
	ready.install.process = VesktopProcess::NotRunning;
	ready.link.state = LinkState::NeverConnected;
	ready.link.connected_at_us.reset();
	ready.uninstall.obs_streaming = false;
	ready.uninstall.obs_recording = false;
	list.push_back(ready);
	Observation streaming = ready;
	streaming.uninstall.obs_streaming = true;
	list.push_back(streaming);
	Observation removed = ready;
	removed.uninstall.phase = UninstallPhase::Done;
	list.push_back(removed);

	return list;
}

// 総当たりに使う代表的な引数。
std::vector<ActionArgs> representative_args()
{
	std::vector<ActionArgs> list;
	list.push_back(ActionArgs{});

	ActionArgs screen;
	screen.user_id = "100";
	screen.kind = cordmux::config::SlotKind::Screen;
	screen.slot = 1;
	screen.on = true;
	list.push_back(screen);

	ActionArgs voice;
	voice.user_id = "100";
	voice.kind = cordmux::config::SlotKind::Voice;
	voice.slot = 8;
	list.push_back(voice);

	ActionArgs clear;
	clear.user_id = "100";
	clear.slot = 0; // 枠から外す
	list.push_back(clear);

	ActionArgs bad;
	bad.user_id = "100";
	bad.kind = cordmux::config::SlotKind::Screen;
	bad.slot = 9;
	list.push_back(bad);

	// 名前を入れた引数 (SetNameOverride が使う。空の名前は上の 4 つで確認する)。
	ActionArgs named;
	named.user_id = "100";
	named.name = "hanako";
	list.push_back(named);

	// 使う声を入れた引数 (SetVoiceSource が使う。空の使う声 (戻す操作) は上の 5 つで確認する)。
	ActionArgs voice_source;
	voice_source.user_id = "100";
	voice_source.voice_user_id = "200";
	list.push_back(voice_source);

	// 音量を入れた引数 (Set*Volume が使う。範囲の外の音量も入れる)。
	ActionArgs volume;
	volume.user_id = "100";
	volume.volume_pct = 50;
	list.push_back(volume);
	ActionArgs bad_volume;
	bad_volume.user_id = "100";
	bad_volume.volume_pct = 201;
	list.push_back(bad_volume);

	return list;
}

} // namespace

TEST_CASE("押せる条件の表は ActionId を 1 つずつ漏れなく持つ")
{
	CHECK(action_rule_count() == kActionIdCount);

	// 同じ id が 2 行にないこと。
	std::vector<int> seen;
	for (std::size_t i = 0; i < action_rule_count(); ++i) {
		const int value = static_cast<int>(action_rule_id_at(i));
		for (const int other : seen)
			CHECK(other != value);
		seen.push_back(value);
	}
	CHECK(seen.size() == kActionIdCount);
}

TEST_CASE("代表的な状態と引数の総当たりで、Intent が None にならず、押せないときは理由が付く")
{
	const std::vector<Observation> observations = representative_observations();
	const std::vector<ActionArgs> args_list = representative_args();

	std::size_t combinations = 0;
	std::size_t none_kinds = 0;
	std::size_t empty_reasons = 0;

	for (std::size_t i = 0; i < action_rule_count(); ++i) {
		const ActionId id = action_rule_id_at(i);
		for (const Observation &obs : observations) {
			for (const ActionArgs &args : args_list) {
				++combinations;
				const Intent intent = apply(obs, id, args);
				if (intent.kind == IntentKind::None)
					++none_kinds;
				// 押せないときは、必ず理由のキーが付く。
				if (intent.disabled.has_value() &&
				    (intent.disabled->reason_text_id == nullptr ||
				     std::string(intent.disabled->reason_text_id).empty()))
					++empty_reasons;
				// 渡した引数は、そのまま Intent に入る (ドックは Intent だけを見る)。
				if (intent.args != args)
					++empty_reasons;
			}
		}
	}

	MESSAGE("combinations=" << combinations);
	CHECK(combinations == kActionIdCount * observations.size() * args_list.size());
	CHECK(none_kinds == 0);
	CHECK(empty_reasons == 0);
}

TEST_CASE("WriteVencordDir は、Vesktop の設定が既に同梱の Vencord を指していれば押せない")
{
	Observation obs = healthy();
	obs.install.process = VesktopProcess::NotRunning;
	obs.install.dir = VencordDirState::Matches;
	Intent intent = apply(obs, ActionId::WriteVencordDir);
	CHECK(intent.kind == IntentKind::WriteVencordDir);
	REQUIRE(intent.disabled.has_value());
	CHECK(std::string(intent.disabled->reason_text_id) == reason::kDirMatches);

	// まだ読めていないときは押せる (読めないことを理由にインストールを止めない)。
	obs.install.dir = VencordDirState::Unknown;
	CHECK(apply(obs, ActionId::WriteVencordDir).can_run());
	obs.install.dir = VencordDirState::PointsElsewhere;
	CHECK(apply(obs, ActionId::WriteVencordDir).can_run());
	// vencordDir のキーがまだないとき (インストールした日) は、この操作を表示する場面そのものなので、Vesktop が
	// 起動していなければ押せる。
	obs.install.dir = VencordDirState::NotConfigured;
	CHECK(apply(obs, ActionId::WriteVencordDir).can_run());
}

TEST_CASE("[Vesktopに追加] の 2 つの操作は、Vesktop の起動中と、起動しているかを読めていない間は押せず、理由を文で出す")
{
	// 起動中の Vesktop は state.json をメモリの値で書き直すので、起動中に書いた値は消える (actions.cpp の
	// vesktop_busy_reason)。アンインストールの行と同じ形で、起動中は「先にVesktopを終了してください」、読めていない間は
	// 「確認しています」を返す。
	struct Case {
		ActionId id;
		VencordDirState dir;
	};
	const Case cases[] = {
		{ActionId::WriteVencordDir, VencordDirState::NotConfigured},
		{ActionId::WriteVencordDir, VencordDirState::Unknown},
		{ActionId::RepairVencordDir, VencordDirState::PointsElsewhere},
	};
	for (const Case &c : cases) {
		Observation obs = healthy();
		obs.install.dir = c.dir;
		obs.install.process = VesktopProcess::Running;
		const Intent running = apply(obs, c.id);
		REQUIRE(running.disabled.has_value());
		CHECK(std::string(running.disabled->reason_text_id) == text::keys::kDockAddVesktopRunningText);
		obs.install.process = VesktopProcess::Unknown;
		const Intent unknown = apply(obs, c.id);
		REQUIRE(unknown.disabled.has_value());
		CHECK(std::string(unknown.disabled->reason_text_id) == text::keys::kDockVesktopCheckingText);
		obs.install.process = VesktopProcess::NotRunning;
		CHECK(apply(obs, c.id).can_run());
	}
	// 追加済みのときは、Vesktop の状態によらず、押しても何も変わらない理由 (ボタンを置かない) のままにする。
	Observation matches = healthy();
	matches.install.process = VesktopProcess::Running;
	CHECK(std::string(apply(matches, ActionId::WriteVencordDir).disabled->reason_text_id) == reason::kDirMatches);
	CHECK(std::string(apply(matches, ActionId::RepairVencordDir).disabled->reason_text_id) == reason::kDirNotElsewhere);
}

TEST_CASE("RemoveFromVesktop は、同梱の Vencord を指していて、Vesktop が起動していないと分かっているときだけ押せる")
{
	Observation obs = healthy();
	obs.install.process = VesktopProcess::NotRunning;
	CHECK(action_intent_kind(ActionId::RemoveFromVesktop) == IntentKind::RemoveFromVesktop);

	// 同梱の Vencord を指しているとき (ファイルが上書きされていても) は押せる。
	for (const VencordDirState dir : {VencordDirState::Matches, VencordDirState::HashMismatch}) {
		obs.install.dir = dir;
		CHECK(apply(obs, ActionId::RemoveFromVesktop).can_run());
	}
	// 指していないときは、外すものがない (組ごと表示しないので、画面に出さない理由)。
	for (const VencordDirState dir : {VencordDirState::Unknown, VencordDirState::NotConfigured,
					  VencordDirState::PointsElsewhere}) {
		obs.install.dir = dir;
		const Intent intent = apply(obs, ActionId::RemoveFromVesktop);
		REQUIRE(intent.disabled.has_value());
		CHECK(std::string(intent.disabled->reason_text_id) == reason::kNotAdded);
	}

	// Vesktop の起動中と、読めていない間は押せない。状態タブは「Vesktop」の組ごと表示しないので、画面に出さない理由。
	obs.install.dir = VencordDirState::Matches;
	for (const VesktopProcess process : {VesktopProcess::Running, VesktopProcess::Unknown}) {
		obs.install.process = process;
		const Intent intent = apply(obs, ActionId::RemoveFromVesktop);
		REQUIRE(intent.disabled.has_value());
		CHECK(std::string(intent.disabled->reason_text_id) == reason::kVesktopNotStopped);
		// この起動で Vesktop の設定を書かないときも同じ (組が表示されないので、書かない理由も表示しない)。
		obs.install.vesktop_writes_off = true;
		CHECK(std::string(apply(obs, ActionId::RemoveFromVesktop).disabled->reason_text_id) ==
		      reason::kVesktopNotStopped);
		obs.install.vesktop_writes_off = false;
	}

	// この起動で Vesktop の設定を書かないときは、Vesktop を終了しても押せないので、組に表示する理由を返す。
	obs.install.process = VesktopProcess::NotRunning;
	obs.install.vesktop_writes_off = true;
	CHECK(std::string(apply(obs, ActionId::RemoveFromVesktop).disabled->reason_text_id) ==
	      text::keys::kDockVesktopAddFailedWritesOffText);
	obs.ops.settings_unreadable_at_startup = true;
	CHECK(std::string(apply(obs, ActionId::RemoveFromVesktop).disabled->reason_text_id) ==
	      text::keys::kDockVesktopAddFailedSettingsUnreadText);

	// アンインストールを始めた後は押せない (アンインストールが Vesktop の設定を戻す)。
	Observation removed = healthy();
	removed.install.process = VesktopProcess::NotRunning;
	removed.uninstall.phase = UninstallPhase::Stopped;
	CHECK(std::string(apply(removed, ActionId::RemoveFromVesktop).disabled->reason_text_id) ==
	      text::keys::kSettingsUninstallStartedText);
}

TEST_CASE("RepairVencordDir と ReinstallVencordFiles は、それぞれの状態のときだけ押せる")
{
	Observation obs = healthy();
	obs.install.process = VesktopProcess::NotRunning;

	obs.install.dir = VencordDirState::PointsElsewhere;
	CHECK(apply(obs, ActionId::RepairVencordDir).can_run());
	CHECK_FALSE(apply(obs, ActionId::ReinstallVencordFiles).can_run());

	obs.install.dir = VencordDirState::HashMismatch;
	CHECK_FALSE(apply(obs, ActionId::RepairVencordDir).can_run());
	CHECK(apply(obs, ActionId::ReinstallVencordFiles).can_run());

	obs.install.dir = VencordDirState::Matches;
	const Intent repair = apply(obs, ActionId::RepairVencordDir);
	REQUIRE(repair.disabled.has_value());
	CHECK(std::string(repair.disabled->reason_text_id) == reason::kDirNotElsewhere);
	const Intent reinstall = apply(obs, ActionId::ReinstallVencordFiles);
	REQUIRE(reinstall.disabled.has_value());
	CHECK(std::string(reinstall.disabled->reason_text_id) == reason::kFilesMatch);

	// 操作は 2 つでも、RepairVencordDir がさせる仕事は WriteVencordDir と同じである。
	CHECK(action_intent_kind(ActionId::RepairVencordDir) == IntentKind::WriteVencordDir);
	CHECK(action_intent_kind(ActionId::WriteVencordDir) == IntentKind::WriteVencordDir);
}

TEST_CASE("ExportBundle はどの状態でも押せる")
{
	for (const Observation &obs : representative_observations())
		CHECK(apply(obs, ActionId::ExportBundle).can_run());
}

TEST_CASE("SetSlot には、誰に対する操作かと、範囲の中の枠の番号が必要である")
{
	const Observation obs = healthy();

	ActionArgs args;
	const Intent no_user = apply(obs, ActionId::SetSlot, args);
	REQUIRE(no_user.disabled.has_value());
	CHECK(std::string(no_user.disabled->reason_text_id) == reason::kNoUser);

	args.user_id = "100";
	args.kind = cordmux::config::SlotKind::Screen;
	// 空いている枠 (誰も載っていない画面の枠) には載せられる。
	for (int slot = 1; slot <= 4; ++slot) {
		args.slot = slot;
		CAPTURE(slot);
		CHECK(apply(obs, ActionId::SetSlot, args).can_run());
	}
	args.slot = 5;
	const Intent too_big = apply(obs, ActionId::SetSlot, args);
	REQUIRE(too_big.disabled.has_value());
	CHECK(std::string(too_big.disabled->reason_text_id) == reason::kBadSlot);

	// 枠に載っていない人を外しても、何も変わらない。
	args.slot = 0;
	const Intent not_on_slot = apply(obs, ActionId::SetSlot, args);
	REQUIRE(not_on_slot.disabled.has_value());
	CHECK(std::string(not_on_slot.disabled->reason_text_id) == reason::kNotOnSlot);

	// 声の枠には上限がない。この操作は画面の枠だけを見るので、声の枠は番号が 0 以上であることだけが条件である。
	args.kind = cordmux::config::SlotKind::Voice;
	args.slot = 8;
	CHECK(apply(obs, ActionId::SetSlot, args).can_run());
	args.slot = 9;
	CHECK(apply(obs, ActionId::SetSlot, args).can_run());
	args.slot = 30;
	CHECK(apply(obs, ActionId::SetSlot, args).can_run());
	args.slot = -1;
	CHECK_FALSE(apply(obs, ActionId::SetSlot, args).can_run());

	// 「自動で載せる」は、通話に参加していない人にも切り替えられる。
	ActionArgs always;
	CHECK_FALSE(apply(obs, ActionId::SetAlwaysOn, always).can_run());
	always.user_id = "999";
	always.on = true;
	const Intent always_on = apply(obs, ActionId::SetAlwaysOn, always);
	CHECK(always_on.can_run());
	CHECK(always_on.kind == IntentKind::SetAlwaysOn);
	CHECK(always_on.args.on);
}

TEST_CASE("SetSlot で載せられるのは空いている枠だけで、別の人が載っている枠は上書きしない")
{
	// 枠 1 に「100」が載っていて、2 から 4 は空いている。
	Observation obs = healthy();
	PersonFact owner;
	owner.user_id = "100";
	owner.slot = 1;
	obs.people.push_back(owner);
	PersonFact newcomer;
	newcomer.user_id = "200";
	obs.people.push_back(newcomer);

	ActionArgs args;
	args.user_id = "200";
	args.kind = cordmux::config::SlotKind::Screen;

	// 別の人が載っている枠には載せられない (上書きしない)。
	args.slot = 1;
	const Intent taken = apply(obs, ActionId::SetSlot, args);
	REQUIRE(taken.disabled.has_value());
	CHECK(std::string(taken.disabled->reason_text_id) == reason::kSlotTaken);

	// 空いている枠には載せられる。空いている枠の最小の番号は 2 である。
	CHECK(free_screen_slot(obs) == 2);
	args.slot = 2;
	CHECK(apply(obs, ActionId::SetSlot, args).can_run());

	// 既に枠に載っている人を、もう一度載せることはできない (押しても何も変わらない)。
	ActionArgs again;
	again.user_id = "100";
	again.kind = cordmux::config::SlotKind::Screen;
	again.slot = 2;
	const Intent already = apply(obs, ActionId::SetSlot, again);
	REQUIRE(already.disabled.has_value());
	CHECK(std::string(already.disabled->reason_text_id) == reason::kAlreadyOnSlot);

	// その人を枠から外すことはできる。
	again.slot = 0;
	CHECK(apply(obs, ActionId::SetSlot, again).can_run());

	// 4 つの枠がすべて使用中なら、理由は「空いている枠がありません。」になる。
	Observation full = healthy();
	for (int slot = 1; slot <= 4; ++slot) {
		PersonFact person;
		person.user_id = std::to_string(100 + slot);
		person.slot = slot;
		full.people.push_back(person);
	}
	PersonFact waiting;
	waiting.user_id = "999";
	full.people.push_back(waiting);
	CHECK(free_screen_slot(full) == 0);

	ActionArgs no_room;
	no_room.user_id = "999";
	no_room.kind = cordmux::config::SlotKind::Screen;
	no_room.slot = 1;
	const Intent full_intent = apply(full, ActionId::SetSlot, no_room);
	REQUIRE(full_intent.disabled.has_value());
	// テキストは、一覧の上の 1 行と同じ。
	CHECK(std::string(full_intent.disabled->reason_text_id) == text::keys::kDockNoFreeSlotText);
}

TEST_CASE("SetSelfFrame: 今と同じ状態にする向きは押せない。ソースが結び付いていなくても押せる")
{
	Observation obs = healthy();
	obs.choices.self_frame_selected = true;
	ActionArgs on;
	on.on = true;
	ActionArgs off;
	off.on = false;
	const Intent same_off = apply(obs, ActionId::SetSelfFrame, off);
	CHECK(same_off.kind == IntentKind::SetSelfFrame);
	REQUIRE(same_off.disabled.has_value());
	CHECK(std::string(same_off.disabled->reason_text_id) == reason::kSameSelfFrame);
	CHECK(apply(obs, ActionId::SetSelfFrame, on).can_run());
	obs.choices.self_frame_on = true;
	CHECK(apply(obs, ActionId::SetSelfFrame, off).can_run());
	REQUIRE(apply(obs, ActionId::SetSelfFrame, on).disabled.has_value());
	CHECK(action_intent_kind(ActionId::SetSelfFrame) == IntentKind::SetSelfFrame);
}

TEST_CASE("SetSelfFrame: 映す映像を OBS から選んでいなければ、どちらの向きも押せず、理由は映像を選ぶ案内")
{
	Observation obs = healthy();
	ActionArgs on;
	on.on = true;
	ActionArgs off;
	off.on = false;
	for (const bool state : {false, true}) {
		obs.choices.self_frame_on = state;
		for (const ActionArgs &args : {on, off}) {
			const Intent intent = apply(obs, ActionId::SetSelfFrame, args);
			REQUIRE(intent.disabled.has_value());
			CHECK(std::string(intent.disabled->reason_text_id) == text::keys::kDockRowSelfNoVideoTip);
		}
	}
	// 選んでいれば、結び付いていなくても (見つからない間も) 押せる。
	obs.choices.self_frame_selected = true;
	obs.choices.self_frame_bound = false;
	obs.choices.self_frame_on = false;
	CHECK(apply(obs, ActionId::SetSelfFrame, on).can_run());
}

TEST_CASE("SetSelfFrame: 出す向きは Discord の人が枠を 4 つ埋めていると押せず、消す向きは押せる。自分の枠の人は数えない")
{
	Observation obs = healthy();
	obs.choices.self_frame_selected = true;
	for (int slot = 1; slot <= 4; ++slot) {
		PersonFact person;
		person.user_id = "p" + std::to_string(slot);
		person.slot = slot;
		obs.people.push_back(person);
	}
	ActionArgs on;
	on.on = true;
	const Intent full = apply(obs, ActionId::SetSelfFrame, on);
	REQUIRE(full.disabled.has_value());
	CHECK(std::string(full.disabled->reason_text_id) == text::keys::kDockNoFreeSlotText);
	// 枠に載っている 1 人が自分の枠の人なら、Discord の人は 3 人で、出せる。
	obs.people[0].is_self_frame = true;
	CHECK(apply(obs, ActionId::SetSelfFrame, on).can_run());
	// 消す向きは、枠が埋まっていても押せる。
	obs.people[0].is_self_frame = false;
	obs.choices.self_frame_on = true;
	ActionArgs off;
	off.on = false;
	CHECK(apply(obs, ActionId::SetSelfFrame, off).can_run());
}

TEST_CASE("SetSlot: 映像を OBS から選んでいる間、自分の枠の人を枠に載せることは受け付けない。外す向きは通る")
{
	Observation obs = healthy();
	PersonFact self;
	self.user_id = "me";
	self.is_self_frame = true;
	obs.people.push_back(self);
	ActionArgs put;
	put.user_id = "me";
	put.kind = cordmux::config::SlotKind::Screen;
	put.slot = 1;
	// 選んでいない間 (Discord の画面共有) は、ほかの人と同じ。
	CHECK(apply(obs, ActionId::SetSlot, put).can_run());
	obs.choices.self_frame_selected = true;
	const Intent refused = apply(obs, ActionId::SetSlot, put);
	REQUIRE(refused.disabled.has_value());
	CHECK(std::string(refused.disabled->reason_text_id) == reason::kSelfFrameRow);
	// 前から載っていた枠は外せる。
	obs.people.back().slot = 2;
	ActionArgs release = put;
	release.slot = 0;
	CHECK(apply(obs, ActionId::SetSlot, release).can_run());
	// ほかの人は通る。
	ActionArgs other = put;
	other.user_id = "someone";
	CHECK(apply(obs, ActionId::SetSlot, other).can_run());
}

namespace {

// 過去の参加者の一覧の 1 行に、使う声の設定を付ける (この設定は seen_people.json にだけある)。
void mark_voice_source(Observation &obs, const std::string &owner, const std::string &source)
{
	SeenPerson person;
	person.user_id = owner;
	person.last_seen_at_us = kNow - 60 * kSec;
	person.voice_source_user_id = source;
	obs.seen.people.push_back(person);
}

ActionArgs voice_args(const std::string &owner, const std::string &source)
{
	ActionArgs args;
	args.user_id = owner;
	args.voice_user_id = source;
	return args;
}

std::string voice_reason(const Observation &obs, const ActionArgs &args)
{
	const Intent intent = apply(obs, ActionId::SetVoiceSource, args);
	CHECK(intent.kind == IntentKind::SetVoiceSource);
	return intent.disabled.has_value() ? std::string(intent.disabled->reason_text_id) : std::string();
}

} // namespace

TEST_CASE("SetVoiceSource は押して何かが変わるときだけ押せ、枠の番号は見ない")
{
	const Observation obs = healthy();

	// 誰に対する操作かを指していなければ押せない。
	CHECK(voice_reason(obs, voice_args("", "200")) == reason::kNoUser);

	// 使う声の設定がない人に、別の人の声を使う: 押せる。通話に参加しているかは条件にしない (設定は、通話に
	// 参加しているかにかかわらず動作する)。枠の種類と番号は渡さなくてよい (ドックは声の枠の番号を知らない)。
	CHECK(voice_reason(obs, voice_args("100", "200")).empty());

	// 戻そうとした (使う声が空か本人) が、使う声の設定がない: 押しても何も変わらない。
	CHECK(voice_reason(obs, voice_args("100", "")) == reason::kNoVoiceSource);
	CHECK(voice_reason(obs, voice_args("100", "100")) == reason::kNoVoiceSource);

	// 使う声の設定がある人: 戻す向きは押せる。今と同じアカウントは押せない (同じ値を書き戻すだけになる)。
	Observation linked = healthy();
	mark_voice_source(linked, "100", "200");
	CHECK(voice_reason(linked, voice_args("100", "")).empty());
	CHECK(voice_reason(linked, voice_args("100", "100")).empty());
	CHECK(voice_reason(linked, voice_args("100", "200")) == reason::kSameVoiceSource);
	// 別の人に替えるのは押せる (使う声のアカウントは 1 人までで、替えると上書きになる)。
	CHECK(voice_reason(linked, voice_args("100", "300")).empty());

	// 空の設定は、設定がないのと同じに扱う (空の名前の上書きと同じ約束)。
	Observation blank = healthy();
	mark_voice_source(blank, "100", "");
	CHECK(voice_reason(blank, voice_args("100", "")) == reason::kNoVoiceSource);
}

TEST_CASE("SetVoiceSource は使う声を 1 対 1 にし、連鎖させない")
{
	// 300 が既に 200 の声を使っている。100 が 200 の声を使うと、1 人の声を 2 人に使うことになる。
	Observation taken = healthy();
	mark_voice_source(taken, "300", "200");
	// テキストは「「%1」の声は、既に「%2」の行で使っています。」(名前は View を作る側が入れる)。
	CHECK(voice_reason(taken, voice_args("100", "200")) == text::keys::kParticipantsVoiceTakenText);
	// 300 自身が 200 を選び直すのは「今と同じ」で、ほかの人が使っているのではない。
	CHECK(voice_reason(taken, voice_args("300", "200")) == reason::kSameVoiceSource);

	// 200 がほかの人 (300) の声を使っている。100 が 200 の声を使うと、連鎖になる。
	Observation uses_other = healthy();
	mark_voice_source(uses_other, "200", "300");
	// テキストは「「%1」は、ほかの人の声を使っています。」(名前は View を作る側が入れる)。
	CHECK(voice_reason(uses_other, voice_args("100", "200")) == text::keys::kParticipantsVoiceUsesOtherText);

	// 100 の声を 300 が使っている。100 がほかの人 (200) の声を使うと、連鎖になる。
	Observation lent = healthy();
	mark_voice_source(lent, "300", "100");
	CHECK(voice_reason(lent, voice_args("100", "200")) == reason::kVoiceSourceLent);

	// 手で書き換えて衝突した設定 (2 人が同じアカウントの声を使う) でも、戻す向きは押せる (戻せなくならない
	// ようにする)。
	Observation conflict = healthy();
	mark_voice_source(conflict, "100", "200");
	mark_voice_source(conflict, "300", "200");
	CHECK(voice_reason(conflict, voice_args("100", "")).empty());
	CHECK(voice_reason(conflict, voice_args("300", "")).empty());
}

TEST_CASE("SetNameOverride は、誰に対する操作かを指していて、押して何かが変わるときだけ押せる")
{
	Observation obs = healthy();
	SeenPerson seen;
	seen.user_id = "100";
	seen.display_name = "taro";
	seen.last_seen_at_us = kNow - 60 * kSec;
	seen.name_override = "hanako";
	obs.seen.people.push_back(seen);
	SeenPerson plain;
	plain.user_id = "200";
	plain.display_name = "jiro";
	plain.last_seen_at_us = kNow - 60 * kSec;
	obs.seen.people.push_back(plain);

	// 誰も指していない引数では押せない (押しても対象が決まらない)。
	const Intent none = apply(obs, ActionId::SetNameOverride);
	REQUIRE(none.disabled.has_value());
	CHECK(std::string(none.disabled->reason_text_id) == reason::kNoUser);
	CHECK(none.kind == IntentKind::SetNameOverride); // 押せなくても、何をする操作かは変わらない

	// 名前を入れれば、通話に参加していない人にも適用される (過去の参加者の一覧にある人)。
	ActionArgs args;
	args.user_id = "200";
	args.name = "hanako";
	const Intent set = apply(obs, ActionId::SetNameOverride, args);
	CHECK(set.can_run());
	CHECK(set.kind == IntentKind::SetNameOverride);
	CHECK(set.args.name == "hanako");

	// 空の名前は「上書きをやめる」。やめる上書きがある人には適用される。
	ActionArgs clear;
	clear.user_id = "100";
	CHECK(apply(obs, ActionId::SetNameOverride, clear).can_run());

	// やめる上書きがなければ押せない (押しても何も変わらない操作を作らない)。
	clear.user_id = "200";
	const Intent nothing = apply(obs, ActionId::SetNameOverride, clear);
	REQUIRE(nothing.disabled.has_value());
	CHECK(std::string(nothing.disabled->reason_text_id) == reason::kNoOverride);

	// 一覧にない人も同じ (上書きがない人と同じに扱う)。
	clear.user_id = "999";
	CHECK_FALSE(apply(obs, ActionId::SetNameOverride, clear).can_run());

	// 空の上書きは、上書きしていないとして扱う (画面の上書きの表示と同じ条件)。
	Observation blank = healthy();
	SeenPerson empty_override;
	empty_override.user_id = "100";
	empty_override.last_seen_at_us = kNow;
	empty_override.name_override = "";
	blank.seen.people.push_back(empty_override);
	ActionArgs blank_args;
	blank_args.user_id = "100";
	CHECK_FALSE(apply(blank, ActionId::SetNameOverride, blank_args).can_run());

	// 今と同じ名前への上書きは押せない (押しても同じ値を書き戻すだけになる)。空の名前のほうだけを押せなくすると、
	// 同じ理由の片方だけが押せるまま残る。
	ActionArgs same;
	same.user_id = "100";
	same.name = "hanako"; // この人の上書きは今 "hanako"
	const Intent unchanged = apply(obs, ActionId::SetNameOverride, same);
	REQUIRE(unchanged.disabled.has_value());
	CHECK(std::string(unchanged.disabled->reason_text_id) == reason::kSameName);

	// 違う名前なら押せる。上書きのない人に同じ名前を渡すのも押せる (こちらは押すと変わる)。
	ActionArgs other = same;
	other.name = "hanako2";
	CHECK(apply(obs, ActionId::SetNameOverride, other).can_run());
	ActionArgs first_time;
	first_time.user_id = "200";
	first_time.name = "hanako";
	CHECK(apply(obs, ActionId::SetNameOverride, first_time).can_run());

	// 表示名と同じ名前で上書きするのは押せる (上書きしているかは別の事実で、一覧の表示もそうなる)。
	ActionArgs same_as_display;
	same_as_display.user_id = "200";
	same_as_display.name = "jiro";
	CHECK(apply(obs, ActionId::SetNameOverride, same_as_display).can_run());
}

TEST_CASE("ON/OFF の操作は、アンインストールを始めた後を除いて、いつでも押せる")
{
	const ActionId toggles[] = {ActionId::ToggleFeature, ActionId::ToggleRecording, ActionId::ToggleDummy};
	for (const Observation &obs : representative_observations()) {
		for (const ActionId id : toggles) {
			// 例外はアンインストールを始めた後だけである (OBS を終了するまで何も書かないので、押しても何も
			// 残らない)。理由は「アンインストールを開始した後は使用できません。」の 1 つである。
			if (uninstall_started(obs)) {
				const Intent blocked = apply(obs, id, ActionArgs{});
				if (id == ActionId::ToggleRecording) {
					CHECK(blocked.can_run()); // 既定の on (false) は止める向きなので押せる
					continue;
				}
				REQUIRE(blocked.disabled.has_value());
				CHECK(std::string(blocked.disabled->reason_text_id) == text::keys::kSettingsUninstallStartedText);
				continue;
			}
			CHECK(apply(obs, id).can_run());
		}
	}
	CHECK(action_intent_kind(ActionId::ToggleFeature) == IntentKind::SetFeature);
	CHECK(action_intent_kind(ActionId::ToggleRecording) == IntentKind::SetRecording);
	CHECK(action_intent_kind(ActionId::ToggleDummy) == IntentKind::SetDummy);
}

// ---- [作成を再開] ----

TEST_CASE("ResumeFinalize は、録画ファイルの作成が終わっていない録画があるときだけ押せる")
{
	Observation none;
	const Intent blocked = apply(none, ActionId::ResumeFinalize);
	CHECK(blocked.kind == IntentKind::ResumeFinalize);
	REQUIRE(blocked.disabled.has_value());
	CHECK(blocked.disabled->reason_text_id == std::string(reason::kNoPendingFinalize));

	Observation pending;
	pending.recording.pending_present = true;
	pending.recording.pending_session_count = 2;
	const Intent ready = apply(pending, ActionId::ResumeFinalize);
	CHECK(ready.kind == IntentKind::ResumeFinalize);
	CHECK(ready.can_run());

	CHECK(action_intent_kind(ActionId::ResumeFinalize) == IntentKind::ResumeFinalize);
}

// ---- 録画の ON/OFF ----

// 保存先は録画を始める時点で決め直すので、書き込めなかったことでは、始める向きを押せなくしない。書き込め
// なかったことは、RecordDirUnwritable のメッセージが伝える。
TEST_CASE("保存先に書き込めなかった後も、録画を始める操作を押し直せる")
{
	Observation obs = healthy();

	// まだ確認していない間は、どちらの向きも押せる (読めないことを理由に録画を止めない)。
	ActionArgs start;
	start.on = true;
	ActionArgs stop;
	stop.on = false;
	CHECK(apply(obs, ActionId::ToggleRecording, start).can_run());
	CHECK(apply(obs, ActionId::ToggleRecording, stop).can_run());

	// 書き込めると分かっていれば押せる。
	obs.recording.save_dir_writable = true;
	CHECK(apply(obs, ActionId::ToggleRecording, start).can_run());

	// 書き込めなかった後も、始める向きは押せる (押すたびに保存先を決め直す)。
	obs.recording.save_dir_writable = false;
	obs.recording.unwritable_dir = "C:/rec";
	obs.recording.phase = RecordPhase::StartFailed;
	const Intent retry = apply(obs, ActionId::ToggleRecording, start);
	CHECK(retry.kind == IntentKind::SetRecording);
	CHECK(retry.can_run());

	// 開始の処理中と停止の処理中は、どちらの向きも押せない。理由のテキストは、ボタンの処理中のテキストである。
	obs.recording.phase = RecordPhase::Starting;
	const Intent starting = apply(obs, ActionId::ToggleRecording, stop);
	REQUIRE(starting.disabled.has_value());
	CHECK(starting.disabled->reason_text_id == std::string(text::keys::kDockRecordStartingButton));
	obs.recording.phase = RecordPhase::Stopping;
	const Intent stopping = apply(obs, ActionId::ToggleRecording, start);
	REQUIRE(stopping.disabled.has_value());
	CHECK(stopping.disabled->reason_text_id == std::string(text::keys::kDockRecordStoppingButton));
	obs.recording.phase = RecordPhase::StartFailed;

	// 止める向きは押せなくしない。保存先は、録画の途中でも書き込めなくなることがある。
	CHECK(apply(obs, ActionId::ToggleRecording, stop).can_run());

	// 操作と仕事の対応 (画面の操作は 1 つで、させる仕事も 1 つ)。
	CHECK(action_intent_kind(ActionId::ToggleRecording) == IntentKind::SetRecording);
}

// ---- 参加者ごとの声と画面共有の音の、ミュートと音量 ----

TEST_CASE("参加者ごとのミュートは、誰を指しているかだけで押せ、今と同じ値でも押せる")
{
	for (const ActionId id : {ActionId::SetVoiceMute, ActionId::SetScreenAudioMute}) {
		const Observation obs = healthy();
		ActionArgs args;
		args.on = true;
		Intent none = apply(obs, id, args);
		CHECK(none.kind == (id == ActionId::SetVoiceMute ? IntentKind::SetVoiceMute : IntentKind::SetScreenAudioMute));
		REQUIRE(none.disabled.has_value());
		CHECK(std::string(none.disabled->reason_text_id) == reason::kNoUser);

		args.user_id = "100";
		CHECK(apply(obs, id, args).can_run());
		args.on = false;
		CHECK(apply(obs, id, args).can_run());
		// 合成のソースがなくても押せる (値は表に入り、合成のソースを使うようになれば動作する)。
		Observation absent = obs;
		absent.choices.composite_present = false;
		CHECK(apply(absent, id, args).can_run());
	}
}

TEST_CASE("参加者ごとの音量は、範囲 (0 から 200) の中だけで押せ、端の値と今と同じ値も押せる")
{
	for (const ActionId id : {ActionId::SetVoiceVolume, ActionId::SetScreenAudioVolume}) {
		const Observation obs = healthy();
		ActionArgs args;
		args.volume_pct = 50;
		REQUIRE(apply(obs, id, args).disabled.has_value());
		CHECK(std::string(apply(obs, id, args).disabled->reason_text_id) == reason::kNoUser);

		args.user_id = "100";
		for (const int ok : {0, 1, 100, 199, 200}) {
			args.volume_pct = ok;
			CAPTURE(ok);
			CHECK(apply(obs, id, args).can_run());
		}
		for (const int bad : {-1, 201, 1000, -100}) {
			args.volume_pct = bad;
			CAPTURE(bad);
			const Intent intent = apply(obs, id, args);
			REQUIRE(intent.disabled.has_value());
			CHECK(std::string(intent.disabled->reason_text_id) == reason::kBadVolume);
		}
	}
	CHECK(action_intent_kind(ActionId::SetVoiceVolume) == IntentKind::SetVoiceVolume);
	CHECK(action_intent_kind(ActionId::SetScreenAudioVolume) == IntentKind::SetScreenAudioVolume);
}

TEST_CASE("音量の引数の比較は volume_pct も見る")
{
	ActionArgs a;
	ActionArgs b;
	CHECK(a == b);
	b.volume_pct = 30;
	CHECK(a != b);
	CHECK(a.volume_pct == cordmux::config::kPersonVolumeDefaultPct);
}

TEST_CASE("アンインストールを始めた後は、参加者ごとの音量とミュートの操作も押せない")
{
	Observation obs = healthy();
	obs.uninstall.phase = UninstallPhase::Running;
	ActionArgs args;
	args.user_id = "100";
	args.volume_pct = 50;
	for (const ActionId id : {ActionId::SetVoiceMute, ActionId::SetScreenAudioMute, ActionId::SetVoiceVolume,
				  ActionId::SetScreenAudioVolume}) {
		const Intent intent = apply(obs, id, args);
		REQUIRE(intent.disabled.has_value());
		CHECK(std::string(intent.disabled->reason_text_id) == text::keys::kSettingsUninstallStartedText);
	}
}

namespace {

// サブアカウントを使う構成で、メインアカウントが別の通話にいて、サブアカウントが入れる状態。
Observation main_in_other_call()
{
	Observation obs = healthy();
	obs.link.state = LinkState::Connected;
	obs.call.presence = Presence::InCall;
	obs.call.roster_count = 2;
	obs.choices.broadcaster_user_id = "main";
	PersonFact sub;
	sub.user_id = "sub";
	sub.is_receiver = true;
	sub.in_roster = true;
	obs.people.push_back(sub);
	obs.call.main_presence = Presence::InCall;
	obs.call.main_call_joinable = true;
	return obs;
}

const char *reason_of(const Observation &obs, ActionId id, const ActionArgs &args = ActionArgs{})
{
	const Intent intent = apply(obs, id, args);
	return intent.disabled.has_value() ? intent.disabled->reason_text_id : nullptr;
}

} // namespace

TEST_CASE("MoveToMainCall: メインアカウントがいる通話にサブアカウントが入れるときだけ押せる")
{
	Observation obs = main_in_other_call();
	const Intent intent = apply(obs, ActionId::MoveToMainCall);
	CHECK(intent.kind == IntentKind::MoveToMainCall);
	CHECK(intent.can_run());

	Observation changed = obs;
	changed.link.state = LinkState::Disconnected;
	CHECK(std::string(reason_of(changed, ActionId::MoveToMainCall)) == text::keys::kDockAccountNotConnectedTip);

	changed = obs;
	changed.choices.broadcaster_user_id.clear();
	CHECK(std::string(reason_of(changed, ActionId::MoveToMainCall)) == reason::kNoSubAccount);
	changed.choices.broadcaster_user_id = "sub";
	CHECK(std::string(reason_of(changed, ActionId::MoveToMainCall)) == reason::kNoSubAccount);

	// メインアカウントが同じ通話に参加している。
	changed = obs;
	PersonFact main;
	main.user_id = "main";
	main.in_roster = true;
	changed.people.push_back(main);
	CHECK(std::string(reason_of(changed, ActionId::MoveToMainCall)) == text::keys::kDockAccountSameCallTip);

	// 分からない間は押せない (移動先を断定できない)。
	changed = obs;
	changed.call.main_presence = Presence::Unknown;
	CHECK(std::string(reason_of(changed, ActionId::MoveToMainCall)) == text::keys::kDockAccountMainCallUnknownTip);
	changed = obs;
	changed.call.main_call_joinable.reset();
	CHECK(std::string(reason_of(changed, ActionId::MoveToMainCall)) == text::keys::kDockAccountMainCallUnknownTip);

	changed = obs;
	changed.call.main_call_joinable = false;
	CHECK(std::string(reason_of(changed, ActionId::MoveToMainCall)) == text::keys::kDockAccountCannotJoinTip);

	// サブアカウントが通話にいなくても、メインアカウントの通話へ移動できる。
	changed = obs;
	changed.call.presence = Presence::NotInCall;
	CHECK(apply(changed, ActionId::MoveToMainCall).can_run());
}

TEST_CASE("LeaveCall: サブアカウントが通話に参加しているときだけ押せる")
{
	Observation obs = main_in_other_call();
	const Intent intent = apply(obs, ActionId::LeaveCall);
	CHECK(intent.kind == IntentKind::LeaveCall);
	CHECK(intent.can_run());

	Observation changed = obs;
	changed.call.presence = Presence::NotInCall;
	CHECK(std::string(reason_of(changed, ActionId::LeaveCall)) == text::keys::kDockAccountSubNoCallTip);
	changed = obs;
	changed.link.state = LinkState::Disconnected;
	CHECK(std::string(reason_of(changed, ActionId::LeaveCall)) == text::keys::kDockAccountNotConnectedTip);
	changed = obs;
	changed.choices.broadcaster_user_id.clear();
	CHECK(std::string(reason_of(changed, ActionId::LeaveCall)) == reason::kNoSubAccount);
}

TEST_CASE("MoveToMainCall と LeaveCall: 通話を選ぶ関数が見つからないと分かっている間は押せない")
{
	for (const ContactState state : {ContactState::Missing, ContactState::Lost}) {
		Observation obs = main_in_other_call();
		obs.contacts.push_back({"voice-select", state});
		CHECK(std::string(reason_of(obs, ActionId::MoveToMainCall)) == text::keys::kDockAccountCallControlMissingTip);
		CHECK(std::string(reason_of(obs, ActionId::LeaveCall)) == text::keys::kDockAccountCallControlMissingTip);
	}
	// まだ分からない間と見つかっている間は押せる。
	for (const ContactState state : {ContactState::Unknown, ContactState::NotSeen, ContactState::Found}) {
		Observation obs = main_in_other_call();
		obs.contacts.push_back({"voice-select", state});
		CHECK(apply(obs, ActionId::MoveToMainCall).can_run());
		CHECK(apply(obs, ActionId::LeaveCall).can_run());
	}
	// 通話の名前を読む依存箇所が見つからなくても、[退出] は押せる ([移動] は通話が分からないので押せない)。
	Observation obs = main_in_other_call();
	obs.contacts.push_back({"call-info", ContactState::Missing});
	CHECK(apply(obs, ActionId::LeaveCall).can_run());
}

TEST_CASE("SetMainAccount: 今と同じ人と Vesktop のアカウントは選べず、空は「使わない」として選べる")
{
	Observation obs = main_in_other_call();
	ActionArgs args;
	args.user_id = "other";
	const Intent intent = apply(obs, ActionId::SetMainAccount, args);
	CHECK(intent.kind == IntentKind::SetMainAccount);
	CHECK(intent.can_run());
	CHECK(intent.args.user_id == "other");

	args.user_id = "main";
	CHECK(std::string(reason_of(obs, ActionId::SetMainAccount, args)) == reason::kSameMainAccount);
	args.user_id = "sub";
	CHECK(std::string(reason_of(obs, ActionId::SetMainAccount, args)) == reason::kMainIsVesktopAccount);
	args.user_id.clear();
	CHECK(apply(obs, ActionId::SetMainAccount, args).can_run());

	// 接続していなくても選べる。
	obs.link.state = LinkState::Disconnected;
	args.user_id = "other";
	CHECK(apply(obs, ActionId::SetMainAccount, args).can_run());

	// 既に使っていないときの「使わない」は押しても変わらない。
	obs.choices.broadcaster_user_id.clear();
	args.user_id.clear();
	CHECK(std::string(reason_of(obs, ActionId::SetMainAccount, args)) == reason::kSameMainAccount);
}

TEST_CASE("ReplaceSlot: 外す人が枠に載っていて、載せる人が載っていないときだけ押せ、空きは問わない")
{
	Observation obs = healthy();
	for (int slot = 1; slot <= 4; ++slot) {
		PersonFact person;
		person.user_id = "s" + std::to_string(slot);
		person.slot = slot;
		obs.people.push_back(person);
	}
	PersonFact waiting;
	waiting.user_id = "w";
	obs.people.push_back(waiting);

	ActionArgs args;
	args.user_id = "w";
	args.replace_user_id = "s2";
	args.slot = 2;
	const Intent intent = apply(obs, ActionId::ReplaceSlot, args);
	CHECK(intent.kind == IntentKind::ReplaceSlot);
	CHECK(intent.can_run());
	CHECK(intent.args.replace_user_id == "s2");

	ActionArgs bad = args;
	bad.user_id.clear();
	CHECK(std::string(reason_of(obs, ActionId::ReplaceSlot, bad)) == reason::kNoUser);
	bad = args;
	bad.replace_user_id.clear();
	CHECK(std::string(reason_of(obs, ActionId::ReplaceSlot, bad)) == reason::kNoUser);
	bad = args;
	bad.user_id = "s1";
	CHECK(std::string(reason_of(obs, ActionId::ReplaceSlot, bad)) == reason::kAlreadyOnSlot);
	bad = args;
	bad.replace_user_id = "w";
	CHECK(std::string(reason_of(obs, ActionId::ReplaceSlot, bad)) == reason::kNotOnSlot);

	// 映像を OBS から選んでいる間の自分の枠の人の行は、自分の枠の付け外しで動かす。
	Observation self = obs;
	self.choices.self_frame_selected = true;
	self.people.back().is_self_frame = true;
	CHECK(std::string(reason_of(self, ActionId::ReplaceSlot, args)) == reason::kSelfFrameRow);
}

TEST_CASE("ActionArgs の等値比較は、枠から外す人も比べる")
{
	ActionArgs a;
	ActionArgs b;
	CHECK(a == b);
	b.replace_user_id = "x";
	CHECK(a != b);
}

TEST_CASE("上部とチェックの新しい操作は、アンインストールを始めた後に押せない")
{
	Observation obs = main_in_other_call();
	obs.uninstall.phase = UninstallPhase::Stopped;
	for (const ActionId id : {ActionId::MoveToMainCall, ActionId::LeaveCall, ActionId::SetMainAccount, ActionId::ReplaceSlot}) {
		CAPTURE(static_cast<int>(id));
		CHECK(std::string(reason_of(obs, id)) == text::keys::kSettingsUninstallStartedText);
	}
}

TEST_CASE("SetFocus: 合成のソースのタイルに描かれている人だけを全画面にでき、全画面の人を替えるときは戻さずに押せる")
{
	Observation obs = healthy();
	obs.choices.focus_candidates = {"100", "200"};
	ActionArgs a;
	a.user_id = "100";
	const Intent intent = apply(obs, ActionId::SetFocus, a);
	CHECK(intent.kind == IntentKind::SetFocus);
	CHECK(intent.can_run());

	// 描かれていない人 (枠に載っていない人、枠が足りずに描けない人) は押せない。
	ActionArgs other;
	other.user_id = "300";
	CHECK(std::string(reason_of(obs, ActionId::SetFocus, other)) == text::keys::kDockRowFullScreenUnavailableTip);

	// 全画面の人がいても、別の人にはそのまま替えられる。同じ人は押しても何も変わらない。
	obs.choices.focus_user_id = "100";
	ActionArgs b;
	b.user_id = "200";
	CHECK(apply(obs, ActionId::SetFocus, b).can_run());
	CHECK(std::string(reason_of(obs, ActionId::SetFocus, a)) == reason::kSameFocus);
}

TEST_CASE("SetFocus: 元に戻す向き (user_id が空) は全画面のときだけ押せ、cordmux が無効の間は全画面にできない")
{
	Observation obs = healthy();
	obs.choices.focus_candidates = {"100"};
	CHECK(std::string(reason_of(obs, ActionId::SetFocus)) == reason::kNoFocus);
	obs.choices.focus_user_id = "100";
	CHECK(reason_of(obs, ActionId::SetFocus) == nullptr);

	Observation off = healthy();
	off.choices.feature = FeatureSwitch::Off;
	off.choices.focus_candidates = {"100"};
	ActionArgs a;
	a.user_id = "100";
	CHECK(std::string(reason_of(off, ActionId::SetFocus, a)) == text::keys::kDockSummaryDisabledStatus);
}

TEST_CASE("SetFocus: アンインストールを始めた後は全画面にできないが、元に戻すことはできる")
{
	Observation obs = healthy();
	obs.choices.focus_candidates = {"100"};
	obs.choices.focus_user_id = "200";
	obs.uninstall.phase = UninstallPhase::Running;
	ActionArgs a;
	a.user_id = "100";
	CHECK(std::string(reason_of(obs, ActionId::SetFocus, a)) == text::keys::kSettingsUninstallStartedText);
	CHECK(reason_of(obs, ActionId::SetFocus) == nullptr);
}
