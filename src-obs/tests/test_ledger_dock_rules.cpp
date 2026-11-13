// SPDX-License-Identifier: Apache-2.0

// decide_dock() (ledger/rules_dock.cpp) の単体テスト。1 行目、メッセージの中身、人の行、一覧の上の行と空の一覧、
// 録画のボタン、メニュー、ダイアログの View、アンインストールの結果、カードの構成と、テキストの規則
// (現在の形のキーだけを使い、引数に「-」も空の文字列も入れない) を確認する。メッセージのレベルの割り当ては
// test_ledger_tiers.cpp で、待ちと並べ方は test_ledger_notice_tracker.cpp で確認する。

#include "doctest.h"

#include "ledger/natural_order.h"
#include "ledger/notice_tracker.h"
#include "ledger/text_ids.h"
#include "ledger_dock_test_helpers.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace ledger_dock_test;
namespace keys = cordmux::ledger::text::keys;

namespace {

std::vector<std::string> form_ids(const dockview::FirstLine &line)
{
	std::vector<std::string> out;
	for (const TextRef &t : line.forms)
		out.push_back(t.id);
	return out;
}

// ini にない形のキー (cordmux.dock.<画面>.<名前> で、画面が status・people・seen・settings・diag・action・
// dialog のどれか) か。
bool old_form_key(const std::string &id)
{
	for (const char *screen : {"cordmux.dock.status.", "cordmux.dock.people.", "cordmux.dock.seen.",
				   "cordmux.dock.settings.", "cordmux.dock.diag.", "cordmux.dock.action.",
				   "cordmux.dock.dialog."}) {
		if (id.rfind(screen, 0) == 0)
			return true;
	}
	return false;
}

// テキストの規則を広い状態で確認するために、多くの状態の Observation を作る。
std::vector<Observation> many_states()
{
	std::vector<Observation> out;
	out.push_back(healthy());
	Observation o = healthy();
	o.people[0].last_video_frame_at_us = kNow - 5 * kSec;
	o.contacts.push_back({"pc-hook", ContactState::Missing});
	o.choices.composite_present = false;
	o.recording.finalize = FinalizeState::Running;
	o.recording.finalize_files_total = 1;
	o.recording.finalize_current_index = 1;
	o.recording.finalize_permille = 420;
	o.recording.gaps.push_back({kNow - 3 * 86400 * kSec, 61 * kSec});
	o.ops.settings_unsaved = true;
	o.ops.vesktop_add_failure = VesktopAddFailure::Refused;
	o.seen.discarded_with_marks = 1;
	o.choices.broadcaster_user_id = "999";
	out.push_back(o);
	Observation d = not_connected();
	d.link.state = LinkState::Disconnected;
	d.link.connected_at_us = kNow - 600 * kSec;
	d.link.disconnected_at_us = kNow - 30 * kSec;
	d.choices.recording = FeatureSwitch::On;
	out.push_back(d);
	Observation u = healthy();
	u.uninstall.phase = UninstallPhase::Stopped;
	u.uninstall.leftovers = {{UninstallLeftoverKind::SettingsFiles, "C:/cfg"},
				 {UninstallLeftoverKind::LocalDataFolder, "C:/local"},
				 {UninstallLeftoverKind::Sources, "stream"},
				 {UninstallLeftoverKind::VencordDir, "C:/state.json"},
				 {UninstallLeftoverKind::CollectionNameDuplicate, "dup"}};
	out.push_back(u);
	Observation running = healthy();
	running.uninstall.phase = UninstallPhase::Running;
	out.push_back(running);
	Observation done = healthy();
	done.uninstall.phase = UninstallPhase::Done;
	done.uninstall.folder_to_delete = "C:/cordmux";
	out.push_back(done);
	// ソースの名前が分かっている DoubleAudio と、使う声の連鎖 (hanako が、通話に参加していない 800 の声を使って
	// いるので、taro は hanako の声を選べない)。
	Observation audio = healthy();
	audio.choices.discord_audio_source = DiscordAudioSource::Found;
	audio.choices.discord_audio_source_name = "Discord audio";
	SeenPerson chain;
	chain.user_id = "200";
	chain.display_name = "hanako";
	chain.voice_source_user_id = "800";
	audio.seen.people.push_back(chain);
	out.push_back(audio);
	Observation off = healthy();
	off.choices.feature = FeatureSwitch::Off;
	out.push_back(off);
	Observation ports = healthy();
	ports.listener.state = ListenerState::PortsExhausted;
	out.push_back(ports);
	return out;
}

} // namespace

// ---- 1 行目 ----

TEST_CASE("平常の 1 行目は通話の人数と表示中の人数で、短い形を後に置き、人数が 1 なら _one のキーにする")
{
	Observation obs = healthy();
	dockview::DockViewV2 v = decide_dock(obs);
	CHECK(v.first_line.kind == dockview::FirstLineKind::Summary);
	REQUIRE(v.first_line.forms.size() == 2);
	CHECK(v.first_line.forms[0].id == keys::kDockSummaryCallStatus.other);
	CHECK(v.first_line.forms[0].args == std::vector<std::string>{"2", "1"});
	CHECK(v.first_line.forms[1].id == keys::kDockSummaryCallShortStatus.other);

	// 表示中の人がいない (枠に載せた人の映像が止まった) ときは「表示中の人はいません」。
	obs.people[0].last_video_frame_at_us = kNow - 5 * kSec;
	v = decide_dock(obs);
	CHECK(v.fallback_line.forms[0].id == keys::kDockSummaryNoneDisplayedStatus.other);

	// 1 人の通話は _one。
	Observation one = healthy();
	one.call.roster_count = 1;
	v = decide_dock(one);
	CHECK(v.first_line.forms[0].id == keys::kDockSummaryCallStatus.one);

	// 通話に参加していない。
	Observation out_of_call = healthy();
	out_of_call.call.presence = Presence::NotInCall;
	out_of_call.call.roster_count = 0;
	CHECK(form_ids(decide_dock(out_of_call).first_line) ==
	      std::vector<std::string>{keys::kDockSummaryConnectedStatus, keys::kDockSummaryConnectedShortStatus});
}

TEST_CASE("表示中の人数は、映像が枠に映っている人だけを数える")
{
	Observation obs = healthy();
	obs.call.roster_count = 6;
	obs.people.push_back(showing("300", "b", 2));
	obs.people.push_back(showing("400", "c", 3));
	PersonFact stalled = showing("500", "d", 4);
	stalled.last_video_frame_at_us = kNow - 3 * kSec;
	obs.people.push_back(stalled);
	const dockview::DockViewV2 v = decide_dock(obs);
	REQUIRE_FALSE(v.fallback_line.forms.empty());
	CHECK(v.fallback_line.forms[0].args == std::vector<std::string>{"6", "3"});
}

TEST_CASE("自分の枠を出している間は、自分も表示中に 1 人として数え、枠にも載っていれば二重に数えない")
{
	Observation obs = healthy();
	obs.call.roster_count = 4;
	obs.people.push_back(showing("300", "b", 2));
	PersonFact self_person = present("700", "me");
	self_person.is_self_frame = true;
	obs.people.push_back(self_person);

	// 自分の枠を出していないときは、枠に映っている 2 人だけ。
	obs.choices.self_frame_shown = false;
	CHECK(decide_dock(obs).fallback_line.forms[0].args == std::vector<std::string>{"4", "2"});

	// 出しているときは、自分を足して 3 人。
	obs.choices.self_frame_shown = true;
	CHECK(decide_dock(obs).fallback_line.forms[0].args == std::vector<std::string>{"4", "3"});

	// 自分の枠の人が Discord の画面共有で枠にも映っているときは、二重に数えない。
	Observation both = healthy();
	both.call.roster_count = 3;
	PersonFact self_showing = showing("700", "me", 3);
	self_showing.is_self_frame = true;
	both.people.push_back(self_showing);
	both.choices.self_frame_shown = true;
	CHECK(decide_dock(both).fallback_line.forms[0].args == std::vector<std::string>{"3", "2"});
}

TEST_CASE("エンコードの段階が unavailable の人は、しきい値の時間内にフレームがあっても表示中に数えない")
{
	Observation obs = healthy();
	obs.call.roster_count = 4;
	// 段階が unavailable になった直後: 最後のフレームはまだしきい値の時間内で、前に届いた解像度も残っている。
	PersonFact gone = showing("300", "b", 2);
	gone.stage = std::string(kStageUnavailable);
	gone.width = 1280;
	gone.height = 720;
	gone.fps = 30.0;
	obs.people.push_back(gone);
	// 知らない名前の段階は数える (画質を下げた映像が映っている見込みで、quality_of() も「画質を下げています」に
	// する)。
	PersonFact other = showing("400", "c", 3);
	other.stage = std::string("experimental");
	obs.people.push_back(other);
	dockview::DockViewV2 v = decide_dock(obs);
	CHECK(v.first_line.kind == dockview::FirstLineKind::Summary);
	REQUIRE_FALSE(v.fallback_line.forms.empty());
	CHECK(v.fallback_line.forms[0].id == keys::kDockSummaryCallStatus.other);
	CHECK(v.fallback_line.forms[0].args == std::vector<std::string>{"4", "2"});
	// 同じ View の人の行は「映像を表示できません」になる (1 行目と人の行が同じ事実を伝える)。
	const dockview::PersonRow *row = row_of(v, "300");
	REQUIRE(row != nullptr);
	REQUIRE(row->quality.has_value());
	CHECK(row->quality->id == keys::kDockRowCantShowStatus);

	// 表示中の人が unavailable の人だけになったら「表示中の人はいません」。
	Observation only = healthy();
	only.people[0].stage = std::string(kStageUnavailable);
	v = decide_dock(only);
	REQUIRE_FALSE(v.fallback_line.forms.empty());
	CHECK(v.fallback_line.forms[0].id == keys::kDockSummaryNoneDisplayedStatus.other);
}

TEST_CASE("利用者が選んだ状態の 1 行目は UserChoice で、機能を無効にしている間はボタンを持ち、ダミーは短い形を持つ")
{
	Observation off = healthy();
	off.choices.feature = FeatureSwitch::Off;
	dockview::DockViewV2 v = decide_dock(off);
	CHECK(v.first_line.kind == dockview::FirstLineKind::UserChoice);
	CHECK(form_ids(v.first_line) == std::vector<std::string>{keys::kDockSummaryDisabledStatus});
	REQUIRE(v.first_line.button.has_value());
	CHECK(v.first_line.button->id == ActionId::ToggleFeature);
	CHECK(v.first_line.button->args.on);
	CHECK(v.first_line.button->label.id == keys::kDockEnableButton);
	CHECK_FALSE(v.first_line.button->disabled_reason.has_value());

	Observation dummy = healthy();
	dummy.choices.dummy = FeatureSwitch::On;
	v = decide_dock(dummy);
	CHECK(v.first_line.kind == dockview::FirstLineKind::UserChoice);
	CHECK(form_ids(v.first_line) ==
	      std::vector<std::string>{keys::kDockSummaryPlaceholdersStatus, keys::kDockSummaryPlaceholdersShortStatus});
}

TEST_CASE("確認中とアンインストールの最中の 1 行目は Working で、Vesktop が起動していないときは Summary になる")
{
	Observation checking = not_connected();
	checking.link.state = LinkState::Unknown;
	checking.install.process = VesktopProcess::Unknown;
	dockview::DockViewV2 v = decide_dock(checking);
	CHECK(v.first_line.kind == dockview::FirstLineKind::Working);
	CHECK(form_ids(v.first_line) == std::vector<std::string>{keys::kDockSummaryCheckingStatus});

	Observation uninstalling = healthy();
	uninstalling.uninstall.phase = UninstallPhase::Running;
	v = decide_dock(uninstalling);
	CHECK(v.first_line.kind == dockview::FirstLineKind::Working);
	CHECK(form_ids(v.first_line) == std::vector<std::string>{keys::kDockUninstallingStatus});
	CHECK(notices_of(v, "Uninstalled").empty());

	Observation not_running = not_connected();
	not_running.install.process = VesktopProcess::NotRunning;
	v = decide_dock(not_running);
	CHECK(v.first_line.kind == dockview::FirstLineKind::Summary);
	CHECK(form_ids(v.first_line) == std::vector<std::string>{keys::kDockSummaryVesktopNotRunningStatus});
}

// ---- メッセージの中身 ----

TEST_CASE("切断のメッセージは、今日なら時刻、別の日なら日付と時刻の引数を持ち、時刻が分からなければ時刻のない形にする")
{
	Observation obs = not_connected();
	obs.link.state = LinkState::Disconnected;
	obs.link.connected_at_us = kNow - 600 * kSec;
	obs.link.disconnected_at_us = kNow - 30 * kSec;
	dockview::DockViewV2 v = decide_dock(obs);
	std::vector<dockview::Notice> n = notices_of(v, "ConnectedThenDropped");
	REQUIRE(n.size() == 1);
	CHECK(n[0].headline.id == keys::kDockDisconnectedHeadline);
	REQUIRE(n[0].headline.args.size() == 1);
	dockview::TimeArgKind kind = dockview::TimeArgKind::DateTime;
	std::int64_t at = 0;
	REQUIRE(dockview::parse_time_arg(n[0].headline.args[0], &kind, &at));
	CHECK(kind == dockview::TimeArgKind::Time);
	CHECK(at == kNow - 30 * kSec);
	CHECK(n[0].tier == dockview::Tier::L1);
	// 段階の行は、見出し「Vesktopとの接続が%1に切れました」と同じことを言う「Vesktop: 接続されていません」を
	// 持たず、動いている段階 (cordmux のソース) だけを持つ。
	REQUIRE(n[0].stages.size() == 1);
	CHECK(n[0].stages[0].text.id == keys::kDockStageCompositeAddedStatus);

	obs.link.disconnected_at_us = kNow - 2 * 86400 * kSec;
	v = decide_dock(obs);
	n = notices_of(v, "ConnectedThenDropped");
	REQUIRE(dockview::parse_time_arg(n[0].headline.args[0], &kind, &at));
	CHECK(kind == dockview::TimeArgKind::DateTime);

	obs.link.disconnected_at_us.reset();
	v = decide_dock(obs);
	n = notices_of(v, "ConnectedThenDropped");
	CHECK(n[0].headline.id == keys::kDockDisconnectedNoTimeHeadline);
	CHECK(n[0].headline.args.empty());

	// トークンが合わずに切れたら (SecretMismatch) 認証の文にし、録画中なら録画の文を追加する。
	obs.link.reason = DisconnectReason::SecretMismatch;
	obs.choices.recording = FeatureSwitch::On;
	v = decide_dock(obs);
	n = notices_of(v, "ConnectedThenDropped");
	REQUIRE(n[0].body.size() == 2);
	CHECK(n[0].body[0].id == keys::kDockDisconnectedAuthText);
	CHECK(n[0].body[1].id == keys::kDockDisconnectedRecordingText);
}

TEST_CASE("Discord の内部への依存箇所が使えないメッセージは 1 件にまとめ、最も影響の大きいものを見出しにする")
{
	Observation obs = healthy();
	obs.contacts = {{"profile", ContactState::Missing}, {"sharing", ContactState::Lost}, {"self", ContactState::Missing}};
	const dockview::DockViewV2 v = decide_dock(obs);
	const std::vector<dockview::Notice> n = notices_of(v, "ContactUnavailable");
	REQUIRE(n.size() == 1);
	CHECK(n[0].headline.id == keys::kDockContactSharingHeadline);
	CHECK(n[0].tier == dockview::Tier::L2);
	REQUIRE(n[0].buttons.size() == 1);
	CHECK(n[0].buttons[0].id == ActionId::ExportBundle);
	CHECK(n[0].buttons[0].label.id == keys::kDockExportDiagnosticsButton);
	// 本文は、表示中の画面共有は続くことと、Vesktop の画面が表示されないときの戻し方 ([Vesktopから削除]) の 2 つ。
	// 戻す操作のボタンは置かない (Vesktop を終了すると接続が切れて、このカードが消えるため。状態タブの「Vesktop」の組にある)。
	REQUIRE(n[0].body.size() == 2);
	CHECK(n[0].body[0].id == keys::kDockContactText);
	CHECK(n[0].body[1].id == keys::kDockContactNoScreenText);
}

TEST_CASE("受信側が知らない依存箇所の id が使えないときは、最も重い見出しにする")
{
	Observation obs = healthy();
	obs.contacts = {{"profile", ContactState::Missing}, {"speaking", ContactState::Missing}};
	const dockview::DockViewV2 v = decide_dock(obs);
	const std::vector<dockview::Notice> n = notices_of(v, "ContactUnavailable");
	REQUIRE(n.size() == 1);
	CHECK(n[0].headline.id == keys::kDockContactMediaHeadline);
	CHECK(n[0].tier == dockview::Tier::L2);
}

TEST_CASE("依存箇所の行が 1 つだけ使えないとき、見出しは行ごとに決まり、メッセージのレベルはどれも 2 になる")
{
	struct HeadlineCase {
		const char *id;
		const char *headline;
	};
	const HeadlineCase cases[] = {
		{"pc-hook", keys::kDockContactMediaHeadline},
		{"sharing", keys::kDockContactSharingHeadline},
		{"watch-stop", keys::kDockContactSharingHeadline},
		{"ssrc-map", keys::kDockContactSsrcHeadline},
		{"roster", keys::kDockContactRosterHeadline},
		{"profile", keys::kDockContactProfileHeadline},
		{"self", keys::kDockContactSelfHeadline},
		{"call-info", keys::kDockContactCallHeadline},
		{"voice-select", keys::kDockContactVoiceSelectHeadline},
	};
	for (const HeadlineCase &c : cases) {
		for (const ContactState state : {ContactState::Missing, ContactState::Lost}) {
			Observation obs = healthy();
			obs.contacts = {{c.id, state}};
			const dockview::DockViewV2 v = decide_dock(obs);
			const std::vector<dockview::Notice> n = notices_of(v, "ContactUnavailable");
			CAPTURE(c.id);
			REQUIRE(n.size() == 1);
			CHECK(n[0].headline.id == c.headline);
			CHECK(n[0].tier == dockview::Tier::L2);
			// 表示中の画面共有は続く、と伝える本文と、Vesktop の画面が表示されないときの戻し方を持ち、診断を書き出す
			// 操作を持つ。戻し方は、どの依存箇所でも同じ (画面が表示されているかを受信側は知らないので、条件付きの文)。
			REQUIRE(n[0].body.size() == 2);
			CHECK(n[0].body[0].id == keys::kDockContactText);
			CHECK(n[0].body[1].id == keys::kDockContactNoScreenText);
			REQUIRE(n[0].buttons.size() == 1);
			CHECK(n[0].buttons[0].id == ActionId::ExportBundle);
		}
	}
}

TEST_CASE("映像が届かない人のメッセージは、枠に載っている人と録画の対象の人を数え、最初のフレームが届かない人も数える")
{
	Observation obs = healthy();
	// 枠に載っていない人も録画の対象にするので、受け取る範囲は画面共有している全員にする (既定の「枠に載せた人だけ」では
	// 枠に載っていない人は対象にならない)。
	obs.choices.receive_scope = cordmux::ledger::ReceiveScope::Everyone;
	// 枠に載っている人の映像が止まった (1 人)。
	obs.people[0].last_video_frame_at_us = kNow - 5 * kSec;
	// 録画の対象 (枠に載っていない、画面共有している、録画中) で、最初のフレームが届かない (1 人)。
	obs.choices.recording = FeatureSwitch::On;
	obs.recording.phase = RecordPhase::Recording;
	obs.people[1].sharing = true;
	obs.people[1].sharing_since_us = kNow - 20 * kSec;
	dockview::DockViewV2 v = decide_dock(obs);
	std::vector<dockview::Notice> n = notices_of(v, "AllVideoStoppedHint");
	REQUIRE(n.size() == 1);
	CHECK(n[0].headline.id == keys::kDockNoVideoHeadline.other);
	CHECK(n[0].headline.args == std::vector<std::string>{"2"});
	CHECK(n[0].settle == dockview::Settle::ClearOnly);
	// 本文: 枠に載っている人と録画の対象だけの人の両方がいる -> 枠の文、録画の文、次の操作の文。
	const auto body_ids = [](const dockview::Notice &notice) {
		std::vector<std::string> ids;
		for (const TextRef &t : notice.body)
			ids.push_back(t.id);
		return ids;
	};
	CHECK(body_ids(n[0]) == std::vector<std::string>{keys::kDockNoVideoText, keys::kDockNoVideoRecordingText,
							  keys::kDockNoVideoNextText});

	// 待ち時間を過ぎていなければ数えない (画面共有を見始めた直後は、フレームがまだないのが普通)。数えるのが 1 人に
	// なるので、メッセージにしない (その人の行と OBS の映像で分かる)。
	obs.people[1].sharing_since_us = kNow - 2 * kSec;
	v = decide_dock(obs);
	CHECK(notices_of(v, "AllVideoStoppedHint").empty());

	// 枠に載っていて録画の対象でもある 2 人 -> 枠の文、録画の文、次の操作の文 (録画の文は、枠に載っているかを問わない)。
	obs.people[1] = showing("200", "hanako", 2);
	obs.people[1].last_video_frame_at_us = kNow - 5 * kSec;
	v = decide_dock(obs);
	n = notices_of(v, "AllVideoStoppedHint");
	REQUIRE(n.size() == 1);
	CHECK(n[0].tier == dockview::Tier::L2);
	CHECK(n[0].headline.args == std::vector<std::string>{"2"});
	CHECK(body_ids(n[0]) == std::vector<std::string>{keys::kDockNoVideoText, keys::kDockNoVideoRecordingText,
							  keys::kDockNoVideoNextText});

	// 同じ 2 人で cordmux の録画をしていない -> 枠の文と次の操作の文だけ (録画していないので、録画に入らない
	// ことを伝えない)。
	obs.choices.recording = FeatureSwitch::Off;
	obs.recording.phase = RecordPhase::Idle;
	v = decide_dock(obs);
	n = notices_of(v, "AllVideoStoppedHint");
	REQUIRE(n.size() == 1);
	CHECK(body_ids(n[0]) == std::vector<std::string>{keys::kDockNoVideoText, keys::kDockNoVideoNextText});

	// 枠に載っていない録画の対象の人に映像が届いていれば、2 人が止まっていてもメッセージにしない。
	obs.choices.recording = FeatureSwitch::On;
	obs.recording.phase = RecordPhase::Recording;
	PersonFact arriving = present("300", "kenji");
	arriving.sharing = true;
	arriving.last_record_frame_at_us = kNow - kSec / 10;
	obs.people.push_back(arriving);
	CHECK(notices_of(decide_dock(obs), "AllVideoStoppedHint").empty());
}

TEST_CASE("映像が届かない人のメッセージの次の操作の文はエンコードの段階で選び、unavailable なら画面共有の始め直しにする")
{
	const auto body_ids = [](const dockview::Notice &notice) {
		std::vector<std::string> ids;
		for (const TextRef &t : notice.body)
			ids.push_back(t.id);
		return ids;
	};
	// 段階が unavailable の人だけが、しきい値を超えても映像が届かない -> 見出しと枠の文はほかの場合と同じで、
	// 次の操作の文は始め直しの 1 文だけにする (Vesktop で確認する文は置かない)。同じ文を 2 回表示しない。
	Observation obs = healthy();
	obs.people[0].stage = std::string(kStageUnavailable);
	obs.people[0].last_video_frame_at_us = kNow - 5 * kSec;
	PersonFact gone = showing("400", "c", 3);
	gone.stage = std::string(kStageUnavailable);
	gone.last_video_frame_at_us = kNow - 5 * kSec;
	obs.people.push_back(gone);
	obs.call.roster_count = 3;
	dockview::DockViewV2 v = decide_dock(obs);
	std::vector<dockview::Notice> n = notices_of(v, "AllVideoStoppedHint");
	REQUIRE(n.size() == 1);
	CHECK(n[0].tier == dockview::Tier::L2);
	CHECK(n[0].headline.id == keys::kDockNoVideoHeadline.other);
	CHECK(n[0].settle == dockview::Settle::ClearOnly);
	CHECK(body_ids(n[0]) == std::vector<std::string>{keys::kDockNoVideoText, keys::kDockNoVideoCantShowNextText});
	// メッセージが始め直しを頼む人の行は、同じ View で「映像を表示できません」になる。
	const dockview::PersonRow *named = row_of(v, obs.people[0].user_id);
	REQUIRE(named != nullptr);
	CHECK_FALSE(named->status.has_value());
	REQUIRE(named->quality.has_value());
	CHECK(named->quality->id == keys::kDockRowCantShowStatus);

	// 段階のない人も止まった -> Vesktop で確認する文が先、始め直しの文が後 (どちらも 1 回)。
	PersonFact plain = showing("300", "b", 2);
	plain.last_video_frame_at_us = kNow - 5 * kSec;
	obs.people.push_back(plain);
	obs.call.roster_count = 4;
	v = decide_dock(obs);
	n = notices_of(v, "AllVideoStoppedHint");
	REQUIRE(n.size() == 1);
	CHECK(n[0].headline.id == keys::kDockNoVideoHeadline.other);
	CHECK(n[0].headline.args == std::vector<std::string>{"3"});
	CHECK(body_ids(n[0]) == std::vector<std::string>{keys::kDockNoVideoText, keys::kDockNoVideoNextText,
							  keys::kDockNoVideoCantShowNextText});
	// 両方の人がいるときも、始め直しを頼む人 (段階が unavailable) と、Vesktop で確認する文の対象の人 (段階の
	// ない人) を、人の行に表示するテキストで見分けられる。
	for (const char *id : {"100", "400"}) {
		const dockview::PersonRow *row = row_of(v, id);
		REQUIRE(row != nullptr);
		CHECK_FALSE(row->status.has_value());
		REQUIRE(row->quality.has_value());
		CHECK(row->quality->id == keys::kDockRowCantShowStatus);
	}
	const dockview::PersonRow *plain_row = row_of(v, "300");
	REQUIRE(plain_row != nullptr);
	REQUIRE(plain_row->status.has_value());
	CHECK(plain_row->status->state == dockview::RowState::NoVideo);
	CHECK(plain_row->status->text.id == keys::kDockRowNotReceivingStatus);

	// 知らない名前の段階は unavailable ではないとみなす (displayed() と同じ判断) -> Vesktop で確認する文だけ。
	Observation other = healthy();
	other.people[1] = showing("200", "hanako", 2);
	for (PersonFact &person : other.people) {
		person.stage = std::string("experimental");
		person.last_video_frame_at_us = kNow - 5 * kSec;
	}
	v = decide_dock(other);
	n = notices_of(v, "AllVideoStoppedHint");
	REQUIRE(n.size() == 1);
	CHECK(body_ids(n[0]) == std::vector<std::string>{keys::kDockNoVideoText, keys::kDockNoVideoNextText});

	// 録画の対象だけの人 (枠に載っていない) が unavailable で止まった -> 録画の文と始め直しの文。
	Observation rec = healthy();
	rec.choices.receive_scope = cordmux::ledger::ReceiveScope::Everyone;
	rec.choices.recording = FeatureSwitch::On;
	rec.recording.phase = RecordPhase::Recording;
	rec.people[0].slot.reset();
	for (PersonFact &person : rec.people) {
		person.sharing = true;
		person.stage = std::string(kStageUnavailable);
		person.last_record_frame_at_us = kNow - 5 * kSec;
	}
	v = decide_dock(rec);
	n = notices_of(v, "AllVideoStoppedHint");
	REQUIRE(n.size() == 1);
	CHECK(body_ids(n[0]) ==
	      std::vector<std::string>{keys::kDockNoVideoRecordingText, keys::kDockNoVideoCantShowNextText});

	// しきい値の時間内の unavailable の人では、メッセージにならない (段階で選ぶのは次の操作の文だけで、メッセージにする
	// 条件は変えない。表示中の人数にも数えない)。
	Observation fresh = healthy();
	fresh.people[0].stage = std::string(kStageUnavailable);
	v = decide_dock(fresh);
	CHECK(notices_of(v, "AllVideoStoppedHint").empty());
}

TEST_CASE("映像が届かない人のメッセージの枠の文は参加者ごとに選び、最初のフレームがまだない人には空いた枠の文を使う")
{
	const auto body_ids = [](const dockview::Notice &notice) {
		std::vector<std::string> ids;
		for (const TextRef &t : notice.body)
			ids.push_back(t.id);
		return ids;
	};
	// 枠に載っていて、画面共有していると分かってから待ち時間を超えても、最初のフレームが届かない人。
	const auto waiting = [](const std::string &user_id, const std::string &name, int slot) {
		PersonFact p = showing(user_id, name, slot);
		p.last_video_frame_at_us.reset();
		p.sharing_since_us = kNow - 20 * kSec;
		return p;
	};

	// 最初の確保でエンコードの段階が尽きた人だけ (エンコーダがない PC で起きやすい) -> 空いた枠の文と始め直しの文。
	// 「最後に届いた映像が止まったまま」とは言わない (その人の枠には何も届いていない)。
	Observation obs = healthy();
	obs.people[0] = waiting("100", "taro", 1);
	obs.people[1] = waiting("200", "hanako", 2);
	obs.people[0].stage = std::string(kStageUnavailable);
	obs.people[1].stage = std::string(kStageUnavailable);
	dockview::DockViewV2 v = decide_dock(obs);
	std::vector<dockview::Notice> n = notices_of(v, "AllVideoStoppedHint");
	REQUIRE(n.size() == 1);
	CHECK(n[0].tier == dockview::Tier::L2);
	CHECK(n[0].headline.id == keys::kDockNoVideoHeadline.other);
	CHECK(n[0].settle == dockview::Settle::ClearOnly);
	CHECK(body_ids(n[0]) == std::vector<std::string>{keys::kDockNoVideoWaitingText, keys::kDockNoVideoCantShowNextText});

	// 段階のない人で最初のフレームが届かない -> 空いた枠の文と Vesktop で確認する文。
	obs.people[0].stage.reset();
	obs.people[1].stage.reset();
	v = decide_dock(obs);
	n = notices_of(v, "AllVideoStoppedHint");
	REQUIRE(n.size() == 1);
	CHECK(body_ids(n[0]) == std::vector<std::string>{keys::kDockNoVideoWaitingText, keys::kDockNoVideoNextText});

	// 3 人目もフレームが届かない -> 空いた枠の文は 1 回。
	obs.people.push_back(waiting("300", "b", 2));
	obs.call.roster_count = 3;
	v = decide_dock(obs);
	n = notices_of(v, "AllVideoStoppedHint");
	REQUIRE(n.size() == 1);
	CHECK(n[0].headline.id == keys::kDockNoVideoHeadline.other);
	CHECK(n[0].headline.args == std::vector<std::string>{"3"});
	CHECK(body_ids(n[0]) == std::vector<std::string>{keys::kDockNoVideoWaitingText, keys::kDockNoVideoNextText});

	// 届いていた映像が止まった人もいる -> 止まった人の文、空いた枠の文の順に 1 回ずつ (次の操作の文と同じ並べ方)。
	// 次の操作の文も段階で選ぶ (止まった人は段階なし、フレームが届かない人の 1 人は unavailable)。
	PersonFact stalled = showing("400", "c", 3);
	stalled.last_video_frame_at_us = kNow - 5 * kSec;
	obs.people.push_back(stalled);
	obs.people[2].stage = std::string(kStageUnavailable); // 300 (フレームが届かない 2 人目)
	obs.call.roster_count = 4;
	v = decide_dock(obs);
	n = notices_of(v, "AllVideoStoppedHint");
	REQUIRE(n.size() == 1);
	CHECK(n[0].headline.args == std::vector<std::string>{"4"});
	CHECK(body_ids(n[0]) == std::vector<std::string>{keys::kDockNoVideoText, keys::kDockNoVideoWaitingText,
							  keys::kDockNoVideoNextText, keys::kDockNoVideoCantShowNextText});

	// cordmux の録画をしている -> 枠の 2 文の後に録画の文、その後に次の操作の文。
	obs.choices.recording = FeatureSwitch::On;
	obs.recording.phase = RecordPhase::Recording;
	v = decide_dock(obs);
	n = notices_of(v, "AllVideoStoppedHint");
	REQUIRE(n.size() == 1);
	CHECK(body_ids(n[0]) == std::vector<std::string>{keys::kDockNoVideoText, keys::kDockNoVideoWaitingText,
							  keys::kDockNoVideoRecordingText, keys::kDockNoVideoNextText,
							  keys::kDockNoVideoCantShowNextText});

	// 録画の対象だけの人 (枠に載っていない) で最初のフレームが届かない -> 空いた枠の文を表示しない (枠がない)。
	// 枠に載っている人 (100) の映像が届いている間は、1 人だけなのでメッセージにしない。
	Observation rec = healthy();
	rec.choices.receive_scope = cordmux::ledger::ReceiveScope::Everyone;
	rec.choices.recording = FeatureSwitch::On;
	rec.recording.phase = RecordPhase::Recording;
	rec.people[1].sharing = true;
	rec.people[1].sharing_since_us = kNow - 20 * kSec;
	CHECK(notices_of(decide_dock(rec), "AllVideoStoppedHint").empty());
	PersonFact only_recorded = rec.people[1];
	only_recorded.user_id = "300";
	rec.people[0].slot.reset();
	rec.people[0].sharing_since_us = kNow - 20 * kSec;
	rec.people[0].last_video_frame_at_us.reset();
	v = decide_dock(rec);
	n = notices_of(v, "AllVideoStoppedHint");
	REQUIRE(n.size() == 1);
	CHECK(body_ids(n[0]) == std::vector<std::string>{keys::kDockNoVideoRecordingText, keys::kDockNoVideoNextText});

	// 枠に載っていてフレームが届かない人と、録画の対象だけの人 -> 空いた枠の文、録画の文、次の操作の文。
	rec.people[0] = waiting("100", "taro", 1);
	v = decide_dock(rec);
	n = notices_of(v, "AllVideoStoppedHint");
	REQUIRE(n.size() == 1);
	CHECK(body_ids(n[0]) ==
	      std::vector<std::string>{keys::kDockNoVideoWaitingText, keys::kDockNoVideoRecordingText,
				       keys::kDockNoVideoNextText});
}

