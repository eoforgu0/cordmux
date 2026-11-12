// SPDX-License-Identifier: Apache-2.0

// インストール・更新・アンインストールにかかわる、押せる条件 (ledger/actions.h) と表の行 (ledger/rules.h) の
// 単体テスト。押せる状態と押せない状態の両方と、行の条件が直れば成り立たなくなることを確認する。表の行数と
// 総当たりは test_ledger_rules.cpp で、テキストと並びは test_ledger_dock_rules.cpp で確認する。

#include "doctest.h"

#include "ledger/actions.h"
#include "ledger/rules.h"
#include "ledger/text_ids.h"

#include <cstddef>
#include <string>
#include <vector>

using namespace cordmux::ledger;

namespace {

constexpr std::int64_t kNow = 1'000'000'000'000;
constexpr std::int64_t kSec = 1'000'000;

// 何も問題がなく、接続している状態 (test_ledger_rules.cpp の healthy() と同じ形)。3 つのバージョンはそろえてある
// (そろっているときだけ、バージョンの行が成り立たない)。
Observation healthy()
{
	Observation obs;
	obs.now_us = kNow;
	obs.choices.feature = FeatureSwitch::On;
	obs.choices.recording = FeatureSwitch::Off;
	obs.choices.dummy = FeatureSwitch::Off;
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
	obs.versions.vesktop = "1.5.3";
	obs.versions.bundled_vencord = "1.0.0";
	obs.uninstall.obs_streaming = false;
	obs.uninstall.obs_recording = false;
	return obs;
}

Observation not_connected()
{
	Observation obs = healthy();
	obs.link.state = LinkState::NeverConnected;
	obs.link.connected_at_us.reset();
	return obs;
}

// アンインストールを押せる状態: 配信も録画もしておらず、Vesktop が起動していないと分かっている。
Observation ready_to_uninstall()
{
	Observation obs = not_connected();
	obs.install.process = VesktopProcess::NotRunning;
	return obs;
}

PersonFact showing_person(const std::string &user_id, int slot)
{
	PersonFact person;
	person.user_id = user_id;
	person.display_name = "taro";
	person.slot = slot;
	person.last_video_frame_at_us = kNow - kSec / 2;
	return person;
}

// ActionId のすべての値 (kActionIdCount と同じ数。押せる条件の表の並び)。
std::vector<ActionId> all_action_ids()
{
	std::vector<ActionId> ids;
	for (std::size_t i = 0; i < action_rule_count(); ++i)
		ids.push_back(action_rule_id_at(i));
	return ids;
}

// 条件が成り立っている notices の行 (表の順)。
std::vector<NoticeRuleId> fired_rules(const Observation &obs)
{
	std::vector<NoticeRuleId> out;
	for (std::size_t i = 0; i < notice_rule_count(); ++i) {
		if (notice_rule_fires(notice_rule_id_at(i), obs))
			out.push_back(notice_rule_id_at(i));
	}
	return out;
}

} // namespace

