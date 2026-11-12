// SPDX-License-Identifier: Apache-2.0

// ledger/rules.h の単体テスト。primary と notices の 2 つの表の条件と、値を文字列にする純粋関数を確認する。
// 入力の組み合わせを総当たりして、primary が常にちょうど 1 つで、表を上から見て最初に条件が成り立つ行になる
// ことを確認する (列挙はすべての値、時刻は代表的な値)。あわせて、notices の行の 3 つの性質 (直れば成り立たなく
// なる、接続する前に起きた問題でも接続した後に成り立つ、利用者が選んだ状態では成り立たない) と、表の行数と
// テストの件数が一致することも確認する。テキスト・ボタン・メッセージのレベルは decide_dock() が決めるので、
// test_ledger_dock_rules.cpp と test_ledger_tiers.cpp で確認する。

#include "doctest.h"

#include "ledger/actions.h"
#include "ledger/rules.h"
#include "ledger/text_ids.h"

#include <optional>
#include <string>
#include <vector>

using namespace cordmux::ledger;

namespace {

// テストの「今」(マイクロ秒)。日時としての意味はなく、時間を引いても負にならない大きめの値にしてある。
constexpr std::int64_t kNow = 1'000'000'000'000;
constexpr std::int64_t kSec = 1'000'000;

// 何も問題がなく、接続している状態。各テストは、ここから状態を変えて作る。
Observation healthy()
{
	Observation obs;
	obs.now_us = kNow;
	obs.utc_offset_sec = 0;
	obs.choices.feature = FeatureSwitch::On;
	obs.choices.recording = FeatureSwitch::Off;
	obs.choices.dummy = FeatureSwitch::Off;
	obs.choices.composite_checked = true;
	obs.choices.composite_present = true;
	obs.choices.discord_audio_source = DiscordAudioSource::NotFound;
	// cordmux のソースが配信に出ていて、鳴る (配信中の普通の状態)。
	obs.choices.composite_active = true;
	obs.choices.composite_audible = true;
	obs.listener.state = ListenerState::Listening;
	obs.install.process = VesktopProcess::Running;
	obs.install.dir = VencordDirState::Matches;
	obs.link.state = LinkState::Connected;
	obs.link.connected_at_us = kNow - 60 * kSec;
	obs.call.presence = Presence::InCall;
	obs.call.roster_count = 4;
	// 3 つのバージョン (plugin、bundled_vencord、discord_side) はそろえておく。すべて同じときだけ、バージョンの
	// 行が成り立たない。どれかを変えるテストは、バージョンの 2 行のテストで行う。
	obs.versions.plugin = "1.0.0";
	obs.versions.discord_side = "1.0.0";
	obs.versions.vesktop = "1.5.3";
	obs.versions.bundled_vencord = "1.0.0";
	// アンインストールを押せる条件のうち、配信も録画もしていないと分かっている状態にする。Vesktop は上で
	// Running にしているので、アンインストールそのものは押せない (押せる状態は test_ledger_install_screens.cpp で
	// 作る)。
	obs.uninstall.obs_streaming = false;
	obs.uninstall.obs_recording = false;
	return obs;
}

// 接続していない状態。インストールの行 (VencordOverwritten から RestartVesktop まで) を確認するテストは、
// ここから作る。primary の表では Connected がそれらの行より上にあるので、接続したままでは、それらの行に
// ならない。接続している間のインストールの食い違いは、notices の InstallMismatch のテストで確認する。
// 待ち受けの 2 行 (OtherObsHolding と ListenPortsExhausted) は Connected より上にあるので、この状態を使わない。
Observation not_connected()
{
	Observation obs = healthy();
	obs.link.state = LinkState::NeverConnected;
	obs.link.connected_at_us.reset();
	return obs;
}

PersonFact showing_person(const std::string &user_id, int slot)
{
	PersonFact person;
	person.user_id = user_id;
	person.display_name = "taro";
	person.slot = slot;
	person.last_video_frame_at_us = kNow - kSec / 2;
	person.width = 1280;
	person.height = 720;
	person.fps = 30.0;
	person.stage = kStageNative;
	return person;
}

// primary の表の 1 行ぶんのテストのデータ (行と、primary がその行になる状態を作る関数)。
struct PrimaryCase {
	PrimaryRuleId id;
	Observation (*make)();
};

const PrimaryCase kPrimaryCases[] = {
	// アンインストールを始めた後。FeatureOff の行より先に成り立つことも確認するため、機能を OFF にしておく。
	{PrimaryRuleId::Uninstalled,
	 []() {
		 Observation obs = healthy();
		 obs.choices.feature = FeatureSwitch::Off;
		 obs.uninstall.phase = UninstallPhase::Done;
		 obs.uninstall.folder_to_delete = "D:/obs/plugins/cordmux";
		 obs.uninstall.record_dir = "D:/rec";
		 return obs;
	 }},
	{PrimaryRuleId::FeatureOff,
	 []() {
		 Observation obs = healthy();
		 obs.choices.feature = FeatureSwitch::Off;
		 return obs;
	 }},
	// 待ち受けの 2 行は Connected より上にあるので、接続したままの healthy() から作る (この 2 件が通れば、
	// その順序も確認したことになる)。
	{PrimaryRuleId::OtherObsHolding,
	 []() {
		 Observation obs = healthy();
		 obs.listener.state = ListenerState::BlockedByOtherWriter;
		 obs.listener.other_writer_pid = 4242;
		 obs.listener.other_writer_started_at_us = kNow - 3600 * kSec;
		 return obs;
	 }},
	{PrimaryRuleId::ListenPortsExhausted,
	 []() {
		 Observation obs = healthy();
		 obs.listener.state = ListenerState::PortsExhausted;
		 obs.listener.port_first = 47800;
		 obs.listener.port_last = 47815;
		 return obs;
	 }},
	{PrimaryRuleId::Connected, []() { return healthy(); }},
	// 接続のための情報のファイルを保存できなかった。接続していない間だけ成り立ち、Vesktop の側の行
	// (VencordOverwritten から VesktopRunningNotLinked まで) より上にある。Vesktop が起動していて接続してこない
	// 状態 (VesktopRunningNotLinked になる状態) から作る。
	{PrimaryRuleId::SessionFileNotSaved,
	 []() {
		 Observation obs = not_connected();
		 obs.install.process = VesktopProcess::Running;
		 obs.listener.state = ListenerState::Listening;
		 obs.listener.session_file_not_saved = true;
		 return obs;
	 }},
	{PrimaryRuleId::VencordOverwritten,
	 []() {
		 Observation obs = not_connected();
		 obs.install.dir = VencordDirState::HashMismatch;
		 obs.install.mismatched_files = {"vencordDesktopRenderer.js"};
		 return obs;
	 }},
	{PrimaryRuleId::VencordDirElsewhere,
	 []() {
		 Observation obs = not_connected();
		 obs.install.dir = VencordDirState::PointsElsewhere;
		 return obs;
	 }},
	{PrimaryRuleId::VencordDirNotConfigured,
	 []() {
		 Observation obs = not_connected();
		 obs.install.dir = VencordDirState::NotConfigured;
		 return obs;
	 }},
	{PrimaryRuleId::RestartVesktop,
	 []() {
		 Observation obs = not_connected();
		 obs.install.vesktop_started_at_us = kNow - 600 * kSec;
		 obs.install.discord_side_written_at_us = kNow - 300 * kSec;
		 return obs;
	 }},
	{PrimaryRuleId::ConnectedThenDropped,
	 []() {
		 Observation obs = healthy();
		 obs.link.state = LinkState::Disconnected;
		 obs.link.disconnected_at_us = kNow - 30 * kSec;
		 obs.link.reason = DisconnectReason::SecretMismatch;
		 return obs;
	 }},
	{PrimaryRuleId::VesktopChecking,
	 []() {
		 Observation obs = healthy();
		 obs.install.process = VesktopProcess::Unknown;
		 obs.link.state = LinkState::Unknown;
		 obs.link.connected_at_us.reset();
		 return obs;
	 }},
	{PrimaryRuleId::VesktopNotRunning,
	 []() {
		 Observation obs = healthy();
		 obs.install.process = VesktopProcess::NotRunning;
		 obs.link.state = LinkState::NeverConnected;
		 obs.link.connected_at_us.reset();
		 return obs;
	 }},
	{PrimaryRuleId::VesktopRunningNotLinked,
	 []() {
		 Observation obs = healthy();
		 obs.link.state = LinkState::NeverConnected;
		 obs.link.connected_at_us.reset();
		 return obs;
	 }},
};

constexpr std::size_t kPrimaryCaseCount = sizeof(kPrimaryCases) / sizeof(kPrimaryCases[0]);

// notices の表の 1 行ぶんのテストのデータ (行と、その行の条件が成り立つ状態を作る関数)。
struct NoticeCase {
	NoticeRuleId id;
	Observation (*make)();
};

const NoticeCase kNoticeCases[] = {
	{NoticeRuleId::ContactUnavailable,
	 []() {
		 Observation obs = healthy();
		 obs.contacts.push_back({"watch-stop", ContactState::Missing});
		 obs.contacts.push_back({"roster", ContactState::Found});
		 return obs;
	 }},
	{NoticeRuleId::CompositeAbsent,
	 []() {
		 Observation obs = healthy();
		 obs.choices.composite_present = false;
		 return obs;
	 }},
	{NoticeRuleId::RecordingOnly,
	 []() {
		 Observation obs = healthy();
		 obs.choices.recording = FeatureSwitch::On;
		 PersonFact person = showing_person("100", 1);
		 person.slot.reset();
		 // この行が見るのは、録画の入力に届いたフレーム (last_record_frame_at_us) である。枠に載っていない
		 // 人のストリームには枠への経路がないので、last_video_frame_at_us は書かれない。
		 person.last_video_frame_at_us.reset();
		 person.last_record_frame_at_us = kNow - kSec / 2;
		 obs.people.push_back(person);
		 return obs;
	 }},
	{NoticeRuleId::InstallMismatch,
	 []() {
		 // 接続したまま、同梱の Vencord のファイルが上書きされた状態。primary は Connected のままで、
		 // インストールの食い違いは notices の行になる。
		 Observation obs = healthy();
		 obs.install.dir = VencordDirState::HashMismatch;
		 obs.install.mismatched_files = {"vencordDesktopRenderer.js"};
		 return obs;
	 }},
	// バージョンの 2 行。それぞれ、その行だけが成り立つ状態で作る。
	{NoticeRuleId::VersionRestartVesktop,
	 []() {
		 Observation obs = healthy();
		 obs.versions.discord_side = "0.9.0"; // bundled_vencord (1.0.0) と違う
		 return obs;
	 }},
	{NoticeRuleId::VersionMixedBundle,
	 []() {
		 Observation obs = healthy();
		 obs.versions.bundled_vencord = "0.9.0"; // plugin (1.0.0) と bundled_vencord が違う
		 obs.versions.discord_side = "0.9.0";    // bundled_vencord と同じ (もう 1 行は成り立たない)
		 return obs;
	 }},
	{NoticeRuleId::DoubleAudio,
	 []() {
		 Observation obs = healthy();
		 obs.choices.discord_audio_source = DiscordAudioSource::Found;
		 return obs;
	 }},
	{NoticeRuleId::BroadcasterNotInCall,
	 []() {
		 Observation obs = healthy();
		 obs.choices.broadcaster_user_id = "999";
		 obs.people.push_back(showing_person("100", 1));
		 return obs;
	 }},
	{NoticeRuleId::AllVideoStoppedHint,
	 []() {
		 Observation obs = healthy();
		 PersonFact person = showing_person("100", 1);
		 person.sharing = true;
		 person.last_video_frame_at_us = kNow - 5 * kSec;
		 obs.people.push_back(person);
		 person.user_id = "101";
		 person.slot = 2;
		 obs.people.push_back(person);
		 return obs;
	 }},
	{NoticeRuleId::RecordSpaceLow,
	 []() {
		 Observation obs = healthy();
		 obs.choices.recording = FeatureSwitch::On;
		 obs.recording.enabled_since_us = kNow - 60 * kSec;
		 obs.recording.sender_recording = true; // 送信側に届いている (別の録画の行を成り立たせない)
		 obs.recording.remaining_seconds = 12 * 60;
		 return obs;
	 }},
	{NoticeRuleId::RecordFinalizeShort,
	 []() {
		 Observation obs = healthy();
		 obs.recording.free_space_bytes = 1000ULL * 1000ULL * 1000ULL;
		 obs.recording.finalize_need_bytes = 4ULL * 1000ULL * 1000ULL * 1000ULL;
		 return obs;
	 }},
	{NoticeRuleId::RecordFinalizeRunning,
	 []() {
		 Observation obs = healthy();
		 obs.recording.finalize = FinalizeState::Running;
		 obs.recording.finalize_files_total = 6;
		 obs.recording.finalize_files_done = 2;
		 return obs;
	 }},
	{NoticeRuleId::RecordFinalizePending,
	 []() {
		 Observation obs = healthy();
		 obs.recording.pending_present = true;
		 obs.recording.pending_session_count = 1;
		 return obs;
	 }},
	{NoticeRuleId::RecordNotReachedSender,
	 []() {
		 Observation obs = healthy();
		 obs.choices.recording = FeatureSwitch::On;
		 // 待ち時間を過ぎている (録画を ON にした直後は成り立たない)。
		 obs.recording.enabled_since_us = kNow - 60 * kSec;
		 obs.recording.sender_recording = false;
		 return obs;
	 }},
	{NoticeRuleId::RecordDirUnwritable,
	 []() {
		 Observation obs = healthy();
		 obs.recording.save_dir_writable = false;
		 return obs;
	 }},
	{NoticeRuleId::RecordStoppedSpaceExhausted,
	 []() {
		 Observation obs = healthy();
		 // 止まった後なので、録画は OFF に戻っている (それでも成り立つ)。
		 obs.choices.recording = FeatureSwitch::Off;
		 obs.recording.stopped_by_space_exhausted = true;
		 return obs;
	 }},
	{NoticeRuleId::ReinstallFailed,
	 []() {
		 Observation obs = healthy();
		 obs.install.reinstall_failed_files = {"vencordDesktopMain.js", "cordmux-build.txt"};
		 return obs;
	 }},
	{NoticeRuleId::RecordFinalizeFailed,
	 []() {
		 Observation obs = healthy();
		 obs.recording.finalize = FinalizeState::Failed;
		 return obs;
	 }},
};

constexpr std::size_t kNoticeCaseCount = sizeof(kNoticeCases) / sizeof(kNoticeCases[0]);

// テストが失敗したときにどの組み合わせか分かるように、状態を数字の並びで書き出す。
std::string describe(const Observation &obs)
{
	std::string s;
	s += "process=" + std::to_string(static_cast<int>(obs.install.process));
	s += " dir=" + std::to_string(static_cast<int>(obs.install.dir));
	s += " listener=" + std::to_string(static_cast<int>(obs.listener.state));
	s += " link=" + std::to_string(static_cast<int>(obs.link.state));
	s += " reason=" + std::to_string(static_cast<int>(obs.link.reason));
	s += " feature=" + std::to_string(static_cast<int>(obs.choices.feature));
	s += " written=" + std::to_string(obs.install.discord_side_written_at_us.value_or(-1));
	s += " started=" + std::to_string(obs.install.vesktop_started_at_us.value_or(-1));
	s += " connected=" + std::to_string(obs.link.connected_at_us.value_or(-1));
	s += " session_not_saved=" + std::to_string(obs.listener.session_file_not_saved ? 1 : 0);
	return s;
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

TEST_CASE("時刻の整形は、時計も OS の時差の設定も使わず、渡した時刻と時差だけで決まる")
{
	CHECK(format_clock_hms(0, 0) == "00:00:00");
	CHECK(format_clock_hms((10 * 3600 + 32 * 60 + 4) * kSec, 0) == "10:32:04");
	// 時差 (+9 時間) で日をまたぐと、0 時から数え直す。
	CHECK(format_clock_hms(20 * 3600 * kSec, 9 * 3600) == "05:00:00");
	// 負の時差。
	CHECK(format_clock_hms(2 * 3600 * kSec, -5 * 3600) == "21:00:00");
	// エポックより前の時刻でも切り下げる (1 秒引く方向)。
	CHECK(format_clock_hms(-1, 0) == "23:59:59");
}

TEST_CASE("fps の整形は、ほぼ整数なら整数にし、そうでなければ小数 1 桁にする")
{
	CHECK(format_fps(30.0) == "30");
	CHECK(format_fps(29.97) == "30");
	CHECK(format_fps(15.5) == "15.5");
	CHECK(format_fps(0.0) == "-");
	CHECK(format_fps(-1.0) == "-");
	// 0 に丸まる正の値を「0」と書かない (0 fps は、フレームが 1 つも届いていないと読めて、分からないことを
	// 断定することになる)。
	CHECK(format_fps(0.02) == "-");
	CHECK(format_fps(0.049) == "-");
	// 0.05 以上は丸めた値を返す (境界のすぐ上は 0.1 になる)。
	CHECK(format_fps(0.06) == "0.1");
}

TEST_CASE("日時の整形は、時計も OS の時差の設定も使わず、うるう年と時差と負の時刻を正しく扱う")
{
	const DateTimeParts epoch = format_date_time_parts(0, 0);
	CHECK(epoch.year == "1970");
	CHECK(epoch.month == "01");
	CHECK(epoch.day == "01");
	CHECK(epoch.hour == "00");
	CHECK(epoch.minute == "00");

	// 2003-09-16 14:05 UTC。
	const DateTimeParts plain = format_date_time_parts(1063721100 * kSec, 0);
	CHECK(plain.year == "2003");
	CHECK(plain.month == "09");
	CHECK(plain.day == "16");
	CHECK(plain.hour == "14");
	CHECK(plain.minute == "05");

	// うるう年の 2 月 29 日 (4 年ごと) と、400 年ごとの例外の側 (2000 年)。
	const DateTimeParts leap = format_date_time_parts(1709251140 * kSec, 0);
	CHECK(leap.year == "2024");
	CHECK(leap.month == "02");
	CHECK(leap.day == "29");
	CHECK(leap.hour == "23");
	CHECK(leap.minute == "59");
	const DateTimeParts leap400 = format_date_time_parts(951825600 * kSec, 0);
	CHECK(leap400.year == "2000");
	CHECK(leap400.month == "02");
	CHECK(leap400.day == "29");

	// 100 年ごとの例外 (1900 年はうるう年ではない): 1900-03-01 00:00 の 1 秒前は 2 月 28 日。
	const DateTimeParts before_1900_march = format_date_time_parts(-2203891200 * kSec - 1, 0);
	CHECK(before_1900_march.year == "1900");
	CHECK(before_1900_march.month == "02");
	CHECK(before_1900_march.day == "28");
	CHECK(before_1900_march.hour == "23");
	CHECK(before_1900_march.minute == "59");

	// エポックより前でも切り下げる (1969-12-31 23:59:59)。
	const DateTimeParts negative = format_date_time_parts(-1, 0);
	CHECK(negative.year == "1969");
	CHECK(negative.month == "12");
	CHECK(negative.day == "31");
	CHECK(negative.hour == "23");
	CHECK(negative.minute == "59");

	// 時差で日をまたぐ (負の側)。
	const DateTimeParts back_a_day = format_date_time_parts(0, -5 * 3600);
	CHECK(back_a_day.year == "1969");
	CHECK(back_a_day.month == "12");
	CHECK(back_a_day.day == "31");
	CHECK(back_a_day.hour == "19");

	// 同じ時刻なら、時と分は format_clock_hms() の結果と一致する (2 つの関数で、秒の単位への切り下げ方と時差の
	// 扱いが同じ)。
	CHECK(format_clock_hms(1063721100 * kSec, 0) == "14:05:00");
}


TEST_CASE("primary の表の行数と、行ごとのテストの件数が一致する")
{
	CHECK(primary_rule_count() == kPrimaryCaseCount);

	// 表の並びとテストの並びが同じであることも確認する。
	for (std::size_t i = 0; i < primary_rule_count(); ++i)
		CHECK(primary_rule_id_at(i) == kPrimaryCases[i].id);
}

TEST_CASE("行ごとに作った状態で、primary がその行になる")
{
	for (std::size_t i = 0; i < kPrimaryCaseCount; ++i) {
		const Observation obs = kPrimaryCases[i].make();
		CAPTURE(i);
		CHECK(primary_rule_of(obs) == kPrimaryCases[i].id);
		CHECK(primary_rule_matches(kPrimaryCases[i].id, obs));
	}
}

TEST_CASE("インストールした日に vencordDir のキーがないときは VencordDirNotConfigured の行になり、次の操作を押せる")
{
	// state.json は読めたが、vencordDir のキーがまだない。Vesktop は起動しているので、この行がないと
	// VesktopRunningNotLinked (「Vesktopは起動していますが、接続されていません」) になり、次の操作が
	// 画面に表示されない。
	Observation obs = not_connected();
	obs.install.dir = VencordDirState::NotConfigured;
	obs.install.process = VesktopProcess::Running;
	CHECK(primary_rule_of(obs) == PrimaryRuleId::VencordDirNotConfigured);
	// Vesktop の起動中は押せず、押せない理由が次の操作 (Vesktop の終了) を伝える。終了すれば押せる (この行のまま)。
	const Intent running = apply(obs, ActionId::WriteVencordDir);
	REQUIRE(running.disabled.has_value());
	CHECK(running.disabled->reason_text_id == std::string(text::keys::kDockAddVesktopRunningText));
	obs.install.process = VesktopProcess::NotRunning;
	CHECK(primary_rule_of(obs) == PrimaryRuleId::VencordDirNotConfigured);
	CHECK(apply(obs, ActionId::WriteVencordDir).can_run());
}

TEST_CASE("Vesktop の設定を修正する必要がある 3 つの状態では、それぞれを修正する操作を押せる")
{
	struct DirCase {
		VencordDirState dir;
		PrimaryRuleId row;
		ActionId action;
	};
	const DirCase cases[] = {
		{VencordDirState::NotConfigured, PrimaryRuleId::VencordDirNotConfigured, ActionId::WriteVencordDir},
		{VencordDirState::PointsElsewhere, PrimaryRuleId::VencordDirElsewhere, ActionId::RepairVencordDir},
		{VencordDirState::HashMismatch, PrimaryRuleId::VencordOverwritten, ActionId::ReinstallVencordFiles},
	};
	for (const DirCase &c : cases) {
		Observation obs = not_connected();
		obs.install.dir = c.dir;
		// Vesktop の設定を書く 2 つ ([Vesktopに追加]) は、Vesktop を終了した後に押す。[再インストール] は同梱の
		// Vencord のファイルをコピーするだけなので、Vesktop の起動中も押せる。
		obs.install.process = c.action == ActionId::ReinstallVencordFiles ? VesktopProcess::Running
										     : VesktopProcess::NotRunning;
		CAPTURE(static_cast<int>(c.dir));
		CHECK(primary_rule_of(obs) == c.row);
		CHECK(apply(obs, c.action).can_run());
	}
}

TEST_CASE("接続している間の primary は Connected で、インストールの食い違いは notices の行が成り立つ")
{
	// primary の表では Connected がインストールの行より上にあるので、接続している間のインストールの食い違いは
	// InstallMismatch になる。
	const VencordDirState dirs[] = {VencordDirState::HashMismatch, VencordDirState::PointsElsewhere,
					VencordDirState::NotConfigured};
	for (const VencordDirState dir : dirs) {
		Observation obs = healthy();
		obs.install.dir = dir;
		CAPTURE(static_cast<int>(dir));
		CHECK(primary_rule_of(obs) == PrimaryRuleId::Connected);
		CHECK(notice_rule_fires(NoticeRuleId::InstallMismatch, obs));

		// 接続していなければ、同じ食い違いは primary の行になり、notices の行は成り立たない (同じことを
		// 2 度伝えない)。
		Observation offline = obs;
		offline.link.state = LinkState::NeverConnected;
		offline.link.connected_at_us.reset();
		CHECK(primary_rule_of(offline) != PrimaryRuleId::Connected);
		CHECK_FALSE(notice_rule_fires(NoticeRuleId::InstallMismatch, offline));

		// 機能を OFF にしている間も成り立たない (利用者が止めているものに、次の操作を勧めない)。
		Observation feature_off = obs;
		feature_off.choices.feature = FeatureSwitch::Off;
		CHECK(primary_rule_of(feature_off) == PrimaryRuleId::FeatureOff);
		CHECK(fired_rules(feature_off).empty());
	}

	// RestartVesktop だけは、接続している間は notices の行にもならない。既に接続している画面で、接続するための
	// Vesktop の再起動を勧めると、事実と違うことを伝えることになる。接続していなければ primary の行になる。
	Observation restart = healthy();
	restart.install.vesktop_started_at_us = kNow - 600 * kSec;
	restart.install.discord_side_written_at_us = kNow - 300 * kSec;
	CHECK(primary_rule_of(restart) == PrimaryRuleId::Connected);
	CHECK(fired_rules(restart).empty());
	restart.link.state = LinkState::NeverConnected;
	restart.link.connected_at_us.reset();
	CHECK(primary_rule_of(restart) == PrimaryRuleId::RestartVesktop);
}

TEST_CASE("総当たりで、primary は常にちょうど 1 つで、表を上から見て最初に条件が成り立つ行になる")
{
	const VesktopProcess processes[] = {VesktopProcess::Unknown, VesktopProcess::NotRunning,
					    VesktopProcess::Running};
	const VencordDirState dirs[] = {VencordDirState::Unknown, VencordDirState::NotConfigured,
					VencordDirState::Matches, VencordDirState::HashMismatch,
					VencordDirState::PointsElsewhere};
	const ListenerState listeners[] = {ListenerState::Unknown, ListenerState::Listening,
					   ListenerState::BlockedByOtherWriter, ListenerState::PortsExhausted,
					   ListenerState::Closed};
	const LinkState links[] = {LinkState::Unknown, LinkState::NeverConnected, LinkState::Connected,
				   LinkState::Disconnected};
	const DisconnectReason reasons[] = {DisconnectReason::Unknown,          DisconnectReason::SecretMismatch,
					    DisconnectReason::WireMismatch,     DisconnectReason::MalformedHello,
					    DisconnectReason::HelloTimeout,     DisconnectReason::HeartbeatTimeout,
					    DisconnectReason::PeerClosed};
	const FeatureSwitch features[] = {FeatureSwitch::Unknown, FeatureSwitch::Off, FeatureSwitch::On};

	// 時刻の代表的な値 5 通り: 両方ない / 書いた時刻だけ / 起動した時刻だけ / 書いた < 起動 / 書いた > 起動。
	// 書いた時刻は cordmux が Vesktop の側を最後に書いた時刻 (discord_side_written_at_us)、起動した時刻は
	// Vesktop を起動した時刻である。
	// RestartVesktop の条件 (observation.h の restart_needed()) は、この 5 通りで分かれる。
	struct TimePair {
		bool has_written;
		bool has_started;
		std::int64_t written;
		std::int64_t started;
	};
	const TimePair time_pairs[] = {
		{false, false, 0, 0},
		{true, false, kNow - 100 * kSec, 0},
		{false, true, 0, kNow - 100 * kSec},
		{true, true, kNow - 200 * kSec, kNow - 100 * kSec},
		{true, true, kNow - 100 * kSec, kNow - 200 * kSec},
	};

	std::size_t combinations = 0;
	std::size_t failures = 0;
	std::string first_failure;

	for (const VesktopProcess process : processes) {
		for (const VencordDirState dir : dirs) {
			for (const ListenerState listener : listeners) {
				for (const LinkState link : links) {
					for (const DisconnectReason reason : reasons) {
						for (const FeatureSwitch feature : features) {
							for (const TimePair &times : time_pairs) {
								// 下の 1 ビットは接続した時刻の有無、上の 1 ビットは
								// 接続のための情報のファイルを保存できなかったか。
								for (int variant = 0; variant < 4; ++variant) {
									const int connected = variant & 1;
									Observation obs = healthy();
									obs.install.process = process;
									obs.install.dir = dir;
									obs.listener.state = listener;
									obs.link.state = link;
									obs.link.reason = reason;
									obs.link.disconnected_at_us = kNow - 5 * kSec;
									obs.choices.feature = feature;
									obs.listener.session_file_not_saved = (variant & 2) != 0;
									obs.install.discord_side_written_at_us.reset();
									obs.install.vesktop_started_at_us.reset();
									if (times.has_written)
										obs.install.discord_side_written_at_us =
											times.written;
									if (times.has_started)
										obs.install.vesktop_started_at_us =
											times.started;
									obs.link.connected_at_us.reset();
									if (connected == 1)
										obs.link.connected_at_us =
											kNow - 60 * kSec;

									++combinations;

									// primary になった行の条件が成り立ち、
									// それより上の行の条件は、どれも成り立たない。
									const PrimaryRuleId id = primary_rule_of(obs);
									bool ok = primary_rule_matches(id, obs);
									for (std::size_t i = 0;
									     ok && i < primary_rule_count(); ++i) {
										const PrimaryRuleId earlier =
											primary_rule_id_at(i);
										if (earlier == id)
											break;
										if (primary_rule_matches(earlier, obs))
											ok = false;
									}
									if (!ok) {
										++failures;
										if (first_failure.empty())
											first_failure = describe(obs);
									}
								}
							}
						}
					}
				}
			}
		}
	}

	MESSAGE("combinations=" << combinations << " first_failure=" << first_failure);
	CHECK(combinations == static_cast<std::size_t>(3 * 5 * 5 * 4 * 7 * 3 * 5 * 4));
	CHECK(failures == 0);
}

TEST_CASE("アンインストールを始めた後は常に Uninstalled になり、書き込む操作を持つ notices の行は成り立たない")
{
	// 始めた後の 3 つの進み具合と、インストール・待ち受け・接続・機能のすべての値を組み合わせる (アンインストール
	// したばかりのものを作り直す操作を、画面が勧めないことを確認する)。書き込む操作を持つ行は、CompositeAbsent・
	// InstallMismatch である。
	const UninstallPhase phases[] = {UninstallPhase::Running, UninstallPhase::Done, UninstallPhase::Stopped};
	const VesktopProcess processes[] = {VesktopProcess::Unknown, VesktopProcess::NotRunning,
					    VesktopProcess::Running};
	const VencordDirState dirs[] = {VencordDirState::Unknown, VencordDirState::NotConfigured,
					VencordDirState::Matches, VencordDirState::HashMismatch,
					VencordDirState::PointsElsewhere};
	const ListenerState listeners[] = {ListenerState::Unknown, ListenerState::Listening,
					   ListenerState::BlockedByOtherWriter, ListenerState::PortsExhausted,
					   ListenerState::Closed};
	const LinkState links[] = {LinkState::Unknown, LinkState::NeverConnected, LinkState::Connected,
				   LinkState::Disconnected};
	const FeatureSwitch features[] = {FeatureSwitch::Unknown, FeatureSwitch::Off, FeatureSwitch::On};
	const NoticeRuleId writing_rows[] = {NoticeRuleId::CompositeAbsent, NoticeRuleId::InstallMismatch};

	std::size_t combinations = 0;
	std::size_t wrong_primary = 0;
	std::size_t writing_fired = 0;
	for (const UninstallPhase phase : phases) {
		for (const VesktopProcess process : processes) {
			for (const VencordDirState dir : dirs) {
				for (const ListenerState listener : listeners) {
					for (const LinkState link : links) {
						for (const FeatureSwitch feature : features) {
							Observation obs = healthy();
							obs.uninstall.phase = phase;
							obs.install.process = process;
							obs.install.dir = dir;
							obs.listener.state = listener;
							obs.link.state = link;
							obs.choices.feature = feature;
							// 始めた後の状態として、cordmux のソースがない状態 (CompositeAbsent の条件) を
							// 入れる。
							obs.choices.composite_present = false;
							++combinations;

							if (primary_rule_of(obs) != PrimaryRuleId::Uninstalled)
								++wrong_primary;
							for (const NoticeRuleId row : writing_rows) {
								if (notice_rule_fires(row, obs))
									++writing_fired;
							}
						}
					}
				}
			}
		}
	}
	MESSAGE("combinations=" << combinations);
	CHECK(combinations == static_cast<std::size_t>(3 * 3 * 5 * 5 * 4 * 3));
	CHECK(wrong_primary == 0);
	CHECK(writing_fired == 0);
}

TEST_CASE("修正した Observation では、notices の行が 1 つも成り立たない")
{
	Observation broken = not_connected();
	broken.install.dir = VencordDirState::HashMismatch;
	broken.choices.composite_present = false;
	broken.contacts.push_back({"watch-stop", ContactState::Missing});
	broken.choices.discord_audio_source = DiscordAudioSource::Found;
	CHECK(primary_rule_of(broken) == PrimaryRuleId::VencordOverwritten);
	CHECK(fired_rules(broken) == std::vector<NoticeRuleId>{NoticeRuleId::ContactUnavailable,
								 NoticeRuleId::CompositeAbsent,
								 NoticeRuleId::DoubleAudio});

	// 表の結果は毎回 Observation から決まるので、直った Observation では消える (消すための操作はない)。
	const Observation fixed = healthy();
	CHECK(primary_rule_of(fixed) == PrimaryRuleId::Connected);
	CHECK(fired_rules(fixed).empty());
}

TEST_CASE("接続する前に起きた問題も、接続した後の Observation に残っていれば、notices の行が成り立つ")
{
	// 接続しているのに、同梱の Vencord のファイルが上書きされている。primary は Connected で、この問題は
	// notices の行になる (primary の表で、Connected がインストールの行より上にあるため)。
	Observation obs = healthy();
	obs.install.dir = VencordDirState::HashMismatch;
	CHECK(primary_rule_of(obs) == PrimaryRuleId::Connected);
	CHECK(fired_rules(obs) == std::vector<NoticeRuleId>{NoticeRuleId::InstallMismatch});
	CHECK(apply(obs, ActionId::ReinstallVencordFiles).can_run());

	// 接続していても、Discord の内部への依存箇所が使えなければ、notices の行が成り立つ。Lost も Missing と同じ。
	Observation with_contact = healthy();
	with_contact.contacts.push_back({"watch-stop", ContactState::Lost});
	CHECK(fired_rules(with_contact) == std::vector<NoticeRuleId>{NoticeRuleId::ContactUnavailable});
	// Found・NotSeen・Unknown の依存箇所では成り立たない。
	Observation found = healthy();
	found.contacts.push_back({"watch-stop", ContactState::Found});
	found.contacts.push_back({"roster", ContactState::NotSeen});
	found.contacts.push_back({"self", ContactState::Unknown});
	CHECK(fired_rules(found).empty());
}

TEST_CASE("利用者が選んだ状態と、起動した直後の Unknown では、notices の行が成り立たない")
{
	// 機能の OFF。
	Observation off = healthy();
	off.choices.feature = FeatureSwitch::Off;
	CHECK(primary_rule_of(off) == PrimaryRuleId::FeatureOff);
	CHECK(fired_rules(off).empty());

	// 録画の OFF (利用者が自分で選んだ)。
	Observation chosen = healthy();
	chosen.choices.recording = FeatureSwitch::Off;
	CHECK(fired_rules(chosen).empty());

	// 起動した直後 (すべて Unknown で、まだ何も確認していない)。
	const Observation fresh;
	CHECK(primary_rule_of(fresh) == PrimaryRuleId::VesktopChecking);
	CHECK(fired_rules(fresh).empty());
}

TEST_CASE("notices の表の行数と、行ごとのテストの件数が一致する")
{
	CHECK(notice_rule_count() == kNoticeCaseCount);
	for (std::size_t i = 0; i < notice_rule_count(); ++i)
		CHECK(notice_rule_id_at(i) == kNoticeCases[i].id);
}

TEST_CASE("notices の行は、行ごとに作った状態で成り立ち、healthy() では成り立たない")
{
	const Observation quiet = healthy();
	CHECK(fired_rules(quiet).empty());
	for (std::size_t i = 0; i < kNoticeCaseCount; ++i) {
		CAPTURE(i);
		CHECK_FALSE(notice_rule_fires(kNoticeCases[i].id, quiet));
		CHECK(notice_rule_fires(kNoticeCases[i].id, kNoticeCases[i].make()));
	}
}

TEST_CASE("notices の 2 つの行が同時に成り立ち、primary は Connected のままになる")
{
	Observation obs = healthy();
	obs.choices.composite_present = false;
	obs.choices.recording = FeatureSwitch::On;
	// RecordingOnly が見るのは、録画の入力に届いたフレームである。録画の状態は送信側に届いていることにして、
	// 録画のほかの行が成り立たないようにする。
	obs.recording.enabled_since_us = kNow - 60 * kSec;
	obs.recording.sender_recording = true;
	PersonFact person = showing_person("100", 1);
	person.slot.reset();
	person.last_record_frame_at_us = kNow - kSec / 2;
	obs.people.push_back(person);

	CHECK(fired_rules(obs) == std::vector<NoticeRuleId>{NoticeRuleId::CompositeAbsent, NoticeRuleId::RecordingOnly});
	// 接続していることは表示し続ける (cordmux のソースがないことを、接続していないとは伝えない)。
	CHECK(primary_rule_of(obs) == PrimaryRuleId::Connected);
}

TEST_CASE("CompositeAbsent は、調べ終えたあとで cordmux のソースがないときだけ成り立つ")
{
	Observation obs = healthy();
	obs.choices.composite_present = false;
	CHECK(fired_rules(obs) == std::vector<NoticeRuleId>{NoticeRuleId::CompositeAbsent});

	// まだ調べていない間 (シーンの読み込みの前と、シーンコレクションの切り替えの途中) は成り立たない。
	Observation unchecked = obs;
	unchecked.choices.composite_checked = false;
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::CompositeAbsent, unchecked));

