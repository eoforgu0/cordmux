// SPDX-License-Identifier: Apache-2.0

// dock/build_observation.h の単体テスト。周期ごとに、Pipeline のスナップショット、OS への問い合わせの結果、
// ドックの設定から Observation を作る関数を確認する。Qt を起動しない (ドックのウィジェット、登録、周期は、
// このテストの実行ファイルに入っていない)。

#include "doctest.h"

#include "dock/build_observation.h"
#include "ledger/rules.h"
#include "ledger/text_ids.h"
#include "platform/platform.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

using cordmux::dock::build_observation;
using cordmux::dock::DockFacts;

namespace {

constexpr std::int64_t kNow = 1'700'000'000'000'000; // システム時刻 (エポックからのマイクロ秒。2023 年 11 月)
constexpr std::int64_t kSec = 1'000'000;

DockFacts facts_at(std::int64_t now_us)
{
	DockFacts facts;
	facts.now_us = now_us;
	facts.utc_offset_sec = 9 * 3600;
	facts.video_stall_threshold_us = 2 * kSec;
	facts.plugin_version = "1.2.3";
	return facts;
}

// 何も起きていないスナップショット (起動した直後。Listener も Pipeline も何も設定していない)。
cordmux::pipeline::PipelineSnapshot empty_snapshot()
{
	cordmux::pipeline::PipelineSnapshot snapshot;
	snapshot.taken_at_wall_us = kNow;
	snapshot.taken_at_steady_us = 60 * kSec;
	return snapshot;
}

cordmux::pipeline::SlotSnapshot screen_slot(int slot, const std::string &user_id)
{
	cordmux::pipeline::SlotSnapshot row;
	row.kind = cordmux::config::SlotKind::Screen;
	row.slot = slot;
	row.owner_user_id = user_id;
	row.voice_user_id = user_id;
	row.output_allowed = true;
	row.video_allowed = true;
	return row;
}

// steady の時刻で「ago_us マイクロ秒前に最後のフレームが届いた」ストリームを作る。
cordmux::pipeline::StreamFrameClock frame_clock(std::uint32_t stream_id, const std::string &user_id,
						const cordmux::pipeline::PipelineSnapshot &snapshot,
						std::int64_t ago_us)
{
	cordmux::pipeline::StreamFrameClock clock;
	clock.stream_id = stream_id;
	clock.user_id = user_id;
	clock.kind = cordmux::config::SlotKind::Screen;
	clock.last_video_frame_steady_us = snapshot.taken_at_steady_us - ago_us;
	clock.last_video_frame_wall_us = snapshot.taken_at_wall_us - ago_us;
	return clock;
}

const cordmux::ledger::PersonFact *person_of(const cordmux::ledger::Observation &obs, const std::string &user_id)
{
	for (const cordmux::ledger::PersonFact &person : obs.people) {
		if (person.user_id == user_id)
			return &person;
	}
	return nullptr;
}

namespace keys = cordmux::ledger::text::keys;

// 人の行 (decide_dock の View) を userId で探す (並びは見えている名前の自然な順なので、添字で引かない)。
const cordmux::ledger::dockview::PersonRow *row_of(const cordmux::ledger::dockview::DockViewV2 &view,
						   const std::string &user_id)
{
	for (const cordmux::ledger::dockview::PersonRow &row : view.people) {
		if (row.user_id == user_id)
			return &row;
	}
	return nullptr;
}

// 設定ウィンドウの「バージョン情報」のページの、バージョンの行の値 (なければ空)。
std::string version_value(const cordmux::ledger::dockview::DockViewV2 &view, const char *label)
{
	for (const cordmux::ledger::dockview::VersionRow &row : view.about.versions) {
		if (row.label.id == label)
			return row.value.literal;
	}
	return std::string();
}

} // namespace

TEST_CASE("起動した直後のスナップショットからは、すべての事実が「まだ分からない」になる")
{
	const cordmux::ledger::Observation obs =
		build_observation(empty_snapshot(), cordmux::platform::OsProbeResult{}, facts_at(kNow));

	CHECK(obs.install.process == cordmux::ledger::VesktopProcess::Unknown);
	CHECK(obs.install.dir == cordmux::ledger::VencordDirState::Unknown);
	CHECK_FALSE(obs.install.vesktop_started_at_us.has_value());
	CHECK_FALSE(obs.install.discord_side_written_at_us.has_value());
	CHECK(obs.listener.state == cordmux::ledger::ListenerState::Unknown);
	CHECK(obs.link.state == cordmux::ledger::LinkState::Unknown);
	CHECK(obs.call.presence == cordmux::ledger::Presence::Unknown);
	CHECK_FALSE(obs.call.roster_count.has_value());
	CHECK(obs.people.empty());
	CHECK(obs.contacts.empty());
	CHECK(obs.versions.discord_side.empty());

	// 起動した直後を異常として扱わない。
	CHECK(cordmux::ledger::primary_rule_of(obs) == cordmux::ledger::PrimaryRuleId::VesktopChecking);
}

TEST_CASE("時刻、時差、しきい値、利用者が選んだ状態は、DockFacts からそのままコピーされる")
{
	DockFacts facts = facts_at(kNow);
	facts.utc_offset_sec = -5 * 3600;
	facts.video_stall_threshold_us = 7 * kSec;
	facts.feature = cordmux::ledger::FeatureSwitch::Off;
	facts.dummy = cordmux::ledger::FeatureSwitch::On;
	facts.composite_present = true;
	facts.composite_checked = true;
	facts.discord_audio_source = cordmux::ledger::DiscordAudioSource::Found;
	facts.discord_audio_source_name = "Discord audio";
	facts.composite_active = true;
	facts.composite_audible = true;
	facts.bundled_vencord_version = "2000-01-01";

	const cordmux::ledger::Observation obs =
		build_observation(empty_snapshot(), cordmux::platform::OsProbeResult{}, facts);

	CHECK(obs.now_us == kNow);
	CHECK(obs.utc_offset_sec == -5 * 3600);
	CHECK(obs.video_stall_threshold_us == 7 * kSec);
	CHECK(obs.choices.feature == cordmux::ledger::FeatureSwitch::Off);
	// 録画のオンとオフだけは DockFacts から来ない (下の録画のテスト)。
	CHECK(obs.choices.recording == cordmux::ledger::FeatureSwitch::Off);
	CHECK(obs.choices.dummy == cordmux::ledger::FeatureSwitch::On);
	CHECK(obs.choices.composite_present);
	CHECK(obs.choices.composite_checked);
	CHECK(obs.choices.discord_audio_source == cordmux::ledger::DiscordAudioSource::Found);
	CHECK(obs.choices.discord_audio_source_name == "Discord audio");
	CHECK(obs.choices.composite_active);
	CHECK(obs.choices.composite_audible);
	CHECK(obs.versions.plugin == "1.2.3");
	CHECK(obs.versions.bundled_vencord == "2000-01-01");
	// Vesktop のバージョンは DockFacts からは来ない (送信側が hello で送る。下の「接続している」のテスト)。
	CHECK(obs.versions.vesktop.empty());
}

TEST_CASE("OS への問い合わせの結果が、install (Vesktop への cordmux の追加) の各フィールドにコピーされる")
{
	SUBCASE("問い合わせがまだ終わっていなければ、Running とも NotRunning ともしない")
	{
		cordmux::platform::OsProbeResult probe;
		probe.probed = false;
		probe.vesktop_process = true; // probed が false なら見ない
		const cordmux::ledger::Observation obs = build_observation(empty_snapshot(), probe, facts_at(kNow));
		CHECK(obs.install.process == cordmux::ledger::VesktopProcess::Unknown);
		CHECK(obs.install.dir == cordmux::ledger::VencordDirState::Unknown);
	}

	SUBCASE("Vesktop が起動していなければ NotRunning にする")
	{
		cordmux::platform::OsProbeResult probe;
		probe.probed = true;
		const cordmux::ledger::Observation obs = build_observation(empty_snapshot(), probe, facts_at(kNow));
		CHECK(obs.install.process == cordmux::ledger::VesktopProcess::NotRunning);
		CHECK_FALSE(obs.install.vesktop_started_at_us.has_value());
	}

	SUBCASE("起動しているが開始時刻だけを読めないときは、開始時刻を持たない (0 は「分からない」で、1970 年ではない)")
	{
		cordmux::platform::OsProbeResult probe;
		probe.probed = true;
		probe.vesktop_process = true;
		probe.vesktop_started_at_us = 0;
		const cordmux::ledger::Observation obs = build_observation(empty_snapshot(), probe, facts_at(kNow));
		CHECK(obs.install.process == cordmux::ledger::VesktopProcess::Running);
		CHECK_FALSE(obs.install.vesktop_started_at_us.has_value());
	}

	SUBCASE("起動していて開始時刻も読めたら、開始時刻をコピーする")
	{
		cordmux::platform::OsProbeResult probe;
		probe.probed = true;
		probe.vesktop_process = true;
		probe.vesktop_started_at_us = kNow - 60 * kSec;
		const cordmux::ledger::Observation obs = build_observation(empty_snapshot(), probe, facts_at(kNow));
		CHECK(obs.install.vesktop_started_at_us.value_or(0) == kNow - 60 * kSec);
	}

	SUBCASE("state.json を読めないときは「まだ分からない」、キーがないときは「まだ設定していない」にする")
	{
		// 2 つは別の事実である。キーがないときを Unknown にまとめると、画面から [Vesktopに追加] が消え、
		// 利用者が次に何をすればよいかが表示されなくなる。
		cordmux::platform::OsProbeResult probe;
		probe.probed = true;
		probe.state_json_read = false;
		CHECK(build_observation(empty_snapshot(), probe, facts_at(kNow)).install.dir ==
		      cordmux::ledger::VencordDirState::Unknown);

		probe.state_json_read = true; // 読めたが vencordDir のキーがない
		const cordmux::ledger::Observation day_one = build_observation(empty_snapshot(), probe, facts_at(kNow));
		CHECK(day_one.install.dir == cordmux::ledger::VencordDirState::NotConfigured);
		// その状態では、次の操作を表示する (ledger/rules.cpp の表の該当する行)。
		CHECK(cordmux::ledger::primary_rule_of(day_one) ==
		      cordmux::ledger::PrimaryRuleId::VencordDirNotConfigured);
	}

	SUBCASE("vencordDir が別の場所を指していれば PointsElsewhere にする")
	{
		cordmux::platform::OsProbeResult probe;
		probe.probed = true;
		probe.state_json_read = true;
		probe.vencord_dir = std::string("C:/elsewhere/vencord");
		probe.points_at_bundle = false;
		probe.bundle_hash.state = cordmux::platform::BundleState::Matches;
		const cordmux::ledger::Observation obs = build_observation(empty_snapshot(), probe, facts_at(kNow));
		CHECK(obs.install.dir == cordmux::ledger::VencordDirState::PointsElsewhere);
	}

	SUBCASE("同梱の場所を指していれば、4 つのファイルの照合の結果をそのまま使う")
	{
		cordmux::platform::OsProbeResult probe;
		probe.probed = true;
		probe.state_json_read = true;
		probe.vencord_dir = std::string("C:/plugins/cordmux/data/vencord");
		probe.points_at_bundle = true;

		probe.bundle_hash.state = cordmux::platform::BundleState::Matches;
		CHECK(build_observation(empty_snapshot(), probe, facts_at(kNow)).install.dir ==
		      cordmux::ledger::VencordDirState::Matches);

		probe.bundle_hash.state = cordmux::platform::BundleState::Mismatch;
		CHECK(build_observation(empty_snapshot(), probe, facts_at(kNow)).install.dir ==
		      cordmux::ledger::VencordDirState::HashMismatch);

		// 元のファイルを読めずに照合できなかったときは、「違う」としない。
		probe.bundle_hash.state = cordmux::platform::BundleState::Unknown;
		CHECK(build_observation(empty_snapshot(), probe, facts_at(kNow)).install.dir ==
		      cordmux::ledger::VencordDirState::Unknown);
	}

	SUBCASE("保存した「最後に書き込んだ時刻」がそのまま渡り、再起動が必要かの判定に使われる")
	{
		cordmux::platform::OsProbeResult probe;
		probe.probed = true;
		probe.vesktop_process = true;
		probe.vesktop_started_at_us = kNow - 600 * kSec;
		probe.state_json_written_at_us = kNow - 60 * kSec; // 起動より後に書き込んだ
		const cordmux::ledger::Observation obs = build_observation(empty_snapshot(), probe, facts_at(kNow));
		CHECK(obs.install.discord_side_written_at_us.value_or(0) == kNow - 60 * kSec);
		CHECK(cordmux::ledger::restart_needed(obs));
	}

	SUBCASE("ドックが自分で見た Vesktop の状態 (DockFacts::vesktop_process_seen) は、OsProbe の結果より先に使う")
	{
		// 書く直前に起動していたのを見たとき (Running か Unknown) と、接続していない間に終了を見たとき (NotRunning)。
		// 押せるかは Install::process で決まるので、OsProbe の古い結果のままにしない。起動の時刻は OsProbe の値のまま。
		cordmux::platform::OsProbeResult probe;
		probe.probed = true;
		probe.vesktop_process = true;
		probe.vesktop_started_at_us = kNow - 600 * kSec;
		DockFacts facts = facts_at(kNow);
		facts.vesktop_process_seen = cordmux::ledger::VesktopProcess::NotRunning;
		cordmux::ledger::Observation obs = build_observation(empty_snapshot(), probe, facts);
		CHECK(obs.install.process == cordmux::ledger::VesktopProcess::NotRunning);
		CHECK(obs.install.vesktop_started_at_us.value_or(0) == kNow - 600 * kSec);

		probe.vesktop_process = false;
		probe.vesktop_started_at_us = 0;
		facts.vesktop_process_seen = cordmux::ledger::VesktopProcess::Running;
		obs = build_observation(empty_snapshot(), probe, facts);
		CHECK(obs.install.process == cordmux::ledger::VesktopProcess::Running);
		facts.vesktop_process_seen = cordmux::ledger::VesktopProcess::Unknown;
		CHECK(build_observation(empty_snapshot(), probe, facts).install.process ==
		      cordmux::ledger::VesktopProcess::Unknown);

		// 見ていないとき (nullopt) は OsProbe の結果のまま。
		facts.vesktop_process_seen.reset();
		CHECK(build_observation(empty_snapshot(), probe, facts).install.process ==
		      cordmux::ledger::VesktopProcess::NotRunning);
	}
}