TEST_CASE("保存先に書き込めなかったメッセージは、場所の行と、設定の [録画] ページを開く [保存先を変更] を持つ")
{
	Observation obs = healthy();
	obs.recording.phase = RecordPhase::StartFailed;
	obs.recording.unwritable_dir = "D:/rec";
	const dockview::DockViewV2 v = decide_dock(obs);
	const std::vector<dockview::Notice> n = notices_of(v, "RecordDirUnwritable");
	REQUIRE(n.size() == 1);
	CHECK(n[0].headline.id == keys::kDockRecordStartFailedHeadline);
	REQUIRE(n[0].location.has_value());
	CHECK(n[0].location->path == "D:/rec");
	REQUIRE(n[0].buttons.size() == 1);
	CHECK(n[0].buttons[0].id == ActionId::OpenSettings);
	CHECK(n[0].buttons[0].args.page == SettingsPage::Recording);
	CHECK_FALSE(n[0].buttons[0].disabled_reason.has_value());
	// 録画のボタンは失敗の状態で、押せる (押すたびに保存先を決め直す)。
	CHECK(v.record.state == dockview::RecordButtonState::Failed);
	CHECK_FALSE(v.record.action.disabled_reason.has_value());
}

TEST_CASE("作成しているメッセージの 2 行目は実際の数から作り、件数・現在のファイルの割合・進んでいない時間の 3 つの形がある")
{
	Observation obs = healthy();
	obs.recording.finalize = FinalizeState::Running;
	obs.recording.finalize_files_total = 8;
	obs.recording.finalize_files_done = 3;
	obs.recording.phase_since_us = kNow - 12 * 60 * kSec;
	dockview::DockViewV2 v = decide_dock(obs);
	std::vector<dockview::Notice> n = notices_of(v, "RecordFinalizeRunning");
	REQUIRE(n.size() == 1);
	CHECK(n[0].tier == dockview::Tier::L3);
	REQUIRE(n[0].line2.has_value());
	CHECK(n[0].line2->id == keys::kDockFinalizeProgressStatus.other);
	CHECK(n[0].line2->args == std::vector<std::string>{"3", "8", "12"});

	obs.recording.finalize_current_index = 4;
	obs.recording.finalize_permille = 429;
	v = decide_dock(obs);
	n = notices_of(v, "RecordFinalizeRunning");
	CHECK(n[0].line2->id == keys::kDockFinalizeProgressFileStatus.other);
	CHECK(n[0].line2->args == std::vector<std::string>{"3", "8", "4", "42", "12"});

	obs.recording.finalize_stalled = true;
	obs.recording.finalize_last_progress_us = kNow - 6 * 60 * kSec;
	v = decide_dock(obs);
	n = notices_of(v, "RecordFinalizeRunning");
	CHECK(n[0].line2->id == keys::kDockFinalizeStalledStatus.other);
	CHECK(n[0].line2->args == std::vector<std::string>{"3", "8", "4", "6"});

	obs.recording.finalize_files_total = 1;
	obs.recording.finalize_files_done = 0;
	v = decide_dock(obs);
	n = notices_of(v, "RecordFinalizeRunning");
	CHECK(n[0].line2->id == keys::kDockFinalizeStalledStatus.one);
}

TEST_CASE("数の書き方は、足りない容量を 1024 の累乗と小数 1 桁 (切り上げ) にし、4 桁以上の分を桁区切りにする")
{
	Observation obs = healthy();
	obs.recording.finalize_need_bytes = 3ULL * 1024 * 1024 * 1024 + 1;
	obs.recording.free_space_bytes = 1ULL * 1024 * 1024 * 1024;
	dockview::DockViewV2 v = decide_dock(obs);
	std::vector<dockview::Notice> n = notices_of(v, "RecordFinalizeShort");
	REQUIRE(n.size() == 1);
	CHECK(n[0].headline.args == std::vector<std::string>{"2.1 GB"});

	obs.recording.finalize_need_bytes = 1ULL * 1024 * 1024 * 1024 + 512ULL * 1024 * 1024;
	v = decide_dock(obs);
	n = notices_of(v, "RecordFinalizeShort");
	CHECK(n[0].headline.args == std::vector<std::string>{"512.0 MB"});

	Observation space = healthy();
	space.choices.recording = FeatureSwitch::On;
	space.recording.phase = RecordPhase::Recording;
	space.recording.space_warn_seconds = 100000 * 60;
	space.recording.remaining_seconds = 1234 * 60 + 59;
	v = decide_dock(space);
	n = notices_of(v, "RecordSpaceLow");
	REQUIRE(n.size() == 1);
	CHECK(n[0].headline.args == std::vector<std::string>{"1,234"});
	CHECK(n[0].settle == dockview::Settle::Debounced);

	Observation port = healthy();
	port.listener.state = ListenerState::PortsExhausted;
	port.listener.port_first = 49650;
	port.listener.port_last = 49665;
	v = decide_dock(port);
	n = notices_of(v, "ListenPortsExhausted");
	REQUIRE(n.size() == 1);
	REQUIRE(n[0].body.size() == 3); // 理由 (ポート番号) -> 帰結 -> 次の操作
	CHECK(n[0].body[0].id == keys::kDockPortsInUseText);
	CHECK(n[0].body[0].args == std::vector<std::string>{"49650", "49665"}); // ポート番号は区切らない
}

TEST_CASE("メインアカウントが通話に参加していないメッセージは、名前が分かれば %1 に入れ、分からなければ名前のない形にする")
{
	Observation obs = healthy();
	obs.choices.broadcaster_user_id = "999";
	dockview::DockViewV2 v = decide_dock(obs);
	std::vector<dockview::Notice> n = notices_of(v, "BroadcasterNotInCall");
	REQUIRE(n.size() == 1);
	CHECK(n[0].headline.id == keys::kDockMainAccountNotInCallNoNameHeadline);
	SeenPerson seen;
	seen.user_id = "999";
	seen.display_name = "main";
	obs.seen.people.push_back(seen);
	v = decide_dock(obs);
	n = notices_of(v, "BroadcasterNotInCall");
	CHECK(n[0].headline.id == keys::kDockMainAccountNotInCallHeadline);
	CHECK(n[0].headline.args == std::vector<std::string>{"main"});
}

TEST_CASE("再インストールの失敗は、上書きのメッセージがあればそのカードに文を追加し、カードを 2 枚にしない")
{
	Observation obs = not_connected();
	obs.install.dir = VencordDirState::HashMismatch;
	obs.install.reinstall_failed_files = {"renderer.js"};
	const dockview::DockViewV2 v = decide_dock(obs);
	CHECK(notices_of(v, "ReinstallFailed").empty());
	const std::vector<dockview::Notice> n = notices_of(v, "VencordOverwritten");
	REQUIRE(n.size() == 1);
	REQUIRE(n[0].body.size() == 2);
	CHECK(n[0].body[1].id == keys::kDockReinstallFailedText);
	// ファイルの名前は画面に表示しない (どのテキストの引数にも入れない)。
	std::vector<TextRef> refs;
	collect_refs(v, &refs);
	for (const TextRef &t : refs)
		for (const std::string &a : t.args)
			CHECK(a.find("renderer.js") == std::string::npos);
}

TEST_CASE("[Vesktopに追加] の失敗は、理由ごとに理由の文を選んで帰結の後に置き、"
	  "押し直しても同じ失敗になる理由ではボタンを置かない")
{
	struct Case {
		VesktopAddFailure failure;
		bool settings_unreadable; // 起動のときに cordmux の settings.json を読めなかった
		const char *text;         // nullptr = 理由の文なし
		bool button;              // [Vesktopに追加] を置くか
	};
	const Case cases[] = {
		{VesktopAddFailure::StatePathUnknown, false, keys::kDockVesktopAddFailedStateMissingText, true},
		{VesktopAddFailure::StateUnreadable, false, keys::kDockVesktopAddFailedStateUnreadableText, true},
		{VesktopAddFailure::BundledDirUnknown, false, keys::kDockVesktopAddFailedBundledMissingText, false},
		{VesktopAddFailure::Refused, false, nullptr, true},
		{VesktopAddFailure::WritesDisabled, false, keys::kDockVesktopAddFailedWritesOffText, false},
		// settings.json を読めなかったために書き込みを無効にした起動では、「settings.jsonで無効」とは言わない
		// (利用者は無効にしていない)。
		{VesktopAddFailure::WritesDisabled, true, keys::kDockVesktopAddFailedSettingsUnreadText, false},
		// バックアップの保存先がないとき (NoBackupPlace) はボタンを置かないので、バックアップを書けないとき
		// (BackupWriteFailed。ボタンあり) とキーを分ける (次の操作を含むキーにする)。
		{VesktopAddFailure::NoBackupPlace, false, keys::kDockVesktopAddFailedNoBackupText, false},
		{VesktopAddFailure::BackupWriteFailed, false, keys::kDockVesktopAddFailedBackupText, true},
		{VesktopAddFailure::StateWriteFailed, false, keys::kDockVesktopAddFailedStateWriteText, true},
		// ポータブル版の Vesktop を見たときは、同じ起動の中では変わらないので、ボタンを置かない。
		{VesktopAddFailure::PortableVesktop, false, keys::kDockVesktopAddFailedPortableText, false},
	};
	for (const Case &c : cases) {
		Observation obs = healthy();
		obs.install.dir = VencordDirState::PointsElsewhere;
		obs.ops.vesktop_add_failure = c.failure;
		obs.ops.settings_unreadable_at_startup = c.settings_unreadable;
		const dockview::DockViewV2 v = decide_dock(obs);
		const std::vector<dockview::Notice> n = notices_of(v, "VesktopAddFailed");
		REQUIRE(n.size() == 1);
		CHECK(n[0].buttons.size() == (c.button ? 1u : 0u));
		// ボタンを置かないときも、このカードがインストールの食い違いのカードを兼ね、食い違いのカードを
		// ボタン付きで別に表示しない (ドック全体の [Vesktopに追加] は 0 個)。
		CHECK(notices_of(v, "InstallMismatch").empty());
		int add_buttons = 0;
		for (const dockview::Notice &card : v.notices)
			for (const dockview::ViewButton &b : card.buttons)
				add_buttons += b.label.id == keys::kDockAddToVesktopButton ? 1 : 0;
		CHECK(add_buttons == (c.button ? 1 : 0));
		if (c.text == nullptr) {
			REQUIRE(n[0].body.size() == 1);
			CHECK(n[0].body[0].id == keys::kDockCannotReceiveAfterRestartText);
		} else {
			REQUIRE(n[0].body.size() == 2);
			CHECK(n[0].body[0].id == keys::kDockCannotReceiveAfterRestartText);
			CHECK(n[0].body[1].id == c.text);
		}
		// settings.json を読めなかった起動の理由のキーは、次の操作を含まない。次の操作は、同じ起動の間ずっと
		// 表示される設定のカード (SettingsUnreadAtStartup) が伝えるので、そのカードが同じドックに並ぶことを
		// 確認する (次の操作がどこにもない状態にしない)。
		CHECK(notices_of(v, "SettingsUnreadAtStartup").size() == (c.settings_unreadable ? 1u : 0u));
	}
}

TEST_CASE("[Vesktopに追加] の失敗は、接続していない間も帰結を理由より前に置く")
{
	Observation obs = not_connected();
	obs.install.dir = VencordDirState::NotConfigured;
	obs.ops.vesktop_add_failure = VesktopAddFailure::NoBackupPlace;
	const std::vector<dockview::Notice> n = notices_of(decide_dock(obs), "VesktopAddFailed");
	REQUIRE(n.size() == 1);
	REQUIRE(n[0].body.size() == 2);
	CHECK(n[0].body[0].id == keys::kDockCannotReceiveText);
	CHECK(n[0].body[1].id == keys::kDockVesktopAddFailedNoBackupText);
	CHECK(n[0].buttons.empty());
}

TEST_CASE("この起動で Vesktop の設定を書かない間は、追加されていない状態のカードに [Vesktopに追加] を置かず、"
	  "失敗のカードと同じ理由と次の操作を帰結の後に置き、[再インストール] は残す")
{
	// ドックにある [Vesktopに追加] の数。
	const auto add_buttons = [](const dockview::DockViewV2 &v) {
		int count = 0;
		for (const dockview::Notice &card : v.notices)
			for (const dockview::ViewButton &b : card.buttons)
				count += b.label.id == keys::kDockAddToVesktopButton ? 1 : 0;
		return count;
	};
	struct Case {
		VencordDirState dir;
		bool connected;
		const char *rule;   // 状態のカードの規則の名前
		const char *result; // 帰結のキー
	};
	const Case cases[] = {
		{VencordDirState::NotConfigured, false, "VencordDirNotConfigured", keys::kDockCannotReceiveText},
		{VencordDirState::PointsElsewhere, false, "VencordDirElsewhere", keys::kDockCannotReceiveText},
		// 接続している間 (InstallMismatch) も同じ。次に起動したときに困ることを伝えるカードで、押しても
		// 同じ失敗になることは変わらない。
		{VencordDirState::NotConfigured, true, "InstallMismatch", keys::kDockCannotReceiveAfterRestartText},
		{VencordDirState::PointsElsewhere, true, "InstallMismatch", keys::kDockCannotReceiveAfterRestartText},
	};
	for (const Case &c : cases) {
		for (const bool unreadable : {false, true}) {
			Observation obs = c.connected ? healthy() : not_connected();
			obs.install.dir = c.dir;
			obs.ops.settings_unreadable_at_startup = unreadable;
			const dockview::DockViewV2 writable = decide_dock(obs);
			const std::vector<dockview::Notice> before = notices_of(writable, c.rule);
			REQUIRE(before.size() == 1);
			CHECK(add_buttons(writable) == 1);

			obs.install.vesktop_writes_off = true;
			const dockview::DockViewV2 v = decide_dock(obs);
			const std::vector<dockview::Notice> n = notices_of(v, c.rule);
			REQUIRE(n.size() == 1);
			CHECK(n[0].buttons.empty());
			CHECK(add_buttons(v) == 0);
			// 並びは失敗のカードと同じ: 帰結 -> 理由と次の操作。settings.json を読めなかった起動では、
			// 「settings.jsonで無効」とは言わない (利用者は無効にしていない)。次の操作は、同じドックの
			// 設定のカードが伝える。
			REQUIRE(n[0].body.size() == 2);
			CHECK(n[0].body[0].id == c.result);
			CHECK(n[0].body[1].id == (unreadable ? keys::kDockVesktopAddFailedSettingsUnreadText
							     : keys::kDockVesktopAddFailedWritesOffText));
			CHECK(notices_of(v, "SettingsUnreadAtStartup").size() == (unreadable ? 1u : 0u));
			// 見出し・メッセージのレベル・段階の行は、書き込める起動と同じ (変えるのはボタンと理由の文だけ)。
			CHECK(n[0].headline == before[0].headline);
			CHECK(n[0].tier == before[0].tier);
			CHECK(n[0].stages == before[0].stages);
		}
	}

	// 上書きされているときの [再インストール] は残す (コピーするのは cordmux に同梱のファイルで、Vesktop の
	// 設定を書かない)。
	Observation overwritten = not_connected();
	overwritten.install.dir = VencordDirState::HashMismatch;
	overwritten.install.vesktop_writes_off = true;
	const std::vector<dockview::Notice> o = notices_of(decide_dock(overwritten), "VencordOverwritten");
	REQUIRE(o.size() == 1);
	REQUIRE(o[0].buttons.size() == 1);
	CHECK(o[0].buttons[0].label.id == keys::kDockReinstallButton);
	REQUIRE(o[0].body.size() == 1);
	CHECK(o[0].body[0].id == keys::kDockCannotReceiveText);
}

TEST_CASE("エンコードの段階が unavailable の間は、送られていない映像の解像度を表示しない")
{
	Observation obs = healthy();
	obs.people[0].stage = std::string(kStageUnavailable);
	// 段階 (facts) と解像度 (configure) は別のメッセージで届くので、古い解像度が残っている状態を作る。
	obs.people[0].width = 1280;
	obs.people[0].height = 720;
	obs.people[0].fps = 30.0;
	// row_of() は渡した View の中を指すので、View を変数に入れてから読む。
	dockview::DockViewV2 v = decide_dock(obs);
	const dockview::PersonRow *row = row_of(v, obs.people[0].user_id);
	REQUIRE(row != nullptr);
	REQUIRE(row->quality.has_value());
	CHECK(row->quality->id == keys::kDockRowCantShowStatus);
	CHECK_FALSE(row->resolution.has_value());
	// ほかの段階では、届いている解像度を表示する (画質を下げた映像が映っている)。
	obs.people[0].stage = std::string(kStageDownscaled);
	v = decide_dock(obs);
	row = row_of(v, obs.people[0].user_id);
	REQUIRE(row != nullptr);
	REQUIRE(row->resolution.has_value());
	CHECK(row->resolution->id == keys::kDockRowResolutionStatus);
}

TEST_CASE("解像度は最初のフレームが届いた後だけ表示し、画質のテキストは枠に載せていない人にも表示する")
{
	const auto row_in = [](const dockview::DockViewV2 &view, const char *user_id) {
		const dockview::PersonRow *row = row_of(view, user_id);
		REQUIRE(row != nullptr);
		return row;
	};
	// 枠に載っていて最初のフレームを待つ人: 解像度 (configure で届いたストリームの値) があっても表示しない。
	Observation obs = healthy();
	PersonFact &p = obs.people[0];
	p.last_video_frame_at_us.reset();
	p.stage = std::string(kStageDownscaled);
	p.width = 1280;
	p.height = 720;
	p.fps = 30.0;
	dockview::DockViewV2 v = decide_dock(obs);
	const dockview::PersonRow *row = row_in(v, "100");
	REQUIRE(row->status.has_value());
	CHECK(row->status->state == dockview::RowState::Waiting);
	REQUIRE(row->quality.has_value());
	CHECK(row->quality->id == keys::kDockRowResolutionReducedStatus);
	CHECK_FALSE(row->resolution.has_value());
	// 最初のフレームが届いた -> 表示する。
	p.last_video_frame_at_us = kNow - kSec / 10;
	v = decide_dock(obs);
	row = row_in(v, "100");
	REQUIRE(row->resolution.has_value());
	CHECK(row->resolution->id == keys::kDockRowResolutionStatus);
	CHECK(row->resolution->args == std::vector<std::string>{"1280", "720", "30"});
	// 届いていた映像が止まった後も表示する (届いていた映像の解像度)。
	p.last_video_frame_at_us = kNow - 5 * kSec;
	v = decide_dock(obs);
	row = row_in(v, "100");
	REQUIRE(row->resolution.has_value());

	// 枠に載せていない人: 画質のテキストは表示する (枠に載せる前に分かる事実)。録画の入力にもフレームがなければ、
	// 解像度は表示しない。
	PersonFact &q = obs.people[1];
	q.sharing = true;
	q.sharing_since_us = kNow - 20 * kSec;
	q.stage = std::string(kStageDownscaled);
	q.width = 1920;
	q.height = 1080;
	q.fps = 60.0;
	v = decide_dock(obs);
	row = row_in(v, "200");
	CHECK_FALSE(row->checked);
	CHECK_FALSE(row->status.has_value());
	REQUIRE(row->quality.has_value());
	CHECK(row->quality->id == keys::kDockRowResolutionReducedStatus);
	CHECK_FALSE(row->resolution.has_value());
	// 段階が unavailable でも同じ (枠に載せても表示できないと分かる)。
	q.stage = std::string(kStageUnavailable);
	v = decide_dock(obs);
	row = row_in(v, "200");
	REQUIRE(row->quality.has_value());
	CHECK(row->quality->id == keys::kDockRowCantShowStatus);
	CHECK_FALSE(row->resolution.has_value());
	// cordmux の録画の入力にフレームが届いている -> 届いている映像の解像度を表示する。
	q.stage = std::string(kStageDownscaled);
	obs.choices.recording = FeatureSwitch::On;
	obs.recording.phase = RecordPhase::Recording;
	q.last_record_frame_at_us = kNow - kSec / 10;
	v = decide_dock(obs);
	row = row_in(v, "200");
	REQUIRE(row->resolution.has_value());
	CHECK(row->resolution->args == std::vector<std::string>{"1920", "1080", "60"});
	// 録画の入力にフレームがあっても、段階が unavailable の間は表示しない (送られていない映像の解像度になる)。
	q.stage = std::string(kStageUnavailable);
	v = decide_dock(obs);
	row = row_in(v, "200");
	CHECK_FALSE(row->resolution.has_value());
}

TEST_CASE("session.json を保存できなかったメッセージは、メッセージのレベル 2 の 1 枚で、待ちもボタンも段階の行もない")
{
	Observation obs = not_connected();
	obs.install.process = VesktopProcess::Running;
	obs.listener.state = ListenerState::Listening;
	obs.listener.session_file_not_saved = true;
	const dockview::DockViewV2 v = decide_dock(obs);
	const std::vector<dockview::Notice> n = notices_of(v, "SessionFileNotSaved");
	REQUIRE(n.size() == 1);
	CHECK(n[0].tier == dockview::Tier::L2);
	CHECK(n[0].headline.id == keys::kDockSessionNotSavedHeadline);
	REQUIRE(n[0].body.size() == 1);
	CHECK(n[0].body[0].id == keys::kDockSessionNotSavedText);
	CHECK(n[0].buttons.empty());
	CHECK(n[0].stages.empty());
	CHECK(n[0].settle == dockview::Settle::Immediate);
	// 同じ原因の「Vesktopは起動していますが、接続されていません」は表示しない (1 件にまとめる)。
	CHECK(notices_of(v, "VesktopRunningNotLinked").empty());
	// 古い session.json のトークンで接続してきて切られた場合も、こちらにする (原因と違う
	// 「Vesktopとの接続が切れました」を表示しない)。
	obs.link.state = LinkState::Disconnected;
	obs.link.connected_at_us = kNow - 10 * kSec;
	obs.link.disconnected_at_us = kNow - 9 * kSec;
	obs.link.reason = DisconnectReason::SecretMismatch;
	CHECK(primary_rule_of(obs) == PrimaryRuleId::SessionFileNotSaved);
	// 接続していれば表示しない (万一接続したときに「Vesktopから接続できません」は事実と違う)。
	{
		Observation connected = healthy();
		connected.listener.state = ListenerState::Listening;
		connected.listener.session_file_not_saved = true;
		CHECK(primary_rule_of(connected) == PrimaryRuleId::Connected);
		CHECK(notices_of(decide_dock(connected), "SessionFileNotSaved").empty());
	}
	// 待ち受けを閉じていれば (機能を無効にした)、この行にならない。
	obs.listener.state = ListenerState::Closed;
	CHECK(primary_rule_of(obs) != PrimaryRuleId::SessionFileNotSaved);
}

TEST_CASE("録画の途切れは 1 件ごとに 1 つのメッセージにし、分は切り上げ、別の日なら日付と時刻の引数にする")
{
	Observation obs = healthy();
	obs.recording.gaps = {{kNow - 600 * kSec, 61 * kSec}, {kNow - 2 * 86400 * kSec, 30 * kSec}};
	const dockview::DockViewV2 v = decide_dock(obs);
	const std::vector<dockview::Notice> n = notices_of(v, "RecordGap");
	REQUIRE(n.size() == 2);
	CHECK(n[0].headline.id == keys::kDockRecordGapHeadline.other);
	CHECK(n[0].headline.args[1] == "2");
	CHECK(n[1].headline.id == keys::kDockRecordGapHeadline.one);
	dockview::TimeArgKind kind = dockview::TimeArgKind::Time;
	REQUIRE(dockview::parse_time_arg(n[1].headline.args[0], &kind, nullptr));
	CHECK(kind == dockview::TimeArgKind::DateTime);
	CHECK(n[0].key != n[1].key);
}

TEST_CASE("参加者の設定がある過去の参加者を上限で削除したときだけ、削除した人数のメッセージを出し、1 人のときは単数の鍵にする")
{
	Observation obs = healthy();
	CHECK(notices_of(decide_dock(obs), "SeenDiscarded").empty());

	obs.seen.discarded_with_marks = 2;
	std::vector<dockview::Notice> n = notices_of(decide_dock(obs), "SeenDiscarded");
	REQUIRE(n.size() == 1);
	CHECK(n[0].tier == dockview::Tier::L3);
	CHECK(n[0].headline.id == keys::kDockPastRemovedHeadline.other);
	CHECK(n[0].headline.args[0] == "2");
	REQUIRE(n[0].tip.has_value());
	CHECK(n[0].tip->id == keys::kDockPastRemovedText);

	obs.seen.discarded_with_marks = 1;
	n = notices_of(decide_dock(obs), "SeenDiscarded");
	REQUIRE(n.size() == 1);
	CHECK(n[0].headline.id == keys::kDockPastRemovedHeadline.one);
}

// ---- 人の行 ----

TEST_CASE("人の行の状態は 1 つで、画面共有していない > まだ届いていない > 届かなくなった の順に選ぶ")
{
	Observation obs = healthy();
	PersonFact &p = obs.people[0];
	// 平常は何も表示しない。
	CHECK_FALSE(row_of(decide_dock(obs), "100")->status.has_value());
	p.last_video_frame_at_us = kNow - 5 * kSec;
	CHECK(row_of(decide_dock(obs), "100")->status->state == dockview::RowState::NoVideo);
	CHECK(row_of(decide_dock(obs), "100")->status->settle == dockview::Settle::ClearOnly);
	p.last_video_frame_at_us.reset();
	CHECK(row_of(decide_dock(obs), "100")->status->state == dockview::RowState::Waiting);
	// 画面共有しているかが分かっていない間は、「画面共有していません」と言わない。
	p.sharing = false;
	p.sharing_known = false;
	CHECK(row_of(decide_dock(obs), "100")->status->state == dockview::RowState::Waiting);
	p.sharing_known = true;
	CHECK(row_of(decide_dock(obs), "100")->status->state == dockview::RowState::NotSharing);
	// 枠に載せていない人には表示しない。
	CHECK_FALSE(row_of(decide_dock(obs), "200")->status.has_value());
}

TEST_CASE("Vesktop のアカウントが通話に参加していないと分かっている間は、「画面共有していません」を表示しない")
{
	Observation obs = healthy();
	PersonFact &p = obs.people[0];
	p.sharing = false;
	p.sharing_known = true;
	// 通話に参加している: 今までどおり表示する。
	CHECK(row_of(decide_dock(obs), "100")->status->state == dockview::RowState::NotSharing);

	// 通話に参加していない: 誰の画面共有も見えないので、していないと分からない。状態を空にする。
	obs.call.presence = Presence::NotInCall;
	obs.call.roster_count = 0;
	CHECK_FALSE(row_of(decide_dock(obs), "100")->status.has_value());

	// 段階の値が残っていても、画質のテキストを表示しない (前の画面共有のときの値)。
	p.stage = std::string(kStageUnavailable);
	CHECK_FALSE(row_of(decide_dock(obs), "100")->quality.has_value());
	obs.call.presence = Presence::InCall;
	obs.call.roster_count = 2;
	CHECK(row_of(decide_dock(obs), "100")->status->state == dockview::RowState::NotSharing);
	p.stage.reset();

	// 通話の参加者をまだ読めていない間と、接続していない間は、今までどおり。
	obs.call.presence = Presence::Unknown;
	obs.call.roster_count.reset();
	CHECK(row_of(decide_dock(obs), "100")->status->state == dockview::RowState::NotSharing);
	obs.call.presence = Presence::NotInCall;
	obs.link.state = LinkState::Disconnected;
	CHECK(row_of(decide_dock(obs), "100")->status->state == dockview::RowState::NotSharing);
}

TEST_CASE("段階が unavailable の間は、しきい値を超えてもフレームがなくても、人の行は「映像を表示できません」になる")
{
	// 人の行の状態が空で、画質のテキストが「映像を表示できません」である (画質のテキストは、状態がないときに
	// 人の行に表示される)。
	const auto says_cant_show = [](const Observation &o) {
		const dockview::DockViewV2 v = decide_dock(o);
		const dockview::PersonRow *row = row_of(v, "100");
		REQUIRE(row != nullptr);
		return !row->status.has_value() && row->quality.has_value() && row->quality->id == keys::kDockRowCantShowStatus;
	};
	const auto state_of = [](const Observation &o) {
		const dockview::DockViewV2 v = decide_dock(o);
		const dockview::PersonRow *row = row_of(v, "100");
		REQUIRE(row != nullptr);
		REQUIRE(row->status.has_value());
		return row->status->state;
	};
	Observation obs = healthy();
	PersonFact &p = obs.people[0];
	p.stage = std::string(kStageUnavailable);
	// しきい値の時間内。
	CHECK(says_cant_show(obs));
	// しきい値を超えた: 「映像が届いていません」にしない (メッセージが始め直しを頼む人と、人の行のテキストがそろう)。
	p.last_video_frame_at_us = kNow - 5 * kSec;
	CHECK(says_cant_show(obs));
	// フレームが 1 つも届いていない (最初の確保で段階が尽きた人): 「映像を待っています...」にしない。
	p.last_video_frame_at_us.reset();
	CHECK(says_cant_show(obs));
	// 画面共有しているかが分かっていない間も同じ (画面共有していないとは言わない)。
	p.sharing = false;
	p.sharing_known = false;
	CHECK(says_cant_show(obs));
	// 画面共有していないと分かっている人は NotSharing が先 (段階は、前の画面共有のときの値が残っているだけ)。
	p.sharing_known = true;
	CHECK(state_of(obs) == dockview::RowState::NotSharing);
	p.sharing = true;
	CHECK(says_cant_show(obs));
	// ほかの段階と知らない名前の段階では、しきい値を超えれば「映像が届いていません」(画質を下げた映像が届いて
	// いた人)。
	p.last_video_frame_at_us = kNow - 5 * kSec;
	for (const char *stage : {kStageSoftware, "experimental"}) {
		p.stage = std::string(stage);
		CHECK(state_of(obs) == dockview::RowState::NoVideo);
	}
	// 段階が unavailable でなくなった (新しい画面共有で、最初の段階から確保し直した) ら、届き方に応じた
	// テキストに戻る。
	p.stage.reset();
	CHECK(state_of(obs) == dockview::RowState::NoVideo);
	p.last_video_frame_at_us.reset();
	CHECK(state_of(obs) == dockview::RowState::Waiting);
}