TEST_CASE("Uninstall を押せない理由は状態ごとに 1 つ選ばれ、状態を読めていない間は「確認しています」になる")
{
	// 押せる状態。
	const Observation ready = ready_to_uninstall();
	const Intent ok = apply(ready, ActionId::Uninstall);
	CHECK(ok.kind == IntentKind::Uninstall);
	CHECK(ok.can_run());
	// リプレイバッファと仮想カメラは、読めていない (nullopt) 間も「確認しています」の条件に数えない。
	CHECK_FALSE(ready.uninstall.obs_replay_buffer.has_value());
	CHECK_FALSE(ready.uninstall.obs_virtual_camera.has_value());
	{
		Observation both_off = ready;
		both_off.uninstall.obs_replay_buffer = false;
		both_off.uninstall.obs_virtual_camera = false;
		CHECK(apply(both_off, ActionId::Uninstall).can_run());
	}

	struct ReasonCase {
		const char *name;
		Observation obs;
		const char *reason;
	};
	std::vector<ReasonCase> cases;
	{
		Observation o = ready;
		o.uninstall.obs_streaming = true;
		cases.push_back({"配信中", o, text::keys::kSettingsUninstallStreamingText});
	}
	{
		Observation o = ready;
		o.uninstall.obs_recording = true;
		cases.push_back({"OBS が録画中", o, text::keys::kSettingsUninstallObsRecordingText});
	}
	// リプレイバッファと仮想カメラ (配信と同じ理由で押せない。アンインストールがシーンコレクションを切り替えて、
	// ソースを消すため)。
	{
		Observation o = ready;
		o.uninstall.obs_replay_buffer = true;
		cases.push_back({"リプレイバッファが動作中", o, text::keys::kSettingsUninstallReplayBufferText});
	}
	{
		Observation o = ready;
		o.uninstall.obs_virtual_camera = true;
		cases.push_back({"仮想カメラが動作中", o, text::keys::kSettingsUninstallVirtualCameraText});
	}
	// 理由の優先順位は、OBS のコントロールのボタンの順 (配信、録画、リプレイバッファ、仮想カメラ) の後に、
	// cordmux の録画と Vesktop。
	{
		Observation o = ready;
		o.uninstall.obs_recording = true;
		o.uninstall.obs_replay_buffer = true;
		o.uninstall.obs_virtual_camera = true;
		cases.push_back({"OBS の録画とリプレイバッファと仮想カメラ", o, text::keys::kSettingsUninstallObsRecordingText});
	}
	{
		Observation o = ready;
		o.uninstall.obs_replay_buffer = true;
		o.uninstall.obs_virtual_camera = true;
		o.choices.recording = FeatureSwitch::On;
		cases.push_back({"リプレイバッファと仮想カメラと cordmux の録画", o, text::keys::kSettingsUninstallReplayBufferText});
	}
	{
		Observation o = ready;
		o.uninstall.obs_virtual_camera = true;
		o.install.process = VesktopProcess::Running;
		cases.push_back({"仮想カメラと Vesktop", o, text::keys::kSettingsUninstallVirtualCameraText});
	}
	{
		Observation o = ready;
		o.choices.recording = FeatureSwitch::On;
		// OBS の録画と cordmux の録画は、文を分ける。
		cases.push_back({"cordmux が録画中", o, text::keys::kSettingsUninstallPerPersonRecordingText});
	}
	{
		Observation o = ready;
		o.install.process = VesktopProcess::Running;
		cases.push_back({"Vesktop が動いている", o, text::keys::kSettingsUninstallVesktopRunningText});
	}
	{
		Observation o = ready;
		o.uninstall.phase = UninstallPhase::Done;
		cases.push_back({"もう外した", o, text::keys::kSettingsUninstallDoneButtonText});
	}
	{
		Observation o = ready;
		o.uninstall.phase = UninstallPhase::Running;
		cases.push_back({"外している最中", o, text::keys::kSettingsUninstallStartedText});
	}
	// 読めていない間は押せず、「配信中です」のように、読めていない事実を断定しない。
	{
		Observation o = ready;
		o.uninstall.obs_streaming.reset();
		cases.push_back({"配信が読めていない", o, text::keys::kSettingsUninstallCheckingText});
	}
	{
		Observation o = ready;
		o.uninstall.obs_recording.reset();
		cases.push_back({"録画が読めていない", o, text::keys::kSettingsUninstallCheckingText});
	}
	{
		Observation o = ready;
		o.install.process = VesktopProcess::Unknown;
		cases.push_back({"Vesktop が読めていない", o, text::keys::kSettingsUninstallCheckingText});
	}
	// 読めた事実に押せない理由があれば、そちらを先に返す (読めていないフィールドがあっても)。
	{
		Observation o = ready;
		o.uninstall.obs_streaming = true;
		o.install.process = VesktopProcess::Unknown;
		cases.push_back({"配信中で Vesktop が読めていない", o, text::keys::kSettingsUninstallStreamingText});
	}
	// 途中で止まったときは押し直せる (残りを片付ける次の操作)。ほかの条件はそのまま動作する。
	{
		Observation o = ready;
		o.uninstall.phase = UninstallPhase::Stopped;
		o.install.process = VesktopProcess::Running;
		cases.push_back({"止まった後に Vesktop が動いている", o, text::keys::kSettingsUninstallVesktopRunningText});
	}

	for (const ReasonCase &c : cases) {
		CAPTURE(c.name);
		const Intent intent = apply(c.obs, ActionId::Uninstall);
		CHECK(intent.kind == IntentKind::Uninstall);
		REQUIRE(intent.disabled.has_value());
		CHECK(intent.disabled->reason_text_id == std::string(c.reason));
		// 画面 (設定ウィンドウの「バージョン情報」の [cordmuxをアンインストール...]) にも、同じ理由が付く
		// (押せないボタンのそばに表示する)。
		const dockview::DockViewV2 view = decide_dock(c.obs);
		REQUIRE(view.about.uninstall_button.disabled_reason.has_value());
		CHECK(view.about.uninstall_button.disabled_reason->id == std::string(c.reason));
	}

	Observation stopped = ready;
	stopped.uninstall.phase = UninstallPhase::Stopped;
	CHECK(apply(stopped, ActionId::Uninstall).can_run());
}