TEST_CASE("待ち受けの結果 (Listener::stats() の open) が listener にコピーされる")
{
	SUBCASE("まだ待ち受けを始めようとしていなければ Unknown にする")
	{
		cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
		snapshot.listener.open.attempted = false;
		snapshot.listener.open.outcome = cordmux::ws::OpenOutcome::PortsExhausted; // 既定値に影響されない
		const cordmux::ledger::Observation obs =
			build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
		CHECK(obs.listener.state == cordmux::ledger::ListenerState::Unknown);
	}

	SUBCASE("待ち受けを始められたことと、今待ち受けていることは区別する")
	{
		cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
		snapshot.listener.open.attempted = true;
		snapshot.listener.open.outcome = cordmux::ws::OpenOutcome::Opened;

		snapshot.listener.listening = true;
		CHECK(build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow)).listener.state ==
		      cordmux::ledger::ListenerState::Listening);

		snapshot.listener.listening = false;
		CHECK(build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow)).listener.state ==
		      cordmux::ledger::ListenerState::Closed);
	}

	SUBCASE("2 つ目の OBS が動いているときは、その pid と、画面に表示できる時刻に変換した起動時刻をコピーする")
	{
		cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
		snapshot.listener.open.attempted = true;
		snapshot.listener.open.outcome = cordmux::ws::OpenOutcome::BlockedByOtherWriter;
		snapshot.listener.open.other_writer_pid = 4242;
		// session.json の started_at は、1601 年からの 100 ナノ秒単位の値 (docs/wire_format.md の
		// 「トークンファイル」)。そのまま時刻として表示すると桁違いの値になるので、変換を通すことを確認する。
		const std::int64_t os_time = 133'000'000'000'000'000LL;
		snapshot.listener.open.other_writer_started_at = os_time;

		const cordmux::ledger::Observation obs =
			build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
		CHECK(obs.listener.state == cordmux::ledger::ListenerState::BlockedByOtherWriter);
		CHECK(obs.listener.other_writer_pid.value_or(0) == 4242);
		CHECK(obs.listener.other_writer_started_at_us.value_or(0) ==
		      cordmux::platform::os_process_time_to_epoch_us(os_time));
		CHECK(obs.listener.other_writer_started_at_us.value_or(0) != os_time);
		CHECK(cordmux::ledger::primary_rule_of(obs) == cordmux::ledger::PrimaryRuleId::OtherObsHolding);
	}

	SUBCASE("2 つ目の OBS が開き直して待ち受けを始めると、別の OBS が動いているというメッセージの元の状態が消える")
	{
		cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
		snapshot.listener.open.attempted = true;
		snapshot.listener.open.outcome = cordmux::ws::OpenOutcome::BlockedByOtherWriter;
		snapshot.listener.open.other_writer_pid = 4242;
		snapshot.listener.open.other_writer_started_at = 133'000'000'000'000'000LL;
		REQUIRE(cordmux::ledger::primary_rule_of(
				build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow))) ==
			cordmux::ledger::PrimaryRuleId::OtherObsHolding);

		// Listener::try_open() が Opened で成功すると、open は書き換わり、別の OBS の pid と起動時刻は 0 に戻る。
		snapshot.listener.open.outcome = cordmux::ws::OpenOutcome::Opened;
		snapshot.listener.open.other_writer_pid = 0;
		snapshot.listener.open.other_writer_started_at = 0;
		snapshot.listener.listening = true;
		const cordmux::ledger::Observation obs =
			build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
		CHECK(obs.listener.state == cordmux::ledger::ListenerState::Listening);
		CHECK_FALSE(obs.listener.other_writer_pid.has_value());
		CHECK(cordmux::ledger::primary_rule_of(obs) != cordmux::ledger::PrimaryRuleId::OtherObsHolding);
	}

	SUBCASE("ポート番号が尽きたときは、試した番号の範囲だけをコピーする (試していないときはコピーしない)")
	{
		cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
		snapshot.listener.open.attempted = true;
		snapshot.listener.open.outcome = cordmux::ws::OpenOutcome::PortsExhausted;
		snapshot.listener.open.port_first = 4470;
		snapshot.listener.open.port_last = 4485;

		cordmux::ledger::Observation obs =
			build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
		CHECK(obs.listener.state == cordmux::ledger::ListenerState::PortsExhausted);
		CHECK(obs.listener.port_first.value_or(0) == 4470);
		CHECK(obs.listener.port_last.value_or(0) == 4485);
		CHECK(cordmux::ledger::primary_rule_of(obs) == cordmux::ledger::PrimaryRuleId::ListenPortsExhausted);

		snapshot.listener.open.port_first = 0;
		snapshot.listener.open.port_last = 0;
		obs = build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
		CHECK_FALSE(obs.listener.port_first.has_value());
		CHECK_FALSE(obs.listener.port_last.has_value());
	}

	SUBCASE("待ち受けを始めたが接続の情報のファイルを保存できなかったときは、待ち受け中のままフラグをコピーする")
	{
		cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
		snapshot.listener.open.attempted = true;
		snapshot.listener.open.outcome = cordmux::ws::OpenOutcome::Opened;
		snapshot.listener.listening = true;
		snapshot.listener.open.problem = cordmux::ws::OpenProblem::SessionFileNotSaved;

		cordmux::ledger::Observation obs =
			build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
		CHECK(obs.listener.state == cordmux::ledger::ListenerState::Listening);
		CHECK(obs.listener.session_file_not_saved);

		// 保存できたときはフラグを立てない。
		snapshot.listener.open.problem = cordmux::ws::OpenProblem::None;
		obs = build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
		CHECK_FALSE(obs.listener.session_file_not_saved);

		// 接続の受け付けを始められなかったとき (outcome は PortsExhausted) は、このフラグを立てない。
		snapshot.listener.open.outcome = cordmux::ws::OpenOutcome::PortsExhausted;
		snapshot.listener.listening = false;
		snapshot.listener.open.problem = cordmux::ws::OpenProblem::AcceptFailed;
		obs = build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
		CHECK(obs.listener.state == cordmux::ledger::ListenerState::PortsExhausted);
		CHECK_FALSE(obs.listener.session_file_not_saved);
		// 受け付けの失敗は accept_failed にコピーされる (ポート番号の範囲の文を表示しないためのフラグ)。
		CHECK(obs.listener.accept_failed);

		// ポート番号がすべて使われていたとき (problem は None) は立てない。
		snapshot.listener.open.problem = cordmux::ws::OpenProblem::None;
		obs = build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
		CHECK(obs.listener.state == cordmux::ledger::ListenerState::PortsExhausted);
		CHECK_FALSE(obs.listener.accept_failed);
	}
}

TEST_CASE("接続の事実が link にコピーされる")
{
	SUBCASE("待ち受けているが、まだ一度もクライアントが接続していなければ NeverConnected にする")
	{
		cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
		snapshot.listener.open.attempted = true;
		snapshot.listener.open.outcome = cordmux::ws::OpenOutcome::Opened;
		snapshot.listener.listening = true;
		const cordmux::ledger::Observation obs =
			build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
		CHECK(obs.link.state == cordmux::ledger::LinkState::NeverConnected);
		CHECK_FALSE(obs.link.connected_at_us.has_value());
	}

	SUBCASE("接続しているときは、送信側が送ったバージョンもコピーする")
	{
		cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
		snapshot.listener.open.attempted = true;
		snapshot.listener.open.outcome = cordmux::ws::OpenOutcome::Opened;
		snapshot.listener.listening = true;
		snapshot.listener.client_connected = true;
		snapshot.listener.connection_opened_at_us = kNow - 30 * kSec;
		snapshot.listener.hello_ok_at_us = kNow - 29 * kSec;
		snapshot.listener.peer_version = "0.9.1";
		snapshot.listener.peer_vesktop_version = "1.6.7";

		const cordmux::ledger::Observation obs =
			build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
		CHECK(obs.link.state == cordmux::ledger::LinkState::Connected);
		CHECK(obs.link.connected_at_us.value_or(0) == kNow - 30 * kSec);
		CHECK(obs.versions.discord_side == "0.9.1");
		CHECK(cordmux::ledger::primary_rule_of(obs) == cordmux::ledger::PrimaryRuleId::Connected);

		// Vesktop のバージョンも hello で送られたものがコピーされ、設定ウィンドウの「バージョン情報」の
		// ページのバージョンの行に表示される (docs/wire_format.md の「メッセージの種類」)。
		CHECK(obs.versions.vesktop == "1.6.7");
		CHECK(version_value(cordmux::ledger::decide_dock(obs), keys::kSettingsVersionVesktopLabel) == "1.6.7");

		// 古い送信側 (キーを送らない) や、Vesktop の API を読めなかった送信側では空 (分からない) になる。
		// バージョンの行は "-" を表示する (適当な値で埋めない)。
		snapshot.listener.peer_vesktop_version.clear();
		const cordmux::ledger::Observation unknown =
			build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
		CHECK(unknown.versions.vesktop.empty());
		CHECK(version_value(cordmux::ledger::decide_dock(unknown), keys::kSettingsVersionVesktopLabel) == "-");
	}

	SUBCASE("トークンが合わずに切断された接続も、接続した後に切断されたものとして扱う")
	{
		// hello を通らずに切断されたので、hello_ok_at_us は 0 のままである。接続した時刻を hello の成否で
		// 決めると、このメッセージを一度も表示できなくなる (ws/listener.cpp の connection_opened_at_us の
		// コメント)。
		cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
		snapshot.listener.open.attempted = true;
		snapshot.listener.open.outcome = cordmux::ws::OpenOutcome::Opened;
		snapshot.listener.listening = true;
		snapshot.listener.client_connected = false;
		snapshot.listener.connection_opened_at_us = kNow - 20 * kSec;
		snapshot.listener.hello_ok_at_us = 0;
		snapshot.listener.connection_closed_at_us = kNow - 19 * kSec;
		snapshot.listener.last_disconnect_reason = cordmux::ws::DisconnectReason::SecretMismatch;

		const cordmux::ledger::Observation obs =
			build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
		CHECK(obs.link.state == cordmux::ledger::LinkState::Disconnected);
		CHECK(obs.link.connected_at_us.value_or(0) == kNow - 20 * kSec);
		CHECK(obs.link.disconnected_at_us.value_or(0) == kNow - 19 * kSec);
		CHECK(obs.link.reason == cordmux::ledger::DisconnectReason::SecretMismatch);
		CHECK(cordmux::ledger::primary_rule_of(obs) == cordmux::ledger::PrimaryRuleId::ConnectedThenDropped);
	}

	SUBCASE("切断の理由を ledger の値に置き換える (こちらから閉じた場合だけは、対応する値がない)")
	{
		struct Row {
			cordmux::ws::DisconnectReason from;
			cordmux::ledger::DisconnectReason to;
		};
		const Row rows[] = {
			{cordmux::ws::DisconnectReason::Unknown, cordmux::ledger::DisconnectReason::Unknown},
			{cordmux::ws::DisconnectReason::SecretMismatch,
			 cordmux::ledger::DisconnectReason::SecretMismatch},
			{cordmux::ws::DisconnectReason::WireMismatch, cordmux::ledger::DisconnectReason::WireMismatch},
			{cordmux::ws::DisconnectReason::MalformedHello,
			 cordmux::ledger::DisconnectReason::MalformedHello},
			{cordmux::ws::DisconnectReason::HelloTimeout, cordmux::ledger::DisconnectReason::HelloTimeout},
			{cordmux::ws::DisconnectReason::HeartbeatTimeout,
			 cordmux::ledger::DisconnectReason::HeartbeatTimeout},
			{cordmux::ws::DisconnectReason::PeerClosed, cordmux::ledger::DisconnectReason::PeerClosed},
			{cordmux::ws::DisconnectReason::LocalClose, cordmux::ledger::DisconnectReason::Unknown},
		};

		for (const Row &row : rows) {
			cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
			snapshot.listener.open.attempted = true;
			snapshot.listener.open.outcome = cordmux::ws::OpenOutcome::Opened;
			snapshot.listener.connection_opened_at_us = kNow - 20 * kSec;
			snapshot.listener.connection_closed_at_us = kNow - 19 * kSec;
			snapshot.listener.last_disconnect_reason = row.from;
			const cordmux::ledger::Observation obs =
				build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
			CHECK(obs.link.reason == row.to);
		}
	}
}