TEST_CASE("人の行は表示する名前の自然順に並び、チェック・画面共有・画質・解像度を持つ")
{
	Observation obs = healthy();
	obs.people.push_back(present("300", "user10"));
	obs.people.push_back(present("400", "user9"));
	obs.people[0].stage = std::string(kStageSoftware);
	obs.people[0].width = 1280;
	obs.people[0].height = 720;
	obs.people[0].fps = 30.0;
	const dockview::DockViewV2 v = decide_dock(obs);
	std::vector<std::string> order;
	for (const dockview::PersonRow &r : v.people)
		order.push_back(r.name.literal);
	CHECK(order == std::vector<std::string>{"hanako", "taro", "user9", "user10"});
	const dockview::PersonRow *taro = row_of(v, "100");
	CHECK(taro->checked);
	CHECK(taro->sharing);
	REQUIRE(taro->quality.has_value());
	CHECK(taro->quality->id == keys::kDockRowBothReducedStatus);
	REQUIRE(taro->resolution.has_value());
	CHECK(taro->resolution->id == keys::kDockRowResolutionStatus);
	CHECK(taro->resolution->args == std::vector<std::string>{"1280", "720", "30"});
	const dockview::PersonRow *hanako = row_of(v, "200");
	CHECK_FALSE(hanako->checked);
	CHECK_FALSE(hanako->sharing);
	CHECK(hanako->check_action.id == ActionId::SetSlot);
	CHECK(hanako->check_action.args.slot == 2);
	// 画面共有していない人のチェックは押せない (薄く表示し、理由を言う)。
	CHECK_FALSE(hanako->check_enabled);
	REQUIRE(hanako->check_action.disabled_reason.has_value());
	CHECK(hanako->check_action.disabled_reason->id == keys::kDockRowCheckNotSharingTip);
	CHECK(natural_less("user9", "user10"));
}

TEST_CASE("画面共有していない人のチェックは押せず、共有を始めると押せる。外す向きは共有していなくても押せる")
{
	Observation obs = healthy();
	// 画面共有していない人は、空き枠があっても載せられない。ledger::apply でも断る。
	ActionArgs add;
	add.user_id = "200";
	add.kind = cordmux::config::SlotKind::Screen;
	add.slot = 2;
	const Intent refused = apply(obs, ActionId::SetSlot, add);
	REQUIRE(refused.disabled.has_value());
	CHECK(std::string(refused.disabled->reason_text_id) == keys::kDockRowCheckNotSharingTip);
	ActionArgs replace;
	replace.user_id = "200";
	replace.replace_user_id = "100";
	replace.slot = 1;
	const Intent replace_refused = apply(obs, ActionId::ReplaceSlot, replace);
	REQUIRE(replace_refused.disabled.has_value());
	CHECK(std::string(replace_refused.disabled->reason_text_id) == keys::kDockRowCheckNotSharingTip);

	// 共有を始めると載せられる。
	obs.people[1].sharing = true;
	CHECK(apply(obs, ActionId::SetSlot, add).can_run());
	CHECK(row_of(decide_dock(obs), "200")->check_enabled);

	// 共有が分からない間 (facts が届いていない) と、Vesktop のアカウントが通話にいない間は、していないとは分からないので押せる。
	obs.people[1].sharing = false;
	obs.people[1].sharing_known = false;
	CHECK(apply(obs, ActionId::SetSlot, add).can_run());
	obs.people[1].sharing_known = true;
	obs.call.presence = Presence::NotInCall;
	CHECK(apply(obs, ActionId::SetSlot, add).can_run());

	// 載っている人は、共有をやめていても外せる。
	Observation stale = healthy();
	stale.people[0].sharing = false;
	ActionArgs remove;
	remove.user_id = "100";
	remove.kind = cordmux::config::SlotKind::Screen;
	remove.slot = 0;
	CHECK(apply(stale, ActionId::SetSlot, remove).can_run());
}

TEST_CASE("人の行の 2 行目は、声・Discord での名前・同じ名前の人のユーザー名を、区切りでつながずに別の部分として持つ")
{
	Observation obs = healthy();
	// taro に「TARO」の上書きと、hanako の声。
	SeenPerson taro;
	taro.user_id = "100";
	taro.display_name = "taro";
	taro.name_override = "TARO";
	taro.voice_source_user_id = "200";
	obs.seen.people.push_back(taro);
	obs.people[0].display_name = "TARO";
	// 同じ Discord での名前の人がもう 1 人 (ユーザー名で見分ける)。
	PersonFact twin = present("300", "taro");
	twin.username = "taro_2";
	obs.people.push_back(twin);
	obs.people[0].username = "taro_1";
	const dockview::DockViewV2 v = decide_dock(obs);
	const dockview::PersonRow *row = row_of(v, "100");
	REQUIRE(row != nullptr);
	REQUIRE(row->line2_parts.size() == 3);
	CHECK(row->line2_parts[0].id == keys::kDockRowVoiceStatus);
	CHECK(row->line2_parts[0].args == std::vector<std::string>{"hanako"});
	CHECK(row->line2_parts[1].id == keys::kDockRowDiscordNameStatus);
	CHECK(row->line2_parts[1].args == std::vector<std::string>{"taro"});
	CHECK(row->line2_parts[2].id == keys::kDockRowUsernameStatus);
	CHECK(row->line2_parts[2].args == std::vector<std::string>{"taro_1"});
	CHECK(row->same_discord_name_exists);
	CHECK_FALSE(row->same_visible_name_exists);
	for (const TextRef &t : row->line2_parts)
		CHECK(t.id != keys::kDockRowDiscordNameUsernameStatus);
	// 使う声のアカウントが通話に参加していないときは、「声: %1 (通話に参加していません)」の形にする。
	Observation away = obs;
	away.people.erase(away.people.begin() + 1); // hanako が抜けた
	SeenPerson hanako;
	hanako.user_id = "200";
	hanako.display_name = "hanako";
	away.seen.people.push_back(hanako);
	const dockview::DockViewV2 away_view = decide_dock(away);
	const dockview::PersonRow *row2 = row_of(away_view, "100");
	REQUIRE_FALSE(row2->line2_parts.empty());
	CHECK(row2->line2_parts[0].id == keys::kDockRowVoiceAbsentStatus);
}

TEST_CASE("Vesktop のアカウントの行は、チェックもメニューも持たず、"
	  "2 行目に Vesktop のアカウントであることだけを表示する")
{
	Observation obs = healthy();
	obs.people[1].is_receiver = true;
	// メインアカウントと別のアカウントで Vesktop を使う構成。
	obs.choices.broadcaster_user_id = "100";
	const dockview::DockViewV2 v = decide_dock(obs);
	const dockview::PersonRow *row = row_of(v, "200");
	CHECK_FALSE(row->checkable);
	CHECK_FALSE(row->check_enabled);
	REQUIRE(row->line2_parts.size() == 1);
	CHECK(row->line2_parts[0].id == keys::kDockRowReceivingAccountStatus);
	CHECK(row->menu.voices.empty());
	// 参加者の設定ウィンドウには表示しない。
	for (const dockview::ParticipantRow &p : v.participants)
		CHECK(p.user_id != "200");
}

TEST_CASE("枠がすべて使用中のときは、枠に載せていない人のチェックだけを押せなくし、理由を一覧の上の 1 行にする")
{
	Observation obs = healthy();
	obs.people.push_back(showing("300", "b", 2));
	obs.people.push_back(showing("400", "c", 3));
	obs.people.push_back(showing("500", "d", 4));
	obs.call.roster_count = 5;
	obs.people[1].sharing = true; // 画面共有している人だけが載せられる
	const dockview::DockViewV2 v = decide_dock(obs);
	CHECK(v.list_full);
	REQUIRE(v.list_reason.has_value());
	CHECK(v.list_reason->text.id == keys::kDockNoFreeSlotText);
	REQUIRE(v.list_reason->short_text.has_value());
	CHECK(v.list_reason->short_text->id == keys::kDockNoFreeSlotShortText);
	const dockview::PersonRow *hanako = row_of(v, "200");
	CHECK_FALSE(hanako->check_enabled);
	REQUIRE(hanako->check_action.disabled_reason.has_value());
	CHECK(hanako->check_action.disabled_reason->id == keys::kDockNoFreeSlotText);
	CHECK(row_of(v, "100")->check_enabled); // 枠に載せている人は外せる

	// 押した直後に枠が埋まっていた人の行のそばの 1 行 (短い形。全文は一覧の上の行にある)。
	Observation refused = obs;
	refused.ops.last_slot_refused_user_id = "200";
	const dockview::DockViewV2 refused_view = decide_dock(refused);
	const dockview::PersonRow *r = row_of(refused_view, "200");
	REQUIRE(r->refused_note.has_value());
	CHECK(r->refused_note->id == keys::kDockNoFreeSlotShortText);
}

namespace {

// 枠が 4 つとも埋まっていて、画面共有している hanako (200) を押したが載せられなかった直後。
Observation refused_with_full_slots()
{
	Observation obs = healthy();
	obs.people.push_back(showing("300", "b", 2));
	obs.people.push_back(showing("400", "c", 3));
	obs.people.push_back(showing("500", "d", 4));
	obs.call.roster_count = 5;
	obs.people[1].sharing = true;
	obs.ops.last_slot_refused_user_id = "200";
	return obs;
}

// DockPoll の周期と同じく、Observation を作った直後に返事を終えるかを決め、持ち続ける値を返す (dock/dock_poll.cpp の tick)。
// 返った ops を次の周期の Observation に入れるのは、テストの側で行う。
OperationFacts settle_ops(Observation *obs)
{
	obs->ops = settle_operation_facts(*obs);
	return obs->ops;
}

bool refused_note_shown(const Observation &obs, const std::string &user_id)
{
	const dockview::DockViewV2 v = decide_dock(obs);
	const dockview::PersonRow *row = row_of(v, user_id);
	return row != nullptr && row->refused_note.has_value();
}

} // namespace

TEST_CASE("載せられなかった返事は、空いている枠ができたら終え、後でまた枠が埋まっても、前に押した人の行に出さない")
{
	// 押した直後は、枠が埋まっている間ずっと出る (周期を重ねても消えない)。
	Observation obs = refused_with_full_slots();
	CHECK_FALSE(slot_refusal_finished(obs));
	for (int i = 0; i < 3; ++i) {
		CHECK(settle_ops(&obs).last_slot_refused_user_id == "200");
		CHECK(refused_note_shown(obs, "200"));
	}

	// d (500) が画面共有をやめて枠が空いた。返事は事実でなくなるので終え、この周期から出さない。
	Observation freed = obs;
	freed.people.pop_back();
	freed.call.roster_count = 4;
	CHECK(slot_refusal_finished(freed));
	CHECK(settle_ops(&freed).last_slot_refused_user_id.empty());
	CHECK_FALSE(refused_note_shown(freed, "200"));

	// しばらく後に、別の人 (600) が自動で空いた枠に載り、また埋まった。hanako は押し直していないので出さない。
	Observation refilled = freed;
	refilled.people.push_back(showing("600", "e", 4));
	refilled.call.roster_count = 5;
	CHECK_FALSE(slot_refusal_finished(refilled));
	CHECK(settle_ops(&refilled).last_slot_refused_user_id.empty());
	CHECK_FALSE(refused_note_shown(refilled, "200"));
	// 枠が埋まっていることは、ほかの人と同じ押せないチェックと一覧の上の 1 行が伝える。
	const dockview::DockViewV2 v = decide_dock(refilled);
	const dockview::PersonRow *hanako = row_of(v, "200");
	REQUIRE(hanako != nullptr);
	REQUIRE(hanako->check_action.disabled_reason.has_value());
	CHECK(hanako->check_action.disabled_reason->id == keys::kDockNoFreeSlotText);
	REQUIRE(v.list_reason.has_value());
	CHECK(v.list_reason->text.id == keys::kDockNoFreeSlotText);
}

TEST_CASE("載せられなかった返事は、その人が載った、画面共有をやめた、通話を抜けた、接続が切れたときにも終える")
{
	SUBCASE("その人が枠に載った (入れ替えで載った。押した操作の結果が出た)")
	{
		Observation obs = refused_with_full_slots();
		obs.people[2].slot.reset();
		obs.people[1].slot = 2;
		obs.people[1].slot_visible = true;
		CHECK(slot_refusal_finished(obs));
		CHECK(settle_ops(&obs).last_slot_refused_user_id.empty());
		// 後でその人が枠から外れ、枠がまた埋まっても出さない。
		obs.people[1].slot.reset();
		obs.people[2].slot = 2;
		CHECK_FALSE(refused_note_shown(obs, "200"));
	}
	SUBCASE("その人が画面共有をやめた (チェックを押せない理由が、画面共有していないことになった)")
	{
		Observation obs = refused_with_full_slots();
		obs.people[1].sharing = false;
		CHECK(slot_refusal_finished(obs));
		CHECK_FALSE(refused_note_shown(obs, "200"));
		CHECK(settle_ops(&obs).last_slot_refused_user_id.empty());
		// 画面共有を始め直しても、押し直すまで出さない。
		obs.people[1].sharing = true;
		CHECK_FALSE(refused_note_shown(obs, "200"));
	}
	SUBCASE("その人が通話を抜けた (参加者の一覧にいない)")
	{
		Observation obs = refused_with_full_slots();
		obs.people.erase(obs.people.begin() + 1);
		CHECK(slot_refusal_finished(obs));
		CHECK(settle_ops(&obs).last_slot_refused_user_id.empty());
	}
	SUBCASE("接続が切れた ([移動] と [退出] と同じく、結果を見られない)")
	{
		Observation obs = refused_with_full_slots();
		obs.link.state = LinkState::Disconnected;
		CHECK(slot_refusal_finished(obs));
		CHECK(settle_ops(&obs).last_slot_refused_user_id.empty());
	}
	SUBCASE("画面共有しているかが分からない間は、終えない (分からないことを、やめたとは扱わない)")
	{
		Observation obs = refused_with_full_slots();
		obs.people[1].sharing = false;
		obs.people[1].sharing_known = false;
		CHECK_FALSE(slot_refusal_finished(obs));
		CHECK(settle_ops(&obs).last_slot_refused_user_id == "200");
	}
}

TEST_CASE("載せられなかった返事は、参加者の一覧も画面共有の事実も読めない間は、その人の行がなくても終えない")
{
	// 接続したまま、送信側が参加者の一覧と画面共有の事実を読めなかった (facts の roster と sharing のキーがない)。枠に載って
	// いる 4 人は枠から行ができるが、hanako (200) は枠にも一覧にもいないので、行がない。通話を抜けたとは分からない。
	Observation obs = refused_with_full_slots();
	obs.people.erase(obs.people.begin() + 1);
	obs.call.roster_count.reset();
	obs.call.presence = Presence::Unknown;
	for (PersonFact &person : obs.people) {
		person.in_roster = false;
		person.sharing = false;
		person.sharing_known = false;
	}
	REQUIRE(free_screen_slot(obs) == 0);
	CHECK_FALSE(person_known_not_in_roster(obs, "200"));
	CHECK_FALSE(person_known_not_sharing(obs, "200"));
	for (int i = 0; i < 3; ++i) {
		CHECK_FALSE(slot_refusal_finished(obs));
		CHECK(settle_ops(&obs).last_slot_refused_user_id == "200");
	}

	SUBCASE("また読めるようになって、hanako がいて枠がまだ埋まっていれば、返事を出し続ける")
	{
		Observation back = refused_with_full_slots();
		back.ops = obs.ops;
		CHECK_FALSE(slot_refusal_finished(back));
		CHECK(refused_note_shown(back, "200"));
		CHECK(settle_ops(&back).last_slot_refused_user_id == "200");
	}
	SUBCASE("また読めるようになって、hanako が一覧にいなければ、通話を抜けたので終える")
	{
		Observation back = refused_with_full_slots();
		back.people.erase(back.people.begin() + 1);
		back.call.roster_count = 4;
		back.ops = obs.ops;
		CHECK(person_known_not_in_roster(back, "200"));
		CHECK(slot_refusal_finished(back));
		CHECK(settle_ops(&back).last_slot_refused_user_id.empty());
	}
	SUBCASE("読めない間でも、空いている枠ができれば終える (返事の文が事実でなくなった)")
	{
		Observation freed = obs;
		freed.people.pop_back();
		CHECK(slot_refusal_finished(freed));
		CHECK(settle_ops(&freed).last_slot_refused_user_id.empty());
	}
}

TEST_CASE("載せられなかった返事は、参加者の一覧を読めていてその人がいなければ、行が残っていても終える")
{
	SUBCASE("その人は一覧にいないが、前に届いたフレームで行が残っている (通話を抜けた)")
	{
		Observation obs = refused_with_full_slots();
		obs.people[1].in_roster = false;
		obs.people[1].last_video_frame_at_us = kNow - kSec;
		obs.call.roster_count = 4;
		CHECK(person_known_not_in_roster(obs, "200"));
		CHECK(slot_refusal_finished(obs));
		CHECK_FALSE(refused_note_shown(obs, "200"));
		CHECK(settle_ops(&obs).last_slot_refused_user_id.empty());
	}
	SUBCASE("Vesktop のアカウントが通話を抜けた (一覧が空。画面共有の事実は分からないことになる)")
	{
		Observation obs = refused_with_full_slots();
		obs.call.presence = Presence::NotInCall;
		obs.call.roster_count = 0;
		for (PersonFact &person : obs.people)
			person.in_roster = false;
		REQUIRE(free_screen_slot(obs) == 0);
		CHECK_FALSE(person_known_not_sharing(obs, "200"));
		CHECK(person_known_not_in_roster(obs, "200"));
		CHECK(slot_refusal_finished(obs));
		CHECK(settle_ops(&obs).last_slot_refused_user_id.empty());
	}
}

TEST_CASE("返事を終えるかを決めても、終えていない返事と、ほかの操作の結果は変えない")
{
	// 別の人が載せられなかったときは、DockPoll がその人に替える。返事を終えるかは、替わった人で決める。
	Observation obs = refused_with_full_slots();
	obs.people.push_back(present("600", "e"));
	obs.people.back().sharing = true;
	obs.call.roster_count = 6;
	obs.ops.last_slot_refused_user_id = "600";
	CHECK(settle_ops(&obs).last_slot_refused_user_id == "600");
	CHECK(refused_note_shown(obs, "600"));
	CHECK_FALSE(refused_note_shown(obs, "200"));

	// 押した [移動] は、結果が出ていなければ持ち続け、載せられなかった返事を終えても残る。
	obs.ops.call_action = CallActionPress{CallActionKind::Move, kNow - kSec};
	obs.ops.call_action_timeout_us = 7 * kSec;
	obs.people.pop_back();
	const OperationFacts kept = settle_ops(&obs);
	CHECK(kept.last_slot_refused_user_id.empty());
	REQUIRE(kept.call_action.has_value());
	CHECK(kept.call_action->kind == CallActionKind::Move);
	CHECK(kept.call_action->pressed_at_us == kNow - kSec);
	CHECK(kept.call_action_timeout_us == 7 * kSec);

	// 押していなければ、終える返事もない。
	Observation none = healthy();
	CHECK_FALSE(slot_refusal_finished(none));
	CHECK(settle_ops(&none).last_slot_refused_user_id.empty());
}

TEST_CASE("自分の枠を使っている間は枠に載せられるのが 3 人までで、一覧の上の 1 行と押せないチェックになる")
{
	Observation obs = healthy();
	// 自分の枠を使っている (映像を OBS から選び、自分の行のチェックが付いている)。
	obs.choices.self_frame_selected = true;
	obs.choices.self_frame_bound = true;
	obs.choices.self_frame_on = true;
	obs.choices.self_frame_shown = true;
	obs.people.push_back(showing("300", "b", 2));
	obs.people.push_back(showing("400", "c", 3));
	obs.people[1].sharing = true; // 画面共有している人だけが載せられる
	const dockview::DockViewV2 v = decide_dock(obs);
	REQUIRE(v.list_reason.has_value());
	CHECK(v.list_reason->text.id == keys::kDockSelfSlotLimitText);
	// 狭いときの短い形は、事実だけの「枠に載せられるのは3人までです。」。
	REQUIRE(v.list_reason->short_text.has_value());
	CHECK(v.list_reason->short_text->id == keys::kDockSelfSlotLimitShortText);
	CHECK_FALSE(v.list_full);
	const dockview::PersonRow *hanako = row_of(v, "200");
	REQUIRE(hanako->check_action.disabled_reason.has_value());
	CHECK(hanako->check_action.disabled_reason->id == keys::kDockSelfSlotLimitText);

	// 選んだ映像がまだ結び付いていない間 (OBS の起動の途中、見つからない間) も、チェックが付いていれば同じ。結び付いた
	// 瞬間に自分の枠が描かれるので、その前に 4 人目を載せると 5 つになる。枠の数も、自分の枠を 1 つと数える。
	Observation unbound = obs;
	unbound.choices.self_frame_bound = false;
	unbound.choices.self_frame_shown = false;
	const dockview::DockViewV2 u = decide_dock(unbound);
	REQUIRE(u.list_reason.has_value());
	CHECK(u.list_reason->text.id == keys::kDockSelfSlotLimitText);
	REQUIRE(row_of(u, "200")->check_action.disabled_reason.has_value());
	CHECK(row_of(u, "200")->check_action.disabled_reason->id == keys::kDockSelfSlotLimitText);
	CHECK(u.people_list.slots_used == 4);

	// チェックを外せば、4 人目を載せられる。
	Observation off = unbound;
	off.choices.self_frame_on = false;
	const dockview::DockViewV2 o = decide_dock(off);
	CHECK_FALSE(o.list_reason.has_value());
	CHECK(row_of(o, "200")->check_enabled);
	CHECK(o.people_list.slots_used == 3);
}

TEST_CASE("メインアカウントだけの構成の自分の行は、2 行目に「サブアカウント」を表示しない")
{
	Observation obs = healthy();
	obs.people[1].is_receiver = true;
	// メインアカウントの userId が空 (メインアカウントだけの構成)。
	const dockview::DockViewV2 only_main = decide_dock(obs);
	CHECK(row_of(only_main, "200")->line2_parts.empty());
	// メインアカウントの userId が Vesktop のアカウントと同じでも、その人がメインアカウントである。
	obs.choices.broadcaster_user_id = "200";
	const dockview::DockViewV2 same = decide_dock(obs);
	CHECK(row_of(same, "200")->line2_parts.empty());
}

TEST_CASE("自分の枠に映す映像を OBS から選んでいる間、自分の行のチェックは自分の枠の付け外しになる")
{
	Observation obs = healthy();
	obs.people[0].slot.reset();
	obs.people[0].is_self_frame = true; // 別アカウント方式のメインアカウントの行
	obs.choices.self_frame_selected = true;
	obs.choices.self_frame_bound = true;
	obs.choices.self_frame_on = true;
	const dockview::DockViewV2 on = decide_dock(obs);
	const dockview::PersonRow *self = row_of(on, "100");
	REQUIRE(self != nullptr);
	CHECK(self->self_frame_check);
	CHECK(self->checkable);
	CHECK(self->checked); // 自分の枠を出している
	CHECK_FALSE(self->slot.has_value());
	CHECK(self->check_enabled);
	CHECK(self->check_action.id == ActionId::SetSelfFrame);
	CHECK_FALSE(self->check_action.args.on); // 押すと消す
	CHECK(self->check_action.args.user_id == "100");
	CHECK(self->check_action.label.id == keys::kDockRowCheckA11y);
	// [自動で載せる] を持つ (メニューはほかの人と同じ全部の項目)。
	CHECK(self->menu.always_on_toggle.id == ActionId::SetAlwaysOn);
	CHECK_FALSE(self->menu.auto_add_only);
	CHECK(self->menu.rename.id == ActionId::SetNameOverride);
	// ほかの人の行は、今までどおり枠に載せる。
	CHECK_FALSE(row_of(on, "200")->self_frame_check);
	CHECK(row_of(on, "200")->check_action.id == ActionId::SetSlot);

	// 消している間は、チェックが外れ、押すと出す。
	obs.choices.self_frame_on = false;
	const dockview::DockViewV2 off = decide_dock(obs);
	const dockview::PersonRow *self_off = row_of(off, "100");
	CHECK_FALSE(self_off->checked);
	CHECK(self_off->check_enabled);
	CHECK(self_off->check_action.id == ActionId::SetSelfFrame);
	CHECK(self_off->check_action.args.on);

	// 映像を選んでいて、まだ結び付いていない間 (見つからない間) も、チェックは自分の枠の付け外し。
	obs.choices.self_frame_bound = false;
	obs.choices.self_frame_on = true;
	const dockview::DockViewV2 unbound_view = decide_dock(obs);
	const dockview::PersonRow *unbound = row_of(unbound_view, "100");
	CHECK(unbound->self_frame_check);
	CHECK(unbound->checked);
	CHECK(unbound->check_action.id == ActionId::SetSelfFrame);
}

TEST_CASE("自分の枠のチェックが付いていて、選んだ映像が見つからない間は、自分の行の状態がそれを伝える")
{
	Observation obs = healthy();
	obs.people[1].is_self_frame = true; // 200 が自分の枠の人。枠に載っていない
	obs.people[1].sharing_known = true;
	obs.people[1].sharing = false;
	obs.choices.composite_checked = true;
	obs.choices.composite_present = true;
	obs.choices.self_frame_selected = true;
	obs.choices.self_frame_bound = false;
	obs.choices.self_frame_on = true;
	const dockview::DockViewV2 missing = decide_dock(obs);
	const dockview::PersonRow *row = row_of(missing, "200");
	REQUIRE(row != nullptr);
	REQUIRE(row->status.has_value());
	CHECK(row->status->state == dockview::RowState::NoVideo);
	CHECK(row->status->text.id == keys::kDockRowSelfVideoMissingStatus);
	CHECK(row->status->tip.id == keys::kDockRowSelfVideoMissingTip);
	CHECK(row->status->settle == dockview::Settle::Debounced);
	CHECK(row->checked);
	// ほかの人の行には表示しない。
	const dockview::PersonRow *other = row_of(missing, "100");
	REQUIRE(other != nullptr);
	CHECK_FALSE(other->status.has_value());

	// メインアカウントだけの構成の自分の行 (Vesktop のアカウント) にも表示する。
	{
		Observation solo = obs;
		solo.people[1].is_receiver = true;
		const dockview::DockViewV2 v = decide_dock(solo);
		REQUIRE(row_of(v, "200")->status.has_value());
		CHECK(row_of(v, "200")->status->text.id == keys::kDockRowSelfVideoMissingStatus);
	}

	// 表示しない場合。どれも、見つからないとは言えないか、何も映らないのが当然の間である。
	const auto no_missing_status = [](const Observation &o) {
		const dockview::DockViewV2 v = decide_dock(o);
		const dockview::PersonRow *r = row_of(v, "200");
		return r != nullptr && (!r->status.has_value() || r->status->text.id != keys::kDockRowSelfVideoMissingStatus);
	};
	Observation bound = obs;
	bound.choices.self_frame_bound = true;
	CHECK(no_missing_status(bound));
	Observation off = obs;
	off.choices.self_frame_on = false;
	CHECK(no_missing_status(off));
	Observation discord_share = obs;
	discord_share.choices.self_frame_selected = false;
	CHECK(no_missing_status(discord_share));
	Observation loading = obs;
	loading.choices.composite_checked = false;
	loading.choices.composite_present = false;
	CHECK(no_missing_status(loading));
	Observation no_source = obs;
	no_source.choices.composite_present = false;
	CHECK(no_missing_status(no_source));
	Observation disabled = obs;
	disabled.choices.feature = FeatureSwitch::Off;
	CHECK(no_missing_status(disabled));
	Observation other_person = obs;
	other_person.people[1].is_self_frame = false;
	CHECK(no_missing_status(other_person));
}

TEST_CASE("映像を OBS から選んでいない間 (Discord の画面共有)、サブアカウントがあれば自分の行はほかの人と同じ行のまま")
{
	Observation obs = healthy();
	obs.people[1].is_self_frame = true;
	obs.choices.self_frame_selected = false;
	obs.choices.self_frame_bound = false;
	obs.choices.self_frame_on = true;
	const dockview::DockViewV2 v = decide_dock(obs);
	const dockview::PersonRow *row = row_of(v, "200");
	CHECK_FALSE(row->self_frame_check);
	CHECK_FALSE(row->checked);
	CHECK(row->checkable);
	CHECK(row->check_action.id == ActionId::SetSlot);
	// [自動で載せる] もほかの人と同じにある。
	CHECK(row->menu.always_on_toggle.id == ActionId::SetAlwaysOn);
	CHECK_FALSE(row->menu.auto_add_only);
}

TEST_CASE("サブアカウントがない構成の自分の行 (Vesktop のアカウント) は、映像を選んでいれば押せるチェックを持つ")
{
	Observation obs = healthy();
	obs.people[1].is_receiver = true;
	obs.people[1].is_self_frame = true;
	obs.choices.self_frame_selected = true;
	obs.choices.self_frame_bound = true;
	obs.choices.self_frame_on = false;
	const dockview::DockViewV2 v = decide_dock(obs);
	const dockview::PersonRow *row = row_of(v, "200");
	CHECK_FALSE(row->checkable); // 音量は出さない
	CHECK(row->self_frame_check);
	CHECK(row->check_enabled);
	CHECK(row->check_action.id == ActionId::SetSelfFrame);
	CHECK(row->check_action.args.on);
	CHECK(row->line2_parts.empty());
	// メニューは [自動で載せる] だけ。
	CHECK(row->menu.auto_add_only);
	CHECK(row->menu.always_on_toggle.id == ActionId::SetAlwaysOn);
}

TEST_CASE("サブアカウントがない構成の自分の行は、映像を選んでいなければチェックが押せず、理由は映像を選ぶ案内")
{
	Observation obs = healthy();
	obs.people[1].is_receiver = true;
	obs.people[1].is_self_frame = true;
	obs.choices.self_frame_selected = false;
	for (const bool on : {false, true}) {
		obs.choices.self_frame_on = on;
		const dockview::DockViewV2 view = decide_dock(obs);
		const dockview::PersonRow *row = row_of(view, "200");
		CHECK(row->self_frame_check); // チェックは常に出す
		CHECK_FALSE(row->checkable);
		CHECK_FALSE(row->check_enabled);
		REQUIRE(row->check_action.disabled_reason.has_value());
		CHECK(row->check_action.disabled_reason->id == keys::kDockRowSelfNoVideoTip);
		CHECK_FALSE(row->checked); // 枠に載らないので、残っている値があっても付けて見せない
		CHECK(row->menu.auto_add_only);
		CHECK(row->menu.always_on_toggle.id == ActionId::SetAlwaysOn);
	}
}