	// 機能そのものを OFF にしている間は成り立たない (利用者が止めているものに、次の操作を勧めない)。
	Observation feature_off = obs;
	feature_off.choices.feature = FeatureSwitch::Off;
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::CompositeAbsent, feature_off));

	// ソースを追加すれば、成り立たない (表の結果は毎回、現在の Observation から決まる)。
	Observation added = obs;
	added.choices.composite_present = true;
	CHECK(fired_rules(added).empty());
}

TEST_CASE("DoubleAudio は、Discord の音声を取り込むソースがあり、cordmux の声も配信に出る状態のときだけ成り立つ")
{
	Observation obs = healthy();
	obs.choices.discord_audio_source = DiscordAudioSource::Found;
	obs.choices.discord_audio_source_name = "Application Audio Capture (BETA)";
	CHECK(fired_rules(obs) == std::vector<NoticeRuleId>{NoticeRuleId::DoubleAudio});

	// cordmux のソースが配信の出力に出ていなければ、cordmux の声は配信に入らないので成り立たない。
	Observation scene_off = obs;
	scene_off.choices.composite_active = false;
	CHECK(fired_rules(scene_off).empty());

	// cordmux がミュートか音量 0% なら、声は二重にならないので成り立たない (cordmux を止めて、
	// アプリケーション音声キャプチャで通話の声を出している利用者)。
	Observation silent = obs;
	silent.choices.composite_audible = false;
	CHECK(fired_rules(silent).empty());

	// 機能を OFF にしている間は、cordmux が声を鳴らさないので成り立たない。
	Observation feature_off = obs;
	feature_off.choices.feature = FeatureSwitch::Off;
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::DoubleAudio, feature_off));

	// ソースが見つからないときと、まだ読めていないときは成り立たない。
	Observation not_found = obs;
	not_found.choices.discord_audio_source = DiscordAudioSource::NotFound;
	not_found.choices.discord_audio_source_name.clear();
	CHECK(fired_rules(not_found).empty());
	Observation unknown = obs;
	unknown.choices.discord_audio_source = DiscordAudioSource::Unknown;
	unknown.choices.discord_audio_source_name.clear();
	CHECK(fired_rules(unknown).empty());
}