TEST_CASE("直近の facts が contacts (Discord の内部への依存箇所) と call (通話) にコピーされる")
{
	SUBCASE("facts が一度も届いていなければ、人数を 0 人としない")
	{
		const cordmux::ledger::Observation obs =
			build_observation(empty_snapshot(), cordmux::platform::OsProbeResult{}, facts_at(kNow));
		CHECK(obs.call.presence == cordmux::ledger::Presence::Unknown);
		CHECK_FALSE(obs.call.roster_count.has_value());
	}

	SUBCASE("roster のキーが省かれていれば、「まだ分からない」のままにする")
	{
		cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
		snapshot.facts.received = true;
		const cordmux::ledger::Observation obs =
			build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
		CHECK(obs.call.presence == cordmux::ledger::Presence::Unknown);
	}

	SUBCASE("人数は user_ids の数で数える (送信側の count は使わない)")
	{
		cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
		snapshot.facts.received = true;
		cordmux::pipeline::UserIdSet roster;
		roster.sent_count = 99; // 食い違っていても user_ids の数を使う
		roster.user_ids = {"u1", "u2", "u3"};
		snapshot.facts.roster = roster;

		const cordmux::ledger::Observation obs =
			build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
		CHECK(obs.call.presence == cordmux::ledger::Presence::InCall);
		CHECK(obs.call.roster_count.value_or(-1) == 3);
		CHECK(obs.people.size() == 3);
	}

	SUBCASE("空の roster は、「分からない」ではなく「通話に参加していない」にする")
	{
		cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
		snapshot.facts.received = true;
		snapshot.facts.roster = cordmux::pipeline::UserIdSet{};
		const cordmux::ledger::Observation obs =
			build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
		CHECK(obs.call.presence == cordmux::ledger::Presence::NotInCall);
		CHECK(obs.call.roster_count.value_or(-1) == 0);
	}

	SUBCASE("読めなくなった依存箇所の id が、並びの順のまま contacts にコピーされる")
	{
		cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
		snapshot.facts.received = true;
		snapshot.facts.warnings = {"ssrc-map", "profile"};
		const cordmux::ledger::Observation obs =
			build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
		REQUIRE(obs.contacts.size() == 2);
		CHECK(obs.contacts[0].id == "ssrc-map");
		CHECK(obs.contacts[0].state == cordmux::ledger::ContactState::Missing);
		CHECK(obs.contacts[1].id == "profile");
	}
}

TEST_CASE("枠、フレームの時刻、エンコードの段階が people (人) にコピーされる")
{
	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
	snapshot.slots.push_back(screen_slot(1, "alice"));
	snapshot.slots.push_back(screen_slot(2, "bob"));
	snapshot.facts.received = true;
	cordmux::pipeline::UserIdSet roster;
	roster.user_ids = {"alice", "bob", "carol"};
	roster.sent_count = 3;
	snapshot.facts.roster = roster;

	// alice は 0.5 秒前、bob は 5 秒前 (しきい値 2 秒を超えている)。
	snapshot.last_frames[11] = frame_clock(11, "alice", snapshot, kSec / 2);
	snapshot.last_frames[12] = frame_clock(12, "bob", snapshot, 5 * kSec);
	snapshot.facts.stages = std::vector<cordmux::pipeline::StreamStage>{{11, "halffps"}};

	const cordmux::ledger::Observation obs =
		build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));

	SUBCASE("枠に載っている人が先に並び、通話に参加しているだけの人が後に続く")
	{
		REQUIRE(obs.people.size() == 3);
		CHECK(obs.people[0].user_id == "alice");
		CHECK(obs.people[1].user_id == "bob");
		CHECK(obs.people[2].user_id == "carol");
	}

	SUBCASE("画面共有の枠の番号は枠の行から決まり、枠に載せていない人は番号を持たない")
	{
		CHECK(person_of(obs, "alice")->slot.value_or(0) == 1);
		CHECK(person_of(obs, "bob")->slot.value_or(0) == 2);
		CHECK_FALSE(person_of(obs, "carol")->slot.has_value());
	}

	SUBCASE("最後のフレームの時刻は、steady で測った経過時間を現在のシステム時刻から引いて求める")
	{
		CHECK(person_of(obs, "alice")->last_video_frame_at_us.value_or(0) == kNow - kSec / 2);
		CHECK(person_of(obs, "bob")->last_video_frame_at_us.value_or(0) == kNow - 5 * kSec);
		CHECK_FALSE(person_of(obs, "carol")->last_video_frame_at_us.has_value());
	}

	SUBCASE("段階は stream_id から人に対応付け、報告のないストリームには段階を付けない")
	{
		CHECK(person_of(obs, "alice")->stage.value_or("") == "halffps");
		CHECK_FALSE(person_of(obs, "bob")->stage.has_value());
	}

	SUBCASE("フレームが 2 秒届いていない人だけを、映像が届かなくなった状態にする")
	{
		const cordmux::ledger::dockview::DockViewV2 view = cordmux::ledger::decide_dock(obs);
		REQUIRE(view.people.size() == 3);
		const cordmux::ledger::dockview::PersonRow *alice = row_of(view, "alice");
		const cordmux::ledger::dockview::PersonRow *bob = row_of(view, "bob");
		const cordmux::ledger::dockview::PersonRow *carol = row_of(view, "carol");
		REQUIRE(alice != nullptr);
		REQUIRE(bob != nullptr);
		REQUIRE(carol != nullptr);
		CHECK_FALSE(alice->status.has_value());
		REQUIRE(bob->status.has_value());
		CHECK(bob->status->state == cordmux::ledger::dockview::RowState::NoVideo);
		CHECK_FALSE(carol->status.has_value()); // 枠に載せていない人には、状態のテキストを表示しない
		// 映像が止まっていても、接続が切れたとはしない。
		CHECK(obs.link.state != cordmux::ledger::LinkState::Disconnected);
	}
}

TEST_CASE("声の枠だけの人も人の行になる")
{
	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
	cordmux::pipeline::SlotSnapshot screen = screen_slot(1, "alice");
	snapshot.slots.push_back(screen);

	cordmux::pipeline::SlotSnapshot voice;
	voice.kind = cordmux::config::SlotKind::Voice;
	voice.slot = 3;
	voice.owner_user_id = "bob";
	voice.voice_user_id = "bob";
	snapshot.slots.push_back(voice);

	const cordmux::ledger::Observation obs =
		build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));

	REQUIRE(obs.people.size() == 2);
	CHECK(person_of(obs, "alice") != nullptr);
	// 声だけの枠は、画面共有の枠の番号を持たない (ledger::PersonFact::slot は画面共有の枠の 1 から 4)。
	CHECK_FALSE(person_of(obs, "bob")->slot.has_value());
}

TEST_CASE("同じ人にストリームが 2 本あれば、新しい方のフレームの時刻と段階を使う")
{
	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
	snapshot.slots.push_back(screen_slot(1, "alice"));
	snapshot.last_frames[21] = frame_clock(21, "alice", snapshot, 4 * kSec);
	snapshot.last_frames[22] = frame_clock(22, "alice", snapshot, kSec);
	snapshot.facts.stages = std::vector<cordmux::pipeline::StreamStage>{{21, "software"}, {22, "downscaled"}};

	const cordmux::ledger::Observation obs =
		build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
	REQUIRE(obs.people.size() == 1);
	CHECK(obs.people[0].last_video_frame_at_us.value_or(0) == kNow - kSec);
	CHECK(obs.people[0].stage.value_or("") == "downscaled");
}

TEST_CASE("steady の時刻を持たないスナップショットでは、システム時刻の値をそのまま使う")
{
	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
	snapshot.taken_at_steady_us = 0;
	snapshot.slots.push_back(screen_slot(1, "alice"));
	cordmux::pipeline::StreamFrameClock clock;
	clock.stream_id = 31;
	clock.user_id = "alice";
	clock.last_video_frame_steady_us = 0;
	clock.last_video_frame_wall_us = kNow - 3 * kSec;
	snapshot.last_frames[31] = clock;

	const cordmux::ledger::Observation obs =
		build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
	REQUIRE(obs.people.size() == 1);
	CHECK(obs.people[0].last_video_frame_at_us.value_or(0) == kNow - 3 * kSec);
}

TEST_CASE("ほかの OBS の起動時刻を変換する処理は、このテストのプロセス自身の起動時刻で確認できる")
{
	// session.json に書く started_at (OS に依存する値) を画面の時刻に変換する処理が、find_vesktop_process が使う
	// のと同じ変換であることを、今動いているプロセスで確認する (platform.h の current_process_started_at_us の
	// コメントと同じ考え方)。
	const cordmux::platform::ProcessIdentity me = cordmux::platform::current_process_identity();
	const std::int64_t converted = cordmux::platform::os_process_time_to_epoch_us(me.started_at);
	CHECK(converted == cordmux::platform::current_process_started_at_us());
	CHECK(converted > 0);

	// 0 以下は「分からない」(1970 年の時刻を作らない)。
	CHECK(cordmux::platform::os_process_time_to_epoch_us(0) == 0);
	CHECK(cordmux::platform::os_process_time_to_epoch_us(-1) == 0);
}

// ---- 参加者ごとの事実と、DockFacts からコピーする値 ----

TEST_CASE("profiles の表示名が people にコピーされる")
{
	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
	snapshot.slots.push_back(screen_slot(1, "alice"));
	snapshot.facts.received = true;
	cordmux::pipeline::UserIdSet roster;
	roster.user_ids = {"alice", "bob"};
	roster.sent_count = 2;
	snapshot.facts.roster = roster;

	cordmux::ws::ProfilesMessage message;
	cordmux::ws::ProfileEntry named;
	named.user_id = "alice";
	named.display_name = "きつね";
	message.profiles.push_back(named);
	cordmux::ws::ProfileEntry nameless;
	nameless.user_id = "bob"; // 表示名を読めなかった人
	message.profiles.push_back(nameless);
	snapshot.profiles = cordmux::pipeline::make_profiles_snapshot(message, kNow);

	const cordmux::ledger::Observation obs =
		build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));

	CHECK(person_of(obs, "alice")->display_name.value_or("") == "きつね");
	// 表示名を読めない userId には名前を持たせない (画面には userId をそのまま表示する)。
	CHECK_FALSE(person_of(obs, "bob")->display_name.has_value());

	const cordmux::ledger::dockview::DockViewV2 view = cordmux::ledger::decide_dock(obs);
	REQUIRE(view.people.size() == 2);
	REQUIRE(row_of(view, "alice") != nullptr);
	REQUIRE(row_of(view, "bob") != nullptr);
	CHECK(row_of(view, "alice")->name.literal == "きつね");
	CHECK(row_of(view, "bob")->name.literal == "bob");
}