TEST_CASE("自分の行ではない Vesktop のアカウントの行は、チェックもメニューも持たない")
{
	Observation obs = healthy();
	obs.people[1].is_receiver = true;
	obs.people[1].is_self_frame = false;
	obs.choices.self_frame_selected = true;
	const dockview::DockViewV2 view = decide_dock(obs);
	const dockview::PersonRow *row = row_of(view, "200");
	CHECK_FALSE(row->self_frame_check);
	CHECK_FALSE(row->checkable);
	CHECK_FALSE(row->menu.auto_add_only);
}

TEST_CASE("自分の行のチェックは、Discord の人で枠が埋まっているとき、出す向きを押せず、理由は枠の空きがない文字列")
{
	Observation obs = healthy();
	obs.people.push_back(showing("300", "b", 2));
	obs.people.push_back(showing("400", "c", 3));
	obs.people.push_back(showing("500", "d", 4));
	obs.people[1].is_self_frame = true; // 200 が自分の枠の人。枠に載っていない
	obs.choices.self_frame_selected = true;
	obs.choices.self_frame_bound = true;
	obs.choices.self_frame_on = false;
	const dockview::DockViewV2 v = decide_dock(obs);
	const dockview::PersonRow *row = row_of(v, "200");
	CHECK_FALSE(row->check_enabled);
	REQUIRE(row->check_action.disabled_reason.has_value());
	CHECK(row->check_action.disabled_reason->id == keys::kDockNoFreeSlotText);
	// 出している間は、枠が埋まっていても消せる。
	obs.choices.self_frame_on = true;
	const dockview::DockViewV2 on = decide_dock(obs);
	CHECK(row_of(on, "200")->check_enabled);
}

TEST_CASE("人の行のメニューは自動で載せる・名前を変更・使う声で、現在の値にチェックを付け、押せない項目に理由を付ける")
{
	Observation obs = healthy();
	obs.people.push_back(present("300", "jiro"));
	// jiro の声は、通話に参加していない過去の参加者 ghost が使う声にしている (連鎖にならない形)。
	SeenPerson ghost;
	ghost.user_id = "800";
	ghost.display_name = "ghost";
	ghost.voice_source_user_id = "300";
	obs.seen.people.push_back(ghost);
	const dockview::DockViewV2 v = decide_dock(obs);
	const dockview::PersonRow *taro = row_of(v, "100");
	REQUIRE(taro != nullptr);
	CHECK_FALSE(taro->menu.always_on);
	CHECK(taro->menu.always_on_toggle.args.on);
	CHECK(taro->menu.always_on_toggle.label.id == keys::kDockRowAutoAddMenu);
	CHECK(taro->menu.rename.id == ActionId::SetNameOverride);
	CHECK(taro->menu.rename.args.name.empty());
	CHECK_FALSE(taro->menu.rename.disabled_reason.has_value());
	REQUIRE(taro->menu.voices.size() == 3);
	CHECK(taro->menu.voices[0].checked); // 本人の声
	CHECK_FALSE(taro->menu.voices[0].action.disabled_reason.has_value());
	// 候補は自然順で hanako、jiro。jiro は使えない (ghost が使っている)。
	CHECK(taro->menu.voices[1].label.literal == "hanako");
	CHECK_FALSE(taro->menu.voices[1].action.disabled_reason.has_value());
	REQUIRE(taro->menu.voices[2].label.text.has_value());
	CHECK(taro->menu.voices[2].label.text->id == keys::kDockRowVoiceTakenMenu);
	CHECK(taro->menu.voices[2].label.text->args == std::vector<std::string>{"jiro", "ghost"});
	REQUIRE(taro->menu.voices[2].action.disabled_reason.has_value());
	CHECK(taro->menu.voices[2].action.disabled_reason->id == keys::kParticipantsVoiceTakenText);
	CHECK(taro->menu.voices[2].action.disabled_reason->args == std::vector<std::string>{"jiro", "ghost"});

	// 選び方が「自動」の間は「自動で載せる」を押せない。参加者の設定ウィンドウでは、そのウィンドウ用のキーにする。
	Observation autom = obs;
	autom.choices.assign_mode = AssignMode::Auto;
	const dockview::DockViewV2 a = decide_dock(autom);
	REQUIRE(row_of(a, "100")->menu.always_on_toggle.disabled_reason.has_value());
	CHECK(row_of(a, "100")->menu.always_on_toggle.disabled_reason->id == keys::kDockRowAutoAddDisabledTip);
	REQUIRE_FALSE(a.participants.empty());
	REQUIRE(a.participants[0].always_on_toggle.disabled_reason.has_value());
	CHECK(a.participants[0].always_on_toggle.disabled_reason->id == keys::kParticipantsAutoAddDisabledTip);
}

TEST_CASE("人の行のメニューの [全画面で表示] は、描かれている人の行だけで押せ、全画面の人の行はチェックが付いて押すと戻す")
{
	Observation obs = healthy();
	obs.choices.focus_candidates = {"100"};
	const dockview::DockViewV2 v = decide_dock(obs);
	const dockview::PersonRow *taro = row_of(v, "100");
	const dockview::PersonRow *hanako = row_of(v, "200");
	REQUIRE(taro != nullptr);
	REQUIRE(hanako != nullptr);
	CHECK_FALSE(taro->menu.focus);
	CHECK(taro->menu.focus_toggle.id == ActionId::SetFocus);
	CHECK(taro->menu.focus_toggle.args.user_id == "100");
	CHECK(taro->menu.focus_toggle.label.id == keys::kDockRowFullScreenMenu);
	CHECK_FALSE(taro->menu.focus_toggle.disabled_reason.has_value());
	// 描かれていない人の行にも項目は出し、押せなくする。
	CHECK(hanako->menu.focus_toggle.id == ActionId::SetFocus);
	REQUIRE(hanako->menu.focus_toggle.disabled_reason.has_value());
	CHECK(hanako->menu.focus_toggle.disabled_reason->id == keys::kDockRowFullScreenUnavailableTip);

	// 全画面の人の行は、チェックが付き、押すと元に戻す (user_id が空)。
	obs.choices.focus_user_id = "100";
	const dockview::DockViewV2 f = decide_dock(obs);
	const dockview::PersonRow *focused = row_of(f, "100");
	CHECK(focused->menu.focus);
	CHECK(focused->menu.focus_toggle.args.user_id.empty());
	CHECK_FALSE(focused->menu.focus_toggle.disabled_reason.has_value());
	CHECK_FALSE(row_of(f, "200")->menu.focus);

	// サブアカウントがない構成の自分の行 ([自動で載せる] だけのメニュー) にも出す。自分の枠も全画面にできる。
	Observation self = healthy();
	self.people[1].is_receiver = true;
	self.people[1].is_self_frame = true;
	self.choices.self_frame_selected = true;
	self.choices.self_frame_bound = true;
	self.choices.self_frame_on = true;
	self.choices.self_frame_shown = true;
	self.choices.focus_candidates = {"200"};
	const dockview::DockViewV2 sv = decide_dock(self);
	const dockview::PersonRow *own = row_of(sv, "200");
	REQUIRE(own != nullptr);
	CHECK(own->menu.auto_add_only);
	CHECK(own->menu.focus_toggle.id == ActionId::SetFocus);
	CHECK(own->menu.focus_toggle.args.user_id == "200");
	CHECK_FALSE(own->menu.focus_toggle.disabled_reason.has_value());
}

TEST_CASE("人の行の「使う声」の候補に、Vesktop のアカウント (サブアカウント) は出ない")
{
	Observation obs = healthy();
	obs.people.push_back(present("300", "jiro"));
	// 200 が Vesktop のアカウント。声は受信側に届かないので、ほかの人の声の置き換え先にしない。
	obs.people[1].is_receiver = true;
	const dockview::DockViewV2 v = decide_dock(obs);
	const dockview::PersonRow *taro = row_of(v, "100");
	REQUIRE(taro != nullptr);
	// 本人の声と jiro だけ。
	REQUIRE(taro->menu.voices.size() == 2);
	for (const dockview::VoiceChoice &choice : taro->menu.voices)
		CHECK(choice.action.args.voice_user_id != "200");
	CHECK(taro->menu.voices[1].action.args.voice_user_id == "300");
	// 参加者の設定ウィンドウの「使う声」の候補にも出ない。
	for (const dockview::ParticipantRow &p : v.participants) {
		for (const dockview::VoiceChoice &choice : p.voices)
			CHECK(choice.action.args.voice_user_id != "200");
	}
}

// ---- 空の一覧 ----

TEST_CASE("空の一覧は 3 つの理由を書き分け、通話に Vesktop のアカウントしかいなければ Alone にする")
{
	Observation obs = not_connected();
	obs.people.clear();
	dockview::DockViewV2 v = decide_dock(obs);
	REQUIRE(v.empty.has_value());
	CHECK(v.empty->kind == dockview::EmptyKind::NotConnected);
	CHECK(v.empty->text.id == keys::kDockEmptyNotConnectedEmpty);

	Observation out_of_call = healthy();
	out_of_call.people.clear();
	out_of_call.call.presence = Presence::NotInCall;
	out_of_call.call.roster_count = 0;
	v = decide_dock(out_of_call);
	REQUIRE(v.empty.has_value());
	CHECK(v.empty->kind == dockview::EmptyKind::NotInCall);

	Observation alone = healthy();
	alone.people.clear();
	PersonFact me = present("900", "receiver");
	me.is_receiver = true;
	alone.people.push_back(me);
	alone.call.roster_count = 1;
	v = decide_dock(alone);
	REQUIRE(v.empty.has_value());
	CHECK(v.empty->kind == dockview::EmptyKind::Alone);
	CHECK(v.empty->text.id == keys::kDockEmptyAloneEmpty);

	CHECK_FALSE(decide_dock(healthy()).empty.has_value());
}

// ---- 録画のボタン ----

TEST_CASE("録画のボタンは 5 つの状態を持ち、状態が変わってから 1 秒までは前のテキストのままにする")
{
	Observation obs = healthy();
	dockview::DockViewV2 v = decide_dock(obs);
	CHECK(v.record.state == dockview::RecordButtonState::Idle);
	CHECK(v.record.label.id == keys::kDockRecordStartButton);
	CHECK_FALSE(v.record.pressed_look);
	CHECK(v.record.action.args.on);
	REQUIRE(v.record.width_labels.size() == 5);

	obs.recording.phase = RecordPhase::Starting;
	obs.recording.phase_since_us = kNow - 500 * 1000;
	v = decide_dock(obs);
	CHECK(v.record.state == dockview::RecordButtonState::Starting);
	CHECK(v.record.label.id == keys::kDockRecordStartButton); // 1 秒までは前のテキスト
	REQUIRE(v.record.action.disabled_reason.has_value());
	obs.recording.phase_since_us = kNow - 1500 * 1000;
	v = decide_dock(obs);
	CHECK(v.record.label.id == keys::kDockRecordStartingButton);

	obs.recording.phase = RecordPhase::Recording;
	obs.choices.recording = FeatureSwitch::On;
	v = decide_dock(obs);
	CHECK(v.record.state == dockview::RecordButtonState::Recording);
	CHECK(v.record.label.id == keys::kDockRecordStopButton);
	CHECK(v.record.pressed_look);
	CHECK_FALSE(v.record.action.args.on);
	CHECK(v.menu.feature_toggle.label.id == keys::kDockMenuDisableRecordingMenu);

	obs.recording.phase = RecordPhase::Stopping;
	obs.recording.phase_since_us = kNow - 200 * 1000;
	v = decide_dock(obs);
	CHECK(v.record.state == dockview::RecordButtonState::Stopping);
	CHECK(v.record.label.id == keys::kDockRecordStopButton);
	CHECK(v.record.pressed_look);
	obs.recording.phase_since_us = kNow - 3 * kSec;
	CHECK(decide_dock(obs).record.label.id == keys::kDockRecordStoppingButton);

	Observation space = healthy();
	space.recording.stopped_by_space_exhausted = true;
	CHECK(decide_dock(space).record.state == dockview::RecordButtonState::Failed);

	// 機能を無効にしている間は押せず、理由は 1 行目が伝える。
	Observation off = healthy();
	off.choices.feature = FeatureSwitch::Off;
	v = decide_dock(off);
	REQUIRE(v.record.action.disabled_reason.has_value());
	CHECK(v.record.action.disabled_reason->id == keys::kDockRecordDisabledOffText);
	CHECK(v.menu.feature_toggle.label.id == keys::kDockMenuEnableMenu);
	CHECK(v.menu.feature_toggle.args.on);
}

// ---- メニュー ----

TEST_CASE("ドックのメニューは 4 つの項目を持つ")
{
	const dockview::DockViewV2 v = decide_dock(healthy());
	CHECK(v.menu.open_settings.id == ActionId::OpenSettings);
	CHECK(v.menu.open_settings.args.page == SettingsPage::Recording);
	CHECK(v.menu.open_settings.label.id == keys::kDockMenuSettingsMenu);
	CHECK(v.menu.open_participants.id == ActionId::OpenParticipants);
	CHECK(v.menu.export_bundle.id == ActionId::ExportBundle);
	CHECK(v.menu.export_bundle.label.id == keys::kDockMenuExportDiagnosticsMenu);
	CHECK(v.menu.feature_toggle.label.id == keys::kDockMenuDisableMenu);
	CHECK_FALSE(v.menu.feature_toggle.args.on);
}

// ---- ダイアログの View ----

TEST_CASE("参加者の設定ウィンドウは、現在の参加者を先に、それぞれを最終参加日時の新しい順 (同じ時刻は自然順) に並べる")
{
	Observation obs = healthy();
	SeenPerson past;
	past.user_id = "700";
	past.display_name = "zeta";
	past.last_seen_at_us = kNow - 86400 * kSec;
	obs.seen.people.push_back(past);
	SeenPerson past2;
	past2.user_id = "800";
	past2.display_name = "alpha";
	obs.seen.people.push_back(past2);
	// 通話に参加した時刻を覚えていないうちは、現在の参加者は同じ値 (なし) で、名前の自然順になる。
	const dockview::DockViewV2 v = decide_dock(obs);
	REQUIRE(v.participants.size() == 4);
	CHECK(v.participants[0].name.literal == "hanako");
	CHECK(v.participants[0].in_call);
	CHECK_FALSE(v.participants[0].last_seen_at_us.has_value());
	CHECK(v.participants[1].name.literal == "taro");
	// 過去の参加者は最後に通話にいた日時の新しい順 (日時のない人は最後)。
	CHECK(v.participants[2].name.literal == "zeta");
	CHECK_FALSE(v.participants[2].in_call);
	REQUIRE(v.participants[2].last_seen_at_us.has_value());
	CHECK(v.participants[3].name.literal == "alpha");
	CHECK_FALSE(v.participants[3].last_seen_at_us.has_value());
	REQUIRE_FALSE(v.participants[0].voices.empty());
	CHECK(v.participants[0].voices[0].label.text->id == keys::kParticipantsOwnVoiceStatus);
}

TEST_CASE("参加者の設定ウィンドウの現在の参加者は、通話に参加した時刻を最終参加日時として持ち、新しく入った人が先に並ぶ")
{
	Observation obs = healthy();
	obs.seen.call_joined_at_us["100"] = kNow - 600 * kSec; // taro
	obs.seen.call_joined_at_us["200"] = kNow - 60 * kSec;  // hanako (後から入った)
	const dockview::DockViewV2 v = decide_dock(obs);
	REQUIRE(v.participants.size() == 2);
	CHECK(v.participants[0].name.literal == "hanako");
	REQUIRE(v.participants[0].last_seen_at_us.has_value());
	CHECK(*v.participants[0].last_seen_at_us == kNow - 60 * kSec);
	CHECK(v.participants[1].name.literal == "taro");
	REQUIRE(v.participants[1].last_seen_at_us.has_value());
	CHECK(*v.participants[1].last_seen_at_us == kNow - 600 * kSec);

	// 同じ時刻 (cordmux が接続した時点で、既に通話にいた人たち) は名前の自然順。
	obs.seen.call_joined_at_us["100"] = kNow;
	obs.seen.call_joined_at_us["200"] = kNow;
	const dockview::DockViewV2 same = decide_dock(obs);
	REQUIRE(same.participants.size() == 2);
	CHECK(same.participants[0].name.literal == "hanako");
	CHECK(same.participants[1].name.literal == "taro");

	// 通話から抜けた人 (call_joined_at_us にもういない) は、過去の参加者として最後に通話にいた日時の順に並ぶ。
	SeenPerson left;
	left.user_id = "300";
	left.display_name = "left-early";
	left.last_seen_at_us = kNow - 7200 * kSec;
	obs.seen.people.push_back(left);
	SeenPerson left_later;
	left_later.user_id = "400";
	left_later.display_name = "left-later";
	left_later.last_seen_at_us = kNow - 3600 * kSec;
	obs.seen.people.push_back(left_later);
	const dockview::DockViewV2 past = decide_dock(obs);
	REQUIRE(past.participants.size() == 4);
	CHECK(past.participants[2].name.literal == "left-later");
	CHECK(past.participants[3].name.literal == "left-early");
}

TEST_CASE("参加者の設定ウィンドウの列「通話のみ時ラベル非表示」は、既定で並びに出す値になり、付けた人は出さない値と、外す向きのボタンを持つ")
{
	Observation obs = healthy();
	SeenPerson hidden_past;
	hidden_past.user_id = "700";
	hidden_past.display_name = "bot";
	hidden_past.roster_hidden = true;
	obs.seen.people.push_back(hidden_past);
	const dockview::DockViewV2 v = decide_dock(obs);
	REQUIRE(v.participants.size() == 3);

	// seen_people.json に行がない、現在の参加者は出す (チェックが付いている)。押すと外す向き。
	const dockview::ParticipantRow &current = v.participants[0];
	CHECK(current.roster_shown);
	CHECK(current.roster_toggle.id == ActionId::SetRosterShown);
	CHECK(current.roster_toggle.args.user_id == current.user_id);
	CHECK_FALSE(current.roster_toggle.args.on);
	CHECK(current.roster_toggle.label.id == keys::kParticipantsColRosterHideLabel);
	CHECK_FALSE(current.roster_toggle.disabled_reason.has_value());

	// 付けた人は、過去の参加者でも出さない値を持ち、押すと外す向き。
	const dockview::ParticipantRow &past = v.participants[2];
	CHECK(past.user_id == "700");
	CHECK_FALSE(past.roster_shown);
	CHECK(past.roster_toggle.args.on);
	CHECK_FALSE(past.roster_toggle.disabled_reason.has_value());
}

TEST_CASE("参加者の設定ウィンドウの列「通話のみ時ラベル非表示」は、メインアカウントの行でもほかの人と同じに押せる")
{
	// メインアカウントも、自分の枠に名前ラベル付きで映っていない間は、ほかの参加者と同じく並びに出る
	// (dock/build_observation.h の composite_roster_people)。外したい人は、この列で外せる。
	Observation obs = healthy();
	const dockview::DockViewV2 before = decide_dock(obs);
	REQUIRE_FALSE(before.participants.empty());
	const std::string main_account = before.participants[0].user_id;
	obs.choices.broadcaster_user_id = main_account;
	const dockview::DockViewV2 v = decide_dock(obs);
	REQUIRE_FALSE(v.participants.empty());
	bool main_seen = false;
	for (const dockview::ParticipantRow &row : v.participants) {
		CHECK_FALSE(row.roster_toggle.disabled_reason.has_value());
		if (row.user_id == main_account) {
			main_seen = true;
			CHECK(row.roster_toggle.id == ActionId::SetRosterShown);
			CHECK(row.roster_toggle.args.user_id == main_account);
		}
	}
	CHECK(main_seen);
}

TEST_CASE("参加者の設定ウィンドウの列「通話のみ時ラベル非表示」は、設定の [表示] がオフの間は全員の行で押せず、チェックの値は変わらない")
{
	Observation obs = healthy();
	const dockview::DockViewV2 before = decide_dock(obs);
	REQUIRE_FALSE(before.participants.empty());
	obs.choices.roster_show = false;
	const dockview::DockViewV2 v = decide_dock(obs);
	REQUIRE(v.participants.size() == before.participants.size());
	for (std::size_t i = 0; i < v.participants.size(); ++i) {
		REQUIRE(v.participants[i].roster_toggle.disabled_reason.has_value());
		CHECK(v.participants[i].roster_toggle.disabled_reason->id == keys::kParticipantsShowOffTip);
		CHECK(v.participants[i].roster_shown == before.participants[i].roster_shown);
	}
	obs.choices.roster_show = true;
	for (const dockview::ParticipantRow &row : decide_dock(obs).participants)
		CHECK_FALSE(row.roster_toggle.disabled_reason.has_value());
}

TEST_CASE("参加者の設定ウィンドウの列「通話のみ時ラベル非表示」は、[Botを表示] がオフの間は Bot の行だけ押せず、Bot と分からない人は押せる")
{
	Observation obs = healthy();
	const dockview::DockViewV2 before = decide_dock(obs);
	REQUIRE_FALSE(before.participants.empty());
	const std::string bot = before.participants[0].user_id;
	for (PersonFact &person : obs.people) {
		if (person.user_id == bot)
			person.is_bot = true;
	}
	// 設定がオン (Bot も並びに出す) なら、Bot の行も押せる。
	obs.choices.roster_show_bots = true;
	for (const dockview::ParticipantRow &row : decide_dock(obs).participants)
		CHECK_FALSE(row.roster_toggle.disabled_reason.has_value());
	obs.choices.roster_show_bots = false;
	for (const dockview::ParticipantRow &row : decide_dock(obs).participants) {
		if (row.user_id == bot) {
			REQUIRE(row.roster_toggle.disabled_reason.has_value());
			CHECK(row.roster_toggle.disabled_reason->id == keys::kParticipantsShowBotsOffTip);
		} else {
			CHECK_FALSE(row.roster_toggle.disabled_reason.has_value());
		}
	}
	// [表示] がオフのときは、Bot の行も全員と同じ理由にする。
	obs.choices.roster_show = false;
	for (const dockview::ParticipantRow &row : decide_dock(obs).participants) {
		REQUIRE(row.roster_toggle.disabled_reason.has_value());
		CHECK(row.roster_toggle.disabled_reason->id == keys::kParticipantsShowOffTip);
	}
}

TEST_CASE("[バージョン情報] のバージョンは読めなければ「-」になる")
{
	Observation obs = healthy();
	obs.versions.gpu.clear();
	dockview::DockViewV2 v = decide_dock(obs);
	REQUIRE(v.about.versions.size() == 7);
	CHECK(v.about.versions[0].label.id == keys::kSettingsVersionObsPluginLabel);
	CHECK(v.about.versions[0].value.literal == "1.0.0+abc");
	CHECK(v.about.versions[6].value.literal == "-");

	// 「同梱のVencord」は印の vencord_tag (本家のバージョンの文字列) で、build_id (cordmux のバージョン) は出さない。
	// 印が読めないときは「-」。バージョンの食い違いの判定は build_id のまま (rules.cpp)。
	CHECK(v.about.versions[3].label.id == keys::kSettingsVersionBundledVencordLabel);
	CHECK(v.about.versions[3].value.literal == "-");
	obs.versions.bundled_vencord_tag = "v1.15.9";
	v = decide_dock(obs);
	CHECK(v.about.versions[3].value.literal == "v1.15.9");
	obs.versions.bundled_vencord_tag.clear();

	obs.ops.export_result = ExportResultFact{ExportResultKind::Written, "C:/out/cordmux-diagnostics.zip"};
	v = decide_dock(obs);
	REQUIRE(v.about.export_result.has_value());
	CHECK(v.about.export_result->text.id == keys::kSettingsExportDoneStatus);
	REQUIRE(v.about.export_result->location.has_value());
	CHECK(v.about.export_result->location->path == "C:/out/cordmux-diagnostics.zip");
	// 場所は zip のファイルなので、ファイルの行にする ([ファイルの場所を開く])。open_folder は false のまま。
	CHECK(v.about.export_result->location->kind == dockview::LocationKind::File);
	CHECK_FALSE(v.about.export_result->location->open_folder);
	CHECK(v.about.uninstall_button.label.id == keys::kSettingsUninstallButton);
}

TEST_CASE("アンインストールの結果のダイアログは、完了・手順書と違う置き方・確認できなかった・途中で止まったの形を書き分ける")
{
	Observation done = healthy();
	done.uninstall.phase = UninstallPhase::Done;
	done.uninstall.folder_to_delete = "C:/ProgramData/obs-studio/plugins/cordmux";
	done.uninstall.record_dir = "C:/Videos/cordmux";
	dockview::DockViewV2 v = decide_dock(done);
	REQUIRE(v.uninstall_result.has_value());
	CHECK(v.uninstall_result->kind == dockview::UninstallResultKind::Done);
	CHECK(v.uninstall_result->main.id == keys::kDialogUninstallDoneHeadline);
	REQUIRE(v.uninstall_result->folder_to_delete.has_value());
	REQUIRE(v.uninstall_result->record_dir.has_value());
	CHECK(v.uninstall_result->record_dir->path == "C:/Videos/cordmux");
	CHECK(v.uninstall_result->groups.empty());
	// ドックの 1 行目は、メッセージのレベル 2 のカードの見出しにする (ダイアログの主文の指示ではない)。確認できなかった
	// シーンコレクションがあるときも同じで、下のテストで確認する。
	CHECK(v.first_line.kind == dockview::FirstLineKind::Notice);
	CHECK(form_ids(v.first_line) == std::vector<std::string>{keys::kDockUninstallDoneHeadline});

	Observation loose = done;
	loose.uninstall.folder_to_delete.clear();
	v = decide_dock(loose);
	CHECK(v.uninstall_result->loose);
	CHECK(v.uninstall_result->main.id == keys::kDialogUninstallDoneLooseHeadline);
	CHECK_FALSE(v.uninstall_result->folder_to_delete.has_value());
	// cordmux がしたことは、手順書と違う置き方かどうかによらず伝える。
	REQUIRE(v.uninstall_result->notes.size() == 1);
	CHECK(v.uninstall_result->notes[0].id == keys::kDialogUninstallDoneText);

	Observation unconfirmed = done;
	unconfirmed.uninstall.leftovers = {{UninstallLeftoverKind::CollectionNotOpened, "stream"}};
	v = decide_dock(unconfirmed);
	CHECK(v.uninstall_result->kind == dockview::UninstallResultKind::DoneUnconfirmed);
	REQUIRE(v.uninstall_result->groups.size() == 1);
	CHECK(v.uninstall_result->groups[0].items[0].literal == "stream");

	Observation stopped = healthy();
	stopped.uninstall.phase = UninstallPhase::Stopped;
	stopped.uninstall.leftovers = {{UninstallLeftoverKind::SettingsFiles, "C:/cfg"},
				       {UninstallLeftoverKind::LocalDataFolder, "C:/local"},
				       {UninstallLeftoverKind::Sources, "stream"},
				       {UninstallLeftoverKind::VencordDir, "C:/state.json"},
				       {UninstallLeftoverKind::CollectionNameDuplicate, "dup"}};
	v = decide_dock(stopped);
	REQUIRE(v.uninstall_result.has_value());
	CHECK(v.uninstall_result->kind == dockview::UninstallResultKind::Stopped);
	REQUIRE(v.uninstall_result->groups.size() == 3);
	const dockview::LeftoverGroup &not_deleted = v.uninstall_result->groups[0];
	CHECK(not_deleted.heading.id == keys::kDialogNotDeletedText);
	REQUIRE(not_deleted.items.size() == 2);
	CHECK(not_deleted.items[0].text->id == keys::kDialogLeftSettingsFilesItemText);
	REQUIRE(not_deleted.item_locations.size() == 2);
	// 場所の行は、接続のための情報の保存先だけになる。設定のフォルダー (バックアップが残る) は、
	// 「元に戻せなかった設定:」の注意の場所の行に移る。
	REQUIRE(not_deleted.item_locations[0].size() == 1);
	CHECK(not_deleted.item_locations[0][0].path == "C:/local");
	CHECK(not_deleted.item_locations[1].empty());
	const dockview::LeftoverGroup &not_restored = v.uninstall_result->groups[1];
	CHECK(not_restored.heading.id == keys::kDialogNotRestoredText);
	// プラグインのフォルダーが分からない状態なので、注意は「cordmuxのファイルと次のフォルダー」を含む文で、場所の
	// 行は設定のフォルダー 1 つ。
	REQUIRE(not_restored.caution.has_value());
	CHECK(not_restored.caution->id == keys::kDialogDontDeleteFilesAndFoldersText.one);
	REQUIRE(not_restored.caution_locations.size() == 1);
	CHECK(not_restored.caution_locations[0].path == "C:/cfg");
	REQUIRE(not_restored.item_locations.size() == 1);
	REQUIRE(not_restored.item_locations[0].size() == 1);
	CHECK_FALSE(not_restored.item_locations[0][0].open_folder);
	// Vesktop の設定のファイルはファイルの行 (表示する側が [ファイルの場所を開く] を置く)。
	// ほかの場所の行はフォルダー。
	CHECK(not_restored.item_locations[0][0].kind == dockview::LocationKind::File);
	CHECK(not_deleted.item_locations[0][0].kind == dockview::LocationKind::Folder);
	const dockview::LeftoverGroup &not_checked = v.uninstall_result->groups[2];
	CHECK(not_checked.heading.id == keys::kDialogNotCheckedText);
	CHECK(not_checked.item_locations.empty());
	CHECK(not_checked.items[0].text->args == std::vector<std::string>{"dup"});
}

// ---- テキストの規則 (すべての状態で) ----

TEST_CASE("どの状態でも、テキストのキーは現在の形のキーで、引数は「-」でも空でもない")
{
	for (const Observation &obs : many_states()) {
		std::vector<TextRef> refs;
		collect_refs(decide_dock(obs), &refs);
		REQUIRE_FALSE(refs.empty());
		for (const TextRef &t : refs) {
			CAPTURE(t.id);
			CHECK(t.id.rfind("cordmux.", 0) == 0);
			CHECK_FALSE(old_form_key(t.id));
			for (const std::string &arg : t.args) {
				CHECK(arg != "-");
				CHECK_FALSE(arg.empty());
			}
		}
	}
}

TEST_CASE("同じ Observation からは同じ View を作る (時計を読まない)")
{
	for (const Observation &obs : many_states())
		CHECK(decide_dock(obs) == decide_dock(obs));
}

TEST_CASE("many_states() の状態から、メッセージのレベル 1・2・3 のメッセージがどれも作られる")
{
	std::set<int> seen;
	for (const Observation &obs : many_states())
		for (const dockview::Notice &n : decide_dock(obs).notices)
			seen.insert(static_cast<int>(n.tier));
	CHECK(seen.count(static_cast<int>(dockview::Tier::L1)) == 1);
	CHECK(seen.count(static_cast<int>(dockview::Tier::L2)) == 1);
	CHECK(seen.count(static_cast<int>(dockview::Tier::L3)) == 1);
}