TEST_CASE("アンインストールを始めた後は、書き込む操作をどれも押せない")
{
	// 始める前なら押せる操作 (それぞれの行の条件を満たす引数) を作り、始めた後に押せなくなることを見る。
	Observation before = ready_to_uninstall();
	before.install.dir = VencordDirState::PointsElsewhere;
	before.choices.composite_present = false;
	before.choices.self_frame_selected = true; // 自分の枠のチェックを押せる状態 (押せない理由が映像を選ぶ案内にならないように)
	before.people.push_back(showing_person("100", 1));
	before.recording.pending_present = true;
	SeenPerson seen;
	seen.user_id = "100";
	seen.name_override = "old";
	before.seen.people.push_back(seen);

	const auto args_for = [](ActionId id) {
		ActionArgs args;
		args.user_id = "100";
		switch (id) {
		case ActionId::SetSlot:
			args.slot = 0; // 枠から外す (100 は枠 1 に載っている)
			break;
		case ActionId::SetVoiceSource:
			args.voice_user_id = "200"; // 100 の枠で 200 の声を使う (設定はまだないので付ける向き)
			break;
		case ActionId::SetNameOverride:
			args.name = "new";
			break;
		case ActionId::ToggleRecording:
			args.on = true; // 始める向き
			break;
		case ActionId::ReplaceSlot:
			args.user_id = "200";
			args.replace_user_id = "100";
			break;
		default:
			args.on = true;
			break;
		}
		return args;
	};

	// ReinstallVencordFiles だけは、同梱の Vencord が上書きされているときにしか押せないので、その操作のときだけ
	// Vesktop の設定の状態を変える (ほかの操作は、別の場所を指している状態で押せる)。[移動] と [退出] は、サブアカウントを
	// 使う構成で、サブアカウントが通話に参加していて、メインアカウントの通話に参加できるときに押せる。
	const auto before_for = [&before](ActionId id) {
		Observation obs = before;
		if (id == ActionId::ReinstallVencordFiles)
			obs.install.dir = VencordDirState::HashMismatch;
		// [Vesktopから削除] は、同梱の Vencord を指しているときにしか押せない。
		if (id == ActionId::RemoveFromVesktop)
			obs.install.dir = VencordDirState::Matches;
		if (id == ActionId::MoveToMainCall || id == ActionId::LeaveCall) {
			obs.link.state = LinkState::Connected;
			obs.call.presence = Presence::InCall;
			obs.choices.broadcaster_user_id = "main";
			obs.call.main_presence = Presence::InCall;
			obs.call.main_call_joinable = true;
		}
		// 全画面にする向きは、合成のソースのタイルに描かれている人だけを押せる。
		if (id == ActionId::SetFocus)
			obs.choices.focus_candidates = {"100"};
		return obs;
	};

	Observation after = before;
	after.uninstall.phase = UninstallPhase::Done;

	// 始めた後も押せなくしない操作 (cordmux の残りを増やさない 5 つ。アンインストール自身は自分の行の条件で
	// 決まる)。設定ウィンドウと参加者の設定ウィンドウを開く操作は、結果をもう一度見るために押せなくしない
	// (開くだけでは何も書かない)。映像途切れの累計のリセットは、メモリの値を戻すだけで何も書かない。
	const ActionId still_allowed[] = {ActionId::ExportBundle, ActionId::ResumeFinalize, ActionId::OpenSettings,
					  ActionId::OpenParticipants, ActionId::ResetStallTally};
	std::size_t blocked = 0;
	for (const ActionId id : all_action_ids()) {
		CAPTURE(static_cast<int>(id));
		const ActionArgs args = args_for(id);
		bool allowed = false;
		for (const ActionId keep : still_allowed)
			allowed = allowed || keep == id;
		if (allowed) {
			CHECK(apply(after, id, args).can_run());
			continue;
		}
		if (id == ActionId::Uninstall)
			continue; // 自分の行の条件で決まる (下で個別に見る)
		// 始める前は押せる (押せない理由が「アンインストールを開始した後は使用できません。」
		// だけであることを確認する)。
		const Observation not_pressed = before_for(id);
		CHECK(apply(not_pressed, id, args).can_run());
		Observation pressed = not_pressed;
		pressed.uninstall.phase = UninstallPhase::Done;
		const Intent intent = apply(pressed, id, args);
		REQUIRE(intent.disabled.has_value());
		CHECK(intent.disabled->reason_text_id == std::string(text::keys::kSettingsUninstallStartedText));
		++blocked;
	}
	// 書き込む 20 の操作 (参加者ごとの音量とミュートの 4 つ、自分の枠の付け外し、上部の 3 つ、入れ替え、[Vesktopから削除] を
	// 含む) と、録画を始める向き (ToggleRecording) と、全画面にする向き (SetFocus) で 22 個。
	CHECK(blocked == 22);

	// 録画を止める向きは押せなくしない (録画中の録画を止められなくしない)。
	ActionArgs stop_recording;
	stop_recording.on = false;
	CHECK(apply(after, ActionId::ToggleRecording, stop_recording).can_run());

	// 途中で止まったときも、同じく押せなくする (設定もソースも一部は消えている)。理由の文は、終わったときと同じ
	// 「アンインストールを開始した後は使用できません。」である (どの進み具合でも正しい文で、次にすることは 1 行目と
	// メッセージのレベル 2 のカードが伝える。actions.cpp の blocked_after_uninstall のコメント)。終わったときの文
	// (kSettingsUninstallDoneButtonText) を、止まったときに表示しない。押せなくする操作の数は、終わったときと同じ。
	std::size_t blocked_stopped = 0;
	for (const ActionId id : all_action_ids()) {
		CAPTURE(static_cast<int>(id));
		const ActionArgs args = args_for(id);
		Observation stopped = before_for(id);
		stopped.uninstall.phase = UninstallPhase::Stopped;
		const Intent intent = apply(stopped, id, args);
		if (!intent.disabled.has_value())
			continue;
		if (intent.disabled->reason_text_id == std::string(text::keys::kSettingsUninstallStartedText))
			++blocked_stopped;
		CHECK(intent.disabled->reason_text_id != std::string(text::keys::kSettingsUninstallDoneButtonText));
	}
	CHECK(blocked_stopped == 22); // 終わったときと同じ数 (上の 22)
}