TEST_CASE("cordmux での名前は、届いた表示名より優先される")
{
	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
	snapshot.slots.push_back(screen_slot(1, "alice"));
	cordmux::ws::ProfilesMessage message;
	cordmux::ws::ProfileEntry named;
	named.user_id = "alice";
	named.display_name = "きつね";
	message.profiles.push_back(named);
	snapshot.profiles = cordmux::pipeline::make_profiles_snapshot(message, kNow);

	DockFacts facts = facts_at(kNow);
	cordmux::ledger::SeenPerson seen_row;
	seen_row.user_id = "alice";
	seen_row.display_name = "きつね";
	seen_row.name_override = "たぬき";
	facts.seen.people.push_back(seen_row);

	const cordmux::ledger::Observation obs = build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts);
	CHECK(person_of(obs, "alice")->display_name.value_or("") == "たぬき");

	// 空の cordmux での名前 (名前の変更を取り消した結果) は優先されない。
	facts.seen.people[0].name_override = "";
	const cordmux::ledger::Observation without =
		build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts);
	CHECK(person_of(without, "alice")->display_name.value_or("") == "きつね");

	// 名前を決める関数は 1 つだけ (3 か所に配る値は、この関数の結果そのもの)。
	CHECK(cordmux::dock::effective_display_name(facts.seen.people, snapshot.profiles, "alice") == "きつね");
	CHECK(cordmux::dock::effective_display_name(facts.seen.people, snapshot.profiles, "unknown-user").empty());
}

TEST_CASE("profiles に名前のない人は、最後に見たときの表示名が使われる")
{
	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
	snapshot.slots.push_back(screen_slot(1, "alice"));

	DockFacts facts = facts_at(kNow);
	cordmux::ledger::SeenPerson seen_row;
	seen_row.user_id = "alice";
	seen_row.display_name = "きつね";
	facts.seen.people.push_back(seen_row);

	const cordmux::ledger::Observation obs = build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts);
	CHECK(person_of(obs, "alice")->display_name.value_or("") == "きつね");

	// 届いた表示名があれば、そちらが先。
	cordmux::ws::ProfilesMessage message;
	cordmux::ws::ProfileEntry named;
	named.user_id = "alice";
	named.display_name = "たぬき";
	message.profiles.push_back(named);
	snapshot.profiles = cordmux::pipeline::make_profiles_snapshot(message, kNow);
	CHECK(cordmux::dock::effective_display_name(facts.seen.people, snapshot.profiles, "alice") == "たぬき");

	// cordmux での名前があれば、それが最も先。
	facts.seen.people[0].name_override = "うさぎ";
	CHECK(cordmux::dock::effective_display_name(facts.seen.people, snapshot.profiles, "alice") == "うさぎ");

	// どれもなければ空 (行は userId のまま)。見たときの表示名が空の人も同じ。
	facts.seen.people[0].name_override.reset();
	facts.seen.people[0].display_name.clear();
	cordmux::pipeline::PipelineSnapshot bare = empty_snapshot();
	bare.slots.push_back(screen_slot(1, "alice"));
	CHECK(cordmux::dock::effective_display_name(facts.seen.people, bare.profiles, "alice").empty());
	const cordmux::ledger::Observation none = build_observation(bare, cordmux::platform::OsProbeResult{}, facts);
	CHECK_FALSE(person_of(none, "alice")->display_name.has_value());
}

TEST_CASE("configure の解像度とフレームレートが、people の 3 つのフィールドにコピーされる")
{
	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
	snapshot.slots.push_back(screen_slot(1, "alice"));
	snapshot.slots.push_back(screen_slot(2, "bob"));
	snapshot.last_frames[11] = frame_clock(11, "alice", snapshot, kSec / 2);
	snapshot.last_frames[12] = frame_clock(12, "bob", snapshot, kSec / 2);

	cordmux::pipeline::StreamFormat alice_format;
	alice_format.stream_id = 11;
	alice_format.user_id = "alice";
	alice_format.screen = true;
	alice_format.width = 1280;
	alice_format.height = 720;
	alice_format.fps = 30.0;
	snapshot.stream_formats[11] = alice_format;

	// bob のストリームは configure に載っているが、まだエンコーダを確保していないので、3 つとも 0 で届く
	// (適当な値で埋めない)。
	cordmux::pipeline::StreamFormat bob_format;
	bob_format.stream_id = 12;
	bob_format.user_id = "bob";
	bob_format.screen = true;
	snapshot.stream_formats[12] = bob_format;

	const cordmux::ledger::Observation obs =
		build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));

	CHECK(person_of(obs, "alice")->width.value_or(0) == 1280);
	CHECK(person_of(obs, "alice")->height.value_or(0) == 720);
	CHECK(person_of(obs, "alice")->fps.value_or(0.0) == doctest::Approx(30.0));

	// 0 のフィールドは持たない (画面に 0x0 と表示しない)。
	CHECK_FALSE(person_of(obs, "bob")->width.has_value());
	CHECK_FALSE(person_of(obs, "bob")->height.has_value());
	CHECK_FALSE(person_of(obs, "bob")->fps.has_value());

	const cordmux::ledger::dockview::DockViewV2 view = cordmux::ledger::decide_dock(obs);
	REQUIRE(view.people.size() == 2);
	REQUIRE(row_of(view, "alice") != nullptr);
	REQUIRE(row_of(view, "bob") != nullptr);
	CHECK(row_of(view, "alice")->resolution.has_value());
	CHECK_FALSE(row_of(view, "bob")->resolution.has_value());
}

TEST_CASE("解像度とフレームレートは、段階と同じストリームから取る (食い違う 2 つを同じ行に表示しない)")
{
	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
	snapshot.slots.push_back(screen_slot(1, "alice"));
	// 同じ人にストリームが 2 本ある。新しいフレームはストリーム 21 の方。
	snapshot.last_frames[20] = frame_clock(20, "alice", snapshot, 3 * kSec);
	snapshot.last_frames[21] = frame_clock(21, "alice", snapshot, kSec / 2);
	snapshot.facts.stages = std::vector<cordmux::pipeline::StreamStage>{{20, "native"}, {21, "downscaled"}};

	cordmux::pipeline::StreamFormat old_format;
	old_format.stream_id = 20;
	old_format.width = 1920;
	old_format.height = 1080;
	old_format.fps = 60.0;
	snapshot.stream_formats[20] = old_format;
	cordmux::pipeline::StreamFormat new_format;
	new_format.stream_id = 21;
	new_format.width = 1280;
	new_format.height = 720;
	new_format.fps = 30.0;
	snapshot.stream_formats[21] = new_format;

	const cordmux::ledger::Observation obs =
		build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
	CHECK(person_of(obs, "alice")->stage.value_or("") == "downscaled");
	CHECK(person_of(obs, "alice")->width.value_or(0) == 1280);
	CHECK(person_of(obs, "alice")->fps.value_or(0.0) == doctest::Approx(30.0));
}

namespace {

// configure の画面共有のストリーム 1 本 (stream_formats の 1 行)。解像度とフレームレートの 0 は、「省かれた」を
// 表す。
cordmux::pipeline::StreamFormat screen_format(std::uint32_t stream_id, const std::string &user_id, int width = 0,
					      int height = 0, double fps = 0.0)
{
	cordmux::pipeline::StreamFormat format;
	format.stream_id = stream_id;
	format.user_id = user_id;
	format.screen = true;
	format.width = width;
	format.height = height;
	format.fps = fps;
	return format;
}

} // namespace

TEST_CASE("フレームがまだ届いていない人の段階は、configure のストリームと人の組から対応付ける")
{
	// alice: 枠に載っていて、画面共有を始めて 30 秒。最初のエンコーダの確保で使える段階が尽きた (unavailable)
	//        ので、フレームは 1 枚も届かない (src-vencord/encode/session.ts の start())。configure には
	//        ストリーム 11 が解像度なしで載る。古いストリーム 9 も残っている。
	// bob:   枠に載っていない。ストリーム 12 は downscaled で、解像度がある。
	// carol: 声のストリームだけ (声には段階がない)。user_id のない画面共有のストリーム 13 と、参加者の一覧にいない
	//        dave のストリーム 15 もある。
	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
	snapshot.slots.push_back(screen_slot(1, "alice"));
	snapshot.listener.client_connected = true;
	snapshot.facts.received = true;
	cordmux::pipeline::UserIdSet roster;
	roster.user_ids = {"alice", "bob", "carol"};
	roster.sent_count = 3;
	snapshot.facts.roster = roster;
	cordmux::pipeline::UserIdSet sharing;
	sharing.user_ids = {"alice", "bob"};
	sharing.sent_count = 2;
	snapshot.facts.sharing = sharing;
	snapshot.facts.sharing_valid = true;
	snapshot.facts.sharing_since_us["alice"] = kNow - 30 * kSec;
	snapshot.facts.sharing_since_us["bob"] = kNow - 30 * kSec;

	snapshot.stream_formats[9] = screen_format(9, "alice");
	snapshot.stream_formats[11] = screen_format(11, "alice");
	snapshot.stream_formats[12] = screen_format(12, "bob", 1280, 720, 30.0);
	snapshot.stream_formats[13] = screen_format(13, "");
	cordmux::pipeline::StreamFormat voice = screen_format(14, "carol");
	voice.screen = false;
	snapshot.stream_formats[14] = voice;
	snapshot.stream_formats[15] = screen_format(15, "dave");
	snapshot.facts.stages = std::vector<cordmux::pipeline::StreamStage>{
		{9, "native"}, {11, "unavailable"}, {12, "downscaled"}, {13, "native"}, {14, "native"}, {15, "native"}};

	const cordmux::ledger::Observation obs =
		build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));

	SUBCASE("段階は configure のストリームの user_id の人に付き、同じ人にストリームが 2 本あれば番号の大きい (後から確保した) 方を使う")
	{
		REQUIRE(person_of(obs, "alice") != nullptr);
		CHECK_FALSE(person_of(obs, "alice")->last_video_frame_at_us.has_value());
		CHECK(person_of(obs, "alice")->stage.value_or("") == "unavailable");
		// unavailable の間は送信側が解像度を省くので、解像度は付かない。
		CHECK_FALSE(person_of(obs, "alice")->width.has_value());
		CHECK_FALSE(person_of(obs, "alice")->fps.has_value());
	}

	SUBCASE("枠に載っていない人にも段階が付き、解像度は段階と同じストリームから取る")
	{
		REQUIRE(person_of(obs, "bob") != nullptr);
		CHECK(person_of(obs, "bob")->stage.value_or("") == "downscaled");
		CHECK(person_of(obs, "bob")->width.value_or(0) == 1280);
		CHECK(person_of(obs, "bob")->height.value_or(0) == 720);
		CHECK(person_of(obs, "bob")->fps.value_or(0.0) == doctest::Approx(30.0));
	}

	SUBCASE("声のストリームと user_id のないストリームには段階を付けず、configure にしかいない人の行は作らない")
	{
		REQUIRE(person_of(obs, "carol") != nullptr);
		CHECK_FALSE(person_of(obs, "carol")->stage.has_value());
		CHECK(person_of(obs, "dave") == nullptr);
		CHECK(obs.people.size() == 3);
	}

	SUBCASE("1 人だけなのでメッセージにせず、その人の行は「映像を表示できません」になる")
	{
		const cordmux::ledger::dockview::DockViewV2 view = cordmux::ledger::decide_dock(obs);
		for (const cordmux::ledger::dockview::Notice &notice : view.notices)
			CHECK(notice.rule_name != "AllVideoStoppedHint");
		const cordmux::ledger::dockview::PersonRow *alice = row_of(view, "alice");
		REQUIRE(alice != nullptr);
		CHECK_FALSE(alice->status.has_value());
		REQUIRE(alice->quality.has_value());
		CHECK(alice->quality->id == cordmux::ledger::text::keys::kDockRowCantShowStatus);
	}
}