TEST_CASE("この行の人の声を別の人が使っているときは、どの声も選べず、理由は 1 対 1 のテキストに名前を入れる")
{
	Observation obs = healthy();
	// ghost (通話に参加していない過去の参加者) が taro の声を使っている。
	SeenPerson ghost;
	ghost.user_id = "800";
	ghost.display_name = "ghost";
	ghost.voice_source_user_id = "100";
	obs.seen.people.push_back(ghost);
	const dockview::DockViewV2 v = decide_dock(obs);
	const dockview::PersonRow *taro = row_of(v, "100");
	REQUIRE(taro != nullptr);
	REQUIRE(taro->menu.voices.size() == 2);
	const dockview::ViewButton &hanako = taro->menu.voices[1].action;
	REQUIRE(hanako.disabled_reason.has_value());
	CHECK(hanako.disabled_reason->id == keys::kParticipantsVoiceTakenText);
	CHECK(hanako.disabled_reason->args == std::vector<std::string>{"taro", "ghost"});
	// ドックのメニューの項目のテキストは名前だけ (長い理由はツールチップ)。短い理由のキー row_voice_taken.menu は
	// 「候補の人の声が別の行で使われている」の形なので、ここで使うと、使っている人の名前が空の
	// 「hanako (既に「」の行で使っています)」になる。
	const dockview::VoiceChoice &item = taro->menu.voices[1];
	CHECK_FALSE(item.label.text.has_value());
	CHECK(item.label.literal == "hanako");
	// どの項目のテキストにも、空の引数がない。
	for (const dockview::VoiceChoice &choice : taro->menu.voices) {
		if (!choice.label.text.has_value())
			continue;
		for (const std::string &arg : choice.label.text->args)
			CHECK_FALSE(arg.empty());
	}
	// 現在の値が本人の声なので、「使う声」の項目そのものを押せなくし、括弧の理由を 1 回だけ付ける。
	REQUIRE(taro->menu.voices_disabled_label.has_value());
	CHECK(taro->menu.voices_disabled_label->id == keys::kDockRowVoiceFromLentMenu);
	CHECK(taro->menu.voices_disabled_label->args == std::vector<std::string>{"taro", "ghost"});
	REQUIRE(taro->menu.voices_disabled_reason.has_value());
	CHECK(taro->menu.voices_disabled_reason->id == keys::kParticipantsVoiceTakenText);
	CHECK(taro->menu.voices_disabled_reason->args == std::vector<std::string>{"taro", "ghost"});
	// ほかの行は押せる (hanako の声は誰も使っていない)。
	const dockview::PersonRow *hanako_row = row_of(v, "200");
	REQUIRE(hanako_row != nullptr);
	CHECK_FALSE(hanako_row->menu.voices_disabled_label.has_value());
	CHECK_FALSE(hanako_row->menu.voices_disabled_reason.has_value());

	// 現在の値が本人の声でない (手で書き換えて、taro も hanako の声を使っている。設定がぶつかっている):
	// 「本人の声」へ戻せるように、項目そのものは押せなくしない。
	Observation conflict = obs;
	SeenPerson taro_seen;
	taro_seen.user_id = "100";
	taro_seen.display_name = "taro";
	taro_seen.voice_source_user_id = "200";
	conflict.seen.people.push_back(taro_seen);
	const dockview::DockViewV2 cv = decide_dock(conflict);
	const dockview::PersonRow *taro_conflict = row_of(cv, "100");
	REQUIRE(taro_conflict != nullptr);
	CHECK_FALSE(taro_conflict->menu.voices_disabled_label.has_value());
	CHECK_FALSE(taro_conflict->menu.voices_disabled_reason.has_value());
	REQUIRE_FALSE(taro_conflict->menu.voices.empty());
	CHECK_FALSE(taro_conflict->menu.voices[0].action.disabled_reason.has_value()); // 「本人の声」へ戻せる
}

TEST_CASE("再インストールの失敗を追加した上書きのカードは、元のカードのメッセージのレベルのままにする")
{
	// 接続している + 上書き + 配信中で枠に人がいる + 再インストールの失敗: 現在の接続は続くので、
	// メッセージのレベル 2 の 1 枚。
	Observation connected = healthy();
	connected.install.dir = VencordDirState::HashMismatch;
	connected.obs_outputs.streaming = true;
	connected.install.reinstall_failed_files = {"renderer.js"};
	dockview::DockViewV2 v = decide_dock(connected);
	CHECK(notices_of(v, "ReinstallFailed").empty());
	std::vector<dockview::Notice> n = notices_of(v, "InstallMismatch");
	REQUIRE(n.size() == 1);
	CHECK(n[0].tier == dockview::Tier::L2);
	CHECK(n[0].body.back().id == keys::kDockReinstallFailedText);

	// 接続していない: 上書きのカードがメッセージのレベル 1 で、そこに追加する。
	Observation down = connected;
	down.link.state = LinkState::NeverConnected;
	down.link.connected_at_us.reset();
	v = decide_dock(down);
	n = notices_of(v, "VencordOverwritten");
	REQUIRE(n.size() == 1);
	CHECK(n[0].tier == dockview::Tier::L1);
	CHECK(n[0].body.back().id == keys::kDockReinstallFailedText);
}

// ---- カードの構成 ----

namespace {

int primary_count(const std::vector<dockview::Notice> &notices)
{
	int n = 0;
	for (const dockview::Notice &notice : notices)
		for (const dockview::ViewButton &b : notice.buttons)
			n += b.primary ? 1 : 0;
	return n;
}

int stage_card_count(const std::vector<dockview::Notice> &notices)
{
	int n = 0;
	for (const dockview::Notice &notice : notices)
		n += notice.stages.empty() ? 0 : 1;
	return n;
}

// インストールした日: Vesktop に cordmux が追加されておらず、OBS にも cordmux のソースがない。
// インストールした日。手順書のとおり、[Vesktopに追加] を押す前に Vesktop を終了している ([Vesktopに追加] は Vesktop の
// 起動中は押せない。ledger/actions.cpp の vesktop_busy_reason)。
Observation first_day()
{
	Observation o = not_connected();
	o.install.dir = VencordDirState::NotConfigured;
	o.install.process = VesktopProcess::NotRunning;
	o.choices.composite_present = false;
	return o;
}

} // namespace

TEST_CASE("Vesktop の設定のファイルが読めない間は、Vesktop が起動していても [Vesktopに追加] のカードもボタンも出さない")
{
	// state.json がまだない (Vesktop を一度も起動していない) と、vencordDir は Unknown になる。「追加されていません」
	// とは言えないので、動いているのに繋がらない行に落ち、押しても直らないボタンを置かない。
	Observation obs = not_connected();
	obs.install.dir = VencordDirState::Unknown;
	obs.install.process = VesktopProcess::Running;
	CHECK(primary_rule_of(obs) == PrimaryRuleId::VesktopRunningNotLinked);
	const dockview::DockViewV2 v = decide_dock(obs);

	const std::vector<dockview::Notice> not_linked = notices_of(v, "VesktopRunningNotLinked");
	REQUIRE(not_linked.size() == 1);
	CHECK(not_linked[0].headline.id == keys::kDockVesktopNotConnectedHeadline);
	REQUIRE(not_linked[0].body.size() == 1);
	CHECK(not_linked[0].body[0].id == keys::kDockVesktopNotConnectedText);

	CHECK(notices_of(v, "VencordDirNotConfigured").empty());
	CHECK(notices_of(v, "VencordDirElsewhere").empty());
	CHECK(notices_of(v, "VencordOverwritten").empty());
	CHECK(notices_of(v, "InstallMismatch").empty());
	for (const dockview::Notice &card : v.notices) {
		for (const dockview::ViewButton &b : card.buttons) {
			CHECK(b.label.id != keys::kDockAddToVesktopButton);
			CHECK(b.id != ActionId::WriteVencordDir);
		}
	}
}

TEST_CASE("プライマリのボタンはドックに 1 つで、メッセージのレベルの順で最初の、押せるボタンにする")
{
	const Observation day = first_day();
	const dockview::DockViewV2 v = decide_dock(day);
	REQUIRE(v.notices.size() >= 2);
	CHECK(primary_count(v.notices) == 1);
	// プライマリは [Vesktopに追加] (メッセージのレベル 2 の先のメッセージ。cordmux のソースのメッセージはボタンを持たない)。
	std::vector<dockview::Notice> install = notices_of(v, "VencordDirNotConfigured");
	REQUIRE(install.size() == 1);
	REQUIRE_FALSE(install[0].buttons.empty());
	CHECK(install[0].buttons[0].primary);

	// 待ちを通した後も 1 つ (並びは表示した順で決め直す)。
	const StabilizeResult r = stabilize(NoticeTrackerState{}, v, kNow, NoticeTrackerConfig{});
	CHECK(primary_count(r.view.notices) == 1);

	// メッセージのレベル 1 があれば、そのボタンがプライマリ (メッセージのレベル 2 のボタンは普通のボタン)。
	Observation many = healthy();
	many.install.dir = VencordDirState::PointsElsewhere; // メッセージのレベル 2 の [Vesktopに追加] (接続している間の食い違い)
	many.recording.unwritable_dir = "D:/rec"; // メッセージのレベル 1 の [保存先を変更] (始めようとして書き込めなかった)
	many.recording.phase = RecordPhase::StartFailed;
	const dockview::DockViewV2 mv = decide_dock(many);
	CHECK(primary_count(mv.notices) == 1);
	std::vector<dockview::Notice> unwritable = notices_of(mv, "RecordDirUnwritable");
	REQUIRE(unwritable.size() == 1);
	REQUIRE(unwritable[0].tier == dockview::Tier::L1);
	REQUIRE_FALSE(unwritable[0].buttons.empty());
	CHECK(unwritable[0].buttons[0].primary);

	// 押せないボタンと、メッセージのレベル 3 のボタンはプライマリにしない。
	std::vector<dockview::Notice> manual;
	dockview::Notice l3;
	l3.tier = dockview::Tier::L3;
	l3.buttons.resize(1);
	dockview::Notice l2;
	l2.tier = dockview::Tier::L2;
	l2.buttons.resize(1);
	l2.buttons[0].disabled_reason = TextRef{};
	l2.buttons[0].primary = true;
	manual.push_back(l3);
	manual.push_back(l2);
	mark_primary_button(&manual);
	CHECK(primary_count(manual) == 0);
}

TEST_CASE("この起動で Vesktop の設定を書かないインストールの日は、押せるボタンがないので"
	  "プライマリのボタンもない")
{
	Observation day = first_day();
	day.install.vesktop_writes_off = true;
	const dockview::DockViewV2 v = decide_dock(day);
	CHECK(primary_count(v.notices) == 0);
	// 状態のカードはボタンを持たない (プライマリもない)。段階の行はこのカードに残る (段階の行を持つのは、
	// 止まった段階のカード 1 枚だけ)。
	const std::vector<dockview::Notice> install = notices_of(v, "VencordDirNotConfigured");
	REQUIRE(install.size() == 1);
	CHECK(install[0].buttons.empty());
	CHECK(stage_card_count(v.notices) == 1);
	CHECK_FALSE(install[0].stages.empty());
	// cordmux のソースのメッセージはボタンを持たない。
	const std::vector<dockview::Notice> composite = notices_of(v, "CompositeAbsent");
	REQUIRE(composite.size() == 1);
	CHECK(composite[0].buttons.empty());
	// 待ちを通した後も同じ。
	const StabilizeResult r = stabilize(NoticeTrackerState{}, v, kNow, NoticeTrackerConfig{});
	CHECK(primary_count(r.view.notices) == 0);
}

TEST_CASE("段階の行を持つのは、止まった段階のカード 1 枚だけにする")
{
	// インストールした日: Vesktop の段階のカードが持ち、cordmux のソースのカードは持たない。
	dockview::DockViewV2 v = decide_dock(first_day());
	CHECK(stage_card_count(v.notices) == 1);
	std::vector<dockview::Notice> scenes = notices_of(v, "CompositeAbsent");
	REQUIRE(scenes.size() == 1);
	CHECK(scenes[0].stages.empty());

	// 接続していて cordmux のソースがなく、インストールの食い違いもある: 食い違いのカードは止まった段階では
	// ないので持たず、cordmux のソースのカードが持つ。
	Observation connected = healthy();
	connected.install.dir = VencordDirState::HashMismatch;
	connected.choices.composite_present = false;
	v = decide_dock(connected);
	std::vector<dockview::Notice> mismatch = notices_of(v, "InstallMismatch");
	REQUIRE(mismatch.size() == 1);
	CHECK(mismatch[0].stages.empty());
	scenes = notices_of(v, "CompositeAbsent");
	REQUIRE(scenes.size() == 1);
	CHECK_FALSE(scenes[0].stages.empty());
	CHECK(stage_card_count(v.notices) == 1);
}

namespace {

std::vector<std::string> stage_ids(const dockview::Notice &n)
{
	std::vector<std::string> out;
	for (const dockview::StageItem &s : n.stages)
		out.push_back(s.text.id);
	return out;
}

// ドックのどこかのカードの段階の行に、そのキーがあるか。
bool face_has_stage(const std::vector<dockview::Notice> &notices, const std::string &id)
{
	for (const dockview::Notice &n : notices)
		for (const dockview::StageItem &s : n.stages)
			if (s.text.id == id)
				return true;
	return false;
}

// ドックのカードのうち、そのキーのラベルのボタンを持つカードの数。
int cards_with_button(const std::vector<dockview::Notice> &notices, const std::string &label)
{
	int count = 0;
	for (const dockview::Notice &n : notices) {
		bool has = false;
		for (const dockview::ViewButton &b : n.buttons)
			has = has || b.label.id == label;
		count += has ? 1 : 0;
	}
	return count;
}

} // namespace

TEST_CASE("段階の行は、止まった段階の値を同じドックの見出しが伝えていれば表示しない")
{
	// インストールした日: 下のカードの見出し「cordmuxのソースがまだ追加されていません」が伝えるので、
	// 「cordmuxのソース: 追加されていません」はない。「Vesktop: 接続されていません」は残す (見出し
	// 「Vesktopにcordmuxが追加されていません」は原因で、接続の状態そのものは伝えていない)。
	dockview::DockViewV2 v = decide_dock(first_day());
	CHECK_FALSE(face_has_stage(v.notices, keys::kDockStageCompositeNotAddedStatus));
	std::vector<dockview::Notice> install = notices_of(v, "VencordDirNotConfigured");
	REQUIRE(install.size() == 1);
	CHECK(stage_ids(install[0]) == std::vector<std::string>{keys::kDockStageVesktopDisconnectedStatus});

	// 接続していて cordmux のソースがない: cordmux のソースのカードは、動いている段階 (Vesktop と通話) だけを持つ。
	Observation absent = healthy();
	absent.choices.composite_present = false;
	v = decide_dock(absent);
	std::vector<dockview::Notice> scenes = notices_of(v, "CompositeAbsent");
	REQUIRE(scenes.size() == 1);
	// 帰結と次の一手は 1 つの本文。ボタンは持たない。
	REQUIRE(scenes[0].body.size() == 1);
	CHECK(scenes[0].body[0].id == keys::kDockCompositeAbsentText);
	CHECK(scenes[0].buttons.empty());
	CHECK(stage_ids(scenes[0]) ==
	      std::vector<std::string>{keys::kDockStageVesktopConnectedStatus, keys::kDockStageCallJoinedStatus.other});

	// Vesktop は起動しているが接続していない: 見出しが伝えるので、「Vesktop: 接続されていません」はない。
	Observation not_linked = not_connected();
	not_linked.install.process = VesktopProcess::Running;
	v = decide_dock(not_linked);
	CHECK_FALSE(face_has_stage(v.notices, keys::kDockStageVesktopDisconnectedStatus));

	// 同じドックに、止まった段階の値を伝える見出しがなければ、その行は残す (再起動を求めるカード)。
	Observation restart = not_connected();
	restart.install.process = VesktopProcess::Running;
	restart.install.dir = VencordDirState::Matches;
	restart.install.vesktop_started_at_us = kNow - 120 * kSec;
	restart.install.discord_side_written_at_us = kNow - 60 * kSec;
	v = decide_dock(restart);
	std::vector<dockview::Notice> restart_cards = notices_of(v, "RestartVesktop");
	REQUIRE(restart_cards.size() == 1);
	REQUIRE_FALSE(restart_cards[0].stages.empty());
	CHECK(restart_cards[0].stages.front().text.id == keys::kDockStageVesktopDisconnectedStatus);
}

TEST_CASE("cordmux のソースの有無を調べていない間は、段階の行もメッセージも出さない")
{
	// シーンの読み込みの前と、シーンコレクションを切り替えている途中は、composite_present が false でも
	// 「ない」と断定しない。起動直後の普通の状態を、cordmux をまだ置いていないと表示しない。
	Observation unchecked = not_connected();
	unchecked.choices.composite_checked = false;
	unchecked.choices.composite_present = false;
	const dockview::DockViewV2 v = decide_dock(unchecked);
	CHECK(notices_of(v, "CompositeAbsent").empty());
	CHECK_FALSE(face_has_stage(v.notices, keys::kDockStageCompositeNotAddedStatus));
	CHECK_FALSE(face_has_stage(v.notices, keys::kDockStageCompositeAddedStatus));

	// 調べ終えれば、ある・ないに合わせた行を出す (接続が切れたカードの段階の行で確認する)。
	Observation dropped = not_connected();
	dropped.link.state = LinkState::Disconnected;
	dropped.link.connected_at_us = kNow - 600 * kSec;
	dropped.link.disconnected_at_us = kNow - 30 * kSec;
	dropped.choices.composite_present = true;
	CHECK(face_has_stage(decide_dock(dropped).notices, keys::kDockStageCompositeAddedStatus));
}

TEST_CASE("押した操作の失敗のカードは、同じボタンを持つ状態のカードの代わりに 1 枚だけ表示する")
{
	// 接続していない + 追加されていない + [Vesktopに追加] の失敗: 失敗のカード 1 枚が、状態のカードの段階の行を
	// 引き継ぐ。
	Observation add = first_day();
	add.choices.composite_present = true;
	add.ops.vesktop_add_failure = VesktopAddFailure::StateWriteFailed;
	dockview::DockViewV2 v = decide_dock(add);
	CHECK(notices_of(v, "VencordDirNotConfigured").empty());
	std::vector<dockview::Notice> failed = notices_of(v, "VesktopAddFailed");
	REQUIRE(failed.size() == 1);
	CHECK(failed[0].key == "VesktopAddFailed");
	CHECK(failed[0].headline.id == keys::kDockVesktopAddFailedHeadline);
	// 帰結の後に理由の 1 文。次の操作 (ボタン) が最後。
	REQUIRE(failed[0].body.size() == 2);
	CHECK(failed[0].body[0].id == keys::kDockCannotReceiveText);
	CHECK(failed[0].body[1].id == keys::kDockVesktopAddFailedStateWriteText);
	CHECK(stage_ids(failed[0]) == std::vector<std::string>{keys::kDockStageVesktopDisconnectedStatus,
								keys::kDockStageCompositeAddedStatus});
	CHECK(cards_with_button(v.notices, keys::kDockAddToVesktopButton) == 1);
	CHECK(primary_count(v.notices) == 1);
	REQUIRE(failed[0].buttons.size() == 1);
	CHECK(failed[0].buttons[0].id == ActionId::WriteVencordDir);

	// 同じ状態で、押し直しても同じ失敗になる理由 (同梱の Vencord が見つからない) のとき: 失敗のカードは
	// ボタンを持たないが、それでも状態のカードの代わりに 1 枚にする (状態のカードをボタン付きで別に表示しない)。
	Observation bundled = add;
	bundled.ops.vesktop_add_failure = VesktopAddFailure::BundledDirUnknown;
	v = decide_dock(bundled);
	CHECK(notices_of(v, "VencordDirNotConfigured").empty());
	failed = notices_of(v, "VesktopAddFailed");
	REQUIRE(failed.size() == 1);
	CHECK(failed[0].buttons.empty());
	// 理由のキーが次の操作を含むので、理由の文が最後。
	REQUIRE(failed[0].body.size() == 2);
	CHECK(failed[0].body[1].id == keys::kDockVesktopAddFailedBundledMissingText);
	CHECK(stage_ids(failed[0]) == std::vector<std::string>{keys::kDockStageVesktopDisconnectedStatus,
								keys::kDockStageCompositeAddedStatus});
	CHECK(cards_with_button(v.notices, keys::kDockAddToVesktopButton) == 0);

	// 接続している間: 帰結は「次にVesktopを起動したときから、...」の文で、メッセージのレベルは InstallMismatch と
	// 同じ 2。
	Observation up = healthy();
	up.install.dir = VencordDirState::PointsElsewhere;
	up.ops.vesktop_add_failure = VesktopAddFailure::StateWriteFailed;
	v = decide_dock(up);
	CHECK(notices_of(v, "InstallMismatch").empty());
	failed = notices_of(v, "VesktopAddFailed");
	REQUIRE(failed.size() == 1);
	CHECK(failed[0].tier == dockview::Tier::L2);
	REQUIRE(failed[0].body.size() == 2);
	CHECK(failed[0].body[0].id == keys::kDockCannotReceiveAfterRestartText);
	CHECK(cards_with_button(v.notices, keys::kDockAddToVesktopButton) == 1);
	// 別の場所の Vencord を読むように変わっている間は、このカードが代わる状態のカードと同じ直す操作
	// (RepairVencordDir) を持つ。
	REQUIRE(failed[0].buttons.size() == 1);
	CHECK(failed[0].buttons[0].id == ActionId::RepairVencordDir);

	// 接続していない + 配信中 + 枠に人がいる: 2 枚のうち、影響の大きいほうのメッセージのレベル (1) にする。
	Observation down = up;
	down.obs_outputs.streaming = true;
	down.link.state = LinkState::NeverConnected;
	down.link.connected_at_us.reset();
	v = decide_dock(down);
	failed = notices_of(v, "VesktopAddFailed");
	REQUIRE(failed.size() == 1);
	CHECK(failed[0].tier == dockview::Tier::L1);
	CHECK(failed[0].log_level == dockview::LogLevel::Error);
	CHECK(notices_of(v, "VencordDirElsewhere").empty());
}

TEST_CASE("設定を保存できていないカードは、設定ウィンドウを開く [cordmuxの設定を開く] を持つ")
{
	Observation obs = healthy();
	obs.ops.settings_unsaved = true;
	const std::vector<dockview::Notice> n = notices_of(decide_dock(obs), "SettingsUnsaved");
	REQUIRE(n.size() == 1);
	REQUIRE(n[0].buttons.size() == 1);
	CHECK(n[0].buttons[0].id == ActionId::OpenSettings);
	CHECK(n[0].buttons[0].label.id == keys::kDockOpenSettingsButton);
	CHECK_FALSE(n[0].buttons[0].disabled_reason.has_value());
	CHECK(n[0].buttons[0].primary);
}

TEST_CASE("[Vesktopに追加] と再インストールの失敗は、接続している間は配信中でもメッセージのレベル 2 にする")
{
	// 接続している + 配信中 + 枠に人がいる + [Vesktopに追加] の失敗 (別の場所を指している): 現在の受け取りは続く。
	Observation up = healthy();
	up.obs_outputs.streaming = true;
	up.install.dir = VencordDirState::PointsElsewhere;
	up.ops.vesktop_add_failure = VesktopAddFailure::StateWriteFailed;
	std::vector<dockview::Notice> n = notices_of(decide_dock(up), "VesktopAddFailed");
	REQUIRE(n.size() == 1);
	CHECK(n[0].tier == dockview::Tier::L2);

	// 接続していない: 枠の映像が今損なわれているので、メッセージのレベル 1。
	Observation down = up;
	down.link.state = LinkState::NeverConnected;
	down.link.connected_at_us.reset();
	n = notices_of(decide_dock(down), "VesktopAddFailed");
	REQUIRE(n.size() == 1);
	CHECK(n[0].tier == dockview::Tier::L1);

	// 再インストールの失敗が 1 枚のカードになる場合 (上書きのカードがない) も同じ。
	Observation reinstall = healthy();
	reinstall.obs_outputs.streaming = true;
	reinstall.install.reinstall_failed_files = {"renderer.js"};
	n = notices_of(decide_dock(reinstall), "ReinstallFailed");
	REQUIRE(n.size() == 1);
	CHECK(n[0].tier == dockview::Tier::L2);
}

TEST_CASE("メッセージが見出しの短い形を持てば、1 行目の forms の 2 つ目に入れる")
{
	dockview::Notice n;
	n.key = "RecordStoppedSpaceExhausted";
	n.tier = dockview::Tier::L1;
	n.headline.id = keys::kDockRecordStoppedSpaceHeadline;
	TextRef short_form;
	short_form.id = "short";
	n.headline_short = short_form;
	const dockview::FirstLine line = first_line_of({n}, dockview::FirstLine{});
	REQUIRE(line.forms.size() == 2);
	CHECK(line.forms[0].id == keys::kDockRecordStoppedSpaceHeadline);
	CHECK(line.forms[1].id == "short");
	CHECK(line.notice_key == "RecordStoppedSpaceExhausted");

	// 短い形がなければ見出し 1 つ (省略は、Qt の末尾の省略に任せる)。
	n.headline_short.reset();
	CHECK(first_line_of({n}, dockview::FirstLine{}).forms.size() == 1);
}

TEST_CASE("空き容量が尽きて録画が止まったメッセージの短い形は、見出しから括弧を丸ごと落としたテキストにする")
{
	Observation obs = healthy();
	obs.recording.stopped_by_space_exhausted = true;
	const dockview::DockViewV2 v = decide_dock(obs);
	const std::vector<dockview::Notice> n = notices_of(v, "RecordStoppedSpaceExhausted");
	REQUIRE(n.size() == 1);
	CHECK(n[0].headline.id == keys::kDockRecordStoppedSpaceHeadline);
	REQUIRE(n[0].headline_short.has_value());
	CHECK(n[0].headline_short->id == keys::kDockRecordStoppedSpaceShortHeadline);
	CHECK(n[0].headline_short->args.empty());
	// 1 行目に表示するのはメッセージのレベル 1 のこのメッセージで、短い形が forms の 2 つ目 (待ちを通した後も同じ。
	// stabilize() は Notice をそのまま渡す)。
	CHECK(v.first_line.notice_key == "RecordStoppedSpaceExhausted");
	CHECK(form_ids(v.first_line) ==
	      std::vector<std::string>{keys::kDockRecordStoppedSpaceHeadline, keys::kDockRecordStoppedSpaceShortHeadline});
}

TEST_CASE("見出しのキーごとに決まった短い形を持ち、表にない見出しは短い形を持たない")
{
	// 見出しのキー -> 短い形のキー (rules_dock.cpp の kShortHeadlines と同じ表を、テストの側でもう一度書く)。
	const std::map<std::string, std::string> expected = {
		{keys::kDockRecordStoppedSpaceHeadline, keys::kDockRecordStoppedSpaceShortHeadline},
		{keys::kDockDisconnectedHeadline, keys::kDockDisconnectedNoTimeHeadline},
		{keys::kDockVesktopNotAddedHeadline, keys::kDockVesktopNotAddedShortHeadline},
		{keys::kDockVesktopAddFailedHeadline, keys::kDockVesktopAddFailedShortHeadline},
		{keys::kDockVencordElsewhereHeadline, keys::kDockVencordElsewhereShortHeadline},
		{keys::kDockVencordOverwrittenHeadline, keys::kDockVencordOverwrittenShortHeadline},
		{keys::kDockVersionDiffersHeadline, keys::kDockVersionDiffersShortHeadline},
		{keys::kDockFilesMixedHeadline, keys::kDockFilesMixedShortHeadline},
		{keys::kDockVesktopNotConnectedHeadline, keys::kDockVesktopNotConnectedShortHeadline},
		{keys::kDockPortsInUseHeadline, keys::kDockPortsInUseShortHeadline},
		{keys::kDockContactRosterHeadline, keys::kDockContactRosterShortHeadline},
		{keys::kDockContactSelfHeadline, keys::kDockContactSelfShortHeadline},
		{keys::kDockCompositeAbsentHeadline, keys::kDockCompositeAbsentShortHeadline},
		{keys::kDockDoubleAudioHeadline, keys::kDockDoubleAudioShortHeadline},
		{keys::kDockMainAccountNotInCallHeadline, keys::kDockMainAccountNotInCallShortHeadline},
		{keys::kDockMainAccountNotInCallNoNameHeadline, keys::kDockMainAccountNotInCallShortHeadline},
		{keys::kDockNoVideoHeadline.other, keys::kDockNoVideoShortHeadline.other},
		{keys::kDockNoVideoHeadline.one, keys::kDockNoVideoShortHeadline.one},
		{keys::kDockFinalizeSpaceShortHeadline, keys::kDockFinalizeSpaceShortShortHeadline},
		{keys::kDockFinalizePendingHeadline, keys::kDockFinalizePendingShortHeadline},
		{keys::kDockFinalizeFailedHeadline, keys::kDockFinalizeFailedShortHeadline},
	};
	// 短い形を持たせない見出し (理由は rules_dock.cpp の kShortHeadlines のコメント)。どれかに短い形を
	// 持たせると、このテストが失敗する。
	const std::set<std::string> without = {
		keys::kDockContactMediaHeadline,      keys::kDockContactSharingHeadline, keys::kDockContactSsrcHeadline,
		keys::kDockContactProfileHeadline,    keys::kDockContactCallHeadline,    keys::kDockContactVoiceSelectHeadline,
		keys::kDockRecordStartFailedHeadline,
		keys::kDockSettingsUnreadHeadline,    keys::kDockSettingsUnsavedHeadline, keys::kDockParticipantsUnreadHeadline,
		keys::kDockParticipantsUnsavedHeadline, keys::kDockOtherObsHeadline,
		keys::kDockDisconnectedNoTimeHeadline, keys::kDockUninstallDoneHeadline, keys::kDialogUninstallStoppedHeadline,
		keys::kDockRestartVesktopHeadline,    keys::kDockSessionNotSavedHeadline,
	};

	// many_states() に、表の見出しのメッセージが作られる状態を加えて、作られたメッセージのレベル 1 と 2 のメッセージを
	// すべて見る。
	std::vector<Observation> states = many_states();
	{
		Observation o = healthy();
		o.recording.stopped_by_space_exhausted = true;
		o.recording.finalize = FinalizeState::Failed;
		o.recording.session_dir = "D:/rec/cordmux/2000-01-01_2000";
		o.recording.pending_present = true;
		o.recording.finalize_need_bytes = 3ULL * 1024 * 1024 * 1024 + 1;
		o.recording.free_space_bytes = 1ULL * 1024 * 1024 * 1024;
		states.push_back(o);
		Observation pending = healthy();
		pending.recording.finalize = FinalizeState::NotStarted;
		pending.recording.pending_present = true;
		states.push_back(pending);
		Observation not_linked = not_connected();
		not_linked.install.process = VesktopProcess::Running;
		states.push_back(not_linked);
		Observation start_failed = healthy();
		start_failed.recording.phase = RecordPhase::StartFailed;
		start_failed.recording.unwritable_dir = "D:/rec";
		states.push_back(start_failed);
		Observation roster = healthy();
		roster.contacts = {{"roster", ContactState::Missing}};
		states.push_back(roster);
		Observation self = healthy();
		self.contacts = {{"self", ContactState::Missing}};
		states.push_back(self);
		Observation one_video = healthy();
		one_video.people[0].last_video_frame_at_us = kNow - 5 * kSec;
		states.push_back(one_video);
		// 2 人 (枠に載っている人の映像が止まり、録画の対象の人の最初のフレームが届かない)。上の、映像が
		// 届かない人のメッセージのテストと同じ状態である。
		Observation two_video = one_video;
		two_video.choices.receive_scope = cordmux::ledger::ReceiveScope::Everyone;
		two_video.choices.recording = FeatureSwitch::On;
		two_video.recording.phase = RecordPhase::Recording;
		two_video.people[1].sharing = true;
		two_video.people[1].sharing_since_us = kNow - 20 * kSec;
		states.push_back(two_video);
	}
	std::set<std::string> seen_with_short;
	for (const Observation &obs : states) {
		for (const dockview::Notice &n : decide_dock(obs).notices) {
			if (n.tier == dockview::Tier::L3)
				continue;
			CAPTURE(n.rule_name);
			CAPTURE(n.headline.id);
			const auto it = expected.find(n.headline.id);
			if (it == expected.end()) {
				CHECK_FALSE(n.headline_short.has_value());
				continue;
			}
			REQUIRE(n.headline_short.has_value());
			CHECK(n.headline_short->id == it->second);
			seen_with_short.insert(n.headline.id);
			// 引数は、短い形が同じ %1 を持つときだけ渡す (人数・足りない容量)。時刻・ソースの名前・
			// アカウントの名前は、短い形が持たないので渡さない。
			const bool keeps = n.headline.id == keys::kDockNoVideoHeadline.other ||
					   n.headline.id == keys::kDockNoVideoHeadline.one ||
					   n.headline.id == keys::kDockFinalizeSpaceShortHeadline;
			if (keeps)
				CHECK(n.headline_short->args == n.headline.args);
			else
				CHECK(n.headline_short->args.empty());
		}
	}
	for (const std::string &id : without)
		CHECK(expected.count(id) == 0);
	// テストの状態から、表の主な見出しが実際に作られたこと (状態の作り方が変わって、確認せずに通らないように)。
	for (const char *id : {keys::kDockRecordStoppedSpaceHeadline, keys::kDockDisconnectedHeadline,
			       keys::kDockVesktopNotConnectedHeadline, keys::kDockPortsInUseHeadline,
			       keys::kDockNoVideoHeadline.other,
			       keys::kDockFinalizeSpaceShortHeadline, keys::kDockFinalizePendingHeadline,
			       keys::kDockFinalizeFailedHeadline, keys::kDockContactRosterHeadline,
			       keys::kDockContactSelfHeadline}) {
		const std::string key = id;
		CAPTURE(key);
		CHECK(seen_with_short.count(key) == 1);
	}
	// 作成待ちと録画ファイルの作成の失敗は別の短い形 (1 行目で見分けられる)。
	CHECK(std::string(keys::kDockFinalizePendingShortHeadline) != keys::kDockFinalizeFailedShortHeadline);
}