TEST_CASE("BroadcasterNotInCall は、メインアカウントが決まっていて、その人が通話の参加者の一覧にいないときだけ成り立つ")
{
	Observation obs = healthy();
	obs.people.push_back(showing_person("100", 1));

	// メインアカウントだけの構成 (broadcaster_user_id が空) では成り立たない。
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::BroadcasterNotInCall, obs));

	// broadcaster_user_id が空でなく、その userId が通話の参加者の一覧にない。
	obs.choices.broadcaster_user_id = "999";
	CHECK(fired_rules(obs) == std::vector<NoticeRuleId>{NoticeRuleId::BroadcasterNotInCall});

	// その人が通話の参加者の一覧にいれば成り立たない。枠に載っていても同じ。
	Observation present = obs;
	PersonFact broadcaster = showing_person("999", 2);
	broadcaster.in_roster = true;
	present.people.push_back(broadcaster);
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::BroadcasterNotInCall, present));

	// 枠に載っていなくても、通話の参加者の一覧にいれば成り立たない (見るのは一覧だけ)。
	Observation present_without_slot = obs;
	PersonFact listener_only = broadcaster;
	listener_only.slot.reset();
	listener_only.last_video_frame_at_us.reset();
	present_without_slot.people.push_back(listener_only);
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::BroadcasterNotInCall, present_without_slot));

	// 枠だけが残っていて、通話の参加者の一覧にいない人は、通話に参加していないとみなす。自動配置で枠に載っていた
	// メインアカウントが通話を抜けても、枠は割り当てたままなので、people から消えない (observation.h の
	// PersonFact::in_roster)。
	Observation left_with_slot = obs;
	PersonFact left = showing_person("999", 2);
	left.in_roster = false;
	left_with_slot.people.push_back(left);
	CHECK(notice_rule_fires(NoticeRuleId::BroadcasterNotInCall, left_with_slot));

	// 通話の参加者の一覧をまだ読めていない間は、参加していないと断定しない。
	Observation unknown_roster = obs;
	unknown_roster.call.roster_count.reset();
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::BroadcasterNotInCall, unknown_roster));

	// Vesktop のアカウントが通話に参加していない間は成り立たない (対象の通話がないので断定しない)。
	Observation out_of_call = obs;
	out_of_call.call.presence = Presence::NotInCall;
	out_of_call.call.roster_count = 0;
	out_of_call.people.clear();
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::BroadcasterNotInCall, out_of_call));

	// 通話に参加しているかがまだ分からない間も同じ。
	Observation unknown_presence = obs;
	unknown_presence.call.presence = Presence::Unknown;
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::BroadcasterNotInCall, unknown_presence));
}

