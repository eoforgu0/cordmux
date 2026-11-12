// SPDX-License-Identifier: Apache-2.0

// メッセージのレベルの割り当てを、decide_dock() の出力で確認する。kTierCases の 1 行は、規則の名前・条件・メッセージの
// レベル・ログレベルの組である。各行はその条件の状態を作り、その規則の名前のメッセージが、そのメッセージのレベルと
// ログレベルで作られることを確認する。メッセージのレベルが "none" の行は、その規則の名前のメッセージが 1 つも作られない
// ことを確認する (平常の 1 行目、利用者が選んだ状態、人の行の状態だけの場合)。メッセージのレベルの決め方は
// docs/ui_guidelines.md の「メッセージのレベルと表示方法」にある。

#include "doctest.h"

#include "ledger_dock_test_helpers.h"

#include <string>
#include <vector>

using namespace ledger_dock_test;

namespace {

struct TierCase {
	const char *rule;
	const char *case_name;
	const char *tier;
	const char *log_level;
	Observation (*make)();
};

Observation streaming(Observation obs)
{
	obs.obs_outputs.streaming = true;
	return obs;
}

const TierCase kTierCases[] = {
	// ---- primary の表 ----
	// アンインストールの最中は、1 行目を処理中のテキストにする。メッセージのレベル 3 の行は作らない (1 行目と同じ
	// テキストを 2 度表示することになる)。
	{"Uninstalled", "Running", "none", "Info",
	 +[] {
		 Observation o = healthy();
		 o.uninstall.phase = UninstallPhase::Running;
		 return o;
	 }},
	{"Uninstalled", "Done.no_leftovers", "L2", "Warning",
	 +[] {
		 Observation o = healthy();
		 o.uninstall.phase = UninstallPhase::Done;
		 o.uninstall.folder_to_delete = "C:/ProgramData/obs-studio/plugins/cordmux";
		 return o;
	 }},
	{"Uninstalled", "Done.leftovers", "L2", "Warning",
	 +[] {
		 Observation o = healthy();
		 o.uninstall.phase = UninstallPhase::Done;
		 o.uninstall.folder_to_delete = "C:/ProgramData/obs-studio/plugins/cordmux";
		 o.uninstall.leftovers.push_back({UninstallLeftoverKind::CollectionNotOpened, "stream"});
		 return o;
	 }},
	{"Uninstalled", "Stopped", "L2", "Warning",
	 +[] {
		 Observation o = healthy();
		 o.uninstall.phase = UninstallPhase::Stopped;
		 o.uninstall.leftovers.push_back({UninstallLeftoverKind::SettingsFiles, "C:/x"});
		 return o;
	 }},
	{"FeatureOff", "-", "none", "Info",
	 +[] {
		 Observation o = healthy();
		 o.choices.feature = FeatureSwitch::Off;
		 return o;
	 }},
	{"OtherObsHolding", "-", "L2", "Warning",
	 +[] {
		 Observation o = healthy();
		 o.listener.state = ListenerState::BlockedByOtherWriter;
		 return o;
	 }},
	{"ListenPortsExhausted", "-", "L2", "Warning",
	 +[] {
		 Observation o = healthy();
		 o.listener.state = ListenerState::PortsExhausted;
		 o.listener.port_first = 49650;
		 o.listener.port_last = 49665;
		 return o;
	 }},
	{"Connected", "-", "none", "Info", +[] { return healthy(); }},
	// 起動したときにしか起きないので、配信中でもメッセージのレベル 2 にする (配信中で枠に人を載せた状態から作り、条件で
	// メッセージのレベルが分かれないことを確認する)。
	{"SessionFileNotSaved", "-", "L2", "Warning",
	 +[] {
		 Observation o = streaming(not_connected());
		 o.listener.state = ListenerState::Listening;
		 o.listener.session_file_not_saved = true;
		 return o;
	 }},
	{"VencordOverwritten", "streaming_with_slotted_person", "L1", "Error",
	 +[] {
		 Observation o = streaming(not_connected());
		 o.install.dir = VencordDirState::HashMismatch;
		 return o;
	 }},
	{"VencordOverwritten", "otherwise", "L2", "Warning",
	 +[] {
		 Observation o = not_connected();
		 o.install.dir = VencordDirState::HashMismatch;
		 return o;
	 }},
	{"VencordDirElsewhere", "streaming_with_slotted_person", "L1", "Error",
	 +[] {
		 Observation o = streaming(not_connected());
		 o.install.dir = VencordDirState::PointsElsewhere;
		 return o;
	 }},
	{"VencordDirElsewhere", "otherwise", "L2", "Warning",
	 +[] {
		 Observation o = not_connected();
		 o.install.dir = VencordDirState::PointsElsewhere;
		 return o;
	 }},
	{"VencordDirNotConfigured", "-", "L2", "Warning",
	 +[] {
		 Observation o = not_connected();
		 o.install.dir = VencordDirState::NotConfigured;
		 return o;
	 }},
	{"RestartVesktop", "-", "L2", "Warning",
	 +[] {
		 Observation o = not_connected();
		 o.install.vesktop_started_at_us = kNow - 600 * kSec;
		 o.install.discord_side_written_at_us = kNow - 300 * kSec;
		 return o;
	 }},
	{"ConnectedThenDropped", "-", "L1", "Error",
	 +[] {
		 Observation o = not_connected();
		 o.link.state = LinkState::Disconnected;
		 o.link.connected_at_us = kNow - 600 * kSec;
		 o.link.disconnected_at_us = kNow - 30 * kSec;
		 return o;
	 }},
	{"VesktopChecking", "-", "none", "Info",
	 +[] {
		 Observation o = not_connected();
		 o.link.state = LinkState::Unknown;
		 o.install.process = VesktopProcess::Unknown;
		 return o;
	 }},
	{"VesktopNotRunning", "-", "none", "Info",
	 +[] {
		 Observation o = not_connected();
		 o.install.process = VesktopProcess::NotRunning;
		 return o;
	 }},
	{"VesktopRunningNotLinked", "-", "L2", "Warning", +[] { return not_connected(); }},

	// ---- notices の表 ----
	// どの依存箇所でも、配信中にできることがないのでレベル 2 にする。
	{"ContactUnavailable", "any:pc-hook,ssrc-map,sharing,watch-stop", "L2", "Warning",
	 +[] {
		 Observation o = healthy();
		 o.contacts.push_back({"roster", ContactState::Missing});
		 o.contacts.push_back({"ssrc-map", ContactState::Missing});
		 return o;
	 }},
	{"ContactUnavailable", "only:roster,profile,self", "L2", "Warning",
	 +[] {
		 Observation o = healthy();
		 o.contacts.push_back({"profile", ContactState::Missing});
		 o.contacts.push_back({"self", ContactState::Lost});
		 return o;
	 }},
	{"ContactUnavailable", "only:call-info,voice-select", "L2", "Warning",
	 +[] {
		 Observation o = healthy();
		 o.contacts.push_back({"call-info", ContactState::Missing});
		 o.contacts.push_back({"voice-select", ContactState::Lost});
		 return o;
	 }},
	{"CompositeAbsent", "-", "L2", "Warning",
	 +[] {
		 Observation o = healthy();
		 o.choices.composite_present = false;
		 return o;
	 }},
	{"RecordingOnly", "-", "L3", "Info",
	 +[] {
		 Observation o = healthy();
		 o.choices.recording = FeatureSwitch::On;
		 o.recording.phase = RecordPhase::Recording;
		 o.people[1].sharing = true;
		 o.people[1].last_record_frame_at_us = kNow - kSec / 10;
		 return o;
	 }},
	{"InstallMismatch", "-", "L2", "Warning",
	 +[] {
		 Observation o = healthy();
		 o.install.dir = VencordDirState::PointsElsewhere;
		 return o;
	 }},
	{"VersionRestartVesktop", "-", "L2", "Warning",
	 +[] {
		 Observation o = healthy();
		 o.versions.discord_side = "0.9.0+old";
		 return o;
	 }},
	{"VersionMixedBundle", "-", "L2", "Warning",
	 +[] {
		 Observation o = healthy();
		 o.versions.plugin = "1.1.0+new";
		 return o;
	 }},
	{"DoubleAudio", "streaming", "L1", "Error",
	 +[] {
		 Observation o = streaming(healthy());
		 o.choices.discord_audio_source = DiscordAudioSource::Found;
		 o.choices.discord_audio_source_name = "Discord audio";
		 return o;
	 }},
	// 枠に人が載っていなくても、配信中なら視聴者全員に聞こえる。
	{"DoubleAudio", "streaming.no_slot", "L1", "Error",
	 +[] {
		 Observation o = streaming(healthy());
		 o.people[0].slot.reset();
		 o.choices.discord_audio_source = DiscordAudioSource::Found;
		 o.choices.discord_audio_source_name = "Discord audio";
		 return o;
	 }},
	{"DoubleAudio", "otherwise", "L2", "Warning",
	 +[] {
		 Observation o = healthy();
		 o.choices.discord_audio_source = DiscordAudioSource::Found;
		 o.choices.discord_audio_source_name = "Discord audio";
		 return o;
	 }},
	{"BroadcasterNotInCall", "-", "L2", "Warning",
	 +[] {
		 Observation o = healthy();
		 o.choices.broadcaster_user_id = "999";
		 return o;
	 }},
	// 2 人以上が止まり、対象の中に映像が届いている人がいないときだけメッセージにする。
	{"AllVideoStoppedHint", "two_stopped.none_arriving", "L2", "Warning",
	 +[] {
		 Observation o = healthy();
		 o.people[1] = showing("200", "hanako", 2);
		 o.people[0].last_video_frame_at_us = kNow - 5 * kSec;
		 o.people[1].last_video_frame_at_us = kNow - 5 * kSec;
		 return o;
	 }},
	{"AllVideoStoppedHint", "first_frame_missing.two.none_arriving", "L2", "Warning",
	 +[] {
		 Observation o = healthy();
		 o.people[1] = showing("200", "hanako", 2);
		 o.people[0].last_video_frame_at_us.reset();
		 o.people[0].sharing_since_us = kNow - 10 * kSec;
		 o.people[1].last_video_frame_at_us.reset();
		 o.people[1].sharing_since_us = kNow - 10 * kSec;
		 return o;
	 }},
	// 配信中でも、対象が全員止まってもレベル 2 のままにする。行と OBS の映像で分かるためである。
	{"AllVideoStoppedHint", "two_stopped.streaming", "L2", "Warning",
	 +[] {
		 Observation o = streaming(healthy());
		 o.people[1] = showing("200", "hanako", 2);
		 o.people[0].last_video_frame_at_us = kNow - 5 * kSec;
		 o.people[1].last_video_frame_at_us = kNow - 5 * kSec;
		 return o;
	 }},
	// 1 人だけなら行で分かり、OBS の映像でも見える。
	{"AllVideoStoppedHint", "one_stopped", "none", "Info",
	 +[] {
		 Observation o = healthy();
		 o.people[0].last_video_frame_at_us = kNow - 5 * kSec;
		 return o;
	 }},
	{"AllVideoStoppedHint", "first_frame_missing.one", "none", "Info",
	 +[] {
		 Observation o = healthy();
		 o.people[0].last_video_frame_at_us.reset();
		 o.people[0].sharing_since_us = kNow - 10 * kSec;
		 return o;
	 }},
	// 2 人が止まっても、ほかの人の映像が届いていれば、その 2 人の側の原因なのでメッセージにしない。
	{"AllVideoStoppedHint", "two_stopped.other_arriving", "none", "Info",
	 +[] {
		 Observation o = healthy();
		 o.people[1] = showing("200", "hanako", 2);
		 o.people.push_back(showing("300", "kenji", 3));
		 o.people[0].last_video_frame_at_us = kNow - 5 * kSec;
		 o.people[1].last_video_frame_at_us = kNow - 5 * kSec;
		 return o;
	 }},
	{"AllVideoStoppedHint", "other_person", "none", "Info",
	 +[] {
		 Observation o = healthy();
		 // 枠に載っておらず、録画の対象でもない人の映像が届かない: 出力も録画も損なわれない。
		 o.people[1].sharing = true;
		 o.people[1].sharing_since_us = kNow - 30 * kSec;
		 return o;
	 }},
	{"RecordSpaceLow", "-", "L2", "Warning",
	 +[] {
		 Observation o = healthy();
		 o.choices.recording = FeatureSwitch::On;
		 o.recording.phase = RecordPhase::Recording;
		 o.recording.remaining_seconds = 600;
		 return o;
	 }},
	{"RecordFinalizeShort", "-", "L2", "Warning",
	 +[] {
		 Observation o = healthy();
		 o.recording.finalize_need_bytes = 10ULL * 1024 * 1024 * 1024;
		 o.recording.free_space_bytes = 1ULL * 1024 * 1024 * 1024;
		 return o;
	 }},
	{"RecordFinalizeRunning", "-", "L3", "Info",
	 +[] {
		 Observation o = healthy();
		 o.recording.finalize = FinalizeState::Running;
		 o.recording.finalize_files_total = 8;
		 o.recording.finalize_files_done = 3;
		 return o;
	 }},
	{"RecordFinalizePending", "-", "L2", "Warning",
	 +[] {
		 Observation o = healthy();
		 o.recording.pending_present = true;
		 return o;
	 }},
	{"RecordNotReachedSender", "-", "L3", "Info",
	 +[] {
		 Observation o = healthy();
		 o.choices.recording = FeatureSwitch::On;
		 o.recording.phase = RecordPhase::Recording;
		 o.recording.enabled_since_us = kNow - 60 * kSec;
		 o.recording.sender_recording = false;
		 return o;
	 }},
	{"RecordDirUnwritable", "recording_on", "L1", "Error",
	 +[] {
		 Observation o = healthy();
		 o.recording.phase = RecordPhase::StartFailed;
		 o.recording.save_dir_writable = false;
		 o.recording.unwritable_dir = "D:/rec";
		 return o;
	 }},
	{"RecordDirUnwritable", "recording_off", "L2", "Warning",
	 +[] {
		 Observation o = healthy();
		 o.recording.phase = RecordPhase::Idle;
		 o.recording.unwritable_dir = "D:/rec";
		 return o;
	 }},
	{"RecordStoppedSpaceExhausted", "-", "L1", "Error",
	 +[] {
		 Observation o = healthy();
		 o.recording.stopped_by_space_exhausted = true;
		 return o;
	 }},
	// 配信中で、接続しておらず、枠に人が載っている (枠の映像が今損なわれている) ときは、メッセージのレベル 1 にする。
	{"ReinstallFailed", "streaming_not_connected_with_slotted_person", "L1", "Error",
	 +[] {
		 Observation o = streaming(not_connected());
		 o.install.reinstall_failed_files = {"renderer.js"};
		 return o;
	 }},
	{"ReinstallFailed", "otherwise", "L2", "Warning",
	 +[] {
		 Observation o = healthy();
		 o.install.reinstall_failed_files = {"renderer.js"};
		 return o;
	 }},
	// 録画ファイルの作成に失敗した。過去の失敗で、配信の後に対応すればよいので、メッセージのレベル 2 にする。
	{"RecordFinalizeFailed", "-", "L2", "Warning",
	 +[] {
		 Observation o = healthy();
		 o.recording.finalize = FinalizeState::Failed;
		 o.recording.pending_present = true;
		 return o;
	 }},

	// ---- decide_dock() だけが使う規則 (AddedRuleId) ----
	{"SettingsUnreadAtStartup", "-", "L2", "Warning",
	 +[] {
		 Observation o = healthy();
		 o.ops.settings_unreadable_at_startup = true;
		 return o;
	 }},
	{"SettingsUnsaved", "-", "L2", "Warning",
	 +[] {
		 Observation o = healthy();
		 o.ops.settings_unsaved = true;
		 return o;
	 }},
	{"SeenUnreadAtStartup", "-", "L2", "Warning",
	 +[] {
		 Observation o = healthy();
		 o.ops.seen_unreadable_at_startup = true;
		 return o;
	 }},
	{"SeenUnsaved", "-", "L2", "Warning",
	 +[] {
		 Observation o = healthy();
		 o.ops.seen_unsaved = true;
		 return o;
	 }},
	{"VesktopAddFailed", "streaming_not_connected_with_slotted_person", "L1", "Error",
	 +[] {
		 Observation o = streaming(not_connected());
		 o.install.dir = VencordDirState::NotConfigured;
		 o.ops.vesktop_add_failure = VesktopAddFailure::StateWriteFailed;
		 return o;
	 }},
	{"VesktopAddFailed", "otherwise", "L2", "Warning",
	 +[] {
		 Observation o = not_connected();
		 o.install.dir = VencordDirState::NotConfigured;
		 o.ops.vesktop_add_failure = VesktopAddFailure::Refused;
		 return o;
	 }},
	{"RecordGap", "-", "L3", "Info",
	 +[] {
		 Observation o = healthy();
		 o.recording.gaps.push_back({kNow - 600 * kSec, 180 * kSec});
		 return o;
	 }},
	{"CallActionFailed", "-", "L2", "Warning",
	 +[] {
		 Observation o = healthy();
		 o.ops.call_action = CallActionPress{CallActionKind::Leave, kNow - 10 * kSec};
		 return o;
	 }},
	{"SeenDiscarded", "-", "L3", "Info",
	 +[] {
		 Observation o = healthy();
		 o.seen.discarded_with_marks = 2;
		 return o;
	 }},
	// [Vesktopから削除] の失敗は、配信中で枠に人がいても 2 (外せなかったときは押す前のままで、枠の映像は損なわれない)。
	{"VesktopRemoveFailed", "streaming_not_connected_with_slotted_person", "L2", "Warning",
	 +[] {
		 Observation o = streaming(not_connected());
		 o.install.process = VesktopProcess::NotRunning;
		 o.ops.vesktop_remove_failure = VesktopRemoveFailure::StateWriteFailed;
		 return o;
	 }},
	// 今は同梱の Vencord を指していない (外せた後か、ほかの方法で外した) ときは、フラグが残っていても出さない。
	{"VesktopRemoveFailed", "not_added", "none", "-",
	 +[] {
		 Observation o = not_connected();
		 o.install.dir = VencordDirState::NotConfigured;
		 o.ops.vesktop_remove_failure = VesktopRemoveFailure::NotRestorable;
		 return o;
	 }},
};

dockview::Tier tier_of(const std::string &name)
{
	if (name == "L1")
		return dockview::Tier::L1;
	if (name == "L2")
		return dockview::Tier::L2;
	return dockview::Tier::L3;
}

dockview::LogLevel level_of(const std::string &name)
{
	if (name == "Error")
		return dockview::LogLevel::Error;
	if (name == "Warning")
		return dockview::LogLevel::Warning;
	return dockview::LogLevel::Info;
}

} // namespace