TEST_CASE("1 行目の短い形は FirstLine::forms の 2 つ目に入り、メッセージのレベル 1 の残りの件数は残る")
{
	// メッセージのレベル 1 が 2 件 (切断と、空き容量で止まった録画)。1 行目は、先のメッセージの見出しと短い形と、
	// 残りの件数「(他1件)」。
	Observation obs = not_connected();
	obs.link.state = LinkState::Disconnected;
	obs.link.connected_at_us = kNow - 600 * kSec;
	obs.link.disconnected_at_us = kNow - 30 * kSec;
	obs.recording.stopped_by_space_exhausted = true;
	const dockview::DockViewV2 v = decide_dock(obs);
	REQUIRE(v.first_line.kind == dockview::FirstLineKind::Notice);
	REQUIRE(v.first_line.forms.size() == 2);
	const std::string head = v.first_line.forms[0].id;
	if (head == keys::kDockDisconnectedHeadline) {
		CHECK(v.first_line.forms[1].id == keys::kDockDisconnectedNoTimeHeadline);
		CHECK(v.first_line.forms[1].args.empty());
	} else {
		CHECK(head == keys::kDockRecordStoppedSpaceHeadline);
		CHECK(v.first_line.forms[1].id == keys::kDockRecordStoppedSpaceShortHeadline);
	}
	REQUIRE(v.first_line.more_count.has_value());
	CHECK(v.first_line.more_count->args == std::vector<std::string>{"1"});
}

TEST_CASE("アンインストールを始めた後は人の行を作らず、空の一覧は Uninstalled の 1 文にする")
{
	for (const UninstallPhase phase : {UninstallPhase::Running, UninstallPhase::Done, UninstallPhase::Stopped}) {
		CAPTURE(static_cast<int>(phase));
		// 待ち受けを閉じる前のコピーが残り、接続していて通話に人がいる Observation でも、人の行を作らない。
		Observation obs = healthy();
		REQUIRE_FALSE(obs.people.empty());
		obs.uninstall.phase = phase;
		dockview::DockViewV2 v = decide_dock(obs);
		CHECK(v.people.empty());
		CHECK_FALSE(v.list_reason.has_value());
		REQUIRE(v.empty.has_value());
		CHECK(v.empty->kind == dockview::EmptyKind::Uninstalled);
		CHECK(v.empty->text.id == keys::kDockEmptyUninstalledEmpty);
		CHECK_FALSE(v.empty->button.has_value());
		// 接続していないときも同じ (「Vesktopと接続されると、ここに参加者が表示されます。」は事実と違う)。
		Observation gone = not_connected();
		gone.people.clear();
		gone.uninstall.phase = phase;
		v = decide_dock(gone);
		REQUIRE(v.empty.has_value());
		CHECK(v.empty->kind == dockview::EmptyKind::Uninstalled);
		// 待ちを通した後も同じ (stabilize() は一覧と空の一覧をそのまま渡す)。
		const StabilizeResult s = stabilize(NoticeTrackerState{}, v, kNow, NoticeTrackerConfig{});
		CHECK(s.view.people.empty());
		REQUIRE(s.view.empty.has_value());
		CHECK(s.view.empty->kind == dockview::EmptyKind::Uninstalled);
	}
	// 始める前は人の行を作る。
	CHECK_FALSE(decide_dock(healthy()).people.empty());
}

TEST_CASE("アンインストールの後にシーンコレクションのフォルダーを読めなかっただけのときは、すべてを開く文を置く")
{
	Observation obs = healthy();
	obs.uninstall.phase = UninstallPhase::Done;
	obs.uninstall.folder_to_delete = "C:/ProgramData/obs-studio/plugins/cordmux";
	obs.uninstall.leftovers = {{UninstallLeftoverKind::CollectionFolderUnreadable, ""}};
	dockview::DockViewV2 v = decide_dock(obs);
	std::vector<dockview::Notice> n = notices_of(v, "Uninstalled");
	REQUIRE(n.size() == 1);
	REQUIRE(n[0].body.size() == 3);
	CHECK(n[0].body[0].id == keys::kDialogLeftCollectionFolderItemText);
	CHECK(n[0].body[0].args.empty());
	// 名前の分かるシーンコレクションがないので、「この」「これらの」ではなく、すべてを開く文にする。
	CHECK(n[0].body[1].id == keys::kDialogUninstallUncheckedAllNextText);
	CHECK(n[0].body[2].id == keys::kDockUninstallDoneText);

	// ダイアログの View: 見出しは「確認できなかったもの:」で、項目は理由の文 1 つ。
	const auto &result = v.uninstall_result;
	REQUIRE(result.has_value());
	REQUIRE(result->groups.size() == 1);
	CHECK(result->groups[0].heading.id == keys::kDialogNotCheckedText);
	REQUIRE(result->groups[0].items.size() == 1);
	CHECK(result->groups[0].items[0].is_text());
	CHECK(result->groups[0].items[0].text->id == keys::kDialogLeftCollectionFolderItemText);
}

TEST_CASE("シーンコレクションのフォルダーを読めなかったことが、名前の分かるシーンコレクションと一緒にあれば、数は名前で選ぶ")
{
	Observation obs = healthy();
	obs.uninstall.phase = UninstallPhase::Done;
	obs.uninstall.folder_to_delete = "C:/ProgramData/obs-studio/plugins/cordmux";
	obs.uninstall.leftovers = {{UninstallLeftoverKind::CollectionNotOpened, "stream"},
				   {UninstallLeftoverKind::CollectionFolderUnreadable, ""}};
	const std::vector<dockview::Notice> n = notices_of(decide_dock(obs), "Uninstalled");
	REQUIRE(n.size() == 1);
	REQUIRE(n[0].body.size() == 4);
	CHECK(n[0].body[0].id == keys::kDialogLeftCollectionNotOpenedItemText);
	CHECK(n[0].body[1].id == keys::kDialogLeftCollectionFolderItemText);
	CHECK(n[0].body[2].id == keys::kDialogUninstallUncheckedNextText.one); // 名前の分かる 1 つ
}

TEST_CASE("アンインストールの後に確認できなかったシーンコレクションがあれば、完了のカード 1 枚に利用者がする順で並べる")
{
	Observation obs = healthy();
	obs.uninstall.phase = UninstallPhase::Done;
	obs.uninstall.folder_to_delete = "C:/ProgramData/obs-studio/plugins/cordmux";
	obs.uninstall.leftovers = {{UninstallLeftoverKind::CollectionNotOpened, "stream"},
				   {UninstallLeftoverKind::CollectionNameDuplicate, "dup"}};
	dockview::DockViewV2 v = decide_dock(obs);
	std::vector<dockview::Notice> n = notices_of(v, "Uninstalled");
	REQUIRE(n.size() == 1);
	CHECK(n[0].tier == dockview::Tier::L2);
	CHECK(n[0].headline.id == keys::kDockUninstallDoneHeadline);
	CHECK_FALSE(n[0].headline_short.has_value());
	// ダイアログのまとまりの見出し「次のシーンコレクションは確認できませんでした。」は置かず、理由の付いた項目から
	// 始める (カードには箇条書きの記号がないので、見出しと理由が同じことを 2 度言うことになる)。
	REQUIRE(n[0].body.size() == 4);
	for (const TextRef &t : n[0].body) {
		CHECK(t.id != keys::kDialogUninstallUncheckedText.other);
		CHECK(t.id != keys::kDialogUninstallUncheckedText.one);
	}
	CHECK(n[0].body[0].id == keys::kDialogLeftCollectionNotOpenedItemText);
	CHECK(n[0].body[0].args == std::vector<std::string>{"stream"});
	CHECK(n[0].body[1].id == keys::kDialogLeftCollectionDuplicateItemText);
	// 次の操作の文は、シーンコレクションの数で選ぶ (2 つなら「これらの」/ "these")。
	CHECK(n[0].body[2].id == keys::kDialogUninstallUncheckedNextText.other);
	// 削除の文を最後に置き、その直後に来る場所の行と並べる。
	CHECK(n[0].body[3].id == keys::kDockUninstallDoneText);
	REQUIRE(n[0].location.has_value());
	CHECK(n[0].location->path == "C:/ProgramData/obs-studio/plugins/cordmux");
	CHECK(n[0].location->open_folder);
	CHECK(n[0].buttons.empty());
	// 1 行目はこのカードの見出し (ダイアログの主文の指示ではない)。
	CHECK(v.first_line.kind == dockview::FirstLineKind::Notice);
	CHECK(form_ids(v.first_line) == std::vector<std::string>{keys::kDockUninstallDoneHeadline});

	// 手順書と違う置き方: 削除するフォルダーが 1 か所ではないので場所の行はなく、名前を伝える文を追加する。
	Observation loose = obs;
	loose.uninstall.folder_to_delete.clear();
	n = notices_of(decide_dock(loose), "Uninstalled");
	REQUIRE(n.size() == 1);
	REQUIRE(n[0].body.size() == 5);
	CHECK(n[0].body[2].id == keys::kDialogUninstallUncheckedNextText.other);
	CHECK(n[0].body[3].id == keys::kDockUninstallDoneLooseText);
	CHECK(n[0].body[4].id == keys::kDialogUninstallDoneLooseText);
	CHECK_FALSE(n[0].location.has_value());

	// 残りがないときも同じカード 1 枚にする (メッセージのレベル 2)。1 行目だけでは、指示が途中で省略され、全文と場所が
	// ツールチップにしか残らない。本文は削除の文だけで、場所の行がその直後に来る。
	Observation clean = obs;
	clean.uninstall.leftovers.clear();
	v = decide_dock(clean);
	n = notices_of(v, "Uninstalled");
	REQUIRE(n.size() == 1);
	CHECK(n[0].tier == dockview::Tier::L2);
	CHECK(n[0].key == "Uninstalled:Done");
	CHECK(n[0].headline.id == keys::kDockUninstallDoneHeadline);
	REQUIRE(n[0].body.size() == 1);
	CHECK(n[0].body[0].id == keys::kDockUninstallDoneText);
	REQUIRE(n[0].location.has_value());
	CHECK(n[0].location->path == "C:/ProgramData/obs-studio/plugins/cordmux");
	CHECK(form_ids(v.first_line) == std::vector<std::string>{keys::kDockUninstallDoneHeadline});

	Observation clean_loose = clean;
	clean_loose.uninstall.folder_to_delete.clear();
	n = notices_of(decide_dock(clean_loose), "Uninstalled");
	REQUIRE(n.size() == 1);
	REQUIRE(n[0].body.size() == 2);
	CHECK(n[0].body[0].id == keys::kDockUninstallDoneLooseText);
	CHECK(n[0].body[1].id == keys::kDialogUninstallDoneLooseText);
	CHECK_FALSE(n[0].location.has_value());
}

TEST_CASE("確認できなかったシーンコレクションが 1 つなら、見出しと次の操作の文を _one にする (数は名前で数える)")
{
	Observation obs = healthy();
	obs.uninstall.phase = UninstallPhase::Done;
	obs.uninstall.folder_to_delete = "C:/ProgramData/obs-studio/plugins/cordmux";
	// 1 つのシーンコレクションが「開けなかった」と「同じ名前が 2 つ以上」の両方に出る (install/uninstall.cpp の
	// walk_collections() は、両方に同じ名前を入れることがある)。項目は 2 つでも、シーンコレクションは 1 つ。
	obs.uninstall.leftovers = {{UninstallLeftoverKind::CollectionNotOpened, "stream"},
				   {UninstallLeftoverKind::CollectionNameDuplicate, "stream"}};
	const dockview::DockViewV2 v = decide_dock(obs);
	const std::vector<dockview::Notice> n = notices_of(v, "Uninstalled");
	REQUIRE(n.size() == 1);
	REQUIRE(n[0].body.size() == 4);
	CHECK(n[0].body[0].id == keys::kDialogLeftCollectionNotOpenedItemText);
	CHECK(n[0].body[1].id == keys::kDialogLeftCollectionDuplicateItemText);
	CHECK(n[0].body[2].id == keys::kDialogUninstallUncheckedNextText.one);

	// ダイアログ: 名前だけの箇条書きなので、同じ名前は 1 つにまとめる。見出しは 1 つのときの形。
	REQUIRE(v.uninstall_result.has_value());
	REQUIRE(v.uninstall_result->groups.size() == 1);
	const dockview::LeftoverGroup &group = v.uninstall_result->groups[0];
	CHECK(group.heading.id == keys::kDialogUninstallUncheckedText.one);
	REQUIRE(group.items.size() == 1);
	CHECK(group.items[0].literal == "stream");

	// 名前が 2 つなら、どちらも 2 つ以上のときの形。
	Observation two = obs;
	two.uninstall.leftovers = {{UninstallLeftoverKind::CollectionNotOpened, "stream"},
				   {UninstallLeftoverKind::CollectionNameDuplicate, "dup"}};
	const dockview::DockViewV2 v2 = decide_dock(two);
	REQUIRE(v2.uninstall_result.has_value());
	CHECK(v2.uninstall_result->groups[0].heading.id == keys::kDialogUninstallUncheckedText.other);
	CHECK(v2.uninstall_result->groups[0].items.size() == 2);
	CHECK(notices_of(v2, "Uninstalled")[0].body[2].id == keys::kDialogUninstallUncheckedNextText.other);
}

TEST_CASE("途中で止まったアンインストールのカードは、まとまりごとに見出し・項目と場所・次の操作を持ち、"
	  "確認・設定・削除の順に並べ、まとまりの間に区切りを置く")
{
	const auto ids = [](const dockview::Notice &n) {
		std::vector<std::string> out;
		for (const TextRef &t : n.body)
			out.push_back(t.id);
		return out;
	};
	// 場所の行を「直前の文のキー -> パス」の組で並べる (どの文の直後に来るかをキーで確認する)。
	// 文の番号の外を指す行は「(外)」にする (表示する側は描かない約束なので、作る側が作らないことを確認する)。
	const auto places = [](const dockview::Notice &n) {
		std::vector<std::pair<std::string, std::string>> out;
		for (const dockview::BodyLocation &line : n.body_locations)
			out.emplace_back(line.after < n.body.size() ? n.body[line.after].id : std::string("(外)"),
					 line.line.path);
		return out;
	};
	using Places = std::vector<std::pair<std::string, std::string>>;

	// 3 つのまとまりがすべて表示される。設定のファイルの場所が 2 つ (設定のフォルダーと、接続のための情報の
	// 保存先)。プラグインのフォルダーも分かる。
	Observation all = healthy();
	all.uninstall.phase = UninstallPhase::Stopped;
	all.uninstall.folder_to_delete = "C:/plugins/cordmux";
	all.uninstall.leftovers = {{UninstallLeftoverKind::SettingsFiles, "C:/cfg"},
				   {UninstallLeftoverKind::LocalDataFolder, "C:/local"},
				   {UninstallLeftoverKind::Sources, "stream"},
				   {UninstallLeftoverKind::VencordDir, "C:/state.json"},
				   {UninstallLeftoverKind::CollectionNameDuplicate, "dup"}};
	dockview::DockViewV2 v = decide_dock(all);
	std::vector<dockview::Notice> n = notices_of(v, "Uninstalled");
	REQUIRE(n.size() == 1);
	CHECK(n[0].key == "Uninstalled:Stopped");
	CHECK(n[0].tier == dockview::Tier::L2);
	CHECK(n[0].headline.id == keys::kDialogUninstallStoppedHeadline);
	CHECK(ids(n[0]) == std::vector<std::string>{
				   keys::kDialogNotCheckedText,
				   keys::kDialogLeftCollectionDuplicateItemText,
				   keys::kDialogUninstallUncheckedNextText.one,
				   keys::kDialogNotRestoredText,
				   keys::kDialogLeftVesktopSettingsItemText,
				   keys::kDialogNotRestoredNextText,
				   // 設定を元に戻せなかったときの設定のフォルダーには、元に戻すためのバックアップが
				   // あるので、プラグインのフォルダーと同じ注意で示す (場所の行が 2 つなので、複数の
				   // ときのテキスト)。
				   keys::kDialogDontDeleteFoldersText,
				   keys::kDialogNotDeletedText,
				   // 設定のファイルの項目に残るのは、接続のための情報の保存先だけ (手で削除してよい)。
				   keys::kDialogLeftSettingsFilesItemText,
				   keys::kDialogLeftSourcesItemText,
				   keys::kDialogNotDeletedNextText,
			   });
	// まとまりをまたいで「残ったもの」と言う前の 1 文 (uninstall_again) は、上の並びにない。キーを名前で指して
	// 確認しないのは、使われなくなったキーを text_ids.h から消したときに、このテストがビルドできなくならない
	// ためである。
	// まとまりの間の区切り: 2 つ目と 3 つ目のまとまりの見出しの前 (1 つ目の前には入れない)。
	CHECK(n[0].body_group_starts == std::vector<std::size_t>{3, 7});
	// 場所の行は、ダイアログと同じ数と位置: Vesktop の設定の項目の下にファイル、注意の直後にプラグインの
	// フォルダーと設定のフォルダー、設定のファイルの項目の下に接続のための情報の保存先。
	CHECK(places(n[0]) == Places{{keys::kDialogLeftVesktopSettingsItemText, "C:/state.json"},
				     {keys::kDialogDontDeleteFoldersText, "C:/plugins/cordmux"},
				     {keys::kDialogDontDeleteFoldersText, "C:/cfg"},
				     {keys::kDialogLeftSettingsFilesItemText, "C:/local"}});
	// 種類と [フォルダーを開く]: Vesktop の設定はファイルの行 (open_folder は false)、ほかはフォルダーの行。
	REQUIRE(n[0].body_locations.size() == 4);
	CHECK(n[0].body_locations[0].line.kind == dockview::LocationKind::File);
	CHECK_FALSE(n[0].body_locations[0].line.open_folder);
	for (std::size_t i = 1; i < 4; ++i) {
		CHECK(n[0].body_locations[i].line.kind == dockview::LocationKind::Folder);
		CHECK(n[0].body_locations[i].line.open_folder);
	}
	// カードの最後の場所の行 (Notice::location) は使わない (場所はまとまりの中にある)。
	CHECK_FALSE(n[0].location.has_value());
	CHECK(n[0].buttons.empty());
	// ダイアログも同じ値: 止まったときの folder_to_delete は、注意が指すプラグインのフォルダー。
	REQUIRE(v.uninstall_result.has_value());
	REQUIRE(v.uninstall_result->folder_to_delete.has_value());
	CHECK(v.uninstall_result->folder_to_delete->path == "C:/plugins/cordmux");

	// 削除できなかった設定のファイルの場所が 1 つ: 削除のまとまりだけ。場所の行は項目の直後。まとまりの区切りは
	// ない (まとまりが 1 つ)。
	Observation one_place = all;
	one_place.uninstall.leftovers = {{UninstallLeftoverKind::SettingsFiles, "C:/cfg"},
					 {UninstallLeftoverKind::Sources, "stream"}};
	n = notices_of(decide_dock(one_place), "Uninstalled");
	REQUIRE(n.size() == 1);
	// 設定のフォルダーの残りがあるので、次の操作の文は、手で削除する方法を伝えない (設定を消す段階で止まった
	// ときで、バックアップが残っている)。
	CHECK(ids(n[0]) == std::vector<std::string>{keys::kDialogNotDeletedText, keys::kDialogLeftSettingsFilesItemText,
						    keys::kDialogLeftSourcesItemText,
						    keys::kDialogNotDeletedUninstallAgainNextText});
	CHECK(n[0].body[2].args == std::vector<std::string>{"stream"});
	CHECK(places(n[0]) == Places{{keys::kDialogLeftSettingsFilesItemText, "C:/cfg"}});
	CHECK(n[0].body_group_starts.empty());
	CHECK_FALSE(n[0].location.has_value());

	// 元に戻せなかった設定だけで、プラグインのフォルダーが分からない (手順書と違う置き方など): 注意の後に場所の行を
	// 置かない (場所のない行を作らない)。注意は「次のフォルダー」ではなく「cordmuxのファイル」のテキストにする
	// (指すもののない「次の」を表示しない)。Vesktop の設定のファイルの行は置く。
	Observation restore_only = all;
	restore_only.uninstall.folder_to_delete.clear();
	restore_only.uninstall.leftovers = {{UninstallLeftoverKind::VencordDir, "C:/state.json"}};
	n = notices_of(decide_dock(restore_only), "Uninstalled");
	REQUIRE(n.size() == 1);
	CHECK(ids(n[0]) == std::vector<std::string>{keys::kDialogNotRestoredText, keys::kDialogLeftVesktopSettingsItemText,
						    keys::kDialogNotRestoredNextText, keys::kDialogDontDeleteFilesText});
	CHECK(places(n[0]) == Places{{keys::kDialogLeftVesktopSettingsItemText, "C:/state.json"}});
	// 「次のフォルダー」のテキストは、直後に場所の行があるときだけ使う (上の all の並び)。
	const std::vector<std::string> restore_only_ids = ids(n[0]);
	CHECK(std::find(restore_only_ids.begin(), restore_only_ids.end(), std::string(keys::kDialogDontDeleteFolderText)) ==
	      restore_only_ids.end());
	CHECK_FALSE(n[0].location.has_value());
	v = decide_dock(restore_only);
	REQUIRE(v.uninstall_result.has_value());
	CHECK_FALSE(v.uninstall_result->folder_to_delete.has_value());

	// 場所の分からない残り (設定のファイルの場所が空) は場所の行を作らない。
	Observation no_where = all;
	no_where.uninstall.leftovers = {{UninstallLeftoverKind::SettingsFiles, ""}};
	n = notices_of(decide_dock(no_where), "Uninstalled");
	REQUIRE(n.size() == 1);
	CHECK(n[0].body_locations.empty());

	// 1 行目は、止まったときの主文。
	CHECK(form_ids(v.first_line) == std::vector<std::string>{keys::kDialogUninstallStoppedHeadline});
}

TEST_CASE("[バージョン情報] のアンインストールのボタンは、最中だけ「アンインストール中...」にし、押せない理由は残す")
{
	Observation obs = healthy();
	CHECK(decide_dock(obs).about.uninstall_button.label.id == keys::kSettingsUninstallButton);
	obs.uninstall.phase = UninstallPhase::Running;
	const dockview::ViewButton running = decide_dock(obs).about.uninstall_button;
	CHECK(running.label.id == keys::kSettingsUninstallBusyButton);
	REQUIRE(running.disabled_reason.has_value());
	CHECK(running.disabled_reason->id == keys::kSettingsUninstallStartedText);
	for (const UninstallPhase phase : {UninstallPhase::Done, UninstallPhase::Stopped}) {
		obs.uninstall.phase = phase;
		CHECK(decide_dock(obs).about.uninstall_button.label.id == keys::kSettingsUninstallButton);
	}
}

TEST_CASE("人の行は、2 行目に表示するかにかかわらずユーザー名を持ち、Vesktop のアカウントの行は持たない")
{
	Observation obs = healthy();
	obs.people[0].username = "taro_1";
	PersonFact me = present("900", "receiver");
	me.is_receiver = true;
	me.username = "receiver_1";
	obs.people.push_back(me);
	const dockview::DockViewV2 v = decide_dock(obs);
	const dockview::PersonRow *taro = row_of(v, "100");
	REQUIRE(taro != nullptr);
	REQUIRE(taro->username.has_value());
	CHECK(*taro->username == "taro_1");
	CHECK_FALSE(taro->same_discord_name_exists);
	for (const TextRef &t : taro->line2_parts)
		CHECK(t.id != keys::kDockRowUsernameStatus); // 2 行目には表示しない (同じ Discord での名前の人がいない)
	const dockview::PersonRow *receiver = row_of(v, "900");
	REQUIRE(receiver != nullptr);
	CHECK_FALSE(receiver->username.has_value());
	// ユーザー名が届いていない人は持たない。
	const dockview::PersonRow *hanako = row_of(v, "200");
	REQUIRE(hanako != nullptr);
	CHECK_FALSE(hanako->username.has_value());
}

TEST_CASE("声が二重になるメッセージは見出しにソースの名前を入れ、名前がなければ表示しない")
{
	Observation obs = healthy();
	obs.choices.discord_audio_source = DiscordAudioSource::Found;
	obs.choices.discord_audio_source_name = "Discord audio";
	std::vector<dockview::Notice> n = notices_of(decide_dock(obs), "DoubleAudio");
	REQUIRE(n.size() == 1);
	CHECK(n[0].tier == dockview::Tier::L2);
	CHECK(n[0].headline.id == keys::kDockDoubleAudioHeadline);
	CHECK(n[0].headline.args == std::vector<std::string>{"Discord audio"});
	REQUIRE(n[0].body.size() == 1);
	CHECK(n[0].body[0].id == keys::kDockDoubleAudioText);
	// 本文は、ミュートするソースの名前を持つ。
	CHECK(n[0].body[0].args == std::vector<std::string>{"Discord audio"});

	obs.choices.discord_audio_source_name.clear();
	CHECK(notices_of(decide_dock(obs), "DoubleAudio").empty());
}

TEST_CASE("候補の人がほかの人の声を使っているときは選べず、ドックは括弧の短い理由、ダイアログは名前を入れた理由にする")
{
	Observation obs = healthy();
	// hanako (200) が過去の参加者 ghost (800) の声を使っている。taro が hanako の声を使うと連鎖になる。
	SeenPerson hanako;
	hanako.user_id = "200";
	hanako.display_name = "hanako";
	hanako.voice_source_user_id = "800";
	obs.seen.people.push_back(hanako);
	const dockview::DockViewV2 v = decide_dock(obs);
	const dockview::PersonRow *taro = row_of(v, "100");
	REQUIRE(taro != nullptr);
	REQUIRE(taro->menu.voices.size() == 2);
	const dockview::VoiceChoice &dock_item = taro->menu.voices[1];
	REQUIRE(dock_item.action.disabled_reason.has_value());
	CHECK(dock_item.action.disabled_reason->id == keys::kParticipantsVoiceUsesOtherText);
	CHECK(dock_item.action.disabled_reason->args == std::vector<std::string>{"hanako"});
	REQUIRE(dock_item.label.text.has_value());
	CHECK(dock_item.label.text->id == keys::kDockRowVoiceUsesOtherMenu);
	CHECK(dock_item.label.text->args == std::vector<std::string>{"hanako"});

	// 参加者の設定ウィンドウ: 項目のテキストは名前のままで、理由に名前を入れる。
	const dockview::ParticipantRow *window_taro = nullptr;
	for (const dockview::ParticipantRow &row : v.participants)
		if (row.user_id == "100")
			window_taro = &row;
	REQUIRE(window_taro != nullptr);
	REQUIRE(window_taro->voices.size() == 2);
	CHECK_FALSE(window_taro->voices[1].label.text.has_value());
	REQUIRE(window_taro->voices[1].action.disabled_reason.has_value());
	CHECK(window_taro->voices[1].action.disabled_reason->args == std::vector<std::string>{"hanako"});
}

TEST_CASE("アンインストールを始めた後の押せない理由は 1 つのテキストで、アンインストールのボタンは最中と完了で分ける")
{
	for (const UninstallPhase phase : {UninstallPhase::Running, UninstallPhase::Done, UninstallPhase::Stopped}) {
		CAPTURE(static_cast<int>(phase));
		Observation obs = healthy();
		obs.uninstall.phase = phase;
		const dockview::DockViewV2 v = decide_dock(obs);
		// 人の行は作らない (上の、アンインストールを始めた後の一覧のテスト)。人の行の操作の理由は、apply() の側
		// (同じ 1 か所) で見る。
		ActionArgs slot_args;
		slot_args.user_id = "200";
		slot_args.kind = cordmux::config::SlotKind::Screen;
		slot_args.slot = 1;
		const Intent slot = apply(obs, ActionId::SetSlot, slot_args);
		REQUIRE(slot.disabled.has_value());
		CHECK(std::string(slot.disabled->reason_text_id) == keys::kSettingsUninstallStartedText);
		REQUIRE(v.record.action.disabled_reason.has_value());
		CHECK(v.record.action.disabled_reason->id == keys::kSettingsUninstallStartedText);
		if (phase == UninstallPhase::Running) {
			REQUIRE(v.about.uninstall_button.disabled_reason.has_value());
			CHECK(v.about.uninstall_button.disabled_reason->id == keys::kSettingsUninstallStartedText);
		} else if (phase == UninstallPhase::Done) {
			REQUIRE(v.about.uninstall_button.disabled_reason.has_value());
			CHECK(v.about.uninstall_button.disabled_reason->id == keys::kSettingsUninstallDoneButtonText);
		} else {
			// 止まったときは押し直せる (押せないのは、配信中や Vesktop の起動中などの理由があるときだけ。
			// ここでは Vesktop が起動している)。
			if (v.about.uninstall_button.disabled_reason.has_value()) {
				CHECK(v.about.uninstall_button.disabled_reason->id != keys::kSettingsUninstallStartedText);
				CHECK(v.about.uninstall_button.disabled_reason->id != keys::kSettingsUninstallDoneButtonText);
			}
			Observation idle = obs;
			idle.install.process = VesktopProcess::NotRunning;
			CHECK_FALSE(decide_dock(idle).about.uninstall_button.disabled_reason.has_value());
		}
	}
}