TEST_CASE("フレームが届いている人の段階は、フレームが届いているストリームから取り、configure のストリームでは上書きしない")
{
	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
	snapshot.slots.push_back(screen_slot(1, "alice"));
	snapshot.last_frames[21] = frame_clock(21, "alice", snapshot, kSec / 2);
	snapshot.stream_formats[21] = screen_format(21, "alice", 1920, 1080, 60.0);
	snapshot.stream_formats[25] = screen_format(25, "alice");
	snapshot.facts.stages = std::vector<cordmux::pipeline::StreamStage>{{21, "native"}, {25, "unavailable"}};

	cordmux::ledger::Observation obs = build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
	CHECK(person_of(obs, "alice")->stage.value_or("") == "native");
	CHECK(person_of(obs, "alice")->width.value_or(0) == 1920);

	// フレームが届いているストリームに段階の報告がなければ、段階を付けない (configure のストリームの段階で
	// 埋めない)。
	snapshot.facts.stages = std::vector<cordmux::pipeline::StreamStage>{{25, "unavailable"}};
	obs = build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
	CHECK_FALSE(person_of(obs, "alice")->stage.has_value());
}

TEST_CASE("画面共有している人が、people の sharing にコピーされる")
{
	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
	snapshot.slots.push_back(screen_slot(1, "alice"));
	snapshot.facts.received = true;
	cordmux::pipeline::UserIdSet roster;
	roster.user_ids = {"alice", "bob"};
	roster.sent_count = 2;
	snapshot.facts.roster = roster;
	cordmux::pipeline::UserIdSet sharing;
	sharing.user_ids = {"alice"};
	sharing.sent_count = 1;
	snapshot.facts.sharing = sharing;

	const cordmux::ledger::Observation obs =
		build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
	CHECK(person_of(obs, "alice")->sharing);
	CHECK_FALSE(person_of(obs, "bob")->sharing);
}

TEST_CASE("roster にいる人だけが in_roster になり、枠だけが残っているメインアカウントについては、通話に参加していないことを伝える")
{
	// 画面共有の枠と声の枠に自動で載っていたメインアカウント (999) が、通話から退出した形。枠は載せたままなので、
	// 999 の行は people に残るが、roster には残りの 3 人だけがいる。people にいるかで判断すると、通話に参加している
	// 人数 (3 人) を表示する横で、メッセージが表示されない (ledger/observation.h の PersonFact::in_roster の
	// コメント)。
	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
	snapshot.slots.push_back(screen_slot(1, "999"));
	cordmux::pipeline::SlotSnapshot voice;
	voice.kind = cordmux::config::SlotKind::Voice;
	voice.slot = 1;
	voice.owner_user_id = "999";
	voice.voice_user_id = "999";
	snapshot.slots.push_back(voice);
	snapshot.facts.received = true;
	cordmux::pipeline::UserIdSet roster;
	roster.user_ids = {"alice", "bob", "carol"};
	roster.sent_count = 3;
	snapshot.facts.roster = roster;

	DockFacts facts = facts_at(kNow);
	facts.broadcaster_user_id = "999";

	const auto broadcaster_notice_count = [](const cordmux::ledger::Observation &obs) {
		int count = 0;
		for (const cordmux::ledger::dockview::Notice &notice : cordmux::ledger::decide_dock(obs).notices) {
			if (notice.rule_name == "BroadcasterNotInCall")
				++count;
		}
		return count;
	};

	const cordmux::ledger::Observation left =
		build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts);
	CHECK(left.call.presence == cordmux::ledger::Presence::InCall);
	CHECK(left.call.roster_count.value_or(-1) == 3);
	// 枠に載っている人は people に残るが、通話に参加していることのフラグは立たない。
	REQUIRE(person_of(left, "999") != nullptr);
	CHECK(person_of(left, "999")->slot.value_or(0) == 1);
	CHECK_FALSE(person_of(left, "999")->in_roster);
	REQUIRE(person_of(left, "alice") != nullptr);
	CHECK(person_of(left, "alice")->in_roster);
	CHECK(broadcaster_notice_count(left) == 1);

	// 戻れば (roster に入れば) 表示しない。枠に載ったままでも同じ。
	snapshot.facts.roster->user_ids.push_back("999");
	snapshot.facts.roster->sent_count = 4;
	const cordmux::ledger::Observation back =
		build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts);
	REQUIRE(person_of(back, "999") != nullptr);
	CHECK(person_of(back, "999")->in_roster);
	CHECK(person_of(back, "999")->slot.value_or(0) == 1);
	CHECK(broadcaster_notice_count(back) == 0);

	// roster のキーが省かれている間は、誰にもフラグを立てず、メッセージも表示しない。
	snapshot.facts.roster.reset();
	const cordmux::ledger::Observation unknown =
		build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts);
	REQUIRE(person_of(unknown, "999") != nullptr);
	CHECK_FALSE(person_of(unknown, "999")->in_roster);
	CHECK(broadcaster_notice_count(unknown) == 0);
}

TEST_CASE("facts.self_user_id の人だけが is_receiver になり、Vesktop のアカウントのためだけに行は作らない")
{
	// Vesktop のアカウント (Vesktop にログインしている Discord のアカウント) は roster に入る。is_receiver は、
	// 使う声のメニューから除くためのフラグ (ledger/observation.h の PersonFact::is_receiver)。
	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
	snapshot.facts.received = true;
	cordmux::pipeline::UserIdSet roster;
	roster.user_ids = {"alice", "me"};
	roster.sent_count = 2;
	snapshot.facts.roster = roster;
	snapshot.facts.self_user_id = "me";

	const cordmux::ledger::Observation obs =
		build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
	REQUIRE(person_of(obs, "me") != nullptr);
	CHECK(person_of(obs, "me")->is_receiver);
	CHECK(person_of(obs, "me")->in_roster); // Vesktop のアカウントも通話に参加している 1 人のまま (人数も変えない)
	REQUIRE(person_of(obs, "alice") != nullptr);
	CHECK_FALSE(person_of(obs, "alice")->is_receiver);
	CHECK(obs.call.roster_count.value_or(-1) == 2);

	// 送信側が self_user_id を送ってこない間 (まだ分からないとき、古い送信側) は、誰にもフラグを立てない。
	snapshot.facts.self_user_id.reset();
	const cordmux::ledger::Observation unknown =
		build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
	REQUIRE(person_of(unknown, "me") != nullptr);
	CHECK_FALSE(person_of(unknown, "me")->is_receiver);

	// roster にも枠にもいない Vesktop のアカウントの行は作らない (通話に参加していない人を、参加者の一覧に
	// 表示しない)。
	snapshot.facts.roster->user_ids = {"alice"};
	snapshot.facts.roster->sent_count = 1;
	snapshot.facts.self_user_id = "me";
	const cordmux::ledger::Observation absent =
		build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
	CHECK(person_of(absent, "me") == nullptr);
	REQUIRE(person_of(absent, "alice") != nullptr);
	CHECK_FALSE(person_of(absent, "alice")->is_receiver);
}

TEST_CASE("Vesktop のアカウントの userId と名前は、通話の外でも Observation に入り、切れた間は最後に分かった値を使う")
{
	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
	snapshot.facts.received = true;
	snapshot.facts.self_user_id = "me";
	cordmux::ws::ProfilesMessage message;
	cordmux::ws::ProfileEntry self_entry;
	self_entry.user_id = "me";
	self_entry.display_name = "サブ";
	message.profiles.push_back(self_entry);
	snapshot.profiles = cordmux::pipeline::make_profiles_snapshot(message, kNow);

	// roster も枠もなく、通話の外にいる。
	const cordmux::ledger::Observation outside =
		build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
	CHECK(outside.vesktop_user_id == "me");
	CHECK(outside.vesktop_name == "サブ");
	CHECK(person_of(outside, "me") == nullptr);

	// 接続が切れて facts が空になった間は、DockFacts の最後に分かった値を使う。
	cordmux::pipeline::PipelineSnapshot cut = empty_snapshot();
	DockFacts facts = facts_at(kNow);
	facts.last_known_vesktop_user_id = "me";
	facts.last_known_vesktop_name = "サブ";
	const cordmux::ledger::Observation held = build_observation(cut, cordmux::platform::OsProbeResult{}, facts);
	CHECK(held.vesktop_user_id == "me");
	CHECK(held.vesktop_name == "サブ");
}

TEST_CASE("自分の枠の人の行だけが is_self_frame になる (メインアカウントの userId があればその人、なければ Vesktop のアカウント)")
{
	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
	snapshot.facts.received = true;
	cordmux::pipeline::UserIdSet roster;
	roster.user_ids = {"alice", "main", "me"};
	roster.sent_count = 3;
	snapshot.facts.roster = roster;
	snapshot.facts.self_user_id = "me";

	// メインアカウントの userId を設定している (サブアカウントを使う構成): メインアカウントの行。
	DockFacts separate = facts_at(kNow);
	separate.broadcaster_user_id = "main";
	const cordmux::ledger::Observation with_main =
		build_observation(snapshot, cordmux::platform::OsProbeResult{}, separate);
	REQUIRE(person_of(with_main, "main") != nullptr);
	CHECK(person_of(with_main, "main")->is_self_frame);
	CHECK_FALSE(person_of(with_main, "me")->is_self_frame);
	CHECK_FALSE(person_of(with_main, "alice")->is_self_frame);

	// 空 (メインアカウントだけの構成): Vesktop のアカウントの行。
	const cordmux::ledger::Observation solo =
		build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
	CHECK(person_of(solo, "me")->is_self_frame);
	CHECK(person_of(solo, "me")->is_receiver);
	CHECK_FALSE(person_of(solo, "main")->is_self_frame);

	// どちらも分からない間は、誰も自分の枠の人にしない。
	snapshot.facts.self_user_id.reset();
	const cordmux::ledger::Observation unknown =
		build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
	for (const cordmux::ledger::PersonFact &person : unknown.people)
		CHECK_FALSE(person.is_self_frame);
}

TEST_CASE("自分の枠、メインアカウントのフィールドと seen は、DockFacts からそのままコピーされる")
{
	DockFacts facts = facts_at(kNow);
	facts.self_frame_shown = true;
	facts.self_frame_selected = true;
	facts.self_frame_bound = true;
	facts.self_frame_on = true;
	facts.self_frame_share = cordmux::ledger::SelfShareState::Off;
	facts.broadcaster_user_id = "999";
	facts.dummy = cordmux::ledger::FeatureSwitch::On;
	cordmux::ledger::SeenPerson seen_row;
	seen_row.user_id = "alice";
	seen_row.display_name = "きつね";
	seen_row.last_seen_at_us = kNow - 60 * kSec;
	facts.seen.people.push_back(seen_row);
	facts.seen.discarded_with_marks = 2;
	facts.assign_mode = cordmux::ledger::AssignMode::Auto;

	const cordmux::ledger::Observation obs =
		build_observation(empty_snapshot(), cordmux::platform::OsProbeResult{}, facts);
	CHECK(obs.choices.self_frame_shown);
	CHECK(obs.choices.self_frame_selected);
	CHECK(obs.choices.self_frame_bound);
	CHECK(obs.choices.self_frame_on);
	CHECK(obs.choices.self_frame_share == cordmux::ledger::SelfShareState::Off);
	CHECK(obs.choices.broadcaster_user_id == "999");
	CHECK(obs.choices.dummy == cordmux::ledger::FeatureSwitch::On);
	// 誰を枠に載せるか。既定は手動なので、コピーし忘れるとこのテストが失敗する。
	CHECK(obs.choices.assign_mode == cordmux::ledger::AssignMode::Auto);
	REQUIRE(obs.seen.people.size() == 1);
	CHECK(obs.seen.people[0].user_id == "alice");
	CHECK(obs.seen.discarded_with_marks == 2);

	// 一覧は表示の規則の側が作る (通話に参加していない人なので、参加者の設定ウィンドウの「過去の参加者」に
	// 表示される)。
	const cordmux::ledger::dockview::DockViewV2 view = cordmux::ledger::decide_dock(obs);
	REQUIRE(view.participants.size() == 1);
	CHECK_FALSE(view.participants[0].in_call);
	CHECK(view.participants[0].name.literal == "きつね");
	// 設定 (cordmux での名前など) を持つ行を捨てたことは、メッセージのレベル 3 の行で伝える。
	int discarded = 0;
	for (const cordmux::ledger::dockview::Notice &notice : view.notices) {
		if (notice.rule_name == "SeenDiscarded")
			++discarded;
	}
	CHECK(discarded == 1);
}