TEST_CASE("AllVideoStoppedHint は、2 人以上の映像が届かず、対象の中に届いている人がいないときだけ成り立つ")
{
	Observation obs = healthy();
	PersonFact sharer = showing_person("100", 1);
	sharer.sharing = true;
	sharer.last_video_frame_at_us = kNow - 5 * kSec; // 止まったとみなすしきい値 (2 秒) を超えた
	obs.people.push_back(sharer);
	sharer.user_id = "101";
	sharer.slot = 2;
	obs.people.push_back(sharer);
	CHECK(fired_rules(obs) == std::vector<NoticeRuleId>{NoticeRuleId::AllVideoStoppedHint});

	// 1 人だけなら成り立たない (その人の行と OBS の映像で分かる)。
	Observation one = obs;
	one.people.pop_back();
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::AllVideoStoppedHint, one));

	// どちらかのフレームが届いていれば成り立たない。
	Observation flowing = obs;
	flowing.people[0].last_video_frame_at_us = kNow - kSec / 2;
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::AllVideoStoppedHint, flowing));

	// 1 度もフレームが届いていない人は、最初のフレームの待ち時間を過ぎてから数える (見始めた直後に伝えない)。
	Observation waiting = obs;
	waiting.people[0].last_video_frame_at_us.reset();
	waiting.people[0].sharing_known = true;
	waiting.people[0].sharing_since_us = kNow - kSec;
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::AllVideoStoppedHint, waiting));
	waiting.people[0].sharing_since_us = kNow - 30 * kSec;
	CHECK(notice_rule_fires(NoticeRuleId::AllVideoStoppedHint, waiting));

	// 画面共有していない人は数えない (画面共有をやめた人の枠と、最後のフレームの時刻は、しばらく残る)。
	Observation not_sharing = obs;
	not_sharing.people[0].sharing = false;
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::AllVideoStoppedHint, not_sharing));

	// 枠に載っていない人は、録画の対象でなければ数えない (枠の映像として見ていない)。
	Observation no_slot = obs;
	no_slot.people[0].slot.reset();
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::AllVideoStoppedHint, no_slot));

	// 接続していないときと、通話に参加していないときは成り立たない (映像が届かない理由がほかにあるため)。
	Observation offline = obs;
	offline.link.state = LinkState::NeverConnected;
	offline.link.connected_at_us.reset();
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::AllVideoStoppedHint, offline));
	Observation out_of_call = obs;
	out_of_call.call.presence = Presence::NotInCall;
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::AllVideoStoppedHint, out_of_call));
}