namespace {

std::vector<std::string> audit_body_ids(const dockview::Notice &notice)
{
	std::vector<std::string> ids;
	for (const TextRef &t : notice.body)
		ids.push_back(t.id);
	return ids;
}

} // namespace

TEST_CASE("録画ファイルの作成の失敗はメッセージのレベル 2 のカード 1 枚で、帰結・次の操作・セッションのフォルダー・"
	  "[作成を再開] を持ち、作成待ちのカードを重ねない")
{
	Observation obs = healthy();
	obs.recording.finalize = FinalizeState::Failed;
	obs.recording.session_dir = "D:/rec/cordmux/2000-01-01_2000";
	// 録画の側は、録画ファイルの作成に失敗したセッションも作成待ちに数える。
	obs.recording.pending_present = true;
	obs.recording.pending_session_count = 1;
	dockview::DockViewV2 v = decide_dock(obs);
	std::vector<dockview::Notice> n = notices_of(v, "RecordFinalizeFailed");
	REQUIRE(n.size() == 1);
	CHECK(n[0].tier == dockview::Tier::L2);
	CHECK(n[0].log_level == dockview::LogLevel::Warning);
	CHECK(n[0].headline.id == keys::kDockFinalizeFailedHeadline);
	CHECK(audit_body_ids(n[0]) == std::vector<std::string>{keys::kDockFinalizePendingText, keys::kDockFinalizeFailedNextText});
	REQUIRE(n[0].location.has_value());
	CHECK(n[0].location->path == "D:/rec/cordmux/2000-01-01_2000");
	CHECK(n[0].location->open_folder);
	REQUIRE(n[0].buttons.size() == 1);
	CHECK(n[0].buttons[0].id == ActionId::ResumeFinalize);
	CHECK(n[0].buttons[0].label.id == keys::kDockResumeFinalizingButton);
	CHECK_FALSE(n[0].buttons[0].disabled_reason.has_value());
	// 帰結とボタンが同じ作成待ちのカードは表示しない。
	CHECK(notices_of(v, "RecordFinalizePending").empty());
	// 1 行目は、このカードの見出し。
	CHECK(v.first_line.kind == dockview::FirstLineKind::Notice);
	CHECK(form_ids(v.first_line).front() == keys::kDockFinalizeFailedHeadline);

	// セッションのフォルダーが分からなければ、場所の行を置かない。
	Observation no_dir = obs;
	no_dir.recording.session_dir.clear();
	n = notices_of(decide_dock(no_dir), "RecordFinalizeFailed");
	REQUIRE(n.size() == 1);
	CHECK_FALSE(n[0].location.has_value());

	// 押して作成し直している間 (Running) は表示しない。完成すれば (Done) も表示しない。
	for (const FinalizeState state : {FinalizeState::Running, FinalizeState::Done, FinalizeState::Pending,
					  FinalizeState::NotStarted, FinalizeState::Unknown}) {
		CAPTURE(static_cast<int>(state));
		Observation other = obs;
		other.recording.finalize = state;
		other.recording.pending_present = false;
		CHECK(notices_of(decide_dock(other), "RecordFinalizeFailed").empty());
	}

	// 途中で打ち切られただけのセッション (録画ログに失敗がない。この起動の finalize は NotStarted で、作成待ちに
	// 数えられている): 作成待ちのカードが伝える。
	Observation earlier = healthy();
	earlier.recording.finalize = FinalizeState::NotStarted;
	earlier.recording.pending_present = true;
	v = decide_dock(earlier);
	CHECK(notices_of(v, "RecordFinalizeFailed").empty());
	CHECK(notices_of(v, "RecordFinalizePending").size() == 1);

	// 失敗したセッションが録画ログに残ったまま、次のセッションを録画している (録画ファイルの作成が成功するまでメッセージ続ける)。
	// メッセージのレベル 2 のカードのままで、場所の行は失敗したセッション (今録画しているセッションのフォルダーでは
	// ない)。作成待ちのカードは重ねない。
	Observation later = healthy();
	later.recording.finalize = FinalizeState::NotStarted;
	later.recording.session_dir = "D:/rec/cordmux/2000-01-01_2200";
	later.recording.failed_session_dir = "D:/rec/cordmux/2000-01-01_2000";
	later.recording.pending_present = true;
	v = decide_dock(later);
	n = notices_of(v, "RecordFinalizeFailed");
	REQUIRE(n.size() == 1);
	CHECK(n[0].tier == dockview::Tier::L2);
	REQUIRE(n[0].location.has_value());
	CHECK(n[0].location->path == "D:/rec/cordmux/2000-01-01_2000");
	CHECK_FALSE(n[0].buttons[0].disabled_reason.has_value());
	CHECK(notices_of(v, "RecordFinalizePending").empty());
}

TEST_CASE("録画ファイルの作成の失敗の次の操作は、FFmpeg を起動できなかったときだけ FFmpeg のファイルの置き直しにする")
{
	Observation obs = healthy();
	obs.recording.finalize = FinalizeState::Failed;
	obs.recording.session_dir = "D:/rec/cordmux/2000-01-01_2000";
	obs.recording.pending_present = true;
	obs.recording.pending_session_count = 1;

	// 理由が分からない失敗は、空き容量の確認を言う。
	std::vector<dockview::Notice> n = notices_of(decide_dock(obs), "RecordFinalizeFailed");
	REQUIRE(n.size() == 1);
	CHECK(audit_body_ids(n[0]) == std::vector<std::string>{keys::kDockFinalizePendingText, keys::kDockFinalizeFailedNextText});

	obs.recording.finalize_ffmpeg_unavailable = true;
	n = notices_of(decide_dock(obs), "RecordFinalizeFailed");
	REQUIRE(n.size() == 1);
	CHECK(audit_body_ids(n[0]) ==
	      std::vector<std::string>{keys::kDockFinalizePendingText, keys::kDockFinalizeFailedFfmpegNextText});
	REQUIRE(n[0].buttons.size() == 1);
	CHECK(n[0].buttons[0].id == ActionId::ResumeFinalize);

	// 録画ログに失敗が残っているだけ (この起動の finalize が Failed ではない) なら、理由は分からないので使わない。
	obs.recording.finalize = FinalizeState::NotStarted;
	obs.recording.failed_session_dir = "D:/rec/cordmux/2000-01-01_2000";
	n = notices_of(decide_dock(obs), "RecordFinalizeFailed");
	REQUIRE(n.size() == 1);
	CHECK(audit_body_ids(n[0]) == std::vector<std::string>{keys::kDockFinalizePendingText, keys::kDockFinalizeFailedNextText});
}

TEST_CASE("接続が切れた後に Vesktop が終了したときは、配信していなければ平常の 1 行目にし、"
	  "配信中で枠に人がいればメッセージのレベル 1 のまま起動を勧める")
{
	Observation obs = not_connected();
	obs.link.state = LinkState::Disconnected;
	obs.link.connected_at_us = kNow - 600 * kSec;
	obs.link.disconnected_at_us = kNow - 30 * kSec;
	obs.link.reason = DisconnectReason::PeerClosed;
	obs.install.process = VesktopProcess::NotRunning;

	// 配信していない: 「Vesktopが起動していません」の平常の 1 行目。メッセージのレベル 1 は表示しない。
	dockview::DockViewV2 v = decide_dock(obs);
	CHECK(primary_rule_of(obs) == PrimaryRuleId::VesktopNotRunning);
	CHECK(notices_of(v, "ConnectedThenDropped").empty());
	CHECK(v.first_line.kind == dockview::FirstLineKind::Summary);
	CHECK(form_ids(v.first_line) == std::vector<std::string>{keys::kDockSummaryVesktopNotRunningStatus});

	// 配信中でも枠に人がいなければ同じ (損なわれている枠がない)。
	Observation empty_slots = obs;
	empty_slots.obs_outputs.streaming = true;
	empty_slots.people[0].slot.reset();
	CHECK(primary_rule_of(empty_slots) == PrimaryRuleId::VesktopNotRunning);

	// 配信中で枠に人がいる: メッセージのレベル 1 の切断のカード。本文は起動を勧める 1 文 (切れた理由ごとの文ではない)。
	Observation live = obs;
	live.obs_outputs.streaming = true;
	v = decide_dock(live);
	std::vector<dockview::Notice> n = notices_of(v, "ConnectedThenDropped");
	REQUIRE(n.size() == 1);
	CHECK(n[0].tier == dockview::Tier::L1);
	CHECK(n[0].headline.id == keys::kDockDisconnectedHeadline);
	CHECK(audit_body_ids(n[0]) == std::vector<std::string>{keys::kDockDisconnectedVesktopNotRunningText});
	// トークンが合わずに切れていても、終了している Vesktop には起動を勧める。録画中は、録画の 1 文を後に追加する。
	live.link.reason = DisconnectReason::SecretMismatch;
	live.choices.recording = FeatureSwitch::On;
	n = notices_of(decide_dock(live), "ConnectedThenDropped");
	REQUIRE(n.size() == 1);
	CHECK(audit_body_ids(n[0]) ==
	      std::vector<std::string>{keys::kDockDisconnectedVesktopNotRunningText, keys::kDockDisconnectedRecordingText});
	// 配信中かが分からない間は、配信中とみなさない。
	Observation unknown_stream = obs;
	unknown_stream.obs_outputs.streaming.reset();
	CHECK(primary_rule_of(unknown_stream) == PrimaryRuleId::VesktopNotRunning);

	// 起動している・まだ確認していない: メッセージのレベル 1 と、切れた理由ごとの文。
	for (const VesktopProcess process : {VesktopProcess::Running, VesktopProcess::Unknown}) {
		CAPTURE(static_cast<int>(process));
		Observation still = obs;
		still.install.process = process;
		CHECK(primary_rule_of(still) == PrimaryRuleId::ConnectedThenDropped);
		n = notices_of(decide_dock(still), "ConnectedThenDropped");
		REQUIRE(n.size() == 1);
		CHECK(n[0].tier == dockview::Tier::L1);
		CHECK(audit_body_ids(n[0]) == std::vector<std::string>{keys::kDockDisconnectedText});
	}
}

TEST_CASE("映像が届かない人のメッセージは、対象の中に映像が届いている人がいれば出さない")
{
	// 4 人を枠に載せ、1 人 (ren) だけが止まった。
	Observation obs = healthy();
	obs.people[1] = showing("200", "hanako", 2);
	obs.people.push_back(showing("300", "kenji", 3));
	PersonFact ren = showing("400", "ren", 4);
	ren.last_video_frame_at_us = kNow - 5 * kSec;
	obs.people.push_back(ren);
	obs.call.roster_count = 4;
	CHECK(notices_of(decide_dock(obs), "AllVideoStoppedHint").empty());

	// 3 人が止まっても、1 人の映像が届いていれば出さない。
	obs.people[0].last_video_frame_at_us = kNow - 5 * kSec;
	obs.people[1].last_video_frame_at_us = kNow - 5 * kSec;
	CHECK(notices_of(decide_dock(obs), "AllVideoStoppedHint").empty());

	// 全員が止まった: Vesktop で確認する文。
	for (PersonFact &p : obs.people)
		p.last_video_frame_at_us = kNow - 5 * kSec;
	std::vector<dockview::Notice> n = notices_of(decide_dock(obs), "AllVideoStoppedHint");
	REQUIRE(n.size() == 1);
	CHECK(n[0].tier == dockview::Tier::L2);
	CHECK(n[0].headline.args == std::vector<std::string>{"4"});
	CHECK(audit_body_ids(n[0]) == std::vector<std::string>{keys::kDockNoVideoText, keys::kDockNoVideoNextText});

	// 届いているのが、枠にも録画にも入っていない人だけ (録画していないときの、枠に載せていない人のフレーム) なら
	// 数えない: 枠にも録画の入力にも通らない映像は、届いているかが分からない。
	Observation alone = healthy();
	alone.choices.receive_scope = cordmux::ledger::ReceiveScope::Everyone;
	alone.people[0].last_video_frame_at_us = kNow - 5 * kSec;
	PersonFact second = showing("300", "kenji", 2);
	second.last_video_frame_at_us = kNow - 5 * kSec;
	alone.people.push_back(second);
	alone.people[1].sharing = true;
	alone.people[1].last_record_frame_at_us = kNow - kSec / 10; // 録画は OFF (前の録画の時刻が残っている)
	n = notices_of(decide_dock(alone), "AllVideoStoppedHint");
	REQUIRE(n.size() == 1);
	CHECK(audit_body_ids(n[0]) == std::vector<std::string>{keys::kDockNoVideoText, keys::kDockNoVideoNextText});

	// 録画の対象の人 (枠に載せていない) の、録画の入力にフレームが届いている: 届いている人に数える。
	alone.choices.recording = FeatureSwitch::On;
	alone.recording.phase = RecordPhase::Recording;
	CHECK(notices_of(decide_dock(alone), "AllVideoStoppedHint").empty());

	// 届いているのが段階 unavailable の人だけ (しきい値の時間内に残ったフレーム): 届いている人に数えない
	// (displayed() と同じ理由)。
	Observation stale = healthy();
	stale.people[1] = showing("200", "hanako", 2);
	stale.people[0].last_video_frame_at_us = kNow - 5 * kSec;
	stale.people[1].last_video_frame_at_us = kNow - 5 * kSec;
	PersonFact cant = showing("300", "kenji", 3);
	cant.stage = std::string(kStageUnavailable);
	stale.people.push_back(cant);
	stale.call.roster_count = 3;
	n = notices_of(decide_dock(stale), "AllVideoStoppedHint");
	REQUIRE(n.size() == 1);
	CHECK(audit_body_ids(n[0]) == std::vector<std::string>{keys::kDockNoVideoText, keys::kDockNoVideoNextText});
}

TEST_CASE("録画の対象だけの人の行にも、まだ届いていない・届かなくなったの状態を表示し、ツールチップは録画の文にする")
{
	Observation obs = healthy();
	obs.choices.receive_scope = cordmux::ledger::ReceiveScope::Everyone; // 枠に載っていない人も録画の対象にする
	obs.choices.recording = FeatureSwitch::On;
	obs.recording.phase = RecordPhase::Recording;
	PersonFact &hanako = obs.people[1];
	hanako.sharing = true;
	hanako.sharing_since_us = kNow - 30 * kSec;

	// 録画の入力にフレームがまだ 1 つもない -> まだ届いていない。
	dockview::DockViewV2 first = decide_dock(obs);
	const dockview::PersonRow *row = row_of(first, "200");
	REQUIRE(row != nullptr);
	REQUIRE(row->status.has_value());
	CHECK(row->status->state == dockview::RowState::Waiting);
	CHECK(row->status->text.id == keys::kDockRowWaitingStatus);
	CHECK(row->status->tip.id == keys::kDockRowWaitingRecordingTip);

	// 録画の入力に、しきい値を超えてもフレームが届かない -> 届かなくなった。枠の人 (100) の映像は届いているので、
	// メッセージにはしない。
	hanako.last_record_frame_at_us = kNow - 5 * kSec;
	dockview::DockViewV2 v = decide_dock(obs);
	row = row_of(v, "200");
	REQUIRE(row->status.has_value());
	CHECK(row->status->state == dockview::RowState::NoVideo);
	CHECK(row->status->text.id == keys::kDockRowNotReceivingStatus);
	CHECK(row->status->tip.id == keys::kDockRowNotReceivingRecordingTip);
	CHECK(row->status->settle == dockview::Settle::ClearOnly);
	CHECK(notices_of(v, "AllVideoStoppedHint").empty());

	// 届いている -> 何も表示しない。
	hanako.last_record_frame_at_us = kNow - kSec / 10;
	CHECK_FALSE(row_of(decide_dock(obs), "200")->status.has_value());

	// 録画していなければ表示しない (録画の対象ではない)。
	hanako.last_record_frame_at_us = kNow - 5 * kSec;
	obs.choices.recording = FeatureSwitch::Off;
	obs.recording.phase = RecordPhase::Idle;
	CHECK_FALSE(row_of(decide_dock(obs), "200")->status.has_value());

	// 画面共有していない人は録画の対象ではない (「画面共有していません」も表示しない)。
	obs.choices.recording = FeatureSwitch::On;
	hanako.sharing = false;
	CHECK_FALSE(row_of(decide_dock(obs), "200")->status.has_value());

	// 枠に載っている人のツールチップは枠の文のまま。
	Observation slotted = healthy();
	slotted.people[0].last_video_frame_at_us.reset();
	const dockview::DockViewV2 slotted_view = decide_dock(slotted);
	row = row_of(slotted_view, "100");
	REQUIRE(row->status.has_value());
	CHECK(row->status->tip.id == keys::kDockRowWaitingTip);
	slotted.people[0].last_video_frame_at_us = kNow - 5 * kSec;
	CHECK(row_of(decide_dock(slotted), "100")->status->tip.id == keys::kDockRowNotReceivingTip);
	slotted.people[0].sharing = false;
	CHECK(row_of(decide_dock(slotted), "100")->status->tip.id == keys::kDockRowNotSharingTip);
}

TEST_CASE("保存先に書き込めなかったメッセージは、録画を始めようとしたとき (メッセージのレベル 1) だけもう一度開始する文を持つ")
{
	Observation obs = healthy();
	obs.recording.phase = RecordPhase::StartFailed;
	obs.recording.unwritable_dir = "D:/rec";
	std::vector<dockview::Notice> n = notices_of(decide_dock(obs), "RecordDirUnwritable");
	REQUIRE(n.size() == 1);
	CHECK(n[0].tier == dockview::Tier::L1);
	CHECK(audit_body_ids(n[0]) ==
	      std::vector<std::string>{keys::kDockSaveFolderUnwritableText, keys::kDockRecordStartAgainText});

	obs.recording.phase = RecordPhase::Idle;
	n = notices_of(decide_dock(obs), "RecordDirUnwritable");
	REQUIRE(n.size() == 1);
	CHECK(n[0].tier == dockview::Tier::L2);
	CHECK(audit_body_ids(n[0]) == std::vector<std::string>{keys::kDockSaveFolderUnwritableText});
}

TEST_CASE("途中で止まったアンインストールで、元に戻すためのバックアップが残る設定のフォルダーは、"
	  "どこにも手で削除してよいものとして表示しない")
{
	const auto group_of = [](const dockview::UninstallResultView &r, const char *heading) {
		const dockview::LeftoverGroup *found = nullptr;
		for (const dockview::LeftoverGroup &g : r.groups) {
			if (g.heading.id == heading)
				found = &g;
		}
		return found;
	};
	const auto paths = [](const std::vector<dockview::LocationLine> &lines) {
		std::vector<std::string> out;
		for (const dockview::LocationLine &line : lines)
			out.push_back(line.path);
		return out;
	};

	// (1) 設定を元に戻せず、設定のフォルダーも残りとして示されたとき (製品では同時には起きないが、起きた場合):
	// 設定のフォルダーは「元に戻せなかった設定:」の注意の場所の行に、プラグインのフォルダーの後に並び、
	// 「削除できなかったもの:」には表示しない。
	Observation obs = healthy();
	obs.uninstall.phase = UninstallPhase::Stopped;
	obs.uninstall.folder_to_delete = "C:/plugins/cordmux";
	obs.uninstall.leftovers = {{UninstallLeftoverKind::SettingsFiles, "C:/cfg"},
				   {UninstallLeftoverKind::VencordDir, "C:/state.json"}};
	dockview::DockViewV2 v = decide_dock(obs);
	REQUIRE(v.uninstall_result.has_value());
	CHECK(group_of(*v.uninstall_result, keys::kDialogNotDeletedText) == nullptr);
	const dockview::LeftoverGroup *not_restored = group_of(*v.uninstall_result, keys::kDialogNotRestoredText);
	REQUIRE(not_restored != nullptr);
	CHECK_FALSE(not_restored->next.has_value()); // 次の操作 (next) を持つのは、削除できなかったもののまとまりだけ
	REQUIRE(not_restored->caution.has_value());
	CHECK(not_restored->caution->id == keys::kDialogDontDeleteFoldersText);
	CHECK(paths(not_restored->caution_locations) == std::vector<std::string>{"C:/plugins/cordmux", "C:/cfg"});
	for (const dockview::LocationLine &line : not_restored->caution_locations) {
		CHECK(line.kind == dockview::LocationKind::Folder);
		CHECK(line.open_folder); // [フォルダーを開く] (注意の場所の行)
	}
	// カードも同じテキストと並び。手で削除する方法のテキストは、どこにもない。
	std::vector<dockview::Notice> n = notices_of(v, "Uninstalled");
	REQUIRE(n.size() == 1);
	std::vector<std::string> card = audit_body_ids(n[0]);
	CHECK(card == std::vector<std::string>{keys::kDialogNotRestoredText, keys::kDialogLeftVesktopSettingsItemText,
					       keys::kDialogNotRestoredNextText, keys::kDialogDontDeleteFoldersText});
	CHECK(std::count(card.begin(), card.end(), std::string(keys::kDialogNotDeletedNextText)) == 0);
	std::vector<std::string> card_places;
	for (const dockview::BodyLocation &line : n[0].body_locations) {
		if (line.after < n[0].body.size() && n[0].body[line.after].id == keys::kDialogDontDeleteFoldersText)
			card_places.push_back(line.line.path);
	}
	CHECK(card_places == std::vector<std::string>{"C:/plugins/cordmux", "C:/cfg"});

	// 同時にシーンと接続のための情報の保存先も残っている: 削除できなかったもののまとまりは残り、次の操作は手で
	// 削除してよいことを伝えるテキストにする (設定のフォルダーは注意に移ったので、このまとまりのものはどれも手で
	// 削除してよい)。接続のための情報の保存先は、項目の下に残る。
	Observation mixed = obs;
	mixed.uninstall.leftovers = {{UninstallLeftoverKind::SettingsFiles, "C:/cfg"},
				     {UninstallLeftoverKind::LocalDataFolder, "C:/local"},
				     {UninstallLeftoverKind::Sources, "stream"},
				     {UninstallLeftoverKind::VencordDir, "C:/state.json"}};
	v = decide_dock(mixed);
	const dockview::LeftoverGroup *not_deleted = group_of(*v.uninstall_result, keys::kDialogNotDeletedText);
	REQUIRE(not_deleted != nullptr);
	REQUIRE(not_deleted->next.has_value());
	CHECK(not_deleted->next->id == keys::kDialogNotDeletedNextText);
	REQUIRE(not_deleted->item_locations.size() == 2);
	CHECK(paths(not_deleted->item_locations[0]) == std::vector<std::string>{"C:/local"});
	for (const std::vector<dockview::LocationLine> &lines : not_deleted->item_locations) {
		for (const dockview::LocationLine &line : lines)
			CHECK(line.path != "C:/cfg");
	}

	// 手順書と違う置き方 (プラグインのフォルダーが分からない): 注意は「cordmuxのファイルと次のフォルダー」の
	// テキスト。単数と複数は、続く場所の行の数で選ぶ。
	Observation loose = obs;
	loose.uninstall.folder_to_delete.clear();
	v = decide_dock(loose);
	not_restored = group_of(*v.uninstall_result, keys::kDialogNotRestoredText);
	REQUIRE(not_restored != nullptr);
	REQUIRE(not_restored->caution.has_value());
	CHECK(not_restored->caution->id == keys::kDialogDontDeleteFilesAndFoldersText.one);
	CHECK(paths(not_restored->caution_locations) == std::vector<std::string>{"C:/cfg"});
	Observation loose_two = loose;
	loose_two.uninstall.leftovers = {{UninstallLeftoverKind::SettingsFiles, "C:/cfg/settings.json"},
					    {UninstallLeftoverKind::SettingsFiles, "C:/cfg/sub"},
					    {UninstallLeftoverKind::VencordDir, "C:/state.json"}};
	v = decide_dock(loose_two);
	not_restored = group_of(*v.uninstall_result, keys::kDialogNotRestoredText);
	REQUIRE(not_restored != nullptr);
	REQUIRE(not_restored->caution.has_value());
	CHECK(not_restored->caution->id == keys::kDialogDontDeleteFilesAndFoldersText.other);
	CHECK(not_restored->caution_locations.size() == 2);

	// 設定を元に戻せなかったが、設定のフォルダーは示されない (Vesktop の設定を戻す段階で止まったとき):
	// 注意はプラグインのフォルダーだけを示す。場所が分からなければ「cordmuxのファイル」のテキストで、
	// 場所の行はない。
	Observation restore_only = obs;
	restore_only.uninstall.leftovers = {{UninstallLeftoverKind::VencordDir, "C:/state.json"}};
	v = decide_dock(restore_only);
	not_restored = group_of(*v.uninstall_result, keys::kDialogNotRestoredText);
	REQUIRE(not_restored != nullptr);
	REQUIRE(not_restored->caution.has_value());
	CHECK(not_restored->caution->id == keys::kDialogDontDeleteFolderText);
	CHECK(paths(not_restored->caution_locations) == std::vector<std::string>{"C:/plugins/cordmux"});
	restore_only.uninstall.folder_to_delete.clear();
	v = decide_dock(restore_only);
	not_restored = group_of(*v.uninstall_result, keys::kDialogNotRestoredText);
	REQUIRE(not_restored != nullptr);
	REQUIRE(not_restored->caution.has_value());
	CHECK(not_restored->caution->id == keys::kDialogDontDeleteFilesText);
	CHECK(not_restored->caution_locations.empty());

	// 設定を元に戻せず、設定のフォルダーの残りの場所が分からない (まれな場合): 注意で示せないので
	// 「削除できなかったもの:」に残し、次の操作は手で削除する方法を伝えない。
	Observation no_where = obs;
	no_where.uninstall.leftovers = {{UninstallLeftoverKind::SettingsFiles, ""},
					{UninstallLeftoverKind::VencordDir, "C:/state.json"}};
	v = decide_dock(no_where);
	not_deleted = group_of(*v.uninstall_result, keys::kDialogNotDeletedText);
	REQUIRE(not_deleted != nullptr);
	REQUIRE(not_deleted->next.has_value());
	CHECK(not_deleted->next->id == keys::kDialogNotDeletedUninstallAgainNextText);
	not_restored = group_of(*v.uninstall_result, keys::kDialogNotRestoredText);
	REQUIRE(not_restored != nullptr);
	REQUIRE(not_restored->caution.has_value());
	CHECK(not_restored->caution->id == keys::kDialogDontDeleteFolderText);

	// (2) 設定を消す段階で止まったとき (製品で起きる状態: 設定のフォルダーと接続のための情報の保存先の残りだけ。
	// Vesktop の設定を戻す段階はまだ実行しておらず、バックアップが残る): 次の操作は
	// 「もう一度アンインストールしてください。」だけ。
	Observation step1 = obs;
	step1.uninstall.leftovers = {{UninstallLeftoverKind::SettingsFiles, "C:/cfg/settings.json"},
				     {UninstallLeftoverKind::LocalDataFolder, "C:/local"}};
	v = decide_dock(step1);
	REQUIRE(v.uninstall_result->groups.size() == 1);
	REQUIRE(v.uninstall_result->groups[0].next.has_value());
	CHECK(v.uninstall_result->groups[0].next->id == keys::kDialogNotDeletedUninstallAgainNextText);
	CHECK_FALSE(v.uninstall_result->groups[0].caution.has_value()); // 注意は元に戻せなかった設定のまとまりだけ
	card = audit_body_ids(notices_of(v, "Uninstalled")[0]);
	CHECK(card.back() == keys::kDialogNotDeletedUninstallAgainNextText);
	CHECK(std::count(card.begin(), card.end(), std::string(keys::kDialogNotDeletedNextText)) == 0);

	// 接続のための情報の保存先だけが残った (バックアップを持たない): 手で削除してよい。条件は、項目のテキスト
	// ではなく、残りの種類 (UninstallLeftoverKind) で見る。
	Observation local_only = obs;
	local_only.uninstall.leftovers = {{UninstallLeftoverKind::LocalDataFolder, "C:/local"}};
	v = decide_dock(local_only);
	REQUIRE(v.uninstall_result->groups.size() == 1);
	CHECK(v.uninstall_result->groups[0].items[0].text->id == keys::kDialogLeftSettingsFilesItemText);
	CHECK(v.uninstall_result->groups[0].next->id == keys::kDialogNotDeletedNextText);

	// 設定を元に戻せず、残ったのがシーンだけ: シーンは手で削除してよい。注意はプラグインのフォルダーだけを示す。
	Observation scenes = obs;
	scenes.uninstall.leftovers = {{UninstallLeftoverKind::Sources, "stream"},
				      {UninstallLeftoverKind::VencordDir, "C:/state.json"}};
	v = decide_dock(scenes);
	n = notices_of(v, "Uninstalled");
	REQUIRE(n.size() == 1);
	CHECK(audit_body_ids(n[0]).back() == keys::kDialogNotDeletedNextText);
	not_restored = group_of(*v.uninstall_result, keys::kDialogNotRestoredText);
	REQUIRE(not_restored != nullptr);
	REQUIRE(not_restored->caution.has_value());
	CHECK(not_restored->caution->id == keys::kDialogDontDeleteFolderText);

	// 完了したときのまとまり (確認できなかったもの) は、次の操作も注意も持たない (ダイアログが数で選ぶ)。
	Observation done = healthy();
	done.uninstall.phase = UninstallPhase::Done;
	done.uninstall.folder_to_delete = "C:/plugins/cordmux";
	done.uninstall.leftovers = {{UninstallLeftoverKind::CollectionNotOpened, "stream"}};
	v = decide_dock(done);
	REQUIRE(v.uninstall_result->groups.size() == 1);
	CHECK_FALSE(v.uninstall_result->groups[0].next.has_value());
	CHECK_FALSE(v.uninstall_result->groups[0].caution.has_value());
	CHECK(v.uninstall_result->groups[0].caution_locations.empty());
}

TEST_CASE("待ち受けを始められないメッセージは理由・帰結・OBS の再起動の順で、accept_failed のときはポートの文を持たない")
{
	Observation obs = not_connected();
	obs.listener.state = ListenerState::PortsExhausted;
	obs.listener.port_first = 4470;
	obs.listener.port_last = 4485;
	std::vector<dockview::Notice> n = notices_of(decide_dock(obs), "ListenPortsExhausted");
	REQUIRE(n.size() == 1);
	CHECK(n[0].tier == dockview::Tier::L2);
	CHECK(audit_body_ids(n[0]) == std::vector<std::string>{keys::kDockPortsInUseText, keys::kDockCannotReceiveText,
								keys::kDockRestartObsText});
	CHECK(n[0].body[0].args == std::vector<std::string>{"4470", "4485"});

	obs.listener.accept_failed = true;
	n = notices_of(decide_dock(obs), "ListenPortsExhausted");
	REQUIRE(n.size() == 1);
	CHECK(audit_body_ids(n[0]) == std::vector<std::string>{keys::kDockCannotReceiveText, keys::kDockRestartObsText});

	// ポート番号が分からない: %N に「-」を入れず、帰結と次の操作の文だけにする。
	obs.listener.accept_failed = false;
	obs.listener.port_first.reset();
	n = notices_of(decide_dock(obs), "ListenPortsExhausted");
	REQUIRE(n.size() == 1);
	CHECK(audit_body_ids(n[0]) == std::vector<std::string>{keys::kDockCannotReceiveText, keys::kDockRestartObsText});
}