TEST_CASE("規則の名前と条件ごとに、決めたメッセージのレベルとログレベルのメッセージが作られる")
{
	for (const TierCase &c : kTierCases) {
		CAPTURE(c.rule);
		CAPTURE(c.case_name);
		const dockview::DockViewV2 view = decide_dock(c.make());
		const std::vector<dockview::Notice> found = notices_of(view, c.rule);
		if (std::string(c.tier) == "none") {
			CHECK(found.empty());
			continue;
		}
		REQUIRE_FALSE(found.empty());
		for (const dockview::Notice &n : found) {
			CHECK(n.tier == tier_of(c.tier));
			CHECK(n.log_level == level_of(c.log_level));
			// ログレベルはメッセージのレベルから決まる。
			CHECK(n.log_level == rule_log_level(n.tier));
		}
	}
}

TEST_CASE("ログレベルは、メッセージのレベル 1 が Error、2 が Warning、3 が Info になる")
{
	CHECK(rule_log_level(dockview::Tier::L1) == dockview::LogLevel::Error);
	CHECK(rule_log_level(dockview::Tier::L2) == dockview::LogLevel::Warning);
	CHECK(rule_log_level(dockview::Tier::L3) == dockview::LogLevel::Info);
}

TEST_CASE("規則の英語の名前は、列挙子の名前と同じ文字列になる")
{
	for (std::size_t i = 0; i < primary_rule_count(); ++i) {
		const std::string name = rule_log_name(primary_rule_id_at(i));
		CHECK_FALSE(name.empty());
		CHECK(name.rfind("Unknown", 0) != 0);
	}
	for (std::size_t i = 0; i < notice_rule_count(); ++i) {
		const std::string name = rule_log_name(notice_rule_id_at(i));
		CHECK_FALSE(name.empty());
		CHECK(name.rfind("Unknown", 0) != 0);
	}
	CHECK(added_rule_count() == 9);
	for (std::size_t i = 0; i < added_rule_count(); ++i) {
		const std::string name = rule_log_name(added_rule_id_at(i));
		CHECK_FALSE(name.empty());
		CHECK(name.rfind("Unknown", 0) != 0);
	}
	// 列挙子の名前を 1 つずつ確認する (列挙子を追加して名前を書き忘れると Unknown になる)。
	CHECK(std::string(rule_log_name(PrimaryRuleId::Uninstalled)) == "Uninstalled");
	CHECK(std::string(rule_log_name(PrimaryRuleId::VesktopRunningNotLinked)) == "VesktopRunningNotLinked");
	CHECK(std::string(rule_log_name(NoticeRuleId::AllVideoStoppedHint)) == "AllVideoStoppedHint");
	CHECK(std::string(rule_log_name(NoticeRuleId::ReinstallFailed)) == "ReinstallFailed");
	CHECK(std::string(rule_log_name(NoticeRuleId::RecordFinalizeFailed)) == "RecordFinalizeFailed");
	CHECK(std::string(rule_log_name(AddedRuleId::SeenDiscarded)) == "SeenDiscarded");
	CHECK(std::string(rule_log_name(AddedRuleId::RecordGap)) == "RecordGap");
	CHECK(std::string(rule_log_name(AddedRuleId::VesktopRemoveFailed)) == "VesktopRemoveFailed");
}