TEST_CASE("自分の枠を表示しているかは配置だけが使い、どちらの表の結果も変えない")
{
	// self_frame_shown は、配置が表示する人数に自分を含めるかを決めるためだけのフィールドで、どちらの
	// 表の行もこのフィールドでは変わらない。画面の判断にも使うと、枠についての同じ事実を 2 つのフィールドで
	// 持つことになる。
	Observation without = healthy();
	Observation with = healthy();
	with.choices.self_frame_shown = true;
	CHECK(primary_rule_of(without) == primary_rule_of(with));
	CHECK(fired_rules(without) == fired_rules(with));
}

TEST_CASE("RecordingOnly は、枠に載っていない人のうち、録画の入力にフレームが今届いている人がいるときだけ成り立つ")
{
	Observation obs = healthy();
	obs.choices.recording = FeatureSwitch::On;
	// 枠に載っている人 (録画にも届くが、この行では数えない)。
	PersonFact on_slot = showing_person("100", 1);
	on_slot.last_record_frame_at_us = kNow - kSec / 2;
	obs.people.push_back(on_slot);
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::RecordingOnly, obs));

	// 枠に載っていない人。枠への経路がないので、last_video_frame_at_us は空のままである。
	PersonFact person = showing_person("200", 1);
	person.slot.reset();
	person.last_video_frame_at_us.reset();
	person.last_record_frame_at_us = kNow - kSec / 2;
	obs.people.push_back(person);
	CHECK(notice_rule_fires(NoticeRuleId::RecordingOnly, obs));

	// 録画の入力に何も届いていない人は数えない (届いていないものを「受け取っています」と言わない)。
	Observation silent = obs;
	for (PersonFact &p : silent.people)
		p.last_record_frame_at_us.reset();
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::RecordingOnly, silent));

	// 止まってからしばらくたった人も数えない (「受け取っています」は現在のことを言う文である)。
	Observation stale = obs;
	for (PersonFact &p : stale.people) {
		if (!p.slot.has_value())
			p.last_record_frame_at_us = kNow - 30 * kSec;
	}
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::RecordingOnly, stale));

	// 録画が OFF なら成り立たない (録画のために受け取る理由がない)。
	Observation not_recording = obs;
	not_recording.choices.recording = FeatureSwitch::Off;
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::RecordingOnly, not_recording));

	// 枠に載っていない人を全画面にしている間は、その人は配信に映っているので数えない。
	Observation focused = obs;
	focused.choices.focus_user_id = "200";
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::RecordingOnly, focused));
	CHECK(recording_only_person(obs, person));
	CHECK_FALSE(recording_only_person(focused, person));
}