TEST_CASE("アンインストールのボタンはリプレイバッファと仮想カメラの動作中も押せず、[バージョン情報] に理由を表示する")
{
	Observation obs = healthy();
	obs.install.process = VesktopProcess::NotRunning;
	CHECK_FALSE(decide_dock(obs).about.uninstall_button.disabled_reason.has_value());
	obs.uninstall.obs_virtual_camera = true;
	dockview::DockViewV2 v = decide_dock(obs);
	REQUIRE(v.about.uninstall_button.disabled_reason.has_value());
	CHECK(v.about.uninstall_button.disabled_reason->id == keys::kSettingsUninstallVirtualCameraText);
	obs.uninstall.obs_replay_buffer = true;
	v = decide_dock(obs);
	REQUIRE(v.about.uninstall_button.disabled_reason.has_value());
	CHECK(v.about.uninstall_button.disabled_reason->id == keys::kSettingsUninstallReplayBufferText);
}

TEST_CASE("vencordDir を元に戻す段階で止まり、バックアップを残したアンインストールでは、"
	  "設定のフォルダーをプラグインのフォルダーと同じ注意で示す")
{
	const auto group_of = [](const dockview::UninstallResultView &r, const char *heading) {
		const dockview::LeftoverGroup *found = nullptr;
		for (const dockview::LeftoverGroup &g : r.groups) {
			if (g.heading.id == heading)
				found = &g;
		}
		return found;
	};
	const auto paths = [](const std::vector<dockview::LocationLine> &lines) {
		std::vector<std::string> out;
		for (const dockview::LocationLine &line : lines)
			out.push_back(line.path);
		return out;
	};

	// Vesktop の設定を戻す段階で止まったとき: 残りは Vesktop の設定だけで、バックアップを残した設定の
	// フォルダーは別のフィールド (settings_dir) にある。
	Observation obs = healthy();
	obs.uninstall.phase = UninstallPhase::Stopped;
	obs.uninstall.folder_to_delete = "C:/plugins/cordmux";
	obs.uninstall.settings_dir = "C:/obs/plugin_config/cordmux";
	obs.uninstall.leftovers = {{UninstallLeftoverKind::VencordDir, "C:/state.json", true}};
	dockview::DockViewV2 v = decide_dock(obs);
	REQUIRE(v.uninstall_result.has_value());
	REQUIRE(v.uninstall_result->groups.size() == 1);
	CHECK(group_of(*v.uninstall_result, keys::kDialogNotDeletedText) == nullptr);
	const dockview::LeftoverGroup *not_restored = group_of(*v.uninstall_result, keys::kDialogNotRestoredText);
	REQUIRE(not_restored != nullptr);
	REQUIRE(not_restored->caution.has_value());
	CHECK(not_restored->caution->id == keys::kDialogDontDeleteFoldersText);
	// 並びはプラグインのフォルダー -> 設定のフォルダー。どちらもフォルダーの行で [フォルダーを開く]。
	CHECK(paths(not_restored->caution_locations) ==
	      std::vector<std::string>{"C:/plugins/cordmux", "C:/obs/plugin_config/cordmux"});
	for (const dockview::LocationLine &line : not_restored->caution_locations) {
		CHECK(line.kind == dockview::LocationKind::Folder);
		CHECK(line.open_folder);
	}
	// カードも同じテキストと、同じ並びの場所の行 (ダイアログと同じ uninstall_result を読む)。
	std::vector<dockview::Notice> n = notices_of(v, "Uninstalled");
	REQUIRE(n.size() == 1);
	CHECK(audit_body_ids(n[0]) == std::vector<std::string>{keys::kDialogNotRestoredText,
							       keys::kDialogLeftVesktopSettingsItemText,
							       keys::kDialogNotRestoredNextText,
							       keys::kDialogDontDeleteFoldersText});
	std::vector<std::string> card_places;
	for (const dockview::BodyLocation &line : n[0].body_locations) {
		if (line.after < n[0].body.size() && n[0].body[line.after].id == keys::kDialogDontDeleteFoldersText)
			card_places.push_back(line.line.path);
	}
	CHECK(card_places == std::vector<std::string>{"C:/plugins/cordmux", "C:/obs/plugin_config/cordmux"});

	// 手順書と違う置き方 (プラグインのフォルダーが分からない): 「cordmuxのファイルと次のフォルダー」を含む文で、
	// 続く場所の行は 1 つ。
	Observation loose = obs;
	loose.uninstall.folder_to_delete.clear();
	v = decide_dock(loose);
	not_restored = group_of(*v.uninstall_result, keys::kDialogNotRestoredText);
	REQUIRE(not_restored != nullptr);
	REQUIRE(not_restored->caution.has_value());
	CHECK(not_restored->caution->id == keys::kDialogDontDeleteFilesAndFoldersText.one);
	CHECK(paths(not_restored->caution_locations) == std::vector<std::string>{"C:/obs/plugin_config/cordmux"});

	// バックアップがないとき (settings_dir が空): プラグインのフォルダーだけを示すテキスト (2 文目の
	// 「Vesktopがその場所に公式のVencordを入れます」が当てはまる)。
	Observation no_backup = obs;
	no_backup.uninstall.settings_dir.clear();
	v = decide_dock(no_backup);
	not_restored = group_of(*v.uninstall_result, keys::kDialogNotRestoredText);
	REQUIRE(not_restored != nullptr);
	CHECK(not_restored->caution->id == keys::kDialogDontDeleteFolderText);
	CHECK(paths(not_restored->caution_locations) == std::vector<std::string>{"C:/plugins/cordmux"});

	// テストのために組み合わせた状態 (製品では同時に起きない): settings_dir と同じ文字列の設定のフォルダーの
	// 残りは 1 行にまとめ、違う文字列の残りは後に続く。
	Observation mixed = obs;
	mixed.uninstall.leftovers = {{UninstallLeftoverKind::SettingsFiles, "C:/obs/plugin_config/cordmux"},
				     {UninstallLeftoverKind::SettingsFiles, "C:/obs/plugin_config/cordmux/settings.json", true},
				     {UninstallLeftoverKind::VencordDir, "C:/state.json", true}};
	v = decide_dock(mixed);
	not_restored = group_of(*v.uninstall_result, keys::kDialogNotRestoredText);
	REQUIRE(not_restored != nullptr);
	CHECK(paths(not_restored->caution_locations) ==
	      std::vector<std::string>{"C:/plugins/cordmux", "C:/obs/plugin_config/cordmux",
				       "C:/obs/plugin_config/cordmux/settings.json"});
	// ファイルの残りは、注意の場所の行でもファイルの行 (種類は行ごとに決める)。
	CHECK(not_restored->caution_locations[2].kind == dockview::LocationKind::File);
	CHECK(group_of(*v.uninstall_result, keys::kDialogNotDeletedText) == nullptr);

	// 設定を消す段階で止まったときに settings_dir があっても (アンインストールの処理は入れないが、あった場合)、
	// 「元に戻せなかった設定:」のまとまりがないので、注意も場所の行も追加しない。
	Observation step1 = obs;
	step1.uninstall.leftovers = {{UninstallLeftoverKind::SettingsFiles, "C:/obs/plugin_config/cordmux/settings.json", true}};
	v = decide_dock(step1);
	REQUIRE(v.uninstall_result->groups.size() == 1);
	CHECK_FALSE(v.uninstall_result->groups[0].caution.has_value());
	CHECK(v.uninstall_result->groups[0].caution_locations.empty());
}

TEST_CASE("途中で止まったアンインストールで、アンインストールの処理がファイルとして返した残りの場所の行は、"
	  "[ファイルの場所を開く] の行にする")
{
	// 設定を消す段階で止まったとき: 削除できなかった設定のファイル 1 つ (ファイルの行) と、接続のための情報の
	// 保存先 (トークンとポート番号を保存するフォルダー。フォルダーの行)。
	Observation obs = healthy();
	obs.uninstall.phase = UninstallPhase::Stopped;
	obs.uninstall.folder_to_delete = "C:/plugins/cordmux";
	obs.uninstall.leftovers = {{UninstallLeftoverKind::SettingsFiles, "C:/obs/plugin_config/cordmux/settings.json", true},
				   {UninstallLeftoverKind::LocalDataFolder, "C:/local/cordmux", false}};
	dockview::DockViewV2 v = decide_dock(obs);
	REQUIRE(v.uninstall_result.has_value());
	REQUIRE(v.uninstall_result->groups.size() == 1);
	const dockview::LeftoverGroup &not_deleted = v.uninstall_result->groups[0];
	REQUIRE(not_deleted.item_locations.size() == 1);
	REQUIRE(not_deleted.item_locations[0].size() == 2);
	CHECK(not_deleted.item_locations[0][0].path == "C:/obs/plugin_config/cordmux/settings.json");
	CHECK(not_deleted.item_locations[0][0].kind == dockview::LocationKind::File);
	CHECK_FALSE(not_deleted.item_locations[0][0].open_folder);
	CHECK(not_deleted.item_locations[0][1].kind == dockview::LocationKind::Folder);
	CHECK(not_deleted.item_locations[0][1].open_folder);

	// カードの場所の行も同じ種類 (表示する側の notice_list が [ファイルの場所を開く] を置く)。
	std::vector<dockview::Notice> n = notices_of(v, "Uninstalled");
	REQUIRE(n.size() == 1);
	std::vector<std::pair<std::string, dockview::LocationKind>> card;
	for (const dockview::BodyLocation &line : n[0].body_locations)
		card.emplace_back(line.line.path, line.line.kind);
	CHECK(card == std::vector<std::pair<std::string, dockview::LocationKind>>{
			      {"C:/obs/plugin_config/cordmux/settings.json", dockview::LocationKind::File},
			      {"C:/local/cordmux", dockview::LocationKind::Folder}});

	// 名前に点を含むフォルダーも、アンインストールの処理がフォルダーとして返したならフォルダーの行にする (名前から
	// 推測しない)。
	Observation dotted = obs;
	dotted.uninstall.leftovers = {{UninstallLeftoverKind::SettingsFiles, "C:/obs/plugin_config/cordmux/old.json", false}};
	v = decide_dock(dotted);
	CHECK(v.uninstall_result->groups[0].item_locations[0][0].kind == dockview::LocationKind::Folder);
}

// ---- 人の行の音量とミュートの値 ----

namespace {

const dockview::PersonRow *audio_row_of(const dockview::DockViewV2 &view, const std::string &user_id)
{
	for (const dockview::PersonRow &row : view.people) {
		if (row.user_id == user_id)
			return &row;
	}
	return nullptr;
}

} // namespace

TEST_CASE("人の行の音量とミュートは、値を持たない人は既定 (ミュートなし、100%) で、合成のソースがなければ動作しない")
{
	Observation obs = healthy();
	obs.choices.composite_present = false;
	const dockview::DockViewV2 v = decide_dock(obs);
	const dockview::PersonRow *row = audio_row_of(v, "100");
	REQUIRE(row != nullptr);
	CHECK_FALSE(row->audio.applies);
	CHECK_FALSE(row->audio.is_main_account);
	CHECK_FALSE(row->audio.voice_muted);
	CHECK(row->audio.voice_volume_pct == 100);
	CHECK_FALSE(row->audio.has_screen_audio);
	CHECK_FALSE(row->audio.screen_muted);
	CHECK(row->audio.screen_volume_pct == 100);
}

TEST_CASE("人の行の音量とミュートは、人の事実の値をそのまま持ち、合成のソースがあれば動作し、メインアカウントの行に印を付ける")
{
	Observation obs = healthy();
	obs.choices.composite_present = true;
	obs.choices.broadcaster_user_id = "100";
	obs.people[0].audio.voice_muted = true;
	obs.people[0].audio.voice_volume_pct = 40;
	obs.people[1].audio.has_screen_audio = true;
	obs.people[1].audio.screen_muted = true;
	obs.people[1].audio.screen_volume_pct = 180;

	const dockview::DockViewV2 v = decide_dock(obs);
	const dockview::PersonRow *main = audio_row_of(v, "100");
	const dockview::PersonRow *other = audio_row_of(v, "200");
	REQUIRE(main != nullptr);
	REQUIRE(other != nullptr);

	CHECK(main->audio.applies);
	CHECK(main->audio.is_main_account);
	CHECK(main->audio.voice_muted);
	CHECK(main->audio.voice_volume_pct == 40);
	CHECK_FALSE(main->audio.has_screen_audio);

	CHECK(other->audio.applies);
	CHECK_FALSE(other->audio.is_main_account);
	CHECK_FALSE(other->audio.voice_muted);
	CHECK(other->audio.voice_volume_pct == 100);
	CHECK(other->audio.has_screen_audio);
	CHECK(other->audio.screen_muted);
	CHECK(other->audio.screen_volume_pct == 180);
}

TEST_CASE("Vesktop のアカウントの行は、音量とミュートの値を持たない")
{
	Observation obs = healthy();
	obs.choices.composite_present = true;
	obs.people[1].is_receiver = true;
	obs.people[1].audio.voice_volume_pct = 20;
	const dockview::DockViewV2 v = decide_dock(obs);
	const dockview::PersonRow *row = audio_row_of(v, "200");
	REQUIRE(row != nullptr);
	CHECK_FALSE(row->audio.applies);
	CHECK(row->audio.voice_volume_pct == 100);
}

TEST_CASE("状態タブの受信の表は、合成のソースがあるときだけ、直近 1 分にフレームが届いた人を名前の自然順に並べる")
{
	Observation obs = healthy();
	obs.choices.composite_present = true;
	cordmux::pipeline::ArrivalWindow w;
	w.delays_ms = {10, 20};
	w.mean_ms = 15.0;
	w.max_ms = 20.0;
	obs.receive.arrival = {ArrivalFact{"200", {w}}, ArrivalFact{"100", {w}}, ArrivalFact{"300", {}},
			       ArrivalFact{"ghost", {w}}, ArrivalFact{"", {w}}};
	obs.people[0].display_name = "たろう10";
	obs.people[1].display_name = "たろう2";
	const dockview::DockViewV2 v = decide_dock(obs);
	CHECK(v.status_tab.composite_present);
	REQUIRE(v.status_tab.rows.size() == 3); // フレームのない人と userId のない行は入れない
	CHECK(v.status_tab.rows[0].name.literal == "ghost");
	CHECK(v.status_tab.rows[1].name.literal == "たろう2");
	CHECK(v.status_tab.rows[2].name.literal == "たろう10");
	CHECK(v.status_tab.rows[2].streams.size() == 1);

	// 合成のソースがなければ行を作らず、フラグを偽にする。
	obs.choices.composite_present = false;
	const dockview::DockViewV2 off = decide_dock(obs);
	CHECK_FALSE(off.status_tab.composite_present);
	CHECK(off.status_tab.rows.empty());

	// 状態タブが見えていない間 (事実が空) は表も空。
	obs.choices.composite_present = true;
	obs.receive.arrival.clear();
	CHECK(decide_dock(obs).status_tab.rows.empty());
}

// ---- 映像を受け取ると決めていない間 ----

namespace {

// 枠に 2 人載せ、どちらにも映像がまだ届いていない状態。
Observation two_slotted_without_video()
{
	Observation obs = healthy();
	obs.people[0].last_video_frame_at_us.reset();
	obs.people[1] = showing("200", "hanako", 2);
	obs.people[1].last_video_frame_at_us.reset();
	obs.call.roster_count = 2;
	return obs;
}

bool any_row_has_state(const dockview::DockViewV2 &v, dockview::RowState state)
{
	for (const dockview::PersonRow &row : v.people) {
		if (row.status.has_value() && row.status->state == state)
			return true;
	}
	return false;
}

} // namespace

TEST_CASE("機能を無効にしている間は、映像が届いていないメッセージも、行の待ちの状態も出さない")
{
	// 受け取ると決めている間は、今までどおりメッセージと待ちの状態が出る。
	Observation obs = two_slotted_without_video();
	dockview::DockViewV2 v = decide_dock(obs);
	REQUIRE(notices_of(v, "AllVideoStoppedHint").size() == 1);
	CHECK(any_row_has_state(v, dockview::RowState::Waiting));

	obs.choices.feature = FeatureSwitch::Off;
	v = decide_dock(obs);
	CHECK(notices_of(v, "AllVideoStoppedHint").empty());
	CHECK_FALSE(any_row_has_state(v, dockview::RowState::Waiting));
	CHECK(v.first_line.kind == dockview::FirstLineKind::UserChoice);
	CHECK(form_ids(v.first_line) == std::vector<std::string>{keys::kDockSummaryDisabledStatus});
}

TEST_CASE("現在のシーンコレクションに cordmux のソースがない間は、映像が届いていないメッセージも待ちの状態も出さず、ソースのメッセージを出す")
{
	Observation obs = two_slotted_without_video();
	obs.choices.composite_present = false;
	dockview::DockViewV2 v = decide_dock(obs);
	CHECK(notices_of(v, "AllVideoStoppedHint").empty());
	CHECK_FALSE(any_row_has_state(v, dockview::RowState::Waiting));
	CHECK(notices_of(v, "CompositeAbsent").size() == 1);
}

TEST_CASE("ソースがなくても録画が ON なら、録画の対象の人の映像が届かないメッセージを今までどおり出す")
{
	Observation obs = two_slotted_without_video();
	obs.choices.composite_present = false;
	obs.choices.recording = FeatureSwitch::On;
	obs.recording.phase = RecordPhase::Recording;
	dockview::DockViewV2 v = decide_dock(obs);
	CHECK(notices_of(v, "AllVideoStoppedHint").size() == 1);
	CHECK(any_row_has_state(v, dockview::RowState::Waiting));

	// 機能を無効にすれば、録画が ON でも受け取らない。
	obs.choices.feature = FeatureSwitch::Off;
	CHECK(notices_of(decide_dock(obs), "AllVideoStoppedHint").empty());
}

TEST_CASE("ソースの有無を調べ終えていない間は、ソースがないと断定せず、映像が届かないメッセージを今までどおり出す")
{
	Observation obs = two_slotted_without_video();
	obs.choices.composite_checked = false;
	obs.choices.composite_present = false;
	dockview::DockViewV2 v = decide_dock(obs);
	CHECK(notices_of(v, "AllVideoStoppedHint").size() == 1);
	CHECK(any_row_has_state(v, dockview::RowState::Waiting));
	CHECK(notices_of(v, "CompositeAbsent").empty());
}

TEST_CASE("映像が届いていた人も、無効にした後に時間が過ぎても、届かなくなった状態もメッセージも出さない")
{
	Observation obs = healthy();
	obs.choices.feature = FeatureSwitch::Off;
	obs.people[0].last_video_frame_at_us = kNow - 600 * kSec;
	dockview::DockViewV2 v = decide_dock(obs);
	CHECK(notices_of(v, "AllVideoStoppedHint").empty());
	CHECK_FALSE(any_row_has_state(v, dockview::RowState::NoVideo));

	// ソースがない場合も同じ。
	Observation no_source = healthy();
	no_source.choices.composite_present = false;
	no_source.people[0].last_video_frame_at_us = kNow - 600 * kSec;
	v = decide_dock(no_source);
	CHECK(notices_of(v, "AllVideoStoppedHint").empty());
	CHECK_FALSE(any_row_has_state(v, dockview::RowState::NoVideo));
}

TEST_CASE("映像を受け取ると決めていない間は、前の段階の画質の文を行に出さない")
{
	Observation obs = healthy();
	obs.people[0].stage = std::string(kStageUnavailable);
	REQUIRE(row_of(decide_dock(obs), "100")->quality.has_value());
	obs.choices.feature = FeatureSwitch::Off;
	CHECK_FALSE(row_of(decide_dock(obs), "100")->quality.has_value());
}

TEST_CASE("受け取ると決めている状態に戻った直後は、待ち時間を戻った時刻から数え、古い時刻ですぐに赤を出さない")
{
	// 届いていた映像: 最後のフレームから 10 分たっているが、戻ったのは 1 秒前。
	Observation obs = healthy();
	obs.people[0].last_video_frame_at_us = kNow - 600 * kSec;
	obs.video_expected_since_us = kNow - kSec;
	dockview::DockViewV2 v = decide_dock(obs);
	CHECK(notices_of(v, "AllVideoStoppedHint").empty());
	CHECK_FALSE(any_row_has_state(v, dockview::RowState::NoVideo));

	// しきい値を超えて届かなければ、戻った時刻から数えても出る。
	obs.video_expected_since_us = kNow - 5 * kSec;
	PersonFact stopped = showing("300", "kenji", 2);
	stopped.last_video_frame_at_us = kNow - 600 * kSec;
	obs.people.push_back(stopped);
	v = decide_dock(obs);
	CHECK(notices_of(v, "AllVideoStoppedHint").size() == 1);
	CHECK(any_row_has_state(v, dockview::RowState::NoVideo));

	// 最初のフレームを待つ人: 画面共有は 60 秒前からだが、戻ったのは 1 秒前。
	Observation first = two_slotted_without_video();
	first.video_expected_since_us = kNow - kSec;
	v = decide_dock(first);
	CHECK(notices_of(v, "AllVideoStoppedHint").empty());
	first.video_expected_since_us = kNow - 30 * kSec;
	CHECK(notices_of(decide_dock(first), "AllVideoStoppedHint").size() == 1);
}


namespace {

// メインアカウント (500) が参加者の一覧にいる状態。
Observation with_main_in_roster_for_call_action()
{
	Observation obs = healthy();
	obs.choices.broadcaster_user_id = "500";
	obs.people.push_back(present("500", "main"));
	return obs;
}

} // namespace

TEST_CASE("[移動] と [退出] を押して時間がたっても通話が変わらないときだけ、レベル 2 のメッセージにする")
{
	Observation obs = healthy();
	obs.ops.call_action_timeout_us = 5 * kSec;

	// 押していない。
	CHECK(notices_of(decide_dock(obs), "CallActionFailed").empty());

	// 押して 3 秒。まだ待つ。
	obs.ops.call_action = CallActionPress{CallActionKind::Move, kNow - 3 * kSec};
	CHECK_FALSE(call_action_failed(obs));
	CHECK(notices_of(decide_dock(obs), "CallActionFailed").empty());

	// 押して 6 秒たっても、メインアカウントが参加者の一覧にいない。
	obs.ops.call_action = CallActionPress{CallActionKind::Move, kNow - 6 * kSec};
	std::vector<dockview::Notice> n = notices_of(decide_dock(obs), "CallActionFailed");
	REQUIRE(n.size() == 1);
	CHECK(n[0].tier == dockview::Tier::L2);
	CHECK(n[0].headline.id == keys::kDockCallMoveFailedHeadline);
	REQUIRE(n[0].body.size() == 1);
	CHECK(n[0].body[0].id == keys::kDockCallActionFailedText);

	obs.ops.call_action = CallActionPress{CallActionKind::Leave, kNow - 6 * kSec};
	n = notices_of(decide_dock(obs), "CallActionFailed");
	REQUIRE(n.size() == 1);
	CHECK(n[0].headline.id == keys::kDockCallLeaveFailedHeadline);

	// もう一度押すと、押し直した時刻から数え直す。
	obs.ops.call_action = CallActionPress{CallActionKind::Leave, kNow};
	CHECK(notices_of(decide_dock(obs), "CallActionFailed").empty());
}

TEST_CASE("[移動] は参加者の一覧にメインアカウントが入ったとき、[退出] は通話からいなくなったとき、結果が出たとみなして消す")
{
	Observation move = with_main_in_roster_for_call_action();
	move.ops.call_action_timeout_us = 5 * kSec;
	move.ops.call_action = CallActionPress{CallActionKind::Move, kNow - 10 * kSec};
	CHECK(call_action_done(move));
	CHECK(call_action_finished(move));
	CHECK_FALSE(call_action_failed(move));
	CHECK(notices_of(decide_dock(move), "CallActionFailed").empty());

	Observation leave = healthy();
	leave.ops.call_action_timeout_us = 5 * kSec;
	leave.ops.call_action = CallActionPress{CallActionKind::Leave, kNow - 10 * kSec};
	leave.call.presence = Presence::NotInCall;
	CHECK(call_action_done(leave));
	CHECK_FALSE(call_action_failed(leave));
	CHECK(notices_of(decide_dock(leave), "CallActionFailed").empty());
}

TEST_CASE("接続が切れたら、押した [移動] と [退出] の結果は見られないので、メッセージにせず記録を捨てる対象にする")
{
	Observation obs = healthy();
	obs.ops.call_action_timeout_us = 5 * kSec;
	obs.ops.call_action = CallActionPress{CallActionKind::Leave, kNow - 10 * kSec};
	obs.link.state = LinkState::Disconnected;
	CHECK(call_action_finished(obs));
	CHECK_FALSE(call_action_failed(obs));
	CHECK(notices_of(decide_dock(obs), "CallActionFailed").empty());

	// 押していなければ、捨てる記録もない。
	obs.ops.call_action.reset();
	CHECK_FALSE(call_action_finished(obs));
}

TEST_CASE("Vesktop の起動中は、最初の設定のカードの [Vesktopに追加] が押せず、押せない理由がボタンに付く")
{
	// 参加者タブの一覧の場所 (setup_notices_of) と状態タブは、同じカードを表示する。どちらも押せない理由をボタンの下に
	// 文で出す (dock/notice_list.cpp の NoticeButtons)。
	for (const VencordDirState dir : {VencordDirState::NotConfigured, VencordDirState::PointsElsewhere}) {
		CAPTURE(static_cast<int>(dir));
		Observation running = first_day();
		running.people.clear(); // 一覧が空の間だけ、一覧の場所に最初の設定のカードを出す
		running.install.dir = dir;
		running.install.process = VesktopProcess::Running;
		const dockview::DockViewV2 v = decide_dock(running);
		const std::vector<dockview::Notice> setup = setup_notices_of(v);
		int add_buttons = 0;
		for (const dockview::Notice &n : setup) {
			for (const dockview::ViewButton &b : n.buttons) {
				if (b.label.id != keys::kDockAddToVesktopButton)
					continue;
				++add_buttons;
				REQUIRE(b.disabled_reason.has_value());
				CHECK(b.disabled_reason->id == keys::kDockAddVesktopRunningText);
			}
		}
		CHECK(add_buttons == 1);
		// Vesktop を終了すれば押せる (同じカードのまま)。
		Observation quit = running;
		quit.install.process = VesktopProcess::NotRunning;
		const std::vector<dockview::Notice> setup_quit = setup_notices_of(decide_dock(quit));
		bool enabled = false;
		for (const dockview::Notice &n : setup_quit)
			for (const dockview::ViewButton &b : n.buttons)
				enabled = enabled || (b.label.id == keys::kDockAddToVesktopButton && !b.disabled_reason.has_value());
		CHECK(enabled);
	}
}

TEST_CASE("状態タブの「Vesktop」の組は、同梱の Vencord を指していて、Vesktop が起動していない間だけあり、[Vesktopから削除] を押せる")
{
	Observation obs = not_connected();
	obs.install.process = VesktopProcess::NotRunning;
	for (const VencordDirState dir : {VencordDirState::Matches, VencordDirState::HashMismatch}) {
		obs.install.dir = dir;
		const dockview::DockViewV2 v = decide_dock(obs);
		REQUIRE(v.status_tab.vesktop.has_value());
		CHECK(v.status_tab.vesktop->text.id == keys::kStatusVesktopAddedText);
		CHECK(v.status_tab.vesktop->remove.id == ActionId::RemoveFromVesktop);
		CHECK(v.status_tab.vesktop->remove.label.id == keys::kStatusRemoveFromVesktopButton);
		CHECK_FALSE(v.status_tab.vesktop->remove.disabled_reason.has_value());
	}
	// 起動中と、起動しているかを読めていない間は、組ごとない (配信中に、押せないボタンと終了の指示を並べない)。
	obs.install.dir = VencordDirState::Matches;
	for (const VesktopProcess process : {VesktopProcess::Running, VesktopProcess::Unknown}) {
		obs.install.process = process;
		CHECK_FALSE(decide_dock(obs).status_tab.vesktop.has_value());
	}
	// 接続している間も同じ (Vesktop の画面が表示されないときは、接続していることがある。その道筋は、接続できない
	// ときのカードの文が「Vesktopを終了して、[Vesktopから削除]」を伝え、終了すると組が現れる)。
	Observation connected = healthy();
	connected.install.process = VesktopProcess::Running;
	connected.contacts = {{"pc-hook", ContactState::Missing}};
	CHECK_FALSE(decide_dock(connected).status_tab.vesktop.has_value());
	connected.install.process = VesktopProcess::NotRunning;
	CHECK(decide_dock(connected).status_tab.vesktop.has_value());

	// この起動で Vesktop の設定を書かないときは、組に押せない理由を表示する (Vesktop を終了しても押せない)。
	obs.install.process = VesktopProcess::NotRunning;
	obs.install.vesktop_writes_off = true;
	dockview::DockViewV2 v = decide_dock(obs);
	REQUIRE(v.status_tab.vesktop.has_value());
	REQUIRE(v.status_tab.vesktop->remove.disabled_reason.has_value());
	CHECK(v.status_tab.vesktop->remove.disabled_reason->id == keys::kDockVesktopAddFailedWritesOffText);
	obs.install.vesktop_writes_off = false;

	// 指していない間と、アンインストールを始めた後は、組ごとない。
	for (const VencordDirState dir :
	     {VencordDirState::Unknown, VencordDirState::NotConfigured, VencordDirState::PointsElsewhere}) {
		obs.install.dir = dir;
		CHECK_FALSE(decide_dock(obs).status_tab.vesktop.has_value());
	}
	obs.install.dir = VencordDirState::Matches;
	obs.uninstall.phase = UninstallPhase::Stopped;
	CHECK_FALSE(decide_dock(obs).status_tab.vesktop.has_value());
}

TEST_CASE("[Vesktopから削除] の失敗は、理由の 1 文だけのメッセージのレベル 2 のカードにし、外せた後は出さない")
{
	struct Case {
		VesktopRemoveFailure failure;
		bool settings_unread;
		const char *text;
	};
	const Case cases[] = {
		{VesktopRemoveFailure::StateUnreadable, false, keys::kDockVesktopAddFailedStateUnreadableText},
		{VesktopRemoveFailure::NotRestorable, false, keys::kDockVesktopRemoveFailedNotRestorableText},
		{VesktopRemoveFailure::WritesDisabled, false, keys::kDockVesktopAddFailedWritesOffText},
		{VesktopRemoveFailure::WritesDisabled, true, keys::kDockVesktopAddFailedSettingsUnreadText},
		{VesktopRemoveFailure::StateWriteFailed, false, keys::kDockVesktopAddFailedStateWriteText},
	};
	for (const Case &c : cases) {
		CAPTURE(c.text);
		Observation obs = not_connected();
		obs.install.process = VesktopProcess::NotRunning;
		obs.ops.vesktop_remove_failure = c.failure;
		obs.ops.settings_unreadable_at_startup = c.settings_unread;
		const std::vector<dockview::Notice> n = notices_of(decide_dock(obs), "VesktopRemoveFailed");
		REQUIRE(n.size() == 1);
		CHECK(n[0].tier == dockview::Tier::L2);
		CHECK(n[0].headline.id == keys::kDockVesktopRemoveFailedHeadline);
		REQUIRE(n[0].headline_short.has_value());
		CHECK(n[0].headline_short->id == keys::kDockVesktopRemoveFailedShortHeadline);
		REQUIRE(n[0].body.size() == 1);
		CHECK(n[0].body[0].id == c.text);
		CHECK(n[0].buttons.empty());
		// 最初の設定のカードには入れない (状態タブにだけ出す)。
		CHECK(setup_notices_of(decide_dock(obs)).empty());
	}
	Observation removed = not_connected();
	removed.install.dir = VencordDirState::NotConfigured;
	removed.ops.vesktop_remove_failure = VesktopRemoveFailure::NotRestorable;
	CHECK(notices_of(decide_dock(removed), "VesktopRemoveFailed").empty());
}