TEST_CASE("OBS、Windows、GPU のバージョンは、DockFacts からそのままコピーされる")
{
	DockFacts facts = facts_at(kNow);
	facts.obs_version = "31.1.1";
	facts.os_version = "10.0.12345.6789";
	facts.gpu_name = "test-device";

	const cordmux::ledger::Observation obs =
		build_observation(empty_snapshot(), cordmux::platform::OsProbeResult{}, facts);
	CHECK(obs.versions.obs == "31.1.1");
	CHECK(obs.versions.windows == "10.0.12345.6789");
	CHECK(obs.versions.gpu == "test-device");

	// 読めていない値 (既定の DockFacts) は空のまま渡り、画面の "-" は decide_dock が入れる。
	const cordmux::ledger::Observation blank =
		build_observation(empty_snapshot(), cordmux::platform::OsProbeResult{}, facts_at(kNow));
	CHECK(blank.versions.obs.empty());
	CHECK(blank.versions.windows.empty());
	CHECK(blank.versions.gpu.empty());
	const cordmux::ledger::dockview::DockViewV2 view = cordmux::ledger::decide_dock(blank);
	// バージョンの 7 行 (設定ウィンドウの「バージョン情報」のページ)。
	REQUIRE(view.about.versions.size() == 7);
	CHECK(version_value(view, keys::kSettingsVersionObsLabel) == "-");
	CHECK(version_value(view, keys::kSettingsVersionWindowsLabel) == "-");
	CHECK(version_value(view, keys::kSettingsVersionGpuLabel) == "-");
	CHECK(version_value(cordmux::ledger::decide_dock(obs), keys::kSettingsVersionObsLabel) == "31.1.1");
}

TEST_CASE("依存箇所の「最後に読めていた時刻」は、読めない依存箇所の一覧から消えた周期で記録する")
{
	std::map<std::string, std::int64_t> last_ok;
	std::vector<cordmux::ledger::ContactFact> contacts;
	cordmux::ledger::ContactFact missing;
	missing.id = "pc-hook";
	missing.state = cordmux::ledger::ContactState::Missing;
	contacts.push_back(missing);

	// facts が一度も届いていない間は、何も記録しない。
	cordmux::dock::update_contact_success(&last_ok, contacts, false, kNow);
	CHECK(last_ok.empty());

	// 読めない依存箇所として挙がっている間は、「一度も読めていない」(0) のまま一覧に入る。
	cordmux::dock::update_contact_success(&last_ok, contacts, true, kNow);
	REQUIRE(last_ok.count("pc-hook") == 1);
	CHECK(last_ok["pc-hook"] == 0);

	// 一覧から消えたら、そのときは読めていたということ。
	cordmux::dock::update_contact_success(&last_ok, {}, true, kNow + kSec);
	CHECK(last_ok["pc-hook"] == kNow + kSec);

	// もう一度読めなくなっても、最後に読めていた時刻は巻き戻らない。
	cordmux::dock::update_contact_success(&last_ok, contacts, true, kNow + 2 * kSec);
	CHECK(last_ok["pc-hook"] == kNow + kSec);
}

TEST_CASE("今読めない依存箇所の集合は 1 つの関数で作る (空の id は数えない)")
{
	std::vector<cordmux::ledger::ContactFact> contacts;
	cordmux::ledger::ContactFact named;
	named.id = "pc-hook";
	named.state = cordmux::ledger::ContactState::Missing;
	contacts.push_back(named);
	cordmux::ledger::ContactFact nameless;
	nameless.state = cordmux::ledger::ContactState::Missing;
	contacts.push_back(nameless);

	const std::set<std::string> ids = cordmux::dock::unavailable_contact_ids(contacts);
	CHECK(ids.size() == 1);
	CHECK(ids.count("pc-hook") == 1);
	CHECK(ids.count("") == 0);

	// 時刻を記録する側も同じ集合を使う (診断情報の manifest.json の state と食い違わないように)。
	std::map<std::string, std::int64_t> last_ok;
	cordmux::dock::update_contact_success(&last_ok, contacts, true, kNow);
	CHECK(last_ok.size() == 1);
	CHECK(last_ok.count("pc-hook") == 1);
}

// ---- 録画の事実 (recording) ----

TEST_CASE("録画のオンとオフは、Pipeline のスナップショットだけから決まる")
{
	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();

	// 起動した直後はオフ (起動し直すとオフに戻る)。Unknown にはならない。自分のメモリの値なので、「まだ読めて
	// いない」という状態がないため。
	CHECK(build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow)).choices.recording ==
	      cordmux::ledger::FeatureSwitch::Off);

	snapshot.recording.enabled = true;
	CHECK(build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow)).choices.recording ==
	      cordmux::ledger::FeatureSwitch::On);
}

TEST_CASE("録画の事実は、スナップショットのフィールドからそのまま recording にコピーされる")
{
	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
	snapshot.recording.enabled = true;
	snapshot.recording.started_at_wall_us = kNow - 30 * kSec;
	snapshot.recording.segment_count = 5;
	snapshot.recording.open_segment_count = 2;
	snapshot.recording.bytes_written = 1234;
	snapshot.recording.free_space_bytes = 900ULL * 1000ULL * 1000ULL;
	snapshot.recording.remaining_seconds = 420;
	snapshot.recording.finalize_need_bytes = 3ULL * 1000ULL * 1000ULL * 1000ULL;
	snapshot.recording.finalize.state = cordmux::record::FinalizeState::Running;
	snapshot.recording.finalize.branch = cordmux::record::FinalizeBranch::ProRes;
	snapshot.recording.finalize.files_total = 6;
	snapshot.recording.finalize.files_done = 2;
	snapshot.recording.finalize.ffmpeg_unavailable = true;
	snapshot.recording.pending_present = true;
	snapshot.recording.pending_session_count = 3;
	snapshot.recording.failed_session_dir = "D:/rec/cordmux/2000-01-01 12-34-56";
	snapshot.sender_recording = false;
	snapshot.settings.record_space_warn_minutes = 45;

	const cordmux::ledger::Observation obs =
		build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
	const cordmux::ledger::RecordingFacts &rec = obs.recording;

	// セグメントの数は「閉じたセグメント + 今開いているセグメント」。
	CHECK(rec.segment_count.value_or(0) == 7);
	CHECK(rec.bytes_written.value_or(0) == 1234);
	CHECK(rec.free_space_bytes.value_or(0) == 900ULL * 1000ULL * 1000ULL);
	CHECK(rec.remaining_seconds.value_or(0) == 420);
	CHECK(rec.finalize_need_bytes.value_or(0) == 3ULL * 1000ULL * 1000ULL * 1000ULL);
	CHECK(rec.finalize == cordmux::ledger::FinalizeState::Running);
	CHECK(rec.branch == cordmux::ledger::FinalizeBranch::ProRes);
	CHECK(rec.finalize_files_total == 6);
	CHECK(rec.finalize_files_done == 2);
	CHECK(rec.finalize_ffmpeg_unavailable);
	CHECK(rec.pending_present);
	CHECK(rec.pending_session_count == 3);
	CHECK(rec.failed_session_dir == "D:/rec/cordmux/2000-01-01 12-34-56");
	CHECK(rec.sender_recording.has_value());
	CHECK_FALSE(*rec.sender_recording);
	CHECK(rec.enabled_since_us.value_or(0) == kNow - 30 * kSec);
	// しきい値は、settings の分を秒に換算した値。
	CHECK(rec.space_warn_seconds == 45 * 60);
	// 保存先に書き込めたかどうかも、スナップショットからそのままコピーされる (値を持たないのは、まだ確認していない
	// とき)。
	CHECK_FALSE(rec.save_dir_writable.has_value());
}

TEST_CASE("保存先に書き込めなかったことは、recording にそのままコピーされる")
{
	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
	snapshot.recording.save_dir_writable = false;
	const cordmux::ledger::Observation obs =
		build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
	REQUIRE(obs.recording.save_dir_writable.has_value());
	CHECK_FALSE(*obs.recording.save_dir_writable);

	snapshot.recording.save_dir_writable = true;
	const cordmux::ledger::Observation ok =
		build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
	REQUIRE(ok.recording.save_dir_writable.has_value());
	CHECK(*ok.recording.save_dir_writable);
}

TEST_CASE("録画の処理が自分で止まったときは、recording に true としてコピーされる")
{
	// 止まっていないとき (値を持たないのは、自分では止まっていないとき)。
	CHECK_FALSE(build_observation(empty_snapshot(), cordmux::platform::OsProbeResult{}, facts_at(kNow))
			    .recording.stopped_by_space_exhausted);

	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
	snapshot.recording.self_stopped_reason = cordmux::record::StopReason::SpaceExhausted;
	CHECK(build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow))
		      .recording.stopped_by_space_exhausted);

	// 利用者がオフにしたときと、OBS の終了は含めない (止めた側が既に知っているため)。
	snapshot.recording.self_stopped_reason = cordmux::record::StopReason::TurnedOff;
	CHECK_FALSE(build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow))
			    .recording.stopped_by_space_exhausted);
}

TEST_CASE("空き容量は、録画の処理が持っていないときだけ、OS への問い合わせの結果で埋める")
{
	cordmux::platform::OsProbeResult probe;
	probe.free_space_bytes = 500ULL * 1000ULL * 1000ULL;

	// 録画していない間 (録画の処理は空き容量をまだ調べていない)。
	const cordmux::ledger::Observation idle = build_observation(empty_snapshot(), probe, facts_at(kNow));
	CHECK(idle.recording.free_space_bytes.value_or(0) == 500ULL * 1000ULL * 1000ULL);

	// 録画中は録画の処理の値が新しいので、そちらを使う。
	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
	snapshot.recording.free_space_bytes = 900ULL * 1000ULL * 1000ULL;
	const cordmux::ledger::Observation running = build_observation(snapshot, probe, facts_at(kNow));
	CHECK(running.recording.free_space_bytes.value_or(0) == 900ULL * 1000ULL * 1000ULL);
}

TEST_CASE("読めていない録画のフィールドは、値を持たないまま渡る")
{
	const cordmux::ledger::Observation obs =
		build_observation(empty_snapshot(), cordmux::platform::OsProbeResult{}, facts_at(kNow));
	const cordmux::ledger::RecordingFacts &rec = obs.recording;

	CHECK_FALSE(rec.free_space_bytes.has_value());
	CHECK_FALSE(rec.remaining_seconds.has_value());
	CHECK_FALSE(rec.finalize_need_bytes.has_value());
	CHECK_FALSE(rec.sender_recording.has_value());
	// 一度もオンにしていないときは、「いつオンにしたか」を持たない (0 を時刻にしない)。
	CHECK_FALSE(rec.enabled_since_us.has_value());
	CHECK(rec.finalize == cordmux::ledger::FinalizeState::NotStarted);
	CHECK(rec.branch == cordmux::ledger::FinalizeBranch::Unknown);
	CHECK_FALSE(rec.pending_present);
}