TEST_CASE("RecordSpaceLow は、録画中で、録画できる残りの時間がしきい値以下になったときだけ成り立つ")
{
	Observation obs = healthy();
	obs.choices.recording = FeatureSwitch::On;
	obs.recording.sender_recording = true;
	obs.recording.space_warn_seconds = 30 * 60;

	// まだ確認していない間は成り立たない (分からないことを、空き容量がないとは扱わない)。
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::RecordSpaceLow, obs));
	obs.recording.remaining_seconds = 31 * 60;
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::RecordSpaceLow, obs));
	obs.recording.remaining_seconds = 30 * 60; // しきい値ちょうどで成り立つ
	CHECK(notice_rule_fires(NoticeRuleId::RecordSpaceLow, obs));

	// しきい値は設定から受け取る。60 分にすれば、同じ残りの時間でも成り立つ。
	Observation wide = obs;
	wide.recording.remaining_seconds = 45 * 60;
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::RecordSpaceLow, wide));
	wide.recording.space_warn_seconds = 60 * 60;
	CHECK(notice_rule_fires(NoticeRuleId::RecordSpaceLow, wide));

	// 録画していない間は成り立たない (見積もりに意味がない)。
	Observation stopped = obs;
	stopped.choices.recording = FeatureSwitch::Off;
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::RecordSpaceLow, stopped));
}