// 機能を OFF にしている間は、録画を始められない (OFF の間は media を false のまま送るので、始めても何も
// 届かない)。止める向きは押せなくしない。保存先に書き込めないときも、押せない理由は機能が OFF であることにする。
TEST_CASE("機能を OFF にしている間は録画を始められないが、止める操作はいつでも押せる")
{
	Observation obs = healthy();
	ActionArgs start;
	start.on = true;
	ActionArgs stop;
	stop.on = false;
	REQUIRE(apply(obs, ActionId::ToggleRecording, start).can_run());

	obs.choices.feature = FeatureSwitch::Off;
	const Intent blocked = apply(obs, ActionId::ToggleRecording, start);
	REQUIRE(blocked.disabled.has_value());
	CHECK(blocked.disabled->reason_text_id == std::string(text::keys::kDockRecordDisabledOffText));
	CHECK(apply(obs, ActionId::ToggleRecording, stop).can_run());

	obs.recording.save_dir_writable = false;
	const Intent both = apply(obs, ActionId::ToggleRecording, start);
	REQUIRE(both.disabled.has_value());
	CHECK(both.disabled->reason_text_id == std::string(text::keys::kDockRecordDisabledOffText));

	// 保存先に書き込めなかったことでは押せなくしない (押すたびに保存先を決め直す)。
	obs.choices.feature = FeatureSwitch::On;
	CHECK(apply(obs, ActionId::ToggleRecording, start).can_run());
}