TEST_CASE("録画ファイルの作成の進み具合と方法は、record:: のすべての値が 1 対 1 でコピーされる")
{
	struct StateCase {
		cordmux::record::FinalizeState from;
		cordmux::ledger::FinalizeState to;
	};
	const StateCase states[] = {
		{cordmux::record::FinalizeState::NotStarted, cordmux::ledger::FinalizeState::NotStarted},
		{cordmux::record::FinalizeState::Running, cordmux::ledger::FinalizeState::Running},
		{cordmux::record::FinalizeState::Done, cordmux::ledger::FinalizeState::Done},
		{cordmux::record::FinalizeState::Pending, cordmux::ledger::FinalizeState::Pending},
		{cordmux::record::FinalizeState::Failed, cordmux::ledger::FinalizeState::Failed},
	};
	for (const StateCase &one : states) {
		cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
		snapshot.recording.finalize.state = one.from;
		CHECK(build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow)).recording.finalize ==
		      one.to);
	}

	struct BranchCase {
		cordmux::record::FinalizeBranch from;
		cordmux::ledger::FinalizeBranch to;
	};
	const BranchCase branches[] = {
		{cordmux::record::FinalizeBranch::Remux, cordmux::ledger::FinalizeBranch::Remux},
		{cordmux::record::FinalizeBranch::ProRes, cordmux::ledger::FinalizeBranch::ProRes},
	};
	for (const BranchCase &one : branches) {
		cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
		snapshot.recording.finalize.branch = one.from;
		CHECK(build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow)).recording.branch ==
		      one.to);
	}
}

TEST_CASE("録画の入力を通ったフレームは、枠に載っていない人の行にもコピーされる")
{
	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
	// 枠にも経路にも現れない人 (載せ方の既定は手動なので、これが普通の形)。
	cordmux::pipeline::RecordOnlyFrameClock clock;
	clock.stream_id = 7;
	clock.user_id = "u9";
	clock.screen = true;
	clock.last_frame_steady_us = snapshot.taken_at_steady_us - kSec / 2;
	clock.last_frame_wall_us = snapshot.taken_at_wall_us - kSec / 2;
	snapshot.record_only_frames[7] = clock;

	const cordmux::ledger::Observation obs =
		build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
	const cordmux::ledger::PersonFact *person = person_of(obs, "u9");
	REQUIRE(person != nullptr);
	// 経路のあるストリームのフィールド (last_video_frame_at_us) には入らない。入るのは録画のフィールドだけ。
	CHECK_FALSE(person->last_video_frame_at_us.has_value());
	REQUIRE(person->last_record_frame_at_us.has_value());
	CHECK(*person->last_record_frame_at_us == kNow - kSec / 2);
	CHECK_FALSE(person->slot.has_value());
}

TEST_CASE("同じ人に画面共有のストリームが複数あれば、録画のフィールドはいちばん新しいフレームを使う")
{
	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
	for (int i = 0; i < 2; ++i) {
		cordmux::pipeline::RecordOnlyFrameClock clock;
		clock.stream_id = static_cast<std::uint32_t>(10 + i);
		clock.user_id = "u9";
		clock.screen = true;
		clock.last_frame_steady_us = snapshot.taken_at_steady_us - (i == 0 ? 5 * kSec : kSec);
		clock.last_frame_wall_us = snapshot.taken_at_wall_us - (i == 0 ? 5 * kSec : kSec);
		snapshot.record_only_frames[clock.stream_id] = clock;
	}

	const cordmux::ledger::Observation obs =
		build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
	const cordmux::ledger::PersonFact *person = person_of(obs, "u9");
	REQUIRE(person != nullptr);
	CHECK(person->last_record_frame_at_us.value_or(0) == kNow - kSec);
}

TEST_CASE("声のストリームは録画のフィールドに入れない (声の枠だけの人を、録画のためだけに受け取っているとしないため)")
{
	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
	cordmux::pipeline::RecordOnlyFrameClock voice;
	voice.stream_id = 21;
	voice.user_id = "u9";
	voice.screen = false;
	voice.last_frame_steady_us = snapshot.taken_at_steady_us - kSec / 2;
	voice.last_frame_wall_us = snapshot.taken_at_wall_us - kSec / 2;
	snapshot.record_only_frames[21] = voice;

	const cordmux::ledger::Observation obs =
		build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
	const cordmux::ledger::PersonFact *person = person_of(obs, "u9");
	REQUIRE(person != nullptr); // 一覧には入る (届いてはいるので)
	CHECK_FALSE(person->last_record_frame_at_us.has_value());

	// 画面共有のストリームが届いた時点で、フィールドに値が入る。
	cordmux::pipeline::RecordOnlyFrameClock screen = voice;
	screen.stream_id = 22;
	screen.screen = true;
	snapshot.record_only_frames[22] = screen;
	const cordmux::ledger::Observation with_screen =
		build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
	CHECK(person_of(with_screen, "u9")->last_record_frame_at_us.value_or(0) == kNow - kSec / 2);
}

// ---- Vesktop への cordmux の追加、利用者の選択、同梱の Vencord、アンインストール ----

namespace {

// 同梱の場所を指していて、4 つのファイルの照合が「違う」になった問い合わせの結果。
cordmux::platform::OsProbeResult overwritten_probe()
{
	cordmux::platform::OsProbeResult probe;
	probe.probed = true;
	probe.probed_at_us = kNow - kSec;
	probe.state_json_read = true;
	probe.vencord_dir = "C:/obs/plugins/cordmux/data/vencord";
	probe.points_at_bundle = true;
	probe.bundle_hash.state = cordmux::platform::BundleState::Mismatch;
	probe.bundle_hash.missing = {"vencordDesktopRenderer.css"};
	probe.bundle_hash.differing = {"vencordDesktopMain.js"};
	return probe;
}

} // namespace

TEST_CASE("上書きされたファイルの名前は、照合で違ったファイルから決まり、上書きとするときだけ入る")
{
	const cordmux::ledger::Observation obs = build_observation(empty_snapshot(), overwritten_probe(), facts_at(kNow));
	CHECK(obs.install.dir == cordmux::ledger::VencordDirState::HashMismatch);
	// 並びは照合の順 (platform::kVencordBundleFiles。読めなかったものと違ったものを 1 つに並べる)。
	REQUIRE(obs.install.mismatched_files.size() == 2);
	CHECK(obs.install.mismatched_files[0] == "vencordDesktopMain.js");
	CHECK(obs.install.mismatched_files[1] == "vencordDesktopRenderer.css");

	// 別の場所を指しているときは上書きとしないので、名前も入れない。
	cordmux::platform::OsProbeResult elsewhere = overwritten_probe();
	elsewhere.points_at_bundle = false;
	const cordmux::ledger::Observation elsewhere_obs = build_observation(empty_snapshot(), elsewhere, facts_at(kNow));
	CHECK(elsewhere_obs.install.dir == cordmux::ledger::VencordDirState::PointsElsewhere);
	CHECK(elsewhere_obs.install.mismatched_files.empty());
}

TEST_CASE("以前のバージョンの同梱のファイルをコピーし直した直後は、コピーする前の照合の結果で「上書き」としない")
{
	DockFacts facts = facts_at(kNow);
	facts.hide_hash_mismatch = true;
	const cordmux::ledger::Observation obs = build_observation(empty_snapshot(), overwritten_probe(), facts);
	// 「まだ確認していない」にする (次の照合を待っている)。名前も入れない。
	CHECK(obs.install.dir == cordmux::ledger::VencordDirState::Unknown);
	CHECK(obs.install.mismatched_files.empty());

	// 一致している照合の結果には適用されない (隠すのは「違う」だけ)。
	cordmux::platform::OsProbeResult matches = overwritten_probe();
	matches.bundle_hash.state = cordmux::platform::BundleState::Matches;
	matches.bundle_hash.missing.clear();
	matches.bundle_hash.differing.clear();
	CHECK(build_observation(empty_snapshot(), matches, facts).install.dir ==
	      cordmux::ledger::VencordDirState::Matches);
}

TEST_CASE("起動したときの書き直しと、[再インストール] でコピーできなかったファイルの名前は、DockFacts から install にコピーされる")
{
	DockFacts facts = facts_at(kNow);
	facts.rewrote_at_startup = true;
	facts.reinstall_failed_files = {"vencordDesktopMain.js"};
	const cordmux::ledger::Observation obs =
		build_observation(empty_snapshot(), cordmux::platform::OsProbeResult{}, facts);
	CHECK(obs.install.rewrote_at_startup);
	REQUIRE(obs.install.reinstall_failed_files.size() == 1);
	CHECK(obs.install.reinstall_failed_files[0] == "vencordDesktopMain.js");

	// 既定は「書き直していない」「コピーできなかったファイルはない」。
	const cordmux::ledger::Observation plain =
		build_observation(empty_snapshot(), cordmux::platform::OsProbeResult{}, facts_at(kNow));
	CHECK_FALSE(plain.install.rewrote_at_startup);
	CHECK(plain.install.reinstall_failed_files.empty());
}

TEST_CASE("この起動では Vesktop の設定を書き込まないことを表すフラグは、DockFacts から install にコピーされ、問い合わせの前でもコピーされる")
{
	DockFacts facts = facts_at(kNow);
	facts.vesktop_writes_off = true;
	// 問い合わせが一度も終わっていないスナップショットでもコピーする (起動したときの設定の値で、照合の結果に
	// よらない)。
	CHECK(build_observation(empty_snapshot(), cordmux::platform::OsProbeResult{}, facts).install.vesktop_writes_off);
	CHECK(build_observation(empty_snapshot(), overwritten_probe(), facts).install.vesktop_writes_off);
	// 既定は書き込む (フラグがない)。
	CHECK_FALSE(build_observation(empty_snapshot(), cordmux::platform::OsProbeResult{}, facts_at(kNow))
			    .install.vesktop_writes_off);
}

TEST_CASE("cordmux の有効と無効、録画の 2 つのフィールド、画質の上限は、DockFacts から choices にコピーされる")
{
	DockFacts facts = facts_at(kNow);
	facts.feature = cordmux::ledger::FeatureSwitch::Off;
	facts.record_dir = "D:/rec";
	facts.record_output_pcm = cordmux::ledger::RecordPcm::Int24;
	facts.limit_max_long_edge = 1280;
	facts.limit_fps_divisor = 2;
	const cordmux::ledger::Observation obs =
		build_observation(empty_snapshot(), cordmux::platform::OsProbeResult{}, facts);
	CHECK(obs.choices.feature == cordmux::ledger::FeatureSwitch::Off);
	CHECK(obs.choices.record_dir == "D:/rec");
	CHECK(obs.choices.record_output_pcm == cordmux::ledger::RecordPcm::Int24);
	CHECK(obs.choices.limit_max_long_edge == 1280);
	CHECK(obs.choices.limit_fps_divisor == 2);
	// cordmux を無効にした Observation では、主な規則 (primary_rule_of) が FeatureOff になる。
	CHECK(cordmux::ledger::primary_rule_of(obs) == cordmux::ledger::PrimaryRuleId::FeatureOff);
}

TEST_CASE("同梱の Vencord のバージョン、コミット、タグは、DockFacts から versions にコピーされる")
{
	DockFacts facts = facts_at(kNow);
	facts.bundled_vencord_version = "1.2.3+abc1234";
	facts.bundled_vencord_commit = "0123456789abcdef0123456789abcdef01234567";
	facts.bundled_vencord_tag = "v1.15.9";
	const cordmux::ledger::Observation obs =
		build_observation(empty_snapshot(), cordmux::platform::OsProbeResult{}, facts);
	CHECK(obs.versions.bundled_vencord == "1.2.3+abc1234");
	CHECK(obs.versions.bundled_vencord_commit == "0123456789abcdef0123456789abcdef01234567");
	CHECK(obs.versions.bundled_vencord_tag == "v1.15.9");
	CHECK(obs.versions.plugin == "1.2.3");

	// cordmux-build.txt を読めなければ空のまま (画面は "-" を表示し、バージョンの食い違いのメッセージは表示しない)。
	const cordmux::ledger::Observation unread =
		build_observation(empty_snapshot(), cordmux::platform::OsProbeResult{}, facts_at(kNow));
	CHECK(unread.versions.bundled_vencord.empty());
	CHECK(unread.versions.bundled_vencord_commit.empty());
	CHECK(unread.versions.bundled_vencord_tag.empty());
}