TEST_CASE("RecordFinalizeShort は、2 つの数がそろっていて、録画ファイルの作成に使う量が空き容量を超えたときだけ成り立つ")
{
	Observation obs = healthy();
	obs.recording.free_space_bytes = 1000ULL * 1000ULL * 1000ULL;

	// 片方だけでは成り立たない (2 つの数がそろって初めて比べられる)。
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::RecordFinalizeShort, obs));
	obs.recording.finalize_need_bytes = 1000ULL * 1000ULL * 1000ULL;
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::RecordFinalizeShort, obs)); // ちょうど足りる
	obs.recording.finalize_need_bytes = 1000ULL * 1000ULL * 1000ULL + 1;
	CHECK(notice_rule_fires(NoticeRuleId::RecordFinalizeShort, obs));

	// 録画を止めた後も成り立つ (足りないと困るのは録画ファイルの作成の最中である)。
	Observation stopped = obs;
	stopped.choices.recording = FeatureSwitch::Off;
	CHECK(notice_rule_fires(NoticeRuleId::RecordFinalizeShort, stopped));
}

TEST_CASE("RecordFinalizeRunning は作成している間だけ、RecordFinalizePending は作成待ちの録画があるときだけ成り立つ")
{
	Observation running = healthy();
	running.recording.finalize = FinalizeState::Running;
	CHECK(notice_rule_fires(NoticeRuleId::RecordFinalizeRunning, running));

	// 作成していない状態では成り立たない (6 つの値のうち、成り立つのは Running だけ)。
	const FinalizeState quiet_states[] = {FinalizeState::Unknown, FinalizeState::NotStarted, FinalizeState::Done,
					      FinalizeState::Pending, FinalizeState::Failed};
	for (const FinalizeState state : quiet_states) {
		Observation obs = healthy();
		obs.recording.finalize = state;
		CHECK_FALSE(notice_rule_fires(NoticeRuleId::RecordFinalizeRunning, obs));
	}

	// 録画ファイルの作成が終わっていない録画があるときだけ成り立ち、そのとき [作成を再開] を押せる。
	Observation pending = healthy();
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::RecordFinalizePending, pending));
	pending.recording.pending_present = true;
	CHECK(notice_rule_fires(NoticeRuleId::RecordFinalizePending, pending));
	CHECK(apply(pending, ActionId::ResumeFinalize).can_run());

	// 機能を OFF にしていても成り立つ (録画したものから録画ファイルを作成するためのメッセージで、利用者が止めているものに
	// 操作を勧めるメッセージではない)。
	Observation feature_off = pending;
	feature_off.choices.feature = FeatureSwitch::Off;
	CHECK(notice_rule_fires(NoticeRuleId::RecordFinalizePending, feature_off));
}

TEST_CASE("RecordFinalizeFailed は finalize が Failed のときに成り立ち、機能や録画の ON と OFF を条件にしない")
{
	Observation failed = healthy();
	failed.recording.finalize = FinalizeState::Failed;
	CHECK(notice_rule_fires(NoticeRuleId::RecordFinalizeFailed, failed));
	for (const FinalizeState state : {FinalizeState::Unknown, FinalizeState::NotStarted, FinalizeState::Running,
					  FinalizeState::Done, FinalizeState::Pending}) {
		CAPTURE(static_cast<int>(state));
		Observation obs = healthy();
		obs.recording.finalize = state;
		CHECK_FALSE(notice_rule_fires(NoticeRuleId::RecordFinalizeFailed, obs));
	}
	Observation off = failed;
	off.choices.feature = FeatureSwitch::Off;
	off.choices.recording = FeatureSwitch::Off;
	CHECK(notice_rule_fires(NoticeRuleId::RecordFinalizeFailed, off));
	// [作成を再開] を押せるかは pending_present で決まる (録画の側は、録画ファイルの作成に失敗したセッションも数える)。
	// 数えられていなければ押せない。
	CHECK_FALSE(apply(failed, ActionId::ResumeFinalize).can_run());
	failed.recording.pending_present = true;
	CHECK(apply(failed, ActionId::ResumeFinalize).can_run());
}

TEST_CASE("RecordFinalizeFailed は、失敗したセッションが残っていれば、finalize の値にかかわらず成り立つ")
{
	// 新しいセッションを始めた (finalize は NotStarted に戻る) 後も、OBS を再起動した (Unknown) 後も、失敗した
	// セッションが残っていれば成り立つ。別のセッションを作成している間 (Running) も消さない。押し直して、失敗した
	// セッションそのものを作成している間は、録画の側がそのセッションを failed_session_dir に入れない
	// (record/record_types.h の RecordingSnapshot::failed_session_dir) ので、その場合は下の「空なら成り立たない」と
	// 同じである。
	for (const FinalizeState state : {FinalizeState::Unknown, FinalizeState::NotStarted, FinalizeState::Done,
					  FinalizeState::Pending, FinalizeState::Running}) {
		CAPTURE(static_cast<int>(state));
		Observation obs = healthy();
		obs.recording.finalize = state;
		obs.recording.failed_session_dir = "D:/rec/cordmux/2000-01-01_200000";
		CHECK(notice_rule_fires(NoticeRuleId::RecordFinalizeFailed, obs));
		// 録画ファイルの作成が成功して、失敗したセッションがなくなれば成り立たない。
		obs.recording.failed_session_dir.clear();
		CHECK_FALSE(notice_rule_fires(NoticeRuleId::RecordFinalizeFailed, obs));
	}
}

TEST_CASE("ConnectedThenDropped は、Vesktop が起動していないときは、配信中で枠に人が載っているときだけ成り立つ")
{
	Observation obs = healthy();
	obs.link.state = LinkState::Disconnected;
	obs.link.disconnected_at_us = kNow - 30 * kSec;
	obs.people.push_back(showing_person("100", 1));
	for (const VesktopProcess process : {VesktopProcess::Unknown, VesktopProcess::Running}) {
		CAPTURE(static_cast<int>(process));
		obs.install.process = process;
		CHECK(primary_rule_matches(PrimaryRuleId::ConnectedThenDropped, obs));
	}
	obs.install.process = VesktopProcess::NotRunning;
	obs.obs_outputs.streaming = false;
	CHECK_FALSE(primary_rule_matches(PrimaryRuleId::ConnectedThenDropped, obs));
	CHECK(primary_rule_of(obs) == PrimaryRuleId::VesktopNotRunning);
	obs.obs_outputs.streaming.reset();
	CHECK(primary_rule_of(obs) == PrimaryRuleId::VesktopNotRunning);
	obs.obs_outputs.streaming = true;
	CHECK(primary_rule_of(obs) == PrimaryRuleId::ConnectedThenDropped);
	obs.people.clear();
	CHECK(primary_rule_of(obs) == PrimaryRuleId::VesktopNotRunning);
}