TEST_CASE("問題が直った後と、アンインストールの後と、起動時の書き直しでは notices の行が成り立たない")
{
	// (i) 直れば成り立たなくなる: 上書き、コピーできなかったファイル、バージョンの 2 行を同時に成り立たせ、
	// 修正した Observation ではどれも成り立たない。
	Observation broken = not_connected();
	broken.install.dir = VencordDirState::HashMismatch;
	broken.install.mismatched_files = {"vencordDesktopMain.js"};
	broken.install.reinstall_failed_files = {"vencordDesktopMain.js"};
	broken.versions.discord_side = "0.9.0";
	broken.versions.bundled_vencord = "0.8.0";
	CHECK(primary_rule_of(broken) == PrimaryRuleId::VencordOverwritten);
	CHECK(fired_rules(broken) == std::vector<NoticeRuleId>{NoticeRuleId::VersionRestartVesktop,
								NoticeRuleId::VersionMixedBundle,
								NoticeRuleId::ReinstallFailed});
	CHECK(primary_rule_of(healthy()) == PrimaryRuleId::Connected);
	CHECK(fired_rules(healthy()).empty());

	// (ii) 接続する前に起きた問題も、接続した後に成り立つ: 接続した Observation では、上書きは notices の行に移る。
	Observation connected = broken;
	connected.link.state = LinkState::Connected;
	connected.link.connected_at_us = kNow - kSec;
	CHECK(fired_rules(connected) == std::vector<NoticeRuleId>{NoticeRuleId::InstallMismatch,
								   NoticeRuleId::VersionRestartVesktop,
								   NoticeRuleId::VersionMixedBundle,
								   NoticeRuleId::ReinstallFailed});

	// (iii) 利用者が自分で始めたアンインストールの結果は Uninstalled の行で、notices の行は成り立たない。
	Observation done = ready_to_uninstall();
	done.uninstall.phase = UninstallPhase::Done;
	CHECK(primary_rule_of(done) == PrimaryRuleId::Uninstalled);
	CHECK(fired_rules(done).empty());
	// 起動時の書き直しは、利用者がすることがないので、notices の行にしない。
	Observation rewrote = healthy();
	rewrote.install.rewrote_at_startup = true;
	CHECK(fired_rules(rewrote).empty());
}