TEST_CASE("アンインストールの進み具合と結果は、DockFacts から uninstall にそのままコピーされる")
{
	DockFacts facts = facts_at(kNow);
	facts.uninstall.phase = cordmux::ledger::UninstallPhase::Stopped;
	facts.uninstall.obs_streaming = false;
	facts.uninstall.obs_recording = true;
	facts.uninstall.folder_to_delete = "C:/obs/plugins/cordmux";
	facts.uninstall.record_dir = "D:/rec";
	facts.uninstall.leftovers.push_back({cordmux::ledger::UninstallLeftoverKind::Sources, "coll-a"});
	const cordmux::ledger::Observation obs =
		build_observation(empty_snapshot(), cordmux::platform::OsProbeResult{}, facts);
	CHECK(obs.uninstall.phase == cordmux::ledger::UninstallPhase::Stopped);
	CHECK(obs.uninstall.obs_streaming == std::optional<bool>(false));
	CHECK(obs.uninstall.obs_recording == std::optional<bool>(true));
	CHECK(obs.uninstall.folder_to_delete == "C:/obs/plugins/cordmux");
	CHECK(obs.uninstall.record_dir == "D:/rec");
	REQUIRE(obs.uninstall.leftovers.size() == 1);
	CHECK(obs.uninstall.leftovers[0].kind == cordmux::ledger::UninstallLeftoverKind::Sources);
	CHECK(obs.uninstall.leftovers[0].where == "coll-a");

	// 起動したときはアンインストールを始めていない (アンインストール中のフラグはメモリにだけあるので、Unknown に
	// ならない)。
	const cordmux::ledger::Observation fresh =
		build_observation(empty_snapshot(), cordmux::platform::OsProbeResult{}, facts_at(kNow));
	CHECK(fresh.uninstall.phase == cordmux::ledger::UninstallPhase::NotStarted);
}

// ---- 画面共有、名前、録画の詳細、OBS の出力 ----

TEST_CASE("画面共有しているかが分かるのは sharing のキーが正しく届いた facts だけで、画面共有していると分かった時刻もコピーされる")
{
	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
	snapshot.slots.push_back(screen_slot(1, "alice"));
	snapshot.facts.received = true;
	cordmux::pipeline::UserIdSet roster;
	roster.user_ids = {"alice", "bob"};
	roster.sent_count = 2;
	snapshot.facts.roster = roster;
	cordmux::pipeline::UserIdSet sharing;
	sharing.user_ids = {"alice"};
	sharing.sent_count = 1;
	snapshot.facts.sharing = sharing;

	// キーがないか壊れた facts (sharing_valid が false): 誰の画面共有も「分かっていない」。
	cordmux::ledger::Observation obs = build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
	CHECK_FALSE(person_of(obs, "alice")->sharing_known);
	CHECK_FALSE(person_of(obs, "bob")->sharing_known);
	CHECK_FALSE(person_of(obs, "alice")->sharing_since_us.has_value());

	snapshot.facts.sharing_valid = true;
	snapshot.facts.sharing_since_us["alice"] = kNow - 30 * kSec;
	obs = build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
	CHECK(person_of(obs, "alice")->sharing_known);
	CHECK(person_of(obs, "bob")->sharing_known); // bob は共有していないと分かっている
	CHECK(person_of(obs, "alice")->sharing_since_us.value_or(0) == kNow - 30 * kSec);
	CHECK_FALSE(person_of(obs, "bob")->sharing_since_us.has_value());
}

TEST_CASE("Discord での名前とユーザー名は、profiles の値をそのまま使う (cordmux での名前を適用しない)")
{
	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
	snapshot.slots.push_back(screen_slot(1, "alice"));
	cordmux::ws::ProfilesMessage message;
	cordmux::ws::ProfileEntry entry;
	entry.user_id = "alice";
	entry.display_name = "fox";
	entry.username = "fox_1";
	message.profiles.push_back(entry);
	snapshot.profiles = cordmux::pipeline::make_profiles_snapshot(message, kNow);
	DockFacts facts = facts_at(kNow);
	cordmux::ledger::SeenPerson seen;
	seen.user_id = "alice";
	seen.name_override = "FOX";
	facts.seen.people.push_back(seen);

	const cordmux::ledger::Observation obs = build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts);
	CHECK(person_of(obs, "alice")->display_name.value_or("") == "FOX");
	CHECK(person_of(obs, "alice")->discord_name.value_or("") == "fox");
	CHECK(person_of(obs, "alice")->username.value_or("") == "fox_1");
}

TEST_CASE("Bot の印は profiles の値のまま people に入り、profiles にいない人は Bot と分からない側になる。参加者の設定の値も Choices に入る")
{
	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
	snapshot.slots.push_back(screen_slot(1, "alice"));
	snapshot.slots.push_back(screen_slot(2, "robo"));
	snapshot.slots.push_back(screen_slot(3, "carol"));
	cordmux::ws::ProfilesMessage message;
	cordmux::ws::ProfileEntry human;
	human.user_id = "alice";
	message.profiles.push_back(human);
	cordmux::ws::ProfileEntry bot;
	bot.user_id = "robo";
	bot.bot = true;
	message.profiles.push_back(bot);
	snapshot.profiles = cordmux::pipeline::make_profiles_snapshot(message, kNow);
	DockFacts facts = facts_at(kNow);
	facts.roster_show = false;
	facts.roster_show_bots = false;

	const cordmux::ledger::Observation obs = build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts);
	CHECK_FALSE(person_of(obs, "alice")->is_bot);
	CHECK(person_of(obs, "robo")->is_bot);
	CHECK_FALSE(person_of(obs, "carol")->is_bot);
	CHECK_FALSE(obs.choices.roster_show);
	CHECK_FALSE(obs.choices.roster_show_bots);
}

TEST_CASE("録画の段階、欠落、録画ファイルの作成の詳細、保存先が recording にコピーされる")
{
	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
	snapshot.recording.phase = cordmux::record::RecordPhase::StartFailed;
	snapshot.recording.phase_since_wall_us = kNow - kSec;
	snapshot.recording.gaps.push_back({kNow - 600 * kSec, 90 * kSec});
	snapshot.recording.unwritable_dir = "D:/rec";
	snapshot.recording.session_dir = "D:/rec/2000-01-01 12-34-56";
	snapshot.recording.finalize.current_index = 2;
	snapshot.recording.finalize.current_permille = 500;
	snapshot.recording.finalize.last_progress_wall_us = kNow - 5 * kSec;
	snapshot.recording.finalize.stalled = true;
	const cordmux::ledger::Observation obs =
		build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts_at(kNow));
	CHECK(obs.recording.phase == cordmux::ledger::RecordPhase::StartFailed);
	CHECK(obs.recording.phase_since_us.value_or(0) == kNow - kSec);
	REQUIRE(obs.recording.gaps.size() == 1);
	CHECK(obs.recording.gaps[0].start_us == kNow - 600 * kSec);
	CHECK(obs.recording.gaps[0].duration_us == 90 * kSec);
	CHECK(obs.recording.unwritable_dir == "D:/rec");
	CHECK(obs.recording.session_dir == "D:/rec/2000-01-01 12-34-56");
	CHECK(obs.recording.finalize_current_index == 2);
	CHECK(obs.recording.finalize_permille.value_or(0) == 500);
	CHECK(obs.recording.finalize_last_progress_us.value_or(0) == kNow - 5 * kSec);
	CHECK(obs.recording.finalize_stalled);

	// 段階のすべての値が 1 対 1 でコピーされる。時刻 0 は持たない。
	for (const auto &[from, to] :
	     std::vector<std::pair<cordmux::record::RecordPhase, cordmux::ledger::RecordPhase>>{
		     {cordmux::record::RecordPhase::Idle, cordmux::ledger::RecordPhase::Idle},
		     {cordmux::record::RecordPhase::Starting, cordmux::ledger::RecordPhase::Starting},
		     {cordmux::record::RecordPhase::Recording, cordmux::ledger::RecordPhase::Recording},
		     {cordmux::record::RecordPhase::Stopping, cordmux::ledger::RecordPhase::Stopping},
		     {cordmux::record::RecordPhase::StartFailed, cordmux::ledger::RecordPhase::StartFailed}}) {
		cordmux::pipeline::PipelineSnapshot s = empty_snapshot();
		s.recording.phase = from;
		const cordmux::ledger::Observation o = build_observation(s, cordmux::platform::OsProbeResult{}, facts_at(kNow));
		CHECK(o.recording.phase == to);
		CHECK_FALSE(o.recording.phase_since_us.has_value());
	}
}

TEST_CASE("OBS 自身の配信と録画の状態は、uninstall の 2 つのフィールドから obs_outputs にコピーされる")
{
	DockFacts facts = facts_at(kNow);
	facts.uninstall.obs_streaming = true;
	facts.uninstall.obs_recording = false;
	const cordmux::ledger::Observation obs = build_observation(empty_snapshot(), cordmux::platform::OsProbeResult{}, facts);
	CHECK(obs.obs_outputs.streaming.value_or(false));
	REQUIRE(obs.obs_outputs.recording.has_value());
	CHECK_FALSE(*obs.obs_outputs.recording);
	const cordmux::ledger::Observation unknown =
		build_observation(empty_snapshot(), cordmux::platform::OsProbeResult{}, facts_at(kNow));
	CHECK_FALSE(unknown.obs_outputs.streaming.has_value());
}

TEST_CASE("通話の名前とメインアカウントがいる通話が、facts から call にコピーされる")
{
	cordmux::pipeline::PipelineSnapshot snapshot = empty_snapshot();
	snapshot.facts.received = true;
	cordmux::pipeline::UserIdSet roster;
	roster.user_ids = {"1", "2"};
	snapshot.facts.roster = roster;
	DockFacts facts = facts_at(kNow);
	facts.broadcaster_user_id = "5";

	SUBCASE("どちらも省かれていれば、名前は空で、メインアカウントの通話は分からない")
	{
		const cordmux::ledger::Observation obs =
			build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts);
		CHECK(obs.call.name.empty());
		CHECK(obs.call.main_presence == cordmux::ledger::Presence::Unknown);
		CHECK(obs.call.main_name.empty());
		CHECK_FALSE(obs.call.main_call_joinable.has_value());
	}

	SUBCASE("読めた値をコピーする")
	{
		snapshot.facts.call_name = "Server / Room";
		cordmux::pipeline::MainCallFact main_call;
		main_call.user_id = "5";
		main_call.name = "Other";
		main_call.joinable = true;
		snapshot.facts.main_call = main_call;
		const cordmux::ledger::Observation obs =
			build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts);
		CHECK(obs.call.name == "Server / Room");
		CHECK(obs.call.main_presence == cordmux::ledger::Presence::InCall);
		CHECK(obs.call.main_name == "Other");
		REQUIRE(obs.call.main_call_joinable.has_value());
		CHECK(*obs.call.main_call_joinable);
	}

	SUBCASE("メインアカウントの通話が見つからないときは、NotInCall にせず、分からないままにする")
	{
		// 見えない通話に参加していることと、どの通話にもいないことは、送信側が区別できない。
		const cordmux::ledger::Observation obs =
			build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts);
		CHECK(obs.call.main_presence != cordmux::ledger::Presence::NotInCall);
	}

	SUBCASE("送信側が読んだときのメインアカウントが、現在の設定と違うときは使わない (設定を変えた直後の古い値)")
	{
		cordmux::pipeline::MainCallFact main_call;
		main_call.user_id = "6";
		main_call.name = "Other";
		main_call.joinable = true;
		snapshot.facts.main_call = main_call;
		const cordmux::ledger::Observation obs =
			build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts);
		CHECK(obs.call.main_presence == cordmux::ledger::Presence::Unknown);
		CHECK_FALSE(obs.call.main_call_joinable.has_value());
	}

	SUBCASE("メインアカウントを使わない構成 (userId が空) では、メインアカウントの通話を使わない")
	{
		cordmux::pipeline::MainCallFact main_call;
		main_call.user_id = "5";
		snapshot.facts.main_call = main_call;
		facts.broadcaster_user_id.clear();
		const cordmux::ledger::Observation obs =
			build_observation(snapshot, cordmux::platform::OsProbeResult{}, facts);
		CHECK(obs.call.main_presence == cordmux::ledger::Presence::Unknown);
	}
}

TEST_CASE("受け取る範囲が設定から Choices にコピーされる")
{
	DockFacts facts = facts_at(kNow);
	CHECK(build_observation(empty_snapshot(), cordmux::platform::OsProbeResult{}, facts).choices.receive_scope ==
	      cordmux::ledger::ReceiveScope::Slots);
	facts.receive_scope = cordmux::ledger::ReceiveScope::Everyone;
	CHECK(build_observation(empty_snapshot(), cordmux::platform::OsProbeResult{}, facts).choices.receive_scope ==
	      cordmux::ledger::ReceiveScope::Everyone);
}