TEST_CASE("RecordNotReachedSender は、録画の状態が送信側に届くまでの待ち時間を過ぎてから成り立つ")
{
	Observation obs = healthy();
	obs.choices.recording = FeatureSwitch::On;
	obs.recording.enabled_since_us = kNow - 60 * kSec;
	obs.recording.sender_recording = false;
	CHECK(notice_rule_fires(NoticeRuleId::RecordNotReachedSender, obs));

	// 送信側が「録画している」と返していれば成り立たない。
	Observation reached = obs;
	reached.recording.sender_recording = true;
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::RecordNotReachedSender, reached));

	// キーごと返さない古い送信側も、届いていないとみなす。
	Observation old_sender = obs;
	old_sender.recording.sender_recording.reset();
	CHECK(notice_rule_fires(NoticeRuleId::RecordNotReachedSender, old_sender));

	// 録画を ON にした直後は成り立たない (送信側は facts を、値が変わったときに 1 秒に 1 回まで送る)。
	Observation just_pressed = obs;
	just_pressed.recording.enabled_since_us = kNow - kSec;
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::RecordNotReachedSender, just_pressed));

	// 再接続した直後も成り立たない (録画の状態は接続した直後に送り直すので、その返事を待つ)。
	Observation reconnected = obs;
	reconnected.link.connected_at_us = kNow - kSec;
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::RecordNotReachedSender, reconnected));

	// いつ ON にしたか分からない間は、届いていないと断定しない。
	Observation no_clock = obs;
	no_clock.recording.enabled_since_us.reset();
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::RecordNotReachedSender, no_clock));

	// 接続していなければ成り立たない (届かないのは当然で、接続していないことは primary が伝える)。
	Observation dropped = obs;
	dropped.link.state = LinkState::Disconnected;
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::RecordNotReachedSender, dropped));

	// 録画していなければ成り立たない。
	Observation stopped = obs;
	stopped.choices.recording = FeatureSwitch::Off;
	CHECK_FALSE(notice_rule_fires(NoticeRuleId::RecordNotReachedSender, stopped));
}

TEST_CASE("バージョンの 2 行は文字列の一致で比べ、どれかを読めていなければ成り立たない")
{
	Observation obs = healthy();
	CHECK(fired_rules(obs).empty()); // 3 つともそろっている

	Observation restart = obs;
	restart.versions.discord_side = "0.9.0"; // bundled_vencord と違う
	CHECK(fired_rules(restart) == std::vector<NoticeRuleId>{NoticeRuleId::VersionRestartVesktop});

	Observation mixed = obs;
	mixed.versions.bundled_vencord = "0.9.0"; // plugin とも discord_side とも違う
	CHECK(fired_rules(mixed) ==
	      std::vector<NoticeRuleId>{NoticeRuleId::VersionRestartVesktop, NoticeRuleId::VersionMixedBundle});

	// 同梱の Vencord のマーカーファイルを読めなければ (bundled_vencord が空なら)、どちらの行も成り立たない。
	Observation unread = mixed;
	unread.versions.bundled_vencord.clear();
	CHECK(fired_rules(unread).empty());
}

TEST_CASE("録画の対象: 受け取る範囲が枠に載せた人だけのときは、枠に載っている人だけを対象にする")
{
	Observation obs = healthy();
	obs.choices.recording = FeatureSwitch::On;

	PersonFact slotted;
	slotted.user_id = "1";
	slotted.slot = 2;
	slotted.sharing_known = true;
	slotted.sharing = true;
	PersonFact unslotted;
	unslotted.user_id = "2";
	unslotted.sharing_known = true;
	unslotted.sharing = true;

	// 既定 (枠に載せた人だけ) は、枠に載っている人だけを対象にする。
	CHECK(record_target(obs, slotted));
	CHECK_FALSE(record_target(obs, unslotted));

	// 画面共有している全員にすると、枠に載っていない人も対象にする。
	obs.choices.receive_scope = ReceiveScope::Everyone;
	CHECK(record_target(obs, slotted));
	CHECK(record_target(obs, unslotted));

	obs.choices.receive_scope = ReceiveScope::Slots;
	CHECK(record_target(obs, slotted));
	CHECK_FALSE(record_target(obs, unslotted));

	// 枠に載っていても、画面共有していなければ対象にしない。録画していなければ誰も対象にしない。
	slotted.sharing = false;
	CHECK_FALSE(record_target(obs, slotted));
	slotted.sharing = true;
	obs.choices.recording = FeatureSwitch::Off;
	CHECK_FALSE(record_target(obs, slotted));
}

TEST_CASE("最初のフレームの待ち時間: 枠に載せた人だけでは、視聴の範囲に入れた時刻から数える")
{
	Observation obs = healthy();
	PersonFact p;
	p.user_id = "1";
	p.slot = 1;
	p.sharing_known = true;
	p.sharing = true;
	// ずっと前から共有している人。
	p.sharing_since_us = kNow - 600 * kSec;
	obs.choices.receive_scope = ReceiveScope::Slots;
	const std::int64_t wait = obs.first_frame_wait_threshold_us;

	// 視聴の範囲に入れた時刻を持たなければ、共有していると分かった時刻から数える (今までどおり)。
	CHECK(first_frame_missing(obs, p));

	// 今チェックを付けた (視聴の範囲に入れた) ばかりなら、待ち時間がたつまで、届いていない人に数えない。
	p.watching_since_us = kNow;
	CHECK_FALSE(first_frame_missing(obs, p));
	p.watching_since_us = kNow - wait + 1;
	CHECK_FALSE(first_frame_missing(obs, p));
	// 待ち時間を超えても届かなければ数える。
	p.watching_since_us = kNow - wait;
	CHECK(first_frame_missing(obs, p));

	// チェックを外して付け直しても同じ (付け直した時刻が入る)。
	p.watching_since_us = kNow - 100 * kSec; // 最初に付けたのは前
	CHECK(first_frame_missing(obs, p));
	p.watching_since_us = kNow - kSec; // 外して付け直した
	CHECK_FALSE(first_frame_missing(obs, p));

	// 共有を始めたのが範囲に入れた後なら、共有を始めた時刻から数える。
	p.watching_since_us = kNow - 600 * kSec;
	p.sharing_since_us = kNow - kSec;
	CHECK_FALSE(first_frame_missing(obs, p));

	// 画面共有している全員では、視聴の範囲に入れた時刻を使わない。
	p.sharing_since_us = kNow - 600 * kSec;
	p.watching_since_us = kNow;
	obs.choices.receive_scope = ReceiveScope::Everyone;
	CHECK(first_frame_missing(obs, p));
}

TEST_CASE("届いていた映像が止まったとき: 付け直した人の古いフレームから数えず、止まった後は今までどおり")
{
	Observation obs = healthy();
	obs.choices.receive_scope = ReceiveScope::Slots;
	PersonFact p = showing_person("1", 1);
	p.sharing = true;
	p.sharing_known = true;
	p.sharing_since_us = kNow - 600 * kSec;
	// 外す前の最後のフレーム (止まったとみなすしきい値を超えている)。
	p.last_video_frame_at_us = kNow - 60 * kSec;
	CHECK(stalled_video(obs, p));
	// 付け直したばかり (視聴の範囲に入れた時刻より前のフレームしかない) なら、止まった人に数えない。
	p.watching_since_us = kNow - kSec / 2;
	CHECK_FALSE(stalled_video(obs, p));
	// 付け直した後に届いたフレームが止まったときは、今までどおりフレームの時刻から数える。
	p.watching_since_us = kNow - 60 * kSec;
	p.last_video_frame_at_us = kNow - 10 * kSec;
	CHECK(stalled_video(obs, p));
	p.last_video_frame_at_us = kNow - kSec / 2;
	CHECK_FALSE(stalled_video(obs, p));
}
